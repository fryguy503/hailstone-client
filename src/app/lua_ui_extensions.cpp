#include "app/lua_ui_extensions.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "plugin/lua_keep.h"
#include "plugin/lua_value.h"
#include "plugin/policy.h"

namespace hailstone::app {
namespace {
constexpr std::uint32_t kFloor = plugin::kCapRead | plugin::kCapUi;
constexpr std::size_t kMaximum = 128, kPerOwner = 32;
constexpr char kRegistry = 0;
std::atomic<std::uint64_t> gGeneration = 0;

struct Entry {
    OwnedUiCallback owner;
    std::string handle, window, section, id, label, placement;
    std::uint32_t required = kFloor;
    int callback = 0, priority = 0;
    bool action = false, selection = false, review = false, active = true;
};
struct Registry {
    UiCallbackContext context;
    std::vector<std::shared_ptr<Entry>> entries;
    std::uint64_t generation = ++gGeneration, serial = 0;
    int dispatching = 0;
};
struct Call {
    std::shared_ptr<Entry> entry;
    std::vector<std::shared_ptr<Entry>> entries;
    plugin::UiValue context, response;
    std::string error;
};
struct Invocation { Registry* registry = nullptr; Entry* entry = nullptr; int results = 0; bool ok = false, drawing = false; };

Registry& registry(lua_State* L) { return *static_cast<Registry*>(lua_touserdata(L, lua_upvalueindex(1))); }
void raw_field(lua_State* L, int at, const char* name) {
    at = lua_absindex(L, at); lua_pushstring(L, name); lua_rawget(L, at);
}
std::string_view text(lua_State* L, int at, std::size_t limit = 128) {
    std::size_t length = 0; const char* value = luaL_checklstring(L, at, &length);
    if (!length || length > limit) luaL_argerror(L, at, "nonempty bounded string expected");
    for (std::size_t i = 0; i < length; ++i)
        if (static_cast<unsigned char>(value[i]) < 32 || value[i] == 127) luaL_argerror(L, at, "control characters are not allowed");
    return {value, length};
}
bool point(std::string_view window, std::string_view section) {
    const auto identifier = [](std::string_view value) {
        return !value.empty() && value.size() <= 128 && std::ranges::all_of(value, [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '.' || c == ':' || c == '-';
        });
    };
    return identifier(window) && identifier(section);
}
bool visible(const Entry& e) {
    const auto tag = ui::lua::LuaUi::caller_tag();
    return (tag.empty() && plugin::caller_name().empty()) || (!tag.empty() && tag == e.owner.source.tag);
}
bool ready(lua_State* L, const Entry& e) {
    bool permitted = false;
    plugin::cxx_guard(L, [&] { permitted = e.active && visible(e) && e.owner.ready_for_call(e.required); });
    return permitted;
}
void retire(lua_State* L, Entry& e, int state = lua_upvalueindex(1)) {
    e.active = false;
    if (e.callback) {
        lua_getiuservalue(L, state, 1);
        lua_pushnil(L); lua_rawseti(L, -2, e.callback); lua_pop(L, 1); e.callback = 0;
    }
}
void maintain(lua_State* L) {
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kRegistry) == LUA_TUSERDATA) {
        auto& r = *static_cast<Registry*>(lua_touserdata(L, -1));
        const int state = lua_gettop(L);
        for (const auto& e : r.entries)
            if (!e->active || !e->owner.alive()) retire(L, *e, state);
        std::erase_if(r.entries, [](const auto& e) { return !e->active; });
    }
    lua_pop(L, 1);
}
void prune(lua_State* L, Registry& r) {
    for (const auto& e : r.entries) {
        const bool expanded = e->owner.source.tag.starts_with("plugin:") && !e->owner.context.lifetime.expired() &&
            e->owner.context.capabilities && (e->owner.context.capabilities(e->owner.source.tag) & ~e->owner.ceiling) != 0;
        if (!e->active || !e->owner.ready(plugin::kCapUi) || expanded) retire(L, *e);
    }
    std::erase_if(r.entries, [](const auto& e) { return !e->active; });
}
Entry* find(Registry& r, std::string_view handle) {
    for (const auto& e : r.entries) if (e->active && e->handle == handle) return e.get();
    return nullptr;
}
void ordered(Call& call, Registry& r, std::string_view window, std::string_view section, bool action, std::string_view placement = {}) {
    for (const auto& e : r.entries)
        if (e->active && e->action == action && e->window == window && e->section == section &&
            (action || placement.empty() || e->placement == placement))
            call.entries.push_back(e);
    std::ranges::sort(call.entries, [](const auto& a, const auto& b) {
        if (a->priority != b->priority) return a->priority < b->priority;
        if (a->owner.source.tag != b->owner.source.tag) return a->owner.source.tag < b->owner.source.tag;
        if (a->id != b->id) return a->id < b->id;
        return a->handle < b->handle;
    });
}
bool selection(lua_State* L, int at) {
    if (!lua_istable(L, at)) return false;
    raw_field(L, at, "selection");
    bool selected = false;
    if (lua_istable(L, -1)) {
        raw_field(L, -1, "name");
        std::size_t length = 0; const char* name = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &length) : nullptr;
        selected = name && length > 0 && length <= 256;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return selected;
}
void field(lua_State* L, const char* name, std::string_view value) {
    lua_pushlstring(L, value.data(), value.size()); lua_setfield(L, -2, name);
}
void flag(lua_State* L, const char* name, bool value) { lua_pushboolean(L, value); lua_setfield(L, -2, name); }
int response(lua_State* L, bool ok, std::string_view message, std::string_view token = {}) {
    lua_createtable(L, 0, 3); flag(L, "ok", ok); field(L, "message", message);
    if (!token.empty()) field(L, "reviewToken", token);
    return 1;
}
bool context(lua_State* L, int at, Call& call) {
    if (!lua_istable(L, at)) { call.error = "A data context table is required"; return false; }
    ui::lua::unview(L, at);
    auto value = plugin::to_value_checked(L, at, &call.error);
    if (!value) return false;
    call.context = std::move(*value);
    return true;
}
bool invoke(lua_State* L, Registry& r, Entry& e, int args, int results, bool drawing = false) {
    if (!e.active || !visible(e) || r.dispatching >= 8) { lua_pop(L, args + 1); return false; }
    const int at = lua_gettop(L) - args;
    auto& call = plugin::lua_keep<Invocation>(L);
    call.registry = &r; call.entry = &e; call.results = results;
    call.drawing = drawing || (GImGui && GImGui->WithinFrameScope && GImGui->CurrentWindow);
    lua_insert(L, at); // keep the box rooted outside the protected invocation
    lua_pushvalue(L, at);
    lua_pushcclosure(L, [](lua_State* state) -> int {
        auto& invocation = *static_cast<Invocation*>(lua_touserdata(state, lua_upvalueindex(1)));
        plugin::cxx_guard(state, [&] {
            auto& entry = *invocation.entry;
            if (!entry.active || !visible(entry) || !entry.owner.ready_for_call(entry.required)) return;
            struct Depth { int& value; explicit Depth(int& v) : value(v) { ++value; } ~Depth() { --value; } } depth(invocation.registry->dispatching);
            plugin::CallerScope scope(entry.owner.ceiling, entry.owner.source.tag);
            struct WidgetIds {
                bool active;
                WidgetIds(bool on, const char* id) : active(on) { if(active) ImGui::PushID(id); }
                ~WidgetIds() { if(active) ImGui::PopID(); }
            } ids(invocation.drawing, entry.handle.c_str());
            invocation.ok = entry.owner.source.invoke(state, lua_gettop(state) - 1, invocation.results);
        });
        return invocation.ok ? invocation.results : 0;
    }, 1);
    lua_insert(L, at + 1);
    const int status = lua_pcall(L, args + 1, results, 0);
    const bool ok = status == LUA_OK && call.ok;
    lua_remove(L, at);
    if (!ok) lua_pop(L, status == LUA_OK ? results : 1);
    // The snapshot survives self-removal; a retired callback cannot publish its response.
    if (!ok) retire(L, e);
    if (!ready(L, e)) { if (ok) lua_pop(L, results); return false; }
    return ok;
}

