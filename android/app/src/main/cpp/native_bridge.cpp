// C ABI used by Dart FFI (lib/services/vesper_native.dart) plus the one JNI
// entry point MainActivity uses to hand over the viewfinder Surface.
#include "camera_engine.h"
#include "color_science.h"
#include "recorder.h"
#include "vulkan_engine.h"

#include <android/native_window_jni.h>
#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

using namespace vesper;

namespace {

struct Settings {
    int cropMode = 0;        // 0 = 16:9, 1 = 4:3 open gate
    int resolution = 0;      // 0 = 1080p, 1 = UHD
    int monitoringMode = 1;  // 0 log, 1 Rec.709, 2 false colour, 3 peaking, 4 zebras
    float zebra = 0.95f;     // fraction of sensor clip
    float peaking = 0.06f;   // Apple Log code-value gradient
    float headroomStops = 5.5f; // stops of highlight headroom above 18% grey -> clip
    double shutterAngle = 180.0;
    int64_t fixedExposureNs = 0; // > 0: shutter set as a speed, not an angle
    int32_t iso = 100;
    ColorState color;
};

std::unique_ptr<CameraEngine> gCamera;
std::unique_ptr<VulkanEngine> gGpu;
std::unique_ptr<Recorder> gRecorder;
ColorPipeline gColor;

std::mutex gStateMutex;
Settings gSettings;

// Raw centre-patch metering (camera thread writes, WB lock reads), gStateMutex.
Vec3 gCentreRaw{0, 0, 0};

// Whole-frame exposure statistics for the auto-exposure assist (gStateMutex).
struct MeterStats {
    double logMeanG = 0;   // mean of ln(green), green normalised 0..1 of clip
    double p995 = 0;       // 99.5th percentile of the brightest channel per quad
    int64_t exposureNs = 0;
    int32_t iso = 0;
    bool valid = false;
};
MeterStats gMeter;
bool gHaveCentreSample = false;

// Frame statistics, camera thread only (published under gStateMutex).
int64_t gLastTimestampNs = 0;
int64_t gCameraDrops = 0;
double gMeasuredFps = 0;
int gRawW = 0, gRawH = 0;

void outputSize(const Settings& s, int& w, int& h) {
    const bool uhd = s.resolution == 1;
    if (s.cropMode == 1) { w = uhd ? 3840 : 1920; h = uhd ? 2880 : 1440; }
    else                 { w = uhd ? 3840 : 1920; h = uhd ? 2160 : 1080; }
}

// Upright output = raw rotated by (sensor orientation - display rotation).
// The UI is locked to landscape (Surface.ROTATION_90).
int rotationDegrees() {
    constexpr int kDisplayRotation = 90;
    return ((gCamera->sensorInfo().orientation - kDisplayRotation) % 360 + 360) % 360;
}

// Never call into the camera while holding gStateMutex: the camera's frame
// callback takes gStateMutex while holding the camera's own locks.
void applyShutter() {
    double angle;
    int32_t iso;
    int64_t fixed;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        angle = gSettings.shutterAngle;
        iso = gSettings.iso;
        fixed = gSettings.fixedExposureNs;
    }
    int64_t ns = fixed > 0 ? fixed : static_cast<int64_t>(angle / 360.0 / gCamera->frameRate() * 1e9);
    gCamera->setExposure(ns, iso);
}

void setColorLocked(const ColorState& c) { gSettings.color = c; }

