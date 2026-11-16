#pragma once

// The opt-in server add-on policy's login check. Files are inspected by the caller; this evaluation neither
// downloads nor executes anything, so a missing requirement still goes through the manager's install card.

#include <string>
#include <vector>

#include "app/addon_install.h"
#include "app/addon_suggest.h"
#include "plugin/consent.h"

namespace hailstone::ui { struct LoginView; }

namespace hailstone::app {

struct RequiredAddonCandidate {
    std::string name, source;
    AddonKind kind = AddonKind::Script;
    std::uint32_t caps = 0;
    bool intact = false; // readable registry and every shipped file unchanged, or the server's verified entry
    bool server_owned = false;
    plugin::Consent consent = plugin::Consent::Unasked;
};

struct AddonRequirementProblem {
    std::string identity;
    std::string message;
};

// Unasked native-code consent is handled by the existing prompt after connecting. A refusal blocks joining.
// Upscaler selection and activation are checked separately against the provider loaded at process startup.
std::vector<AddonRequirementProblem> addon_requirement_problems(
    const plugin::Rules& rules, const std::vector<RequiredAddonCandidate>& candidates);

// Required offers ignore optional-suggestion preferences and dismissals, retaining an index entry's name/key.
std::vector<AddonSuggestion> required_addon_suggestions(
    const std::vector<AddonRequirementProblem>& problems, const AddonIndex* index);

// Opens the local review panel and prefills a required source. Fetching its manifest still needs the player's
// Review action, just like any other add-on link; presenting a failed login never contacts that repository.
void present_addon_requirement(ui::LoginView& view, const std::vector<AddonRequirementProblem>& problems,
                               const AddonIndex* index, std::string message);

} // namespace hailstone::app
