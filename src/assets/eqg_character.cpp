// EQG character models. Layouts follow OpenEQ
// crates/openeq-assets/src/character/modern.rs (MIT OR Apache-2.0); see
// THIRD_PARTY.md. Notes in docs/formats/characters.md ("EQG models").

#include "assets/eqg_character.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "assets/eqg.h"
#include "core/byte_io.h"
#include "core/log.h"
#include "game/mechanics/anim_select.h"
#include "game/race_models.h"

namespace hailstone::assets {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::uint32_t count(ByteReader& r, std::size_t limit) {
    auto v = r.le<std::uint32_t>();
    if (v > limit) throw std::runtime_error("implausible count " + std::to_string(v));
    return v;
}

// The longest name a string table gives. The RoF2 files' longest is 59 bytes; without a bound every reference into
// one long run without a NUL would copy all of it.
constexpr std::size_t kMaxNameLength = 256;

std::string name_at(std::span<const std::byte> strings, std::uint32_t offset) {
    if (offset >= strings.size()) throw std::runtime_error("string offset out of range");
    const char* p = reinterpret_cast<const char*>(strings.data()) + offset;
    std::size_t n = 0;
    while (offset + n < strings.size() && p[n] != '\0') {
        if (n == kMaxNameLength) throw std::runtime_error("string too long");
        ++n;
    }
    if (offset + n == strings.size()) throw std::runtime_error("unterminated string");
    return std::string(p, n);
}

glm::vec3 vec3(ByteReader& r) {
    float x = r.f32();
    float y = r.f32();
    float z = r.f32();
    return {x, y, z};
}

// Translation, rotation (x, y, z, w; stored as the inverse of the usual
// convention, so conjugated), scale.
BonePose transform(ByteReader& r) {
    BonePose p;
    p.translation = vec3(r);
    float x = r.f32();
    float y = r.f32();
    float z = r.f32();
    float w = r.f32();
    glm::quat q(w, x, y, z);
    float len = glm::length(q);
    q = std::isfinite(len) && len > 1e-6f ? q / len : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    p.rotation = glm::conjugate(q);
    p.scale = vec3(r);
    if (!std::isfinite(p.translation.x + p.translation.y + p.translation.z + p.scale.x + p.scale.y + p.scale.z))
        throw std::runtime_error("non-finite bone transform");
    return p;
}

BonePose blend(const BonePose& a, const BonePose& b, float t) {
    BonePose p;
    p.rotation = glm::normalize(glm::slerp(a.rotation, b.rotation, t));
    p.translation = glm::mix(a.translation, b.translation, t);
    p.scale = glm::mix(a.scale, b.scale, t);
    return p;
}

// The archive a model texture belongs to: "c_<code>_..." is <code>.eqg's; "" for other names.
std::string texture_owner(std::string_view member) {
    if (!member.starts_with("c_")) return {};
    std::size_t us = member.find('_', 2);
    if (us == std::string_view::npos || us == 2) return {};
    return std::string(member.substr(2, us - 2)) + ".eqg";
}

std::vector<std::string> text_lines(const std::vector<std::byte>& data) {
    std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

bool safe_member(std::string_view s) {
    return !s.empty() && s.find_first_of("/\\:") == std::string_view::npos;
}

std::string layer_token(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (text.empty() || text.size() > kMaxNameLength ||
        !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; })) return {};
    std::string result = upper(text);
    // IT-1 is an invalid catalog entry, not an item model. Other names are
    // retained for bounded generic layer chains and existing asset extensions.
    if (result.starts_with("IT") && (result.size() == 2 ||
        !std::all_of(result.begin() + 2, result.end(), [](unsigned char c) { return std::isdigit(c); }))) return {};
    return result;
}

} // namespace

