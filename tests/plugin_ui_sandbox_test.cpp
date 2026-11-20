// What a plugin's windows (ui::lua::LuaUi::set_folders, docs/plugins.md "Plugin windows") may do: through the hs
// table's plugin part (app/lua_ui_plugins.cpp) and the UI bridge (plugin/ui_bridge.h) the bridge's lists stay sound
// while plugin code runs from them and one owner's code does not drive another's; hs functions that act need the
// plugin's capability (app/lua_ui_caps.h); the consent question, the pack's shared tables, its settings and the Lua
// state's time and memory are guarded (ui/lua/lua_host.h).
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <fstream>
#include <memory>
#include <iterator>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_caps.h"
#include "app/lua_ui_plugins.h"
#include "plugin/policy.h"
#include "plugin/ui_bridge.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

// A pack, the plugin part of hs over a bridge, and plugin folders by tag (each with a file, so its globals exist).
struct SandboxUi {
    test::ScratchDir scratch{"plugin_ui_sandbox"};
    ImGuiContext* ctx = nullptr;
    plugin::UiBridge bridge;
    app::UiHost host;
    std::unique_ptr<app::UiApi> api;
    std::unique_ptr<ui::lua::LuaUi> lua;
    std::map<std::string, std::uint32_t> caps; // each folder tag's capabilities (the app's plugin_caps)
    std::vector<ui::lua::LuaUi::Folder> folders;

    SandboxUi() {
        ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        fs::create_directories(scratch.path() / "pack");
        host.plugin_ui = &bridge;
        api = std::make_unique<app::UiApi>(host);
        lua = std::make_unique<ui::lua::LuaUi>(scratch.path() / "pack", scratch.path() / "ui.txt",
            std::vector<fs::path>{}, ui::lua::LuaUi::DataRoots{});
        lua->add_ui_api([this](lua_State* L) { api->register_functions(L); });
        // As the app does (ClientApp::lua_caller_scope): a folder's code runs with its owner's capabilities.
        lua->set_caller_scope([this](std::string_view tag) -> std::shared_ptr<void> {
            auto it = caps.find(std::string(tag));
            return std::make_shared<plugin::CallerScope>(it == caps.end() ? 0u : it->second, tag);
        });
    }
    ~SandboxUi() {
        lua.reset();
        api.reset();
        ImGui::DestroyContext(ctx);
    }
    // A file of the pack's (before the first frame, which loads the pack).
    void add_pack_file(const std::string& name, const std::string& code) {
        std::ofstream(scratch.path() / "pack" / name) << code;
    }
    void add_folder(const std::string& tag, const std::string& code = "loaded = true\n") {
        const fs::path dir = scratch.path() / "folders" / std::to_string(folders.size());
        fs::create_directories(dir);
        std::ofstream(dir / "w.lua") << code;
        folders.push_back({tag, dir});
        lua->set_folders(folders);
    }
    void frame() {
        ImGui::GetIO().DisplaySize = ImVec2(800, 600);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        api->begin_frame();
        ImGui::NewFrame();
        lua->frame();
        ImGui::Render();
    }
    std::string run(const std::string& tag, const std::string& code) { return lua->run_for_test(tag, code); }
};

} // namespace

// hs.PluginOptions runs each option's get; a get that adds options (moving the bridge's list) or replaces its own
// option (destroying the function running) leaves the call sound.
TEST_CASE("Plugin UI: hs.PluginOptions with a get that adds and replaces options") {
    SandboxUi s;
    int added = 0;
    std::function<int()> get = [&]() -> int {
        // Enough new options to move the list (up to the owner's UiBridge::kMaxOptions), and this very option replaced.
        for (int i = 0; i < 62; ++i)
            s.bridge.add_option({"P", "extra" + std::to_string(added++), "Extra", 0, 1, [] { return 0; }, [](int) {}});
        s.bridge.add_option({"P", "first", "First again", 0, 1, [] { return 5; }, [](int) {}});
        return 1;
    };
    s.bridge.add_option({"P", "first", "First", 0, 1, get, [](int) {}});
    s.bridge.add_option({"P", "second", "Second", 0, 1, [] { return 2; }, [](int) {}});
    s.frame();
    CHECK(s.run("", "local o = hs.PluginOptions() return #o .. ' ' .. o[1].value .. ' ' .. o[2].value") == "2 1 2");
    CHECK(added == 62);
    CHECK(s.bridge.options().size() == 64);
}

// A plugin's menu entry runs its command with the plugin's capabilities, whoever chose it (the player in the EQ menu,
// or the gamepad pack's More page).
TEST_CASE("Plugin UI: a menu entry's command runs as its owner") {
    SandboxUi s;
    struct Sent {
        std::string line;
        std::uint32_t caps;
        std::string as;
    };
    std::vector<Sent> sent;
    s.host.submit_chat = [&](const std::string& line) {
        sent.push_back({line, plugin::caller_caps(), std::string(plugin::caller_name())});
    };
    s.host.plugin_caps = [](std::string_view tag) {
        return tag == "plugin:P" ? plugin::kCapRead | plugin::kCapUi : tag == "script:P" ? plugin::kCapAll : 0u;
    };
    s.bridge.add_menu({"P", "Cast", "/cast 1"});
    s.bridge.add_menu({"P", "Theirs", "/say hi", plugin::UiBridge::Kind::Script});
    s.frame();
    CHECK(s.run("", "hs.PluginMenuClick(1) hs.PluginMenuClick(2) hs.PluginMenuClick(3) return #hs.PluginMenu()") ==
          "2");
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].line == "/cast 1");
    CHECK(sent[0].caps == (plugin::kCapRead | plugin::kCapUi));
    CHECK(sent[0].as == "plugin:P");
    CHECK(sent[1].caps == plugin::kCapAll);
    CHECK(sent[1].as == "script:P");
    CHECK(plugin::caller_caps() == plugin::kCapAll); // the scope ended
    CHECK(plugin::caller_name().empty());
}

