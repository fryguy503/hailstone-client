#pragma once

// Skills and combat abilities as the original client uses them: the skill
// table's timers (CSkillMgr), the reuse rules (GetSkillReuseTime, haste
// through ModifyAttackSpeed), the per-frame timer refresh (DoPassageOfTime),
// the melee checks (CanIHit, __get_melee_range, CanUseCombatSkill, DoAttack's
// combat-skill path) and CharacterZoneClient::UseSkill @0x45b2d0 itself, which
// decides locally whether a skill may be used, prints the original's messages
// and names the packet to send. The server decides the outcome.
//
// Checked against the original's UseSkill
// @0x45b2d0, PlayerZoneClient::DoAttack @0x5a5000, CanIHit @0x5a2940,
// CanUseCombatSkill @0x5a2f10, CanMeleeAttackNow @0x5a0600, __get_melee_range
// @0x4ddeb0, __get_bearing @0x4ef250, the CSkillMgr accessors @0x5ba1b0..
// @0x5bae40, CharacterZoneClient_GetSkillReuseTime @0x4427f0,
// CharacterZoneClient::DoPassageOfTime @0x45daf0's timer loops) and
// PlayerZoneClient::ModifyAttackSpeed @0x5a2c20. UseSkill itself is the vector-checked port in
// game/mechanics/use_skill.h; use_skill() below answers its callees from a
// SkillContext. Gaps: docs/unknowns.md "Abilities".

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "game/mechanics/melee.h"

namespace hailstone::game { class EqStrings; }

namespace hailstone::game::mechanics {

class Character;

// EQEmu skill ids (refs/eqemu common/skills.h), the original's skill indexes.
namespace skill {
inline constexpr std::uint32_t kApplyPoison = 6, kBackstab = 8, kBindWound = 9, kBash = 10, kDisarm = 16,
                               kDisarmTraps = 17, kDragonPunch = 21, kEagleStrike = 23, kFeignDeath = 25,
                               kFlyingKick = 26, kForage = 27, kHide = 29, kKick = 30, kMend = 32, kPickLock = 35,
                               kRoundKick = 38, kSafeFall = 39, kSenseHeading = 40, kSneak = 42, kPickPockets = 48,
                               kTigerClaw = 52, kTracking = 53, kFishing = 55, kSenseTraps = 62, kBegging = 67,
                               kIntimidation = 71, kTaunt = 73, kFrenzy = 74, kRemoveTraps = 75, kSlam = 111;
}

// The skill table's timer fields (CSkillMgr_InitSkillTable @0x4a7930; skill_timers.inc).
struct SkillTableEntry {
    std::int32_t reuse_ms = 0;    // ReuseTimer (+0xc): CSkillMgr::GetReuseTime @0x5bab70
    std::int32_t base_damage = 0; // BaseDamage (+0x10): CSkillMgr::GetBaseDamage @0x5bab40
    std::int32_t combat_type = 0; // SkillCombatType (+0x14): 2 = a combat skill (DoAttack slot 100)
};
// The entry of skill 0..99; zeros for other ids.
const SkillTableEntry& skill_table_entry(std::uint32_t skill);
inline std::int32_t skill_combat_type(std::uint32_t skill) { return skill_table_entry(skill).combat_type; }

// PlayerZoneClient::ModifyAttackSpeed @0x5a2c20 with no bow flag: delay / (haste * 0.01) truncated, with the
// haste total TotalEffect(11); 0 stays 0. (The bow branch's quiver speedup is not needed here.)
std::uint32_t modify_attack_speed(const Character& ch, std::uint32_t delay);
// CharacterZoneClient_GetSkillReuseTime @0x4427f0: GetReuseTime(skill) - 1000 * TotalEffect(227, sub = skill).
std::int32_t skill_reuse_time(const Character& ch, std::uint32_t skill);
// GetCombatSkillTimerIndex @0x5ba2b0: Tiger Claw has the second combat timer.
inline int combat_timer_index(std::uint32_t skill) { return skill == skill::kTigerClaw ? 1 : 0; }

// The skill timers of CSkillMgr (+0x2c6514 last used, +0x2c66a4 durations, +0x2c6844 can-use flags, and the two
// combat-skill timers +0x2c6834 / +0x2c683c / +0x2c68a8) with the player's own combat-skill clock
// (PlayerZoneClient +0x368 CombatSkillTicks[2], +0x390 CombatSkillUsed[2]). Times are EQGetTime milliseconds.
class SkillTimers {
public:
    SkillTimers();

