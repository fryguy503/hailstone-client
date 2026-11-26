// The quantity dialog from UiHost's state (app/lua_ui_dialogs.cpp quantity_sync, quantity_set_slider,
// quantity_accept): what a script's /notify QuantityWnd sets and accepts, with no UI drawn and no ImGui frame.
#include <doctest/doctest.h>

#include <optional>

#include <imgui.h>

#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "core/game_dir.h"
#include "game/quantity_dialog.h"
#include "net/game_connection.h"
#include "scratch_dir.h"

using namespace hailstone;

TEST_CASE("quantity dialog: opened, set and accepted from UiHost's state, no ImGui frame") {
    ImGuiContext* had = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(nullptr); // as a client with no UI: open_quantity must not reach ImGui
    {
        hailstone::test::ScratchDir scratch("notify_quantity");
        GameDir game(scratch.path());
        net::GameConnection conn(game, {});
        app::UiHost host;
        host.conn = &conn;
        CHECK(app::quantity_sync(host, nullptr) == 0);
        CHECK_FALSE(app::quantity_set_slider(host, 3));
        CHECK_FALSE(app::quantity_accept(host));

        std::optional<std::int32_t> got;
        game::QuantityOpen q;
        q.max = q.start = 20;
        app::open_quantity(host, q, [&](std::int32_t n) { got = n; });
        CHECK(app::quantity_sync(host, nullptr) == 2); // new: its slider at its start
        CHECK(app::quantity_sync(host, nullptr) == 1);
        CHECK(host.quantity_entry.text == "20");
        CHECK(app::quantity_set_slider(host, 5)); // newvalue: the slider, min-relative (min 0 here)
        CHECK(host.quantity_entry.text == "5");
        CHECK(app::quantity_set_slider(host, 99)); // clamped to the range
        CHECK(host.quantity_entry.text == "20");
        CHECK(app::quantity_set_slider(host, 7));
        CHECK(app::quantity_accept(host));
        REQUIRE(got);
        CHECK(*got == 7);
        CHECK_FALSE(host.quantity);
        CHECK(app::quantity_sync(host, nullptr) == 0);

        // A script's forced Shift (/shiftkey) takes the whole amount at once, as the keyboard's would.
        got.reset();
        host.forced_mods = std::array<bool, 2>{true, false};
        q.use_keys = true;
        app::open_quantity(host, q, [&](std::int32_t n) { got = n; });
        REQUIRE(got);
        CHECK(*got == 20);
        CHECK_FALSE(host.quantity);
        // Forced off (/nomodkey): the dialog stays open for its slider.
        got.reset();
        host.forced_mods = std::array<bool, 2>{false, false};
        app::open_quantity(host, q, [&](std::int32_t n) { got = n; });
        CHECK_FALSE(got);
        CHECK(app::quantity_sync(host, nullptr) == 2);
    }
    ImGui::SetCurrentContext(had);
}
