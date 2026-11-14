#include "game/hot_button_state.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace hailstone::game {

namespace {

constexpr std::int32_t kAltAbilityBase = 120;

bool is_item(HotButtonType t) {
    return t == HotButtonType::InventorySlot || t == HotButtonType::ItemLink || t == HotButtonType::KronoSlot;
}

} // namespace

void update_item_cool_down(ButtonCoolDown& cd, const std::optional<HotButtonStateQueries::ItemRecast>& r,
                           std::uint32_t now, std::uint32_t fast_now) {
    if (!r) {
        cd.stop(); // vtable +0x194
        return;
    }
    const auto left = static_cast<std::int32_t>(r->timer - fast_now);
    const bool done = r->timer == 0 || left <= 0;
    const std::int32_t diff = left * 1000 - static_cast<std::int32_t>(cd.left(now));
    if (cd.duration == 0 || std::abs(diff) >= 1000) {
        if (done) cd.stop();
        else cd.set_ending(fast_time_to_eq_time(r->timer, fast_now, now), r->recast * 1000u);
    } else if (done) {
        cd.stop();
    }
}

void update_potion_belt_cool_down(ButtonCoolDown& cd, const std::optional<HotButtonStateQueries::ItemRecast>& r,
                                  std::uint32_t now, std::uint32_t fast_now) {
    if (!r || cd.duration != 0) return;
    if (static_cast<std::int32_t>(r->timer) <= static_cast<std::int32_t>(fast_now)) return;
    cd.set_ending(fast_time_to_eq_time(r->timer, fast_now, now), r->recast * 1000u);
}

void update_skill_button(ButtonCoolDown& cd, bool& checked, std::uint32_t skill, HotButtonStateQueries& q) {
    // The queries in their written order (an argument list's order is the compiler's).
    const std::uint32_t now = q.now();
    const auto last_used = q.skill_last_used(skill);
    const auto duration = q.skill_timer_duration(skill);
    cd.set_running(now, last_used, duration);
    checked = !q.skill_available(skill);
}

bool weapon_button_checked(bool melee, HotButtonStateQueries& q, bool extras) {
    if (!q.primary_ready() || q.auto_fire()) return true;
    return extras && melee && q.auto_attack();
}

void update_hot_button(HotButtonDisplay& d, const HotButton& b, HotButtonStateQueries& q, bool extras) {
    const std::uint32_t now = q.now();
    if (b.type != d.type || b.slot != d.slot || !d.seen) {
        if (d.seen) d.cool_down.stop();
        d.checked = false;
        d.updated = now - 1000; // updated at once
        d.seen = true;
        d.type = b.type;
        d.slot = b.slot;
    }
    if (is_item(b.type)) {
        // The recast before fast_now, as written (an argument list's order is the compiler's).
        const auto recast = q.item_recast(b);
        update_item_cool_down(d.cool_down, recast, now, q.fast_now());
    }
    // OnProcessFrame's throttle: last + 100 (200 for an item link) < now.
    const std::uint32_t interval = b.type == HotButtonType::ItemLink ? 200 : 100;
    if (static_cast<std::int32_t>(now - (d.updated + interval)) <= 0) return;
    d.updated = now;
    d.checked = false; // SetCheck(0)
    const std::int32_t v = b.slot;
    switch (b.type) {
    case HotButtonType::WeaponSlot:
        d.checked = weapon_button_checked(v == 0, q, extras);
        break;
    case HotButtonType::Skill:
        if (v != -1) update_skill_button(d.cool_down, d.checked, static_cast<std::uint32_t>(v), q);
        break;
    case HotButtonType::CombatSkill:
    case HotButtonType::Ability:
        if (!extras) break;
        if (auto s = q.list_skill(b.type, v)) update_skill_button(d.cool_down, d.checked, *s, q);
        else d.cool_down.stop();
        break;
    case HotButtonType::Social:
        if (v < kAltAbilityBase) {
            auto [start, length] = q.social_cool_down(v);
            d.checked = d.cool_down.set_running(now, start, length);
        } else if (auto a = q.alt_ability(v - kAltAbilityBase)) {
            if (!a->owned) {
                d.checked = true;
                if (d.cool_down.duration != 0) d.cool_down.stop();
            } else {
                d.checked = d.cool_down.set_running(now, static_cast<std::uint32_t>((a->left - a->total) * 1000) + now,
                                                    static_cast<std::uint32_t>(a->total * 1000));
            }
        }
        break;
    case HotButtonType::CombatAbility: {
        auto a = v < kCombatAbilityValueBase ? std::nullopt : q.combat_ability(v - kCombatAbilityValueBase);
        if (!a) {
            d.cool_down.stop();
            break;
        }
        const auto fast_now = q.fast_now();
        if (a->ready <= fast_now) {
            d.cool_down.stop();
            break;
        }
        d.checked = true;
        const auto left = static_cast<std::uint64_t>(a->ready - fast_now) * 1000;
        const auto reuse = static_cast<std::uint64_t>(
            std::max<std::int64_t>(static_cast<std::int64_t>(a->recast) - a->focus, 0));
        // A shared timer's wait can exceed this ability's reuse. Keep it shaded instead of passing a future
        // begin to SetCoolDown's unsigned elapsed test. The button's EQ-time fields hold at most 2^32-1 ms.
        constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
        const auto duration = static_cast<std::uint32_t>(std::min<std::uint64_t>(std::max(reuse, left), limit));
        const auto remaining = static_cast<std::uint32_t>(std::min<std::uint64_t>(left, limit));
        d.cool_down.set_ending(now + remaining, duration);
        break;
    }
    case HotButtonType::Leadership: {
        // C division and remainder by 8 (value / 8 compared unsigned with 30).
        const std::int32_t ability = v / 8, rank = v % 8;
        if (static_cast<std::uint32_t>(ability) < 31 && !q.leadership_active(ability, rank)) d.checked = true;
        break;
    }
    case HotButtonType::PetCommand:
        if (auto c = q.pet_button(v)) d.checked = *c;
        break;
    case HotButtonType::MenuButton:
        if (extras && v == static_cast<std::int32_t>(MenuButton::Sit)) d.checked = q.sitting();
        break;
    default:
        break;
    }
}

} // namespace hailstone::game
