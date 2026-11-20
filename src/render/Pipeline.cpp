#include "Pipeline.hpp"
#include <fstream>
#include <stdexcept>
namespace sim::vk
{
VkShaderModule loadModule(Device& d, const std::filesystem::path& path, Lifetime& life)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    auto size = f.tellg();
    if (size <= 0 || size % 4)
        throw std::runtime_error("Invalid shader: " + path.string());
    std::vector<uint32_t> bytes(size_t(size) / 4);
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("Shader read failed");
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size_t(size);
    ci.pCode = bytes.data();
    VkShaderModule shader;
    check(vkCreateShaderModule(d.device, &ci, nullptr, &shader), "shader module");
    life.release.push_back([&d, shader] { vkDestroyShaderModule(d.device, shader, nullptr); });
    return shader;
}
VkPipeline graphicsPipeline(Device& d, Lifetime& life, const std::filesystem::path& dir, const char* vs,
                            const char* fs, VkPipelineLayout layout, std::span<const VkFormat> colors,
                            const GraphicsOptions& options)
{
    Lifetime temporary;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < (fs ? 2 : 1); ++i)
    {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = loadModule(d, dir / (std::string(i ? fs : vs) + ".spv"), temporary);
        stages[i].pName = "main";
    }
    VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)}};
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (options.vertexInput)
    {
        vertex.vertexBindingDescriptionCount = 1;
        vertex.pVertexBindingDescriptions = &binding;
        vertex.vertexAttributeDescriptionCount = fs ? 3 : 1;
        vertex.pVertexAttributeDescriptions = attributes;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.lineWidth = 1;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // 保持与既有渲染一致的"不剔除"：示例资产含单面几何，剔除会改变参考画面。
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.depthBiasEnable = options.depthBias;
    raster.depthBiasConstantFactor = 1.25f;
    raster.depthBiasSlopeFactor = 1.6f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo z{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    z.depthTestEnable = options.depthTest;
    z.depthWriteEnable = options.depthWrite;
    z.depthCompareOp = options.depthCompare;
    std::vector<VkPipelineColorBlendAttachmentState> masks(colors.size());
    for (auto& m : masks)
        m.colorWriteMask = 15;
    if (options.blendIndex >= 0 && size_t(options.blendIndex) < masks.size())
    {
        // 预乘 alpha：点云按覆盖度叠加到已光照的颜色上，边缘平滑过渡。
        auto& m = masks[size_t(options.blendIndex)];
        m.blendEnable = VK_TRUE;
        m.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        m.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        m.colorBlendOp = VK_BLEND_OP_ADD;
        m.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        m.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        m.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = uint32_t(masks.size());
    blend.pAttachments = masks.data();
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = uint32_t(colors.size());
    rendering.pColorAttachmentFormats = colors.data();
    rendering.depthAttachmentFormat = options.depthTest ? depthFormat : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    ci.stageCount = fs ? 2 : 1;
    ci.pStages = stages;
    ci.pVertexInputState = &vertex;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &z;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dyn;
    ci.layout = layout;
    VkPipeline result;
    check(vkCreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &ci, nullptr, &result), "graphics pipeline");
    life.release.push_back([&d, result] { vkDestroyPipeline(d.device, result, nullptr); });
    return result;
}
Pipelines::Pipelines(Device& device, const std::filesystem::path& dir) : d(device)
{
    auto setLayout = [&](unsigned n, bool buffers)
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings(n);
        for (unsigned i = 0; i < n; ++i)
        {
            auto type = buffers ? (i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                                : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i] = {i, type, 1,
                           VkShaderStageFlags(buffers
                                                  ? (VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT)
                                                  : VK_SHADER_STAGE_FRAGMENT_BIT),
                           nullptr};
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = n;
        ci.pBindings = bindings.data();
        VkDescriptorSetLayout h;
        check(vkCreateDescriptorSetLayout(d.device, &ci, nullptr, &h), "descriptor layout");
        life.release.push_back([this, h] { vkDestroyDescriptorSetLayout(d.device, h, nullptr); });
        return h;
    };
    frame = setLayout(3, true);
    material = setLayout(3, false);
    lighting = setLayout(7, false);
    resolve = setLayout(4, false);
    colorSet = setLayout(1, false);
    {
        // 点云实例缓冲：只读存储缓冲，顶点与片元都要读。
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        check(vkCreateDescriptorSetLayout(d.device, &ci, nullptr, &points), "descriptor layout");
        life.release.push_back([this] { vkDestroyDescriptorSetLayout(d.device, points, nullptr); });
    }
    auto layout = [&](VkDescriptorSetLayout second)
    {
        VkDescriptorSetLayout sets[] = {frame, second};
        VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 12};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 2;
        ci.pSetLayouts = sets;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &range;
        VkPipelineLayout h;
        check(vkCreatePipelineLayout(d.device, &ci, nullptr, &h), "pipeline layout");
        life.release.push_back([this, h] { vkDestroyPipelineLayout(d.device, h, nullptr); });
        return h;
    };
    meshLayout = layout(material);
    lightLayout = layout(lighting);
    resolveLayout = layout(resolve);
    colorLayout = layout(colorSet);
    pointsLayout = layout(points);
    GraphicsOptions mesh;
    GraphicsOptions zOnly;
    zOnly.depthBias = true;
    GraphicsOptions fullscreen;
    fullscreen.vertexInput = false;
    fullscreen.depthTest = fullscreen.depthWrite = false;
    GraphicsOptions splatState;
    splatState.vertexInput = false;
    // 点云与网格互相遮挡依赖同一张深度图：点在后被网格挡住，点在前覆盖网格，
    // 因此比较用 LESS_OR_EQUAL 并在通过时写深度。
    splatState.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    splatState.blendIndex = 5;
    shadow = graphicsPipeline(d, life, dir, "shadow.vert", nullptr, meshLayout, {}, zOnly);
    geometry = graphicsPipeline(d, life, dir, "geometry.vert", "geometry.frag", meshLayout, gFormats, mesh);
    shade = graphicsPipeline(d, life, dir, "fullscreen.vert", "lighting.frag", lightLayout, std::array{hdrFormat},
                             fullscreen);
    temporal =
        graphicsPipeline(d, life, dir, "fullscreen.vert", "temporal.frag", resolveLayout, std::array{hdrFormat},
                         fullscreen);
    color = graphicsPipeline(d, life, dir, "fullscreen.vert", "color.frag", colorLayout, std::array{colorFormat},
                             fullscreen);
    // 点云写 HDR 与全部 G-buffer：深度、ID、米制深度与速度必须与网格一致，
    // 否则同一像素的几何真值与颜色会分属不同物体。
    const std::array<VkFormat, 6> splatFormats{colorFormat, hdrFormat, hdrFormat, VK_FORMAT_R32_SFLOAT,
                                               VK_FORMAT_R32_UINT, hdrFormat};
    splat = graphicsPipeline(d, life, dir, "points.vert", "points.frag", pointsLayout, splatFormats, splatState);
    // 几何真值 pass 没有 HDR 附件：同一份点云只重画 G-buffer 与深度。
    GraphicsOptions splatDepthState;
    splatDepthState.vertexInput = false;
    splatDepthState.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    splatDepth = graphicsPipeline(d, life, dir, "points.vert", "points.depth.frag", pointsLayout,
                                 std::span<const VkFormat>(splatFormats).first(5), splatDepthState);
    auto sampler = [&](VkFilter filter, VkSamplerAddressMode mode)
    {
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = ci.minFilter = filter;
        ci.mipmapMode =
            filter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.maxLod = VK_LOD_CLAMP_NONE;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = mode;
        VkSampler h;
        check(vkCreateSampler(d.device, &ci, nullptr, &h), "sampler");
        life.release.push_back([this, h] { vkDestroySampler(d.device, h, nullptr); });
        return h;
    };
    point = sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    linear = sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    repeat = sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
}
VkDescriptorPool descriptorPool(Device& d, Lifetime& life, uint32_t count)
{
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count * 3},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count * 7}};
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = count;
    ci.poolSizeCount = 3;
    ci.pPoolSizes = sizes;
    VkDescriptorPool h;
    check(vkCreateDescriptorPool(d.device, &ci, nullptr, &h), "descriptor pool");
    life.release.push_back([&d, h] { vkDestroyDescriptorPool(d.device, h, nullptr); });
    return h;
}
VkDescriptorSet allocateSet(Device& d, VkDescriptorPool pool, VkDescriptorSetLayout layout)
{
    VkDescriptorSetAllocateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ci.descriptorPool = pool;
    ci.descriptorSetCount = 1;
    ci.pSetLayouts = &layout;
    VkDescriptorSet set;
    check(vkAllocateDescriptorSets(d.device, &ci, &set), "descriptor set");
    return set;
}
void bindImage(Device& d, VkDescriptorSet set, uint32_t binding, const Image& img, VkSampler sampler)
{
    VkDescriptorImageInfo image{sampler, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &image;
    vkUpdateDescriptorSets(d.device, 1, &w, 0, nullptr);
}
void bindBuffer(Device& d, VkDescriptorSet set, uint32_t binding, const Buffer& buf, VkDescriptorType type)
{
    VkDescriptorBufferInfo info{buf.handle, 0, buf.size};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pBufferInfo = &info;
    vkUpdateDescriptorSets(d.device, 1, &w, 0, nullptr);
}
void viewport(VkCommandBuffer cmd, int x, int y, uint32_t w, uint32_t h)
{
    VkViewport v{float(x), float(y), float(w), float(h), 0, 1};
    VkRect2D s{{x, y}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &s);
}
void beginPass(VkCommandBuffer cmd, std::span<Image* const> colors, Image* depth, bool load)
{
    // 附件描述按上限静态分配：点云的六附件 pass 与网格的五附件 pass 共用这条入口，
    // 超出上限说明调用方声明了设备根本不允许的帧，直接拒绝而不是写坏栈。
    if (colors.size() > maxColorAttachments)
        throw std::invalid_argument("Too many color attachments");
    auto attach = [load](Image* img, bool z)
    {
        VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = img->view;
        a.imageLayout = img->layout;
        // 保留模式用于在已有结果上叠加：点云必须在网格光照之上混合，且沿用已写入的深度。
        a.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (z && !load)
            a.clearValue.depthStencil.depth = 1;
        return a;
    };
    std::array<VkRenderingAttachmentInfo, maxColorAttachments> attachments{};
    for (size_t i = 0; i < colors.size(); ++i)
        attachments[i] = attach(colors[i], false);
    VkRenderingAttachmentInfo z{};
    if (depth)
        z = attach(depth, true);
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea.extent = (depth ? depth : colors.front())->extent;
    info.layerCount = 1;
    info.colorAttachmentCount = uint32_t(colors.size());
    info.pColorAttachments = attachments.data();
    info.pDepthAttachment = depth ? &z : nullptr;
    vkCmdBeginRendering(cmd, &info);
    viewport(cmd, 0, 0, info.renderArea.extent.width, info.renderArea.extent.height);
}
}
