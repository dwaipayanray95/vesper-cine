// C ABI used by Dart FFI (lib/services/vesper_native.dart) plus the one JNI
// entry point MainActivity uses to hand over the viewfinder Surface.
#include "camera_engine.h"
#include "app_log.h"
#include "color_science.h"
#include "focus_controller.h"
#include "iso_analysis.h"
#include "recorder.h"
#include "vulkan_engine.h"

#include <android/native_window_jni.h>
#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <iterator>
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
    int sharpening = 1;      // detail enhancement 0 off, 1 low, 2 medium, 3 high
    bool oversampling = true; // HQ: full-res green luma, area-downsampled (green.comp)
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
    if (!rf) { vesperLog(ANDROID_LOG_ERROR, "Vesper", "calibration dump: cannot write %s", base.c_str()); return; }
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
    vesperLog(ANDROID_LOG_INFO, "Vesper", "Calibration frame saved: %s (.raw10/.json)", base.c_str());
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
// The UI runs in either landscape (Surface.ROTATION_90 or ROTATION_270); the
// activity reports changes via nativeSetDisplayRotation. While recording the
// rotation is frozen so a clip never flips mid-take.
std::atomic<int> gDisplayRotation{90};
int gRecordingRotation = -1; // frame thread only: rotation captured at record start

