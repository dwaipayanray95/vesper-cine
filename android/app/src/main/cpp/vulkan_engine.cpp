#include "vulkan_engine.h"
#include "shaders/unpack_spv.h"
#include "shaders/render_spv.h"
#include "shaders/clean_spv.h"
#include "shaders/align_spv.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace vesper {

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

constexpr uint64_t kFenceTimeoutNs = 250'000'000; // a frame this late is dropped, not waited for

VkShaderModule makeModule(VkDevice dev, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &info, nullptr, &m);
    return m;
}

VkImageMemoryBarrier imageBarrier(VkImage img, VkAccessFlags src, VkAccessFlags dst,
                                  VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list) if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Setup / teardown
// ---------------------------------------------------------------------------

bool VulkanEngine::initialize() {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (initialized_) return true;
    if (!createInstanceAndDevice() || !createPipelines()) {
        VK_LOGE("Vulkan initialisation failed");
        return false;
    }
    initialized_ = true;
    VK_LOGI("Vulkan engine ready (queue family %u, graphics=%d, swapchain=%d)",
            queueFamily_, queueHasGraphics_, haveSwapchainExt_);
    return true;
}

bool VulkanEngine::createInstanceAndDevice() {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> instExts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, instExts.data());
    std::vector<const char*> enabled;
    haveSurfaceExt_ = hasExtension(instExts, VK_KHR_SURFACE_EXTENSION_NAME) &&
                      hasExtension(instExts, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    if (haveSurfaceExt_) {
        enabled.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        enabled.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Vesper Cine";
    app.pEngineName = "Vesper Engine";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    ici.ppEnabledExtensionNames = enabled.data();
    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) return false;

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance_, &devCount, nullptr);
    if (devCount == 0) return false;
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(instance_, &devCount, devs.data());
    physical_ = devs[0];
    vkGetPhysicalDeviceMemoryProperties(physical_, &memProps_);

    // Prefer a universal (graphics + compute) family: it can also blit and
    // present, which lets the viewfinder go straight to a swapchain.
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> fams(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, fams.data());
    int chosen = -1;
    for (uint32_t i = 0; i < qn; ++i) {
        VkQueueFlags f = fams[i].queueFlags;
        if ((f & VK_QUEUE_COMPUTE_BIT) && (f & VK_QUEUE_GRAPHICS_BIT)) { chosen = static_cast<int>(i); break; }
    }
    if (chosen < 0) {
        for (uint32_t i = 0; i < qn; ++i) if (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { chosen = static_cast<int>(i); break; }
    }
    if (chosen < 0) return false;
    queueFamily_ = static_cast<uint32_t>(chosen);
    queueHasGraphics_ = (fams[queueFamily_].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
    if (fams[queueFamily_].timestampValidBits > 0) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_, &props);
        timestampPeriodNs_ = props.limits.timestampPeriod;
    }

    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> devExts(n);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, devExts.data());
    haveSwapchainExt_ = haveSurfaceExt_ && hasExtension(devExts, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    std::vector<const char*> devEnabled;
    if (haveSwapchainExt_) devEnabled.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(devEnabled.size());
    dci.ppEnabledExtensionNames = devEnabled.data();
    if (vkCreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS) return false;
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    if (timestampPeriodNs_ > 0) {
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = kStamps * kRingSize;
        if (vkCreateQueryPool(device_, &qpi, nullptr, &queryPool_) != VK_SUCCESS) queryPool_ = VK_NULL_HANDLE;
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(device_, &pci, nullptr, &cmdPool_) != VK_SUCCESS) return false;

    for (auto& s : slots_) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = cmdPool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &cai, &s.cmd) != VK_SUCCESS) return false;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(device_, &fci, nullptr, &s.fence) != VK_SUCCESS) return false;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(device_, &sci, nullptr, &s.acquireSem) != VK_SUCCESS) return false;
    }
    return true;
}

