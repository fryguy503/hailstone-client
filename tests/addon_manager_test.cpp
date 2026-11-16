// The add-on manager (app/addon_manager.h, docs/addons.md) against GitHub in memory: a link's manifest becomes the
// install card, the player's yes downloads, checks and installs it; packages that do not match or that come from
// another host are refused; updates at start are installed when they ask for nothing more, wait for the player
// when they ask for more capabilities or would overwrite the player's changes, and are refused when signed with
// another key; and the check interval holds.

#include <doctest/doctest.h>

#include <algorithm>

#include "addon_test_util.h"
#include "app/addon_install.h"
#include "app/addon_manager.h"
#include "plugin/policy.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::app;
using namespace hailstone::addon_test;
namespace fs = std::filesystem;

namespace {

const std::string kRepo = "owner/autoloot";

struct Harness {
    test::ScratchDir dir{"addon_manager"};
    FakeGitHub github;
    std::int64_t clock = 1'000'000;
    AddonSettings settings;
    // Called on the worker with each address it fetches, before the fetch (the tests' way to act between two steps
    // of a check).
    std::function<void(const std::string&)> on_fetch;
    std::unique_ptr<AddonManager> manager;

    void start() {
        AddonsEnv env;
        env.data_dir = dir.path();
        env.client_version = "0.2.0-alpha";
        env.platform = this_platform();
        env.fetch = [this, inner = github.fetch()](const std::string& url, const auto& allow, const auto& sink,
                                                   const auto& keep_going) {
            if (on_fetch) on_fetch(url);
            return inner(url, allow, sink, keep_going);
        };
        env.settings = settings;
        env.now = [this] { return clock; };
        manager = std::make_unique<AddonManager>(std::move(env));
        manager->wait_idle();
    }

    // A release of the script on the fake GitHub.
    void publish(const std::string& version, const std::string& caps, const std::string& init = "-- script\n",
                 const Keys* key = nullptr, const std::string& extra = {}) {
        const std::string zip = zip_of({{"init.lua", init}, {"data.txt", "v" + version}});
        const std::string file = "autoloot-" + version + ".zip";
        std::string m = manifest_for("autoloot", "script", version, caps, "any", zip, file, extra);
        if (key) m = key->sign(m);
        github.release(kRepo, m, file, zip);
    }

    // A release of a second script, owner/autobuff.
    void publish_buff(const std::string& version) {
        const std::string zip = zip_of({{"init.lua", "-- buff " + version + "\n"}});
        const std::string file = "autobuff-" + version + ".zip";
        github.release("owner/autobuff", manifest_for("autobuff", "script", version, "read", "any", zip, file), file,
                       zip);
    }

    AddonsStatus inspect(const std::string& link) {
        manager->inspect(link);
        manager->wait_idle();
        return manager->status();
    }

    // Inspect, say yes, install.
    void install(const std::string& link = "https://github.com/" + kRepo) {
        const AddonsStatus s = inspect(link);
        REQUIRE_MESSAGE(s.offer, s.message);
        REQUIRE_MESSAGE(s.offer->blocked.empty(), s.offer->blocked);
        manager->install_offer();
        manager->wait_idle();
        REQUIRE_MESSAGE(manager->has_ready(), manager->status().message);
        REQUIRE(manager->install_ready().size() == 1);
    }

    const AddonStatus* row(const std::string& name) {
        static AddonsStatus last;
        last = manager->status();
        for (const auto& r : last.addons)
            if (r.addon.name == name) return &r;
        return nullptr;
    }

    // The player's Update / Approve on the row as it shows now (ClientApp::addons_login_action): what they saw.
    void update(const std::string& name) {
        const AddonStatus* r = row(name);
        REQUIRE(r);
        manager->update(name, r->state == AddonState::NeedsApproval ? r->new_caps : r->addon.caps, r->new_version);
    }

    fs::path folder() const { return addon_folder(dir.path(), AddonKind::Script, "autoloot"); }
};

} // namespace

