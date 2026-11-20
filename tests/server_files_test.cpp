// net/server_files.h and net/server_files_packets.h: the server's client files, brought into a profile's folder.
#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "core/sha256.h"
#include "net/server_files.h"
#include "net/server_files_packets.h"

namespace fs = std::filesystem;
using namespace hailstone;
using net::ServerFileSync;

namespace {
struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / (std::string("hailstone_srvfiles_") + tag + "_" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The server script's side, in memory: pages of `page` entries, pieces as asked.
struct FakeServer {
    std::map<std::string, std::string> files;
    std::uint32_t list_id = 7;
    std::uint32_t page = 2;
    bool answer = true;
    bool damage = false; // flips a byte of each piece
    bool twice = false;  // sends each piece two times
    bool stray = false;  // also sends a piece at an offset nobody asked for
    std::set<std::size_t> drop; // piece requests (1, 2 ... in the order they come) left unanswered
    std::string never;          // a file whose pieces are never sent
    std::deque<net::AppPacket> replies;
    std::size_t pieces = 0;
    std::size_t requests = 0;

    std::vector<net::ServerFileEntry> list() const {
        std::vector<net::ServerFileEntry> out;
        for (const auto& [path, data] : files)
            out.push_back({static_cast<std::uint32_t>(data.size()), sha256(data), path});
        return out;
    }

    void receive(const net::AppPacket& p) {
        if (!answer) return;
        REQUIRE(p.opcode == net::kOpHsFilesRequest);
        auto u16 = [&](std::size_t at) { return std::to_integer<std::uint16_t>(p.data[at]) | std::to_integer<std::uint16_t>(p.data[at + 1]) << 8; };
        auto u32 = [&](std::size_t at) { return std::uint32_t(u16(at)) | std::uint32_t(u16(at + 2)) << 16; };
        auto all = list();
        if (u16(6) == net::kServerFilesList) {
            net::ServerFilesPage v;
            v.list_id = list_id;
            v.total = static_cast<std::uint32_t>(all.size());
            v.first = u32(8);
            for (std::uint32_t i = v.first; i < all.size() && i < v.first + page; ++i) v.entries.push_back(all[i]);
            replies.push_back(net::encode(v));
            return;
        }
        std::uint32_t id = u32(8), index = u32(12), offset = u32(16), length = u32(20);
        if (id != list_id) {
            replies.push_back(net::encode(net::ServerFilesError{id, index, net::kServerFilesStale}));
            return;
        }
        ++requests;
        if (drop.contains(requests) || all.at(index).path == never) return;
        net::ServerFilesChunk c{id, index, offset, {}};
        const std::string& data = files.at(all.at(index).path);
        for (std::uint32_t i = offset; i < offset + length && i < data.size(); ++i)
            c.data.push_back(static_cast<std::byte>(data[i]));
        if (damage && !c.data.empty()) c.data[0] ^= std::byte{1};
        ++pieces;
        replies.push_back(net::encode(c));
        if (twice) replies.push_back(net::encode(c));
        if (stray && offset == 0 && data.size() > 1) {
            net::ServerFilesChunk odd{id, index, 1, {}};
            for (std::uint32_t i = 1; i < 1 + length && i < data.size(); ++i)
                odd.data.push_back(static_cast<std::byte>(data[i]));
            replies.push_front(net::encode(odd));
        }
    }
};

// Runs a sync against the fake server until it finishes.
ServerFileSync::State run(const fs::path& profile, FakeServer& server, bool* changed = nullptr,
                          ServerFileSync::Limits limits = {}, std::string* error = nullptr) {
    ServerFileSync sync(profile, [&](net::AppPacket p) { server.receive(p); }, limits);
    auto now = ServerFileSync::Clock::now();
    sync.start(now);
    for (int steps = 0; !sync.finished() && steps < 100000; ++steps) {
        if (server.replies.empty()) {
            now += std::chrono::seconds(10);
            sync.update(now);
            continue;
        }
        auto p = server.replies.front();
        server.replies.pop_front();
        auto reply = net::decode_server_files(p);
        REQUIRE(reply);
        sync.handle(*reply, now);
    }
    if (changed) *changed = sync.changed();
    if (error) *error = sync.error();
    return sync.state();
}
} // namespace

TEST_CASE("server files: packets round trip") {
    net::ServerFilesPage page{3, 5, 1, {{4, sha256("abcd"), "Resources/SkillCaps.txt"}}};
    auto p = net::encode(page);
    CHECK(p.opcode == 0x1435);
    auto d = net::decode_server_files(p);
    REQUIRE(d);
    const auto& got = std::get<net::ServerFilesPage>(*d);
    CHECK(got.list_id == 3);
    CHECK(got.total == 5);
    CHECK(got.first == 1);
    REQUIRE(got.entries.size() == 1);
    CHECK(got.entries[0].path == "Resources/SkillCaps.txt");
    CHECK(got.entries[0].sha256 == sha256("abcd"));

    auto req = net::encode_server_files_chunk_request(3, 2, 65536, 32768);
    CHECK(req.opcode == 0x1434);
    CHECK(req.data.size() == 24);
    CHECK(req.data[0] == std::byte{'H'});
    CHECK(req.data[3] == std::byte{'L'});

    // A page that claims more entries than its total, and a piece longer than the most, are bad packets.
    page.total = 0;
    CHECK_FALSE(net::decode_server_files(net::encode(page)));
    net::ServerFilesChunk big{1, 0, 0, std::vector<std::byte>(net::kServerFilesChunkMax + 1)};
    CHECK_FALSE(net::decode_server_files(net::encode(big)));
}

TEST_CASE("server files: safe paths") {
    CHECK(net::is_safe_server_file_path("spells_us.txt"));
    CHECK(net::is_safe_server_file_path("Resources/SkillCaps.txt"));
    CHECK(net::is_safe_server_file_path("sounds/x y.wav"));
    CHECK_FALSE(net::is_safe_server_file_path(""));
    CHECK_FALSE(net::is_safe_server_file_path("/etc/passwd"));
    CHECK_FALSE(net::is_safe_server_file_path("../hailstone.ini"));
    CHECK_FALSE(net::is_safe_server_file_path("a/../../b"));
    CHECK_FALSE(net::is_safe_server_file_path("a//b"));
    CHECK_FALSE(net::is_safe_server_file_path("C:/x.txt"));
    CHECK_FALSE(net::is_safe_server_file_path("a\\b.txt"));
    CHECK_FALSE(net::is_safe_server_file_path(".hidden"));
    CHECK_FALSE(net::is_safe_server_file_path("dinput8.dll"));
    CHECK_FALSE(net::is_safe_server_file_path("plugins/x.SO"));
    CHECK_FALSE(net::is_safe_server_file_path("scripts/autorun.lua"));
    // Windows drops a trailing dot or space: "evil.exe." would land as evil.exe.
    CHECK_FALSE(net::is_safe_server_file_path("assets/evil.exe."));
    CHECK_FALSE(net::is_safe_server_file_path("evil.exe "));
    CHECK_FALSE(net::is_safe_server_file_path("dir./x.txt"));
    // Windows device names, in any case and with any extension, refused everywhere.
    for (const char* bad : {"CON", "con.txt", "a/Nul.dds", "prn", "AUX.wav", "com1", "COM9.txt", "lpt1", "Lpt9.x/y.txt",
                            "con .txt", "conin$", "CONOUT$.txt", "com\xc2\xb9.txt", "lpt\xc2\xb2"})
        CHECK_MESSAGE(!net::is_safe_server_file_path(bad), bad);
    for (const char* ok : {"console.txt", "com0.txt", "com10", "lpt.txt", "nul_x.txt", "a/aux2/x.txt", "xcon.txt"})
        CHECK_MESSAGE(net::is_safe_server_file_path(ok), ok);
    CHECK_FALSE(net::server_file_destination("plugins/con.ini"));
}

TEST_CASE("server files: a piece that comes twice or unasked is not counted") {
    TempDir dir("twice");
    FakeServer server;
    server.files["spells_us.txt"] = std::string(100000, 's'); // four pieces
    server.files["b.txt"] = "bee";
    server.twice = true;
    server.stray = true;
    CHECK(run(dir.path, server) == ServerFileSync::State::Done);
    CHECK(read(dir.path / "assets" / "spells_us.txt") == server.files["spells_us.txt"]);
    CHECK(read(dir.path / "assets" / "b.txt") == "bee");
}

TEST_CASE("server files: a piece the server skipped is asked for again, at once") {
    TempDir dir("skipped");
    FakeServer server;
    server.files["a.bin"] = std::string(300000, 'a'); // ten pieces
    server.files["b.bin"] = std::string(70000, 'b');
    server.drop = {2, 5, 6}; // an older script over its limit: later requests are answered, these are not
    ServerFileSync sync(dir.path, [&](net::AppPacket p) { server.receive(p); });
    auto now = ServerFileSync::Clock::now();
    sync.start(now);
    while (!sync.finished() && !server.replies.empty()) {
        auto p = server.replies.front();
        server.replies.pop_front();
        sync.handle(*net::decode_server_files(p), now); // no time passes: no timeout is involved
    }
    // a.bin came out of order: update() hashes it from .download/.
    for (int calls = 0; !sync.finished() && calls < 100; ++calls) sync.update(now);
    CHECK(sync.state() == ServerFileSync::State::Done);
    CHECK(server.requests == server.pieces + 3);
    CHECK(read(dir.path / "assets" / "a.bin") == server.files["a.bin"]);
    CHECK(read(dir.path / "assets" / "b.bin") == server.files["b.bin"]);
}

TEST_CASE("server files: the last pieces lost are asked for again after a silence") {
    TempDir dir("tail");
    FakeServer server;
    server.files["a.bin"] = std::string(100000, 'a'); // four pieces, all asked at once
    server.drop = {3, 4};                             // nothing asked after them to show the loss
    ServerFileSync sync(dir.path, [&](net::AppPacket p) { server.receive(p); });
    auto now = ServerFileSync::Clock::now();
    sync.start(now);
    auto drain = [&] {
        while (!sync.finished() && !server.replies.empty()) {
            auto p = server.replies.front();
            server.replies.pop_front();
            sync.handle(*net::decode_server_files(p), now);
        }
    };
    drain();
    CHECK(sync.state() == ServerFileSync::State::Downloading);
    CHECK(server.requests == 4);
    now += ServerFileSync::kChunkRetry - std::chrono::seconds(1);
    sync.update(now);
    CHECK(server.requests == 4); // not yet
    now += std::chrono::seconds(1);
    sync.update(now);
    CHECK(server.requests == 6);
    drain();
    CHECK(sync.state() == ServerFileSync::State::Done);
    CHECK(read(dir.path / "assets" / "a.bin") == server.files["a.bin"]);
}

TEST_CASE("server files: a piece that never comes is asked kChunkRetries times, then the sync waits out kChunkTimeout") {
    TempDir dir("never");
    FakeServer server;
    server.files["a.bin"] = "never sent";
    server.never = "a.bin";
    ServerFileSync sync(dir.path, [&](net::AppPacket p) { server.receive(p); });
    auto now = ServerFileSync::Clock::now();
    sync.start(now);
    std::optional<ServerFileSync::Clock::time_point> downloading_since;
    for (int steps = 0; !sync.finished() && steps < 100; ++steps) {
        while (!server.replies.empty()) {
            auto p = server.replies.front();
            server.replies.pop_front();
            sync.handle(*net::decode_server_files(p), now);
        }
        if (!downloading_since && sync.state() == ServerFileSync::State::Downloading) downloading_since = now;
        now += ServerFileSync::kChunkRetry;
        sync.update(now);
    }
    CHECK(sync.state() == ServerFileSync::State::Failed);
    CHECK(server.requests == static_cast<std::size_t>(ServerFileSync::kChunkRetries));
    // The retries (one every kChunkRetry) did not end it early: the silence did, after kChunkTimeout.
    REQUIRE(downloading_since);
    CHECK(now - *downloading_since > ServerFileSync::kChunkTimeout);
    CHECK(now - *downloading_since <= ServerFileSync::kChunkTimeout + ServerFileSync::kChunkRetry);
    CHECK(sync.error() == "the server stopped sending its files");
}

TEST_CASE("server files: a piece skipped kChunkRetries times while others come fails the sync") {
    TempDir dir("skipped_always");
    FakeServer server;
    server.files["a.bin"] = "never sent";
    server.never = "a.bin";
    server.files["b.bin"] = std::string(8000000, 'b'); // enough pieces asked after each request for a.bin
    ServerFileSync sync(dir.path, [&](net::AppPacket p) { server.receive(p); });
    auto now = ServerFileSync::Clock::now();
    sync.start(now);
    while (!sync.finished() && !server.replies.empty()) {
        auto p = server.replies.front();
        server.replies.pop_front();
        sync.handle(*net::decode_server_files(p), now); // no time passes: only the skips count
    }
    CHECK(sync.state() == ServerFileSync::State::Failed);
    CHECK(sync.error().find("did not come after 5 requests") != std::string::npos);
}

TEST_CASE("server files: download, keep, replace, delete") {
    TempDir dir("sync");
    fs::path assets = dir.path / "assets";
    FakeServer server;
    server.files["spells_us.txt"] = std::string(100000, 's'); // several pieces
    server.files["Resources/SkillCaps.txt"] = "caps";
    server.files["empty.txt"] = "";
    write(assets / "mine.txt", "the player's own"); // not the server's: kept

    bool changed = false;
    CHECK(run(dir.path, server, &changed) == ServerFileSync::State::Done);
    CHECK(changed);
    CHECK(read(assets / "spells_us.txt") == server.files["spells_us.txt"]);
    CHECK(read(assets / "Resources/SkillCaps.txt") == "caps");
    CHECK(fs::exists(assets / "empty.txt"));
    CHECK(read(assets / "mine.txt") == "the player's own");
    CHECK_FALSE(fs::exists(dir.path / ".download"));
    CHECK(fs::exists(dir.path / "server_files.txt"));

    // Nothing changed: nothing comes.
    server.pieces = 0;
    CHECK(run(dir.path, server, &changed) == ServerFileSync::State::Done);
    CHECK_FALSE(changed);
    CHECK(server.pieces == 0);

    // One file changed, one gone: the changed one comes, the gone one is deleted, the player's stays.
    server.files["Resources/SkillCaps.txt"] = "new caps";
    server.files.erase("empty.txt");
    server.list_id = 8;
    CHECK(run(dir.path, server, &changed) == ServerFileSync::State::Done);
    CHECK(changed);
    CHECK(server.pieces == 1);
    CHECK(read(assets / "Resources/SkillCaps.txt") == "new caps");
    CHECK_FALSE(fs::exists(assets / "empty.txt"));
    CHECK(fs::exists(assets / "mine.txt"));

    // The server has none any more: what it brought goes.
    server.files.clear();
    CHECK(run(dir.path, server, &changed) == ServerFileSync::State::Done);
    CHECK_FALSE(fs::exists(assets / "spells_us.txt"));
    CHECK(fs::exists(assets / "mine.txt"));
}

TEST_CASE("server files: a file over the size limit is refused; a list over the total limit writes nothing") {
    TempDir dir("limits");
    fs::path assets = dir.path / "assets";
    FakeServer server;
    server.files["big.txt"] = std::string(2000, 'b');
    server.files["small.txt"] = "small";
    ServerFileSync::Limits limits;
    limits.max_file = 1000;
    CHECK(run(dir.path, server, nullptr, limits) == ServerFileSync::State::Done);
    CHECK_FALSE(fs::exists(assets / "big.txt"));
    CHECK(read(assets / "small.txt") == "small");

    // Three files of 600 bytes, at most 1000 in all: nothing asked for, nothing in .download/, the old files stay.
    server.files.clear();
    server.files["a.txt"] = std::string(600, 'a');
    server.files["b.txt"] = std::string(600, 'b');
    server.files["small.txt"] = std::string(600, 'c');
    server.list_id = 8;
    server.pieces = 0;
    limits.max_total = 1000;
    std::string error;
    CHECK(run(dir.path, server, nullptr, limits, &error) == ServerFileSync::State::Failed);
    CHECK(error.find("more than the limit") != std::string::npos);
    CHECK(server.pieces == 0);
    CHECK_FALSE(fs::exists(dir.path / ".download"));
    CHECK(read(assets / "small.txt") == "small");
}

TEST_CASE("server files: a file the player put there by hand is taken as is when it matches") {
    TempDir dir("adopt");
    FakeServer server;
    server.files["spells_us.txt"] = "spells";
    write(dir.path / "assets" / "SPELLS_US.TXT", "spells");
    bool changed = true;
    CHECK(run(dir.path, server, &changed) == ServerFileSync::State::Done);
    CHECK_FALSE(changed);
    CHECK(server.pieces == 0);
}

TEST_CASE("server files: damaged pieces change nothing; no answer; unsafe names refused") {
    TempDir dir("damage");
    fs::path assets = dir.path / "assets";
    write(assets / "spells_us.txt", "old");
    FakeServer server;
    server.files["spells_us.txt"] = "new spells";
    server.damage = true;
    CHECK(run(dir.path, server) == ServerFileSync::State::Failed);
    CHECK(read(assets / "spells_us.txt") == "old");

    server.answer = false;
    CHECK(run(dir.path, server) == ServerFileSync::State::NoAnswer);

    server.answer = true;
    server.damage = false;
    server.files.clear();
    server.files["../escape.txt"] = "x";
    server.files["dinput8.dll"] = "x";
    server.files["ok.txt"] = "fine";
    CHECK(run(dir.path, server) == ServerFileSync::State::Done);
    CHECK(read(assets / "ok.txt") == "fine");
    CHECK_FALSE(fs::exists(dir.path / "escape.txt"));
    CHECK_FALSE(fs::exists(assets / "dinput8.dll"));
}

TEST_CASE("server files: where each file goes; scripts and this platform's plugins only in their folders") {
    using net::server_file_destination;
    const std::string plat(net::plugin_platform());
#if defined(_WIN32)
    const std::string lib = ".dll", other_lib = ".so";
#elif defined(__APPLE__)
    const std::string lib = ".dylib", other_lib = ".so";
#else
    const std::string lib = ".so", other_lib = ".dll";
#endif
    CHECK(server_file_destination("hailstone.cfg") == "hailstone.cfg");
    CHECK(server_file_destination("spells_us.txt") == "assets/spells_us.txt");
    CHECK(server_file_destination("Resources/SkillCaps.txt") == "assets/Resources/SkillCaps.txt");
    CHECK(server_file_destination("assets/commons.s3d") == "assets/commons.s3d");
    CHECK(server_file_destination("scripts/hud/init.lua") == "scripts/hud/init.lua");
    CHECK(server_file_destination("scripts/hud/icons.json") == "scripts/hud/icons.json");
    CHECK_FALSE(server_file_destination("scripts/setup.sh"));
    CHECK_FALSE(server_file_destination("assets/x.lua"));
    CHECK_FALSE(server_file_destination("autorun.lua"));
    CHECK(server_file_destination("plugins/MQ2Hud" + lib) == "plugins/MQ2Hud" + lib);
    CHECK(server_file_destination("plugins/" + plat + "/MQ2Hud/MQ2Hud" + lib) == "plugins/MQ2Hud/MQ2Hud" + lib);
    CHECK(server_file_destination("plugins/MQ2Hud/MQ2Hud.ini") == "plugins/MQ2Hud/MQ2Hud.ini");
    CHECK_FALSE(server_file_destination("plugins/setup.exe"));
    // A plugin's Lua windows (plugins/<name>/ui/...), and no Lua elsewhere in plugins/.
    CHECK(server_file_destination("plugins/nms/ui/modern/nmsloot.lua") == "plugins/nms/ui/modern/nmsloot.lua");
    CHECK(server_file_destination("plugins/" + plat + "/nms/ui/gamepad/lib/x.lua") == "plugins/nms/ui/gamepad/lib/x.lua");
    CHECK_FALSE(server_file_destination("plugins/nms/init.lua"));
    CHECK_FALSE(server_file_destination("plugins/ui/x.lua"));
    CHECK_FALSE(server_file_destination("plugins/nms/data/ui/x.lua"));
    bool other = false;
    CHECK_FALSE(server_file_destination("plugins/MQ2Hud" + other_lib, &other));
    CHECK(other);
    std::string other_plat = plat == "linux-x64" ? "windows-x64" : "linux-x64";
    CHECK_FALSE(server_file_destination("plugins/" + other_plat + "/MQ2Hud" + lib, &other));
    CHECK(other);
    CHECK_FALSE(server_file_destination("plugins/x.exe", &other));
    CHECK_FALSE(other);
    CHECK_FALSE(server_file_destination("../hailstone.cfg"));
}

TEST_CASE("server files: hailstone.cfg, scripts and plugins land beside assets/; only assets count as changed") {
    TempDir dir("kinds");
    FakeServer server;
    server.files["hailstone.cfg"] = "[scripts]\nplayer = off\n";
    server.files["scripts/hud.lua"] = "print('hud')";
    server.files["plugins/" + std::string(net::plugin_platform()) + "/MQ2Hud/MQ2Hud.ini"] = "[x]";
    server.files["assets/commons.s3d"] = "zone";
    bool changed = false;
    REQUIRE(run(dir.path, server, &changed) == ServerFileSync::State::Done);
    CHECK(changed);
    CHECK(read(dir.path / "hailstone.cfg") == server.files["hailstone.cfg"]);
    CHECK(read(dir.path / "scripts/hud.lua") == "print('hud')");
    CHECK(read(dir.path / "plugins/MQ2Hud/MQ2Hud.ini") == "[x]");
    CHECK(read(dir.path / "assets/commons.s3d") == "zone");

    ServerFileSync sync(dir.path, {});
    auto installed = sync.installed();
    CHECK(installed.size() == 4);

    server.files["scripts/hud.lua"] = "print('hud 2')";
    server.list_id = 9;
    REQUIRE(run(dir.path, server, &changed) == ServerFileSync::State::Done);
    CHECK_FALSE(changed); // a script: no game files to read again
    CHECK(read(dir.path / "scripts/hud.lua") == "print('hud 2')");

    server.files.erase("hailstone.cfg");
    REQUIRE(run(dir.path, server) == ServerFileSync::State::Done);
    CHECK_FALSE(fs::exists(dir.path / "hailstone.cfg"));
}

TEST_CASE("server files: a record from before scripts and plugins is read as assets/") {
    TempDir dir("record1");
    write(dir.path / "assets" / "spells_us.txt", "spells");
    write(dir.path / "server_files.txt", "# The files Hailstone downloaded from this server into assets/: SHA-256, size, path.\n" +
                                             to_hex(sha256(std::string_view("spells"))) + " 6 spells_us.txt\n");
    FakeServer server;
    server.files["spells_us.txt"] = "spells";
    bool changed = true;
    CHECK(run(dir.path, server, &changed) == ServerFileSync::State::Done);
    CHECK_FALSE(changed);
    CHECK(server.pieces == 0);
    CHECK(read(dir.path / "server_files.txt").find(" assets/spells_us.txt") != std::string::npos);
}

TEST_CASE("server files: the last login's rules, provisional, for before the zone-in") {
    TempDir dir("cached");
    plugin::Rules none = net::cached_server_rules(dir.path); // a server never visited
    CHECK(none.provisional);
    CHECK_FALSE(none.configured);
    CHECK(net::cached_server_rules({}).provisional); // a login without a server folder

    FakeServer server;
    server.files["hailstone.cfg"] = "[plugins]\nplayer = on\nplayer_caps = read,login\n";
    server.files["plugins/ext/settings.txt"] = "x";
    REQUIRE(run(dir.path, server) == ServerFileSync::State::Done);
    plugin::Rules r = net::cached_server_rules(dir.path);
    CHECK(r.provisional);
    CHECK(r.configured);
    CHECK(r.player_plugin_caps == (plugin::kCapRead | plugin::kCapLogin));
    REQUIRE(r.server_files.size() == 1);
    CHECK(r.server_files[0].kind == plugin::FileKind::Plugin);
    CHECK(r.dir == dir.path);
}

TEST_CASE("server files: [assets] player = off keeps the server's files and textures of the assets folder") {
    TempDir dir("assetrule");
    FakeServer server;
    server.files["spells_us.txt"] = "spells";
    REQUIRE(run(dir.path, server) == ServerFileSync::State::Done);
    CHECK_FALSE(net::server_asset_only(dir.path)); // no hailstone.cfg

    server.files["hailstone.cfg"] = "[assets]\nplayer = off\n";
    REQUIRE(run(dir.path, server) == ServerFileSync::State::Done);
    auto only = net::server_asset_only(dir.path);
    REQUIRE(only);
    CHECK(only->contains("spells_us.txt"));
    CHECK(net::asset_counts(*only, "spells_us.txt"));
    CHECK_FALSE(net::asset_counts(*only, "commons.s3d"));
    CHECK(net::asset_counts(*only, "uifiles/default/window.dds"));

    server.files["hailstone.cfg"] = "[assets]\nplayer = on\n";
    REQUIRE(run(dir.path, server) == ServerFileSync::State::Done);
    CHECK_FALSE(net::server_asset_only(dir.path));
}

TEST_CASE("server files: files the record does not know are hashed a budget at a time, not all in one call") {
    // A profile whose server_files.txt is gone but whose files are there (copied in by hand): every one is hashed
    // before the download is planned. That happens over several update() calls so the frame goes on.
    TempDir dir("verify");
    FakeServer server;
    server.page = 10;
    server.files["a.bin"] = std::string(5000, 'a');
    server.files["b.bin"] = std::string(3000, 'b');
    server.files["c.bin"] = std::string(4000, 'c'); // not there yet: downloaded
    write(dir.path / "assets" / "a.bin", server.files["a.bin"]);
    write(dir.path / "assets" / "b.bin", std::string(3000, 'x')); // the right size, other bytes: downloaded again
    write(dir.path / ".download" / "assets" / "c.bin", server.files["c.bin"]); // came complete in an earlier try
    ServerFileSync sync(dir.path, [&](net::AppPacket p) { server.receive(p); });
    sync.set_hash_budget(1000);
    auto now = ServerFileSync::Clock::now();
    sync.start(now);
    while (sync.state() == ServerFileSync::State::Listing && !server.replies.empty()) {
        auto p = server.replies.front();
        server.replies.pop_front();
        sync.handle(*net::decode_server_files(p), now);
    }
    REQUIRE(sync.state() == ServerFileSync::State::Verifying); // 1000 bytes of 12000 hashed in the last page's call
    CHECK(server.requests == 0);
    int calls = 0;
    while (sync.state() == ServerFileSync::State::Verifying && calls < 100) {
        now += std::chrono::seconds(10); // no list timeout while hashing
        sync.update(now);
        ++calls;
    }
    CHECK(calls >= 11);
    REQUIRE(sync.state() == ServerFileSync::State::Downloading);
    CHECK(sync.files_needed() == 2); // b.bin, and c.bin already in .download/
    for (int steps = 0; !sync.finished() && steps < 1000; ++steps) {
        if (server.replies.empty()) {
            sync.update(now);
            continue;
        }
        auto p = server.replies.front();
        server.replies.pop_front();
        sync.handle(*net::decode_server_files(p), now);
    }
    CHECK(sync.state() == ServerFileSync::State::Done);
    CHECK(server.requests == 1); // b.bin's one piece; a.bin matched, c.bin came before
    CHECK(read(dir.path / "assets" / "a.bin") == server.files["a.bin"]);
    CHECK(read(dir.path / "assets" / "b.bin") == server.files["b.bin"]);
    CHECK(read(dir.path / "assets" / "c.bin") == server.files["c.bin"]);
    CHECK(read(dir.path / "server_files.txt").find("a.bin") != std::string::npos); // the player's copy recorded
}

TEST_CASE("server files: a file whose pieces came out of order is hashed from disk a budget at a time") {
    // A piece asked for again comes after later ones, so the file cannot be hashed as it comes: it is read back from
    // .download/ over several calls instead of all at once (a large file would stop the frame).
    TempDir dir("rehash");
    FakeServer server;
    server.files["a.bin"] = std::string(100000, 'a'); // four pieces
    server.drop = {1};                                // the first: asked again after the others came
    ServerFileSync sync(dir.path, [&](net::AppPacket p) { server.receive(p); });
    sync.set_hash_budget(1000);
    auto now = ServerFileSync::Clock::now();
    sync.start(now);
    for (int steps = 0; !server.replies.empty() && steps < 1000; ++steps) {
        auto p = server.replies.front();
        server.replies.pop_front();
        sync.handle(*net::decode_server_files(p), now);
    }
    CHECK(server.requests == 5);
    REQUIRE(sync.state() == ServerFileSync::State::Downloading); // all came; 1000 of 100000 bytes checked
    int calls = 0;
    while (sync.state() == ServerFileSync::State::Downloading && calls < 1000) {
        now += std::chrono::seconds(10); // no chunk timeout while hashing: nothing is asked for
        sync.update(now);
        ++calls;
    }
    CHECK(calls >= 99);
    CHECK(sync.state() == ServerFileSync::State::Done);
    CHECK(read(dir.path / "assets" / "a.bin") == server.files["a.bin"]);
}

TEST_CASE("server files: a file whose pieces came out of order and does not match its hash fails the sync") {
    TempDir dir("rehash_bad");
    FakeServer server;
    server.files["a.bin"] = std::string(100000, 'a');
    server.drop = {1};
    ServerFileSync sync(dir.path, [&](net::AppPacket p) { server.receive(p); });
    auto now = ServerFileSync::Clock::now();
    sync.start(now);
    bool spoiled = false;
    for (int steps = 0; !sync.finished() && steps < 1000; ++steps) {
        if (server.replies.empty()) {
            now += std::chrono::seconds(1);
            sync.update(now);
            continue;
        }
        auto p = server.replies.front();
        server.replies.pop_front();
        auto reply = net::decode_server_files(p);
        REQUIRE(reply);
        if (auto* c = std::get_if<net::ServerFilesChunk>(&*reply); c && c->offset == 0 && !spoiled) {
            c->data[5] ^= std::byte{1}; // the piece asked for again, damaged
            spoiled = true;
        }
        sync.handle(*reply, now);
    }
    CHECK(spoiled);
    CHECK(sync.state() == ServerFileSync::State::Failed);
    CHECK(sync.error().find("arrived damaged") != std::string::npos);
    CHECK_FALSE(fs::exists(dir.path / ".download" / "assets" / "a.bin"));
}

TEST_CASE("server files: chunks of other files handled in one frame do not hash an out-of-order file") {
    // The frame drains every chunk that came: hashing a budget's worth on each would hash a large file in one frame.
    // The file that came out of order is hashed by update() alone, one budget a call.
    TempDir dir("rehash_frame");
    FakeServer server;
    server.files["a.bin"] = std::string(100000, 'a');                          // four pieces, the first asked again
    server.files["b.bin"] = std::string(64 * net::kServerFilesChunkMax, 'b'); // still coming after a.bin is all here
    server.drop = {1};
    ServerFileSync sync(dir.path, [&](net::AppPacket p) { server.receive(p); });
    sync.set_hash_budget(1000);
    auto now = ServerFileSync::Clock::now();
    sync.start(now);
    for (int steps = 0; !server.replies.empty() && steps < 1000; ++steps) {
        auto p = server.replies.front();
        server.replies.pop_front();
        sync.handle(*net::decode_server_files(p), now);
    }
    REQUIRE(sync.state() == ServerFileSync::State::Downloading); // b.bin came in order; a.bin is not hashed yet
    int calls = 0;
    while (sync.state() == ServerFileSync::State::Downloading && calls < 1000) {
        now += std::chrono::seconds(10);
        sync.update(now);
        ++calls;
    }
    CHECK(calls >= 100); // all of a.bin's 100000 bytes, 1000 a call
    CHECK(sync.state() == ServerFileSync::State::Done);
    CHECK(read(dir.path / "assets" / "a.bin") == server.files["a.bin"]);
    CHECK(read(dir.path / "assets" / "b.bin") == server.files["b.bin"]);
}

TEST_CASE("server files: the stale errors for every piece in flight list the files again once") {
    // The server's files changed with a window of pieces asked for: it answers each of those requests with a stale
    // error. The first lists again; the others are for the old list and are left, so the download still finishes.
    TempDir dir("stale_window");
    FakeServer server;
    server.files["a.bin"] = std::string(64 * net::kServerFilesChunkMax, 'a');
    ServerFileSync sync(dir.path, [&](net::AppPacket p) { server.receive(p); });
    auto now = ServerFileSync::Clock::now();
    sync.start(now);
    while (sync.state() == ServerFileSync::State::Listing && !server.replies.empty()) {
        auto p = server.replies.front();
        server.replies.pop_front();
        sync.handle(*net::decode_server_files(p), now);
    }
    REQUIRE(sync.state() == ServerFileSync::State::Downloading);
    const std::size_t asked = server.replies.size();
    REQUIRE(asked >= 4);
    server.replies.clear(); // the pieces were not sent: the list changed first
    const std::uint32_t old_id = server.list_id;
    server.list_id = old_id + 1;
    for (std::size_t i = 0; i < asked; ++i)
        server.replies.push_back(net::encode(net::ServerFilesError{old_id, 0, net::kServerFilesStale}));
    for (int steps = 0; !sync.finished() && steps < 10000; ++steps) {
        if (server.replies.empty()) {
            now += std::chrono::seconds(1);
            sync.update(now);
            continue;
        }
        auto p = server.replies.front();
        server.replies.pop_front();
        sync.handle(*net::decode_server_files(p), now);
    }
    CHECK(sync.state() == ServerFileSync::State::Done);
    CHECK(read(dir.path / "assets" / "a.bin") == server.files["a.bin"]);
}
