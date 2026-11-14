// Active quest views for Lua-owned journals and trackers: no dependency on TaskWnd's visibility or selection.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "app/lua_eq_caps.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "net/task_packets.h"
#include "plugin/policy.h"
#include "scratch_dir.h"

using namespace hailstone;

namespace {

struct TasksHarness {
    ImGuiContext* context = ImGui::CreateContext();
    test::ScratchDir scratch{"lua_eq_tasks_test"};
    GameDir game;
    std::unique_ptr<net::GameConnection> conn;
    app::EqHost host;
    std::unique_ptr<app::EqApi> api;
    lua_State* lua = luaL_newstate();

    TasksHarness() {
        std::ofstream(scratch.path() / "eqstr_us.txt") << "EQST0002\n"
            << "3474 ? ? ?\n3475 Unlimited\n3476 Done\n3477 Rewards:<BR>\n13205 ALL\n"
            << "6115 Optional: \n138 Experience\n1215 points\n";
        game.set_base(scratch.path());
        conn = std::make_unique<net::GameConnection>(game);
        host.conn = conn.get();
        host.game = &game;
        host.in_game = host.ui_ready = true;
        host.zone_short_name = "qeynos";
        api = std::make_unique<app::EqApi>(host);
        api->load_layout(scratch.path() / "defaults.ini", scratch.path() / "UI_Test_Server.ini", {});
        ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
        ImGui::GetIO().IniFilename = nullptr;
        luaL_openlibs(lua);
        lua_newtable(lua);
        api->register_functions(lua);
        lua_setglobal(lua, "eq");
        api->begin_frame();
    }
    ~TasksHarness() {
        lua_close(lua);
        api.reset();
        ImGui::DestroyContext(context);
    }
    std::string run(const std::string& code) {
        const auto status = luaL_dostring(lua, code.c_str());
        INFO((status == LUA_OK ? "" : lua_tostring(lua, -1)));
        REQUIRE(status == LUA_OK);
        std::string value = lua_isnil(lua, -1) ? "nil" : luaL_tolstring(lua, -1, nullptr);
        lua_settop(lua, 0);
        return value;
    }
    void task(std::uint32_t id, std::uint32_t type, std::uint32_t sequence, std::uint32_t duration = 0,
              std::uint32_t elapsed = 0) {
        game::Task t;
        t.task_id = id;
        t.type = type;
        t.sequence = sequence;
        t.title = "Task " + std::to_string(id);
        t.description = "Description " + std::to_string(id) + ": %3";
        t.duration = duration;
        t.elapsed = elapsed;
        t.has_rewards = true;
        t.xp_reward = true;
        t.points = 3;
        conn->receive_zone_packet_for_test(net::encode_task_description(t));
    }
    void element(std::uint32_t id, std::uint32_t type, std::uint32_t sequence, std::uint32_t activity,
                 std::uint32_t group, bool hidden, int count = 0, int goal = 5, const std::string& zones = "1;2") {
        net::TaskActivity packet;
        packet.task_id = id;
        packet.type = type;
        packet.sequence = sequence;
        auto& e = packet.element;
        e.activity_id = activity;
        e.step = group;
        e.hidden = hidden;
        e.type = hidden ? -1 : 2;
        e.description = "Objective " + std::to_string(activity);
        e.goal_count = goal;
        e.done_count = count;
        e.zones = zones;
        conn->receive_zone_packet_for_test(net::encode(packet));
    }
    void fixtures(std::uint32_t offset = 0) {
        const auto quest = 101u + offset, shared = 201u + offset, task_id = 301u + offset;
        // Arrival order differs from native type/slot order.
        task(quest, game::kTaskTypeQuest, 4, 60, 65);
        task(shared, game::kTaskTypeShared, 0);
        task(task_id, game::kTaskTypeTask, 0, 3600);
        element(quest, game::kTaskTypeQuest, 4, 0, 1, false, 1, 1);
        element(quest, game::kTaskTypeQuest, 4, 1, 0, true);
        element(quest, game::kTaskTypeQuest, 4, 2, 0, false, 2);
        element(quest, game::kTaskTypeQuest, 4, 3, 1, true);
        element(quest, game::kTaskTypeQuest, 4, 4, 20, false); // outside the native window's 0..19 groups
        element(shared, game::kTaskTypeShared, 0, 0, 0, false, 0, 3, "2");
    }
};

} // namespace

TEST_CASE("eq Tasks: all active tasks and individual hidden objectives while the journal is closed") {
    TasksHarness t;
    t.fixtures();
    CHECK_FALSE(t.api->shown("TaskWnd"));
    CHECK(t.run("local s=eq.Tasks(); return #s .. ':' .. s[1].id .. ':' .. s[2].id .. ':' .. s[3].id") == "3:301:201:101");
    CHECK(t.run("return eq.Tasks().selectedId") == "0");
    CHECK(t.run("local q=eq.Tasks()[3]; return q.total .. ':' .. q.done .. ':' .. #q.objectives") == "4:1:5");
    CHECK(t.run("local o=eq.Tasks()[3].objectives; return o[1].activityId .. ':' .. o[2].activityId .. ':' .. tostring(o[3].separator) .. ':' .. o[4].activityId .. ':' .. o[5].activityId") == "1:2:true:0:3");
    CHECK(t.run("local o=eq.Tasks()[3].objectives; return tostring(o[1].hidden) .. ':' .. tostring(o[5].hidden) .. ':' .. o[1].text") == "true:true:? ? ?");
    CHECK(t.run("local h=eq.Tasks()[3].objectives[1]; return tostring(h.count) .. ':' .. tostring(h.goal) .. ':' .. h.status") == "nil:nil:");
    CHECK(t.run("local o=eq.Tasks()[3].objectives[2]; return o.count .. ':' .. o.goal .. ':' .. tostring(o.currentZone)") == "2:5:true");
    CHECK(t.run("return tostring(eq.Tasks()[2].objectives[1].currentZone)") == "false");
    CHECK_FALSE(t.api->shown("TaskWnd"));

    // Changing progress while hidden is visible immediately, with no cached TaskWnd rebuild or selected row.
    t.element(101, game::kTaskTypeQuest, 4, 2, 0, false, 5);
    CHECK(t.run("return eq.Tasks()[3].done") == "2");
    CHECK(t.run("return eq.Tasks().selectedId") == "0");
    CHECK_FALSE(t.api->shown("TaskWnd"));
}

