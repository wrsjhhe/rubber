#include "sim/Renderer.hpp"
#include "sim/VulkanInterop.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

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

    // 新版本使用新对象发布：旧快照继续持有旧版本数据。
    auto green = std::make_shared<MeshStandardMaterial>();
    green->color = {0.f, 1.f, 0.f};
    scene.get(id).material = green;
    auto updated = scene.snapshot(1.1);
    auto third = renderer.submit({updated, camera, *near, {{Channel::AlbedoMetal, Delivery::Cpu}}});
    auto stale = renderer.submit({snapshot, rear, *far, {{Channel::AlbedoMetal, Delivery::Cpu}}});
    stats = renderer.assetStats();
    expect(stats.scenePreparations == 2, "New asset version reused the old shared scene");
    expect(stats.materialUploads == 2, "New material version was not published");
    expect(stats.meshUploads == 1, "Unchanged geometry was uploaded again");
    expect(stats.textureUploads == 2, "Unchanged textures were uploaded again");
    auto fresh = centerAlbedo(*third.frame);
    auto old = centerAlbedo(*stale.frame);
    expect(fresh.r < 32 && fresh.g > 200, "New material version not applied");
    expect(old.r > 200 && old.g > 200 && old.r == old.g, "Old snapshot inherited the new material");

    // 原地修改必须递增版本才会重新发布，否则共享场景仍是可见事实。
    green->color = {0.f, 0.f, 1.f};
    green->revision = 2;
    auto published = scene.snapshot(1.2);
    auto fourth = renderer.submit({published, camera, *near, {{Channel::AlbedoMetal, Delivery::Cpu}}});
    expect(renderer.assetStats().scenePreparations == 3, "Version bump did not republish the scene");
    auto blue = centerAlbedo(*fourth.frame);
    expect(blue.b > 200 && blue.g < 32, "Version bump did not apply the new material");

    // 清缓存只切换代际：在途结果的资产与实例数据必须继续有效。
    first.frame.reset();
    auto held = renderer.submit({snapshot, camera, *near, {{Channel::MetricDepth, Delivery::CpuAndGpu}}});
    expect(held.status == SubmitStatus::Submitted, "Slot was not released before the cache switch");
    renderer.clearAssetCache();
    renderer.clearAssetCache();
    expect(renderer.assetStats().retiredGenerations >= 1, "Generation dropped while frames were in flight");
    expect(std::abs(centerDepth(*held.frame) - 4.5f) < .001f, "Cache switch destroyed in-flight assets");
    auto native = VulkanInterop::output(*held.frame, Channel::MetricDepth);
    expect(native.image && native.completion, "GPU export lost across the cache switch");

    // 视图改用新代际场景并释放旧结果后，旧代际才被回收。
    second.frame.reset();
    third.frame.reset();
    stale.frame.reset();
    fourth.frame.reset();
    held.frame.reset();
    for (int i = 0; i < 2; ++i)
    {
        auto repaintNear = renderer.submit({snapshot, camera, *near, {{Channel::Color, Delivery::Cpu}}});
        auto repaintFar = renderer.submit({snapshot, rear, *far, {{Channel::Color, Delivery::Cpu}}});
        repaintNear.frame.reset();
        repaintFar.frame.reset();
    }
    stats = renderer.assetStats();
    expect(stats.retiredGenerations == 0, "Retired generations were never reclaimed");
    expect(stats.meshUploads == 2, "New generation reused the previous generation buffers");
    std::cout << "PASS: shared scene, asset versions, upload dependency and deferred reclamation\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
