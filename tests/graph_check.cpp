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
    std::cout << "PASS: frame graph culling, diagnostics, subresources and import/export\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
