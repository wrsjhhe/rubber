#pragma once
#include "../../render/Module.hpp"
#include "../../render/RenderScene.hpp"
#include <string>
#include <unordered_map>

// 网格模块：掌握三角网格的资产、实例批次、可见性，以及阴影、几何与光照三类 pass。
// 主调度只通过 ModuleView::declare 调用，不出现任何网格专用判断。
namespace sim::vk
{
class MeshModule final : public RenderModule
{
  public:
    MeshModule(Device&, Pipelines&, AssetProvider&, AssetStats&);
    std::string_view name() const override
    {
        return "mesh";
    }
    void prepare(const SceneSnapshot&, FrameLifetime& frame) override;
    void invalidateAssets() override;
    std::unique_ptr<ModuleView> view(const ModuleViewDesc&) override;
    // 本帧准备好的共享场景；视图在 declare 时读取，生命周期由 FrameLifetime 保证。
    const RenderScene& scene() const
    {
        return *current;
    }

  private:
    Device& d;
    Pipelines& pipe;
    AssetProvider& provider;
    AssetStats& stats;
    std::unordered_map<uint64_t, std::weak_ptr<RenderScene>> scenes;
    uint64_t nextVersion = 0;
    std::shared_ptr<RenderScene> current;
};
}