// Mean of a 128x128 raw patch at the frame centre, per channel, skipping
// clipped pixels — what a camera's own AWB statistics block measures.
void meterCentre(const RawFrame& f) {
    const CaptureMetadata& m = *f.meta;
    const int cfa = gCamera->sensorInfo().cfa;
    const int x0 = (f.width / 2 - 64) & ~3, y0 = (f.height / 2 - 64) & ~1;
    double sum[3] = {0, 0, 0};
    int count[3] = {0, 0, 0};
    for (int y = y0; y < y0 + 128; ++y) {
        const uint8_t* row = f.data + static_cast<size_t>(y) * f.rowStride;
        for (int x = x0; x < x0 + 128; x += 4) {
            const uint8_t* g = row + (x / 4) * 5;
            if (g + 5 > f.data + f.size) return;
            for (int i = 0; i < 4; ++i) {
                int dn = (g[i] << 2) | ((g[4] >> (2 * i)) & 3);
                int site = cfaSite(cfa, x + i, y);
                if (dn >= m.whiteLevel - 1) continue;
                double v = (dn - m.blackLevel[site]) / (m.whiteLevel - m.blackLevel[site]);
                int ch = site == 0 ? 0 : (site == 3 ? 2 : 1);
                sum[ch] += v;
                ++count[ch];
            }
        }
    }
    if (!count[0] || !count[1] || !count[2]) return;
    std::lock_guard<std::mutex> lk(gStateMutex);
    gCentreRaw = {static_cast<float>(sum[0] / count[0]), static_cast<float>(sum[1] / count[1]), static_cast<float>(sum[2] / count[2])};
    gHaveCentreSample = true;
}

// Whole-frame metering on a sparse grid of 2x2 quads (~12k samples), from
// raw data before white balance: log-average green for mid-tones and the
// 99.5th percentile of each quad's brightest channel for highlight clipping.
void meterFrame(const RawFrame& f) {
    const CaptureMetadata& m = *f.meta;
    const int cfa = gCamera->sensorInfo().cfa;
    constexpr int kBins = 512;
    int hist[kBins] = {};
    double logSum = 0;
    int n = 0;
    const int step = std::max(8, (f.width / 128) & ~3);
    for (int y = step / 2 & ~1; y + 1 < f.height; y += step) {
        for (int x = step / 2 & ~3; x + 1 < f.width; x += step) {
            float g = 0, mx = 0;
            for (int k = 0; k < 4; ++k) {
                int px = x + (k & 1), py = y + (k >> 1);
                const uint8_t* grp = f.data + static_cast<size_t>(py) * f.rowStride + (px / 4) * 5;
                if (grp + 5 > f.data + f.size) return;
                int i = px & 3;
                int dn = (grp[i] << 2) | ((grp[4] >> (2 * i)) & 3);
                int site = cfaSite(cfa, px, py);
                float v = std::max(0.0f, (dn - m.blackLevel[site]) / (m.whiteLevel - m.blackLevel[site]));
                if (site == 1 || site == 2) g += 0.5f * v;
                mx = std::max(mx, v);
            }
            logSum += std::log(std::max(g, 1e-4f));
            ++hist[std::min(kBins - 1, static_cast<int>(mx * (kBins - 1)))];
            ++n;
        }
    }
    if (n == 0) return;
    int target = static_cast<int>(n * 0.995), acc = 0, bin = kBins - 1;
    for (int b = 0; b < kBins; ++b) { acc += hist[b]; if (acc >= target) { bin = b; break; } }
    std::lock_guard<std::mutex> lk(gStateMutex);
    gMeter = {logSum / n, static_cast<double>(bin) / (kBins - 1), m.exposureNs, m.iso, true};
}

