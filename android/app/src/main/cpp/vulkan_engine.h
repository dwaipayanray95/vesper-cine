#pragma once

#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>
#include "app_log.h"
#include <android/native_window.h>
#include <android/log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#define VK_TAG "Vesper_Vulkan"
#define VK_LOGI(...) vesperLog(ANDROID_LOG_INFO, VK_TAG, __VA_ARGS__)
#define VK_LOGW(...) vesperLog(ANDROID_LOG_WARN, VK_TAG, __VA_ARGS__)
#define VK_LOGE(...) vesperLog(ANDROID_LOG_ERROR, VK_TAG, __VA_ARGS__)

namespace vesper {

// Mirrors `FrameParams` in shaders/frame_params.glsl (std140, all 16-byte members).
struct FrameParams {
    float blackLevel[4];        // raw DN, logical R, Gr, Gb, B
    float wbGains[4];           // r, g, b, whiteLevel
    int32_t rawInfo[4];         // rawW, rawH, rowStrideBytes, cfaPattern
    int32_t quadInfo[4];        // quadW, quadH, shadingCols, shadingRows
    float cropRect[4];          // raw-space x0, y0, w, h
    int32_t outInfo[4];         // outW, outH, rotation, monitoringMode
    int32_t flags[4];           // writeP010, swapRB (set by the engine), writeViewfinder, sharpening 0-3
    float cfaOffsets[4];        // R sample centre (x, y), B sample centre (x, y), raw px within a quad
    float exposure[4];          // clipLinear, zebraThreshold, peakingThreshold, log2(clipLinear/0.18)
    float camToRec2020[12];     // 3 rows, each padded to vec4
    float sensorMap[4];         // raw px -> pre-correction array px: array = raw * xy + zw
    float arrayInfo[4];         // pre-correction array width, height
    float lensK[4];             // k1, k2, k3, enable
    float lensP[4];             // p1, p2
    float lensF[4];             // fx, fy, cx, cy
    float noise[4];             // temporal strength, chroma strength, noise S, noise O
    int32_t cleanFlags[4];      // hot-pixel fix, temporal NR, history valid (set by the engine), tile alignment
};
static_assert(sizeof(FrameParams) == 304, "FrameParams must match the std140 GLSL block");

struct FrameInput {
    const uint8_t* raw = nullptr;   // packed RAW10 plane, valid for the duration of processFrame
    size_t rawSize = 0;
    const float* shading = nullptr; // [rows][cols][4] gain map, or nullptr
    size_t shadingFloats = 0;
    FrameParams params{};
};

// Owns all GPU work. Threading contract:
//   processFrame()                           — camera thread only
//   waitEncoderFrame()/releaseEncoderFrame() — encoder thread
//   setViewfinderWindow()                    — any thread (applied on the next frame)
//   initialize()/release()                   — control thread, while no frames are flowing
class VulkanEngine {
public:
    static constexpr int kRingSize = 3;

    VulkanEngine() = default;
    ~VulkanEngine() { release(); }

    bool initialize();
    void release();

    void setViewfinderWindow(ANativeWindow* window);

    // Uploads, dispatches and presents one frame. When `encoderSlot` is non-null
    // the render pass also writes P010 into a per-slot buffer; the slot index is
    // returned there and stays reserved until releaseEncoderFrame(slot).
    // Returns false if the frame was dropped (GPU or encoder too far behind).
    bool processFrame(const FrameInput& in, int* encoderSlot);

    // Camera frame interval; used to keep optional GPU passes within budget.
    void setFrameBudgetMs(double ms);
    // GPU over budget with nothing left to pause: frames will drop at this frame rate.
    bool overloaded() const { return overloaded_; }
    // Next present starts a fresh pacing measurement (after a pause, not a hitch).
    void resetPacing() { pacingReset_ = true; }
    // Viewfinder magnifier: show 1/scale of the frame around (cx, cy) (normalised).
    void setViewfinderZoom(float cx, float cy, float scale) { zoomCx_ = cx; zoomCy_ = cy; zoomScale_ = scale; }
    // false: never pause alignment / HQ / NR automatically (frames may drop instead).
    void setBudgetGuard(bool enabled);
    bool oversamplingSupported() const { return hqSupported_; }
    bool oversamplingThrottled() const { return hqThrottled_; }
    // Averaged GPU time per frame (ms) from timestamp queries; 0 if unsupported.
    double gpuFrameMs() const { return gpuFrameMs_; }
    // True once alignment was switched off automatically because the GPU ran over budget.
    bool alignmentThrottled() const { return alignThrottled_; }
    // First guard step: the motion search runs every 4th frame instead of every 2nd.
    bool alignmentReduced() const { return alignReduced_ && !alignThrottled_; }
    // Second stage: temporal + chroma NR paused too because alignment alone wasn't enough.
    bool noiseReductionThrottled() const { return nrThrottled_; }
    // HQ oversampled luma: unsupported on this GPU, or paused by the budget guard.
    bool oversamplingAvailable() const { return hqSupported_ && !hqThrottled_; }
    // GPU benchmark A/B: opt-in optimisation experiments (kExp* bits), off in
    // normal use until the benchmark on the phone has confirmed them.
    static constexpr int kExpHqWide = 1;       // HQ on 32x16-quad tiles (64x32 px), halo shared; implies exact shading
    static constexpr int kExpHqExactShading = 32; // HQ: lens shading per raw pixel (quality fix) instead of per 32x32 block
    static constexpr int kExpRender16x8 = 2;   // render workgroup 16x8 (normally 8x8)
    static constexpr int kExpRender16x16 = 4;  // render workgroup 16x16
    static constexpr int kExpClean16x16 = 8;   // clean (NR) workgroup 16x16 (normally 16x8)
    static constexpr int kExpClean8x8 = 16;    // clean workgroup 8x8
    void setExperiments(int mask) { experiments_ = mask; }
    // GPU benchmark: run the motion search every n-th frame (2 = normal,
    // 4 = the guard's reduced rate); 0 = automatic (guard decides).
    void setAlignInterval(int n) { alignInterval_ = n; }
    // GPU benchmark pass-cost probe: dispatch one pass (or part of one) an
    // extra time per frame, same output, so the frame-time increase is its
    // cost. -1 off, 0 unpack, 1 HQ, 2 alignment, 3 clean (NR), 4 render,
    // 5 alignment 1/4-res luma only, 6 alignment search only,
    // 7 HQ raw tile load only, 8 HQ load + demosaic (no filter / output).
    void setRepeatPass(int pass) { repeatPass_ = pass; }

