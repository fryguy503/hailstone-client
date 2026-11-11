// The Ability Book (Hailstone's own window, game/ability_book.h; ui/<name>/abilitybook.lua): the activated AAs,
// the combat abilities and the activated skills in one list, and Autoskill.
//
//   list = eq.AbilityBook()         -- every ability: { key, kind ("aa" | "disc" | "skill"), id, name, icon (spell
//                                      icon, -1 none), spell (0 none), classes (bit c-1; 0 every class), level, rank,
//                                      maxRank, endurance, mana, timerKind ("aa" | "disc" | "skill"), timer (-1
//                                      none), reuse (s), remaining (s), ready, description (AAs and skills;
//                                      a discipline's is eq.SpellDetails(spell)), combat (a combat skill), autoable,
//                                      beneficial (usable outside a fight), buff (leaves a buff), buffOn (it is on
//                                      the player), favorite }; passive AAs are not listed: nothing in the book is
//                                      one the player cannot use
//   st = eq.AbilityBookState()      -- { classes = { {id, name} ... } (the player's), autoskill = { on, allowed,
//                                      held (a script holds it off: hailstone.hold_autoskill), list = { {key, endurance, mana, targetHp, targetHpAbove, selfHp, when
//                                      ("fighting" | "always" | "resting"), keepBuff} ... } } }
//   ok = eq.AbilityBookUse(key)     -- uses it as its hot button does
//   eq.AbilityBookFavorite(key, on)
//   eq.AbilityBookAutoskill(key, on)            -- on or off the autoskill list (at its end)
//   eq.AbilityBookAutoskillMove(key, delta)     -- up (-1) or down (+1) the list
//   eq.AbilityBookAutoskillRule(key, endurance, mana, targetHp, targetHpAbove, selfHp, when, keepBuff)
//   eq.AbilityBookAutoskillEnable(on)           -- the master switch (kept per character)
//
// The window "AbilityBookWnd": eq.PickUp("AbilityBookWnd", key) puts the ability's hotbutton on the cursor;
// eq.ClickChild("AbilityBookWnd", "AutoskillOn" | "AutoskillOff" | "AutoskillToggle") is /abilitybook auto. Its
// frame runs Autoskill. The favourites, the autoskill list and the master switch are kept per character in
// hotbuttons.ini [AbilityBook].
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <ctime>
#include <format>

#include <lua.hpp>

#include "app/actions_ui.h"
#include "core/ascii_case.h"
#include "core/distribution.h"
#include "core/log.h"
#include "app/lua_eq_windows.h"
#include "game/ability_book.h"
#include "game/alt_adv.h"
#include "game/eq_names.h"
#include "game/hot_button_press.h"
#include "game/hotbuttons.h"
#include "game/mechanics/abilities.h"
#include "game/mechanics/character.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

namespace mech = game::mechanics;
using game::AbilityKind;

constexpr int kCombatAbilities = 300; // PcBase::GetCombatAbility's slots
constexpr std::string_view kSection = "AbilityBook";
constexpr std::uint8_t kFeignState = 115;

struct Entry {
    std::string key;
    AbilityKind kind = AbilityKind::Skill;
    std::int32_t id = 0; // AA group, discipline profile index, skill id
    std::string name;
    std::int32_t icon = -1;
    std::uint32_t spell = 0;
    std::uint32_t classes = 0; // bit c-1; 0 every class
    std::int32_t level = 0, rank = 0, max_rank = 0, endurance = 0, mana = 0;
    std::int32_t timer = -1, reuse = 0, remaining = 0;
    bool passive = false, combat = false;
    bool beneficial = false; // Autoskill may use it outside a fight (game::AutoskillWhen)
    bool buff = false;       // its spell leaves a buff with a duration (Autoskill's keep_buff)
    bool buff_on = false;    // that buff is on the player
    std::string description;
    game::HotButton button;

