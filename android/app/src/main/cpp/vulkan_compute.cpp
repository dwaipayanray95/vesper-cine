#include "vulkan_compute.h"
#include "shaders/mhc_rlog_spv.h"

#include <algorithm>
#include <chrono>
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

    // Binding 0 (the raw-frame storage buffer) is format-independent, so the
    // whole pipeline can be built up front rather than deferred to first frame.
    if (!createComputePipeline()) {
        VK_LOGE("Failed to create compute pipeline");
        isInitialized_ = false;
        return false;
    }

    if (viewfinderWindow) {
        // Inline what setViewfinderWindow() does rather than calling it: vkMutex_
        // is already held here (non-recursive), so calling that setter would deadlock.
        viewfinderWindow_ = viewfinderWindow;
        ANativeWindow_acquire(viewfinderWindow_);
        ANativeWindow_setBuffersGeometry(viewfinderWindow_, outputWidth_, outputHeight_, WINDOW_FORMAT_RGBA_8888);
    }

    VK_LOGI("Vulkan compute engine fully initialized (%dx%d)", outputWidth_, outputHeight_);
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
    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Vesper Cine",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "RCameraEngine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1
    };

    // Pure headless compute: no VkSurfaceKHR, no external-memory interop, so
    // no instance extensions are required.
    VkInstanceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo
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

    // No device extensions required: everything used (buffers, storage images,
    // compute pipelines) is Vulkan 1.0/1.1 core.
    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;

    VkResult res = vkCreateDevice(physicalDevice_, &deviceCreateInfo, nullptr, &device_);
    if (res != VK_SUCCESS) {
        VK_LOGE("vkCreateDevice error: %d", res);
        return false;
    }

    vkGetDeviceQueue(device_, computeQueueFamilyIndex_, 0, &computeQueue_);

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
    if (vkAllocateCommandBuffers(device_, &cbAlloc, &cb) != VK_SUCCESS) {
        VK_LOGE("vkAllocateCommandBuffers (output image transition) failed");
        return false;
    }

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

    // processRawFrame() deliberately does not wait for its own GPU submission
    // before returning (see there) — it waits at the *start* of the next call
    // instead, so the camera thread isn't blocked on a full round trip every
    // frame. That means GPU work referencing these images can genuinely still
    // be in flight here (e.g. a crop-mode change right after a frame lands),
    // so this wait is required, not just defensive.
    if (computeQueue_) {
        vkQueueWaitIdle(computeQueue_);
    }
    // Any deferred present would now reference a stale/about-to-be-freed buffer.
    viewfinderPresentPending_ = false;

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

bool VulkanComputeEngine::createComputePipeline() {
    VkShaderModuleCreateInfo shaderModuleInfo{};
    shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderModuleInfo.codeSize = MHC_RLOG_SPV_SIZE;
    shaderModuleInfo.pCode = MHC_RLOG_SPV;

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &shaderModuleInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        VK_LOGE("vkCreateShaderModule failed");
        return false;
    }

    // Binding 0: raw sensor frame, uploaded from the CPU each frame (see
    // ensureRawStagingBuffer) rather than imported as a sampled image — this
    // device's gralloc cannot report a byte-per-pixel size for a RAW10
    // AHardwareBuffer when asked to import it as a Vulkan sampled image.
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

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
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
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
    // Binding 0 (the raw-frame buffer) is written lazily by ensureRawStagingBuffer()
    // on the first frame, once its required size is known.
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

    VK_LOGI("Vulkan compute pipeline built");
    return true;
}

bool VulkanComputeEngine::ensureRawStagingBuffer(VkDeviceSize requiredSize) {
    if (rawStagingBuffer_ != VK_NULL_HANDLE && rawStagingCapacity_ >= requiredSize) {
        return true;
    }

    if (rawStagingMapped_) { vkUnmapMemory(device_, rawStagingMemory_); rawStagingMapped_ = nullptr; }
    if (rawStagingBuffer_) { vkDestroyBuffer(device_, rawStagingBuffer_, nullptr); rawStagingBuffer_ = VK_NULL_HANDLE; }
    if (rawStagingMemory_) { vkFreeMemory(device_, rawStagingMemory_, nullptr); rawStagingMemory_ = VK_NULL_HANDLE; }
    rawStagingCapacity_ = 0;

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = requiredSize;
    bufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bufInfo, nullptr, &rawStagingBuffer_) != VK_SUCCESS) {
        VK_LOGE("vkCreateBuffer (raw staging) failed");
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device_, rawStagingBuffer_, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        memReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device_, &allocInfo, nullptr, &rawStagingMemory_) != VK_SUCCESS) {
        VK_LOGE("vkAllocateMemory (raw staging) failed");
        return false;
    }
    if (vkBindBufferMemory(device_, rawStagingBuffer_, rawStagingMemory_, 0) != VK_SUCCESS) {
        VK_LOGE("vkBindBufferMemory (raw staging) failed");
        return false;
    }
    if (vkMapMemory(device_, rawStagingMemory_, 0, requiredSize, 0, &rawStagingMapped_) != VK_SUCCESS) {
        VK_LOGE("vkMapMemory (raw staging) failed");
        return false;
    }
    rawStagingCapacity_ = requiredSize;

    VkDescriptorBufferInfo bufDescInfo{};
    bufDescInfo.buffer = rawStagingBuffer_;
    bufDescInfo.offset = 0;
    bufDescInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufDescInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return true;
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

