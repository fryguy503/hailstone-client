// Fellowship window behavior over packets from an in-process server.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/lua_eq_windows.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "manual_clock.h"
#include "mock_server/harness.h"
#include "net/fellowship_packets.h"
#include "plugin/hooks.h"
#include "scratch_dir.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"

using namespace hailstone;

namespace {

net::AppPacket roster(const std::vector<net::FellowshipMember>& members, std::uint32_t id = 71,
                      const std::string& motd = "Received message", bool camp = false,
                      std::uint32_t expiry = 2000000000) {
    const auto slots = std::max(net::kNativeFellowshipMembers, members.size());
    ByteWriter w;
    w.le<std::uint32_t>(5);
    w.le(id);
    w.fixed_string("Fixture fellowship", 64);
    w.fixed_string(motd, 1024);
    w.le(static_cast<std::uint32_t>(members.size()));
    for (const auto& member : members) {
        w.le(member.character_id);
        w.fixed_string(member.name, 64);
        w.le(member.zone_id);
        w.le(member.level);
        w.le(member.class_);
        w.le(member.last_seen);
    }
    w.zeros((slots - members.size()) * 84);
    w.le<std::uint32_t>(1);
    w.zeros(slots * 32);
    for (const auto& member : members) w.le<std::uint8_t>(member.sharing ? 1 : 0);
    w.zeros(slots - members.size());
    for (const auto& member : members) w.le<std::uint8_t>(member.capped ? 1 : 0);
    w.zeros(slots - members.size());
    for (const auto& member : members) w.le<std::uint8_t>(member.offline ? 1 : 0);
    w.zeros(slots - members.size());
    w.f32(20);
    w.f32(10);
    w.f32(30);
    w.le<std::uint32_t>(camp ? 2 : 0);
    w.le(expiry);
    w.zeros(8);
    w.le<std::uint32_t>(camp ? 1 : 0);
    return net::AppPacket(net::kOpFellowshipRoster, w.take());
}

std::vector<net::FellowshipMember> members(std::size_t count) {
    std::vector<net::FellowshipMember> list;
    for (std::size_t i = 0; i < count; ++i) {
        net::FellowshipMember m;
        m.character_id = 1001 + static_cast<std::uint32_t>(i);
        m.name = i ? "Member" + std::to_string(i) : "Gennro";
        m.zone_id = i % 2 ? 0 : 2;
        m.level = static_cast<std::uint32_t>(60 + i % 10);
        m.class_ = 2;
        list.push_back(std::move(m));
    }
    return list;
}

struct Window {
    app::EqHost host;
    std::vector<std::unique_ptr<app::StockWindow>> windows;
    app::StockWindow* window = nullptr;
    explicit Window(test::Rig& rig) {
        host.conn = &rig.conn;
        host.game = &rig.game;
        host.in_game = host.ui_ready = true;
        app::add_fellowship_windows(windows);
        for (auto& w : windows) if (w->screen() == "FellowshipWnd") window = w.get();
    }
    app::Child child(const std::string& id) {
        app::Child value;
        window->child(host, id, value);
        return value;
    }
    bool click(const std::string& id) { return window->click(host, id, "left", false); }
    void answer(bool yes) {
        REQUIRE(host.confirm);
        auto done = std::move(host.confirm->done);
        host.confirm.reset();
        done(yes);
    }
};

} // namespace