namespace eqg_character {

MeshFile parse_mesh(std::span<const std::byte> data, std::string_view label) {
    ByteReader r(data);
    MeshFile f;
    auto magic = r.fixed_string(4);
    if (magic != "EQGS" && magic != "EQGM") throw std::runtime_error(std::string(label) + ": not EQGS/EQGM");
    f.mds = magic == "EQGS";
    f.version = r.le<std::uint32_t>();
    if (f.version < 1 || f.version > 3) throw std::runtime_error("unsupported version " + std::to_string(f.version));
    const std::size_t limit = data.size();
    std::uint32_t ns = count(r, limit);
    std::uint32_t nm = count(r, limit);
    std::uint32_t nb = 0, np = 1, nv = 0, nt = 0;
    if (f.mds) {
        nb = count(r, limit);
        np = count(r, limit);
    } else {
        nv = count(r, limit);
        nt = count(r, limit);
        nb = count(r, limit);
    }
    // A .mod without bones is a static piece (hero's forge helms, bracers):
    // its vertices are held by the point it is attached to.
    if ((nb == 0 && f.mds) || nb > 1024 || np == 0 || np > 512) throw std::runtime_error("bad bone or piece count");
    auto strings = r.bytes(ns);
    for (std::uint32_t i = 0; i < nm; ++i) {
        MaterialInfo m;
        eqg::MaterialDef def;
        def.id = r.le<std::uint32_t>();
        m.name = name_at(strings, r.le<std::uint32_t>());
        m.shader = name_at(strings, r.le<std::uint32_t>());
        def.name = m.name;
        def.shader = m.shader;
        std::uint32_t props = count(r, limit);
        for (std::uint32_t k = 0; k < props; ++k) {
            eqg::Property property;
            property.name = name_at(strings, r.le<std::uint32_t>());
            property.type = r.le<std::uint32_t>();
            property.u = r.le<std::uint32_t>();
            if (property.type == 0) property.f = std::bit_cast<float>(property.u);
            else if (property.type == 2) {
                property.text = name_at(strings, property.u);
                // Keep the character loader's exact-name, last-wins diffuse
                // selection; the retained definition also supplies armor maps.
                if (property.name == "e_TextureDiffuse0") m.diffuse = property.text;
            }
            def.properties.push_back(std::move(property));
        }
        m.effect = eqg::effect_params(def, eqg::shader_class(m.shader));
        const std::string environment = lower(def.texture("e_TextureEnvironment0"));
        if (!environment.empty()) m.effect.textures = {environment};
        auto has = [&](std::string_view property, std::uint32_t type) {
            const auto* p = def.find(property);
            return p && p->type == type;
        };
        m.surface_properties.diffuse = has("e_TextureDiffuse0", 2);
        m.surface_properties.normal = has("e_TextureNormal0", 2);
        m.surface_properties.environment = has("e_TextureEnvironment0", 2);
        m.surface_properties.shininess = has("e_fShininess0", 0);
        m.surface_properties.env_strength = has("e_fEnvMapStrength0", 0);
        f.materials.push_back(std::move(m));
    }
    auto read_bones = [&] {
        for (std::uint32_t i = 0; i < nb; ++i) {
            Bone b;
            b.name = name_at(strings, r.le<std::uint32_t>());
            b.next = r.le<std::int32_t>();
            b.children = count(r, nb);
            b.child = r.le<std::int32_t>();
            b.bind = transform(r);
            f.bones.push_back(std::move(b));
        }
    };
    auto read_vertices = [&](Piece& p, std::uint32_t n) {
        // Position, normal and uv (32 bytes), plus a colour and a second uv from version 3: a count the rest of the
        // file cannot hold is refused before the vertices are allocated.
        const std::size_t vertex_size = f.version >= 3 ? 44 : 32;
        if (n > r.remaining() / vertex_size) throw std::runtime_error("vertex count past the end of the file");
        p.vertices.resize(n);
        for (auto& v : p.vertices) {
            v.position = vec3(r);
            v.normal = vec3(r);
            if (f.version >= 3) r.skip(4); // colour
            float u = r.f32();
            float w = r.f32();
            // A mesh with bones has its UVs run up from the bottom of the texture: a DDS's top row is the stored
            // v = 1. Read as they are, the textures land upside down (the puma's teeth on its back, the worg's
            // mouth on its spine, a hero's forge breastplate's chainmail across it). A mesh without bones (a held
            // item, a hero's forge helm or bracer) stores v from the top, as the zones' .mod objects do: turned,
            // the 10th anniversary shield's lettering reads upside down and the Haunting Nightmares ornament shows
            // the inside of its pumpkin.
            v.uv = {u, nb > 0 ? 1.0f - w : w};
            if (f.version >= 3) r.skip(8); // second uv
        }
    };
    auto read_triangles = [&](Piece& p, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) {
            std::array<std::uint32_t, 3> t{};
            for (auto& x : t) x = r.le<std::uint32_t>();
            auto mat = r.le<std::int32_t>();
            r.le<std::uint32_t>(); // flags
            for (auto x : t)
                if (x >= p.vertices.size()) throw std::runtime_error("triangle index out of range");
            if (mat < -1 || mat >= static_cast<std::int32_t>(nm)) throw std::runtime_error("bad triangle material");
            p.triangles.push_back(t);
            p.triangle_materials.push_back(mat);
        }
    };
    auto read_weights = [&](Piece& p) {
        for (auto& v : p.vertices) {
            std::uint32_t n = r.le<std::uint32_t>();
            if (n > 4) throw std::runtime_error("more than four bone weights");
            v.weights = glm::vec4(0.0f);
            for (std::uint32_t k = 0; k < 4; ++k) {
                auto bone = r.le<std::int32_t>();
                float w = r.f32();
                if (k >= n) continue;
                if (bone < 0 || static_cast<std::uint32_t>(bone) >= nb || !std::isfinite(w) || w < 0.0f)
                    throw std::runtime_error("bad bone weight");
                v.bones[k] = static_cast<std::uint16_t>(bone);
                v.weights[static_cast<glm::length_t>(k)] = w;
            }
            float sum = v.weights.x + v.weights.y + v.weights.z + v.weights.w;
            if (sum > 1e-6f) {
                v.weights /= sum;
            } else {
                v.bones = {};
                v.weights = {1.0f, 0.0f, 0.0f, 0.0f};
            }
        }
    };
    if (f.mds) {
        read_bones();
        for (std::uint32_t i = 0; i < np; ++i) {
            Piece p;
            r.le<std::uint32_t>(); // main piece flag
            p.name = name_at(strings, r.le<std::uint32_t>());
            std::uint32_t pv = count(r, limit);
            std::uint32_t pt = count(r, limit);
            count(r, limit); // bone assignment count
            read_vertices(p, pv);
            read_triangles(p, pt);
            read_weights(p);
            f.pieces.push_back(std::move(p));
        }
    } else {
        Piece p;
        p.name = upper(label);
        read_vertices(p, nv);
        read_triangles(p, nt);
        read_bones();
        if (nb > 0) read_weights(p);
        f.pieces.push_back(std::move(p));
    }
    return f;
}

AnimationFile parse_animation(std::span<const std::byte> data) {
    ByteReader r(data);
    if (r.fixed_string(4) != "EQGA") throw std::runtime_error("not EQGA");
    std::uint32_t version = r.le<std::uint32_t>();
    std::uint32_t ns = count(r, data.size());
    std::uint32_t n = count(r, data.size());
    if (version > 1) r.le<std::uint32_t>();
    auto strings = r.bytes(ns);
    AnimationFile a;
    for (std::uint32_t i = 0; i < n; ++i) {
        Track t;
        std::uint32_t frames = count(r, data.size());
        t.bone = name_at(strings, r.le<std::uint32_t>());
        for (std::uint32_t k = 0; k < frames; ++k) {
            std::uint32_t ms = r.le<std::uint32_t>();
            if (!t.times_ms.empty() && ms < t.times_ms.back()) throw std::runtime_error("unordered frame times");
            t.times_ms.push_back(ms);
            t.poses.push_back(transform(r));
        }
        a.tracks.push_back(std::move(t));
    }
    return a;
}