TEST_CASE("eq OpenTask: selects the requested native entry and leaves invalid requests alone") {
    TasksHarness t;
    t.fixtures();
    CHECK(t.run("return eq.OpenTask(999)") == "false");
    CHECK(t.run("return eq.OpenTask(-1)") == "false");
    CHECK(t.run("return eq.OpenTask(4294967296)") == "false");
    CHECK(t.run("return (pcall(eq.OpenTask, {}))") == "false");
    CHECK(t.run("return (pcall(eq.OpenTask, 0/0))") == "false");
    CHECK_FALSE(t.api->shown("TaskWnd"));
    CHECK(t.run("return eq.OpenTask(101)") == "true");
    CHECK(t.api->shown("TaskWnd"));
    CHECK(t.run("return eq.Tasks().selectedId") == "101");
    CHECK(t.run("return eq.TabPage('TaskWnd', 'TaskSubwindows')") == "0");
    CHECK(t.run("return (eq.WindowText('TaskWnd', 'TASK_Description'))").find("Description 101") != std::string::npos);
    CHECK(t.run("return (eq.ListInfo('TaskWnd', 'TASK_TaskElementList'))") == "5");

    t.api->show("TaskWnd", false);
    t.element(201, game::kTaskTypeShared, 0, 0, 0, false, 2, 3, "2");
    CHECK(t.run("return eq.Tasks()[2].objectives[1].count") == "2");
    CHECK(t.run("return eq.Tasks().selectedId") == "101");
    CHECK_FALSE(t.api->shown("TaskWnd"));
    CHECK(t.run("return eq.OpenTask(201)") == "true");
    CHECK(t.run("return eq.Tasks().selectedId") == "201");
    CHECK(t.run("return (eq.ListInfo('TaskWnd', 'TASK_TaskElementList'))") == "1");
    t.api->begin_frame();
    CHECK(t.run("return eq.Tasks().selectedId") == "201");
    CHECK(t.run("return (eq.WindowText('TaskWnd', 'TASK_Description'))").find("Description 201") != std::string::npos);
    CHECK(app::eq_call_caps("Tasks") == 0);
    CHECK(app::eq_call_caps("OpenTask") == plugin::kCapCommands);
}

TEST_CASE("eq Tasks: real reward and timer fields, and no active view outside the world") {
    TasksHarness t;
    t.fixtures();
    CHECK(t.run("return eq.Tasks()[3].left") == "0");
    CHECK(t.run("return eq.Tasks()[2].left") == "0");
    CHECK(t.run("local s=eq.Tasks(); return s[1].left>3500 and s[1].left<=3600") == "true");
    CHECK(t.run("return eq.Tasks()[3].description") == "Description 101: Objective 2");
    CHECK(t.run("return eq.Tasks()[3].rewards") == "Rewards:<BR>Experience<BR>3 points");
    CHECK(t.run("return eq.Tasks()[3].details").find("Description 101: Objective 2<BR><BR>Rewards:") == 0);
    ByteWriter timer;
    timer.le(std::uint32_t{60000});
    t.conn->receive_zone_packet_for_test(net::AppPacket(net::to_wire(net::Op::TaskRequestTimer), timer.take()));
    CHECK(t.run("local n=eq.Tasks().requestTimerLeft; return n>=59 and n<=60") == "true");
    t.host.in_game = false;
    CHECK(t.run("return #eq.Tasks()") == "0");
    CHECK(t.run("return eq.Tasks().selectedId") == "0");
    CHECK(t.run("return eq.OpenTask(101)") == "false");
}

TEST_CASE("eq OpenTask: a new character with an equal task revision rebuilds the old journal rows") {
    TasksHarness t;
    t.fixtures();
    REQUIRE(t.run("return eq.OpenTask(101)") == "true");
    const auto old_revision = t.conn->tasks().revision;
    t.api->show("TaskWnd", false);

    // The UI and its stock window models outlive a character's connection; its revision counter starts again.
    t.conn = std::make_unique<net::GameConnection>(t.game);
    t.host.conn = t.conn.get();
    t.fixtures(1000);
    REQUIRE(t.conn->tasks().revision == old_revision);
    CHECK(t.run("return eq.OpenTask(1101)") == "true");
    CHECK(t.api->shown("TaskWnd"));
    CHECK(t.run("return eq.Tasks().selectedId") == "1101");
    CHECK(t.run("return (eq.WindowText('TaskWnd', 'TASK_Description'))").find("Description 1101") == 0);
}
