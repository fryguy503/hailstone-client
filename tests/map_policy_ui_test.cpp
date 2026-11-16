// Generic map ownership presentation over the actual map/minimap/Find Lua scripts.
#include <doctest/doctest.h>
#include <filesystem>
#include <string>
#include <lua.hpp>

TEST_CASE("Map policy presenters: ownership retires cached NPC UI without changing preferences") {
    lua_State* state=luaL_newstate();REQUIRE(state);luaL_openlibs(state);
    const auto root=std::filesystem::path(HAILSTONE_SOURCE_DIR).generic_string();
    lua_pushlstring(state,root.data(),root.size());lua_setglobal(state,"CLIENT_ROOT");
    const auto file=(std::filesystem::path(HAILSTONE_SOURCE_DIR)/"tests/map_policy_ui_test.lua").string();
    int result=luaL_loadfilex(state,file.c_str(),"t");
    if(result==LUA_OK)result=lua_pcall(state,0,0,0);
    const std::string error=result==LUA_OK?"":lua_tostring(state,-1);
    lua_close(state);CHECK_MESSAGE(result==LUA_OK,error);
}
