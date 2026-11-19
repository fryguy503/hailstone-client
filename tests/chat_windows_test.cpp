// The chat windows over the hs API (app/lua_ui_chat.cpp): new windows, per-window channels, default channel,
// clear, close, and the settings kept in the character's UI ini.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>

#include <imgui.h>
#include <lua.hpp>

#include "lua_ui_api_mock.h"

#include "app/chat_context_menu.h"
#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "core/ini_file.h"
#include "game/chat_windows.h"
#include "game/combat.h"
#include "game/item_links.h"
#include "game/server_ext.h"
#include "game/social.h"
#include "ui/ui_ini.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "platform/local_time.h"
#include "ui/dev_screens.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct Lua {
    lua_State* L = luaL_newstate();
    explicit Lua(app::UiApi& api) {
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
    }
    ~Lua() { lua_close(L); }
    // Runs `code` (which returns one value) and gives it as text.
    std::string run(const std::string& code) {
        if (luaL_dostring(L, code.c_str()) != LUA_OK) {
            std::string e = lua_tostring(L, -1);
            lua_pop(L, 1);
            return "error: " + e;
        }
        std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
        lua_settop(L, 0);
        return out;
    }
};

// The texts of a window's lines, joined with '|' (the STML tags stripped).
const char* kLines = R"(
local function lines(slot)
  local _, l = hs.ChatText(slot)
  local out = {}
  for _, s in ipairs(l or {}) do out[#out + 1] = (s:gsub("<[^>]*>", "")) end
  return table.concat(out, "|")
end
)";

} // namespace

// The eqstr texts the chat window manager's menus and answers use (as in eqstr_us.txt).
void write_strings(const fs::path& game) {
    std::ofstream(game / "eqstr_us.txt") << "EQST0002\n"
                                          << "6623 Main Chat\n6624 Chat\n6630 Say\n6631 Tell\n6632 Group\n"
                                          << "6633 Guild\n6634 OOC\n6635 Auction\n6636 Shout\n6637 Emotes\n"
                                          << "5113 Raid\n6638 Chat Channel\n6639 Melee\n6640 Spells\n6645 Filters\n"
                                          << "3135 will go to this window.\n3136 will go to the main window.\n"
                                          << "3138  is now the default channel for this window.\n"
                                          << "6041 All melee channels will now filter to this window.\n"
                                          << "15014 Your Hits\n15015 Your Misses\n15016 You Being Hit\n"
                                          << "15017 You Being Missed\n15018 Others' Hits\n15019 Others' Misses\n"
                                          << "15026 My Death\n15034 Normal\n15035 Abbreviated\n"
                                          << "15036 Number Only\n15037 Miss Only\n15038 Hit Modes\n"
                                          << "13205 ALL\n6300 Bad Word\n6306 Guild Chat\n6301 PC Spells\n";
}