TEST_CASE("addon_manager: managed activation stays separate from update preferences through install and upgrade") {
    Harness h;
    h.publish("1.0.0", "read,ui", "-- managed\n", nullptr, "autoload 1\n");
    h.start();
    h.install();
    REQUIRE(h.row("autoloot"));
    CHECK(h.row("autoloot")->addon.autoload);
    CHECK(h.row("autoloot")->addon.enabled);
    CHECK_FALSE(h.row("autoloot")->addon.auto_update);
    h.manager->set_auto_update("autoloot", true);
    const auto serial = h.manager->serial();
    CHECK(h.manager->set_enabled("autoloot", false));
    CHECK(h.manager->serial() != serial);
    CHECK_FALSE(h.row("autoloot")->addon.enabled);
    CHECK(h.row("autoloot")->addon.auto_update);
    CHECK_FALSE(read_installed_addons(h.dir.path()).front().enabled);
    CHECK_FALSE(h.manager->set_enabled("missing", true));
    h.publish("1.0.1", "read,ui", "-- managed update\n", nullptr, "autoload 1\n");
    h.install();
    REQUIRE(h.row("autoloot"));
    CHECK(h.row("autoloot")->addon.version == "1.0.1");
    CHECK_FALSE(h.row("autoloot")->addon.enabled);
    CHECK(h.row("autoloot")->addon.auto_update);
    CHECK(h.manager->set_enabled("autoloot", true));
    CHECK(h.row("autoloot")->addon.enabled);
}

TEST_CASE("addon_manager: a link's manifest is the install card, and yes installs it") {
    Harness h;
    h.publish("1.0.0", "read,ui,actions");
    h.start();
    const AddonsStatus s = h.inspect("https://github.com/owner/autoloot");
    REQUIRE_MESSAGE(s.offer, s.message);
    CHECK_FALSE(s.inspecting);
    CHECK(s.offer->source == kRepo);
    CHECK(s.offer->page == "https://github.com/owner/autoloot");
    CHECK(s.offer->manifest.name == "autoloot");
    CHECK(s.offer->caps == (plugin::kCapRead | plugin::kCapUi | plugin::kCapActions));
    CHECK(s.offer->installed_version.empty());
    CHECK(s.offer->blocked.empty());
    CHECK_FALSE(fs::exists(h.folder())); // nothing is written before the yes

    h.manager->install_offer();
    h.manager->wait_idle();
    CHECK_FALSE(h.manager->status().offer);
    const AddonStatus* r = h.row("autoloot");
    REQUIRE(r);
    CHECK(r->state == AddonState::Ready);
    CHECK_FALSE(r->installed);
    CHECK_FALSE(fs::exists(h.folder() / "init.lua")); // the files move only through install_ready()

    CHECK(h.manager->install_ready() == std::vector<std::string>{"autoloot"});
    CHECK(read_file(h.folder() / "init.lua") == "-- script\n");
    r = h.row("autoloot");
    REQUIRE(r);
    CHECK(r->installed);
    CHECK(r->state == AddonState::UpToDate);
    const auto installed = read_installed_addons(h.dir.path());
    REQUIRE(installed.size() == 1);
    CHECK(installed[0].source == kRepo);
    CHECK(installed[0].caps == (plugin::kCapRead | plugin::kCapUi | plugin::kCapActions));
    CHECK(h.manager->status().serial > 0);
    CHECK(h.manager->serial() == h.manager->status().serial); // what the world reads each frame
    // The package goes once installed.
    CHECK(fs::is_empty(addons_dir(h.dir.path()) / "download"));

    // The same link again: an update offer of the installed one (nothing newer, but it can be put back).
    const AddonsStatus again = h.inspect("owner/autoloot");
    REQUIRE(again.offer);
    CHECK(again.offer->installed_version == "1.0.0");
}

