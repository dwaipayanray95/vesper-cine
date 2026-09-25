#include "recorder.h"
#include "vulkan_engine.h"

#include <android/log.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>

#define REC_TAG "Vesper_Recorder"
#define RLOGI(...) __android_log_print(ANDROID_LOG_INFO, REC_TAG, __VA_ARGS__)
#define RLOGW(...) __android_log_print(ANDROID_LOG_WARN, REC_TAG, __VA_ARGS__)
#define RLOGE(...) __android_log_print(ANDROID_LOG_ERROR, REC_TAG, __VA_ARGS__)

namespace vesper {

namespace {

constexpr int32_t kColorFormatYuvP010 = 54;       // MediaCodecInfo.CodecCapabilities.COLOR_FormatYUVP010
constexpr int32_t kColorStandardBt2020 = 6;       // MediaFormat.COLOR_STANDARD_BT2020
constexpr int32_t kColorRangeLimited = 2;         // MediaFormat.COLOR_RANGE_LIMITED
constexpr int32_t kBitrateModeVbr = 1;            // BITRATE_MODE_VBR
constexpr int32_t kHevcProfileMain10 = 2;         // HEVCProfileMain10
constexpr int32_t kAv1ProfileMain10 = 2;          // AV1ProfileMain10
constexpr int32_t kAacLc = 2;
constexpr int64_t kMinFreeBytes = 500LL * 1024 * 1024;

int64_t clockNs(clockid_t clock) {
    timespec ts{};
    clock_gettime(clock, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

} // namespace

bool Recorder::start(const RecorderConfig& config, VulkanEngine* gpu, std::string* error) {
    stop("user");
    cfg_ = config;
    gpu_ = gpu;
    auto fail = [&](const std::string& why) {
        RLOGE("%s", why.c_str());
        if (error) *error = why;
        closeAll();
        return false;
    };
    if (cfg_.fd < 0 || !gpu_) return fail("invalid output file");

    if (cfg_.bitrate <= 0) {
        // ~0.9 bits/pixel/frame for HEVC Log masters (1080p24 ~45 Mb/s, 1080p60 ~112, UHD30 ~224).
        // AV1 needs roughly 30% less for the same quality.
        double bpp = cfg_.codec == VideoCodec::Av1 ? 0.63 : 0.9;
        cfg_.bitrate = std::min<int64_t>(static_cast<int64_t>(cfg_.width * cfg_.height * cfg_.fps * bpp), 240'000'000);
    }

    const char* mime = cfg_.codec == VideoCodec::Av1 ? "video/av01" : "video/hevc";
    video_ = AMediaCodec_createEncoderByType(mime);
    if (!video_ && cfg_.codec == VideoCodec::Av1) {
        RLOGW("No AV1 encoder on this device, falling back to HEVC");
        cfg_.codec = VideoCodec::Hevc;
        mime = "video/hevc";
        video_ = AMediaCodec_createEncoderByType(mime);
    }
    if (!video_) return fail("no video encoder");

    AMediaFormat* vf = AMediaFormat_new();
    AMediaFormat_setString(vf, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(vf, AMEDIAFORMAT_KEY_WIDTH, cfg_.width);
    AMediaFormat_setInt32(vf, AMEDIAFORMAT_KEY_HEIGHT, cfg_.height);
    AMediaFormat_setInt32(vf, "color-format", kColorFormatYuvP010);
    AMediaFormat_setInt32(vf, "profile", cfg_.codec == VideoCodec::Av1 ? kAv1ProfileMain10 : kHevcProfileMain10);
    AMediaFormat_setInt32(vf, "bitrate", static_cast<int32_t>(cfg_.bitrate));
    AMediaFormat_setInt32(vf, "bitrate-mode", kBitrateModeVbr);
    AMediaFormat_setFloat(vf, "frame-rate", static_cast<float>(cfg_.fps));
    AMediaFormat_setInt32(vf, "i-frame-interval", 1);
    AMediaFormat_setInt32(vf, "priority", 0); // realtime
    // BT.2020 primaries + matrix, limited range. There is no code point for
    // Apple Log, so transfer is left unspecified (as on iPhone): set
    // "Apple Log" as the input colour space in Resolve/Premiere/FCP.
    AMediaFormat_setInt32(vf, "color-standard", kColorStandardBt2020);
    AMediaFormat_setInt32(vf, "color-range", kColorRangeLimited);
    media_status_t st = AMediaCodec_configure(video_, vf, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(vf);
    if (st != AMEDIA_OK) return fail("10-bit P010 encoder configuration rejected (" + std::string(mime) + ")");

    AMediaFormat* inFmt = AMediaCodec_getInputFormat(video_);
    if (inFmt) {
        AMediaFormat_getInt32(inFmt, "stride", &inStride_);
        AMediaFormat_getInt32(inFmt, "slice-height", &inSliceHeight_);
        AMediaFormat_delete(inFmt);
    }
    if (inStride_ <= 0) inStride_ = cfg_.width * 2;
    if (inStride_ < cfg_.width * 2) inStride_ *= 2; // reported in pixels, P010 is 2 bytes/px
    if (inSliceHeight_ < cfg_.height) inSliceHeight_ = cfg_.height;

    char* name = nullptr;
    std::string codecName = mime;
    if (AMediaCodec_getName(video_, &name) == AMEDIA_OK && name) {
        codecName = name;
        AMediaCodec_releaseName(video_, name);
    }

    bool audioOk = cfg_.audio && startAudio();

    muxer_ = AMediaMuxer_new(cfg_.fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!muxer_) return fail("could not create MP4 muxer");
    if (AMediaCodec_start(video_) != AMEDIA_OK) return fail("video encoder failed to start");
    if (audioCodec_ && AMediaCodec_start(audioCodec_) != AMEDIA_OK) {
        RLOGW("AAC encoder failed to start — recording without audio");
        AMediaCodec_delete(audioCodec_);
        audioCodec_ = nullptr;
        audioOk = false;
    }
    if (audioStream_ && (!audioOk || AAudioStream_requestStart(audioStream_) != AAUDIO_OK)) {
        AAudioStream_close(audioStream_);
        audioStream_ = nullptr;
        if (audioCodec_) { AMediaCodec_stop(audioCodec_); AMediaCodec_delete(audioCodec_); audioCodec_ = nullptr; }
        audioOk = false;
    }

    thermal_ = AThermal_acquireManager();
    videoTrack_ = audioTrack_ = -1;
    muxerStarted_ = false;
    pending_.clear();
    firstVideoNs_ = -1;
    lastVideoPtsUs_ = -1;
    audioBaseNs_ = -1;
    lastGuardCheckNs_ = 0;
    {
        std::lock_guard<std::mutex> lk(statusMutex_);
        status_ = RecorderStatus{};
        status_.recording = true;
        status_.audio = audioOk;
        status_.codecName = codecName;
    }
    stopRequested_ = false;
    running_ = true;
    accepting_ = true;
    thread_ = std::thread(&Recorder::threadMain, this);
    RLOGI("Recording %dx%d @ %.3f fps, %s, %.0f Mb/s, audio=%d, input stride %d slice %d",
          cfg_.width, cfg_.height, cfg_.fps, codecName.c_str(), cfg_.bitrate / 1e6, audioOk, inStride_, inSliceHeight_);
    return true;
}

bool Recorder::startAudio() {
    ring_.assign(static_cast<size_t>(kSampleRate) * kChannels * 4, 0); // 4 s of headroom
    ringWrite_ = 0;
    ringRead_ = 0;
    firstAudioCallbackNs_ = -1;

    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(b, kSampleRate);
    AAudioStreamBuilder_setChannelCount(b, kChannels);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_CAMCORDER);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(b, &Recorder::audioCallback, this);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &audioStream_);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !audioStream_) {
        RLOGW("Microphone unavailable (%d) — recording without audio", r);
        audioStream_ = nullptr;
        return false;
    }
    if (AAudioStream_getSampleRate(audioStream_) != kSampleRate || AAudioStream_getChannelCount(audioStream_) != kChannels) {
        RLOGW("Microphone opened with unexpected format — recording without audio");
        AAudioStream_close(audioStream_);
        audioStream_ = nullptr;
        return false;
    }

    audioCodec_ = AMediaCodec_createEncoderByType("audio/mp4a-latm");
    if (!audioCodec_) return false;
    AMediaFormat* af = AMediaFormat_new();
    AMediaFormat_setString(af, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(af, AMEDIAFORMAT_KEY_SAMPLE_RATE, kSampleRate);
    AMediaFormat_setInt32(af, AMEDIAFORMAT_KEY_CHANNEL_COUNT, kChannels);
    AMediaFormat_setInt32(af, "bitrate", 320000);
    AMediaFormat_setInt32(af, "aac-profile", kAacLc);
    AMediaFormat_setInt32(af, "max-input-size", 16384);
    media_status_t st = AMediaCodec_configure(audioCodec_, af, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(af);
    if (st != AMEDIA_OK) {
        AMediaCodec_delete(audioCodec_);
        audioCodec_ = nullptr;
        return false;
    }
    return true;
}

aaudio_data_callback_result_t Recorder::audioCallback(AAudioStream*, void* user, void* data, int32_t frames) {
    auto* self = static_cast<Recorder*>(user);
    if (self->firstAudioCallbackNs_.load(std::memory_order_relaxed) < 0) {
        self->firstAudioCallbackNs_ = clockNs(self->cfg_.timestampRealtime ? CLOCK_BOOTTIME : CLOCK_MONOTONIC);
    }
    const auto* src = static_cast<const int16_t*>(data);
    const size_t capFrames = self->ring_.size() / kChannels;
    uint64_t w = self->ringWrite_.load(std::memory_order_relaxed);
    for (int32_t i = 0; i < frames; ++i) {
        size_t at = static_cast<size_t>((w + i) % capFrames) * kChannels;
        self->ring_[at] = src[i * kChannels];
        self->ring_[at + 1] = src[i * kChannels + 1];
    }
    self->ringWrite_.store(w + frames, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void Recorder::submitVideoFrame(int slot, int64_t ts) {
    if (!accepting_) {
        if (gpu_) gpu_->releaseEncoderFrame(slot);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(jobMutex_);
        jobs_.push_back({slot, ts});
    }
    jobCv_.notify_one();
}

void Recorder::stop(const std::string& reason) {
    if (!running_) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(statusMutex_);
        if (status_.stopReason.empty()) status_.stopReason = reason;
    }
    stopRequested_ = true;
    jobCv_.notify_all();
    if (thread_.joinable() && std::this_thread::get_id() != thread_.get_id()) thread_.join();
}

RecorderStatus Recorder::status() {
    std::lock_guard<std::mutex> lk(statusMutex_);
    return status_;
}

void Recorder::threadMain() {
    bool videoEos = false, audioEos = audioCodec_ == nullptr;
    bool eosQueued = false;
    while (true) {
        VideoJob job{-1, 0};
        {
            std::unique_lock<std::mutex> lk(jobMutex_);
            jobCv_.wait_for(lk, std::chrono::milliseconds(5), [&] { return !jobs_.empty() || stopRequested_; });
            if (!jobs_.empty()) { job = jobs_.front(); jobs_.pop_front(); }
        }
        if (job.slot >= 0) encodeVideo(job);

        bool stopping = stopRequested_.load();
        if (stopping) accepting_ = false;
        bool jobsLeft;
        { std::lock_guard<std::mutex> lk(jobMutex_); jobsLeft = !jobs_.empty(); }

        if (stopping && !jobsLeft && !eosQueued) {
            ssize_t idx = AMediaCodec_dequeueInputBuffer(video_, 50'000);
            if (idx >= 0) {
                AMediaCodec_queueInputBuffer(video_, static_cast<size_t>(idx), 0, 0,
                                             static_cast<uint64_t>(std::max<int64_t>(lastVideoPtsUs_ + 1, 0)),
                                             AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                if (audioCodec_) feedAudio(true);
                eosQueued = true;
            }
        } else if (audioCodec_ && !eosQueued) {
            feedAudio(false);
        }

        if (!videoEos) drain(video_, videoTrack_, videoEos, eosQueued ? 10'000 : 0);
        if (!audioEos) drain(audioCodec_, audioTrack_, audioEos, 0);
        if (eosQueued && videoEos && audioEos) break;
        if (!stopping) checkGuards();
    }
    closeAll();
    running_ = false;
    std::lock_guard<std::mutex> lk(statusMutex_);
    status_.recording = false;
    RLOGI("Recording finished: %lld frames, %lld dropped, %.2fs, reason=%s",
          static_cast<long long>(status_.framesEncoded), static_cast<long long>(status_.framesDropped),
          status_.durationUs / 1e6, status_.stopReason.c_str());
}

void Recorder::encodeVideo(const VideoJob& job) {
    auto drop = [&] {
        gpu_->releaseEncoderFrame(job.slot);
        std::lock_guard<std::mutex> lk(statusMutex_);
        ++status_.framesDropped;
    };
    if (firstVideoNs_ < 0) firstVideoNs_ = job.timestampNs;
    int64_t ptsUs = (job.timestampNs - firstVideoNs_) / 1000;
    if (ptsUs <= lastVideoPtsUs_) { drop(); return; }

    ssize_t idx = AMediaCodec_dequeueInputBuffer(video_, 30'000);
    if (idx < 0) { drop(); return; }
    size_t cap = 0;
    uint8_t* dst = AMediaCodec_getInputBuffer(video_, static_cast<size_t>(idx), &cap);
    size_t srcSize = 0;
    const uint8_t* src = gpu_->waitEncoderFrame(job.slot, &srcSize);
    const size_t rowBytes = static_cast<size_t>(cfg_.width) * 2;
    const size_t stride = static_cast<size_t>(inStride_);
    const size_t need = stride * inSliceHeight_ + stride * (cfg_.height / 2);
    if (!dst || !src || cap < need || srcSize < rowBytes * cfg_.height * 3 / 2) {
        AMediaCodec_queueInputBuffer(video_, static_cast<size_t>(idx), 0, 0, static_cast<uint64_t>(ptsUs), 0);
        drop();
        return;
    }
    const uint8_t* srcUv = src + rowBytes * cfg_.height;
    uint8_t* dstUv = dst + stride * inSliceHeight_;
    if (stride == rowBytes && static_cast<size_t>(inSliceHeight_) == static_cast<size_t>(cfg_.height)) {
        std::memcpy(dst, src, rowBytes * cfg_.height * 3 / 2);
    } else {
        for (int y = 0; y < cfg_.height; ++y) std::memcpy(dst + y * stride, src + y * rowBytes, rowBytes);
        for (int y = 0; y < cfg_.height / 2; ++y) std::memcpy(dstUv + y * stride, srcUv + y * rowBytes, rowBytes);
    }
    gpu_->releaseEncoderFrame(job.slot);
    AMediaCodec_queueInputBuffer(video_, static_cast<size_t>(idx), 0, need, static_cast<uint64_t>(ptsUs), 0);
    lastVideoPtsUs_ = ptsUs;

    std::lock_guard<std::mutex> lk(statusMutex_);
    ++status_.framesEncoded;
    status_.durationUs = ptsUs;
}

void Recorder::feedAudio(bool endOfStream) {
    if (!audioCodec_) return;
    if (firstVideoNs_ < 0 && !endOfStream) {
        // Nothing to sync to yet: discard captured audio so it starts with the picture.
        ringRead_ = ringWrite_.load(std::memory_order_acquire);
        return;
    }
    const int64_t clock = firstAudioCallbackNs_.load();
    if (audioBaseNs_ < 0 && clock >= 0 && audioStream_) {
        int64_t framePos = 0, timeNs = 0;
        if (AAudioStream_getTimestamp(audioStream_, cfg_.timestampRealtime ? CLOCK_BOOTTIME : CLOCK_MONOTONIC,
                                      &framePos, &timeNs) == AAUDIO_OK) {
            audioBaseNs_ = timeNs - framePos * 1'000'000'000LL / kSampleRate;
        } else {
            audioBaseNs_ = clock; // callback time of frame 0 — good to a few ms
        }
    }
    constexpr int kChunk = 1024;
    const size_t capFrames = ring_.size() / kChannels;
    while (true) {
        uint64_t avail = ringWrite_.load(std::memory_order_acquire) - ringRead_;
        if (avail > capFrames) { ringRead_ = ringWrite_.load() - capFrames; avail = capFrames; } // overrun
        if (avail < static_cast<uint64_t>(kChunk) && !(endOfStream && avail > 0)) break;
        int frames = static_cast<int>(std::min<uint64_t>(avail, kChunk));
        int64_t ptsNs = audioBaseNs_ + static_cast<int64_t>(ringRead_) * 1'000'000'000LL / kSampleRate - firstVideoNs_;
        if (ptsNs < 0) { ringRead_ += frames; continue; } // captured before the first video frame
        ssize_t idx = AMediaCodec_dequeueInputBuffer(audioCodec_, 0);
        if (idx < 0) break;
        size_t cap = 0;
        uint8_t* dst = AMediaCodec_getInputBuffer(audioCodec_, static_cast<size_t>(idx), &cap);
        frames = std::min<int>(frames, static_cast<int>(cap / (kChannels * sizeof(int16_t))));
        auto* out = reinterpret_cast<int16_t*>(dst);
        for (int i = 0; i < frames; ++i) {
            size_t at = static_cast<size_t>((ringRead_ + i) % capFrames) * kChannels;
            out[i * 2] = ring_[at];
            out[i * 2 + 1] = ring_[at + 1];
        }
        ringRead_ += frames;
        AMediaCodec_queueInputBuffer(audioCodec_, static_cast<size_t>(idx), 0, frames * kChannels * sizeof(int16_t),
                                     static_cast<uint64_t>(ptsNs / 1000), 0);
    }
    if (endOfStream) {
        ssize_t idx = AMediaCodec_dequeueInputBuffer(audioCodec_, 50'000);
        if (idx >= 0) {
            int64_t ptsUs = std::max<int64_t>(0, (audioBaseNs_ + static_cast<int64_t>(ringRead_) * 1'000'000'000LL / kSampleRate - firstVideoNs_) / 1000);
            AMediaCodec_queueInputBuffer(audioCodec_, static_cast<size_t>(idx), 0, 0, static_cast<uint64_t>(ptsUs),
                                         AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        }
        if (audioStream_) AAudioStream_requestStop(audioStream_);
    }
}

bool Recorder::drain(AMediaCodec* codec, int& track, bool& eos, int64_t timeoutUs) {
    while (true) {
        AMediaCodecBufferInfo info{};
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec, &info, timeoutUs);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return true;
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* fmt = AMediaCodec_getOutputFormat(codec);
            track = static_cast<int>(AMediaMuxer_addTrack(muxer_, fmt));
            AMediaFormat_delete(fmt);
            bool audioReady = audioCodec_ == nullptr || audioTrack_ >= 0;
            if (videoTrack_ >= 0 && audioReady && !muxerStarted_) {
                if (AMediaMuxer_start(muxer_) == AMEDIA_OK) {
                    muxerStarted_ = true;
                    for (auto& p : pending_) AMediaMuxer_writeSampleData(muxer_, static_cast<size_t>(p.track), p.data.data(), &p.info);
                    pending_.clear();
                } else {
                    stop("error: muxer start failed");
                }
            }
            continue;
        }
        if (idx < 0) return true; // OUTPUT_BUFFERS_CHANGED etc.
        size_t size = 0;
        uint8_t* data = AMediaCodec_getOutputBuffer(codec, static_cast<size_t>(idx), &size);
        bool config = (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0;
        if (data && info.size > 0 && !config && track >= 0) writeSample(track, data + info.offset, info);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) eos = true;
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(idx), false);
        if (eos) return true;
        timeoutUs = 0;
    }
}

void Recorder::writeSample(int track, const uint8_t* data, const AMediaCodecBufferInfo& info) {
    AMediaCodecBufferInfo i = info;
    i.offset = 0;
    if (!muxerStarted_) {
        pending_.push_back({track, std::vector<uint8_t>(data, data + info.size), i});
        return;
    }
    AMediaMuxer_writeSampleData(muxer_, static_cast<size_t>(track), data, &i);
}

// Thermal and storage guards, checked once a second: stop cleanly (like the
// stock camera) rather than let the OS throttle us into dropped frames.
void Recorder::checkGuards() {
    int64_t now = clockNs(CLOCK_MONOTONIC);
    if (now - lastGuardCheckNs_ < 1'000'000'000LL) return;
    lastGuardCheckNs_ = now;
    int32_t thermal = thermal_ ? static_cast<int32_t>(AThermal_getCurrentThermalStatus(thermal_)) : 0;
    {
        std::lock_guard<std::mutex> lk(statusMutex_);
        status_.thermalStatus = thermal;
    }
    if (thermal >= ATHERMAL_STATUS_SEVERE) {
        RLOGW("Thermal status %d — stopping recording", thermal);
        stop("thermal");
        return;
    }
    struct statvfs vfs{};
    if (fstatvfs(cfg_.fd, &vfs) == 0 && static_cast<int64_t>(vfs.f_bavail) * static_cast<int64_t>(vfs.f_frsize) < kMinFreeBytes) {
        RLOGW("Storage nearly full — stopping recording");
        stop("storage");
    }
}

void Recorder::closeAll() {
    accepting_ = false;
    {
        // Release any frames still queued so the GPU ring isn't starved.
        std::lock_guard<std::mutex> lk(jobMutex_);
        for (auto& j : jobs_) if (gpu_) gpu_->releaseEncoderFrame(j.slot);
        jobs_.clear();
    }
    if (audioStream_) { AAudioStream_requestStop(audioStream_); AAudioStream_close(audioStream_); audioStream_ = nullptr; }
    if (video_) { AMediaCodec_stop(video_); AMediaCodec_delete(video_); video_ = nullptr; }
    if (audioCodec_) { AMediaCodec_stop(audioCodec_); AMediaCodec_delete(audioCodec_); audioCodec_ = nullptr; }
    if (muxer_) {
        if (muxerStarted_) AMediaMuxer_stop(muxer_);
        AMediaMuxer_delete(muxer_);
        muxer_ = nullptr;
    }
    muxerStarted_ = false;
    if (thermal_) { AThermal_releaseManager(thermal_); thermal_ = nullptr; }
    if (cfg_.fd >= 0) { fsync(cfg_.fd); close(cfg_.fd); cfg_.fd = -1; }
}

} // namespace vesper