    bool ready() const { return !passive && remaining <= 0; }
    bool autoable() const { return !passive && (kind != AbilityKind::Skill || combat || beneficial); }
};

// The per-character state: the favourites and autoskill list (hotbuttons.ini [AbilityBook]), the master switch.
struct BookState {
    std::filesystem::path loaded; // the hotbuttons.ini they came from
    std::vector<std::string> favorites;
    std::vector<game::AutoskillRule> rules;
    bool on = false;
    game::Autoskill autoskill;
};
BookState& state() {
    static BookState s;
    return s;
}

EqHost& host(lua_State* L) { return *static_cast<EqHost*>(lua_touserdata(L, lua_upvalueindex(1))); }

// Reads the character's lists and master switch when the character changed (a new hotbuttons.ini).
void sync(EqHost& h) {
    auto& st = state();
    if (!h.actions) return;
    const auto& path = h.actions->char_ini_path();
    if (path == st.loaded) return;
    st.loaded = path;
    st.on = false;
    st.autoskill.reset();
    st.favorites.clear();
    const std::string fav = h.actions->char_ini_string(kSection, "Favorites");
    for (const auto& r : game::parse_autoskill_rules(fav)) st.favorites.push_back(r.key);
    st.rules = game::parse_autoskill_rules(h.actions->char_ini_string(kSection, "Autoskill"));
    // Hailstone's own: the switch is as the player left it. frame() still turns it off where the server forbids
    // autoskill, without saving that.
    st.on = !distribution::managed() && h.actions->char_ini_int(kSection, "AutoskillOn", 0) != 0;
    if (st.on) {
        log::info("autoskill on from the last session ({} on the list)", st.rules.size());
        if (h.conn) h.conn->show_chat("Autoskill is on.", 15);
    }
}

void save(EqHost& h) {
    auto& st = state();
    if (!h.actions || h.actions->char_ini_path().empty()) return;
    std::string fav;
    for (const auto& k : st.favorites) fav += (fav.empty() ? "" : ",") + k;
    h.actions->write_char_ini(kSection, "Favorites", fav);
    h.actions->write_char_ini(kSection, "Autoskill", game::format_autoskill_rules(st.rules));
}

bool allowed(const EqHost& h) {
    if (distribution::managed()) return false;
    const auto& rules = h.conn ? h.conn->server_rules() : std::nullopt;
    return !rules || !rules->configured || rules->autoskill;
}

std::uint32_t spell_classes(const game::SpellData& s) {
    std::uint32_t m = 0;
    for (std::uint32_t c = 0; c < 16; ++c)
        if (s.class_level[c] != 255) m |= 1u << c;
    return m;
}

void fill_spell(const net::GameConnection& c, Entry& e, std::uint32_t spell) {
    const game::SpellData* s = spell ? c.spell_table().get(spell) : nullptr;
    if (!s) return;
    e.spell = spell;
    e.icon = s->spell_icon;
    e.endurance = static_cast<std::int32_t>(s->endurance_cost);
    e.mana = static_cast<std::int32_t>(s->mana);
    e.beneficial = s->beneficial();
    // The buff checked is the ability's own spell; an ability whose spell only triggers another (the buff then
    // has the other's id) is never seen as on, so keep_buff waits on its reuse timer alone (docs/unknowns.md).
    e.buff = e.beneficial && (s->duration_formula != 0 || s->duration_cap != 0);
    if (e.buff)
        for (const auto& b : c.spell_state().buffs) e.buff_on = e.buff_on || (!b.empty() && b.spell == spell);
}

// Activated skills Autoskill may use outside a fight: they act on the player alone (Hailstone's own list).
bool beneficial_skill(std::uint32_t skill) {
    namespace sk = mech::skill;
    switch (skill) {
    case sk::kForage: case sk::kMend: case sk::kSenseHeading: case sk::kSenseTraps: case sk::kFishing:
    case sk::kHide: case sk::kSneak:
        return true;
    default: return false;
    }
}

