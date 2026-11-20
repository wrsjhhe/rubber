#include "Passes.hpp"

namespace sim::vk
{
// 读回：三个通道依次落到暂存缓冲的连续区间，偏移与分辨率对齐。
void recordReadback(VkCommandBuffer cmd, std::span<Image* const> sources, Buffer& staging, uint32_t width,
                    uint32_t height)
{
    VkDeviceSize offset = 0;
    for (auto image : sources)
    {
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, image->handle, image->layout, staging.handle, 1, &copy);
        offset += VkDeviceSize(width) * height * 4;
    }
}
}
