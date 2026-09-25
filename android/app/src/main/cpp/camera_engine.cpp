#include "camera_engine.h"

#include <cstring>
#include <cmath>

namespace rcamera {

namespace {
// Mirrors cfaSiteAt() in shaders/mhc_rlog.comp: resolves which logical CFA
// channel (0=R, 1=Gr, 2=Gb, 3=B) occupies a raw 2x2 quadrant position, given
// the sensor's actual reported CFA arrangement (0=RGGB,1=GRBG,2=GBRG,3=BGGR).
int CfaSiteAt(int32_t cfaPattern, int phaseX, int phaseY) {
    int bit = (phaseY << 1) | phaseX;
    switch (cfaPattern) {
        case 0: { static const int m[4] = {0, 1, 2, 3}; return m[bit]; } // RGGB
        case 1: { static const int m[4] = {1, 0, 3, 2}; return m[bit]; } // GRBG
        case 2: { static const int m[4] = {2, 3, 0, 1}; return m[bit]; } // GBRG
        default:{ static const int m[4] = {3, 2, 1, 0}; return m[bit]; } // BGGR
    }
}
} // namespace

// Static C-to-C++ callback shims
static void sOnDeviceDisconnected(void* context, ACameraDevice* device) {
    auto* engine = static_cast<CameraEngine*>(context);
    engine->onDeviceDisconnected(device);
}

static void sOnDeviceError(void* context, ACameraDevice* device, int error) {
    auto* engine = static_cast<CameraEngine*>(context);
    engine->onDeviceError(device, error);
}

static void sOnSessionActive(void* context, ACameraCaptureSession* session) {
    auto* engine = static_cast<CameraEngine*>(context);
    engine->onSessionActive(session);
}

static void sOnSessionClosed(void* context, ACameraCaptureSession* session) {
    auto* engine = static_cast<CameraEngine*>(context);
    engine->onSessionClosed(session);
}

static void sOnSessionReady(void* context, ACameraCaptureSession* session) {
    auto* engine = static_cast<CameraEngine*>(context);
    engine->onSessionReady(session);
}

static void sOnImageAvailable(void* context, AImageReader* reader) {
    auto* engine = static_cast<CameraEngine*>(context);
    engine->onImageAvailable(reader);
}

CameraEngine::CameraEngine() = default;

CameraEngine::~CameraEngine() {
    stopCaptureSession();
    closeCamera();
    if (cameraManager_) {
        ACameraManager_delete(cameraManager_);
        cameraManager_ = nullptr;
    }
}

bool CameraEngine::initialize() {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (isInitialized_) return true;

    cameraManager_ = ACameraManager_create();
    if (!cameraManager_) {
        LOGE("Failed to create ACameraManager");
        return false;
    }

    isInitialized_ = true;
    LOGI("ACameraManager successfully initialized");
    return true;
}

std::vector<CameraDeviceInfo> CameraEngine::enumerateCameras() {
    std::lock_guard<std::mutex> lock(engineMutex_);
    std::vector<CameraDeviceInfo> devices;

    if (!cameraManager_) return devices;

    ACameraIdList* cameraIdList = nullptr;
    camera_status_t status = ACameraManager_getCameraIdList(cameraManager_, &cameraIdList);
    if (status != ACAMERA_OK || !cameraIdList) {
        LOGE("Failed to get camera ID list: %d", status);
        return devices;
    }

    for (int i = 0; i < cameraIdList->numCameras; ++i) {
        const char* cameraId = cameraIdList->cameraIds[i];
        ACameraMetadata* chars = nullptr;
        status = ACameraManager_getCameraCharacteristics(cameraManager_, cameraId, &chars);
        if (status != ACAMERA_OK || !chars) continue;

        CameraDeviceInfo info;
        info.cameraId = cameraId;

        // Hardware level
        ACameraMetadata_const_entry entry;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL, &entry) == ACAMERA_OK) {
            info.hardwareLevel = entry.data.u8[0];
        }

