// The accessibility options (game/accessibility.h; docs/dev-ui.md "Accessibility"): the colour-vision con colours, the
// options' load, store and clamping, the screen-effects scaling, and the minimum text size (ui/text_floor.h) through
// the Lua bridge's font calls over Dear ImGui with no renderer.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "core/config.h"
#include "game/accessibility.h"
#include "game/combat.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"
#include "ui/text_floor.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

constexpr game::Con kCons[] = {game::Con::Gray,   game::Con::Green,  game::Con::LightBlue, game::Con::DarkBlue,
                               game::Con::White,  game::Con::Yellow, game::Con::Red};

// Restores the process-wide settings a case changed.
struct AccessibilityReset {
    ~AccessibilityReset() {
        game::set_accessibility({});
        ui::set_text_floor_px(0.0f);
    }
};

} // namespace

TEST_CASE("Accessibility: off keeps the client's own con colours") {
    AccessibilityReset reset;
    game::set_accessibility({});
    CHECK(game::con_rgb(game::Con::Gray) == 0x9a9a9au);
    CHECK(game::con_rgb(game::Con::Green) == 0x00ee00u);
    CHECK(game::con_rgb(game::Con::LightBlue) == 0x00e0ffu);
    CHECK(game::con_rgb(game::Con::DarkBlue) == 0x4060ffu);
    CHECK(game::con_rgb(game::Con::White) == 0xffffffu);
    CHECK(game::con_rgb(game::Con::Yellow) == 0xffff00u);
    CHECK(game::con_rgb(game::Con::Red) == 0xff2020u);
    CHECK(game::con_rgb(game::Con::None) == 0xc0c0c0u);
}

TEST_CASE("Accessibility: each colour-vision mode gives its set, and con_rgb follows the mode in force") {
    AccessibilityReset reset;
    // Okabe and Ito's set for both red-green modes: green, red and yellow are no longer the stock pure hues.
    for (auto mode : {game::ColourVision::Deuteranopia, game::ColourVision::Protanopia}) {
        CHECK(game::con_rgb(game::Con::Green, mode) == 0x009e73u);
        CHECK(game::con_rgb(game::Con::LightBlue, mode) == 0x56b4e9u);
        CHECK(game::con_rgb(game::Con::DarkBlue, mode) == 0x0072b2u);
        CHECK(game::con_rgb(game::Con::Yellow, mode) == 0xf0e442u);
        CHECK(game::con_rgb(game::Con::Red, mode) == 0xd55e00u);
    }
    // Tol's vibrant for tritanopia.
    CHECK(game::con_rgb(game::Con::Gray, game::ColourVision::Tritanopia) == 0xbbbbbbu);
    CHECK(game::con_rgb(game::Con::Green, game::ColourVision::Tritanopia) == 0x009988u);
    CHECK(game::con_rgb(game::Con::Yellow, game::ColourVision::Tritanopia) == 0xee7733u);
    CHECK(game::con_rgb(game::Con::Red, game::ColourVision::Tritanopia) == 0xcc3311u);
    // Every mode: seven different colours, white kept white, Con::None the neutral grey.
    for (int m = 0; m < game::kColourVisionModes; ++m) {
        const auto mode = static_cast<game::ColourVision>(m);
        std::set<std::uint32_t> seen;
        for (game::Con c : kCons) seen.insert(game::con_rgb(c, mode));
        CHECK(seen.size() == 7);
        CHECK(game::con_rgb(game::Con::White, mode) == 0xffffffu);
        CHECK(game::con_rgb(game::Con::None, mode) == 0xc0c0c0u);
    }
    // The one-argument form every caller uses reads the mode in force.
    game::Accessibility a;
    a.colour_vision = game::ColourVision::Deuteranopia;
    game::set_accessibility(a);
    CHECK(game::con_rgb(game::Con::Red) == 0xd55e00u);
    a.colour_vision = game::ColourVision::Tritanopia;
    game::set_accessibility(a);
    CHECK(game::con_rgb(game::Con::Red) == 0xcc3311u);
}

TEST_CASE("Accessibility: con colour names") {
    CHECK(game::con_colour_name(game::Con::Gray) == "Gray");
    CHECK(game::con_colour_name(game::Con::LightBlue) == "Light Blue");
    CHECK(game::con_colour_name(game::Con::DarkBlue) == "Blue");
    CHECK(game::con_colour_name(game::Con::Red) == "Red");
    CHECK(game::con_colour_name(game::Con::None).empty());
}

