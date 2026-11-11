// The client's own pre-game screens for the UI packs: the login screen (ui::LoginView), the server select
// (ui::ServerSelectView), the character select (ui::CharacterSelectView) and the character creation
// (ui::CharacterCreateView). A Hailstone extension: these screens are
// the client's, not the original's (eqmain.dll's login and server select pages and CCharacterListWnd are not
// followed), so the packs draw them in their own look over the same views and actions the ImGui screens of
// ui/dev_screens.cpp use. A pack's window that reads a view each frame stands in for the client's screen
// (EqApi::replaces); its buttons come back to the client as the screen's actions. Lists are 1-based here, 0 none.
// Only the pack's own windows (and the player's scripts in it) see these screens: a plugin's window
// (LuaUi::caller_tag) gets nil and its actions do nothing, as the login view holds the typed or saved password.
// One action is taken per frame: a second in the same frame replaces the first.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lua.hpp>

#include "app/lua_eq_windows.h"
#include "core/distribution.h"
#include "core/server_profile.h"
#include "game/eq_names.h"
#include "game/zone_names.h"
#include "net/game_connection.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

namespace {

EqHost& host_of(lua_State* L) { return *static_cast<EqHost*>(lua_touserdata(L, lua_upvalueindex(1))); }

// The pre-game screens are the pack's; a plugin's window has no part in them.
bool plugin_window() { return !ui::lua::LuaUi::caller_tag().empty(); }

int ManagedDistribution(lua_State* L) {
    lua_pushboolean(L, distribution::managed());
    return 1;
}

// The window drawing this screen keeps it from the client.
void mark(lua_State* L, std::string_view screen) {
    auto* s = static_cast<ScreenReplacements*>(lua_touserdata(L, lua_upvalueindex(2)));
    if (s && s->at && s->frame) (*s->at)[std::string(screen)] = *s->frame;
}

void set_string(lua_State* L, const char* key, const std::string& value) {
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, key);
}
void set_int(lua_State* L, const char* key, lua_Integer value) {
    lua_pushinteger(L, value);
    lua_setfield(L, -2, key);
}
void set_bool(lua_State* L, const char* key, bool value) {
    lua_pushboolean(L, value);
    lua_setfield(L, -2, key);
}
void push_status(lua_State* L, const ui::StatusLine& status) {
    lua_newtable(L);
    set_string(L, "text", status.text);
    set_bool(L, "error", status.is_error);
    lua_setfield(L, -2, "status");
}

