#include "app/lua_ui_api.h"

#include <iterator>
#include <algorithm>

#include <lua.hpp>

#include "plugin/policy.h"
#include "app/lua_ui_hooks.h"

namespace hailstone::app {
namespace {

using plugin::kCapActions;
using plugin::kCapCommands;
using plugin::kCapMove;
using plugin::kCapTarget;

constexpr std::string_view kHookable[] = {
#define UI_METHOD(binding, module, member, caps)
#define UI_CONSTANT(binding, module, member)
#define UI_HOOK_POLICY(binding) binding,
#include "app/lua_ui_api.inc"
#undef UI_HOOK_POLICY
#undef UI_CONSTANT
#undef UI_METHOD
};
constexpr bool hookable(std::string_view binding) {
    for (auto name : kHookable) if (name == binding) return true;
    return false;
}
constexpr UiApiEntry kEntries[] = {
#define UI_METHOD(binding, module, member, caps) {binding, module, member, caps, UiApiKind::Function, hookable(binding)},
#define UI_CONSTANT(binding, module, member) {binding, module, member, 0, UiApiKind::Constant},
#define UI_HOOK_POLICY(binding)
#include "app/lua_ui_api.inc"
#undef UI_CONSTANT
#undef UI_METHOD
#undef UI_HOOK_POLICY
};

constexpr bool valid_catalog() {
    for (std::size_t i = 0; i < std::size(kEntries); ++i) {
        const auto& entry = kEntries[i];
        if (entry.binding.empty() || entry.module.empty() || entry.member.empty()) return false;
        if (entry.module.front() < 'a' || entry.module.front() > 'z') return false;
        if (entry.member.front() < 'A' || entry.member.front() > 'Z') return false;
        if (i && (entry.module < kEntries[i - 1].module ||
                  (entry.module == kEntries[i - 1].module && entry.member <= kEntries[i - 1].member))) return false;
    }
    for (std::size_t i = 0; i < std::size(kHookable); ++i) {
        if (kHookable[i].find('.') != std::string_view::npos) return false;
        bool found = false;
        for (const auto& entry : kEntries)
            found = found || (entry.binding == kHookable[i] && entry.kind == UiApiKind::Function);
        if (!found) return false;
        for (std::size_t j = 0; j < i; ++j) if (kHookable[i] == kHookable[j]) return false;
    }
    return true;
}
static_assert(valid_catalog(), "UI contract entries must have names and unique, sorted module/member pairs");

// No temporary strings cross allocating Lua calls: every name comes from the static catalog or Lua's stack.
void push_string(lua_State* L, std::string_view text) { lua_pushlstring(L, text.data(), text.size()); }

bool path_matches(std::string_view path, std::string_view prefix, std::string_view key) {
    if (prefix.empty()) return path == key;
    return path.size() == prefix.size() + 1 + key.size() && path.starts_with(prefix) &&
           path[prefix.size()] == '.' && path.substr(prefix.size() + 1) == key;
}

const UiApiEntry* find_member(std::string_view module, std::string_view member) {
    for (const auto& entry : kEntries)
        if (entry.module == module && entry.member == member) return &entry;
    return nullptr;
}

bool has_module(std::string_view module) {
    for (const auto& entry : kEntries)
        if (entry.module == module) return true;
    return false;
}

std::string_view checked_string(lua_State* L, int index) {
    std::size_t length = 0;
    const char* data = luaL_checklstring(L, index, &length);
    return {data, length};
}

// Leaves only the value on the stack, for the root's field or one field of its gamepad sub-table.
void push_binding(lua_State* L, int root, std::string_view path) {
    root = lua_absindex(L, root);
    const auto dot = path.find('.');
    if (dot == std::string_view::npos) {
        push_string(L, path);
        lua_rawget(L, root);
        return;
    }
    push_string(L, path.substr(0, dot));
    lua_rawget(L, root);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return;
    }
    push_string(L, path.substr(dot + 1));
    lua_rawget(L, -2);
    lua_remove(L, -2);
}

bool matches_kind(lua_State* L, int index, UiApiKind kind) {
    return kind == UiApiKind::Function ? lua_isfunction(L, index) : lua_isinteger(L, index);
}

void validate_registered(lua_State* L, int table, std::string_view prefix) {
    table = lua_absindex(L, table);
    lua_pushnil(L);
    while (lua_next(L, table)) {
        if (lua_type(L, -2) != LUA_TSTRING)
            luaL_error(L, "UI contract: registered hs keys must be strings");
        std::size_t length = 0;
        const char* key = lua_tolstring(L, -2, &length);
        const std::string_view name{key, length};
        if (prefix.empty() && name == "UI") { // the reserved facade is replaced, never a binding registration
            lua_pop(L, 1);
            continue;
        }
        if (lua_istable(L, -1) && prefix.empty()) {
            validate_registered(L, -1, name);
        } else {
            const UiApiEntry* found = nullptr;
            for (const auto& entry : kEntries) {
                if (!path_matches(entry.binding, prefix, name)) continue;
                if (found) luaL_error(L, "UI contract: duplicate binding field '%s'", key);
                found = &entry;
            }
            if (!found || !matches_kind(L, -1, found->kind))
                luaL_error(L, "UI contract: unlisted or mistyped registered hs field '%s'", key);
        }
        lua_pop(L, 1);
    }
}

int supports(lua_State* L) {
    const auto module = checked_string(L, 1);
    if (lua_isnoneornil(L, 2)) {
        lua_pushboolean(L, has_module(module));
        return 1;
    }
    const auto member = checked_string(L, 2);
    const auto* entry = find_member(module, member);
    if (!entry) {
        lua_pushboolean(L, false);
        return 1;
    }
    push_string(L, module);
    lua_rawget(L, lua_upvalueindex(1));
    if (!lua_istable(L, -1)) {
        lua_pushboolean(L, false);
        return 1;
    }
    push_string(L, member);
    lua_rawget(L, -2);
    lua_pushboolean(L, matches_kind(L, -1, entry->kind));
    return 1;
}

void push_version(lua_State* L) {
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "major");
    lua_pushinteger(L, 7);
    lua_setfield(L, -2, "minor");
}

