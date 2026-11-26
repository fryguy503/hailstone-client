#pragma once

// Window places as the original keeps them: the per-character UI_<name>_<server>.ini (this client's
// characters/<world>/<name>/UI.ini, core/character_files.h) over defaults.ini.
//
// CSidlScreenWnd::LoadIniInfo @0x85dda0 starts from the XML rectangle, then reads Width / Height and XPos / YPos
// (CSidlScreenWnd_LoadIniSize / LoadIniPosition, keys "<key><W>x<H>" in full screen, "<key>Windowed" in a window),
// each through CEverQuest::ReadUIINI*: the default, then defaults.ini, then the character's file. A position is
// stored for one screen size and moved to the current one by CSidlScreenWnd::ConvertToRes: a window right of the
// middle keeps its distance from the right edge, one over the middle keeps its distance from the middle, the rest
// keep theirs from the left; windowed places are stored for a 4096 x 4096 screen (CResolutionHandler::
// GetFullscreenSize). "Show" comes from the character's file only (defaults.ini too for "HotButtonWnd"); without it
// a window is shown when its IniFlags have bit 9 (CSidlScreenWnd_InitIniStorage).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/ini_file.h"

namespace hailstone::ui {

// CSidlScreenWnd::ConvertToRes @0x88bfa0: `pos` of a window `extent` wide, stored for a screen `from` wide, on a
// screen `to` wide.
int convert_to_res(int pos, int extent, int from, int to);

// What the original's window class gives CSidlScreenWnd: the IniFlags (bit 0 position, bit 1 size, bit 8 the
// shown state is stored, bit 9 shown at first), the IniVersion and the ini section (IniStorageName).
struct WindowIniInfo {
    std::string section;
    std::uint32_t flags = 0xfffffcff; // CSidlScreenWnd2's (no shown state; hidden at first)
    int version = 1;
};
WindowIniInfo window_ini_info(std::string_view screen);

struct WindowRect {
    int x = 0, y = 0, w = 0, h = 0;
};

class UiIni {
public:
    // defaults: the game's defaults.ini; file: the character's UI ini (written by store / save); seed: read in
    // place of `file` while that does not exist yet (the game's own UI_<name>_<server>.ini).
    void load(const std::filesystem::path& defaults, const std::filesystem::path& file,
              const std::filesystem::path& seed = {});
    bool loaded() const { return loaded_; }
    // Bumped by every load: windows placed under an older one are placed again.
    int generation() const { return generation_; }

    // LoadIniSize + LoadIniPosition: the XML rectangle `xml` on a screen sw x sh (windowed). `sizable`: the window
    // can be resized (WindowStyle 0x200; without it IniFlags bit 1 is cleared and the size is the XML's).
    WindowRect place(const WindowIniInfo& info, WindowRect xml, int sw, int sh, bool sizable) const;
    // The stored shown state, when the window keeps one.
    std::optional<bool> shown(const WindowIniInfo& info) const;

    // StoreIniInfo: the window's place on a sw x sh screen; StoreIniVis: its shown state (bit 8).
    void store(const WindowIniInfo& info, WindowRect r, int sw, int sh, bool sizable);
    void store_shown(const WindowIniInfo& info, bool shown);
    // A window's own settings (CEverQuest::ReadUIINIString: the character's file over defaults.ini, an empty value
    // counting as none; UIIni_WriteString: into the character's file).
    std::optional<std::string> get(std::string_view section, std::string_view key) const {
        return read(section, key, true, true);
    }
    // The character's file alone (defaults.ini not read).
    std::optional<std::string> get_own(std::string_view section, std::string_view key) const {
        return read(section, key, true, false);
    }
    void put(std::string_view section, std::string_view key, std::string_view value);
    // DeleteUIINIKey: the key out of the character's file.
    void erase(std::string_view section, std::string_view key);
    bool dirty() const { return dirty_; }
    bool save();
    // Save a replacement character UI.ini before changing the live copy.
    bool replace_file(IniFile file);
    // StoreIniInfo's writes: the changes go to the file at most every 2 s (of time, not of frames: at hundreds of
    // frames a second a frame count rewrote it several times a second while a window was dragged).
    static constexpr std::chrono::milliseconds kSaveInterval{2000};
    bool save_if_due(std::chrono::steady_clock::time_point now);

    // Tests: the two files' contents.
    IniFile& defaults() { return defaults_; }
    IniFile& file() { return file_; }

private:
    std::optional<std::string> read(std::string_view section, std::string_view key, bool use_ui, bool use_defaults) const;
    bool use_ui(const WindowIniInfo& info) const;
    void set(std::string_view section, std::string_view key, int value);

    IniFile defaults_, file_;
    std::filesystem::path path_;
    bool loaded_ = false, dirty_ = false;
    int generation_ = 0;
    std::optional<std::chrono::steady_clock::time_point> last_save_;
};

} // namespace hailstone::ui
