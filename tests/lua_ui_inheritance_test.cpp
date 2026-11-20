// A composed UI inherits windows/helpers without gaining arbitrary file access or another owner's authority.
#include <doctest/doctest.h>

#include <filesystem>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "plugin/policy.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"
#include "ui/ui_textures.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct ComposedUi {
    test::ScratchDir scratch{"lua_ui_inheritance"};
    fs::path root = scratch.path() / "ui";
    ImGuiContext* context = ImGui::CreateContext();
    app::UiHost host;
    ui::lua::LuaUi::DataRoots data_roots;
    std::unique_ptr<app::UiApi> api;
    std::unique_ptr<ui::lua::LuaUi> lua;

    ComposedUi() {
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        for (const char* name : {"base", "middle", "leaf"}) fs::create_directories(root / name / "lib");
    }
    ~ComposedUi() {
        lua.reset();
        api.reset();
        ImGui::DestroyContext(context);
    }
    void put(const fs::path& path, const char* source) {
        fs::create_directories(path.parent_path());
        std::ofstream(path) << source;
    }
    void start(bool real_api = false) {
        lua = std::make_unique<ui::lua::LuaUi>(root / "leaf", scratch.path() / "leaf-settings.txt",
                                              std::vector<fs::path>{root / "middle", root / "base"}, data_roots);
        if (real_api) {
            api = std::make_unique<app::UiApi>(host);
            lua->add_ui_api([&](lua_State* L) { api->register_functions(L); });
        }
        lua->add_ui_api([](lua_State* L) {
            lua_pushcfunction(L, [](lua_State* state) -> int {
                const auto tag = ui::lua::LuaUi::caller_tag();
                lua_pushlstring(state, tag.data(), tag.size());
                return 1;
            });
            lua_setfield(L, -2, "Who");
            lua_pushcfunction(L, [](lua_State* state) -> int {
                std::size_t size = 0;
                const char* name = luaL_checklstring(state, 1, &size);
                const auto path = ui::lua::interface_image_path(state, std::string_view(name, size));
                if (!path) { lua_pushnil(state); return 1; }
                const auto text = path->generic_string();
                lua_pushlstring(state, text.data(), text.size());
                return 1;
            });
            lua_setfield(L, -2, "ResolveImage");
        });
    }
    void frame(bool rescan = false, bool draw = true) {
        if (rescan) lua->rescan_for_test();
        ImGui::GetIO().DisplaySize = ImVec2(800, 600);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        lua->frame(draw);
        ImGui::Render();
    }
    std::string run(const char* source, const char* tag = "") { return lua->run_for_test(tag, source); }
};

} // namespace

TEST_CASE("UI interface images: selected and approved bases resolve independently of local presentation data") {
    ComposedUi ui;
    ui.put(ui.root / "leaf/assets/Atlas.PNG", "selected raster fixture");
    ui.put(ui.root / "middle/assets/Atlas.PNG", "near base raster fixture");
    ui.put(ui.root / "base/assets/Atlas.PNG", "far base raster fixture");
    ui.put(ui.root / "base/assets/shared.png", "shared raster fixture");
    const auto data = ui.scratch.path() / "ui-data";
    ui.data_roots = {data, {"leaf", "base"}};
    ui.put(data / "leaf/assets/local-only.png", "local presentation data fixture");
    ui.put(ui.root / "unapproved/assets/secret.png", "unapproved sibling fixture");
    ui.start(true);
    ui.frame();
    const auto resolve = [&](const char* path) { return ui::lua::interface_image_path(ui.lua->state(), path); };
    CHECK(resolve("assets/Atlas.PNG") == fs::weakly_canonical(ui.root / "leaf/assets/Atlas.PNG"));
    CHECK(resolve("assets\\Atlas.PNG") == resolve("assets/Atlas.PNG"));
    CHECK(resolve("assets/shared.png") == fs::weakly_canonical(ui.root / "base/assets/shared.png"));
    REQUIRE(fs::remove(ui.root / "leaf/assets/Atlas.PNG"));
    CHECK(resolve("assets/Atlas.PNG") == fs::weakly_canonical(ui.root / "middle/assets/Atlas.PNG"));
    REQUIRE(fs::remove(ui.root / "middle/assets/Atlas.PNG"));
    CHECK(resolve("assets/Atlas.PNG") == fs::weakly_canonical(ui.root / "base/assets/Atlas.PNG"));
    for (const auto* path : {"assets/local-only.png", "assets/secret.png", "../base/assets/shared.png",
                             "assets/../assets/shared.png", "/assets/shared.png", "C:/assets/shared.png",
                             "assets/shared.png/", "assets//shared.png", "assets./shared.png", "assets/shared.txt"}) {
        CAPTURE(path);
        CHECK_FALSE(resolve(path));
    }
    CHECK_FALSE(ui::lua::interface_image_path(ui.lua->state(), std::string("assets/shared.png\0", 18)));
    // The public binding keeps its single-nil result with no GPU/artwork provider.
    CHECK(ui.run("return select('#',hs.UI.assets.InterfaceImage('assets/shared.png'))") == "1");
    CHECK(ui.run("return hs.UI.assets.InterfaceImage('assets/shared.png')") == "nil");
}

