#include <doctest/doctest.h>

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/addon_policy_check.h"
#include "app/update_install.h"
#include "plugin/consent.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

void put(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}

app::InstalledAddon install_record(const fs::path& data, std::string name, AddonKind kind, std::string source,
                                   const std::string& file, const std::string& bytes) {
    app::InstalledAddon addon;
    addon.name = std::move(name);
    addon.kind = kind;
    addon.source = std::move(source);
    addon.version = "1.0.0";
    addon.caps = kind == AddonKind::Upscaler ? 0u : static_cast<std::uint32_t>(plugin::kCapRead);
    addon.files.push_back({file, static_cast<std::uint64_t>(bytes.size()), sha256(bytes)});
    put(app::addon_folder(data, kind, addon.name) / file, bytes);
    return addon;
}

app::AddonPolicyCheckRequest request(const fs::path& data, const fs::path& profile, const std::string& entries) {
    app::AddonPolicyCheckRequest out;
    out.data_dir = data;
    out.platform = "windows-x64";
    out.rules = plugin::parse_server_cfg("[scripts]\nplayer = on\nplayer_caps = read\n"
                                         "[plugins]\nplayer = on\nplayer_caps = read\n"
                                         "[addons]\nenforce = on\ndefault = deny\n" + entries);
    out.rules.dir = profile;
    return out;
}

} // namespace

TEST_CASE("addon policy check: managed requirements verify every file and publish extra-code provenance") {
    test::ScratchDir scratch("addon_policy_worker_integrity");
    const auto data = scratch.path() / "data";
    auto addon = install_record(data, "helper", AddonKind::Script, "maker/helper", "init.lua", "-- entry");
    const std::string module = "-- module";
    addon.files.push_back({"lib/module.lua", static_cast<std::uint64_t>(module.size()), sha256(module)});
    put(app::addon_folder(data, addon.kind, addon.name) / "lib/module.lua", module);
    REQUIRE(app::write_installed_addons(data, {addon}));
    const auto input = request(data, scratch.path() / "profile", "required = maker/helper\n");
    auto checked = app::check_addon_policy(input);
    CHECK_FALSE(checked.canceled);
    CHECK(checked.ceilings_ready);
    CHECK(checked.problem.empty());
    CHECK(checked.problems.empty());
    REQUIRE(checked.ceilings.size() == 2);
    CHECK(std::all_of(checked.ceilings.begin(), checked.ceilings.end(), [](const auto& ceiling) {
        return ceiling.verified && ceiling.addon_source == "maker/helper";
    }));
    plugin::Policy policy;
    policy.set(input.rules);
    policy.set_ceilings(checked.ceilings);
    const auto required = policy.required_entries(plugin::FileKind::Script);
    REQUIRE(required.size() == 1);
    CHECK(required[0].first == "helper");
    CHECK(required[0].second == addon.files[0].sha256);

    const std::string edited = "-- changed module";
    const std::string extra = "-- extra module";
    put(app::addon_folder(data, addon.kind, addon.name) / "lib/module.lua", edited);
    put(app::addon_folder(data, addon.kind, addon.name) / "extra.lua", extra);
    checked = app::check_addon_policy(input);
    REQUIRE(checked.problems.size() == 1);
    CHECK(checked.problem.find("missing or changed") != std::string::npos);
    for (const auto& bytes : {edited, extra}) {
        const auto found = std::find_if(checked.ceilings.begin(), checked.ceilings.end(), [&](const auto& ceiling) {
            return ceiling.sha256 == sha256(bytes);
        });
        REQUIRE(found != checked.ceilings.end());
        CHECK_FALSE(found->verified);
        CHECK(found->addon_source == "maker/helper");
    }
    fs::remove(app::addon_folder(data, addon.kind, addon.name) / "init.lua");
    CHECK_FALSE(app::check_addon_policy(input).problem.empty());
}

