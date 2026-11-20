#pragma once
#include "RenderScene.hpp"
#include "sim/Scene.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// 每帧常量与几何辅助：pass 与帧图编排共用，避免把布局推导和剔除逻辑复制两份。
namespace sim::vk
{
struct Globals
{
    glm::mat4 vp, oldVP, invVP, lightVP[4];
    glm::vec4 eye, sunDirection, sunColor, splits, jitter, settings, pointPosition[16], pointColor[16], info;
    // 纯投影的两轴焦距（像素单位）。vp 含视图旋转，用它的元素当焦距会让点尺寸随俯仰/翻滚变化，
    // 因此单独传递内参：x、y 各一个尺度，fx != fy 的标定相机也能正确换算出椭圆足迹。
    glm::vec4 lens;
};
static_assert(sizeof(Globals) == 1088);
struct Draw
{
    uint32_t object;
    int cascade, stable;
};
constexpr unsigned temporalSamples = 16;
// 视图侧历史记录快照当时的固定资产：几何身份与材质值。材质是值类型，内含的
// 贴图只按不可变身份比较，所以更换贴图必然使静止冻结与重投影历史失效。
struct MeshHistory
{
    glm::mat4 world;
    Geometry geometry;
    MeshStandardMaterial material;
    bool castsShadow;
    uint32_t order;
    bool operator==(const MeshHistory&) const = default;
};
inline bool sameLights(const std::vector<PointLight>& a, const std::vector<PointLight>& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y)
                                              { return x.position == y.position && x.intensity == y.intensity; });
}
inline std::array<glm::vec4, 8> corners(const glm::mat4& m, glm::vec3 lo, glm::vec3 hi)
{
    std::array<glm::vec4, 8> p;
    for (int i = 0; i < 8; ++i)
        p[i] = m * glm::vec4(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z, 1);
    return p;
}
inline bool visible(const glm::mat4& world, glm::vec3 lo, glm::vec3 hi, const glm::mat4& vp)
{
    auto p = corners(vp * world, lo, hi);
    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::all_of(p.begin(), p.end(), [&](auto v) { return v[axis] < (axis == 2 ? 0.f : -v.w); }))
            return false;
        if (std::all_of(p.begin(), p.end(), [&](auto v) { return v[axis] > v.w; }))
            return false;
    }
    return true;
}
// 快照内容键：只描述影响共享 GPU 数据的字段，相机、时间与光照变化不改变键。
// 几何与贴图用资产身份，材质用内容，因此用户手里的对象被修改或释放都不会让键漂移。
inline uint64_t sceneKey(const SceneSnapshot& scene, uint64_t generation)
{
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](const void* data, size_t bytes)
    {
        auto bytes8 = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i)
        {
            hash ^= bytes8[i];
            hash *= 1099511628211ull;
        }
    };
    mix(&generation, sizeof(generation));
    for (auto& mesh : scene.meshes)
    {
        AssetId geometry = mesh.geometry ? mesh.geometry->id : 0;
        mix(&mesh.id, sizeof(mesh.id));
        mix(&mesh.castsShadow, sizeof(mesh.castsShadow));
        mix(&mesh.world, sizeof(mesh.world));
        mix(&geometry, sizeof(geometry));
        auto& material = mesh.material;
        mix(&material.color, sizeof(material.color));
        mix(&material.roughness, sizeof(material.roughness));
        mix(&material.metalness, sizeof(material.metalness));
        mix(&material.uvRepeat, sizeof(material.uvRepeat));
        for (const auto& texture : {material.map, material.normalMap, material.metalRoughMap})
        {
            AssetId id = texture ? texture->id : 0;
            mix(&id, sizeof(id));
        }
    }
    return hash;
}
// 阴影：按相机视锥与场景包围盒拟合四级 CSM，texel 对齐后写入同一张图集。
inline void fitShadows(Globals& g, const PerspectiveCamera& camera, const std::vector<RenderScene::Item>& items,
                       uint32_t tile)
{
    glm::vec3 dir = g.sunDirection;
    auto rotation =
        glm::lookAtRH(glm::vec3(0), dir, std::abs(dir.y) > .98f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));
    auto frustum = corners(g.invVP, {-1, -1, 0}, {1, 1, 1});
    for (auto& p : frustum)
        p = rotation * (p / p.w);
    // 把当前场景潜在投影物体的深度纳入覆盖范围。
    float loZ = frustum[0].z, hiZ = loZ;
    for (auto& item : items)
        if (item.castsShadow)
        {
            for (auto p : corners(rotation * item.world, item.minimum, item.maximum))
            {
                loZ = std::min(loZ, p.z);
                hiZ = std::max(hiZ, p.z);
            }
        }
    float n = camera.nearPlane, f = camera.farPlane, previous = n;
    for (int c = 0; c < 4; ++c)
    {
        float t = (c + 1) * .25f;
        float split = glm::mix(n + (f - n) * t, n * std::pow(f / n, t), .72f);
        g.splits[c] = split;
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (int i = 0; i < 4; ++i)
            for (float s : {previous, split})
            {
                glm::vec3 p = glm::mix(frustum[i], frustum[i + 4], (s - n) / (f - n));
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
        float extent = std::max(hi.x - lo.x, hi.y - lo.y) * .5f + 0.04f;
        float step = 2 * extent / tile;
        glm::vec2 center = glm::round(glm::vec2((lo + hi) * .5f) / step) * step;
        auto projection = glm::orthoRH_ZO(center.x - extent, center.x + extent, center.y - extent,
                                          center.y + extent, -std::max(hi.z, hiZ) - 1, -std::min(lo.z, loZ) + 1);
        g.lightVP[c] = projection * rotation;
        previous = split;
    }
}
inline float halton(unsigned index, unsigned base)
{
    float f = 1, r = 0;
    while (index)
    {
        f /= base;
        r += f * (index % base);
        index /= base;
    }
    return r;
}
}
