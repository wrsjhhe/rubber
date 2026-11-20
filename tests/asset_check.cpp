#include "sim/Renderer.hpp"
#include "sim/VulkanInterop.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace sim;
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
template <typename T> T center(const OutputData& data)
{
    auto index = (data.info.height / 2) * data.info.width + data.info.width / 2;
    T value{};
    std::memcpy(&value, data.bytes.data() + index * sizeof(T), sizeof(T));
    return value;
}
float centerDepth(const FrameResult& result)
{
    return center<float>(result.read(Channel::MetricDepth));
}
struct Rgba
{
    uint8_t r, g, b, a;
};
Rgba centerAlbedo(const FrameResult& result)
{
    return center<Rgba>(result.read(Channel::AlbedoMetal));
}
Rgba centerColor(const FrameResult& result)
{
    return center<Rgba>(result.read(Channel::Color));
}
std::shared_ptr<const Texture> solid(uint8_t r, uint8_t g, uint8_t b)
{
    std::vector<uint8_t> pixels;
    for (int i = 0; i < 4; ++i)
        pixels.insert(pixels.end(), {r, g, b, 255});
    return std::make_shared<Texture>(2, 2, std::move(pixels));
}
int main(int argc, char** argv)
try
{
    DeviceOptions device;
    device.validation = argc > 1 && std::string_view(argv[1]) == "--validation";
    Renderer renderer(device);
    Scene scene;
    auto gray = std::make_shared<MeshStandardMaterial>();
    gray->color = {0.8f, 0.8f, 0.8f};
    auto geometry = BoxGeometry();
    auto id = scene.add(geometry, gray);
    auto snapshot = scene.snapshot(1);
    PerspectiveCamera camera;
    camera.position = {0, 0, 5};
    auto near = renderer.createView(64, 48, 64);
    auto far = renderer.createView(64, 48, 64);

    // 同快照的两台相机共享一次场景准备与一次资产上传。
    auto first = renderer.submit({snapshot, camera, *near, {{Channel::MetricDepth, Delivery::Cpu}}});
    auto rear = camera;
    rear.position.z = 8;
    auto second = renderer.submit({snapshot, rear, *far, {{Channel::MetricDepth, Delivery::Cpu}}});
    auto stats = renderer.assetStats();
    expect(stats.scenePreparations == 1, "Same snapshot prepared more than once");
    expect(stats.sceneReuses == 1, "Second camera did not reuse the shared scene");
    expect(stats.meshUploads == 1, "Geometry uploaded once per camera");
    expect(stats.materialUploads == 1, "Material prepared once per camera");
    expect(stats.textureUploads == 2, "Default textures uploaded more than once");
    expect(stats.uploadBatches == 1, "Scene uploads were not batched");
    expect(std::abs(centerDepth(*first.frame) - 4.5f) < .001f, "Near camera depth from shared scene");
    expect(std::abs(centerDepth(*second.frame) - 7.5f) < .001f, "Far camera depth from shared scene");
    first.frame.reset();
    second.frame.reset();

    // R-M2-001：材质按值进入快照，之后修改用户手中的对象与场景节点都不改变已发布版本。
    gray->color = {0.f, 0.f, 1.f};
    scene.get(id).material.color = {0.f, 1.f, 0.f};
    auto stale = renderer.submit({snapshot, rear, *far, {{Channel::AlbedoMetal, Delivery::Cpu}}});
    expect(renderer.assetStats().scenePreparations == 1, "In-place edit republished an old snapshot");
    auto old = centerAlbedo(*stale.frame);
    expect(old.r == old.g && old.r > 200, "Old snapshot lost its published material");
    stale.frame.reset();

    auto edited = scene.snapshot(1.2);
    auto fresh = renderer.submit({edited, camera, *near, {{Channel::AlbedoMetal, Delivery::Cpu}}});
    expect(renderer.assetStats().scenePreparations == 2, "Edited material did not republish the scene");
    auto green = centerAlbedo(*fresh.frame);
    expect(green.g > 200 && green.r < 32, "Edited material not applied to the new snapshot");
    fresh.frame.reset();

    // R-M2-004：资产身份在构造时分配且不复用。释放一个几何后新建同尺寸分配的对象，
    // 必须重新上传并按新内容绘制，不能命中旧缓冲。
    AssetId released = 0;
    {
        auto probeScene = std::make_unique<Scene>();
        auto releasedGeometry = BoxGeometry(1, 1, 1);
        released = releasedGeometry->id;
        probeScene->add(releasedGeometry, gray);
        auto probeView = renderer.createView(64, 48, 64);
        auto probe = renderer.submit(
            {probeScene->snapshot(4), camera, *probeView, {{Channel::MetricDepth, Delivery::Cpu}}});
        expect(std::abs(centerDepth(*probe.frame) - 4.5f) < .001f, "Probe geometry depth");
        probe.frame.reset();
    }
    auto replaced = BoxGeometry(1, 1, 3);
    expect(replaced->id != released && replaced->id > released, "Asset identity was recycled after release");
    {
        Scene reuse;
        reuse.add(replaced, gray);
        auto uploads = renderer.assetStats().meshUploads;
        auto probe =
            renderer.submit({reuse.snapshot(4.1), camera, *near, {{Channel::MetricDepth, Delivery::Cpu}}});
        expect(std::abs(centerDepth(*probe.frame) - 3.5f) < .001f,
               "New geometry reused the released asset's GPU buffers");
        expect(renderer.assetStats().meshUploads == uploads + 1, "Replacement geometry was not uploaded");
        probe.frame.reset();
    }

    // R-M2-002：静止收敛后更换贴图，Color 不能继续输出旧历史。
    auto red = solid(255, 0, 0);
    auto redMaterial = std::make_shared<MeshStandardMaterial>();
    redMaterial->color = {1, 1, 1};
    redMaterial->roughness = 1;
    redMaterial->map = red;
    Scene textured;
    auto texturedId = textured.add(BoxGeometry(), redMaterial);
    auto before = renderer.submit({textured.snapshot(10), camera, *near, {{Channel::Color, Delivery::Cpu}}});
    for (int i = 0; i < 18; ++i)
    {
        before.frame.reset();
        before =
            renderer.submit({textured.snapshot(10 + i * .01), camera, *near, {{Channel::Color, Delivery::Cpu}}});
    }
    auto lit = centerColor(*before.frame);
    expect(lit.r > lit.g && lit.r > 100, "Stationary frames did not converge to the red texture");
    auto greenMaterial = *redMaterial;
    greenMaterial.map = solid(0, 255, 0);
    textured.get(texturedId).material = greenMaterial;
    for (int i = 0; i < 3; ++i)
    {
        before.frame.reset();
        before =
            renderer.submit({textured.snapshot(11 + i * .01), camera, *near, {{Channel::Color, Delivery::Cpu}}});
    }
    auto swapped = centerColor(*before.frame);
    expect(swapped.g > swapped.r, "Texture swap kept frozen TAA history");
    before.frame.reset();

    // 清缓存只切换代际：在途结果的资产与实例数据必须继续有效。
    auto held = renderer.submit({snapshot, camera, *near, {{Channel::MetricDepth, Delivery::CpuAndGpu}}});
    expect(held.status == SubmitStatus::Submitted, "Slot was not released before the cache switch");
    renderer.clearAssetCache();
    renderer.clearAssetCache();
    expect(renderer.assetStats().retiredGenerations >= 1, "Generation dropped while frames were in flight");
    expect(std::abs(centerDepth(*held.frame) - 4.5f) < .001f, "Cache switch destroyed in-flight assets");
    auto native = VulkanInterop::output(*held.frame, Channel::MetricDepth);
    expect(native.image && native.completion, "GPU export lost across the cache switch");

    // 旧快照在新代际里按自己持有的固定资产重新上传，仍重现当初的画面。
    auto replay = renderer.submit({snapshot, rear, *far, {{Channel::AlbedoMetal, Delivery::Cpu}}});
    auto replayed = centerAlbedo(*replay.frame);
    expect(replayed.r == replayed.g && replayed.r > 200, "Old snapshot lost its content across generations");
    replay.frame.reset();
    held.frame.reset();

    // R-M2-003：持续加载新资产时，已完成的上传批次在准备阶段回收，暂存副本随之释放。
    auto probeView = renderer.createView(64, 48, 64);
    SceneSnapshot last;
    unsigned batches = 0;
    for (int i = 0; i < 5; ++i)
    {
        Scene loader;
        loader.add(BoxGeometry(1, 1, 1 + i * .5f), gray);
        last = loader.snapshot(20 + i);
        auto loaded = renderer.submit({last, camera, *probeView, {{Channel::MetricDepth, Delivery::Cpu}}});
        centerDepth(*loaded.frame);
        loaded.frame.reset();
        batches = unsigned(renderer.assetStats().uploadBatches);
    }
    // 相同快照不会新增上传，本次准备会回收上一批已完成的上传。
    auto settled = renderer.submit({last, camera, *probeView, {{Channel::MetricDepth, Delivery::Cpu}}});
    centerDepth(*settled.frame);
    settled.frame.reset();
    stats = renderer.assetStats();
    expect(stats.uploadBatches == batches, "Reused snapshot still queued an upload batch");
    expect(stats.pendingUploads == 0, "Completed upload batches were never reclaimed");
    expect(stats.stagingBytes == 0, "Upload staging copies outlived their batch");

    // 视图改用新代际场景并释放旧结果后，旧代际才被回收。
    replay.frame.reset();
    for (int i = 0; i < 2; ++i)
    {
        auto repaintNear = renderer.submit({snapshot, camera, *near, {{Channel::Color, Delivery::Cpu}}});
        auto repaintFar = renderer.submit({snapshot, rear, *far, {{Channel::Color, Delivery::Cpu}}});
        repaintNear.frame.reset();
        repaintFar.frame.reset();
    }
    stats = renderer.assetStats();
    expect(stats.retiredGenerations == 0, "Retired generations were never reclaimed");
    expect(stats.meshUploads >= 2, "New generation reused the previous generation buffers");
    expect(stats.pendingUploads == 0, "Upload batches left behind after reclamation");
    std::cout << "PASS: shared scene, immutable asset versions, upload dependency and deferred reclamation\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
