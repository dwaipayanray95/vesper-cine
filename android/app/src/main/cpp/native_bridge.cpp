#include "camera_engine.h"
#include "vulkan_compute.h"

#include <jni.h>
#include <android/native_window_jni.h>
#include <string>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <limits>

using namespace rcamera;

static std::unique_ptr<CameraEngine> gCameraEngine;
static std::unique_ptr<VulkanComputeEngine> gVulkanCompute;
static ComputeUniformData gUniforms;

// Most recent raw-sensor-domain center-patch average, refreshed every frame
// by sampleRawCenterPatch() below. Read by rcamera_lock_white_balance_from_center().
// Benign torn reads across the 3 floats/bool are the same tolerated race as
// gUniforms elsewhere in this file (self-correcting, at most one stale frame).
static float gRawCenterR = 0.0f, gRawCenterG = 0.0f, gRawCenterB = 0.0f;
static bool gHasRawCenterSample = false;

// Mirrors CfaSiteAt() in camera_engine.cpp / cfaSiteAt() in mhc_rlog.comp:
// resolves which logical CFA channel (0=R, 1=Gr, 2=Gb, 3=B) physically sits
// at raw pixel phase (x&1, y&1), given the sensor's actual CFA arrangement.
static inline int cfaSiteAt(int32_t cfaPattern, int x, int y) {
    int bit = ((y & 1) << 1) | (x & 1);
    switch (cfaPattern) {
        case 0: { static const int m[4] = {0, 1, 2, 3}; return m[bit]; } // RGGB
        case 1: { static const int m[4] = {1, 0, 3, 2}; return m[bit]; } // GRBG
        case 2: { static const int m[4] = {2, 3, 0, 1}; return m[bit]; } // GBRG
        default:{ static const int m[4] = {3, 2, 1, 0}; return m[bit]; } // BGGR
    }
}

// Reads and linearizes one RAW10 pixel straight off the packed sensor
// bytes — mirrors fetchBayer() in mhc_rlog.comp exactly (same MIPI RAW10
// unpack, same black-level subtraction), so this is the identical value the
// GPU demosaic sees, before any WB gain, color matrix, or tone curve.
static inline float fetchRawPixel(const uint8_t* data, size_t dataLength, int32_t rowStrideBytes,
                                   int32_t x, int32_t y, const float blackLevel[4], float whiteLevel,
                                   int32_t cfaPattern) {
    int32_t groupIndex = x >> 2;
    int32_t pixelInGroup = x & 3;
    size_t groupByteOffset = static_cast<size_t>(y) * static_cast<size_t>(rowStrideBytes) +
                              static_cast<size_t>(groupIndex) * 5;
    if (groupByteOffset + 4 >= dataLength) return 0.0f;
    uint32_t msb = data[groupByteOffset + pixelInGroup];
    uint32_t lsbByte = data[groupByteOffset + 4];
    uint32_t lsb = (lsbByte >> (pixelInGroup * 2)) & 0x3u;
    float raw = static_cast<float>((msb << 2) | lsb);
    float bl = blackLevel[cfaSiteAt(cfaPattern, x, y)];
    return std::max(0.0f, (raw - bl) / (whiteLevel - bl));
}

// Averages a small window of RAW10 sensor data around the center of the
// active array, per logical CFA channel (Gr+Gb pooled as one green count).
// This is deliberately the same thing a real camera's 3A AWB stats block
// reads: raw Bayer data, before demosaic, before any color matrix, before
// any tone curve. A gray-world ratio computed here is a plain diagonal
// gain that can be fed straight into wbGains (applied pre-matrix in the
// shader) with no inversion math needed — unlike sampling anywhere
// downstream of the color matrix, where cross-channel mixing means a
// post-matrix ratio doesn't invert cleanly through a simple per-channel
// scale. Runs on the frame-callback thread; cost is a fixed ~128x128-pixel
// scan, negligible next to the GPU debayer of the full frame.
static void sampleRawCenterPatch(const uint8_t* data, size_t dataLength, int32_t rowStrideBytes,
                                  int32_t rawWidth, int32_t rawHeight, const float blackLevel[4],
                                  float whiteLevel, int32_t cfaPattern) {
    if (rawWidth <= 0 || rawHeight <= 0 || rowStrideBytes <= 0) return;
    constexpr int32_t kHalfPatch = 64; // ~128x128 raw px around center
    int32_t cx = (rawWidth / 2) & ~1;
    int32_t cy = (rawHeight / 2) & ~1;
    int32_t x0 = std::max(0, cx - kHalfPatch);
    int32_t x1 = std::min(rawWidth, cx + kHalfPatch);
    int32_t y0 = std::max(0, cy - kHalfPatch);
    int32_t y1 = std::min(rawHeight, cy + kHalfPatch);

    double sumR = 0.0, sumG = 0.0, sumB = 0.0;
    int64_t countR = 0, countG = 0, countB = 0;
    for (int32_t y = y0; y < y1; ++y) {
        for (int32_t x = x0; x < x1; ++x) {
            float v = fetchRawPixel(data, dataLength, rowStrideBytes, x, y, blackLevel, whiteLevel, cfaPattern);
            int site = cfaSiteAt(cfaPattern, x, y);
            if (site == 0) { sumR += v; ++countR; }
            else if (site == 3) { sumB += v; ++countB; }
            else { sumG += v; ++countG; }
        }
    }
    if (countR == 0 || countG == 0 || countB == 0) return;

    gRawCenterR = static_cast<float>(sumR / static_cast<double>(countR));
    gRawCenterG = static_cast<float>(sumG / static_cast<double>(countG));
    gRawCenterB = static_cast<float>(sumB / static_cast<double>(countB));
    gHasRawCenterSample = true;
}

