// 帧图接入验证：按请求裁剪出的 pass 清单、输出通道与既有语义。
#include "sim/Renderer.hpp"
#include "sim/VulkanInterop.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <array>
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
    device.forceRasterShadows = true;
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

    // 解析输出以本帧写回的历史槽为导出根：时序解析及其生产者保留，颜色变换没有消费者。
    auto resolved = renderer.submit({snapshot, camera, *view, {{Channel::Resolved, Delivery::Cpu}}});
    expect(resolved.status == SubmitStatus::Submitted, "Resolved delivery was rejected");
    expectPasses(*view, {"shadow", "geometry", "lighting", "temporal"}, "Resolved delivery");
    resolved.frame.reset();

    // 调试图引入独立的呈现 pass；它读稳定 G-buffer，因此稳定几何副本也进入清单。
    RenderOptions debug;
    debug.debugChannel = 2;
    auto debugged = renderer.submit({snapshot, camera, *view,
                                     {{Channel::Color, Delivery::Cpu}, {Channel::Debug, Delivery::Cpu}}, debug});
    expect(debugged.status == SubmitStatus::Submitted, "Debug delivery was rejected");
    expectPasses(*view, {"history.clear", "shadow", "geometry", "lighting", "temporal", "geometry.stable", "color",
                         "presentation"},
                 "Debug request after a channel change");
    debugged.frame.reset();

    // 调试图不改变历史有效性：同一调试通道的下一帧没有清除，也没有多出别的 pass。
    auto repeated = renderer.submit({snapshot, camera, *view,
                                     {{Channel::Color, Delivery::Cpu}, {Channel::Debug, Delivery::Cpu}}, debug});
    expectPasses(*view, {"shadow", "geometry", "lighting", "temporal", "geometry.stable", "color", "presentation"},
                 "Repeated debug request");
    repeated.frame.reset();

    debug.contactShadowLength = 0;
    auto changed = renderer.submit({snapshot, camera, *view, {{Channel::Color, Delivery::Cpu}}, debug});
    expect(changed.status == SubmitStatus::Submitted, "Shadow option request was rejected");
    expect(view->activePasses().front() == "history.clear", "Shadow option change reused color history");
    changed.frame.reset();

    // 用地面和悬空遮挡物分别验证 PCSS 和短距离屏幕空间阴影确实改变光照。
    Scene shadowScene;
    shadowScene.sun.direction = {-0.7f, -1.f, 0.5f};
    shadowScene.add(BoxGeometry(), material,
                    glm::scale(glm::translate(glm::mat4(1), {0, -.1f, 0}), {8, .2f, 8}));
    auto caster = shadowScene.add(BoxGeometry(), material, glm::translate(glm::mat4(1), {0, .65f, 0}));
    auto shadowSnapshot = shadowScene.snapshot(2);
    auto shadowView = renderer.createView(256, 192, 512);
    PerspectiveCamera shadowCamera;
    shadowCamera.position = {2, 2, 4};
    shadowCamera.farPlane = 20;
    auto captureShadow = [&](float angularRadius, float contactLength, bool rayRequested = false)
    {
        RenderOptions options;
        options.taa = false;
        options.sunAngularRadius = angularRadius;
        options.rayQueryContactShadows = rayRequested;
        options.contactShadowLength = contactLength;
        auto result = renderer.submit({shadowSnapshot, shadowCamera, *shadowView,
                                       {{Channel::Hdr, Delivery::Cpu}, {Channel::MetricDepth, Delivery::Cpu},
                                        {Channel::InstanceId, Delivery::Cpu}}, options});
        expect(result.status == SubmitStatus::Submitted, "Shadow test capture was rejected");
        if (rayRequested)
            expect(result.frame->shadowInfo().rayQueryRequested &&
                       result.frame->shadowInfo().contact == ContactShadow::ScreenSpace,
                   "Disabled Ray Query device did not report screen-space fallback");
        return std::array{result.frame->read(Channel::Hdr).bytes,
                          result.frame->read(Channel::MetricDepth).bytes,
                          result.frame->read(Channel::InstanceId).bytes};
    };
    const auto pcf = captureShadow(0, 0);
    const auto pcss = captureShadow(.06f, 0);
    const auto contact = captureShadow(.06f, .5f);
    expect(pcf[0] != pcss[0], "PCSS did not change the shadowed radiance");
    expect(pcss[0] != contact[0], "Contact shadows did not change the shadowed radiance");
    expect(captureShadow(.06f, .5f, true) == contact, "Ray Query fallback changed raster output");
    expect(pcf[1] == pcss[1] && pcss[1] == contact[1] && pcf[2] == pcss[2] && pcss[2] == contact[2],
           "Shadow options changed geometry truth");
    expect(!renderer.supportsRayQueryContactShadows(), "Forced raster device enabled Ray Query");
    device.forceRasterShadows = false;
    Renderer automatic(device);
    auto autoView = automatic.createView(256, 192, 512);
    auto secondView = automatic.createView(256, 192, 512);
    auto autoCapture = [&](View& view, const SceneSnapshot& source, bool ray, float contactLength = .5f,
                           const PerspectiveCamera* cameraOverride = nullptr)
    {
        RenderOptions options;
        options.taa = false;
        options.rayQueryContactShadows = ray;
        options.contactShadowLength = contactLength;
        auto result = automatic.submit({source, cameraOverride ? *cameraOverride : shadowCamera, view,
                                        {{Channel::Hdr, Delivery::Cpu}, {Channel::MetricDepth, Delivery::Cpu},
                                         {Channel::InstanceId, Delivery::Cpu}}, options});
        expect(result.status == SubmitStatus::Submitted, "Contact shadow capture was rejected");
        return result.frame;
    };
    const auto raster = autoCapture(*autoView, shadowSnapshot, false);
    expect(raster->shadowInfo().contact == ContactShadow::ScreenSpace, "Default contact mode is not screen space");
    expectPasses(*autoView, {"shadow", "geometry", "lighting"}, "Default CSM/PCSS contact");

    // 连续提交两台相机，再读回结果。第二次提交应复用同队列里刚入队的加速结构。
    const auto enhanced = autoCapture(*autoView, shadowSnapshot, true);
    if (automatic.supportsRayQueryContactShadows())
    {
        expectPasses(*autoView, {"shadow.acceleration", "shadow", "geometry", "lighting"}, "Hybrid first build");
        auto shared = autoCapture(*secondView, shadowSnapshot, true);
        expectPasses(*secondView, {"shadow", "geometry", "lighting"}, "Hybrid cross-view reuse");
        expect(enhanced->shadowInfo().contact == ContactShadow::RayQuery &&
                   enhanced->shadowInfo().cascaded && enhanced->shadowInfo().rayQueryRequested,
               "Hybrid shadow metadata did not retain CSM and Ray Query");
        expect(shared->read(Channel::Hdr).bytes == enhanced->read(Channel::Hdr).bytes,
               "Hybrid output differed across views using the same camera");

        shadowScene.get(caster).local[3].x += 1.f;
        auto moved = autoCapture(*secondView, shadowScene.snapshot(3), true);
        expectPasses(*secondView, {"shadow.acceleration", "shadow", "geometry", "lighting"}, "Moved caster rebuild");
        expect(moved->read(Channel::Hdr).bytes != shared->read(Channel::Hdr).bytes, "Moved caster did not update lighting");
        // 凸球没有真实的自身遮挡。短射线从球面背面穿出时，不能把平滑明暗交界切成分面。
        Scene sphere;
        sphere.sun = shadowScene.sun;
        sphere.add(SphereGeometry(1.f, 16, 8), material);
        const auto singleSphere = sphere.snapshot(4);
        auto sphereView = automatic.createView(320, 240, 512);
        auto sphereBase = autoCapture(*sphereView, singleSphere, false, 0);
        auto sphereRay = autoCapture(*sphereView, singleSphere, true, .3f);
        expect(sphereBase->read(Channel::Hdr).bytes == sphereRay->read(Channel::Hdr).bytes,
               "Ray Query darkened the terminator of a convex sphere");
        // 相机只看到受光板。细遮挡条在相机背后，其光线投影穿过板中心。
        Scene offscreen;
        offscreen.sun.direction = {-.35f, 0, -1};
        const auto panel = offscreen.add(BoxGeometry(.5f, .3f, .02f), material,
                                         glm::translate(glm::mat4(1), {0, 0, -.01f}));
        const auto hidden = offscreen.add(BoxGeometry(.018f, .24f, .006f), material,
                                          glm::translate(glm::mat4(1), {.35f * .21f, 0, .21f}));
        auto offscreenSnapshot = offscreen.snapshot(5);
        PerspectiveCamera nearby;
        nearby.position = {0, 0, .15f};
        nearby.nearPlane = .01f;
        nearby.farPlane = 120;
        auto offscreenView = automatic.createView(256, 192, 64);
        const auto screen = autoCapture(*offscreenView, offscreenSnapshot, false, .3f, &nearby);
        const auto world = autoCapture(*offscreenView, offscreenSnapshot, true, .3f, &nearby);
        const auto screenHdr = screen->read(Channel::Hdr);
        const auto worldHdr = world->read(Channel::Hdr);
        const auto labels = world->read(Channel::InstanceId);
        auto label = [&](unsigned x, unsigned y)
        {
            uint32_t value;
            std::memcpy(&value, labels.bytes.data() + y * labels.info.rowBytes + x * sizeof(value), sizeof(value));
            return value;
        };
        auto red = [](const OutputData& image, unsigned x, unsigned y)
        {
            // 非负有限半精度颜色的位表示与亮度单调一致。
            uint16_t value;
            std::memcpy(&value, image.bytes.data() + y * image.info.rowBytes + x * 8, sizeof(value));
            return value;
        };
        for (unsigned y = 0; y < labels.info.height; ++y)
            for (unsigned x = 0; x < labels.info.width; ++x)
                expect(label(x, y) != hidden, "Behind-camera caster leaked into the G-buffer");
        expect(label(128, 96) == panel && label(72, 96) == panel, "Expected visible receiver samples");
        expect(red(worldHdr, 128, 96) + 64 < red(screenHdr, 128, 96),
               "Ray Query did not darken the predicted offscreen contact");
        expect(red(worldHdr, 72, 96) == red(screenHdr, 72, 96),
               "Ray Query changed the receiver outside the projected stripe");
        std::cout << "PASS: CSM and hybrid contact shadows, shared and dynamic TLAS\n";
    }
    else
    {
        expectPasses(*autoView, {"shadow", "geometry", "lighting"}, "Unsupported-Ray Query fallback");
        expect(enhanced->shadowInfo().contact == ContactShadow::ScreenSpace && enhanced->shadowInfo().rayQueryRequested,
               "Fallback metadata lost the requested or actual contact mode");
    }
    expect(raster->read(Channel::MetricDepth).bytes == enhanced->read(Channel::MetricDepth).bytes &&
               raster->read(Channel::InstanceId).bytes == enhanced->read(Channel::InstanceId).bytes,
           "Contact shadow mode changed geometry truth");

    auto noContact = autoCapture(*secondView, shadowSnapshot, true, 0);
    expectPasses(*secondView, {"shadow", "geometry", "lighting"}, "Zero contact reach keeps CSM");
    expect(noContact->shadowInfo().contact == ContactShadow::None && noContact->shadowInfo().cascaded,
           "Zero contact reach disabled CSM");
    std::cout << "PASS: frame graph culls passes to the requested outputs\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
