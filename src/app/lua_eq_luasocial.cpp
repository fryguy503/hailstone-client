// Lua socials for the UI packs (game/lua_social.h, mq::ScriptHost): what the Lua social editor and the hot buttons'
// faces read and change. Socials are numbered 0..119 as eq.OpenSocialEditor numbers them.
//
//   eq.LuaSocialRules()                 the server's rules for Lua socials: {mode = "off" | "press" | "full", caps,
//                                       press_commands, faces}
//   eq.LuaSocial(n)                     {name, color, lines, lua, press, face, running, hash, rules = {mode, caps, press_commands, faces}}
//                                       or nil for an empty social
//   eq.SetLuaSocial(n, t)               t = {name, color, press, face [, lines]}: saved; empty press and face make it
//                                       a plain social again (with `lines`, up to five, as its command lines), an
//                                       empty name clears it
//   eq.LuaSocialFace(n)                 {label, color = {r, g, b}, cooldown, overlay, enabled, tooltip, error} or nil
//   eq.LuaSocialDryRun(n [, t [, mods]]) the commands the press would send: {ok, lines, error, mode}; t gives unsaved
//                                       code, mods = {shift, ctrl, alt}
//   eq.PressLuaSocial(n), eq.StopLuaSocial(n)
//   eq.LuaSocialExport(n [, t])         the share text of social n (Lua or plain), or of t = {name, color, press,
//                                       face, lines} (an editor's unsaved social);
//   eq.LuaSocialImport(text)            -> {name, color, press, face, lua, lines} or nil, why
//   eq.LuaComplete(before)              completion for the editor at the end of `before`: start (1-based, where the
//                                       word begins), {names}: mq.TLO paths, mq.*, button.*
//   eq.LuaSocialEditRequest()           a social the editor was asked to open (a right click, /luasocial), or nil
//   eq.OpenLuaSocialEditor(n), eq.FreeSocial(), eq.SocialToCursor(n)
//   eq.SocialColors()                   the 20 social colours as {r, g, b} (the social editor's colour buttons)

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lua.hpp>

#include "app/actions_ui.h"
#include "app/lua_eq.h"
#include "app/lua_eq_windows.h"
#include "core/sha256.h"
#include "core/distribution.h"
#include "game/hotbuttons.h"
#include "game/lua_social.h"
#include "mq/lua_mq.h"
#include "mq/tlo.h"

namespace hailstone::app {

namespace {

constexpr int kSocials = game::kSocialPages * game::kSocialButtons;
constexpr int kEditorState = 1000; // the Lua state an editor's dry run uses (mq::ButtonPress::state)

EqHost& host_of(lua_State* L) { return *static_cast<EqHost*>(lua_touserdata(L, lua_upvalueindex(1))); }

// The social number argument, or -1 when it is out of range or there are no socials.
int social_arg(lua_State* L, int i) {
    EqHost& h = host_of(L);
    lua_Integer n = luaL_checkinteger(L, i);
    return h.hot_buttons && n >= 0 && n < kSocials ? static_cast<int>(n) : -1;
}

const char* mode_name(plugin::ButtonMode m) {
    switch (m) {
    case plugin::ButtonMode::Press: return "press";
    case plugin::ButtonMode::Full: return "full";
    default: return "off";
    }
}

void set_str(lua_State* L, const char* key, std::string_view v) {
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, key);
}
void set_int(lua_State* L, const char* key, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
}
void set_bool(lua_State* L, const char* key, bool v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, key);
}

void push_rules(lua_State* L, const plugin::ButtonRules& r) {
    lua_createtable(L, 0, 4);
    set_str(L, "mode", mode_name(r.mode));
    set_str(L, "caps", plugin::caps_text(r.caps));
    set_int(L, "press_commands", r.press_commands);
    set_bool(L, "faces", r.faces);
}

// The string at stack index i ("" for any other value). For fields read onto the stack first: lua_getfield runs a
// table's __index, whose error would longjmp past any std::string alive then (Lua is built as C).
std::string string_at(lua_State* L, int i) {
    std::size_t n = 0;
    const char* p = lua_type(L, i) == LUA_TSTRING ? lua_tolstring(L, i, &n) : "";
    return std::string(p, n);
}

void push_file(lua_State* L, const game::LuaSocialFile& f) {
    lua_createtable(L, 0, 4);
    set_str(L, "name", f.name);
    set_int(L, "color", f.color);
    set_str(L, "press", f.lua.press);
    set_str(L, "face", f.lua.face);
    set_bool(L, "lua", !f.lua.empty());
    lua_createtable(L, game::kSocialLines, 0);
    for (int i = 0; i < game::kSocialLines; ++i) {
        const auto k = static_cast<std::size_t>(i);
        const std::string empty;
        const std::string& line = k < f.lines.size() ? f.lines[k] : empty;
        lua_pushlstring(L, line.data(), line.size());
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "lines");
}

