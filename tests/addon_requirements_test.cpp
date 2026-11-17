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

plugin::Rules minimum_rules(std::string minimum) {
    return plugin::parse_server_cfg("[scripts]\nplayer = on\n[plugins]\nplayer = on\n"
                                   "[addons]\nenforce = on\nrequired = maker/helper:" + minimum + "\n");
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

TEST_CASE("addon requirements: required manifest capabilities bypass player settings while explicit blocks win") {
    auto r = required_rules();
    r.addons.default_allow = false;
    for (const auto kind : {AddonKind::Script, AddonKind::Plugin}) {
        auto a = helper();
        a.kind = kind;
        a.caps = plugin::kCapRead | plugin::kCapMove;
        r.player_script_caps = r.player_plugin_caps = 0;
        CHECK(addon_requirement_problems(r, {a}).empty());
        r.player_script_caps = r.player_plugin_caps = plugin::kCapUi;
        CHECK(addon_requirement_problems(r, {a}).empty());
        r.addons.block = {"maker/helper"};
        const auto problems = addon_requirement_problems(r, {a});
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].message.find("blocked") != std::string::npos);
        r.addons.block.clear();
    }
}

TEST_CASE("addon requirements: player code off does not refuse a required managed package") {
    const auto r = plugin::parse_server_cfg("[scripts]\nplayer = off\n[plugins]\nplayer = off\n"
                                           "[addons]\nenforce = on\nrequired = maker/helper\ndefault = deny\n");
    REQUIRE(r.player_script_caps == 0);
    REQUIRE(r.player_plugin_caps == 0);
    auto a = helper();
    CHECK(addon_requirement_problems(r, {a}).empty());
    a.kind = AddonKind::Plugin;
    CHECK(addon_requirement_problems(r, {a}).empty());
    a.source = "other/helper";
    CHECK(addon_requirement_problems(r, {a}).size() == 1);
    auto disabled = r;
    disabled.addons.enabled = false;
    CHECK(addon_requirement_problems(disabled, {}).empty());
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

// A pack put in ui/ by hand (source builtin, but not one Hailstone ships) has no repository: under a server's
// default = deny it is refused as any unlisted add-on is, and so is an interface built on it; modern and gamepad stay.
TEST_CASE("addon requirements: a hand-placed ui/ pack is refused under default deny, built-in packs never") {
    ui::UiPack modern; modern.id = "modern";
    ui::UiPack gamepad; gamepad.id = "gamepad";
    ui::UiPack hand; hand.id = "myskin"; hand.base_ids = {"modern"};
    ui::UiPack derived; derived.id = "addon.maker.skin"; derived.source = "addon"; derived.addon = "maker";
    derived.base_ids = {"myskin", "modern"};
    InstalledAddon installed; installed.name = "maker"; installed.kind = AddonKind::Ui; installed.source = "maker/skin";
    auto rules = required_rules(); rules.addons.required.clear();
    rules.addons.allow = {"maker/skin"};
    const std::vector<ui::UiPack> fresh{modern, gamepad, hand, derived};
    auto packs = fresh;
    rules.addons.default_allow = true;
    apply_ui_addon_policy(packs, &rules, {installed});
    CHECK(packs == fresh);
    rules.addons.default_allow = false;
    apply_ui_addon_policy(packs, &rules, {installed});
    CHECK(ui::find_ui_pack_exact(packs, "modern")->available);
    CHECK(ui::find_ui_pack_exact(packs, "gamepad")->available);
    CHECK_FALSE(ui::find_ui_pack_exact(packs, "myskin")->available);
    CHECK(ui::find_ui_pack_exact(packs, "myskin")->unavailable_reason.find("by hand") != std::string::npos);
    CHECK_FALSE(ui::find_ui_pack_exact(packs, "addon.maker.skin")->available); // it inherits the refused pack's code
    CHECK(ui::find_ui_pack(packs, "myskin")->id == "modern");
}

TEST_CASE("ui packs: only the shipped modern and gamepad packs are trusted") {
    ui::UiPack pack; pack.id = "modern";
    CHECK(ui::trusted_ui_pack(pack));
    pack.id = "gamepad"; CHECK(ui::trusted_ui_pack(pack));
    pack.id = "myskin"; CHECK_FALSE(ui::trusted_ui_pack(pack)); // put in ui/ by hand
    pack.id = "modern"; pack.source = "addon"; CHECK_FALSE(ui::trusted_ui_pack(pack));
}

TEST_CASE("addon requirements: minimum versions apply to scripts, plugins, tools and upscalers") {
    const auto r = minimum_rules("1.2.3");
    REQUIRE(r.addons.error.empty());
    REQUIRE(r.addons.minimum_versions.size() == 1);
    for (const auto kind : {AddonKind::Script, AddonKind::Plugin, AddonKind::Tool, AddonKind::Upscaler}) {
        auto a = helper();
        a.kind = kind;
        a.version = "1.2.3";
        CHECK(addon_requirement_problems(r, {a}).empty());
        a.version = "1.2.4";
        CHECK(addon_requirement_problems(r, {a}).empty());
        a.version = "1.2.2";
        CHECK(addon_requirement_problems(r, {a}).size() == 1);
        a.version.clear();
        CHECK(addon_requirement_problems(r, {a}).size() == 1);
    }
}

TEST_CASE("addon requirements: minimum versions use semantic numeric and prerelease precedence") {
    const auto r = minimum_rules("1.0.0-preview.4");
    REQUIRE(r.addons.error.empty());
    auto a = helper();
    for (const std::string version : {"1.0.0-preview.4", "1.0.0-preview.10", "1.0.0", "1.0.1", "2.0.0",
                                      "v1.0.0-preview.4+windows.1"}) {
        CAPTURE(version);
        a.version = version;
        CHECK(addon_requirement_problems(r, {a}).empty());
    }
    for (const std::string version : {"0.99.99", "1.0.0-preview.3", "1.0.0-alpha.100", "1.0.0-preview"}) {
        CAPTURE(version);
        a.version = version;
        CHECK(addon_requirement_problems(r, {a}).size() == 1);
    }
    // A prerelease of the same numeric version does not meet a final-release minimum.
    a.version = "1.0.0-preview.10";
    CHECK(addon_requirement_problems(minimum_rules("1.0.0"), {a}).size() == 1);
}

TEST_CASE("addon requirements: prerelease case matters while build metadata does not") {
    auto a = helper();
    a.version = "1.0.0-Alpha";
    CHECK(addon_requirement_problems(minimum_rules("1.0.0-alpha"), {a}).size() == 1);
    a.version = "1.0.0-alpha";
    CHECK(addon_requirement_problems(minimum_rules("1.0.0-Alpha"), {a}).empty());
    a.version = "1.0.0-preview.4+linux.1";
    CHECK(addon_requirement_problems(minimum_rules("1.0.0-preview.4+windows.2"), {a}).empty());
}

TEST_CASE("addon requirements: absent or invalid installed versions fail only a versioned requirement") {
    const auto r = minimum_rules("1.2.3");
    auto a = helper();
    for (const std::string version : {"", "unknown", "1.2", "1.2.3-01", "1.2.3+"}) {
        CAPTURE(version);
        a.version = version;
        const auto problems = addon_requirement_problems(r, {a});
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].message.find("no valid installed version") != std::string::npos);
        CHECK(problems[0].message.find("1.2.3 or newer") != std::string::npos);
        CHECK(addon_requirement_problems(required_rules(), {a}).empty());
    }
    auto disabled = r;
    disabled.addons.enabled = false;
    CHECK(addon_requirement_problems(disabled, {}).empty());
}

