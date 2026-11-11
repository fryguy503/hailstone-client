// The Advanced Inventory (Hailstone's own window, InventoryManagerWnd; ui/<name>/inventorymanager.lua): every item
// the character has, worn, carried and banked, in one list, and the Keep marks (game/kept_items.h).
//   list = eq.AllItems()         -- { { item, place, where, slot, sub, bag, kept }, ... } in place order: worn, carried
//                                   (the general slots, each bag after its slot), bank, shared bank. item: eq.Item's
//                                   table and value (copper), noTrade, temporary, attuned, reqLevel, recLevel,
//                                   equipSlots (bit n: possessions slot n), tradeskill, augment, usable (the
//                                   character may equip or use it: CharacterZoneClient::CanUseItem(item, 1, 0));
//                                   place: "i:<slot>[:<sub>]" (worn and carried), "b:..." (bank), "s:..." (shared
//                                   bank); where: "worn", "carried", "bank", "shared"; bag: the bag's name, for an
//                                   item in one; kept: marked Keep (or a bag holding a kept item)
//                                   item also has tribute (its favour), noDestroy and stats: { [key] = value }, the
//                                   stats it has (not 0), keyed as eq.ItemStats names them
//   list = eq.ItemStats()        -- { { key, label }, ... }: the stats the window filters and sorts by, in its order
//   count, err = eq.BulkItems(mode, { place, ... } [, dry])
//                                -- the Advanced Inventory's bulk actions (mode "sell", "destroy", "tribute"; places as
//                                   eq.AllItems gives them, only carried ones taken; GameConnection::start_bulk_job):
//                                   count is how many of them the action takes, err why it cannot run (nil: it started,
//                                   or with dry, it could). dry only counts
//   text = eq.TakeItemSearch()   -- /finditem's search, once (nil when there is none)
//   inv, keep = eq.ItemsRevision()  -- change counters: the inventory's and the Keep marks' (a window's cache)
//   on = eq.ItemKept(id)
//   eq.SetItemKept(id, name, on)    -- marks or unmarks every copy of the item (kept per character)
// The bank's and shared bank's items are known from zoning in, so they are listed while the bank is closed; acting on
// them still needs the bank open (eq.BankSlotClick).

#include <charconv>
#include <cmath>
#include <format>
#include <iterator>
#include <optional>
#include <string_view>

#include <lua.hpp>

#include "app/lua_eq.h"
#include "core/distribution.h"
#include "app/lua_eq_windows.h"
#include "net/game_connection.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

