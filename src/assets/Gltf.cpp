#include "sim/Gltf.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <map>
#include <numeric>
#include <stdexcept>
#include <limits>
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <stb_image.h>
namespace sim
{
namespace
{
void require(bool ok, const char* msg)
{
    if (!ok)
        throw std::runtime_error(msg);
}
struct Import
{
    std::filesystem::path dir;
    Scene scene;
    std::map<const cgltf_image*, std::shared_ptr<const Texture>> images;
    std::map<const cgltf_primitive*, Geometry> geometries;
    std::map<const cgltf_material*, std::shared_ptr<const MeshStandardMaterial>> materials;
    std::shared_ptr<const Texture> image(const cgltf_texture_view& view)
    {
        if (!view.texture)
            return {};
        require(view.texcoord == 0 && !view.has_transform,
                "Importer expects TEXCOORD_0 without texture transform");
        auto img = view.texture->image;
        require(img, "Missing core glTF image");
        if (auto it = images.find(img); it != images.end())
            return it->second;
        if (!img->buffer_view)
        {
            require(img->uri, "Missing texture URI");
            std::string uri = img->uri;
            require(!uri.starts_with("data:"), "Use GLB for embedded textures");
            uri.resize(cgltf_decode_uri(uri.data()));
            return images[img] = Texture::load(dir / std::filesystem::u8path(uri));
        }
        int w, h, c;
        auto bv = img->buffer_view;
        require(bv->size <= INT_MAX, "Texture too large");
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> p(
            stbi_load_from_memory(cgltf_buffer_view_data(bv), int(bv->size), &w, &h, &c, 4), stbi_image_free);
        require(bool(p), "Cannot decode embedded texture");
        return images[img] = std::make_shared<Texture>(uint32_t(w), uint32_t(h),
                                                       std::vector<uint8_t>(p.get(), p.get() + size_t(w) * h * 4));
    }
    std::shared_ptr<const MeshStandardMaterial> material(const cgltf_material* src)
    {
        if (auto it = materials.find(src); it != materials.end())
            return it->second;
        auto m = std::make_shared<MeshStandardMaterial>();
        m->color = glm::vec3(1);
        m->metalness = m->roughness = 1;
        if (src)
        {
            require(src->alpha_mode == cgltf_alpha_mode_opaque, "Only opaque glTF materials are supported");
            auto& mr = src->pbr_metallic_roughness;
            m->color = glm::make_vec3(mr.base_color_factor);
            m->roughness = mr.roughness_factor;
            m->metalness = mr.metallic_factor;
            m->map = image(mr.base_color_texture);
            m->metalRoughMap = image(mr.metallic_roughness_texture);
            m->normalMap = image(src->normal_texture);
        }
        return materials[src] = m;
    }
    Geometry geometry(const cgltf_primitive& prim)
    {
        if (auto it = geometries.find(&prim); it != geometries.end())
            return it->second;
        require(prim.type == cgltf_primitive_type_triangles && !prim.has_draco_mesh_compression,
                "Expected uncompressed triangles");
        auto pos = cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
        auto norm = cgltf_find_accessor(&prim, cgltf_attribute_type_normal, 0);
        auto uv = cgltf_find_accessor(&prim, cgltf_attribute_type_texcoord, 0);
        require(pos && pos->count <= UINT32_MAX, "Missing or oversized POSITION");
        auto unpack = [&](const cgltf_accessor* attr, size_t stride)
        {
            std::vector<float> out(pos->count * stride, 0.f);
            if (attr)
                require(cgltf_accessor_unpack_floats(attr, out.data(), out.size()) == out.size(),
                        "Invalid vertex accessor");
            return out;
        };
        auto ps = unpack(pos, 3), ns = unpack(norm, 3), uvs = unpack(uv, 2);
        std::vector<Vertex> v(pos->count);
        for (size_t i = 0; i < v.size(); ++i)
            v[i] = {glm::make_vec3(&ps[i * 3]), glm::make_vec3(&ns[i * 3]), glm::make_vec2(&uvs[i * 2])};
        std::vector<uint32_t> indices(prim.indices ? prim.indices->count : v.size());
        if (prim.indices)
            require(cgltf_accessor_unpack_indices(prim.indices, indices.data(), 4, indices.size()) ==
                        indices.size(),
                    "Invalid index accessor");
        else
            std::iota(indices.begin(), indices.end(), 0u);
        require(indices.size() % 3 == 0, "Incomplete triangle");
        if (!norm)
        {
            for (size_t i = 0; i < indices.size(); i += 3)
            {
                auto& a = v.at(indices[i]);
                auto& b = v.at(indices[i + 1]);
                auto& c = v.at(indices[i + 2]);
                auto n = glm::cross(b.position - a.position, c.position - a.position);
                a.normal += n;
                b.normal += n;
                c.normal += n;
            }
            for (auto& a : v)
                a.normal = glm::length(a.normal) > 0 ? glm::normalize(a.normal) : glm::vec3(0, 1, 0);
        }
        return geometries[&prim] = TriangleMeshGeometry(std::move(v), std::move(indices));
    }
    void node(const cgltf_node& n, uint32_t parent)
    {
        glm::mat4 local;
        cgltf_node_transform_local(&n, glm::value_ptr(local));
        auto group = scene.addGroup(local, parent);
        if (n.mesh)
            for (size_t i = 0; i < n.mesh->primitives_count; ++i)
            {
                auto& p = n.mesh->primitives[i];
                scene.add(geometry(p), material(p.material), glm::mat4(1), group);
            }
        for (size_t i = 0; i < n.children_count; ++i)
            node(*n.children[i], group);
    }
};
}
Scene loadGltf(const std::filesystem::path& path)
{
    cgltf_options options{};
    cgltf_data* raw = nullptr;
    auto file = path.string();
    require(cgltf_parse_file(&options, file.c_str(), &raw) == cgltf_result_success, "Cannot parse glTF");
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, cgltf_free);
    require(cgltf_load_buffers(&options, raw, file.c_str()) == cgltf_result_success, "Cannot load glTF buffers");
    require(cgltf_validate(raw) == cgltf_result_success, "Invalid glTF");
    Import importer{path.parent_path()};
    auto scene = raw->scene ? raw->scene : (raw->scenes_count ? raw->scenes : nullptr);
    if (scene)
        for (size_t i = 0; i < scene->nodes_count; ++i)
            importer.node(*scene->nodes[i], 0);
    else
        for (size_t i = 0; i < raw->nodes_count; ++i)
            if (!raw->nodes[i].parent)
                importer.node(raw->nodes[i], 0);
    return std::move(importer.scene);
}
}