        // Lens facing (check rear = ACAMERA_LENS_FACING_BACK)
        uint8_t facing = ACAMERA_LENS_FACING_FRONT;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_FACING, &entry) == ACAMERA_OK) {
            facing = entry.data.u8[0];
        }

        // Check RAW10 stream configuration
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry) == ACAMERA_OK) {
            // Entry format: format, width, height, isInput
            for (size_t c = 0; c < entry.count; c += 4) {
                int32_t format = entry.data.i32[c];
                int32_t width = entry.data.i32[c + 1];
                int32_t height = entry.data.i32[c + 2];
                int32_t isInput = entry.data.i32[c + 3];

                if (format == AIMAGE_FORMAT_RAW10 && isInput == 0) {
                    info.supportsRaw10 = true;
                    if (width * height > info.rawWidth * info.rawHeight) {
                        info.rawWidth = width;
                        info.rawHeight = height;
                    }
                }
            }
        }

        // What does the HAL itself claim the max achievable frame rate is for this
        // exact RAW10 stream size? Diagnostic for a measured ~7fps far below the
        // app's 24fps UI target — this settles whether that's a hardware/driver
        // ceiling for full-resolution RAW10 readout (nothing app-level can change)
        // versus a bug still to find in our own pipeline.
        int64_t minFrameDurationNs = -1;
        if (info.supportsRaw10 && ACameraMetadata_getConstEntry(
                chars, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, &entry) == ACAMERA_OK) {
            // Entry format: format, width, height, minFrameDurationNs
            for (size_t c = 0; c + 3 < entry.count; c += 4) {
                if (entry.data.i64[c] == AIMAGE_FORMAT_RAW10 &&
                    entry.data.i64[c + 1] == info.rawWidth &&
                    entry.data.i64[c + 2] == info.rawHeight) {
                    minFrameDurationNs = entry.data.i64[c + 3];
                    break;
                }
            }
        }

        // Log camera details
        LOGI("Found Camera ID: %s, Facing: %d, HW Level: %d, Supports RAW10: %d, Max RAW: %dx%d",
             cameraId, facing, info.hardwareLevel, info.supportsRaw10, info.rawWidth, info.rawHeight);
        if (minFrameDurationNs > 0) {
            LOGI("RAW10 %dx%d min frame duration: %lld ns (HAL-reported max ~%.2f fps)",
                 info.rawWidth, info.rawHeight, static_cast<long long>(minFrameDurationNs),
                 1e9 / static_cast<double>(minFrameDurationNs));
        } else if (info.supportsRaw10) {
            LOGW("RAW10 %dx%d min frame duration not reported by this HAL", info.rawWidth, info.rawHeight);
        }

        devices.push_back(info);
        ACameraMetadata_free(chars);
    }

    ACameraManager_deleteCameraIdList(cameraIdList);
    return devices;
}

bool CameraEngine::openCamera(const std::string& cameraId) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (!cameraManager_) return false;

    if (cameraDevice_) {
        closeCamera();
    }

    activeCameraId_ = cameraId;

    // Read calibration metadata before opening
    ACameraMetadata* chars = nullptr;
    if (ACameraManager_getCameraCharacteristics(cameraManager_, cameraId.c_str(), &chars) == ACAMERA_OK) {
        querySensorCalibration(chars);
        ACameraMetadata_free(chars);
    }

    ACameraDevice_StateCallbacks deviceCallbacks {
        .context = this,
        .onDisconnected = sOnDeviceDisconnected,
        .onError = sOnDeviceError
    };

    camera_status_t status = ACameraManager_openCamera(
        cameraManager_,
        cameraId.c_str(),
        &deviceCallbacks,
        &cameraDevice_
    );

    if (status != ACAMERA_OK || !cameraDevice_) {
        LOGE("Failed to open camera %s: %d", cameraId.c_str(), status);
        return false;
    }

    LOGI("Successfully opened camera device %s", cameraId.c_str());
    return true;
}

