// The one-call slot reads the packs draw hot bars, spell gems and buff trays with (app/lua_ui.cpp): hs.HotButtonInfo,
// hs.GemInfo and hs.BuffSlot's overlays give what hs.HotButton / HotButtonFace / HotButtonState, hs.ChildShown /
// ChildTooltip / ChildArt / GemTint / GemOverlay and hs.BuffOverlay give, and a record passed in is refilled.
#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "lua_ui_api_mock.h"

#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "core/game_dir.h"
#include "core/sha256.h"
#include "game/hotbuttons.h"
#include "game/lua_social.h"
#include "game/pet_buttons.h"
#include "game/spells.h"
#include "net/game_connection.h"
#include "net/pet_packets.h"
#include "net/spell_packets.h"
#include "scratch_dir.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"

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

// spells_us.txt lines: id, name and the fields given (the rest 0).
std::shared_ptr<game::SpellTable> spells() {
    auto line = [](const char* id, const char* name, std::vector<std::pair<int, const char*>> set) {
        std::vector<std::string> f(237, "0");
        f[0] = id;
        f[1] = name;
        for (const auto& [k, v] : set) f[static_cast<std::size_t>(k)] = v;
        std::string out;
        for (std::size_t i = 0; i < f.size(); ++i) out += (i ? "^" : "") + f[i];
        return out;
    };
    auto table = std::make_shared<game::SpellTable>();
    // 300: permanent (duration formula 50), a poison counter of 5 (SPA 36) and 3 hits (the limit overlay)
    REQUIRE(table->parse(line("200", "Minor Healing", {}) + "\n" + line("201", "Courage", {}) + "\n" +
                         line("300", "Venom", {{16, "50"}, {20, "5"}, {86, "36"}, {175, "1"}, {176, "3"}})) == 3);
    return table;
}

// Lua: each new call against the old ones, field by field; "ok" or the first difference.
constexpr const char* kCompare = R"(
local function eqv(a, b, what) if a ~= b then error(what .. ': ' .. tostring(a) .. ' ~= ' .. tostring(b), 0) end end
function checkHot(w, id, rec)
  local info = hs.HotButtonInfo(w, id, rec, 10, 50)
  if rec then eqv(info, rec, 'record') end
  local k, l, tex, u0, v0, u1, v1 = hs.HotButton(w, id)
  eqv(info.kind, k, 'kind') eqv(info.label, l, 'label') eqv(info.tex, tex, 'tex')
  eqv(info.u0, u0, 'u0') eqv(info.v0, v0, 'v0') eqv(info.u1, u1, 'u1') eqv(info.v1, v1, 'v1')
  local face = hs.HotButtonFace(w, id)
  eqv(info.social, face ~= nil, 'social')
  if face then
    for i = 1, 3 do eqv(info.color[i], face.color[i], 'color') end
    eqv(info.lua, face.lua, 'lua') eqv(info.running, face.running, 'running') eqv(info.enabled, face.enabled, 'enabled')
    eqv(info.faceCooldown, face.cooldown, 'faceCooldown') eqv(info.overlay, face.overlay, 'overlay')
    eqv(info.tooltip, face.tooltip, 'tooltip')
  else
    eqv(info.color, nil, 'color') eqv(info.tooltip, nil, 'tooltip') eqv(info.enabled, nil, 'enabled')
  end
  local st = hs.HotButtonState(w, id, 10, 50)
  eqv(info.state, st ~= nil, 'state')
  if st then
    eqv(info.checked, st.checked, 'checked') eqv(info.left, st.left, 'left') eqv(info.cooldown, st.cooldown, 'cooldown')
    eqv(info.top, st.top, 'top')
    if st.tint then
      for i = 1, 4 do eqv(info.tint[i], st.tint[i], 'tint') end
      eqv(info.iconAlpha, st.tint[4], 'iconAlpha')
    else
      eqv(info.tint, nil, 'tint') eqv(info.iconAlpha, nil, 'iconAlpha')
    end
  else
    eqv(info.checked, nil, 'checked') eqv(info.left, nil, 'left') eqv(info.cooldown, nil, 'cooldown')
    eqv(info.tint, nil, 'tint')
  end
  return 'ok'
