#include "ShadowDemo.hpp"
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
void captureShadowDemo(Renderer& renderer, const char* filename, ShadowPreset preset, float cameraOffset)
{
    ShadowScene scene(preset);
    auto view = renderer.createView(960, 600);
    auto snapshot = scene.snapshot(0, false);
    RenderOptions options;
    options.readback = true;
    options.taa = false;
    options.rayQueryContactShadows = false;
    renderer.render(snapshot, shadowCamera(preset, cameraOffset), *view, options);
    auto raster = view->read();
    if (!renderer.supportsRayQueryContactShadows())
    {
        save(raster, filename);
        std::cout << "Ray Query unavailable. Saved CSM only to " << filename << '\n';
        return;
    }

    options.rayQueryContactShadows = true;
    renderer.render(snapshot, shadowCamera(preset, cameraOffset), *view, options);
    auto ray = view->read();
    Frame pair{};
    pair.width = raster.width + ray.width;
    pair.height = raster.height;
    pair.rgba.reserve(size_t(pair.width) * pair.height * 4);
    const size_t stride = size_t(raster.width) * 4;
    for (unsigned y = 0; y < pair.height; ++y)
        for (const auto* image : {&raster, &ray})
            pair.rgba.insert(pair.rgba.end(), image->rgba.begin() + y * stride, image->rgba.begin() + (y + 1) * stride);
    save(pair, filename);
    std::cout << "Saved screen-space contact (left) and Ray Query contact (right) to " << filename << '\n';
}