TEST_CASE("UI interface images: plugin ownership survives saved closures and ends with folder removal") {
    ComposedUi ui;
    const auto plugin = ui.scratch.path() / "plugin", other = ui.scratch.path() / "other";
    ui.put(ui.root / "leaf/assets/owned.png", "selected raster fixture");
    ui.put(ui.root / "base/assets/shared.png", "base raster fixture");
    ui.put(plugin / "assets/owned.png", "plugin raster fixture");
    ui.put(other / "assets/private.png", "other plugin fixture");
    ui.put(plugin / "a.lua", "OWNER_IMAGE=hs.ResolveImage('assets/owned.png') "
        "SHARED_IMAGE=hs.ResolveImage('assets/shared.png') PRIVATE_IMAGE=hs.ResolveImage('assets/private.png') "
        "SAVED_IMAGE=hs.ResolveImage");
    ui.start();
    ui.lua->set_folders({{"plugin:Image", plugin}, {"plugin:Other", other}});
    ui.frame();
    CHECK(ui.run("return OWNER_IMAGE", "plugin:Image") == fs::weakly_canonical(plugin / "assets/owned.png").generic_string());
    CHECK(ui.run("return SHARED_IMAGE", "plugin:Image") == fs::weakly_canonical(ui.root / "base/assets/shared.png").generic_string());
    CHECK(ui.run("return PRIVATE_IMAGE", "plugin:Image") == "nil");
    CHECK(ui.run("return hs.ResolveImage('../other/assets/private.png')", "plugin:Image") == "nil");
    CHECK(ui.run("return hs.ResolveImage('assets/owned.png')") == fs::weakly_canonical(ui.root / "leaf/assets/owned.png").generic_string());
    ui.lua->push_folder_env_for_test("plugin:Image");
    lua_getfield(ui.lua->state(), -1, "SAVED_IMAGE");
    lua_setglobal(ui.lua->state(), "PLUGIN_IMAGE");
    lua_pop(ui.lua->state(), 1);
    CHECK(ui.run("return PLUGIN_IMAGE('assets/owned.png')") == fs::weakly_canonical(plugin / "assets/owned.png").generic_string());
    ui.lua->set_folders({});
    ui.frame();
    CHECK(ui.run("return PLUGIN_IMAGE('assets/owned.png')") == "nil");
    CHECK(ui.run("return PLUGIN_IMAGE('assets/shared.png')") == "nil");
    CHECK(ui.run("return hs.ResolveImage('assets/owned.png')") == fs::weakly_canonical(ui.root / "leaf/assets/owned.png").generic_string());
}

TEST_CASE("UI interface images: symlinks cannot reach outside approved roots") {
    ComposedUi ui;
    ui.put(ui.root / "base/assets/good.png", "base raster fixture");
    const auto outside = ui.scratch.path() / "outside/secret.png";
    ui.put(outside, "outside raster fixture");
    fs::create_directories(ui.root / "leaf/assets");
    std::error_code ec;
    fs::create_symlink(outside, ui.root / "leaf/assets/escape.png", ec);
    if (ec) { MESSAGE("File link creation is unavailable on this host"); return; }
    ui.start();
    ui.frame();
    CHECK_FALSE(ui::lua::interface_image_path(ui.lua->state(), "assets/escape.png"));
    fs::create_directory_symlink(outside.parent_path(), ui.root / "leaf/linked", ec);
    REQUIRE_FALSE(ec);
    CHECK_FALSE(ui::lua::interface_image_path(ui.lua->state(), "linked/secret.png"));
    CHECK(ui::lua::interface_image_path(ui.lua->state(), "assets/good.png") ==
          fs::weakly_canonical(ui.root / "base/assets/good.png"));
}

