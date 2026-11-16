#include <doctest/doctest.h>

#include "app/addon_requirements.h"
#include "addon_test_util.h"
#include "scratch_dir.h"
#include "ui/dev_screens.h"
#include "ui/ui_packs.h"

using namespace hailstone;
using namespace hailstone::app;

namespace {

plugin::Rules required_rules(std::string identity = "maker/helper") {
    plugin::Rules r;
    r.configured = true;
    r.player_script_caps = plugin::kCapAll;
    r.player_plugin_caps = plugin::kCapAll;
    r.addons.enabled = true;
    r.addons.required.push_back(std::move(identity));
    return r;
}

RequiredAddonCandidate helper() {
    return {"helper", "maker/helper", AddonKind::Script, plugin::kCapRead, true};
}

} // namespace

TEST_CASE("addon requirements: servers must opt in and invalid opted-in policy fails closed") {
    auto r = required_rules();
    r.addons.enabled = false;
    r.addons.error = "unknown field";
    CHECK(addon_requirement_problems(r, {}).empty());
    r.addons.enabled = true;
    auto problems = addon_requirement_problems(r, {});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].message.find("invalid") != std::string::npos);
}

TEST_CASE("addon requirements: a repository must match even when another package has its name") {
    auto r = required_rules();
    auto a = helper();
    CHECK(addon_requirement_problems(r, {a}).empty());
    a.source = "other/helper";
    CHECK(addon_requirement_problems(r, {a}).size() == 1);
    a.source.clear(); // a hand-installed copy of the same name has no identity
    CHECK(addon_requirement_problems(r, {a}).size() == 1);
    a.source = "Maker/Helper@v2";
    CHECK(addon_requirement_problems(r, {a}).empty());
}

TEST_CASE("addon requirements: missing or modified packages cannot satisfy a requirement; a tool's install does") {
    const auto r = required_rules();
    CHECK(addon_requirement_problems(r, {}).size() == 1);
    auto a = helper();
    a.intact = false;
    auto problems = addon_requirement_problems(r, {a});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].message.find("missing or changed") != std::string::npos);
    // A tool (Texture Studio, say) is a program of its own: installed and intact is all the client can check.
    a.kind = AddonKind::Tool;
    a.caps = 0;
    CHECK(addon_requirement_problems(r, {a}).size() == 1);
    a.intact = true;
    CHECK(addon_requirement_problems(r, {a}).empty());
    auto blocked = r;
    blocked.addons.block = {"maker/helper"};
    CHECK(addon_requirement_problems(blocked, {a}).size() == 1);
}

TEST_CASE("addon requirements: blocked identities and incompatible capabilities refuse login") {
    auto r = required_rules();
    const auto a = helper();
    r.addons.block = {"maker/helper"};
    auto problems = addon_requirement_problems(r, {a});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].message.find("blocked") != std::string::npos);
    r.addons.block.clear();
    r.player_script_caps = plugin::kCapUi;
    problems = addon_requirement_problems(r, {a});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].message.find("capabilities") != std::string::npos);
    r.player_script_caps = 0;
    auto zero = a;
    zero.caps = 0;
    CHECK(addon_requirement_problems(r, {zero}).size() == 1);
}

TEST_CASE("addon requirements: native consent remains explicit and refusal prevents joining") {
    auto a = helper();
    a.kind = AddonKind::Plugin;
    const auto r = required_rules();
    CHECK(addon_requirement_problems(r, {a}).empty()); // the existing runtime prompt asks before loading
    a.consent = plugin::Consent::Denied;
    auto problems = addon_requirement_problems(r, {a});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].message.find("not allowed") != std::string::npos);
    a.consent = plugin::Consent::Allowed;
    CHECK(addon_requirement_problems(r, {a}).empty());
}

TEST_CASE("addon requirements: installed upscalers require integrity and defer activation to startup check") {
    auto a = helper();
    a.kind = AddonKind::Upscaler;
    a.caps = 0;
    const auto r = required_rules();
    CHECK(addon_requirement_problems(r, {a}).empty());
    a.intact = false;
    CHECK(addon_requirement_problems(r, {a}).size() == 1);
}

