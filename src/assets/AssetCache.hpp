#pragma once
#include "../render/Pipeline.hpp"
#include <map>
#include <tuple>

namespace sim::vk
{
struct GpuMesh
{
    Buf vertices, indices;
    uint32_t count;
};
using TexturePtr = std::shared_ptr<const Texture>;
// 缓存键由对象身份与内容版本组成。身份区分不同对象，版本区分同一对象的多次发布，
// 贴图键额外区分色彩空间，避免 sRGB 与线性数据共用同一 GPU 图像。
struct GeometryKey
{
    const void* identity;
    uint64_t revision;
    bool operator<(const GeometryKey& other) const
    {
        return std::tie(identity, revision) < std::tie(other.identity, other.revision);
    }
};
struct TextureKey
{
    const void* identity;
    uint64_t revision;
    bool srgb;
    bool operator<(const TextureKey& other) const
    {
        return std::tie(identity, revision, srgb) < std::tie(other.identity, other.revision, other.srgb);
    }
};
struct MaterialKey
{
    const void* identity;
    uint64_t revision;
    bool operator<(const MaterialKey& other) const
    {
        return std::tie(identity, revision) < std::tie(other.identity, other.revision);
    }
};
// 一个缓存代际的 GPU 资产。上传先累积到当前批次，再由 flush 一次提交，
// 完成后通过设备时间线建立提交依赖；代际只由缓存引用持有，销毁即释放资源。
struct AssetCache
{
    Device& d;
    Pipelines* pipelines;
    Lifetime life;
    AssetStats* stats;
    uint64_t generation;
    std::map<GeometryKey, GpuMesh> meshes;
    std::map<TextureKey, Img> textures;
    std::map<MaterialKey, VkDescriptorSet> materials;
    std::unique_ptr<UploadBatch> batch;
    std::vector<std::unique_ptr<UploadBatch>> inflight;
    TexturePtr white = std::make_shared<Texture>(Texture{1, 1, {255, 255, 255, 255}});
    VkDescriptorPool pool{};
    size_t used = 0;
    AssetCache(Device& device, Pipelines& programs, AssetStats& counters, uint64_t id)
        : d(device), pipelines(&programs), stats(&counters), generation(id)
    {
    }
    UploadBatch& uploading()
    {
        if (!batch)
            batch = std::make_unique<UploadBatch>(d);
        return *batch;
    }
    // 提交当前累积的上传，返回属于该代的完成时间线值；没有待上传内容时返回零。
    uint64_t flush()
    {
        if (!batch || batch->empty())
            return 0;
        auto value = batch->submit();
        inflight.push_back(std::move(batch));
        ++stats->uploadBatches;
        return value;
    }
    // 回收已完成的上传批次，释放其复制源。
    void collect()
    {
        std::erase_if(inflight,
                      [](auto& upload)
                      {
                          if (!upload->complete())
                              return false;
                          upload->wait();
                          return true;
                      });
    }
    const GpuMesh& mesh(const Geometry& key)
    {
        GeometryKey cacheKey{key.get(), key->revision};
        auto it = meshes.find(cacheKey);
        if (it != meshes.end())
            return it->second;
        auto& uploads = uploading();
        GpuMesh m;
        m.vertices = uploads.upload(key->vertices.data(), key->vertices.size() * sizeof(Vertex),
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        m.indices = uploads.upload(key->indices.data(), key->indices.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        m.count = uint32_t(key->indices.size());
        ++stats->meshUploads;
        return meshes.emplace(cacheKey, std::move(m)).first->second;
    }
    VkDescriptorSet material(const std::shared_ptr<const MeshStandardMaterial>& m)
    {
        MaterialKey cacheKey{m.get(), m->revision};
        if (auto it = materials.find(cacheKey); it != materials.end())
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
            TextureKey key{tex.get(), tex->revision, i == 0};
            auto it = textures.find(key);
            if (it == textures.end())
            {
                it = textures.emplace(key, uploading().texture(*tex, i == 0)).first;
                ++stats->textureUploads;
            }
            bindImage(d, set, i++, *it->second, pipelines->repeat);
        }
        materials.emplace(cacheKey, set);
        ++stats->materialUploads;
        return set;
    }
};
}
