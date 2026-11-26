// EQG file parsers. Layouts follow OpenEQ (crates/openeq-assets/src/zone.rs,
// terrain.rs and terrain/regions.rs, MIT OR Apache-2.0), checked against the
// RoF2 install; see docs/formats/eqg.md.

#include "assets/eqg.h"

#include <algorithm>
#include <limits>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

#include "core/byte_io.h"
#include "core/float_chars.h"
#include "core/saturate.h"

namespace hailstone::assets::eqg {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The longest name a string table gives. The RoF2 files' longest is 59 bytes; without a bound every reference into
// one long run without a NUL would copy all of it.
constexpr std::size_t kMaxStringLength = 256;

// The NUL-terminated string at `offset` of a string table ("" when out of range), cut at kMaxStringLength.
std::string string_at(std::span<const std::byte> table, std::int64_t offset) {
    if (offset < 0 || static_cast<std::size_t>(offset) >= table.size()) return {};
    const char* p = reinterpret_cast<const char*>(table.data()) + offset;
    std::size_t max = std::min(table.size() - static_cast<std::size_t>(offset), kMaxStringLength);
    std::size_t len = 0;
    while (len < max && p[len] != '\0') ++len;
    return std::string(p, len);
}

glm::vec3 vec3(ByteReader& r) {
    float x = r.f32();
    float y = r.f32();
    float z = r.f32();
    return {x, y, z};
}

glm::vec2 vec2(ByteReader& r) {
    float x = r.f32();
    float y = r.f32();
    return {x, y};
}

// A count that must fit in the remaining bytes at `unit` bytes each.
std::uint32_t count(ByteReader& r, std::size_t unit, const char* what) {
    auto n = r.le<std::uint32_t>();
    if (unit && n > r.remaining() / unit) throw std::runtime_error(std::string("EQG: bad ") + what + " count");
    return n;
}

} // namespace

const Property* MaterialDef::find(std::string_view property) const {
    for (const auto& p : properties)
        if (iequals(p.name, property)) return &p;
    return nullptr;
}

std::string MaterialDef::texture(std::string_view property) const {
    const Property* p = find(property);
    if (!p || p->type != 2 || p->text.empty() || iequals(p->text, "none")) return {};
    return p->text;
}

const MaterialDef* ModelFile::material_for(std::uint32_t ordinal) const {
    if (ordinal >= materials.size()) return nullptr;
    if (first_material.size() == materials.size()) return &materials[first_material[ordinal]];
    const auto& name = materials[ordinal].name;
    for (const auto& m : materials)
        if (m.name == name) return &m;
    return &materials[ordinal];
}

ModelFile parse_model(std::span<const std::byte> data, bool terrain) {
    ByteReader r(data);
    ModelFile m;
    m.terrain = terrain;
    auto magic = r.le<std::uint32_t>();
    if (magic != (terrain ? kTerMagic : kModMagic))
        throw std::runtime_error(terrain ? "not a TER file (EQGT)" : "not a MOD file (EQGM)");
    m.version = r.le<std::uint32_t>();
    if (m.version < 1 || m.version > 3) throw std::runtime_error("unsupported EQG model version " + std::to_string(m.version));
    auto string_size = count(r, 1, "string table");
    auto material_count = count(r, 16, "material");
    auto vertex_count = r.le<std::uint32_t>();
    auto triangle_count = r.le<std::uint32_t>();
    if (!terrain) m.bone_count = r.le<std::uint32_t>();
    auto strings = r.bytes(string_size);

    m.materials.reserve(material_count);
    for (std::uint32_t i = 0; i < material_count; ++i) {
        MaterialDef mat;
        mat.id = r.le<std::uint32_t>();
        mat.name = string_at(strings, r.le<std::int32_t>());
        mat.shader = string_at(strings, r.le<std::int32_t>());
        auto props = count(r, 12, "property");
        mat.properties.reserve(props);
        for (std::uint32_t k = 0; k < props; ++k) {
            Property p;
            p.name = string_at(strings, r.le<std::int32_t>());
            p.type = r.le<std::uint32_t>();
            switch (p.type) {
            case 0: p.f = r.f32(); break;
            case 1: case 3: p.u = r.le<std::uint32_t>(); break;
            case 2: p.text = string_at(strings, r.le<std::int32_t>()); break;
            default: throw std::runtime_error("unknown EQG material property type " + std::to_string(p.type));
            }
            mat.properties.push_back(std::move(p));
        }
        m.materials.push_back(std::move(mat));
    }

    // v1/v2: position, normal, uv (32 bytes); v3: position, normal, colour, uv, uv2 (44 bytes).
    const bool v3 = m.version == 3;
    const std::size_t stride = v3 ? 44 : 32;
    if (vertex_count > r.remaining() / stride) throw std::runtime_error("EQG: bad vertex count");
    m.positions.resize(vertex_count);
    m.normals.resize(vertex_count);
    m.uvs.resize(vertex_count);
    if (v3) m.colors.resize(vertex_count);
    for (std::uint32_t i = 0; i < vertex_count; ++i) {
        m.positions[i] = vec3(r);
        m.normals[i] = vec3(r);
        if (v3) m.colors[i] = r.le<std::uint32_t>();
        m.uvs[i] = vec2(r);
        if (v3) {
            if (m.uvs2.empty()) m.uvs2.resize(vertex_count);
            m.uvs2[i] = vec2(r); // second UV set
        }
    }
    if (triangle_count > r.remaining() / 20) throw std::runtime_error("EQG: bad triangle count");
    m.triangles.resize(triangle_count);
    for (auto& t : m.triangles) {
        t.v[0] = r.le<std::uint32_t>();
        t.v[1] = r.le<std::uint32_t>();
        t.v[2] = r.le<std::uint32_t>();
        t.material = r.le<std::uint32_t>();
        t.flags = r.le<std::uint32_t>();
    }
    // TER v2 may end with a tagged second UV stream: tag 1 or 2, then one
    // float pair per vertex (OpenEQ docs/EQG_TER_TRAILING_CHANNELS.md). Bones
    // and weights of .mod files are not needed for static zone geometry.
    if (terrain && m.version == 2 && r.remaining() >= 4) {
        auto tag = r.le<std::uint32_t>();
        if ((tag == 1 || tag == 2) && r.remaining() >= static_cast<std::size_t>(vertex_count) * 8) {
            m.uvs2.resize(vertex_count);
            for (auto& uv : m.uvs2) uv = vec2(r);
        }
    }
    std::unordered_map<std::string_view, std::uint32_t> first;
    m.first_material.reserve(m.materials.size());
    for (std::uint32_t i = 0; i < m.materials.size(); ++i)
        m.first_material.push_back(first.emplace(m.materials[i].name, i).first->second);
    return m;
}