void push_member_description(lua_State* L, const UiApiEntry& entry) {
    lua_createtable(L, 0, 4);
    push_string(L, entry.member);
    lua_setfield(L, -2, "name");
    push_string(L, entry.binding);
    lua_setfield(L, -2, "binding");
    lua_pushstring(L, entry.kind == UiApiKind::Function ? "function" : "constant");
    lua_setfield(L, -2, "kind");
    lua_pushinteger(L, entry.required_caps);
    lua_setfield(L, -2, "requiredCaps");
    lua_pushboolean(L, entry.hookable);
    lua_setfield(L, -2, "hookable");
    lua_pushstring(L, entry.hookable ? "plain-capabilities" : "closed");
    lua_setfield(L, -2, "hookPolicy");
    lua_pushstring(L, entry.hookable ? "" : "This method has not been approved for hook argument/result exposure");
    lua_setfield(L, -2, "hookReason");
}

void push_module_description(lua_State* L, std::string_view module) {
    lua_createtable(L, 0, 2);
    push_string(L, module);
    lua_setfield(L, -2, "name");
    lua_newtable(L);
    lua_Integer index = 0;
    for (const auto& entry : kEntries) {
        if (entry.module != module) continue;
        push_member_description(L, entry);
        lua_rawseti(L, -2, ++index);
    }
    lua_setfield(L, -2, "methods");
}

int describe(lua_State* L) {
    if (!lua_isnoneornil(L, 1)) {
        const auto module = checked_string(L, 1);
        if (!lua_isnoneornil(L, 2)) {
            const auto* entry = find_member(module, checked_string(L, 2));
            if (entry) push_member_description(L, *entry);
            else lua_pushnil(L);
        } else if (has_module(module)) push_module_description(L, module);
        else lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 2);
    push_version(L);
    lua_setfield(L, -2, "version");
    lua_newtable(L);
    std::string_view previous;
    lua_Integer index = 0;
    for (const auto& entry : kEntries) {
        if (entry.module == previous) continue;
        previous = entry.module;
        push_module_description(L, entry.module);
        lua_rawseti(L, -2, ++index);
    }
    lua_setfield(L, -2, "modules");
    return 1;
}

} // namespace

std::span<const UiApiEntry> ui_api_entries() { return kEntries; }
const UiApiEntry* ui_api_method(std::string_view canonical) {
    for (const auto& entry : kEntries)
        if (entry.kind == UiApiKind::Function && path_matches(canonical, entry.module, entry.member)) return &entry;
    return nullptr;
}

std::optional<std::uint32_t> ui_api_call_caps(std::string_view name) {
    if (name == "UI.Supports" || name == "UI.Describe") return 0;
    for (const auto& entry : kEntries) {
        if (entry.kind != UiApiKind::Function) continue;
        if (name == entry.binding) return entry.required_caps;
        if (name.starts_with("UI.") && path_matches(name.substr(3), entry.module, entry.member))
            return entry.required_caps;
    }
    return std::nullopt;
}

void register_ui_api(lua_State* L) {
    const int root = lua_absindex(L, -1);
    validate_registered(L, root, {});
    lua_newtable(L);
    const int facade = lua_gettop(L);
    for (const auto& entry : kEntries) {
        push_string(L, entry.module);
        lua_rawget(L, facade);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            push_string(L, entry.module);
            lua_pushvalue(L, -2);
            lua_rawset(L, facade);
        }
        const int module = lua_gettop(L);
        push_binding(L, root, entry.binding);
        if (!matches_kind(L, -1, entry.kind))
            luaL_error(L, "UI contract: missing or mistyped hs.%s", entry.binding.data());
        if (entry.hookable) {
            // Publish the same dispatcher under both names. Old references retain the guarded original closure
            // and consult the same VM-local registration registry on every call.
            char canonical[160];
            const auto n = entry.module.size() + 1 + entry.member.size();
            if (n >= sizeof(canonical)) luaL_error(L, "UI hook method name is too long");
            std::copy(entry.module.begin(), entry.module.end(), canonical);
            canonical[entry.module.size()] = '.';
            std::copy(entry.member.begin(), entry.member.end(), canonical + entry.module.size() + 1);
            wrap_ui_hook_method(L, std::string_view{canonical,n});
            lua_pushvalue(L, -1);
            push_string(L, entry.binding);
            lua_insert(L, -2);
            lua_rawset(L, root); // opted-in bindings currently have flat names
        }
        push_string(L, entry.member);
        lua_insert(L, -2);
        lua_rawset(L, module);
        lua_pop(L, 1);
    }
    push_version(L);
    lua_setfield(L, facade, "version");
    lua_pushvalue(L, facade);
    lua_pushcclosure(L, &supports, 1);
    lua_setfield(L, facade, "Supports");
    lua_pushcfunction(L, &describe);
    lua_setfield(L, facade, "Describe");
    lua_setfield(L, root, "UI");
}

} // namespace hailstone::app