std::vector<Layer> parse_layers(std::span<const std::byte> data) {
    ByteReader r(data);
    if (r.fixed_string(4) != "EQGL") throw std::runtime_error("not EQGL");
    std::uint32_t version = r.le<std::uint32_t>();
    std::uint32_t ns = count(r, data.size());
    std::uint32_t n = count(r, data.size());
    // Entry sizes in u32s per version (0x100645f0).
    std::size_t words = 0;
    switch (version) {
    case 1: words = 10; break;
    case 2: words = 16; break;
    case 3: words = 7; break;
    case 4: words = 8; break;
    default: throw std::runtime_error("unsupported EQGL version " + std::to_string(version));
    }
    auto strings = r.bytes(ns);
    std::vector<Layer> out;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::vector<std::uint32_t> w(words);
        for (auto& x : w) x = r.le<std::uint32_t>();
        Layer l;
        l.name = name_at(strings, w[0]);
        for (std::size_t k = 1; k <= 5; ++k) {
            if (w[k] == 0xffffffffu) continue;
            std::string tex = name_at(strings, w[k]);
            // The role: the character five from the end, compared as is.
            char role = tex.size() >= 5 ? tex[tex.size() - 5] : '\0';
            if (role == 'C') l.diffuse = tex;
            else if (role == 'N') l.normal = tex;
            else if (role == 'E') l.environment = tex;
        }
        if (version == 3 || version == 4) {
            l.shininess = std::bit_cast<float>(w[6]);
            l.env_strength = version == 4 ? std::bit_cast<float>(w[7]) : 1.0f;
            if (!std::isfinite(*l.shininess) || !std::isfinite(*l.env_strength))
                throw std::runtime_error("non-finite EQGL scalar");
        }
        out.push_back(std::move(l));
    }
    return out;
}

std::string layer_name(std::string_view code, std::uint32_t variant, std::size_t material) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "C_%s_S%02u_M%02u", upper(code).c_str(), variant,
                  static_cast<unsigned>(material + 1));
    return buf;
}

std::string_view clip_code(std::string_view prefix) {
    // The tag's sequence (0xaae3d8) and its old-style number (the new-to-old map at 0xaaf428): STND -> 0x20 P01.
    std::string p = upper(prefix);
    for (int code = 0x50; code <= 0xb6; ++code)
        if (game::mechanics::new_style_tag(static_cast<std::uint8_t>(code)) == p)
            return game::mechanics::old_anim_clip(game::mechanics::new_to_old_anim(static_cast<std::uint8_t>(code)));
    return {};
}

bool is_track_name(std::string_view name) {
    // InitAnimationSoundTracks' "%s_%s_%d" before the model code: a sequence's tag, a weapon style of 0xaaf410 and
    // 1..4.
    if (name.size() != 9 || name[4] != '_' || name[7] != '_' || name[8] < '1' || name[8] > '4') return false;
    std::string_view style = name.substr(5, 2);
    if (std::find(std::begin(kTrackStyles), std::end(kTrackStyles), style) == std::end(kTrackStyles)) return false;
    std::string_view tag = name.substr(0, 4);
    for (int code = 0x50; code <= 0xb6; ++code)
        if (game::mechanics::new_style_tag(static_cast<std::uint8_t>(code)) == tag) return true;
    return false;
}

std::map<std::string, std::string, std::less<>> classic_aliases(
    const std::function<bool(std::string_view)>& has_track) {
    std::map<std::string, std::string, std::less<>> out;
    for (std::uint8_t old = 1; old < 0x4f; ++old) {
        std::string_view tag = game::mechanics::new_style_tag(game::mechanics::old_to_new_anim(old));
        if (tag.empty()) continue;
        std::string track = std::string(tag) + "_BA_1";
        if (has_track(track)) out.emplace(std::string(game::mechanics::old_anim_clip(old)), track);
    }
    return out;
}

std::vector<std::size_t> select_pieces(const MeshFile& mesh, std::string_view code_view, std::uint32_t body,
                                       std::uint32_t head) {
    std::string code = upper(code_view);
    auto find_piece = [&](const std::string& name) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < mesh.pieces.size(); ++i)
            if (upper(mesh.pieces[i].name) == name) return i;
        return std::nullopt;
    };
    auto two = [](std::uint32_t v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%02u", v % 100);
        return std::string(buf);
    };
    std::vector<std::size_t> out;
    if (auto b = find_piece(code + two(body))) out.push_back(*b);
    else if (auto p = find_piece(code)) out.push_back(*p);
    else if (auto p00 = find_piece(code + "00")) out.push_back(*p00);
    else out.push_back(0);
    std::optional<std::size_t> he = find_piece(code + "HE" + two(head));
    if (!he) he = find_piece(code + "HE00");
    if (he && *he != out.front()) out.push_back(*he);
    return out;
}

std::optional<std::vector<std::int32_t>> part_bone_parents(const MeshFile& mesh) {
    const std::size_t n = mesh.bones.size();
    std::vector<std::int32_t> parent(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        std::int32_t c = mesh.bones[i].child;
        for (std::uint32_t k = 0; k < mesh.bones[i].children && k < n && c >= 0 && static_cast<std::size_t>(c) < n; ++k) {
            if (parent[static_cast<std::size_t>(c)] == -1 && static_cast<std::size_t>(c) != i)
                parent[static_cast<std::size_t>(c)] = static_cast<std::int32_t>(i);
            c = mesh.bones[static_cast<std::size_t>(c)].next;
        }
    }
    // Every chain reaches a root within n steps, or the hierarchy loops (build_model refuses those too).
    for (std::size_t i = 0; i < n; ++i) {
        std::int32_t b = parent[i];
        std::size_t steps = 0;
        while (b >= 0 && steps++ < n) b = parent[static_cast<std::size_t>(b)];
        if (b >= 0) return std::nullopt;
    }
    return parent;
}