TEST_CASE("addon_manager: signed UI bundles install without scripts and preserve their kind and pinned signer") {
    Harness h;
    Keys author(43), other(44);
    auto publish = [&](const std::string& version, const Keys& key, const std::string& kind = "ui") {
        const auto zip = zip_of({{"ui/example/ui.ini", "name=Example\nbase=builtin:modern\n"}});
        const auto manifest = manifest_for("interfaces", kind, version, "none", "any", zip, "interfaces.zip");
        h.github.release("owner/interfaces", key.sign(manifest), "interfaces.zip", zip);
    };
    publish("1.0.0", author);
    h.settings.allow_unsigned = false;
    h.start();
    h.install("owner/interfaces");
    const auto folder = h.dir.path() / "ui-addons/interfaces";
    CHECK(fs::is_regular_file(folder / "ui/example/ui.ini"));
    CHECK_FALSE(fs::exists(h.dir.path() / "scripts/interfaces"));
    const auto* installed = h.row("interfaces");
    REQUIRE(installed);
    CHECK(installed->addon.kind == AddonKind::Ui);
    CHECK(installed->addon.caps == 0);
    publish("2.0.0", other);
    auto offer = h.inspect("owner/interfaces");
    REQUIRE(offer.offer);
    CHECK_FALSE(offer.offer->blocked.empty());
    publish("2.0.0", author);
    h.install("owner/interfaces");
    CHECK(read_installed_addons(h.dir.path()).front().version == "2.0.0");
    // The trust grant belongs to this kind; another kind cannot silently inherit it.
    const auto script = zip_of({{"init.lua", "return true"}});
    h.github.release("owner/interfaces", author.sign(manifest_for("interfaces", "script", "3.0.0", "read,ui", "any", script, "script.zip")),
                     "script.zip", script);
    offer = h.inspect("owner/interfaces");
    REQUIRE(offer.offer);
    CHECK_FALSE(offer.offer->blocked.empty());
    std::string why;
    REQUIRE_MESSAGE(h.manager->remove("interfaces", &why), why);
    CHECK_FALSE(fs::exists(folder / "ui/example/ui.ini"));
}

// A link to an older release than the one installed (a tag, or a release taken back) shows as older, and only the
// player's second yes installs it.
TEST_CASE("addon_manager: an older version from a link needs an explicit yes") {
    Harness h;
    h.publish("1.2.0", "read,ui");
    h.start();
    h.install();
    h.publish("1.0.0", "read,ui");
    AddonsStatus s = h.inspect("owner/autoloot");
    REQUIRE(s.offer);
    CHECK(s.offer->older);
    CHECK(s.offer->installed_version == "1.2.0");
    CHECK(s.offer->blocked.empty());
    h.manager->install_offer(); // the plain yes: not enough
    h.manager->wait_idle();
    s = h.manager->status();
    REQUIRE(s.offer); // still up
    CHECK(s.message.find("older") != std::string::npos);
    CHECK_FALSE(h.manager->has_ready());
    CHECK(read_installed_addons(h.dir.path())[0].version == "1.2.0");
    h.manager->install_offer(true);
    h.manager->wait_idle();
    REQUIRE(h.manager->has_ready());
    REQUIRE(h.manager->install_ready().size() == 1);
    CHECK(read_installed_addons(h.dir.path())[0].version == "1.0.0");
    // The same version again, or a newer one, is no downgrade.
    s = h.inspect("owner/autoloot");
    REQUIRE(s.offer);
    CHECK_FALSE(s.offer->older);
}