bool VulkanComputeEngine::processRawFrame(const uint8_t* data, size_t dataLength, const ComputeUniformData& uniforms) {
    if (!data || dataLength == 0) return false;
    if (!isInitialized_) return false;

    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();

    std::lock_guard<std::mutex> lock(vkMutex_);

    // Wait for the PREVIOUS frame's GPU work before reusing its command buffer /
    // staging buffers. A full camera frame interval has normally already elapsed
    // by the time we get here, so this is typically an instant no-op rather than
    // a real stall — unlike waiting at the END of this function (the previous
    // structure), which forced every single frame through a full synchronous
    // CPU-upload -> GPU-dispatch -> GPU-wait -> CPU-readback round trip before
    // the camera could even start delivering the next frame (measured ~7fps
    // instead of the UI's target 24fps).
    vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, UINT64_MAX);
    auto t1 = Clock::now();

    // Present the PREVIOUS frame's now-finished viewfinder output, deferred by
    // one frame so this frame's GPU dispatch below can run concurrently with
    // the camera delivering the next one instead of blocking on it.
    if (viewfinderPresentPending_) {
        presentViewfinder();
        viewfinderPresentPending_ = false;
    }
    auto t2 = Clock::now();

    if (!ensureRawStagingBuffer(static_cast<VkDeviceSize>(dataLength))) {
        return false;
    }
    std::memcpy(rawStagingMapped_, data, dataLength);
    auto t3 = Clock::now();

    vkResetFences(device_, 1, &frameFence_);
    vkResetCommandBuffer(commandBuffer_, 0);

    uint32_t groupCountX = (static_cast<uint32_t>(outputWidth_) + 15) / 16;
    uint32_t groupCountY = (static_cast<uint32_t>(outputHeight_) + 15) / 16;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer_, &beginInfo);

    // The CPU write above must be visible to the shader's buffer read.
    VkBufferMemoryBarrier hostToShaderRead{};
    hostToShaderRead.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    hostToShaderRead.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostToShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    hostToShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostToShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostToShaderRead.buffer = rawStagingBuffer_;
    hostToShaderRead.offset = 0;
    hostToShaderRead.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          0, 0, nullptr, 1, &hostToShaderRead, 0, nullptr);

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
    auto t4 = Clock::now();
    vkQueueSubmit(computeQueue_, 1, &submitInfo, frameFence_);
    // Deliberately not waiting here — see the wait at the top of this function
    // and viewfinderPresentPending_ above. Returning now lets the camera
    // callback thread move on to the next frame while the GPU works in the
    // background.
    auto t5 = Clock::now();

    if (wantsPresent) {
        viewfinderPresentPending_ = true;
    }

    // Timing breakdown, logged periodically. Diagnostic for a measured ~7fps
    // (vs the UI's 24fps target) that persisted even after removing the
    // synchronous end-of-frame GPU wait, despite the HAL itself reporting
    // 30fps is achievable at this RAW10 resolution — narrows down which
    // stage(s) of this function are actually the bottleneck.
    static int frameCounter = 0;
    if (++frameCounter % 30 == 0) {
        auto ms = [](Clock::time_point a, Clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        VK_LOGI("processRawFrame timing (ms): waitPrevFence=%.2f present=%.2f rawUpload=%.2f "
                "recordCmds=%.2f submit=%.2f TOTAL=%.2f",
                ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4), ms(t4, t5), ms(t0, t5));
    }

    return true;
}

void VulkanComputeEngine::release() {
    std::lock_guard<std::mutex> lock(vkMutex_);
    if (!isInitialized_) return;

    if (device_) {
        vkDeviceWaitIdle(device_);
        destroyOutputImages();
        if (rawStagingMapped_) { vkUnmapMemory(device_, rawStagingMemory_); rawStagingMapped_ = nullptr; }
        if (rawStagingBuffer_) vkDestroyBuffer(device_, rawStagingBuffer_, nullptr);
        if (rawStagingMemory_) vkFreeMemory(device_, rawStagingMemory_, nullptr);
        if (computePipeline_) vkDestroyPipeline(device_, computePipeline_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (descriptorSetLayout_) vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
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
    rawStagingBuffer_ = VK_NULL_HANDLE;
    rawStagingMemory_ = VK_NULL_HANDLE;
    rawStagingCapacity_ = 0;
    frameFence_ = VK_NULL_HANDLE;
    commandPool_ = VK_NULL_HANDLE;
    commandBuffer_ = VK_NULL_HANDLE;

    isInitialized_ = false;
    VK_LOGI("Vulkan compute engine released");
}

} // namespace rcamera