bool VulkanEngine::createPipelines() {
    auto layoutFor = [&](const std::vector<VkDescriptorType>& types, VkDescriptorSetLayout& out) {
        std::vector<VkDescriptorSetLayoutBinding> b(types.size());
        for (size_t i = 0; i < types.size(); ++i) {
            b[i].binding = static_cast<uint32_t>(i);
            b[i].descriptorType = types[i];
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = static_cast<uint32_t>(b.size());
        ci.pBindings = b.data();
        return vkCreateDescriptorSetLayout(device_, &ci, nullptr, &out) == VK_SUCCESS;
    };
    if (!layoutFor({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, unpackLayout_)) return false;
    if (!layoutFor({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, cleanLayout_)) return false;
    if (!layoutFor({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                   alignLayout_)) return false;
    if (!layoutFor({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, renderLayout_)) return false;

    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4 * kRingSize},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kRingSize},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 11 * kRingSize},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kRingSize},
    };
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 4 * kRingSize;
    dpi.poolSizeCount = 4;
    dpi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device_, &dpi, nullptr, &descPool_) != VK_SUCCESS) return false;
    for (auto& s : slots_) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &unpackLayout_;
        if (vkAllocateDescriptorSets(device_, &ai, &s.unpackSet) != VK_SUCCESS) return false;
        ai.pSetLayouts = &renderLayout_;
        if (vkAllocateDescriptorSets(device_, &ai, &s.renderSet) != VK_SUCCESS) return false;
        ai.pSetLayouts = &cleanLayout_;
        if (vkAllocateDescriptorSets(device_, &ai, &s.cleanSet) != VK_SUCCESS) return false;
        ai.pSetLayouts = &alignLayout_;
        if (vkAllocateDescriptorSets(device_, &ai, &s.alignSet) != VK_SUCCESS) return false;
    }

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (vkCreateSampler(device_, &sci, nullptr, &sampler_) != VK_SUCCESS) return false;

    auto makePipe = [&](const uint32_t* code, size_t bytes, VkDescriptorSetLayout set,
                        VkPipelineLayout& layout, VkPipeline& pipe, uint32_t pushBytes = 0) {
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set;
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
        if (pushBytes) {
            pli.pushConstantRangeCount = 1;
            pli.pPushConstantRanges = &push;
        }
        if (vkCreatePipelineLayout(device_, &pli, nullptr, &layout) != VK_SUCCESS) return false;
        VkShaderModule mod = makeModule(device_, code, bytes);
        if (!mod) return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = mod;
        ci.stage.pName = "main";
        ci.layout = layout;
        VkResult r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe);
        vkDestroyShaderModule(device_, mod, nullptr);
        return r == VK_SUCCESS;
    };
    return makePipe(kUnpackSpv, sizeof(kUnpackSpv), unpackLayout_, unpackPipeLayout_, unpackPipe_) &&
           makePipe(kCleanSpv, sizeof(kCleanSpv), cleanLayout_, cleanPipeLayout_, cleanPipe_) &&
           makePipe(kAlignSpv, sizeof(kAlignSpv), alignLayout_, alignPipeLayout_, alignPipe_, sizeof(int32_t)) &&
           makePipe(kRenderSpv, sizeof(kRenderSpv), renderLayout_, renderPipeLayout_, renderPipe_);
}

void VulkanEngine::release() {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (!device_) {
        if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
        return;
    }
    drainAll();
    destroySwapchain();
    {
        std::lock_guard<std::mutex> wl(windowMutex_);
        if (pendingWindow_) { ANativeWindow_release(pendingWindow_); pendingWindow_ = nullptr; }
        windowChanged_ = false;
    }
    if (window_) { ANativeWindow_release(window_); window_ = nullptr; }
    destroyResources();
    for (auto& s : slots_) {
        if (s.fence) vkDestroyFence(device_, s.fence, nullptr);
        if (s.acquireSem) vkDestroySemaphore(device_, s.acquireSem, nullptr);
        s = Slot{};
    }
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    if (unpackPipe_) vkDestroyPipeline(device_, unpackPipe_, nullptr);
    if (renderPipe_) vkDestroyPipeline(device_, renderPipe_, nullptr);
    if (cleanPipe_) vkDestroyPipeline(device_, cleanPipe_, nullptr);
    if (alignPipe_) vkDestroyPipeline(device_, alignPipe_, nullptr);
    if (alignPipeLayout_) vkDestroyPipelineLayout(device_, alignPipeLayout_, nullptr);
    if (alignLayout_) vkDestroyDescriptorSetLayout(device_, alignLayout_, nullptr);
    alignPipe_ = VK_NULL_HANDLE;
    alignPipeLayout_ = VK_NULL_HANDLE;
    alignLayout_ = VK_NULL_HANDLE;
    if (cleanPipeLayout_) vkDestroyPipelineLayout(device_, cleanPipeLayout_, nullptr);
    if (cleanLayout_) vkDestroyDescriptorSetLayout(device_, cleanLayout_, nullptr);
    cleanPipe_ = VK_NULL_HANDLE;
    cleanPipeLayout_ = VK_NULL_HANDLE;
    cleanLayout_ = VK_NULL_HANDLE;
    if (unpackPipeLayout_) vkDestroyPipelineLayout(device_, unpackPipeLayout_, nullptr);
    if (renderPipeLayout_) vkDestroyPipelineLayout(device_, renderPipeLayout_, nullptr);
    if (unpackLayout_) vkDestroyDescriptorSetLayout(device_, unpackLayout_, nullptr);
    if (renderLayout_) vkDestroyDescriptorSetLayout(device_, renderLayout_, nullptr);
    if (descPool_) vkDestroyDescriptorPool(device_, descPool_, nullptr);
    if (cmdPool_) vkDestroyCommandPool(device_, cmdPool_, nullptr);
    if (queryPool_) vkDestroyQueryPool(device_, queryPool_, nullptr);
    queryPool_ = VK_NULL_HANDLE;
    vkDestroyDevice(device_, nullptr);
    vkDestroyInstance(instance_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    unpackPipe_ = renderPipe_ = VK_NULL_HANDLE;
    unpackPipeLayout_ = renderPipeLayout_ = VK_NULL_HANDLE;
    unpackLayout_ = renderLayout_ = VK_NULL_HANDLE;
    descPool_ = VK_NULL_HANDLE;
    cmdPool_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    geom_ = {};
    initialized_ = false;
}

// Waits for every in-flight frame and every encoder hold.
void VulkanEngine::drainAll() {
    if (!device_) return;
    {
        std::unique_lock<std::mutex> lk(encoderMutex_);
        encoderCv_.wait_for(lk, std::chrono::seconds(2), [&] {
            for (auto& s : slots_) if (s.encoderHold) return false;
            return true;
        });
    }
    vkQueueWaitIdle(queue_);
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

int32_t VulkanEngine::findMemoryType(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
    for (int pass = 0; pass < 2; ++pass) {
        VkMemoryPropertyFlags want = pass == 0 ? (required | preferred) : required;
        for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & want) == want) return static_cast<int32_t>(i);
        }
    }
    return -1;
}

