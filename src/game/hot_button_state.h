#pragma once

// What a hot button shows besides its face: its check (the button drawn pressed) and its cool-down, as
// CHotButtonWnd_UpdateButtonStates @0x5516c0 sets them on the hot button's CButtonWnd (+0x220) every 100 ms (200
// for an item link) and on each change of the button, read from the disassembly:
//
//  - first SetCheck(0) (CHotButton::SetCheck @0x54fbe0 -> the button's vtable +0x170), then by type
//    (game/hotbuttons.h):
//  - 1 weapon slot (melee and range alike): checked while the primary swing is not ready (0xde0864, which eqlib
//    calls __RangeAttackReady) and while auto fire (__Autofire 0xe635ab) is on;
//  - 9 skill: SetCoolDown(CSkillMgr::GetSkillLastUsed, GetSkillTimerDuration) (vtable +0x184), checked while
//    CSkillMgr::IsAvailable is false (a running timer; sneak while sneaking and hide while hidden, which the timers
//    keep unavailable);
//  - 4 social below 120: SetCoolDown(the /timer start +0x514, length +0x518), checked while it runs;
//  - 4 from 120 (alternate ability group n - 120, 0x4a3d70's rank): not owned (HasAlternateAbility) checks it
//    and stops the cool-down; owned, AltAdvManager::IsAbilityReady's (left, total) seconds give
//    SetCoolDown(now + (left - total) * 1000, total * 1000), checked while it runs;
//  - 10 combat ability (0x84 + n): SetCoolDown(FastTimeToEQTime(GetDisciplineReuseTimer(+0x198)) - reuse, reuse)
//    with reuse = the recast +0x18 less CharacterZoneClient::GetFocusReuseMod, checked while it runs; none: stopped;
//    presentation fix: when a shared timer exceeds this ability's reuse, keep it checked and fully shaded until
//    the shared wait fits that reuse, rather than letting a future begin look ready;
//  - 11 leadership (ability value / 8 below 31, rank value % 8): checked while Leadership_IsAbilityActive is false;
//  - 8 pet: the pet window's button's check (+0x1e4) when that button is enabled (+0x258);
//  - 2 / 3 (the Actions window's lists), 6 (menu), 7 (spell gem: its CSpellGemWnd draws the gem's own tint and
//    cool-down), 5 / 12 / 13 (items: the CInvSlotWnd draws, below): nothing.
// CInvSlotWnd_Draw @0x69a080 gives an item with a recast clicky (ItemBase_HasRecastClicky @0x7b0c60: not a
// container, a click spell and a recast time) its recast timer's cool-down (update_item_cool_down below).
//
// Hailstone extras (the `extras` switch, option hotbutton_extras, on by default; docs/mechanics.md): the Actions
// window's list buttons (2 / 3) get their skill's timer and check as type 9 does, the melee button (type 1, value 0)
// is checked while auto attack is on, and the Sit menu button while sitting. A button that changes also drops the
// cool-down the previous one left (the original keeps it running to its end on the types that never set one).

#include <cstdint>
#include <optional>
#include <utility>

#include "game/hotbuttons.h"
#include "game/spell_gem.h"

