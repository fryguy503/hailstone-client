// plugin/policy.h: the server's rules from its hailstone.cfg, and net/settings_packets.h.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "core/sha256.h"
#include "net/settings_packets.h"
#include "plugin/policy.h"

using namespace hailstone;
using plugin::FileKind;
using plugin::Policy;
namespace fs = std::filesystem;

TEST_CASE("hailstone.cfg: shipped templates leave player code disabled and identity lists empty") {
    const auto source = fs::path(__FILE__).parent_path().parent_path();
    for (const char* name : {"hailstone.cfg.example", "hailstone-addon-policy.cfg.example"}) {
        CAPTURE(name);
        std::ifstream in(source / "server" / name, std::ios::binary);
        REQUIRE(in.good());
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        std::vector<std::string> warnings;
        const auto rules = plugin::parse_server_cfg(text, &warnings);
        CHECK(warnings.empty());
        CHECK(rules.addons.error.empty());
        CHECK_FALSE(rules.deny_client);
        CHECK(rules.player_script_caps == 0);
        CHECK(rules.player_plugin_caps == 0);
        CHECK(rules.addons.required.empty());
        CHECK(rules.addons.allow.empty());
    }
}

TEST_CASE("hailstone.cfg: [client] deny is off by default and can deny Hailstone access") {
    CHECK_FALSE(plugin::Rules{}.deny_client);
    CHECK_FALSE(plugin::parse_server_cfg("").deny_client);
    CHECK_FALSE(plugin::parse_server_cfg("[client]\ndeny = off\n").deny_client);
    std::vector<std::string> warnings;
    CHECK(plugin::parse_server_cfg("[Client]\nDeny = ON ; no Hailstone\n", &warnings).deny_client);
    CHECK(warnings.empty());
    CHECK_FALSE(plugin::parse_server_cfg("[client]\ndeny = maybe\n", &warnings).deny_client);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front() == "[client] deny: \"maybe\" is not on or off");
}

TEST_CASE("hailstone.cfg: every setting, defaults for the rest, warnings for what it does not know") {
    std::vector<std::string> warnings;
    auto r = plugin::parse_server_cfg(R"(
; a comment
[scripts]
player = on
player_caps = read, ui     ; trailing comment
run = hud, autoloot

[Plugins]
Player = Off

[assets]
player = off

[socials]
mode = full
caps = read,target,actions,move
press_commands = 3
faces = no

[web]
allow = Example.org, wiki.example.net
)",
                                      &warnings);
    CHECK(warnings.empty());
    CHECK(r.configured);
    CHECK(r.player_script_caps == (plugin::kCapRead | plugin::kCapUi));
    CHECK(r.player_plugin_caps == 0);
    CHECK_FALSE(r.player_assets);
    CHECK(r.run_scripts == std::vector<std::string>{"hud", "autoloot"});
    CHECK(r.social_mode == plugin::ButtonMode::Full);
    CHECK(r.social_caps == (plugin::kCapRead | plugin::kCapTarget | plugin::kCapActions | plugin::kCapMove));
    CHECK(r.social_press_commands == 3);
    CHECK_FALSE(r.social_faces);
    CHECK(r.web_allow == std::vector<std::string>{"example.org", "wiki.example.net"});

    auto d = plugin::parse_server_cfg("");
    CHECK(d.configured);
    CHECK(d.player_script_caps == 0);
    CHECK(d.player_plugin_caps == 0);
    CHECK(d.player_assets);
    CHECK(d.social_mode == plugin::ButtonMode::Press);
    CHECK(d.web_allow.empty());

    // "player = on" alone: every capability.
    CHECK(plugin::parse_server_cfg("[plugins]\nplayer = yes\n").player_plugin_caps == plugin::kCapAll);

    warnings.clear();
    auto w = plugin::parse_server_cfg("[scripts]\nplayer = maybe\nplayer_caps = read,fly\ncolour = red\n[macros]\nx = 1\n"
                                      "[socials]\nmode = loud\npress_commands = -1\n",
                                      &warnings);
    CHECK(w.player_script_caps == 0);
    CHECK(warnings.size() == 7); // maybe, fly, colour, [macros], loud, -1, and player_caps without player = on
    // A section that is not the client's is kept for scripts (hailstone.server_rule).
    CHECK(w.script_sections.size() == 1);
    CHECK(w.script_sections["macros"]["x"] == "1");
    auto a = plugin::parse_server_cfg("[AutoCombat]\nHunter = off   ; no unattended hunting\nassist = on\n");
    CHECK(a.script_sections["autocombat"]["hunter"] == "off");
    CHECK(a.script_sections["autocombat"]["assist"] == "on");
    CHECK(r.script_sections.empty());
}

