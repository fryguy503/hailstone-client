// Add-ons (docs/addons.md): the GitHub links a player pastes (core/addon_source.h), the manifest
// (core/addon_manifest.h) with its signature, and the library header check (core/binary_header.h). Malformed and
// random input is refused without a crash; libFuzzer runs the same parsers (tests/fuzz_libfuzzer.cpp, HAILSTONE_FUZZ_ZIP).
// The capability ceiling the manifest puts on an add-on's files is in tests/plugin_policy_test.cpp.

#include <doctest/doctest.h>

#include <array>
#include <random>
#include <string>
#include <vector>

#include "core/addon_manifest.h"
#include "core/addon_source.h"
#include "core/binary_header.h"
#include "core/ed25519.h"
#include "core/sha256.h"

using namespace hailstone;

namespace {

const std::string kHash = to_hex(sha256(std::string_view("package")));

std::string script_manifest(const std::string& extra = {}) {
    return "hailstone-addon 1\n"
           "name autoloot\n"
           "kind script\n"
           "version 1.3.0\n"
           "author Some One\n"
           "description Loots what you tell it to.\n"
           "caps read, events,actions\n"
           "package any 41233 " + kHash + " autoloot-1.3.0.zip\n" + extra;
}

struct Keys {
    Ed25519SecretKey secret{};
    Ed25519PublicKey pub{};
    explicit Keys(std::uint8_t seed_byte = 3) {
        std::array<std::uint8_t, 32> seed{};
        seed.fill(seed_byte);
        ed25519_key_pair(seed, secret, pub);
    }
    std::string sign(std::string body) const {
        body += "key " + hex_encode(pub) + "\n";
        return body + "signature " + hex_encode(ed25519_sign(secret, body)) + "\n";
    }
};

std::vector<std::byte> bytes(std::initializer_list<unsigned> list, std::size_t size = 0) {
    std::vector<std::byte> out;
    for (unsigned v : list) out.push_back(static_cast<std::byte>(v));
    if (out.size() < size) out.resize(size);
    return out;
}

void put16(std::vector<std::byte>& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::byte>(v & 0xff);
    b[at + 1] = static_cast<std::byte>(v >> 8);
}
void put32(std::vector<std::byte>& b, std::size_t at, std::uint32_t v, bool big = false) {
    for (std::size_t i = 0; i < 4; ++i) b[at + (big ? 3 - i : i)] = static_cast<std::byte>((v >> (8 * i)) & 0xff);
}

std::vector<std::byte> elf(std::uint16_t type, std::uint16_t machine) {
    auto b = bytes({0x7f, 'E', 'L', 'F', 2, 1, 1}, 64);
    put16(b, 16, type);
    put16(b, 18, machine);
    return b;
}

std::vector<std::byte> pe(std::uint16_t machine, std::uint16_t characteristics) {
    auto b = bytes({'M', 'Z'}, 512);
    put32(b, 0x3c, 0x80);
    put32(b, 0x80, 0x00004550);
    put16(b, 0x84, machine);
    put16(b, 0x84 + 18, characteristics);
    return b;
}

std::vector<std::byte> macho(std::uint32_t cputype, std::uint32_t filetype) {
    auto b = bytes({0xcf, 0xfa, 0xed, 0xfe}, 64);
    put32(b, 4, cputype);
    put32(b, 12, filetype);
    return b;
}

} // namespace