TEST_CASE("fellowship window: 64 members remain selectable by identity after sort and roster reorder") {
    plugin::Hooks hooks;
    const auto owner = hooks.new_owner("fellowship window fixture");
    std::vector<net::AppPacket> sent;
    hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, net::kOpFellowshipRequest,
                    [&](net::AppPacket& p) { sent.push_back(p); return plugin::Flow::Continue; });
    auto options = test::quick_options();
    options.hooks = &hooks;
    test::Rig rig(test::MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    auto list = members(64);
    rig.conn.receive_zone_packet_for_test(roster(list));
    Window ui(rig);
    REQUIRE(ui.window);
    CHECK(ui.window->title(ui.host) == "Fellowship (64 members)");
    CHECK(ui.child("InviteMemberButton").enabled == false);
    const auto* rows = ui.window->list(ui.host, "MemberList");
    REQUIRE(rows);
    REQUIRE(rows->rows.size() == 64);
    CHECK(rows->rows.front().texts[3] == "L");
    CHECK(rows->rows[1].texts[4] == "Offline");
    REQUIRE(ui.window->list_click(ui.host, "MemberList", 63, false, 0));
    REQUIRE(ui.window->list_header_click(ui.host, "MemberList", 0));
    rows = ui.window->list(ui.host, "MemberList");
    REQUIRE(rows->selected >= 0);
    CHECK(rows->rows[static_cast<std::size_t>(rows->selected)].texts[0] == "Member63");
    REQUIRE(ui.click("RemoveMemberButton"));
    REQUIRE(ui.host.confirm);
    CHECK(ui.host.confirm->text.find("Member63") != std::string::npos);
    std::reverse(list.begin() + 1, list.end());
    rig.conn.receive_zone_packet_for_test(roster(list));
    ui.answer(true);
    REQUIRE(sent.size() == 1);
    ByteReader request(sent.back().data);
    CHECK(request.le<std::uint32_t>() == static_cast<std::uint32_t>(net::FellowshipAction::Remove));
    CHECK(request.le<std::uint32_t>() == 1064u);
    CHECK(rig.conn.fellowship().members.size() == 64); // waits for the server's answer

    list.erase(std::find_if(list.begin(), list.end(), [](const auto& m) { return m.character_id == 1064; }));
    rig.conn.receive_zone_packet_for_test(roster(list));
    CHECK(ui.child("RemoveMemberButton").enabled == false);
    REQUIRE(ui.click("RemoveMemberButton"));
    CHECK_FALSE(ui.host.confirm);
    CHECK(sent.size() == 1);
}

TEST_CASE("fellowship window: MOTD drafts, self sharing and confirmations follow current membership") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto list = members(3);
    rig.conn.receive_zone_packet_for_test(roster(list));
    ByteWriter vitality;
    vitality.le<std::uint64_t>(5000000000);
    vitality.le<std::uint64_t>(9000000000);
    vitality.le<std::uint32_t>(15);
    vitality.le<std::uint32_t>(0);
    vitality.le<std::uint32_t>(50);
    rig.conn.receive_zone_packet_for_test(net::AppPacket(net::kOpFellowshipVitality, vitality.take()));
    Window ui(rig);
    REQUIRE(ui.window);
    CHECK(ui.child("MOTDInput").text == "Received message");
    CHECK(ui.child("FellowshipVitality").text == "XP vitality: 5000000000 / 9000000000    AA vitality: 15 / 50");
    REQUIRE(ui.window->set_text(ui.host, "MOTDInput", std::string(1400, 'x')));
    CHECK(ui.child("MOTDInput").text == std::string(1023, 'x'));
    rig.conn.receive_zone_packet_for_test(roster(list));
    CHECK(ui.child("MOTDInput").text == std::string(1023, 'x'));
    rig.conn.receive_zone_packet_for_test(roster(list, 71, "Server replacement"));
    CHECK(ui.child("MOTDInput").text == "Server replacement");
    REQUIRE(ui.click("EndFellowshipButton"));
    REQUIRE(ui.host.confirm);
    ui.answer(false);
    CHECK(rig.conn.fellowship().active());

    std::swap(list[0], list[1]);
    rig.conn.receive_zone_packet_for_test(roster(list));
    CHECK(ui.child("EndFellowshipButton").enabled == false);
    CHECK(ui.child("MOTDInput").enabled == false);
    CHECK_FALSE(ui.window->set_text(ui.host, "MOTDInput", "Forbidden"));
    REQUIRE(ui.window->list(ui.host, "MemberList"));
    REQUIRE(ui.window->list_click(ui.host, "MemberList", 1, false, 0));
    CHECK(ui.child("ToggleExpSharingButton").enabled == true);
    CHECK(ui.child("RemoveMemberButton").enabled == false);
    list[1].capped = true;
    rig.conn.receive_zone_packet_for_test(roster(list));
    CHECK(ui.child("ToggleExpSharingButton").enabled == false);
    list[1].sharing = true;
    rig.conn.receive_zone_packet_for_test(roster(list));
    CHECK(ui.child("ToggleExpSharingButton").enabled == true);

    REQUIRE(ui.click("LeaveFellowshipButton"));
    REQUIRE(ui.host.confirm);
    rig.conn.receive_zone_packet_for_test(roster({}, 71));
    CHECK(ui.child("CreateFellowshipButton").enabled == true);
    CHECK(ui.child("LeaveFellowshipButton").enabled == false);
    ui.answer(true); // an old confirmation cannot affect a departed fellowship
    CHECK_FALSE(rig.conn.fellowship().active());
}

