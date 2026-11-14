// Combat abilities (disciplines) and activated skills, for UIs that list them rather than draw the stock buttons
// (ui/gamepad/abilities.lua):
//
//   list = eq.Disciplines()   -- the combat abilities CCombatSkillsSelectWnd lists: { index (0-based, the profile's
//                                list), spell, name, icon, level, endurance, mana, reuse (s), remaining (s), timer }
//   list = eq.UsableSkills()  -- the skills the Actions window's buttons take (game/mechanics/abilities.h
//                                usable_skills): { id, name, combat, remaining (s) }
//   ok = eq.UseSkill(id)      -- uses skill id as a hot button holding it does (DoHotButton's skill button:
//                                UseSkill on the target), for a skill of eq.UsableSkills only (the Tracking skill
//                                opens the tracking window, CTrackingWnd::AboutToShow)
//
// and the CombatAbilityWnd model: shown only for the classes CCombatAbilityWnd::AboutToShow accepts; a drag from its
// "Discipline" row n (eq.PickUp("CombatAbilityWnd", "Discipline", n)) makes the discipline's hotbutton.
#include <algorithm>
#include <cstdio>
#include <ctime>

#include <lua.hpp>

#include "app/actions_ui.h"
#include "app/lua_eq_values.h"
#include "app/lua_eq_windows.h"
#include "app/spell_ui.h"
#include "game/combat_ability_menu.h"
#include "game/mechanics/character.h"
#include "game/hotbuttons.h"
#include "game/mechanics/abilities.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

EqHost& host(lua_State* L) { return *static_cast<EqHost*>(lua_touserdata(L, lua_upvalueindex(1))); }

void set_int(lua_State* L, const char* k, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, k);
}
void set_str(lua_State* L, const char* k, const std::string& v) {
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, k);
}

// The profile's combat abilities (PcBase::GetCombatAbility: 300 slots, 0 empty).
constexpr int kCombatAbilities = 300;

// pinstCCombatSkillsSelectWnd shown: the combat ability window's "S" shows it, its AboutToHide hides it.
bool& combat_skill_select_open() {
    static bool open = false;
    return open;
}

// CCombatSkillsSelectWnd::ShouldDisplayThisSkill @0x4772a0: an ability is listed unless another of the character's
// abilities is a higher rank (+0x1dc) of the same spell group (+0x1d4).
bool should_display(const net::GameConnection& c, int n) {
    const game::SpellData* s = c.spell_table().get(c.combat_ability(n));
    if (!s) return true;
    for (int k = 0; k < kCombatAbilities; ++k) {
        if (k == n) continue;
        const game::SpellData* o = c.spell_table().get(c.combat_ability(k));
        if (o && s->spell_group == o->spell_group && s->spell_rank < o->spell_rank) return false;
    }
    return true;
}

// The discipline's hotbutton (CCombatSkillsSelectWnd's Make Hotkey): type 10, value 0x84 + n (DoHotButton runs
// PcZoneClient::doCombatAbility(GetCombatAbility(value - 0x84), 1)), labelled with the spell's name.
std::optional<game::HotButton> discipline_button(const net::GameConnection& c, int n) {
    const game::SpellData* s = n >= 0 && n < kCombatAbilities ? c.spell_table().get(c.combat_ability(n)) : nullptr;
    if (!s) return std::nullopt;
    game::HotButton b;
    b.type = game::HotButtonType::CombatAbility;
    b.slot = 0x84 + n;
    b.label = s->name.substr(0, 63);
    return b;
}

int Disciplines(lua_State* L) {
    auto& h = host(L);
    lua_newtable(L);
    if (!h.conn) return 1;
    const auto& c = *h.conn;
    const std::uint32_t cls = c.visible_class();
    const auto now = static_cast<std::uint32_t>(std::time(nullptr));
    int out = 0;
    for (int n = 0; n < kCombatAbilities; ++n) {
        const std::uint32_t id = c.combat_ability(n);
        const game::SpellData* s = id ? c.spell_table().get(id) : nullptr;
        if (!s || !should_display(c, n)) continue;
        lua_createtable(L, 0, 10);
        set_int(L, "index", n);
        set_int(L, "spell", id);
        set_str(L, "name", s->name);
        set_int(L, "icon", s->spell_icon);
        set_int(L, "level", c.spell_level_needed(*s, cls));
        set_int(L, "endurance", s->endurance_cost);
        set_int(L, "mana", s->mana);
        // CCombatAbilityWnd_UpdateAbilityTimers: GetDisciplineReuseTimer(+0x198) against the recast time (+0x18;
        // the focus reuse modifier is not applied here).
        set_int(L, "reuse", s->recast_time / 1000);
        const std::int32_t timer = c.combat_ability_timer(*s);
        set_int(L, "timer", timer);
        const std::uint32_t r = c.discipline_reuse_timer(timer);
        const std::uint32_t left = r > now ? r - now : 0;
        set_int(L, "remaining", left);
        lua_rawseti(L, -2, ++out);
    }
    return 1;
}