void CameraEngine::closeCamera() {
    stopCaptureSession();
    if (cameraDevice_) {
        ACameraDevice_close(cameraDevice_);
        cameraDevice_ = nullptr;
        LOGI("Camera device closed");
    }
}

void CameraEngine::querySensorCalibration(ACameraMetadata* metadata) {
    ACameraMetadata_const_entry entry;

    // White level
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_INFO_WHITE_LEVEL, &entry) == ACAMERA_OK) {
        calibrationMetadata_.whiteLevel = entry.data.i32[0];
    }

    // Active array size
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &entry) == ACAMERA_OK && entry.count >= 4) {
        calibrationMetadata_.activeArrayWidth = entry.data.i32[2];
        calibrationMetadata_.activeArrayHeight = entry.data.i32[3];
    }

    // Color filter arrangement (CFA) — queried before black level below, since
    // converting the black level pattern's physical ordering into the logical
    // [R,Gr,Gb,B] order the shader expects requires knowing it.
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &entry) == ACAMERA_OK) {
        calibrationMetadata_.cfaPattern = entry.data.u8[0];
    }

    // Black level. ACAMERA_SENSOR_DYNAMIC_BLACK_LEVEL (previously queried here) is a
    // CaptureResult-only key — it is not a valid tag on static CameraCharacteristics,
    // which is all `metadata` is at camera-open time, so that query always silently
    // missed and blackLevel stayed at its hardcoded {64,64,64,64} struct default
    // regardless of the real sensor. ACAMERA_SENSOR_BLACK_LEVEL_PATTERN is the static
    // equivalent, but per the Camera2 docs it's an int32 array ordered by physical
    // 2x2 position (top-left -> bottom-right), not the fixed logical [R,Gr,Gb,B]
    // order dynamic black level uses — remap it through the CFA arrangement so
    // blackLevel[] stays indexed the way fetchBayer()'s cfaSiteAt() expects.
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &entry) == ACAMERA_OK && entry.count >= 4) {
        for (int bit = 0; bit < 4; ++bit) {
            int phaseX = bit & 1;
            int phaseY = (bit >> 1) & 1;
            int logicalSite = CfaSiteAt(calibrationMetadata_.cfaPattern, phaseX, phaseY);
            calibrationMetadata_.blackLevel[logicalSite] = static_cast<float>(entry.data.i32[bit]);
        }
    }

    // Physical sensor mounting angle relative to the device's natural (portrait) orientation.
    // On Pixel rear cameras this is typically 90 degrees clockwise.
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_ORIENTATION, &entry) == ACAMERA_OK) {
        calibrationMetadata_.sensorOrientation = entry.data.i32[0];
    }

    // Color transform 1 (3x3 rational/float)
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_COLOR_TRANSFORM1, &entry) == ACAMERA_OK && entry.count >= 9) {
        for (int i = 0; i < 9; ++i) {
            calibrationMetadata_.colorTransform1[i] = static_cast<float>(entry.data.r[i].numerator) /
                                                      static_cast<float>(entry.data.r[i].denominator);
        }
    }

    // Forward matrix 1: maps white-balanced sensor RGB -> CIE XYZ (D50). This is
    // the matrix the render pipeline actually needs (see native_bridge.cpp).
    // ACAMERA_SENSOR_FORWARD_MATRIX1 is OPTIONAL per the Camera2/DNG spec — if a
    // device's camera HAL doesn't populate it, forwardMatrix1 silently stays at
    // the identity default below, which the log line makes visible.
    bool haveForwardMatrix = false;
    if (ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_FORWARD_MATRIX1, &entry) == ACAMERA_OK && entry.count >= 9) {
        for (int i = 0; i < 9; ++i) {
            calibrationMetadata_.forwardMatrix1[i] = static_cast<float>(entry.data.r[i].numerator) /
                                                     static_cast<float>(entry.data.r[i].denominator);
        }
        haveForwardMatrix = true;
    }

    LOGI("Queried Sensor Calibration: WhiteLevel=%d, BlackLevel=[%.1f, %.1f, %.1f, %.1f], ActiveArray=%dx%d, CFA=%d, SensorOrientation=%d",
         calibrationMetadata_.whiteLevel,
         calibrationMetadata_.blackLevel[0], calibrationMetadata_.blackLevel[1],
         calibrationMetadata_.blackLevel[2], calibrationMetadata_.blackLevel[3],
         calibrationMetadata_.activeArrayWidth, calibrationMetadata_.activeArrayHeight,
         calibrationMetadata_.cfaPattern, calibrationMetadata_.sensorOrientation);

    const auto& fm = calibrationMetadata_.forwardMatrix1;
    LOGI("ForwardMatrix1 present=%d: [%.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f]",
         haveForwardMatrix, fm[0], fm[1], fm[2], fm[3], fm[4], fm[5], fm[6], fm[7], fm[8]);
}