int rotationDegrees() {
    return ((gCamera->sensorInfo().orientation - gDisplayRotation.load()) % 360 + 360) % 360;
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
float gWbPoint[2] = {0.5f, 0.5f}; // eyedropper sample point, output-normalised (gStateMutex)
void outputToRawPx(const FrameGeometry& g, float ox, float oy, float& rx, float& ry);

// Raw R/G/B of a small patch around the eyedropper point (frame centre by
// default), shading-corrected, before white balance. Feeds tap-to-pick WB.
void meterCentre(const RawFrame& f) {
    const CaptureMetadata& m = *f.meta;
    const SensorInfo& info = gCamera->sensorInfo();
    const int cfa = info.cfa;
    FrameGeometry geo;
    float px, py;
    bool lensCorrection;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        geo = gGeom;
        px = gWbPoint[0];
        py = gWbPoint[1];
        lensCorrection = gSettings.lensCorrection;
    }
    float rx, ry;
    outputToRawPx(geo, px, py, rx, ry);
    // The viewfinder is lens-corrected: sample where that point came from on the sensor.
    if (lensCorrection && info.hasDistortion) distortRaw(info, geo.sensorMap, rx, ry);
    const int half = std::max(16, f.width / 80) & ~3; // ~2.5% of the frame wide
    const int x0 = std::clamp(static_cast<int>(rx) - half, 0, std::max(0, f.width - 2 * half)) & ~3;
    const int y0 = std::clamp(static_cast<int>(ry) - half, 0, std::max(0, f.height - 2 * half)) & ~1;
    // Per-channel lens shading gain at the patch (the map spans the sensor array).
    float gain[4] = {1, 1, 1, 1};
    if (!info.lensShadingApplied && m.shadingCols > 0 && m.shadingRows > 0 &&
        m.shadingMap.size() >= static_cast<size_t>(m.shadingCols * m.shadingRows * 4)) {
        float ax = (x0 + half) * geo.sensorMap[0] + geo.sensorMap[2];
        float ay = (y0 + half) * geo.sensorMap[1] + geo.sensorMap[3];
        int gx = std::clamp(static_cast<int>(std::lround(ax / std::max(1, info.preWidth) * (m.shadingCols - 1))), 0, m.shadingCols - 1);
        int gy = std::clamp(static_cast<int>(std::lround(ay / std::max(1, info.preHeight) * (m.shadingRows - 1))), 0, m.shadingRows - 1);
        for (int c = 0; c < 4; ++c) gain[c] = m.shadingMap[(gy * m.shadingCols + gx) * 4 + c];
    }
    double sum[3] = {0, 0, 0};
    int count[3] = {0, 0, 0};
    for (int y = y0; y < y0 + 2 * half && y < f.height; ++y) {
        const uint8_t* row = f.data + static_cast<size_t>(y) * f.rowStride;
        for (int x = x0; x < x0 + 2 * half && x + 3 < f.width; x += 4) {
            const uint8_t* g = row + (x / 4) * 5;
            if (g + 5 > f.data + f.size) return;
            for (int i = 0; i < 4; ++i) {
                int dn = (g[i] << 2) | ((g[4] >> (2 * i)) & 3);
                int site = cfaSite(cfa, x + i, y);
                if (dn >= m.whiteLevel - 1) continue;
                double v = (dn - m.blackLevel[site]) / (m.whiteLevel - m.blackLevel[site]) * gain[site];
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

std::atomic<bool> gExposureRamping{false}; // auto-exposure glide in progress
int gNativeBaseIso = 0, gNativeHcgIso = 0; // measured native ISOs (Settings > Native ISO Analysis), gStateMutex

// --- Scopes -----------------------------------------------------------------
// Luma histogram + waveform of the recorded signal (Apple Log Y', BT.2020
// weights), computed from a sparse raw grid in output orientation so the
// waveform's x axis matches the viewfinder. Only while the overlay is shown.
constexpr int kHistBins = 64, kWaveCols = 128, kWaveBins = 64, kWaveRows = 72;
std::atomic<bool> gScopesEnabled{false};
std::mutex gScopeMutex;
float gHist[kHistBins];                 // normalised to the tallest bin
float gWave[kWaveCols * kWaveBins];     // fraction of each column's samples per bin
bool gScopesValid = false;

float appleLogEncode(float x) {
    const float R0 = -0.05641088f, Rt = 0.01f, c = 47.28711236f;
    const float beta = 0.00964052f, gamma = 0.08550479f, delta = 0.69336945f;
    if (x >= Rt) return gamma * std::log2(x + beta) + delta;
    if (x >= R0) return c * (x - R0) * (x - R0);
    return 0.0f;
}

void computeScopes(const RawFrame& f) {
    const CaptureMetadata& m = *f.meta;
    const SensorInfo& info = gCamera->sensorInfo();
    FrameGeometry geo;
    ColorState color;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        geo = gGeom;
        color = gSettings.color;
    }
    const bool shade = !info.lensShadingApplied && m.shadingCols > 0 && m.shadingRows > 0 &&
                       m.shadingMap.size() >= static_cast<size_t>(m.shadingCols * m.shadingRows * 4);
    static int hist[kHistBins];
    static int wave[kWaveCols * kWaveBins];
    std::fill(std::begin(hist), std::end(hist), 0);
    std::fill(std::begin(wave), std::end(wave), 0);
    int colN[kWaveCols] = {};
    for (int col = 0; col < kWaveCols; ++col) {
        for (int row = 0; row < kWaveRows; ++row) {
            float rx, ry;
            outputToRawPx(geo, (col + 0.5f) / kWaveCols, (row + 0.5f) / kWaveRows, rx, ry);
            int x = static_cast<int>(rx) & ~1, y = static_cast<int>(ry) & ~1;
            if (x < 0 || y < 0 || x + 1 >= f.width || y + 1 >= f.height) continue;
            float gain[4] = {1, 1, 1, 1};
            if (shade) {
                float ax = x * geo.sensorMap[0] + geo.sensorMap[2], ay = y * geo.sensorMap[1] + geo.sensorMap[3];
                int gx = std::clamp(static_cast<int>(ax / std::max(1, info.preWidth) * (m.shadingCols - 1) + 0.5f), 0, m.shadingCols - 1);
                int gy = std::clamp(static_cast<int>(ay / std::max(1, info.preHeight) * (m.shadingRows - 1) + 0.5f), 0, m.shadingRows - 1);
                for (int c = 0; c < 4; ++c) gain[c] = m.shadingMap[(gy * m.shadingCols + gx) * 4 + c];
            }
            float rgb[3] = {0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                int px = x + (k & 1), py = y + (k >> 1);
                const uint8_t* grp = f.data + static_cast<size_t>(py) * f.rowStride + (px / 4) * 5;
                if (grp + 5 > f.data + f.size) return;
                int i = px & 3;
                int dn = (grp[i] << 2) | ((grp[4] >> (2 * i)) & 3);
                int site = cfaSite(info.cfa, px, py);
                float v = std::max(0.0f, (dn - m.blackLevel[site]) / (m.whiteLevel - m.blackLevel[site])) * gain[site];
                int ch = site == 0 ? 0 : (site == 3 ? 2 : 1);
                rgb[ch] += ch == 1 ? 0.5f * v : v;
            }
            float cam[3];
            for (int c = 0; c < 3; ++c) cam[c] = std::min(rgb[c] * color.wbGains[c], 1.0f); // highlight clip, as unpack.comp
            float y2020 = 0;
            const float w[3] = {0.2627f, 0.6780f, 0.0593f};
            for (int r = 0; r < 3; ++r) {
                float lin = color.camToRec2020[r * 3] * cam[0] + color.camToRec2020[r * 3 + 1] * cam[1] + color.camToRec2020[r * 3 + 2] * cam[2];
                y2020 += w[r] * appleLogEncode(lin);
            }
            float code = std::clamp(y2020, 0.0f, 1.0f);
            ++hist[std::min(kHistBins - 1, static_cast<int>(code * kHistBins))];
            ++wave[col * kWaveBins + std::min(kWaveBins - 1, static_cast<int>(code * kWaveBins))];
            ++colN[col];
        }
    }
    int maxBin = 1;
    for (int b = 0; b < kHistBins; ++b) maxBin = std::max(maxBin, hist[b]);
    std::lock_guard<std::mutex> lk(gScopeMutex);
    for (int b = 0; b < kHistBins; ++b) gHist[b] = static_cast<float>(hist[b]) / maxBin;
    for (int col = 0; col < kWaveCols; ++col)
        for (int b = 0; b < kWaveBins; ++b)
            gWave[col * kWaveBins + b] = colN[col] ? static_cast<float>(wave[col * kWaveBins + b]) / colN[col] : 0.0f;
    gScopesValid = true;
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
    if (gExposureRamping) return; // mid-transition frames would mislead the next AE pass
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
struct FocusSample { double dt = 0; };
std::mutex gFocusMutex;
std::condition_variable gFocusCv;
FocusController gFocus;
bool gFocusHasSample = false;
FocusSample gFocusSample;
bool gFocusLocked = false;         // lens held (AF-L)
ExposureRamp gExpRamp;             // smooth auto-exposure transition (gFocusMutex)
int64_t gExpRampFinalNs = 0;       // what to store in settings when it ends
int32_t gExpRampFinalIso = 0;
bool gExpRampFixed = false;        // final exposure is a fixed time (not the shutter angle)
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
        if (gExpRamp.active()) {
            double t, iso;
            bool more = gExpRamp.update(s.dt, t, iso);
            int64_t finalNs = gExpRampFinalNs;
            int32_t finalIso = gExpRampFinalIso;
            bool fixed = gExpRampFixed;
            lk.unlock();
            if (more) {
                if (gCamera) gCamera->setExposure(static_cast<int64_t>(t), static_cast<int32_t>(std::lround(iso)));
            } else {
                {
                    std::lock_guard<std::mutex> sl(gStateMutex);
                    gSettings.iso = finalIso;
                    if (fixed) gSettings.fixedExposureNs = finalNs;
                    gMeter.valid = false; // meter again only at the settled exposure
                }
                if (gCamera) applyShutter();
                gExposureRamping = false;
            }
            lk.lock();
        }
        if (!gFocus.active()) continue;
        double cmd = gFocus.update(s.dt);
        lk.unlock();
        if (std::fabs(cmd - lastCmd) > 1e-4 && gCamera) gCamera->setFocusDistance(static_cast<float>(pToDiopter(cmd)));
        lastCmd = cmd;
        lk.lock();
    }
}

void feedFocus(double frameSeconds) {
    bool active;
    {
        std::lock_guard<std::mutex> lk(gFocusMutex);
        active = gFocus.active() || gExpRamp.active();
    }
    if (!active) return;
    FocusSample s;
    s.dt = frameSeconds;
    {
        std::lock_guard<std::mutex> lk(gFocusMutex);
        gFocusSample = s;
        gFocusHasSample = true;
    }
    gFocusCv.notify_one();
}

void cancelExposureRamp() {
    std::lock_guard<std::mutex> lk(gFocusMutex);
    gExpRamp.cancel();
    gExposureRamping = false;
}

void ensureFocusThread() {
    if (gFocusThread.joinable()) return;
    gFocusQuit = false;
    gFocusThread = std::thread(focusThreadMain);
}

// --- Native ISO analysis ----------------------------------------------------
// Dark-frame ISO sweep (lens covered): at each ISO, the dark noise of the green
// pixels in a central patch; iso_analysis.cpp turns that into base / HCG ISOs.
struct SweepState {
    std::mutex mutex;
    std::condition_variable cv;
    bool collecting = false;
    int skip = 0;                       // frames to ignore after an ISO change (pipeline latency)
    int64_t exposureNs = 0;
    std::vector<IsoSample> frames;      // this step's measurements
    std::string resultPath, error, outPath, device;
    bool cancel = false;
};
SweepState gSweep;
std::atomic<float> gSweepProgress{-1.0f}; // -1 idle, 0..1 running
std::thread gSweepThread;

// Mean and (outlier-trimmed) std of the green pixels in a central 512x512 raw
// patch, as a fraction of the black..white range.
bool darkStats(const RawFrame& f, IsoSample& out) {
    const CaptureMetadata& m = *f.meta;
    const int cfa = gCamera->sensorInfo().cfa;
    const int half = std::min({256, f.width / 4, f.height / 4}) & ~3;
    const int x0 = (f.width / 2 - half) & ~3, y0 = (f.height / 2 - half) & ~1;
    auto pass = [&](double centre, double limit, double& mean, double& sd) {
        double sum = 0, sq = 0;
        long n = 0;
        for (int y = y0; y < y0 + 2 * half; ++y) {
            const uint8_t* row = f.data + static_cast<size_t>(y) * f.rowStride;
            for (int x = x0; x < x0 + 2 * half; x += 4) {
                const uint8_t* g = row + (x / 4) * 5;
                if (g + 5 > f.data + f.size) return;
                for (int i = 0; i < 4; ++i) {
                    int site = cfaSite(cfa, x + i, y);
                    if (site != 1 && site != 2) continue;
                    int dn = (g[i] << 2) | ((g[4] >> (2 * i)) & 3);
                    double v = (dn - m.blackLevel[site]) / (m.whiteLevel - m.blackLevel[site]);
                    if (limit > 0 && std::fabs(v - centre) > limit) continue; // hot pixels
                    sum += v;
                    sq += v * v;
                    ++n;
                }
            }
        }
        if (n < 100) { mean = 0; sd = 0; return; }
        mean = sum / n;
        sd = std::sqrt(std::max(0.0, sq / n - mean * mean));
    };
    double mean, sd;
    pass(0, 0, mean, sd);
    if (!(sd > 0)) return false;
    pass(mean, 5 * sd, mean, sd);
    if (!(sd > 0)) return false;
    out.iso = m.iso;
    out.mean = mean;
    out.sigma = sd;
    return true;
}

void feedSweep(const RawFrame& f) {
    std::lock_guard<std::mutex> lk(gSweep.mutex);
    if (!gSweep.collecting) return;
    if (gSweep.skip > 0) { --gSweep.skip; return; }
    if (std::llabs(f.meta->exposureNs - gSweep.exposureNs) > gSweep.exposureNs / 20) return;
    IsoSample s;
    if (darkStats(f, s)) gSweep.frames.push_back(s);
    if (gSweep.frames.size() >= 4) {
        gSweep.collecting = false;
        gSweep.cv.notify_all();
    }
}

void applyShutter();

void isoSweepMain() {
    const SensorInfo info = gCamera->sensorInfo();
    const double frameNs = 1e9 / gCamera->frameRate();
    const int64_t exposureNs = static_cast<int64_t>(std::min(20e6, frameNs * 0.9));
    std::vector<int> isos;
    for (double iso = info.minIso; iso < info.maxIso * 0.98; iso *= std::cbrt(2.0)) isos.push_back(static_cast<int>(std::lround(iso)));
    isos.push_back(info.maxIso);
    std::vector<IsoSample> samples;
    std::string error;
    for (size_t i = 0; i < isos.size(); ++i) {
        {
            std::lock_guard<std::mutex> lk(gSweep.mutex);
            if (gSweep.cancel) { error = "cancelled"; break; }
            gSweep.frames.clear();
            gSweep.exposureNs = exposureNs;
            gSweep.skip = 5;
            gSweep.collecting = true;
        }
        gCamera->setExposure(exposureNs, isos[i]);
        std::unique_lock<std::mutex> lk(gSweep.mutex);
        gSweep.cv.wait_for(lk, std::chrono::seconds(3), [] { return !gSweep.collecting || gSweep.cancel; });
        gSweep.collecting = false;
        if (gSweep.frames.size() >= 2) {
            // Median frame by sigma; the ISO the camera actually applied.
            auto fr = gSweep.frames;
            std::sort(fr.begin(), fr.end(), [](const IsoSample& a, const IsoSample& b) { return a.sigma < b.sigma; });
            IsoSample med = fr[fr.size() / 2];
            if (samples.empty() || med.iso > samples.back().iso) samples.push_back(med);
        }
        lk.unlock();
        gSweepProgress = static_cast<float>(i + 1) / isos.size();
    }
    applyShutter(); // back to the user's exposure

    IsoAnalysis a;
    if (error.empty()) {
        a = analyzeIsoSweep(samples, info.maxAnalogIso);
        if (!a.valid) error = a.error;
    }
    std::string path;
    {
        std::lock_guard<std::mutex> lk(gSweep.mutex);
        path = gSweep.outPath;
    }
    if (error.empty()) {
        std::string j = "{\"format\":\"vesper-iso-analysis/1\",\"device\":\"" + jsonEscape(gSweep.device) + "\",\"cameraId\":\"" +
                        jsonEscape(gCamera->cameraId()) + "\",";
        char b[256];
        std::snprintf(b, sizeof(b), "\"minIso\":%d,\"maxIso\":%d,\"maxAnalogIso\":%d,\"exposureNs\":%lld,\"baseIso\":%d,\"hcgIso\":%d,\"digitalFromIso\":%d,",
                      info.minIso, info.maxIso, info.maxAnalogIso, static_cast<long long>(exposureNs), a.baseIso, a.hcgIso, a.digitalFromIso);
        j += b;
        j += "\"nativeIsos\":[";
        for (size_t i = 0; i < a.nativeIsos.size(); ++i) j += (i ? "," : "") + std::to_string(a.nativeIsos[i]);
        j += "],\"steps\":[";
        for (size_t i = 0; i < samples.size(); ++i) {
            std::snprintf(b, sizeof(b), "%s{\"iso\":%d,\"sigma\":%.6g,\"mean\":%.6g,\"inputNoise\":%.4f}", i ? "," : "", samples[i].iso,
                          samples[i].sigma, samples[i].mean, a.inputNoise[i]);
            j += b;
        }
        j += "]}";
        FILE* fp = std::fopen(path.c_str(), "w");
        if (!fp) {
            error = "could not write result";
        } else {
            std::fputs(j.c_str(), fp);
            std::fclose(fp);
        }
        vesperLog(ANDROID_LOG_INFO, "Vesper", "ISO analysis: base %d, HCG %d, digital from %d (%zu steps)", a.baseIso, a.hcgIso,
                            a.digitalFromIso, samples.size());
    }
    for (const auto& x : samples)
        vesperLog(ANDROID_LOG_INFO, "Vesper", "  ISO %5d dark sigma %.6f mean %.5f", x.iso, x.sigma, x.mean);
    std::lock_guard<std::mutex> lk(gSweep.mutex);
    gSweep.error = error;
    gSweep.resultPath = error.empty() ? path : "";
    gSweepProgress = -1.0f;
}

// Settings page open (not recording): skip all per-frame work but keep the
// camera streaming, so returning is instant and the UI gets the GPU/CPU.
std::atomic<bool> gProcessingPaused{false};

void onFrame(const RawFrame& f) {
    if (!gGpu) return;
    if (gProcessingPaused && !(gRecorder && gRecorder->isRecording())) {
        // Paused (Settings open): no frames are expected, so restart the drop
        // and viewfinder-pacing measurements instead of counting the pause.
        gLastTimestampNs = 0;
        gGpu->resetPacing();
        return;
    }
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
    {
        // Sensor-side drops (frames the camera never delivered), per ~5 s.
        static int64_t lastDrops = 0;
        static int logTick = 0;
        if (++logTick % 120 == 0) {
            int64_t d = gCameraDrops;
            if (d < lastDrops) lastDrops = 0; // counter was reset (Settings opened)
            LOGI("Camera: %.2f fps measured, %lld sensor drops in last 120 frames", gMeasuredFps,
                 static_cast<long long>(d - lastDrops));
            lastDrops = d;
        }
    }
    feedFocus(frameNs * 1e-9);
    if (gSweepProgress >= 0) feedSweep(f);
    if ((++frameIndex & 3) == 0) meterCentre(f);
    if ((frameIndex & 3) == 2) meterFrame(f);
    if ((frameIndex & 3) == 3 && gScopesEnabled) computeScopes(f);

    Settings s;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        s = gSettings;
        gRawW = f.width;
        gRawH = f.height;
    }
    int outW, outH;
    outputSize(s, outW, outH);
    // Freeze the rotation for the length of a recording.
    const bool recNow = gRecorder && gRecorder->isRecording();
    if (!recNow) gRecordingRotation = -1;
    else if (gRecordingRotation < 0) gRecordingRotation = rotationDegrees();
    int rot = recNow ? gRecordingRotation : rotationDegrees();

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

    p.flags[3] = s.sharpening | (s.oversampling ? 16 : 0);
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
    vesperLog(ANDROID_LOG_INFO, "Vesper", "vesper_init: %d", ok);
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
    vesperLog(ANDROID_LOG_INFO, "Vesper", "Stream mode %dx%d (max %.1f fps) for crop %d @ %.3f fps: %d",
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

// Stops a running ISO sweep and waits for it (it drives the camera).
void stopIsoSweep() {
    {
        std::lock_guard<std::mutex> lk(gSweep.mutex);
        gSweep.cancel = true;
        gSweep.cv.notify_all();
    }
    if (gSweepThread.joinable()) gSweepThread.join();
}

EXPORT void vesper_close_camera() {
    stopIsoSweep();
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
    cancelExposureRamp(); // a manual setting wins over a running AE glide
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
    cancelExposureRamp(); // a manual setting wins over a running AE glide
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
    std::snprintf(buf, sizeof(buf), "{\"minExposureNs\":%lld,\"maxExposureNs\":%lld,\"minIso\":%d,\"maxIso\":%d,\"maxAnalogIso\":%d,\"minFocus\":%.3f,\"modes\":[",
                  static_cast<long long>(s.minExposureNs), static_cast<long long>(s.maxExposureNs), s.minIso, s.maxIso, s.maxAnalogIso, s.minFocusDiopters);
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
// than 0.5% of the frame — highlights win. priority 2 = cleanest (native ISO, shutter to 180 deg, then gain); 0 = keep the shutter
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
    // Never into digital gain: it adds no information to the raw, just clips sooner.
    const int32_t isoCap = std::min(info.maxIso, info.maxAnalogIso > 0 ? info.maxAnalogIso : 3200);
    double t = static_cast<double>(m.exposureNs), iso = m.iso;
    const double target = t * iso * scale;
    auto clampT = [&](double v) { return std::clamp(v, static_cast<double>(info.minExposureNs), std::min(frameNs, static_cast<double>(info.maxExposureNs))); };
    auto clampIso = [&](double v) { return std::clamp(v, static_cast<double>(info.minIso), static_cast<double>(isoCap)); };
    if (priority == 2) {
        // Cleanest image: native ISO first, shutter up to 180 degrees, then gain.
        int base, hcg;
        {
            std::lock_guard<std::mutex> lk(gStateMutex);
            base = gNativeBaseIso;
            hcg = gNativeHcgIso;
        }
        const double maxT = std::min(frameNs * 0.5, static_cast<double>(info.maxExposureNs));
        ExposureChoice c = solveCleanExposure(target, static_cast<double>(info.minExposureNs), maxT, info.minIso, isoCap, base, hcg);
        t = c.exposureNs;
        iso = c.iso;
    } else if (priority == 0) {
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
        gMeter.valid = false; // wait for a frame at the new exposure before metering again
    }
    // Refinement passes that land within ~1/6 stop leave the exposure alone
    // (no visible nudges) and report the exposure actually in use.
    const double stops = std::log2(static_cast<double>(ns) * isoI / (static_cast<double>(m.exposureNs) * m.iso));
    if (std::fabs(stops) < 0.17) {
        if (outExposureNs) *outExposureNs = m.exposureNs;
        if (outIso) *outIso = m.iso;
        return 0;
    }
    // Glide from the current exposure to the new one (see ExposureRamp).
    ensureFocusThread();
    {
        std::lock_guard<std::mutex> lk(gFocusMutex);
        gExpRampFinalNs = ns;
        gExpRampFinalIso = isoI;
        gExpRampFixed = priority != 0 || std::llabs(ns - m.exposureNs) > m.exposureNs / 50;
        gExpRamp.start(static_cast<double>(m.exposureNs), m.iso, static_cast<double>(ns), isoI);
        gExposureRamping = true;
    }
    if (outExposureNs) *outExposureNs = ns;
    if (outIso) *outIso = isoI;
    vesperLog(ANDROID_LOG_INFO, "Vesper", "Auto-expose (%s): meanG=%.4f p99.5=%.3f x%.3f -> %.3fms ISO %d",
                        m.spot ? "spot" : (m.faceValid ? "face" : "centre"),
                        std::exp(m.faceValid ? m.faceLogMeanG : m.logMeanG), m.p995, scale, ns / 1e6, isoI);
    return 0;
}

EXPORT void vesper_set_kelvin_tint(int32_t kelvin, int32_t tint) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.awbAuto = false;
    setColorLocked(gColor.fromKelvinTint(kelvin, tint));
}

// Native ISO analysis: dark-frame sweep over the ISO range (lens covered, ~15 s).
// Writes the result JSON to outPath; progress / result / error via status.
EXPORT int32_t vesper_iso_sweep_start(const char* outPath, const char* deviceModel) {
    if (!gCamera || !outPath || gSweepProgress >= 0) return -1;
    if (gSweepThread.joinable()) gSweepThread.join();
    cancelExposureRamp();
    {
        std::lock_guard<std::mutex> lk(gSweep.mutex);
        gSweep.outPath = outPath;
        gSweep.device = deviceModel ? deviceModel : "";
        gSweep.error.clear();
        gSweep.resultPath.clear();
        gSweep.cancel = false;
    }
    gSweepProgress = 0.0f;
    gSweepThread = std::thread(isoSweepMain);
    return 0;
}

EXPORT void vesper_iso_sweep_cancel() {
    std::lock_guard<std::mutex> lk(gSweep.mutex);
    gSweep.cancel = true;
    gSweep.cv.notify_all();
}

// Pause / resume per-frame processing (viewfinder freezes; recording is never paused).
EXPORT void vesper_set_processing_paused(int32_t paused) {
    // Opening Settings starts the DROP counter afresh (never mid-recording).
    if (paused && !(gRecorder && gRecorder->isRecording())) gCameraDrops = 0;
    gProcessingPaused = paused != 0;
}

// Measured native ISOs for the clean auto-exposure (0 = unknown).
EXPORT void vesper_set_native_isos(int32_t baseIso, int32_t hcgIso) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gNativeBaseIso = std::max(0, baseIso);
    gNativeHcgIso = std::max(0, hcgIso);
}

// false: never auto-pause alignment / HQ / NR when the GPU is over budget.
EXPORT void vesper_set_budget_guard(int32_t enable) {
    if (gGpu) gGpu->setBudgetGuard(enable != 0);
}

// GPU benchmark A/B (developer tool): 0 = current shaders, 1 = without the
// latest optimisation step, 2 = without the last two (see VulkanEngine).
EXPORT void vesper_set_previous_shaders(int32_t level) {
    if (gGpu) gGpu->setPreviousShaders(level);
}

// HQ oversampling: luma rebuilt from the full-resolution sensor and
// area-downsampled to the output (sharper, less moire); off = quad-only path.
EXPORT void vesper_set_oversampling(int32_t enable) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.oversampling = enable != 0;
}

