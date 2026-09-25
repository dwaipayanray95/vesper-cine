#include "camera_engine.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace vesper {

namespace {

void sOnDisconnected(void* ctx, ACameraDevice*) {
    LOGW("Camera disconnected");
    static_cast<CameraEngine*>(ctx)->onDeviceError(-1);
}
void sOnError(void* ctx, ACameraDevice*, int error) { static_cast<CameraEngine*>(ctx)->onDeviceError(error); }
void sOnSessionClosed(void*, ACameraCaptureSession*) { LOGI("Capture session closed"); }
void sOnSessionReady(void*, ACameraCaptureSession*) {}
void sOnSessionActive(void*, ACameraCaptureSession*) { LOGI("Capture session active"); }
void sOnImage(void* ctx, AImageReader* reader) { static_cast<CameraEngine*>(ctx)->onImageAvailable(reader); }
void sOnCaptureCompleted(void* ctx, ACameraCaptureSession*, ACaptureRequest*, const ACameraMetadata* result) {
    static_cast<CameraEngine*>(ctx)->onCaptureCompleted(result);
}

float rational(const ACameraMetadata_rational& r) {
    return r.denominator ? static_cast<float>(r.numerator) / static_cast<float>(r.denominator) : 0.0f;
}

bool readMatrix(const ACameraMetadata* m, uint32_t tag, Mat3& out) {
    ACameraMetadata_const_entry e;
    if (ACameraMetadata_getConstEntry(m, tag, &e) != ACAMERA_OK || e.count < 9) return false;
    for (int i = 0; i < 9; ++i) out[i] = rational(e.data.r[i]);
    return true;
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

CameraEngine::~CameraEngine() {
    {
        std::lock_guard<std::mutex> lock(recoveryMutex_);
        closing_ = true;
    }
    if (recoveryThread_.joinable()) recoveryThread_.join();
    closeCamera();
    if (manager_) ACameraManager_delete(manager_);
}

bool CameraEngine::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!manager_) manager_ = ACameraManager_create();
    return manager_ != nullptr;
}

std::vector<CameraDeviceInfo> CameraEngine::enumerateCameras() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CameraDeviceInfo> out;
    if (!manager_) return out;
    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(manager_, &ids) != ACAMERA_OK || !ids) return out;

    for (int i = 0; i < ids->numCameras; ++i) {
        ACameraMetadata* chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(manager_, ids->cameraIds[i], &chars) != ACAMERA_OK) continue;
        CameraDeviceInfo d;
        d.id = ids->cameraIds[i];
        ACameraMetadata_const_entry e;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_FACING, &e) == ACAMERA_OK) d.facing = e.data.u8[0];
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL, &e) == ACAMERA_OK) d.hardwareLevel = e.data.u8[0];
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
            for (uint32_t c = 0; c + 3 < e.count; c += 4) {
                if (e.data.i32[c] == AIMAGE_FORMAT_RAW10 && e.data.i32[c + 3] == 0) {
                    d.supportsRaw10 = true;
                    if (e.data.i32[c + 1] * e.data.i32[c + 2] > d.largestRaw.width * d.largestRaw.height) {
                        d.largestRaw.width = e.data.i32[c + 1];
                        d.largestRaw.height = e.data.i32[c + 2];
                    }
                }
            }
        }
        if (d.supportsRaw10 && ACameraMetadata_getConstEntry(chars, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, &e) == ACAMERA_OK) {
            for (uint32_t c = 0; c + 3 < e.count; c += 4) {
                if (e.data.i64[c] == AIMAGE_FORMAT_RAW10 && e.data.i64[c + 1] == d.largestRaw.width &&
                    e.data.i64[c + 2] == d.largestRaw.height) {
                    d.largestRaw.minFrameDurationNs = e.data.i64[c + 3];
                }
            }
        }
        LOGI("Camera %s facing=%d hw=%d RAW10=%d %dx%d max %.1f fps", d.id.c_str(), d.facing, d.hardwareLevel,
             d.supportsRaw10, d.largestRaw.width, d.largestRaw.height, d.largestRaw.maxFps());
        out.push_back(d);
        ACameraMetadata_free(chars);
    }
    ACameraManager_deleteCameraIdList(ids);
    // Back-facing RAW cameras first: the main camera is what the app opens.
    std::stable_sort(out.begin(), out.end(), [](const CameraDeviceInfo& a, const CameraDeviceInfo& b) {
        auto rank = [](const CameraDeviceInfo& d) { return (d.supportsRaw10 ? 0 : 2) + (d.facing == ACAMERA_LENS_FACING_BACK ? 0 : 1); };
        return rank(a) < rank(b);
    });
    return out;
}