void onFrame(const RawFrame& f) {
    if (!gGpu) return;
    static int frameIndex = 0;
    const SensorInfo& info = gCamera->sensorInfo();
    const CaptureMetadata& meta = *f.meta;

    // Drop detection / fps from sensor timestamps.
    double frameNs = 1e9 / gCamera->frameRate();
    if (gLastTimestampNs > 0) {
        double gap = static_cast<double>(f.timestampNs - gLastTimestampNs);
        if (gap > 1.5 * frameNs) gCameraDrops += static_cast<int64_t>(std::lround(gap / frameNs)) - 1;
        gMeasuredFps = 0.9 * gMeasuredFps + 0.1 * (1e9 / std::max(gap, 1.0));
    }
    gLastTimestampNs = f.timestampNs;
    if ((++frameIndex & 3) == 0) meterCentre(f);
    if ((frameIndex & 3) == 2) meterFrame(f);

    Settings s;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        s = gSettings;
        gRawW = f.width;
        gRawH = f.height;
    }
    int outW, outH;
    outputSize(s, outW, outH);
    int rot = rotationDegrees();

    FrameInput in;
    FrameParams& p = in.params;
    std::copy(meta.blackLevel, meta.blackLevel + 4, p.blackLevel);
    p.wbGains[0] = s.color.wbGains[0];
    p.wbGains[1] = s.color.wbGains[1];
    p.wbGains[2] = s.color.wbGains[2];
    p.wbGains[3] = meta.whiteLevel;
    p.rawInfo[0] = f.width;
    p.rawInfo[1] = f.height;
    p.rawInfo[2] = f.rowStride;
    p.rawInfo[3] = info.cfa;
    p.quadInfo[0] = f.width / 2;
    p.quadInfo[1] = f.height / 2;
    p.quadInfo[2] = meta.shadingCols;
    p.quadInfo[3] = meta.shadingRows;
    if (!meta.shadingMap.empty()) {
        in.shading = meta.shadingMap.data();
        in.shadingFloats = meta.shadingMap.size();
    }

    // Largest centred crop of the output aspect (measured in unrotated raw space).
    double aspect = (rot == 90 || rot == 270) ? static_cast<double>(outH) / outW : static_cast<double>(outW) / outH;
    double cw = f.width, ch = f.width / aspect;
    if (ch > f.height) { ch = f.height; cw = f.height * aspect; }
    p.cropRect[0] = static_cast<float>(std::floor((f.width - cw) / 4) * 2);
    p.cropRect[1] = static_cast<float>(std::floor((f.height - ch) / 4) * 2);
    p.cropRect[2] = static_cast<float>(cw);
    p.cropRect[3] = static_cast<float>(ch);

    p.outInfo[0] = outW;
    p.outInfo[1] = outH;
    p.outInfo[2] = rot;
    p.outInfo[3] = s.monitoringMode;
    for (int py = 0; py < 2; ++py) {
        for (int px = 0; px < 2; ++px) {
            int site = cfaSite(info.cfa, px, py);
            if (site == 0) { p.cfaOffsets[0] = px + 0.5f; p.cfaOffsets[1] = py + 0.5f; }
            if (site == 3) { p.cfaOffsets[2] = px + 0.5f; p.cfaOffsets[3] = py + 0.5f; }
        }
    }
    float k = 0.18f * std::exp2(s.headroomStops);
    p.exposure[0] = k;
    p.exposure[1] = s.zebra;
    p.exposure[2] = s.peaking;
    p.exposure[3] = std::log2(k / 0.18f);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) p.camToRec2020[r * 4 + c] = s.color.camToRec2020[r * 3 + c];
        p.camToRec2020[r * 4 + 3] = 0.0f;
    }
    in.raw = f.data;
    in.rawSize = f.size;

    const bool recording = gRecorder && gRecorder->isRecording();
    int slot = -1;
    bool ok = gGpu->processFrame(in, recording ? &slot : nullptr);
    if (ok && recording && slot >= 0) gRecorder->submitVideoFrame(slot, f.timestampNs);
    if (!ok) ++gCameraDrops;
}

bool ensureInit() {
    if (!gCamera) gCamera = std::make_unique<CameraEngine>();
    if (!gGpu) gGpu = std::make_unique<VulkanEngine>();
    if (!gRecorder) gRecorder = std::make_unique<Recorder>();
    return gCamera->initialize() && gGpu->initialize();
}

int writeString(const std::string& s, char* out, int32_t maxLen) {
    if (!out || maxLen <= 0 || s.size() + 1 > static_cast<size_t>(maxLen)) return -2;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return static_cast<int>(s.size());
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

} // namespace

#define EXPORT __attribute__((visibility("default")))