// An owned AA's name as listed: its rank's title, else (an activated one) its spell's, else "Ability <group>".
std::string aa_entry_name(const net::GameConnection& c, const game::AltAbility& a) {
    std::string name = c.aa_name(a);
    if (name.empty() && a.activated())
        if (const game::SpellData* s = c.spell_table().get(static_cast<std::uint32_t>(a.spell))) name = s->name;
    if (name.empty()) name = std::format("Ability {}", a.group);
    return name;
}

// An owned AA as an entry: its owned rank; its reuse from AltAdvManager::IsAbilityReady.
std::optional<Entry> aa_entry(const net::GameConnection& c, const game::AltAbility& a) {
    if (a.group <= 0) return std::nullopt;
    Entry e;
    e.kind = AbilityKind::AltAbility;
    e.id = a.group;
    e.key = game::ability_key(AbilityKind::AltAbility, a.group);
    e.name = aa_entry_name(c, a);
    e.description = c.aa_description(a);
    // AltAbility_IsClassAllowed: bit (1 << class). An NMS multiclass server sends 0xFFFFFFF for every AA one of
    // the player's classes may use (zone/aa.cpp), so there each counts as every class's (docs/unknowns.md).
    e.classes = (a.classes >> 1) & 0xffffu;
    if (e.classes == 0xffffu) e.classes = 0;
    e.level = a.level_req;
    e.rank = a.rank;
    e.max_rank = a.max_rank;
    e.passive = !a.activated();
    if (!e.passive) {
        fill_spell(c, e, static_cast<std::uint32_t>(a.spell));
        e.timer = a.timers.empty() ? -1 : a.timers.front();
        const game::AAReadiness r = c.aa_readiness(a.id);
        e.reuse = r.total > 0 ? r.total : a.refresh;
        e.remaining = r.ready ? 0 : std::max(1, r.remaining);
        e.button.type = game::HotButtonType::Social; // the AA window's hotkey: DoHotButton queues /alt activate
        e.button.slot = game::kSocialCount + a.group;
    }
    if (!e.passive) e.button.label = e.name.substr(0, 63);
    return e;
}

std::optional<Entry> discipline_entry(const net::GameConnection& c, int n) {
    const std::uint32_t id = c.combat_ability(n);
    const game::SpellData* s = id ? c.spell_table().get(id) : nullptr;
    if (!s) return std::nullopt;
    Entry e;
    e.kind = AbilityKind::Discipline;
    e.id = n;
    e.key = game::discipline_key(s->spell_group, id);
    e.name = s->name;
    fill_spell(c, e, id);
    e.classes = spell_classes(*s);
    e.level = c.spell_level_needed(*s, c.visible_class());
    e.rank = s->spell_rank;
    e.timer = c.combat_ability_timer(*s);
    e.reuse = static_cast<std::int32_t>(s->recast_time / 1000);
    const std::uint32_t r = c.discipline_reuse_timer(e.timer);
    const auto now = static_cast<std::uint32_t>(std::time(nullptr));
    e.remaining = r > now ? static_cast<std::int32_t>(r - now) : 0;
    if (auto b = combat_ability_button(c, n)) e.button = *b;
    return e;
}

// The player's classes that give a skill: those whose skill cap at the player's level is above 0 (the cap table
// alone, CSkillMgr::GetSkillCap without the character's items, AAs and buffs). 0 when none does (a racial skill).
std::uint32_t skill_classes(const EqHost& h, std::uint32_t skill) {
    const std::uint32_t mask = h.conn->player_class_mask();
    const auto level = static_cast<std::int32_t>(h.character->sheet().level);
    std::uint32_t m = 0;
    for (std::uint32_t c = 1; c <= 16; ++c)
        if ((mask & (1u << (c - 1))) &&
            h.character->skill_cap(level, static_cast<std::int32_t>(c), static_cast<std::int32_t>(skill), false) > 0)
            m |= 1u << (c - 1);
    return m;
}

