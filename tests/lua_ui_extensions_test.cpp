// Real UI VM, owner generations and host permission scopes for window extension callbacks.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "plugin/policy.h"
#include "plugin/ui_bridge.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"

namespace {
using namespace hailstone;
namespace fs=std::filesystem;
constexpr auto kFloor=plugin::kCapRead|plugin::kCapUi;
constexpr const char* kA="plugin:extension-a";
constexpr const char* kB="plugin:extension-b";
constexpr const char* kC="plugin:extension-c";
constexpr const char* kContext="{selection={name='Offzone',group=0},revision='roster:1'}";

struct ExtensionUi {
    test::ScratchDir scratch{"ui_extensions"};
    ImGuiContext* previous=ImGui::GetCurrentContext();
    ImGuiContext* imgui=ImGui::CreateContext();
    app::UiHost host;
    plugin::UiBridge bridge;
    std::unique_ptr<app::UiApi> api;
    std::unique_ptr<ui::lua::LuaUi> lua;
    std::map<std::string,std::uint32_t,std::less<>> caps{{kA,plugin::kCapAll},{kB,plugin::kCapAll},{kC,plugin::kCapAll}};
    bool throw_scope=false;
    int native_calls=0, capability_queries=0, throw_capability_query=0;

    ExtensionUi() {
        ImGui::SetCurrentContext(imgui);
        ImGui::GetIO().IniFilename=nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        for(const auto* dir:{"pack","a","b","c"})fs::create_directories(scratch.path()/dir);
        for(const auto* dir:{"a","b","c"})std::ofstream(scratch.path()/dir/"entry.lua")<<"loaded=true\n";
        host.in_game=host.ui_ready=true;
        host.plugin_ui=&bridge;
        host.plugin_caps=[this](std::string_view tag){
            if(++capability_queries==throw_capability_query)throw std::runtime_error("capability query failed");
            const auto found=caps.find(tag);return found==caps.end()?0u:found->second;
        };
        bridge.add_function("extension-a","native_chat",[this](const std::vector<plugin::UiValue>&){
            // Deliberately model a native host table that uses its live owner's
            // CHAT bit rather than CallerScope. The registry must not lend it.
            if(caps[kA]&plugin::kCapChat)++native_calls;
            return plugin::UiValue::boolean(true);
        });
        api=std::make_unique<app::UiApi>(host);
        lua=std::make_unique<ui::lua::LuaUi>(scratch.path()/"pack",scratch.path()/"settings.ini");
        lua->set_caller_scope([this](std::string_view tag)->std::shared_ptr<void>{
            if(throw_scope&&tag==kA)throw std::runtime_error("scope failed");
            return std::make_shared<plugin::CallerScope>(host.plugin_caps(tag),tag);
        });
        lua->add_ui_api([this](lua_State* state){api->register_functions(state);});
        lua->set_folders({{kA,scratch.path()/"a"},{kB,scratch.path()/"b"},{kC,scratch.path()/"c"}});
        frame();
    }
    ~ExtensionUi(){lua.reset();api.reset();ImGui::DestroyContext(imgui);ImGui::SetCurrentContext(previous);}
    void frame(){
        ImGui::SetCurrentContext(imgui);ImGui::GetIO().DisplaySize={900,600};ImGui::GetIO().DeltaTime=1.0f/60.0f;
        if(api) api->begin_frame();
        ImGui::NewFrame();lua->frame();ImGui::Render();
    }
    std::string run(const std::string& code,std::string_view tag={}){return lua->run_for_test(tag,code);}
    std::string add(std::string_view id,std::string_view tag={},std::string_view options="{}",std::string_view fn="function()return {ok=true,message='ok'}end"){
        return run("return hs.UI.extensions.RegisterAction('RaidWindow','member_actions','"+std::string(id)+"','"+
            std::string(id)+"',"+std::string(options)+","+std::string(fn)+")",tag);
    }
    static std::string quoted(const std::string& handle){
        REQUIRE(handle.starts_with("extension:"));
        REQUIRE(handle.find_first_not_of("extension:0123456789")==std::string::npos);
        return "'"+handle+"'";
    }
    std::string invoke(const std::string& handle,std::string_view tag={},std::string_view phase="invoke",std::string_view context=kContext,std::string_view token=""){
        return run("return hs.UI.extensions.InvokeAction("+quoted(handle)+",'"+std::string(phase)+"',"+
            std::string(context)+",'"+std::string(token)+"').ok",tag);
    }
    void rewrite(const std::string& code){
        const auto file=scratch.path()/"a/entry.lua";const auto before=fs::last_write_time(file);
        std::ofstream(file,std::ios::binary)<<code;
        fs::last_write_time(file,before+std::chrono::seconds(2));
        REQUIRE(fs::last_write_time(file)!=before);
        lua->rescan_for_test();frame();
    }
};
}

