#include "app/player_movement.h"

#include "game/mechanics/character.h"
#include "game/mechanics/floor_height.h"
#include "game/mechanics/item_stats.h"
#include "game/mechanics/movement_inputs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <format>
#include <utility>

#include <glm/gtc/constants.hpp>

#include "assets/regions.h"
#include "game/collision_callbacks.h"
#include "game/race_models.h"
#include "game/coords.h"
#include "game/inventory.h"
#include "game/spells.h"
#include "net/game_connection.h"
#include "net/item_packets.h"
#include "net/zone_packets.h"

namespace hailstone::app {

namespace mech = game::mechanics;

std::int32_t remote_collision_type(const game::Spawn& spawn, const glm::vec3& from, const glm::vec3& to,
                                    const MoveWorld& world) {
    if (!world.collision_loaded()) return 0;
    const float radius = mech::sweep_radius({spawn.heights.avatar_height, spawn.race});
    const float lift = mech::collision_height_offset(spawn.stand_state, spawn.body_wet, spawn.race,
                                                     spawn.heights.avatar_height, radius);
    const glm::vec3 offset(0.0f, 0.0f, lift);
    const MoveWorld::Sweep hit = world.sweep(from + offset, to + offset, radius);
    std::int32_t type = 0;
    auto contact = [&](std::int32_t actor_type) {
        game::HitActor actor;
        actor.type = actor_type;
        actor.has_owner = true;
        actor.collisions_enabled = true;
        actor.kind = 1;
        actor.player_found = true;
        if (auto result = game::collision_hit_set_colliding_type(actor, false); result.colliding_type)
            type = *result.colliding_type;
    };
    if (hit.ladder_hit) contact(game::kActorLadder);
    // ZoneMoveWorld visits doors after the zone's placed ladder geometry, just as the local adapter does.
    for (const auto& door : hit.door_hits) {
        (void)door;
        contact(game::kActorSwitch);
    }
    return type;
}

namespace {

// Zone flags from the original's zone table (ZoneManager::ZoneManager
// @0x7dca10, EQZoneInfo +0x198).
constexpr std::uint16_t kBazaarZones[] = {151};      // 0x10000000 EQWorldData_IsBazaarZone
constexpr std::uint16_t kBarterZones[] = {151, 346}; // 0x02000000 EQWorldData_IsBarterZone
constexpr std::uint16_t kNoAirZones[] = {171};       // 0x00004000 EQWorldData::IsNoAirZone
constexpr std::uint16_t kNoLevitateZones[] = {       // 0x00000008 EQWorldData_IsZoneFlag8Set
    39, 71, 109, 112, 124, 125, 154, 158, 159, 160, 163, 164, 179, 189, 207, 215, 219,
    223, 227, 225, 281, 292, 316, 351, 371, 381, 421, 459, 460, 461, 462, 463, 464, 465};

// Hailstone: how far over its floor a body must be for a collision hold to keep its SpeedZ (on the ground the feet
// rest a hair over the floor).
constexpr float kHeldInAir = 0.05f;

template <std::size_t N> bool in(const std::uint16_t (&zones)[N], std::uint16_t id) {
    return std::find(std::begin(zones), std::end(zones), id) != std::end(zones);
}

// Fallback before the character is built (first frame): the sum of the
// player's buffs' base values for an SPA. Once the
// Character exists, TotalEffect (game/mechanics) is used instead.
std::int32_t buff_effect(const net::GameConnection& conn, int spa) {
    std::int32_t sum = 0;
    for (const auto& b : conn.spell_state().buffs) {
        if (b.empty()) continue;
        const game::SpellData* s = conn.spell_table().get(b.spell);
        if (!s) continue;
        for (std::size_t i = 0; i < s->effect.size(); ++i)
            if (s->effect[i] == spa) sum += s->base[i];
    }
    return sum;
}

// SpellCache_GetBuffValue(57, k) before the character is built: the
// levitate effects whose Base2 is k (CacheBuffSlotEffects keys SPA 57 by
// Base2), their base values summed (an inverted buff, type 4, subtracts). The
// character's cache (cached_buff) has the CalcAffectChangeGeneric values.
std::int32_t levitate_value(const net::GameConnection& conn, std::int32_t key) {
    std::uint32_t sum = 0;
    for (const auto& b : conn.spell_state().buffs) {
        if (b.empty()) continue;
        const game::SpellData* s = conn.spell_table().get(b.spell);
        if (!s) continue;
        for (std::size_t i = 0; i < s->effect.size(); ++i) {
            if (s->effect[i] != 57 || s->base2[i] != key) continue;
            auto v = static_cast<std::uint32_t>(s->base[i]);
            sum = b.effect_type == 4 ? sum - v : sum + v;
        }
    }
    return static_cast<std::int32_t>(sum);
}

std::int32_t skill(const net::PlayerProfile* p, std::size_t id) {
    return p && id < p->skills.size() ? static_cast<std::int32_t>(p->skills[id]) : 0;
}

// The weights of the 34 possessions slots and their bags' contents.
std::int32_t carried_weight(const net::GameConnection& conn, std::int32_t spa221) {
    const game::Inventory& inv = conn.inventory();
    std::vector<std::vector<std::int32_t>> contents(net::kSlotCursor + 1);
    std::vector<mech::CarriedItem> items;
    for (std::int16_t slot = 0; slot <= net::kSlotCursor; ++slot) {
        const net::Item* it = inv.at(net::ItemSlot::possessions(slot));
        if (!it) continue;
        mech::CarriedItem c;
        c.weight = mech::item_weight(*it);
        c.bag = it->item_class == 1;
        if (c.bag) {
            c.weight_reduction = it->weight_reduction;
            auto& w = contents[static_cast<std::size_t>(slot)];
            for (std::int16_t sub = 0; sub < it->bag_slots; ++sub)
                if (const net::Item* in_bag = inv.at(net::ItemSlot::possessions(slot, sub)))
                    w.push_back(mech::item_weight(*in_bag));
            c.contents = w;
        }
        items.push_back(c);
    }
    return mech::calc_carried_weight(items, spa221);
}

// The movement inputs before the character exists (the first frames after zoning in): the profile's
// values and the buffs' base values, as the client did before the stats port was wired in. Once the
// Character is built, mech::movement_facts takes everything from it (each following the original exactly).
mech::MovementFacts fallback_facts(const net::GameConnection& conn, const net::PlayerProfile* prof,
                                   const net::PlayerStatus& st) {
    mech::MovementFacts f;
    f.spa99 = buff_effect(conn, 99);
    f.spa3 = buff_effect(conn, 3);
    f.spa271 = buff_effect(conn, 271);
    f.spa290 = buff_effect(conn, 290);
    f.spa228 = buff_effect(conn, 228);
    f.spa246 = buff_effect(conn, 246);
    f.spa14 = buff_effect(conn, 14);
    f.swimming_skill = skill(prof, 50);
    f.safe_fall_adjusted = skill(prof, 39);
    f.safe_fall_base = skill(prof, 39);
    f.carried = carried_weight(conn, buff_effect(conn, 221));
    f.strength = prof ? static_cast<std::int32_t>(prof->str) : 0;
    f.stamina = prof ? static_cast<std::int32_t>(prof->sta) : 0;
    f.hp = static_cast<std::int32_t>(st.hp);
    f.max_hp = static_cast<std::int32_t>(st.max_hp > 0 ? st.max_hp : st.hp);
    f.levitate_mode2 = levitate_value(conn, 0);
    f.levitate_mode5 = levitate_value(conn, 1);
    return f;
}

std::uint8_t region_type(const assets::ZoneRegions* regions, const glm::vec3& p) {
    if (!regions || regions->empty()) return 0;
    return static_cast<std::uint8_t>(regions->lookup(p).flags & 0xff);
}

using mech::Vec3f;

std::string v3(const glm::vec3& v) { return std::format("({:.3f}, {:.3f}, {:.3f})", v.x, v.y, v.z); }

glm::vec3 to_glm(const Vec3f& v) { return {v[0], v[1], v[2]}; }
Vec3f to_vec(const glm::vec3& v) { return {v.x, v.y, v.z}; }

// The floor test's world: the zone mesh, the floor switches and the vehicles (MoveWorld::floor_sweep), and the
// spawn's Vehicle (+0x150) and pTouchingSwitch (+0x2d4) the sweep's callback sets.
struct FloorProbe final : mech::FloorQueries {
    const MoveWorld& w;
    std::uint32_t* vehicle;
    std::int32_t* touching = nullptr; // +0x2d4 (a door id, -1 none)
    float height = 0.0f;              // PlayerClient::Height (+0x13c)
    std::int32_t zone = 0;            // unk_CurrentZoneID
    FloorProbe(const MoveWorld& world, std::uint32_t* standing_on) : w(world), vehicle(standing_on) {}
    bool is_3d() override { return true; }
    bool has_vehicle() override { return vehicle && *vehicle != 0; }
    bool scene_accepts(const std::array<float, 3>& p) override { return w.in_world(glm::vec3(p[0], p[1], p[2])); }
    void set_vehicle_none() override {
        if (vehicle) *vehicle = 0;
    }
    void touching_cleared() override {
        if (touching) *touching = -1;
    }
    std::uint32_t sweep(const std::array<float, 3>& a, const std::array<float, 3>& b, float r, bool,
                        float& hit_z) override {
        std::vector<MoveWorld::FloorSwitchHit> switches;
        hit_z = w.floor_sweep(glm::vec3(a[0], a[1], a[2]), glm::vec3(b[0], b[1], b[2]), r, vehicle,
                              touching ? &switches : nullptr);
        // CollsnCallbackForFloor for each switch hit the sweep kept: a floor switch close enough under the spawn
        // becomes the switch it touches.
        for (const auto& s : switches) {
            game::FloorHit h;
            h.player_found = true;
            h.has_owner = true;
            h.type = game::kActorSwitch;
            h.switch_present = true;
            h.switch_type = s.type;
            h.zone = zone;
            h.player_height = height;
            h.distance = s.distance;
            if (game::collsn_callback_for_floor(h).touching_switch) *touching = s.id;
        }
        return 0;
    }
};

// What DoCollisionMovement asks, answered for the local player (the original
// spawn's index order is kept: [0], [1], [2] are the world's x, y, z here).
class PlayerCollision final : public mech::CollisionQueries {
public:
    PlayerCollision(mech::CollisionSpawn& spawn, const MoveWorld* world, glm::vec3& actor, float radius, float height,
                    std::uint32_t now, bool can_swim_jump)
        : spawn_(spawn), world_(world), actor_(actor), radius_(radius), height_(height), now_(now),
          can_swim_jump_(can_swim_jump) {}

