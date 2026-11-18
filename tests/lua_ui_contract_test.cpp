// The versioned contract is a view of the existing guarded UI closures, not a second set of bindings.
#include <doctest/doctest.h>

#include <set>
#include <string>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_caps.h"
#include "app/lua_ui_api.h"
#include "plugin/policy.h"
#include "ui/dev_screens.h"

using namespace hailstone;

namespace {

struct ContractUi {
    ImGuiContext* context = ImGui::CreateContext();
    app::UiHost host;
    app::UiApi api{host};
    lua_State* L = luaL_newstate();

    ContractUi() {
        ImGui::GetIO().IniFilename = nullptr;
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
    }
    ~ContractUi() {
        lua_close(L);
        ImGui::DestroyContext(context);
    }
    std::string run(const char* code) {
        const int top = lua_gettop(L);
        std::string result;
        if (luaL_loadstring(L, code) != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK)
            result = std::string("error: ") + lua_tostring(L, -1);
        else if (lua_isboolean(L, -1)) result = lua_toboolean(L, -1) ? "true" : "false";
        else if (lua_isnil(L, -1)) result = "nil";
        else if (lua_isstring(L, -1)) result = lua_tostring(L, -1);
        else result = luaL_typename(L, -1);
        lua_settop(L, top);
        return result;
    }
};

} // namespace

