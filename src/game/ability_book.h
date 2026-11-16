#pragma once

// The Ability Book (Hailstone's own window, not in the original; ui/<name>/abilitybook.lua): every ability the
// player can activate apart from spells, in one list. Three kinds, each used through the stock path its own window
// uses, so the game's and the server's rules hold:
//   - activated alternate abilities (game/alt_adv.h): the AA window's hot button (a social of number 120 + the
//     ability's group, which DoHotButton runs as /alt activate);
//   - combat abilities (disciplines): the combat ability hot button (0x84 + the profile index, doCombatAbility);
//   - activated skills (game/mechanics/abilities.h usable_skills): the skill hot button (UseSkill).
//
// Autoskill is the client's own helper: while it is on, the abilities on its list are used in their order as they
// come ready, one at a time, by default while the player fights (auto attack on an NPC in melee range). A
// beneficial ability may instead be used at any time or only out of combat, and one that leaves a buff can wait
// until the buff has worn off. It uses the same hot button path as a key press and is held to a minimum gap
// between uses. A server's hailstone.cfg can turn it off
// ([abilities] autoskill = off, plugin/policy.h).
//
// This header is the part without the connection: the keys, the skill descriptions and Autoskill's choice.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hailstone::game {

enum class AbilityKind : std::uint8_t { AltAbility = 0, Discipline = 1, Skill = 2 };

// An ability's key, stable across ranks so the favourites and the autoskill list keep it: "aa:<group>" (the AA's
// group id), "disc:<spell group>" or, for a spell without a group, "spell:<spell id>", and "skill:<skill id>".
std::string ability_key(AbilityKind kind, std::int32_t id);
std::string discipline_key(std::int32_t spell_group, std::uint32_t spell_id);
struct AbilityKeyParts {
    AbilityKind kind = AbilityKind::Skill;
    bool spell_id = false; // a discipline keyed by its spell id (no group)
    std::int32_t id = 0;
};
std::optional<AbilityKeyParts> parse_ability_key(std::string_view key);

// A short description of an activated skill (the original has none: these are Hailstone's own); "" for a skill
// this does not know.
std::string_view skill_description(std::uint32_t skill);

// ---- Autoskill

// When a rule may fire. Anything but Fighting applies only to a beneficial ability (AutoskillAbility::beneficial);
// a harmful one keeps to Fighting whatever its rule says, so Autoskill never starts a fight.
enum class AutoskillWhen : std::uint8_t {
    Fighting = 0, // auto attack on an NPC in melee range
    Always = 1,   // whenever the player is free to act
    Resting = 2,  // out of combat: auto attack off and the server's combat flag (OP_RestState) clear
};

// One ability on the autoskill list, with its conditions (0: none).
struct AutoskillRule {
    std::string key;
    int min_endurance_pct = 0; // use it only while the player's endurance is at least this
    int min_mana_pct = 0;      // likewise mana
    int target_hp_below = 0;   // only while the target's HP is below this percent (0: any)
    int target_hp_above = 0;   // only while the target's HP is above this percent (0: any)
    int self_hp_below = 0;     // only while the player's HP is below this percent (0: any)
    AutoskillWhen when = AutoskillWhen::Fighting;
    bool keep_buff = false;    // only while the ability's buff is not on the player (it has worn off)
};

// The list as the character ini keeps it: "key[;end;mana;hp[;hpabove;selfhp;when;buff]]" entries,
// comma-separated, in order (unknown keys and repeats dropped, percents clamped to 0..100, when to 0..2, buff 0
// or 1).
std::string format_autoskill_rules(const std::vector<AutoskillRule>& rules);
std::vector<AutoskillRule> parse_autoskill_rules(std::string_view text);

// Moves the rule for `key` `delta` places along the list (hs.AbilityBookAutoskillMove), stopping at either end; any
// delta, up to the int64 range, is safe. False when no rule has the key or it does not move.
bool move_autoskill_rule(std::vector<AutoskillRule>& rules, std::string_view key, std::int64_t delta);

// What Autoskill sees this frame.
struct AutoskillWorld {
    std::uint32_t now_ms = 0;
    bool in_game = false;
    bool dead = false, sitting = false, feigned = false, stunned = false, casting = false, invisible = false;
    bool auto_attack = false;
    bool target_npc = false;      // the target is a living NPC
    bool target_in_range = false; // within melee range
    int target_hp_pct = 100;
    int hp_pct = 100, endurance_pct = 100, mana_pct = 100;
    bool in_combat = false;       // the server's combat flag (OP_RestState)
};

// Whether the player is free to act at all: in game and alive, standing, not stunned, feigning, casting or
// invisible.
bool autoskill_free(const AutoskillWorld& w);
// Whether the player fights: free, auto attack on, an NPC target within melee range.
bool autoskill_can_act(const AutoskillWorld& w);
// Whether the world allows the rule's `when` for an ability (`beneficial` or not; see AutoskillWhen).
bool autoskill_when_holds(AutoskillWhen when, bool beneficial, const AutoskillWorld& w);

// An ability on the list as Autoskill's caller sees it this frame.
struct AutoskillAbility {
    bool ready = false;      // off its timer and usable
    bool beneficial = false; // safe to use outside a fight (a beneficial spell, or a skill on no one else)
    bool buff_on = false;    // its buff is on the player
};
// The ability of a key; none when the player cannot use it.
using AutoskillQuery = std::function<std::optional<AutoskillAbility>(const std::string&)>;

class Autoskill {
public:
    static constexpr std::uint32_t kGapMs = 600;     // between any two uses
    static constexpr std::uint32_t kRetryMs = 2000;  // before the same ability is tried again (its timer may lag)
    static constexpr int kMaxBackoff = 4;            // a use that keeps failing waits at most kRetryMs << 4 (32 s)

    // The rule to use now: the first, in list order, whose ability is ready (`query(key)`), whose `when` the world
    // allows and whose conditions hold; none while the player is not free or within kGapMs of the last use. The
    // caller uses it and calls used(). An ability tried before waits kRetryMs, doubled for each try after which it
    // was still ready (the use failed: no shield, out of reach ...); seen not ready (its timer runs) or its buff on
    // (the use went through) it waits no more.
    std::optional<std::size_t> next(const std::vector<AutoskillRule>& rules, const AutoskillQuery& query,
                                    const AutoskillWorld& w);
    void used(const std::string& key, std::uint32_t now_ms);
    void reset();

private:
    struct Tried {
        std::uint32_t at_ms = 0;
        int misses = 0; // tries in a row after which it was still ready
    };
    std::optional<std::uint32_t> last_ms_;
    std::unordered_map<std::string, Tried> tried_;
};

} // namespace hailstone::game