TEST_CASE("Accessibility: the options load, store, clamp, and default to the old behaviour") {
    Config empty;
    const game::Accessibility d = game::load_accessibility(empty);
    CHECK(d.colour_vision == game::ColourVision::Off);
    CHECK_FALSE(d.con_labels);
    CHECK(d.min_text_size == 0);
    CHECK(d.screen_effects == 100);

    Config c;
    c.set("accessibility.colour_vision", "tritanopia");
    c.set_bool("accessibility.con_labels", true);
    c.set_int("accessibility.min_text_size", 3); // below the range: held to its low end
    c.set_int("accessibility.screen_effects", 250);
    game::Accessibility a = game::load_accessibility(c);
    CHECK(a.colour_vision == game::ColourVision::Tritanopia);
    CHECK(a.con_labels);
    CHECK(a.min_text_size == game::kMinTextSizeLow);
    CHECK(a.screen_effects == 100);
    c.set("accessibility.colour_vision", "mauve");
    CHECK(game::load_accessibility(c).colour_vision == game::ColourVision::Off);

    // hs.Option / hs.SetOption
    CHECK(game::set_accessibility_option(a, "colour_vision", 2));
    CHECK(game::accessibility_option(a, "colour_vision") == 2);
    CHECK(game::set_accessibility_option(a, "colour_vision", 9));
    CHECK(game::accessibility_option(a, "colour_vision") == 3);
    CHECK(game::set_accessibility_option(a, "min_text_size", 0));
    CHECK(game::accessibility_option(a, "min_text_size") == 0);
    CHECK(game::set_accessibility_option(a, "min_text_size", 40));
    CHECK(game::accessibility_option(a, "min_text_size") == game::kMinTextSizeHigh);
    CHECK(game::set_accessibility_option(a, "screen_effects", -5));
    CHECK(game::accessibility_option(a, "screen_effects") == 0);
    CHECK(game::set_accessibility_option(a, "con_labels", 0));
    CHECK(game::accessibility_option(a, "con_labels") == 0);
    CHECK_FALSE(game::set_accessibility_option(a, "palette", 1));
    CHECK_FALSE(game::accessibility_option(a, "palette"));

    Config out;
    game::store_accessibility(a, out);
    CHECK(out.get_string("accessibility.colour_vision") == "tritanopia");
    const game::Accessibility back = game::load_accessibility(out);
    CHECK(back.colour_vision == a.colour_vision);
    CHECK(back.min_text_size == a.min_text_size);
    CHECK(back.screen_effects == a.screen_effects);
    CHECK(back.con_labels == a.con_labels);

    // The player's, so every character shares them (docs/layout.md "Shared keys").
    CHECK(Config::is_shared_key("accessibility.colour_vision"));
    CHECK(Config::is_shared_key("accessibility.min_text_size"));
}

TEST_CASE("Accessibility: Screen Effects scales the shake, the flash and the warp") {
    CHECK(game::screen_effect_scale(100) == doctest::Approx(1.0f));
    CHECK(game::screen_effect_scale(50) == doctest::Approx(0.5f));
    CHECK(game::screen_effect_scale(0) == doctest::Approx(0.0f));
    CHECK(game::screen_effect_scale(300) == doctest::Approx(1.0f));
    const glm::vec3 still(10.0f, 20.0f, 30.0f), shaken(12.0f, 16.0f, 31.0f);
    CHECK(game::scaled_shake(still, shaken, 100) == shaken);
    CHECK(game::scaled_shake(still, shaken, 0) == still);
    const glm::vec3 half = game::scaled_shake(still, shaken, 50);
    CHECK(half.x == doctest::Approx(11.0f));
    CHECK(half.y == doctest::Approx(18.0f));
    CHECK(half.z == doctest::Approx(30.5f));
}

TEST_CASE("Minimum text size: the floor and scale helpers") {
    CHECK(ui::floor_text_px(10.0f, 0.0f) == 10.0f);
    CHECK(ui::floor_text_px(10.0f, 14.0f) == 14.0f);
    CHECK(ui::floor_text_px(20.0f, 14.0f) == 20.0f);
    CHECK(ui::floor_text_scale(10.0f, 0.5f, 0.0f) == 0.5f);
    CHECK(ui::floor_text_scale(10.0f, 0.5f, 20.0f) == doctest::Approx(1.0f));
    CHECK(ui::floor_text_scale(25.0f, 1.25f, 20.0f) == 1.25f);
    CHECK(ui::floor_text_scale(0.0f, 0.5f, 20.0f) == 0.5f);
}

TEST_CASE("Minimum text size: ui.SetWindowFontScale and ui.PushFont never go below the floor") {
    AccessibilityReset reset;
    test::ScratchDir scratch("accessibility_text_floor");
    fs::create_directories(scratch.path() / "ui");
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.0f / 60.0f;
    {
        ui::lua::LuaUi lua(scratch.path() / "ui", scratch.path() / "settings.txt");
        lua_State* L = lua.state();
        auto run = [&](const char* code) {
            const int rc = luaL_dostring(L, code);
            CHECK_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
            lua_settop(L, 0);
        };
        ImGui::NewFrame();
        ImGui::Begin("floor");
        const float base = ImGui::GetFontSize();
        REQUIRE(base > 0.0f);

        ui::set_text_floor_px(0.0f); // no floor: the scale as asked
        run("ui.SetWindowFontScale(0.5)");
        CHECK(ImGui::GetFontSize() < base);

        const float floor = base + 7.0f;
        ui::set_text_floor_px(floor);
        run("ui.SetWindowFontScale(0.5)");
        CHECK(ImGui::GetFontSize() >= floor);
        run("ui.SetWindowFontScale(3)"); // above it: as asked
        CHECK(ImGui::GetFontSize() == doctest::Approx(std::round(base * 3.0f)));

        // A font pushed into a window scaled down before the floor came in (no EQ fonts here: PushFont keeps the
        // current font, at its own size or the floor), and the size before it back after the pop.
        ui::set_text_floor_px(0.0f);
        run("ui.SetWindowFontScale(0.5)");
        const float before = ImGui::GetFontSize();
        REQUIRE(before < floor);
        ui::set_text_floor_px(floor);
        run("ui.PushFont(1)");
        CHECK(ImGui::GetFontSize() >= floor);
        run("ui.PopFont()");
        CHECK(ImGui::GetFontSize() == doctest::Approx(before));
        ImGui::End();
        ImGui::EndFrame();
    }
    ImGui::DestroyContext(ctx);
}
