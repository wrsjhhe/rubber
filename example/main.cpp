#include "Gallery.hpp"
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

int gallery(int argc, char** argv)
{
    bool smoke = false, benchmark = false;
    DeviceOptions options;
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
            options.validation = true;
        else if (arg == "--raster-shadows")
            options.forceRasterShadows = true;
        else if (arg == "--model" && i + 1 < argc)
            asset = argv[++i];
        else if (arg == "--capture" && i + 1 < argc)
            capture = argv[++i];
        else
            throw std::invalid_argument("Usage: gallery [--model asset.glb] [--capture file.png] [--smoke] "
                                        "[--benchmark] [--validation] [--raster-shadows]");
    }

    auto architecture = courtyard(asset);
    auto shapes = primitives();
    float azimuth = .46f, elevation = .12f, distance = 6.8f;
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
    Window window("Aperture | Gallery", smoke);
    window.configure(options);
    Renderer renderer(options);
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
