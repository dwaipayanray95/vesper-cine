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

struct ComputeUniformData {
    float blackLevel[4];          // R, Gr, Gb, B
    float wbGains[4];             // rGain, gGain, bGain, padding
    float sensorToXyzMatrix[9];   // 3x3
    float pad0;
    float xyzToRec2020Matrix[9];  // 3x3
    float pad1;
    int32_t rawWidth;
    int32_t rawHeight;
    int32_t outputWidth;
    int32_t outputHeight;
    int32_t cropMode;             // 0: 16:9, 1: 4:3 open-gate
    int32_t monitoringMode;       // 0: R-Log, 1: Rec.709, 2: False Color, 3: Peaking, 4: Zebras
    float zebraThreshold;         // e.g. 0.95
    float peakingThreshold;       // e.g. 0.15
    float whiteLevel;             // e.g. 1023.0
    float pad2;
};

class VulkanComputeEngine {
public:
    VulkanComputeEngine();
    ~VulkanComputeEngine();

    bool initialize(ANativeWindow* codecWindow, ANativeWindow* viewfinderWindow);
    void release();

    // Process a raw frame from AHardwareBuffer directly into codec & viewfinder surfaces
    bool processRawFrame(AHardwareBuffer* hwBuffer, const ComputeUniformData& uniforms);

    void setOutputDimensions(int32_t width, int32_t height) {
        outputWidth_ = width;
        outputHeight_ = height;
    }

private:
    bool initInstance();
    bool initDevice();
    bool createComputePipeline();
    bool setupOutputSurfaces(ANativeWindow* codecWindow, ANativeWindow* viewfinderWindow);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue computeQueue_ = VK_NULL_HANDLE;
    uint32_t computeQueueFamilyIndex_ = 0;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline computePipeline_ = VK_NULL_HANDLE;

    // Output target surfaces
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