TEST_CASE("addon_manager: links that are not add-ons, and what an offer cannot install") {
    Harness h;
    h.start();
    AddonsStatus s = h.inspect("https://gitlab.com/owner/repo");
    CHECK_FALSE(s.offer);
    CHECK(s.message_error);
    CHECK(s.message.find("GitHub") != std::string::npos);
    s = h.inspect("owner/nothing");
    CHECK_FALSE(s.offer);
    CHECK(s.message.find("not a Hailstone add-on") != std::string::npos);
    // Both addresses were tried: the release's asset, then the default branch's file.
    CHECK(h.github.asked.size() == 2);

    // A manifest only in the branch (no release) is found there.
    const std::string zip = zip_of({{"init.lua", "--\n"}});
    h.github.files["https://raw.githubusercontent.com/owner/branchy/HEAD/hailstone-addon.txt"] =
        manifest_for("branchy", "script", "0.1.0", "read", "any", zip, "branchy.zip");
    s = h.inspect("owner/branchy");
    REQUIRE_MESSAGE(s.offer, s.message);
    CHECK(s.offer->manifest_url.starts_with("https://raw.githubusercontent.com/"));

    // A newer Hailstone needed; a capability this one does not know; no build for this platform.
    h.publish("1.0.0", "read", "--\n", nullptr, "min_client 9.0.0\n");
    s = h.inspect(kRepo);
    REQUIRE(s.offer);
    CHECK(s.offer->blocked.find("needs Hailstone 9.0.0") != std::string::npos);
    h.publish("1.0.0", "read,teleport");
    s = h.inspect(kRepo);
    REQUIRE(s.offer);
    CHECK(s.offer->blocked.find("does not know") != std::string::npos);
    h.github.files["https://github.com/owner/plug/releases/latest/download/hailstone-addon.txt"] =
        manifest_for("plug", "plugin", "1.0.0", "read", "someos-x64", "z", "plug.zip");
    s = h.inspect("owner/plug");
    REQUIRE(s.offer);
    CHECK(s.offer->blocked.find("no build") != std::string::npos);

    // A blocked offer installs nothing.
    h.publish("1.0.0", "read,teleport");
    h.inspect(kRepo);
    h.manager->install_offer();
    h.manager->wait_idle();
    CHECK_FALSE(h.manager->has_ready());

    // A folder of that name installed by hand.
    put_file(h.folder() / "init.lua", "-- mine\n");
    h.publish("1.0.0", "read");
    s = h.inspect(kRepo);
    REQUIRE(s.offer);
    CHECK(s.offer->blocked.find("by hand") != std::string::npos);
}

TEST_CASE("addon_manager: unsigned add-ons can be switched off") {
    Harness h;
    h.settings.allow_unsigned = false;
    h.publish("1.0.0", "read");
    h.start();
    AddonsStatus s = h.inspect(kRepo);
    REQUIRE(s.offer);
    CHECK(s.offer->blocked.find("not signed") != std::string::npos);
    const Keys k;
    h.publish("1.0.0", "read", "--\n", &k);
    s = h.inspect(kRepo);
    REQUIRE(s.offer);
    CHECK(s.offer->blocked.empty());
    REQUIRE(s.offer->manifest.key);
}

TEST_CASE("addon_manager: a package that does not match, or comes from elsewhere, is refused") {
    Harness h;
    h.publish("1.0.0", "read");
    h.start();
    // Changed after its manifest was made.
    h.github.files["https://github.com/owner/autoloot/releases/latest/download/autoloot-1.0.0.zip"] += "x";
    AddonsStatus s = h.inspect(kRepo);
    REQUIRE(s.offer);
    h.manager->install_offer();
    h.manager->wait_idle();
    CHECK_FALSE(h.manager->has_ready());
    const AddonStatus* r = h.row("autoloot");
    REQUIRE(r);
    CHECK(r->state == AddonState::Failed);
    CHECK(r->error.find("does not match") != std::string::npos);
    CHECK_FALSE(fs::exists(h.folder()));

    // Redirected off GitHub.
    h.publish("1.0.0", "read");
    h.github.redirects["https://github.com/owner/autoloot/releases/latest/download/autoloot-1.0.0.zip"] =
        "https://evil.example/autoloot.zip";
    h.github.files["https://evil.example/autoloot.zip"] =
        h.github.files["https://github.com/owner/autoloot/releases/latest/download/autoloot-1.0.0.zip"];
    s = h.inspect(kRepo);
    REQUIRE(s.offer);
    h.manager->install_offer();
    h.manager->wait_idle();
    CHECK_FALSE(h.manager->has_ready());
    r = h.row("autoloot");
    REQUIRE(r);
    CHECK(r->error.find("may not come from") != std::string::npos);
}