TEST_CASE("server_rules: none without hailstone.cfg; its own scripts and plugins listed; a changed cfg gives defaults") {
    fs::path dir = fs::temp_directory_path() / "hailstone_server_rules_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string cfg = "[scripts]\nplayer = on\n";
    std::ofstream(dir / "hailstone.cfg", std::ios::binary) << cfg;
    Sha256Digest script = sha256(std::string_view("print(1)"));
    std::vector<plugin::ListedFile> files{{"assets/spells_us.txt", sha256(std::string_view("s"))},
                                          {"scripts/hud.lua", script},
                                          {"plugins/MQ2Hud.so", sha256(std::string_view("p"))}};

    auto none = plugin::server_rules(dir, files);
    CHECK_FALSE(none.configured);
    CHECK(none.dir == dir);
    CHECK(none.server_files.size() == 2);

    files.push_back({"hailstone.cfg", sha256(cfg)});
    auto r = plugin::server_rules(dir, files);
    CHECK(r.configured);
    CHECK(r.player_script_caps == plugin::kCapAll);
    REQUIRE(r.server_files.size() == 2);
    CHECK(r.server_files[0].kind == FileKind::Script);
    CHECK(r.server_files[1].kind == FileKind::Plugin);

    std::ofstream(dir / "hailstone.cfg", std::ios::binary) << "[scripts]\nplayer = on\n[plugins]\nplayer = on\n";
    std::vector<std::string> warnings;
    auto changed = plugin::server_rules(dir, files, &warnings);
    CHECK(changed.configured);
    CHECK(changed.player_script_caps == 0);
    CHECK(changed.player_plugin_caps == 0);
    CHECK(warnings.size() == 1);
    fs::remove_all(dir);
}

TEST_CASE("Policy: nothing before the rules; everything without a cfg; the server's own, the player's as allowed") {
    Policy pol;
    Sha256Digest server = sha256(std::string_view("server's")), mine = sha256(std::string_view("mine"));
    CHECK(pol.caps(FileKind::Script, mine) == 0);
    CHECK(pol.button_rules().mode == plugin::ButtonMode::Off);

    pol.set(plugin::Rules{});
    CHECK(pol.caps(FileKind::Script, mine) == plugin::kCapAll);
    CHECK(pol.caps(FileKind::Plugin, mine) == plugin::kCapAll);
    CHECK(pol.button_rules().mode == plugin::ButtonMode::Full);
    CHECK(pol.button_rules().caps ==
          (plugin::kCapAll & ~(plugin::kCapPackets | plugin::kCapExtend | plugin::kCapLogin | plugin::kCapNet)));

    plugin::Rules r;
    r.configured = true;
    r.player_script_caps = plugin::kCapRead;
    r.server_files.push_back({FileKind::Plugin, "plugins/x.so", server});
    pol.set(r);
    CHECK(pol.caps(FileKind::Plugin, server) == plugin::kCapAll);
    CHECK(pol.server_file(FileKind::Plugin, server));
    CHECK_FALSE(pol.server_file(FileKind::Script, server));
    CHECK(pol.caps(FileKind::Plugin, mine) == 0);
    CHECK(pol.caps(FileKind::Script, mine) == plugin::kCapRead);
    CHECK(pol.refusal(FileKind::Plugin) == "this server runs only its own plugins (its hailstone.cfg)");
    pol.clear();
    CHECK(pol.caps(FileKind::Plugin, server) == 0);
}

TEST_CASE("Policy: provisional rules before the zone-in run only what has 'login'") {
    Policy pol;
    Sha256Digest server = sha256(std::string_view("server's")), mine = sha256(std::string_view("mine"));
    plugin::Rules r;
    r.provisional = true; // no hailstone.cfg known from the last login
    pol.set(r);
    CHECK(pol.caps(FileKind::Plugin, mine) == plugin::kCapAll);
    CHECK(pol.button_rules().mode == plugin::ButtonMode::Off); // socials from the zone-in

    r.configured = true;
    r.player_plugin_caps = plugin::kCapRead;
    r.server_files.push_back({FileKind::Plugin, "plugins/x.so", server});
    pol.set(r);
    CHECK(pol.caps(FileKind::Plugin, server) == plugin::kCapAll);
    CHECK(pol.caps(FileKind::Plugin, mine) == 0);
    CHECK(pol.refusal(FileKind::Plugin).find("'login'") != std::string::npos);
    r.player_plugin_caps = plugin::kCapRead | plugin::kCapLogin;
    pol.set(r);
    CHECK(pol.caps(FileKind::Plugin, mine) == (plugin::kCapRead | plugin::kCapLogin));

    // After the zone-in 'login' is not needed.
    r.provisional = false;
    r.player_plugin_caps = plugin::kCapRead;
    pol.set(r);
    CHECK(pol.caps(FileKind::Plugin, mine) == plugin::kCapRead);
    CHECK(plugin::parse_caps("read,login") == (plugin::kCapRead | plugin::kCapLogin));
    CHECK(plugin::caps_text(plugin::kCapLogin | plugin::kCapExtend) == "extend,login");
}

TEST_CASE("Policy: Lua social rules from [socials]") {
    Policy pol;
    plugin::Rules r = plugin::parse_server_cfg("[socials]\nmode = press\ncaps = all\npress_commands = 2\n");
    pol.set(r);
    auto b = pol.button_rules();
    CHECK(b.mode == plugin::ButtonMode::Press);
    CHECK(b.caps == (plugin::kCapAll & ~(plugin::kCapEvents | plugin::kCapUi | plugin::kCapWeb | plugin::kCapPackets |
                                     plugin::kCapExtend | plugin::kCapLogin | plugin::kCapNet)));
    CHECK(b.press_commands == 2);
    CHECK(b.faces);
    pol.set(plugin::parse_server_cfg("[socials]\nmode = full\ncaps = all\n"));
    CHECK(pol.button_rules().caps ==
          (plugin::kCapAll & ~(plugin::kCapPackets | plugin::kCapExtend | plugin::kCapLogin | plugin::kCapNet)));
    pol.set(plugin::parse_server_cfg("[socials]\nmode = off\n"));
    CHECK(pol.button_rules().mode == plugin::ButtonMode::Off);
    CHECK(pol.button_rules().caps == 0);
}

