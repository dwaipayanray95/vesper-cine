#pragma once

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <aaudio/AAudio.h>
#include <android/thermal.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vesper {

class VulkanEngine;

enum class VideoCodec { Hevc = 0, Av1 = 1 };

struct RecorderConfig {
    int fd = -1;                // writable file descriptor; the recorder takes ownership and closes it
    int width = 1920, height = 1080;
    double fps = 24.0;
    VideoCodec codec = VideoCodec::Hevc;
    int64_t bitrate = 0;        // 0 = pick from resolution/fps
    bool audio = true;
    bool timestampRealtime = true; // camera timestamps are CLOCK_BOOTTIME (else CLOCK_MONOTONIC)
};

struct RecorderStatus {
    bool recording = false;
    int64_t durationUs = 0;
    int64_t framesEncoded = 0;
    int64_t framesDropped = 0;
    int32_t thermalStatus = 0;  // AThermalStatus
    bool audio = false;
    std::string codecName;
    std::string stopReason;     // "", "user", "thermal", "storage", "error: ..."
};

// Encodes the GPU's P010 output (BT.2020, limited range, Apple Log) with a
// hardware 10-bit encoder in ByteBuffer mode and muxes it with AAC audio.
//
// ByteBuffer/P010 input is used instead of an input Surface on purpose: we
// control the exact RGB->YCbCr matrix, range and 10-bit quantisation, and
// every frame carries its own sensor timestamp as PTS.
class Recorder {
public:
    Recorder() = default;
    ~Recorder() { stop("user"); }

    bool start(const RecorderConfig& config, VulkanEngine* gpu, std::string* error);
    // Camera thread: hand over a rendered slot. Always consumes the slot
    // (releases it immediately if the recorder isn't accepting frames).
    void submitVideoFrame(int slot, int64_t sensorTimestampNs);
    void stop(const std::string& reason);
    bool isRecording() const { return running_.load(); }
    RecorderStatus status();

private:
    struct VideoJob { int slot; int64_t timestampNs; };
    struct PendingSample { int track; std::vector<uint8_t> data; AMediaCodecBufferInfo info; };

    void threadMain();
    void encodeVideo(const VideoJob& job);
    void feedAudio(bool endOfStream);
    bool drain(AMediaCodec* codec, int& track, bool& eos, int64_t timeoutUs);
    void writeSample(int track, const uint8_t* data, const AMediaCodecBufferInfo& info);
    void checkGuards();
    void closeAll();

    static aaudio_data_callback_result_t audioCallback(AAudioStream*, void* user, void* data, int32_t frames);
    bool startAudio();

    RecorderConfig cfg_;
    VulkanEngine* gpu_ = nullptr;
    AMediaCodec* video_ = nullptr;
    AMediaCodec* audioCodec_ = nullptr;
    AMediaMuxer* muxer_ = nullptr;
    AAudioStream* audioStream_ = nullptr;
    AThermalManager* thermal_ = nullptr;
    int32_t inStride_ = 0, inSliceHeight_ = 0;

    int videoTrack_ = -1, audioTrack_ = -1;
    bool muxerStarted_ = false;
    std::vector<PendingSample> pending_;

    std::thread thread_;
    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    std::deque<VideoJob> jobs_;
    std::atomic<bool> running_{false};
    std::atomic<bool> accepting_{false};
    std::atomic<bool> stopRequested_{false};

    int64_t firstVideoNs_ = -1;
    int64_t lastVideoPtsUs_ = -1;

    // Audio SPSC ring (interleaved stereo int16), written by the AAudio callback.
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;
    std::vector<int16_t> ring_;
    std::atomic<uint64_t> ringWrite_{0};  // in frames
    uint64_t ringRead_ = 0;               // in frames, recorder thread only
    std::atomic<int64_t> firstAudioCallbackNs_{-1};
    int64_t audioBaseNs_ = -1;            // time of stream frame 0 in the camera clock

    std::mutex statusMutex_;
    RecorderStatus status_;
    int64_t lastGuardCheckNs_ = 0;
};

} // namespace vesper
