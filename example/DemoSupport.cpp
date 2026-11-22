#include "DemoSupport.hpp"
#include <filesystem>
#include <stdexcept>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace demo
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

Presenter::Presenter(const DeviceHandles& device, GLFWwindow* glfw) : d(device)
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
Presenter::~Presenter()
{
    vkDeviceWaitIdle(d.device);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    // 表面由设备上下文持有。
    window.Surface = VK_NULL_HANDLE;
    ImGui_ImplVulkanH_DestroyWindow(d.instance, d.device, &window, nullptr);
}
void Presenter::resize(int w, int h)
{
    ImGui_ImplVulkanH_CreateOrResizeWindow(d.instance, d.physical, d.device, &window, d.family, nullptr, w, h,
                                           2);
    window.FrameIndex = 0;
    rebuild = false;
}
void Presenter::present()
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

void caption(const char* text)
{
    ImGui::Spacing();
    ImGui::TextColored({.96f, .71f, .42f, 1}, "%s", text);
    ImGui::Separator();
}

Window::Window(const char* title, bool smoke)
{
    if (!glfwInit())
        throw std::runtime_error("GLFW unavailable");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, smoke ? GLFW_FALSE : GLFW_TRUE);
    handle = glfwCreateWindow(1440, 830, title, nullptr, nullptr);
    if (!handle)
    {
        glfwTerminate();
        throw std::runtime_error("Cannot create window");
    }
}

Window::~Window()
{
    glfwDestroyWindow(handle);
    glfwTerminate();
}

GLFWwindow* Window::get() const
{
    return handle;
}

void Window::configure(DeviceOptions& options) const
{
    uint32_t count = 0;
    const auto names = glfwGetRequiredInstanceExtensions(&count);
    if (!names)
        throw std::runtime_error("GLFW Vulkan extensions unavailable");
    options.instanceExtensions.assign(names, names + count);
    options.createSurface = [window = handle](VkInstance instance)
    {
        VkSurfaceKHR surface{};
        check(glfwCreateWindowSurface(instance, window, nullptr, &surface));
        return surface;
    };
}
}