TEST_CASE("UI interface images: empty and oversized files are refused before texture allocation") {
    ComposedUi ui;
    const auto file = ui.root / "leaf/assets/bounded.png";
    ui.put(file, "");
    ui.start();
    ui.frame();
    CHECK_FALSE(ui::lua::interface_image_path(ui.lua->state(), "assets/bounded.png"));
    // Resize exercises the byte limit without allocating a 64 MiB fixture in memory.
    fs::resize_file(file, ui::UiTextures::kMaxInterfaceImageBytes);
    CHECK(ui::lua::interface_image_path(ui.lua->state(), "assets/bounded.png") == fs::weakly_canonical(file));
    fs::resize_file(file, ui::UiTextures::kMaxInterfaceImageBytes + 1);
    CHECK_FALSE(ui::lua::interface_image_path(ui.lua->state(), "assets/bounded.png"));
    REQUIRE(fs::remove(file));
    CHECK_FALSE(ui::lua::interface_image_path(ui.lua->state(), "assets/bounded.png"));
}

TEST_CASE("UI hidden frames: reload maintenance and garbage collection continue without any window callbacks") {
    ComposedUi ui;
    const auto script = ui.root / "leaf/a.lua";
    const auto plugin = ui.scratch.path() / "plugin";
    ui.put(script, "LOADS=(LOADS or 0)+1 ui.SetSetting('palette','kept') "
        "do local held={string.rep('x',256*1024)} GARBAGE=setmetatable({held},{__mode='v'}) end "
        "ui.window('HiddenPack',function() DRAWS=(DRAWS or 0)+1 ui.Begin('Hidden pack body') ui.End() end)");
    ui.put(ui.root / "base/broken.lua", "error('hidden source error')");
    ui.put(plugin / "plugin.lua", "OWNER=hs.Who() ui.window('HiddenPlugin',function() "
        "DRAWS=(DRAWS or 0)+1 ui.Begin('Hidden plugin body') ui.End() end)");
    ui.start();
    ui.lua->set_folders({{"plugin:Hidden", plugin}});
    ui.lua->set_show_control(true);
    const auto maintenance = std::make_shared<unsigned>(0);
    REQUIRE(ui::lua::LuaUi::set_callback_maintenance(ui.lua->state(), "fixture.hidden", [maintenance](lua_State*) { ++*maintenance; }));
    ui.frame(false, false);
    CHECK(ui.run("return LOADS") == "1");
    CHECK(ui.run("return DRAWS") == "nil");
    CHECK(ui.run("return OWNER", "plugin:Hidden") == "plugin:Hidden");
    CHECK(ui.run("return DRAWS", "plugin:Hidden") == "nil");
    CHECK(ui.run("return GARBAGE[1]==nil") == "true"); // the host's initial GC ran, not a test-forced collection
    CHECK(ui.lua->setting("palette") == "kept");
    CHECK(ui.lua->load_status().window_count == 1); // plugin windows are separate from pack status
    CHECK(ui.lua->load_status().error_count == 1);
    CHECK_FALSE(ImGui::FindWindowByName("Hidden pack body"));
    CHECK_FALSE(ImGui::FindWindowByName("Hidden plugin body"));
    CHECK_FALSE(ImGui::FindWindowByName("Lua UI"));
    CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    const auto before = *maintenance;
    const auto stamp = fs::last_write_time(script);
    ui.put(script, "LOADS=(LOADS or 0)+1 ui.window('HiddenPack',function() "
        "DRAWS=(DRAWS or 0)+1 ui.Begin('Hidden pack body') ui.End() end)");
    fs::last_write_time(script, stamp + std::chrono::seconds(2));
    ui.frame(true, false);
    CHECK(*maintenance > before);
    CHECK(ui.run("return LOADS") == "2");
    CHECK(ui.run("return DRAWS") == "nil");
    CHECK(ui.run("return DRAWS", "plugin:Hidden") == "nil");
    CHECK(ui.lua->setting("palette") == "kept");
    ui.frame();
    CHECK(ui.run("return DRAWS") == "1");
    CHECK(ui.run("return DRAWS", "plugin:Hidden") == "1");
    CHECK(ImGui::FindWindowByName("Hidden pack body"));
    CHECK(ImGui::FindWindowByName("Hidden plugin body"));
    ui.frame(false, false);
    CHECK(ui.run("return DRAWS") == "1");
    CHECK(ui.run("return DRAWS", "plugin:Hidden") == "1");
}

TEST_CASE("UI hidden frames: removed sources release owned extension captures without invoking them") {
    ComposedUi ui;
    const auto script = ui.root / "leaf/a.lua";
    ui.put(script, "local held={string.rep('x',256*1024)} WEAK=setmetatable({held},{__mode='v'}) "
        "HANDLE=hs.UI.extensions.RegisterAction('MapViewWnd','controls','capture','Capture',{},function() "
        "ACTED=true return {ok=true,message=held[1]} end) "
        "ui.window('NeverPainted',function() DRAWN=true end)");
    ui.start(true);
    ui.frame(false, false);
    REQUIRE(ui.run("return type(HANDLE)") == "string");
    CHECK(ui.run("return WEAK[1]~=nil") == "true");
    CHECK(ui.run("return DRAWN") == "nil");
    REQUIRE(fs::remove(script));
    ui.frame(true, false);
    CHECK(ui.lua->load_status().window_count == 0);
    CHECK(ui.run("collectgarbage('collect') collectgarbage('collect') return WEAK[1]==nil and ACTED==nil and DRAWN==nil") == "true");
    CHECK(ui.run("return hs.UI.extensions.IsRegistered(HANDLE)") == "false");
}

