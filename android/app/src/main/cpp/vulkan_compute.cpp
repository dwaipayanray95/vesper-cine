#include "vulkan_compute.h"
#include "shaders/mhc_rlog_spv.h"

#include <vector>
#include <cstring>

namespace rcamera {

VulkanComputeEngine::VulkanComputeEngine() = default;

VulkanComputeEngine::~VulkanComputeEngine() {
    release();
}

bool VulkanComputeEngine::initialize(ANativeWindow* codecWindow, ANativeWindow* viewfinderWindow) {
    std::lock_guard<std::mutex> lock(vkMutex_);
    if (isInitialized_) return true;

    codecWindow_ = codecWindow;
    viewfinderWindow_ = viewfinderWindow;

    if (!initInstance()) {
        VK_LOGE("Failed to create Vulkan instance");
        return false;
    }

    if (!initDevice()) {
        VK_LOGE("Failed to create Vulkan logical device");
        return false;
    }

    if (!createComputePipeline()) {
        VK_LOGE("Failed to create compute pipeline");
        return false;
    }

    isInitialized_ = true;
    VK_LOGI("Vulkan compute engine successfully initialized");
    return true;
}

bool VulkanComputeEngine::initInstance() {
    std::vector<const char*> instanceExtensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME
    };

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Project RawEdge",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "RCameraEngine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1
    };

    VkInstanceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size()),
        .ppEnabledExtensionNames = instanceExtensions.data()
    };

    VkResult res = vkCreateInstance(&createInfo, nullptr, &instance_);
    if (res != VK_SUCCESS) {
        VK_LOGE("vkCreateInstance error: %d", res);
        return false;
    }

    // Pick physical device supporting compute queue
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
    if (deviceCount == 0) return false;

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());
    physicalDevice_ = devices[0];

    // Find compute queue family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            computeQueueFamilyIndex_ = i;
            break;
        }
    }

    return true;
}

bool VulkanComputeEngine::initDevice() {
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = computeQueueFamilyIndex_,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority
    };

    std::vector<const char*> deviceExtensions = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME
    };

    VkDeviceCreateInfo deviceCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueCreateInfo,
        .enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size()),
        .ppEnabledExtensionNames = deviceExtensions.data()
    };

    VkResult res = vkCreateDevice(physicalDevice_, &deviceCreateInfo, nullptr, &device_);
    if (res != VK_SUCCESS) {
        VK_LOGE("vkCreateDevice error: %d", res);
        return false;
    }

    vkGetDeviceQueue(device_, computeQueueFamilyIndex_, 0, &computeQueue_);

    // Load Android Hardware Buffer extension function pointer
    vkGetAndroidHardwareBufferPropertiesANDROID_ =
        reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
            vkGetDeviceProcAddr(device_, "vkGetAndroidHardwareBufferPropertiesANDROID"));

    if (!vkGetAndroidHardwareBufferPropertiesANDROID_) {
        VK_LOGE("Failed to get vkGetAndroidHardwareBufferPropertiesANDROID function pointer");
        return false;
    }

    // Create command pool
    VkCommandPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = computeQueueFamilyIndex_
    };
    vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);

    VkCommandBufferAllocateInfo cmdAllocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool_,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    vkAllocateCommandBuffers(device_, &cmdAllocInfo, &commandBuffer_);

    return true;
}

bool VulkanComputeEngine::createComputePipeline() {
    // 1. Create Shader Module from embedded SPIR-V
    VkShaderModuleCreateInfo shaderModuleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = MHC_RLOG_SPV_SIZE,
        .pCode = MHC_RLOG_SPV
    };

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkResult res = vkCreateShaderModule(device_, &shaderModuleInfo, nullptr, &shaderModule);
    if (res != VK_SUCCESS) {
        VK_LOGE("vkCreateShaderModule error: %d", res);
        return false;
    }

    // 2. Descriptor Set Layout
    // Binding 0: Sampler2D (raw bayer)
    // Binding 1: Storage Image (codec)
    // Binding 2: Storage Image (viewfinder)
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        },
        {
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        }
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<uint32_t>(bindings.size()),
        .pBindings = bindings.data()
    };
    vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_);

    // Push constant range for uniforms
    VkPushConstantRange pushConstantRange = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(ComputeUniformData)
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptorSetLayout_,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange
    };
    vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_);

    // 3. Compute Pipeline
    VkComputePipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shaderModule,
            .pName = "main"
        },
        .layout = pipelineLayout_
    };

    res = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &computePipeline_);
    vkDestroyShaderModule(device_, shaderModule, nullptr);

    if (res != VK_SUCCESS) {
        VK_LOGE("vkCreateComputePipelines error: %d", res);
        return false;
    }

    VK_LOGI("Vulkan compute pipeline initialized successfully");
    return true;
}

bool VulkanComputeEngine::processRawFrame(AHardwareBuffer* hwBuffer, const ComputeUniformData& uniforms) {
    if (!isInitialized_ || !hwBuffer) return false;

    std::lock_guard<std::mutex> lock(vkMutex_);

    // Dispatch compute shader (16x16 workgroup size)
    uint32_t groupCountX = (outputWidth_ + 15) / 16;
    uint32_t groupCountY = (outputHeight_ + 15) / 16;

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    vkBeginCommandBuffer(commandBuffer_, &beginInfo);
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline_);

    // Push uniforms
    vkCmdPushConstants(
        commandBuffer_,
        pipelineLayout_,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(ComputeUniformData),
        &uniforms
    );

    vkCmdDispatch(commandBuffer_, groupCountX, groupCountY, 1);
    vkEndCommandBuffer(commandBuffer_);

    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer_
    };

    vkQueueSubmit(computeQueue_, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(computeQueue_);

    // Release acquired hardware buffer ref
    AHardwareBuffer_release(hwBuffer);

    return true;
}

void VulkanComputeEngine::release() {
    std::lock_guard<std::mutex> lock(vkMutex_);
    if (!isInitialized_) return;

    if (device_) {
        vkDeviceWaitIdle(device_);
        if (computePipeline_) vkDestroyPipeline(device_, computePipeline_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorSetLayout_) vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }

    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
    }

    isInitialized_ = false;
    VK_LOGI("Vulkan compute engine released");
}

} // namespace rcamera