Entry skill_entry(EqHost& h, std::uint32_t skill, bool combat) {
    Entry e;
    e.kind = AbilityKind::Skill;
    e.id = static_cast<std::int32_t>(skill);
    e.key = game::ability_key(AbilityKind::Skill, e.id);
    e.name = h.conn->skill_name(skill);
    e.classes = skill_classes(h, skill);
    e.beneficial = beneficial_skill(skill);
    e.description = std::string(game::skill_description(skill));
    e.combat = combat;
    e.timer = e.id;
    const auto now = static_cast<std::uint32_t>(net::GameConnection::now_ms());
    const std::uint32_t left = h.actions ? h.actions->skill_state().timers.remaining(*h.character, skill, now) : 0;
    e.remaining = static_cast<std::int32_t>((left + 999) / 1000);
    e.reuse = mech::skill_table_entry(skill).reuse_ms / 1000;
    e.button.type = game::HotButtonType::Skill;
    e.button.slot = e.id;
    e.button.label = e.name.substr(0, 63);
    return e;
}

std::vector<Entry> all_entries(EqHost& h) {
    std::vector<Entry> out;
    if (!h.conn || !h.character) return out;
    const auto& c = *h.conn;
    for (const auto& o : c.alt_adv().owned)
        if (const game::AltAbility* a = o.id ? c.alt_adv().get(o.id) : nullptr)
            if (auto e = aa_entry(c, *a); e && !e->passive) out.push_back(std::move(*e));
    for (int n = 0; n < kCombatAbilities; ++n)
        if (c.combat_ability(n) && combat_ability_listed(c, n))
            if (auto e = discipline_entry(c, n)) out.push_back(std::move(*e));
    for (bool combat : {true, false})
        for (std::uint32_t s : mech::usable_skills(*h.character, combat)) out.push_back(skill_entry(h, s, combat));
    return out;
}

// One ability by key (the autoskill's check, without building the whole list).
std::optional<Entry> find(EqHost& h, const std::string& key) {
    auto p = game::parse_ability_key(key);
    if (!p || !h.conn || !h.character) return std::nullopt;
    const auto& c = *h.conn;
    switch (p->kind) {
    case AbilityKind::AltAbility:
        if (const game::AltAbility* a = c.alt_adv().owned_rank(p->id)) return aa_entry(c, *a);
        return std::nullopt;
    case AbilityKind::Discipline:
        for (int n = 0; n < kCombatAbilities; ++n) {
            const std::uint32_t id = c.combat_ability(n);
            const game::SpellData* s = id ? c.spell_table().get(id) : nullptr;
            if (!s || !combat_ability_listed(c, n)) continue;
            if (p->spell_id ? id == static_cast<std::uint32_t>(p->id) : s->spell_group == p->id)
                return discipline_entry(c, n);
        }
        return std::nullopt;
    case AbilityKind::Skill:
        for (bool combat : {true, false})
            for (std::uint32_t s : mech::usable_skills(*h.character, combat))
                if (s == static_cast<std::uint32_t>(p->id)) return skill_entry(h, s, combat);
        return std::nullopt;
    }
    return std::nullopt;
}

bool use(EqHost& h, const Entry& e) {
    if (!h.in_game || !h.run_button || e.button.empty()) return false;
    h.run_button(e.button);
    return true;
}

