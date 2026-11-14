#include "app/world_objects.h"

#include "game/collision_callbacks.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <span>

#include <glm/gtc/matrix_transform.hpp>

#include "assets/character.h"
#include "assets/eqg.h"
#include "assets/eqg_character.h"
#include "assets/model.h"
#include "assets/model_points.h"
#include "assets/pfs.h"
#include "assets/zone.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "game/coords.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// An item model (equipment archives) as a static mesh.
assets::Mesh to_mesh(const assets::CharacterMesh& c) {
    assets::Mesh m;
    m.vertices.reserve(c.vertices.size());
    for (const auto& v : c.vertices) {
        assets::Vertex out;
        out.position = v.position;
        out.normal = v.normal;
        out.uv = v.uv;
        m.vertices.push_back(out);
    }
    m.indices = c.indices;
    m.parts = c.parts;
    m.materials = c.materials;
    return m;
}

// "IT63_ACTORDEF" / "IT63" -> 63.
std::uint32_t item_number(const std::string& name) {
    if (name.size() > 2 && (name[0] == 'I' || name[0] == 'i') && (name[1] == 'T' || name[1] == 't') &&
        std::isdigit(static_cast<unsigned char>(name[2])))
        return static_cast<std::uint32_t>(std::strtoul(name.c_str() + 2, nullptr, 10));
    return 0;
}

// An EQGS (.mds) model placed as a switch (harbingers' BCN, the beacon): its default pieces (select_pieces(0, 0), as
// for a character) in the bind pose, as a static mesh. EQG triangles wind the other way.
std::optional<assets::Mesh> mds_mesh(std::span<const std::byte> data, const std::string& base) {
    try {
        auto file = assets::eqg_character::parse_mesh(data, base);
        if (file.pieces.empty()) return std::nullopt;
        assets::CharacterMesh c;
        assets::eqg_character::append_pieces(file, assets::eqg_character::select_pieces(file, base, 0, 0), c);
        if (c.indices.empty()) return std::nullopt;
        assets::Mesh m = to_mesh(c);
        m.front_ccw = true;
        return m;
    } catch (const std::exception& e) {
        log::debug("object model {}: {}", base, e.what());
        return std::nullopt;
    }
}

} // namespace

WorldObjects::WorldObjects(render::VkContext& vk, const GameDir& game, const std::filesystem::path& shader_dir)
    : vk_(vk), game_(game), shader_dir_(shader_dir), renderer_(std::make_unique<render::ObjectRenderer>(vk, shader_dir)) {}

WorldObjects::~WorldObjects() = default;

void WorldObjects::set_zone(const assets::Zone* zone, std::shared_ptr<const assets::CharacterLibrary> items) {
    zone_ = zone;
    items_ = std::move(items);
    // The renderer's models read this zone's textures: start a new one.
    vk_.wait_idle();
    renderer_.reset();
    renderer_ = std::make_unique<render::ObjectRenderer>(vk_, shader_dir_);
    models_.clear();
    zone_assets_.clear();
    zone_assets_loaded_ = false;
    doors_.clear();
    objects_.clear();
    placed_.clear();
    placed_tags_.clear();
    ghost_.reset();
    instances_.clear();
    // The missiles' emitter meshes were the old models' (Model::emitters, freed above).
    missile_actors_.clear();
    missile_transforms_.clear();
    prepared_ = false;
}

void WorldObjects::reset_game_files() {
    on_demand_.clear();
    on_demand_loaded_ = false;
    archives_.clear(); // the models made from them go at the next set_zone
}