TEST_CASE("UI data imports: selected and base modules stay separate from manager-owned code") {
    ComposedUi ui;
    const auto data = ui.scratch.path() / "ui-data";
    ui.data_roots = {data, {"leaf", "middle", "base"}};
    ui.put(data / "leaf/lib/value.lua", "return 'selected',nil,3");
    ui.put(data / "middle/lib/value.lua", "return 'middle'");
    ui.put(data / "base/lib/fallback.lua", "return 'base'");
    ui.put(ui.root / "leaf/lib/value.lua", "return 'manager code'");
    ui.put(ui.root / "leaf/lib/code-only.lua", "return 'code only'");
    ui.start(true);
    ui.frame();
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/value.lua')") == "selected");
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/value.lua',true)") == "middle");
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/fallback.lua')") == "base");
    CHECK(ui.run("return select('#',hs.UI.interfaces.LoadData('lib/value.lua'))") == "3");
    CHECK(ui.run("local a,b,c=hs.UI.interfaces.LoadData('lib/value.lua') return a=='selected' and b==nil and c==3") == "true");
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/code-only.lua')").starts_with("error:"));
    ui.put(ui.root / "leaf/lib/value.lua", "return 'updated code'");
    fs::remove(ui.root / "leaf/lib/value.lua");
    CHECK(fs::is_regular_file(data / "leaf/lib/value.lua"));
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/value.lua')") == "selected");
}

TEST_CASE("UI data imports: shared modules retain plugin environments permissions and lifetime") {
    ComposedUi ui;
    const auto data = ui.scratch.path() / "ui-data";
    ui.data_roots = {data, {"leaf", "base"}};
    ui.put(data / "leaf/lib/owner.lua", "return 'selected'");
    ui.put(data / "base/lib/owner.lua", "DATA_OWNER=hs.Who() return DATA_OWNER,hs.UI.windows.ClickChild('OptionsWindow','missing','left')");
    const auto folder = ui.scratch.path() / "plugin";
    ui.put(folder / "lib/owner.lua", "return 'plugin folder'");
    ui.put(folder / "a.lua", "OWNER,CLICKED=hs.UI.interfaces.LoadData('lib/owner.lua',true) SAVED_DATA=hs.UI.interfaces.LoadData");
    ui.start(true);
    ui.lua->set_caller_scope([](std::string_view tag) -> std::shared_ptr<void> {
        return std::make_shared<plugin::CallerScope>(tag.empty() ? std::uint32_t{plugin::kCapAll} : 0u, tag);
    });
    ui.lua->set_folders({{"plugin:Data", folder}});
    ui.frame();
    CHECK(ui.run("return OWNER", "plugin:Data") == "plugin:Data");
    CHECK(ui.run("return CLICKED", "plugin:Data") == "nil");
    CHECK(ui.run("return DATA_OWNER") == "nil");
    ui.lua->push_folder_env_for_test("plugin:Data");
    lua_getfield(ui.lua->state(), -1, "SAVED_DATA");
    lua_setglobal(ui.lua->state(), "PLUGIN_DATA");
    lua_pop(ui.lua->state(), 1);
    CHECK(ui.run("return PLUGIN_DATA('lib/owner.lua',true)") == "plugin:Data");
    CHECK(ui.run("return DATA_OWNER") == "nil");
    ui.lua->set_folders({});
    ui.frame();
    CHECK(ui.run("local value=PLUGIN_DATA('lib/owner.lua',true) return value") == "nil");
    CHECK(fs::is_regular_file(data / "base/lib/owner.lua"));
}