TEST_CASE("command_caps: EQ commands by their handler, abbreviations too; text is chat") {
    using namespace plugin;
    CHECK(command_caps("/target a_bat") == kCapTarget);
    CHECK(command_caps("/assist") == kCapTarget);
    CHECK(command_caps("/cast 1") == kCapActions);
    CHECK(command_caps("/disc Fortitude") == kCapActions);
    CHECK(command_caps("/useitem 22") == kCapActions);
    CHECK(command_caps("/attack on") == kCapActions);
    // Hailstone's own item commands (not in RoF2's table) need what using an item needs, by their whole word.
    CHECK(command_caps("/destroycursor 123") == kCapActions);
    CHECK(command_caps("/STOWCURSOR 123") == kCapActions);
    CHECK(command_caps("/destroy") == kCapActions);
    CHECK(command_caps("/Destroy") == kCapActions);
    CHECK(command_caps("/stowcursorx 123") == kCapCommands);
    CHECK(command_caps("/destro") == kCapCommands); // no prefix match: the client runs it as no command
    CHECK(command_caps("/finditem sword") == kCapCommands); // a window, not an item move
    // RoF2 commands that act in the game, not only windows or options.
    CHECK(command_caps("/pet attack") == kCapActions);
    CHECK(command_caps("/loot") == kCapActions);
    CHECK(command_caps("/autoinventory") == kCapActions);
    CHECK(command_caps("/alt act 1") == kCapActions);
    CHECK(command_caps("/alternateadv buy 1") == kCapActions);
    CHECK(command_caps("/split 1 0 0 0") == kCapActions);
    CHECK(command_caps("/consent bob") == kCapActions);
    CHECK(command_caps("/guilddelete") == kCapActions);
    CHECK(command_caps("/petition help") == kCapCommands); // not /pet
    CHECK(command_caps("/autosplit") == kCapCommands);      // an option
    CHECK(command_caps("/sit") == kCapMove);
    CHECK(command_caps("/follow") == kCapMove);
    CHECK(command_caps("/g hello") == kCapChat);
    CHECK(command_caps("/tell bob hi") == kCapChat);
    CHECK(command_caps("hello there") == kCapChat);
    CHECK(command_caps(";tell x hi") == kCapChat);
    CHECK(command_caps("/who all") == kCapCommands);
    CHECK(command_caps("/stick 10") == kCapCommands); // an MQ command: its handler checks movement itself
    CHECK(command_caps("/url https://example.org") == kCapWeb);
    CHECK(command_caps("/www example.org") == kCapWeb);
    CHECK(command_caps("") == 0);
}

TEST_CASE("caps text round trip") {
    using namespace plugin;
    CHECK(caps_text(kCapRead | kCapUi) == "read,ui");
    CHECK(caps_text(kCapWeb) == "web");
    CHECK(parse_caps("web") == kCapWeb);
    CHECK(caps_text(0) == "none");
    CHECK(parse_caps("read, ui,bogus") == (kCapRead | kCapUi));
    CHECK(parse_caps("all") == kCapAll);
    CHECK(parse_caps(caps_text(kCapAll)) == kCapAll);
}

TEST_CASE("settings packets: request layout, answer round trip, later fields ignored") {
    auto req = net::encode_hailstone_settings_request();
    CHECK(req.opcode == 0x1432);
    REQUIRE(req.data.size() == 6);
    CHECK(req.data[0] == std::byte{'N'});
    CHECK(req.data[4] == std::byte{1});

    net::HailstoneSettings v;
    v.flags = 0x5; // no flag is defined now; any value goes through
    auto p = net::encode(v);
    CHECK(p.opcode == 0x1433);
    REQUIRE(p.data.size() == 10);
    auto d = net::decode_hailstone_settings(p);
    REQUIRE(d);
    CHECK(d->flags == 0x5u);

    auto longer = p;
    longer.data.push_back(std::byte{7}); // a later version's field
    REQUIRE(net::decode_hailstone_settings(longer));
    auto bad = p;
    bad.data.resize(9);
    CHECK_FALSE(net::decode_hailstone_settings(bad));
    bad = p;
    bad.data[4] = std::byte{2};
    CHECK_FALSE(net::decode_hailstone_settings(bad));
    bad = p;
    bad.opcode = 0x1421; // another Hailstone opcode
    CHECK_FALSE(net::decode_hailstone_settings(bad));
}

