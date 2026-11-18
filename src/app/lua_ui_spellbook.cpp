// The spell book as a whole, for UIs that show it as one list rather than the stock window's pages of 8
// (ui/modern/spellbook.lua), and the spell sets (game/spell_sets.h; GameConnection::memorize_set):
//
//   book = hs.SpellBook()        -- every scribed spell: { slot (0-based), id, name, icon, level, mana, endurance,
//                                   castTime, recastTime, recoveryTime, range, aeRange, duration, beneficial,
//                                   targetType, skill, skillName, category, categoryName, subcategory,
//                                   subcategoryName, resist, gem (0-based, when memorized) }
//   state = hs.SpellBookState()  -- { open, sitting, gems = { id or -1 x12 }, usableGems, memorizing = { gem, spell }
//                                   or nil, settingSet, scribing, cursorSpell (book slot or nil), holdingItem,
//                                   swapSlot (the book slot picked up to move, or nil) }
//   texture, u0, v0, u1, v1 = hs.SpellIcon(icon)    -- a spell icon (A_SpellIcons), nil when its sheet is missing
//   ok = hs.SpellBookClick(slot)  -- the book's click on that book slot: sitting, a
//                                   scroll on the cursor is scribed there, else the spell goes on the cursor for a gem
//   hs.SpellBookSwap(slot)       -- the stock book's quick right click on that book slot (Send_OP_SwapSpell): the first
//                                   picks it up (state.swapSlot), the same slot again puts it back, another slot asks
//                                   the server to swap the two (the book follows the server's echo)
//   hs.SpellBookMemorize(slot, gem)  -- memorizes the slot's spell into gem (0-based), as the cursor and a gem click do
//                                   (an empty gem only: StartSpellMemorization with bReplace 0)
//   hs.ShowSpell(id [, temporary])   -- the spell's display window (CSpellDisplayManager::ShowSpell)
//   title, stml = hs.SpellDetails(id) -- the spell display window's text (game/spell_display.h: duration, level,
//                                   skill, mana, cast time, timers, the effect description), STML lines
//   sets = hs.SpellSets()        -- 10 of { name, inUse, spells = { id or -1 x12 } }
//   hs.SaveSpellSet(name)        -- the gems as a set (SaveLoadout)
//   n = hs.SaveSpellSet(name, spells) -- (Hailstone's set editor) `spells` (gem 1..12: id, or -1 / nil for none) as
//                                   set `name`, nothing memorized and nothing said; its number after the sort, nil
//                                   when all 10 are in use by other names
//   n = hs.EditSpellSet(n, name, spells) -- (the set editor) set n renamed and given `spells` (a set already called
//                                   `name` is replaced); its number after the sort, nil when n is not in use
//   ok = hs.MemorizeSpellSet(n)  -- 1-based; MemorizeSet
//   hs.DeleteSpellSet(n)         -- 1-based
#include <array>
#include <cstdint>
#include <string>

#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "ui/lua/lua_host.h"
#include "game/spell_display.h"
#include "net/game_connection.h"
#include "plugin/lua_keep.h"
#include "ui/ui_textures.h"

namespace hailstone::app {

namespace {

UiHost& host(lua_State* L) { return *static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1))); }

void set_int(lua_State* L, const char* k, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, k);
}
void set_num(lua_State* L, const char* k, lua_Number v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, k);
}
void set_str(lua_State* L, const char* k, const std::string& v) {
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, k);
}
void set_bool(lua_State* L, const char* k, bool v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, k);
}

std::int32_t as_id(std::uint32_t spell) { return spell == game::kNoSpell ? -1 : static_cast<std::int32_t>(spell); }

