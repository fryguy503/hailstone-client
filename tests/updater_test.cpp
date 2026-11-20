// The updater (docs/updater.md): core/version.h's order, core/ed25519.h against RFC 8032, core/update_manifest.h's
// checks, and app/updater.h with app/update_install.h over a fake feed in a scratch folder: check, download,
// install, the start count, the rollback of a cut-off install and of a version that does not start.

#include <doctest/doctest.h>

#include <ctime>
#include <fstream>
#include <map>

#include "app/addon_install.h"
#include "app/update_install.h"
#include "app/updater.h"
#include "core/atomic_file.h"
#include "core/config.h"
#include "core/ed25519.h"
#include "core/sha256.h"
#include "core/update_manifest.h"
#include "core/version.h"
#include "core/zip_write.h"
#include "platform/durable_file.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::app;
namespace fs = std::filesystem;

namespace {

int cmp(const char* a, const char* b) { return compare_versions(*parse_version(a), *parse_version(b)); }

struct Keys {
    Ed25519SecretKey secret{};
    Ed25519PublicKey pub{};
    Keys() {
        std::array<std::uint8_t, 32> seed{};
        for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<std::uint8_t>(i * 7 + 1);
        ed25519_key_pair(seed, secret, pub);
    }
};

std::string sign(const Keys& k, const std::string& body) {
    return body + "signature " + hex_encode(ed25519_sign(k.secret, body)) + "\n";
}

std::string zip_of(const std::map<std::string, std::string>& files) {
    ZipWriter z;
    std::tm when{};
    when.tm_year = 120;
    when.tm_mday = 1;
    for (const auto& [name, text] : files) REQUIRE(z.add(name, text, when));
    const auto& bytes = z.finish();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string get(const fs::path& path) { return read_text_file(path).value_or("<missing>"); }

// A feed held in memory: address -> body.
struct Feed {
    std::map<std::string, std::string> files;
    int fetches = 0;
    UpdateFetch fetch() {
        return [this](const std::string& url, const std::function<bool(std::span<const std::byte>)>& sink,
                      const std::function<bool(std::uint64_t, std::uint64_t)>& progress,
                      const std::function<bool()>&) {
            ++fetches;
            platform::HttpGetResult r;
            auto it = files.find(url);
            if (it == files.end()) {
                r.status = 404;
                r.error = "404";
                return r;
            }
            r.status = 200;
            const auto bytes = std::as_bytes(std::span(it->second.data(), it->second.size()));
            if (!sink(bytes) || (progress && !progress(bytes.size(), bytes.size()))) {
                r.error = "cancelled";
                return r;
            }
            r.ok = true;
            return r;
        };
    }
};

constexpr const char* kFeed = "https://example.invalid/latest/download/hailstone-update.txt";
constexpr const char* kBase = "https://example.invalid/latest/download/";

std::string package_line(const std::string& component, const std::string& file, const std::string& zip,
                         const std::string& needs = "") {
    return "package test-x64 " + component + " " + std::to_string(zip.size()) + " " + to_hex(sha256(zip)) + " " +
           file + (needs.empty() ? "" : " " + needs) + "\n";
}

UpdaterEnv env_for(const fs::path& dir, Feed& feed, const Keys& k, const std::string& running = "0.1.0-alpha") {
    UpdaterEnv env;
    env.install_dir = dir;
    env.feed = kFeed;
    env.version = running;
    env.platform = "test-x64";
    env.key = k.pub;
    env.fetch = feed.fetch();
    env.auto_download = true; // update.download switched on: most cases test the download itself
    return env;
}

} // namespace

TEST_CASE("versions: SemVer precedence") {
    CHECK(cmp("0.1.0-alpha", "0.1.0") < 0);
    CHECK(cmp("0.1.0", "0.1.0-alpha") > 0);
    CHECK(cmp("0.1.0-alpha", "0.1.0-beta") < 0);
    CHECK(cmp("1.0.0-alpha.2", "1.0.0-alpha.10") < 0);
    CHECK(cmp("1.0.0-alpha.1", "1.0.0-alpha.beta") < 0);
    CHECK(cmp("1.0.0-alpha", "1.0.0-alpha.1") < 0);
    CHECK(cmp("0.2.0-alpha", "0.1.9") > 0);
    CHECK(cmp("v0.1.0-alpha", "0.1.0-alpha") == 0);
    CHECK(cmp("0.1.0+abc", "0.1.0") == 0);
    CHECK(cmp("0.1.0-rc.1+build.007.x-y", "0.1.0-rc.1") == 0);
    CHECK(cmp("0.10.0", "0.9.0") > 0);
    CHECK(parse_version("0.1.0-alpha")->to_string() == "0.1.0-alpha");
    for (const char* bad : {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.2.3-", "1.2.3-a..b", "1.2.3-01", "a.b.c",
                            "1.2.3-é", "1.2.3+", "99999999999.0.0", "1.2.3+a..b", "1.2.3+a.", "1.2.3+.a",
                            "1.2.3+a\x1b[2J", "1.2.3+a b", "1.2.3+\xc3\xa9", "1.2.3+a+b", "1.2.3+a\n"})
        CHECK_MESSAGE(!parse_version(bad), bad);
}

TEST_CASE("ed25519: RFC 8032 test 1 and a changed message") {
    auto pub = hex_decode<32>("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    auto sig = hex_decode<64>("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e3970"
                              "1cf9b46bd25bf5f0595bbe24655141438e7a100b");
    REQUIRE(pub);
    REQUIRE(sig);
    CHECK(ed25519_verify(*sig, *pub, std::string_view()));
    CHECK(!ed25519_verify(*sig, *pub, std::string_view("x")));

    auto seed = hex_decode<32>("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    Ed25519SecretKey secret{};
    Ed25519PublicKey made{};
    ed25519_key_pair(*seed, secret, made);
    CHECK(made == *pub);
    CHECK(ed25519_sign(secret, "") == *sig);
    CHECK(!hex_decode<32>("zz"));
}

TEST_CASE("update manifest: signature, records and refusals") {
    Keys k;
    const std::string body = "hailstone-update 1\nversion 0.2.0-alpha\nnotes https://example.invalid/r\n"
                             "# a comment\nfuture-record whatever\n" +
                             package_line("core", "core.zip", "abc") +
                             package_line("plugin-nms", "nms.zip", "de", "plugins/nms");
    std::string error;
    auto m = parse_update_manifest(sign(k, body), k.pub, &error);
    REQUIRE_MESSAGE(m, error);
    CHECK(m->version_text == "0.2.0-alpha");
    CHECK(m->notes == "https://example.invalid/r");
    REQUIRE(m->packages.size() == 2);
    CHECK(m->packages[1].needs == "plugins/nms");
    CHECK(packages_for(*m, "test-x64").size() == 2);
    CHECK(packages_for(*m, "other").empty());

    // One byte changed, a different key, no signature, lines after it.
    std::string changed = sign(k, body);
    changed[30] = '9';
    CHECK(!parse_update_manifest(changed, k.pub));
    Keys other;
    other.pub[0] ^= 1;
    CHECK(!parse_update_manifest(sign(k, body), other.pub));
    CHECK(!parse_update_manifest(body, k.pub, &error));
    CHECK(error.find("not signed") != std::string::npos);
    CHECK(!parse_update_manifest(sign(k, body) + "version 9.9.9\n", k.pub));

    auto refused = [&](const std::string& b) { return !parse_update_manifest(sign(k, b), k.pub); };
    CHECK(refused("hailstone-update 2\nversion 1.0.0\n"));
    CHECK(refused("version 1.0.0\n"));
    CHECK(refused("hailstone-update 1\n"));
    CHECK(refused("hailstone-update 1\nversion one\n"));
    CHECK(refused("hailstone-update 1\nversion 1.0.0\nnotes javascript:x\n"));
    CHECK(refused("hailstone-update 1\nversion 1.0.0\n" + package_line("core", "../core.zip", "a")));
    CHECK(refused("hailstone-update 1\nversion 1.0.0\n" + package_line("core", "a/b.zip", "a")));
    CHECK(refused("hailstone-update 1\nversion 1.0.0\n" + package_line("core", "c.zip", "a", "../up")));
    CHECK(refused("hailstone-update 1\nversion 1.0.0\n" + package_line("core", "c.zip", "a") +
                  package_line("core", "d.zip", "b")));
    CHECK(refused("hailstone-update 1\nversion 1.0.0\npackage test-x64 core 0 " + to_hex(sha256("")) + " c.zip\n"));
}

TEST_CASE("update install: the player's files are never an update's") {
    for (const char* p : {"hailstone.ini", "HAILSTONE.INI", "servers/a/server.ini", "logs/hailstone.log",
                          "update/state.txt", "ui/settings/modern.txt", "ui/loadingbackgrounds/a.png", "maps/x.txt",
                          "scripts/a.lua", "textures/default/a.dds", "web/a/b", "notes.txt", "Servers/x",
                          "tools/texstudio/texstudio", "Tools/x/data.txt", "ui-data/modern/lib/style.lua",
                          "UI-DATA/addon.bundle.skin/lib/compiled/window.lua", "ui-data/", "ui-data", "UI-DATA",
                          "ui-addons/skin/ui/dark/ui.ini", "UI-ADDONS/skin/ui/dark/window.lua", "ui-addons"})
        CHECK_MESSAGE(is_player_path(p), p);
    for (const char* p : {"hailstone", "hailstone.exe", "shaders/a.spv", "ui/modern/login.lua", "browser/libcef.so",
                          "plugins/nms/nms.so", "icons/hailstone.png", "servers.txt", "ui-data-copy/file.lua",
                          "ui-database/file.lua", "ui-data.lua", "ui-addons.lua"})
        CHECK_MESSAGE(!is_player_path(p), p);
}

TEST_CASE("updater: check, download, install, start and the next update") {
    test::ScratchDir dir("updater");
    const fs::path inst = dir.path() / "hailstone";
    put(inst / "hailstone", "old binary");
    put(inst / "shaders/a.spv", "old a");
    put(inst / "hailstone.ini", "player settings");
    put(inst / "plugins/nms/nms.so", "old nms");
    Keys k;
    Feed feed;
    const std::string core = zip_of({{"hailstone", "new binary"}, {"shaders/a.spv", "new a"}, {"shaders/b.spv", "b"}});
    const std::string nms = zip_of({{"plugins/nms/nms.so", "new nms"}});
    const std::string extra = zip_of({{"plugins/extra/extra.so", "extra"}});
    feed.files[kBase + std::string("core.zip")] = core;
    feed.files[kBase + std::string("nms.zip")] = nms;
    feed.files[kBase + std::string("extra.zip")] = extra;
    feed.files[kFeed] = sign(k, "hailstone-update 1\nversion 0.2.0-alpha\nnotes https://example.invalid/r\n" +
                                     package_line("core", "core.zip", core) +
                                     package_line("plugin-nms", "nms.zip", nms, "plugins/nms") +
                                     package_line("plugin-extra", "extra.zip", extra, "plugins/extra"));
    {
        Updater u(env_for(inst, feed, k));
        u.check();
        u.wait();
        const auto s = u.status();
        REQUIRE_MESSAGE(s.state == UpdateState::Ready, s.error);
        CHECK(s.version == "0.2.0-alpha");
        CHECK(s.notes == "https://example.invalid/r");
        CHECK(feed.fetches == 3); // the manifest, core and nms; not extra (no plugins/extra/)
    }
    // Nothing in the folder changed yet.
    CHECK(get(inst / "hailstone") == "old binary");
    CHECK(staged_update_version(inst) == "0.2.0-alpha");
    CHECK(staged_update_ok(inst));

    // A second check finds it staged and fetches only the manifest.
    feed.fetches = 0;
    {
        Updater u(env_for(inst, feed, k));
        u.check();
        u.wait();
        CHECK(u.status().state == UpdateState::Ready);
        CHECK(feed.fetches == 1);
    }

    auto r = install_staged_update(inst, "0.1.0-alpha");
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(get(inst / "hailstone") == "new binary");
    CHECK(get(inst / "shaders/a.spv") == "new a");
    CHECK(get(inst / "shaders/b.spv") == "b");
    CHECK(get(inst / "plugins/nms/nms.so") == "new nms");
    CHECK(get(inst / "hailstone.ini") == "player settings");
    CHECK(get(inst / "update/previous/hailstone") == "old binary");
    CHECK(!fs::exists(inst / "update/download"));
    CHECK(staged_update_version(inst).empty());

    // The new version starts and reaches the login screen: previous/ goes.
    CHECK(update_at_start(inst, "0.2.0-alpha").message.empty());
    update_started_fine(inst, "0.2.0-alpha");
    CHECK(!fs::exists(inst / "update/previous"));
    CHECK(!fs::exists(inst / "update/state.txt"));

    // The next release drops shaders/b.spv and keeps nms as it was: b goes, nms is not downloaded again.
    const std::string core3 = zip_of({{"hailstone", "binary 3"}, {"shaders/a.spv", "a 3"}});
    feed.files[kBase + std::string("core3.zip")] = core3;
    feed.files[kFeed] = sign(k, "hailstone-update 1\nversion 0.3.0-alpha\n" + package_line("core", "core3.zip", core3) +
                                     package_line("plugin-nms", "nms.zip", nms, "plugins/nms"));
    feed.fetches = 0;
    {
        Updater u(env_for(inst, feed, k, "0.2.0-alpha"));
        u.check();
        u.wait();
        REQUIRE(u.status().state == UpdateState::Ready);
        CHECK(feed.fetches == 2);
    }
    REQUIRE(install_staged_update(inst, "0.2.0-alpha").ok);
    CHECK(get(inst / "hailstone") == "binary 3");
    CHECK(!fs::exists(inst / "shaders/b.spv"));
    CHECK(get(inst / "update/previous/shaders/b.spv") == "b");
    CHECK(get(inst / "plugins/nms/nms.so") == "new nms");
}

TEST_CASE("updater: nothing is downloaded or installed unless the player asks") {
    const UpdateSettings defaults = update_settings(Config{});
    CHECK(defaults.check);
    CHECK_FALSE(defaults.download);
    CHECK_FALSE(update_installs_at_quit(defaults, false)); // a package an earlier run left: Restart now only
    CHECK(update_installs_at_quit(defaults, true));         // Download pressed in this run
    UpdateSettings on = defaults;
    on.download = true;
    CHECK(update_installs_at_quit(on, false));
    UpdateSettings never = defaults;
    never.install_on_quit = false;
    CHECK_FALSE(update_installs_at_quit(never, true));

    test::ScratchDir dir("updater_ask");
    const fs::path inst = dir.path();
    Keys k;
    Feed feed;
    const std::string core = zip_of({{"hailstone", "x"}});
    feed.files[kBase + std::string("core.zip")] = core;
    feed.files[kFeed] = sign(k, "hailstone-update 1\nversion 0.2.0\n" + package_line("core", "core.zip", core));
    auto env = env_for(inst, feed, k, "0.1.0");
    env.auto_download = defaults.download;
    Updater u(env);
    u.check();
    u.wait();
    CHECK(u.status().state == UpdateState::Available);
    CHECK(feed.fetches == 1); // the manifest only
    CHECK(!fs::exists(inst / "update/download/core.zip"));
    u.download();
    u.wait();
    CHECK(u.status().state == UpdateState::Ready);
    CHECK(feed.fetches == 2);
}

TEST_CASE("updater: up to date, older, skipped, refused and missing") {
    test::ScratchDir dir("updater_none");
    const fs::path inst = dir.path();
    Keys k;
    Feed feed;
    auto status_for = [&](const std::string& running, const std::string& skip = "") {
        auto env = env_for(inst, feed, k, running);
        env.skip = skip;
        Updater u(env);
        u.check();
        u.wait();
        return u.status();
    };
    CHECK(status_for("0.1.0").state == UpdateState::Failed); // no manifest yet (404)
    const std::string core = zip_of({{"hailstone", "x"}});
    feed.files[kBase + std::string("core.zip")] = core;
    feed.files[kFeed] = sign(k, "hailstone-update 1\nversion 0.2.0\n" + package_line("core", "core.zip", core));
    CHECK(status_for("0.2.0").state == UpdateState::UpToDate);
    CHECK(status_for("0.3.0-alpha").state == UpdateState::UpToDate);
    auto skipped = status_for("0.1.0", "0.2.0");
    CHECK(skipped.state == UpdateState::Available);
    CHECK(skipped.skipped);
    CHECK(!fs::exists(inst / "update/download/core.zip"));
    {
        // Skip pressed while Available shows, before the automatic download starts: nothing is downloaded.
        auto env = env_for(inst, feed, k, "0.1.0");
        Updater* self = nullptr;
        env.on_available = [&] { self->skip(); };
        Updater u(env);
        self = &u;
        feed.fetches = 0;
        u.check();
        u.wait();
        CHECK(u.status().state == UpdateState::Available);
        CHECK(u.status().skipped);
        CHECK(feed.fetches == 1); // the manifest only
        CHECK(!fs::exists(inst / "update/download/core.zip"));
    }

    // A package that does not match its manifest is not kept. One larger than the manifest says is stopped as its
    // bytes come (the sink, not the progress callback): nothing of it is written.
    feed.files[kBase + std::string("core.zip")] = core + "x";
    auto bad = status_for("0.1.0");
    CHECK(bad.state == UpdateState::Failed);
    CHECK(bad.error.find("larger than the manifest says") != std::string::npos);
    CHECK(!fs::exists(inst / "update/ready.txt"));
    CHECK(!fs::exists(inst / "update/download/core.zip.part"));
    // One of the right size whose bytes differ.
    feed.files[kBase + std::string("core.zip")] = std::string(core.size(), 'x');
    auto changed = status_for("0.1.0");
    CHECK(changed.state == UpdateState::Failed);
    CHECK(changed.error.find("did not match") != std::string::npos);

    // No package for this platform: only the notes.
    feed.files[kFeed] = sign(k, "hailstone-update 1\nversion 0.2.0\nnotes https://example.invalid/r\n");
    auto manual = status_for("0.1.0");
    CHECK(manual.state == UpdateState::Available);
    CHECK(!manual.has_package);

    // Signed by another key.
    Keys other;
    other.secret[0] ^= 1;
    other.pub[0] ^= 1;
    feed.files[kFeed] = sign(k, "hailstone-update 1\nversion 0.2.0\n");
    auto env = env_for(inst, feed, other, "0.1.0");
    Updater u(env);
    u.check();
    u.wait();
    CHECK(u.status().state == UpdateState::Failed);
}

TEST_CASE("update install: refusals leave the folder as it was") {
    test::ScratchDir dir("updater_refuse");
    const fs::path inst = dir.path();
    put(inst / "hailstone", "old");
    put(inst / "hailstone.ini", "mine");
    auto stage = [&](const std::string& component, const std::string& zip) {
        put(inst / "update/download/p.zip", zip);
        UpdatePackage p;
        p.component = component;
        p.file = "p.zip";
        p.size = zip.size();
        p.sha256 = sha256(zip);
        REQUIRE(stage_update(inst, "9.0.0", {p}, nullptr));
    };
    stage("core", zip_of({{"hailstone", "new"}, {"hailstone.ini", "theirs"}}));
    CHECK(!install_staged_update(inst, "1.0.0").ok);
    CHECK(get(inst / "hailstone") == "old");
    CHECK(get(inst / "hailstone.ini") == "mine");

    stage("plugin-nms", zip_of({{"plugins/nms/a.so", "a"}, {"hailstone", "new"}}));
    CHECK(!install_staged_update(inst, "1.0.0").ok);
    CHECK(get(inst / "hailstone") == "old");

    // A package changed after its download.
    stage("core", zip_of({{"hailstone", "new"}}));
    put(inst / "update/download/p.zip", "tampered");
    CHECK(!install_staged_update(inst, "1.0.0").ok);
    CHECK(get(inst / "hailstone") == "old");
    CHECK(!fs::exists(inst / "update/state.txt"));
}

// installed.txt is saved before any file moves; when it cannot be, nothing moves (a roll-back would otherwise take
// it there was none and delete it).
TEST_CASE("update install: an installed.txt that cannot be saved stops the install before anything moves") {
    test::ScratchDir dir("updater_registry");
    const fs::path inst = dir.path();
    put(inst / "hailstone", "old");
    const std::string zip = zip_of({{"hailstone", "new"}});
    put(inst / "update/download/p.zip", zip);
    UpdatePackage p;
    p.component = "core";
    p.file = "p.zip";
    p.size = zip.size();
    p.sha256 = sha256(zip);
    REQUIRE(stage_update(inst, "9.0.0", {p}, nullptr));
    put(inst / "update/installed.txt", "the registry\n");
    put(inst / "update/installed.prev.txt/in-the-way", "x"); // a folder there: the copy fails
    const auto r = install_staged_update(inst, "1.0.0");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("cannot save") != std::string::npos);
    CHECK(get(inst / "hailstone") == "old");
    CHECK(get(inst / "update/installed.txt") == "the registry\n");
    CHECK_FALSE(fs::exists(inst / "update/state.txt"));
}

TEST_CASE("update install: an optional plugin package is left out when an add-on took its folder since") {
    test::ScratchDir dir("updater_addon_folder");
    const fs::path inst = dir.path();
    put(inst / "hailstone", "old");
    put(inst / "plugins/extra/extra.so", "the add-on's");
    std::vector<UpdatePackage> staged;
    auto add = [&](const std::string& component, const std::string& file, const std::string& zip) {
        put(inst / "update/download" / file, zip);
        UpdatePackage p;
        p.component = component;
        p.file = file;
        p.size = zip.size();
        p.sha256 = sha256(zip);
        staged.push_back(p);
    };
    add("core", "core.zip", zip_of({{"hailstone", "new"}}));
    add("plugin-extra", "extra.zip", zip_of({{"plugins/extra/extra.so", "the release's"}}));
    REQUIRE(stage_update(inst, "0.2.0", staged, nullptr));
    // Installed after the download, while the update waited for the client to quit.
    InstalledAddon a;
    a.name = "extra";
    a.kind = AddonKind::Plugin;
    a.source = "owner/extra";
    a.version = "1.0.0";
    REQUIRE(write_installed_addons(inst, {a}));

    REQUIRE(install_staged_update(inst, "0.1.0").ok);
    CHECK(get(inst / "hailstone") == "new");
    CHECK(get(inst / "plugins/extra/extra.so") == "the add-on's");
    const InstalledUpdate now = read_installed_update(inst);
    CHECK(now.components.count("core") == 1);
    CHECK(now.components.count("plugin-extra") == 0);
    CHECK(now.files.count("plugin-extra") == 0);
}

namespace {
UpdatePackage stage_one(const fs::path& inst, const std::string& component, const std::string& zip,
                        const std::string& version = "0.2.0") {
    put(inst / "update/download/p.zip", zip);
    UpdatePackage p;
    p.component = component;
    p.file = "p.zip";
    p.size = zip.size();
    p.sha256 = sha256(zip);
    REQUIRE(stage_update(inst, version, {p}, nullptr));
    return p;
}
} // namespace

TEST_CASE("update install: each move is in the journal, written before it") {
    test::ScratchDir dir("updater_journal");
    const fs::path inst = dir.path();
    put(inst / "hailstone", "old");
    stage_one(inst, "core", zip_of({{"hailstone", "new"}, {"icons/n.png", "n"}}));
    REQUIRE(install_staged_update(inst, "0.1.0").ok);
    CHECK(get(inst / "update/journal.txt") == "moved hailstone\nadded icons/n.png\n");
    CHECK(get(inst / "update/previous/hailstone") == "old");
}

// An update replaces files: a folder where one of its files goes holds what it did not put there, so the install
// stops before anything moves (docs/updater.md).
TEST_CASE("update install: a folder where a file goes stops the install before anything moves") {
    test::ScratchDir dir("updater_folder_target");
    const fs::path inst = dir.path();
    put(inst / "hailstone", "old");
    put(inst / "shaders/a.spv/mine.txt", "the player's");
    stage_one(inst, "core", zip_of({{"hailstone", "new"}, {"shaders/a.spv", "a"}}));
    const auto r = install_staged_update(inst, "0.1.0");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("is a folder") != std::string::npos);
    CHECK(get(inst / "hailstone") == "old");
    CHECK(get(inst / "shaders/a.spv/mine.txt") == "the player's");
    CHECK_FALSE(fs::exists(inst / "update/state.txt"));
    CHECK_FALSE(fs::exists(inst / "update/previous"));
    CHECK_FALSE(fs::exists(inst / "update/new"));
}

