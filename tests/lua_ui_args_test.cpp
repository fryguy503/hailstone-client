// The hs API's arguments and lifetime (app/lua_ui*.cpp): Lua is built as C, so a failed argument check longjmps past
// C++ destructors; the functions check every argument before they make C++ objects. Lua numbers reach ints through a
// conversion that holds NaN and out-of-range values, and an UiApi takes its handlers off the host when it goes.
#include <doctest/doctest.h>

#include <limits>
#include <optional>
#include <string>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "plugin/lua_value.h"
#include "net/item_packets.h"
#include "mq_test_world.h"
#include "scratch_dir.h"

using namespace hailstone;

namespace {

std::string run(lua_State* L, const std::string& code) {
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_settop(L, 0);
        return "error: " + e;
    }
    std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}

} // namespace

TEST_CASE("hs API: number_to_int holds NaN, the infinities and values past int's range") {
    constexpr double inf = std::numeric_limits<double>::infinity();
    CHECK(app::number_to_int(std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(app::number_to_int(inf) == std::numeric_limits<int>::max());
    CHECK(app::number_to_int(-inf) == std::numeric_limits<int>::min());
    CHECK(app::number_to_int(1e300) == std::numeric_limits<int>::max());
    CHECK(app::number_to_int(-1e300) == std::numeric_limits<int>::min());
    CHECK(app::number_to_int(42.9) == 42);
    CHECK(app::number_to_int(-3.7) == -3);
}

TEST_CASE("hs API: bad arguments are Lua errors before anything is made; odd numbers are held") {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
    ImGui::GetIO().IniFilename = nullptr;
    {
        test::ScratchDir dir("lua_ui_args_test");
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        int preview_w = -1, preview_h = -1;
        host.item_preview_image = [&](int w, int h, int) -> void* {
            preview_w = w;
            preview_h = h;
            return nullptr;
        };
        int controls = 0;
        host.item_preview_control = [&](std::string_view, float, float, int) { ++controls; };
        app::UiApi api(host);
        api.load_layout(dir.path() / "defaults.ini", dir.path() / "UI_Test_Server.ini", {});
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        api.begin_frame();
        host.spell_displays = {{200, false}, {201, true}};
        std::vector<net::Item> displays(2);
        displays[0].id = 123;
        displays[0].name = "Example";
        displays[1].id = 456;
        host.item_displays = &displays;
        CHECK(run(L, "return hs.DisplaySpellId(2)") == "201");
        CHECK(run(L, "return hs.DisplaySpellId(3)") == "nil");
        CHECK(run(L, "return hs.DisplayItemData(1).id") == "123");
        CHECK(run(L, "return hs.DisplayItemData(2).id") == "456");
        CHECK(run(L, "return hs.DisplayItemData(0)") == "nil");
        displays[0].stack_count = 7;
        displays[0].item_type = 20;
        CHECK(run(L, "local d = hs.DisplayItemData(1) return d.stackCount .. ' ' .. d.stack_count") == "7 7");
        CHECK(run(L, "local d = hs.DisplayItemData(1) return d.itemType .. ' ' .. d.item_type") == "20 20");
        displays[0].effects[net::kEffectScroll].effect = 200;
        displays[0].effects[net::kEffectFocus].effect = -1;
        displays[1].effects[net::kEffectFocus].effect = 201;
        displays[1].effects[net::kEffectProc].effect = 202;
        displays[1].effects[net::kEffectWorn].effect = 203;
        displays[1].effects[net::kEffectClick].effect = 204;
        CHECK(run(L, "local d = hs.DisplayItemData(1) return d.scrollSpell") == "200");
        CHECK(run(L, "local d = hs.DisplayItemData(1) return d.focusSpell") == "0");
        CHECK(run(L, "local d = hs.DisplayItemData(2) "
                     "return table.concat({d.scrollSpell, d.focusSpell, d.procSpell, "
                     "d.wornSpell, d.clickSpell}, ' ')") == "0 201 202 203 204");
        auto fails = [&](const std::string& call) { return run(L, "return (pcall(" + call + "))") == "false"; };
        CHECK(fails("hs.WindowPlaced, 'Chat1', 10, 'x'"));
        CHECK(fails("hs.FindItems, 'all'"));
        CHECK(fails("hs.PressButtonFrom, 'ActionsWindow'"));
        CHECK(fails("hs.ClickHotButton, 'HotButtonWnd'"));
        CHECK(fails("hs.BuffOverlay, 'BuffWindow', 'BW_Buff0_Button'"));
        CHECK(fails("hs.SetWindowText, 'ActionsWindow', 'AMP_WhoButton'"));
        CHECK(fails("hs.WebNav, 1, 'sideways'"));
        CHECK(fails("hs.WebNav, 1"));
        // A gem with no cool-down is nil whatever y0 and y1 are (they are read only for one that has it).
        CHECK(run(L, "return hs.GemOverlay('CastSpellWnd', 'CSPW_Spell0')") == "nil");
        // NaN, the infinities and huge values are held, not undefined casts.
        CHECK(run(L, "hs.WindowPlaced('Chat1', 0/0, math.huge, -math.huge, 1e300, true); return 'ok'") == "ok");
        CHECK(run(L, "return hs.ItemPreviewImage(0/0, 1e300)") == "nil");
        CHECK(preview_w == 0);
        CHECK(preview_h == std::numeric_limits<int>::max());
        // The preview's camera takes no NaN, infinity or number past float's range.
        CHECK(run(L, "hs.ItemPreviewControl('rotate', 0.5, 0.1) return 'ok'") == "ok");
        CHECK(controls == 1);
        CHECK(run(L, "hs.ItemPreviewControl('zoom', 0/0) hs.ItemPreviewControl('pan', 0, math.huge) "
                     "hs.ItemPreviewControl('rotate', 1e300, 0) return 'ok'") == "ok");
        CHECK(controls == 1);
        CHECK(run(L, "return (pcall(hs.GemOverlay, 'CastSpellWnd', 'CSPW_Spell0', 0/0, -math.huge))") == "true");
        // A record table with a metatable is not filled (its metamethods would run mid-fill): a new one comes back.
        CHECK(run(L, "local t = setmetatable({}, {__newindex = function() error('no') end, __index = function() "
                     "error('no') end}) local r = hs.GemInfo('CastSpellWnd', 'CSPW_Spell0', t) "
                     "return tostring(r ~= t and rawget(t, 'tooltip') == nil and r.tooltip ~= nil)") == "true");
        CHECK(run(L, "local t = setmetatable({}, {__newindex = function() error('no') end}) "
                     "local r = hs.HotButtonInfo('HotButtonWnd', 'HB_Button1', t) return tostring(r ~= t)") == "true");
        CHECK(run(L, "local t = {} return tostring(hs.GemInfo('CastSpellWnd', 'CSPW_Spell0', t) == t)") == "true");
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("hs API: a right click on a spell gem notes the gem's hit box, not an earlier click's rectangle") {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int fw = 0, fh = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
    {
        test::ScratchDir dir("lua_ui_args_gem_rect");
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        app::UiApi api(host);
        api.load_layout(dir.path() / "defaults.ini", dir.path() / "UI_Test_Server.ini", {});
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        ImGui::NewFrame();
        api.begin_frame();
        ImGui::SetNextWindowPos(ImVec2(300, 200));
        ImGui::SetNextWindowSize(ImVec2(400, 200));
        ImGui::Begin("gem rect");
        ImGui::SetCursorScreenPos(ImVec2(320, 220));
        ImGui::InvisibleButton("##gem", ImVec2(40, 40));
        host.click_rect = {5, 6, 7, 8}; // an earlier ClickChild / ClickSlot's
        run(L, "return hs.SpellGemClick('CSPW_Spell0', 'right')");
        CHECK(host.click_rect[0] == doctest::Approx(320.0f));
        CHECK(host.click_rect[1] == doctest::Approx(220.0f));
        CHECK(host.click_rect[2] == doctest::Approx(360.0f));
        CHECK(host.click_rect[3] == doctest::Approx(260.0f));
        // the other phases (the press, the held button) leave it alone
        host.click_rect = {5, 6, 7, 8};
        run(L, "return hs.SpellGemClick('CSPW_Spell0', 'rightdown')");
        CHECK(host.click_rect[0] == doctest::Approx(5.0f));
        ImGui::End();
        ImGui::EndFrame();
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("hs API: its handlers on the host, and the chat's UI ini, go with it") {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    {
        test::ScratchDir dir("lua_ui_args_gone");
        app::UiHost host;
        {
            app::UiApi api(host);
            api.load_layout(dir.path() / "defaults.ini", dir.path() / "UI_Test_Server.ini", {});
            CHECK(host.show_window);
            CHECK(host.open_help);
            CHECK(host.open_social_editor);
            CHECK(app::chat_ui_ini() != nullptr);
        }
        CHECK_FALSE(host.open_social_editor);
        CHECK_FALSE(host.open_help);
        CHECK_FALSE(host.layout_sources);
        CHECK_FALSE(host.copy_layout);
        CHECK_FALSE(host.show_tip_of_day);
        CHECK_FALSE(host.activate_tip);
        CHECK_FALSE(host.show_window);
        CHECK(app::chat_ui_ini() == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("hs API: the session's menus and dialogs go when the connection does") {
    app::UiHost host;
    bool ran = false;
    host.menus.emplace_back();
    host.menus.back().rows.push_back({});
    host.menus.back().rows.back().act = [&] { ran = true; };
    host.quantity = app::UiHost::UiQuantity{};
    host.text_entry = app::UiHost::TextEntry{};
    host.confirm = app::UiHost::Confirm{};
    host.pick_color(0x123456, [&](std::uint32_t) { ran = true; });
    host.drop_session_dialogs();
    CHECK(host.menus.empty());
    CHECK_FALSE(host.quantity);
    CHECK_FALSE(host.text_entry);
    CHECK_FALSE(host.confirm);
    CHECK_FALSE(host.color_pick);
    CHECK_FALSE(ran);
}

TEST_CASE("plugin values: deep tables both ways stop at the depth limit, with room on the stack") {
    lua_State* L = luaL_newstate();
    // A UiValue nested 1000 arrays deep (a plugin's answer): only kMaxValueDepth levels become tables.
    plugin::UiValue deep = plugin::UiValue::integer(7);
    for (int i = 0; i < 1000; ++i) {
        plugin::UiValue outer;
        outer.kind = plugin::UiValue::Kind::Array;
        outer.items.push_back(std::move(deep));
        deep = std::move(outer);
    }
    plugin::push_value(L, deep);
    REQUIRE(lua_gettop(L) == 1);
    int levels = 0;
    while (lua_type(L, -1) == LUA_TTABLE) {
        ++levels;
        lua_rawgeti(L, -1, 1);
    }
    CHECK(levels == plugin::kMaxValueDepth);
    CHECK(lua_isnil(L, -1));
    lua_settop(L, 0);
    // And back: a Lua table nested deeper than the limit is cut there too.
    REQUIRE(luaL_dostring(L, "local t = 1; for i = 1, 100 do t = { k = t } end; return t") == LUA_OK);
    plugin::UiValue v = plugin::to_value(L, -1);
    levels = 0;
    for (const plugin::UiValue* p = &v; p && p->kind == plugin::UiValue::Kind::Table; p = p->field("k")) ++levels;
    CHECK(levels == plugin::kMaxValueDepth);
    lua_close(L);
}

TEST_CASE("plugin values: a table inside itself or past the size budget is refused, not expanded") {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    // Eight references to itself at every level: 8^16 values below the depth limit.
    REQUIRE(luaL_dostring(L, "local t = {} for i = 1, 8 do t[i] = t end return t") == LUA_OK);
    std::string why;
    CHECK_FALSE(plugin::to_value_checked(L, -1, &why));
    CHECK(why == "a table holds itself");
    CHECK(plugin::to_value(L, -1).kind == plugin::UiValue::Kind::Nil);
    CHECK(lua_gettop(L) == 1);
    lua_settop(L, 0);
    // The same fan-out without a cycle: eight references to the level below, sixteen levels.
    REQUIRE(luaL_dostring(L, "local t = 1 for d = 1, 15 do local n = {} for i = 1, 8 do n[i] = t end t = n end return t") ==
            LUA_OK);
    CHECK_FALSE(plugin::to_value_checked(L, -1, &why));
    CHECK(why.find("values") != std::string::npos);
    lua_settop(L, 0);
    // Too much text.
    REQUIRE(luaL_dostring(L, "local s = string.rep('x', 1 << 20) local t = {} for i = 1, 17 do t[i] = s end return t") ==
            LUA_OK);
    CHECK_FALSE(plugin::to_value_checked(L, -1, &why));
    CHECK(why.find("bytes") != std::string::npos);
    lua_settop(L, 0);
    // A table met twice, not inside itself, is fine.
    REQUIRE(luaL_dostring(L, "local x = {1, 2} return {a = x, b = x}") == LUA_OK);
    auto ok = plugin::to_value_checked(L, -1);
    REQUIRE(ok);
    CHECK(ok->field("b")->items.size() == 2);
    lua_settop(L, 0);
    // The other way: a plugin's answer past the budget is nil as a whole.
    plugin::UiValue wide;
    wide.kind = plugin::UiValue::Kind::Array;
    wide.items.assign(plugin::kMaxValueNodes, plugin::UiValue::integer(1));
    plugin::push_value(L, wide);
    CHECK(lua_isnil(L, -1));
    wide.items.resize(10);
    plugin::push_value(L, wide);
    CHECK(lua_istable(L, -1));
    lua_close(L);
}

TEST_CASE("plugin values: number keys get Lua's own text without a Lua string made for them") {
    lua_State* L = luaL_newstate();
    REQUIRE(luaL_dostring(L, "return {[1.5] = 'a', [2.0] = 'b', [-3] = 'c', [1e100] = 'd', x = 'e'}") == LUA_OK);
    const plugin::UiValue v = plugin::to_value(L, -1);
    REQUIRE(v.kind == plugin::UiValue::Kind::Table);
    // Each key as tostring(key) gives it in Lua.
    for (const char* k : {"1.5", "-3", "1e+100", "x"}) {
        INFO(k);
        CHECK(v.field(k));
    }
    // 2.0 is the integer key 2 in a Lua table.
    CHECK(v.field("2"));
    lua_close(L);
}

TEST_CASE("hs API: integer_as, held_int and held_index narrow without wrapping") {
    CHECK(app::integer_as<std::int16_t>(23LL) == std::optional<std::int16_t>(23));
    CHECK_FALSE(app::integer_as<std::int16_t>(65536LL + 23));
    CHECK_FALSE(app::integer_as<std::int16_t>(-32769LL));
    CHECK_FALSE(app::integer_as<std::uint32_t>(-1LL));
    CHECK_FALSE(app::integer_as<std::uint32_t>(4294967296LL));
    CHECK(app::held_int(4294967296LL + 3) == std::numeric_limits<int>::max());
    CHECK(app::held_int(std::numeric_limits<long long>::min()) == std::numeric_limits<int>::min());
    CHECK(app::held_int(-7LL) == -7);
    CHECK(app::held_index(1LL) == 0);
    CHECK(app::held_index(0LL) == -1);
    CHECK(app::held_index(std::numeric_limits<long long>::min()) == std::numeric_limits<int>::min());
}

// A number past a slot's, an index's or a choice's range reaches no other slot, row or choice than it names: a cast
// would wrap 65536 + 23 to slot 23 and 2^32 + 3 to choice 3.
TEST_CASE("hs API: slots, indexes and combo choices past their range are refused, not wrapped") {
    mq_test::World world("hailstone_lua_ui_args_narrow_test");
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
    ImGui::GetIO().IniFilename = nullptr;
    {
        test::ScratchDir dir("lua_ui_args_narrow");
        net::Item item;
        item.id = 1001;
        item.name = "Rusty Dagger";
        net::GameConnectionTestAccess::inventory(*world.conn).put(net::ItemSlot::possessions(23), item);
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = world.game.get();
        host.conn = world.conn.get();
        app::UiApi api(host);
        api.load_layout(dir.path() / "defaults.ini", dir.path() / "UI_Test_Server.ini", {});
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        api.begin_frame();
        CHECK(run(L, "return hs.Item(23).id") == "1001");
        CHECK(run(L, "return hs.Item(65536 + 23)") == "nil");
        CHECK(run(L, "return hs.Item(23, 65536 - 1)") == "nil");
        CHECK(run(L, "return hs.ClickSlot(65536 + 23, nil, 'rightheld')") == "false");
        CHECK(run(L, "return hs.ClickSlot(23, nil, 'rightheld')") == "true");
        CHECK(run(L, "return hs.BankItem(65536 + 1)") == "nil");
        CHECK(run(L, "return hs.CompareItem(65536 + 23)") == "nil");
        // The LFGuild window's class and time zone go into its packets as chosen: only the combo's own choices.
        CHECK(run(L, "return hs.ComboSelect('LFGuildWnd', 'PSFG_ClassCombo', 4294967296 + 3)") == "true");
        CHECK(run(L, "return select(2, hs.ComboInfo('LFGuildWnd', 'PSFG_ClassCombo'))") == "0");
        CHECK(run(L, "hs.ComboSelect('LFGuildWnd', 'PSFG_ClassCombo', 99) "
                     "return select(2, hs.ComboInfo('LFGuildWnd', 'PSFG_ClassCombo'))") == "0");
        CHECK(run(L, "hs.ComboSelect('LFGuildWnd', 'PSFG_ClassCombo', 3) "
                     "return select(2, hs.ComboInfo('LFGuildWnd', 'PSFG_ClassCombo'))") == "3");
        // Huge and odd indexes elsewhere are held, not wrapped or overflowed.
        CHECK(run(L, "hs.ListClick('LFGuildWnd', 'GSFP_SearchResultList', 4294967296, 'left', math.mininteger, "
                     "math.maxinteger) hs.PickUp('ActionsWindow', 'AMP_WhoButton', math.mininteger) "
                     "hs.CascadeClick(math.mininteger, math.maxinteger) return 'ok'") == "ok");
        CHECK(run(L, "return hs.BagSlotItem('Bag1', 4294967296)") == "nil");
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}