TEST_CASE("addon_manager: updates at start") {
    Harness h;
    const Keys k;
    h.publish("1.0.0", "read,ui", "-- v1\n", &k);
    h.start();
    h.install();
    CHECK_FALSE(h.row("autoloot")->addon.auto_update); // off until the player switches it on
    h.manager->set_auto_update("autoloot", true);

    SUBCASE("asking for nothing more: downloaded and installed") {
        h.publish("1.1.0", "read", "-- v1.1\n", &k);
        h.manager->check_updates(false);
        h.manager->wait_idle();
        const AddonStatus* r = h.row("autoloot");
        REQUIRE(r);
        CHECK(r->state == AddonState::Ready);
        CHECK(r->new_version == "1.1.0");
        CHECK(h.manager->install_ready().size() == 1);
        CHECK(read_file(h.folder() / "init.lua") == "-- v1.1\n");
        // An update that asks for less lowers the ceiling with it.
        CHECK(read_installed_addons(h.dir.path())[0].caps == plugin::kCapRead);
    }
    SUBCASE("nothing newer") {
        const auto before = h.manager->status().message_serial;
        h.manager->check_updates(true);
        CHECK(h.manager->status().message == "Looking for newer versions...");
        h.manager->wait_idle();
        CHECK(h.row("autoloot")->state == AddonState::UpToDate);
        CHECK_FALSE(h.manager->has_ready());
        CHECK(h.manager->status().message == "Every add-on is up to date");
        CHECK(h.manager->status().message_serial == before + 2);
    }
    SUBCASE("asking for more: waits for the player's approval") {
        h.publish("2.0.0", "read,ui,chat", "-- v2\n", &k);
        h.manager->check_updates(false);
        h.manager->wait_idle();
        const AddonStatus* r = h.row("autoloot");
        REQUIRE(r);
        CHECK(r->state == AddonState::NeedsApproval);
        CHECK(r->new_caps == (plugin::kCapRead | plugin::kCapUi | plugin::kCapChat));
        CHECK_FALSE(h.manager->has_ready());
        h.update("autoloot");
        h.manager->wait_idle();
        CHECK(h.row("autoloot")->state == AddonState::Ready);
        h.manager->install_ready();
        CHECK(read_installed_addons(h.dir.path())[0].caps == (plugin::kCapRead | plugin::kCapUi | plugin::kCapChat));
    }
    SUBCASE("asking for more after the player looked: not taken on that approval") {
        h.publish("2.0.0", "read,ui,chat", "-- v2\n", &k);
        h.manager->check_updates(false);
        h.manager->wait_idle();
        const AddonStatus* r = h.row("autoloot");
        REQUIRE(r);
        REQUIRE(r->state == AddonState::NeedsApproval);
        const std::uint32_t seen = r->new_caps;
        const std::string seen_version = r->new_version;
        // The repository changes between the player's look and their click.
        h.publish("2.0.0", "read,ui,chat,web", "-- v2 with more\n", &k);
        h.manager->update("autoloot", seen, seen_version);
        h.manager->wait_idle();
        CHECK_FALSE(h.manager->has_ready());
        r = h.row("autoloot");
        REQUIRE(r);
        CHECK(r->state == AddonState::NeedsApproval);
        CHECK(r->new_caps == (plugin::kCapRead | plugin::kCapUi | plugin::kCapChat | plugin::kCapWeb));
        CHECK(h.manager->status().message.find("review it again") != std::string::npos);
        CHECK(h.manager->status().message_error);
        // Another version than the one seen, even asking for no more: not on that approval either.
        h.publish("2.0.1", "read,ui,chat", "-- v2.0.1\n", &k);
        h.manager->update("autoloot", seen, seen_version);
        h.manager->wait_idle();
        CHECK_FALSE(h.manager->has_ready());
        CHECK(h.row("autoloot")->state == AddonState::NeedsApproval);
        // Approved as it now shows: taken.
        h.update("autoloot");
        h.manager->wait_idle();
        CHECK(h.row("autoloot")->state == AddonState::Ready);
        h.manager->install_ready();
        CHECK(read_installed_addons(h.dir.path())[0].caps == (plugin::kCapRead | plugin::kCapUi | plugin::kCapChat));
        CHECK(read_file(h.folder() / "init.lua") == "-- v2.0.1\n");
    }
    SUBCASE("the player changed a file: asked before it is overwritten") {
        put_file(h.folder() / "init.lua", "-- my change\n");
        h.publish("1.1.0", "read,ui", "-- v1.1\n", &k);
        h.manager->check_updates(false);
        h.manager->wait_idle();
        const AddonStatus* r = h.row("autoloot");
        REQUIRE(r);
        CHECK(r->state == AddonState::Modified);
        CHECK(r->modified == std::vector<std::string>{"init.lua"});
        CHECK(read_file(h.folder() / "init.lua") == "-- my change\n");
        h.update("autoloot");
        h.manager->wait_idle();
        h.manager->install_ready();
        CHECK(read_file(h.folder() / "init.lua") == "-- v1.1\n");
    }
    SUBCASE("signed with another key, or no longer signed: refused") {
        const Keys other(77);
        h.publish("1.1.0", "read,ui", "-- evil\n", &other);
        h.manager->check_updates(false);
        h.manager->wait_idle();
        CHECK(h.row("autoloot")->state == AddonState::Blocked);
        CHECK(h.row("autoloot")->error.find("another key") != std::string::npos);
        h.update("autoloot"); // the player's click does not get past it either
        h.manager->wait_idle();
        CHECK_FALSE(h.manager->has_ready());
        h.publish("1.1.0", "read,ui", "-- unsigned\n");
        h.manager->check_updates(true);
        h.manager->wait_idle();
        CHECK(h.row("autoloot")->error.find("no longer signed") != std::string::npos);
        CHECK(read_file(h.folder() / "init.lua") == "-- v1\n");
        // Nor through a new link to the same repository.
        const AddonsStatus s = h.inspect(kRepo);
        REQUIRE(s.offer);
        h.publish("1.1.0", "read,ui", "-- evil\n", &other);
        const AddonsStatus s2 = h.inspect(kRepo);
        REQUIRE(s2.offer);
        CHECK(s2.offer->blocked.find("another key") != std::string::npos);
    }
    SUBCASE("auto-update off: offered, not taken") {
        h.manager->set_auto_update("autoloot", false);
        CHECK_FALSE(read_installed_addons(h.dir.path())[0].auto_update);
        h.publish("1.1.0", "read,ui", "-- v1.1\n", &k);
        h.manager->check_updates(false);
        h.manager->wait_idle();
        CHECK(h.row("autoloot")->state == AddonState::UpToDate); // not checked: only auto-update add-ons at start
        CHECK(h.row("autoloot")->new_version.empty());
        h.manager->check_updates(true);
        h.manager->wait_idle();
        CHECK(h.row("autoloot")->state == AddonState::Available);
        h.update("autoloot");
        h.manager->wait_idle();
        CHECK(h.manager->install_ready().size() == 1);
    }
    SUBCASE("the repository now holds another add-on") {
        const std::string zip = zip_of({{"init.lua", "--\n"}});
        h.github.release(kRepo, k.sign(manifest_for("other", "script", "9.0.0", "read", "any", zip, "o.zip")), "o.zip",
                         zip);
        h.manager->check_updates(true);
        h.manager->wait_idle();
        CHECK(h.row("autoloot")->state == AddonState::Blocked);
    }
    SUBCASE("removed") {
        std::string why;
        REQUIRE(h.manager->remove("autoloot", &why));
        CHECK_FALSE(h.row("autoloot"));
        CHECK(read_installed_addons(h.dir.path()).empty());
        CHECK_FALSE(fs::exists(h.folder()));
    }
}