TEST_CASE("update install: the core package leaves an add-on's plugin folder and tools/ alone") {
    test::ScratchDir dir("updater_core_addon");
    const fs::path inst = dir.path();
    put(inst / "hailstone", "old");
    put(inst / "plugins/extra/extra.so", "the add-on's");
    put(inst / "plugins/extra/old.lua", "the add-on's too");
    InstalledAddon a;
    a.name = "extra";
    a.kind = AddonKind::Plugin;
    a.source = "owner/extra";
    a.version = "1.0.0";
    REQUIRE(write_installed_addons(inst, {a}));
    // An earlier core package shipped plugins/extra/old.lua; this one no longer does, and ships extra.so.
    REQUIRE(write_file_atomic(inst / "update/installed.txt",
                              "component core " + std::string(64, '0') + "\nfile core hailstone\n"
                              "file core plugins/extra/old.lua\n"));
    stage_one(inst, "core", zip_of({{"hailstone", "new"}, {"plugins/extra/extra.so", "the release's"},
                                    {"plugins/other/other.so", "o"}}));
    REQUIRE(install_staged_update(inst, "0.1.0").ok);
    CHECK(get(inst / "hailstone") == "new");
    CHECK(get(inst / "plugins/extra/extra.so") == "the add-on's");
    CHECK(get(inst / "plugins/extra/old.lua") == "the add-on's too");
    CHECK(get(inst / "plugins/other/other.so") == "o");
    const InstalledUpdate now = read_installed_update(inst);
    CHECK(now.files.at("core") == std::vector<std::string>{"hailstone", "plugins/other/other.so"});

    // tools/ is the tool add-ons' (docs/addons.md): a package that writes there is refused whole.
    stage_one(inst, "core", zip_of({{"hailstone", "newer"}, {"tools/x/x", "a program"}}), "0.3.0");
    CHECK_FALSE(install_staged_update(inst, "0.2.0").ok);
    CHECK(get(inst / "hailstone") == "new");
    CHECK_FALSE(fs::exists(inst / "tools"));
}

