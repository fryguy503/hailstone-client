// hs.gamepad: the gamepad for the Lua windows (docs/gamepad.md section 5, the contract with ui/gamepad). A client
// extension: RoF2 has no gamepad, so there is no original API to follow. Buttons and axes go by the names of
// platform/gamepad.h; bars, buttons, sets and slots are 1-based.

#include <algorithm>

#include <lua.hpp>

#include "app/gamepad.h"
#include "app/lua_ui.h"
#include "net/game_connection.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

namespace {

Gamepad* pad(lua_State* L) {
    auto* host = static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    return host ? host->gamepad : nullptr;
}

const platform::GamepadState& state(lua_State* L) {
    static const platform::GamepadState none;
    Gamepad* g = pad(L);
    return g ? g->state() : none;
}

// With the interface locked (/safelock) the buttons reach only the chat input's windows (pad_chat_window): the rest of
// the UI has no mouse then (__ProcessMouseEvents), and the pad stands in for it.
bool buttons_locked(lua_State* L) {
    auto* host = static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    return host && host->conn && host->in_game && host->conn->interface_locked() &&
           !pad_chat_window(ui::lua::LuaUi::drawing_window());
}

std::optional<platform::PadButton> button_arg(lua_State* L, int i) {
    const char* name = luaL_optstring(L, i, "");
    return platform::pad_button_by_name(name);
}

// connected, name = hs.gamepad.connected()
int Connected(lua_State* L) {
    const auto& s = state(L);
    lua_pushboolean(L, s.connected);
    lua_pushstring(L, s.connected ? s.name.c_str() : "");
    return 2;
}

// "xbox" | "playstation" | "nintendo" | "generic"
int Type(lua_State* L) {
    lua_pushstring(L, platform::pad_type_name(state(L).type));
    return 1;
}

// hs.gamepad.down(b) / pressed(b) / released(b): held, went down this frame, went up this frame.
int Down(lua_State* L) {
    auto b = button_arg(L, 1);
    lua_pushboolean(L, b && state(L).is_down(*b) && !buttons_locked(L));
    return 1;
}
int Pressed(lua_State* L) {
    auto b = button_arg(L, 1);
    lua_pushboolean(L, b && state(L).was_pressed(*b) && !buttons_locked(L));
    return 1;
}
int Released(lua_State* L) {
    auto b = button_arg(L, 1);
    lua_pushboolean(L, b && state(L).was_released(*b) && !buttons_locked(L));
    return 1;
}

// hs.gamepad.axis(a): sticks -1..1 after the dead zone, triggers 0..1 (0 for an unknown name).
int Axis(lua_State* L) {
    auto a = platform::pad_axis_by_name(luaL_optstring(L, 1, ""));
    lua_pushnumber(L, a ? state(L).axis(*a) : 0.0f);
    return 1;
}

int Mode(lua_State* L) {
    Gamepad* g = pad(L);
    lua_pushstring(L, pad_mode_name(g ? g->mode() : PadMode::Game));
    return 1;
}

// hs.gamepad.set_mode("game" | "menu" | "cursor"): the mode now (unchanged for an unknown name).
int SetMode(lua_State* L) {
    Gamepad* g = pad(L);
    if (g)
        if (auto m = pad_mode_by_name(luaL_checkstring(L, 1))) g->set_mode(*m);
    return Mode(L);
}

// 0 none, 1 LT, 2 RT, 3 LT+RT (game mode only).
int CrossSet(lua_State* L) {
    Gamepad* g = pad(L);
    lua_pushinteger(L, g ? g->cross_set() : 0);
    return 1;
}

// bar, button = hs.gamepad.cross_slot(set, slot); nil for a set or slot out of range.
int CrossSlot(lua_State* L) {
    Gamepad* g = pad(L);
    if (!g) return 0;
    game::CrossSlot c = g->bindings().cross(static_cast<int>(luaL_checkinteger(L, 1)),
                                            static_cast<int>(luaL_checkinteger(L, 2)));
    if (c.bar == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, c.bar);
    lua_pushinteger(L, c.button);
    return 2;
}

// ok = hs.gamepad.set_cross_slot(set, slot, bar, button) (an addition for the Options page: the gamepad.cross keys)
int SetCrossSlot(lua_State* L) {
    Gamepad* g = pad(L);
    auto n = [&](int i) { return static_cast<int>(luaL_checkinteger(L, i)); };
    lua_pushboolean(L, g && g->set_cross_slot(n(1), n(2), n(3), n(4)));
    return 1;
}

// The connected pad's label for a button ("" for an unknown name).
int Glyph(lua_State* L) {
    auto b = button_arg(L, 1);
    lua_pushstring(L, b ? game::pad_glyph(state(L).type, *b) : "");
    return 1;
}

// hs.gamepad.binding(input): the action text ("" for a chord with no binding of its own: its cross slot or the
// plain button applies); nil for an unknown input.
int Binding(lua_State* L) {
    Gamepad* g = pad(L);
    auto text = g ? g->binding_text(luaL_checkstring(L, 1)) : std::nullopt;
    if (!text) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, text->c_str());
    return 1;
}

