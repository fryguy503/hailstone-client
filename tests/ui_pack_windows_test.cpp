// The shipped UIs (ui/modern, ui/gamepad) run for real over Dear ImGui with no renderer: every script loads, and the
// windows each draws for itself (the ones that once came from the Classic UI among them) draw with the game asking
// for them, with no script error.
#include <doctest/doctest.h>

#include "ui_pack_session.h"

namespace {

net::AppPacket respawn_window_packet(std::uint8_t validity) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(60000);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(2);
    for (std::uint32_t id = 0; id < 2; ++id) {
        w.le(id);
        w.le<std::uint32_t>(2);
        for (int i = 0; i < 4; ++i) w.f32(0);
        const std::string_view name = id == 0 ? "Bind Location" : "Resurrect";
        w.bytes(std::as_bytes(std::span(name.data(), name.size())));
        w.le<std::uint8_t>(0);
        w.le<std::uint8_t>(id == 0 ? 0 : validity);
    }
    return net::AppPacket(net::to_wire(net::Op::RespawnWindow), w.take());
}

net::AppPacket resurrection_offer_packet() {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint16_t>(2);
    w.le<std::uint16_t>(0);
    w.f32(30);
    w.f32(40);
    w.f32(12);
    w.le<std::uint32_t>(0);
    w.fixed_string("Gennro", 64);
    w.le<std::uint32_t>(0);
    w.fixed_string("Healer", 64);
    w.le<std::uint32_t>(100);
    w.fixed_string("Gennro's corpse", 64);
    w.le<std::uint32_t>(0);
    w.zeros(8);
    return net::AppPacket(net::to_wire(net::Op::RezzRequest), w.take());
}

// Windows the packs redid from the Classic UI's (and a few more of theirs): shown one after another.
constexpr const char* kScreens[] = {
    "GroupSearchWnd", "GroupSearchFiltersWnd", "GroupMentorWindow", "FellowshipWnd", "SocialEditWnd",
    "IconSelectionWnd", "CombatAbilityWnd", "JournalNPCWnd", "JournalCatWnd", "HelpWindow", "StoryWnd",
    "AlertHistoryWnd", "ZonePathWnd", "TributeTrophyWnd",
    "LoadskinWnd", "LayoutCopyWindow", "LayoutProfilesWindow", "VideoModesWindow",
    "NameChangeWnd", "NameChangePetWnd", "NameChangeMercWnd",
    "OptionsWindow", "AlarmWnd", "TipWindow", "NoteWindow", "EditLabelWnd", "BugReportWindow", "FeedbackWindow",
    "ClaimWnd", "ColorPickerWnd", "BodyTintWnd", "PlayerCustomizationWnd", "TitleWnd", "BreathWindow",
    "AggroMeterWnd", "PotionBeltWnd", "BandolierWnd", "BlockedBuffWnd", "BlockedPetBuffWnd", "InspectWnd",
    "LeadershipWindow", "LFGuildWnd", "MailAddressBookWindow", "MailIgnoreListWindow", "RaidOptionsWindow",
    "CombatSkillSelectWnd", "AdventureRequestWnd", "AdventureStatsWnd", "AdventureLeaderboardWnd", "DynamicZoneWnd",
    "PvPStatsWnd", "PvpLeaderboardWnd", "BazaarSearchWnd", "BarterSearchWnd", "TrainWindow", "TrackingWnd",
    "GuildBankWnd", "HotButtonWnd2", "HotButtonWnd3", "BrowserWnd", "RealEstatePurchaseWnd", "RealEstateManageWnd",
    "RealEstateItemsWnd", "RealEstateNeighborhoodWnd", "RealEstatePlotSearchWnd", "ScriptsWnd", "ItemExpTransferWnd"};


} // namespace

TEST_CASE("UI packs (gamepad): real window chrome fits resized viewports and explicit positions") {
    struct ScaleGuard {
        float previous = ui::ui_scale();
        ~ScaleGuard() { ui::set_ui_scale(previous); }
    } scale_guard;
    for (float scale : {1.0f, 2.0f, 0.5f}) {
        CAPTURE(scale);
        test::ScratchDir scratch("ui_gamepad_window_bounds");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::create_directories(dir / "gamepad");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui/gamepad/lib", dir / "gamepad/lib",
                 fs::copy_options::recursive);
        std::ofstream(dir / "gamepad/bounds.lua") << R"lua(
            local W = dofile('lib/window.lua')
            desiredWidth = 1720
            local invisible, line = ui.InvisibleButton, ui.DrawLine
            ui.InvisibleButton = function(id, ...)
                local clicked = invisible(id, ...)
                if id == '##close' then closeRect = {ui.GetItemRect()} end
                return clicked
            end
            ui.DrawLine = function(x0,y0,x1,y1,...)
                if y0 == y1 then dividers[#dividers+1] = {x0,y0,x1,y1} end
                return line(x0,y0,x1,y1,...)
            end
            ui.window('Geometry', function()
                dividers = {}
                if W.begin('Geometry', {w=desiredWidth,h=960,x=requestX,y=requestY,title='Title',subtitle='Subtitle',
                        hints={{'a','Select'},{'b','Close'}},noFocus=true}) then
                    contentRect = {W.area()}
                    ui.InvisibleButton('Content', 40, 12)
                    bodyRect = {ui.GetItemRect()}
                end
                closed = W.finish()
            end)
        )lua";
        std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
        ImFontConfig font; font.SizePixels = 20.0f * scale;
        io.Fonts->AddFontDefault(&font);
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        ui::set_ui_scale(scale);
        Session s(dir, "gamepad");
        auto frame = [&](float width, float height) {
            io.DisplaySize = ImVec2(width, height); io.DeltaTime = 1.0f / 60.0f;
            s.api.begin_frame(); ImGui::NewFrame(); s.lua.frame(); ImGui::Render();
            CHECK(s.lua.load_status().error_count == 0);
        };
        auto table = [&](const char* name) {
            std::vector<double> values;
            lua_State* L = s.lua.state(); lua_getglobal(L, name); REQUIRE(lua_istable(L, -1));
            for (std::size_t i = 1; i <= lua_rawlen(L, -1); ++i) {
                lua_rawgeti(L, -1, static_cast<lua_Integer>(i)); values.push_back(lua_tonumber(L, -1)); lua_pop(L, 1);
            }
            lua_pop(L, 1); return values;
        };
        auto check_bounds = [&](float width, float height) {
            const auto* w = ImGui::FindWindowByName("Geometry"); REQUIRE(w);
            CHECK(w->Pos.x >= 0); CHECK(w->Pos.y >= 0);
            CHECK(w->Pos.x + w->Size.x <= width + 0.01f); CHECK(w->Pos.y + w->Size.y <= height + 0.01f);
            const auto area = table("contentRect"), close = table("closeRect"), body = table("bodyRect");
            REQUIRE(area.size() == 6); REQUIRE(close.size() == 4); REQUIRE(body.size() == 4);
            CHECK(area[0] >= w->Pos.x); CHECK(area[1] > w->Pos.y);
            CHECK(area[2] <= w->Pos.x + w->Size.x); CHECK(area[3] < w->Pos.y + w->Size.y);
            CHECK(area[4] > 0); CHECK(area[5] > 0);
            CHECK(close[0] >= w->Pos.x); CHECK(close[1] >= w->Pos.y);
            CHECK(close[2] <= width); CHECK(close[3] <= height);
            CHECK(body[0] >= area[0]); CHECK(body[1] >= area[1]);
            CHECK(body[2] <= area[2]); CHECK(body[3] <= area[3]);
            bool header = false, footer = false;
            lua_State* L = s.lua.state(); lua_getglobal(L, "dividers");
            for (std::size_t i = 1; i <= lua_rawlen(L, -1); ++i) {
                lua_rawgeti(L, -1, static_cast<lua_Integer>(i)); lua_rawgeti(L, -1, 2);
                const double y = lua_tonumber(L, -1); lua_pop(L, 2);
                header = header || (y > w->Pos.y && y < area[1]);
                footer = footer || (y > area[3] && y < w->Pos.y + w->Size.y);
            }
            lua_pop(L, 1); CHECK(header); CHECK(footer);
        };
        for (int desired : {1720, 1500}) {
            REQUIRE(s.lua.run_for_test("", "desiredWidth=" + std::to_string(desired) + " return true") == "true");
            for (const auto [width, height] : {std::pair{1920.0f,1080.0f}, {720.0f,700.0f}, {1280.0f,360.0f}, {1920.0f,1080.0f}}) {
                CAPTURE(desired); CAPTURE(width); CAPTURE(height);
                frame(width, height); frame(width, height); check_bounds(width, height);
                const auto* w = ImGui::FindWindowByName("Geometry"); REQUIRE(w);
                if (width == 1920) {
                    CHECK(w->Size.x == desired); CHECK(w->Size.y == 960);
                    CHECK(w->Pos.x == doctest::Approx((1920.0f - static_cast<float>(desired)) / 2)); CHECK(w->Pos.y == 60);
                } else if (width == 720) {
                    CHECK(w->Size.x == 708); CHECK(w->Size.y == 480); CHECK(w->Pos.x == 6); CHECK(w->Pos.y == 110);
                }
            }
        }
        for (const auto [x,y] : {std::pair{-10000, -10000}, {0,0}, {10000,10000}}) {
            CAPTURE(x); CAPTURE(y);
            REQUIRE(s.lua.run_for_test("", "requestX="+std::to_string(x)+" requestY="+std::to_string(y)+" return true") == "true");
            frame(720,700); frame(720,700); check_bounds(720,700);
            const auto* w = ImGui::FindWindowByName("Geometry"); REQUIRE(w);
            CHECK(w->Pos.x == (x > 0 ? 12 : 0)); CHECK(w->Pos.y == (y > 0 ? 220 : 0));
        }
        const auto close = table("closeRect");
        io.AddMousePosEvent(static_cast<float>((close[0]+close[2])/2), static_cast<float>((close[1]+close[3])/2));
        io.AddMouseButtonEvent(0,true); frame(720,700);
        io.AddMouseButtonEvent(0,false); frame(720,700);
        CHECK(s.lua.run_for_test("", "return closed") == "true");
    }
}