int LuaSocialRules(lua_State* L) {
    EqHost& h = host_of(L);
    push_rules(L, h.scripts ? h.scripts->button_rules({}) : plugin::ButtonRules{});
    return 1;
}

int LuaSocial(lua_State* L) {
    EqHost& h = host_of(L);
    int n = social_arg(L, 1);
    if (n < 0 || h.hot_buttons->social(n).empty()) {
        lua_pushnil(L);
        return 1;
    }
    const game::Social& s = h.hot_buttons->social(n);
    lua_createtable(L, 0, 10);
    set_str(L, "name", s.name);
    set_int(L, "color", s.color);
    lua_createtable(L, game::kSocialLines, 0);
    for (int i = 0; i < game::kSocialLines; ++i) {
        const std::string& line = s.lines[static_cast<std::size_t>(i)];
        lua_pushlstring(L, line.data(), line.size());
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "lines");
    set_bool(L, "lua", s.is_lua());
    set_str(L, "press", s.lua.press);
    set_str(L, "face", s.lua.face);
    set_bool(L, "running", h.scripts && h.scripts->button_running(n));
    set_str(L, "hash", to_hex(game::lua_social_hash(s.lua)));
    push_rules(L, h.scripts ? h.scripts->button_rules(s.lua) : plugin::ButtonRules{});
    lua_setfield(L, -2, "rules");
    return 1;
}

