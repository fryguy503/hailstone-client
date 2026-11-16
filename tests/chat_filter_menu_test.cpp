#include <doctest/doctest.h>
#include <lua.hpp>

#include "lua_ui_api_mock.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST_CASE("Modern chat menu: nested categories retain their parent and send the selected route") {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "ui/modern/chat.lua";
    std::ifstream file(path);
    REQUIRE(file.good());
    const std::string source{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    const auto start = source.find("local function filtersMenu(slot)");
    REQUIRE(start != std::string::npos);
    const auto end = source.find("\nlocal function ", start + 1);
    REQUIRE(end != std::string::npos);
    const std::string menu = "local API = hs.UI\n" + source.substr(start, end - start) + "\nfiltersMenu(2)";
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    struct CloseLua { lua_State* state; ~CloseLua() { lua_close(state); } } close{L};
    luaL_openlibs(L);
    const char* fixture = R"lua(
paths, clicks, stack = {}, {}, {}
ui = {
  BeginMenu = function(name)
    if name == "Hit Modes" then return false end
    stack[#stack + 1] = name:match("^(.-)##") or name
    return true
  end,
  EndMenu = function() table.remove(stack) end,
  Separator = function() end,
  MenuItem = function(name)
    local label = name:match("^(.-)##") or name
    paths[table.concat(stack, "/") .. "/" .. label] = true
    return name == "Heals received##filter101" or name == "Show All##group4"
  end,
}
hs = {
  ChatFilters = function()
    return {
      {id=106, menu=0, parent=0, group="Filters", name="Client messages", on=false},
      {id=81, menu=3, parent=0, group="Spells", name="Spell notices", on=false},
      {id=80, menu=4, parent=3, group="Direct damage", name="Other damage", on=false},
      {id=101, menu=6, parent=3, group="Healing", name="Heals received", on=false},
      {id=60, menu=9, parent=0, group="Random rolls", name="Others", on=false},
    }
  end,
  ChatFilterGroups = function()
    return {
      {id=3, parent=0, name="Spells"},
      {id=4, parent=3, name="Direct damage"},
      {id=6, parent=3, name="Healing"},
      {id=9, parent=0, name="Random rolls"},
      {id=1, parent=0, name="Chat Channel"},
    }
  end,
  ChatSetFilter = function(slot, category, on)
    clicks[#clicks + 1] = {slot, category, on}
  end,
}
)lua";
    REQUIRE(luaL_dostring(L, fixture) == LUA_OK);
    REQUIRE(hailstone::test::attach_mock_ui_api(L) == LUA_OK);
    const int result = luaL_dostring(L, menu.c_str());
    INFO((result == LUA_OK ? "" : lua_tostring(L, -1)));
    REQUIRE(result == LUA_OK);
    const char* checks = R"lua(
assert(paths["Filters/Client messages"])
assert(paths["Filters/Spells/Spell notices"])
assert(paths["Filters/Spells/Direct damage/Other damage"])
assert(paths["Filters/Spells/Healing/Heals received"])
assert(paths["Filters/Random rolls/Others"])
assert(not paths["Filters/Healing/Heals received"])
assert(not paths["Filters/Chat Channel/Show All"]) -- no joined channel: no Chat Channel menu
assert(#stack == 0)
assert(#clicks == 2)
assert(clicks[1][1] == 2 and clicks[1][2] == "Direct damage" and clicks[1][3])
assert(clicks[2][1] == 2 and clicks[2][2] == 101 and clicks[2][3])
)lua";
    const int checked = luaL_dostring(L, checks);
    INFO((checked == LUA_OK ? "" : lua_tostring(L, -1)));
    CHECK(checked == LUA_OK);
}
