// The reads of the hot button states (game/hot_button_state.h) over this client's state: the connection, the
// Actions window's skill timers and lists (ActionsUi), the character rules (focus reuse modifier).

#include <ctime>

#include "app/actions_ui.h"
#include "app/lua_ui_windows.h"
#include "game/hot_button_menu.h"
#include "game/mechanics/character.h"
#include "game/spell_gem.h"
#include "net/game_connection.h"

namespace hailstone::app {

std::uint32_t HostButtonQueries::now() { return net::GameConnection::eq_time(); }
std::uint32_t HostButtonQueries::fast_now() { return static_cast<std::uint32_t>(std::time(nullptr)); }

bool HostButtonQueries::primary_ready() { return !h_.actions || h_.actions->attack_state().primary_ready; }
bool HostButtonQueries::auto_fire() { return h_.conn && h_.conn->auto_fire(); }
bool HostButtonQueries::auto_attack() { return h_.conn && h_.conn->auto_attack(); }
bool HostButtonQueries::sitting() { return h_.conn && h_.conn->sitting(); }

std::uint32_t HostButtonQueries::skill_last_used(std::uint32_t skill) {
    return h_.actions ? h_.actions->skill_state().timers.last_used(skill) : 0;
}
std::uint32_t HostButtonQueries::skill_timer_duration(std::uint32_t skill) {
    return h_.actions && h_.character ? h_.actions->skill_state().timers.timer_duration(*h_.character, skill) : 0;
}
bool HostButtonQueries::skill_available(std::uint32_t skill) {
    return !h_.actions || h_.actions->skill_state().timers.is_available(skill);
}

std::optional<std::uint32_t> HostButtonQueries::list_skill(game::HotButtonType type, std::int32_t index) {
    if (!h_.hot_buttons || index < 0) return std::nullopt;
    std::int32_t s = -1;
    if (type == game::HotButtonType::CombatSkill && index < 4) s = h_.hot_buttons->combat_skills[static_cast<std::size_t>(index)];
    else if (type == game::HotButtonType::Ability && index < 6) s = h_.hot_buttons->abilities[static_cast<std::size_t>(index)];
    if (s < 0 || s >= 100) return std::nullopt;
    return static_cast<std::uint32_t>(s);
}

std::pair<std::uint32_t, std::uint32_t> HostButtonQueries::social_cool_down(std::int32_t social) {
    if (!h_.hot_buttons || social < 0 || social >= game::kSocialPages * game::kSocialButtons) return {0, 0};
    const game::Social& s = h_.hot_buttons->social(social);
    return {s.cooldown_start, s.cooldown_length};
}

std::optional<HostButtonQueries::AltAbility> HostButtonQueries::alt_ability(std::int32_t group) {
    if (!h_.conn) return std::nullopt;
    const game::AltAbility* rank = h_.conn->alt_adv().owned_rank(group);
    if (!rank) return std::nullopt;
    AltAbility a;
    a.owned = h_.conn->alt_adv().has(rank->id);
    if (a.owned) {
        game::AAReadiness r = h_.conn->aa_readiness(rank->id);
        a.left = r.remaining;
        a.total = r.total;
    }
    return a;
}

std::optional<HostButtonQueries::CombatAbility> HostButtonQueries::combat_ability(std::int32_t index) {
    if (!h_.conn || index < 0) return std::nullopt;
    const auto& c = *h_.conn;
    const std::uint32_t id = c.combat_ability(index);
    const game::SpellData* s = id > 0 ? c.spell_table().get(id) : nullptr;
    if (!s) return std::nullopt;
    CombatAbility a;
    a.ready = c.discipline_reuse_timer(c.combat_ability_timer(*s));
    a.recast = static_cast<std::int32_t>(s->recast_time);
    a.focus = h_.character && !c.server_ext().reuse_focus_dropped(c, *s) ? h_.character->focus_reuse_mod(*s) : 0;
    return a;
}

bool HostButtonQueries::leadership_active(std::int32_t ability, std::int32_t rank) {
    return h_.conn && h_.conn->leadership_ability_active(ability, rank);
}

std::optional<bool> HostButtonQueries::pet_button(std::int32_t index) { return pet_button_checked(h_, index); }

std::optional<HostButtonQueries::ItemRecast> item_recast_of(const net::GameConnection& c, const net::Item* it) {
    // ItemBase_HasRecastClicky @0x7b0c60: not a container, a click spell and a recast time.
    if (!it) return std::nullopt;
    const net::ItemEffect& click = it->effects[net::kEffectClick];
    if (it->is_bag() || click.effect == 0 || static_cast<std::int32_t>(click.recast_delay()) <= 0) return std::nullopt;
    return HostButtonQueries::ItemRecast{c.item_recast_timer(it), click.recast_delay()};
}

std::optional<HostButtonQueries::ItemRecast> HostButtonQueries::item_recast(const game::HotButton& b) {
    if (!h_.conn) return std::nullopt;
    // The button's CInvSlotWnd item: an inventory slot (5) and an item link (12, wherever its item is:
    // game::hot_button_item_slot, as the press finds it) in the possessions; the Krono slot (13) is the inventory
    // window's InvSlotKrono, whose item comes from KronoManager, which EQEmu never feeds: none.
    const auto& inv = h_.conn->inventory();
    const auto slot = game::hot_button_item_slot(inv, b);
    if (!slot) return std::nullopt;
    return item_recast_of(*h_.conn, inv.at(*slot));
}

bool hotbutton_extras(UiHost& host) {
    if (!host.find_option) return true;
    return host.find_option("hotbutton_extras").value_or(1) != 0;
}

std::uint32_t cool_down_color() {
    // GetUserColor(0x15f) @0x761c00: user colour 95 (eqclient.ini [TextColors] User_96), black taken as 0xf0f0f0.
    std::uint32_t c = user_color_rgb(static_cast<int>(game::kSpellGemOverlayUserColor) - 256);
    return game::spell_gem_overlay_color(c == 0 ? 0xf0f0f0u : c);
}

} // namespace hailstone::app
