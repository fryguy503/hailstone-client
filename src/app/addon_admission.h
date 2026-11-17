#pragma once

#include <cstdint>
#include <string>

namespace hailstone::app {

enum class AddonAdmissionPhase { Inactive, Checking, AwaitingConsent, Starting, Ready, Degraded, Refused };

// One connection's admission decision. Package verification and runtime readiness are separate: a runtime
// success cannot clear a package failure. Once admitted, later failures leave the connection playable for recovery.
class AddonAdmission {
public:
    void reset();
    void disable();
    void begin_check(std::uint32_t rules_serial);
    // The caller matches worker generations before delivering a result; disabled/reset checks ignore late results.
    void verification_finished(std::string problem);
    void runtime_changed(bool ready, bool awaiting_consent, std::string problem);
    void admit();
    // Stays unsatisfied until a fresh check is explicitly started; ordinary runtime updates cannot restart it.
    void emergency_stop(std::string problem);
    // A repeated zone-in check may revisit the same failure; warn again only after recovery or a new problem.
    std::string take_warning();

    bool may_enter() const;
    bool admitted() const { return admitted_; }
    AddonAdmissionPhase phase() const { return phase_; }
    const std::string& problem() const { return problem_; }
    std::uint32_t rules_serial() const { return rules_serial_; }
    std::uint64_t revision() const { return revision_; }

private:
    void change(AddonAdmissionPhase phase, std::string problem = {}, bool force = false);

    AddonAdmissionPhase phase_ = AddonAdmissionPhase::Inactive;
    bool admitted_ = false;
    bool verified_ = false;
    std::string problem_;
    std::string warned_problem_;
    std::uint32_t rules_serial_ = 0;
    std::uint64_t revision_ = 0;
};

} // namespace hailstone::app