    // Blocks until the slot's GPU work is done, then returns its P010 bytes
    // (Y plane of outW*outH uint16, then interleaved CbCr of outW*outH/2 uint16).
    const uint8_t* waitEncoderFrame(int slot, size_t* size);
    void releaseEncoderFrame(int slot);

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
        bool coherent = true;
        uint32_t memType = 0;
    };
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct Slot {
        Buffer raw, shading, params, p010, vfReadback;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquireSem = VK_NULL_HANDLE;
        VkCommandBuffer presentCmd = VK_NULL_HANDLE; // viewfinder copy, recorded on the submit thread
        VkFence presentFence = VK_NULL_HANDLE;
        VkDescriptorSet unpackSet = VK_NULL_HANDLE;
        VkDescriptorSet cleanSets[2] = {};  // per ping-pong parity
        VkDescriptorSet alignSets[2] = {};
        VkDescriptorSet renderSets[2] = {};
        VkDescriptorSet greenSet = VK_NULL_HANDLE;
        bool vfReadbackPending = false;
        bool encoderHold = false; // guarded by encoderMutex_
        bool timed = false;       // timestamp queries were written for this slot's last frame
    };
    struct Geometry {
        int rawW = 0, rawH = 0, stride = 0, outW = 0, outH = 0, shadingFloats = 0;
        bool operator==(const Geometry&) const = default;
    };

