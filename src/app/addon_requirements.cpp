#include "app/addon_requirements.h"

#include <algorithm>
#include <set>
#include <utility>

#include "core/version.h"
#include "mq/lua_mq.h"
#include "ui/dev_screens.h"
#include "ui/ui_packs.h"

namespace hailstone::app {

bool required_script_found(const std::filesystem::path& data_dir, std::string_view name) {
    namespace fs = std::filesystem;
    // As ScriptHost::run asks for it: by its lower-cased name, which finds the folder's own spelling.
    std::string key(name);
    for (char& c : key)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    const fs::path entry = addon_folder(data_dir, AddonKind::Script, name) / "init.lua";
    const auto found = mq::script_spelled_path(entry.parent_path().parent_path(), fs::path(key) / "init.lua");
    std::error_code ec;
    return found && fs::is_regular_file(*found, ec) && fs::equivalent(*found, entry, ec);
}

void apply_ui_addon_policy(std::vector<ui::UiPack>& packs, const plugin::Rules* rules,
                           const std::vector<InstalledAddon>& installed) {
    if (!rules || rules->provisional || !rules->addons.enabled) return;
    auto folded = [](std::string name) {
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return name;
    };
    std::set<std::string> denied;
    for (auto& pack : packs) {
        if (ui::trusted_ui_pack(pack)) continue;
        std::string reason;
        if (pack.source == "addon") {
            const auto owner = std::find_if(installed.begin(), installed.end(), [&](const InstalledAddon& addon) {
                return addon.kind == AddonKind::Ui && folded(addon.name) == folded(pack.addon);
            });
            if (owner == installed.end() || plugin::addon_allowed(rules->addons, owner->source)) continue;
            reason = "This server blocks UI add-on " + owner->source + ".";
        } else {
            // A pack put in ui/ by hand has no repository, so no list names it: like any unlisted add-on, it is
            // refused where the server's default is deny (docs/plugins.md "Opt-in add-on policies").
            if (plugin::addon_allowed(rules->addons, {})) continue;
            reason = "This server allows only the add-ons it lists; this interface was put in ui/ by hand.";
        }
        denied.insert(pack.id);
        if (!pack.available) continue;
        pack.available = false;
        pack.unavailable_reason = std::move(reason);
    }
    // Derived interfaces cannot retain a blocked base's code through inheritance. Policy is independent of the
    // requested UI name; the normal registry fallback selects a built-in without rewriting the user's setting.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& pack : packs) {
            if (denied.contains(pack.id)) continue;
            if (std::none_of(pack.base_ids.begin(), pack.base_ids.end(), [&](const auto& id) { return denied.contains(id); }))
                continue;
            denied.insert(pack.id);
            if (pack.available) {
                pack.available = false;
                pack.unavailable_reason = "This interface inherits a UI add-on blocked by this server.";
            }
            changed = true;
        }
    }
}

