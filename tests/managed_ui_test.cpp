#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "core/distribution.h"
#include "managed_ui_fixtures.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"

namespace {
namespace fs = std::filesystem;
using hailstone::ui::lua::LuaUi;
namespace fixture = hailstone::test::managed_ui;

void write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

void pack(const fs::path& root) {
    write(root / "managed_fixture.lua", fixture::entry);
    write(root / "lib/managed_value.lua", fixture::module);
}

struct Frame {
    ImGuiContext* context = ImGui::CreateContext();
    Frame() {
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~Frame() { ImGui::DestroyContext(context); }
    void draw(LuaUi& ui) {
        ImGui::GetIO().DisplaySize = {800, 600};
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ui.frame();
        ImGui::Render();
    }
};

bool boolean(lua_State* L, const char* key) {
    lua_getfield(L, -1, key);
    const bool result = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return result;
}

bool nil_global(lua_State* L, const char* key) {
    lua_getglobal(L, key);
    const bool result = lua_isnil(L, -1);
    lua_pop(L, 1);
    return result;
}
} // namespace

TEST_CASE("managed UI: approved entry and module retain layout data without dynamic code or test actions") {
    REQUIRE(hailstone::distribution::managed());
    hailstone::test::ScratchDir scratch("managed_ui");
    const auto root = scratch.path() / "modern";
    pack(root);
    write(scratch.path() / "settings.txt", "test.click.ManagedFixture=fire\nhud.alpha=0.75\n");
    Frame frame;
    LuaUi ui(root, scratch.path() / "settings.txt");
    frame.draw(ui);
    lua_State* L = ui.state();
    lua_getglobal(L, "managed_probe");
    REQUIRE(lua_istable(L, -1));
    CHECK(boolean(L, "managed"));
    CHECK(boolean(L, "load_denied"));
    CHECK(boolean(L, "reader_denied"));
    CHECK(boolean(L, "no_require"));
    CHECK(boolean(L, "layout_write"));
    CHECK_FALSE(boolean(L, "test_write"));
    lua_getfield(L, -1, "module");
    CHECK(lua_tointeger(L, -1) == 17);
    lua_pop(L, 1);
    lua_getfield(L, -1, "test_read");
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, -1, "layout_read");
    REQUIRE(lua_isstring(L, -1));
    CHECK(std::string(lua_tostring(L, -1)) == "0.75");
    lua_settop(L, 0);
    CHECK(ui.run_for_test("", "injected = true").starts_with("error:"));
    CHECK(nil_global(L, "injected"));
}

TEST_CASE("managed UI: edited dependencies and renamed approved files cannot execute") {
    REQUIRE(hailstone::distribution::managed());
    hailstone::test::ScratchDir scratch("managed_ui_reject");
    const auto root = scratch.path() / "modern";
    pack(root);
    Frame frame;
    SUBCASE("an edited dofile dependency") {
        write(root / "lib/managed_value.lua", "injected = true; return 18\n");
        LuaUi ui(root, scratch.path() / "settings.txt");
        frame.draw(ui);
        CHECK(nil_global(ui.state(), "managed_probe"));
        CHECK(nil_global(ui.state(), "injected"));
    }
    SUBCASE("approved bytes at another logical path") {
        write(root / "renamed.lua", fixture::entry);
        LuaUi ui(root, scratch.path() / "settings.txt");
        ui.set_entry_scripts({"renamed.lua"});
        frame.draw(ui);
        CHECK(nil_global(ui.state(), "managed_probe"));
    }
}

TEST_CASE("managed UI: existing code cannot hot reload while approved plugin files can load with core dependencies") {
    REQUIRE(hailstone::distribution::managed());
    hailstone::test::ScratchDir scratch("managed_ui_fixed");
    const auto root = scratch.path() / "modern", plugin_root = scratch.path() / "plugin/modern";
    pack(root);
    write(plugin_root / "managed_window.lua", fixture::plugin);
    Frame frame;
    LuaUi ui(root, scratch.path() / "settings.txt");
    frame.draw(ui);
    write(root / "managed_fixture.lua", "injected = true\n");
    ui.set_show_control(true);
    ui.set_folders({{"plugin:managedfixture", plugin_root}});
    frame.draw(ui);
    CHECK(nil_global(ui.state(), "injected"));
    ui.push_folder_env_for_test("plugin:managedfixture");
    REQUIRE(lua_istable(ui.state(), -1));
    lua_getfield(ui.state(), -1, "managed_plugin");
    CHECK(lua_tointeger(ui.state(), -1) == 17);
    lua_settop(ui.state(), 0);
    CHECK(ImGui::FindWindowByName("Lua UI") == nullptr);
}