const WorldObjects::Model* WorldObjects::model_for(const std::string& raw) {
    std::string name = upper(raw);
    if (auto it = models_.find(name); it != models_.end()) return it->second.get();
    const assets::Mesh* mesh = nullptr;
    std::optional<assets::Mesh> owned;
    render::ObjectRenderer::FileReader read;
    std::vector<assets::ModelPoint> points;
    std::vector<assets::ModelEmitter> prt_emitters;
    std::string base = name.ends_with("_ACTORDEF") ? name.substr(0, name.size() - 9) : name;
    if (zone_) {
        // EQSwitch::PostInit creates "<name>_ACTORDEF" (classic object
        // archives); EQG zones name their models "<name>.mod".
        for (const std::string& key : {base + "_ACTORDEF", base, base + ".MOD"}) {
            if (auto it = zone_->objects.find(key); it != zone_->objects.end()) {
                mesh = &it->second;
                break;
            }
        }
        if (!mesh) {
            if (auto data = zone_->read_file(lower(base) + ".mod")) {
                try {
                    assets::eqg::MeshStats stats;
                    owned = assets::eqg::build_mesh(assets::eqg::parse_model(*data, false), base + ".MOD", stats);
                    mesh = &*owned;
                } catch (const std::exception& e) {
                    log::debug("object model {}: {}", base, e.what());
                }
            }
        }
        if (!mesh) {
            // EQSwitch's actor definition can also be an EQG skinned model (EQGS, "<name>.mds").
            if (auto data = zone_->read_file(lower(base) + ".mds")) {
                owned = mds_mesh(*data, base);
                if (owned) mesh = &*owned;
            }
        }
        if (mesh) {
            const assets::Zone* z = zone_;
            read = [z](std::string_view file) { return z->read_file(file); };
        }
    }
    if (!mesh) {
        // The archives <zone>_assets.txt names (CDisplay::StartWorldDisplay loads them after the zone's object and
        // character archives, e.g. the Bazaar's tradeskill_objects.eqg), and PoK's poknowledge_obj3.eqg.
        load_zone_assets();
        for (const auto& a : zone_assets_) {
            auto data = a->read(lower(base) + ".mod");
            if (!data) {
                if (auto mds = a->read(lower(base) + ".mds"); mds && (owned = mds_mesh(*mds, base))) {
                    mesh = &*owned;
                    read = [a](std::string_view file) { return a->read(file); };
                    break;
                }
                continue;
            }
            try {
                assets::eqg::MeshStats stats;
                owned = assets::eqg::build_mesh(assets::eqg::parse_model(*data, false), base + ".MOD", stats);
                mesh = &*owned;
                read = [a](std::string_view file) { return a->read(file); };
                break;
            } catch (const std::exception& e) {
                log::debug("object model {}: {}", base, e.what());
            }
        }
    }
    if (!mesh && items_) {
        // Dropped items: the equipment archives' IT<n> models. Other names: CDisplay::CreateActor's one actor
        // definition table also holds the GlobalLoad archives' other actors (global_obj.s3d's POKTELE500).
        std::uint32_t n = item_number(base);
        if (auto item = n ? items_->item_mesh(n) : items_->actor_mesh(base)) {
            owned = to_mesh(item->mesh);
            owned->front_ccw = item->eqg; // EQG triangles wind the other way
            mesh = &*owned;
            points = item->points; // an EQG item's .pts and .prt (CharacterLibrary::item_mesh)
            prt_emitters = item->emitters;
            auto lib = items_;
            read = [lib](std::string_view file) { return lib->read_file(file); };
        }
    }
    if (!mesh) {
        // Models the original loads on demand: Resources/OnDemandResources.txt
        // (tradeskill containers such as IT10804 in tradeskill_objects.eqg).
        load_on_demand_index();
        if (auto it = on_demand_.find(base); it != on_demand_.end()) {
            if (auto a = archive(it->second.first)) {
                if (auto data = a->read(it->second.second)) {
                    try {
                        assets::eqg::MeshStats stats;
                        owned = assets::eqg::build_mesh(assets::eqg::parse_model(*data, false), base, stats);
                        mesh = &*owned;
                        read = [a](std::string_view file) { return a->read(file); };
                        // The EQGM loader (0x100624e0) reads the model's .pts and .prt beside it.
                        auto pts = a->read(lower(base) + ".pts");
                        auto prt = a->read(lower(base) + ".prt");
                        if (pts && prt) {
                            points = assets::parse_pts(*pts);
                            prt_emitters = assets::parse_prt(*prt);
                        }
                    } catch (const std::exception& e) {
                        log::debug("object model {}: {}", base, e.what());
                    }
                }
            }
        }
    }
    if (!mesh) {
        log::info("no model for door or object {}", name);
        models_.emplace(name, nullptr);
        return nullptr;
    }
    auto model = std::make_unique<Model>();
    model->gpu = renderer_->add_model(*mesh, read);
    model->shape = game::ObjectShape::from_mesh(*mesh);
    for (const auto& e : prt_emitters) {
        const assets::ModelPoint* p = assets::find_point(points, e.point);
        if (!p) continue;
        if (!model->emitters) model->emitters = std::make_unique<assets::CharacterMesh>();
        model->emitters->emitters.push_back({0, assets::point_matrix(*p), e});
    }
    log::debug("object model {}: {} points, {} of {} emitters placed, radius {:.1f}, solid ({:.1f}, {:.1f}, {:.1f})..({:.1f}, {:.1f}, {:.1f})", name,
               points.size(), model->emitters ? model->emitters->emitters.size() : 0, prt_emitters.size(),
               model->shape.radius, model->shape.lo.x, model->shape.lo.y, model->shape.lo.z, model->shape.hi.x,
               model->shape.hi.y, model->shape.hi.z);
    const Model* out = model.get();
    models_.emplace(name, std::move(model));
    return out;
}