game::AutoskillWorld world(EqHost& h) {
    game::AutoskillWorld w;
    w.now_ms = static_cast<std::uint32_t>(net::GameConnection::now_ms());
    if (!h.conn) return w;
    const auto& c = *h.conn;
    const auto& ps = c.player_status();
    w.in_game = h.in_game;
    w.dead = ps.dead;
    w.sitting = c.sitting();
    w.feigned = ps.stand_state == kFeignState;
    w.stunned = c.stunned();
    w.casting = c.spell_state().casting.casting();
    w.invisible = c.invisible();
    w.auto_attack = c.auto_attack();
    const auto& spawns = c.world().spawns;
    if (auto t = spawns.find(c.world().target_id); t != spawns.end()) {
        w.target_npc = t->second.is_npc && !t->second.is_corpse && t->second.hp_percent > 0;
        w.target_hp_pct = t->second.hp_percent;
    }
    w.target_in_range = w.target_npc && h.target_in_melee_range && h.target_in_melee_range();
    auto pct = [](std::uint64_t cur, std::uint64_t max) { return max ? static_cast<int>(cur * 100 / max) : 100; };
    w.endurance_pct = pct(ps.endurance, ps.max_endurance);
    w.mana_pct = pct(ps.mana, ps.max_mana);
    const auto own = c.own_hp(); // the items' HP included, as the player window's
    w.hp_pct = own.max > 0 ? static_cast<int>(std::max<std::int64_t>(own.cur, 0) * 100 / own.max) : 100;
    w.in_combat = c.in_combat();
    return w;
}

// `remember`: the player's choice, saved for the next login (not when the server's rules turn it off).
void set_enabled(EqHost& h, bool on, bool remember = true) {
    auto& st = state();
    if (on && !allowed(h)) {
        if (h.conn) h.conn->show_chat(distribution::managed() ? "Autoskill is disabled in this distribution."
                                                           : "This server does not allow autoskill.", 13);
        on = false;
        remember = false;
    }
    if (remember && h.actions && !h.actions->char_ini_path().empty())
        h.actions->write_char_ini(kSection, "AutoskillOn", on ? "1" : "0");
    if (st.on == on) return;
    st.on = on;
    log::info("autoskill {} ({} on the list)", on ? "on" : "off", st.rules.size());
    st.autoskill.reset();
    if (h.conn)
        h.conn->show_chat(on ? (st.rules.empty() ? "Autoskill is on, but its list is empty: add abilities in the "
                                                   "Ability Book."
                                                 : "Autoskill is on.")
                             : "Autoskill is off.",
                          15);
}

class AbilityBookWindow : public StockWindow {
public:
    AbilityBookWindow() : StockWindow("AbilityBookWnd", false) {}

    bool click(EqHost& h, std::string_view id, std::string_view button, bool) override {
        if (button != "left") return false;
        sync(h);
        if (id == "AutoskillOn") set_enabled(h, true);
        else if (id == "AutoskillOff") set_enabled(h, false);
        else if (id == "AutoskillToggle") set_enabled(h, !state().on);
        else return false;
        return true;
    }

    std::optional<game::HotButton> pick_up(EqHost& h, std::string_view id, int) override {
        auto e = find(h, std::string(id));
        if (!e || e->button.empty()) return std::nullopt;
        return e->button;
    }

    // Autoskill: one use at most a frame, through the hot button path.
    void frame(EqHost& h) override {
        sync(h);
        auto& st = state();
        if (!st.on) return;
        if (!allowed(h)) {
            set_enabled(h, false, false);
            return;
        }
        if (st.rules.empty() || (h.autoskill_held && h.autoskill_held())) return;
        const game::AutoskillWorld w = world(h);
        auto query = [&](const std::string& key) -> std::optional<game::AutoskillAbility> {
            auto e = find(h, key);
            if (!e || !e->autoable()) return std::nullopt;
            return game::AutoskillAbility{e->ready(), e->beneficial, e->buff_on};
        };
        if (auto i = st.autoskill.next(st.rules, query, w))
            if (auto chosen = find(h, st.rules[*i].key)) {
                log::info("autoskill: {} ({})", chosen->name, chosen->key);
                use(h, *chosen);
                st.autoskill.used(chosen->key, w.now_ms);
            }
    }
};

