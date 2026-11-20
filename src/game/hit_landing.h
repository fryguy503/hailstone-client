#pragma once

// A spell landing on the player: OP_Action type 231 with effect flag 4 goes
// through PlayerZoneClient::IWasHit @0x5a07b0 to EQ_Character::HitBySpell
// (game/mechanics/hit_by_spell.h). This runs those rules with the client's state
// behind its callees and reports what the client applies itself.
//
// What the callees answer here (docs/unknowns.md, Spells):
//  - FindAffectSlot (@0x43d300, game/mechanics/find_affect_slot.h) picks the
//    buff's slot with the real stacking rules and loop 2 runs; the buff it
//    writes is reported (placed_buffs) and the buffs it overwrites are
//    removed, as the original does before the server's OP_Buff arrives. The
//    character's extra buff slots (vtable +0x2c) answer 0 and the spawn's vfdc
//    "no buff may be dropped" answers no;
//  - CalcAffectChange is CalcAffectChangeGeneric with the effect's base,
//    SpellDuration / PvPSpellDuration the ported SpellDuration;
//  - the stun check (sub_00433830), the spawn's vfdc, the shadow
//    step's collision and the gem-timer focus answer "no"; DoSpellCheck "ok";
//  - gate / teleport / bind and the target packet are left to the server
//    (MoveToZone, the bind setters and the OP 0x56b1 send do nothing).
// Applied: the Teleport sound (0x89) of SPA 83 / 145 with a zone (player spawns
// only) and of a valid SPA 379 (whose IsValidTeleport answers "no" here),
// the messages (immunity 0x30a0, bind refusals, books, identify),
// buffs removed (the invisibility removal), drunkenness, the push, the
// stop-turning of SPA 56 and the feign death of SPA 74 (reported; the
// connection stops the cast and lies the player down). The one change made in
// place: the counters a cure spends are written back into the SpellState's buffs.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "game/mechanics/hit_by_spell.h"
#include "game/spells.h"

namespace hailstone::game {

class EqStrings;
struct WorldState;
namespace mechanics {
class Character;
}

struct HitLandingContext {
    const SpellTable* spells = nullptr;
    const WorldState* world = nullptr;
    SpellState* spell_state = nullptr;
    const mechanics::Character* character = nullptr; // the stats (may be null)
    const EqStrings* strings = nullptr;
    std::uint8_t class_id = 0;
    std::uint8_t level = 1;
    std::int32_t mana = 0, weight = 0;
    std::int32_t drunkenness = 0;
    glm::vec3 velocity{0.0f}; // the player's, units per tick (world axes)
    std::uint32_t now_ms = 0;
    std::int32_t pvp_rules = 0;  // 0xdcee70 (OP_LogServer +4): the group / bind rules of HitBySpell
    std::uint8_t zone_type = 0;  // __ZoneType (NewZone time_type)
    // Bits of counter SPAs (35/36/116/369) EQEmu faded before this OP_Action: skip the local cure prediction for
    // those types, the server has already applied the cure (game/counter_spas.h).
    std::uint8_t faded_counter_spas = 0;
};

// A buff HitBySpell wrote into a slot (FindAffectSlot's choice).
struct PlacedBuff {
    int slot = -1;
    std::uint32_t spell = 0, caster_id = 0;
    std::int32_t tics = 0;
    std::uint8_t level = 0, effect_type = 2;
    float modifier = 1.0f;
    std::array<std::int32_t, 12> slot_data{};
    bool counters_known = false;
};

struct HitLandingResult {
    std::uint8_t value = 0;            // HitBySpell's answer
    glm::vec3 velocity_change{0.0f};   // the push (world axes, units per tick)
    bool heading_changed = false;      // SPA 56 sets the heading (and its speed) to 0
    float heading = 0.0f;
    std::int32_t drunkenness = 0;      // after
    std::vector<std::string> chat;     // in order
    std::vector<std::uint32_t> chat_colors; // each line's dsp_chat colour (0 none given), as chat
    std::vector<int> removed_buffs;    // buff slots RemoveAffect cleared
    std::vector<PlacedBuff> placed_buffs; // the buff HitBySpell placed (after the removals)
    std::vector<std::int32_t> sounds;  // PlayerClient::PlaySound(wave) at the player, in order
    bool buff_timers_on_hold = false;  // SPA 0x1b4 landed: 0x4325e0(1), the spawn's +0x22d
    bool feigned = false;              // SPA 74 landed: StopCastAndFeignDeath (the twist, the cast, lie down)
    // PlayerClient_RecordPositionSample(kind, 0, 0) on the player, in order (3 teleport, 7 knock-back, 9 spell move).
    std::vector<std::uint32_t> position_samples;
};

// The OP_Action fields HitBySpell reads (+2, +4, +0xa, +0x12, +0x1b, +0x1d, +0x21, +0x25).
mechanics::hit::Hit hit_record(std::uint16_t source, std::uint16_t level, float modifier, float heading,
                               std::uint32_t damage, std::uint16_t unknown31, std::uint32_t spell_id,
                               std::uint8_t spell_level);

HitLandingResult land_spell(const mechanics::hit::Hit& hit, const HitLandingContext& ctx);

} // namespace hailstone::game