int shadowDemo(Renderer& renderer, GLFWwindow* window, bool smoke, ShadowPreset preset, float offset)
{
    constexpr unsigned width = 960, height = 600;
    const bool ray = renderer.supportsRayQueryContactShadows();
    auto rasterView = renderer.createView(width, height);
    auto rayView = ray ? renderer.createView(width, height) : nullptr;
    Presenter presenter(renderer.handles(), window);
    const auto texture = [&](const View& view)
    {
        return ImGui_ImplVulkan_AddTexture(renderer.displaySampler(), view.colorView(),
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };
    const auto rasterImage = texture(*rasterView);
    const auto rayImage = ray ? texture(*rayView) : VkDescriptorSet{};
    std::array scenes{ShadowScene(ShadowPreset::Colonnade), ShadowScene(ShadowPreset::ContactStudy),
                      ShadowScene(ShadowPreset::OffscreenCaster)};
    std::optional<ShadowTimings> timings;
    RenderOptions options;
    options.taa = false;
    unsigned frame = 0;
    int display = ray ? 0 : 1;
    float cameraOffset = offset;
    bool moving = false;
    while (!glfwWindowShouldClose(window) && (!smoke || frame < 8))
    {
        glfwPollEvents();
        int fw, fh;
        glfwGetFramebufferSize(window, &fw, &fh);
        if (fw <= 0 || fh <= 0)
        {
            glfwWaitEventsTimeout(.05);
            continue;
        }
        if (fw != presenter.window.Width || fh != presenter.window.Height || presenter.rebuild)
            presenter.resize(fw, fh);
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const auto size = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("Shadow comparison", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::TextUnformatted("Shadow comparison");
        ImGui::SameLine();
        ImGui::TextDisabled("960 x 600 per view");
        if (!ray)
            ImGui::TextUnformatted("Ray Query unavailable. Showing screen-space contact.");
        if (ray)
        {
            ImGui::SetNextItemWidth(170);
            ImGui::Combo("View", &display, "Side by side\0Screen-space contact\0Ray Query contact\0");
            ImGui::SameLine();
        }
        int selected = static_cast<int>(preset);
        ImGui::SetNextItemWidth(170);
        if (ImGui::Combo("Scene", &selected, "Long colonnade\0Contact study\0Offscreen caster\0"))
        {
            preset = static_cast<ShadowPreset>(selected);
            timings.reset();
        }
        if (preset == ShadowPreset::OffscreenCaster)
            ImGui::TextDisabled("Five thin strips behind the camera project onto the panel.");
        ImGui::SameLine();
        ImGui::Checkbox("Moving caster", &moving);
        ImGui::SameLine();
        ImGui::Checkbox("TAA", &options.taa);
        ImGui::SetNextItemWidth(150);
        ImGui::SliderFloat("Camera offset", &cameraOffset, -5.f, 5.f, "%.1f m");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        if (ImGui::SliderFloat("Sun radius", &options.sunAngularRadius, 0.f, .08f, "%.3f rad"))
            timings.reset();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        if (ImGui::SliderFloat("Contact reach", &options.contactShadowLength, 0.f, 1.f, "%.2f m"))
            timings.reset();
        if (ImGui::Button("Run benchmark"))
        {
            check(vkDeviceWaitIdle(renderer.handles().device));
            timings = measureShadows(renderer, preset, options);
            printShadowTimings(std::cout, *timings, ray);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Render + wait, no presentation or readback, TAA off");
        if (timings)
        {
            const char* labels[] = {"Off", "Screen", "Ray Query"};
            for (unsigned motion = 0; motion < timings->size(); ++motion)
            {
                ImGui::TextUnformatted(motion ? "Moving" : "Static");
                for (unsigned path = 0; path < (ray ? 3u : 2u); ++path)
                {
                    const auto& result = (*timings)[motion][path];
                    ImGui::SameLine();
                    ImGui::Text("%s  %.2f / %.2f ms (submit %.2f)", labels[path], result.median,
                                result.p95, result.submitMedian);
                }
            }
            ImGui::TextDisabled("p50 / p95 from 100 measured frames");
        }

        if (smoke && ray)
            display = int(frame % 3);
        const auto snapshot = scenes[static_cast<unsigned>(preset)].snapshot(frame, moving || smoke);
        const auto camera = shadowCamera(preset, cameraOffset);
        const int columns = display == 0 ? 2 : 1;
        const auto room = ImGui::GetContentRegionAvail();
        const float imageWidth = std::max(1.f, std::min((room.x - (columns - 1) * 10.f) / columns,
                                                        (room.y - 30.f) * 1.6f));
        const ImVec2 dimensions{imageWidth, imageWidth / 1.6f};
        const auto draw = [&](const char* title, View& view, VkDescriptorSet image, bool raster)
        {
            options.rayQueryContactShadows = !raster;
            renderer.render(snapshot, camera, view, options);
            ImGui::BeginGroup();
            ImGui::TextUnformatted(title);
            ImGui::Image((ImTextureID)(uintptr_t)image, dimensions);
            ImGui::EndGroup();
        };
        if (display != 2)
            draw("CSM + PCSS | Screen-space contact", *rasterView, rasterImage, true);
        if (display == 0)
            ImGui::SameLine(0, 10);
        if (ray && display != 1)
            draw("CSM + PCSS | Ray Query contact", *rayView, rayImage, false);
        ImGui::End();
        ImGui::Render();
        presenter.present();
        ++frame;
    }
    check(vkDeviceWaitIdle(renderer.handles().device));
    ImGui_ImplVulkan_RemoveTexture(rasterImage);
    if (rayImage)
        ImGui_ImplVulkan_RemoveTexture(rayImage);
    return 0;
}

int runShadowDemo(int argc, char** argv)
{
    bool smoke = false, benchmark = false;
    DeviceOptions options;
    ShadowPreset preset = ShadowPreset::Colonnade;
    float offset = 0;
    const char* capture = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i];
        if (arg == "--smoke")
            smoke = true;
        else if (arg == "--validation")
            options.validation = true;
        else if (arg == "--raster-shadows")
            options.forceRasterShadows = true;
        else if (arg == "--benchmark")
            benchmark = true;
        else if (arg == "--closeup")
            preset = ShadowPreset::ContactStudy;
        else if (arg == "--offscreen")
            preset = ShadowPreset::OffscreenCaster;
        else if (arg == "--offset" && i + 1 < argc)
            offset = std::stof(argv[++i]);
        else if (arg == "--capture" && i + 1 < argc)
            capture = argv[++i];
        else
            throw std::invalid_argument("Usage: shadow_demo [--closeup | --offscreen] [--offset metres] "
                                        "[--benchmark | --capture file.png] [--smoke] [--validation] [--raster-shadows]");
    }

    if (benchmark || capture)
    {
        Renderer renderer(options);
        if (benchmark)
        {
            VkPhysicalDeviceProperties gpu{};
            vkGetPhysicalDeviceProperties(renderer.handles().physical, &gpu);
            std::cout << gpu.deviceName << " | " << shadowPresetName(preset) << '\n';
            printShadowTimings(std::cout, measureShadows(renderer, preset, {}), renderer.supportsRayQueryContactShadows());
        }
        else
            captureShadowDemo(renderer, capture, preset, offset);
        return 0;
    }

    Window window("Aperture | Shadow comparison", smoke);
    window.configure(options);
    Renderer renderer(options);
    return shadowDemo(renderer, window.get(), smoke, preset, offset);
}
}
int main(int argc, char** argv)
try
{
    return runShadowDemo(argc, argv);
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