// A plugin's code reaches only its own exported functions, options and menu entries; the pack reaches all of them.
TEST_CASE("Plugin UI: a plugin cannot drive another's functions, options or menu entries") {
    SandboxUi s;
    std::vector<std::string> calls, sent;
    int a_value = 0, b_value = 0;
    for (const char* who : {"A", "B"}) {
        s.bridge.add_function(who, "f", [&calls, who](const std::vector<plugin::UiValue>&) {
            calls.push_back(who);
            return plugin::UiValue::string(who);
        });
    }
    s.bridge.add_option({"A", "o", "O", 0, 9, [&] { return a_value; }, [&](int v) { a_value = v; }});
    s.bridge.add_option({"B", "o", "O", 0, 9, [&] { return b_value; }, [&](int v) { b_value = v; }});
    s.bridge.add_menu({"B", "B's", "/b"});
    s.bridge.add_function("A", "g", [](const std::vector<plugin::UiValue>&) { return plugin::UiValue::string("s"); },
                          plugin::UiBridge::Kind::Script); // a script named A: not the plugin A
    s.host.submit_chat = [&](const std::string& line) { sent.push_back(line); };
    s.host.plugin_caps = [](std::string_view) { return plugin::kCapAll; };
    s.add_folder("plugin:A");
    s.frame();

    // The pack: everyone's.
    CHECK(s.run("", "PB = hs.Plugin('B') return PB.f() .. hs.Plugin('A').f()") == "BA");
    CHECK(s.run("", "hs.SetPluginOption('B', 'o', 4) return #hs.PluginOptions()") == "2");
    CHECK(b_value == 4);
    calls.clear();

    // Plugin A: its own only.
    CHECK(s.run("plugin:A", "return hs.Plugin('A').f() .. ' ' .. hs.Plugin('A').name .. ' ' .. hs.Plugin('A').owner") ==
          "A A plugin:A");
    CHECK(s.run("plugin:A", "return hs.Plugin('B')") == "nil");
    CHECK(s.run("plugin:A", "return hs.Plugin('plugin:B')") == "nil");
    CHECK(s.run("plugin:A", "return hs.Plugin('script:A')") == "nil"); // the script of its name is another owner
    CHECK(s.run("plugin:A", "return PB.f()") == "nil");                // B's object the pack left in its globals
    CHECK(s.run("plugin:A", "return PB.call('f')") == "nil");
    CHECK(calls == std::vector<std::string>{"A"});
    CHECK(s.run("plugin:A", "hs.SetPluginOption('B', 'o', 7) hs.SetPluginOption('A', 'o', 3) return 1") == "1");
    CHECK(b_value == 4);
    CHECK(a_value == 3);
    CHECK(s.run("plugin:A", "local o = hs.PluginOptions() return #o .. o[1].owner") == "1plugin:A");
    CHECK(s.run("plugin:A", "hs.PluginMenuClick(1) return #hs.PluginMenu()") == "0"); // B's entry: not listed
    CHECK(sent.empty());
    CHECK(s.run("", "hs.PluginMenuClick(1) return 1") == "1");
    CHECK(sent == std::vector<std::string>{"/b"});
}

// A plugin's hs.PluginMenu lists its own entries alone, and hs.PluginMenuClick numbers them as that list does; the
// pack's lists and numbers every entry.
TEST_CASE("Plugin UI: a plugin's menu list and clicks are its own entries") {
    SandboxUi s;
    std::vector<std::string> sent;
    s.host.submit_chat = [&](const std::string& line) { sent.push_back(line); };
    s.host.plugin_caps = [](std::string_view) { return plugin::kCapAll; };
    s.bridge.add_menu({"B", "B's", "/b"});
    s.bridge.add_menu({"A", "A's", "/a"});
    s.add_folder("plugin:A");
    s.frame();
    CHECK(s.run("plugin:A", "local m = hs.PluginMenu() return #m .. m[1].label .. m[1].owner") == "1A'splugin:A");
    CHECK(s.run("plugin:A", "hs.PluginMenuClick(2) return 1") == "1"); // past its own list: nothing
    CHECK(sent.empty());
    CHECK(s.run("plugin:A", "hs.PluginMenuClick(1) return 1") == "1");
    CHECK(sent == std::vector<std::string>{"/a"});
    CHECK(s.run("", "return #hs.PluginMenu()") == "2");
    CHECK(s.run("", "hs.PluginMenuClick(1) return 1") == "1");
    CHECK(sent == std::vector<std::string>{"/a", "/b"});
}

// The player's one-shot keys and requests are the pack's windows' to take: a plugin's code that asks gets nil and
// leaves them (a pending Link All is not sent from its call). Nor can it put away the client's dialogs that act when
// answered, to draw a question of its own in their place.
TEST_CASE("Plugin UI: a plugin's chat line is sent but stays out of the player's input history") {
    SandboxUi s;
    std::vector<std::string> sent;
    s.host.submit_chat = [&](const std::string& line) { sent.push_back(line); };
    s.caps["plugin:A"] = plugin::kCapAll;
    s.add_folder("plugin:A");
    s.frame();
    s.run("", "hs.ChatSubmit(1, '/mine') return 1");
    s.run("plugin:A", "hs.ChatSubmit(1, '/theirs') return 1");
    REQUIRE(sent.size() == 2);
    CHECK(sent[1] == "/theirs");
    // The input's Up gives the player's own line, not the plugin's.
    CHECK(s.run("", "return hs.ChatHistory(1, true)") == "/mine");
}

TEST_CASE("Plugin UI: a plugin cannot take the player's keys and requests, nor replace an acting dialog") {
    SandboxUi s;
    std::vector<std::string> sent;
    s.host.submit_chat = [&](const std::string& line) { sent.push_back(line); };
    s.caps["plugin:A"] = plugin::kCapAll;
    s.add_folder("plugin:A");
    s.frame();

    s.host.lua_social_edit = 3;
    CHECK(s.run("plugin:A", "return hs.LuaSocialEditRequest()") == "nil");
    CHECK(s.host.lua_social_edit == 3);
    CHECK(s.run("", "return hs.LuaSocialEditRequest()") == "3");
    CHECK(s.host.lua_social_edit == -1);

    app::chat_page_key(-1);
    CHECK(s.run("plugin:A", "return hs.ChatPage(1)") == "nil");
    app::chat_history_key(true);
    CHECK(s.run("plugin:A", "return hs.ChatHistory(1, true)") == "nil");
    app::request_chat_link_all({"tag"});
    CHECK(s.run("plugin:A", "return hs.ChatTakeFocus(1, '')") == "nil");
    CHECK(sent.empty());
    app::tell_key();
    CHECK(s.run("plugin:A", "return hs.ChatTakeFocus(1)") == "nil");
    // (whatever the pack's chat windows here make of them, the pending keys are taken by the pack)
    s.run("", "hs.ChatTakeFocus(1, '') hs.ChatTakeFocus(1) hs.ChatPage(1) return 1");

    for (const char* dialog : {"ConfirmationDialogBox", "TradeWnd", "GiveWnd", "QuantityWnd", "GroupInvite",
                               "RespawnWnd", "PluginConsent"}) {
        CAPTURE(dialog);
        s.run("plugin:A", std::string("hs.ReplaceWindow('") + dialog + "') hs.WindowShown('" + dialog + "') return 1");
        CHECK_FALSE(s.api->replaces(dialog));
        s.run("", std::string("hs.WindowShown('") + dialog + "') return 1");
        CHECK(s.api->replaces(dialog));
    }
    s.run("plugin:A", "hs.WindowShown('InventoryWindow') return 1"); // a window that does not act: as before
    CHECK(s.api->replaces("InventoryWindow"));
}

