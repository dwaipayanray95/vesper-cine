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
    p.sensorMap[0] = 1; p.sensorMap[1] = 1;
    p.arrayInfo[0] = W; p.arrayInfo[1] = H;
    p.noise[2] = 0.0f; p.noise[3] = 3.3e-5f; // matches makeNoisyRaw10 (uniform +-1% of clip)
    return p;
}

struct P010 {
    const uint16_t* y;
    const uint16_t* uv;
    int luma(int x, int yy) const { return y[yy * OUT_W + x] >> 6; }
    int cb(int x, int yy) const { return uv[(yy / 2) * OUT_W + (x / 2) * 2] >> 6; }
    int cr(int x, int yy) const { return uv[(yy / 2) * OUT_W + (x / 2) * 2 + 1] >> 6; }
};

// Deterministic per-pixel noise so temporal NR has something to average.
static std::vector<uint8_t> makeNoisyRaw10(const Scene& scene, unsigned seed) {
    struct Noisy : Scene {
        const Scene& base; unsigned seed;
        Noisy(const Scene& b, unsigned s) : base(b), seed(s) {}
        Vec3 at(int x, int y) const override {
            Vec3 v = base.at(x, y);
            unsigned h = (x * 73856093u) ^ (y * 19349663u) ^ (seed * 83492791u);
            h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
            float n = ((h & 0xffff) / 65535.0f - 0.5f) * 0.02f; // +-1% of clip
            return {v[0] + n, v[1] + n, v[2] + n};
        }
    };
    return makeRaw10(Noisy(scene, seed));
}

static double regionStdDevY(const struct P010& f, int x0, int y0, int w, int h);

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

