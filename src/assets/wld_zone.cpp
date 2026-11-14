// Classic zone loading: WLD fragments -> model.h meshes, placements, lights.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>

#include "assets/wld.h"
#include "assets/wld_detail.h"
#include "assets/wld_particles.h"
#include "assets/zone.h"
#include "core/asset_folder.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "game/zone_names.h"

namespace hailstone::assets {

namespace wld {

BlendMode blend_mode(std::uint32_t rm) {
    if (rm & 0x80000000u) {
        // "User-defined" types 0x00..0x29: what CMaterial::ResolveTransparency
        // @0x100138d0 makes of the flag word CRender::InitLegacyRenderMethodTable
        // @0x1009aa40 stores for each (decoded as for a standard method below).
        switch (rm & 0x7fffffffu) {
        case 0x00: case 0x1a: case 0x1e: return BlendMode::Invisible;
        case 0x01: case 0x02: case 0x03: case 0x04: case 0x0c: case 0x0d: case 0x0e: case 0x11: case 0x12:
        case 0x14: case 0x15: case 0x1f: case 0x20: case 0x21: case 0x22: return BlendMode::Opaque;
        case 0x07: case 0x08: case 0x13: case 0x16: case 0x19: case 0x25: case 0x26: return BlendMode::Masked;
        case 0x06: case 0x09: case 0x24: case 0x27: return BlendMode::Transparent25;
        case 0x05: case 0x0f: case 0x18: case 0x23: return BlendMode::Transparent50;
        case 0x0a: case 0x28: return BlendMode::Transparent75;
        case 0x0b: case 0x10: case 0x17: case 0x29: return BlendMode::Additive;
        // 0x1b..0x1d are never stored and 0x2a and up lie past the table, so
        // what the DLL reads for them is not known (docs/unknowns.md). Shipped:
        // 0x4b and 0x53 (untextured, kept invisible) and one 0x31 (gequip).
        case 0x4b: case 0x53: return BlendMode::Invisible;
        default: return BlendMode::Opaque;
        }
    }
    // Standard methods, as ResolveTransparency decodes them: none of the bits
    // 0x0100ff00 set, or draw style (bits 0-1) 0, is not drawn; bit 7 is
    // alpha-tested; bit 24 is blended, additive with bit 20 set, otherwise at
    // the opacity in bits 16-19 (x 16). Shipped: 0x4b and 0x53 are not drawn,
    // 0x54b, 0x553 and 0x573 are opaque, 0x5d3 is masked.
    if ((rm & 0x0100ff00u) == 0 || (rm & 0x3) == 0) return BlendMode::Invisible;
    if (rm & 0x80u) return BlendMode::Masked;
    if (rm & 0x01000000u) {
        if (rm & 0x00100000u) return BlendMode::Additive;
        // BlendMode has three opacities; the table only uses 0x40, 0x80 and 0xc0.
        auto alpha = ((rm >> 16) & 0xfu) << 4;
        return alpha < 0x60 ? BlendMode::Transparent25 : alpha < 0xa0 ? BlendMode::Transparent50
                                                                      : BlendMode::Transparent75;
    }
    return BlendMode::Opaque;
}

std::uint32_t bgra_to_rgba(std::uint32_t c) {
    return (c & 0xff00ff00u) | ((c & 0xffu) << 16) | ((c >> 16) & 0xffu);
}

namespace {

Material build_material(const WldFile& wld, Ref ref) {
    Material m;
    const Fragment* f = wld.resolve(ref);
    const auto* def = f ? f->get<MaterialDef>() : nullptr;
    if (!def) {
        m.blend = BlendMode::Invisible;
        return m;
    }
    m.name = f->name;
    m.raw_render_method = def->render_method;
    m.blend = blend_mode(def->render_method);
    m.two_sided = (def->flags & 1) != 0;
    const auto* sprite = wld.get<BitmapInfoRef>(def->bitmap);
    const auto* info = sprite ? wld.get<BitmapInfo>(sprite->info) : nullptr;
    if (!info) return m;
    for (Ref frame : info->frames) {
        // Each frame's first name is the diffuse layer; later names are
        // detail maps, not animation frames (OpenEQ docs/ARCHITECTURE.md).
        const auto* bmp = wld.get<BitmapName>(frame);
        if (bmp && !bmp->files.empty()) {
            m.frames.push_back(bmp->files.front());
            apply_detail_names(bmp->files, m);
        }
    }
    if (m.frames.size() > 1) m.frame_delay_ms = info->frame_delay_ms;
    return m;
}

} // namespace

Mesh build_mesh(const WldFile& wld, const MeshDef& def, std::string name) {
    Mesh out;
    out.name = std::move(name);
    out.vertices.resize(def.positions.size());
    for (std::size_t i = 0; i < def.positions.size(); ++i) {
        auto& v = out.vertices[i];
        v.position = def.positions[i];
        if (i < def.normals.size()) v.normal = def.normals[i];
        if (i < def.uvs.size()) v.uv = def.uvs[i];
        if (i < def.colors.size()) v.color = bgra_to_rgba(def.colors[i]);
    }
    const auto* list = wld.get<MaterialList>(def.material_list);
    const std::size_t slots = list ? list->materials.size() : 0;

    // Polygons are stored in runs of {count, material slot}; gather them per
    // slot so each material is one draw.
    // Passable polygons (flag 0x10: no collision) go in their own parts.
    std::vector<std::vector<std::uint32_t>> buckets(slots * 2);
    std::size_t cursor = 0, bad_slot = 0, bad_index = 0;
    auto add_run = [&](std::size_t count, std::size_t slot) {
        std::size_t end = std::min(cursor + count, def.polygons.size());
        for (; cursor < end; ++cursor) {
            if (slot >= slots) { ++bad_slot; continue; }
            const auto& p = def.polygons[cursor];
            if (p.v[0] >= out.vertices.size() || p.v[1] >= out.vertices.size() || p.v[2] >= out.vertices.size()) {
                ++bad_index;
                continue;
            }
            auto& b = buckets[slot * 2 + ((p.flags & kPolygonPassable) ? 1 : 0)];
            b.insert(b.end(), {p.v[0], p.v[1], p.v[2]});
        }
    };
    for (const auto& g : def.render_groups) add_run(g[0], g[1]);
    if (def.render_groups.empty()) add_run(def.polygons.size(), 0);
    std::size_t leftover = def.polygons.size() - cursor;
    if (bad_slot || bad_index || leftover)
        log::warn("{}: mesh {}: dropped {} polygons with a bad material slot, {} with bad indices, {} outside any run",
                  wld.label(), out.name, bad_slot, bad_index, leftover);

    // Zone meshes share one palette of every zone material; build only the
    // slots this mesh draws, in slot order (not the whole palette per mesh).
    for (std::size_t slot = 0; slot < slots; ++slot) {
        if (buckets[slot * 2].empty() && buckets[slot * 2 + 1].empty()) continue;
        auto material = static_cast<std::uint32_t>(out.materials.size());
        out.materials.push_back(build_material(wld, list->materials[slot]));
        for (std::size_t passable = 0; passable < 2; ++passable) {
            const auto& b = buckets[slot * 2 + passable];
            if (b.empty()) continue;
            MeshPart part;
            part.first_index = static_cast<std::uint32_t>(out.indices.size());
            part.index_count = static_cast<std::uint32_t>(b.size());
            part.material = material;
            part.solid = passable == 0;
            out.indices.insert(out.indices.end(), b.begin(), b.end());
            out.parts.push_back(part);
        }
    }
    return out;
}

namespace {

// Concatenates b into a (an actor can have several mesh sprites).
void append_mesh(Mesh& a, const Mesh& b) {
    auto vbase = static_cast<std::uint32_t>(a.vertices.size());
    auto ibase = static_cast<std::uint32_t>(a.indices.size());
    auto mbase = static_cast<std::uint32_t>(a.materials.size());
    a.vertices.insert(a.vertices.end(), b.vertices.begin(), b.vertices.end());
    for (auto i : b.indices) a.indices.push_back(i + vbase);
    a.materials.insert(a.materials.end(), b.materials.begin(), b.materials.end());
    for (auto p : b.parts) {
        p.first_index += ibase;
        p.material += mbase;
        a.parts.push_back(p);
    }
}

glm::mat4 node_local(const WldFile& wld, const SkeletonNode& node) {
    const auto* ref = wld.get<TrackRef>(node.track);
    const auto* def = ref ? wld.get<TrackDef>(ref->track) : nullptr;
    if (!def || def->frames.empty()) return glm::mat4(1.0f);
    const auto& f = def->frames.front();
    glm::quat q = f.rotation;
    float len = glm::length(q);
    q = std::isfinite(len) && len > 1e-6f ? q / len : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    // Frame 0 keeps the stored W sign; the native rest pose feeds it to
    // D3DXMatrixRotationQuaternion unchanged (OpenEQ docs/WLD_OBJECT_ANIMATION.md).
    return glm::translate(glm::mat4(1.0f), f.translation) * glm::mat4_cast(q) *
           glm::scale(glm::mat4(1.0f), glm::vec3(f.scale));
}

void transform_vertices(Mesh& m, std::size_t first, std::size_t count, const glm::mat4& xf) {
    glm::mat3 rot(xf);
    for (std::size_t i = first; i < first + count; ++i) {
        auto& v = m.vertices[i];
        v.position = glm::vec3(xf * glm::vec4(v.position, 1.0f));
        glm::vec3 n = rot * v.normal;
        float len = glm::length(n);
        if (len > 1e-6f) v.normal = n / len;
    }
}

} // namespace

// Rest-pose assembly follows OpenEQ docs/WLD_PLACED_OBJECTS.md: parent *
// local(T * R * S) per node; meshes without vertex runs follow the node that
// references them; no fallback to node 0.
std::optional<std::vector<glm::mat4>> skeleton_rest_pose(const WldFile& wld, const Skeleton& sk) {
    const std::size_t n = sk.nodes.size();
    if (n == 0 || n > 4096) return std::nullopt;
    std::vector<std::int64_t> parent(n, -1);
    for (std::size_t i = 0; i < n; ++i)
        for (auto c : sk.nodes[i].children) {
            if (c < 0 || static_cast<std::size_t>(c) >= n || static_cast<std::size_t>(c) == i || parent[c] != -1)
                return std::nullopt;
            parent[c] = static_cast<std::int64_t>(i);
        }
    std::vector<glm::mat4> world(n, glm::mat4(1.0f));
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < n; ++i)
        if (parent[i] < 0) order.push_back(i);
    for (std::size_t k = 0; k < order.size(); ++k) {
        std::size_t i = order[k];
        glm::mat4 local = node_local(wld, sk.nodes[i]);
        world[i] = parent[i] < 0 ? local : world[static_cast<std::size_t>(parent[i])] * local;
        for (auto c : sk.nodes[i].children) order.push_back(static_cast<std::size_t>(c));
    }
    if (order.size() != n) return std::nullopt; // a cycle without a root
    return world;
}

// The skeleton reader (0x1001e182 in OpenEQ's build) sends a node's
// attachment to one of three branches: 0x2d meshes, 0x11 nested skeletons and
// 0x34 particle clouds, the last becoming a particle actor owned by the
// runtime node (OpenEQ docs/WLD_PARTICLE_ACTORS.md). Its suppression of
// clouds on nodes scaled below about 0.05 (with further translation checks
// that are not traced) is not modelled; no shipped particle node is scaled.
std::vector<ParticleAttachment> skeleton_particles(const WldFile& wld, const Skeleton& sk) {
    std::vector<ParticleAttachment> out;
    std::optional<std::vector<glm::mat4>> world;
    for (std::size_t i = 0; i < sk.nodes.size(); ++i) {
        const Fragment* f = sk.nodes[i].sprite ? wld.resolve(sk.nodes[i].sprite) : nullptr;
        if (!f || !f->holds<ParticleCloud>()) continue;
        if (!world && !(world = skeleton_rest_pose(wld, sk))) return {};
        out.push_back({f, (*world)[i]});
    }
    return out;
}

std::optional<Mesh> build_skeleton_pose(const WldFile& wld, const Skeleton& sk, std::string name) {
    auto pose = skeleton_rest_pose(wld, sk);
    if (!pose) return std::nullopt;
    const std::size_t n = sk.nodes.size();
    const std::vector<glm::mat4>& world = *pose;

    std::vector<Ref> sprites = sk.meshes;
    if (sprites.empty())
        for (const auto& node : sk.nodes)
            if (node.sprite) sprites.push_back(node.sprite);
    std::optional<Mesh> out;
    std::unordered_set<const Fragment*> seen;
    for (Ref s : sprites) {
        const Fragment* sprite = wld.resolve(s);
        const auto* ref = sprite ? sprite->get<MeshRef>() : nullptr;
        const Fragment* mesh_frag = ref ? wld.resolve(ref->mesh) : nullptr;
        const auto* md = mesh_frag ? mesh_frag->get<MeshDef>() : nullptr;
        if (!md || !seen.insert(sprite).second) continue; // particle sprites and repeats
        Mesh m = build_mesh(wld, *md, mesh_frag->name);
        if (!md->vertex_pieces.empty()) {
            std::size_t cursor = 0;
            for (const auto& [count, bone] : md->vertex_pieces) {
                if (bone >= n || cursor + count > m.vertices.size()) return std::nullopt;
                transform_vertices(m, cursor, count, world[bone]);
                cursor += count;
            }
        } else {
            std::size_t owner = n;
            for (std::size_t i = 0; i < n && owner == n; ++i)
                if (sk.nodes[i].sprite && wld.resolve(sk.nodes[i].sprite) == sprite) owner = i;
            if (owner == n) return std::nullopt;
            transform_vertices(m, 0, m.vertices.size(), world[owner]);
        }
        if (!out) out = std::move(m);
        else append_mesh(*out, m);
    }
    if (out) out->name = std::move(name);
    return out;
}

glm::mat4 placement_transform(const ActorInstance& inst) {
    constexpr float kUnit = 2.0f * std::numbers::pi_v<float> / 512.0f;
    glm::mat4 m = glm::translate(glm::mat4(1.0f), inst.position);
    m = glm::rotate(m, inst.rotation_zyx.x * kUnit, glm::vec3(0, 0, 1));
    // The Y angle turns the other way (EQEmu zone-utilities azone negates it;
    // tilted objects only sit flush on sloped terrain this way, see wld.md).
    m = glm::rotate(m, -inst.rotation_zyx.y * kUnit, glm::vec3(0, 1, 0));
    m = glm::rotate(m, inst.rotation_zyx.z * kUnit, glm::vec3(1, 0, 0));
    float s = std::isfinite(inst.scale) && inst.scale > 0.0f ? inst.scale : 1.0f;
    return glm::scale(m, glm::vec3(s));
}

} // namespace wld

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::shared_ptr<const PfsArchive> open_archive(const GameDir& game, const std::string& file) {
    auto path = game.find(file);
    if (!path) return nullptr;
    return std::make_shared<const PfsArchive>(PfsArchive::open(
        *path, PfsArchive::storage_for(game, *path), game.shared_file_cache()));
}