extern "C" {

EXPORT int32_t vesper_init() {
    bool ok = ensureInit();
    __android_log_print(ANDROID_LOG_INFO, "Vesper", "vesper_init: %d", ok);
    return ok ? 0 : -1;
}

EXPORT int32_t vesper_enumerate_cameras(char* out, int32_t maxLen) {
    if (!gCamera) return -1;
    std::string json = "[";
    auto devices = gCamera->enumerateCameras();
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& d = devices[i];
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "{\"id\":\"%s\",\"facing\":%d,\"hwLevel\":%d,\"supportsRaw10\":%s,\"rawWidth\":%d,\"rawHeight\":%d,\"maxFps\":%.2f}",
                      jsonEscape(d.id).c_str(), d.facing, d.hardwareLevel, d.supportsRaw10 ? "true" : "false",
                      d.largestRaw.width, d.largestRaw.height, d.largestRaw.maxFps());
        json += buf;
        if (i + 1 < devices.size()) json += ",";
    }
    json += "]";
    return writeString(json, out, maxLen);
}

EXPORT int32_t vesper_open_camera(const char* id) {
    if (!gCamera || !id) return -1;
    if (!gCamera->openCamera(id)) return -1;
    const DngCalibration cal = gCamera->sensorInfo().calibration;
    std::lock_guard<std::mutex> lk(gStateMutex);
    gColor.setCalibration(cal);
    gColor.setClipLinear(0.18f * std::exp2(gSettings.headroomStops));
    setColorLocked(gColor.fromKelvinTint(gSettings.color.kelvin, gSettings.color.tint));
    gHaveCentreSample = false;
    gLastTimestampNs = 0;
    gCameraDrops = 0;
    return 0;
}

// Streams the sensor's largest RAW10 mode — on Pixel that is the 2x2-binned
// readout (e.g. 4080x3072), which is the right choice for video: full field
// of view, best SNR per output pixel, fastest readout (least rolling shutter).
} // extern "C"

namespace {

// Picks the RAW10 mode for the crop and frame rate: matching aspect first
// (the 16:9 binned readout is faster than 4:3 — 60 fps on Pixel 10), then any
// mode fast enough, then the largest. All Pixel modes cover the full width.
RawMode pickMode(int cropMode, double fps) {
    const auto& modes = gCamera->sensorInfo().rawModes; // largest first
    const double want = cropMode == 1 ? 4.0 / 3.0 : 16.0 / 9.0;
    const RawMode* best = nullptr;
    auto score = [&](const RawMode& m) {
        double aspect = static_cast<double>(m.width) / m.height;
        bool fast = m.maxFps() + 0.5 >= fps;
        bool aspectOk = std::fabs(aspect - want) < 0.05 || (cropMode == 0 && aspect < want); // 4:3 can be cropped to 16:9
        bool fullWidth = m.width >= modes.front().width * 0.9;
        return (fast ? 8 : 0) + (fullWidth ? 4 : 0) + (std::fabs(aspect - want) < 0.05 ? 2 : 0) + (aspectOk ? 1 : 0);
    };
    for (const auto& m : modes) if (!best || score(m) > score(*best)) best = &m;
    return best ? *best : RawMode{};
}

// (Re)starts the capture stream if the chosen RAW mode changed.
int32_t restartStreamIfNeeded(bool force) {
    int crop;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        crop = gSettings.cropMode;
    }
    RawMode mode = pickMode(crop, gCamera->frameRate());
    if (mode.width == 0) return -1;
    if (!force && gCamera->isStreaming() && gCamera->streamWidth() == mode.width && gCamera->streamHeight() == mode.height) return 0;
    gLastTimestampNs = 0;
    bool ok = gCamera->startCapture(mode.width, mode.height, onFrame);
    __android_log_print(ANDROID_LOG_INFO, "Vesper", "Stream mode %dx%d (max %.1f fps) for crop %d @ %.3f fps: %d",
                        mode.width, mode.height, mode.maxFps(), crop, gCamera->frameRate(), ok);
    return ok ? 0 : -1;
}

} // namespace

