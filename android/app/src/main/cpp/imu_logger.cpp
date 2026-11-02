#include "imu_logger.h"
#include "app_log.h"

#include <android/looper.h>
#include <android/sensor.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

#define IMU_TAG "Vesper_Gyroflow"
#define ILOGI(...) vesperLog(ANDROID_LOG_INFO, IMU_TAG, __VA_ARGS__)
#define ILOGW(...) vesperLog(ANDROID_LOG_WARN, IMU_TAG, __VA_ARGS__)

namespace vesper {

namespace {
constexpr const char* kPackage = "com.theawesomeray.vespercine";
constexpr int kLooperId = 3;
constexpr size_t kMaxPending = 6000; // samples kept before the first frame is known (a few seconds)

int64_t clockNs(clockid_t id) {
    timespec ts{};
    clock_gettime(id, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}
} // namespace

int64_t measureBoottimeMinusMonotonicNs() {
    // Sandwich the BOOTTIME read between two MONOTONIC reads to cancel the call time.
    const int64_t m0 = clockNs(CLOCK_MONOTONIC), b = clockNs(CLOCK_BOOTTIME), m1 = clockNs(CLOCK_MONOTONIC);
    return b - (m0 + m1) / 2;
}

struct ImuLogger::State {
    std::string path;
    ImuTimeBase base;