wld::WldFile parse_member(const PfsArchive& archive, std::string_view member) {
    auto data = archive.read(member);
    if (!data) throw std::runtime_error(archive.label() + ": missing " + std::string(member));
    return wld::WldFile::parse(*data, archive.label() + ":" + std::string(member));
}

// The particle nodes of each actor (upper-case name) and where the zone's
// definitions of the clouds are, by cloud name.
struct ActorParticles {
    std::unordered_map<std::string, std::vector<ParticleFixture>> by_actor; // transforms in actor space
    std::unordered_map<std::string, std::uint32_t> definitions;
};

// The zone's definition index for a 0x34 cloud. The DLL registers a converted
// cloud as a named resource and a later cloud of the same name reuses it
// before decoding (OpenEQ docs/WLD_PARTICLE_LOAD_CONTEXT.md), so the first
// definition of a name wins here too. 0 when the cloud cannot be converted.
std::uint32_t particle_definition(Zone& zone, ActorParticles& particles, const wld::WldFile& w,
                                  const wld::Fragment& cloud) {
    if (auto it = particles.definitions.find(cloud.name); it != particles.definitions.end()) return it->second;
    std::uint32_t index = 0;
    const auto& c = cloud.as<wld::ParticleCloud>();
    if (auto tex = particle_texture(w, c.sprite)) {
        if (zone.particle_definitions.empty()) zone.particle_definitions.emplace_back().name = "None";
        index = static_cast<std::uint32_t>(zone.particle_definitions.size());
        zone.particle_definitions.push_back(particle_cloud_definition(c, *tex, cloud.name));
    } else {
        log::debug("{}: particle cloud {} has no texture", w.label(), cloud.name);
    }
    particles.definitions.emplace(cloud.name, index);
    return index;
}

