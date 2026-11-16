// The Lua scripts' part of the `hs` table (app/lua_ui_plugins.h): the Scripts window's list, run, stop, pause and the
// character's autorun list, over the script host (mq/lua_mq.h). Running, stopping and the autorun list are the
// player's: a plugin's code in the UI (LuaUi::caller_tag) is refused them, and its list holds only its own row.
#include "app/lua_ui_plugins.h"

#include <algorithm>
#include <chrono>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include <lua.hpp>

#include "app/lua_ui.h"
#include "game/slash_commands.h"
#include "mq/lua_mq.h"
#include "plugin/lua_keep.h"
#include "plugin/policy.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

namespace {

// A row of hs.Scripts().
struct Row {
    std::string name, status, error, why, caps;
    bool found = false, allowed = false, server = false, kept = false;
};

// found()'s last answer: it reads the folders, and the window asks each frame. It also holds hs.Scripts()'s rows and
// folder names while they are pushed: it is the state's (its __gc frees it), and a push that fails longjmps past the
// C function's own objects (Lua is built as C; plugin/lua_keep.h).
struct FoundCache {
    std::vector<mq::ScriptHost::Found> found;
    std::chrono::steady_clock::time_point at{};
    bool valid = false;
    std::vector<Row> rows;
    std::vector<std::string> folders;
};
constexpr auto kRescan = std::chrono::seconds(1);

UiHost& host_of(lua_State* L) { return *static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1))); }
FoundCache& cache_of(lua_State* L) { return *static_cast<FoundCache*>(lua_touserdata(L, lua_upvalueindex(2))); }
bool plugin_code() { return !ui::lua::LuaUi::caller_tag().empty(); }

int cache_gc(lua_State* L) {
    static_cast<FoundCache*>(lua_touserdata(L, 1))->~FoundCache();
    return 0;
}

std::string status_word(const std::string& s) {
    if (s == "RUNNING") return "running";
    if (s == "PAUSED") return "paused";
    if (s == "EXITED") return "exited";
    if (s == "ERROR") return "error";
    return {};
}

void set_string(lua_State* L, const char* key, const std::string& v) {
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, key);
}
void set_bool(lua_State* L, const char* key, bool v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, key);
}

// list, info = hs.Scripts([rescan])
int Scripts(lua_State* L) {
    UiHost& h = host_of(L);
    FoundCache& c = cache_of(L);
    const bool rescan = lua_toboolean(L, 1) != 0;
    lua_newtable(L);
    lua_newtable(L); // info
    if (!h.scripts) return 2;
    auto& rows = c.rows;
    plugin::cxx_guard(L, [&] {
        auto now = std::chrono::steady_clock::now();
        if (!c.valid || rescan || now - c.at >= kRescan) {
            c.found = h.scripts->found();
            c.at = now;
            c.valid = true;
        }
        c.folders.clear();
        for (const auto& f : h.scripts->folders()) c.folders.push_back(f.string());
        rows.clear();
        rows.reserve(c.found.size());
        for (const auto& f : c.found) {
            Row& r = rows.emplace_back();
            r.name = f.name;
            r.found = true;
            r.allowed = f.caps != 0;
            r.why = f.why;
            r.server = f.server;
            if (f.caps) r.caps = plugin::caps_text(f.caps);
        }
        for (const auto& i : h.scripts->list()) {
            auto it = std::find_if(rows.begin(), rows.end(), [&](const Row& r) { return r.name == i.name; });
            if (it == rows.end()) { // running, though its file is gone or was renamed
                rows.emplace_back().name = i.name;
                it = rows.end() - 1;
            }
            it->status = status_word(i.status);
            it->error = i.error;
            it->kept = i.kept;
            if (i.caps) it->caps = plugin::caps_text(i.caps);
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.name < b.name; });
        // A plugin's code sees its own row alone (a script of its tag's name), and no folders: the others' errors,
        // permissions and where the player keeps them are not its business.
        if (plugin_code()) {
            const std::string_view tag = ui::lua::LuaUi::caller_tag();
            std::erase_if(rows, [&](const Row& r) { return r.name != tag; });
            c.folders.clear();
        }
    });
    lua_newtable(L);
    lua_Integer k = 0;
    for (const std::string& f : c.folders) {
        lua_pushlstring(L, f.data(), f.size());
        lua_rawseti(L, -2, ++k);
    }
    lua_setfield(L, -2, "folders");
    set_bool(L, "autorun", h.scripts->autorun_available()); // (set at a character's zone-in)
    lua_Integer n = 0;
    for (const Row& r : rows) {
        lua_createtable(L, 0, 10);
        set_string(L, "name", r.name);
        set_string(L, "status", r.status);
        set_string(L, "error", r.error);
        set_bool(L, "found", r.found);
        set_bool(L, "allowed", r.allowed);
        set_string(L, "why", r.why);
        set_bool(L, "server", r.server);
        set_bool(L, "kept", r.kept);
        set_bool(L, "autorun", h.scripts->autorun(r.name)); // (the push cannot raise)
        set_string(L, "caps", r.caps);
        lua_rawseti(L, -3, ++n);
    }
    return 2;
}

