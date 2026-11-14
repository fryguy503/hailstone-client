// What a shipped UI window keeps between frames must not outlive what it was for: a right click slid off a spell gem
// forgets nothing, a spell dragged from a closed book memorizes nothing, the spell set editor of one character is
// gone for the next, and the gamepad book's progress bar is the scribe gauge while a scroll is scribed. The stock
// windows' state is stood in for from Lua (eq.* replaced), and the mouse is ImGui's own input.
#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/lua_eq.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct Session {
    GameDir game;
    net::GameConnection conn;
    ui::ChatView view;
    app::EqHost host;
    app::EqApi api;
    ui::lua::LuaUi lua;
    Session(const fs::path& dir, const std::string& pack)
        : game(dir / "game"), conn(game), api(host), lua(dir / pack, dir / "ui.txt") {
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        host.in_game = host.ui_ready = true;
        lua.add_eq_api([this](lua_State* L) {
            api.register_functions(L);
            lua_pushcfunction(L, [](lua_State* S) { lua_pushboolean(S, 1); return 1; });
            lua_setfield(L, -2, "InGame");
        });
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1600, 900);
            io.DeltaTime = 1.0f / 60.0f;
            api.begin_frame();
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        }
    }
};

// Every theme text ends in ui.DrawText (seen, saw); the eq calls a test watches are listed in `calls`; each
// InvisibleButton's rectangle by id (rects); fake.shown / fake.inGame / fake.name stand in for the game.
constexpr const char* kPrelude = R"lua(
seen, calls, rects = {}, {}, {}
local DrawText = ui.DrawText
ui.DrawText = function(...)
  seen[#seen + 1] = select(7, ...)
  return DrawText(...)
end
function saw(needle)
  for _, s in ipairs(seen) do
    if type(s) == "string" and s:find(needle, 1, true) then return true end
  end
  return false
end
function called(needle)
  for _, c in ipairs(calls) do
    if c:find(needle, 1, true) then return true end
  end
  return false
end
local IB = ui.InvisibleButton
ui.InvisibleButton = function(id, w, h, ...)
  local x, y = ui.GetCursorScreenPos()
  rects[id] = { x, y, w, h }
  return IB(id, w, h, ...)
end
fake = { shown = {}, inGame = true, name = "Alpha" }
eq.InGame = function() return fake.inGame end
local Label = eq.Label
eq.Label = function(n, ...)
  if n == 1 then return fake.name end
  return Label(n, ...)
end
local ReplaceWindow, WindowShown = eq.ReplaceWindow, eq.WindowShown
eq.ReplaceWindow = function(win, ...)
  local r = table.pack(ReplaceWindow(win, ...))
  if fake.shown[win] ~= nil then r[1] = fake.shown[win] end
  return table.unpack(r, 1, math.max(r.n, 1))
end
eq.WindowShown = function(win)
  if fake.shown[win] ~= nil then return fake.shown[win] end
  return WindowShown(win)
end
)lua";

struct PackRig {
    test::ScratchDir scratch;
    ImGuiContext* ctx = nullptr;
    std::unique_ptr<Session> s;
    lua_State* L = nullptr;

    explicit PackRig(const char* pack) : scratch("ui_pack_stale_state_test") {
        const fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        const fs::path dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        s = std::make_unique<Session>(dir, pack);
        s->frames(2);
        L = s->lua.state();
        run(kPrelude);
    }
    ~PackRig() {
        s.reset();
        ImGui::DestroyContext(ctx);
    }
    PackRig(const PackRig&) = delete;
    PackRig& operator=(const PackRig&) = delete;