extern "C" {

EXPORT int32_t vesper_start_stream() {
    if (!gCamera) return -1;
    int32_t r = restartStreamIfNeeded(true);
    applyShutter();
    return r;
}

EXPORT int32_t vesper_stop_stream() {
    if (gRecorder) gRecorder->stop("user");
    if (gCamera) gCamera->stopCapture();
    return 0;
}

EXPORT void vesper_close_camera() {
    if (gRecorder) gRecorder->stop("user");
    if (gCamera) gCamera->closeCamera();
}

EXPORT void vesper_set_frame_rate(double fps) {
    if (!gCamera) return;
    gCamera->setFrameRate(fps);
    if (gCamera->isStreaming()) restartStreamIfNeeded(false);
    applyShutter();
}

// Shutter as an angle: exposure follows the frame rate.
EXPORT void vesper_set_shutter_angle(double angle, int32_t iso) {
    if (!gCamera) return;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        gSettings.shutterAngle = std::clamp(angle, 0.1, 360.0);
        gSettings.fixedExposureNs = 0;
        gSettings.iso = iso;
    }
    applyShutter();
}

// Shutter as a speed (exposure time in ns), independent of frame rate.
// The camera clamps it to [sensor minimum, frame duration].
EXPORT void vesper_set_exposure_time(int64_t exposureNs, int32_t iso) {
    if (!gCamera) return;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        gSettings.fixedExposureNs = std::max<int64_t>(exposureNs, 1);
        gSettings.iso = iso;
    }
    applyShutter();
}

// {"minExposureNs":..,"maxExposureNs":..,"minIso":..,"maxIso":..,"minFocus":..,"modes":[{"w":..,"h":..,"maxFps":..}]}
EXPORT int32_t vesper_get_capabilities(char* out, int32_t maxLen) {
    if (!gCamera) return -1;
    const SensorInfo& s = gCamera->sensorInfo();
    char buf[256];
    std::snprintf(buf, sizeof(buf), "{\"minExposureNs\":%lld,\"maxExposureNs\":%lld,\"minIso\":%d,\"maxIso\":%d,\"minFocus\":%.3f,\"modes\":[",
                  static_cast<long long>(s.minExposureNs), static_cast<long long>(s.maxExposureNs), s.minIso, s.maxIso, s.minFocusDiopters);
    std::string json = buf;
    for (size_t i = 0; i < s.rawModes.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%s{\"w\":%d,\"h\":%d,\"maxFps\":%.2f}", i ? "," : "",
                      s.rawModes[i].width, s.rawModes[i].height, s.rawModes[i].maxFps());
        json += buf;
    }
    json += "]}";
    return writeString(json, out, maxLen);
}

