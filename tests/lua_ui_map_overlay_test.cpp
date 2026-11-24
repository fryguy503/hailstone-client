// Map overlays (hs.UI.map.RegisterOverlay): the client-owned drawing layer of the zone map and the minimap, which any
// add-on draws into whoever's copy of map.lua/minimap.lua draws the view. app/lua_ui_map_overlay.cpp.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include <lua.hpp>
#include <imgui.h>

#include "app/lua_ui.h"
#include "mq_test_world.h"
#include "plugin/policy.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct OverlayUi {
    mq_test::World world{"hailstone_map_overlay_test"};
    test::ScratchDir scratch{"ui_map_overlay"};
    ImGuiContext* context = ImGui::CreateContext();
    app::UiHost host;
    // qol holds commands as well; Auto Combat only what an overlay needs. A callback must run under its own source's
    // grants, never the drawing add-on's. A test may take a grant away to see a registration suspended.
    std::uint32_t qol_caps = plugin::kCapRead | plugin::kCapUi | plugin::kCapCommands;
    std::uint32_t autocombat_caps = plugin::kCapRead | plugin::kCapUi;
    std::unique_ptr<app::UiApi> api;
    std::unique_ptr<ui::lua::LuaUi> lua;

    OverlayUi() {
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigErrorRecoveryEnableAssert = false;
        ImGui::GetIO().ConfigErrorRecoveryEnableTooltip = false;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        for (const char* folder : {"ui", "qol", "autocombat"}) fs::create_directories(scratch.path() / folder);
        host.in_game = host.ui_ready = host.feet_valid = true; // the player at the origin (mq_test_world.h)
        host.game = world.game.get();
        host.conn = world.conn.get();
        host.zone_short_name = "qeynos2";
        host.maps_dir = scratch.path() / "maps";
        host.plugin_caps = [this](std::string_view tag) {
            return tag == "plugin:qol" ? qol_caps : autocombat_caps;
        };
        api = std::make_unique<app::UiApi>(host);
        lua = std::make_unique<ui::lua::LuaUi>(scratch.path() / "ui", scratch.path() / "settings.txt");
        lua->add_ui_api([&](lua_State* L) {
            api->register_functions(L);
            // The caller a retained callback actually runs as, and the authority the client installed for it.
            lua_pushcfunction(L, [](lua_State* S) -> int {
                const auto tag = ui::lua::LuaUi::caller_tag();
                lua_pushlstring(S, tag.data(), tag.size());
                lua_pushinteger(S, static_cast<lua_Integer>(plugin::caller_caps()));
                return 2;
            });
            lua_setfield(L, -2, "TestCaller");
        });
        lua->set_folders({{"plugin:qol", scratch.path() / "qol"}, {"plugin:autocombat", scratch.path() / "autocombat"}});
        lua->set_caller_scope([&](std::string_view tag) -> std::shared_ptr<void> {
            return std::make_shared<plugin::CallerScope>(host.plugin_caps(tag), tag);
        });
    }
    ~OverlayUi() {
        lua.reset();
        api.reset();
        ImGui::DestroyContext(context);
    }
    void put(const char* folder, const std::string& text, const char* file = "entry.lua") {
        std::ofstream(scratch.path() / folder / file) << text;
    }
    std::string run(const char* tag, const std::string& code) { return lua->run_for_test(tag, code); }
    // A frame that loads the scripts but draws no map: no overlay runs.
    void idle() {
        ImGui::GetIO().DisplaySize = ImVec2(900, 700);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        api->begin_frame();
        lua->frame();
        ImGui::Render();
    }
    // A frame in which `code` draws, as the folder `tag`'s code would ("" the pack's own).
    std::string draw(const char* tag, const std::string& code) {
        ImGui::GetIO().DisplaySize = ImVec2(900, 700);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        api->begin_frame();
        ImGui::Begin("Map probe");
        std::string out = run(tag, code);
        ImGui::End();
        ImGui::Render();
        return out;
    }
};

} // namespace

