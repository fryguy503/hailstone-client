#include "app/lua_ui_plugins.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lua.hpp>

#include "app/lua_ui.h"
#include "core/ascii_case.h"
#include "core/sha256.h"
#include "plugin/lua_keep.h"
#include "plugin/lua_value.h"
#include "plugin/policy.h"
#include "plugin/ui_bridge.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

namespace {

using plugin::UiValue;
using plugin::push_value;
using plugin::to_value;

UiHost& host_of(lua_State* L) { return *static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1))); }

// Whose code runs: "" the pack's (the player's), else a plugin folder's owner tag (LuaUi::caller_tag).
std::string_view caller() { return ui::lua::LuaUi::caller_tag(); }

// The owner a name means to the caller: a plugin's code names itself by its bare name (a script's windows call
// hs.Plugin("<script>") even beside a compiled plugin of that name); otherwise the bridge's resolve. "" none.
std::string target_owner(const UiHost& h, std::string_view name) {
    if (!h.plugin_ui) return {};
    const std::string_view me = caller();
    if (!me.empty())
        if (auto t = plugin::UiBridge::parse_tag(me); t && iequals(t->second, name) && !h.plugin_ui->resolve(me).empty())
            return std::string(me);
    return h.plugin_ui->resolve(name);
}

// A plugin's code may reach only its own functions, options and menu entries; the pack (the player) reaches all.
// A plugin that drives another's exported functions would act with the other's capabilities (docs/plugins.md
// "Plugin windows").
bool may_reach(std::string_view owner) { return caller().empty() || iequals(caller(), owner); }

// A call's arguments and result, in a userdata on the stack that the state owns (its __gc destroys them): Lua is
// built as C, and a push that fails (the UI state's memory limit) longjmps past this frame, whose own vectors and
// values would never be destroyed. Made before any of them, so that it is the only place they live.
struct CallBox {
    std::vector<UiValue> args;
    UiValue result;
};
constexpr const char* kCallBoxMeta = "hailstone.hs.plugin_call";

int call_box_gc(lua_State* L) {
    static_cast<CallBox*>(lua_touserdata(L, 1))->~CallBox();
    return 0;
}

CallBox& push_call_box(lua_State* L) {
    if (luaL_newmetatable(L, kCallBoxMeta)) {
        lua_pushcfunction(L, call_box_gc);
        lua_setfield(L, -2, "__gc");
    }
    void* mem = lua_newuserdatauv(L, sizeof(CallBox), 0);
    auto* box = new (mem) CallBox(); // (nothing after this raises: the metatable is set, the box is the state's)
    lua_rotate(L, -2, 1);
    lua_setmetatable(L, -2);
    return *box;
}

// The call itself, its arguments the stack's from `first` to `last`: the result on top (nil without one).
int call_plugin(lua_State* L, const UiHost& h, const char* owner, std::string_view name, int first, int last) {
    CallBox& box = push_call_box(L);
    for (int i = first; i <= last; ++i) box.args.push_back(to_value(L, i)); // (to_value never raises)
    bool found = false;
    if (h.plugin_ui) {
        auto r = h.plugin_ui->call(owner, name, box.args);
        if (r) {
            box.result = std::move(*r);
            found = true;
        }
    }
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    push_value(L, box.result);
    return 1;
}

// p.fn(...): upvalue 1 the host, 2 the owner's tag, 3 the function's name; the arguments are the call's.
int plugin_fn(lua_State* L) {
    UiHost& h = host_of(L);
    const char* owner = lua_tostring(L, lua_upvalueindex(2));
    const char* name = lua_tostring(L, lua_upvalueindex(3));
    if (!owner || !may_reach(owner)) {
        lua_pushnil(L);
        return 1;
    }
    if (static_cast<std::size_t>(lua_gettop(L)) > plugin::UiBridge::kMaxCallArgs) { // (the bridge refuses them too)
        lua_pushnil(L);
        return 1;
    }
    for (int i = 1; i <= lua_gettop(L); ++i) ui::lua::unview(L, i); // (to_value reads tables raw)
    return call_plugin(L, h, owner, name ? name : "", 1, lua_gettop(L));
}

