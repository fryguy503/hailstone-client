// Tool add-ons (docs/addons.md "Tools"): a program of its own with its files, in tools/<name>/, that the client
// starts when the player asks and never loads or runs itself. The manifest's kind and caps, a program's header, what a
// tool's package may hold, its install and removal, and the manager installing one from GitHub in memory.

#include <doctest/doctest.h>

#include "addon_test_util.h"
#include "app/addon_install.h"
#include "app/addon_manager.h"
#include "core/addon_manifest.h"
#include "core/binary_header.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::app;
using namespace hailstone::addon_test;
namespace fs = std::filesystem;

namespace {

std::string tool_manifest(const std::string& name, const std::string& platform, const std::string& caps = "none",
                          const std::string& version = "1.0.0") {
    return manifest_for(name, "tool", version, caps, platform, "x", "t.zip");
}

AddonManifest tool(const std::string& name, const std::string& version = "1.0.0") {
    auto m = parse_addon_manifest(tool_manifest(name, this_platform(), "none", version));
    REQUIRE(m);
    return *m;
}

std::optional<std::vector<AddonFile>> unpack(const fs::path& dir, const AddonManifest& m,
                                             const std::map<std::string, std::string>& files, std::string* error) {
    const fs::path zip = dir / "pkg.zip";
    put_file(zip, zip_of(files));
    return unpack_addon(dir, m, zip, this_platform(), error);
}

std::string program_name(const std::string& name) { return addon_tool_program(name, this_platform()); }

} // namespace

TEST_CASE("addon tool: the manifest's kind, caps and packages") {
    std::string why;
    auto m = parse_addon_manifest(tool_manifest("texstudio", "linux-x64"), &why);
    REQUIRE_MESSAGE(m, why);
    CHECK(m->kind == AddonKind::Tool);
    CHECK(m->caps.empty());
    // A tool asks for nothing: the client grants it nothing.
    CHECK_FALSE(parse_addon_manifest(tool_manifest("texstudio", "linux-x64", "read"), &why));
    CHECK(why.find("caps none") != std::string::npos);
    // Each package is for one platform.
    CHECK_FALSE(parse_addon_manifest(tool_manifest("texstudio", "any"), &why));
    CHECK(why.find("one platform") != std::string::npos);
    CHECK_FALSE(parse_addon_manifest(manifest_for("x", "program", "1.0.0", "none", "any", "x", "t.zip"), &why));
    CHECK(why.find("script, plugin, tool, upscaler or ui") != std::string::npos);
    CHECK(addon_tool_program("texstudio", "windows-x64") == "texstudio.exe");
    CHECK(addon_tool_program("texstudio", "linux-x64") == "texstudio");
    CHECK(addon_tool_program("texstudio", "macos-arm64") == "texstudio");
}

TEST_CASE("addon tool: a program's header, told from a library's") {
    for (const char* p : {"linux-x64", "windows-x64", "macos-arm64", "linux-arm64", "windows-arm64"}) {
        CAPTURE(p);
        const std::string prog = program_for(p), lib = library_for(p);
        const BinaryHeader hp = read_binary_header(std::as_bytes(std::span(prog.data(), prog.size())));
        const BinaryHeader hl = read_binary_header(std::as_bytes(std::span(lib.data(), lib.size())));
        CHECK(program_fits_platform(hp, p));
        CHECK_FALSE(binary_fits_platform(hp, p));
        CHECK(binary_fits_platform(hl, p));
        // A DLL or a dylib is no program; an ELF shared object may be one (a position-independent program is ET_DYN).
        CHECK(program_fits_platform(hl, p) == std::string_view(p).starts_with("linux"));
    }
    const std::string win = program_for("windows-x64");
    const BinaryHeader h = read_binary_header(std::as_bytes(std::span(win.data(), win.size())));
    CHECK_FALSE(program_fits_platform(h, "linux-x64"));
    CHECK_FALSE(program_fits_platform(h, "windows-arm64"));
}