    bool createInstanceAndDevice();
    bool createPipelines();
    bool ensureResources(const Geometry& g);
    void destroyResources();
    void drainAll();
    // Submission + presentation run on their own thread so the camera thread
    // never blocks in the driver (vkQueueSubmit/vkQueuePresentKHR can take
    // tens of ms on mobile GPUs when the queue is busy).
    struct SubmitJob {
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        int slot = -1;
        bool present = false; // copy the viewfinder image to the swapchain and present it
    };
    void presentViewfinder(Slot& s);
    void submitLoop();
    void startSubmitThread();
    void stopSubmitThread();
    void flushSubmits(); // waits until every queued job has been submitted

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostCached, Buffer& out);
    void destroyBuffer(Buffer& b);
    bool createImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, Image& out);
    void destroyImage(Image& img);
    int32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred);
    void writeDescriptors(Slot& s);
    void writeDescriptors(Slot& s, int par);
    const Image& pingPong(int par) const { return par ? historyImage_ : cleanImage_; }

    void applyPendingWindow();
    bool createSwapchain();
    void destroySwapchain();
    void presentCpuFallback(Slot& s);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    bool queueHasGraphics_ = false;
    bool haveSurfaceExt_ = false;
    bool haveSwapchainExt_ = false;
    VkPhysicalDeviceMemoryProperties memProps_{};

    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout unpackLayout_ = VK_NULL_HANDLE, cleanLayout_ = VK_NULL_HANDLE, renderLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout alignLayout_ = VK_NULL_HANDLE, greenLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout greenPipeLayout_ = VK_NULL_HANDLE;
    VkPipeline greenPipe_ = VK_NULL_HANDLE;
    VkPipelineLayout alignPipeLayout_ = VK_NULL_HANDLE;
    VkPipeline alignPipe_ = VK_NULL_HANDLE;
    VkPipelineLayout unpackPipeLayout_ = VK_NULL_HANDLE, cleanPipeLayout_ = VK_NULL_HANDLE, renderPipeLayout_ = VK_NULL_HANDLE;
    VkPipeline unpackPipe_ = VK_NULL_HANDLE, cleanPipe_ = VK_NULL_HANDLE, renderPipe_ = VK_NULL_HANDLE;
    std::atomic<int> experiments_{0}, repeatPass_{-1}, alignInterval_{0};
    // Benchmark experiments / probes (VK_NULL_HANDLE where the variant doesn't apply: HQ ones need fp16 + R16F).
    VkPipeline greenProbe1Pipe_ = VK_NULL_HANDLE, greenProbe2Pipe_ = VK_NULL_HANDLE, greenWidePipe_ = VK_NULL_HANDLE;
    VkPipeline greenExactPipe_ = VK_NULL_HANDLE;
    VkPipeline render16x8Pipe_ = VK_NULL_HANDLE, render16x16Pipe_ = VK_NULL_HANDLE;
    VkPipeline clean16x16Pipe_ = VK_NULL_HANDLE, clean8x8Pipe_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    Slot slots_[kRingSize];
    Image quadImage_, cleanImage_, historyImage_, vfImage_; // clean/history: temporal NR ping-pong pair
    int pingParity_ = 0;
    bool fp16_ = false; // shaderFloat16 enabled: HQ pass uses the 16-bit variant
    Image curLowImage_, histLowImage_, motionImage_; // tile alignment (align.comp)
    Image greenImage_;                              // full-res green (green.comp), HQ oversampling
    bool hqSupported_ = false;                      // a storage + linearly filterable format for greenImage_
    VkFormat hqFormat_ = VK_FORMAT_R16_SFLOAT;      // R16F, or RGBA16F on GPUs without R16F storage
    bool hqThrottled_ = false;
    // Budget guard: throttled stages come back once there is headroom again.
    bool guardEnabled_ = true;
    int graceFrames_ = 0;       // ignore the first frames after (re)allocation: always slow
    int underBudgetFrames_ = 0;
    std::atomic<bool> overloaded_{false};
    bool recordingNow_ = false;
    // Guard stages: 0 alignment off, 1 HQ off, 2 NR off, 3 alignment at reduced rate.
    // Paused in the order 3, 0, 1, 2 and restored in reverse.
    double stageCostMs_[4] = {6.0, 12.0, 6.0, 1.5}; // measured when paused (initial guesses)
    bool costReliable_[4] = {false, false, false, false}; // measured by a restore (under budget)
    bool alignReduced_ = false;   // stage 3
    bool alignRequested_ = false; // temporal NR with alignment is on: the alignment stages can save something
    bool measuringRestore_ = false;
    int measuringStage_ = -1, lastRestored_ = -1;
    double costBeforeMs_ = 0;
    int recoverFrames_ = 24;   // under-budget frames needed before re-enabling a stage (doubles on flapping)
    bool historyValid_ = false;

    // GPU timing: kStamps timestamps per slot (start, unpack, align, clean, end).
    static constexpr uint32_t kStamps = 5;
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    double timestampPeriodNs_ = 0;
    double passMs_[kStamps - 1] = {};
    int timedFrames_ = 0;
    double gpuFrameMs_ = 0;
    std::atomic<double> frameBudgetMs_{41.7};
    int overBudgetFrames_ = 0;
    bool alignThrottled_ = false;
    bool nrThrottled_ = false;
    void readTimestamps(int slot);
    Geometry geom_;
    int nextSlot_ = 0;

    // Viewfinder
    std::mutex windowMutex_;
    ANativeWindow* pendingWindow_ = nullptr;
    bool windowChanged_ = false;
    ANativeWindow* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapImages_;
    std::vector<VkSemaphore> swapRenderDone_;
    VkExtent2D swapExtent_{};
    bool swapRB_ = false;
    std::atomic<bool> swapchainStale_{false};
    bool cpuFallback_ = false;

    std::mutex encoderMutex_;
    std::condition_variable encoderCv_;

    bool motionValid_ = false;
    uint32_t alignTick_ = 0;
    std::thread submitThread_;
    std::mutex submitMutex_;
    std::condition_variable submitCv_;
    std::deque<SubmitJob> submitJobs_;
    bool submitBusy_ = false, submitStop_ = false;
    std::mutex queueMutex_; // vkQueue* calls (queue is externally synchronised)
    VkCommandPool presentPool_ = VK_NULL_HANDLE; // used only by the submit thread
    std::atomic<uint64_t> vfSkipped_{0};
    std::atomic<float> zoomCx_{0.5f}, zoomCy_{0.5f}, zoomScale_{1.0f};
    std::atomic<uint64_t> vfHitches_{0};
    std::atomic<bool> pacingReset_{false};
    std::atomic<double> vfMaxGapMs_{0.0};
    std::chrono::steady_clock::time_point lastPresent_{}; // submit thread only
    double maxWaitMs_ = 0;
    std::mutex frameMutex_; // serialises processFrame against release()
    bool initialized_ = false;
    uint64_t frameCount_ = 0;
    uint64_t droppedFrames_ = 0;
    double accCopyMs_ = 0, accWaitMs_ = 0, accSubmitMs_ = 0;
};

} // namespace vesper