TEST_CASE("policy: an add-on's caps are a ceiling on what its files get") {
    using namespace hailstone::plugin;
    CHECK(parse_caps_strict("read, ui") == (kCapRead | kCapUi));
    CHECK(parse_caps_strict("all") == kCapAll);
    CHECK(parse_caps_strict("none") == 0u);
    CHECK_FALSE(parse_caps_strict("read,flying"));
    CHECK(caps_described(kCapRead | kCapChat).size() == 2);
    CHECK(caps_described(kCapAll).size() == 13);

    const Sha256Digest addon = sha256(std::string_view("addon.lua")), own = sha256(std::string_view("mine.lua"));
    const Sha256Digest server = sha256(std::string_view("server.lua"));
    Policy p;
    p.set_ceilings({{FileKind::Script, addon, kCapRead | kCapUi}});

    // A server without a hailstone.cfg: the player's own get everything, the add-on its ceiling.
    Rules open;
    open.server_files = {{FileKind::Script, "scripts/server.lua", server}};
    p.set(open);
    CHECK(p.caps(FileKind::Script, own) == kCapAll);
    CHECK(p.caps(FileKind::Script, addon) == (kCapRead | kCapUi));
    CHECK(p.caps(FileKind::Script, server) == kCapAll);
    CHECK(p.caps(FileKind::Plugin, addon) == kCapAll); // a ceiling is for its kind
    CHECK(p.ceiling(FileKind::Script, addon) == (kCapRead | kCapUi));
    CHECK_FALSE(p.ceiling(FileKind::Script, own));

    // A server that allows less: the less of the two.
    Rules strict = parse_server_cfg("[scripts]\nplayer = on\nplayer_caps = read,actions\n");
    p.set(strict);
    CHECK(p.caps(FileKind::Script, own) == (kCapRead | kCapActions));
    CHECK(p.caps(FileKind::Script, addon) == kCapRead);
    // The ceilings stay over a change of server and a disconnect.
    p.clear();
    p.set(open);
    CHECK(p.caps(FileKind::Script, addon) == (kCapRead | kCapUi));

    // Before the zone-in only what has 'login'; the ceiling takes it away too.
    Rules early = open;
    early.provisional = true;
    p.set(early);
    CHECK(p.caps(FileKind::Script, own) == kCapAll);
    CHECK(p.caps(FileKind::Script, addon) == 0u);

    // A file both of two add-ons ship gets what both allow.
    p.set(open);
    p.set_ceilings({{FileKind::Script, addon, kCapRead | kCapUi}, {FileKind::Script, addon, kCapRead | kCapChat}});
    CHECK(p.caps(FileKind::Script, addon) == kCapRead);
    // The server's own file with the same bytes as an add-on's is the server's.
    p.set_ceilings({{FileKind::Script, server, kCapRead}});
    CHECK(p.caps(FileKind::Script, server) == kCapAll);
}

TEST_CASE("add-on rules: enforcement is opt-in and suggestions keep their meaning") {
    using namespace plugin;
    const auto legacy = parse_server_cfg("[scripts]\nplayer = on\nplayer_caps = read,ui\n");
    const auto dormant = parse_server_cfg("[scripts]\nplayer = on\nplayer_caps = read,ui\n"
                                          "[addons]\ndefault = deny\nrequired = owner/needed\nblock = owner/mine\n"
                                          "suggest = Owner/Helpful, https://github.com/Owner/Other\n");
    CHECK_FALSE(dormant.addons.enabled);
    CHECK(dormant.addons.error.empty());
    CHECK(addon_allowed(dormant.addons, "owner/mine"));
    CHECK_FALSE(addon_required(dormant.addons, "owner/needed"));
    CHECK(dormant.suggest_addons == std::vector<std::string>{"owner/helpful", "owner/other"});
    Policy p;
    const auto hash = sha256(std::string_view("untracked script"));
    p.set(legacy);
    const auto before = p.caps(FileKind::Script, hash);
    p.set(dormant);
    CHECK(p.caps(FileKind::Script, hash, "mine") == before);

    const auto off = parse_server_cfg("[addons]\nenforce = off\ndefault = broken\nallow = *\n");
    CHECK_FALSE(off.addons.enabled);
    CHECK(off.addons.error.empty());
    CHECK(addon_allowed(off.addons, "owner/anything"));
}