TEST_CASE("addon requirements: required interfaces need intact installation without script startup or selection") {
    auto addon = helper();
    addon.kind = AddonKind::Ui;
    addon.caps = 0;
    addon.startable = false; // interface packages intentionally have no /lua entrypoint
    auto rules = required_rules();
    rules.player_script_caps = rules.player_plugin_caps = 0;
    CHECK(addon_requirement_problems(rules, {addon}).empty());
    addon.intact = false;
    REQUIRE(addon_requirement_problems(rules, {addon}).size() == 1);
    CHECK(addon_requirement_problems(rules, {addon})[0].message.find("missing or changed files") != std::string::npos);
}

TEST_CASE("addon requirements: UI repository policy blocks owners and inherited code while retaining builtin fallback") {
    ui::UiPack builtin; builtin.id = "modern";
    ui::UiPack owner; owner.id = "addon.interfaces.skin"; owner.source = "addon"; owner.addon = "interfaces";
    ui::UiPack child; child.id = "addon.child.skin"; child.source = "addon"; child.addon = "child";
    child.base_ids = {owner.id, builtin.id};
    ui::UiPack grandchild; grandchild.id = "addon.other.skin"; grandchild.source = "addon"; grandchild.addon = "other";
    grandchild.base_ids = {child.id};
    const std::vector<ui::UiPack> fresh{grandchild, child, owner, builtin};
    InstalledAddon installed; installed.name = "INTERFACES"; installed.kind = AddonKind::Ui;
    installed.source = "maker/interfaces";
    auto rules = required_rules(); rules.addons.required.clear(); rules.addons.block = {"maker/interfaces"};
    auto packs = fresh;
    apply_ui_addon_policy(packs, &rules, {installed});
    for (const auto& id : {owner.id, child.id, grandchild.id}) {
        const auto* pack = ui::find_ui_pack_exact(packs, id);
        REQUIRE(pack); CHECK_FALSE(pack->available); CHECK_FALSE(pack->unavailable_reason.empty());
        REQUIRE(ui::find_ui_pack(packs, id)); CHECK(ui::find_ui_pack(packs, id)->id == "modern");
    }
    REQUIRE(ui::find_ui_pack_exact(packs, "modern")); CHECK(ui::find_ui_pack_exact(packs, "modern")->available);
    for (int context = 0; context < 3; ++context) {
        auto unruled = fresh;
        auto local = rules;
        if (context == 0) local.addons.enabled = false;
        if (context == 1) local.provisional = true;
        apply_ui_addon_policy(unruled, context == 2 ? nullptr : &local, {installed});
        CHECK(unruled == fresh); // cached/absent/opted-out policy restores through fresh registry metadata
    }
    rules.addons.block.clear();
    packs = fresh;
    apply_ui_addon_policy(packs, &rules, {installed});
    CHECK(packs == fresh);
}

TEST_CASE("addon requirements: required offers keep index identity and key without optional dismissals") {
    AddonIndex index;
    Ed25519PublicKey key{};
    key[0] = 17;
    index.addons.push_back({"helper", "maker/helper", key});
    const auto offers = required_addon_suggestions({{"Maker/Helper", "missing"}, {"maker/helper", "changed"},
                                                   {"other/addon", "missing"}, {"", "invalid policy"}}, &index);
    REQUIRE(offers.size() == 2);
    CHECK(offers[0].name == "helper");
    CHECK(offers[0].source == "maker/helper");
    REQUIRE(offers[0].key);
    CHECK(*offers[0].key == key);
    CHECK(offers[0].from_server);
    CHECK(offers[1].source == "other/addon");
    CHECK_FALSE(offers[1].key);
}

