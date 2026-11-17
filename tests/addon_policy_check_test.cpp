#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "app/addon_policy_check.h"
#include "core/binary_header.h"
#include "core/update_manifest.h"
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

TEST_CASE("addon policy worker: installed manifest versions meet required minimums without losing verified provenance") {
    test::ScratchDir scratch("addon_policy_worker_minimum_version");
    const auto data = scratch.path() / "data";
    const std::string platform(update_platform());
    const std::string extension(plugin_extension(platform));
    REQUIRE_FALSE(extension.empty());
    auto addon = install_record(data, "helper", AddonKind::Plugin, "maker/helper", "helper" + extension,
                                "native package bytes");
    addon.version = "1.0.0-preview.3";
    REQUIRE(app::write_installed_addons(data, {addon}));
    auto input = request(data, scratch.path() / "profile", "required = maker/helper:1.0.0-preview.4\n");
    input.platform = platform;
    input.rules.player_plugin_caps = 0; // required managed code uses its manifest capabilities
    app::AddonPolicyCheckWorker worker;
    const auto old_generation = worker.submit(input);
    worker.wait_idle();
    auto checked = worker.poll();
    REQUIRE(checked);
    CHECK(checked->generation == old_generation);
    CHECK_FALSE(checked->canceled);
    CHECK(checked->ceilings_ready);
    REQUIRE(checked->problems.size() == 1);
    CHECK(checked->problems[0].identity == "maker/helper");
    CHECK(checked->problem.find("maker/helper") != std::string::npos);
    CHECK(checked->problem.find("1.0.0-preview.4 or newer") != std::string::npos);
    CHECK(checked->problem.find("installed version is 1.0.0-preview.3") != std::string::npos);
    const auto offers = app::required_addon_suggestions(checked->problems, nullptr);
    REQUIRE(offers.size() == 1);
    CHECK(offers[0].source == "maker/helper");

    for (const std::string version : {"1.0.0-preview.4", "1.0.0-preview.10", "1.0.0+build.7"}) {
        CAPTURE(version);
        addon.version = version;
        REQUIRE(app::write_installed_addons(data, {addon}));
        const auto generation = worker.submit(input);
        worker.wait_idle();
        checked = worker.poll();
        REQUIRE(checked);
        CHECK(checked->generation == generation);
        CHECK(checked->generation > old_generation);
        CHECK_FALSE(checked->canceled);
        CHECK(checked->problem.empty());
        CHECK(checked->problems.empty());
        CHECK(checked->ceilings_ready);
        REQUIRE(checked->ceilings.size() == 1);
        const auto& ceiling = checked->ceilings[0];
        CHECK(ceiling.verified);
        CHECK(ceiling.entry_point);
        CHECK(ceiling.addon_source == "maker/helper");
        CHECK(ceiling.addon_name == "helper");
        CHECK(ceiling.sha256 == addon.files[0].sha256);
        CHECK(ceiling.caps == addon.caps);
    }
    // A newer registry version does not make changed package bytes satisfy the requirement.
    put(app::addon_folder(data, addon.kind, addon.name) / addon.files[0].path, "edited native package bytes");
    const auto changed_generation = worker.submit(input);
    worker.wait_idle();
    checked = worker.poll();
    REQUIRE(checked);
    CHECK(checked->generation == changed_generation);
    REQUIRE(checked->problems.size() == 1);
    CHECK(checked->problems[0].identity == "maker/helper");
    CHECK(checked->problem.find("missing or changed") != std::string::npos);
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

TEST_CASE("addon policy check: required tools are satisfied by an intact program for the current platform") {
    test::ScratchDir scratch("addon_policy_worker_tool");
    const auto data = scratch.path() / "data";
    auto addon = install_record(data, "helper", AddonKind::Tool, "maker/helper", "helper.exe", "tool bytes");
    addon.caps = 0;
    REQUIRE(app::write_installed_addons(data, {addon}));
    auto input = request(data, scratch.path() / "profile", "required = maker/helper\n");
    CHECK(app::check_addon_policy(input).problem.empty());
    input.platform = "linux-x64";
    CHECK(app::check_addon_policy(input).problem.find("missing or changed") != std::string::npos);
    input.platform = "windows-x64";
    put(app::addon_folder(data, addon.kind, addon.name) / "helper.exe", "edited tool");
    CHECK(app::check_addon_policy(input).problem.find("missing or changed") != std::string::npos);
}

TEST_CASE("addon policy check: portable required UI bundles have manifests instead of executable entrypoints") {
    test::ScratchDir scratch("addon_policy_worker_ui");
    const auto data = scratch.path() / "data";
    auto addon = install_record(data, "interfaces", AddonKind::Ui, "maker/interfaces", "ui/First/ui.ini",
                                "name=First\nbase=builtin:modern\n");
    addon.caps = 0;
    const std::string module = "error('integrity checks must not execute presentation Lua')\n";
    addon.files.push_back({"ui/First/lib/presentation.lua", static_cast<std::uint64_t>(module.size()), sha256(module)});
    put(app::addon_folder(data, addon.kind, addon.name) / addon.files.back().path, module);
    REQUIRE(app::write_installed_addons(data, {addon}));
    auto input = request(data, scratch.path() / "profile", "required = maker/interfaces\n");
    app::AddonPolicyCheckWorker worker;
    for (const char* platform : {"windows-x64", "linux-x64", "macos-arm64"}) {
        CAPTURE(platform);
        input.platform = platform;
        const auto direct = app::check_addon_policy(input);
        CHECK_FALSE(direct.canceled);
        CHECK(direct.ceilings_ready);
        CHECK(direct.problems.empty());
        CHECK(direct.problem.empty());
        CHECK(direct.ceilings.empty());
        const auto generation = worker.submit(input);
        worker.wait_idle();
        const auto checked = worker.poll();
        REQUIRE(checked);
        CHECK(checked->generation == generation);
        CHECK_FALSE(checked->canceled);
        CHECK(checked->ceilings_ready);
        CHECK(checked->problems.empty());
        CHECK(checked->problem.empty());
        CHECK(checked->ceilings.empty());
    }
}

TEST_CASE("addon policy check: UI bundle installation must meet the required minimum version") {
    test::ScratchDir scratch("addon_policy_worker_ui_minimum");
    const auto data = scratch.path() / "data";
    auto addon = install_record(data, "interfaces", AddonKind::Ui, "maker/interfaces", "ui/first/ui.ini",
                                "name=First\nbase=builtin:modern\n");
    addon.caps = 0;
    const auto input = request(data, scratch.path() / "profile", "required = maker/interfaces:1.2.3\n");
    REQUIRE(input.rules.addons.error.empty());
    app::AddonPolicyCheckWorker worker;
    for (const std::string version : {"1.2.2", "1.2.3", "1.3.0"}) {
        CAPTURE(version);
        addon.version = version;
        REQUIRE(app::write_installed_addons(data, {addon}));
        const auto direct = app::check_addon_policy(input);
        CHECK_FALSE(direct.canceled);
        CHECK(direct.ceilings_ready);
        CHECK(direct.ceilings.empty());
        if (version == "1.2.2") {
            REQUIRE(direct.problems.size() == 1);
            CHECK(direct.problems.front().identity == "maker/interfaces");
            CHECK(direct.problem.find("installed version is 1.2.2") != std::string::npos);
        } else {
            CHECK(direct.problems.empty());
            CHECK(direct.problem.empty());
        }
        const auto generation = worker.submit(input);
        worker.wait_idle();
        const auto checked = worker.poll();
        REQUIRE(checked);
        CHECK(checked->generation == generation);
        CHECK_FALSE(checked->canceled);
        CHECK(checked->ceilings_ready);
        CHECK(checked->ceilings.empty());
        CHECK(checked->problems.size() == direct.problems.size());
        CHECK(checked->problem == direct.problem);
    }
}

TEST_CASE("addon policy check: required UI bundles still verify every recorded presentation file") {
    test::ScratchDir scratch("addon_policy_worker_ui_integrity");
    const auto data = scratch.path() / "data";
    auto addon = install_record(data, "interfaces", AddonKind::Ui, "maker/interfaces", "ui/first/ui.ini",
                                "name=First\nbase=builtin:modern\n");
    addon.caps = 0;
    const std::string module = "return 1\n";
    addon.files.push_back({"ui/first/lib/presentation.lua", static_cast<std::uint64_t>(module.size()), sha256(module)});
    const auto folder = app::addon_folder(data, addon.kind, addon.name);
    put(folder / addon.files.back().path, module);
    REQUIRE(app::write_installed_addons(data, {addon}));
    const auto input = request(data, scratch.path() / "profile", "required = maker/interfaces\n");
    REQUIRE(app::check_addon_policy(input).problem.empty());
    SUBCASE("missing direct UI manifest") {
        REQUIRE(fs::remove(folder / addon.files.front().path));
    }
    SUBCASE("missing nested helper") {
        REQUIRE(fs::remove(folder / addon.files.back().path));
    }
    SUBCASE("changed nested helper") {
        put(folder / addon.files.back().path, "return 2\n"); // same byte count: the hash must catch the edit
    }
    SUBCASE("empty file record") {
        addon.files.clear();
        REQUIRE(app::write_installed_addons(data, {addon}));
    }
    const auto direct = app::check_addon_policy(input);
    REQUIRE(direct.problems.size() == 1);
    CHECK(direct.problems.front().identity == "maker/interfaces");
    CHECK(direct.problem.find("missing or changed") != std::string::npos);
    CHECK(direct.ceilings_ready);
    app::AddonPolicyCheckWorker worker;
    const auto generation = worker.submit(input);
    worker.wait_idle();
    const auto checked = worker.poll();
    REQUIRE(checked);
    CHECK(checked->generation == generation);
    REQUIRE(checked->problems.size() == 1);
    CHECK(checked->problem.find("missing or changed") != std::string::npos);
    CHECK(checked->ceilings_ready);
}

TEST_CASE("addon policy check: UI entry manifests require a direct valid pack folder") {
    test::ScratchDir scratch("addon_policy_worker_ui_entry");
    const auto data = scratch.path() / "data";
    const auto input = request(data, scratch.path() / "profile", "required = maker/interfaces\n");
    for (const char* path : {"README.md", "init.lua", "interfaces.dll", "ui.ini", "ui/first/lib/ui.ini",
                            "ui/bad.name/ui.ini", "other/first/ui.ini"}) {
        CAPTURE(path);
        auto addon = install_record(data, "interfaces", AddonKind::Ui, "maker/interfaces", path, "name=First\n");
        addon.caps = 0;
        REQUIRE(app::write_installed_addons(data, {addon}));
        const auto checked = app::check_addon_policy(input);
        REQUIRE(checked.problems.size() == 1);
        CHECK(checked.problem.find("missing or changed") != std::string::npos);
        CHECK(checked.ceilings_ready);
    }
}

TEST_CASE("addon policy check: required scripts preserve mixed-case package folders") {
    test::ScratchDir scratch("addon_policy_worker_script_case");
    const auto data = scratch.path() / "data";
    const auto addon = install_record(data, "MiXeD", AddonKind::Script, "maker/helper", "init.lua", "-- entry");
    REQUIRE(app::write_installed_addons(data, {addon}));
    const auto input = request(data, scratch.path() / "profile", "required = maker/helper\n");
    const auto checked = app::check_addon_policy(input);
    CHECK(checked.problem.empty());
    plugin::Policy policy;
    policy.set(input.rules);
    policy.set_ceilings(checked.ceilings);
    const auto entries = policy.required_entries(plugin::FileKind::Script);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].first == "mixed");
    CHECK(entries[0].second == addon.files[0].sha256);
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
    // The first 64 KiB read has finished when the third cancellation check interrupts hashing.
    CHECK_FALSE(app::addon_file_sha256(file, [&] { return ++callbacks >= 3; }));
    CHECK(callbacks == 3);

    callbacks = 0;
    CHECK_FALSE(app::addon_file_matches(file, bytes.size(), sha256(bytes), [&] { return ++callbacks >= 4; }));
    CHECK(callbacks == 4);

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