bool bind_part_to_wld(const MeshFile& file, std::string_view body_code,
                      std::span<const std::string> body_bones, int anchor,
                      const glm::mat4& adjustment, CharacterMesh& mesh) {
    if (anchor < 0 || static_cast<std::size_t>(anchor) >= body_bones.size()) return false;
    auto parents = part_bone_parents(file);
    if (!parents) return false;
    const auto count = file.bones.size();
    std::vector<glm::mat4> bind(count, glm::mat4(1.0f));
    std::vector<bool> done(count, false);
    for (std::size_t pass = 0; pass < count; ++pass)
        for (std::size_t b = 0; b < count; ++b) {
            if (done[b]) continue;
            const auto parent = (*parents)[b];
            if (parent >= 0 && !done[static_cast<std::size_t>(parent)]) continue;
            const auto& pose = file.bones[b].bind;
            const auto local = glm::translate(glm::mat4(1.0f), pose.translation) * glm::mat4_cast(pose.rotation) *
                               glm::scale(glm::mat4(1.0f), pose.scale);
            bind[b] = parent < 0 ? local : bind[static_cast<std::size_t>(parent)] * local;
            done[b] = true;
        }
    std::vector<int> host(count, -1);
    for (std::size_t b = 0; b < count; ++b) {
        const auto name = upper(body_code) + upper(file.bones[b].name) + "_TRACK";
        for (std::size_t h = 0; h < body_bones.size(); ++h)
            if (upper(body_bones[h]) == name) { host[b] = static_cast<int>(h); break; }
    }
    std::vector<SkinnedVertex> vertices = mesh.vertices;
    std::vector<CharacterMesh::SkinBinding> bindings;
    std::map<std::int32_t, std::uint16_t> palette;
    for (auto& vertex : vertices)
        for (std::size_t k = 0; k < 4; ++k) {
            if (!(vertex.weights[static_cast<glm::length_t>(k)] > 0.0f)) { vertex.bones[k] = 0; continue; }
            std::int32_t b = vertex.bones[k] < count ? static_cast<std::int32_t>(vertex.bones[k]) : -1;
            while (b >= 0 && host[static_cast<std::size_t>(b)] < 0) b = (*parents)[static_cast<std::size_t>(b)];
            auto entry = palette.find(b);
            if (entry == palette.end()) {
                const auto index = body_bones.size() + mesh.skin_bindings.size() + bindings.size();
                if (index > std::numeric_limits<std::uint16_t>::max()) return false;
                CharacterMesh::SkinBinding binding;
                binding.host_bone = static_cast<std::uint16_t>(b < 0 ? anchor : host[static_cast<std::size_t>(b)]);
                binding.local = adjustment;
                if (b >= 0) {
                    if (std::abs(glm::determinant(bind[static_cast<std::size_t>(b)])) < 1e-12f) return false;
                    binding.local *= glm::inverse(bind[static_cast<std::size_t>(b)]);
                }
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        if (!std::isfinite(binding.local[c][r])) return false;
                bindings.push_back(binding);
                entry = palette.emplace(b, static_cast<std::uint16_t>(index)).first;
            }
            vertex.bones[k] = entry->second;
        }
    mesh.vertices = std::move(vertices);
    mesh.skin_bindings.insert(mesh.skin_bindings.end(), bindings.begin(), bindings.end());
    return true;
}

void append_pieces(const MeshFile& mesh, const std::vector<std::size_t>& pieces, CharacterMesh& out) {
    for (std::size_t pi : pieces) {
        const Piece& piece = mesh.pieces[pi];
        auto vbase = static_cast<std::uint32_t>(out.vertices.size());
        out.vertices.insert(out.vertices.end(), piece.vertices.begin(), piece.vertices.end());
        // Group triangles by material, one part each.
        std::map<std::int32_t, std::vector<std::uint32_t>> groups;
        for (std::size_t t = 0; t < piece.triangles.size(); ++t) {
            if (piece.triangle_materials[t] < 0) continue;
            auto& g = groups[piece.triangle_materials[t]];
            for (auto x : piece.triangles[t]) g.push_back(vbase + x);
        }
        for (auto& [mat, idx] : groups) {
            const MaterialInfo& info = mesh.materials[static_cast<std::size_t>(mat)];
            std::string shader = lower(info.shader);
            Material material;
            material.name = info.name;
            if (!info.diffuse.empty() && lower(info.diffuse) != "none") material.frames.push_back(info.diffuse);
            material.blend = eqg::shader_blend(info.shader);
            material.eqg_shader = eqg::shader_class(info.shader);
            if (info.surface_maps) material.effect = info.effect;
            if (shader.find("invis") != std::string::npos || shader.find("boundary") != std::string::npos)
                material.blend = BlendMode::Invisible;
            std::uint32_t mi = 0;
            for (; mi < out.materials.size(); ++mi) {
                const Material& existing = out.materials[mi];
                if (existing.name != material.name || existing.frames != material.frames) continue;
                if (!info.surface_maps || (
                    existing.blend == material.blend && existing.eqg_shader == material.eqg_shader &&
                    existing.effect.normal_map == material.effect.normal_map &&
                    existing.effect.textures == material.effect.textures &&
                    existing.effect.shininess == material.effect.shininess && existing.effect.gloss == material.effect.gloss &&
                    existing.effect.env_strength == material.effect.env_strength &&
                    existing.effect.specular == material.effect.specular &&
                    existing.effect.reflective == material.effect.reflective)) break;
            }
            if (mi == out.materials.size()) {
                out.materials.push_back(material);
                out.tints.push_back(0);
            }
            MeshPart part;
            part.first_index = static_cast<std::uint32_t>(out.indices.size());
            part.index_count = static_cast<std::uint32_t>(idx.size());
            part.material = mi;
            out.indices.insert(out.indices.end(), idx.begin(), idx.end());
            out.parts.push_back(part);
        }
    }
}