bool CameraEngine::startCaptureSession(int32_t width, int32_t height, FrameCallback callback) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (!cameraDevice_) {
        LOGE("Cannot start capture session: cameraDevice is null");
        return false;
    }

    stopCaptureSession();
    frameCallback_ = callback;
    lastStreamWidth_ = width;
    lastStreamHeight_ = height;

    // CPU-read usage: this hardware's GPU driver cannot import a RAW10
    // AHardwareBuffer as a sampled Vulkan image (see VulkanComputeEngine), so
    // frames are read on the CPU via AImage_getPlaneData and uploaded to the
    // GPU as a storage buffer instead of a zero-copy sampled image.
    uint64_t usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    int32_t maxImages = 6; // Ring buffer depth for smooth capture

    media_status_t mStatus = AImageReader_newWithUsage(
        width, height,
        AIMAGE_FORMAT_RAW10,
        usage,
        maxImages,
        &imageReader_
    );

    if (mStatus != AMEDIA_OK || !imageReader_) {
        LOGE("Failed to create AImageReader for RAW10 (%dx%d): %d", width, height, mStatus);
        return false;
    }

    AImageReader_ImageListener imageListener {
        .context = this,
        .onImageAvailable = sOnImageAvailable
    };
    AImageReader_setImageListener(imageReader_, &imageListener);

    AImageReader_getWindow(imageReader_, &imageReaderWindow_);
    if (!imageReaderWindow_) {
        LOGE("Failed to get ANativeWindow from AImageReader");
        return false;
    }

    // Option B: Camera2 outputs exclusively to the AImageReader (RAW10). The
    // Flutter viewfinder surface is never registered as a capture target —
    // it is fed by the GPU compute pipeline (see VulkanComputeEngine), so
    // what's on screen is always what the R-Log color pipeline produced,
    // never the ISP's own tonemapped preview.
    ACaptureSessionOutputContainer_create(&outputContainer_);
    ACaptureSessionOutput_create(imageReaderWindow_, &sessionOutput_);
    ACaptureSessionOutputContainer_add(outputContainer_, sessionOutput_);

    ACameraCaptureSession_stateCallbacks sessionCallbacks {
        .context = this,
        .onClosed = sOnSessionClosed,
        .onReady = sOnSessionReady,
        .onActive = sOnSessionActive
    };

    camera_status_t cStatus = ACameraDevice_createCaptureSession(
        cameraDevice_,
        outputContainer_,
        &sessionCallbacks,
        &captureSession_
    );

    if (cStatus != ACAMERA_OK || !captureSession_) {
        LOGE("Failed to create capture session: %d", cStatus);
        return false;
    }

    // Create repeating capture request with ISP bypass
    if (!configureCaptureRequest()) {
        LOGE("Failed to configure capture request");
        return false;
    }

    cStatus = ACameraCaptureSession_setRepeatingRequest(
        captureSession_,
        nullptr, // No capture result callback needed for maximum throughput
        1,
        &captureRequest_,
        nullptr
    );

    if (cStatus != ACAMERA_OK) {
        LOGE("Failed to set repeating capture request: %d", cStatus);
        return false;
    }

    isStreaming_ = true;
    LOGI("RAW10 capture session successfully started at %dx%d", width, height);
    return true;
}

