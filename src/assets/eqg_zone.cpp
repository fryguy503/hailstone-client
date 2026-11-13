// EQG zone loading: binary EQGZ zones (TER terrain + MOD objects) and
// heightmap (EQTZP) zones, into the same assets::Zone as classic zones.
// Formats: assets/eqg.h, docs/formats/eqg.md. The assembly rules (terrain
// drawn untransformed, material ordinals, collision flags, heightmap
// topology and texture layers) follow OpenEQ's notes on EQGraphicsDX9.dll
// (crates/openeq-assets/src/loader.rs, terrain/bake.rs; MIT OR Apache-2.0).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <unordered_set>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>

#include "assets/eqg.h"
#include "assets/flora.h"
#include "assets/texture.h"
#include "assets/wld.h"
#include "assets/zone.h"
#include "assets/zone_fires.h"
#include "core/asset_folder.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "core/log.h"

namespace hailstone::assets {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

bool finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// The native TER upload converts UVs of Opaque_MaxCB1 terrain to SHORT2 /
// 256 (truncate, keep the low 16 bits; NaN and overflow become 0); OpenEQ
// docs/EQG_TER_UV_PACKING.md.
float short2_uv(float v) {
    double p = static_cast<double>(v) * 256.0;
    if (!std::isfinite(p) || p >= 2147483648.0 || p < -2147483648.0) return 0.0f;
    auto i = static_cast<std::int32_t>(p);
    auto s = static_cast<std::int16_t>(static_cast<std::uint16_t>(static_cast<std::uint32_t>(i) & 0xffffu));
    return static_cast<float>(s) / 256.0f;
}

} // namespace

namespace eqg {

// CMaterial::InitFromShaderName @0x10014660 runs case-sensitive substring
// tests in this order: WaterFall, Water, Terrain, Lava2, Lava, AddAlpha,
// Alpha, Chroma, then the opaque suffixes. So Opaque_AddAlphaC1Max.fx is additive.
EqgShader shader_class(std::string_view shader) {
    if (contains(shader, "WaterFall")) return EqgShader::WaterFall;
    if (contains(shader, "Water")) return EqgShader::Water;
    if (contains(shader, "Terrain")) return EqgShader::Terrain;
    if (contains(shader, "Lava2")) return EqgShader::Lava2;
    if (contains(shader, "Lava")) return EqgShader::Lava;
    if (contains(shader, "AddAlpha")) return EqgShader::AddAlpha;
    if (contains(shader, "Alpha")) return EqgShader::Alpha;
    if (contains(shader, "Chroma")) return EqgShader::Chroma;
    return EqgShader::None;
}

// SelectRenderEffectForMaterial @0x1008f0d0 has no zone-mesh case for Alpha
// CG1/CB1/CBS1 and the opaque VSB and CBS_2UV classes (render classes 0xb,
// 0xc); the batch builder then abandons the batch ("CRender::
// CreateBatchWithoutColor() failed."). The suffix is read from the name's
// end (Alpha_MaxCB1.fx) or before "Max" (AlphaSModelCB1Max.fx); which
// substring tests InitFromShaderName runs for the suffixes is not traced.
bool region_effect_unmapped(std::string_view shader) {
    std::string_view core = shader;
    if (ends_with(core, ".fx")) core.remove_suffix(3);
    if (ends_with(core, "Max")) core.remove_suffix(3);
    auto suffix = [&](std::string_view s) { return ends_with(core, s); };
    switch (shader_class(shader)) {
    case EqgShader::Alpha: return suffix("CG1") || suffix("CB1") || suffix("CBS1");
    case EqgShader::None: return suffix("VSB") || suffix("CBS_2UV");
    default: return false;
    }
}

BlendMode shader_blend(std::string_view shader) {
    switch (shader_class(shader)) {
    case EqgShader::Water: return BlendMode::Transparent50; // not solid
    case EqgShader::AddAlpha: return BlendMode::Additive;
    case EqgShader::Alpha:
    case EqgShader::Chroma: return BlendMode::Masked;
    default: return BlendMode::Opaque;
    }
}

EffectParams effect_params(const MaterialDef& def, EqgShader cls) {
    EffectParams e;
    auto f = [&](std::string_view name, float fallback) {
        const Property* p = def.find(name);
        return p && p->type == 0 && std::isfinite(p->f) ? p->f : fallback;
    };
    auto color = [&](std::string_view name) {
        const Property* p = def.find(name);
        if (!p || p->type != 3) return glm::vec4(1.0f);
        std::uint32_t c = p->u; // ARGB
        return glm::vec4(static_cast<float>((c >> 16) & 255), static_cast<float>((c >> 8) & 255),
                         static_cast<float>(c & 255), static_cast<float>(c >> 24)) /
               255.0f;
    };
    e.slide = {f("e_fSlide1X", 0.0f), f("e_fSlide1Y", 0.0f), f("e_fSlide2X", 0.0f), f("e_fSlide2Y", 0.0f)};
    auto tex = [&](std::string_view name) { e.textures.push_back(lower(def.texture(name))); };
    switch (cls) {
    case EqgShader::Water:
    case EqgShader::LavaH:
        tex("e_TextureNormal0");
        tex("e_TextureEnvironment0");
        e.water_color1 = color("e_fWaterColor1");
        e.water_color2 = color("e_fWaterColor2");
        e.reflection_color = color("e_fReflectionColor");
        e.fresnel_bias = f("e_fFresnelBias", 0.0f);
        e.fresnel_power = f("e_fFresnelPower", 1.0f);
        e.reflection_amount = f("e_fReflectionAmount", 0.0f);
        e.uv_scale = f("e_fUVScale", 1.0f);
        break;
    case EqgShader::Lava:
    case EqgShader::Lava2:
        tex("e_TextureDiffuse1");
        tex("e_TextureNormal0");
        break;
    case EqgShader::Terrain:
        tex("e_TextureDetail2");
        tex("e_TextureCoverage");
        e.coverage_scale = f("e_fCoverageScale", 1.0f);
        break;
    case EqgShader::None:
    case EqgShader::Alpha:
    case EqgShader::Chroma: {
        e.normal_map = lower(def.texture("e_TextureNormal0"));
        e.shininess = f("e_fShininess0", 40.0f);
        e.gloss = f("e_fGloss0", 0.2f);
        e.env_strength = 2.0f * f("e_fEnvMapStrength0", 0.0f);
        const std::string& sh = def.shader;
        e.specular = contains(sh, "CBS") || contains(sh, "SB") || contains(sh, "GB") || contains(sh, "Full");
        e.reflective = contains(sh, "CE1") || contains(sh, "CBSGE1");
        break;
    }
    default: break;
    }
    return e;
}

// One mesh per model, vertices 1:1 with the file's (so per-vertex lighting
// arrays line up), one part per (material, solid) pair.
Mesh build_mesh(const ModelFile& m, std::string name, MeshStats& stats) {
    Mesh out;
    out.name = std::move(name);
    out.front_ccw = true;
    const std::size_t n = m.positions.size();
    out.vertices.resize(n);
    std::vector<bool> short2(n, false);
    if (m.terrain)
        for (const auto& t : m.triangles)
            if (const auto* mat = m.material_for(t.material); mat && mat->shader == "Opaque_MaxCB1.fx")
                for (auto v : t.v)
                    if (v < n) short2[v] = true;
    for (std::size_t i = 0; i < n; ++i) {
        auto& v = out.vertices[i];
        v.position = m.positions[i];
        if (i < m.normals.size() && finite(m.normals[i]) && glm::dot(m.normals[i], m.normals[i]) > 0.0f)
            v.normal = m.normals[i];
        glm::vec2 uv = i < m.uvs.size() ? m.uvs[i] : glm::vec2(0.0f);
        if (short2[i]) {
            uv = {short2_uv(uv.x), short2_uv(uv.y)};
        } else if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) {
            ++stats.bad_uvs;
            uv = glm::vec2(0.0f);
        }
        v.uv = uv;
        if (i < m.uvs2.size() && std::isfinite(m.uvs2[i].x) && std::isfinite(m.uvs2[i].y)) v.uv2 = m.uvs2[i];
        if (i < m.colors.size()) v.color = wld::bgra_to_rgba(m.colors[i]);
    }