// A WLD's bitmaps are its own archive's files: an object archive can hold a file of the same name as the zone's, but
// another picture (qeytoqrg.s3d's treea.bmp is an opaque DXT1 on the willows' green, qeytoqrg_obj.s3d's the DXT5
// cut-out the willows draw). Zone::read_file searches the zone's archives in order, so such a frame is renamed
// "<archive>/<file>" (assets::stem_of and the texture packs still see <file>) with the object archive's bytes as a
// generated file. A file only in later archives, or the same bytes, keeps its name; `same` remembers the aliases
// found to have the same bytes, so a frame many meshes share is inflated and compared once.
void use_own_textures(Zone& zone, const PfsArchive& own, Mesh& mesh, std::unordered_set<std::string>& same) {
    std::size_t index = 0;
    while (index < zone.archives.size() && zone.archives[index].get() != &own) ++index;
    for (auto& m : mesh.materials)
        for (auto& frame : m.frames) {
            const PfsArchive* earlier = nullptr;
            for (std::size_t i = 0; i < index && !earlier; ++i)
                if (zone.archives[i]->contains(frame)) earlier = zone.archives[i].get();
            if (!earlier || !own.contains(frame)) continue;
            std::string alias = lower(std::filesystem::path(own.label()).stem().string() + "/" + frame);
            if (same.contains(alias)) continue;
            if (!zone.generated_files.contains(alias)) {
                auto mine = own.read(frame);
                if (!mine || mine == earlier->read(frame)) {
                    same.insert(std::move(alias));
                    continue;
                }
                zone.generated_files.emplace(alias, std::move(*mine));
            }
            frame = alias;
        }
}