int UsableSkills(lua_State* L) {
    auto& h = host(L);
    lua_newtable(L);
    if (!h.conn || !h.character) return 1;
    const auto now = static_cast<std::uint32_t>(net::GameConnection::now_ms());
    int out = 0;
    for (bool combat : {true, false})
        for (std::uint32_t s : game::mechanics::usable_skills(*h.character, combat)) {
            lua_createtable(L, 0, 4);
            set_int(L, "id", s);
            set_str(L, "name", h.conn->skill_name(s));
            lua_pushboolean(L, combat);
            lua_setfield(L, -2, "combat");
            std::uint32_t left = h.actions ? h.actions->skill_state().timers.remaining(*h.character, s, now) : 0;
            set_int(L, "remaining", (left + 999) / 1000);
            lua_rawseti(L, -2, ++out);
        }
    return 1;
}

int UseSkill(lua_State* L) {
    auto& h = host(L);
    lua_Integer id = luaL_checkinteger(L, 1);
    bool ok = false;
    if (h.conn && h.character && h.in_game && h.run_button && id >= 0 && id < 100) {
        const auto skill = static_cast<std::uint32_t>(id);
        for (bool combat : {true, false})
            for (std::uint32_t s : game::mechanics::usable_skills(*h.character, combat)) ok = ok || s == skill;
        if (ok) {
            game::HotButton b;
            b.type = game::HotButtonType::Skill;
            b.slot = static_cast<std::int32_t>(skill);
            b.label = h.conn->skill_name(skill).substr(0, 63);
            h.run_button(b);
        }
    }
    lua_pushboolean(L, ok);
    return 1;
}

// CCombatAbilityWnd @0x65b3c0 ("CombatAbilityWnd"): CMD_TOGGLE_COMBAT_ABILITY_WIN shows and hides it; AboutToShow
// refuses unless PcClient_IsCombatAbilityClass (Warrior, Paladin, Ranger, Shadow Knight, Monk, Bard, Rogue,
// Beastlord, Berserker). Eight buttons hold abilities ([CombatAbilities], HotButtonStore::combat_abilities):
//  - CCombatAbilityWnd_UpdateAbilityTimers @0x65a0a0 (every 200 ms while shown; here every frame): a button's text
//    and tooltip are its ability's name; CButtonWnd::SetCoolDown(ready - reuse, reuse) (vtable +0x184; the ready
//    time GetDisciplineReuseTimer(+0x198) as EQ time, the reuse the recast +0x18 less the focus reuse modifier,
//    CharacterZoneClient::GetFocusReuseMod) and SetCheck(while cooling down) (+0x170); an empty button has no
//    text and no cool-down;
//  - CCombatAbilityWnd_UpdateDisciplineTimer @0x659f20: CAW_CombatEffectLabel names the active discipline (the
//    effect in the discipline slot, GetActiveDisciplineEffect) or "No Effect" (6815); CAW_CombatEffectButton shows
//    its icon (A_SpellIcons) only while one is active;
//  - WndNotification @0x65a2e0: the effect button's click removes the discipline (Cmd_stopdisc's RemoveMyAffect),
//    CAW_OpenCombatSkillSelectWnd shows CombatSkillSelectWnd; a button's left click with a combat ability on the
//    cursor (attachment type 12) puts it there (the button's old one goes on the cursor), else uses the button
//    (0x659c80); a drag takes the ability off; a right click pops up the ability menu (built by
//    CCombatAbilityWnd_RebuildAbilityMenus, game/combat_ability_menu.h) at the button's centre, whose item puts
//    10000 + n as 0x84 + n on the button (SaveCombatAbilitiesToIni).
// A drag from the "Discipline" row n (eq.PickUp("CombatAbilityWnd", "Discipline", n)) makes the discipline's
// hotbutton (the gamepad pack's list).
class CombatAbilityWindow : public StockWindow {
public:
    CombatAbilityWindow() : StockWindow("CombatAbilityWnd", false) {}