TEST_CASE("addon_source: the GitHub links a player pastes") {
    struct Case {
        const char* link;
        const char* text;
    };
    for (const Case& c : {Case{"https://github.com/owner/repo", "owner/repo"},
                          Case{"  https://github.com/owner/repo/  ", "owner/repo"},
                          Case{"http://www.github.com/Owner-1/my.repo_x.git", "Owner-1/my.repo_x"},
                          Case{"github.com/owner/repo?tab=readme-ov-file#install", "owner/repo"},
                          Case{"https://github.com/owner/repo/releases", "owner/repo"},
                          Case{"https://github.com/owner/repo/releases/latest", "owner/repo"},
                          Case{"https://github.com/owner/repo/releases/tag/v1.2.0", "owner/repo@v1.2.0"},
                          Case{"https://github.com/owner/repo/tree/feature/x", "owner/repo#feature/x"},
                          Case{"owner/repo", "owner/repo"}, Case{"owner/repo@v2", "owner/repo@v2"},
                          Case{"owner/repo#dev", "owner/repo#dev"}}) {
        CAPTURE(c.link);
        auto s = parse_addon_link(c.link);
        REQUIRE(s);
        CHECK(addon_source_text(*s) == c.text);
        // The text form reads back as the same source.
        auto again = parse_addon_link(addon_source_text(*s));
        REQUIRE(again);
        CHECK(addon_source_text(*again) == c.text);
    }
    for (const char* bad : {"", "   ", "https://gitlab.com/owner/repo", "https://github.com.evil.com/owner/repo",
                            "https://github.com/owner", "https://github.com/owner/repo/blob/main/x.lua",
                            "https://github.com/-owner/repo", "https://github.com/ow--ner/repo",
                            "https://github.com/owner/..", "https://github.com/owner/repo/tree/../../x",
                            "https://github.com/owner/repo/releases/tag/.hidden", "owner", "owner/repo/extra",
                            "owner/repo@", "owner/repo@a b", "owner\\repo", "file:///etc/passwd",
                            "https://user:pw@github.com/owner/repo"}) {
        CAPTURE(bad);
        std::string why;
        CHECK_FALSE(parse_addon_link(bad, &why));
        CHECK_FALSE(why.empty());
    }
}

TEST_CASE("addon_manifest: managed Lua activation is an explicit strict script-only opt-in") {
    const auto ordinary = parse_addon_manifest(script_manifest());
    REQUIRE(ordinary);
    CHECK_FALSE(ordinary->autoload);
    const auto managed = parse_addon_manifest(script_manifest("autoload 1\n"));
    REQUIRE(managed);
    CHECK(managed->autoload);
    CHECK(parse_addon_manifest(script_manifest("autoload 0\n")));
    for (const auto* bad : {"autoload yes\n", "autoload 2\n", "autoload\n", "autoload 1 0\n", "autoload 1\nautoload 1\n"})
        CHECK_FALSE(parse_addon_manifest(script_manifest(bad)));
    for (const std::string kind : {"plugin", "tool", "upscaler", "ui"}) {
        auto other = script_manifest();
        other.replace(other.find("kind script"), std::string("kind script").size(), "kind " + kind);
        if (kind != "ui") other.replace(other.find("package any"), std::string("package any").size(), "package windows-x64");
        if (kind != "plugin") other.replace(other.find("caps read, events,actions"), std::string("caps read, events,actions").size(), "caps none");
        REQUIRE(parse_addon_manifest(other)); // the kind is otherwise valid: activation is the reason for refusal
        std::string why;
        CHECK_FALSE(parse_addon_manifest(other + "autoload 1\n", &why));
        CHECK(why.find("autoload") != std::string::npos);
    }
}

TEST_CASE("addon_source: the manifest's addresses, and only GitHub's hosts") {
    auto latest = addon_manifest_urls(*parse_addon_link("owner/repo"));
    REQUIRE(latest.size() == 2);
    CHECK(latest[0] == "https://github.com/owner/repo/releases/latest/download/hailstone-addon.txt");
    CHECK(latest[1] == "https://raw.githubusercontent.com/owner/repo/HEAD/hailstone-addon.txt");
    auto tag = addon_manifest_urls(*parse_addon_link("owner/repo@v1"));
    REQUIRE(tag.size() == 2);
    CHECK(tag[0] == "https://github.com/owner/repo/releases/download/v1/hailstone-addon.txt");
    CHECK(tag[1] == "https://raw.githubusercontent.com/owner/repo/v1/hailstone-addon.txt");
    auto branch = addon_manifest_urls(*parse_addon_link("owner/repo#dev"));
    REQUIRE(branch.size() == 1);
    CHECK(branch[0] == "https://raw.githubusercontent.com/owner/repo/dev/hailstone-addon.txt");
    for (const auto& u : latest) CHECK(addon_download_allowed(u));

    CHECK(addon_download_allowed("https://objects.githubusercontent.com/github-production-release-asset/1"));
    CHECK(addon_download_allowed("https://release-assets.githubusercontent.com/x"));
    CHECK(addon_download_allowed("https://codeload.github.com/o/r/zip/refs/tags/v1"));
    CHECK(addon_download_allowed("HTTPS://GitHub.com/o/r"));
    CHECK_FALSE(addon_download_allowed("http://github.com/o/r"));
    CHECK_FALSE(addon_download_allowed("https://github.com.evil.com/o/r"));
    CHECK_FALSE(addon_download_allowed("https://evil.com/github.com/o/r"));
    CHECK_FALSE(addon_download_allowed("https://user@github.com/o/r"));
    CHECK_FALSE(addon_download_allowed("https://github.com:8443/o/r"));
    CHECK_FALSE(addon_download_allowed("https://gist.github.com/x"));

    CHECK(addon_url_beside("https://github.com/o/r/releases/latest/download/hailstone-addon.txt", "a.zip") ==
          "https://github.com/o/r/releases/latest/download/a.zip");
    CHECK(addon_url_beside("https://raw.githubusercontent.com/o/r/HEAD/hailstone-addon.txt?x=1", "a.zip") ==
          "https://raw.githubusercontent.com/o/r/HEAD/a.zip");
    CHECK(addon_url_beside("https://x/m.txt", "https://codeload.github.com/o/r/zip/v1") ==
          "https://codeload.github.com/o/r/zip/v1");
}

