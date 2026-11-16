// Real UI owners: retained canvas callbacks follow file generations, capabilities and both destruction orders.
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/lua_ui.h"
#include "game/nameplate_canvas.h"
#include "plugin/policy.h"
#include "plugin/ui_bridge.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"

namespace {
using namespace hailstone;
namespace fs = std::filesystem;
constexpr std::uint32_t kCanvasCaps = plugin::kCapRead | plugin::kCapUi | plugin::kCapExtend;
constexpr const char* kTag = "script:canvas-test";
constexpr const char* kRegister = R"lua(
local n=hs.UI.nameplates
saved_register=n.Register
render=function(f)
 return {{token=f.candidates[1].token,bounds={x=-20,y=-10,w=40,h=10},
   commands={{kind='RectFilled',x0=-20,y0=-10,x1=20,y1=0,color=0x123456}}}}
end
registration=n.Register('main',render)
)lua";

game::NameplateCanvasFrame frame_data() {
    game::NameplateCanvasFrame f;
    f.viewport = {0,0,640,480};
    game::NameplateCanvasCandidate c;
    c.spawn.id = 10;
    c.spawn.incarnation = 17;
    c.x = 200;
    c.y = 100;
    c.depth = 40;
    f.candidates.push_back(c);
    return f;
}

struct CanvasUi {
    test::ScratchDir scratch{"lua_canvas_lifetime"};
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = nullptr;
    std::unique_ptr<game::NameplateCanvasRegistry> registry = std::make_unique<game::NameplateCanvasRegistry>();
    app::UiHost host;
    plugin::UiBridge bridge;
    std::unique_ptr<app::UiApi> api;
    std::unique_ptr<ui::lua::LuaUi> lua;
    std::uint32_t caps = kCanvasCaps;
    std::string tag;
    int native_calls = 0;
    int capability_queries = 0;

    explicit CanvasUi(std::string owner_tag = kTag) : tag(std::move(owner_tag)) {
        context = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
        fs::create_directories(scratch.path() / "pack");
        fs::create_directories(scratch.path() / "plugin");
        host.nameplates = registry.get();
        host.plugin_ui = &bridge;
        host.in_game = host.ui_ready = true;
        host.plugin_caps = [this](std::string_view owner) { ++capability_queries; return owner == tag ? caps : 0u; };
        bridge.add_function("canvas-test","native_chat",[this](const std::vector<plugin::UiValue>&) {
            // A native host table can use its owner's full authority instead of CallerScope.
            if(caps & plugin::kCapChat)++native_calls;
            return plugin::UiValue::boolean(true);
        });
        api = std::make_unique<app::UiApi>(host);
        lua = std::make_unique<ui::lua::LuaUi>(scratch.path() / "pack",scratch.path() / "settings.ini");
        lua->add_ui_api([this](lua_State* L) { api->register_functions(L); });
        lua->set_caller_scope([this](std::string_view owner) -> std::shared_ptr<void> {
            return std::make_shared<plugin::CallerScope>(owner == tag ? caps : 0u,owner);
        });
        write("a.lua",kRegister);
        if(!tag.empty())lua->set_folders({{tag,scratch.path() / "plugin"}});
    }
    ~CanvasUi() {
        lua.reset(); // callback references are retired while both the registry and API still exist
        api.reset();
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }
    void write(const std::string& name,const std::string& source) {
        const auto file = scratch.path() / (tag.empty() ? "pack" : "plugin") / name;
        std::error_code ec;
        const auto before = fs::last_write_time(file,ec);
        std::ofstream(file,std::ios::binary) << source;
        if (!ec) fs::last_write_time(file,before + std::chrono::seconds(2));
    }
    void draw() {
        ImGui::SetCurrentContext(context);
        ImGui::GetIO().DisplaySize = ImVec2(800,600);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        api->begin_frame();
        ImGui::NewFrame();
        lua->frame();
        ImGui::Render();
    }
    std::string run(const std::string& source) { return lua->run_for_test(tag,source); }
};
}

TEST_CASE("Lua nameplate lifetime: UI source reload retires callbacks and stale registration factories") {
    CanvasUi ui;
    ui.write("z.lua",R"lua(
ui.window('Canvas controls',function()
 if try_saved then
   try_saved=false
   saved_ok=pcall(saved_register,'resurrected',render)
 end
end)
)lua");
    ui.draw();
    auto f = frame_data();
    auto first = ui.registry->frame(f);
    REQUIRE(first.size() == 1);
    const auto old = first[0].owner;
    ui.write("a.lua","reloaded=true\n");
    ui.lua->rescan_for_test();
    ui.draw();
    CHECK_FALSE(ui.registry->owner_valid(old));
    CHECK(ui.registry->frame(f).empty());
    CHECK(ui.run("try_saved=true; return true") == "true");
    ui.draw();
    CHECK(ui.run("return saved_ok") == "false");
    CHECK(ui.registry->frame(f).empty());
    CHECK(ui.run("local h=hs.UI.nameplates.Register('current',render); return type(h)") == "string");
    CHECK(ui.registry->frame(f).size() == 1); // a current owner may still register normally
}