TEST_CASE("add-on rules: identities are GitHub repositories, without a release or branch") {
    using namespace plugin;
    std::vector<std::string> warnings;
    const auto r = parse_server_cfg("[addons]\nEnforce = ON\nDefault = DENY\n"
                                    "Required = https://GitHub.com/Owner/Required.git/\n"
                                    "Allow = Owner/Optional, OWNER/OPTIONAL\nBlock = owner/forbidden\n", &warnings);
    CHECK(warnings.empty());
    CHECK(r.addons.enabled);
    CHECK_FALSE(r.addons.default_allow);
    CHECK(r.addons.error.empty());
    CHECK(r.addons.required == std::vector<std::string>{"owner/required"});
    CHECK(r.addons.allow == std::vector<std::string>{"owner/optional"});
    CHECK(r.addons.block == std::vector<std::string>{"owner/forbidden"});
    CHECK(addon_identity_matches("OWNER/REQUIRED", "owner/required@v2.0"));
    CHECK(addon_identity_matches("owner/required", "https://github.com/OWNER/Required/tree/main"));
    CHECK(addon_identity_matches("Owner/Required.git", "owner/required@v2"));
    CHECK_FALSE(addon_identity_matches("owner/required", "another/required"));
    CHECK_FALSE(addon_identity_matches("owner/required", ""));
    CHECK_FALSE(addon_identity_matches("required", "required"));
    CHECK(addon_allowed(r.addons, "Owner/Required#next"));
    CHECK(addon_allowed(r.addons, "owner/optional@v1"));
    CHECK_FALSE(addon_allowed(r.addons, "owner/forbidden"));
    CHECK_FALSE(addon_allowed(r.addons, "owner/anything_else"));
    CHECK_FALSE(addon_allowed(r.addons, "")); // a hand-installed add-on has no identity to allow
    CHECK(addon_identity("owner/Required@v1") == "owner/required");
    CHECK(addon_identity("").empty());
    for (const char* bad : {"*", "helper", "Required_Name", "../optional", "https://example.org/owner/optional"}) {
        CAPTURE(bad);
        CHECK_FALSE(normalize_addon_identity(bad));
    }

    const auto open = parse_server_cfg("[addons]\nenforce = on\n");
    CHECK(open.addons.default_allow);
    CHECK(addon_allowed(open.addons, "owner/anything_else"));
    CHECK(addon_allowed(open.addons, ""));
}

TEST_CASE("add-on rules: a suggested repository is allowed under default deny, optional, blockable, no caps") {
    using namespace plugin;
    const auto r = parse_server_cfg("[scripts]\nplayer = on\nplayer_caps = read,ui\n"
                                    "[addons]\nenforce = on\ndefault = deny\nallow = owner/manual\n"
                                    "suggest = Owner/Optional@v2, https://github.com/Owner/Branches/tree/next, "
                                    "owner/blocked, owner/manual\nblock = owner/blocked\n");
    REQUIRE(r.addons.error.empty());
    CHECK(r.addons.allow == std::vector<std::string>{"owner/manual", "owner/optional", "owner/branches", "owner/blocked"});
    CHECK(addon_allowed(r.addons, "owner/optional@v9"));
    CHECK(addon_allowed(r.addons, "OWNER/BRANCHES"));
    CHECK_FALSE(addon_allowed(r.addons, "owner/blocked")); // block wins over a suggestion
    CHECK_FALSE(addon_allowed(r.addons, "owner/unlisted"));
    CHECK_FALSE(addon_required(r.addons, "owner/optional")); // a suggestion is never a requirement

    const auto optional = sha256(std::string_view("optional script"));
    const auto blocked = sha256(std::string_view("blocked script"));
    Policy p;
    p.set_ceilings({{FileKind::Script, optional, kCapRead | kCapUi | kCapMove, "optional", "owner/optional"},
                    {FileKind::Script, blocked, kCapRead, "blocked", "owner/blocked"}});
    p.set(r);
    // Allowed by identity, but no more than player_caps: a suggestion grants no capability.
    CHECK(p.caps(FileKind::Script, optional) == (kCapRead | kCapUi));
    CHECK(p.caps(FileKind::Script, blocked) == 0);
    CHECK(p.required_entries(FileKind::Script).empty());

    // With the player's scripts off, a suggested one does not run either.
    p.set(parse_server_cfg("[addons]\nenforce = on\ndefault = deny\nsuggest = owner/optional\n"));
    CHECK(p.caps(FileKind::Script, optional) == 0);

    // Not enforced: the suggestion changes nothing.
    const auto off = parse_server_cfg("[scripts]\nplayer = on\n[addons]\ndefault = deny\nsuggest = owner/optional\n");
    CHECK(addon_allowed(off.addons, "owner/unlisted"));
}

TEST_CASE("add-on rules: required minimum versions keep repository identity separate and preserve SemVer case") {
    using namespace plugin;
    std::vector<std::string> warnings;
    const auto r = parse_server_cfg("[addons]\nenforce = on\ndefault = deny\n"
                                    "required = Owner/Need:1.0.0-preview.4, "
                                    "https://github.com/Owner/Other.git:V2.3.4-Preview.X+Build.9, Owner/Plain ; comment\n",
                                    &warnings);
    REQUIRE(r.addons.error.empty());
    CHECK(warnings.empty());
    CHECK(r.addons.required == std::vector<std::string>{"owner/need", "owner/other", "owner/plain"});
    REQUIRE(r.addons.minimum_versions.size() == 2);
    CHECK(r.addons.minimum_versions.at("owner/need").to_string() == "1.0.0-preview.4");
    CHECK(r.addons.minimum_versions.at("owner/other").to_string() == "2.3.4-Preview.X");
    CHECK_FALSE(r.addons.minimum_versions.contains("owner/plain"));
    CHECK(addon_required(r.addons, "OWNER/NEED@v0.9.0")); // installed-version validation is admission's job
    CHECK(addon_allowed(r.addons, "owner/need"));
    CHECK_FALSE(addon_allowed(r.addons, "another/need"));
}

