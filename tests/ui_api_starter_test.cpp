// Install the SDK's real minimal pack over the shipped Modern UI and run it through the real game/UI boundary.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "core/config.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "scratch_dir.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"
#include "ui/ui_packs.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct StarterContext {
    ImGuiContext* context = ImGui::CreateContext();
    float before_scale = ui::ui_scale();
    StarterContext() {
        ui::set_ui_scale(1);
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~StarterContext() {
        ImGui::DestroyContext(context);
        ui::set_ui_scale(before_scale);
    }
};

} // namespace

TEST_CASE("UI Options: real presenters show unavailable interface details and keep the active section after reload") {
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        test::ScratchDir scratch("ui_options_interface_status");
        const auto source = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        fs::copy(source, scratch.path() / "pack", fs::copy_options::recursive);
        fs::create_directories(scratch.path() / "game");
        StarterContext context;
        GameDir game(scratch.path() / "game"); net::GameConnection conn(game);
        app::UiHost host;
        host.game = &game; host.conn = &conn; host.in_game = host.ui_ready = true;
        ui::UiPack active; active.id = pack; active.name = pack;
        ui::UiPack unavailable;
        unavailable.id = "addon.example.alternate"; unavailable.name = "Unavailable interface";
        unavailable.source = "addon"; unavailable.addon = "example"; unavailable.runtime = "future-runtime";
        unavailable.available = false; unavailable.unavailable_reason = "Interface runtime is unavailable";
        ui::InterfaceStatus status;
        status.selected = unavailable; status.loaded = active; status.state = "fallback";
        status.diagnostics = {std::string(400, 'x') + " A complete diagnostic must stay readable in either interface."};
        int reads = 0;
        host.interface_list = [&] { return std::vector<ui::UiPack>{active, unavailable}; };
        host.interface_status = [&] { ++reads; return status; };
        host.find_option = [](std::string_view name) -> std::optional<int> {
            if (name == "ui_name") return 1;
            if (name == "ui_name_now") return 0;
            return std::nullopt;
        };
        host.option_choices = [&](std::string_view name) -> std::vector<std::string> {
            if (name == "ui_name") return {active.name, unavailable.name};
            if (name == "ui_info") return {"Active interface", unavailable.unavailable_reason};
            return {};
        };
        app::UiApi api(host);
        api.show("OptionsWindow", true);
        const auto settings = scratch.path() / "settings.txt";
        for (int load = 0; load < 2; ++load) {
            ui::lua::LuaUi lua(scratch.path() / "pack", settings);
            lua.set_entry_scripts({"options.lua"});
            if (load == 0) REQUIRE(lua.set_setting("options.section", "Interface"));
            lua.add_ui_api([&](lua_State* L) { api.register_functions(L); });
            for (int frame = 0; frame < 3; ++frame) {
                ImGui::GetIO().DisplaySize = ImVec2(1600, 1000);
                ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame(); api.begin_frame(); lua.frame(); ImGui::Render();
            }
            const auto loaded = lua.load_status();
            CHECK(loaded.error_count == 0);
            CHECK(loaded.loaded_script_count == 1);
            CHECK(loaded.window_count == 1);
            CHECK(api.shown("OptionsWindow"));
            CHECK(api.replaces("OptionsWindow"));
            CHECK(reads > 0);
            CHECK(lua.setting("options.section") == "Interface");
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
    }
}