    void set_shown(EqHost& host, bool show) override {
        if (show && !(host.conn && combat_ability_class(host.conn->visible_class()))) return;
        open_ = show;
        // AboutToHide: the combat skill list closes with it.
        if (!show) combat_skill_select_open() = false;
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        auto& c = *host.conn;
        if (id == "CAW_CombatEffectTimeRemainingGauge") {
            const auto gauge = eq_gauge(host, 26);
            out.has_gauge = true;
            out.gauge = gauge.value;
            out.gauge_text = gauge.text;
            return;
        }
        if (id == "CAW_CombatEffectLabel" || id == "CAW_CombatEffectButton") {
            const auto& b = c.spell_state().buffs[game::kBuffSlots - 1];
            const game::SpellData* s = c.discipline_active() ? c.spell_table().get(b.spell) : nullptr;
            if (id == "CAW_CombatEffectLabel") {
                out.text = s ? s->name : c.strings().format(6815, {}); // No Effect
                if (auto t = c.server_ext().combat_effect_label(c)) out.text = *t;
                return;
            }
            out.shown = s != nullptr;
            out.decal.kind = ChildArt::None;
            if (s && host.textures)
                if (auto cell = host.textures->spell_icon(s->spell_icon)) {
                    out.decal.kind = ChildArt::Image;
                    out.decal.cell = *cell;
                }
            return;
        }
        const int n = button_of(id);
        if (n < 0 || !host.hot_buttons) return;
        const game::SpellData* s = ability(host, n);
        out.text = s ? s->name : std::string();
        out.tooltip = s ? s->name : std::string();
        out.checked = false;
        if (!s) return;
        const std::uint32_t ready = c.discipline_reuse_timer(c.combat_ability_timer(*s)); // FastTime seconds
        const auto fast_now = static_cast<std::uint32_t>(std::time(nullptr));
        if (ready <= fast_now) return;
        out.checked = true;
        out.cool_down_left = static_cast<std::uint64_t>(ready - fast_now) * 1000;
        const std::int32_t focus =
            host.character && !c.server_ext().reuse_focus_dropped(c, *s) ? host.character->focus_reuse_mod(*s) : 0;
        out.cool_down_duration = static_cast<std::uint64_t>(
            std::max<std::int64_t>(static_cast<std::int64_t>(s->recast_time) - focus, 0));
        // Readiness belongs to the server's shared timer. Inferring begin = ready - this ability's reuse can
        // put begin in the future for a shorter-reuse ability and make SetCoolDown's unsigned test look ready.
        // Keep the overlay full while the shared wait exceeds this ability's reuse (also when reuse is zero).
        const auto duration = std::max(out.cool_down_duration, out.cool_down_left);
        out.cool_down = std::min(1.0 - static_cast<double>(out.cool_down_left) / static_cast<double>(duration), 0.87);
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool outside) override {
        if (!host.conn || !host.actions || outside) return false;
        auto& c = *host.conn;
        if (id == "CAW_CombatEffectButton") {
            if (button == "left") c.stop_discipline();
            return true;
        }
        if (id == "CAW_OpenCombatSkillSelectWnd") {
            if (button == "left") combat_skill_select_open() = true;
            return true;
        }
        const int n = button_of(id);
        if (n < 0) return false;
        if (button == "left") {
            const bool held = host.cursor_button && host.cursor_button->type == game::HotButtonType::CombatAbility;
            if (!held) {
                host.actions->use_combat_ability_button(c, n);
                return true;
            }
            // The cursor's ability goes on the button; the button's old one onto the cursor (none: cleared).
            const std::int32_t old = host.hot_buttons->combat_abilities[static_cast<std::size_t>(n)];
            host.actions->set_combat_ability_button(n, host.cursor_button->slot);
            host.cursor_button.reset();
            if (old > 0)
                if (auto b = discipline_button(c, old - game::kCombatAbilityValueBase)) host.cursor_button = *b;
            ++host.cursor_serial;
            return true;
        }
        if (button == "rightup") {
            const auto menu = ability_menu(c);
            if (menu.empty()) return true; // no menu was built
            const auto& r = host.click_rect;
            std::vector<EqHost::MenuRow> rows;
            for (const auto& cat : menu) {
                EqHost::MenuRow row;
                row.text = cat.name;
                row.submenu = [&host, cat, n] {
                    std::vector<EqHost::MenuRow> subs;
                    for (const auto& sub : cat.children) {
                        EqHost::MenuRow sr;
                        sr.text = sub.name;
                        sr.submenu = [&host, sub, n] {
                            std::vector<EqHost::MenuRow> items;
                            for (const auto& it : sub.items) {
                                EqHost::MenuRow ir;
                                ir.text = it.text;
                                const std::int32_t value = it.index + game::kCombatAbilityValueBase;
                                ir.act = [&host, n, value] {
                                    if (host.actions) host.actions->set_combat_ability_button(n, value);
                                };
                                items.push_back(std::move(ir));
                            }
                            return items;
                        };
                        subs.push_back(std::move(sr));
                    }
                    return subs;
                };
                rows.push_back(std::move(row));
            }
            host.open_menu((r[0] + r[2]) / 2, (r[1] + r[3]) / 2, std::move(rows));
            return true;
        }
        return button == "right" || button.starts_with("right");
    }