TEST_CASE("addon requirements: presenting a required review fetches nothing before the player's click") {
    test::ScratchDir scratch("required_review");
    addon_test::FakeGitHub github;
    const std::string zip = addon_test::zip_of({{"init.lua", "-- helper"}});
    github.release("maker/helper", addon_test::manifest_for("helper", "script", "1.0.0", "read", "any", zip,
                                                           "helper.zip"), "helper.zip", zip);
    AddonsEnv env;
    env.data_dir = scratch.path();
    env.platform = addon_test::this_platform();
    env.client_version = "1.0.0";
    env.fetch = github.fetch();
    AddonManager manager(std::move(env));
    ui::LoginView view;
    view.busy = true;
    present_addon_requirement(view, {{"maker/helper", "Required add-on is missing."}}, nullptr,
                              "Required add-on is missing.");
    manager.wait_idle();
    CHECK(github.asked.empty());
    CHECK_FALSE(manager.status().offer);
    CHECK_FALSE(view.busy);
    CHECK(view.addons.open);
    CHECK(view.addons.link == "maker/helper");
    CHECK(view.status.is_error);
    // The existing Review action explicitly starts inspection; inspection still does not install a package.
    manager.inspect(view.addons.link);
    manager.wait_idle();
    CHECK_FALSE(github.asked.empty());
    CHECK(manager.status().offer.has_value());
    CHECK(read_installed_addons(scratch.path()).empty());
}

namespace {

// Drives AddonAdmission as ClientApp does, counting the calls the gate makes.
struct Gate {
    AddonAdmission admission;
    std::string zone_problem;
    std::optional<std::string> ready = std::string();
    int zone_checks = 0, ready_checks = 0;
    AddonAdmission::Result frame(const plugin::Rules* rules, std::uint32_t serial, bool in_world) {
        return admission.update(rules, serial, in_world, [&] { ++zone_checks; return zone_problem; },
                                [&] { ++ready_checks; return ready; });
    }
};

using Step = AddonAdmission::Step;

} // namespace

TEST_CASE("addon admission: decided at the zone-in by its rules, never before or by the cached ones") {
    Gate g;
    auto rules = required_rules();
    // Out of the world, and on the provisional (cached) rules of the world login and character select: no check.
    CHECK(g.frame(nullptr, 1, false).step == Step::Play);
    rules.provisional = true;
    g.zone_problem = "Required add-on maker/helper is not installed.";
    CHECK(g.frame(&rules, 2, false).step == Step::Play);
    CHECK(g.zone_checks == 0);
    // The zone-in's rules: a missing requirement refuses entry before the world is shown.
    rules.provisional = false;
    const auto refused = g.frame(&rules, 3, false);
    CHECK(refused.step == Step::Refuse);
    CHECK(refused.message == g.zone_problem);
    CHECK(refused.warning.empty());
    CHECK_FALSE(g.admission.admitted());
    // The server fixed its rules: the next zone-in's are decided afresh.
    g.admission.reset();
    g.zone_problem.clear();
    CHECK(g.frame(&rules, 4, false).step == Step::Play); // still loading
    CHECK(g.frame(&rules, 4, true).step == Step::Start); // autostart and the consent prompt run first
    g.ready = std::nullopt;
    CHECK(g.frame(&rules, 4, true).step == Step::Consent);
    CHECK(g.frame(&rules, 4, true).step == Step::Consent);
    g.ready = std::string();
    CHECK(g.frame(&rules, 4, true).step == Step::Play);
    CHECK(g.admission.admitted());
    CHECK(g.zone_checks == 2);
}

TEST_CASE("addon admission: a failure before the world shows refuses; after, it warns once and play goes on") {
    Gate g;
    const auto rules = required_rules();
    g.frame(&rules, 1, true);
    g.ready = "Required add-on helper did not load";
    const auto refused = g.frame(&rules, 1, true);
    CHECK(refused.step == Step::Refuse);
    CHECK(refused.message == *g.ready);

    g.admission.reset();
    g.ready = std::string();
    g.frame(&rules, 1, true);
    REQUIRE(g.frame(&rules, 1, true).step == Step::Play);
    REQUIRE(g.admission.admitted());
    // Mid-session: never refused, never held; each failure is said once.
    g.ready = "Required add-on helper did not remain running";
    auto r = g.frame(&rules, 1, true);
    CHECK(r.step == Step::Play);
    CHECK(r.warning == *g.ready);
    for (int i = 0; i < 3; ++i) {
        r = g.frame(&rules, 1, true);
        CHECK(r.step == Step::Play);
        CHECK(r.warning.empty());
    }
    g.ready = std::nullopt; // a consent question in the world does not hold the screen
    CHECK(g.frame(&rules, 1, true).step == Step::Play);
    g.ready = std::string();
    CHECK(g.frame(&rules, 1, true).warning.empty());
    g.ready = "Required add-on helper did not remain running";
    CHECK(g.frame(&rules, 1, true).warning == *g.ready); // failed again: said again
    g.ready = "Required add-on other did not load";
    CHECK(g.frame(&rules, 1, true).warning == *g.ready);
}