TEST_CASE("UI data imports: invalid paths chunks and recursion leave later imports usable") {
    ComposedUi ui;
    const auto data = ui.scratch.path() / "ui-data";
    ui.data_roots = {data, {"leaf", "base"}};
    ui.put(data / "base/lib/good.lua", "return 'good'");
    ui.put(data / "leaf/lib/loop.lua", "return hs.UI.interfaces.LoadData('lib/loop.lua')");
    ui.put(data / "leaf/lib/binary.lua", "\x1bLua");
    ui.put(data / "leaf/lib/fail.lua", "error('bad data')");
    ui.start(true);
    ui.frame();
    for (const auto* path : {"../base/lib/good.lua", "lib/../lib/good.lua", "/good.lua", "C:/good.lua",
                             "lib/good.txt", "lib/missing.lua"}) {
        CAPTURE(path);
        CHECK(ui.lua->run_for_test("", std::string("return hs.UI.interfaces.LoadData('") + path + "')").starts_with("error:"));
    }
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/good.lua'..string.char(0))").starts_with("error:"));
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/binary.lua')").find("binary chunk") != std::string::npos);
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/loop.lua')").find("recursive presentation-data") != std::string::npos);
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/fail.lua')").find("bad data") != std::string::npos);
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/good.lua')") == "good");
    ui.put(data / "leaf/lib/fail.lua", "return 'repaired'");
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/fail.lua')") == "repaired");
    ui.put(ui.root / "base/lib/cross.lua", "return hs.UI.interfaces.LoadData('lib/cross.lua')");
    std::error_code ec;
    fs::create_hard_link(ui.root / "base/lib/cross.lua", data / "base/lib/cross.lua", ec);
    if (ec) { MESSAGE("Hard-link creation is unavailable on this host"); return; }
    CHECK(ui.run("return hs.UI.interfaces.LoadBase('lib/cross.lua')").find("recursive presentation-data") != std::string::npos);
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/good.lua')") == "good");
}

TEST_CASE("UI data imports: missing roots and escaped files or ID folders never grant access") {
    ComposedUi ui;
    ui.start(true);
    ui.frame();
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/value.lua')").find("no approved presentation-data") != std::string::npos);
    ui.lua.reset();
    ui.api.reset();
    const auto data = ui.scratch.path() / "ui-data";
    ui.data_roots = {data, {"leaf", "base"}};
    ui.put(data / "base/lib/good.lua", "return 'good'");
    const auto outside = ui.scratch.path() / "outside/lib/value.lua";
    ui.put(outside, "return 'outside'");
    std::error_code ec;
    fs::create_symlink(outside, data / "base/lib/escape.lua", ec);
    // (Wine makes something that is not a link and reports success.)
    if (ec || !fs::is_symlink(data / "base/lib/escape.lua")) { MESSAGE("File link creation is unavailable on this host"); return; }
    ui.start(true);
    ui.frame();
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/escape.lua')").starts_with("error:"));
    fs::create_directory_symlink(ui.scratch.path() / "outside", data / "leaf", ec);
    if (ec || !fs::is_symlink(data / "leaf")) { MESSAGE("Directory link creation is unavailable on this host"); return; }
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/value.lua')").find("escapes its approved root") != std::string::npos);
    REQUIRE(fs::remove(data / "leaf"));
    ui.put(data / "unapproved/lib/value.lua", "UNAPPROVED_LOADED=true return 'unapproved sibling'");
    fs::create_directory_symlink(data / "unapproved", data / "leaf", ec);
    REQUIRE_FALSE(ec);
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/value.lua')").find("redirects its approved ID") != std::string::npos);
    CHECK(ui.run("return UNAPPROVED_LOADED") == "nil");
    REQUIRE(fs::remove(data / "leaf"));
    CHECK(ui.run("return hs.UI.interfaces.LoadData('lib/good.lua')") == "good");
}

TEST_CASE("UI base imports: helper decorators follow the approved chain and preserve presenter ownership") {
    ComposedUi ui;
    ui.put(ui.root / "base/lib/value.lua", "return 'base',7");
    ui.put(ui.root / "middle/lib/value.lua", "local v,n=hs.UI.interfaces.LoadBase('lib/value.lua') return v..':middle',n+1");
    ui.put(ui.root / "leaf/lib/value.lua", "local v,n=hs.UI.interfaces.LoadBase('lib/value.lua') return v..':leaf',n+1");
    ui.put(ui.root / "base/a.lua", "local v,n=dofile('lib/value.lua') ui.window('A',function() VALUE=v..':'..n end)");
    ui.put(ui.root / "leaf/a.lua", "hs.UI.interfaces.LoadBase('a.lua')");
    ui.put(ui.root / "base/lib/multiple.lua", "return 1,nil,3");
    ui.start(true);
    ui.frame();
    CHECK(ui.run("return VALUE") == "base:middle:leaf:9");
    CHECK(ui.run("local v,n=hs.UI.interfaces.LoadBase('lib/value.lua') return v..':'..n") == "base:middle:8");
    CHECK(ui.run("return select('#',hs.UI.interfaces.LoadBase('lib/multiple.lua'))") == "3");
    CHECK(ui.run("local a,b,c=hs.UI.interfaces.LoadBase('lib/multiple.lua') return a==1 and b==nil and c==3") == "true");
    CHECK(ui.lua->load_status().window_count == 1);
    fs::remove(ui.root / "leaf/a.lua");
    ui.frame(true);
    CHECK(ui.lua->load_status().window_count == 1);
    CHECK(ui.run("return VALUE") == "base:middle:leaf:9");
    // A new inherited presenter also receives the selected helper's decoration without another copy.
    ui.put(ui.root / "base/future.lua", "local v=dofile('lib/value.lua') ui.window('Future',function() FUTURE=v end)");
    ui.frame(true);
    CHECK(ui.run("return FUTURE") == "base:middle:leaf");
    CHECK(ui.lua->load_status().error_count == 0);
}