TEST_CASE("add-on rules: duplicate requirements retain the strongest minimum regardless of order") {
    using namespace plugin;
    for (const char* list : {
            "owner/repo, owner/repo:1.0.0-preview.10, OWNER/REPO:1.0.0-preview.4, owner/repo:1.0.0, owner/repo",
            "owner/repo:1.0.0, owner/repo:1.0.0-preview.4, owner/repo, owner/repo:1.0.0-preview.10"}) {
        CAPTURE(list);
        const auto r = parse_server_cfg(std::string("[addons]\nenforce = on\nrequired = ") + list + "\n");
        REQUIRE(r.addons.error.empty());
        CHECK(r.addons.required == std::vector<std::string>{"owner/repo"});
        REQUIRE(r.addons.minimum_versions.size() == 1);
        CHECK(r.addons.minimum_versions.at("owner/repo").to_string() == "1.0.0");
    }
    const auto pre = parse_server_cfg("[addons]\nenforce = on\n"
                                      "required = owner/repo:1.0.0-preview.4, owner/repo:1.0.0-preview.10\n");
    REQUIRE(pre.addons.error.empty());
    CHECK(pre.addons.minimum_versions.at("owner/repo").to_string() == "1.0.0-preview.10");
}

TEST_CASE("add-on rules: malformed minimums fail closed only when enforcement is enabled") {
    using namespace plugin;
    for (const char* item : {"owner/repo:", "owner/repo:1.2", "owner/repo:01.2.3", "owner/repo:>=1.2.3",
                             "owner/repo:1.2.3.4", "owner/repo:1.2.3-pre..1", "owner/repo:1.2.3+bad!",
                             "owner/repo:1.2.3:2.0.0", "repo:1.2.3", "owner/repo:1.0.0-preview.04"}) {
        CAPTURE(item);
        const std::string policy = std::string("[addons]\ndefault = deny\nrequired = ") + item + "\n";
        const auto enabled = parse_server_cfg(policy + "enforce = on\n");
        CHECK(enabled.addons.enabled);
        CHECK_FALSE(enabled.addons.error.empty());
        CHECK_FALSE(addon_allowed(enabled.addons, "owner/repo"));
        CHECK_FALSE(addon_required(enabled.addons, "owner/repo"));
        CHECK(enabled.addons.minimum_versions.empty());
        const auto disabled = parse_server_cfg(policy + "enforce = off\n");
        CHECK_FALSE(disabled.addons.enabled);
        CHECK(disabled.addons.error.empty());
        CHECK(addon_allowed(disabled.addons, "owner/repo"));
    }
    for (const char* key : {"allow", "block"}) {
        CAPTURE(key);
        const auto r = parse_server_cfg(std::string("[addons]\nenforce = on\n") + key + " = owner/repo:1.2.3\n");
        CHECK_FALSE(r.addons.error.empty());
        CHECK(r.addons.minimum_versions.empty());
    }
    const auto conflict = parse_server_cfg("[addons]\nenforce = on\nrequired = Owner/Repo:1.2.3\nblock = owner/repo\n");
    CHECK(conflict.addons.error.find("both required and blocked") != std::string::npos);
}

TEST_CASE("add-on rules: a bare name is refused with a message for the server owner") {
    using namespace plugin;
    for (const char* key : {"required", "allow", "block"}) {
        CAPTURE(key);
        std::vector<std::string> warnings;
        const auto r = parse_server_cfg(std::string("[addons]\nenforce = on\n") + key + " = owner/fine, helper\n",
                                        &warnings);
        CHECK(r.addons.enabled);
        CHECK(r.addons.error.find("\"helper\" is a bare name") != std::string::npos);
        CHECK(r.addons.error.find("owner/repo") != std::string::npos);
        REQUIRE(warnings.size() == 1);
        CHECK(warnings[0] == r.addons.error);
        for (const auto* list : {&r.addons.required, &r.addons.allow, &r.addons.block})
            CHECK(std::find(list->begin(), list->end(), "helper") == list->end());
    }
}

TEST_CASE("add-on rules: malformed enabled policies fail closed and a required block is an error") {
    using namespace plugin;
    const char* invalid[] = {
        "[addons]\nenforce = maybe\n",
        "[addons]\nenforce = on\ndefault = denny\n",
        "[addons]\nenforce = on\nallow = *\n",
        "[addons]\nenforce = on\nallow = optional\n",
        "[addons]\nenforce = on\nrequired = owner/repo,,owner/other\n",
        "[addons]\nenforce = on\nallow = owner/optional,\n",
        "[addons]\nenforce = on\nrequire = owner/repo\n",
        "[addons]\nenforce = on\ndefault deny\n",
        "[addons\nenforce = on\n",
        "[addons]\nenforce = on\nrequired = OWNER/Repo\nblock = https://github.com/owner/repo/\n",
    };
    for (const auto* text : invalid) {
        CAPTURE(text);
        std::vector<std::string> warnings;
        auto r = parse_server_cfg(text, &warnings);
        CHECK(r.addons.enabled);
        CHECK_FALSE(r.addons.error.empty());
        CHECK_FALSE(warnings.empty());
        CHECK_FALSE(addon_allowed(r.addons, "owner/repo"));
        CHECK_FALSE(addon_allowed(r.addons, ""));
        const auto installed = sha256(std::string_view("installed plugin"));
        const auto server = sha256(std::string_view("server plugin"));
        r.server_files.push_back({FileKind::Plugin, "plugins/anything.dll", server});
        Policy p;
        p.set_ceilings({{FileKind::Plugin, installed, kCapAll, "repo", "owner/repo"}});
        p.set(r);
        CHECK(p.caps(FileKind::Plugin, installed) == 0);
        CHECK_FALSE(p.required_file(FileKind::Plugin, installed));
        CHECK(p.caps(FileKind::Plugin, server) == kCapAll); // the server's own files are not add-ons
    }
}