TEST_CASE("addon admission: a later zone's rules only warn once the session is admitted") {
    Gate g;
    auto rules = required_rules();
    g.frame(&rules, 1, true);
    REQUIRE(g.frame(&rules, 1, true).step == Step::Play);
    REQUIRE(g.admission.admitted());
    // Zoning: the next zone-in brings rules the installed add-ons do not meet.
    g.zone_problem = "Required add-on maker/other is not installed. Open Add-ons to install it.";
    rules.addons.required.push_back("maker/other");
    auto r = g.frame(&rules, 2, false);
    CHECK(r.step == Step::Play);
    CHECK(r.warning == g.zone_problem);
    CHECK(g.frame(&rules, 2, true).warning.empty());
    // Back at character select, admission starts over: the next entry is refused.
    plugin::Rules cached = rules;
    cached.provisional = true;
    CHECK(g.frame(&cached, 3, false).step == Step::Play);
    CHECK_FALSE(g.admission.admitted());
    CHECK(g.frame(&rules, 4, false).step == Step::Refuse);
}

TEST_CASE("addon admission: a server that does not enforce never asks the add-ons") {
    Gate g;
    plugin::Rules rules;
    rules.configured = true;
    g.zone_problem = "not consulted";
    CHECK(g.frame(&rules, 1, false).step == Step::Play);
    CHECK(g.frame(&rules, 1, true).step == Step::Play);
    CHECK(g.admission.admitted());
    CHECK(g.frame(&rules, 1, true).step == Step::Play);
    CHECK(g.zone_checks == 0);
    CHECK(g.ready_checks == 0);
}

TEST_CASE("addon requirements: a required script is found whatever its case, as the script loader finds it") {
    test::ScratchDir scratch("required_script_case");
    addon_test::put_file(addon_folder(scratch.path(), AddonKind::Script, "helper") / "init.lua", "-- helper");
    addon_test::put_file(addon_folder(scratch.path(), AddonKind::Script, "MixedCase") / "init.lua", "-- mixed");
    CHECK(required_script_found(scratch.path(), "helper"));
    CHECK(required_script_found(scratch.path(), "MixedCase"));
    // The registry's name is the add-on's own folder: another spelling is the folder the loader would start only
    // where the file system folds case.
    CHECK(required_script_found(scratch.path(), "HELPER") ==
          std::filesystem::exists(scratch.path() / "scripts" / "HELPER"));
    CHECK_FALSE(required_script_found(scratch.path(), "absent"));
    // A folder whose name differs only in case: the loader takes the exact (lower-case) one, never MixedCase's.
    if (std::filesystem::exists(scratch.path() / "scripts" / "mixedcase")) {
        MESSAGE("case-insensitive file system: two folders differing only in case cannot exist");
        return;
    }
    addon_test::put_file(addon_folder(scratch.path(), AddonKind::Script, "mixedcase") / "init.lua", "-- other");
    CHECK_FALSE(required_script_found(scratch.path(), "MixedCase"));
    CHECK(required_script_found(scratch.path(), "mixedcase"));

    const auto r = required_rules();
    auto a = helper();
    a.name = "MixedCase";
    a.startable = false;
    auto problems = addon_requirement_problems(r, {a});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].message.find("scripts/MixedCase") != std::string::npos);
    a.kind = AddonKind::Plugin; // only scripts are looked up by a lower-cased name
    CHECK(addon_requirement_problems(r, {a}).empty());
}
