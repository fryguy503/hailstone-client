#include "platform/paths.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <SDL3/SDL_filesystem.h>

namespace hailstone::platform {

std::filesystem::path env_path(const char* name) {
#if defined(_WIN32)
    // The wide form: getenv's text is in the ANSI code page, which turns a name it cannot show (a CJK user name in
    // APPDATA) into '?'.
    std::wstring wide(name, name + std::char_traits<char>::length(name));
    const wchar_t* v = _wgetenv(wide.c_str());
#else
    const char* v = std::getenv(name);
#endif
    return (v && *v) ? std::filesystem::path(v) : std::filesystem::path();
}

namespace {

std::filesystem::path config_dir_named(const char* kApp) {
#if defined(_WIN32)
    if (auto appdata = env_path("APPDATA"); !appdata.empty()) return appdata / kApp;
    if (auto home = env_path("USERPROFILE"); !home.empty()) return home / "AppData" / "Roaming" / kApp;
#elif defined(__APPLE__)
    if (auto home = env_path("HOME"); !home.empty()) return home / "Library" / "Application Support" / kApp;
#else
    if (auto xdg = env_path("XDG_CONFIG_HOME"); !xdg.empty() && xdg.is_absolute()) return xdg / kApp;
    if (auto home = env_path("HOME"); !home.empty()) return home / ".config" / kApp;
#endif
    return std::filesystem::path(".") / kApp;
}

} // namespace

std::filesystem::path install_dir() {
    static const std::filesystem::path dir = [] {
        // SDL gives UTF-8, which a plain narrow path would read in the ANSI code page on Windows.
        if (const char* base = SDL_GetBasePath(); base && *base)
            return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(base)));
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        return ec ? std::filesystem::path(".") : cwd;
    }();
    return dir;
}

bool is_writable_dir(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    const fs::path probe = dir / ".hailstone-write-test";
    bool ok = static_cast<bool>(std::ofstream(probe, std::ios::binary | std::ios::trunc));
    fs::remove(probe, ec);
    return ok;
}

namespace {
// A path as lower-case UTF-8 (ASCII letters only), which never throws on a name the narrow code page cannot show.
std::string utf8_lower(const std::filesystem::path& p) {
    std::string out;
    try {
        const std::u8string u = p.generic_u8string();
        out.assign(u.begin(), u.end());
    } catch (const std::exception&) { // a name that is not even valid UTF-16 (Windows): matches nothing
        return {};
    }
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// True when `dir` holds a top-level file named (in any case) one of `names`.
bool has_any_file(const std::filesystem::path& dir, std::initializer_list<std::string_view> names) {
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        // As UTF-8: string() throws on Windows for a name the ANSI code page cannot show.
        const std::string name = utf8_lower(it->path().filename());
        for (std::string_view n : names)
            if (name == n) return true;
    }
    return false;
}

std::mutex g_data_dir_mutex;
std::optional<std::filesystem::path> g_test_data_dir;
std::string g_data_dir_reason;

std::string g_data_dir_problem;
std::string g_given_reason;  // use_data_dir's
std::string g_given_problem; // use_data_dir's

std::filesystem::path chosen_data_dir() {
    static const std::filesystem::path dir = [] {
        if (auto env = env_path("HAILSTONE_DATA_DIR"); !env.empty()) {
            std::error_code ec;
            auto abs = std::filesystem::absolute(env, ec);
            g_data_dir_reason = "HAILSTONE_DATA_DIR";
            g_data_dir_problem = given_data_dir_problem(ec ? env : abs);
            return ec ? env : abs;
        }
        DataDirChoice c = choose_data_dir(install_dir(), config_dir_named("hailstone"));
        g_data_dir_reason = std::move(c.reason);
        g_data_dir_problem = std::move(c.problem);
        return c.dir;
    }();
    return dir;
}
} // namespace

std::string given_data_dir_problem(const std::filesystem::path& dir) {
    if (!has_any_file(dir, {"eqgame.exe", "spells_us.txt", "eqclient.ini"})) return {};
    std::string folder;
    try {
        const std::u8string u = dir.u8string();
        folder.assign(u.begin(), u.end());
    } catch (const std::exception&) {
        folder = "the folder given";
    }
    return "Hailstone's data folder (" + folder + ") is an EverQuest folder, which Hailstone only reads. Name a "
           "folder of its own (HAILSTONE_DATA_DIR) and start it again.";
}

DataDirChoice choose_data_dir(const std::filesystem::path& install, const std::filesystem::path& bundle_data) {
    // Inside a macOS app bundle (Hailstone.app/Contents/Resources/): replacing the app, its normal update, would
    // delete the data, and writing there breaks the bundle's signature.
    const std::string where = utf8_lower(install);
    if (where.find(".app/contents/") != std::string::npos || where.ends_with(".app/contents"))
        return {bundle_data, "per-user (inside an app bundle)", {}};
    std::string folder; // as UTF-8: string() throws on Windows for a name the ANSI code page cannot show
    try {
        const std::u8string u = install.u8string();
        folder.assign(u.begin(), u.end());
    } catch (const std::exception&) {
        folder = "its folder";
    }
    // Unpacked into the EverQuest folder: that is the asset folder, which Hailstone never writes (not even the probe
    // file of the write test).
    if (has_any_file(install, {"eqgame.exe", "spells_us.txt", "eqclient.ini"}))
        return {install, "portable", "Hailstone is in an EverQuest folder (" + folder +
                                         "), which it only reads. Move the Hailstone folder somewhere of its own, "
                                         "for example your Documents or Games folder, and start it again."};
    if (!is_writable_dir(install))
        return {install, "portable", "Hailstone keeps all its settings in its own folder, and it cannot write to " +
                                         folder + ". Move the Hailstone folder somewhere you can write to, for "
                                         "example your Documents or Games folder, and start it again."};
    return {install, "portable", {}};
}

