// The in-game browser for the UI packs (docs/browser.md; ui/<pack>/browser.lua) over plugin::WebAccess. The pack's
// own windows act as the player; a plugin's code (LuaUi::caller_tag) as its owner ("plugin:<name>" for a compiled
// plugin's, "script:<name>" for a Lua plugin's, whose windows share the script's mq.web tabs), with that owner's
// permissions (it needs 'web'), on its own tabs only.
//
//   hs.WebAvailable()            the helper is there (or not tried yet) and has not been given up
//   hs.WebOpen(url [, w, h])     a new tab, or nil and why; "example.org" becomes "https://example.org"
//   hs.WebTabs()                 the caller's tabs, oldest first
//   hs.WebState(tab)             { url, title, loading, canBack, canForward, closed, why } or nil
//   hs.WebNavigate(tab, url)     true, or false and why
//   hs.WebNav(tab, what)         "back", "forward", "reload", "stop"
//   hs.WebClose(tab)
//   hs.WebView(tab, w, h)        draws the page w x h and gives it the mouse, and the keyboard once clicked;
//                                the page takes that size. Returns whether it is hovered
//   hs.WebText(tab, text)        types the text where the page has its cursor (the gamepad pack's on-screen keys)
//   hs.WebEval(tab, script)      runs script in the page
//   hs.WebPost(tab, message)     calls the page's window.hailstone.onmessage(message)
//   hs.WebMessages(tab)          what the page posted (window.hailstone.post) since the last call; a plugin's tabs
//                                only: the player's pages have no one to talk to
//   hs.WebOpenExternal(url)      in the system's browser (http(s) only; the player's windows only)
#include <lua.hpp>

#include <optional>
#include <string>
#include <vector>

#include "app/lua_ui.h"
#include "core/web_url.h"
#include "platform/open_folder.h"
#include "plugin/lua_keep.h"
#include "plugin/policy.h"
#include "plugin/web_access.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