// p.call("fn", ...)
int plugin_call(lua_State* L) {
    UiHost& h = host_of(L);
    const char* owner = lua_tostring(L, lua_upvalueindex(2));
    const char* name = luaL_checkstring(L, 1);
    if (!owner || !may_reach(owner)) {
        lua_pushnil(L);
        return 1;
    }
    if (static_cast<std::size_t>(lua_gettop(L) - 1) > plugin::UiBridge::kMaxCallArgs) {
        lua_pushnil(L);
        return 1;
    }
    for (int i = 2; i <= lua_gettop(L); ++i) ui::lua::unview(L, i); // (to_value reads tables raw)
    return call_plugin(L, h, owner, name, 2, lua_gettop(L));
}

// The plugin object's __index: p.call, p.name, p.owner, and p.<fn> as a function calling it.
int plugin_index(lua_State* L) {
    // upvalue 1 the host, 2 the owner's tag; arguments: the object, the key.
    const char* key = luaL_checkstring(L, 2);
    const std::string_view k = key;
    if (k == "name" || k == "owner") {
        const char* owner = lua_tostring(L, lua_upvalueindex(2));
        if (k == "owner" || !owner) {
            lua_pushstring(L, owner);
            return 1;
        }
        // The tag's name, in a box the state owns across the push (plugin/lua_keep.h).
        auto& t = plugin::lua_keep<std::optional<std::pair<plugin::UiBridge::Kind, std::string>>>(L);
        plugin::cxx_guard(L, [&] { t = plugin::UiBridge::parse_tag(owner); });
        lua_pushstring(L, t ? t->second.c_str() : owner);
        return 1;
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, lua_upvalueindex(2));
    if (k == "call") {
        lua_pushcclosure(L, plugin_call, 2);
        return 1;
    }
    lua_pushstring(L, key);
    lua_pushcclosure(L, plugin_fn, 3);
    return 1;
}

// p = hs.Plugin(name)   -- name: a bare name or an owner tag; nil for none, and from a plugin's code for another's
int Plugin(lua_State* L) {
    UiHost& h = host_of(L);
    const char* name = luaL_checkstring(L, 1);
    std::string& owner = plugin::lua_keep<std::string>(L); // (plugin/lua_keep.h)
    plugin::cxx_guard(L, [&] { owner = target_owner(h, name); });
    if (owner.empty() || !may_reach(owner)) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);           // the object
    lua_createtable(L, 0, 1);  // its metatable
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushstring(L, owner.c_str());
    lua_pushcclosure(L, plugin_index, 2);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    return 1;
}