std::string data_dir_reason() {
    {
        std::lock_guard lock(g_data_dir_mutex);
        if (g_test_data_dir && !g_given_reason.empty()) return g_given_reason;
    }
    chosen_data_dir();
    return g_data_dir_reason;
}

std::string data_dir_problem() {
    {
        std::lock_guard lock(g_data_dir_mutex);
        // The folder another Hailstone runs from (checked as it was given), or a test's.
        if (g_test_data_dir) return g_given_problem;
    }
    chosen_data_dir();
    return g_data_dir_problem;
}

std::filesystem::path data_dir() {
    {
        std::lock_guard lock(g_data_dir_mutex);
        if (g_test_data_dir) return *g_test_data_dir;
    }
    return chosen_data_dir();
}

bool data_dir_is_portable() {
    std::error_code ec;
    return std::filesystem::equivalent(data_dir(), install_dir(), ec);
}

void set_data_dir_for_tests(std::filesystem::path dir) {
    std::lock_guard lock(g_data_dir_mutex);
    g_given_reason.clear();
    g_given_problem.clear();
    if (dir.empty()) g_test_data_dir.reset();
    else g_test_data_dir = std::move(dir);
}

void use_data_dir(std::filesystem::path dir, std::string reason) {
    std::error_code ec;
    auto abs = std::filesystem::absolute(dir, ec);
    std::string problem = given_data_dir_problem(ec ? dir : abs);
    std::lock_guard lock(g_data_dir_mutex);
    g_test_data_dir = ec ? std::move(dir) : std::move(abs);
    g_given_reason = std::move(reason);
    g_given_problem = std::move(problem);
}

std::filesystem::path shaders_dir() { return install_dir() / "shaders"; }
std::filesystem::path bundled_vulkan_library(const std::filesystem::path& install) {
#if defined(__APPLE__)
    std::error_code ec;
    if (auto lib = install / "libMoltenVK.dylib"; std::filesystem::is_regular_file(lib, ec)) return lib;
#else
    (void)install;
#endif
    return {};
}
std::filesystem::path ui_packs_dir() { return install_dir() / "ui"; }
std::filesystem::path icons_dir() { return install_dir() / "icons"; }
std::filesystem::path pictures_dir() { return install_dir() / "pictures"; }
std::filesystem::path flora_dir() { return install_dir() / "flora"; }

std::filesystem::path config_file() { return data_dir() / "hailstone.ini"; }
std::filesystem::path imgui_ini() { return data_dir() / "hailstone-windows.ini"; }
// under ui/ beside the packs (ui/settings has no ui.ini, so it is no pack; a build or update replaces only the packs)
std::filesystem::path ui_settings_dir() { return data_dir() / "ui" / "settings"; }
std::filesystem::path ui_settings_file(std::string_view pack) {
    return ui_settings_dir() / (std::string(pack) + ".txt");
}
std::filesystem::path plugins_dir() { return data_dir() / "plugins"; }
std::filesystem::path upscalers_dir() { return data_dir() / "upscalers"; }
std::filesystem::path scripts_dir() { return data_dir() / "scripts"; }
std::filesystem::path scripts_config_dir() { return scripts_dir() / "config"; }
std::filesystem::path textures_dir() { return data_dir() / "textures"; }
std::filesystem::path maps_dir() { return data_dir() / "maps"; }
std::filesystem::path servers_dir() { return data_dir() / "servers"; }
std::filesystem::path logs_dir() { return data_dir() / "logs"; }
std::filesystem::path log_file() { return logs_dir() / "hailstone.log"; }
std::filesystem::path screenshots_dir() { return data_dir() / "screenshots"; }
std::filesystem::path bug_reports_dir() { return data_dir() / "bug-reports"; }
std::filesystem::path cache_dir() { return data_dir() / "cache"; }
std::filesystem::path plugin_cache_dir() { return cache_dir() / "plugins"; }
std::filesystem::path pipeline_cache_file() { return cache_dir() / "pipelines.bin"; }

std::filesystem::path home_dir() {
#if defined(_WIN32)
    return env_path("USERPROFILE");
#else
    return env_path("HOME");
#endif
}

std::vector<std::filesystem::path> create_data_dirs() {
    // scripts/config/ and textures/default/ make scripts/ and textures/ too.
    const std::filesystem::path dirs[] = {ui_settings_dir(), plugins_dir(),  scripts_config_dir(), textures_dir() / "default",
                                          maps_dir(),        servers_dir(),  logs_dir(),           screenshots_dir(),
                                          bug_reports_dir(), cache_dir()};
    std::vector<std::filesystem::path> failed;
    for (const auto& dir : dirs) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (!std::filesystem::is_directory(dir, ec)) failed.push_back(dir);
    }
    return failed;
}

std::filesystem::path screenshot_path(const std::filesystem::path& given) {
    if (given.empty() || given.is_absolute()) return given;
    return screenshots_dir() / given;
}

std::string screenshot_file_name(const std::tm& local, int n) {
    char name[64];
    std::snprintf(name, sizeof name, "hailstone-%04d%02d%02d-%02d%02d%02d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    std::string out = name;
    if (n > 1) out += "-" + std::to_string(n);
    return out + ".png";
}

std::filesystem::path new_screenshot_path(const std::filesystem::path& dir, std::time_t now) {
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::error_code ec;
    for (int n = 1;; ++n) {
        std::filesystem::path p = dir / screenshot_file_name(local, n);
        if (!std::filesystem::exists(p, ec)) return p;
    }
}

} // namespace hailstone::platform
