#include "Passes.hpp"

namespace sim::vk
{
// 光照：读取 G-buffer 和当前阴影路径的资源，写线性 HDR 颜色。
void recordLighting(VkCommandBuffer cmd, const Pipelines& pipe, VkDescriptorSet frame, VkDescriptorSet inputs,
                    Image& hdr, bool rayContact)
{
    std::array<Image*, 1> outputs{&hdr};
    const auto pipeline = rayContact ? pipe.shadeRay : pipe.shade;
    const auto layout = rayContact ? pipe.rayLightLayout : pipe.lightLayout;
    drawFullscreen(cmd, pipeline, layout, frame, inputs, outputs);
}
}