// A package's declared sizes are added up before anything is written: one that would unpack past the limit (a zip
// bomb within its declared sizes) is refused whole.
TEST_CASE("update install: an update larger unpacked than the limit is refused before anything is written") {
    test::ScratchDir dir("updater_unpacked_limit");
    const fs::path inst = dir.path();
    put(inst / "hailstone", "old");
    std::string zip = zip_of({{"a", "x"}, {"b", "y"}});
    // Each central directory entry's size made 3.75 GiB (the reader takes it; the extraction would stop at it).
    for (std::size_t at = 0; (at = zip.find("PK\x01\x02", at)) != std::string::npos; at += 4)
        for (std::size_t i = 0; i < 4; ++i) zip[at + 24 + i] = i == 3 ? '\xf0' : '\0';
    stage_one(inst, "core", zip);
    const auto r = install_staged_update(inst, "0.1.0");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("larger unpacked") != std::string::npos);
    CHECK(get(inst / "hailstone") == "old");
    CHECK_FALSE(fs::exists(inst / "update/new"));
    CHECK_FALSE(fs::exists(inst / "a"));
}

TEST_CASE("update install: a cut-off install and a version that does not start are rolled back") {
    test::ScratchDir dir("updater_rollback");
    const fs::path inst = dir.path();
    put(inst / "hailstone", "old");
    put(inst / "shaders/a.spv", "old a");
    auto stage = [&] {
        const std::string zip = zip_of({{"hailstone", "new"}, {"shaders/a.spv", "new a"}, {"icons/n.png", "n"}});
        put(inst / "update/download/p.zip", zip);
        UpdatePackage p;
        p.component = "core";
        p.file = "p.zip";
        p.size = zip.size();
        p.sha256 = sha256(zip);
        REQUIRE(stage_update(inst, "0.2.0", {p}, nullptr));
    };

    SUBCASE("cut off") {
        stage();
        REQUIRE(install_staged_update(inst, "0.1.0").ok);
        // As though the client died while moving: the state still says installing.
        REQUIRE(write_file_atomic(inst / "update/state.txt", "installing 0.2.0 0.1.0\n"));
        auto note = update_at_start(inst, "0.2.0");
        CHECK(note.restart);
        CHECK(note.message.find("cut off") != std::string::npos);
        CHECK(get(inst / "hailstone") == "old");
        CHECK(get(inst / "shaders/a.spv") == "old a");
        CHECK(!fs::exists(inst / "icons/n.png"));
        CHECK(!fs::exists(inst / "update/installed.txt"));
        // Every file went back: previous/ goes, and nothing is kept aside.
        CHECK_FALSE(fs::exists(inst / "update/previous"));
        for (const auto& e : fs::directory_iterator(inst / "update"))
            CHECK_FALSE(e.path().filename().string().starts_with("kept-"));
    }
    SUBCASE("files in previous/ the journal does not name are kept aside, not deleted") {
        // As after a power cut that lost the journal's lines but not the moves (or a damaged journal): previous/
        // may hold the only copy of a replaced file.
        put(inst / "update/previous/hailstone", "the only copy");
        put(inst / "update/journal.txt", "");
        REQUIRE(write_file_atomic(inst / "update/state.txt", "installing 0.2.0 0.1.0\n"));
        update_at_start(inst, "0.1.0");
        CHECK_FALSE(fs::exists(inst / "update/previous"));
        CHECK_FALSE(fs::exists(inst / "update/state.txt"));
        std::vector<fs::path> kept;
        for (const auto& e : fs::directory_iterator(inst / "update"))
            if (e.path().filename().string().starts_with("kept-")) kept.push_back(e.path());
        REQUIRE(kept.size() == 1);
        CHECK(get(kept[0] / "hailstone") == "the only copy");
        update_at_start(inst, "0.1.0"); // a later start leaves it alone
        CHECK(get(kept[0] / "hailstone") == "the only copy");
    }
    SUBCASE("cut off between a journal line and its move") {
        // The journal names a move to previous/ that never happened: the file in place is the original and stays.
        put(inst / "update/journal.txt", "moved hailstone\nremoved shaders/a.spv\n");
        REQUIRE(write_file_atomic(inst / "update/state.txt", "installing 0.2.0 0.1.0\n"));
        update_at_start(inst, "0.1.0");
        update_at_start(inst, "0.1.0"); // the next start empties the trash
        CHECK(get(inst / "hailstone") == "old");
        CHECK(get(inst / "shaders/a.spv") == "old a");
    }
    SUBCASE("cut off between the hard link into previous/ and the rename over the old file") {
        // Linux and macOS: previous/ holds a second name for the old file, which is still in place.
        std::error_code ec;
        fs::create_directories(inst / "update/previous", ec);
        if (platform::link_file_synced(inst / "hailstone", inst / "update/previous/hailstone", ec)) {
            put(inst / "update/journal.txt", "moved hailstone\n");
            REQUIRE(write_file_atomic(inst / "update/state.txt", "installing 0.2.0 0.1.0\n"));
            update_at_start(inst, "0.1.0");
            update_at_start(inst, "0.1.0");
            CHECK(get(inst / "hailstone") == "old");
            CHECK_FALSE(fs::exists(inst / "update/previous"));
        }
    }
    SUBCASE("a torn or damaged journal moves no folder and nothing of the player's") {
        // The last line was cut off mid-name ("added ui" of "added ui/x.lua"): it names a move that never began.
        // The lines before it name a folder, the player's settings and a path outside the folder: none is acted on.
        put(inst / "ui/settings/modern.txt", "the player's");
        put(inst / "ui/modern/login.lua", "shipped");
        put(inst / "hailstone.ini", "mine");
        put(inst / "update/journal.txt",
            "added shaders\nadded ui/settings/modern.txt\nadded hailstone.ini\nadded ../outside\nadded ui");
        REQUIRE(write_file_atomic(inst / "update/state.txt", "installing 0.2.0 0.1.0\n"));
        update_at_start(inst, "0.1.0");
        update_at_start(inst, "0.1.0");
        CHECK(get(inst / "ui/settings/modern.txt") == "the player's");
        CHECK(get(inst / "ui/modern/login.lua") == "shipped");
        CHECK(get(inst / "shaders/a.spv") == "old a");
        CHECK(get(inst / "hailstone.ini") == "mine");
        CHECK(get(inst / "hailstone") == "old");
    }
    SUBCASE("a whole journal line is still undone") {
        put(inst / "icons/n.png", "added by the cut-off install");
        put(inst / "update/journal.txt", "added icons/n.png\nadded ico");
        REQUIRE(write_file_atomic(inst / "update/state.txt", "installing 0.2.0 0.1.0\n"));
        update_at_start(inst, "0.1.0");
        update_at_start(inst, "0.1.0");
        CHECK_FALSE(fs::exists(inst / "icons/n.png"));
        CHECK(fs::is_directory(inst / "icons"));
    }
    SUBCASE("does not start") {
        stage();
        REQUIRE(install_staged_update(inst, "0.1.0").ok);
        CHECK(update_at_start(inst, "0.2.0").message.empty()); // start 1
        CHECK(update_at_start(inst, "0.2.0").message.empty()); // start 2
        auto note = update_at_start(inst, "0.2.0");          // start 3: the first two never reached the login
        CHECK(note.restart);
        CHECK(get(inst / "hailstone") == "old");
        CHECK(!fs::exists(inst / "icons/n.png"));
        CHECK(failed_update_version(inst) == "0.2.0");
        // The old version it starts is told why, once; the trash goes.
        auto told = update_at_start(inst, "0.1.0");
        CHECK(!told.restart);
        CHECK(told.message.find("did not start") != std::string::npos);
        CHECK(!fs::exists(inst / "update/trash"));
        CHECK(update_at_start(inst, "0.1.0").message.empty());
    }
    SUBCASE("the player put another version in") {
        stage();
        REQUIRE(install_staged_update(inst, "0.1.0").ok);
        CHECK(update_at_start(inst, "0.5.0").message.empty());
        CHECK(!fs::exists(inst / "update/state.txt"));
        CHECK(!fs::exists(inst / "update/previous"));
    }
}