TEST_CASE("UI base imports: paths and recursive errors stay bounded and leave the next import usable") {
    ComposedUi ui;
    ui.put(ui.root / "base/lib/good.lua", "return 'good'");
    ui.put(ui.root / "leaf/lib/selected.lua", "return 'selected'");
    ui.put(ui.root / "base/lib/fail.lua", "error('base failure')");
    ui.put(ui.root / "base/lib/loop.lua", "return load([[return hs.UI.interfaces.LoadBase('lib/loop.lua')]])()");
    ui.start(true);
    ui.frame();
    for (const auto* path : {"../base/lib/good.lua", "lib/../lib/good.lua", "/lib/good.lua", "C:/good.lua",
                             "lib/good.txt", "lib/selected.lua", "lib/missing.lua"}) {
        CAPTURE(path);
        const auto source = std::string("return hs.UI.interfaces.LoadBase('") + path + "')";
        CHECK(ui.lua->run_for_test("", source).starts_with("error:"));
    }
    CHECK(ui.run("return hs.UI.interfaces.LoadBase('lib/good.lua'..string.char(0)..'.lua')").starts_with("error:"));
    CHECK(ui.run("return hs.UI.interfaces.LoadBase('lib/loop.lua')").find("recursive base-file") != std::string::npos);
    CHECK(ui.run("return hs.UI.interfaces.LoadBase('lib/fail.lua')").find("base failure") != std::string::npos);
    CHECK(ui.run("return hs.UI.interfaces.LoadBase('lib/good.lua')") == "good");
    CHECK(ui.run("return hs.UI.interfaces.LoadBase('lib/good.lua')") == "good");
}

TEST_CASE("UI base imports: plugin environments capabilities and lifetime remain scoped to their owner") {
    ComposedUi ui;
    ui.put(ui.root / "base/lib/owner.lua", "BASE_VALUE='base' return hs.Who(),hs.UI.windows.ClickChild('OptionsWindow','missing','left')");
    ui.put(ui.root / "leaf/lib/owner.lua", "return 'selected'");
    const auto plugin = ui.scratch.path() / "plugin";
    ui.put(plugin / "lib/owner.lua", "return 'plugin'");
    ui.put(plugin / "a.lua", "local who,clicked=hs.UI.interfaces.LoadBase('lib/owner.lua') OWNER=who CLICKED=clicked SAVED_IMPORT=hs.UI.interfaces.LoadBase");
    ui.start(true);
    ui.lua->set_caller_scope([](std::string_view tag) -> std::shared_ptr<void> {
        return std::make_shared<plugin::CallerScope>(tag.empty() ? std::uint32_t{plugin::kCapAll} : 0u, tag);
    });
    ui.lua->set_folders({{"plugin:Importer", plugin}});
    ui.frame();
    CHECK(ui.run("return OWNER", "plugin:Importer") == "plugin:Importer");
    CHECK(ui.run("return BASE_VALUE", "plugin:Importer") == "base");
    CHECK(ui.run("return CLICKED", "plugin:Importer") == "nil");
    CHECK(ui.run("return BASE_VALUE") == "nil");
    CHECK(ui.run("return hs.UI.interfaces.LoadBase('lib/owner.lua')") == "");
    CHECK(ui.run("return BASE_VALUE") == "base");
    // A pack can retain a plugin-owned function without gaining the plugin's environment or extending its life.
    ui.lua->push_folder_env_for_test("plugin:Importer");
    lua_getfield(ui.lua->state(), -1, "SAVED_IMPORT");
    lua_setglobal(ui.lua->state(), "REMOVED_IMPORT");
    lua_pop(ui.lua->state(), 1);
    CHECK(ui.run("return REMOVED_IMPORT('lib/owner.lua')") == "plugin:Importer");
    ui.lua->set_folders({});
    ui.frame();
    CHECK(ui.run("local v=REMOVED_IMPORT('lib/owner.lua') return v") == "nil");
}

