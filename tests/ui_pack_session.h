#pragma once

// Shared by the ui_pack_windows*_test.cpp files, which run the shipped UIs (ui/modern, ui/gamepad) over Dear ImGui
// with no renderer: a pack loaded against a game connection, and frames drawn as the app draws them. The tests are
// split over several files so they compile in parallel.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <set>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "lua_ui_api_mock.h"

#include "app/actions_ui.h"
#include "app/chat_tabs.h"
#include "app/lua_ui.h"
#include "app/gamepad.h"
#include "app/market_ui.h"
#include "core/byte_io.h"
#include "core/config.h"
#include "core/game_dir.h"
#include "net/aa_packets.h"
#include "net/game_connection.h"
#include "net/evolving_packets.h"
#include "net/group_packets.h"
#include "net/merchant_packets.h"
#include "net/packet_io.h"
#include "net/social_packets.h"
#include "net/spell_packets.h"
#include "net/task_packets.h"
#include "game/mechanics/character.h"
#include "game/mechanics/tables.h"
#include "mq/map_filter.h"
#include "platform/paths.h"
#include "ui/dev_screens.h"
#include "ui/layout_profile.h"
#include "ui/lua/lua_host.h"
#include "mock_server/harness.h"
#include "manual_clock.h"
#include "scratch_dir.h"
#include "app/web_service.h"
#include "mq/lua_mq.h"
#include "plugin/policy.h"
#include "plugin/ui_bridge.h"
#include "web/browser_host.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct Session {
    GameDir game;
    net::GameConnection conn;
    ui::ChatView view;
    app::UiHost host;
    app::UiApi api;
    ui::lua::LuaUi lua;
    ImVec2 display_size{1600, 900};
    Session(const fs::path& dir, const std::string& pack, net::ConnectionOptions options = {}, bool in_game = true)
        : game(dir / "game"), conn(game, std::move(options)), api(host), lua(dir / pack, dir / "ui.txt") {
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        host.in_game = host.ui_ready = in_game;
        lua.add_ui_api([this, in_game](lua_State* L) {
            api.register_functions(L);
            if (in_game) {
                lua_pushcfunction(L, [](lua_State* S) { lua_pushboolean(S, 1); return 1; });
                lua_setfield(L, -2, "InGame");
            }
            hailstone::test::sync_ui_api_aliases(L);
        });
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
    }
    void frames(int n, ui::InGameView* fallback = nullptr, const ui::LoadingView* loading = nullptr) {
        for (int i = 0; i < n; ++i) {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = display_size;
            io.DeltaTime = 1.0f / 60.0f;
            hailstone::test::sync_ui_api_aliases(lua.state());
            api.begin_frame();
            ImGui::NewFrame();
            if (fallback) (void)ui::in_game_overlay(*fallback);
            // As the app draws the loading screen (ClientApp::run): over everything, or, while the plugins' question
            // waits (Automation::frame's guard), a window after the others with the question raised above it.
            const bool question = loading && host.plugin_consent && host.plugin_consent().has_value();
            ui::lua::set_consent_guard(question);
            if (loading && !question) ui::loading_overlay(*loading);
            lua.frame();
            if (question) {
                ui::loading_overlay(*loading, false);
                ui::lua::raise_guarded_windows();
            }
            ImGui::Render();
            ui::lua::set_consent_guard(false);
        }
    }
};

struct ReplacementCheck {
    explicit ReplacementCheck(const app::UiApi& api) {
        ui::lua::set_replaced_check([p = &api](std::string_view screen) { return p->replaces(screen); });
    }
    ~ReplacementCheck() { ui::lua::set_replaced_check({}); }
};

} // namespace