TEST_CASE("addon tool: a tool's package: its program, its libraries, its data") {
    if (this_platform().empty()) return;
    test::ScratchDir dir("addon_tool");
    const AddonManifest m = tool("texstudio");
    const std::string prog = program_for(this_platform()), lib = library_for(this_platform());
    const std::string libname = this_platform().starts_with("linux") ? "libSDL3.so.0" : "SDL3" + lib_ext();
    std::string why;
    auto files = unpack(dir.path(), m,
                        {{program_name("texstudio"), prog},
                         {libname, lib},
                         {"models/upscaler.bin", "weights"},
                         {"ui/modern/tools/texstudio.lua", "local x = 1\nreturn x\n"},
                         {"shaders/post.frag.spv", "spv"}},
                        &why);
    REQUIRE_MESSAGE(files, why);
    CHECK(files->size() == 5);
    const fs::path out = addons_dir(dir.path()) / "new" / "texstudio";
    CHECK(read_file(out / "models" / "upscaler.bin") == "weights");
#ifndef _WIN32
    // The program can be started; nothing else is made runnable.
    CHECK((fs::status(out / program_name("texstudio")).permissions() & fs::perms::owner_exec) != fs::perms::none);
    CHECK((fs::status(out / "models" / "upscaler.bin").permissions() & fs::perms::owner_exec) == fs::perms::none);
#endif

    struct Case {
        std::map<std::string, std::string> files;
        const char* why;
    };
    const std::string other = this_platform().starts_with("linux") ? "windows-x64" : "linux-x64";
    const std::string p = program_name("texstudio");
    for (const Case& c : std::vector<Case>{
             {{{"models/a.bin", "x"}}, "no program"},
             // (on Windows the program's name is a program's anywhere: refused as one)
             {{{"bin/" + p, prog}}, this_platform().starts_with("windows") ? "may hold" : "no program"},
             {{{p, lib}}, this_platform().starts_with("linux") ? "" : "not a program"},
             {{{p, "not a program at all"}}, "not a program"},
             {{{p, program_for(other)}}, "not a program"},
             {{{p, prog}, {"helper.exe", "MZ"}}, "may hold"},
             {{{p, prog}, {"run.sh", "rm -rf /"}}, "may hold"},
             {{{p, prog}, {"tools/fix.bat", "x"}}, "may hold"},
             {{{p, prog}, {"lib/" + libname, lib}}, "top"},
             {{{p, prog}, {libname, "not a library"}}, "not a library"},
             {{{p, prog}, {other.starts_with("linux") ? "libx.so" : "x.dll", "x"}}, "another platform"},
             {{{p, prog}, {"ui/x.lua", "x = "}}, "does not compile"},
             {{{p, prog}, {".hidden", "x"}}, "not a name"},
             {{{p, prog}, {"Models/a.bin", "x"}, {"models/A.bin", "y"}}, "twice"},
         }) {
        if (std::string_view(c.why).empty()) continue; // (an ELF shared object can be a program: not refused)
        std::string names;
        for (const auto& [n, _] : c.files) names += n + " ";
        CAPTURE(names);
        CHECK_FALSE(unpack(dir.path(), m, c.files, &why));
        CHECK_MESSAGE(why.find(c.why) != std::string::npos, why);
    }
    // The program's name in another case is not the program.
    if (!this_platform().starts_with("windows")) {
        CHECK_FALSE(unpack(dir.path(), m, {{"TexStudio", prog}}, &why));
        CHECK_MESSAGE(why.find("named texstudio") != std::string::npos, why);
    }
}