TEST_CASE("UI packs (gamepad): stock Raid and Map stay inside narrow and short native viewports") {
    test::ScratchDir scratch("ui_gamepad_stock_bounds"); const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui/gamepad", dir / "gamepad", fs::copy_options::recursive);
    std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    Session s(dir, "gamepad"); s.lua.set_entry_scripts({"raid.lua", "map.lua"});
    for (int member = 0; member < 72; ++member) {
        net::RaidUpdate update; update.general.action = net::kRaidAddMember;
        update.general.player_name = "Member"+std::to_string(member); update.general.parameter = static_cast<std::uint32_t>(member/6);
        update.class_ = 2; update.level = 60;
        s.conn.receive_zone_packet_for_test(net::encode_raid_update(update));
    }
    REQUIRE(s.conn.raid().active); REQUIRE(s.conn.raid().members.size() == 72);
    REQUIRE(s.lua.run_for_test("", "hs.UI.windows.Show('RaidWindow',true) hs.UI.windows.Show('MapViewWnd',true) return true") == "true");
    auto frame = [&](float w, float h) {
        io.DisplaySize = ImVec2(w,h); io.DeltaTime = 1.0f/60.0f;
        s.api.begin_frame(); ImGui::NewFrame(); s.lua.frame(); ImGui::Render();
        CHECK(s.lua.load_status().error_count == 0);
    };
    for (const auto [w,h] : {std::pair{720.0f,700.0f}, {1280.0f,360.0f}}) {
        CAPTURE(w); CAPTURE(h); frame(w,h); frame(w,h);
        REQUIRE(s.lua.run_for_test("", "return hs.UI.windows.ListClick('RaidWindow','RAID_PlayerList',0,'left')") == "true");
        frame(w,h); frame(w,h);
        for (const char* name : {"Raid", "Map"}) {
            CAPTURE(name); const auto* window = ImGui::FindWindowByName(name); REQUIRE(window);
            CHECK(window->Active); CHECK(window->Pos.x >= 0); CHECK(window->Pos.y >= 0);
            CHECK(window->Pos.x+window->Size.x <= w); CHECK(window->Pos.y+window->Size.y <= h);
            CHECK(window->Size.x > 0); CHECK(window->Size.y > 0);
        }
        const auto* raid = ImGui::FindWindowByName("Raid"); REQUIRE(raid);
        const ImGuiWindow* actions = nullptr;
        for (const auto* child : raid->DC.ChildWindows) if (child->Name && std::string_view(child->Name).find("##raidactions") != std::string_view::npos) actions=child;
        REQUIRE(actions); CHECK(actions->Active); CHECK(actions->Size.y > 0);
        CHECK(actions->Pos.x >= raid->Pos.x); CHECK(actions->Pos.y > raid->Pos.y);
        CHECK(actions->Pos.x+actions->Size.x <= raid->Pos.x+raid->Size.x);
        CHECK(actions->Pos.y+actions->Size.y < raid->Pos.y+raid->Size.y);
        if (h <= 360) {
            CHECK(actions->ScrollMax.y > 0);
            ImGui::SetScrollY(const_cast<ImGuiWindow*>(actions), actions->ScrollMax.y);
            frame(w,h); frame(w,h);
            CHECK(actions->Scroll.y == doctest::Approx(actions->ScrollMax.y));
        }
    }
}

// Vanilla maps (hs.MapVanilla): the Modern map and minimap leave out their own markers and Find marks, and keep
// drawing the map's lines and everything around them.
TEST_CASE("UI packs: Modern's maps leave their own marks out of vanilla maps") {
    const fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui/modern";
    hailstone::test::ScratchDir scratch("ui_pack_vanilla_map");
    const fs::path dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::create_directories(dir / "maps");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    std::ofstream(dir / "maps" / "qeynos2.txt") << "L -100, -100, 0,  100, 100, 0,  0, 255, 0\n";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        mq::MapFilters filters;
        s.host.map_filters = &filters;
        s.host.zone_short_name = "qeynos2";
        s.host.global_maps_dir = dir / "maps";
        s.host.feet_valid = true;
        auto& world = s.conn.world_for_test();
        world.zone_short_name = "qeynos2";
        world.player_id = 1;
        game::Spawn me;
        me.id = 1;
        me.name = "Tester";
        world.spawns[1] = me;
        ++world.spawn_set_serial;
        lua_State* L = s.lua.state();
        // A red marker (the fourth colour) in the middle of the map, the sidebar off so its list draws no dot.
        REQUIRE(luaL_dostring(L, "ui.SetSetting('map.sidebar', '0') ui.SetSetting('map.markers.qeynos2', 'Here|0.0|0.0|4') "
                                 "hs.ShowWindow('MapViewWnd', true)") == LUA_OK);
        auto count = [](ImU32 rgb) {
            int n = 0;
            const ImDrawData* data = ImGui::GetDrawData();
            for (int i = 0; data && i < data->CmdLists.Size; ++i)
                for (const ImDrawVert& v : data->CmdLists[i]->VtxBuffer)
                    if ((v.col & 0x00ffffffu) == rgb) ++n;
            return n;
        };
        constexpr ImU32 kMarker = IM_COL32(255, 107, 107, 0), kLine = IM_COL32(0, 255, 0, 0);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(s.api.shown("MapViewWnd"));
        CHECK(count(kMarker) > 0);
        CHECK(count(kLine) > 0);
        filters.set_vanilla_enabled(true);
        s.frames(2);
        CHECK(count(kMarker) == 0);
        CHECK(count(kLine) > 0);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        filters.set_vanilla_enabled(false);
        s.frames(2);
        CHECK(count(kMarker) > 0);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs: context submenus cascade inward at either screen edge and accept a choice") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        test::ScratchDir scratch("ui_pack_context_menu_cascade");
        const auto dir = scratch.path();
        const auto src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        Session s(dir, pack);
        s.lua.set_entry_scripts({"menus.lua"});

        int choices = 0;
        using Row = app::UiHost::MenuRow;
        Row spell;
        spell.text = "Resist Magic";
        spell.act = [&] { ++choices; };
        Row subcategory;
        subcategory.text = "Resist Buff";
        subcategory.submenu = [&] { return std::vector{spell}; };
        Row category;
        category.text = "Utility Beneficial";
        category.submenu = [&] { return std::vector{subcategory}; };

        const auto panel = [](int menu) {
            const std::string name = "##gamemenu" + std::to_string(menu);
            const auto* window = ImGui::FindWindowByName(name.c_str());
            REQUIRE(window);
            REQUIRE(window->Active);
            return window;
        };
        const auto move_to = [&](const ImGuiWindow& window) {
            io.AddMousePosEvent(window.Pos.x + window.Size.x / 2.0f, window.Pos.y + window.Size.y / 2.0f);
            s.frames(2);
        };
        for (const bool at_right : {true, false}) {
            CAPTURE(at_right);
            io.AddMousePosEvent(-100, -100);
            s.frames(1);
            s.host.open_menu(at_right ? 1580.0f : 8.0f, at_right ? 890.0f : 180.0f, {category});
            s.frames(2);
            REQUIRE(s.host.menus.size() == 1);
            move_to(*panel(1));
            REQUIRE(s.host.menus.size() == 2);
            move_to(*panel(2));
            REQUIRE(s.host.menus.size() == 3);
            REQUIRE(s.lua.load_status().error_count == 0);

            const ImGuiWindow* parent = nullptr;
            for (int m = 1; m <= 3; ++m) {
                const auto* window = panel(m);
                CHECK(window->Pos.x >= 0.0f);
                CHECK(window->Pos.x + window->Size.x <= io.DisplaySize.x);
                CHECK(window->Pos.y >= 0.0f);
                CHECK(window->Pos.y + window->Size.y <= io.DisplaySize.y);
                if (parent) {
                    const float gap = at_right ? parent->Pos.x - (window->Pos.x + window->Size.x)
                                               : window->Pos.x - (parent->Pos.x + parent->Size.x);
                    CHECK(std::abs(gap) <= 1.0f); // ImGui rounds dimensions to pixels
                }
                parent = window;
            }
            move_to(*panel(3));
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            s.frames(1);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            s.frames(2);
            CHECK(choices == (at_right ? 1 : 2));
            CHECK(s.host.menus.empty());
        }
    }
}

