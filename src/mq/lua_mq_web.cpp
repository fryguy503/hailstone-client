// mq.web: the in-game browser for scripts and Lua plugins (docs/browser.md, docs/plugins.md), over
// plugin::WebAccess as "script:<name>". Each call needs the server's 'web' permission (a refusal is a Lua error,
// as for mq.imgui); addresses are http(s) on the hosts the server's [web] allow names; a script sees only its own
// tabs, which close when it stops.
//
//   tab = mq.web.open(url [, w, h])     nil, why when refused
//   ok, why = mq.web.navigate(tab, url)
//   mq.web.nav(tab, "back"|"forward"|"reload"|"stop")
//   mq.web.close(tab)
//   mq.web.eval(tab, script)            runs script in the page
//   mq.web.post(tab, message)           the page's window.hailstone.onmessage(message)
//   mq.web.messages(tab)                what the page posted (window.hailstone.post) since the last call
//   mq.web.text(tab, text)              typed where the page has its cursor
//   mq.web.state(tab)                   { url, title, loading, canBack, canForward, closed, why } or nil
//   mq.web.tabs()                       the script's tabs
//   mq.web.view(tab, w, h)              in an mq.imgui window: draws the page and gives it the mouse and keyboard
//   mq.web.available()
#include <lua.hpp>

#include <string>
#include <string_view>

#include "mq/lua_script.h"
#include "plugin/policy.h"
#include "plugin/web_access.h"