int SetLuaSocial(lua_State* L) {
    if (distribution::managed()) return 0;
    EqHost& h = host_of(L);
    int n = social_arg(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (n < 0) return 0;
    // Every field onto the stack before any std::string exists.
    const int base = lua_gettop(L);
    for (const char* k : {"name", "color", "press", "face", "lines"}) lua_getfield(L, 2, k);
    std::string name = string_at(L, base + 1);
    int color = static_cast<int>(lua_tointeger(L, base + 2));
    game::LuaSocial lua{string_at(L, base + 3), string_at(L, base + 4)};
    std::optional<std::array<std::string, game::kSocialLines>> lines;
    if (lua_istable(L, base + 5)) {
        lines.emplace();
        for (int i = 0; i < game::kSocialLines; ++i) {
            lua_rawgeti(L, base + 5, i + 1); // (raw: no metamethod can raise past the strings)
            (*lines)[static_cast<std::size_t>(i)] = string_at(L, -1).substr(0, 255); // the editor's limit
            lua_pop(L, 1);
        }
    }
    lua_settop(L, base);
    game::Social& s = h.hot_buttons->social(n);
    if (name.empty()) {
        s = game::Social{};
    } else {
        s.name = name.substr(0, 15);
        s.color = color;
        s.lua = std::move(lua);
        if (s.is_lua()) s.lines = {};
        else if (lines) s.lines = std::move(*lines);
    }
    if (h.actions) h.actions->social_changed(n);
    else if (h.scripts) h.scripts->forget_button(n);
    lua_pushboolean(L, 1);
    return 1;
}

int LuaSocialFace(lua_State* L) {
    if (distribution::managed()) return 0;
    EqHost& h = host_of(L);
    int n = social_arg(L, 1);
    if (n < 0 || !h.scripts) return 0;
    const game::Social& s = h.hot_buttons->social(n);
    if (!s.is_lua()) return 0;
    const mq::ButtonFace& f = h.scripts->button_face(n, s.name, s.lua);
    if (!f.has && f.error.empty()) return 0;
    lua_createtable(L, 0, 7);
    set_str(L, "label", f.label);
    if (f.color) {
        lua_createtable(L, 3, 0);
        for (int i = 0; i < 3; ++i) {
            lua_pushnumber(L, ((*f.color >> (16 - 8 * i)) & 0xff) / 255.0);
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "color");
    }
    lua_pushnumber(L, f.cooldown);
    lua_setfield(L, -2, "cooldown");
    set_str(L, "overlay", f.overlay);
    set_bool(L, "enabled", f.enabled);
    set_str(L, "tooltip", f.tooltip);
    if (!f.error.empty()) set_str(L, "error", f.error);
    return 1;
}

int LuaSocialDryRun(lua_State* L) {
    if (distribution::managed()) {
        lua_createtable(L, 0, 3);
        set_bool(L, "ok", false);
        set_str(L, "error", "Lua socials are disabled in this distribution");
        set_str(L, "mode", "off");
        return 1;
    }
    EqHost& h = host_of(L);
    int n = social_arg(L, 1);
    if (n < 0 || !h.scripts) return 0;
    const game::Social& s = h.hot_buttons->social(n);
    // The caller's fields are read (their __index may raise) before the ButtonPress and its strings exist.
    const bool edited = lua_istable(L, 2);
    const int base = lua_gettop(L);
    if (edited) {
        lua_getfield(L, 2, "press");
        lua_getfield(L, 2, "face");
    }
    bool mods[3] = {};
    if (lua_istable(L, 3)) {
        int i = 0;
        for (const char* key : {"shift", "ctrl", "alt"}) {
            lua_getfield(L, 3, key);
            mods[i++] = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
    }
    mq::ButtonPress p;
    p.social = n;
    p.name = s.name.empty() ? "new" : s.name;
    p.lua = s.lua;
    if (edited) {
        p.lua = {string_at(L, base + 1), string_at(L, base + 2)};
        p.state = kEditorState + n; // unsaved code: not the social's own state
    }
    lua_settop(L, base);
    p.shift = mods[0];
    p.ctrl = mods[1];
    p.alt = mods[2];
    mq::ButtonDryRun d = h.scripts->dry_run_button(p);
    lua_createtable(L, 0, 4);
    set_bool(L, "ok", d.ok);
    set_str(L, "error", d.error);
    set_str(L, "mode", mode_name(d.rules.mode));
    lua_createtable(L, static_cast<int>(d.lines.size()), 0);
    for (std::size_t i = 0; i < d.lines.size(); ++i) {
        lua_pushlstring(L, d.lines[i].data(), d.lines[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "lines");
    return 1;
}

int PressLuaSocial(lua_State* L) {
    if (distribution::managed()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    EqHost& h = host_of(L);
    int n = social_arg(L, 1);
    if (n < 0 || !h.scripts || !h.hot_buttons->social(n).is_lua()) return 0;
    const game::Social& s = h.hot_buttons->social(n);
    mq::ButtonPress p;
    p.social = n;
    p.name = s.name;
    p.lua = s.lua;
    lua_pushboolean(L, h.scripts->press_button(p));
    return 1;
}

int StopLuaSocial(lua_State* L) {
    EqHost& h = host_of(L);
    int n = social_arg(L, 1);
    lua_pushboolean(L, n >= 0 && h.scripts && h.scripts->stop_button(n));
    return 1;
}

int LuaSocialExport(lua_State* L) {
    EqHost& h = host_of(L);
    int n = social_arg(L, 1);
    if (n < 0) return 0;
    const game::Social& s = h.hot_buttons->social(n);
    if (lua_istable(L, 2)) {
        // The editor's unsaved social: every field onto the stack before any std::string exists.
        const int base = lua_gettop(L);
        for (const char* k : {"name", "color", "press", "face", "lines"}) lua_getfield(L, 2, k);
        game::LuaSocialFile f{string_at(L, base + 1), static_cast<int>(lua_tointeger(L, base + 2)),
                              {string_at(L, base + 3), string_at(L, base + 4)}, {}};
        if (lua_istable(L, base + 5))
            for (int i = 0; i < game::kSocialLines; ++i) {
                lua_rawgeti(L, base + 5, i + 1);
                f.lines.push_back(string_at(L, -1));
                lua_pop(L, 1);
            }
        lua_settop(L, base);
        std::string text = game::export_lua_social(f);
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }
    std::string text = game::export_lua_social(
        {s.name, s.color, s.lua, std::vector<std::string>(s.lines.begin(), s.lines.end())});
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int LuaSocialImport(lua_State* L) {
    auto f = game::import_lua_social(luaL_checkstring(L, 1));
    if (!f) {
        lua_pushnil(L);
        lua_pushstring(L, "not a Hailstone social (it starts with hailstone-social:1:)");
        return 2;
    }
    push_file(L, *f);
    return 1;
}

// ---------------------------------------------------------------- completion

constexpr const char* kMqNames[] = {
    "TLO", "cmd", "cmdf", "delay", "event", "unevent", "doevents", "flushevents", "bind", "unbind", "parse", "gettime",
    "exit", "getAllSpawns", "getFilteredSpawns", "pickle", "imgui", "configDir", "luaDir"};
constexpr const char* kButtonNames[] = {"social", "bar", "button", "shift", "ctrl", "alt", "presses"};

bool is_ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool starts_nocase(std::string_view name, std::string_view prefix) {
    if (name.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(name[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

// MQ's path for the TLO chain at the end of Lua text ("x = mq.TLO.Me.Buff(1).Du" -> "Me.Buff[1].Du"); nullopt when
// the text does not end in one, or ends inside a call's arguments.
std::optional<std::string> lua_tlo_path(std::string_view before) {
    auto at = before.rfind("TLO.");
    if (at == std::string_view::npos || (at > 0 && is_ident(before[at - 1]))) return std::nullopt;
    std::string out;
    int depth = 0;
    for (char c : before.substr(at + 4)) {
        if (depth > 0) {
            if (c == ')' && --depth == 0) out += ']';
            else if (c == '(') ++depth, out += c;
            else if (c != '\'' && c != '"') out += c;
        } else if (c == '(') {
            depth = 1;
            out += '[';
        } else if (is_ident(c) || c == '.') {
            out += c;
        } else {
            return std::nullopt;
        }
    }
    if (depth > 0) return std::nullopt;
    return out;
}

int LuaComplete(lua_State* L) {
    EqHost& h = host_of(L);
    std::size_t len = 0;
    const char* text = luaL_checklstring(L, 1, &len);
    std::string_view before(text, len);
    std::size_t word = before.size();
    while (word > 0 && is_ident(before[word - 1])) --word;
    std::string_view prefix = before.substr(word);
    std::vector<std::string> items;
    auto take = [&](std::span<const char* const> names) {
        for (const char* n : names)
            if (starts_nocase(n, prefix)) items.emplace_back(n);
    };
    auto ends_with_object = [&](std::string_view object) {
        if (word < object.size() + 1 || before[word - 1] != '.') return false;
        std::size_t start = word - 1 - object.size();
        return before.substr(start, object.size()) == object && (start == 0 || !is_ident(before[start - 1]));
    };
    if (auto path = lua_tlo_path(before); path && h.registry && h.tlo_context) {
        items = h.registry->complete(h.tlo_context(), *path);
    } else if (ends_with_object("mq")) {
        take(kMqNames);
    } else if (ends_with_object("button")) {
        take(kButtonNames);
    }
    if (items.size() > 50) items.resize(50);
    lua_pushinteger(L, static_cast<lua_Integer>(word + 1));
    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        lua_pushlstring(L, items[i].data(), items[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 2;
}

// ---------------------------------------------------------------- opening, new socials

int LuaSocialEditRequest(lua_State* L) {
    if (distribution::managed()) return 0;
    EqHost& h = host_of(L);
    if (h.lua_social_edit < 0) return 0;
    lua_pushinteger(L, h.lua_social_edit);
    h.lua_social_edit = -1;
    return 1;
}

int OpenLuaSocialEditor(lua_State* L) {
    if (distribution::managed()) return 0;
    EqHost& h = host_of(L);
    int n = social_arg(L, 1);
    if (n >= 0) h.lua_social_edit = n;
    return 0;
}

// The first empty social, or nil.
int FreeSocial(lua_State* L) {
    EqHost& h = host_of(L);
    if (!h.hot_buttons) return 0;
    for (int i = 0; i < kSocials; ++i)
        if (h.hot_buttons->social(i).empty()) {
            lua_pushinteger(L, i);
            return 1;
        }
    return 0;
}

// The social as a hot button on the cursor, to drop on a bar.
int SocialToCursor(lua_State* L) {
    EqHost& h = host_of(L);
    int n = social_arg(L, 1);
    if (n < 0 || h.hot_buttons->social(n).empty()) return 0;
    game::HotButton b;
    b.type = game::HotButtonType::Social;
    b.slot = n;
    h.cursor_button = b;
    ++h.cursor_serial;
    lua_pushboolean(L, 1);
    return 1;
}

int SocialColors(lua_State* L) {
    lua_createtable(L, 20, 0);
    for (int i = 0; i < 20; ++i) {
        Rgb c = social_text_color(i);
        lua_createtable(L, 3, 0);
        for (int k = 0; k < 3; ++k) {
            lua_pushnumber(L, c[static_cast<std::size_t>(k)]);
            lua_rawseti(L, -2, k + 1);
        }
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

} // namespace

void register_luasocial_api(lua_State* L, EqHost& h) {
    static const luaL_Reg fns[] = {{"LuaSocialRules", LuaSocialRules},
                                   {"LuaSocial", LuaSocial},
                                   {"SetLuaSocial", SetLuaSocial},
                                   {"LuaSocialFace", LuaSocialFace},
                                   {"LuaSocialDryRun", LuaSocialDryRun},
                                   {"PressLuaSocial", PressLuaSocial},
                                   {"StopLuaSocial", StopLuaSocial},
                                   {"LuaSocialExport", LuaSocialExport},
                                   {"LuaSocialImport", LuaSocialImport},
                                   {"LuaComplete", LuaComplete},
                                   {"LuaSocialEditRequest", LuaSocialEditRequest},
                                   {"OpenLuaSocialEditor", OpenLuaSocialEditor},
                                   {"FreeSocial", FreeSocial},
                                   {"SocialToCursor", SocialToCursor},
                                   {"SocialColors", SocialColors},
                                   {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h);
    luaL_setfuncs(L, fns, 1);
}

} // namespace hailstone::app
