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
VkShaderModule loadModule(Device&, const std::filesystem::path&, Lifetime&);
VkPipeline graphicsPipeline(Device&, Lifetime&, const std::filesystem::path&, const char*, const char*,
                            VkPipelineLayout, std::span<const VkFormat>, bool, bool);
struct Pipelines
{
    Device& d;
    Lifetime life;
    VkDescriptorSetLayout frame{}, material{}, lighting{}, resolve{}, colorSet{};
    VkPipelineLayout meshLayout{}, lightLayout{}, resolveLayout{}, colorLayout{};
    VkPipeline shadow{}, geometry{}, shade{}, temporal{}, color{};
    VkSampler point{}, linear{}, repeat{};
    explicit Pipelines(Device&, const std::filesystem::path&);
};
VkDescriptorPool descriptorPool(Device&, Lifetime&, uint32_t sets);
VkDescriptorSet allocateSet(Device&, VkDescriptorPool, VkDescriptorSetLayout);
void bindImage(Device&, VkDescriptorSet, uint32_t, const Image&, VkSampler);
void bindBuffer(Device&, VkDescriptorSet, uint32_t, const Buffer&, VkDescriptorType);
void viewport(VkCommandBuffer, int x, int y, uint32_t w, uint32_t h);
// 只按当前布局进入渲染：布局必须已由帧图转换好，pass 内不再做隐式转换。
void beginPass(VkCommandBuffer, std::span<Image* const> colors, Image* depth);
}