ZonFile parse_zon(std::span<const std::byte> data) {
    ByteReader r(data);
    if (r.le<std::uint32_t>() != kZonMagic) throw std::runtime_error("not a binary ZON file (EQGZ)");
    ZonFile z;
    z.version = r.le<std::uint32_t>();
    if (z.version < 1 || z.version > 2) throw std::runtime_error("unsupported ZON version " + std::to_string(z.version));
    auto string_size = count(r, 1, "string table");
    auto model_count = r.le<std::uint32_t>();
    auto placement_count = r.le<std::uint32_t>();
    auto region_count = r.le<std::uint32_t>();
    auto light_count = r.le<std::uint32_t>();
    auto strings = r.bytes(string_size);

    if (model_count > r.remaining() / 4) throw std::runtime_error("ZON: bad model count");
    for (std::uint32_t i = 0; i < model_count; ++i) z.models.push_back(string_at(strings, r.le<std::int32_t>()));

    if (placement_count > r.remaining() / 36) throw std::runtime_error("ZON: bad placement count");
    z.placements.reserve(placement_count);
    for (std::uint32_t i = 0; i < placement_count; ++i) {
        ZonPlacement p;
        p.model = r.le<std::int32_t>();
        p.name = string_at(strings, r.le<std::int32_t>());
        p.position = vec3(r);
        p.rotation = vec3(r);
        p.scale = r.f32();
        if (z.version >= 2) {
            auto n = count(r, 4, "placement lighting");
            p.lighting.resize(n);
            for (auto& w : p.lighting) w = r.le<std::uint32_t>();
        }
        z.placements.push_back(std::move(p));
    }

    if (region_count > r.remaining() / 40) throw std::runtime_error("ZON: bad region count");
    for (std::uint32_t i = 0; i < region_count; ++i) {
        ZonRegion g;
        g.name = string_at(strings, r.le<std::int32_t>());
        g.center = vec3(r);
        g.angles = vec3(r);
        g.extents = vec3(r);
        z.regions.push_back(std::move(g));
    }

    if (light_count > r.remaining() / 32) throw std::runtime_error("ZON: bad light count");
    for (std::uint32_t i = 0; i < light_count; ++i) {
        ZonLight l;
        l.name = string_at(strings, r.le<std::int32_t>());
        l.position = vec3(r);
        l.color = vec3(r);
        l.radius = r.f32();
        z.lights.push_back(std::move(l));
    }
    return z;
}

// ---- Heightmap zones ----

namespace {

std::vector<std::string_view> tokens(std::span<const std::byte> data) {
    std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        std::size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i > start) out.push_back(text.substr(start, i - start));
    }
    return out;
}

template <typename T> std::optional<T> number(std::string_view s) {
    T v{};
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    auto [p, ec] = [&] {
        if constexpr (std::is_floating_point_v<T>) return float_from_chars(s.data(), s.data() + s.size(), v);
        else return std::from_chars(s.data(), s.data() + s.size(), v);
    }();
    if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
    if constexpr (std::is_floating_point_v<T>)
        if (!std::isfinite(v)) return std::nullopt;
    return v;
}

std::string read_cstring(ByteReader& r) { return r.cstring(); }

