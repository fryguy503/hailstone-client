#include "assets/gltf_mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

#include <cgltf.h>

#include "core/byte_io.h"

namespace hailstone::assets {

namespace {

// Limits on what a replacement may hold: well past a detailed tree, short of what would stall a zone load.
constexpr std::size_t kMaxVertices = 1u << 20;
constexpr std::size_t kMaxIndices = 3u << 20;

glm::vec3 from_gltf(const glm::vec3& v) { return {v.x, -v.z, v.y}; }
glm::vec3 to_gltf(const glm::vec3& v) { return {v.x, v.z, -v.y}; }

std::uint32_t pack_rgba(const float c[4]) {
    std::uint32_t out = 0;
    for (int i = 0; i < 4; ++i) {
        const float f = std::isfinite(c[i]) ? std::clamp(c[i], 0.0f, 1.0f) : 1.0f;
        out |= static_cast<std::uint32_t>(std::lround(f * 255.0f)) << (8 * i);
    }
    return out;
}

// The texture name an image stands for: its URI's file name, decoded.
std::string image_name(const cgltf_image* image) {
    if (!image || !image->uri || std::strncmp(image->uri, "data:", 5) == 0) return {};
    std::string uri(image->uri);
    std::vector<char> buf(uri.begin(), uri.end());
    buf.push_back('\0');
    cgltf_decode_uri(buf.data());
    std::string name(buf.data());
    if (auto slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
    return name;
}

Material read_material(const cgltf_material* m, std::size_t index) {
    Material out;
    if (m && m->name) {
        // cgltf keeps a string's JSON escapes.
        std::vector<char> buf(m->name, m->name + std::strlen(m->name) + 1);
        cgltf_decode_string(buf.data());
        out.name = buf.data();
    } else {
        out.name = "gltf_material_" + std::to_string(index);
    }
    if (!m) {
        out.frames.push_back("");
        return out;
    }
    const cgltf_texture* tex = m->has_pbr_metallic_roughness ? m->pbr_metallic_roughness.base_color_texture.texture : nullptr;
    out.frames.push_back(tex ? image_name(tex->image) : std::string());
    switch (m->alpha_mode) {
    case cgltf_alpha_mode_mask: out.blend = BlendMode::Masked; break;
    case cgltf_alpha_mode_blend: out.blend = BlendMode::Transparent50; break;
    default: out.blend = BlendMode::Opaque; break;
    }
    out.two_sided = m->double_sided != 0;
    return out;
}

std::uint64_t parse_source_hash(const cgltf_data* data) {
    if (!data->asset.extras.data) return 0;
    std::string_view json(data->asset.extras.data);
    constexpr std::string_view kKey = "\"hailstone_source\"";
    auto at = json.find(kKey);
    if (at == std::string_view::npos) return 0;
    at = json.find('"', json.find(':', at + kKey.size()));
    if (at == std::string_view::npos || at + 17 > json.size()) return 0;
    std::uint64_t v = 0;
    for (std::size_t i = at + 1; i < at + 17; ++i) {
        const char c = json[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v << 4 | static_cast<std::uint64_t>(d);
    }
    return v;
}

struct DataFree {
    void operator()(cgltf_data* d) const { cgltf_free(d); }
};

bool fail(std::string* error, std::string why) {
    if (error) *error = std::move(why);
    return false;
}

} // namespace

std::optional<GltfMesh> read_glb(std::span<const std::byte> bytes, std::string* error) {
    // glTF stores its numbers little-endian and cgltf reads them as the machine's: true of every platform built.
    cgltf_options options{};
    options.type = cgltf_file_type_glb;
    cgltf_data* raw = nullptr;
    if (cgltf_parse(&options, bytes.data(), bytes.size(), &raw) != cgltf_result_success) {
        fail(error, "not a binary glTF file");
        return std::nullopt;
    }
    std::unique_ptr<cgltf_data, DataFree> data(raw);
    for (cgltf_size i = 0; i < data->buffers_count; ++i)
        if (data->buffers[i].uri) {
            fail(error, "a buffer outside the file");
            return std::nullopt;
        }
    // No path: nothing outside the file is read.
    if (cgltf_load_buffers(&options, data.get(), nullptr) != cgltf_result_success ||
        cgltf_validate(data.get()) != cgltf_result_success) {
        fail(error, "a damaged glTF file");
        return std::nullopt;
    }

    GltfMesh out;
    out.source_hash = parse_source_hash(data.get());
    Mesh& mesh = out.mesh;
    mesh.front_ccw = true;
    for (cgltf_size i = 0; i < data->materials_count; ++i) mesh.materials.push_back(read_material(&data->materials[i], i));
    std::size_t default_material = std::numeric_limits<std::size_t>::max();

    const cgltf_scene* scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : nullptr;
    std::vector<const cgltf_node*> nodes;
    if (scene) {
        std::vector<const cgltf_node*> stack(scene->nodes, scene->nodes + scene->nodes_count);
        while (!stack.empty() && nodes.size() < 65536) {
            const cgltf_node* n = stack.back();
            stack.pop_back();
            nodes.push_back(n);
            for (cgltf_size c = 0; c < n->children_count; ++c) stack.push_back(n->children[c]);
        }
    }
    for (const cgltf_node* node : nodes) {
        if (!node->mesh) continue;
        float m[16];
        cgltf_node_transform_world(node, m);
        glm::mat4 world;
        std::memcpy(&world[0][0], m, sizeof m);
        const glm::mat3 normal_world = glm::transpose(glm::inverse(glm::mat3(world)));
        std::array<const void*, 4> last_key{};
        std::size_t last_base = 0;
        for (cgltf_size pi = 0; pi < node->mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node->mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *col = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& at = prim.attributes[a];
                if (at.type == cgltf_attribute_type_position) pos = at.data;
                else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
                else if (at.type == cgltf_attribute_type_color && at.index == 0) col = at.data;
            }
            if (!pos || pos->count == 0) continue;
            // Primitives of a node that share their vertex data (one part per material) share the vertices.
            const std::array<const void*, 4> key{pos, nrm, uv, col};
            const bool shared = pi > 0 && key == last_key;
            const std::size_t base = shared ? last_base : mesh.vertices.size();
            last_key = key;
            last_base = base;
            const std::size_t count = prim.indices ? prim.indices->count : pos->count;
            if (mesh.vertices.size() + pos->count > kMaxVertices || mesh.indices.size() + count > kMaxIndices) {
                fail(error, "too many vertices");
                return std::nullopt;
            }
            for (cgltf_size v = 0; v < (shared ? 0 : pos->count); ++v) {
                Vertex vx;
                float f[4] = {0, 0, 0, 1};
                cgltf_accessor_read_float(pos, v, f, 3);
                vx.position = from_gltf(glm::vec3(world * glm::vec4(f[0], f[1], f[2], 1.0f)));
                if (nrm && cgltf_accessor_read_float(nrm, v, f, 3)) {
                    glm::vec3 n = normal_world * glm::vec3(f[0], f[1], f[2]);
                    vx.normal = glm::dot(n, n) > 0.0f ? from_gltf(glm::normalize(n)) : glm::vec3(0, 0, 1);
                }
                if (uv && cgltf_accessor_read_float(uv, v, f, 2)) vx.uv = {f[0], f[1]};
                if (col) {
                    float c[4] = {1, 1, 1, 1};
                    cgltf_accessor_read_float(col, v, c, cgltf_num_components(col->type) >= 4 ? 4 : 3);
                    vx.color = pack_rgba(c);
                }
                mesh.vertices.push_back(vx);
            }
            MeshPart part;
            part.first_index = static_cast<std::uint32_t>(mesh.indices.size());
            const std::size_t tris = count / 3 * 3;
            for (std::size_t k = 0; k < tris; ++k) {
                const std::size_t idx = prim.indices ? cgltf_accessor_read_index(prim.indices, k) : k;
                if (idx >= pos->count) {
                    fail(error, "an index past its vertices");
                    return std::nullopt;
                }
                mesh.indices.push_back(static_cast<std::uint32_t>(base + idx));
            }
            // A transform that mirrors turns the winding round.
            if (glm::determinant(glm::mat3(world)) < 0.0f)
                for (std::size_t k = part.first_index; k + 2 < mesh.indices.size(); k += 3)
                    std::swap(mesh.indices[k + 1], mesh.indices[k + 2]);
            part.index_count = static_cast<std::uint32_t>(tris);
            if (prim.material) {
                part.material = static_cast<std::uint32_t>(prim.material - data->materials);
            } else {
                if (default_material == std::numeric_limits<std::size_t>::max()) {
                    default_material = mesh.materials.size();
                    mesh.materials.push_back(read_material(nullptr, default_material));
                }
                part.material = static_cast<std::uint32_t>(default_material);
            }
            part.solid = false;
            if (part.index_count) mesh.parts.push_back(part);
        }
    }
    if (mesh.parts.empty()) {
        fail(error, "no triangles");
        return std::nullopt;
    }
    return out;
}

namespace {

void json_string(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += buf;
        } else {
            out += c;
        }
    }
    out += '"';
}

std::string uri_encode(std::string_view s) {
    std::string out;
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '.' || c == '_' || c == '-') {
            out += c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", static_cast<unsigned>(u));
            out += buf;
        }
    }
    return out;
}

