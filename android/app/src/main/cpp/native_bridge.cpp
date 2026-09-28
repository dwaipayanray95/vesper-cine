// C ABI used by Dart FFI (lib/services/vesper_native.dart) plus the one JNI
// entry point MainActivity uses to hand over the viewfinder Surface.
#include "camera_engine.h"
#include "color_science.h"
#include "focus_controller.h"
#include "recorder.h"
#include "vulkan_engine.h"

#include <android/native_window_jni.h>
#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <cstdio>
#include <string>
#include <vector>

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
    bool awbAuto = false;        // follow the HAL's (Google) AWB neutral point
    bool lensCorrection = true;  // undo lens distortion like the stock camera
    bool hotPixelFix = true;
    float temporalNr = 0.0f;     // 0 = off, else max blend weight toward history (0.5..0.85)
    bool nrAlignment = true;     // warp history by a per-tile motion field before blending
    float chromaNr = 0.0f;       // 0 = off, 1 = full chroma smoothing
    int meterMode = 0;           // 0 = centre-weighted (+ face priority), 1 = spot at (meterX, meterY)
    float meterX = 0.5f, meterY = 0.5f; // output-normalised
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
    bool spot = false;          // measured with a spot around the tapped point
    bool faceValid = false;     // a face was found and measured separately
    double faceLogMeanG = 0;
};
MeterStats gMeter;

// Geometry of the last rendered frame, used to map taps and faces (gStateMutex).
struct FrameGeometry {
    float crop[4] = {0, 0, 1, 1};   // raw px
    float sensorMap[4] = {1, 1, 0, 0};
    int rot = 0;
    float faceNorm[4] = {0, 0, 0, 0}; // largest face in output-normalised coords (w == 0: none)
    float focusDiopters = 0;
    int afState = 0;
};
FrameGeometry gGeom;

// Chart calibration (tools/calibration). gStateMutex.
struct ColorProfile { int count = 0; Mat3 fm[2]; float kelvin[2] = {5600, 5600}; };
ColorProfile gProfile;
bool gProfileEnabled = true;
std::string gCalibrationRequest;  // base path for the next frame dump ("" = none)
std::string gCalibrationDevice;
std::string gCalibrationSaved;    // last written base path (reported in status)

std::string jsonEscape(const std::string& in);

DngCalibration withProfile(DngCalibration cal) {
    if (gProfileEnabled && gProfile.count > 0) {
        cal.profileCount = gProfile.count;
        for (int i = 0; i < gProfile.count; ++i) {
            cal.profileMatrix[i] = gProfile.fm[i];
            cal.profileKelvin[i] = gProfile.kelvin[i];
        }
    }
    return cal;
}

std::string jsonArray(const float* v, size_t n) {
    std::string s = "[";
    char b[32];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%s%.7g", i ? "," : "", v[i]);
        s += b;
    }
    return s + "]";
}

