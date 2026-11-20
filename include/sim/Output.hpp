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
    Hdr
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
class Renderer;
class VulkanInterop;
class FrameResult
{
  public:
    ~FrameResult();
    bool ready() const;
    void wait() const;
    double time() const;
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