end
function checkGem(id, y0, y1, rec)
  local W = 'CastSpellWnd'
  local info = hs.GemInfo(W, id, rec, y0, y1)
  if rec then eqv(info, rec, 'record') end
  eqv(info.shown, hs.ChildShown(W, id), 'shown') eqv(info.tooltip, hs.ChildTooltip(W, id), 'tooltip')
  local tex, u0, v0, u1, v1 = hs.ChildArt(W, id, 'spellicon')
  eqv(info.tex, tex, 'tex') eqv(info.u0, u0, 'u0') eqv(info.v0, v0, 'v0') eqv(info.u1, u1, 'u1') eqv(info.v1, v1, 'v1')
  local r, g, b, a = hs.GemTint(W, id)
  eqv(info.tintR, r, 'tintR') eqv(info.tintG, g, 'tintG') eqv(info.tintB, b, 'tintB') eqv(info.tintA, a, 'tintA')
  local top, cr, cg, cb, ca = hs.GemOverlay(W, id, y0, y1)
  eqv(info.top, top, 'top') eqv(info.overlay ~= nil, top ~= nil, 'overlay')
  eqv(info.coolR, cr, 'coolR') eqv(info.coolG, cg, 'coolG') eqv(info.coolB, cb, 'coolB') eqv(info.coolA, ca, 'coolA')
  return 'ok'
end
function checkBuff(w, id)
  local a = table.pack(hs.BuffSlot(w, id, true))
  local b = table.pack(hs.BuffSlot(w, id))
  if b[1] == nil then
    eqv(a.n, 1, 'count') eqv(a[1], nil, 'empty')
    return 'ok'
  end
  eqv(b.n, 5, 'plain count') eqv(a.n, 13, 'count')
  for i = 1, 5 do eqv(a[i], b[i], 'slot ' .. i) end
  local c = table.pack(hs.BuffOverlay(w, id, 'counter'))
  for i = 1, 4 do eqv(a[5 + i], c[i], 'counter ' .. i) end
  local l = table.pack(hs.BuffOverlay(w, id, 'limit'))
  for i = 1, 4 do eqv(a[9 + i], l[i], 'limit ' .. i) end
  return 'ok'
end
)";

} // namespace

