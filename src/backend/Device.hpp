#pragma once
#include "sim/Renderer.hpp"
#include <cstring>
#include <utility>
#include <span>

namespace sim::vk
{
void check(VkResult result, const char* operation);
struct Lifetime
{
    std::vector<std::function<void()>> release;
    Lifetime() = default;
    Lifetime(const Lifetime&) = delete;
    void clear();
    ~Lifetime();
};
struct Buffer
{
    VkDevice device{};
    VkBuffer handle{};
    VkDeviceMemory memory{};
    void* mapped{};
    VkDeviceSize size{};
    ~Buffer();
};
struct Image
{
    VkDevice device{};
    VkImage handle{};
    VkImageView view{};
    VkDeviceMemory memory{};
    uint32_t levels = 1;
    VkImageAspectFlags aspect{};
    VkFormat format{};
    VkExtent2D extent{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    ~Image();
    void transition(VkCommandBuffer, VkImageLayout);
};
using Buf = std::unique_ptr<Buffer>;
using Img = std::unique_ptr<Image>;
struct Device : DeviceHandles
{
    Lifetime lifetime;
    VkCommandPool pool{};
    VkPhysicalDeviceProperties properties{};
    void init(const DeviceOptions&);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
    Buf buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) const;
    Img image(uint32_t w, uint32_t h, VkFormat, VkImageUsageFlags usage, uint32_t levels = 1) const;
    VkCommandBuffer command() const;
    void immediate(const std::function<void(VkCommandBuffer)>& record) const;
    Buf upload(const void* data, size_t bytes, VkBufferUsageFlags usage) const;
    Img texture(const Texture&, bool srgb) const;
};
}
