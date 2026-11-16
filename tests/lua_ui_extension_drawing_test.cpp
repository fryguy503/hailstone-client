#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "plugin/policy.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"

using namespace hailstone;
namespace fs = std::filesystem;
namespace {
struct DrawingUi {
    test::ScratchDir scratch{"ui_extension_drawing"};
    ImGuiContext* imgui = ImGui::CreateContext();
    app::UiHost host;
    std::unique_ptr<app::UiApi> api;
    std::unique_ptr<ui::lua::LuaUi> lua;
    int checked = 0, good = 0;
    bool implicit_written = false;
    ImGuiWindow* parent = nullptr;
    ImGuiTable* table = nullptr;
    ImGuiTabBar* tabs = nullptr;
    int windows = 0, colors = 0, vars = 0, fonts = 0, ids = 0, groups = 0, trees = 0, wraps = 0, clips = 0, disabled = 0;
    bool scope_failure = false;
    ImGuiLastItemData last;
    int tabItem = 0;

    DrawingUi() {
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigErrorRecoveryEnableAssert = false;
        ImGui::GetIO().ConfigErrorRecoveryEnableTooltip = false;
        unsigned char* pixels = nullptr; int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        fs::create_directories(scratch.path()/"ui"); fs::create_directories(scratch.path()/"owner");
        host.plugin_caps = [](std::string_view) { return plugin::kCapRead | plugin::kCapUi; };
        api = std::make_unique<app::UiApi>(host);
        lua = std::make_unique<ui::lua::LuaUi>(scratch.path()/"ui", scratch.path()/"settings.txt");
        lua->add_ui_api([&](lua_State* L) { api->register_functions(L); });
        lua->set_folders({{"plugin:display",scratch.path()/"owner"}});
        lua->set_caller_scope([&](std::string_view tag) -> std::shared_ptr<void> {
            if (scope_failure) throw std::runtime_error("scope setup failure");
            return std::make_shared<plugin::CallerScope>(host.plugin_caps(tag), tag);
        });
        lua_State* L = lua->state(); lua_getglobal(L,"hs");
        for (const auto& item : {std::pair{"SetupParent",0}, {"CheckParent",1}, {"FinishParent",2}, {"GoodSection",3},
                {"ForceCombo",4}, {"KindCounts",5}}) {
            lua_pushlightuserdata(L,this); lua_pushinteger(L,item.second);
            lua_pushcclosure(L, [](lua_State* state) -> int {
                auto& fixture=*static_cast<DrawingUi*>(lua_touserdata(state,lua_upvalueindex(1)));
                switch (lua_tointeger(state,lua_upvalueindex(2))) {
                case 0: fixture.setup(); break;
                case 1: fixture.check(); break;
                case 2: fixture.finish(); break;
                case 3: ++fixture.good; ImGui::TextUnformatted("A later extension still draws"); break;
                case 4: ImGui::OpenPopupEx(ImHashStr("##ComboPopup",0,ImGui::GetID("owned combo")),ImGuiPopupFlags_None);break;
                case 5: lua_pushinteger(state,GImGui->CurrentWindowStack.Size);lua_pushinteger(state,GImGui->BeginComboDepth);return 2;
                }
                return 0;
            },2); lua_setfield(L,-2,item.first);
        }
        lua_pop(L,1);
        lua_getglobal(L,"hs");lua_pushinteger(L,ImGuiTabItemFlags_NoPushId);lua_setfield(L,-2,"NoPushTab");lua_pop(L,1);
    }
    ~DrawingUi() { lua.reset();api.reset();ImGui::DestroyContext(imgui); }
    void put(const char* folder,const char* text) { std::ofstream(scratch.path()/folder/"entry.lua")<<text; }
    void setup() {
        parent=ImGui::GetCurrentWindow();
        ImGui::PushFont(GImGui->Font);
        ImGui::TreePush("native member tree");
        REQUIRE(ImGui::BeginTable("native roster",2));ImGui::TableNextRow();ImGui::TableNextColumn();
        REQUIRE(ImGui::BeginTabBar("native member tabs"));REQUIRE(ImGui::BeginTabItem("Native actions"));
        table=GImGui->CurrentTable;tabs=GImGui->CurrentTabBar;
        windows=GImGui->CurrentWindowStack.Size;colors=GImGui->ColorStack.Size;vars=GImGui->StyleVarStack.Size;
        fonts=GImGui->FontStack.Size;ids=parent->IDStack.Size;groups=GImGui->GroupStack.Size;
        trees=parent->DC.TreeDepth;wraps=parent->DC.TextWrapPosStack.Size;clips=parent->DrawList->_ClipRectStack.Size;
        disabled=GImGui->DisabledStackSize;
        last=GImGui->LastItemData;tabItem=tabs->LastTabItemIdx;
        ImGui::SetWindowFontScale(1.25f);
        ImGui::SetNextWindowSize(ImVec2(311,277));ImGui::SetNextItemWidth(157);
    }
    void check() {
        ++checked;
        CHECK(GImGui->CurrentWindow==parent);CHECK(GImGui->CurrentWindowStack.Size==windows);
        CHECK(GImGui->CurrentTable==table);CHECK(GImGui->CurrentTabBar==tabs);
        CHECK(tabs->LastTabItemIdx==tabItem);CHECK(GImGui->LastItemData.ID==last.ID);
        CHECK(GImGui->LastItemData.Rect.Min.x==last.Rect.Min.x);CHECK(GImGui->LastItemData.Rect.Max.y==last.Rect.Max.y);
        CHECK(GImGui->ColorStack.Size==colors);CHECK(GImGui->StyleVarStack.Size==vars);CHECK(GImGui->FontStack.Size==fonts);
        CHECK(parent->IDStack.Size==ids);CHECK(GImGui->GroupStack.Size==groups);CHECK(parent->DC.TreeDepth==trees);
        CHECK(parent->DC.TextWrapPosStack.Size==wraps);CHECK(parent->DrawList->_ClipRectStack.Size==clips);
        CHECK(GImGui->DisabledStackSize==disabled);CHECK(parent->FontWindowScale==doctest::Approx(1.25));
        CHECK((GImGui->NextWindowData.HasFlags&ImGuiNextWindowDataFlags_HasSize)!=0);
        CHECK(GImGui->NextWindowData.SizeVal.x==doctest::Approx(311));
        CHECK((GImGui->NextItemData.HasFlags&ImGuiNextItemDataFlags_HasWidth)!=0);
        CHECK(GImGui->NextItemData.Width==doctest::Approx(157));
    }
    void finish() {
        GImGui->NextWindowData.ClearFlags();GImGui->NextItemData.ClearFlags();
        ImGui::EndTabItem();ImGui::EndTabBar();ImGui::EndTable();ImGui::TreePop();ImGui::PopFont();
        ImGui::TextUnformatted("The stock Raid parent remains usable");
    }
    void frame() {
        ImGui::GetIO().DisplaySize=ImVec2(900,700);ImGui::GetIO().DeltaTime=1.0f/60.0f;
        ImGui::NewFrame();api->begin_frame();lua->frame();
        const auto* implicit=ImGui::FindWindowByName("Debug##Default");
        implicit_written=implicit&&implicit->WriteAccessed;
        ImGui::Render();
    }
};
}

