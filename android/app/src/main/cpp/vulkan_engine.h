#pragma once

#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <android/log.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#define VK_TAG "Vesper_Vulkan"
#define VK_LOGI(...) __android_log_print(ANDROID_LOG_INFO, VK_TAG, __VA_ARGS__)
#define VK_LOGW(...) __android_log_print(ANDROID_LOG_WARN, VK_TAG, __VA_ARGS__)
#define VK_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, VK_TAG, __VA_ARGS__)

namespace vesper {

// Mirrors `FrameParams` in shaders/frame_params.glsl (std140, all 16-byte members).
struct FrameParams {
    float blackLevel[4];        // raw DN, logical R, Gr, Gb, B
    float wbGains[4];           // r, g, b, whiteLevel
    int32_t rawInfo[4];         // rawW, rawH, rowStrideBytes, cfaPattern
    int32_t quadInfo[4];        // quadW, quadH, shadingCols, shadingRows
    float cropRect[4];          // raw-space x0, y0, w, h
    int32_t outInfo[4];         // outW, outH, rotation, monitoringMode
    int32_t flags[4];           // writeP010, swapRB (set by the engine), writeViewfinder, 0
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
    void setFrameBudgetMs(double ms) { frameBudgetMs_ = ms; }
    // Averaged GPU time per frame (ms) from timestamp queries; 0 if unsupported.
    double gpuFrameMs() const { return gpuFrameMs_; }
    // True once alignment was switched off automatically because the GPU ran over budget.
    bool alignmentThrottled() const { return alignThrottled_; }

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
        VkDescriptorSet unpackSet = VK_NULL_HANDLE;
        VkDescriptorSet cleanSet = VK_NULL_HANDLE;
        VkDescriptorSet alignSet = VK_NULL_HANDLE;
        VkDescriptorSet renderSet = VK_NULL_HANDLE;
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

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostCached, Buffer& out);
    void destroyBuffer(Buffer& b);
    bool createImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, Image& out);
    void destroyImage(Image& img);
    int32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred);
    void writeDescriptors(Slot& s);

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
    VkDescriptorSetLayout alignLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout alignPipeLayout_ = VK_NULL_HANDLE;
    VkPipeline alignPipe_ = VK_NULL_HANDLE;
    VkPipelineLayout unpackPipeLayout_ = VK_NULL_HANDLE, cleanPipeLayout_ = VK_NULL_HANDLE, renderPipeLayout_ = VK_NULL_HANDLE;
    VkPipeline unpackPipe_ = VK_NULL_HANDLE, cleanPipe_ = VK_NULL_HANDLE, renderPipe_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    Slot slots_[kRingSize];
    Image quadImage_, cleanImage_, historyImage_, vfImage_;
    Image curLowImage_, histLowImage_, motionImage_; // tile alignment (align.comp)
    bool historyValid_ = false;

    // GPU timing: kStamps timestamps per slot (start, unpack, align, clean, end).
    static constexpr uint32_t kStamps = 5;
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    double timestampPeriodNs_ = 0;
    double passMs_[kStamps - 1] = {};
    int timedFrames_ = 0;
    double gpuFrameMs_ = 0;
    double frameBudgetMs_ = 41.7;
    int overBudgetFrames_ = 0;
    bool alignThrottled_ = false;
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
    bool swapchainStale_ = false;
    bool cpuFallback_ = false;

    std::mutex encoderMutex_;
    std::condition_variable encoderCv_;

    std::mutex frameMutex_; // serialises processFrame against release()
    bool initialized_ = false;
    uint64_t frameCount_ = 0;
    uint64_t droppedFrames_ = 0;
    double accCopyMs_ = 0, accWaitMs_ = 0, accSubmitMs_ = 0;
};

} // namespace vesper
