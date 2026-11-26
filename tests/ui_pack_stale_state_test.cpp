// What a shipped UI window keeps between frames must not outlive what it was for: a right click slid off a spell gem
// forgets nothing, a spell dragged from a closed book memorizes nothing, the spell set editor of one character is
// gone for the next, and the gamepad book's progress bar is the scribe gauge while a scroll is scribed. The stock
// windows' state is stood in for from Lua (hs.* replaced), and the mouse is ImGui's own input.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "lua_ui_api_mock.h"

#include "app/lua_ui.h"
#include "app/addon_requirements.h"
#include "core/config.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"
#include "ui/ui_packs.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

struct Session {
    GameDir game;
    net::GameConnection conn;
    ui::ChatView view;
    app::UiHost host;
    app::UiApi api;
    ui::lua::LuaUi lua;
    Session(const fs::path& dir, const std::string& pack)
        : game(dir / "game"), conn(game), api(host), lua(dir / pack, dir / "ui.txt") {
        host.game = &game;
        host.conn = &conn;
        host.chat = &view;
        host.in_game = host.ui_ready = true;
        lua.add_ui_api([this](lua_State* L) {
            api.register_functions(L);
            lua_pushcfunction(L, [](lua_State* S) { lua_pushboolean(S, 1); return 1; });
            lua_setfield(L, -2, "InGame");
            hailstone::test::sync_ui_api_aliases(L);
        });
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1600, 900);
            io.DeltaTime = 1.0f / 60.0f;
            hailstone::test::sync_ui_api_aliases(lua.state());
            api.begin_frame();
            ImGui::NewFrame();
            lua.frame();
            ImGui::Render();
        }
    }
};

// Every theme text ends in ui.DrawText (seen, saw); the hs calls a test watches are listed in `calls`; each
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
hs.InGame = function() return fake.inGame end
local Label = hs.Label
hs.Label = function(n, ...)
  if n == 1 then return fake.name end
  return Label(n, ...)
end
local ReplaceWindow, WindowShown = hs.ReplaceWindow, hs.WindowShown
hs.ReplaceWindow = function(win, ...)
  local r = table.pack(ReplaceWindow(win, ...))
  if fake.shown[win] ~= nil then r[1] = fake.shown[win] end
  return table.unpack(r, 1, math.max(r.n, 1))
end
hs.WindowShown = function(win)
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
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
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
hs.GemInfo = function(win, id, info)
  info.shown, info.tex, info.tooltip, info.overlay = true, nil, "", nil
  return info
end
hs.SpellGemClick = function(id, phase) calls[#calls + 1] = "gem " .. id .. " " .. tostring(phase) return true end
hs.SpellBookMemorize = function(slot, gem) calls[#calls + 1] = "memorize " .. slot .. " " .. gem return true end
)lua";

} // namespace

