#include "RenderScene.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

namespace sim::vk
{
RenderScene::RenderScene(Device& d, const std::shared_ptr<AssetCache>& cache, const SceneSnapshot& snapshot,
                         uint64_t version)
    : number(version), assets(cache)
{
    draws.reserve(snapshot.meshes.size());
    auto bytes = std::max<size_t>(1, snapshot.meshes.size()) * sizeof(SceneInstance);
    if (bytes > d.properties.limits.maxStorageBufferRange)
        throw std::length_error("Instance buffer exceeds device limit");
    staticData = d.buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    std::unordered_set<uint32_t> ids;
    for (auto& mesh : snapshot.meshes)
    {
        if (!mesh.geometry || !mesh.material || !mesh.id)
            throw std::invalid_argument("Invalid snapshot mesh");
        if (!ids.insert(mesh.id).second)
            throw std::invalid_argument("Duplicate snapshot mesh ID");
        auto& material = *mesh.material;
        SceneInstance instance{
            mesh.world,
            {material.color, material.roughness},
            {material.metalness, material.uvRepeat.x, material.uvRepeat.y, float(bool(material.normalMap))},
            {mesh.id, 0, 0, 0},
            {mesh.geometry->minimum, 1},
            {mesh.geometry->maximum, 1}};
        auto offset = draws.size() * sizeof(SceneInstance);
        std::memcpy(static_cast<char*>(staticData->mapped) + offset, &instance, sizeof(instance));
        // 首次出现的网格与材质记录到当前上传批次，命中则直接引用已有 GPU 数据。
        auto& gpu = assets->mesh(mesh.geometry);
        auto set = assets->material(mesh.material);
        draws.push_back({mesh.id, mesh.geometry, mesh.material, mesh.world, mesh.geometry->minimum,
                         mesh.geometry->maximum, mesh.castsShadow, &gpu, set});
    }
    // 整个场景的首次上传在一次提交内完成，帧提交等待该值而不是重新同步上传。
    uploadValue = assets->flush();
}
bool RenderScene::matches(const SceneSnapshot& scene) const
{
    if (scene.meshes.size() != draws.size())
        return false;
    for (size_t i = 0; i < draws.size(); ++i)
    {
        auto& mesh = scene.meshes[i];
        auto& item = draws[i];
        if (mesh.id != item.id || mesh.castsShadow != item.castsShadow || mesh.world != item.world ||
            mesh.geometry != item.geometry || mesh.material != item.material ||
            mesh.geometry->revision != item.geometry->revision ||
            mesh.material->revision != item.material->revision)
            return false;
    }
    return true;
}
}