    Vec3f actor_location() override { return to_vec(actor_); }
    bool zone_collision_loaded() override { return world_ && world_->collision_loaded(); }
    bool scene_box(Vec3f& min, Vec3f& max) override {
        auto box = world_ ? world_->world_box() : std::nullopt;
        if (!box) return false;
        min = to_vec(box->first);
        max = to_vec(box->second);
        return true;
    }
    void zone_client() override {}
    bool mounted() override { return false; }
    bool is_vehicle_race() override { return false; }
    bool has_property(std::uint32_t) override { return false; }
    double height() override { return height_; }
    double collision_radius() override { return radius_; }
    // 0x506c90 (game/mechanics/can_i_fit_here.h): a jump animation, levitation, the auto-duck.
    bool moving_state() override { return mech::must_collide_still(still); }
    bool rider() override { return false; }
    void set_rider_stand_time(std::uint32_t) override {}
    std::uint32_t display_time() override { return now_; }
    bool sweep_request(const std::array<float, 7>&) override { return true; } // PlayerClient vfe8 @0x4639c0
    bool is_controlled() override { return true; }
    void sweep(const Vec3f& from, const Vec3f& to, bool, bool hit_callback, mech::SweepResult& out) override {
        if (!world_) {
            out = {to, {0.0f, 0.0f, 0.0f}, 0.0f};
            return;
        }
        MoveWorld::Sweep s = world_->sweep(to_glm(from), to_glm(to), radius_);
        if (trace)
            trace->push_back(std::format("sweep {} -> {}: stop {} skitter {:.4f}{}{}{}", v3(to_glm(from)), v3(to_glm(to)),
                                         v3(s.out.position), s.out.skitter_dist, s.collided ? " hit" : "",
                                         s.door_hit ? " door" : "", s.ladder_hit ? " ladder" : ""));
        out.position = to_vec(s.out.position);
        out.skitter_dir = to_vec(s.out.skitter_dir);
        out.skitter_dist = s.out.skitter_dist;
        // CollisionHit_SetCollidingType for a ladder (actor type 6, game/zone_ladders.h) and the doors (switch
        // actors with their EQSwitch) the sweep kept; 0xddf671 (which enables it) is always set in game
        // (ResetClientStateForZone, before any zone). A ladder sets CollidingType 6: the player climbs.
        if (hit_callback && s.ladder_hit) {
            game::HitActor a;
            a.has_owner = true;
            a.type = game::kActorLadder;
            a.collisions_enabled = true;
            a.kind = 1;
            a.player_found = true;
            game::HitResult r = game::collision_hit_set_colliding_type(a, false);
            if (r.colliding_type) spawn_.colliding_type = *r.colliding_type;
        }
        if (hit_callback)
            for (const auto& d : s.door_hits) {
                game::HitActor a;
                a.has_owner = true;
                a.type = game::kActorSwitch;
                a.collisions_enabled = true;
                a.kind = 1;
                a.player_found = true;
                a.door_has_switch = true;
                game::HitResult r = game::collision_hit_set_colliding_type(a, game::switch_acts_on_touch(d.type));
                if (r.colliding_type) spawn_.colliding_type = *r.colliding_type;
                // A switch that acts on touch: +0x2d4 = the switch and PlayerClient::TouchingSwitch.
                if (r.touching_switch && touch) touch(d.id);
            }
    }
    // 0x509a00 (DoCollisionMovement @0x508787, when the spawn does not collide): if the spawn's vf6c holds, a
    // still sphere of the actor's radius x 0.5 (r + r x -0.5, 0x9c4e48) at the actor's location against the
    // switches that act on touch; each one hit is the local player's touched switch (0x475c50).
    void no_collide() override {
        if (!world_ || !touch) return;
        for (std::uint8_t id : world_->touch_overlap(actor_, radius_ + radius_ * -0.5f)) touch(id);
    }
    void move_without_collision(const Vec3f& target, Vec3f& out) override { out = target; }
    bool in_scene_region(const Vec3f& p) override {
        bool in = !world_ || world_->in_world(to_glm(p));
        if (trace) trace->push_back(std::format("IsPointInWorld {}: {}", v3(to_glm(p)), in));
        return in;
    }
    // PlayerZoneClient::CanIFitHere @0x509650 (exact; game/mechanics/can_i_fit_here.h) at the lifted
    // position: the floor test below it, the ceiling above it, room for the collision height.
    bool can_i_fit_here(const Vec3f& p, std::uint32_t) override {
        if (!world_ || !floor_cache || !auto_duck) return true;
        struct Room final : mech::FitQueries {
            const MoveWorld& w;
            mech::FloorCache& cache;
            float avatar_height;
            std::optional<float>* player_ceiling;
            std::vector<std::string>* trace;
            std::uint32_t* standing_on = nullptr;
            std::int32_t* touching = nullptr;
            float touch_height = 0.0f;
            std::int32_t touch_zone = 0;
            Room(const MoveWorld& world, mech::FloorCache& c, float ah, std::optional<float>* pc,
                 std::vector<std::string>* t)
                : w(world), cache(c), avatar_height(ah), player_ceiling(pc), trace(t) {}
            double floor(const std::array<float, 3>& at) override {
                FloorProbe probe(w, standing_on);
                probe.touching = touching;
                probe.height = touch_height;
                probe.zone = touch_zone;
                double f = mech::get_player_floor_height(cache, at, false, avatar_height, mech::FloorWorld{true}, probe);
                if (trace) trace->push_back(std::format("  floor at {}: {:.4f}", v3({at[0], at[1], at[2]}), f));
                return f;
            }
            double ceiling(const std::array<float, 3>& at, float r) override {
                double c = ceiling_probe(at, r);
                if (trace)
                    trace->push_back(std::format("  ceiling at {} r {:.3f}: {:.4f}", v3({at[0], at[1], at[2]}), r, c));
                return c;
            }
            // CDisplay::GetCeilingHeight @0x48c7b0, from its disassembly (no vectors): the scene must accept
            // the point, then a sphere of r x CDisplay+0x2d20 (1.0) is swept from the position up to z 32767
            // (s3dCollideSphereWithWorld with its twelfth argument 1, the floor-only mode: the terrain's walls
            // are skipped; its actor filter 0x4764d0 refuses switches, so doors never count); the ceiling is the stop point's z + r when that is not below z + r (a sphere that
            // cannot move up, already touching something, finds none), else -1e27.
            double ceiling_probe(const std::array<float, 3>& at, float r) {
                glm::vec3 from(at[0], at[1], at[2]);
                if (!w.in_world(from)) return static_cast<double>(mech::kNoFloorHeight);
                MoveWorld::Sweep s = w.world_sweep(from, glm::vec3(at[0], at[1], 32767.0f), r);
                mech::Fpu fpu;
                double top = mech::x87_add(fpu, s.out.position.z, r); // z' + r
                double start = mech::x87_add(fpu, r, at[2]);            // r + z
                if (start < top || start == top || std::isnan(start) || std::isnan(top)) return top;
                return static_cast<double>(mech::kNoFloorHeight);
            }
            // ChangePosition(0x64): the auto-duck's time is up and there is room again (the caller sends it).
            void stand_up() override {
                if (stood_up) *stood_up = true;
            }
            bool* stood_up = nullptr;
            void set_player_ceiling(float c) override {
                if (player_ceiling) *player_ceiling = c; // the listener environment reads it
            }
        } room(*world_, *floor_cache, avatar_height, player_ceiling, trace);
        room.stood_up = stood_up;
        room.standing_on = standing_on;
        room.touching = touching;
        room.touch_height = touch_height;
        room.touch_zone = touch_zone;
        mech::FitSpawn fs;
        fs.actor_radius = radius_;
        fs.avatar_height = avatar_height;
        fs.stand_state = stand_state;
        fs.special_height = special_height;
        floor_cache->collision_counter = spawn_.step_count; // CollisionCounter (+0x1060) is one field
        bool fits = mech::can_i_fit_here(*floor_cache, {p[0], p[1], p[2]}, fs, false, *auto_duck, now_, room);
        spawn_.step_count = floor_cache->collision_counter;
        if (trace) trace->push_back(std::format("CanIFitHere {}: {}", v3(to_glm(p)), fits));
        return fits;
    }
    void clear_stuck(std::uint32_t) override {}
    // 0x5064c0: z - height below the floor height puts z at floor + height and stops the fall.
    bool check_stuck() override {
        float& z = spawn_.physics[2];
        if (!(z - height_ < spawn_.floor_height)) return false;
        if (trace) trace->push_back(std::format("check_stuck: z {:.4f} - {:.3f} below floor {:.4f}", z, height_,
                                                spawn_.floor_height));
        z = height_ + spawn_.floor_height;
        spawn_.physics[5] = 0.0f;
        return true;
    }
    bool collision_test(const mech::CollisionTestInfo& info) override {
        bool hit = world_ && world_->sweep(to_glm(info.from), to_glm(info.to), info.radius).collided;
        if (trace) trace->push_back(std::format("overlap {} r {:.3f}: {}", v3(to_glm(info.from)), info.radius, hit));
        return hit;
    }
    void set_actor_location(const Vec3f& p) override { actor_ = to_glm(p); }
    bool is_player_spawn() override { return true; }
    void after_place() override {}
    bool can_swim_jump_now() override { return can_swim_jump_; }
    bool on_ladder() override { return spawn_.colliding_type == 6; } // 0x59fb80
    void finish_move() override {
        auto box = world_ ? world_->world_box() : std::nullopt;
        Vec3f lo{}, hi{};
        if (box) {
            lo = to_vec(box->first);
            hi = to_vec(box->second);
        }
        bool kept = mech::keep_in_scene(spawn_, box && world_->collision_loaded(), lo, hi);
        if (kept_move) *kept_move = kept;
    }
    void after_move(const Vec3f&, const Vec3f&) override {}

