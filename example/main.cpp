#include "Gallery.hpp"
#include "ShadowDemo.hpp"
#include "sim/Gltf.hpp"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace
{
using namespace sim;
void check(VkResult r)
{
    if (r != VK_SUCCESS)
        throw std::runtime_error("Vulkan presentation error " + std::to_string(r));
}
void save(const Frame& f, const char* file)
{
    if (!stbi_write_png(file, int(f.width), int(f.height), 4, f.rgba.data(), int(f.width) * 4))
        throw std::runtime_error("Cannot write image");
}
struct Presenter
{
    const DeviceHandles& d;
    ImGui_ImplVulkanH_Window window{};
    bool rebuild = false;
    Presenter(const DeviceHandles& device, GLFWwindow* glfw) : d(device)
    {
        window.Surface = d.surface;
        VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
        window.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(d.physical, d.surface, formats, 2,
                                                                     VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
        window.PresentMode = VK_PRESENT_MODE_FIFO_KHR;
        int w, h;
        glfwGetFramebufferSize(glfw, &w, &h);
        resize(w, h);
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
#ifdef _WIN32
        if (std::filesystem::exists("C:/Windows/Fonts/segoeui.ttf"))
            io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 18);
#endif
        ImGui::StyleColorsDark();
        auto& style = ImGui::GetStyle();
        style.WindowPadding = {20, 18};
        style.ItemSpacing = {9, 10};
        style.FramePadding = {8, 5};
        style.FrameRounding = 4;
        style.Colors[ImGuiCol_WindowBg] = {.07f, .09f, .10f, 1};
        style.Colors[ImGuiCol_Header] = {.13f, .28f, .29f, 1};
        style.Colors[ImGuiCol_FrameBg] = {.12f, .16f, .17f, 1};
        style.Colors[ImGuiCol_CheckMark] = {.95f, .66f, .32f, 1};
        style.Colors[ImGuiCol_SliderGrab] = {.85f, .56f, .25f, 1};
        ImGui_ImplGlfw_InitForVulkan(glfw, true);
        ImGui_ImplVulkan_InitInfo init{};
        init.ApiVersion = VK_API_VERSION_1_3;
        init.Instance = d.instance;
        init.PhysicalDevice = d.physical;
        init.Device = d.device;
        init.QueueFamily = d.family;
        init.Queue = d.queue;
        init.DescriptorPoolSize = 32;
        init.MinImageCount = 2;
        init.ImageCount = window.ImageCount;
        init.RenderPass = window.RenderPass;
        init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init.CheckVkResultFn = check;
        if (!ImGui_ImplVulkan_Init(&init))
            throw std::runtime_error("ImGui initialization failed");
    }
    ~Presenter()
    {
        vkDeviceWaitIdle(d.device);
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        // 表面由设备上下文持有。
        window.Surface = VK_NULL_HANDLE;
        ImGui_ImplVulkanH_DestroyWindow(d.instance, d.device, &window, nullptr);
    }
    void resize(int w, int h)
    {
        ImGui_ImplVulkanH_CreateOrResizeWindow(d.instance, d.physical, d.device, &window, d.family, nullptr, w, h,
                                               2);
        window.FrameIndex = 0;
        rebuild = false;
    }
    void present()
    {
        auto& w = window;
        auto acquired = w.FrameSemaphores[w.SemaphoreIndex].ImageAcquiredSemaphore;
        auto r = vkAcquireNextImageKHR(d.device, w.Swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &w.FrameIndex);
        if (r == VK_ERROR_OUT_OF_DATE_KHR)
        {
            rebuild = true;
            return;
        }
        if (r == VK_SUBOPTIMAL_KHR)
            rebuild = true;
        else
            check(r);
        auto signal = w.FrameSemaphores[w.FrameIndex].RenderCompleteSemaphore;
        auto& f = w.Frames[w.FrameIndex];
        check(vkWaitForFences(d.device, 1, &f.Fence, VK_TRUE, UINT64_MAX));
        check(vkResetFences(d.device, 1, &f.Fence));
        check(vkResetCommandPool(d.device, f.CommandPool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(f.CommandBuffer, &begin));
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = w.RenderPass;
        rp.framebuffer = f.Framebuffer;
        rp.renderArea.extent = {uint32_t(w.Width), uint32_t(w.Height)};
        rp.clearValueCount = 1;
        rp.pClearValues = &w.ClearValue;
        vkCmdBeginRenderPass(f.CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), f.CommandBuffer);
        vkCmdEndRenderPass(f.CommandBuffer);
        check(vkEndCommandBuffer(f.CommandBuffer));
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &f.CommandBuffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &signal;
        check(vkQueueSubmit(d.queue, 1, &submit, f.Fence));
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &signal;
        present.swapchainCount = 1;
        present.pSwapchains = &w.Swapchain;
        present.pImageIndices = &w.FrameIndex;
        r = vkQueuePresentKHR(d.queue, &present);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
            rebuild = true;
        else
            check(r);
        w.SemaphoreIndex = (w.SemaphoreIndex + 1) % w.SemaphoreCount;
    }
};
void caption(const char* text)
{
    ImGui::Spacing();
    ImGui::TextColored({.96f, .71f, .42f, 1}, "%s", text);
    ImGui::Separator();
}
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

int shadowDemo(Renderer& renderer, GLFWwindow* window, bool smoke, ShadowPreset preset)
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
    float cameraOffset = 0;
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

int gallery(int argc, char** argv)
{
    bool smoke = false, validation = false, benchmark = false, rasterShadows = false;
    bool compareShadows = false, shadowBenchmark = false;
    ShadowPreset preset = ShadowPreset::Colonnade;
    float captureOffset = 0;
    const char* capture = nullptr;
    std::filesystem::path asset;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i];
        if (arg == "--smoke")
            smoke = true;
        else if (arg == "--benchmark")
            benchmark = true;
        else if (arg == "--validation")
            validation = true;
        else if (arg == "--shadow-demo")
            compareShadows = true;
        else if (arg == "--shadow-benchmark")
            shadowBenchmark = true;
        else if (arg == "--shadow-closeup")
            preset = ShadowPreset::ContactStudy;
        else if (arg == "--shadow-offscreen")
            preset = ShadowPreset::OffscreenCaster;
        else if (arg == "--shadow-offset" && i + 1 < argc)
            captureOffset = std::stof(argv[++i]);
        else if (arg == "--raster-shadows")
            rasterShadows = true;
        else if (arg == "--model" && i + 1 < argc)
            asset = argv[++i];
        else if (arg == "--capture" && i + 1 < argc)
            capture = argv[++i];
        else
            throw std::invalid_argument(
                "Usage: gallery [--model asset.glb] [--capture file.png] [--smoke] [--benchmark] [--validation] "
                "[--raster-shadows] [--shadow-demo | --shadow-benchmark] [--shadow-closeup | --shadow-offscreen] "
                "[--shadow-offset metres]");
    }

    float azimuth = .46f, elevation = .12f, distance = 6.8f;
    DeviceOptions options;
    options.validation = validation;
    options.forceRasterShadows = rasterShadows;
    if (shadowBenchmark)
    {
        Renderer r(options);
        VkPhysicalDeviceProperties gpu{};
        vkGetPhysicalDeviceProperties(r.handles().physical, &gpu);
        std::cout << gpu.deviceName << " | " << shadowPresetName(preset) << '\n';
        printShadowTimings(std::cout, measureShadows(r, preset, {}), r.supportsRayQueryContactShadows());
        return 0;
    }
    if (compareShadows && capture)
    {
        Renderer r(options);
        captureShadowDemo(r, capture, preset, captureOffset);
        return 0;
    }
    auto architecture = compareShadows ? Scene{} : courtyard(asset);
    auto shapes = compareShadows ? Scene{} : primitives();
    if (benchmark)
    {
        Renderer r(options);
        std::vector<std::unique_ptr<View>> views;
        for (int count : {1, 4, 8})
        {
            while (int(views.size()) < count)
                views.push_back(r.createView(960, 600, 512));

            {
                RenderOptions o;
                auto batch = [&](unsigned frame)
                {
                    auto snap = architecture.snapshot(double(frame) / 30);
                    for (int i = 0; i < count; ++i)
                        r.render(snap, galleryCamera(azimuth + .12f * i + .002f * frame, elevation, distance),
                                 *views[i], o);
                };
                for (int i = 0; i < 4; ++i)
                    batch(i);
                for (auto& view : views)
                    view->wait();
                auto start = std::chrono::steady_clock::now();
                for (int i = 4; i < 124; ++i)
                    batch(i);
                for (auto& view : views)
                    view->wait();
                auto ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() /
                    120;
                std::cout << count << " views, ms/batch=" << ms << ", camera frames/s=" << 1000 * count / ms
                          << '\n';
            }
        }
        return 0;
    }
    if (capture)
    {
        Renderer r(options);
        auto view = r.createView(1280, 800);
        auto snap = architecture.snapshot(0);
        RenderOptions render;
        for (int i = 0; i < 16; ++i)
        {
            render.readback = i == 15;
            r.render(snap, galleryCamera(azimuth, elevation, distance), *view, render);
        }
        save(view->read(), capture);
        std::cout << "Saved " << capture << '\n';
        return 0;
    }
    if (!glfwInit())
        throw std::runtime_error("GLFW unavailable");
    struct Quit
    {
        ~Quit()
        {
            glfwTerminate();
        }
    } quit;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, smoke ? GLFW_FALSE : GLFW_TRUE);
    std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)> window(
        glfwCreateWindow(1440, 830, "Aperture | Simulation Renderer", nullptr, nullptr), glfwDestroyWindow);
    if (!window)
        throw std::runtime_error("Cannot create window");
    uint32_t count;
    auto names = glfwGetRequiredInstanceExtensions(&count);
    if (!names)
        throw std::runtime_error("GLFW Vulkan extensions unavailable");
    options.instanceExtensions.assign(names, names + count);
    options.createSurface = [&](VkInstance instance)
    {
        VkSurfaceKHR surface;
        check(glfwCreateWindowSurface(instance, window.get(), nullptr, &surface));
        return surface;
    };
    Renderer renderer(options);
    if (compareShadows)
        return shadowDemo(renderer, window.get(), smoke, preset);
    std::unique_ptr<View> view;
    int viewWidth = 0, viewHeight = 0;
    Presenter presenter(renderer.handles(), window.get());
    VkDescriptorSet image{};
    RenderOptions render;
    int sceneIndex = 0, frames = 0;
    int imageChannel = 0;
    bool orbit = false;
    float sunlight = 1;
    while (!glfwWindowShouldClose(window.get()) && (!smoke || frames < 12))
    {
        glfwPollEvents();
        if (smoke && frames == 3)
            render.debugChannel = 3;
        if (smoke && frames == 5)
            render.debugChannel = 0;
        if (smoke && frames == 6)
        {
            glfwSetWindowSize(window.get(), 1080, 680);
            sceneIndex = 1;
            distance = 12;
        }
        int fw, fh;
        glfwGetFramebufferSize(window.get(), &fw, &fh);
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
        auto& io = ImGui::GetIO();
        float panel = std::min(274.f, io.DisplaySize.x * .4f);
        // 视图与窗口物理像素一一对应，避免缩放和高 DPI 下对边缘二次采样。
        float sx = fw / io.DisplaySize.x;
        float sy = fh / io.DisplaySize.y;
        int left = int(std::round(panel * sx));
        int available = std::max(1, fw - left);
        int width = std::min(available, std::max(1, int(std::round(fh * 1.6))));
        int height = std::clamp(int(std::round(width / 1.6)), 1, fh);
        if (width != viewWidth || height != viewHeight || render.debugChannel != imageChannel)
        {
            if (image)
            {
                check(vkDeviceWaitIdle(renderer.handles().device));
                ImGui_ImplVulkan_RemoveTexture(image);
            }
            if (width != viewWidth || height != viewHeight)
            {
                view = renderer.createView(width, height);
                viewWidth = width;
                viewHeight = height;
            }
            imageChannel = render.debugChannel;
            // 窗口只消费输出：Beauty 与调试图是两张图，选择调试通道时换采样目标，
            // 而不是让调试覆盖同帧的真实颜色。
            image = ImGui_ImplVulkan_AddTexture(renderer.displaySampler(),
                                                imageChannel ? view->debugView() : view->colorView(),
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({panel, io.DisplaySize.y});
        ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextUnformatted("Aperture");
        ImGui::SetWindowFontScale(1);
        caption("SCENE");
        ImGui::SetNextItemWidth(-1);
        bool cut = ImGui::Combo("##scene", &sceneIndex, "Camera courtyard\0Geometry collection\0");
        if (cut)
            distance = sceneIndex ? 12.f : 6.8f;
        ImGui::Checkbox("Orbit camera", &orbit);
        caption("OUTPUT");
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##channel", &render.debugChannel,
                     "Beauty\0World normals\0Metric depth\0Instance IDs\0Shadow cascades\0");
        ImGui::Checkbox("Temporal AA", &render.taa);
        ImGui::Checkbox("Directional shadows", &render.shadows);
        if (renderer.supportsRayQueryContactShadows())
            ImGui::Checkbox("Ray Query contact", &render.rayQueryContactShadows);
        if (render.shadows)
        {
            ImGui::SliderFloat("Sun angular radius (rad)", &render.sunAngularRadius, 0.f, 0.06f, "%.3f");
            ImGui::SliderFloat("Contact shadow length (m)", &render.contactShadowLength, 0.f, 1.f, "%.2f");
        }
        ImGui::Checkbox("Frustum culling", &render.culling);
        caption("LIGHT");
        ImGui::SliderFloat("Exposure", &render.exposure, .25f, 2.5f, "%.2f");
        ImGui::SliderFloat("Sun power", &sunlight, 0.f, 2.f, "%.2f");
        caption("CAMERA");
        ImGui::TextWrapped("Drag the image to orbit. Scroll to change distance.");
        ImGui::Text("%u visible meshes", view->visibleMeshes());
        ImGui::Text("%.1f fps", io.Framerate);
        render.readback = ImGui::Button("Save color frame");
        ImGui::End();
        int x = left + (available - width) / 2;
        int y = (fh - height) / 2;
        ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)(uintptr_t)image, {x / sx, y / sy},
                                                 {(x + width) / sx, (y + height) / sy});
        if (io.MousePos.x >= panel && !io.WantCaptureMouse)
        {
            if (ImGui::IsMouseDown(0))
            {
                azimuth -= io.MouseDelta.x * .007f;
                elevation = std::clamp(elevation + io.MouseDelta.y * .005f, -.08f, 1.1f);
            }
            distance = std::clamp(distance - io.MouseWheel * .55f, 2.8f, 18.f);
        }
        if (orbit)
            azimuth += io.DeltaTime * .12f;
        auto scene = (sceneIndex ? shapes : architecture).snapshot(glfwGetTime());
        scene.sun.radiance *= sunlight;
        render.resetHistory = cut;
        renderer.render(scene, galleryCamera(azimuth, elevation, distance), *view, render);
        if (render.readback)
            save(view->read(), "gallery.png");
        ImGui::Render();
        presenter.present();
        ++frames;
    }
    check(vkDeviceWaitIdle(renderer.handles().device));
    ImGui_ImplVulkan_RemoveTexture(image);
    return 0;
}
}
int main(int argc, char** argv)
try
{
    return gallery(argc, argv);
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