// Detail enhancement: 0 off, 1 low, 2 medium, 3 high (noise-aware unsharp mask,
// applied to recording and viewfinder).
// In-app log (developer "App log" screen). Writes the buffered lines, oldest
// first, newline-separated; returns the length, or -(needed bytes) if maxLen
// is too small.
EXPORT int32_t vesper_get_log(char* out, int32_t maxLen) {
    auto& log = vesper::appLog();
    std::string text;
    {
        std::lock_guard<std::mutex> lk(log.mutex);
        if (log.dropped) text += "... " + std::to_string(log.dropped) + " older lines dropped\n";
        for (const auto& l : log.lines) { text += l; text += '\n'; }
    }
    if (!out || text.size() + 1 > static_cast<size_t>(maxLen)) return -static_cast<int32_t>(text.size() + 1);
    std::memcpy(out, text.c_str(), text.size() + 1);
    return static_cast<int32_t>(text.size());
}

EXPORT void vesper_clear_log() {
    auto& log = vesper::appLog();
    std::lock_guard<std::mutex> lk(log.mutex);
    log.lines.clear();
    log.dropped = 0;
}

// Lets the Dart side add its own lines (benchmark results, UI events).
EXPORT void vesper_log_line(const char* tag, const char* message) {
    vesperLog(ANDROID_LOG_INFO, tag ? tag : "Vesper_UI", "%s", message ? message : "");
}