TEST_CASE("UI base imports: invalid chunks and escaped links cannot bypass approved text files") {
    ComposedUi ui;
    ui.put(ui.root / "base/lib/binary.lua", "\x1bLua");
    ui.put(ui.root / "base/lib/syntax.lua", "return (");
    ui.put(ui.root / "leaf/lib/selected.lua", "return 'selected'");
    const auto outside = ui.scratch.path() / "outside.lua";
    ui.put(outside, "return 'outside'");
    ui.start(true);
    ui.frame();
    CHECK(ui.run("return hs.UI.interfaces.LoadBase('lib/binary.lua')").find("binary chunk") != std::string::npos);
    CHECK(ui.run("return hs.UI.interfaces.LoadBase('lib/syntax.lua')").starts_with("error:"));
    ui.put(ui.root / "base/lib/syntax.lua", "return 'repaired'");
    CHECK(ui.run("return hs.UI.interfaces.LoadBase('lib/syntax.lua')") == "repaired");
    std::error_code ec;
    fs::create_symlink(outside, ui.root / "base/lib/escape.lua", ec);
    if (ec) { MESSAGE("File link creation is unavailable on this host"); return; }
    fs::create_symlink(ui.root / "leaf/lib/selected.lua", ui.root / "base/lib/selected.lua", ec);
    REQUIRE_FALSE(ec);
    CHECK(ui.run("return hs.UI.interfaces.LoadBase('lib/escape.lua')").starts_with("error:"));
    CHECK(ui.run("return hs.UI.interfaces.LoadBase('lib/selected.lua')").starts_with("error:"));
}

TEST_CASE("UI loading status: actual scan and errors follow reload and exclude plugin owners") {
    ComposedUi ui;
    ui.put(ui.root / "base/a.lua", "ui.window('A',function() end)");
    ui.put(ui.root / "leaf/b.lua", "error('broken presentation')");
    ui.put(ui.scratch.path() / "plugin/c.lua", "error('separate plugin failure')");
    ui.start();
    ui.lua->set_folders({{"plugin:Separate", ui.scratch.path() / "plugin"}});
    CHECK_FALSE(ui.lua->load_status().scanned);
    ui.frame();
    const auto broken = ui.lua->load_status();
    CHECK(broken.scanned);
    CHECK(broken.script_count == 2);
    CHECK(broken.loaded_script_count == 1);
    CHECK(broken.window_count == 1);
    CHECK(broken.error_count == 1);
    REQUIRE(broken.errors.size() == 1);
    CHECK(broken.errors.front().find("broken presentation") != std::string::npos);
    const auto broken_stamp = fs::last_write_time(ui.root / "leaf/b.lua");
    ui.put(ui.root / "leaf/b.lua", "ui.window('B',function() end)");
    // A polling rescan still compares file timestamps. Immediate writes can share a filesystem clock tick.
    fs::last_write_time(ui.root / "leaf/b.lua", broken_stamp + std::chrono::seconds(2));
    REQUIRE(fs::last_write_time(ui.root / "leaf/b.lua") != broken_stamp);
    ui.frame(true);
    const auto ready = ui.lua->load_status();
    CHECK(ready.loaded_script_count == 2);
    CHECK(ready.window_count == 2);
    CHECK(ready.error_count == 0);
    CHECK(ready.errors.empty());
}

TEST_CASE("UI inheritance: nearest filenames override, helper fallback and removal restore the base") {
    ComposedUi ui;
    ui.put(ui.root / "base" / "a.lua", "local h=dofile('lib/value.lua') "
           "ui.window('A',function() A_SOURCE='base' A_HELPER=h A_COUNT=(A_COUNT or 0)+1 end)");
    ui.put(ui.root / "leaf" / "a.lua", "local h=dofile('lib/value.lua') "
           "ui.window('A',function() A_SOURCE='leaf' A_HELPER=h A_COUNT=(A_COUNT or 0)+1 end)");
    ui.put(ui.root / "base" / "b.lua", "ui.window('B',function() B_SOURCE='base' end)");
    ui.put(ui.root / "middle" / "b.lua", "local h=dofile('lib/nearest.lua') "
           "ui.window('B',function() B_SOURCE='middle' B_HELPER=h end)");
    ui.put(ui.root / "base" / "lib" / "value.lua", "return 'base-helper'");
    ui.put(ui.root / "leaf" / "lib" / "value.lua", "return 'leaf-helper'");
    ui.put(ui.root / "base" / "lib" / "nearest.lua", "return 'base-nearest'");
    ui.put(ui.root / "middle" / "lib" / "nearest.lua", "return 'middle-nearest'");
    // Equal timestamps still select the changed source path when an override is removed.
    fs::last_write_time(ui.root / "leaf" / "a.lua", fs::last_write_time(ui.root / "base" / "a.lua"));
    ui.start();
    ui.frame();
    CHECK(ui.run("return A_SOURCE..':'..A_HELPER..':'..A_COUNT") == "leaf:leaf-helper:1");
    CHECK(ui.run("return B_SOURCE..':'..B_HELPER") == "middle:middle-nearest");
    fs::remove(ui.root / "leaf" / "a.lua");
    ui.frame(true);
    CHECK(ui.run("return A_SOURCE..':'..A_HELPER..':'..A_COUNT") == "base:leaf-helper:2");
    ui.put(ui.root / "leaf" / "b.lua", "ui.window('B',function() B_SOURCE='leaf' end)");
    ui.frame(true);
    CHECK(ui.run("return B_SOURCE") == "leaf");

    // A future base window appears in the derived pack without a copy or a new adapter.
    ui.put(ui.root / "base" / "future.lua", "ui.window('Future',function() FUTURE=(FUTURE or 0)+1 end)");
    ui.frame(true);
    CHECK(ui.run("return FUTURE") == "1");
    fs::remove(ui.root / "base" / "future.lua");
    ui.frame(true);
    CHECK(ui.run("return FUTURE") == "1");
    CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
    REQUIRE(ui.lua->set_setting("palette", "custom"));
    ui.lua.reset();
    CHECK(fs::is_regular_file(ui.scratch.path() / "leaf-settings.txt"));
    CHECK_FALSE(fs::exists(ui.root / "base" / "settings.txt"));
}