TEST_CASE("addon_manifest: a script's manifest reads") {
    std::string why;
    auto m = parse_addon_manifest(script_manifest("notes https://github.com/o/r/releases/tag/v1.3.0\nfuture thing\n"),
                                  &why);
    REQUIRE_MESSAGE(m, why);
    CHECK(m->name == "autoloot");
    CHECK(m->kind == AddonKind::Script);
    CHECK(m->version_text == "1.3.0");
    CHECK(m->author == "Some One");
    CHECK(m->description == "Loots what you tell it to.");
    CHECK(m->caps == std::vector<std::string>{"read", "events", "actions"});
    CHECK(m->notes == "https://github.com/o/r/releases/tag/v1.3.0");
    REQUIRE(m->packages.size() == 1);
    CHECK(m->packages[0].platform == "any");
    CHECK(m->packages[0].size == 41233);
    CHECK(to_hex(m->packages[0].sha256) == kHash);
    CHECK_FALSE(m->key);
    CHECK(addon_package_for(*m, "any") == &m->packages[0]);
    CHECK(addon_package_for(*m, "linux-x64") == nullptr);
}

TEST_CASE("addon_manifest: a plugin's packages, each for a platform") {
    std::string text = "hailstone-addon 1\nname MQ2Hud\nkind plugin\nversion 2.0.0-beta.1\nmin_client 0.2.0\ncaps read,ui\n"
                       "package linux-x64 10 " + kHash + " MQ2Hud-linux-x64.zip\n"
                       "package windows-x64 20 " + kHash +
                       " https://github.com/o/r/releases/download/v2/MQ2Hud-windows-x64.zip\n";
    std::string why;
    auto m = parse_addon_manifest(text, &why);
    REQUIRE_MESSAGE(m, why);
    CHECK(m->kind == AddonKind::Plugin);
    REQUIRE(m->min_client);
    CHECK(m->min_client_text == "0.2.0");
    CHECK(m->packages.size() == 2);
    CHECK(addon_package_for(*m, "windows-x64")->file.starts_with("https://github.com/"));
}