// Every hs function says what a plugin's code needs to call it (app/lua_ui_caps.h): a new one must be put in a group.
TEST_CASE("Plugin UI: every hs function is listed with what a plugin needs to call it") {
    SandboxUi s;
    lua_State* L = s.lua->state();
    std::vector<std::string> unlisted;
    std::function<void(int, const std::string&)> walk = [&](int t, const std::string& prefix) {
        t = lua_absindex(L, t);
        lua_pushnil(L);
        while (lua_next(L, t)) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const std::string name = prefix + lua_tostring(L, -2);
                if (lua_isfunction(L, -1) && !app::ui_call_caps(name)) unlisted.push_back(name);
                else if (lua_istable(L, -1)) walk(-1, name + ".");
            }
            lua_pop(L, 1);
        }
    };
    lua_getglobal(L, "hs");
    walk(-1, "");
    lua_pop(L, 1);
    std::sort(unlisted.begin(), unlisted.end());
    std::string all;
    for (const auto& n : unlisted) all += n + " ";
    CHECK_MESSAGE(unlisted.empty(), all);
}

// A plugin's code calls an hs function that acts in the game only with the capability it needs (app/lua_ui_caps.h);
// refused, it does nothing and gives nil. The pack's own code calls everything.
TEST_CASE("Plugin UI: hs functions that act need the plugin's capability") {
    SandboxUi s;
    s.caps["plugin:R"] = plugin::kCapRead | plugin::kCapUi;
    s.caps["plugin:A"] = plugin::kCapRead | plugin::kCapUi | plugin::kCapActions;
    s.add_folder("plugin:R");
    s.add_folder("plugin:A");
    s.frame();
    CHECK(app::ui_call_caps("ClickSlot") == plugin::kCapActions);
    CHECK(app::ui_call_caps("ShowWindow") == plugin::kCapCommands);
    CHECK(app::ui_call_caps("Item") == 0u);
    CHECK(app::ui_call_caps("ItemJob") == 0u); // reads the job; StopItemJob acts
    CHECK(app::ui_call_caps("StopItemJob") == plugin::kCapActions);
    // Walks: a /nav walk is movement, the Find window's and the zone finder's alike.
    CHECK(app::ui_call_caps("ZoneExitWalk") == plugin::kCapMove);
    CHECK(app::ui_call_caps("FindNpcWalk") == plugin::kCapMove);
    // Menu mode, as set_mode("menu"): the pad's buttons stop driving the character.
    CHECK(app::ui_call_caps("gamepad.request_focus") == plugin::kCapCommands);
    CHECK(app::ui_call_caps("gamepad.set_mode") == plugin::kCapCommands);
    CHECK(app::ui_call_caps("gamepad.rumble") == 0u);
    // A dry run runs the code it is given with the server's Lua social capabilities: as SetLuaSocial.
    CHECK(app::ui_call_caps("LuaSocialDryRun") == (plugin::kCapCommands | plugin::kCapActions));
    // The autoskill list's entries and rules choose what it uses later, on its own: actions as well.
    CHECK(app::ui_call_caps("AbilityBookAutoskill") == (plugin::kCapCommands | plugin::kCapActions));
    CHECK(app::ui_call_caps("AbilityBookAutoskillRule") == (plugin::kCapCommands | plugin::kCapActions));
    CHECK_FALSE(app::ui_call_caps("NoSuchFunction"));
    // (no such window: the real functions give false; a refused call gives nil)
    const std::string calls = "return tostring(hs.ClickChild('NoWnd', 'X')) .. ' ' .. tostring(hs.WindowButton('NoWnd', "
                              "'X')) .. ' ' .. tostring(hs.InGame())";
    CHECK(s.run("", calls) == "false false false");
    CHECK(s.run("plugin:A", calls) == "false false false");
    CHECK(s.run("plugin:R", calls) == "nil nil false");
    // Through a function of the pack's that the plugin calls, and through one it took earlier: the same.
    CHECK(s.run("", "function PackClick() return tostring(hs.ClickChild('NoWnd', 'X')) end return 1") == "1");
    CHECK(s.run("plugin:R", "return PackClick()") == "nil");
    CHECK(s.run("plugin:R", "local f = hs.ClickChild return tostring(f('NoWnd', 'X'))") == "nil");
    CHECK(s.run("", "return PackClick()") == "false");
}

TEST_CASE("Plugin UI: modular contract aliases retain caller identity and nested read-only views") {
    SandboxUi s;
    s.caps["plugin:R"] = plugin::kCapRead | plugin::kCapUi;
    s.caps["plugin:A"] = plugin::kCapRead | plugin::kCapUi | plugin::kCapActions;
    s.add_folder("plugin:R");
    s.add_folder("plugin:A");
    s.frame();

    // `eq`, the name up to 0.7, stays for good, in the pack's own code as in an add-on's: the same table as hs.
    CHECK(s.run("", "return hs.UI ~= nil and eq == hs") == "true");
    CHECK(s.run("plugin:R", "return hs.UI ~= nil and eq == hs") == "true");

    const std::string action = "return tostring(hs.UI.windows.ClickChild('NoWnd', 'X'))";
    CHECK(s.run("", action) == "false");
    CHECK(s.run("plugin:A", action) == "false");
    CHECK(s.run("plugin:R", action) == "nil");
    CHECK(s.run("plugin:R", "return hs.UI.windows.ClickChild == hs.ClickChild and "
                            "hs.UI.gamepad.SetMode == hs.gamepad.set_mode") == "true");
    CHECK(s.run("plugin:R", "return hs.UI.Supports('windows', 'ClickChild')") == "true");
    CHECK(s.run("plugin:R", "return hs.UI.Describe('windows', 'ClickChild').requiredCaps") ==
          std::to_string(plugin::kCapActions));
    CHECK(s.run("plugin:R", "hs.UI.windows.Show = function() end").find("cannot be changed") != std::string::npos);
    CHECK(s.run("plugin:R", "hs.UI.version.major = 999").find("cannot be changed") != std::string::npos);
    // rawset can shadow a field on this folder's proxy, as on the legacy hs proxy, without changing the real API.
    CHECK(s.run("plugin:R", "rawset(hs.UI.windows, 'Show', function() return 'local' end) "
                            "return hs.UI.windows.Show()") == "local");
    CHECK(s.run("", "return hs.UI.windows.Show == hs.ShowWindow") == "true");
    CHECK(s.run("plugin:A", "return hs.UI.windows.Show == hs.ShowWindow") == "true");
    CHECK(s.run("plugin:R", "return getmetatable(hs.UI.windows)") == "false");

    CHECK(s.run("", "function ModularPackClick() return tostring(hs.UI.windows.ClickChild('NoWnd', 'X')) end "
                    "return 1") == "1");
    CHECK(s.run("plugin:R", "return ModularPackClick()") == "nil");
    CHECK(s.run("plugin:R", "saved = hs.UI.windows.ClickChild return tostring(saved('NoWnd', 'X'))") == "nil");
    CHECK(s.run("", "return ModularPackClick()") == "false");

    s.host.lua_social_edit = 7;
    CHECK(s.run("plugin:A", "return hs.UI.luasocials.EditRequest()") == "nil");
    CHECK(s.host.lua_social_edit == 7);
    CHECK(s.run("", "return hs.UI.luasocials.EditRequest()") == "7");
    CHECK(s.host.lua_social_edit == -1);

    // A folder's saved alias keeps its owner when the pack calls it, and stops after that folder goes away.
    CHECK(s.run("", "MODULAR_SHARED = {} return 1") == "1");
    CHECK(s.run("plugin:R", "MODULAR_SHARED.click = hs.UI.windows.ClickChild return 1") == "1");
    CHECK(s.run("", "local result = MODULAR_SHARED.click('NoWnd', 'X') return tostring(result)") == "nil");
    CHECK(s.run("plugin:A", "return MODULAR_SHARED.click('NoWnd', 'X')").find("plugin:R's") != std::string::npos);
    s.folders.erase(s.folders.begin());
    s.lua->set_folders(s.folders);
    s.frame();
    CHECK(s.run("", "local result = MODULAR_SHARED.click('NoWnd', 'X') return tostring(result)") == "nil");
    CHECK(s.run("", "return ModularPackClick()") == "false");
}

