// 帧图的编译期验证：裁剪、诊断、子资源与缓冲范围、导入导出。不需要设备。
#include "render/graph/FrameGraph.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace sim::render;
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
template <typename F> void expectThrow(F&& body, std::string_view text)
{
    try
    {
        body();
    }
    catch (const std::exception& e)
    {
        if (std::string_view(e.what()).find(text) != std::string_view::npos)
            return;
        throw std::runtime_error(std::string("Unexpected diagnostic: ") + e.what());
    }
    throw std::runtime_error(std::string("Expected a diagnostic containing: ") + std::string(text));
}
ImageDesc color(uint32_t w = 64, uint32_t h = 64, uint32_t mips = 1)
{
    ImageDesc desc;
    desc.format = Format::Rgba8Unorm;
    desc.width = w;
    desc.height = h;
    desc.mipLevels = mips;
    desc.usage = ImageUsage::Sampled | ImageUsage::ColorAttachment;
    return desc;
}
int main()
try
{
    // 反向裁剪：只有导出与副作用是根，没有读者的 pass 不进入执行清单。
    {
        FrameGraph graph;
        auto x = graph.createImage("x", color());
        auto y = graph.createImage("y", color());
        auto z = graph.createImage("z", color());
        auto w = graph.createImage("w", color());
        auto a = graph.reserve("a"), b = graph.reserve("b"), c = graph.reserve("c"), d = graph.reserve("d");
        graph.addPass(a, [&](FrameGraph::Declaration& p) { p.write(x, Access::ColorWrite); }, [](VkCommandBuffer) {});
        graph.addPass(
            b,
            [&](FrameGraph::Declaration& p)
            {
                p.read(x);
                p.write(y, Access::ColorWrite);
            },
            [](VkCommandBuffer) {});
        graph.addPass(
            c,
            [&](FrameGraph::Declaration& p)
            {
                p.read(y);
                p.write(z, Access::ColorWrite);
            },
            [](VkCommandBuffer) {});
        graph.addPass(d, [&](FrameGraph::Declaration& p) { p.write(w, Access::ColorWrite); }, [](VkCommandBuffer) {});
        graph.exportImage(z, Layout::ShaderRead);
        graph.compile();
        expect(graph.activePasses() == std::vector<std::string>{"a", "b", "c"}, "Culling kept a dead pass");
        expect(graph.transients().size() == 3, "Transient list did not follow culling");
        // 裁剪后 x 只需一次 Attachment 写入，y 在 pass c 前转成只读，z 导出转只读。
        expect(graph.barrierCount() >= 3, "Barriers were not planned for active passes");
    }
    // 导入未初始化的资源被读取：编译期诊断，而不是交给验证层。
    {
        FrameGraph graph;
        auto history = graph.importImage("history", color(), Layout::ShaderRead, false);
        auto out = graph.createImage("out", color());
        auto p = graph.reserve("resolve");
        graph.addPass(
            p,
            [&](FrameGraph::Declaration& d)
            {
                d.read(history);
                d.write(out, Access::ColorWrite);
            },
            [](VkCommandBuffer) {});
        graph.exportImage(out, Layout::ShaderRead);
        expectThrow([&] { graph.compile(); }, "reads uninitialized image history");
    }
    // 同一 pass 读写同一资源构成反馈回路。
    {
        FrameGraph graph;
        auto img = graph.createImage("feedback", color());
        auto p = graph.reserve("bad");
        expectThrow(
            [&]
            {
                graph.addPass(
                    p,
                    [&](FrameGraph::Declaration& d)
                    {
                        d.read(img);
                        d.write(img, Access::ColorWrite);
                    },
                    [](VkCommandBuffer) {});
            },
            "reads and writes feedback");
    }
    // 互相声明顺序约束构成循环。
    {
        FrameGraph graph;
        auto a = graph.reserve("a"), b = graph.reserve("b");
        graph.addPass(
            a,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.after(b);
            },
            [](VkCommandBuffer) {});
        graph.addPass(
            b,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.after(a);
            },
            [](VkCommandBuffer) {});
        expectThrow([&] { graph.compile(); }, "cycle");
    }
    // 子资源：只写 mip 1 的 pass 不会给 mip 0 产生屏障，导出时两个 mip 各自收尾。
    {
        FrameGraph graph;
        auto desc = color(64, 64, 2);
        desc.usage = desc.usage | ImageUsage::TransferDst;
        auto mips = graph.createImage("mips", desc);
        auto sampled = graph.createImage("sampled", color());
        auto a = graph.reserve("mip0"), b = graph.reserve("mip1"), c = graph.reserve("sample");
        graph.addPass(a, [&](FrameGraph::Declaration& d) { d.write(mips, Access::CopyDestination, {0, 1, 0, 1}); },
                      [](VkCommandBuffer) {});
        graph.addPass(b, [&](FrameGraph::Declaration& d) { d.write(mips, Access::CopyDestination, {1, 1, 0, 1}); },
                      [](VkCommandBuffer) {});
        graph.addPass(
            c,
            [&](FrameGraph::Declaration& d)
            {
                d.read(mips, Access::Sampled, {0, 1, 0, 1});
                d.write(sampled, Access::ColorWrite);
            },
            [](VkCommandBuffer) {});
        graph.exportImage(sampled, Layout::ShaderRead);
        graph.exportImage(mips, Layout::ShaderRead);
        graph.compile();
        expect(graph.activePasses() == std::vector<std::string>{"mip0", "mip1", "sample"},
               "Subresource consumers were not resolved");
        for (auto& pass : graph.passes())
            for (auto& barrier : pass.barriers)
                if (pass.name == "sample")
                {
                    expect(barrier.image, "Image barrier expected");
                    expect(barrier.sub.mip == 0 && barrier.sub.mipCount == 1, "Sampling mip 0 touched other mips");
                }
        // mip 0 已被采样 pass 转到只读，导出只需补齐 mip 1，不会重复转换。
        unsigned exported = 0;
        for (auto& barrier : graph.finalBarriers())
            if (barrier.resource == mips.index)
            {
                ++exported;
                expect(barrier.to == Layout::ShaderRead, "Export did not reach the requested layout");
                expect(barrier.sub.mip == 1 && barrier.sub.mipCount == 1,
                       "Export transitioned mips that were already in the requested layout");
            }
        expect(exported == 1, "Export did not leave exactly the stale mip to transition");
    }
    // 缓冲依赖按字节范围声明，导出时交给宿主读取。
    {
        FrameGraph graph;
        BufferDesc desc{4096 * 3, BufferUsage::TransferDst | BufferUsage::HostRead};
        auto staging = graph.createBuffer("staging", desc);
        auto shown = color();
        shown.usage = shown.usage | ImageUsage::TransferSrc;
        auto image = graph.importImage("shown", shown, Layout::ShaderRead, true);
        auto p = graph.reserve("readback");
        graph.addPass(
            p,
            [&](FrameGraph::Declaration& d)
            {
                d.read(image, Access::CopySource);
                d.write(staging, Access::CopyDestination, {4096, 4096});
            },
            [](VkCommandBuffer) {});
        graph.exportImage(image, Layout::ShaderRead);
        graph.exportBuffer(staging);
        graph.compile();
        expect(graph.activePasses() == std::vector<std::string>{"readback"}, "Readback pass was culled");
        bool found = false;
        for (auto& barrier : graph.finalBarriers())
            if (!barrier.image)
            {
                found = true;
                expect(barrier.dstAccess == VK_ACCESS_HOST_READ_BIT, "Export did not hand the buffer to the host");
                expect(barrier.range.offset == 4096 && barrier.range.size == 4096,
                       "Export lost the declared buffer range");
            }
        expect(found, "Buffer export produced no barrier");
    }
    // 用途声明与访问不匹配时立即诊断。
    {
        FrameGraph graph;
        auto sampled = color();
        sampled.usage = ImageUsage::Sampled;
        auto sampledOnly = graph.createImage("sampled-only", sampled);
        auto p = graph.reserve("bad-usage");
        expectThrow(
            [&]
            {
                graph.addPass(p, [&](FrameGraph::Declaration& d) { d.write(sampledOnly, Access::ColorWrite); },
                              [](VkCommandBuffer) {});
            },
            "declared usage does not allow");
    }
    // 导出终态：颜色附件写入后必须转到只读，供采样与拷贝消费。
    {
        FrameGraph graph;
        auto display = graph.createImage("display", color());
        auto p = graph.reserve("color");
        graph.addPass(p, [&](FrameGraph::Declaration& d) { d.write(display, Access::ColorWrite); },
                      [](VkCommandBuffer) {});
        graph.exportImage(display, Layout::ShaderRead);
        graph.compile();
        expect(graph.finalBarriers().size() == 1, "Export did not emit exactly one transition");
        auto& barrier = graph.finalBarriers().front();
        expect(barrier.from == Layout::ColorAttachment && barrier.to == Layout::ShaderRead,
               "Export transition did not start from the attachment layout");
        expect(barrier.dstAccess == VK_ACCESS_MEMORY_READ_BIT, "Export consumer access is wrong");
    }
    // 多 layer 的 mip 子范围：只应触及 mip1 的两层，压平成连续 cell 区间会误改无关子资源。
    {
        FrameGraph graph;
        auto desc = color(64, 64, 4);
        desc.layers = 2;
        desc.usage = desc.usage | ImageUsage::TransferDst;
        auto atlas = graph.createImage("atlas", desc);
        auto p = graph.reserve("write-pair");
        graph.addPass(p, [&](FrameGraph::Declaration& d) { d.write(atlas, Access::CopyDestination, {1, 1, 0, 2}); },
                      [](VkCommandBuffer) {});
        graph.exportImage(atlas, Layout::ShaderRead);
        graph.compile();
        std::vector<std::pair<uint32_t, uint32_t>> touched;
        for (auto& barrier : graph.finalBarriers())
        {
            expect(barrier.sub.mipCount == 1 && barrier.sub.layerCount == 1,
                   "Subresource range was not expanded per mip/layer");
            touched.emplace_back(barrier.sub.layer, barrier.sub.mip);
        }
        expect(touched.size() == 2, "Multi-layer mip range touched extra subresources");
        expect(std::find(touched.begin(), touched.end(), std::pair<uint32_t, uint32_t>{0, 1}) != touched.end(),
               "Layer 0 mip 1 was not exported");
        expect(std::find(touched.begin(), touched.end(), std::pair<uint32_t, uint32_t>{1, 1}) != touched.end(),
               "Layer 1 mip 1 was not exported");
    }
    // 同图但读取未声明的 mip：压平实现会把它误标为已初始化。
    {
        FrameGraph graph;
        auto desc = color(64, 64, 4);
        desc.layers = 2;
        desc.usage = desc.usage | ImageUsage::TransferDst;
        auto atlas = graph.createImage("atlas", desc);
        auto write = graph.reserve("write-pair"), read = graph.reserve("read-other-mip");
        graph.addPass(write, [&](FrameGraph::Declaration& d) { d.write(atlas, Access::CopyDestination, {1, 1, 0, 2}); },
                      [](VkCommandBuffer) {});
        graph.addPass(
            read,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.read(atlas, Access::Sampled, {2, 1, 0, 1});
            },
            [](VkCommandBuffer) {});
        expectThrow([&] { graph.compile(); }, "reads uninitialized image atlas");
    }
    // 跨子资源的覆盖写：所有旧写入者都必须在覆写之前完成。
    // 两个生产者各自还有别的产出（被导出），因此不会因"输出被完全覆写"而被裁掉；
    // 覆写句柄按更小的下标预留，一旦漏掉某个生产者，调度器就会把它排到覆写之后。
    {
        FrameGraph graph;
        auto image = graph.createImage("two-mips", color(64, 64, 2));
        auto keepLow = graph.createImage("keep-low", color());
        auto keepHigh = graph.createImage("keep-high", color());
        auto overwrite = graph.reserve("overwrite");
        auto low = graph.reserve("mip0"), high = graph.reserve("mip1");
        graph.addPass(
            low,
            [&](FrameGraph::Declaration& d)
            {
                d.write(image, Access::ColorWrite, {0, 1, 0, 1});
                d.write(keepLow, Access::ColorWrite);
            },
            [](VkCommandBuffer) {});
        graph.addPass(
            high,
            [&](FrameGraph::Declaration& d)
            {
                d.write(image, Access::ColorWrite, {1, 1, 0, 1});
                d.write(keepHigh, Access::ColorWrite);
            },
            [](VkCommandBuffer) {});
        graph.addPass(
            overwrite,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.write(image, Access::ColorWrite, {0, 2, 0, 1});
            },
            [](VkCommandBuffer) {});
        graph.exportImage(keepLow, Layout::ShaderRead);
        graph.exportImage(keepHigh, Layout::ShaderRead);
        graph.exportImage(image, Layout::ShaderRead);
        graph.compile();
        expect(graph.activePasses() == std::vector<std::string>{"mip0", "mip1", "overwrite"},
               "Cross-subresource WAW edge was lost");
    }
    // 跨子资源的读取者：只读了其中一个 mip 的 pass 也必须先于覆盖写完成。
    // 读取者被显式 after 约束拖后，漏掉 WAR 边时覆写会插到它前面。
    {
        FrameGraph graph;
        auto image = graph.importImage("history", color(64, 64, 2), Layout::ShaderRead, true);
        auto unrelated = graph.createImage("unrelated", color());
        auto overwrite = graph.reserve("overwrite"), hold = graph.reserve("hold"), late = graph.reserve("late");
        graph.addPass(
            hold,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.read(image, Access::Sampled, {0, 1, 0, 1});
                d.after(late);
            },
            [](VkCommandBuffer) {});
        graph.addPass(
            late,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.write(unrelated, Access::ColorWrite);
            },
            [](VkCommandBuffer) {});
        graph.addPass(
            overwrite,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.write(image, Access::ColorWrite, {0, 2, 0, 1});
            },
            [](VkCommandBuffer) {});
        graph.compile();
        expect(graph.activePasses() == std::vector<std::string>{"late", "hold", "overwrite"},
               "Cross-subresource WAR edge was lost");
    }
    // 缓冲：完全未写就读，编译期拒绝，而不是把未定义内容交给消费者。
    {
        FrameGraph graph;
        auto scratch = graph.createBuffer("scratch", {256, BufferUsage::Storage});
        auto p = graph.reserve("read-scratch");
        graph.addPass(
            p,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.read(scratch, Access::StorageRead, {0, 256});
            },
            [](VkCommandBuffer) {});
        expectThrow([&] { graph.compile(); }, "reads uninitialized buffer scratch");
    }
    // 缓冲：只写了一半，另一段仍未定义；整体读取必须被拒绝。
    {
        FrameGraph graph;
        auto half = graph.createBuffer("half", {64, BufferUsage::Storage | BufferUsage::TransferDst});
        auto write = graph.reserve("write-half"), read = graph.reserve("read-all");
        graph.addPass(write, [&](FrameGraph::Declaration& d) { d.write(half, Access::CopyDestination, {0, 32}); },
                      [](VkCommandBuffer) {});
        graph.addPass(
            read,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.read(half, Access::StorageRead, {0, 0});
            },
            [](VkCommandBuffer) {});
        expectThrow([&] { graph.compile(); }, "reads uninitialized buffer half");
    }
    // 缓冲：非重叠访问不构成冲突，屏障只覆盖各自声明的区间。
    {
        FrameGraph graph;
        auto shared = graph.importBuffer("shared", {64, BufferUsage::Storage | BufferUsage::TransferDst}, true);
        auto reader = graph.reserve("read-low"), writer = graph.reserve("write-high");
        graph.addPass(
            reader,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.read(shared, Access::StorageRead, {0, 32});
            },
            [](VkCommandBuffer) {});
        graph.addPass(writer, [&](FrameGraph::Declaration& d) { d.write(shared, Access::CopyDestination, {32, 32}); },
                      [](VkCommandBuffer) {});
        graph.exportBuffer(shared);
        graph.compile();
        expect(graph.activePasses() == std::vector<std::string>{"read-low", "write-high"},
               "Non-overlapping buffer accesses produced a false conflict");
        for (auto& pass : graph.passes())
            for (auto& barrier : pass.barriers)
                if (!barrier.image)
                {
                    auto expected = pass.name == "read-low" ? 0u : 32u;
                    expect(barrier.range.offset == expected && barrier.range.size == 32,
                           "Buffer barrier covered bytes the pass never declared");
                }
    }
    // 缓冲：两段各自写入后整体导出，两段的生产者都要保留。
    {
        FrameGraph graph;
        auto split = graph.createBuffer("split", {64, BufferUsage::TransferDst | BufferUsage::HostRead});
        auto low = graph.reserve("write-low"), high = graph.reserve("write-high");
        graph.addPass(low, [&](FrameGraph::Declaration& d) { d.write(split, Access::CopyDestination, {0, 32}); },
                      [](VkCommandBuffer) {});
        graph.addPass(high, [&](FrameGraph::Declaration& d) { d.write(split, Access::CopyDestination, {32, 32}); },
                      [](VkCommandBuffer) {});
        graph.exportBuffer(split);
        graph.compile();
        expect(graph.activePasses() == std::vector<std::string>{"write-low", "write-high"},
               "Buffer export only kept the last range producer");
        std::vector<std::pair<uint64_t, uint64_t>> handed;
        for (auto& barrier : graph.finalBarriers())
            if (!barrier.image)
                handed.emplace_back(barrier.range.offset, barrier.range.size);
        expect(handed.size() == 2, "Buffer export did not cover both written ranges");
    }
    // 缓冲：跨区间覆盖写的 WAW 边要覆盖全部旧写入者。
    // 两个生产者各自另有被导出的产出，所以不会因"结果被完全覆写"而被裁掉；
    // 覆写的下标比其中一个生产者小，漏边时调度器会把它插到生产者之前。
    {
        FrameGraph graph;
        auto buffer = graph.createBuffer("staging", {64, BufferUsage::TransferDst | BufferUsage::HostRead});
        auto keepLow = graph.createBuffer("keep-low", {16, BufferUsage::TransferDst | BufferUsage::HostRead});
        auto keepHigh = graph.createBuffer("keep-high", {16, BufferUsage::TransferDst | BufferUsage::HostRead});
        auto last = graph.reserve("write-high"), overwrite = graph.reserve("overwrite"),
             first = graph.reserve("write-low");
        graph.addPass(
            first,
            [&](FrameGraph::Declaration& d)
            {
                d.write(buffer, Access::CopyDestination, {0, 32});
                d.write(keepLow, Access::CopyDestination, {0, 16});
            },
            [](VkCommandBuffer) {});
        graph.addPass(
            last,
            [&](FrameGraph::Declaration& d)
            {
                d.write(buffer, Access::CopyDestination, {32, 32});
                d.write(keepHigh, Access::CopyDestination, {0, 16});
            },
            [](VkCommandBuffer) {});
        graph.addPass(
            overwrite,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.write(buffer, Access::CopyDestination, {0, 0});
            },
            [](VkCommandBuffer) {});
        graph.exportBuffer(keepLow);
        graph.exportBuffer(keepHigh);
        graph.compile();
        expect(graph.activePasses() == std::vector<std::string>{"write-high", "write-low", "overwrite"},
               "Cross-range buffer WAW edge was lost");
    }
    // 缓冲：先读后写同一区间产生 WAR 边，覆写不能抢在读取之前。
    {
        FrameGraph graph;
        auto buffer = graph.importBuffer("history", {64, BufferUsage::Storage | BufferUsage::TransferDst}, true);
        auto overwrite = graph.reserve("overwrite"), hold = graph.reserve("hold");
        graph.addPass(
            hold,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.read(buffer, Access::StorageRead, {16, 16});
            },
            [](VkCommandBuffer) {});
        graph.addPass(
            overwrite,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.write(buffer, Access::CopyDestination, {0, 0});
            },
            [](VkCommandBuffer) {});
        graph.compile();
        expect(graph.activePasses() == std::vector<std::string>{"hold", "overwrite"},
               "Cross-range buffer WAR edge was lost");
    }
    // 缓冲：局部写 + 读另一半 + 整体覆写。声明期两段最终状态相同会被合并，
    // 执行期若复用合并后的段状态，读取未写的后半段就会被误判为已定义。
    // 写入者带独立副作用（同时写回持久缓冲），因此不会被覆写裁掉，且必定在读取之前执行。
    {
        FrameGraph graph;
        auto scratch = graph.createBuffer(
            "scratch", {64, BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::HostRead});
        auto low = graph.reserve("write-low"), high = graph.reserve("read-high"), cover = graph.reserve("overwrite");
        graph.addPass(
            low,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.write(scratch, Access::CopyDestination, {0, 32});
            },
            [](VkCommandBuffer) {});
        graph.addPass(
            high,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.read(scratch, Access::StorageRead, {32, 32});
            },
            [](VkCommandBuffer) {});
        graph.addPass(cover, [&](FrameGraph::Declaration& d) { d.write(scratch, Access::CopyDestination, {0, 0}); },
                      [](VkCommandBuffer) {});
        graph.exportBuffer(scratch);
        expectThrow([&] { graph.compile(); }, "reads uninitialized buffer scratch");
    }
    // 缓冲：完整写入后分段以不同访问方式读取，再整体覆写。每段的屏障源状态
    // 必须来自该段自己的最后一次访问，而不是被合并段上的某一个状态统一覆盖。
    {
        FrameGraph graph;
        auto usage = BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst |
                     BufferUsage::Uniform | BufferUsage::HostRead;
        auto staging = graph.importBuffer("staging", {64, usage}, false);
        auto fill = graph.reserve("fill"), head = graph.reserve("sample-head"), mid = graph.reserve("copy-mid"),
             tail = graph.reserve("uniform-tail"), cover = graph.reserve("overwrite");
        graph.addPass(fill, [&](FrameGraph::Declaration& d) { d.write(staging, Access::CopyDestination, {0, 0}); },
                      [](VkCommandBuffer) {});
        graph.addPass(
            head,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.read(staging, Access::StorageRead, {0, 16});
            },
            [](VkCommandBuffer) {});
        graph.addPass(
            mid,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.read(staging, Access::CopySource, {16, 16});
            },
            [](VkCommandBuffer) {});
        graph.addPass(
            tail,
            [&](FrameGraph::Declaration& d)
            {
                d.sideEffect();
                d.read(staging, Access::UniformRead, {32, 32});
            },
            [](VkCommandBuffer) {});
        graph.addPass(cover, [&](FrameGraph::Declaration& d) { d.write(staging, Access::CopyDestination, {0, 0}); },
                      [](VkCommandBuffer) {});
        graph.exportBuffer(staging);
        graph.compile();
        const CompiledPass* entry = nullptr;
        for (auto& pass : graph.passes())
            if (pass.name == "overwrite")
                entry = &pass;
        expect(entry != nullptr, "Overwrite pass did not survive culling");
        // 三段各自一个屏障，源访问分别来自存储读、传输读与 uniform 读。
        struct Source
        {
            uint64_t offset = 0, size = 0;
            VkAccessFlags access = 0;
            bool operator==(const Source&) const = default;
        };
        std::vector<Source> sources, expected{{0, 16, VK_ACCESS_SHADER_READ_BIT},
                                              {16, 16, VK_ACCESS_TRANSFER_READ_BIT},
                                              {32, 32, VK_ACCESS_UNIFORM_READ_BIT}};
        for (auto& barrier : entry->barriers)
            sources.push_back({barrier.range.offset, barrier.range.size, barrier.srcAccess});
        expect(sources == expected, "Merged range lost the per-segment source state of the overwrite barriers");
        // 导出收尾按执行期真实边界分段，所有本帧写过的字节都被交给宿主。
        uint64_t covered = 0;
        for (auto& barrier : graph.finalBarriers())
            if (!barrier.image)
                covered += barrier.range.size;
        expect(covered == 64, "Buffer export did not hand over every byte written this frame");
    }
    // 颜色附件的读改写：以 LOAD 进入或做混合时，旧附件值也是这次访问的输入。
    // 只声明 WRITE 会让依赖图有边而屏障没把先前写入变成可见，因此目标访问必须含读与写。
    {
        FrameGraph graph;
        auto hdr = graph.createImage("hdr", color());
        auto a = graph.reserve("lighting"), b = graph.reserve("splat");
        graph.addPass(a, [&](FrameGraph::Declaration& d) { d.write(hdr, Access::ColorWrite); },
                      [](VkCommandBuffer) {});
        graph.addPass(b, [&](FrameGraph::Declaration& d) { d.readWrite(hdr, Access::ColorWrite); },
                      [](VkCommandBuffer) {});
        graph.exportImage(hdr, Layout::ShaderRead);
        graph.compile();
        expect(graph.activePasses() == std::vector<std::string>{"lighting", "splat"},
               "Read-write pass was culled");
        const CompiledPass* write = nullptr;
        const CompiledPass* readWrite = nullptr;
        for (auto& pass : graph.passes())
        {
            if (pass.name == "lighting")
                write = &pass;
            if (pass.name == "splat")
                readWrite = &pass;
        }
        expect(write && readWrite, "Read-write graph lost a pass");
        auto readWriteBarrier = [&]
        {
            for (auto& barrier : readWrite->barriers)
                if (barrier.image && barrier.resource == hdr.index)
                    return &barrier;
            return static_cast<const Barrier*>(nullptr);
        }();
        expect(readWriteBarrier != nullptr, "Read-write of an initialized attachment produced no barrier");
        expect(readWriteBarrier->to == Layout::ColorAttachment, "Read-write changed the attachment layout");
        expect((readWriteBarrier->dstAccess & VK_ACCESS_COLOR_ATTACHMENT_READ_BIT) != 0,
               "Read-write barrier did not make the previous attachment value visible to reads");
        expect((readWriteBarrier->dstAccess & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) != 0,
               "Read-write barrier lost the attachment write access");
        expect(readWriteBarrier->dstStage == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               "Read-write barrier targeted the wrong stage");
        expect((readWriteBarrier->srcAccess & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) != 0,
               "Read-write barrier did not wait for the previous writer");
        // 覆盖写不消费旧值：它的目标访问只应有写，不需要额外等待附件读取。
        for (auto& barrier : write->barriers)
            if (barrier.image && barrier.resource == hdr.index)
                expect((barrier.dstAccess & VK_ACCESS_COLOR_ATTACHMENT_READ_BIT) == 0,
                       "A plain attachment write asked for read access it never uses");
    }
    std::cout << "PASS: frame graph culling, diagnostics, subresources and import/export\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
