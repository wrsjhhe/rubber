#include "Passes.hpp"

namespace sim::vk
{
// 光照：读取 G-buffer、深度与阴影图集，写线性 HDR 颜色。
void recordLighting(VkCommandBuffer cmd, const Pipelines& pipe, VkDescriptorSet frame, VkDescriptorSet inputs,
                    Image& hdr)
{
    std::array<Image*, 1> outputs{&hdr};
    drawFullscreen(cmd, pipe.shade, pipe.lightLayout, frame, inputs, outputs);
}
}
