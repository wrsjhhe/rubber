#include "MeshModule.hpp"
#include "../../render/GraphBridge.hpp"
#include "../../render/passes/Passes.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace sim::vk
{
namespace
{
// 视图侧网格状态：阴影图集、上一帧变换、可见列表与光照描述符集合。
// 这些资源与相机相关，因此随视图创建，不与其他相机共享。
class MeshView final : public ModuleView
{
  public:
    MeshView(MeshModule& module, const ModuleViewDesc& desc)
        : module(module), d(*desc.device), pipe(*desc.pipelines), frameSet(desc.frameSet), w(desc.width),
          h(desc.height), tile(desc.shadowTile)
    {
    }
    std::string_view name() const override
    {
        return "mesh";
    }
    bool unchanged() const override
    {
        return stationary;
    }
    void declare(FrameResources&, FrameStats&) override;
    void declareLate(FrameResources&, FrameStats&) override;

  private:
    // 共享场景的缓冲与视图历史只在等待本视图上一帧后重新绑定。
    void useScene(const RenderScene& next)
    {
        if (scene == &next)
            return;
        scene = &next;
        auto bytes = std::max<size_t>(1, next.items().size()) * sizeof(ViewInstance);
        if (!viewData || viewData->size < bytes)
            viewData = d.buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        bindBuffer(d, frameSet, 1, next.instances(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        bindBuffer(d, frameSet, 2, *viewData, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
    // 颜色链路的私有资源：阴影图集与光照描述符集合。
    void ensureColor(const FrameResources& res)
    {
        if (shadow)
            return;
        shadow = d.image(tile * 2, tile * 2, depthFormat, vkUsageOf(depthUsage));
        auto pool = descriptorPool(d, life, 8);
        lighting = allocateSet(d, pool, pipe.lighting);
        unsigned slot = 0;
        for (auto* image : {res.albedo.image, res.normalRough.image, res.motion.image, res.metricDepth.image,
                            res.instanceId.image})
            bindImage(d, lighting, slot++, *image, pipe.point);
        bindImage(d, lighting, 5, *res.hardwareDepth.image, pipe.point);
        bindImage(d, lighting, 6, *shadow, pipe.point);
    }

    MeshModule& module;
    Device& d;
    Pipelines& pipe;
    Lifetime life;
    VkDescriptorSet frameSet{};
    uint32_t w, h, tile;
    const RenderScene* scene = nullptr;
    Img shadow;
    Buf viewData;
    VkDescriptorSet lighting{};
    std::array<Image*, 5> targets{};
    std::vector<uint32_t> visibleItems;
    render::PassHandle stableHandle;
    std::unordered_map<uint32_t, MeshHistory> lastModels;
    bool stationary = true;
};

void MeshView::declare(FrameResources& res, FrameStats& stats)
{
    auto& graph = *res.graph;
    auto& globals = *res.constants;
    const auto& options = *res.options;
    const auto& sceneRef = module.scene();
    const auto& items = sceneRef.items();
    useScene(sceneRef);
    targets = {res.albedo.image, res.normalRough.image, res.motion.image, res.metricDepth.image,
               res.instanceId.image};
    if (res.color)
        ensureColor(res);
    // 实例历史是网格模块私有的视图缓冲：每帧导入并绑定，由帧图管理本帧的布局与屏障。
    const auto viewBuffer =
        graph.importBuffer("view-history", {viewData->size, render::BufferUsage::Storage}, true);
    graph.bind(viewBuffer, *viewData);
    render::ImageHandle shadowMap;
    if (res.color)
    {
        shadowMap = graph.importImage(
            "shadow", imageDesc(render::Format::D32Float, tile * 2, tile * 2, depthUsage, render::Aspect::Depth),
            graphLayout(shadow->layout), true);
        graph.bind(shadowMap, *shadow);
    }

    // 抖动后的裁剪矩阵用于剔除：边缘物体不能因为亚像素位移被裁掉。
    auto rasterVP = globals.vp;
    for (unsigned j = 0; j < 4; ++j)
    {
        rasterVP[j].x += globals.jitter.x * rasterVP[j].w;
        rasterVP[j].y += globals.jitter.y * rasterVP[j].w;
    }
    visibleItems.clear();
    std::unordered_map<uint32_t, MeshHistory> models;
    bool still = items.size() == lastModels.size();
    for (uint32_t i = 0; i < items.size(); ++i)
    {
        auto& item = items[i];
        auto prev = lastModels.find(item.id);
        MeshHistory state{item.world, item.geometry, item.material, item.castsShadow, i};
        if (!models.emplace(item.id, state).second)
            throw std::invalid_argument("Duplicate snapshot mesh ID");
        // 资产身份或材质值变化即视为新对象：既不能复用旧姿态，也不能继续冻结历史。
        bool known = prev != lastModels.end() && prev->second.geometry == item.geometry &&
                     prev->second.material == item.material;
        still = still && known && prev->second == state;
        // 视图只写上一帧变换与匹配标记；当前变换来自共享场景的静态实例数据。
        ViewInstance history{known ? prev->second.world : item.world, {0, uint32_t(known), 0, 0}};
        std::memcpy(static_cast<char*>(viewData->mapped) + i * sizeof(history), &history, sizeof(history));
        if (!options.culling || visible(item.world, item.minimum, item.maximum, globals.vp) ||
            (options.taa && visible(item.world, item.minimum, item.maximum, rasterVP)))
            visibleItems.push_back(i);
    }
    stationary = still;
    // 只有颜色帧推进历史：几何真值或 GPU 交付帧不改变 TAA 与重投影的历史基准。
    if (res.color)
        lastModels = std::move(models);
    stats.visibleMeshes = uint32_t(visibleItems.size());
    if (res.color && options.shadows)
        fitShadows(globals, *res.camera, items, tile);

    using render::Access;
    auto declareTargets = [&](render::FrameGraph::Declaration& d)
    {
        d.write(res.albedo.handle, Access::ColorWrite);
        d.write(res.normalRough.handle, Access::ColorWrite);
        d.write(res.motion.handle, Access::ColorWrite);
        d.write(res.metricDepth.handle, Access::ColorWrite);
        d.write(res.instanceId.handle, Access::ColorWrite);
        d.write(res.hardwareDepth.handle, Access::DepthWrite);
        d.read(res.globals.handle, Access::UniformRead);
        d.read(viewBuffer, Access::StorageRead);
    };
    if (res.color && options.shadows)
        graph.addPass(
            graph.reserve("shadow"),
            [&](render::FrameGraph::Declaration& d) { d.write(shadowMap, Access::DepthWrite); },
            [this, &items, &globals](VkCommandBuffer cmd)
            { recordShadow(cmd, pipe, frameSet, *shadow, items, globals, tile); });
    graph.addPass(
        graph.reserve("geometry"), declareTargets,
        [this, &items, &res](VkCommandBuffer cmd)
        { recordGeometry(cmd, pipe, frameSet, items, visibleItems, targets, *res.hardwareDepth.image, false); });
    // 稳定几何按原有时序预留：它必须在时序解析之后、读回之前执行。
    if (res.stable)
        stableHandle = graph.reserve("geometry.stable");
    if (res.color)
        graph.addPass(
            graph.reserve("lighting"),
            [&](render::FrameGraph::Declaration& d)
            {
                d.read(res.albedo.handle);
                d.read(res.normalRough.handle);
                d.read(res.motion.handle);
                d.read(res.metricDepth.handle);
                d.read(res.instanceId.handle);
                d.read(res.hardwareDepth.handle);
                d.read(shadowMap);
                d.read(res.globals.handle, Access::UniformRead);
                d.write(res.hdr.handle, Access::ColorWrite);
            },
            [this, &res](VkCommandBuffer cmd)
            { recordLighting(cmd, pipe, frameSet, lighting, *res.hdr.image); });
}

void MeshView::declareLate(FrameResources& res, FrameStats&)
{
    if (!res.stable)
        return;
    auto& graph = *res.graph;
    const auto viewBuffer =
        graph.importBuffer("view-history", {viewData->size, render::BufferUsage::Storage}, true);
    graph.bind(viewBuffer, *viewData);
    using render::Access;
    // 稳定几何覆盖 G-buffer 供真值读回：固定像素中心采样，且不再抖动。
    graph.addPass(
        stableHandle,
        [&](render::FrameGraph::Declaration& d)
        {
            d.write(res.albedo.handle, Access::ColorWrite);
            d.write(res.normalRough.handle, Access::ColorWrite);
            d.write(res.motion.handle, Access::ColorWrite);
            d.write(res.metricDepth.handle, Access::ColorWrite);
            d.write(res.instanceId.handle, Access::ColorWrite);
            d.write(res.hardwareDepth.handle, Access::DepthWrite);
            d.read(res.globals.handle, Access::UniformRead);
            d.read(viewBuffer, Access::StorageRead);
        },
        [this, &res](VkCommandBuffer cmd)
        {
            const auto& items = module.scene().items();
            recordGeometry(cmd, pipe, frameSet, items, visibleItems, targets, *res.hardwareDepth.image, true);
        });
}
}

MeshModule::MeshModule(Device& device, Pipelines& programs, AssetProvider& assets, AssetStats& counters)
    : d(device), pipe(programs), provider(assets), stats(counters)
{
}
void MeshModule::prepare(const SceneSnapshot& scene, FrameLifetime& frame)
{
    auto assets = provider.currentAssets();
    auto key = sceneKey(scene, assets->generation);
    if (auto it = scenes.find(key); it != scenes.end())
        if (auto live = it->second.lock(); live && live->matches(scene))
        {
            ++stats.sceneReuses;
            current = std::move(live);
            frame.hold(current);
            frame.waitUpload(current->uploads());
            return;
        }
    auto prepared = std::make_shared<RenderScene>(d, assets, scene, ++nextVersion);
    scenes[key] = prepared;
    ++stats.scenePreparations;
    std::erase_if(scenes, [](const auto& entry) { return entry.second.expired(); });
    current = prepared;
    frame.hold(current);
    frame.waitUpload(current->uploads());
}
void MeshModule::invalidateAssets()
{
    scenes.clear();
    current.reset();
}
std::unique_ptr<ModuleView> MeshModule::view(const ModuleViewDesc& desc)
{
    return std::make_unique<MeshView>(*this, desc);
}
}