TEST_CASE("Plugin UI: retained modular aliases keep caller checks after real API registration repeats") {
    SandboxUi s;
    s.caps["plugin:R"] = plugin::kCapRead | plugin::kCapUi;
    s.add_folder("plugin:R");
    s.frame();
    CHECK(s.run("plugin:R", "oldUI=hs.UI oldClick=hs.UI.windows.ClickChild "
                            "return tostring(oldClick('NoWnd','X'))") == "nil");
    s.lua->add_ui_api([&](lua_State* L) { s.api->register_functions(L); });
    CHECK(s.run("plugin:R", "return oldUI ~= hs.UI and hs.UI.windows.ClickChild == hs.ClickChild") == "true");
    CHECK(s.run("plugin:R", "return tostring(oldClick('NoWnd','X')) .. ':' .. "
                            "tostring(hs.UI.windows.ClickChild('NoWnd','X'))") == "nil:nil");
    s.caps["plugin:R"] |= plugin::kCapActions;
    CHECK(s.run("plugin:R", "return tostring(oldClick('NoWnd','X')) .. ':' .. "
                            "tostring(hs.UI.windows.ClickChild('NoWnd','X'))") == "false:false");
}

TEST_CASE("Plugin UI: deprecated addon namespace is the same guarded view without a root alias") {
    SandboxUi s;
    s.host.in_game = true;
    s.caps["plugin:legacy"] = plugin::kCapRead | plugin::kCapUi;
    s.caps["script:legacy"] = plugin::kCapRead | plugin::kCapUi | plugin::kCapActions;
    s.bridge.add_function("legacy", "read", [](const std::vector<plugin::UiValue>&) {
        return plugin::UiValue::string("native");
    });
    s.bridge.add_function("legacy", "read", [](const std::vector<plugin::UiValue>&) {
        return plugin::UiValue::string("script");
    }, plugin::UiBridge::Kind::Script);
    for (const char* tag : {"plugin:legacy", "script:legacy"})
        s.add_folder(tag, R"lua(
local owner=eq.Plugin('legacy')
legacyRead=eq.InGame() and owner.read()
ui.window('Legacy addon##'..owner.owner,function() legacyDraw=eq.InGame() end)
)lua");
    s.frame();
    CHECK(s.run("", "return hs.UI ~= nil and eq == hs and rawget(_G,'eq') == hs") == "true");
    CHECK(s.run("plugin:legacy", "return legacyRead .. ':' .. tostring(legacyDraw)") == "native:true");
    CHECK(s.run("script:legacy", "return legacyRead .. ':' .. tostring(legacyDraw)") == "script:true");
    for (const char* tag : {"plugin:legacy", "script:legacy"}) {
        CHECK(s.run(tag, "return eq == hs and eq.UI == hs.UI and eq.ClickChild == hs.ClickChild") == "true");
        CHECK(s.run(tag, "return type(eq.SpellBook)..':'..type(eq.AbilityBook)..':'..type(eq.ItemIcon)..':'.."
                         "type(eq.SpellIcon)..':'..type(eq.ShowSpell)") == "function:function:function:function:function");
        CHECK(s.run(tag, "eq.InGame = function() return false end").find("cannot be changed") != std::string::npos);
        CHECK(s.run(tag, "eq.UI.windows.ClickChild = function() return true end").find("cannot be changed") != std::string::npos);
    }
    CHECK(s.run("plugin:legacy", "return tostring(eq.ClickChild('NoWnd','X'))") == "nil");
    CHECK(s.run("script:legacy", "return tostring(eq.ClickChild('NoWnd','X'))") == "false");
    s.caps["plugin:legacy"] |= plugin::kCapActions;
    CHECK(s.run("plugin:legacy", "return tostring(eq.ClickChild('NoWnd','X'))") == "false");
    s.caps["plugin:legacy"] &= ~plugin::kCapActions;
    CHECK(s.run("plugin:legacy", "return tostring(eq.ClickChild('NoWnd','X'))") == "nil");
}

TEST_CASE("Plugin UI: the eq alias in add-ons retains reload generations and removed-owner checks") {
    SandboxUi s;
    s.caps["plugin:legacy"] = plugin::kCapRead | plugin::kCapUi;
    s.add_pack_file("shared.lua", "LEGACY_SAVED={}\n");
    s.add_folder("plugin:legacy", R"lua(
LEGACY_SAVED.click=eq.ClickChild
LEGACY_SAVED.register=eq.UI.extensions.RegisterAction
version=1
)lua");
    s.frame();
    CHECK(s.run("", "return tostring(LEGACY_SAVED.click('NoWnd','X'))") == "nil");
    const auto file = s.folders[0].dir / "w.lua";
    const auto before = fs::last_write_time(file);
    std::ofstream(file) << "version=2;legacy_reload=eq==hs\n";
    fs::last_write_time(file, before + std::chrono::seconds(2));
    s.lua->rescan_for_test();s.frame();
    CHECK(s.run("plugin:legacy", "return version==2 and legacy_reload") == "true");
    CHECK(s.run("plugin:legacy", "return pcall(LEGACY_SAVED.register,'RaidWindow','member_actions','stale','Stale',{},function()end)") == "false");
    s.caps["plugin:legacy"] |= plugin::kCapActions;
    CHECK(s.run("", "return tostring(LEGACY_SAVED.click('NoWnd','X'))") == "false");
    s.lua->set_folders({});s.frame();
    CHECK(s.run("", "local result=LEGACY_SAVED.click('NoWnd','X');return tostring(result)") == "nil");
    CHECK(s.run("", "return hs.UI ~= nil and eq == hs") == "true");
}

