#pragma once
#include "Pipeline.hpp"
#include "graph/FrameGraph.hpp"
#include <cstdint>

// 声明层枚举与 Vulkan 之间的换算。渲染侧与模块共用同一份，避免各写一份而漂移。
namespace sim::vk
{
inline VkFormat vkFormatOf(render::Format format)
{
    switch (format)
    {
    case render::Format::Rgba8Unorm:
        return colorFormat;
    case render::Format::Rgba16Float:
        return hdrFormat;
    case render::Format::R32Float:
        return VK_FORMAT_R32_SFLOAT;
    case render::Format::R32Uint:
        return VK_FORMAT_R32_UINT;
    case render::Format::D32Float:
        return depthFormat;
    }
    return VK_FORMAT_UNDEFINED;
}
inline VkImageUsageFlags vkUsageOf(render::ImageUsage usage)
{
    VkImageUsageFlags flags = 0;
    if (has(usage, render::ImageUsage::Sampled))
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (has(usage, render::ImageUsage::ColorAttachment))
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (has(usage, render::ImageUsage::DepthAttachment))
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (has(usage, render::ImageUsage::TransferSrc))
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (has(usage, render::ImageUsage::TransferDst))
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return flags;
}
inline VkBufferUsageFlags vkUsageOf(render::BufferUsage usage)
{
    VkBufferUsageFlags flags = 0;
    if (has(usage, render::BufferUsage::Uniform))
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (has(usage, render::BufferUsage::Storage))
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (has(usage, render::BufferUsage::TransferSrc))
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (has(usage, render::BufferUsage::TransferDst))
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return flags;
}
// 视图资源对象记录的是实际 VkImageLayout；导入时换算成帧图的声明层枚举。
inline render::Layout graphLayout(VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return render::Layout::ColorAttachment;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return render::Layout::DepthAttachment;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return render::Layout::ShaderRead;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return render::Layout::TransferSrc;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return render::Layout::TransferDst;
    default:
        return render::Layout::Undefined;
    }
}
inline render::ImageDesc imageDesc(render::Format format, uint32_t width, uint32_t height,
                                   render::ImageUsage usage, render::Aspect aspect = render::Aspect::Color)
{
    render::ImageDesc desc;
    desc.format = format;
    desc.width = width;
    desc.height = height;
    desc.aspect = aspect;
    desc.usage = usage;
    return desc;
}
// 视图资源的声明用途：G-buffer 与显示类都要能被采样、作为附件写入和拷贝出去。
constexpr render::ImageUsage colorUsage = render::ImageUsage::Sampled | render::ImageUsage::ColorAttachment |
                                          render::ImageUsage::TransferSrc;
constexpr render::ImageUsage depthUsage = render::ImageUsage::Sampled | render::ImageUsage::DepthAttachment |
                                          render::ImageUsage::TransferSrc;
// 历史槽需要被采样、被解析结果写入，并在失效时清除。
constexpr render::ImageUsage historyUsage = colorUsage | render::ImageUsage::TransferDst;
}
