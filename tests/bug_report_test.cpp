// core/zip_write.h and app/bug_report.h: the archive read back, the size budget, the report's pieces.
#include <doctest/doctest.h>

#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <zlib.h>

#include "app/bug_report.h"
#include "core/zip_write.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

std::uint32_t le16(const std::vector<std::byte>& b, std::size_t at) {
    return std::to_integer<std::uint32_t>(b[at]) | std::to_integer<std::uint32_t>(b[at + 1]) << 8;
}
std::uint32_t le32(const std::vector<std::byte>& b, std::size_t at) { return le16(b, at) | le16(b, at + 2) << 16; }

// The archive's files by name, read through its central directory (each checked against its CRC).
std::map<std::string, std::string> unzip(const std::vector<std::byte>& zip) {
    std::map<std::string, std::string> out;
    REQUIRE(zip.size() >= 22);
    const std::size_t end = zip.size() - 22;
    REQUIRE(le32(zip, end) == 0x06054b50);
    const std::uint32_t count = le16(zip, end + 10);
    std::size_t at = le32(zip, end + 16);
    for (std::uint32_t i = 0; i < count; ++i) {
        REQUIRE(le32(zip, at) == 0x02014b50);
        const std::uint32_t method = le16(zip, at + 10), crc = le32(zip, at + 16), packed = le32(zip, at + 20),
                            size = le32(zip, at + 24), name_len = le16(zip, at + 28), offset = le32(zip, at + 42);
        std::string name(reinterpret_cast<const char*>(zip.data() + at + 46), name_len);
        REQUIRE(le32(zip, offset) == 0x04034b50);
        const std::size_t body = offset + 30 + le16(zip, offset + 26) + le16(zip, offset + 28);
        std::string data(size, '\0');
        if (method == 0) {
            REQUIRE(packed == size);
            std::memcpy(data.data(), zip.data() + body, size);
        } else {
            REQUIRE(method == 8);
            z_stream z{};
            REQUIRE(inflateInit2(&z, -15) == Z_OK);
            z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(zip.data() + body));
            z.avail_in = packed;
            z.next_out = reinterpret_cast<Bytef*>(data.data());
            z.avail_out = size;
            CHECK(inflate(&z, Z_FINISH) == Z_STREAM_END);
            inflateEnd(&z);
        }
        CHECK(crc32(0L, reinterpret_cast<const Bytef*>(data.data()), size) == crc);
        out[name] = data;
        at += 46 + name_len + le16(zip, at + 30) + le16(zip, at + 32);
    }
    return out;
}

std::vector<std::byte> read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), {});
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

void write_text(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string noise(std::size_t n) {
    std::mt19937 rng(7);
    std::string s(n, '\0');
    for (char& c : s) c = static_cast<char>(rng());
    return s;
}

std::tm some_day() {
    std::tm t{};
    t.tm_year = 126, t.tm_mon = 9, t.tm_mday = 29, t.tm_hour = 14, t.tm_min = 5, t.tm_sec = 9;
    return t;
}

} // namespace

TEST_CASE("zip: text deflated, noise stored, both read back") {
    ZipWriter zip;
    std::string text;
    for (int i = 0; i < 2000; ++i) text += "[info] the same log line again\n";
    const std::string random = noise(5000);
    REQUIRE(zip.add("logs/hailstone.log", text, some_day()));
    REQUIRE(zip.add("noise.bin", random, some_day()));
    REQUIRE(zip.add("empty.txt", std::string_view(), some_day()));
    const std::size_t predicted = zip.finished_size();
    const auto& bytes = zip.finish();
    CHECK(bytes.size() == predicted);
    CHECK(bytes.size() < text.size() / 4 + random.size() + 400); // the log packed
    auto files = unzip(bytes);
    REQUIRE(files.size() == 3);
    CHECK(files["logs/hailstone.log"] == text);
    CHECK(files["noise.bin"] == random);
    CHECK(files["empty.txt"].empty());
    CHECK_FALSE(zip.add("late.txt", "after finish", some_day()));
}

