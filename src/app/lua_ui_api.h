#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

struct lua_State;

namespace hailstone::app {

enum class UiApiKind { Function, Constant };

struct UiApiEntry {
    std::string_view binding;
    std::string_view module;
    std::string_view member;
    std::uint32_t required_caps;
    UiApiKind kind;
    bool hookable = false; // explicit argument/result exposure audit, independent of the capability mask
};

// The same catalog owns the old capability names and the versioned UI contract. Entries are sorted by
// module/member; the aliases keep the exact existing closure, including its upvalues and capability gate.
std::span<const UiApiEntry> ui_api_entries();
std::optional<std::uint32_t> ui_api_call_caps(std::string_view name);
const UiApiEntry* ui_api_method(std::string_view canonical);

// The hs table is on top of the stack. Call after gate_ui_calls: aliases must refer to the final guarded closure,
// never to a raw binding. Checks that the catalog and the registered functions/constants match before publishing.
void register_ui_api(lua_State* L);

} // namespace hailstone::app
