#include "SensorDemo.hpp"
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace sim;

namespace
{
constexpr uint32_t width = 640;
constexpr uint32_t height = 360;
constexpr float boardDepth = 4.5f;
constexpr double tick = 1.0 / 60.0;
constexpr std::array channels{Channel::Color, Channel::MetricDepth, Channel::InstanceId};
constexpr const char* cameraNames[] = {"A  |  Reference", "B  |  Oblique"};

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void check(VkResult result)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error("Sensor preview upload: VkResult " + std::to_string(result));
}

glm::mat4 at(glm::vec3 position)
{
    return glm::translate(glm::mat4(1), position);
}

MeshStandardMaterial matte(glm::vec3 color)
{
    MeshStandardMaterial material;
    material.color = color;
    material.roughness = .76f;
    return material;
}

template <class T>
T pixel(const OutputData& image, unsigned x, unsigned y)
{
    T value{};
    std::memcpy(&value, image.bytes.data() + y * image.info.rowBytes + x * sizeof(T), sizeof(T));
    return value;
}

std::vector<uint8_t> preview(const OutputData& data, unsigned channel)
{
    if (channel == 0)
        return data.bytes;

    // CPU 派生的预览只用于显示，像素检查仍读取原始浮点深度或整数标签。
    const std::array<float, 3> nearColor{251.f, 183.f, 82.f};
    const std::array<float, 3> farColor{42.f, 117.f, 180.f};
    std::vector<uint8_t> rgba(size_t(width) * height * 4);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
        {
            auto* dest = rgba.data() + (size_t(y) * width + x) * 4;
            dest[0] = 16;
            dest[1] = 24;
            dest[2] = 33;
            dest[3] = 255;
            if (channel == 1)
            {
                const float depth = pixel<float>(data, x, y);
                if (!(depth > 0))
                    continue;
                const float t = std::clamp((depth - 1.f) / 7.f, 0.f, 1.f);
                for (unsigned c = 0; c < 3; ++c)
                    dest[c] = uint8_t(std::lerp(nearColor[c], farColor[c], t));
            }
            else
            {
                const uint32_t id = pixel<uint32_t>(data, x, y);
                if (!id)
                    continue;
                uint32_t hash = id * 0x9e3779b9u;
                hash ^= hash >> 16;
                for (unsigned c = 0; c < 3; ++c)
                    dest[c] = uint8_t(75 + ((hash >> (8 * c)) & 255) * 175 / 255);
            }
        }
    return rgba;
}

class PreviewTexture
{
  public:
    explicit PreviewTexture(const DeviceHandles& handles) : d(handles) {}
    PreviewTexture(const PreviewTexture&) = delete;
    ~PreviewTexture()
    {
        if (submitted)
            vkQueueWaitIdle(d.queue);
        if (descriptor)
            ImGui_ImplVulkan_RemoveTexture(descriptor);
        vkDestroyFence(d.device, fence, nullptr);
        vkDestroyCommandPool(d.device, pool, nullptr);
        if (mapped)
            vkUnmapMemory(d.device, uploadMemory);
        vkDestroyBuffer(d.device, staging, nullptr);
        vkFreeMemory(d.device, uploadMemory, nullptr);
        vkDestroyImageView(d.device, view, nullptr);
        vkDestroyImage(d.device, image, nullptr);
        vkFreeMemory(d.device, imageMemory, nullptr);
    }

    void init(VkSampler sampler)
    {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType = VK_IMAGE_TYPE_2D;
        create.format = VK_FORMAT_R8G8B8A8_UNORM;
        create.extent = {width, height, 1};
        create.mipLevels = create.arrayLayers = 1;
        create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        check(vkCreateImage(d.device, &create, nullptr, &image));
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(d.device, image, &req);
        imageMemory = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkBindImageMemory(d.device, image, imageMemory, 0));