    std::mutex mutex;
    std::condition_variable cv;
    bool stopRequested = false;
    bool done = false;
    ALooper* looper = nullptr;     // for ALooper_wake
    bool haveFirst = false;
    int64_t t0Boottime = 0;
    GcsvHeader header;
    std::string lensPath, lensJson;
};

namespace {

// Small file via <path>.part + rename, so a reader never sees a partial file.
bool writeWhole(const std::string& path, const std::string& data) {
    const std::string tmp = partPathFor(path);
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    const bool closed = std::fclose(f) == 0;
    return ok && closed && std::rename(tmp.c_str(), path.c_str()) == 0;
}

void threadBody(const std::shared_ptr<ImuLogger::State>& st) {
    ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    {
        std::lock_guard<std::mutex> lk(st->mutex);
        st->looper = looper;
    }
    ASensorEventQueue* queue = nullptr;
    const ASensor *gyro = nullptr, *accel = nullptr;
    FILE* file = nullptr;
    bool failed = false;
    int64_t gyroCount = 0, firstTs = 0, lastTs = 0, maxGapNs = 0;
    std::string fileName = st->path.substr(st->path.find_last_of('/') + 1);

    ASensorManager* mgr = ASensorManager_getInstanceForPackage(kPackage);
    if (mgr) {
        // Calibrated gyroscope first: Android has already removed its running
        // bias estimate, which is what Gyroflow's own phone loggers record. The
        // uncalibrated type is only a fallback when the calibrated one is absent.
        gyro = ASensorManager_getDefaultSensor(mgr, ASENSOR_TYPE_GYROSCOPE);
        if (!gyro) gyro = ASensorManager_getDefaultSensor(mgr, ASENSOR_TYPE_GYROSCOPE_UNCALIBRATED);
        accel = ASensorManager_getDefaultSensor(mgr, ASENSOR_TYPE_ACCELEROMETER);
    }
    if (!gyro) {
        ILOGW("Gyroflow: no gyroscope on this phone, no motion file");
        failed = true;
    } else {
        queue = ASensorManager_createEventQueue(mgr, looper, kLooperId, nullptr, nullptr);
        if (!queue) { ILOGW("Gyroflow: could not create the sensor queue, no motion file"); failed = true; }
    }
    int gyroType = gyro ? ASensor_getType(gyro) : 0;
    if (!failed) {
        auto period = [](const ASensor* s) {
            int d = ASensor_getMinDelay(s); // microseconds; 0 = not continuous
            return d > 0 ? d : 5000;
        };
        const int gp = period(gyro), ap = accel ? period(accel) : 0;
        // A short report latency batches events (fewer wake-ups); timestamps are exact anyway.
        bool ok = ASensorEventQueue_registerSensor(queue, gyro, gp, 20000) == 0;
        if (ok && accel) ok = ASensorEventQueue_registerSensor(queue, accel, ap, 20000) == 0;
        if (!ok) { ILOGW("Gyroflow: could not start the sensors, no motion file"); failed = true; }
        else
            ILOGI("Gyroflow: gyro %s %.0f Hz, accel %s %.0f Hz, timestamps %s", ASensor_getName(gyro), 1e6 / gp,
                  accel ? ASensor_getName(accel) : "none", accel ? 1e6 / ap : 0.0,
                  st->base.cameraRealtime ? "BOOTTIME"
                                          : ("MONOTONIC(+" + std::to_string(st->base.boottimeMinusMonotonicNs) + " ns)").c_str());
    }

    std::deque<GyroSample> pending; // before the first frame is known
    GyroSample lastAccel{};
    std::vector<char> ioBuf; // stdio buffer, must outlive the FILE
    std::string out;
    out.reserve(1 << 16);
    bool headerWritten = false;
    int64_t t0 = 0;
    ASensorEvent events[64];

    auto flushOut = [&] {
        if (file && !out.empty()) {
            if (std::fwrite(out.data(), 1, out.size(), file) != out.size()) {
                ILOGW("Gyroflow: write to %s failed, motion file stopped", fileName.c_str());
                failed = true;
            }
        }
        out.clear();
    };
    auto emit = [&](const GyroSample& g) { out += gcsvRow(g, t0); };

    // A stop request is noticed at the top of the loop; one wake() pending in the
    // looper (ALooper keeps it) makes a poll return at once, so a stop that
    // arrives before or between polls never costs the full timeout. After a stop
    // request the loop runs ONE more pass: drain the sensor queue, open
    // the file if the first frame became known meanwhile, write everything.
    bool stopping = false;
    while (!failed) {
        {
            std::lock_guard<std::mutex> lk(st->mutex);
            if (st->stopRequested) stopping = true;
        }
        // (The NDK has no queue flush; the 20 ms poll below lets the last batched events arrive.)
        int fd, ev;
        void* data;
        ALooper_pollOnce(stopping ? 20 : 200, &fd, &ev, &data);
        ssize_t n;
        while ((n = ASensorEventQueue_getEvents(queue, events, 64)) > 0) {
            for (ssize_t i = 0; i < n; ++i) {
                const ASensorEvent& e = events[i];
                if (e.type == ASENSOR_TYPE_ACCELEROMETER) {
                    lastAccel.accel[0] = e.data[0]; lastAccel.accel[1] = e.data[1]; lastAccel.accel[2] = e.data[2];
                } else if (e.type == gyroType) {
                    GyroSample g = lastAccel; // accelerometer held between its (slower) samples
                    g.tsNs = e.timestamp;
                    g.gyro[0] = e.data[0]; g.gyro[1] = e.data[1]; g.gyro[2] = e.data[2];
                    if (gyroCount == 0) firstTs = g.tsNs;
                    else maxGapNs = std::max(maxGapNs, g.tsNs - lastTs);
                    lastTs = g.tsNs;
                    ++gyroCount;
                    if (headerWritten) emit(g);
                    else {
                        pending.push_back(g);
                        if (pending.size() > kMaxPending) pending.pop_front();
                    }
                }
            }
        }
        // First frame known: open the file, write the header, then the buffered samples.
        if (!headerWritten) {
            GcsvHeader h;
            bool have;
            std::string lensPath, lensJson;
            {
                std::lock_guard<std::mutex> lk(st->mutex);
                have = st->haveFirst;
                t0 = st->t0Boottime;
                h = st->header;
                lensPath.swap(st->lensPath);
                lensJson.swap(st->lensJson);
            }
            if (have) {
                if (!lensPath.empty() && !writeWhole(lensPath, lensJson)) ILOGW("Gyroflow: could not write the lens profile");
                file = std::fopen(partPathFor(st->path).c_str(), "w");
                if (!file) {
                    ILOGW("Gyroflow: could not create %s, no motion file", fileName.c_str());
                    failed = true;
                    break;
                }
                ioBuf.resize(1 << 16);
                std::setvbuf(file, ioBuf.data(), _IOFBF, ioBuf.size());
                out = gcsvHeader(h);
                headerWritten = true;
                for (const GyroSample& g : pending) emit(g);
                pending.clear();
            }
        }
        if (out.size() >= (1 << 15) || (headerWritten && n <= 0)) flushOut();
        if (stopping) break;
    }
    if (!failed) flushOut();
    if (file) {
        const bool closed = std::fclose(file) == 0;
        if (!closed) ILOGW("Gyroflow: closing %s failed", fileName.c_str());
        // Complete only now: the app publishes final names only, never a half-written file.
        if (closed && !failed && std::rename(partPathFor(st->path).c_str(), st->path.c_str()) != 0)
            ILOGW("Gyroflow: could not finish %s", fileName.c_str());
    }
    if (queue) {
        if (gyro) ASensorEventQueue_disableSensor(queue, gyro);
        if (accel) ASensorEventQueue_disableSensor(queue, accel);
        ASensorManager_destroyEventQueue(mgr, queue);
    }
    if (!failed && gyroCount > 1) {
        const double secs = (lastTs - firstTs) * 1e-9;
        ILOGI("Gyroflow: %lld gyro samples (%.0f Hz, max gap %.1f ms), file %s%s", static_cast<long long>(gyroCount),
              secs > 0 ? (gyroCount - 1) / secs : 0.0, maxGapNs * 1e-6, fileName.c_str(),
              headerWritten ? "" : " (never written: no video frame)");
    } else if (!failed) {
        ILOGW("Gyroflow: %lld gyro samples, no usable motion file", static_cast<long long>(gyroCount));
    }
}

// Nothing may escape the thread, and `done` must always be set: finish() waits for it.
void threadMain(std::shared_ptr<ImuLogger::State> st) {
    try {
        threadBody(st);
    } catch (const std::exception& e) {
        ILOGW("Gyroflow: sensor thread failed (%s), motion file stopped", e.what());
    } catch (...) {
        ILOGW("Gyroflow: sensor thread failed, motion file stopped");
    }
    {
        std::lock_guard<std::mutex> lk(st->mutex);
        st->looper = nullptr;
        st->done = true;
    }
    st->cv.notify_all();
}

} // namespace

void ImuLogger::start(const std::string& path, const ImuTimeBase& base) {
    std::lock_guard<std::mutex> sl(stopMutex_);
    requestStop();
    // A previous thread still closing its file finishes by itself (its own State).
    if (thread_.joinable()) thread_.detach();
    auto st = std::make_shared<State>();
    st->path = path;
    st->base = base;
    try {
        thread_ = std::thread(threadMain, st);
    } catch (...) {
        ILOGW("Gyroflow: could not start the sensor thread");
        return;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    shared_ = st;
    firstFrameSet_ = false;
}

bool ImuLogger::waitingForFirstFrame() {
    std::lock_guard<std::mutex> lk(mutex_);
    return shared_ && !firstFrameSet_;
}

void ImuLogger::setFirstFrame(int64_t cameraTimestampNs, const GcsvHeader& header, const std::string& lensPath,
                              const std::string& lensJson) {
    std::shared_ptr<State> st;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!shared_ || firstFrameSet_) return;
        firstFrameSet_ = true;
        st = shared_;
    }
    std::lock_guard<std::mutex> lk(st->mutex);
    st->t0Boottime = toBoottimeNs(cameraTimestampNs, st->base.cameraRealtime, st->base.boottimeMinusMonotonicNs);
    st->header = header;
    st->lensPath = lensPath;
    st->lensJson = lensJson;
    st->haveFirst = true;
    if (st->looper) ALooper_wake(st->looper);
}

void ImuLogger::requestStop() {
    std::shared_ptr<State> st;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        st = std::move(shared_);
        shared_.reset();
        if (st) stopping_ = st;
    }
    if (!st) return;
    std::lock_guard<std::mutex> lk(st->mutex);
    st->stopRequested = true;
    if (st->looper) ALooper_wake(st->looper);
}

void ImuLogger::finish(int timeoutMs) {
    std::lock_guard<std::mutex> sl(stopMutex_);
    requestStop();
    std::shared_ptr<State> st;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        st = std::move(stopping_);
        stopping_.reset();
    }
    if (!st) return;
    bool done;
    {
        std::unique_lock<std::mutex> lk(st->mutex);
        done = st->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return st->done; });
    }
    if (thread_.joinable()) {
        if (done) thread_.join();
        else {
            ILOGW("Gyroflow: sensor thread slow to stop, leaving it to finish");
            thread_.detach();
        }
    }
}

} // namespace vesper