    // Resolve each ordinal to its first same-named record, then bucket.
    std::map<std::pair<std::int64_t, bool>, std::vector<std::uint32_t>> buckets;
    for (const auto& t : m.triangles) {
        if (t.v[0] >= n || t.v[1] >= n || t.v[2] >= n || !finite(m.positions[t.v[0]]) ||
            !finite(m.positions[t.v[1]]) || !finite(m.positions[t.v[2]])) {
            ++stats.bad_triangles;
            continue;
        }
        std::int64_t mat = -1;
        if (t.material != kNoMaterial) {
            const MaterialDef* def = m.material_for(t.material);
            if (!def) {
                ++stats.bad_triangles;
                continue;
            }
            mat = def - m.materials.data();
        }
        bool solid = !(t.flags & kTrianglePassable);
        auto& b = buckets[{mat, solid}];
        b.insert(b.end(), {t.v[0], t.v[1], t.v[2]});
    }
    std::map<std::int64_t, std::uint32_t> material_slot;
    for (auto& [key, indices] : buckets) {
        auto [mat, solid] = key;
        auto slot = material_slot.find(mat);
        if (slot == material_slot.end()) {
            Material out_mat;
            if (mat < 0) {
                out_mat.name = "(none)";
                out_mat.blend = BlendMode::Invisible; // material -1: solid, not drawn
            } else {
                const auto& def = m.materials[static_cast<std::size_t>(mat)];
                out_mat.name = def.name;
                out_mat.blend = shader_blend(def.shader);
                out_mat.eqg_shader = shader_class(def.shader);
                out_mat.effect = effect_params(def, out_mat.eqg_shader);
                // RegionTerrain samples e_TextureDetail1 (and Detail2,
                // Coverage); RegionWater's colour does not use a diffuse map,
                // so its normal map stands in when there is none.
                std::string diffuse = def.texture(out_mat.eqg_shader == EqgShader::Terrain ? "e_TextureDetail1"
                                                                                          : "e_TextureDiffuse0");
                if (diffuse.empty() && out_mat.eqg_shader == EqgShader::Water) diffuse = def.texture("e_TextureNormal0");
                if (m.terrain && region_effect_unmapped(def.shader)) out_mat.eqg_shader = EqgShader::Unmapped;
                // The CB* / CBS* region classes carry MPL triples (notes, EQG material shaders).
                out_mat.multipass_lighting = out_mat.eqg_shader == EqgShader::None && contains(def.shader, "CB");
                if (diffuse.empty()) out_mat.blend = BlendMode::Invisible;
                else out_mat.frames.push_back(lower(diffuse));
            }
            slot = material_slot.emplace(mat, static_cast<std::uint32_t>(out.materials.size())).first;
            out.materials.push_back(std::move(out_mat));
        }
        MeshPart part;
        part.first_index = static_cast<std::uint32_t>(out.indices.size());
        part.index_count = static_cast<std::uint32_t>(indices.size());
        part.material = slot->second;
        // Water is never solid (EQEmu's map generator and OpenEQ eqg_collision.rs).
        part.solid = solid && out.materials[slot->second].blend != BlendMode::Transparent50;
        out.indices.insert(out.indices.end(), indices.begin(), indices.end());
        out.parts.push_back(part);
    }
    return out;
}

glm::mat4 placement_transform(const ZonPlacement& p) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), p.position);
    m = glm::rotate(m, p.rotation.x, glm::vec3(0, 0, 1));
    m = glm::rotate(m, p.rotation.y, glm::vec3(0, 1, 0));
    m = glm::rotate(m, p.rotation.z, glm::vec3(1, 0, 0));
    float s = std::isfinite(p.scale) && p.scale > 0.0f ? p.scale : 1.0f;
    return glm::scale(m, glm::vec3(s));
}

} // namespace eqg

namespace {

struct Source {
    std::shared_ptr<const PfsArchive> archive;
    const GameDir* game = nullptr;