// Text lines split into whitespace tokens.
std::vector<std::vector<std::string_view>> lines(std::span<const std::byte> data) {
    std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    std::vector<std::vector<std::string_view>> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find_first_of("\r\n", start);
        if (end == std::string_view::npos) end = text.size();
        std::span<const std::byte> line(data.data() + start, end - start);
        auto t = tokens(line);
        if (!t.empty()) out.push_back(std::move(t));
        start = end + 1;
    }
    return out;
}

} // namespace

bool is_heightmap_zon(std::span<const std::byte> data) {
    auto t = tokens(data.subspan(0, std::min<std::size_t>(data.size(), 64)));
    return !t.empty() && t.front() == "EQTZP";
}

HeightmapOptions parse_heightmap_zon(std::span<const std::byte> data) {
    auto t = tokens(data);
    if (t.empty() || t.front() != "EQTZP") throw std::runtime_error("not a heightmap ZON (EQTZP)");
    auto field = [&](std::string_view key) -> std::string_view {
        for (std::size_t i = 0; i + 1 < t.size(); ++i)
            if (t[i] == key) return t[i + 1];
        throw std::runtime_error("heightmap ZON: missing " + std::string(key));
    };
    auto integer = [&](std::string_view key) {
        auto v = number<std::int32_t>(field(key));
        if (!v) throw std::runtime_error("heightmap ZON: bad " + std::string(key));
        return *v;
    };
    HeightmapOptions o;
    o.name = std::string(field("*NAME"));
    o.min_lng = integer("*MINLNG");
    o.max_lng = integer("*MAXLNG");
    o.min_lat = integer("*MINLAT");
    o.max_lat = integer("*MAXLAT");
    auto upv = number<float>(field("*UNITSPERVERT"));
    auto q = integer("*QUADSPERTILE");
    if (!upv || *upv <= 0.0f || q < 1 || q > 512 || o.min_lng > o.max_lng || o.min_lat > o.max_lat)
        throw std::runtime_error("heightmap ZON: bad grid");
    o.units_per_vertex = *upv;
    o.quads_per_tile = static_cast<std::uint32_t>(q);
    return o;
}

float HeightmapTile::height_at(const HeightmapOptions& o, float x, float y) const {
    const std::uint32_t q = o.quads_per_tile;
    // A non-finite position (corrupt object record) would make the casts below undefined.
    if (!std::isfinite(x)) x = 0.0f;
    if (!std::isfinite(y)) y = 0.0f;
    float gx = std::clamp(x / o.units_per_vertex, 0.0f, static_cast<float>(q));
    float gy = std::clamp(y / o.units_per_vertex, 0.0f, static_cast<float>(q));
    std::uint32_t col = std::min(static_cast<std::uint32_t>(gx), q - 1);
    std::uint32_t row = std::min(static_cast<std::uint32_t>(gy), q - 1);
    float fx = gx - static_cast<float>(col), fy = gy - static_cast<float>(row);
    float a = heights[row * (q + 1) + col];
    float b = heights[(row + 1) * (q + 1) + col];
    float c = heights[(row + 1) * (q + 1) + col + 1];
    float d = heights[row * (q + 1) + col + 1];
    if (quad_flags[row * q + col] & 0x80) {
        if (fx + fy <= 1.0f) return a + (d - a) * fx + (b - a) * fy;
        return c + (1.0f - fx) * (b - c) + (1.0f - fy) * (d - c);
    }
    if (fy >= fx) return a + (b - a) * fy + (c - b) * fx;
    return a + (d - a) * fx + (c - d) * fy;
}

glm::mat4 heightmap_transform(const glm::vec3& position, const glm::vec3& degrees, const glm::vec3& scale) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    m = glm::rotate(m, glm::radians(degrees.z), glm::vec3(0, 0, 1));
    m = glm::rotate(m, glm::radians(degrees.y), glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(degrees.x), glm::vec3(1, 0, 0));
    return glm::scale(m, scale);
}