        VkImageViewCreateInfo imageView{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        imageView.image = image;
        imageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
        imageView.format = create.format;
        imageView.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(d.device, &imageView, nullptr, &view));
        descriptor = ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        require(descriptor != VK_NULL_HANDLE, "Cannot register sensor preview texture");

        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = bytes;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateBuffer(d.device, &buffer, nullptr, &staging));
        vkGetBufferMemoryRequirements(d.device, staging, &req);
        uploadMemory = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkBindBufferMemory(d.device, staging, uploadMemory, 0));
        check(vkMapMemory(d.device, uploadMemory, 0, bytes, 0, &mapped));

        VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        commands.queueFamilyIndex = d.family;
        commands.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(d.device, &commands, nullptr, &pool));
        VkCommandBufferAllocateInfo list{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        list.commandPool = pool;
        list.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(d.device, &list, &cmd));
        VkFenceCreateInfo completion{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(d.device, &completion, nullptr, &fence));
    }

    void update(const std::vector<uint8_t>& rgba)
    {
        require(rgba.size() == bytes, "Unexpected sensor preview size");
        if (submitted)
        {
            check(vkWaitForFences(d.device, 1, &fence, VK_TRUE, UINT64_MAX));
            check(vkResetFences(d.device, 1, &fence));
        }
        std::memcpy(mapped, rgba.data(), bytes);
        check(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cmd, &begin));

        // 窗口和上传在同一队列。覆盖前等待上一轮 ImGui 采样，写入后再供本轮采样。
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = submitted ? VK_ACCESS_SHADER_READ_BIT : 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = submitted ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, submitted ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        check(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        check(vkQueueSubmit(d.queue, 1, &submit, fence));
        submitted = true;
    }

    ImTextureID id() const
    {
        return (ImTextureID)(uintptr_t)descriptor;
    }

  private:
    VkDeviceMemory allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags flags) const
    {
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(d.physical, &memory);
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        {
            if (!(req.memoryTypeBits & (1u << i)) || (memory.memoryTypes[i].propertyFlags & flags) != flags)
                continue;
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = i;
            VkDeviceMemory result{};
            check(vkAllocateMemory(d.device, &alloc, nullptr, &result));
            return result;
        }
        throw std::runtime_error("No memory type for sensor preview");
    }

    const DeviceHandles& d;
    static constexpr size_t bytes = size_t(width) * height * 4;
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory imageMemory{}, uploadMemory{};
    VkBuffer staging{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkDescriptorSet descriptor{};
    void* mapped = nullptr;
    bool submitted = false;
};

struct CameraSample
{
    PerspectiveCamera camera;
    std::unique_ptr<View> view;
    std::shared_ptr<Observation> observation;
    std::array<OutputData, 3> data;
    std::unique_ptr<PreviewTexture> texture;
    unsigned x = width / 2, y = height / 2;
    int shown = 0;
    bool dirty = true;
};

std::string listPasses(const View& view)
{
    std::string names;
    for (const auto& pass : view.activePasses())
    {
        if (!names.empty())
            names += " > ";
        names += pass;
    }
    return names;
}
}

struct SensorDemo::Impl
{
    Renderer& renderer;
    Scene scene;
    std::array<CameraSample, 2> cameras;
    uint32_t board{}, cart{};
    double time = 0;
    bool paused = false, truth = false, sampled = false;