    // A member of the archive, else a loose file of the install.
    std::optional<std::vector<std::byte>> read(const std::string& name) const {
        if (archive->contains(name)) return archive->read(name);
        return game->read(name);
    }
};

void grow(Zone& zone, bool& any, glm::vec3 p) {
    if (!finite(p)) return;
    zone.bounds_min = any ? glm::min(zone.bounds_min, p) : p;
    zone.bounds_max = any ? glm::max(zone.bounds_max, p) : p;
    any = true;
}

void compute_bounds(Zone& zone) {
    bool any = false;
    for (const auto& m : zone.terrain)
        for (const auto& v : m.vertices) grow(zone, any, v.position);
    std::unordered_map<std::string, std::pair<glm::vec3, glm::vec3>> boxes;
    for (const auto& [key, m] : zone.objects) {
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
        bool some = false;
        for (const auto& v : m.vertices)
            if (finite(v.position)) {
                lo = glm::min(lo, v.position);
                hi = glm::max(hi, v.position);
                some = true;
            }
        if (some) boxes.emplace(key, std::make_pair(lo, hi));
    }
    for (const auto& p : zone.placements) {
        auto box = boxes.find(p.actor);
        if (box == boxes.end()) continue;
        auto [lo, hi] = box->second;
        for (int c = 0; c < 8; ++c) {
            glm::vec3 corner(c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z);
            grow(zone, any, glm::vec3(p.transform * glm::vec4(corner, 1.0f)));
        }
    }
}

// Per-vertex lighting words (BGRA, as WLD object colours) when they match the mesh.
std::vector<std::uint32_t> lighting_colors(const std::vector<std::uint32_t>& words, const Mesh& mesh) {
    std::vector<std::uint32_t> out;
    if (words.empty() || words.size() != mesh.vertices.size()) return out;
    out.reserve(words.size());
    for (auto w : words) out.push_back(wld::bgra_to_rgba(w));
    return out;
}

// An "EQGP" .lit file: u32 magic, u32 count, count lighting words.
std::vector<std::uint32_t> parse_lit(std::span<const std::byte> data) {
    std::vector<std::uint32_t> out;
    try {
        ByteReader r(data);
        if (r.le<std::uint32_t>() != 0x50475145u) return out; // "EQGP"
        auto n = r.le<std::uint32_t>();
        if (n > r.remaining() / 4) return out;
        out.resize(n);
        for (auto& w : out) w = r.le<std::uint32_t>();
    } catch (const std::exception&) {
        out.clear();
    }
    return out;
}

// ---- Binary EQGZ zones ----

void load_binary(Zone& zone, const Source& src, std::span<const std::byte> zon_data) {
    auto zon = eqg::parse_zon(zon_data);
    std::vector<std::string> keys(zon.models.size()); // object key per model; empty = terrain or missing
    std::size_t missing = 0;
    eqg::MeshStats stats;
    // Each model file is read at most once. A .ter listed again is the same terrain: loading it twice would draw and
    // collide with it twice. A .mod listed again reuses the loaded object; one that was missing or failed to parse
    // is not read again, so a forged list of one name repeated costs one read, not one per entry.
    std::unordered_set<std::string> tried;
    for (std::size_t i = 0; i < zon.models.size(); ++i) {
        std::string file = lower(zon.models[i]);
        bool terrain = ends_with(file, ".ter");
        if (!terrain && !ends_with(file, ".mod")) continue;
        std::string key = upper(file);
        if (!terrain && zone.objects.contains(key)) {
            keys[i] = key;
            continue;
        }
        if (!tried.insert(key).second) continue;
        auto data = src.read(file);
        if (!data) {
            ++missing;
            log::debug("{}: model {} not found", zone.name, file);
            continue;
        }
        try {
            auto model = eqg::parse_model(*data, terrain);
            Mesh mesh = eqg::build_mesh(model, key, stats);
            // Terrain is registered with its file coordinates; its ZON
            // placement is not applied (OpenEQ docs/EQGZ_NATIVE_REGIONS.md,
            // EQGraphicsDX9.dll 0x10020970).
            if (terrain) {
                zone.terrain.push_back(std::move(mesh));
            } else {
                zone.objects.emplace(key, std::move(mesh));
                keys[i] = key;
            }
        } catch (const std::exception& e) {
            log::warn("{}: model {}: {}", zone.name, file, e.what());
        }
    }
    if (missing) log::warn("{}: {} of {} models missing", zone.name, missing, zon.models.size());
    if (stats.bad_triangles || stats.bad_uvs)
        log::info("{}: dropped {} triangles with bad indices or non-finite positions, zeroed {} non-finite UVs",
                  zone.name, stats.bad_triangles, stats.bad_uvs);

    std::size_t lit = 0;
    for (const auto& p : zon.placements) {
        if (p.model < 0 || static_cast<std::size_t>(p.model) >= keys.size() ||
            keys[static_cast<std::size_t>(p.model)].empty())
            continue;
        if (!finite(p.position) || !finite(p.rotation)) continue;
        Placement out;
        out.actor = keys[static_cast<std::size_t>(p.model)];
        out.transform = eqg::placement_transform(p);
        const Mesh& mesh = zone.objects.at(out.actor);
        out.vertex_colors = lighting_colors(p.lighting, mesh);
        if (out.vertex_colors.empty() && !p.name.empty())
            if (auto data = src.archive->read(lower(p.name) + ".lit"))
                out.vertex_colors = lighting_colors(parse_lit(*data), mesh);
        if (!out.vertex_colors.empty()) ++lit;
        zone.placements.push_back(std::move(out));
    }
    log::debug("{}: {} of {} placements have vertex lighting", zone.name, lit, zone.placements.size());

    for (const auto& l : zon.lights) {
        PointLight light;
        // Stored as (y, -x, z) relative to the geometry: OpenEQ zone.rs reads
        // them so, and 380 of Anguish's 452 lights then lie within 20 units of
        // its geometry (7 as stored); docs/formats/eqg.md.
        light.position = {l.position.y, -l.position.x, l.position.z};
        light.color = l.color;
        light.radius = l.radius;
        if (finite_light(light)) zone.lights.push_back(light);
    }

    std::vector<RegionBox> boxes;
    for (const auto& g : zon.regions) boxes.push_back({g.name, g.center, g.angles, g.extents});
    zone.regions = ZoneRegions::from_boxes(boxes);
}

// ---- Heightmap (EQTZP) zones ----

// An uncompressed 32-bit DDS (A8R8G8B8) for a baked tile.
std::vector<std::byte> make_dds(std::uint32_t w, std::uint32_t h, const std::vector<std::uint8_t>& rgba) {
    ByteWriter out;
    out.le<std::uint32_t>(0x20534444u); // "DDS "
    out.le<std::uint32_t>(124);
    out.le<std::uint32_t>(0x1 | 0x2 | 0x4 | 0x8 | 0x1000); // caps, height, width, pitch, pixel format
    out.le<std::uint32_t>(h);
    out.le<std::uint32_t>(w);
    out.le<std::uint32_t>(w * 4);
    out.le<std::uint32_t>(0); // depth
    out.le<std::uint32_t>(1); // mip count
    out.zeros(44);
    out.le<std::uint32_t>(32);         // pixel format size
    out.le<std::uint32_t>(0x40 | 0x1); // RGB | ALPHAPIXELS
    out.le<std::uint32_t>(0);
    out.le<std::uint32_t>(32);
    out.le<std::uint32_t>(0x00ff0000u);
    out.le<std::uint32_t>(0x0000ff00u);
    out.le<std::uint32_t>(0x000000ffu);
    out.le<std::uint32_t>(0xff000000u);
    out.le<std::uint32_t>(0x1000); // caps: texture
    out.zeros(16);
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        out.le<std::uint8_t>(rgba[i + 2]);
        out.le<std::uint8_t>(rgba[i + 1]);
        out.le<std::uint8_t>(rgba[i]);
        out.le<std::uint8_t>(rgba[i + 3]);
    }
    return out.take();
}

struct Rgba {
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> px;

    // Bilinear, wrapping.
    glm::vec4 sample(float u, float v) const {
        float x = (u - std::floor(u)) * static_cast<float>(w) - 0.5f;
        float y = (v - std::floor(v)) * static_cast<float>(h) - 0.5f;
        float fx = std::floor(x), fy = std::floor(y);
        auto texel = [&](float tx, float ty) {
            auto ix = static_cast<std::int64_t>(tx), iy = static_cast<std::int64_t>(ty);
            ix = ((ix % w) + w) % w;
            iy = ((iy % h) + h) % h;
            const std::uint8_t* p = &px[(static_cast<std::size_t>(iy) * w + static_cast<std::size_t>(ix)) * 4];
            return glm::vec4(p[0], p[1], p[2], p[3]) / 255.0f;
        };
        float ax = x - fx, ay = y - fy;
        return glm::mix(glm::mix(texel(fx, fy), texel(fx + 1, fy), ax),
                        glm::mix(texel(fx, fy + 1), texel(fx + 1, fy + 1), ax), ay);
    }
};

float mask_at(const eqg::HeightmapLayer& layer, float u, float v) {
    const auto size = layer.mask_size;
    if (size == 0 || layer.mask.size() < static_cast<std::size_t>(size) * size) return 1.0f;
    float x = std::clamp(u * static_cast<float>(size) - 0.5f, 0.0f, static_cast<float>(size - 1));
    float y = std::clamp(v * static_cast<float>(size) - 0.5f, 0.0f, static_cast<float>(size - 1));
    auto ix = static_cast<std::uint32_t>(x), iy = static_cast<std::uint32_t>(y);
    auto at = [&](std::uint32_t a, std::uint32_t b) {
        return static_cast<float>(layer.mask[std::min(b, size - 1) * size + std::min(a, size - 1)]) / 255.0f;
    };
    float fx = x - static_cast<float>(ix), fy = y - static_cast<float>(iy);
    float top = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * fx;
    float bottom = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * fx;
    return top + (bottom - top) * fy;
}