// Writes the raw RAW10 plane + everything the calibration tool needs to
// linearise it exactly like the GPU does. Runs on the camera thread, once.
void dumpCalibrationFrame(const RawFrame& f, const std::string& base, const std::string& device,
                          const Settings& s) {
    const CaptureMetadata& m = *f.meta;
    const SensorInfo& info = gCamera->sensorInfo();
    FILE* rf = std::fopen((base + ".raw10").c_str(), "wb");
    if (!rf) { __android_log_print(ANDROID_LOG_ERROR, "Vesper", "calibration dump: cannot write %s", base.c_str()); return; }
    std::fwrite(f.data, 1, std::min(static_cast<size_t>(f.rowStride) * f.height, f.size), rf);
    std::fclose(rf);
    Mat3 fmNow = gColor.forwardMatrixFor(s.color.kelvin);
    const DngCalibration& cal = info.calibration;
    std::string j = "{";
    char b[512];
    std::snprintf(b, sizeof(b), "\"format\":\"vesper-calibration-capture/1\",\"device\":\"%s\",\"cameraId\":\"0\","
                  "\"width\":%d,\"height\":%d,\"rowStride\":%d,\"cfa\":%d,\"whiteLevel\":%.1f,"
                  "\"arrayWidth\":%d,\"arrayHeight\":%d,\"kelvin\":%.0f,\"tint\":%.1f,\"iso\":%d,\"exposureNs\":%lld,",
                  jsonEscape(device).c_str(), f.width, f.height, f.rowStride, info.cfa, m.whiteLevel,
                  info.preWidth, info.preHeight, s.color.kelvin, s.color.tint, m.iso, static_cast<long long>(m.exposureNs));
    j += b;
    j += "\"blackLevel\":" + jsonArray(m.blackLevel, 4) + ",";
    j += "\"cameraNeutral\":" + jsonArray(s.color.cameraNeutral.data(), 3) + ",";
    j += "\"forwardMatrix\":" + jsonArray(fmNow.data(), 9) + ",";
    j += "\"factoryForwardMatrix1\":" + jsonArray(cal.forwardMatrix1.data(), 9) + ",";
    j += "\"factoryForwardMatrix2\":" + jsonArray(cal.forwardMatrix2.data(), 9) + ",";
    std::snprintf(b, sizeof(b), "\"factoryIlluminantKelvin\":[%.0f,%.0f],\"shadingCols\":%d,\"shadingRows\":%d,",
                  cal.illuminant1Kelvin, cal.illuminant2Kelvin, m.shadingCols, m.shadingRows);
    j += b;
    j += "\"shadingMap\":" + jsonArray(m.shadingMap.data(), m.shadingMap.size()) + "}";
    FILE* jf = std::fopen((base + ".json").c_str(), "w");
    if (!jf) return;
    std::fwrite(j.data(), 1, j.size(), jf);
    std::fclose(jf);
    __android_log_print(ANDROID_LOG_INFO, "Vesper", "Calibration frame saved: %s (.raw10/.json)", base.c_str());
}
Vec3 gAwbNeutral{0, 0, 0};

// Output-normalised (upright) <-> crop-normalised (unrotated raw), mirroring rawPosFor() in render.comp.
void outputToCrop(int rot, float ox, float oy, float& u, float& v) {
    if (rot == 90)       { u = oy;     v = 1 - ox; }
    else if (rot == 180) { u = 1 - ox; v = 1 - oy; }
    else if (rot == 270) { u = 1 - oy; v = ox; }
    else                 { u = ox;     v = oy; }
}
void cropToOutput(int rot, float u, float v, float& ox, float& oy) {
    if (rot == 90)       { ox = 1 - v; oy = u; }
    else if (rot == 180) { ox = 1 - u; oy = 1 - v; }
    else if (rot == 270) { ox = v;     oy = 1 - u; }
    else                 { ox = u;     oy = v; }
}

// Same lens model as distort() in render.comp, in raw px.
void distortRaw(const SensorInfo& s, const float map[4], float& x, float& y) {
    float ax = x * map[0] + map[2], ay = y * map[1] + map[3];
    float px = (ax - s.intrinsics[2]) / s.intrinsics[0], py = (ay - s.intrinsics[3]) / s.intrinsics[1];
    float r2 = px * px + py * py;
    float radial = 1 + r2 * (s.distortion[0] + r2 * (s.distortion[1] + r2 * s.distortion[2]));
    float dx = px * radial + 2 * s.distortion[3] * px * py + s.distortion[4] * (r2 + 2 * px * px);
    float dy = py * radial + s.distortion[3] * (r2 + 2 * py * py) + 2 * s.distortion[4] * px * py;
    x = (dx * s.intrinsics[0] + s.intrinsics[2] - map[2]) / map[0];
    y = (dy * s.intrinsics[1] + s.intrinsics[3] - map[3]) / map[1];
}
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
// Raw (unrotated) pixel position of an upright, output-normalised point.
void outputToRawPx(const FrameGeometry& g, float ox, float oy, float& rx, float& ry) {
    float u, v;
    outputToCrop(g.rot, ox, oy, u, v);
    rx = g.crop[0] + u * g.crop[2];
    ry = g.crop[1] + v * g.crop[3];
}

float rawGreen(const RawFrame& f, int cfa, const CaptureMetadata& m, int x, int y) {
    // Mean of the two greens of the 2x2 quad containing (x, y).
    x &= ~1; y &= ~1;
    float g = 0;
    for (int k = 0; k < 4; ++k) {
        int px = x + (k & 1), py = y + (k >> 1);
        int site = cfaSite(cfa, px, py);
        if (site != 1 && site != 2) continue;
        const uint8_t* grp = f.data + static_cast<size_t>(py) * f.rowStride + (px / 4) * 5;
        int i = px & 3;
        int dn = (grp[i] << 2) | ((grp[4] >> (2 * i)) & 3);
        g += 0.5f * (dn - m.blackLevel[site]) / (m.whiteLevel - m.blackLevel[site]);
    }
    return g;
}

