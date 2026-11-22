#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace sim
{
enum class Channel
{
    Color,
    AlbedoMetal,
    NormalRough,
    Motion,
    MetricDepth,
    InstanceId,
    HardwareDepth,
    Hdr,
    // 时序解析后的线性颜色：Hdr 是光照结果，这里才是 TAA 之后的辐亮度，
    // 未经曝光与色调映射。传感器若需要线性图像应取这个通道。
    // A 分量是该帧的米制深度（时序拒绝依赖它），不是透明通道。
    Resolved,
    // 调试呈现：由独立的 presentation 生成，与 Color/Hdr 互不覆盖。
    // 请求它必须同时用 RenderOptions::debugChannel 指定要看的通道。
    Debug
};
enum class Delivery
{
    Cpu,
    Gpu,
    CpuAndGpu
};
enum class PixelFormat
{
    Rgba8Unorm,
    Rgba16Float,
    R32Float,
    R32Uint
};
struct OutputDesc
{
    Channel channel = Channel::Color;
    Delivery delivery = Delivery::Cpu;
};
struct OutputInfo
{
    OutputDesc requested;
    PixelFormat format;
    uint32_t width, height, rowBytes;
};
struct OutputData
{
    OutputInfo info;
    std::vector<uint8_t> bytes;
};
enum class ContactShadow
{
    None,
    ScreenSpace,
    RayQuery
};
struct ShadowInfo
{
    bool cascaded = false;
    bool rayQueryRequested = false;
    ContactShadow contact = ContactShadow::None;
    float sunAngularRadius = 0;
    float contactLength = 0;
    uint32_t cascadeTile = 0;
};
class Renderer;
class VulkanInterop;
class FrameResult
{
  public:
    ~FrameResult();
    bool ready() const;
    void wait() const;
    double time() const;
    ShadowInfo shadowInfo() const;
    const std::vector<OutputInfo>& outputs() const;
    OutputData read(Channel) const;

  private:
    friend class Renderer;
    friend class VulkanInterop;
    struct Impl;
    explicit FrameResult(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl;
};
enum class SubmitStatus
{
    Submitted,
    Busy
};
struct Submission
{
    SubmitStatus status = SubmitStatus::Busy;
    std::shared_ptr<FrameResult> frame;
};
}
