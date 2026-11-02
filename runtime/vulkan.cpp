// Backend Vulkan : instance, device, swapchain et présentation (via SDL3 + volk)
#include "vulkan.h"
#include "guest.h"
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>

static VkInstance s_instance;
static VkSurfaceKHR s_surface;
static VkPhysicalDevice s_physical;
static VkDevice s_device;
static VkQueue s_queue;
static uint32_t s_queueFamily;
static VkSwapchainKHR s_swapchain;
static VkFormat s_swapFormat;
static VkExtent2D s_extent;
static std::vector<VkImage> s_images;
static VkCommandPool s_pool;
static VkCommandBuffer s_cmd;
static VkSemaphore s_acquired, s_rendered;
// Deux images en vol : le thread GPU enregistre l'image n+1 pendant que la carte exécute l'image n
static constexpr uint32_t kFrames = 2;
static VkCommandBuffer s_cmds[kFrames];
static VkFence s_fences[kFrames];
static VkSemaphore s_acquireds[kFrames], s_rendereds[kFrames];
static uint32_t s_cur;
static VkFence s_fence;
static SDL_Window* s_window;
static std::mutex s_mutex;
static uint64_t s_frame;

// Capture (IKARUGA_SHOT=n) : relit l'image n présentée et l'écrit dans build/shot.bmp
static uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s_physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return 0;
}

static void WriteBmp(const char* path, const uint8_t* bgra, uint32_t w, uint32_t h)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t size = 54 + w * h * 4;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4); hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4); int32_t nh = -int32_t(h); memcpy(hdr + 22, &nh, 4);
    hdr[26] = 1; hdr[28] = 32;
    fwrite(hdr, 1, 54, f); fwrite(bgra, 4, size_t(w) * h, f); fclose(f);
}

#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { KLOG("Vulkan : %s -> %d", #x, r_); return false; } } while (0)

static bool CreateSwapchain()
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_physical, s_surface, &caps);
    s_extent = caps.currentExtent;
    if (s_extent.width == 0xFFFFFFFF)
    {
        int w, h; SDL_GetWindowSizeInPixels(s_window, &w, &h);
        s_extent = { uint32_t(w), uint32_t(h) };
    }
    if (s_extent.width == 0 || s_extent.height == 0) return false;   // fenêtre réduite

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_physical, s_surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_physical, s_surface, &count, formats.data());
    VkSurfaceFormatKHR format = formats[0];
    for (auto& f : formats) if (f.format == VK_FORMAT_B8G8R8A8_UNORM) format = f;
    s_swapFormat = format.format;

    VkSwapchainKHR old = s_swapchain;
    VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = s_surface;
    info.minImageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = s_extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = old;
    VK_CHECK(vkCreateSwapchainKHR(s_device, &info, nullptr, &s_swapchain));
    if (old) vkDestroySwapchainKHR(s_device, old, nullptr);

    vkGetSwapchainImagesKHR(s_device, s_swapchain, &count, nullptr);
    s_images.resize(count);
    vkGetSwapchainImagesKHR(s_device, s_swapchain, &count, s_images.data());
    return true;
}

bool VulkanInit(SDL_Window* window)
{
    s_window = window;
    VK_CHECK(volkInitialize());

    uint32_t extCount = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&extCount);
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Ikaruga";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = extCount;
    ici.ppEnabledExtensionNames = exts;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &s_instance));
    volkLoadInstance(s_instance);

    if (!SDL_Vulkan_CreateSurface(window, s_instance, nullptr, &s_surface)) { KLOG("SDL_Vulkan_CreateSurface : %s", SDL_GetError()); return false; }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(s_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(s_instance, &count, devices.data());
    for (auto d : devices)
    {
        uint32_t qc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qc, qs.data());
        for (uint32_t i = 0; i < qc; i++)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, s_surface, &present);
            if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present && !s_physical) { s_physical = d; s_queueFamily = i; }
        }
    }
    if (!s_physical) { KLOG("Vulkan : aucun GPU compatible"); return false; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(s_physical, &props);
    KLOG("Vulkan : %s", props.deviceName);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = s_queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    const char* devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    // Requis par le SPIR-V de XenosRecomp : adresses de buffers en push constants (uint64),
    // tableaux de descripteurs "bindless" indexés dynamiquement
    VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.bufferDeviceAddress = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f12;
    f2.features.shaderInt64 = VK_TRUE;
    f2.features.samplerAnisotropy = VK_TRUE;
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;
    VK_CHECK(vkCreateDevice(s_physical, &dci, nullptr, &s_device));
    volkLoadDevice(s_device);
    vkGetDeviceQueue(s_device, s_queueFamily, 0, &s_queue);

    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = s_queueFamily;
    VK_CHECK(vkCreateCommandPool(s_device, &pci, nullptr, &s_pool));
    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = s_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kFrames;
    VK_CHECK(vkAllocateCommandBuffers(s_device, &cai, s_cmds));
    VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < kFrames; i++)
    {
        vkCreateSemaphore(s_device, &sci, nullptr, &s_acquireds[i]);
        vkCreateSemaphore(s_device, &sci, nullptr, &s_rendereds[i]);
        vkCreateFence(s_device, &fci, nullptr, &s_fences[i]);
    }
    s_cmd = s_cmds[0]; s_fence = s_fences[0]; s_acquired = s_acquireds[0]; s_rendered = s_rendereds[0];

    return CreateSwapchain();
}

