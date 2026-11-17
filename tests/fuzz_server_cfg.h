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
        }
    for (const auto& [identity, minimum] : addons.minimum_versions) {
        require(std::find(addons.required.begin(), addons.required.end(), identity) != addons.required.end(),
                "minimum version retained without its required repository");
        const auto parsed = parse_version(minimum.to_string());
        require(parsed && compare_versions(*parsed, minimum) == 0, "server cfg retained an invalid minimum version");
    }

    if (!addons.enabled) {
        require(plugin::addon_allowed(addons, "maker/helper"), "disabled add-on policy refused an identity");
        require(!plugin::addon_required(addons, "maker/helper"), "disabled add-on policy required an identity");
        return;
    }
    if (!addons.error.empty()) {
        require(!plugin::addon_allowed(addons, "maker/helper"), "invalid add-on policy permitted an identity");
        require(!plugin::addon_allowed(addons, ""), "invalid add-on policy permitted a hand-installed add-on");
        plugin::Policy policy;
        auto invalid = rules;
        const auto hash = sha256(std::string_view("configuration fuzz fixture"));
        const auto server = sha256(std::string_view("configuration fuzz server file"));
        invalid.player_script_caps = plugin::kCapAll;
        invalid.server_files.push_back({plugin::FileKind::Script, "scripts/server/init.lua", server});
        policy.set_ceilings({{plugin::FileKind::Script, hash, plugin::kCapAll, "helper", "maker/helper"}});
        policy.set(std::move(invalid));
        require(policy.caps(plugin::FileKind::Script, hash, "helper") == 0,
                "invalid add-on policy permitted a registered add-on file");
        require(policy.caps(plugin::FileKind::Script, server) != 0,
                "an add-on policy refused one of the server's own files");
        return;
    }
    for (const auto& identity : addons.block)
        require(!plugin::addon_allowed(addons, identity), "an explicit add-on block did not win");
    for (const auto& source : rules.suggest_addons) {
        const auto identity = plugin::addon_identity(source);
        require(!identity.empty(), "server cfg retained an invalid suggestion");
        if (std::find(addons.block.begin(), addons.block.end(), identity) == addons.block.end())
            require(plugin::addon_allowed(addons, source), "an unblocked suggestion was not permitted");
    }
    for (const auto* named : {&addons.required, &addons.allow})
        for (const auto& identity : *named) {
            if (!plugin::addon_allowed(addons, identity)) continue;
            auto restricted = rules;
            restricted.player_script_caps = 0;
            restricted.player_plugin_caps = 0;
            const auto hash = sha256(std::string_view("named configuration fuzz fixture"));
            for (const auto kind : {plugin::FileKind::Script, plugin::FileKind::Plugin}) {
                plugin::Policy policy;
                policy.set(restricted);
                policy.set_ceilings({{kind, hash, plugin::kCapRead | plugin::kCapUi, "fixture", identity}});
                require(policy.caps(kind, hash) == (plugin::kCapRead | plugin::kCapUi),
                        "a named managed add-on lost its manifest authorization to general player permissions");
                policy.set_ceilings({{kind, hash, plugin::kCapRead | plugin::kCapUi, "fixture", identity, false}});
                require(policy.caps(kind, hash, "fixture") == 0,
                        "an unverified add-on gained manifest authorization from its identity or runtime name");
            }
        }
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
