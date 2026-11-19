// The UI's Lua collector runs in Lua's generational mode, on its own, once the scripts have loaded
// (src/ui/lua/lua_host.cpp collect_garbage).
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <imgui.h>
#include <lua.hpp>

#include "core/frame_sections.h"
#include "ui/lua/lua_gc_live.h"
#include "ui/lua/lua_host.h"
#include "scratch_dir.h"

namespace fs = std::filesystem;

TEST_CASE("Lua UI: the generational collector runs on its own and keeps the heap bounded") {
    hailstone::test::ScratchDir scratch("lua_gc_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    // A live table of about 2 MB, and a few hundred KB of garbage each frame, as the shipped windows make.
    std::ofstream(dir / "garbage.lua") << "keep = {}\n"
                                          "for i = 1, 20000 do keep[i] = { i, tostring(i) } end\n"
                                          "ui.window('Garbage', function()\n"
                                          "  local t = {}\n"
                                          "  for i = 1, 3000 do t[i] = { i, 'x' .. i } end\n"
                                          "end)\n";

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        auto frame = [&] {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        };
        frame(); // loads the script, then collects its loading's garbage and turns the collector generational
        const int live = lua_gc(L, LUA_GCCOUNT);
        CHECK(lua_gc(L, LUA_GCISRUNNING) == 1);
        CHECK(lua_gc(L, LUA_GCGEN, 0, 0) == LUA_GCGEN); // (0, 0: its parameters kept)
        int most = 0;
        for (int i = 0; i < 400; ++i) {
            frame();
            most = std::max(most, lua_gc(L, LUA_GCCOUNT));
        }
        // LuaUi::frame collects in full at three times the live heap (kGcMajorTrigger): the heap stays under that and
        // a frame's garbage.
        CHECK(most < live * 3 + 1024);
        CHECK(lua_gc(L, LUA_GCISRUNNING) == 1);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Lua UI: the host runs the major collections, at three times the live heap, timed") {
    // Tables kept for 40 frames outlive the minor collections, so only a major one frees them: the old generation
    // grows until LuaUi::frame collects. Lua's own majors (at eleven times) must never come first, and a collector
    // that has left generational mode is put back by the next frame.
    hailstone::test::ScratchDir scratch("lua_gc_major_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    std::ofstream(dir / "old.lua") << "local ring, n = {}, 0\n"
                                      "ui.window('Old', function()\n"
                                      "  n = n + 1\n"
                                      "  local t = {}\n"
                                      "  for i = 1, 2000 do t[i] = { i } end\n"
                                      "  ring[n % 40] = t\n"
                                      "end)\n";

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        lua_State* L = lua.state();
        namespace sections = hailstone::core::frame_sections;
        // Whether this frame ran a major collection (its section).
        auto frame = [&] {
            sections::take();
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
            bool major = false;
            for (const auto& [name, ms] : sections::take()) major |= std::string_view(name) == "Lua GC major";
            return major;
        };
        frame(); // loads the script and turns the collector generational
        int majors = 0;
        for (int i = 0; i < 400; ++i) {
            const std::size_t live_before = hailstone::ui::lua::gc_live_bytes(L);
            const bool major = frame();
            const std::size_t live = hailstone::ui::lua::gc_live_bytes(L);
            majors += major ? 1 : 0;
            // GCestimate changes only at a major collection (a minor one keeps it, lgc.c genstep): one changed in a
            // frame without the host's section was Lua's own, untimed.
            if (live != live_before) CHECK(major);
            // At the frame's end the heap is under the trigger, or the host collected.
            CHECK(lua_gc(L, LUA_GCCOUNT) < 3 * std::max<int>(1, static_cast<int>(live >> 10)) + 1);
            CHECK_FALSE(hailstone::ui::lua::gc_left_generational(L));
        }
        CHECK(majors >= 2);
        CHECK(majors < 200); // (not every frame)

        // Lua in incremental mode (as after a bad collection of its own): the next frame collects, timed, and puts it
        // back.
        lua_gc(L, LUA_GCINC, 0, 0, 0);
        REQUIRE(hailstone::ui::lua::gc_left_generational(L));
        CHECK(frame());
        CHECK_FALSE(hailstone::ui::lua::gc_left_generational(L));
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Lua UI: each window's allocations go to its frame section") {
    hailstone::test::ScratchDir scratch("lua_gc_bytes_test");
    const fs::path dir = scratch.path() / "pack";
    fs::create_directories(dir);
    // 'Garbage' makes 3000 small tables a frame (well over 100 KB); 'Quiet' only adds to a number.
    std::ofstream(dir / "garbage.lua") << "ui.window('Garbage', function()\n"
                                          "  local t = {}\n"
                                          "  for i = 1, 3000 do t[i] = { i } end\n"
                                          "end)\n"
                                          "local n = 0\n"
                                          "ui.window('Quiet', function() n = n + 1 end)\n";

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        hailstone::ui::lua::LuaUi lua(dir, scratch.path() / "ui.txt");
        auto frame = [&] {
            ImGui::GetIO().DisplaySize = ImVec2(800, 600);
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        };
        frame(); // loads the script
        namespace sections = hailstone::core::frame_sections;
        sections::take();
        sections::take_bytes();
        const std::size_t before = lua.allocated_bytes();
        frame();
        const std::size_t total = lua.allocated_bytes() - before;
        std::size_t garbage = 0, quiet = 0;
        bool saw_quiet = false;
        for (const auto& [name, bytes] : sections::take_bytes()) {
            if (std::string_view(name) == "Lua window Garbage") garbage = bytes;
            if (std::string_view(name) == "Lua window Quiet") {
                quiet = bytes;
                saw_quiet = true;
            }
        }
        sections::take();
        CHECK(garbage > 100 * 1024);
        CHECK(saw_quiet); // listed with 0, not left out
        // Next to nothing: 'Quiet' makes no tables, but what the host does around a window (its draw list, the
        // collector's state when a slice falls in it) is not fixed, so 0 would hold only by luck of order and timing.
        CHECK(quiet < 1024);
        CHECK(total >= garbage);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Lua UI: the live heap is what the cycle found reachable, not the garbage made while it ran") {
    lua_State* L = luaL_newstate();
    lua_gc(L, LUA_GCINC, 0, 1, 0); // small steps, so a cycle spreads over many (gc_live_bytes in any mode)
    lua_gc(L, LUA_GCSTOP);
    // About 2 MB kept.
    REQUIRE(luaL_dostring(L, "keep = {} for i = 1, 20000 do keep[i] = { i, i } end") == LUA_OK);
    lua_gc(L, LUA_GCCOLLECT);
    const std::size_t live = static_cast<std::size_t>(lua_gc(L, LUA_GCCOUNT)) * 1024;
    // A cycle in steps with garbage made between them, as frames make it between slices.
    int cycles = 0;
    for (int i = 0; i < 100000 && cycles == 0; ++i) {
        luaL_dostring(L, "local t = {} for i = 1, 1000 do t[i] = { i } end");
        if (lua_gc(L, LUA_GCSTEP, 0)) ++cycles;
    }
    REQUIRE(cycles == 1);
    const std::size_t count = static_cast<std::size_t>(lua_gc(L, LUA_GCCOUNT)) * 1024;
    const std::size_t found = hailstone::ui::lua::gc_live_bytes(L);
    CHECK(count > live + live / 2); // the sweep's garbage is still there
    CHECK(found < live + live / 4);  // and is not counted
    CHECK(found > live / 2);
    lua_close(L);
}