EXPORT void vesper_set_viewfinder_zoom(float cx, float cy, float scale) {
    if (gGpu) gGpu->setViewfinderZoom(cx, cy, scale);
}

EXPORT void vesper_set_sharpening(int32_t level) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gSettings.sharpening = std::clamp(level, 0, 3);
}

// Histogram / waveform overlay: computed only while enabled (~6 Hz at 24 fps).
EXPORT void vesper_set_scopes(int32_t enable) {
    gScopesEnabled = enable != 0;
    if (!enable) {
        std::lock_guard<std::mutex> lk(gScopeMutex);
        gScopesValid = false;
    }
}

// Copies the latest scopes: hist[64] (0..1 of the tallest bin) and
// wave[128 columns x 64 bins] (column-major, fraction of that column's
// samples; bin 0 = code value 0). Returns 0, or -1 if none yet.
EXPORT int32_t vesper_get_scopes(float* hist, float* wave) {
    std::lock_guard<std::mutex> lk(gScopeMutex);
    if (!gScopesValid) return -1;
    if (hist) std::copy(gHist, gHist + kHistBins, hist);
    if (wave) std::copy(gWave, gWave + kWaveCols * kWaveBins, wave);
    return 0;
}

// Eyedropper: sample white balance at an output-normalised point from the next
// frames. Poll vesper_lock_white_balance until it stops returning -1.
EXPORT void vesper_pick_white_balance(float ox, float oy) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    gWbPoint[0] = std::clamp(ox, 0.0f, 1.0f);
    gWbPoint[1] = std::clamp(oy, 0.0f, 1.0f);
    gHaveCentreSample = false;
}