    // The CSkillMgr accessors; combat skills (type 2, and 111) use the combat arrays.
    std::uint32_t last_used(std::uint32_t skill) const;                         // GetSkillLastUsed @0x5baca0
    std::uint32_t raw_duration(std::uint32_t skill) const;                      // the stored duration
    std::uint32_t timer_duration(const Character& ch, std::uint32_t skill) const; // GetSkillTimerDuration @0x5bad40
    bool is_available(std::uint32_t skill) const;                               // IsAvailable @0x5bae40
    void set_last_used(std::uint32_t skill, std::uint32_t when);                // CSkillMgr_SetSkillLastUsed @0x5bac50
    void set_timer_duration(std::uint32_t skill, std::uint32_t ms);             // @0x5bacf0
    void set_can_use(std::uint32_t skill, bool can);                            // CSkillMgr_SetSkillCanUse @0x5bada0

    // DoPassageOfTime @0x45daf0's two loops (@0x45e650.., @0x45e9a0..): skills 0..73 whose time is up become
    // usable again (not sneak while sneaking, nor hide while hidden); then the combat timers.
    void update(const Character& ch, std::uint32_t now, bool sneaking, bool hidden);
    // Milliseconds until the skill's timer runs out (0 when available; for the windows).
    std::uint32_t remaining(const Character& ch, std::uint32_t skill, std::uint32_t now) const;