TEST_CASE("Plugin UI: interface roots written against eq share the API without widening addon views") {
    SandboxUi s;
    s.host.in_game = true;
    s.caps["plugin:legacy"] = plugin::kCapRead | plugin::kCapUi;
    s.add_pack_file("legacy.lua", R"lua(
rootLoaded=eq.InGame() and eq==hs and eq.UI==hs.UI
rootClick=eq.ClickChild
ui.window('Legacy root',function() rootDraw=eq.InGame() end)
)lua");
    s.add_folder("plugin:legacy", "addonLoaded=eq==hs and eq.InGame()\n");
    s.frame();
    CHECK(s.run("", "return rootLoaded and rootDraw and eq.UI.Supports('windows','ClickChild')") == "true");
    CHECK(s.run("", "return eq==hs and eq.UI.windows.ClickChild==hs.ClickChild") == "true");
    CHECK(s.run("", "return tostring(eq.ClickChild('NoWnd','X'))") == "false");
    CHECK(s.run("plugin:legacy", "return addonLoaded and eq==hs and eq.UI==hs.UI") == "true");
    CHECK(s.run("plugin:legacy", "return tostring(eq.ClickChild('NoWnd','X')) .. ':' .. tostring(rootClick('NoWnd','X'))") == "nil:nil");
    CHECK(s.run("plugin:legacy", "eq.ClickChild=function()return true end").find("cannot be changed") != std::string::npos);
    CHECK(s.run("", "return eq.ClickChild==hs.ClickChild") == "true");
    s.lua->add_ui_api([&](lua_State* L) { s.api->register_functions(L); });
    CHECK(s.run("", "return eq==hs and eq.UI==hs.UI and eq.UI.windows.ClickChild==hs.ClickChild") == "true");
    CHECK(s.run("plugin:legacy", "return tostring(eq.UI.windows.ClickChild('NoWnd','X'))") == "nil");
    s.caps["plugin:legacy"] |= plugin::kCapActions;
    CHECK(s.run("plugin:legacy", "return tostring(eq.ClickChild('NoWnd','X'))") == "false");
}

// While the player is asked about a server's compiled plugins, no plugin can draw over the question (ui.Foreground),
// find where it is (ui.FindWindowRect) or put the client's own prompt away (hs.WindowShown("PluginConsent")), and the
// question's windows are raised above the rest at the end of the frame.
TEST_CASE("Plugin UI: the consent question's windows are the pack's alone") {
    SandboxUi s;
    plugin::ConsentPrompt prompt;
    prompt.server = "S";
    prompt.files.push_back({"P", "p.so", 10, {}});
    bool asking = true;
    s.host.plugin_consent = [&]() -> std::optional<plugin::ConsentPrompt> {
        if (asking) return prompt;
        return std::nullopt;
    };
    s.add_pack_file("consent.lua", "ui.window('Server Plugins', function()\n"
                                   "  if not hs.PluginConsent() or NO_PACK_CONSENT then return end\n"
                                   "  hs.WindowShown('PluginConsent')\n"
                                   "  ui.SetNextWindowPos(100, 100)\n"
                                   "  ui.Begin('Consent##q') ui.Text('Allow?') ui.End()\n"
                                   "end)\n");
    s.caps["plugin:X"] = plugin::kCapAll;
    s.add_folder("plugin:X", "ui.window('Cover', function()\n"
                             "  ui.Begin('Cover##c') ui.Text('cover') ui.End()\n"
                             "  ui.Foreground(true) ui.DrawRectFilled(0, 0, 50, 50, 1, 0, 0, 1) ui.Foreground(false)\n"
                             "  local x = ui.FindWindowRect('Consent##q') RECT = x and math.floor(x) or false\n"
                             "  hs.WindowShown('PluginConsent')\n"
                             "  INTO = ui.Begin('Consent##q') ui.Button('inject')\n"
                             "  if ui.BeginChild('c', 10, 10) then ui.Text('inject') end ui.EndChild()\n"
                             "  ui.End()\n"
                             "  PROMPT = ui.Begin(\"Allow these plugins?###hailstone_plugin_consent\") ui.End()\n"
                             "end)\n");
    int foreground = 0;
    auto frame = [&] {
        ImGui::GetIO().DisplaySize = ImVec2(800, 600);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        s.api->begin_frame();
        ImGui::NewFrame();
        s.lua->frame();
        foreground = ImGui::GetForegroundDrawList()->VtxBuffer.Size;
        ui::lua::raise_guarded_windows();
        ImGui::Render();
    };
    ui::lua::set_consent_guard(true);
    frame();
    frame();
    CHECK(s.run("plugin:X", "return RECT") == "false"); // the plugin does not find it
    CHECK(s.run("", "return math.floor(ui.FindWindowRect('Consent##q'))") == "100"); // the pack does
    CHECK(foreground == 0);                                               // nothing drawn over it
    REQUIRE(GImGui->Windows.Size > 0);
    CHECK(std::string(GImGui->Windows.back()->Name) == "Consent##q"); // and it is on top
    CHECK(s.api->replaces("PluginConsent"));                         // (the pack's window stands in)
    // The plugin cannot begin the question's window to add to it, or a child of it, or end it.
    const ImGuiWindow* consent = ImGui::FindWindowByName("Consent##q");
    REQUIRE(consent);
    CHECK(consent->BeginCount == 1); // the pack's Begin alone
    CHECK(s.run("plugin:X", "return tostring(INTO)") == "false");
    CHECK(s.run("plugin:X", "return tostring(PROMPT)") == "false");
    // The client's own prompt neither, before it was ever drawn: the plugin did not create it.
    CHECK_FALSE(ImGui::FindWindowByName(ui::lua::kConsentWindowName));

    // Without the pack's window: the plugin cannot put the client's prompt away.
    s.run("", "NO_PACK_CONSENT = true");
    frame();
    frame();
    frame();
    CHECK_FALSE(s.api->replaces("PluginConsent"));

    // No question: the plugin draws and finds windows as before.
    asking = false;
    ui::lua::set_consent_guard(false);
    s.run("", "NO_PACK_CONSENT = nil");
    frame();
    CHECK(foreground > 0);
    CHECK(s.run("plugin:X", "return RECT") == "100"); // (the window as it was last drawn)
}

