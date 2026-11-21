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
struct Device;
// 一次提交完成的上传批次。录制阶段的复制源由批次持有，提交后通过时间线信号量
// 向使用该资产的帧提交声明依赖，而不是由每台相机重新同步上传。
struct UploadBatch
{
    Device& d;
    VkCommandBuffer cmd{};
    VkFence fence{};
    // 非零表示批次已提交，值即时间线信号量上的完成值。
    uint64_t signal = 0;
    std::vector<Buf> staging;
    explicit UploadBatch(Device& device);
    ~UploadBatch();
    UploadBatch(const UploadBatch&) = delete;
    bool empty() const
    {
        return !recording;
    }
    Buf upload(const void* data, size_t bytes, VkBufferUsageFlags usage);
    Img texture(const Texture&, bool srgb);
    // 本批次仍持有的复制源字节数；回收后归零。
    uint64_t stagingBytes() const;
    // 提交整批复制；返回后续帧提交需要等待的时间线值。
    uint64_t submit();
    bool complete() const;
    void wait();

  private:
    bool recording = false;
};
struct Device : DeviceHandles
{
    Lifetime lifetime;
    VkCommandPool pool{};
    VkPhysicalDeviceProperties properties{};
    // 上传完成时间线。每批上传递增一个值，帧提交等待对应值即建立提交依赖。
    VkSemaphore uploadTimeline{};
    uint64_t uploadValue = 0;
    void init(const DeviceOptions&);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
    Buf buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) const;
    Img image(uint32_t w, uint32_t h, VkFormat, VkImageUsageFlags usage, uint32_t levels = 1) const;
    VkCommandBuffer command() const;
    uint64_t nextUploadSignal();
};
}
