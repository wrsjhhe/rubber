#include "sim/Renderer.hpp"
#include "sim/VulkanInterop.hpp"
#include <glm/gtc/matrix_transform.hpp>
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
float centerDepth(const FrameResult& result)
{
    auto data = result.read(Channel::MetricDepth);
    auto index = (data.info.height / 2) * data.info.width + data.info.width / 2;
    float value;
    std::memcpy(&value, data.bytes.data() + index * 4, 4);
    return value;
}
int main(int argc, char** argv)
try
{
    DeviceOptions device;
    device.validation = argc > 1 && std::string_view(argv[1]) == "--validation";
    auto renderer = std::make_unique<Renderer>(device);
    Scene scene;
    auto material = std::make_shared<MeshStandardMaterial>();
    auto id = scene.add(BoxGeometry(), material);
    auto snapshot = scene.snapshot(12.5);
    PerspectiveCamera camera;
    camera.position = {0, 0, 5};
    auto a = renderer->createView(64, 48, 64);
    auto b = renderer->createView(64, 48, 64);
    std::vector<OutputDesc> channels{{Channel::MetricDepth, Delivery::CpuAndGpu},
                                     {Channel::InstanceId, Delivery::Cpu}};
    auto first = renderer->submit({snapshot, camera, *a, channels});
    auto rear = camera;
    rear.position.z = 8;
    auto second = renderer->submit({snapshot, rear, *b, channels});
    expect(std::abs(centerDepth(*first.frame) - 4.5f) < .001f, "First camera depth");
    expect(std::abs(centerDepth(*second.frame) - 7.5f) < .001f, "Second camera depth");
    expect(first.frame->time() == 12.5, "Frame timestamp");
    auto native = VulkanInterop::output(*first.frame, Channel::MetricDepth);
    expect(native.image && native.view && native.completion, "GPU export missing");

    scene.get(id).local = glm::translate(glm::mat4(1), {0, 0, 1});
    auto moved = scene.snapshot(13);
    auto third = renderer->submit({moved, camera, *a, channels});
    auto fourth = renderer->submit({moved, camera, *a, channels});
    auto busy = renderer->submit({moved, camera, *a, channels});
    expect(busy.status == SubmitStatus::Busy && !busy.frame, "Unbounded outstanding outputs");
    expect(std::abs(centerDepth(*third.frame) - 3.5f) < .001f, "Updated scene depth");
    expect(std::abs(centerDepth(*first.frame) - 4.5f) < .001f, "Retained result overwritten");
    third.frame.reset();
    auto resumed = renderer->submit({moved, camera, *a, {{Channel::Color, Delivery::Cpu}}});
    expect(resumed.status == SubmitStatus::Submitted, "Released slot did not recover");
    expect(resumed.frame->read(Channel::Color).bytes.size() == 64 * 48 * 4, "Lazy color allocation");
    bool rejected = false;
    try
    {
        second.frame->read(Channel::Color);
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    expect(rejected, "Missing channel silently accepted");
    a.reset();
    b.reset();
    renderer.reset();
    expect(std::abs(centerDepth(*first.frame) - 4.5f) < .001f, "Result does not own device lifetime");
    std::cout << "PASS: requested channels, bounded slots, retained CPU/GPU outputs and independent cameras\n";
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
