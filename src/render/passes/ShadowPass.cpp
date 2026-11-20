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
// 阴影：四级 CSM 写进同一张深度图集的四个象限，剔除按各自光源视锥。
void recordShadow(VkCommandBuffer cmd, const Pipelines& pipe, VkDescriptorSet frame, Image& shadow,
                  const std::vector<RenderScene::Item>& items, const Globals& globals, uint32_t tile)
{
    beginPass(cmd, {}, &shadow);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.shadow);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.meshLayout, 0, 1, &frame, 0, nullptr);
    for (int cascade = 0; cascade < 4; ++cascade)
    {
        viewport(cmd, (cascade % 2) * tile, (cascade / 2) * tile, tile, tile);
        for (uint32_t i = 0; i < items.size(); ++i)
        {
            auto& item = items[i];
            if (item.castsShadow && visible(item.world, item.minimum, item.maximum, globals.lightVP[cascade]))
                drawItem(cmd, pipe, item, i, cascade, false);
        }
    }
    vkCmdEndRendering(cmd);
}
}
