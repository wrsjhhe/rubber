#pragma once
#include "../../render/Module.hpp"
#include "RenderPoints.hpp"
#include <string>
#include <unordered_map>

// 点云模块：掌握点云资产、共享 GPU 缓冲与自己的绘制 pass。
// 与网格模块并列注册，主调度不区分类型；两者的遮挡由同一张深度图保证。
namespace sim::vk
{
class PointsModule final : public RenderModule
{
  public:
    PointsModule(Device&, Pipelines&);
    std::string_view name() const override
    {
        return "points";
    }
    void prepare(const SceneSnapshot&, FrameLifetime& frame) override;
    void invalidateAssets() override;
    std::unique_ptr<ModuleView> view(const ModuleViewDesc&) override;
    // 本帧准备好的共享点云数据；生命周期由 FrameLifetime 保证。
    const RenderPoints* points() const
    {
        return current.get();
    }

  private:
    Device& d;
    Pipelines& pipe;
    std::unordered_map<uint64_t, std::weak_ptr<RenderPoints>> cache;
    uint64_t nextVersion = 0;
    std::shared_ptr<RenderPoints> current;
};
}
