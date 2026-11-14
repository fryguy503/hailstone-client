// The hot buttons' checks and cool-downs (game/hot_button_state.h: CHotButtonWnd_UpdateButtonStates @0x5516c0,
// CInvSlotWnd_Draw's recast part, CActionsWnd's combat page) and CButtonWnd's cool-down (game/spell_gem.h).
#include <doctest/doctest.h>

#include <map>

#include "game/hot_button_state.h"

using namespace hailstone;
using namespace hailstone::game;

namespace {

struct Fake : HotButtonStateQueries {
    std::uint32_t t = 100000, fast = 5000;
    bool primary = true, fire = false, attack = false, sit = false;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> skills; // last used, duration
    std::map<std::uint32_t, bool> available;
    std::pair<std::uint32_t, std::uint32_t> social{0, 0};
    std::optional<AltAbility> aa;
    std::optional<CombatAbility> disc;
    bool leader = false;
    std::optional<bool> pet;
    std::optional<ItemRecast> item;

    std::uint32_t now() override { return t; }
    std::uint32_t fast_now() override { return fast; }
    bool primary_ready() override { return primary; }
    bool auto_fire() override { return fire; }
    bool auto_attack() override { return attack; }
    bool sitting() override { return sit; }
    std::uint32_t skill_last_used(std::uint32_t s) override { return skills[s].first; }
    std::uint32_t skill_timer_duration(std::uint32_t s) override { return skills[s].second; }
    bool skill_available(std::uint32_t s) override { return available.contains(s) ? available[s] : true; }
    std::optional<std::uint32_t> list_skill(HotButtonType type, std::int32_t index) override {
        if (type == HotButtonType::CombatSkill && index == 0) return 30u; // kick
        return std::nullopt;
    }
    std::pair<std::uint32_t, std::uint32_t> social_cool_down(std::int32_t) override { return social; }
    std::optional<AltAbility> alt_ability(std::int32_t) override { return aa; }
    std::optional<CombatAbility> combat_ability(std::int32_t) override { return disc; }
    bool leadership_active(std::int32_t, std::int32_t) override { return leader; }
    std::optional<bool> pet_button(std::int32_t) override { return pet; }
    std::optional<ItemRecast> item_recast(const HotButton&) override { return item; }
};

HotButton button(HotButtonType t, std::int32_t v) {
    HotButton b;
    b.type = t;
    b.slot = v;
    return b;
}

} // namespace

TEST_CASE("CButtonWnd's cool-down: SetCoolDown, its left and the rectangle's fraction") {
    ButtonCoolDown cd;
    CHECK(cd.set_running(1000, 500, 1000));
    CHECK(cd.begin == 500);
    CHECK(cd.duration == 1000);
    CHECK(cd.left(1000) == 500);
    // half gone; capped at 0.87
    CHECK(*cd.overlay_fraction(1000) == doctest::Approx(0.5));
    CHECK(*cd.overlay_fraction(1450) == doctest::Approx(0.87));
    // its time up: SetCoolDown stops it and says so; the draw stops one that ran out too
    CHECK_FALSE(cd.set_running(1500, 500, 1000));
    CHECK(cd.duration == 0);
    cd.set(2000, 100);
    CHECK_FALSE(cd.overlay_fraction(2100));
    CHECK(cd.duration == 0);
    // vf96: a zero begin stops it; FastTimeToEQTime
    cd.set(0, 5);
    CHECK(cd.duration == 0);
    CHECK(fast_time_to_eq_time(5010, 5000, 100000) == 110000);
    // the rectangle: the gem's, from the top moved down by the part gone
    CHECK(spell_gem_overlay_top(0.5, 10, 50) == 30);
}

