#pragma once

// Required-package integrity and provenance checks run outside the frame loop. A worker owns only copied rules
// and filesystem paths; it never calls the connection, UI, script host or native code.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/addon_requirements.h"

namespace hailstone::app {

using AddonCheckCancel = std::function<bool()>;

struct AddonPolicyCheckRequest {
    plugin::Rules rules;
    std::filesystem::path data_dir;
    std::string platform;
    std::string active_upscaler;
    std::filesystem::path loaded_upscaler_folder;
};

struct AddonPolicyCheckResult {
    std::uint64_t generation = 0;
    std::vector<AddonRequirementProblem> problems;
    std::string problem;
    std::vector<plugin::Ceiling> ceilings;
    bool ceilings_ready = false; // true only after a complete scan, including a genuinely empty registry
    bool canceled = false;
};

AddonPolicyCheckResult check_addon_policy(const AddonPolicyCheckRequest& request,
                                         const AddonCheckCancel& canceled = {});

class AddonPolicyCheckWorker {
public:
    using Check = std::function<AddonPolicyCheckResult(const AddonPolicyCheckRequest&, const AddonCheckCancel&)>;

    explicit AddonPolicyCheckWorker(Check check = check_addon_policy);
    ~AddonPolicyCheckWorker();
    AddonPolicyCheckWorker(const AddonPolicyCheckWorker&) = delete;
    AddonPolicyCheckWorker& operator=(const AddonPolicyCheckWorker&) = delete;

    // At most one job runs and one (the newest) waits. A superseded job cannot publish its result.
    std::uint64_t submit(AddonPolicyCheckRequest request);
    std::optional<AddonPolicyCheckResult> poll();
    // Invalidates queued/finished work immediately. A running check notices at its next bounded file read.
    void cancel();
    // For shutdown-independent test synchronization, never for a frame or connection callback.
    void wait_idle();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hailstone::app