bool VulkanEngine::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostCached, Buffer& out) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, out.buffer, &req);
    // Uploads: plain HOST_VISIBLE|COHERENT (write-combined, ideal for memcpy).
    // Read-backs: HOST_CACHED, otherwise CPU reads crawl through uncached memory.
    VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    VkMemoryPropertyFlags preferred = hostCached ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int32_t type = findMemoryType(req.memoryTypeBits, required, preferred);
    if (type < 0) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = static_cast<uint32_t>(type);
    if (vkAllocateMemory(device_, &ai, nullptr, &out.memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(device_, out.buffer, out.memory, 0);
    if (vkMapMemory(device_, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) return false;
    out.size = size;
    out.coherent = (memProps_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return true;
}

void VulkanEngine::destroyBuffer(Buffer& b) {
    if (b.mapped) vkUnmapMemory(device_, b.memory);
    if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    b = Buffer{};
}

bool VulkanEngine::createImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, Image& out) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ii, nullptr, &out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, out.image, &req);
    int32_t type = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type < 0) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = static_cast<uint32_t>(type);
    if (vkAllocateMemory(device_, &ai, nullptr, &out.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, out.image, out.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(device_, &vi, nullptr, &out.view) == VK_SUCCESS;
}

void VulkanEngine::destroyImage(Image& img) {
    if (img.view) vkDestroyImageView(device_, img.view, nullptr);
    if (img.image) vkDestroyImage(device_, img.image, nullptr);
    if (img.memory) vkFreeMemory(device_, img.memory, nullptr);
    img = Image{};
}

void VulkanEngine::destroyResources() {
    for (auto& s : slots_) {
        destroyBuffer(s.raw);
        destroyBuffer(s.shading);
        destroyBuffer(s.params);
        destroyBuffer(s.p010);
        destroyBuffer(s.vfReadback);
        s.vfReadbackPending = false;
    }
    destroyImage(quadImage_);
    destroyImage(cleanImage_);
    destroyImage(historyImage_);
    destroyImage(curLowImage_);
    destroyImage(histLowImage_);
    destroyImage(motionImage_);
    destroyImage(vfImage_);
    historyValid_ = false;
    alignThrottled_ = false;
    overBudgetFrames_ = 0;
    geom_ = {};
}

bool VulkanEngine::ensureResources(const Geometry& g) {
    if (g == geom_ && quadImage_.image) return true;
    drainAll();
    destroyResources();

    const uint32_t quadW = static_cast<uint32_t>(g.rawW / 2), quadH = static_cast<uint32_t>(g.rawH / 2);
    const VkDeviceSize rawBytes = static_cast<VkDeviceSize>(g.stride) * g.rawH + 8; // +8: shader reads whole words
    const VkDeviceSize shadingBytes = sizeof(float) * std::max(g.shadingFloats, 4);
    const VkDeviceSize p010Bytes = static_cast<VkDeviceSize>(g.outW) * g.outH * 3;
    const VkDeviceSize vfBytes = static_cast<VkDeviceSize>(g.outW) * g.outH * 4;

    for (auto& s : slots_) {
        if (!createBuffer(rawBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, s.raw) ||
            !createBuffer(shadingBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, s.shading) ||
            !createBuffer(sizeof(FrameParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, false, s.params) ||
            !createBuffer(p010Bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, s.p010) ||
            !createBuffer(vfBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, s.vfReadback)) {
            VK_LOGE("Failed to allocate per-slot buffers");
            destroyResources();
            return false;
        }
    }
    if (!createImage(quadW, quadH, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, quadImage_) ||
        !createImage(quadW, quadH, VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, cleanImage_) ||
        !createImage(quadW, quadH, VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, historyImage_) ||
        !createImage((quadW + 3) / 4, (quadH + 3) / 4, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, curLowImage_) ||
        !createImage((quadW + 3) / 4, (quadH + 3) / 4, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, histLowImage_) ||
        !createImage((quadW + 31) / 32, (quadH + 31) / 32, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, motionImage_) ||
        !createImage(static_cast<uint32_t>(g.outW), static_cast<uint32_t>(g.outH), VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, vfImage_)) {
        VK_LOGE("Failed to allocate intermediate images");
        destroyResources();
        return false;
    }

    // Both images live in GENERAL for their whole lifetime.
    VkCommandBuffer cb = slots_[0].cmd;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier bars[7] = {
        imageBarrier(curLowImage_.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
        imageBarrier(histLowImage_.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
        imageBarrier(motionImage_.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
        imageBarrier(quadImage_.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
        imageBarrier(cleanImage_.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
        imageBarrier(historyImage_.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
        imageBarrier(vfImage_.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 7, bars);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);

    geom_ = g;
    for (auto& s : slots_) writeDescriptors(s);
    swapchainStale_ = true; // viewfinder extent may need to follow the new output size
    VK_LOGI("GPU resources: raw %dx%d (stride %d) -> quad %ux%u -> out %dx%d",
            g.rawW, g.rawH, g.stride, quadW, quadH, g.outW, g.outH);
    return true;
}

void VulkanEngine::writeDescriptors(Slot& s) {
    VkDescriptorBufferInfo params{s.params.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo raw{s.raw.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo shading{s.shading.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo p010{s.p010.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo quadStorage{VK_NULL_HANDLE, quadImage_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo cleanStorage{VK_NULL_HANDLE, cleanImage_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo historyStorage{VK_NULL_HANDLE, historyImage_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo quadSampled{sampler_, cleanImage_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo curLow{VK_NULL_HANDLE, curLowImage_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo histLow{VK_NULL_HANDLE, histLowImage_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo motion{VK_NULL_HANDLE, motionImage_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo vf{VK_NULL_HANDLE, vfImage_.view, VK_IMAGE_LAYOUT_GENERAL};

    auto w = [](VkDescriptorSet set, uint32_t binding, VkDescriptorType type) {
        VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        x.dstSet = set;
        x.dstBinding = binding;
        x.descriptorCount = 1;
        x.descriptorType = type;
        return x;
    };
    VkWriteDescriptorSet writes[19] = {
        w(s.unpackSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        w(s.unpackSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        w(s.unpackSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        w(s.unpackSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        w(s.renderSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        w(s.renderSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        w(s.renderSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        w(s.renderSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        w(s.cleanSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        w(s.cleanSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        w(s.cleanSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        w(s.cleanSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        w(s.cleanSet, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        w(s.alignSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        w(s.alignSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        w(s.alignSet, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        w(s.alignSet, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        w(s.alignSet, 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        w(s.alignSet, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
    };
    writes[0].pBufferInfo = &params;
    writes[1].pBufferInfo = &raw;
    writes[2].pBufferInfo = &shading;
    writes[3].pImageInfo = &quadStorage;
    writes[4].pBufferInfo = &params;
    writes[5].pImageInfo = &quadSampled;
    writes[6].pImageInfo = &vf;
    writes[7].pBufferInfo = &p010;
    writes[8].pBufferInfo = &params;
    writes[9].pImageInfo = &quadStorage;
    writes[10].pImageInfo = &historyStorage;
    writes[11].pImageInfo = &cleanStorage;
    writes[12].pImageInfo = &motion;
    writes[13].pBufferInfo = &params;
    writes[14].pImageInfo = &quadStorage;
    writes[15].pImageInfo = &historyStorage;
    writes[16].pImageInfo = &curLow;
    writes[17].pImageInfo = &histLow;
    writes[18].pImageInfo = &motion;
    vkUpdateDescriptorSets(device_, 19, writes, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Viewfinder
// ---------------------------------------------------------------------------

void VulkanEngine::setViewfinderWindow(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(windowMutex_);
    if (pendingWindow_) ANativeWindow_release(pendingWindow_);
    pendingWindow_ = window;
    if (pendingWindow_) ANativeWindow_acquire(pendingWindow_);
    windowChanged_ = true;
}

void VulkanEngine::applyPendingWindow() {
    ANativeWindow* next = nullptr;
    {
        std::lock_guard<std::mutex> lock(windowMutex_);
        if (!windowChanged_) return;
        next = pendingWindow_;
        pendingWindow_ = nullptr;
        windowChanged_ = false;
    }
    vkQueueWaitIdle(queue_);
    for (auto& s : slots_) s.vfReadbackPending = false;
    destroySwapchain();
    if (window_) ANativeWindow_release(window_);
    window_ = next; // already acquired in setViewfinderWindow
    cpuFallback_ = false;
    if (window_ && geom_.outW > 0 && !createSwapchain()) {
        VK_LOGW("Swapchain unavailable for viewfinder window — using CPU copy fallback");
        cpuFallback_ = true;
    }
}

bool VulkanEngine::createSwapchain() {
    if (!haveSwapchainExt_ || !window_) return false;
    VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    sci.window = window_;
    if (vkCreateAndroidSurfaceKHR(instance_, &sci, nullptr, &surface_) != VK_SUCCESS) return false;
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical_, queueFamily_, surface_, &supported);
    if (!supported) { destroySwapchain(); return false; }

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps);
    uint32_t fc = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fc, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fc);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fc, formats.data());
    VkSurfaceFormatKHR chosen{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (auto f : formats) if (f.format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = f; break; }
    if (chosen.format == VK_FORMAT_UNDEFINED)
        for (auto f : formats) if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = f; break; }
    if (chosen.format == VK_FORMAT_UNDEFINED) { destroySwapchain(); return false; }
    swapRB_ = chosen.format == VK_FORMAT_B8G8R8A8_UNORM;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) extent = {static_cast<uint32_t>(geom_.outW), static_cast<uint32_t>(geom_.outH)};
    bool sameSize = extent.width == static_cast<uint32_t>(geom_.outW) && extent.height == static_cast<uint32_t>(geom_.outH);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) || (!sameSize && !queueHasGraphics_)) {
        destroySwapchain();
        return false;
    }

    // FIFO is always available; MAILBOX avoids ever back-pressuring the camera.
    uint32_t pmc = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pmc, nullptr);
    std::vector<VkPresentModeKHR> modes(pmc);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pmc, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_) != VK_SUCCESS) { destroySwapchain(); return false; }

    uint32_t ic = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &ic, nullptr);
    swapImages_.resize(ic);
    vkGetSwapchainImagesKHR(device_, swapchain_, &ic, swapImages_.data());
    swapRenderDone_.assign(ic, VK_NULL_HANDLE);
    for (auto& sem : swapRenderDone_) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device_, &si, nullptr, &sem);
    }
    swapExtent_ = extent;
    swapchainStale_ = false;
    VK_LOGI("Viewfinder swapchain %ux%u, %u images, format %d, mode %d", extent.width, extent.height, ic, chosen.format, mode);
    return true;
}

void VulkanEngine::destroySwapchain() {
    if (!device_) return;
    if (swapchain_ || surface_) vkQueueWaitIdle(queue_);
    for (auto sem : swapRenderDone_) if (sem) vkDestroySemaphore(device_, sem, nullptr);
    swapRenderDone_.clear();
    swapImages_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
}

void VulkanEngine::presentCpuFallback(Slot& s) {
    if (!s.vfReadbackPending || !window_) return;
    s.vfReadbackPending = false;
    if (!s.vfReadback.coherent) {
        VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        r.memory = s.vfReadback.memory;
        r.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(device_, 1, &r);
    }
    ANativeWindow_setBuffersGeometry(window_, geom_.outW, geom_.outH, WINDOW_FORMAT_RGBA_8888);
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(window_, &buf, nullptr) != 0) return;
    const auto* src = static_cast<const uint8_t*>(s.vfReadback.mapped);
    auto* dst = static_cast<uint8_t*>(buf.bits);
    int rows = std::min(buf.height, geom_.outH);
    size_t rowBytes = static_cast<size_t>(std::min(buf.width, geom_.outW)) * 4;
    for (int y = 0; y < rows; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * buf.stride * 4, src + static_cast<size_t>(y) * geom_.outW * 4, rowBytes);
    }
    ANativeWindow_unlockAndPost(window_);
}

// ---------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------

bool VulkanEngine::processFrame(const FrameInput& in, int* encoderSlot) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (!initialized_ || !in.raw) return false;
    auto t0 = Clock::now();

    Geometry g;
    g.rawW = in.params.rawInfo[0];
    g.rawH = in.params.rawInfo[1];
    g.stride = in.params.rawInfo[2];
    g.outW = in.params.outInfo[0];
    g.outH = in.params.outInfo[1];
    g.shadingFloats = in.shading ? static_cast<int>(in.shadingFloats) : 0;
    if (g.rawW <= 0 || g.rawH <= 0 || g.outW <= 0 || g.outH <= 0 || (g.outW & 1) || (g.outH & 1)) return false;
    if (in.rawSize > static_cast<size_t>(g.stride) * g.rawH) return false;
    if (!ensureResources(g)) return false;
    applyPendingWindow();
    if (swapchainStale_ && window_ && !cpuFallback_) {
        destroySwapchain();
        if (!createSwapchain()) cpuFallback_ = true;
    }

    int idx = nextSlot_;
    Slot& s = slots_[idx];
    {
        std::unique_lock<std::mutex> lk(encoderMutex_);
        if (!encoderCv_.wait_for(lk, std::chrono::milliseconds(250), [&] { return !s.encoderHold; })) {
            ++droppedFrames_;
            nextSlot_ = (nextSlot_ + 1) % kRingSize; // don't wedge on one stuck slot
            VK_LOGW("Frame dropped: encoder still holds slot %d", idx);
            return false;
        }
    }
    if (vkWaitForFences(device_, 1, &s.fence, VK_TRUE, kFenceTimeoutNs) != VK_SUCCESS) {
        ++droppedFrames_;
        nextSlot_ = (nextSlot_ + 1) % kRingSize;
        VK_LOGW("Frame dropped: GPU still busy with slot %d", idx);
        return false;
    }
    auto tWait = Clock::now();
    readTimestamps(idx);
    presentCpuFallback(s);

    std::memcpy(s.raw.mapped, in.raw, in.rawSize);
    if (in.shading && g.shadingFloats > 0) std::memcpy(s.shading.mapped, in.shading, sizeof(float) * g.shadingFloats);
    FrameParams params = in.params;
    params.flags[0] = encoderSlot ? 1 : 0;
    params.flags[1] = swapRB_ && swapchain_ ? 1 : 0;
    params.flags[2] = window_ ? 1 : 0;
    if (!g.shadingFloats) { params.quadInfo[2] = 0; params.quadInfo[3] = 0; }
    const bool temporal = params.cleanFlags[1] != 0;
    params.cleanFlags[2] = temporal && historyValid_ ? 1 : 0;
    const bool align = params.cleanFlags[2] != 0 && params.cleanFlags[3] != 0 && !alignThrottled_;
    if (!align) params.cleanFlags[3] = 0;
    std::memcpy(s.params.mapped, &params, sizeof(params));
    auto tCopy = Clock::now();

    // Viewfinder target for this frame (never block the camera on the display).
    uint32_t imageIndex = UINT32_MAX;
    if (swapchain_ && window_) {
        VkResult ar = vkAcquireNextImageKHR(device_, swapchain_, 0, s.acquireSem, VK_NULL_HANDLE, &imageIndex);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_ERROR_SURFACE_LOST_KHR) {
            swapchainStale_ = true;
            imageIndex = UINT32_MAX;
        } else if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            imageIndex = UINT32_MAX; // VK_NOT_READY / VK_TIMEOUT: skip the viewfinder this frame
        }
    }

    VkCommandBuffer cb = s.cmd;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    // Host writes -> shader reads, and the previous frame's use of the shared
    // quad/viewfinder images -> this frame's writes.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);

    const uint32_t q0 = static_cast<uint32_t>(idx) * kStamps;
    if (queryPool_) {
        vkCmdResetQueryPool(cb, queryPool_, q0, kStamps);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool_, q0);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, unpackPipe_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, unpackPipeLayout_, 0, 1, &s.unpackSet, 0, nullptr);
    uint32_t groups = static_cast<uint32_t>((g.rawW + 3) / 4);
    vkCmdDispatch(cb, (groups + 15) / 16, static_cast<uint32_t>((g.rawH / 2 + 7) / 8), 1);

    VkImageMemoryBarrier quadReady = imageBarrier(quadImage_.image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &quadReady);

    if (queryPool_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queryPool_, q0 + 1);
    if (align) {
        // Motion field: 1/4-res luma of current + history, then a per-tile search.
        const uint32_t qw = static_cast<uint32_t>(g.rawW / 2), qh = static_cast<uint32_t>(g.rawH / 2);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, alignPipe_);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, alignPipeLayout_, 0, 1, &s.alignSet, 0, nullptr);
        int32_t mode = 0;
        vkCmdPushConstants(cb, alignPipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mode), &mode);
        vkCmdDispatch(cb, ((qw + 3) / 4 + 15) / 16, ((qh + 3) / 4 + 7) / 8, 1);
        VkMemoryBarrier lowReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        lowReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        lowReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             1, &lowReady, 0, nullptr, 0, nullptr);
        mode = 1;
        vkCmdPushConstants(cb, alignPipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mode), &mode);
        vkCmdDispatch(cb, (qw + 31) / 32, (qh + 31) / 32, 1); // one workgroup per tile
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             1, &lowReady, 0, nullptr, 0, nullptr);
    }

    if (queryPool_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queryPool_, q0 + 2);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cleanPipe_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cleanPipeLayout_, 0, 1, &s.cleanSet, 0, nullptr);
    vkCmdDispatch(cb, static_cast<uint32_t>((g.rawW / 2 + 15) / 16), static_cast<uint32_t>((g.rawH / 2 + 7) / 8), 1);

    VkImageMemoryBarrier cleanReady = imageBarrier(cleanImage_.image, VK_ACCESS_SHADER_WRITE_BIT,
                                                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                                                   VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &cleanReady);
    if (temporal) {
        // This frame's result becomes next frame's history.
        VkImageMemoryBarrier histWrite = imageBarrier(historyImage_.image, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &histWrite);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.extent = {static_cast<uint32_t>(g.rawW / 2), static_cast<uint32_t>(g.rawH / 2), 1};
        vkCmdCopyImage(cb, cleanImage_.image, VK_IMAGE_LAYOUT_GENERAL, historyImage_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    }
    historyValid_ = temporal;

    if (queryPool_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queryPool_, q0 + 3);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, renderPipe_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, renderPipeLayout_, 0, 1, &s.renderSet, 0, nullptr);
    vkCmdDispatch(cb, static_cast<uint32_t>((g.outW / 2 + 7) / 8), static_cast<uint32_t>((g.outH / 2 + 7) / 8), 1);

    if (window_) {
        VkImageMemoryBarrier vfReady = imageBarrier(vfImage_.image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &vfReady);
        if (imageIndex != UINT32_MAX) {
            VkImage dst = swapImages_[imageIndex];
            VkImageMemoryBarrier toDst = imageBarrier(dst, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &toDst);
            if (swapExtent_.width == static_cast<uint32_t>(g.outW) && swapExtent_.height == static_cast<uint32_t>(g.outH)) {
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.dstSubresource = region.srcSubresource;
                region.extent = {swapExtent_.width, swapExtent_.height, 1};
                vkCmdCopyImage(cb, vfImage_.image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            } else {
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.dstSubresource = blit.srcSubresource;
                blit.srcOffsets[1] = {g.outW, g.outH, 1};
                blit.dstOffsets[1] = {static_cast<int32_t>(swapExtent_.width), static_cast<int32_t>(swapExtent_.height), 1};
                vkCmdBlitImage(cb, vfImage_.image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &blit, VK_FILTER_LINEAR);
            }
            VkImageMemoryBarrier toPresent = imageBarrier(dst, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &toPresent);
        } else if (cpuFallback_) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {static_cast<uint32_t>(g.outW), static_cast<uint32_t>(g.outH), 1};
            vkCmdCopyImageToBuffer(cb, vfImage_.image, VK_IMAGE_LAYOUT_GENERAL, s.vfReadback.buffer, 1, &region);
            s.vfReadbackPending = true;
        }
    }

    // Device writes -> host reads (a fence alone doesn't make them visible).
    VkMemoryBarrier toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &toHost, 0, nullptr, 0, nullptr);
    if (queryPool_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_, q0 + 4);
    s.timed = queryPool_ != VK_NULL_HANDLE;
    vkEndCommandBuffer(cb);

    vkResetFences(device_, 1, &s.fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (imageIndex != UINT32_MAX) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s.acquireSem;
        si.pWaitDstStageMask = &waitStage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &swapRenderDone_[imageIndex];
    }
    if (vkQueueSubmit(queue_, 1, &si, s.fence) != VK_SUCCESS) {
        VK_LOGE("vkQueueSubmit failed");
        return false;
    }
    if (imageIndex != UINT32_MAX) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &swapRenderDone_[imageIndex];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &imageIndex;
        VkResult pr = vkQueuePresentKHR(queue_, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_ERROR_SURFACE_LOST_KHR) swapchainStale_ = true;
    }

    if (encoderSlot) {
        std::lock_guard<std::mutex> lk(encoderMutex_);
        s.encoderHold = true;
        *encoderSlot = idx;
    }
    nextSlot_ = (nextSlot_ + 1) % kRingSize;

    accWaitMs_ += std::chrono::duration<double, std::milli>(tWait - t0).count();
    accCopyMs_ += std::chrono::duration<double, std::milli>(tCopy - tWait).count();
    accSubmitMs_ += msSince(tCopy);
    if (++frameCount_ % 120 == 0) {
        VK_LOGI("Frame %llu: avg wait %.2fms, upload %.2fms, record+submit %.2fms, dropped %llu, vf=%s",
                static_cast<unsigned long long>(frameCount_), accWaitMs_ / 120, accCopyMs_ / 120, accSubmitMs_ / 120,
                static_cast<unsigned long long>(droppedFrames_),
                swapchain_ ? "swapchain" : (cpuFallback_ ? "cpu-fallback" : "none"));
        if (timedFrames_ > 0) {
            VK_LOGI("GPU per frame: unpack %.2fms, align %.2fms, clean %.2fms, render+present %.2fms = %.2fms (budget %.1fms)%s",
                    passMs_[0] / timedFrames_, passMs_[1] / timedFrames_, passMs_[2] / timedFrames_, passMs_[3] / timedFrames_,
                    gpuFrameMs_, frameBudgetMs_, alignThrottled_ ? ", alignment throttled" : "");
            for (double& m : passMs_) m = 0;
            timedFrames_ = 0;
        }
        accWaitMs_ = accCopyMs_ = accSubmitMs_ = 0;
    }
    return true;
}

// Reads the slot's previous-frame timestamps (its fence has signalled) and
// switches tile alignment off if the GPU can't keep up with the camera.
void VulkanEngine::readTimestamps(int slot) {
    Slot& s = slots_[slot];
    if (!queryPool_ || !s.timed) return;
    s.timed = false;
    uint64_t t[kStamps];
    if (vkGetQueryPoolResults(device_, queryPool_, static_cast<uint32_t>(slot) * kStamps, kStamps, sizeof(t), t,
                              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    for (uint32_t i = 0; i + 1 < kStamps; ++i) passMs_[i] += (t[i + 1] - t[i]) * timestampPeriodNs_ * 1e-6;
    ++timedFrames_;
    const double frameMs = (t[kStamps - 1] - t[0]) * timestampPeriodNs_ * 1e-6;
    gpuFrameMs_ = gpuFrameMs_ == 0 ? frameMs : 0.9 * gpuFrameMs_ + 0.1 * frameMs;
    // Sustained >85% of the frame interval means we are about to drop frames:
    // alignment is the optional, most expensive pass, so it goes first.
    overBudgetFrames_ = gpuFrameMs_ > 0.85 * frameBudgetMs_ ? overBudgetFrames_ + 1 : 0;
    if (overBudgetFrames_ > 30 && !alignThrottled_) {
        alignThrottled_ = true;
        VK_LOGW("GPU %.1fms over %.1fms budget: temporal NR alignment disabled", gpuFrameMs_, frameBudgetMs_);
    }
}

const uint8_t* VulkanEngine::waitEncoderFrame(int slot, size_t* size) {
    if (slot < 0 || slot >= kRingSize) return nullptr;
    Slot& s = slots_[slot];
    // The camera thread won't reset this fence while encoderHold is set.
    if (vkWaitForFences(device_, 1, &s.fence, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS) return nullptr;
    if (!s.p010.coherent) {
        VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        r.memory = s.p010.memory;
        r.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(device_, 1, &r);
    }
    if (size) *size = static_cast<size_t>(geom_.outW) * geom_.outH * 3;
    return static_cast<const uint8_t*>(s.p010.mapped);
}

void VulkanEngine::releaseEncoderFrame(int slot) {
    if (slot < 0 || slot >= kRingSize) return;
    {
        std::lock_guard<std::mutex> lk(encoderMutex_);
        slots_[slot].encoderHold = false;
    }
    encoderCv_.notify_all();
}

} // namespace vesper
