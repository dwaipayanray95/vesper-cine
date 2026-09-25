#include "vulkan_compute.h"
#include "shaders/mhc_rlog_spv.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace rcamera {

VulkanComputeEngine::VulkanComputeEngine() = default;

VulkanComputeEngine::~VulkanComputeEngine() {
    release();
}

bool VulkanComputeEngine::initialize(ANativeWindow* codecWindow, ANativeWindow* viewfinderWindow) {
    std::lock_guard<std::mutex> lock(vkMutex_);
    if (isInitialized_) return true;

    codecWindow_ = codecWindow;

    if (!initInstance()) {
        VK_LOGE("Failed to create Vulkan instance");
        return false;
    }

    if (!initDevice()) {
        VK_LOGE("Failed to create Vulkan logical device");
        return false;
    }

    isInitialized_ = true;

    if (!createOutputImages()) {
        VK_LOGE("Failed to create GPU output images");
        isInitialized_ = false;
        return false;
    }

    if (viewfinderWindow) {
        // Route through the normal setter so acquire + buffer geometry logic
        // only lives in one place.
        viewfinderWindow_ = nullptr; // setViewfinderWindow() diffs against the current value
        setViewfinderWindow(viewfinderWindow);
    }

    VK_LOGI("Vulkan compute engine initialized (%dx%d); compute pipeline finishes building on first frame", outputWidth_, outputHeight_);
    return true;
}

void VulkanComputeEngine::setViewfinderWindow(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(vkMutex_);
    if (viewfinderWindow_ == window) {
        return;
    }
    if (viewfinderWindow_) {
        ANativeWindow_release(viewfinderWindow_);
    }
    viewfinderWindow_ = window;
    if (viewfinderWindow_) {
        ANativeWindow_acquire(viewfinderWindow_);
        ANativeWindow_setBuffersGeometry(viewfinderWindow_, outputWidth_, outputHeight_, WINDOW_FORMAT_RGBA_8888);
    }
    VK_LOGI("VulkanComputeEngine: viewfinderWindow_ set to %p (%dx%d)", viewfinderWindow_, outputWidth_, outputHeight_);
}

void VulkanComputeEngine::setOutputDimensions(int32_t width, int32_t height) {
    std::lock_guard<std::mutex> lock(vkMutex_);
    if (width <= 0 || height <= 0) return;
    if (width == outputWidth_ && height == outputHeight_ && codecImage_ != VK_NULL_HANDLE) return;

    outputWidth_ = width;
    outputHeight_ = height;

    if (isInitialized_) {
        createOutputImages();
    }
    if (viewfinderWindow_) {
        ANativeWindow_setBuffersGeometry(viewfinderWindow_, outputWidth_, outputHeight_, WINDOW_FORMAT_RGBA_8888);
    }
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
        .pApplicationName = "Vesper Cine",
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

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
    if (deviceCount == 0) return false;

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());
    physicalDevice_ = devices[0];

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

    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures{};
    ycbcrFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    ycbcrFeatures.samplerYcbcrConversion = VK_TRUE;

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pNext = &ycbcrFeatures;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();

    VkResult res = vkCreateDevice(physicalDevice_, &deviceCreateInfo, nullptr, &device_);
    if (res != VK_SUCCESS) {
        VK_LOGE("vkCreateDevice error: %d", res);
        return false;
    }

    vkGetDeviceQueue(device_, computeQueueFamilyIndex_, 0, &computeQueue_);

    vkGetAndroidHardwareBufferPropertiesANDROID_ =
        reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
            vkGetDeviceProcAddr(device_, "vkGetAndroidHardwareBufferPropertiesANDROID"));

    if (!vkGetAndroidHardwareBufferPropertiesANDROID_) {
        VK_LOGE("Failed to get vkGetAndroidHardwareBufferPropertiesANDROID function pointer");
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = computeQueueFamilyIndex_;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        VK_LOGE("vkCreateCommandPool failed");
        return false;
    }

    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = commandPool_;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &cmdAllocInfo, &commandBuffer_) != VK_SUCCESS) {
        VK_LOGE("vkAllocateCommandBuffers failed");
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device_, &fenceInfo, nullptr, &frameFence_) != VK_SUCCESS) {
        VK_LOGE("vkCreateFence failed");
        return false;
    }

    return true;
}