void WorldObjects::load_zone_assets() {
    if (zone_assets_loaded_ || !zone_) return;
    zone_assets_loaded_ = true;
    // StartWorldDisplay loads poknowledge_obj3.eqg by zone id (0xca, the Plane of Knowledge) when the zone is not
    // EQG, before the object archives: the Guild Lobby and Bazaar FACADE doors, PORTARENA and PORTBASE live there.
    if (lower(zone_->name) == "poknowledge" && !assets::is_eqg_zone(game_, zone_->name))
        if (auto a = archive("poknowledge_obj3.eqg")) zone_assets_.push_back(std::move(a));
    // One archive per line (StartWorldDisplay's "%s_assets.txt"). The EQG ones hold models; the others
    // (potimeb_chr2, ...) are S3D character archives, which the character loader reads.
    auto data = game_.read(lower(zone_->name) + "_assets.txt");
    if (!data) return;
    std::string text(reinterpret_cast<const char*>(data->data()), data->size());
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? text.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        std::string file = lower(line);
        if (!file.ends_with(".eqg") || file.find('/') != std::string::npos || file.find('\\') != std::string::npos ||
            file.find("..") != std::string::npos)
            continue;
        if (auto a = archive(file)) zone_assets_.push_back(std::move(a));
    }
}

void WorldObjects::load_on_demand_index() {
    if (on_demand_loaded_) return;
    on_demand_loaded_ = true;
    auto data = game_.read("Resources/OnDemandResources.txt");
    if (!data) return;
    std::string text(reinterpret_cast<const char*>(data->data()), data->size());
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // archive^member^name^type
        std::array<std::string, 4> f;
        std::size_t at = 0, n = 0;
        for (; n < 4; ++n) {
            std::size_t caret = line.find('^', at);
            f[n] = line.substr(at, caret == std::string::npos ? std::string::npos : caret - at);
            if (caret == std::string::npos) break;
            at = caret + 1;
        }
        if (n < 3) continue;
        std::string type = upper(f[3]), name = upper(f[2]);
        if ((type != "EQGM" && type != "EQGS") || !name.ends_with("_ACTORDEF")) continue;
        std::string file = lower(f[0]), member = lower(f[1]);
        if (!file.ends_with(".eqg") || file.find('/') != std::string::npos || file.find('\\') != std::string::npos ||
            file.find("..") != std::string::npos)
            continue;
        on_demand_.try_emplace(name.substr(0, name.size() - 9), file, member);
    }
}