TEST_CASE("UI extensions: metadata is callback free and registration uses named capabilities") {
    ExtensionUi ui;
    const auto handle=ui.add("grant",kA,"{requiresSelection=true,requiresReview=true,requiredCaps={'chat'}}",
        "function()count=(count or 0)+1 return {ok=true,message='review',reviewToken='ticket'}end");
    CHECK(handle.starts_with("extension:"));
    CHECK(ui.run("return hs.UI.extensions.IsRegistered("+ExtensionUi::quoted(handle)+")",kA)=="true");
    CHECK(ui.run("local a=hs.UI.extensions.ListActions('RaidWindow','member_actions',"+std::string(kContext)+") return #a==1 and a[1].enabled and a[1].requiresSelection and a[1].requiresReview and a[1].owner=='plugin:extension-a'")=="true");
    CHECK(ui.run("local a=hs.UI.extensions.ListActions('RaidWindow','member_actions',{})return #a==1 and not a[1].enabled")=="true");
    CHECK(ui.run("return count==nil",kA)=="true");
    CHECK(ui.run("return not pcall(hs.UI.extensions.RegisterAction,'Invalid window','member_actions','x','X',{},function()end)",kA)=="true");
    CHECK(ui.run("return not pcall(hs.UI.extensions.RegisterAction,'RaidWindow','member_actions','x','X',{requiredCaps={'all'}},function()end)",kA)=="true");
    CHECK(ui.run("return not pcall(hs.UI.extensions.RegisterAction,'RaidWindow','member_actions','x','X',{requiredCaps={'chat,read'}},function()end)",kA)=="true");
    CHECK(ui.run("return not pcall(hs.UI.extensions.RegisterAction,'RaidWindow','member_actions','x','X',{requiresReview=1},function()end)",kA)=="true");
    ui.caps[kB]=plugin::kCapUi;
    CHECK(ui.add("missing-read",kB)=="nil");
    ui.caps[kB]=plugin::kCapRead;
    CHECK(ui.add("missing-ui",kB)=="nil");
}

TEST_CASE("UI extensions: action phases contexts and strict response values") {
    ExtensionUi ui;
    const auto reviewed=ui.add("review",kA,"{requiresSelection=true,requiresReview=true}",R"(function(window,id,phase,context,token)
      calls=(calls or 0)+1
      assert(window=='RaidWindow' and id=='review' and context.selection.name=='Offzone')
      if phase=='review' then return {ok=true,message='Review Offzone',reviewToken='ticket'} end
      return {ok=token=='ticket',message=phase}
    end)");
    CHECK(ui.invoke(reviewed,{},"invoke")=="false");
    CHECK(ui.invoke(reviewed,{},"confirm")=="false");
    CHECK(ui.invoke(reviewed,{},"review","{}")=="false");
    CHECK(ui.run("return calls==nil",kA)=="true");
    CHECK(ui.invoke(reviewed,{},"review")=="true");
    CHECK(ui.invoke(reviewed,{},"confirm",kContext,"wrong")=="false");
    CHECK(ui.invoke(reviewed,{},"confirm",kContext,"ticket")=="true");
    CHECK(ui.invoke(reviewed,{},"cancel",kContext,"ticket")=="true");
    CHECK(ui.run("return calls",kA)=="4");
    const auto numeric=ui.add("numeric",kA,"{}","function()return {ok=1,message='not boolean'}end");
    CHECK(ui.invoke(numeric)=="false");
    const auto invalidReview=ui.add("empty-review",kA,"{requiresReview=true}","function()return {ok=true,message='',reviewToken=''}end");
    CHECK(ui.invoke(invalidReview,{},"review")=="false");
    const auto tooLarge=ui.add("large",kA,"{}","function()return {ok=true,message=string.rep('x',4097)}end");
    CHECK(ui.invoke(tooLarge)=="false");
    const auto plain=ui.add("plain",kA);
    CHECK(ui.invoke(plain,{},"review")=="false");
    CHECK(ui.invoke(plain)=="true");
}