uint32_t VulkanComputeEngine::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    VK_LOGE("findMemoryType: no suitable memory type for bits=0x%x props=0x%x", typeBits, properties);
    return 0;
}

bool VulkanComputeEngine::createOutputImages() {
    destroyOutputImages();

    auto createStorageImage = [&](VkFormat format, VkImage& image, VkDeviceMemory& memory, VkImageView& view) -> bool {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { static_cast<uint32_t>(outputWidth_), static_cast<uint32_t>(outputHeight_), 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(device_, &imageInfo, nullptr, &image) != VK_SUCCESS) {
            VK_LOGE("vkCreateImage (output) failed");
            return false;
        }

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(device_, image, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device_, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
            VK_LOGE("vkAllocateMemory (output image) failed");
            return false;
        }
        vkBindImageMemory(device_, image, memory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &viewInfo, nullptr, &view) != VK_SUCCESS) {
            VK_LOGE("vkCreateImageView (output) failed");
            return false;
        }
        return true;
    };

    // Binding 1: clean 10-bit-equivalent R-Log master feeding AMediaCodec (Phase 2)
    if (!createStorageImage(VK_FORMAT_R16G16B16A16_SFLOAT, codecImage_, codecImageMemory_, codecImageView_)) {
        return false;
    }
    // Binding 2: WYSIWYG monitoring feed presented straight to the Flutter viewfinder
    if (!createStorageImage(VK_FORMAT_R8G8B8A8_UNORM, viewfinderImage_, viewfinderImageMemory_, viewfinderImageView_)) {
        return false;
    }

    // One-time layout transition UNDEFINED -> GENERAL; both images stay in
    // GENERAL for their whole lifetime (written by the compute shader, read
    // back for the viewfinder via a copy that also accepts GENERAL).
    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool = commandPool_;
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cbAlloc, &cb);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &beginInfo);

    VkImageMemoryBarrier barriers[2]{};
    for (auto& b : barriers) {
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
    }
    barriers[0].image = codecImage_;
    barriers[1].image = viewfinderImage_;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          0, 0, nullptr, 0, nullptr, 2, barriers);
    vkEndCommandBuffer(cb);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(computeQueue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(computeQueue_);
    vkFreeCommandBuffers(device_, commandPool_, 1, &cb);

    // Host-visible staging buffer used to hand the viewfinder frame to
    // ANativeWindow_lock/unlockAndPost (Option 3A: direct buffer write).
    VkDeviceSize stagingSize = static_cast<VkDeviceSize>(outputWidth_) * static_cast<VkDeviceSize>(outputHeight_) * 4;
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = stagingSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bufInfo, nullptr, &viewfinderStagingBuffer_) != VK_SUCCESS) {
        VK_LOGE("Failed to create viewfinder staging buffer");
        return false;
    }

    VkMemoryRequirements bufMemReq;
    vkGetBufferMemoryRequirements(device_, viewfinderStagingBuffer_, &bufMemReq);
    VkMemoryAllocateInfo bufAllocInfo{};
    bufAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufAllocInfo.allocationSize = bufMemReq.size;
    bufAllocInfo.memoryTypeIndex = findMemoryType(
        bufMemReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device_, &bufAllocInfo, nullptr, &viewfinderStagingMemory_) != VK_SUCCESS) {
        VK_LOGE("Failed to allocate viewfinder staging memory");
        return false;
    }
    vkBindBufferMemory(device_, viewfinderStagingBuffer_, viewfinderStagingMemory_, 0);
    vkMapMemory(device_, viewfinderStagingMemory_, 0, stagingSize, 0, &viewfinderStagingMapped_);
    viewfinderStagingSize_ = stagingSize;

    // If the pipeline/descriptor set already exists (this is a resize, not
    // first-time creation), re-point bindings 1/2 at the freshly (re)created images.
    if (descriptorSet_ != VK_NULL_HANDLE) {
        VkDescriptorImageInfo codecInfo{};
        codecInfo.imageView = codecImageView_;
        codecInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo vfInfo{};
        vfInfo.imageView = viewfinderImageView_;
        vfInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSet_;
        writes[0].dstBinding = 1;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &codecInfo;

        writes[1] = writes[0];
        writes[1].dstBinding = 2;
        writes[1].pImageInfo = &vfInfo;

        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    }

    return true;
}

