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
// 读改写的访问掩码必须同时含读与写：颜色附件以 LOAD 进入、混合需要旧值，
// 只声明 WRITE 会让依赖图有边而屏障实际没把先前写入对附件读取变得可见。
StageAccess stageAccess(Access access, bool readsBack = false)
{
    StageAccess result;
    switch (access)
    {
    case Access::Sampled:
        result = {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_SHADER_READ_BIT};
        break;
    case Access::UniformRead:
        result = {shaderStages, VK_ACCESS_UNIFORM_READ_BIT};
        break;
    case Access::StorageRead:
        result = {shaderStages, VK_ACCESS_SHADER_READ_BIT};
        break;
    case Access::ColorWrite:
        result = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        break;
    case Access::DepthWrite:
        result = {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT};
        break;
    case Access::CopySource:
        result = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        break;
    case Access::CopyDestination:
        result = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        break;
    case Access::HostRead:
        result = {VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT};
        break;
    }
    // 深度读改写已隐含读；颜色附件只有写，需要显式补上读。传输目标不做读改写。
    if (readsBack && access == Access::ColorWrite)
        result.access |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    return result;
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
// 依赖快照按"相交子资源的全集"记录：同一个 pass 跨多个 mip/layer 或字节区间时去重合并，
// 不能只保留第一个写入者或最后一个 cell 的读者。
void appendUnique(std::vector<int32_t>& list, int32_t value)
{
    if (value < 0)
        return;
    if (std::find(list.begin(), list.end(), value) == list.end())
        list.push_back(value);
}
void appendUnique(std::vector<uint32_t>& list, uint32_t value)
{
    if (std::find(list.begin(), list.end(), value) == list.end())
        list.push_back(value);
}
void appendUnique(std::vector<int32_t>& list, const std::vector<uint32_t>& values)
{
    for (auto value : values)
        appendUnique(list, int32_t(value));
}
void appendUnique(std::vector<uint32_t>& list, const std::vector<uint32_t>& values)
{
    for (auto value : values)
        appendUnique(list, value);
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
void FrameGraph::Declaration::readWrite(ImageHandle handle, Access access, Subresource sub)
{
    if (!handle || handle.index >= graph->images.size())
        throw std::logic_error("Frame graph read-write of an unknown image");
    graph->addUse(*this, true, handle.index, sub, {}, true, access, true);
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
std::vector<uint32_t> FrameGraph::cellsOf(const ImageResource& image, const Subresource& sub) const
{
    auto& desc = image.desc;
    uint32_t mips = sub.mipCount ? sub.mipCount : desc.mipLevels - std::min(sub.mip, desc.mipLevels);
    uint32_t layers = sub.layerCount ? sub.layerCount : desc.layers - std::min(sub.layer, desc.layers);
    if (!mips || !layers || sub.mip + mips > desc.mipLevels || sub.layer + layers > desc.layers)
        throw std::logic_error("Frame graph subresource out of range on " + image.name + ": " +
                               subresourceText(sub));
    // layer × mip 双重展开：多 layer 的子范围不是一段连续的 cell 区间，压平会把无关子资源算进来。
    std::vector<uint32_t> cells;
    cells.reserve(size_t(mips) * layers);
    for (uint32_t layer = sub.layer; layer < sub.layer + layers; ++layer)
        for (uint32_t mip = sub.mip; mip < sub.mip + mips; ++mip)
            cells.push_back(layer * desc.mipLevels + mip);
    return cells;
}
FrameGraph::Segment* FrameGraph::findSegment(BufferResource& buffer, uint64_t begin, uint64_t end)
{
    for (auto& segment : buffer.segments)
        if (segment.begin == begin && segment.end == end)
            return &segment;
    return nullptr;
}
std::vector<std::pair<uint64_t, uint64_t>> FrameGraph::segmentsOf(BufferResource& buffer, uint64_t begin,
                                                                  uint64_t end)
{
    if (begin >= end)
        return {};
    // 未声明过的字节按导入声明推导：没有写入者，是否已定义取决于导入时是否初始化。
    auto untouched = [&buffer]
    {
        Cell state;
        state.layout = Layout::Undefined;
        state.stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        state.access = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        state.writer = -1;
        state.defined = buffer.initialized;
        state.lastWrite = buffer.initialized;
        return state;
    };
    auto sort = [&buffer]
    { std::sort(buffer.segments.begin(), buffer.segments.end(), [](auto& a, auto& b) { return a.begin < b.begin; }); };
    // 先让段边界与本次访问对齐，之后的依赖与屏障都以段为单位。
    auto splitAt = [&buffer](uint64_t point)
    {
        for (size_t i = 0; i < buffer.segments.size(); ++i)
        {
            auto segment = buffer.segments[i];
            if (segment.begin < point && point < segment.end)
            {
                buffer.segments[i].end = point;
                segment.begin = point;
                buffer.segments.push_back(segment);
            }
        }
    };
    splitAt(begin);
    splitAt(end);
    sort();
    // 补上区间内还没有段覆盖的空洞。
    std::vector<Segment> filled;
    auto cursor = begin;
    for (auto& segment : buffer.segments)
    {
        if (segment.end <= begin || segment.begin >= end)
            continue;
        if (segment.begin > cursor)
            filled.push_back({cursor, segment.begin, untouched()});
        cursor = std::max(cursor, segment.end);
    }
    if (cursor < end)
        filled.push_back({cursor, end, untouched()});
    if (!filled.empty())
    {
        buffer.segments.insert(buffer.segments.end(), filled.begin(), filled.end());
        sort();
    }
    std::vector<std::pair<uint64_t, uint64_t>> pieces;
    for (auto& segment : buffer.segments)
        if (segment.begin >= begin && segment.end <= end)
            pieces.push_back({segment.begin, segment.end});
    return pieces;
}
void FrameGraph::mergeSegments(BufferResource& buffer)
{
    std::sort(buffer.segments.begin(), buffer.segments.end(),
              [](const Segment& a, const Segment& b) { return a.begin < b.begin; });
    auto same = [](const Cell& a, const Cell& b)
    {
        return a.layout == b.layout && a.stage == b.stage && a.access == b.access && a.writer == b.writer &&
               a.defined == b.defined && a.lastWrite == b.lastWrite && a.readers == b.readers;
    };
    std::vector<Segment> merged;
    for (auto& segment : buffer.segments)
    {
        if (!merged.empty() && merged.back().end == segment.begin && same(merged.back().state, segment.state))
            merged.back().end = segment.end;
        else
            merged.push_back(segment);
    }
    buffer.segments = std::move(merged);
}
FrameGraph::Use& FrameGraph::addUse(Declaration& decl, bool image, uint32_t resource, const Subresource& sub,
                                    const BufferRange& range, bool write, Access access, bool readsBack)
{
    auto& node = nodes[decl.pass];
    Use use;
    use.image = image;
    use.resource = resource;
    use.sub = sub;
    use.range = range;
    use.write = write;
    use.readsBack = readsBack;
    use.access = access;
    if (image)
    {
        auto& res = images[resource];
        use.cells = cellsOf(res, sub);
        for (auto index : use.cells)
        {
            auto& cell = res.cells[index];
            if (write)
            {
                // 同一 pass 内的读后写会形成反馈回路，属于冲突声明。
                if (std::find(cell.readers.begin(), cell.readers.end(), decl.pass) != cell.readers.end())
                    throw std::logic_error("Frame graph pass '" + node.name + "' reads and writes " + res.name +
                                           " in the same pass");
                // 覆盖写要等所有这些读取者与上一个写入者完成：WAR 与 WAW 都按全部相交子资源建立。
                appendUnique(use.writers, cell.writer);
                appendUnique(use.readers, cell.readers);
                cell.writer = int32_t(decl.pass);
                cell.readers.clear();
                cell.defined = true;
            }
            else
            {
                appendUnique(use.writers, cell.writer);
                appendUnique(cell.readers, decl.pass);
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
        // 缓冲按区间切段：只跟踪被声明过的字节，空洞不会因为"取最小到最大的并集"而被算作已写。
        use.pieces = segmentsOf(res, start, end);
        for (auto& piece : use.pieces)
        {
            auto* segment = findSegment(res, piece.first, piece.second);
            if (!segment)
                throw std::logic_error("Frame graph lost the tracked range of " + res.name);
            if (write)
            {
                if (std::find(segment->state.readers.begin(), segment->state.readers.end(), decl.pass) !=
                    segment->state.readers.end())
                    throw std::logic_error("Frame graph pass '" + node.name + "' reads and writes " + res.name +
                                           " in the same pass");
                appendUnique(use.writers, segment->state.writer);
                appendUnique(use.readers, segment->state.readers);
                segment->state.writer = int32_t(decl.pass);
                segment->state.readers.clear();
                segment->state.defined = true;
            }
            else
            {
                appendUnique(use.writers, segment->state.writer);
                appendUnique(segment->state.readers, decl.pass);
            }
        }
        mergeSegments(res);
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
    // 段按声明范围惰性建立：没被访问过的字节不需要状态。
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
    // 缓冲按段激活：只激活整段导出所必需的区间生产者，任何区间都不能被漏掉。
    for (auto& image : images)
        if (image.exported)
            for (auto& cell : image.cells)
                activate(cell.writer);
    for (auto& buffer : buffers)
        if (buffer.exported)
            for (auto& segment : buffer.segments)
                activate(segment.state.writer);
    for (uint32_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].added && nodes[i].sideEffect)
            activate(int32_t(i));
    while (!stack.empty())
    {
        auto pass = stack.back();
        stack.pop_back();
        for (auto& use : nodes[pass].uses)
            if (use.reads())
                for (auto writer : use.writers)
                    activate(writer);
    }
    return live;
}
std::vector<uint32_t> FrameGraph::order(const std::vector<bool>& live)
{
    std::vector<uint32_t> seq, pending(nodes.size(), 0);
    std::vector<std::vector<uint32_t>> edges(nodes.size());
    auto addEdge = [&](uint32_t from, uint32_t to)
    {
        if (from == to)
            return;
        // 同一对 pass 可能由多个子资源产生同一条依赖，去重后再计数。
        if (std::find(edges[from].begin(), edges[from].end(), to) != edges[from].end())
            return;
        edges[from].push_back(to);
        ++pending[to];
    };
    for (uint32_t i = 0; i < nodes.size(); ++i)
    {
        if (!live[i])
            continue;
        for (auto pred : nodes[i].after)
            if (pred < nodes.size() && live[pred])
                addEdge(pred, i);
        // 资源依赖：写入者先于读取者，前一写入者与读取者也约束本 pass。
        // use.writers/readers 已按全部相交子资源去重合并，跨 mip/layer/字节区间的覆盖写不会漏边。
        for (auto& use : nodes[i].uses)
        {
            for (auto writer : use.writers)
                if (writer >= 0 && uint32_t(writer) < nodes.size() && live[size_t(writer)])
                    addEdge(uint32_t(writer), i);
            // WAR：本次写入之前已经读取该资源的 pass 必须先完成。
            if (use.write)
                for (auto reader : use.readers)
                    if (reader < nodes.size() && live[reader])
                        addEdge(reader, i);
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
            cell.used = false;
        }
    // 缓冲的执行期状态与声明期段表分离：从导入声明独立建立一张覆盖整段缓冲的区间表，
    // 并按所有活跃访问的边界预切分。声明期的段会为了压缩依赖把最终状态相同的相邻区间合并，
    // 若直接在其上重放，一次局部访问就会改写未访问字节的状态（例如把未写的后半段标成已定义）。
    std::vector<std::vector<Segment>> exec(buffers.size());
    for (size_t i = 0; i < buffers.size(); ++i)
    {
        auto& buffer = buffers[i];
        if (!buffer.desc.size)
            continue;
        Segment whole;
        whole.begin = 0;
        whole.end = buffer.desc.size;
        whole.state.layout = Layout::Undefined;
        whole.state.stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        whole.state.access = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        whole.state.defined = buffer.initialized;
        whole.state.lastWrite = buffer.initialized;
        exec[i].push_back(whole);
    }
    auto splitExec = [&exec](uint32_t resource, uint64_t point)
    {
        auto& list = exec[resource];
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].begin < point && point < list[i].end)
            {
                auto tail = list[i];
                tail.begin = point;
                list[i].end = point;
                list.insert(list.begin() + std::ptrdiff_t(i) + 1, tail);
                return;
            }
    };
    for (auto pass : seq)
        for (auto& use : nodes[pass].uses)
            if (!use.image)
                for (auto& piece : use.pieces)
                {
                    splitExec(use.resource, piece.first);
                    splitExec(use.resource, piece.second);
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
            auto stage = stageAccess(use.access, use.readsBack);
            if (use.image)
            {
                auto& image = images[use.resource];
                for (auto i : use.cells)
                {
                    auto& cell = image.cells[i];
                    cell.used = true;
                    // 未初始化读取只在活跃 pass 上诊断，避免被裁掉的声明报错；
                    // 读改写同样消费先前内容，因此也要求已定义。
                    if (use.reads() && !cell.defined)
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
                // 边界已按全部访问预切分，所以每个 piece 都是若干完整段的并集：
                // 状态更新只落在真正被访问的字节上，未访问的字节保持自己的历史状态。
                for (auto& piece : use.pieces)
                    for (auto& segment : exec[use.resource])
                    {
                        if (segment.begin < piece.first || segment.end > piece.second)
                            continue;
                        // 未初始化读取只在活跃 pass 上诊断，避免被裁掉的声明报错。
                        if (!use.write && !segment.state.defined)
                            throw std::logic_error("Frame graph pass '" + nodes[pass].name +
                                                   "' reads uninitialized buffer " + buffer.name);
                        auto hazard = use.write ? (segment.state.defined || segment.state.lastWrite)
                                                : segment.state.lastWrite;
                        if (hazard)
                            entry.barriers.push_back(
                                barrierFor(false, use.resource, {},
                                           {segment.begin, segment.end - segment.begin}, segment.state,
                                           Layout::Undefined, stage));
                        segment.state.stage = stage.stage;
                        segment.state.access = stage.access;
                        segment.state.lastWrite = use.write;
                        segment.state.defined = segment.state.defined || use.write;
                    }
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
            // 本帧从未被访问的子资源保持原状：导出不应把无关 mip/layer 一起转换。
            if (!cell.used)
                continue;
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
    for (size_t i = 0; i < buffers.size(); ++i)
    {
        auto& buffer = buffers[i];
        if (!buffer.exported)
            continue;
        auto stage = stageAccess(Access::HostRead);
        // 只对本次真正写过的区间收尾，未触碰的字节不产生屏障；
        // 范围取执行期区间表的真实边界，不会覆盖中间未写的空洞。
        for (auto& segment : exec[i])
            if (segment.state.lastWrite)
                final.push_back(barrierFor(false, uint32_t(i), {},
                                           {segment.begin, segment.end - segment.begin}, segment.state,
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
