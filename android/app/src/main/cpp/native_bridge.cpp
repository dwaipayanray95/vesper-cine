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
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        angle = gSettings.shutterAngle;
        iso = gSettings.iso;
    }
    gCamera->setExposure(static_cast<int64_t>(angle / 360.0 / gCamera->frameRate() * 1e9), iso);
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

extern "C" {

#define EXPORT __attribute__((visibility("default")))

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
EXPORT int32_t vesper_start_stream() {
    if (!gCamera) return -1;
    const auto& modes = gCamera->sensorInfo().rawModes;
    if (modes.empty()) return -1;
    const RawMode& mode = modes.front();
    return gCamera->startCapture(mode.width, mode.height, onFrame) ? 0 : -1;
}

EXPORT int32_t vesper_stop_stream() {
    if (gRecorder) gRecorder->stop("user");
    if (gCamera) gCamera->stopCapture();
    return 0;
}

EXPORT void vesper_set_frame_rate(double fps) {
    if (!gCamera) return;
    gCamera->setFrameRate(fps);
    applyShutter();
}

EXPORT void vesper_set_shutter_angle(double angle, int32_t iso) {
    if (!gCamera) return;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        gSettings.shutterAngle = std::clamp(angle, 1.0, 360.0);
        gSettings.iso = iso;
    }
    applyShutter();
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
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.cropMode = mode == 1 ? 1 : 0;
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
    long long drops;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        outputSize(gSettings, ow, oh);
        kelvin = gSettings.color.kelvin;
        tint = gSettings.color.tint;
        rw = gRawW;
        rh = gRawH;
        fps = gMeasuredFps;
        drops = gCameraDrops;
    }
    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "{\"streaming\":%s,\"fps\":%.2f,\"raw\":[%d,%d],\"output\":[%d,%d],\"cameraDrops\":%lld,"
                  "\"kelvin\":%.0f,\"tint\":%.1f,\"recording\":%s,\"durationMs\":%lld,\"framesEncoded\":%lld,"
                  "\"framesDropped\":%lld,\"thermal\":%d,\"audio\":%s,\"codec\":\"%s\",\"stopReason\":\"%s\"}",
                  gCamera && gCamera->isStreaming() ? "true" : "false", fps, rw, rh, ow, oh, drops, kelvin, tint,
                  r.recording ? "true" : "false", static_cast<long long>(r.durationUs / 1000),
                  static_cast<long long>(r.framesEncoded), static_cast<long long>(r.framesDropped), r.thermalStatus,
                  r.audio ? "true" : "false", jsonEscape(r.codecName).c_str(), jsonEscape(r.stopReason).c_str());
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
