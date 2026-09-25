// End-to-end GPU test: runs the real VulkanEngine + SPIR-V shaders on a host
// Vulkan driver (Mesa lavapipe works) over a synthetic RAW10 frame, and checks
// the P010 the encoder would receive. Build & run: test/native/run_tests.sh
#include "vulkan_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <vector>

// --- Host stand-ins for the Android-only symbols the engine links against ---
extern "C" {
int __android_log_print(int, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("[%s] ", tag);
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
    return 0;
}
void ANativeWindow_acquire(ANativeWindow*) {}
void ANativeWindow_release(ANativeWindow*) {}
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow*, int32_t, int32_t, int32_t) { return 0; }
int32_t ANativeWindow_lock(ANativeWindow*, ANativeWindow_Buffer*, ARect*) { return -1; }
int32_t ANativeWindow_unlockAndPost(ANativeWindow*) { return 0; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateAndroidSurfaceKHR(VkInstance, const VkAndroidSurfaceCreateInfoKHR*,
                                                         const VkAllocationCallbacks*, VkSurfaceKHR*) {
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}
}

using namespace vesper;
using Vec3 = std::array<float, 3>;

static int failures = 0;
static void check(bool ok, const char* what, double got, double want) {
    if (!ok) { std::printf("FAIL %s: got %.3f want %.3f\n", what, got, want); ++failures; }
}

// Apple Log reference, same constants as render.comp.
static double appleLog(double x) {
    if (x >= 0.01) return 0.08550479 * std::log2(x + 0.00964052) + 0.69336945;
    if (x >= -0.05641088) return 47.28711236 * (x + 0.05641088) * (x + 0.05641088);
    return 0;
}

constexpr int W = 1280, H = 960, STRIDE = W * 5 / 4 + 16, OUT_W = 640, OUT_H = 480;
constexpr float BLACK = 64, WHITE = 1023;

struct Scene {
    // Linear (0..1 of clip) per logical channel for raw pixel (x, y).
    virtual Vec3 at(int x, int y) const = 0;
    virtual ~Scene() = default;
};

// GRBG sensor: even rows G R G R, odd rows B G B G.
static std::vector<uint8_t> makeRaw10(const Scene& scene) {
    std::vector<uint8_t> raw(static_cast<size_t>(STRIDE) * H, 0);
    for (int y = 0; y < H; ++y) {
        for (int g = 0; g < W / 4; ++g) {
            uint8_t lsb = 0;
            for (int i = 0; i < 4; ++i) {
                int x = g * 4 + i;
                Vec3 c = scene.at(x, y);
                bool evenRow = (y & 1) == 0, evenCol = (x & 1) == 0;
                float v = evenRow ? (evenCol ? c[1] : c[0]) : (evenCol ? c[2] : c[1]);
                int dn = static_cast<int>(std::lround(std::clamp(BLACK + v * (WHITE - BLACK), 0.0f, WHITE)));
                raw[y * STRIDE + g * 5 + i] = static_cast<uint8_t>(dn >> 2);
                lsb |= static_cast<uint8_t>((dn & 3) << (2 * i));
            }
            raw[y * STRIDE + g * 5 + 4] = lsb;
        }
    }
    return raw;
}

static FrameParams baseParams(int rotation) {
    FrameParams p{};
    for (float& b : p.blackLevel) b = BLACK;
    p.wbGains[0] = 2.0f; p.wbGains[1] = 1.0f; p.wbGains[2] = 1.25f; p.wbGains[3] = WHITE;
    p.rawInfo[0] = W; p.rawInfo[1] = H; p.rawInfo[2] = STRIDE; p.rawInfo[3] = 1; // GRBG
    p.quadInfo[0] = W / 2; p.quadInfo[1] = H / 2;
    p.cropRect[0] = 0; p.cropRect[1] = 0; p.cropRect[2] = W; p.cropRect[3] = H;
    p.outInfo[0] = OUT_W; p.outInfo[1] = OUT_H; p.outInfo[2] = rotation; p.outInfo[3] = 1;
    p.cfaOffsets[0] = 1.5f; p.cfaOffsets[1] = 0.5f; // R at (1,0)
    p.cfaOffsets[2] = 0.5f; p.cfaOffsets[3] = 1.5f; // B at (0,1)
    const float k = 8.0f;
    p.exposure[0] = k; p.exposure[1] = 0.95f; p.exposure[2] = 0.06f; p.exposure[3] = std::log2(k / 0.18f);
    // Identity camera->Rec.2020 (x k) so expected values are easy to derive.
    p.camToRec2020[0] = k; p.camToRec2020[5] = k; p.camToRec2020[10] = k;
    return p;
}

struct P010 {
    const uint16_t* y;
    const uint16_t* uv;
    int luma(int x, int yy) const { return y[yy * OUT_W + x] >> 6; }
    int cb(int x, int yy) const { return uv[(yy / 2) * OUT_W + (x / 2) * 2] >> 6; }
    int cr(int x, int yy) const { return uv[(yy / 2) * OUT_W + (x / 2) * 2 + 1] >> 6; }
};

static int yCode(double logv) { return static_cast<int>(std::lround(64 + 876 * logv)); }