std::shared_ptr<const assets::PfsArchive> WorldObjects::archive(const std::string& file) {
    if (auto it = archives_.find(file); it != archives_.end()) return it->second;
    std::shared_ptr<const assets::PfsArchive> a;
    if (auto path = game_.find(file)) {
        try {
            a = std::make_shared<const assets::PfsArchive>(assets::PfsArchive::open(
                *path, assets::PfsArchive::storage_for(game_, *path), game_.shared_file_cache()));
        } catch (const std::exception& e) {
            log::warn("{}: {}", file, e.what());
        }
    }
    archives_.emplace(file, a);
    return a;
}

void WorldObjects::place(Placed& p, const glm::mat4& transform, float scale) {
    // Doors and placed items are placed again each update; most have not moved.
    if (!p.inverted || p.transform != transform) {
        if (p.solid && p.model) floor_changed_ = true;
        p.transform = transform;
        p.inverse = glm::inverse(transform);
        p.inverted = true;
    }
    p.scale = scale;
    p.center = glm::vec3(transform[3]);
    p.bound = p.model ? p.model->shape.radius * scale : 0.0f;
}

void WorldObjects::update(const net::GameConnection& conn) {
    // What a list no longer holds goes: each entry found this update is marked with its serial (no seen-sets to
    // build every frame).
    const std::uint32_t serial = ++update_serial_;
    // Doors.
    for (const auto& d : conn.doors()) {
        auto [it, fresh] = doors_.try_emplace(d.id);
        Placed& p = it->second;
        p.seen = serial;
        if (fresh || !p.model) {
            p.model = game::door_has_actor(d) ? model_for(d.name) : nullptr;
            p.visible = !game::door_is_invisible(d);
            // Invisible switches block too: only the sweep's filter's types 0x32 and 0x33 do not
            // (game::door_blocks_movement).
            p.solid = game::door_blocks_movement(d);
            if (p.model && p.solid) floor_changed_ = true;
        }
        p.floor_switch = game::is_floor_switch(d, conn.zone_id());
        p.type = d.type;
        // EQSwitch::PostInit: type 0x37 loses mask bit 2 while the byte at 0xddf654 is 0 (taken as 0).
        p.pick_mask = d.type == 0x37 ? 0u : 2u;
        glm::mat4 t = game::door_transform(d);
        place(p, t, d.scale());
    }
    std::erase_if(doors_, [&](const auto& kv) {
        const bool gone = kv.second.seen != serial;
        if (gone && kv.second.model && kv.second.solid) floor_changed_ = true;
        return gone;
    });

    // Ground objects.
    for (const auto& o : conn.ground_objects()) {
        auto [it, fresh] = objects_.try_emplace(o.drop_id);
        Placed& p = it->second;
        p.seen = serial;
        if (fresh) {
            // The OP_GroundSpawn handler (@0x4d050f) creates the actor with CreateActor's fallbacks on: a model it
            // lacks shows as IT63 (or RED_BLOB). Every definition in the table is named "..._ACTORDEF", so a long
            // name without that ending finds none.
            std::string def = upper(game::create_actor_definition(o.name));
            p.model = def.ends_with("_ACTORDEF") ? model_for(def) : nullptr;
            for (const char* fallback : game::kCreateActorFallbacks)
                if (!p.model) p.model = model_for(fallback);
            p.solid = false; // items and containers on the ground do not block (an assumption)
            glm::mat4 t = game::ground_object_transform(o);
            place(p, t, std::isfinite(o.size) && o.size > 0.0f ? o.size : 1.0f);
        }
    }
    std::erase_if(objects_, [&](const auto& kv) { return kv.second.seen != serial; });

    // Placed real estate objects (0x1045 / 0x2a9e, game/placement.h): EQPlacedItem_CreateActor's model by
    // CreateActor's rules with its fallbacks, solid unless +0x98 (and never for NPC items, which are spawns: their
    // creature draws as one; the placed record only names it).
    for (const auto& o : conn.real_estate().placed_items()) {
        std::uint64_t key = (std::uint64_t{o.re} << 32) | o.item_id;
        auto [it, fresh] = placed_.try_emplace(key);
        Placed& p = it->second;
        p.seen = serial;
        if (fresh || placed_tags_[key] != o.tag) {
            placed_tags_[key] = o.tag;
            p.model = o.npc_item ? nullptr : placed_model(o.tag);
            floor_changed_ = true;
        }
        if (p.solid != game::placed_solid(o)) floor_changed_ = true;
        p.solid = game::placed_solid(o);
        p.pick_mask = 2;
        // EQPlacedItem_CopyFrom takes a known key's new data without rebuilding the actor; the transform here
        // follows the data (whether the original's actor moves is not traced: docs/unknowns.md).
        place(p, game::placed_transform(o), std::isfinite(o.scale) && o.scale > 0.0f ? o.scale : 1.0f);
    }
    std::erase_if(placed_, [&](const auto& kv) {
        const bool gone = kv.second.seen != serial;
        if (gone && kv.second.model && kv.second.solid) floor_changed_ = true;
        return gone;
    });
    if (placed_tags_.size() != placed_.size())
        std::erase_if(placed_tags_, [&](const auto& kv) { return !placed_.contains(kv.first); });
    // The placement ghost (BeginPlacement's copy of the object, actor type 5).
    if (conn.placement().active() && !conn.placement().ghost().npc_item) {
        const net::RePlacedItem& g = conn.placement().ghost();
        if (!ghost_ || ghost_tag_ != g.tag) {
            ghost_ = Placed{};
            ghost_tag_ = g.tag;
            ghost_->model = placed_model(g.tag);
        }
        ghost_->solid = false;
        place(*ghost_, game::placed_transform(g), std::isfinite(g.scale) && g.scale > 0.0f ? g.scale : 1.0f);
    } else {
        ghost_.reset();
    }

    if (floor_changed_) ++floor_generation_;
    floor_changed_ = false;

    instances_.clear();
    for (const auto& [id, p] : doors_)
        if (p.model && p.visible) instances_.push_back({p.model->gpu, p.transform});
    for (const auto& [id, p] : objects_)
        if (p.model) instances_.push_back({p.model->gpu, p.transform});
    for (const auto& [id, p] : placed_)
        if (p.model) instances_.push_back({p.model->gpu, p.transform});
    if (ghost_ && ghost_->model) instances_.push_back({ghost_->model->gpu, ghost_->transform});
}