// Adds every static (mesh-sprite) actor definition of a WLD to the zone and
// returns the fragment indices of the meshes they use. Skeletal actors' 0x34
// particle nodes go to `particles`, also for an actor without a mesh. `own` is
// the WLD's archive when it is not the zone's first (use_own_textures, with `same` its memo).
std::unordered_set<const wld::Fragment*> add_actor_defs(Zone& zone, const wld::WldFile& w, ActorParticles& particles,
                                                        const PfsArchive* own = nullptr,
                                                        std::unordered_set<std::string>* same = nullptr) {
    std::unordered_set<const wld::Fragment*> used;
    std::size_t failed = 0;
    for (const auto& f : w.fragments()) {
        const auto* def = f.get<wld::ActorDef>();
        if (!def || f.name.empty()) continue;
        std::optional<Mesh> mesh;
        const std::string key = upper(f.name);
        const bool first = !zone.objects.contains(key) && !particles.by_actor.contains(key);
        for (wld::Ref s : def->sprites) {
            const auto* sprite = w.resolve(s);
            std::optional<Mesh> built;
            if (const auto* ref = sprite ? sprite->get<wld::MeshRef>() : nullptr) {
                const auto* mesh_frag = w.resolve(ref->mesh);
                if (const auto* md = mesh_frag ? mesh_frag->get<wld::MeshDef>() : nullptr) {
                    used.insert(mesh_frag);
                    built = wld::build_mesh(w, *md, mesh_frag->name);
                }
            } else if (const auto* sref = sprite ? sprite->get<wld::SkeletonRef>() : nullptr) {
                if (const auto* sk = w.get<wld::Skeleton>(sref->skeleton)) {
                    built = wld::build_skeleton_pose(w, *sk, f.name);
                    if (!built) ++failed;
                    if (first)
                        for (const auto& a : wld::skeleton_particles(w, *sk))
                            if (auto index = particle_definition(zone, particles, w, *a.cloud))
                                particles.by_actor[key].push_back({index, a.node});
                }
            }
            if (!built) continue;
            if (!mesh) mesh = std::move(built);
            else wld::append_mesh(*mesh, *built);
        }
        if (!mesh) continue;
        mesh->name = f.name;
        if (own && same && first) use_own_textures(zone, *own, *mesh, *same);
        zone.objects.try_emplace(upper(f.name), std::move(*mesh));
    }
    if (failed) log::debug("{}: {} skeletal actors could not be posed", w.label(), failed);
    return used;
}