int SpellBook(lua_State* L) {
    auto& h = host(L);
    // The names made for a row, in a box the state owns across their pushes (plugin/lua_keep.h).
    std::string& text = plugin::lua_keep<std::string>(L);
    const auto set_made = [&](const char* k, const auto& make) {
        plugin::cxx_guard(L, [&] { text = make(); });
        set_str(L, k, text);
    };
    lua_newtable(L);
    if (!h.conn) return 1;
    const auto& c = *h.conn;
    const auto& st = c.spell_state();
    const auto& strings = c.strings();
    const std::uint32_t cls = c.profile() ? c.profile()->class_ : 0;
    int n = 0;
    for (int slot = 0; slot < game::kSpellBookSize; ++slot) {
        std::uint32_t id = st.book[static_cast<std::size_t>(slot)];
        if (id == game::kNoSpell || id == 0) continue;
        const game::SpellData* s = c.spell_table().get(id);
        if (!s) continue;
        lua_createtable(L, 0, 24);
        set_int(L, "slot", slot);
        set_int(L, "id", id);
        set_str(L, "name", s->name);
        set_int(L, "icon", s->spell_icon);
        set_int(L, "level", c.spell_level_needed(*s, cls));
        set_int(L, "mana", s->mana);
        set_int(L, "endurance", s->endurance_cost);
        set_int(L, "castTime", s->cast_time);
        set_int(L, "recastTime", s->recast_time);
        set_int(L, "recoveryTime", s->recovery_time);
        set_num(L, "range", s->range);
        set_num(L, "aeRange", s->ae_range);
        set_int(L, "duration", s->duration_cap);
        set_bool(L, "beneficial", s->spell_type != 0);
        set_int(L, "targetType", s->target_type);
        set_int(L, "skill", s->skill);
        if (h.skill_name) set_made("skillName", [&] { return h.skill_name(s->skill); });
        set_int(L, "category", s->category);
        set_made("categoryName", [&] {
            return s->category > 0 ? strings.db_string(static_cast<std::uint32_t>(s->category), 5) : std::string();
        });
        set_int(L, "subcategory", s->subcategory);
        set_made("subcategoryName", [&] {
            return s->subcategory > 0 ? strings.db_string(static_cast<std::uint32_t>(s->subcategory), 5) : std::string();
        });
        set_int(L, "resist", s->resist);
        for (int g = 0; g < game::kSpellGems; ++g)
            if (st.gems[static_cast<std::size_t>(g)].spell == id) set_int(L, "gem", g);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int SpellBookState(lua_State* L) {
    auto& h = host(L);
    lua_createtable(L, 0, 10);
    if (!h.conn) return 1;
    const auto& c = *h.conn;
    const auto& st = c.spell_state();
    set_bool(L, "open", c.spell_book_open());
    set_bool(L, "sitting", c.sitting());
    lua_createtable(L, game::kSpellGems, 0);
    for (int g = 0; g < game::kSpellGems; ++g) {
        lua_pushinteger(L, as_id(st.gems[static_cast<std::size_t>(g)].spell));
        lua_rawseti(L, -2, g + 1);
    }
    lua_setfield(L, -2, "gems");
    set_int(L, "usableGems", c.usable_gems());
    if (st.memorizing) {
        lua_createtable(L, 0, 2);
        set_int(L, "gem", st.memorizing->gem);
        set_int(L, "spell", st.memorizing->spell);
        lua_setfield(L, -2, "memorizing");
    }
    set_bool(L, "settingSet", c.memorizing_set());
    set_bool(L, "scribing", st.scribing.has_value());
    if (h.cursor_spell) set_int(L, "cursorSpell", *h.cursor_spell);
    set_bool(L, "holdingScroll", c.cursor_scroll_spell() != 0);
    if (c.spell_book_swap_slot() >= 0) set_int(L, "swapSlot", c.spell_book_swap_slot());
    return 1;
}

int SpellIcon(lua_State* L) {
    auto& h = host(L);
    auto icon = h.textures ? h.textures->spell_icon(static_cast<std::int32_t>(luaL_checkinteger(L, 1))) : std::nullopt;
    if (!icon) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlightuserdata(L, icon->id);
    lua_pushnumber(L, icon->u0);
    lua_pushnumber(L, icon->v0);
    lua_pushnumber(L, icon->u1);
    lua_pushnumber(L, icon->v1);
    return 5;
}

int SpellBookClick(lua_State* L) {
    auto& h = host(L);
    int slot = held_int(luaL_checkinteger(L, 1));
    bool ok = false;
    if (h.conn && slot >= 0 && slot < game::kSpellBookSize && h.conn->sitting()) {
        auto& c = *h.conn;
        // CSpellBookWnd @0x75e790, sitting (or mounted): a scroll on the cursor is scribed into the slot
        // (StartSpellScribe); else the spell goes on the cursor (AttachToCursor type 1) when CanStartMemming allows.
        if (c.cursor_scroll_spell() != 0) {
            c.scribe_cursor(slot);
            ok = true;
        } else if (c.can_start_memming(slot)) {
            h.cursor_button.reset();
            h.cursor_spell = slot;
            ++h.cursor_serial;
            ok = true;
        }
    }
    lua_pushboolean(L, ok);
    return 1;
}

int SpellBookSwap(lua_State* L) {
    auto& h = host(L);
    if (h.conn) h.conn->spell_book_swap_click(held_int(luaL_checkinteger(L, 1)));
    return 0;
}

int SpellBookMemorize(lua_State* L) {
    auto& h = host(L);
    int slot = held_int(luaL_checkinteger(L, 1)), gem = held_int(luaL_checkinteger(L, 2));
    // HandleSpellLeftClick's StartSpellMemorization(slot, gem, bReplace 0) @0x75bf30 refuses a gem that holds a
    // spell; Hailstone's replace_gem_spell lets it through (UiHost::gem_takes_book_spell). The book is never
    // written over.
    if (h.gem_takes_book_spell(gem)) {
        h.conn->memorize(slot, gem);
        if (h.cursor_spell && *h.cursor_spell == slot) h.cursor_spell.reset();
    }
    return 0;
}

int ShowSpell(lua_State* L) {
    host(L).show_spell(static_cast<std::int32_t>(luaL_checkinteger(L, 1)), lua_toboolean(L, 2) != 0);
    return 0;
}

int SpellDetails(lua_State* L) {
    auto& h = host(L);
    const game::SpellData* s =
        h.conn ? h.conn->spell_table().get(static_cast<std::uint32_t>(luaL_checkinteger(L, 1))) : nullptr;
    if (!s) {
        lua_pushnil(L);
        return 1;
    }
    const auto& c = *h.conn;
    // The display in a box the state owns across the pushes (plugin/lua_keep.h).
    game::SpellDisplay& d = plugin::lua_keep<game::SpellDisplay>(L);
    plugin::cxx_guard(L, [&] {
        game::SpellDisplayContext ctx;
        ctx.strings = &c.strings();
        ctx.character = h.character;
        ctx.player_level = c.player_status().level;
        ctx.player_class = c.profile() ? static_cast<std::uint8_t>(c.profile()->class_) : 0;
        ctx.db_string = [&c](std::uint32_t id, std::uint32_t type) { return c.strings().db_string(id, type); };
        d = game::build_spell_display(*s, ctx);
    });
    lua_pushlstring(L, d.title.data(), d.title.size());
    lua_pushlstring(L, d.description.data(), d.description.size());
    return 2;
}

// The actual display instance, not a lookup by its (possibly non-unique) title.
int DisplaySpellId(lua_State* L) {
    lua_Integer n = luaL_checkinteger(L, 1);
    const auto& displays = host(L).spell_displays;
    if (n < 1 || static_cast<lua_Unsigned>(n) > displays.size()) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, displays[static_cast<std::size_t>(n - 1)].first);
    return 1;
}

int DisplayItemData(lua_State* L) {
    lua_Integer n = luaL_checkinteger(L, 1);
    const auto* displays = host(L).item_displays;
    if (!displays || n < 1 || static_cast<lua_Unsigned>(n) > displays->size()) {
        lua_pushnil(L);
        return 1;
    }
    const auto& item = (*displays)[static_cast<std::size_t>(n - 1)];
    lua_createtable(L, 0, 17);
    set_int(L, "id", item.id);
    set_int(L, "icon", item.icon);
    set_str(L, "name", item.name);
    set_str(L, "lore", item.lore);
    set_int(L, "value", item.value);
    set_int(L, "stack_count", item.stack_count);
    set_int(L, "stackCount", item.stack_count); // camelCase as hs's other tables; the snake_case keys stay for callers
    set_int(L, "damage", item.damage);
    set_int(L, "delay", item.delay);
    set_int(L, "item_type", item.item_type);
    set_int(L, "itemType", item.item_type);
    set_int(L, "favor", item.favor);
    auto spell_id = [&](net::ItemEffectKind kind) {
        const auto& effect = item.effects[kind];
        return effect.present() ? effect.effect : 0;
    };
    set_int(L, "scrollSpell", spell_id(net::kEffectScroll));
    set_int(L, "focusSpell", spell_id(net::kEffectFocus));
    set_int(L, "procSpell", spell_id(net::kEffectProc));
    set_int(L, "wornSpell", spell_id(net::kEffectWorn));
    set_int(L, "clickSpell", spell_id(net::kEffectClick));
    return 1;
}

int SpellSets(lua_State* L) {
    auto& h = host(L);
    lua_createtable(L, game::kSpellLoadouts, 0);
    if (!h.conn) return 1;
    for (int n = 0; n < game::kSpellLoadouts; ++n) {
        const auto& s = h.conn->spell_loadouts().at(n);
        lua_createtable(L, 0, 3);
        static const std::string kNone; // (a std::string() here would be a copy alive across the push)
        set_str(L, "name", s.in_use ? s.name : kNone);
        set_bool(L, "inUse", s.in_use);
        lua_createtable(L, game::kLoadoutGems, 0);
        for (int k = 0; k < game::kLoadoutGems; ++k) {
            lua_pushinteger(L, s.spells[static_cast<std::size_t>(k)]);
            lua_rawseti(L, -2, k + 1);
        }
        lua_setfield(L, -2, "spells");
        lua_rawseti(L, -2, n + 1);
    }
    return 1;
}

// A set's 12 spells from a Lua table at `idx`: an id >= 0, anything else (nil, -1, not a number) an empty gem.
std::array<std::int32_t, game::kLoadoutGems> loadout_spells(lua_State* L, int idx) {
    luaL_checktype(L, idx, LUA_TTABLE);
    ui::lua::unview(L, idx);
    std::array<std::int32_t, game::kLoadoutGems> spells{};
    for (int k = 0; k < game::kLoadoutGems; ++k) {
        lua_rawgeti(L, idx, k + 1);
        int ok = 0;
        lua_Integer id = lua_tointegerx(L, -1, &ok);
        lua_pop(L, 1);
        spells[static_cast<std::size_t>(k)] = ok && id >= 0 && id <= INT32_MAX ? static_cast<std::int32_t>(id) : -1;
    }
    return spells;
}

int SaveSpellSet(lua_State* L) {
    auto& h = host(L);
    if (lua_isnoneornil(L, 2)) {
        if (h.conn) h.conn->save_loadout(luaL_checkstring(L, 1));
        return 0;
    }
    std::size_t len = 0;
    const char* name = luaL_checklstring(L, 1, &len);
    auto spells = loadout_spells(L, 2);
    int at = -1;
    if (h.conn && len > 0 &&
        h.conn->spell_loadouts().save_set({name, len}, spells, &at) == game::SpellLoadouts::SaveResult::Saved && at >= 0)
        lua_pushinteger(L, at + 1);
    else
        lua_pushnil(L);
    return 1;
}

int EditSpellSet(lua_State* L) {
    auto& h = host(L);
    int n = held_index(luaL_checkinteger(L, 1));
    std::size_t len = 0;
    const char* name = luaL_checklstring(L, 2, &len);
    auto spells = loadout_spells(L, 3);
    int at = -1;
    if (h.conn && h.conn->spell_loadouts().edit(n, {name, len}, spells, &at) && at >= 0)
        lua_pushinteger(L, at + 1);
    else
        lua_pushnil(L);
    return 1;
}

int MemorizeSpellSet(lua_State* L) {
    auto& h = host(L);
    lua_pushboolean(L, h.conn && h.conn->memorize_loadout(held_index(luaL_checkinteger(L, 1))));
    return 1;
}

int DeleteSpellSet(lua_State* L) {
    auto& h = host(L);
    if (h.conn) h.conn->delete_loadout(held_index(luaL_checkinteger(L, 1)));
    return 0;
}

} // namespace