static double regionStdDevY(const P010& f, int x0, int y0, int w, int h) {
    double sum = 0, sq = 0;
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) { double v = f.luma(x, y); sum += v; sq += v * v; }
    double n = static_cast<double>(w) * h, mean = sum / n;
    return std::sqrt(std::max(0.0, sq / n - mean * mean));
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

    // 4. Hot pixel: one stuck green photosite in the grey area.
    std::vector<uint8_t> hot = raw;
    {
        const int hx = W / 2, hy = H * 3 / 4; // even row, even col = G on GRBG
        int g = hx / 4, i = hx % 4;
        hot[hy * STRIDE + g * 5 + i] = 0xFF;
        hot[hy * STRIDE + g * 5 + 4] |= static_cast<uint8_t>(3 << (2 * i));
        auto peak = [&](const P010& fr) {
            int m = 0;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) m = std::max(m, fr.luma(hx * OUT_W / W + dx, hy * OUT_H / H + dy));
            return m;
        };
        FrameParams hp = baseParams(0);
        runFrame(gpu, hot, hp, nullptr, f);
        int unfixed = peak(f);
        hp.cleanFlags[0] = 1;
        runFrame(gpu, hot, hp, nullptr, f);
        int fixedPeak = peak(f);
        check(unfixed > greyY + 40, "hot pixel visible without the fix", unfixed, greyY + 40);
        check(fixedPeak <= greyY + 4, "hot pixel removed by the fix", fixedPeak, greyY + 4);
    }

    // 4b. Fine detail must survive the hot-pixel fix. A small white glint on
    //     green foliage lifts R and B far above their neighbours but G only a
    //     little; a per-channel defect filter clamps R and B and leaves a green
    //     speck (seen on device). Invariant: fix on == fix off at the glint.
    {
        struct Glint : Scene {
            Vec3 at(int x, int y) const override {
                const int gx = W / 2 & ~1, gy = H / 2 & ~1; // one quad-aligned 2x2 raw block
                if (x >= gx && x < gx + 2 && y >= gy && y < gy + 2) return {0.35f / 2.0f, 0.35f, 0.35f / 1.25f};
                return {0.04f / 2.0f, 0.30f, 0.04f / 1.25f};
            }
        };
        std::vector<uint8_t> glintRaw = makeRaw10(Glint());
        const int ox = (W / 2 & ~1) * OUT_W / W, oy = (H / 2 & ~1) * OUT_H / H;
        FrameParams gp = baseParams(0);
        runFrame(gpu, glintRaw, gp, nullptr, f);
        int cbOff = f.cb(ox, oy), crOff = f.cr(ox, oy);
        gp.cleanFlags[0] = 1;
        runFrame(gpu, glintRaw, gp, nullptr, f);
        int delta = std::max(std::abs(f.cb(ox, oy) - cbOff), std::abs(f.cr(ox, oy) - crOff));
        check(delta <= 4, "hot-pixel fix keeps a white glint on foliage (no green speck)", delta, 4);
    }

    // 5. Temporal NR lowers noise on a static scene.
    {
        FrameParams tp = baseParams(0);
        runFrame(gpu, makeNoisyRaw10(TestScene(), 1), tp, nullptr, f);
        double noisy = regionStdDevY(f, OUT_W / 4, OUT_H / 2, 64, 64);
        tp.cleanFlags[1] = 1;
        tp.noise[0] = 0.8f;
        for (unsigned i = 2; i < 14; ++i) runFrame(gpu, makeNoisyRaw10(TestScene(), i), tp, nullptr, f);
        double denoised = regionStdDevY(f, OUT_W / 4, OUT_H / 2, 64, 64);
        check(denoised < noisy * 0.6, "temporal NR reduces noise (std dev)", denoised, noisy * 0.6);
        // Motion: a region that changes (grey -> blown highlight) must not ghost.
        struct Flash : Scene { Vec3 at(int, int) const override { return {1, 1, 1}; } };
        runFrame(gpu, makeRaw10(Flash()), tp, nullptr, f);
        int flashY = f.luma(OUT_W / 2, OUT_H * 3 / 4);
        check(std::abs(flashY - yCode(appleLog(8.0))) <= 4, "temporal NR does not ghost on change", flashY, yCode(appleLog(8.0)));
        // Chroma NR keeps luma level.
        FrameParams cp = baseParams(0);
        cp.noise[1] = 1.0f;
        runFrame(gpu, raw, cp, nullptr, f);
        int cY = f.luma(OUT_W / 2, OUT_H * 3 / 4);
        check(std::abs(cY - greyY) <= 2, "chroma NR leaves luma alone", cY, greyY);
    }

    // 5b. Tile alignment: a textured scene panning 12 raw px per frame. With
    //     alignment, temporal NR keeps denoising (history is warped onto the
    //     current frame); without it every tile is "motion" and NR switches off.
    {
        struct Pan : Scene {
            int shift;
            explicit Pan(int s) : shift(s) {}
            Vec3 at(int x, int y) const override {
                float t = 0.5f + 0.5f * std::sin((x + shift) * 0.09f) * std::cos(y * 0.07f);
                float v = 0.01f + 0.03f * t;
                return {v / 2.0f, v, v / 1.25f};
            }
        };
        auto noisyPan = [&](int frame) {
            struct Noisy : Scene {
                Pan pan; unsigned seed;
                Noisy(int sh, unsigned sd) : pan(sh), seed(sd) {}
                Vec3 at(int x, int y) const override {
                    Vec3 v = pan.at(x, y);
                    unsigned h = (x * 73856093u) ^ (y * 19349663u) ^ (seed * 83492791u);
                    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
                    float n = ((h & 0xffff) / 65535.0f - 0.5f) * 0.02f;
                    return {v[0] + n, v[1] + n, v[2] + n};
                }
            };
            return makeRaw10(Noisy(frame * 12, 100 + frame));
        };
        const int last = 10;
        FrameParams clean0 = baseParams(0);
        runFrame(gpu, makeRaw10(Pan(last * 12)), clean0, nullptr, f); // noiseless truth of the last frame
        std::vector<int> truth;
        for (int y = OUT_H / 3; y < OUT_H * 2 / 3; ++y)
            for (int x = OUT_W / 3; x < OUT_W * 2 / 3; ++x) truth.push_back(f.luma(x, y));
        auto errorVsTruth = [&](const P010& fr) {
            double e = 0; size_t i = 0;
            for (int y = OUT_H / 3; y < OUT_H * 2 / 3; ++y)
                for (int x = OUT_W / 3; x < OUT_W * 2 / 3; ++x) { double d = fr.luma(x, y) - truth[i++]; e += d * d; }
            return std::sqrt(e / static_cast<double>(truth.size()));
        };
        FrameParams single = baseParams(0);
        runFrame(gpu, noisyPan(last), single, nullptr, f);
        double noNr = errorVsTruth(f);
        for (int alignOn = 0; alignOn < 2; ++alignOn) {
            FrameParams tp = baseParams(0);
            tp.cleanFlags[1] = 1;
            tp.cleanFlags[3] = alignOn;
            tp.noise[0] = 0.8f;
            for (int fr = 0; fr <= last; ++fr) runFrame(gpu, noisyPan(fr), tp, nullptr, f);
            double err = errorVsTruth(f);
            if (alignOn) check(err < noNr * 0.75, "aligned temporal NR denoises a pan (RMS error vs truth)", err, noNr * 0.75);
            else check(err < noNr * 1.3, "unaligned temporal NR does not ghost on a pan", err, noNr * 1.3);
        }
    }

    // 6. Lens model with all-zero distortion is an identity.
    {
        FrameParams lp = baseParams(0);
        lp.lensK[3] = 1; lp.lensF[0] = 1000; lp.lensF[1] = 1000; lp.lensF[2] = W / 2.0f; lp.lensF[3] = H / 2.0f;
        runFrame(gpu, raw, lp, nullptr, f);
        int lY = f.luma(OUT_W / 16, OUT_H / 16);
        check(std::abs(lY - yCode(appleLog(8.0))) <= 3, "zero distortion = identity", lY, yCode(appleLog(8.0)));
    }

    // 7. Ring: many frames in a row must all complete (fence/slot reuse).
    for (int i = 0; i < 12; ++i) {
        if (!runFrame(gpu, raw, baseParams(0), nullptr, f)) { std::puts("FAIL ring reuse"); ++failures; break; }
    }

    gpu.release();
    std::printf(failures ? "%d GPU FAILURES\n" : "all GPU pipeline tests passed\n", failures);
    return failures ? 1 : 0;
}
