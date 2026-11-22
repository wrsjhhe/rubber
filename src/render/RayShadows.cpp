#include "RenderScene.hpp"
#include "Module.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace sim::vk
{
struct Acceleration
{
    Device& device;
    Buf storage;
    VkAccelerationStructureKHR handle{};

    explicit Acceleration(Device& d) : device(d)
    {
    }

    ~Acceleration()
    {
        if (handle)
            device.destroyAcceleration(device.device, handle, nullptr);
    }

    VkDeviceAddress address() const
    {
        VkAccelerationStructureDeviceAddressInfoKHR info{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        info.accelerationStructure = handle;
        return device.accelerationAddress(device.device, &info);
    }
};

namespace
{
void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 source, VkAccessFlags2 sourceAccess,
             VkPipelineStageFlags2 target, VkAccessFlags2 targetAccess)
{
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memory.srcStageMask = source;
    memory.srcAccessMask = sourceAccess;
    memory.dstStageMask = target;
    memory.dstAccessMask = targetAccess;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

std::shared_ptr<Acceleration> build(Device& d, VkCommandBuffer cmd, VkAccelerationStructureTypeKHR type,
                                    const VkAccelerationStructureGeometryKHR& geometry, uint32_t count,
                                    Buf& scratch)
{
    VkAccelerationStructureBuildGeometryInfoKHR info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = type;
    info.flags = type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                     ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
                     : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    d.accelerationBuildSizes(d.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sizes);
    auto result = std::make_shared<Acceleration>(d);
    result->storage = d.buffer(sizes.accelerationStructureSize,
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
    VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    create.buffer = result->storage->handle;
    create.size = sizes.accelerationStructureSize;
    create.type = type;
    check(d.createAcceleration(d.device, &create, nullptr, &result->handle), "create acceleration structure");

    const VkDeviceSize alignment = d.accelerationLimits.minAccelerationStructureScratchOffsetAlignment;
    const auto scratchSize = sizes.buildScratchSize + alignment;
    if (!scratch || scratch->size < scratchSize)
        scratch = d.buffer(scratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                           false);
    const auto address = d.address(*scratch);
    info.scratchData.deviceAddress = (address + alignment - 1) & ~(alignment - 1);
    info.dstAccelerationStructure = result->handle;
    VkAccelerationStructureBuildRangeInfoKHR range{count};
    const auto* ranges = &range;
    d.buildAcceleration(cmd, 1, &info, &ranges);
    return result;
}
}

VkAccelerationStructureKHR RenderScene::shadowAcceleration(Device& d, VkCommandBuffer cmd, FrameLifetime& work,
                                                           ShadowBuildBuffers& buffers) const
{
    if (shadowTlas)
        return shadowTlas->handle;

    // 只有需要阴影时才构建。BLAS 按 GPU 网格缓存，TLAS 由同一快照的所有相机共享。
    auto temporary = std::make_shared<std::vector<Buf>>();
    work.hold(temporary);
    std::unordered_map<const GpuMesh*, std::shared_ptr<Acceleration>> newBlas;
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    constexpr auto buildStage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            buildStage, VK_ACCESS_2_MEMORY_READ_BIT);

    if (draws.size() > (1u << 24))
        throw std::length_error("Shadow instance index exceeds Vulkan's 24-bit limit");

    for (uint32_t index = 0; index < draws.size(); ++index)
    {
        const auto& item = draws[index];
        if (!item.castsShadow || item.mesh->count < 3)
            continue;

        // 实例变换必须可逆。零缩放对象仍可进入光栅几何路径。
        const float determinant = glm::determinant(glm::mat3(item.world));
        if (!std::isfinite(determinant) || determinant == 0)
            continue;

        auto blas = item.mesh->shadowBlas;
        if (!blas)
        {
            auto& entry = newBlas[item.mesh];
            if (!entry)
            {
                const auto count = item.mesh->count / 3;
                if (count > d.accelerationLimits.maxPrimitiveCount)
                    throw std::length_error("Shadow mesh exceeds acceleration structure primitive limit");

                VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
                auto& triangles = geometry.geometry.triangles;
                triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                triangles.vertexData.deviceAddress = d.address(*item.mesh->vertices) + offsetof(Vertex, position);
                triangles.vertexStride = sizeof(Vertex);
                triangles.maxVertex = uint32_t(item.geometry->vertices.size() - 1);
                triangles.indexType = VK_INDEX_TYPE_UINT32;
                triangles.indexData.deviceAddress = d.address(*item.mesh->indices);
                entry = build(d, cmd, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geometry, count,
                              temporary->emplace_back());
            }
            blas = entry;
        }

        VkAccelerationStructureInstanceKHR instance{};
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col)
                instance.transform.matrix[row][col] = item.world[col][row];
        instance.instanceCustomIndex = index;
        instance.mask = 0xff;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = blas->address();
        instances.push_back(instance);
    }

    if (instances.size() > d.accelerationLimits.maxInstanceCount)
        throw std::length_error("Scene exceeds acceleration structure instance limit");
    const auto bytes = instances.size() * sizeof(VkAccelerationStructureInstanceKHR);
    const auto inputSize = std::max(bytes, sizeof(VkAccelerationStructureInstanceKHR));
    auto& input = buffers.instances;
    if (!input || input->size < inputSize)
        input = d.buffer(inputSize, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
    if (bytes)
        std::memcpy(input->mapped, instances.data(), bytes);

    barrier(cmd, buildStage, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            buildStage, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    auto& data = geometry.geometry.instances;
    data.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    data.data.deviceAddress = d.address(*input);
    auto tlas = build(d, cmd, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, geometry,
                      uint32_t(instances.size()), buffers.scratch);
    barrier(cmd, buildStage, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    const auto handle = tlas->handle;
    work.onSubmitted([this, tlas = std::move(tlas), newBlas = std::move(newBlas)]() mutable
    {
        for (auto& [mesh, blas] : newBlas)
            mesh->shadowBlas = std::move(blas);
        shadowTlas = std::move(tlas);
    });
    return handle;
}
}