// Whole-frame metering on a sparse grid of 2x2 quads (~12k samples), from raw
// data before white balance:
//  * centre-weighted log-average of green (the frame centre counts ~4x the corners);
//  * the largest detected face measured separately (face priority);
//  * spot mode: a tight Gaussian around the tapped point;
//  * the 99.5th percentile of each quad's brightest channel for highlight protection.
void meterFrame(const RawFrame& f) {
    const CaptureMetadata& m = *f.meta;
    const int cfa = gCamera->sensorInfo().cfa;
    FrameGeometry geo;
    int mode;
    float spotX, spotY;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        geo = gGeom;
        mode = gSettings.meterMode;
        spotX = gSettings.meterX;
        spotY = gSettings.meterY;
    }
    float cx = geo.crop[0] + geo.crop[2] * 0.5f, cy = geo.crop[1] + geo.crop[3] * 0.5f;
    float halfW = std::max(1.0f, geo.crop[2] * 0.5f), halfH = std::max(1.0f, geo.crop[3] * 0.5f);
    float spotRx = cx, spotRy = cy;
    if (mode == 1) outputToRawPx(geo, spotX, spotY, spotRx, spotRy);
    // Face rectangle (pre-correction array px) -> raw px.
    const SensorInfo& info = gCamera->sensorInfo();
    bool face = m.face[2] > m.face[0];
    float fx0 = 0, fy0 = 0, fx1 = 0, fy1 = 0;
    if (face) {
        float sx = geo.sensorMap[0] > 0 ? geo.sensorMap[0] : 1.0f;
        fx0 = (m.face[0] - info.preLeft - geo.sensorMap[2]) / sx;
        fx1 = (m.face[2] - info.preLeft - geo.sensorMap[2]) / sx;
        fy0 = (m.face[1] - info.preTop - geo.sensorMap[3]) / sx;
        fy1 = (m.face[3] - info.preTop - geo.sensorMap[3]) / sx;
    }

    constexpr int kBins = 512;
    int hist[kBins] = {};
    double logSum = 0, wSum = 0, faceSum = 0;
    int n = 0, faceN = 0;
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
            const double lg = std::log(std::max(g, 1e-4f));
            double w;
            if (mode == 1) {
                // Spot: sigma = 5% of the frame width around the tapped point.
                double dx = (x - spotRx) / (0.05 * geo.crop[2]), dy = (y - spotRy) / (0.05 * geo.crop[2]);
                w = std::exp(-0.5 * (dx * dx + dy * dy)) + 1e-4;
            } else {
                double dx = (x - cx) / halfW, dy = (y - cy) / halfH;
                w = 0.25 + 0.75 * std::exp(-(dx * dx + dy * dy) / (2 * 0.35 * 0.35));
            }
            logSum += w * lg;
            wSum += w;
            if (face && x >= fx0 && x <= fx1 && y >= fy0 && y <= fy1) { faceSum += lg; ++faceN; }
            ++hist[std::min(kBins - 1, static_cast<int>(mx * (kBins - 1)))];
            ++n;
        }
    }
    if (n == 0 || wSum <= 0) return;
    int target = static_cast<int>(n * 0.995), acc = 0, bin = kBins - 1;
    for (int b = 0; b < kBins; ++b) { acc += hist[b]; if (acc >= target) { bin = b; break; } }
    std::lock_guard<std::mutex> lk(gStateMutex);
    gMeter = {logSum / wSum, static_cast<double>(bin) / (kBins - 1), m.exposureNs, m.iso, true};
    gMeter.spot = mode == 1;
    gMeter.faceValid = mode == 0 && faceN >= 4;
    gMeter.faceLogMeanG = faceN ? faceSum / faceN : 0;
}