TEST_CASE("addon policy worker: shutdown joins idle checks and cancels active checks") {
    SUBCASE("an idle worker can be destroyed immediately after finishing a check") {
        for (int i = 0; i < 16; ++i) {
            app::AddonPolicyCheckWorker worker([](const auto&, const auto&) {
                app::AddonPolicyCheckResult result;
                result.ceilings_ready = true;
                return result;
            });
            worker.submit({});
            worker.wait_idle();
            REQUIRE(worker.poll());
        }
    }
    SUBCASE("active work observes shutdown before its worker joins") {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        std::atomic<bool> observed_cancel = false;
        auto worker = std::make_unique<app::AddonPolicyCheckWorker>([&](const auto&, const auto& canceled) {
            {
                std::lock_guard lock(mutex);
                entered = true;
                changed.notify_all();
            }
            while (!canceled()) std::this_thread::yield();
            observed_cancel = true;
            app::AddonPolicyCheckResult result;
            result.ceilings_ready = true;
            return result; // the worker must discard it even without result.canceled being set
        });
        worker->submit({});
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [&] { return entered; });
        }
        SUBCASE("cancel discards a completed result before idle shutdown") {
            worker->cancel();
            worker->wait_idle();
            CHECK(observed_cancel.load());
            CHECK_FALSE(worker->poll());
        }
        SUBCASE("shutdown itself requests cancellation of an active check") {
            worker.reset();
            CHECK(observed_cancel.load());
        }
        worker.reset();
        CHECK(observed_cancel.load());
    }
}