// One-shot auto-exposure assist from the latest whole-frame raw statistics.
// Places the scene's log-average at 18% grey *unless* that would clip more
// than 0.5% of the frame — highlights win. priority 0 = keep the shutter
// (move ISO first, cinema-style), 1 = keep ISO (move the shutter first).
// Writes the new exposure/ISO; returns 0, or -1 if no statistics yet.
// Call repeatedly for a converging result on very over/under-exposed scenes.
EXPORT int32_t vesper_auto_expose(int32_t priority, int64_t* outExposureNs, int32_t* outIso) {
    if (!gCamera) return -1;
    MeterStats m;
    float headroom;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        m = gMeter;
        headroom = gSettings.headroomStops;
    }
    if (!m.valid || m.exposureNs <= 0 || m.iso <= 0) return -1;
    const SensorInfo& info = gCamera->sensorInfo();

    const double greyRaw = std::exp2(-headroom);          // raw green level that encodes as 18% grey
    double scale = greyRaw / std::exp(m.logMeanG);
    if (m.p995 >= 0.98) scale = std::min(scale, 0.35);    // clipped: true level unknown, step down ~1.5 stops
    else scale = std::min(scale, 0.92 / std::max(m.p995, 1e-3));
    scale = std::clamp(scale, 1.0 / 64, 64.0);

    const double frameNs = 1e9 / gCamera->frameRate();
    const int32_t isoCap = std::min(info.maxIso, 3200);    // beyond this noise dominates
    double t = static_cast<double>(m.exposureNs), iso = m.iso;
    const double target = t * iso * scale;
    auto clampT = [&](double v) { return std::clamp(v, static_cast<double>(info.minExposureNs), std::min(frameNs, static_cast<double>(info.maxExposureNs))); };
    auto clampIso = [&](double v) { return std::clamp(v, static_cast<double>(info.minIso), static_cast<double>(isoCap)); };
    if (priority == 0) {
        iso = clampIso(target / t);
        t = clampT(target / iso);
    } else {
        t = clampT(target / iso);
        iso = clampIso(target / t);
    }
    int64_t ns = static_cast<int64_t>(t);
    int32_t isoI = static_cast<int32_t>(std::lround(iso));
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        gSettings.iso = isoI;
        if (priority != 0 || std::llabs(ns - m.exposureNs) > m.exposureNs / 50) gSettings.fixedExposureNs = ns;
        gMeter.valid = false; // wait for a frame at the new exposure before metering again
    }
    applyShutter();
    if (outExposureNs) *outExposureNs = ns;
    if (outIso) *outIso = isoI;
    __android_log_print(ANDROID_LOG_INFO, "Vesper", "Auto-expose: logMeanG=%.4f p99.5=%.3f x%.3f -> %.3fms ISO %d",
                        std::exp(m.logMeanG), m.p995, scale, ns / 1e6, isoI);
    return 0;
}

EXPORT void vesper_set_kelvin_tint(int32_t kelvin, int32_t tint) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    setColorLocked(gColor.fromKelvinTint(kelvin, tint));
}

// One-shot white balance off the raw centre patch. Writes the equivalent
// Kelvin/tint back so the UI dial can follow. Returns 0 on success.
EXPORT int32_t vesper_lock_white_balance(double* outKelvin, double* outTint) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    if (!gHaveCentreSample) return -1;
    Vec3 n = gCentreRaw;
    if (n[0] < 0.005f || n[1] < 0.005f || n[2] < 0.005f) return -2; // too dark to trust
    ColorState c = gColor.fromCameraNeutral(n);
    setColorLocked(c);
    if (outKelvin) *outKelvin = c.kelvin;
    if (outTint) *outTint = c.tint;
    __android_log_print(ANDROID_LOG_INFO, "Vesper", "WB lock: raw neutral %.4f %.4f %.4f -> %.0fK tint %.1f, gains R%.3f B%.3f",
                        n[0], n[1], n[2], c.kelvin, c.tint, c.wbGains[0], c.wbGains[2]);
    return 0;
}

EXPORT void vesper_set_ois(int32_t enable) { if (gCamera) gCamera->setOpticalStabilization(enable != 0); }
EXPORT void vesper_set_focus(float diopters) { if (gCamera) gCamera->setFocusDistance(diopters); }
EXPORT float vesper_get_min_focus() { return gCamera ? gCamera->sensorInfo().minFocusDiopters : 0.0f; }

EXPORT void vesper_set_crop_mode(int32_t mode) {
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        gSettings.cropMode = mode == 1 ? 1 : 0;
    }
    if (gCamera && gCamera->isStreaming()) {
        restartStreamIfNeeded(false);
        applyShutter();
    }
}

EXPORT void vesper_set_resolution(int32_t resolution) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.resolution = resolution == 1 ? 1 : 0;
}

EXPORT void vesper_get_output_size(int32_t* w, int32_t* h) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    int ow, oh;
    outputSize(gSettings, ow, oh);
    if (w) *w = ow;
    if (h) *h = oh;
}

EXPORT void vesper_set_monitoring_mode(int32_t mode) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.monitoringMode = std::clamp(mode, 0, 4);
}