TEST_CASE("UI inheritance: plugin helpers keep their caller and traversal stays confined") {
    ComposedUi ui;
    ui.put(ui.root / "base" / "lib" / "value.lua", "return 'base'");
    ui.put(ui.root / "middle" / "lib" / "caller.lua", "return hs.Who()");
    ui.put(ui.root / "leaf" / "lib" / "value.lua", "return 'leaf'");
    ui.put(ui.root / "leaf" / "a.lua", "PACK_VALUE=dofile('lib/value.lua') PACK_CALLER=dofile('lib/caller.lua')");
    const auto plugin = ui.scratch.path() / "plugin";
    ui.put(plugin / "lib" / "value.lua", "return 'plugin'");
    ui.put(plugin / "window.lua", "PLUGIN_VALUE=dofile('lib/value.lua') PLUGIN_CALLER=dofile('lib/caller.lua') "
           "ui.window('Plugin',function() PLUGIN_DRAW=(PLUGIN_DRAW or 0)+1 end)");
    ui.put(ui.root / "secret.lua", "return 'outside'");
    ui.start();
    ui.lua->set_folders({{"plugin:Test", plugin}});
    ui.frame();
    CHECK(ui.run("return PACK_VALUE..':'..PACK_CALLER") == "leaf:");
    CHECK(ui.run("return PLUGIN_VALUE..':'..PLUGIN_CALLER..':'..PLUGIN_DRAW", "plugin:Test") == "plugin:plugin:Test:1");
    CHECK(ui.run("return dofile('../secret.lua')").starts_with("error:"));
    CHECK(ui.run("return dofile('../secret.lua')", "plugin:Test").starts_with("error:"));
    const auto absolute = "return dofile([=[" + (ui.root / "secret.lua").string() + "]=])";
    CHECK(ui.lua->run_for_test("plugin:Test", absolute).starts_with("error:"));
    CHECK(ui.run("hs.Who=function() return 'changed' end", "plugin:Test").find("cannot be changed") != std::string::npos);
    CHECK(ui.run("return hs.Who()") == "");
    ui.lua->set_folders({});
    ui.frame();
    ui.lua->push_folder_env_for_test("plugin:Test");
    CHECK(lua_isnil(ui.lua->state(), -1));
    lua_pop(ui.lua->state(), 1);
    CHECK_FALSE(ImGui::FindWindowByName("UI errors"));
}

TEST_CASE("UI inheritance: linked entry scripts and helpers cannot escape approved folders") {
    ComposedUi ui;
    const auto outside = ui.scratch.path() / "outside.lua";
    ui.put(outside, "OUTSIDE=true return 'outside'");
    std::error_code ec;
    fs::create_symlink(outside, ui.root / "leaf" / "escape.lua", ec);
    if (ec) { MESSAGE("File link creation is unavailable on this host"); return; }
    fs::create_symlink(outside, ui.root / "leaf" / "lib" / "escape.lua", ec);
    REQUIRE_FALSE(ec);
    ui.start();
    ui.frame();
    CHECK(ui.run("return OUTSIDE") == "nil");
    CHECK(ui.run("return dofile('lib/escape.lua')").starts_with("error:"));
    CHECK(ui.run("return OUTSIDE") == "nil");
}
