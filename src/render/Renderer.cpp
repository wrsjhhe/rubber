#include "Pipeline.hpp"
#include "RenderScene.hpp"
#include "FrameData.hpp"
#include "graph/FrameGraph.hpp"
#include "passes/Passes.hpp"
#include "GraphBridge.hpp"
#include "Module.hpp"
#include "ShaderDirectory.hpp"
#include "../modules/mesh/MeshModule.hpp"
#include "../modules/points/PointsModule.hpp"
#include "../presentation/Presentation.hpp"
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
struct Renderer::Impl final : AssetProvider
{
    Device d;
    Lifetime life;
    std::unique_ptr<Pipelines> pipelines;
    std::filesystem::path shaderDirectory;
    AssetStats stats;
    std::shared_ptr<AssetCache> assets;
    // 已切换但仍有在途帧或视图引用的旧缓存代际，待最后一个引用消失后释放。
    std::vector<std::shared_ptr<AssetCache>> retired;
    // 模块按接口注册：主调度只遍历，不判断类型。
    std::vector<std::unique_ptr<RenderModule>> modules;
    uint64_t nextGeneration = 0;
    explicit Impl(const DeviceOptions& o)
    {
        d.init(o);
        shaderDirectory = vk::resolveShaderDirectory(o.shaderDirectory);
        pipelines = std::make_unique<Pipelines>(d, shaderDirectory);
        assets = std::make_shared<AssetCache>(d, *pipelines, stats, ++nextGeneration);
        modules.push_back(std::make_unique<MeshModule>(d, *pipelines, *this, stats));
        modules.push_back(std::make_unique<PointsModule>(d, *pipelines));
    }
    std::shared_ptr<AssetCache> currentAssets() override
    {
        return assets;
    }
    // 快照准备交给各模块：同一内容的多台相机仍只准备与上传一次。
    void prepare(const SceneSnapshot& scene, FrameLifetime& frame)
    {
        collect();
        for (auto& module : modules)
            module->prepare(scene, frame);
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
        for (auto& module : modules)
            module->invalidateAssets();
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
    // 结果持有产生它的帧共享数据，保证 GPU 完成前资产与实例缓冲存活。
    std::shared_ptr<vk::FrameLifetime> frame;
    Lifetime life;
    std::vector<OutputInfo> infos;
    std::vector<Img> images;
    std::vector<Buf> readbacks;
    VkCommandBuffer command{};
    VkFence fence{};
    double timestamp = 0;
    ShadowInfo shadows;
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
ShadowInfo FrameResult::shadowInfo() const
{
    return impl->shadows;
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
    Img g[5], depth, hdr, history[2], display, debug;
    std::vector<std::weak_ptr<FrameResult>> results;
    std::weak_ptr<FrameResult> pendingExport;
    // 视图只保留相机相关状态：帧级共享资源、历史与模块的视图侧状态。
    Buf globals;
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkDescriptorSet frame{}, resolve[2]{}, colorSet[2]{}, debugSet{};
    // 每个模块一份视图状态（阴影图、实例历史、点云描述符），由模块自己创建。
    std::vector<std::unique_ptr<vk::ModuleView>> modules;
    vk::FrameStats stats;
    vk::FrameLifetime work;
    ShadowInfo shadows;
    unsigned number = 0, visibleCount = 0, pointCount = 0;
    // 没有结果对象时，本帧的共享数据由视图保活到下一次等待完成。
    std::shared_ptr<vk::FrameLifetime> retained;
    // 历史槽的奇偶只随真正写入历史的帧前进：不参与 TAA 的请求不能让历史失效或换槽。
    unsigned historyNumber = 0;
    // 最近一帧真正写入的历史槽；-1 表示还没有可读的解析结果。
    int resolvedSlot = -1;
    double time = 0;
    bool hasReadback = false, lastTaa = false;
    // 上一帧是否被判为静止（相机与所有模块的输入都与上一帧相同）。静止判定只在颜色帧上
    // 有意义，几何真值请求不改变历史基准，也不会把这里置真。
    bool stationary = false;
    unsigned historySlot() const
    {
        return historyNumber % 2;
    }
    int lastChannel = 0;
    glm::mat4 lastVP{1};
    unsigned stillSamples = 0;
    DirectionalLight lastSun;
    std::vector<PointLight> lastLights;
    bool lastShadows = false;
    bool lastRayShadows = false;
    glm::vec2 lastShadowOptions{};
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
        // 模块视图只拿到设备、管线、帧描述符集合与尺寸，不接触视图的其余状态。
        vk::ModuleViewDesc desc;
        desc.device = &d;
        desc.pipelines = &pipe;
        desc.frameSet = frame;
        desc.width = w;
        desc.height = h;
        desc.shadowTile = tile;
        for (auto& module : owner->modules)
            modules.push_back(module->view(desc));
    }
    ~Impl()
    {
        if (auto pending = pendingExport.lock())
            pending->wait();
        if (number)
            vkWaitForFences(d.device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    void ensureColor()
    {
        if (display)
            return;
        // 与导入声明共用同一组 usage 常量：历史槽额外带 TRANSFER_DST，供失效时的清除使用。
        hdr = d.image(w, h, hdrFormat, vkUsageOf(colorUsage));
        display = d.image(w, h, colorFormat, vkUsageOf(colorUsage));
        for (auto& img : history)
            img = d.image(w, h, hdrFormat, vkUsageOf(historyUsage));
        auto pool = descriptorPool(d, life, 4);
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
    // 调试呈现图：与显示图同格式，但只有请求调试图时才分配。它与 Beauty 各自一份，
    // 因此同一帧既能看到真实颜色，也能看到调试通道。
    void ensureDebug()
    {
        ensureColor();
        if (debug)
            return;
        debug = d.image(w, h, colorFormat, vkUsageOf(colorUsage));
        auto pool = descriptorPool(d, life, 1);
        debugSet = allocateSet(d, pool, pipe.presentationSet);
        // 与 presentation.frag 的绑定顺序一致：法线、米制深度、实例标签、HDR。
        bindImage(d, debugSet, 0, *g[1], pipe.point);
        bindImage(d, debugSet, 1, *g[3], pipe.point);
        bindImage(d, debugSet, 2, *g[4], pipe.point);
        bindImage(d, debugSet, 3, *hdr, pipe.point);
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
        case Channel::Resolved:
            return resolvedSlot < 0 ? nullptr : history[unsigned(resolvedSlot)].get();
        case Channel::Debug:
            return debug.get();
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
bool Renderer::supportsRayQueryContactShadows() const
{
    return impl->d.rayQuery;
}
const DeviceHandles& Renderer::handles() const
{
    return impl->d;
}
VkSampler Renderer::displaySampler() const
{
    return impl->pipelines->linear;
}
std::filesystem::path Renderer::shaderDirectory() const
{
    return impl->shaderDirectory;
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
    if (request.options.debugChannel < 0 || request.options.debugChannel > 4)
        throw std::invalid_argument("Invalid debug channel");
    std::array<bool, 10> seen{};
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
        // 调试图显示哪个通道由 RenderOptions 决定：没有选择就没有图可交付。
        if (output.channel == Channel::Debug && request.options.debugChannel == 0)
            throw std::invalid_argument("Debug output requires a debug channel");
        seen[i] = true;
        channels.push_back(output.channel);
        // 调试图属于颜色链路的呈现副本：它要读 G-buffer，级联视图还要读 HDR。
        // 解析输出同样需要颜色链路：它就是本帧时序解析写回的历史槽。
        color = color || output.channel == Channel::Color || output.channel == Channel::Hdr ||
                output.channel == Channel::Resolved || output.channel == Channel::Debug;
    }
    std::erase_if(v.results, [](const auto& result) { return result.expired(); });
    if (v.results.size() >= 3)
        return {};
    // 同内容快照在此复用同一份共享数据，两台相机不会重复准备或上传。
    auto frame = std::make_shared<vk::FrameLifetime>();
    impl->prepare(request.scene, *frame);

    auto state = std::make_shared<FrameResult::Impl>(impl);
    state->timestamp = request.scene.time;
    std::vector<Image*> sources;
    if (color)
        v.ensureColor();
    if (request.options.debugChannel != 0)
        v.ensureDebug();
    // Resolved 的源是本帧将要写入的历史槽：请求它本身就让时序解析成为裁剪根，
    // 因此这里的槽号与 renderFrame 将要写入的一致。
    const unsigned resolvedSlot = v.historySlot();
    auto sourceOf = [&](Channel channel) -> Image*
    { return channel == Channel::Resolved ? v.history[resolvedSlot].get() : v.source(channel); };
    for (auto output : request.outputs)
    {
        auto source = sourceOf(output.channel);
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
    renderFrame(frame, request.scene, request.camera, request.view, options, color, channels);
    state->shadows = v.shadows;

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
    // 结果与视图共同持有本帧的共享数据，GPU 完成前资产与实例缓冲不会被回收。
    state->frame = frame;
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
VkImageView View::debugView() const
{
    impl->ensureDebug();
    return impl->debug->view;
}
uint32_t View::width() const
{
    return impl->w;
}
uint32_t View::height() const
{
    return impl->h;
}
VkImageView View::imageView(Channel channel) const
{
    auto& v = *impl;
    switch (channel)
    {
    case Channel::Color:
        v.ensureColor();
        return v.display->view;
    case Channel::Debug:
        v.ensureDebug();
        return v.debug->view;
    case Channel::HardwareDepth:
        return v.depth->view;
    case Channel::Hdr:
        v.ensureColor();
        return v.hdr->view;
    case Channel::Resolved:
        if (v.resolvedSlot < 0)
            throw std::logic_error("Resolved has not been produced by this view");
        return v.history[unsigned(v.resolvedSlot)]->view;
    default:
    {
        auto i = int(channel) - int(Channel::AlbedoMetal);
        if (i < 0 || i >= 5)
            throw std::invalid_argument("Unknown channel");
        return v.g[i]->view;
    }
    }
}
ShadowInfo View::shadowInfo() const
{
    return impl->shadows;
}
const std::vector<std::string>& View::activePasses() const
{
    return impl->activePasses;
}
uint32_t View::visibleMeshes() const
{
    return impl->visibleCount;
}
uint32_t View::visiblePoints() const
{
    return impl->pointCount;
}
bool View::stationary() const
{
    return impl->stationary;
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
    auto frame = std::make_shared<vk::FrameLifetime>();
    impl->prepare(scene, *frame);
    renderFrame(frame, scene, camera, view, o, true, channels);
}
void Renderer::renderFrame(const std::shared_ptr<vk::FrameLifetime>& frame, const SceneSnapshot& snapshot,
                           const PerspectiveCamera& camera, View& view, const RenderOptions& o, bool color,
                           const std::vector<Channel>& exports)
{
    auto& v = *view.impl;
    if (v.owner != impl)
        throw std::invalid_argument("View belongs to a different renderer");
    if (snapshot.lights.size() > 16 || glm::length(snapshot.sun.direction) < 1e-6f)
        throw std::invalid_argument("Invalid lights");
    if (o.debugChannel < 0 || o.debugChannel > 4)
        throw std::invalid_argument("Invalid debug channel");
    if (!std::isfinite(o.sunAngularRadius) || o.sunAngularRadius < 0 || o.sunAngularRadius > 0.25f ||
        !std::isfinite(o.contactShadowLength) || o.contactShadowLength < 0)
        throw std::invalid_argument("Invalid directional shadow options");
    view.wait();
    v.work = {};
    // 光照与 HDR 是否产出，与"是否出 Beauty"不是一回事：presentation 的 HDR 描述符是
    // 静态使用的，级联视图还要读它的内容，所以只要选了调试通道就必须有光照结果。
    // 旧实现按 color 导入 HDR，几何-only + 调试通道因此引用了未导入的图。
    const bool lighting = color || o.debugChannel != 0;
    if (lighting)
        v.ensureColor();
    if (o.debugChannel != 0)
        v.ensureDebug();
    // 上一帧已等待完成，此时才能替换共享平台缓冲与描述符绑定，也能安全回收瞬时资源。
    v.sweepTransients();
    v.beginTransients();

    Globals f{};
    const auto projection = camera.projection(v.w, v.h);
    f.vp = projection * camera.view();
    // 点尺寸用纯投影矩阵换算内参：投影的两轴尺度乘上各轴分辨率即为像素焦距，
    // 与视图旋转无关，也不会把 y 向下的负号带进半径。
    f.lens = {std::abs(projection[0][0]) * .5f * float(v.w), std::abs(projection[1][1]) * .5f * float(v.h), 0, 0};
    f.oldVP = v.lastVP;
    f.invVP = glm::inverse(f.vp);
    f.eye = {camera.position, 1};
    f.sunDirection = {glm::normalize(snapshot.sun.direction), 0};
    f.sunColor = {snapshot.sun.radiance, 1};
    const bool cascaded = lighting && o.shadows;
    const bool contact = cascaded && o.contactShadowLength > 0;
    const bool rayShadows = contact && o.rayQueryContactShadows && v.d.rayQuery;
    const glm::vec2 shadowOptions = cascaded ? glm::vec2(o.sunAngularRadius, o.contactShadowLength) : glm::vec2(0);
    f.shadowOptions = {shadowOptions, 0, 0};
    bool accumulate =
        o.taa && v.lastTaa && !o.resetHistory && v.lastChannel == o.debugChannel && snapshot.time >= v.time;
    accumulate = accumulate && snapshot.sun.direction == v.lastSun.direction &&
                 snapshot.sun.radiance == v.lastSun.radiance && sameLights(snapshot.lights, v.lastLights) &&
                 o.shadows == v.lastShadows && rayShadows == v.lastRayShadows && shadowOptions == v.lastShadowOptions;
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

    // 帧图：导入持久资源，再把帧级资源交给各模块声明自己的 pass。
    using render::Access;
    using render::Layout;
    auto& graph = v.graph;
    graph.reset();
    const auto albedo = graph.importImage("albedo", imageDesc(render::Format::Rgba8Unorm, v.w, v.h, colorUsage),
                                          graphLayout(v.g[0]->layout), false);
    const auto normalRough = graph.importImage("normal-rough",
                                               imageDesc(render::Format::Rgba16Float, v.w, v.h, colorUsage),
                                               graphLayout(v.g[1]->layout), false);
    const auto motion = graph.importImage("motion", imageDesc(render::Format::Rgba16Float, v.w, v.h, colorUsage),
                                          graphLayout(v.g[2]->layout), false);
    const auto metric = graph.importImage("metric-depth",
                                          imageDesc(render::Format::R32Float, v.w, v.h, colorUsage),
                                          graphLayout(v.g[3]->layout), false);
    const auto instanceId = graph.importImage("instance-id",
                                              imageDesc(render::Format::R32Uint, v.w, v.h, colorUsage),
                                              graphLayout(v.g[4]->layout), false);
    const auto depthImage = graph.importImage(
        "hardware-depth", imageDesc(render::Format::D32Float, v.w, v.h, depthUsage, render::Aspect::Depth),
        graphLayout(v.depth->layout), false);
    const auto globalsBuffer = graph.importBuffer("globals", {sizeof(Globals), render::BufferUsage::Uniform}, true);
    graph.bind(albedo, *v.g[0]);
    graph.bind(normalRough, *v.g[1]);
    graph.bind(motion, *v.g[2]);
    graph.bind(metric, *v.g[3]);
    graph.bind(instanceId, *v.g[4]);
    graph.bind(depthImage, *v.depth);
    graph.bind(globalsBuffer, *v.globals);
    // HDR 在"出颜色"或"选了调试通道"时都要导入：前者是时序解析的输入，
    // 后者是 presentation 的静态绑定（级联通道真的采样它，其余通道要求绑定有效）。
    // 显示图与历史槽只服务颜色链路，几何请求不分配也不需要它们。
    render::ImageHandle hdrImage, displayImage, historyImage[2];
    if (lighting)
    {
        hdrImage = graph.importImage("hdr", imageDesc(render::Format::Rgba16Float, v.w, v.h, colorUsage),
                                     graphLayout(v.hdr->layout), false);
        graph.bind(hdrImage, *v.hdr);
    }
    if (color)
    {
        displayImage = graph.importImage("display", imageDesc(render::Format::Rgba8Unorm, v.w, v.h, colorUsage),
                                         graphLayout(v.display->layout), false);
        for (int i = 0; i < 2; ++i)
            historyImage[i] = graph.importImage("history-" + std::to_string(i),
                                                imageDesc(render::Format::Rgba16Float, v.w, v.h, historyUsage),
                                                graphLayout(v.history[i]->layout), i == 1 - v.historySlot() && accumulate);
        graph.bind(displayImage, *v.display);
        graph.bind(historyImage[0], *v.history[0]);
        graph.bind(historyImage[1], *v.history[1]);
    }
    // 调试呈现图独立于 Beauty：只有选了调试通道才导入，且只被 presentation 写。
    render::ImageHandle debugImage;
    if (o.debugChannel != 0)
    {
        debugImage = graph.importImage("debug", imageDesc(render::Format::Rgba8Unorm, v.w, v.h, colorUsage),
                                       graphLayout(v.debug->layout), false);
        graph.bind(debugImage, *v.debug);
    }
    const auto stagingBuffer = graph.createBuffer(
        "readback", {VkDeviceSize(v.w) * v.h * 12, render::BufferUsage::TransferDst | render::BufferUsage::HostRead});

    vk::FrameResources res;
    res.graph = &graph;
    res.work = &v.work;
    res.rayContact = rayShadows;
    res.albedo = {albedo, v.g[0].get()};
    res.normalRough = {normalRough, v.g[1].get()};
    res.motion = {motion, v.g[2].get()};
    res.metricDepth = {metric, v.g[3].get()};
    res.instanceId = {instanceId, v.g[4].get()};
    res.hardwareDepth = {depthImage, v.depth.get()};
    res.hdr = {hdrImage, lighting ? v.hdr.get() : nullptr};
    res.display = {displayImage, color ? v.display.get() : nullptr};
    res.history[0] = {historyImage[0], color ? v.history[0].get() : nullptr};
    res.history[1] = {historyImage[1], color ? v.history[1].get() : nullptr};
    res.globals = {globalsBuffer, v.globals.get()};
    res.constants = &f;
    res.options = &o;
    res.camera = &camera;
    res.frameSet = v.frame;
    res.width = v.w;
    res.height = v.h;
    res.shadowTile = v.tile;
    res.lighting = lighting;
    res.stable = color && o.taa && (o.readback || o.stableGeometry);

    unsigned slot = v.historySlot();
    // 历史失效时先清除再采样：既保证定义内容，也让验证层看到合法布局。
    // 它只写历史槽，与模块无关，因此在模块声明前先排定。
    if (color && !accumulate)
        graph.addPass(
            graph.reserve("history.clear"),
            [&](render::FrameGraph::Declaration& d) { d.write(historyImage[1 - slot], Access::CopyDestination); },
            [&](VkCommandBuffer cmd) { recordHistoryClear(cmd, *v.history[1 - slot]); });
    // 模块按接口声明自己的 pass：主调度不判断网格或点云。
    v.stats = {};
    for (auto& module : v.modules)
        module->declare(res, v.stats);
    // 静止冻结要求相机与所有模块的输入都与上一帧相同；仍要有一个完整抖动周期。
    bool stationary = accumulate && f.vp == v.lastVP;
    for (auto& module : v.modules)
        stationary = stationary && module->unchanged();
    // 静止时逐像素平均一个完整抖动周期，保留前景与背景各自的覆盖贡献。
    f.info.w = stationary ? (v.stillSamples < temporalSamples ? 1.f / (v.stillSamples + 1) : 0.f) : -1.f;
    std::memcpy(v.globals->mapped, &f, sizeof(f));

    // 时序解析写回历史槽：它是否真的执行由编译结果决定，是历史状态提交的唯一依据。
    const auto temporalHandle = graph.reserve("temporal");
    if (color)
        graph.addPass(
            temporalHandle,
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
            graph.reserve("color"),
            [&](render::FrameGraph::Declaration& d)
            {
                d.read(historyImage[slot]);
                d.read(globalsBuffer, Access::UniformRead);
                d.write(displayImage, Access::ColorWrite);
            },
            [&](VkCommandBuffer cmd) { recordColor(cmd, v.pipe, v.frame, v.colorSet[slot], *v.display); });
    // 第二阶段：几何真值写回必须晚于时序解析与颜色变换。
    for (auto& module : v.modules)
        module->declareLate(res, v.stats);
    // 调试呈现排在稳定几何之后：有稳定副本时读到的法线、深度与标签就是像素中心的值，
    // 与同时交付的几何真值一致。它只写自己的图像，不参与时序解析与颜色变换。
    if (debugImage)
        graph.addPass(
            graph.reserve("presentation"),
            [&](render::FrameGraph::Declaration& d)
            {
                d.read(normalRough);
                d.read(metric);
                d.read(instanceId);
                // 描述符在创建时就绑定了 HDR，级联视图也确实读它，因此这里总是声明：
                // 声明与实际可访问的资源必须一致，否则屏障不会把该图转到可采样状态。
                d.read(hdrImage);
                d.read(globalsBuffer, Access::UniformRead);
                d.write(debugImage, Access::ColorWrite);
                // 选了调试通道，这一帧就产出调试图：窗口与集成方直接采样视图里的
                // 这张图，不经过 FrameResult 的复制通道，因此它是本帧的必备输出。
                d.sideEffect();
            },
            [&](VkCommandBuffer cmd) { recordPresentation(cmd, v.pipe, v.frame, v.debugSet, *v.debug); });

    if (o.readback)
        graph.addPass(
            graph.reserve("readback"),
            [&](render::FrameGraph::Declaration& d)
            {
                d.read(displayImage, Access::CopySource);
                d.read(metric, Access::CopySource);
                d.read(instanceId, Access::CopySource);
                d.write(stagingBuffer, Access::CopyDestination, {0, VkDeviceSize(v.w) * v.h * 12});
            },
            [&](VkCommandBuffer cmd)
            {
                std::array<Image*, 3> copies{v.display.get(), v.g[3].get(), v.g[4].get()};
                recordReadback(cmd, copies, *v.staging, v.w, v.h);
            });
    // 导出只覆盖本次请求的通道；恢复声明让持久视图资源在帧末回到只读，供下一帧导入。
    auto handleOf = [&](Channel channel)
    {
        switch (channel)
        {
        case Channel::Color:
            return displayImage;
        case Channel::Hdr:
            return hdrImage;
        case Channel::Resolved:
            return historyImage[slot];
        case Channel::Debug:
            return debugImage;
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
    // 只出光照不出 Beauty 的帧（几何-only + 调试通道）也必须把 HDR 放回可采样状态，
    // 否则物理图像的布局与视图记录的布局会分叉。
    if (lighting)
        graph.restoreImage(hdrImage, Layout::ShaderRead);
    if (color)
        for (auto handle : {displayImage, historyImage[0], historyImage[1]})
            graph.restoreImage(handle, Layout::ShaderRead);
    if (debugImage)
        graph.restoreImage(debugImage, Layout::ShaderRead);
    graph.compile();
    // 只有真实写入了颜色历史的帧才能推进历史状态。"出颜色"不等于"时序解析
    // 执行了"：仅导出 Hdr 的请求没有 color 的导出根，未启用 TAA 时也没有副作用根，时序解析
    // 会被裁掉，此时颜色历史仍是上一份内容，提前推进会让下一帧误判静止并冻结旧图。
    const bool resolved = color && graph.executed(temporalHandle);
    // 解析输出的可读槽位：未写入历史的帧没有 Resolved 可交付。
    v.resolvedSlot = resolved ? int(slot) : -1;
    // 编译结果决定实际需要的瞬时资源：只分配存活的声明。
    for (auto& entry : graph.transients())
    {
        if (entry.image)
            graph.bind(render::ImageHandle{entry.resource}, *v.transientImage(entry.imageDesc));
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
    // 模块登记的首次上传与本帧在同一队列上，用时间线值声明提交依赖。
    auto uploads = frame->uploads();
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    VkPipelineStageFlags uploadStage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (rayShadows)
        uploadStage |= VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
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
    v.retained = frame;
    v.work.publishSubmitted();
    v.shadows = {cascaded, o.rayQueryContactShadows,
                 rayShadows ? ContactShadow::RayQuery : (contact ? ContactShadow::ScreenSpace : ContactShadow::None),
                 shadowOptions.x, shadowOptions.y, cascaded ? v.tile : 0};

    // 历史有效性状态只在真正写入历史的帧上推进：几何真值、仅 HDR 交付或其他被裁剪的帧
    // 没有触碰历史，不能让下一帧误判历史已作废而重新积累，也不能让它误判为可复用。
    if (resolved)
    {
        v.lastVP = f.vp;
        v.stillSamples = stationary ? std::min(temporalSamples, v.stillSamples + 1) : unsigned(o.taa && !accumulate);
        v.lastSun = snapshot.sun;
        v.lastLights = snapshot.lights;
        v.lastShadows = o.shadows;
        v.lastRayShadows = rayShadows;
        v.lastShadowOptions = shadowOptions;
        v.lastTaa = o.taa;
        v.lastChannel = o.debugChannel;
        v.time = snapshot.time;
        ++v.historyNumber;
    }
    // 模块的颜色历史基线同样只在时序解析真正执行并成功提交后提交；
    // 未提交的帧只是把本帧身份丢弃，不改变基线。
    for (auto& module : v.modules)
        module->commitHistory(resolved);
    v.hasReadback = o.readback;
    v.stationary = resolved && stationary;
    v.visibleCount = v.stats.visibleMeshes;
    v.pointCount = v.stats.visiblePoints;
    ++v.number;
}
}
