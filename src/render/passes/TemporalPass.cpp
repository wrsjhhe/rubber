#include "Passes.hpp"

namespace sim::vk
{
// 时序解析：读取当前 HDR、速度、米制深度与旧历史，写本帧的历史槽。
// 写历史是跨帧副作用，即使本次请求不需要颜色也要执行。
void recordTemporal(VkCommandBuffer cmd, const Pipelines& pipe, VkDescriptorSet frame, VkDescriptorSet inputs,
                    Image& history)
{
    std::array<Image*, 1> outputs{&history};
    drawFullscreen(cmd, pipe.temporal, pipe.resolveLayout, frame, inputs, outputs);
}
void recordHistoryClear(VkCommandBuffer cmd, Image& history)
{
    VkClearColorValue value{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, history.handle, history.layout, &value, 1, &range);
}
}
