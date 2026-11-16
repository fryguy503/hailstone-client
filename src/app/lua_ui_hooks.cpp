#include "app/lua_ui_hooks.h"

#include <lua.hpp>
#include <algorithm>
#include <utility>
#include <imgui.h>
#include <imgui_internal.h>

#include "app/lua_ui_api.h"
#include "core/log.h"
#include "plugin/lua_keep.h"
#include "plugin/policy.h"

namespace hailstone::app {
namespace {
char kEngine, kContext;
constexpr std::uint32_t kHookCaps = plugin::kCapUi | plugin::kCapRead | plugin::kCapExtend;

UiCallbackContext& context(lua_State* L) {
    return *static_cast<UiCallbackContext*>(lua_touserdata(L, lua_upvalueindex(1)));
}

int capture(lua_State* L) {
    auto& held = plugin::lua_keep<OwnedUiCallback>(L);
    bool captured = false;
    plugin::cxx_guard(L, [&] {
        if (auto owner = ui::lua::LuaUi::callback_owner(L)) {
            held.source = std::move(*owner);
            held.context = lua_isuserdata(L, 1) && lua_gettop(L) > 1
                ? *static_cast<UiCallbackContext*>(lua_touserdata(L, 1)) : context(L);
            held.ceiling = plugin::caller_caps();
            captured = held.can_register(0);
        }
    });
    if (!captured) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, held.source.key.data(), held.source.key.size());
    lua_pushlstring(L, held.source.tag.data(), held.source.tag.size());
    return 3;
}

int ready(lua_State* L) {
    auto* owner = static_cast<OwnedUiCallback*>(lua_touserdata(L, 1));
    const auto needed = static_cast<std::uint32_t>(luaL_checkinteger(L, 2));
    const bool hook = lua_toboolean(L, 3) != 0;
    bool allowed = false;
    plugin::cxx_guard(L, [&] {
        const auto required = needed | (hook ? kHookCaps : plugin::kCapUi);
        allowed = owner && owner->ready(required);
    });
    lua_pushboolean(L, allowed);
    return 1;
}

int same(lua_State* L) {
    const auto* owner = static_cast<OwnedUiCallback*>(lua_touserdata(L, 1));
    lua_pushboolean(L, owner && owner->source.tag == ui::lua::LuaUi::caller_tag());
    return 1;
}
int alive(lua_State* L) {
    const auto* owner = static_cast<OwnedUiCallback*>(lua_touserdata(L,1));
    bool valid=false;
    plugin::cxx_guard(L,[&]{valid=owner&&owner->alive();});
    lua_pushboolean(L,valid);
    return 1;
}
int call_ready(lua_State* L) {
    auto* owner = static_cast<OwnedUiCallback*>(lua_touserdata(L, 1));
    const auto needed = static_cast<std::uint32_t>(luaL_checkinteger(L, 2));
    bool allowed = false;
    plugin::cxx_guard(L, [&] { allowed = owner && owner->ready_for_call(needed | kHookCaps); });
    lua_pushboolean(L, allowed);
    return 1;
}
int live(lua_State* L) {
    const auto* env = static_cast<UiCallbackContext*>(lua_touserdata(L, 1));
    lua_pushboolean(L, env && !env->lifetime.expired());
    return 1;
}

int invoke(lua_State* L) {
    auto* owner = static_cast<OwnedUiCallback*>(lua_touserdata(L, 1));
    const auto needed = static_cast<std::uint32_t>(luaL_checkinteger(L, 2));
    const bool hook = lua_toboolean(L, 3) != 0;
    const char* handle = hook ? luaL_checkstring(L,4) : nullptr;
    luaL_checktype(L, 5, LUA_TFUNCTION);
    bool allowed = false;
    plugin::cxx_guard(L, [&] {
        allowed = owner && (hook ? owner->ready_for_call(needed | kHookCaps) : owner->ready(needed | plugin::kCapUi));
    });
    if (!allowed) { lua_pushnil(L); return 1; }
    lua_remove(L, 2); lua_remove(L, 2); lua_remove(L, 2);
    // Leave the userdata rooted below the protected call: a collector slice must not destroy its C++ owner.
    const int arguments = lua_gettop(L) - 2;
    bool ok = false;
    plugin::cxx_guard(L, [&] {
        plugin::CallerScope ceiling(owner->ceiling, owner->source.tag);
        struct WidgetNamespace {
            ImGuiWindow* window=nullptr;
            int depth=0;
            explicit WidgetNamespace(const char* id) {
                auto* ctx=ImGui::GetCurrentContext();
                if (!id || !ctx || !ctx->WithinFrameScope || !ctx->CurrentWindow) return;
                window=ctx->CurrentWindow;depth=window->IDStack.Size;
                ImGui::PushID("hs.UI.hooks");ImGui::PushID(id);
            }
            ~WidgetNamespace(){if(window && window->IDStack.Size>depth)window->IDStack.resize(depth);}
        } widget_namespace(handle); // below source.invoke's protected stack floor
        ok = owner->source.invoke && owner->source.invoke(L, arguments, LUA_MULTRET);
    });
    const int results = lua_gettop(L) - 1;
    lua_pushboolean(L, ok);
    lua_insert(L, 2);
    return results + 1;
}

int describe(lua_State* L) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    const auto* entry = ui_api_method(std::string_view{text, length});
    lua_createtable(L, 0, 4);
    const bool hookable = entry && entry->hookable;
    lua_pushboolean(L, hookable); lua_setfield(L, -2, "hookable");
    lua_pushinteger(L, entry ? entry->required_caps : 0); lua_setfield(L, -2, "requiredCaps");
    lua_pushstring(L, hookable ? "plain-capabilities" : "closed"); lua_setfield(L, -2, "policy");
    lua_pushstring(L, hookable ? "" : entry ? "This method has not been approved for hook argument/result exposure"
                                                        : "Unknown canonical hs.UI method");
    lua_setfield(L, -2, "reason");
    return 1;
}
int failed(lua_State* L) {
    const char* method = luaL_checkstring(L, 1);
    std::size_t length = 0;
    const char* why = lua_type(L, 2) == LUA_TSTRING ? lua_tolstring(L, 2, &length) : "non-string error";
    if (lua_type(L, 2) != LUA_TSTRING) length = std::char_traits<char>::length(why);
    plugin::cxx_guard(L, [&] { log::warn("UI hook {} retired after callback error: {}", method,
        std::string_view{why,std::min<std::size_t>(length,512)}); });
    return 0;
}