void CameraEngine::querySensorInfo(const ACameraMetadata* m) {
    SensorInfo s;
    ACameraMetadata_const_entry e;
    if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_INFO_WHITE_LEVEL, &e) == ACAMERA_OK) s.whiteLevel = e.data.i32[0];
    if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &e) == ACAMERA_OK) s.cfa = e.data.u8[0];
    if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_ORIENTATION, &e) == ACAMERA_OK) s.orientation = e.data.i32[0];
    if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &e) == ACAMERA_OK && e.count >= 4) {
        s.activeWidth = e.data.i32[2];
        s.activeHeight = e.data.i32[3];
    }
    // Physical 2x2 order (row-major) -> logical R, Gr, Gb, B.
    if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &e) == ACAMERA_OK && e.count >= 4) {
        for (int bit = 0; bit < 4; ++bit) s.blackLevel[cfaSite(s.cfa, bit & 1, bit >> 1)] = static_cast<float>(e.data.i32[bit]);
    }
    if (ACameraMetadata_getConstEntry(m, ACAMERA_LENS_INFO_SHADING_MAP_SIZE, &e) == ACAMERA_OK && e.count >= 2) {
        s.shadingCols = e.data.i32[0];
        s.shadingRows = e.data.i32[1];
    }
    if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_INFO_LENS_SHADING_APPLIED, &e) == ACAMERA_OK) s.lensShadingApplied = e.data.u8[0] != 0;
    if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE, &e) == ACAMERA_OK)
        s.timestampRealtime = e.data.u8[0] == ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
    if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE, &e) == ACAMERA_OK && e.count >= 2) {
        s.minExposureNs = e.data.i64[0];
        s.maxExposureNs = e.data.i64[1];
    }
    if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, &e) == ACAMERA_OK && e.count >= 2) {
        s.minIso = e.data.i32[0];
        s.maxIso = e.data.i32[1];
    }
    if (ACameraMetadata_getConstEntry(m, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &e) == ACAMERA_OK) s.minFocusDiopters = e.data.f[0];

    // RAW10 sizes + their minimum frame durations (bounds the achievable fps).
    if (ACameraMetadata_getConstEntry(m, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
        for (uint32_t c = 0; c + 3 < e.count; c += 4) {
            if (e.data.i32[c] == AIMAGE_FORMAT_RAW10 && e.data.i32[c + 3] == 0) {
                s.rawModes.push_back({e.data.i32[c + 1], e.data.i32[c + 2], 0});
            }
        }
    }
    if (ACameraMetadata_getConstEntry(m, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, &e) == ACAMERA_OK) {
        for (auto& mode : s.rawModes) {
            for (uint32_t c = 0; c + 3 < e.count; c += 4) {
                if (e.data.i64[c] == AIMAGE_FORMAT_RAW10 && e.data.i64[c + 1] == mode.width && e.data.i64[c + 2] == mode.height)
                    mode.minFrameDurationNs = e.data.i64[c + 3];
            }
        }
    }
    std::sort(s.rawModes.begin(), s.rawModes.end(), [](const RawMode& a, const RawMode& b) {
        return a.width * a.height > b.width * b.height;
    });

    DngCalibration& cal = s.calibration;
    readMatrix(m, ACAMERA_SENSOR_COLOR_TRANSFORM1, cal.colorMatrix1);
    cal.haveColorMatrix2 = readMatrix(m, ACAMERA_SENSOR_COLOR_TRANSFORM2, cal.colorMatrix2);
    cal.haveForwardMatrix1 = readMatrix(m, ACAMERA_SENSOR_FORWARD_MATRIX1, cal.forwardMatrix1);
    cal.haveForwardMatrix2 = readMatrix(m, ACAMERA_SENSOR_FORWARD_MATRIX2, cal.forwardMatrix2);
    readMatrix(m, ACAMERA_SENSOR_CALIBRATION_TRANSFORM1, cal.calibration1);
    if (!readMatrix(m, ACAMERA_SENSOR_CALIBRATION_TRANSFORM2, cal.calibration2)) cal.calibration2 = cal.calibration1;
    if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_REFERENCE_ILLUMINANT1, &e) == ACAMERA_OK)
        cal.illuminant1Kelvin = illuminantToKelvin(e.data.u8[0]);
    if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_REFERENCE_ILLUMINANT2, &e) == ACAMERA_OK)
        cal.illuminant2Kelvin = illuminantToKelvin(e.data.u8[0]);

    LOGI("Sensor: white=%d black=[%.1f %.1f %.1f %.1f] CFA=%d orient=%d active=%dx%d shadingMap=%dx%d (applied=%d) "
         "tsRealtime=%d ISO %d-%d minFocus=%.2fD",
         s.whiteLevel, s.blackLevel[0], s.blackLevel[1], s.blackLevel[2], s.blackLevel[3], s.cfa, s.orientation,
         s.activeWidth, s.activeHeight, s.shadingCols, s.shadingRows, s.lensShadingApplied, s.timestampRealtime,
         s.minIso, s.maxIso, s.minFocusDiopters);
    LOGI("Calibration: illuminants %.0fK/%.0fK, CM2=%d FM1=%d FM2=%d", cal.illuminant1Kelvin, cal.illuminant2Kelvin,
         cal.haveColorMatrix2, cal.haveForwardMatrix1, cal.haveForwardMatrix2);
    for (const auto& mode : s.rawModes) LOGI("RAW10 mode %dx%d max %.1f fps", mode.width, mode.height, mode.maxFps());
    info_ = s;
}

