#pragma once

// Missiles: arrows, thrown weapons and spell bolts in flight (EQMissile,
// constructor 0x5005b0, ProcessMissile 0x4fff70 and MoveMissile 0x5019c0).
//
// The server sends each one as OP_SomeItemPacketMaybe (0x747c, 116 bytes;
// EQEmu's Mob::ProjectileAnimation / SendItemAnimation). The client never makes
// one of its own: the packet is the constructor's only caller
// (HandleWorldMessage -> CEverQuest_HandleMissilePacket @0x5005b0, the
// EQMissile constructor with the packet).
//
// The constructor copies the packet into the missile (offsets in the packet /
// in EQMissile): position +0x00 / +0x20, velocity +0x0c / +0x2c, speed +0x18 /
// +0x38, heading +0x1c / +0x3c, tilt +0x20 / +0x40 and its rate +0x24 / +0x44,
// the heading's rate +0x28 / +0x48, pitch +0x2c / +0x4c, source id +0x30 /
// +0x108, target id +0x34 / +0x5c, item +0x38 / +0x10c, +0x3c / +0x110, spell
// (or damage) +0x40 / +0x114, light +0x44 / +0x118, +0x45 / +0x11c, the range
// byte +0x49 / +0x58, item type +0x4a / +0x120, skill +0x4b / +0x121, the
// fly-to-a-location flag +0x4c / +0x10, the location +0x4d / +0x14, the model
// name +0x59 / +0x122. EQEmu's RoF2 struct (rof2_structs.h Arrow_Struct) puts
// its skill at +0x49 and the item type at +0x4a, so the client reads EQEmu's
// skill as the range byte (non-zero for every missile EQEmu sends) and its own
// skill byte (+0x4b) as 0.
//
// What happens then, once a frame (CDisplay::RealRender_World's missile loop):
//  - without an actor: EQMissile::ProcessMissile @0x4fff70 waits for the
//    launch (+0x104: the source's attack animation, below) and makes the
//    actor (CDisplay::CreateActor with the model name);
//  - with one, when more than 32 ms passed since the last step (+0x54):
//    EQMissile::MissileAI @0x4ffb10 (homing), the physics step 0x8d2790
//    (PhysicsObject at +4, vtable 0x9cef90) and CDisplay::MoveMissile
//    @0x5019c0 (the actor's motion, the hit and the end).
//
// A physical missile without a target (+0x6c null), and every spell missile, sweeps the zone (MoveMissile's
// s3dCollideActorSweep); a physical one with a target tests only
// its step against the target's Height, so it flies through walls and floors,
// as the original's does. The source and target are spawn pointers there:
// when a spawn goes, PlayerZoneClient::DeleteMyMissiles @0x5a0b60 (from
// ~PlayerClient) deletes the missiles it shot and clears the target of the
// ones aimed at it, which then stop homing and sweep (MissileList::update).
//
// The hit only ends the missile (EQMissile::HitActor @0x4ff9d0 prints a
// message for spell missiles with SPA 0x4c and does nothing else): the damage,
// the impact effect and its sound come from the server's own packets
// (OP_Action / OP_Damage through IWasHit), as for any other hit.
//
// Spell missiles (the client's item type 9): EQEmu never writes the spell
// (+0x40 stays 0), and ProcessMissile ends an item type 9 missile whose spell
// fails IsValidSpellID @0x4ad150 (1..45000) before it makes the actor, so the
// original shows nothing for them. Valid spell ids use their AE duration (or
// 30 s for travel type 3, 50 ms otherwise), start stationary on the target for
// area target types and play the projectile/area stage. Stationary SPA 76 clouds contact moving spawns in range
// every 2 s, and other stationary spells attempt contact on the local player every 3 s with the source's LOS
// (or its NPCNoLoS bypass). The contact itself is presentation only. Spell 859 calls the local IWasHit presentation
// within 40 horizontal units when destroyed. Only spell 54 plays an impact at the missile on destruction. The
// NMS server sends its bolts as item type 20 with the spell's player_1 model:
// an ordinary missile.
//
// Coordinates: the packet's (Y, X, Z) are the world's (x, y, z) (game/coords.h);
// headings and pitches are in 512ths of a turn, heading 0 along +x.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::assets {
struct AnimationClip;
}

namespace hailstone::net {
struct Item;
}