void push_engine(lua_State* L) { lua_rawgetp(L, LUA_REGISTRYINDEX, &kEngine); }
void maintain_hooks(lua_State* L) {
    push_engine(L);
    if (lua_istable(L,-1)) {
        lua_getfield(L,-1,"Maintain");
        if (lua_isfunction(L,-1)) lua_call(L,0,0);
        else lua_pop(L,1);
    }
    lua_pop(L,1);
}
int forward(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}
// handle, why = hs.UiHookRegister("module.Member", phase, callback [, {id,priority}]).
// Before receives copied packed args; after receives copied packed results and args; around receives next,...args.
int UiHookRegister(lua_State* L) {
    if (lua_isfunction(L,lua_upvalueindex(1))) return forward(L);
    lua_pushnil(L);lua_pushliteral(L,"UI hooks require the client UI state");return 2;
}
// removed = hs.UiHookRemove(handle). Only the live registering source owns the handle.
int UiHookRemove(lua_State* L) {
    if (lua_isfunction(L,lua_upvalueindex(1))) return forward(L);
    lua_pushboolean(L,false);return 1;
}
// records = hs.UiHookList(["module.Member"]). Own-source data records; method queries include hookable/policy/reason.
int UiHookList(lua_State* L) {
    if (lua_isfunction(L,lua_upvalueindex(1))) return forward(L);
    if (lua_isnoneornil(L,1)) lua_newtable(L);
    else {
        describe(L);
        lua_pushvalue(L,1);lua_setfield(L,-2,"method");
    }
    lua_pushboolean(L,false);lua_setfield(L,-2,"available");
    lua_pushliteral(L,"UI hooks require the client UI state");lua_setfield(L,-2,"unavailableReason");
    return 1;
}
} // namespace