const WorldObjects::Model* WorldObjects::placed_model(const std::string& tag) {
    // CDisplay::CreateActor (EQPlacedItem_CreateActor passes the fallbacks on): "_ACTORDEF" for a short tag, IT63,
    // then RED_BLOB.
    std::string def = upper(game::create_actor_definition(tag));
    const Model* m = def.ends_with("_ACTORDEF") ? model_for(def) : nullptr;
    for (const char* fallback : game::kCreateActorFallbacks)
        if (!m) m = model_for(fallback);
    return m;
}

std::optional<WorldObjects::PlacedBounds> WorldObjects::placed_bounds(std::uint32_t re, std::uint32_t item) const {
    auto it = placed_.find((std::uint64_t{re} << 32) | item);
    if (it == placed_.end() || !it->second.model) return std::nullopt;
    const Placed& p = it->second;
    return PlacedBounds{p.center, p.bound, p.model->shape.lo, p.model->shape.hi};
}

std::optional<WorldObjects::PlacedBounds> WorldObjects::ghost_bounds() const {
    if (!ghost_ || !ghost_->model) return std::nullopt;
    return PlacedBounds{ghost_->center, ghost_->bound, ghost_->model->shape.lo, ghost_->model->shape.hi};
}

void WorldObjects::set_missiles(const std::vector<game::Missile>& missiles,
                                const std::function<bool(std::uint32_t serial)>& hidden) {
    constexpr float kUnit = 2.0f * std::numbers::pi_v<float> / 512.0f;
    missile_actors_.clear();
    missile_transforms_.clear();
    for (const game::Missile& m : missiles) {
        if (!m.has_actor || (hidden && hidden(m.serial))) continue;
        const Model* model = model_for(m.packet.model);
        if (!model) model = model_for("IT63");
        if (!model) model = model_for("RED_BLOB");
        if (!model) continue;
        glm::mat4 t = glm::translate(glm::mat4(1.0f), m.actor_position);
        t = glm::rotate(t, m.heading * kUnit, glm::vec3(0, 0, 1));
        t = glm::rotate(t, -m.tilt * kUnit, glm::vec3(0, 1, 0));
        t = glm::scale(t, glm::vec3(m.actor_scale));
        instances_.push_back({model->gpu, t});
        if (model->emitters) {
            const std::uint32_t id = kMissileActorBit | m.serial;
            missile_actors_.push_back({id, model->emitters.get()});
            missile_transforms_[id] = t;
        }
    }
}