TEST_CASE("hot buttons: weapon slots are checked while the primary swing is not ready or auto fire is on") {
    Fake q;
    HotButtonDisplay d;
    update_hot_button(d, button(HotButtonType::WeaponSlot, 0), q, false);
    CHECK_FALSE(d.checked);
    q.t += 101;
    q.primary = false;
    update_hot_button(d, button(HotButtonType::WeaponSlot, 0), q, false);
    CHECK(d.checked);
    // the 100 ms throttle: nothing changes before it
    q.primary = true;
    q.t += 50;
    update_hot_button(d, button(HotButtonType::WeaponSlot, 0), q, false);
    CHECK(d.checked);
    q.t += 51;
    update_hot_button(d, button(HotButtonType::WeaponSlot, 0), q, false);
    CHECK_FALSE(d.checked);
    q.fire = true;
    q.t += 101;
    update_hot_button(d, button(HotButtonType::WeaponSlot, 1), q, false);
    CHECK(d.checked);
    // extras: the melee button while auto attack is on
    q.fire = false;
    q.attack = true;
    HotButtonDisplay m;
    update_hot_button(m, button(HotButtonType::WeaponSlot, 0), q, false);
    CHECK_FALSE(m.checked);
    q.t += 101;
    update_hot_button(m, button(HotButtonType::WeaponSlot, 0), q, true);
    CHECK(m.checked);
    CHECK(weapon_button_checked(false, q, true) == false);
}

TEST_CASE("hot buttons: a skill's timer and check, and the extras' list buttons") {
    Fake q;
    q.skills[30] = {q.t - 4000, 10000};
    q.available[30] = false;
    HotButtonDisplay d;
    update_hot_button(d, button(HotButtonType::Skill, 30), q, false);
    CHECK(d.checked);
    CHECK(d.cool_down.left(q.t) == 6000);
    CHECK(*d.cool_down.overlay_fraction(q.t) == doctest::Approx(0.4));
    // a type 2 list button: nothing in the original, the skill's state with the extras
    HotButtonDisplay l;
    update_hot_button(l, button(HotButtonType::CombatSkill, 0), q, false);
    CHECK_FALSE(l.checked);
    CHECK(l.cool_down.duration == 0);
    HotButtonDisplay e;
    update_hot_button(e, button(HotButtonType::CombatSkill, 0), q, true);
    CHECK(e.checked);
    CHECK(e.cool_down.left(q.t) == 6000);
    // the Actions window's skill buttons
    ButtonCoolDown cd;
    bool checked = false;
    update_skill_button(cd, checked, 30, q);
    CHECK(checked);
    CHECK(cd.left(q.t) == 6000);
}

TEST_CASE("hot buttons: socials, alternate abilities, combat abilities, leadership, pets") {
    Fake q;
    HotButtonDisplay s;
    q.social = {q.t - 1000, 3000};
    update_hot_button(s, button(HotButtonType::Social, 5), q, false);
    CHECK(s.checked);
    CHECK(s.cool_down.left(q.t) == 2000);

    // AA: not owned is checked; owned: (left, total) seconds
    HotButtonDisplay a;
    q.aa = HotButtonStateQueries::AltAbility{false, 0, 0};
    update_hot_button(a, button(HotButtonType::Social, 120 + 7), q, false);
    CHECK(a.checked);
    q.aa = HotButtonStateQueries::AltAbility{true, 30, 120};
    q.t += 101;
    update_hot_button(a, button(HotButtonType::Social, 120 + 7), q, false);
    CHECK(a.checked);
    CHECK(a.cool_down.duration == 120000);
    CHECK(a.cool_down.left(q.t) == 30000);

    // combat ability: ready at FastTime + 20 s, recast 60 s less a 15 s focus
    HotButtonDisplay c;
    q.disc = HotButtonStateQueries::CombatAbility{q.fast + 20, 60000, 15000};
    update_hot_button(c, button(HotButtonType::CombatAbility, kCombatAbilityValueBase + 2), q, false);
    CHECK(c.checked);
    CHECK(c.cool_down.duration == 45000);
    CHECK(c.cool_down.left(q.t) == 20000);
    // no ability: stopped
    q.disc.reset();
    q.t += 101;
    update_hot_button(c, button(HotButtonType::CombatAbility, kCombatAbilityValueBase + 2), q, false);
    CHECK_FALSE(c.checked);
    CHECK(c.cool_down.duration == 0);

    // leadership: checked while the ability is not active
    HotButtonDisplay l;
    update_hot_button(l, button(HotButtonType::Leadership, 3 * 8 + 1), q, false);
    CHECK(l.checked);
    q.leader = true;
    q.t += 101;
    update_hot_button(l, button(HotButtonType::Leadership, 3 * 8 + 1), q, false);
    CHECK_FALSE(l.checked);

    // pet: the pet window button's check
    HotButtonDisplay p;
    q.pet = true;
    update_hot_button(p, button(HotButtonType::PetCommand, 4), q, false);
    CHECK(p.checked);

    // menu Sit: extras only
    HotButtonDisplay m;
    q.sit = true;
    update_hot_button(m, button(HotButtonType::MenuButton, static_cast<std::int32_t>(MenuButton::Sit)), q, false);
    CHECK_FALSE(m.checked);
    HotButtonDisplay m2;
    update_hot_button(m2, button(HotButtonType::MenuButton, static_cast<std::int32_t>(MenuButton::Sit)), q, true);
    CHECK(m2.checked);
}