TEST_CASE("addon policy check: a registry snapshot keeps required identities when another client changes the registry") {
    test::ScratchDir scratch("addon_policy_worker_snapshot");
    const auto data = scratch.path() / "data";
    const auto addon = install_record(data, "helper", AddonKind::Script, "maker/helper", "init.lua", "-- required");
    REQUIRE(app::write_installed_addons(data, {addon}));
    const auto snapshot = app::read_addon_registry(data);
    REQUIRE(snapshot.addons.size() == 1);
    const auto input = request(data, scratch.path() / "profile", "required = maker/helper\n");
    SUBCASE("the current registry no longer lists the add-on") {
        REQUIRE(app::write_installed_addons(data, {}));
    }
    SUBCASE("the registry and its backup have gone") {
        REQUIRE(fs::remove(app::addons_dir(data) / "installed.txt"));
        REQUIRE(fs::remove(app::addons_dir(data) / "installed.bak.txt"));
    }
    REQUIRE(app::read_addon_registry(data).addons.empty());
    plugin::Policy policy;
    policy.set(input.rules);
    policy.set_ceilings(app::addon_ceilings(data, snapshot, true));
    const auto required = policy.required_entries(plugin::FileKind::Script);
    REQUIRE(required.size() == 1);
    CHECK(required[0].first == "helper");
    CHECK(required[0].second == addon.files[0].sha256);
    // A fresh job still sees the new registry and refuses; only a job that already took the snapshot retains it.
    CHECK(app::check_addon_policy(input).problem.find("not installed") != std::string::npos);
}

TEST_CASE("addon policy check: repository identity and current-platform entry points are required") {
    test::ScratchDir scratch("addon_policy_worker_entry");
    const auto data = scratch.path() / "data";
    auto addon = install_record(data, "helper", AddonKind::Script, "other/helper", "init.lua", "-- entry");
    REQUIRE(app::write_installed_addons(data, {addon}));
    auto input = request(data, scratch.path() / "profile", "required = maker/helper\n");
    CHECK(app::check_addon_policy(input).problem.find("not installed") != std::string::npos);
    addon.source = "maker/helper";
    addon.files[0].path = "support.lua";
    put(app::addon_folder(data, addon.kind, addon.name) / "support.lua", "-- entry");
    REQUIRE(app::write_installed_addons(data, {addon}));
    CHECK(app::check_addon_policy(input).problem.find("missing or changed") != std::string::npos);

    addon = install_record(data, "helper", AddonKind::Plugin, "maker/helper", "helper.so", "other platform");
    REQUIRE(app::write_installed_addons(data, {addon}));
    CHECK(app::check_addon_policy(input).problem.find("missing or changed") != std::string::npos);
    addon = install_record(data, "helper", AddonKind::Plugin, "maker/helper", "helper.dll", "native bytes");
    REQUIRE(app::write_installed_addons(data, {addon}));
    CHECK(app::check_addon_policy(input).problem.empty()); // execution still waits for the runtime consent prompt
    fs::create_directories(input.rules.dir);
    REQUIRE(plugin::set_plugin_consent(input.rules.dir, addon.name, addon.files[0].sha256, false));
    CHECK(app::check_addon_policy(input).problem.find("not allowed") != std::string::npos);
}

TEST_CASE("addon policy check: external providers use the running copy and remain explicitly allowlisted") {
    test::ScratchDir scratch("addon_policy_worker_upscaler");
    const auto data = scratch.path() / "data";
    const auto addon = install_record(data, "provider", AddonKind::Upscaler, "maker/provider", "provider.dll", "provider");
    REQUIRE(app::write_installed_addons(data, {addon}));
    auto input = request(data, scratch.path() / "profile", "allow = maker/provider\n");
    input.active_upscaler = "provider";
    input.loaded_upscaler_folder = scratch.path() / "loaded";
    put(input.loaded_upscaler_folder / "provider.dll", "provider");
    CHECK(app::check_addon_policy(input).problem.empty());
    put(input.loaded_upscaler_folder / "provider.dll", "edited");
    CHECK(app::check_addon_policy(input).problem.find("does not match") != std::string::npos);
    input.rules.addons.allow.clear();
    CHECK(app::check_addon_policy(input).problem.find("does not allow") != std::string::npos);
    input.rules.addons.enabled = false;
    CHECK(app::check_addon_policy(input).problem.empty());
}