    explicit Impl(Renderer& renderer) : renderer(renderer)
    {
        const auto navy = matte({.09f, .16f, .21f});
        const auto ground = matte({.30f, .36f, .39f});
        const auto cream = matte({.82f, .79f, .66f});
        const auto teal = matte({.11f, .46f, .44f});
        const auto orange = matte({.91f, .29f, .065f});
        const auto cube = BoxGeometry();
        auto block = [&](glm::vec3 p, glm::vec3 size, const MeshStandardMaterial& material, uint32_t parent = 0)
        {
            return scene.add(cube, material, glm::scale(at(p), size), parent);
        };
        block({0, -.10f, -.3f}, {9, .2f, 6}, ground);
        block({0, 1.85f, -2.65f}, {8.5f, 3.7f, .2f}, navy);
        for (int i = -4; i <= 4; ++i)
        {
            block({float(i), .004f, 0}, {.012f, .008f, 5.6f}, navy);
            block({0, .006f, float(i) * .58f}, {8.5f, .008f, .012f}, navy);
            block({float(i), 2.6f, -2.53f}, {.48f, .045f, .018f}, teal);
        }

        // 板的正面位于世界 z=0，A 相机正对板，解析光轴深度始终为 4.5 m。
        board = block({0, 1.42f, -.05f}, {1.62f, .88f, .10f}, cream);
        for (float x : {-.83f, .83f})
            block({x, .91f, -.10f}, {.065f, 1.83f, .11f}, navy);
        for (float y : {.99f, 1.86f})
            block({0, y, .006f}, {1.63f, .035f, .025f}, teal);
        for (int i = -3; i <= 3; ++i)
            block({i * .21f, 1.72f, .006f}, {.012f, .10f, .014f}, navy);
        for (float x : {-.68f, .68f})
            block({x, 1.42f, .008f}, {.08f, .08f, .015f}, orange);
        block({0, .015f, .8f}, {5.8f, .018f, .87f}, navy);
        for (int i = -5; i <= 5; ++i)
            block({i * .46f, .03f, 1.17f}, {.23f, .014f, .034f}, cream);

        cart = scene.addGroup();
        block({0, .29f, 0}, {.79f, .40f, .57f}, orange, cart);
        block({0, .53f, -.07f}, {.50f, .12f, .34f}, teal, cart);
        const auto wheel = CylinderGeometry(.13f, .13f, .075f, 20);
        for (float x : {-.27f, .27f})
            for (float z : {-.30f, .30f})
                scene.add(wheel, navy, glm::rotate(at({x, .14f, z}), 1.5707963f, glm::vec3(1, 0, 0)), cart);
        block({-2.2f, .39f, -.55f}, {.42f, .78f, .45f}, teal);
        block({2.18f, .56f, -.62f}, {.49f, 1.12f, .48f}, teal);

        auto& a = cameras[0].camera;
        a.position = {0, 1.42f, boardDepth};
        a.target = {0, 1.42f, 0};
        a.fx = 480;
        a.fy = 420;
        a.cx = width * .5f;
        a.cy = height * .5f;
        auto& b = cameras[1].camera;
        b.position = {3.55f, 2.55f, 4.2f};
        b.target = {0, 1.f, -.10f};
        b.fx = 495;
        b.fy = 465;
        b.cx = 304;
        b.cy = 182;
        for (auto& camera : cameras)
            camera.view = renderer.createView(width, height, 512);
    }

    void sample()
    {
        scene.get(cart).local = at({2.15f * float(std::sin(time * 1.25)), .025f, .79f});
        const auto snapshot = scene.snapshot(time);
        std::vector<OutputDesc> outputs{{Channel::MetricDepth, Delivery::Cpu}, {Channel::InstanceId, Delivery::Cpu}};
        if (!truth)
            outputs.insert(outputs.begin(), {Channel::Color, Delivery::Cpu});

        // 两台相机先提交同一个快照，再逐个读回，展示共享场景与独立观测。
        for (auto& camera : cameras)
        {
            auto observation = renderer.observe({snapshot, camera.camera, *camera.view, outputs, {}, SampleTime{time, 0}});
            require(observation.status == SubmitStatus::Submitted, "Dual camera observation returned Busy");
            camera.observation = std::move(observation.observation);
        }
        for (auto& camera : cameras)
        {
            for (unsigned i = truth ? 1u : 0u; i < channels.size(); ++i)
                camera.data[i] = camera.observation->read(channels[i]);
            if (truth)
            {
                camera.data[0] = {};
                if (camera.shown == 0)
                    camera.shown = 1;
            }
            camera.dirty = true;
        }
        sampled = true;
    }