CharacterModel build_model(const MeshFile& mesh, std::string_view code_view,
                           const std::vector<std::pair<std::string, AnimationFile>>& animations,
                           std::size_t max_poses) {
    std::string code = upper(code_view);
    CharacterModel m;
    m.code = code;
    m.eqg = true;
    const std::size_t n = mesh.bones.size();
    if (n == 0) throw std::runtime_error("no bones");
    m.parents.assign(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        std::int32_t c = mesh.bones[i].child;
        for (std::uint32_t k = 0; k < mesh.bones[i].children; ++k) {
            if (c < 0 || static_cast<std::size_t>(c) >= n || static_cast<std::size_t>(c) == i ||
                m.parents[static_cast<std::size_t>(c)] != -1)
                throw std::runtime_error("bad bone hierarchy");
            m.parents[static_cast<std::size_t>(c)] = static_cast<std::int32_t>(i);
            c = mesh.bones[static_cast<std::size_t>(c)].next;
        }
    }
    for (std::size_t i = 0; i < n; ++i)
        if (m.parents[i] < 0) m.order.push_back(static_cast<std::uint32_t>(i));
    for (std::size_t k = 0; k < m.order.size(); ++k)
        for (std::size_t i = 0; i < n; ++i)
            if (m.parents[i] == static_cast<std::int32_t>(m.order[k])) m.order.push_back(static_cast<std::uint32_t>(i));
    if (m.order.size() != n) throw std::runtime_error("cycle in bone hierarchy");

    AnimationClip rest;
    rest.tracks.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        m.bone_names.push_back(mesh.bones[i].name);
        if (m.name_bone < 0 && mesh.bones[i].name == "HEAD_NAME") m.name_bone = static_cast<int>(i);
        rest.tracks[i].push_back(mesh.bones[i].bind);
    }
    std::vector<glm::mat4> bind;
    {
        std::vector<BonePose> local(n);
        for (std::size_t i = 0; i < n; ++i) local[i] = mesh.bones[i].bind;
        m.compose(local, bind);
    }
    m.inverse_bind.resize(n);
    for (std::size_t i = 0; i < n; ++i) m.inverse_bind[i] = glm::inverse(bind[i]);

    CharacterMesh geometry;
    append_pieces(mesh, select_pieces(mesh, code, 0, 0), geometry);
    m.vertices = std::move(geometry.vertices);
    m.indices = std::move(geometry.indices);
    m.parts = std::move(geometry.parts);
    m.materials = std::move(geometry.materials);
    if (m.indices.empty()) throw std::runtime_error("no visible mesh");

    // Clips: tracks matched to bones by name (case-insensitively), resampled
    // to the mean key spacing of the longest track. Bones without a track
    // keep the bind pose.
    std::unordered_map<std::string, std::size_t> by_name;
    for (std::size_t i = 0; i < n; ++i) by_name.emplace(upper(m.bone_names[i]), i);
    std::size_t poses = 0; // resampled across all clips, against kMaxModelPoses
    for (const auto& [clip_name, file] : animations) {
        if (m.clips.contains(clip_name)) continue;
        std::uint32_t duration = 0, step = 0;
        std::size_t max_keys = 0;
        for (const auto& t : file.tracks) {
            if (t.times_ms.empty()) continue;
            duration = std::max(duration, t.times_ms.back());
            max_keys = std::max(max_keys, t.times_ms.size());
        }
        if (max_keys > 1) step = duration / static_cast<std::uint32_t>(max_keys - 1);
        step = std::clamp<std::uint32_t>(step ? step : 33, 10, 200);
        // A clip is cut at kMaxClipFrames frames (13.6 minutes at the 200 ms
        // ceiling; stock clips run seconds), so a bad file's key times (0 and
        // 0xFFFFFFFF) cannot ask for a pose per bone per frame for 50 days.
        duration = std::min(duration, (kMaxClipFrames - 1) * step);
        AnimationClip a;
        a.frame_ms = step;
        a.frame_count = std::max<std::uint32_t>(1, duration / step + 1);
        std::size_t matched = 0;
        for (const auto& t : file.tracks)
            if (!t.poses.empty() && by_name.contains(upper(t.bone))) ++matched;
        if (matched * a.frame_count > max_poses - poses) {
            log::debug("EQG model {}: clip {} skipped, the model's clips are past {} poses", code, clip_name,
                       max_poses);
            continue;
        }
        poses += matched * a.frame_count;
        a.tracks = rest.tracks;
        bool any = false;
        for (const auto& t : file.tracks) {
            auto it = by_name.find(upper(t.bone));
            if (it == by_name.end() || t.poses.empty()) continue;
            any = true;
            auto& keys = a.tracks[it->second];
            keys.clear();
            keys.reserve(a.frame_count);
            for (std::uint32_t fr = 0; fr < a.frame_count; ++fr) {
                std::uint32_t ms = fr * step;
                auto up = std::upper_bound(t.times_ms.begin(), t.times_ms.end(), ms);
                if (up == t.times_ms.begin()) {
                    keys.push_back(t.poses.front());
                } else if (up == t.times_ms.end()) {
                    keys.push_back(t.poses.back());
                } else {
                    auto k = static_cast<std::size_t>(up - t.times_ms.begin());
                    float span = static_cast<float>(t.times_ms[k] - t.times_ms[k - 1]);
                    float u = span > 0.0f ? static_cast<float>(ms - t.times_ms[k - 1]) / span : 0.0f;
                    keys.push_back(blend(t.poses[k - 1], t.poses[k], u));
                }
            }
        }
        if (any) m.clips.emplace(clip_name, std::move(a));
    }
    m.clips.emplace("", std::move(rest));

    // Bind-pose bounds.
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    for (const auto& v : m.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    m.bounds_min = lo;
    m.bounds_max = hi;
    m.compact_animations();
    return m;
}

} // namespace eqg_character