Heightmap parse_heightmap(const HeightmapOptions& options, std::span<const std::byte> dat) {
    ByteReader r(dat);
    Heightmap map;
    map.options = options;
    map.version = r.le<std::uint32_t>();
    r.skip(8); // two header words of unknown meaning
    map.base_texture = read_cstring(r);
    auto tile_count = count(r, 12, "tile");
    const std::uint32_t q = options.quads_per_tile;
    const std::size_t verts = static_cast<std::size_t>(q + 1) * (q + 1);
    const float tile_size = options.tile_size();
    // A tile's record holds at least its vertices (12 bytes each), quad flags and seven words (index, id, water,
    // counts): `count` checked only 12 bytes a tile, against a HeightmapTile of some 160 before its vectors.
    const std::size_t min_record = verts * 12 + std::size_t{q} * q + 28;
    map.tiles.reserve(std::min<std::size_t>(tile_count, r.remaining() / min_record));
    for (std::uint32_t t = 0; t < tile_count; ++t) {
        HeightmapTile tile;
        // Unsigned subtraction: a bogus tile index wraps instead of overflowing a signed int.
        tile.lng = static_cast<std::int32_t>(r.le<std::uint32_t>() - 100000u);
        tile.lat = static_cast<std::int32_t>(r.le<std::uint32_t>() - 100000u);
        r.skip(4); // editor tile id
        if (verts * 12 + q * q > r.remaining()) throw std::runtime_error("heightmap DAT: truncated tile");
        tile.heights.resize(verts);
        for (auto& h : tile.heights) {
            h = r.f32();
            if (!std::isfinite(h)) throw std::runtime_error("heightmap DAT: non-finite height");
        }
        tile.colors.resize(verts);
        for (auto& c : tile.colors) c = r.le<std::uint32_t>();
        tile.colors2.resize(verts);
        for (auto& c : tile.colors2) c = r.le<std::uint32_t>();
        auto flags = r.bytes(static_cast<std::size_t>(q) * q);
        tile.quad_flags.resize(flags.size());
        for (std::size_t i = 0; i < flags.size(); ++i) tile.quad_flags[i] = std::to_integer<std::uint8_t>(flags[i]);
        // CTerrainTile::ReadDatRecord @0x101004a0: f32 waterElevation (+0x28);
        // version > 20: i32 waterDataIndex, u8 hasSheet and, when set, f32
        // minX, maxX, minY, maxY; then an f32 (+0x24).
        tile.water_level = r.f32();
        if (map.version > 20) {
            tile.water_index = r.le<std::int32_t>();
            tile.has_water_sheet = r.le<std::uint8_t>() != 0;
            if (tile.has_water_sheet) {
                tile.water_min.x = r.f32();
                tile.water_max.x = r.f32();
                tile.water_min.y = r.f32();
                tile.water_max.y = r.f32();
            }
        }
        r.skip(4);
        auto layer_count = count(r, 1, "layer");
        for (std::uint32_t l = 0; l < layer_count; ++l) {
            HeightmapLayer layer;
            layer.ecosystem = read_cstring(r);
            if (l > 0) {
                layer.mask_size = r.le<std::uint32_t>();
                if (layer.mask_size > 4096) throw std::runtime_error("heightmap DAT: mask too large");
                auto bytes = r.bytes(static_cast<std::size_t>(layer.mask_size) * layer.mask_size);
                layer.mask.resize(bytes.size());
                for (std::size_t i = 0; i < bytes.size(); ++i) layer.mask[i] = std::to_integer<std::uint8_t>(bytes[i]);
            }
            tile.layers.push_back(std::move(layer));
        }
        const glm::vec3 origin(static_cast<float>(tile.lng) * tile_size, static_cast<float>(tile.lat) * tile_size, 0.0f);

        auto singles = count(r, 1, "object");
        for (std::uint32_t i = 0; i < singles; ++i) {
            HeightmapObject o;
            o.model = lower(read_cstring(r));
            read_cstring(r); // ecosystem
            r.skip(8);       // longitude, latitude again
            glm::vec3 pos = vec3(r);
            glm::vec3 rot = vec3(r);
            glm::vec3 scale = vec3(r);
            r.skip(1);
            if (map.version & 2) r.skip(4);
            // Z is relative to the terrain under the object.
            pos.z += tile.height_at(options, std::fmod(std::fmod(pos.x, tile_size) + tile_size, tile_size),
                                    std::fmod(std::fmod(pos.y, tile_size) + tile_size, tile_size));
            o.transform = heightmap_transform(pos + origin, rot, scale);
            map.objects.push_back(std::move(o));
        }
        auto regions = count(r, 1, "region");
        for (std::uint32_t i = 0; i < regions; ++i) {
            HeightmapRegion g;
            g.name = read_cstring(r);
            g.type = r.le<std::uint32_t>();
            g.alt_name = read_cstring(r);
            r.skip(8); // grid
            g.tile = map.tiles.size();
            g.index = i;
            g.position = vec3(r);
            g.rotation = vec3(r);
            g.scale = vec3(r);
            g.size = vec3(r);
            map.regions.push_back(std::move(g));
        }
        auto lights = count(r, 1, "light");
        for (std::uint32_t i = 0; i < lights; ++i) {
            HeightmapLight l;
            l.name = read_cstring(r);
            l.definition = read_cstring(r);
            r.skip(1 + 8);
            glm::vec3 pos = vec3(r);
            r.skip(24); // rotation and scale
            l.radius = r.f32();
            pos.z += tile.height_at(options, pos.x, pos.y);
            l.position = pos + origin;
            map.lights.push_back(std::move(l));
        }
        auto groups = count(r, 1, "group");
        for (std::uint32_t i = 0; i < groups; ++i) {
            HeightmapObject o;
            o.model = lower(read_cstring(r));
            r.skip(8);
            glm::vec3 pos = vec3(r) + origin;
            glm::vec3 rot = vec3(r);
            glm::vec3 scale = vec3(r);
            float z_adjust = r.f32();
            pos.z += scale.z * z_adjust;
            o.transform = heightmap_transform(pos, rot, scale);
            map.groups.push_back(std::move(o));
        }
        map.tiles.push_back(std::move(tile));
    }
    if (!r.empty()) throw std::runtime_error("heightmap DAT: " + std::to_string(r.remaining()) + " unparsed bytes");
    return map;
}