// One-shot white balance off the raw eyedropper patch (centre by default). Writes the equivalent
// Kelvin/tint back so the UI dial can follow. Returns 0 on success.
EXPORT int32_t vesper_lock_white_balance(double* outKelvin, double* outTint) {
    std::lock_guard<std::mutex> lk(gStateMutex);
    if (!gHaveCentreSample) return -1;
    Vec3 n = gCentreRaw;
    if (n[0] < 0.005f || n[1] < 0.005f || n[2] < 0.005f) return -2; // too dark to trust
    ColorState c = gColor.fromCameraNeutral(n);
    gSettings.awbAuto = false;
    setColorLocked(c);
    if (outKelvin) *outKelvin = c.kelvin;
    if (outTint) *outTint = c.tint;
    vesperLog(ANDROID_LOG_INFO, "Vesper", "WB lock: raw neutral %.4f %.4f %.4f -> %.0fK tint %.1f, gains R%.3f B%.3f",
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
        gFocusLocked = mode != 1;
    }
    if (gCamera) gCamera->setFocusMode(mode == 1 ? FocusMode::Continuous : FocusMode::Manual);
}

// Tap-to-focus at an output-normalised point (0..1, upright viewfinder), using
// the HAL's PDAF + laser AF. lock = 0: track the region (CONTINUOUS_PICTURE,
// scan restarted now); lock = 1: one PDAF scan, then the HAL holds focus.
EXPORT void vesper_focus_at(float ox, float oy, int32_t lock) {
    if (!gCamera) return;
    {
        std::lock_guard<std::mutex> lk(gFocusMutex);
        gFocus.cancel();
        gFocusLocked = lock != 0;
    }
    FrameGeometry g;
    bool lensCorrection;
    {
        std::lock_guard<std::mutex> lk(gStateMutex);
        g = gGeom;
        lensCorrection = gSettings.lensCorrection;
    }
    const SensorInfo& info = gCamera->sensorInfo();
    float u, v;
    outputToCrop(g.rot, std::clamp(ox, 0.0f, 1.0f), std::clamp(oy, 0.0f, 1.0f), u, v);
    float rx = g.crop[0] + u * g.crop[2], ry = g.crop[1] + v * g.crop[3];
    // The viewfinder is lens-corrected: find the raw pixel the tapped point came from.
    if (lensCorrection && info.hasDistortion) distortRaw(info, g.sensorMap, rx, ry);
    float ax = rx * g.sensorMap[0] + g.sensorMap[2];
    float ay = ry * g.sensorMap[1] + g.sensorMap[3];
    float nx = ax / std::max(1, info.preWidth), ny = ay / std::max(1, info.preHeight);
    // ~8% of the frame width, square on the sensor.
    const float hw = 0.04f, hh = 0.04f * info.preWidth / std::max(1, info.preHeight);
    gCamera->setFocusRegion(nx - hw, ny - hh, 2 * hw, 2 * hh);
    gCamera->setFocusMode(lock ? FocusMode::Single : FocusMode::Continuous);
    gCamera->triggerAutofocus();
}

