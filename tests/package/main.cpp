// 外部消费者：只用 find_package(SimRenderer) 提供的 sim::renderer，离屏渲染一帧并读回。
// 这个程序同时是"安装后能否不依赖原源码/构建目录运行"的证据：它打印实际使用的着色器
// 目录，check.cmake 断言该目录位于安装前缀之下。
#include "sim/Renderer.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

int main()
{
    try
    {
        sim::DeviceOptions options;
        sim::Renderer renderer(options);
        std::cout << "shader-directory: " << renderer.shaderDirectory().generic_string() << "\n";

        sim::Scene scene;
        auto material = std::make_shared<sim::MeshStandardMaterial>();
        material->color = {0.8f, 0.35f, 0.2f};
        scene.add(sim::BoxGeometry(), material);
        const auto snapshot = scene.snapshot(1.0);

        sim::PerspectiveCamera camera;
        camera.position = {0, 0, 5};
        auto view = renderer.createView(96, 64, 128);
        auto submission = renderer.submit({snapshot, camera, *view,
                                           {{sim::Channel::Color, sim::Delivery::Cpu},
                                            {sim::Channel::MetricDepth, sim::Delivery::Cpu}}});
        if (submission.status != sim::SubmitStatus::Submitted || !submission.frame)
        {
            std::cerr << "submit failed: the renderer returned no frame\n";
            return 1;
        }
        const auto color = submission.frame->read(sim::Channel::Color);
        const auto depth = submission.frame->read(sim::Channel::MetricDepth);
        if (color.bytes.size() != 96u * 64u * 4u || depth.bytes.size() != 96u * 64u * 4u)
        {
            std::cerr << "unexpected output sizes: color=" << color.bytes.size() << " depth=" << depth.bytes.size()
                      << "\n";
            return 1;
        }
        size_t bright = 0;
        for (size_t i = 0; i < color.bytes.size(); i += 4)
            if (color.bytes[i] > 8 || color.bytes[i + 1] > 8 || color.bytes[i + 2] > 8)
                ++bright;
        float center = 0;
        std::memcpy(&center, depth.bytes.data() + (32u * 96u + 48u) * 4u, 4);
        std::cout << "color-bytes: " << color.bytes.size() << "\n";
        std::cout << "bright-pixels: " << bright << "\n";
        std::cout << "center-depth: " << center << "\n";
        if (bright < 200)
        {
            std::cerr << "rendered image is empty\n";
            return 1;
        }
        // 相机在 z=5，箱体前表面在 z=0.5，米制深度应当是 4.5。
        if (std::abs(center - 4.5f) > 0.01f)
        {
            std::cerr << "unexpected center depth: " << center << "\n";
            return 1;
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
