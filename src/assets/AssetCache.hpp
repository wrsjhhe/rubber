#pragma once
#include "../render/Pipeline.hpp"
#include <map>
#include <tuple>

namespace sim::vk
{
struct Acceleration;
struct GpuMesh
{
    Buf vertices, indices;
    uint32_t count;
    mutable std::shared_ptr<Acceleration> shadowBlas;
};
using TexturePtr = std::shared_ptr<const Texture>;
// 几何与贴图按资产身份索引：身份在构造时分配且不复用，缓存不需要保活 CPU 对象
// 也不会因为地址回收而被新对象误命中。贴图键额外区分色彩空间，避免 sRGB 与
// 线性数据共用同一 GPU 图像。
struct TextureKey
{
    AssetId identity;
    bool srgb;
    bool operator<(const TextureKey& other) const
    {
        return std::tie(identity, srgb) < std::tie(other.identity, other.srgb);
    }
};
// 材质按内容索引：内容相同的两个材质对象共享描述符集，内容不同必然分到不同条目。
// 贴图只参与身份比较，因为贴图内容本身不可变。
struct MaterialKey
{
    glm::vec3 color;
    float roughness, metalness;
    glm::vec2 uvRepeat;
    AssetId map, normalMap, metalRoughMap;
    auto tie() const
    {
        return std::tie(color.x, color.y, color.z, roughness, metalness, uvRepeat.x, uvRepeat.y, map, normalMap,
                        metalRoughMap);
    }
    bool operator<(const MaterialKey& other) const
    {
        return tie() < other.tie();
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
    std::map<AssetId, GpuMesh> meshes;
    std::map<TextureKey, Img> textures;
    std::map<MaterialKey, VkDescriptorSet> materials;
    std::unique_ptr<UploadBatch> batch;
    std::vector<std::unique_ptr<UploadBatch>> inflight;
    TexturePtr white = std::make_shared<Texture>(1, 1, std::vector<uint8_t>{255, 255, 255, 255});
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
    // 回收已完成的上传批次，释放其暂存副本。未完成的批次保留，帧提交仍在等待它们。
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
    // 仍未被回收的上传批次数与它们持有的暂存字节，用于确认正常加载路径会回落。
    uint32_t pendingUploads() const
    {
        return uint32_t(inflight.size()) + (batch ? 1u : 0u);
    }
    uint64_t stagingBytes() const
    {
        VkDeviceSize bytes = 0;
        for (auto& upload : inflight)
            bytes += upload->stagingBytes();
        if (batch)
            bytes += batch->stagingBytes();
        return bytes;
    }
    const GpuMesh& mesh(const Geometry& key)
    {
        auto it = meshes.find(key->id);
        if (it != meshes.end())
            return it->second;
        auto& uploads = uploading();
        GpuMesh m;
        VkBufferUsageFlags buildInput = 0;
        if (d.rayQuery)
            buildInput = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        m.vertices = uploads.upload(key->vertices.data(), key->vertices.size() * sizeof(Vertex),
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | buildInput);
        m.indices = uploads.upload(key->indices.data(), key->indices.size() * 4,
                                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT | buildInput);
        m.count = uint32_t(key->indices.size());
        ++stats->meshUploads;
        return meshes.emplace(key->id, std::move(m)).first->second;
    }
    // 材质的贴图槽解析为实际参与缓存的贴图：缺省槽使用共享白图。
    const Texture& slot(const TexturePtr& tex) const
    {
        return tex ? *tex : *white;
    }
    VkDescriptorSet material(const MeshStandardMaterial& m)
    {
        MaterialKey cacheKey{m.color,
                             m.roughness,
                             m.metalness,
                             m.uvRepeat,
                             slot(m.map).id,
                             slot(m.normalMap).id,
                             slot(m.metalRoughMap).id};
        if (auto it = materials.find(cacheKey); it != materials.end())
            return it->second;
        if (used % 128 == 0)
            pool = descriptorPool(d, life, 128);
        auto set = allocateSet(d, pool, pipelines->material);
        ++used;
        unsigned i = 0;
        for (auto tex : {m.map, m.normalMap, m.metalRoughMap})
        {
            const auto& image = slot(tex);
            TextureKey key{image.id, i == 0};
            auto it = textures.find(key);
            if (it == textures.end())
            {
                it = textures.emplace(key, uploading().texture(image, i == 0)).first;
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
