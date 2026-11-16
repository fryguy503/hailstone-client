// The Lua UI's plugin folders (src/ui/lua/lua_host.h LuaUi::set_folders): a plugin's windows load beside the UI's,
// dofile resolves against each script's own folder, a folder that goes takes its windows with it, and a plugin's window
// covers the pack's of the same name.
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "ui/lua/lua_host.h"
#include "scratch_dir.h"

namespace fs = std::filesystem;

namespace {
std::string global(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "nil";
    lua_pop(L, 1);
    return v;
}
// A global of a plugin folder's own (its scripts' globals are a table of their own).
std::string folder_global(hailstone::ui::lua::LuaUi& lua, const char* tag, const char* name) {
    lua_State* L = lua.state();
    lua.push_folder_env_for_test(tag);
    std::string v = "nil";
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, name);
        if (lua_isstring(L, -1)) v = lua_tostring(L, -1);
        else if (lua_isboolean(L, -1)) v = lua_toboolean(L, -1) ? "true" : "false";
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return v;
}
void set_folder_global(hailstone::ui::lua::LuaUi& lua, const char* tag, const char* name) {
    lua_State* L = lua.state();
    lua.push_folder_env_for_test(tag);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
}
// Runs `code` as a chunk of the folder `tag` (with its globals); its first result as text, or "error: ...".
std::string run_in(hailstone::ui::lua::LuaUi& lua, const char* tag, const std::string& code) {
    lua_State* L = lua.state();
    if (luaL_loadstring(L, code.c_str()) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_pop(L, 1);
        return "error: " + e;
    }
    lua.push_folder_env_for_test(tag);
    lua_setupvalue(L, -2, 1);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_pop(L, 1);
        return "error: " + e;
    }
    std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}
} // namespace

