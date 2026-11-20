#pragma once
#include "Output.hpp"
#include <vulkan/vulkan.h>

namespace sim
{
struct VulkanOutput
{
    VkImage image;
    VkImageView view;
    VkFormat format;
    VkImageLayout layout;
    VkFence completion;
};
class VulkanInterop
{
  public:
    // 调用方保活 FrameResult，并在其他队列使用前等待完成；不能修改图像布局或内容。
    static VulkanOutput output(const FrameResult&, Channel);
};
}