// The login, server select and character select views are written through one of these. LuaOut builds the Lua
// table; Fingerprint hashes the same calls (every key, value and table edge), so a view's revision (push_view)
// changes whenever the table it would build changes, with no list of fields to keep in step.
struct LuaOut {
    lua_State* L;
    void table() { lua_newtable(L); }
    void close(const char* key) { lua_setfield(L, -2, key); }                          // the table as parent[key]
    void close(std::size_t i) { lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1)); } // as parent[i + 1]
    void str(const char* key, std::string_view v) {
        lua_pushlstring(L, v.data(), v.size());
        lua_setfield(L, -2, key);
    }
    void integer(const char* key, lua_Integer v) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, key);
    }
    void boolean(const char* key, bool v) {
        lua_pushboolean(L, v);
        lua_setfield(L, -2, key);
    }
    void number(const char* key, double v) {
        lua_pushnumber(L, static_cast<lua_Number>(v));
        lua_setfield(L, -2, key);
    }
    void item(std::size_t i, std::string_view v) {
        lua_pushlstring(L, v.data(), v.size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    void item(std::size_t i, lua_Integer v) {
        lua_pushinteger(L, v);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
};

// FNV-1a, 64 bits; only ever compared within one run.
struct Fingerprint {
    std::uint64_t h = 14695981039346656037ull;
    void byte(unsigned char b) { h = (h ^ b) * 1099511628211ull; }
    void u64(std::uint64_t v) {
        for (int k = 0; k < 8; ++k) byte(static_cast<unsigned char>(v >> (8 * k)));
    }
    void text(std::string_view s) {
        u64(s.size());
        for (char c : s) byte(static_cast<unsigned char>(c));
    }
    void key(char tag, const char* k) {
        byte(static_cast<unsigned char>(tag));
        text(k);
    }
    void table() { byte('{'); }
    void close(const char* k) { key('}', k); }
    void close(std::size_t i) {
        byte(']');
        u64(i);
    }
    void str(const char* k, std::string_view v) {
        key('s', k);
        text(v);
    }
    void integer(const char* k, lua_Integer v) {
        key('i', k);
        u64(static_cast<std::uint64_t>(v));
    }
    void boolean(const char* k, bool v) { key(v ? 'T' : 'F', k); }
    void number(const char* k, double v) {
        key('n', k);
        u64(std::bit_cast<std::uint64_t>(v));
    }
    void item(std::size_t i, std::string_view v) {
        byte('S');
        u64(i);
        text(v);
    }
    void item(std::size_t i, lua_Integer v) {
        byte('I');
        u64(i);
        u64(static_cast<std::uint64_t>(v));
    }
};

template <class Out>
void emit_status(Out& o, const ui::StatusLine& status) {
    o.table();
    o.str("text", status.text);
    o.boolean("error", status.is_error);
    o.close("status");
}

// A view's table, with its revision (view.revision). Given the revision of the table the script holds (argument
// arg) and nothing changed since, true instead: the script keeps its table, and nothing is built.
template <class Emit>
int push_view(lua_State* L, int arg, const Emit& emit) {
    Fingerprint f;
    emit(f);
    const auto revision = static_cast<lua_Integer>(f.h);
    int known = 0;
    const lua_Integer held = lua_tointegerx(L, arg, &known);
    if (known && held == revision) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_newtable(L);
    LuaOut out{L};
    emit(out);
    set_int(L, "revision", revision);
    return 1;
}

// A 1-based Lua index as a 0-based one; -1 when out of [1, n].
int index_arg(lua_State* L, int arg, std::size_t n) {
    lua_Integer i = luaL_optinteger(L, arg, 0);
    return i >= 1 && i <= static_cast<lua_Integer>(n) ? static_cast<int>(i - 1) : -1;
}

// ---------------------------------------------------------------- login

bool login_ready(const ui::LoginView& v) {
    return !v.busy && !v.username.empty() && !v.password.empty() && !v.host.empty();
}

template <class Out>
void emit_strings(Out& o, const char* key, const std::vector<std::string>& list) {
    o.table();
    for (std::size_t i = 0; i < list.size(); ++i) o.item(i, list[i]);
    o.close(key);
}

// view.addons (docs/addons.md): the add-on manager's part of the login screen.
template <class Out>
void emit_addons(Out& o, const ui::AddonsView& a) {
    o.table();
    o.boolean("open", a.open);
    o.boolean("available", a.available);
    o.str("whyOff", a.why_off);
    o.str("link", a.link);
    o.boolean("inspecting", a.inspecting);
    emit_status(o, a.status);
    o.str("server", a.server);
    o.table();
    for (std::size_t i = 0; i < a.suggestions.size(); ++i) {
        const ui::AddonSuggestionView& sg = a.suggestions[i];
        o.table();
        o.str("name", sg.name);
        o.str("source", sg.source);
        o.str("text", sg.text);
        o.boolean("fromServer", sg.from_server);
        o.close(i);
    }
    o.close("suggestions");
    if (a.offer) {
        const ui::AddonOfferView& of = *a.offer;
        o.table();
        o.str("name", of.name);
        o.str("kind", of.kind);
        o.str("version", of.version);
        o.str("author", of.author);
        o.str("description", of.description);
        o.str("source", of.source);
        o.str("page", of.page);
        o.str("notes", of.notes);
        o.str("key", of.key);
        o.str("installedVersion", of.installed_version);
        o.boolean("older", of.older);
        o.str("blocked", of.blocked);
        emit_strings(o, "caps", of.caps);
        o.close("offer");
    }
    o.table();
    for (std::size_t i = 0; i < a.rows.size(); ++i) {
        const ui::AddonRowView& r = a.rows[i];
        o.table();
        o.str("name", r.name);
        o.str("kind", r.kind);
        o.str("group", r.group);
        o.str("version", r.version);
        o.str("source", r.source);
        o.str("page", r.page);
        o.str("author", r.author);
        o.str("description", r.description);
        o.str("state", r.state);
        o.str("stateText", r.state_text);
        o.str("newVersion", r.new_version);
        emit_strings(o, "caps", r.caps);
        emit_strings(o, "newCaps", r.new_caps);
        emit_strings(o, "modified", r.modified);
        o.str("note", r.note);
        o.str("consent", r.consent);
        o.boolean("autoUpdate", r.auto_update);
        o.boolean("signed", r.signed_);
        o.boolean("canUpdate", r.can_update);
        o.boolean("canRemove", r.can_remove);
        o.boolean("canStart", r.can_start);
        o.str("folder", r.folder);
        o.close(i);
    }
    o.close("rows");
    // The store (docs/addons.md "Finding add-ons")
    o.boolean("browsing", a.browsing);
    o.boolean("browsed", a.browsed);
    o.str("storeError", a.store_error);
    o.table();
    for (std::size_t i = 0; i < a.store.size(); ++i) {
        const ui::AddonStoreItemView& it = a.store[i];
        o.table();
        o.str("name", it.name);
        o.str("source", it.source);
        o.str("page", it.page);
        o.str("kind", it.kind);
        o.str("version", it.version);
        o.str("author", it.author);
        o.str("description", it.description);
        o.str("worlds", it.worlds);
        o.str("installedVersion", it.installed_version);
        o.str("state", it.state);
        o.str("problem", it.problem);
        o.close(i);
    }
    o.close("store");
    o.close("addons");
}

const ui::AddonRowView* addon_row(const ui::AddonsView& a, std::string_view name) {
    const ui::AddonRowView* found = nullptr;
    for (const auto& r : a.rows)
        if (r.name == name && (!found || r.group == "installed")) found = &r;
    return found;
}

template <class Out>
void emit_saved_character(Out& o, const SavedCharacter& c) {
    o.table();
    o.str("name", c.name);
    o.str("world", c.world);
    o.integer("level", c.level);
    o.str("classes", c.classes);
}

template <class Out>
void emit_login(Out& o, const ui::LoginView& v) {
    o.boolean("managed", distribution::managed());
    o.boolean("busy", v.busy);
    emit_status(o, v.status);
    // The updater's news (docs/updater.md); nil when it has none. text is the sentence every screen shows.
    if (std::string text = ui::update_notice_text(v.update); !text.empty()) {
        const ui::UpdateNotice& u = v.update;
        o.table();
        o.str("text", text);
        o.str("state", u.state);
        o.str("version", u.version);
        o.str("current", u.current);
        o.str("notes", u.notes);
        o.str("error", u.error);
        o.boolean("canDownload", (u.state == "available" && u.has_package) || u.state == "failed");
        o.boolean("canRestart", u.state == "ready" && u.can_restart);
        o.boolean("canSkip", u.state == "available");
        o.boolean("hasNotes", !u.notes.empty() && !u.state.empty());
        o.number("progress", static_cast<double>(u.progress));
        o.close("update");
    }
    // This build and the updater's word on it, for the screen's corner (docs/updater.md "What the player sees"):
    // always there, unlike update.
    o.table();
    o.str("version", v.update.current);
    o.str("build", v.version);
    o.str("status", v.update.check);
    o.str("text", ui::update_status_text(v.update));
    o.str("page", v.project_page);
    emit_strings(o, "notes", v.release_notes);
    o.close("about");
    emit_addons(o, v.addons);
    o.boolean("transient", v.transient);
    o.str("version", v.version); // this build (LoginView::version), "" none
    o.boolean("ready", login_ready(v));
    o.table();
    for (std::size_t i = 0; i < v.profiles.size(); ++i) {
        const ServerProfile& p = v.profiles[i];
        o.table();
        o.str("id", p.id);
        o.str("name", p.name);
        o.str("host", p.host);
        o.integer("port", p.port);
        o.str("ui", p.ui.empty() ? v.default_ui : p.ui);
        o.close(i);
    }
    o.close("servers");
    o.integer("selected", v.selected >= 0 && v.selected < static_cast<int>(v.profiles.size()) ? v.selected + 1 : 0);
    o.str("name", v.name);
    o.str("host", v.host);
    o.integer("port", v.port);
    o.str("username", v.username);
    o.str("password", v.password);
    o.boolean("remember", v.remember);
    const bool keyring = ui::can_save_passwords(v);
    o.boolean("savePassword", v.save_password && v.remember && keyring);
    o.boolean("canSavePasswords", keyring);
    o.boolean("music", v.pregame_music);
    o.str("ui", v.ui);
    o.str("defaultUi", v.default_ui);
    o.table();
    for (std::size_t i = 0; i < v.ui_packs.size(); ++i) {
        const ui::UiPack& p = v.ui_packs[i];
        o.table();
        o.str("id", p.id);
        o.str("name", p.name);
        o.str("description", p.description);
        o.str("input", p.input);
        o.close(i);
    }
    o.close("uiPacks");
    o.table();
    for (std::size_t i = 0; i < v.accounts.size(); ++i) {
        const SavedAccount& a = v.accounts[i];
        o.table();
        o.str("username", a.username);
        o.boolean("passwordSaved", a.save_password);
        o.boolean("canBox", a.save_password && keyring);
        o.boolean("box", a.box && a.save_password && keyring);
        o.str("world", a.world);
        o.str("character", a.character);
        o.table();
        for (std::size_t j = 0; j < a.characters.size(); ++j) {
            emit_saved_character(o, a.characters[j]);
            o.close(j);
        }
        o.close("characters");
        o.close(i);
    }
    o.close("accounts");
    emit_strings(o, "worlds", v.worlds);
    const auto quick = ui::quick_characters(v);
    o.table();
    for (std::size_t i = 0; i < quick.size(); ++i) {
        const ui::QuickCharacter& q = quick[i];
        emit_saved_character(o, q.character);
        o.str("account", q.username);
        o.boolean("passwordSaved", q.password_saved);
        o.boolean("last", q.last);
        o.close(i);
    }
    o.close("characters");
    o.integer("quick", v.quick_selected >= 0 && v.quick_selected < static_cast<int>(quick.size()) ? v.quick_selected + 1 : 0);
    o.table();
    for (std::size_t i = 0; i < v.game_folders.size(); ++i) {
        const ui::GameFolder& f = v.game_folders[i];
        o.table();
        o.str("path", f.path);
        o.str("label", ui::game_folder_label(f));
        o.boolean("live", f.live);
        o.boolean("usable", f.usable);
        o.close(i);
    }
    o.close("gameFolders");
    o.boolean("gameFolderFixed", v.game_folder_fixed);
    o.str("assetsDir", ui::selected_assets_dir(v).string());
    o.integer("boxCount", static_cast<lua_Integer>(ui::box_accounts(v).size()));
    o.boolean("boxReady", ui::box_ready(v));
}

// view = eq.LoginScreen([revision])   -- nil unless the client shows its login screen; reading it draws the screen
// in the client's place this frame. view.revision changes whenever anything in the view does: given the revision of
// the table the script holds, eq.LoginScreen gives true while nothing changed (keep that table; nothing is built),
// else a new view. (The same holds for eq.ServerSelect and eq.CharacterSelect.)
// view: revision, busy, status {text, error}, transient (a test login: no saved servers), ready (Login can go),
//   servers {name, host, port, id, ui}, selected (0: a new server), name, host, port, username, password, remember,
//   savePassword, canSavePasswords, music, ui, defaultUi, uiPacks {id, name, description, input},
//   accounts {username, passwordSaved, box, canBox, world, character, characters {name, world, level, classes}},
//   worlds, characters {name, world, level, classes, account, passwordSaved, last}, quick (0: none),
//   gameFolders {path, label, live, usable} (the one in use first), gameFolderFixed, assetsDir, boxCount, boxReady,
//   addons (docs/addons.md): open (the panel shown: eq.LoginSet "addonsOpen"), available (links can be installed;
//     else whyOff says why), link, inspecting, status,
//     server (the picked server's name), suggestions {name ("" a server's suggestion), source, text, fromServer} (the
//     picked server's suggested add-ons the player does not have: docs/addons.md "Suggested add-ons"), offer {name, kind, version, author, description, source, page, notes, key
//     (the signing key's start, "" unsigned), installedVersion, blocked (why not, "" it can), caps (in words)},
//     rows {name, kind ("script"/"plugin"/"tool"), group ("installed", "manual", "server"), version, source, page, author,
//     description, state, stateText, newVersion, caps, newCaps, modified, note, autoUpdate, signed, canUpdate,
//     canRemove, canStart (an installed tool: eq.LoginDo "addonStart"), folder, consent (the server's compiled plugins: "allowed", "denied", "unasked"; else "")}.
int LoginScreen(lua_State* L) {
    EqHost& h = host_of(L);
    if (!h.login || plugin_window()) {
        lua_pushnil(L);
        return 1;
    }
    mark(L, "LoginScreen");
    const ui::LoginView& v = *h.login;
    return push_view(L, 1, [&v](auto& o) { emit_login(o, v); });
}

// eq.LoginSet(field, value): a field as the player edits it. "name", "host", "username", "password", "ui" (a pack's
// id: the client switches to it at once, and a saved server keeps it at once), strings; "port", 1..65535; "remember", "savePassword", "music" (the
// pre-game music, kept in hailstone.ini at once), booleans; "quick", the Characters list's row (0: none); "addonLink",
// the add-on link field; "addonsOpen", the add-ons panel shown.
int LoginSet(lua_State* L) {
    EqHost& h = host_of(L);
    if (!h.login || h.login->busy || plugin_window()) return 0;
    ui::LoginView& v = *h.login;
    const std::string_view field = luaL_checkstring(L, 1);
    if (distribution::managed() && (field == "addonLink" || field == "addonsOpen")) return 0;
    auto text = [&] { return std::string(luaL_checkstring(L, 2)); };
    if (field == "name") v.name = text();
    else if (field == "host") {
        std::string s = text();
        std::erase_if(s, [](char c) { return c == ' ' || c == '\t'; }); // CharsNoBlank, as the client's field
        v.host = std::move(s);
    } else if (field == "port") v.port = static_cast<int>(std::clamp<lua_Integer>(luaL_checkinteger(L, 2), 1, 65535));
    else if (field == "username") v.username = text();
    else if (field == "password") v.password = text();
    else if (field == "remember") v.remember = lua_toboolean(L, 2) != 0;
    else if (field == "savePassword") {
        if (v.remember && ui::can_save_passwords(v)) v.save_password = lua_toboolean(L, 2) != 0;
    } else if (field == "music") {
        v.pregame_music = lua_toboolean(L, 2) != 0;
        h.login_action = static_cast<int>(ui::LoginAction::MusicChanged);
    } else if (field == "ui") {
        std::string id = text();
        if (std::none_of(v.ui_packs.begin(), v.ui_packs.end(), [&](const ui::UiPack& p) { return p.id == id; }))
            return 0;
        v.ui = id;
        // A saved server keeps it at once (its server.ini; only the interface: the other fields wait for Save or
        // Log in), so picking another server or quitting does not lose it, as the in-game Options' choice.
        const std::string profile = ui::selected_profile_id(v);
        if (!v.transient && !profile.empty()) {
            if (auto saved = load_profile(profile, v.servers_dir); saved && saved->ui != id) {
                saved->ui = id;
                if (save_profile(*saved, v.servers_dir))
                    v.profiles[static_cast<std::size_t>(v.selected)].ui = id;
            }
        }
    } else if (field == "addonLink") {
        v.addons.link = text().substr(0, 400);
    } else if (field == "addonsOpen") {
        v.addons.open = lua_toboolean(L, 2) != 0;
    } else if (field == "quick") {
        const std::size_t n = ui::quick_characters(v).size(); // (gone before the argument is checked)
        v.quick_selected = index_arg(L, 2, n);
    } else {
        return luaL_error(L, "eq.LoginSet: no field %s", lua_tostring(L, 1));
    }
    return 0;
}

SavedAccount* account_named(ui::LoginView& v, std::string_view username) {
    for (auto& a : v.accounts)
        if (a.username == username) return &a;
    return nullptr;
}

// ok = eq.LoginDo(action [, arg [, value]]): what the login screen's buttons do.
//   "login"            log in with the fields (only when ready)
//   "quit"             quit the client
//   "updateDownload"   download the release the view's update offers (or try a failed download again)
//   "updateRestart"    install the downloaded release and start it (update.canRestart)
//   "updateSkip"       do not offer this release again
//   "updateNotes"      open the release's page in the system browser
//   "openProject"      open Hailstone's page (about.page) in the system browser
//   "select", i        pick saved server i (0: a new server)
//   "new"              a new server with the default fields
//   "save"             save the picked server's fields
//   "delete"           delete the picked server (ask first: its folder goes to servers/.deleted/)
//   "account", name    put a saved account in the fields (its saved password too)
//   "play" [, i]       play the Characters list's row i (else the picked one): its account, world and character
//   "gameFolder", i    play with game folder i of gameFolders; "browse": the folder picker
//   "openAssets"       open the picked server's own files folder
//   "loginBox"         log in the box set (each other account in a window of its own)
//   "forget", name     forget a saved account and its saved password (ask first)
//   "box", name, on    tick an account in or out of the box set
//   "world", name, w   the world the account's box login plays ("" the last played)
//   "character", name, c  the character it enters, on its world ("" choose at character select)
//   "addonInspect"     fetch the add-on link's manifest (addons.offer, the install card)
//   "addonInstall" [, "older"]  install the offer (the player said yes to what it asks); an offer older than the
//                      installed version (addons.offer.older) needs "older", the player's yes to going back;
//                      "addonCancel" drops it
//   "addonUpdate", n   update add-on n now (approves what it asks for more; overwrites the player's changes)
//   "addonRemove", n   remove add-on n (ask first; only the installed ones: never the server's)
//   "addonAuto", n, on  auto-update for add-on n
//   "addonFolder", n   open row n's folder; "addonPage" [, n]: its page (none: the offer's)
//   "addonStart", n    start the installed tool n (its row's canStart)
//   "addonCheck"       look for newer versions now
//   "addonRevoke", n   the server's compiled plugin n no longer runs there (consent "allowed")
//   "addonAskAgain", n the next visit asks again about the server's compiled plugin n (consent "denied")
//   "addonSuggestLook", src   open the install card of the suggestion whose source is src (addons.suggestions)
//   "addonSuggestLater", src  put the suggestion away until the next start; "addonSuggestNever", src: for good
//                             on this server
//   "addonBrowse" [, again]   fetch the store's list (addons.store) once a run; again: once more
//   "addonStoreLook", src     open the install card of the store's add-on whose source is src
// false when the action cannot go now (busy, not ready, no such row).
int LoginDo(lua_State* L) {
    EqHost& h = host_of(L);
    if (!h.login || plugin_window()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    ui::LoginView& v = *h.login;
    const std::string_view what = luaL_checkstring(L, 1);
    if (distribution::managed() && what.starts_with("addon")) {
        lua_pushboolean(L, false);
        return 1;
    }
    using A = ui::LoginAction;
    std::optional<A> action;
    bool ok = true;
    const bool picked = v.selected >= 0 && v.selected < static_cast<int>(v.profiles.size());
    const ui::UpdateNotice& u = v.update;
    if (what == "quit") {
        action = A::Quit;
    } else if (what == "updateDownload") {
        ok = (u.state == "available" && u.has_package) || u.state == "failed";
        if (ok) action = A::UpdateDownload;
    } else if (what == "updateRestart") {
        ok = u.state == "ready" && u.can_restart;
        if (ok) action = A::UpdateRestart;
    } else if (what == "updateSkip") {
        ok = u.state == "available";
        if (ok) action = A::UpdateSkip;
    } else if (what == "updateNotes") {
        ok = !u.notes.empty() && !u.state.empty();
        if (ok) action = A::UpdateNotes;
    } else if (what == "openProject") {
        ok = !v.project_page.empty();
        if (ok) action = A::OpenProject;
    } else if (what.starts_with("addon")) {
        // (the action and its argument checked before `name` is made: a Lua error longjmps past destructors)
        static constexpr std::string_view kAddonActions[] = {"addonInspect", "addonInstall", "addonCancel",
                                                             "addonCheck",   "addonUpdate",  "addonRemove",
                                                             "addonAuto",    "addonFolder",  "addonPage",
                                                             "addonRevoke",  "addonAskAgain", "addonSuggestLook",
                                                             "addonSuggestLater", "addonSuggestNever",
                                                             "addonBrowse",  "addonStoreLook", "addonStart"};
        if (std::find(std::begin(kAddonActions), std::end(kAddonActions), what) == std::end(kAddonActions))
            return luaL_error(L, "eq.LoginDo: no action %s", lua_tostring(L, 1));
        if (what != "addonBrowse") luaL_optstring(L, 2, ""); // (addonBrowse's is a flag: any value)
        ui::AddonsView& a = v.addons;
        const std::string name = what == "addonInspect" || what == "addonInstall" || what == "addonCancel" ||
                                         what == "addonCheck" || what == "addonBrowse"
                                     ? std::string()
                                     : std::string(luaL_optstring(L, 2, ""));
        const ui::AddonRowView* row = addon_row(a, name);
        if (what == "addonInspect") {
            ok = a.available && !a.inspecting && !a.link.empty();
            if (ok) action = A::AddonInspect;
        } else if (what == "addonInstall") {
            // An older version than the installed one goes in only when the call says that is meant ("older").
            const bool older = std::string_view(luaL_optstring(L, 2, "")) == "older";
            ok = a.offer.has_value() && a.offer->blocked.empty() && (!a.offer->older || older);
            if (ok) {
                a.choice_on = older;
                action = A::AddonInstall;
            }
        } else if (what == "addonCancel") {
            ok = a.offer.has_value();
            if (ok) action = A::AddonCancel;
        } else if (what == "addonCheck") {
            ok = a.available;
            if (ok) action = A::AddonCheck;
        } else if (what == "addonUpdate") {
            ok = row && row->can_update;
            if (ok) action = A::AddonUpdate;
        } else if (what == "addonRemove") {
            ok = row && row->group == "installed" && row->can_remove && !v.busy;
            if (ok) action = A::AddonRemove;
        } else if (what == "addonAuto") {
            ok = row && row->group == "installed";
            if (ok) {
                a.choice_on = lua_toboolean(L, 3) != 0;
                action = A::AddonAuto;
            }
        } else if (what == "addonRevoke" || what == "addonAskAgain") {
            // The server's own compiled plugin of that name (an installed add-on may share it).
            const ui::AddonRowView* server = nullptr;
            for (const auto& r : a.rows)
                if (r.group == "server" && r.kind == "plugin" && r.name == name) server = &r;
            ok = server && (what == "addonRevoke" ? server->consent == "allowed" : server->consent == "denied");
            if (ok) action = what == "addonRevoke" ? A::AddonRevoke : A::AddonAskAgain;
        } else if (what.starts_with("addonSuggest")) {
            // A suggestion of the picked server's, by its source.
            ok = !v.busy && std::any_of(a.suggestions.begin(), a.suggestions.end(),
                                        [&](const ui::AddonSuggestionView& sg) { return sg.source == name; });
            if (ok)
                action = what == "addonSuggestLook"    ? A::AddonSuggestLook
                         : what == "addonSuggestLater" ? A::AddonSuggestLater
                                                       : A::AddonSuggestNever;
        } else if (what == "addonBrowse") {
            ok = a.available && !a.browsing;
            if (ok) {
                a.choice_on = lua_toboolean(L, 2) != 0;
                action = A::AddonBrowse;
            }
        } else if (what == "addonStoreLook") {
            ok = a.available && !a.inspecting &&
                 std::any_of(a.store.begin(), a.store.end(), [&](const ui::AddonStoreItemView& it) {
                     return it.source == name && (it.state == "install" || it.state == "update");
                 });
            if (ok) action = A::AddonStoreLook;
        } else if (what == "addonStart") {
            ok = row && row->can_start;
            if (ok) action = A::AddonStart;
        } else if (what == "addonFolder") {
            ok = row && !row->folder.empty();
            if (ok) action = A::AddonFolder;
        } else { // addonPage
            ok = name.empty() ? a.offer && !a.offer->page.empty() : row && !row->page.empty();
            if (ok) action = A::AddonPage;
        }
        if (ok) a.choice = name;
    } else if (v.busy) {
        ok = false;
    } else if (what == "login") {
        ok = login_ready(v);
        if (ok) action = A::Submit;
    } else if (what == "select") {
        if (!v.transient) ui::select_profile(v, index_arg(L, 2, v.profiles.size()));
        else ok = false;
    } else if (what == "new") {
        if (!v.transient) ui::new_profile(v);
        else ok = false;
    } else if (what == "save") {
        ok = !v.transient && !v.host.empty();
        if (ok) action = A::Save;
    } else if (what == "delete") {
        ok = !v.transient && picked;
        if (ok) action = A::Delete;
    } else if (what == "account") {
        ui::pick_account(v, luaL_checkstring(L, 2));
    } else if (what == "play") {
        if (!lua_isnoneornil(L, 2)) luaL_checkinteger(L, 2); // (before `quick` is made)
        const auto quick = ui::quick_characters(v);
        if (!lua_isnoneornil(L, 2)) v.quick_selected = index_arg(L, 2, quick.size());
        ok = v.quick_selected >= 0 && v.quick_selected < static_cast<int>(quick.size());
        if (ok) action = A::PlayCharacter;
    } else if (what == "gameFolder") {
        const int i = index_arg(L, 2, v.game_folders.size());
        ok = !v.game_folder_fixed && i > 0 && v.game_folders[static_cast<std::size_t>(i)].usable; // 1: in use already
        if (ok) {
            v.game_choice = v.game_folders[static_cast<std::size_t>(i)].path;
            action = A::UseGameFolder;
        }
    } else if (what == "browse") {
        ok = !v.game_folder_fixed;
        if (ok) action = A::BrowseGameFolder;
    } else if (what == "openAssets") {
        ok = picked;
        if (ok) action = A::OpenAssets;
    } else if (what == "loginBox") {
        ok = picked && ui::box_ready(v);
        if (ok) action = A::LoginBox;
    } else if (what == "forget") {
        ok = account_named(v, luaL_checkstring(L, 2)) != nullptr;
        if (ok) {
            v.account_choice = luaL_checkstring(L, 2);
            action = A::ForgetAccount;
        }
    } else if (what == "box" || what == "world" || what == "character") {
        SavedAccount* a = account_named(v, luaL_checkstring(L, 2));
        ok = a != nullptr && picked;
        if (ok && what == "box") {
            ok = a->save_password && ui::can_save_passwords(v);
            if (ok) a->box = lua_toboolean(L, 3) != 0;
        } else if (ok && what == "world") {
            // as the client's World list: the account's character there, the one entered last, else the first seen
            std::string w = luaL_optstring(L, 3, "");
            a->world = w;
            a->character.clear();
            if (!w.empty())
                for (const auto& c : a->characters)
                    if (c.world == w && a->character.empty()) a->character = c.name;
        } else if (ok) {
            // as the client's Character list: the world goes with the character (the account's own world first
            // when the name is on several)
            std::string c = luaL_optstring(L, 3, "");
            const SavedCharacter* pick = nullptr;
            for (const auto& sc : a->characters)
                if (sc.name == c && (!pick || sc.world == a->world)) pick = &sc;
            a->character.clear();
            if (pick) {
                a->character = pick->name;
                a->world = pick->world;
            }
        }
        if (ok) action = A::AccountsChanged;
    } else {
        return luaL_error(L, "eq.LoginDo: no action %s", lua_tostring(L, 1));
    }
    if (action) h.login_action = static_cast<int>(*action);
    lua_pushboolean(L, ok);
    return 1;
}

// ---------------------------------------------------------------- server select

// view = eq.ServerSelect([revision])   -- nil unless the client shows its server select; reading it draws the screen
// in the client's place (as the stock serverselect page's script did). revision: as eq.LoginScreen's.
// view: revision, busy, status {text, error}, server (the saved server's name), servers {name, status, canJoin,
//  favorite} (no player count: no UI shows one; the favourites first, then by name), selected (0: none), last (the
//  row of the world played last here, 0: none), addonHint (a world listed has an add-on the player does not have:
//  the sentence that says so, "" none; docs/addons.md "Suggested add-ons").
template <class Out>
void emit_server_select(Out& o, const EqHost& h, const ui::ServerSelectView& v) {
    o.boolean("busy", v.busy);
    emit_status(o, v.status);
    o.str("server", h.login_server);
    o.table();
    std::size_t last = 0;
    for (std::size_t i = 0; i < v.servers.size(); ++i) {
        const ui::ServerEntry& s = v.servers[i];
        o.table();
        o.str("name", s.name);
        o.str("status", s.status);
        o.boolean("canJoin", s.can_join);
        o.boolean("favorite", s.favorite);
        o.close(i);
        if (!h.last_server.empty() && s.name == h.last_server && last == 0) last = i + 1;
    }
    o.close("servers");
    o.integer("selected", v.selected >= 0 && v.selected < static_cast<int>(v.servers.size()) ? v.selected + 1 : 0);
    o.integer("last", static_cast<lua_Integer>(last));
    o.str("addonHint", v.addon_hint);
}

int ServerSelect(lua_State* L) {
    EqHost& h = host_of(L);
    if (!h.server_select || plugin_window()) {
        lua_pushnil(L);
        return 1;
    }
    mark(L, "serverselect");
    const ui::ServerSelectView& v = *h.server_select;
    return push_view(L, 1, [&h, &v](auto& o) { emit_server_select(o, h, v); });
}

// ok = eq.ServerSelectDo(action [, i]): "select", i; "play" [, i] (the picked world, or row i; only one that can be
// joined); "favorite", i (marks or unmarks row i as a favourite: the list is sorted again the next frame); "back" (to
// the login screen).
int ServerSelectDo(lua_State* L) {
    EqHost& h = host_of(L);
    bool ok = false;
    if (h.server_select && !plugin_window()) {
        ui::ServerSelectView& v = *h.server_select;
        const std::string_view what = luaL_checkstring(L, 1);
        if (what == "back") {
            h.server_select_action = static_cast<int>(ui::ServerSelectAction::Back);
            ok = true;
        } else if (what == "favorite") {
            const int i = index_arg(L, 2, v.servers.size());
            ok = i >= 0 && i < static_cast<int>(v.servers.size());
            if (ok) {
                v.favorite_row = i;
                h.server_select_action = static_cast<int>(ui::ServerSelectAction::Favorite);
            }
        } else if (what == "select" || what == "play") {
            if (!v.busy) {
                const int i = lua_isnoneornil(L, 2) ? v.selected : index_arg(L, 2, v.servers.size());
                ok = i >= 0 && i < static_cast<int>(v.servers.size());
                if (ok) v.selected = i;
                if (ok && what == "play") {
                    ok = v.servers[static_cast<std::size_t>(i)].can_join;
                    if (ok) h.server_select_action = static_cast<int>(ui::ServerSelectAction::Play);
                }
            }
        } else {
            return luaL_error(L, "eq.ServerSelectDo: no action %s", lua_tostring(L, 1));
        }
    }
    lua_pushboolean(L, ok);
    return 1;
}

// ---------------------------------------------------------------- character select

// view = eq.CharacterSelect([revision])   -- nil unless the client shows its character select; reading it draws the
// screen in the client's place. revision: as eq.LoginScreen's.
// view: revision, busy, status {text, error}, world (the world's name), server (the saved server's name), scene (the
//   model is drawn in the middle of the screen), selected (0: none), characters {name, level, race, raceName,
//   classes {ids}, className ("Shadow Knight / Cleric"), classShort ("SHD/CLR"), zone}.
template <class Out>
void emit_char_select(Out& o, const EqHost& h, const ui::CharacterSelectView& v) {
    o.boolean("busy", v.busy);
    emit_status(o, v.status);
    o.str("world", v.server_name);
    o.str("server", h.login_server);
    o.boolean("scene", v.scene);
    o.integer("selected", v.selected >= 0 && v.selected < static_cast<int>(v.characters.size()) ? v.selected + 1 : 0);
    o.table();
    for (std::size_t i = 0; i < v.characters.size(); ++i) {
        const ui::CharacterEntry& c = v.characters[i];
        o.table();
        o.str("name", c.name);
        o.integer("level", c.level);
        o.integer("race", c.race);
        o.str("raceName", game::race_name(c.race));
        o.table();
        for (std::size_t j = 0; j < c.classes.size(); ++j) o.item(j, static_cast<lua_Integer>(c.classes[j]));
        o.close("classes");
        o.str("className", game::class_list_name(c.classes));
        o.str("classShort", game::class_list_name(c.classes, true));
        o.str("zone", c.zone);
        o.boolean("canGoHome", c.can_go_home);
        o.str("homeQuestion", c.home_question);
        o.close(i);
    }
    o.close("characters");
}

int CharacterSelect(lua_State* L) {
    EqHost& h = host_of(L);
    if (!h.char_select || plugin_window()) {
        lua_pushnil(L);
        return 1;
    }
    mark(L, "CharacterListWnd");
    const ui::CharacterSelectView& v = *h.char_select;
    return push_view(L, 1, [&h, &v](auto& o) { emit_char_select(o, h, v); });
}

// ok = eq.CharacterSelectDo(action [, arg]): "select", i (the scene shows that character); "enter" [, i] (enter the
// world with the picked one, or row i); "create" (character creation); "delete" (the picked one, permanently: ask
// first); "home" (Return Home: the picked one enters the world in its home city, when its canGoHome allows; ask first
// with its homeQuestion); "back" / "logout" (to the login screen).
int CharacterSelectDo(lua_State* L) {
    EqHost& h = host_of(L);
    bool ok = false;
    if (h.char_select && !plugin_window()) {
        ui::CharacterSelectView& v = *h.char_select;
        const std::string_view what = luaL_checkstring(L, 1);
        using A = ui::CharacterSelectAction;
        std::optional<A> action;
        const bool picked = v.selected >= 0 && v.selected < static_cast<int>(v.characters.size());
        if (what == "back") action = A::Back;
        else if (what == "logout") action = A::Logout;
        else if (v.busy) {
        } else if (what == "select" || what == "enter") {
            const int i = lua_isnoneornil(L, 2) ? v.selected : index_arg(L, 2, v.characters.size());
            ok = i >= 0 && i < static_cast<int>(v.characters.size());
            if (ok) v.selected = i;
            if (ok && what == "enter") action = A::EnterWorld;
        } else if (what == "create") action = A::Create;
        else if (what == "delete") {
            if (picked) action = A::Delete;
        } else if (what == "home") {
            if (picked && v.characters[static_cast<std::size_t>(v.selected)].can_go_home) action = A::ReturnHome;
        } else {
            return luaL_error(L, "eq.CharacterSelectDo: no action %s", lua_tostring(L, 1));
        }
        if (action) {
            h.char_select_action = static_cast<int>(*action);
            ok = true;
        }
    }
    lua_pushboolean(L, ok);
    return 1;
}


// ---------------------------------------------------------------- character creation

constexpr const char* kStatNames[7] = {"Strength", "Dexterity", "Agility", "Stamina", "Intelligence", "Wisdom", "Charisma"};
constexpr const char* kStatShort[7] = {"STR", "DEX", "AGI", "STA", "INT", "WIS", "CHA"};

void push_choices(lua_State* L, const char* key, const std::vector<std::uint32_t>& ids, std::uint32_t current,
                  const char* index_key, const auto& name_of) {
    lua_newtable(L);
    int at = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        lua_newtable(L);
        set_int(L, "id", ids[i]);
        set_string(L, "name", name_of(ids[i]));
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        if (ids[i] == current) at = static_cast<int>(i + 1);
    }
    lua_setfield(L, -2, key);
    set_int(L, index_key, at);
}

// view = eq.CharacterCreate()   -- nil unless the client shows its character creation (and not while the
// customization window is up on the model); reading it draws the screen in the client's place.
// view: busy, status {text, error}, scene (the model is drawn in the middle of the screen), name, namePending (a
//   random name is on its way), gender (0 male, 1 female), race, raceName, class, className, deity, deityName,
//   city, cityName (ids and names; 0 / "" none), races, classes, deities, cities ({id, name} each: the race's
//   classes, the deities of race and class, the start cities of the deity), raceIndex, classIndex, deityIndex,
//   cityIndex (1-based rows of those lists, 0 none), stats {name, short, value, base, cap, favored (the class puts
//   points in it)}, pointsLeft, ready (Create can go: a name of 4 letters or more, every point spent).
int CharacterCreate(lua_State* L) {
    EqHost& h = host_of(L);
    if (!h.char_create_view || plugin_window()) {
        lua_pushnil(L);
        return 1;
    }
    // (kept from the client while the customization window is up too: the client's screen would show for a frame
    // as that window hides)
    mark(L, "CharacterCreateScreen");
    if (h.customization_shown) {
        lua_pushnil(L);
        return 1;
    }
    ui::CharacterCreateView& v = *h.char_create_view;
    ui::create_normalize(v);
    const ui::CreateOption* o = v.option >= 0 && v.option < static_cast<int>(v.options.size())
                                    ? &v.options[static_cast<std::size_t>(v.option)]
                                    : nullptr;
    const std::uint32_t deity = o ? o->deity : 0, city = o ? o->start_zone : 0;
    lua_newtable(L);
    set_bool(L, "busy", v.busy);
    push_status(L, v.status);
    set_bool(L, "scene", v.scene);
    set_string(L, "name", v.name);
    set_bool(L, "namePending", v.name_pending);
    set_int(L, "gender", v.gender);
    set_int(L, "race", v.race);
    set_string(L, "raceName", game::race_name(v.race));
    set_int(L, "class", o ? v.class_id : 0);
    set_string(L, "className", o ? game::class_name(v.class_id) : std::string());
    set_int(L, "deity", deity);
    set_string(L, "deityName", o ? ui::create_deity_label(v, deity) : std::string());
    set_int(L, "city", city);
    set_string(L, "cityName", o ? game::zone_long_name(city) : std::string());
    push_choices(L, "races", ui::create_races(v), v.race, "raceIndex", [](std::uint32_t r) { return game::race_name(r); });
    push_choices(L, "classes", ui::create_classes(v), v.class_id, "classIndex",
                 [](std::uint32_t c) { return game::class_name(c); });
    push_choices(L, "deities", ui::create_deities(v), deity, "deityIndex",
                 [&v](std::uint32_t d) { return ui::create_deity_label(v, d); });
    push_choices(L, "cities", ui::create_cities(v), city, "cityIndex",
                 [](std::uint32_t z) { return game::zone_long_name(z); });
    lua_newtable(L);
    for (std::size_t i = 0; i < 7; ++i) {
        lua_newtable(L);
        set_string(L, "name", kStatNames[i]);
        set_string(L, "short", kStatShort[i]);
        set_int(L, "value", v.stats[i]);
        set_int(L, "base", o ? static_cast<lua_Integer>(o->base[i]) : 0);
        set_int(L, "cap", ui::create_stat_cap(v, static_cast<int>(i)));
        set_bool(L, "favored", o && o->points[i] > 0);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "stats");
    set_int(L, "pointsLeft", v.points_left);
    set_bool(L, "ready", ui::create_ready(v));
    return 1;
}

// text, title = eq.CharacterCreateHelp(kind, id): the game's description of a choice (dbstr_us.txt, as the stock
// page's help box: a race's type 8, a class's 9, a stat's 10, a deity's 14, a start city's 15) and its name. kind:
// "race", "class", "deity", "city" (by id) or "stat" (1..7, STR..CHA). "" when the game folder has none.
int CharacterCreateHelp(lua_State* L) {
    EqHost& h = host_of(L);
    const std::string_view kind = luaL_checkstring(L, 1);
    const lua_Integer id = luaL_checkinteger(L, 2);
    // (the kind checked before a C++ object is made: a Lua error longjmps past destructors)
    if (kind != "race" && kind != "class" && kind != "deity" && kind != "city" && kind != "stat")
        return luaL_error(L, "eq.CharacterCreateHelp: no kind %s", lua_tostring(L, 1));
    std::uint32_t type = 0, key = 0;
    std::string title;
    if (id >= 0 && id <= 0xffff) key = static_cast<std::uint32_t>(id);
    if (kind == "race") type = 8, title = game::race_name(key);
    else if (kind == "class") type = 9, title = game::class_name(key);
    else if (kind == "deity") {
        type = 14;
        if (h.char_create_view) title = ui::create_deity_label(*h.char_create_view, key);
    } else if (kind == "city") type = 15, title = game::zone_long_name(key);
    else { // stat
        if (id < 1 || id > 7) key = 0, type = 0;
        else type = 10, key = static_cast<std::uint32_t>(id - 1), title = kStatNames[key];
    }
    std::string text = type && h.conn && !plugin_window() ? h.conn->strings().db_string(key, type) : std::string();
    lua_pushlstring(L, text.data(), text.size());
    lua_pushlstring(L, title.data(), title.size());
    return 2;
}

// ok = eq.CharacterCreateDo(action [, value]): what the creation's controls do.
//   "name", text       the name as typed (the client keeps letters only and capitalises it on Create)
//   "randomName"       asks the server for a name for the race and gender (OP_RandomNameGenerator); it fills name
//   "race", id         a race of races; "class", id: a class of classes; "gender", 0 / 1
//   "deity", id        a deity of deities (the city stays when it still goes with it); "city", id: a city of cities
//   "statUp", i / "statDown", i / "statMax", i   stat i (1..7): one point in, one back, as many as it takes
//   "resetStats"       every stat back to its base, every point to spend again
//   "appearance"       the customization window on the model (with the scene)
//   "create"           sends the character (only when view.ready)
//   "cancel"           back to the character select
// A race, class or deity resets the stats. false when the action cannot go now (busy, no such choice).
int CharacterCreateDo(lua_State* L) {
    EqHost& h = host_of(L);
    bool ok = false;
    if (h.char_create_view && !plugin_window()) {
        ui::CharacterCreateView& v = *h.char_create_view;
        const std::string_view what = luaL_checkstring(L, 1);
        using A = ui::CharacterCreateAction;
        std::optional<A> action;
        auto in = [](const std::vector<std::uint32_t>& ids, lua_Integer id) {
            return std::find(ids.begin(), ids.end(), static_cast<std::uint32_t>(id)) != ids.end();
        };
        auto stat = [&]() {
            lua_Integer i = luaL_checkinteger(L, 2);
            return i >= 1 && i <= 7 ? static_cast<int>(i - 1) : -1;
        };
        ui::create_normalize(v);
        if (what == "cancel") {
            if (!v.busy) action = A::Cancel;
        } else if (v.busy) {
        } else if (what == "name") {
            std::string t = luaL_checkstring(L, 2);
            if (t.size() > 64) t.resize(64);
            v.name = std::move(t);
            ok = true;
        } else if (what == "randomName") {
            if (h.conn && !v.name_pending) {
                h.conn->request_random_name(v.race, v.gender);
                v.name_pending = ok = true;
            }
        } else if (what == "race") {
            lua_Integer id = luaL_checkinteger(L, 2);
            if ((ok = in(ui::create_races(v), id))) v.race = static_cast<std::uint32_t>(id);
        } else if (what == "class") {
            lua_Integer id = luaL_checkinteger(L, 2);
            if ((ok = in(ui::create_classes(v), id))) v.class_id = static_cast<std::uint32_t>(id);
        } else if (what == "gender") {
            lua_Integer g = luaL_checkinteger(L, 2);
            if ((ok = g == 0 || g == 1)) v.gender = static_cast<std::uint32_t>(g);
        } else if (what == "deity") {
            lua_Integer id = luaL_checkinteger(L, 2);
            ok = in(ui::create_deities(v), id) && ui::create_set_deity(v, static_cast<std::uint32_t>(id));
        } else if (what == "city") {
            lua_Integer id = luaL_checkinteger(L, 2);
            ok = in(ui::create_cities(v), id) && ui::create_set_city(v, static_cast<std::uint32_t>(id));
        } else if (what == "statUp") {
            ok = ui::create_stat_up(v, stat());
        } else if (what == "statDown") {
            ok = ui::create_stat_down(v, stat());
        } else if (what == "statMax") {
            ok = ui::create_stat_max(v, stat());
        } else if (what == "resetStats") {
            ui::create_reset_stats(v);
            ok = true;
        } else if (what == "appearance") {
            if (v.scene) action = A::Appearance;
        } else if (what == "create") {
            if (ui::create_ready(v)) action = A::Create;
        } else {
            return luaL_error(L, "eq.CharacterCreateDo: no action %s", lua_tostring(L, 1));
        }
        ui::create_normalize(v);
        if (action) {
            h.char_create_action = static_cast<int>(*action);
            ok = true;
        }
    }
    lua_pushboolean(L, ok);
    return 1;
}

} // namespace

void register_pregame_api(lua_State* L, EqHost& host, ScreenReplacements& screens) {
    static const luaL_Reg fns[] = {{"ManagedDistribution", ManagedDistribution},
                                   {"LoginScreen", LoginScreen},         {"LoginSet", LoginSet},
                                   {"LoginDo", LoginDo},                 {"ServerSelect", ServerSelect},
                                   {"ServerSelectDo", ServerSelectDo},   {"CharacterSelect", CharacterSelect},
                                   {"CharacterSelectDo", CharacterSelectDo}, {"CharacterCreate", CharacterCreate},
                                   {"CharacterCreateHelp", CharacterCreateHelp},
                                   {"CharacterCreateDo", CharacterCreateDo}, {nullptr, nullptr}};
    lua_pushlightuserdata(L, &host);
    lua_pushlightuserdata(L, &screens);
    luaL_setfuncs(L, fns, 2);
}

} // namespace hailstone::app
