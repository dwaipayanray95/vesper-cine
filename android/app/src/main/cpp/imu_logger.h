#pragma once
// Gyro + accelerometer logger for Gyroflow (0.28.0). Runs on its OWN thread
// with its own ALooper and ASensorEventQueue: never on the camera or recorder
// thread. Every failure only logs a warning and ends the IMU log; the video
// recording is never touched (start() returns at once, stop never waits on
// the video path).

#include "gyroflow.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace vesper {

struct ImuTimeBase {
    bool cameraRealtime = true;         // camera timestamps are CLOCK_BOOTTIME
    int64_t boottimeMinusMonotonicNs = 0; // measured at start; used when !cameraRealtime
};

// Reads clock_gettime(BOOTTIME) - clock_gettime(MONOTONIC).
int64_t measureBoottimeMinusMonotonicNs();

class ImuLogger {
public:
    ImuLogger() = default;
    ~ImuLogger() { requestStop(); }

    // Spawns the sensor thread and returns. `path`: the .gcsv to write (opened
    // by the thread once the first video frame is known).
    void start(const std::string& path, const ImuTimeBase& base);
    // Camera thread, once per take: the first recorded frame's camera
    // timestamp (t = 0 of the log) and the header fields known by then.
    // Takes a mutex for a few microseconds, nothing else.
    // `lensPath` / `lensJson` (may be empty) are written by the sensor thread too.
    void setFirstFrame(int64_t cameraTimestampNs, const GcsvHeader& header, const std::string& lensPath,
                       const std::string& lensJson);
    bool waitingForFirstFrame();
    // Signals the thread to finish (final drain, header if the first frame is
    // known, close, rename) and returns at once. Safe on any thread, any time.
    void requestStop();
    // requestStop() + waits up to `timeoutMs` for the file to be complete.
    // Used on the publish path only, never while a take is running.
    void finish(int timeoutMs);

    struct State; // shared with the thread (so a late thread can't touch freed memory)
private:
    std::mutex stopMutex_;   // serialises start()/finish() (they own thread_)
    std::mutex mutex_;       // guards shared_ / firstFrameSet_ (the camera thread reads them)
    std::shared_ptr<State> shared_;   // running log
    std::shared_ptr<State> stopping_; // signalled to stop, maybe still closing its file
    std::thread thread_;
    bool firstFrameSet_ = false;
};

} // namespace vesper