TEST_CASE("UI extensions: map controls share copied contexts owner admission and lifecycle with raid actions") {
    ExtensionUi ui;
    const std::string context = "{window='MapViewWnd',section='controls',revision='policy:7/context:9',"
        "policy={custom_spawns=true,stock_npcs=false,npc_find=false,name='Map provider',revision='7',context='9',source='3'}}";
    const auto handle = ui.run(R"(
      return hs.UI.extensions.RegisterAction('MapViewWnd','controls','settings','Map settings',{},
        function(window,id,phase,context)
          assert(window=='MapViewWnd' and id=='settings' and phase=='invoke')
          assert(context.window==window and context.section=='controls')
          assert(context.policy.revision=='7' and context.policy.context=='9' and context.policy.source=='3')
          assert(context.policy.custom_spawns and not context.policy.stock_npcs and not context.policy.npc_find)
          mapCalls=(mapCalls or 0)+1
          context.policy.name='callback copy'
          return {ok=true,message='Map settings'}
        end)
    )", kA);
    REQUIRE(handle.starts_with("extension:"));
    const auto raid = ui.add("raid", kB);
    CHECK(ui.run("mapContext=" + context + " local rows=hs.UI.extensions.ListActions('MapViewWnd','controls',mapContext) "
        "return #rows==1 and rows[1].enabled and rows[1].id=='settings'") == "true");
    CHECK(ui.run("return mapCalls==nil", kA) == "true");
    CHECK(ui.run("return #hs.UI.extensions.ListActions('RaidWindow','member_actions',{})") == "1");
    CHECK(ui.invoke(handle, {}, "invoke", "mapContext") == "true");
    CHECK(ui.run("return mapContext.policy.name=='Map provider'") == "true");
    CHECK(ui.run("return #hs.UI.extensions.ListActions('MapViewWnd','controls',{})", kB) == "0");
    CHECK(ui.invoke(handle, kB, "invoke", context) == "false");
    ui.caps[kA] = plugin::kCapAll & ~plugin::kCapRead;
    CHECK(ui.invoke(handle, {}, "invoke", context) == "false");
    CHECK(ui.run("local rows=hs.UI.extensions.ListActions('MapViewWnd','controls',{}) return #rows==1 and not rows[1].enabled") == "true");
    CHECK(ui.run("return hs.UI.extensions.IsRegistered(" + ExtensionUi::quoted(handle) + ")") == "true");
    ui.caps[kA] = plugin::kCapAll;
    CHECK(ui.invoke(handle, {}, "invoke", context) == "true");
    CHECK(ui.run("return mapCalls", kA) == "2");
    for (const auto caps : {plugin::kCapRead, plugin::kCapUi}) {
        ui.caps[kC] = caps;
        CHECK(ui.run("return hs.UI.extensions.RegisterAction('MapViewWnd','controls','denied','Denied',{},function()end)", kC) == "nil");
    }
    const auto needs_selection = ui.run("return hs.UI.extensions.RegisterAction('MapViewWnd','controls','selection','Selection',"
        "{requiresSelection=true},function()selectionCalled=true return {ok=true}end)", kA);
    CHECK(ui.invoke(needs_selection, {}, "invoke", context) == "false");
    CHECK(ui.run("return selectionCalled==nil", kA) == "true");
    ui.lua->set_folders({{kB,ui.scratch.path()/"b"},{kC,ui.scratch.path()/"c"}});
    ui.frame();
    CHECK(ui.run("return hs.UI.extensions.IsRegistered(" + ExtensionUi::quoted(handle) + ")") == "false");
    CHECK(ui.invoke(handle, {}, "invoke", context) == "false");
    CHECK(ui.invoke(raid) == "true");
}