void VulkanComputeEngine::destroyOutputImages() {
    if (!device_) return;

    // Defensive: callers only reach here with the GPU already idle (processRawFrame
    // waits on frameFence_ before releasing vkMutex_), but a stray in-flight submission
    // referencing these images would be a use-after-free, so make it unconditional.
    if (computeQueue_) {
        vkQueueWaitIdle(computeQueue_);
    }

    if (viewfinderStagingMapped_) {
        vkUnmapMemory(device_, viewfinderStagingMemory_);
        viewfinderStagingMapped_ = nullptr;
    }
    if (viewfinderStagingBuffer_) { vkDestroyBuffer(device_, viewfinderStagingBuffer_, nullptr); viewfinderStagingBuffer_ = VK_NULL_HANDLE; }
    if (viewfinderStagingMemory_) { vkFreeMemory(device_, viewfinderStagingMemory_, nullptr); viewfinderStagingMemory_ = VK_NULL_HANDLE; }

    if (codecImageView_) { vkDestroyImageView(device_, codecImageView_, nullptr); codecImageView_ = VK_NULL_HANDLE; }
    if (codecImage_) { vkDestroyImage(device_, codecImage_, nullptr); codecImage_ = VK_NULL_HANDLE; }
    if (codecImageMemory_) { vkFreeMemory(device_, codecImageMemory_, nullptr); codecImageMemory_ = VK_NULL_HANDLE; }

    if (viewfinderImageView_) { vkDestroyImageView(device_, viewfinderImageView_, nullptr); viewfinderImageView_ = VK_NULL_HANDLE; }
    if (viewfinderImage_) { vkDestroyImage(device_, viewfinderImage_, nullptr); viewfinderImage_ = VK_NULL_HANDLE; }
    if (viewfinderImageMemory_) { vkFreeMemory(device_, viewfinderImageMemory_, nullptr); viewfinderImageMemory_ = VK_NULL_HANDLE; }
}

bool VulkanComputeEngine::ensureExternalFormatResources(AHardwareBuffer* hwBuffer) {
    VkAndroidHardwareBufferFormatPropertiesANDROID formatProps{};
    formatProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;

    VkAndroidHardwareBufferPropertiesANDROID bufferProps{};
    bufferProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    bufferProps.pNext = &formatProps;

    if (vkGetAndroidHardwareBufferPropertiesANDROID_(device_, hwBuffer, &bufferProps) != VK_SUCCESS) {
        VK_LOGE("vkGetAndroidHardwareBufferPropertiesANDROID failed");
        return false;
    }

    if (rawImmutableSampler_ != VK_NULL_HANDLE && cachedExternalFormat_ == formatProps.externalFormat) {
        return true; // Already built for this buffer's format; nothing to do.
    }

    VK_LOGI("Building GPU pipeline for AHardwareBuffer externalFormat=%llu (vkFormat=%d)",
            static_cast<unsigned long long>(formatProps.externalFormat), formatProps.format);

    // Tear down anything built for a previous (different) format.
    releaseFrameImage();
    if (computePipeline_) { vkDestroyPipeline(device_, computePipeline_, nullptr); computePipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_) { vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (descriptorPool_) { vkDestroyDescriptorPool(device_, descriptorPool_, nullptr); descriptorPool_ = VK_NULL_HANDLE; descriptorSet_ = VK_NULL_HANDLE; }
    if (descriptorSetLayout_) { vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr); descriptorSetLayout_ = VK_NULL_HANDLE; }
    if (rawImmutableSampler_) { vkDestroySampler(device_, rawImmutableSampler_, nullptr); rawImmutableSampler_ = VK_NULL_HANDLE; }
    if (rawYcbcrConversion_) { vkDestroySamplerYcbcrConversion(device_, rawYcbcrConversion_, nullptr); rawYcbcrConversion_ = VK_NULL_HANDLE; }

    bool useExternalFormat = (formatProps.format == VK_FORMAT_UNDEFINED);

    VkExternalFormatANDROID externalFormatInfo{};
    externalFormatInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    externalFormatInfo.externalFormat = formatProps.externalFormat;

    VkSamplerYcbcrConversionCreateInfo convInfo{};
    convInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
    convInfo.pNext = useExternalFormat ? &externalFormatInfo : nullptr;
    convInfo.format = useExternalFormat ? VK_FORMAT_UNDEFINED : formatProps.format;
    convInfo.ycbcrModel = formatProps.suggestedYcbcrModel;
    convInfo.ycbcrRange = formatProps.suggestedYcbcrRange;
    convInfo.components = formatProps.samplerYcbcrConversionComponents;
    convInfo.xChromaOffset = formatProps.suggestedXChromaOffset;
    convInfo.yChromaOffset = formatProps.suggestedYChromaOffset;
    // Raw Bayer sensor data is never chroma-subsampled: never interpolate across texels.
    convInfo.chromaFilter = VK_FILTER_NEAREST;
    convInfo.forceExplicitReconstruction = VK_FALSE;

    if (vkCreateSamplerYcbcrConversion(device_, &convInfo, nullptr, &rawYcbcrConversion_) != VK_SUCCESS) {
        VK_LOGE("vkCreateSamplerYcbcrConversion failed");
        return false;
    }

    VkSamplerYcbcrConversionInfo samplerConvInfo{};
    samplerConvInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    samplerConvInfo.conversion = rawYcbcrConversion_;

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.pNext = &samplerConvInfo;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.compareOp = VK_COMPARE_OP_NEVER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

    if (vkCreateSampler(device_, &samplerInfo, nullptr, &rawImmutableSampler_) != VK_SUCCESS) {
        VK_LOGE("vkCreateSampler (raw ycbcr) failed");
        return false;
    }

    cachedExternalFormat_ = formatProps.externalFormat;

    return createComputePipeline(rawImmutableSampler_);
}