int Register(lua_State* L, bool action) {
    const auto window = text(L, 1), section = text(L, 2);
    if (!point(window, section)) return luaL_error(L, "Window and section must be bounded ASCII identifiers");
    const int idAt = action ? 3 : 4, optionsAt = action ? 5 : 6, callbackAt = action ? 6 : 5;
    const auto id = text(L, idAt);
    std::string_view label, placement;
    if (action) label = text(L, 4, 256);
    else { placement = text(L, 3); if (placement != "before" && placement != "after" && placement != "replace") return luaL_error(L, "Expected before, after or replace"); }
    luaL_checktype(L, callbackAt, LUA_TFUNCTION);
    if (!lua_isnoneornil(L, optionsAt)) { ui::lua::unview(L, optionsAt); luaL_checktype(L, optionsAt, LUA_TTABLE); }
    auto& call = plugin::lua_keep<Call>(L);
    auto& r = registry(L);
    plugin::cxx_guard(L, [&] {
        prune(L, r); call.entry = std::make_shared<Entry>();
        auto owner = ui::lua::LuaUi::callback_owner(L);
        if (!owner) { call.error = "Window extensions require a live managed UI source"; return; }
        call.entry->owner = {*owner, r.context, plugin::caller_caps()};
        call.entry->window = window; call.entry->section = section; call.entry->id = id;
        call.entry->label = label; call.entry->placement = placement; call.entry->action = action;
    });
    if (!call.error.empty()) { lua_pushnil(L); lua_pushlstring(L, call.error.data(), call.error.size()); return 2; }
    Entry& e = *call.entry;
    if (lua_istable(L, optionsAt)) {
        for (const auto& item : {std::pair{"requiresSelection", &e.selection}, std::pair{"requiresReview", &e.review}}) {
            raw_field(L, optionsAt, item.first);
            if (!lua_isnil(L, -1) && !lua_isboolean(L, -1)) return luaL_error(L, "Extension flags must be booleans");
            *item.second = lua_toboolean(L, -1); lua_pop(L, 1);
        }
        raw_field(L, optionsAt, "priority");
        const auto priority = luaL_optinteger(L, -1, 0); lua_pop(L, 1);
        if (priority < -10000 || priority > 10000) return luaL_error(L, "Extension priority is out of range");
        e.priority = static_cast<int>(priority);
        raw_field(L, optionsAt, "requiredCaps");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TTABLE);
            const auto count = lua_rawlen(L, -1);
            if (count > 13) return luaL_error(L, "Too many required capabilities");
            for (std::size_t i = 1; i <= count; ++i) {
                lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
                const auto name = text(L, -1, 32);
                std::optional<std::uint32_t> caps;
                plugin::cxx_guard(L, [&] { caps = plugin::parse_caps_strict(name); });
                if (!caps || !*caps || (*caps & (*caps - 1)) ||
                    std::ranges::any_of(name, [](char c) { return c < 'a' || c > 'z'; })) return luaL_error(L, "Unknown required capability");
                e.required |= *caps; lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    bool registration = false;
    plugin::cxx_guard(L, [&] { registration = e.owner.can_register(e.required) && e.owner.ready_for_call(e.required); });
    if (!registration) {
        lua_pushnil(L); lua_pushliteral(L, "Registration permissions are unavailable"); return 2;
    }
    plugin::cxx_guard(L, [&] {
        for (const auto& previous : r.entries)
            if (previous->owner.source.tag == e.owner.source.tag && previous->window == window && previous->section == section &&
                previous->id == id && previous->action == action && previous->placement == placement) retire(L, *previous);
        prune(L, r);
        const auto own = std::ranges::count_if(r.entries, [&](const auto& item) { return item->owner.source.tag == e.owner.source.tag; });
        if (r.entries.size() >= kMaximum || own >= static_cast<std::ptrdiff_t>(kPerOwner)) { call.error = "Window extension registration limit reached"; return; }
        e.handle = "extension:" + std::to_string(r.generation) + ":" + std::to_string(++r.serial);
        for (int slot = 1; slot <= static_cast<int>(kMaximum); ++slot)
            if (std::ranges::none_of(r.entries, [&](const auto& item) { return item->callback == slot; })) { e.callback = slot; break; }
    });
    if (!call.error.empty()) { lua_pushnil(L); lua_pushlstring(L, call.error.data(), call.error.size()); return 2; }
    lua_getiuservalue(L, lua_upvalueindex(1), 1);
    lua_pushvalue(L, callbackAt); lua_rawseti(L, -2, e.callback); lua_pop(L, 1);
    r.entries.push_back(call.entry); // capacity is reserved; the callback table has a bounded preallocated array
    lua_pushlstring(L, e.handle.data(), e.handle.size()); return 1;
}
int RegisterAction(lua_State* L) { return Register(L, true); }
int RegisterSection(lua_State* L) { return Register(L, false); }
int IsRegistered(lua_State* L) {
    const auto handle = text(L, 1); auto& r = registry(L); plugin::cxx_guard(L, [&] { prune(L, r); });
    const auto* e = find(r, handle); lua_pushboolean(L, e && visible(*e)); return 1;
}
int Remove(lua_State* L) {
    const auto handle = text(L, 1); auto& r = registry(L); plugin::cxx_guard(L, [&] { prune(L, r); });
    auto* e = find(r, handle);
    const bool own = e && visible(*e) && e->owner.source.tag == ui::lua::LuaUi::caller_tag();
    if (own) retire(L, *e);
    lua_pushboolean(L, own); return 1;
}
int ListActions(lua_State* L) {
    const auto window = text(L, 1), section = text(L, 2);
    if (!point(window, section)) return luaL_error(L, "Window and section must be bounded ASCII identifiers");
    const bool selected = selection(L, 3);
    auto& r = registry(L); auto& call = plugin::lua_keep<Call>(L);
    plugin::cxx_guard(L, [&] { prune(L, r); ordered(call, r, window, section, true); });
    lua_createtable(L, static_cast<int>(call.entries.size()), 0); int i = 0;
    for (const auto& e : call.entries) {
        if (!visible(*e)) continue;
        lua_createtable(L, 0, 8); field(L, "handle", e->handle); field(L, "owner", e->owner.source.tag);
        field(L, "id", e->id); field(L, "label", e->label);
        flag(L, "enabled", ready(L, *e) && (!e->selection || selected));
        flag(L, "requiresSelection", e->selection); flag(L, "requiresReview", e->review);
        lua_pushinteger(L, e->priority); lua_setfield(L, -2, "priority"); lua_rawseti(L, -2, ++i);
    }
    call.entries.clear();
    plugin::cxx_guard(L, [&] { ordered(call, r, window, section, false); });
    lua_createtable(L, static_cast<int>(call.entries.size()), 0); i = 0;
    for (const auto& e : call.entries) {
        if (!visible(*e)) continue;
        lua_createtable(L, 0, 6); field(L, "handle", e->handle); field(L, "owner", e->owner.source.tag);
        field(L, "id", e->id); field(L, "placement", e->placement); flag(L, "enabled", ready(L, *e));
        lua_pushinteger(L, e->priority); lua_setfield(L, -2, "priority"); lua_rawseti(L, -2, ++i);
    }
    lua_setfield(L, -2, "sections");
    return 1;
}
int InvokeAction(lua_State* L) {
    const auto handle = text(L, 1), phase = text(L, 2, 16);
    const bool selected = selection(L, 3);
    std::size_t tokenLength = 0;
    const char* token = luaL_optlstring(L, 4, "", &tokenLength);
    if (tokenLength > 1024) return luaL_error(L, "Review token is too large");
    auto& r = registry(L); auto& call = plugin::lua_keep<Call>(L);
    plugin::cxx_guard(L, [&] {
        prune(L, r);
        for (const auto& e : r.entries) if (e->handle == handle && e->active && e->action) call.entry = e;
        if (call.entry) context(L, 3, call);
    });
    if (!call.entry || !call.error.empty()) return response(L, false, call.error.empty() ? "Action is no longer registered" : call.error);
    Entry& e = *call.entry;
    if (!ready(L, e)) return response(L, false, "Action permissions are unavailable");
    if (e.selection && !selected && phase != "cancel") return response(L, false, "Select a current member first");
    if ((!e.review && phase != "invoke") || (e.review && phase != "review" && phase != "confirm" && phase != "cancel") ||
        (e.review && phase != "review" && !tokenLength)) return response(L, false, "Action phase or review token is invalid");
    lua_getiuservalue(L, lua_upvalueindex(1), 1); lua_rawgeti(L, -1, e.callback); lua_remove(L, -2);
    lua_pushlstring(L, e.window.data(), e.window.size()); lua_pushlstring(L, e.id.data(), e.id.size());
    lua_pushlstring(L, phase.data(), phase.size()); plugin::push_value(L, call.context); lua_pushlstring(L, token, tokenLength);
    if (!invoke(L, r, e, 5, 1)) return response(L, false, "Action callback failed or retired");
    plugin::cxx_guard(L, [&] { call.response = plugin::to_value(L, -1); }); lua_pop(L, 1);
    const auto* ok = call.response.field("ok"), *message = call.response.field("message"), *review = call.response.field("reviewToken");
    if (!ok || ok->kind != plugin::UiValue::Kind::Bool || (message && message->kind != plugin::UiValue::Kind::String) ||
        (review && review->kind != plugin::UiValue::Kind::String)) return response(L, false, "Action callback returned an invalid response");
    const std::string_view note = message ? message->s : std::string_view{}, reviewToken = review ? review->s : std::string_view{};
    if (note.size() > 4096 || reviewToken.size() > 1024 || (ok->b && phase == "review" && (note.empty() || reviewToken.empty())))
        return response(L, false, "Action callback returned an invalid review");
    return response(L, ok->b, note, reviewToken);
}
int DrawSection(lua_State* L) {
    const auto window = text(L, 1), section = text(L, 2), placement = text(L, 3, 16);
    if (!point(window, section) || (placement != "before" && placement != "after" && placement != "replace")) return luaL_error(L, "Invalid display point or placement");
    const bool replacement = placement == "replace";
    if (!ImGui::GetCurrentContext() || !GImGui->WithinFrameScope || !GImGui->CurrentWindow) {
        if (replacement) lua_pushboolean(L, false); else lua_pushinteger(L, 0);
        return 1;
    }
    auto& r = registry(L); auto& call = plugin::lua_keep<Call>(L);
    plugin::cxx_guard(L, [&] { prune(L, r); context(L, 4, call); ordered(call, r, window, section, false, placement); });
    int count = 0;
    if (call.error.empty()) for (const auto& e : call.entries) {
        if (!ready(L, *e)) continue;
        lua_getiuservalue(L, lua_upvalueindex(1), 1); lua_rawgeti(L, -1, e->callback); lua_remove(L, -2);
        lua_pushlstring(L, window.data(), window.size()); lua_pushlstring(L, section.data(), section.size());
        lua_pushlstring(L, placement.data(), placement.size()); plugin::push_value(L, call.context);
        if (invoke(L, r, *e, 4, 0, true)) ++count;
        if (replacement) break; // one eligible winner; failure restores the presenter's default, not another addon
    }
    if (replacement) lua_pushboolean(L, count != 0); else lua_pushinteger(L, count);
    return 1;
}
} // namespace