TEST_CASE("update: a server plugin an earlier release shipped goes when it is as shipped, its settings stay") {
    test::ScratchDir dir("update_retire");
    const fs::path inst = dir.path();
    const std::string lib = "the plugin as 0.2.0 shipped it", window = "-- its window", changed = "-- edited";
    const std::string lib_hash = to_hex(sha256(lib)), window_hash = to_hex(sha256(window));
    const std::vector<RetiredFile> retired = {
        {"plugins/nms/nms.so", lib.size(), lib_hash},
        {"plugins/nms/ui/modern/lib/loot.lua", window.size(), window_hash},
        {"plugins/nms/ui/modern/vault.lua", window.size(), window_hash},
        {"plugins/projecttriune/projecttriune.so", lib.size(), lib_hash},
        {"plugins/projecttriune/ui/modern/triunewaypoints.lua", window.size(), window_hash},
    };
    put(inst / "plugins/nms/nms.so", lib);
    put(inst / "plugins/nms/ui/modern/lib/loot.lua", window);
    put(inst / "plugins/nms/ui/modern/vault.lua", changed); // the player changed it: it stays
    put(inst / "plugins/nms/nms.ini", "settings");          // the plugin's settings stay
    put(inst / "plugins/projecttriune/projecttriune.so", "another build (the add-on's)");
    // The add-on's window file, unchanged since the old release: the add-on's, so it stays.
    put(inst / "plugins/projecttriune/ui/modern/triunewaypoints.lua", window);
    put(inst / "plugins/other/other.so", lib); // not in the list

    // While an update is pending, a rollback to the release that shipped them needs them: nothing goes.
    REQUIRE(write_file_atomic(inst / "update/state.txt", "pending 0.3.0 0.2.0 1\n"));
    CHECK(retire_old_release_files(inst, retired).empty());
    CHECK(fs::exists(inst / "plugins/nms/nms.so"));
    fs::remove(inst / "update/state.txt");

    const auto removed = retire_old_release_files(inst, retired);
    CHECK(removed == std::vector<std::string>{"plugins/nms/nms.so", "plugins/nms/ui/modern/lib/loot.lua"});
    CHECK(!fs::exists(inst / "plugins/nms/nms.so"));
    CHECK(!fs::exists(inst / "plugins/nms/ui/modern/lib")); // emptied, so gone
    CHECK(get(inst / "plugins/nms/ui/modern/vault.lua") == changed);
    CHECK(get(inst / "plugins/nms/nms.ini") == "settings");
    CHECK(get(inst / "plugins/projecttriune/projecttriune.so") == "another build (the add-on's)");
    CHECK(get(inst / "plugins/projecttriune/ui/modern/triunewaypoints.lua") == window);
    CHECK(get(inst / "plugins/other/other.so") == lib);
    CHECK(retire_old_release_files(inst, retired).empty()); // a second start finds nothing

    // A folder holding nothing else goes, but never plugins/ itself.
    test::ScratchDir dir2("update_retire_all");
    put(dir2.path() / "plugins/nms/nms.so", lib);
    CHECK(retire_old_release_files(dir2.path(), retired).size() == 1);
    CHECK(!fs::exists(dir2.path() / "plugins/nms"));
    CHECK(fs::is_directory(dir2.path() / "plugins"));
}