bool VulkanComputeEngine::createComputePipeline(VkSampler immutableRawSampler) {
    VkShaderModuleCreateInfo shaderModuleInfo{};
    shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleInfo.codeSize = MHC_RLOG_SPV_SIZE;
    shaderModuleInfo.pCode = MHC_RLOG_SPV;

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &shaderModuleInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        VK_LOGE("vkCreateShaderModule failed");
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[0].pImmutableSamplers = &immutableRawSampler;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_) != VK_SUCCESS) {
        VK_LOGE("vkCreateDescriptorSetLayout failed");
        vkDestroyShaderModule(device_, shaderModule, nullptr);
        return false;
    }

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(ComputeUniformData);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        VK_LOGE("vkCreatePipelineLayout failed");
        vkDestroyShaderModule(device_, shaderModule, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shaderModule;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = pipelineLayout_;

    VkResult res = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &computePipeline_);
    vkDestroyShaderModule(device_, shaderModule, nullptr);
    if (res != VK_SUCCESS) {
        VK_LOGE("vkCreateComputePipelines error: %d", res);
        return false;
    }

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        VK_LOGE("vkCreateDescriptorPool failed");
        return false;
    }

    VkDescriptorSetAllocateInfo dsAllocInfo{};
    dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAllocInfo.descriptorPool = descriptorPool_;
    dsAllocInfo.descriptorSetCount = 1;
    dsAllocInfo.pSetLayouts = &descriptorSetLayout_;
    if (vkAllocateDescriptorSets(device_, &dsAllocInfo, &descriptorSet_) != VK_SUCCESS) {
        VK_LOGE("vkAllocateDescriptorSets failed");
        return false;
    }

    // Output storage images already exist (created in initialize()); bind them now.
    VkDescriptorImageInfo codecInfo{};
    codecInfo.imageView = codecImageView_;
    codecInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo vfInfo{};
    vfInfo.imageView = viewfinderImageView_;
    vfInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptorSet_;
    writes[0].dstBinding = 1;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &codecInfo;

    writes[1] = writes[0];
    writes[1].dstBinding = 2;
    writes[1].pImageInfo = &vfInfo;

    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

    VK_LOGI("Vulkan compute pipeline (re)built");
    return true;
}