bool WorldObjects::missile_transform(std::uint32_t id, glm::mat4& world) const {
    auto it = missile_transforms_.find(id);
    if (it == missile_transforms_.end()) return false;
    world = it->second;
    return true;
}

void WorldObjects::draw(const render::FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
                        const game::ViewDistance& view) {
    if (!prepared_) renderer_->prepare(frame, instances_);
    renderer_->draw_prepared(frame, view_proj, camera_pos, view);
    prepared_ = false;
}

void WorldObjects::prepare(const render::FrameInfo& frame) {
    renderer_->prepare(frame, instances_);
    prepared_ = true;
}

void WorldObjects::draw_shadow(const render::FrameInfo& frame, std::uint32_t cascade) {
    renderer_->draw_shadow(frame, cascade);
}

void WorldObjects::set_brightness(float b) { renderer_->set_brightness(b); }
void WorldObjects::set_time(double seconds) { renderer_->set_time(seconds); }
void WorldObjects::set_lighting(const render::SceneLighting& lighting) { renderer_->set_lighting(lighting); }

bool WorldObjects::line_of_sight(const glm::vec3& a, const glm::vec3& b) const {
    for (const auto& [id, p] : doors_)
        if (p.model && p.solid && p.visible && game::blocks(p.model->shape, p.transform, p.inverse, p.scale, a, b))
            return false; // the line-segment filter (0x100080a0) skips invisible actors
    for (const auto& [id, p] : placed_)
        if (p.model && p.solid && game::blocks(p.model->shape, p.transform, p.inverse, p.scale, a, b)) return false;
    return true;
}

std::optional<float> WorldObjects::floor_below(const glm::vec3& at, float step_up) const {
    std::optional<float> best;
    for (const auto& [id, p] : doors_) {
        if (!p.model || !p.solid) continue;
        if (glm::length(glm::vec2(p.center) - glm::vec2(at)) > p.bound + 0.5f) continue;
        if (auto z = game::floor_under(p.model->shape, p.inverse, at, step_up); z && (!best || *z > *best)) best = z;
    }
    for (const auto& [id, p] : placed_) {
        if (!p.model || !p.solid) continue;
        if (glm::length(glm::vec2(p.center) - glm::vec2(at)) > p.bound + 0.5f) continue;
        if (auto z = game::floor_under(p.model->shape, p.inverse, at, step_up); z && (!best || *z > *best)) best = z;
    }
    return best;
}

bool WorldObjects::sweep_sphere(game::SphereSweep& sweep, std::vector<DoorHit>* hits,
                                const std::vector<std::uint8_t>* pass) const {
    bool hit = false;
    for (const auto& [id, p] : doors_) {
        if (pass && std::find(pass->begin(), pass->end(), id) != pass->end()) continue;
        if (p.model && p.solid && game::sweep_sphere(p.model->shape, p.transform, p.inverse, p.scale, sweep)) {
            hit = true;
            if (hits) hits->push_back({id, p.type, sweep.t});
        }
    }
    // Placed objects block like any solid actor; they are not switches, so no door hit is recorded.
    for (const auto& [id, p] : placed_)
        if (p.model && p.solid && game::sweep_sphere(p.model->shape, p.transform, p.inverse, p.scale, sweep)) hit = true;
    return hit;
}