TEST_CASE("UI extensions drawing: malformed sections preserve stock parent containers and pending state") {
    DrawingUi f;
    f.put("ui",R"lua(
ui.window('Stock Raid',function()
 ui.Begin('Stock Raid parent');ui.BeginChild('member actions',650,550)
 ui.PushStyleColor(ui.Col.Text,1,1,1,1);ui.PushStyleVar(ui.StyleVar.Alpha,0.9)
 ui.BeginDisabled(true);ui.PushID('native');ui.BeginGroup();ui.PushTextWrapPos(300)
 ui.PushClipRect(0,0,900,700,true);hs.SetupParent()
 hs.UI.extensions.DrawSection('RaidWindow','member_actions','before',{selection={name='Offzone'}})
 hs.CheckParent()
 hs.UI.extensions.DrawSection('RaidWindow','member_actions','after',{selection={name='Offzone'}})
 hs.CheckParent();hs.FinishParent()
 ui.PopClipRect();ui.PopTextWrapPos();ui.EndGroup();ui.PopID();ui.EndDisabled()
 ui.PopStyleVar();ui.PopStyleColor();ui.EndChild();ui.End()
end)
)lua");
    f.put("owner",R"lua(
local E=hs.UI.extensions
E.RegisterSection('RaidWindow','member_actions','before','malformed',function()
 ui.End();ui.EndChild();ui.EndTable();ui.EndTabItem();ui.EndTabBar();ui.EndGroup();ui.TreePop();ui.PopID()
 ui.PopFont();ui.PopStyleColor(100);ui.PopStyleVar(100);ui.EndDisabled();ui.PopTextWrapPos();ui.PopClipRect()
 ui.EndPopup();ui.EndMenu();ui.EndCombo();ui.EndTooltip();ui.CloseCurrentPopup()
 assert(ui.BeginTabItem('not the callback tab',hs.NoPushTab)==false)
 ui.SetWindowFontScale(3);ui.SetNextWindowPos(90,80);ui.SetNextWindowSize(77,66);ui.SetNextWindowFocus()
 ui.SetNextWindowCollapsed(true);ui.SetNextItemWidth(999);ui.SetNextItemAllowOverlap()
 ui.Begin('forgotten extension window');ui.BeginChild('forgotten extension child',100,50)
 ui.PushStyleColor(ui.Col.Text,1,0,0,1);ui.PushStyleVar(ui.StyleVar.Alpha,0.2);ui.PushClipRect(1,1,9,9,true)
 error('isolated display failure')
end)
)lua");
    // A failing source retires that source's other callbacks; a different source must remain usable.
    std::ofstream(f.scratch.path()/"owner"/"legitimate.lua") << R"lua(
hs.UI.extensions.RegisterSection('RaidWindow','member_actions','after','legitimate',function() hs.GoodSection() end)
)lua";
    f.frame();CHECK(f.checked==2);CHECK(f.good==1);
    CHECK(f.lua->run_for_test("", "return tostring(hs.UI.extensions.DrawSection('RaidWindow','member_actions','before',{}))")=="0");
}