std::vector<EcoLayer> parse_eco(std::span<const std::byte> data) {
    std::vector<EcoLayer> out;
    bool active = false;
    std::optional<EcoLayer> layer;
    auto file = [](std::string_view w) {
        std::string name(w);
        std::replace(name.begin(), name.end(), '\\', '/');
        if (auto slash = name.rfind('/'); slash != std::string::npos) name = name.substr(slash + 1);
        return lower(name);
    };
    for (const auto& w : lines(data)) {
        const auto key = w[0];
        if (key == "*TEXTUREPART") { active = true; continue; }
        if (key == "*END_TEXTUREPART") break;
        if (!active) continue;
        if (key == "*LAYER") {
            layer = EcoLayer{};
            if (w.size() > 1) layer->name = std::string(w[1]);
            continue;
        }
        if (key == "*END_LAYER") {
            if (layer) out.push_back(*layer); // appended in declaration order
            layer.reset();
            continue;
        }
        if (!layer || w.size() < 2) continue;
        if (key == "*COVERMAP" || key == "*COLORMAP") { layer->cover_map = file(w[1]); continue; }
        if (key == "*BLENDMAP") { layer->blend_map = file(w[1]); continue; }
        if (key == "*LAYERINGMAP" || key == "*COVERAGEMAP") { layer->layering_map = file(w[1]); continue; }
        if (key == "*DETAILMAP") { layer->detail_map = file(w[1]); continue; }
        if (key == "*NORMALMAP") { layer->normal_map = file(w[1]); continue; }
        auto v = number<float>(w[1]);
        if (!v) continue;
        // Saturated: a value past int32's range (or NaN) is undefined to cast.
        auto as_int = [&] { return saturate_cast<std::int32_t>(*v); };
        if (key == "*DETAILREPEAT") layer->detail_repeat = static_cast<std::uint32_t>(std::max(as_int(), 0));
        else if (key == "*NORMALREPEAT") layer->normal_repeat = static_cast<std::uint32_t>(std::max(as_int(), 0));
        else if (key == "*BLENDSOFTNESS") layer->blend_softness = as_int();
        else if (key == "*LAYERINGAREA") layer->layering_area = as_int();
        else if (key == "*MINHEIGHT") layer->min_height = *v;
        else if (key == "*MAXHEIGHT") layer->max_height = *v;
        else if (key == "*HEIGHTTOL") layer->height_tolerance = *v;
        else if (key == "*MINSLOPE") layer->min_slope = as_int();
        else if (key == "*MAXSLOPE") layer->max_slope = as_int();
        else if (key == "*SLOPETOL") layer->slope_tolerance = as_int();
    }
    return out;
}

std::vector<EcoFloraLayer> parse_eco_flora(std::span<const std::byte> data) {
    std::vector<EcoFloraLayer> out;
    bool active = false;
    std::optional<EcoFloraLayer> layer;
    for (const auto& w : lines(data)) {
        const auto key = w[0];
        if (key == "*FLORAPART") { active = true; continue; }
        if (key == "*END_FLORAPART") break;
        if (!active) continue;
        if (key == "*LAYER") {
            layer = EcoFloraLayer{};
            if (w.size() > 1) layer->name = lower(std::string(w[1]));
            continue;
        }
        if (key == "*END_LAYER") {
            if (layer) out.push_back(*layer);
            layer.reset();
            continue;
        }
        if (!layer || w.size() < 2) continue;
        if (key == "*FLORA") { layer->flora = lower(std::string(w[1])); continue; }
        if (key == "*TEXTURELAYER") { layer->texture_layer = lower(std::string(w[1])); continue; }
        auto v = number<float>(w[1]);
        if (!v) continue;
        // Saturated: a value past int32's range (or NaN) is undefined to cast.
        auto as_int = [&] { return saturate_cast<std::int32_t>(*v); };
        if (key == "*MINHEIGHT") layer->min_height = *v;
        else if (key == "*MAXHEIGHT") layer->max_height = *v;
        else if (key == "*HEIGHTTOL") layer->height_tolerance = *v;
        else if (key == "*MINSLOPE") layer->min_slope = as_int();
        else if (key == "*MAXSLOPE") layer->max_slope = as_int();
        else if (key == "*SLOPETOL") layer->slope_tolerance = as_int();
        else if (key == "*DENSITY") layer->density = *v >= 0.0f ? std::min(*v, kMaxFloraDensity) : 0.0f; // NaN is 0
        else if (key == "*ITERATIONS") layer->iterations = std::clamp(as_int(), 0, kMaxFloraIterations);
        else if (key == "*MINSCALE") layer->min_scale = *v;
        else if (key == "*MAXSCALE") layer->max_scale = *v;
        else if (key == "*MINALPHA") layer->min_alpha = static_cast<std::uint8_t>(std::clamp(as_int(), 0, 255));
    }
    return out;
}

