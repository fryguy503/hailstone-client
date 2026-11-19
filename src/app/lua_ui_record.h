#pragma once

// Records an hs.* query fills in a table the caller keeps from frame to frame (its optional `into` argument), so a
// window that asks every frame makes no garbage while the answer stays the same: the record is refilled in place,
// every field written on every call (nil for one the answer lacks), so it holds exactly what a new table would.
// Without `into` (or given a table with a metatable, whose __newindex would run Lua code mid-fill) the query makes a
// new table as before.
//
// What a caller may keep across calls (a list it caches by revision) is never refilled: a list gets a new table
// whenever it has entries, and the table already in the field is reused only while the list stays empty. Plain-value
// subrecords (a few scalars, never cached by the windows) are refilled in place.

#include <cstddef>
#include <string_view>

#include <lua.hpp>

#include "core/lua_keep.h"

namespace hailstone::app::record {

// The record to fill, left on top of the stack: the table at `arg` when it is a plain table (a caller's, kept from
// frame to frame), else a new one with room for `fields`. A table with a metatable (a plugin's view of the pack's, or
// one of its own) is not filled: its __newindex or __index would run Lua code in the middle of the fill, which can
// raise past the C++ objects the fill reads from, or call back in. (The hs.*Info calls use it too.)
inline void push(lua_State* L, int arg, int fields) {
    if (lua_type(L, arg) == LUA_TTABLE) {
        if (!lua_getmetatable(L, arg)) {
            lua_pushvalue(L, arg);
            return;
        }
        lua_pop(L, 1);
    }
    lua_createtable(L, 0, fields);
}

// The plain table in field `field` of the record on top (refilled in place), or a new one put there; left on top.
inline void subrecord(lua_State* L, const char* field, int fields) {
    if (lua_getfield(L, -1, field) == LUA_TTABLE) {
        if (!lua_getmetatable(L, -1)) return;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_createtable(L, 0, fields);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, field);
}

// The list for field `field` of the record on top, left on top (the caller fills it and sets the field): the table
// already there when the list is empty and so is that table (a caller may have kept it; empty, it stays as it was),
// else a new one with room for `count`.
inline void list(lua_State* L, const char* field, std::size_t count) {
    if (count == 0 && lua_getfield(L, -1, field) == LUA_TTABLE) {
        if (lua_getmetatable(L, -1)) {
            lua_pop(L, 1);
        } else {
            lua_pushnil(L);
            if (!lua_next(L, -2)) return; // empty: reused
            lua_pop(L, 2);                // (its first key and value)
        }
        lua_pop(L, 1);
    } else if (count == 0) {
        lua_pop(L, 1);
    }
    lua_createtable(L, static_cast<int>(count), 0);
}

// Field `field` of the record on top set to the text s (all of it, NULs too), unless it already holds that text: Lua
// makes a new object for every string over 40 bytes (core::set_string_field).
inline void text(lua_State* L, const char* field, std::string_view s) {
    lua_getfield(L, -1, field);
    std::size_t n = 0;
    const char* old = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &n) : nullptr;
    const bool same = old && n == s.size() && s.compare(0, s.size(), old, n) == 0;
    lua_pop(L, 1);
    if (same) return;
    lua_pushlstring(L, s.data(), s.size());
    lua_setfield(L, -2, field);
}

// For a list refilled in place (only where the binding says its callers keep no entry past the next call): the plain
// table at index `at` of the list on top, or a new one with room for `fields`; left on top for the caller to fill and
// put back at `at` (lua_rawseti).
inline void element(lua_State* L, lua_Integer at, int fields) {
    if (lua_rawgeti(L, -1, at) == LUA_TTABLE) {
        if (!lua_getmetatable(L, -1)) return;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_createtable(L, 0, fields);
}

// The entries of the list on top from index `from` on set to nil (what the last fill left past this one's end).
inline void trim(lua_State* L, lua_Integer from) {
    for (lua_Integer k = from; lua_rawgeti(L, -1, k) != LUA_TNIL; ++k) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_rawseti(L, -2, k);
    }
    lua_pop(L, 1); // (the nil that ended it)
}

// Field `field` of the record on top set to nil.
inline void clear(lua_State* L, const char* field) {
    lua_pushnil(L);
    lua_setfield(L, -2, field);
}

} // namespace hailstone::app::record