bool CameraEngine::openCamera(const std::string& id) {
    closeCamera();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!manager_) return false;
    ACameraMetadata* chars = nullptr;
    if (ACameraManager_getCameraCharacteristics(manager_, id.c_str(), &chars) != ACAMERA_OK) return false;
    querySensorInfo(chars);
    ACameraMetadata_free(chars);

    static ACameraDevice_StateCallbacks callbacks;
    callbacks.context = this;
    callbacks.onDisconnected = sOnDisconnected;
    callbacks.onError = sOnError;
    if (ACameraManager_openCamera(manager_, id.c_str(), &callbacks, &device_) != ACAMERA_OK || !device_) {
        LOGE("openCamera(%s) failed", id.c_str());
        device_ = nullptr;
        return false;
    }
    cameraId_ = id;
    LOGI("Opened camera %s", id.c_str());
    return true;
}

void CameraEngine::closeCamera() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopCaptureLocked();
    if (device_) {
        ACameraDevice_close(device_);
        device_ = nullptr;
    }
}

bool CameraEngine::startCapture(int32_t width, int32_t height, FrameCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_) return false;
    stopCaptureLocked();
    streamW_ = width;
    streamH_ = height;
    callback_ = std::move(callback);

    if (AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_RAW10, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
                                  /*maxImages=*/5, &reader_) != AMEDIA_OK || !reader_) {
        LOGE("AImageReader RAW10 %dx%d failed", width, height);
        reader_ = nullptr;
        return false;
    }
    AImageReader_ImageListener listener{this, sOnImage};
    AImageReader_setImageListener(reader_, &listener);
    AImageReader_getWindow(reader_, &readerWindow_);

    ACaptureSessionOutputContainer_create(&outputs_);
    ACaptureSessionOutput_create(readerWindow_, &output_);
    ACaptureSessionOutputContainer_add(outputs_, output_);
    static ACameraCaptureSession_stateCallbacks sessionCallbacks;
    sessionCallbacks.context = this;
    sessionCallbacks.onClosed = sOnSessionClosed;
    sessionCallbacks.onReady = sOnSessionReady;
    sessionCallbacks.onActive = sOnSessionActive;
    if (ACameraDevice_createCaptureSession(device_, outputs_, &sessionCallbacks, &session_) != ACAMERA_OK) {
        LOGE("createCaptureSession failed");
        stopCaptureLocked();
        return false;
    }
    if (!buildRequestLocked()) {
        stopCaptureLocked();
        return false;
    }
    streaming_ = true;
    submitLocked();
    LOGI("RAW10 capture started %dx%d @ %.3f fps", width, height, fps_);
    return true;
}

