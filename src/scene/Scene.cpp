#include "sim/Scene.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace sim
{
namespace
{
void require(bool ok, const char* message)
{
    if (!ok)
        throw std::invalid_argument(message);
}
bool finite(glm::vec3 p)
{
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
struct Ring
{
    float r, y, nr, ny;
};
Geometry lathe(const std::vector<Ring>& rings, unsigned segments)
{
    require(segments >= 3 && segments <= 4096, "Invalid radial segments");
    std::vector<Vertex> v;
    std::vector<uint32_t> idx;
    for (size_t j = 0; j < rings.size(); ++j)
    {
        auto [r, y, nr, ny] = rings[j];
        for (unsigned i = 0; i <= segments; ++i)
        {
            float u = float(i) / segments;
            float a = u * glm::two_pi<float>();
            glm::vec2 d{std::cos(a), std::sin(a)};
            v.push_back(
                {{r * d.x, y, r * d.y}, {nr * d.x, ny, nr * d.y}, {u, 1.f - float(j) / (rings.size() - 1)}});
            if (j && i)
            {
                uint32_t e = uint32_t(v.size() - 1), b = e - segments - 1;
                if (rings[j - 1].r > 0)
                    idx.insert(idx.end(), {b - 1, e - 1, b});
                if (r > 0)
                    idx.insert(idx.end(), {b, e - 1, e});
            }
        }
    }
    return TriangleMeshGeometry(std::move(v), std::move(idx));
}
}
BufferGeometry::BufferGeometry(std::vector<Vertex> v, std::vector<uint32_t> i)
    : vertices(std::move(v)), indices(std::move(i))
{
    require(!vertices.empty() && !indices.empty() && indices.size() % 3 == 0, "Expected indexed triangles");
    minimum = maximum = vertices.front().position;
    for (auto& vertex : vertices)
    {
        require(finite(vertex.position) && finite(vertex.normal), "Non-finite vertex");
        minimum = glm::min(minimum, vertex.position);
        maximum = glm::max(maximum, vertex.position);
    }
    for (auto index : indices)
        require(index < vertices.size(), "Index outside mesh");
}
Geometry TriangleMeshGeometry(std::vector<Vertex> v, std::vector<uint32_t> i)
{
    return std::make_shared<BufferGeometry>(std::move(v), std::move(i));
}
Geometry BoxGeometry(float w, float h, float d)
{
    require(w > 0 && h > 0 && d > 0, "Invalid box size");
    glm::vec3 half = glm::vec3(w, h, d) * 0.5f;
    std::vector<Vertex> v;
    std::vector<uint32_t> idx;
    for (int axis = 0; axis < 3; ++axis)
    {
        for (float sign : {-1.f, 1.f})
        {
            glm::vec3 n(0), u(0);
            n[axis] = sign;
            u[(axis + 1) % 3] = 1;
            auto t = glm::cross(n, u);
            uint32_t k = uint32_t(v.size());
            for (glm::vec2 uv : {glm::vec2(0, 0), {1, 0}, {1, 1}, {0, 1}})
                v.push_back({half * (n + u * (2 * uv.x - 1) + t * (2 * uv.y - 1)), n, uv});
            idx.insert(idx.end(), {k, k + 1, k + 2, k, k + 2, k + 3});
        }
    }
    return TriangleMeshGeometry(std::move(v), std::move(idx));
}
Geometry CylinderGeometry(float top, float bottom, float h, unsigned segments)
{
    require(top >= 0 && bottom >= 0 && top + bottom > 0 && h > 0, "Invalid cylinder");
    auto n = glm::normalize(glm::vec2(h, bottom - top));
    return lathe({{0, -h / 2, 0, -1},
                  {bottom, -h / 2, 0, -1},
                  {bottom, -h / 2, n.x, n.y},
                  {top, h / 2, n.x, n.y},
                  {top, h / 2, 0, 1},
                  {0, h / 2, 0, 1}},
                 segments);
}
Geometry ConeGeometry(float r, float h, unsigned s)
{
    return CylinderGeometry(0, r, h, s);
}
Geometry CapsuleGeometry(float r, float length, unsigned segments, unsigned caps)
{
    require(r > 0 && length >= 0 && caps >= 2 && caps <= 2048, "Invalid capsule");
    std::vector<Ring> rings;
    for (float side : {-1.f, 1.f})
    {
        for (unsigned i = 0; i <= caps; ++i)
        {
            float a = (float(i) / caps + (side < 0 ? -1.f : 0.f)) * glm::half_pi<float>();
            float c = std::max(0.f, std::cos(a));
            rings.push_back({r * c, r * std::sin(a) + side * length / 2, c, std::sin(a)});
        }
    }
    return lathe(rings, segments);
}
Geometry SphereGeometry(float r, unsigned segments, unsigned rings)
{
    require(r > 0 && rings >= 2 && rings <= 4096, "Invalid sphere");
    std::vector<Ring> profile;
    for (unsigned i = 0; i <= rings; ++i)
    {
        float a = (float(i) / rings - 0.5f) * glm::pi<float>();
        float c = std::max(0.f, std::cos(a));
        profile.push_back({r * c, r * std::sin(a), c, std::sin(a)});
    }
    return lathe(profile, segments);
}
Geometry ConvexGeometry(const std::vector<glm::vec3>& p)
{
    require(p.size() >= 4, "Hull requires four non-coplanar points");
    for (auto q : p)
        require(finite(q), "Invalid hull point");
    auto farthest = [&](auto distance)
    {
        return int(
            std::max_element(p.begin(), p.end(), [&](auto x, auto y) { return distance(x) < distance(y); }) -
            p.begin());
    };
    int a = 0;
    int b = farthest([&](auto q) { return glm::length(q - p[a]); });
    int c = farthest([&](auto q) { return glm::length(glm::cross(q - p[a], p[b] - p[a])); });
    auto normal = glm::cross(p[b] - p[a], p[c] - p[a]);
    int d = farthest([&](auto q) { return std::abs(glm::dot(q - p[a], normal)); });
    float eps = glm::length(p[b] - p[a]) * 1e-6f;
    require(glm::length(normal) > 0 && std::abs(glm::dot(glm::normalize(normal), p[d] - p[a])) > eps,
            "Degenerate convex hull");
    auto center = (p[a] + p[b] + p[c] + p[d]) * 0.25f;
    std::vector<glm::ivec3> faces;
    auto add = [&](int x, int y, int z)
    {
        if (glm::dot(glm::cross(p[y] - p[x], p[z] - p[x]), center - p[x]) > 0)
            std::swap(y, z);
        faces.push_back({x, y, z});
    };
    add(a, b, c);
    add(a, b, d);
    add(a, c, d);
    add(b, c, d);
    for (int i = 0; i < int(p.size()); ++i)
    {
        std::map<std::pair<int, int>, unsigned> edges;
        std::erase_if(faces,
                      [&](auto f)
                      {
                          auto n = glm::cross(p[f.y] - p[f.x], p[f.z] - p[f.x]);
                          if (glm::dot(n, p[i] - p[f.x]) <= eps * glm::length(n))
                              return false;
                          for (int j = 0; j < 3; ++j)
                              ++edges[std::minmax(f[j], f[(j + 1) % 3])];
                          return true;
                      });
        for (auto [edge, count] : edges)
            if (count == 1)
                add(edge.first, edge.second, i);
    }
    std::vector<Vertex> v;
    std::vector<uint32_t> idx;
    for (auto f : faces)
    {
        auto n = glm::normalize(glm::cross(p[f.y] - p[f.x], p[f.z] - p[f.x]));
        for (int j = 0; j < 3; ++j)
        {
            idx.push_back(uint32_t(v.size()));
            v.push_back({p[f[j]], n, {p[f[j]].x, p[f[j]].y}});
        }
    }
    return TriangleMeshGeometry(std::move(v), std::move(idx));
}
std::shared_ptr<const Texture> Texture::load(const std::filesystem::path& path)
{
    int w, h, channels;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load(path.string().c_str(), &w, &h, &channels, 4), stbi_image_free);
    if (!pixels)
        throw std::runtime_error("Cannot load texture: " + path.string());
    return std::make_shared<Texture>(
        Texture{uint32_t(w), uint32_t(h), {pixels.get(), pixels.get() + size_t(w) * h * 4}});
}
uint32_t Scene::addGroup(glm::mat4 local, uint32_t parent)
{
    if (parent)
        get(parent);
    require(nextId != 0, "Object ID overflow");
    Object3D node;
    node.nodeId = nextId;
    node.parentId = parent;
    node.local = local;
    nodes.push_back(std::move(node));
    return nextId++;
}
uint32_t Scene::add(Geometry geometry, std::shared_ptr<const MeshStandardMaterial> material, glm::mat4 local,
                    uint32_t parent)
{
    require(geometry && material, "Invalid mesh");
    auto id = addGroup(local, parent);
    nodes.back().geometry = std::move(geometry);
    nodes.back().material = std::move(material);
    return id;
}
Object3D& Scene::get(uint32_t id)
{
    auto it = std::lower_bound(nodes.begin(), nodes.end(), id,
                               [](const auto& obj, uint32_t key) { return obj.id() < key; });
    if (it == nodes.end() || it->id() != id)
        throw std::out_of_range("Unknown object ID");
    return *it;
}
void Scene::remove(uint32_t id)
{
    if (!id)
        return;
    std::unordered_set<uint32_t> removed{id};
    for (auto& node : nodes)
        if (removed.contains(node.parent()))
            removed.insert(node.id());
    std::erase_if(nodes, [&](auto& node) { return removed.contains(node.id()); });
}
SceneSnapshot Scene::snapshot(double t) const
{
    SceneSnapshot result{t, sun, lights, {}};
    result.meshes.reserve(nodes.size());
    std::unordered_map<uint32_t, glm::mat4> transforms;
    for (auto& node : nodes)
    {
        auto world = node.parent() ? transforms.at(node.parent()) * node.local : node.local;
        transforms.emplace(node.id(), world);
        if (node.geometry)
            result.meshes.push_back({node.id(), node.geometry, node.material, world, node.castsShadow});
    }
    return result;
}
glm::mat4 PerspectiveCamera::view() const
{
    require(glm::length(glm::cross(target - position, up)) > 1e-6f, "Invalid camera orientation");
    return glm::lookAtRH(position, target, up);
}
glm::mat4 PerspectiveCamera::projection(uint32_t w, uint32_t h) const
{
    require(w && h && nearPlane > 0 && farPlane > nearPlane && verticalFov > 0 && verticalFov < 179,
            "Invalid camera frustum");
    auto m = glm::perspectiveRH_ZO(glm::radians(verticalFov), float(w) / h, nearPlane, farPlane);
    m[1][1] *= -1;
    if (fx != 0 || fy != 0)
    {
        require(fx > 0 && fy > 0, "Invalid camera intrinsics");
        m[0][0] = 2 * fx / w;
        m[1][1] = -2 * fy / h;
        m[2][0] = 1 - 2 * cx / w;
        m[2][1] = 1 - 2 * cy / h;
    }
    return m;
}
}
