#include "camera_engine.h"
#include "vulkan_compute.h"

#include <jni.h>
#include <android/native_window_jni.h>
#include <string>
#include <sstream>
#include <cmath>
#include <algorithm>

using namespace rcamera;

static std::unique_ptr<CameraEngine> gCameraEngine;
static std::unique_ptr<VulkanComputeEngine> gVulkanCompute;
static ComputeUniformData gUniforms;

// Inverse of the shader's linearToRLog() (mhc_rlog.comp) — must be kept
// byte-for-byte in sync with those constants. Needed because the viewfinder
// buffer we sample for tap-to-white-balance is log-encoded (it's literally
// what's on screen), but white balance gains are applied in the LINEAR
// domain, upstream of the log curve. Computing a correction ratio directly
// on log-space samples and feeding it into a linear multiplier is wrong —
// the log curve's flat black-floor offset makes near-black log samples look
// like a huge color imbalance that isn't really there, so the "correction"
// mostly just amplifies sensor read noise. Delogging first fixes that.
static float rLogToLinear(float logVal) {
    const float a = 0.225f;
    const float b = 5.5555f;
    const float c = 0.385f;
    const float d = 0.100f;
    const float logAtCutoff = c * 0.01f + d; // logVal at the curve's x=0.01 breakpoint
    if (logVal < logAtCutoff) {
        return std::max(0.0f, (logVal - d) / c);
    }
    return (std::exp((logVal - c) / a) - 1.0f) / b;
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

// The sensor's own calibrated "camera neutral" gains — what raw R/G/B ratios
// THIS specific device's sensor reports for a neutral gray scene under its
// calibration reference illuminant — computed once from real calibration
// data when the camera opens (see rcamera_open_camera). Used to anchor the
// Kelvin/tint dial to reality instead of the display-color approximation
// kelvinTintToRgbGains() (above) was never designed to substitute for.
static float gCalibratedRGain = 1.0f;
static float gCalibratedGGain = 1.0f;
static float gCalibratedBGain = 1.0f;

// ACAMERA_SENSOR_COLOR_TRANSFORM1 maps CIE XYZ -> camera-native RGB under the
// sensor's reference illuminant. Per the DNG spec, this is exactly the
// documented way to compute "camera neutral" (the raw RGB a neutral gray
// scene produces under that illuminant) when no direct AWB/AsShotNeutral
// estimate is available: cameraNeutral = ColorTransform1 * XYZ_white.
static void computeCalibratedNeutralGains(const float colorTransform1[9], float& rGain, float& gGain, float& bGain) {
    // CIE D50 white point — matches the target of ACAMERA_SENSOR_FORWARD_MATRIX1.
    const float xw = 0.9642f, yw = 1.0000f, zw = 0.8249f;
    float r = colorTransform1[0] * xw + colorTransform1[1] * yw + colorTransform1[2] * zw;
    float g = colorTransform1[3] * xw + colorTransform1[4] * yw + colorTransform1[5] * zw;
    float b = colorTransform1[6] * xw + colorTransform1[7] * yw + colorTransform1[8] * zw;

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
        PackMat3ForPushConstant(meta.forwardMatrix1, gUniforms.sensorToXyzMatrix);
        computeCalibratedNeutralGains(meta.colorTransform1, gCalibratedRGain, gCalibratedGGain, gCalibratedBGain);
        LOGI("Calibrated neutral WB gains from ColorTransform1: R=%.3f G=%.3f B=%.3f",
             gCalibratedRGain, gCalibratedGGain, gCalibratedBGain);
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
    // kelvinTintToRgbGains() approximates a blackbody radiator's DISPLAY color
    // (Tanner Helland algorithm) — it has no relationship to this sensor's
    // actual raw-domain R/G/B response and, used directly as gains, pushes
    // color the wrong way (e.g. it suppresses red at daylight temperatures,
    // when real sensors need red boosted well above green). Used only as a
    // *relative* shift here: the dial moves gains away from the sensor's own
    // calibrated neutral (computeCalibratedNeutralGains, real per-device DNG
    // calibration data) by the same ratio the approximation would move them
    // away from its 5600K/tint-0 reference point, so the default dial
    // position reflects real calibration and only user adjustment relies on
    // the approximation.
    float rAtKelvin = 1.0f, gAtKelvin = 1.0f, bAtKelvin = 1.0f;
    kelvinTintToRgbGains(kelvin, tint, rAtKelvin, gAtKelvin, bAtKelvin);
    float rAtRef = 1.0f, gAtRef = 1.0f, bAtRef = 1.0f;
    kelvinTintToRgbGains(5600, 0, rAtRef, gAtRef, bAtRef);

    float r = gCalibratedRGain * (rAtKelvin / rAtRef);
    float b = gCalibratedBGain * (bAtKelvin / bAtRef);
    rcamera_set_white_balance_gains(r, gCalibratedGGain, b);
}

// Samples the current viewfinder's center patch and adjusts the white
// balance gains so that patch renders neutral — i.e. "point the camera at
// something you know is gray/white and press this." Multiplicative on top
// of whatever gains are currently active (calibrated base * Kelvin/tint
// dial), so it corrects for the *actual* scene lighting rather than
// requiring the Kelvin dial to guess it. Returns 0 on success, -1 if no
// frame has been presented yet to sample from.
EXPORT int32_t rcamera_lock_white_balance_from_center() {
    if (!gVulkanCompute) return -1;

    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (!gVulkanCompute->sampleViewfinderCenterPatch(r, g, b)) {
        return -1;
    }
    if (r < 0.02f || g < 0.02f || b < 0.02f) {
        LOGW("rcamera_lock_white_balance_from_center: sampled patch too dark to use (R=%.3f G=%.3f B=%.3f)", r, g, b);
        return -1;
    }

    // The sampled RGB is read off the on-screen viewfinder buffer, which is
    // R-Log encoded (see mhc_rlog.comp). White balance gains are multiplied
    // in the LINEAR domain, upstream of that curve, so the correction ratio
    // has to be computed in linear space too — otherwise the log curve's
    // black-floor offset makes ordinary shadow noise look like a massive
    // color cast. Delog each channel first.
    float linR = rLogToLinear(r);
    float linG = rLogToLinear(g);
    float linB = rLogToLinear(b);
    if (linR < 0.01f || linG < 0.01f || linB < 0.01f) {
        LOGW("rcamera_lock_white_balance_from_center: linearized patch too dark/noisy to use (linR=%.4f linG=%.4f linB=%.4f)", linR, linG, linB);
        return -1;
    }

    float gray = (linR + linG + linB) / 3.0f;
    float newR = gUniforms.wbGains[0] * (gray / linR);
    float newG = gUniforms.wbGains[1] * (gray / linG);
    float newB = gUniforms.wbGains[2] * (gray / linB);
    // Keep the convention (used everywhere else) that green stays the
    // unity-gain reference channel.
    if (newG > 1e-6f) {
        newR /= newG;
        newB /= newG;
        newG = 1.0f;
    }
    // Clamp to a physically sane range. A single 48x48 patch is still small
    // enough that read noise or a slightly non-neutral surface can produce
    // an outlier ratio; real AWB implementations cap gain swings for the
    // same reason. +/-4 stops of correction is generous for any real light
    // source and keeps a bad sample from tanking the image.
    newR = std::clamp(newR, 0.25f, 4.0f);
    newB = std::clamp(newB, 0.25f, 4.0f);

    LOGI("White balance locked from center patch: sampled(log) R=%.3f G=%.3f B=%.3f -> linear R=%.3f G=%.3f B=%.3f -> gains R=%.3f G=%.3f B=%.3f",
         r, g, b, linR, linG, linB, newR, newG, newB);
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