namespace hailstone::game {

struct SpellData;

// OP_SomeItemPacketMaybe as the client reads it (offsets above).
struct MissilePacket {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float speed = 0.0f;
    float heading = 0.0f;
    float tilt = 0.0f;
    float tilt_rate = 0.0f;
    float heading_rate = 0.0f;
    float pitch = 0.0f;
    std::uint32_t source_id = 0;
    std::uint32_t target_id = 0;
    std::uint32_t item_id = 0;
    std::uint32_t item_extra = 0; // +0x3c
    std::uint32_t spell = 0;      // +0x40: the spell (item type 9) or the damage
    std::uint8_t light = 0;
    std::uint8_t range = 0;
    std::uint8_t item_type = 0;
    std::uint8_t skill = 0;
    bool to_location = false;
    glm::vec3 location{0.0f};
    std::string model;
};

// The 116-byte packet; nullopt when shorter or when any of its floats is NaN or infinite (Hailstone's check, not the
// original's). The model name is the NUL-terminated text at +0x59 (27 bytes).
std::optional<MissilePacket> decode_missile_packet(std::span<const std::byte> data);

inline constexpr std::uint8_t kMissileItemSpell = 9;
inline constexpr std::uint8_t kMissileItemThrown = 7; // MissileAI leaves its tilt rate alone

// The projectile describes the ammo. Only the local inventory can supply the bow that shot it.
std::uint32_t missile_bow_model(const MissilePacket& packet, std::uint32_t player_id, const net::Item* range_item);

// The animation the source plays at the launch (Send_OP_Animation_0058f540 with
// no spell -> PlayerZoneClient_PlayAttackAnimation @0x5a19d0 with slot 7:
// mechanics::attack_animation): on old-style models item type 9 -> T06 (0x2c);
// with a range byte arrows (27) C09, 28 C10, others C05; on new-style models
// MCST, BATK, WATK, BLPR. An old-style number below 0x4f, else a new-style
// code; 0xfe and 0x4f play nothing.
std::uint8_t missile_launch_animation(std::uint8_t skill, std::uint8_t item_type, std::uint8_t range,
                                      std::uint16_t source_race, std::uint8_t source_class = 0,
                                      std::uint8_t body_wet = 0, bool new_style = false);
// The launch delay from that animation's length (PlayerClient::CalcAnimLength, ms): with a range byte
// trunc(length x 0.7), or 1500 when that is below 1; without one 0.
std::uint32_t missile_launch_delay(std::uint32_t anim_length_ms, std::uint8_t range);
// The same from the animation clip itself: 70 % of the clip's own period (the .ani's last key time), truncated;
// 1500 without a clip or when that is below 1; 0 without a range byte.
std::uint32_t missile_launch_delay(const assets::AnimationClip* clip, std::uint8_t range);
// 0x58f540's launch delay: 70 % of the animation's length (0x9cf8b8).
inline constexpr float kLaunchFraction = 0.7f;
// 0x5dc: the launch delay when 70 % of the clip's length is below 1 ms.
inline constexpr std::uint32_t kDefaultLaunchMs = 1500;

// CMath vtable +0x18 (0x100b9490) for the arguments MissileAI gives it (a, b >= 0): the angle of (a, b) from
// the b axis in 512ths, floored to the DLL's tangent table; 128 when b is 0.
float cmath_angle512(float a, float b);

// What a missile asks about a spawn (PlayerClient fields).
struct MissileSpawn {
    std::uint32_t id = 0;
    glm::vec3 position{0.0f};  // +0x64..+0x6c
    float avatar_height = 0.0f; // +0x138
    float height = 0.0f;        // +0x13c
    std::uint16_t race = 0;     // vf88
    bool dead = false;          // +0x35c == 0x78
    // The source's physics timestamp (+0x30), on the same clock as update. Absent: no extra extrapolation;
    // an offline world or a presentation layer without that timestamp must not invent its age.
    std::optional<std::uint32_t> stepped_at;
    float speed_run = 0.0f;     // +0x7c: SPA 76's area contact checks moving spawns only
    bool is_npc = false;
    std::uint32_t rider_id = 0;
    bool has_actor = true;
};

struct Missile {
    std::uint32_t serial = 0; // MissileList::add's count (the client's own handle for its actor)
    MissilePacket packet;
    // The physics object (+0x20..+0x4c).
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float speed = 0.0f, heading = 0.0f, tilt = 0.0f, tilt_rate = 0.0f, heading_rate = 0.0f, pitch = 0.0f;
    std::uint32_t target_id = 0; // +0x6c, cleared when the missile turns on its own source or the target goes
    std::uint32_t launch_at = 0; // +0x104
    std::uint32_t ends_at = 0;   // +0xec
    std::uint32_t last_step = 0; // +0x54
    std::uint32_t last_aim = 0;  // +0xf8
    float aim_heading = 0.0f;    // +0xd4
    float aim_pitch = 0.0f;      // +0xd0
    float aim_distance = 0.0f;   // +0xd8
    bool done = false;           // +0x101
    // The actor (+0x2b8): its position as the DLL keeps it, its scale and its collision sphere's radius.
    bool has_actor = false;
    glm::vec3 actor_position{0.0f};
    float actor_scale = 1.0f;
    float radius = 0.0f;
    std::uint32_t next_contact = 0;
    bool area_contact = false; // MoveMissile sets this only for a stationary SPA 76 cloud
};

// The constructor's packet path; `now` is __EQGetTime, `launch_delay` missile_launch_delay's answer (0 without
// the source spawn).
Missile make_missile(const MissilePacket& p, std::uint32_t now, std::uint32_t launch_delay);

// What the missile's update asks the world.
struct MissileWorld {
    std::function<std::optional<MissileSpawn>(std::uint32_t id)> spawn;
    // s3dCollideActorSweep for a target-less missile from `from` to `to` with the actor's sphere: true when
    // the sweep hit something solid (the hit callback ends the missile, and the skitter it leaves deletes it).
    std::function<bool(const glm::vec3& from, const glm::vec3& to, float radius)> sweep;
    // pinstSceneGraph vtable +0x4c: the point is inside the zone.
    std::function<bool(const glm::vec3& p)> inside;
    std::int32_t zone_id = 0;
    std::function<const SpellData*(std::uint32_t id)> spell;
    std::function<std::optional<glm::vec3>(std::uint32_t id)> launch_point;
    // A visual stage with the missile's actual actor/position, and retirement of that actor's attachments.
    std::function<void(const Missile&, int stage)> effect;
    std::function<void(const Missile&)> ended;
    std::function<std::vector<MissileSpawn>()> spawns;
    std::uint32_t local_id = 0;
    std::function<bool(const glm::vec3& from, const glm::vec3& to)> visible;
    // HitActor's contact attempt, not damage or an impact stage. SPA 76's moving-area contacts print text only.
    std::function<void(const Missile&, const MissileSpawn& target)> contact;
    std::function<void(const Missile&)> spell_hit; // spell 859's nearby local IWasHit, no network request
};

// CDisplay::CreateActor's scale for an IT model (1.5 for IT66 / IT69 / IT73 and BBBOARD, 1.3 for IT74; in
// zones 0x13, 0x37, 0x3c and 0x43 0.8 for IT70 / IT74 instead).
float missile_actor_scale(const std::string& model, std::int32_t zone_id);

// EQMissile::ProcessMissile: the launch; invalid/absent spells and missing sources end before making an actor.
void process_missile(Missile& m, std::uint32_t now, const MissileWorld& world);
// The extra velocity steps a spell missile's launch takes for its source's time since its last physics step
// (ProcessMissile: (now - stepped_at) x 0.02), none for a time not before now, the age capped at 12 s.
float spell_launch_steps(std::optional<std::uint32_t> stepped_at, std::uint32_t now);
// EQMissile::MissileAI.
void missile_ai(Missile& m, std::uint32_t now, const MissileWorld& world);
// 0x8d2790: the physics step.
void missile_physics(Missile& m, std::uint32_t now);
// CDisplay::MoveMissile: false when the missile ends.
bool move_missile(Missile& m, std::uint32_t now, const MissileWorld& world);

// The missiles in flight (the list at 0xdd2608) and RealRender_World's loop over them.
class MissileList {
public:
    void add(Missile m) {
        m.serial = ++serial_;
        missiles_.push_back(std::move(m));
    }
    void clear() { missiles_.clear(); } // PlayerZoneClient::DeleteMyMissiles / ClearZoneWorldState
    void update(std::uint32_t now, const MissileWorld& world);
    const std::vector<Missile>& missiles() const { return missiles_; }

private:
    std::vector<Missile> missiles_;
    std::uint32_t serial_ = 0;
};

} // namespace hailstone::game
