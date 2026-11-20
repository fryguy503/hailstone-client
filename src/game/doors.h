#pragma once

// Doors, lifts, levers and other "switches" (the original's EQSwitch), and
// ground objects (EQGroundItem: dropped items, ground spawns, tradeskill
// containers). Independent of the network and the renderer: GameConnection
// fills them from OP_SpawnDoor / OP_MoveDoor / OP_GroundSpawn, the app draws
// and collides with them.
//
// The rules are the original client's (EQSwitch and CDisplay):
//   EQSwitch::LoadSwitchSounds @0x511990   the constructor from an OP_SpawnDoor record
//   EQSwitch::PostInit @0x510960           type conversions, scale and remain-open fixes
//   EQSwitch::ResetSwitchState @0x510b50   the state a spawned door starts in
//   EQSwitch::ChangeState @0x50f540
//   EQSwitch::RepopFrequency @0x50f370     how long until a moved door returns
//   EQSwitch::RepopSwitch @0x512690
//   EQSwitch::TopSpeed @0x50f2c0, GetCustomMoveDistance @0x50ff20
//   EQSwitch_IsFloorSwitch @0x50ff80
//   CDisplay_ProcessSwitches @0x48d530     the per-frame animation
//   CDisplay::SlideSwitchLeftRight @0x4880a0
//   EQSwitch::UseSwitch @0x511d70          the click range check
//
// Field names and units are the original's: Y, X, Z as the EQ client stores
// them (Y first: the server's y, which is world x; see game/coords.h),
// headings and door angles in 512ths of a turn, times in milliseconds.

#include <cstdint>
#include <functional>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "game/mechanics/switches.h"

namespace hailstone::game {

// EQSwitch::State values.
inline constexpr std::uint8_t kSwitchClosed = 0;
inline constexpr std::uint8_t kSwitchOpen = 1;
inline constexpr std::uint8_t kSwitchOpening = 2;
inline constexpr std::uint8_t kSwitchClosing = 3;
inline constexpr std::uint8_t kSwitchTriggered = 4; // a trap that went off; resets after 60 s

// What an OP_SpawnDoor record carries (net/door_packets.h DoorRecord, in game terms).
struct DoorSpawn {
    std::string name;
    float y = 0, x = 0, z = 0, heading = 0;
    std::int32_t incline = 0;
    std::uint16_t size = 100;
    std::uint8_t id = 0, type = 0, state = 0, default_state = 0;
    std::int32_t param = -1;
    bool usable = true, remain_open = false, visible = true, heading_changed = false;
    float top_speed1 = 0, top_speed2 = 0;
    bool stamp_time = false;
    std::int32_t adventure_id = 0; // the record's +68 (EQEmu sends 0)
    std::uint32_t dyn_door_id = 0; // +72
    std::uint8_t has_script = 0;   // +80
};

struct Door {
    std::uint8_t id = 0;              // +0x05
    std::string name;                 // +0x06; the model is "<name>_ACTORDEF"
    std::uint8_t type = 0;            // +0x26 (EQEmu "open type")
    std::uint8_t state = kSwitchClosed;    // +0x27
    float default_y = 0, default_x = 0, default_z = 0, default_heading = 0, default_door_angle = 0; // +0x28..+0x38
    float top_speed1 = 0, top_speed2 = 0, accel = 0; // +0x3c, +0x40, +0xb0
    float y = 0, x = 0, z = 0, heading = 0, door_angle = 0; // +0x44..+0x54
    std::uint8_t default_state = kSwitchClosed; // +0x58 (EQEmu invert_state)
    std::int32_t key = -1;            // +0x70 (not in the RoF2 record)
    std::int16_t scale_factor = 100;  // +0x74, percent
    std::int32_t param = -1;          // +0x78 "SpellID": travel distance and more
    std::uint32_t timestamp = 0;      // +0xac, ms of the last state change
    std::int32_t adventure_id = 0;    // +0xb8 (record +0x44); TouchingSwitch's type 0x39 test
    std::uint32_t dyn_door_id = 0;    // +0xc8 (record +0x48)
    std::uint8_t has_script = 0;      // +0xcc (record +0x50)
    bool usable = true;               // +0xd4
    bool remain_open = false;         // +0xd5
    bool visible = true;              // +0xd6
    bool heading_changed = false;     // +0xd7
    bool particle = false;            // +0xa8: a running type 0x35 particle effect
    std::vector<std::uint8_t> sound_states; // states entered since the sounds were last taken (ChangeState plays them)