namespace {

// Who calls, and the texts and lists a call makes: in a box the state owns (plugin/lua_keep.h), as the pushes after
// may raise. Each function below checks its arguments before it makes its Caller, and does its C++ work inside
// plugin::cxx_guard.
struct Caller {
    plugin::WebAccess* web = nullptr;
    std::string owner;
    std::uint32_t caps = 0;
    std::string text, why;
    std::vector<std::uint32_t> tabs;
    std::optional<plugin::WebState> state;
};

// The caller's box, pushed on the stack (below what the function returns).
Caller& caller(lua_State* L) {
    auto& h = *static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    Caller& c = plugin::lua_keep<Caller>(L);
    c.web = h.web;
    const std::string_view tag = ui::lua::LuaUi::caller_tag();
    if (tag.empty()) {
        c.caps = plugin::kCapAll;
    } else {
        plugin::cxx_guard(L, [&] {
            c.owner = std::string(tag); // (plugin::UiBridge::owner_tag)
            c.caps = h.plugin_caps ? h.plugin_caps(tag) : 0;
        });
    }
    return c;
}

std::uint32_t tab_arg(lua_State* L, int i) {
    const lua_Integer t = luaL_checkinteger(L, i);
    return t > 0 && t <= 0xffffffff ? static_cast<std::uint32_t>(t) : 0;
}

int WebAvailable(lua_State* L) {
    Caller& c = caller(L);
    lua_pushboolean(L, c.web && c.web->available());
    return 1;
}

int WebOpen(lua_State* L) {
    const char* url_arg = luaL_checkstring(L, 1);
    const int w = static_cast<int>(luaL_optinteger(L, 2, 800)), h = static_cast<int>(luaL_optinteger(L, 3, 600));
    Caller& c = caller(L);
    std::uint32_t id = 0;
    plugin::cxx_guard(L, [&] {
        c.why = "the in-game browser is not installed";
        id = c.web ? c.web->open(c.owner, c.caps, url_arg, w, h, &c.why) : 0;
    });
    if (!id) {
        lua_pushnil(L);
        lua_pushstring(L, c.why.c_str());
        return 2;
    }
    lua_pushinteger(L, id);
    return 1;
}

int WebTabs(lua_State* L) {
    Caller& c = caller(L);
    if (c.web) plugin::cxx_guard(L, [&] { c.tabs = c.web->tabs(c.owner); });
    lua_newtable(L);
    lua_Integer n = 0;
    for (std::uint32_t id : c.tabs) {
        lua_pushinteger(L, id);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int WebState(lua_State* L) {
    const std::uint32_t tab = tab_arg(L, 1);
    Caller& c = caller(L);
    plugin::cxx_guard(L, [&] { c.state = c.web ? c.web->state(c.owner, tab) : std::nullopt; });
    const auto& s = c.state;
    if (!s) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 7);
    lua_pushstring(L, s->url.c_str());
    lua_setfield(L, -2, "url");
    lua_pushstring(L, s->title.c_str());
    lua_setfield(L, -2, "title");
    lua_pushboolean(L, s->loading);
    lua_setfield(L, -2, "loading");
    lua_pushboolean(L, s->can_back);
    lua_setfield(L, -2, "canBack");
    lua_pushboolean(L, s->can_forward);
    lua_setfield(L, -2, "canForward");
    lua_pushboolean(L, s->closed);
    lua_setfield(L, -2, "closed");
    lua_pushstring(L, s->why.c_str());
    lua_setfield(L, -2, "why");
    return 1;
}

int WebNavigate(lua_State* L) {
    const std::uint32_t tab = tab_arg(L, 1);
    const char* url_arg = luaL_checkstring(L, 2);
    Caller& c = caller(L);
    bool ok = false;
    plugin::cxx_guard(L, [&] {
        c.why = "the in-game browser is not installed";
        ok = c.web && c.web->navigate(c.owner, c.caps, tab, url_arg, &c.why);
    });
    lua_pushboolean(L, ok);
    if (ok) return 1;
    lua_pushstring(L, c.why.c_str());
    return 2;
}

int WebNav(lua_State* L) {
    const std::uint32_t tab = tab_arg(L, 1);
    const std::string_view what = luaL_checkstring(L, 2);
    if (what != "back" && what != "forward" && what != "reload" && what != "stop")
        return luaL_argerror(L, 2, "back, forward, reload or stop");
    Caller& c = caller(L);
    if (c.web) plugin::cxx_guard(L, [&] { c.web->nav(c.owner, c.caps, tab, std::string(what)); });
    return 0;
}

int WebClose(lua_State* L) {
    const std::uint32_t tab = tab_arg(L, 1);
    Caller& c = caller(L);
    if (c.web) plugin::cxx_guard(L, [&] { c.web->close(c.owner, tab); });
    return 0;
}

int WebText(lua_State* L) {
    const std::uint32_t tab = tab_arg(L, 1);
    const char* text_arg = luaL_checkstring(L, 2);
    Caller& c = caller(L);
    if (c.web) plugin::cxx_guard(L, [&] { c.web->text(c.owner, c.caps, tab, text_arg); });
    return 0;
}

int WebEval(lua_State* L) {
    const std::uint32_t tab = tab_arg(L, 1);
    const char* script_arg = luaL_checkstring(L, 2);
    Caller& c = caller(L);
    bool ok = false;
    if (c.web) plugin::cxx_guard(L, [&] { ok = c.web->eval(c.owner, c.caps, tab, script_arg); });
    lua_pushboolean(L, ok);
    return 1;
}

int WebPost(lua_State* L) {
    const std::uint32_t tab = tab_arg(L, 1);
    const char* message_arg = luaL_checkstring(L, 2);
    Caller& c = caller(L);
    bool ok = false;
    if (c.web) plugin::cxx_guard(L, [&] { ok = c.web->post(c.owner, c.caps, tab, message_arg); });
    lua_pushboolean(L, ok);
    return 1;
}

int WebMessages(lua_State* L) {
    const std::uint32_t tab = tab_arg(L, 1);
    Caller& c = caller(L);
    lua_newtable(L);
    if (!c.web) return 1;
    lua_Integer n = 0;
    for (;;) {
        bool more = false;
        plugin::cxx_guard(L, [&] {
            auto m = c.web->take_message(c.owner, tab);
            if ((more = m.has_value())) c.text = std::move(*m);
        });
        if (!more) break;
        lua_pushlstring(L, c.text.data(), c.text.size());
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int WebView(lua_State* L) {
    const std::uint32_t tab = tab_arg(L, 1);
    const auto w = static_cast<float>(luaL_checknumber(L, 2)), h = static_cast<float>(luaL_checknumber(L, 3));
    Caller& c = caller(L);
    bool hovered = false;
    if (c.web) plugin::cxx_guard(L, [&] { hovered = c.web->view(c.owner, c.caps, tab, w, h); });
    lua_pushboolean(L, hovered);
    return 1;
}

int WebOpenExternal(lua_State* L) {
    const char* url_arg = luaL_checkstring(L, 1);
    Caller& c = caller(L);
    // Only from the player's windows: a plugin's page in the player's own browser is out of the rules' reach.
    bool ok = false;
    if (c.owner.empty()) plugin::cxx_guard(L, [&] { ok = web::url_allowed(url_arg, {}) && platform::open_url(url_arg); });
    lua_pushboolean(L, ok);
    return 1;
}

} // namespace

void register_web_api(lua_State* L, UiHost& h) {
    static const luaL_Reg fns[] = {
        {"WebAvailable", WebAvailable}, {"WebOpen", WebOpen},       {"WebTabs", WebTabs},
        {"WebState", WebState},         {"WebNavigate", WebNavigate}, {"WebNav", WebNav},
        {"WebClose", WebClose},         {"WebView", WebView},       {"WebText", WebText},
        {"WebEval", WebEval},           {"WebPost", WebPost},       {"WebMessages", WebMessages},
        {"WebOpenExternal", WebOpenExternal}, {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h);
    luaL_setfuncs(L, fns, 1);
}

} // namespace hailstone::app