void CameraEngine::stopCapture() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopCaptureLocked();
}

void CameraEngine::stopCaptureLocked() {
    streaming_ = false;
    if (session_) {
        ACameraCaptureSession_stopRepeating(session_);
        ACameraCaptureSession_abortCaptures(session_);
        ACameraCaptureSession_close(session_);
        session_ = nullptr;
    }
    if (request_) { ACaptureRequest_free(request_); request_ = nullptr; }
    if (target_) { ACameraOutputTarget_free(target_); target_ = nullptr; }
    if (outputs_) {
        if (output_) { ACaptureSessionOutputContainer_remove(outputs_, output_); ACaptureSessionOutput_free(output_); output_ = nullptr; }
        ACaptureSessionOutputContainer_free(outputs_);
        outputs_ = nullptr;
    }
    if (reader_) {
        AImageReader_setImageListener(reader_, nullptr);
        AImageReader* r = reader_;
        reader_ = nullptr;
        readerWindow_ = nullptr;
        // Drain a frame callback already in progress, but delete outside the
        // lock: AImageReader_delete joins the reader thread, which may be
        // about to enter onImageAvailable and take frameMutex_.
        { std::lock_guard<std::mutex> drain(frameMutex_); }
        AImageReader_delete(r); // also releases the window the reader owns
    }
}

bool CameraEngine::buildRequestLocked() {
    if (request_) { ACaptureRequest_free(request_); request_ = nullptr; }
    if (ACameraDevice_createCaptureRequest(device_, TEMPLATE_MANUAL, &request_) != ACAMERA_OK) return false;
    if (!target_) ACameraOutputTarget_create(readerWindow_, &target_);
    ACaptureRequest_addTarget(request_, target_);

    auto u8 = [&](uint32_t tag, uint8_t v) { ACaptureRequest_setEntry_u8(request_, tag, 1, &v); };
    // Full manual: no 3A. (NR/edge/shading/tonemap modes never touch the RAW
    // stream itself — shading is set to FAST only so the HAL reports a real
    // lens shading map instead of unity gains.)
    u8(ACAMERA_CONTROL_MODE, ACAMERA_CONTROL_MODE_OFF);
    u8(ACAMERA_CONTROL_AE_MODE, ACAMERA_CONTROL_AE_MODE_OFF);
    u8(ACAMERA_CONTROL_AWB_MODE, ACAMERA_CONTROL_AWB_MODE_OFF);
    u8(ACAMERA_CONTROL_AF_MODE, ACAMERA_CONTROL_AF_MODE_OFF);
    u8(ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF);
    u8(ACAMERA_NOISE_REDUCTION_MODE, ACAMERA_NOISE_REDUCTION_MODE_OFF);
    u8(ACAMERA_EDGE_MODE, ACAMERA_EDGE_MODE_OFF);
    u8(ACAMERA_SHADING_MODE, ACAMERA_SHADING_MODE_FAST);
    u8(ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE, ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE_ON);
    u8(ACAMERA_LENS_OPTICAL_STABILIZATION_MODE,
       ois_ ? ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_ON : ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_OFF);
    ACaptureRequest_setEntry_float(request_, ACAMERA_LENS_FOCUS_DISTANCE, 1, &focusDiopters_);
    return true;
}