TEST_CASE("UI extensions: map sections execute only at their declared active drawing point") {
    ExtensionUi ui;
    CHECK(ui.run(R"(
      hs.UI.extensions.RegisterSection('MapViewWnd','controls','before','before',function(window,section,placement,context)
        assert(window=='MapViewWnd' and section=='controls' and placement=='before' and context.policy.revision=='7')
        mapBefore=(mapBefore or 0)+1
      end)
      hs.UI.extensions.RegisterSection('MapViewWnd','controls','after','after',function(_,_,placement)
        assert(placement=='after') mapAfter=(mapAfter or 0)+1
      end)
      return true
    )", kA) == "true");
    for (const auto& [window, section] : {std::pair{"Map ViewWnd", "member_actions"},
            std::pair{"RaidWindow", "controls/bad"}, std::pair{"MapViewWnd", "invalid section"}}) {
        CHECK(ui.run("return not pcall(hs.UI.extensions.RegisterAction,'" + std::string(window) + "','" + section +
            "','bad','Bad',{},function()end)", kA) == "true");
        CHECK(ui.run("return not pcall(hs.UI.extensions.RegisterSection,'" + std::string(window) + "','" + section +
            "','before','bad',function()end)", kA) == "true");
    }
    CHECK(ui.run("return hs.UI.extensions.DrawSection('MapViewWnd','controls','before',{policy={revision='7'}})") == "0");
    CHECK(ui.run("return mapBefore==nil and mapAfter==nil", kA) == "true");
    CHECK(ui.run(R"(
      local rows=hs.UI.extensions.ListActions('MapViewWnd','controls',{})
      assert(#rows==0 and #rows.sections==2)
      for _,item in ipairs(rows.sections) do
        assert(item.owner=='plugin:extension-a' and item.enabled and item.priority==0)
        assert(type(item.handle)=='string' and type(item.id)=='string' and item.callback==nil)
        assert(item.placement=='before' or item.placement=='after')
      end
      return true
    )") == "true");
    CHECK(ui.run("return #hs.UI.extensions.ListActions('MapViewWnd','controls',{}).sections", kB) == "0");
    CHECK(ui.run("return mapBefore==nil and mapAfter==nil", kA) == "true");
    ImGui::SetCurrentContext(ui.imgui);
    ui.api->begin_frame(); ImGui::NewFrame(); ImGui::Begin("Map controls fixture");
    CHECK(ui.run("return hs.UI.extensions.DrawSection('RaidWindow','member_actions','before',{})") == "0");
    CHECK(ui.run("return hs.UI.extensions.DrawSection('MapViewWnd','controls','before',{policy={revision='7'}})") == "1");
    CHECK(ui.run("return hs.UI.extensions.DrawSection('MapViewWnd','controls','after',{})") == "1");
    CHECK(ui.run("return mapBefore==1 and mapAfter==1", kA) == "true");
    ImGui::End(); ImGui::Render();
}

TEST_CASE("UI extensions: only declared slots, valid identifiers, and nothing injected into unemitted components") {
    ExtensionUi ui;
    CHECK(ui.run(R"(
      local E=hs.UI.extensions
      arbitrary=E.RegisterAction('MapViewWnd','toolbar.left-2','action','Action',{},function()actionRan=true return {ok=true}end)
      display=E.RegisterSection('MapViewWnd','toolbar.left-2','replace','display',function()displayRan=true end)
      local rows=E.ListActions('MapViewWnd','toolbar.left-2',{})
      assert(#rows==1 and #rows.sections==1 and rows.sections[1].placement=='replace')
      for _,bad in ipairs({'bad name','bad/name','bad+name','bad\0name','é',string.rep('x',129)}) do
        assert(not pcall(E.RegisterAction,bad,'slot','bad','Bad',{},function()end))
        assert(not pcall(E.RegisterSection,'Window',bad,'before','bad',function()end))
        assert(not pcall(E.ListActions,'Window',bad,{}))
        assert(not pcall(E.DrawSection,bad,'slot','replace',{}))
      end
      -- Only the points the interfaces offer (lua_ui_extensions.cpp): another window or part is refused.
      assert(not pcall(E.RegisterSection,'New.Window','caption','before','undeclared',function()end))
      assert(not pcall(E.RegisterAction,'MapViewWnd','legend','undeclared','Undeclared',{},function()end))
      assert(not pcall(E.RegisterSection,'RaidWindow','member_actions.extra','before','undeclared',function()end))
      assert(not pcall(E.RegisterSection,'Window','slot','unknown','bad',function()end))
      return E.DrawSection('MapViewWnd','toolbar.left-2','replace',{})==false and actionRan==nil and displayRan==nil
    )", kA) == "true");
    ui.frame();
    CHECK(ui.run("return actionRan==nil and displayRan==nil", kA) == "true");
    CHECK(ui.run("local rows=hs.UI.extensions.ListActions('MapViewWnd','toolbar.other',{}) return #rows==0 and #rows.sections==0") == "true");
}

TEST_CASE("UI extensions: one deterministic replacement wins and an empty success suppresses the default") {
    ExtensionUi ui;
    CHECK(ui.run(R"(
      high=hs.UI.extensions.RegisterSection('MapViewWnd','toolbar.caption','replace','high',function(...)
        assert(select('#',...)==4)
        local window,section,placement,context=...
        assert(window=='MapViewWnd' and section=='toolbar.caption' and placement=='replace' and context.value==7)
        highCalls=(highCalls or 0)+1
      end,{priority=-10,requiredCaps={'chat'}})
      return type(high)
    )", kA) == "string");
    CHECK(ui.run(R"(
      low=hs.UI.extensions.RegisterSection('MapViewWnd','toolbar.caption','replace','low',function()lowCalls=(lowCalls or 0)+1 end,{priority=10})
      return type(low)
    )", kB) == "string");
    ImGui::SetCurrentContext(ui.imgui);
    ui.api->begin_frame(); ImGui::NewFrame(); ImGui::Begin("Replacement fixture");
    const auto draw = [&] {
        return ui.run("local replaced=hs.UI.extensions.DrawSection('MapViewWnd','toolbar.caption','replace',{value=7}) "
            "if not replaced then defaults=(defaults or 0)+1 end return type(replaced)..':'..tostring(replaced)");
    };
    CHECK(draw() == "boolean:true");
    CHECK(ui.run("return highCalls", kA) == "1"); CHECK(ui.run("return lowCalls==nil", kB) == "true");
    CHECK(ui.run("return defaults==nil") == "true"); // an empty successful callback intentionally hides the default
    ui.caps[kA] &= ~plugin::kCapChat;
    CHECK(ui.run("local s=hs.UI.extensions.ListActions('MapViewWnd','toolbar.caption',{}).sections return #s==2 and not s[1].enabled and s[2].enabled") == "true");
    CHECK(draw() == "boolean:true"); CHECK(ui.run("return lowCalls", kB) == "1");
    ui.caps[kA] = plugin::kCapAll;
    CHECK(draw() == "boolean:true"); CHECK(ui.run("return highCalls", kA) == "2");
    CHECK(ui.run("return hs.UI.extensions.DrawSection('MapViewWnd','toolbar.caption','replace',{value=7})", kC) == "false");
    CHECK(ui.run("return lowCalls", kB) == "1");
    CHECK(ui.run("failure=hs.UI.extensions.RegisterSection('MapViewWnd','toolbar.caption','replace','failure',function()error('replace failed')end,{priority=-20}) return failure~=nil", kA) == "true");
    CHECK(draw() == "boolean:false");
    CHECK(ui.run("return defaults") == "1");
    CHECK(ui.run("return lowCalls", kB) == "1"); // a failed winner restores default, not the lower-priority addon
    // These callbacks were created by owner-scoped test chunks, not one managed source file. Only the failed
    // registration retires, so the surviving higher-priority callback still wins until explicitly removed.
    CHECK(ui.run("return not hs.UI.extensions.IsRegistered(failure) and hs.UI.extensions.IsRegistered(high)", kA) == "true");
    CHECK(draw() == "boolean:true");
    CHECK(ui.run("return highCalls", kA) == "3"); CHECK(ui.run("return lowCalls", kB) == "1");
    CHECK(ui.run("return hs.UI.extensions.Remove(high)", kA) == "true");
    CHECK(draw() == "boolean:true"); CHECK(ui.run("return lowCalls", kB) == "2");
    CHECK(ui.run("return hs.UI.extensions.DrawSection('MapViewWnd','toolbar.unemitted','replace',{})") == "false");
    ImGui::End(); ImGui::Render();
}

TEST_CASE("UI extensions: root same owner foreign and named native roles stay distinct") {
    ExtensionUi ui;
    const auto owned=ui.add("owned",kA);
    const auto root=ui.add("root");
    CHECK(ui.invoke(owned)=="true");
    CHECK(ui.invoke(owned,kA)=="true");
    CHECK(ui.invoke(owned,kB)=="false");
    CHECK(ui.invoke(root,kA)=="false");
    CHECK(ui.run("return #hs.UI.extensions.ListActions('RaidWindow','member_actions',"+std::string(kContext)+")",kB)=="0");
    CHECK(ui.run("return hs.UI.extensions.IsRegistered("+ExtensionUi::quoted(owned)+")",kB)=="false");
    CHECK(ui.run("return hs.UI.extensions.Remove("+ExtensionUi::quoted(owned)+")",kB)=="false");
    {
        plugin::CallerScope native(plugin::kCapAll,"foreign-native");
        CHECK(ui.invoke(root)=="false");
        CHECK(ui.invoke(owned)=="false");
        CHECK(ui.run("return hs.UI.extensions.Remove("+ExtensionUi::quoted(root)+")")=="false");
        CHECK(ui.add("native-root")=="nil");
        CHECK(ui.run("return #hs.UI.extensions.ListActions('RaidWindow','member_actions',"+std::string(kContext)+")")=="0");
    }
    CHECK(ui.invoke(root)=="true");
    CHECK(ui.run("return hs.UI.extensions.Remove("+ExtensionUi::quoted(owned)+")",kA)=="true");
    CHECK(ui.invoke(owned)=="false");
}

TEST_CASE("UI extensions: temporary read and chat loss retains lifecycle without dispatch") {
    ExtensionUi ui;
    const auto handle=ui.add("chat",kA,"{requiredCaps={'chat'}}","function()calls=(calls or 0)+1 return {ok=true}end");
    CHECK(ui.invoke(handle)=="true");
    for(const auto missing:{plugin::kCapChat,plugin::kCapRead}){
        ui.caps[kA]=plugin::kCapAll&~missing;
        CHECK(ui.run("return hs.UI.extensions.IsRegistered("+ExtensionUi::quoted(handle)+")",kA)=="true");
        CHECK(ui.run("local a=hs.UI.extensions.ListActions('RaidWindow','member_actions',"+std::string(kContext)+") return #a==1 and not a[1].enabled")=="true");
        CHECK(ui.invoke(handle)=="false");
        CHECK(ui.run("return calls",kA)=="1");
    }
    ui.caps[kA]=plugin::kCapAll;
    CHECK(ui.invoke(handle)=="true");
    CHECK(ui.run("return calls",kA)=="2");
}

TEST_CASE("UI extensions: native authority cannot exceed current or captured ceilings") {
    ExtensionUi ui;
    ui.caps[kA]=kFloor|plugin::kCapChat;
    const auto handle=ui.add("native",kA,"{}","function()hs.UI.plugins.Get('extension-a').native_chat() return {ok=true}end");
    {
        plugin::CallerScope reduced(kFloor,"");
        CHECK(ui.invoke(handle)=="false");
    }
    CHECK(ui.native_calls==0);
    CHECK(ui.run("return hs.UI.extensions.IsRegistered("+ExtensionUi::quoted(handle)+")")=="true");
    CHECK(ui.invoke(handle)=="true");
    CHECK(ui.native_calls==1);
    ui.caps[kA]|=plugin::kCapCommands;
    CHECK(ui.run("return hs.UI.extensions.IsRegistered("+ExtensionUi::quoted(handle)+")")=="false");
    CHECK(ui.invoke(handle)=="false");
    CHECK(ui.native_calls==1);
    ui.caps[kA]=plugin::kCapAll;
    {
        plugin::CallerScope reduced(kFloor,kA);
        CHECK(ui.add("narrowed",kA)=="nil");
    }
}

TEST_CASE("UI extensions: replacement source reload folder removal and host lifetime") {
    ExtensionUi ui;
    const auto old=ui.add("replace",kA), current=ui.add("replace",kA);
    CHECK(old!=current);
    CHECK(ui.invoke(old)=="false");CHECK(ui.invoke(current)=="true");
    const auto self=ui.add("self",kA,"{}","function()hs.UI.extensions.Remove(self) return {ok=true,message='must not publish'}end");
    CHECK(ui.run("self="+ExtensionUi::quoted(self)+" return true",kA)=="true");
    CHECK(ui.invoke(self)=="false");
    ui.rewrite(R"(
      savedRegister=hs.UI.extensions.RegisterAction
      fileHandle=savedRegister('RaidWindow','member_actions','file','File',{},function()count=(count or 0)+1 return {ok=true}end)
    )");
    const auto file=ui.run("return fileHandle",kA);
    CHECK(ui.invoke(file)=="true");
    ui.rewrite("replacement=true\n");
    CHECK(ui.invoke(file)=="false");
    CHECK(ui.run("return pcall(savedRegister,'RaidWindow','member_actions','stale','Stale',{},function()end)",kA)=="false");
    const auto removed=ui.add("removed",kA);
    ui.lua->set_folders({{kB,ui.scratch.path()/"b"},{kC,ui.scratch.path()/"c"}});
    CHECK(ui.invoke(removed)=="false");
    const auto root=ui.add("root");
    ui.api.reset();
    CHECK(ui.invoke(root)=="false");
    CHECK(ui.run("return hs.UI.extensions.IsRegistered("+ExtensionUi::quoted(root)+")")=="false");
    CHECK(ui.add("expired")=="nil");
}

TEST_CASE("UI extensions: limits are global per owner and replacement does not spend a slot") {
    ExtensionUi ui;
    const std::string fill=R"(
      for i=1,32 do assert(hs.UI.extensions.RegisterAction('RaidWindow','member_actions','slot'..i,'Slot',{},function()return {ok=true}end)) end
      return hs.UI.extensions.RegisterAction('RaidWindow','member_actions','overflow','Overflow',{},function()end)==nil
    )";
    CHECK(ui.run(fill,kA)=="true");
    CHECK(ui.run(fill,kB)=="true");
    CHECK(ui.run(fill,kC)=="true");
    CHECK(ui.run(fill)=="true");
    CHECK(ui.run("return #hs.UI.extensions.ListActions('RaidWindow','member_actions',"+std::string(kContext)+")")=="128");
    CHECK(ui.add("slot1",kA).starts_with("extension:"));
    CHECK(ui.run("return #hs.UI.extensions.ListActions('RaidWindow','member_actions',"+std::string(kContext)+")")=="128");
}

TEST_CASE("UI extensions: metadata order is deterministic and sections do not dispatch outside drawing") {
    ExtensionUi ui;
    ui.add("beta",kA);ui.add("alpha",kB);ui.add("alpha",kA);ui.add("first",{},"{priority=-5}");
    CHECK(ui.run("local a=hs.UI.extensions.ListActions('RaidWindow','member_actions',{})return #a==4 and a[1].id=='first' and a[2].owner=='plugin:extension-a' and a[2].id=='alpha' and a[3].id=='beta' and a[4].owner=='plugin:extension-b'")=="true");
    CHECK(ui.run("section=hs.UI.extensions.RegisterSection('RaidWindow','member_actions','before','section',function()sectionCalls=(sectionCalls or 0)+1 end) return type(section)",kA)=="string");
    CHECK(ui.run("return hs.UI.extensions.DrawSection('RaidWindow','member_actions','before',{})")=="0");
    CHECK(ui.run("return sectionCalls==nil",kA)=="true");
}

TEST_CASE("UI extensions: callback errors and bounded recursion leave later dispatch usable") {
    ExtensionUi ui;
    const auto failed=ui.add("failure",kA,"{}","function()error('callback error')end");
    CHECK(ui.invoke(failed)=="false");
    CHECK(ui.run("return hs.UI.extensions.IsRegistered("+ExtensionUi::quoted(failed)+")")=="false");
    const auto recursive=ui.add("recursive",kA,"{}",R"(function(_,_,_,context)
      recursion=(recursion or 0)+1
      return hs.UI.extensions.InvokeAction(recursive,'invoke',context)
    end)");
    CHECK(ui.run("recursive="+ExtensionUi::quoted(recursive)+" return true",kA)=="true");
    CHECK(ui.invoke(recursive)=="false");
    CHECK(ui.run("return recursion",kA)=="8");
    CHECK(ui.invoke(recursive)=="false");
    CHECK(ui.run("return recursion",kA)=="16");
    CHECK(ui.invoke(ui.add("after-recursion",kA))=="true");
}

TEST_CASE("UI extensions: throwing source scope restores caller and dispatch depth") {
    ExtensionUi ui;
    const auto handle=ui.add("throwing",kA);
    ui.throw_scope=true;
    CHECK(ui.invoke(handle)=="false");
    CHECK(hailstone::ui::lua::LuaUi::caller_tag().empty());
    CHECK(plugin::caller_name().empty());
    CHECK(plugin::caller_caps()==plugin::kCapAll);
    ui.throw_scope=false;
    CHECK(ui.invoke(ui.add("after-throw",kA))=="true");
    CHECK(ui.invoke(ui.add("root-after-throw"))=="true");
}

TEST_CASE("UI extensions: throwing lifecycle permission queries stay inside Lua error boundaries") {
    for(int fail=1;fail<=4;++fail){
        ExtensionUi ui;
        const auto handle=ui.add("permission-query",kA);
        ui.capability_queries=0;ui.throw_capability_query=fail;
        std::string result;
        CHECK_NOTHROW(result=ui.run("return hs.UI.extensions.IsRegistered("+ExtensionUi::quoted(handle)+")"));
        CAPTURE(fail);CAPTURE(result);
        CHECK((result=="true"||(result.starts_with("error: ")&&result.ends_with("capability query failed"))));
        ui.throw_capability_query=0;
        CHECK(ui.run("return hs.UI.extensions.IsRegistered("+ExtensionUi::quoted(handle)+")")=="true");
    }
}

TEST_CASE("UI extensions: idle maintenance releases retired captures and retains temporarily unavailable sources") {
    ExtensionUi ui;
    CHECK(ui.run(R"(
      local held={payload=string.rep('x',256*1024)}
      weak=setmetatable({held},{__mode='v'})
      handle=hs.UI.extensions.RegisterAction('RaidWindow','member_actions','capture','Capture',
        {requiredCaps={'chat'}},function()
          held.called=true
          hs.UI.plugins.Get('extension-a').native_chat()
          return {ok=true}
        end)
      return type(handle)
    )",kA)=="string");
    auto* state=ui.lua->state();
    ui.lua->push_folder_env_for_test(kA);lua_getfield(state,-1,"weak");
    REQUIRE(lua_istable(state,-1));
    const int weak=luaL_ref(state,LUA_REGISTRYINDEX);lua_pop(state,1);
    const auto held=[&]{
        const int top=lua_gettop(state);
        lua_rawgeti(state,LUA_REGISTRYINDEX,weak);lua_rawgeti(state,-1,1);
        const bool present=!lua_isnil(state,-1);
        if(present){lua_getfield(state,-1,"called");CHECK(lua_isnil(state,-1));}
        lua_settop(state,top);return present;
    };
    CHECK(held());
    ui.caps[kA]=plugin::kCapUi; // READ and CHAT are temporary permissions, not source lifetimes.
    ui.frame();
    CHECK(ui.run("collectgarbage('collect') collectgarbage('collect') return true")=="true");
    CHECK(held());
    ui.caps[kA]=plugin::kCapAll;
    ui.frame();
    CHECK(ui.run("collectgarbage('collect') collectgarbage('collect') return true")=="true");
    CHECK(held());
    ui.lua->set_folders({{kB,ui.scratch.path()/"b"},{kC,ui.scratch.path()/"c"}});
    ui.frame();
    CHECK(ui.run("collectgarbage('collect') collectgarbage('collect') return true")=="true");
    CHECK_FALSE(held()); // No List, Invoke, IsRegistered or Remove query caused cleanup.
    CHECK(ui.native_calls==0);
    luaL_unref(state,LUA_REGISTRYINDEX,weak);
}
