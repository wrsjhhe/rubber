#pragma once
#include "../assets/AssetCache.hpp"
#include "sim/Scene.hpp"

namespace sim::vk
{
// 顶点与片元着色器共用的静态实例数据。内容由快照决定，与该快照的所有相机共享；
// 上一帧变换与历史匹配标记属于视图，见 ViewInstance。
struct SceneInstance
{
    glm::mat4 model;
    glm::vec4 colorRough, material;
    glm::uvec4 label;
    glm::vec4 boundsLo, boundsHi;
};
// 视图侧实例数据：上一帧模型矩阵与“存在匹配旧实例”标记。
struct ViewInstance
{
    glm::mat4 oldModel;
    glm::uvec4 label;
};
static_assert(sizeof(SceneInstance) == 144);
static_assert(sizeof(ViewInstance) == 80);

// 一个快照版本的共享 GPU 数据。网格、材质与静态实例数据只准备一次，
// 由同一快照的所有视图复用；资产上传的完成值作为帧提交的时间线依赖。
// 对象不可变：快照内容变化产生新版本，旧版本由旧快照与在途结果持有。
class RenderScene
{
  public:
    struct Item
    {
        uint32_t id;
        Geometry geometry;
        std::shared_ptr<const MeshStandardMaterial> material;
        glm::mat4 world;
        glm::vec3 minimum, maximum;
        bool castsShadow;
        const GpuMesh* mesh;
        VkDescriptorSet materialSet;
    };
    RenderScene(Device&, const std::shared_ptr<AssetCache>&, const SceneSnapshot&, uint64_t version);
    uint64_t version() const
    {
        return number;
    }
    const std::vector<Item>& items() const
    {
        return draws;
    }
    const Buffer& instances() const
    {
        return *staticData;
    }
    // 本场景资产上传完成的时间线值，零表示没有待完成上传。
    uint64_t uploads() const
    {
        return uploadValue;
    }
    // 内容一致性核对，用于哈希键命中后排除碰撞。
    bool matches(const SceneSnapshot&) const;

  private:
    uint64_t number = 0, uploadValue = 0;
    std::shared_ptr<AssetCache> assets;
    std::vector<Item> draws;
    Buf staticData;
};
}