// The tables the pack keeps in its globals, seen from a plugin's code: its windows still share the pack's state, but
// what it leaves there runs as the plugin whoever calls it, stops with the plugin, and cannot be used by another.
TEST_CASE("Plugin UI: what a plugin leaves in the pack's shared tables runs as the plugin") {
    SandboxUi s;
    s.add_pack_file("state.lua", "SHARED = { focus = 'pack', list = { 1, 2 }, nested = { deep = { v = 1 } } }\n"
                                 "function Who() return hs.Who() end\n");
    s.lua->add_ui_api([](lua_State* L) {
        lua_pushcfunction(L, [](lua_State* S) -> int {
            const std::string tag(ui::lua::LuaUi::caller_tag());
            lua_pushstring(S, tag.empty() ? "pack" : tag.c_str());
            return 1;
        });
        lua_setfield(L, -2, "Who");
    });
    s.caps["plugin:A"] = plugin::kCapAll;
    s.caps["plugin:B"] = plugin::kCapAll;
    s.add_folder("plugin:A");
    s.add_folder("plugin:B");
    s.frame();

    // Shared state: read and written as before (the pack's focus and palette).
    CHECK(s.run("plugin:A", "local n = 0 for k, v in pairs(SHARED) do n = n + 1 end "
                            "return SHARED.focus .. #SHARED.list .. n .. SHARED.nested.deep.v") == "pack231");
    CHECK(s.run("plugin:A", "SHARED.focus = 'A' table.insert(SHARED.list, 3) SHARED.nested.deep.v = 2 "
                            "local s = 0 for _, v in ipairs(SHARED.list) do s = s + v end return s") == "6");
    CHECK(s.run("", "return SHARED.focus .. #SHARED.list .. SHARED.nested.deep.v") == "A32");
    CHECK(s.run("plugin:A", "return SHARED == SHARED and SHARED.list == SHARED.list") == "true");
    // A C function that reads a table raw gets the table behind a view (the pack's style lists, lib/style.lua).
    ImGui::NewFrame();
    CHECK(s.run("plugin:A", "SHARED.style = { ui.Col.Text, 1, 0, 0, 1 } "
                            "ui.Begin('S') local n = ui.PushStyleColors(SHARED.style) ui.PopStyleColor(n) ui.End() "
                            "return n") == "1");
    CHECK(s.run("", "ui.Begin('S') local n = ui.PushStyleColors(SHARED.style) ui.PopStyleColor(n) ui.End() return n") ==
          "1"); // (the plugin's own table, as the pack sees it)
    ImGui::Render();

    // A function it leaves there, or in a table of its own it leaves there: the plugin's, whoever calls it.
    CHECK(s.run("plugin:A", "SHARED.cb = function() return Who() end "
                            "SHARED.mine = { cb = function() return hs.Who() end, n = 5 } return 1") == "1");
    CHECK(s.run("", "return SHARED.cb() .. ' ' .. SHARED.mine.cb() .. ' ' .. SHARED.mine.n .. ' ' .. Who()") ==
          "plugin:A plugin:A 5 pack");
    // Another plugin cannot use them to act as A, nor change them.
    CHECK(s.run("plugin:B", "return SHARED.cb()").find("plugin:A's") != std::string::npos);
    CHECK(s.run("plugin:B", "return SHARED.mine.cb()").find("plugin:A's") != std::string::npos);
    CHECK(s.run("plugin:B", "SHARED.mine.cb = function() return 'B' end").find("plugin:A's") != std::string::npos);
    // Nor can rawset, setmetatable or a finalizer get past a view.
    CHECK(s.run("plugin:A", "return getmetatable(SHARED)") == "false");
    CHECK(s.run("plugin:A", "setmetatable(SHARED, {})").find("cannot be changed") != std::string::npos);
    CHECK(s.run("plugin:A", "rawset(SHARED.nested, 'cb', function() end)").find("rawset") != std::string::npos);
    // (Its own table, read back through the pack's, is its own again: rawset there still leaves a function the pack
    // reads through the owned view, which runs it as the plugin.)
    CHECK(s.run("plugin:A", "rawset(SHARED.mine, 'raw', function() return hs.Who() end) "
                            "return tostring(rawequal(SHARED.mine, SHARED.mine))") == "true");
    CHECK(s.run("", "return SHARED.mine.raw()") == "plugin:A");
    CHECK(s.run("plugin:A", "setmetatable({}, { __gc = function() end })").find("__gc") != std::string::npos);
    CHECK(s.run("plugin:A", "return getmetatable(setmetatable({}, { __index = { x = 7 } })).__index.x") == "7");

    // The plugin unloads: what it left does nothing.
    s.folders.erase(s.folders.begin()); // (plugin:A)
    s.lua->set_folders(s.folders);
    s.frame();
    CHECK(s.run("", "return tostring((SHARED.cb())) .. ' ' .. tostring((SHARED.mine.cb()))") == "nil nil");
}

// A plugin's ui.GetSetting / SetSetting / DisableWindow are its own: its keys under "plugin.<tag>.", the pack's read
// as a fallback but never written, the pack's test hooks (test.click.<window> clicks a window's children) out of
// reach, and only its own windows turned off.
TEST_CASE("Plugin UI: a plugin's settings and window switches are its own") {
    SandboxUi s;
    s.add_pack_file("w.lua", "ui.window('PackWin', function() PACK_DRAWN = (PACK_DRAWN or 0) + 1 end)\n");
    s.caps["plugin:A"] = plugin::kCapAll;
    s.add_folder("plugin:A", "ui.window('PlugWin', function() PLUG_DRAWN = (PLUG_DRAWN or 0) + 1 end)\n");
    s.add_folder("plugin:B");
    s.frame();
    CHECK(s.run("", "ui.SetSetting('palette', 'dark') ui.SetSetting('test.show', 'X') return 1") == "1");
    // Writes: its own key; the pack's of that name and the test hooks stay as they were.
    CHECK(s.run("plugin:A", "ui.SetSetting('test.click.InventoryWindow', 'IW_Done') ui.SetSetting('palette', 'neon') "
                            "ui.SetSetting('hud.pos.PlugWin', '1,2') return 1") == "1");
    CHECK(s.run("", "return tostring(ui.GetSetting('test.click.InventoryWindow')) .. ' ' .. ui.GetSetting('palette') "
                    ".. ' ' .. tostring(ui.GetSetting('hud.pos.PlugWin'))") == "nil dark nil");
    CHECK(s.run("", "return ui.GetSetting('plugin.plugin:A.hud.pos.PlugWin')") == "1,2");
    // Reads: its own first, then the pack's, never a reserved key or another plugin's.
    CHECK(s.run("plugin:A", "return ui.GetSetting('palette') .. ' ' .. ui.GetSetting('hud.pos.PlugWin')") == "neon 1,2");
    CHECK(s.run("plugin:B", "return ui.GetSetting('palette')") == "dark");
    CHECK(s.run("plugin:B", "return tostring(ui.GetSetting('test.show')) .. ' ' .. "
                            "tostring(ui.GetSetting('plugin.plugin:A.hud.pos.PlugWin')) .. ' ' .. "
                            "ui.GetSetting('hud.pos.PlugWin', 'none')") == "nil nil none");
    // A window: only its own.
    CHECK(s.run("plugin:A", "ui.DisableWindow('PackWin') ui.DisableWindow('PlugWin') return 1") == "1");
    s.run("", "PACK_DRAWN = 0");
    s.run("plugin:A", "PLUG_DRAWN = 0");
    s.frame();
    CHECK(s.run("", "return PACK_DRAWN") == "1");
    CHECK(s.run("plugin:A", "return PLUG_DRAWN") == "0");
    CHECK(s.run("", "return tostring(ui.GetSetting('enabled.PackWin')) .. ' ' .. tostring(ui.GetSetting('enabled.PlugWin'))") ==
          "nil nil");
}