void register_ui_hooks_api(lua_State* L, const UiCallbackContext& env) {
    const int root = lua_absindex(L, -1);
    // UiApi also serves bare tools/tests, including states with no standard libraries. The marker is
    // state-local: callback_owner can instead refer to an outer UI callback in another VM.
    const bool managed=lua_getfield(L,LUA_REGISTRYINDEX,ui::lua::kPackStateKey)==LUA_TBOOLEAN && lua_toboolean(L,-1);
    lua_pop(L,1);
    if (!managed) {
        lua_pushnil(L);lua_rawsetp(L,LUA_REGISTRYINDEX,&kEngine);
        lua_pushnil(L);lua_rawsetp(L,LUA_REGISTRYINDEX,&kContext);
        const luaL_Reg closed[]={{"UiHookList",UiHookList},{"UiHookRegister",UiHookRegister},{"UiHookRemove",UiHookRemove},{nullptr,nullptr}};
        luaL_setfuncs(L,closed,0);
        return;
    }
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kContext) == LUA_TUSERDATA) {
        plugin::cxx_guard(L, [&] { *static_cast<UiCallbackContext*>(lua_touserdata(L, -1)) = env; });
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
        auto& held = plugin::lua_keep<UiCallbackContext>(L);
        plugin::cxx_guard(L, [&] { held = env; });
        const int holder = lua_gettop(L);
        lua_pushvalue(L, holder); lua_rawsetp(L, LUA_REGISTRYINDEX, &kContext);
        lua_newtable(L);
        const luaL_Reg native[] = {{"capture",capture},{"ready",ready},{"same",same},{"invoke",invoke},
                                  {"describe",describe},{"live",live},{"failed",failed},{"callready",call_ready},
                                  {"alive",alive},{nullptr,nullptr}};
        for (const auto* method = native; method->name; ++method) {
            lua_pushvalue(L, holder); lua_pushcclosure(L, method->func, 1); lua_setfield(L, -2, method->name);
        }
        static constexpr char source[] =
#include "app/lua_ui_hooks_script.inc"
        ;
        if (luaL_loadbufferx(L, source, sizeof(source) - 1, "=hs.UI.hooks", "t") != LUA_OK) lua_error(L);
        lua_insert(L, -2);
        lua_call(L, 1, 1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &kEngine);
        lua_pop(L, 1); // context holder
    }
    if (!ui::lua::LuaUi::set_callback_maintenance(L,"hs.UI.hooks",maintain_hooks)) {
        bool owned=false;
        plugin::cxx_guard(L,[&]{owned=ui::lua::LuaUi::callback_owner(L).has_value();});
        if (owned) luaL_error(L,"UI hook lifecycle maintenance is unavailable");
    }
    push_engine(L);
    const luaL_Reg methods[] = {{"UiHookList", UiHookList}, {"UiHookRegister", UiHookRegister}, {"UiHookRemove", UiHookRemove}, {nullptr,nullptr}};
    constexpr const char* members[] = {"List", "Register", "Remove"};
    for (int i=0; methods[i].name; ++i) {
        lua_getfield(L, -1, members[i]); lua_pushcclosure(L, methods[i].func, 1); lua_setfield(L, root, methods[i].name);
    }
    lua_pop(L, 1);
}

void wrap_ui_hook_method(lua_State* L, std::string_view method) {
    push_engine(L);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, "Wrap"); lua_remove(L, -2);
    lua_insert(L, -2);
    lua_pushlstring(L, method.data(), method.size());
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kContext);
    auto& held = plugin::lua_keep<UiCallbackContext>(L);
    plugin::cxx_guard(L, [&] { held = *static_cast<UiCallbackContext*>(lua_touserdata(L, -2)); });
    lua_remove(L, -2);
    lua_call(L, 3, 1);
}
} // namespace hailstone::app
