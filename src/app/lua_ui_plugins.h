#pragma once

// The plugins' part of the `hs` table (plugin/ui_bridge.h; docs/plugins.md "Plugin windows"):
//   p = hs.Plugin(name)          a loaded plugin that gives the UI something, else nil; p.fn(...) or
//                                p.call("fn", ...) calls its function fn (nil when it has none)
//   hs.PluginOptions()           {plugin, owner, name, label, min, max, value} for each plugin option (owner: its
//                                tag, plugin::UiBridge::owner_tag)
//   hs.SetPluginOption(plugin, name, value)   plugin: a name or an owner tag
//   hs.PluginMenu()              {plugin, owner, label, command} for each menu entry the caller may reach (a
//                                plugin's code: its own); hs.PluginMenuClick(i) runs that list's entry i as its
//                                owner would (run_plugin_menu)
//   q = hs.PluginConsent()       the server's compiled plugins waiting for the player's word, else nil:
//                                {server, serial, warning, plugins = {{name, file, size, sha256, short}}}
//   ok = hs.AnswerPluginConsent(allow, serial)   true allow, false refuse (both kept for those bytes), nil put away
//                                until the next zone-in; serial the question's. False when there is no question or
//                                it is not that one any more. A plugin's code gets nil / false.
//   q = hs.UI.addons.Policy()    active server requirements and recovery state for a built-in interface; nil otherwise.
//   ok = hs.UI.addons.PolicyAction(action, serial)   built-in interface only: "stop", "retry" or "camp" against
//                                the current snapshot; stale or unavailable actions return false.

// The Lua scripts' part (mq/lua_mq.h; the Scripts window, docs/plugins.md "The Scripts window"; app/lua_ui_scripts.cpp):
//   list, info = hs.Scripts([rescan])   {name, status ("" | "running" | "paused" | "exited" | "error"), error,
//                                       found, allowed, why, server, kept, autorun, caps} for each script on disk
//                                       or running, by name; info = {folders = {...}, autorun = a character's list
//                                       is there}. The folders are read again at most once a second (or `rescan`).
//   ok, why = hs.ScriptRun(name [, args])   as /lua run name args
//   hs.ScriptStop(name), hs.ScriptPause(name)   true when there was such a script
//   ok = hs.SetScriptAutorun(name, on)     the character's autorun list (false: no character yet)

#include <cstddef>

struct lua_State;

namespace hailstone::app {

struct UiHost;
void register_plugins_api(lua_State* L, UiHost& host);
// Runs the plugins' menu entry i (0-based; UiBridge::menu) with its owner's capabilities (UiHost::plugin_caps under a
// plugin::CallerScope), not the player's: whoever chose it, the command is the plugin's. False: no such entry.
bool run_plugin_menu(UiHost& host, std::size_t i);
void register_scripts_api(lua_State* L, UiHost& host);

} // namespace hailstone::app
