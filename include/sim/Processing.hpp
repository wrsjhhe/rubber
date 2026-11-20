#pragma once
#include "Observation.hpp"
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 传感器处理链入口：阶段只读观测、只能写自己声明的派生输出，因此"处理"不可能覆盖
// 真值通道。F11—F13 的畸变、曝光/运动模糊、噪声各自作为阶段接入，不改变本协议。
namespace sim
{
struct DerivedDesc
{
    std::string name;
    PixelFormat format = PixelFormat::Rgba8Unorm;
    uint32_t width = 0, height = 0;
};
struct DerivedOutput
{
    DerivedDesc desc;
    uint32_t rowBytes = 0;
    std::vector<uint8_t> bytes;
};
class ProcessingChain;
// 阶段写入句柄：按名字取自己声明的输出。未声明的名字、别人的产物、输入通道都不在这里。
class DerivedSink
{
  public:
    const Observation& input() const
    {
        return *source;
    }
    DerivedOutput& write(std::string_view name);

  private:
    friend class ProcessingChain;
    DerivedSink(const Observation& in, std::span<DerivedOutput> own) : source(&in), own(own)
    {
    }
    const Observation* source;
    std::span<DerivedOutput> own;
};
class SensorStage
{
  public:
    virtual ~SensorStage() = default;
    virtual std::string_view name() const = 0;
    // 声明本阶段新增的输出：名字非空、全链唯一、且不能与任何输入通道同名。
    virtual std::vector<DerivedDesc> outputs(const Observation&) const = 0;
    // 只读输入观测，只写自己声明的输出。
    virtual void process(DerivedSink&) const = 0;
};
struct ProcessedObservation
{
    // 源观测与派生输出一起返回：调用方读派生数据时源结果仍然有效。
    std::shared_ptr<const Observation> source;
    std::vector<DerivedOutput> derived;
    const DerivedOutput* find(std::string_view name) const;
};
class ProcessingChain
{
  public:
    explicit ProcessingChain(std::string name = {});
    void add(std::unique_ptr<SensorStage>);
    size_t size() const
    {
        return stages.size();
    }
    const std::string& name() const
    {
        return chainName;
    }
    // 先校验全链声明，再按加入顺序执行；声明非法时整体拒绝，不留半成品输出。
    ProcessedObservation run(const std::shared_ptr<const Observation>&) const;

  private:
    std::string chainName;
    std::vector<std::unique_ptr<SensorStage>> stages;
};
}
