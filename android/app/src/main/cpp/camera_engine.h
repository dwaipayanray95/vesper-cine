#pragma once

#include "color_science.h"

#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraMetadata.h>
#include <media/NdkImageReader.h>
#include <media/NdkImage.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/log.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define CAM_TAG "Vesper_Camera"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, CAM_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, CAM_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, CAM_TAG, __VA_ARGS__)

namespace vesper {

// Logical CFA channel (0=R, 1=Gr, 2=Gb, 3=B) at raw parity (px, py) for
// ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT 0=RGGB 1=GRBG 2=GBRG 3=BGGR.
// Mirrors cfaSite() in shaders/unpack.comp.
inline int cfaSite(int32_t cfa, int px, int py) {
    int bit = ((py & 1) << 1) | (px & 1);
    switch (cfa) {
        case 0: return bit;
        case 1: return bit ^ 1;
        case 2: return bit ^ 2;
        default: return 3 - bit;
    }
}

struct RawMode {
    int32_t width = 0, height = 0;
    int64_t minFrameDurationNs = 0;
    double maxFps() const { return minFrameDurationNs > 0 ? 1e9 / static_cast<double>(minFrameDurationNs) : 30.0; }
};

// Static per-camera facts (CameraCharacteristics).
struct SensorInfo {
    DngCalibration calibration;
    int32_t whiteLevel = 1023;
    float blackLevel[4] = {64, 64, 64, 64}; // logical R, Gr, Gb, B
    int32_t cfa = 0;
    int32_t orientation = 90;
    int32_t activeWidth = 0, activeHeight = 0;
    // Pre-correction active array: the coordinate system of RAW streams,
    // the lens shading map, lens distortion and AF/face rectangles.
    int32_t preLeft = 0, preTop = 0, preWidth = 0, preHeight = 0;
    // ACAMERA_LENS_DISTORTION (k1, k2, k3, p1, p2) and
    // ACAMERA_LENS_INTRINSIC_CALIBRATION (fx, fy, cx, cy, s), pre-correction pixels.
    bool hasDistortion = false;
    float distortion[5] = {0, 0, 0, 0, 0};
    float intrinsics[5] = {0, 0, 0, 0, 0};
    int32_t maxAfRegions = 0;
    int32_t maxFaces = 0;
    int32_t shadingCols = 0, shadingRows = 0;
    bool lensShadingApplied = false;   // raw already shading-corrected by the HAL
    bool timestampRealtime = false;    // SENSOR_INFO_TIMESTAMP_SOURCE == REALTIME (CLOCK_BOOTTIME)
    int64_t minExposureNs = 1000, maxExposureNs = 1'000'000'000;
    int32_t minIso = 50, maxIso = 3200;
    float minFocusDiopters = 0.0f;     // 0 = fixed focus
    std::vector<RawMode> rawModes;     // RAW10 output sizes, largest first
};

// Per-frame facts (CaptureResult), matched to the image by sensor timestamp.
struct CaptureMetadata {
    int64_t timestampNs = 0;
    float blackLevel[4] = {64, 64, 64, 64}; // logical R, Gr, Gb, B
    float whiteLevel = 1023.0f;
    std::vector<float> shadingMap;          // rows x cols x [R, Geven, Godd, B]
    int32_t shadingCols = 0, shadingRows = 0;
    Vec3 neutral{0, 0, 0};                  // SENSOR_NEUTRAL_COLOR_POINT (0 if absent)
    int64_t exposureNs = 0;
    int32_t iso = 0;
    float focusDiopters = 0;                // LENS_FOCUS_DISTANCE actually used
    uint8_t afState = 0;                    // CONTROL_AF_STATE
    float noiseS = 0, noiseO = 0;           // SENSOR_NOISE_PROFILE, averaged over channels
    int32_t face[4] = {0, 0, 0, 0};         // largest face (l, t, r, b), pre-correction px; r == 0 if none
};

enum class FocusMode { Manual = 0, Continuous = 1 };

struct RawFrame {
    const uint8_t* data = nullptr;
    size_t size = 0;
    int32_t rowStride = 0;
    int32_t width = 0, height = 0;
    int64_t timestampNs = 0;
    const CaptureMetadata* meta = nullptr; // never null during the callback
};

using FrameCallback = std::function<void(const RawFrame&)>;

struct CameraDeviceInfo {
    std::string id;
    int32_t facing = 0;
    int32_t hardwareLevel = 0;
    bool supportsRaw10 = false;
    RawMode largestRaw;
};

class CameraEngine {
public:
    CameraEngine() = default;
    ~CameraEngine();