    std::optional<game::HotButton> pick_up(EqHost& host, std::string_view id, int row) override {
        if (!host.conn) return std::nullopt;
        if (id == "Discipline") return discipline_button(*host.conn, row);
        // The drag off a button (notification 0x17): its ability goes on the cursor and the button empties.
        const int n = button_of(id);
        if (n < 0 || !host.actions || !host.hot_buttons) return std::nullopt;
        const std::int32_t value = host.hot_buttons->combat_abilities[static_cast<std::size_t>(n)];
        auto b = value > 0 ? discipline_button(*host.conn, value - game::kCombatAbilityValueBase) : std::nullopt;
        if (b) host.actions->set_combat_ability_button(n, -1);
        return b;
    }

private:
    static bool combat_ability_class(std::uint32_t c) {
        return c == 1 || c == 3 || c == 4 || c == 5 || c == 7 || c == 8 || c == 9 || c == 15 || c == 16;
    }
    // "CAW_Button1".."CAW_Button8" -> 0..7, else -1.
    static int button_of(std::string_view id) {
        constexpr std::string_view kPrefix = "CAW_Button";
        if (!id.starts_with(kPrefix) || id.size() != kPrefix.size() + 1) return -1;
        const int n = id.back() - '1';
        return n >= 0 && n < game::kCombatAbilityButtons ? n : -1;
    }
    // The button's ability (GetCombatAbility(value - 0x84)), null when it holds none.
    static const game::SpellData* ability(EqHost& host, int n) {
        const std::int32_t value = host.hot_buttons->combat_abilities[static_cast<std::size_t>(n)];
        if (value <= 0) return nullptr;
        return host.conn->spell_table().get(host.conn->combat_ability(value - game::kCombatAbilityValueBase));
    }
    static std::vector<game::CombatAbilityMenuGroup> ability_menu(const net::GameConnection& c) {
        std::vector<game::CombatAbilityEntry> list;
        const std::uint32_t cls = c.visible_class();
        for (int n = 0; n < kCombatAbilities; ++n) {
            const std::uint32_t id = c.combat_ability(n);
            const game::SpellData* s = id ? c.spell_table().get(id) : nullptr;
            if (!s) continue;
            list.push_back({n, s->category, s->subcategory, s->subcategory2, c.spell_level_needed(*s, cls), s->name});
        }
        return game::build_combat_ability_menu(
            list, [&c](std::int32_t id) { return c.strings().db_string(static_cast<std::uint32_t>(id), 5); });
    }
};

// CCombatSkillsSelectWnd @0x4771d0 ("CombatSkillSelectWnd"), opened by the combat ability window's "S": every
// combat ability ShouldDisplayThisSkill lets through, one line each (0x4774f0): Name, Level (GetSpellLevelNeeded
// of the class), Cost (+0x194 endurance), Upkeep (+0x19c), Target (6816 Self for target type 6, 6817 Group for 41,
// else 6818 Other), Duration (GetSpellDuration at the character's level, ticks * 6 as "%d:%02d:%02d"), Reuse
// (recast / 1000 the same way) and Timer (+0x198); the line's data 0x84 + n. A click selects the line (the
// description in CSSW_SkillDescriptionStmlBox, GetSpellDescriptionText) and enables Make Hotkey; Make Hotkey (a
// double click, notification 4, does the same) puts the ability on the cursor (attachment type 12; here its type 10
// hotbutton). The column sorts (notification 0x10) are not modelled.
class CombatSkillSelectWindow : public StockWindow {
public:
    CombatSkillSelectWindow() : StockWindow("CombatSkillSelectWnd", false) {}