namespace hailstone::game {

class HotButtonStateQueries {
public:
    virtual ~HotButtonStateQueries() = default;
    virtual std::uint32_t now() = 0;      // __EQGetTime (ms)
    virtual std::uint32_t fast_now() = 0; // Util::FastTime (s)
    virtual bool primary_ready() = 0;     // 0xde0864
    virtual bool auto_fire() = 0;         // __Autofire
    virtual bool auto_attack() = 0;       // __Attack (extras)
    virtual bool sitting() = 0;           // StandState 110 (extras)
    // CSkillMgr (game/mechanics/abilities.h SkillTimers).
    virtual std::uint32_t skill_last_used(std::uint32_t skill) = 0;
    virtual std::uint32_t skill_timer_duration(std::uint32_t skill) = 0;
    virtual bool skill_available(std::uint32_t skill) = 0;
    // The [CombatSkills] (type 2) / [Abilities] (type 3) entry `index`: its skill, none when empty (extras).
    virtual std::optional<std::uint32_t> list_skill(HotButtonType type, std::int32_t index) = 0;
    // A social's /timer cool-down: {start, length} (ms).
    virtual std::pair<std::uint32_t, std::uint32_t> social_cool_down(std::int32_t social) = 0;
    struct AltAbility {
        bool owned = false;
        std::int32_t left = 0, total = 0; // IsAbilityReady's seconds
    };
    // The alternate ability group's rank (0x4a3d70), none without one.
    virtual std::optional<AltAbility> alt_ability(std::int32_t group) = 0;
    struct CombatAbility {
        std::uint32_t ready = 0; // GetDisciplineReuseTimer: FastTime seconds
        std::int32_t recast = 0; // the spell's +0x18 (ms)
        std::int32_t focus = 0;  // GetFocusReuseMod
    };
    // PcBase::GetCombatAbility(n)'s spell, none when it has none.
    virtual std::optional<CombatAbility> combat_ability(std::int32_t index) = 0;
    virtual bool leadership_active(std::int32_t ability, std::int32_t rank) = 0;
    // The pet window's button `index`: its check when it is enabled, none otherwise.
    virtual std::optional<bool> pet_button(std::int32_t index) = 0;
    struct ItemRecast {
        std::uint32_t timer = 0;  // PcZoneClient::GetItemRecastTimer: Util::FastTime seconds, 0 none
        std::uint32_t recast = 0; // ItemBase_GetSpellRecastTime(0): seconds
    };
    // The button's item when it has a recast clicky; none otherwise (no item, or no recast).
    virtual std::optional<ItemRecast> item_recast(const HotButton& b) = 0;
};

// One hot button's state, kept between updates as the button window keeps it.
struct HotButtonDisplay {
    ButtonCoolDown cool_down;
    bool checked = false;
    std::uint32_t updated = 0; // +0x1e0: when it was last updated
    bool seen = false;
    HotButtonType type = HotButtonType::None; // +0x1e8 / +0x1ec: what it was updated for
    std::int32_t slot = 0;
};

// An item slot's recast cool-down as CInvSlotWnd_Draw @0x69a080 keeps it, each draw, for an item with a recast
// clicky (`r`; none: stopped): with left = the timer - Util::FastTime (seconds), done when there is no timer or
// left <= 0; when the button has no cool-down (GetCoolDownTotalDuration 0) or left x 1000 differs from what the
// cool-down has left by 1000 ms or more, it is stopped when done, else restarted by vf94
// SetCoolDownCompletionFastTime(timer, recast x 1000) (@0x897930: ending at FastTimeToEQTime(timer)); otherwise it
// is stopped when done.
void update_item_cool_down(ButtonCoolDown& cd, const std::optional<HotButtonStateQueries::ItemRecast>& r,
                           std::uint32_t now, std::uint32_t fast_now);
// CPotionBeltWnd::PostDraw @0x71ee90 for one belt slot holding an item with a recast clicky (`r`): while its button
// has no cool-down and the timer is after Util::FastTime, the same vf94 start; the cool-down otherwise runs out
// on its own (DrawCooldown stops it).
void update_potion_belt_cool_down(ButtonCoolDown& cd, const std::optional<HotButtonStateQueries::ItemRecast>& r,
                                  std::uint32_t now, std::uint32_t fast_now);

// UpdateButtonStates' part for `b` (its throttle included: nothing until 100 ms after the last update, 200 for an
// item link, unless the button changed).
void update_hot_button(HotButtonDisplay& d, const HotButton& b, HotButtonStateQueries& q, bool extras);

// The Actions window's Combat and Abilities pages (CActionsWnd::CombatPageOnProcessFrame @0x613db0,
// 0x613a70): a skill button's SetCoolDown(GetSkillLastUsed, GetSkillTimerDuration) and SetCheck(!IsAvailable);
// the melee and range buttons are both checked while the primary swing is not ready or auto fire is on (with the
// extras, the melee one also while auto attack is on).
void update_skill_button(ButtonCoolDown& cd, bool& checked, std::uint32_t skill, HotButtonStateQueries& q);
bool weapon_button_checked(bool melee, HotButtonStateQueries& q, bool extras);

} // namespace hailstone::game