TEST_CASE("Policy: required and allowed add-ons are identified by every registered file hash") {
    using namespace plugin;
    const auto entry = sha256(std::string_view("required init.lua"));
    const auto module = sha256(std::string_view("required helper.lua"));
    const auto optional = sha256(std::string_view("optional plugin"));
    const auto changed = sha256(std::string_view("edited init.lua"));
    Policy p;
    p.set_ceilings({{FileKind::Script, entry, kCapRead | kCapUi, "needed", "owner/needed@v1"},
                    {FileKind::Script, module, kCapRead | kCapUi, "needed", "owner/needed@v1"},
                    {FileKind::Plugin, optional, kCapRead, "optional", "https://github.com/Owner/Optional"}});
    auto r = parse_server_cfg("[scripts]\nplayer = on\n[plugins]\nplayer = on\n"
                              "[addons]\nenforce = on\ndefault = deny\nrequired = owner/needed\nallow = owner/optional\n");
    p.set(r);
    CHECK(p.caps(FileKind::Script, entry, "renamed") == (kCapRead | kCapUi));
    CHECK(p.caps(FileKind::Script, module) == (kCapRead | kCapUi));
    CHECK(p.caps(FileKind::Plugin, optional) == kCapRead);
    CHECK(p.required_file(FileKind::Script, entry));
    CHECK(p.required_file(FileKind::Script, module));
    CHECK_FALSE(p.required_file(FileKind::Plugin, optional));
    CHECK(p.caps(FileKind::Plugin, entry, "needed") == 0); // the other file kind is not registered
    CHECK(p.caps(FileKind::Script, changed, "needed") == 0); // a name alone cannot admit edited bytes
    CHECK(p.caps(FileKind::Script, changed, "optional") == 0);
    CHECK_FALSE(p.required_file(FileKind::Script, changed));

    r.player_script_caps = kCapRead | kCapActions;
    p.set(r);
    CHECK(p.caps(FileKind::Script, entry) == kCapRead);
    r.player_script_caps = 0; // identity permission never overrides the existing capability restrictions
    p.set(r);
    CHECK(p.caps(FileKind::Script, entry) == 0);
    p.clear();
    CHECK(p.caps(FileKind::Script, entry) == 0);
}

TEST_CASE("Policy: shared bytes obey all add-on identities and explicit blocks win") {
    using namespace plugin;
    const auto shared = sha256(std::string_view("shared helper.lua"));
    Policy p;
    p.set_ceilings({{FileKind::Script, shared, kCapRead | kCapUi, "needed", "owner/needed"},
                    {FileKind::Script, shared, kCapRead | kCapChat, "other", "owner/other"}});
    auto r = parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\ndefault = deny\nrequired = owner/needed\n");
    p.set(r);
    CHECK(p.caps(FileKind::Script, shared) == 0);
    CHECK(p.required_file(FileKind::Script, shared)); // a refused requirement must still prevent joining
    r.addons.allow.push_back("owner/other");
    p.set(r);
    CHECK(p.caps(FileKind::Script, shared) == kCapRead);
    CHECK(p.required_file(FileKind::Script, shared));
    r.addons.allow.clear();
    r.addons.block.push_back("owner/other");
    p.set(r);
    CHECK(p.caps(FileKind::Script, shared) == 0);
    CHECK(p.required_file(FileKind::Script, shared));

    // Under default allow, other bytes running under a blocked add-on's name are still refused.
    r.addons.default_allow = true;
    p.set(r);
    const auto manual = sha256(std::string_view("hand installed"));
    CHECK(p.caps(FileKind::Script, manual, "Other") == 0);
    CHECK(p.caps(FileKind::Script, manual, "another_alias") == kCapAll);
}

TEST_CASE("Policy: the server's own files are never named, required or refused by add-on rules") {
    using namespace plugin;
    const auto script = sha256(std::string_view("server script"));
    const auto plugin_hash = sha256(std::string_view("server plugin"));
    auto r = parse_server_cfg("[addons]\nenforce = on\ndefault = deny\nrequired = owner/needed\n");
    r.server_files = {{FileKind::Script, "scripts/needed.lua", script},
                      {FileKind::Plugin, "plugins/needed/needed.dll", plugin_hash}};
    Policy p;
    p.set(r);
    CHECK(p.caps(FileKind::Script, script) == kCapAll);
    CHECK(p.caps(FileKind::Plugin, plugin_hash) == kCapAll);
    CHECK_FALSE(p.required_file(FileKind::Script, script));
    CHECK(p.required_entries(FileKind::Script).empty());
    CHECK(p.required_entries(FileKind::Plugin).empty());
    r.addons.block = {"owner/needed"};
    r.addons.required.clear();
    p.set(r);
    CHECK(p.caps(FileKind::Script, script, "needed") == kCapAll);
    r.addons.error = "broken";
    p.set(r);
    CHECK(p.caps(FileKind::Plugin, plugin_hash) == kCapAll);

    // An installed add-on's registered source also identifies the same bytes supplied by a server.
    r.addons.block.clear();
    r.addons.error.clear();
    r.addons.required = {"owner/approved"};
    p.set_ceilings({{FileKind::Plugin, plugin_hash, kCapRead, "approved", "owner/approved"}});
    p.set(r);
    CHECK(p.caps(FileKind::Plugin, plugin_hash) == kCapAll); // server-owned capabilities are preserved
    CHECK(p.required_file(FileKind::Plugin, plugin_hash));
}

