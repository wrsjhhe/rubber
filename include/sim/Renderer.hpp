#pragma once
#include "Scene.hpp"
#include "Output.hpp"
#include <vulkan/vulkan.h>
#include <functional>
#include <string>

namespace sim
{
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
class Renderer
{
  public:
    explicit Renderer(const DeviceOptions& options = {});
    ~Renderer();
    std::unique_ptr<View> createView(uint32_t width, uint32_t height, uint32_t shadowTile = 1024);
    void render(const SceneSnapshot&, const PerspectiveCamera&, View&, const RenderOptions& = {});
    // 槽满时不等待结果的持有者，直接返回 Busy，避免调用线程自锁。
    Submission submit(const RenderRequest&);
    void clearAssetCache();
    const DeviceHandles& handles() const;
    VkSampler displaySampler() const;

  private:
    friend class View;
    friend class FrameResult;
    void renderFrame(const SceneSnapshot&, const PerspectiveCamera&, View&, const RenderOptions&, bool color);
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
