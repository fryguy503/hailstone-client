#include "app/addon_policy_check.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include "app/addon_upscaler_policy.h"
#include "core/ascii_case.h"
#include "core/binary_header.h"
#include "plugin/consent.h"

namespace hailstone::app {

namespace {

bool is_canceled(const AddonCheckCancel& canceled) { return canceled && canceled(); }

bool entry_file(const InstalledAddon& addon, const AddonFile& file, std::string_view platform) {
    const bool top = file.path.find_first_of("/\\") == std::string::npos;
    switch (addon.kind) {
    case AddonKind::Ui: {
        // UI bundles are portable presentations, with manifests instead of executable entrypoints.
        const std::string_view path(file.path);
        const auto slash = path.find('/', 3);
        return path.starts_with("ui/") && slash != std::string_view::npos &&
               addon_name_ok(path.substr(3, slash - 3)) && path.substr(slash + 1) == "ui.ini";
    }
    case AddonKind::Script: return top && iequals(file.path, "init.lua");
    case AddonKind::Tool: return top && iequals(file.path, addon_tool_program(addon.name, platform));
    case AddonKind::Upscaler: {
        const std::string extension(plugin_extension(platform));
        return top && !extension.empty() && iequals(file.path, addon.name + extension);
    }
    case AddonKind::Plugin: {
        const std::string extension(plugin_extension(platform));
        if (!top || extension.empty()) return false;
        // Match native_plugins::plugin_name: only the literal lowercase "lib" prefix is removed, on every host.
        std::string_view filename = file.path;
        if (filename.size() > 3 + extension.size() && filename.starts_with("lib")) filename.remove_prefix(3);
        return iequals(filename, addon.name + extension);
    }
    }
    return false;
}

std::vector<RequiredAddonCandidate> candidates(const AddonPolicyCheckRequest& request, const AddonRegistry& registry,
                                              const AddonCheckCancel& canceled) {
    std::vector<RequiredAddonCandidate> out;
    for (const auto& addon : registry.addons) {
        if (is_canceled(canceled)) break;
        if (!plugin::addon_required(request.rules.addons, addon.source)) continue;
        const bool entry = std::any_of(addon.files.begin(), addon.files.end(), [&](const AddonFile& file) {
            return entry_file(addon, file, request.platform);
        });
        RequiredAddonCandidate candidate{addon.name, addon.source, addon.kind, addon.caps,
                                         !registry.newer && entry && !addon.files.empty() &&
                                             modified_addon_files(request.data_dir, addon, canceled).empty()};
        candidate.version = addon.version;
        if (addon.kind == AddonKind::Script)
            candidate.startable = required_script_found(request.data_dir, addon.name);
        if (addon.kind == AddonKind::Plugin)
            for (const auto& file : addon.files) {
                if (is_canceled(canceled)) break;
                if (entry_file(addon, file, request.platform)) {
                    candidate.consent = plugin::plugin_consent(request.rules.dir, file.sha256);
                    break;
                }
            }
        out.push_back(std::move(candidate));
    }
    return out;
}

} // namespace

AddonPolicyCheckResult check_addon_policy(const AddonPolicyCheckRequest& request, const AddonCheckCancel& canceled) {
    AddonPolicyCheckResult result;
    const auto stop = [&] {
        result.canceled = is_canceled(canceled);
        return result.canceled;
    };
    if (stop() || !request.rules.addons.enabled) return result;
    const auto registry = read_addon_registry(request.data_dir);
    if (stop()) return result;
    result.problems = addon_requirement_problems(request.rules, candidates(request, registry, canceled));
    if (stop()) return result;
    result.problem = result.problems.empty()
                         ? addon_upscaler_problem(request.rules.addons, registry, request.active_upscaler,
                                                  request.loaded_upscaler_folder, canceled)
                         : result.problems.front().message;
    if (stop()) return result;
    result.ceilings = addon_ceilings(request.data_dir, registry, true, canceled);
    if (!stop()) result.ceilings_ready = true;
    return result;
}

struct AddonPolicyCheckWorker::Impl {
    struct Job {
        std::uint64_t generation = 0;
        AddonPolicyCheckRequest request;
    };

    Check check;
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Job> pending;
    std::optional<AddonPolicyCheckResult> result;
    std::atomic<std::uint64_t> generation = 0;
    bool working = false;
    std::jthread thread;

    explicit Impl(Check fn) : check(std::move(fn)), thread([this](std::stop_token stop) { run(stop); }) {}
    ~Impl() {
        // Synchronize with the wait predicate so its notification cannot be lost just before an idle wait.
        {
            std::lock_guard lock(mutex);
            thread.request_stop();
        }
        changed.notify_all();
        thread.join();
    }

    void run(std::stop_token stop) {
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            changed.wait(lock, [&] { return stop.stop_requested() || pending.has_value(); });
            if (stop.stop_requested()) break;
            Job job = std::move(*pending);
            pending.reset();
            working = true;
            lock.unlock();
            AddonPolicyCheckResult checked;
            const auto canceled = [&] { return stop.stop_requested() || generation.load() != job.generation; };
            try {
                checked = check(job.request, canceled);
            } catch (const std::exception& error) {
                checked.ceilings.clear();
                checked.ceilings_ready = false;
                checked.problem = std::string("Could not check this server's required add-ons: ") + error.what();
            } catch (...) {
                checked.ceilings.clear();
                checked.ceilings_ready = false;
                checked.problem = "Could not check this server's required add-ons.";
            }
            checked.generation = job.generation;
            lock.lock();
            if (!canceled() && !checked.canceled) result = std::move(checked);
            working = false;
            changed.notify_all();
        }
        working = false;
        pending.reset();
        changed.notify_all();
    }
};

AddonPolicyCheckWorker::AddonPolicyCheckWorker(Check check) : impl_(std::make_unique<Impl>(std::move(check))) {}
AddonPolicyCheckWorker::~AddonPolicyCheckWorker() = default;

std::uint64_t AddonPolicyCheckWorker::submit(AddonPolicyCheckRequest request) {
    std::lock_guard lock(impl_->mutex);
    const auto generation = impl_->generation.fetch_add(1) + 1;
    impl_->pending = Impl::Job{generation, std::move(request)};
    impl_->result.reset();
    impl_->changed.notify_all();
    return generation;
}

std::optional<AddonPolicyCheckResult> AddonPolicyCheckWorker::poll() {
    std::lock_guard lock(impl_->mutex);
    auto result = std::move(impl_->result);
    impl_->result.reset();
    return result;
}

void AddonPolicyCheckWorker::cancel() {
    std::lock_guard lock(impl_->mutex);
    impl_->generation.fetch_add(1);
    impl_->pending.reset();
    impl_->result.reset();
    impl_->changed.notify_all();
}

void AddonPolicyCheckWorker::wait_idle() {
    std::unique_lock lock(impl_->mutex);
    impl_->changed.wait(lock, [&] { return !impl_->working && !impl_->pending; });
}

} // namespace hailstone::app