// ---------------------------------------------------------------------------
// EqgCharacterStore

EqgCharacterStore::EqgCharacterStore(GameDir game) : game_(std::move(game)) {
    // OnDemandResources.txt: archive^member^name^type.
    if (auto data = game_.read("Resources/OnDemandResources.txt")) {
        for (const auto& line : text_lines(*data)) {
            std::array<std::string, 4> f;
            std::size_t start = 0, i = 0;
            for (; i < 4; ++i) {
                std::size_t p = line.find('^', start);
                f[i] = line.substr(start, p == std::string::npos ? std::string::npos : p - start);
                if (p == std::string::npos) break;
                start = p + 1;
            }
            if (i < 3) continue;
            std::string archive = lower(f[0]), member = lower(f[1]), name = upper(f[2]), type = upper(f[3]);
            if (!archive.ends_with(".eqg") || !safe_member(archive) || !safe_member(member)) continue;
            if ((type == "EQGS" || type == "EQGM") && name.ends_with("_ACTORDEF")) {
                meshes_.try_emplace(name.substr(0, name.size() - 9), Source{archive, member});
            } else if (type == "EQGA") {
                // <ANIM>_<VARIANT>_<N>_<CODE>
                auto us = name.rfind('_');
                if (us == std::string::npos) continue;
                animations_[name.substr(us + 1)].emplace_back(name.substr(0, us), Source{archive, member});
            } else if (type == "EQGL") {
                layer_sources_.try_emplace(name, Source{archive, member});
            }
        }
    }
}

std::shared_ptr<const PfsArchive> EqgCharacterStore::archive(const std::string& name) {
    if (auto it = archives_.find(name); it != archives_.end()) return it->second;
    std::shared_ptr<const PfsArchive> a;
    if (auto path = game_.find(name)) {
        try {
            a = std::make_shared<const PfsArchive>(PfsArchive::open(
                *path, PfsArchive::storage_for(game_, *path), game_.shared_file_cache()));
            // A model's own textures ("c_pma_bd_s02_c.dds") are its archive's (pma.eqg): another archive's copies
            // of them (mpu.eqg's, an older puma's left in the puma mount's) do not take their place.
            for (const auto& member : a->names())
                if (texture_owner(member) == name) files_.insert_or_assign(member, name);
                else files_.try_emplace(member, name);
        } catch (const std::exception& e) {
            log::warn("EQG archive {}: {}", name, e.what());
        }
    }
    archives_.emplace(name, a);
    // The client loads every member of an EQG archive, its .lay layers too.
    if (a)
        for (const auto& member : a->names())
            if (member.ends_with(".lay")) load_layers(Source{name, member});
    return a;
}

void EqgCharacterStore::load_layers(const Source& lay) {
    std::string key = lay.archive + "/" + lay.member;
    if (std::find(loaded_lays_.begin(), loaded_lays_.end(), key) != loaded_lays_.end()) return;
    loaded_lays_.push_back(key);
    auto a = archive(lay.archive);
    auto data = a ? a->read(lay.member) : std::nullopt;
    if (!data) return;
    try {
        for (auto& l : eqg_character::parse_layers(*data)) {
            std::string name = upper(l.name);
            layers_.try_emplace(std::move(name), std::move(l));
        }
    } catch (const std::exception& e) {
        log::debug("EQG layers {}: {}", key, e.what());
    }
}

const eqg_character::Layer* EqgCharacterStore::layer(std::string_view name_view) {
    std::string name = upper(name_view);
    if (auto it = layers_.find(name); it != layers_.end()) return &it->second;
    if (auto src = layer_sources_.find(name); src != layer_sources_.end()) {
        load_layers(src->second);
        if (auto it = layers_.find(name); it != layers_.end()) return &it->second;
    }
    return nullptr;
}