TEST_CASE("UI packs: every script of the shipped UIs loads, and their windows draw without script errors") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        REQUIRE(fs::exists(src / "ui.ini"));
        hailstone::test::ScratchDir dir_scratch("ui_pack_windows_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        CHECK_FALSE(fs::exists(dir / "classic")); // nothing of the Classic UI is there to borrow

        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            lua_State* L = s.lua.state();
            for (const char* screen : kScreens) {
                INFO(screen);
                std::string show = std::string("hs.ShowWindow('") + screen + "', true)";
                REQUIRE(luaL_dostring(L, show.c_str()) == LUA_OK);
                s.frames(2);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
                std::string hide = std::string("hs.ShowWindow('") + screen + "', false)";
                REQUIRE(luaL_dostring(L, hide.c_str()) == LUA_OK);
            }
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: EQ menu opens Layout Profiles when the current pack draws it") {
    for (const std::string_view pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        test::ScratchDir scratch("lp-menu");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack, fs::copy_options::recursive);
        struct ResetDataDirectory {
            ~ResetDataDirectory() { platform::set_data_dir_for_tests({}); }
        } reset_directory;
        platform::set_data_dir_for_tests(dir);
        std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

        Config config;
        app::ActionsUi actions(config, dir / "hailstone.ini");
        Session s(dir, std::string(pack));
        s.host.actions = &actions;
        s.host.layout_profile_pack = [&] { return std::pair{&s.lua, pack}; };
        s.lua.set_character_settings(dir / "pack.txt");
        s.lua.set_entry_scripts({"menus.lua"});
        s.frames(2);
        auto run = [&](const char* code) {
            INFO(std::string(code));
            const auto rc = luaL_dostring(s.lua.state(), code);
            REQUIRE_MESSAGE(rc == LUA_OK, std::string(rc == LUA_OK ? "" : lua_tostring(s.lua.state(), -1)));
        };
        auto click_menu = [&] { REQUIRE(s.api.click_child("EQMainWnd", "EQButton", "left")); };
        click_menu();
        run(R"lua(
            function layoutProfileRow()
              local root = assert(hs.UI.menus.Cascades()[1])
              for row, item in ipairs(root.items) do
                if item.text == "Layout Profiles" then return row end
              end
            end
            assert(not layoutProfileRow())
        )lua");
        click_menu();
        s.lua.set_entry_scripts({"menus.lua", "layoutprofiles.lua"});
        s.lua.rescan_for_test();
        s.frames(2);
        REQUIRE(s.api.replaces("LayoutProfilesWindow"));
        CHECK_FALSE(s.api.shown("LayoutProfilesWindow"));
        click_menu();
        run(R"lua(
            hs.UI.menus.CascadeClick(1, assert(layoutProfileRow()))
            assert(#hs.UI.menus.Cascades() == 0)
        )lua");
        CHECK(s.api.shown("LayoutProfilesWindow"));
        s.frames(2);
        const auto* window = ImGui::FindWindowByName(pack == "modern" ? "Layout Profiles###modern.LayoutProfilesWindow"
                                                                    : "Layout Profiles");
        REQUIRE(window);
        CHECK(window->Active);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
}

TEST_CASE("UI packs: visual layout preview draws saved positions without applying") {
    for (const std::string_view pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        test::ScratchDir scratch("lp-preview");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack, fs::copy_options::recursive);
        struct ResetDataDirectory {
            ~ResetDataDirectory() { platform::set_data_dir_for_tests({}); }
        } reset_directory;
        platform::set_data_dir_for_tests(dir);
        std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

        Session s(dir, std::string(pack));
        s.host.layout_profile_pack = [&] { return std::pair{&s.lua, pack}; };
        s.lua.set_character_settings(dir / "pack.txt");
        std::ofstream(dir / pack / "preview_fixture.lua") << R"lua(
            local function panel(name, x, y, w, h)
              ui.window(name, function()
                ui.SetNextWindowPos(x, y, ui.Cond.Always)
                ui.SetNextWindowSize(w, h, ui.Cond.Always)
                ui.Begin(name)
                ui.End()
              end)
            end
            panel('Saved Frame', 24, 40, 290, 164)
            panel('Second Frame', 1040, 270, 290, 150)
        )lua";
        s.lua.set_entry_scripts({"layoutprofiles.lua", "preview_fixture.lua"});
        s.display_size = {1280, 720};
        s.frames(2);
        auto run = [&](const char* code) {
            INFO(std::string(code));
            const auto rc = luaL_dostring(s.lua.state(), code);
            REQUIRE_MESSAGE(rc == LUA_OK, std::string(rc == LUA_OK ? "" : lua_tostring(s.lua.state(), -1)));
        };
        REQUIRE(s.lua.set_setting("hud.locked", "1"));
        run(R"lua(
            local profiles = hs.UI.layoutProfiles
            assert(profiles.Save('Desktop'))
            local model = assert(profiles.Preview('Desktop'))
            assert(model.width == 1280 and model.height == 720 and model.approximate == nil)
            assert(#model.rects == 2)
            local rect = {}
            for _, item in ipairs(model.rects) do rect[item.name] = item end
            local first, second = assert(rect['Saved Frame']), assert(rect['Second Frame'])
            assert(first.x == 24 and first.y == 40 and first.w == 290 and first.h == 164)
            assert(second.x == 1040 and second.y == 270 and second.w == 240 and second.h == 150)
        )lua");
        REQUIRE(s.lua.set_setting("hud.locked", "0"));
        s.lua.set_entry_scripts({"layoutprofiles.lua"});
        s.lua.rescan_for_test();
        run(R"lua(
            visualText, visualActions = {}, 0
            for _, name in ipairs({'LayoutProfilesSave', 'LayoutProfilesApply', 'LayoutProfilesUndo'}) do
              local native = hs[name]
              hs[name] = function(...)
                visualActions = visualActions + 1
                return native(...)
              end
            end
            local text, draw, button, invisible = ui.TextWrapped, ui.DrawText, ui.Button, ui.InvisibleButton
            ui.TextWrapped = function(t, ...) visualText[t] = true; return text(t, ...) end
            ui.DrawText = function(x, y, r, g, b, a, t, ...)
              visualText[t] = true
              return draw(x, y, r, g, b, a, t, ...)
            end
            local function press(button, label, ...)
              local clicked = button(label, ...)
              if label ~= pressControl then return clicked end
              pressControl = nil
              return true
            end
            ui.Button = function(...) return press(button, ...) end
            ui.InvisibleButton = function(...) return press(invisible, ...) end
        )lua");
        s.api.show("LayoutProfilesWindow", true);
        s.frames(2);
        auto press = [&](const char* modern, const char* gamepad) {
            lua_pushstring(s.lua.state(), pack == "modern" ? modern : gamepad);
            lua_setglobal(s.lua.state(), "pressControl");
            s.frames(2);
            run("assert(pressControl == nil)");
        };
        press("Visual Preview", "##profile-visual-preview");
        run(R"lua(
            assert(visualText['Saved Frame'] and visualText['Second Frame'])
            assert(visualText['Read-only view of the saved window positions.'])
            assert(visualActions == 0 and not hs.UI.layoutProfiles.CanUndo())
        )lua");
        CHECK(s.lua.setting("hud.locked") == "0");
        press("Close Preview", "##close-visual-preview");
        CHECK(s.api.shown("LayoutProfilesWindow"));
        const auto profile_dir = dir / "layout-profiles" / platform::ui_settings_file(pack).stem();
        auto preview_saved_file = [&](const ui::LayoutProfile& profile) {
            s.api.show("LayoutProfilesWindow", false);
            s.frames(1);
            REQUIRE(ui::save_layout_profile(profile_dir, profile, true) == ui::LayoutProfileSave::Saved);
            run("visualText = {}");
            s.api.show("LayoutProfilesWindow", true);
            s.frames(2);
            press("Visual Preview", "##profile-visual-preview");
        };
        ui::LayoutProfile empty;
        empty.name = "Desktop";
        empty.width = 1280;
        empty.height = 720;
        preview_saved_file(empty);
        run(R"lua(
            assert(visualText['No window positions are available. Save this layout again to capture a visual preview.'])
            assert(visualActions == 0 and not hs.UI.layoutProfiles.CanUndo())
        )lua");
        CHECK(s.lua.setting("hud.locked") == "0");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
}

TEST_CASE("UI packs: unsaved layout switch can cancel, skip, or save before applying") {
    for (const std::string_view pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        test::ScratchDir scratch("lp");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack, fs::copy_options::recursive);
        platform::set_data_dir_for_tests(dir);
        struct ResetDataDirectory {
            ~ResetDataDirectory() { platform::set_data_dir_for_tests({}); }
        } reset_directory;
        std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

        auto options = test::quick_options();
        options.profile_id = "p";
        options.servers_dir = dir / "s";
        test::Rig rig(test::MockConfig::standard(), options);
        REQUIRE(rig.to_zone());
        const auto config_path = dir / "hailstone.ini";
        Config config;
        REQUIRE(config.attach_character(dir / "keys.ini"));
        app::ActionsUi actions(config, config_path);

        Session s(dir, std::string(pack));
        s.host.conn = &rig.conn;
        s.host.actions = &actions;
        s.host.layout_profile_pack = [&] { return std::pair{&s.lua, pack}; };
        s.lua.set_character_settings(dir / "pack.txt");
        s.lua.set_entry_scripts({"layoutprofiles.lua"});
        s.display_size = {1280, 720};
        s.frames(2);
        auto run = [&](const char* code) {
            INFO(std::string(code));
            const auto rc = luaL_dostring(s.lua.state(), code);
            REQUIRE_MESSAGE(rc == LUA_OK, std::string(rc == LUA_OK ? "" : lua_tostring(s.lua.state(), -1)));
        };
        REQUIRE(s.lua.set_setting("hud.locked", "1"));
        run("assert(hs.UI.layoutProfiles.Save('Desktop')); assert(hs.UI.layoutProfiles.Save('P'))");
        const int forward = static_cast<int>(game::KeyCommand::FORWARD);
        const game::KeyCombo remap{0x11, true, true, false};
        REQUIRE(actions.keymap().bind(forward, 0, remap));
        REQUIRE(s.lua.set_setting("hud.locked", "0"));
        REQUIRE(s.lua.set_setting("tasks.tracked.800", "1"));
        run(R"lua(
            local wrapped = ui.TextWrapped
            local input = ui.InputTextWithHint
            shown = {}
            ui.TextWrapped = function(text)
              shown[text] = true
              return wrapped(text)
            end
            ui.InputTextWithHint = function(label, ...)
              local text, entered = input(label, ...)
              if label == '##save-before-name' and nextName then
                text, entered, nextName = nextName, false, nil
              end
              return text, entered
            end
            local function press(control, clicked)
              if control ~= pressControl then return clicked end
              pressControl = nil
              return true
            end
            local button, invisible = ui.Button, ui.InvisibleButton
            ui.Button = function(label, ...)
              return press(label, button(label, ...))
            end
            ui.InvisibleButton = function(label, ...)
              return press(label, invisible(label, ...))
            end
            hs.ShowWindow('LayoutProfilesWindow', true)
        )lua");
        s.frames(2);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK(s.lua.setting("hud.locked") == "0");
        run(R"lua(
            local profiles = hs.UI.layoutProfiles
            assert(shown['Chat includes tabs, filters and colors.'])
            assert(not profiles.CanUndo())
        )lua");
        auto press = [&](const char* modern, const char* gamepad) {
            lua_pushstring(s.lua.state(), pack == "modern" ? modern : gamepad);
            lua_setglobal(s.lua.state(), "pressControl");
            s.frames(2);
            run("assert(pressControl == nil)");
        };
        auto request_apply = [&] {
            run("shown = {}");
            press("Apply Profile", "##profile-apply");
            run(R"lua(
              local found = false
              for text in pairs(shown) do
                if text:find('Save the current layout before applying', 1, true) then found = true end
              end
              assert(found and not hs.UI.layoutProfiles.CanUndo())
            )lua");
            CHECK(s.lua.setting("hud.locked") == "0");
        };
        request_apply();
        press("Cancel", "##cancel-layout-apply");
        CHECK(s.lua.setting("hud.locked") == "0");
        run("assert(#hs.UI.layoutProfiles.List() == 2 and not hs.UI.layoutProfiles.CanUndo())");

        request_apply();
        press("Apply Without Saving", "##apply-without-saving");
        CHECK(s.lua.setting("hud.locked") == "1");
        run("assert(#hs.UI.layoutProfiles.List() == 2 and shown['Applied Desktop.'])");
        press("Undo Apply", "##profile-undo");
        CHECK(s.lua.setting("hud.locked") == "0");
        run(R"lua(
            assert(not hs.UI.layoutProfiles.CanUndo())
            assert(shown['Restored the previous setup.'])
        )lua");

        request_apply();
        if (pack == "modern") {
            run("nextName = 'dEsKtop'");
            press("Save & Apply", "##save-before-apply");
            CHECK(s.lua.setting("hud.locked") == "0");
            run("assert(shown['Choose a different name from the profile you are applying.'])");
            run("nextName = 'P'");
        } else {
            press("", "##save-before-name");
            press("", "##kb2_10"); // P, with the keyboard's first-letter shift
            press("", "##kbok");
        }
        press("Save & Apply", "##save-before-apply");
        CHECK(s.lua.setting("hud.locked") == "0"); // An existing profile requires confirmation.
        run("assert(not hs.UI.layoutProfiles.CanUndo())");
        press("Replace & Apply", "##confirmyes");
        CHECK(s.lua.setting("hud.locked") == "1");
        run("assert(shown['Applied Desktop.'] and hs.UI.layoutProfiles.CanUndo())");
        CHECK(s.lua.setting("tasks.tracked.800") == "1");

        // The applied layouts already have matching saved snapshots, so neither switch needs a prompt.
        run("assert(hs.UI.layoutProfiles.Preview('P')); assert(hs.UI.layoutProfiles.Apply('P'))");
        s.frames(2);
        CHECK(s.lua.setting("hud.locked") == "0");
        run("assert(hs.UI.layoutProfiles.Preview('Desktop')); assert(hs.UI.layoutProfiles.Apply('Desktop'))");
        s.frames(2);
        CHECK(s.lua.setting("hud.locked") == "1");
        CHECK(actions.keymap().binding(forward, 0) == remap);

        // Another client overwrites the profile after the preview: Apply uses the file as it is now.
        run("assert(hs.UI.layoutProfiles.Preview('Desktop'))");
        const auto saved_dir = dir / "layout-profiles" / platform::ui_settings_file(pack).stem();
        auto later = ui::read_layout_profile(saved_dir, "Desktop");
        REQUIRE(later);
        const auto at = later->pack_settings.find("hud.locked=1\n");
        REQUIRE(at != std::string::npos);
        later->pack_settings.replace(at, 13, "hud.locked=0\n");
        REQUIRE(ui::save_layout_profile(saved_dir, *later, true) == ui::LayoutProfileSave::Saved);
        run("assert(hs.UI.layoutProfiles.Apply('Desktop', true))");
        s.frames(2);
        CHECK(s.lua.setting("hud.locked") == "0");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
}

TEST_CASE("UI packs: evolving transfer reviews server previews and drawn confirmation sends once") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        const bool gamepad_pack = std::string_view(pack) == "gamepad";
        test::ScratchDir scratch("ui_pack_evolving_transfer_test");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        std::ofstream(dir / "UI_Test_Server.ini") << "[Tips]\nShowTips=0\nShowTOTD=0\n";
        ImGuiContext* ctx = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            test::Rig rig;
            REQUIRE(rig.to_zone());
            Session s(dir, pack);
            s.host.conn = &rig.conn;
            auto item = [](std::uint32_t id, std::uint32_t record, int slot, std::string name) {
                net::Item value;
                value.id = id;
                value.name = std::move(name);
                value.is_evolving = 1;
                value.instance_id = record;
                value.slot = net::ItemSlot::possessions(static_cast<std::int16_t>(slot));
                value.evolving.evolve_level = 2;
                value.evolving.evolve_max_level = 5;
                value.evolving.progress = 12.5;
                return value;
            };
            const auto source = item(101, 11, 23, "Source evolving item");
            const auto destination = item(102, 12, 24, "Destination evolving item");
            rig.conn.receive_zone_packet_for_test(net::encode_char_inventory({source, destination}));
            rig.conn.open_evolving_transfer();
            REQUIRE(rig.conn.select_evolving_transfer(true, 11));
            REQUIRE(rig.conn.select_evolving_transfer(false, 12));
            auto projected_source = source;
            projected_source.name = "Projected source";
            projected_source.evolving.evolve_level = 1;
            auto projected_destination = destination;
            projected_destination.name = "Projected destination";
            projected_destination.evolving.evolve_level = 4;
            rig.conn.receive_zone_packet_for_test(net::encode(net::EvolvingTransferDetails{
                11, 12, 75, 4, projected_source, projected_destination}));
            REQUIRE(rig.conn.can_transfer_evolving());
            lua_State* L = s.lua.state();
            auto run = [&](const char* code) {
                INFO(code);
                const int rc = luaL_dostring(L, code);
                REQUIRE_MESSAGE(rc == LUA_OK, (rc == LUA_OK ? "" : lua_tostring(L, -1)));
                lua_settop(L, 0);
            };
            run(R"lua(
                controls, lines, padPress = {}, {}, nil
                local function record(label)
                  if not label:find('evolve-',1,true) then return end
                  local key=label:match('##(.*)$')
                  controls[key]={rect={ui.GetItemRect()},id=ui.GetItemID()}
                end
                local button,invisible,text=ui.Button,ui.InvisibleButton,ui.Text
                ui.Button=function(label,...)
                  local clicked=button(label,...);record(label);return clicked
                end
                ui.InvisibleButton=function(label,...)
                  local clicked=invisible(label,...);record(label);return clicked
                end
                ui.Text=function(value,...)
                  lines[value]=true;return text(value,...)
                end
                ui.SetSetting('hud.size.ItemExpTransferWnd','760,620')
                ui.SetSetting('test.pos.ItemExpTransferWnd','20,20')
                hs.ShowWindow('TipWindow',false)
            )lua");
            if (gamepad_pack) run(R"lua(
                hs.gamepad={
                  connected=function() return true end,type=function() return 'xbox' end,
                  mode=function() return 'menu' end,set_mode=function() return 'menu' end,
                  pressed=function(button) return button==padPress end,
                  down=function(button) return button==padPress end,released=function() return false end,
                  axis=function() return 0 end,glyph=function(button) return button:upper() end,
                  takes_b=function() end,request_focus=function(window) ui.SetWindowFocus(window) end,
                }
            )lua");
            s.frames(30); // the gamepad's initial window-focus settling period
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            run("assert(lines['Compatibility: 75%'] and lines['Max transfer level: 4'])");
            run("assert(lines['Source after transfer: Projected source (level 1/5, 12.5% experience)'])");
            run("assert(lines['Destination after transfer: Projected destination (level 4/5, 12.5% experience)'])");
            auto rectangle = [&](const char* key) {
                lua_getglobal(L, "controls");
                lua_getfield(L, -1, key);
                REQUIRE(lua_istable(L, -1));
                lua_getfield(L, -1, "rect");
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
            auto click = [&](const char* key) {
                const auto r = rectangle(key);
                io.AddMousePosEvent((r.x + r.z) / 2, (r.y + r.w) / 2);
                s.frames(1);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                s.frames(1);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                s.frames(2);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            };
            auto transfer_count = [&] {
                int count = 0;
                for (const auto& packet : rig.server.zone_log)
                    if (const auto request = net::decode_evolving_request(packet);
                        request && request->action == net::EvolvingAction::Transfer) ++count;
                return count;
            };
            click("evolve-transfer");
            run("assert(lines['Confirm experience transfer'] and controls['evolve-confirm'])");
            CHECK_FALSE(rig.conn.evolving_transfer().submitted);
            click("evolve-cancel");
            CHECK(rig.conn.can_transfer_evolving());
            CHECK_FALSE(rig.conn.evolving_transfer().submitted);
            if (gamepad_pack) {
                click("evolve-transfer");
                run("padPress='b'");
                io.AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
                s.frames(1);
                run("padPress=nil");
                io.AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
                s.frames(2);
                CHECK(rig.conn.evolving_transfer().shown);
                CHECK(rig.conn.can_transfer_evolving());
                CHECK_FALSE(rig.conn.evolving_transfer().submitted);
            }
            rig.run_for(std::chrono::milliseconds(20));
            CHECK(transfer_count() == 0);
            click("evolve-transfer");
            if (gamepad_pack) {
                // Put ImGui's navigation on the actual drawn Confirm item, then activate it with A.
                lua_getglobal(L, "controls");
                lua_getfield(L, -1, "evolve-confirm");
                lua_getfield(L, -1, "id");
                const auto id = static_cast<ImGuiID>(lua_tointeger(L, -1));
                lua_settop(L, 0);
                ImGuiWindow* content = nullptr;
                for (ImGuiWindow* window : ctx->Windows)
                    if (window->Active && std::string_view(window->Name).find("##evolve-content") != std::string_view::npos)
                        content = window;
                REQUIRE(content);
                ImGui::FocusWindow(content);
                ImGui::SetFocusID(id, content);
                ImGui::SetNavCursorVisible(true);
                io.AddMousePosEvent(-100, -100);
                s.frames(1);
                io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
                s.frames(1);
                io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
                s.frames(2);
            } else click("evolve-confirm");
            CHECK(rig.conn.evolving_transfer().submitted);
            REQUIRE(rig.until([&] { return transfer_count() == 1; }));
            s.frames(3);
            rig.run_for(std::chrono::milliseconds(20));
            CHECK(transfer_count() == 1);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: active discipline shows seconds and its icon cancels on a mouse click") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        test::ScratchDir scratch("ui_pack_discipline_test");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
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
discActive, discFraction, discTime, endClicks = true, 1, "1:00", 0
local shown, replace, child, text, art, gauge, click = hs.WindowShown, hs.ReplaceWindow,
  hs.ChildShown, hs.WindowText, hs.ChildArt, hs.Gauge, hs.ClickChild
hs.WindowShown = function(win) if win == "CombatAbilityWnd" then return true end return shown(win) end
hs.ReplaceWindow = function(win)
  if win == "CombatAbilityWnd" then return true, 40, 40, 360, 190 end
  return replace(win)
end
hs.ChildShown = function(win, id)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectButton" then return discActive end
  return child(win, id)
end
hs.WindowText = function(win, id)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectLabel" then return "Test Discipline" end
  return text(win, id)
end
hs.ChildArt = function(win, id, kind)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectButton" then return nil end
  return art(win, id, kind)
end
hs.Gauge = function(kind)
  if kind == 26 then gaugeReads = (gaugeReads or 0) + 1; return discFraction, discTime end
  return gauge(kind)
end
hs.ClickChild = function(win, id, button)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectButton" then
    assert(button == "left")
    endClicks, discActive = endClicks + 1, false
    return true
  end
  return click(win, id, button)
end
local button, drawText = ui.InvisibleButton, ui.DrawText
ui.InvisibleButton = function(id, ...)
  local hit = button(id, ...)
  if id == "##cawEffect" then discIcon = { ui.GetItemRect() } end
  return hit
end
ui.DrawText = function(x, y, r, g, b, a, value)
  if value == discTime then drewDiscTime = true end
  return drawText(x, y, r, g, b, a, value)
end
)lua");
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(run("return tostring(drewDiscTime and gaugeReads > 0 and discIcon ~= nil)") == "true");
            run("discFraction, discTime, drewDiscTime = 0.5, '30s', false");
            s.frames(2);
            CHECK(run("return tostring(drewDiscTime)") == "true");
            lua_getglobal(L, "discIcon");
            float rect[4]{};
            for (int i = 0; i < 4; ++i) {
                lua_rawgeti(L, -1, i + 1);
                rect[i] = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            lua_settop(L, 0);
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            CHECK(run("return tostring(endClicks)") == "1");
            run("gaugeReads, drewDiscTime, discIcon = 0, false, nil");
            s.frames(2);
            CHECK(run("return tostring(gaugeReads == 0 and not drewDiscTime and discIcon == nil)") == "true");
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: the gamepad's first focus is the discipline list, and LB ends the active discipline") {
    test::ScratchDir scratch("ui_pack_discipline_pad_test");
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
        // A pad in menu mode; padPress is the button that went down this frame.
        run(R"lua(
padPress, windowShown, discActive, endClicks, uses = nil, false, true, 0, 0
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
local shown, replace, child, text, art, click = hs.WindowShown, hs.ReplaceWindow, hs.ChildShown, hs.WindowText,
  hs.ChildArt, hs.ClickChild
hs.WindowShown = function(win) if win == "CombatAbilityWnd" then return windowShown end return shown(win) end
hs.ReplaceWindow = function(win)
  if win == "CombatAbilityWnd" then return true, 40, 40, 360, 190 end
  return replace(win)
end
hs.ChildShown = function(win, id)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectButton" then return discActive end
  return child(win, id)
end
hs.WindowText = function(win, id)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectLabel" then return "Test Discipline" end
  return text(win, id)
end
hs.ChildArt = function(win, id, kind)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectButton" then return nil end
  return art(win, id, kind)
end
hs.Gauge = function() return 0.5, "30s" end
hs.Disciplines = function()
  return { { index = 0, spell = 101, name = "Guard", icon = 0, level = 50, endurance = 0, reuse = 60,
    remaining = 0, timer = 1 } }
end
hs.PressButtonFrom = function() uses = uses + 1 end
hs.ClickChild = function(win, id, button)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectButton" then
    endClicks, discActive = endClicks + 1, false
    return true
  end
  return click(win, id, button)
end
local button = ui.InvisibleButton
ui.InvisibleButton = function(id, ...)
  local hit = button(id, ...)
  if id == "##cawEffect" then effectFocused = ui.IsItemFocused() end
  if id == "##row" then rowFocused = ui.IsItemFocused() end
  return hit
end
)lua");
        s.frames(240); // the pack takes the pad's focus for a window only from 3 seconds after zoning in
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        run("windowShown = true");
        s.frames(30); // the window appears with the focus; past its settle time
        CHECK(run("return tostring(rowFocused == true and effectFocused == false)") == "true");
        auto& io = ImGui::GetIO();
        // A press of A uses the focused discipline; the running one is not ended. (ImGui activates only with its
        // navigation cursor shown, as it is once the pad has moved it.)
        ImGui::SetNavCursorVisible(true);
        io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
        s.frames(1);
        io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
        s.frames(2);
        CHECK(run("return tostring(endClicks == 0 and uses == 1)") == "true");
        // LB ends it.
        run("padPress = 'lb'");
        s.frames(1);
        run("padPress = nil");
        s.frames(2);
        CHECK(run("return tostring(endClicks)") == "1");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs: offered-task starting-zone hint and explicit preview follow the selected offer") {
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        test::ScratchDir scratch("ui_pack_task_offer_rewards");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
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
offerSelected, offerPreviewEnabled, offerPreviewClicks = 0, true, {}
offerWindowShown, activeTaskShown, activePreviewEnabled, activePreviewClicks = true, false, true, 0
offerTexts = {}
assert(type(hs.ChildState) == "function", "ChildState must be registered before the fixture wraps it")
local shown, replace, childShown, childState, text, list, row, listClick, click =
  hs.WindowShown, hs.ReplaceWindow, hs.ChildShown, hs.ChildState,
  hs.WindowText, hs.ListInfo, hs.ListRow, hs.ListClick, hs.ClickChild
hs.WindowShown = function(win)
  if win == "TaskSelectWnd" then return offerWindowShown end
  if win == "TaskWnd" then return activeTaskShown end
  return shown(win)
end
hs.ReplaceWindow = function(win)
  if win == "TaskSelectWnd" then return true, 40, 40, 600, 400 end
  return replace(win)
end
hs.ChildShown = function(win, id)
  if win == "TaskSelectWnd" then return true end
  return childShown(win, id)
end
hs.ChildState = function(win, id)
  if win == "TaskSelectWnd" then
    if id == "TSEL_PreviewRewardButton" then return offerPreviewEnabled, false end
    return true, false
  end
  if win == "TaskWnd" and id == "TASK_PreviewRewardButton" then return activePreviewEnabled, false end
  return childState(win, id)
end
hs.ListInfo = function(win, id)
  if win == "TaskSelectWnd" and id == "TaskList" then return 2, 2, offerSelected end
  if win == "TaskWnd" then
    if id == "TASK_TaskList" then return 1, 3, 0 end
    if id == "TASK_TaskElementList" then return 1, 3, -1 end
  end
  return list(win, id)
end
hs.ListRow = function(win, id, index)
  if win == "TaskSelectWnd" and id == "TaskList" then
    return {index == 0 and "First offer" or "Second offer", ""}
  end
  if win == "TaskWnd" then
    assert(index == 0)
    if id == "TASK_TaskList" then return {"Q", "Active task", "Unlimited"} end
    if id == "TASK_TaskElementList" then return {"ActiveObjective", "0/1", "North Qeynos"} end
  end
  return row(win, id, index)
end
hs.ListClick = function(win, id, index, ...)
  if win == "TaskSelectWnd" and id == "TaskList" then offerSelected = index; return end
  if win == "TaskWnd" and id == "TASK_TaskList" then assert(index == 0); return end
  return listClick(win, id, index, ...)
end
hs.WindowText = function(win, id)
  if win == "TaskSelectWnd" and id == "Description" then
    return offerSelected == 0 and "FirstDescription<br>TaskBeginsInFirst" or "SecondDescription<br>TaskBeginsInSecond"
  end
  if win == "TaskWnd" and id == "TASK_Description" then return "ActiveTaskDescription" end
  return text(win, id)
end
hs.ClickChild = function(win, id, button)
  if win == "TaskSelectWnd" then
    assert(id == "TSEL_PreviewRewardButton" and button == "left", "Preview must use its matching native action")
    offerPreviewClicks[#offerPreviewClicks + 1] = offerSelected
    return true
  end
  if win == "TaskWnd" then
    assert(id == "TASK_PreviewRewardButton" and button == "left", "Active preview must use its matching native action")
    activePreviewClicks = activePreviewClicks + 1
    return true
  end
  return click(win, id, button)
end
local button, invisible, colored, wrapped, drawText = ui.Button, ui.InvisibleButton, ui.TextColored, ui.TextWrapped, ui.DrawText
ui.Button = function(label, ...)
  local hit = button(label, ...)
  if label == "Preview Reward##TSEL_PreviewRewardButton" then offerPreviewRect = {ui.GetItemRect()} end
  return hit
end
ui.InvisibleButton = function(id, ...)
  local hit = invisible(id, ...)
  if id == "##offerpreview" or id == "##taskpreview" then offerPreviewRect = {ui.GetItemRect()} end
  return hit
end
ui.TextColored = function(r, g, b, a, value)
  offerTexts[value] = true
  return colored(r, g, b, a, value)
end
ui.TextWrapped = function(value)
  offerTexts[value] = true
  return wrapped(value)
end
ui.DrawText = function(x, y, r, g, b, a, value)
  offerTexts[value] = true
  return drawText(x, y, r, g, b, a, value)
end
)lua");
            s.frames(20);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(run("return tostring(offerTexts.TaskBeginsInFirst and offerPreviewRect ~= nil and #offerPreviewClicks == 0)") == "true");
            auto click_preview = [&] {
                lua_getglobal(L, "offerPreviewRect");
                REQUIRE(lua_istable(L, -1));
                float rect[4]{};
                for (int i = 0; i < 4; ++i) {
                    lua_rawgeti(L, -1, i + 1);
                    rect[i] = static_cast<float>(lua_tonumber(L, -1));
                    lua_pop(L, 1);
                }
                lua_settop(L, 0);
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2);
                s.frames(1);
                io.AddMouseButtonEvent(0, true);
                s.frames(1);
                io.AddMouseButtonEvent(0, false);
                s.frames(1);
            };
            click_preview();
            CHECK(run("return tostring(#offerPreviewClicks == 1 and offerPreviewClicks[1] == 0)") == "true");
            run("offerSelected, offerTexts = 1, {}");
            s.frames(2);
            CHECK(run("return tostring(offerTexts.TaskBeginsInSecond and not offerTexts.TaskBeginsInFirst and #offerPreviewClicks == 1)") == "true");
            click_preview();
            CHECK(run("return tostring(#offerPreviewClicks == 2 and offerPreviewClicks[2] == 1)") == "true");
            run("offerPreviewEnabled = false");
            s.frames(2);
            click_preview();
            CHECK(run("return tostring(#offerPreviewClicks)") == "2");
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            if (std::string_view(pack) == "gamepad") {
                run("offerWindowShown, activeTaskShown, offerPreviewRect, offerTexts = false, true, nil, {}");
                s.frames(20);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
                CHECK(run("return tostring(offerTexts.ActiveTaskDescription and offerTexts.ActiveObjective and offerPreviewRect ~= nil and activePreviewClicks == 0)") == "true");
                click_preview();
                CHECK(run("return tostring(activePreviewClicks)") == "1");
                run("activePreviewEnabled = false");
                s.frames(2);
                click_preview();
                CHECK(run("return tostring(activePreviewClicks == 1 and #offerPreviewClicks == 2)") == "true");
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            }
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: linked discipline cooldowns show red fills and matching seconds until ready") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        test::ScratchDir scratch("ui_pack_discipline_cooldown_test");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
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
sharedLeft, expectedTime, timerDraws, redArea, redFills, abilityClicks = 600, "10:00", 0, 0, 0, 0
local shown, replace, child, text, cooldown, click = hs.WindowShown, hs.ReplaceWindow,
  hs.ChildShown, hs.WindowText, hs.ButtonCoolDown, hs.ClickChild
local names, reuse = { "Long Guard", "Short Guard", "Independent" }, { 600, 300, 120 }
hs.WindowShown = function(win) if win == "CombatAbilityWnd" then return true end return shown(win) end
hs.ReplaceWindow = function(win)
  if win == "CombatAbilityWnd" then return true, 40, 40, 360, 190 end
  return replace(win)
end
hs.ChildShown = function(win, id)
  if win == "CombatAbilityWnd" and id == "CAW_CombatEffectButton" then return false end
  return child(win, id)
end
hs.WindowText = function(win, id)
  local n = id:match("^CAW_Button(%d)$")
  if win == "CombatAbilityWnd" and n then return names[tonumber(n)] or "" end
  return text(win, id)
end
hs.ButtonCoolDown = function(win, id, y0, y1)
  local n = id:match("^CAW_Button(%d)$")
  n = n and tonumber(n)
  if win == "CombatAbilityWnd" and n and n <= 2 and sharedLeft > 0 then
    return y0, 0, 0, 0, 0.25, sharedLeft * 1000, reuse[n] * 1000
  end
  return cooldown(win, id, y0, y1)
end
local row = 0
hs.Disciplines = function()
  row = 0
  local out = {}
  for i = 1, 3 do
    out[i] = { index = i - 1, spell = 100 + i, name = names[i], icon = 0, level = 50,
      endurance = 0, reuse = reuse[i], remaining = i <= 2 and sharedLeft or 0, timer = i <= 2 and 1 or 2 }
  end
  return out
end
hs.ClickChild = function(win, id, button)
  if win == "CombatAbilityWnd" and id == "CAW_Button1" and button == "left" then
    abilityClicks = abilityClicks + 1; return true
  end
  return click(win, id, button)
end
hs.PressButtonFrom = function(win, id, n)
  assert(win == "CombatAbilityWnd" and id == "Discipline" and n == 1)
  abilityClicks = abilityClicks + 1
end
local button, rect, drawText = ui.InvisibleButton, ui.DrawRectFilled, ui.DrawText
ui.InvisibleButton = function(id, ...)
  local hit = button(id, ...)
  if id == "##row" then row = row + 1 end
  if id == "##caw1" or (id == "##row" and row == 1) then firstAbility = { ui.GetItemRect() } end
  return hit
end
ui.DrawRectFilled = function(x0, y0, x1, y1, r, g, b, a, ...)
  if a and math.abs(a - 0.20) < 0.0001 and r > g * 1.5 and r > b * 1.5 then
    redFills, redArea = redFills + 1, redArea + (x1 - x0) * (y1 - y0)
  end
  return rect(x0, y0, x1, y1, r, g, b, a, ...)
end
ui.DrawText = function(x, y, r, g, b, a, value)
  if value == expectedTime then timerDraws = timerDraws + 1 end
  return drawText(x, y, r, g, b, a, value)
end
)lua");
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            run("redFills, redArea, timerDraws = 0, 0, 0");
            s.frames(1);
            CHECK(run("return tostring(redFills == 2 and timerDraws >= 2)") == "true");
            run("fullArea = redArea");
            lua_getglobal(L, "firstAbility");
            float rect[4]{};
            for (int i = 0; i < 4; ++i) {
                lua_rawgeti(L, -1, i + 1);
                rect[i] = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            lua_settop(L, 0);
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            CHECK(run("return tostring(abilityClicks)") == "1"); // The normal use handler still receives the click.
            run("sharedLeft, expectedTime, redFills, redArea, timerDraws = 30, '0:30', 0, 0, 0");
            s.frames(1);
            CHECK(run("return tostring(redFills == 2 and timerDraws >= 2 and redArea < fullArea)") == "true");
            run("sharedLeft, redFills, timerDraws = 0, 0, 0");
            s.frames(1);
            CHECK(run("return tostring(redFills == 0 and timerDraws == 0)") == "true");
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: server popup table rows and columns keep their formatting") {
    constexpr std::string_view text =
        "Intro<br><TABLE><Tr><TD><c \"#12AB34\">Name&amp;Kind</c></TD><td>Status</td><td>Score</td></Tr>"
        "<tr><td></td><td>Ready<br>Now</td><td>42</td></tr></TABLE><br>Outro";
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        const fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        test::ScratchDir scratch("ui_pack_popup_table");
        const fs::path dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::create_directories(dir / pack);
        fs::copy(src / "lib", dir / pack / "lib", fs::copy_options::recursive);
        fs::copy_file(src / "largedialog.lua", dir / pack / "largedialog.lua");
        std::unique_ptr<ImGuiContext, void(*)(ImGuiContext*)> context(ImGui::CreateContext(), ImGui::DestroyContext);
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        Session s(dir, pack);
        s.frames(2);
        lua_State* L = s.lua.state();
        lua_pushcfunction(L, [](lua_State* S) {
            const auto& color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            lua_pushnumber(S, color.x);
            lua_pushnumber(S, color.y);
            lua_pushnumber(S, color.z);
            return 3;
        });
        lua_setglobal(L, "popup_text_color");
        const auto run = [L](const char* script) {
            const int status = luaL_dostring(L, script);
            const char* error = status == LUA_OK ? "" : lua_tostring(L, -1);
            REQUIRE_MESSAGE(status == LUA_OK, error);
        };
        run(R"lua(
            popup_text = {}
            local function record(text, x, y, r, g, b)
              if text and text ~= "" then popup_text[text] = { x = x, y = y, r = r, g = g, b = b } end
            end
            local textWrapped, textColored, drawText = ui.TextWrapped, ui.TextColored, ui.DrawText
            ui.TextWrapped = function(text)
              local x, y = ui.GetCursorScreenPos()
              record(text, x, y, popup_text_color())
              return textWrapped(text)
            end
            ui.TextColored = function(r, g, b, a, text)
              local x, y = ui.GetCursorScreenPos()
              record(text, x, y, r, g, b)
              return textColored(r, g, b, a, text)
            end
            ui.DrawText = function(x, y, r, g, b, a, text, ...)
              record(text, x, y, r, g, b)
              return drawText(x, y, r, g, b, a, text, ...)
            end
        )lua");

        ByteWriter w;
        for (const std::string_view field : std::array<std::string_view, 4>{"Results", text, "", ""}) {
            w.le<std::uint32_t>(static_cast<std::uint32_t>(field.size() + 1));
            w.fixed_string(field, field.size() + 1);
        }
        w.zeros(22); // OK button, no sound controls, no duration or response ids
        s.conn.receive_zone_packet_for_test(net::AppPacket(net::to_wire(net::Op::OnLevelMessage), w.take()));
        REQUIRE(s.conn.large_dialog());
        s.frames(3);
        REQUIRE(s.lua.load_status().error_count == 0);
        run(R"lua(
            local function seen(text) return assert(popup_text[text], "Popup did not draw " .. text) end
            local name, status, score = seen("Name&Kind"), seen("Status"), seen("Score")
            local ready, now, value = seen("Ready"), seen("Now"), seen("42")
            local function near(a, b) return math.abs(a - b) < 1 end
            assert(name.x < status.x and status.x < score.x and near(name.y, status.y) and near(status.y, score.y),
              "The three cells share the first row and occupy distinct columns")
            assert(near(status.x, ready.x) and near(ready.x, now.x) and near(score.x, value.x)
              and near(ready.y, value.y), "The empty cell keeps the second row aligned to the first")
            assert(seen("Intro").y < name.y and name.y < ready.y and ready.y < now.y and now.y < seen("Outro").y,
              "The rows, in-cell break and surrounding prose stay in order")
            assert(math.abs(name.r - 0x12/255) < 0.001 and math.abs(name.g - 0xAB/255) < 0.001
              and math.abs(name.b - 0x34/255) < 0.001, "The cell keeps its STML color")
        )lua");
    }
}