// ok = hs.gamepad.set_binding(input, action): nil or "" puts the default back.
int SetBinding(lua_State* L) {
    Gamepad* g = pad(L);
    lua_pushboolean(L, g && g->set_binding_text(luaL_checkstring(L, 1), luaL_optstring(L, 2, "")));
    return 1;
}

// hs.gamepad.setting(name): the gamepad.* number ("deadzone" or "gamepad.deadzone"; booleans 0 / 1), nil if none.
int Setting(lua_State* L) {
    Gamepad* g = pad(L);
    auto v = g ? g->setting(luaL_checkstring(L, 1)) : std::nullopt;
    if (!v) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, *v);
    return 1;
}

// ok = hs.gamepad.set_setting(name, value) (booleans: a number or true / false); saved to hailstone.ini. False for
// NaN or an infinity (GamepadSettings::set).
int SetSetting(lua_State* L) {
    Gamepad* g = pad(L);
    float v = lua_isboolean(L, 2) ? (lua_toboolean(L, 2) ? 1.0f : 0.0f) : static_cast<float>(luaL_checknumber(L, 2));
    lua_pushboolean(L, g && g->set_setting(luaL_checkstring(L, 1), v));
    return 1;
}

// ok = hs.gamepad.rumble(low, high, ms): strengths 0..1, ms at most Gamepad::kMaxRumbleMs; false without a pad,
// rumble, with gamepad.rumble off, or for a NaN or infinite strength.
int Rumble(lua_State* L) {
    Gamepad* g = pad(L);
    const lua_Integer ms = std::clamp<lua_Integer>(luaL_optinteger(L, 3, 200), 0, Gamepad::kMaxRumbleMs);
    lua_pushboolean(L, g && g->rumble(static_cast<float>(luaL_optnumber(L, 1, 0.5)),
                                      static_cast<float>(luaL_optnumber(L, 2, 0.5)), static_cast<int>(ms)));
    return 1;
}

// hs.gamepad.request_focus(window): menu mode, ImGui's focus on that window (its ui.Begin title) from the next frame.
int RequestFocus(lua_State* L) {
    if (Gamepad* g = pad(L)) g->request_focus(luaL_checkstring(L, 1));
    return 0;
}

// ok = hs.gamepad.drop_cursor(): drops the cursor item on the ground, as a click on the world with an item held does
// (CEverQuest::LMouseUp: GameConnection::request_drop_cursor, then its confirmation, eqstr 9057, when it asks). The
// pad has no pointer to click the world with. False when nothing is held or the drop was refused.
int DropCursor(lua_State* L) {
    auto* host = static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    bool ok = false;
    if (host && host->conn && host->in_game && host->conn->inventory().cursor()) {
        ok = true;
        if (host->conn->request_drop_cursor() && host->drop_confirm) *host->drop_confirm = true;
    }
    lua_pushboolean(L, ok);
    return 1;
}

// hs.gamepad.takes_b(): a window of the UI closes itself on B this frame (game mode), so B leaves the target alone.
int TakesB(lua_State* L) {
    if (Gamepad* g = pad(L)) g->window_takes_b();
    return 0;
}

} // namespace

void register_gamepad_api(lua_State* L, UiHost& host) {
    static const luaL_Reg fns[] = {
        {"connected", Connected}, {"type", Type}, {"down", Down}, {"pressed", Pressed}, {"released", Released},
        {"axis", Axis}, {"mode", Mode}, {"set_mode", SetMode}, {"cross_set", CrossSet}, {"cross_slot", CrossSlot},
        {"set_cross_slot", SetCrossSlot}, {"glyph", Glyph}, {"binding", Binding}, {"set_binding", SetBinding},
        {"setting", Setting}, {"set_setting", SetSetting}, {"rumble", Rumble}, {"request_focus", RequestFocus},
        {"drop_cursor", DropCursor}, {"takes_b", TakesB},
        {nullptr, nullptr}};
    lua_newtable(L);
    lua_pushlightuserdata(L, &host);
    luaL_setfuncs(L, fns, 1);
    lua_setfield(L, -2, "gamepad");
}

} // namespace hailstone::app