    void run(const char* code) {
        INFO(code);
        REQUIRE_MESSAGE(luaL_dostring(L, code) == LUA_OK, (lua_isstring(L, -1) ? lua_tostring(L, -1) : ""));
        lua_settop(L, 0);
    }
    bool truth(const std::string& code) {
        INFO(code);
        REQUIRE(luaL_dostring(L, ("return " + code).c_str()) == LUA_OK);
        const bool v = lua_toboolean(L, -1) != 0;
        lua_settop(L, 0);
        return v;
    }
    bool saw(const std::string& needle) { return truth("saw('" + needle + "')"); }
    bool called(const std::string& needle) { return truth("called('" + needle + "')"); }
    // The middle of the InvisibleButton `id` as last drawn.
    ImVec2 centre(const std::string& id) {
        REQUIRE(luaL_dostring(L, ("local r = rects['" + id + "'] if not r then return nil end "
                                  "return r[1] + r[3] / 2, r[2] + r[4] / 2").c_str()) == LUA_OK);
        REQUIRE_MESSAGE(lua_isnumber(L, -2), id);
        ImVec2 c(static_cast<float>(lua_tonumber(L, -2)), static_cast<float>(lua_tonumber(L, -1)));
        lua_settop(L, 0);
        return c;
    }
    void mouse_to(ImVec2 p, int frames = 2) {
        ImGui::GetIO().AddMousePosEvent(p.x, p.y);
        s->frames(frames);
    }
    void button(ImGuiMouseButton b, bool down, int frames = 2) {
        ImGui::GetIO().AddMouseButtonEvent(b, down);
        s->frames(frames);
    }
    bool no_errors() { return ImGui::FindWindowByName("UI errors") == nullptr; }
};

// The spell bar (modern spellgems.lua) with every gem shown, empty, and the clicks on them listed.
constexpr const char* kGems = R"lua(
fake.shown.CastSpellWnd = true
eq.GemInfo = function(win, id, info)
  info.shown, info.tex, info.tooltip, info.overlay = true, nil, "", nil
  return info
end
eq.SpellGemClick = function(id, phase) calls[#calls + 1] = "gem " .. id .. " " .. tostring(phase) return true end
eq.SpellBookMemorize = function(slot, gem) calls[#calls + 1] = "memorize " .. slot .. " " .. gem return true end
)lua";

} // namespace

TEST_CASE("UI packs (modern): a right click slid off a spell gem forgets nothing; let go on it, it does") {
    PackRig r("modern");
    r.run(kGems);
    r.s->frames(3);
    const ImVec2 gem = r.centre("##CSPW_Spell0");
    r.mouse_to(gem);
    r.button(ImGuiMouseButton_Right, true);
    CHECK(r.called("gem CSPW_Spell0 rightdown"));
    r.mouse_to(ImVec2(gem.x, gem.y - 300)); // off the gem, and off the bar
    r.button(ImGuiMouseButton_Right, false);
    CHECK_FALSE(r.called("rightup"));
    // pressed and let go on the gem: forgotten
    r.run("calls = {}");
    r.mouse_to(gem);
    r.button(ImGuiMouseButton_Right, true);
    r.button(ImGuiMouseButton_Right, false);
    CHECK(r.called("gem CSPW_Spell0 rightup"));
    CHECK(r.no_errors());
}