bool CameraEngine::configureCaptureRequest() {
    if (!cameraDevice_ || !imageReaderWindow_) return false;

    if (captureRequest_) {
        ACaptureRequest_free(captureRequest_);
        captureRequest_ = nullptr;
    }

    // TEMPLATE_MANUAL guarantees zero default auto-exposure/auto-processing interference
    camera_status_t status = ACameraDevice_createCaptureRequest(cameraDevice_, TEMPLATE_MANUAL, &captureRequest_);
    if (status != ACAMERA_OK || !captureRequest_) {
        LOGE("Failed to create manual capture request: %d", status);
        return false;
    }

    ACameraOutputTarget* outputTarget = nullptr;
    ACameraOutputTarget_create(imageReaderWindow_, &outputTarget);
    ACaptureRequest_addTarget(captureRequest_, outputTarget);
    ACameraOutputTarget_free(outputTarget);

    // =========================================================================
    // ISP BYPASS CONFIGURATION: Completely disable post-processing stages
    // =========================================================================

    // 1. Completely turn off Noise Reduction
    uint8_t nrMode = ACAMERA_NOISE_REDUCTION_MODE_OFF;
    ACaptureRequest_setEntry_u8(captureRequest_, ACAMERA_NOISE_REDUCTION_MODE, 1, &nrMode);

    // 2. Completely turn off Edge Enhancement (Unsharp mask)
    uint8_t edgeMode = ACAMERA_EDGE_MODE_OFF;
    ACaptureRequest_setEntry_u8(captureRequest_, ACAMERA_EDGE_MODE, 1, &edgeMode);

    // 3. Set Tonemap to manual contrast curve (bypassing algorithmic HDR tone mapping)
    uint8_t tonemapMode = ACAMERA_TONEMAP_MODE_CONTRAST_CURVE;
    ACaptureRequest_setEntry_u8(captureRequest_, ACAMERA_TONEMAP_MODE, 1, &tonemapMode);

    // 4. Disable lens shading correction if supported
    uint8_t shadingMode = ACAMERA_SHADING_MODE_OFF;
    ACaptureRequest_setEntry_u8(captureRequest_, ACAMERA_SHADING_MODE, 1, &shadingMode);

    // 5. Disable Auto Exposure (AE) & Auto White Balance (AWB) & Auto Focus (AF)
    uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_OFF;
    ACaptureRequest_setEntry_u8(captureRequest_, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);

    uint8_t awbMode = ACAMERA_CONTROL_AWB_MODE_OFF;
    ACaptureRequest_setEntry_u8(captureRequest_, ACAMERA_CONTROL_AWB_MODE, 1, &awbMode);

    uint8_t afMode = ACAMERA_CONTROL_AF_MODE_OFF;
    ACaptureRequest_setEntry_u8(captureRequest_, ACAMERA_CONTROL_AF_MODE, 1, &afMode);

    // 6. Set manual exposure time & sensitivity
    ACaptureRequest_setEntry_i64(captureRequest_, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &currentExposureNs_);
    ACaptureRequest_setEntry_i32(captureRequest_, ACAMERA_SENSOR_SENSITIVITY, 1, &currentIso_);

    // 7. Set manual white balance color correction gains (R, Gr, Gb, B)
    float gains[4] = { currentRGain_, currentGGain_, currentGGain_, currentBGain_ };
    ACaptureRequest_setEntry_float(captureRequest_, ACAMERA_COLOR_CORRECTION_GAINS, 4, gains);

    // 8. Optical Image Stabilization (Voice-Coil Motor hardware lens floating)
    uint8_t oisMode = oisEnabled_ ? ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_ON
                                  : ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_OFF;
    ACaptureRequest_setEntry_u8(captureRequest_, ACAMERA_LENS_OPTICAL_STABILIZATION_MODE, 1, &oisMode);

    // 9. Manual Focus distance
    ACaptureRequest_setEntry_float(captureRequest_, ACAMERA_LENS_FOCUS_DISTANCE, 1, &focusDistance_);

    LOGI("Capture request configured with ISP BYPASS: NR=OFF, Edge=OFF, Tonemap=CURVE, AE=OFF, Exp=%lld ns, ISO=%d",
         static_cast<long long>(currentExposureNs_), currentIso_);

    return true;
}

