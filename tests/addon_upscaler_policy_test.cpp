#include <doctest/doctest.h>

#include <fstream>

#include "app/addon_upscaler_policy.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::app;

namespace {
InstalledAddon upscaler_record(const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "provider.bin", std::ios::binary) << "provider bytes";
    std::ofstream(folder / "runtime.bin", std::ios::binary) << "runtime bytes";
    InstalledAddon addon;
    addon.name = "xess";
    addon.kind = AddonKind::Upscaler;
    addon.source = "owner/hailstone-xess";
    addon.files = {{"provider.bin", 14, sha256("provider bytes")}, {"runtime.bin", 13, sha256("runtime bytes")}};
    return addon;
}
} // namespace

TEST_CASE("server add-on upscalers: legacy servers do not govern providers") {
    plugin::AddonRules rules;
    rules.default_allow = false;
    rules.block = {"owner/hailstone-xess"};
    CHECK(addon_upscaler_problem(rules, {}, "xess", {}).empty());
}

TEST_CASE("server add-on upscalers: allow-only uses the bytes actually loaded") {
    test::ScratchDir dir("addon-upscaler-loaded-copy");
    const auto loaded = dir.path() / "loaded-copy";
    AddonRegistry registry;
    registry.addons.push_back(upscaler_record(loaded));
    plugin::AddonRules rules;
    rules.enabled = true;
    rules.default_allow = false;
    rules.allow = {"owner/hailstone-xess"};
    CHECK(addon_upscaler_problem(rules, registry, "xess", loaded).empty());
    registry.unreadable.push_back({"another", AddonKind::Script});
    CHECK(addon_upscaler_problem(rules, registry, "xess", loaded).empty());
    registry.unreadable.push_back({"xess", AddonKind::Upscaler});
    CHECK(addon_upscaler_problem(rules, registry, "xess", loaded).find("unreadable") != std::string::npos);
    registry.unreadable.clear();
    CHECK_FALSE(addon_upscaler_problem(rules, {}, "xess", loaded).empty());
    CHECK_FALSE(addon_upscaler_problem(rules, registry, "other", loaded).empty());

    // The installed provider may have been updated while the running Vulkan device kept its old copy.
    registry.addons.front().files[1].sha256 = sha256("new runtime");
    CHECK(addon_upscaler_problem(rules, registry, "xess", loaded).find("restart") != std::string::npos);
    registry.addons.front() = upscaler_record(loaded);
    std::ofstream(loaded / "runtime.bin", std::ios::binary) << "changed bytes";
    CHECK_FALSE(addon_upscaler_problem(rules, registry, "xess", loaded).empty());
}

TEST_CASE("server add-on upscalers: blocks win and required providers need a restart") {
    test::ScratchDir dir("addon-upscaler-required");
    AddonRegistry registry;
    registry.addons.push_back(upscaler_record(dir.path()));
    plugin::AddonRules rules;
    rules.enabled = true;
    rules.required = {"owner/hailstone-xess"};
    CHECK(addon_upscaler_problem(rules, registry, "", {}).find("Select it") != std::string::npos);
    CHECK(addon_upscaler_problem(rules, registry, "XESS", dir.path()).empty());
    rules.block = {"owner/hailstone-xess"};
    CHECK(addon_upscaler_problem(rules, registry, "xess", dir.path()).find("does not allow") != std::string::npos);
    rules.error = "conflicting add-on rules";
    CHECK(addon_upscaler_problem(rules, registry, "xess", dir.path()) == rules.error);
}

TEST_CASE("server add-on upscalers: repository exclusions match recorded sources") {
    test::ScratchDir dir("addon-upscaler-blocked-source");
    AddonRegistry registry;
    registry.addons.push_back(upscaler_record(dir.path()));
    plugin::AddonRules rules;
    rules.enabled = true;
    rules.block = {"owner/hailstone-xess"};
    CHECK(addon_upscaler_problem(rules, {}, "another", {}).empty());
    CHECK_FALSE(addon_upscaler_problem(rules, registry, "xess", dir.path()).empty());
    CHECK(addon_upscaler_problem(rules, {}, "", {}).empty()); // built-in rendering stays available
}
