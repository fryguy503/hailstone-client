#pragma once

#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::platform {

// The folder that holds the binary (SDL_GetBasePath(), else the current directory). The shipped parts are read
// from here: shaders/ and ui/<pack>/.
std::filesystem::path install_dir();

// The root of everything Hailstone writes: install_dir() itself, so every setting stays in the Hailstone folder and
// goes with it when it is copied (choose_data_dir() has the one exception, a macOS app bundle). Nothing outside it is
// read for settings either. The choice is made once and kept. HAILSTONE_DATA_DIR (read on the first call) names
// another root, as does set_data_dir_for_tests(). Nothing is created here: create_data_dirs() makes the layout at
// start-up, and each writer still creates the folders it writes into (the player may delete one while it runs).
std::filesystem::path data_dir();
// Why data_dir() is where it is ("portable", "HAILSTONE_DATA_DIR", ...), for the log.
std::string data_dir_reason();
// Why data_dir() cannot be used, for the player (empty when it can): the folder is read-only, or it is an EverQuest
// folder, which Hailstone never writes. The client then stops at start with this message.
std::string data_dir_problem();

struct DataDirChoice {
    std::filesystem::path dir;
    std::string reason;
    std::string problem; // non-empty: `dir` cannot be used (see data_dir_problem())
};
// The data folder for a binary in `install`: `install` itself, unless
//   it is inside a macOS app bundle (.app/Contents/), where writing would break the bundle and an update would delete
//   the data: `bundle_data`, the user's folder for the app (the bundle can carry nothing else);
//   it is an EverQuest folder (eqgame.exe, spells_us.txt or eqclient.ini): a problem, since the asset folder is
//   never written;
//   it is not writable: a problem.
// No other folder is ever chosen: a Hailstone folder that cannot hold its settings asks to be moved instead.
DataDirChoice choose_data_dir(const std::filesystem::path& install, const std::filesystem::path& bundle_data);
// True when data_dir() is install_dir() (the portable layout).
bool data_dir_is_portable();
// Replaces data_dir() until called again; an empty path goes back to the normal choice.
void set_data_dir_for_tests(std::filesystem::path dir);
// The data folder of the Hailstone that started this program (a tool add-on in <data>/tools/<name>/, docs/addons.md,
// is told it with --data-dir): replaces the choice for the rest of the run; data_dir_reason() says `reason`.
// An EverQuest folder given that way is refused as choose_data_dir refuses one: data_dir_problem() then says so.
void use_data_dir(std::filesystem::path dir, std::string reason);
// Why a data folder named for this run (HAILSTONE_DATA_DIR, use_data_dir) cannot be used ("" it can): it is an
// EverQuest folder (eqgame.exe, spells_us.txt or eqclient.ini in it), which Hailstone never writes.
std::string given_data_dir_problem(const std::filesystem::path& dir);
// True when a file can be created in `dir` (which must exist); a probe file is written and removed.
bool is_writable_dir(const std::filesystem::path& dir);

// The shipped folders, beside the binary.
std::filesystem::path shaders_dir();  // install_dir()/shaders
std::filesystem::path ui_packs_dir(); // install_dir()/ui, one folder with a ui.ini per pack
std::filesystem::path icons_dir();    // install_dir()/icons, the window icon
std::filesystem::path pictures_dir(); // install_dir()/pictures, the shipped pre-game and loading pictures
std::filesystem::path flora_dir();    // install_dir()/flora, the zone flora rules (assets/zone_flora.h)
// The Vulkan driver a release ships beside the binary in `install`: libMoltenVK.dylib on macOS, which has no system
// Vulkan of its own. Empty when the file is not there (a dev build using an installed SDK) and on every other system,
// where the graphics driver brings Vulkan.
std::filesystem::path bundled_vulkan_library(const std::filesystem::path& install);

// The writable files and folders, all under data_dir().
std::filesystem::path config_file();                     // hailstone.ini, the client's settings
std::filesystem::path imgui_ini();                       // hailstone-windows.ini, ImGui's own
std::filesystem::path ui_settings_dir();                 // ui/settings/ (ui-settings/ before 2026-11)
std::filesystem::path ui_settings_file(std::string_view pack); // ui/settings/<pack>.txt, a UI pack's script settings
std::filesystem::path plugins_dir();                     // plugins/, one folder per native plugin
std::filesystem::path upscalers_dir();                   // upscalers/, one folder per upscaler provider (render/upscaler_provider.h)
std::filesystem::path scripts_dir();                     // scripts/, the Lua scripts
std::filesystem::path scripts_config_dir();              // scripts/config/, the scripts' saved settings (mq.configDir)
std::filesystem::path textures_dir();                    // textures/, one folder per texture pack
std::filesystem::path maps_dir();                        // maps/, the player's in-game maps for every server
std::filesystem::path servers_dir();                     // servers/, one folder per saved server (core/server_profile.h)
std::filesystem::path logs_dir();                        // logs/, the logs and the crash reports
std::filesystem::path log_file();                        // logs/hailstone.log
std::filesystem::path screenshots_dir();                 // screenshots/
std::filesystem::path bug_reports_dir();                 // bug-reports/, the bug and crash reports (app/bug_report.h)
std::filesystem::path cache_dir();                       // cache/, safe to delete
std::filesystem::path plugin_cache_dir();                // cache/plugins/, the approved copies of plugins

// The value of the environment variable `name` as a path, empty when unset or empty. On Windows it is read in the
// wide form, so a folder name the ANSI code page cannot show (a CJK user name) survives.
std::filesystem::path env_path(const char* name);

// The user's home folder (HOME, USERPROFILE on Windows), empty when unknown. Only bug reports use it, to take it out
// of the paths they hold (app/bug_report.h).
std::filesystem::path home_dir();

// Creates data_dir() and the folders the player looks in or drops files into (ui/settings/, plugins/, scripts/,
// scripts/config/, textures/, textures/default/, maps/, servers/, logs/, screenshots/, bug-reports/, cache/) when
// they are missing, so a first start shows the whole layout. Existing folders and their files are left alone.
// Returns the folders that could not be created.
std::vector<std::filesystem::path> create_data_dirs();

// Where a screenshot named `given` is written: an absolute path as it is, a relative one under screenshots_dir().
std::filesystem::path screenshot_path(const std::filesystem::path& given);
// The screenshot key's file name for the local time `local`: hailstone-YYYYMMDD-HHMMSS.png, with -<n> before the
// extension for n > 1 (a second picture in the same second).
std::string screenshot_file_name(const std::tm& local, int n = 1);
// The first screenshot_file_name() for `now` (local time) that is not in `dir` yet.
std::filesystem::path new_screenshot_path(const std::filesystem::path& dir, std::time_t now);
// The cached Vulkan pipelines (render/pipeline_cache.h): cache/pipelines.bin.
std::filesystem::path pipeline_cache_file();

} // namespace hailstone::platform
