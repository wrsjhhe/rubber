#pragma once
#include "../backend/Device.hpp"
#include <array>
namespace sim::vk
{
constexpr VkFormat hdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
constexpr std::array<VkFormat, 5> gFormats{colorFormat, hdrFormat, hdrFormat, VK_FORMAT_R32_SFLOAT,
                                           VK_FORMAT_R32_UINT};
// 单次渲染的颜色附件上限：五个 G-buffer 通道再加 HDR，是当前最宽的一帧。
// Device 在选择设备时按同一数值要求 maxColorAttachments，两者的取值必须一致。
constexpr uint32_t maxColorAttachments = 6;
VkShaderModule loadModule(Device&, const std::filesystem::path&, Lifetime&);
// 管线状态由调用方声明：网格读顶点缓冲，点云用实例化公告板并由着色器生成几何，
// 因此顶点输入、深度比较与首附件混合都要可选。
struct GraphicsOptions
{
    bool vertexInput = true;   // 读取 Vertex 顶点布局
    bool depthTest = true;
    bool depthWrite = true;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS;
    bool depthBias = false;    // 阴影图
    int blendIndex = -1;       // 按预乘 alpha 混合的附件下标（点云叠加到 HDR）
};
VkPipeline graphicsPipeline(Device&, Lifetime&, const std::filesystem::path&, const char*, const char*,
                            VkPipelineLayout, std::span<const VkFormat>, const GraphicsOptions&);
struct Pipelines
{
    Device& d;
    Lifetime life;
    VkDescriptorSetLayout frame{}, material{}, lighting{}, resolve{}, colorSet{}, points{}, presentationSet{};
    VkPipelineLayout meshLayout{}, lightLayout{}, resolveLayout{}, colorLayout{}, pointsLayout{}, presentationLayout{};
    VkPipeline shadow{}, geometry{}, shade{}, temporal{}, color{}, splat{}, splatDepth{}, presentation{};
    VkSampler point{}, linear{}, repeat{};
    explicit Pipelines(Device&, const std::filesystem::path&);
};
VkDescriptorPool descriptorPool(Device&, Lifetime&, uint32_t sets);
VkDescriptorSet allocateSet(Device&, VkDescriptorPool, VkDescriptorSetLayout);
void bindImage(Device&, VkDescriptorSet, uint32_t, const Image&, VkSampler);
void bindBuffer(Device&, VkDescriptorSet, uint32_t, const Buffer&, VkDescriptorType);
void viewport(VkCommandBuffer, int x, int y, uint32_t w, uint32_t h);
// 只按当前布局进入渲染：布局必须已由帧图转换好，pass 内不再做隐式转换。
// load 为真时保留附件内容（在已有结果上叠加），否则清除。
void beginPass(VkCommandBuffer, std::span<Image* const> colors, Image* depth, bool load = false);
}
