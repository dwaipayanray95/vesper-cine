#include "camera_engine.h"
#include "vulkan_compute.h"

#include <jni.h>
#include <android/native_window_jni.h>
#include <string>
#include <sstream>
#include <cmath>

using namespace rcamera;

static std::unique_ptr<CameraEngine> gCameraEngine;
static std::unique_ptr<VulkanComputeEngine> gVulkanCompute;
static ComputeUniformData gUniforms;

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

    // Default Rec.2020 matrix and standard D65 matrices
    // CIE XYZ -> Rec.2020 Linear matrix
    const float kXyzToRec2020[9] = {
         1.7166511880f, -0.3556707838f, -0.2533662814f,
        -0.6666843518f,  1.6164812366f,  0.0157685458f,
         0.0176398574f, -0.0427706133f,  0.9421031212f
    };
    PackMat3ForPushConstant(kXyzToRec2020, gUniforms.xyzToRec2020Matrix);

    // Default WB gains (5600K Daylight)
    gUniforms.wbGains[0] = 1.8f;
    gUniforms.wbGains[1] = 1.0f;
    gUniforms.wbGains[2] = 1.9f;
    gUniforms.wbGains[3] = 0.0f;

    gUniforms.cropMode = 0;          // Default 16:9 crop
    gUniforms.monitoringMode = 1;    // Default Rec.709 preview LUT
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
        gUniforms.outputWidth = 3840;
        gUniforms.outputHeight = 2160;

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

EXPORT void rcamera_set_ois(int32_t enable) {
    if (gCameraEngine) {
        gCameraEngine->setOpticalStabilization(enable != 0);
    }
}

EXPORT void rcamera_set_crop_mode(int32_t cropMode) {
    // 0: 16:9 UHD crop, 1: 4:3 open gate
    gUniforms.cropMode = cropMode;
    if (cropMode == 1) {
        gUniforms.outputWidth = 3840;
        gUniforms.outputHeight = 2880;
    } else {
        gUniforms.outputWidth = 3840;
        gUniforms.outputHeight = 2160;
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