// Picks the declaration: <name>.zon in the archive, a loose <name>.zon, or the
// archive's one usable declaration (feerrott2 holds feerrott.zon, oldcommons
// commonlands.zon; OpenEQ loader.rs read_eqg_declaration).
std::vector<std::byte> find_declaration(const Source& src, const std::string& name) {
    // A heightmap declaration is usable only with its .dat: the archived
    // oldcommons.zon names one the archive lacks, and the scan below finds commonlands.zon.
    auto usable_heightmap = [&](const std::vector<std::byte>& d) {
        try {
            return src.archive->contains(eqg::parse_heightmap_zon(d).name + ".dat");
        } catch (const std::exception&) {
            return false;
        }
    };
    for (auto d : {src.archive->read(name + ".zon"), src.game->read(name + ".zon")})
        if (d && (!eqg::is_heightmap_zon(*d) || usable_heightmap(*d))) return *d;
    std::optional<std::vector<std::byte>> chosen;
    std::string chosen_name;
    for (const auto& member : src.archive->names()) {
        if (!ends_with(member, ".zon")) continue;
        auto d = src.archive->read(member);
        if (!d) continue;
        bool usable = false;
        try {
            if (eqg::is_heightmap_zon(*d)) {
                usable = usable_heightmap(*d);
            } else {
                auto z = eqg::parse_zon(*d);
                for (const auto& m : z.models) usable |= ends_with(lower(m), ".ter") && src.archive->contains(m);
            }
        } catch (const std::exception&) {
        }
        if (!usable) continue;
        if (chosen && *chosen != *d)
            throw std::runtime_error("archive has several zone declarations (" + chosen_name + ", " + member + ")");
        chosen = std::move(d);
        chosen_name = member;
    }
    if (!chosen) throw std::runtime_error("no zone declaration (" + name + ".zon) in " + src.archive->label());
    return *chosen;
}