// The settings file is one key=value a line: a value with a line break (or a key with '=' or one) is refused, so a
// plugin cannot write lines that read back as the pack's test hooks, the window switches or another's keys.
TEST_CASE("Plugin UI: ui.SetSetting refuses text that would add lines to the settings file") {
    std::string file;
    {
        SandboxUi s;
        s.caps["plugin:A"] = plugin::kCapAll;
        s.add_folder("plugin:A");
        s.frame();
        CHECK(s.run("plugin:A", "return tostring(ui.SetSetting('x', '1\\ntest.click.RaidWindow=B\\nenabled.Inv=0'))") ==
              "false");
        CHECK(s.run("plugin:A", "return tostring(ui.SetSetting('x', '1\\rtest.show=Raid'))") == "false");
        CHECK(s.run("plugin:A", "return tostring(ui.SetSetting('y\\ntest.show', 'Raid'))") == "false");
        CHECK(s.run("plugin:A", "return tostring(ui.SetSetting('y=test', 'Raid'))") == "false");
        CHECK(s.run("plugin:A", "return tostring(ui.SetSetting('z', 'a\\0b'))") == "false");
        CHECK(s.run("plugin:A", "return tostring(ui.SetSetting('ok', 'a=b'))") == "true"); // '=' in a value reads back
        CHECK(s.run("", "return tostring(ui.SetSetting('pack\\nenabled.Inv', '0'))") == "false");
        CHECK(s.run("plugin:A", "return tostring(ui.GetSetting('x'))") == "nil");
        CHECK(s.run("plugin:A", "return ui.GetSetting('ok')") == "a=b");
        const fs::path path = s.scratch.path() / "ui.txt";
        s.lua.reset(); // saves
        std::ifstream f(path);
        file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    CHECK(file == "plugin.plugin:A.ok=a=b\n");
}

// A window that runs forever or keeps allocating is stopped with an error and turned off, and the UI goes on; a
// plugin's collectgarbage cannot stop or retune the collector.
TEST_CASE("Plugin UI: a runaway window is stopped, the state's memory is capped, collectgarbage is limited") {
    SandboxUi s;
    s.lua->set_limits_for_test(std::size_t{256} << 20, std::chrono::milliseconds(400), std::chrono::milliseconds(100));
    s.add_pack_file("w.lua", "ui.window('PackWin', function() PACK_DRAWN = (PACK_DRAWN or 0) + 1 end)\n");
    s.caps["plugin:A"] = plugin::kCapAll;
    s.add_folder("plugin:A", "ui.window('Spin', function()\n"
                             "  SPUN = (SPUN or 0) + 1\n"
                             "  while true do pcall(function() while true do end end) end\n"
                             "end)\n"
                             "ui.window('Hog', function()\n"
                             "  HOGGED = (HOGGED or 0) + 1\n"
                             "  local t = {} for i = 1, 1e9 do t[i] = string.rep('x', 4096) .. i end\n"
                             "end)\n");
    const auto start = std::chrono::steady_clock::now();
    s.frame();
    s.frame();
    s.frame();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(20));
    CHECK(s.run("plugin:A", "return SPUN .. ' ' .. HOGGED") == "1 1"); // each ran once, and was turned off
    CHECK(s.run("", "return PACK_DRAWN") == "3");                      // the pack's window draws on
    CHECK(ImGui::FindWindowByName("UI errors") != nullptr);
    // The pack's own code has a budget too.
    CHECK(s.run("", "while true do end").find("ran too long") != std::string::npos);
    CHECK(s.run("", "return 1 + 1") == "2"); // and runs again after it

    CHECK(s.run("plugin:A", "return type(collectgarbage('count'))") == "number");
    CHECK(s.run("plugin:A", "return collectgarbage('step') ~= nil") == "true");
    for (const char* opt : {"'stop'", "'restart'", "'collect'", "'incremental', 10, 10", "'generational'"})
        CHECK(s.run("plugin:A", std::string("collectgarbage(") + opt + ")").find("only 'count' and 'step'") !=
              std::string::npos);
    // The plugins' calls above changed nothing: the generational collector runs on its own (lua_host.cpp
    // collect_garbage).
    CHECK(s.run("", "return collectgarbage('isrunning')") == "true");
}

// The player's clipboard (ui.GetClipboardText / SetClipboardText ask the state's "hailstone.clipboard_allowed", as a
// script's state answers in mq/lua_mq.cpp): the pack's own code, and a plugin's only when the app lets its owner
// (an owner with every capability).
TEST_CASE("Plugin UI: the clipboard is the pack's, and a plugin's only with every capability") {
    SandboxUi s;
    s.add_folder("plugin:All");
    s.add_folder("plugin:Some");
    s.frame();
    lua_State* L = s.lua->state();
    lua_getfield(L, LUA_REGISTRYINDEX, "hailstone.clipboard_allowed");
    REQUIRE(lua_isfunction(L, -1));
    lua_setglobal(L, "ASK_CLIPBOARD"); // (the pack's global: the plugins read it through their fallback)
    const std::string ask = "return ASK_CLIPBOARD()";
    CHECK(s.run("", ask) == "true");
    CHECK(s.run("plugin:All", ask) == "false"); // unset: no plugin's
    s.lua->set_clipboard_check([](std::string_view tag) { return tag == "plugin:All"; });
    CHECK(s.run("", ask) == "true");
    CHECK(s.run("plugin:All", ask) == "true");
    CHECK(s.run("plugin:Some", ask) == "false");
}

namespace {
const char kThingMeta = 0; // (the registry key of hs.Thing's metatable, below)
} // namespace

