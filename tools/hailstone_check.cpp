// hailstone_check: checks a server's client_files/ folder as Hailstone will take it (docs/plugins.md): its
// hailstone.cfg (what it says, and what Hailstone does not understand in it), and where each file goes in a player's
// servers/<profile>/ folder (net/server_files.h server_file_destination), naming the files a client refuses.
//
//   hailstone_check CLIENT_FILES_DIR
//
// Exits 1 when the cfg has warnings or files are refused.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "net/server_files.h"
#include "plugin/policy.h"

namespace fs = std::filesystem;
using namespace hailstone;

namespace {

const char* mode_name(plugin::ButtonMode m) {
    switch (m) {
    case plugin::ButtonMode::Press: return "press";
    case plugin::ButtonMode::Full: return "full";
    default: return "off";
    }
}

std::string caps_or_no(std::uint32_t caps) { return caps ? "yes (" + plugin::caps_text(caps) + ")" : "no"; }

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
        std::puts("usage: hailstone_check CLIENT_FILES_DIR");
        return argc == 2 ? 0 : 2;
    }
    fs::path dir = argv[1];
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::fprintf(stderr, "%s is not a folder\n", dir.string().c_str());
        return 2;
    }
    int problems = 0;

    fs::path cfg = dir / plugin::kServerCfgName;
    if (!fs::is_regular_file(cfg, ec)) {
        std::printf("no %s: players' clients set no rules (their own plugins, scripts and assets all run)\n",
                    std::string(plugin::kServerCfgName).c_str());
    } else {
        std::ifstream in(cfg, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::string> warnings;
        plugin::Rules r = plugin::parse_server_cfg(text, &warnings);
        std::printf("%s:\n", cfg.string().c_str());
        for (const auto& w : warnings) std::printf("  warning: %s\n", w.c_str());
        problems += static_cast<int>(warnings.size());
        std::printf("  Hailstone access: %s\n", r.deny_client ? "denied" : "allowed");
        std::printf("  the player's own scripts: %s\n", caps_or_no(r.player_script_caps).c_str());
        std::printf("  the player's own plugins: %s\n", caps_or_no(r.player_plugin_caps).c_str());
        std::printf("  the player's own models and zones in the server's assets folder: %s\n",
                    r.player_assets ? "yes" : "no (textures still count)");
        std::string run;
        for (const auto& n : r.run_scripts) run += (run.empty() ? "" : ", ") + n;
        std::printf("  scripts started at each zone-in: %s\n", run.empty() ? "none" : run.c_str());
        std::printf("  Lua socials: %s, may %s%s, button faces %s\n", mode_name(r.social_mode),
                    plugin::caps_text(r.social_caps).c_str(),
                    r.social_mode == plugin::ButtonMode::Press
                        ? (", " + std::to_string(r.social_press_commands) + " commands a press").c_str()
                        : "",
                    r.social_faces ? "on" : "off");
        std::printf("  autoskill (the Ability Book): %s\n", r.autoskill ? "allowed" : "off");
        std::printf("  NPC collision: %s\n", r.npc_collision ? "on" : "off");
        std::printf("  passable ladders climbed: %s\n", r.passable_ladders ? "on" : "off");
    }

    // Where the files go, as the server's script lists them (names starting with '.' are not sent).
    std::map<std::string, int> counts;
    std::vector<std::string> refused, other;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string rel = fs::relative(it->path(), dir, ec).generic_string();
        if (rel.empty() || rel.front() == '.' || rel.find("/.") != std::string::npos) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        bool other_platform = false;
        auto dest = net::server_file_destination(rel, &other_platform);
        if (dest) ++counts[dest->substr(0, dest->find('/'))];
        else if (other_platform) other.push_back(rel);
        else refused.push_back(rel);
    }
    std::printf("files (for a %s client):\n", std::string(net::plugin_platform()).c_str());
    for (const auto& [where, n] : counts) std::printf("  %-14s %d\n", where.c_str(), n);
    if (!other.empty()) std::printf("  %zu for other platforms (plugins/<platform>/ or another kind of library)\n", other.size());
    for (const auto& f : refused) std::printf("  refused: %s (a program, or a name a client does not take)\n", f.c_str());
    problems += static_cast<int>(refused.size());
    return problems ? 1 : 0;
}
