// The shipped UIs over Dear ImGui, part 4 (part 1, tests/ui_pack_windows_test.cpp, says what they cover).
#include <doctest/doctest.h>

#include "ui_pack_session.h"

// The modern windows whose per-frame reading was cut down (the reads kept to what changed, the tables made once)
// draw without script errors when shown.
TEST_CASE("UI packs (modern): the inventory, loot, trade, raid, guild, merchant, map and their kin draw") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_data_windows_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    // a small atlas for the map's World view
    fs::create_directories(dir / "game" / "Atlas" / "Default");
    std::ofstream(dir / "game" / "Atlas" / "Default" / "Default.xml")
        << "<Label Text = \"Antonica\" X = 5 Y = 6/><Link ClickAction = 1 FileToOpen = \"Q.xml\" ToolTip = \"Q\">"
           "<Rect X = 0 Y = 0 CX = 50 CY = 50/></Link><Link ClickAction = 2 FileToOpen = \"qeynos2\" ToolTip = \"North Qeynos\">"
           "<Rect X = 60 Y = 0 CX = 50 CY = 50/></Link>";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        s.frames(2);
        for (const char* screen : {"InventoryWindow", "LootWnd", "TradeWnd", "RaidWindow", "GuildManagementWnd",
                                   "MerchantWnd", "MapViewWnd", "AuraWindow", "PotionBeltWnd", "Bag1",
                                   "MarketplaceWnd", "PointMerchantWnd", "MMTW_MerchantWnd", "MMGW_ManageWnd",
                                   "MIW_InfoWnd", "SpellDisplayWindow"}) {
            INFO(screen);
            REQUIRE(luaL_dostring(L, (std::string("hs.ShowWindow('") + screen + "', true)").c_str()) == LUA_OK);
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            REQUIRE(luaL_dostring(L, (std::string("hs.ShowWindow('") + screen + "', false)").c_str()) == LUA_OK);
        }
        // the map's World view (the atlas and the zone finder, lib/atlas.lua) over the small atlas above, then a search
        // and a route followed (the banner over the Zone view)
        // (a window's stored title changes only in some cases, so the World view is told by the zone finder's list)
        auto finder_drawn = [] {
            for (ImGuiWindow* win : GImGui->Windows)
                if (std::string_view(win->Name).find("##zonelist") != std::string_view::npos && win->Active) return true;
            return false;
        };
        REQUIRE(luaL_dostring(L, "ui.SetSetting('map.view', 'world') hs.ShowWindow('MapViewWnd', true)") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK(finder_drawn());
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('MapViewWnd', false)") == LUA_OK);
        s.frames(1);
        REQUIRE(luaL_dostring(L, "ui.SetSetting('atlas.route', 'qeytoqrg') ui.SetSetting('map.view', 'zone')"
                                 " hs.ShowWindow('MapViewWnd', true)") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK_FALSE(finder_drawn()); // the Zone view
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('MapViewWnd', false) ui.SetSetting('atlas.route', '') ui.SetSetting('map.view', '')") == LUA_OK);
        s.frames(1);
        // the dialogs a test setting draws though the game has not opened them (their opts.shown, set each frame)
        REQUIRE(luaL_dostring(L, "ui.SetSetting('test.draw.MarketplaceWnd', '1') ui.SetSetting('test.draw.ZonePathWnd', '1')"
                                 " ui.SetSetting('test.draw.PointMerchantWnd', '1')") == LUA_OK);
        s.frames(2);
        CHECK(ImGui::FindWindowByName("Zone Path###modern.ZonePathWnd") != nullptr);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(luaL_dostring(L, "ui.SetSetting('test.draw.MarketplaceWnd', '') ui.SetSetting('test.draw.ZonePathWnd', '')"
                                 " ui.SetSetting('test.draw.PointMerchantWnd', '')") == LUA_OK);
        s.frames(2);
        ImGuiWindow* zp = ImGui::FindWindowByName("Zone Path###modern.ZonePathWnd");
        CHECK((zp == nullptr || !zp->WasActive));
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs: both guild banner pages draw the catalog and appearance editor") {
    const auto ui_source = fs::path(__FILE__).parent_path().parent_path() / "ui";
    for (const std::string pack : {"modern", "gamepad"}) {
        INFO(pack);
        hailstone::test::ScratchDir scratch("ui_pack_guild_banners");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(ui_source / pack, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
            s.frames(2);
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, R"lua(
                local g = 'GuildManagementWnd'
                local shown, text, info, row = hs.ChildShown, hs.WindowText, hs.ListInfo, hs.ListRow
                fixture = { editing = false, lists = 0, layers = 0 }
                hs.ChildShown = function(win, child, ...)
                    if win == g and child == 'SaveBannerSettings' then return fixture.editing end
                    return shown(win, child, ...)
                end
                hs.WindowText = function(win, child, ...)
                    if win == g then
                        if child == 'GuildName' then return 'Fixture guild' end
                        if child == 'BannerDescription' then return 'Fixture description' end
                        if child == 'BannerBackgroundPattern' or child == 'BannerEmblemPattern' then
                            fixture.layers = fixture.layers + 1
                            return '1'
                        end
                    end
                    return text(win, child, ...)
                end
                hs.ListInfo = function(win, child, ...)
                    if win == g and child == 'BannerKitList' then
                        fixture.lists = fixture.lists + 1
                        return 1, 2, 0, {'', 'Banner'}, {0, 400}, false
                    end
                    return info(win, child, ...)
                end
                hs.ListRow = function(win, child, ...)
                    if win == g and child == 'BannerKitList' then return {'', 'Fixture kit'}, 1, 1, 1 end
                    return row(win, child, ...)
                end
                hs.SetTabPage(g, 'Subwindows', 4)
                hs.ShowWindow(g, true)
            )lua") == LUA_OK);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            REQUIRE(luaL_dostring(L, "assert(fixture.lists > 0); fixture.editing = true") == LUA_OK);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(luaL_dostring(L, "assert(fixture.layers > 0)") == LUA_OK);
        }
        ImGui::DestroyContext(ctx);
    }
}