std::vector<FloraDefinition> parse_radial_flora(std::span<const std::byte> data) {
    std::vector<FloraDefinition> out;
    bool active = false;
    std::optional<FloraDefinition> def;
    auto on = [](std::string_view v) { return lower(std::string(v)) == "enabled"; };
    for (const auto& w : lines(data)) {
        const auto key = w[0];
        if (key == "*BEGIN_RADIALFLORA") { active = true; continue; }
        if (key == "*END_RADIALFLORA") break;
        if (!active) continue;
        if (key == "*FLORADEFINITION") {
            def = FloraDefinition{};
            if (w.size() > 1) def->name = lower(std::string(w[1]));
            continue;
        }
        if (key == "*END_DEFINITION") {
            if (def) out.push_back(*def);
            def.reset();
            continue;
        }
        if (!def || w.size() < 2) continue;
        if (key == "*TEXTURENAME") { def->texture = lower(std::string(w[1])); continue; }
        if (key == "*SWAY") { def->sway = on(w[1]); continue; }
        if (key == "*CROSSFLORA") { def->cross = on(w[1]); continue; }
        if (key == "*QUADTEXTURE") { def->quad_texture = on(w[1]); continue; }
        if (key == "*INHERITVERTEXCOLOR") { def->inherit_vertex_color = on(w[1]); continue; }
        auto v = number<float>(w[1]);
        if (!v) continue;
        if (key == "*MINWIDTH") def->min_width = *v;
        else if (key == "*MAXWIDTH") def->max_width = *v;
        else if (key == "*MINHEIGHT") def->min_height = *v;
        else if (key == "*MAXHEIGHT") def->max_height = *v;
        else if (key == "*SWAYDISPLACEMENT") def->sway_displacement = *v;
        else if (key == "*SWAYPERIOD") def->sway_period = *v;
        else if (key == "*TEXTUREINDEX") def->texture_index = saturate_cast<std::int32_t>(*v);
    }
    return out;
}

std::optional<float> eco_layer_weight(const EcoLayer& l, float h, std::int32_t s, std::int32_t layering,
                                      std::int32_t blend) {
    // 1. Height band.
    const float tol = l.height_tolerance;
    if (h < l.min_height - tol || h > l.max_height + tol) return std::nullopt;
    float w = 1.0f;
    if (h > l.max_height) w = 1.0f - (h - l.max_height) / tol;
    else if (h < l.min_height) w = 1.0f - (l.min_height - h) / tol;
    // 2. Slope band (integers; in 64 bits, so values near int32's limits from a damaged file do not overflow).
    const std::int64_t st = l.slope_tolerance, lo = l.min_slope, hi = l.max_slope, s64 = s;
    if (s64 < lo - st || s64 > hi + st) return std::nullopt;
    if (s64 < lo) w *= 1.0f - static_cast<float>(lo - s64) / static_cast<float>(st);
    else if (s64 > hi) w *= 1.0f - static_cast<float>(s64 - hi) / static_cast<float>(st);
    // 3. LAYERINGMAP.
    if (layering >= 0) {
        if (layering <= 0) return std::nullopt;
        if (layering < 255) w *= static_cast<float>(layering) / 255.0f;
    }
    // 4. BLENDMAP, only inside the soft edge.
    if (blend >= 0 && w > 0.02f && w < 0.98f) {
        auto t = static_cast<std::int32_t>((1.0f - w) * 255.0f);
        auto base = static_cast<std::int32_t>(0.01f * static_cast<float>(t * t) - 255.0f);
        auto gain = saturate_cast<std::int32_t>(
            static_cast<float>(t) * static_cast<float>(100 - std::int64_t{l.blend_softness}) * 0.001f + 1.0f);
        const std::int64_t q = (std::int64_t{blend} - base) * gain;
        if (q < 0) return std::nullopt;
        w = q > 255 ? 1.0f : static_cast<float>(q) / 255.0f;
    }
    return w;
}

std::uint8_t eco_accumulate(float w, std::int32_t& acc, std::int32_t parent) {
    if (parent == 0) return 0;
    if (parent > 0 && parent < 255) w *= static_cast<float>(parent) / 255.0f;
    auto mask = static_cast<std::int32_t>(w * static_cast<float>(255 - acc));
    mask = std::clamp(mask, 0, 255);
    if (parent > 0) acc += static_cast<std::int32_t>(255.0f / static_cast<float>(parent) * static_cast<float>(mask));
    else acc += mask;
    acc = std::min(acc, 255);
    return static_cast<std::uint8_t>(mask);
}

std::uint8_t eco_color(std::uint8_t value, std::uint8_t cover, std::uint8_t mask, bool base) {
    if (mask < 3) return base ? 0 : value;
    if (mask >= 0xfd) return cover;
    auto add = static_cast<std::uint8_t>(std::lround(static_cast<float>(cover) * (static_cast<float>(mask) * 0.003921569f)));
    return base ? add : static_cast<std::uint8_t>(value + add);
}

