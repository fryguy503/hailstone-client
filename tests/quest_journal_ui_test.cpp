// The journal and its independent tracker over the real Lua host, Dear ImGui and packet-fed task state.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "net/task_packets.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {
struct JournalSession {
    test::ScratchDir scratch{"quest_journal_ui_test"};
    ImGuiContext* context = ImGui::CreateContext();
    GameDir game;
    std::unique_ptr<net::GameConnection> conn;
    ui::ChatView chat;
    app::EqHost host;
    std::unique_ptr<app::EqApi> api;
    std::unique_ptr<ui::lua::LuaUi> lua;

    JournalSession() {
        const auto root = scratch.path();
        fs::create_directories(root / "game");
        std::ofstream(root / "game" / "eqstr_us.txt")
            << "EQST0002\n3474 ? ? ?\n3475 Unlimited\n3476 Done\n3477 Rewards:<BR>\n13205 ALL\n"
            << "6115 Optional: \n138 Experience\n1215 points\n";
        const auto pack = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
        fs::copy(pack, root / "modern", fs::copy_options::recursive);
        game.set_base(root / "game");
        conn = std::make_unique<net::GameConnection>(game);
        host.game = &game;
        host.conn = conn.get();
        host.chat = &chat;
        host.in_game = host.ui_ready = true;
        host.zone_short_name = "qeynos";
        api = std::make_unique<app::EqApi>(host);
        api->load_layout(root / "defaults.ini", root / "UI_Test_Server.ini", {});
        lua = std::make_unique<ui::lua::LuaUi>(root / "modern", root / "ui.txt");
        lua->add_eq_api([this](lua_State* L) {
            api->register_functions(L);
            lua_pushcfunction(L, [](lua_State* S) { lua_pushboolean(S, 1); return 1; });
            lua_setfield(L, -2, "InGame");
        });
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~JournalSession() {
        lua.reset();
        api.reset();
        ImGui::DestroyContext(context);
    }
    void frames(int count = 2) {
        for (int i = 0; i < count; ++i) {
            auto& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1280, 800);
            io.DeltaTime = 1.0f / 60.0f;
            api->begin_frame();
            ImGui::NewFrame();
            lua->frame();
            ImGui::Render();
            REQUIRE(ImGui::FindWindowByName("UI errors") == nullptr);
        }
    }
    std::string run(const std::string& code) {
        auto* L = lua->state();
        const int result = luaL_dostring(L, code.c_str());
        INFO((result == LUA_OK ? "" : lua_tostring(L, -1)));
        REQUIRE(result == LUA_OK);
        std::string value = lua_isstring(L, -1) ? lua_tostring(L, -1) : "nil";
        lua_settop(L, 0);
        return value;
    }
    void task(std::uint32_t id, std::uint32_t type, const std::string& title) {
        game::Task task;
        task.task_id = id;
        task.type = type;
        task.title = title;
        task.description = "A description unique to " + title;
        task.has_rewards = true;
        task.xp_reward = true;
        conn->receive_zone_packet_for_test(net::encode_task_description(task));
    }
    void activity(std::uint32_t id, std::uint32_t type, std::uint32_t slot, std::uint32_t group,
                  const std::string& description, bool hidden) {
        net::TaskActivity packet;
        packet.task_id = id;
        packet.type = type;
        packet.element.activity_id = slot;
        packet.element.step = group;
        packet.element.type = hidden ? -1 : 2;
        packet.element.hidden = hidden;
        packet.element.description = description;
        packet.element.goal_count = 5;
        packet.element.done_count = 1;
        conn->receive_zone_packet_for_test(net::encode(packet));
    }
};
} // namespace