TEST_CASE("addon policy check: Linux plugin identities preserve the listed filesystem case") {
    test::ScratchDir scratch("addon_policy_worker_linux_case");
    const auto data = scratch.path() / "data";
    auto addon = install_record(data, "helper", AddonKind::Plugin, "maker/helper", "libHeLpEr.SO", "native bytes");
    REQUIRE(app::write_installed_addons(data, {addon}));
    auto input = request(data, scratch.path() / "profile", "required = maker/helper\n");
    input.platform = "linux-x64";
    auto checked = app::check_addon_policy(input);
    CHECK(checked.problem.empty());
    REQUIRE(checked.ceilings.size() == 1);
    CHECK(checked.ceilings[0].addon_name == "helper");
    CHECK(checked.ceilings[0].entry_point);
#if defined(__linux__)
    // Matching the logical name is case-insensitive; opening a listed path must retain its real spelling.
    addon.files[0].path = "libhelper.so";
    REQUIRE(app::write_installed_addons(data, {addon}));
    CHECK(app::check_addon_policy(input).problem.find("missing or changed") != std::string::npos);
#endif
}

TEST_CASE("addon policy check: cancellation interrupts bounded file reads and discards partial results") {
    test::ScratchDir scratch("addon_policy_worker_cancel_reads");
    const std::string bytes(4u << 16, 'x');
    const auto file = scratch.path() / "large.bin";
    put(file, bytes);
    unsigned callbacks = 0;
    CHECK_FALSE(app::package_file_matches(file, bytes.size(), sha256(bytes), [&] { return ++callbacks >= 3; }));
    CHECK(callbacks == 3);

    const auto addon = install_record(scratch.path() / "data", "helper", AddonKind::Script, "maker/helper", "init.lua", bytes);
    REQUIRE(app::write_installed_addons(scratch.path() / "data", {addon}));
    auto input = request(scratch.path() / "data", scratch.path() / "profile", "required = maker/helper\n");
    callbacks = 0;
    const auto canceled = app::check_addon_policy(input, [&] { return ++callbacks >= 7; });
    CHECK(canceled.canceled);
    CHECK_FALSE(canceled.ceilings_ready);
    CHECK(canceled.ceilings.empty());
}

TEST_CASE("addon policy worker: newer jobs supersede queued and running results without blocking submit") {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false;
    std::vector<std::string> calls;
    app::AddonPolicyCheckWorker worker([&](const auto& input, const auto& canceled) {
        {
            std::unique_lock lock(mutex);
            calls.push_back(input.platform);
            if (input.platform == "first") {
                entered = true;
                changed.notify_all();
                changed.wait(lock, [&] { return release; });
            }
        }
        app::AddonPolicyCheckResult result;
        result.problem = input.platform;
        result.canceled = canceled();
        return result;
    });
    app::AddonPolicyCheckRequest input;
    input.platform = "first";
    worker.submit(input);
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return entered; });
    }
    input.platform = "superseded";
    worker.submit(input);
    input.platform = "last";
    const auto last = worker.submit(input);
    CHECK_FALSE(worker.poll());
    {
        std::lock_guard lock(mutex);
        release = true;
        changed.notify_all();
    }
    worker.wait_idle();
    const auto result = worker.poll();
    REQUIRE(result);
    CHECK(result->generation == last);
    CHECK(result->problem == "last");
    CHECK(calls == std::vector<std::string>{"first", "last"});
    CHECK_FALSE(worker.poll());
}

TEST_CASE("addon policy worker: cancel invalidates active results and exceptions fail closed") {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false;
    app::AddonPolicyCheckWorker worker([&](const auto&, const auto&) -> app::AddonPolicyCheckResult {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
        throw std::runtime_error("unreadable fixture");
    });
    worker.submit({});
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return entered; });
    }
    worker.cancel();
    {
        std::lock_guard lock(mutex);
        release = true;
        changed.notify_all();
    }
    worker.wait_idle();
    CHECK_FALSE(worker.poll());
    const auto next = worker.submit({});
    worker.wait_idle();
    const auto result = worker.poll();
    REQUIRE(result);
    CHECK(result->generation == next);
    CHECK(result->problem.find("unreadable fixture") != std::string::npos);
    CHECK_FALSE(result->ceilings_ready);
    CHECK(result->ceilings.empty());
}