TEST_CASE("Policy: opted-in identity checks preserve provisional login capability requirements") {
    using namespace plugin;
    const auto entry = sha256(std::string_view("required script"));
    Policy p;
    p.set_ceilings({{FileKind::Script, entry, kCapRead | kCapUi, "needed", "owner/needed"}});
    auto r = parse_server_cfg("[scripts]\nplayer = on\n[addons]\nenforce = on\ndefault = deny\nrequired = owner/needed\n");
    r.provisional = true;
    p.set(r);
    CHECK(p.caps(FileKind::Script, entry) == 0);
    p.set_ceilings({{FileKind::Script, entry, kCapRead | kCapLogin, "needed", "owner/needed"}});
    CHECK(p.caps(FileKind::Script, entry) == (kCapRead | kCapLogin));
}

TEST_CASE("Policy: edited add-on bytes retain blocked source identity without changing legacy capabilities") {
    using namespace plugin;
    const auto changed = sha256(std::string_view("edited addon source"));
    Policy p;
    p.set_ceilings({{FileKind::Script, changed, 0, "edited", "owner/original", false}});
    auto r = parse_server_cfg("[scripts]\nplayer = on\nplayer_caps = read,ui\n");
    p.set(r);
    CHECK_FALSE(p.ceiling(FileKind::Script, changed));
    CHECK(p.caps(FileKind::Script, changed) == (kCapRead | kCapUi));

    r.addons.enabled = true;
    r.addons.default_allow = true;
    p.set(r);
    CHECK(p.caps(FileKind::Script, changed) == (kCapRead | kCapUi));
    r.addons.block = {"owner/original"};
    p.set(r);
    CHECK(p.caps(FileKind::Script, changed, "renamed") == 0);
    const auto edited_later = sha256(std::string_view("edited again after the registry scan"));
    CHECK(p.caps(FileKind::Script, edited_later, "edited") == 0);
    CHECK(p.caps(FileKind::Script, edited_later, "untracked_copy") == (kCapRead | kCapUi));
    CHECK_FALSE(p.ceiling(FileKind::Script, edited_later));
    CHECK_FALSE(p.required_file(FileKind::Script, edited_later));

    r.addons.block.clear();
    r.addons.default_allow = false;
    r.addons.required = {"owner/original"};
    p.set(r);
    CHECK(p.caps(FileKind::Script, changed, "edited") == 0);
    CHECK(p.caps(FileKind::Script, edited_later, "edited") == 0);
    CHECK_FALSE(p.required_file(FileKind::Script, changed));
}

TEST_CASE("Policy: required entrypoints remain expected when absent shadowed or refused") {
    using namespace plugin;
    const auto entry = sha256(std::string_view("required init.lua"));
    const auto helper = sha256(std::string_view("required helper.lua"));
    const auto changed = sha256(std::string_view("edited init.lua"));
    const auto shadow = sha256(std::string_view("server's different init.lua"));
    Policy p;
    p.set_ceilings({{FileKind::Script, entry, kCapRead, "Needed", "owner/needed", true, true},
                    {FileKind::Script, entry, kCapRead, "Needed", "owner/needed", true, true},
                    {FileKind::Script, entry, kCapRead, "blocked", "owner/blocked", true, true},
                    {FileKind::Script, helper, kCapRead, "Needed", "owner/needed", true, false},
                    {FileKind::Script, changed, 0, "Needed", "owner/needed", false, true}});
    auto r = parse_server_cfg("[addons]\nenforce = on\ndefault = deny\nrequired = owner/needed\nblock = owner/blocked\n");
    r.server_files = {{FileKind::Script, "scripts/needed/init.lua", shadow}};
    p.set(r);
    const auto scripts = p.required_entries(FileKind::Script);
    REQUIRE(scripts.size() == 1);
    CHECK(scripts[0].first == "needed");
    CHECK(scripts[0].second == entry); // the shadowing server file cannot satisfy a repository requirement
    CHECK(p.caps(FileKind::Script, entry, "needed") == 0); // denied requirements remain expected
    CHECK(p.required_file(FileKind::Script, entry));
    CHECK(p.required_file(FileKind::Script, helper)); // modules retain restrictions but are not autostarted
    CHECK(p.required_entries(FileKind::Plugin).empty());
    r.addons.enabled = false;
    p.set(r);
    CHECK(p.required_entries(FileKind::Script).empty());
    p.clear();
    CHECK(p.required_entries(FileKind::Plugin).empty());
}