static void Barrier(VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static bool s_recording;

// Command buffer de l'image en cours (enregistré par le thread GPU entre deux présentations)
VkCommandBuffer VulkanFrameCmd()
{
    if (!s_recording)
    {
        s_cmd = s_cmds[s_cur]; s_fence = s_fences[s_cur]; s_acquired = s_acquireds[s_cur]; s_rendered = s_rendereds[s_cur];
        auto t0 = std::chrono::steady_clock::now();
        vkWaitForFences(s_device, 1, &s_fence, VK_TRUE, UINT64_MAX);
        extern std::atomic<uint64_t> g_gpuWaitUs[4];
        g_gpuWaitUs[2] += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        vkResetCommandBuffer(s_cmd, 0);
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(s_cmd, &begin);
        s_recording = true;
    }
    return s_cmd;
}

void VulkanImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1 };   // tous les niveaux de mip
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkDevice VulkanDevice() { return s_device; }
uint32_t VulkanFrameSlot() { return s_cur; }
VkPhysicalDevice VulkanPhysicalDevice() { return s_physical; }
uint32_t VulkanFindMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) { return FindMemoryType(bits, flags); }

// Termine l'image : copie (mise à l'échelle) de src vers la swapchain, soumission et présentation.
// src en VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ; sans src, image noire.
void VulkanPresentFrame(VkImage src, uint32_t width, uint32_t height)
{
    std::lock_guard lock(s_mutex);
    if (!s_device || !s_swapchain) return;
    VulkanFrameCmd();

    uint32_t index;
    auto t0 = std::chrono::steady_clock::now();
    VkResult res = vkAcquireNextImageKHR(s_device, s_swapchain, UINT64_MAX, s_acquired, VK_NULL_HANDLE, &index);
    extern std::atomic<uint64_t> g_gpuWaitUs[4];
    g_gpuWaitUs[3] += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    if (res == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // Image perdue : on soumet quand même le travail enregistré, sans présenter
        vkEndCommandBuffer(s_cmd);
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &s_cmd;
        vkResetFences(s_device, 1, &s_fence);
        vkQueueSubmit(s_queue, 1, &submit, s_fence);
        s_recording = false;
        s_cur = (s_cur + 1) % kFrames;
        vkDeviceWaitIdle(s_device); CreateSwapchain();
        return;
    }
    vkResetFences(s_device, 1, &s_fence);

    Barrier(s_images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkClearColorValue black{};
    vkCmdClearColorImage(s_cmd, s_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    if (src)
    {
        // Conserve le rapport d'aspect de l'image du jeu
        float scale = std::min(float(s_extent.width) / width, float(s_extent.height) / height);
        int32_t w = int32_t(width * scale), h = int32_t(height * scale);
        int32_t x = (int32_t(s_extent.width) - w) / 2, y = (int32_t(s_extent.height) - h) / 2;
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1] = { int32_t(width), int32_t(height), 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[0] = { x, y, 0 };
        blit.dstOffsets[1] = { x + w, y + h, 1 };
        vkCmdBlitImage(s_cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    static uint64_t shotFrame = getenv("IKARUGA_SHOT") ? strtoull(getenv("IKARUGA_SHOT"), nullptr, 10) : 0;
    VkBuffer shotBuf = VK_NULL_HANDLE; VkDeviceMemory shotMem = VK_NULL_HANDLE;
    bool shot = shotFrame && ++s_frame == shotFrame;
    if (shot)
    {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = VkDeviceSize(s_extent.width) * s_extent.height * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(s_device, &bci, nullptr, &shotBuf);
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(s_device, shotBuf, &req);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(s_device, &mai, nullptr, &shotMem);
        vkBindBufferMemory(s_device, shotBuf, shotMem, 0);
        Barrier(s_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { s_extent.width, s_extent.height, 1 };
        vkCmdCopyImageToBuffer(s_cmd, s_images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shotBuf, 1, &copy);
        Barrier(s_images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    else
        Barrier(s_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(s_cmd);
    s_recording = false;
    s_cur = (s_cur + 1) % kFrames;

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &s_acquired;
    submit.pWaitDstStageMask = &wait;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &s_cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &s_rendered;
    vkQueueSubmit(s_queue, 1, &submit, s_fence);

    if (shot)
    {
        vkWaitForFences(s_device, 1, &s_fence, VK_TRUE, UINT64_MAX);
        void* data; vkMapMemory(s_device, shotMem, 0, VK_WHOLE_SIZE, 0, &data);
        WriteBmp("build/shot.bmp", (const uint8_t*)data, s_extent.width, s_extent.height);
        vkUnmapMemory(s_device, shotMem);
        vkDestroyBuffer(s_device, shotBuf, nullptr); vkFreeMemory(s_device, shotMem, nullptr);
        KLOG("capture de l'image %llu -> build/shot.bmp (%ux%u)", (unsigned long long)s_frame, s_extent.width, s_extent.height);
    }

    VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &s_rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &s_swapchain;
    present.pImageIndices = &index;
    res = vkQueuePresentKHR(s_queue, &present);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(s_device); CreateSwapchain(); }
}
