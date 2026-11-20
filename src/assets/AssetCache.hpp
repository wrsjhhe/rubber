#pragma once
#include "../render/Pipeline.hpp"
#include <map>

namespace sim::vk
{
struct GpuMesh
{
    Buf vertices, indices;
    uint32_t count;
};
struct Packet
{
    const GpuMesh* mesh;
    VkDescriptorSet material;
};
struct AssetCache
{
    Device& d;
    Pipelines* pipelines;
    Lifetime life;
    std::map<Geometry, GpuMesh> meshes;
    using TexturePtr = std::shared_ptr<const Texture>;
    std::map<std::pair<TexturePtr, bool>, Img> textures;
    std::map<std::shared_ptr<const MeshStandardMaterial>, VkDescriptorSet> materials;
    TexturePtr white = std::make_shared<Texture>(Texture{1, 1, {255, 255, 255, 255}});
    VkDescriptorPool pool{};
    size_t used = 0;
    AssetCache(Device& device, Pipelines& programs) : d(device), pipelines(&programs) {}
    const GpuMesh& mesh(const Geometry& key)
    {
        auto it = meshes.find(key);
        if (it != meshes.end())
            return it->second;
        GpuMesh m;
        m.vertices = d.upload(key->vertices.data(), key->vertices.size() * sizeof(Vertex),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        m.indices = d.upload(key->indices.data(), key->indices.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        m.count = uint32_t(key->indices.size());
        return meshes.emplace(key, std::move(m)).first->second;
    }
    VkDescriptorSet material(const std::shared_ptr<const MeshStandardMaterial>& m)
    {
        if (auto it = materials.find(m); it != materials.end())
            return it->second;
        if (used % 128 == 0)
            pool = descriptorPool(d, life, 128);
        auto set = allocateSet(d, pool, pipelines->material);
        ++used;
        unsigned i = 0;
        for (auto tex : {m->map, m->normalMap, m->metalRoughMap})
        {
            if (!tex)
                tex = white;
            auto key = std::make_pair(tex, i == 0);
            auto it = textures.find(key);
            if (it == textures.end())
                it = textures.emplace(key, d.texture(*tex, i == 0)).first;
            bindImage(d, set, i++, *it->second, pipelines->repeat);
        }
        materials.emplace(m, set);
        return set;
    }
};
}