// Neutral grey at 18% (after WB and k): raw = 0.18/k / gain per channel.
// Top-left 1/4 block: a saturated highlight (all channels at raw clip).
// Top-right block: pure red subject.
struct TestScene : Scene {
    Vec3 at(int x, int y) const override {
        const float grey = 0.18f / 8.0f;
        if (x < W / 4 && y < H / 4) return {1.0f, 1.0f, 1.0f};
        if (x >= 3 * W / 4 && y < H / 4) return {grey * 4 / 2.0f, grey * 0.25f, grey * 0.25f / 1.25f};
        return {grey / 2.0f, grey, grey / 1.25f};
    }
};

static bool runFrame(VulkanEngine& gpu, const std::vector<uint8_t>& raw, const FrameParams& p,
                     const std::vector<float>* shading, P010& out) {
    FrameInput in;
    in.raw = raw.data();
    in.rawSize = raw.size();
    in.params = p;
    if (shading) { in.shading = shading->data(); in.shadingFloats = shading->size(); }
    int slot = -1;
    if (!gpu.processFrame(in, &slot) || slot < 0) return false;
    size_t size = 0;
    const uint8_t* data = gpu.waitEncoderFrame(slot, &size);
    if (!data) return false;
    static std::vector<uint8_t> copy;
    copy.assign(data, data + size);
    gpu.releaseEncoderFrame(slot);
    out.y = reinterpret_cast<const uint16_t*>(copy.data());
    out.uv = out.y + OUT_W * OUT_H;
    return true;
}

int main() {
    VulkanEngine gpu;
    if (!gpu.initialize()) { std::puts("SKIP: no Vulkan device"); return 0; }
    std::vector<uint8_t> raw = makeRaw10(TestScene());
    P010 f{};

    // 1. Neutral grey, highlights, colour, orientation (rotation 0).
    if (!runFrame(gpu, raw, baseParams(0), nullptr, f)) { std::puts("FAIL frame"); return 1; }
    int greyY = f.luma(OUT_W / 2, OUT_H * 3 / 4);
    check(std::abs(greyY - yCode(appleLog(0.18))) <= 2, "18% grey -> Apple Log 0.488 (Y code)", greyY, yCode(appleLog(0.18)));
    check(std::abs(f.cb(OUT_W / 2, OUT_H * 3 / 4) - 512) <= 2, "grey Cb neutral", f.cb(OUT_W / 2, OUT_H * 3 / 4), 512);
    check(std::abs(f.cr(OUT_W / 2, OUT_H * 3 / 4) - 512) <= 2, "grey Cr neutral", f.cr(OUT_W / 2, OUT_H * 3 / 4), 512);
    // Clipped highlight must be neutral (no magenta) and at clip = k -> Apple Log(8).
    int hiY = f.luma(OUT_W / 16, OUT_H / 16);
    check(std::abs(hiY - yCode(appleLog(8.0))) <= 3, "clipped highlight level (top-left)", hiY, yCode(appleLog(8.0)));
    check(std::abs(f.cb(OUT_W / 16, OUT_H / 16) - 512) <= 3, "clipped highlight Cb neutral", f.cb(OUT_W / 16, OUT_H / 16), 512);
    check(std::abs(f.cr(OUT_W / 16, OUT_H / 16) - 512) <= 3, "clipped highlight Cr neutral", f.cr(OUT_W / 16, OUT_H / 16), 512);
    // Red subject sits top-right: Cr well above neutral, Cb below.
    int redCr = f.cr(OUT_W - OUT_W / 16, OUT_H / 16), redCb = f.cb(OUT_W - OUT_W / 16, OUT_H / 16);
    check(redCr > 600, "red subject Cr (top-right)", redCr, 600);
    check(redCb < 512, "red subject Cb (top-right)", redCb, 512);

    // 2. Rotation 90: the raw top-left highlight must land at output top-right
    //    (output (x, y) samples raw (y, outW - 1 - x)).
    if (!runFrame(gpu, raw, baseParams(90), nullptr, f)) { std::puts("FAIL frame"); return 1; }
    int rotY = f.luma(OUT_W - OUT_W / 16, OUT_H / 16);
    check(std::abs(rotY - yCode(appleLog(8.0))) <= 3, "rotation 90: highlight at top-right", rotY, yCode(appleLog(8.0)));

    // 3. Lens shading: a uniform 1.5x gain map lifts grey by exactly 1.5x linear.
    std::vector<float> shading(3 * 3 * 4, 1.5f);
    FrameParams p = baseParams(0);
    p.quadInfo[2] = 3; p.quadInfo[3] = 3;
    if (!runFrame(gpu, raw, p, &shading, f)) { std::puts("FAIL frame"); return 1; }
    int shadedY = f.luma(OUT_W / 2, OUT_H * 3 / 4);
    check(std::abs(shadedY - yCode(appleLog(0.27))) <= 2, "lens shading gain applied", shadedY, yCode(appleLog(0.27)));

    // 4. Ring: many frames in a row must all complete (fence/slot reuse).
    for (int i = 0; i < 12; ++i) {
        if (!runFrame(gpu, raw, baseParams(0), nullptr, f)) { std::puts("FAIL ring reuse"); ++failures; break; }
    }

    gpu.release();
    std::printf(failures ? "%d GPU FAILURES\n" : "all GPU pipeline tests passed\n", failures);
    return failures ? 1 : 0;
}