namespace hailstone::mq {

namespace {

struct Caller {
    plugin::WebAccess& web;
    std::string owner;
    std::uint32_t caps;
};

// The script's browser: a Lua error without 'web' or a browser. Asked before the arguments are read, as before.
void check(lua_State* L, const char* what) {
    need_caps(L, plugin::kCapWeb, what);
    if (!Script::of(L).host->env.web) luaL_error(L, "%s: this client has no browser", what);
}

// Made only once every argument is read: Lua is built as C, so an argument error longjmps past C++ objects. What a
// function pushes while it lives it pushes under HostWork, so a push is not refused at the script's memory limit and
// raises past it only when the system itself is out of memory.
Caller caller(lua_State* L) {
    const Script& s = Script::of(L);
    return {*s.host->env.web, "script:" + s.name, s.caps};
}

std::uint32_t tab_arg(lua_State* L, int i) {
    const lua_Integer t = luaL_checkinteger(L, i);
    return t > 0 && t <= 0xffffffff ? static_cast<std::uint32_t>(t) : 0;
}

int l_available(lua_State* L) {
    Script& s = Script::of(L);
    lua_pushboolean(L, (s.caps & plugin::kCapWeb) && s.host->env.web && s.host->env.web->available());
    return 1;
}

int l_open(lua_State* L) {
    check(L, "mq.web.open");
    const char* url = luaL_checkstring(L, 1);
    const int w = static_cast<int>(luaL_optinteger(L, 2, 800)), h = static_cast<int>(luaL_optinteger(L, 3, 600));
    std::uint32_t id = 0;
    {
        HostWork unlimited(Script::of(L));
        std::string why;
        Caller c = caller(L);
        id = c.web.open(c.owner, c.caps, url, w, h, &why);
        if (!id) lua_pushstring(L, why.c_str());
    }
    if (!id) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    lua_pushinteger(L, id);
    return 1;
}

int l_navigate(lua_State* L) {
    check(L, "mq.web.navigate");
    const std::uint32_t tab = tab_arg(L, 1);
    const char* url = luaL_checkstring(L, 2);
    bool ok = false;
    {
        HostWork unlimited(Script::of(L));
        std::string why;
        Caller c = caller(L);
        ok = c.web.navigate(c.owner, c.caps, tab, url, &why);
        lua_pushboolean(L, ok);
        if (!ok) lua_pushstring(L, why.c_str());
    }
    return ok ? 1 : 2;
}

int l_nav(lua_State* L) {
    check(L, "mq.web.nav");
    const std::uint32_t tab = tab_arg(L, 1);
    const std::string_view what = luaL_checkstring(L, 2);
    if (what != "back" && what != "forward" && what != "reload" && what != "stop")
        return luaL_argerror(L, 2, "back, forward, reload or stop");
    Caller c = caller(L);
    c.web.nav(c.owner, c.caps, tab, std::string(what));
    return 0;
}

int l_close(lua_State* L) {
    check(L, "mq.web.close");
    const std::uint32_t tab = tab_arg(L, 1);
    Caller c = caller(L);
    c.web.close(c.owner, tab);
    return 0;
}

int l_eval(lua_State* L) {
    check(L, "mq.web.eval");
    const std::uint32_t tab = tab_arg(L, 1);
    const char* script = luaL_checkstring(L, 2);
    Caller c = caller(L);
    lua_pushboolean(L, c.web.eval(c.owner, c.caps, tab, script));
    return 1;
}

int l_post(lua_State* L) {
    check(L, "mq.web.post");
    const std::uint32_t tab = tab_arg(L, 1);
    const char* message = luaL_checkstring(L, 2);
    Caller c = caller(L);
    lua_pushboolean(L, c.web.post(c.owner, c.caps, tab, message));
    return 1;
}

int l_text(lua_State* L) {
    check(L, "mq.web.text");
    const std::uint32_t tab = tab_arg(L, 1);
    const char* text = luaL_checkstring(L, 2);
    Caller c = caller(L);
    lua_pushboolean(L, c.web.text(c.owner, c.caps, tab, text));
    return 1;
}

int l_messages(lua_State* L) {
    check(L, "mq.web.messages");
    const std::uint32_t tab = tab_arg(L, 1);
    Script& s = Script::of(L);
    HostWork unlimited(s); // (a message is at most 1 MB, and at most 8 MB wait: web_access.h)
    lua_newtable(L);
    // Each message is moved into the Script's scratch in a block of its own and pushed from there, so no C++ object
    // of this frame is alive across a push (one the system's own memory refuses still longjmps).
    for (lua_Integer n = 1;; ++n) {
        bool got = false;
        {
            Caller c = caller(L);
            if (auto m = c.web.take_message(c.owner, tab)) {
                s.scratch = std::move(*m);
                got = true;
            }
        }
        if (!got) break;
        lua_pushlstring(L, s.scratch.data(), s.scratch.size());
        lua_rawseti(L, -2, n);
    }
    std::string().swap(s.scratch); // up to 1 MB
    return 1;
}

int l_state(lua_State* L) {
    check(L, "mq.web.state");
    const std::uint32_t tab = tab_arg(L, 1);
    HostWork unlimited(Script::of(L));
    Caller c = caller(L);
    auto st = c.web.state(c.owner, tab);
    if (!st) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 7);
    lua_pushstring(L, st->url.c_str());
    lua_setfield(L, -2, "url");
    lua_pushstring(L, st->title.c_str());
    lua_setfield(L, -2, "title");
    lua_pushboolean(L, st->loading);
    lua_setfield(L, -2, "loading");
    lua_pushboolean(L, st->can_back);
    lua_setfield(L, -2, "canBack");
    lua_pushboolean(L, st->can_forward);
    lua_setfield(L, -2, "canForward");
    lua_pushboolean(L, st->closed);
    lua_setfield(L, -2, "closed");
    lua_pushstring(L, st->why.c_str());
    lua_setfield(L, -2, "why");
    return 1;
}

int l_tabs(lua_State* L) {
    check(L, "mq.web.tabs");
    HostWork unlimited(Script::of(L));
    Caller c = caller(L);
    lua_newtable(L);
    lua_Integer n = 0;
    for (std::uint32_t id : c.web.tabs(c.owner)) {
        lua_pushinteger(L, id);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int l_view(lua_State* L) {
    check(L, "mq.web.view");
    const std::uint32_t tab = tab_arg(L, 1);
    const auto w = static_cast<float>(luaL_checknumber(L, 2)), h = static_cast<float>(luaL_checknumber(L, 3));
    Caller c = caller(L);
    lua_pushboolean(L, c.web.view(c.owner, c.caps, tab, w, h));
    return 1;
}

} // namespace

void push_web_module(lua_State* L) {
    lua_newtable(L);
    for (auto [n, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
             {"available", l_available}, {"open", l_open}, {"navigate", l_navigate}, {"nav", l_nav},
             {"close", l_close}, {"eval", l_eval}, {"post", l_post}, {"text", l_text}, {"messages", l_messages},
             {"state", l_state}, {"tabs", l_tabs}, {"view", l_view}}) {
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, n);
    }
}

} // namespace hailstone::mq
