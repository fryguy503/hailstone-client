// VM-local method interception: real host scopes and source generations, with observable test host calls.
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_api.h"
#include "app/lua_ui_hooks.h"
#include "plugin/policy.h"
#include "plugin/ui_bridge.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"

namespace {
using namespace hailstone;
namespace fs = std::filesystem;
constexpr auto kHooks = plugin::kCapUi | plugin::kCapRead | plugin::kCapExtend;
constexpr const char* kOwner = "plugin:hooks-test";
constexpr const char* kOther = "plugin:hooks-other";

struct HookUi {
    test::ScratchDir scratch{"ui_hooks"};
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    app::UiHost host;
    plugin::UiBridge bridge;
    std::unique_ptr<app::UiApi> api;
    std::unique_ptr<ui::lua::LuaUi> lua;
    std::map<std::string,std::uint32_t,std::less<>> caps{{kOwner,plugin::kCapAll},{kOther,plugin::kCapAll}};
    int calls = 0, actions = 0;
    int native_calls = 0, action_id = 0;
    ImGuiID hook_widget_id = 0;
    std::uint32_t action_caps = 0;
    bool unusual_results = false;
    bool oversized_results = false;
    std::string large_result = std::string(65537,'x');

    HookUi() {
        ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr; int w = 0, h = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
        fs::create_directories(scratch.path()/"pack");
        fs::create_directories(scratch.path()/"owner");
        fs::create_directories(scratch.path()/"other");
        std::ofstream(scratch.path()/"owner/a.lua") << "loaded=true\n";
        std::ofstream(scratch.path()/"other/a.lua") << "loaded=true\n";
        host.in_game = host.ui_ready = true;
        host.plugin_ui=&bridge;
        bridge.add_function("hooks-test","native_host",[this](const std::vector<plugin::UiValue>&) {
            ++native_calls;return plugin::UiValue{}; // native host exports can have authority outside CallerScope
        });
        bridge.add_function("hooks-test","revoke",[this](const std::vector<plugin::UiValue>&) {
            caps[kOwner]=0;return plugin::UiValue{};
        });
        bridge.add_function("hooks-test","record_id",[this](const std::vector<plugin::UiValue>&) {
            hook_widget_id=ImGui::GetItemID();return plugin::UiValue{};
        });
        host.plugin_caps = [this](std::string_view tag) { auto it=caps.find(tag); return it==caps.end()?0u:it->second; };
        api=std::make_unique<app::UiApi>(host);
        lua=std::make_unique<ui::lua::LuaUi>(scratch.path()/"pack",scratch.path()/"settings.ini");
        lua->set_caller_scope([this](std::string_view tag)->std::shared_ptr<void> {
            return std::make_shared<plugin::CallerScope>(host.plugin_caps(tag),tag);
        });
        lua->add_ui_api([this](lua_State* L) {
            api->register_functions(L);
            lua_pushlightuserdata(L,this);
            lua_newtable(L); // one cached host record: pass-through hooks must preserve its identity
            lua_pushcclosure(L,[](lua_State* state)->int {
                auto& rig=*static_cast<HookUi*>(lua_touserdata(state,lua_upvalueindex(1)));
                ++rig.calls;
                if(lua_type(state,1)==LUA_TSTRING && std::string_view(lua_tostring(state,1))=="error")
                    return luaL_error(state,"original failed");
                if(lua_type(state,1)==LUA_TSTRING && std::string_view(lua_tostring(state,1))=="error-nil") {
                    lua_pushnil(state);return lua_error(state);
                }
                if(lua_type(state,1)==LUA_TSTRING && std::string_view(lua_tostring(state,1))=="error-false") {
                    lua_pushboolean(state,false);return lua_error(state);
                }
                if(lua_type(state,1)==LUA_TSTRING && std::string_view(lua_tostring(state,1))=="error-object") {
                    lua_pushvalue(state,2);return lua_error(state);
                }
                if(lua_type(state,1)==LUA_TSTRING && std::string_view(lua_tostring(state,1))=="error-userdata") {
                    lua_pushlightuserdata(state,&rig);return lua_error(state);
                }
                lua_pushnil(state);
                if(lua_gettop(state)>=2)lua_pushvalue(state,1);else lua_pushnil(state);
                if(rig.oversized_results) lua_pushlstring(state,rig.large_result.data(),rig.large_result.size());
                else if(rig.unusual_results) lua_pushlightuserdata(state,&rig);
                else {lua_pushvalue(state,lua_upvalueindex(2));lua_pushinteger(state,rig.calls);lua_setfield(state,-2,"calls");}
                lua_pushnil(state);
                return 4;
            },2);
            lua_setfield(L,-2,"Gauge");
            lua_pushlightuserdata(L,this);
            lua_pushcclosure(L,[](lua_State* state)->int {
                auto& rig=*static_cast<HookUi*>(lua_touserdata(state,lua_upvalueindex(1)));
                if((plugin::caller_caps()&plugin::kCapCommands)==0){lua_pushnil(state);return 1;}
                ++rig.actions;rig.action_caps=plugin::caller_caps();
                rig.action_id=static_cast<int>(lua_tointeger(state,1));
                lua_pushboolean(state,true);return 1;
            },1);
            lua_setfield(L,-2,"OpenTask");
            app::register_ui_api(L);
        });
        lua->set_folders({{kOwner,scratch.path()/"owner"},{kOther,scratch.path()/"other"}});
        frame();
    }
    ~HookUi() {
        lua.reset();api.reset();ImGui::DestroyContext(context);ImGui::SetCurrentContext(previous);
    }
    void frame() {
        ImGui::SetCurrentContext(context);
        ImGui::GetIO().DisplaySize=ImVec2(800,600);ImGui::GetIO().DeltaTime=1.0f/60.0f;
        if(api)api->begin_frame();
        ImGui::NewFrame();lua->frame();ImGui::Render();
    }
    std::string run(const std::string& code,std::string_view tag={}) { return lua->run_for_test(tag,code); }
    void rewrite(const std::string& code) {
        const auto path=scratch.path()/"owner/a.lua";
        const auto stamp=fs::last_write_time(path);
        std::ofstream(path,std::ios::binary)<<code;
        fs::last_write_time(path,stamp+std::chrono::seconds(2));
        lua->rescan_for_test();frame();
    }
};
}