TEST_CASE("UI SDK starter: installed example overrides Player and inherits real Modern windows") {
    test::ScratchDir scratch("ui_api_starter");
    const auto source = fs::path(__FILE__).parent_path().parent_path();
    const auto packs_root = scratch.path() / "ui";
    const auto selected_dir = packs_root / "example";
    const auto base_dir = packs_root / "modern";
    fs::create_directories(packs_root);
    fs::copy(source / "sdk" / "ui" / "example", selected_dir, fs::copy_options::recursive);
    fs::copy(source / "ui" / "modern", base_dir, fs::copy_options::recursive);
    REQUIRE(fs::exists(selected_dir / "player.lua"));
    CHECK_FALSE(fs::exists(selected_dir / "inventory.lua"));
    CHECK_FALSE(fs::exists(selected_dir / "lib"));

    const auto packs = ui::list_ui_packs(packs_root);
    const auto* selected = ui::find_ui_pack(packs, "example");
    REQUIRE(selected);
    REQUIRE(selected->id == "example");
    REQUIRE(selected->base_dirs == std::vector<fs::path>{base_dir});

    const auto data = scratch.path() / "data";
    const auto settings = data / "ui" / "settings" / "example.txt";
    const auto character_settings = data / "character" / "ui-example.txt";
    fs::create_directories(scratch.path() / "game");
    StarterContext context;
    {
        GameDir game(scratch.path() / "game");
        net::GameConnection conn(game);
        net::PlayerProfile profile;
        profile.class_ = 1;
        profile.level = 60;
        profile.cur_hp = 100;
        conn.set_profile_for_test(profile);
        game::Spawn player;
        player.id = 1;
        player.name = "Starter Tester";
        player.level = 60;
        auto& world = conn.world_for_test();
        world.player_id = player.id;
        world.spawns.emplace(player.id, player);
        ui::ChatView chat;
        app::UiHost host;
        host.game = &game;
        host.conn = &conn;
        host.chat = &chat;
        host.in_game = host.ui_ready = true;
        host.game_state = 5;
        host.zone_short_name = "qeynos";
        app::UiApi api(host);
        api.load_layout(data / "defaults.ini", data / "UI_Test_Server.ini", {});
        ui::lua::LuaUi lua(selected->dir, settings, selected->base_dirs);
        lua.set_character_settings(character_settings);
        lua.add_ui_api([&](lua_State* L) { api.register_functions(L); });
        api.show("PlayerWindow", true);
        api.show("InventoryWindow", true);
        api.show("SkillsWindow", true);
        REQUIRE(lua.set_setting("starter.acceptance", "selected-pack"));
        REQUIRE(lua.set_setting("hud.pos.PlayerWindow", "40,40"));
        for (int frame = 0; frame < 3; ++frame) {
            auto& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1600, 900);
            io.DeltaTime = 1.0f / 60.0f;
            api.begin_frame();
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
            REQUIRE(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        CHECK(lua.run_for_test("", "return hs.UI.character.Name()") == "Starter Tester");
        CHECK(api.replaces("PlayerWindow"));
        CHECK(api.replaces("InventoryWindow"));
        CHECK(api.replaces("SkillsWindow"));
        const auto* player_window = ImGui::FindWindowByName("Player###modern.PlayerWindow");
        REQUIRE(player_window);
        CHECK(player_window->Active);
        CHECK(player_window->BeginCount == 1);
        CHECK(player_window->Size.x == doctest::Approx(260));
        CHECK(player_window->Size.y == doctest::Approx(72));
        CHECK(std::count_if(GImGui->Windows.begin(), GImGui->Windows.end(), [](const ImGuiWindow* window) {
            return std::string_view(window->Name) == "Player###modern.PlayerWindow";
        }) == 1);
        for (const char* name : {"Inventory###modern.InventoryWindow", "Skills###modern.SkillsWindow"}) {
            CAPTURE(name);
            const auto* inherited = ImGui::FindWindowByName(name);
            REQUIRE(inherited);
            CHECK(inherited->Active);
            CHECK(inherited->BeginCount == 1);
        }
    }
    Config saved, character;
    REQUIRE(saved.load(settings));
    REQUIRE(character.load(character_settings));
    CHECK(saved.get_string("starter.acceptance") == "selected-pack");
    CHECK(character.get_string("hud.pos.PlayerWindow") == "40,40");
    CHECK_FALSE(fs::exists(selected_dir / "settings.txt"));
    CHECK_FALSE(fs::exists(base_dir / "settings.txt"));
    CHECK_FALSE(fs::exists(base_dir / "ui.txt"));
}
