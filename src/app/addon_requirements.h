#pragma once

// The opt-in server add-on policy's login check. Files are inspected by the caller; this evaluation neither
// downloads nor executes anything, so a missing requirement still goes through the manager's install card.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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
    std::string version{}; // the installed manifest version; checked only against a minimum version
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
// cannot tell whether it runs). A versioned requirement (owner/repo:1.0.0) also needs an installed manifest version
// at least that new, in SemVer order; one that does not parse cannot meet it.
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
// The log's lines for an entry the server's add-on policy refused: one for each problem, else `message` alone (an
// upscaler, a required add-on that did not start).
std::vector<std::string> addon_refusal_log_lines(const std::vector<AddonRequirementProblem>& problems,
                                                 std::string_view message);
void present_addon_requirement(ui::LoginView& view, const std::vector<AddonRequirementProblem>& problems,
                               const AddonIndex* index, std::string message);

// The opted-in policy's gate on a world session, each frame (ClientApp::addon_world_allowed). Decided against the
// rules the zone-in brought, never the cached ones. Before the world is first shown in the session (admission), a
// problem refuses entry and a pending consent or autostart holds the loading screen; once admitted, a problem
// (a later zone's rules, a required add-on that failed) is a warning, once for each failure, and play goes on.
class AddonAdmission {
public:
    enum class Step : std::uint8_t {
        Play,    // go on (the world shows)
        Start,   // hold the loading screen a frame: the new rules' autostart and consent prompt run first
        Consent, // hold the loading screen: a required plugin's permission question is open
        Refuse,  // back to login with `message`
    };
    struct Result {
        Step step = Step::Play;
        std::string message; // Refuse: why
        std::string warning; // non-empty: say it to the player (in the world, once for each failure)
    };
    // `rules`: the connection's, nullptr none (out of the world). `zone_problem` runs once for each new rules serial
    // of an enforcing server (the installed add-ons against them: "" none); `ready` asks the running add-ons
    // (nullopt: waiting for consent or a start, "" ready, else the failure).
    Result update(const plugin::Rules* rules, std::uint32_t serial, bool in_world,
                  const std::function<std::string()>& zone_problem,
                  const std::function<std::optional<std::string>()>& ready);
    void reset(); // a new world session
    bool admitted() const { return admitted_; }
    // Once admitted: the failure in force now ("" none), whether the running add-ons reported it (a required plugin
    // or script that failed: Retry may help) rather than the zone's rules, and a serial that changes with it. The
    // Server add-ons warning shows only while there is one (ClientApp::addon_policy_view).
    const std::string& failure() const { return failure_; }
    bool failure_retriable() const { return failure_retriable_; }
    std::uint64_t failure_serial() const { return failure_serial_; }

private:
    Result warn(std::string problem, bool retriable);
    std::optional<std::uint32_t> serial_;
    bool admitted_ = false, wait_frame_ = false;
    std::string zone_problem_, warned_;
    std::string failure_;
    bool failure_retriable_ = false;
    std::uint64_t failure_serial_ = 0;
};

} // namespace hailstone::app