std::optional<std::size_t> nearest_painted_tile(std::int32_t lng, std::int32_t lat,
                                                const std::map<std::pair<std::int32_t, std::int32_t>, std::size_t>& painted,
                                                std::int32_t reach, std::uint64_t* budget) {
    for (std::int32_t ring = 1; ring <= reach; ++ring) {
        std::optional<std::size_t> best;
        std::int64_t best_d2 = 0;
        bool spent = false;
        auto visit = [&](std::int32_t dx, std::int32_t dy) {
            if (spent) return;
            // In 64 bits: a corrupt tile's index lies near INT32_MIN or INT32_MAX, and a neighbour past it is none.
            const std::int64_t x = std::int64_t{lng} + dx, y = std::int64_t{lat} + dy;
            if (x < std::numeric_limits<std::int32_t>::min() || x > std::numeric_limits<std::int32_t>::max() ||
                y < std::numeric_limits<std::int32_t>::min() || y > std::numeric_limits<std::int32_t>::max())
                return;
            if (budget) {
                if (*budget == 0) {
                    spent = true;
                    return;
                }
                --*budget;
            }
            auto it = painted.find({static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)});
            if (it == painted.end()) return;
            const std::int64_t d2 = std::int64_t{dx} * dx + std::int64_t{dy} * dy;
            if (!best || d2 < best_d2) {
                best = it->second;
                best_d2 = d2;
            }
        };
        // The ring's cells only, row by row and left to right in each row (the order ties are settled in): the
        // whole top and bottom rows, the two ends of the rows between.
        for (std::int32_t dy = -ring; dy <= ring; ++dy) {
            if (dy == -ring || dy == ring) {
                for (std::int32_t dx = -ring; dx <= ring; ++dx) visit(dx, dy);
            } else {
                visit(-ring, dy);
                visit(ring, dy);
            }
        }
        if (spent) return std::nullopt;
        if (best) return best;
    }
    return std::nullopt;
}

std::optional<TextureAnimation> parse_texture_animation(std::span<const std::byte> data) {
    std::vector<std::string> lines;
    std::string line;
    auto flush = [&] {
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        std::size_t start = 0;
        while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) ++start;
        if (start < line.size()) lines.push_back(line.substr(start));
        line.clear();
    };
    for (std::byte b : data) {
        char c = static_cast<char>(b);
        if (c == '\n' || c == '\r') flush();
        else line += c;
    }
    flush();
    if (lines.size() < 4) return std::nullopt;
    auto number = [](const std::string& s) -> std::optional<std::uint32_t> {
        std::uint32_t v = 0;
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
        return v;
    };
    auto count = number(lines[0]);
    auto delay = number(lines[1]);
    if (!count || !delay || *delay == 0) return std::nullopt;
    TextureAnimation out;
    out.delay_ms = *delay;
    for (std::size_t i = 2; i < lines.size() && out.frames.size() < *count; ++i) {
        std::string name = lines[i];
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out.frames.push_back(std::move(name));
    }
    if (out.frames.size() < 2) return std::nullopt;
    return out;
}

std::vector<HeightmapObject> parse_tog(std::span<const std::byte> data) {
    std::vector<HeightmapObject> out;
    std::string model;
    glm::vec3 pos(0.0f), rot(0.0f), scale(1.0f);
    auto three = [](const std::vector<std::string_view>& w, glm::vec3& v) {
        if (w.size() < 4) return;
        auto x = number<float>(w[1]), y = number<float>(w[2]), z = number<float>(w[3]);
        if (x && y && z) v = {*x, *y, *z};
    };
    for (const auto& w : lines(data)) {
        const auto key = w[0];
        if (key == "*BEGIN_OBJECT") {
            model.clear();
            pos = rot = glm::vec3(0.0f);
            scale = glm::vec3(1.0f);
        } else if (key == "*NAME" && w.size() >= 2) {
            model = lower(std::string(w[1]));
        } else if (key == "*POSITION") {
            three(w, pos);
        } else if (key == "*ROTATION") {
            three(w, rot);
        } else if (key == "*SCALE" && w.size() >= 2) {
            if (auto s = number<float>(w[1])) scale = glm::vec3(*s);
        } else if (key == "*END_OBJECT" && !model.empty()) {
            // CEQTerrainObject::UpdateActorTransform @0x100a3060 passes the
            // degrees x 1.4222 to the actor's Rz(a) Ry(-b) Rx(c) without the
            // pre-negation .zon placements get, so the Y angle turns the other
            // way (medium confidence, see docs/unknowns.md).
            out.push_back({model, heightmap_transform(pos, {rot.x, -rot.y, rot.z}, scale)});
        }
    }
    return out;
}

namespace {

std::optional<std::int32_t> parse_index(std::string_view s) {
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        std::uint32_t v = 0;
        auto [p, ec] = std::from_chars(s.data() + 2, s.data() + s.size(), v, 16);
        if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
        return static_cast<std::int32_t>(v);
    }
    return number<std::int32_t>(s);
}

