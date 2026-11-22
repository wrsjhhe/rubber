#include "SensorDemo.hpp"
#include "DemoSupport.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace
{
using namespace sim;
using namespace demo;
int sensorDemo(Renderer& renderer, GLFWwindow* window, bool smoke)
{
    Presenter presenter(renderer.handles(), window);
    SensorDemo demo(renderer);
    unsigned frame = 0;
    while (!glfwWindowShouldClose(window) && (!smoke || frame < 8))
    {
        glfwPollEvents();
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        if (width <= 0 || height <= 0)
        {
            glfwWaitEventsTimeout(.05);
            continue;
        }
        if (width != presenter.window.Width || height != presenter.window.Height || presenter.rebuild)
            presenter.resize(width, height);

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        demo.draw(smoke, frame);
        ImGui::Render();
        presenter.present();
        ++frame;
    }
    return 0;
}

int runSensorDemo(int argc, char** argv)
{
    bool smoke = false, verify = false;
    DeviceOptions options;
    const char* capture = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i];
        if (arg == "--smoke")
            smoke = true;
        else if (arg == "--validation")
            options.validation = true;
        else if (arg == "--check")
            verify = true;
        else if (arg == "--capture" && i + 1 < argc)
            capture = argv[++i];
        else
            throw std::invalid_argument("Usage: sensor_demo [--check | --capture file.png] [--smoke] [--validation]");
    }

    if (verify || capture)
    {
        Renderer renderer(options);
        SensorDemo demo(renderer);
        if (verify)
            demo.verify();
        else
        {
            save(demo.capture(), capture);
            std::cout << "Saved camera A (left), B (right), with RGB, metric Z and ID rows to " << capture << '\n';
        }
        return 0;
    }

    Window window("Aperture | Dual camera capture", smoke);
    window.configure(options);
    Renderer renderer(options);
    return sensorDemo(renderer, window.get(), smoke);
}
}
int main(int argc, char** argv)
try
{
    return runSensorDemo(argc, argv);
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