// The raid window's members move between groups for the raid leader: a member dragged onto another group's card
// in an unlocked raid locks it first (the game moves members only in a locked raid, CRaidWnd::UpdateButtons) and is
// moved once the lock comes back; in a locked raid at once; a full group is refused. Each is the stock window's row
// click and group button, so what goes out is CRaid_SendMoveGroup's OP_RaidInvite {6, leader, member, group}.
TEST_CASE("UI packs (modern): the raid leader drags a member onto another group") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_raid_move_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        hailstone::test::Rig rig;
        REQUIRE(rig.to_zone());
        auto& conn = rig.conn;
        s.host.conn = &conn;
        const std::string me = conn.character_name();
        auto raid = [&](std::uint32_t action, const std::string& player, std::uint32_t group = 0) {
            net::RaidUpdate u;
            u.general.action = action;
            u.general.player_name = player;
            u.general.leader_name = me;
            u.general.parameter = group;
            u.class_ = 1;
            u.level = 50;
            conn.receive_zone_packet_for_test(net::encode_raid_update(u));
        };
        raid(net::kRaidCreate, me);
        raid(net::kRaidAddMember, me, 0);
        raid(net::kRaidAddMember, "Alpha", 0);
        raid(net::kRaidAddMember, "Bravo", 1);
        for (int i = 1; i <= 6; ++i) raid(net::kRaidAddMember, "Full" + std::to_string(i), 3); // group 4 is full
        REQUIRE(conn.raid().members.size() == 9);
        REQUIRE(conn.raid().leader == me);
        // OP_RaidInvite's raid commands so far: {action, the member (leader_name), parameter}
        auto commands = [&] {
            std::vector<std::tuple<std::uint32_t, std::string, std::uint32_t>> v;
            for (const auto& p : rig.server.zone_log) {
                if (p.opcode != net::to_wire(net::Op::RaidInvite)) continue;
                ByteReader r(p.data);
                std::uint32_t action = r.le<std::uint32_t>();
                r.fixed_string(64);
                r.skip(4);
                std::string member = r.fixed_string(64);
                v.emplace_back(action, member, r.le<std::uint32_t>());
            }
            return v;
        };
        auto count = [&](std::uint32_t action, const std::string& member, std::uint32_t parameter) {
            int n = 0;
            for (const auto& [a, m, p] : commands()) n += a == action && m == member && p == parameter;
            return n;
        };

        lua_State* L = s.lua.state();
        auto run = [&](const char* code) {
            INFO(code);
            REQUIRE(luaL_dostring(L, code) == LUA_OK);
        };
        // every text drawn, with where (T.Text ends in ui.DrawText(x, y, r, g, b, a, text))
        run(R"lua(
            drawn = {}
            local DrawText = ui.DrawText
            ui.DrawText = function(...)
              local x, y, text = select(1, ...), select(2, ...), select(7, ...)
              if type(text) == "string" then drawn[text] = { x, y } end
              return DrawText(...)
            end
            ui.SetSetting('hud.size.RaidWindow', '1200,640')
            ui.SetSetting('test.pos.RaidWindow', '20,20')
            hs.ShowWindow('RaidWindow', true)
        )lua");
        s.frames(4);
        REQUIRE(ImGui::FindWindowByName("UI errors") == nullptr);
        auto at = [&](const char* text) -> ImVec2 {
            lua_getglobal(L, "drawn");
            lua_getfield(L, -1, text);
            ImVec2 p(-1, -1);
            if (lua_istable(L, -1)) {
                lua_rawgeti(L, -1, 1);
                lua_rawgeti(L, -2, 2);
                p = ImVec2(static_cast<float>(lua_tonumber(L, -2)), static_cast<float>(lua_tonumber(L, -1)));
                lua_pop(L, 2);
            }
            lua_settop(L, 0);
            return p;
        };
        ImGuiIO& io = ImGui::GetIO();
        auto drag = [&](ImVec2 from, ImVec2 to) {
            io.AddMousePosEvent(from.x, from.y);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            for (int i = 1; i <= 10; ++i) {
                io.AddMousePosEvent(from.x + (to.x - from.x) * static_cast<float>(i) / 10,
                                    from.y + (to.y - from.y) * static_cast<float>(i) / 10);
                s.frames(1);
            }
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            io.AddMousePosEvent(-1000, -1000);
            s.frames(2);
        };
        auto name = [&](const char* text) {
            ImVec2 p = at(text);
            REQUIRE(p.x >= 0);
            return ImVec2(p.x + 4, p.y + 4);
        };
        // (a point inside group g's card: under its header)
        auto card = [&](const char* header) {
            ImVec2 p = at(header);
            REQUIRE(p.x >= 0);
            return ImVec2(p.x + 20, p.y + 40);
        };

        // /raidwin (/raidwindow's prefix) asks the app for the raid window, which toggles it as the raid key does
        conn.send_command("/raidwin");
        CHECK(conn.take_window_toggle() == std::optional<std::string>("raid"));

        // unlocked: the drop locks the raid; nothing moves until the lock comes back
        CHECK(at("The game moves players only while the raid is locked. Dragging a player locks it for you.").x >= 0);
        drag(name("Alpha"), card("Group 3"));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(rig.until([&] { return count(net::kRaidCmdLock, me, 0) == 1; }));
        CHECK(count(net::kRaidMoveGroup, "Alpha", 2) == 0);
        raid(net::kRaidLock, me);
        s.frames(2);
        REQUIRE(rig.until([&] { return count(net::kRaidMoveGroup, "Alpha", 2) == 1; }));
        CHECK(conn.raid().target == "Alpha");
        // EQEmu's answer (Raid::SendRaidMoveAll): the member removed and added again in its new group
        raid(net::kRaidRemoveMember, "Alpha");
        raid(net::kRaidAddMember, "Alpha", 2);
        s.frames(3);

        // locked: a drop moves at once, out of the groups too (parameter -1)
        drag(name("Bravo"), card("Group 5"));
        REQUIRE(rig.until([&] { return count(net::kRaidMoveGroup, "Bravo", 4) == 1; }));
        drag(name("Alpha"), card("Not in a group"));
        REQUIRE(rig.until([&] { return count(net::kRaidMoveGroup, "Alpha", 0xffffffffu) == 1; }));
        // a full group takes no one
        drag(name("Bravo"), card("Group 4"));
        rig.run_for(std::chrono::milliseconds(100));
        CHECK(count(net::kRaidMoveGroup, "Bravo", 3) == 0);
        CHECK(count(net::kRaidCmdLock, me, 0) == 1); // (locked once only)
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs: raid member context menus and leadership hotkeys are reachable through drawn controls") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        const bool modern = std::string_view(pack) == "modern";
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir scratch("ui_pack_raid_leadership_test");
        fs::path dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        std::ofstream(dir / "UI_Test_Server.ini") << "[Tips]\nShowTips=0\nShowTOTD=0\n";
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
            Config gamepad_config;
            app::Gamepad gamepad(gamepad_config, {});
            s.host.gamepad = &gamepad;
            auto hot_buttons = std::make_unique<game::HotButtonStore>();
            s.host.hot_buttons = hot_buttons.get();
            s.host.set_hot_button = [&](int bar, int button, const game::HotButton& value) {
                hot_buttons->current(bar, button) = value;
            };
            hailstone::test::Rig rig;
            REQUIRE(rig.to_zone());
            auto& conn = rig.conn;
            s.host.conn = &conn;
            const std::string me = conn.character_name();
            net::RaidUpdate raid;
            raid.general.action = net::kRaidAddMember;
            raid.general.leader_name = me;
            raid.class_ = 1;
            raid.level = 60;
            for (int i = 0; i < 18; ++i) {
                raid.general.player_name = i ? "Member" + std::to_string(i) : me;
                raid.general.parameter = static_cast<std::uint32_t>(i / 6);
                conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            }
            conn.set_raid_target("Member1");
            lua_State* L = s.lua.state();
            auto run = [&](const std::string& code) {
                INFO(code);
                REQUIRE(luaL_dostring(L, code.c_str()) == LUA_OK);
            };
            // Record the real controls' rectangles; clicks below are mouse input, never direct ClickChild calls.
            run(R"lua(
                controls, memberPoints = {}, {}
                local button, invisible, menuItem, drawText = ui.Button, ui.InvisibleButton, ui.MenuItem, ui.DrawText
                local function record(id)
                  controls[id] = { ui.GetItemRect() }
                end
                ui.Button = function(id, ...)
                  local clicked = button(id, ...)
                  record(id)
                  return clicked
                end
                ui.InvisibleButton = function(id, ...)
                  local clicked = invisible(id, ...)
                  record(id)
                  return clicked
                end
                ui.MenuItem = function(label, ...)
                  local clicked = menuItem(label, ...)
                  controls['menu.' .. label] = { ui.GetItemRect() }
                  return clicked
                end
                ui.DrawText = function(...)
                  local x, y, text = select(1, ...), select(2, ...), select(7, ...)
                  if text == 'Member2' then memberPoints[text] = {x + 3, y + 3} end
                  if text == 'Main Assist' or text == 'Mark NPC' then
                    controls['menu.' .. text] = { ui.GetItemRect() }
                  end
                  return drawText(...)
                end
                ui.SetSetting('hud.size.RaidWindow', '1000,700')
                ui.SetSetting('test.pos.RaidWindow', '20,20')
                hs.ShowWindow('RaidWindow', true)
            )lua");
            s.frames(4);
            // The gamepad pack's initial help tip covers the action column; dismiss unrelated onboarding first.
            run("hs.ShowWindow('TipWindow', false)");
            s.frames(2);
            auto rectangle = [&](const std::string& id) {
                lua_getglobal(L, "controls");
                lua_getfield(L, -1, id.c_str());
                REQUIRE(lua_istable(L, -1));
                ImVec4 r;
                float* fields[] = {&r.x, &r.y, &r.z, &r.w};
                for (int i = 0; i < 4; ++i) {
                    lua_rawgeti(L, -1, i + 1);
                    *fields[i] = static_cast<float>(lua_tonumber(L, -1));
                    lua_pop(L, 1);
                }
                lua_settop(L, 0);
                return r;
            };
            auto mouse_click = [&](ImVec2 at, int button) {
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent(at.x, at.y);
                s.frames(1);
                io.AddMouseButtonEvent(button, true);
                s.frames(1);
                io.AddMouseButtonEvent(button, false);
                s.frames(2);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            };
            auto click = [&](const std::string& id) {
                const ImVec4 r = rectangle(id);
                mouse_click(ImVec2((r.x + r.z) / 2, (r.y + r.w) / 2), ImGuiMouseButton_Left);
            };
            auto member_menu = [&] {
                lua_getglobal(L, "memberPoints");
                lua_getfield(L, -1, "Member2");
                REQUIRE(lua_istable(L, -1));
                lua_rawgeti(L, -1, 1);
                const float x = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
                lua_rawgeti(L, -1, 2);
                const float y = static_cast<float>(lua_tonumber(L, -1));
                lua_settop(L, 0);
                mouse_click(ImVec2(x, y), ImGuiMouseButton_Right);
                REQUIRE(s.host.menus.size() == 1);
                REQUIRE_FALSE(s.host.menus[0].rows.empty());
                CHECK(s.host.menus[0].rows.front().text == "Member2");
            };
            const std::array<const char*, 3> control_ids = modern ? std::array<const char*, 3>{
                     "Invite Expedition##RAID_InviteExpedButton", "Invite Task##RAID_InviteTaskButton",
                     "Member actions##RAID_MemberActionsButton"}
                                        : std::array<const char*, 3>{"##top6", "##top7", "##memberactions"};
            for (const char* id : control_ids)
                CHECK(rectangle(id).z > 0);
            run(modern ? "assert(controls['Main Assist##RAID_MainAssistButton'] == nil)"
                       : "assert(controls['##assist'] == nil)");
            auto delegates = [&] {
                std::vector<net::DelegateAbility> result;
                for (const auto& p : rig.server.zone_log)
                    if (p.opcode == net::to_wire(net::Op::RaidDelegateAbility))
                        if (auto d = net::decode_delegate_ability(p)) result.push_back(*d);
                return result;
            };
            // A different member was selected first: opening the menu must use the row under the right click.
            member_menu();
            const auto disabled_assist = std::find_if(s.host.menus[0].rows.begin(), s.host.menus[0].rows.end(),
                                                     [](const auto& row) { return row.text == "Main Assist"; });
            REQUIRE(disabled_assist != s.host.menus[0].rows.end());
            CHECK_FALSE(disabled_assist->enabled);
            CHECK(delegates().empty());
            s.host.menus.clear();
            s.frames(2);
            raid = {};
            raid.general.action = net::kRaidSetLeader;
            raid.general.player_name = raid.general.leader_name = me;
            raid.leadership[16] = 3;
            raid.leadership[0x13] = raid.leadership[0x14] = raid.leadership[0x1c] = 1;
            conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            s.frames(2);
            CHECK(conn.leadership_ability_active(0x13, 1));
            CHECK(conn.raid().leader == me);
            conn.set_raid_target("Member1");
            member_menu();
            REQUIRE(std::any_of(s.host.menus[0].rows.begin(), s.host.menus[0].rows.end(), [](const auto& row) {
                return row.text == "Main Assist" && row.enabled;
            }));
            click("menu.Main Assist");
            REQUIRE(rig.until([&] { return delegates().size() == 1; }));
            CHECK(delegates()[0].ability == 3);
            CHECK(delegates()[0].name == "Member2");
            member_menu();
            click("menu.Mark NPC");
            REQUIRE(rig.until([&] { return delegates().size() == 2; }));
            CHECK(delegates()[1].ability == 4);
            CHECK(delegates()[1].name == "Member2");
            click(modern ? "Member actions##RAID_MemberActionsButton" : "##memberactions");
            REQUIRE(s.host.menus.size() == 1);
            CHECK(s.host.menus[0].rows.front().text == "Member2");
            s.host.menus.clear();
            s.frames(2);

            // Disabling context menus restores direct member controls in each shipped pack.
            conn.set_client_option_for_session(game::ClientOption::ContextMenus, 0);
            run("controls = {}");
            s.frames(3);
            run(modern ? "assert(controls['Member actions##RAID_MemberActionsButton'] == nil)"
                       : "assert(controls['##memberactions'] == nil)");
            click(modern ? "Main Assist##RAID_MainAssistButton" : "##assist");
            REQUIRE(rig.until([&] { return delegates().size() == 3; }));
            CHECK(delegates()[2].ability == 3);
            CHECK(delegates()[2].name == "Member2");
            click(modern ? "Mark NPC##RAID_MarkNPCButton" : "##marker");
            REQUIRE(rig.until([&] { return delegates().size() == 4; }));
            CHECK(delegates()[3].ability == 4);
            CHECK(delegates()[3].name == "Member2");
            raid = {};
            raid.general.action = net::kRaidSetLootType;
            raid.general.parameter = 3;
            conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            if (!modern) {
                const ImVec4 r = rectangle("##assist");
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent((r.x + r.z) / 2, (r.y + r.w) / 2);
                s.frames(1);
                io.AddMouseWheelEvent(0, -8);
                s.frames(3);
            }
            click(modern ? "Add Looter##RAID_AddLooterButton" : "##addlooter");
            const auto add_looter = net::encode(net::RaidGeneral{net::kRaidCmdAddLooter, me, "Member2"});
            REQUIRE(rig.until([&] {
                return std::any_of(rig.server.zone_log.begin(), rig.server.zone_log.end(), [&](const auto& packet) {
                    return packet.opcode == add_looter.opcode && packet.data == add_looter.data;
                });
            }));
            conn.set_client_option_for_session(game::ClientOption::ContextMenus, 1);

            run("hs.ShowWindow('RaidWindow', false) hs.ShowWindow('LeadershipWindow', true)"
                " hs.ListInfo('LeadershipWindow', 'LEList2') hs.ListClick('LeadershipWindow', 'LEList2', 0, 'left')");
            s.frames(20); // let the gamepad window's focus-settling interval finish before pressing its actions
            {
                // Purchase is offered with any point, below the RoF2 cost too: the server's costs decide.
                net::LeadershipExp points;
                points.raid_points = 1;
                conn.receive_zone_packet_for_test(net::encode(points));
                run("assert(hs.ChildState('LeadershipWindow', 'LEW_PurchaseButton') == true)");
                points.raid_points = 0;
                conn.receive_zone_packet_for_test(net::encode(points));
                run("assert(hs.ChildState('LeadershipWindow', 'LEW_PurchaseButton') == false)");
            }
            for (int n = 1; n <= 3; ++n) {
                CAPTURE(n);
                const std::string id = modern ? "Create Hotkey " + std::to_string(n) + "##LEW_HotkeyButton" + std::to_string(n)
                                             : "##LeadershipWindowLEW_HotkeyButton" + std::to_string(n);
                run("assert(hs.ChildState('LeadershipWindow', 'LEW_HotkeyButton" + std::to_string(n) + "'))");
                click(id);
                REQUIRE(s.host.cursor_button);
                CHECK(s.host.cursor_button->type == game::HotButtonType::Leadership);
                CHECK(s.host.cursor_button->slot == 16 * 8 + n);
                if (modern) {
                    s.host.cursor_button.reset();
                } else {
                    run("assert(GPSTATE.assign and GPSTATE.assign.from == 'Leadership')");
                    s.frames(20);
                    click("##slot"); // the final cross-hotbar slot drawn by the assignment picker
                    CHECK_FALSE(s.host.cursor_button);
                    run("assert(not GPSTATE.assign)");
                    CHECK(hot_buttons->current(0, 7).type == game::HotButtonType::Leadership);
                    CHECK(hot_buttons->current(0, 7).slot == 16 * 8 + n);
                    s.frames(20);
                }
            }
            // A passive selection disables all three creation controls instead of making inert hot buttons.
            run("hs.ListInfo('LeadershipWindow', 'LEList2') hs.ListClick('LeadershipWindow', 'LEList2', 1, 'left')");
            s.frames(2);
            click(modern ? "Create Hotkey 1##LEW_HotkeyButton1" : "##LeadershipWindowLEW_HotkeyButton1");
            CHECK_FALSE(s.host.cursor_button);
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: leadership names follow received ranks, purchases and training eligibility") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        const bool modern = std::string_view(pack) == "modern";
        test::ScratchDir scratch("ui_pack_leadership_colors");
        const fs::path dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        std::ofstream(dir / "UI_Test_Server.ini") << "[Tips]\nShowTips=0\nShowTOTD=0\n";
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
            Config config;
            app::Gamepad gamepad(config, {});
            platform::GamepadState pad;
            pad.connected = true;
            gamepad.set_state(&pad);
            gamepad.set_mode(app::PadMode::Menu);
            s.host.gamepad = &gamepad;
            test::Rig rig;
            REQUIRE(rig.to_zone());
            auto& conn = rig.conn;
            s.host.conn = &conn;
            const std::string me = conn.character_name();
            auto owned = [&](std::uint32_t ability, std::uint32_t rank) {
                ByteWriter w;
                w.le(ability);
                w.le(rank);
                w.le<std::uint32_t>(0);
                conn.receive_zone_packet_for_test(net::AppPacket(net::to_wire(net::Op::UpdateLeadershipAA), w.take()));
            };
            for (const std::uint32_t ability : {0u, 1u, 16u, 17u}) owned(ability, 1);
            owned(0, 3);
            auto group = [&](int count, const std::string& leader) {
                net::GroupUpdateB u;
                u.leader = leader;
                for (int i = 0; i < count; ++i) {
                    net::GroupUpdateMember member;
                    member.slot = static_cast<std::uint32_t>(i);
                    member.name = i ? "Friend" + std::to_string(i) : me;
                    u.members.push_back(member);
                }
                conn.receive_zone_packet_for_test(net::encode(u));
            };
            net::GroupLeadershipUpdate active_group;
            active_group.ranks[0] = active_group.ranks[1] = active_group.ranks[8] = 1;
            group(2, "Friend1");
            conn.receive_zone_packet_for_test(net::encode(active_group));
            auto* L = s.lua.state();
            auto run = [&](const std::string& code) {
                const int rc = luaL_dostring(L, code.c_str());
                INFO(code);
                INFO((rc == LUA_OK ? "" : lua_tostring(L, -1)));
                REQUIRE(rc == LUA_OK);
                lua_settop(L, 0);
            };
            run(R"lua(
                leadershipDrawn = {}
                local draw = ui.DrawText
                ui.DrawText = function(x, y, r, g, b, a, text, ...)
                    if type(text) == 'string' then leadershipDrawn[text] = {r, g, b} end
                    return draw(x, y, r, g, b, a, text, ...)
                end
                function checkLeadershipColor(list, name, r, g, b, rendered)
                    local count = hs.ListInfo('LeadershipWindow', list)
                    local found
                    for i = 0, count - 1 do
                        local row, cr, cg, cb = hs.ListRow('LeadershipWindow', list, i)
                        if row[1] == name then found = {cr, cg, cb} break end
                    end
                    assert(found, 'Missing leadership row: ' .. name)
                    local function check(c)
                        assert(math.abs(c[1] - r) < 0.001 and math.abs(c[2] - g) < 0.001
                            and math.abs(c[3] - b) < 0.001, 'Unexpected leadership color: ' .. name)
                    end
                    check(found)
                    if rendered then
                        assert(leadershipDrawn[name], 'Leadership name was not drawn: ' .. name)
                        check(leadershipDrawn[name])
                    end
                end
                ui.SetSetting('hud.size.LeadershipWindow', '1000,700')
                ui.SetSetting('test.pos.LeadershipWindow', '20,20')
                hs.ShowWindow('LeadershipWindow', true)
            )lua");
            auto render = [&] {
                run("leadershipDrawn = {}");
                s.frames(3);
                REQUIRE(ImGui::FindWindowByName("UI errors") == nullptr);
            };
            auto gold = [&](const char* list, const char* name) {
                run(std::string("checkLeadershipColor('") + list + "', '" + name +
                    "', 214 / 255, 178 / 255, 40 / 255, true)");
            };
            auto green = [&](const char* list, const char* name) {
                run(std::string("checkLeadershipColor('") + list + "', '" + name + "', 0, 1, 0, true)");
            };
            render();
            CHECK_FALSE(conn.leadership_ability_active(1, 1));
            green("LEList1", "NPC Health");
            green("LEList1", "Mark NPC"); // leader rank 1 suffices even when the personal purchase is rank 3
            group(3, "Friend1");
            render();
            CHECK(conn.leadership_ability_active(1, 1));
            CHECK_FALSE(conn.leadership_ability_active(0, 1));
            green("LEList1", "NPC Health");
            green("LEList1", "Mark NPC"); // the window's color does not check the current member's role
            const auto profile = *conn.profile();
            auto without_god = profile;
            without_god.expansions &= ~0x40u;
            conn.set_profile_for_test(without_god);
            render();
            CHECK_FALSE(conn.leadership_ability_active(1, 1));
            green("LEList1", "NPC Health");
            conn.set_profile_for_test(profile);
            // The current leader's ranks take precedence even without a personal purchase.
            run("checkLeadershipColor('LEList1', 'Spell Awareness', 0, 1, 0, false)");
            conn.receive_zone_packet_for_test(net::encode(net::GroupLeadershipUpdate{}));
            render();
            gold("LEList1", "NPC Health");
            gold("LEList1", "Mark NPC");
            run("checkLeadershipColor('LEList1', 'Spell Awareness', 192 / 255, 192 / 255, 192 / 255, false)");
            owned(1, 0);
            render();
            run("checkLeadershipColor('LEList1', 'NPC Health', 192 / 255, 192 / 255, 192 / 255, true)");
            net::LeadershipExp points;
            points.group_points = 10;
            conn.receive_zone_packet_for_test(net::encode(points));
            render();
            run("checkLeadershipColor('LEList1', 'NPC Health', 1, 1, 1, true)");
            run("checkLeadershipColor('LEList1', 'Delegate Mark NPC', 1, 1, 1, true)");
            owned(0, 0);
            conn.receive_zone_packet_for_test(net::encode(points));
            render();
            run("checkLeadershipColor('LEList1', 'Delegate Mark NPC', 192 / 255, 192 / 255, 192 / 255, true)");
            owned(0, 0xffffffffu);
            conn.receive_zone_packet_for_test(net::encode(points));
            render();
            run("checkLeadershipColor('LEList1', 'Mark NPC', 192 / 255, 192 / 255, 192 / 255, true)");
            run("checkLeadershipColor('LEList1', 'Delegate Mark NPC', 192 / 255, 192 / 255, 192 / 255, true)");
            owned(1, 1);
            render();
            gold("LEList1", "NPC Health");
            conn.receive_zone_packet_for_test(net::encode(active_group));
            group(2, "Friend1");
            render();
            CHECK_FALSE(conn.leadership_ability_active(1, 1));
            green("LEList1", "NPC Health");
            conn.receive_zone_packet_for_test(net::encode_group_generic(net::Op::GroupDisbandYou, {me, me}));
            render();
            gold("LEList1", "NPC Health");

            net::RaidUpdate raid;
            raid.general.action = net::kRaidAddMember;
            raid.general.leader_name = "Raider1";
            for (int i = 0; i < 17; ++i) {
                raid.general.player_name = i ? "Raider" + std::to_string(i) : me;
                conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            }
            raid = {};
            raid.general.action = net::kRaidSetLeader;
            raid.general.leader_name = "Raider1";
            raid.leadership[16] = raid.leadership[17] = 1;
            conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            if (modern) {
                std::optional<ImVec2> at;
                ImGuiContext& g = *ImGui::GetCurrentContext();
                for (int i = 0; i < g.TabBars.GetMapSize() && !at; ++i)
                    if (ImGuiTabBar* bar = g.TabBars.TryGetMapData(i))
                        for (ImGuiTabItem& tab : bar->Tabs)
                            if (std::string_view(ImGui::TabBarGetTabName(bar, &tab)) == "Raid abilities")
                                at = ImVec2(bar->BarRect.Min.x + tab.Offset + tab.Width / 2,
                                            (bar->BarRect.Min.y + bar->BarRect.Max.y) / 2);
                REQUIRE(at);
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent(at->x, at->y);
                s.frames(1);
                io.AddMouseButtonEvent(0, true);
                s.frames(1);
                io.AddMouseButtonEvent(0, false);
            } else {
                run("ui.SetWindowFocus('Leadership')");
                pad.pressed[static_cast<std::size_t>(platform::PadButton::RB)] = true;
                s.frames(1);
                pad.pressed.fill(false);
            }
            render();
            CHECK_FALSE(conn.leadership_ability_active(17, 1));
            green("LEList2", "NPC Health");
            green("LEList2", "Mark NPC");
            raid = {};
            raid.general.action = net::kRaidAddMember;
            raid.general.player_name = "Raider17";
            conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            render();
            CHECK(conn.leadership_ability_active(17, 1));
            CHECK_FALSE(conn.leadership_ability_active(16, 1));
            green("LEList2", "NPC Health");
            green("LEList2", "Mark NPC");
            raid.general.action = net::kRaidRemoveMember;
            conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            render();
            CHECK_FALSE(conn.leadership_ability_active(17, 1));
            green("LEList2", "NPC Health");
            raid.general.action = net::kRaidDisband;
            conn.receive_zone_packet_for_test(net::encode_raid_update(raid));
            render();
            gold("LEList2", "NPC Health");
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs (modern): the Options window's Textures section picks the texture pack and starts the studio") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_textures_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        std::set<std::string> asked;
        s.host.find_option = [&](std::string_view o) -> std::optional<int> {
            asked.insert(std::string(o));
            if (o == "texture_pack") return 2;
            if (o == "texture_studio") return 1;
            if (o == "upscaler") return 4; // the first upscaler add-on (render/temporal.h kFirstProviderChoice)
            return std::nullopt;
        };
        std::set<std::string> listed;
        s.host.option_choices = [&listed](std::string_view o) {
            listed.insert(std::string(o));
            if (o == "upscaler_providers") return std::vector<std::string>{"Intel XeSS"};
            return o == "texture_pack" ? std::vector<std::string>{"Off", "default", "hd"} : std::vector<std::string>{};
        };
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('OptionsWindow', true)") == LUA_OK);
        s.frames(3);
        // a click on the Graphics tab (found in the window's tab bar)
        ImGuiContext& g = *ImGui::GetCurrentContext();
        std::optional<ImVec2> at;
        for (int i = 0; i < g.TabBars.GetMapSize() && !at; ++i)
            if (ImGuiTabBar* bar = g.TabBars.TryGetMapData(i))
                for (ImGuiTabItem& tab : bar->Tabs)
                    if (std::string_view(ImGui::TabBarGetTabName(bar, &tab)) == "Graphics")
                        at = ImVec2(bar->BarRect.Min.x + tab.Offset + tab.Width / 2, (bar->BarRect.Min.y + bar->BarRect.Max.y) / 2);
        REQUIRE(at);
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at->x, at->y);
        s.frames(1);
        io.AddMouseButtonEvent(0, true);
        s.frames(1);
        io.AddMouseButtonEvent(0, false);
        s.frames(3);
        CHECK(asked.contains("texture_pack"));
        CHECK(asked.contains("texture_studio")); // the Texture Studio button is offered
        // The Upscaler choices go on with the upscaler add-ons installed, one of them chosen.
        CHECK(asked.contains("upscaler"));
        CHECK(listed.contains("upscaler_providers"));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs: Options changes the Find trail style, Tab targeting and respects addon contributions to name plates") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(std::string(pack));
        test::ScratchDir scratch("ui_pack_display_options");
        const fs::path dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        ImGuiContext* prior_context = ImGui::GetCurrentContext();
        auto destroy_context = [prior_context](ImGuiContext* context) {
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(prior_context);
        };
        std::unique_ptr<ImGuiContext, decltype(destroy_context)> ctx(ImGui::CreateContext(), destroy_context);
        ImGui::SetCurrentContext(ctx.get());
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            plugin::UiBridge bridge;
            Session s(dir, pack);
            s.host.plugin_ui = &bridge;
            s.host.plugin_caps = [](std::string_view owner) {
                return owner == "plugin:captions" || owner == "plugin:visibility"
                    ? plugin::kCapUi | plugin::kCapExtend : 0u;
            };
            std::vector<std::pair<std::string, std::string>> actions;
            s.host.submit_chat = [&](const std::string& line) { actions.emplace_back(plugin::caller_name(), line); };
            std::map<std::string_view, int> settings{
                {"find_trail_style", 0}, {"show_invis_undead", 0},
                {"tab_target_through_walls", 0}, {"tab_target_offscreen", 0}};
            std::map<std::string_view, std::vector<int>> changes;
            s.host.find_option = [&](std::string_view name) -> std::optional<int> {
                if (auto it = settings.find(name); it != settings.end()) return it->second;
                return std::nullopt;
            };
            s.host.set_option = [&](std::string_view name, int value) {
                if (auto it = settings.find(name); it != settings.end()) {
                    changes[it->first].push_back(value);
                    it->second = value;
                }
            };
            lua_State* L = s.lua.state();
            // Record real control coordinates without changing their activation or rendering.
            REQUIRE(luaL_dostring(L, R"lua(
                widgets = {}
                -- Keep the fixture's settings and the two notices in the Gamepad list's visible rows.
                local supports = hs.UI.Supports
                hs.UI.Supports = function(module, member)
                    return (module ~= "interfaces" or member ~= "Status") and supports(module, member)
                end
                local combo, selectable, checkbox, text = ui.BeginCombo, ui.Selectable, ui.Checkbox, ui.DrawText
                local button, wrapped = ui.Button, ui.TextWrapped
                local buttons = { ["Open caption settings"] = "action", ["Open visibility settings"] = "visibility action" }
                local notices = { ["captions: Captions are active."] = "section notice",
                    ["captions: Captions own the brackets."] = "captions reason",
                    ["visibility: Visibility owns the brackets."] = "visibility reason" }
                local function record(name)
                    local x0, y0, x1, y1 = ui.GetItemRect()
                    widgets[name] = { (x0 + x1) / 2, (y0 + y1) / 2 }
                end
                ui.Button = function(label, ...)
                    local clicked = button(label, ...)
                    if buttons[label] then record(buttons[label]) end
                    return clicked
                end
                ui.TextWrapped = function(label, ...)
                    wrapped(label, ...)
                    if notices[label] then record(notices[label]) end
                end
                ui.BeginCombo = function(label, ...)
                    local open = combo(label, ...)
                    if label == "##find_trail_style" then record("combo") end
                    return open
                end
                ui.Selectable = function(label, ...)
                    local clicked = selectable(label, ...)
                    local name = label:match("^Rainbow trail##") and "rainbow"
                        or label:match("^Gold line %(default%)##") and "navigation"
                    if name then record(name) end
                    return clicked
                end
                ui.Checkbox = function(label, ...)
                    local value, changed = checkbox(label, ...)
                    if label == "##show_invis_undead" then record("Bracket Invis vs. Undead")
                    elseif label == "##tab_target_through_walls" then record("Target through walls and doors")
                    elseif label == "##tab_target_offscreen" then record("Target off-screen") end
                    return value, changed
                end
                ui.DrawText = function(...)
                    local label = select(7, ...)
                    local key = notices[label] or buttons[label]
                    if key or label == "Interface" or label == "Find trail" or label == "Bracket Invis vs. Undead"
                        or label == "Target through walls and doors" or label == "Target off-screen" then
                        local x, y = ...
                        local w, h = ui.CalcTextSize(label)
                        widgets[key or label] = { x + w / 2, y + h / 2 }
                    end
                    return text(...)
                end
                hs.ShowWindow("OptionsWindow", true)
            )lua") == LUA_OK);
            s.frames(3);
            auto point = [&](const char* name) {
                lua_getglobal(L, "widgets");
                lua_getfield(L, -1, name);
                REQUIRE_MESSAGE(lua_istable(L, -1), std::string(name));
                lua_rawgeti(L, -1, 1);
                const float x = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
                lua_rawgeti(L, -1, 2);
                const float y = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 3);
                return ImVec2(x, y);
            };
            auto click = [&](const ImVec2& at) {
                ImGuiIO& io = ImGui::GetIO();
                io.AddMousePosEvent(at.x, at.y);
                s.frames(1);
                io.AddMouseButtonEvent(0, true);
                s.frames(1);
                io.AddMouseButtonEvent(0, false);
                s.frames(3);
            };
            if (std::string_view(pack) == "modern") {
                std::optional<ImVec2> tab_at;
                ImGuiContext& g = *ImGui::GetCurrentContext();
                for (int i = 0; i < g.TabBars.GetMapSize() && !tab_at; ++i)
                    if (ImGuiTabBar* bar = g.TabBars.TryGetMapData(i))
                        for (ImGuiTabItem& tab : bar->Tabs)
                            if (std::string_view(ImGui::TabBarGetTabName(bar, &tab)) == "Display")
                                tab_at = ImVec2(bar->BarRect.Min.x + tab.Offset + tab.Width / 2,
                                                (bar->BarRect.Min.y + bar->BarRect.Max.y) / 2);
                REQUIRE(tab_at);
                click(*tab_at);
                CHECK(changes.empty());
                click(point("combo"));
                click(point("rainbow"));
                REQUIRE(settings.at("find_trail_style") == 1);
                click(point("combo"));
                click(point("navigation"));
            } else {
                click(point("Interface"));
                CHECK(changes.empty());
                click(point("Find trail"));
                REQUIRE(settings.at("find_trail_style") == 1);
                click(point("Find trail"));
            }
            CHECK(settings.at("find_trail_style") == 0);
            CHECK(changes["find_trail_style"] == std::vector<int>{1, 0});
            CHECK_FALSE(changes.contains("show_invis_undead"));
            click(point("Bracket Invis vs. Undead"));
            CHECK(settings.at("show_invis_undead") == 1);
            click(point("Bracket Invis vs. Undead"));
            CHECK(settings.at("show_invis_undead") == 0);
            CHECK(changes["show_invis_undead"] == std::vector<int>{1, 0});

            REQUIRE(bridge.set_contribution({"captions", "status", "OptionsWindow", "section:nameplates",
                                             "Captions are active.", {}, {}}));
            REQUIRE(bridge.set_contribution({"captions", "brackets", "OptionsWindow", "setting:show_invis_undead",
                                             "Captions own the brackets.", "Open caption settings", "/captions", true}));
            REQUIRE(bridge.set_contribution({"visibility", "brackets", "OptionsWindow", "setting:show_invis_undead",
                                             "Visibility owns the brackets.", "Open visibility settings", "/visibility", true}));
            s.frames(3);
            CHECK(point("section notice").y < point("Bracket Invis vs. Undead").y);
            CHECK(point("captions reason").y < point("visibility reason").y);
            click(point("Bracket Invis vs. Undead"));
            CHECK(changes["show_invis_undead"] == std::vector<int>{1, 0});
            click(point("action"));
            CHECK(actions == std::vector<std::pair<std::string, std::string>>{{"plugin:captions", "/captions"}});

            if (std::string_view(pack) == "modern") {
                click(point("combo"));
                click(point("rainbow"));
            } else {
                click(point("Find trail"));
            }
            CHECK(settings.at("find_trail_style") == 1);

            // Releasing a click after its notice disappears must not run the action that moved into its row.
            const auto removed_action = point("action");
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(removed_action.x, removed_action.y);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            REQUIRE(bridge.remove_contribution("captions", "brackets"));
            s.frames(3);
            io.AddMouseButtonEvent(0, false);
            s.frames(3);
            REQUIRE(actions.size() == 1);
            click(point("visibility action"));
            REQUIRE(actions.size() == 2);
            CHECK(actions.back() == std::pair<std::string, std::string>{"plugin:visibility", "/visibility"});
            click(point("Bracket Invis vs. Undead"));
            CHECK(changes["show_invis_undead"] == std::vector<int>{1, 0});
            REQUIRE(bridge.remove_contribution("visibility", "brackets"));
            s.frames(3);
            click(point("Bracket Invis vs. Undead"));
            CHECK(changes["show_invis_undead"] == std::vector<int>{1, 0, 1});

            CHECK_FALSE(changes.contains("tab_target_through_walls"));
            CHECK_FALSE(changes.contains("tab_target_offscreen"));
            click(point("Target through walls and doors"));
            CHECK(settings.at("tab_target_through_walls") == 1);
            CHECK(settings.at("tab_target_offscreen") == 0);
            if (std::string_view(pack) == "gamepad") {
                // The off-screen option is just below the visible rows at this window size.
                ImGui::GetIO().AddMouseWheelEvent(0, -1);
                s.frames(3);
            }
            click(point("Target off-screen"));
            CHECK(settings.at("tab_target_through_walls") == 1);
            CHECK(settings.at("tab_target_offscreen") == 1);
            click(point("Target through walls and doors"));
            CHECK(settings.at("tab_target_through_walls") == 0);
            CHECK(settings.at("tab_target_offscreen") == 1);
            click(point("Target off-screen"));
            CHECK(settings.at("tab_target_offscreen") == 0);
            CHECK(changes["tab_target_through_walls"] == std::vector<int>{1, 0});
            CHECK(changes["tab_target_offscreen"] == std::vector<int>{1, 0});
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
    }
}