EXPORT void vesper_set_focus_point(float ox, float oy) { vesper_focus_at(ox, oy, 0); }

EXPORT void vesper_clear_focus_point() {
    if (gCamera) gCamera->setFocusRegion(0, 0, 0, 0);
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
    gFocusLocked = false;
    gFocus.pullTo(current, diopterToP(diopters));
}

// Seconds for a full-range focus move (cinema pulls ~1-2 s).
EXPORT void vesper_set_focus_speed(float fullRangeSeconds) {
    std::lock_guard<std::mutex> lk(gFocusMutex);
    gFocus.setSpeed(fullRangeSeconds);
}

// Stops any smooth pull and holds the lens where it is; used after hardware
// AF converges to lock it ("AF-L").
EXPORT void vesper_focus_lock() {
    {
        std::lock_guard<std::mutex> lk(gFocusMutex);
        gFocus.cancel();
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
    std::string calSaved, sweepResult, sweepError;
    {
        std::lock_guard<std::mutex> lk(gSweep.mutex);
        sweepResult = gSweep.resultPath;
        sweepError = gSweep.error;
    }
    bool focusSearching, focusLocked;
    {
        std::lock_guard<std::mutex> lk(gFocusMutex);
        focusSearching = gFocus.active();
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
                  "\"face\":[%.4f,%.4f,%.4f,%.4f],\"gpuMs\":%.2f,\"alignThrottled\":%s,\"nrThrottled\":%s,\"hqAvailable\":%s,\"hqSupported\":%s,\"calibrationSaved\":\"%s\",\"profileActive\":%s,"
                  "\"focusPulling\":%s,\"exposureRamping\":%s,\"isoSweep\":%.3f,\"isoSweepResult\":\"%s\",\"isoSweepError\":\"%s\",\"focusLocked\":%s,\"gpuOverloaded\":%s}",
                  gCamera && gCamera->isStreaming() ? "true" : "false", fps, rw, rh, ow, oh, drops, kelvin, tint,
                  r.recording ? "true" : "false", static_cast<long long>(r.durationUs / 1000),
                  static_cast<long long>(r.framesEncoded), static_cast<long long>(r.framesDropped), r.thermalStatus,
                  r.audio ? "true" : "false", jsonEscape(r.codecName).c_str(), jsonEscape(r.stopReason).c_str(), expNs, iso,
                  awbAuto ? "true" : "false", afState, focusD, face[0], face[1], face[2], face[3],
                  gGpu ? gGpu->gpuFrameMs() : 0.0, gGpu && gGpu->alignmentThrottled() ? "true" : "false",
                  gGpu && gGpu->noiseReductionThrottled() ? "true" : "false", gGpu && gGpu->oversamplingAvailable() ? "true" : "false", gGpu && gGpu->oversamplingSupported() ? "true" : "false", jsonEscape(calSaved).c_str(),
                  profileActive ? "true" : "false", focusSearching ? "true" : "false", gExposureRamping ? "true" : "false", static_cast<double>(gSweepProgress.load()), jsonEscape(sweepResult).c_str(), jsonEscape(sweepError).c_str(), focusLocked ? "true" : "false", gGpu && gGpu->overloaded() ? "true" : "false");
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
    stopIsoSweep();
    if (gRecorder) gRecorder->stop("user");
    if (gCamera) gCamera->closeCamera();
    if (gGpu) gGpu->release();
}

// Display rotation in degrees (90 or 270: the two landscape orientations).
JNIEXPORT void JNICALL Java_com_vesper_cine_MainActivity_nativeSetDisplayRotation(JNIEnv*, jobject, jint degrees) {
    if (degrees == 90 || degrees == 270) gDisplayRotation = degrees;
}

JNIEXPORT jint JNICALL Java_com_vesper_cine_MainActivity_nativeSetViewfinderSurface(JNIEnv* env, jobject, jobject surface) {
    ensureInit();
    ANativeWindow* win = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (gGpu) gGpu->setViewfinderWindow(win); // takes its own reference
    if (win) ANativeWindow_release(win);
    return 0;
}

} // extern "C"