void push_entry(lua_State* L, const Entry& e) {
    auto& st = state();
    lua_createtable(L, 0, 24);
    auto str = [L](const char* k, std::string_view v) {
        lua_pushlstring(L, v.data(), v.size());
        lua_setfield(L, -2, k);
    };
    auto num = [L](const char* k, lua_Integer v) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, k);
    };
    auto flag = [L](const char* k, bool v) {
        lua_pushboolean(L, v);
        lua_setfield(L, -2, k);
    };
    static constexpr const char* kKinds[] = {"aa", "disc", "skill"};
    const char* kind = kKinds[static_cast<int>(e.kind)];
    str("key", e.key);
    str("kind", kind);
    num("id", e.id);
    str("name", e.name);
    num("icon", e.icon);
    num("spell", e.spell);
    num("classes", e.classes);
    num("level", e.level);
    num("rank", e.rank);
    num("maxRank", e.max_rank);
    num("endurance", e.endurance);
    num("mana", e.mana);
    str("timerKind", kind);
    num("timer", e.timer);
    num("reuse", e.reuse);
    num("remaining", e.remaining);
    flag("ready", e.ready());
    flag("combat", e.combat);
    flag("autoable", e.autoable());
    flag("beneficial", e.beneficial);
    flag("buff", e.buff);
    flag("buffOn", e.buff_on);
    flag("favorite", std::ranges::find(st.favorites, e.key) != st.favorites.end());
    str("description", e.description);
}

int AbilityBook(lua_State* L) {
    auto& h = host(L);
    sync(h);
    const auto list = all_entries(h);
    lua_createtable(L, static_cast<int>(list.size()), 0);
    int n = 0;
    for (const auto& e : list) {
        push_entry(L, e);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int AbilityBookState(lua_State* L) {
    auto& h = host(L);
    sync(h);
    auto& st = state();
    lua_createtable(L, 0, 2);
    lua_newtable(L);
    const std::uint32_t mask = h.conn ? h.conn->player_class_mask() : 0;
    int n = 0;
    for (std::uint32_t c = 1; c <= 16; ++c) {
        if (!(mask & (1u << (c - 1)))) continue;
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, c);
        lua_setfield(L, -2, "id");
        const std::string name = game::class_name(c);
        lua_pushlstring(L, name.data(), name.size());
        lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, ++n);
    }
    lua_setfield(L, -2, "classes");
    lua_createtable(L, 0, 4);
    lua_pushboolean(L, st.on);
    lua_setfield(L, -2, "on");
    lua_pushboolean(L, allowed(h));
    lua_setfield(L, -2, "allowed");
    lua_pushboolean(L, h.autoskill_held && h.autoskill_held());
    lua_setfield(L, -2, "held");
    lua_createtable(L, static_cast<int>(st.rules.size()), 0);
    n = 0;
    for (const auto& r : st.rules) {
        lua_createtable(L, 0, 8);
        lua_pushlstring(L, r.key.data(), r.key.size());
        lua_setfield(L, -2, "key");
        lua_pushinteger(L, r.min_endurance_pct);
        lua_setfield(L, -2, "endurance");
        lua_pushinteger(L, r.min_mana_pct);
        lua_setfield(L, -2, "mana");
        lua_pushinteger(L, r.target_hp_below);
        lua_setfield(L, -2, "targetHp");
        lua_pushinteger(L, r.target_hp_above);
        lua_setfield(L, -2, "targetHpAbove");
        lua_pushinteger(L, r.self_hp_below);
        lua_setfield(L, -2, "selfHp");
        static constexpr const char* kWhen[] = {"fighting", "always", "resting"};
        lua_pushstring(L, kWhen[static_cast<int>(r.when)]);
        lua_setfield(L, -2, "when");
        lua_pushboolean(L, r.keep_buff);
        lua_setfield(L, -2, "keepBuff");
        lua_rawseti(L, -2, ++n);
    }
    lua_setfield(L, -2, "list");
    lua_setfield(L, -2, "autoskill");
    return 1;
}