TEST_CASE("UI extensions drawing: ordinary presenters leave the implicit default window inactive") {
    DrawingUi f;
    f.put("ui",R"lua(
ui.window('Ordinary presenter',function()
 ui.SetNextWindowPos(500,450,ui.Cond.Always);ui.SetNextWindowSize(180,120,ui.Cond.Always)
 ui.Begin('Ordinary presenter');ui.Text('Only this window is drawn');ui.End()
end)
)lua");
    auto& io=ImGui::GetIO();
    io.AddMousePosEvent(100,100);
    f.frame();f.frame();f.frame();
    const auto* implicit=ImGui::FindWindowByName("Debug##Default");
    REQUIRE(implicit);
    CHECK_FALSE(implicit->Active);
    CHECK_FALSE(f.implicit_written);
    CHECK(GImGui->HoveredWindow==nullptr);
    CHECK_FALSE(io.WantCaptureMouse);
}

TEST_CASE("UI extensions drawing: hook cleanup preserves whether a callback drew in the implicit window") {
    for (const bool draw : {false,true}) {
        CAPTURE(draw);
        DrawingUi f;
        const std::string script=R"lua(
hookCalls=0
assert(hs.UI.hooks.Register('character.Name','before',function()
 hookCalls=hookCalls+1
)lua"+std::string(draw?"ui.Text('An intentional callback drawing')\n":"")+R"lua(
end))
ui.window('Ordinary presenter',function()
 hs.UI.character.Name()
 ui.SetNextWindowPos(500,450,ui.Cond.Always);ui.SetNextWindowSize(180,120,ui.Cond.Always)
 ui.Begin('Ordinary presenter');ui.Text('Only this window is drawn by the presenter');ui.End()
end)
)lua";
        f.put("ui",script.c_str());
        auto& io=ImGui::GetIO();io.AddMousePosEvent(100,100);
        f.frame();f.frame();f.frame();
        CHECK(f.lua->run_for_test("","return hookCalls")=="3");
        const auto* implicit=ImGui::FindWindowByName("Debug##Default");
        REQUIRE(implicit);
        CHECK(implicit->Active==draw);
        CHECK(f.implicit_written==draw);
        CHECK(GImGui->HoveredWindow==(draw?implicit:nullptr));
        CHECK(io.WantCaptureMouse==draw);
    }
}

TEST_CASE("UI extensions drawing: registration namespaces preserve builtin and sibling widget identities") {
    DrawingUi f;
    f.put("ui",R"lua(
ui.window('Identity parent',function()
 ui.Begin('Raid identity parent');ui.InvisibleButton('Promote',100,30)
 builtin=ui.GetItemID()
 hs.UI.extensions.DrawSection('RaidWindow','member_actions','before',{})
 after=ui.GetItemID();ui.End()
end)
)lua");
    f.put("owner",R"lua(
local E=hs.UI.extensions
E.RegisterSection('RaidWindow','member_actions','before','first',function()
 ui.PopID();ui.Button('Promote');first=ui.GetItemID()
end)
E.RegisterSection('RaidWindow','member_actions','before','second',function()
 ui.PopID();ui.Button('Promote');second=ui.GetItemID()
end)
)lua");
    f.frame();
    const auto builtin=f.lua->run_for_test("","return builtin");
    const auto after=f.lua->run_for_test("","return after");
    const auto first=f.lua->run_for_test("plugin:display","return first");
    const auto second=f.lua->run_for_test("plugin:display","return second");
    CHECK(builtin!="nil");CHECK(first!="nil");CHECK(second!="nil");
    CHECK(first!=second);CHECK(first!=builtin);CHECK(second!=builtin);CHECK(after==builtin);
}

