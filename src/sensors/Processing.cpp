#include "sim/Processing.hpp"
#include <array>
#include <stdexcept>

namespace sim
{
namespace
{
uint32_t bytesPerPixel(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::Rgba8Unorm:
        return 4;
    case PixelFormat::Rgba16Float:
        return 8;
    case PixelFormat::R32Float:
    case PixelFormat::R32Uint:
        return 4;
    }
    throw std::invalid_argument("Unknown pixel format");
}
const std::array<Channel, 10>& channels()
{
    static const std::array<Channel, 10> all{Channel::Color,      Channel::AlbedoMetal, Channel::NormalRough,
                                             Channel::Motion,     Channel::MetricDepth, Channel::InstanceId,
                                             Channel::HardwareDepth, Channel::Hdr,      Channel::Resolved,
                                             Channel::Debug};
    return all;
}
}
DerivedOutput& DerivedSink::write(std::string_view name)
{
    for (auto& output : own)
        if (output.desc.name == name)
            return output;
    // 输入通道不在这里：写它们的唯一途径是不声明就写，而那正是要拒绝的行为。
    throw std::invalid_argument("Stage wrote an undeclared output: " + std::string(name));
}
const DerivedOutput* ProcessedObservation::find(std::string_view name) const
{
    for (const auto& output : derived)
        if (output.desc.name == name)
            return &output;
    return nullptr;
}
ProcessingChain::ProcessingChain(std::string name) : chainName(std::move(name))
{
}
void ProcessingChain::add(std::unique_ptr<SensorStage> stage)
{
    if (!stage)
        throw std::invalid_argument("Processing chain stage is null");
    stages.push_back(std::move(stage));
}
ProcessedObservation ProcessingChain::run(const std::shared_ptr<const Observation>& source) const
{
    if (!source)
        throw std::invalid_argument("Processing chain requires an observation");
    // 先统一校验全链声明：与输入通道同名意味着改写真值，重名意味着两个阶段抢一块输出。
    // 任一条不合法就整体拒绝，不会留下执行了一半的派生数据。
    std::vector<DerivedDesc> declared;
    std::vector<size_t> counts;
    for (const auto& stage : stages)
    {
        const auto outputs = stage->outputs(*source);
        counts.push_back(outputs.size());
        for (const auto& desc : outputs)
        {
            if (desc.name.empty())
                throw std::invalid_argument("Stage declared an unnamed output");
            if (!desc.width || !desc.height)
                throw std::invalid_argument("Stage declared an empty output: " + desc.name);
            for (auto channel : channels())
                if (desc.name == Observation::semantic(channel).name)
                    throw std::invalid_argument("Derived output cannot shadow input channel: " + desc.name);
            for (const auto& other : declared)
                if (other.name == desc.name)
                    throw std::invalid_argument("Duplicate derived output: " + desc.name);
            declared.push_back(desc);
        }
    }
    ProcessedObservation result;
    result.source = source;
    result.derived.reserve(declared.size());
    for (const auto& desc : declared)
    {
        DerivedOutput output;
        output.desc = desc;
        output.rowBytes = desc.width * bytesPerPixel(desc.format);
        output.bytes.assign(size_t(output.rowBytes) * desc.height, 0);
        result.derived.push_back(std::move(output));
    }
    // 每个阶段只拿到自己声明的那一段：别人的输出与输入通道都不可写。
    size_t offset = 0;
    for (size_t i = 0; i < stages.size(); ++i)
    {
        auto own = std::span<DerivedOutput>(result.derived.data() + offset, counts[i]);
        DerivedSink sink(*source, own);
        stages[i]->process(sink);
        offset += own.size();
    }
    return result;
}
}
