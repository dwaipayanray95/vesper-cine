#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/log.h>

#include <vector>
#include <mutex>
#include <memory>
#include <string>

#define TAG_VK "RCamera_Vulkan"
#define VK_LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG_VK, __VA_ARGS__)
#define VK_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG_VK, __VA_ARGS__)

namespace rcamera {

// Mirrors the `UniformData` push_constant block in shaders/mhc_rlog.comp
// byte-for-byte (std430 push-constant packing rules: each mat3 column is
// vec4-aligned, i.e. 3 columns x 16 bytes = 48 bytes per matrix, not 9
// packed floats). Only 3 of every 4 floats in *Matrix are used per column;
// the 4th is padding required by the GPU-side layout.
struct ComputeUniformData {
    float blackLevel[4];           // R, Gr, Gb, B                — offset 0
    float wbGains[4];              // rGain, gGain, bGain, 0.0     — offset 16
    float sensorToXyzMatrix[12];   // mat3, column-major, padded   — offset 32
    float xyzToRec2020Matrix[12];  // mat3, column-major, padded   — offset 80
    int32_t rawWidth;              //                              — offset 128
    int32_t rawHeight;             //                              — offset 132
    int32_t outputWidth;           //                              — offset 136
    int32_t outputHeight;          //                              — offset 140
    int32_t cropMode;              // 0: 16:9, 1: 4:3 open-gate    — offset 144
    int32_t monitoringMode;        // 0..4                         — offset 148
    float zebraThreshold;          // e.g. 0.95                    — offset 152
    float peakingThreshold;        // e.g. 0.15                    — offset 156
    float whiteLevel;              // e.g. 1023.0                  — offset 160
    int32_t sensorOrientation;     // 0/90/180/270 clockwise       — offset 164
};

// Packs a plain row-major 3x3 (9 floats, src9[row*3+col]) into the 12-float,
// column-padded layout the compute shader's `mat3` push-constant field
// expects. GLSL mat3 is column-major, so `M * v` requires storage column j
// to hold (M[0][j], M[1][j], M[2][j]) — this transposes src9 while packing
// so that `mat3(dst12...) * v` reproduces the intended row-major M * v.
inline void PackMat3ForPushConstant(const float src9[9], float dst12[12]) {
    dst12[0] = src9[0]; dst12[1] = src9[3]; dst12[2] = src9[6]; dst12[3] = 0.0f;
    dst12[4] = src9[1]; dst12[5] = src9[4]; dst12[6] = src9[7]; dst12[7] = 0.0f;
    dst12[8] = src9[2]; dst12[9] = src9[5]; dst12[10] = src9[8]; dst12[11] = 0.0f;
}

class VulkanComputeEngine {
public:
    VulkanComputeEngine();
    ~VulkanComputeEngine();

    bool initialize(ANativeWindow* codecWindow, ANativeWindow* viewfinderWindow);
    void release();

    // Process a raw frame from AHardwareBuffer: debayers + grades it on the
    // GPU and presents the WYSIWYG monitoring output straight to the
    // viewfinder ANativeWindow. Takes ownership of one reference on
    // hwBuffer (releases it before returning).
    bool processRawFrame(AHardwareBuffer* hwBuffer, const ComputeUniformData& uniforms);

    void setOutputDimensions(int32_t width, int32_t height);

    // Called from the JNI bridge whenever Flutter's SurfaceProducer texture
    // is (re)created or torn down. Acquires/releases the window and
    // (re)configures its buffer geometry to match the compute output.
    void setViewfinderWindow(ANativeWindow* window);

private:
    bool initInstance();
    bool initDevice();
    bool createComputePipeline(VkSampler immutableRawSampler);
    bool createOutputImages();
    void destroyOutputImages();

    // Lazily imports the AHardwareBuffer's opaque/external format as a
    // VkSamplerYcbcrConversion + immutable VkSampler the first time a frame
    // arrives (or whenever the buffer's external format changes), then
    // rebuilds the descriptor set layout / pipeline layout / pipeline to
    // bind it, since ycbcr conversion samplers must be immutable.
    bool ensureExternalFormatResources(AHardwareBuffer* hwBuffer);

    // Imports one frame's AHardwareBuffer as a transient sampled VkImage,
    // updates descriptor binding 0, and frees the previous frame's import.
    bool importFrameImage(AHardwareBuffer* hwBuffer, int32_t width, int32_t height);
    void releaseFrameImage();

    // Copies the viewfinder storage image to the host-visible staging
    // buffer and blits it into the ANativeWindow's next buffer.
    bool presentViewfinder();

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue computeQueue_ = VK_NULL_HANDLE;
    uint32_t computeQueueFamilyIndex_ = 0;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence frameFence_ = VK_NULL_HANDLE;

    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline computePipeline_ = VK_NULL_HANDLE;

    // Raw sensor input: per-frame imported AHardwareBuffer (binding 0)
    VkSamplerYcbcrConversion rawYcbcrConversion_ = VK_NULL_HANDLE;
    VkSampler rawImmutableSampler_ = VK_NULL_HANDLE;
    uint64_t cachedExternalFormat_ = 0;
    VkImage rawFrameImage_ = VK_NULL_HANDLE;
    VkDeviceMemory rawFrameMemory_ = VK_NULL_HANDLE;
    VkImageView rawFrameView_ = VK_NULL_HANDLE;

    // Output 1: clean R-Log master feeding AMediaCodec (binding 1)
    VkImage codecImage_ = VK_NULL_HANDLE;
    VkDeviceMemory codecImageMemory_ = VK_NULL_HANDLE;
    VkImageView codecImageView_ = VK_NULL_HANDLE;

    // Output 2: WYSIWYG monitoring feed presented to the Flutter viewfinder (binding 2)
    VkImage viewfinderImage_ = VK_NULL_HANDLE;
    VkDeviceMemory viewfinderImageMemory_ = VK_NULL_HANDLE;
    VkImageView viewfinderImageView_ = VK_NULL_HANDLE;

    // Host-visible readback buffer used to present the viewfinder image via
    // ANativeWindow_lock/unlockAndPost (Option 3A: direct buffer write).
    VkBuffer viewfinderStagingBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory viewfinderStagingMemory_ = VK_NULL_HANDLE;
    void* viewfinderStagingMapped_ = nullptr;
    VkDeviceSize viewfinderStagingSize_ = 0;

    ANativeWindow* codecWindow_ = nullptr;
    ANativeWindow* viewfinderWindow_ = nullptr;

    int32_t outputWidth_ = 3840;
    int32_t outputHeight_ = 2160;

    std::mutex vkMutex_;
    bool isInitialized_ = false;

    // Vulkan Android Hardware Buffer function pointers
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID vkGetAndroidHardwareBufferPropertiesANDROID_ = nullptr;
};

} // namespace rcamera