TEST_CASE("quest journal UI: real task state, individual hidden rows and separate content sections") {
    JournalSession session;
    session.task(101, game::kTaskTypeQuest, "First quest");
    session.activity(101, game::kTaskTypeQuest, 0, 0, "Known objective", false);
    session.activity(101, game::kTaskTypeQuest, 1, 0, "", true);
    session.activity(101, game::kTaskTypeQuest, 2, 2, "", true);
    session.activity(101, game::kTaskTypeQuest, 3, 2, "Another objective", false);
    session.frames();
    session.run(R"lua(
        journalSeen = {}
        local draw = ui.DrawText
        ui.DrawText = function(x,y,r,g,b,a,value)
            journalSeen[value] = (journalSeen[value] or 0) + 1
            return draw(x,y,r,g,b,a,value)
        end
        local colored = ui.TextColored
        ui.TextColored = function(r,g,b,a,value)
            journalSeen[value] = (journalSeen[value] or 0) + 1
            return colored(r,g,b,a,value)
        end
        local wrapped = ui.TextWrapped
        ui.TextWrapped = function(value)
            journalSeen[value] = (journalSeen[value] or 0) + 1
            return wrapped(value)
        end
        assert(eq.OpenTask(101))
        ui.SetSetting('tasks.tracker.visible', '0')
        ui.SetSetting('tasks.section.101', 'objectives')
    )lua");
    session.frames();
    CHECK(session.run("return tostring(journalSeen['Known objective'] ~= nil)") == "true");
    CHECK(session.run("return tostring((journalSeen['? ? ?'] or 0) >= 2)") == "true");
    CHECK(session.run("return tostring(journalSeen['Step 1'] == nil and journalSeen['Step 2'] == nil)") == "true");
    CHECK(session.run("return tostring(eq.Tasks()[1].total)") == "4");

    session.run("journalSeen = {}; ui.SetSetting('tasks.section.101', 'description')");
    session.frames();
    CHECK(session.run("return tostring(journalSeen['A description unique to First quest'] ~= nil)") == "true");
    CHECK(session.run("return tostring(journalSeen['Known objective'] == nil)") == "true");

    session.run("journalSeen = {}; ui.SetSetting('tasks.section.101', 'rewards')");
    session.frames();
    CHECK(session.run("return tostring(journalSeen['Experience'] ~= nil)") == "true");
    CHECK(session.run("return tostring(journalSeen['A description unique to First quest'] == nil)") == "true");
}

TEST_CASE("quest tracker UI: journal closed, saved dimensions and roll-up over Dear ImGui") {
    JournalSession session;
    session.task(101, game::kTaskTypeQuest, "First quest");
    session.activity(101, game::kTaskTypeQuest, 0, 0, "Known objective", false);
    session.task(202, game::kTaskTypeShared, "Shared quest");
    session.activity(202, game::kTaskTypeShared, 0, 0, "Shared objective", false);
    session.frames();
    session.run(R"lua(
        ui.SetSetting('tasks.tracked.101', '1')
        ui.SetSetting('tasks.tracked.202', '1')
        ui.SetSetting('tasks.tracker.visible', '1')
        ui.SetSetting('hud.size.Quest Tracker', '240,150')
        eq.ShowWindow('TaskWnd', false)
    )lua");
    session.frames();
    auto* tracker = ImGui::FindWindowByName("###modern.Quest Tracker");
    REQUIRE(tracker != nullptr);
    CHECK(tracker->Active);
    CHECK(tracker->Size.x == doctest::Approx(240));
    CHECK(tracker->Size.y == doctest::Approx(150));
    CHECK(session.run("return tostring(eq.WindowShown('TaskWnd'))") == "false");

    session.run("ui.SetSetting('hud.size.Quest Tracker', '220,100')");
    session.frames();
    CHECK(tracker->Size.x == doctest::Approx(220));
    CHECK(tracker->Size.y == doctest::Approx(100));
    session.run("ui.SetSetting('hud.size.Quest Tracker', '240,150')");
    session.frames();

    session.run("assert(eq.OpenTask(202)); eq.ShowWindow('TaskWnd', false)");
    session.frames();
    CHECK(tracker->Active);
    CHECK(session.run("local Q=dofile('lib/task_journal.lua'); return tostring(#Q.getTracked(Q.tasks()))") == "2");

    session.run("ui.SetSetting('hud.rolled.Quest Tracker', '1')");
    session.frames();
    CHECK(tracker->Size.y == doctest::Approx(28));
    session.run("ui.SetSetting('hud.rolled.Quest Tracker', '0')");
    session.frames();
    CHECK(tracker->Size.y == doctest::Approx(150));
    session.run("ui.SetSetting('tasks.tracker.visible', '0')");
    session.frames();
    CHECK_FALSE(tracker->Active);
    CHECK(session.run("return ui.GetSetting('tasks.tracked.101')") == "1");
}

