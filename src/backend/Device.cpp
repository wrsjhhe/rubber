#include "Device.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>
namespace sim::vk
{
void check(VkResult r, const char* op)
{
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(op) + " (VkResult " + std::to_string(r) + ")");
}
Lifetime::~Lifetime()
{
    clear();
}
void Lifetime::clear()
{
    for (auto it = release.rbegin(); it != release.rend(); ++it)
        (*it)();
    release.clear();
}
void Device::init(const DeviceOptions& options)
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SimRenderer";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(options.instanceExtensions.size());
    ci.ppEnabledExtensionNames = options.instanceExtensions.data();
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (options.validation)
    {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
    }
    check(vkCreateInstance(&ci, nullptr, &instance), "create instance");
    lifetime.release.push_back([this] { vkDestroyInstance(instance, nullptr); });
    if (options.createSurface)
    {
        surface = options.createSurface(instance);
        if (!surface)
            throw std::runtime_error("Surface creation failed");
        lifetime.release.push_back([this] { vkDestroySurfaceKHR(instance, surface, nullptr); });
    }
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate GPUs");
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate GPUs");
    for (auto gpu : devices)
    {
        VkPhysicalDeviceVulkan13Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features};
        vkGetPhysicalDeviceFeatures2(gpu, &f);
        vkGetPhysicalDeviceProperties(gpu, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3 || !features.dynamicRendering ||
            properties.limits.maxColorAttachments < 5)
            continue;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
        for (uint32_t i = 0; i < count; ++i)
        {
            VkBool32 present = VK_TRUE;
            if (surface)
                check(vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present), "surface support");
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present)
            {
                physical = gpu;
                family = i;
                break;
            }
        }
        if (physical)
            break;
    }
    if (!physical)
        throw std::runtime_error("Requires Vulkan 1.3 dynamic rendering and five MRTs");
    float priority = 1;
    VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qc.queueFamilyIndex = family;
    qc.queueCount = 1;
    qc.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f};
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &qc;
    const char* swap = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (surface)
    {
        dc.enabledExtensionCount = 1;
        dc.ppEnabledExtensionNames = &swap;
    }
    check(vkCreateDevice(physical, &dc, nullptr, &device), "create device");
    lifetime.release.push_back([this] { vkDestroyDevice(device, nullptr); });
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.queueFamilyIndex = family;
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check(vkCreateCommandPool(device, &pc, nullptr, &pool), "create command pool");
    lifetime.release.push_back([this] { vkDestroyCommandPool(device, pool, nullptr); });
}
uint32_t Device::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
{
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(physical, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    throw std::runtime_error("No compatible memory type");
}
Buffer::~Buffer()
{
    if (mapped)
        vkUnmapMemory(device, memory);
    vkDestroyBuffer(device, handle, nullptr);
    vkFreeMemory(device, memory, nullptr);
}
Buf Device::buffer(VkDeviceSize bytes, VkBufferUsageFlags usage, bool host) const
{
    auto b = std::make_unique<Buffer>();
    b->device = device;
    b->size = bytes;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = bytes;
    ci.usage = usage;
    check(vkCreateBuffer(device, &ci, nullptr, &b->handle), "create buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b->handle, &req);
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    a.allocationSize = req.size;
    a.memoryTypeIndex = memoryType(req.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device, &a, nullptr, &b->memory), "allocate buffer");
    check(vkBindBufferMemory(device, b->handle, b->memory, 0), "bind buffer");
    if (host)
        check(vkMapMemory(device, b->memory, 0, bytes, 0, &b->mapped), "map buffer");
    return b;
}
Image::~Image()
{
    vkDestroyImageView(device, view, nullptr);
    vkDestroyImage(device, handle, nullptr);
    vkFreeMemory(device, memory, nullptr);
}
Img Device::image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, uint32_t levels) const
{
    auto img = std::make_unique<Image>();
    img->device = device;
    img->extent = {w, h};
    img->aspect = format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    img->format = format;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.extent = {w, h, 1};
    img->levels = levels;
    ci.mipLevels = levels;
    ci.arrayLayers = 1;
    ci.format = format;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = usage;
    check(vkCreateImage(device, &ci, nullptr, &img->handle), "create image");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, img->handle, &req);
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    a.allocationSize = req.size;
    a.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device, &a, nullptr, &img->memory), "allocate image");
    check(vkBindImageMemory(device, img->handle, img->memory, 0), "bind image");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img->handle;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {img->aspect, 0, levels, 0, 1};
    check(vkCreateImageView(device, &vi, nullptr, &img->view), "create image view");
    return img;
}
namespace
{
struct Access
{
    VkPipelineStageFlags stage;
    VkAccessFlags access;
};
Access access(VkImageLayout l)
{
    switch (l)
    {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_GENERAL:
        return {VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    default:
        throw std::logic_error("Unknown image usage");
    }
}
}
void Image::transition(VkCommandBuffer cmd, VkImageLayout next)
{
    auto src = access(layout), dst = access(next);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src.access;
    b.dstAccessMask = dst.access;
    b.oldLayout = layout;
    b.newLayout = next;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = handle;
    b.subresourceRange = {aspect, 0, levels, 0, 1};
    vkCmdPipelineBarrier(cmd, src.stage, dst.stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    layout = next;
}
VkCommandBuffer Device::command() const
{
    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    a.commandPool = pool;
    a.commandBufferCount = 1;
    VkCommandBuffer cmd;
    check(vkAllocateCommandBuffers(device, &a, &cmd), "allocate command buffer");
    return cmd;
}
void Device::immediate(const std::function<void(VkCommandBuffer)>& record) const
{
    auto cmd = command();
    Lifetime life;
    life.release.push_back([&] { vkFreeCommandBuffers(device, pool, 1, &cmd); });
    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &b), "begin upload");
    record(cmd);
    check(vkEndCommandBuffer(cmd), "end upload");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkFence fence;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(device, &fi, nullptr, &fence), "upload fence");
    life.release.push_back([this, fence] { vkDestroyFence(device, fence, nullptr); });
    check(vkQueueSubmit(queue, 1, &si, fence), "submit upload");
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait upload");
}
Buf Device::upload(const void* data, size_t bytes, VkBufferUsageFlags usage) const
{
    auto staging = buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging->mapped, data, bytes);
    auto result = buffer(bytes, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    immediate(
        [&](auto cmd)
        {
            VkBufferCopy copy{0, 0, bytes};
            vkCmdCopyBuffer(cmd, staging->handle, result->handle, 1, &copy);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1,
                                 &barrier, 0, nullptr, 0, nullptr);
        });
    return result;
}
Img Device::texture(const Texture& t, bool srgb) const
{
    if (!t.width || !t.height || uint64_t(t.width) * t.height != t.rgba.size() / 4 || t.rgba.size() % 4)
        throw std::invalid_argument("Invalid texture size");
    auto staging = buffer(t.rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging->mapped, t.rgba.data(), t.rgba.size());
    auto format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
    constexpr auto required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    auto levels = (properties.optimalTilingFeatures & required) == required
                      ? std::bit_width(std::max(t.width, t.height))
                      : 1u;
    auto img = image(
        t.width, t.height, format,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, levels);
    immediate(
        [&](auto cmd)
        {
            img->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy c{};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {t.width, t.height, 1};
            vkCmdCopyBufferToImage(cmd, staging->handle, img->handle, img->layout, 1, &c);
            // 每一层在成为下一次缩小采样的来源前，单独转换布局。
            auto transitionLevel = [&](uint32_t level, VkImageLayout oldLayout, VkImageLayout next)
            {
                auto src = access(oldLayout), dst = access(next);
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barrier.srcAccessMask = src.access;
                barrier.dstAccessMask = dst.access;
                barrier.oldLayout = oldLayout;
                barrier.newLayout = next;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = img->handle;
                barrier.subresourceRange = {img->aspect, level, 1, 0, 1};
                vkCmdPipelineBarrier(cmd, src.stage, dst.stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            };
            int w = int(t.width), h = int(t.height);
            for (uint32_t level = 1; level < levels; ++level)
            {
                transitionLevel(level - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkImageBlit region{};
                region.srcSubresource = {img->aspect, level - 1, 0, 1};
                region.srcOffsets[1] = {w, h, 1};
                w = std::max(1, w / 2);
                h = std::max(1, h / 2);
                region.dstSubresource = {img->aspect, level, 0, 1};
                region.dstOffsets[1] = {w, h, 1};
                vkCmdBlitImage(cmd, img->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img->handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
                transitionLevel(level - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            transitionLevel(levels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            img->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        });
    return img;
}
}
