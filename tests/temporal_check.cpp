#include "sim/Renderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
constexpr int width = 180;
constexpr int height = 120;
constexpr int scale = 4;

double linearRed(const sim::Frame& frame, int x, int y)
{
    double s = frame.rgba[4 * (y * frame.width + x)] / 255.0;
    return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}

// 在诊断通道中，物体呈均匀的线性颜色。参考图的子像素平均值直接反映边缘覆盖率。
double referenceError(const sim::Frame& image, const sim::Frame& reference)
{
    double error = 0;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            double sum = 0;
            for (int j = 0; j < scale; ++j)
                for (int i = 0; i < scale; ++i)
                    sum += linearRed(reference, x * scale + i, y * scale + j);
            error += std::abs(linearRed(image, x, y) - sum / (scale * scale));
        }
    }
    return error;
}
}

int main(int argc, char** argv)
try
{
    sim::DeviceOptions device;
    device.validation = argc > 1 && std::string_view(argv[1]) == "--validation";
    sim::Renderer renderer(device);
    auto view = renderer.createView(width, height, 64);
    auto referenceView = renderer.createView(width * scale, height * scale, 64);
    sim::PerspectiveCamera camera;
    camera.position = {0, 0, 4};

    sim::Scene scene;
    auto mesh = scene.add(sim::BoxGeometry(2.7f, 0.17f, 0.1f), std::make_shared<sim::MeshStandardMaterial>(),
                          glm::rotate(glm::mat4(1), 0.39f, {0, 0, 1}));
    auto snapshot = scene.snapshot(0);
    sim::RenderOptions raw;
    raw.shadows = raw.taa = false;
    raw.readback = true;
    raw.debugChannel = 3;
    renderer.render(snapshot, camera, *referenceView, raw);
    auto reference = referenceView->read();
    renderer.render(snapshot, camera, *view, raw);
    auto aliased = view->read();

    auto taa = raw;
    taa.taa = true;
    sim::Frame converged;
    for (int i = 0; i < 34; ++i)
    {
        snapshot.time = i / 60.0;
        renderer.render(snapshot, camera, *view, taa);
        if (i == 15)
            converged = view->read();
        if (i > 15 && view->read().rgba != converged.rgba)
            throw std::runtime_error("Stationary silhouette still flickers");
    }
    double rawError = referenceError(aliased, reference);
    double taaError = referenceError(converged, reference);
    std::cout << "Static absolute error versus 4x4 coverage: raw=" << rawError << ", TAA=" << taaError << '\n';
    if (rawError <= 0 || taaError >= rawError * 0.45)
        throw std::runtime_error("Insufficient subpixel coverage on the diagonal edge");
    if (aliased.ids != converged.ids || aliased.depth != converged.depth)
        throw std::runtime_error("Temporal filtering changed sensor ground truth");

    // 平移相机后对照同一时刻的独立高分辨率视图，避免通过冻结历史掩盖运动或产生长拖尾。
    double movingRaw = 0, movingTaa = 0;
    auto single = renderer.createView(width, height, 64);
    for (int i = 0; i < 12; ++i)
    {
        camera.position.y += 0.038f;
        camera.target.y += 0.038f;
        snapshot.time += 1.0 / 60;
        renderer.render(snapshot, camera, *view, taa);
        auto moving = view->read();
        renderer.render(snapshot, camera, *referenceView, raw);
        reference = referenceView->read();
        movingTaa += referenceError(moving, reference);
        // 用参考视图投影到像素中心的独立单样本绘制作为比较基准。
        renderer.render(snapshot, camera, *single, raw);
        movingRaw += referenceError(single->read(), reference);
    }
    std::cout << "Moving silhouette error: raw=" << movingRaw << ", TAA=" << movingTaa << '\n';
    if (movingTaa >= movingRaw)
        throw std::runtime_error("Moving edge quality regressed");

    scene.remove(mesh);
    renderer.render(scene.snapshot(2), camera, *view, taa);
    auto empty = view->read();
    for (size_t i = 0; i < empty.rgba.size(); i += 4)
        if (empty.rgba[i] != 0)
            throw std::runtime_error("Removed object left a ghost");
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