// --- Smooth focus -----------------------------------------------------------
// FocusController (focus_controller.cpp) decides where the lens goes; this
// worker feeds it one sample per camera frame and commands the lens. The lens
// is driven from its own thread, never from the camera frame thread, which
// holds camera locks the lens command also needs.
struct FocusSample { double p = 0, sharpness = 0, dt = 0; };
std::mutex gFocusMutex;
std::condition_variable gFocusCv;
FocusController gFocus;
bool gFocusHasSample = false;
FocusSample gFocusSample;
bool gFocusSearchActive = false;   // contrast AF running (gFocusMutex)
bool gFocusLocked = false;         // lens held where the last search/lock left it
float gFocusRegion[2] = {0.5f, 0.5f};
std::thread gFocusThread;
bool gFocusQuit = false;

double diopterToP(double d) {
    double maxD = gCamera ? gCamera->sensorInfo().minFocusDiopters : 0;
    return maxD > 0 ? std::sqrt(std::clamp(d / maxD, 0.0, 1.0)) : 0;
}
double pToDiopter(double p) {
    double maxD = gCamera ? gCamera->sensorInfo().minFocusDiopters : 0;
    return p * p * maxD;
}

void focusThreadMain() {
    double lastCmd = -1;
    std::unique_lock<std::mutex> lk(gFocusMutex);
    while (!gFocusQuit) {
        gFocusCv.wait(lk, [] { return gFocusQuit || gFocusHasSample; });
        if (gFocusQuit) break;
        FocusSample s = gFocusSample;
        gFocusHasSample = false;
        if (!gFocus.active()) continue;
        double cmd = gFocus.update(s.dt, s.p, s.sharpness);
        bool finished = !gFocus.active();
        if (finished && gFocusSearchActive) {
            gFocusSearchActive = false;
            gFocusLocked = true;
        }
        lk.unlock();
        if (std::fabs(cmd - lastCmd) > 1e-4 && gCamera) gCamera->setFocusDistance(static_cast<float>(pToDiopter(cmd)));
        lastCmd = cmd;
        lk.lock();
    }
}

// Normalised contrast (gradient energy / mean) of the green channel in a
// square around the AF point, ~10% of the frame wide.
double regionSharpness(const RawFrame& f, float ox, float oy) {
    FrameGeometry geo;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        geo = gGeom;
    }
    float rx, ry;
    outputToRawPx(geo, ox, oy, rx, ry);
    const int half = std::max(16, static_cast<int>(geo.crop[2] * 0.05f)) & ~1;
    const int x0 = std::clamp(static_cast<int>(rx) - half, 2, f.width - 4) & ~1;
    const int y0 = std::clamp(static_cast<int>(ry) - half, 2, f.height - 4) & ~1;
    const int x1 = std::min(f.width - 4, x0 + 2 * half), y1 = std::min(f.height - 4, y0 + 2 * half);
    const int step = std::max(2, ((x1 - x0) / 96) & ~1); // ~96x96 quads max
    const int cfa = gCamera->sensorInfo().cfa;
    double grad = 0, sum = 0;
    for (int y = y0; y < y1; y += step)
        for (int x = x0; x < x1; x += step) {
            float g = rawGreen(f, cfa, *f.meta, x, y);
            grad += std::fabs(rawGreen(f, cfa, *f.meta, x + 2, y) - g) + std::fabs(rawGreen(f, cfa, *f.meta, x, y + 2) - g);
            sum += g;
        }
    return sum > 0 ? grad / sum : 0;
}

void feedFocus(const RawFrame& f, double frameSeconds) {
    bool active, searching;
    float ox, oy;
    {
        std::lock_guard<std::mutex> lk(gFocusMutex);
        active = gFocus.active();
        searching = gFocusSearchActive;
        ox = gFocusRegion[0];
        oy = gFocusRegion[1];
    }
    if (!active) return;
    FocusSample s;
    s.p = diopterToP(f.meta->focusDiopters);
    s.sharpness = searching ? regionSharpness(f, ox, oy) : 0;
    s.dt = frameSeconds;
    {
        std::lock_guard<std::mutex> lk(gFocusMutex);
        gFocusSample = s;
        gFocusHasSample = true;
    }
    gFocusCv.notify_one();
}

