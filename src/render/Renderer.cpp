#include "Pipeline.hpp"
#include "../assets/AssetCache.hpp"
#include "sim/VulkanInterop.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <stdexcept>
#include <cmath>

namespace sim
{
using namespace vk;
namespace
{
struct Globals
{
    glm::mat4 vp, oldVP, invVP, lightVP[4];
    glm::vec4 eye, sunDirection, sunColor, splits, jitter, settings, pointPosition[16], pointColor[16], info;
};
struct Instance
{
    glm::mat4 model, oldModel;
    glm::vec4 colorRough, material;
    glm::uvec4 label;
    glm::vec4 boundsLo, boundsHi;
};
static_assert(sizeof(Globals) == 1072 && sizeof(Instance) == 208);
struct Draw
{
    uint32_t object;
    int cascade, stable;
};
constexpr unsigned temporalSamples = 16;
struct MeshHistory
{
    glm::mat4 world;
    Geometry geometry;
    std::shared_ptr<const MeshStandardMaterial> material;
    glm::vec4 colorRough, parameters;
    bool castsShadow;
    uint32_t order;
    bool operator==(const MeshHistory&) const = default;
};
bool sameLights(const std::vector<PointLight>& a, const std::vector<PointLight>& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y)
                                              { return x.position == y.position && x.intensity == y.intensity; });
}
std::array<glm::vec4, 8> corners(const glm::mat4& m, glm::vec3 lo, glm::vec3 hi)
{
    std::array<glm::vec4, 8> p;
    for (int i = 0; i < 8; ++i)
        p[i] = m * glm::vec4(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z, 1);
    return p;
}
bool visible(const Mesh& mesh, const glm::mat4& vp)
{
    auto p = corners(vp * mesh.world, mesh.geometry->minimum, mesh.geometry->maximum);
    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::all_of(p.begin(), p.end(), [&](auto v) { return v[axis] < (axis == 2 ? 0.f : -v.w); }))
            return false;
        if (std::all_of(p.begin(), p.end(), [&](auto v) { return v[axis] > v.w; }))
            return false;
    }
    return true;
}
void fitShadows(Globals& g, const PerspectiveCamera& camera, const SceneSnapshot& scene, uint32_t tile)
{
    glm::vec3 dir = g.sunDirection;
    auto rotation =
        glm::lookAtRH(glm::vec3(0), dir, std::abs(dir.y) > .98f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));
    auto frustum = corners(g.invVP, {-1, -1, 0}, {1, 1, 1});
    for (auto& p : frustum)
        p = rotation * (p / p.w);
    // 把当前场景潜在投影物体的深度纳入覆盖范围。
    float loZ = frustum[0].z, hiZ = loZ;
    for (auto& m : scene.meshes)
        if (m.castsShadow)
        {
            for (auto p : corners(rotation * m.world, m.geometry->minimum, m.geometry->maximum))
            {
                loZ = std::min(loZ, p.z);
                hiZ = std::max(hiZ, p.z);
            }
        }
    float n = camera.nearPlane, f = camera.farPlane, previous = n;
    for (int c = 0; c < 4; ++c)
    {
        float t = (c + 1) * .25f;
        float split = glm::mix(n + (f - n) * t, n * std::pow(f / n, t), .72f);
        g.splits[c] = split;
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (int i = 0; i < 4; ++i)
            for (float s : {previous, split})
            {
                glm::vec3 p = glm::mix(frustum[i], frustum[i + 4], (s - n) / (f - n));
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
        float extent = std::max(hi.x - lo.x, hi.y - lo.y) * .5f + 0.04f;
        float step = 2 * extent / tile;
        glm::vec2 center = glm::round(glm::vec2((lo + hi) * .5f) / step) * step;
        auto projection = glm::orthoRH_ZO(center.x - extent, center.x + extent, center.y - extent,
                                          center.y + extent, -std::max(hi.z, hiZ) - 1, -std::min(lo.z, loZ) + 1);
        g.lightVP[c] = projection * rotation;
        previous = split;
    }
}
float halton(unsigned index, unsigned base)
{
    float f = 1, r = 0;
    while (index)
    {
        f /= base;
        r += f * (index % base);
        index /= base;
    }
    return r;
}
}
struct Renderer::Impl
{
    Device d;
    Lifetime life;
    std::unique_ptr<Pipelines> pipelines;
    std::filesystem::path shaderDirectory;
    std::shared_ptr<AssetCache> assets;
    explicit Impl(const DeviceOptions& o)
    {
        d.init(o);
        shaderDirectory =
            o.shaderDirectory.empty() ? std::filesystem::path(RENDERER_SHADER_DIR) : o.shaderDirectory;
        pipelines = std::make_unique<Pipelines>(d, shaderDirectory);
        assets = std::make_shared<AssetCache>(d, *pipelines);
    }
};
struct FrameResult::Impl
{
    std::shared_ptr<Renderer::Impl> owner;
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
    std::shared_ptr<AssetCache> frameAssets;
    Buf globals, instances, staging;
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkDescriptorSet frame{}, lighting{}, resolve[2]{};
    unsigned number = 0, visibleCount = 0;
    double time = 0;
    bool hasReadback = false, lastTaa = false;
    int lastChannel = 0;
    glm::mat4 lastVP{1};
    unsigned stillSamples = 0;
    DirectionalLight lastSun;
    std::vector<PointLight> lastLights;
    bool lastShadows = false;
    std::unordered_map<uint32_t, MeshHistory> lastModels;
    Impl(std::shared_ptr<Renderer::Impl> r, uint32_t width, uint32_t height, uint32_t shadowTile)
        : owner(std::move(r)), d(owner->d), pipe(*owner->pipelines), w(width), h(height), tile(shadowTile)
    {
        auto max = d.properties.limits.maxImageDimension2D;
        if (!w || !h || !tile || w > max || h > max || tile > max / 2)
            throw std::invalid_argument("Invalid view dimensions");
        const auto color = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        const auto z = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        for (unsigned i = 0; i < 5; ++i)
            g[i] = d.image(w, h, gFormats[i], color | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        depth = d.image(w, h, depthFormat, z | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
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
    void ensureColor()
    {
        if (display)
            return;
        const auto color = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        const auto z = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        shadow = d.image(tile * 2, tile * 2, depthFormat, z);
        hdr = d.image(w, h, hdrFormat, color | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        display = d.image(w, h, colorFormat, color | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        for (auto& img : history)
            img = d.image(w, h, hdrFormat, color | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        auto pool = descriptorPool(d, life, 4);
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
        }
    }
    void draw(const Packet& p, uint32_t i, int cascade, bool stable)
    {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &p.mesh->vertices->handle, &offset);
        vkCmdBindIndexBuffer(cmd, p.mesh->indices->handle, 0, VK_INDEX_TYPE_UINT32);
        if (cascade < 0)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.meshLayout, 1, 1, &p.material, 0,
                                    nullptr);
        Draw push{i, std::max(cascade, 0), stable};
        vkCmdPushConstants(cmd, pipe.meshLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDrawIndexed(cmd, p.mesh->count, 1, 0, 0, 0);
    }
    void shadowPass(const SceneSnapshot& scene, const std::vector<Packet>& packets, const Globals& f, bool enabled)
    {
        if (enabled)
        {
            beginPass(cmd, {}, shadow.get());
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.shadow);
            for (int c = 0; c < 4; ++c)
            {
                viewport(cmd, (c % 2) * tile, (c / 2) * tile, tile, tile);
                for (uint32_t i = 0; i < scene.meshes.size(); ++i)
                {
                    auto& m = scene.meshes[i];
                    if (m.castsShadow && visible(m, f.lightVP[c]))
                        draw(packets[i], i, c, false);
                }
            }
            vkCmdEndRendering(cmd);
        }
        shadow->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    void geometryPass(const std::vector<Packet>& packets, const std::vector<uint32_t>& list, bool stable)
    {
        std::array<Image*, 5> colors;
        for (int i = 0; i < 5; ++i)
            colors[i] = g[i].get();
        beginPass(cmd, colors, depth.get());
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.geometry);
        for (auto i : list)
            draw(packets[i], i, -1, stable);
        vkCmdEndRendering(cmd);
        for (auto& img : g)
            img->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        depth->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    void fullscreen(std::span<Image*> outputs, VkPipeline pipeline, VkPipelineLayout layout,
                    VkDescriptorSet inputs)
    {
        beginPass(cmd, outputs, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkDescriptorSet sets[] = {frame, inputs};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        for (auto img : outputs)
            img->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    void copyOutputs()
    {
        VkDeviceSize offset = 0;
        for (auto img : {display.get(), g[3].get(), g[4].get()})
        {
            img->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy c{};
            c.bufferOffset = offset;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(cmd, img->handle, img->layout, staging->handle, 1, &c);
            img->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            offset += VkDeviceSize(w) * h * 4;
        }
    }
};
Renderer::Renderer(const DeviceOptions& o) : impl(std::make_shared<Impl>(o)) {}
Renderer::~Renderer() = default;
View::View(std::unique_ptr<Impl> p) : impl(std::move(p)) {}
View::~View() = default;
void Renderer::clearAssetCache()
{
    impl->assets = std::make_shared<AssetCache>(impl->d, *impl->pipelines);
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
    for (auto output : request.outputs)
    {
        auto i = size_t(output.channel);
        if (i >= seen.size() || seen[i])
            throw std::invalid_argument("Invalid or duplicate output channel");
        if (output.delivery != Delivery::Cpu && output.delivery != Delivery::Gpu &&
            output.delivery != Delivery::CpuAndGpu)
            throw std::invalid_argument("Invalid output delivery");
        seen[i] = true;
        color = color || output.channel == Channel::Color || output.channel == Channel::Hdr;
    }
    std::erase_if(v.results, [](const auto& result) { return result.expired(); });
    if (v.results.size() >= 3)
        return {};

    auto state = std::make_shared<FrameResult::Impl>(impl);
    state->timestamp = request.scene.time;
    std::vector<Image*> sources;
    if (color)
        v.ensureColor();
    for (auto output : request.outputs)
    {
        Image* source = nullptr;
        switch (output.channel)
        {
        case Channel::Color:
            source = v.display.get();
            break;
        case Channel::Hdr:
            source = v.hdr.get();
            break;
        case Channel::HardwareDepth:
            source = v.depth.get();
            break;
        default:
            source = v.g[size_t(output.channel) - 1].get();
            break;
        }
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
    renderFrame(request.scene, request.camera, request.view, options, color);

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
uint32_t View::visibleMeshes() const
{
    return impl->visibleCount;
}
Frame View::read() const
{
    auto& v = *impl;
    if (!v.hasReadback)
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
    renderFrame(scene, camera, view, o, true);
}
void Renderer::renderFrame(const SceneSnapshot& scene, const PerspectiveCamera& camera, View& view,
                           const RenderOptions& o, bool color)
{
    auto& v = *view.impl;
    if (v.owner != impl)
        throw std::invalid_argument("View belongs to a different renderer");
    if (scene.lights.size() > 16 || glm::length(scene.sun.direction) < 1e-6f)
        throw std::invalid_argument("Invalid lights");
    view.wait();
    if (color)
        v.ensureColor();
    v.frameAssets = impl->assets;

    Globals f{};
    f.vp = camera.projection(v.w, v.h) * camera.view();
    f.oldVP = v.lastVP;
    f.invVP = glm::inverse(f.vp);
    f.eye = {camera.position, 1};
    f.sunDirection = {glm::normalize(scene.sun.direction), 0};
    f.sunColor = {scene.sun.radiance, 1};
    bool accumulate =
        o.taa && v.lastTaa && !o.resetHistory && v.lastChannel == o.debugChannel && scene.time >= v.time;
    accumulate = accumulate && scene.sun.direction == v.lastSun.direction &&
                 scene.sun.radiance == v.lastSun.radiance && sameLights(scene.lights, v.lastLights) &&
                 o.shadows == v.lastShadows;
    bool stationary = accumulate && f.vp == v.lastVP && scene.meshes.size() == v.lastModels.size();
    auto index = (v.number % temporalSamples) + 1;
    glm::vec2 jitter =
        o.taa ? glm::vec2((2 * halton(index, 2) - 1) / v.w, (2 * halton(index, 3) - 1) / v.h) : glm::vec2(0);
    f.jitter = {jitter, 0, 0};
    f.settings = {o.exposure, float(accumulate), float(o.shadows), float(o.debugChannel)};
    f.info = {float(scene.lights.size()), 1.f / v.w, 1.f / v.h, 0};
    for (size_t i = 0; i < scene.lights.size(); ++i)
    {
        f.pointPosition[i] = {scene.lights[i].position, 1};
        f.pointColor[i] = {scene.lights[i].intensity, 1};
    }

    size_t bytes = std::max<size_t>(1, scene.meshes.size()) * sizeof(Instance);
    if (bytes > v.d.properties.limits.maxStorageBufferRange)
        throw std::length_error("Instance buffer exceeds device limit");
    if (!v.instances || v.instances->size < bytes)
    {
        v.instances = v.d.buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        bindBuffer(v.d, v.frame, 1, *v.instances, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
    auto rasterVP = f.vp;
    for (unsigned j = 0; j < 4; ++j)
    {
        rasterVP[j].x += jitter.x * rasterVP[j].w;
        rasterVP[j].y += jitter.y * rasterVP[j].w;
    }
    std::vector<uint32_t> visibleList;
    std::vector<Packet> packets;
    std::unordered_map<uint32_t, MeshHistory> models;
    for (uint32_t i = 0; i < scene.meshes.size(); ++i)
    {
        auto& m = scene.meshes[i];
        if (!m.geometry || !m.material || !m.id)
            throw std::invalid_argument("Invalid snapshot mesh");
        auto prev = v.lastModels.find(m.id);
        auto& mat = *m.material;
        MeshHistory state{m.world,
                          m.geometry,
                          m.material,
                          {mat.color, mat.roughness},
                          {mat.metalness, mat.uvRepeat.x, mat.uvRepeat.y, float(bool(mat.normalMap))},
                          m.castsShadow,
                          i};
        if (!models.emplace(m.id, state).second)
            throw std::invalid_argument("Duplicate snapshot mesh ID");
        bool known = prev != v.lastModels.end() && prev->second.geometry == m.geometry &&
                     prev->second.material == m.material;
        stationary = stationary && known && prev->second == state;
        Instance obj{m.world,
                     known ? prev->second.world : m.world,
                     state.colorRough,
                     state.parameters,
                     {m.id, known, 0, 0},
                     {m.geometry->minimum, 1},
                     {m.geometry->maximum, 1}};
        std::memcpy(static_cast<char*>(v.instances->mapped) + i * sizeof(obj), &obj, sizeof(obj));
        packets.push_back({&v.frameAssets->mesh(m.geometry), v.frameAssets->material(m.material)});
        if (!o.culling || visible(m, f.vp) || (o.taa && visible(m, rasterVP)))
            visibleList.push_back(i);
    }
    if (color && o.shadows)
        fitShadows(f, camera, scene, v.tile);
    // 静止时逐像素平均一个完整抖动周期，保留前景与背景各自的覆盖贡献。
    f.info.w = stationary ? (v.stillSamples < temporalSamples ? 1.f / (v.stillSamples + 1) : 0.f) : -1.f;
    std::memcpy(v.globals->mapped, &f, sizeof(f));
    if (o.readback && !v.staging)
        v.staging = v.d.buffer(VkDeviceSize(v.w) * v.h * 12, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);

    check(vkResetCommandBuffer(v.cmd, 0), "reset frame commands");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(v.cmd, &bi), "begin frame");
    vkCmdBindDescriptorSets(v.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v.pipe.meshLayout, 0, 1, &v.frame, 0, nullptr);
    if (color)
        v.shadowPass(scene, packets, f, o.shadows);
    v.geometryPass(packets, visibleList, false);
    if (color)
    {
        std::array lightOutputs{v.hdr.get()};
        v.fullscreen(lightOutputs, v.pipe.shade, v.pipe.lightLayout, v.lighting);
        unsigned slot = v.number % 2;
        v.history[1 - slot]->transition(v.cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        std::array resolveOutputs{v.history[slot].get(), v.display.get()};
        v.fullscreen(resolveOutputs, v.pipe.temporal, v.pipe.resolveLayout, v.resolve[slot]);
    }
    // 传感器真值始终以无抖动的像素中心采样。
    if (o.taa && (o.readback || o.stableGeometry))
        v.geometryPass(packets, visibleList, true);
    if (o.readback)
        v.copyOutputs();
    check(vkEndCommandBuffer(v.cmd), "end frame");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &v.cmd;
    check(vkResetFences(v.d.device, 1, &v.fence), "reset frame fence");
    check(vkQueueSubmit(v.d.queue, 1, &si, v.fence), "submit frame");

    v.lastVP = f.vp;
    v.stillSamples = stationary ? std::min(temporalSamples, v.stillSamples + 1) : unsigned(o.taa && !accumulate);
    v.lastSun = scene.sun;
    v.lastLights = scene.lights;
    v.lastShadows = o.shadows;
    v.lastModels = std::move(models);
    v.lastTaa = o.taa;
    v.lastChannel = o.debugChannel;
    v.hasReadback = o.readback;
    v.visibleCount = uint32_t(visibleList.size());
    v.time = scene.time;
    ++v.number;
}
}
