#include "Presentation.hpp"
#include "../render/passes/Passes.hpp"

namespace sim::vk
{
// 调试呈现：读 G-buffer（法线、米制深度、实例标签）与级联视图所需的 HDR，
// 写自己的图像。函数体内没有任何对 Beauty/HDR 的写入。
void recordPresentation(VkCommandBuffer cmd, const Pipelines& pipe, VkDescriptorSet frame, VkDescriptorSet inputs,
                        Image& target)
{
    std::array<Image*, 1> outputs{&target};
    drawFullscreen(cmd, pipe.presentation, pipe.presentationLayout, frame, inputs, outputs);
}
}