    std::array<std::uint32_t, 2> combat_ticks{}; // PlayerZoneClient CombatSkillTicks: the reuse DoAttack set
    std::array<std::uint32_t, 2> combat_used{};  // PlayerZoneClient CombatSkillUsed: when it was used

private:
    bool uses_combat_timer(std::uint32_t skill) const;
    std::array<std::uint32_t, 100> last_{}, duration_{};
    std::array<bool, 100> can_use_{};
    std::array<std::uint32_t, 2> combat_last_{}, combat_duration_{};
    std::array<bool, 2> combat_ready_{};
};

// A spawn as the melee and skill checks read it (PlayerBase / PlayerZoneClient fields).
struct SkillActor {
    std::uint32_t id = 0;            // SpawnID +0x148
    std::uint8_t type = 0;           // Type +0x125: 0 PC, 1 NPC, 2+ corpse
    float x = 0, y = 0, z = 0;       // server coordinates: X +0x68, Y +0x64, Z +0x6c
    float heading = 0;               // Heading 0..512
    float avatar_height = 3.125f;    // AvatarHeight +0x138
    float melee_radius = 0.0f;       // MeleeRadius +0x105c (the spawn packet's)
    float scale = 1.0f;              // +0x31c, SetHeights' model scale
    std::uint8_t body_wet = 0;       // BodyWet +0xa2 (the region type at the spawn: 0 dry, 5..10 liquids)
    std::uint8_t level = 1;
    bool pvp_flag = false;           // +0x349
    bool mercenary = false;
    bool same_deity_ok = true;       // NotOnSameDeity (PvP ruleset 4)
    bool is_trap_or_box = false;     // vtable +0x98 (LDoN traps and chests: pick lock, disarm traps)
    bool is_pet = false;
    bool gm = false;                 // vtable +0xdc (GM +0x25c): pick pockets leaves GMs alone
    std::string name;                // display name (trimName)
};

// The mutable state UseSkill and DoAttack keep between calls.
struct SkillState {
    SkillTimers timers;
    bool sneaking = false;           // PlayerZoneClient::Sneak +0x264 (set from the server's SpawnAppearance 15)
    bool hidden = false;             // PcBase hide flag (HideMode +0x3b8)
    std::int32_t activity = -1;      // 0xddfccc: the skill in progress (hide 29, fishing 55, tracking 53)
    std::int32_t hide_x = 0, hide_y = 0, hide_z = 0; // 0xddf8d8..: where hiding started (truncated)
    std::uint32_t last_intimidate = 0; // PlayerZoneClient LastIntimidateUse +0x188 (GetTickCount)
    bool fishing = false;            // FishingEvent
    std::uint32_t fishing_eta = 0;
    std::int32_t next_intimidate_time = 0; // PlayerZoneClient +0x3c8 (UseSkill zeroes it on a refusal)
    std::uint8_t intimidate_count = 0;     // +0x434 (6 after instill doubt went out)
    // DoAttack's timers and the two swing-ready flags (game/mechanics/melee.h); its combat-skill arrays and
    // LastIntimidateUse are copies of timers.combat_ticks / combat_used and last_intimidate.
    melee::AttackState attack;
};

// What UseSkill sees of the player and the world this frame.
struct SkillContext {
    const Character* ch = nullptr;
    SkillActor me;
    std::optional<SkillActor> target; // pinstTarget
    bool target_is_me = false;
    std::uint8_t stand_state = 100;   // 100 stand, 102 frozen, 110 sit, 111 crouch, 115 feigned
    std::int32_t cur_hp = 1;
    std::uint8_t char_state = 0;      // the character state byte (1 stunned; 2..4 other no-melee states)
    bool casting = false;             // CastingData.SpellID != -1
    bool is_bard = false;
    bool mounted = false;             // GetMount or a SPA 113 buff
    bool attacking = false, autofire = false; // __Attack, __Autofire
    float speed_x = 0, speed_y = 0, speed_z = 0; // SpeedX/Y/Z (units per physics tick)
    bool cursor_has_item = false;     // the fishing check's slot 0x21
    std::int32_t form_slam = 0xff; // current appearance's innate Slam slot (0xff = unavailable)
    std::uint32_t form_race = 0;   // the player spawn's race now (GetRace, vtable +0x88: +0xeb4); 0 = the profile's
    std::uint8_t pvp_ruleset = 0;     // 0xdcee70
    bool can_see_target = true;       // PlayerBase::CanSee (the zone's line of sight)
    bool tracking_window_open = false;
    bool compass_window = true;       // pinstCCompassWnd (made with the game's windows, so always there in game)
    bool gm = false;                  // 0xddf654: CanIHit failures are ignored
    // PlayerZoneClient::AllowedToAttack(target, -1) for CanIHit (game/attack_check.h); unset: the old
    // approximation (NPCs always, players both flagged within 4 levels at 6+).
    std::optional<bool> allowed_to_attack;
    bool attack_locked = false;       // 0xddf70c: DoAttack refuses the local player
    // A server extension's (game/server_ext.h backstab_either_hand): Backstab also takes a piercer in the secondary.
    bool backstab_either_hand = false;
    // GetPCSpellAffect: the buff slots holding an effect of this SPA, in slot order.
    std::function<std::vector<int>(std::int32_t spa)> buff_slots_with_spa;
    std::uint32_t now = 0;            // __EQGetTime / CDisplay TimeStamp, ms
    std::uint32_t tick_count = 0;     // GetTickCount, ms
    std::function<std::int32_t()> random;                                  // Random()
    std::function<std::int32_t(std::int32_t, std::int32_t)> random_range;  // RandomNumberLowHigh(lo, hi)
};

// A chat line: an eqstr id with %1.. arguments, in a chat colour (0x10e skills, 0xd, 0x12f, 0x111).
// An argument is literal text (a spawn's name stays as it is, whatever it starts with) unless arg_eqstr gives it an
// id: the original copies getString(id) into that argument, so it reads as that eqstr's text.
struct SkillMessage {
    std::uint32_t eqstr = 0;
    std::vector<std::string> args;
    std::uint32_t color = 0x10e;
    std::vector<std::optional<std::uint32_t>> arg_eqstr; // per argument; shorter than args when the rest are text
};

// The message's text from the string tables, with each eqstr argument looked up.
std::string format_skill_message(const EqStrings& strings, const SkillMessage& m);

// A packet UseSkill sends (the bytes are in net/ability_packets.h).
struct SkillSend {
    enum class Kind : std::uint8_t {
        CombatAbility,   // OP_CombatAbility {target, atk = 100, skill}
        RangedAttack,    // OP_CombatAbility {target, atk = 11, skill} (DoRangedAttack)
        BindWound,       // OP_Bind_Wound {target}
        Unhide,          // OP_SpawnAppearance {me, 3 (invisible), 0}
        StopSneak,       // OP_SpawnAppearance {me, 15 (sneak), 0}
        Disarm,          // OP_Disarm {me, target, skill value, 0xff}
        DisarmTraps, PickLock, SenseTraps, Forage, Mend, SenseHeading, TrackUnknown, Fishing, Hide, Sneak,
        InstillDoubt,    // empty packets
        FeignDeath,      // OP_FeignDeath {me, 0xddf708, 0}
        PickPocket,      // OP_PickPocket {target, me, skill value, 0xff}
        Begging,         // OP_Begging {target, me, skill value, 0xff}
        Taunt,           // OP_Taunt {target}
        RemoveTrap,      // OP_RemoveTrap {target}
        FishAnimation,   // OP_Animation {me, action 5, speed 5}
        LDoNInspect, LDoNOpen, // Send_OP_LDoNInspect / Send_OP_LDoNOpen with their checks (net::GameConnection)
        Reveal,                // Cmd_reveal(0, 0) with its checks
    };
    Kind kind{};
    std::uint32_t target = 0;
    std::uint32_t skill = 0;
    std::uint32_t value = 0; // the skill value (profile skills) for Disarm / PickPocket / Begging
    std::uint32_t source = 0; // the player's spawn id (Disarm / PickPocket / Begging)
    // The bytes UseSkill built itself (game/mechanics/use_skill.h): the wire opcode and the payload after it;
    // `kind` then only names the packet.
    bool raw = false;
    std::uint16_t opcode = 0;
    std::vector<std::uint8_t> payload;
};

struct SkillResult {
    std::vector<SkillMessage> messages;
    std::vector<SkillSend> sends;
    std::optional<std::uint8_t> stand_state; // unk_SetStandState: 105 (bind wound kneel), 115 (feign death)
    bool stop_attack = false;                // SetAutoAttack(0) (feign death)
    bool stop_auto_fire = false;             // SetAutoRangeAttack(0) (feign death; DoAttack(11) with auto fire on)
    bool open_tracking_window = false;
    bool track_request = false;              // Send_OP_Track (the tracking window was already open)
    bool compass_reset = false;              // 0x65b650: the compass's error back to 0 (game/compass.h)
    bool visibility_changed = false;         // EQ_Character::UpdateMyVisibleStatus
    bool used = false;                       // a timer started or a request went out
    std::vector<int> removed_buffs;          // RemovePCAffectex(affect, 0): buff slots to clear locally
    // DoAttack's melee swings (SendCombatAbilityAttack for slot 13 / 14: no packet; the animation it computes is
    // dropped, the server's OP_Animation plays it).
    struct Swing {
        std::uint8_t slot = 0;      // 13 primary, 14 secondary
        std::uint8_t item_type = 0; // the weapon's (0x16 hand to hand)
        bool fist = false;          // the fist pseudo-item
    };
    std::vector<Swing> swings;
    // PlayerClient::SetHeldModel(7 / 8, model, 0, 1) from DoAttack's range and melee preambles and DoRangedAttack, in
    // their order: the held slot, the possessions slot whose item's model it shows (-1: "0", an empty hand) and how
    // many of `sends` went out before it. The caller shows it and sends OP_WearChange (net::GameConnection).
    struct HeldModel {
        std::uint8_t slot = 0;
        std::int32_t item_slot = -1;
        std::size_t after_sends = 0;
    };
    std::vector<HeldModel> held_models;
};

// __get_bearing @0x4ef250: the heading (0..511.5) from (x1, y1) toward (x2, y2), in the original's argument order
// (the first pair is PlayerBase Y, X of the viewer, the second the target's).
double bearing(double y1, double x1, double y2, double x2);
// __get_melee_range @0x4ddeb0.
float melee_range(const SkillActor* a, const SkillActor* b);
// PlayerZoneClient::CanIHit @0x5a2940 (the caller passes the player's SPA 40 total).
bool can_i_hit(const SkillContext& ctx, float extra_range, SkillResult& out);
// PlayerZoneClient::CanUseCombatSkill @0x5a2f10 for slot 100. `slam`: the attack is Slam's (UseSkill's skill 111),
// whose Bash needs no shield (a deliberate change, docs/mechanics.md "Slam").
bool can_use_combat_skill(const SkillContext& ctx, std::uint32_t skill, bool slam = false);
// PlayerZoneClient::DoAttack @0x5a5000 for the local player (game/mechanics/melee.h) on this context:
// slots 13 / 14 (auto attack; the swings go to SkillResult::swings) and 100 (a combat skill: OP_CombatAbility).
// `skill` is the argument as passed (DoAttack compares its low byte). Returns the original's al.
// `slam` as for can_use_combat_skill.
bool do_attack(const SkillContext& ctx, SkillState& state, std::uint8_t slot, std::uint32_t skill, SkillResult& out,
               bool slam = false);
// DoAttack(100, skill, target).
bool do_attack_skill(const SkillContext& ctx, SkillState& state, std::uint32_t skill, SkillResult& out);
// DoPassageOfTime @0x45daf0's swing timers on this context, in its order: the attack-on reset (@0x45db76,
// melee::attack_on_reset), then (after its stand check) the two ready flags in state.attack at ctx.now
// (melee::swing_ready).
void attack_on_reset(const SkillContext& ctx, SkillState& state, bool attack_on);
void swing_ready(const SkillContext& ctx, SkillState& state);
// CharacterZoneClient::UseSkill @0x45b2d0: the original's rules (game/mechanics/use_skill.h) run on this context, its
// callees answered from it (DoAttack(100, ...) is do_attack_skill, CanIHit can_i_hit, the timers SkillTimers) and
// its messages, packets and state changes collected in the result.
SkillResult use_skill(const SkillContext& ctx, SkillState& state, std::uint32_t skill);
// DoPassageOfTime @0x45daf0's hide check: moving while hidden (or while hide is in progress) ends it, except for a
// sneaking rogue (class 9). Returns the message when it ended.
std::optional<SkillMessage> check_hide_moved(const SkillContext& ctx, SkillState& state, std::uint8_t class_id);

// CharacterZoneClient::HasSkill @0x44a1b0's value (GetAdjustedSkill). With a class mask and skill rules on the sheet
// (CharacterSheet::skill_rules from the server extension), those rules first; the NMS plugin's are the NMS patch's
// HasSkill detour (the NMS plugin's nms_rules.h, in its own repository): a profile skill of -1 and Tracking without
// Ranger, Druid or Bard give 0; otherwise GetAdjustedSkill runs with the class it picks for the skill (Rogue for
// Backstab, ...), else the profile's (Bard). GetAdjustedSkill itself is not hooked.
std::int32_t adjusted_skill(const Character& ch, std::uint32_t skill);

// The Actions window's lists: skills the player has (CharacterZoneClient::HasSkill @0x44a1b0: adjusted skill
// above 0) that the original activates through UseSkill; combat = SkillCombatType 1 or 2.
// current_race is the player spawn's current appearance race (which can change with illusions).
// Zero falls back to the original profile race for callers without live spawn information.
std::vector<std::uint32_t> usable_skills(const Character& ch, bool combat, std::uint32_t current_race = 0);

// EQ_Character::InitInnates @0x44d4d0: the profile's 25 innate ability slots (+0x3230, 4 bytes each; 0 = has it,
// 0xff = not) for a race and class. The client fills them itself (EQEmu sends them all 0xff): when the local player's
// actor is made (and on an illusion, EQPlayer::do_change_form, with the actor's current race) and when a buff with
// SPA 65 or 66 fades. Both callers keep slot 10 (+0x3258, regeneration) as it was. The race's GetRawSkillCap pass
// (its argument 1 path) and 0x5bac00's string are left out.
inline constexpr std::size_t kInnateInfravision = 6;  // +0x3248 (eqstr 6617 'Infravision')
inline constexpr std::size_t kInnateSlam = 11;        // +0x325c
inline constexpr std::size_t kInnateUltravision = 13; // +0x3264 (eqstr 6622 'Ultravision')
std::array<std::int32_t, 25> init_innates(std::uint32_t race, std::uint32_t class_id);

// The Slam slot: 0 for races 2, 9, 10, 90, 92 and 93 (Barbarian, Troll, Ogre and their variants), else 0xff.
inline std::int32_t slam_innate(std::uint32_t race) { return init_innates(race, 0)[kInnateSlam]; }

} // namespace hailstone::game::mechanics