bool UiHost::gem_takes_book_spell(int gem) const {
    if (!conn || gem < 0 || gem >= game::kSpellGems) return false;
    if (conn->spell_state().gems[static_cast<std::size_t>(gem)].spell == game::kNoSpell) return true;
    // Hailstone: a deliberate difference (docs/mechanics.md "Spell gems"). A stock EQEmu server's MemSpell forgets
    // the gem's spell before memorizing the new one, and GameConnection's forget handler keeps quiet for it.
    return !get_option || get_option("replace_gem_spell") != 0;
}

void register_spellbook_api(lua_State* L, UiHost& h) {
    static const luaL_Reg fns[] = {{"SpellBook", SpellBook},       {"SpellBookState", SpellBookState},
                                   {"SpellIcon", SpellIcon},       {"SpellBookClick", SpellBookClick},
                                   {"SpellBookMemorize", SpellBookMemorize}, {"ShowSpell", ShowSpell},
                                   {"SpellBookSwap", SpellBookSwap},
                                   {"SpellDetails", SpellDetails}, {"DisplaySpellId", DisplaySpellId},
                                   {"DisplayItemData", DisplayItemData},
                                   {"SpellSets", SpellSets},       {"SaveSpellSet", SaveSpellSet},
                                   {"MemorizeSpellSet", MemorizeSpellSet}, {"DeleteSpellSet", DeleteSpellSet},
                                   {"EditSpellSet", EditSpellSet},
                                   {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h);
    luaL_setfuncs(L, fns, 1);
}

} // namespace hailstone::app
