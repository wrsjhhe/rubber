#include "Passes.hpp"

namespace sim::vk
{
namespace
{
void drawItem(VkCommandBuffer cmd, const Pipelines& pipe, const RenderScene::Item& item, uint32_t index,
              int cascade, bool stable)
{
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &item.mesh->vertices->handle, &offset);
    vkCmdBindIndexBuffer(cmd, item.mesh->indices->handle, 0, VK_INDEX_TYPE_UINT32);
    if (cascade < 0)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.meshLayout, 1, 1, &item.materialSet, 0,
                                nullptr);
    Draw push{index, std::max(cascade, 0), stable};
    vkCmdPushConstants(cmd, pipe.meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDrawIndexed(cmd, item.mesh->count, 1, 0, 0, 0);
}
}
// 几何：一次渲染写五个 G-buffer 与深度。stable 版本使用无抖动投影，
// 供需要像素中心真值的消费者使用，同一帧内可以排在时序解析之后。
void recordGeometry(VkCommandBuffer cmd, const Pipelines& pipe, VkDescriptorSet frame,
                    const std::vector<RenderScene::Item>& items, const std::vector<uint32_t>& visible,
                    std::span<Image* const> colors, Image& depth, bool stable)
{
    beginPass(cmd, colors, &depth);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.geometry);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.meshLayout, 0, 1, &frame, 0, nullptr);
    for (auto i : visible)
        drawItem(cmd, pipe, items[i], i, -1, stable);
    vkCmdEndRendering(cmd);
}
}