std::optional<EqgCharacterStore::Source> EqgCharacterStore::mesh_source(const std::string& code) {
    if (auto it = meshes_.find(code); it != meshes_.end()) return it->second;
    std::string file = lower(code) + ".eqg";
    if (!game_.exists(file)) return std::nullopt;
    auto a = archive(file);
    if (!a) return std::nullopt;
    for (const char* ext : {".mds", ".mod"}) {
        std::string member = lower(code) + ext;
        if (a->contains(member)) return Source{file, member};
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, EqgCharacterStore::Source>> EqgCharacterStore::animation_sources(
    const std::string& code) {
    std::vector<std::pair<std::string, Source>> out;
    if (auto it = animations_.find(code); it != animations_.end()) out = it->second;
    // Members of <code>.eqg named <anim>_<code>.ani that the index lacks.
    std::string file = lower(code) + ".eqg";
    if (game_.exists(file))
        if (auto a = archive(file)) {
            std::string suffix = "_" + lower(code) + ".ani";
            for (const auto& member : a->names()) {
                if (!member.ends_with(suffix)) continue;
                std::string clip = upper(member.substr(0, member.size() - suffix.size()));
                bool known = std::any_of(out.begin(), out.end(), [&](const auto& s) { return s.first == clip; });
                if (!known) out.emplace_back(clip, Source{file, member});
            }
        }
    return out;
}

bool EqgCharacterStore::has_model(std::string_view code) { return model(code) != nullptr; }

std::vector<std::string> EqgCharacterStore::codes() const {
    std::vector<std::string> out;
    for (const auto& [code, src] : meshes_) out.push_back(code);
    std::sort(out.begin(), out.end());
    return out;
}

std::shared_ptr<const CharacterModel> EqgCharacterStore::model(std::string_view code_view) {
    std::string code = upper(code_view);
    if (auto it = models_.find(code); it != models_.end()) return it->second;
    std::shared_ptr<const CharacterModel> result;
    try {
        if (auto src = mesh_source(code)) {
            auto a = archive(src->archive);
            auto data = a ? a->read(src->member) : std::nullopt;
            if (!data) throw std::runtime_error(src->archive + ": no " + src->member);
            auto mesh = eqg_character::parse_mesh(*data, code);
            // Animations: the model's own, then its fallback models' for the
            // clips it lacks (game::animation_fallback_code).
            std::vector<std::pair<std::string, eqg_character::AnimationFile>> anims;
            std::string anim_code = code;
            std::vector<std::string> visited;
            for (int depth = 0; depth < 4 && !anim_code.empty(); ++depth) {
                if (std::find(visited.begin(), visited.end(), anim_code) != visited.end()) break;
                visited.push_back(anim_code);
                auto sources = animation_sources(anim_code);
                std::sort(sources.begin(), sources.end(),
                          [](const auto& x, const auto& y) { return x.first < y.first; });
                for (const auto& [name, s] : sources) {
                    // The tracks InitAnimationSoundTracks binds: <tag>_<style>_<n> for the sequence table's tags
                    // (assets/eqg_character.h is_track_name), each kept under that name.
                    if (!eqg_character::is_track_name(name)) continue;
                    if (std::any_of(anims.begin(), anims.end(), [&](const auto& x) { return x.first == name; }))
                        continue;
                    auto aa = archive(s.archive);
                    auto ad = aa ? aa->read(s.member) : std::nullopt;
                    if (!ad) continue;
                    try {
                        anims.emplace_back(name, eqg_character::parse_animation(*ad));
                    } catch (const std::exception& e) {
                        log::debug("EQG animation {}: {}", s.member, e.what());
                    }
                }
                anim_code = std::string(game::animation_fallback_code(anim_code));
            }
            auto m = eqg_character::build_model(mesh, code, anims);
            // The classic clip codes the old-style path and the client's other users ask for: each old-style
            // number's sequence (0xaaf5c8) in the base style, variant 1.
            m.clip_aliases = eqg_character::classic_aliases([&](std::string_view t) { return m.clips.contains(t); });
            mesh_files_[code] = std::make_shared<const eqg_character::MeshFile>(std::move(mesh));
            m.source = src->archive + ":" + src->member;
            // The EQGM loader reads <name>.pts from the mesh's archive (0x100624e0).
            if (auto pts = a->read(lower(code) + ".pts")) {
                try {
                    m.points = parse_pts(*pts);
                } catch (const std::exception& e) {
                    log::debug("EQG model {} points: {}", code, e.what());
                }
            }
            // ... and <name>.prt, the actor emitters on them.
            if (auto prt = a->read(lower(code) + ".prt")) {
                try {
                    m.emitters = parse_prt(*prt);
                } catch (const std::exception& e) {
                    log::debug("EQG model {} emitters: {}", code, e.what());
                }
            }
            if (visited.size() > 1) m.animation_donor = visited[1];
            result = std::make_shared<const CharacterModel>(std::move(m));
            log::debug("EQG model {}: {} bones, {} clips", code, result->bone_names.size(), result->clips.size() - 1);
        }
    } catch (const std::exception& e) {
        log::warn("EQG character model {}: {}", code, e.what());
        result = nullptr;
    }
    models_.emplace(code, result);
    return result;
}

std::optional<std::vector<std::byte>> EqgCharacterStore::read_file(std::string_view name) {
    open_texture_owner(name);
    auto it = files_.find(lower(name));
    if (it == files_.end()) return std::nullopt;
    auto a = archives_.find(it->second);
    if (a == archives_.end() || !a->second) return std::nullopt;
    return a->second->read(lower(name));
}

bool EqgCharacterStore::has_file(std::string_view name) {
    open_texture_owner(name);
    return files_.contains(lower(name));
}

void EqgCharacterStore::open_texture_owner(std::string_view name) {
    std::string owner = texture_owner(lower(name));
    if (!owner.empty() && !archives_.contains(owner) && game_.exists(owner)) archive(owner);
}

std::shared_ptr<const eqg_character::MeshFile> EqgCharacterStore::part_mesh(std::string_view name_view) {
    return part_mesh(name_view, 0);
}

std::shared_ptr<const eqg_character::MeshFile> EqgCharacterStore::part_mesh(std::string_view name_view,
                                                                         std::uint32_t depth) {
    std::string name = upper(name_view);
    if (auto it = parts_.find(name); it != parts_.end()) return it->second;
    std::shared_ptr<const eqg_character::MeshFile> result;
    try {
        std::optional<std::vector<std::byte>> data = read_file(lower(name) + ".mod");
        if (!data)
            if (auto src = mesh_source(name)) {
                auto a = archive(src->archive);
                if (a) data = a->read(src->member);
            }
        if (data) result = std::make_shared<const eqg_character::MeshFile>(eqg_character::parse_mesh(*data, name));
    } catch (const std::exception& e) {
        log::debug("EQG part {}: {}", name, e.what());
    }
    if (!result)
        if (auto layered = layered_part(name)) {
            parts_.emplace(name, nullptr); // a row naming itself as its base stops here
            // Live rows layer on a real mesh, one level deep; a longer chain of
            // distinct names (A on B on C ...) ends at kMaxLayerDepth.
            std::shared_ptr<const eqg_character::MeshFile> base;
            if (depth < kMaxLayerDepth) base = part_mesh(layered->first, depth + 1);
            // The layer: indexed or in an archive opened so far, else in <layer>.eqg (RALLOSIANLEATHER).
            const eqg_character::Layer* l = layer(layered->second);
            if (!l && game_.exists(lower(layered->second) + ".eqg")) {
                archive(lower(layered->second) + ".eqg");
                l = layer(layered->second);
            }
            if (base && l && !l->diffuse.empty()) {
                auto mesh = std::make_shared<eqg_character::MeshFile>(*base);
                for (auto& m : mesh->materials) m.diffuse = lower(l->diffuse);
                result = std::move(mesh);
            } else {
                log::debug("EQG part {}: layered on {} / {}, {}", name, layered->first, layered->second,
                           base ? "no layer" : "no base mesh");
            }
            parts_[name] = result;
            return result;
        }
    parts_.emplace(name, result);
    return result;
}

std::shared_ptr<const eqg_character::MeshFile> EqgCharacterStore::armor_part_mesh(std::string_view name) {
    const std::string key = upper(name);
    if (auto it = armor_parts_.find(key); it != armor_parts_.end()) return it->second;
    // Separate from parts_: an armor alias and an unrelated direct resource
    // may have the same IT number. The provisional entry also breaks cycles.
    armor_parts_.emplace(key, nullptr);
    std::shared_ptr<eqg_character::MeshFile> result;
    bool legacy_fallback = false;
    try {
        if (auto mapped = layered_part(key)) {
            // The native scoped redirect asks for the base actor definition
            // once. A base which also has an alias keeps its direct mesh.
            auto base = part_mesh(mapped->first, 1);
            const eqg_character::Layer* surface = layer(mapped->second);
            if (!surface && game_.exists(lower(mapped->second) + ".eqg")) {
                archive(lower(mapped->second) + ".eqg");
                surface = layer(mapped->second);
            }
            const bool recognized = surface && (!surface->diffuse.empty() || !surface->normal.empty() ||
                                               !surface->environment.empty());
            const bool diffuse_ready = surface && (surface->diffuse.empty() || has_file(surface->diffuse));
            if (base && recognized && diffuse_ready) {
                result = std::make_shared<eqg_character::MeshFile>(*base);
                for (auto& material : result->materials) {
                    const auto& properties = material.surface_properties;
                    if (properties.diffuse && !surface->diffuse.empty()) material.diffuse = lower(surface->diffuse);
                    // Native SetLayer replaces supplied texture roles only;
                    // omitted roles retain the base material's own maps.
                    if (properties.normal && !surface->normal.empty()) material.effect.normal_map = lower(surface->normal);
                    if (properties.environment && !surface->environment.empty())
                        material.effect.textures = {lower(surface->environment)};
                    if (properties.shininess && surface->shininess) material.effect.shininess = *surface->shininess;
                    // Unlike the mesh's named e_* property reader, SetLayer
                    // forwards its environment scalar without doubling it.
                    if (properties.env_strength && surface->env_strength) material.effect.env_strength = *surface->env_strength;
                }
            } else if (base) {
                // Hailstone compatibility policy, not native model-class
                // identification: retain a mixed-era direct appearance only
                // when its bone/piece/vertex/triangle counts match the base.
                auto direct = part_mesh(key);
                const bool compatible = direct && direct->bones.size() == base->bones.size() &&
                    direct->pieces.size() == base->pieces.size() &&
                    std::equal(direct->pieces.begin(), direct->pieces.end(), base->pieces.begin(),
                               [](const eqg_character::Piece& a, const eqg_character::Piece& b) {
                                   return a.vertices.size() == b.vertices.size() && a.triangles.size() == b.triangles.size();
                               });
                if (compatible) {
                    result = std::make_shared<eqg_character::MeshFile>(*direct);
                    legacy_fallback = true;
                    log::debug("EQG armor {}: unavailable layer {}, retaining legacy direct part", key, mapped->second);
                } else {
                    log::debug("EQG armor {}: unavailable layer {} or compatible legacy direct part", key, mapped->second);
                }
            } else {
                log::debug("EQG armor {}: unavailable base {} or layer {}", key, mapped->first, mapped->second);
            }
        } else if (auto direct = part_mesh(key)) {
            result = std::make_shared<eqg_character::MeshFile>(*direct);
        }
        if (result)
            for (auto& material : result->materials) material.surface_maps = !legacy_fallback;
    } catch (const std::exception& e) {
        log::debug("EQG armor {}: {}", key, e.what());
    }
    // A depth-limited or incomplete lookup must not poison a later lookup
    // beginning closer to the real base; successful immutable meshes cache.
    if (result) armor_parts_[key] = result;
    else armor_parts_.erase(key);
    return result;
}

std::optional<std::pair<std::string, std::string>> EqgCharacterStore::layered_part(std::string_view name) {
    if (!layered_parts_) {
        layered_parts_.emplace();
        for (const auto& entry : game_.list("Resources/Layers")) {
            if (!lower(entry.name).ends_with(".txt")) continue;
            auto data = game_.read(entry.relative);
            if (!data) continue;
            for (const auto& raw_line : text_lines(*data)) {
                std::string_view line = raw_line;
                if (line.starts_with("\xef\xbb\xbf")) line.remove_prefix(3);
                std::size_t a = line.find('^'), b = a == std::string::npos ? a : line.find('^', a + 1);
                if (b == std::string::npos || line.find('^', b + 1) != std::string::npos) continue;
                std::string alias = layer_token(line.substr(0, a));
                std::string base = layer_token(line.substr(a + 1, b - a - 1));
                std::string layer = layer_token(line.substr(b + 1));
                if (alias.empty() || base.empty() || layer.empty()) continue;
                layered_parts_->try_emplace(std::move(alias), std::move(base), std::move(layer));
            }
        }
        if (!layered_parts_->empty()) log::info("hero's forge layers: {} layered pieces", layered_parts_->size());
    }
    auto it = layered_parts_->find(upper(name));
    if (it == layered_parts_->end()) return std::nullopt;
    return it->second;
}

std::shared_ptr<const eqg_character::MeshFile> EqgCharacterStore::mesh_file(std::string_view code) const {
    auto it = mesh_files_.find(upper(code));
    return it == mesh_files_.end() ? nullptr : it->second;
}

} // namespace hailstone::assets
