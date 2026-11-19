#pragma once

// The zone's doors (switches) and ground objects as the app sees them:
// their models (found in the zone's object archives, the zone's EQG archive
// or the equipment item models), drawn with render/object_renderer.h, solid
// for movement and the camera, pickable with the mouse, (a lift's riders
// follow it through the floor test, game/mechanics/switches.h). The door states and
// their animation are GameConnection's (game/doors.h).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "game/doors.h"
#include "game/missiles.h"
#include "render/scene_lighting.h"
#include "game/mechanics/ladder.h"
#include "game/object_collision.h"
#include "game/picking.h"
#include "render/object_renderer.h"

namespace hailstone { class GameDir; }
namespace hailstone::assets {
class PfsArchive;
struct Zone;
struct Mesh;
class CharacterLibrary;
struct CharacterMesh;
}
namespace hailstone::net { class GameConnection; }
namespace hailstone::game { struct ViewDistance; }

namespace hailstone::app {

class WorldObjects {
public:
    WorldObjects(render::VkContext& vk, const GameDir& game, const std::filesystem::path& shader_dir);
    ~WorldObjects();

    // A new zone: forgets every model (they come from the zone's archives).
    // `items` supplies item models (IT<n>) for dropped items.
    void set_zone(const assets::Zone* zone, std::shared_ptr<const assets::CharacterLibrary> items);
    // Forgets the archives opened and the on-demand index (Resources/OnDemandResources.txt), which last across
    // zones: the game files' layers changed (another server's assets folder). Call with no zone set.
    void reset_game_files();

    // Follows the connection's doors and ground objects: loads new models
    // and computes this frame's transforms. Call once per frame after the
    // connection's update.
    void update(const net::GameConnection& conn);
    // The missiles in flight (game/missiles.h), after update: their actors this frame. CDisplay::CreateActor's
    // model, or IT63, or RED_BLOB when it has none; at the actor's position, turned (heading, tilt, 0) as
    // CActorData::SetEulerRotation512 does, at CreateActor's scale.
    // `hidden(serial)`: a missile not drawn as its model (one a spell bolt follows).
    void set_missiles(const std::vector<game::Missile>& missiles,
                      const std::function<bool(std::uint32_t serial)>& hidden = {});
    // The drawn missiles whose models have actor emitters (an EQG model's `<model>.prt` on its `.pts` points, e.g.
    // a spell bolt's flames): an id (kMissileActorBit | the missile's serial) and the emitters, each on bone 0,
    // whose world matrix missile_transform gives (app/actor_emitters.h).
    static constexpr std::uint32_t kMissileActorBit = 0x80000000u;
    struct MissileActor {
        std::uint32_t id = 0;
        const assets::CharacterMesh* emitters = nullptr;
    };
    const std::vector<MissileActor>& missile_actors() const { return missile_actors_; }
    bool missile_transform(std::uint32_t id, glm::mat4& world) const;
    // Doors, dropped/placed items and the placement ghost, with their own actor identities. Like missiles, their
    // emitter meshes use bone 0 for the complete object transform; removal drops the identity from this list.
    static constexpr std::uint32_t kObjectActorBit = 0x40000000u;
    const std::vector<MissileActor>& object_actors() const { return object_actors_; }
    bool object_transform(std::uint32_t id, glm::mat4& world) const;

    // This frame's instances to the renderer (render::ObjectRenderer::prepare), before the shadows: the doors and
    // objects then cast into the cascades (draw_shadow) and the lights' cubes (renderer(), for
    // render::plan_point_shadows and draw_point_shadows). draw records the scene pass; without a prepare this frame
    // it prepares first.
    void prepare(const render::FrameInfo& frame);
    void draw_shadow(const render::FrameInfo& frame, std::uint32_t cascade);
    render::ObjectRenderer* renderer() { return renderer_.get(); }
    void draw(const render::FrameInfo& frame, const glm::mat4& view_proj, const glm::vec3& camera_pos,
              const game::ViewDistance& view);
    void set_brightness(float b);
    // The animated textures' clock (render::ObjectRenderer::set_time), before the shadows and the scene.
    void set_time(double seconds);
    void set_lighting(const render::SceneLighting& lighting);