// Writes the current fps/exposure/ISO into the request and (re)submits it.
void CameraEngine::submitLocked() {
    if (!session_ || !request_) return;
    int64_t minFrame = 0;
    for (const auto& m : info_.rawModes)
        if (m.width == streamW_ && m.height == streamH_) minFrame = m.minFrameDurationNs;
    int64_t frameNs = std::max<int64_t>(static_cast<int64_t>(1e9 / fps_ + 0.5), minFrame);
    int64_t exposure = std::clamp(exposureNs_, info_.minExposureNs, std::min(info_.maxExposureNs, frameNs));
    int32_t iso = std::clamp(iso_, info_.minIso, info_.maxIso);
    int32_t fpsRange[2] = {static_cast<int32_t>(fps_), static_cast<int32_t>(fps_ + 0.5)};

    ACaptureRequest_setEntry_i64(request_, ACAMERA_SENSOR_FRAME_DURATION, 1, &frameNs);
    ACaptureRequest_setEntry_i64(request_, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exposure);
    ACaptureRequest_setEntry_i32(request_, ACAMERA_SENSOR_SENSITIVITY, 1, &iso);
    ACaptureRequest_setEntry_i32(request_, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fpsRange);

    static ACameraCaptureSession_captureCallbacks cb;
    cb.context = this;
    cb.onCaptureCompleted = sOnCaptureCompleted;
    if (ACameraCaptureSession_setRepeatingRequest(session_, &cb, 1, &request_, nullptr) != ACAMERA_OK) {
        LOGE("setRepeatingRequest failed");
    }
    if (minFrame > 0 && 1e9 / fps_ < minFrame) {
        LOGW("%.3f fps exceeds this RAW mode's max %.1f fps", fps_, 1e9 / static_cast<double>(minFrame));
    }
}

void CameraEngine::setFrameRate(double fps) {
    std::lock_guard<std::mutex> lock(mutex_);
    fps_ = std::clamp(fps, 1.0, 240.0);
    submitLocked();
}

void CameraEngine::setExposure(int64_t exposureNs, int32_t iso) {
    std::lock_guard<std::mutex> lock(mutex_);
    exposureNs_ = exposureNs;
    iso_ = iso;
    submitLocked();
}

void CameraEngine::setOpticalStabilization(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    ois_ = enable;
    if (!request_) return;
    uint8_t v = enable ? ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_ON : ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_OFF;
    ACaptureRequest_setEntry_u8(request_, ACAMERA_LENS_OPTICAL_STABILIZATION_MODE, 1, &v);
    submitLocked();
}

void CameraEngine::setFocusDistance(float diopters) {
    std::lock_guard<std::mutex> lock(mutex_);
    focusDiopters_ = std::clamp(diopters, 0.0f, std::max(info_.minFocusDiopters, 0.0f));
    if (!request_) return;
    ACaptureRequest_setEntry_float(request_, ACAMERA_LENS_FOCUS_DISTANCE, 1, &focusDiopters_);
    submitLocked();
}

void CameraEngine::onCaptureCompleted(const ACameraMetadata* r) {
    CaptureMetadata m;
    ACameraMetadata_const_entry e;
    if (ACameraMetadata_getConstEntry(r, ACAMERA_SENSOR_TIMESTAMP, &e) == ACAMERA_OK) m.timestampNs = e.data.i64[0];
    std::copy(info_.blackLevel, info_.blackLevel + 4, m.blackLevel);
    m.whiteLevel = static_cast<float>(info_.whiteLevel);
    // DYNAMIC_BLACK_LEVEL uses the same physical 2x2 order as BLACK_LEVEL_PATTERN.
    if (ACameraMetadata_getConstEntry(r, ACAMERA_SENSOR_DYNAMIC_BLACK_LEVEL, &e) == ACAMERA_OK && e.count >= 4) {
        for (int bit = 0; bit < 4; ++bit) m.blackLevel[cfaSite(info_.cfa, bit & 1, bit >> 1)] = e.data.f[bit];
    }
    if (ACameraMetadata_getConstEntry(r, ACAMERA_SENSOR_DYNAMIC_WHITE_LEVEL, &e) == ACAMERA_OK && e.count >= 1) {
        m.whiteLevel = static_cast<float>(e.data.i32[0]);
    }
    if (!info_.lensShadingApplied && info_.shadingCols > 1 && info_.shadingRows > 1 &&
        ACameraMetadata_getConstEntry(r, ACAMERA_STATISTICS_LENS_SHADING_MAP, &e) == ACAMERA_OK &&
        e.count == static_cast<uint32_t>(4 * info_.shadingCols * info_.shadingRows)) {
        m.shadingMap.assign(e.data.f, e.data.f + e.count);
        m.shadingCols = info_.shadingCols;
        m.shadingRows = info_.shadingRows;
    }
    if (ACameraMetadata_getConstEntry(r, ACAMERA_SENSOR_NEUTRAL_COLOR_POINT, &e) == ACAMERA_OK && e.count >= 3) {
        m.neutral = {rational(e.data.r[0]), rational(e.data.r[1]), rational(e.data.r[2])};
    }
    if (ACameraMetadata_getConstEntry(r, ACAMERA_SENSOR_EXPOSURE_TIME, &e) == ACAMERA_OK) m.exposureNs = e.data.i64[0];
    if (ACameraMetadata_getConstEntry(r, ACAMERA_SENSOR_SENSITIVITY, &e) == ACAMERA_OK) m.iso = e.data.i32[0];

    std::lock_guard<std::mutex> lock(metaMutex_);
    // Keep the last good shading map if this result didn't carry one.
    if (m.shadingMap.empty()) {
        const auto& prev = metaRing_[(metaNext_ + metaRing_.size() - 1) % metaRing_.size()];
        m.shadingMap = prev.shadingMap;
        m.shadingCols = prev.shadingCols;
        m.shadingRows = prev.shadingRows;
    }
    metaRing_[metaNext_] = std::move(m);
    metaNext_ = (metaNext_ + 1) % metaRing_.size();
}