TEST_CASE("addon_manager: the player's removal and auto-update switch win over a check under way") {
    Harness h;
    h.publish("1.0.0", "read,ui");
    h.publish_buff("1.0.0");
    h.start();
    h.install();
    h.install("https://github.com/owner/autobuff");
    REQUIRE(read_installed_addons(h.dir.path()).size() == 2);
    h.manager->set_auto_update("autoloot", true);
    h.manager->set_auto_update("autobuff", true);
    h.publish("1.1.0", "read,ui", "-- v1.1\n");
    h.publish_buff("1.1.0");
    const std::string buff_manifest = "https://github.com/owner/autobuff/releases/latest/download/hailstone-addon.txt";
    const std::string loot_zip = "https://github.com/" + kRepo + "/releases/latest/download/autoloot-1.1.0.zip";
    auto loot_auto = [&] {
        for (const auto& a : read_installed_addons(h.dir.path()))
            if (a.name == "autoloot") return std::optional<bool>(a.auto_update);
        return std::optional<bool>();
    };

    SUBCASE("removed while the check looks at another add-on: not brought back") {
        // The check read installed.txt (autobuff, autoloot) before the player removed autoloot.
        h.on_fetch = [&](const std::string& url) {
            if (url != buff_manifest) return;
            std::string why;
            CHECK_MESSAGE(h.manager->remove("autoloot", &why), why);
        };
        h.manager->check_updates(true);
        h.manager->wait_idle();
        h.on_fetch = nullptr;
        CHECK_FALSE(h.row("autoloot"));
        CHECK(h.manager->install_ready() == std::vector<std::string>{"autobuff"});
        const auto installed = read_installed_addons(h.dir.path());
        REQUIRE(installed.size() == 1);
        CHECK(installed[0].name == "autobuff");
        CHECK_FALSE(fs::exists(h.folder() / "init.lua"));
        // Installed again on the player's word: checked as before.
        h.install();
        CHECK(read_installed_addons(h.dir.path()).size() == 2);
        CHECK(read_file(h.folder() / "init.lua") == "-- v1.1\n");
    }
    SUBCASE("auto-update switched off while the check looks at another add-on: offered, not taken") {
        h.on_fetch = [&](const std::string& url) {
            if (url == buff_manifest) h.manager->set_auto_update("autoloot", false);
        };
        h.manager->check_updates(false);
        h.manager->wait_idle();
        h.on_fetch = nullptr;
        CHECK(h.row("autoloot")->state == AddonState::Available);
        CHECK(h.manager->install_ready() == std::vector<std::string>{"autobuff"});
        CHECK(loot_auto() == false);
        CHECK(read_file(h.folder() / "init.lua") == "-- script\n");
    }
    SUBCASE("auto-update switched off while the update downloads: not installed") {
        h.on_fetch = [&](const std::string& url) {
            if (url == loot_zip) h.manager->set_auto_update("autoloot", false);
        };
        h.manager->check_updates(false);
        h.manager->wait_idle();
        h.on_fetch = nullptr;
        CHECK(h.manager->install_ready() == std::vector<std::string>{"autobuff"});
        CHECK(h.row("autoloot")->state == AddonState::Available);
        CHECK(loot_auto() == false);
        CHECK(read_file(h.folder() / "init.lua") == "-- script\n");
        // The player's own Update takes it, and the switch stays off.
        h.update("autoloot");
        h.manager->wait_idle();
        CHECK(h.manager->install_ready() == std::vector<std::string>{"autoloot"});
        CHECK(read_file(h.folder() / "init.lua") == "-- v1.1\n");
        CHECK(loot_auto() == false);
        CHECK_FALSE(h.row("autoloot")->addon.auto_update);
    }
    SUBCASE("auto-update switched on while the player's update downloads: kept on") {
        h.manager->set_auto_update("autoloot", false);
        h.manager->check_updates(true);
        h.manager->wait_idle();
        REQUIRE(h.row("autoloot")->state == AddonState::Available);
        h.on_fetch = [&](const std::string& url) {
            if (url == loot_zip) h.manager->set_auto_update("autoloot", true);
        };
        h.update("autoloot");
        h.manager->wait_idle();
        h.on_fetch = nullptr;
        const auto done = h.manager->install_ready();
        CHECK(std::find(done.begin(), done.end(), "autoloot") != done.end());
        CHECK(read_file(h.folder() / "init.lua") == "-- v1.1\n");
        CHECK(loot_auto() == true);
    }
}