TEST_CASE("UI hooks: unmanaged states stay closed without opening standard libraries") {
    HookUi ui;
    for (const bool libraries : {false,true}) {
        lua_State* bare=luaL_newstate();
        REQUIRE(bare!=nullptr);
        if (libraries) luaL_openlibs(bare);
        lua_newtable(bare);ui.api->register_functions(bare);lua_setglobal(bare,"hs");
        CHECK(lua_gettop(bare)==0);
        lua_getglobal(bare,"table");CHECK(lua_isnil(bare,-1)==!libraries);lua_pop(bare,1);
        CHECK(luaL_loadstring(bare,R"(
          local api=hs.UI
          local h,why=api.hooks.Register('character.Gauge','before',function()end)
          local records=api.hooks.List('character.Gauge')
          return h==nil and why~=nil and api.hooks.Remove('hook:1')==false
            and records.available==false and records.hookable==true
            and api.character.Gauge==hs.Gauge and api.Supports('hooks','Register')
        )")==LUA_OK);
        REQUIRE(lua_pcall(bare,0,1,0)==LUA_OK);
        CHECK(lua_toboolean(bare,-1));lua_pop(bare,1);
        lua_getglobal(bare,"table");CHECK(lua_isnil(bare,-1)==!libraries);lua_pop(bare,1);
        lua_pushcfunction(bare,[](lua_State* state)->int{lua_pushliteral(state,"untouched");return 1;});
        const void* original=lua_topointer(bare,-1);
        app::wrap_ui_hook_method(bare,"character.Gauge");
        CHECK(lua_topointer(bare,-1)==original);lua_pop(bare,1);
        lua_close(bare);
    }
}

TEST_CASE("UI hooks: stable aliases deterministic observers and nil positions") {
    HookUi ui;
    CHECK(ui.run(R"(
      local H=hs.UI.hooks
      saved=hs.UI.character.Gauge
      assert(saved==hs.Gauge)
      seen={}
      H.Register('character.Gauge','before',function(args)
        assert(args.n==4 and args[2]==nil and args[4]==nil)
        args[1].value='changed';seen[#seen+1]='late'
      end,{priority=2})
      H.Register('character.Gauge','before',function(args) seen[#seen+1]='early' end,{priority=-1})
      H.Register('character.Gauge','after',function(results,args)
        assert(results.n==4 and results[1]==nil and results[4]==nil and args.n==4)
        results[2].value='changed';results[3].calls=-1;seen[#seen+1]='after'
      end)
      local original={value='original'}
      local r=table.pack(saved(original,nil,3,nil))
      return r.n==4 and r[1]==nil and r[2]==original and original.value=='original'
        and r[3].calls==1 and r[4]==nil and table.concat(seen,',')=='early,late,after'
    )")=="true");
    CHECK(ui.calls==1);
    CHECK(ui.run("return hs.UI.Describe('character','Gauge').hookable and not hs.UI.Describe('interfaces','LoadData').hookable")=="true");
    CHECK(ui.run("local h,e=hs.UI.hooks.Register('interfaces.LoadData','before',function()error('exposed')end) return h==nil and #e>0")=="true");
    CHECK(ui.run("local d=hs.UI.hooks.List('interfaces.LoadData') return not d.hookable and #d.reason>0 and #d==0")=="true");
}

TEST_CASE("UI hooks: unhooked hot paths remain allocation bounded") {
    HookUi ui;
    CHECK(ui.run("hs.UI.hooks.Register('character.Gauge','before',function()end) return true")=="true");
    const auto before=ui.lua->allocated_bytes();
    CHECK(ui.run("for i=1,10000 do hs.UI.character.Name() end return true")=="true");
    CHECK(ui.lua->allocated_bytes()-before < 256u*1024u);
}

TEST_CASE("UI hooks: around replacement preserves varargs and continuation cannot repeat or escape") {
    HookUi ui;
    CHECK(ui.run(R"(
      hs.UI.hooks.Register('character.Gauge','around',function(next,a,b,c)
        held=next
        local r=table.pack(next('replacement',nil,c))
        assert(not pcall(next,'duplicate'))
        return r[1],r[2],r[3],nil,'extra'
      end)
      local r=table.pack(hs.Gauge('input',nil,3))
      return r.n==5 and r[1]==nil and r[2]=='replacement' and r[3].calls==1
        and r[4]==nil and r[5]=='extra' and not pcall(held,'late')
    )")=="true");
    CHECK(ui.calls==1);
}

TEST_CASE("UI hooks: pass through preserves cached host tables arguments and captured aliases") {
    HookUi ui;
    CHECK(ui.run(R"(
      captured=hs.Gauge
      input={nested={value='original'}}
      local _,_,record=captured(input)
      cached=record
      hs.UI.hooks.Register('character.Gauge','around',function(next,...)return next(...)end)
      local r=table.pack(captured(input))
      return r.n==4 and r[2]==input and r[2].nested==input.nested and r[3]==cached
    )")=="true");
    ui.lua->add_ui_api([](lua_State* L){app::register_ui_api(L);});
    CHECK(ui.run(R"(
      local r=table.pack(captured(input))
      return r[2]==input and r[3]==cached and hs.Gauge==hs.UI.character.Gauge
    )")=="true");
    CHECK(ui.calls==3);
}

TEST_CASE("UI hooks: retained continuations release completed call data") {
    HookUi ui;
    CHECK(ui.run(R"(
      hs.UI.hooks.Register('character.Gauge','around',function(next,...)
        retained=next;return next(...)
      end)
      local temporary={value='transient'}
      weak=setmetatable({temporary},{__mode='v'})
      hs.Gauge(temporary)
      return true
    )")=="true");
    CHECK(ui.run("collectgarbage('collect') return weak[1]==nil and not pcall(retained,1)")=="true");
    CHECK(ui.calls==1);
}

TEST_CASE("UI hooks: callback widgets have a per-registration namespace") {
    HookUi ui;
    CHECK(ui.run("hs.UI.hooks.Register('character.Gauge','before',function()ui.Button('same') hs.Plugin('hooks-test').record_id() end) return true")=="true");
    ImGui::GetIO().DisplaySize=ImVec2(800,600);ImGui::GetIO().DeltaTime=1.0f/60.0f;
    ImGui::NewFrame();ImGui::Begin("hook host");
    ImGui::Button("same");const auto parent=ImGui::GetItemID();
    CHECK(ui.run("hs.Gauge(1) return true")=="true");
    CHECK(ui.hook_widget_id!=0);
    CHECK(ui.hook_widget_id!=parent);
    CHECK(ImGui::GetID("same")==parent);
    ImGui::End();ImGui::Render();
}

TEST_CASE("UI hooks: idle source unload releases captured data without method calls") {
    HookUi ui;
    ui.rewrite(R"(
      probe={n=0}
      local held={payload=string.rep('x',256*1024)}
      weak=setmetatable({held},{__mode='v'})
      hs.UI.hooks.Register('tasks.Open','before',function()probe.n=probe.n+1 return held end)
    )");
    auto* L=ui.lua->state();
    ui.lua->push_folder_env_for_test(kOwner);lua_getfield(L,-1,"weak");
    lua_setglobal(L,"retired_weak");lua_getfield(L,-1,"probe");lua_setglobal(L,"retired_probe");lua_pop(L,1);
    CHECK(ui.run("return retired_weak[1]~=nil and retired_probe.n==0")=="true");
    ui.lua->set_folders({{kOther,ui.scratch.path()/"other"}});
    ui.frame();
    CHECK(ui.run("collectgarbage('collect') return retired_weak[1]==nil and retired_probe.n==0")=="true");
    CHECK(ui.calls==0);CHECK(ui.actions==0);CHECK(ui.native_calls==0);
}

TEST_CASE("UI hooks: idle host retirement releases data before VM close") {
    HookUi ui;
    CHECK(ui.run(R"(
      retired_probe={n=0}
      local held={payload=string.rep('x',256*1024)}
      retired_weak=setmetatable({held},{__mode='v'})
      hs.UI.hooks.Register('tasks.Open','before',function()retired_probe.n=retired_probe.n+1 return held end)
      return true
    )")=="true");
    ui.api.reset();ui.frame();
    CHECK(ui.run("collectgarbage('collect') return retired_weak[1]==nil and retired_probe.n==0")=="true");
    CHECK(ui.calls==0);CHECK(ui.actions==0);
}

TEST_CASE("UI hooks: maintenance preserves live sources during temporary authority narrowing") {
    HookUi ui;
    CHECK(ui.run("seen=0 hs.UI.hooks.Register('tasks.Open','before',function()seen=seen+1 end) return true",kOwner)=="true");
    ui.caps[kOwner]=kHooks;
    {
        plugin::CallerScope narrower(plugin::kCapRead|plugin::kCapUi,"temporary-native");
        ui.frame();
    }
    ui.caps[kOwner]=plugin::kCapAll;
    CHECK(ui.run("return hs.UI.tasks.Open(1)")=="true");
    CHECK(ui.run("return seen",kOwner)=="1");
    CHECK(ui.actions==1);
}

TEST_CASE("UI hooks: private maintenance is bounded protected and safe before close") {
    HookUi ui;
    auto calls=std::make_shared<int>(0);
    int registered=0;
    for (;registered<16;++registered)
        if (!ui::lua::LuaUi::set_callback_maintenance(ui.lua->state(),"fixture.hooks."+std::to_string(registered),[calls](lua_State*){++*calls;})) break;
    REQUIRE(registered>0);
    CHECK(registered<16); // at least the hook service is present; other core services may share the facility
    CHECK_FALSE(ui::lua::LuaUi::set_callback_maintenance(ui.lua->state(),"overflow",[](lua_State*){}));
    CHECK(ui::lua::LuaUi::set_callback_maintenance(ui.lua->state(),"fixture.hooks.0",[](lua_State* state){luaL_error(state,"maintenance fixture failure");}));
    ui.frame();
    CHECK(*calls>=registered-1);
    CHECK(lua_gettop(ui.lua->state())==0);
    CHECK(ui.calls==0);CHECK(ui.actions==0);
    ui.lua.reset(); // callbacks finish while the state is still alive; the failing service does not stop closure
    CHECK(*calls>=2*(registered-1));
}

TEST_CASE("UI hooks: callback failures preserve original calls and errors without retrying") {
    HookUi ui;
    CHECK(ui.run(R"(
      hs.UI.hooks.Register('tasks.Open','around',function(next,...)
        next(...);error('after original action')
      end)
      return hs.UI.tasks.Open(1)
    )")=="true");
    CHECK(ui.actions==1);
    CHECK(ui.run("return #hs.UI.hooks.List('tasks.Open')")=="0");
    CHECK(ui.run(R"(
      hs.UI.hooks.Register('character.Gauge','before',function()error('before')end)
      hs.UI.hooks.Register('character.Gauge','after',function()error('after')end)
      local r=table.pack(hs.Gauge(5)) return r.n==4 and r[2]==5
    )")=="true");
    CHECK(ui.calls==1);
    CHECK(ui.run(R"(
      hs.UI.hooks.Register('character.Gauge','around',function(next,...) return next(...) end)
      local ok,why=pcall(hs.Gauge,'error') return not ok and why:find('original failed')~=nil
    )")=="true");
    CHECK(ui.calls==2);
    CHECK(ui.run("local ok,e=pcall(hs.Gauge,'error-nil') return not ok and e==nil")=="true");
    CHECK(ui.run("local ok,e=pcall(hs.Gauge,'error-false') return not ok and e==false")=="true");
    CHECK(ui.calls==4);
}

TEST_CASE("UI hooks: unsupported and oversized data preserve exact host values") {
    HookUi ui;
    CHECK(ui.run(R"(
      observed=0
      hs.UI.hooks.Register('character.Gauge','before',function()observed=observed+1 end)
      local input={};input.self=input
      local r=table.pack(hs.Gauge(input))
      return observed==0 and r.n==4 and r[2]==input and r[2].self==input
    )")=="true");
    ui.unusual_results=true;
    CHECK(ui.run(R"(
      hs.UI.hooks.Register('character.Gauge','around',function(next,...)
        local ok=pcall(next,...);return 'must not replace unsupported host results'
      end)
      local r=table.pack(hs.Gauge(string.rep('x',65537)))
      return r.n==4 and #r[2]==65537 and type(r[3])=='userdata'
    )")=="true");
    CHECK(ui.run("local r=table.pack(hs.Gauge('small')) return r.n==4 and r[2]=='small' and type(r[3])=='userdata'")=="true");
    CHECK(ui.calls==3);
    ui.unusual_results=false;ui.oversized_results=true;
    CHECK(ui.run(R"(
      exposed=false
      hs.UI.hooks.Register('character.Gauge','after',function()exposed=true end)
      hs.UI.hooks.Register('character.Gauge','around',function(next,...)
        local ok=pcall(next,...);assert(not ok);return 'wrong'
      end)
      local r=table.pack(hs.Gauge('small'))
      return r.n==4 and r[2]=='small' and #r[3]==65537 and not exposed
    )")=="true");
    CHECK(ui.calls==4);
}

TEST_CASE("UI hooks: error observations are copied while original error identity is preserved") {
    HookUi ui;
    ui.unusual_results=true;
    CHECK(ui.run("local _,_,opaque=hs.Gauge(1) expected_opaque=opaque return true")=="true");
    CHECK(ui.run(R"(
      hs.UI.hooks.Register('character.Gauge','around',function(next,...)
        local ok,why=pcall(next,...);assert(not ok)
        observed_type=type(why)
        if type(why)=='table' then why.changed=true end
        return 'must not replace an original error'
      end)
      local original={message='host error'}
      local ok,why=pcall(hs.Gauge,'error-object',original)
      assert(not ok and why==original and not original.changed and observed_type=='table')
      ok,why=pcall(hs.Gauge,'error-userdata')
      return not ok and why==expected_opaque and observed_type=='string' and #hs.UI.hooks.List()==1
    )")=="true");
    CHECK(ui.calls==3);
}

TEST_CASE("UI hooks: live caller owner and registration ceilings all intersect") {
    HookUi ui;
    ui.caps[kOwner]=kHooks|plugin::kCapCommands;
    CHECK(ui.run("handle=hs.UI.hooks.Register('tasks.Open','around',function(next,...) return next(...) end) return type(handle)",kOwner)=="string");
    ui.caps[kOther]=plugin::kCapUi|plugin::kCapRead;
    CHECK(ui.run("return hs.UI.tasks.Open(1)==nil",kOther)=="true");
    CHECK(ui.actions==0);
    CHECK(ui.run("return hs.UI.tasks.Open(1)")=="true");
    CHECK(ui.actions==1);
    CHECK((ui.action_caps&~(kHooks|plugin::kCapCommands))==0);
    ui.caps[kOwner]=kHooks;
    CHECK(ui.run("return hs.UI.tasks.Open(1)")=="true");
    CHECK(ui.actions==2);
    ui.caps[kOwner]=plugin::kCapAll;
    CHECK(ui.run("return #hs.UI.hooks.List('tasks.Open')",kOwner)=="0");
    {
        plugin::CallerScope reduced(kHooks,kOwner);
        CHECK(ui.run("executed=false restricted=hs.UI.hooks.Register('character.Gauge','before',function() executed=true hs.Plugin('hooks-test').native_host() escaped=hs.UI.tasks.Open(9) end) return type(restricted)",kOwner)=="string");
    }
    CHECK(ui.run("hs.Gauge(1) return true")=="true");
    CHECK(ui.actions==2);
    CHECK(ui.native_calls==0);
    CHECK(ui.run("return escaped==nil and not executed and #hs.UI.hooks.List('character.Gauge')==1",kOwner)=="true");
    ui.caps[kOwner]=kHooks;
    CHECK(ui.run("hs.Gauge(1) return true")=="true");
    CHECK(ui.run("return executed and escaped==nil",kOwner)=="true");
    CHECK(ui.actions==2);
    CHECK(ui.native_calls==1);
}

TEST_CASE("UI hooks: mid-call revocation rejects changed arguments and resumes original once") {
    HookUi ui;
    CHECK(ui.run(R"(
      hs.UI.hooks.Register('tasks.Open','around',function(next,...)
        hs.Plugin('hooks-test').revoke()
        assert(not pcall(next,99))
        return 'revoked replacement'
      end)
      return true
    )",kOwner)=="true");
    CHECK(ui.run("return hs.UI.tasks.Open(7)")=="true");
    CHECK(ui.actions==1);
    CHECK(ui.action_id==7);
    ui.caps[kOwner]=plugin::kCapAll;
    CHECK(ui.run("return #hs.UI.hooks.List('tasks.Open')",kOwner)=="0");
}

TEST_CASE("UI hooks: all capability bits do not lend root UI or foreign owner roles") {
    HookUi ui;
    CHECK(ui.run("root_seen=0 hs.UI.hooks.Register('character.Gauge','before',function()root_seen=root_seen+1 end) return true")=="true");
    CHECK(ui.run("owner_seen=0 hs.UI.hooks.Register('character.Gauge','before',function()owner_seen=owner_seen+1 hs.Plugin('hooks-test').native_host() end) return true",kOwner)=="true");
    CHECK(ui.run("hs.Gauge(1) return true",kOther)=="true");
    CHECK(ui.run("return root_seen")=="0");
    CHECK(ui.run("return owner_seen",kOwner)=="0");
    CHECK(ui.native_calls==0);
    CHECK(ui.run("hs.Gauge(1) return true",kOwner)=="true");
    CHECK(ui.run("return root_seen")=="0");
    CHECK(ui.run("return owner_seen",kOwner)=="1");
    CHECK(ui.run("hs.Gauge(1) return true")=="true");
    CHECK(ui.run("return root_seen")=="1");
    CHECK(ui.run("return owner_seen",kOwner)=="2");
    CHECK(ui.native_calls==2);
}

TEST_CASE("UI hooks: a named native caller without a Lua role is not root UI") {
    HookUi ui;
    CHECK(ui.run("root_seen=0 hs.UI.hooks.Register('character.Gauge','before',function()root_seen=root_seen+1 end) return true")=="true");
    {
        plugin::CallerScope named_native(plugin::kCapAll,"foreign-native");
        REQUIRE(ui::lua::LuaUi::caller_tag().empty());
        CHECK(ui.run("hs.Gauge(1) return true")=="true");
        CHECK(ui.run("return root_seen")=="0");
        CHECK(ui.run("local h=hs.UI.hooks.Register('character.Gauge','before',function()root_seen=-999 end) return h==nil")=="true");
    }
    CHECK(ui.run("hs.Gauge(1) return root_seen")=="1");
    CHECK(ui.calls==2);
}

TEST_CASE("UI hooks: data budget skips retain valid registrations for a later fitting result") {
    HookUi ui;
    ui.oversized_results=true;
    CHECK(ui.run(R"(
      hs.UI.hooks.Register('character.Gauge','around',function(next,...)
        local r=table.pack(pcall(next,...))
        if not r[1] then return 'must not replace a skipped host result' end
        return table.unpack(r,2,r.n)
      end)
      local r=table.pack(hs.Gauge('first'))
      return r.n==4 and #r[3]==65537 and #hs.UI.hooks.List()==1
    )")=="true");
    ui.oversized_results=false;
    CHECK(ui.run("local r=table.pack(hs.Gauge('second')) return r.n==4 and r[2]=='second' and type(r[3])=='table' and #hs.UI.hooks.List()==1")=="true");
    CHECK(ui.calls==2);
}

TEST_CASE("UI hooks: source reload removal and expired host retire captured registrations") {
    HookUi ui;
    ui.rewrite(R"(
      count=0
      oldRegister=hs.UI.hooks.Register
      handle=oldRegister('character.Gauge','before',function()count=count+1 end)
    )");
    CHECK(ui.run("hs.Gauge(1) return true")=="true");
    CHECK(ui.run("return count",kOwner)=="1");
    ui.run("savedRegister=oldRegister",kOwner);
    ui.rewrite("reloaded=true\n");
    CHECK(ui.run("hs.Gauge(1) return true")=="true");
    CHECK(ui.run("return count",kOwner)=="1");
    CHECK(ui.run("return pcall(savedRegister,'character.Gauge','before',function()end)",kOwner)=="false");
    ui.lua->set_folders({{kOther,ui.scratch.path()/"other"}});
    CHECK(ui.run("hs.Gauge(1) return true")=="true");
    CHECK(ui.run("oldAlias=hs.Gauge return true")=="true");
    ui.api.reset();
    CHECK(ui.run("return oldAlias(1)==nil")=="true");
}

TEST_CASE("UI hooks: registration snapshots owner handles limits and bounded recursion") {
    HookUi ui;
    CHECK(ui.run(R"(
      local H=hs.UI.hooks
      n=0
      local h
      h=H.Register('character.Gauge','before',function()
        n=n+1;H.Remove(h)
        H.Register('character.Gauge','before',function()n=n+10 end)
      end)
      hs.Gauge(1);assert(n==1);hs.Gauge(2);assert(n==11)
      for _,r in ipairs(H.List())do H.Remove(r.handle)end
      n=0
      H.Register('character.Gauge','before',function()n=n+1;hs.Gauge(3)end)
      hs.Gauge(4);assert(n==8)
      for _,r in ipairs(H.List())do H.Remove(r.handle)end
      for i=1,16 do assert(H.Register('character.Gauge','before',function()end))end
      local h,why=H.Register('character.Gauge','before',function()end)
      return h==nil and #why>0 and not pcall(H.Register,'character.Gauge','before',function()end,{priority=0/0})
    )")=="true");
    const auto own=ui.run("own=hs.UI.hooks.Register('character.Label','before',function()end) return own",kOwner);
    REQUIRE(own.starts_with("hook:"));
    CHECK(ui.run("return hs.UI.hooks.Remove('"+own+"')",kOther)=="false");
    CHECK(ui.run("return hs.UI.hooks.Remove(own)",kOwner)=="true");
}