constexpr float kParkedZ = -32767.0f;

void grow(Zone& zone, bool& any, glm::vec3 p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return;
    zone.bounds_min = any ? glm::min(zone.bounds_min, p) : p;
    zone.bounds_max = any ? glm::max(zone.bounds_max, p) : p;
    any = true;
}

} // namespace

std::optional<std::vector<std::byte>> Zone::read_file(std::string_view file) const {
    if (!generated_files.empty()) {
        std::string key(file);
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (auto it = generated_files.find(key); it != generated_files.end()) return it->second;
    }
    for (const auto& a : archives)
        if (a && a->contains(file)) return a->read(file);
    return std::nullopt;
}

Zone load_classic_zone(const GameDir& game, std::string_view name_view, const ZoneLoadCancel& cancel) {
    auto check = [&] {
        if (cancel && cancel()) throw ZoneLoadCancelled();
    };
    check();
    std::string name(name_view);
    Zone zone;
    zone.name = name;
    auto main = open_archive(game, name + ".s3d");
    if (!main)
        throw std::runtime_error("zone archive " + name + ".s3d not found" +
                                 (is_live_asset_folder(game)
                                      ? std::string(" (the live client replaced this zone: the server's assets folder "
                                                    "needs its RoF2 files)")
                                      : std::string()));
    zone.archives.push_back(main);
    // The object archives in CDisplay::StartWorldDisplay's order (@0x49b200), all at the zone's resource level, so
    // the first definition of a name wins: OBEQUIP_LEXIT for the Lost Dungeons of Norrath zones (expansion 6: the
    // adventure entrance and exit portals, GKPORTAL701E, MPORTAL700E...), then <zone>_obj2, <zone>_obj and
    // <zone>_2_obj (the Lost Dungeons' and Gates' additions to old zones: grobb's signs, iceclad's rocks, the
    // camps' portals).
    std::vector<std::string> obj_files;
    if (game::zone_expansion(game::zone_id_by_short_name(name)) == 6) obj_files.push_back("obequip_lexit.s3d");
    for (const char* suffix : {"_obj2", "_obj", "_2_obj"}) obj_files.push_back(name + suffix + ".s3d");
    std::vector<std::shared_ptr<const PfsArchive>> obj_archives;
    for (const auto& file : obj_files)
        if (auto a = open_archive(game, file)) {
            zone.archives.push_back(a);
            obj_archives.push_back(a);
        }
    check();

    ActorParticles particles;
    std::unordered_set<std::string> same_textures; // use_own_textures' memo
    // Object meshes: every WLD in the object archives, then any actor
    // definitions in the zone WLD itself. The first definition of a name wins.
    for (const auto& a : obj_archives)
        for (const auto& member : a->names())
            if (member.size() > 4 && member.ends_with(".wld")) {
                check();
                add_actor_defs(zone, parse_member(*a, member), particles, a.get(), &same_textures);
            }

    check();
    auto terrain = parse_member(*main, name + ".wld");
    auto used = add_actor_defs(zone, terrain, particles);
    for (const auto& f : terrain.fragments())
        if (const auto* md = f.get<wld::MeshDef>(); md && !used.contains(&f)) {
            zone.terrain.push_back(wld::build_mesh(terrain, *md, f.name));
            // A region mesh without colours: CTerrain::AddWldRegionMesh @0x1001f630 gives every vertex 0xFF1F1F1F
            // (the float -2.1150884e38 it stores), dark grey with full alpha.
            if (md->colors.empty())
                for (auto& v : zone.terrain.back().vertices) v.color = 0xff1f1f1fu;
        }
    zone.regions = ZoneRegions::from_wld(terrain);
    for (const auto& f : terrain.fragments())
        if (const auto* g = f.get<wld::GlobalAmbient>()) {
            // Stored B, G, R, A; CS3DFile::LoadGlobalAmbient @0x1001b6a0 -> CSceneGraph vtable +0xd0.
            zone.global_ambient = glm::vec3((g->color >> 16) & 0xff, (g->color >> 8) & 0xff, g->color & 0xff) / 255.0f;
            break;
        }

    check();
    if (main->contains("objects.wld")) {
        auto objects = parse_member(*main, "objects.wld");
        std::size_t missing = 0;
        for (const auto& f : objects.fragments()) {
            const auto* inst = f.get<wld::ActorInstance>();
            if (!inst || !inst->has_location) continue;
            std::string actor = inst->actor < 0 ? objects.string_at(inst->actor)
                                                : (objects.at(inst->actor) ? objects.at(inst->actor)->name : "");
            actor = upper(actor);
            // The actor's particle nodes, posed by the placement (the
            // particle actor reads its runtime node's world matrix). Objects
            // parked at Z -32768 are out of the world.
            if (auto it = particles.by_actor.find(actor); it != particles.by_actor.end()) {
                glm::mat4 placed = wld::placement_transform(*inst);
                if (placed[3].z > kParkedZ)
                    for (const auto& a : it->second) zone.particle_fixtures.push_back({a.definition, placed * a.transform});
            }
            auto obj = zone.objects.find(actor);
            if (obj == zone.objects.end()) {
                ++missing;
                continue;
            }
            Placement p;
            p.actor = actor;
            p.transform = wld::placement_transform(*inst);
            if (const auto* ref = objects.get<wld::VertexColorsRef>(inst->vertex_colors))
                if (const auto* vc = objects.get<wld::VertexColors>(ref->colors);
                    vc && vc->colors.size() == obj->second.vertices.size()) {
                    p.vertex_colors.reserve(vc->colors.size());
                    for (auto c : vc->colors) p.vertex_colors.push_back(wld::bgra_to_rgba(c));
                }
            zone.placements.push_back(std::move(p));
        }
        if (missing) log::debug("{}: {} placements name an actor with no static mesh", name, missing);
    }

    if (main->contains("lights.wld")) {
        auto lights = parse_member(*main, "lights.wld");
        for (const auto& f : lights.fragments()) {
            const auto* pl = f.get<wld::PointLight>();
            if (!pl) continue;
            PointLight light;
            light.position = pl->position;
            light.radius = pl->radius;
            const auto* ref = lights.get<wld::LightRef>(pl->light);
            if (const auto* def = ref ? lights.get<wld::LightDef>(ref->light) : nullptr) {
                if (!def->colors.empty()) light.color = def->colors.front();
                else if (!def->levels.empty()) light.color = glm::vec3(def->levels.front());
            }
            if (finite_light(light)) zone.lights.push_back(light);
        }
    }

    bool any = false;
    for (const auto& m : zone.terrain)
        for (const auto& v : m.vertices) grow(zone, any, v.position);
    // Placed objects contribute the corners of their transformed boxes.
    std::unordered_map<std::string, std::pair<glm::vec3, glm::vec3>> boxes;
    for (const auto& [key, m] : zone.objects) {
        if (m.vertices.empty()) continue;
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
        for (const auto& v : m.vertices) {
            lo = glm::min(lo, v.position);
            hi = glm::max(hi, v.position);
        }
        boxes.emplace(key, std::make_pair(lo, hi));
    }
    for (const auto& p : zone.placements) {
        auto box = boxes.find(p.actor);
        if (box == boxes.end()) continue;
        // Z of -32768 marks objects the zone moved out of the world; they
        // would stretch the bounds 32k units down (docs/formats/wld.md).
        if (p.transform[3].z <= kParkedZ) continue;
        auto [lo, hi] = box->second;
        for (int c = 0; c < 8; ++c) {
            glm::vec3 corner(c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z);
            grow(zone, any, glm::vec3(p.transform * glm::vec4(corner, 1.0f)));
        }
    }
    return zone;
}

} // namespace hailstone::assets
