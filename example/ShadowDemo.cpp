#include "ShadowDemo.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <vector>

using namespace sim;
namespace
{
glm::mat4 placement(glm::vec3 position, glm::vec3 size)
{
    return glm::scale(glm::translate(glm::mat4(1), position), size);
}

MeshStandardMaterial paint(glm::vec3 color)
{
    MeshStandardMaterial material;
    material.color = color;
    material.roughness = .78f;
    return material;
}
}

const char* shadowPresetName(ShadowPreset preset)
{
    const char* names[] = {"Long colonnade", "Contact study", "Offscreen caster"};
    return names[static_cast<unsigned>(preset)];
}

ShadowScene::ShadowScene(ShadowPreset preset)
{
    scene.sun.direction = {-.65f, -.82f, -.38f};
    const auto stone = paint({.70f, .58f, .42f});
    const auto blue = paint({.10f, .31f, .40f});
    const auto iron = paint({.24f, .29f, .31f});
    const auto orange = paint({.85f, .28f, .07f});
    const auto sand = paint({.48f, .42f, .32f});
    const auto cube = BoxGeometry();
    const auto ball = SphereGeometry(1, 32, 20);
    const auto column = CylinderGeometry(1, 1, 1, 24);

    if (preset == ShadowPreset::OffscreenCaster)
    {
        // 受光板离相机 15 cm，细条在相机后方，投影落入画面。
        scene.sun.direction = {-.35f, 0, -1};
        scene.add(cube, blue, placement({0, 0, -.01f}, {.5f, .3f, .02f}));
        for (int i = -2; i <= 2; ++i)
        {
            const auto id = scene.add(cube, iron, placement({.35f * .21f + .03f * i, 0, .21f},
                                                            {.009f, .092f, .006f}));
            if (i == 0)
                movingCaster = id;
        }
        motionAmplitude = .014f;
        return;
    }

    scene.add(cube, sand, placement({0, -.34f, -28}, {42, .6f, 108}));
    if (preset == ShadowPreset::ContactStudy)
    {
        scene.add(cube, blue, placement({0, .13f, 0}, {7, .34f, 5}));
        scene.add(ball, stone, placement({-1.75f, .91f, .55f}, {.62f, .62f, .62f}));
        scene.add(ball, orange, placement({0, 1.55f, .45f}, {.62f, .62f, .62f}));
        scene.add(ball, stone, placement({1.75f, 2.2f, .35f}, {.62f, .62f, .62f}));
        for (int i = 0; i < 9; ++i)
            scene.add(cube, iron, placement({(i - 4) * .68f, .92f, -1.65f}, {.065f, 1.24f, .08f}));
        scene.add(cube, iron, placement({0, 1.53f, -1.65f}, {5.6f, .09f, .1f}));
        movingCaster = scene.add(cube, orange, placement({0, 2.8f, -1.1f}, {1.1f, .16f, .8f}));
    }
    else
    {
        // 近处细栏杆、中距柱廊和远端塔体同时覆盖多个级联。
        for (int i = 0; i < 32; ++i)
        {
            const float z = 4.f - 2.6f * i;
            scene.add(cube, i % 2 ? stone : blue, placement({0, -.06f, z}, {5.5f, .15f, 2.42f}));
            if (i % 2 == 0)
            {
                for (float x : {-3.8f, 3.8f})
                {
                    scene.add(column, stone, placement({x, 1.6f, z}, {.38f, 3.2f, .38f}));
                    scene.add(cube, iron, placement({x, 3.23f, z}, {1.05f, .18f, 1.05f}));
                    scene.add(cube, stone, placement({x, .15f, z}, {.95f, .3f, .95f}));
                }
                scene.add(cube, iron, placement({0, 3.4f, z}, {8.7f, .16f, .18f}));
            }
            else
            {
                const float x = i % 4 == 1 ? -2.15f : 2.15f;
                scene.add(ball, orange, placement({x, .65f, z}, {.53f, .53f, .53f}));
            }
        }
        for (int i = 0; i < 19; ++i)
            scene.add(cube, iron, placement({-2.8f + i * .31f, .57f, 3}, {.035f, 1.18f, .07f}));
        scene.add(cube, iron, placement({0, 1.18f, 3}, {5.75f, .07f, .08f}));
        scene.add(cube, blue, placement({0, 4.4f, -83}, {8, 8.8f, 2.5f}));
        movingCaster = scene.add(cube, orange, placement({0, 4.5f, -6}, {2.8f, .22f, 1.5f}));
    }
}