TEST_CASE("fellowship window: campsite availability and selected kit control creation") {
    plugin::Hooks hooks;
    const auto owner = hooks.new_owner("fellowship camp window fixture");
    std::vector<net::AppPacket> sent;
    hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, net::kOpFellowshipRequest,
                    [&](net::AppPacket& p) { sent.push_back(p); return plugin::Flow::Continue; });
    auto options = test::quick_options();
    options.hooks = &hooks;
    test::Rig rig(test::MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    const auto list = members(3);
    rig.conn.receive_zone_packet_for_test(roster(list));
    ByteWriter w;
    w.le<std::uint32_t>(2);
    w.le<std::uint8_t>(0);
    w.le<std::uint32_t>(16);
    w.le<std::uint32_t>(0);
    w.fixed_string("Unavailable", 12);
    w.le<std::uint8_t>(1);
    w.le<std::uint32_t>(17);
    w.le<std::uint32_t>(0);
    w.fixed_string("Available", 10);
    rig.conn.receive_zone_packet_for_test(net::AppPacket(net::kOpCampsiteKitList, w.take()));
    Window ui(rig);
    REQUIRE(ui.window);
    CHECK(ui.child("CreateCampsite").enabled == false);
    const auto* rows = ui.window->list(ui.host, "CampsiteKitList");
    REQUIRE(rows);
    REQUIRE(rows->rows.size() == 2);
    REQUIRE(ui.window->list_click(ui.host, "CampsiteKitList", 0, false, 0));
    CHECK(ui.child("CreateCampsite").enabled == false);
    REQUIRE(ui.window->list_click(ui.host, "CampsiteKitList", 1, false, 0));
    CHECK(ui.child("CreateCampsite").enabled == true);
    CHECK(ui.child("CampsiteDescription").text == "Available<br><br>No description supplied.<br><br>Available to create.");
    REQUIRE(ui.click("CreateCampsite"));
    REQUIRE(sent.size() == 1);
    ByteReader request(sent.back().data);
    CHECK(request.le<std::uint32_t>() == static_cast<std::uint32_t>(net::FellowshipAction::CreateCamp));
    request.skip(0x428 - 4);
    CHECK(request.le<std::uint32_t>() == 17u);
    rig.conn.receive_zone_packet_for_test(roster(list, 71, "Received message", true));
    CHECK(ui.child("CreateCampsite").enabled == false);
    CHECK(ui.child("DestroyCampsite").enabled == true);
    CHECK(ui.child("CampsiteViewer").text == "Location: 20.0, 10.0, 30.0");
    REQUIRE(ui.click("DestroyCampsite"));
    REQUIRE(ui.host.confirm);
    ui.answer(true);
    REQUIRE(sent.size() == 2);
    ByteReader destroy(sent.back().data);
    CHECK(destroy.le<std::uint32_t>() == static_cast<std::uint32_t>(net::FellowshipAction::DestroyCamp));
}