TEST_CASE("UI packs: hover respawn draws available and unavailable choices without script errors") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        const fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        test::ScratchDir scratch("ui_pack_respawn_test");
        const fs::path dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            test::ManualClock clock;
            clock.hold();
            net::ConnectionOptions options;
            options.session.clock = clock.source();
            Session s(dir, pack, std::move(options));
            s.conn.receive_zone_packet_for_test(respawn_window_packet(1));
            clock.advance(std::chrono::milliseconds(1));
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            ImGuiWindow* respawn = ImGui::FindWindowByName(std::string_view(pack) == "modern"
                ? "###modern.RespawnWnd" : "Respawn");
            REQUIRE(respawn);
            CHECK(respawn->Active);
            CHECK(s.api.child_state("RespawnWnd", "SelectButton") == true);
            CHECK(s.api.list_texts("RespawnWnd", "OptionsList") == std::vector<std::string>{"*", "r"});

            CHECK(s.api.click_list("RespawnWnd", "OptionsList", 1, 0));
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(s.api.child_state("RespawnWnd", "SelectButton") == false);
            CHECK(s.api.click_child("RespawnWnd", "SelectButton"));
            CHECK_FALSE(s.conn.respawn_chosen());

            s.conn.receive_zone_packet_for_test(respawn_window_packet(0));
            clock.advance(std::chrono::milliseconds(1));
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(s.api.child_state("RespawnWnd", "SelectButton") == true);
            CHECK(s.api.list_texts("RespawnWnd", "OptionsList") == std::vector<std::string>{"*", ""});
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: the respawn window leaves the focus alone for a question that is not a resurrection") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        const bool gamepad_pack = std::string_view(pack) == "gamepad";
        const fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        test::ScratchDir scratch("ui_pack_respawn_plain_question_test");
        const fs::path dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            test::ManualClock clock;
            clock.hold();
            net::ConnectionOptions options;
            options.session.clock = clock.source();
            Session s(dir, pack, std::move(options));
            s.conn.receive_zone_packet_for_test(respawn_window_packet(1));
            clock.advance(std::chrono::milliseconds(1));
            s.frames(3);
            app::UiHost::Confirm q;
            q.text = "A question of a window's own";
            s.host.confirm = std::move(q);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            ImGuiWindow* confirm = ImGui::FindWindowByName(gamepad_pack ? "Confirm" : "###modern.ConfirmationDialogBox");
            ImGuiWindow* respawn = ImGui::FindWindowByName(gamepad_pack ? "Respawn" : "###modern.RespawnWnd");
            REQUIRE(confirm);
            REQUIRE(respawn);
            CHECK(confirm->Active);
            REQUIRE(luaL_dostring(s.lua.state(),
                "assert(hs.ChildShown('ConfirmationDialogBox', 'ResurrectionOffer') == false)") == LUA_OK);
            ImGui::FocusWindow(respawn);
            s.frames(3);
            REQUIRE(GImGui->NavWindow);
            CHECK(GImGui->NavWindow->RootWindow == respawn);
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: a resurrection offer uses the pack's confirmation from the first frame") {
    for (const char* pack : {"modern", "gamepad", "custom"}) {
        INFO(pack);
        const bool custom = std::string_view(pack) == "custom";
        const bool gamepad = std::string_view(pack) == "gamepad";
        const fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / (gamepad ? "gamepad" : "modern");
        test::ScratchDir scratch("ui_pack_resurrection_confirmation");
        const fs::path dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        if (custom) REQUIRE(fs::remove(dir / pack / "confirmationdialogbox.lua"));
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        {
            test::Rig rig;
            REQUIRE(rig.to_zone());
            auto& conn = rig.conn;
            Session s(dir, pack);
            s.host.conn = &conn;
            ReplacementCheck replacements(s.api);

            auto spells = std::make_shared<game::SpellTable>();
            std::vector<std::string> fields(237, "0");
            fields[0] = "100";
            fields[1] = "Test Resurrection";
            fields[20] = "96"; // first effect's base value
            fields[86] = "81"; // first effect: resurrection
            std::string spell_line = fields[0];
            for (std::size_t i = 1; i < fields.size(); ++i) spell_line += '^' + fields[i];
            REQUIRE(spells->parse(spell_line) == 1);
            conn.set_spell_table_for_test(spells);
            REQUIRE(luaL_dostring(s.lua.state(), R"lua(
local TextWrapped, TextColored, DrawText = ui.TextWrapped, ui.TextColored, ui.DrawText
local function observe(text)
  if type(text) == "string" and text:find("(96 percent)", 1, true) then RezPercentPainted = true end
end
ui.TextWrapped = function(text, ...)
  observe(text)
  return TextWrapped(text, ...)
end
ui.TextColored = function(r, g, b, a, text, ...)
  observe(text)
  return TextColored(r, g, b, a, text, ...)
end
ui.DrawText = function(x, y, r, g, b, a, text, ...)
  observe(text)
  return DrawText(x, y, r, g, b, a, text, ...)
end
)lua") == LUA_OK);

            net::Death death;
            death.spawn_id = death.corpse_id = conn.world().player_id;
            death.killer_id = 101;
            death.damage = 5000;
            conn.receive_zone_packet_for_test(net::encode(death));
            conn.receive_zone_packet_for_test(respawn_window_packet(1));
            conn.receive_zone_packet_for_test(resurrection_offer_packet());
            REQUIRE(conn.confirmation());
            CHECK_FALSE(s.api.replaces("ConfirmationDialogBox"));

            app::MarketUi market;
            ImGui::GetIO().DisplaySize = s.display_size;
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
            auto frame = [&] {
                ImGui::NewFrame();
                market.draw(conn, false);
                s.api.begin_frame();
                s.lua.frame();
                market.draw_confirmation_fallback(conn);
                ImGui::Render();
            };
            frame();
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            ImGuiWindow* respawn = ImGui::FindWindowByName(gamepad ? "Respawn" : "###modern.RespawnWnd");
            REQUIRE(respawn);
            CHECK(respawn->Active);
            ImGuiWindow* fallback = ImGui::FindWindowByName("Confirm###market_confirm");
            if (custom) {
                REQUIRE(fallback);
                CHECK(fallback->Active);
                CHECK_FALSE(s.api.replaces("ConfirmationDialogBox"));
            } else {
                ImGuiWindow* confirm = ImGui::FindWindowByName(gamepad ? "Confirm" : "###modern.ConfirmationDialogBox");
                REQUIRE(confirm);
                CHECK(confirm->Active);
                CHECK(s.api.replaces("ConfirmationDialogBox"));
                CHECK(fallback == nullptr);
                frame();
                REQUIRE(luaL_dostring(s.lua.state(), "assert(RezPercentPainted)") == LUA_OK);
            }
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: resurrection keeps the original respawn choices and timer until an explicit destination") {
    for (const char* pack : {"modern", "gamepad"}) {
        for (int choice : {0, 1, 2}) {
            const bool accept = choice != 0, manual_bind = choice == 2;
            INFO(pack, " accept=", accept, " manual_bind=", manual_bind);
            const bool gamepad_pack = std::string_view(pack) == "gamepad";
            const fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
            test::ScratchDir scratch("ui_pack_respawn_question_test");
            const fs::path dir = scratch.path();
            fs::create_directories(dir / "game");
            fs::copy(src, dir / pack, fs::copy_options::recursive);
            ImGuiContext* ctx = ImGui::CreateContext();
            ImGui::GetIO().IniFilename = nullptr;
            ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
            {
                test::ManualClock clock;
                auto options = test::quick_options();
                options.session.clock = clock.source();
                test::Rig rig(test::MockConfig::standard(), std::move(options));
                REQUIRE(rig.to_zone());
                clock.hold();
                Config config;
                platform::GamepadState state;
                state.connected = true;
                state.type = platform::PadType::Xbox;
                app::Gamepad pad(config, dir / "gamepad.ini");
                pad.set_state(&state);
                Session s(dir, pack);
                auto& conn = rig.conn;
                s.host.conn = &conn;
                s.host.gamepad = &pad;
                // Observe the actual pack's paint and selected-row input, rather than only its shared model.
                REQUIRE(luaL_dostring(s.lua.state(), R"lua(
local TextColored, DrawText, ListInfo = ui.TextColored, ui.DrawText, hs.ListInfo
ui.TextColored = function(r, g, b, a, text, ...)
  if text == "Resurrect" then RespawnPaintColor = { r, g, b } end
  return TextColored(r, g, b, a, text, ...)
end
ui.DrawText = function(x, y, r, g, b, a, text, ...)
  if text == "Resurrect" then RespawnPaintColor = { r, g, b } end
  return DrawText(x, y, r, g, b, a, text, ...)
end
hs.ListInfo = function(win, child)
  local rows, columns, selected, labels, widths, multi = ListInfo(win, child)
  if win == "RespawnWnd" and child == "OptionsList" then RespawnPaintSelected = selected end
  return rows, columns, selected, labels, widths, multi
end
)lua") == LUA_OK);

                net::Death death;
                death.spawn_id = death.corpse_id = conn.world().player_id;
                death.killer_id = 101;
                death.damage = 5000;
                conn.receive_zone_packet_for_test(net::encode(death));
                conn.receive_zone_packet_for_test(respawn_window_packet(1));
                if (manual_bind) CHECK(s.api.click_list("RespawnWnd", "OptionsList", 0, 0));
                conn.receive_zone_packet_for_test(resurrection_offer_packet());
                REQUIRE(conn.confirmation());
                REQUIRE(conn.confirmation()->id == net::kConfirmRezz);
                clock.advance(std::chrono::milliseconds(1));
                s.frames(3); // both packets arrived before the first UI frame, within its initial three seconds
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
                ImGuiWindow* confirm = ImGui::FindWindowByName(gamepad_pack ? "Confirm" : "###modern.ConfirmationDialogBox");
                REQUIRE(confirm);
                CHECK(confirm->Active);
                REQUIRE(GImGui->NavWindow);
                CHECK(GImGui->NavWindow->RootWindow == confirm);
                const char* respawn_name = gamepad_pack ? "Respawn" : "###modern.RespawnWnd";
                ImGuiWindow* respawn = ImGui::FindWindowByName(respawn_name);
                REQUIRE(respawn);
                CHECK(respawn->Active);
                CHECK(s.api.replaces("RespawnWnd"));
                REQUIRE(luaL_dostring(s.lua.state(),
                    "assert(RespawnPaintColor and math.abs(RespawnPaintColor[1]-1)<0.001 and "
                    "math.abs(RespawnPaintColor[2]-0.25)<0.001 and math.abs(RespawnPaintColor[3]-0.25)<0.001); "
                    "assert(RespawnPaintSelected==0)") == LUA_OK);
                // Match the app's fallback draw before the Lua pass: the pack must retain ownership while
                // the question is present, so a second "You have died" window cannot replace its original.
                ReplacementCheck replacements(s.api);
                ui::InGameView fallback;
                fallback.respawn.options = {{0, "Bind Location"}, {1, "Resurrect"}};
                fallback.respawn.ready = true;
                s.frames(3, &fallback);
                ImGuiWindow* duplicate = ImGui::FindWindowByName("You have died##hailstone_respawn");
                CHECK((!duplicate || !duplicate->Active));
                CHECK(respawn->Active);
                CHECK(confirm->Active);
                CHECK(GImGui->NavWindow->RootWindow == confirm);
                auto requests = pad.take_focus_requests();
                if (gamepad_pack) {
                    REQUIRE_FALSE(requests.empty());
                    CHECK(requests.back() == "Confirm");
                    CHECK(std::find(requests.begin(), requests.end(), "Respawn") == requests.end());
                }
                REQUIRE(luaL_dostring(s.lua.state(),
                    "assert(hs.ChildShown('ConfirmationDialogBox', 'ResurrectionOffer') == true)") == LUA_OK);
                // Another window taken in hand while the offer waits (typing in chat) keeps the focus: the offer
                // was put in front once, as it appeared, not every frame (which would take the active widget).
                ImGui::FocusWindow(respawn);
                s.frames(3);
                REQUIRE(GImGui->NavWindow);
                CHECK(GImGui->NavWindow->RootWindow == respawn);
                CHECK(confirm->Active);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);

                CHECK(s.api.click_child("ConfirmationDialogBox", accept ? "Yes_Button" : "No_Button"));
                CHECK_FALSE(conn.confirmation());
                CHECK_FALSE(conn.respawn_chosen());
                CHECK(conn.respawn_option_available(0));
                CHECK(conn.respawn_option_available(1) == accept);
                const auto remaining = conn.respawn_ms_left();
                clock.advance(std::chrono::milliseconds(2000));
                s.frames(2);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
                respawn = ImGui::FindWindowByName(respawn_name);
                REQUIRE(respawn);
                CHECK(respawn->Active);
                CHECK(s.api.child_state("RespawnWnd", "SelectButton") == true);
                CHECK(s.api.child_state("RespawnWnd", "OptionsList") == true);
                CHECK(conn.respawn_ms_left() == remaining - 2000);
                CHECK_FALSE(conn.respawn_chosen());
                auto markers = s.api.list_texts("RespawnWnd", "OptionsList");
                REQUIRE(markers.size() == 2);
                CHECK(markers[0] == "*");
                CHECK(markers[1] == "r"); // the original's marker stays; Yes only enables the line
                if (accept) {
                    REQUIRE(luaL_dostring(s.lua.state(),
                        "assert(RespawnPaintColor and math.abs(RespawnPaintColor[1]-0.25)<0.001 and "
                        "math.abs(RespawnPaintColor[2]-1)<0.001 and math.abs(RespawnPaintColor[3]-0.35)<0.001)") == LUA_OK);
                }
                // Yes leaves the selection where it was (0x74e9e0 never changes it).
                REQUIRE(luaL_dostring(s.lua.state(), "assert(RespawnPaintSelected==0)") == LUA_OK);
                if (gamepad_pack) {
                    requests = pad.take_focus_requests();
                    REQUIRE_FALSE(requests.empty());
                    CHECK(requests.back() == "Respawn");
                }
                CHECK(s.api.click_list("RespawnWnd", "OptionsList", accept ? 1 : 0, 0));
                CHECK(s.api.child_state("RespawnWnd", "SelectButton") == true);
                CHECK_FALSE(conn.respawn_chosen());
                CHECK(s.api.click_child("RespawnWnd", "SelectButton"));
                CHECK(conn.respawn_chosen());
                CHECK(s.api.child_state("RespawnWnd", "SelectButton") == false);
                net::ZonePlayerToBind bind;
                bind.zone_id = 2;
                bind.zone_name = accept ? "Resurrection destination" : "Bind destination";
                conn.receive_zone_packet_for_test(net::encode(bind));
                s.frames(2);
                CHECK_FALSE(conn.respawn_window());
                CHECK_FALSE(respawn->Active);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            }
            ImGui::DestroyContext(ctx);
        }
    }
}

TEST_CASE("UI packs (modern): login Add-ons opens within narrow and short viewports") {
    const auto src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    test::ScratchDir scratch("ui_pack_login_addon_bounds");
    const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern", {}, false);
        s.display_size = ImVec2(1280, 720);
        ui::LoginView login;
        login.servers_dir = dir / "servers";
        login.ui = login.default_ui = "modern";
        ServerProfile profile;
        profile.id = "alpha";
        profile.name = "Alpha";
        profile.host = "127.0.0.1";
        profile.username = "first";
        profile.accounts = {{"first", "Bob", "World One", {{"Bob", "World One", 12, "WAR"}}, false, false}};
        login.profiles = {profile};
        ui::select_profile(login, 0);
        login.status = {"Enter your password to log in.", true};
        login.addons.available = login.addons.browsed = true;
        login.addons.suggestions = {
            {"", "owner/one", "Required to play on this server: owner/one.", true, true},
            {"", "owner/two", "Required to play on this server: owner/two.", true, true},
            {"", "owner/three", "Required to play on this server: owner/three.", true, true},
            {"", "owner/extra", "Alpha suggests another add-on: owner/extra.", true}};
        s.host.login = &login;
        lua_State* L = s.lua.state();
        // Observe the real button bounds. The button only reports a click when it receives mouse input.
        REQUIRE(luaL_dostring(L, R"lua(
            ui.SetSetting('test.login', 'boxset')
            local button, lastFrame = ui.Button, -1
            ui.Button = function(label, ...)
                local clicked = button(label, ...)
                local frame = ui.GetFrameCount()
                if lastFrame ~= frame then buttonRects, lastFrame = {}, frame end
                if not buttonRects[label] then buttonRects[label] = { ui.GetItemRect() } end
                return clicked
            end
        )lua") == LUA_OK);
        auto button_rect = [&](const char* label) {
            INFO(label);
            lua_getglobal(L, "buttonRects");
            REQUIRE(lua_istable(L, -1));
            lua_getfield(L, -1, label);
            REQUIRE(lua_istable(L, -1));
            std::array<float, 4> rect{};
            for (std::size_t i = 0; i < rect.size(); ++i) {
                lua_rawgeti(L, -1, static_cast<lua_Integer>(i + 1));
                rect[i] = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            lua_pop(L, 2);
            return ImRect(rect[0], rect[1], rect[2], rect[3]);
        };
        auto click = [&](const char* label) {
            INFO(label);
            const auto rect = button_rect(label);
            REQUIRE(ImRect(0, 0, s.display_size.x, s.display_size.y).Contains(rect));
            const auto center = rect.GetCenter();
            io.AddMousePosEvent(center.x, center.y);
            s.frames(1);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            s.frames(1);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            s.frames(6); // Let a newly opened card settle at its measured height.
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        };
        auto active = [](const char* key) -> ImGuiWindow* {
            auto* window = ImGui::FindWindowByName(key);
            return window && window->Active ? window : nullptr;
        };
        auto fits = [&](const ImGuiWindow* window) {
            REQUIRE(window);
            CHECK(ImRect(0, 0, s.display_size.x, s.display_size.y).Contains(window->Rect()));
        };
        constexpr char login_key[] = "###modern.pregame.login";
        constexpr char box_key[] = "###modern.pregame.login.boxset";
        constexpr char quick_key[] = "###modern.pregame.login.quick";
        constexpr char addons_key[] = "###modern.pregame.login.addons";

        s.frames(4);
        fits(active(login_key));
        REQUIRE(active(box_key));
        REQUIRE(active(quick_key));
        click("Add-ons");
        REQUIRE(login.addons.open);
        auto* addons = active(addons_key);
        fits(addons);
        CHECK(addons->Rect().GetCenter().x == doctest::Approx(640.0f).epsilon(0.003));
        CHECK_FALSE(active(login_key));
        CHECK_FALSE(active(box_key));
        CHECK_FALSE(active(quick_key));
        CHECK(button_rect("Close").Max.y <= addons->Pos.y + 112);
        click("Close");
        CHECK_FALSE(login.addons.open);
        CHECK_FALSE(active(addons_key));
        fits(active(login_key));

        click("Review required add-on");
        CHECK(s.host.login_action == static_cast<int>(ui::LoginAction::AddonSuggestLook));
        CHECK(login.addons.choice == "owner/one");
        // Apply the client's response to AddonSuggestLook without fetching a real add-on.
        login.addons.open = true;
        auto& offer = login.addons.offer.emplace();
        offer.name = "one";
        offer.kind = "plugin";
        offer.source = "owner/one";
        offer.description = "An add-on the player needs to review before joining this server.";
        offer.caps = {"read the game state", "draw windows"};
        s.display_size = ImVec2(760, 380);
        s.frames(4);
        addons = active(addons_key);
        fits(addons);
        CHECK(addons->ScrollbarY);
        CHECK(button_rect("Close").Max.y <= addons->Pos.y + 112);
        click("Close");
        CHECK_FALSE(login.addons.open);
        auto* login_window = active(login_key);
        fits(login_window);
        CHECK(login_window->ScrollbarY);

        login.addons.offer.reset();
        s.display_size = ImVec2(380, 680);
        s.frames(4);
        fits(active(login_key));
        click("Add-ons");
        REQUIRE(login.addons.open);
        fits(active(addons_key));
        click("Close");
        CHECK_FALSE(login.addons.open);

        s.display_size = ImVec2(1900, 900);
        s.frames(4);
        click("Add-ons");
        addons = active(addons_key);
        login_window = active(login_key);
        fits(addons);
        fits(login_window);
        CHECK(login_window->Rect().Max.x < addons->Pos.x);
        CHECK(addons->Pos.y == login_window->Pos.y);
    }
    ImGui::DestroyContext(ctx);
}