void load_heightmap(Zone& zone, const Source& src, std::span<const std::byte> zon_data) {
    auto options = eqg::parse_heightmap_zon(zon_data);
    auto dat = src.archive->read(options.name + ".dat");
    if (!dat) throw std::runtime_error("heightmap " + options.name + ".dat not found");
    auto map = eqg::parse_heightmap(options, *dat);
    const std::uint32_t q = options.quads_per_tile;
    const float step = options.units_per_vertex;
    const float tile_size = options.tile_size();

    // Ecosystems (.eco texture layers) by name.
    std::map<std::string, std::vector<eqg::EcoLayer>> ecos;
    std::map<std::string, std::optional<Rgba>> images;
    auto image = [&](const std::string& file) -> const Rgba* {
        if (file.empty() || file == "default.bmp") return nullptr; // the DLL's "no map" name
        auto key = lower(file);
        auto it = images.find(key);
        if (it == images.end()) {
            std::optional<Rgba> out;
            if (auto data = src.read(key))
                if (auto img = decode_texture(*data)) {
                    auto rgba = to_rgba8(*img);
                    Rgba r;
                    r.w = rgba.width;
                    r.h = rgba.height;
                    r.px.resize(rgba.mips[0].size());
                    for (std::size_t i = 0; i < r.px.size(); ++i) r.px[i] = std::to_integer<std::uint8_t>(rgba.mips[0][i]);
                    if (r.w && r.h && r.px.size() >= static_cast<std::size_t>(r.w) * r.h * 4) out = std::move(r);
                }
            if (!out) log::debug("{}: terrain texture {} unavailable", zone.name, key);
            it = images.emplace(key, std::move(out)).first;
        }
        return it->second ? &*it->second : nullptr;
    };
    // Their flora layers (*FLORAPART) and the shapes they name (RadialFloraDefs.rfd).
    std::map<std::string, std::vector<eqg::EcoFloraLayer>> eco_flora;
    for (const auto& tile : map.tiles)
        for (const auto& layer : tile.layers) {
            auto key = lower(layer.ecosystem);
            if (ecos.contains(key)) continue;
            auto data = src.read(key + ".eco");
            ecos[key] = data ? eqg::parse_eco(*data) : std::vector<eqg::EcoLayer>{};
            eco_flora[key] = data ? eqg::parse_eco_flora(*data) : std::vector<eqg::EcoFloraLayer>{};
        }
    if (auto rfd = src.read("radialfloradefs.rfd")) zone.flora.definitions = eqg::parse_radial_flora(*rfd);
    zone.flora.tile_size = tile_size;

    // Heights by global grid coordinate, for normals across tile seams.
    std::unordered_map<std::int64_t, float> heights;
    auto grid_key = [](std::int64_t x, std::int64_t y) { return (x << 32) ^ (y & 0xffffffffLL); };
    for (const auto& tile : map.tiles)
        for (std::uint32_t row = 0; row <= q; ++row)
            for (std::uint32_t col = 0; col <= q; ++col)
                heights[grid_key(static_cast<std::int64_t>(tile.lng) * q + col,
                                 static_cast<std::int64_t>(tile.lat) * q + row)] = tile.heights[row * (q + 1) + col];

    // The tile texture size S (tile +0x14; the DLL picks 4..512). Not traced:
    // 64, halved while one texture kind would exceed 16M texels.
    std::uint32_t S = 64;
    while (S > 16 && static_cast<std::size_t>(S) * S * map.tiles.size() > 16u * 1024 * 1024) S /= 2;
    const float half = 0.5f / static_cast<float>(S);
    const std::uint32_t N = q + 1;

    Mesh terrain;
    terrain.name = upper(options.name) + "_TERRAIN";
    terrain.front_ccw = true; // the quads below are wound counter-clockwise seen from above
    terrain.heightmap_terrain = true;
    // Detail masks of each tile's first ecosystem, for the border average.
    struct MaskRef { std::size_t texture; std::string eco; };
    std::map<std::pair<std::int32_t, std::int32_t>, MaskRef> base_masks;
    std::vector<std::vector<std::uint8_t>> mask_pixels; // RGBA per generated mask
    std::vector<std::string> mask_names;
    std::size_t too_many_layers = 0;
    // A tile the editor never painted (no DAT layers; scorchedwoods has one in the canyon) is not drawn by the
    // original, which leaves a hole. Here it takes the base ecosystem of the nearest painted tile, its texture layers
    // laid by this tile's own heights and slopes, so the ground runs on across it.
    std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> painted;
    for (std::size_t t = 0; t < map.tiles.size(); ++t)
        if (!map.tiles[t].layers.empty()) painted.emplace(std::pair{map.tiles[t].lng, map.tiles[t].lat}, t);
    std::size_t unpainted = 0;
    for (std::size_t t = 0; t < map.tiles.size(); ++t) {
        const auto& tile = map.tiles[t];
        std::vector<eqg::HeightmapLayer> borrowed;
        if (tile.layers.empty()) {
            if (auto near = eqg::nearest_painted_tile(tile.lng, tile.lat, painted)) {
                borrowed.push_back({map.tiles[*near].layers.front().ecosystem, 0, {}});
                ++unpainted;
                log::debug("{}: the unpainted tile at ({}, {}) takes {}", zone.name, static_cast<float>(tile.lng) * tile_size,
                           static_cast<float>(tile.lat) * tile_size, borrowed.back().ecosystem);
            } else {
                continue;
            }
        }
        const std::vector<eqg::HeightmapLayer>& tile_layers = borrowed.empty() ? tile.layers : borrowed;
        auto vbase = static_cast<std::uint32_t>(terrain.vertices.size());
        std::vector<glm::vec3> normals(N * N);
        // TerrainTileChunk_BuildVertices @0x100ac170: UV0 has a half-texel
        // inset, UV1 runs 0..1 over the tile; both are packed x 256 and truncated.
        auto uv0 = [&](std::uint32_t i) {
            float f = static_cast<float>(i) / static_cast<float>(q);
            return std::trunc(((1.0f - 2.0f * half) * f + half) * 256.0f) / 256.0f;
        };
        auto uv1 = [&](std::uint32_t i) {
            return std::trunc(static_cast<float>(i) / static_cast<float>(q) * 256.0f) / 256.0f;
        };
        for (std::uint32_t row = 0; row <= q; ++row)
            for (std::uint32_t col = 0; col <= q; ++col) {
                std::int64_t gx = static_cast<std::int64_t>(tile.lng) * q + col;
                std::int64_t gy = static_cast<std::int64_t>(tile.lat) * q + row;
                float h = tile.heights[row * N + col];
                auto at = [&](std::int64_t x, std::int64_t y) -> std::optional<float> {
                    auto it = heights.find(grid_key(x, y));
                    return it == heights.end() ? std::nullopt : std::optional<float>(it->second);
                };
                auto l = at(gx - 1, gy), r = at(gx + 1, gy), d = at(gx, gy - 1), u = at(gx, gy + 1);
                float dx = (r.value_or(h) - l.value_or(h)) / (step * (l && r ? 2.0f : 1.0f));
                float dy = (u.value_or(h) - d.value_or(h)) / (step * (u && d ? 2.0f : 1.0f));
                glm::vec3 normal = glm::normalize(glm::vec3(-dx, -dy, 1.0f));
                normals[row * N + col] = normal;
                Vertex v;
                v.position = {static_cast<float>(gx) * step, static_cast<float>(gy) * step, h};
                v.normal = normal;
                v.uv = {uv0(col), uv0(row)};
                v.uv2 = {uv1(col), uv1(row)};
                // Vertex +0xc (the shaders' v1) is the tile's colour array read
                // by 0x100f1870 -> 0x100fdcc0; taken as the DAT's first array
                // (0x808080 in Moors: neutral under the shaders' x 2). Vertex
                // +8 (C of the lighting formula) is computed by 0x100f2190 from
                // the neighbouring heights through terrain vtable +0x25c, which
                // is not traced; the DAT's second array stands in (0xff000000
                // in Moors: no baked light, full ambient and sun weight).
                v.color2 = wld::bgra_to_rgba(tile.colors[row * N + col]);
                v.color = row * N + col < tile.colors2.size() ? wld::bgra_to_rgba(tile.colors2[row * N + col]) : 0xff000000u;
                terrain.vertices.push_back(v);
            }
        auto first_index = static_cast<std::uint32_t>(terrain.indices.size());
        for (std::uint32_t row = 0; row < q; ++row)
            for (std::uint32_t col = 0; col < q; ++col) {
                auto flags = tile.quad_flags[row * q + col];
                if (flags & 0x01) continue; // hidden quad
                std::uint32_t a = vbase + row * N + col, b = a + N;
                if (flags & 0x80) terrain.indices.insert(terrain.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
                else terrain.indices.insert(terrain.indices.end(), {a, a + 1, b + 1, a, b + 1, b});
            }
        auto index_count = static_cast<std::uint32_t>(terrain.indices.size()) - first_index;
        if (index_count == 0) continue;

        // Per texel: the bilinear height of the tile grid and the slope in
        // whole degrees (texel x sits at grid column x q / (S - 1), where UV0
        // puts the texel centres on the tile edges).
        std::vector<float> th(S * S);
        std::vector<std::int32_t> ts(S * S);
        for (std::uint32_t y = 0; y < S; ++y)
            for (std::uint32_t x = 0; x < S; ++x) {
                float gx = static_cast<float>(x) * static_cast<float>(q) / static_cast<float>(S - 1);
                float gy = static_cast<float>(y) * static_cast<float>(q) / static_cast<float>(S - 1);
                auto cx = std::min(static_cast<std::uint32_t>(gx), q - 1), cy = std::min(static_cast<std::uint32_t>(gy), q - 1);
                float fx = gx - static_cast<float>(cx), fy = gy - static_cast<float>(cy);
                auto H = [&](std::uint32_t c, std::uint32_t r) { return tile.heights[r * N + c]; };
                auto Nn = [&](std::uint32_t c, std::uint32_t r) { return normals[r * N + c]; };
                th[y * S + x] = (H(cx, cy) * (1 - fx) + H(cx + 1, cy) * fx) * (1 - fy) +
                                (H(cx, cy + 1) * (1 - fx) + H(cx + 1, cy + 1) * fx) * fy;
                glm::vec3 n = (Nn(cx, cy) * (1 - fx) + Nn(cx + 1, cy) * fx) * (1 - fy) +
                              (Nn(cx, cy + 1) * (1 - fx) + Nn(cx + 1, cy + 1) * fx) * fy;
                float nz = glm::length(n) > 0.0f ? glm::normalize(n).z : 1.0f;
                ts[y * S + x] = static_cast<std::int32_t>(glm::degrees(std::acos(std::clamp(nz, -1.0f, 1.0f))));
            }
        auto sample = [&](const Rgba* img, float u, float v) {
            return img ? img->sample(u, v) * 255.0f : glm::vec4(255.0f);
        };

        const auto flora_first = static_cast<std::uint32_t>(zone.flora.instances.size());
        // Each DAT layer is an ecosystem; the first is opaque, the others are
        // drawn over it with their DAT mask as the ColorMap alpha.
        for (std::size_t e = 0; e < tile_layers.size() && e < 11; ++e) {
            const auto& layers = ecos[lower(tile_layers[e].ecosystem)];
            const std::size_t n = std::min<std::size_t>(layers.size(), 10); // layer index above 9 ignored
            if (n == 0) continue;
            std::vector<std::vector<std::uint8_t>> masks(n, std::vector<std::uint8_t>(S * S, 0));
            std::vector<std::int32_t> acc(S * S, 0);
            // From the last declared layer back to the second; the first is
            // the base and takes 255 - acc.
            for (std::size_t k = n; k-- > 1;) {
                const auto& L = layers[k];
                const Rgba* layering = image(L.layering_map);
                const Rgba* blend = image(L.blend_map);
                const std::int32_t area = std::max(L.layering_area, 1);
                for (std::uint32_t y = 0; y < S; ++y)
                    for (std::uint32_t x = 0; x < S; ++x) {
                        std::size_t i = y * S + x;
                        std::int32_t lay = -1, bl = -1;
                        if (layering) {
                            // ((lng mod area) S + x, (lat mod area) S + y) in the
                            // map stretched to area S (raw lng/lat, 100000-based).
                            // In 64 bits: a damaged file's lng, lat or LAYERINGAREA near int32's limits would overflow.
                            auto px = static_cast<float>(((std::int64_t{tile.lng} + 100000) % area) * std::int64_t{S}) + static_cast<float>(x);
                            auto py = static_cast<float>(((std::int64_t{tile.lat} + 100000) % area) * std::int64_t{S}) + static_cast<float>(y);
                            float span = static_cast<float>(area) * static_cast<float>(S);
                            lay = static_cast<std::int32_t>(layering->sample((px + 0.5f) / span, (py + 0.5f) / span).r * 255.0f);
                        }
                        if (blend)
                            bl = static_cast<std::int32_t>(blend->sample((static_cast<float>(x) + 0.5f) / static_cast<float>(S),
                                                                         (static_cast<float>(y) + 0.5f) / static_cast<float>(S)).r * 255.0f);
                        if (auto w = eqg::eco_layer_weight(L, th[i], ts[i], lay, bl))
                            masks[k][i] = eqg::eco_accumulate(*w, acc[i]);
                    }
            }
            for (std::size_t i = 0; i < S * S; ++i) masks[0][i] = static_cast<std::uint8_t>(255 - std::min(acc[i], 255));

            // The ecosystem's radial flora on this tile (assets/flora.h), on its texture layers' masks within the
            // ecosystem's DAT mask.
            if (const auto& flora = eco_flora[lower(tile_layers[e].ecosystem)];
                !flora.empty() && !zone.flora.definitions.empty()) {
                std::vector<std::string> names;
                for (std::size_t k = 0; k < n; ++k) names.push_back(lower(layers[k].name));
                std::vector<std::uint8_t> mask;
                if (e > 0) {
                    mask.resize(S * S);
                    for (std::uint32_t y = 0; y < S; ++y)
                        for (std::uint32_t x = 0; x < S; ++x)
                            mask[y * S + x] = static_cast<std::uint8_t>(std::lround(
                                std::clamp(mask_at(tile_layers[e], (static_cast<float>(x) + 0.5f) / static_cast<float>(S),
                                                   (static_cast<float>(y) + 0.5f) / static_cast<float>(S)),
                                           0.0f, 1.0f) * 255.0f));
                }
                FloraTile ft;
                ft.origin = glm::vec2(static_cast<float>(tile.lng), static_cast<float>(tile.lat)) * tile_size;
                ft.tile_size = tile_size;
                ft.texels = S;
                ft.heights = th;
                ft.slopes = ts;
                ft.layer_names = names;
                ft.layer_masks = masks;
                ft.dat_mask = mask;
                ft.height_at = [&](float u, float v) { return tile.height_at(options, u * tile_size, v * tile_size); };
                ft.color_at = [&](float u, float v) -> std::uint32_t {
                    auto col = std::min(static_cast<std::uint32_t>(std::lround(u * static_cast<float>(q))), q);
                    auto row = std::min(static_cast<std::uint32_t>(std::lround(v * static_cast<float>(q))), q);
                    return row * N + col < tile.colors.size() ? wld::bgra_to_rgba(tile.colors[row * N + col]) : 0xffffffffu;
                };
                // The tile's own sequence, the same at every load.
                MsvcRand rand{flora_tile_seed(tile.lng, tile.lat, e)};
                place_flora(ft, flora, zone.flora.definitions, rand, zone.flora.instances);
            }

            // ColorMap: the base layer's COVERMAP x mask0, then each later
            // layer's COVERMAP x its mask added (byte arithmetic).
            std::vector<std::uint8_t> color(S * S * 4, 0);
            for (std::size_t k = 0; k < n; ++k) {
                const Rgba* cover = image(layers[k].cover_map);
                for (std::uint32_t y = 0; y < S; ++y)
                    for (std::uint32_t x = 0; x < S; ++x) {
                        std::size_t i = y * S + x;
                        glm::vec4 c = sample(cover, (static_cast<float>(x) + 0.5f) / static_cast<float>(S),
                                             (static_cast<float>(y) + 0.5f) / static_cast<float>(S));
                        for (int ch = 0; ch < 3; ++ch)
                            color[i * 4 + ch] = eqg::eco_color(color[i * 4 + ch], static_cast<std::uint8_t>(std::lround(c[ch])),
                                                               masks[k][i], k == 0);
                    }
            }
            for (std::uint32_t y = 0; y < S; ++y)
                for (std::uint32_t x = 0; x < S; ++x) {
                    float a = e == 0 ? 1.0f
                                     : mask_at(tile_layers[e], (static_cast<float>(x) + 0.5f) / static_cast<float>(S),
                                               (static_cast<float>(y) + 0.5f) / static_cast<float>(S));
                    color[(y * S + x) * 4 + 3] = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
                }
            // DetailMaskMap: detail layer k's mask in byte 2 - k of the
            // A8R8G8B8 texel, so k = 0 -> R, 1 -> G, 2 -> B
            // (TerrainTileChunk_UploadDetailMask @0x100ac770).
            std::vector<std::uint8_t> mask(S * S * 4, 0);
            for (std::size_t i = 0; i < S * S; ++i) {
                for (std::size_t k = 0; k < std::min<std::size_t>(n, 3); ++k) mask[i * 4 + k] = masks[k][i];
                mask[i * 4 + 3] = 255;
            }
            std::string stem = "__terrain_" + lower(options.name) + "_" + std::to_string(t) + "_" + std::to_string(e);
            std::string color_name = stem + "_c.dds", mask_name = stem + "_m.dds";
            zone.generated_files.emplace(color_name, make_dds(S, S, color));
            if (e == 0) base_masks[{tile.lng, tile.lat}] = {mask_pixels.size(), lower(tile_layers[0].ecosystem)};
            mask_pixels.push_back(std::move(mask));
            mask_names.push_back(mask_name);

            Material mat;
            mat.name = stem;
            mat.eqg_shader = EqgShader::TerrainTile;
            mat.frames.push_back(color_name);
            mat.effect.textures = {mask_name};
            const std::size_t details = std::min<std::size_t>(n, 3);
            if (n > 3) ++too_many_layers; // CreateTerrainTileNode leaves the type unset; drawn with three here
            for (std::size_t k = 0; k < 3; ++k) {
                mat.effect.textures.push_back(k < details ? lower(layers[k].detail_map) : std::string());
                mat.effect.detail_scale[static_cast<int>(k)] = k < details ? static_cast<float>(layers[k].detail_repeat) : 1.0f;
            }
            mat.effect.detail_scale.w = static_cast<float>(details);
            mat.effect.overlay = e > 0;
            mat.blend = e > 0 ? BlendMode::Transparent50 : BlendMode::Opaque; // for tools; drawn by class
            MeshPart part;
            part.first_index = first_index;
            part.index_count = index_count;
            part.material = static_cast<std::uint32_t>(terrain.materials.size());
            part.solid = e == 0; // the overlays repeat the same triangles
            terrain.materials.push_back(std::move(mat));
            terrain.parts.push_back(part);
        }
        if (auto count = static_cast<std::uint32_t>(zone.flora.instances.size()) - flora_first; count > 0)
            zone.flora.tiles.push_back({glm::vec2(static_cast<float>(tile.lng), static_cast<float>(tile.lat)) * tile_size,
                                        flora_first, count});
    }
    if (!zone.flora.empty())
        log::info("{}: radial flora: {} plants of {} kinds on {} tiles", zone.name, zone.flora.instances.size(),
                  zone.flora.definitions.size(), zone.flora.tiles.size());
    if (too_many_layers) log::warn("{}: {} tile ecosystems with more than three texture layers", zone.name, too_many_layers);
    if (unpainted) log::info("{}: {} unpainted tiles take their nearest painted neighbour's ecosystem", zone.name, unpainted);
    // A border texel of the detail mask is the average (a + b) / 2 with the
    // neighbouring tile's opposite-edge texel (UploadDetailMask), for
    // neighbours of the same base ecosystem.
    auto original = mask_pixels;
    for (const auto& [key, ref] : base_masks) {
        auto [lng, lat] = key;
        auto& px = mask_pixels[ref.texture];
        auto blend_edge = [&](std::int32_t dlng, std::int32_t dlat) {
            // NOLINTNEXTLINE(clang-analyzer-core.NullDereference): analyzer false positive on the lookup key.
            auto it = base_masks.find({lng + dlng, lat + dlat});
            if (it == base_masks.end() || it->second.eco != ref.eco) return;
            const auto& other = original[it->second.texture];
            for (std::uint32_t k = 0; k < S; ++k) {
                std::uint32_t x = dlng < 0 ? 0 : dlng > 0 ? S - 1 : k, y = dlat < 0 ? 0 : dlat > 0 ? S - 1 : k;
                std::uint32_t ox = dlng < 0 ? S - 1 : dlng > 0 ? 0 : k, oy = dlat < 0 ? S - 1 : dlat > 0 ? 0 : k;
                for (int c = 0; c < 3; ++c) {
                    auto& a = px[(y * S + x) * 4 + c];
                    a = static_cast<std::uint8_t>((original[ref.texture][(y * S + x) * 4 + c] + other[(oy * S + ox) * 4 + c]) / 2);
                }
            }
        };
        blend_edge(-1, 0);
        blend_edge(1, 0);
        blend_edge(0, -1);
        blend_edge(0, 1);
    }
    for (std::size_t i = 0; i < mask_pixels.size(); ++i) zone.generated_files.emplace(mask_names[i], make_dds(S, S, mask_pixels[i]));
    zone.terrain.push_back(std::move(terrain));

    // Objects: individual placements plus .tog groups.
    std::vector<eqg::HeightmapObject> placements = map.objects;
    std::size_t missing_groups = 0;
    for (const auto& g : map.groups) {
        auto data = src.read(g.model + ".tog");
        if (!data) {
            ++missing_groups;
            continue;
        }
        for (auto o : eqg::parse_tog(*data)) {
            o.transform = g.transform * o.transform;
            placements.push_back(std::move(o));
        }
    }
    if (missing_groups) log::warn("{}: {} object groups missing", zone.name, missing_groups);
    eqg::MeshStats stats;
    std::set<std::string> missing_models;
    for (const auto& p : placements) {
        std::string file = ends_with(p.model, ".mod") ? p.model : p.model + ".mod";
        std::string key = upper(file);
        if (!zone.objects.contains(key)) {
            if (missing_models.contains(key)) continue;
            auto data = src.archive->read(file);
            if (!data) {
                missing_models.insert(key);
                continue;
            }
            try {
                zone.objects.emplace(key, eqg::build_mesh(eqg::parse_model(*data, false), key, stats));
            } catch (const std::exception& e) {
                log::debug("{}: model {}: {}", zone.name, file, e.what());
                missing_models.insert(key);
                continue;
            }
        }
        bool ok = true;
        for (int c = 0; c < 4; ++c) ok &= finite(glm::vec3(p.transform[c]));
        if (!ok) continue;
        Placement out;
        out.actor = key;
        out.transform = p.transform;
        zone.placements.push_back(std::move(out));
    }
    if (!missing_models.empty()) log::debug("{}: {} object models missing", zone.name, missing_models.size());

    // Water: water.dat's finite sheets, and the tiles' indexed sheets
    // (version > 20, hasSheet): the rectangle (x0 + minX .. x0 + maxX,
    // y0 + minY .. y0 + maxY) at the tile's water elevation, drawn with the
    // material of the first *WATERSHEETDATA whose *INDEX equals the tile's
    // waterDataIndex (CWaterSheetManager_FindDataByIndex @0x100fa4b0; none ->
    // a default material). Drawn as RegionWater (assets::EqgShader::Water).
    eqg::WaterFile water;
    if (auto data = src.archive->read("water.dat")) water = eqg::parse_water_file(*data);
    {
        Mesh mesh;
        mesh.name = "WATER";
        std::map<std::string, std::uint32_t> materials; // material key -> index
        std::vector<std::vector<std::uint32_t>> indices;
        auto material_for = [&](const eqg::WaterMaterial& wm) {
            auto file = [](std::string name) {
                std::replace(name.begin(), name.end(), '\\', '/');
                if (auto slash = name.rfind('/'); slash != std::string::npos) name = name.substr(slash + 1);
                return lower(name);
            };
            Material mat;
            mat.name = "water";
            mat.blend = BlendMode::Transparent50;
            mat.eqg_shader = EqgShader::Water;
            mat.two_sided = true; // seen from above and from under water
            std::string normal = wm.normal_map.empty() ? "water_n.dds" : file(wm.normal_map);
            mat.frames.push_back(normal);
            mat.effect.textures = {normal, file(wm.environment_map)};
            mat.effect.water_color1 = wm.water_color1;
            mat.effect.water_color2 = wm.water_color2;
            mat.effect.reflection_color = wm.reflection_color;
            mat.effect.fresnel_bias = wm.fresnel_bias;
            mat.effect.fresnel_power = wm.fresnel_power;
            mat.effect.reflection_amount = wm.reflection_amount;
            mat.effect.uv_scale = wm.uv_scale;
            std::string key = normal + "|" + mat.effect.textures[1] + "|" + std::to_string(wm.uv_scale) + "|" +
                              std::to_string(wm.fresnel_bias) + "|" + std::to_string(wm.fresnel_power) + "|" +
                              std::to_string(wm.water_color1.r) + std::to_string(wm.water_color2.r) +
                              std::to_string(wm.water_color1.g) + std::to_string(wm.water_color2.b);
            auto [it, added] = materials.emplace(key, static_cast<std::uint32_t>(mesh.materials.size()));
            if (added) {
                mesh.materials.push_back(std::move(mat));
                indices.emplace_back();
            }
            return it->second;
        };
        auto add_rect = [&](glm::vec2 lo, glm::vec2 hi, float z, const eqg::WaterMaterial& wm) {
            if (!(hi.x > lo.x) || !(hi.y > lo.y) || !std::isfinite(z)) return;
            std::uint32_t mi = material_for(wm);
            auto first = static_cast<std::uint32_t>(mesh.vertices.size());
            for (glm::vec2 c : {lo, glm::vec2(hi.x, lo.y), hi, glm::vec2(lo.x, hi.y)}) {
                Vertex v;
                v.position = {c.x, c.y, z};
                v.uv = c / 256.0f; // world units as the raw UV; the effect scales by UVScale
                mesh.vertices.push_back(v);
            }
            indices[mi].insert(indices[mi].end(), {first, first + 1, first + 2, first, first + 2, first + 3});
        };
        for (const auto& s : water.sheets) add_rect(s.min, s.max, s.height, s.material);
        std::size_t indexed = 0, unmatched = 0;
        static const eqg::WaterMaterial kDefaultWater;
        for (const auto& tile : map.tiles) {
            if (!tile.has_water_sheet) continue;
            glm::vec2 origin(static_cast<float>(tile.lng) * tile_size, static_cast<float>(tile.lat) * tile_size);
            const eqg::WaterSheetData* d = water.find(tile.water_index);
            if (!d) ++unmatched;
            add_rect(origin + tile.water_min, origin + tile.water_max, tile.water_level, d ? d->material : kDefaultWater);
            ++indexed;
        }
        if (indexed) log::debug("{}: {} indexed water tiles ({} without sheet data)", zone.name, indexed, unmatched);
        for (std::size_t i = 0; i < indices.size(); ++i) {
            MeshPart part;
            part.solid = false;
            part.material = static_cast<std::uint32_t>(i);
            part.first_index = static_cast<std::uint32_t>(mesh.indices.size());
            part.index_count = static_cast<std::uint32_t>(indices[i].size());
            mesh.indices.insert(mesh.indices.end(), indices[i].begin(), indices[i].end());
            mesh.parts.push_back(part);
        }
        if (!mesh.indices.empty()) zone.terrain.push_back(std::move(mesh));
    }

    // Region boxes from the DAT areas (CEQTerrainSystem::vf3 @0x100a5d40):
    // the ATP areas of every tile first, then the other areas, in tile and
    // record order (FindRegionContaining takes the first containing box);
    // see eqg::dat_region_box. The DLL then adds the .tog group areas, but
    // never positions them (their update at 0x10103c40 has no caller), so
    // they are not placed here.
    std::vector<RegionBox> boxes;
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& g : map.regions)
            if (g.name.starts_with("ATP") == (pass == 0))
                boxes.push_back(eqg::dat_region_box(g, map.tiles[g.tile], tile_size));
    zone.regions = ZoneRegions::from_boxes(boxes);

    for (const auto& l : map.lights) {
        PointLight light;
        light.position = l.position;
        light.radius = l.radius;
        if (auto def = src.archive->read(l.definition + ".def"))
            if (auto c = eqg::light_definition_color(
                    std::string_view(reinterpret_cast<const char*>(def->data()), def->size())))
                light.color = *c;
        if (finite_light(light)) zone.lights.push_back(light);
    }
}

} // namespace