void register_ui_extensions(lua_State* L, const UiCallbackContext& context) {
    const int root = lua_absindex(L, -1);
    Registry* data = nullptr;
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kRegistry) == LUA_TUSERDATA) {
        auto* old = static_cast<Registry*>(lua_touserdata(L, -1));
        if (!old->context.lifetime.owner_before(context.lifetime) && !context.lifetime.owner_before(old->context.lifetime)) data = old;
    }
    if (!data) {
        lua_pop(L, 1);
        if (luaL_newmetatable(L, "hailstone.ui.extensions")) {
            lua_pushcfunction(L, [](lua_State* state) -> int { static_cast<Registry*>(lua_touserdata(state, 1))->~Registry(); return 0; });
            lua_setfield(L, -2, "__gc");
        }
        data = new (lua_newuserdatauv(L, sizeof(Registry), 1)) Registry;
        lua_pushvalue(L, -2); lua_setmetatable(L, -2); lua_remove(L, -2);
        plugin::cxx_guard(L, [&] { data->context = context; data->entries.reserve(kMaximum); });
        lua_createtable(L, static_cast<int>(kMaximum), 0); lua_setiuservalue(L, -2, 1);
        lua_pushvalue(L, -1); lua_rawsetp(L, LUA_REGISTRYINDEX, &kRegistry);
    }
    const int state = lua_absindex(L, -1);
    for (const auto& binding : std::initializer_list<std::pair<const char*, lua_CFunction>>{
            {"ExtensionDrawSection", &DrawSection}, {"ExtensionInvokeAction", &InvokeAction},
            {"ExtensionIsRegistered", &IsRegistered}, {"ExtensionListActions", &ListActions}, {"ExtensionRegisterAction", &RegisterAction},
            {"ExtensionRegisterSection", &RegisterSection}, {"ExtensionRemove", &Remove}}) {
        lua_pushvalue(L, state); lua_pushcclosure(L, binding.second, 1); lua_setfield(L, root, binding.first);
    }
    lua_pop(L, 1);
    static const ui::lua::LuaUi::CallbackMaintenance cleanup = maintain;
    ui::lua::LuaUi::set_callback_maintenance(L, "hs.UI.extensions", cleanup);
}
} // namespace hailstone::app