// The screenshot's bytes read at Send are what goes in: the file is the next window's to delete and write again
// while this report is still being written.
TEST_CASE("bug report: the screenshot taken at Send goes in, not the file as it is later") {
    test::ScratchDir tmp("bugreport-capture");
    const fs::path capture = tmp.path() / "bug-report-capture.png";
    write_text(capture, "the next window's picture");
    app::BugReport r;
    r.time = 1791115200;
    r.capture = capture;
    r.capture_png = "this report's picture";
    auto saved = app::save_bug_report_async(r, tmp.path() / "bug-reports").get();
    REQUIRE(saved);
    auto files = unzip(read_bytes(saved->zip));
    CHECK(files["screenshots/at-report.png"] == "this report's picture");
}

TEST_CASE("zip: add_within keeps the finished archive under the limit") {
    ZipWriter zip;
    const std::string a = noise(1000), b = noise(1000);
    REQUIRE(zip.add_within("a.bin", std::as_bytes(std::span(a.data(), a.size())), some_day(), 1500));
    CHECK_FALSE(zip.add_within("b.bin", std::as_bytes(std::span(b.data(), b.size())), some_day(), 1500));
    CHECK(zip.finish().size() <= 1500);
    CHECK(unzip(zip.finish()).size() == 1);
}

TEST_CASE("bug report: file names, settings scrubbed") {
    CHECK(app::bug_report_file_name(false, some_day()) == "hailstone-bug-20261029-140509.zip");
    CHECK(app::bug_report_file_name(true, some_day(), 2) == "hailstone-crash-20261029-140509-2.zip");
    CHECK(app::scrub_settings("[login]\nuser=bob\nPassword=hunter2\nsession_key = abc\nkeymap=1\n") ==
          "[login]\nuser=bob\nPassword= (removed)\nsession_key = (removed)\nkeymap=1\n");
    // A plugin's auth key and API keys go; hotkey and keymap settings stay.
    CHECK(app::scrub_settings("[NMS]\nauth_key = 1234\nApiKey=x\nprivate_key=y\nHotkeyBank=2\nKeyRepeat=1\n") ==
          "[NMS]\nauth_key = (removed)\nApiKey= (removed)\nprivate_key= (removed)\nHotkeyBank=2\nKeyRepeat=1\n");
}

TEST_CASE("bug report: the summary fits a Discord message") {
    app::BugReport r;
    r.category = "Graphics";
    r.text = "Trees flicker in Kithicor\n" + std::string(5000, 'x');
    r.facts = {{"Build", "Hailstone 0.1.0 (abc1234)"}, {"Zone", ""}};
    const std::string s = app::bug_report_summary(r, "hailstone-bug-1.zip");
    CHECK(s.size() <= 1900);
    CHECK(s.starts_with("**[Graphics] Trees flicker in Kithicor**\n"));
    CHECK(s.find("- **Build:** Hailstone 0.1.0 (abc1234)\n") != std::string::npos);
    CHECK(s.find("Zone") == std::string::npos); // empty facts left out
    CHECK(s.find("hailstone-bug-1.zip") != std::string::npos);
}

TEST_CASE("bug report: saved with its files, what does not fit named") {
    test::ScratchDir tmp("bugreport");
    const fs::path log = tmp.path() / "logs" / "hailstone.log", ini = tmp.path() / "hailstone.ini";
    write_text(log, "[info] line one\n[warn] line two\n");
    write_text(ini, "[login]\npassword=secret\nui.name=modern\n");
    write_text(tmp.path() / "capture.png", noise(50000)); // will not fit
    app::BugReport r;
    r.time = std::time(nullptr);
    r.category = "Other";
    r.flags = {"Can be repeated"};
    r.text = "The bank window closes by itself.";
    r.facts = {{"Character", "Bob, level 12 Human Warrior"}};
    r.logs = {log, tmp.path() / "logs" / "missing.log"};
    r.settings = {ini};
    r.capture = tmp.path() / "capture.png";
    auto saved = app::save_bug_report(r, tmp.path() / "bug-reports", 20000);
    REQUIRE(saved);
    CHECK(saved->zip.parent_path() == tmp.path() / "bug-reports");
    CHECK(saved->zip.filename().string().starts_with("hailstone-bug-"));
    CHECK(saved->size == fs::file_size(saved->zip));
    REQUIRE(saved->left_out == std::vector<std::string>{"screenshots/at-report.png"});
    auto files = unzip(read_bytes(saved->zip));
    CHECK(files.count("logs/hailstone.log") == 1);
    CHECK(files["settings/hailstone.ini"].find("secret") == std::string::npos);
    CHECK(files["summary.md"] == saved->summary);
    const std::string& report = files["report.txt"];
    CHECK(report.find("The bank window closes by itself.") != std::string::npos);
    CHECK(report.find("Character:   Bob, level 12 Human Warrior") != std::string::npos);
    CHECK(report.find("Left out to stay under the upload limit") != std::string::npos);
    CHECK(report.find("  screenshots/at-report.png") != std::string::npos);

    // Written again over the same file (a crash report's note).
    r.file = saved->zip;
    r.text = "with a note";
    auto again = app::save_bug_report(r, tmp.path() / "bug-reports", 20000);
    REQUIRE(again);
    CHECK(again->zip == saved->zip);
    CHECK(unzip(read_bytes(again->zip))["report.txt"].find("with a note") != std::string::npos);
}