TEST_CASE("addon requirements: minimum version failures retain plain repository identities for review") {
    const auto r = minimum_rules("1.0.0-preview.4");
    const auto missing = addon_requirement_problems(r, {});
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].identity == "maker/helper");
    CHECK(missing[0].message.find("1.0.0-preview.4 or newer") != std::string::npos);
    auto a = helper();
    a.version = "1.0.0-preview.3";
    const auto problems = addon_requirement_problems(r, {a});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].identity == "maker/helper");
    CHECK(problems[0].message.find("1.0.0-preview.4 or newer") != std::string::npos);
    CHECK(problems[0].message.find("installed version is 1.0.0-preview.3") != std::string::npos);
    const auto unlisted = required_addon_suggestions(problems, nullptr);
    REQUIRE(unlisted.size() == 1);
    CHECK(unlisted[0].source == "maker/helper");
    AddonIndex index;
    index.addons.push_back({"helper", "Maker/Helper", {}});
    const auto listed = required_addon_suggestions(problems, &index);
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].name == "helper");
    CHECK(listed[0].source == "Maker/Helper");
    ui::LoginView view;
    present_addon_requirement(view, problems, nullptr, problems[0].message);
    CHECK(view.addons.link == "maker/helper");
}

TEST_CASE("addon requirements: a new version still needs an intact permitted package and native consent") {
    auto r = minimum_rules("1.2.3");
    auto a = helper();
    a.version = "2.0.0";
    a.intact = false;
    CHECK(addon_requirement_problems(r, {a}).size() == 1);
    a.intact = true;
    a.startable = false;
    CHECK(addon_requirement_problems(r, {a}).size() == 1);
    a.startable = true;
    r.player_script_caps = 0;
    CHECK(addon_requirement_problems(r, {a}).empty());
    r.player_script_caps = plugin::kCapAll;
    r.addons.block = {"maker/helper"};
    CHECK(addon_requirement_problems(r, {a}).size() == 1);
    r.addons.block.clear();
    a.kind = AddonKind::Plugin;
    a.consent = plugin::Consent::Denied;
    CHECK(addon_requirement_problems(r, {a}).size() == 1);
}

TEST_CASE("addon requirements: a newer required script or plugin still needs supported manifest capabilities") {
    auto r = minimum_rules("1.2.3");
    r.player_script_caps = r.player_plugin_caps = 0;
    for (const auto kind : {AddonKind::Script, AddonKind::Plugin}) {
        auto a = helper();
        a.kind = kind;
        a.version = "2.0.0";
        a.caps = 0;
        const auto problems = addon_requirement_problems(r, {a});
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].message.find("declares no capabilities") != std::string::npos);
        a.caps = 1u << 31; // no supported bit: the policy would return no execution capabilities
        CHECK(addon_requirement_problems(r, {a}).size() == 1);
        a.caps = plugin::kCapRead;
        CHECK(addon_requirement_problems(r, {a}).empty());
    }
}

TEST_CASE("addon requirements: candidate versions cannot substitute for their repository or intact files") {
    const auto r = minimum_rules("1.2.3");
    auto old = helper();
    old.version = "1.2.2";
    old.source = "maker/helper@v9.0.0"; // the source's tag is not the installed manifest version
    auto newer = helper();
    newer.version = "1.2.4";
    newer.source = "other/helper";
    CHECK(addon_requirement_problems(r, {old, newer}).size() == 1);
    newer.source = "maker/helper";
    newer.intact = false;
    CHECK(addon_requirement_problems(r, {old, newer}).size() == 1);
    newer.intact = true;
    CHECK(addon_requirement_problems(r, {old, newer}).empty());
    CHECK(addon_requirement_problems(r, {newer, old}).empty());
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