    void verify()
    {
        time = 0;
        truth = false;
        const auto before = renderer.assetStats();
        sample();
        const auto after = renderer.assetStats();
        require(after.scenePreparations == before.scenePreparations + 1 && after.sceneReuses >= before.sceneReuses + 1,
                "Dual camera demo did not share its snapshot");
        const auto& a = cameras[0];
        const auto& b = cameras[1];
        require(a.observation->calibration() != b.observation->calibration(), "Demo cameras share the same calibration");
        require(a.observation->sampleTime().time == b.observation->sampleTime().time, "Demo camera times disagree");
        require(pixel<uint32_t>(a.data[2], width / 2, height / 2) == board, "Reference center missed the board");
        require(std::abs(pixel<float>(a.data[1], width / 2, height / 2) - boardDepth) < .001f,
                "Reference board has the wrong metric depth");
        const auto rgbPasses = a.view->activePasses();
        require(std::find(rgbPasses.begin(), rgbPasses.end(), "lighting") != rgbPasses.end(),
                "Full observation did not execute lighting");
        const std::array depth{a.data[1].bytes, b.data[1].bytes};
        const std::array ids{a.data[2].bytes, b.data[2].bytes};

        truth = true;
        sample();
        for (unsigned i = 0; i < cameras.size(); ++i)
        {
            const auto& camera = cameras[i];
            require(!camera.observation->has(Channel::Color), "Truth-only observation produced RGB");
            require(camera.view->activePasses() == std::vector<std::string>{"geometry"},
                    "Truth-only observation executed extra render passes");
            require(depth[i] == camera.data[1].bytes && ids[i] == camera.data[2].bytes,
                    "Request mode changed geometry truth");
        }
        time = 1;
        sample();
        require(cameras[0].data[2].bytes != ids[0] && cameras[1].data[2].bytes != ids[1],
                "Moving cart did not update both observations");
        std::cout << "PASS: one shared snapshot, distinct calibration, 4.500 m board, motion and truth-only pass pruning\n";
    }

