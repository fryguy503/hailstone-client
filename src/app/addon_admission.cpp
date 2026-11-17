#include "app/addon_admission.h"

#include <utility>

namespace hailstone::app {

void AddonAdmission::change(AddonAdmissionPhase phase, std::string problem, bool force) {
    if (!force && phase_ == phase && problem_ == problem) return;
    phase_ = phase;
    problem_ = std::move(problem);
    if (phase_ == AddonAdmissionPhase::Ready || phase_ == AddonAdmissionPhase::Inactive) warned_problem_.clear();
    ++revision_;
}

void AddonAdmission::reset() {
    const bool changed = admitted_ || verified_ || rules_serial_ != 0;
    admitted_ = verified_ = false;
    rules_serial_ = 0;
    change(AddonAdmissionPhase::Inactive, {}, changed);
}

void AddonAdmission::disable() {
    const bool changed = verified_ || rules_serial_ != 0;
    verified_ = false;
    rules_serial_ = 0;
    change(AddonAdmissionPhase::Inactive, {}, changed);
}

void AddonAdmission::begin_check(std::uint32_t rules_serial) {
    verified_ = false;
    rules_serial_ = rules_serial;
    change(AddonAdmissionPhase::Checking, {}, true);
}

void AddonAdmission::verification_finished(std::string problem) {
    if (phase_ != AddonAdmissionPhase::Checking) return;
    verified_ = problem.empty();
    change(verified_ ? AddonAdmissionPhase::Starting
                     : admitted_ ? AddonAdmissionPhase::Degraded : AddonAdmissionPhase::Refused,
           std::move(problem));
}

void AddonAdmission::runtime_changed(bool ready, bool awaiting_consent, std::string problem) {
    if (!verified_) return;
    if (!problem.empty()) {
        change(admitted_ ? AddonAdmissionPhase::Degraded : AddonAdmissionPhase::Refused, std::move(problem));
    } else if (ready) {
        change(AddonAdmissionPhase::Ready);
    } else {
        change(awaiting_consent ? AddonAdmissionPhase::AwaitingConsent : AddonAdmissionPhase::Starting);
    }
}

void AddonAdmission::admit() {
    if (!admitted_ && (phase_ == AddonAdmissionPhase::Inactive || phase_ == AddonAdmissionPhase::Ready)) {
        admitted_ = true;
        ++revision_;
    }
}

void AddonAdmission::emergency_stop(std::string problem) {
    if (phase_ == AddonAdmissionPhase::Inactive) return;
    verified_ = false;
    change(admitted_ ? AddonAdmissionPhase::Degraded : AddonAdmissionPhase::Refused, std::move(problem), true);
}

bool AddonAdmission::may_enter() const {
    return admitted_ || phase_ == AddonAdmissionPhase::Inactive || phase_ == AddonAdmissionPhase::Ready;
}

std::string AddonAdmission::take_warning() {
    if (phase_ != AddonAdmissionPhase::Degraded || problem_.empty() || problem_ == warned_problem_) return {};
    warned_problem_ = problem_;
    return problem_;
}

} // namespace hailstone::app