int AbilityBookUse(lua_State* L) {
    auto& h = host(L);
    auto e = find(h, luaL_checkstring(L, 1));
    lua_pushboolean(L, e && !e->passive && use(h, *e));
    return 1;
}

int AbilityBookFavorite(lua_State* L) {
    auto& h = host(L);
    sync(h);
    std::string key = luaL_checkstring(L, 1);
    const bool on = lua_toboolean(L, 2);
    auto& f = state().favorites;
    std::erase(f, key);
    if (on && game::parse_ability_key(key)) f.push_back(std::move(key));
    save(h);
    return 0;
}

int AbilityBookAutoskill(lua_State* L) {
    if (distribution::managed()) return 0;
    auto& h = host(L);
    sync(h);
    std::string key = luaL_checkstring(L, 1);
    const bool on = lua_toboolean(L, 2);
    auto& rules = state().rules;
    const bool had = std::erase_if(rules, [&](const auto& r) { return r.key == key; }) > 0;
    if (on) {
        auto e = find(h, key);
        if (e && e->autoable()) rules.push_back({key});
    }
    log::info("autoskill list: {} {} ({} on it)", key, on ? "added" : "removed", rules.size());
    if (had || on) save(h);
    return 0;
}

int AbilityBookAutoskillMove(lua_State* L) {
    if (distribution::managed()) return 0;
    auto& h = host(L);
    sync(h);
    const char* key_arg = luaL_checkstring(L, 1);
    const auto delta = luaL_checkinteger(L, 2);
    const std::string key = key_arg; // (after the checks: a failed one longjmps past destructors)
    auto& rules = state().rules;
    auto it = std::ranges::find_if(rules, [&](const auto& r) { return r.key == key; });
    if (it == rules.end()) return 0;
    const auto from = static_cast<lua_Integer>(it - rules.begin());
    const auto to = std::clamp<lua_Integer>(from + delta, 0, static_cast<lua_Integer>(rules.size()) - 1);
    if (to == from) return 0;
    auto r = std::move(*it);
    rules.erase(it);
    rules.insert(rules.begin() + to, std::move(r));
    save(h);
    return 0;
}

int AbilityBookAutoskillRule(lua_State* L) {
    if (distribution::managed()) return 0;
    auto& h = host(L);
    sync(h);
    // Every argument read before any C++ object exists: a failed check longjmps past destructors.
    const std::string_view key = luaL_checkstring(L, 1);
    std::array<int, 7> pct{};
    for (int i = 2; i <= 6; ++i)
        pct[static_cast<std::size_t>(i)] = static_cast<int>(std::clamp<lua_Integer>(luaL_optinteger(L, i, 0), 0, 100));
    auto when = game::AutoskillWhen::Fighting;
    const std::string_view w = luaL_optstring(L, 7, "fighting");
    if (w == "always") when = game::AutoskillWhen::Always;
    else if (w == "resting") when = game::AutoskillWhen::Resting;
    for (auto& r : state().rules)
        if (r.key == key) {
            r.min_endurance_pct = pct[2];
            r.min_mana_pct = pct[3];
            r.target_hp_below = pct[4];
            r.target_hp_above = pct[5];
            r.self_hp_below = pct[6];
            r.when = when;
            r.keep_buff = lua_toboolean(L, 8);
            save(h);
        }
    return 0;
}

int AbilityBookAutoskillEnable(lua_State* L) {
    auto& h = host(L);
    sync(h);
    set_enabled(h, lua_toboolean(L, 1));
    return 0;
}

// A discipline's name without its rank (" Rk. II"), as the name a script may give for any rank.
std::string_view without_rank(std::string_view name) {
    const auto at = name.rfind(" Rk.");
    return at == std::string_view::npos ? name : name.substr(0, at);
}

bool names(AbilityKind kind, std::int32_t id, std::string_view entry_name, std::string_view name) {
    if (iequals(entry_name, name) || iequals(without_rank(entry_name), name)) return true;
    std::int32_t n = 0;
    const auto [end, ec] = std::from_chars(name.data(), name.data() + name.size(), n);
    return ec == std::errc() && end == name.data() + name.size() && kind != AbilityKind::Discipline && id == n;
}