    // World position of the actor (Y, X, Z -> world x, y, z; game/coords.h).
    glm::vec3 world_position() const { return {y, x, z}; }
    float scale() const { return static_cast<float>(scale_factor) * 0.01f; }
};

// __FixHeading @0x8d1f50: into 0..512.
float fix_heading(float h);
// __HeadingDiff @0x8d1fc0: the shorter way between two headings, 0..256.
float heading_diff(float a, float b);

// The constructor from a spawn record, PostInit included; `now_ms` is __EQGetTime.
Door make_door(const DoorSpawn& spawn, std::uint32_t now_ms);

// EQSwitch::ChangeState (sounds aside): returns true when the state changed.
bool change_state(Door& door, std::uint8_t state, std::uint32_t now_ms);
std::uint32_t repop_frequency(const Door& door);
float custom_move_distance(const Door& door);
bool is_floor_switch(const Door& door, std::int32_t zone_id);
// Switch types that never get an actor (0x35: a particle emitter) or whose
// actor is made invisible (PostInit: 0x36, 0x32, 0x39 with a door angle of 1).
bool door_has_actor(const Door& door);
bool door_is_invisible(const Door& door);
// Switch types EQEmu uses for zone destinations.
constexpr bool door_is_portal(std::uint8_t type) { return type == 57 || type == 58; }
// Whether the movement sweep and the floor test collide with the door's
// actor. The sweep's filter (CollisionFilter_Movement @0x4760c0) skips switch
// types 0x32 and 0x33 and nothing else; neither the DLL's actor loop (only
// IsDisabled, actor vf4) nor the filters ask IsInvisible (actor vf38, the
// flag PostInit's vf34(1) sets), so invisible switches (0x36, 0x39 with a
// door angle of 1) block like visible ones. Only the line-segment queries
// (picking, line of sight: the DLL's filter 0x100080a0) skip invisible actors.
bool door_blocks_movement(const Door& door);
// UseSwitch's early outs: types 0x36..0x38, unusable or invisible switches,
// and switches still moving (the trap and 0x9d exceptions aside).
bool door_can_be_used(const Door& door);
// CDisplay_UpdateDayNightSwitch @0x488030: type 0x35's incline is 0 (keep the packet state), 1 (day) or 2
// (night, or the display's rain-screen override). `hour` is the native/server 1..24 clock, not zero-based.
void update_particle_switch_day_night(Door& door, int hour, bool rain_screen = false);

// One frame of CDisplay_ProcessSwitches for every door. `ticks` are 1/50 s
// since the last call (the original: elapsed ms * 0.02, and nothing moves on
// a frame where that is above 15). Returns the doors RepopSwitch "uses"
// again (types 0x28 and 0x2d: the original sends OP_ClickDoor for them).
struct SwitchClock {
    std::uint32_t last_ms = 0;
    bool started = false;
    // The frame's working lists, kept between frames so a frame allocates nothing while the doors stay the same.
    struct Scratch {
        std::vector<Door*> doors;
        std::vector<mechanics::Switch> switches;
        std::vector<mechanics::Switch*> list;
    } scratch;
};
// The spawns ProcessSwitches looks at for a moving floor switch's riders: a
// rider's floor cache is reset (CollisionCounter = 0) so its next floor test
// finds the switch's new top; nothing carries it.
struct DoorRiders {
    std::span<const mechanics::SwitchSpawn> spawns;
    std::function<void(std::size_t)> reset_collision_counter;
    std::function<double(const Door&)> actor_radius; // the door actor's vf44
};
// Type 0x35 switches have no model: the app owns their particle effects. A successful start persists in
// Door::particle until the switch closes; a replacement spawn record starts without a running effect.
struct DoorParticles {
    std::function<bool()> local_player;
    std::function<void(Door&)> update_day_night;
    std::function<bool(Door&, std::int32_t)> start;
    std::function<void(Door&)> stop;
};
// The frame is game/mechanics/switches.h's exact ProcessSwitches; `zone` is
// for EQSwitch_IsFloorSwitch.
std::vector<std::uint8_t> process_doors(std::vector<Door>& doors, SwitchClock& clock, std::uint32_t now_ms,
                                        std::int32_t zone = 0, const DoorRiders* riders = nullptr,
                                        const DoorParticles* particles = nullptr);
// The same for one door with an explicit tick count (tests).
std::vector<std::uint8_t> process_door(Door& door, float ticks, std::uint32_t now_ms);

// The door actor's transform: translate to (Y, X, Z), turn by Heading about
// +Z (counterclockwise, as spawn headings), tilt by DoorAngle about the
// actor's Y axis (the second ActorInstance angle, negated as in
// assets/wld_zone.cpp placement_transform), scale by ScaleFactor %.
glm::mat4 door_transform(const Door& door);

// UseSwitch measures to the mouse hit, or to the switch origin with the actor radius added to reach.
bool door_within_reach(const Door& door, const glm::vec3& player_world, float player_height, float actor_radius,
                      std::optional<glm::vec3> hit = std::nullopt);

// A ground object (OP_GroundSpawn).
struct GroundObject {
    std::uint32_t drop_id = 0;
    std::string name; // actor name, e.g. "IT63_ACTORDEF"
    glm::vec3 position{0.0f}; // world coordinates
    float heading = 0;        // 0..512
    float tilt_x = 0, tilt_y = 0;
    float size = 1;
    std::int32_t type = 0;    // EQEmu object type: 1 = temporary (an item to pick up)
};
glm::mat4 ground_object_transform(const GroundObject& o);
// The actor definition CDisplay::CreateActor @0x48b710 looks up for `name`: a name shorter than 10 characters
// (other than PLAYER_1) gets "_ACTORDEF" ("IT63" -> "IT63_ACTORDEF"); a longer one is looked up as it is.
std::string create_actor_definition(std::string_view name);
// CreateActor's fallbacks when that definition is missing and its first flag is set (the OP_GroundSpawn handler
// passes it, EQSwitch::PostInit does not): IT63, then RED_BLOB, else no actor.
inline constexpr const char* kCreateActorFallbacks[] = {"IT63_ACTORDEF", "RED_BLOB_ACTORDEF"};
// LMouseUp picks a ground item up when the player is under 20 units away
// (CDisplay::TrueDistance with z).
inline constexpr float kGroundItemReach = 20.0f;

// An open tradeskill container (OP_ClickObjectAction); its items are in the inventory's World slots 0..9.
inline constexpr int kWorldContainerSlots = 10;
struct WorldContainer {
    std::uint32_t drop_id = 0;
    std::uint32_t type = 0;
    std::uint32_t icon = 0;
    std::string name;
    // Shown in its own container window: not when it opened in the tradeskill window (CContainerMgr::OpenContainer),
    // until Experiment hands it over (CContainerMgr::OpenExperimentContainer). Closing the tradeskill window only
    // hides that window (CTradeSkillWnd::WndNotification's close button), so the container gets no window of its own.
    bool window = false;
};

} // namespace hailstone::game
