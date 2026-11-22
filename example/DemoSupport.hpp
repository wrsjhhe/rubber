#pragma once
#include "sim/Renderer.hpp"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

namespace demo
{
void check(VkResult);
void save(const sim::Frame&, const char* file);
void caption(const char* text);

struct Presenter
{
    const sim::DeviceHandles& d;
    ImGui_ImplVulkanH_Window window{};
    bool rebuild = false;
    Presenter(const sim::DeviceHandles& device, GLFWwindow* glfw);
    ~Presenter();
    void resize(int w, int h);
    void present();
};

class Window
{
  public:
    Window(const char* title, bool smoke);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    GLFWwindow* get() const;
    void configure(sim::DeviceOptions&) const;

  private:
    GLFWwindow* handle{};
};
}