void ensureFocusThread() {
    if (gFocusThread.joinable()) return;
    gFocusQuit = false;
    gFocusThread = std::thread(focusThreadMain);
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
    feedFocus(f, frameNs * 1e-9);
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
    // Raw stream -> pre-correction array: binned readouts scale uniformly;
    // the 16:9 readout is additionally a centred vertical crop of the array.
    const float arrayW = info.preWidth > 0 ? static_cast<float>(info.preWidth) : static_cast<float>(f.width);
    const float arrayH = info.preHeight > 0 ? static_cast<float>(info.preHeight) : static_cast<float>(f.height);
    const float sx = arrayW / f.width;
    p.sensorMap[0] = sx;
    p.sensorMap[1] = sx;
    p.sensorMap[2] = 0.0f;
    p.sensorMap[3] = std::max(0.0f, (arrayH - f.height * sx) * 0.5f);
    p.arrayInfo[0] = arrayW;
    p.arrayInfo[1] = arrayH;

    // Lens distortion: pull the crop in just enough that the corrected frame
    // never samples outside the sensor (barrel correction pushes corners out).
    double shrink = 1.0;
    if (s.lensCorrection && info.hasDistortion) {
        for (; shrink < 1.25; shrink += 0.005) {
            bool inside = true;
            double hw = cw / (2 * shrink), hh = ch / (2 * shrink), cx = f.width * 0.5, cy = f.height * 0.5;
            for (int i = 0; i < 8 && inside; ++i) {
                static const float px[8] = {-1, 0, 1, 1, 1, 0, -1, -1}, py[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
                float x = static_cast<float>(cx + px[i] * hw), y = static_cast<float>(cy + py[i] * hh);
                distortRaw(info, p.sensorMap, x, y);
                inside = x >= 1 && y >= 1 && x <= f.width - 1 && y <= f.height - 1;
            }
            if (inside) break;
        }
        p.lensK[0] = info.distortion[0];
        p.lensK[1] = info.distortion[1];
        p.lensK[2] = info.distortion[2];
        p.lensK[3] = 1.0f;
        p.lensP[0] = info.distortion[3];
        p.lensP[1] = info.distortion[4];
        for (int i = 0; i < 4; ++i) p.lensF[i] = info.intrinsics[i];
    }
    cw /= shrink;
    ch /= shrink;
    p.cropRect[0] = static_cast<float>((f.width - cw) * 0.5);
    p.cropRect[1] = static_cast<float>((f.height - ch) * 0.5);
    p.cropRect[2] = static_cast<float>(cw);
    p.cropRect[3] = static_cast<float>(ch);

    p.noise[0] = s.temporalNr;
    p.noise[1] = s.chromaNr;
    p.noise[2] = meta.noiseS > 0 ? meta.noiseS : 2e-4f; // typical phone sensor at base ISO if the HAL omits it
    p.noise[3] = meta.noiseO > 0 ? meta.noiseO : 2e-6f;
    p.cleanFlags[0] = s.hotPixelFix ? 1 : 0;
    p.cleanFlags[1] = s.temporalNr > 0 ? 1 : 0;
    p.cleanFlags[3] = s.nrAlignment ? 1 : 0;

    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        std::copy(p.cropRect, p.cropRect + 4, gGeom.crop);
        std::copy(p.sensorMap, p.sensorMap + 4, gGeom.sensorMap);
        gGeom.rot = rot;
        gGeom.focusDiopters = meta.focusDiopters;
        gGeom.afState = meta.afState;
        gGeom.faceNorm[2] = 0;
        if (meta.face[2] > meta.face[0]) {
            float c[4];
            for (int i = 0; i < 2; ++i) {
                float rx = (meta.face[i * 2] - info.preLeft - p.sensorMap[2]) / sx;
                float ry = (meta.face[i * 2 + 1] - info.preTop - p.sensorMap[3]) / sx;
                cropToOutput(rot, (rx - p.cropRect[0]) / p.cropRect[2], (ry - p.cropRect[1]) / p.cropRect[3], c[i * 2], c[i * 2 + 1]);
            }
            gGeom.faceNorm[0] = std::min(c[0], c[2]);
            gGeom.faceNorm[1] = std::min(c[1], c[3]);
            gGeom.faceNorm[2] = std::fabs(c[2] - c[0]);
            gGeom.faceNorm[3] = std::fabs(c[3] - c[1]);
        }
        // Google AWB: adopt the HAL's neutral point, smoothed so it glides instead of stepping.
        if (s.awbAuto && meta.neutral[0] > 0 && meta.neutral[1] > 0 && meta.neutral[2] > 0) {
            Vec3 n = {meta.neutral[0] / meta.neutral[1], 1.0f, meta.neutral[2] / meta.neutral[1]};
            if (gAwbNeutral[1] == 0) gAwbNeutral = n;
            for (int i = 0; i < 3; ++i) gAwbNeutral[i] += 0.15f * (n[i] - gAwbNeutral[i]);
            if ((frameIndex & 3) == 1) gSettings.color = gColor.fromCameraNeutral(gAwbNeutral);
            s.color = gSettings.color;
            p.wbGains[0] = s.color.wbGains[0];
            p.wbGains[1] = s.color.wbGains[1];
            p.wbGains[2] = s.color.wbGains[2];
        }
    }

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
    {
        std::string calBase, calDevice;
        {
            std::lock_guard<std::mutex> lk(gStateMutex);
            calBase.swap(gCalibrationRequest);
            calDevice = gCalibrationDevice;
        }
        if (!calBase.empty()) {
            dumpCalibrationFrame(f, calBase, calDevice, s);
            std::lock_guard<std::mutex> lk(gStateMutex);
            gCalibrationSaved = calBase;
        }
    }
    in.raw = f.data;
    in.rawSize = f.size;

    const bool recording = gRecorder && gRecorder->isRecording();
    int slot = -1;
    gGpu->setFrameBudgetMs(frameNs * 1e-6);
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
    const DngCalibration cal = withProfile(gCamera->sensorInfo().calibration);
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
    double scale;
    if (m.faceValid) {
        // Face priority: skin half a stop over 18% grey — a middle ground that
        // keeps light skin out of the shoulder and dark skin out of the noise.
        scale = greyRaw * std::sqrt(2.0) / std::exp(m.faceLogMeanG);
    } else {
        scale = greyRaw / std::exp(m.logMeanG);
    }
    // Highlight protection. A spot reading is a deliberate choice, so the
    // rest of the frame may clip; centre/face metering keeps <0.5% clipped.
    if (!m.spot) {
        if (m.p995 >= 0.98) scale = std::min(scale, 0.35); // clipped: true level unknown, step down ~1.5 stops
        else scale = std::min(scale, 0.92 / std::max(m.p995, 1e-3));
    }
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
    __android_log_print(ANDROID_LOG_INFO, "Vesper", "Auto-expose (%s): meanG=%.4f p99.5=%.3f x%.3f -> %.3fms ISO %d",
                        m.spot ? "spot" : (m.faceValid ? "face" : "centre"),
                        std::exp(m.faceValid ? m.faceLogMeanG : m.logMeanG), m.p995, scale, ns / 1e6, isoI);
    return 0;
}

