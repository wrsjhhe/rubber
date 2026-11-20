#pragma once
#include "../FrameData.hpp"
#include "../Pipeline.hpp"
#include "../RenderScene.hpp"
#include <span>

// 六个 pass 只负责录制绘制：进入渲染、绑定管线与描述符、发draw、结束渲染。
// 资源布局与屏障由帧图在 pass 前后处理，pass 内不得再做隐式状态转换。
namespace sim::vk
{
inline void drawFullscreen(VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet frame,
                           VkDescriptorSet inputs, std::span<Image* const> outputs)
{
    beginPass(cmd, outputs, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorSet sets[] = {frame, inputs};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}
void recordShadow(VkCommandBuffer, const Pipelines&, VkDescriptorSet frame, Image& shadow,
                  const std::vector<RenderScene::Item>&, const Globals&, uint32_t tile);
void recordGeometry(VkCommandBuffer, const Pipelines&, VkDescriptorSet frame,
                    const std::vector<RenderScene::Item>&, const std::vector<uint32_t>& visible,
                    std::span<Image* const> colors, Image& depth, bool stable);
void recordLighting(VkCommandBuffer, const Pipelines&, VkDescriptorSet frame, VkDescriptorSet inputs, Image& hdr);
void recordTemporal(VkCommandBuffer, const Pipelines&, VkDescriptorSet frame, VkDescriptorSet inputs,
                    Image& history);
// 历史失效（首帧、重置、时间回退）时先清空再采样，避免读到未定义内容。
void recordHistoryClear(VkCommandBuffer, Image& history);
void recordColor(VkCommandBuffer, const Pipelines&, VkDescriptorSet frame, VkDescriptorSet inputs, Image& display);
// 传感器读回：把显示、米制深度与实例标签复制到暂存缓冲。
void recordReadback(VkCommandBuffer, std::span<Image* const> sources, Buffer& staging, uint32_t width, uint32_t height);
}