bool is_revamped_zone_name(std::string_view name_view) {
    std::string name = lower(std::string(name_view));
    for (std::string_view revamped : {"arena", "bazaar", "highpasshold", "lavastorm", "nektulos"})
        if (name == revamped) return true;
    return false;
}

bool is_eqg_zone(const GameDir& game, std::string_view name_view) {
    std::string name = lower(std::string(name_view));
    // Zones the live client revamped under the same name: its <zone>.eqg is the new zone, while a RoF2 server
    // places its spawns on the RoF2 client's <zone>.s3d, which wins when there is one.
    if (is_revamped_zone_name(name) && game.exists(name + ".s3d")) return false;
    return game.exists(name + ".eqg");
}

Zone load_eqg_zone(const GameDir& game, std::string_view name_view, const ZoneLoadCancel& cancel) {
    auto check = [&] {
        if (cancel && cancel()) throw ZoneLoadCancelled();
    };
    check();
    std::string name = lower(std::string(name_view));
    auto path = game.find(name + ".eqg");
    if (!path) throw std::runtime_error("zone archive " + name + ".eqg not found");
    Zone zone;
    zone.name = name;
    Source src{std::make_shared<const PfsArchive>(PfsArchive::open(
                   *path, PfsArchive::storage_for(game, *path), game.shared_file_cache())), &game};
    zone.archives.push_back(src.archive);
    auto zon = find_declaration(src, name);
    check();
    if (eqg::is_heightmap_zon(zon)) {
        log::debug("{}: heightmap zone (EQTZP)", name);
        load_heightmap(zone, src, zon);
    } else {
        log::debug("{}: binary zone (EQGZ v{})", name, zon.size() >= 8 ? std::to_integer<int>(zon[4]) : 0);
        load_binary(zone, src, zon);
        // harbingers.eqg's Water materials take class 0x37, SPL/RegionLavaH.
        if (name == "harbingers")
            for (auto& m : zone.terrain)
                for (auto& mat : m.materials)
                    if (mat.eqg_shader == EqgShader::Water) mat.eqg_shader = EqgShader::LavaH;
    }
    check();
    // Animated textures: a diffuse map with a `<texture>.txt` sidecar runs through the frames it lists
    // (bloodfalls' falls, the chambers' blood, akhevatwo's shadows, harbingers' plasma).
    {
        std::map<std::string, std::optional<eqg::TextureAnimation>> animations;
        std::size_t animated = 0;
        auto animate = [&](Mesh& m) {
            for (auto& mat : m.materials) {
                if (mat.frames.size() != 1 || mat.eqg_shader == EqgShader::TerrainTile) continue;
                const std::string& texture = mat.frames.front();
                auto [it, fresh] = animations.try_emplace(texture);
                if (fresh) {
                    auto dot = texture.rfind('.');
                    if (auto data = src.read((dot == std::string::npos ? texture : texture.substr(0, dot)) + ".txt"))
                        it->second = eqg::parse_texture_animation(*data);
                }
                if (!it->second) continue;
                mat.frames = it->second->frames;
                mat.frame_delay_ms = it->second->delay_ms;
                ++animated;
            }
        };
        for (auto& m : zone.terrain) animate(m);
        for (auto& [key, m] : zone.objects) animate(m);
        if (animated) log::info("{}: {} materials with animated textures", name, animated);
    }
    // Textures the archive lacks: loose files of the install, and the shared
    // water textures in Resources/WaterSwap (water_c.bmp for heightmap water).
    std::set<std::string> frames;
    auto collect = [&](const Mesh& m) {
        for (const auto& mat : m.materials) frames.insert(mat.frames.begin(), mat.frames.end());
    };
    for (const auto& m : zone.terrain) collect(m);
    for (const auto& [key, m] : zone.objects) collect(m);
    for (const auto& f : frames) {
        if (zone.generated_files.contains(f) || src.archive->contains(f)) continue;
        auto data = game.read(f);
        if (!data) data = game.read("Resources/WaterSwap/" + f);
        if (data) zone.generated_files.emplace(f, std::move(*data));
    }
    compute_bounds(zone);
    return zone;
}