EXPORT void vesper_set_kelvin_tint(int32_t kelvin, int32_t tint) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.awbAuto = false;
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

// White balance source: 1 = Google's AWB (HAL neutral point, continuously
// followed), 0 = manual. Switching to manual keeps the current K/tint.
EXPORT void vesper_set_auto_white_balance(int32_t enable) {
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        gSettings.awbAuto = enable != 0;
        gAwbNeutral = {0, 0, 0};
    }
    if (gCamera) gCamera->setAutoWhiteBalance(enable != 0);
}

// 0 = manual (vesper_set_focus), 1 = continuous AF (PDAF + laser, via the HAL).
// Switching from continuous to manual locks the lens where AF left it.
EXPORT void vesper_set_focus_mode(int32_t mode) {
    {
        std::lock_guard<std::mutex> lk(gFocusMutex);
        gFocus.cancel();
        gFocusSearchActive = false;
        gFocusLocked = mode != 1;
    }
    if (gCamera) gCamera->setFocusMode(mode == 1 ? FocusMode::Continuous : FocusMode::Manual);
}

// Tap-to-focus at an output-normalised point (0..1, upright viewfinder).
// Switches to continuous AF metering that region.
EXPORT void vesper_set_focus_point(float ox, float oy) {
    if (!gCamera) return;
    {
        std::lock_guard<std::mutex> lk(gFocusMutex);
        gFocus.cancel();
        gFocusSearchActive = false;
        gFocusLocked = false;
    }
    FrameGeometry g;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        g = gGeom;
    }
    const SensorInfo& info = gCamera->sensorInfo();
    float u, v;
    outputToCrop(g.rot, std::clamp(ox, 0.0f, 1.0f), std::clamp(oy, 0.0f, 1.0f), u, v);
    float ax = (g.crop[0] + u * g.crop[2]) * g.sensorMap[0] + g.sensorMap[2];
    float ay = (g.crop[1] + v * g.crop[3]) * g.sensorMap[1] + g.sensorMap[3];
    float nx = ax / std::max(1, info.preWidth), ny = ay / std::max(1, info.preHeight);
    constexpr float kHalf = 0.06f;
    gCamera->setFocusRegion(nx - kHalf, ny - kHalf, 2 * kHalf, 2 * kHalf);
    gCamera->setFocusMode(FocusMode::Continuous);
}