SceneSnapshot ShadowScene::snapshot(unsigned frame, bool moving)
{
    auto result = scene.snapshot(double(frame) / 60);
    if (moving)
        for (auto& mesh : result.meshes)
            if (mesh.id == movingCaster)
                mesh.world[3].x += motionAmplitude * std::sin(float(frame) * .08f);
    return result;
}

PerspectiveCamera shadowCamera(ShadowPreset preset, float offset)
{
    PerspectiveCamera camera;
    if (preset == ShadowPreset::OffscreenCaster)
    {
        camera.position = {offset * .002f, 0, .15f};
        camera.target = {camera.position.x, 0, 0};
        camera.nearPlane = .01f;
        camera.farPlane = 120;
        return camera;
    }

    const bool closeup = preset == ShadowPreset::ContactStudy;
    camera.position = closeup ? glm::vec3(4.8f + offset, 3.9f, 7.1f) : glm::vec3(7.2f + offset, 5.4f, 12);
    camera.target = closeup ? glm::vec3(0, .75f, 0) : glm::vec3(0, .8f, -17);
    camera.farPlane = closeup ? 38 : 120;
    return camera;
}

ShadowTimings measureShadows(Renderer& renderer, ShadowPreset preset, RenderOptions options)
{
    using Clock = std::chrono::steady_clock;
    ShadowScene scene(preset);
    const auto camera = shadowCamera(preset);
    ShadowTimings result{};
    options.taa = false;
    options.debugChannel = 0;
    options.readback = false;
    constexpr unsigned warmup = 20, samples = 100;
    for (unsigned motion = 0; motion < result.size(); ++motion)
    {
        std::array<std::vector<double>, 3> measured, submitted;
        std::array<std::unique_ptr<View>, 3> views;
        // 所有路径预先创建同尺寸视图。仅单路提交并等完成后才开始下一路。
        for (unsigned path = 0; path < views.size(); ++path)
            if (path != 2 || renderer.supportsRayQueryContactShadows())
                views[path] = renderer.createView(960, 600, 1024);
        renderer.clearAssetCache();

        for (unsigned frame = 0; frame < warmup + samples; ++frame)
        {
            // 每条路径都使用同一运动轨迹。轮换测量顺序，减少热机状态带来的偏差。
            auto snapshot = scene.snapshot(frame, motion != 0);

            for (unsigned i = 0; i < views.size(); ++i)
            {
                const unsigned path = (i + frame) % unsigned(views.size());
                if (!views[path])
                    continue;
                options.shadows = path != 0;
                options.rayQueryContactShadows = path == 2;
                const auto start = Clock::now();
                renderer.render(snapshot, camera, *views[path], options);
                const auto queued = Clock::now();
                views[path]->wait();
                const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                if (frame == 0)
                    result[motion][path].first = ms;
                if (frame >= warmup)
                {
                    measured[path].push_back(ms);
                    submitted[path].push_back(std::chrono::duration<double, std::milli>(queued - start).count());
                }
            }
        }
        for (unsigned path = 0; path < views.size(); ++path)
        {
            auto& values = measured[path];
            if (values.empty())
                continue;
            std::sort(values.begin(), values.end());
            result[motion][path].median = (values[49] + values[50]) * .5;
            result[motion][path].p95 = values[94];
            auto& submit = submitted[path];
            std::sort(submit.begin(), submit.end());
            result[motion][path].submitMedian = (submit[49] + submit[50]) * .5;
        }
    }
    return result;
}

void printShadowTimings(std::ostream& out, const ShadowTimings& timings, bool rayQuery)
{
    const char* paths[] = {"No shadows", "CSM + PCSS + screen contact", "CSM + PCSS + Ray Query contact"};
    out << "960x600 | CSM 1024 px/cascade | TAA off | 20 warm-up + 100 measured per path\n"
           "End-to-end render + wait, milliseconds. Shared raster assets. No presentation or readback.\n";
    for (unsigned motion = 0; motion < timings.size(); ++motion)
    {
        out << (motion ? "Moving caster\n" : "Static scene\n");
        for (unsigned path = 0; path < (rayQuery ? 3u : 2u); ++path)
        {
            const auto& t = timings[motion][path];
            out << "  " << paths[path] << ": p50=" << std::fixed << std::setprecision(3) << t.median
                << "  p95=" << t.p95 << "  submit p50=" << t.submitMedian << "  first=" << t.first << " ms\n";
        }
    }
    if (!rayQuery)
        out << "Ray Query unavailable on this device.\n";
}