    // Collision with doors and objects (closed doors block, open ones block
    // where they swung to); the same queries as game::CollisionMesh.
    bool line_of_sight(const glm::vec3& a, const glm::vec3& b) const;
    std::optional<float> floor_below(const glm::vec3& p, float step_up) const;
    // Drawn doors and placed scenery, including passable parts, at their current transform. Never dropped items,
    // placement ghosts, missiles or creature actors. This does not affect movement collision.
    std::optional<float> visual_surface_below(const glm::vec3& p) const;
    std::uint32_t visual_generation() const { return visual_generation_; }
    bool visual_surfaces(const glm::vec2& low,const glm::vec2& high,
                         const std::function<bool(const game::CollisionTriangle&)>& visit,std::uint32_t& work) const;
    // Hailstone's grass (render::FloraRenderer::set_cover): the drawn doors, ground objects and placed items whose
    // bounds touch [lo, hi], their opaque triangles copied into the world (a fire's footprint; as the zone's own
    // objects, assets::append_object_cover); the test is true where one lies over a spot
    // as assets::covers_from_above takes a placed object's (kObjectCoverClearance, kObjectCoverReach). Empty when none
    // touches. The test holds no reference to this.
    std::function<bool(const glm::vec3&)> flora_cover(glm::vec2 lo, glm::vec2 hi) const;
    // Where such cover came, went or moved since the last call (each an object's bounds, lo and hi), emptied by it.
    // A door swinging is not counted: the grass keeps the place it was grown by.
    std::vector<std::pair<glm::vec2, glm::vec2>> take_flora_changes() { return std::exchange(flora_changes_, {}); }
    // Counts the updates that changed what floor_below finds: a solid door or placed object came, went, got its model
    // or moved. Spawns measured against the floor (game::WorldState::reground) are measured again when it changes.
    std::uint32_t floor_generation() const { return floor_generation_; }
    // The movement sweep against the solid doors (CCollision::CollideActor's
    // actor pass, after the zone's mesh): true when a door hit was kept, as
    // the hit callback then sets CollidingType to the switch type (3).
    // `hits` gets each door whose hit was kept when it was tested (the hit callback runs for each), with its type.
    struct DoorHit {
        std::uint8_t id = 0;
        std::uint8_t type = 0;
        float t = 1.0f; // the sweep's t after the hit
    };
    // `pass`: door ids the sphere goes through.
    bool sweep_sphere(game::SphereSweep& sweep, std::vector<DoorHit>* hits = nullptr,
                      const std::vector<std::uint8_t>* pass = nullptr) const;
    // The floor test's sweep against the doors FloorActorFilter lets through: floor switches only.
    void sweep_floor_switches(game::SphereSweep& sweep, std::vector<DoorHit>* hits = nullptr) const;
    // 0x509a00's overlap: a still sphere against the switches that act on touch (invisible ones too) (0x510210).
    void touch_overlap(game::SphereSweep& sweep, std::vector<DoorHit>& hits) const;


    // The doors' and ground objects' actors for the mouse pick
    // (game/picking.h), appended to `actors` with what each one is in `refs`:
    // static collision meshes (their solid triangles), mask 2 (CDisplay::
    // CreateActor; EQSwitch::PostInit clears bit 2 for type 0x37), invisible
    // doors flagged as IsInvisible.
    struct PickRef {
        bool is_door = true;
        std::uint32_t id = 0;
        bool is_placed = false;          // a placed real estate object (id unused: re, item)
        std::uint32_t re = 0, item = 0;
    };
    void pick_actors(std::vector<game::PickActor>& actors, std::vector<PickRef>& refs) const;
    // The nearest usable door whose centre is within `range` of `at` and
    // within 60 degrees of facing `yaw` (the U key without a mouse target).
    std::optional<std::uint8_t> nearest_door(const glm::vec3& at, float yaw, float range) const;

    // A ground object that is a ladder (an EQObject whose actor tag starts
    // with "LADDER", EQObject_Construct @0x503e20) as Ladder_MountClickedLadder
    // reads it (app/world_objects_ladder.cpp).
    std::optional<game::mechanics::LadderSwitch> ladder(const net::GameConnection& conn, std::uint32_t drop_id) const;

    // The centre of a door's solid bounds in the world, as it is now.
    std::optional<glm::vec3> door_center(std::uint8_t id) const;
    // The door actor's bounding radius at its scale (UseSwitch's reach test).
    float door_radius(std::uint8_t id) const;

    // A placed real estate object's actor centre and bounding radius (CEverQuest::RMouseUp's reach test), and its
    // model's local bounds (the placement ghost's box for AreaManager::Query); none without a model.
    struct PlacedBounds {
        glm::vec3 center{0.0f};
        float radius = 0.0f;
        glm::vec3 lo{0.0f}, hi{0.0f};
    };
    std::optional<PlacedBounds> placed_bounds(std::uint32_t re, std::uint32_t item) const;
    // The placement ghost's model's local bounds (from its tag), none without a model.
    std::optional<PlacedBounds> ghost_bounds() const;
    std::size_t placed_count() const { return placed_.size(); }