TEST_CASE("combat ability hot buttons keep a longer shared reuse timer shaded") {
    Fake q;
    HotButtonDisplay longer, shorter, zero;
    const auto ready = q.fast + 600;
    q.disc = HotButtonStateQueries::CombatAbility{ready, 600000, 0};
    update_hot_button(longer, button(HotButtonType::CombatAbility, kCombatAbilityValueBase), q, false);
    q.disc = HotButtonStateQueries::CombatAbility{ready, 300000, 0};
    update_hot_button(shorter, button(HotButtonType::CombatAbility, kCombatAbilityValueBase + 1), q, false);
    CHECK(longer.checked);
    CHECK(shorter.checked);
    CHECK(longer.cool_down.left(q.t) == 600000);
    CHECK(shorter.cool_down.left(q.t) == 600000);
    REQUIRE(shorter.cool_down.overlay_fraction(q.t));
    CHECK(*shorter.cool_down.overlay_fraction(q.t) == 0.0);
    q.disc = HotButtonStateQueries::CombatAbility{ready, 0, 0};
    update_hot_button(zero, button(HotButtonType::CombatAbility, kCombatAbilityValueBase + 2), q, false);
    CHECK(zero.checked);
    CHECK(zero.cool_down.left(q.t) == 600000);
    CHECK(*zero.cool_down.overlay_fraction(q.t) == 0.0);

    q.fast += 400;
    q.t += 400000;
    q.disc = HotButtonStateQueries::CombatAbility{ready, 300000, 0};
    update_hot_button(shorter, button(HotButtonType::CombatAbility, kCombatAbilityValueBase + 1), q, false);
    CHECK(shorter.checked);
    CHECK(shorter.cool_down.duration == 300000);
    CHECK(shorter.cool_down.left(q.t) == 200000);
    CHECK(*shorter.cool_down.overlay_fraction(q.t) == doctest::Approx(1.0 / 3.0));
    // A focus larger than recast is clamped for presentation; it cannot override the shared server deadline.
    q.disc->focus = 400000;
    q.t += 101;
    update_hot_button(shorter, button(HotButtonType::CombatAbility, kCombatAbilityValueBase + 1), q, false);
    CHECK(shorter.checked);
    CHECK(shorter.cool_down.left(q.t) == 200000);
    CHECK(*shorter.cool_down.overlay_fraction(q.t) == 0.0);

    q.fast = ready;
    q.t += 200000;
    update_hot_button(shorter, button(HotButtonType::CombatAbility, kCombatAbilityValueBase + 1), q, false);
    CHECK_FALSE(shorter.checked);
    CHECK(shorter.cool_down.duration == 0);
    q.disc->ready = q.fast + 10;
    q.t += 101;
    update_hot_button(shorter, button(HotButtonType::CombatAbility, kCombatAbilityValueBase + 1), q, false);
    REQUIRE(shorter.checked);
    q.disc->ready = 0; // An explicit server clear also stops it.
    q.t += 101;
    update_hot_button(shorter, button(HotButtonType::CombatAbility, kCombatAbilityValueBase + 1), q, false);
    CHECK_FALSE(shorter.checked);
    CHECK(shorter.cool_down.duration == 0);
    q.disc->ready = q.fast + 10;
    q.t += 101;
    update_hot_button(shorter, button(HotButtonType::CombatAbility, kCombatAbilityValueBase + 1), q, false);
    REQUIRE(shorter.checked);
    update_hot_button(shorter, button(HotButtonType::MenuButton, 0), q, false);
    CHECK_FALSE(shorter.checked);
    CHECK(shorter.cool_down.duration == 0);
}

