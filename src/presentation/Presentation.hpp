#pragma once
#include "../render/Pipeline.hpp"

// 呈现层：把调试通道画到独立图像上。Beauty 与 HDR 由光照与时序解析产出，这里既不读它们的
// 写入意图也不回写，因此同一帧的调试图和真实颜色互不覆盖。
namespace sim::vk
{
void recordPresentation(VkCommandBuffer, const Pipelines&, VkDescriptorSet frame, VkDescriptorSet inputs,
                        Image& target);
}
