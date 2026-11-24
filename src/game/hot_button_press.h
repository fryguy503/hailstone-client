#pragma once

// Pressing a hot button and what follows, as the original does it, checked against CHotButtonWnd_DoHotButton.jsonl, CHotButton_RightClick.jsonl and
// Social_Stop.jsonl.
//
// CHotButtonWnd::DoHotButton @0x68c8b0: a stunned local player (stand state 0x66) or a pending item request
// (IsItemPending > 0) prints 12084 (colour 0x111); a mouse press (a left click on the button) of types 5 and 13
// does nothing. Then by type (game/hotbuttons.h):
//   1 melee: value 0 AUTOPRIM (__ExecuteCmd 10); 1 a ranged attack (DoAttack 11) while __RangeAttackReady is set;
//   2 / 3: UseSkill of the [CombatSkills] / [Abilities] list entry `value` (its low byte); 9: UseSkill(value);
//   10 discipline: value 0x84 + n: PcZoneClient::doCombatAbility(GetCombatAbility(n), 1) when the spell exists;
//   4 social or AA (120 + group): only queued (__CurrentSocial; DoPassageOfTime runs it, SocialRunner);
//   11 leadership: Leadership_ActivateAbility(value / 8, value % 8);
//   5, 12, 13: a right click on the item slot's window at its screen rectangle's top left (OP_ItemVerifyRequest
//     for a usable item);
//   7: the bard twist ends and the gem's left click runs; 8 pet: the pet window's button flips its check (when
//     enabled) and the window gets the click (Cmd_pet's word, OP_PetCommands);
//   6 actions: 0 who (unless 0xddf654), 1 follow when invited else invite, 2 disband, 3 camp, 4 sit, 5 run/walk
//     (0xddf670 ^= 1), 7 options, 9 inventory (activated unless active), 8 toggle view, 10 the spell book (closes
//     when active; 12085 while the controlled spawn moves faster than 0.01; nothing while binding wounds), 11 the
//     byte 0xddf89c flips, 12 the find window (__ExecuteCmd 354).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::game {

inline constexpr std::uint32_t kStrCantUseCommand = 12084; // "You can't use that command right now..."
inline constexpr std::uint32_t kStrMustBeStopped = 12085;  // "You must be completely stopped before doing this action."
inline constexpr std::uint32_t kCmdAutoPrimary = 10, kCmdToggleFindWindow = 354;
inline constexpr std::uint8_t kStandStunned = 0x66, kStandBindWound = 0x69;
inline constexpr std::int32_t kSocialCount = 120; // 10 pages x 12; 120 + n queues alternate ability group n

// The record pressed (0x6c bytes at 0xe3ce08).
struct HotButtonRecord {
    std::uint8_t type = 0;
    std::int32_t value = 0;
    // Type 12: the item's location (ItemGlobalIndex slot / bag slot); nullopt without an item.
    struct Item {
        std::int16_t slot = 0, bag_slot = -1;
    };
    std::optional<Item> item;
};

// The globals DoHotButton reads and flips.
struct HotButtonGlobals {
    std::optional<std::uint8_t> stand_state;  // the local player's +0x35c; nullopt: no local player
    std::int32_t item_pending = 0;            // EverQuestinfo__IsItemPending
    std::uint8_t range_attack_ready = 0;      // __RangeAttackReady
    std::uint8_t group_invited = 0;           // 0xe634e0
    std::uint8_t no_who = 0;                  // 0xddf654
    std::uint8_t run_walk = 0;                // 0xddf670 __RunWalkState
    std::uint8_t toggle_89c = 0;              // 0xddf89c
    std::optional<float> controlled_speed;    // pinstControlledPlayer +0x7c; nullopt: none
};

enum class HotButtonWindow : std::uint8_t { Options, Inventory, SpellBook };

// An item slot (CInvSlot) the press found: its window (+4) and that window's screen rectangle's top left.
struct HotButtonSlot {
    bool window = false;
    std::int32_t left = 0, top = 0;
};
// A pet window button: +0x258 enabled, +0x1e4 checked.
struct HotButtonPetButton {
    std::uint8_t enabled = 0, checked = 0;
};

class HotButtonQueries {
public:
    virtual ~HotButtonQueries() = default;
    virtual void cant_use_command() = 0;                            // getString(12084) + dsp_chat(0x111, 1, 1)
    virtual void must_be_stopped() = 0;                             // getString(12085) + dsp_chat_2
    virtual void execute_cmd(std::uint32_t command) = 0;            // __ExecuteCmd(command, 1, 0)
    virtual void ranged_attack() = 0;                               // DoAttack(11, 0, target)
    virtual std::uint32_t list_entry(std::uint8_t type, std::int32_t index) = 0; // [CombatSkills] / [Abilities]
    virtual void use_skill(std::uint8_t skill) = 0;                 // CharacterZoneClient::UseSkill(skill, target)
    virtual std::int32_t combat_ability(std::int32_t index) = 0;    // PcBase::GetCombatAbility
    virtual bool spell_exists(std::int32_t spell) = 0;              // the spell manager's +0xc
    virtual void do_combat_ability(std::int32_t spell) = 0;         // PcZoneClient::doCombatAbility(spell, 1)
    virtual void set_current_social(std::int32_t social) = 0;       // 0x6143a0
    virtual void leadership_ability(std::int32_t group, std::int32_t ability) = 0;
    virtual std::optional<HotButtonSlot> find_inv_slot(std::int32_t slot, std::int32_t bag_slot) = 0;
    virtual std::optional<HotButtonSlot> find_krono_slot() = 0;     // 0x697ec0
    virtual void inv_slot_right_click(std::int32_t x, std::int32_t y) = 0; // CInvSlot::HandleRButtonUp
    virtual void who() = 0;
    virtual void group_follow() = 0;                                // Send_OP_GroupFollow2
    virtual void group_invite() = 0;                                // DoGroupInvite(nullptr)
    virtual void disband() = 0;
    virtual void camp() = 0;
    virtual void sit() = 0;
    virtual void toggle_view() = 0;
    virtual bool window_active(HotButtonWindow w) = 0;              // CXWnd::IsActive
    virtual void window_activate(HotButtonWindow w) = 0;            // virtual +0x90
    virtual void window_deactivate(HotButtonWindow w) = 0;          // virtual +0x94
    virtual void clear_bard_twist() = 0;
    virtual void spell_gem_left_click(std::int32_t gem) = 0;        // CCastSpellWnd::HandleSpellLeftClick
    virtual std::optional<HotButtonPetButton> pet_button(std::int32_t button) = 0; // CPetInfoWnd::GetButton
    virtual void pet_button_set_check(bool checked) = 0;            // the button's +0x170
    virtual void pet_window_click(std::int32_t button) = 0;         // the pet window's +0x88 (clicked)
};