TEST_CASE("hot buttons: an item's recast timer (CInvSlotWnd_Draw) and a changed button") {
    Fake q;
    HotButtonDisplay d;
    // the timer 9 s after Util::FastTime: ending at FastTimeToEQTime(timer), the recast's length
    q.item = HotButtonStateQueries::ItemRecast{q.fast + 9, 30};
    update_hot_button(d, button(HotButtonType::InventorySlot, 22), q, false);
    CHECK(d.cool_down.duration == 30000);
    CHECK(d.cool_down.left(q.t) == 9000);
    CHECK_FALSE(d.checked);
    // within a second of the timer it is left alone
    q.t += 400;
    update_hot_button(d, button(HotButtonType::InventorySlot, 22), q, false);
    CHECK(d.cool_down.left(q.t) == 8600);
    // a second off (the timer moved on): restarted
    q.item->timer += 2;
    q.t += 200;
    update_hot_button(d, button(HotButtonType::InventorySlot, 22), q, false);
    CHECK(d.cool_down.left(q.t) == 11000);
    // its end reached: stopped
    q.fast = q.item->timer;
    q.t += 200;
    update_hot_button(d, button(HotButtonType::InventorySlot, 22), q, false);
    CHECK(d.cool_down.duration == 0);
    // no recast clicky: none
    q.item.reset();
    update_hot_button(d, button(HotButtonType::InventorySlot, 22), q, false);
    CHECK(d.cool_down.duration == 0);

    // another button in the place drops the old cool-down
    HotButtonDisplay s;
    q.skills[30] = {q.t, 10000};
    q.available[30] = false;
    update_hot_button(s, button(HotButtonType::Skill, 30), q, false);
    CHECK(s.cool_down.duration == 10000);
    q.t += 5;
    update_hot_button(s, button(HotButtonType::MenuButton, 0), q, false);
    CHECK(s.cool_down.duration == 0);
    CHECK_FALSE(s.checked);
}

TEST_CASE("CPotionBeltWnd::PostDraw: a belt slot's recast cool-down") {
    ButtonCoolDown cd;
    const std::uint32_t now = 100000, fast = 5000;
    // no timer after now: nothing
    update_potion_belt_cool_down(cd, HotButtonStateQueries::ItemRecast{fast, 30}, now, fast);
    CHECK(cd.duration == 0);
    // started once, then left to run out (a later timer does not restart a running one)
    update_potion_belt_cool_down(cd, HotButtonStateQueries::ItemRecast{fast + 12, 30}, now, fast);
    CHECK(cd.duration == 30000);
    CHECK(cd.left(now) == 12000);
    update_potion_belt_cool_down(cd, HotButtonStateQueries::ItemRecast{fast + 20, 30}, now, fast);
    CHECK(cd.left(now) == 12000);
    update_potion_belt_cool_down(cd, std::nullopt, now, fast);
    CHECK(cd.duration == 30000);
}