TEST_CASE("quest journal UI: native shared roster popup and membership updates without task revisions") {
    JournalSession session;
    session.task(101, game::kTaskTypeQuest, "Ordinary quest");
    session.activity(101, game::kTaskTypeQuest, 0, 0, "Ordinary objective", false);
    session.task(202, game::kTaskTypeShared, "Shared quest");
    session.activity(202, game::kTaskTypeShared, 0, 0, "Shared objective", false);
    const auto revision = session.conn->tasks().revision;
    session.conn->receive_zone_packet_for_test(net::encode(std::vector<net::SharedTaskMember>{
        {"Aster", true}, {"Beryl", false},
    }));
    REQUIRE(session.conn->tasks().revision == revision);
    session.frames();
    session.run(R"lua(
        sharedRosterSeen = {}
        local colored = ui.TextColored
        ui.TextColored = function(r,g,b,a,value)
            sharedRosterSeen[value] = true
            return colored(r,g,b,a,value)
        end
        local button = ui.Button
        ui.Button = function(label,w,h)
            local clicked = button(label,w,h)
            if sharedSmokeClick and label:match('##(.+)$') == sharedSmokeClick then
                sharedSmokeClick = nil
                return true
            end
            return clicked
        end
        assert(eq.OpenTask(202))
        ui.SetSetting('tasks.category', 'shared')
        eq.SetTabPage('TaskWnd', 'TaskSubwindows', 1)
    )lua");
    session.frames();
    CHECK(session.run("return ui.GetSetting('tasks.category')") == "active");
    CHECK(session.run("return tostring(eq.TabPage('TaskWnd','TaskSubwindows'))") == "0");
    CHECK(session.run("return tostring(sharedRosterSeen['Aster'] and sharedRosterSeen['Beryl'])") == "true");
    auto& context = *ImGui::GetCurrentContext();
    REQUIRE(context.OpenPopupStack.Size == 1);
    REQUIRE(context.OpenPopupStack.back().Window != nullptr);
    CHECK(context.OpenPopupStack.back().Window->Active);
    for (const char* id : {"STASK_AddPlayerButton", "STASK_RemovePlayerButton", "STASK_MakeLeaderButton",
                           "STASK_InviteRaidButton", "STASK_InviteExpedButton"}) {
        CHECK(session.run("return tostring(eq.ChildState('TaskWnd','" + std::string(id) + "'))") == "false");
    }

    session.conn->receive_zone_packet_for_test(net::encode(net::SharedTaskMemberChange{true, "Cyra"}));
    CHECK(session.conn->tasks().revision == revision);
    session.run("sharedRosterSeen = {}");
    session.frames();
    CHECK(session.run("return tostring(sharedRosterSeen['Cyra'] and sharedRosterSeen['Beryl'])") == "true");
    session.conn->receive_zone_packet_for_test(net::encode(net::SharedTaskMemberChange{false, "Beryl"}));
    CHECK(session.conn->tasks().revision == revision);
    session.run("sharedRosterSeen = {}");
    session.frames();
    CHECK(session.run("return tostring(sharedRosterSeen['Cyra'] and sharedRosterSeen['Beryl'] == nil)") == "true");

    session.run("sharedSmokeClick = 'closeSharedMembers'");
    session.frames();
    CHECK(context.OpenPopupStack.Size == 0);
    session.run("sharedSmokeClick = 'sharedMembers'");
    session.frames();
    CHECK(context.OpenPopupStack.Size == 1);
    CHECK(session.run("return tostring(eq.Tasks().selectedId)") == "202");
    CHECK(session.run("return tostring(eq.TabPage('TaskWnd','TaskSubwindows'))") == "0");
}