    float avatar_height = 0.0f;
    float special_height = 0.0f; // +0x144 Length (SetHeights)
    std::uint8_t stand_state = 0x64;
    mech::FloorCache* floor_cache = nullptr;
    mech::AutoDuck* auto_duck = nullptr;
    bool* stood_up = nullptr;                       // CanIFitHere's ChangePosition(0x64) ran
    std::optional<float>* player_ceiling = nullptr; // PlayerClient +0x11b4
    std::uint32_t* standing_on = nullptr;           // PlayerClient +0x150
    std::int32_t* touching = nullptr;               // PlayerClient +0x2d4 (a door id, -1 none)
    bool* kept_move = nullptr;                      // 0x507430 saved the position as the last good one
    float touch_height = 0.0f;                      // Height (+0x13c), for CollsnCallbackForFloor
    std::int32_t touch_zone = 0;
    std::function<void(std::uint8_t)> touch;        // +0x2d4 = the door, then PlayerClient::TouchingSwitch
    mech::StillSpawn still;
    std::vector<std::string>* trace = nullptr;

private:
    mech::CollisionSpawn& spawn_;
    const MoveWorld* world_;
    glm::vec3& actor_;
    float radius_, height_;
    std::uint32_t now_;
    bool can_swim_jump_;
};

} // namespace

PlayerMovement::RoomCheck PlayerMovement::can_i_fit_here_now(const MoveWorld* world, float floor_offset,
                                                             float special_height, std::uint32_t now) {
    RoomCheck out;
    if (!world) return out;
    const mech::PhysicsBody& body = ctl_.body;
    float height = mech::collision_height_offset(ctl_.stand_state(), body.body_wet, race_, floor_offset, radius_);
    PlayerCollision pc(collision_, world, actor_, radius_, height, now, false);
    pc.avatar_height = floor_offset;
    pc.special_height = special_height;
    pc.stand_state = ctl_.stand_state();
    pc.floor_cache = &floor_cache_;
    pc.auto_duck = &auto_duck_;
    bool stood_up = false; // the auto-duck's ChangePosition(0x64): the next frame's own CanIFitHere repeats it
    pc.stood_up = &stood_up;
    pc.player_ceiling = &player_ceiling_;
    pc.standing_on = &standing_on_;
    pc.touching = &touching_switch_;
    pc.touch_height = touch_height_;
    pc.touch_zone = touch_zone_;
    pc.touch = [this](std::uint8_t id) { touch_now(id); };
    out.fits = pc.can_i_fit_here({body.feet.x, body.feet.y, body.feet.z + floor_offset}, 0);
    if (player_ceiling_)
        out.room = static_cast<double>(*player_ceiling_) - static_cast<double>(floor_cache_.floor);
    return out;
}

// CDisplay::GetPlayerFloorHeight at the spawn's position (the feet +
// AvatarHeight): the sphere of radius AvatarHeight x 0.1 swept down through the
// zone and the floor switches, cached until the position changes or a moving
// floor switch resets CollisionCounter (game/mechanics/floor_height.h).
//
// Hailstone: the test starts at the movement sphere's bottom when that is above the spawn's z. The sphere's centre
// is max(1.9, radius) above the spawn's z, so with the original's radius (the model's, docs/unknowns.md) its bottom is
// the spawn's z and the sweep never stands on a surface the floor test cannot see. With the stand-in radius (1.78 for
// a human) the bottom is up to 0.12 higher: the sweep rides over the lip of a step a little taller than AvatarHeight
// (the Bazaar arena's 4-unit tiers for a 3.75 one) and rests on top with the spawn's z under it, the floor test
// starting below the top finds nothing, and the player hangs there with its feet inside the step, unable to move.
float PlayerMovement::floor_probe_lift(float floor_offset) const {
    const mech::PhysicsBody& body = ctl_.body;
    const float h = mech::collision_height_offset(ctl_.stand_state(), body.body_wet, race_, floor_offset, radius_);
    return std::max(0.0f, h - radius_);
}

void PlayerMovement::floor_test(const MoveWorld* world, float floor_offset) {
    floor_valid_ = true;
    if (!world || !world->collision_loaded()) return;
    const mech::PhysicsBody& body = ctl_.body;
    FloorProbe probe(*world, &standing_on_);
    probe.touching = &touching_switch_;
    probe.height = touch_height_;
    probe.zone = touch_zone_;
    floor_cache_.collision_counter = collision_.step_count; // CollisionCounter (+0x1060) is one field
    mech::FloorWorld fw;
    fw.controlled = true;
    std::array<float, 3> at{body.feet.x, body.feet.y, body.feet.z + floor_offset + floor_probe_lift(floor_offset)};
    floor_ = static_cast<float>(mech::get_player_floor_height(floor_cache_, at, true, floor_offset, fw, probe));
    collision_.step_count = floor_cache_.collision_counter;
}

void PlayerMovement::reset(const glm::vec3& feet, float yaw) {
    ctl_.reset(feet, game::yaw_to_heading(yaw));
    fly_ = {};
    last_second_ = 0;
    last_step_ = 0;
    drunken_motion_ = {};
    drunkenness_motion_updates_ = 0;
    air_ = -1;
    feet_regions_ = {};
    player_ceiling_.reset();
    standing_on_ = carried_by_ = 0;
    touching_switch_ = -1;
    collision_ = {};
    actor_valid_ = false;
    floor_cache_ = {};
    auto_duck_ = {};
    floor_cache_.ceiling = mech::kNoFloorHeight;
    floor_valid_ = false;
}

mech::FlyMode PlayerMovement::local_fly_mode(const net::GameConnection& conn, const game::Spawn* me,
                                            const mech::MovementFacts& facts, bool no_levitate_zone,
                                            mech::FlyMode current) {
    // The buff slots' spells as the cache last saw them; a change clears the cache flag, so the next effect read
    // rebuilds it. Hailstone sees the slots, not the packets: an OP_Buff that refreshes a slot with the same spell
    // also clears the flag in the original, and is missed here (docs/unknowns.md).
    const auto& slots = conn.spell_state().buffs;
    std::vector<std::uint32_t> now(slots.size(), mech::kNoBuffSpell);
    for (std::size_t i = 0; i < slots.size(); ++i)
        if (!slots[i].empty()) now[i] = slots[i].spell;
    const bool lev2 = facts.levitate_mode2 != 0, lev5 = facts.levitate_mode5 != 0;

    mech::FlyModeEvents ev;
    if (!fly_.ready) {
        // Zone-in: the spawn record's mode, then a rebuild with the profile's buffs. Whether the original's first
        // rebuild comes after its own spawn exists is not settled (docs/unknowns.md); rebuilding keeps a
        // no-levitation zone's rule.
        if (!me) return current;
        fly_.ready = true;
        if (me->fly_mode <= 5) ev.server_mode = static_cast<mech::FlyMode>(me->fly_mode);
        ev.recache = true;
    } else {
        mech::BuffLevitationChange change =
            mech::buff_levitation_change(fly_.buffs, now, [&](std::uint32_t id) -> std::optional<std::int32_t> {
                if (const game::SpellData* s = conn.spell_table().get(id))
                    for (std::size_t i = 0; i < s->effect.size(); ++i)
                        if (s->effect[i] == 57) return s->base2[i];
                return std::nullopt;
            });
        ev.levitate_faded = change.faded;
        ev.levitate_landed = change.landed;
        // The character's cache values standing in for the fallback's (or worn levitation) are a rebuild too.
        ev.recache = change.changed || lev2 != fly_.lev2 || lev5 != fly_.lev5;
        // PlayerPhysics_SetFlyMode throws above 5; such a value is ignored.
        if (me && me->fly_mode_sets != fly_.sets && me->fly_mode <= 5)
            ev.server_mode = static_cast<mech::FlyMode>(me->fly_mode);
    }
    if (me) fly_.sets = me->fly_mode_sets;
    fly_.buffs = std::move(now);
    fly_.lev2 = lev2;
    fly_.lev5 = lev5;
    return mech::local_fly_mode(current, ev, lev2, lev5, std::nullopt, no_levitate_zone);
}

PlayerMovement::Frame PlayerMovement::update(const MovementKeys& keys, float yaw, float pitch, float elapsed_ms,
                                             std::uint32_t now, net::GameConnection& conn,
                                             const assets::ZoneRegions* regions, float floor_offset,
                                             const MoveWorld* world, const game::mechanics::Character* character,
                                             std::uint32_t step_ms, bool local_passive) {
    // The character's values (game/mechanics/movement_inputs.h): TotalEffect, STR / STA with items and
    // buffs, adjusted skills, the heroic AGI bonus, CalcCarriedWeight, Cur_HP / Max_HP; the profile and the
    // buffs' base values before the character is built.
    const mech::MovementFacts facts =
        character ? mech::movement_facts(*character)
                  : fallback_facts(conn, conn.profile() ? &*conn.profile() : nullptr, conn.player_status());
    Frame out;
    mech::PhysicsBody& body = ctl_.body;
    const game::Spawn* me = conn.world().player();
    // Riding (game/mounts.h): the body is the local player's mount, pinstControlledPlayer; ProcessControls, the
    // jump and the physics run on it with its speeds, heights and race, and the rider's character.
    const game::Spawn* mount = nullptr;
    const std::uint32_t controlled = local_passive ? (me ? me->mount_id : 0) : conn.controlled_id();
    const bool eye = !local_passive && conn.eye_id() != 0;
    const game::Spawn* remote = nullptr;
    if (auto it = conn.world().spawns.find(controlled); controlled && it != conn.world().spawns.end())
        remote = &it->second;
    if (!eye) mount = remote;
    const game::Spawn* cp = remote ? remote : me;
    ctl_.body.mount_race = mount && game::is_mount_race(mount->race);
    const net::PlayerProfile* prof = conn.profile() ? &*conn.profile() : nullptr;
    const net::PlayerStatus& st = conn.player_status();
    if (!eye && conn.drunkenness_motion_updates() != drunkenness_motion_updates_) {
        drunkenness_motion_updates_ = conn.drunkenness_motion_updates();
        mech::update_drunken_motion(body, drunken_motion_, conn.drunkenness(), !mount);
    }
    std::uint16_t zone = conn.zone_id();

    // Mouse look sets the heading (HandleMouseLook); the pitch is CameraAngle.
    body.heading = game::yaw_to_heading(yaw);
    body.camera_angle = pitch * 256.0f / glm::pi<float>();
    const float camera_angle_in = body.camera_angle;
    // A stun starting, sitting, looting or lying down stops the body (GameConnection::speed_stops): ProcessControls
    // only stops asking for speed while it cannot move, so the speed the body had would carry it on.
    if (conn.speed_stops() != speed_stops_) {
        body.speed_run = 0.0f;
        if (st.owner_status == 3) body.velocity = glm::vec3(0.0f);
    }
    if (conn.turn_stops() != turn_stops_) body.speed_heading = 0.0f;
    // StunMe's lean: the turn rate 32, which turns the stunned player until it can move again.
    if (conn.stun_leans() != stun_leans_) body.speed_heading = 32.0f;
    speed_stops_ = conn.speed_stops();
    turn_stops_ = conn.turn_stops();
    stun_leans_ = conn.stun_leans();
    if (local_passive) {
        body.speed_run = body.speed_heading = 0.0f;
        auto& passive = ctl_.controls_state();
        passive.keys = {};
        passive.autorun = passive.strafing = 0;
        passive.strafe_scale = passive.held_scale = 0.0f;
    }

    // Vehicle_BeforeStep @0x8d2220 / Vehicle_AfterStep @0x8d22e0 around the vehicle's step: a body standing on a
    // vehicle (+0x150) goes into the vehicle's frame as it was and back into the world as it is now, so it keeps its
    // place on the deck while the vehicle moves and turns; its actor is placed there
    // (Vehicle_PostMoveSetActorLocation). The vehicle is where it was drawn (the model the floor test stood on).
    std::uint32_t moving_deck = 0; // the vehicle that carried the body this frame, if it moved
    if (auto v = standing_on_ ? conn.world().spawns.find(standing_on_) : conn.world().spawns.end();
        v != conn.world().spawns.end()) {
        const game::Spawn& vs = v->second;
        const glm::vec3 vp = vs.display_valid ? vs.display_position : vs.position;
        const mech::VehiclePose now_pose{vp.x, vp.y, vp.z, vs.display_valid ? vs.display_heading : vs.heading, 0.0f};
        if (carried_by_ == standing_on_) {
            if (now_pose.x != carried_pose_.x || now_pose.y != carried_pose_.y || now_pose.z != carried_pose_.z ||
                now_pose.heading != carried_pose_.heading)
                moving_deck = standing_on_;
            mech::VehicleBody b{body.feet.x, body.feet.y, body.feet.z, body.velocity.x, body.velocity.y,
                                body.velocity.z, body.heading, 0.0f};
            mech::vehicle_to_local(carried_pose_, b);
            mech::vehicle_to_world(now_pose, b);
            body.feet = glm::vec3(b.x, b.y, b.z);
            body.velocity = glm::vec3(b.speed_x, b.speed_y, b.speed_z);
            body.heading = b.heading;
            if (actor_valid_) actor_ = body.feet + glm::vec3(0, 0, floor_offset);
            // The last good position (+0x284), where a stuck step goes back to, rides along too (game/boats.h): left
            // behind in the world it would drop the player in the water.
            mech::VehicleBody g{collision_.last_good[0], collision_.last_good[1], collision_.last_good[2]};
            mech::vehicle_to_local(carried_pose_, g);
            mech::vehicle_to_world(now_pose, g);
            collision_.last_good = {g.x, g.y, g.z};
        }
        carried_by_ = standing_on_;
        carried_pose_ = now_pose;
    } else {
        carried_by_ = 0;
    }

    // Where the regions are probed (game/mechanics/regions.h): the body at the
    // spawn's z (the feet plus AvatarHeight, floor_offset), the head at
    // z + ViewHeight - AvatarHeight (the spawn's SetHeights, game/spawn_heights.h),
    // the feet with CheckRegionChange's sample point.
    glm::vec3 body_point = body.feet + glm::vec3(0, 0, floor_offset);
    body.body_wet = region_type(regions, body_point);
    float view_height = cp ? cp->heights.view_height : floor_offset;
    // JumpStrength / SwimStrength (+0x10 / +0x14) as SetHeights wrote them.
    if (cp && cp->heights.jump_strength > 0.0f) ctl_.jump_strength = cp->heights.jump_strength;
    if (cp && cp->heights.swim_strength > 0.0f) ctl_.swim_strength = cp->heights.swim_strength;
    std::uint8_t head_wet = region_type(regions, body_point + glm::vec3(0, 0, view_height - floor_offset));
    if (regions && !regions->empty()) {
        // PlayerClient +0x144 (eqlib's Length): SetHeights' (0 unless ComputeSpecialHeights sets it, races 36
        // and 91); at 0 the sample is the feet + 0.25 (z + 0.25 - AvatarHeight).
        feet_regions_.body.length = cp ? cp->heights.length : 0.0f;
        feet_regions_.body.stand_state = ctl_.stand_state();
        feet_regions_.body.avatar_height = floor_offset;
        feet_regions_.area_friction = body.area_friction;
        feet_regions_.acceleration_friction = body.acceleration_friction;
        struct Probe : mech::RegionQueries {
            const assets::ZoneRegions& zone_regions;
            Frame& out;
            const net::GameConnection& conn;
            Probe(const assets::ZoneRegions& r, Frame& o, const net::GameConnection& c)
                : zone_regions(r), out(o), conn(c) {}
            bool inside(const mech::RegionPoint&) override { return true; }
            std::uint32_t regions(const mech::RegionPoint& p) override {
                return zone_regions.lookup(glm::vec3(p.x, p.y, p.z)).flags;
            }
            // Zone lines are app/zone_lines' (ZoneLineTracker), checked after the move.
            bool do_teleport(const mech::RegionPoint&) override { return false; }
            bool is_player_spawn() override { return true; }
            void print(std::uint32_t id) override { out.messages.push_back(conn.strings().format(id, {})); }
            void feet_environment_change(std::uint32_t) override {}
        } probe(*regions, out, conn);
        mech::RegionZone rz{mech::kGroundFriction, mech::kGroundAcceleration, zone};
        mech::check_region_change(feet_regions_, {body_point.x, body_point.y, body_point.z}, rz, probe);
        body.area_friction = feet_regions_.area_friction;
        body.acceleration_friction = feet_regions_.acceleration_friction;
    }
    // PlayerClient_IsSwimSurfaceClear (game/mechanics/movement.h): the point one AvatarHeight ahead along the
    // heading, 1 below the spawn's z, is clear unless it is in the world and in a region.
    {
        glm::vec3 probe = mech::swim_probe_point(body_point, body.heading, floor_offset);
        bool in_world = !world || world->in_world(probe);
        ctl_.swim_surface_clear = mech::swim_surface_clear(in_world, in_world ? region_type(regions, probe) : 0);
    }

    // The character.
    mech::CharacterInput ch;
    std::uint8_t level = st.level ? st.level : (prof ? prof->level : 1);
    std::uint8_t cls = prof ? prof->class_ : 0;
    if (cp && cp->run_speed > 0.0f) ch.run_speed = cp->run_speed;
    if (cp && cp->walk_speed > 0.0f) ch.walk_speed = cp->walk_speed;
    ch.riding = mount != nullptr;
    ch.remote_control = eye;
    ch.in_combat = conn.in_combat();
    ch.avatar_height = cp ? cp->heights.avatar_height : floor_offset;
    ch.sneak_speed = mech::sneak_speed(cls, level);
    ch.run_walk = conn.running() ? 1 : 0;
    ch.swimming_skill = facts.swimming_skill; // GetAdjustedSkill(50)
    ch.endurance = static_cast<std::int32_t>(st.endurance);
    ch.max_endurance = static_cast<std::int32_t>(st.max_endurance);
    // Max_Endurance (virtual +0x28) as a server's client patch answers it (the NMS patch: the server's value)
    if (character && character->sheet().vitals.max_endurance) ch.max_endurance = character->max_endurance();
    ch.dead = st.dead;
    ch.frozen = conn.sitting() || st.dead || conn.stunned();
    ch.interface_locked = conn.interface_locked();
    if (local_passive) ch.interface_locked = true; // the local body still falls, swims and takes environment damage
    if (eye) {
        ch.frozen = conn.stunned() || st.owner_status > 0 || (cp && cp->stand_state == net::kStandFreeze);
        ch.weight_factor = 1.0f;
    }
    // PlayerZoneClient's Sneak flag: the server's SpawnAppearance 15 (Handle_OP_SpawnAppearance), kept by the
    // abilities code. Sneaking uses sneak_speed and halves the back speed (controls.h).
    ch.sneaking = conn.sneaking();
    if (eye) ch.sneaking = false;
    ch.race = static_cast<std::int32_t>(race_);
    ch.crouch_window = crouch_window_;
    std::int32_t rate = mech::movement_rate_effect(facts.spa99, facts.spa3);
    // On a mount the rider's rate gives the mount's modifier (eqspa_movement_rate's GetMountSpawn part).
    mech::MovementRate mr =
        mount ? mech::eqspa_movement_rate(rate, me && me->run_speed > 0.0f ? me->run_speed : 0.7f, ch.run_walk != 0,
                                          facts.spa271, mount->run_speed)
              : mech::eqspa_movement_rate(rate, ch.run_speed, ch.run_walk != 0, facts.spa271);
    ch.speed_multiplier = eye ? 0.0f : (mount ? mr.mount_multiplier.value_or(0.0f) : mr.speed_multiplier);
    ch.spa290 = facts.spa290;
    std::int32_t carried = facts.carried;
    mech::WeightInput w;
    w.hp = facts.hp;
    w.max_hp = facts.max_hp;
    w.low_level_exempt_off = facts.low_level_exempt_off;
    w.level = level;
    w.carried = carried;
    w.strength = facts.strength;
    w.bazaar = in(kBazaarZones, zone);
    w.barter = in(kBarterZones, zone);
    ch.weight_factor = mech::calculate_weight(w);
    if (std::uint32_t msg = mech::encumbrance_message(encumbered_shown_, w.carried, w.strength))
        out.messages.push_back(conn.strings().format(msg, {}));

    // The fly mode. An eye of Zomm's body takes its own spawn's mode. The player's is set outright by the server
    // (the spawn record at zone-in, OP_SpawnAppearance 19) and by a levitate buff landing or fading, and recomputed
    // from the buffs by UpdateLevitationState only when the effect cache is rebuilt (mech::local_fly_mode). A
    // server-set mode therefore lasts until the buffs next change, as in the original.
    if (eye) {
        if (cp && cp->fly_mode <= 5) body.fly_mode = static_cast<mech::FlyMode>(cp->fly_mode);
    } else {
        body.fly_mode = local_fly_mode(conn, me, facts, in(kNoLevitateZones, zone), body.fly_mode);
    }

    // ProcessControls and the jump.
    mech::ControlInput ci;
    ci.forward = keys.forward;
    ci.back = keys.back;
    ci.turn_left = keys.turn_left;
    ci.turn_right = keys.turn_right;
    ci.strafe_left = keys.strafe_left;
    ci.strafe_right = keys.strafe_right;
    ci.jump = keys.jump;
    ci.mouse_look = keys.mouse_look;
    ci.move_down = keys.move_down;
    ci.both_buttons = keys.both_buttons;
    ci.camera_index = keys.camera_index;
    ci.camera = keys.camera;
    ci.camera_active = keys.camera_active;
    ci.look_spring = conn.client_option(game::ClientOption::LookSpring);
    ci.forward_sits = conn.client_option(game::ClientOption::AllowAutoStand);
    ci.loot_window_open = conn.loot_session().has_value();
    ci.auto_duck = auto_duck_.active != 0;
    ch.sitting = eye ? cp && cp->stand_state == net::kStandSitting : conn.sitting();
    ch.looting = eye ? cp && cp->stand_state == net::kStandLooting : st.stand_state == net::kStandLooting;
    // Feigning (115) reaches ProcessControls as itself: neither standing nor ducking, it cannot move, and the
    // forward key's auto-stand (sitting only) leaves it lying.
    ch.lying = eye ? cp && cp->stand_state == net::kStandLying : st.stand_state == net::kStandLying || st.owner_status == 3;
    // The crouch is the player's stand state (111), from the DUCK key or the standing jump.
    ctl_.crouching = eye ? cp && cp->stand_state == net::kStandCrouching
                         : conn.player_status().stand_state == net::kStandCrouching;
    last_ch_ = ch;
    last_facts_ = facts;
    mech::ControlEvents ev;
    if (at_helm_) {
        auto& cs = ctl_.controls_state();
        const mech::ControlKeys held = cs.keys;
        const std::uint8_t autorun = cs.autorun;
        cs.keys.forward = cs.keys.backward = cs.keys.turn_left = cs.keys.turn_right = false;
        cs.keys.strafe_left = cs.keys.strafe_right = false;
        cs.autorun = 0;
        ci.forward = ci.back = ci.turn_left = ci.turn_right = ci.strafe_left = ci.strafe_right = false;
        ci.both_buttons = false;
        ev = ctl_.controls(ci, ch, now);
        cs.keys = held;
        cs.autorun = autorun;
    } else if (key_override_) {
        auto& cs = ctl_.controls_state();
        const mech::ControlKeys held = cs.keys;
        cs.keys.forward = *key_override_;
        cs.keys.backward = cs.keys.turn_left = cs.keys.turn_right = false;
        cs.keys.strafe_left = cs.keys.strafe_right = false;
        ci.forward = *key_override_;
        ci.back = ci.turn_left = ci.turn_right = ci.strafe_left = ci.strafe_right = false;
        ev = ctl_.controls(ci, ch, now);
        cs.keys = held;
    } else {
        ev = ctl_.controls(ci, ch, now);
    }
    if (ev.auto_duck_cleared) auto_duck_.active = 0;
    if (ev.crouch_changed) out.stand_state = ctl_.crouching ? net::kStandCrouching : net::kStandStanding;
    if (ev.message) out.messages.push_back(conn.strings().format(ev.message, {}));
    if (ev.jumped) conn.report_jump(ev.endurance_cost);
    if (ev.mount_jump) conn.report_mount_jump();
    out.jumped = ev.jumped;
    out.follow_off = ev.turn_off_auto_follow;
    out.camera = ev.camera;
    out.camera_pitch_set = ev.camera_pitch_set;
    out.sit_toggle = ev.sit_toggle;
    out.close_loot = ev.close_loot;
    out.attacks = std::move(ev.attacks);

    // Physics, then walls and floors, then the landing check.
    mech::Environment env;
    if (const auto& z = conn.zone_info(); z && z->gravity != 0.0f) env.gravity = z->gravity;
    env.zone_id = zone;
    // A placement (zone-in, a teleport, a test placement) finds its floor before the first physics step, from the
    // placed spawn z: the EQPlayer constructor's FloorHeight (+0x28, CDisplay::GetFloorHeight at 0x596798 from the
    // OP_ZoneEntry position) and DoTeleportB's GetPlayerFloorHeight(x, y, z + 1). The first landing check then stands
    // the feet on it. (Tested after the step instead, a long first frame let gravity take the feet under a ground only
    // a little below them, and the floor test, starting under it, found the next floor down.)
    if (!floor_valid_) {
        floor_test(world, floor_offset);
        if (world && world->collision_loaded()) body.floor_height = floor_;
    }
    glm::vec3 from = body.feet;
    float hover_floor = body.floor_height;
    if (!actor_valid_) {
        // Placed (zone-in, a teleport): the actor stands there and that is the
        // last good position (+0x284).
        actor_ = from + glm::vec3(0, 0, floor_offset);
        collision_.last_good = {actor_.x, actor_.y, actor_.z};
        actor_valid_ = true;
    }
    // Step measures whole milliseconds since its last run; on the game clock, its step (16 or 17 ms, whatever time a
    // dropped stretch after a hitch took).
    if (last_step_ == 0) last_step_ = now - static_cast<std::uint32_t>(elapsed_ms);
    ctl_.step(env, static_cast<float>(step_ms ? step_ms : now - last_step_), ch, hover_floor);
    if (!eye && !mount) mech::apply_drunken_motion(body, drunken_motion_);
    last_step_ = now;

    // The landing check (PlayerZoneClient_LandingAndFallCheck, PlayerClient
    // vf74 after each physics step, before CollisionPass) against FloorHeight,
    // the last floor test's (below; a placement ran one above).
    // Without a floor (-1e27: the scene graph refused the position) nothing lands and the fall goes on.
    std::optional<float> floor;
    if (!world || !world->collision_loaded()) floor = from.z; // no scenery: a flat plane
    else floor = floor_;
    if (floor) {
        mech::FallDamageInput fall;
        fall.carried = carried;
        fall.safe_fall_adjusted = facts.safe_fall_adjusted; // GetAdjustedSkill(39)
        fall.safe_fall_base = facts.safe_fall_base;
        fall.spa228 = facts.spa228;
        // The zone's no-fall-damage flag (0xddfc6e) comes from OP_NewZone.
        if (const auto& z = conn.zone_info()) fall.no_fall_damage_zone = z->fall_damage_off != 0;
        // dmg -= dmg * h / (h + 1000) with h = heroic AGI
        // (CharacterZoneClient_ApplyFallDamage @0x45ac60).
        fall.heroic_agi_bonus = facts.heroic_agi;
        float vz = body.velocity.z;
        mech::LandingEvents land = ctl_.land(*floor, fall);
        out.landed = land.landed;
        out.land_sound = land.land_sound;
        if (land.landed) out.landing_speed = vz;
        // A ridden mount lands as the controlled spawn: IHaveFallen does nothing for a spawn without a PcClient.
        if (land.fell && !mount && !eye) out.fall_damage = land.damage.damage;
        if (land.damage.safe_fall_used && !mount && !eye) conn.report_safe_fall();
        if (land.damage.damage > 0 && !mount && !eye)
            conn.report_env_damage(static_cast<std::uint32_t>(land.damage.damage), mech::kEnvDamageFalling);
    } else {
        // Nothing below (outside the zone's geometry): hold the height.
        body.feet.z = from.z;
        body.velocity.z = 0.0f;
        body.floor_height = from.z;
    }
    // PlayerClient::CollisionPass: DoCollisionMovement from the actor's
    // location to the integrated position (the spawn's z is the feet +
    // AvatarHeight).
    mech::CollisionSpawn& cs = collision_;
    cs.physics = {body.feet.x, body.feet.y, body.feet.z + floor_offset, body.velocity.x, body.velocity.y,
                  body.velocity.z, body.speed_run, body.heading, body.camera_angle, 0.0f, body.speed_heading,
                  cs.physics[11]};
    cs.floor_height = body.floor_height;
    cs.body_wet = body.body_wet;
    cs.colliding_type = body.colliding_type;
    float height = mech::collision_height_offset(ctl_.stand_state(), body.body_wet, race_, floor_offset, radius_);
    lift_ = floor_offset + height;
    PlayerCollision pc(cs, world, actor_, radius_, height, now,
                       mech::can_swim_jump_now(ctl_.next_swim(), now));
    pc.avatar_height = floor_offset;
    pc.special_height = cp ? cp->heights.length : 0.0f;
    pc.stand_state = ctl_.stand_state();
    pc.floor_cache = &floor_cache_;
    // CanIFitHere ducks under a low ceiling only with AllowAutoDuck (0xde0d5a).
    auto_duck_.allowed = conn.client_option(game::ClientOption::AllowAutoDuck);
    pc.auto_duck = &auto_duck_;
    bool stood_up = false;
    pc.stood_up = &stood_up;
    pc.player_ceiling = &player_ceiling_;
    pc.standing_on = &standing_on_;
    pc.touching = &touching_switch_;
    pc.touch_height = touch_height_;
    pc.touch_zone = touch_zone_;
    pc.touch = [this](std::uint8_t id) { touch_now(id); };
    // 0x506c90's inputs: the jump animation (+0x10c0 L03 / L04, JMPA / JMPU) as the renderer last chose it;
    // without a drawn model, taken as playing for kJumpWindowMs after a jump.
    pc.still.jump_animation = jump_animation_ ? *jump_animation_
                                              : ctl_.jump_ended() != 0 && now - ctl_.jump_ended() < mech::kJumpWindowMs;
    pc.still.controlled = true;
    pc.still.levitate = static_cast<std::int32_t>(body.fly_mode);
    pc.still.auto_duck = auto_duck_.active;
    // PlayerClient::CollisionPass @0x509050 → 0x509240, before DoCollisionMovement: the controlled player handles the
    // switch the last floor test left it touching (PlayerClient::TouchingSwitch).
    if (touching_switch_ >= 0) touch_now(static_cast<std::uint8_t>(touching_switch_));
    mech::Vec3f target{cs.physics[0], cs.physics[1], cs.physics[2]};
    if (trace_) {
        pc.trace = &out.trace;
        out.trace.push_back(std::format("move {} -> {} (spawn z), v {}, floor {:.4f}, lift {:.3f}, r {:.3f}, still {}",
                                        v3(actor_), v3(to_glm(target)), v3(body.velocity), body.floor_height, height,
                                        radius_, mech::must_collide_still(pc.still)));
    }
    bool kept_move = false;
    pc.kept_move = &kept_move;
    const float speed_z = cs.physics[5];
    mech::CollisionMove cm = mech::do_collision_movement(cs, target, pc);
    // Hailstone (a deliberate difference; docs/mechanics.md "Held in the air"): a body in the air that the collision
    // pass holds where it was (a move under 0.01, as at a jump's top) keeps its SpeedZ instead of losing it. Above
    // about 126 frames a second gravity alone cannot make a move of 0.01 in one frame (0.4 x dt^2), so the
    // original's rule held a jump at its top until a slow frame came. Kept, the speed grows as it would over the
    // fewer, longer frames the original ran at, and the body falls on.
    if (cm.held && body.fly_mode == mech::FlyMode::Ground && body.body_wet == 0 &&
        body.feet.z > body.floor_height + kHeldInAir)
        cs.physics[5] = speed_z;
    // 0x507430 after saving the last good position: PlayerClient_RecordPositionSample(1) there, then
    // Send_OP_FloatListThing(0) (once a second, the samples older than 2 s).
    if (kept_move && !eye) {
        const glm::vec3 at(cs.physics[0], cs.physics[1], cs.physics[2]);
        conn.record_position_sample(game::kPositionMove, &at);
        conn.send_position_history(false);
    }
    if (trace_)
        out.trace.push_back(std::format("result {} (spawn z), possibly_stuck {}, stopped {}, region {}, in world {}",
                                        v3(to_glm({cs.physics[0], cs.physics[1], cs.physics[2]})), cs.possibly_stuck,
                                        cm.stopped, region_type(regions, to_glm({cs.physics[0], cs.physics[1],
                                                                                 cs.physics[2]})),
                                        !world || world->in_world(to_glm({cs.physics[0], cs.physics[1],
                                                                           cs.physics[2] + height}))));
    body.feet = glm::vec3(cs.physics[0], cs.physics[1], cs.physics[2] - floor_offset);
    body.velocity = glm::vec3(cs.physics[3], cs.physics[4], cs.physics[5]);
    body.colliding_type = cs.colliding_type;
    if (stood_up) {
        ctl_.crouching = false;
        out.stand_state = net::kStandStanding;
    }
    // The floor under the moved spawn for the next landing check.
    floor_test(world, floor_offset);
    if (trace_) out.trace.push_back(std::format("floor test: {:.4f}", floor_));
    // Rails (game/boats.h kRailDrop; Hailstone's own design, not the original's): on a moving vehicle a walking body
    // whose step would take it off the deck over a drop (no deck of that vehicle under it, the floor more than
    // kRailDrop below) stays where the deck carried it. A jump, swimming or levitating goes over; a still boat
    // (alongside a dock) has none, and a dock at the deck's height is stepped onto.
    const bool walking = !ev.jumped && from.z <= hover_floor + 0.05f && body.fly_mode == mech::FlyMode::Ground &&
                         body.body_wet == 0;
    if (moving_deck && walking && standing_on_ != moving_deck && floor_ < from.z - game::kRailDrop) {
        body.feet = from;
        body.velocity.x = body.velocity.y = 0.0f;
        actor_ = from + glm::vec3(0, 0, floor_offset);
        floor_test(world, floor_offset);
        if (standing_on_ != moving_deck) {
            standing_on_ = moving_deck;
            floor_ = hover_floor;
        }
        if (trace_) out.trace.push_back(std::format("rail: kept on vehicle {} at {}", moving_deck, v3(from)));
    }

    // Once a second (DoPassageOfTime's 1000 ms block): air supply and drowning.
    if (last_second_ == 0) last_second_ = now;
    if (now - last_second_ > 1000 && prof && !st.dead && !eye) {
        last_second_ = now;
        std::int32_t max_air = mech::max_air_supply(facts.stamina, facts.spa246, prof->race);
        max_air_ = max_air;
        // AirSupply (PcBase +0x2084) starts as the profile's (ApplyProfileData).
        if (air_ < 0) air_ = prof->air_supply;
        mech::AirSupply air = mech::process_air_supply(
            air_, mech::can_breathe(head_wet, in(kNoAirZones, zone), prof->race, facts.spa14), max_air, w.max_hp);
        air_ = air.air;
        if (air.drowning_damage > 0)
            conn.report_env_damage(static_cast<std::uint32_t>(air.drowning_damage), mech::kEnvDamageDrowning);
        // Lava and freezing water, with the character's resists.
        if (character) {
            const auto& z = conn.zone_info();
            mech::RegionDamage rd = mech::region_damage(body.body_wet, zone, facts.sv_fire, facts.sv_cold, facts.sv_disease,
                                                        z ? static_cast<std::int32_t>(z->lava_damage) : 0,
                                                        z ? static_cast<std::int32_t>(z->min_lava_damage) : 0);
            if (rd.damage > 0) conn.report_env_damage(static_cast<std::uint32_t>(rd.damage), rd.type);
        }
    }

    out.feet = body.feet;
    out.yaw = game::heading_to_yaw(body.heading);
    out.pitch = body.camera_angle == camera_angle_in ? pitch : body.camera_angle * glm::pi<float>() / 256.0f;
    out.velocity = body.velocity;
    out.moving = body.speed_run != 0.0f || ctl_.strafe_mode() != 0 || body.velocity != glm::vec3(0.0f);
    out.wet = body.body_wet != 0;
    out.flying = body.fly_mode != mech::FlyMode::Ground;
    out.running = conn.running();
    out.speed_run = body.speed_run;
    out.speed_heading = body.speed_heading;
    out.body_wet = body.body_wet;
    out.feet_wet = region_type(regions, body.feet);
    out.head_wet = head_wet;
    out.colliding_type = body.colliding_type;
    if (cp) conn.note_physics_step(cp->id, now);
    if (mount && me) conn.note_physics_step(me->id, now); // rider vf8 step completed
    return out;
}

} // namespace hailstone::app