TEST_CASE("bug report: account names and the home folder taken out") {
    const std::vector<std::string> accounts = {"bob", "bobby", ""};
    const fs::path home = "/home/alice";
    // A whole word only, any case; the longer name first.
    CHECK(app::redact_private("login as Bob, bobby and bobcat (box: bob)", accounts, home) ==
          "login as <account>, <account> and bobcat (box: <account>)");
    // The home folder in either separator; its name as a folder name elsewhere; not a longer folder name.
    CHECK(app::redact_private("/home/alice/hs and /home/alice and \\home\\alice\\x", {}, home) ==
          "~/hs and ~ and ~\\x");
    CHECK(app::redact_private("/run/media/alice/Games /home/alicex alice", {}, home) ==
          "/run/media/<user>/Games /home/alicex alice");
    CHECK(app::redact_private("/home/alice/x", {}, "/home/alice/") == "~/x");
    CHECK(app::redact_private("nothing here", {}, {}) == "nothing here");
    // All of them in one text, the account named like the home folder's owner.
    CHECK(app::redact_private("/home/alice/x alice ALICE /mnt/Alice/y bob", {"alice", "bob"}, home) ==
          "~/x <account> <account> /mnt/<user>/y <account>");

    test::ScratchDir tmp("bugreport-redact");
    const fs::path log = tmp.path() / "hailstone.log", ini = tmp.path() / "hailstone.ini",
                   crash = tmp.path() / "crash-1.txt", dump = tmp.path() / "crash-1.dmp";
    write_text(log, "[info] logging in as Bob\n[info] game folder /home/alice/EverQuest\n");
    write_text(ini, "[login]\nusername=bob\n");
    write_text(crash, "what: signal 11\nlog: /home/alice/hailstone/logs/hailstone.log\n");
    write_text(dump, "MDMP /home/alice bob");
    app::BugReport r;
    r.time = std::time(nullptr);
    r.text = "bob's bags vanish";
    r.facts = {{"Install", "/home/alice/hailstone"}};
    r.logs = {log};
    r.settings = {ini};
    r.crash_files = {crash, dump};
    r.accounts = {"bob"};
    r.home = home;
    auto saved = app::save_bug_report(r, tmp.path() / "bug-reports");
    REQUIRE(saved);
    auto files = unzip(read_bytes(saved->zip));
    CHECK(files["logs/hailstone.log"] == "[info] logging in as <account>\n[info] game folder ~/EverQuest\n");
    CHECK(files["settings/hailstone.ini"] == "[login]\nusername=<account>\n");
    CHECK(files["crash/crash-1.txt"] == "what: signal 11\nlog: ~/hailstone/logs/hailstone.log\n");
    CHECK(files["crash/crash-1.dmp"] == "MDMP /home/alice bob"); // binary: as it is
    for (const char* name : {"report.txt", "summary.md"}) {
        CHECK(files[name].find("bob") == std::string::npos);
        CHECK(files[name].find("alice") == std::string::npos);
    }
    CHECK(files["report.txt"].find("<account>'s bags vanish") != std::string::npos);
    CHECK(files["report.txt"].find("Install:     ~/hailstone") != std::string::npos);
}