namespace {

EqHost& host(lua_State* L) { return *static_cast<EqHost*>(lua_touserdata(L, lua_upvalueindex(1))); }

constexpr int kSharedBankSlots = 2; // BW_SharedBankSlot0..1 (game/bank.h)

// The stats of eq.ItemStats, in the window's order (STR before Heroic STR, the resists after the attributes).
struct StatDef {
    const char* key;
    const char* label;
    double (*get)(const net::Item&);
};
#define HAILSTONE_STAT(key, label, expr) {key, label, [](const net::Item& i) -> double { return static_cast<double>(expr); }}
constexpr StatDef kStats[] = {
    HAILSTONE_STAT("ac", "AC", i.ac),
    HAILSTONE_STAT("hp", "HP", i.hp),
    HAILSTONE_STAT("mana", "Mana", i.mana),
    HAILSTONE_STAT("endurance", "Endurance", i.endurance),
    HAILSTONE_STAT("str", "STR", i.astr),
    HAILSTONE_STAT("sta", "STA", i.asta),
    HAILSTONE_STAT("agi", "AGI", i.aagi),
    HAILSTONE_STAT("dex", "DEX", i.adex),
    HAILSTONE_STAT("wis", "WIS", i.awis),
    HAILSTONE_STAT("int", "INT", i.aint),
    HAILSTONE_STAT("cha", "CHA", i.acha),
    HAILSTONE_STAT("hstr", "Heroic STR", i.heroic_str),
    HAILSTONE_STAT("hsta", "Heroic STA", i.heroic_sta),
    HAILSTONE_STAT("hagi", "Heroic AGI", i.heroic_agi),
    HAILSTONE_STAT("hdex", "Heroic DEX", i.heroic_dex),
    HAILSTONE_STAT("hwis", "Heroic WIS", i.heroic_wis),
    HAILSTONE_STAT("hint", "Heroic INT", i.heroic_int),
    HAILSTONE_STAT("hcha", "Heroic CHA", i.heroic_cha),
    HAILSTONE_STAT("mr", "Magic resist", i.mr),
    HAILSTONE_STAT("fr", "Fire resist", i.fr),
    HAILSTONE_STAT("cr", "Cold resist", i.cr),
    HAILSTONE_STAT("pr", "Poison resist", i.pr),
    HAILSTONE_STAT("dr", "Disease resist", i.dr),
    HAILSTONE_STAT("corrupt", "Corruption resist", i.sv_corruption),
    HAILSTONE_STAT("hmr", "Heroic magic resist", i.heroic_mr),
    HAILSTONE_STAT("hfr", "Heroic fire resist", i.heroic_fr),
    HAILSTONE_STAT("hcr", "Heroic cold resist", i.heroic_cr),
    HAILSTONE_STAT("hpr", "Heroic poison resist", i.heroic_pr),
    HAILSTONE_STAT("hdr", "Heroic disease resist", i.heroic_dr),
    HAILSTONE_STAT("hcorrupt", "Heroic corruption resist", i.heroic_sv_corrup),
    HAILSTONE_STAT("attack", "Attack", i.attack),
    HAILSTONE_STAT("haste", "Haste", i.haste),
    HAILSTONE_STAT("regen", "HP regen", i.regen),
    HAILSTONE_STAT("manaRegen", "Mana regen", i.mana_regen),
    HAILSTONE_STAT("endRegen", "Endurance regen", i.end_regen),
    HAILSTONE_STAT("damageShield", "Damage shield", i.damage_shield),
    HAILSTONE_STAT("dsMitigation", "Damage shield mitigation", i.ds_mitigation),
    HAILSTONE_STAT("shielding", "Shielding", i.shielding),
    HAILSTONE_STAT("spellShield", "Spell shield", i.spell_shield),
    HAILSTONE_STAT("dotShield", "DoT shielding", i.dot_shield),
    HAILSTONE_STAT("avoidance", "Avoidance", i.avoidance),
    HAILSTONE_STAT("accuracy", "Accuracy", i.accuracy),
    HAILSTONE_STAT("strikethrough", "Strikethrough", i.strike_through),
    HAILSTONE_STAT("stunResist", "Stun resist", i.stun_resist),
    HAILSTONE_STAT("combatEffects", "Combat effects", i.combat_effects),
    HAILSTONE_STAT("healAmount", "Heal amount", i.heal_amt),
    HAILSTONE_STAT("spellDamage", "Spell damage", i.spell_dmg),
    HAILSTONE_STAT("clairvoyance", "Clairvoyance", i.clairvoyance),
    HAILSTONE_STAT("damage", "Damage", i.damage),
    HAILSTONE_STAT("delay", "Delay", i.delay),
    // damage per delay, as players compare weapons (two places; arithmetic, not a game rule)
    HAILSTONE_STAT("ratio", "Ratio", i.damage > 0 && i.delay > 0 ? std::round(100.0 * i.damage / i.delay) / 100.0 : 0.0),
    HAILSTONE_STAT("range", "Range", i.range),
    HAILSTONE_STAT("backstab", "Backstab damage", i.backstab_dmg),
    HAILSTONE_STAT("elemDamage", "Elemental damage", i.elem_dmg_amt),
    HAILSTONE_STAT("baneDamage", "Bane damage", i.bane_dmg_amt),
    HAILSTONE_STAT("bagSlots", "Bag slots", i.is_bag() ? i.bag_slots : 0),
    HAILSTONE_STAT("weightReduction", "Weight reduction", i.is_bag() ? i.weight_reduction : 0),
    HAILSTONE_STAT("tribute", "Tribute", i.favor),
};
#undef HAILSTONE_STAT

void set_int(lua_State* L, const char* k, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, k);
}
void set_bool(lua_State* L, const char* k, bool v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, k);
}