void CameraEngine::stopCaptureSession() {
    isStreaming_ = false;

    if (captureSession_) {
        ACameraCaptureSession_stopRepeating(captureSession_);
        ACameraCaptureSession_close(captureSession_);
        captureSession_ = nullptr;
    }

    if (captureRequest_) {
        ACaptureRequest_free(captureRequest_);
        captureRequest_ = nullptr;
    }

    if (outputContainer_) {
        if (sessionOutput_) {
            ACaptureSessionOutputContainer_remove(outputContainer_, sessionOutput_);
            ACaptureSessionOutput_free(sessionOutput_);
            sessionOutput_ = nullptr;
        }
        ACaptureSessionOutputContainer_free(outputContainer_);
        outputContainer_ = nullptr;
    }

    if (imageReaderWindow_) {
        ANativeWindow_release(imageReaderWindow_);
        imageReaderWindow_ = nullptr;
    }

    if (imageReader_) {
        AImageReader_delete(imageReader_);
        imageReader_ = nullptr;
    }

    LOGI("Capture session stopped and resources cleaned up");
}

void CameraEngine::setExposure(int64_t exposureTimeNs, int32_t iso) {
    currentExposureNs_ = exposureTimeNs;
    currentIso_ = iso;

    if (captureSession_ && captureRequest_) {
        ACaptureRequest_setEntry_i64(captureRequest_, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &currentExposureNs_);
        ACaptureRequest_setEntry_i32(captureRequest_, ACAMERA_SENSOR_SENSITIVITY, 1, &currentIso_);
        ACameraCaptureSession_setRepeatingRequest(captureSession_, nullptr, 1, &captureRequest_, nullptr);
    }
}

void CameraEngine::setShutterAngle(float shutterAngleDeg, float fps, int32_t iso) {
    if (fps <= 0.0f) fps = 24.0f;
    // Exposure time = (shutterAngle / 360.0) / fps
    double exposureSeconds = (static_cast<double>(shutterAngleDeg) / 360.0) / static_cast<double>(fps);
    int64_t exposureNs = static_cast<int64_t>(exposureSeconds * 1e9);
    setExposure(exposureNs, iso);
}

void CameraEngine::setWhitebalanceGains(float rGain, float gGain, float bGain) {
    currentRGain_ = rGain;
    currentGGain_ = gGain;
    currentBGain_ = bGain;

    if (captureSession_ && captureRequest_) {
        float gains[4] = { currentRGain_, currentGGain_, currentGGain_, currentBGain_ };
        ACaptureRequest_setEntry_float(captureRequest_, ACAMERA_COLOR_CORRECTION_GAINS, 4, gains);
        ACameraCaptureSession_setRepeatingRequest(captureSession_, nullptr, 1, &captureRequest_, nullptr);
    }
}

void CameraEngine::setOpticalStabilization(bool enableOis) {
    oisEnabled_ = enableOis;
    if (captureSession_ && captureRequest_) {
        uint8_t oisMode = oisEnabled_ ? ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_ON
                                      : ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_OFF;
        ACaptureRequest_setEntry_u8(captureRequest_, ACAMERA_LENS_OPTICAL_STABILIZATION_MODE, 1, &oisMode);
        ACameraCaptureSession_setRepeatingRequest(captureSession_, nullptr, 1, &captureRequest_, nullptr);
    }
}

void CameraEngine::setFocusDistance(float diopters) {
    focusDistance_ = diopters;
    if (captureSession_ && captureRequest_) {
        ACaptureRequest_setEntry_float(captureRequest_, ACAMERA_LENS_FOCUS_DISTANCE, 1, &focusDistance_);
        ACameraCaptureSession_setRepeatingRequest(captureSession_, nullptr, 1, &captureRequest_, nullptr);
    }
}