EXPORT void vesper_clear_focus_point() {
    if (gCamera) gCamera->setFocusRegion(0, 0, 0, 0);
}

// Cinema focus: contrast-detect AF at an output-normalised point, driven
// smoothly (never snapping), ending locked on the sharpest position.
EXPORT void vesper_focus_search_at(float ox, float oy) {
    if (!gCamera || gCamera->sensorInfo().minFocusDiopters <= 0) return;
    ensureFocusThread();
    double current = 0;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        current = diopterToP(gGeom.focusDiopters);
    }
    std::lock_guard<std::mutex> lk(gFocusMutex);
    gFocusRegion[0] = std::clamp(ox, 0.0f, 1.0f);
    gFocusRegion[1] = std::clamp(oy, 0.0f, 1.0f);
    gFocusSearchActive = true;
    gFocusLocked = false;
    gFocus.startSearch(current);
}

// Smooth rack to a manual focus distance (diopters).
EXPORT void vesper_focus_pull_to(float diopters) {
    if (!gCamera) return;
    ensureFocusThread();
    double current;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        current = diopterToP(gGeom.focusDiopters);
    }
    std::lock_guard<std::mutex> lk(gFocusMutex);
    gFocusSearchActive = false;
    gFocusLocked = false;
    gFocus.pullTo(current, diopterToP(diopters));
}

// Seconds for a full-range focus move (cinema pulls ~1-2 s).
EXPORT void vesper_set_focus_speed(float fullRangeSeconds) {
    std::lock_guard<std::mutex> lk(gFocusMutex);
    gFocus.setSpeed(fullRangeSeconds);
}

// Stops any smooth focus move / search and holds the lens where it is.
// Also used after hardware AF to lock ("AF-L").
EXPORT void vesper_focus_lock() {
    {
        std::lock_guard<std::mutex> lk(gFocusMutex);
        gFocus.cancel();
        gFocusSearchActive = false;
        gFocusLocked = true;
    }
    if (gCamera) gCamera->setFocusMode(FocusMode::Manual);
}

// Metering: mode 0 = centre-weighted with face priority, 1 = spot at (x, y) (output-normalised).
EXPORT void vesper_set_metering(int32_t mode, float x, float y) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.meterMode = mode == 1 ? 1 : 0;
    gSettings.meterX = std::clamp(x, 0.0f, 1.0f);
    gSettings.meterY = std::clamp(y, 0.0f, 1.0f);
    gMeter.valid = false; // next auto-expose waits for fresh statistics
}

EXPORT void vesper_set_face_detection(int32_t enable) {
    if (gCamera) gCamera->setFaceDetection(enable != 0);
}

// Image processing toggles (all applied on the GPU, all optional).
EXPORT void vesper_set_lens_correction(int32_t enable) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.lensCorrection = enable != 0;
}
EXPORT void vesper_set_hot_pixel_fix(int32_t enable) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.hotPixelFix = enable != 0;
}
// 0 = off; typical 0.5 (low) .. 0.85 (high): max weight given to the previous frame.
EXPORT void vesper_set_temporal_nr(float strength) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.temporalNr = std::clamp(strength, 0.0f, 0.9f);
}
// Tile alignment for temporal NR (HDR+-style motion field): keeps denoising
// through handheld shake and pans instead of rejecting moving tiles.
EXPORT void vesper_set_nr_alignment(int32_t enable) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.nrAlignment = enable != 0;
}
// 0 = off .. 1 = full chroma smoothing (luma/detail untouched).
EXPORT void vesper_set_chroma_nr(float strength) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.chromaNr = std::clamp(strength, 0.0f, 1.0f);
}

// Asks the camera thread to save the next raw frame + metadata to
// `<basePath>.raw10` / `<basePath>.json` for tools/calibration.
EXPORT void vesper_capture_calibration_frame(const char* basePath, const char* deviceModel) {
    if (!basePath) return;
    std::lock_guard<std::mutex> lk(gStateMutex);
    gCalibrationRequest = basePath;
    gCalibrationDevice = deviceModel ? deviceModel : "";
}