TEST_CASE("bug report: chat lines' words taken out of the logs and the crash report") {
    CHECK(app::redact_chat("[info] chat: Soandso tells you, 'my secret'\n[info] zone in\n") ==
          "[info] chat: (chat taken out of the report)\n[info] zone in\n");
    CHECK(app::redact_chat("[info] chat: last line") == "[info] chat: (chat taken out of the report)");
    CHECK(app::redact_chat("[warn] chat: a\r\n") == "[warn] chat: (chat taken out of the report)\r\n");
    // Only the level's tag before it: a line that holds the words later stays.
    CHECK(app::redact_chat("[info] loaded [x] chat: y\n") == "[info] loaded [x] chat: y\n");
    CHECK(app::redact_chat("chat: no tag\n") == "chat: no tag\n");
    // A chat text with newlines in it: its other lines (no level's tag) go too, up to the next tagged line.
    CHECK(app::redact_chat("[info] chat: Soandso tells you, 'one\ntwo\r\nthree'\n[info] zone in\nfour\n") ==
          "[info] chat: (chat taken out of the report)\n[info] zone in\nfour\n");
    CHECK(app::redact_chat("what: signal 11\nlast log lines:\n[info] chat: a\nb") ==
          "what: signal 11\nlast log lines:\n[info] chat: (chat taken out of the report)\n");

    // A crash report's stack after its last log lines is never a chat line's continuation, even when the newest
    // log line was chat (redact_crash_report).
    CHECK(app::redact_crash_report("last log lines:\n[info] chat: a\n\nstack:\n/opt/hailstone(+0x1a) [0x2]\n"
                                   "/lib/libc.so.6(abort+0x12) [0x7f3a]\n") ==
          "last log lines:\n[info] chat: (chat taken out of the report)\n\nstack:\n/opt/hailstone(+0x1a) [0x2]\n"
          "/lib/libc.so.6(abort+0x12) [0x7f3a]\n");
    // With no stack (Windows, a build without backtrace), a chat line's continuation that reads "stack:" is still
    // chat, and so is what follows it.
    CHECK(app::redact_crash_report("last log lines:\n[info] chat: a\nstack:\nmeet at the docks\n") ==
          "last log lines:\n[info] chat: (chat taken out of the report)\n");
    CHECK(app::redact_crash_report("what: x\n[info] chat: a\nb\n") == "what: x\n[info] chat: (chat taken out of the report)\n");

    test::ScratchDir tmp("bugreport-chat");
    const fs::path log = tmp.path() / "hailstone.log", crash = tmp.path() / "crash-2.txt";
    write_text(log, "[info] chat: Soandso tells you, 'meet at the docks'\n[info] zoning\n");
    write_text(crash, "what: signal 11\n[info] chat: Guildie tells the guild, 'hi'\n\nstack:\n/opt/hailstone(+0x1a2b) [0x5]\n");
    app::BugReport r;
    r.time = 1791115200; // noon UTC, 2026-10-04
    r.logs = {log};
    r.crash_files = {crash};
    auto saved = app::save_bug_report_async(r, tmp.path() / "bug-reports").get();
    REQUIRE(saved);
    auto files = unzip(read_bytes(saved->zip));
    CHECK(files["logs/hailstone.log"] == "[info] chat: (chat taken out of the report)\n[info] zoning\n");
    CHECK(files["crash/crash-2.txt"] ==
          "what: signal 11\n[info] chat: (chat taken out of the report)\n\nstack:\n/opt/hailstone(+0x1a2b) [0x5]\n");
    CHECK(files["report.txt"].find("(2026-10-04 12:00:00 UTC)") != std::string::npos);
}

TEST_CASE("bug report: crash reports found once") {
    test::ScratchDir tmp("crashes");
    write_text(tmp.path() / "crash-20261029-100000.txt", "a");
    write_text(tmp.path() / "crash-20261028-100000.txt", "b");
    write_text(tmp.path() / "crash-20261028-100000.dmp", "dump");
    write_text(tmp.path() / "hailstone.log", "log");
    auto found = app::unreported_crashes(tmp.path());
    REQUIRE(found.size() == 2);
    CHECK(found[0].filename() == "crash-20261028-100000.txt");
    app::mark_crash_reported(found[0]);
    CHECK(fs::exists(tmp.path() / "crash-20261028-100000.reported.txt"));
    CHECK(fs::exists(tmp.path() / "crash-20261028-100000.reported.dmp"));
    found = app::unreported_crashes(tmp.path());
    REQUIRE(found.size() == 1);
    CHECK(found[0].filename() == "crash-20261029-100000.txt");
}