TEST_CASE("addon_manifest: what is refused") {
    struct Case {
        std::string text;
        const char* why;
    };
    const std::string pkg = "package any 1 " + kHash + " a.zip\n";
    const std::string head = "hailstone-addon 1\nname a\nkind script\nversion 1.0.0\ncaps read\n";
    for (const Case& c : std::vector<Case>{
             {"", "not a Hailstone"},
             {"hailstone-update 1\n", "not a Hailstone"},
             {"hailstone-addon 2\n", "newer Hailstone"},
             {"hailstone-addon 1\nkind script\nversion 1.0.0\ncaps read\n" + pkg, "no name"},
             {"hailstone-addon 1\nname a\nversion 1.0.0\ncaps read\n" + pkg, "no kind"},
             {"hailstone-addon 1\nname a\nkind script\ncaps read\n" + pkg, "no version"},
             {"hailstone-addon 1\nname a\nkind script\nversion 1.0.0\n" + pkg, "caps"},
             {head, "no package"},
             {head + "name b\n" + pkg, "bad name"},
             {"hailstone-addon 1\nname ../x\nkind script\nversion 1\ncaps read\n" + pkg, "bad name"},
             {"hailstone-addon 1\nname -x\nkind script\nversion 1\ncaps read\n" + pkg, "bad name"},
             {"hailstone-addon 1\nname a\nkind macro\nversion 1\ncaps read\n" + pkg, "kind"},
             {"hailstone-addon 1\nname a\nkind script\nversion one\ncaps read\n" + pkg, "bad version"},
             {head + "caps ui\n" + pkg, "repeats caps"},
             {"hailstone-addon 1\nname a\nkind script\nversion 1.0.0\ncaps re4d\n" + pkg, "bad capability"},
             {head + "package linux-x64 1 " + kHash + " a.zip\n", "any platform"},
             {"hailstone-addon 1\nname a\nkind plugin\nversion 1.0.0\ncaps read\n" + pkg, "one platform"},
             {head + "package any 0 " + kHash + " a.zip\n", "not a package"},
             {head + "package any 1 xyz a.zip\n", "not a package"},
             {head + "package any 1 " + kHash + " ../a.zip\n", "not a package"},
             {head + "package any 1 " + kHash + " https://evil.com/a.zip\n", "not a package"},
             {head + "package any 1 " + kHash + " http://github.com/a.zip\n", "not a package"},
             {head + pkg + pkg, "repeats a platform"},
             {head + pkg + "notes javascript:alert(1)\n", "notes"},
             {head + pkg + "author \x1b[31mred\n", "author"},
             {head + pkg + "key 1234\n", "bad key"},
         }) {
        CAPTURE(c.text);
        std::string why;
        CHECK_FALSE(parse_addon_manifest(c.text, &why));
        CHECK_MESSAGE(why.find(c.why) != std::string::npos, why);
    }
    CHECK_FALSE(parse_addon_manifest(std::string(70 * 1024, '#')));
}

TEST_CASE("addon_manifest: UI packages are portable interface bundles without script capabilities") {
    const std::string header = "hailstone-addon 1\nname interfaces\nkind ui\nversion 1.0.0\n";
    const std::string package = "package any 1 " + kHash + " interfaces.zip\n";
    for (const auto& caps : {std::string{}, std::string{"caps none\n"}}) {
        std::string why;
        const auto parsed = parse_addon_manifest(header + caps + package, &why);
        REQUIRE_MESSAGE(parsed, why);
        CHECK(parsed->kind == AddonKind::Ui);
        CHECK(parsed->caps.empty());
        CHECK(addon_package_for(*parsed, "any") != nullptr);
    }
    for (const auto& suffix : {"caps read,ui\n" + package,
                               "caps none\npackage windows-x64 1 " + kHash + " interfaces.zip\n"}) {
        std::string why;
        CHECK_FALSE(parse_addon_manifest(header + suffix, &why));
        CHECK_FALSE(why.empty());
    }
}

TEST_CASE("addon_manifest: the signature is the manifest's own key's") {
    const Keys k;
    std::string why;
    auto m = parse_addon_manifest(k.sign(script_manifest()), &why);
    REQUIRE_MESSAGE(m, why);
    REQUIRE(m->key);
    CHECK(*m->key == k.pub);

    // Changed after signing.
    std::string tampered = k.sign(script_manifest());
    tampered.replace(tampered.find("1.3.0"), 5, "9.9.9");
    CHECK_FALSE(parse_addon_manifest(tampered, &why));
    CHECK(why.find("does not match") != std::string::npos);
    // Another key's signature over the same text.
    const Keys other(9);
    std::string body = script_manifest() + "key " + hex_encode(k.pub) + "\n";
    CHECK_FALSE(parse_addon_manifest(body + "signature " + hex_encode(ed25519_sign(other.secret, body)) + "\n"));
    // A key without a signature, a signature without a key, lines after the signature.
    CHECK_FALSE(parse_addon_manifest(body, &why));
    CHECK(why.find("no signature") != std::string::npos);
    std::string plain = script_manifest();
    CHECK_FALSE(parse_addon_manifest(plain + "signature " + hex_encode(ed25519_sign(k.secret, plain)) + "\n", &why));
    CHECK(why.find("does not give its key") != std::string::npos);
    CHECK_FALSE(parse_addon_manifest(k.sign(script_manifest()) + "name x\n"));
}