TEST_CASE("addon tool: install into tools/, record, name taken, removal") {
    if (this_platform().empty()) return;
    test::ScratchDir dir("addon_tool_apply");
    const AddonManifest m = tool("texstudio");
    std::string why;
    auto files = unpack(dir.path(), m, {{program_name("texstudio"), program_for(this_platform())}, {"a.txt", "1"}}, &why);
    REQUIRE_MESSAGE(files, why);
    InstalledAddon a;
    a.name = m.name;
    a.kind = m.kind;
    a.source = "owner/texstudio";
    a.version = m.version_text;
    a.files = *files;
    REQUIRE_MESSAGE(apply_addon(dir.path(), a, &why), why);
    const fs::path folder = dir.path() / "tools" / "texstudio";
    CHECK(addon_folder(dir.path(), AddonKind::Tool, "texstudio") == folder);
    CHECK(fs::exists(folder / program_name("texstudio")));
    auto all = read_installed_addons(dir.path());
    REQUIRE(all.size() == 1);
    CHECK(all[0].kind == AddonKind::Tool);
    CHECK(addon_kind_word(all[0].kind) == "tool");
    CHECK(addon_owns_folder(dir.path(), "tools/texstudio"));
    // A tool gets no ceilings: nothing of it runs in the client.
    CHECK(addon_ceilings(all).empty());
    // Its name is taken, and so is any folder of that name in tools/.
    CHECK_FALSE(addon_name_taken(dir.path(), all, AddonKind::Tool, "texstudio").empty());
    put_file(dir.path() / "tools" / "other" / "x.txt", "x");
    CHECK(addon_name_taken(dir.path(), {}, AddonKind::Tool, "other").find("tools/other") != std::string::npos);
    CHECK(addon_name_taken(dir.path(), {}, AddonKind::Tool, "free").empty());
    // A tool cannot become a script.
    InstalledAddon s = a;
    s.kind = AddonKind::Script;
    CHECK_FALSE(apply_addon(dir.path(), s, &why));
    CHECK(why.find("tool to script") != std::string::npos);

    put_file(folder / "settings.ini", "mine");
    REQUIRE_MESSAGE(remove_addon(dir.path(), "texstudio", &why), why);
    CHECK_FALSE(fs::exists(folder / program_name("texstudio")));
    CHECK(read_file(folder / "settings.ini") == "mine"); // the player's own stays
    CHECK(read_installed_addons(dir.path()).empty());
}

TEST_CASE("addon tool: the manager installs one from GitHub, and refuses one with no build here") {
    if (this_platform().empty()) return;
    test::ScratchDir dir("addon_tool_manager");
    FakeGitHub github;
    AddonsEnv env;
    env.data_dir = dir.path();
    env.client_version = "0.7.0-beta";
    env.platform = this_platform();
    env.fetch = github.fetch();
    env.now = [] { return std::int64_t{1'000'000}; };
    AddonManager manager(std::move(env));
    manager.wait_idle();

    const std::string zip = zip_of({{program_name("texstudio"), program_for(this_platform())}, {"models/m.bin", "w"}});
    const Keys key;
    github.release("owner/texstudio", key.sign(manifest_for("texstudio", "tool", "1.0.0", "none", this_platform(), zip,
                                                            "t.zip")),
                   "t.zip", zip);
    manager.inspect("https://github.com/owner/texstudio");
    manager.wait_idle();
    AddonsStatus st = manager.status();
    REQUIRE_MESSAGE(st.offer, st.message);
    CHECK_MESSAGE(st.offer->blocked.empty(), st.offer->blocked);
    CHECK(st.offer->caps == 0);
    manager.install_offer();
    manager.wait_idle();
    REQUIRE_MESSAGE(manager.has_ready(), manager.status().message);
    REQUIRE(manager.install_ready().size() == 1);
    CHECK(fs::exists(dir.path() / "tools" / "texstudio" / program_name("texstudio")));
    CHECK(fs::exists(dir.path() / "tools" / "texstudio" / "models" / "m.bin"));

    // A release with no build for this platform cannot be installed here.
    const std::string other = this_platform().starts_with("linux") ? "windows-x64" : "linux-x64";
    github.release("owner/elsewhere", manifest_for("elsewhere", "tool", "1.0.0", "none", other, zip, "e.zip"), "e.zip",
                   zip);
    manager.inspect("https://github.com/owner/elsewhere");
    manager.wait_idle();
    st = manager.status();
    REQUIRE(st.offer);
    CHECK(st.offer->blocked.find("no build") != std::string::npos);
}