// hs.PluginOptions(): {plugin, owner (its tag: plugin::UiBridge::owner_tag), name, label, min, max, value} each
int PluginOptions(lua_State* L) {
    UiHost& h = host_of(L);
    if (!h.plugin_ui) {
        lua_newtable(L);
        return 1;
    }
    // A copy: each get runs plugin code, which may add or replace options (hailstone.option{...}) and so move the
    // list, or the very function running, under a loop over the bridge's own. It and each owner tag live in boxes
    // the state owns across the pushes (plugin/lua_keep.h).
    auto& options = plugin::lua_keep<std::vector<plugin::UiBridge::Option>>(L);
    std::string& owner = plugin::lua_keep<std::string>(L);
    plugin::cxx_guard(L, [&] {
        options = h.plugin_ui->options();
        std::erase_if(options, [](const plugin::UiBridge::Option& o) { return !may_reach(o.owner()); }); // its own
    });
    lua_newtable(L);
    lua_Integer i = 0;
    for (const auto& o : options) {
        lua_Integer value = 0;
        plugin::cxx_guard(L, [&] {
            owner = o.owner();
            value = o.get ? o.get() : 0;
        });
        lua_createtable(L, 0, 7);
        lua_pushstring(L, o.plugin.c_str());
        lua_setfield(L, -2, "plugin");
        lua_pushstring(L, owner.c_str());
        lua_setfield(L, -2, "owner");
        lua_pushstring(L, o.name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, o.label.c_str());
        lua_setfield(L, -2, "label");
        lua_pushinteger(L, o.min);
        lua_setfield(L, -2, "min");
        lua_pushinteger(L, o.max);
        lua_setfield(L, -2, "max");
        lua_pushinteger(L, value);
        lua_setfield(L, -2, "value");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// hs.SetPluginOption(plugin, name, value)   -- plugin: a name or an owner tag (an option's `owner`)
int SetPluginOption(lua_State* L) {
    UiHost& h = host_of(L);
    const char* plugin = luaL_checkstring(L, 1);
    const char* name = luaL_checkstring(L, 2);
    lua_Integer v = lua_isboolean(L, 3) ? (lua_toboolean(L, 3) ? 1 : 0) : luaL_checkinteger(L, 3);
    if (!h.plugin_ui) return 0;
    const std::string owner = target_owner(h, plugin);
    if (owner.empty() || !may_reach(owner)) return 0;
    if (auto* o = h.plugin_ui->find_option(owner, name); o && o->set) {
        lua_Integer clamped = v < o->min ? o->min : v > o->max ? o->max : v;
        auto set = o->set; // the call may change the list
        set(static_cast<int>(clamped));
    }
    return 0;
}

// The menu entries the caller may reach (a plugin's code: its own; the pack: all), as their places in the bridge's menu.
std::vector<std::size_t> reachable_menu(const plugin::UiBridge& ui) {
    std::vector<std::size_t> out;
    const auto& menu = ui.menu();
    for (std::size_t i = 0; i < menu.size(); ++i)
        if (may_reach(menu[i].owner())) out.push_back(i);
    return out;
}

// hs.PluginMenu(): {plugin, owner, label, command} each; a plugin's code sees its own entries alone, numbered as
// hs.PluginMenuClick takes them.
int PluginMenu(lua_State* L) {
    UiHost& h = host_of(L);
    std::string& owner = plugin::lua_keep<std::string>(L); // each entry's tag, across its push (plugin/lua_keep.h)
    auto& shown = plugin::lua_keep<std::vector<std::size_t>>(L);
    lua_newtable(L);
    if (!h.plugin_ui) return 1;
    plugin::cxx_guard(L, [&] { shown = reachable_menu(*h.plugin_ui); });
    lua_Integer i = 0;
    for (std::size_t at : shown) {
        const plugin::UiBridge::MenuEntry& e = h.plugin_ui->menu()[at];
        plugin::cxx_guard(L, [&] { owner = e.owner(); });
        lua_createtable(L, 0, 4);
        lua_pushstring(L, e.plugin.c_str());
        lua_setfield(L, -2, "plugin");
        lua_pushstring(L, owner.c_str());
        lua_setfield(L, -2, "owner");
        lua_pushstring(L, e.label.c_str());
        lua_setfield(L, -2, "label");
        lua_pushstring(L, e.command.c_str());
        lua_setfield(L, -2, "command");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// hs.PluginMenuClick(i): runs entry i of hs.PluginMenu() (the caller's own list) as its owner (run_plugin_menu).
int PluginMenuClick(lua_State* L) {
    UiHost& h = host_of(L);
    lua_Integer i = luaL_checkinteger(L, 1);
    if (i < 1 || !h.plugin_ui) return 0;
    std::optional<std::size_t> at;
    plugin::cxx_guard(L, [&] {
        const auto shown = reachable_menu(*h.plugin_ui);
        if (static_cast<std::uint64_t>(i) <= shown.size()) at = shown[static_cast<std::size_t>(i - 1)];
    });
    if (at) run_plugin_menu(h, *at);
    return 0;
}

// The question about the server's compiled plugins is the player's alone: a plugin's code in the UI
// (LuaUi::caller_tag: a server's own plugin windows among them) neither sees it nor answers it.
bool plugin_code() { return !caller().empty(); }

// q = hs.PluginConsent()
int PluginConsent(lua_State* L) {
    UiHost& h = host_of(L);
    // The question and each file's texts live in a box the state owns across the pushes (plugin/lua_keep.h).
    struct Consent {
        std::optional<plugin::ConsentPrompt> q;
        plugin::ConsentWording words;
        std::string hex, file;
    };
    Consent& c = plugin::lua_keep<Consent>(L);
    plugin::cxx_guard(L, [&] {
        c.q = h.plugin_consent && !plugin_code() && h.trusted_ui ? h.plugin_consent() : std::nullopt;
        if (c.q) c.words = plugin::consent_wording(*c.q, c.q->server);
    });
    const auto& q = c.q;
    if (!q) {
        lua_pushnil(L);
        return 1;
    }
    if (ui::lua::pack_code(L)) ui::lua::guard_drawing_window(); // the window asking shows it: guarded (lua_host.h)
    lua_createtable(L, 0, 7);
    lua_pushstring(L, q->server.c_str());
    lua_setfield(L, -2, "server");
    lua_pushinteger(L, static_cast<lua_Integer>(q->serial));
    lua_setfield(L, -2, "serial");
    // Worded for what is asked about: the server's plugins, or the player's own add-ons it requires.
    lua_pushlstring(L, c.words.title.data(), c.words.title.size());
    lua_setfield(L, -2, "title");
    lua_pushlstring(L, c.words.intro.data(), c.words.intro.size());
    lua_setfield(L, -2, "intro");
    lua_pushlstring(L, c.words.warning.data(), c.words.warning.size());
    lua_setfield(L, -2, "warning");
    lua_createtable(L, static_cast<int>(q->files.size()), 0);
    lua_Integer i = 0;
    for (const auto& f : q->files) {
        plugin::cxx_guard(L, [&] {
            c.hex = to_hex(f.sha256);
            const auto u8 = f.file.filename().u8string();
            c.file.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
        });
        lua_createtable(L, 0, 6);
        lua_pushstring(L, f.name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushlstring(L, c.file.data(), c.file.size());
        lua_setfield(L, -2, "file");
        lua_pushinteger(L, static_cast<lua_Integer>(f.size));
        lua_setfield(L, -2, "size");
        lua_pushlstring(L, c.hex.data(), c.hex.size());
        lua_setfield(L, -2, "sha256");
        lua_pushlstring(L, c.hex.data(), std::min<std::size_t>(c.hex.size(), 16));
        lua_setfield(L, -2, "short");
        lua_pushboolean(L, f.required_addon ? 1 : 0);
        lua_setfield(L, -2, "required");
        lua_rawseti(L, -2, ++i);
    }
    lua_setfield(L, -2, "plugins");
    return 1;
}

// ok = hs.AnswerPluginConsent(allow, serial): allow true, false, or nil (put away until the next zone-in); serial the
// question's (hs.PluginConsent().serial). An answer to a question that is no longer the one asked (another server's,
// or the same server's with other bytes, come up between the draw and the click) is refused: false, nothing kept.
int AnswerPluginConsent(lua_State* L) {
    UiHost& h = host_of(L);
    std::optional<bool> allow;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TBOOLEAN);
        allow = lua_toboolean(L, 1) != 0;
    }
    const lua_Integer serial = luaL_checkinteger(L, 2);
    bool asked = false;
    plugin::cxx_guard(L, [&] {
        // Only the client's own interfaces answer it (UiHost::trusted_ui); under another one the client draws it.
        if (plugin_code() || !h.trusted_ui || !h.answer_plugin_consent || !h.plugin_consent) return;
        const auto q = h.plugin_consent();
        asked = q && static_cast<lua_Integer>(q->serial) == serial;
    });
    if (!asked) {
        lua_pushboolean(L, 0);
        return 1;
    }
    h.answer_plugin_consent(allow);
    lua_pushboolean(L, 1);
    return 1;
}

// The table hs.AddonPolicy last built, by the view's serial: a pack asks every frame, and it is built again only when
// the failure changes.
const char kAddonPolicyTable = 0;

// q = hs.AddonPolicy(): {serial, message, can_retry} while a required add-on has failed in the world, else nil. The
// same table each frame until the failure changes (read it, do not change it). nil to plugin code and to an interface
// Hailstone does not ship (UiHost::trusted_ui: the client draws the warning under one).
int AddonPolicy(lua_State* L) {
    const UiHost& h = host_of(L);
    const ui::AddonPolicyView* view = h.addon_policy;
    if (plugin_code() || !h.trusted_ui || !view || !view->failed) {
        lua_pushnil(L);
        return 1;
    }
    const auto serial = static_cast<lua_Integer>(view->serial);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kAddonPolicyTable);
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "serial");
        const bool current = lua_isinteger(L, -1) && lua_tointeger(L, -1) == serial;
        lua_pop(L, 1);
        if (current) return 1;
    }
    lua_pop(L, 1);
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, serial);
    lua_setfield(L, -2, "serial");
    lua_pushlstring(L, view->message.data(), view->message.size());
    lua_setfield(L, -2, "message");
    lua_pushboolean(L, view->can_retry);
    lua_setfield(L, -2, "can_retry");
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kAddonPolicyTable);
    return 1;
}