void CameraEngine::onDeviceDisconnected(ACameraDevice* device) {
    LOGW("Camera device disconnected");
    isStreaming_ = false;
}

void CameraEngine::onDeviceError(ACameraDevice* device, int error) {
    LOGE("Camera device error: %d", error);
    isStreaming_ = false;

    // ERROR_CAMERA_DEVICE / ERROR_CAMERA_SERVICE are documented as fatal: the
    // ACameraDevice is already unusable and the framework expects the app to
    // close and reopen it. Previously this callback only logged and left the
    // engine idle forever (viewfinder frozen until the app was killed).
    if (error == ERROR_CAMERA_DEVICE || error == ERROR_CAMERA_SERVICE) {
        attemptDeviceErrorRecovery();
    }
}

void CameraEngine::attemptDeviceErrorRecovery() {
    // Rate-limit: a truly dead camera/driver would otherwise retrigger this
    // callback in a tight loop as each recovery attempt itself fails.
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (nowMs - lastRecoveryAttemptMs_ < 2000) {
        LOGW("Skipping camera device recovery attempt (rate-limited)");
        return;
    }
    lastRecoveryAttemptMs_ = nowMs;

    if (activeCameraId_.empty() || lastStreamWidth_ <= 0 || lastStreamHeight_ <= 0) {
        LOGE("Cannot recover camera device: no prior session to restore");
        return;
    }

    LOGW("Attempting camera device recovery: reopening %s and restarting %dx%d stream",
         activeCameraId_.c_str(), lastStreamWidth_, lastStreamHeight_);

    std::string cameraId = activeCameraId_; // openCamera() below reassigns this member
    FrameCallback callback = frameCallback_; // startCaptureSession() below reassigns this member
    int32_t width = lastStreamWidth_;
    int32_t height = lastStreamHeight_;

    if (!openCamera(cameraId)) {
        LOGE("Camera device recovery failed: could not reopen %s", cameraId.c_str());
        return;
    }
    if (!startCaptureSession(width, height, callback)) {
        LOGE("Camera device recovery failed: could not restart capture session");
        return;
    }

    LOGI("Camera device recovery succeeded");
}

void CameraEngine::onSessionActive(ACameraCaptureSession* session) {
    LOGI("Capture session active");
}

void CameraEngine::onSessionClosed(ACameraCaptureSession* session) {
    LOGI("Capture session closed");
}

void CameraEngine::onSessionReady(ACameraCaptureSession* session) {
    LOGI("Capture session ready");
}

void CameraEngine::onImageAvailable(AImageReader* reader) {
    AImage* image = nullptr;
    media_status_t status = AImageReader_acquireLatestImageAsync(reader, &image, nullptr);
    if (status != AMEDIA_OK) {
        static int errCount = 0;
        if (++errCount % 30 == 0) {
            LOGW("AImageReader_acquireLatestImageAsync returned error: %d", status);
        }
        return;
    }
    if (!image) return;

    static int frameCounter = 0;
    if (++frameCounter % 30 == 0) {
        LOGI("Successfully received %d RAW10 frames from Pixel sensor!", frameCounter);
    }

    int64_t timestamp = 0;
    AImage_getTimestamp(image, &timestamp);

    uint8_t* data = nullptr;
    int dataLength = 0;
    int32_t rowStride = 0;
    media_status_t planeStatus = AImage_getPlaneData(image, 0, &data, &dataLength);
    AImage_getPlaneRowStride(image, 0, &rowStride);

    if (planeStatus == AMEDIA_OK && data && dataLength > 0 && frameCallback_) {
        // Synchronous: frameCallback_ (GPU dispatch) fully completes, including its
        // fence wait, before returning, so `data` stays valid for its whole duration.
        frameCallback_(data, static_cast<size_t>(dataLength), rowStride, timestamp);
    }

    AImage_delete(image);
}

} // namespace rcamera