EXPORT void vesper_set_zebra_threshold(float t) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.zebra = std::clamp(t, 0.5f, 1.0f);
}

// Highlight headroom above 18% grey (stops). Sensor clip maps to scene-linear
// 0.18 * 2^stops; 5.5 puts clip at Apple Log ~0.95. See README.
EXPORT void vesper_set_highlight_headroom(float stops) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.headroomStops = std::clamp(stops, 3.0f, 6.3f);
    gColor.setClipLinear(0.18f * std::exp2(gSettings.headroomStops));
    setColorLocked(gColor.fromKelvinTint(gSettings.color.kelvin, gSettings.color.tint));
}

// Takes ownership of `fd`. codec: 0 = HEVC, 1 = AV1. Returns 0 on success.
EXPORT int32_t vesper_start_recording(int32_t fd, int32_t codec, int32_t audio) {
    if (!gRecorder || !gGpu || !gCamera || !gCamera->isStreaming()) {
        if (fd >= 0) close(fd);
        return -1;
    }
    RecorderConfig cfg;
    cfg.fd = fd;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        outputSize(gSettings, cfg.width, cfg.height);
    }
    cfg.fps = gCamera->frameRate();
    cfg.codec = codec == 1 ? VideoCodec::Av1 : VideoCodec::Hevc;
    cfg.audio = audio != 0;
    cfg.timestampRealtime = gCamera->sensorInfo().timestampRealtime;
    std::string error;
    return gRecorder->start(cfg, gGpu.get(), &error) ? 0 : -1;
}

EXPORT void vesper_stop_recording() {
    if (gRecorder) gRecorder->stop("user");
}

EXPORT int32_t vesper_get_status(char* out, int32_t maxLen) {
    RecorderStatus r = gRecorder ? gRecorder->status() : RecorderStatus{};
    int ow, oh, rw, rh;
    double kelvin, tint, fps;
    long long drops, expNs;
    int iso;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        outputSize(gSettings, ow, oh);
        kelvin = gSettings.color.kelvin;
        tint = gSettings.color.tint;
        rw = gRawW;
        rh = gRawH;
        fps = gMeasuredFps;
        drops = gCameraDrops;
        expNs = gMeter.exposureNs;
        iso = gMeter.iso;
    }
    char buf[768];
    std::snprintf(buf, sizeof(buf),
                  "{\"streaming\":%s,\"fps\":%.2f,\"raw\":[%d,%d],\"output\":[%d,%d],\"cameraDrops\":%lld,"
                  "\"kelvin\":%.0f,\"tint\":%.1f,\"recording\":%s,\"durationMs\":%lld,\"framesEncoded\":%lld,"
                  "\"framesDropped\":%lld,\"thermal\":%d,\"audio\":%s,\"codec\":\"%s\",\"stopReason\":\"%s\","
                  "\"exposureNs\":%lld,\"iso\":%d}",
                  gCamera && gCamera->isStreaming() ? "true" : "false", fps, rw, rh, ow, oh, drops, kelvin, tint,
                  r.recording ? "true" : "false", static_cast<long long>(r.durationUs / 1000),
                  static_cast<long long>(r.framesEncoded), static_cast<long long>(r.framesDropped), r.thermalStatus,
                  r.audio ? "true" : "false", jsonEscape(r.codecName).c_str(), jsonEscape(r.stopReason).c_str(), expNs, iso);
    return writeString(buf, out, maxLen);
}

EXPORT void vesper_close() {
    if (gRecorder) gRecorder->stop("user");
    if (gCamera) gCamera->closeCamera();
    if (gGpu) gGpu->release();
}

JNIEXPORT jint JNICALL Java_com_vesper_cine_MainActivity_nativeSetViewfinderSurface(JNIEnv* env, jobject, jobject surface) {
    ensureInit();
    ANativeWindow* win = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (gGpu) gGpu->setViewfinderWindow(win); // takes its own reference
    if (win) ANativeWindow_release(win);
    return 0;
}

} // extern "C"