TEST_CASE("Lua UI: a plugin's folder of windows, its own dofile root, gone with the folder") {
    hailstone::test::ScratchDir scratch("lua_folders_test");
    const fs::path pack = scratch.path() / "pack", plug = scratch.path() / "plug" / "ui" / "pack";
    fs::create_directories(pack / "lib");
    fs::create_directories(plug / "lib");
    std::ofstream(pack / "lib" / "x.lua") << "return 'pack'\n";
    std::ofstream(plug / "lib" / "x.lua") << "return 'plugin'\n";
    std::ofstream(pack / "a.lua") << "pack_lib = dofile('lib/x.lua')\n";
    std::ofstream(plug / "p.lua") << "plug_lib = dofile('lib/x.lua')\n"
                                     "ui.window('PluginWin', function() drawn = dofile('lib/x.lua') end)\n";

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        hailstone::ui::lua::LuaUi lua(pack, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        auto frame = [&] {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        };
        frame();
        CHECK(global(L, "pack_lib") == "pack");
        CHECK(global(L, "plug_lib") == "nil");

        lua.set_folders({{"plug", plug}});
        frame();
        CHECK(folder_global(lua, "plug", "plug_lib") == "plugin");
        CHECK(folder_global(lua, "plug", "drawn") == "plugin"); // dofile while drawing: the window's folder
        CHECK(global(L, "plug_lib") == "nil"); // the plugin's globals are its own

        lua.set_folders({});
        frame();
        CHECK(folder_global(lua, "plug", "drawn") == "nil"); // its window and its globals went with the folder
        CHECK(global(L, "pack_lib") == "pack");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Lua UI: a plugin's window covers the pack's of the same name; ui.base_window draws the pack's") {
    hailstone::test::ScratchDir scratch("lua_folders_cover_test");
    const fs::path pack = scratch.path() / "pack", plug = scratch.path() / "plug", other = scratch.path() / "other";
    fs::create_directories(pack);
    fs::create_directories(plug);
    fs::create_directories(other);
    std::ofstream(pack / "pet.lua") << "ui.window('Pet', function() pack_draws = (pack_draws or 0) + 1 end)\n";
    std::ofstream(plug / "pet.lua") << "ui.window('Pet', function()\n"
                                       "  plug_draws = (plug_draws or 0) + 1\n"
                                       "  if use_base then based = ui.base_window() end\n"
                                       "end)\n";

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        hailstone::ui::lua::LuaUi lua(pack, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        auto frame = [&] {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        };
        auto reset = [&] {
            for (const char* g : {"pack_draws", "plug_draws", "based"}) {
                lua_pushnil(L);
                lua_setglobal(L, g);
                set_folder_global(lua, "plug", g);
            }
        };
        frame();
        CHECK(global(L, "pack_draws") == "1");

        // The plugin's covers it: only the plugin's draws.
        lua.set_folders({{"plug", plug}});
        reset();
        frame();
        CHECK(folder_global(lua, "plug", "plug_draws") == "1");
        CHECK(global(L, "pack_draws") == "nil");

        // ui.base_window draws the pack's beneath it.
        lua_pushboolean(L, 1);
        lua_setglobal(L, "use_base");
        reset();
        frame();
        CHECK(global(L, "pack_draws") == "1");
        CHECK(folder_global(lua, "plug", "based") == "true");
        lua_pushboolean(L, 0);
        lua_setglobal(L, "use_base");

        // The pack's file reloading does not take the name back.
        std::ofstream(pack / "pet.lua") << "ui.window('Pet', function() pack_draws = (pack_draws or 0) + 10 end)\n";
        fs::last_write_time(pack / "pet.lua", fs::last_write_time(pack / "pet.lua") + std::chrono::seconds(5));
        lua.set_folders({{"plug", plug}, {"other", other}}); // a folder change rescans every folder
        reset();
        frame();
        CHECK(folder_global(lua, "plug", "plug_draws") == "1");
        CHECK(global(L, "pack_draws") == "nil");

        // The plugin goes: the pack's (reloaded) returns.
        lua.set_folders({});
        reset();
        frame();
        CHECK(folder_global(lua, "plug", "plug_draws") == "nil");
        CHECK(global(L, "pack_draws") == "10");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Lua UI: a plugin's code keeps to its own globals, sees hs, ui and the libraries read-only, loads text only") {
    hailstone::test::ScratchDir scratch("lua_folders_confine_test");
    const fs::path pack = scratch.path() / "pack", plug = scratch.path() / "plug";
    fs::create_directories(pack / "lib");
    fs::create_directories(plug);
    std::ofstream(pack / "lib" / "x.lua") << "shared_by_lib = (shared_by_lib or 0) + 1\nreturn 'pack'\n";
    std::ofstream(pack / "a.lua") << "hs.Thing = function() return 'real' end\n"
                                     "shared = { n = 1 }\n";
    std::ofstream(plug / "p.lua") << "seen_shared = shared.n\n"
                                     "shared = 'mine'\n"           // its own global, not the pack's
                                     "pack_lib = dofile('lib/x.lua')\n";
    // A precompiled chunk, and files outside the folders.
    {
        lua_State* other = luaL_newstate();
        luaL_openlibs(other);
        REQUIRE(luaL_dostring(other, "return string.dump(function() return 'binary' end)") == LUA_OK);
        std::size_t n = 0;
        const char* bytes = lua_tolstring(other, -1, &n);
        std::ofstream(pack / "lib" / "bin.lua", std::ios::binary).write(bytes, static_cast<std::streamsize>(n));
        std::ofstream(pack / "bin2.lua", std::ios::binary).write(bytes, static_cast<std::streamsize>(n));
        lua_close(other);
    }
    std::ofstream(scratch.path() / "outside.lua") << "return 'outside'\n";

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        hailstone::ui::lua::LuaUi lua(pack, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        auto frame = [&] {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        };
        lua.set_folders({{"plug", plug}});
        frame();
        // The pack's precompiled script did not load.
        CHECK(ImGui::FindWindowByName("UI errors") != nullptr);
        CHECK(folder_global(lua, "plug", "seen_shared") == "1"); // the pack's globals read through
        CHECK(run_in(lua, "plug", "return type(shared)") == "string");

        auto pack_run = [&](const char* code) {
            if (luaL_dostring(L, code) != LUA_OK) {
                std::string e = lua_tostring(L, -1);
                lua_settop(L, 0);
                return "error: " + e;
            }
            std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
            lua_settop(L, 0);
            return out;
        };
        CHECK(pack_run("return type(shared)") == "table");
        CHECK(pack_run("return shared_by_lib") == "nil"); // the pack's lib, dofile'd by the plugin, set the plugin's
        CHECK(folder_global(lua, "plug", "shared_by_lib") == "1");

        // hs, ui and the libraries: read, not written; the pack's stay as they are.
        CHECK(run_in(lua, "plug", "return hs.Thing()") == "real");
        CHECK(run_in(lua, "plug", "hs.Thing = function() return 'evil' end").find("cannot be changed") != std::string::npos);
        CHECK(run_in(lua, "plug", "ui.Begin = nil").find("cannot be changed") != std::string::npos);
        CHECK(run_in(lua, "plug", "ui.Col.Text = 99").find("cannot be changed") != std::string::npos);
        CHECK(run_in(lua, "plug", "string.format = nil").find("cannot be changed") != std::string::npos);
        CHECK(run_in(lua, "plug", "os.time = nil").find("cannot be changed") != std::string::npos);
        CHECK(run_in(lua, "plug", "return getmetatable('')") == "false");
        CHECK(run_in(lua, "plug", "return getmetatable(hs)") == "false");
        CHECK(run_in(lua, "plug", "return getmetatable(_G)") == "false");
        CHECK(run_in(lua, "plug", "return rawget(_G, 'hs')") == "nil");
        CHECK(run_in(lua, "plug", "_G.hs = {} hs = nil return type(hs.Thing)") == "function"); // its own, then the view
        CHECK(run_in(lua, "plug", "local t = {} for k in pairs(_G) do t[#t+1] = k end table.sort(t) return table.concat(t, ',')")
                  .find("ui") == std::string::npos);
        CHECK(pack_run("return hs.Thing()") == "real");
        CHECK(pack_run("return type(string.format) .. type(ui.Begin) .. ui.Col.Text") == "functionfunction0");
        // load: text only, with the plugin's own globals unless it names others.
        CHECK(run_in(lua, "plug", "return load('loaded_here = 1 return 2')()") == "2");
        CHECK(folder_global(lua, "plug", "loaded_here") == "1");
        CHECK(pack_run("return loaded_here") == "nil");
        CHECK(run_in(lua, "plug", "return select(2, load(string.dump(function() end)))").find("binary") != std::string::npos);
        CHECK(pack_run("return select(2, load(string.dump(function() end), 'x', 'b'))").find("binary") != std::string::npos);
        CHECK(pack_run("return loadfile") == "nil");
        CHECK(run_in(lua, "plug", "return loadfile") == "nil");

        // dofile: inside the folders only, text only.
        CHECK(run_in(lua, "plug", "return dofile('lib/x.lua')") == "pack");
        CHECK(run_in(lua, "plug", "return dofile('lib/bin.lua')").find("binary") != std::string::npos);
        CHECK(pack_run("return dofile('lib/bin.lua')").find("binary") != std::string::npos);
        CHECK(pack_run("return dofile('../outside.lua')").find("not in the UI's folder") != std::string::npos);
        CHECK(run_in(lua, "plug", "return dofile('../outside.lua')").find("not in the UI's folder") != std::string::npos);
        const std::string absolute = (scratch.path() / "outside.lua").string();
        CHECK(pack_run(("return dofile([[" + absolute + "]])").c_str()).find("not in the UI's folder") != std::string::npos);
        CHECK(run_in(lua, "plug", "return dofile([[" + absolute + "]])").find("not in the UI's folder") !=
              std::string::npos);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Lua UI: a plugin's code runs as the plugin: its load, its windows, and the hs functions it took") {
    hailstone::test::ScratchDir scratch("lua_folders_caller_test");
    const fs::path pack = scratch.path() / "pack", plug = scratch.path() / "plug";
    fs::create_directories(pack);
    fs::create_directories(plug);
    // The pack calls whatever a plugin put into a table they share (a plugin closure run by the pack's code).
    std::ofstream(pack / "a.lua") << "shared = {}\n"
                                     "ui.window('PackWin', function()\n"
                                     "  pack_draw = hs.Who()\n"
                                     "  if shared.cb then via_pack = shared.cb() end\n"
                                     "end)\n";
    std::ofstream(plug / "p.lua") << "at_load = hs.Who()\n"
                                     "local who = hs.Who\n"
                                     "shared.cb = function() return who() end\n"
                                     "ui.window('PlugWin', function() at_draw = hs.Who() end)\n";
    std::vector<std::string> scopes;
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        hailstone::ui::lua::LuaUi lua(pack, scratch.path() / "ui.txt");
        lua.add_ui_api([](lua_State* L) {
            lua_pushcfunction(L, [](lua_State* S) -> int {
                const std::string tag(hailstone::ui::lua::LuaUi::caller_tag());
                lua_pushstring(S, tag.empty() ? "pack" : tag.c_str());
                return 1;
            });
            lua_setfield(L, -2, "Who");
        });
        int live = 0;
        lua.set_caller_scope([&](std::string_view tag) -> std::shared_ptr<void> {
            scopes.emplace_back(tag);
            ++live;
            return std::shared_ptr<void>(nullptr, [&](void*) { --live; });
        });
        lua_State* L = lua.state();
        auto frame = [&] {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        };
        lua.set_folders({{"plug", plug}});
        frame();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK(folder_global(lua, "plug", "at_load") == "plug");
        CHECK(folder_global(lua, "plug", "at_draw") == "plug");
        CHECK(global(L, "pack_draw") == "pack");
        CHECK(global(L, "via_pack") == "plug"); // the plugin's closure, run by the pack: still the plugin
        CHECK(live == 0); // every scope ended
        CHECK(std::find(scopes.begin(), scopes.end(), "plug") != scopes.end());
        CHECK(std::find(scopes.begin(), scopes.end(), "") == scopes.end()); // the pack's own code: none
        CHECK(hailstone::ui::lua::LuaUi::caller_tag().empty());
        // An error inside a wrapped call ends its scope too.
        CHECK(run_in(lua, "plug", "return pcall(hs.Who, nil) and hs.Who()") == "plug");
        CHECK(run_in(lua, "plug", "return select(2, pcall(ui.window, 'x'))").find("bad argument") != std::string::npos);
        CHECK(live == 0);
        CHECK(hailstone::ui::lua::LuaUi::caller_tag().empty());
        // A window the plugin registers from outside its load is still the plugin's: it goes with the folder, and
        // one registered after that is not taken.
        run_in(lua, "plug",
               "shared.cb = function()\n"
               "  ui.window('Late', function() shared.late = hs.Who() end)\n"
               "  return 'x'\n"
               "end");
        frame();
        frame();
        CHECK(luaL_dostring(L, "return shared.late") == LUA_OK);
        CHECK(std::string(lua_tostring(L, -1) ? lua_tostring(L, -1) : "nil") == "plug");
        lua_settop(L, 0);
        lua.set_folders({});
        frame(); // (PackWin calls the plugin's cb again: refused, its folder gone)
        REQUIRE(luaL_dostring(L, "shared.late = nil") == LUA_OK);
        frame();
        CHECK(luaL_dostring(L, "return shared.late") == LUA_OK);
        CHECK(lua_isnil(L, -1));
        lua_settop(L, 0);
    }
    ImGui::DestroyContext(ctx);
}