    bool shown(EqHost& host) override { return host.in_game && combat_skill_select_open(); }
    void set_shown(EqHost&, bool show) override {
        combat_skill_select_open() = show;
        if (!show) selected_ = -1;
    }
    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn || id != "CSSW_SkillSelectorList") return nullptr;
        const auto& c = *host.conn;
        list_ = {};
        list_.columns = 8;
        list_.labels = {"Name", "Level", "Cost", "Upkeep", "Target", "Duration", "Reuse", "Timer"};
        list_.widths = {130, 50, 50, 50, 50, 50, 50, 50};
        rows_.clear();
        const std::uint32_t cls = c.visible_class();
        auto hms = [](std::int32_t sec) {
            char t[32];
            std::snprintf(t, sizeof t, "%d:%02d:%02d", sec / 3600, (sec - sec / 3600 * 3600) / 60, sec - sec / 60 * 60);
            return std::string(t);
        };
        for (int n = 0; n < kCombatAbilities; ++n) {
            const game::SpellData* s = c.spell_table().get(c.combat_ability(n));
            if (!s || !should_display(c, n)) continue;
            ListRow r;
            const std::uint32_t target = s->target_type == 6 ? 6816 : s->target_type == 41 ? 6817 : 6818;
            const std::int32_t ticks = host.character ? host.character->spell_duration(*s, c.player_status().level) : 0;
            r.texts = {s->name,
                       std::to_string(c.spell_level_needed(*s, cls)),
                       std::to_string(s->endurance_cost),
                       std::to_string(s->endurance_upkeep),
                       c.strings().format(target, {}),
                       hms(ticks * 6),
                       hms(static_cast<std::int32_t>(s->recast_time / 1000)),
                       std::to_string(c.combat_ability_timer(*s))};
            if (n == selected_) list_.selected = static_cast<int>(list_.rows.size());
            list_.rows.push_back(std::move(r));
            rows_.push_back(n);
        }
        return &list_;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool right, int) override {
        if (id != "CSSW_SkillSelectorList" || right || row < 0 || row >= static_cast<int>(rows_.size())) return false;
        (void)host;
        selected_ = rows_[static_cast<std::size_t>(row)];
        return true;
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (id == "CSSW_MakeHotkeyButton") {
            out.enabled = selected_ >= 0;
        } else if (id == "CSSW_SkillDescriptionStmlBox" && host.conn) {
            const game::SpellData* s = selected_ >= 0 ? host.conn->spell_table().get(host.conn->combat_ability(selected_))
                                                      : nullptr;
            out.text = s ? stml_escape(spell_description(*s, host.conn->strings(),
                                                         description_player(*host.conn, host.character)))
                         : std::string();
        }
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (button != "left" || id != "CSSW_MakeHotkeyButton") return false;
        make_hotkey(host);
        return true;
    }

private:
    void make_hotkey(EqHost& host) {
        if (!host.conn || selected_ < 0) return;
        if (auto b = discipline_button(*host.conn, selected_)) {
            host.cursor_button = *b;
            host.cursor_spell.reset();
            ++host.cursor_serial;
        }
    }
    ListData list_;
    std::vector<int> rows_;
    int selected_ = -1;
};

} // namespace

bool combat_ability_listed(const net::GameConnection& c, int n) { return should_display(c, n); }
std::optional<game::HotButton> combat_ability_button(const net::GameConnection& c, int n) {
    return discipline_button(c, n);
}

void add_ability_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<CombatAbilityWindow>());
    out.push_back(std::make_unique<CombatSkillSelectWindow>());
}

void register_abilities_api(lua_State* L, EqHost& h) {
    static const luaL_Reg fns[] = {{"Disciplines", Disciplines}, {"UsableSkills", UsableSkills},
                                   {"UseSkill", UseSkill}, {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h);
    luaL_setfuncs(L, fns, 1);
}

} // namespace hailstone::app
