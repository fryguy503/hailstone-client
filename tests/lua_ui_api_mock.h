#pragma once

// Plain Lua window fixtures keep flat mocks so existing assertions and monkeypatches stay observable.
// The generated adapter presents their documented hs.UI paths, with explicit feature discovery.
#include <filesystem>
#include <string>

#include <lua.hpp>

namespace hailstone::test {

inline int register_mock_ui_api(lua_State* L) {
    const auto file = std::filesystem::path(__FILE__).parent_path() / "lua_ui_api_mock.lua";
    int result = luaL_loadfile(L, file.string().c_str());
    if (result == LUA_OK) result = lua_pcall(L, 0, 1, 0);
    if (result == LUA_OK) lua_setglobal(L, "TEST_UI_API_ATTACH");
    return result;
}

inline int attach_mock_ui_api(lua_State* L) {
    const int top = lua_gettop(L);
    lua_getglobal(L, "hs");
    if (!lua_istable(L, -1)) { lua_settop(L, top); return LUA_OK; }
    const int root = lua_gettop(L);
    lua_pushliteral(L, "UI");
    lua_rawget(L, root);
    if (lua_istable(L, -1)) { lua_settop(L, top); return LUA_OK; }
    lua_pop(L, 1);
    const auto file = std::filesystem::path(__FILE__).parent_path() / "lua_ui_api_mock.lua";
    int result = luaL_loadfile(L, file.string().c_str());
    if (result == LUA_OK) result = lua_pcall(L, 0, 1, 0);
    if (result == LUA_OK) {
        lua_pushvalue(L, root);
        result = lua_pcall(L, 1, 0, 0);
    }
    if (result == LUA_OK) lua_settop(L, top);
    return result;
}

// A real-host test may deliberately instrument a binding function after registration. Rebind its exact override
// into the facade as well; production callers never need this and keep the original guarded closures.
inline void sync_ui_api_aliases(lua_State* L) {
    struct Alias { const char* binding; const char* module; const char* member; };
    static constexpr Alias aliases[] = {
#define UI_METHOD(binding, module, member, caps) {binding, module, member},
#define UI_CONSTANT(binding, module, member) {binding, module, member},
#define UI_HOOK_POLICY(binding)
#include "app/lua_ui_api.inc"
#undef UI_CONSTANT
#undef UI_METHOD
#undef UI_HOOK_POLICY
    };
    const int top = lua_gettop(L);
    lua_getglobal(L, "hs");
    if (!lua_istable(L, -1)) { lua_settop(L, top); return; }
    const int root = lua_gettop(L);
    lua_getfield(L, root, "UI");
    if (!lua_istable(L, -1)) { lua_settop(L, top); return; }
    const int facade = lua_gettop(L);
    for (const auto& alias : aliases) {
        lua_getfield(L, facade, alias.module);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        const std::string path(alias.binding);
        const auto dot = path.find('.');
        if (dot == std::string::npos) lua_getfield(L, root, alias.binding);
        else {
            lua_getfield(L, root, path.substr(0, dot).c_str());
            if (lua_istable(L, -1)) lua_getfield(L, -1, path.substr(dot + 1).c_str());
            else lua_pushnil(L);
            lua_remove(L, -2);
        }
        lua_setfield(L, -2, alias.member);
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

} // namespace hailstone::test
