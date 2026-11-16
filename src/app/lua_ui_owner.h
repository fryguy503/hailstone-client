#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "ui/lua/lua_host.h"
#include "plugin/policy.h"

namespace hailstone::app {

// Shared by UI callback services: source generations belong to LuaUi, live permissions to the app.
// The weak host lifetime must be checked before calling a permission query that reaches the host.
struct UiCallbackContext {
    std::weak_ptr<void> lifetime;
    std::function<std::uint32_t(std::string_view)> capabilities;
};

struct OwnedUiCallback {
    ui::lua::LuaUi::CallbackOwner source;
    UiCallbackContext context;
    std::uint32_t ceiling = 0;

    bool alive() const {
        return !context.lifetime.expired() && source.valid && source.valid();
    }

    bool ready(std::uint32_t required) const {
        if (!alive()) return false;
        if ((ceiling & required) != required) return false;
        return source.tag.empty() || (context.capabilities &&
            (context.capabilities(source.tag) & required) == required);
    }
    bool can_register(std::uint32_t required) const {
        if (!ready(required)) return false;
        const auto caller = ui::lua::LuaUi::caller_tag();
        return source.tag.empty() ? caller.empty() && plugin::caller_name().empty() : caller == source.tag;
    }
    // Native UiFns can query their owner's full host table. Until that bridge carries a narrower
    // authority token, do not enter native code when its live authority exceeds this call's ceiling.
    bool ready_for_call(std::uint32_t required) const {
        if (!ready(required)) return false;
        const auto caller = ui::lua::LuaUi::caller_tag();
        const auto effective = ceiling & plugin::caller_caps();
        // Root UI access includes pack-only services that no capability bit grants to an addon.
        if (source.tag.empty()) return caller.empty() && plugin::caller_name().empty() && effective == plugin::kCapAll;
        if (caller.empty() && !plugin::caller_name().empty()) return false;
        if (!caller.empty() && caller != source.tag) return false;
        if (source.tag.starts_with("plugin:"))
            return context.capabilities && (context.capabilities(source.tag) & ~effective) == 0;
        return true;
    }
};

} // namespace hailstone::app
