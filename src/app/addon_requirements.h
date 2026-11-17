#pragma once

// The opt-in server add-on policy's login check. Files are inspected by the caller; this evaluation neither
// downloads nor executes anything, so a missing requirement still goes through the manager's install card.

#include <cstdint>
#include <string>
#include <vector>

#include "app/addon_install.h"
#include "app/addon_suggest.h"
#include "plugin/consent.h"

namespace hailstone::ui { struct LoginView; struct UiPack; }

namespace hailstone::app {

struct RequiredAddonCandidate {
    std::string name, source;
    AddonKind kind = AddonKind::Script;
    std::uint32_t caps = 0;
    bool intact = false; // readable registry and every shipped file unchanged
    bool startable = true; // a script: where the script loader looks for it (required_script_found)
    plugin::Consent consent = plugin::Consent::Unasked;
    std::string version{}; // the installed package's manifest version, checked only for a minimum requirement
};

struct AddonRequirementProblem {
    std::string identity;
    std::string message;
};

// Whether the script loader finds the installed script add-on `name` to start it. ScriptHost looks a script up by
// its lower-cased name and finds it whatever its case (mq::script_spelled_path), unless two folders' names differ
// only in case (scripts/AutoLoot beside scripts/autoloot): then it finds the exact one or neither, and admission holds
// a requirement whose own folder it would not start unmet rather than passing it and failing to start it.
bool required_script_found(const std::filesystem::path& data_dir, std::string_view name);

// Unasked native-code consent is handled by the existing prompt after connecting. A refusal blocks joining.
// Upscaler selection and activation are checked separately against the provider loaded at process startup. A tool
// is a program of its own: a required one is met by its intact installation (the client does not start it, and
// cannot tell whether it runs). A versioned requirement also needs an installed manifest version at least as new
// as its minimum, using SemVer precedence. Required managed scripts and plugins use their manifest capabilities
// independently of the general player-code switches and caps; an empty capability set still cannot run.
std::vector<AddonRequirementProblem> addon_requirement_problems(
    const plugin::Rules& rules, const std::vector<RequiredAddonCandidate>& candidates);

// Restricts installed interfaces and their approved dependents by repository identity. Fresh pack metadata is
// supplied on each refresh; provisional/cached rules never retire an interface, and built-ins remain available.
// This changes availability only: requirements do not select an interface or turn it into an autorun script.
void apply_ui_addon_policy(std::vector<ui::UiPack>& packs, const plugin::Rules* rules,
                           const std::vector<InstalledAddon>& installed);

// Required offers ignore optional-suggestion preferences and dismissals, retaining an index entry's name/key.
std::vector<AddonSuggestion> required_addon_suggestions(
    const std::vector<AddonRequirementProblem>& problems, const AddonIndex* index);

// Opens the local review panel and prefills a required source. Fetching its manifest still needs the player's
// Review action, just like any other add-on link; presenting a failed login never contacts that repository.
void present_addon_requirement(ui::LoginView& view, const std::vector<AddonRequirementProblem>& problems,
                               const AddonIndex* index, std::string message);

} // namespace hailstone::app