TEST_CASE("UI contract: every registered function and constant is the original guarded value") {
    ContractUi ui;
    std::set<std::string_view> binding;
    std::set<std::pair<std::string_view, std::string_view>> canonical;
    for (const auto& entry : app::ui_api_entries()) {
        CAPTURE(entry.binding);
        CHECK(binding.insert(entry.binding).second);
        CHECK(canonical.emplace(entry.module, entry.member).second);
        if (entry.kind == app::UiApiKind::Function) {
            CHECK(app::ui_call_caps(entry.binding) == entry.required_caps);
            CHECK(app::ui_call_caps(std::string("UI.") + std::string(entry.module) + "." +
                                    std::string(entry.member)) == entry.required_caps);
        }
    }
    CHECK(ui.run(R"(
        local api, n, last = hs.UI, 0, ''
        for _, mod in ipairs(api.Describe().modules) do
          assert(mod.name > last) last = mod.name
          local previous = ''
          for _, method in ipairs(mod.methods) do
            assert(method.name > previous) previous = method.name
            local original = hs
            for part in method.binding:gmatch('[^.]+') do original = original[part] end
            assert(rawequal(original, api[mod.name][method.name]), method.binding)
            assert(api.Supports(mod.name, method.name))
            assert(type(original) == (method.kind == 'function' and 'function' or 'number'))
            n = n + 1
          end
        end
        return n
    )") == std::to_string(app::ui_api_entries().size()));
    CHECK(lua_gettop(ui.L) == 0);
    CHECK(ui.run("return hs.UI.windows.Show == hs.ShowWindow and hs.UI.tasks.Open == hs.OpenTask") == "true");
    CHECK(ui.run("return hs.UI.inventory.FirstBagSlot == hs.FirstBagSlot") == "true");
}

TEST_CASE("UI interfaces: installed records and actual loading status are independent read-only snapshots") {
    ContractUi ui;
    hailstone::ui::UiPack modern;
    modern.id = "modern"; modern.name = "Modern";
    hailstone::ui::UiPack unavailable;
    unavailable.id = "addon.pack.future"; unavailable.name = "Future"; unavailable.description = "Installed interface";
    unavailable.source = "addon"; unavailable.addon = "pack"; unavailable.runtime = "future-runtime";
    unavailable.available = false; unavailable.unavailable_reason = "Unsupported interface runtime: future-runtime";
    unavailable.base_ids = {"modern"};
    hailstone::ui::InterfaceStatus status;
    status.selected = unavailable; status.loaded = modern; status.state = "fallback";
    status.diagnostics = {unavailable.unavailable_reason}; status.script_count = status.loaded_script_count = 2;
    status.window_count = 3;
    ui.host.interface_list = [&] { return std::vector<hailstone::ui::UiPack>{modern, unavailable}; };
    ui.host.interface_status = [&] { return status; };
    plugin::CallerScope no_caps(0, "plugin:interface-status-test");
    CHECK(ui.run(R"lua(
        local api = hs.UI.interfaces
        local all, s = api.List(), api.Status()
        assert(#all == 2 and all[2].id == 'addon.pack.future' and not all[2].available)
        assert(all[2].source == 'addon' and all[2].addon == 'pack' and all[2].runtime == 'future-runtime')
        assert(all[2].description == 'Installed interface' and all[2].bases[1] == 'modern')
        assert(s.selected.id == all[2].id and s.loaded.id == 'modern' and s.state == 'fallback')
        assert(s.scriptCount == 2 and s.loadedScriptCount == 2 and s.windowCount == 3 and s.errorCount == 0)
        assert(#s.diagnostics == 1)
        all[2].available=true; s.selected.id='changed'; s.diagnostics[1]='changed'
        return not api.List()[2].available and api.Status().selected.id == 'addon.pack.future'
            and api.Status().diagnostics[1] == 'Unsupported interface runtime: future-runtime'
    )lua") == "true");
    status.state = "ready"; status.selected = modern; status.diagnostics.clear();
    CHECK(ui.run("return hs.UI.interfaces.Status().state") == "ready");
    CHECK(app::ui_call_caps("InterfaceList") == 0);
    CHECK(app::ui_call_caps("InterfaceStatus") == 0);
}

TEST_CASE("UI contract: discovery reports client structure independently of game and permission state") {
    ContractUi ui;
    CHECK(ui.run("return hs.UI.version.major .. '.' .. hs.UI.version.minor") == "1.7");
    CHECK(ui.run("return hs.UI.session.InGame()") == "false");
    CHECK(ui.run("return hs.UI.Supports('session', 'InGame') and hs.UI.Supports('chat')") == "true");
    CHECK(ui.run("return hs.UI.Supports('missing') or hs.UI.Supports('chat', 'Missing')") == "false");
    CHECK(ui.run("return hs.UI.Supports('chat\\0extra', 'Windows')") == "false");
    CHECK(ui.run("return hs.UI.Describe('missing')") == "nil");
    CHECK(ui.run("return hs.UI.Describe('chat', 'missing')") == "nil");
    CHECK(ui.run("return hs.UI.Describe('chat', 'Windows').binding") == "ChatWindows");
    CHECK(ui.run("return hs.UI.Supports('interfaces', 'LoadBase')") == "true");
    CHECK(ui.run("return hs.UI.Supports('interfaces', 'LoadData')") == "true");
    CHECK(ui.run("return hs.UI.interfaces.LoadBase('lib/theme.lua')").find("requires the client UI state") != std::string::npos);
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/plan.lua')").find("requires the client UI state") != std::string::npos);
    CHECK(ui.run("return hs.UI.Describe('windows', 'ClickChild').requiredCaps") ==
          std::to_string(plugin::kCapActions));
    CHECK(ui.run("return hs.UI.Describe('inventory', 'CursorSlot').kind") == "constant");
    CHECK(ui.run("local d = hs.UI.Describe() d.modules[1].name='changed' d.version.major=999 "
                 "return hs.UI.Describe().version.major == 1 and hs.UI.Describe().modules[1].name ~= 'changed'") == "true");
    CHECK(ui.run("return hs.UI.Supports()").starts_with("error:"));
}

TEST_CASE("UI contract: unlisted, missing and mistyped registered fields fail validation") {
    for (const char* change : {"hs.NewUnlistedCall = function() end", "hs.ClickChild = nil",
                               "hs.FirstWornSlot = false", "hs.gamepad.unlisted = function() end"}) {
        CAPTURE(change);
        ContractUi ui;
        REQUIRE_FALSE(ui.run(change).starts_with("error:"));
        lua_pushcfunction(ui.L, [](lua_State* L) -> int {
            lua_getglobal(L, "hs");
            lua_pushnil(L);
            lua_setfield(L, -2, "UI");
            app::register_ui_api(L);
            return 0;
        });
        REQUIRE(lua_pcall(ui.L, 0, 0, 0) != LUA_OK);
        CHECK(std::string(lua_tostring(ui.L, -1)).find("UI contract:") != std::string::npos);
        lua_pop(ui.L, 1);
    }
}

TEST_CASE("UI contract: namespaced mutations retain same-frame list invalidation") {
    ContractUi ui;
    ui::ServerSelectView view;
    ui::ServerEntry first;
    first.name = "Alpha";
    view.servers.push_back(first);
    ui.host.server_select = &view;
    ui.api.begin_frame();
    CHECK(ui.run("return (hs.UI.windows.ListInfo('serverselect', 'ServerList'))") == "1");
    ui::ServerEntry second;
    second.name = "Beta";
    view.servers.push_back(second);
    CHECK(ui.run("return (hs.ListInfo('serverselect', 'ServerList'))") == "1");
    CHECK(ui.run("return hs.UI.windows.ListClick('serverselect', 'ServerList', 0, 'left')") == "true");
    CHECK(ui.run("return (hs.ListInfo('serverselect', 'ServerList'))") == "2");
    CHECK(ui.run("return hs.UI.windows.ListRow('serverselect', 'ServerList', 1)[1]") == "Beta");
}

TEST_CASE("UI contract: repeated registration republishes guarded aliases and retained readers share the cache") {
    ContractUi ui;
    ui::ServerSelectView view;
    ui::ServerEntry entry;
    entry.name = "Alpha";
    view.servers.push_back(entry);
    ui.host.server_select = &view;
    ui.api.begin_frame();
    CHECK(ui.run("oldUI=hs.UI oldRead=hs.UI.windows.ListInfo "
                 "return (oldRead('serverselect','ServerList'))") == "1");
    entry.name = "Beta";
    view.servers.push_back(entry);
    lua_getglobal(ui.L, "hs");
    ui.api.register_functions(ui.L);
    lua_pop(ui.L, 1);
    CHECK(ui.run("return oldUI ~= hs.UI and hs.UI.windows.ClickChild == hs.ClickChild and "
                 "hs.UI.gamepad.SetMode == hs.gamepad.set_mode") == "true");
    CHECK(ui.run("return (oldRead('serverselect','ServerList'))") == "1");
    CHECK(ui.run("return hs.UI.windows.ListClick('serverselect','ServerList',0,'left')") == "true");
    CHECK(ui.run("return (oldRead('serverselect','ServerList'))") == "2");
    CHECK(ui.run("return oldUI.Supports('chat','Windows') and hs.UI.Supports('chat','Windows')") == "true");
}