// A bag window resized: as many slots to a row as its width holds, its height grown to fit the rows; unsized, rows
// of five. A click lands on the slot drawn under the mouse (hs.BagSlotClick's index).
TEST_CASE("UI packs (modern): a resized bag window lays its slots out to its width") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_bag_reflow_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, R"(
            bagClicked = nil
            hs.BagInfo = function(win) if win == 'Bag1' then return 'Test Bag', 10, false, 'inventory', 23 end end
            hs.BagSlotItem = function() return nil end
            local WindowShown = hs.WindowShown -- (the game opens a bag window only on a bag it has)
            hs.WindowShown = function(win) if win == 'Bag1' then return true end return WindowShown(win) end
            hs.BagSlotClick = function(win, i, button) if button == 'left' then bagClicked = i end end
            ui.SetSetting('test.pos.Bag1', '100,100')
        )") == LUA_OK);
        s.frames(3);
        REQUIRE(luaL_dostring(L, "return ui.GetIO().uiScale or 1") == LUA_OK);
        const float scale = static_cast<float>(lua_tonumber(L, -1));
        lua_settop(L, 0);
        auto window = [] {
            for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows) {
                if (std::string_view(win->Name).ends_with("###modern.Bag1") && win->WasActive) return win;
            }
            return static_cast<ImGuiWindow*>(nullptr);
        };
        ImGuiIO& io = ImGui::GetIO();
        // the slot clicked at (col, row) of the grid: 44 design pixels and a gap of 5, below the 30 of the toolbar
        auto click = [&](int col, int row) {
            ImGuiWindow* win = window();
            REQUIRE(win);
            const ImVec2 o = win->DC.CursorStartPos;
            const float x = o.x + (static_cast<float>(col) * 49 + 22) * scale;
            const float y = o.y + (30 + static_cast<float>(row) * 49 + 22) * scale;
            REQUIRE(luaL_dostring(L, "bagClicked = nil") == LUA_OK);
            io.AddMousePosEvent(x, y);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            io.AddMouseButtonEvent(0, false);
            s.frames(2);
            io.AddMousePosEvent(-1000, -1000);
            s.frames(1);
            REQUIRE(luaL_dostring(L, "return bagClicked or -1") == LUA_OK);
            const int i = static_cast<int>(lua_tointeger(L, -1));
            lua_settop(L, 0);
            return i;
        };
        auto bottomOfRows = [&](int rows) {
            ImGuiWindow* win = window();
            REQUIRE(win);
            return win->DC.CursorStartPos.y + (30 + static_cast<float>(rows) * 49 - 5) * scale;
        };
        // unsized: two rows of five
        CHECK(click(4, 0) == 4);
        CHECK(click(0, 1) == 5);
        // wide: one row of ten, the window no taller than that needs and still all of it
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.Bag1', '560,60')") == LUA_OK);
        s.frames(3);
        CHECK(click(7, 0) == 7);
        CHECK(click(0, 1) == -1);
        ImGuiWindow* win = window();
        REQUIRE(win);
        CHECK(win->Pos.y + win->Size.y >= bottomOfRows(1));
        CHECK(win->Pos.y + win->Size.y < bottomOfRows(2));
        // narrow (below its least width): four to a row, three rows, the window grown to hold them
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.Bag1', '100,60')") == LUA_OK);
        s.frames(3);
        CHECK(click(3, 0) == 3);
        CHECK(click(1, 2) == 9);
        win = window();
        REQUIRE(win);
        CHECK(win->Pos.y + win->Size.y >= bottomOfRows(3));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// The buff menu (ui/modern/lib/buffmenu.lua): a right click on a target's or the pet's buff opens that buff's menu,
