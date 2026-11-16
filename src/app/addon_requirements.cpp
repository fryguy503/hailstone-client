#include "app/addon_requirements.h"

#include <algorithm>

#include "ui/dev_screens.h"

namespace hailstone::app {

std::vector<AddonRequirementProblem> addon_requirement_problems(
    const plugin::Rules& rules, const std::vector<RequiredAddonCandidate>& candidates) {
    std::vector<AddonRequirementProblem> out;
    if (!rules.addons.enabled) return out;
    if (!rules.addons.error.empty()) {
        out.push_back({{}, "This server's add-on policy is invalid: " + rules.addons.error});
        return out;
    }
    for (const auto& required : rules.addons.required) {
        std::string problem = "Required add-on " + required + " is not installed. Open Add-ons to install it.";
        bool found = false;
        for (const auto& a : candidates) {
            if (!plugin::addon_identity_matches(required, a.name, a.source)) continue;
            const auto allowed_caps = a.kind == AddonKind::Script ? rules.player_script_caps : rules.player_plugin_caps;
            if (!plugin::addon_allowed(rules.addons, a.name, a.source)) {
                problem = "Required add-on " + required + " is blocked by this server's add-on policy.";
            } else if (a.kind == AddonKind::Tool) {
                problem = "Required add-on " + required + " is a separate tool and cannot run as a game add-on.";
            } else if (!a.intact) {
                problem = "Required add-on " + required + " has missing or changed files. Reinstall it in Add-ons.";
            } else if (!a.server_owned && (a.kind == AddonKind::Script || a.kind == AddonKind::Plugin) &&
                       (a.caps == 0 || (a.caps & ~allowed_caps))) {
                problem = "Required add-on " + required + " needs capabilities this server does not allow.";
            } else if (a.kind == AddonKind::Plugin && a.consent == plugin::Consent::Denied) {
                problem = "Required plugin " + required + " was not allowed to run. Open Add-ons to review your answer.";
            } else {
                found = true;
                break;
            }
        }
        if (!found) out.push_back({required, std::move(problem)});
    }
    return out;
}

std::vector<AddonSuggestion> required_addon_suggestions(
    const std::vector<AddonRequirementProblem>& problems, const AddonIndex* index) {
    std::vector<AddonSuggestion> out;
    for (const auto& p : problems) {
        if (p.identity.empty()) continue;
        const AddonIndexAddon* listed = nullptr;
        if (index)
            for (const auto& a : index->addons)
                if (plugin::addon_identity_matches(p.identity, a.name, a.source)) { listed = &a; break; }
        AddonSuggestion sg;
        sg.from_server = true;
        if (listed) {
            sg.name = listed->name;
            sg.source = listed->source;
            sg.key = listed->key;
        } else {
            sg.source = addon_repo_key(p.identity);
        }
        if (sg.source.empty()) continue;
        const bool seen = std::any_of(out.begin(), out.end(), [&](const AddonSuggestion& a) {
            return addon_repo_key(a.source) == addon_repo_key(sg.source);
        });
        if (!seen) out.push_back(std::move(sg));
    }
    return out;
}

void present_addon_requirement(ui::LoginView& view, const std::vector<AddonRequirementProblem>& problems,
                               const AddonIndex* index, std::string message) {
    view.busy = false;
    view.status = {message, true};
    view.addons.open = true;
    view.addons.status = {std::move(message), true};
    const auto offers = required_addon_suggestions(problems, index);
    if (!offers.empty()) view.addons.link = offers.front().source;
}

} // namespace hailstone::app