Zone load_zone(const GameDir& game, std::string_view name, const ZoneLoadCancel& cancel,
               const RevampedZoneTest& revamped_test) {
    if (cancel && cancel()) throw ZoneLoadCancelled();
    // A revamped zone with only the live client's new .eqg: refused unless the server's own files bring it or the
    // test shows the server runs it (game/revamped_zone.h). A RoF2 server may place its spawns on the old geometry
    // (PEQ's Bazaar version 0) or on the revamp (version 1); the original loads the .eqg by name either way.
    const std::string lname = lower(std::string(name));
    const bool unconfirmed_revamp = is_revamped_zone_name(lname) && is_eqg_zone(game, lname) &&
                                    !game.from_overlay(lname + ".eqg") && is_live_asset_folder(game);
    auto refuse = [&](const std::string& why) {
        return std::runtime_error("zone archive " + lname + ".s3d not found (the live client's " + lname +
                                  ".eqg is a revamped zone" + why + ": the server's assets folder needs the RoF2 " +
                                  lname + ".s3d)");
    };
    if (unconfirmed_revamp && !revamped_test) throw refuse("");
    Zone zone = is_eqg_zone(game, name) ? load_eqg_zone(game, name, cancel) : load_classic_zone(game, name, cancel);
    if (cancel && cancel()) throw ZoneLoadCancelled();
    if (unconfirmed_revamp) {
        if (!revamped_test(zone)) throw refuse(" that does not match the server's safe point");
        log::info("{}: loading the live client's revamped {}.eqg, the zone the server runs", lname, lname);
    }
    if (std::size_t n = add_environment_emitters(game, zone)) log::info("{}: {} environment emitters", zone.name, n);
    return zone;
}

} // namespace hailstone::assets
