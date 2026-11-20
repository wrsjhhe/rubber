#include "FrameGraph.hpp"
#include "../../backend/Device.hpp"
#include <algorithm>
#include <stdexcept>

namespace sim::render
{
namespace
{
using sim::vk::Buffer;
using sim::vk::Image;
struct StageAccess
{
    VkPipelineStageFlags stage = 0;
    VkAccessFlags access = 0;
};
constexpr VkPipelineStageFlags shaderStages =
    VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
StageAccess stageAccess(Access access)
{
    switch (access)
    {
    case Access::Sampled:
        return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT};
    case Access::UniformRead:
        return {shaderStages, VK_ACCESS_UNIFORM_READ_BIT};
    case Access::StorageRead:
        return {shaderStages, VK_ACCESS_SHADER_READ_BIT};
    case Access::ColorWrite:
        return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case Access::DepthWrite:
        return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT};
    case Access::CopySource:
        return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    case Access::CopyDestination:
        return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    case Access::HostRead:
        return {VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT};
    }
    return {};
}
Layout layoutOf(Access access)
{
    switch (access)
    {
    case Access::Sampled:
    case Access::UniformRead:
    case Access::StorageRead:
        return Layout::ShaderRead;
    case Access::ColorWrite:
        return Layout::ColorAttachment;
    case Access::DepthWrite:
        return Layout::DepthAttachment;
    case Access::CopySource:
        return Layout::TransferSrc;
    case Access::CopyDestination:
        return Layout::TransferDst;
    case Access::HostRead:
        return Layout::Undefined;
    }
    return Layout::Undefined;
}
VkImageLayout vkLayout(Layout layout)
{
    switch (layout)
    {
    case Layout::Undefined:
        return VK_IMAGE_LAYOUT_UNDEFINED;
    case Layout::ColorAttachment:
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case Layout::DepthAttachment:
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case Layout::ShaderRead:
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case Layout::TransferSrc:
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case Layout::TransferDst:
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}
VkImageAspectFlags vkAspect(Aspect aspect)
{
    return aspect == Aspect::Depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}
// 声明访问是否被资源声明的用途允许。
ImageUsage requiredUsage(Access access)
{
    switch (access)
    {
    case Access::Sampled:
        return ImageUsage::Sampled;
    case Access::ColorWrite:
        return ImageUsage::ColorAttachment;
    case Access::DepthWrite:
        return ImageUsage::DepthAttachment;
    case Access::CopySource:
        return ImageUsage::TransferSrc;
    case Access::CopyDestination:
        return ImageUsage::TransferDst;
    case Access::UniformRead:
    case Access::StorageRead:
    case Access::HostRead:
        break;
    }
    throw std::logic_error("Image access is not supported on image resources");
}
BufferUsage requiredUsageBuffer(Access access)
{
    switch (access)
    {
    case Access::UniformRead:
        return BufferUsage::Uniform;
    case Access::StorageRead:
        return BufferUsage::Storage;
    case Access::CopySource:
        return BufferUsage::TransferSrc;
    case Access::CopyDestination:
        return BufferUsage::TransferDst;
    case Access::HostRead:
        return BufferUsage::HostRead;
    case Access::Sampled:
    case Access::ColorWrite:
    case Access::DepthWrite:
        break;
    }
    throw std::logic_error("Access is not supported on buffer resources");
}
std::string subresourceText(const Subresource& s)
{
    return "mip " + std::to_string(s.mip) + "+" + std::to_string(s.mipCount) + " layer " +
           std::to_string(s.layer) + "+" + std::to_string(s.layerCount);
}
}
void FrameGraph::Declaration::read(ImageHandle handle, Access access, Subresource sub)
{
    if (!handle || handle.index >= graph->images.size())
        throw std::logic_error("Frame graph read of an unknown image");
    graph->addUse(*this, true, handle.index, sub, {}, false, access);
}
void FrameGraph::Declaration::write(ImageHandle handle, Access access, Subresource sub)
{
    if (!handle || handle.index >= graph->images.size())
        throw std::logic_error("Frame graph write of an unknown image");
    graph->addUse(*this, true, handle.index, sub, {}, true, access);
}
void FrameGraph::Declaration::read(BufferHandle handle, Access access, BufferRange range)
{
    if (!handle || handle.index >= graph->buffers.size())
        throw std::logic_error("Frame graph read of an unknown buffer");
    graph->addUse(*this, false, handle.index, {}, range, false, access);
}
void FrameGraph::Declaration::write(BufferHandle handle, Access access, BufferRange range)
{
    if (!handle || handle.index >= graph->buffers.size())
        throw std::logic_error("Frame graph write of an unknown buffer");
    graph->addUse(*this, false, handle.index, {}, range, true, access);
}
void FrameGraph::Declaration::after(PassHandle handle)
{
    if (!handle || handle.index >= graph->nodes.size())
        throw std::logic_error("Frame graph ordering against an unknown pass");
    graph->nodes[pass].after.push_back(handle.index);
}
void FrameGraph::Declaration::sideEffect()
{
    graph->nodes[pass].sideEffect = true;
}
std::pair<uint32_t, uint32_t> FrameGraph::cellRange(const ImageResource& image, const Subresource& sub) const
{
    auto& desc = image.desc;
    uint32_t mips = sub.mipCount ? sub.mipCount : desc.mipLevels - std::min(sub.mip, desc.mipLevels);
    uint32_t layers = sub.layerCount ? sub.layerCount : desc.layers - std::min(sub.layer, desc.layers);
    if (!mips || !layers || sub.mip + mips > desc.mipLevels || sub.layer + layers > desc.layers)
        throw std::logic_error("Frame graph subresource out of range on " + image.name + ": " +
                               subresourceText(sub));
    auto first = sub.layer * desc.mipLevels + sub.mip;
    auto last = (sub.layer + layers - 1) * desc.mipLevels + sub.mip + mips - 1;
    return {first, last};
}
FrameGraph::Use& FrameGraph::addUse(Declaration& decl, bool image, uint32_t resource, const Subresource& sub,
                                    const BufferRange& range, bool write, Access access)
{
    auto& node = nodes[decl.pass];
    Use use;
    use.image = image;
    use.resource = resource;
    use.sub = sub;
    use.range = range;
    use.write = write;
    use.access = access;
    if (image)
    {
        auto& res = images[resource];
        auto [first, last] = cellRange(res, sub);
        for (auto i = first; i <= last; ++i)
        {
            auto& cell = res.cells[i];
            use.writers.push_back(cell.writer);
            if (write)
            {
                // 同一 pass 内的读后写会形成反馈回路，属于冲突声明。
                if (std::find(cell.readers.begin(), cell.readers.end(), decl.pass) != cell.readers.end())
                    throw std::logic_error("Frame graph pass '" + node.name + "' reads and writes " + res.name +
                                           " in the same pass");
                use.readers = cell.readers;
                cell.writer = int32_t(decl.pass);
                cell.readers.clear();
                cell.defined = true;
            }
            else
            {
                cell.readers.push_back(decl.pass);
            }
        }
    }
    else
    {
        auto& res = buffers[resource];
        auto size = res.desc.size;
        auto start = range.offset, end = range.size ? range.offset + range.size : size;
        if (end > size || start > end)
            throw std::logic_error("Frame graph buffer range out of bounds on " + res.name);
        use.writers.push_back(res.state.writer);
        if (write)
        {
            if (std::find(res.state.readers.begin(), res.state.readers.end(), decl.pass) !=
                res.state.readers.end())
                throw std::logic_error("Frame graph pass '" + node.name + "' reads and writes " + res.name +
                                       " in the same pass");
            use.readers = res.state.readers;
            res.state.writer = int32_t(decl.pass);
            res.state.readers.clear();
            res.state.defined = true;
            if (!res.written.size)
                res.written = {start, end - start};
            else
            {
                auto begin = std::min(res.written.offset, start);
                auto finish = std::max(res.written.offset + res.written.size, end);
                res.written = {begin, finish - begin};
            }
        }
        else
            res.state.readers.push_back(decl.pass);
    }
    validate(node, use);
    node.uses.push_back(std::move(use));
    return node.uses.back();
}
void FrameGraph::validate(const PassNode& node, const Use& use) const
{
    if (use.write && (use.access == Access::Sampled || use.access == Access::UniformRead ||
                      use.access == Access::StorageRead || use.access == Access::CopySource ||
                      use.access == Access::HostRead))
        throw std::logic_error("Frame graph pass '" + node.name + "' declares a write with a read-only access");
    if (use.image)
    {
        auto& res = images[use.resource];
        if (!has(res.desc.usage, requiredUsage(use.access)))
            throw std::logic_error("Frame graph pass '" + node.name + "' uses " + res.name +
                                   " with an access its declared usage does not allow");
        if (use.access == Access::DepthWrite && res.desc.aspect != Aspect::Depth)
            throw std::logic_error("Frame graph pass '" + node.name + "' writes depth to " + res.name +
                                   " which is not a depth image");
        if (use.access == Access::ColorWrite && res.desc.aspect != Aspect::Color)
            throw std::logic_error("Frame graph pass '" + node.name + "' writes color to " + res.name +
                                   " which is not a color image");
    }
    else
    {
        auto& res = buffers[use.resource];
        if (!has(res.desc.usage, requiredUsageBuffer(use.access)))
            throw std::logic_error("Frame graph pass '" + node.name + "' uses " + res.name +
                                   " with an access its declared usage does not allow");
    }
}
ImageHandle FrameGraph::importImage(std::string name, const ImageDesc& desc, Layout initial, bool initialized)
{
    ImageResource res;
    res.name = std::move(name);
    res.desc = desc;
    res.imported = true;
    res.initialized = initialized;
    res.importLayout = initial;
    res.cells.resize(size_t(desc.mipLevels) * desc.layers);
    for (auto& cell : res.cells)
    {
        cell.layout = initial;
        cell.stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        cell.access = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        cell.defined = initialized;
        cell.lastWrite = initialized;
    }
    images.push_back(std::move(res));
    return {uint32_t(images.size() - 1)};
}
BufferHandle FrameGraph::importBuffer(std::string name, const BufferDesc& desc, bool initialized)
{
    BufferResource res;
    res.name = std::move(name);
    res.desc = desc;
    res.imported = true;
    res.initialized = initialized;
    res.state.layout = Layout::Undefined;
    res.state.stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    res.state.access = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    res.state.defined = initialized;
    res.state.lastWrite = initialized;
    buffers.push_back(std::move(res));
    return {uint32_t(buffers.size() - 1)};
}
ImageHandle FrameGraph::createImage(std::string name, const ImageDesc& desc)
{
    ImageResource res;
    res.name = std::move(name);
    res.desc = desc;
    res.cells.resize(size_t(desc.mipLevels) * desc.layers);
    images.push_back(std::move(res));
    return {uint32_t(images.size() - 1)};
}
BufferHandle FrameGraph::createBuffer(std::string name, const BufferDesc& desc)
{
    BufferResource res;
    res.name = std::move(name);
    res.desc = desc;
    buffers.push_back(std::move(res));
    return {uint32_t(buffers.size() - 1)};
}
void FrameGraph::exportImage(ImageHandle handle, Layout finalLayout)
{
    if (!handle || handle.index >= images.size())
        throw std::logic_error("Frame graph export of an unknown image");
    images[handle.index].exported = true;
    images[handle.index].exportLayout = finalLayout;
}
void FrameGraph::exportBuffer(BufferHandle handle)
{
    if (!handle || handle.index >= buffers.size())
        throw std::logic_error("Frame graph export of an unknown buffer");
    buffers[handle.index].exported = true;
}
void FrameGraph::restoreImage(ImageHandle handle, Layout finalLayout)
{
    if (!handle || handle.index >= images.size())
        throw std::logic_error("Frame graph restore of an unknown image");
    images[handle.index].restored = true;
    images[handle.index].restoreLayout = finalLayout;
}
PassHandle FrameGraph::reserve(std::string_view name)
{
    PassNode node;
    node.name = std::string(name);
    nodes.push_back(std::move(node));
    passNames.push_back(std::string(name));
    return {uint32_t(nodes.size() - 1)};
}
void FrameGraph::addPass(PassHandle handle, std::function<void(Declaration&)> declare,
                         std::function<void(VkCommandBuffer)> body)
{
    if (!handle || handle.index >= nodes.size())
        throw std::logic_error("Frame graph pass was not reserved");
    auto& node = nodes[handle.index];
    if (node.added)
        throw std::logic_error("Frame graph pass '" + node.name + "' added twice");
    if (!body)
        throw std::logic_error("Frame graph pass '" + node.name + "' has no body");
    node.added = true;
    node.declare = std::move(declare);
    node.body = std::move(body);
    if (node.declare)
    {
        Declaration decl{this, handle.index};
        node.declare(decl);
    }
}
void FrameGraph::bind(ImageHandle handle, Image& image)
{
    if (!handle || handle.index >= images.size())
        throw std::logic_error("Frame graph bind of an unknown image");
    images[handle.index].physical = &image;
}
void FrameGraph::bind(BufferHandle handle, Buffer& buffer)
{
    if (!handle || handle.index >= buffers.size())
        throw std::logic_error("Frame graph bind of an unknown buffer");
    buffers[handle.index].physical = &buffer;
}
std::vector<bool> FrameGraph::cull()
{
    std::vector<bool> live(nodes.size(), false);
    std::vector<uint32_t> stack;
    auto activate = [&](int32_t pass)
    {
        if (pass < 0 || uint32_t(pass) >= nodes.size() || !nodes[size_t(pass)].added)
            return;
        auto i = uint32_t(pass);
        if (live[i])
            return;
        live[i] = true;
        stack.push_back(i);
    };
    // 根：导出的资源由导出点决定需要的生产者；GPU 交付的输出没有被误裁的可能。
    for (auto& image : images)
        if (image.exported)
            for (auto& cell : image.cells)
                activate(cell.writer);
    for (auto& buffer : buffers)
        if (buffer.exported)
            activate(buffer.state.writer);
    for (uint32_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].added && nodes[i].sideEffect)
            activate(int32_t(i));
    while (!stack.empty())
    {
        auto pass = stack.back();
        stack.pop_back();
        for (auto& use : nodes[pass].uses)
            if (!use.write)
                for (auto writer : use.writers)
                    activate(writer);
    }
    return live;
}
std::vector<uint32_t> FrameGraph::order(const std::vector<bool>& live)
{
    std::vector<uint32_t> seq, pending(nodes.size(), 0);
    std::vector<std::vector<uint32_t>> edges(nodes.size());
    for (uint32_t i = 0; i < nodes.size(); ++i)
    {
        if (!live[i])
            continue;
        for (auto pred : nodes[i].after)
            if (pred < nodes.size() && live[pred])
            {
                edges[pred].push_back(i);
                ++pending[i];
            }
        // 资源依赖：写入者先于读取者，前一写入者与读取者也约束本 pass。
        for (auto& use : nodes[i].uses)
        {
            if (use.write)
            {
                // WAW：前一个写入者必须先完成。
                if (use.writers.front() >= 0 && live[size_t(use.writers.front())])
                {
                    edges[size_t(use.writers.front())].push_back(i);
                    ++pending[i];
                }
                // WAR：本次写入之前已经读取该资源的 pass 必须先完成。
                for (auto reader : use.readers)
                    if (reader < nodes.size() && live[reader] && reader != i)
                    {
                        edges[reader].push_back(i);
                        ++pending[i];
                    }
            }
            else
                for (auto writer : use.writers)
                    if (writer >= 0 && live[size_t(writer)])
                    {
                        edges[size_t(writer)].push_back(i);
                        ++pending[i];
                    }
        }
    }
    std::vector<bool> done(nodes.size(), false);
    for (size_t round = 0; round < nodes.size(); ++round)
    {
        auto pick = UINT32_MAX;
        for (uint32_t i = 0; i < nodes.size(); ++i)
            if (live[i] && !done[i] && !pending[i])
            {
                pick = i;
                break;
            }
        if (pick == UINT32_MAX)
            break;
        done[pick] = true;
        seq.push_back(pick);
        for (auto next : edges[pick])
            --pending[next];
    }
    auto scheduled = std::count(live.begin(), live.end(), true);
    if (seq.size() != size_t(scheduled))
    {
        std::string cycle;
        for (uint32_t i = 0; i < nodes.size(); ++i)
            if (live[i] && !done[i])
                cycle += (cycle.empty() ? "" : " -> ") + nodes[i].name;
        throw std::logic_error("Frame graph has a cycle: " + cycle);
    }
    return seq;
}
void FrameGraph::plan(const std::vector<uint32_t>& seq)
{
    // 执行期状态从导入声明重新推导：被裁掉的 pass 不参与状态链。
    for (auto& image : images)
        for (auto& cell : image.cells)
        {
            cell.layout = image.initialized ? image.importLayout : Layout::Undefined;
            cell.stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            cell.access = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
            cell.defined = image.initialized;
            cell.lastWrite = image.initialized;
        }
    for (auto& buffer : buffers)
    {
        buffer.state.layout = Layout::Undefined;
        buffer.state.stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        buffer.state.access = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        buffer.state.defined = buffer.initialized;
        buffer.state.lastWrite = buffer.initialized;
    }
    auto barrierFor = [](bool image, uint32_t resource, const Subresource& sub, const BufferRange& range,
                         const Cell& from, Layout target, StageAccess to)
    {
        Barrier barrier;
        barrier.image = image;
        barrier.resource = resource;
        barrier.sub = sub;
        barrier.range = range;
        barrier.from = from.layout;
        barrier.to = target;
        barrier.srcStage = from.stage;
        barrier.srcAccess = from.access;
        barrier.dstStage = to.stage;
        barrier.dstAccess = to.access;
        return barrier;
    };
    for (auto pass : seq)
    {
        CompiledPass entry;
        entry.name = nodes[pass].name;
        entry.pass = pass;
        for (auto& use : nodes[pass].uses)
        {
            auto target = layoutOf(use.access);
            auto stage = stageAccess(use.access);
            if (use.image)
            {
                auto& image = images[use.resource];
                auto [first, last] = cellRange(image, use.sub);
                for (auto i = first; i <= last; ++i)
                {
                    auto& cell = image.cells[i];
                    // 未初始化读取只在活跃 pass 上诊断，避免被裁掉的声明报错。
                    if (!use.write && !cell.defined)
                        throw std::logic_error("Frame graph pass '" + nodes[pass].name +
                                               "' reads uninitialized image " + image.name);
                    auto hazard = use.write ? (cell.defined || cell.lastWrite) : (cell.lastWrite);
                    if (cell.layout != target || hazard)
                    {
                        Subresource single{i % image.desc.mipLevels, 1, i / image.desc.mipLevels, 1};
                        entry.barriers.push_back(barrierFor(true, use.resource, single, {}, cell, target, stage));
                    }
                    cell.layout = target;
                    cell.stage = stage.stage;
                    cell.access = stage.access;
                    cell.lastWrite = use.write;
                    cell.defined = cell.defined || use.write;
                }
            }
            else
            {
                auto& buffer = buffers[use.resource];
                auto hazard = use.write ? (buffer.state.defined || buffer.state.lastWrite)
                                        : buffer.state.lastWrite;
                if (hazard)
                    entry.barriers.push_back(barrierFor(false, use.resource, {}, use.range, buffer.state,
                                                        Layout::Undefined, stage));
                buffer.state.stage = stage.stage;
                buffer.state.access = stage.access;
                buffer.state.lastWrite = use.write;
            }
        }
        compiled.push_back(std::move(entry));
    }
    // 导出：最终状态写在帧尾，资源仍归库所有，只把转移后的状态交给消费者。
    // 使用方可能是后续提交的采样或拷贝，因此对图像用 ALL_COMMANDS/MEMORY_READ 收尾。
    StageAccess consumer{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT};
    auto transition = [&](ImageResource& image, Layout target)
    {
        auto resource = uint32_t(&image - images.data());
        for (uint32_t i = 0; i < image.cells.size(); ++i)
        {
            auto& cell = image.cells[i];
            if (cell.layout == target && !cell.lastWrite)
                continue;
            // 每个子资源单独收尾，已处于目标布局的 mip/layer 不会被重复转换。
            Subresource sub{i % image.desc.mipLevels, 1, i / image.desc.mipLevels, 1};
            final.push_back(barrierFor(true, resource, sub, {}, cell, target, consumer));
            cell.layout = target;
            cell.lastWrite = false;
        }
    };
    for (auto& image : images)
        if (image.exported)
            transition(image, image.exportLayout);
    // 恢复只作用于未被导出的持久资源，保证下一帧导入声明与实际状态一致。
    for (auto& image : images)
        if (image.restored && !image.exported)
            transition(image, image.restoreLayout);
    for (auto& buffer : buffers)
    {
        if (!buffer.exported)
            continue;
        auto stage = stageAccess(Access::HostRead);
        if (buffer.state.lastWrite)
            final.push_back(barrierFor(false, uint32_t(&buffer - buffers.data()), {}, buffer.written, buffer.state,
                                       Layout::Undefined, stage));
    }
}
void FrameGraph::record(VkCommandBuffer cmd, const Barrier& barrier)
{
    auto subresourceRange = [](VkImageAspectFlags aspect, const Subresource& sub)
    {
        VkImageSubresourceRange range{aspect, sub.mip, sub.mipCount, sub.layer, sub.layerCount};
        return range;
    };
    if (barrier.image)
    {
        auto& image = images[barrier.resource];
        auto& physical = *image.physical;
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = barrier.srcAccess;
        b.dstAccessMask = barrier.dstAccess;
        b.oldLayout = vkLayout(barrier.from);
        b.newLayout = vkLayout(barrier.to);
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = physical.handle;
        b.subresourceRange = subresourceRange(vkAspect(image.desc.aspect), barrier.sub);
        vkCmdPipelineBarrier(cmd, barrier.srcStage, barrier.dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
        physical.layout = b.newLayout;
    }
    else
    {
        auto& buffer = buffers[barrier.resource];
        auto& physical = *buffer.physical;
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcAccessMask = barrier.srcAccess;
        b.dstAccessMask = barrier.dstAccess;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = physical.handle;
        b.offset = barrier.range.offset;
        b.size = barrier.range.size ? barrier.range.size : buffer.desc.size - barrier.range.offset;
        vkCmdPipelineBarrier(cmd, barrier.srcStage, barrier.dstStage, 0, 0, nullptr, 1, &b, 0, nullptr);
    }
}
void FrameGraph::compile()
{
    compiled.clear();
    final.clear();
    active.clear();
    transient.clear();
    // 声明期状态只用于裁剪与排序，plan 会按导入声明重新推导执行期状态；
    // 因此 compile 每帧只调用一次，重建前先 reset。
    auto live = cull();
    auto seq = order(live);
    plan(seq);
    for (auto pass : seq)
        active.push_back(nodes[pass].name);
    for (uint32_t i = 0; i < images.size(); ++i)
    {
        auto& image = images[i];
        if (image.imported)
            continue;
        bool used = false;
        for (auto pass : seq)
            for (auto& use : nodes[pass].uses)
                used = used || (use.image && use.resource == i);
        if (used)
            transient.push_back({true, i, image.desc, {}});
    }
    for (uint32_t i = 0; i < buffers.size(); ++i)
    {
        auto& buffer = buffers[i];
        if (buffer.imported)
            continue;
        bool used = false;
        for (auto pass : seq)
            for (auto& use : nodes[pass].uses)
                used = used || (!use.image && use.resource == i);
        if (used)
            transient.push_back({false, i, {}, buffer.desc});
    }
}
void FrameGraph::execute(VkCommandBuffer cmd)
{
    for (auto& pass : compiled)
    {
        for (auto& barrier : pass.barriers)
        {
            if (barrier.image && !images[barrier.resource].physical)
                throw std::logic_error("Frame graph image was never bound: " + images[barrier.resource].name);
            if (!barrier.image && !buffers[barrier.resource].physical)
                throw std::logic_error("Frame graph buffer was never bound: " + buffers[barrier.resource].name);
            record(cmd, barrier);
        }
        nodes[pass.pass].body(cmd);
    }
    for (auto& barrier : final)
        record(cmd, barrier);
}
void FrameGraph::reset()
{
    nodes.clear();
    passNames.clear();
    images.clear();
    buffers.clear();
    active.clear();
    compiled.clear();
    final.clear();
    transient.clear();
}
uint32_t FrameGraph::barrierCount() const
{
    uint32_t count = uint32_t(final.size());
    for (auto& pass : compiled)
        count += uint32_t(pass.barriers.size());
    return count;
}
}