// eq.Item's table and what the window filters and sorts by.
void push_full_item(lua_State* L, const net::GameConnection& c, const net::Item& it) {
    push_item(L, &it);
    set_int(L, "value", it.value);
    set_bool(L, "noTrade", it.no_trade());
    set_bool(L, "temporary", it.no_rent == 0);
    set_bool(L, "attuned", it.attuned != 0);
    set_int(L, "reqLevel", it.req_level);
    set_int(L, "recLevel", it.rec_level);
    set_int(L, "equipSlots", it.equip_slots);
    set_bool(L, "tradeskill", it.tradeskills != 0);
    set_bool(L, "augment", it.aug_type != 0 && it.item_type == 54); // ItemType 54: augmentation
    set_bool(L, "usable", c.item_usable(it));
    set_int(L, "tribute", it.favor);
    set_bool(L, "noDestroy", it.no_destroy());
    lua_createtable(L, 0, 4);
    for (const StatDef& d : kStats) {
        double v = d.get(it);
        if (v == 0) continue;
        if (v == std::floor(v)) lua_pushinteger(L, static_cast<lua_Integer>(v));
        else lua_pushnumber(L, v);
        lua_setfield(L, -2, d.key);
    }
    lua_setfield(L, -2, "stats");
}

int AllItems(lua_State* L) {
    auto& h = host(L);
    lua_newtable(L);
    if (!h.conn || !h.in_game) return 1;
    const net::GameConnection& c = *h.conn;
    const auto& inv = c.inventory();
    lua_Integer n = 0;
    auto row = [&](const net::Item& it, const net::ItemSlot& at, const char* where, const net::Item* bag) {
        lua_createtable(L, 0, 7);
        push_full_item(L, c, it);
        lua_setfield(L, -2, "item");
        const char kind = at.type == net::kInvBank ? 'b' : at.type == net::kInvSharedBank ? 's' : 'i';
        std::string place = std::format("{}:{}", kind, at.slot) + (at.sub >= 0 ? std::format(":{}", at.sub) : std::string());
        lua_pushstring(L, place.c_str());
        lua_setfield(L, -2, "place");
        lua_pushstring(L, where);
        lua_setfield(L, -2, "where");
        set_int(L, "slot", at.slot);
        set_int(L, "sub", at.sub);
        if (bag) {
            lua_pushstring(L, bag->name.c_str());
            lua_setfield(L, -2, "bag");
        }
        set_bool(L, "kept", c.item_kept(it));
        lua_rawseti(L, -2, ++n);
    };
    auto scan = [&](std::int16_t type, int first, int last, const char* where, bool bags) {
        for (int s = first; s <= last; ++s) {
            const net::ItemSlot top{type, static_cast<std::int16_t>(s), -1, -1, 0, 0};
            const net::Item* it = inv.at(top);
            if (!it) continue;
            row(*it, top, where, nullptr);
            if (!bags || !it->is_bag()) continue;
            for (int i = 0; i < it->bag_slots; ++i) {
                net::ItemSlot in = top;
                in.sub = static_cast<std::int16_t>(i);
                if (const net::Item* ci = inv.at(in)) row(*ci, in, where, it);
            }
        }
    };
    scan(net::kInvPossessions, net::kSlotCharm, net::kSlotAmmo, "worn", false);
    scan(net::kInvPossessions, net::kSlotGeneral1, net::kSlotGeneral10, "carried", true);
    scan(net::kInvBank, 0, (c.bank() ? c.bank()->slots : 24) - 1, "bank", true);
    scan(net::kInvSharedBank, 0, kSharedBankSlots - 1, "shared", true);
    return 1;
}

int ItemsRevision(lua_State* L) {
    auto& h = host(L);
    lua_pushinteger(L, h.conn ? static_cast<lua_Integer>(h.conn->inventory().revision()) : 0);
    lua_pushinteger(L, h.conn ? static_cast<lua_Integer>(h.conn->kept_items_revision()) : 0);
    return 2;
}

