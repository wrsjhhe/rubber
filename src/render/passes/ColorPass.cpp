#include "Passes.hpp"

namespace sim::vk
{
// 颜色变换：曝光、色调映射与 sRGB 编码，写显示图像。不修改时序历史。
void recordColor(VkCommandBuffer cmd, const Pipelines& pipe, VkDescriptorSet frame, VkDescriptorSet inputs,
                 Image& display)
{
    std::array<Image*, 1> outputs{&display};
    drawFullscreen(cmd, pipe.color, pipe.colorLayout, frame, inputs, outputs);
}
}
