#pragma once
#include "FrameData.hpp"
#include "Pipeline.hpp"
#include "../assets/AssetCache.hpp"
#include "graph/FrameGraph.hpp"
#include "sim/Renderer.hpp"
#include <memory>
#include <string_view>
#include <vector>

// 模块协议：主调度只按接口遍历模块，不出现网格/点云等类型分支。
// 模块负责自己的资产准备、批次、可见性、资源与 pass 注册；帧级共享资源由视图导入后
// 通过 FrameResources 按语义交给模块，模块不接触视图的私有状态。
namespace sim::vk
{
// 声明句柄与物理资源成对出现：前者用于帧图声明，后者用于录制绘制。
struct FrameImage
{
    render::ImageHandle handle;
    Image* image = nullptr;
    explicit operator bool() const
    {
        return bool(handle);
    }
};
struct FrameBuffer
{
    render::BufferHandle handle;
    Buffer* buffer = nullptr;
};
// 每帧统计：字段按类型分开，写入方是各模块本身，调度侧只做汇总。
struct FrameStats
{
    uint32_t visibleMeshes = 0;
    uint32_t visiblePoints = 0;
};
// 帧级共享资源。句柄由视图导入并绑定，模块只读使用；私有资源（阴影图等）由模块自己导入。
class FrameLifetime;
struct FrameResources
{
    render::FrameGraph* graph = nullptr;
    FrameLifetime* work = nullptr;
    bool rayContact = false;
    // 几何真值：五个 G-buffer 与硬件深度。
    FrameImage albedo, normalRough, motion, metricDepth, instanceId, hardwareDepth;
    // 颜色路径：光照结果、显示图、阴影图集与历史槽。
    FrameImage hdr, display, shadowMap;
    FrameImage history[2];
    FrameBuffer globals;
    // 本帧常量。模块可以写入自己负责的部分（例如 CSM 拟合结果），上传发生在所有模块声明之后。
    Globals* constants = nullptr;
    const RenderOptions* options = nullptr;
    const PerspectiveCamera* camera = nullptr;
    VkDescriptorSet frameSet = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0, shadowTile = 0;
    // 本帧是否产出光照结果（HDR）。它与"是否出 Beauty"不同：几何真值 + 调试通道的请求
    // 也需要光照，因为 presentation 的 HDR 绑定是静态使用的、级联视图要读它的内容。
    // 模块据此决定是否声明阴影与光照 pass、是否把 HDR 当颜色附件写。
    bool lighting = false;
    bool stable = false;  // 需要一份不抖动的几何真值
};
// 本帧所有模块共享数据的存活凭据：GPU 完成前，资产与实例缓冲不能回收。
class FrameLifetime
{
  public:
    void hold(std::shared_ptr<const void> data)
    {
        if (data)
            held.push_back(std::move(data));
    }
    // 模块登记自己首次上传的时间线值：帧提交据此建立依赖，而不是每台相机重新同步。
    void waitUpload(uint64_t value)
    {
        uploadValue = std::max(uploadValue, value);
    }
    uint64_t uploads() const
    {
        return uploadValue;
    }
    // 缓存只有在包含构建的帧成功入队后才可发布。
    void onSubmitted(std::function<void()> callback)
    {
        submitted.push_back(std::move(callback));
    }
    void publishSubmitted()
    {
        for (const auto& callback : submitted)
            callback();
        submitted.clear();
    }
    size_t size() const
    {
        return held.size();
    }

  private:
    std::vector<std::shared_ptr<const void>> held;
    std::vector<std::function<void()>> submitted;
    uint64_t uploadValue = 0;
};
// 视图侧模块状态：只保存与该相机相关的资源（阴影图、实例历史、描述符集合）。
struct ModuleViewDesc
{
    Device* device = nullptr;
    Pipelines* pipelines = nullptr;
    VkDescriptorSet frameSet = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0, shadowTile = 0;
};
class ModuleView
{
  public:
    virtual ~ModuleView() = default;
    virtual std::string_view name() const = 0;
    // 声明本模块的 pass。调用时 FrameResources 中的共享资源已导入并绑定。
    virtual void declare(FrameResources&, FrameStats&) = 0;
    // 第二阶段声明：在视图级后处理（时序解析、颜色变换、读回）之后调用。
    // 几何真值写回必须晚于这些 pass，否则它会在 TAA 解析前覆盖抖动样本。
    virtual void declareLate(FrameResources&, FrameStats&) {}
    // 与上一帧相比，本模块的输入是否完全未变；静止冻结由调度侧汇总各模块的结果。
    virtual bool unchanged() const
    {
        return true;
    }
    // 提交颜色历史：只有本帧的时序解析 pass 真正执行并成功提交后才传 true。
    // 模块在 declare 阶段只能记下"待提交"的状态，不能提前改写历史基线，否则一次被裁剪的
    // 时序解析（例如仅导出 Hdr 且未启用 TAA）也会让基线跳到未写入的内容上。
    virtual void commitHistory(bool)
    {
    }
};
// 当前代际的资产缓存入口：模块按身份取 GPU 资产，不持有代际的所有权。
struct AssetProvider
{
    virtual ~AssetProvider() = default;
    virtual std::shared_ptr<AssetCache> currentAssets() = 0;
};
// 渲染侧模块：每个 Renderer 一份，负责跨相机共享的准备与缓存。
class RenderModule
{
  public:
    virtual ~RenderModule() = default;
    virtual std::string_view name() const = 0;
    // 快照准备：同一内容被多台相机请求时只准备与上传一次。共享数据登记进 frame，
    // 保证 GPU 完成前不被回收。
    virtual void prepare(const SceneSnapshot&, FrameLifetime& frame) = 0;
    // 缓存代际切换：按内容缓存的数据全部作废。
    virtual void invalidateAssets() {}
    virtual std::unique_ptr<ModuleView> view(const ModuleViewDesc&) = 0;
};
}