TEST_CASE("UI packs: blocked bases cannot start or retain a Lua presentation and keep the saved selection") {
    struct PolicyUi {
        test::ScratchDir scratch{"ui_pack_policy_lifetime"};
        fs::path builtin = scratch.path() / "ui", managed = scratch.path() / "ui-addons";
        std::unique_ptr<ImGuiContext, void (*)(ImGuiContext*)> context{ImGui::CreateContext(), ImGui::DestroyContext};
        std::unique_ptr<ui::lua::LuaUi> lua;
        std::vector<ui::UiPack> packs;
        std::vector<app::InstalledAddon> installed;
        std::string loaded;
        int builtin_frames = 0, leaf_frames = 0, inherited_frames = 0;

        PolicyUi() {
            ImGui::GetIO().IniFilename = nullptr;
            ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
            auto put = [](const fs::path& file, const char* text) {
                fs::create_directories(file.parent_path());
                std::ofstream(file) << text;
            };
            put(builtin / "modern/ui.ini", "name=Modern\n");
            put(builtin / "modern/policy.lua", "POLICY_UI='builtin' ui.window('PolicySurface',function() recordPolicyFrame(1) end)");
            put(managed / "interfaces/ui/base/ui.ini", "base=builtin:modern\n");
            put(managed / "interfaces/ui/base/inherited.lua", "ui.window('PolicyInherited',function() recordPolicyFrame(3) end)");
            put(managed / "decor/ui/leaf/ui.ini", "base=addon.interfaces.base\n");
            put(managed / "decor/ui/leaf/policy.lua", "POLICY_UI='managed' ui.window('PolicySurface',function() recordPolicyFrame(2) end)");
            for (const char* name : {"interfaces", "decor"}) {
                app::InstalledAddon addon;
                addon.name = name; addon.kind = AddonKind::Ui; addon.source = std::string("maker/") + name;
                installed.push_back(std::move(addon));
            }
        }
        ~PolicyUi() { lua.reset(); }
        void refresh(const plugin::Rules* rules) {
            packs = ui::list_ui_packs(builtin, managed, {"interfaces", "decor"});
            app::apply_ui_addon_policy(packs, rules, installed);
        }
        bool load_requested(const Config& config) {
            const auto* pack = ui::find_ui_pack(packs, config.get_string("ui.name"));
            if (!pack) return false;
            if (lua && loaded == pack->id) return true;
            lua.reset();
            loaded = pack->id;
            lua = std::make_unique<ui::lua::LuaUi>(pack->dir, scratch.path() / (loaded + "-settings.txt"), pack->base_dirs);
            lua->add_ui_api([this](lua_State* L) {
                lua_pushlightuserdata(L, this);
                lua_pushcclosure(L, [](lua_State* state) -> int {
                    auto* self = static_cast<PolicyUi*>(lua_touserdata(state, lua_upvalueindex(1)));
                    const auto kind = luaL_checkinteger(state, 1);
                    if (kind == 1) ++self->builtin_frames;
                    else if (kind == 2) ++self->leaf_frames;
                    else if (kind == 3) ++self->inherited_frames;
                    return 0;
                }, 1);
                lua_setglobal(L, "recordPolicyFrame");
            });
            return true;
        }
        void frames(int count) {
            for (int i = 0; i < count; ++i) {
                ImGui::GetIO().DisplaySize = ImVec2(800, 600);
                ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame(); lua->frame(); ImGui::Render();
            }
        }
    } rig;
    Config saved;
    const auto config_file = rig.scratch.path() / "hailstone.ini";
    saved.set("ui.name", "addon.decor.leaf");
    REQUIRE(saved.save(config_file));
    const auto saved_text = saved.to_string();
    plugin::Rules blocked;
    blocked.configured = true; blocked.addons.enabled = true;
    blocked.addons.block = {"maker/interfaces"};

    // A cached/provisional rule cannot block loading. The live rule blocks the inherited base and its leaf.
    auto provisional = blocked; provisional.provisional = true;
    rig.refresh(&provisional);
    REQUIRE(ui::find_ui_pack_exact(rig.packs, "addon.decor.leaf"));
    CHECK(ui::find_ui_pack_exact(rig.packs, "addon.decor.leaf")->available);
    rig.refresh(&blocked);
    REQUIRE(ui::find_ui_pack_exact(rig.packs, "addon.interfaces.base"));
    CHECK_FALSE(ui::find_ui_pack_exact(rig.packs, "addon.interfaces.base")->available);
    CHECK_FALSE(ui::find_ui_pack_exact(rig.packs, "addon.decor.leaf")->available);
    REQUIRE(rig.load_requested(saved));
    rig.frames(2);
    CHECK(rig.loaded == "modern");
    CHECK(rig.lua->run_for_test("", "return POLICY_UI") == "builtin");
    CHECK(rig.builtin_frames > 0); CHECK(rig.leaf_frames == 0); CHECK(rig.inherited_frames == 0);

    // Explicitly selecting the still-saved request after leaving the policy loads both managed presentations.
    rig.refresh(nullptr);
    REQUIRE(rig.load_requested(saved));
    rig.frames(2);
    CHECK(rig.lua->run_for_test("", "return POLICY_UI") == "managed");
    CHECK(rig.leaf_frames > 0); CHECK(rig.inherited_frames > 0);
    const int leaf_frames = rig.leaf_frames, inherited_frames = rig.inherited_frames, builtin_frames = rig.builtin_frames;
    rig.refresh(&blocked);
    REQUIRE(rig.load_requested(saved));
    rig.frames(4);
    CHECK(rig.loaded == "modern");
    CHECK(rig.lua->run_for_test("", "return POLICY_UI") == "builtin");
    CHECK(rig.leaf_frames == leaf_frames); CHECK(rig.inherited_frames == inherited_frames);
    CHECK(rig.builtin_frames > builtin_frames);
    CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    CHECK(saved.get_string("ui.name") == "addon.decor.leaf");
    Config reread;
    REQUIRE(reread.load(config_file));
    CHECK(reread.to_string() == saved_text);
}

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
hs.SpellBookState = function() return { gems = { 200, -1 } } end
local shown = hs.WindowShown
local rect = ui.FindWindowRect
hs.WindowShown = function(window)
  if window == 'SpellDisplayWindow' then return fake.display end
  return shown(window)
end
hs.SpellDetails = function() return 'Test spell' end
hs.WindowTitle = function() return fake.otherTitle and 'Another spell' or 'Test spell' end
ui.FindWindowRect = function(name)
  if name == '###modern.SpellDisplayWindow' then return 100, 100, 200, 100 end
  return rect(name)