std::string num(float f) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(std::isfinite(f) ? f : 0.0f));
    return buf;
}

} // namespace

std::vector<std::byte> write_glb(const Mesh& mesh, std::uint64_t source_hash) {
    ByteWriter bin;
    const std::size_t n = mesh.vertices.size();
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    for (const auto& v : mesh.vertices) {
        const glm::vec3 p = to_gltf(v.position);
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
        bin.f32(p.x), bin.f32(p.y), bin.f32(p.z);
    }
    const std::size_t normals_at = bin.size();
    for (const auto& v : mesh.vertices) {
        const glm::vec3 p = to_gltf(v.normal);
        bin.f32(p.x), bin.f32(p.y), bin.f32(p.z);
    }
    const std::size_t uvs_at = bin.size();
    for (const auto& v : mesh.vertices) bin.f32(v.uv.x), bin.f32(v.uv.y);
    const std::size_t colors_at = bin.size();
    for (const auto& v : mesh.vertices) bin.le(v.color);
    const std::size_t vertex_bytes = bin.size();

    std::string views, accessors, prims;
    auto view = [&](std::size_t offset, std::size_t length, int target) {
        if (!views.empty()) views += ',';
        views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) + ",\"byteLength\":" + std::to_string(length) +
                 ",\"target\":" + std::to_string(target) + "}";
    };
    view(0, normals_at, 34962);
    view(normals_at, uvs_at - normals_at, 34962);
    view(uvs_at, colors_at - uvs_at, 34962);
    view(colors_at, vertex_bytes - colors_at, 34962);
    const std::string count = std::to_string(n);
    accessors = "{\"bufferView\":0,\"componentType\":5126,\"count\":" + count + ",\"type\":\"VEC3\",\"min\":[" + num(lo.x) +
                "," + num(lo.y) + "," + num(lo.z) + "],\"max\":[" + num(hi.x) + "," + num(hi.y) + "," + num(hi.z) + "]}";
    accessors += ",{\"bufferView\":1,\"componentType\":5126,\"count\":" + count + ",\"type\":\"VEC3\"}";
    accessors += ",{\"bufferView\":2,\"componentType\":5126,\"count\":" + count + ",\"type\":\"VEC2\"}";
    accessors += ",{\"bufferView\":3,\"componentType\":5121,\"normalized\":true,\"count\":" + count + ",\"type\":\"VEC4\"}";
    int next_view = 4, next_accessor = 4;
    for (const auto& part : mesh.parts) {
        const std::size_t at = bin.size();
        for (std::uint32_t k = 0; k + 2 < part.index_count; k += 3) {
            const std::uint32_t* t = &mesh.indices[part.first_index + k];
            bin.le(t[0]);
            bin.le(mesh.front_ccw ? t[1] : t[2]);
            bin.le(mesh.front_ccw ? t[2] : t[1]);
        }
        view(at, bin.size() - at, 34963);
        accessors += ",{\"bufferView\":" + std::to_string(next_view++) + ",\"componentType\":5125,\"count\":" +
                     std::to_string(part.index_count / 3 * 3) + ",\"type\":\"SCALAR\"}";
        if (!prims.empty()) prims += ',';
        prims += "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"COLOR_0\":3},\"indices\":" +
                 std::to_string(next_accessor++) + ",\"material\":" + std::to_string(part.material) + "}";
    }

    std::string materials, textures, images;
    for (std::size_t i = 0; i < mesh.materials.size(); ++i) {
        const Material& m = mesh.materials[i];
        if (i) materials += ',', textures += ',', images += ',';
        images += "{\"uri\":";
        json_string(images, uri_encode(m.frames.empty() ? std::string() : m.frames[0]));
        images += '}';
        textures += "{\"source\":" + std::to_string(i) + "}";
        materials += "{\"name\":";
        json_string(materials, m.name);
        materials += ",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":" + std::to_string(i) +
                     "},\"metallicFactor\":0,\"roughnessFactor\":1}";
        if (m.blend == BlendMode::Masked) materials += ",\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5";
        else if (m.blend != BlendMode::Opaque) materials += ",\"alphaMode\":\"BLEND\"";
        if (m.two_sided) materials += ",\"doubleSided\":true";
        materials += '}';
    }

    char hash[17];
    std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(source_hash));
    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Hailstone\"";
    if (source_hash) json += ",\"extras\":{\"hailstone_source\":\"" + std::string(hash) + "\"}";
    json += "},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0";
    json += ",\"name\":";
    json_string(json, mesh.name);
    json += "}],\"meshes\":[{\"primitives\":[" + prims + "]}]";
    if (!materials.empty())
        json += ",\"materials\":[" + materials + "],\"textures\":[" + textures + "],\"images\":[" + images + "]";
    json += ",\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]";
    json += ",\"bufferViews\":[" + views + "],\"accessors\":[" + accessors + "]}";
    while (json.size() % 4) json += ' ';
    std::vector<std::byte> body = bin.take();
    while (body.size() % 4) body.push_back(std::byte{0});

    ByteWriter out;
    out.le(std::uint32_t{0x46546c67}); // "glTF"
    out.le(std::uint32_t{2});
    out.le(static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + body.size()));
    out.le(static_cast<std::uint32_t>(json.size()));
    out.le(std::uint32_t{0x4e4f534a}); // "JSON"
    out.bytes({reinterpret_cast<const std::byte*>(json.data()), json.size()});
    out.le(static_cast<std::uint32_t>(body.size()));
    out.le(std::uint32_t{0x004e4942}); // "BIN\0"
    out.bytes(body);
    return out.take();
}

std::uint64_t object_mesh_hash(const Mesh& mesh) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    auto feed = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (8 * i)) & 0xff;
            h *= 0x100000001b3ull;
        }
    };
    auto bits = [](float f) {
        std::uint32_t b;
        std::memcpy(&b, &f, sizeof b);
        return b;
    };
    feed(static_cast<std::uint32_t>(mesh.vertices.size()));
    for (const auto& v : mesh.vertices) feed(bits(v.position.x)), feed(bits(v.position.y)), feed(bits(v.position.z));
    feed(static_cast<std::uint32_t>(mesh.indices.size()));
    for (std::uint32_t i : mesh.indices) feed(i);
    return h;
}

} // namespace hailstone::assets