TEST_CASE("Map overlays: a drawn view publishes its geometry by priority, and only when drawn") {
    OverlayUi f;
    f.put("ui", R"lua(
order = {}
hs.UI.map.RegisterOverlay(function(v) order[#order+1] = 'later' end, {priority = 10})
hs.UI.map.RegisterOverlay(function(v) order[#order+1] = 'first'; first = v end, {priority = -10})
hs.UI.map.RegisterOverlay(function(v) order[#order+1] = 'tie-a' end)
hs.UI.map.RegisterOverlay(function(v) order[#order+1] = 'tie-b'; last = v end)
)lua");
    f.idle(); // the scripts load, no map draws: no callback
    CHECK(f.run("", "return tostring(#order)") == "0");
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1, 0, 0, true)");
    CHECK(f.run("", "return table.concat(order, ',')") == "first,tie-a,tie-b,later");
    CHECK(f.run("", R"lua(
      return tostring(first.kind == 'zone' and first.x0 == 0 and first.y0 == 0 and first.x1 == 600 and
        first.y1 == 500 and first.scale > 0 and first.radius == 0 and first.turn == 0 and
        first.zone == 'qeynos2' and first.current_zone == true and first.player_x ~= nil and
        first.cx == 300 and first.cy == 250)
    )lua") == "true");
    f.draw("", "hs.DrawMinimap(300, 250, 100, 500)");
    CHECK(f.run("", "return table.concat(order, ',')") ==
          "first,tie-a,tie-b,later,first,tie-a,tie-b,later");
    CHECK(f.run("", R"lua(
      return tostring(last.kind == 'minimap' and math.abs(last.scale - 0.2) < 1e-6 and last.radius == 100 and
        last.cx == 300 and last.cy == 250 and last.x0 == 200 and last.y0 == 150 and last.x1 == 400 and
        last.y1 == 350 and last.turn == 0 and last.zone == 'qeynos2')
    )lua") == "true");
}

TEST_CASE("Map overlays: a failed file is reported and retired whole, and another file keeps drawing") {
    OverlayUi f;
    // The failed callback's file stops as a whole, its sibling included; another file's overlay keeps drawing.
    f.put("ui", R"lua(
attempts, sibling = 0, 0
hs.UI.map.RegisterOverlay(function() attempts = attempts + 1; error('overlay boom') end, {priority = -1})
hs.UI.map.RegisterOverlay(function() sibling = sibling + 1 end)
)lua");
    f.put("ui", R"lua(
good = 0
hs.UI.map.RegisterOverlay(function() good = good + 1 end)
)lua", "other.lua");
    f.idle();
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("", "return attempts .. ',' .. sibling .. ',' .. good") == "1,0,1");
    const auto errors = f.lua->load_status().errors;
    REQUIRE_FALSE(errors.empty());
    bool reported = false;
    for (const auto& line : errors) reported = reported || line.find("overlay boom") != std::string::npos;
    CHECK(reported);
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("", "return attempts .. ',' .. sibling .. ',' .. good") == "1,0,2");
}

TEST_CASE("Map overlays: a non-string error still retires the callback") {
    OverlayUi f;
    f.put("ui", R"lua(
attempts = 0
hs.UI.map.RegisterOverlay(function() attempts = attempts + 1; error({code = 1}) end)
)lua");
    f.idle();
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("", "return tostring(attempts)") == "1");
}

TEST_CASE("Map overlays: an add-on drawing the map runs a second add-on's callback as the second add-on") {
    OverlayUi f;
    // qol keeps its own copy of map.lua and draws the zone map; Auto Combat registered the overlay. qol holds the
    // commands bit, Auto Combat does not: inside each callback the caller and the grants must be its own.
    f.put("qol", R"lua(
qolRuns, qolTag, qolCaps = 0, nil, nil
handle = hs.UI.map.RegisterOverlay(function()
  qolRuns = qolRuns + 1
  qolTag, qolCaps = hs.TestCaller()
end)
)lua");
    f.put("autocombat", R"lua(
runs, nested, tag, caps, commandWorks = 0, 0, nil, nil, nil
handle = hs.UI.map.RegisterOverlay(function(v)
  runs = runs + 1
  tag, caps = hs.TestCaller()
  commandWorks = hs.MapAutoZ() ~= nil
  if v.kind == 'zone' then hs.DrawZoneMap(0, 0, 300, 200, 1); nested = nested + 1 end
end)
)lua");
    f.idle();
    f.draw("plugin:qol", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    const auto floor = std::to_string(plugin::kCapRead | plugin::kCapUi);
    const auto qol_caps = std::to_string(plugin::kCapRead | plugin::kCapUi | plugin::kCapCommands);
    CHECK(f.run("plugin:autocombat", "return tag") == "plugin:autocombat");
    CHECK(f.run("plugin:autocombat", "return tostring(caps)") == floor);          // its ceiling, not qol's grants
    CHECK(f.run("plugin:autocombat", "return tostring(commandWorks)") == "false"); // a commands call is refused
    CHECK(f.run("plugin:qol", "return qolTag") == "plugin:qol");
    CHECK(f.run("plugin:qol", "return tostring(qolCaps)") == qol_caps);
    f.draw("plugin:qol", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("plugin:autocombat", "return tostring(runs)") == "2");   // once per drawn zone map
    CHECK(f.run("plugin:autocombat", "return tostring(nested)") == "2"); // its own draw dispatched nothing again
    CHECK(f.run("plugin:qol", "return tostring(qolRuns)") == "2");
    // Another add-on cannot take an overlay off: only the owner's handle removes it.
    const auto handle = f.run("plugin:autocombat", "return handle");
    REQUIRE_FALSE(handle.empty());
    CHECK(f.run("plugin:qol", "return tostring(hs.UI.map.UnregisterOverlay('" + handle + "'))") == "false");
    f.draw("plugin:qol", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("plugin:autocombat", "return tostring(runs)") == "3");
    CHECK(f.run("plugin:autocombat", "return tostring(hs.UI.map.UnregisterOverlay(handle))") == "true");
    f.draw("plugin:qol", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("plugin:autocombat", "return tostring(runs)") == "3");
    CHECK(f.run("plugin:qol", "return tostring(qolRuns)") == "4");
}

TEST_CASE("Map overlays: a callback's changes to its view do not reach the next callback") {
    OverlayUi f;
    f.put("ui", R"lua(
seen = {}
hs.UI.map.RegisterOverlay(function(v)
  seen[#seen+1] = v.zone .. ':' .. string.format('%.0f', v.cx)
  v.zone, v.cx, v.player_x = 'changed', 999, 12345
end, {priority = -1})
hs.UI.map.RegisterOverlay(function(v)
  seen[#seen+1] = v.zone .. ':' .. string.format('%.0f', v.cx) .. ':' .. tostring(v.player_x ~= nil)
end)
)lua");
    f.idle();
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("", "return seen[1] .. '|' .. seen[2]") == "qeynos2:300|qeynos2:300:true");
}

TEST_CASE("Map overlays: the minimap's turn, and another zone's map has no player") {
    OverlayUi f;
    f.host.heading = 128.0f;
    f.put("ui", R"lua(
turn, seen = nil, {}
hs.UI.map.RegisterOverlay(function(v)
  turn = v.turn
  seen[#seen+1] = {zone = v.zone, own = v.current_zone, player = v.player_x ~= nil}
end)
)lua");
    f.idle();
    f.draw("", "hs.DrawMinimap(300, 250, 100, 500, true)");
    CHECK(f.run("", "return tostring(turn)") == "128.0");
    CHECK(f.run("", "return tostring(seen[1].own)") == "true");
    CHECK(f.run("", "return tostring(seen[1].player)") == "true");
    // The atlas loads another zone's map: the overlay is told, and gets no player position.
    f.draw("", "hs.ShowZoneMap('qeynos'); hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("", "return seen[2].zone") == "qeynos");
    CHECK(f.run("", "return tostring(seen[2].own)") == "false");
    CHECK(f.run("", "return tostring(seen[2].player)") == "false");
    // The minimap draws that other zone's map too: no player marker, so no player fields either, though its circle
    // is still centred on the player.
    f.draw("", "hs.DrawMinimap(300, 250, 100, 500)");
    CHECK(f.run("", "return seen[3].zone") == "qeynos");
    CHECK(f.run("", "return tostring(seen[3].own)") == "false");
    CHECK(f.run("", "return tostring(seen[3].player)") == "false");
}

TEST_CASE("Map overlays: loading another zone inside a callback leaves the drawn view's snapshot intact") {
    OverlayUi f;
    f.put("ui", R"lua(
zones, switch = {}, false
hs.UI.map.RegisterOverlay(function(v)
  if switch then switch = false; hs.ShowZoneMap('qeynos') end
end, {priority = -1})
hs.UI.map.RegisterOverlay(function(v) zones[#zones+1] = v.zone end)
)lua");
    f.idle();
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("", "return zones[1]") == "qeynos2");
    f.run("", "switch = true");
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    // The pass belongs to the view already drawn for qeynos2; the later callback gets that same view, not a name
    // pointing into the map state the first callback just replaced.
    CHECK(f.run("", "return zones[2]") == "qeynos2");
}

TEST_CASE("Map overlays: losing a capability suspends a registration, which draws again when it is back") {
    OverlayUi f;
    f.put("qol", R"lua(
runs = 0
handle = hs.UI.map.RegisterOverlay(function() runs = runs + 1 end)
)lua");
    f.idle();
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("plugin:qol", "return tostring(runs)") == "1");
    // qol loses `ui`: its registration is skipped, not retired.
    f.qol_caps = plugin::kCapRead;
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("plugin:qol", "return tostring(runs)") == "1");
    // The same handle draws again once the grant is back: nothing was retired.
    f.qol_caps = plugin::kCapRead | plugin::kCapUi | plugin::kCapCommands;
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("plugin:qol", "return tostring(runs)") == "2");
}

TEST_CASE("Map overlays: an owner that lost a capability can still take its own overlay off") {
    OverlayUi f;
    f.put("qol", R"lua(
runs = 0
handle = hs.UI.map.RegisterOverlay(function() runs = runs + 1 end)
)lua");
    f.idle();
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    f.qol_caps = plugin::kCapRead;
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    // UnregisterOverlay is ungated: a suspended owner still gets its own handle off (and only its own).
    CHECK(f.run("plugin:qol", "return tostring(hs.UI.map.UnregisterOverlay(handle))") == "true");
    f.qol_caps = plugin::kCapRead | plugin::kCapUi | plugin::kCapCommands;
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("plugin:qol", "return tostring(runs)") == "1");
}

TEST_CASE("Map overlays: an overlay the frame's own time skips stays registered") {
    OverlayUi f;
    f.put("qol", R"lua(
runs = 0
handle = hs.UI.map.RegisterOverlay(function() runs = runs + 1 end)
)lua");
    f.idle();
    // The code that draws the map has no time left of its own: the view is still drawn, but a callback cannot start,
    // and that is not its own failure. It is skipped, not reported to its file, and not retired.
    f.lua->set_limits_for_test(std::size_t{256} << 20, std::chrono::milliseconds(0), std::chrono::milliseconds(400));
    f.draw("", "drawn = hs.DrawZoneMap(0, 0, 600, 500, 1) ~= nil");
    CHECK(f.run("", "return tostring(drawn)") == "true"); // the map itself drew
    CHECK(f.run("plugin:qol", "return tostring(runs)") == "0");
    // The frame has time again: the same handle draws, so nothing was retired.
    f.lua->set_limits_for_test(std::size_t{256} << 20, std::chrono::milliseconds(200), std::chrono::milliseconds(400));
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("plugin:qol", "return tostring(runs)") == "1");
}

TEST_CASE("Map overlays: a callback stopped at the drawing code's own end stays registered") {
    OverlayUi f;
    f.put("ui", R"lua(
runs = 0
handle = hs.UI.map.RegisterOverlay(function()
  runs = runs + 1
  if runs == 1 then while true do end end -- stopped inside the call, not before it
end)
)lua");
    f.idle();
    // The code that draws the map keeps 49 ms for itself and the callback may run longer than that: its own budget
    // (capped at 50 ms) is later, so the frame's end is what stops it mid-call. That is a skip, not its failure.
    f.lua->set_limits_for_test(std::size_t{256} << 20, std::chrono::milliseconds(49), std::chrono::milliseconds(5000));
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("", "return tostring(runs)") == "1"); // it did start
    CHECK(f.lua->load_status().errors.empty());        // and was skipped, so nothing is reported to its file
    // The same handle draws with time to spare, so the skip retired nothing.
    f.lua->set_limits_for_test(std::size_t{256} << 20, std::chrono::milliseconds(4000), std::chrono::milliseconds(5000));
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("", "return tostring(runs)") == "2");
}

TEST_CASE("Map overlays: a callback that uses up its own budget is reported and retired") {
    OverlayUi f;
    f.put("ui", R"lua(
runs = 0
handle = hs.UI.map.RegisterOverlay(function() runs = runs + 1; while true do end end)
)lua");
    f.idle();
    // The drawing code has plenty of time; the callback's own budget (the 50 ms cap on a retained callback) is what
    // runs out, which is its own failure.
    f.lua->set_limits_for_test(std::size_t{256} << 20, std::chrono::milliseconds(4000), std::chrono::milliseconds(4000));
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("", "return tostring(runs)") == "1");
    const auto errors = f.lua->load_status().errors;
    REQUIRE_FALSE(errors.empty());
    bool reported = false;
    for (const auto& line : errors) reported = reported || line.find("ran too long") != std::string::npos;
    CHECK(reported);
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("", "return tostring(runs)") == "1"); // retired, not run again
}

TEST_CASE("Map overlays: an owner granted more than its ceiling retires its overlay for good") {
    OverlayUi f;
    f.put("qol", R"lua(
runs = 0
handle = hs.UI.map.RegisterOverlay(function() runs = runs + 1 end)
)lua");
    f.idle();
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("plugin:qol", "return tostring(runs)") == "1");
    // A wider grant than the one it registered under: it may not go on drawing with it.
    f.qol_caps |= plugin::kCapPackets;
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("plugin:qol", "return tostring(runs)") == "1");
    // And it stays retired when the grant is narrowed again, with the handle already gone.
    f.qol_caps = plugin::kCapRead | plugin::kCapUi | plugin::kCapCommands;
    f.draw("", "hs.DrawZoneMap(0, 0, 600, 500, 1)");
    CHECK(f.run("plugin:qol", "return tostring(runs)") == "1");
    CHECK(f.run("plugin:qol", "return tostring(hs.UI.map.UnregisterOverlay(handle))") == "false");
}