bool VulkanComputeEngine::importFrameImage(AHardwareBuffer* hwBuffer, int32_t width, int32_t height) {
    releaseFrameImage();

    VkAndroidHardwareBufferFormatPropertiesANDROID formatProps{};
    formatProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID bufferProps{};
    bufferProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    bufferProps.pNext = &formatProps;
    if (vkGetAndroidHardwareBufferPropertiesANDROID_(device_, hwBuffer, &bufferProps) != VK_SUCCESS) {
        VK_LOGE("vkGetAndroidHardwareBufferPropertiesANDROID (per-frame) failed");
        return false;
    }

    bool useExternalFormat = (formatProps.format == VK_FORMAT_UNDEFINED);

    VkExternalFormatANDROID externalFormatInfo{};
    externalFormatInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    externalFormatInfo.externalFormat = formatProps.externalFormat;

    VkExternalMemoryImageCreateInfo extMemImageInfo{};
    extMemImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extMemImageInfo.pNext = useExternalFormat ? static_cast<void*>(&externalFormatInfo) : nullptr;
    extMemImageInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &extMemImageInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = useExternalFormat ? VK_FORMAT_UNDEFINED : formatProps.format;
    imageInfo.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &imageInfo, nullptr, &rawFrameImage_) != VK_SUCCESS) {
        VK_LOGE("vkCreateImage (raw frame import) failed");
        return false;
    }

    VkImportAndroidHardwareBufferInfoANDROID importInfo{};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    importInfo.buffer = hwBuffer;

    VkMemoryDedicatedAllocateInfo dedicatedInfo{};
    dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.pNext = &importInfo;
    dedicatedInfo.image = rawFrameImage_;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &dedicatedInfo;
    allocInfo.allocationSize = bufferProps.allocationSize;
    allocInfo.memoryTypeIndex = findMemoryType(bufferProps.memoryTypeBits, 0);

    if (vkAllocateMemory(device_, &allocInfo, nullptr, &rawFrameMemory_) != VK_SUCCESS) {
        VK_LOGE("vkAllocateMemory (raw frame import) failed");
        return false;
    }
    if (vkBindImageMemory(device_, rawFrameImage_, rawFrameMemory_, 0) != VK_SUCCESS) {
        VK_LOGE("vkBindImageMemory (raw frame import) failed");
        return false;
    }

    VkSamplerYcbcrConversionInfo samplerConvInfo{};
    samplerConvInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    samplerConvInfo.conversion = rawYcbcrConversion_;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.pNext = &samplerConvInfo;
    viewInfo.image = rawFrameImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = useExternalFormat ? VK_FORMAT_UNDEFINED : formatProps.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &viewInfo, nullptr, &rawFrameView_) != VK_SUCCESS) {
        VK_LOGE("vkCreateImageView (raw frame import) failed");
        return false;
    }

    VkDescriptorImageInfo descImageInfo{};
    descImageInfo.sampler = rawImmutableSampler_; // ignored by the driver for immutable-sampler bindings
    descImageInfo.imageView = rawFrameView_;
    descImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &descImageInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return true;
}

void VulkanComputeEngine::releaseFrameImage() {
    if (!device_) return;
    if (rawFrameView_) { vkDestroyImageView(device_, rawFrameView_, nullptr); rawFrameView_ = VK_NULL_HANDLE; }
    if (rawFrameImage_) { vkDestroyImage(device_, rawFrameImage_, nullptr); rawFrameImage_ = VK_NULL_HANDLE; }
    if (rawFrameMemory_) { vkFreeMemory(device_, rawFrameMemory_, nullptr); rawFrameMemory_ = VK_NULL_HANDLE; }
}

bool VulkanComputeEngine::presentViewfinder() {
    if (!viewfinderWindow_ || !viewfinderStagingMapped_) return false;

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(viewfinderWindow_, &buffer, nullptr) != 0) {
        VK_LOGE("ANativeWindow_lock failed");
        return false;
    }

    const uint8_t* src = static_cast<const uint8_t*>(viewfinderStagingMapped_);
    uint8_t* dst = static_cast<uint8_t*>(buffer.bits);
    int32_t copyWidth = std::min(buffer.width, outputWidth_);
    int32_t copyHeight = std::min(buffer.height, outputHeight_);
    size_t srcStride = static_cast<size_t>(outputWidth_) * 4;
    size_t dstStride = static_cast<size_t>(buffer.stride) * 4;
    size_t rowBytes = static_cast<size_t>(copyWidth) * 4;

    for (int32_t y = 0; y < copyHeight; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * dstStride, src + static_cast<size_t>(y) * srcStride, rowBytes);
    }

    ANativeWindow_unlockAndPost(viewfinderWindow_);
    return true;
}