TEST_CASE("Lua slot reads: hs.HotButtonInfo, hs.GemInfo and hs.BuffSlot's overlays give what the separate calls do") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        hailstone::test::ScratchDir scratch("lua_slot_info");
        GameDir game(scratch.path());
        net::GameConnection conn(game, {});
        conn.set_spell_table_for_test(spells());
        conn.world_for_test().player_id = 7;
        game::HotButtonStore store;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        host.hot_buttons = &store;
        host.set_hot_button = [&](int bar, int button, const game::HotButton& b) { store.current(bar, button) = b; };
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        REQUIRE(run(L, kCompare) == "nil");
        api.begin_frame(); // the pet window links itself to the connection (its toggle states) on its first frame

        // a gem memorized (gem 1), and the buffs: a permanent one with hits in slot 2, an empty slot before it
        conn.receive_zone_packet_for_test(net::encode(net::MemorizeSpell{1, 201, net::kMemMemorize, 0}));
        REQUIRE(conn.spell_state().gems[1].spell == 201);
        net::BuffPacket bp;
        bp.entity_id = 7;
        bp.buff.effect_type = 2;
        bp.buff.spell_id = 300;
        bp.buff.duration = -2;
        bp.buff.num_hits = 2;
        bp.slot = 2;
        conn.receive_zone_packet_for_test(net::encode(bp));
        REQUIRE(conn.spell_state().buffs[2].spell == 300);

        // bar 1: a social, a pet command (lower case: the pet word is matched without case), the gem's button, an
        // empty one, a menu button
        const std::string pet = conn.strings().format(game::kStrPet, {});
        store.social(0).name = "Hello";
        store.social(0).color = 5;
        store.current(0, 0) = {game::HotButtonType::Social, 0};
        game::HotButton follow{game::HotButtonType::PetCommand, 1};
        for (char ch : pet) follow.label += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        follow.label += " follow";
        store.current(0, 1) = follow;
        store.current(0, 2) = {game::HotButtonType::SpellGem, 1};
        store.current(0, 4) = {game::HotButtonType::MenuButton, 4, -1, -1, "0000000000000000", 0, "Sit"};
        conn.receive_zone_packet_for_test(net::encode(net::PetCommandState{3, 1})); // follow on
        api.begin_frame();
        REQUIRE(run(L, "return hs.HotButtonChecked('HotButtonWnd', 'HB_Button2')") == "true");

        CHECK(run(L, "return (hs.HotButtonInfo('HotButtonWnd', 'HB_Button2').checked)") == "true");
        for (int i = 1; i <= 12; ++i) {
            INFO(i);
            CHECK(run(L, "return checkHot('HotButtonWnd', 'HB_Button" + std::to_string(i) + "')") == "ok");
            CHECK(run(L, "return checkHot('HotButtonWnd5', 'HB_Button" + std::to_string(i) + "')") == "ok");
        }
        CHECK(run(L, "return checkHot('NoSuchWnd', 'HB_Button1')") == "ok");
        CHECK(run(L, "return hs.HotButtonInfo('HotButtonWnd', 'HB_Button1').social") == "true");
        CHECK(run(L, "return hs.HotButtonInfo('HotButtonWnd', 'HB_Button3').iconAlpha ~= nil") == "true");
        // one record through a social, the gem, an empty button and the social again: each read whole
        CHECK(run(L, "rec = {} return checkHot('HotButtonWnd', 'HB_Button1', rec)") == "ok");
        CHECK(run(L, "return checkHot('HotButtonWnd', 'HB_Button3', rec)") == "ok");
        CHECK(run(L, "return checkHot('HotButtonWnd', 'HB_Button4', rec)") == "ok");
        CHECK(run(L, "return checkHot('HotButtonWnd', 'HB_Button1', rec)") == "ok");
        CHECK(run(L, "return checkHot('HotButtonWnd', 'HB_Button3', rec)") == "ok");

        for (int g = 0; g < game::kSpellGems; ++g) {
            INFO(g);
            CHECK(run(L, "return checkGem('CSPW_Spell" + std::to_string(g) + "', 100, 140)") == "ok");
            CHECK(run(L, "gemrec = gemrec or {} return checkGem('CSPW_Spell" + std::to_string(g) + "', 100, 140, gemrec)") == "ok");
        }
        CHECK(run(L, "return hs.GemInfo('CastSpellWnd', 'CSPW_Spell1').tooltip") == "Courage");
        CHECK(run(L, "return hs.GemInfo('CastSpellWnd', 'CSPW_Spell11').shown") == "false");
        CHECK(run(L, "return hs.GemInfo('CastSpellWnd', 'CSPW_Spell1').top") == "nil"); // no span given

        for (int i = 0; i < game::kLongBuffSlots; ++i) {
            INFO(i);
            CHECK(run(L, "return checkBuff('BuffWindow', 'Buff" + std::to_string(i) + "')") == "ok");
        }
        for (const char* w : {"ShortDurationBuffWindow", "TargetWindow", "NoSuchWnd"})
            CHECK(run(L, std::string("return checkBuff('") + w + "', 'Buff0')") == "ok");
        CHECK(run(L, "return checkBuff('PetInfoWindow', 'PetBuff0')") == "ok");
        CHECK(run(L, "return (select(10, hs.BuffSlot('BuffWindow', 'Buff2', true)))") == "2"); // the hits left
        CHECK(run(L, "return (hs.BuffOverlay('BuffWindow', 'Buff2', 'nonsense'))") == "nil");
        // a missing child is a Lua error before anything is made; NaN and huge spans are held
        CHECK(run(L, "return (pcall(hs.HotButtonInfo, 'HotButtonWnd'))") == "false");
        CHECK(run(L, "return (pcall(hs.GemInfo, 'CastSpellWnd'))") == "false");
        CHECK(run(L, "return (pcall(hs.BuffSlot, 'BuffWindow'))") == "false");
        CHECK(run(L, "return (pcall(hs.HotButtonInfo, 'HotButtonWnd', 'HB_Button3', nil, 0/0, 1e300))") == "true");
        CHECK(run(L, "return (pcall(hs.GemInfo, 'CastSpellWnd', 'CSPW_Spell1', nil, -math.huge, 0/0))") == "true");
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Lua slot reads: both packs' hot bars, spell gems and buff trays draw over them without script errors") {
    namespace fs = std::filesystem;
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        hailstone::test::ScratchDir scratch("lua_slot_info_packs");
        const fs::path dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        {
            GameDir game(dir / "game");
            net::GameConnection conn(game, {});
            conn.set_spell_table_for_test(spells());
            conn.world_for_test().player_id = 7;
            ui::ChatView view;
            game::HotButtonStore store;
            app::UiHost host;
            host.game = &game;
            host.conn = &conn;
            host.chat = &view;
            host.hot_buttons = &store;
            host.in_game = host.ui_ready = true;
            app::UiApi api(host);
            ui::lua::LuaUi lua(dir / pack, dir / "ui.txt");
            lua.add_ui_api([&api](lua_State* L) {
                api.register_functions(L);
                lua_pushcfunction(L, [](lua_State* S) { lua_pushboolean(S, 1); return 1; });
                lua_setfield(L, -2, "InGame");
                hailstone::test::sync_ui_api_aliases(L);
            });
            api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
            auto frames = [&](int n) {
                for (int i = 0; i < n; ++i) {
                    ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
                    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
                    api.begin_frame();
                    ImGui::NewFrame();
                    lua.frame();
                    ImGui::Render();
                }
            };
            frames(1);
            conn.receive_zone_packet_for_test(net::encode(net::MemorizeSpell{1, 201, net::kMemMemorize, 0}));
            net::BuffPacket bp;
            bp.entity_id = 7;
            bp.buff.effect_type = 2;
            bp.buff.spell_id = 300;
            bp.buff.duration = -2;
            bp.buff.num_hits = 2;
            for (std::uint32_t slot : {2u, 5u, 43u}) { // two buffs (a gap between) and a song
                bp.slot = slot;
                conn.receive_zone_packet_for_test(net::encode(bp));
            }
            bp.buff.spell_id = 200; // and one running out: its time left shows
            bp.buff.duration = 3;
            bp.slot = 7;
            conn.receive_zone_packet_for_test(net::encode(bp));
            store.social(0).name = "A long social name";
            store.social(0).color = 5;
            for (int bar : {0, 1}) {
                store.current(bar, 0) = {game::HotButtonType::Social, 0};
                store.current(bar, 0).label = store.social(0).name;
                store.current(bar, 2) = {game::HotButtonType::SpellGem, 1};
                store.current(bar, 4) = {game::HotButtonType::MenuButton, 4, -1, -1, "0000000000000000", 0, "Sit"};
            }
            lua_State* L = lua.state();
            for (const char* screen : {"HotButtonWnd", "HotButtonWnd2", "CastSpellWnd", "BuffWindow",
                                       "ShortDurationBuffWindow", "TargetWindow", "PetInfoWindow"}) {
                std::string show = std::string("hs.ShowWindow('") + screen + "', true)";
                REQUIRE(luaL_dostring(L, show.c_str()) == LUA_OK);
            }
            frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            // the social renamed, the gem forgotten: the records kept from frame to frame are read again
            store.social(0).name = "Hi";
            conn.receive_zone_packet_for_test(net::encode(net::MemorizeSpell{1, 201, net::kMemForget, 0}));
            frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// hs.SetLuaSocial reads every field before it builds anything: a table whose __index raises changes nothing (and
// leaves no C++ string behind), and one whose __index gives the fields is read as before.
TEST_CASE("Lua socials: hs.SetLuaSocial reads a table's fields, metamethods included, before building the social") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        hailstone::test::ScratchDir scratch("lua_set_social");
        GameDir game(scratch.path());
        net::GameConnection conn(game, {});
        game::HotButtonStore store;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        host.hot_buttons = &store;
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        CHECK(run(L, R"lua(
local ok, err = pcall(hs.SetLuaSocial, 0, setmetatable({name = 'a name past the short string buffer'},
                                                       {__index = function() error('boom') end}))
return tostring(ok) .. ' ' .. tostring(err):match('boom')
)lua") == "false boom");
        CHECK(store.social(0).name.empty());
        CHECK(run(L, R"lua(
return tostring(hs.SetLuaSocial(0, setmetatable({name = 'Proxy'}, {__index = function(t, k)
  if k == 'press' then return "mq.cmd('/sit')" end
  if k == 'color' then return 4 end
end})))
)lua") == "true");
        CHECK(store.social(0).name == "Proxy");
        CHECK(store.social(0).color == 4);
        CHECK(store.social(0).lua.press == "mq.cmd('/sit')");
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

// hs.LuaSocial's hash is kept per social while its code stays the same, and follows any change to the code, whoever
// makes it (hs.SetLuaSocial, or the store written directly as an import or the file does).
TEST_CASE("Lua socials: hs.LuaSocial's hash follows the code") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        hailstone::test::ScratchDir scratch("lua_social_hash_cache");
        GameDir game(scratch.path());
        net::GameConnection conn(game, {});
        game::HotButtonStore store;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        host.hot_buttons = &store;
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        auto hash_of = [](const game::LuaSocial& l) { return to_hex(game::lua_social_hash(l)); };
        CHECK(run(L, "return hs.LuaSocial(3)") == "nil");
        CHECK(run(L, "return tostring(hs.SetLuaSocial(3, {name = 'Sit', press = \"mq.cmd('/sit')\"}))") == "true");
        const std::string sit = hash_of(game::LuaSocial{"mq.cmd('/sit')", ""});
        CHECK(run(L, "return hs.LuaSocial(3).hash") == sit);
        CHECK(run(L, "return hs.LuaSocial(3).hash") == sit);
        CHECK(run(L, "hs.SetLuaSocial(3, {name = 'Sit', press = \"mq.cmd('/sit')\", face = 'return {}'}) "
                     "return hs.LuaSocial(3).hash") == hash_of(game::LuaSocial{"mq.cmd('/sit')", "return {}"}));
        store.social(3).lua = game::LuaSocial{"mq.cmd('/stand')", ""};
        CHECK(run(L, "return hs.LuaSocial(3).hash") == hash_of(store.social(3).lua));
        store.social(3).lua = game::LuaSocial{"mq.cmd('/sit')", ""};
        CHECK(run(L, "return hs.LuaSocial(3).hash") == sit);
        // Each social has its own.
        store.social(4) = store.social(3);
        store.social(4).lua.press = "mq.cmd('/stand')";
        CHECK(run(L, "return hs.LuaSocial(4).hash") == hash_of(store.social(4).lua));
        CHECK(run(L, "return hs.LuaSocial(3).hash") == sit);
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}
