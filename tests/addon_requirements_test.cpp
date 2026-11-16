#include <doctest/doctest.h>

#include "app/addon_requirements.h"
#include "addon_test_util.h"
#include "scratch_dir.h"
#include "ui/dev_screens.h"

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
    r.addons.required = {"OTHER/HELPER"};
    CHECK(addon_requirement_problems(r, {a}).empty());
    a.source.clear();
    a.name = "OTHER/HELPER";
    CHECK(addon_requirement_problems(r, {a}).size() == 1);
}

TEST_CASE("addon requirements: missing modified and non-game packages cannot satisfy a requirement") {
    const auto r = required_rules();
    CHECK(addon_requirement_problems(r, {}).size() == 1);
    auto a = helper();
    a.intact = false;
    auto problems = addon_requirement_problems(r, {a});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].message.find("missing or changed") != std::string::npos);
    a.intact = true;
    a.kind = AddonKind::Tool;
    problems = addon_requirement_problems(r, {a});
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].message.find("separate tool") != std::string::npos);
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

TEST_CASE("addon requirements: verified server files do not prove a managed repository identity") {
    auto r = required_rules();
    r.player_script_caps = 0;
    r.server_files = {{plugin::FileKind::Script, "ScRiPtS\\Helper\\InIt.LuA", sha256("-- verified server entry")}};
    auto a = helper();
    a.source.clear();
    a.server_owned = true;
    CHECK(addon_requirement_problems(r, {a}).size() == 1);
    a.intact = false;
    CHECK(addon_requirement_problems(r, {a}).size() == 1);
    a.intact = true;
    CHECK(addon_requirement_problems(r, {a}).size() == 1);
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

TEST_CASE("addon requirements: required offers keep index identity and key without optional dismissals") {
    AddonIndex index;
    Ed25519PublicKey key{};
    key[0] = 17;
    index.addons.push_back({"helper", "maker/helper", key});
    const auto offers = required_addon_suggestions({{"maker/helper", "missing"}, {"maker/helper", "changed"},
                                                   {"other/addon", "missing"}, {"unknown-name", "missing"}}, &index);
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
