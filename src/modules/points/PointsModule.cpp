#include "PointsModule.hpp"
#include "../../render/GraphBridge.hpp"
#include "../../render/passes/Passes.hpp"
#include <cstring>

namespace sim::vk
{
namespace
{
// 视图侧点云状态：一个指向共享点云缓冲的描述符集合。点云没有逐相机的历史，
// 因此除尺寸外不需要其他状态。
class PointsView final : public ModuleView
{
  public:
    PointsView(PointsModule& module, const ModuleViewDesc& desc)
        : module(module), d(*desc.device), pipe(*desc.pipelines), frameSet(desc.frameSet), width(desc.width),
          height(desc.height)
    {
    }
    std::string_view name() const override
    {
        return "points";
    }
    void declare(FrameResources&, FrameStats&) override;
    void declareLate(FrameResources&, FrameStats&) override;

  private:
    PointsModule& module;
    Device& d;
    Pipelines& pipe;
    Lifetime life;
    VkDescriptorSet frameSet{};
    uint32_t width, height;
    VkDescriptorSet set{};
    render::PassHandle stableHandle;
    const Buffer* bound = nullptr;
};

void PointsView::declare(FrameResources& res, FrameStats& stats)
{
    auto* points = module.points();
    auto count = points ? points->count() : 0;
    stats.visiblePoints = count;
    if (!count)
        return;
    auto& graph = *res.graph;
    // 共享点云缓冲可能被上一帧的内容替换：只有数据换了才更新描述符。
    if (!set)
    {
        auto pool = descriptorPool(d, life, 1);
        set = allocateSet(d, pool, pipe.points);
    }
    if (bound != points->data().get())
    {
        bindBuffer(d, set, 0, *points->data(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        bound = points->data().get();
    }
    std::array<Image*, 5> geometry{res.albedo.image, res.normalRough.image, res.motion.image,
                                   res.metricDepth.image, res.instanceId.image};
    auto* hdr = res.color ? res.hdr.image : nullptr;
    auto* depth = res.hardwareDepth.image;

    using render::Access;
    auto declareSplat = [&](render::FrameGraph::Declaration& d)
    {
        // 全部附件都以 LOAD 方式进入：保留网格已写入的内容，只在点覆盖的像素上改写。
        d.readWrite(res.albedo.handle, Access::ColorWrite);
        d.readWrite(res.normalRough.handle, Access::ColorWrite);
        d.readWrite(res.motion.handle, Access::ColorWrite);
        d.readWrite(res.metricDepth.handle, Access::ColorWrite);
        d.readWrite(res.instanceId.handle, Access::ColorWrite);
        d.readWrite(res.hardwareDepth.handle, Access::DepthWrite);
        d.read(res.globals.handle, Access::UniformRead);
        if (hdr)
            d.readWrite(res.hdr.handle, Access::ColorWrite);
    };
    auto record = [&, geometry, hdr, depth, count](VkCommandBuffer cmd, bool stable)
    { recordSplat(cmd, pipe, frameSet, set, geometry, *depth, hdr, count, stable); };

    // 稳定点云与稳定几何同序：预留句柄，在第二阶段声明。
    if (res.stable)
        stableHandle = graph.reserve("points.stable");
    graph.addPass(graph.reserve("points"), declareSplat,
                  [record](VkCommandBuffer cmd) { record(cmd, false); });
}

void PointsView::declareLate(FrameResources& res, FrameStats&)
{
    auto* points = module.points();
    if (!res.stable || !points || !points->count())
        return;
    auto& graph = *res.graph;
    std::array<Image*, 5> geometry{res.albedo.image, res.normalRough.image, res.motion.image,
                                   res.metricDepth.image, res.instanceId.image};
    auto* depth = res.hardwareDepth.image;
    const auto count = points->count();
    using render::Access;
    // 几何真值 pass 沿用同一份点云但不触碰 HDR：真值与颜色的像素归属必须一致。
    graph.addPass(
        stableHandle,
        [&](render::FrameGraph::Declaration& d)
        {
            d.readWrite(res.albedo.handle, Access::ColorWrite);
            d.readWrite(res.normalRough.handle, Access::ColorWrite);
            d.readWrite(res.motion.handle, Access::ColorWrite);
            d.readWrite(res.metricDepth.handle, Access::ColorWrite);
            d.readWrite(res.instanceId.handle, Access::ColorWrite);
            d.readWrite(res.hardwareDepth.handle, Access::DepthWrite);
            d.read(res.globals.handle, Access::UniformRead);
        },
        [this, geometry, depth, count](VkCommandBuffer cmd)
        { recordSplat(cmd, pipe, frameSet, set, geometry, *depth, nullptr, count, true); });
}
}

PointsModule::PointsModule(Device& device, Pipelines& programs) : d(device), pipe(programs)
{
}
void PointsModule::prepare(const SceneSnapshot& scene, FrameLifetime& frame)
{
    // 内容键只覆盖影响共享缓冲的字段：节点标签、点云身份与变换。
    uint64_t key = 1469598103934665603ull;
    for (auto& object : scene.points)
    {
        auto identity = object.cloud ? object.cloud->id : 0;
        auto count = object.cloud ? object.cloud->points.size() : 0;
        for (auto value : {uint64_t(object.id), identity, uint64_t(count)})
        {
            key ^= value;
            key *= 1099511628211ull;
        }
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
            {
                uint32_t bits = 0;
                std::memcpy(&bits, &object.world[column][row], sizeof(bits));
                key ^= bits;
                key *= 1099511628211ull;
            }
    }
    if (auto it = cache.find(key); it != cache.end())
        if (auto live = it->second.lock(); live && live->matches(scene))
        {
            current = std::move(live);
            frame.hold(current);
            frame.waitUpload(current->uploads());
            return;
        }
    auto prepared = std::make_shared<RenderPoints>(d, scene, ++nextVersion);
    cache[key] = prepared;
    std::erase_if(cache, [](const auto& entry) { return entry.second.expired(); });
    current = prepared;
    frame.hold(current);
    frame.waitUpload(current->uploads());
}
void PointsModule::invalidateAssets()
{
    cache.clear();
    current.reset();
}
std::unique_ptr<ModuleView> PointsModule::view(const ModuleViewDesc& desc)
{
    return std::make_unique<PointsView>(*this, desc);
}
}