TEST_CASE("addon_manager: the check at start keeps to addons.check_hours") {
    Harness h;
    h.publish("1.0.0", "read");
    h.settings.check_hours = 6;
    h.start();
    h.install();
    h.manager->set_auto_update("autoloot", true);
    h.github.asked.clear();
    h.manager->check_updates(false);
    h.manager->wait_idle();
    CHECK(h.github.asked.size() == 1); // the release's manifest
    h.clock += 3600;
    h.manager->check_updates(false);
    h.manager->wait_idle();
    CHECK(h.github.asked.size() == 1); // an hour later: not again
    h.manager->check_updates(true);    // the player's Check now
    h.manager->wait_idle();
    CHECK(h.github.asked.size() == 2);
    h.clock += 7 * 3600;
    h.manager->check_updates(false);
    h.manager->wait_idle();
    CHECK(h.github.asked.size() == 3);

    // addons.check off: nothing at start.
    h.manager.reset();
    h.settings.check = false;
    h.clock += 7 * 3600;
    h.start();
    h.manager->check_updates(false);
    h.manager->wait_idle();
    CHECK(h.github.asked.size() == 3);
}

TEST_CASE("addon_manager: a plugin installs with its library and windows") {
    if (this_platform().empty()) return;
    Harness h;
    h.start();
    const std::string zip =
        zip_of({{"MQ2Hud" + lib_ext(), library_for(this_platform())}, {"ui/modern/hud.lua", "ui.window('Hud', function() end)\n"}});
    h.github.release("owner/hud", manifest_for("MQ2Hud", "plugin", "1.0.0", "read,ui", this_platform(), zip, "hud.zip"),
                     "hud.zip", zip);
    h.install("https://github.com/owner/hud");
    const fs::path folder = addon_folder(h.dir.path(), AddonKind::Plugin, "MQ2Hud");
    CHECK(fs::exists(folder / ("MQ2Hud" + lib_ext())));
    CHECK(fs::exists(folder / "ui" / "modern" / "hud.lua"));
    const auto c = addon_ceilings(read_installed_addons(h.dir.path()));
    REQUIRE(c.size() == 1);
    CHECK(c[0].kind == plugin::FileKind::Plugin);
    CHECK(c[0].caps == (plugin::kCapRead | plugin::kCapUi));
}