TEST_CASE("UI packs (modern): held spell gem display stays only when released over it") {
    PackRig r("modern");
    r.run(kGems);
    r.run(R"lua(
eq.SpellBookState = function() return { gems = { 200, -1 } } end
local shown = eq.WindowShown
local rect = ui.FindWindowRect
eq.WindowShown = function(window)
  if window == 'SpellDisplayWindow' then return fake.display end
  return shown(window)
end
eq.SpellDetails = function() return 'Test spell' end
eq.WindowTitle = function() return fake.otherTitle and 'Another spell' or 'Test spell' end
ui.FindWindowRect = function(name)
  if name == '###modern.SpellDisplayWindow' then return 100, 100, 200, 100 end
  return rect(name)
end
eq.SpellGemClick = function(id, phase)
  calls[#calls + 1] = 'gem ' .. id .. ' ' .. phase
  if phase == 'rightheld' and id == 'CSPW_Spell0' then fake.display = true end
  if phase == 'rightupheld' then fake.display = false end
end
eq.ShowSpell = function(id) calls[#calls + 1] = 'show ' .. id fake.display = true end
)lua");
    r.s->frames(3);
    const ImVec2 gem = r.centre("##CSPW_Spell0");
    r.mouse_to(gem);
    r.button(ImGuiMouseButton_Right, true, 35);
    CHECK(r.truth("fake.display"));
    r.mouse_to(ImVec2(gem.x, gem.y - 300));
    r.button(ImGuiMouseButton_Right, false);
    CHECK_FALSE(r.truth("fake.display"));
    CHECK_FALSE(r.called("show 200"));
    r.run("calls = {}");
    r.mouse_to(gem);
    r.button(ImGuiMouseButton_Right, true, 35);
    r.mouse_to(ImVec2(150, 150));
    r.button(ImGuiMouseButton_Right, false);
    CHECK(r.truth("fake.display"));
    CHECK(r.called("show 200"));
    CHECK(r.called("rightupheld"));
    r.run("calls = {} fake.display = false fake.otherTitle = true");
    r.mouse_to(gem);
    r.button(ImGuiMouseButton_Right, true, 35);
    r.mouse_to(ImVec2(150, 150));
    r.button(ImGuiMouseButton_Right, false);
    CHECK_FALSE(r.called("show 200"));
    r.run("fake.otherTitle = false");
    r.run("calls = {} fake.display = false");
    r.mouse_to(r.centre("##CSPW_Spell1"));
    r.button(ImGuiMouseButton_Right, true, 35);
    r.button(ImGuiMouseButton_Right, false);
    CHECK_FALSE(r.called("show"));
    CHECK(r.no_errors());
}

TEST_CASE("UI packs (modern): a spell dragged from a book that closed memorizes nothing on a gem") {
    PackRig r("modern");
    r.run(kGems);
    r.s->frames(3);
    const ImVec2 gem = r.centre("##CSPW_Spell1");
    // a drag the book published before it closed (or before a camp): an earlier frame's
    r.run("modern_spell_drag, modern_spell_drag_frame = { slot = 7, icon = 1 }, ui.GetFrameCount() - 5");
    r.mouse_to(gem);
    CHECK_FALSE(r.truth("modern_spell_drag ~= nil"));
    r.run("modern_spell_drag, modern_spell_drag_frame = { slot = 7, icon = 1 }, ui.GetFrameCount() - 5");
    r.button(ImGuiMouseButton_Left, true, 1);
    r.run("modern_spell_drag, modern_spell_drag_frame = { slot = 7, icon = 1 }, ui.GetFrameCount() - 5");
    r.button(ImGuiMouseButton_Left, false, 1);
    CHECK_FALSE(r.called("memorize"));
    CHECK(r.no_errors());
}

// The spell sets the stand-in game has: one in use (Heals), nine free.
constexpr const char* kSets = R"lua(
eq.SpellSets = function()
  local t = { { name = "Heals", inUse = true, spells = { 200, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 } } }
  for n = 2, 10 do t[n] = { name = "", inUse = false, spells = {} } end
  return t
end
eq.EditSpellSet = function(n, ...) calls[#calls + 1] = "edit " .. n return n end
eq.SaveSpellSet = function(...) calls[#calls + 1] = "save" return 1 end
fake.shown.SpellBookWnd = true
)lua";

TEST_CASE("UI packs: the spell set editor is the character's: another character, or a camp, closes it") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        const bool modern = std::string(pack) == "modern";
        const char* open = modern ? "ui.SetSetting('test.book.edit', '1')" : "ui.SetSetting('test.book.view', 'edit1')";
        const char* heading = modern ? "Edit spell set" : "Edit Spell Set";
        PackRig r(pack);
        r.run(kSets);
        r.run(open);
        r.s->frames(3);
        r.run("seen = {}");
        r.s->frames(1);
        CHECK(r.saw(heading));
        // another character (logged in on another server, say): the editor is gone
        r.run("fake.name = 'Bravo'");
        r.s->frames(2);
        r.run("seen = {}");
        r.s->frames(1);
        CHECK_FALSE(r.saw(heading));
        CHECK(r.no_errors());
        // opened again, then camped and back as the same name: gone too
        r.run(modern ? "ui.SetSetting('test.book.edit', 'new')" : "ui.SetSetting('test.book.view', 'editnew')");
        r.s->frames(3);
        r.run("seen = {}");
        r.s->frames(1);
        CHECK(r.saw(modern ? "New spell set" : "New Spell Set"));
        r.run("fake.inGame = false");
        r.s->frames(2);
        r.run("fake.inGame = true seen = {}");
        r.s->frames(2);
        CHECK_FALSE(r.saw(modern ? "New spell set" : "New Spell Set"));
        CHECK_FALSE(r.called("edit "));
        CHECK_FALSE(r.called("save"));
        CHECK(r.no_errors());
    }
}

TEST_CASE("UI packs (gamepad): the spell book's bar is the scribe gauge while a scroll is scribed") {
    PackRig r("gamepad");
    r.run(R"lua(
fake.shown.SpellBookWnd = true
fake.state = { scribing = { slot = 3 }, gems = {} }
eq.SpellBookState = function() return fake.state end
local ChildShown, Gauge = eq.ChildShown, eq.Gauge
eq.ChildShown = function(win, child, ...)
  if win == "SpellBookWnd" and child == "SBW_Memorize_Gauge" then return true end
  return ChildShown(win, child, ...)
end
eq.Gauge = function(n, ...)
  if n == 9 or n == 10 then
    calls[#calls + 1] = "gauge " .. n
    return n == 9 and 0 or 0.5
  end
  return Gauge(n, ...)
end
)lua");
    r.s->frames(3);
    r.run("calls, seen = {}, {}");
    r.s->frames(1);
    CHECK(r.called("gauge 10"));
    CHECK_FALSE(r.called("gauge 9"));
    CHECK(r.saw("Scribing"));
    CHECK_FALSE(r.saw("Memorizing"));
    r.run("fake.state = { memorizing = { gem = 0, spell = 200 }, gems = {} } calls, seen = {}, {}");
    r.s->frames(1);
    CHECK(r.called("gauge 9"));
    CHECK_FALSE(r.called("gauge 10"));
    CHECK(r.saw("Memorizing"));
    CHECK(r.no_errors());
}

TEST_CASE("UI packs: adventure controls remain visible away from a recruiter and totals use the final record") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        const bool modern = std::string(pack) == "modern";
        PackRig r(pack);
        r.run(R"lua(
fake.shown.AdventureRequestWnd = true
local ChildState, ChildShown, WindowText, ComboInfo = eq.ChildState, eq.ChildShown, eq.WindowText, eq.ComboInfo
eq.ChildState = function(win, child, ...)
  if win == 'AdventureRequestWnd' then return child == 'AdvRqst_ViewStatsButton' end
  return ChildState(win, child, ...)
end
eq.ChildShown = function(win, child, ...)
  if win == 'AdventureRequestWnd' then return child ~= 'AdvRqst_LeaveButton' end
  return ChildShown(win, child, ...)
end
eq.WindowText = function(win, child, ...)
  if win == 'AdventureRequestWnd' then
    if child == 'AdvRqst_NPCText' then return 'No adventure is assigned.' end
    return ''
  end
  if win == 'AdventureStatsWnd' and child == 'AdvStats_AvailableValue' then return '901' end
  return WindowText(win, child, ...)
end
eq.ComboInfo = function(win, child, ...)
  if win == 'AdventureRequestWnd' then return { 'Normal', 'High' }, 0 end
  return ComboInfo(win, child, ...)
end
local Button = ui.Button
ui.Button = function(label, ...)
  calls[#calls + 1] = 'button ' .. label
  local clicked = Button(label, ...)
  local x0, y0, x1, y1 = ui.GetItemRect()
  rects[label] = { x0, y0, x1 - x0, y1 - y0 }
  return clicked
end
eq.ClickChild = function(win, child) calls[#calls + 1] = 'click ' .. win .. '.' .. child end
)lua");
        r.s->frames(3);
        if (modern) {
            CHECK(r.called("Request Adventure##AdvRqst_RequestButton"));
            CHECK(r.called("Accept##AdvRqst_AcceptButton"));
            CHECK(r.called("Decline##AdvRqst_DeclineButton"));
        } else {
            CHECK(r.saw("Request Adventure"));
            CHECK(r.saw("Accept"));
            CHECK(r.saw("Decline"));
        }
        CHECK_FALSE(r.saw("Leave Adventure"));
        const ImVec2 request = r.centre(modern ? "Request Adventure##AdvRqst_RequestButton" : "##AdventureRequestWnd.AdvRqst_RequestButton");
        r.mouse_to(request);
        r.button(ImGuiMouseButton_Left, true);
        r.button(ImGuiMouseButton_Left, false);
        CHECK_FALSE(r.called("click AdventureRequestWnd.AdvRqst_RequestButton"));
        const ImVec2 stats = r.centre(modern ? "View Stats##AdvRqst_ViewStatsButton" : "##AdventureRequestWnd.AdvRqst_ViewStatsButton");
        r.mouse_to(stats);
        r.button(ImGuiMouseButton_Left, true);
        r.button(ImGuiMouseButton_Left, false);
        CHECK(r.called("click AdventureRequestWnd.AdvRqst_ViewStatsButton"));
        CHECK(r.no_errors());

        r.run(R"lua(
fake.shown.AdventureRequestWnd = false
fake.shown.AdventureStatsWnd = true
local ListInfo, ListRow = eq.ListInfo, eq.ListRow
local records = {
  { 'Deepest Guk', '5', '2', '1', '66.7%', '8', '88' },
  { 'Totals', '7', '37', '9', '80.4%', '12', '901' },
}
eq.ListInfo = function(win, child, ...)
  if win == 'AdventureStatsWnd' and child == 'AdvStats_ThemeList' then
    return #records, 7, -1, { 'Theme', 'Rank', 'Successes', 'Failures', 'Percent', 'Rank (%)', 'Points' },
      { 150, 60, 70, 70, 60, 60, 60 }
  end
  return ListInfo(win, child, ...)
end
eq.ListRow = function(win, child, row, ...)
  if win == 'AdventureStatsWnd' and child == 'AdvStats_ThemeList' then return records[row + 1] end
  return ListRow(win, child, row, ...)
end
)lua");
        r.s->frames(3);
        r.run("seen = {}");
        r.s->frames(1);
        CHECK(r.saw("37"));
        CHECK(r.saw("901"));
        // The modern table omits Totals, while the gamepad table also repeats it below the summary.
        CHECK(r.truth(std::string("(function() local n = 0 for _, v in ipairs(seen) do if v == '80.4%' then n = n + 1 end end return n >= ") +
                      (modern ? "1" : "2") + " end)()"));
        CHECK(r.no_errors());

        r.run(R"lua(
fake.shown.AdventureStatsWnd = false
fake.shown.AdventureLeaderboardWnd = true
local WindowText = eq.WindowText
eq.WindowText = function(win, child, ...)
  if win == 'AdventureLeaderboardWnd' and child == 'AdvLeaderboard_StatusLabel' then return 'Updating leaderboard...' end
  return WindowText(win, child, ...)
end
local TextColored = ui.TextColored
ui.TextColored = function(...)
  seen[#seen + 1] = select(5, ...)
  return TextColored(...)
end
)lua");
        r.s->frames(3);
        r.run("seen = {}");
        r.s->frames(1);
        CHECK(r.saw("Updating leaderboard..."));
        CHECK_FALSE(r.saw("No adventurers on this board yet."));
        CHECK(r.no_errors());
    }
}

TEST_CASE("UI packs (gamepad): ending an adventure clears its abandon confirmation") {
    PackRig r("gamepad");
    r.run(R"lua(
fake.shown.AdventureRequestWnd = true
fake.activeAdventure = true
local ChildShown, ChildState, WindowText = eq.ChildShown, eq.ChildState, eq.WindowText
eq.ChildShown = function(win, child, ...)
  if win == 'AdventureRequestWnd' then return child ~= 'AdvRqst_LeaveButton' or fake.activeAdventure end
  return ChildShown(win, child, ...)
end
eq.ChildState = function(win, child, ...)
  if win == 'AdventureRequestWnd' then return child == 'AdvRqst_LeaveButton' or child == 'AdvRqst_ViewStatsButton' end
  return ChildState(win, child, ...)
end
eq.WindowText = function(win, child, ...)
  if win == 'AdventureRequestWnd' then return '' end
  return WindowText(win, child, ...)
end
)lua");
    r.s->frames(3);
    const ImVec2 leave = r.centre("##advleave");
    r.mouse_to(leave);
    r.button(ImGuiMouseButton_Left, true);
    r.button(ImGuiMouseButton_Left, false);
    CHECK(r.saw("Give up the current adventure?"));
    r.run("fake.activeAdventure = false seen = {}");
    r.s->frames(2);
    CHECK_FALSE(r.saw("Give up the current adventure?"));
    CHECK(r.no_errors());
}