// One material key of a sheet or data block; false when `key` is not one.
bool water_key(WaterMaterial& m, std::string_view key, const std::vector<std::string_view>& w) {
    auto f = [&](std::size_t i) { return i < w.size() ? number<float>(w[i]).value_or(0.0f) : 0.0f; };
    auto color = [&] { return glm::vec4(f(1), f(2), f(3), w.size() > 4 ? f(4) : 1.0f); };
    if (key == "*NORMALMAP") m.normal_map = w.size() > 1 ? std::string(w[1]) : std::string();
    else if (key == "*ENVIRONMENTMAP") m.environment_map = w.size() > 1 ? std::string(w[1]) : std::string();
    else if (key == "*REFLECTIONCOLOR") m.reflection_color = color();
    else if (key == "*WATERCOLOR1") m.water_color1 = color();
    else if (key == "*WATERCOLOR2") m.water_color2 = color();
    else if (key == "*UVSCALE") m.uv_scale = f(1);
    else if (key == "*REFLECTIONAMOUNT") m.reflection_amount = f(1);
    else if (key == "*FRESNELPOWER") m.fresnel_power = f(1);
    else if (key == "*FRESNELBIAS") m.fresnel_bias = f(1);
    else return false;
    return true;
}

} // namespace

WaterFile parse_water_file(std::span<const std::byte> data) {
    WaterFile out;
    enum class In { None, Sheet, Data } in = In::None;
    WaterSheet sheet;
    WaterSheetData block;
    std::int32_t counter = 0;
    for (const auto& w : lines(data)) {
        const auto key = w[0];
        if (key == "*WATERSHEET") {
            in = In::Sheet;
            sheet = WaterSheet{};
            if (w.size() > 1) sheet.name = std::string(w[1]);
        } else if (key == "*END_SHEET") {
            if (in == In::Sheet) {
                sheet.uv_scale = sheet.material.uv_scale;
                out.sheets.push_back(sheet);
            }
            in = In::None;
        } else if (key == "*WATERSHEETDATA") {
            in = In::Data;
            block = WaterSheetData{};
            block.index = counter++;
        } else if (key == "*ENDWATERSHEETDATA") {
            if (in == In::Data) out.data.push_back(block);
            in = In::None;
        } else if (in == In::Sheet && w.size() >= 2) {
            if (water_key(sheet.material, key, w)) continue;
            auto v = number<float>(w[1]);
            if (!v) continue;
            if (key == "*MINX") sheet.min.x = *v;
            else if (key == "*MINY") sheet.min.y = *v;
            else if (key == "*MAXX") sheet.max.x = *v;
            else if (key == "*MAXY") sheet.max.y = *v;
            else if (key == "*ZHEIGHT") sheet.height = *v;
        } else if (in == In::Data && w.size() >= 2) {
            if (key == "*INDEX") {
                if (auto i = parse_index(w[1])) block.index = *i;
            } else {
                water_key(block.material, key, w);
            }
        }
    }
    return out;
}

const WaterSheetData* WaterFile::find(std::int32_t index) const {
    for (const auto& d : data)
        if (d.index == index) return &d;
    return nullptr;
}

std::vector<WaterSheet> parse_water_sheets(std::span<const std::byte> data) { return parse_water_file(data).sheets; }

assets::RegionBox dat_region_box(const HeightmapRegion& g, const HeightmapTile& tile, float tile_size) {
    constexpr float k512 = 1.4222222f; // 512 / 360, as the DLL multiplies
    assets::RegionBox box;
    box.name = g.name;
    box.center = {static_cast<float>(tile.lng) * tile_size + g.position.x,
                  static_cast<float>(tile.lat) * tile_size + g.position.y, g.position.z};
    box.angles = {g.rotation.z * k512, g.rotation.y * k512, g.rotation.x * k512};
    box.extents = g.size * 0.5f;
    return box;
}

std::optional<glm::vec3> light_definition_color(std::string_view view) {
    const std::string text(view); // strtod needs the terminator
    auto value = [&](std::string_view key) -> std::optional<double> {
        auto at = text.find(key);
        if (at == std::string::npos) return std::nullopt;
        return std::strtod(text.c_str() + at + key.size(), nullptr);
    };
    auto c = value("*COLOR");
    if (!c) return std::nullopt;
    // "1e400" or "nan" in a broken file: casting those to an integer is undefined, so they saturate.
    auto argb = static_cast<std::uint32_t>(saturate_cast<std::int64_t>(*c));
    auto k = static_cast<float>(value("*INTENSITY").value_or(1.0));
    if (!std::isfinite(k)) k = 1.0f;
    return glm::vec3(static_cast<float>((argb >> 16) & 255), static_cast<float>((argb >> 8) & 255),
                     static_cast<float>(argb & 255)) /
           255.0f * k;
}

} // namespace hailstone::assets::eqg