// CHotButtonWnd::DoHotButton(index, mouse) for the record pressed.
void do_hot_button(HotButtonGlobals& g, const HotButtonRecord& r, std::int32_t mouse, HotButtonQueries& q);

// ---- a right click (0x54fc10) and the socials' queue

class HotButtonRightClickQueries {
public:
    virtual ~HotButtonRightClickQueries() = default;
    virtual void social_stop(std::int32_t social, bool keep_queued) = 0; // Social_Stop
    virtual void open_social_editor(std::int32_t social) = 0;           // CSocialEditWnd
    virtual void aa_window_show(std::int32_t group) = 0;                // the AA window activated, ShowAbility
    virtual void inv_slot_right_click(std::int32_t x, std::int32_t y) = 0;
    virtual void spell_gem_right_click(std::int32_t gem) = 0;           // CCastSpellWnd::HandleSpellRightClick
};
struct HotButtonRightClick {
    std::uint8_t type = 0;
    std::int32_t value = 0;
    std::int32_t current_social = -1;     // __CurrentSocial
    std::int32_t social_cooldown_length = 0; // the clicked social's +0x518 (value < 120)
    std::optional<HotButtonSlot> slot;    // the button's slot window (+0x224) when it has a CInvSlot (+0x290)
    bool gem_window = false;              // +0x228
    std::int32_t no_spell_info = 0;       // 0xddf794
};
// 0x54fc10: a social that is queued stops (Social_Stop(-1, 0)), one with a running cooldown has it cleared
// (Social_Stop(n, 1)), otherwise the social editor opens; an ability (120 + group) opens the AA window on it; items
// (5, 12, 13) get the slot's right click; a spell gem (7) its spell display unless 0xddf794. Always true.
bool hot_button_right_click(const HotButtonRightClick& c, HotButtonRightClickQueries& q);

// The queued social (0xab6a28), the /pause ticks left (0xb824c4), the next line (0xb824c8).
struct SocialQueue {
    std::int32_t current = -1;
    std::int32_t pause = 0;
    std::int32_t line = 0;
    std::uint32_t last_tick = 0; // 0xb824c0
};
// 0x6143b0 (Social_Stop; /stopsocial): social -1 means the queued one; below 120 and on page < 10 with a
// button < 12 (C division by 12) its cooldown and the counters clear. Returns the record index cleared
// (records -1..-13 lie before the table; the multiplication by 0x51c wraps as the original's), nullopt when none;
// then, unless keep_queued, nothing stays queued.
std::optional<std::int32_t> social_stop(SocialQueue& s, std::int32_t social, bool keep_queued);

// One line of a social: what DoPassageOfTime runs.
class SocialRunQueries {
public:
    virtual ~SocialRunQueries() = default;
    virtual const std::string* line(std::int32_t social, std::int32_t line) = 0; // the social's line 0..4
    virtual void interpret(const std::string& command) = 0;                     // CEverQuest::InterpretCmd
    virtual void alternate_ability(std::int32_t group) = 0;  // "/alternateadv act <group>" (eqstr 0x3773, 0x33d9)
    virtual bool allow_directive(std::string_view) { return true; } // the server's command policy
    // The social's cooldown (+0x514 start, +0x518 length): started by /timer unless one is running.
    virtual bool cooldown_running(std::int32_t social) = 0;
    virtual void start_cooldown(std::int32_t social, std::int32_t length_ms) = 0;
};
// A social line as DoPassageOfTime reads it: any number of leading "/pause N" and "/timer N" (case-insensitive),
// each followed by the rest of the line after a comma, then the command for the interpreter. A line's wait is the
// last /pause (1..600 steps; 1 without one), each /timer starts the cooldown (0..360000 tenths of a second) unless
// one is running. The social editor shows a line this way too.
struct SocialLineParts {
    std::optional<std::int32_t> pause;
    std::vector<std::int32_t> timers;
    std::string command; // empty: the line runs nothing (it may still wait)
};
SocialLineParts split_social_line(const std::string& text);

// CharacterZoneClient::DoPassageOfTime's social step, run when more than 100 ms passed since the last one: an
// ability as "/alternateadv act <group>"; a social one line per step (empty lines skipped), after which one step
// waits; "/pause N" waits N steps (1..600) and "/timer N" starts the cooldown of N tenths of a second
// (0..360000), each followed by the rest of the line after a comma.
void social_passage_of_time(SocialQueue& s, std::uint32_t now_ms, SocialRunQueries& q);

} // namespace hailstone::game