int ItemStats(lua_State* L) {
    lua_createtable(L, static_cast<int>(std::size(kStats)), 0);
    lua_Integer n = 0;
    for (const StatDef& d : kStats) {
        lua_createtable(L, 2, 0);
        lua_pushstring(L, d.key);
        lua_rawseti(L, -2, 1);
        lua_pushstring(L, d.label);
        lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// "i:<slot>[:<sub>]" (the possessions; a bank's "b:" or "s:" place is not one a bulk action takes)
std::optional<net::ItemSlot> carried_place(std::string_view text) {
    if (text.size() < 3 || text[0] != 'i' || text[1] != ':') return std::nullopt;
    text.remove_prefix(2);
    int slot = 0, sub = -1;
    auto r = std::from_chars(text.data(), text.data() + text.size(), slot);
    if (r.ec != std::errc{}) return std::nullopt;
    if (r.ptr != text.data() + text.size()) {
        if (*r.ptr != ':') return std::nullopt;
        auto r2 = std::from_chars(r.ptr + 1, text.data() + text.size(), sub);
        if (r2.ec != std::errc{} || r2.ptr != text.data() + text.size() || sub < 0) return std::nullopt;
    }
    if (slot < 0 || slot > 0x7fff || sub > 0x7fff) return std::nullopt;
    return net::ItemSlot::possessions(static_cast<std::int16_t>(slot), static_cast<std::int16_t>(sub));
}

int BulkItems(lua_State* L) {
    if (distribution::managed()) {
        lua_pushinteger(L, 0);
        lua_pushliteral(L, "Item automation is disabled in this distribution.");
        return 2;
    }
    auto& h = host(L);
    using M = net::GameConnection::ItemJobMode;
    std::string_view mode_arg = luaL_checkstring(L, 1);
    std::optional<M> mode = mode_arg == "sell"      ? std::optional<M>(M::Sell)
                            : mode_arg == "destroy" ? std::optional<M>(M::Destroy)
                            : mode_arg == "tribute" ? std::optional<M>(M::Tribute)
                                                    : std::nullopt;
    if (!mode) return luaL_argerror(L, 1, "\"sell\", \"destroy\" or \"tribute\"");
    ui::lua::unview(L, 2);
    std::vector<net::ItemSlot> slots;
    if (lua_istable(L, 2))
        for (lua_Integer i = 1;; ++i) {
            lua_rawgeti(L, 2, i);
            bool more = lua_type(L, -1) == LUA_TSTRING;
            if (more)
                if (auto at = carried_place(lua_tostring(L, -1))) slots.push_back(*at);
            lua_pop(L, 1);
            if (!more) break;
        }
    const bool dry = lua_toboolean(L, 3) != 0;
    if (!h.conn || !h.in_game) {
        lua_pushinteger(L, 0);
        lua_pushstring(L, "Not in game.");
        return 2;
    }
    net::GameConnection& c = *h.conn;
    const std::size_t count = c.bulk_items_plan(*mode, slots).size();
    std::string err = dry ? c.bulk_items_blocked(*mode) : c.start_bulk_job(*mode, slots);
    lua_pushinteger(L, static_cast<lua_Integer>(count));
    if (err.empty()) return 1;
    lua_pushstring(L, err.c_str());
    return 2;
}

int TakeItemSearch(lua_State* L) {
    auto& h = host(L);
    if (!h.item_search) return 0;
    lua_pushstring(L, h.item_search->c_str());
    h.item_search.reset();
    return 1;
}

int ItemKept(lua_State* L) {
    auto& h = host(L);
    auto id = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, h.conn && h.conn->kept_items().kept(id));
    return 1;
}

int SetItemKept(lua_State* L) {
    auto& h = host(L);
    auto id = static_cast<std::uint32_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_optstring(L, 2, "");
    if (h.conn) h.conn->set_item_kept(id, name, lua_toboolean(L, 3) != 0);
    return 0;
}

class InventoryManagerWindow : public StockWindow {
public:
    InventoryManagerWindow() : StockWindow("InventoryManagerWnd", false) {}
};

} // namespace

void add_inventory_manager_window(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<InventoryManagerWindow>());
}

void register_inventory_manager_api(lua_State* L, EqHost& h) {
    static const luaL_Reg fns[] = {{"AllItems", AllItems},
                                   {"ItemsRevision", ItemsRevision},
                                   {"ItemKept", ItemKept},
                                   {"SetItemKept", SetItemKept},
                                   {"ItemStats", ItemStats},
                                   {"BulkItems", BulkItems},
                                   {"TakeItemSearch", TakeItemSearch},
                                   {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h);
    luaL_setfuncs(L, fns, 1);
}

} // namespace hailstone::app