end
hs.SpellGemClick = function(id, phase)
  calls[#calls + 1] = 'gem ' .. id .. ' ' .. phase
  if phase == 'rightheld' and id == 'CSPW_Spell0' then fake.display = true end
  if phase == 'rightupheld' then fake.display = false end
end
hs.ShowSpell = function(id) calls[#calls + 1] = 'show ' .. id fake.display = true end
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

TEST_CASE("UI packs (modern): Spell Gems layout options preserve existing frames and wrap the icon tiles") {
    PackRig r("modern");
    r.run(kGems);
    lua_pushlightuserdata(r.L, &r);
    lua_setglobal(r.L, "testSpellIcon");
    lua_pushcfunction(r.L, [](lua_State* L) {
        const auto* w = ImGui::GetCurrentWindowRead();
        lua_pushboolean(L, w && w->ID == ImHashStr("###modern.CastSpellWnd"));
        return 1;
    });
    lua_setglobal(r.L, "testSpellGemsWindow");
    r.run(R"lua(
ui.SetSetting("hud.pos.CastSpellWnd", "700,200")
local button = ui.InvisibleButton
ui.InvisibleButton = function(id, ...)
  local previous = rects[id]
  local pressed = button(id, ...)
  if not testSpellGemsWindow() then rects[id] = previous end
  return pressed
end
local gemInfo = hs.GemInfo
hs.GemInfo = function(win, id, info)
  info = gemInfo(win, id, info)
  if id == "CSPW_Spell0" then info.tex, info.tooltip = testSpellIcon, "Spirit of Wolf" end
  return info
end
local clickChild = hs.ClickChild
hs.ClickChild = function(win, id, phase)
  if win == "CastSpellWnd" then
    calls[#calls + 1] = "book " .. id .. " " .. phase
    return true
  end
  return clickChild(win, id, phase)
end
menuRows = {}
local menuItem = ui.MenuItem
ui.MenuItem = function(label, selected, enabled, ...)
  menuRows[label] = selected and true or false
  if pick == label and enabled ~= false then
    pick = nil
    ui.CloseCurrentPopup()
    return true
  end
  return menuItem(label, selected, enabled, ...)
end
local slider = ui.SliderInt
ui.SliderInt = function(label, ...)
  if label == "Icon size" and label == pick then
    pick = nil
    ui.CloseCurrentPopup()
    return 28, true
  end
  return slider(label, ...)
end
local drawRect = ui.DrawRectFilled
ui.DrawRectFilled = function(x0, y0, x1, y1, ...)
  if testSpellGemsWindow() then
    local x, y = ui.GetWindowPos()
    local w, h = ui.GetWindowSize()
    if x0 == x and y0 == y and x1 == x + w and y1 == y + h then gemPanel = true end
  end
  return drawRect(x0, y0, x1, y1, ...)
end
)lua");
    auto refresh = [&] {
        r.run("seen, rects, gemPanel = {}, {}, false");
        r.s->frames(3);
    };
    auto choose = [&](const char* label) {
        r.run((std::string("pick = '") + label + "'").c_str());
        r.mouse_to(r.centre("##CSPW_Spell0"));
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
        r.button(ImGuiMouseButton_Right, true, 1);
        CHECK(r.truth("pick == nil"));
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
        r.button(ImGuiMouseButton_Right, false);
        ImGui::ClosePopupsExceptModals();
        refresh();
    };
    auto click = [&](ImVec2 p) {
        r.mouse_to(p);
        r.button(ImGuiMouseButton_Left, true);
        r.button(ImGuiMouseButton_Left, false);
    };
    refresh();
    const auto* window = ImGui::FindWindowByName("###modern.CastSpellWnd");
    REQUIRE(window);
    const ImVec2 original = window->Size;
    CHECK(original.x == 596);
    CHECK(original.y == 56);
    CHECK(r.truth("gemPanel and rects['##resize'] == nil"));
    CHECK_FALSE(r.saw("Spirit of Wolf"));
    auto first = r.centre("##CSPW_Spell0");
    auto book = r.centre("##book");
    CHECK(book.x == first.x + 12 * 45);
    CHECK(book.y == first.y);

    // An old saved Vertical still uses a single column of unchanged size.
    r.run("ui.SetSetting('hud.vertical.CastSpellWnd', '1')");
    refresh();
    CHECK(window->Size.x == original.y);
    CHECK(window->Size.y == original.x);
    first = r.centre("##CSPW_Spell0");
    book = r.centre("##book");
    CHECK(book.x == first.x);
    CHECK(book.y == first.y + 12 * 45);
    choose("Vertical");
    CHECK(r.truth("menuRows['Show spell names'] == false and menuRows['Show window background'] == true"
                  " and menuRows['Show resize bar'] == false"));

    choose("Show window background");
    CHECK_FALSE(r.truth("gemPanel"));
    CHECK(window->Size.x == original.x);
    CHECK(window->Size.y == original.y);
    choose("Show window background");
    CHECK(r.truth("gemPanel"));

    choose("Show spell names");
    CHECK(r.saw("Spirit of Wolf"));
    CHECK(r.truth("rects['##CSPW_Spell0'][3] > 40"));
    r.run("calls = {}");
    click(r.centre("##CSPW_Spell0"));
    CHECK(r.called("gem CSPW_Spell0 left"));
    choose("Show spell names");
    CHECK_FALSE(r.saw("Spirit of Wolf"));

    choose("Show resize bar");
    REQUIRE(r.truth("rects['##resize'] ~= nil"));
    const ImVec2 grip = r.centre("##resize");
    r.mouse_to(grip);
    r.button(ImGuiMouseButton_Left, true);
    r.mouse_to(ImVec2(grip.x - 10 * 45, grip.y));
    // The newly taller grid should settle even with the corner held at the same mouse position.
    for (int i = 0; i < 3; ++i) {
        CHECK(r.truth("ui.GetSetting('hud.gemWrap.CastSpellWnd') == '3'"));
        r.s->frames(1);
    }
    r.button(ImGuiMouseButton_Left, false);
    first = r.centre("##CSPW_Spell0");
    const auto lastInFirstRow = r.centre("##CSPW_Spell2");
    const auto firstInSecondRow = r.centre("##CSPW_Spell3");
    book = r.centre("##book");
    CHECK(lastInFirstRow.x == first.x + 2 * 45);
    CHECK(lastInFirstRow.y == first.y);
    CHECK(firstInSecondRow.x == first.x);
    CHECK(firstInSecondRow.y == first.y + 45);
    CHECK(book.x == first.x);
    CHECK(book.y == first.y + 4 * 45);
    r.run("calls = {}");
    click(firstInSecondRow);
    click(book);
    CHECK(r.called("gem CSPW_Spell3 left"));
    CHECK(r.called("book CSPW_SpellBookH left"));

    choose("Vertical");
    first = r.centre("##CSPW_Spell0");
    const auto lastInFirstColumn = r.centre("##CSPW_Spell2");
    const auto firstInSecondColumn = r.centre("##CSPW_Spell3");
    CHECK(lastInFirstColumn.x == first.x);
    CHECK(lastInFirstColumn.y == first.y + 2 * 45);
    CHECK(firstInSecondColumn.x == first.x + 45);
    CHECK(firstInSecondColumn.y == first.y);
    const ImVec2 bottomGrip = r.centre("##resizebottom");
    r.mouse_to(bottomGrip);
    r.button(ImGuiMouseButton_Left, true);
    r.mouse_to(ImVec2(bottomGrip.x, bottomGrip.y + 45));
    r.button(ImGuiMouseButton_Left, false);
    CHECK(r.truth("ui.GetSetting('hud.gemWrap.CastSpellWnd') == '4'"));
    CHECK(r.centre("##CSPW_Spell3").x == r.centre("##CSPW_Spell0").x);
    CHECK(r.centre("##CSPW_Spell4").x == r.centre("##CSPW_Spell0").x + 45);

    choose("Vertical");
    const ImVec2 wrapped = window->Size;
    choose("Show resize bar");
    CHECK_FALSE(r.truth("rects['##resize'] ~= nil"));
    choose("Show spell names");
    CHECK(r.saw("Spirit of Wolf"));
    choose("Show spell names");
    CHECK(window->Size.x <= wrapped.x);
    CHECK(r.centre("##CSPW_Spell4").y == r.centre("##CSPW_Spell0").y + 45);
    choose("Icon size");
    CHECK(r.truth("rects['##CSPW_Spell0'][3] == 28"));
    choose("Reset size");
    CHECK(window->Size.x == original.x);
    CHECK(window->Size.y == original.y);

    // At large configured scales the viewport must still contain every tile and the grip.
    r.run(R"lua(
ui.SetSetting("hud.scale", "2.5")
ui.SetSetting("hud.scale.CastSpellWnd", "1.6")
ui.SetSetting("hud.icon.CastSpellWnd", "64")
ui.SetSetting("hud.gemResize.CastSpellWnd", "1")
ui.SetSetting("hud.gemNames.CastSpellWnd", "1")
)lua");
    refresh();
    CHECK(r.truth(R"lua((function()
  local function fits(r)
    return r and r[1] >= 0 and r[2] >= 0 and r[1] + r[3] <= 1600 and r[2] + r[4] <= 900
  end
  for i = 0, 11 do if not fits(rects["##CSPW_Spell" .. i]) then return false end end
  return fits(rects["##book"]) and fits(rects["##resize"])
end)())lua"));
    CHECK(r.no_errors());
}

// The spell sets the stand-in game has: one in use (Heals), nine free.
constexpr const char* kSets = R"lua(
hs.SpellSets = function()
  local t = { { name = "Heals", inUse = true, spells = { 200, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 } } }
  for n = 2, 10 do t[n] = { name = "", inUse = false, spells = {} } end
  return t
end
hs.EditSpellSet = function(n, ...) calls[#calls + 1] = "edit " .. n return n end
hs.SaveSpellSet = function(...) calls[#calls + 1] = "save" return 1 end
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

TEST_CASE("UI packs (modern): the Lua social editor is the character's: another character, or a camp, closes it") {
    PackRig r("modern");
    // social 6 asked for once, its code editor's calls listed, and nothing saved behind the test's back
    r.run(R"lua(
fake.req = 5
hs.LuaSocialEditRequest = function() local n = fake.req fake.req = nil return n end
hs.LuaSocial = function(n) return { name = "Old", color = 0, press = "mq.cmd('/sit')", face = "", lua = true } end
hs.SetLuaSocial = function(n) calls[#calls + 1] = "set " .. n end
local CodeEditor = ui.CodeEditor
ui.CodeEditor = function(...) calls[#calls + 1] = "code" return CodeEditor(...) end
)lua");
    r.s->frames(3);
    r.run("calls = {}");
    r.s->frames(1);
    CHECK(r.called("code"));
    r.run("fake.name = 'Bravo'");
    r.s->frames(2);
    r.run("calls = {}");
    r.s->frames(1);
    CHECK_FALSE(r.called("code"));
    // opened again, then camped and back as the same name: gone too
    r.run("fake.req = 5");
    r.s->frames(3);
    r.run("calls = {}");
    r.s->frames(1);
    CHECK(r.called("code"));
    r.run("fake.inGame = false");
    r.s->frames(2);
    r.run("fake.inGame = true calls = {}");
    r.s->frames(2);
    CHECK_FALSE(r.called("code"));
    CHECK_FALSE(r.called("set "));
    CHECK(r.no_errors());
}

TEST_CASE("UI packs (gamepad): the spell book's bar is the scribe gauge while a scroll is scribed") {
    PackRig r("gamepad");
    r.run(R"lua(
fake.shown.SpellBookWnd = true
fake.state = { scribing = { slot = 3 }, gems = {} }
hs.SpellBookState = function() return fake.state end
local ChildShown, Gauge = hs.ChildShown, hs.Gauge
hs.ChildShown = function(win, child, ...)
  if win == "SpellBookWnd" and child == "SBW_Memorize_Gauge" then return true end
  return ChildShown(win, child, ...)
end
hs.Gauge = function(n, ...)
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
local ChildState, ChildShown, WindowText, ComboInfo = hs.ChildState, hs.ChildShown, hs.WindowText, hs.ComboInfo
hs.ChildState = function(win, child, ...)
  if win == 'AdventureRequestWnd' then return child == 'AdvRqst_ViewStatsButton' end
  return ChildState(win, child, ...)
end
hs.ChildShown = function(win, child, ...)
  if win == 'AdventureRequestWnd' then return child ~= 'AdvRqst_LeaveButton' end
  return ChildShown(win, child, ...)
end
hs.WindowText = function(win, child, ...)
  if win == 'AdventureRequestWnd' then
    if child == 'AdvRqst_NPCText' then return 'No adventure is assigned.' end
    return ''
  end
  if win == 'AdventureStatsWnd' and child == 'AdvStats_AvailableValue' then return '901' end
  return WindowText(win, child, ...)
end
hs.ComboInfo = function(win, child, ...)
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
hs.ClickChild = function(win, child) calls[#calls + 1] = 'click ' .. win .. '.' .. child end
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
local ListInfo, ListRow = hs.ListInfo, hs.ListRow
local records = {
  { 'Deepest Guk', '5', '2', '1', '66.7%', '8', '88' },
  { 'Totals', '7', '37', '9', '80.4%', '12', '901' },
}
hs.ListInfo = function(win, child, ...)
  if win == 'AdventureStatsWnd' and child == 'AdvStats_ThemeList' then
    return #records, 7, -1, { 'Theme', 'Rank', 'Successes', 'Failures', 'Percent', 'Rank (%)', 'Points' },
      { 150, 60, 70, 70, 60, 60, 60 }
  end
  return ListInfo(win, child, ...)
end
hs.ListRow = function(win, child, row, ...)
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
local WindowText = hs.WindowText
hs.WindowText = function(win, child, ...)
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

TEST_CASE("UI packs (gamepad): ending or replacing an adventure clears its leave confirmation") {
    PackRig r("gamepad");
    r.run(R"lua(
fake.shown.AdventureRequestWnd = true
fake.adventureToken = '1:1'
local ChildShown, ChildState, WindowText = hs.ChildShown, hs.ChildState, hs.WindowText
hs.ChildShown = function(win, child, ...)
  if win == 'AdventureRequestWnd' then return child ~= 'AdvRqst_LeaveButton' or fake.adventureToken ~= '' end
  return ChildShown(win, child, ...)
end
hs.ChildState = function(win, child, ...)
  if win == 'AdventureRequestWnd' then
    return child == 'AdvRqst_ViewStatsButton' or (child == 'AdvRqst_LeaveButton' and fake.adventureToken ~= '')
  end
  return ChildState(win, child, ...)
end
hs.WindowText = function(win, child, ...)
  if win == 'AdventureRequestWnd' then return child == 'AdvRqst_LeaveToken' and fake.adventureToken or '' end
  return WindowText(win, child, ...)
end
)lua");
    r.s->frames(3);
    const ImVec2 leave = r.centre("##advleave");
    r.mouse_to(leave);
    r.button(ImGuiMouseButton_Left, true);
    r.button(ImGuiMouseButton_Left, false);
    CHECK(r.saw("Give up the current adventure?"));
    SUBCASE("the assignment ends") { r.run("fake.adventureToken = '' seen = {}"); }
    SUBCASE("a new assignment replaces it") { r.run("fake.adventureToken = '1:2' seen = {}"); }
    r.s->frames(2);
    CHECK_FALSE(r.saw("Give up the current adventure?"));
    CHECK(r.no_errors());
}

namespace {

// A stand-in for a stock window's lists and buttons: fake.lists[win][list] = { rows = { {texts}, ... }, sel = row
// (0-based, -1 none) }, one column a text with no heads; every child shown and enabled unless fake.off[id]; clicks are listed in `calls`. forced[id]
// makes the next ui.Button / ui.InvisibleButton of that id report a click. ui.Text is seen as DrawText is.
constexpr const char* kStockWindows = R"lua(
forced = {}
fake.lists, fake.off = {}, {}
local function list(win, id) return fake.lists[win] and fake.lists[win][id] end
local ListInfo, ListRow, ListClick = hs.ListInfo, hs.ListRow, hs.ListClick
hs.ListInfo = function(win, id, ...)
  local l = list(win, id)
  if l then
    local cols = #(l.rows[1] or { "" })
    local labels, widths = {}, {}
    for c = 1, cols do labels[c], widths[c] = "", 80 end
    return #l.rows, cols, l.sel, labels, widths, false
  end
  if fake.lists[win] then return 0, 1, -1, { "" }, { 80 }, false end
  return ListInfo(win, id, ...)
end
hs.ListRow = function(win, id, row, ...)
  local l = list(win, id)
  if l then return l.rows[row + 1] end
  if fake.lists[win] then return nil end
  return ListRow(win, id, row, ...)
end
hs.ListClick = function(win, id, row, button, ...)
  local l = list(win, id)
  if l then
    calls[#calls + 1] = "listclick " .. id .. " " .. row
    if button == "left" and l.rows[row + 1] then l.sel = row end
    return true
  end
  return ListClick(win, id, row, button, ...)
end
local ChildShown, ChildState, ClickChild = hs.ChildShown, hs.ChildState, hs.ClickChild
hs.ChildShown = function(win, id, ...)
  if fake.lists[win] then return not fake.off[id] end
  return ChildShown(win, id, ...)
end
hs.ChildState = function(win, id, ...)
  if fake.lists[win] then return not fake.off[id], false end
  return ChildState(win, id, ...)
end
hs.ClickChild = function(win, id, button, ...)
  if fake.lists[win] then calls[#calls + 1] = "click " .. id return true end
  return ClickChild(win, id, button, ...)
end
local Button, InvisibleButton = ui.Button, ui.InvisibleButton
ui.Button = function(id, ...)
  local hit = Button(id, ...)
  if forced[id] then forced[id] = nil return true end
  return hit
end
ui.InvisibleButton = function(id, ...)
  local hit = InvisibleButton(id, ...)
  if forced[id] then forced[id] = nil return true end
  return hit
end
local Text = ui.Text
ui.Text = function(s, ...)
  seen[#seen + 1] = s
  return Text(s, ...)
end
)lua";

} // namespace

TEST_CASE("UI packs (modern): the guild's member actions ask first, naming the member, and Yes acts on that member") {
    PackRig r("modern");
    r.run(kStockWindows);
    r.run(R"lua(
fake.shown.GuildManagementWnd = true
fake.lists.GuildManagementWnd = { MemberList = { sel = 0, rows = {
  { "Alpha", "50", "Warrior", "Member", "", "", "Zone", "" },
  { "Beta", "40", "Cleric", "Member", "", "", "Zone", "" } } } }
)lua");
    r.s->frames(3);
    struct Action { const char* label; const char* child; const char* question; };
    for (const Action& a : {Action{"Promote", "PromoteButton", "Promote Alpha?"},
                            Action{"Demote", "DemoteButton", "Demote Alpha?"},
                            Action{"Make Leader", "MakeLeaderButton", "Make Alpha the guild leader?"},
                            Action{"Remove", "RemoveButton", "Remove Alpha from the guild?"}}) {
        CAPTURE(a.child);
        const std::string button = std::string(a.label) + "##" + a.child;
        r.run(("calls = {} seen = {} fake.lists.GuildManagementWnd.MemberList.sel = 0 forced['" + button + "'] = true")
                  .c_str());
        r.s->frames(2);
        CHECK(r.saw(a.question));
        CHECK_FALSE(r.called(std::string("click ") + a.child)); // one click asks; nothing is sent
        r.run(("forced['" + std::string(a.label) + "##askyes" + a.child + "'] = true").c_str());
        r.s->frames(2);
        CHECK(r.called(std::string("click ") + a.child));
        r.run("seen = {}");
        r.s->frames(1);
        CHECK_FALSE(r.saw(a.question)); // answered: put away
    }
    // Another member selected while it is asked: the question goes, its Yes is never drawn.
    r.run("calls = {} seen = {} forced['Remove##RemoveButton'] = true");
    r.s->frames(2);
    CHECK(r.saw("Remove Alpha from the guild?"));
    r.run("fake.lists.GuildManagementWnd.MemberList.sel = 1 seen = {}");
    r.s->frames(2);
    r.run("seen = {}");
    r.s->frames(1);
    CHECK_FALSE(r.saw("Remove Alpha from the guild?"));
    CHECK_FALSE(r.saw("Remove Beta from the guild?"));
    CHECK_FALSE(r.called("click RemoveButton"));
    CHECK(r.no_errors());
}

TEST_CASE("UI packs (modern): the raid's Leave and Make Leader ask first") {
    PackRig r("modern");
    r.run(kStockWindows);
    r.run(R"lua(
fake.shown.RaidWindow = true
hs.ContextMenus = function() return false end -- (the direct member buttons, Make Leader among them)
fake.lists.RaidWindow = { RAID_PlayerList = { sel = 1, rows = {
  { "1", "Alpha", "60", "Warrior", "", "Raid Leader" },
  { "1", "Beta", "60", "Cleric", "", "" } } } }
)lua");
    r.s->frames(3);
    r.run("calls = {} seen = {} forced['Leave##RAID_DisbandButton'] = true");
    r.s->frames(2);
    CHECK(r.saw("Leave the raid?"));
    CHECK_FALSE(r.called("click RAID_DisbandButton"));
    r.run("forced['Leave##askyesRAID_DisbandButton'] = true");
    r.s->frames(2);
    CHECK(r.called("click RAID_DisbandButton"));
    r.run("calls = {} seen = {} forced['Make Leader##RAID_MakeLeaderButton'] = true");
    r.s->frames(2);
    CHECK(r.saw("Make Beta the raid leader?"));
    CHECK_FALSE(r.called("click RAID_MakeLeaderButton"));
    r.run("forced['Cancel##asknoRAID_MakeLeaderButton'] = true");
    r.s->frames(2);
    CHECK_FALSE(r.called("click RAID_MakeLeaderButton"));
    r.run("seen = {}");
    r.s->frames(1);
    CHECK_FALSE(r.saw("Make Beta the raid leader?"));
    CHECK(r.no_errors());
}

TEST_CASE("UI packs (gamepad): a friend's Remove removes that friend, or nobody once the name is gone") {
    PackRig r("gamepad");
    r.run(kStockWindows);
    r.run(R"lua(
fake.shown.FriendsWindow = true
fake.lists.FriendsWindow = { FriendsList = { sel = 0, rows = { { "Alpha" }, { "Beta" } } }, IgnoreList = { sel = -1, rows = {} } }
)lua");
    r.s->frames(30);
    r.run("calls = {} forced['##remove'] = true");
    r.s->frames(2);
    CHECK(r.saw("Remove Alpha from your friends list?"));
    SUBCASE("the name has gone from the list") {
        r.run("fake.lists.FriendsWindow.FriendsList = { sel = 0, rows = { { 'Beta' } } } forced['##confirmyes'] = true");
        r.s->frames(2);
        CHECK_FALSE(r.called("click DeleteButton"));
        CHECK(r.saw("Alpha is no longer on the list."));
    }
    SUBCASE("the list moved: the name is selected again, then removed") {
        r.run("fake.lists.FriendsWindow.FriendsList = { sel = 0, rows = { { 'Beta' }, { 'Alpha' } } } "
              "forced['##confirmyes'] = true");
        r.s->frames(2);
        CHECK(r.called("listclick FriendsList 1"));
        CHECK(r.called("click DeleteButton"));
        CHECK(r.truth("fake.lists.FriendsWindow.FriendsList.sel == 1"));
    }
    CHECK(r.no_errors());
}

TEST_CASE("UI packs (gamepad): Who's picked player follows a new reply, or the pick goes") {
    PackRig r("gamepad");
    r.run(kStockWindows);
    r.run(R"lua(
submitted = {}
fake.who = { 1, { { name = "Alpha", line = "[50 Warrior] Alpha (Human)" }, { name = "Beta", line = "[40 Cleric] Beta (Elf)" } } }
hs.WhoResults = function() return fake.who[1], fake.who[2] end
hs.ChatSubmit = function(slot, line) submitted[#submitted + 1] = line end
GPSTATE.whoOpen = true
)lua");
    r.s->frames(30);
    r.run("forced['##row'] = true"); // the first row: Alpha
    r.s->frames(2);
    CHECK(r.saw("ALPHA"));
    // a reply from elsewhere (the Friends window's Who, a typed /who) with Alpha second
    r.run("fake.who = { 2, { { name = 'Gamma', line = '[10 Monk] Gamma' }, { name = 'Alpha', line = '[50 Warrior] Alpha' } } }");
    r.s->frames(20); // (the reply is read again every 0.25 s)
    r.run("forced['##invite'] = true");
    r.s->frames(2);
    CHECK(r.truth("submitted[#submitted] == '/invite Alpha'"));
    // a reply without Alpha: nothing is picked
    r.run("fake.who = { 3, { { name = 'Gamma', line = '[10 Monk] Gamma' } } }");
    r.s->frames(20);
    r.run("seen = {}");
    r.s->frames(1);
    CHECK(r.saw("SELECT A PLAYER ON THE LEFT"));
    CHECK(r.no_errors());
}

TEST_CASE("UI packs (gamepad): a merchant's Buy asked on its last frame does not press the next merchant's") {
    PackRig r("gamepad");
    r.run(kStockWindows);
    r.run(R"lua(
fake.shown.MerchantWnd = true
fake.lists.MerchantWnd = { ItemList = { sel = -1, rows = { { "", "Bread", "", "0", "0", "1", "0", "1" } } } }
hs.Parcels = function() return nil end
)lua");
    r.s->frames(30);
    // A on a row selects it, and presses Buy the frame after
    r.run("calls = {} forced['##row'] = true");
    r.s->frames(2);
    CHECK(r.called("click MW_Buy_Button"));
    // the window closes on the frame after the row's A: no Buy, then or when a merchant opens again
    r.run("calls = {} forced['##row'] = true");
    r.s->frames(1);
    r.run("fake.shown.MerchantWnd = false");
    r.s->frames(1);
    r.run("fake.shown.MerchantWnd = true");
    r.s->frames(3);
    CHECK_FALSE(r.called("click MW_Buy_Button"));
    CHECK(r.no_errors());
}

TEST_CASE("UI packs: the fellowship's asking buttons go straight to the stock window, which asks once") {
    // The stock FellowshipWnd model asks (app/lua_ui_fellowship.cpp, the confirmation dialog) for Make Leader, Remove,
    // Leave, End Fellowship and Destroy Camp; a question of the pack's own on top would ask twice.
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(std::string(pack));
        const bool modern = std::string_view(pack) == "modern";
        PackRig r(pack);
        r.run(kStockWindows);
        r.run(R"lua(
fake.shown.FellowshipWnd = true
fake.lists.FellowshipWnd = { MemberList = { sel = 1, rows = {
  { "Alpha", "50", "Warrior", "", "Zone", "", "On" },
  { "Beta", "40", "Cleric", "", "Zone", "", "On" } } } }
)lua");
        r.s->frames(30);
        const std::pair<const char*, const char*> buttons[] = {{"MakeLeaderButton", "Make Leader"},
                                                               {"RemoveMemberButton", "Remove"},
                                                               {"LeaveFellowshipButton", "Leave"},
                                                               {"EndFellowshipButton", "End Fellowship"}};
        for (const auto& [id, label] : buttons) {
            CAPTURE(std::string(id));
            const std::string forced = modern ? std::string(label) + "##" + id : std::string("##FellowshipWnd") + id;
            r.run(("calls = {} seen = {} forced['" + forced + "'] = true").c_str());
            r.s->frames(2);
            CHECK(r.called(std::string("click ") + id));
            CHECK_FALSE(r.saw("fellowship leader?"));
        }
        CHECK(r.no_errors());
    }
}
