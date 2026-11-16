// What a plugin's code in the UI needs to call each hs function (app/lua_ui_caps.h, docs/plugins.md "What plugin
// windows may do"). The groups follow plugin/policy.h's capabilities:
//   0             reads the game or the UI's state, draws, or changes only how the UI looks (a hover, a scroll)
//   kCapCommands  changes windows and options as the slash commands of that group do: shows or hides a window,
//                 an option, a chat window's filters, the hot bars' layout, a spell set's list
//   kCapActions   acts in the game: a stock window's buttons and lists (which run its handler, whatever it does:
//                 buy, trade, destroy, accept), items, the cursor, spells, abilities, hot buttons, parcels, the menus'
//                 commands
//   kCapTarget    changes the target (the Find window's row: hs.FindNpcTarget)
//   kCapMove      moves the player (the Find window's Walk there: hs.FindNpcWalk, a /nav walk)
// hs.ChatSubmit's line goes through the command filter (mq/commands.h), which holds it to the caller's capabilities
// as any command a plugin sends: it needs nothing here. The pre-game screens, the scripts' and the
// plugins' functions and the web's check plugin code themselves (app/lua_ui_pregame.cpp, lua_ui_scripts.cpp,
// lua_ui_plugins.cpp, lua_ui_web.cpp).
#include "app/lua_ui_caps.h"

#include <set>
#include <string>
#include <vector>

#include <lua.hpp>

#include "app/lua_ui_api.h"
#include "core/log.h"
#include "plugin/policy.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

namespace {

using plugin::kCapActions;

// A plugin's code called a function its capabilities do not cover: it did nothing (said once per caller and name).
void refused(std::string_view name, std::uint32_t need) {
    static std::set<std::string> said;
    std::string key = std::string(ui::lua::LuaUi::caller_tag()) + " " + std::string(name);
    if (said.size() < 1000 && said.insert(key).second)
        log::warn("ui: {}: hs.{} needs the '{}' permission; it did nothing", ui::lua::LuaUi::caller_tag(), name,
                  plugin::caps_text(need));
}

// The gate: upvalue 1 the real function, 2 the capabilities it needs, 3 its name.
int gated(lua_State* L) {
    const auto need = static_cast<std::uint32_t>(lua_tointeger(L, lua_upvalueindex(2)));
    if (!ui::lua::LuaUi::caller_tag().empty() && (plugin::caller_caps() & need) != need) {
        refused(lua_tostring(L, lua_upvalueindex(3)), need);
        lua_pushnil(L);
        return 1;
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

// Gates the functions of the table at `t`; names are `prefix` + key.
void gate_table(lua_State* L, int t, const std::string& prefix, int depth) {
    t = lua_absindex(L, t);
    std::vector<std::string> keys;
    lua_pushnil(L);
    while (lua_next(L, t)) {
        if (lua_type(L, -2) == LUA_TSTRING && (lua_isfunction(L, -1) || (depth == 0 && lua_istable(L, -1))))
            keys.emplace_back(lua_tostring(L, -2));
        lua_pop(L, 1);
    }
    for (const std::string& key : keys) {
        lua_getfield(L, t, key.c_str());
        if (lua_istable(L, -1)) {
            gate_table(L, -1, prefix + key + ".", depth + 1);
            lua_pop(L, 1);
            continue;
        }
        const std::string name = prefix + key;
        const std::uint32_t need = ui_call_caps(name).value_or(kCapActions);
        if (need == 0) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushinteger(L, need);
        lua_pushstring(L, name.c_str());
        lua_pushcclosure(L, &gated, 3);
        lua_setfield(L, t, key.c_str());
    }
}

} // namespace

std::optional<std::uint32_t> ui_call_caps(std::string_view name) { return ui_api_call_caps(name); }

void gate_ui_calls(lua_State* L) { gate_table(L, -1, "", 0); }

} // namespace hailstone::app