TEST_CASE("fellowship window: camp expiry counts down without changing server-owned camp state") {
    test::ManualClock clock;
    auto options = test::quick_options();
    options.session.clock = clock.source();
    test::Rig rig(test::MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    clock.hold();
    const auto source = clock.source();
    const auto began = source();
    Window ui(rig);
    ui.host.unix_time = [source, began] {
        return std::int64_t{2000000000} + std::chrono::duration_cast<std::chrono::seconds>(source() - began).count();
    };
    const auto list = members(3);
    rig.conn.receive_zone_packet_for_test(roster(list, 71, "Camp timer", true, 2000003661));
    CHECK(ui.child("CampsiteTime").text == "Time remaining: 1:01:01");
    clock.advance(std::chrono::seconds(61));
    CHECK(ui.child("CampsiteTime").text == "Time remaining: 1:00:00");
    clock.advance(std::chrono::hours(1));
    CHECK(ui.child("CampsiteTime").text == "Awaiting campsite update");
    CHECK(rig.conn.fellowship().camp.active);
    CHECK(ui.child("DestroyCampsite").enabled == true);
    rig.conn.receive_zone_packet_for_test(roster(list, 71, "Camp timer", true, 0));
    CHECK(ui.child("CampsiteTime").text == "Remaining time unavailable");
    rig.conn.receive_zone_packet_for_test(roster(list));
    CHECK(ui.child("CampsiteTime").text == "");
}

TEST_CASE("fellowship window: campsite references retain DBString type-zero STML and escape kit labels") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    test::ScratchDir scratch("fellowship_description_ui");
    {
        std::ofstream file(scratch.path() / "dbstr_us.txt", std::ios::binary);
        file << "900001^0^A fixture camp.<br><c \"#11aa22\">For nearby allies.</c>^\n"
                "900001^6^The wrong string category.^\n";
    }
    rig.game.set_overlay(scratch.path());
    rig.conn.reload_game_files();
    rig.conn.receive_zone_packet_for_test(roster(members(3)));
    const auto send_kit = [&](bool available, std::uint32_t reference) {
        ByteWriter w;
        w.le<std::uint32_t>(1); w.le<std::uint8_t>(available ? 1 : 0);
        w.le<std::uint32_t>(17); w.le(reference); w.fixed_string("Fixture <camp>", 15);
        rig.conn.receive_zone_packet_for_test(net::AppPacket(net::kOpCampsiteKitList, w.take()));
    };
    send_kit(true, 900001);
    Window ui(rig);
    REQUIRE(ui.window->list(ui.host, "CampsiteKitList"));
    REQUIRE(ui.window->list_click(ui.host, "CampsiteKitList", 0, false, 0));
    CHECK(ui.child("CampsiteDescription").text ==
          "Fixture &lt;camp&gt;<br><br>A fixture camp.<br><c \"#11aa22\">For nearby allies.</c><br><br>Available to create.");
    send_kit(false, 900001);
    CHECK(ui.child("CampsiteDescription").text ==
          "Fixture &lt;camp&gt;<br><br>A fixture camp.<br><c \"#11aa22\">For nearby allies.</c><br><br>Creation requirements are not met.");
    send_kit(true, 900002);
    CHECK(ui.child("CampsiteDescription").text ==
          "Fixture &lt;camp&gt;<br><br>No description supplied.<br><br>Available to create.");
}