// install_ready moves a staged add-on out of addons/new/<name>/; a second install of it meanwhile would unpack
// into that same folder on the worker. It waits instead, and the staged one installs as it was.
TEST_CASE("addon_manager: installing an add-on again while it waits to be installed is refused") {
    Harness h;
    h.publish("1.0.0", "read");
    h.start();
    auto s = h.inspect("https://github.com/" + kRepo);
    REQUIRE(s.offer);
    h.manager->install_offer();
    h.manager->wait_idle();
    REQUIRE(h.manager->has_ready());
    h.publish("1.1.0", "read", "-- newer\n");
    s = h.inspect("https://github.com/" + kRepo);
    REQUIRE_MESSAGE(s.offer, s.message);
    h.manager->install_offer();
    h.manager->wait_idle();
    s = h.manager->status();
    CHECK(s.message_error);
    CHECK(s.message.find("already being installed") != std::string::npos);
    const AddonStatus* r = h.row("autoloot");
    REQUIRE(r);
    CHECK(r->state == AddonState::Ready);
    CHECK(h.manager->install_ready() == std::vector<std::string>{"autoloot"});
    CHECK(read_file(h.folder() / "init.lua") == "-- script\n");
}

// Two first installs of one name from two repositories: the card of the second was made before the first was in
// installed.txt, so it was not blocked. When it comes to be installed, the pin holds against the one there now.
TEST_CASE("addon_manager: a staged add-on is not installed over one of its name from another repository") {
    Harness h;
    h.publish("1.0.0", "read");
    const std::string zip = zip_of({{"init.lua", "-- impostor\n"}});
    h.github.release("other/autoloot", manifest_for("autoloot", "script", "2.0.0", "read", "any", zip, "a.zip"),
                     "a.zip", zip);
    h.start();
    auto s = h.inspect("https://github.com/" + kRepo);
    REQUIRE(s.offer);
    h.manager->install_offer();
    h.manager->wait_idle();
    REQUIRE(h.manager->has_ready());
    s = h.inspect("https://github.com/other/autoloot"); // nothing of that name in installed.txt yet
    REQUIRE(s.offer);
    REQUIRE(s.offer->blocked.empty());
    CHECK(h.manager->install_ready() == std::vector<std::string>{"autoloot"});
    h.manager->install_offer(); // the card made before
    h.manager->wait_idle();
    REQUIRE(h.manager->has_ready());
    CHECK(h.manager->install_ready().empty());
    s = h.manager->status();
    CHECK(s.message_error);
    CHECK(s.message.find("installed already, from") != std::string::npos);
    const auto installed = read_installed_addons(h.dir.path());
    REQUIRE(installed.size() == 1);
    CHECK(installed[0].source == kRepo);
    CHECK(read_file(h.folder() / "init.lua") == "-- script\n");
}