// Per-device chart calibration: `count` (1 or 2) forward matrices, 9 floats
// each row-major, with their CCTs. count = 0 clears it (factory calibration).
EXPORT void vesper_set_color_profile(int32_t count, const float* matrices, const float* kelvins) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gProfile.count = std::clamp(count, 0, 2);
    for (int i = 0; i < gProfile.count; ++i) {
        std::copy(matrices + 9 * i, matrices + 9 * i + 9, gProfile.fm[i].begin());
        gProfile.kelvin[i] = kelvins[i];
    }
    if (gCamera) {
        gColor.setCalibration(withProfile(gCamera->sensorInfo().calibration));
        setColorLocked(gColor.fromKelvinTint(gSettings.color.kelvin, gSettings.color.tint));
    }
}

// Switch between the chart profile (if loaded) and the factory calibration.
EXPORT void vesper_use_color_profile(int32_t enable) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gProfileEnabled = enable != 0;
    if (gCamera) {
        gColor.setCalibration(withProfile(gCamera->sensorInfo().calibration));
        setColorLocked(gColor.fromKelvinTint(gSettings.color.kelvin, gSettings.color.tint));
    }
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
    int iso, afState;
    bool awbAuto, profileActive;
    float focusD, face[4];
    std::string calSaved;
    bool focusSearching, focusLocked;
    {
        std::lock_guard<std::mutex> lk(gFocusMutex);
        focusSearching = gFocusSearchActive;
        focusLocked = gFocusLocked;
    }
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
        awbAuto = gSettings.awbAuto;
        calSaved = gCalibrationSaved;
        profileActive = gProfileEnabled && gProfile.count > 0;
        afState = gGeom.afState;
        focusD = gGeom.focusDiopters;
        std::copy(gGeom.faceNorm, gGeom.faceNorm + 4, face);
    }
    char buf[2048];
    std::snprintf(buf, sizeof(buf),
                  "{\"streaming\":%s,\"fps\":%.2f,\"raw\":[%d,%d],\"output\":[%d,%d],\"cameraDrops\":%lld,"
                  "\"kelvin\":%.0f,\"tint\":%.1f,\"recording\":%s,\"durationMs\":%lld,\"framesEncoded\":%lld,"
                  "\"framesDropped\":%lld,\"thermal\":%d,\"audio\":%s,\"codec\":\"%s\",\"stopReason\":\"%s\","
                  "\"exposureNs\":%lld,\"iso\":%d,\"awbAuto\":%s,\"afState\":%d,\"focusDiopters\":%.3f,"
                  "\"face\":[%.4f,%.4f,%.4f,%.4f],\"gpuMs\":%.2f,\"alignThrottled\":%s,\"nrThrottled\":%s,\"calibrationSaved\":\"%s\",\"profileActive\":%s,"
                  "\"focusSearching\":%s,\"focusLocked\":%s}",
                  gCamera && gCamera->isStreaming() ? "true" : "false", fps, rw, rh, ow, oh, drops, kelvin, tint,
                  r.recording ? "true" : "false", static_cast<long long>(r.durationUs / 1000),
                  static_cast<long long>(r.framesEncoded), static_cast<long long>(r.framesDropped), r.thermalStatus,
                  r.audio ? "true" : "false", jsonEscape(r.codecName).c_str(), jsonEscape(r.stopReason).c_str(), expNs, iso,
                  awbAuto ? "true" : "false", afState, focusD, face[0], face[1], face[2], face[3],
                  gGpu ? gGpu->gpuFrameMs() : 0.0, gGpu && gGpu->alignmentThrottled() ? "true" : "false",
                  gGpu && gGpu->noiseReductionThrottled() ? "true" : "false", jsonEscape(calSaved).c_str(),
                  profileActive ? "true" : "false", focusSearching ? "true" : "false", focusLocked ? "true" : "false");
    return writeString(buf, out, maxLen);
}

EXPORT void vesper_close() {
    {
        std::lock_guard<std::mutex> lk(gFocusMutex);
        gFocusQuit = true;
        gFocus.cancel();
    }
    gFocusCv.notify_all();
    if (gFocusThread.joinable()) gFocusThread.join();
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