void WorldObjects::sweep_floor_switches(game::SphereSweep& sweep, std::vector<DoorHit>* hits) const {
    for (const auto& [id, p] : doors_)
        if (p.model && p.floor_switch &&
            game::sweep_sphere(p.model->shape, p.transform, p.inverse, p.scale, sweep) && hits)
            hits->push_back({id, p.type, sweep.t});
}

void WorldObjects::touch_overlap(game::SphereSweep& sweep, std::vector<DoorHit>& hits) const {
    for (const auto& [id, p] : doors_)
        if (p.model && game::switch_acts_on_touch(p.type) &&
            game::sweep_sphere(p.model->shape, p.transform, p.inverse, p.scale, sweep))
            hits.push_back({id, p.type, sweep.t});
}

void WorldObjects::pick_actors(std::vector<game::PickActor>& actors, std::vector<PickRef>& refs) const {
    auto add = [&](const Placed& p, bool is_door, std::uint32_t id) {
        if (!p.model || p.model->shape.records.empty()) return;
        game::PickActor a;
        a.mask = p.pick_mask;
        a.invisible = !p.visible;
        a.position = p.center;
        a.bound_center = p.center;
        a.bound_radius = p.bound;
        a.static_mesh = true;
        const Placed* placed = &p;
        a.triangles = [placed](const game::PickTriangleSink& sink) {
            for (const auto& r : placed->model->shape.records) {
                game::PickTriangle t;
                t.v0 = glm::vec3(placed->transform * glm::vec4(r.v0, 1.0f));
                t.v1 = glm::vec3(placed->transform * glm::vec4(r.v1, 1.0f));
                t.v2 = glm::vec3(placed->transform * glm::vec4(r.v2, 1.0f));
                t.normal = game::normalize_or_zero(glm::cross(t.v0 - t.v1, t.v2 - t.v1));
                t.flags = r.flags;
                sink(t);
            }
        };
        actors.push_back(std::move(a));
        refs.push_back({is_door, id});
    };
    for (const auto& [id, p] : doors_) add(p, true, id);
    for (const auto& [id, p] : objects_) add(p, false, id);
    for (const auto& [key, p] : placed_) {
        std::size_t before = refs.size();
        add(p, false, 0);
        if (refs.size() > before) {
            refs.back().is_placed = true;
            refs.back().re = static_cast<std::uint32_t>(key >> 32);
            refs.back().item = static_cast<std::uint32_t>(key & 0xffffffffu);
        }
    }
}

std::optional<std::uint8_t> WorldObjects::nearest_door(const glm::vec3& at, float yaw, float range) const {
    std::optional<std::uint8_t> best;
    float best_d = range;
    glm::vec2 facing(std::cos(yaw), std::sin(yaw));
    for (const auto& [id, p] : doors_) {
        if (!p.model || !p.visible) continue;
        glm::vec3 c = p.transform * glm::vec4((p.model->shape.lo + p.model->shape.hi) * 0.5f, 1.0f);
        glm::vec2 to = glm::vec2(c) - glm::vec2(at);
        float d = glm::length(to);
        if (d > best_d) continue;
        if (d > 1e-3f && glm::dot(to / d, facing) < 0.5f) continue;
        best = id;
        best_d = d;
    }
    return best;
}

std::optional<glm::vec3> WorldObjects::door_center(std::uint8_t id) const {
    auto it = doors_.find(id);
    if (it == doors_.end() || !it->second.model || it->second.model->shape.empty()) return std::nullopt;
    const auto& s = it->second.model->shape;
    return glm::vec3(it->second.transform * glm::vec4((s.lo + s.hi) * 0.5f, 1.0f));
}

float WorldObjects::door_radius(std::uint8_t id) const {
    auto it = doors_.find(id);
    return it == doors_.end() ? 0.0f : it->second.bound;
}

std::size_t WorldObjects::modelled_doors() const {
    return static_cast<std::size_t>(std::count_if(doors_.begin(), doors_.end(), [](const auto& kv) { return kv.second.model != nullptr; }));
}

} // namespace hailstone::app