void CameraEngine::onImageAvailable(AImageReader* reader) {
    if (!streaming_) return;
    std::lock_guard<std::mutex> frameLock(frameMutex_);
    AImage* image = nullptr;
    if (AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK || !image) return;
    if (!streaming_) { AImage_delete(image); return; }

    RawFrame f;
    AImage_getTimestamp(image, &f.timestampNs);
    AImage_getWidth(image, &f.width);
    AImage_getHeight(image, &f.height);
    uint8_t* data = nullptr;
    int len = 0;
    if (AImage_getPlaneData(image, 0, &data, &len) != AMEDIA_OK || !data || len <= 0 ||
        AImage_getPlaneRowStride(image, 0, &f.rowStride) != AMEDIA_OK) {
        AImage_delete(image);
        return;
    }
    f.data = data;
    f.size = static_cast<size_t>(len);

    {
        // Match the capture result carrying this frame's black level / shading map.
        std::lock_guard<std::mutex> lock(metaMutex_);
        const CaptureMetadata* best = &metaRing_[(metaNext_ + metaRing_.size() - 1) % metaRing_.size()];
        for (const auto& m : metaRing_) if (m.timestampNs == f.timestampNs) { best = &m; break; }
        frameMeta_ = *best;
    }
    if (frameMeta_.timestampNs == 0) {
        std::copy(info_.blackLevel, info_.blackLevel + 4, frameMeta_.blackLevel);
        frameMeta_.whiteLevel = static_cast<float>(info_.whiteLevel);
    }
    f.meta = &frameMeta_;
    if (callback_) callback_(f);
    AImage_delete(image);
}

void CameraEngine::onDeviceError(int error) {
    LOGE("Camera device error %d", error);
    streaming_ = false;
    scheduleRecovery();
}

// Recovery must not run on the camera callback thread (closing the device
// from inside its own callback deadlocks), so it gets its own thread.
void CameraEngine::scheduleRecovery() {
    std::lock_guard<std::mutex> lock(recoveryMutex_);
    if (closing_) return;
    int64_t now = nowMs();
    if (now - lastRecoveryMs_.load() < 2000) return;
    lastRecoveryMs_ = now;
    if (recoveryThread_.joinable()) recoveryThread_.detach();
    recoveryThread_ = std::thread([this] {
        std::string id;
        int32_t w, h;
        FrameCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            id = cameraId_;
            w = streamW_;
            h = streamH_;
            cb = callback_;
        }
        if (id.empty() || w <= 0 || closing_) return;
        LOGW("Recovering camera %s", id.c_str());
        if (openCamera(id) && startCapture(w, h, cb)) LOGI("Camera recovered");
        else LOGE("Camera recovery failed");
    });
}

} // namespace vesper
