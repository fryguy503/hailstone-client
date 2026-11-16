#pragma once

// The plugins' part of the `eq` table (plugin/ui_bridge.h; docs/plugins.md "Plugin windows"):
//   p = eq.Plugin(name)          a loaded plugin that gives the UI something, else nil; p.fn(...) or
//                                p.call("fn", ...) calls its function fn (nil when it has none)
//   eq.PluginOptions()           {plugin, owner, name, label, min, max, value} for each plugin option (owner: its
//                                tag, plugin::UiBridge::owner_tag)
//   eq.SetPluginOption(plugin, name, value)   plugin: a name or an owner tag
//   eq.PluginMenu()              {plugin, owner, label, command} for each menu entry the caller may reach (a
//                                plugin's code: its own); eq.PluginMenuClick(i) runs that list's entry i as its
//                                owner would (run_plugin_menu)
//   q = eq.PluginConsent()       the server's compiled plugins waiting for the player's word, else nil:
//                                {server, serial, warning, plugins = {{name, file, size, sha256, short}}}
//   ok = eq.AnswerPluginConsent(allow, serial)   true allow, false refuse (both kept for those bytes), nil put away
//                                until the next zone-in; serial the question's. False when there is no question or
//                                it is not that one any more. A plugin's code gets nil / false.
//   q = eq.AddonPolicy()         active server requirements and recovery state, else nil; a plugin gets nil.
//   ok = eq.AddonPolicyAction(action, serial)   player's pack only: "stop", "retry" or "camp" against the
//                                current snapshot; stale or unavailable actions return false.

// The Lua scripts' part (mq/lua_mq.h; the Scripts window, docs/plugins.md "The Scripts window"; app/lua_eq_scripts.cpp):
//   list, info = eq.Scripts([rescan])   {name, status ("" | "running" | "paused" | "exited" | "error"), error,
//                                       found, allowed, why, server, kept, autorun, caps} for each script on disk
//                                       or running, by name; info = {folders = {...}, autorun = a character's list
//                                       is there}. The folders are read again at most once a second (or `rescan`).
//   ok, why = eq.ScriptRun(name [, args])   as /lua run name args
//   eq.ScriptStop(name), eq.ScriptPause(name)   true when there was such a script
//   ok = eq.SetScriptAutorun(name, on)     the character's autorun list (false: no character yet)

#include <cstddef>

struct lua_State;

namespace hailstone::app {

struct EqHost;
void register_plugins_api(lua_State* L, EqHost& host);
// Runs the plugins' menu entry i (0-based; UiBridge::menu) with its owner's capabilities (EqHost::plugin_caps under a
// plugin::CallerScope), not the player's: whoever chose it, the command is the plugin's. False: no such entry.
bool run_plugin_menu(EqHost& host, std::size_t i);
void register_scripts_api(lua_State* L, EqHost& host);

} // namespace hailstone::app
