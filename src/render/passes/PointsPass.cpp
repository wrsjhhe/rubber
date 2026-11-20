#include "Passes.hpp"

namespace sim::vk
{
// 点云：一次实例化绘制，每个点展开成六个顶点组成一个正对相机的圆盘。
// 深度测试与回写让点云与网格互相遮挡；颜色链路在此之上按覆盖度混合到已光照的 HDR。
void recordSplat(VkCommandBuffer cmd, const Pipelines& pipe, VkDescriptorSet frame, VkDescriptorSet points,
                 std::span<Image* const> geometry, Image& depth, Image* hdr, uint32_t count, bool stable)
{
    if (!count)
        return;
    std::array<Image*, maxColorAttachments> attachments{};
    for (size_t i = 0; i < geometry.size(); ++i)
        attachments[i] = geometry[i];
    size_t colors = geometry.size();
    if (hdr)
        attachments[colors++] = hdr;
    // 保留已有内容：网格的光照结果与深度都要继续参与比较与混合。
    beginPass(cmd, std::span<Image* const>(attachments.data(), colors), &depth, true);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, stable ? pipe.splatDepth : pipe.splat);
    VkDescriptorSet sets[] = {frame, points};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.pointsLayout, 0, 2, sets, 0, nullptr);
    Draw push{0, 0, stable ? 1 : 0};
    vkCmdPushConstants(cmd, pipe.pointsLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(Draw), &push);
    vkCmdDraw(cmd, 6, count, 0, 0);
    vkCmdEndRendering(cmd);
}
}
