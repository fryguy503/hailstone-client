#pragma once

// MacroQuest's .cfg files (docs/autoexec.md): files of command lines the client runs at set moments, as the player's
// own typed lines. MacroQuest's LoadCfgFile reads Config\Autoexec\<name>.cfg, else Config\<name>.cfg; here the same
// two folders are looked for in three layers, the most specific first, and only the first file found runs:
//   servers/<profile>/characters/<world>/<name>/cfg/   one character
//   servers/<profile>/cfg/                             one server
//   <data>/cfg/                                        every server
// A server's downloads never land in a cfg/ folder (net::server_file_destination puts any other path under
// assets/), and the game folder is never read, so no file here is one a server wrote.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::app {

struct CfgFolders {
    std::filesystem::path data;      // the data folder: what a /loadcfg path is relative to
    std::filesystem::path global;    // <data>/cfg
    std::filesystem::path server;    // servers/<profile>/cfg; empty: no server known
    std::filesystem::path character; // servers/<profile>/characters/<world>/<name>/cfg; empty: no character yet
};

// The largest .cfg file that runs: a larger one is refused with a chat line, so a stray big file is never read whole
// and sent line by line.
inline constexpr std::uintmax_t kMaxCfgBytes = 64 * 1024;

// A .cfg file to run.
struct CfgFile {
    std::filesystem::path file;     // its canonical path: what tells one running file from another
    std::vector<std::string> lines; // cfg_lines
    std::string refused;            // not empty: the file does not run, and this chat line says why
};

// MacroQuest's file name for `name`: ".cfg" added when the name has no '.' ("InGame" -> "InGame.cfg").
std::string cfg_file_name(std::string_view name);

// The file that runs for `name`: in the most specific layer that has it (the character's, the server's, every
// server's), its autoexec/ folder before the layer's own. Names match in any case, as on MacroQuest's Windows, but only
// ASCII letters fold (core/ascii_case.h): "É.cfg" does not match "é.cfg".
// nullopt: no layer has it, or the name is not a plain file name (a path, "..", a separator, a control character).
std::optional<std::filesystem::path> find_cfg(const CfgFolders& folders, std::string_view name);

// /loadcfg's argument. A plain name is looked up as find_cfg does. A path (one with a separator), relative to the data
// folder or absolute, is taken only when the player typed it (`typed`), and only when it names a .cfg file inside one
// of the three cfg folders: a script or plugin, which cannot read files itself, must not run the data folder's other
// files (a plugin's settings, the chat history) as chat lines. MacroQuest takes any path.
std::optional<std::filesystem::path> resolve_cfg(const CfgFolders& folders, std::string_view text, bool typed);

// A file's command lines, in order: each line without its line end and leading blanks; empty lines and lines that
// start with ';' (MacroQuest's comment) or '#' (Hailstone's too) left out. A UTF-8 byte order mark is dropped.
std::vector<std::string> cfg_lines(std::string_view text);

// The file's lines (cfg_lines); nullopt when it cannot be read; `refused` set when it is larger than kMaxCfgBytes.
std::optional<CfgFile> read_cfg(const std::filesystem::path& file);

// /loadcfg's file (resolve_cfg, read_cfg); nullopt: no such file. `refused` is set for a path a script or plugin sent.
std::optional<CfgFile> load_cfg(const CfgFolders& folders, std::string_view text, bool typed);

} // namespace hailstone::app