TEST_CASE("UI extensions drawing: UI observation fails safely before a frame and without a current window") {
    DrawingUi f;f.frame();
    CHECK(f.lua->run_for_test("plugin:display",R"lua(
 local ok,why=pcall(ui.Button,'not in a frame')
 return tostring(not ok and why:find('active ImGui',1,true)~=nil)
)lua")=="true");
    CHECK(f.lua->run_for_test("plugin:display",R"lua(
 local ok,why=pcall(ui.Begin,'not in a frame')
 return tostring(not ok and why:find('active ImGui',1,true)~=nil)
)lua")=="true");
    ImGui::NewFrame();auto* before=GImGui->CurrentWindow;GImGui->CurrentWindow=nullptr;
    CHECK(f.lua->run_for_test("plugin:display", "local ok=pcall(ui.Button,'no window');return tostring(ok)")=="false");
    GImGui->CurrentWindow=before;ImGui::EndFrame();
}

TEST_CASE("UI extensions drawing: context observations and configuration remain available between frames") {
    DrawingUi f;
    const std::string observations=R"lua(
 local io=ui.GetIO()
 assert(type(io.width)=='number' and type(ui.GetTime())=='number' and type(ui.GetFrameCount())=='number')
 assert(type(ui.GetStyleAlpha())=='number')
 local x,y=ui.GetMousePos();assert(type(x)=='number' and type(y)=='number')
 for _,query in ipairs({ui.IsMouseDoubleClicked,ui.IsMouseClicked,ui.IsMouseReleased,ui.IsMouseDown,ui.IsMouseDragging}) do
  assert(type(query(0))=='boolean')
 end
 assert(type(ui.IsKeyDown(ui.Key.A))=='boolean' and type(ui.IsKeyPressed(ui.Key.A))=='boolean')
 assert(type(ui.GetItemID())=='number')
 local x0,y0,x1,y1=ui.GetItemRect();assert(type(x0)=='number' and type(y1)=='number')
 for _,query in ipairs({ui.IsItemActive,ui.IsItemDisabled,ui.IsItemFocused,ui.IsItemDeactivatedAfterEdit,ui.IsWindowAppearing}) do
  assert(type(query())=='boolean')
 end
)lua";
    CHECK(f.lua->run_for_test("plugin:display",observations+"return true")=="true");
    if (!GImGui->Font)
        CHECK(f.lua->run_for_test("plugin:display", "return pcall(ui.CalcTextSize,'not initialized')")=="false");
    f.put("ui",R"lua(
ui.window('Context target',function() ui.Begin('Context target');ui.Button('Last item');ui.End() end)
)lua");
    f.frame();
    CHECK_FALSE(GImGui->WithinFrameScope);
    CHECK(f.lua->run_for_test("plugin:display",observations+R"lua(
 local width,height=ui.CalcTextSize('Between frames');assert(width>0 and height>0)
 local x,y,w,h=ui.FindWindowRect('Context target');assert(type(x)=='number' and w>0 and h>0)
 ui.SetWindowFocus('Context target')
 ui.SetNextWindowSize(410,310);ui.SetNextWindowPos(35,45);ui.SetNextWindowBgAlpha(0.7)
 ui.SetNextWindowCollapsed(false);ui.SetNextWindowFocus();ui.SetNextItemWidth(123);ui.SetNextItemAllowOverlap()
 ui.CaptureKeyboard();ui.SetNavCursorVisible(false)
 return true
)lua")=="true");
    CHECK(GImGui->NavWindow==ImGui::FindWindowByName("Context target"));
    CHECK((GImGui->NextWindowData.HasFlags&ImGuiNextWindowDataFlags_HasSize)!=0);
    CHECK(GImGui->NextWindowData.SizeVal.x==doctest::Approx(410));
    CHECK(GImGui->NextItemData.Width==doctest::Approx(123));
    CHECK(GImGui->WantCaptureKeyboardNextFrame==1);
    auto* current=GImGui->CurrentWindow;GImGui->CurrentWindow=nullptr;
    CHECK(f.lua->run_for_test("plugin:display",observations+"return true")=="true");
    CHECK(f.lua->run_for_test("plugin:display", "return pcall(ui.DrawRectFilled,0,0,10,10,1,1,1,1)")=="false");
    GImGui->CurrentWindow=current;
}

TEST_CASE("UI extensions drawing: action callbacks keep registration IDs and caller item observations separate") {
    DrawingUi f;
    f.put("ui",R"lua(
ui.window('Action identity',function()
 ui.Begin('Action identity parent');ui.InvisibleButton('Promote',100,30)
 builtin=ui.GetItemID()
 local rows=hs.UI.extensions.ListActions('RaidWindow','member_actions',{})
 assert(#rows==1 and hs.UI.extensions.InvokeAction(rows[1].handle,'invoke',{}).ok)
 after=ui.GetItemID();ui.End()
end)
)lua");
    f.put("owner",R"lua(
hs.UI.extensions.RegisterAction('RaidWindow','member_actions','identity','Promote',{},function()
 ui.PopID();ui.Button('Promote');actionItem=ui.GetItemID()
 return {ok=true}
end)
)lua");
    f.frame();
    const auto builtin=f.lua->run_for_test("","return builtin");
    const auto action=f.lua->run_for_test("plugin:display","return actionItem");
    CHECK(builtin!="nil");CHECK(action!="nil");CHECK(action!=builtin);
    CHECK(f.lua->run_for_test("","return after")==builtin);
}

TEST_CASE("UI extensions drawing: popup and combo ends require matching owned begin kinds") {
    DrawingUi f;
    f.put("ui",R"lua(
ui.window('Compound parent',function()
 ui.SetNextWindowSize(650,550)
 ui.Begin('Compound parent')
 hs.UI.extensions.DrawSection('RaidWindow','member_actions','before',{})
 hs.ForceCombo();assert(ui.BeginCombo('owned combo','native parent'))
 local parent,one=hs.KindCounts();assert(one==1)
 hs.UI.extensions.DrawSection('RaidWindow','member_actions','after',{})
 local restored,stillOne=hs.KindCounts();assert(restored==parent and stillOne==1)
 ui.EndCombo()
 ui.Text('The parent still draws');ui.End()
end)
)lua");
    f.put("owner",R"lua(
hs.UI.extensions.RegisterSection('RaidWindow','member_actions','before','compound',function()
 local start,zero=hs.KindCounts();assert(zero==0)
 hs.ForceCombo();assert(ui.BeginCombo('owned combo','choice'))
 local comboDepth,one=hs.KindCounts();assert(one==1)
 ui.EndPopup();ui.EndMenu();ui.EndTooltip()
 local same,stillOne=hs.KindCounts();assert(same==comboDepth and stillOne==1)
 ui.OpenPopup('owned generic')
 assert(ui.BeginPopup('owned generic'))
 local genericDepth=hs.KindCounts()
 ui.EndCombo();ui.EndMenu();ui.EndTooltip()
 local unchanged,comboStillOpen=hs.KindCounts();assert(unchanged==genericDepth and comboStillOpen==1)
 ui.EndPopup();ui.EndCombo()
 local restored,none=hs.KindCounts();assert(restored==start and none==0)
 compoundPassed=true
end)
hs.UI.extensions.RegisterSection('RaidWindow','member_actions','after','parent-combo',function()
 local parent,one=hs.KindCounts();assert(one==1)
 ui.OpenPopup('generic below native combo');assert(ui.BeginPopup('generic below native combo'))
 local nested=hs.KindCounts()
 ui.EndCombo();ui.EndMenu();ui.EndTooltip()
 local same,stillOne=hs.KindCounts();assert(same==nested and stillOne==1)
 ui.EndPopup()
 local restored,oneAgain=hs.KindCounts();assert(restored==parent and oneAgain==1)
 parentComboPassed=true
end)
)lua");
    f.frame();
    CHECK(f.lua->run_for_test("plugin:display","return tostring(compoundPassed)")=="true");
    CHECK(f.lua->run_for_test("plugin:display","return tostring(parentComboPassed)")=="true");
    CHECK(GImGui->BeginComboDepth==0);
}

TEST_CASE("UI extensions presenters: actual Modern and Gamepad Raid extension navigation") {
    lua_State* L=luaL_newstate();REQUIRE(L);luaL_openlibs(L);
    const std::string root=fs::path(HAILSTONE_SOURCE_DIR).generic_string();
    lua_pushlstring(L,root.data(),root.size());lua_setglobal(L,"CLIENT_ROOT");
    const auto file=(fs::path(HAILSTONE_SOURCE_DIR)/"tests/ui_extensions_ui_test.lua").string();
    const int loaded=luaL_loadfilex(L,file.c_str(),"t");
    int result=loaded;if(result==LUA_OK)result=lua_pcall(L,0,0,0);
    const std::string error=result==LUA_OK?"":lua_tostring(L,-1);
    lua_close(L);CHECK_MESSAGE(result==LUA_OK,error);
}
