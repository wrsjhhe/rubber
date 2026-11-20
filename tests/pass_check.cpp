// 帧图接入验证：按请求裁剪出的 pass 清单、输出通道与既有语义。
#include "sim/Renderer.hpp"
#include "sim/VulkanInterop.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace sim;
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
std::string joined(const std::vector<std::string>& names)
{
    std::string text;
    for (auto& name : names)
        text += (text.empty() ? "" : ",") + name;
    return text;
}
void expectPasses(const View& view, std::vector<std::string> expected, const char* what)
{
    auto actual = view.activePasses();
    if (actual != expected)
        throw std::runtime_error(std::string(what) + ": expected [" + joined(expected) + "] but ran [" +
                                 joined(actual) + "]");
}
int main(int argc, char** argv)
try
{
    DeviceOptions device;
    device.validation = argc > 1 && std::string_view(argv[1]) == "--validation";
    Renderer renderer(device);
    Scene scene;
    auto material = std::make_shared<MeshStandardMaterial>();
    scene.add(BoxGeometry(), material);
    auto snapshot = scene.snapshot(1);
    PerspectiveCamera camera;
    camera.position = {0, 0, 5};
    auto view = renderer.createView(64, 48, 64);

    // 只请求几何真值时，阴影、光照、时序解析与颜色变换都不进入执行清单。
    auto depthOnly = renderer.submit({snapshot, camera, *view, {{Channel::MetricDepth, Delivery::Cpu}}});
    expect(depthOnly.status == SubmitStatus::Submitted, "Depth-only request was rejected");
    expectPasses(*view, {"geometry"}, "Depth-only request");
    auto depth = depthOnly.frame->read(Channel::MetricDepth);
    auto center = *reinterpret_cast<const float*>(depth.bytes.data() + (depth.info.height / 2) * depth.info.rowBytes +
                                                  (depth.info.width / 2) * 4);
    expect(std::abs(center - 4.5f) < .001f, "Depth-only request lost the mesh");
    depthOnly.frame.reset();

    // 关闭阴影后阴影 pass 不再声明；首帧历史尚未定义，清除作为独立 pass 排在首位。
    RenderOptions noShadow;
    noShadow.shadows = false;
    auto flat = renderer.submit({snapshot, camera, *view, {{Channel::Color, Delivery::Cpu}}, noShadow});
    expectPasses(*view, {"history.clear", "geometry", "lighting", "temporal", "color"},
                 "First request without shadows");
    flat.frame.reset();

    // 光照条件未变，历史仍有效：清除不再重复，只保留跨帧写回的时序解析。
    auto flatAgain = renderer.submit({snapshot, camera, *view, {{Channel::Color, Delivery::Cpu}}, noShadow});
    expectPasses(*view, {"geometry", "lighting", "temporal", "color"}, "Second request without shadows");
    flatAgain.frame.reset();

    // 重新打开阴影改变了光照输入，历史必须作废重建，于是清除 pass 再次出现。
    auto color = renderer.submit({snapshot, camera, *view, {{Channel::Color, Delivery::Cpu}}});
    expectPasses(*view, {"history.clear", "shadow", "geometry", "lighting", "temporal", "color"},
                 "Color request after re-enabling shadows");
    color.frame.reset();

    // 稳定的颜色请求执行四个渲染 pass；稳定几何副本没有消费者，不执行。
    auto warm = renderer.submit({snapshot, camera, *view, {{Channel::Color, Delivery::Cpu}}});
    expectPasses(*view, {"shadow", "geometry", "lighting", "temporal", "color"}, "Warm color request");
    warm.frame.reset();

    // HDR 输出不需要颜色变换，显示映射不会被执行。
    auto hdr = renderer.submit({snapshot, camera, *view, {{Channel::Hdr, Delivery::Cpu}}});
    expectPasses(*view, {"shadow", "geometry", "lighting", "temporal"}, "Hdr request");
    hdr.frame.reset();

    // GPU 交付的通道同样是裁剪根：导出声明保留生产链，不会被误裁。
    auto gpu = renderer.submit({snapshot, camera, *view, {{Channel::MetricDepth, Delivery::Gpu}}});
    expect(gpu.status == SubmitStatus::Submitted, "GPU delivery was rejected");
    expectPasses(*view, {"geometry"}, "GPU depth delivery");
    auto output = VulkanInterop::output(*gpu.frame, Channel::MetricDepth);
    expect(output.image && output.completion, "GPU delivery lost the image or completion signal");
    gpu.frame.reset();

    // 需要 CPU 读回的路径必须包含稳定几何与读回，且读回结果与渲染一致。
    RenderOptions readback;
    readback.readback = true;
    renderer.render(snapshot, camera, *view, readback);
    expectPasses(*view, {"shadow", "geometry", "lighting", "temporal", "geometry.stable", "color", "readback"},
                 "Readback path");
    auto frame = view->read();
    expect(std::abs(frame.depth[24 * 64 + 32] - 4.5f) < .001f, "Readback lost the metric depth");
    expect(frame.ids[24 * 64 + 32] != 0, "Readback lost the instance label");

    std::cout << "PASS: frame graph culls passes to the requested outputs\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