    void panel(CameraSample& camera, unsigned index, float imageWidth)
    {
        const auto& calibration = camera.observation->calibration();
        ImGui::PushID(int(index));
        ImGui::BeginGroup();
        ImGui::TextUnformatted(cameraNames[index]);
        const auto mode = [&](const char* label, int channel)
        {
            ImGui::BeginDisabled(truth && channel == 0);
            if (ImGui::RadioButton(label, camera.shown == channel) && camera.shown != channel)
            {
                camera.shown = channel;
                camera.dirty = true;
            }
            ImGui::EndDisabled();
        };
        mode("RGB", 0);
        ImGui::SameLine();
        mode("Metric Z", 1);
        ImGui::SameLine();
        mode("Instance ID", 2);
        if (!camera.texture)
        {
            camera.texture = std::make_unique<PreviewTexture>(renderer.handles());
            camera.texture->init(renderer.displaySampler());
        }
        if (camera.dirty)
        {
            camera.texture->update(preview(camera.data[camera.shown], unsigned(camera.shown)));
            camera.dirty = false;
        }

        const ImVec2 size{imageWidth, imageWidth * float(height) / float(width)};
        ImGui::Image(camera.texture->id(), size);
        const auto top = ImGui::GetItemRectMin();
        if (ImGui::IsItemClicked())
        {
            const auto mouse = ImGui::GetMousePos();
            camera.x = unsigned(std::clamp(int((mouse.x - top.x) * width / size.x), 0, int(width) - 1));
            camera.y = unsigned(std::clamp(int((mouse.y - top.y) * height / size.y), 0, int(height) - 1));
        }
        const ImVec2 p{top.x + (camera.x + .5f) * size.x / width, top.y + (camera.y + .5f) * size.y / height};
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddCircle(p, 7, IM_COL32(12, 18, 25, 255), 0, 3);
        draw->AddCircle(p, 7, IM_COL32(250, 191, 81, 255), 0, 1.5f);
        draw->AddCircleFilled(p, 1.7f, IM_COL32(250, 191, 81, 255));

        const float depth = pixel<float>(camera.data[1], camera.x, camera.y);
        const auto instance = pixel<uint32_t>(camera.data[2], camera.x, camera.y);
        ImGui::Text("t %.3f s  |  pixel (%u, %u)", camera.observation->sampleTime().time, camera.x, camera.y);
        if (depth > 0)
            ImGui::Text("Z %.3f m  |  ID %u%s", depth, instance, instance == board ? "  Calibration board" : "");
        else
            ImGui::TextUnformatted("No hit  |  ID 0");
        ImGui::TextDisabled("fx %.0f  fy %.0f  cx %.0f  cy %.0f px", calibration.fx, calibration.fy, calibration.cx,
                            calibration.cy);
        if (camera.observation->has(Channel::Color))
        {
            const auto rgb = pixel<std::array<uint8_t, 4>>(camera.data[0], camera.x, camera.y);
            ImGui::Text("RGB %u, %u, %u", unsigned(rgb[0]), unsigned(rgb[1]), unsigned(rgb[2]));
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + imageWidth);
        ImGui::Text("Passes: %s", listPasses(*camera.view).c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::PopID();
    }
};

SensorDemo::SensorDemo(Renderer& renderer) : impl(std::make_unique<Impl>(renderer)) {}
SensorDemo::~SensorDemo() = default;

void SensorDemo::draw(bool smoke, unsigned frame)
{
    auto& d = *impl;
    const auto size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("Dual camera capture", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
    ImGui::SetWindowFontScale(1.45f);
    ImGui::TextUnformatted("Dual camera capture");
    ImGui::SetWindowFontScale(1.f);
    ImGui::SameLine();
    ImGui::TextDisabled("640 x 360 per camera");
    bool refresh = !d.sampled;
    ImGui::Checkbox("Pause", &d.paused);
    ImGui::SameLine();
    if (ImGui::Button("Step"))
    {
        d.paused = true;
        d.time += tick;
        refresh = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset time"))
    {
        d.time = 0;
        refresh = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Truth only", &d.truth))
        refresh = true;
    ImGui::SameLine();
    ImGui::TextDisabled("Click either image to inspect a pixel");
    if (smoke)
    {
        d.time = frame * tick;
        d.truth = (frame % 4) >= 2;
        d.cameras[0].shown = d.truth ? 1 : 0;
        d.cameras[1].shown = 2;
        refresh = true;
    }
    if (d.sampled && !d.paused && !refresh)
    {
        d.time += tick;
        refresh = true;
    }
    if (refresh)
        d.sample();

    ImGui::Separator();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const auto room = ImGui::GetContentRegionAvail();
    const float imageWidth = std::max(1.f, std::min((room.x - spacing) * .5f, (room.y - 155.f) * width / height));
    d.panel(d.cameras[0], 0, imageWidth);
    ImGui::SameLine(0, spacing);
    d.panel(d.cameras[1], 1, imageWidth);
    ImGui::Separator();
    ImGui::TextDisabled("Reference center: board Z = 4.500 m. Depth colors: 1 m warm, 8 m cool, background dark.");
    const auto stats = d.renderer.assetStats();
    ImGui::TextDisabled("Shared scene preparations %llu  |  snapshot reuses %llu  |  mesh uploads %llu",
                        (unsigned long long)stats.scenePreparations, (unsigned long long)stats.sceneReuses,
                        (unsigned long long)stats.meshUploads);
    ImGui::End();
}

Frame SensorDemo::capture()
{
    auto& d = *impl;
    d.time = 0;
    d.truth = false;
    d.sample();
    Frame result{};
    result.width = width * 2;
    result.height = height * unsigned(channels.size());
    const size_t stride = size_t(width) * 4;
    result.rgba.reserve(size_t(result.width) * result.height * 4);
    for (unsigned i = 0; i < channels.size(); ++i)
    {
        const auto left = preview(d.cameras[0].data[i], i);
        const auto right = preview(d.cameras[1].data[i], i);
        for (unsigned y = 0; y < height; ++y)
            for (const auto* image : {&left, &right})
                result.rgba.insert(result.rgba.end(), image->begin() + y * stride, image->begin() + (y + 1) * stride);
    }
    return result;
}

void SensorDemo::verify()
{
    impl->verify();
}
