#include "Pipeline.hpp"
#include "RenderScene.hpp"
#include "FrameData.hpp"
#include "graph/FrameGraph.hpp"
#include "passes/Passes.hpp"
#include "../assets/AssetCache.hpp"
#include "sim/VulkanInterop.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>
#include <stdexcept>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace sim
{
using namespace vk;
namespace
{
VkFormat vkFormatOf(render::Format format)
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
VkImageUsageFlags vkUsageOf(render::ImageUsage usage)
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
VkBufferUsageFlags vkUsageOf(render::BufferUsage usage)
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
render::Layout graphLayout(VkImageLayout layout)
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
render::ImageDesc imageDesc(render::Format format, uint32_t width, uint32_t height, render::ImageUsage usage,
                            render::Aspect aspect = render::Aspect::Color)
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
uint64_t imageKey(const render::ImageDesc& desc)
{
    uint64_t key = 1469598103934665603ull;
    auto mix = [&key](uint64_t value)
    {
        key ^= value;
        key *= 1099511628211ull;
    };
    mix(uint64_t(desc.format));
    mix(desc.width);
    mix(desc.height);
    mix(desc.mipLevels);
    mix(desc.layers);
    mix(uint64_t(desc.aspect));
    mix(uint64_t(desc.usage));
    return key;
}
uint64_t bufferKey(const render::BufferDesc& desc)
{
    return desc.size * 131 + uint64_t(desc.usage);
}
}
struct Renderer::Impl
{
    Device d;
    Lifetime life;
    std::unique_ptr<Pipelines> pipelines;
    std::filesystem::path shaderDirectory;
    AssetStats stats;
    std::shared_ptr<AssetCache> assets;
    // 已切换但仍有在途帧或视图引用的旧缓存代际，待最后一个引用消失后释放。
    std::vector<std::shared_ptr<AssetCache>> retired;
    std::unordered_map<uint64_t, std::weak_ptr<RenderScene>> scenes;
    uint64_t nextGeneration = 0, nextSceneVersion = 0;
    explicit Impl(const DeviceOptions& o)
    {
        d.init(o);
        shaderDirectory =
            o.shaderDirectory.empty() ? std::filesystem::path(RENDERER_SHADER_DIR) : o.shaderDirectory;
        pipelines = std::make_unique<Pipelines>(d, shaderDirectory);
        assets = std::make_shared<AssetCache>(d, *pipelines, stats, ++nextGeneration);
    }
    // 键相同且内容一致的快照复用同一份共享场景，因此同快照的多台相机只准备与上传一次。
    std::shared_ptr<RenderScene> prepare(const SceneSnapshot& scene)
    {
        collect();
        auto key = sceneKey(scene, assets->generation);
        if (auto it = scenes.find(key); it != scenes.end())
            if (auto live = it->second.lock(); live && live->matches(scene))
            {
                ++stats.sceneReuses;
                return live;
            }
        auto prepared = std::make_shared<RenderScene>(d, assets, scene, ++nextSceneVersion);
        scenes[key] = prepared;
        ++stats.scenePreparations;
        std::erase_if(scenes, [](const auto& entry) { return entry.second.expired(); });
        return prepared;
    }
    void collect()
    {
        // 当前代际也要回收：持续加载新资产时，已完成的批次不能留到切换代际才释放。
        assets->collect();
        for (auto& cache : retired)
            cache->collect();
        std::erase_if(retired, [](const std::shared_ptr<AssetCache>& cache) { return cache.use_count() == 1; });
    }
    // 只切换代际；旧代际的缓冲可能仍被在途提交读取，因此不能立即销毁。
    void retireAssets()
    {
        retired.push_back(assets);
        assets = std::make_shared<AssetCache>(d, *pipelines, stats, ++nextGeneration);
        scenes.clear();
        collect();
    }
    AssetStats statsOf() const
    {
        auto snapshot = stats;
        snapshot.retiredGenerations = uint32_t(retired.size());
        auto count = [&snapshot](const std::shared_ptr<AssetCache>& cache)
        {
            snapshot.pendingUploads += cache->pendingUploads();
            snapshot.stagingBytes += cache->stagingBytes();
        };
        count(assets);
        for (auto& cache : retired)
            count(cache);
        return snapshot;
    }
};
struct FrameResult::Impl
{
    std::shared_ptr<Renderer::Impl> owner;
    // 结果持有产生它的共享场景，保证 GPU 完成前资产与实例数据存活。
    std::shared_ptr<RenderScene> scene;
    Lifetime life;
    std::vector<OutputInfo> infos;
    std::vector<Img> images;
    std::vector<Buf> readbacks;
    VkCommandBuffer command{};
    VkFence fence{};
    double timestamp = 0;
    bool submitted = false;
    explicit Impl(std::shared_ptr<Renderer::Impl> renderer) : owner(std::move(renderer))
    {
        auto& d = owner->d;
        command = d.command();
        life.release.push_back([this] { vkFreeCommandBuffers(owner->d.device, owner->d.pool, 1, &command); });
        VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(d.device, &ci, nullptr, &fence), "result fence");
        life.release.push_back([this] { vkDestroyFence(owner->d.device, fence, nullptr); });
    }
    ~Impl()
    {
        if (submitted)
            vkWaitForFences(owner->d.device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    size_t index(Channel channel) const
    {
        for (size_t i = 0; i < infos.size(); ++i)
            if (infos[i].requested.channel == channel)
                return i;
        throw std::invalid_argument("Channel was not requested");
    }
};
FrameResult::FrameResult(std::shared_ptr<Impl> p) : impl(std::move(p)) {}
FrameResult::~FrameResult() = default;
bool FrameResult::ready() const
{
    auto result = vkGetFenceStatus(impl->owner->d.device, impl->fence);
    if (result == VK_NOT_READY)
        return false;
    check(result, "query result fence");
    return true;
}
void FrameResult::wait() const
{
    check(vkWaitForFences(impl->owner->d.device, 1, &impl->fence, VK_TRUE, UINT64_MAX), "wait for result");
}
double FrameResult::time() const
{
    return impl->timestamp;
}
const std::vector<OutputInfo>& FrameResult::outputs() const
{
    return impl->infos;
}
OutputData FrameResult::read(Channel channel) const
{
    auto i = impl->index(channel);
    if (!impl->readbacks[i])
        throw std::logic_error("CPU delivery was not requested");
    wait();
    OutputData result{impl->infos[i], {}};
    const auto& buffer = *impl->readbacks[i];
    result.bytes.resize(size_t(buffer.size));
    std::memcpy(result.bytes.data(), buffer.mapped, result.bytes.size());
    return result;
}
VulkanOutput VulkanInterop::output(const FrameResult& result, Channel channel)
{
    const auto& state = *result.impl;
    auto i = state.index(channel);
    if (!state.images[i])
        throw std::logic_error("GPU delivery was not requested");
    const auto& image = *state.images[i];
    return {image.handle, image.view, image.format, image.layout, state.fence};
}
struct View::Impl
{
    std::shared_ptr<Renderer::Impl> owner;
    Device& d;
    Pipelines& pipe;
    Lifetime life;
    uint32_t w, h, tile;
    Img g[5], depth, shadow, hdr, history[2], display;
    std::vector<std::weak_ptr<FrameResult>> results;
    std::weak_ptr<FrameResult> pendingExport;
    // 视图只保留相机相关状态：当前共享场景、上一帧变换与历史。
    std::shared_ptr<RenderScene> scene;
    Buf globals, viewData;
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkDescriptorSet frame{}, lighting{}, resolve[2]{}, colorSet[2]{};
    unsigned number = 0, visibleCount = 0;
    // 历史槽的奇偶只随真正写入历史的帧前进：不参与 TAA 的请求不能让历史失效或换槽。
    unsigned historyNumber = 0;
    double time = 0;
    bool hasReadback = false, lastTaa = false;
    int lastChannel = 0;
    glm::mat4 lastVP{1};
    unsigned stillSamples = 0;
    DirectionalLight lastSun;
    std::vector<PointLight> lastLights;
    bool lastShadows = false;
    std::unordered_map<uint32_t, MeshHistory> lastModels;
    // 帧图与瞬时资源：声明由帧图产生，分配与释放留在执行侧。
    render::FrameGraph graph;
    std::vector<std::pair<uint64_t, Img>> transientImages;
    std::vector<std::pair<uint64_t, Buf>> transientBuffers;
    std::vector<bool> liveImages, liveBuffers;
    Buffer* staging = nullptr;
    std::vector<std::string> activePasses;
    Impl(std::shared_ptr<Renderer::Impl> r, uint32_t width, uint32_t height, uint32_t shadowTile)
        : owner(std::move(r)), d(owner->d), pipe(*owner->pipelines), w(width), h(height), tile(shadowTile)
    {
        auto max = d.properties.limits.maxImageDimension2D;
        if (!w || !h || !tile || w > max || h > max || tile > max / 2)
            throw std::invalid_argument("Invalid view dimensions");
        // 分配用的 VkImageUsageFlags 由帧图声明的同一组常量推导，避免声明与分配各写一份而漂移。
        const auto color = vkUsageOf(colorUsage);
        const auto z = vkUsageOf(depthUsage);
        for (unsigned i = 0; i < 5; ++i)
            g[i] = d.image(w, h, gFormats[i], color);
        depth = d.image(w, h, depthFormat, z);
        globals = d.buffer(sizeof(Globals), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        auto pool = descriptorPool(d, life, 1);
        frame = allocateSet(d, pool, pipe.frame);
        bindBuffer(d, frame, 0, *globals, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        cmd = d.command();
        life.release.push_back([this] { vkFreeCommandBuffers(d.device, d.pool, 1, &cmd); });
        VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(d.device, &ci, nullptr, &fence), "frame fence");
        life.release.push_back([this] { vkDestroyFence(d.device, fence, nullptr); });
    }
    ~Impl()
    {
        if (auto pending = pendingExport.lock())
            pending->wait();
        if (number)
            vkWaitForFences(d.device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    // 共享场景的缓冲与视图历史只在等待本视图上一帧后重新绑定。
    void useScene(const std::shared_ptr<RenderScene>& next)
    {
        if (scene == next)
            return;
        scene = next;
        auto bytes = std::max<size_t>(1, scene->items().size()) * sizeof(ViewInstance);
        if (!viewData || viewData->size < bytes)
            viewData = d.buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        bindBuffer(d, frame, 1, scene->instances(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        bindBuffer(d, frame, 2, *viewData, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
    void ensureColor()
    {
        if (display)
            return;
        // 与导入声明共用同一组 usage 常量：历史槽额外带 TRANSFER_DST，供失效时的清除使用。
        shadow = d.image(tile * 2, tile * 2, depthFormat, vkUsageOf(depthUsage));
        hdr = d.image(w, h, hdrFormat, vkUsageOf(colorUsage));
        display = d.image(w, h, colorFormat, vkUsageOf(colorUsage));
        for (auto& img : history)
            img = d.image(w, h, hdrFormat, vkUsageOf(historyUsage));
        auto pool = descriptorPool(d, life, 8);
        lighting = allocateSet(d, pool, pipe.lighting);
        for (unsigned i = 0; i < 5; ++i)
            bindImage(d, lighting, i, *g[i], pipe.point);
        bindImage(d, lighting, 5, *depth, pipe.point);
        bindImage(d, lighting, 6, *shadow, pipe.point);
        for (unsigned i = 0; i < 2; ++i)
        {
            resolve[i] = allocateSet(d, pool, pipe.resolve);
            bindImage(d, resolve[i], 0, *hdr, pipe.point);
            bindImage(d, resolve[i], 1, *g[2], pipe.point);
            bindImage(d, resolve[i], 2, *g[3], pipe.point);
            bindImage(d, resolve[i], 3, *history[1 - i], pipe.linear);
            // 颜色变换读取本帧写入的历史槽，因此每个槽各有一份描述符集合。
            colorSet[i] = allocateSet(d, pool, pipe.colorSet);
            bindImage(d, colorSet[i], 0, *history[i], pipe.point);
        }
    }
    // 瞬时资源按声明缓存：命中复用，帧首释放上一帧未使用的条目。
    Img& transientImage(const render::ImageDesc& desc)
    {
        auto key = imageKey(desc);
        for (size_t i = 0; i < transientImages.size(); ++i)
            if (transientImages[i].first == key)
            {
                liveImages[i] = true;
                return transientImages[i].second;
            }
        transientImages.emplace_back(key, d.image(desc.width, desc.height, vkFormatOf(desc.format),
                                                  vkUsageOf(desc.usage), std::max(1u, desc.mipLevels)));
        liveImages.push_back(true);
        return transientImages.back().second;
    }
    Buf& transientBuffer(const render::BufferDesc& desc)
    {
        auto key = bufferKey(desc);
        for (size_t i = 0; i < transientBuffers.size(); ++i)
            if (transientBuffers[i].first == key)
            {
                liveBuffers[i] = true;
                return transientBuffers[i].second;
            }
        transientBuffers.emplace_back(key, d.buffer(desc.size, vkUsageOf(desc.usage), true));
        liveBuffers.push_back(true);
        return transientBuffers.back().second;
    }
    void beginTransients()
    {
        std::fill(liveImages.begin(), liveImages.end(), false);
        std::fill(liveBuffers.begin(), liveBuffers.end(), false);
    }
    // 上一帧已完成（等待发生在本帧录制前），此时才可以销毁未被引用的瞬时资源。
    void sweepTransients()
    {
        for (size_t i = transientImages.size(); i-- > 0;)
            if (!liveImages[i])
            {
                transientImages.erase(transientImages.begin() + ptrdiff_t(i));
                liveImages.erase(liveImages.begin() + ptrdiff_t(i));
            }
        for (size_t i = transientBuffers.size(); i-- > 0;)
            if (!liveBuffers[i])
            {
                transientBuffers.erase(transientBuffers.begin() + ptrdiff_t(i));
                liveBuffers.erase(liveBuffers.begin() + ptrdiff_t(i));
            }
        staging = nullptr;
    }
    Image* source(Channel channel) const
    {
        switch (channel)
        {
        case Channel::Color:
            return display.get();
        case Channel::Hdr:
            return hdr.get();
        case Channel::HardwareDepth:
            return depth.get();
        default:
            return g[size_t(channel) - 1].get();
        }
    }
};
Renderer::Renderer(const DeviceOptions& o) : impl(std::make_shared<Impl>(o)) {}
Renderer::~Renderer() = default;
View::View(std::unique_ptr<Impl> p) : impl(std::move(p)) {}
View::~View() = default;
void Renderer::clearAssetCache()
{
    impl->retireAssets();
}
AssetStats Renderer::assetStats() const
{
    return impl->statsOf();
}
const DeviceHandles& Renderer::handles() const
{
    return impl->d;
}
VkSampler Renderer::displaySampler() const
{
    return impl->pipelines->linear;
}
std::unique_ptr<View> Renderer::createView(uint32_t w, uint32_t h, uint32_t tile)
{
    return std::unique_ptr<View>(new View(std::make_unique<View::Impl>(impl, w, h, tile)));
}
Submission Renderer::submit(const RenderRequest& request)
{
    auto& v = *request.view.impl;
    if (v.owner != impl)
        throw std::invalid_argument("View belongs to a different renderer");
    if (request.outputs.empty())
        throw std::invalid_argument("Request must contain at least one output");
    std::array<bool, 8> seen{};
    bool color = false;
    std::vector<Channel> channels;
    for (auto output : request.outputs)
    {
        auto i = size_t(output.channel);
        if (i >= seen.size() || seen[i])
            throw std::invalid_argument("Invalid or duplicate output channel");
        if (output.delivery != Delivery::Cpu && output.delivery != Delivery::Gpu &&
            output.delivery != Delivery::CpuAndGpu)
            throw std::invalid_argument("Invalid output delivery");
        seen[i] = true;
        channels.push_back(output.channel);
        color = color || output.channel == Channel::Color || output.channel == Channel::Hdr;
    }
    std::erase_if(v.results, [](const auto& result) { return result.expired(); });
    if (v.results.size() >= 3)
        return {};
    // 同内容快照在此复用同一份共享场景，两台相机不会重复准备或上传。
    auto prepared = impl->prepare(request.scene);

    auto state = std::make_shared<FrameResult::Impl>(impl);
    state->timestamp = request.scene.time;
    std::vector<Image*> sources;
    if (color)
        v.ensureColor();
    for (auto output : request.outputs)
    {
        auto source = v.source(output.channel);
        if (!source)
            throw std::logic_error("Requested channel has no image");
        PixelFormat format = PixelFormat::Rgba8Unorm;
        unsigned pixelBytes = 4;
        if (source->format == hdrFormat)
        {
            format = PixelFormat::Rgba16Float;
            pixelBytes = 8;
        }
        else if (source->format == VK_FORMAT_R32_SFLOAT || source->format == depthFormat)
            format = PixelFormat::R32Float;
        else if (source->format == VK_FORMAT_R32_UINT)
            format = PixelFormat::R32Uint;
        state->infos.push_back({output, format, v.w, v.h, v.w * pixelBytes});
        sources.push_back(source);
        state->images.push_back(output.delivery == Delivery::Cpu
                                    ? nullptr
                                    : v.d.image(v.w, v.h, source->format,
                                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
        state->readbacks.push_back(
            output.delivery == Delivery::Gpu
                ? nullptr
                : v.d.buffer(VkDeviceSize(v.w) * v.h * pixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
    }
    auto options = request.options;
    options.readback = false;
    options.stableGeometry = true;
    if (!color)
    {
        options.taa = false;
        options.shadows = false;
    }
    renderFrame(prepared, request.scene, request.camera, request.view, options, color, channels);

    auto cmd = state->command;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(cmd, &bi), "begin output export");
    for (size_t i = 0; i < sources.size(); ++i)
    {
        auto& source = *sources[i];
        source.transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageSubresourceLayers subresource{source.aspect, 0, 0, 1};
        if (auto& image = state->images[i])
        {
            image->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy copy{subresource, {}, subresource, {}, {v.w, v.h, 1}};
            vkCmdCopyImage(cmd, source.handle, source.layout, image->handle, image->layout, 1, &copy);
            image->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        if (auto& buffer = state->readbacks[i])
        {
            VkBufferImageCopy copy{};
            copy.imageSubresource = subresource;
            copy.imageExtent = {v.w, v.h, 1};
            vkCmdCopyImageToBuffer(cmd, source.handle, source.layout, buffer->handle, 1, &copy);
        }
        source.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    check(vkEndCommandBuffer(cmd), "end output export");
    auto result = std::shared_ptr<FrameResult>(new FrameResult(state));
    // 结果与视图共同持有共享场景，GPU 完成前资产与实例数据不会被回收。
    state->scene = prepared;
    v.results.push_back(result);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &state->command;
    check(vkQueueSubmit(v.d.queue, 1, &si, state->fence), "submit output export");
    state->submitted = true;
    v.pendingExport = result;
    return {SubmitStatus::Submitted, std::move(result)};
}
bool View::ready() const
{
    if (auto pending = impl->pendingExport.lock(); pending && !pending->ready())
        return false;
    auto result = vkGetFenceStatus(impl->d.device, impl->fence);
    if (result == VK_NOT_READY)
        return false;
    check(result, "query frame fence");
    return true;
}
void View::wait() const
{
    check(vkWaitForFences(impl->d.device, 1, &impl->fence, VK_TRUE, UINT64_MAX), "wait for view");
    if (auto pending = impl->pendingExport.lock())
        pending->wait();
}
VkImageView View::colorView() const
{
    impl->ensureColor();
    return impl->display->view;
}
VkImageView View::imageView(Channel channel) const
{
    auto& v = *impl;
    switch (channel)
    {
    case Channel::Color:
        v.ensureColor();
        return v.display->view;
    case Channel::HardwareDepth:
        return v.depth->view;
    case Channel::Hdr:
        v.ensureColor();
        return v.hdr->view;
    default:
    {
        auto i = int(channel) - int(Channel::AlbedoMetal);
        if (i < 0 || i >= 5)
            throw std::invalid_argument("Unknown channel");
        return v.g[i]->view;
    }
    }
}
const std::vector<std::string>& View::activePasses() const
{
    return impl->activePasses;
}
uint32_t View::visibleMeshes() const
{
    return impl->visibleCount;
}
Frame View::read() const
{
    auto& v = *impl;
    if (!v.hasReadback || !v.staging)
        throw std::logic_error("Enable readback for this submission");
    wait();
    size_t n = size_t(v.w) * v.h;
    Frame out{v.w, v.h, v.time};
    out.rgba.resize(n * 4);
    out.depth.resize(n);
    out.ids.resize(n);
    auto data = static_cast<const uint8_t*>(v.staging->mapped);
    std::memcpy(out.rgba.data(), data, n * 4);
    std::memcpy(out.depth.data(), data + 4 * n, n * 4);
    std::memcpy(out.ids.data(), data + 8 * n, n * 4);
    return out;
}
void Renderer::render(const SceneSnapshot& scene, const PerspectiveCamera& camera, View& view,
                      const RenderOptions& o)
{
    if (view.impl->owner != impl)
        throw std::invalid_argument("View belongs to a different renderer");
    std::vector<Channel> channels{Channel::Color};
    if (o.readback)
    {
        channels.push_back(Channel::MetricDepth);
        channels.push_back(Channel::InstanceId);
    }
    renderFrame(impl->prepare(scene), scene, camera, view, o, true, channels);
}
void Renderer::renderFrame(const std::shared_ptr<RenderScene>& scene, const SceneSnapshot& snapshot,
                           const PerspectiveCamera& camera, View& view, const RenderOptions& o, bool color,
                           const std::vector<Channel>& exports)
{
    auto& v = *view.impl;
    if (v.owner != impl)
        throw std::invalid_argument("View belongs to a different renderer");
    if (snapshot.lights.size() > 16 || glm::length(snapshot.sun.direction) < 1e-6f)
        throw std::invalid_argument("Invalid lights");
    if (scene->items().size() != snapshot.meshes.size())
        throw std::invalid_argument("Scene does not match snapshot");
    view.wait();
    if (color)
        v.ensureColor();
    // 本视图上一帧已完成，此时才能替换共享平台缓冲与描述符绑定，也能安全回收瞬时资源。
    v.useScene(scene);
    // 上一帧已等待完成，可以先回收未被引用的瞬时资源，再开始本帧的声明。
    v.sweepTransients();
    v.beginTransients();
    auto& items = scene->items();

    Globals f{};
    f.vp = camera.projection(v.w, v.h) * camera.view();
    f.oldVP = v.lastVP;
    f.invVP = glm::inverse(f.vp);
    f.eye = {camera.position, 1};
    f.sunDirection = {glm::normalize(snapshot.sun.direction), 0};
    f.sunColor = {snapshot.sun.radiance, 1};
    bool accumulate =
        o.taa && v.lastTaa && !o.resetHistory && v.lastChannel == o.debugChannel && snapshot.time >= v.time;
    accumulate = accumulate && snapshot.sun.direction == v.lastSun.direction &&
                 snapshot.sun.radiance == v.lastSun.radiance && sameLights(snapshot.lights, v.lastLights) &&
                 o.shadows == v.lastShadows;
    bool stationary = accumulate && f.vp == v.lastVP && items.size() == v.lastModels.size();
    auto index = (v.number % temporalSamples) + 1;
    glm::vec2 jitter =
        o.taa ? glm::vec2((2 * halton(index, 2) - 1) / v.w, (2 * halton(index, 3) - 1) / v.h) : glm::vec2(0);
    f.jitter = {jitter, 0, 0};
    f.settings = {o.exposure, float(accumulate), float(o.shadows), float(o.debugChannel)};
    f.info = {float(snapshot.lights.size()), 1.f / v.w, 1.f / v.h, 0};
    for (size_t i = 0; i < snapshot.lights.size(); ++i)
    {
        f.pointPosition[i] = {snapshot.lights[i].position, 1};
        f.pointColor[i] = {snapshot.lights[i].intensity, 1};
    }

    auto rasterVP = f.vp;
    for (unsigned j = 0; j < 4; ++j)
    {
        rasterVP[j].x += jitter.x * rasterVP[j].w;
        rasterVP[j].y += jitter.y * rasterVP[j].w;
    }
    std::vector<uint32_t> visibleList;
    std::unordered_map<uint32_t, MeshHistory> models;
    for (uint32_t i = 0; i < items.size(); ++i)
    {
        auto& item = items[i];
        auto prev = v.lastModels.find(item.id);
        MeshHistory state{item.world, item.geometry, item.material, item.castsShadow, i};
        if (!models.emplace(item.id, state).second)
            throw std::invalid_argument("Duplicate snapshot mesh ID");
        // 资产身份或材质值变化即视为新对象：既不能复用旧姿态，也不能继续冻结历史。
        bool known = prev != v.lastModels.end() && prev->second.geometry == item.geometry &&
                     prev->second.material == item.material;
        stationary = stationary && known && prev->second == state;
        // 视图只写上一帧变换与匹配标记；当前变换来自共享场景的静态实例数据。
        ViewInstance history{known ? prev->second.world : item.world, {0, uint32_t(known), 0, 0}};
        std::memcpy(static_cast<char*>(v.viewData->mapped) + i * sizeof(history), &history, sizeof(history));
        if (!o.culling || visible(item.world, item.minimum, item.maximum, f.vp) ||
            (o.taa && visible(item.world, item.minimum, item.maximum, rasterVP)))
            visibleList.push_back(i);
    }
    if (color && o.shadows)
        fitShadows(f, camera, items, v.tile);
    // 静止时逐像素平均一个完整抖动周期，保留前景与背景各自的覆盖贡献。
    f.info.w = stationary ? (v.stillSamples < temporalSamples ? 1.f / (v.stillSamples + 1) : 0.f) : -1.f;
    std::memcpy(v.globals->mapped, &f, sizeof(f));
    unsigned slot = v.historyNumber % 2;

    // 帧图：导入持久资源、声明 pass、导出请求的输出。分配与录制由帧图执行阶段驱动。
    using render::Access;
    using render::Layout;
    auto& graph = v.graph;
    graph.reset();
    const auto albedo = graph.importImage("albedo", imageDesc(render::Format::Rgba8Unorm, v.w, v.h, colorUsage),
                                          graphLayout(v.g[0]->layout), false);
    const auto normalRough = graph.importImage("normal-rough",
                                               imageDesc(render::Format::Rgba16Float, v.w, v.h, colorUsage),
                                               graphLayout(v.g[1]->layout), false);
    const auto motion =
        graph.importImage("motion", imageDesc(render::Format::Rgba16Float, v.w, v.h, colorUsage), graphLayout(v.g[2]->layout),
                          false);
    const auto metric =
        graph.importImage("metric-depth", imageDesc(render::Format::R32Float, v.w, v.h, colorUsage), graphLayout(v.g[3]->layout),
                          false);
    const auto instanceId = graph.importImage("instance-id",
                                              imageDesc(render::Format::R32Uint, v.w, v.h, colorUsage), graphLayout(v.g[4]->layout),
                                              false);
    const auto depthImage = graph.importImage(
        "hardware-depth", imageDesc(render::Format::D32Float, v.w, v.h, depthUsage, render::Aspect::Depth),
        graphLayout(v.depth->layout), false);
    const auto globalsBuffer = graph.importBuffer("globals", {sizeof(Globals), render::BufferUsage::Uniform}, true);
    const auto viewBuffer = graph.importBuffer("view-history", {v.viewData->size, render::BufferUsage::Storage}, true);
    graph.bind(albedo, *v.g[0]);
    graph.bind(normalRough, *v.g[1]);
    graph.bind(motion, *v.g[2]);
    graph.bind(metric, *v.g[3]);
    graph.bind(instanceId, *v.g[4]);
    graph.bind(depthImage, *v.depth);
    graph.bind(globalsBuffer, *v.globals);
    graph.bind(viewBuffer, *v.viewData);
    // 颜色相关资源只在请求颜色时才导入：几何请求不分配也不需要它们。
    render::ImageHandle shadowMap, hdrImage, displayImage, historyImage[2];
    if (color)
    {
        shadowMap = graph.importImage(
            "shadow", imageDesc(render::Format::D32Float, v.tile * 2, v.tile * 2, depthUsage, render::Aspect::Depth),
            graphLayout(v.shadow->layout), true);
        hdrImage = graph.importImage("hdr", imageDesc(render::Format::Rgba16Float, v.w, v.h, colorUsage),
                                     graphLayout(v.hdr->layout), false);
        displayImage = graph.importImage("display", imageDesc(render::Format::Rgba8Unorm, v.w, v.h, colorUsage),
                                         graphLayout(v.display->layout), false);
        for (int i = 0; i < 2; ++i)
            historyImage[i] = graph.importImage("history-" + std::to_string(i),
                                                imageDesc(render::Format::Rgba16Float, v.w, v.h, historyUsage),
                                                graphLayout(v.history[i]->layout), i == 1 - slot ? accumulate : false);
        graph.bind(shadowMap, *v.shadow);
        graph.bind(hdrImage, *v.hdr);
        graph.bind(displayImage, *v.display);
        graph.bind(historyImage[0], *v.history[0]);
        graph.bind(historyImage[1], *v.history[1]);
    }
    const auto stagingBuffer = graph.createBuffer(
        "readback", {VkDeviceSize(v.w) * v.h * 12, render::BufferUsage::TransferDst | render::BufferUsage::HostRead});

    std::array<Image*, 5> targets{v.g[0].get(), v.g[1].get(), v.g[2].get(), v.g[3].get(), v.g[4].get()};
    auto declareTargets = [&](render::FrameGraph::Declaration& d)
    {
        d.write(albedo, Access::ColorWrite);
        d.write(normalRough, Access::ColorWrite);
        d.write(motion, Access::ColorWrite);
        d.write(metric, Access::ColorWrite);
        d.write(instanceId, Access::ColorWrite);
        d.write(depthImage, Access::DepthWrite);
        d.read(globalsBuffer, Access::UniformRead);
        d.read(viewBuffer, Access::StorageRead);
    };
    auto historyClearPass = graph.reserve("history.clear");
    auto shadowPass = graph.reserve("shadow");
    auto geometryPass = graph.reserve("geometry");
    auto stablePass = graph.reserve("geometry.stable");
    auto lightingPass = graph.reserve("lighting");
    auto temporalPass = graph.reserve("temporal");
    auto colorPass = graph.reserve("color");
    auto readbackPass = graph.reserve("readback");
    bool stable = color && o.taa && (o.readback || o.stableGeometry);
    // 历史失效时先清除再采样：既保证定义内容，也让验证层看到合法布局。
    if (color && !accumulate)
        graph.addPass(
            historyClearPass,
            [&](render::FrameGraph::Declaration& d)
            { d.write(historyImage[1 - slot], Access::CopyDestination); },
            [&](VkCommandBuffer cmd) { recordHistoryClear(cmd, *v.history[1 - slot]); });
    if (color && o.shadows)
        graph.addPass(
            shadowPass, [&](render::FrameGraph::Declaration& d) { d.write(shadowMap, Access::DepthWrite); },
            [&](VkCommandBuffer cmd) { recordShadow(cmd, v.pipe, v.frame, *v.shadow, items, f, v.tile); });
    graph.addPass(
        geometryPass, declareTargets,
        [&](VkCommandBuffer cmd)
        { recordGeometry(cmd, v.pipe, v.frame, items, visibleList, targets, *v.depth, false); });
    if (color)
        graph.addPass(
            lightingPass,
            [&](render::FrameGraph::Declaration& d)
            {
                d.read(albedo);
                d.read(normalRough);
                d.read(motion);
                d.read(metric);
                d.read(instanceId);
                d.read(depthImage);
                d.read(shadowMap);
                d.read(globalsBuffer, Access::UniformRead);
                d.write(hdrImage, Access::ColorWrite);
            },
            [&](VkCommandBuffer cmd) { recordLighting(cmd, v.pipe, v.frame, v.lighting, *v.hdr); });
    if (color)
        graph.addPass(
            temporalPass,
            [&](render::FrameGraph::Declaration& d)
            {
                d.read(hdrImage);
                d.read(motion);
                d.read(metric);
                d.read(historyImage[1 - slot]);
                d.read(globalsBuffer, Access::UniformRead);
                d.write(historyImage[slot], Access::ColorWrite);
                // 历史是跨帧副作用：即使本次不输出颜色，也要写回以保证下一帧可用。
                if (o.taa)
                    d.sideEffect();
            },
            [&](VkCommandBuffer cmd)
            { recordTemporal(cmd, v.pipe, v.frame, v.resolve[slot], *v.history[slot]); });
    if (color)
        graph.addPass(
            colorPass,
            [&](render::FrameGraph::Declaration& d)
            {
                d.read(historyImage[slot]);
                d.read(globalsBuffer, Access::UniformRead);
                d.write(displayImage, Access::ColorWrite);
            },
            [&](VkCommandBuffer cmd) { recordColor(cmd, v.pipe, v.frame, v.colorSet[slot], *v.display); });
    // 稳定几何排在时序解析与颜色变换之后：它覆盖 G-buffer 供真值读回，
    // 不能抢在 TAA 之前把抖动样本换成像素中心样本。
    if (stable)
        graph.addPass(
            stablePass, declareTargets,
            [&](VkCommandBuffer cmd)
            { recordGeometry(cmd, v.pipe, v.frame, items, visibleList, targets, *v.depth, true); });
    if (o.readback)
    {
        auto bytes = VkDeviceSize(v.w) * v.h * 4;
        graph.addPass(
            readbackPass,
            [&](render::FrameGraph::Declaration& d)
            {
                d.read(displayImage, Access::CopySource);
                d.read(metric, Access::CopySource);
                d.read(instanceId, Access::CopySource);
                d.write(stagingBuffer, Access::CopyDestination, {0, bytes * 3});
            },
            [&](VkCommandBuffer cmd)
            {
                std::array<Image*, 3> copies{v.display.get(), v.g[3].get(), v.g[4].get()};
                recordReadback(cmd, copies, *v.staging, v.w, v.h);
            });
    }
    // 导出只覆盖本次请求的通道；恢复声明让持久视图资源在帧末回到只读，供下一帧导入。
    auto handleOf = [&](Channel channel)
    {
        switch (channel)
        {
        case Channel::Color:
            return displayImage;
        case Channel::Hdr:
            return hdrImage;
        case Channel::HardwareDepth:
            return depthImage;
        case Channel::AlbedoMetal:
            return albedo;
        case Channel::NormalRough:
            return normalRough;
        case Channel::Motion:
            return motion;
        case Channel::MetricDepth:
            return metric;
        default:
            return instanceId;
        }
    };
    for (auto channel : exports)
        graph.exportImage(handleOf(channel), Layout::ShaderRead);
    if (o.readback)
        graph.exportBuffer(stagingBuffer);
    for (auto handle : {albedo, normalRough, motion, metric, instanceId, depthImage})
        graph.restoreImage(handle, Layout::ShaderRead);
    if (color)
    {
        for (auto handle : {hdrImage, displayImage, historyImage[0], historyImage[1]})
            graph.restoreImage(handle, Layout::ShaderRead);
    }
    graph.compile();
    // 编译结果决定实际需要的瞬时资源：只分配存活的声明。
    for (auto& entry : graph.transients())
    {
        if (entry.image)
        {
            graph.bind(render::ImageHandle{entry.resource}, *v.transientImage(entry.imageDesc));
        }
        else
        {
            auto& buffer = *v.transientBuffer(entry.bufferDesc);
            graph.bind(render::BufferHandle{entry.resource}, buffer);
            v.staging = &buffer;
        }
    }
    if (!o.readback)
        v.staging = nullptr;

    check(vkResetCommandBuffer(v.cmd, 0), "reset frame commands");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(v.cmd, &bi), "begin frame");
    graph.execute(v.cmd);
    check(vkEndCommandBuffer(v.cmd), "end frame");
    v.activePasses = graph.activePasses();
    // 首次出现的资产上传与本帧在同一队列上，用时间线值声明提交依赖。
    auto uploads = scene->uploads();
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    VkPipelineStageFlags uploadStage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &v.cmd;
    if (uploads)
    {
        timeline.waitSemaphoreValueCount = 1;
        timeline.pWaitSemaphoreValues = &uploads;
        si.pNext = &timeline;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &impl->d.uploadTimeline;
        si.pWaitDstStageMask = &uploadStage;
    }
    check(vkResetFences(v.d.device, 1, &v.fence), "reset frame fence");
    check(vkQueueSubmit(v.d.queue, 1, &si, v.fence), "submit frame");

    // 历史有效性状态只在真正写入历史的帧上推进：几何真值或 GPU 交付请求没有触碰历史，
    // 不能让下一帧误判历史已作废而重新积累。
    if (color)
    {
        v.lastVP = f.vp;
        v.stillSamples = stationary ? std::min(temporalSamples, v.stillSamples + 1) : unsigned(o.taa && !accumulate);
        v.lastSun = snapshot.sun;
        v.lastLights = snapshot.lights;
        v.lastShadows = o.shadows;
        v.lastModels = std::move(models);
        v.lastTaa = o.taa;
        v.lastChannel = o.debugChannel;
        v.time = snapshot.time;
        ++v.historyNumber;
    }
    v.hasReadback = o.readback;
    v.visibleCount = uint32_t(visibleList.size());
    ++v.number;
}
}