std::vector<AddonRequirementProblem> addon_requirement_problems(
    const plugin::Rules& rules, const std::vector<RequiredAddonCandidate>& candidates) {
    std::vector<AddonRequirementProblem> out;
    if (!rules.addons.enabled) return out;
    if (!rules.addons.error.empty()) {
        out.push_back({{}, "This server's add-on policy is invalid: " + rules.addons.error});
        return out;
    }
    for (const auto& required : rules.addons.required) {
        const auto minimum = rules.addons.minimum_versions.find(required);
        const bool versioned = minimum != rules.addons.minimum_versions.end();
        const std::string version_needed = versioned ? " version " + minimum->second.to_string() + " or newer" : "";
        std::string problem = "Required add-on " + required + version_needed +
                              " is not installed. Open Add-ons to install it.";
        bool found = false;
        for (const auto& a : candidates) {
            if (!plugin::addon_identity_matches(required, a.source)) continue;
            const auto allowed_caps = a.kind == AddonKind::Script ? rules.player_script_caps : rules.player_plugin_caps;
            const auto installed_version = versioned ? parse_version(a.version) : std::optional<Version>{};
            if (!plugin::addon_allowed(rules.addons, a.source)) {
                problem = "Required add-on " + required + " is blocked by this server's add-on policy.";
            } else if (!a.intact) {
                problem = "Required add-on " + required + " has missing or changed files. Reinstall it in Add-ons.";
            } else if (versioned && !installed_version) {
                problem = "Required add-on " + required + " has no valid installed version;" + version_needed +
                          " is required. Reinstall it in Add-ons.";
            } else if (versioned && compare_versions(*installed_version, minimum->second) < 0) {
                problem = "Required add-on " + required + " needs" + version_needed + "; installed version is " +
                          installed_version->to_string() + ". Open Add-ons to update it.";
            } else if (a.kind == AddonKind::Script && !a.startable) {
                problem = "Required add-on " + required + " cannot be started here: another folder beside scripts/" +
                          a.name + " has the same name in other letter case.";
            } else if ((a.kind == AddonKind::Script || a.kind == AddonKind::Plugin) &&
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
                if (plugin::addon_identity_matches(p.identity, a.source)) { listed = &a; break; }
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

std::vector<std::string> addon_refusal_log_lines(const std::vector<AddonRequirementProblem>& problems,
                                                 std::string_view message) {
    std::vector<std::string> out;
    for (const auto& p : problems)
        out.push_back("Add-ons: entry refused by this server's add-on policy: " + p.message);
    if (out.empty() && !message.empty())
        out.push_back("Add-ons: entry refused by this server's add-on policy: " + std::string(message));
    return out;
}

AddonAdmission::Result AddonAdmission::warn(std::string problem, bool retriable) {
    Result out;
    if (problem != failure_ || (!problem.empty() && retriable != failure_retriable_)) {
        failure_ = problem;
        failure_retriable_ = !problem.empty() && retriable;
        ++failure_serial_;
    }
    if (problem == warned_) return out;
    warned_ = problem;
    out.warning = std::move(problem);
    return out;
}

void AddonAdmission::reset() {
    serial_.reset();
    admitted_ = wait_frame_ = false;
    zone_problem_.clear();
    warned_.clear();
    if (!failure_.empty()) ++failure_serial_;
    failure_.clear();
    failure_retriable_ = false;
}

AddonAdmission::Result AddonAdmission::update(const plugin::Rules* rules, std::uint32_t serial, bool in_world,
                                              const std::function<std::string()>& zone_problem,
                                              const std::function<std::optional<std::string>()>& ready) {
    if (!rules || rules->provisional) {
        // Out of the world (character select, a login): the next zone-in decides again, with its own rules.
        reset();
        return {};
    }
    if (serial_ != serial) {
        serial_ = serial;
        zone_problem_ = rules->addons.enabled ? zone_problem() : std::string();
        if (!admitted_ && !zone_problem_.empty()) return {Step::Refuse, zone_problem_, {}};
        // Automation applies new rules after the connection's frame: let required autostart and the consent prompt
        // run before asking whether the add-ons are ready.
        wait_frame_ = rules->addons.enabled;
    }
    if (admitted_) {
        std::string problem = zone_problem_;
        bool retriable = false;
        if (problem.empty()) {
            if (std::exchange(wait_frame_, false)) return {};
            if (rules->addons.enabled) problem = ready().value_or(std::string());
            retriable = true;
        }
        return warn(std::move(problem), retriable);
    }
    if (!in_world) return {};
    if (!rules->addons.enabled) {
        admitted_ = true;
        return {};
    }
    if (std::exchange(wait_frame_, false)) return {Step::Start, {}, {}};
    const auto state = ready();
    if (!state) return {Step::Consent, {}, {}};
    if (!state->empty()) return {Step::Refuse, *state, {}};
    admitted_ = true;
    return {};
}

} // namespace hailstone::app
