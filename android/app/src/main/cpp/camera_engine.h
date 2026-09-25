#pragma once

#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraMetadata.h>
#include <media/NdkImageReader.h>
#include <android/native_window.h>
#include <android/hardware_buffer.h>
#include <android/log.h>

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <atomic>

#define TAG "RCamera_Engine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace rcamera {

struct SensorCalibrationMetadata {
    int32_t whiteLevel = 1023;
    float blackLevel[4] = { 64.0f, 64.0f, 64.0f, 64.0f }; // R, Gr, Gb, B
    float colorTransform1[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    float calibrationTransform1[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    float forwardMatrix1[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    int32_t activeArrayWidth = 4080;
    int32_t activeArrayHeight = 3072;
    int32_t cfaPattern = 0; // 0=RGGB, 1=GRBG, 2=GBRG, 3=BGGR
    int32_t sensorOrientation = 90; // ACAMERA_SENSOR_ORIENTATION, degrees clockwise
};

struct CameraDeviceInfo {
    std::string cameraId;
    int32_t hardwareLevel = 0;
    bool supportsRaw10 = false;
    int32_t rawWidth = 0;
    int32_t rawHeight = 0;
};

using FrameCallback = std::function<void(AHardwareBuffer* buffer, int64_t timestampNs)>;

class CameraEngine {
public:
    CameraEngine();
    ~CameraEngine();

    bool initialize();
    std::vector<CameraDeviceInfo> enumerateCameras();
    bool openCamera(const std::string& cameraId);
    void closeCamera();

    bool startCaptureSession(int32_t width, int32_t height, FrameCallback callback);
    void stopCaptureSession();

    // Manual controls (Bypassing ISP auto-algorithms)
    void setExposure(int64_t exposureTimeNs, int32_t iso);
    void setShutterAngle(float shutterAngleDeg, float fps, int32_t iso);
    void setWhitebalanceGains(float rGain, float gGain, float bGain);
    void setOpticalStabilization(bool enableOis);
    void setFocusDistance(float diopters);

    const SensorCalibrationMetadata& getCalibrationMetadata() const { return calibrationMetadata_; }
    bool isStreaming() const { return isStreaming_.load(); }

    // Internal NDK callbacks
    void onDeviceDisconnected(ACameraDevice* device);
    void onDeviceError(ACameraDevice* device, int error);
    void onSessionActive(ACameraCaptureSession* session);
    void onSessionClosed(ACameraCaptureSession* session);
    void onSessionReady(ACameraCaptureSession* session);
    void onImageAvailable(AImageReader* reader);

private:
    void querySensorCalibration(ACameraMetadata* metadata);
    bool configureCaptureRequest();

    ACameraManager* cameraManager_ = nullptr;
    ACameraDevice* cameraDevice_ = nullptr;
    ACameraCaptureSession* captureSession_ = nullptr;
    ACaptureSessionOutputContainer* outputContainer_ = nullptr;
    ACaptureSessionOutput* sessionOutput_ = nullptr;
    ACaptureRequest* captureRequest_ = nullptr;
    ANativeWindow* imageReaderWindow_ = nullptr;
    AImageReader* imageReader_ = nullptr;

    std::string activeCameraId_;
    SensorCalibrationMetadata calibrationMetadata_;
    FrameCallback frameCallback_;

    std::mutex engineMutex_;
    std::atomic<bool> isStreaming_{false};
    std::atomic<bool> isInitialized_{false};

    // Manual controls state
    int64_t currentExposureNs_ = 1000000000LL / 48LL; // 1/48s default
    int32_t currentIso_ = 100;
    float currentRGain_ = 1.8f;
    float currentGGain_ = 1.0f;
    float currentBGain_ = 1.9f;
    bool oisEnabled_ = true;
    float focusDistance_ = 0.0f; // infinity/hyperfocal
};

} // namespace rcamera
