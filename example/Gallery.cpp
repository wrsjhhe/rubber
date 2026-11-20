#include "Gallery.hpp"
#include "sim/Gltf.hpp"
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

using namespace sim;
namespace
{
using Mat = std::shared_ptr<MeshStandardMaterial>;
Mat surface(glm::vec3 color, float rough = .6f, float metal = 0)
{
    auto m = std::make_shared<MeshStandardMaterial>();
    m->color = color;
    m->roughness = rough;
    m->metalness = metal;
    return m;
}
glm::mat4 transform(glm::vec3 p, glm::vec3 scale)
{
    return glm::scale(glm::translate(glm::mat4(1), p), scale);
}
std::shared_ptr<Texture> tiles()
{
    constexpr int size = 256;
    auto texture = std::make_shared<Texture>(Texture{size, size, {}});
    texture->rgba.resize(size * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            float u = (x + .5f) / size, v = (y + .5f) / size;
            float edge = std::min({u, v, 1 - u, 1 - v});
            float grain = std::sin(x * 1.7f + y * 2.3f) * 2 + std::sin(x * .07f + y * .13f) * 2;
            glm::vec3 c{183, 157, 130};
            if (edge < .009f)
                c = {106, 94, 82};
            else if (edge < .025f)
                c = {154, 130, 106};
            float diamond = std::abs(u - .5f) + std::abs(v - .5f);
            if (std::abs(diamond - .28f) < .008f || std::abs(diamond - .77f) < .012f)
                c = {127, 111, 92};
            for (int k = 0; k < 3; ++k)
                texture->rgba[(y * size + x) * 4 + k] = uint8_t(c[k] + grain);
            texture->rgba[(y * size + x) * 4 + 3] = 255;
        }
    return texture;
}
// 拱券由独立楔形石组成，侧、顶和拱腹均有真实轮廓。
Geometry voussoir(float start, float end, float r, float width, float depth)
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    auto pt = [&](float a, float radius, float z)
    { return glm::vec3(std::cos(a) * radius, std::sin(a) * radius, z); };
    auto face = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d)
    {
        uint32_t k = uint32_t(vertices.size());
        auto n = glm::normalize(glm::cross(b - a, c - a));
        vertices.insert(vertices.end(), {{a, n, {0, 0}}, {b, n, {1, 0}}, {c, n, {1, 1}}, {d, n, {0, 1}}});
        indices.insert(indices.end(), {k, k + 1, k + 2, k, k + 2, k + 3});
    };
    glm::vec3 p[] = {pt(start, r, depth),        pt(start, r + width, depth), pt(end, r + width, depth),
                     pt(end, r, depth),          pt(start, r, -depth),        pt(start, r + width, -depth),
                     pt(end, r + width, -depth), pt(end, r, -depth)};
    face(p[0], p[1], p[2], p[3]);
    face(p[7], p[6], p[5], p[4]);
    for (int i = 0; i < 4; ++i)
        face(p[i], p[i + 4], p[(i + 1) % 4 + 4], p[(i + 1) % 4]);
    return TriangleMeshGeometry(std::move(vertices), std::move(indices));
}
void mountAsset(Scene& gallery, const Scene& asset)
{
    auto snap = asset.snapshot(0);
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (auto& m : snap.meshes)
        for (auto& vertex : m.geometry->vertices)
        {
            glm::vec3 p = m.world * glm::vec4(vertex.position, 1);
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    if (snap.meshes.empty() || hi.y - lo.y < 1e-6f)
        throw std::runtime_error("Gallery model must have nonzero height");
    float scale = 2.65f / (hi.y - lo.y);
    glm::vec3 base{(lo.x + hi.x) * .5f, lo.y, (lo.z + hi.z) * .5f};
    auto placement = glm::translate(glm::mat4(1), {0, .52f, 0});
    placement = glm::rotate(placement, glm::radians(-16.f), {0, 1, 0});
    placement = glm::translate(glm::scale(placement, glm::vec3(scale)), -base);
    for (auto& m : snap.meshes)
        gallery.add(m.geometry, m.material, placement * m.world);
}
}
Scene courtyard(const std::filesystem::path& asset)
{
    Scene s;
    auto box = BoxGeometry(), drum = CylinderGeometry(1, 1, 1, 128);
    auto plaster = surface({.7f, .48f, .28f}), trim = surface({.78f, .64f, .47f}),
         dark = surface({.034f, .095f, .09f});
    auto brass = surface({.63f, .39f, .15f}, .29f, .8f);
    auto limestone = std::make_shared<Texture>(Texture{256, 256, {}});
    limestone->rgba.reserve(256 * 256 * 4);
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x)
        {
            float grain = std::sin(x * 71.3f + y * 32.1f) * std::sin(y * .8f + x * .5f);
            float vein = std::sin(y * .095f + std::sin(x * .05f) + std::sin(y * .12f + x * .04f) * .7f);
            auto shade = uint8_t(229 + grain * 7 + vein * 5 - std::pow(std::max(0.f, vein), 20.f) * 10);
            limestone->rgba.insert(limestone->rgba.end(), {shade, shade, shade, 255});
        }
    plaster->map = trim->map = limestone;
    auto stone = surface({.37f, .28f, .20f});
    auto floor = surface({.81f, .78f, .74f});
    floor->map = tiles();
    floor->uvRepeat = {21, 18};
    auto block = [&](glm::vec3 p, glm::vec3 dims, Mat m) { s.add(box, m, transform(p, dims)); };
    block({0, -.16f, 0}, {35, .3f, 30}, floor);
    // 展台与底缘的铜质包边。
    for (auto [y, r, h, m] : std::initializer_list<std::tuple<float, float, float, Mat>>{
             {.08f, 1.78f, .16f, stone}, {.19f, 1.69f, .06f, brass}, {.36f, 1.65f, .32f, trim}})
        s.add(drum, m, transform({0, y, 0}, {r, h, r}));
    mountAsset(s, loadGltf(asset.empty() ? std::filesystem::path(GALLERY_ASSETS) / "AntiqueCamera.glb" : asset));

    // 两层连续的拱廊给级联阴影提供不同深度的投影物体。
    constexpr int pieces = 17;
    std::vector<Geometry> arches;
    for (int j = 0; j < pieces; ++j)
        arches.push_back(voussoir(glm::pi<float>() * j / pieces + .0015f,
                                  glm::pi<float>() * (j + 1) / pieces - .0015f, 1.29f, .26f, .28f));
    for (float z : {-3.5f, -7.3f})
    {
        for (int i = -3; i <= 3; ++i)
        {
            float x = i * 3.1f;
            for (int j = 0; j < pieces; ++j)
                s.add(arches[j], j % 4 == 0 ? trim : plaster, transform({x, 2.15f, z}, {1, 1, 1}));
            float px = x - 1.55f;
            block({px, 1.08f, z}, {.5f, 2.16f, .54f}, plaster);
            block({px, .14f, z}, {.69f, .28f, .76f}, trim);
            block({px, 2.06f, z}, {.72f, .18f, .75f}, trim);
            block({px, 3.72f, z}, {3.1f, .19f, .66f}, trim);
        }
    }
    block({0, 2, -9}, {24, 4, .35f}, dark);
    for (int i = -10; i <= 10; ++i)
        block({i * 1.f, 2, -8.77f}, {.025f, 3.8f, .06f}, brass);
    // 植物的枝叶为刚性几何构成，便于直接观察自遮挡与标签。
    auto leaf = SphereGeometry(1, 12, 8);
    auto foliage = surface({.10f, .24f, .095f});
    auto pot = surface({.49f, .18f, .095f});
    auto planter = CylinderGeometry(.57f, .37f, .82f);
    for (float x : {-3.4f, 3.5f})
    {
        s.add(planter, pot, transform({x, .41f, -1.4f}, {1, 1, 1}));
        for (int j = 0; j < 23; ++j)
        {
            float a = j * 2.39996f;
            glm::vec3 origin{x, .68f, -1.4f};
            auto matrix = glm::rotate(glm::translate(glm::mat4(1), origin), a, {0, 1, 0});
            matrix = glm::rotate(matrix, .35f + (j % 5) * .17f, {0, 0, 1});
            matrix = glm::translate(matrix, {0, .52f, 0});
            s.add(leaf, foliage, glm::scale(matrix, {.11f, .67f, .027f}));
        }
    }
    s.sun.direction = {.65f, -.85f, -.55f};
    s.lights.push_back({{-2, 3.5f, 3}, {15, 12, 9}});
    return s;
}
Scene primitives()
{
    Scene s;
    auto base = surface({.38f, .40f, .43f});
    s.add(BoxGeometry(), base, transform({0, -.12f, 0}, {30, .24f, 20}));
    std::vector<Geometry> shapes = {
        BoxGeometry(),
        SphereGeometry(.69f),
        CylinderGeometry(.55f, .55f, 1.3f),
        ConeGeometry(.65f, 1.4f),
        CapsuleGeometry(.42f, .68f),
        ConvexGeometry({{-1, 0, -.5f}, {.7f, -.5f, -.5f}, {.6f, -.4f, .8f}, {0, 1, 0}, {-.6f, 0, .45f}}),
        TriangleMeshGeometry({{{-.7f, -.6f, 0}, {0, 0, 1}, {0, 1}},
                              {{.7f, -.6f, 0}, {0, 0, 1}, {1, 1}},
                              {{0, .8f, 0}, {0, 0, 1}, {.5f, 0}}},
                             {0, 1, 2})};
    for (int i = 0; i < int(shapes.size()); ++i)
    {
        auto m = surface(glm::mix(glm::vec3(.055f, .27f, .25f), glm::vec3(.78f, .26f, .065f), i / 6.f),
                         .16f + i * .11f, .15f + i * .11f);
        s.add(shapes[i], m, transform({(i - 3) * 1.6f, .76f, 0}, {1, 1, 1}));
    }
    return s;
}
PerspectiveCamera galleryCamera(float azimuth, float elevation, float distance)
{
    PerspectiveCamera c;
    c.target = {0, 1.42f, 0};
    c.position = c.target + distance * glm::vec3(std::sin(azimuth) * std::cos(elevation), std::sin(elevation),
                                                 std::cos(azimuth) * std::cos(elevation));
    c.farPlane = 45;
    c.verticalFov = 44;
    return c;
}