bool VulkanComputeEngine::processRawFrame(AHardwareBuffer* hwBuffer, const ComputeUniformData& uniforms) {
    if (!hwBuffer) return false;
    if (!isInitialized_) {
        AHardwareBuffer_release(hwBuffer);
        return false;
    }

    std::lock_guard<std::mutex> lock(vkMutex_);

    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(hwBuffer, &desc);

    if (!ensureExternalFormatResources(hwBuffer)) {
        AHardwareBuffer_release(hwBuffer);
        return false;
    }

    if (!importFrameImage(hwBuffer, static_cast<int32_t>(desc.width), static_cast<int32_t>(desc.height))) {
        AHardwareBuffer_release(hwBuffer);
        return false;
    }

    vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &frameFence_);
    vkResetCommandBuffer(commandBuffer_, 0);

    uint32_t groupCountX = (static_cast<uint32_t>(outputWidth_) + 15) / 16;
    uint32_t groupCountY = (static_cast<uint32_t>(outputHeight_) + 15) / 16;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &beginInfo);

    // Freshly-imported raw frame: UNDEFINED -> SHADER_READ_ONLY_OPTIMAL.
    VkImageMemoryBarrier toShaderRead{};
    toShaderRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShaderRead.srcAccessMask = 0;
    toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.image = rawFrameImage_;
    toShaderRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toShaderRead.subresourceRange.levelCount = 1;
    toShaderRead.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &toShaderRead);

    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline_);
    vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
    vkCmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputeUniformData), &uniforms);
    vkCmdDispatch(commandBuffer_, groupCountX, groupCountY, 1);

    bool wantsPresent = (viewfinderWindow_ != nullptr && viewfinderStagingBuffer_ != VK_NULL_HANDLE);
    if (wantsPresent) {
        // Compute-shader writes to the viewfinder image -> transfer read for the readback copy.
        VkImageMemoryBarrier toTransferRead{};
        toTransferRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransferRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toTransferRead.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransferRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toTransferRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toTransferRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferRead.image = viewfinderImage_;
        toTransferRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransferRead.subresourceRange.levelCount = 1;
        toTransferRead.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &toTransferRead);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { static_cast<uint32_t>(outputWidth_), static_cast<uint32_t>(outputHeight_), 1 };
        vkCmdCopyImageToBuffer(commandBuffer_, viewfinderImage_, VK_IMAGE_LAYOUT_GENERAL, viewfinderStagingBuffer_, 1, &region);
    }

    vkEndCommandBuffer(commandBuffer_);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer_;
    vkQueueSubmit(computeQueue_, 1, &submitInfo, frameFence_);
    vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, UINT64_MAX);

    if (wantsPresent) {
        presentViewfinder();
    }

    // The imported per-frame image/view/memory must not outlive this frame's
    // AHardwareBuffer; drop it now that the GPU work has completed.
    releaseFrameImage();
    AHardwareBuffer_release(hwBuffer);

    return true;
}

void VulkanComputeEngine::release() {
    std::lock_guard<std::mutex> lock(vkMutex_);
    if (!isInitialized_) return;

    if (device_) {
        vkDeviceWaitIdle(device_);
        releaseFrameImage();
        destroyOutputImages();
        if (computePipeline_) vkDestroyPipeline(device_, computePipeline_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (descriptorSetLayout_) vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        if (rawImmutableSampler_) vkDestroySampler(device_, rawImmutableSampler_, nullptr);
        if (rawYcbcrConversion_) vkDestroySamplerYcbcrConversion(device_, rawYcbcrConversion_, nullptr);
        if (frameFence_) vkDestroyFence(device_, frameFence_, nullptr);
        if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }

    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
    }

    if (viewfinderWindow_) {
        ANativeWindow_release(viewfinderWindow_);
        viewfinderWindow_ = nullptr;
    }

    device_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    computeQueue_ = VK_NULL_HANDLE;
    computePipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSetLayout_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;
    rawImmutableSampler_ = VK_NULL_HANDLE;
    rawYcbcrConversion_ = VK_NULL_HANDLE;
    cachedExternalFormat_ = 0;
    frameFence_ = VK_NULL_HANDLE;
    commandPool_ = VK_NULL_HANDLE;
    commandBuffer_ = VK_NULL_HANDLE;

    isInitialized_ = false;
    VK_LOGI("Vulkan compute engine released");
}

} // namespace rcamera