    bool initialize();
    std::vector<CameraDeviceInfo> enumerateCameras();
    bool openCamera(const std::string& id);
    void closeCamera();
    const SensorInfo& sensorInfo() const { return info_; }

    // `callback` runs on the image reader's thread, one frame at a time.
    bool startCapture(int32_t width, int32_t height, FrameCallback callback);
    void stopCapture();
    bool isStreaming() const { return streaming_.load(); }

    // Manual sensor controls. Exposure is clamped to the frame duration.
    void setFrameRate(double fps);
    void setExposure(int64_t exposureNs, int32_t iso);
    void setOpticalStabilization(bool enable);
    void setFocusDistance(float diopters);            // also switches to manual focus
    void setFocusMode(FocusMode mode);
    // AF metering region, normalised to the pre-correction array (0..1); w <= 0 clears it.
    void setFocusRegion(float x, float y, float w, float h);
    void setAutoWhiteBalance(bool enable);            // HAL AWB; RAW is unaffected, we read its neutral point
    void setFaceDetection(bool enable);
    double frameRate() const { return fps_; }
    int32_t streamWidth() const { return streamW_; }
    int32_t streamHeight() const { return streamH_; }

    // NDK callback trampolines
    void onDeviceError(int error);
    void onImageAvailable(AImageReader* reader);
    void onCaptureCompleted(const ACameraMetadata* result);

private:
    void querySensorInfo(const ACameraMetadata* chars);
    bool buildRequestLocked();
    void applyControlsLocked();
    void submitLocked();
    void stopCaptureLocked();
    void scheduleRecovery();

    ACameraManager* manager_ = nullptr;
    ACameraDevice* device_ = nullptr;
    ACameraCaptureSession* session_ = nullptr;
    ACaptureSessionOutputContainer* outputs_ = nullptr;
    ACaptureSessionOutput* output_ = nullptr;
    ACameraOutputTarget* target_ = nullptr;
    ACaptureRequest* request_ = nullptr;
    AImageReader* reader_ = nullptr;
    ANativeWindow* readerWindow_ = nullptr; // owned by reader_

    std::string cameraId_;
    SensorInfo info_;
    int32_t streamW_ = 0, streamH_ = 0;
    FrameCallback callback_;

    std::mutex mutex_;       // device/session/request state
    std::mutex frameMutex_;  // held for the duration of each frame callback
    std::atomic<bool> streaming_{false};

    std::mutex metaMutex_;
    std::vector<CaptureMetadata> metaRing_ = std::vector<CaptureMetadata>(8);
    size_t metaNext_ = 0;
    CaptureMetadata frameMeta_; // scratch, reader thread only

    double fps_ = 24.0;
    int64_t exposureNs_ = 1'000'000'000LL / 48;
    int32_t iso_ = 100;
    bool ois_ = true;
    float focusDiopters_ = 0.0f;
    FocusMode focusMode_ = FocusMode::Manual;
    int32_t afRegion_[5] = {0, 0, 0, 0, 0}; // l, t, r, b, weight (weight 0 = none)
    bool autoWb_ = false;
    bool faceDetect_ = false;

    std::mutex recoveryMutex_; // guards recoveryThread_
    std::atomic<bool> closing_{false};
    std::thread recoveryThread_;
    std::atomic<int64_t> lastRecoveryMs_{0};
};

} // namespace vesper
