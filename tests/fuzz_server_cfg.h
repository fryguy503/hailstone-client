#pragma once

// Shared by the deterministic test and libFuzzer: Hailstone's server configuration is untrusted text. Besides
// crash/exception checks, exercise the distinction between disabled rules and an invalid opted-in policy.

#include <algorithm>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/policy.h"

namespace hailstone::fuzz_server_cfg {

inline constexpr std::size_t kMaxInput = 16u << 10;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::logic_error(message);
}

inline void check_rules(const plugin::Rules& rules) {
    const auto& addons = rules.addons;
    for (const auto* identities : {&addons.required, &addons.allow, &addons.block})
        for (const auto& identity : *identities) {
            const auto normalized = plugin::normalize_addon_identity(identity);
            require(normalized && *normalized == identity, "server cfg retained a non-normalized add-on identity");
            require(identity.find('/') != std::string::npos, "server cfg retained a bare add-on name");
            require(!plugin::addon_identity_matches(identity, identity), "a runtime name proved repository identity");
        }

    if (!addons.enabled) {
        require(plugin::addon_allowed(addons, "helper", "maker/helper"), "disabled add-on policy refused an identity");
        require(!plugin::addon_required(addons, "helper", "maker/helper"), "disabled add-on policy required an identity");
        return;
    }
    if (!addons.error.empty()) {
        require(!plugin::addon_allowed(addons, "helper", "maker/helper"), "invalid add-on policy permitted an identity");
        plugin::Policy policy;
        auto invalid = rules;
        const auto hash = sha256(std::string_view("configuration fuzz fixture"));
        invalid.player_script_caps = plugin::kCapAll;
        invalid.server_files.push_back({plugin::FileKind::Script, "scripts/helper/init.lua", hash});
        policy.set_ceilings({{plugin::FileKind::Script, hash, plugin::kCapAll, "helper", "maker/helper"}});
        policy.set(std::move(invalid));
        require(policy.caps(plugin::FileKind::Script, hash, "helper") == 0,
                "invalid add-on policy permitted a registered/server-owned file");
        return;
    }
    for (const auto& identity : addons.block)
        require(!plugin::addon_allowed(addons, "helper", identity), "an explicit add-on block did not win");
}

inline void feed(std::span<const std::byte> input) {
    input = input.first(std::min(input.size(), kMaxInput));
    const std::string_view text = input.empty() ? std::string_view{}
                                               : std::string_view(reinterpret_cast<const char*>(input.data()), input.size());
    std::vector<std::string> warnings;
    check_rules(plugin::parse_server_cfg(text, &warnings));

    // The final section deliberately overrides any earlier enforce value. Unknown policy keys must fail closed
    // once enabled, even after corrupt headers, embedded NULs, comments or repeated sections in the input.
    std::string enabled(text);
    enabled += "\n[addons]\nenforce = on\nfuzz_unknown_policy_key = value\n";
    auto invalid = plugin::parse_server_cfg(enabled);
    require(invalid.addons.enabled && !invalid.addons.error.empty(), "enabled malformed server cfg did not fail closed");
    check_rules(invalid);

    std::string disabled(text);
    disabled += "\n[addons]\nenforce = off\n";
    const auto legacy = plugin::parse_server_cfg(disabled);
    require(!legacy.addons.enabled, "the final disabled add-on section was not honored");
    check_rules(legacy);
}

} // namespace hailstone::fuzz_server_cfg