    std::size_t door_count() const { return doors_.size(); }
    std::size_t modelled_doors() const;
    std::size_t object_count() const { return objects_.size(); }

private:
    struct Model {
        int gpu = -1;              // ObjectRenderer handle
        game::ObjectShape shape;   // solid triangles in model space
        bool plant = false;        // assets::is_plant: the grass grows under it (flora_cover)
        bool fire = false;         // assets::is_fire_object: a campfire covers its footprint (flora_cover)
        // Its grass cover in model space (assets::cover_triangles, 3 a triangle): the opaque surfaces, and for a fire
        // all it draws; empty for a plant.
        std::vector<glm::vec3> cover_surfaces, cover_drawn;
        // Model emitters (EQG point records or WLD node clouds), bone 0 = the model's own space; null without.
        std::unique_ptr<assets::CharacterMesh> emitters;
    };
    struct Placed {
        const Model* model = nullptr;
        glm::mat4 transform{1.0f};
        glm::mat4 inverse{1.0f};
        bool solid = true;
        bool visible = true;
        bool floor_switch = false;    // EQSwitch_IsFloorSwitch: the floor test hits it
        std::uint8_t type = 0;        // the switch's type (+0x26)
        float scale = 1.0f;
        glm::vec3 center{0.0f};       // world
        float bound = 0.0f;           // world bounding radius
        std::uint32_t pick_mask = 2;  // the actor's collision restriction mask
        std::uint32_t seen = 0;       // the update that last found it in the connection's lists
        bool inverted = false;        // `inverse` is transform's (computed again only when the transform changes)
        std::uint32_t emitter_actor = 0; // allocated once for this placement; independent of server object IDs
        // Its grass cover (assets::append_object_cover, in the world), made by flora_cover when first asked for and
        // again when the transform it was made at changes: each tile that grows near it would otherwise transform
        // the whole mesh again. Written only by flora_cover, which the grass's recording job alone calls.
        mutable std::vector<glm::vec3> flora_tris;
        mutable glm::mat4 flora_at{0.0f};
        mutable const Model* flora_model = nullptr; // the model flora_tris was made from; null before any
    };

    const Model* model_for(const std::string& name);
    void add_object_emitters(Placed& placed);
    // Resources/OnDemandResources.txt EQGM/EQGS entries: "<NAME>_ACTORDEF" ->
    // (archive, member), e.g. IT10804 -> tradeskill_objects.eqg it10804.mod.
    void load_on_demand_index();
    // The EQG archives <zone>_assets.txt lists, in its order.
    void load_zone_assets();
    std::shared_ptr<const assets::PfsArchive> archive(const std::string& file);
    void place(Placed& p, const glm::mat4& transform, float scale);
    // Records `p`'s bounds in flora_changes_ when it is drawn with a model.
    void note_flora(const Placed& p);

    render::VkContext& vk_;
    const GameDir& game_;
    std::filesystem::path shader_dir_;
    std::unordered_map<std::string, std::pair<std::string, std::string>> on_demand_; // actor -> archive, member
    bool on_demand_loaded_ = false;
    std::vector<std::shared_ptr<const assets::PfsArchive>> zone_assets_;
    bool zone_assets_loaded_ = false;
    std::unordered_map<std::string, std::shared_ptr<const assets::PfsArchive>> archives_;
    std::unique_ptr<render::ObjectRenderer> renderer_;
    const assets::Zone* zone_ = nullptr;
    std::shared_ptr<const assets::CharacterLibrary> items_;
    std::unordered_map<std::string, std::unique_ptr<Model>> models_; // upper-case name -> model (null: none)
    std::unordered_map<std::uint8_t, Placed> doors_;
    std::unordered_map<std::uint32_t, Placed> objects_;
    // Placed real estate objects (EQPlacedItemManager), keyed by re << 32 | item, and the placement ghost.
    std::unordered_map<std::uint64_t, Placed> placed_;
    std::unordered_map<std::uint64_t, std::string> placed_tags_;
    std::optional<Placed> ghost_;
    std::string ghost_tag_;
    std::uint32_t update_serial_ = 0; // update()'s count, for Placed::seen
    std::uint32_t floor_generation_ = 0;
    std::uint32_t visual_generation_ = 0;
    std::vector<std::pair<glm::vec2, glm::vec2>> flora_changes_;
    bool floor_changed_ = false; // within update(): floor_generation_ moves at its end
    const Model* placed_model(const std::string& tag);
    std::vector<render::ObjectRenderer::Instance> instances_;
    std::vector<MissileActor> missile_actors_;
    std::unordered_map<std::uint32_t, glm::mat4> missile_transforms_;
    std::vector<MissileActor> object_actors_;
    std::unordered_map<std::uint32_t, glm::mat4> object_transforms_;
    std::uint32_t next_object_actor_ = kObjectActorBit;
    bool prepared_ = false; // prepare ran this frame (draw clears it)
};

} // namespace hailstone::app