TEST_CASE("Lua nameplate lifetime: UI permissions and folder removal invalidate retained submissions") {
    CanvasUi ui;
    ui.draw();
    auto f = frame_data();
    auto first = ui.registry->frame(f);
    REQUIRE(first.size() == 1);
    ui.caps = plugin::kCapRead;
    CHECK_FALSE(ui.registry->owner_valid(first[0].owner));
    CHECK(ui.registry->frame(f).empty());
    ui.lua->set_folders({});
    ui.caps = kCanvasCaps;
    CHECK_FALSE(ui.registry->owner_valid(first[0].owner)); // reviving permissions cannot revive an unloaded folder
    ui.lua.reset();
    CHECK(ui.registry->frame(f).empty());
    CHECK(ui.registry->empty());
}

TEST_CASE("Lua nameplate lifetime: destroying the registry before the UI VM is safe and reports unavailable") {
    CanvasUi ui;
    ui.draw();
    auto f = frame_data();
    REQUIRE(ui.registry->frame(f).size() == 1);
    ui.registry.reset();
    CHECK(ui.run("local h,why=hs.UI.nameplates.Register('late',render); return h==nil and type(why)=='string'") == "true");
    ui.lua.reset();
}

TEST_CASE("Lua nameplate lifetime: native callbacks cannot borrow authority beyond their registration ceiling") {
    CanvasUi ui("plugin:canvas-test");
    ui.caps = kCanvasCaps | plugin::kCapChat;
    ui.write("a.lua","loaded=true\n");
    ui.draw();
    {
        plugin::CallerScope narrowed(kCanvasCaps,"plugin:canvas-test");
        CHECK(ui.run("return pcall(hs.UI.nameplates.Register,'narrow',function() hs.UI.plugins.Get('canvas-test').native_chat() end)") == "false");
    }
    auto f = frame_data();
    CHECK(ui.registry->frame(f).empty());
    CHECK(ui.native_calls == 0);
    CHECK(ui.run(std::string(kRegister) + R"lua(
local original=render
registration=hs.UI.nameplates.Register('main',function(f)
 hs.UI.plugins.Get('canvas-test').native_chat()
 return original(f)
end)
return type(registration)
)lua") == "string");
    const auto first = ui.registry->frame(f);
    REQUIRE(first.size() == 1);
    CHECK(ui.native_calls == 1);
    {
        plugin::CallerScope narrowed(kCanvasCaps,"");
        CHECK(ui.registry->frame(f).empty());
    }
    CHECK(ui.native_calls == 1);
    CHECK(ui.registry->owner_valid(first[0].owner));
    CHECK(ui.registry->frame(f).size() == 1);
    CHECK(ui.native_calls == 2);
    ui.caps |= plugin::kCapCommands;
    CHECK_FALSE(ui.registry->owner_valid(first[0].owner));
    CHECK(ui.registry->frame(f).empty());
    CHECK(ui.native_calls == 2);
}

TEST_CASE("Lua nameplate lifetime: named native callers cannot capture or enter the root UI role") {
    CanvasUi ui("");
    ui.draw();
    auto f = frame_data();
    const auto first = ui.registry->frame(f);
    REQUIRE(first.size() == 1);
    {
        plugin::CallerScope native(plugin::kCapAll,"foreign-native");
        CHECK(ui.run("return pcall(hs.UI.nameplates.Register,'foreign',render)") == "false");
        CHECK(ui.registry->frame(f).empty());
    }
    CHECK(ui.registry->owner_valid(first[0].owner));
    CHECK(ui.registry->frame(f).size() == 1);
    CHECK(ui.run("return type(hs.UI.nameplates.Register('root',render))") == "string");
}

TEST_CASE("Lua nameplate lifetime: destroying UiApi invalidates callbacks while the VM remains alive") {
    CanvasUi ui;
    ui.draw();
    auto f = frame_data();
    const auto first = ui.registry->frame(f);
    REQUIRE(first.size() == 1);
    ui.api.reset();
    const auto queries = ui.capability_queries;
    CHECK_FALSE(ui.registry->owner_valid(first[0].owner));
    CHECK(ui.registry->frame(f).empty());
    CHECK(ui.capability_queries == queries); // never call the retired host's permission query
    CHECK(ui.lua->run_for_test(kTag,"return 19") == "19");
    ui.lua.reset();
    CHECK(ui.registry->empty());
}

TEST_CASE("Lua nameplate lifetime: replacing UiApi in a retained VM retires old factories and admits the new host") {
    CanvasUi ui;
    ui.draw();
    auto f = frame_data();
    const auto first = ui.registry->frame(f);
    REQUIRE(first.size() == 1);
    ui.api.reset();
    ui.api = std::make_unique<app::UiApi>(ui.host);
    ui.lua->add_ui_api([&](lua_State* L) { ui.api->register_functions(L); });
    CHECK_FALSE(ui.registry->owner_valid(first[0].owner));
    CHECK(ui.run("local h,why=saved_register('old',render); return h==nil and type(why)=='string'") == "true");
    CHECK(ui.run("return type(hs.UI.nameplates.Register('new',render))") == "string");
    const auto current = ui.registry->frame(f);
    REQUIRE(current.size() == 1);
    CHECK(current[0].owner != first[0].owner);
    CHECK(ui.registry->owner_valid(current[0].owner));
}