// ok, why = hs.ScriptRun(name [, args]): the arguments split as /lua run's are ("quoted words" one).
int ScriptRun(lua_State* L) {
    UiHost& h = host_of(L);
    const char* name_arg = luaL_checkstring(L, 1);
    const char* a = luaL_optstring(L, 2, nullptr); // (both checked before anything C++ is made)
    if (plugin_code()) {
        lua_pushboolean(L, 0);
        lua_pushliteral(L, "scripts are the player's to run");
        return 2;
    }
    if (!h.scripts) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "scripts are off");
        return 2;
    }
    std::string& why = plugin::lua_keep<std::string>(L); // across the push (plugin/lua_keep.h)
    bool ok = false;
    plugin::cxx_guard(L, [&] {
        std::vector<std::string> args;
        if (a) {
            std::string_view rest = a;
            while (true) {
                while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
                if (rest.empty()) break;
                args.push_back(game::next_cmd_arg(rest));
            }
        }
        ok = h.scripts->run(name_arg, args, &why);
    });
    lua_pushboolean(L, ok);
    if (ok) return 1;
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}

int ScriptStop(lua_State* L) {
    UiHost& h = host_of(L);
    std::string name = luaL_checkstring(L, 1);
    lua_pushboolean(L, h.scripts && !plugin_code() && h.scripts->stop(name));
    return 1;
}

int ScriptPause(lua_State* L) {
    UiHost& h = host_of(L);
    std::string name = luaL_checkstring(L, 1);
    lua_pushboolean(L, h.scripts && !plugin_code() && h.scripts->pause(name));
    return 1;
}

int SetScriptAutorun(lua_State* L) {
    UiHost& h = host_of(L);
    std::string name = luaL_checkstring(L, 1);
    bool on = lua_toboolean(L, 2) != 0;
    lua_pushboolean(L, h.scripts && !plugin_code() && h.scripts->set_autorun(name, on));
    return 1;
}

} // namespace

void register_scripts_api(lua_State* L, UiHost& host) {
    // One cache for the functions, freed with the state.
    void* mem = lua_newuserdatauv(L, sizeof(FoundCache), 0);
    new (mem) FoundCache();
    lua_newtable(L);
    lua_pushcfunction(L, cache_gc);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    const int cache = lua_gettop(L);
    for (auto [name, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
             {"Scripts", Scripts},
             {"ScriptRun", ScriptRun},
             {"ScriptStop", ScriptStop},
             {"ScriptPause", ScriptPause},
             {"SetScriptAutorun", SetScriptAutorun}}) {
        lua_pushlightuserdata(L, &host);
        lua_pushvalue(L, cache);
        lua_pushcclosure(L, fn, 2);
        lua_setfield(L, cache - 1, name); // the hs table, under the cache
    }
    lua_pop(L, 1);
}

} // namespace hailstone::app
