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
class BufferGeometry
{
  public:
    BufferGeometry(std::vector<Vertex> vertices, std::vector<uint32_t> indices);
    const std::vector<Vertex> vertices;
    const std::vector<uint32_t> indices;
    glm::vec3 minimum{}, maximum{};
};
using Geometry = std::shared_ptr<const BufferGeometry>;
Geometry BoxGeometry(float width = 1, float height = 1, float depth = 1);
Geometry SphereGeometry(float radius = 1, unsigned segments = 40, unsigned rings = 24);
Geometry CylinderGeometry(float radiusTop = 1, float radiusBottom = 1, float height = 1, unsigned segments = 40);
Geometry ConeGeometry(float radius = 1, float height = 1, unsigned segments = 40);
Geometry CapsuleGeometry(float radius = 0.5f, float length = 1, unsigned segments = 40, unsigned capSegments = 12);
Geometry TriangleMeshGeometry(std::vector<Vertex> vertices, std::vector<uint32_t> indices);
Geometry ConvexGeometry(const std::vector<glm::vec3>& points);

struct Texture
{
    uint32_t width, height;
    std::vector<uint8_t> rgba;
    static std::shared_ptr<const Texture> load(const std::filesystem::path& path);
};
struct MeshStandardMaterial
{
    glm::vec3 color{0.8f};
    float roughness = 0.5f;
    float metalness = 0;
    glm::vec2 uvRepeat{1};
    std::shared_ptr<const Texture> map, normalMap, metalRoughMap;
};
struct Mesh
{
    uint32_t id;
    Geometry geometry;
    std::shared_ptr<const MeshStandardMaterial> material;
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
};
class Object3D
{
  public:
    glm::mat4 local{1};
    Geometry geometry;
    std::shared_ptr<const MeshStandardMaterial> material;
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
    uint32_t addGroup(glm::mat4 local = glm::mat4(1), uint32_t parent = 0);
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
