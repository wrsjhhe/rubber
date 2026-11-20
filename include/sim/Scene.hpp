#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace sim
{
struct Vertex
{
    glm::vec3 position, normal;
    glm::vec2 uv;
};
// 资产身份：对象构造时分配、进程内不复用。渲染缓存以身份而非地址区分对象，
// 释放后的地址被新对象复用时不会错误命中旧 GPU 数据。
using AssetId = uint64_t;
AssetId nextAssetId();
class BufferGeometry
{
  public:
    BufferGeometry(std::vector<Vertex> vertices, std::vector<uint32_t> indices);
    // 顶点、索引与包围盒都在构造时一次算定，之后全部只读：改变几何只能创建新
    // 对象。旧快照因此始终使用发布时的剔除与阴影范围，缓存切换代际后也能按同
    // 一份 CPU 数据重新上传。
    const std::vector<Vertex> vertices;
    const std::vector<uint32_t> indices;
    const AssetId id = nextAssetId();
    const glm::vec3 minimum, maximum;

  private:
    // 校验内容与包围盒在一次构造里算定，const 成员才能在初始化列表中固定下来。
    struct Baked;
    explicit BufferGeometry(Baked&&);
    static Baked bake(std::vector<Vertex>, std::vector<uint32_t>);
};
using Geometry = std::shared_ptr<const BufferGeometry>;
Geometry BoxGeometry(float width = 1, float height = 1, float depth = 1);
Geometry SphereGeometry(float radius = 1, unsigned segments = 40, unsigned rings = 24);
Geometry CylinderGeometry(float radiusTop = 1, float radiusBottom = 1, float height = 1, unsigned segments = 40);
Geometry ConeGeometry(float radius = 1, float height = 1, unsigned segments = 40);
Geometry CapsuleGeometry(float radius = 0.5f, float length = 1, unsigned segments = 40, unsigned capSegments = 12);
Geometry TriangleMeshGeometry(std::vector<Vertex> vertices, std::vector<uint32_t> indices);
Geometry ConvexGeometry(const std::vector<glm::vec3>& points);

// 点云：位置、线性颜色、强度与半径。半径以世界单位表示，绘制时按透视投影换算成像素，
// 因此同一份数据在不同分辨率、不同距离下保持物理尺寸一致。
struct PointVertex
{
    glm::vec3 position;
    float intensity;
    glm::vec3 color;
    float radius;
};
static_assert(sizeof(PointVertex) == 32);
class BufferPointCloud
{
  public:
    explicit BufferPointCloud(std::vector<PointVertex> points);
    // 与几何同样在构造时一次算定：点数据、包围盒与最大半径都只读，
    // 修改点云内容只能创建新对象，旧快照因此保持自己的剔除范围与上传内容。
    const std::vector<PointVertex> points;
    const AssetId id = nextAssetId();
    const glm::vec3 minimum, maximum;
    const float maxRadius = 0;

  private:
    struct Baked;
    explicit BufferPointCloud(Baked&&);
    static Baked bake(std::vector<PointVertex>);
};
using PointCloud = std::shared_ptr<const BufferPointCloud>;
struct PointObject
{
    uint32_t id = 0;
    PointCloud cloud;
    glm::mat4 world{1};
};
struct Texture
{
    Texture(uint32_t width, uint32_t height, std::vector<uint8_t> pixels);
    // 像素发布后不可变。复制得到的对象内容逐字节相同，因此共享同一身份也不会
    // 产生歧义；改变贴图内容只能创建新对象。
    const uint32_t width, height;
    const std::vector<uint8_t> rgba;
    const AssetId id = nextAssetId();
    static std::shared_ptr<const Texture> load(const std::filesystem::path& path);
};
// 材质是值类型：加入场景时按值复制，快照保存的是当时的内容。
// 之后修改用户手中的原对象不会改变已发布的快照，新快照才会带出新值。
struct MeshStandardMaterial
{
    glm::vec3 color{0.8f};
    float roughness = 0.5f;
    float metalness = 0;
    glm::vec2 uvRepeat{1};
    std::shared_ptr<const Texture> map, normalMap, metalRoughMap;
    bool operator==(const MeshStandardMaterial&) const = default;
};
struct Mesh
{
    uint32_t id;
    Geometry geometry;
    MeshStandardMaterial material;
    glm::mat4 world{1};
    bool castsShadow = true;
};
struct DirectionalLight
{
    glm::vec3 direction{-0.5f, -0.85f, -0.35f};
    glm::vec3 radiance{3.8f, 3.4f, 2.9f};
};
struct PointLight
{
    glm::vec3 position{};
    glm::vec3 intensity{10};
};
struct SceneSnapshot
{
    double time = 0;
    DirectionalLight sun;
    std::vector<PointLight> lights;
    std::vector<Mesh> meshes;
    // 点云与网格并列存在于同一快照：调度方按类型交给对应模块，不把点云伪装成三角网格。
    std::vector<PointObject> points;
};
class Object3D
{
  public:
    glm::mat4 local{1};
    Geometry geometry;
    PointCloud cloud;
    MeshStandardMaterial material;
    bool castsShadow = true;
    uint32_t id() const
    {
        return nodeId;
    }
    uint32_t parent() const
    {
        return parentId;
    }

  private:
    friend class Scene;
    uint32_t nodeId{}, parentId{};
};
class Scene
{
  public:
    DirectionalLight sun;
    std::vector<PointLight> lights;
    uint32_t add(Geometry geometry, std::shared_ptr<const MeshStandardMaterial> material,
                 glm::mat4 local = glm::mat4(1), uint32_t parent = 0);
    // 直接按值挂载，用于把一份快照里的材质原样搬到另一个场景。
    uint32_t add(Geometry geometry, const MeshStandardMaterial& material, glm::mat4 local = glm::mat4(1),
                 uint32_t parent = 0);
    uint32_t addGroup(glm::mat4 local = glm::mat4(1), uint32_t parent = 0);
    // 点云节点与网格节点共用父子变换与稳定 ID，但不是网格：不参与三角绘制与阴影。
    uint32_t addPoints(PointCloud cloud, glm::mat4 local = glm::mat4(1), uint32_t parent = 0);
    Object3D& get(uint32_t id);
    void remove(uint32_t id);
    SceneSnapshot snapshot(double time) const;

  private:
    uint32_t nextId = 1;
    std::vector<Object3D> nodes;
};
struct PerspectiveCamera
{
    glm::vec3 position{0, 2, 5}, target{}, up{0, 1, 0};
    float verticalFov = 48, nearPlane = 0.05f, farPlane = 100;
    // 正焦距启用像素内参，主点以图像左上角为原点。
    float fx = 0, fy = 0, cx = 0, cy = 0;
    glm::mat4 projection(uint32_t width, uint32_t height) const;
    glm::mat4 view() const;
};
}
