#pragma once
#include "Scene.hpp"
#include "Output.hpp"
#include <vulkan/vulkan.h>
#include <functional>
#include <string>

namespace sim
{
namespace vk
{
class RenderScene;
class FrameLifetime;
}
struct DeviceOptions
{
    std::vector<const char*> instanceExtensions;
    std::function<VkSurfaceKHR(VkInstance)> createSurface;
    bool validation = false;
    std::filesystem::path shaderDirectory;
};
struct DeviceHandles
{
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    VkSurfaceKHR surface{};
};
struct RenderOptions
{
    // GPU 消费者可请求像素中心的几何真值，无需同时启用 CPU 读回。
    bool stableGeometry = false;
    bool taa = true, shadows = true, culling = true, readback = false, resetHistory = false;
    float exposure = 1;
    // 0 彩色，1 法线，2 米制深度，3 实例标签，4 级联分区。
    int debugChannel = 0;
};
struct Frame
{
    uint32_t width, height;
    double time;
    std::vector<uint8_t> rgba;
    std::vector<float> depth;
    std::vector<uint32_t> ids;
};
class View
{
  public:
    ~View();
    bool ready() const;
    void wait() const;
    Frame read() const;
    VkImageView colorView() const;
    VkImageView imageView(Channel channel) const;
    uint32_t visibleMeshes() const;
    // 本帧提交绘制的点数；点云不在 CPU 侧剔除，因此等于快照中的点数。
    uint32_t visiblePoints() const;
    // 上一帧是否被判为静止：相机、光照与所有模块的输入都与上一帧相同，历史被冻结复用。
    // 只在颜色帧上为真；几何真值交付不改变颜色历史基准。
    bool stationary() const;
    // 上一帧帧图实际执行的 pass 清单（按执行顺序），用于确认按需裁剪。
    const std::vector<std::string>& activePasses() const;

  private:
    friend class Renderer;
    struct Impl;
    explicit View(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl;
};
struct RenderRequest
{
    const SceneSnapshot& scene;
    const PerspectiveCamera& camera;
    View& view;
    std::vector<OutputDesc> outputs{{Channel::Color, Delivery::Cpu}};
    RenderOptions options;
};
// 资产与共享场景的准备计数，供集成方和测试观察共享行为。
struct AssetStats
{
    uint64_t scenePreparations = 0;  // 新建共享场景版本次数
    uint64_t sceneReuses = 0;        // 命中相同快照内容的次数
    uint64_t meshUploads = 0;        // 网格缓冲上传次数，键为资产身份
    uint64_t textureUploads = 0;     // 贴图上传次数，键为资产身份与色彩空间
    uint64_t materialUploads = 0;    // 材质描述符集创建次数，键为材质内容
    uint64_t uploadBatches = 0;      // 已提交的上传批次数
    uint32_t pendingUploads = 0;     // 尚未回收的上传批次数，正常提交路径应回落到零
    uint64_t stagingBytes = 0;       // 这些批次仍持有的暂存字节数
    uint32_t retiredGenerations = 0; // 仍在等待在途提交结束的旧缓存代际数
};
class Renderer
{
  public:
    explicit Renderer(const DeviceOptions& options = {});
    ~Renderer();
    std::unique_ptr<View> createView(uint32_t width, uint32_t height, uint32_t shadowTile = 1024);
    void render(const SceneSnapshot&, const PerspectiveCamera&, View&, const RenderOptions& = {});
    // 槽满时不等待结果的持有者，直接返回 Busy，避免调用线程自锁。
    Submission submit(const RenderRequest&);
    // 只切换缓存代际，不销毁仍被在途提交引用的缓冲。
    void clearAssetCache();
    AssetStats assetStats() const;
    const DeviceHandles& handles() const;
    VkSampler displaySampler() const;

  private:
    friend class View;
    friend class FrameResult;
    void renderFrame(const vk::FrameLifetime&, const SceneSnapshot&, const PerspectiveCamera&, View&,
                     const RenderOptions&, bool color, const std::vector<Channel>& exports);
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