TEST_CASE("update: a pack file an earlier release shipped and a later one renamed goes, its folder stays") {
    test::ScratchDir dir("update_retire_ui");
    const fs::path inst = dir.path();
    const std::string old_file = "-- the menu button as 0.7.1 shipped it", old_hash = to_hex(sha256(old_file));
    const std::vector<RetiredFile> retired = {{"ui/modern/eqmain.lua", old_file.size(), old_hash}};
    put(inst / "ui/modern/eqmain.lua", old_file);
    const auto removed = retire_old_release_files(inst, retired);
    CHECK(removed == std::vector<std::string>{"ui/modern/eqmain.lua"});
    CHECK(!fs::exists(inst / "ui/modern/eqmain.lua"));
    CHECK(fs::is_directory(inst / "ui/modern")); // emptied, but a pack's folder is not the updater's to remove

    // The player's own edit of it stays.
    put(inst / "ui/modern/eqmain.lua", "-- edited");
    CHECK(retire_old_release_files(inst, retired).empty());
    CHECK(get(inst / "ui/modern/eqmain.lua") == "-- edited");
}

TEST_CASE("update: the retired files' table") {
    // A typo in a row would make it match nothing; every row names a server plugin's file or a renamed pack file.
    CHECK(retired_release_files().size() == 30);
    for (const RetiredFile& f : retired_release_files()) {
        CAPTURE(f.path);
        CHECK(parse_sha256(f.sha256));
        CHECK(f.size > 0);
        CHECK((f.path.starts_with("plugins/nms/") || f.path.starts_with("plugins/projecttriune/") ||
               f.path == "ui/modern/eqmain.lua"));
    }
    test::ScratchDir dir("update_retire_table");
    CHECK(retire_old_release_files(dir.path()).empty());
}

// A box set's other windows run from the folder the first window updates: two updaters would download into the same
// download/<file>.part, and one quitting first could install while its siblings run (docs/updater.md "Install").
TEST_CASE("updater: a box set's other windows and test drives have no updater") {
    CHECK(run_has_updates(0, false, false));
    CHECK_FALSE(run_has_updates(0, false, true));
    CHECK_FALSE(run_has_updates(120, false, false));
    CHECK_FALSE(run_has_updates(0, true, false));
    CHECK(std::string(kBoxChildArg) == "--box-child");
}