bool names(const Entry& e, std::string_view name) { return names(e.kind, e.id, e.name, name); }

} // namespace

namespace {
// One of the player's abilities by name, an AA's group or a skill's number (ability_state, use_ability).
std::optional<Entry> by_name(EqHost& h, mq::AbilityKind kind, std::string_view name) {
    if (!h.conn || !h.character || name.empty()) return std::nullopt;
    const auto& c = *h.conn;
    std::optional<Entry> found;
    switch (kind) {
    case mq::AbilityKind::AltAbility:
        for (const auto& o : c.alt_adv().owned) {
            // The name first: an entry's readiness builds the character's effects (aa_readiness), which a
            // script asking every frame cannot afford for each of a few hundred owned AAs.
            const game::AltAbility* a = o.id ? c.alt_adv().get(o.id) : nullptr;
            if (!a || a->group <= 0 || !a->activated() ||
                !names(AbilityKind::AltAbility, a->group, aa_entry_name(c, *a), name))
                continue;
            found = aa_entry(c, *a);
            break;
        }
        break;
    case mq::AbilityKind::Discipline:
        for (int n = 0; n < kCombatAbilities && !found; ++n)
            if (c.combat_ability(n) && combat_ability_listed(c, n))
                if (auto e = discipline_entry(c, n); e && names(*e, name)) found = std::move(e);
        break;
    case mq::AbilityKind::Skill:
        for (bool combat : {true, false}) {
            for (std::uint32_t s : mech::usable_skills(*h.character, combat)) {
                Entry e = skill_entry(h, s, combat);
                if (names(e, name)) {
                    found = std::move(e);
                    break;
                }
            }
            if (found) break;
        }
        break;
    }
    return found;
}
} // namespace

std::optional<mq::AbilityState> ability_state(EqHost& h, mq::AbilityKind kind, std::string_view name) {
    const std::optional<Entry> found = by_name(h, kind, name);
    if (!found) return std::nullopt;
    mq::AbilityState st;
    st.name = found->name;
    st.ready = found->ready();
    st.remaining_ms = std::int64_t{std::max(found->remaining, 0)} * 1000;
    st.reuse_ms = std::int64_t{std::max(found->reuse, 0)} * 1000;
    return st;
}

bool use_ability(EqHost& h, mq::AbilityKind kind, std::string_view name) {
    const std::optional<Entry> e = by_name(h, kind, name);
    if (!e || e->passive) return false;
    log::info("script uses {} ({})", e->name, e->key);
    return use(h, *e);
}

std::vector<std::pair<game::AutoskillRule, std::string>> autoskill_list(EqHost& h) {
    std::vector<std::pair<game::AutoskillRule, std::string>> out;
    if (!h.in_game) return out;
    sync(h);
    for (const auto& r : state().rules) {
        auto e = find(h, r.key);
        out.emplace_back(r, e ? e->name : std::string());
    }
    return out;
}

void add_ability_book_window(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<AbilityBookWindow>());
}

void register_ability_book_api(lua_State* L, EqHost& h) {
    static const luaL_Reg fns[] = {{"AbilityBook", AbilityBook},
                                   {"AbilityBookState", AbilityBookState},
                                   {"AbilityBookUse", AbilityBookUse},
                                   {"AbilityBookFavorite", AbilityBookFavorite},
                                   {"AbilityBookAutoskill", AbilityBookAutoskill},
                                   {"AbilityBookAutoskillMove", AbilityBookAutoskillMove},
                                   {"AbilityBookAutoskillRule", AbilityBookAutoskillRule},
                                   {"AbilityBookAutoskillEnable", AbilityBookAutoskillEnable},
                                   {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h);
    luaL_setfuncs(L, fns, 1);
}

} // namespace hailstone::app