// --- Dual-illuminant forward-matrix interpolation --------------------------
//
// DNG-calibrated sensors ship TWO reference-illuminant color matrices
// (typically ~2856K tungsten and ~6504K daylight) precisely because no
// single matrix renders color correctly across the full range of real-world
// light spectra. Using only ForwardMatrix1 unconditionally (the previous
// behavior here) measurably mis-renders color under in-between or
// narrow-spectrum lighting (LED, fluorescent) — a green/yellow cast is a
// classic symptom, since many LED/fluorescent sources have a sharp green
// emission spike neither a daylight- nor tungsten-tuned matrix alone
// corrects for. This mirrors what the DNG SDK / libraw / AOSP's own raw
// post-processing do: interpolate linearly between both matrices in
// "mired" (1e6/Kelvin) space, by the scene's actual/assumed CCT.
static float gForwardMatrix1[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static float gForwardMatrix2[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static float gColorTransform1[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static float gColorTransform2[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static float gIlluminant1Kelvin = 2856.0f;
static float gIlluminant2Kelvin = 6504.0f;
static bool gHaveForwardMatrix2 = false;

// TIFF/EXIF LightSource enum (also used by DNG's CalibrationIlluminant1/2)
// -> approximate correlated color temperature. Covers the values real
// sensor calibrations use in practice; anything unlisted falls back to a
// neutral 5500K daylight guess.
static float referenceIlluminantToKelvin(int32_t code) {
    switch (code) {
        case 1:  return 5500.0f;  // Daylight
        case 2:  return 4200.0f;  // Fluorescent
        case 3:  return 3200.0f;  // Tungsten (incandescent)
        case 4:  return 5500.0f;  // Flash
        case 9:  return 5500.0f;  // Fine weather
        case 10: return 6500.0f;  // Cloudy weather
        case 11: return 7500.0f;  // Shade
        case 12: return 6500.0f;  // Daylight fluorescent
        case 13: return 5000.0f;  // Day white fluorescent
        case 14: return 4200.0f;  // Cool white fluorescent
        case 15: return 3450.0f;  // White fluorescent
        case 17: return 2856.0f;  // Standard light A
        case 18: return 4874.0f;  // Standard light B
        case 19: return 6774.0f;  // Standard light C
        case 20: return 5503.0f;  // D55
        case 21: return 6504.0f;  // D65
        case 22: return 7504.0f;  // D75
        case 23: return 5003.0f;  // D50
        case 24: return 3200.0f;  // ISO studio tungsten
        default: return 5500.0f;
    }
}

static void lerpMatrix9(const float a[9], const float b[9], float t, float out[9]) {
    for (int i = 0; i < 9; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}

// Row-major 3x3 * 3-vector (same convention as PackMat3ForPushConstant's src9).
static void matVec3(const float m[9], const float v[3], float out[3]) {
    out[0] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2];
    out[1] = m[3]*v[0] + m[4]*v[1] + m[5]*v[2];
    out[2] = m[6]*v[0] + m[7]*v[1] + m[8]*v[2];
}

// CIE Planckian-locus chromaticity at temperature T (Kelvin), via Krystek's
// polynomial approximation (matches CIE standard tables to a close fit
// across ~1000-15000K, the range any real light source's CCT falls in).
static void planckianLocusXY(float kelvin, float& x, float& y) {
    float T = kelvin;
    float T2 = T * T;
    float u = (0.860117757f + 1.54118254e-4f*T + 1.28641212e-7f*T2) /
              (1.0f + 8.42420235e-4f*T + 7.08145163e-7f*T2);
    float v = (0.317398726f + 4.22806245e-5f*T + 4.20481691e-8f*T2) /
              (1.0f - 2.89741816e-5f*T + 1.61456053e-7f*T2);
    float denom = 2.0f*u - 8.0f*v + 4.0f;
    x = 3.0f*u / denom;
    y = 2.0f*v / denom;
}

// Estimates the scene's correlated color temperature from a raw-domain
// neutral sample (camera-native RGB of something known to be gray/white —
// the same thing DNG calls "AsShotNeutral"). Searches for the mired value
// whose interpolated ColorTransform (XYZ->camera at that CCT) maps the
// Planckian-locus white point at that same CCT closest to the observed
// neutral's chromaticity ratios — i.e. the CCT at which the calibration
// data best explains what the sensor is actually seeing. Conceptually the
// same fixed-point idea DNG converters use for illuminant estimation,
// simplified to a direct grid search since we only need "close enough to
// pick the right matrix blend", not exact CCT to the Kelvin.
static float estimateSceneKelvin(const float neutralRgb[3]) {
    if (!gHaveForwardMatrix2) return 5600.0f;

    float navg = (neutralRgb[0] + neutralRgb[1] + neutralRgb[2]) / 3.0f;
    if (navg < 1e-6f) return 5600.0f;
    float nr = neutralRgb[0] / navg;
    float nb = neutralRgb[2] / navg;

    float mired1 = 1.0e6f / gIlluminant1Kelvin;
    float mired2 = 1.0e6f / gIlluminant2Kelvin;
    float miredLo = std::min(mired1, mired2);
    float miredHi = std::max(mired1, mired2);

    float bestMired = (mired1 + mired2) * 0.5f;
    float bestErr = std::numeric_limits<float>::max();

    constexpr int kSteps = 32;
    for (int i = 0; i <= kSteps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(kSteps);
        float mired = miredLo + (miredHi - miredLo) * t;
        float kelvin = 1.0e6f / mired;
        float g = std::clamp((mired - mired1) / (mired2 - mired1), 0.0f, 1.0f);
        float ct[9];
        lerpMatrix9(gColorTransform1, gColorTransform2, g, ct);

        float lx, ly;
        planckianLocusXY(kelvin, lx, ly);
        float locusXyz[3] = { lx / ly, 1.0f, (1.0f - lx - ly) / ly };
        float predicted[3];
        matVec3(ct, locusXyz, predicted);
        float pavg = (predicted[0] + predicted[1] + predicted[2]) / 3.0f;
        if (pavg < 1e-6f) continue;

        float err = std::fabs(predicted[0]/pavg - nr) + std::fabs(predicted[2]/pavg - nb);
        if (err < bestErr) {
            bestErr = err;
            bestMired = mired;
        }
    }
    return 1.0e6f / bestMired;
}

// Builds the mired-interpolated ForwardMatrix for a given target Kelvin and
// writes it into gUniforms (packed for the shader push-constant). No-op if
// the device's calibration only provides a single illuminant's matrix.
static void applyForwardMatrixForKelvin(float kelvin) {
    if (!gHaveForwardMatrix2) return;
    float mired1 = 1.0e6f / gIlluminant1Kelvin;
    float mired2 = 1.0e6f / gIlluminant2Kelvin;
    float miredTarget = 1.0e6f / kelvin;
    float g = std::clamp((miredTarget - mired1) / (mired2 - mired1), 0.0f, 1.0f);
    float interpolated[9];
    lerpMatrix9(gForwardMatrix1, gForwardMatrix2, g, interpolated);
    PackMat3ForPushConstant(interpolated, gUniforms.sensorToXyzMatrix);
}

// Converts Kelvin (2000K - 10000K) and Tint (-50 to +50) to RGB gains
static void kelvinTintToRgbGains(int kelvin, int tint, float& rGain, float& gGain, float& bGain) {
    float temp = static_cast<float>(kelvin) / 100.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    // Approximate Planckian locus calculation (Tanner Helland algorithm)
    if (temp <= 66.0f) {
        r = 255.0f;
        g = 99.4708025861f * std::log(temp) - 161.1195681661f;
        if (temp <= 19.0f) {
            b = 0.0f;
        } else {
            b = 138.5177312231f * std::log(temp - 10.0f) - 305.0447927307f;
        }
    } else {
        r = 329.698727446f * std::pow(temp - 60.0f, -0.1332047592f);
        g = 288.1221695283f * std::pow(temp - 60.0f, -0.0755148492f);
        b = 255.0f;
    }

    r = std::clamp(r, 0.0f, 255.0f);
    g = std::clamp(g, 0.0f, 255.0f);
    b = std::clamp(b, 0.0f, 255.0f);

    // Apply green-magenta tint (-50 = green, +50 = magenta)
    float tintFactor = 1.0f + (static_cast<float>(tint) / 100.0f);
    g /= tintFactor;

    // Normalize gains relative to green = 1.0
    rGain = (g > 0.0f) ? (g / r) : 1.0f;
    gGain = 1.0f;
    bGain = (g > 0.0f) ? (g / b) : 1.0f;
}

// ACAMERA_SENSOR_COLOR_TRANSFORM{1,2} map CIE XYZ -> camera-native RGB under
// each reference illuminant. Per the DNG spec, this is exactly the
// documented way to compute "camera neutral" (the raw RGB a neutral gray
// scene produces under that illuminant) when no direct AWB/AsShotNeutral
// estimate is available: cameraNeutral = ColorTransform * XYZ_white.
//
// Takes an already CCT-interpolated ColorTransform (see
// applyForwardMatrixForKelvin, which interpolates the matching ForwardMatrix
// the same way) rather than always ColorTransform1 — using a fixed
// single-illuminant baseline here while the render matrix itself is
// interpolated by Kelvin would reintroduce the same single-illuminant
// mismatch at any dial setting away from illuminant1, just in the WB-gain
// baseline instead of the color matrix.
static void computeCalibratedNeutralGains(const float colorTransform[9], float& rGain, float& gGain, float& bGain) {
    // CIE D50 white point — matches the target of ACAMERA_SENSOR_FORWARD_MATRIX{1,2}.
    const float xw = 0.9642f, yw = 1.0000f, zw = 0.8249f;
    float r = colorTransform[0] * xw + colorTransform[1] * yw + colorTransform[2] * zw;
    float g = colorTransform[3] * xw + colorTransform[4] * yw + colorTransform[5] * zw;
    float b = colorTransform[6] * xw + colorTransform[7] * yw + colorTransform[8] * zw;

    if (r > 1e-6f && g > 1e-6f && b > 1e-6f) {
        rGain = g / r;
        gGain = 1.0f;
        bGain = g / b;
    } else {
        rGain = 1.0f;
        gGain = 1.0f;
        bGain = 1.0f;
    }
}

// Interpolates ColorTransform1/2 for the given Kelvin the same way
// applyForwardMatrixForKelvin interpolates ForwardMatrix1/2, then derives
// the calibrated-neutral WB gain baseline from it. Falls back to
// ColorTransform1 unchanged if only one illuminant's calibration is
// available (mirrors applyForwardMatrixForKelvin's own fallback).
static void computeCalibratedNeutralGainsForKelvin(float kelvin, float& rGain, float& gGain, float& bGain) {
    if (!gHaveForwardMatrix2) {
        computeCalibratedNeutralGains(gColorTransform1, rGain, gGain, bGain);
        return;
    }
    float mired1 = 1.0e6f / gIlluminant1Kelvin;
    float mired2 = 1.0e6f / gIlluminant2Kelvin;
    float miredTarget = 1.0e6f / kelvin;
    float g = std::clamp((miredTarget - mired1) / (mired2 - mired1), 0.0f, 1.0f);
    float interpolated[9];
    lerpMatrix9(gColorTransform1, gColorTransform2, g, interpolated);
    computeCalibratedNeutralGains(interpolated, rGain, gGain, bGain);
}

extern "C" {

#define EXPORT __attribute__((visibility("default")))

EXPORT int32_t rcamera_init() {
    if (!gCameraEngine) {
        gCameraEngine = std::make_unique<CameraEngine>();
    }
    if (!gVulkanCompute) {
        gVulkanCompute = std::make_unique<VulkanComputeEngine>();
    }

    // CIE XYZ (D50) -> Linear Rec.2020 matrix. ACAMERA_SENSOR_FORWARD_MATRIX1
    // (used for sensorToXyzMatrix, see rcamera_open_camera) is documented by
    // the DNG spec to output XYZ referenced to the D50 white point — but a
    // standard "XYZ -> Rec.2020" matrix normally assumes D65 input (Rec.2020's
    // own white point). Feeding D50-referenced XYZ through a D65-assuming
    // matrix with no chromatic adaptation between them is a real, measured bug
    // (mathematically verified: a perfectly neutral D50 white point run
    // through the plain D65 matrix comes out as [1.09, 0.99, 0.75] — red
    // boosted, blue crushed by 25% — a textbook color-temperature shift, and
    // exactly the "everything looks yellow/warm" symptom this was causing).
    // This constant is the plain D65 matrix pre-multiplied by a standard
    // Bradford D50->D65 chromatic adaptation matrix, combining both steps
    // into one 3x3 (same neutral D50 white point now maps to [1.0, 1.0, 1.0]).
    const float kXyzD50ToRec2020[9] = {
         1.6473375051f, -0.3935674606f, -0.2359962568f,
        -0.6826034874f,  1.6475886001f,  0.0128191127f,
         0.0296523668f, -0.0628993298f,  1.2531278582f
    };
    PackMat3ForPushConstant(kXyzD50ToRec2020, gUniforms.xyzToRec2020Matrix);

    // Default WB gains (5600K Daylight)
    gUniforms.wbGains[0] = 1.8f;
    gUniforms.wbGains[1] = 1.0f;
    gUniforms.wbGains[2] = 1.9f;
    gUniforms.wbGains[3] = 0.0f;

    gUniforms.cropMode = 0;          // Default 16:9 crop
    gUniforms.monitoringMode = 0;    // Default flat R-Log
    gUniforms.zebraThreshold = 0.95f;
    gUniforms.peakingThreshold = 0.15f;

    bool ok = gCameraEngine->initialize();
    if (ok) {
        // Option B: the GPU compute pipeline owns the viewfinder surface directly;
        // Camera2 never touches it (see CameraEngine::startCaptureSession).
        ok = gVulkanCompute->initialize(/*codecWindow=*/nullptr, /*viewfinderWindow=*/nullptr);
    }
    LOGI("rcamera_init status: %d", ok);
    return ok ? 0 : -1;
}

EXPORT int32_t rcamera_enumerate_cameras(char* outJson, int32_t maxLen) {
    if (!gCameraEngine) return -1;
    auto devices = gCameraEngine->enumerateCameras();

    std::stringstream ss;
    ss << "[";
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& d = devices[i];
        ss << "{"
           << "\"id\":\"" << d.cameraId << "\","
           << "\"hwLevel\":" << d.hardwareLevel << ","
           << "\"supportsRaw10\":" << (d.supportsRaw10 ? "true" : "false") << ","
           << "\"rawWidth\":" << d.rawWidth << ","
           << "\"rawHeight\":" << d.rawHeight
           << "}";
        if (i + 1 < devices.size()) ss << ",";
    }
    ss << "]";

    std::string str = ss.str();
    if (str.length() >= static_cast<size_t>(maxLen)) return -2;

    std::strncpy(outJson, str.c_str(), maxLen);
    return static_cast<int32_t>(str.length());
}

EXPORT int32_t rcamera_open_camera(const char* cameraId) {
    if (!gCameraEngine) return -1;
    // A stale sample from a previous camera/session has a different sensor's
    // black level, CFA arrangement, and calibration matrices baked in — using
    // it for a WB lock before this camera has produced its own first frame
    // would silently apply nonsense gains instead of the "no sample yet"
    // error rcamera_lock_white_balance_from_center() is meant to give.
    gHasRawCenterSample = false;
    bool ok = gCameraEngine->openCamera(cameraId);
    if (ok) {
        const auto& meta = gCameraEngine->getCalibrationMetadata();
        gUniforms.whiteLevel = static_cast<float>(meta.whiteLevel);
        std::memcpy(gUniforms.blackLevel, meta.blackLevel, sizeof(meta.blackLevel));
        // ACAMERA_SENSOR_FORWARD_MATRIX1 maps white-balanced sensor RGB -> CIE XYZ
        // (D50) — the direction this pipeline needs (xyz = matrix * wbRgb below).
        // ACAMERA_SENSOR_COLOR_TRANSFORM1 (meta.colorTransform1, previously used
        // here by mistake) is the *inverse* direction (XYZ -> camera-native), used
        // for AWB estimation, not rendering; applying it forward instead of
        // ForwardMatrix1 produced a strong wrong color cast.
        std::memcpy(gForwardMatrix1, meta.forwardMatrix1, sizeof(gForwardMatrix1));
        std::memcpy(gForwardMatrix2, meta.forwardMatrix2, sizeof(gForwardMatrix2));
        std::memcpy(gColorTransform1, meta.colorTransform1, sizeof(gColorTransform1));
        std::memcpy(gColorTransform2, meta.colorTransform2, sizeof(gColorTransform2));
        gIlluminant1Kelvin = referenceIlluminantToKelvin(meta.referenceIlluminant1);
        gIlluminant2Kelvin = referenceIlluminantToKelvin(meta.referenceIlluminant2);
        // Guard against both illuminant codes mapping to the same (or a near-
        // identical) Kelvin value — the mired interpolation below divides by
        // (mired2 - mired1), which would be a near-zero denominator and NaN
        // the color matrix every frame. Treat that degenerate case the same
        // as "no second matrix": fall back to ForwardMatrix1 unconditionally.
        gHaveForwardMatrix2 = meta.haveForwardMatrix2 &&
                              std::fabs(gIlluminant1Kelvin - gIlluminant2Kelvin) > 50.0f;
        if (gHaveForwardMatrix2) {
            // Start at the same 5600K reference the Kelvin dial defaults to.
            applyForwardMatrixForKelvin(5600.0f);
            LOGI("Dual-illuminant forward matrix available (illuminant1=%.0fK, illuminant2=%.0fK) — interpolating by scene CCT",
                 gIlluminant1Kelvin, gIlluminant2Kelvin);
        } else {
            PackMat3ForPushConstant(meta.forwardMatrix1, gUniforms.sensorToXyzMatrix);
            LOGW("Only ForwardMatrix1 available on this device — color matrix is fixed regardless of scene lighting");
        }
        {
            float r0, g0, b0;
            computeCalibratedNeutralGainsForKelvin(5600.0f, r0, g0, b0);
            LOGI("Calibrated neutral WB gains at 5600K reference: R=%.3f G=%.3f B=%.3f", r0, g0, b0);
        }
        gUniforms.cfaPattern = meta.cfaPattern;
        gUniforms.rawWidth = meta.activeArrayWidth;
        gUniforms.rawHeight = meta.activeArrayHeight;
        // 1920x1080 rather than full 3840x2160: measured on-device, the compute
        // shader alone was taking ~92ms/frame at 4K on this hardware (a naive
        // per-pixel RAW10 SSBO unpack with no texture-cache benefit, over 8.3M
        // pixels), capping the viewfinder at ~7fps. Quartering the pixel count
        // directly targets that cost. Nothing consumes the full-res codec output
        // yet (AMediaCodec recording is a future phase — see README), so there's
        // no current downside; revisit once recording lands and/or the shader
        // is optimized (e.g. workgroup shared-memory tiling for the 5x5 kernel).
        gUniforms.outputWidth = 1920;
        gUniforms.outputHeight = 1080;

        // The app is locked to a single fixed landscape hold (see camera_screen.dart),
        // which corresponds to a display rotation of 90 degrees. The total rotation the
        // compute shader must apply to the raw buffer to land upright is therefore the
        // sensor's physical mounting angle minus that fixed display rotation.
        constexpr int32_t kFixedDisplayRotationDeg = 90;
        gUniforms.sensorOrientation = ((meta.sensorOrientation - kFixedDisplayRotationDeg) % 360 + 360) % 360;

        if (gVulkanCompute) {
            gVulkanCompute->setOutputDimensions(gUniforms.outputWidth, gUniforms.outputHeight);
        }
    }
    return ok ? 0 : -1;
}

EXPORT int32_t rcamera_start_stream(int32_t width, int32_t height) {
    if (!gCameraEngine) return -1;
    gUniforms.rawWidth = width;
    gUniforms.rawHeight = height;

    // Real per-frame black level, replacing the flat [64,64,64,64] struct
    // default — see BlackLevelCallback's doc comment for why the static
    // calibration tags don't work on this hardware. Runs on the capture
    // session's own callback thread, writing 4 independent floats gUniforms
    // is also read from on the image-reader thread without a lock; a torn
    // read of one stale/fresh value for a single frame is a self-correcting,
    // visually inconsequential race, consistent with how gUniforms is
    // already shared elsewhere in this file.
    gCameraEngine->setBlackLevelCallback([](const float blackLevel[4]) {
        gUniforms.blackLevel[0] = blackLevel[0];
        gUniforms.blackLevel[1] = blackLevel[1];
        gUniforms.blackLevel[2] = blackLevel[2];
        gUniforms.blackLevel[3] = blackLevel[3];
    });

    bool ok = gCameraEngine->startCaptureSession(width, height,
        [](const uint8_t* data, size_t dataLength, int32_t rowStrideBytes, int64_t timestampNs) {
            if (gVulkanCompute) {
                gUniforms.rawRowStrideBytes = rowStrideBytes;
                sampleRawCenterPatch(data, dataLength, rowStrideBytes, gUniforms.rawWidth, gUniforms.rawHeight,
                                     gUniforms.blackLevel, gUniforms.whiteLevel, gUniforms.cfaPattern);
                gVulkanCompute->processRawFrame(data, dataLength, gUniforms);
            }
        });

    return ok ? 0 : -1;
}

EXPORT int32_t rcamera_stop_stream() {
    if (!gCameraEngine) return -1;
    gCameraEngine->stopCaptureSession();
    return 0;
}

EXPORT void rcamera_set_exposure(int64_t exposureNs, int32_t iso) {
    if (gCameraEngine) {
        gCameraEngine->setExposure(exposureNs, iso);
    }
}

EXPORT void rcamera_set_shutter_angle(float shutterAngle, float fps, int32_t iso) {
    if (gCameraEngine) {
        gCameraEngine->setShutterAngle(shutterAngle, fps, iso);
    }
}

EXPORT void rcamera_set_white_balance_gains(float rGain, float gGain, float bGain) {
    gUniforms.wbGains[0] = rGain;
    gUniforms.wbGains[1] = gGain;
    gUniforms.wbGains[2] = bGain;
    if (gCameraEngine) {
        gCameraEngine->setWhitebalanceGains(rGain, gGain, bGain);
    }
}

EXPORT void rcamera_set_kelvin_tint(int32_t kelvin, int32_t tint) {
    // Gain baseline comes from computeCalibratedNeutralGainsForKelvin(), i.e.
    // real per-device DNG calibration data (ColorTransform1/2) interpolated
    // to the requested Kelvin — the same interpolation applyForwardMatrixForKelvin
    // below uses for the render matrix, so gains and matrix always agree on
    // which illuminant they're assuming. (An earlier version anchored gains
    // to a single ColorTransform1-derived baseline and shifted it by a
    // Kelvin-only display-color approximation; that meant the gain baseline
    // and the CCT-interpolated matrix silently disagreed at any Kelvin away
    // from illuminant1.)
    float rGain, gGain, bGain;
    computeCalibratedNeutralGainsForKelvin(static_cast<float>(kelvin), rGain, gGain, bGain);

    // kelvinTintToRgbGains() approximates a blackbody radiator's DISPLAY color
    // (Tanner Helland algorithm) — no relationship to this sensor's actual
    // raw-domain response, so it's never used as an absolute gain. Used only
    // to isolate the TINT (green/magenta) axis: comparing it against itself
    // at the same Kelvin with tint=0 cancels out its absolute-value
    // inaccuracy while keeping its relative tint behavior.
    float rTint = 1.0f, gTint = 1.0f, bTint = 1.0f;
    kelvinTintToRgbGains(kelvin, tint, rTint, gTint, bTint);
    float rNoTint = 1.0f, gNoTint = 1.0f, bNoTint = 1.0f;
    kelvinTintToRgbGains(kelvin, 0, rNoTint, gNoTint, bNoTint);
    if (rNoTint > 1e-6f) rGain *= (rTint / rNoTint);
    if (bNoTint > 1e-6f) bGain *= (bTint / bNoTint);

    rcamera_set_white_balance_gains(rGain, gGain, bGain);

    // Also pick the color matrix appropriate for this Kelvin setting (see
    // applyForwardMatrixForKelvin's doc comment) — a fixed single-illuminant
    // matrix mis-renders color once the dial moves away from that
    // illuminant's own calibration temperature.
    applyForwardMatrixForKelvin(static_cast<float>(kelvin));
}

// Sets the white balance gains from the most recent raw-sensor-domain
// center-patch average (see sampleRawCenterPatch, refreshed every frame) —
// i.e. "point the camera at something you know is gray/white and press
// this." The raw patch is sampled before any WB gain, color matrix, or tone
// curve, so the gray-world ratio computed here (gray/R, 1, gray/B) is
// already the correct absolute per-channel gain to feed the shader's
// pre-matrix multiply — it replaces the current wbGains outright rather
// than compounding onto them.
//
// An earlier version of this sampled the on-screen viewfinder buffer
// instead (post color-matrix, post R-Log curve) and tried to back out a
// correction from it. That doesn't work: the color matrix mixes channels
// (its off-diagonal terms are non-trivial), so a ratio measured after it
// doesn't invert cleanly through a simple per-channel scale applied before
// it — in practice this produced wildly different, overcorrected gains
// between calibrations and visibly amplified noise. Real camera AWB always
// meters off raw sensor data for exactly this reason.
//
// Returns 0 on success, -1 if no frame has been sampled yet or the patch
// is too dark/noisy to trust.
EXPORT int32_t rcamera_lock_white_balance_from_center() {
    if (!gHasRawCenterSample) {
        LOGW("rcamera_lock_white_balance_from_center: no raw frame sampled yet");
        return -1;
    }

    float r = gRawCenterR, g = gRawCenterG, b = gRawCenterB;
    if (r < 0.01f || g < 0.01f || b < 0.01f) {
        LOGW("rcamera_lock_white_balance_from_center: sampled patch too dark/noisy to use (R=%.4f G=%.4f B=%.4f)", r, g, b);
        return -1;
    }

    float gray = (r + g + b) / 3.0f;
    float newR = gray / r;
    float newG = 1.0f;
    float newB = gray / b;
    // Clamp to a physically sane range. A single center patch can still be
    // thrown off by read noise or a slightly non-neutral surface; real AWB
    // implementations cap gain swings for the same reason. +/-4 stops is
    // generous for any real light source and keeps a bad sample from
    // tanking the image.
    newR = std::clamp(newR, 0.25f, 4.0f);
    newB = std::clamp(newB, 0.25f, 4.0f);

    // Also re-pick the color matrix to match the scene's actual color
    // temperature (see applyForwardMatrixForKelvin/estimateSceneKelvin) —
    // the WB gains alone only neutralize the sampled patch; under
    // in-between or narrow-spectrum lighting (LED/fluorescent) a matrix
    // still fixed to a single calibration illuminant leaves a visible
    // cast (classically green/yellow) elsewhere in the frame.
    float neutral[3] = { r, g, b };
    float estimatedKelvin = estimateSceneKelvin(neutral);
    applyForwardMatrixForKelvin(estimatedKelvin);

    LOGI("White balance locked from raw center patch: sampled(raw linear) R=%.4f G=%.4f B=%.4f -> gains R=%.3f G=%.3f B=%.3f, estimated scene CCT=%.0fK",
         r, g, b, newR, newG, newB, estimatedKelvin);
    rcamera_set_white_balance_gains(newR, newG, newB);
    return 0;
}

EXPORT void rcamera_set_ois(int32_t enable) {
    if (gCameraEngine) {
        gCameraEngine->setOpticalStabilization(enable != 0);
    }
}

EXPORT void rcamera_set_crop_mode(int32_t cropMode) {
    // 0: 16:9 UHD crop, 1: 4:3 open gate
    gUniforms.cropMode = cropMode;
    // See the matching comment in rcamera_open_camera() for why these are
    // 1920-wide rather than full 3840-wide right now.
    if (cropMode == 1) {
        gUniforms.outputWidth = 1920;
        gUniforms.outputHeight = 1440;
    } else {
        gUniforms.outputWidth = 1920;
        gUniforms.outputHeight = 1080;
    }

    if (gVulkanCompute) {
        gVulkanCompute->setOutputDimensions(gUniforms.outputWidth, gUniforms.outputHeight);
    }
}

EXPORT void rcamera_set_monitoring_mode(int32_t mode) {
    // 0: R-Log flat, 1: Rec.709 preview LUT, 2: False Color, 3: Focus Peaking, 4: Zebras
    gUniforms.monitoringMode = mode;
}

EXPORT void rcamera_set_zebra_threshold(float threshold) {
    gUniforms.zebraThreshold = threshold;
}

EXPORT void rcamera_close() {
    if (gCameraEngine) {
        gCameraEngine->closeCamera();
    }
    if (gVulkanCompute) {
        gVulkanCompute->release();
    }
}

JNIEXPORT jint JNICALL
Java_com_rawedge_r_1camera_MainActivity_nativeSetViewfinderSurface(
    JNIEnv* env, jobject /*thiz*/, jobject surface) {
    if (!gCameraEngine) {
        rcamera_init();
    }
    // Option B: the Flutter viewfinder surface is fed by the GPU compute
    // pipeline directly, never by Camera2 (see CameraEngine::startCaptureSession).
    ANativeWindow* win = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (gVulkanCompute) {
        gVulkanCompute->setViewfinderWindow(win);
    } else if (win) {
        ANativeWindow_release(win);
    }
    return 0;
}

} // extern "C"