// What crosses from the pack to a plugin comes as a view whatever way it comes: a pack function the plugin calls
// returns views (call_pack), the pack's arguments to the plugin's functions are views (call_as), and getmetatable gives
// the plugin a metatable it did not set only as a view. So a function the plugin plants there runs as the plugin.
TEST_CASE("Plugin UI: what pack functions return and pass to a plugin comes as views") {
    SandboxUi s;
    s.add_pack_file("state.lua", "SHARED = { nested = {}, other = {} }\n"
                                 "function GetShared() return SHARED end\n"
                                 "function Fail() error(SHARED.other) end\n"
                                 "SHARED.get = function() return SHARED.nested end\n"
                                 "function Who() return hs.Who() end\n");
    s.lua->add_ui_api([](lua_State* L) {
        lua_pushcfunction(L, [](lua_State* S) -> int {
            const std::string tag(ui::lua::LuaUi::caller_tag());
            lua_pushstring(S, tag.empty() ? "pack" : tag.c_str());
            return 1;
        });
        lua_setfield(L, -2, "Who");
        // A userdata with a metatable of the client's (as a C type's): a plugin must not change what it does.
        lua_pushcfunction(L, [](lua_State* S) -> int {
            lua_newuserdatauv(S, 1, 0);
            if (lua_rawgetp(S, LUA_REGISTRYINDEX, &kThingMeta) != LUA_TTABLE) {
                lua_pop(S, 1);
                lua_createtable(S, 0, 1);
                lua_newtable(S);
                lua_setfield(S, -2, "__index");
                lua_pushvalue(S, -1);
                lua_rawsetp(S, LUA_REGISTRYINDEX, &kThingMeta);
            }
            lua_setmetatable(S, -2);
            return 1;
        });
        lua_setfield(L, -2, "Thing");
    });
    s.caps["plugin:A"] = plugin::kCapAll;
    s.add_folder("plugin:A");
    s.frame();

    // A pack function's result: a view; a function planted in it runs as the plugin.
    CHECK(s.run("plugin:A", "local t = GetShared() t.cb = function() return Who() end "
                            "SHARED.get().f = function() return Who() end return 1") == "1");
    CHECK(s.run("", "return SHARED.cb() .. ' ' .. SHARED.nested.f()") == "plugin:A plugin:A");
    // What it raises comes the same way.
    CHECK(s.run("plugin:A", "local ok, e = pcall(Fail) e.g = function() return Who() end return tostring(ok)") ==
          "false");
    CHECK(s.run("", "return SHARED.other.g()") == "plugin:A");
    // The same pack function is the same value however the plugin reaches it, and goes back to the pack as itself.
    CHECK(s.run("plugin:A", "return rawequal(GetShared, GetShared) and SHARED.get == SHARED.get") == "true");
    CHECK(s.run("plugin:A", "SHARED.again = GetShared return 1") == "1");
    CHECK(s.run("", "return rawequal(SHARED.again, GetShared)") == "true");

    // The pack's arguments to the plugin's function: views.
    CHECK(s.run("plugin:A", "SHARED.hook = function(t) t.h = function() return Who() end end return 1") == "1");
    CHECK(s.run("", "local t = {} SHARED.hook(t) return t.h()") == "plugin:A");
    // (its own table handed back is its own)
    CHECK(s.run("plugin:A", "MINE = {} SHARED.mine = MINE SHARED.same = function(t) return rawequal(t, MINE) end "
                            "return 1") == "1");
    CHECK(s.run("", "return SHARED.same(SHARED.mine)") == "true");

    // getmetatable: its own metatables as they are, the client's as views.
    CHECK(s.run("plugin:A", "local mt = {} return rawequal(getmetatable(setmetatable({}, mt)), mt)") == "true");
    CHECK(s.run("plugin:A", "getmetatable(hs.Thing()).__index = function() return Who() end return 1") == "1");
    CHECK(s.run("", "return hs.Thing().anything") == "plugin:A");
    CHECK(s.run("plugin:A", "return getmetatable('')") == "false");
}

// A plugin's code allocates up to the limit less the pack's reserve, so what it holds leaves the pack room.
TEST_CASE("Plugin UI: what a plugin holds leaves the pack's code memory") {
    SandboxUi s;
    s.caps["plugin:A"] = plugin::kCapAll;
    s.add_folder("plugin:A");
    s.frame();
    s.lua->set_limits_for_test(std::size_t{64} << 20, std::chrono::milliseconds(5000), std::chrono::milliseconds(5000));
    CHECK(s.run("plugin:A", "HOLD = {} local ok = pcall(function() for i = 1, 1e9 do HOLD[i] = string.rep('x', 1 << 20) "
                            ".. i end end) return tostring(ok) .. ' ' .. tostring(#HOLD > 16)") == "false true");
    CHECK(s.run("", "local t = {} for i = 1, 8 do t[i] = string.rep('y', 1 << 20) .. i end return #t") == "8");
}

// A plugin folder's environment is made before the memory limit holds: made under it, a refused allocation there
// was outside any protected call, and Lua aborted the client.
TEST_CASE("Plugin UI: a plugin's globals are made outside the memory limit") {
    SandboxUi s;
    s.frame();
    lua_State* L = s.lua->state();
    lua_gc(L, LUA_GCCOLLECT);
    const auto used = static_cast<std::size_t>(lua_gc(L, LUA_GCCOUNT)) * 1024 + static_cast<std::size_t>(lua_gc(L, LUA_GCCOUNTB));
    s.lua->set_limits_for_test(used + 512, std::chrono::milliseconds(5000), std::chrono::milliseconds(5000));
    const std::string out = s.run("plugin:Z", "return 1"); // (the first use of plugin:Z's globals)
    CHECK((out == "1" || out.find("memory") != std::string::npos));
    s.lua->push_folder_env_for_test("plugin:Z");
    CHECK(lua_istable(L, -1));
    lua_pop(L, 1);
}

// A plugin's share of the host's C++ state: its settings (count and size) and its windows.
TEST_CASE("Plugin UI: a plugin's settings and windows are held to a share") {
    SandboxUi s;
    s.caps["plugin:A"] = plugin::kCapAll;
    s.add_folder("plugin:A");
    s.frame();
    CHECK(s.run("plugin:A", "local n = 0 for i = 1, 1100 do if ui.SetSetting('k' .. i, 'v') then n = n + 1 end end "
                            "return n") == "1024");
    CHECK(s.run("plugin:A", "return tostring(ui.SetSetting('k1', 'changed'))") == "true"); // a key it has
    CHECK(s.run("plugin:A", "return tostring(ui.SetSetting('big', string.rep('x', 20000)))") == "false");
    CHECK(s.run("", "return tostring(ui.SetSetting('pack.key', 'v'))") == "true"); // the pack's own: not counted
    CHECK(s.run("plugin:A", "local ok, e = pcall(function() for i = 1, 200 do ui.window('W' .. i, function() end) end "
                            "end) return tostring(ok) .. ' ' .. tostring(e):match('may have %d+ windows')") ==
          "false may have 128 windows");
}

// The vendored Lua's long-match check stops a plugin's match at its time limit.
TEST_CASE("Plugin UI: a plugin's runaway string match is stopped") {
    SandboxUi s;
    s.lua->set_limits_for_test(std::size_t{256} << 20, std::chrono::milliseconds(400), std::chrono::milliseconds(100));
    s.caps["plugin:A"] = plugin::kCapAll;
    s.add_folder("plugin:A");
    s.frame();
    const auto start = std::chrono::steady_clock::now();
    CHECK(s.run("plugin:A", "return string.find(('a'):rep(1e5), ('a-'):rep(20) .. 'b')").find("ran too long") !=
          std::string::npos);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}
