// Hailstone's item comparison for the UI packs (game/item_compare.h; docs/item-compare.md): what wearing an item
// instead of what the player wears would change. The original has no such thing; nothing here changes a rule.
//
//   c = eq.CompareItem(slot [, bagSlot])     -- an item the player carries (possessions; nil for a worn one)
//   c = eq.CompareSlotItem(window, child)    -- the item in a window's item slot (loot, merchant, trade, bank, give,
//                                               and the item display's Hailstone_Item: a linked item)
//   c = eq.CompareBagSlotItem(bagWindow, i)  -- the item in slot i of a bag window
//
// nil out of the game, before the player's stats exist, for an empty slot or for an item that is not worn gear.
// Else { usable, whyNot, notes = { text, ... }, slots = { { slot, slotName, current = { name, ... } ("" an empty
// slot), verdict ("upgrade" / "downgrade" / "mixed" / "same" / "cantuse"), score, changes = { { key, name, before,
// after, delta, weight }, ... }, weaponBefore, weaponAfter = { damage, delay, bonus } or nil, gained = { text, ... },
// lost = { text, ... } }, ... } }, the best swap first. weight 0: a change that does not count for the class.
#include <chrono>
#include <memory>
#include <string>

#include <lua.hpp>

#include "app/lua_eq.h"
#include "app/lua_eq_windows.h"
#include "game/inventory.h"
#include "game/item_compare.h"
#include "game/item_moves.h"
#include "game/mechanics/character.h"
#include "game/server_ext.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

// The last comparison, kept for half a second while the same item stays under the mouse (a tooltip asks every frame;
// the player's Character is rebuilt every frame, so the age is what refreshes it).
struct CompareCache {
    std::vector<std::unique_ptr<StockWindow>>* windows = nullptr;
    game::mechanics::CharacterSheet scratch;
    std::string key;
    std::uint32_t item_id = 0;
    std::string serial;
    std::chrono::steady_clock::time_point made{};
    std::optional<game::ItemComparison> result;
};

EqHost& host_up(lua_State* L) { return *static_cast<EqHost*>(lua_touserdata(L, lua_upvalueindex(1))); }
CompareCache& cache_up(lua_State* L) { return *static_cast<CompareCache*>(lua_touserdata(L, lua_upvalueindex(2))); }

int gc_cache(lua_State* L) {
    static_cast<CompareCache*>(lua_touserdata(L, 1))->~CompareCache();
    return 0;
}

StockWindow* find_window(CompareCache& c, std::string_view name) {
    if (!c.windows) return nullptr;
    for (auto& w : *c.windows)
        if (w->screen() == name) return w.get();
    return nullptr;
}

game::CompareOptions options_for(const net::GameConnection& conn) {
    game::CompareOptions o;
    o.class_mask = conn.player_class_mask();
    o.can_use_all = conn.server_ext().can_use_all_items(conn);
    const auto& profile = conn.profile();
    o.dual_wield = profile && profile->skills.size() > 22 && profile->skills[22] > 0;
    o.slots_available = 0;
    for (int s = 0; s < 23; ++s)
        if (game::inv_slot_available(s, profile ? profile->inventory_slot_mask : 0xffffffffu,
                                     profile ? profile->expansions : 0, 32))
            o.slots_available |= 1u << s;
    return o;
}