// ok = hs.AddonPolicyAction("retry", serial): the player's Retry for the failure with that serial (hs.AddonPolicy):
// the required add-ons that failed are tried again once. False when there is no such failure any more, it cannot be
// retried, or the caller is plugin code, not the interface's own file, or an interface Hailstone does not ship.
int AddonPolicyAction(lua_State* L) {
    UiHost& h = host_of(L);
    const std::string_view action = luaL_checkstring(L, 1);
    const lua_Integer serial = luaL_checkinteger(L, 2);
    const ui::AddonPolicyView* view = h.addon_policy;
    const bool ok = !plugin_code() && h.trusted_ui && ui::lua::pack_code(L) && view && view->failed &&
                    view->can_retry && action == "retry" && static_cast<lua_Integer>(view->serial) == serial &&
                    h.retry_required_addons;
    if (ok) plugin::cxx_guard(L, [&] { h.retry_required_addons(); });
    lua_pushboolean(L, ok);
    return 1;
}

} // namespace

bool run_plugin_menu(UiHost& h, std::size_t i) {
    if (!h.plugin_ui || i >= h.plugin_ui->menu().size() || !h.submit_chat) return false;
    const plugin::UiBridge::MenuEntry& e = h.plugin_ui->menu()[i];
    const std::string command = e.command, owner = e.owner();
    plugin::CallerScope scope(h.plugin_caps ? h.plugin_caps(owner) : 0u, owner);
    h.submit_chat(command);
    return true;
}

void register_plugins_api(lua_State* L, UiHost& host) {
    for (auto [name, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
             {"Plugin", Plugin},
             {"PluginOptions", PluginOptions},
             {"SetPluginOption", SetPluginOption},
             {"PluginMenu", PluginMenu},
             {"PluginMenuClick", PluginMenuClick},
             {"PluginConsent", PluginConsent},
             {"AnswerPluginConsent", AnswerPluginConsent},
             {"AddonPolicy", AddonPolicy},
             {"AddonPolicyAction", AddonPolicyAction}}) {
        lua_pushlightuserdata(L, &host);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    }
}

} // namespace hailstone::app