TEST_CASE("addon_manifest: random and cut-off input is refused without a crash") {
    std::mt19937 rng(1234);
    const std::string good = Keys().sign(script_manifest());
    for (std::size_t cut = 0; cut < good.size(); ++cut) (void)parse_addon_manifest(good.substr(0, cut));
    for (int i = 0; i < 2000; ++i) {
        std::string s = good;
        for (int j = 0; j < 4; ++j) s[rng() % s.size()] = static_cast<char>(rng() & 0xff);
        (void)parse_addon_manifest(s);
        std::string junk(rng() % 300, '\0');
        for (char& c : junk) c = static_cast<char>(rng() & 0xff);
        (void)parse_addon_manifest(junk);
        (void)parse_addon_link(junk);
        (void)read_binary_header(std::as_bytes(std::span(junk.data(), junk.size())));
    }
}

TEST_CASE("addon_manifest: names an add-on can have") {
    for (const char* ok : {"a", "autoloot", "MQ2Hud", "my_script-2", "0x"}) CHECK(addon_name_ok(ok));
    for (const char* bad : {"", "-a", "_a", "a b", "a/b", "a.b", "..", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})
        CHECK_FALSE(addon_name_ok(bad));
}

TEST_CASE("binary_header: a library's platform from its header") {
    using namespace std::string_view_literals;
    auto check = [](const std::vector<std::byte>& b, const char* platform) {
        return binary_fits_platform(read_binary_header(b), platform);
    };
    CHECK(check(elf(3, 62), "linux-x64"));
    CHECK(check(elf(3, 183), "linux-arm64"));
    CHECK_FALSE(check(elf(3, 183), "linux-x64"));
    CHECK_FALSE(check(elf(2, 62), "linux-x64"));   // ET_EXEC: a program, not a library
    CHECK_FALSE(check(elf(3, 62), "windows-x64"));
    CHECK(check(pe(0x8664, 0x2022), "windows-x64"));
    CHECK(check(pe(0xaa64, 0x2022), "windows-arm64"));
    CHECK_FALSE(check(pe(0x8664, 0x0022), "windows-x64")); // an .exe
    CHECK_FALSE(check(pe(0x014c, 0x2022), "windows-x64"));
    CHECK(check(macho(0x0100000c, 6), "macos-arm64"));
    CHECK(check(macho(0x01000007, 8), "macos-x64"));
    CHECK_FALSE(check(macho(0x0100000c, 2), "macos-arm64")); // MH_EXECUTE

    // A universal file with both Mac architectures.
    auto fat = bytes({0xca, 0xfe, 0xba, 0xbe}, 256);
    put32(fat, 4, 2, true);
    put32(fat, 8 + 8, 64, true);
    put32(fat, 8 + 20 + 8, 128, true);
    auto a = macho(0x01000007, 6), b = macho(0x0100000c, 6);
    std::copy(a.begin(), a.begin() + 16, fat.begin() + 64);
    std::copy(b.begin(), b.begin() + 16, fat.begin() + 128);
    const BinaryHeader h = read_binary_header(fat);
    CHECK(h.format == BinaryFormat::MachO);
    CHECK(h.archs == std::vector<std::string>{"x64", "arm64"});
    CHECK(binary_fits_platform(h, "macos-arm64"));
    CHECK(binary_fits_platform(h, "macos-x64"));

    // Offsets past the end, a PE pointer out of the file, text.
    auto broken = pe(0x8664, 0x2022);
    put32(broken, 0x3c, 0xfffffff0);
    CHECK(read_binary_header(broken).format == BinaryFormat::Unknown);
    put32(fat, 8 + 8, 0xffffff00, true);
    CHECK(read_binary_header(fat).format == BinaryFormat::Unknown);
    const std::string text = "-- a Lua file\nprint('hi')\n";
    CHECK(read_binary_header(std::as_bytes(std::span(text.data(), text.size()))).format == BinaryFormat::Unknown);
    CHECK(read_binary_header({}).format == BinaryFormat::Unknown);

    CHECK(plugin_extension("linux-x64") == ".so"sv);
    CHECK(plugin_extension("windows-arm64") == ".dll"sv);
    CHECK(plugin_extension("macos-arm64") == ".dylib"sv);
    CHECK(plugin_extension("").empty());
}