void set_str(lua_State* L, const char* k, const std::string& v) {
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, k);
}
void set_num(lua_State* L, const char* k, lua_Number v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, k);
}
void set_int(lua_State* L, const char* k, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, k);
}
void push_strings(lua_State* L, const std::vector<std::string>& v) {
    lua_createtable(L, static_cast<int>(v.size()), 0);
    for (std::size_t i = 0; i < v.size(); ++i) {
        lua_pushlstring(L, v[i].data(), v[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
}
void push_weapon(lua_State* L, const char* k, const std::optional<game::WeaponInfo>& w) {
    if (!w) return;
    lua_createtable(L, 0, 3);
    set_int(L, "damage", w->damage);
    set_int(L, "delay", w->delay);
    set_int(L, "bonus", w->bonus);
    lua_setfield(L, -2, k);
}

void push_comparison(lua_State* L, const game::ItemComparison& c) {
    lua_createtable(L, 0, 4);
    lua_pushboolean(L, c.usable);
    lua_setfield(L, -2, "usable");
    set_str(L, "whyNot", c.why_not);
    push_strings(L, c.notes);
    lua_setfield(L, -2, "notes");
    lua_createtable(L, static_cast<int>(c.slots.size()), 0);
    for (std::size_t i = 0; i < c.slots.size(); ++i) {
        const game::SlotComparison& s = c.slots[i];
        lua_createtable(L, 0, 11);
        set_int(L, "slot", s.slot);
        lua_pushstring(L, net::possessions_slot_name(s.slot));
        lua_setfield(L, -2, "slotName");
        push_strings(L, s.current);
        lua_setfield(L, -2, "current");
        lua_pushstring(L, game::verdict_name(s.verdict));
        lua_setfield(L, -2, "verdict");
        set_num(L, "score", s.score);
        lua_createtable(L, static_cast<int>(s.changes.size()), 0);
        for (std::size_t j = 0; j < s.changes.size(); ++j) {
            const game::StatChange& ch = s.changes[j];
            lua_createtable(L, 0, 6);
            lua_pushstring(L, game::compare_stat_key(ch.stat));
            lua_setfield(L, -2, "key");
            lua_pushstring(L, game::compare_stat_name(ch.stat));
            lua_setfield(L, -2, "name");
            set_int(L, "before", ch.before);
            set_int(L, "after", ch.after);
            set_int(L, "delta", ch.delta());
            set_num(L, "weight", ch.weight);
            lua_rawseti(L, -2, static_cast<lua_Integer>(j + 1));
        }
        lua_setfield(L, -2, "changes");
        push_weapon(L, "weaponBefore", s.weapon_before);
        push_weapon(L, "weaponAfter", s.weapon_after);
        push_strings(L, s.gained);
        lua_setfield(L, -2, "gained");
        push_strings(L, s.lost);
        lua_setfield(L, -2, "lost");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "slots");
}

// The comparison of `item` (from the place `key` names), from the cache while it is fresh.
int compare(lua_State* L, std::string key, const net::Item* item) {
    EqHost& h = host_up(L);
    CompareCache& c = cache_up(L);
    if (!item || !h.in_game || !h.conn || !h.character) {
        lua_pushnil(L);
        return 1;
    }
    const auto now = std::chrono::steady_clock::now();
    if (c.key != key || c.item_id != item->id || c.serial != item->serial ||
        now - c.made > std::chrono::milliseconds(500)) {
        c.result = game::compare_item(*h.character, *item, options_for(*h.conn), c.scratch);
        c.key = std::move(key);
        c.item_id = item->id;
        c.serial = item->serial;
        c.made = now;
    }
    if (!c.result) lua_pushnil(L);
    else push_comparison(L, *c.result);
    return 1;
}

int CompareItem(lua_State* L) {
    EqHost& h = host_up(L);
    const auto slot = static_cast<std::int16_t>(luaL_checkinteger(L, 1));
    const auto bag = static_cast<std::int16_t>(lua_isnoneornil(L, 2) ? -1 : luaL_checkinteger(L, 2));
    // What is worn is what the comparison compares against.
    if (bag < 0 && slot >= 0 && slot < 23) {
        lua_pushnil(L);
        return 1;
    }
    const net::Item* item = h.conn && h.in_game ? h.conn->inventory().at(net::ItemSlot::possessions(slot, bag)) : nullptr;
    return compare(L, "inv:" + std::to_string(slot) + ":" + std::to_string(bag), item);
}

int CompareSlotItem(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* child = luaL_checkstring(L, 2);
    StockWindow* w = find_window(cache_up(L), name);
    const net::Item* item = w ? w->slot_item(host_up(L), child) : nullptr;
    return compare(L, std::string("win:") + name + ":" + child, item);
}

int CompareBagSlotItem(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const int index = static_cast<int>(luaL_checkinteger(L, 2));
    StockWindow* w = find_window(cache_up(L), name);
    const net::Item* item = w ? w->bag_item(host_up(L), index) : nullptr;
    return compare(L, std::string("bag:") + name + ":" + std::to_string(index), item);
}

} // namespace

void register_compare_api(lua_State* L, EqHost& host, std::vector<std::unique_ptr<StockWindow>>& windows) {
    static const luaL_Reg fns[] = {{"CompareItem", CompareItem},
                                   {"CompareSlotItem", CompareSlotItem},
                                   {"CompareBagSlotItem", CompareBagSlotItem}};
    // The one cache, the second upvalue of each function (freed with the last of them).
    auto* cache = new (lua_newuserdatauv(L, sizeof(CompareCache), 0)) CompareCache();
    cache->windows = &windows;
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, gc_cache);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    for (const auto& f : fns) {
        lua_pushlightuserdata(L, &host);
        lua_pushvalue(L, -2);
        lua_pushcclosure(L, f.func, 2);
        lua_setfield(L, -3, f.name); // the eq table, under the cache
    }
    lua_pop(L, 1);
}

} // namespace hailstone::app