TEST_CASE("Chat windows: lines go to the window of their colour's channel (ChannelMap), kept in the UI ini") {
    ImGuiContext* ctx = ImGui::CreateContext();
    hailstone::test::ScratchDir dir_scratch("chat_windows_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    write_strings(dir / "game");
    GameDir game(dir / "game");
    std::vector<std::string> submitted;
    {
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        host.submit_chat = [&](const std::string& t) { submitted.push_back(t); };
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        Lua lua(api);
        lua.run(kLines + std::string("_G.lines = lines"));

        view.add(ui::ChatChannel::Say, "hello");
        view.add(ui::ChatChannel::Tell, "psst");
        view.add(ui::ChatChannel::System, "You have been slain", 0, 277); // My Death (channel 28)
        CHECK(lua.run("return #hs.ChatWindows()") == "1");
        CHECK(lua.run("return hs.ChatWindows()[1].title") == "Main Chat");
        CHECK(lua.run("return lines(1)") == "hello|psst|You have been slain"); // every channel starts in the main one

        // A new window is a stock window that shows (hs.ReplaceWindow), empty, with no channels.
        CHECK(lua.run("return tostring(hs.ReplaceWindow('Chat2'))") == "false");
        CHECK(lua.run("return hs.ChatNewWindow()") == "2");
        CHECK(lua.run("return hs.ChatWindows()[2].title") == "Chat 1");
        CHECK(lua.run("return tostring(hs.ReplaceWindow('Chat2'))") == "true");
        CHECK(lua.run("return lines(2)") == "");
        // Melee's ALL: its 18 channels come here, and the window says so; lines already shown stay where they are.
        lua.run("hs.ChatSetFilter(2, 'Melee', true)");
        CHECK(lua.run("return lines(2)") == "All melee channels will now filter to this window.");
        CHECK(lua.run("local n = 0; for _, f in ipairs(hs.ChatFilters(2)) do if f.on then n = n + 1 end end; return n") == "18");
        view.add(ui::ChatChannel::System, "You have been slain", 0, 277);
        view.add(ui::ChatChannel::Combat, "You hit a rat for 3 points of damage.", 0, 265); // Your Hits (9)
        CHECK(lua.run("return lines(2)") ==
              "All melee channels will now filter to this window.|You have been slain|You hit a rat for 3 points of damage.");
        CHECK(lua.run("return lines(1)") == "hello|psst|You have been slain");
        // One channel back to the main window (its item a second time).
        lua.run("hs.ChatSetFilter(2, 28, false)");
        CHECK(lua.run("local _, l = hs.ChatText(2); return #l") == "4"); // "My Death will go to the main window."
        view.add(ui::ChatChannel::System, "You died.", 0, 277);
        CHECK(lua.run("return lines(1)") == "hello|psst|You have been slain|You died.");

        // A tab in the main window: a chat window of its own (Say comes to it).
        CHECK(lua.run("return hs.ChatNewTab(1, 'Social')") == "2");
        lua.run("hs.ChatSetFilter(1, 0, true)");
        CHECK(lua.run("local w = hs.ChatWindows()[1]; return w.tab .. #w.tabs .. w.title") == "22Social");
        view.add(ui::ChatChannel::Say, "news");
        CHECK(lua.run("return lines(1)") == "Say will go to this window.|news");
        view.add(ui::ChatChannel::Tell, "psst again");
        CHECK(lua.run("return tostring(hs.ChatWindows()[1].tabs[1].unread)") == "true"); // the tab not up
        lua.run("hs.ChatSelectTab(1, 1)");
        CHECK(lua.run("return lines(1)") == "hello|psst|You have been slain|You died.|psst again");

        // Plain text goes to the window's channel (the Channel menu); commands as typed.
        lua.run("hs.ChatSetDefault(2, 2)"); // Group
        lua.run("hs.ChatSubmit(2, 'inc')");
        lua.run("hs.ChatSubmit(2, '/who')");
        lua.run("hs.ChatSubmit(1, 'hi')");
        CHECK(submitted == std::vector<std::string>{"/gsay inc", "/who", "hi"});
        CHECK(lua.run("local _, active, d = hs.ChatInfo(1); return tostring(active) .. d") == "trueSay");
        lua.run("hs.ChatSetDefault(2, 1)"); // Tell: disabled in the menu
        CHECK(lua.run("local _, _, d = hs.ChatInfo(2); return d") == "Group");

        // Clear empties one window only.
        lua.run("hs.ChatClear(2)");
        CHECK(lua.run("return lines(2)") == "");
        CHECK(lua.run("return lines(1)") != "");

        // Tell windows: a tell goes to its player's tell window, typed text back to that player.
        lua.run("hs.ChatSetTellWindows(true)");
        view.add(ui::ChatChannel::Tell, "Bob tells you, 'hi'", 0, 257, "Bob");
        CHECK(lua.run("local w = hs.ChatWindows(); return #w .. tostring(w[3].tells) .. w[3].title") == "3trueBob");
        CHECK(lua.run("return lines(hs.ChatWindows()[3].slot)") == "Bob tells you, 'hi'");
        CHECK(lua.run("return hs.ChatWindows()[3].tellsIn") == "1"); // (the modern tell window flashes)
        view.add(ui::ChatChannel::Tell, "You told Ann, 'yo'", 0, 308, "Ann");
        CHECK(lua.run("return #hs.ChatWindows()[3].tabs") == "2"); // Ann's is a tab of Bob's
        CHECK(lua.run("return hs.ChatWindows()[3].tellsIn") == "1"); // our own tell's echo is not one
        view.add(ui::ChatChannel::Tell, "Ann tells you, 'ok'", 0, 257, "Ann");
        CHECK(lua.run("return hs.ChatWindows()[3].tellsIn") == "2"); // Ann's tab counts for the window
        // Timestamps: "[hh:mm:ss] " before each line, another version than without.
        CHECK(lua.run("local s = hs.ChatWindows()[3].slot; local _, l = hs.ChatText(s, nil, false, true) "
                      "return (l[1]:gsub('<[^>]*>', '')):match('^%[%d%d:%d%d:%d%d%] Bob tells you') ~= nil") == "true");
        CHECK(lua.run("local s = hs.ChatWindows()[3].slot; return hs.ChatText(s) ~= hs.ChatText(s, nil, false, true)") ==
              "true");
        // A stamped window that holds the current version gets only the version back, as an unstamped one does.
        CHECK(lua.run("local s = hs.ChatWindows()[3].slot; local v = hs.ChatText(s, nil, false, true) "
                      "local v2, l = hs.ChatText(s, v, false, true) return tostring(v2 == v) .. tostring(l)") ==
              "truenil");
        // The stamp is the line's local time, worked out once a minute: seconds within a minute, the next minute.
        for (std::int64_t t : {std::int64_t{1700000040}, std::int64_t{1700000041}, std::int64_t{1700000099},
                               std::int64_t{1700000100}, std::int64_t{1700003640}}) {
            for (auto& l : view.lines)
                if (l.peer == "Bob") l.time = t;
            auto tm = platform::local_time(static_cast<std::time_t>(t));
            REQUIRE(tm);
            CHECK(lua.run("local s = hs.ChatWindows()[3].slot; local _, l = hs.ChatText(s, nil, false, true) "
                          "return (l[1]:gsub('<[^>]*>', '')):sub(1, 10)") ==
                  std::format("[{:02d}:{:02d}:{:02d}]", tm->tm_hour, tm->tm_min, tm->tm_sec));
        }
        CHECK(lua.run("return hs.ChatWindows()[3].title") == "Bob");
        lua.run("local s = hs.ChatWindows()[3].slot; hs.ChatSubmit(s, 'back')");
        CHECK(submitted.back() == "/tell Bob back");

        // Hit modes: 0..2, ALL sets them all.
        lua.run("hs.ChatSetHitMode(1, 1)");
        CHECK(conn.hit_modes()[0] == 1);
        lua.run("hs.ChatSetHitMode(7, 2)");
        CHECK(conn.hit_modes()[5] == 2);

        // The Options window's chat page lists the filter table's 33 rows (the original's order and names).
        auto filters = api.list_texts("OptionsWindow", "FilterList");
        REQUIRE(filters.size() == 33);
        CHECK(filters[0] == "Bad Word");
        CHECK(filters[1] == "Guild Chat");
        CHECK(filters[7] == "PC Spells");

        lua.run("hs.ChatSetFont(2, 5); hs.ChatSetTitle(2, 'Fights')");
        api.save_layout();
        // the original's keys
        ui::UiIni* ini = app::chat_ui_ini();
        REQUIRE(ini);
        CHECK(ini->get("ChatManager", "NumWindows") == "2");
        CHECK(ini->get("ChatManager", "ChatWindow1_Name") == "Fights");
        CHECK(ini->get("ChatManager", "ChatWindow1_DefaultChannel") == "2");
        CHECK(ini->get("ChatManager", "ChatWindow1_FontStyle") == "5");
        CHECK(ini->get("ChatManager", "ChannelMap9") == "1");
        CHECK(ini->get("ChatManager", "ChannelMap28") == "0");
        CHECK(ini->get("ChatManager", "ChannelMap0") == "2");
        CHECK(ini->get("ChatManager", "HitMode0") == "2");
        // /channel on a chat server channel (Cmd_channel: ChatChannel 0x14, its name at +0x234 and its index at
        // +0x230) is kept too: the exe's "%s_ChatChannel" / "%s_TellTarget" / "%s_DefaultChannel" keys
        lua.run("hs.ChatSetAlwaysHere(1, false)"); // (the main window's lock first: the last-clicked window)
        lua.run("hs.ChatActivate(2)");
        REQUIRE(app::set_active_chat_default(0x14, "nmsguild", 1));
        api.save_layout();
        CHECK(ini->get("ChatManager", "ChatWindow1_DefaultChannel") == "20");
        CHECK(ini->get("ChatManager", "ChatWindow1_ChatChannel") == "1");
        CHECK(ini->get("ChatManager", "ChatWindow1_TellTarget") == "nmsguild");
    }
    {
        // The next session reads the windows back (not the tell windows).
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        Lua lua(api);
        lua.run(kLines + std::string("_G.lines = lines"));
        CHECK(lua.run("local w = hs.ChatWindows(); return #w .. w[2].title .. #w[1].tabs") == "2Fights2");
        CHECK(lua.run("return (hs.ChatInfo(2))") == "5");
        CHECK(lua.run("return (select(3, hs.ChatInfo(2)))") == "nmsguild"); // the chat channel default, read back
        CHECK(lua.run("return tostring(hs.ChatTellWindows())") == "true");
        CHECK(conn.hit_modes()[0] == 2);
        view.add(ui::ChatChannel::System, "a rat hits YOU for 3 points of damage.", 0, 266); // You Being Hit (24)
        view.add(ui::ChatChannel::Say, "chatter");
        view.add(ui::ChatChannel::OOC, "ooc");
        CHECK(lua.run("return lines(2)") == "a rat hits YOU for 3 points of damage.");
        lua.run("hs.ChatSelectTab(1, 2)");
        CHECK(lua.run("return lines(1)") == "chatter");
        lua.run("hs.ChatSelectTab(1, 1)");
        CHECK(lua.run("return lines(1)") == "ooc");
        // Closing a window sends its channels back to the main window; the main window stays.
        lua.run("hs.ChatCloseWindow(2)");
        CHECK(lua.run("return #hs.ChatWindows()") == "1");
        view.add(ui::ChatChannel::System, "a rat hits YOU for 4 points of damage.", 0, 266);
        CHECK(lua.run("return lines(1)") == "ooc|a rat hits YOU for 4 points of damage.");
        lua.run("hs.ChatCloseTab(1, 2); hs.ChatCloseWindow(1)");
        CHECK(lua.run("return #hs.ChatWindows()[1].tabs") == "1");
    }
    ImGui::DestroyContext(ctx);
    fs::remove_all(dir);
}

// Hailstone: a character with no chat windows of its own takes defaults.ini's as tabs of the main window (the game's
// defaults.ini makes "Other Chat", ChatWindow1, a window of its own). After that its own are read alone: through
// defaults.ini, a window the player closed came back at the next start.
TEST_CASE("Chat windows: defaults.ini's extra windows are tabs of the main window; a closed one stays closed") {
    ImGuiContext* ctx = ImGui::CreateContext();
    hailstone::test::ScratchDir dir_scratch("chat_windows_defaults_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    write_strings(dir / "game");
    std::ofstream(dir / "defaults.ini") << "[ChatManager]\nNumWindows=2\nChatWindow0_LanguageId=0\n"
                                        << "ChatWindow0_Name=Main Chat\nChatWindow1_LanguageId=0\n"
                                        << "ChatWindow1_ChatChannel=-1\nChatWindow1_Name=Other Chat\nChannelMap10=1\n";
    GameDir game(dir / "game");
    fs::path layout = dir / "UI_Test_Server.ini";
    {
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        Lua lua(api);
        const std::string tabs = "local t = {} for _, w in ipairs(hs.UI.chat.Windows()) do local n = {} "
                                 "for _, x in ipairs(w.tabs) do n[#n + 1] = x.title end "
                                 "t[#t + 1] = table.concat(n, '+') end return table.concat(t, '|')";
        // A new character: one window, Other Chat a tab of it; again so at the next start.
        api.load_layout(dir / "defaults.ini", layout, {});
        CHECK(lua.run(tabs) == "Main Chat+Other Chat");
        api.save_layout();
        api.load_layout(dir / "defaults.ini", layout, {});
        CHECK(lua.run(tabs) == "Main Chat+Other Chat");
        // The tab closed: it stays closed.
        lua.run("hs.UI.chat.CloseTab(1, 2)");
        CHECK(lua.run(tabs) == "Main Chat");
        api.save_layout();
        api.load_layout(dir / "defaults.ini", layout, {});
        CHECK(lua.run(tabs) == "Main Chat");
        // A window of the player's own is kept, and closed stays closed.
        CHECK(lua.run("return hs.UI.chat.NewWindow()") == "2");
        api.save_layout();
        api.load_layout(dir / "defaults.ini", layout, {});
        CHECK(lua.run("return #hs.UI.chat.Windows()") == "2");
        lua.run("hs.UI.chat.CloseWindow(2)");
        api.save_layout();
        api.load_layout(dir / "defaults.ini", layout, {});
        CHECK(lua.run(tabs) == "Main Chat");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Chat hit modes: spell categories persist independently, ALL keeps index 7 and menus put it last") {
    ImGuiContext* ctx = ImGui::CreateContext();
    test::ScratchDir scratch("chat_spell_hit_modes");
    const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    write_strings(dir / "game");
    GameDir game(dir / "game");
    const auto layout = dir / "UI_Test_Server.ini";
    std::ofstream(layout) << "[ChatManager]\nHitMode0=1\nHitMode6=2\nHitMode7=1\n";
    const game::SpellHitModes expected{1, 2, 0, 1};
    for (int session = 0; session < 2; ++session) {
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", layout, {});
        Lua lua(api);
        REQUIRE(lua.run("return #hs.ChatHitModes()") == "11");
        CHECK(lua.run("return hs.ChatHitModes()[7].id .. ':' .. hs.ChatHitModes()[7].name") == "7:ALL");
        CHECK(lua.run("return hs.ChatHitModes()[8].name") == "Direct Spell Damage");
        CHECK(lua.run("return hs.ChatHitModes()[9].name") == "Damage Over Time");
        CHECK(lua.run("return hs.ChatHitModes()[10].name") == "Healing");
        CHECK(lua.run("return hs.ChatHitModes()[11].name") == "Healing Over Time");
        CHECK(lua.run("return table.concat(hs.ChatHitModes()[8].modes, '|')") == "Normal|Abbreviated|Number Only");
        CHECK(lua.run("return hs.ChatHitModes()[2].modes[3]") == "Miss Only");
        if (session == 0) {
            // Older layouts' unused native modes 6/7 are not spell settings.
            CHECK(conn.hit_modes()[0] == 1);
            CHECK(conn.spell_hit_modes() == game::SpellHitModes{});
            for (std::size_t i = 0; i < expected.size(); ++i)
                lua.run(std::format("hs.ChatSetHitMode({}, {})", i + 8, expected[i]));
            CHECK(conn.hit_modes()[0] == 1);
            CHECK(conn.spell_hit_modes() == expected);
            CHECK(app::chat_ui_ini()->get("ChatManager", "HitMode6") == "2");
            CHECK(app::chat_ui_ini()->get("ChatManager", "HitMode7") == "1");

            // Stock Modern's native menu and hs.ChatMenu use the same rows.
            lua.run("hs.ChatMenu(1)");
            REQUIRE(host.menus.size() == 1);
            const auto native = app::chat_context_rows(1);
            const auto& bound = host.menus[0].rows;
            CHECK(std::equal(native.begin(), native.end(), bound.begin(), bound.end(), [](const auto& a, const auto& b) {
                return a.text == b.text && a.enabled == b.enabled && a.check == b.check && a.separator == b.separator;
            }));
            auto hit = std::find_if(native.begin(), native.end(),
                                    [](const auto& row) { return row.text == "Hit Modes"; });
            REQUIRE(hit != native.end());
            REQUIRE(hit->submenu);
            auto rows = hit->submenu();
            REQUIRE(rows.size() == 11);
            CHECK(rows.back().text == "ALL");
            for (std::size_t i = 0; i < expected.size(); ++i) {
                CAPTURE(i);
                CHECK(rows[i + 6].text == game::spell_hit_category_name(static_cast<int>(i)));
                REQUIRE(rows[i + 6].submenu);
                auto choices = rows[i + 6].submenu();
                REQUIRE(choices.size() == 3);
                CHECK(choices[static_cast<std::size_t>(expected[i])].check);
                choices[2].act();
                CHECK(conn.spell_hit_modes()[i] == 2);
            }

            // Existing scripts' ALL index also sets every spell category.
            lua.run("hs.ChatSetHitMode(7, 2)");
            CHECK(conn.hit_modes() == game::HitModes{2, 2, 2, 2, 2, 2});
            CHECK(conn.spell_hit_modes() == game::SpellHitModes{2, 2, 2, 2});
            CHECK(lua.run("return hs.ChatHitModes()[7].mode") == "2");
            for (std::size_t i = 0; i < expected.size(); ++i)
                lua.run(std::format("hs.ChatSetHitMode({}, {})", i + 8, expected[i]));
            CHECK(lua.run("return hs.ChatHitModes()[7].mode") == "-1");
            lua.run("hs.ChatSetHitMode(0, 0); hs.ChatSetHitMode(-1, 0); hs.ChatSetHitMode(12, 0); "
                    "hs.ChatSetHitMode(math.mininteger, 0); hs.ChatSetHitMode(math.maxinteger, 0); "
                    "hs.ChatSetHitMode(8, -1); hs.ChatSetHitMode(11, 3)");
            CHECK(conn.spell_hit_modes() == expected);
            CHECK(conn.hit_modes() == game::HitModes{2, 2, 2, 2, 2, 2});
            api.save_layout();
            for (std::size_t i = 0; i < expected.size(); ++i)
                CHECK(app::chat_ui_ini()->get("ChatManager", std::format("SpellHitMode{}", i)) ==
                      std::to_string(expected[i]));
        } else {
            CHECK(conn.spell_hit_modes() == expected);
            for (std::size_t i = 0; i < expected.size(); ++i)
                CHECK(lua.run(std::format("return hs.ChatHitModes()[{}].mode", i + 8)) == std::to_string(expected[i]));
            // Changing character/layout resets absent or invalid spell modes to Normal.
            const auto other = dir / "UI_Other_Server.ini";
            std::ofstream(other) << "[ChatManager]\nSpellHitMode0=-1\nSpellHitMode1=3\nSpellHitMode2=bad\n";
            api.load_layout(dir / "defaults.ini", other, {});
            CHECK(conn.spell_hit_modes() == game::SpellHitModes{});
            CHECK(conn.hit_modes() == game::HitModes{});
        }
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Chat hit mode UI: gamepad controls use stable spell indices and put ALL last") {
    const auto root = fs::path(__FILE__).parent_path().parent_path();
    const char* fixture = R"lua(
calls = {}
local modes = {}
for i = 1, 11 do
  modes[i] = {id=i, name='Category ' .. i, mode=0, modes={'Normal', 'Abbreviated', 'Number Only'}}
end
modes[7].name, modes[7].mode = 'ALL', -1
modes[11].name, modes[11].mode = 'Healing Over Time', 1
hs = {
  ChatHitModes=function() return modes end,
  ChatSetHitMode=function(id, mode) calls[#calls+1] = {id, mode} end,
  Option=function() return nil end,
  OptionChoices=function() return {} end,
  ComboInfo=function() return nil end,
}
)lua";
    std::ifstream input(root / "ui/gamepad/options.lua");
    REQUIRE(input.good());
    const std::string source(std::istreambuf_iterator<char>{input}, {});
    lua_State* state = luaL_newstate();
    REQUIRE(state);
    struct CloseLua { lua_State* state; ~CloseLua() { lua_close(state); } } close{state};
    luaL_openlibs(state);
    REQUIRE(luaL_dostring(state, fixture) == LUA_OK);
    REQUIRE(hailstone::test::attach_mock_ui_api(state) == LUA_OK);
    const char* setup = R"lua(
ui = {window=function() end, GetSetting=function() end}
local theme = {C={}, palettes={list={{id='test', name='Test'}}}, palette='test'}
dofile = function(name) return name == 'lib/theme.lua' and theme or {} end
)lua";
    const char* checks = R"lua(
local rows = PAGES[4][2]()
assert(rows[#rows][1] == 'Hit mode: ALL')
local hot
for _, row in ipairs(rows) do if row[1] == 'Hit mode: Healing Over Time' then hot = row end end
assert(hot and hot[2] == 'Abbreviated')
hot[3](1)
rows[#rows][3](-1)
assert(#calls == 2 and calls[1][1] == 11 and calls[1][2] == 2)
assert(calls[2][1] == 7 and calls[2][2] == 2)
)lua";
    const auto code = std::string("local API = hs.UI\n") + setup + source + checks;
    const int result = luaL_dostring(state, code.c_str());
    INFO((result == LUA_OK ? "" : lua_tostring(state, -1)));
    CHECK(result == LUA_OK);
}

TEST_CASE("Chat windows: each window keeps its own last 1000 lines; a busy one does not push out a quiet one's") {
    ImGuiContext* ctx = ImGui::CreateContext();
    hailstone::test::ScratchDir dir_scratch("chat_window_lines_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    write_strings(dir / "game");
    GameDir game(dir / "game");
    {
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        Lua lua(api);
        lua.run(kLines + std::string("_G.lines = lines"));
        const char* count2 = "local _, l = hs.ChatText(2) return #l";
        const char* first2 = "local _, l = hs.ChatText(2) return (l[1]:gsub('<[^>]*>', ''))";

        view.add(ui::ChatChannel::Tell, "psst");
        view.add(ui::ChatChannel::Say, "hello");
        CHECK(lua.run("return hs.ChatNewWindow()") == "2");
        lua.run("hs.ChatSetFilter(2, 'Melee', true)"); // (and its line "All melee channels ...")
        for (int i = 1; i <= 3500; ++i) { // (more than the whole buffer once held)
            view.add(ui::ChatChannel::Combat, std::format("hit {}", i), 0, 265); // Your Hits: window 2
            if (i % 100 == 0) lua.run("return #hs.ChatWindows()"); // routed (and trimmed) as the scripts look
        }
        CHECK(lua.run(count2) == "1000");
        CHECK(lua.run(first2) == "hit 2501");
        CHECK(lua.run("return lines(1)") == "psst|hello"); // the main window's lines all stay
        CHECK(view.lines.size() == 1002);

        // Clearing still goes by each line's number, with lines gone from the middle of the buffer.
        lua.run("hs.ChatClear(1)");
        CHECK(lua.run("return lines(1)") == "");
        view.add(ui::ChatChannel::Say, "again");
        CHECK(lua.run("return lines(1)") == "again");
        CHECK(lua.run(count2) == "1000");
        view.add(ui::ChatChannel::Combat, "hit 3501", 0, 265);
        CHECK(lua.run(first2) == "hit 2502");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Chat windows: hs.ChatText's version is the window's own; the incremental form sends only the new lines") {
    ImGuiContext* ctx = ImGui::CreateContext();
    hailstone::test::ScratchDir dir_scratch("chat_text_since_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    write_strings(dir / "game");
    GameDir game(dir / "game");
    {
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        Lua lua(api);
        // A script's copy of a window's lines, kept up with the incremental form as the modern chat keeps it.
        lua.run(R"(
_G.held = {}
function _G.follow(slot)
  local h = held[slot] or { version = -1, lines = {} }
  held[slot] = h
  local v, l, reset, count = hs.ChatText(slot, h.version, false, false, true)
  if v == h.version then return "same" end
  h.version = v
  if reset then h.lines = {} end
  for _, s in ipairs(l) do h.lines[#h.lines + 1] = (s:gsub("<[^>]*>", "")) end
  while #h.lines > count do table.remove(h.lines, 1) end
  return (reset and "reset:" or "add:") .. #l
end
function _G.text(slot) return table.concat(held[slot].lines, "|") end
function _G.all(slot)
  local _, l = hs.ChatText(slot)
  local out = {}
  for _, s in ipairs(l) do out[#out + 1] = (s:gsub("<[^>]*>", "")) end
  return table.concat(out, "|")
end
)");
        view.add(ui::ChatChannel::Say, "hello");
        CHECK(lua.run("return hs.ChatNewWindow()") == "2");
        lua.run("hs.ChatSetFilter(2, 'Melee', true)");
        CHECK(lua.run("return follow(1)") == "reset:1");
        CHECK(lua.run("return follow(2)") == "reset:1"); // (its "All melee channels ..." line)
        CHECK(lua.run("return follow(1)") == "same");

        // A line in one window leaves the other's version as it was; the window it went to gets that line alone.
        CHECK(lua.run("_G.v1 = hs.ChatText(1) return 'ok'") == "ok");
        view.add(ui::ChatChannel::Combat, "hit 1", 0, 265);
        CHECK(lua.run("return tostring(hs.ChatText(1) == v1)") == "true");
        CHECK(lua.run("return follow(1)") == "same");
        CHECK(lua.run("return follow(2)") == "add:1");
        view.add(ui::ChatChannel::Say, "again");
        view.add(ui::ChatChannel::Say, "and again");
        CHECK(lua.run("return follow(1)") == "add:2");
        CHECK(lua.run("return text(1)") == "hello|again|and again");
        CHECK(lua.run("return text(1) == all(1)") == "true");
        // The old form still gives every line when the version differs, and the version alone when it is the same.
        CHECK(lua.run("local v, l = hs.ChatText(1, -1) return #l") == "3");
        CHECK(lua.run("local v = hs.ChatText(1) local v2, l = hs.ChatText(1, v) return tostring(l)") == "nil");

        // Past 1000 lines the oldest leave the front: the script drops them by the count.
        for (int i = 2; i <= 1003; ++i) view.add(ui::ChatChannel::Combat, std::format("hit {}", i), 0, 265);
        CHECK(lua.run("return follow(2)") == "add:1000"); // (hit 2 and 3 left the buffer before the script looked)
        CHECK(lua.run("return #held[2].lines .. ' ' .. held[2].lines[1]") == "1000 hit 4");
        CHECK(lua.run("return text(2) == all(2)") == "true");

        // Clear: everything again (none), and from there new lines only.
        lua.run("hs.ChatClear(1)");
        CHECK(lua.run("return follow(1)") == "reset:0");
        view.add(ui::ChatChannel::Say, "after");
        CHECK(lua.run("return follow(1)") == "add:1");
        CHECK(lua.run("return text(1)") == "after");
        // Another stamp setting, another window's version: everything again.
        CHECK(lua.run("local v, l, reset = hs.ChatText(1, held[1].version, false, true, true) return tostring(reset)") ==
              "true");
        CHECK(lua.run("local v, l, reset = hs.ChatText(2, held[1].version, false, false, true) return tostring(reset)") ==
              "true");

        // The buffer emptied (a camp): with no line since, everything again (none); with lines since, those, and
        // the lines gone are dropped by the count.
        view.lines.clear();
        ++view.revision;
        CHECK(lua.run("return follow(2)") == "reset:0");
        CHECK(lua.run("return text(2)") == "");
        CHECK(lua.run("return follow(1)") == "reset:0");
        view.add(ui::ChatChannel::Say, "new");
        CHECK(lua.run("return follow(1)") == "add:1");
        CHECK(lua.run("return text(1)") == "new");
        view.lines.clear();
        ++view.revision;
        view.add(ui::ChatChannel::Say, "fresh");
        CHECK(lua.run("return follow(1)") == "add:1");
        CHECK(lua.run("return text(1)") == "fresh");
    }
    ImGui::DestroyContext(ctx);
}

// The first extended colour (355..408) that goes to `channel`, 0 when none does.
int extended_color_of(int channel) {
    for (int color = 355; color <= 408; ++color)
        if (game::chat_channel_of_color(color) == channel) return color;
    return 0;
}

TEST_CASE("Chat windows: extended channels take only the extended colours and keep their own filter menu") {
    ImGuiContext* ctx = ImGui::CreateContext();
    test::ScratchDir scratch("chat_extended_routes");
    fs::path dir = scratch.path();
    fs::create_directories(dir / "game");
    write_strings(dir / "game");
    GameDir game_dir(dir / "game");
    {
        net::GameConnection conn(game_dir);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game_dir;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        Lua lua(api);
        lua.run(kLines + std::string("_G.lines = lines"));
        CHECK(lua.run("return hs.ChatNewWindow('Extended')") == "2");
        const auto original_server_settings = conn.chat_filters().settings;
        for (const auto& item : game::chat_filter_items()) {
            if (item.channel < game::kChatChannelCountRoF2) continue;
            CAPTURE(item.channel);
            int color = extended_color_of(item.channel);
            REQUIRE(color != 0); // every extended channel in the menu is reached by an extended colour
            lua.run(std::format("hs.ChatSetFilter(2, {}, true); hs.ChatClear(2)", item.channel));
            const std::string text = std::format("route {}", item.channel);
            view.add(ui::ChatChannel::System, text, 0, static_cast<std::uint32_t>(color));
            CHECK(lua.run("return lines(2)") == text);
        }
        CHECK(lua.run("for _, f in ipairs(hs.ChatFilters(2)) do if f.id == 107 then return f.name end end") == "Alerts");
        CHECK(lua.run("return #hs.ChatFilterGroups()") == "11");
        CHECK(lua.run("for _, g in ipairs(hs.ChatFilterGroups()) do if g.name == 'Healing' then return g.parent end end") ==
              std::to_string(static_cast<int>(game::ChatFilterMenu::Extended)));
        CHECK(lua.run("for _, f in ipairs(hs.ChatFilters(2)) do if f.id == 101 then return f.menu end end") ==
              std::to_string(static_cast<int>(game::ChatFilterMenu::Healing)));

        CHECK(lua.run("return hs.ChatNewWindow('Healing')") == "3");
        lua.run("hs.ChatSetFilter(3, 'Healing', true); hs.ChatClear(3); hs.ChatClear(2)");
        view.add(ui::ChatChannel::System, "received heal", 0, 402);
        view.add(ui::ChatChannel::System, "other direct damage", 0, 379);
        CHECK(lua.run("return lines(3)") == "received heal");
        CHECK(lua.run("return lines(2)") == "other direct damage");
        CHECK(conn.chat_filters().settings == original_server_settings);

        // The Filters menu is InitContextMenu's (Randoms, Other and the Melee and Spells items where they were, ALL
        // last), then Hailstone's Hide All, then Extended (server). Every submenu ends in ALL and Hide All.
        lua.run("hs.ChatMenu(2)");
        REQUIRE(host.menus.size() == 1);
        auto find = [](const std::vector<app::UiHost::MenuRow>& rows, const char* name) {
            return std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.text == name; });
        };
        auto filters = find(host.menus[0].rows, "Filters");
        REQUIRE(filters != host.menus[0].rows.end());
        REQUIRE(filters->submenu);
        auto filter_rows = filters->submenu();
        auto check_bulk_controls = [](const std::vector<app::UiHost::MenuRow>& rows) {
            REQUIRE(rows.size() >= 2);
            CHECK(rows[rows.size() - 2].text == "ALL");
            CHECK(rows.back().text == "Hide All");
        };
        REQUIRE(filter_rows.size() >= 3);
        CHECK(filter_rows[filter_rows.size() - 3].text == "ALL");
        CHECK(filter_rows[filter_rows.size() - 2].text == "Hide All");
        CHECK(filter_rows.back().text == "Extended (server)");
        auto spells = find(filter_rows, "Spells");
        REQUIRE(spells != filter_rows.end());
        REQUIRE(spells->submenu);
        const auto spell_rows = spells->submenu();
        check_bulk_controls(spell_rows);
        CHECK(spell_rows.size() == 10); // the eight RoF2 items, ALL and Hide All
        auto melee = find(filter_rows, "Melee");
        REQUIRE(melee != filter_rows.end());
        REQUIRE(melee->submenu);
        const auto melee_rows = melee->submenu();
        check_bulk_controls(melee_rows);
        CHECK(melee_rows.size() == 20); // the 18 RoF2 items, ALL and Hide All
        REQUIRE(filter_rows.back().submenu);
        auto extended_rows = filter_rows.back().submenu();
        auto healing = find(extended_rows, "Healing");
        REQUIRE(healing != extended_rows.end());
        REQUIRE(healing->submenu);
        auto healing_rows = healing->submenu();
        check_bulk_controls(healing_rows);
        CHECK(healing_rows.size() == 7); // five channels, ALL and Hide All
        api.save_layout();
    }
    {
        net::GameConnection conn(game_dir);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game_dir;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        Lua lua(api);
        lua.run(kLines + std::string("_G.lines = lines"));
        for (const auto& item : game::chat_filter_items()) {
            if (item.channel < game::kChatChannelCountRoF2) continue;
            const int window = item.menu == game::ChatFilterMenu::Healing ? 3 : 2;
            CHECK(lua.run(std::format("for _, f in ipairs(hs.ChatFilters({})) do if f.id == {} then return f.on end end",
                                      window, item.channel)) == "true");
        }
        view.add(ui::ChatChannel::System, "alert after reload", 0, 408);
        view.add(ui::ChatChannel::System, "heal after reload", 0, 402);
        CHECK(lua.run("return lines(2)") == "alert after reload");
        CHECK(lua.run("return lines(3)") == "heal after reload");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Chat windows: a layout and colours saved before the extended channels load as they were") {
    ImGuiContext* ctx = ImGui::CreateContext();
    test::ScratchDir scratch("chat_main_era_layout");
    fs::path dir = scratch.path();
    fs::create_directories(dir / "game");
    write_strings(dir / "game");
    // User_1..99 only, as the original and the client before the extended colours keep them.
    std::string colors = "[TextColors]\n";
    for (int n = 1; n <= game::kChatUserColorCountRoF2; ++n)
        colors += std::format("User_{0}_Red={0}\nUser_{0}_Green=1\nUser_{0}_Blue=2\n", n);
    std::ofstream(dir / "game" / "eqclient.ini", std::ios::binary) << colors;
    GameDir game_dir(dir / "game");
    {
        net::GameConnection conn(game_dir);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game_dir;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        Lua lua(api);
        CHECK(lua.run("return hs.ChatNewWindow('Combat')") == "2");
        lua.run("hs.ChatSetFilter(2, 22, true); hs.ChatSetFilter(2, 40, true); hs.ChatSetFilter(2, 56, true)");
        api.save_layout();
    }
    // As the client before the extended channels wrote it: ChannelMap0..56 only.
    {
        std::ifstream in(dir / "UI_Test_Server.ini", std::ios::binary);
        std::string saved{std::istreambuf_iterator<char>(in), {}}, kept, line;
        bool had_56 = false;
        for (std::size_t at = 0; at < saved.size();) {
            std::size_t end = saved.find('\n', at);
            line = saved.substr(at, end == std::string::npos ? std::string::npos : end + 1 - at);
            at = end == std::string::npos ? saved.size() : end + 1;
            int n = -1;
            if (line.starts_with("ChannelMap")) n = std::atoi(line.c_str() + 10);
            if (n == 56) had_56 = true;
            if (n < game::kChatChannelCountRoF2) kept += line;
        }
        REQUIRE(had_56);
        std::ofstream(dir / "UI_Test_Server.ini", std::ios::binary | std::ios::trunc) << kept;
    }
    {
        net::GameConnection conn(game_dir);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game_dir;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        Lua lua(api);
        lua.run(kLines + std::string("_G.lines = lines"));
        CHECK(lua.run("return #hs.ChatWindows()") == "2");
        view.add(ui::ChatChannel::System, "non melee", 0, 283); // Chat::NonMelee: Non Melee Hits (40)
        view.add(ui::ChatChannel::System, "red", 0, 13);        // Other (22)
        view.add(ui::ChatChannel::System, "pvp", 0, 350);       // PvP Messages (56)
        view.add(ui::ChatChannel::System, "alert", 0, 408);     // an extended channel: the main window still
        CHECK(lua.run("return lines(2)") == "non melee|red|pvp");
        CHECK(lua.run("return lines(1)") == "alert");
        for (int index = 0; index < game::kChatUserColorCountRoF2; ++index)
            CHECK(app::user_color_rgb(index) == (static_cast<std::uint32_t>(index + 1) << 16 | 0x0102u));
        for (int index = game::kChatUserColorCountRoF2; index < game::kChatUserColorCount; ++index)
            CHECK(app::user_color_rgb(index) == app::default_user_color_rgb(index));
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Chat palette: extended colours load, edit, persist and reset without writing game assets") {
    constexpr std::array considers{game::Con::Red, game::Con::Yellow, game::Con::White,
                                   game::Con::DarkBlue, game::Con::LightBlue, game::Con::Green, game::Con::Gray};
    for (std::size_t index = 0; index < considers.size(); ++index)
        CHECK(app::default_user_color_rgb(390 - 256 + static_cast<int>(index)) == game::con_rgb(considers[index]));
    ImGuiContext* ctx = ImGui::CreateContext();
    test::ScratchDir scratch("chat_extended_colors");
    fs::path dir = scratch.path();
    fs::create_directories(dir / "game");
    write_strings(dir / "game");
    const std::string asset_ini = "[TextColors]\nUser_3_Red=7\nUser_3_Green=8\nUser_3_Blue=9\n"
                                  "User_100_Red=1\nUser_100_Green=2\nUser_100_Blue=3\n"
                                  "User_153_Red=4\nUser_153_Green=5\nUser_153_Blue=6\n";
    std::ofstream(dir / "game" / "eqclient.ini", std::ios::binary) << asset_ini;
    GameDir game_dir(dir / "game");
    auto chosen_color = [](int index) { return 0x010000u + static_cast<std::uint32_t>(index) * 0x0101u; };
    for (int session = 0; session < 2; ++session) {
        net::GameConnection conn(game_dir);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game_dir;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        auto names = api.list_texts("OptionsWindow", "ColorChooseList");
        CHECK(names.size() == 151); // 97 existing visible rows plus all 54 extended colours
        for (int index = 99; index < game::kChatUserColorCount; ++index) {
            CHECK_FALSE(app::extended_chat_color_name(index).empty());
            CHECK(std::count(names.begin(), names.end(), app::extended_chat_color_name(index)) == 1);
        }
        if (session == 0) {
            CHECK(app::user_color_rgb(99) == 0x010203u);
            CHECK(app::user_color_rgb(152) == 0x040506u);
            auto alerts = std::find(names.begin(), names.end(), "Alerts");
            REQUIRE(alerts != names.end());
            CHECK(api.click_list("OptionsWindow", "ColorChooseList", static_cast<int>(alerts - names.begin()), 0));
            CHECK(api.click_child("OptionsWindow", "UserColorChange"));
            REQUIRE(host.color_pick);
            host.color_pick->done(0xff123456u);
            CHECK(app::user_color_rgb(152) == 0x123456u);
            for (int index = 99; index < game::kChatUserColorCount; ++index)
                app::set_user_color(host, index, chosen_color(index));
            app::set_user_color(host, 153, 0xabcdefu); // outside the palette
            IniFile saved;
            REQUIRE(saved.load(dir / "chat.ini"));
            CHECK_FALSE(saved.get("TextColors", "User_154_Red"));
            for (int index = 99; index < game::kChatUserColorCount; ++index)
                CHECK(saved.get_int("TextColors", std::format("User_{}_Red", index + 1), -1) ==
                      static_cast<int>((chosen_color(index) >> 16) & 0xffu));
        } else {
            for (int index = 99; index < game::kChatUserColorCount; ++index)
                CHECK(app::user_color_rgb(index) == chosen_color(index));
            CHECK(app::user_color_rgb(2) == 0x070809u); // unchanged read-only asset default
            CHECK(api.click_child("OptionsWindow", "UserColorDefault"));
            REQUIRE(host.confirm);
            host.confirm->done(false);
            CHECK(app::user_color_rgb(152) == chosen_color(152));
            host.confirm->done(true);
            for (int index = 0; index < game::kChatUserColorCount; ++index)
                CHECK(app::user_color_rgb(index) == app::default_user_color_rgb(index));
            CHECK(app::user_color_rgb(153) == app::user_color_rgb(0));
            IniFile saved;
            REQUIRE(saved.load(dir / "chat.ini"));
            CHECK(saved.get_int("TextColors", "User_153_Red", -1) ==
                  static_cast<int>((app::default_user_color_rgb(152) >> 16) & 0xffu));
        }
    }
    std::ifstream unchanged(dir / "game" / "eqclient.ini", std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(unchanged), {}) == asset_ini);
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Chat windows: GetChannelFromColor, the Filters menu, the tell menus' text, LastTell_Add") {
    CHECK(game::chat_channel_of_color(256) == 0);  // say
    CHECK(game::chat_channel_of_color(307) == 0);  // our say
    CHECK(game::chat_channel_of_color(257) == 1);  // tell
    CHECK(game::chat_channel_of_color(308) == 1);  // the tell echo
    CHECK(game::chat_channel_of_color(327) == 3);  // raid
    CHECK(game::chat_channel_of_color(345) == 4);  // guild
    CHECK(game::chat_channel_of_color(291) == 12); // chat channel 1
    CHECK(game::chat_channel_of_color(324) == 21); // our chat channel 10
    CHECK(game::chat_channel_of_color(277) == 28); // my death
    CHECK(game::chat_channel_of_color(350) == 56); // PvP (0x15e)
    CHECK(game::chat_channel_of_color(13) == 22);  // the basic colours: Other
    CHECK(game::chat_channel_of_color(351) == 22);
    // every channel but the unused 48 is in a menu once
    std::array<int, game::kChatChannelCount> seen{};
    for (const auto& it : game::chat_filter_items()) ++seen[it.channel];
    for (int ch = 0; ch < game::kChatChannelCountRoF2; ++ch) CHECK(seen[static_cast<std::size_t>(ch)] == (ch == 48 ? 0 : 1));

    CHECK(game::tell_menu_text("Bob", "tell") == "/tell Bob ");
    CHECK(game::tell_menu_text("server.Bob", "tell") == ";tell server.Bob ");
    CHECK(game::tell_menu_text(".Bob", "tell") == ";tell Bob ");
    CHECK(game::tell_menu_text("[Bob", "tell") == "[tell Bob ");

    std::vector<std::string> names;
    for (int i = 0; i < 55; ++i) game::add_last_tell(names, "p" + std::to_string(i));
    CHECK(names.size() == 50);
    CHECK(names.front() == "p54");
    game::add_last_tell(names, "P50");
    CHECK(names.size() == 50);
    CHECK(names[0] == "P50");
    CHECK(names[1] == "p54");
    CHECK(std::count_if(names.begin(), names.end(), [](const std::string& n) { return n == "p50"; }) == 0);

    CHECK(game::hit_mode_eqstr(0, 2) == 15036); // Number Only
    CHECK(game::hit_mode_eqstr(1, 2) == 15037); // Miss Only
    CHECK(game::hit_mode_eqstr(6, 2) == 15036); // ALL's
}

TEST_CASE("Chat windows: the reply keys' index (LastTell_CycleIndex) and CYCLEREPLY's text") {
    std::vector<std::string> names;
    int index = 0;
    CHECK_FALSE(game::cycle_last_tell(names, index, false)); // no names: 12364, the index stays
    CHECK(index == 0);
    names = {"Cara", "Bob", "Ann"}; // the newest first
    // CYCLEREPLY: one older while there is one, then back to the newest
    CHECK(game::cycle_last_tell(names, index, false));
    CHECK(index == 1);
    game::cycle_last_tell(names, index, false);
    CHECK(index == 2);
    game::cycle_last_tell(names, index, false);
    CHECK(index == 0);
    // CYCLEREPLY_BACK: one newer, from the newest round to the oldest
    game::cycle_last_tell(names, index, true);
    CHECK(index == 2);
    game::cycle_last_tell(names, index, true);
    CHECK(index == 1);
    std::vector<std::string> full;
    for (int i = 0; i < 50; ++i) full.push_back("p" + std::to_string(i));
    index = 0;
    game::cycle_last_tell(full, index, true);
    CHECK(index == 49);
    game::cycle_last_tell(full, index, false);
    CHECK(index == 0); // past the 50th: the newest

    CHECK(game::reply_target_text("Bob", "/rtarget") == "/rtarget Bob");
    CHECK(game::reply_target_text("[Bob", "/rtarget") == "/rtarget Bob");
    CHECK(game::reply_target_text(";Bob", "/rtarget") == "/rtarget Bob");

    // The chat input's text decides: past its first character "rtarget", else "tell" (case-sensitive), else nothing.
    index = 0;
    auto r = game::cycle_reply("/tell ", names, index, false, "/tell", "/rtarget");
    CHECK(index == 1);
    CHECK(r.text == "/tell Bob ");
    CHECK(r.messages.empty());
    r = game::cycle_reply(";tell Bob hi", names, index, false, "/tell", "/rtarget");
    CHECK(r.text == "/tell Ann ");
    r = game::cycle_reply("/rtarget Ann", names, index, true, "/tell", "/rtarget");
    CHECK(index == 1);
    CHECK(r.text == "/rtarget Bob");
    names[1] = "[Bob";
    r = game::cycle_reply("/tell x", names, index, true, "/tell", "/rtarget");
    CHECK(r.text == "/tell Cara ");
    r = game::cycle_reply("/tell x", names, index, false, "/tell", "/rtarget");
    CHECK(r.text == "[tell Bob ");
    r = game::cycle_reply("/TELL x", names, index, false, "/tell", "/rtarget");
    CHECK(r.text.empty());
    CHECK(index == 1); // not a reply line: the index stays
    r = game::cycle_reply("hello", names, index, false, "/tell", "/rtarget");
    CHECK(r.text.empty());
    CHECK(r.messages.empty());
    // Nobody to reply to: 12364 (the index's) and 12365, the input stays.
    names.clear();
    index = 0;
    r = game::cycle_reply("/tell ", names, index, false, "/tell", "/rtarget");
    CHECK(r.text.empty());
    CHECK(r.messages == std::vector<std::uint32_t>{12364, 12365});

    // Each new name puts the index back on the newest.
    game::SocialState social;
    game::add_last_tell(social, "Ann");
    game::add_last_tell(social, "Bob");
    social.last_tell_index = 1;
    game::add_last_tell(social, "Cara");
    CHECK(social.last_tell_index == 0);
    CHECK(social.last_tells.front() == "Cara");
}

TEST_CASE("Chat windows: the chat keys fill the active window's input (TELL, CMDMODE_*, HISTORY_*) and page it") {
    ImGuiContext* ctx = ImGui::CreateContext();
    hailstone::test::ScratchDir dir_scratch("chat_keys_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    write_strings(dir / "game");
    GameDir game(dir / "game");
    std::vector<std::string> submitted;
    {
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        host.submit_chat = [&](const std::string& t) { submitted.push_back(t); };
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        Lua lua(api);
        const char* take = "local t, r = hs.ChatTakeFocus(1, 'typed'); return tostring(t) .. '|' .. tostring(r)";
        CHECK(lua.run(take) == "nil|nil");
        app::tell_key(); // (no eqstr 13974 here: the English word)
        CHECK(lua.run(take) == "/tell |true");
        CHECK(lua.run(take) == "nil|nil"); // once
        app::put_chat_input("'");
        CHECK(lua.run(take) == "'|true");

        // HISTORY_UP / HISTORY_DOWN: the lines sent before, as the input's Up / Down
        lua.run("hs.ChatSubmit(1, 'one')");
        lua.run("hs.ChatSubmit(1, 'two')");
        app::chat_history_key(true);
        CHECK(lua.run(take) == "two|true");
        app::chat_history_key(true);
        CHECK(lua.run(take) == "one|true");
        app::chat_history_key(false);
        CHECK(lua.run(take) == "two|true");

        // PAGEUP / PAGEDOWN: once, for the active window
        CHECK(lua.run("return hs.ChatPage(1)") == "nil");
        CHECK(lua.run("return hs.ChatNewWindow()") == "2");
        app::chat_page_key(-1);
        CHECK(lua.run("return hs.ChatPage(2)") == "nil");
        CHECK(lua.run("return hs.ChatPage(1)") == "-1");
        CHECK(lua.run("return hs.ChatPage(1)") == "nil");
        lua.run("hs.ChatSetAlwaysHere(1, false)");
        lua.run("hs.ChatActivate(2)");
        app::chat_page_key(1);
        CHECK(lua.run("return hs.ChatPage(2)") == "1");

        // The chat input's Tab with nobody to reply to, or on another line: the input stays.
        CHECK(lua.run("return hs.ChatCycleReply('/tell ', false)") == "nil");
        CHECK(lua.run("return hs.ChatCycleReply('hello', true)") == "nil");

        // ':' and "'" lines are the emote and say commands whatever the window's channel (CMDMODE_EMOTE / _SAY).
        lua.run("hs.ChatSetDefault(1, 2)"); // Group
        lua.run("hs.ChatSubmit(1, ':waves')");
        lua.run("hs.ChatSubmit(1, \"'hi\")");
        lua.run("hs.ChatSubmit(1, 'inc')");
        REQUIRE(submitted.size() >= 3);
        CHECK(std::vector<std::string>(submitted.end() - 3, submitted.end()) ==
              std::vector<std::string>{":waves", "'hi", "/gsay inc"});
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Chat windows: Always Chat Here takes the chat keys; the main window has it by default") {
    ImGuiContext* ctx = ImGui::CreateContext();
    hailstone::test::ScratchDir dir_scratch("chat_here_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    write_strings(dir / "game");
    GameDir game(dir / "game");
    auto takes = [](Lua& lua, int slot) {
        return lua.run(std::format("local t = hs.ChatTakeFocus({}); return tostring(t)", slot));
    };
    const char* here = "local s = '' for i = 1, 3 do s = s .. tostring(select(5, hs.ChatInfo(i))):sub(1, 1) end return s";
    {
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        Lua lua(api);
        lua.run(kLines + std::string("_G.lines = lines"));
        CHECK(lua.run("return hs.ChatNewWindow()") == "2");
        CHECK(lua.run("return hs.ChatNewWindow()") == "3");
        CHECK(lua.run(here) == "tff"); // a new character: the main window's
        // Another window clicked: the keys still start a line in the main window.
        lua.run("hs.ChatActivate(2)");
        app::tell_key();
        CHECK(takes(lua, 2) == "nil");
        CHECK(takes(lua, 1) == "/tell ");
        CHECK(lua.run("return tostring(select(2, hs.ChatInfo(1)))") == "true");

        // Ticked on window 3: it takes the keys (and says so in itself).
        std::string before = lua.run("return lines(3)");
        lua.run("hs.ChatSetAlwaysHere(3, true)");
        CHECK(lua.run(here) == "fft");
        CHECK(lua.run("return lines(3)") != before);
        app::tell_key();
        CHECK(takes(lua, 1) == "nil");
        CHECK(takes(lua, 3) == "/tell ");
        api.save_layout();
        ui::UiIni* ini = app::chat_ui_ini();
        REQUIRE(ini);
        CHECK(ini->get("ChatManager", "LockedActiveWindow") == "2");

        // Unticked there: back to the main window.
        lua.run("hs.ChatSetAlwaysHere(3, false)");
        CHECK(lua.run(here) == "tff");
        // Window 2 locked and closed: the main window again.
        lua.run("hs.ChatSetAlwaysHere(2, true)");
        lua.run("hs.ChatCloseWindow(2)");
        CHECK(lua.run("return tostring(select(5, hs.ChatInfo(1)))") == "true");
        // Unticked on the main window: no lock, the last-clicked window has the keys (the original's way).
        lua.run("hs.ChatSetAlwaysHere(1, false)");
        CHECK(lua.run("return tostring(select(5, hs.ChatInfo(1)))") == "false");
        lua.run("hs.ChatActivate(3)");
        app::tell_key();
        CHECK(takes(lua, 3) == "/tell ");
        api.save_layout();
        CHECK(ini->get("ChatManager", "LockedActiveWindow") == "-1");
    }
    {
        // The next session keeps "no lock".
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        Lua lua(api);
        CHECK(lua.run("return tostring(select(5, hs.ChatInfo(1)))") == "false");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Chat windows: the chat history keeps each line's time; a version 1 file reads without times") {
    ImGuiContext* ctx = ImGui::CreateContext();
    hailstone::test::ScratchDir dir_scratch("chat_history_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    write_strings(dir / "game");
    GameDir game(dir / "game");
    // One session: its lines in a fresh chat view, `check` run once the layout (and the history) is loaded.
    auto session = [&](const std::function<void(Lua&, ui::ChatView&)>& check) {
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        Lua lua(api);
        check(lua, view);
        app::save_chat_history(true);
    };
    const char* stamped = "local _, l = hs.ChatText(1, nil, false, true) local n = 0 "
                          "for _, s in ipairs(l) do if s:gsub('<[^>]*>', ''):match('^%[%d%d:%d%d:%d%d%] ') then n = n + 1 end end "
                          "return n .. '/' .. #l";
    session([&](Lua& lua, ui::ChatView& view) {
        view.add(ui::ChatChannel::Say, "hello");
        view.add(ui::ChatChannel::OOC, "ooc");
        CHECK(lua.run(stamped) == "2/2");
    });
    // The next session: both lines back with their times; the session mark has none.
    session([&](Lua& lua, ui::ChatView&) { CHECK(lua.run(stamped) == "2/3"); });
    // A version 1 file (no time column): its line comes back without a stamp.
    fs::path file;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.path().filename() == "chat_history.txt") file = e.path();
    REQUIRE_FALSE(file.empty());
    std::ofstream(file) << "# Hailstone chat history 1\n" << "1\t0\t0\t\told line\n";
    session([&](Lua& lua, ui::ChatView&) {
        CHECK(lua.run(stamped) == "0/2");
        CHECK(lua.run("local _, l = hs.ChatText(1) return (l[1]:gsub('<[^>]*>', ''))") == "old line");
    });
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Item display: a click on the icon links the item into the active chat window's input "
          "(CItemDisplayWnd::WndNotification, IconButton)") {
    ImGuiContext* ctx = ImGui::CreateContext();
    hailstone::test::ScratchDir dir_scratch("item_icon_link_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    std::ofstream(dir / "game" / "eqstr_us.txt") << "EQST0002\n6623 Main Chat\n6624 Chat\n15009 Always Chat Here\n"
                                                  << "15010 Chat here off\n15011 Chat here on\n";
    GameDir game(dir / "game");
    {
        net::GameConnection conn(game);
        ui::ChatView view;
        std::vector<net::Item> displays(1);
        std::vector<std::uint64_t> ids{1};
        net::Item& item = displays[0];
        item.id = 1001;
        item.name = "Cloth Cap";
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        host.item_displays = &displays;
        host.item_display_ids = &ids;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        Lua lua(api);
        const std::string link = game::make_item_link(item, conn.server_ext().wide_item_links(conn));
        auto take = [&](int slot) {
            return lua.run(std::format("local t, r = hs.ChatTakeFocus({}); return tostring(t) .. '|' .. tostring(r)",
                                       slot));
        };
        const char* click = "return hs.ClickChild('ItemDisplayWindow', 'IconButton', 'left')";

        // By default the main window is the Always Chat Here one (Hailstone's default lock): it gets the link,
        // added after what is typed there, whichever window was used last.
        CHECK(lua.run("return hs.ChatNewWindow()") == "2");
        lua.run("hs.ChatActivate(2)");
        CHECK(lua.run(click) == "true");
        CHECK(take(2) == "nil|nil");
        CHECK(take(1) == link + "|false");
        CHECK(take(1) == "nil|nil"); // once

        // Without a lock: the window used last.
        lua.run("hs.ChatSetAlwaysHere(1, false)");
        lua.run("hs.ChatActivate(2)");
        lua.run(click);
        CHECK(take(1) == "nil|nil");
        CHECK(take(2) == link + "|false");
        lua.run("hs.ChatActivate(1)");
        lua.run(click);
        CHECK(take(2) == "nil|nil");
        CHECK(take(1) == link + "|false");

        // Always Chat Here ticked on the second window (its menu row): it gets the link.
        lua.run("hs.ChatMenu(2)");
        REQUIRE(host.menus.size() == 1);
        auto here = std::find_if(host.menus[0].rows.begin(), host.menus[0].rows.end(),
                                 [](const app::UiHost::MenuRow& r) { return r.text == "Always Chat Here"; });
        REQUIRE(here != host.menus[0].rows.end());
        here->act();
        lua.run("hs.ChatActivate(1)");
        lua.run(click);
        CHECK(take(1) == "nil|nil");
        CHECK(take(2) == link + "|false");

        // An item, or an augment in it, with a BenefitFlag: no link (ItemBase_GetUnknown0x100Max(1) != 0).
        item.benefit_flag = 1;
        CHECK(lua.run(click) == "true");
        CHECK(take(2) == "nil|nil");
        item.benefit_flag = 0;
        net::BagItem aug;
        aug.index = 0;
        aug.item.id = 2002;
        aug.item.benefit_flag = 3;
        item.contents.push_back(aug);
        lua.run(click);
        CHECK(take(2) == "nil|nil");
        item.contents.clear();

        // A right click is not the icon's.
        CHECK(lua.run("return hs.ClickChild('ItemDisplayWindow', 'IconButton', 'right')") == "false");
        CHECK(take(2) == "nil|nil");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Chat windows: a kept list is refilled in place, with no garbage while nothing changes") {
    ImGuiContext* ctx = ImGui::CreateContext();
    hailstone::test::ScratchDir dir_scratch("chat_windows_into_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    write_strings(dir / "game");
    GameDir game(dir / "game");
    {
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        Lua lua(api);
        CHECK(lua.run("return hs.ChatNewWindow()") == "2");
        CHECK(lua.run("into={}; local w=hs.ChatWindows(into); first, tabs=w[1], w[1].tabs; "
                      "return tostring(rawequal(w, into)) .. #w .. w[2].title") == "true2Chat 1");
        CHECK(lua.run("collectgarbage('stop') local b=collectgarbage('count') local w=hs.ChatWindows(into) "
                      "local a=collectgarbage('count') collectgarbage('restart') "
                      "return rawequal(w, into) and rawequal(w[1], first) and rawequal(w[1].tabs, tabs) and a==b") == "true");
        // A window gone: the list is trimmed.
        lua.run("hs.ChatCloseWindow(2)");
        CHECK(lua.run("local w=hs.ChatWindows(into); return #w .. tostring(w[2]) .. w[1].title") == "1nilMain Chat");
        // Without it, a new list with the same windows, as before.
        CHECK(lua.run("local w=hs.ChatWindows(); return tostring(not rawequal(w, into)) .. #w") == "true1");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Chat tab API: moving a tab preserves its identity, routing and preferences") {
    ImGuiContext* ctx = ImGui::CreateContext();
    test::ScratchDir scratch("chat_tab_api");
    const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    write_strings(dir / "game");
    GameDir game(dir / "game");
    const auto layout = dir / "UI_Test_Server.ini";
    {
        net::GameConnection conn(game);
        ui::ChatView view;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", layout, {});
        Lua lua(api);
        lua.run(kLines + std::string("_G.lines = lines"));
        REQUIRE(lua.run(R"(
            chat, into = hs.UI.chat, {}
            assert(hs.UI.Supports('chat', 'SplitTab'))
            assert(chat.NewTab(1, 'Social') == 2)
            chat.SetFilter(1, 0, true)
            assert(chat.SetWindowTitle(1, 'Messages'))
            chat.SetTitle(1, 'Conversation', 2)
            assert(chat.SetTabFlash(1, 2, false))
            assert(chat.SetTabFlashColor(1, 2, 0x2457CA))
            assert(chat.SetTabScrollbar(1, 2, false))
            chat.SelectTab(1, 1)
            tabSlot = chat.Windows(into)[1].tabs[2].slot
            assert(not chat.SetTabFlashColor(1, 2, -1))
            assert(not chat.SetTabFlashColor(1, 2, 0x1000000))
            assert(not chat.ReorderTab(1, 2, math.maxinteger))
            assert(chat.SplitTab(1, math.mininteger) == nil)
            return true
        )") == "true");

        view.add(ui::ChatChannel::Say, "before split");
        REQUIRE(lua.run(R"(
            local w = chat.Windows(into)[1]
            local t = w.tabs[2]
            assert(t.unread and not t.flash and not t.highlighted and not t.scrollbar)
            assert(t.flashColor == 0x2457CA and t.title == 'Conversation')
            detached = assert(chat.SplitTab(1, 2))
            assert(detached ~= 1 and #chat.Windows(into) == 2)
            assert(chat.Windows(into)[2].tabs[1].slot == tabSlot)
            assert(lines(detached):find('before split', 1, true))
            assert(chat.SplitTab(detached, 1) == nil)
            return true
        )") == "true");

        view.add(ui::ChatChannel::Say, "after split");
        REQUIRE(lua.run(R"(
            assert(lines(detached):find('after split', 1, true))
            assert(chat.JoinTab(detached, 1, 1))
            assert(#chat.Windows(into) == 1)
            assert(chat.ReorderTab(1, 2, -1))
            local w = chat.Windows(into)[1]
            assert(w.main and w.windowTitle == 'Messages' and w.tab == 1)
            assert(w.title == 'Conversation' and w.tabs[1].slot == tabSlot)
            assert(w.tabs[1].flashColor == 0x2457CA and not w.tabs[1].flash and not w.tabs[1].scrollbar)
            assert(chat.SetTabFlashColor(1, 1, nil))
            assert(chat.Windows(into)[1].tabs[1].flashColor == nil)
            assert(chat.SetTabFlashColor(1, 1, 0))
            local extra = assert(chat.NewWindow('Extra'))
            assert(chat.CombineTabs(1) == 1)
            assert(chat.CombineTabs(extra) == 0 and #chat.Windows(into)[1].tabs == 3)
            chat.Clear(1, 1)
            chat.SelectTab(1, 1)
            assert(lines(1) == '')
            return true
        )") == "true");

        api.save_layout();
        api.load_layout(dir / "defaults.ini", layout, {});
        CHECK(lua.run(R"(
            local w = chat.Windows()[1]
            local t = w.tabs[1]
            return #chat.Windows() == 1 and #w.tabs == 3 and w.windowTitle == 'Messages'
                and w.tab == 1 and t.title == 'Conversation' and t.flashColor == 0
                and not t.flash and not t.scrollbar and t.slot == tabSlot
        )") == "true");
        CHECK(lua.run(R"(
            chat.CloseWindow(1)
            assert(#chat.Windows()[1].tabs == 3)
            chat.CloseTab(1, 1)
            local w = chat.Windows()[1]
            return w.main and #w.tabs == 2 and w.tabs[1].title == 'Main Chat'
        )") == "true");
    }
    ImGui::DestroyContext(ctx);
}
