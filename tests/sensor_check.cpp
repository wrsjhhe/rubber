#include "sim/Renderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <string_view>

using namespace sim;
void expect(bool ok, const char* error)
{
    if (!ok)
        throw std::runtime_error(error);
}
int main(int argc, char** argv)
try
{
    DeviceOptions options;
    options.validation = argc > 1 && std::string_view(argv[1]) == "--validation";
    Renderer renderer(options);
    Scene scene;
    auto red = std::make_shared<MeshStandardMaterial>();
    red->color = {.75f, .025f, .015f};
    auto box = BoxGeometry();
    auto id = scene.add(box, red);
    scene.add(box, red, glm::translate(glm::mat4(1), {50, 0, 0}));
    auto first = renderer.createView(192, 128, 256), second = renderer.createView(96, 64, 128);
    PerspectiveCamera camera;
    camera.position = {0, 0, 5};
    camera.fx = camera.fy = 130;
    camera.cx = 96;
    camera.cy = 64;
    auto snapshot = scene.snapshot(1.25);
    RenderOptions config;
    config.readback = true;
    renderer.render(snapshot, camera, *first, config);
    auto rear = camera;
    rear.position.z = 8;
    rear.fx = rear.fy = 65;
    rear.cx = 48;
    rear.cy = 32;
    renderer.render(snapshot, rear, *second, config);
    auto a = first->read(), b = second->read();
    auto testCenter = [&](const Frame& f, float z)
    {
        size_t k = f.width * (f.height / 2) + f.width / 2;
        expect(f.ids[k] == id, "Wrong instance ID");
        expect(std::abs(f.depth[k] - z) < .001f, "Wrong metric depth");
        expect(f.rgba[k * 4] > f.rgba[k * 4 + 1], "Material color lost");
        expect(!f.ids[0] && f.depth[0] == 0 && f.time == 1.25, "Invalid background or timestamp");
    };
    testCenter(a, 4.5f);
    testCenter(b, 7.5f);
    expect(first->visibleMeshes() == 1, "Frustum culling failed");
    renderer.render(snapshot, camera, *first, config);
    auto history = first->read();
    expect(a.ids == history.ids && a.depth == history.depth, "TAA jitter contaminated ground truth");
    scene.get(id).local = glm::translate(glm::mat4(1), {3, 0, 0});
    renderer.render(scene.snapshot(1.3), camera, *first, config);
    expect(first->read().ids[64 * 192 + 96] == 0, "Moving mesh retained stale depth");
    auto offset = camera;
    offset.cx = 45;
    renderer.render(snapshot, offset, *first, config);
    auto offAxis = first->read();
    expect(offAxis.ids[64 * 192 + 45] == id && offAxis.ids[64 * 192 + 96] == 0, "Principal point offset lost");
    Scene hierarchy;
    auto group = hierarchy.addGroup(glm::translate(glm::mat4(1), {1, 2, 3}));
    auto nested = hierarchy.addGroup(glm::translate(glm::mat4(1), {2, 0, 0}), group);
    hierarchy.add(box, red, glm::mat4(1), nested);
    auto fixed = hierarchy.snapshot(2);
    hierarchy.remove(group);
    expect(fixed.meshes.size() == 1 && fixed.meshes[0].world[3] == glm::vec4(3, 2, 3, 1) &&
               hierarchy.snapshot(3).meshes.empty(),
           "Hierarchy snapshot failed");
    renderer.clearAssetCache();
    renderer.render(snapshot, camera, *first, config);
    testCenter(first->read(), 4.5f);
    std::cout
        << "PASS: two cameras, calibration, RGB, metric Z, stable ID, independent time history and culling\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