TEST_CASE("fellowship window: installed category identifiers produce native query masks and reload") {
    plugin::Hooks hooks;
    const auto owner = hooks.new_owner("fellowship categories fixture");
    std::vector<net::AppPacket> sent;
    hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, net::kOpCampsiteKitRequest,
                    [&](net::AppPacket& p) { sent.push_back(p); return plugin::Flow::Continue; });
    auto options = test::quick_options(); options.hooks = &hooks;
    test::Rig rig(test::MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    rig.conn.receive_zone_packet_for_test(roster(members(3)));
    test::ScratchDir scratch("fellowship_category_ui");
    const auto catalog = scratch.path() / "Resources" / "campsitecategories.txt";
    std::filesystem::create_directories(catalog.parent_path());
    {
        std::ofstream file(catalog, std::ios::binary);
        file << "# fixture\n2^Fixture focus\n31^Fixture top\n";
    }
    GameDir game(scratch.path());
    Window ui(rig); ui.host.game = &game;
    const auto* combo = ui.window->combo(ui.host, "CampsiteFilter");
    REQUIRE(combo);
    REQUIRE(combo->choices.size() == 3);
    CHECK(combo->choices[1] == "Fixture focus");
    REQUIRE(ui.window->combo_select(ui.host, "CampsiteFilter", 2));
    REQUIRE(sent.size() == 1);
    ByteReader request(sent.back().data);
    CHECK(request.le<std::uint32_t>() == 1);
    CHECK(request.le<std::uint32_t>() == 0x80000000u);
    REQUIRE(ui.click("ShowAllButton"));
    REQUIRE(sent.size() == 2);
    ByteReader all(sent.back().data);
    CHECK(all.le<std::uint32_t>() == 0);
    CHECK(all.le<std::uint32_t>() == 0x80000000u);
    {
        std::ofstream file(catalog, std::ios::binary | std::ios::trunc);
        file << "3^Replacement fixture\n";
    }
    game.rescan();
    REQUIRE(ui.click("RefreshListButton"));
    REQUIRE(sent.size() == 3);
    ByteReader changed(sent.back().data);
    changed.skip(4);
    CHECK(changed.le<std::uint32_t>() == 0xffffffffu);
    combo = ui.window->combo(ui.host, "CampsiteFilter");
    REQUIRE(combo->choices.size() == 2);
    CHECK(combo->choices[1] == "Replacement fixture");
    CHECK(combo->selected == 0);
    CHECK_FALSE(ui.window->combo_select(ui.host, "CampsiteFilter", 2));
}

TEST_CASE("fellowship UI packs: a full roster draws with vitality at normal and narrow sizes") {
    for (const char* pack : {"modern", "gamepad"}) {
      for (int page : {0, 1}) {
        INFO(pack);
        CAPTURE(page);
        test::Rig rig;
        REQUIRE(rig.to_zone());
        rig.conn.receive_zone_packet_for_test(roster(members(64), 71, "Render fixture", true, 2000003600));
        test::ScratchDir scratch("fellowship_ui_pack");
        const auto settings = scratch.path() / "ui.txt";
        {
            std::ofstream file(settings);
            file << "test.tab.FellowshipWnd.Subwindows=" << page << "\n";
        }
        {
            std::ofstream file(scratch.path() / "dbstr_us.txt");
            file << "900001^0^A fixture camp.<br><c \"#11aa22\">For nearby allies.</c>^\n";
        }
        rig.game.set_overlay(scratch.path());
        rig.conn.reload_game_files();
        ByteWriter kit;
        kit.le<std::uint32_t>(1); kit.le<std::uint8_t>(1);
        kit.le<std::uint32_t>(17); kit.le<std::uint32_t>(900001); kit.fixed_string("Fixture <camp>", 15);
        rig.conn.receive_zone_packet_for_test(net::AppPacket(net::kOpCampsiteKitList, kit.take()));
        const auto scripts = std::filesystem::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        app::EqHost host;
        ui::ChatView chat;
        host.game = &rig.game;
        host.conn = &rig.conn;
        host.unix_time = [] { return std::int64_t{2000000000}; };
        host.chat = &chat;
        host.in_game = host.ui_ready = true;
        app::EqApi api(host);
        ui::lua::LuaUi lua(scripts, settings);
        lua.add_eq_api([&](lua_State* L) { api.register_functions(L); });
        api.load_layout(scratch.path() / "defaults.ini", scratch.path() / "UI_Test_Server.ini", {});
        api.show("FellowshipWnd", true);
        REQUIRE(api.list_rows("FellowshipWnd", "CampsiteKitList").size() == 1);
        REQUIRE(api.click_list("FellowshipWnd", "CampsiteKitList", 0, 0));
        const auto description = api.child_text("FellowshipWnd", "CampsiteDescription");
        REQUIRE(description);
        REQUIRE(description->find("<c \"#11aa22\">") != std::string::npos);
        for (const auto& size : {ImVec2(1600, 900), ImVec2(960, 540)}) {
            for (int i = 0; i < 3; ++i) {
                ImGui::GetIO().DisplaySize = size;
                ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
                api.begin_frame();
                ImGui::NewFrame();
                lua.frame();
                ImGui::Render();
            }
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(api.list_rows("FellowshipWnd", "MemberList").size() == 64);
            CHECK(api.replaces("FellowshipWnd"));
        }
      }
    }
}