// and its rows act on the buff that was clicked: Spell Info on the target's, Add to Blocked Pet Spells on the pet's,
// and Remove on a removable pet buff still on the same pet and in the same slot.
// The frame's own menu stays shut (the buff took the click); Alt with a right click still opens it.
TEST_CASE("UI packs (modern): a right click on a target's or the pet's buff opens the buff menu") {
    test::ScratchDir scratch("ui_pack_buff_menu_test");
    const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern", dir / "modern",
             fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        auto* L = s.lua.state();
        auto run = [&](const char* code) {
            const int rc = luaL_dostring(L, code);
            INFO((rc == LUA_OK ? "" : lua_tostring(L, -1)));
            REQUIRE(rc == LUA_OK);
            const std::string result = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return result;
        };
        // A target and a pet with one buff each, and the menu's rows recorded: the pick is the row that acts.
        run(R"lua(
targetShown, petShown = true, true
targetIcon, petIcon = nil, nil
menuLabels, menuPick, menuPicked, shownSpell, blocked, removals = {}, nil, nil, nil, {}, {}
petBeneficial, petEntity, petSlot = true, 104, 19
local buffSlot, label, shown, gauge, menuItem, showSpell, setText, clickChild, windowButton =
  hs.BuffSlot, hs.Label, hs.WindowShown, hs.Gauge, ui.MenuItem, hs.ShowSpell, hs.SetWindowText, hs.ClickChild, hs.WindowButton
hs.BuffSlot = function(win, id, overlays, identity)
  if win == "TargetWindow" and id == "Buff0" then return 11, 60000, "Holy Armor", true end
  if win == "PetInfoWindow" and id == "PetBuff0" then
    local removable = identity and petBeneficial
    if petBeneficial then
      return 11, 60000, "Holy Armor", true, 1, removable and petEntity or nil, removable and petSlot or nil
    end
    return 12, 60000, "Test Slow", false, 0
  end
  return buffSlot(win, id, overlays, identity)
end
hs.Label = function(n) if n == 28 then return "a rat" end return label(n) end
hs.WindowShown = function(win)
  if win == "TargetWindow" then return targetShown end
  if win == "PetInfoWindow" then return petShown end
  return shown(win)
end
hs.Gauge = function(k)
  if k == 6 then return 1, 0, 100 end
  if k == 16 then return 1, "Pet", 100 end
  if k == 27 or k == 41 or k == 149 then return 0, "", -1 end
  return gauge(k)
end
hs.ShowSpell = function(id) shownSpell = id end
hs.SetWindowText = function(win, id, text)
  if win == "BlockedPetBuffWnd" then blocked.edit = id .. "=" .. text end
  return setText(win, id, text)
end
hs.ClickChild = function(win, id, button)
  if win == "BlockedPetBuffWnd" then blocked.add = id .. ":" .. tostring(button); return true end
  return clickChild(win, id, button)
end
hs.WindowButton = function(win, id, message)
  if win == "PetInfoWindow" then removals[#removals + 1] = id .. ":" .. tostring(message); return true end
  return windowButton(win, id, message)
end
ui.MenuItem = function(text, ...)
  menuLabels[#menuLabels + 1] = text
  if text == menuPick then menuPicked = text; return true end
  return menuItem(text, ...)
end
local button = ui.InvisibleButton
ui.InvisibleButton = function(id, ...)
  local hit = button(id, ...)
  if id == "##tbBuff0" then targetIcon = { ui.GetItemRect() } end
  if id == "##PetBuff0" then petIcon = { ui.GetItemRect() } end
  return hit
end
)lua");
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(run("return tostring(targetIcon ~= nil and petIcon ~= nil)") == "true");
        auto centre = [&](const char* name) {
            lua_getglobal(L, name);
            float r[4]{};
            for (int i = 0; i < 4; ++i) {
                lua_rawgeti(L, -1, i + 1);
                r[i] = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            lua_settop(L, 0);
            return ImVec2((r[0] + r[2]) / 2, (r[1] + r[3]) / 2);
        };
        // The mouse moves onto the icon and the button goes down in the same frame: the frame before hovered
        // something else, so an icon that lets a later item lie over it would lose the click to the frame.
        auto press = [&](ImVec2 at) {
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(at.x, at.y);
            io.AddMouseButtonEvent(1, true);
            s.frames(1);
        };
        auto release = [&] {
            ImGui::GetIO().AddMouseButtonEvent(1, false);
            s.frames(1);
        };
        auto closeMenu = [&] {
            ImGui::ClosePopupsExceptModals();
            s.frames(1);
            run("menuPick, menuPicked, menuLabels = nil, nil, {}");
        };
        // the target's buff: Spell Info, and the frame's menu stays shut
        run("menuPick = 'Spell Info'");
        press(centre("targetIcon"));
        CHECK(run("return tostring(shownSpell)") == "11");
        CHECK(run("return table.concat(menuLabels, '|')") == "Spell Info");
        run("menuPick = nil");
        release();
        closeMenu();
        // Alt with a right click still opens the frame's own menu
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
        press(centre("targetIcon"));
        CHECK(run("return menuLabels[1]") == "Lock this frame");
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
        release();
        closeMenu();
        // the pet's buff: Add to Blocked Pet Spells, on the pet's own list
        run("menuPick = 'Add to Blocked Pet Spells'");
        press(centre("petIcon"));
        CHECK(run("return blocked.edit") == "BW_EditSpellToBlockFromPet=11");
        CHECK(run("return blocked.add") == "BW_AddSpellToBlockFromPet:left");
        CHECK(run("return table.concat(menuLabels, '|')") == "Spell Info|Add to Blocked Pet Spells|Remove");
        run("menuPick = nil");
        release();
        closeMenu();
        run("menuPick = 'Remove'");
        press(centre("petIcon"));
        CHECK(run("return table.concat(removals, '|')") == "PetBuff0:3");
        run("menuPick = nil");
        release();
        closeMenu();

        // The same spell under the same display icon is a different effect after either change.
        for (const char* change : {"petEntity = 105", "petSlot = 20"}) {
            press(centre("petIcon"));
            CHECK(run("return menuLabels[#menuLabels]") == "Remove");
            run(change);
            run("menuPick = 'Remove'");
            release();
            CHECK(run("return menuPicked") == "Remove");
            CHECK(run("return table.concat(removals, '|')") == "PetBuff0:3");
            closeMenu();
        }

        run("petBeneficial, menuPick = false, 'Remove'");
        press(centre("petIcon"));
        CHECK(run("return table.concat(menuLabels, '|')") == "Spell Info|Add to Blocked Pet Spells");
        CHECK(run("return table.concat(removals, '|')") == "PetBuff0:3");
        release();
        closeMenu();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs (modern): buff tray options preserve the default layout and old slot settings") {
    test::ScratchDir scratch("ui_pack_buff_tray_slots");
    const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern", dir / "modern",
             fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        auto* L = s.lua.state();
        lua_pushcfunction(L, [](lua_State* state) {
            const auto* window = ImGui::GetCurrentWindowRead();
            const std::string_view name = window ? window->Name : "";
            if (name.ends_with("###modern.BuffWindow")) lua_pushliteral(state, "BuffWindow");
            else if (name.ends_with("###modern.ShortDurationBuffWindow")) lua_pushliteral(state, "ShortDurationBuffWindow");
            else lua_pushnil(state);
            return 1;
        });
        lua_setglobal(L, "testTrayWindow");
        auto run = [&](const char* code) {
            const int rc = luaL_dostring(L, code);
            INFO((rc == LUA_OK ? "" : lua_tostring(L, -1)));
            REQUIRE(rc == LUA_OK);
            const std::string result = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return result;
        };
        run(R"lua(
hs.ShowWindow('BuffWindow', true)
hs.ShowWindow('ShortDurationBuffWindow', true)
ui.SetSetting('hud.pos.BuffWindow', '600,160')
ui.SetSetting('hud.pos.ShortDurationBuffWindow', '600,480')
ui.SetSetting('hud.perRow.BuffWindow', 3)
local names = {
  BuffWindow = { Buff2 = 'Spirit of Wolf', Buff4 = 'Holy Armor', Buff5 = 'Clarity' },
  ShortDurationBuffWindow = { Buff0 = 'Chorus of the Singing Grove', Buff1 = 'Psalm' },
}
local slot, button, textColored, menuItem = hs.BuffSlot, ui.InvisibleButton, ui.TextColored, ui.MenuItem
local drawText, drawRect, beginMenu, endMenu = ui.DrawText, ui.DrawRectFilled, ui.BeginMenu, ui.EndMenu
hs.BuffSlot = function(win, id, overlays)
  local name = names[win] and names[win][id]
  if name then return 11, id == 'Buff0' and 43000 or 60000, name, true end
  return slot(win, id, overlays)
end
buffRects, songRects, menuRows = {}, {}, {}
function clearTrayDraws()
  trayText = { BuffWindow = {}, ShortDurationBuffWindow = {} }
  trayPanel = {}
end
clearTrayDraws()
ui.InvisibleButton = function(id, ...)
  local pressed = button(id, ...)
  local n = tonumber(id:match('^##Buff(%d+)$'))
  local tray = n and testTrayWindow()
  if tray == 'BuffWindow' then
    buffRects[n] = { ui.GetItemRect() }
    local _, h = ui.GetWindowSize()
    trayHeight = h
  elseif tray == 'ShortDurationBuffWindow' then
    songRects[n] = { ui.GetItemRect() }
  end
  return pressed
end
ui.TextColored = function(r, g, b, a, label)
  if type(label) == 'string' and label:find('^Spirit of Wolf') then tooltip = label end
  return textColored(r, g, b, a, label)
end
ui.MenuItem = function(label, ...)
  menuRows[label] = true
  if label == pick then
    pick = nil
    ui.CloseCurrentPopup()
    return true
  end
  return menuItem(label, ...)
end
-- Exercise the layout choices in the live frame popup without depending on the submenu's hover delay.
local forcedLayout
ui.BeginMenu = function(label, ...)
  if label == 'Layout' and (pick == 'Vertical' or pick == 'Horizontal') then
    forcedLayout = true
    return true
  end
  return beginMenu(label, ...)
end
ui.EndMenu = function()
  if forcedLayout then forcedLayout = false; return end
  return endMenu()
end
ui.DrawText = function(x, y, r, g, b, a, text, ...)
  local tray = testTrayWindow()
  if tray then trayText[tray][text] = { x, y } end
  return drawText(x, y, r, g, b, a, text, ...)
end
ui.DrawRectFilled = function(x0, y0, x1, y1, ...)
  local tray = testTrayWindow()
  if tray then
    local x, y = ui.GetWindowPos()
    local w, h = ui.GetWindowSize()
    if x0 == x and y0 == y and x1 == x + w and y1 == y + h then trayPanel[tray] = true end
  end
  return drawRect(x0, y0, x1, y1, ...)
end
)lua");
        auto point_at_buff = [&](bool song = false) {
            REQUIRE(luaL_dostring(L, song ? "return songRects[0][1] + 10, songRects[0][2] + 10"
                                          : "return buffRects[2][1] + 10, buffRects[2][2] + 10") == LUA_OK);
            io.AddMousePosEvent(static_cast<float>(lua_tonumber(L, -2)), static_cast<float>(lua_tonumber(L, -1)));
            lua_settop(L, 0);
        };
        auto choose = [&](const char* label, bool song = false) {
            run((std::string("pick = '") + label + "'").c_str());
            point_at_buff(song);
            io.AddKeyEvent(ImGuiMod_Alt, true);
            io.AddMouseButtonEvent(1, true);
            s.frames(1);
            CHECK(run("return tostring(pick == nil)") == "true");
            io.AddMouseButtonEvent(1, false);
            io.AddKeyEvent(ImGuiMod_Alt, false);
            ImGui::ClosePopupsExceptModals();
            run("buffRects = {}");
            s.frames(2);
        };
        auto title = [&] {
            run("tooltip = nil");
            point_at_buff();
            s.frames(2);
            return run("return tooltip");
        };
        s.frames(3);
        REQUIRE(run(R"lua(
local a, b, c = buffRects[2], buffRects[4], buffRects[5]
compactX, compactY, compactHeight, pitch = a[1], a[2], trayHeight, a[1] - b[1]
local d, e = songRects[0], songRects[1]
return tostring(pitch == 38 and a[3] - a[1] == 34 and b[1] - c[1] == pitch and a[2] == b[2] and a[2] == c[2]
  and d[3] - d[1] == 26 and d[1] - e[1] == 30 and d[2] == e[2])
)lua") == "true");
        CHECK(title() == "Spirit of Wolf");
        CHECK(run(R"lua(
local buffs, songs = trayText.BuffWindow, trayText.ShortDurationBuffWindow
return tostring(buffs['1m'] and songs['1m'] and not buffs['Spirit of Wolf'] and not songs['Chorus of the Singing Grove']
  and not buffs['#3'] and not buffs.Buffs and not songs.Songs
  and not trayPanel.BuffWindow and not trayPanel.ShortDurationBuffWindow)
)lua") == "true");

        choose("Show slot numbers in tooltips");
        CHECK(run("return ui.GetSetting('hud.showSlotNumbers.BuffWindow')") == "1");
        CHECK(run("return tostring(buffRects[2][1] == compactX and trayHeight == compactHeight)") == "true");
        CHECK(title() == "Spirit of Wolf - Slot 3");
        choose("Show slot numbers in tooltips");

        choose("Show empty slots");
        CHECK(run("return ui.GetSetting('hud.showEmptySlots.BuffWindow')") == "1");
        CHECK(run(R"lua(
local a, b, c = buffRects[2], buffRects[4], buffRects[5]
return tostring(a[1] == compactX - 2 * pitch and a[2] == compactY
  and b[1] == compactX - pitch and b[2] > compactY and c[1] == a[1] and c[2] == b[2]
  and trayHeight - compactHeight == b[2] - compactY
  and buffRects[0] == nil and buffRects[1] == nil and buffRects[3] == nil)
)lua") == "true");
        CHECK(title() == "Spirit of Wolf");
        choose("Show empty slots");

        // A bare right click keeps the spell actions. Window options opens the same frame menu as Alt+right-click.
        run("menuRows = {}; pick = 'Window options'");
        point_at_buff();
        io.AddMouseButtonEvent(1, true);
        s.frames(1);
        io.AddMouseButtonEvent(1, false);
        s.frames(2);
        CHECK(run("return tostring(pick == nil and menuRows['Spell Info'] and menuRows.Remove"
                  " and menuRows['Allow window resizing'])") == "true");
        ImGui::ClosePopupsExceptModals();
        s.frames(2);

        ImGuiWindow* buffs = ImGui::FindWindowByName("Buffs###modern.BuffWindow");
        ImGuiWindow* songs = ImGui::FindWindowByName("Songs###modern.ShortDurationBuffWindow");
        REQUIRE(buffs);
        REQUIRE(songs);
        const ImVec2 defaultSize = buffs->Size;
        const ImVec2 songSize = songs->Size;
        auto drag = [&](ImVec2 start, float dx, float dy) {
            io.AddMousePosEvent(start.x, start.y);
            s.frames(2);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            for (int i = 1; i <= 4; ++i) {
                io.AddMousePosEvent(start.x + dx * static_cast<float>(i) / 4,
                                    start.y + dy * static_cast<float>(i) / 4);
                s.frames(1);
            }
            io.AddMouseButtonEvent(0, false);
            io.AddMousePosEvent(-1000, -1000);
            s.frames(3);
        };
        auto leftEdge = [&] { return ImVec2(buffs->Pos.x + 2, buffs->Pos.y + buffs->Size.y / 2); };
        auto bottomEdge = [&] { return ImVec2(buffs->Pos.x + buffs->Size.x / 2, buffs->Pos.y + buffs->Size.y - 2); };
        drag(leftEdge(), 40, 0);
        CHECK(run("return ui.GetSetting('hud.size.BuffWindow') or ''") == "");
        CHECK(buffs->Size.x == defaultSize.x);
        CHECK(buffs->Size.y == defaultSize.y);

        choose("Allow window resizing");
        CHECK(run("return ui.GetSetting('hud.buffResize.BuffWindow')") == "1");
        const float anchoredRight = buffs->Pos.x + buffs->Size.x;
        drag(leftEdge(), 40, 0);
        const auto manualSize = run("return ui.GetSetting('hud.size.BuffWindow') or ''");
        REQUIRE_FALSE(manualSize.empty());
        CHECK(buffs->Size.x < defaultSize.x);
        CHECK(buffs->Size.y > defaultSize.y);
        CHECK(buffs->Pos.x + buffs->Size.x == doctest::Approx(anchoredRight).epsilon(0.002));
        CHECK(songs->Size.x == songSize.x);
        CHECK(songs->Size.y == songSize.y);
        run("ui.SetSetting('hud.lock.BuffWindow', '1')");
        drag(leftEdge(), -30, 0);
        CHECK(run("return ui.GetSetting('hud.size.BuffWindow')") == manualSize);
        CHECK(buffs->Pos.x + buffs->Size.x == doctest::Approx(anchoredRight).epsilon(0.002));
        run("ui.SetSetting('hud.lock.BuffWindow', '0')");
        choose("Allow window resizing");
        const ImVec2 wrapped = buffs->Size;
        drag(leftEdge(), -30, 0);
        CHECK(run("return ui.GetSetting('hud.size.BuffWindow')") == manualSize);
        CHECK(buffs->Size.x == wrapped.x);
        CHECK(buffs->Size.y == wrapped.y);
        choose("Reset size");
        CHECK(run("return ui.GetSetting('hud.size.BuffWindow') or ''") == "");
        CHECK(buffs->Size.x == defaultSize.x);
        CHECK(buffs->Size.y == defaultSize.y);

        choose("Vertical");
        CHECK(run("return ui.GetSetting('hud.buffLayout.BuffWindow')") == "vertical");
        CHECK(run(R"lua(
local a, b, c = buffRects[2], buffRects[4], buffRects[5]
return tostring(a[1] == b[1] and b[1] == c[1] and a[2] < b[2] and b[2] < c[2]
  and songRects[0][2] == songRects[1][2])
)lua") == "true");
        choose("Allow window resizing");
        drag(bottomEdge(), 0, -43);
        CHECK(run(R"lua(
local a, b, c = buffRects[2], buffRects[4], buffRects[5]
return tostring(a[1] == b[1] and c[1] < a[1] and a[2] < b[2] and c[2] == a[2])
)lua") == "true");

        choose("Show spell names");
        choose("Show slot numbers");
        choose("Show remaining time");
        choose("Show title bar");
        CHECK(run("return ui.GetSetting('hud.buffNames.BuffWindow')") == "1");
        CHECK(run("return ui.GetSetting('hud.buffSlotBadges.BuffWindow')") == "1");
        CHECK(run("return ui.GetSetting('hud.buffHideTime.BuffWindow')") == "1");
        CHECK(run("return ui.GetSetting('hud.buffTitleBar.BuffWindow')") == "1");
        run("clearTrayDraws()");
        s.frames(2);
        CHECK(run(R"lua(
local buffs, songs = trayText.BuffWindow, trayText.ShortDurationBuffWindow
return tostring(buffs['Spirit of Wolf'] and buffs['#3'] and buffs.Buffs and not buffs['1m']
  and songs['1m'] and not songs['Chorus of the Singing Grove'] and not songs.Songs and not trayPanel.BuffWindow)
)lua") == "true");
        run("pick = 'Show window background'");
        io.AddMousePosEvent(buffs->Pos.x + buffs->Size.x / 2, buffs->Pos.y + 6);
        io.AddMouseButtonEvent(1, true);
        s.frames(1);
        io.AddMouseButtonEvent(1, false);
        s.frames(2);
        CHECK(run("return tostring(pick == nil)") == "true");
        CHECK(run("return ui.GetSetting('hud.buffBackground.BuffWindow')") == "1");
        choose("Show title bar");
        run("clearTrayDraws()");
        s.frames(2);
        CHECK(run("return tostring(trayPanel.BuffWindow and not trayText.BuffWindow.Buffs"
                  " and not trayPanel.ShortDurationBuffWindow)") == "true");

        // Songs can opt into a named horizontal ribbon while Buffs stays a vertical list.
        choose("Show spell names", true);
        run("clearTrayDraws()");
        s.frames(2);
        CHECK(run(R"lua(
local a, b = songRects[0], songRects[1]
return tostring(a[1] > b[1] and a[2] == b[2] and ui.GetSetting('hud.buffLayout.BuffWindow') == 'vertical')
)lua") == "true");
        CHECK(run(R"lua(
local text = trayText.ShortDurationBuffWindow
local timer, lines = text['43s'], {}
if not timer then return 'false' end
for label, pos in pairs(text) do
  if pos[1] == timer[1] and pos[2] < timer[2] then lines[#lines + 1] = { label, pos[2] } end
end
table.sort(lines, function(a, b) return a[2] < b[2] end)
return tostring(#lines == 2 and lines[1][1] .. ' ' .. lines[2][1] == 'Chorus of the Singing Grove')
)lua") == "true");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs (modern): the Options search draws every tab, and only the sections that match") {
    test::ScratchDir scratch("ui_pack_options_search");
    const fs::path dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern", dir / "modern",
             fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        // Every option there (at 0), so every row is offered to the search.
        s.host.find_option = [](std::string_view) -> std::optional<int> { return 0; };
        lua_State* L = s.lua.state();
        // The search field answers with optSearch, the tab optTab is selected, and the section headings drawn are
        // kept; a plugin with one option gives the Plugins tab.
        REQUIRE(luaL_dostring(L, R"lua(
            optSearch, optTab, optTabs, headings = "", nil, {}, {}
            local input, separator, tabItem = ui.InputTextWithHint, ui.SeparatorText, ui.BeginTabItem
            ui.InputTextWithHint = function(label, ...)
                local text, changed = input(label, ...)
                if label == "##optionssearch" then return optSearch, true end
                return text, changed
            end
            ui.SeparatorText = function(text)
                headings[text] = true
                return separator(text)
            end
            ui.BeginTabItem = function(name, flags)
                if not optTabs[name] then
                    optTabs[name] = true
                    optTabs[#optTabs + 1] = name
                end
                if name == optTab then flags = (flags or 0) | ui.TabItemFlags.SetSelected end
                return tabItem(name, flags)
            end
            hs.PluginOptions = function()
                return { { plugin = "Demo Helper", owner = "plugin:demo", name = "on", label = "Enabled", min = 0,
                           max = 1, value = 0 } }
            end
            hs.ShowWindow("OptionsWindow", true)
        )lua") == LUA_OK);
        s.frames(3);
        auto lua_string = [&](const char* code) {
            REQUIRE_MESSAGE(luaL_dostring(L, code) == LUA_OK, lua_tostring(L, -1));
            std::string out = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return out;
        };
        const std::string tabs = lua_string("return table.concat(optTabs, ',')");
        REQUIRE(tabs.find("Plugins") != std::string::npos);
        // Draws every tab with the search; gives the headings drawn in all of them, sorted.
        auto headings_for = [&](const char* search) {
            lua_pushstring(L, search);
            lua_setglobal(L, "optSearch");
            REQUIRE(luaL_dostring(L, "headings = {}") == LUA_OK);
            for (int i = 1;; ++i) {
                lua_getglobal(L, "optTabs");
                lua_rawgeti(L, -1, i);
                const bool more = lua_isstring(L, -1);
                if (more) lua_setglobal(L, "optTab");
                lua_settop(L, 0);
                if (!more) break;
                s.frames(3);
            }
            return lua_string("local t = {} for k in pairs(headings) do t[#t + 1] = k end table.sort(t) "
                              "return table.concat(t, ',')");
        };
        const std::string all = headings_for("");
        CHECK(all.find("Target Ring") != std::string::npos);
        CHECK(all.find("Name Plates") != std::string::npos);
        CHECK(all.find("Demo Helper") != std::string::npos);
        auto has = [](const std::string& list, std::string_view name) {
            return ("," + list + ",").find("," + std::string(name) + ",") != std::string::npos;
        };
        // The section's title is searched: all its rows show, and the sections with nothing that matches go (the
        // controls tab's key list keeps its own heading).
        const std::string ring = headings_for("target ring");
        CHECK(has(ring, "Target Ring"));
        CHECK_FALSE(has(ring, "Name Plates"));
        CHECK_FALSE(has(ring, "Volume"));
        const std::string demo = headings_for("demo"); // a plugin's section by the plugin's name
        CHECK(demo == "Demo Helper,Key Bindings");
        const std::string fov = headings_for("fov"); // Field of View, by its initials
        CHECK(has(fov, "View"));
        CHECK_FALSE(has(fov, "Target Ring"));
        // Nothing matches: no section's heading or empty table, only the key list's own heading.
        CHECK(headings_for("zzqqxx") == "Key Bindings");
        CHECK(headings_for("") == all);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// The gamepad pack's Looking for Guild window: a test page setting past its four pages (or not a whole number) is
// ignored rather than drawing a page that does not exist (an error every frame).
TEST_CASE("UI packs (gamepad): Looking for Guild ignores a test page it does not have") {
    for (const char* page : {"9", "2.5"}) {
        INFO(page);
        test::ScratchDir scratch("ui_gamepad_lfguild_page_test");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad", dir / "gamepad",
                 fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, "gamepad");
            const std::string code =
                std::string("ui.SetSetting('test.lfguild.page', '") + page + "') hs.ShowWindow('LFGuildWnd', true)";
            REQUIRE(luaL_dostring(s.lua.state(), code.c_str()) == LUA_OK);
            s.frames(4);
            CHECK(ImGui::FindWindowByName("Looking for Guild") != nullptr);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// The gamepad pack's chat keyboard stops at the input box's length (2047 bytes): a key pressed on a full line adds
// nothing, so the line the box shows is the line that is sent.
TEST_CASE("UI packs (gamepad): the chat's on-screen keys stop at the input's length") {
    test::ScratchDir scratch("ui_gamepad_chat_cap_test");
    const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad", dir / "gamepad",
             fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "gamepad");
        auto* L = s.lua.state();
        auto run = [&](const char* code) {
            const int rc = luaL_dostring(L, code);
            INFO((rc == LUA_OK ? "" : lua_tostring(L, -1)));
            REQUIRE(rc == LUA_OK);
            const std::string result = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return result;
        };
        s.frames(2); // (the pack's state exists)
        // forced[id]: that button reports a click once; chatInput: the text the chat's input box was given
        run(R"lua(
forced, chatInput = {}, nil
local button, input = ui.InvisibleButton, ui.InputText
ui.InvisibleButton = function(id, ...)
  local hit = button(id, ...)
  if forced[id] then forced[id] = nil return true end
  return hit
end
ui.InputText = function(id, text, ...)
  if id == "##chatinput" then chatInput = text end
  return input(id, text, ...)
end
GPSTATE.chatText = string.rep("a", 2046)
)lua");
        s.frames(2);
        CHECK(run("return tostring(#(chatInput or ''))") == "2046");
        run("forced['##space'] = true");
        s.frames(2);
        CHECK(run("return tostring(#chatInput)") == "2047");
        run("forced['##space'] = true");
        s.frames(2);
        CHECK(run("return tostring(#chatInput)") == "2047"); // full: the key adds nothing
        run("forced['##k2_1'] = true"); // (q)
        s.frames(2);
        CHECK(run("return tostring(#chatInput)") == "2047");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// The gamepad pack's Who window puts a player's name into /invite (and /target, /friends) as one word: control
// characters from the server's reply are dropped (a newline would start another command), and only the first word
// is kept.
TEST_CASE("UI packs (gamepad): Who strips a name to one word before it goes into a command") {
    test::ScratchDir scratch("ui_gamepad_who_name_test");
    const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad", dir / "gamepad",
             fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "gamepad");
        auto* L = s.lua.state();
        auto run = [&](const char* code) {
            const int rc = luaL_dostring(L, code);
            INFO((rc == LUA_OK ? "" : lua_tostring(L, -1)));
            REQUIRE(rc == LUA_OK);
            const std::string result = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return result;
        };
        s.frames(2); // (the pack's state exists)
        run(R"lua(
forced, submitted = {}, {}
local button = ui.InvisibleButton
ui.InvisibleButton = function(id, ...)
  local hit = button(id, ...)
  if forced[id] then forced[id] = nil return true end
  return hit
end
hs.WhoResults = function()
  return 1, { { name = "Bad\nName /kill", line = "[50 Warrior] Bad\nName /kill (Human)" } }
end
hs.ChatSubmit = function(slot, line) submitted[#submitted + 1] = line end
GPSTATE.whoOpen = true
)lua");
        s.frames(2);
        run("forced['##row'] = true"); // picks the player
        s.frames(2);
        run("forced['##invite'] = true");
        s.frames(2);
        CHECK(run("return submitted[#submitted] or ''") == "/invite BadName");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// The gamepad pack's LB is Loot All's own key: it does nothing while the stock window has Loot All disabled.
TEST_CASE("UI packs (gamepad): LB loots everything only while Loot All is enabled") {
    test::ScratchDir scratch("ui_gamepad_loot_all_test");
    const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad", dir / "gamepad",
             fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "gamepad");
        auto* L = s.lua.state();
        auto run = [&](const char* code) {
            const int rc = luaL_dostring(L, code);
            INFO((rc == LUA_OK ? "" : lua_tostring(L, -1)));
            REQUIRE(rc == LUA_OK);
            const std::string result = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return result;
        };
        run(R"lua(
padPress, lootShown, lootAllEnabled, lootAlls = nil, false, false, 0
hs.gamepad = {
  connected = function() return true end,
  type = function() return 'xbox' end,
  mode = function() return 'menu' end,
  set_mode = function() return 'menu' end,
  pressed = function(b) return b == padPress end,
  down = function(b) return b == padPress end,
  released = function() return false end,
  axis = function() return 0 end,
  glyph = function(b) return string.upper(b) end,
  takes_b = function() end,
  request_focus = function(window) ui.SetWindowFocus(window) end,
}
local shown, state, slot, click = hs.WindowShown, hs.ChildState, hs.SlotItem, hs.ClickChild
hs.WindowShown = function(win) if win == "LootWnd" then return lootShown end return shown(win) end
hs.ChildState = function(win, id, ...)
  if win == "LootWnd" and id == "LootAllButton" then return lootAllEnabled end
  return state(win, id, ...)
end
hs.SlotItem = function(win, child, ...)
  if win == "LootWnd" then
    if child == "LW_LootSlot0" then return { name = "Rusty Dagger", icon = 0, count = 1 } end
    return nil
  end
  return slot(win, child, ...)
end
hs.ClickChild = function(win, id, button)
  if win == "LootWnd" then
    if id == "LootAllButton" then lootAlls = lootAlls + 1 end
    return true
  end
  return click(win, id, button)
end
)lua");
        s.frames(240); // the pack takes the pad's focus for a window only from 3 seconds after zoning in
        run("lootShown = true");
        s.frames(30);
        auto press_lb = [&] {
            run("padPress = 'lb'");
            s.frames(1);
            run("padPress = nil");
            s.frames(2);
        };
        press_lb();
        CHECK(run("return tostring(lootAlls)") == "0");
        run("lootAllEnabled = true");
        s.frames(2);
        press_lb();
        CHECK(run("return tostring(lootAlls)") == "1");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// The gamepad pack's question before a character is deleted starts on Cancel (its rule for every question): an A
// carried over from the Delete press must not answer it.
TEST_CASE("UI packs (gamepad): the delete question starts on Cancel") {
    test::ScratchDir scratch("ui_gamepad_delete_focus_test");
    const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad", dir / "gamepad",
             fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "gamepad", {}, false);
        ui::CharacterSelectView chars;
        chars.server_name = "World One";
        chars.characters = {{"Bob", 12, 1, {1}, "North Qeynos"}, {"Cid", 50, 2, {5, 2}, "Kaladim"}};
        chars.selected = 0;
        s.host.ui_ready = true;
        s.host.game_state = 1;
        s.host.char_select = &chars;
        auto* L = s.lua.state();
        REQUIRE(luaL_dostring(L, R"lua(
forced, focused = {}, {}
local invisible = ui.InvisibleButton
ui.InvisibleButton = function(id, ...)
  local hit = invisible(id, ...)
  focused[id] = ui.IsItemFocused()
  if forced[id] then forced[id] = nil return true end
  return hit
end
)lua") == LUA_OK);
        s.frames(30);
        REQUIRE(luaL_dostring(L, "forced['##delete'] = true") == LUA_OK);
        s.frames(6);
        CHECK(s.lua.run_for_test("", "return tostring(focused['##delno'] == true and focused['##delyes'] == false)") ==
              "true");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        s.host.char_select = nullptr;
    }
    ImGui::DestroyContext(ctx);
}
