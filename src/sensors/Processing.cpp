#include "sim/Processing.hpp"
#include <array>
#include <cstdint>
#include <limits>
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
DerivedPixels DerivedSink::write(std::string_view name) const
{
    for (auto& output : own)
        if (output.desc.name == name)
            return DerivedPixels(output.desc, std::span<uint8_t>(output.bytes));
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
        // 行跨度和总长按 64 位计算：接口里的 rowBytes 是 uint32，若按 32 位相乘，
        // 巨大宽度会回绕成很小的数，分配出的缓冲比声明的尺寸小，阶段就会越界写。
        const uint64_t row = uint64_t(desc.width) * bytesPerPixel(desc.format);
        if (row > UINT32_MAX)
            throw std::invalid_argument("Derived output stride overflows 32 bits: " + desc.name);
        const uint64_t total = row * uint64_t(desc.height);
        if (total > std::numeric_limits<size_t>::max())
            throw std::invalid_argument("Derived output extent overflows size_t: " + desc.name);
        DerivedOutput output;
        output.desc = desc;
        output.rowBytes = uint32_t(row);
        output.bytes.assign(size_t(total), 0);
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
    // 返回前复核：描述、行跨度与数据长度必须仍是声明的那一份。句柄类型已经让阶段改不到
    // 描述，这一步守的是分配与切片本身（例如"按阶段切片时算错偏移"这类内部错误）。
    for (size_t i = 0; i < declared.size(); ++i)
    {
        const auto& output = result.derived[i];
        const uint64_t row = uint64_t(output.desc.width) * bytesPerPixel(output.desc.format);
        if (output.desc.name != declared[i].name || output.desc.format != declared[i].format ||
            output.desc.width != declared[i].width || output.desc.height != declared[i].height)
            throw std::logic_error("Derived output no longer matches its declaration: " + declared[i].name);
        if (output.rowBytes != row || uint64_t(output.bytes.size()) != row * uint64_t(output.desc.height))
            throw std::logic_error("Derived output layout does not match its extent: " + declared[i].name);
    }
    return result;
}
}
