#include "app/autoexec.h"

#include <format>
#include <system_error>

#include "core/ascii_case.h"
#include "core/atomic_file.h"
#include "core/path_utf8.h"

namespace hailstone::app {

namespace fs = std::filesystem;

namespace {

fs::path utf8_path(std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }

// A name that stays one file in the folder it is looked up in.
bool plain_name(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (char ch : name)
        if (ch == '/' || ch == '\\' || ch == ':' || static_cast<unsigned char>(ch) < 32) return false;
    return true;
}

// `file` in `dir`, in any case: the exact name first, then the folder's entries.
std::optional<fs::path> find_in(const fs::path& dir, std::string_view file) {
    if (dir.empty()) return std::nullopt;
    std::error_code ec;
    const fs::path exact = dir / utf8_path(file);
    if (fs::is_regular_file(exact, ec)) return exact;
    if (!fs::is_directory(dir, ec)) return std::nullopt;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!iequals(path_utf8(it->path().filename()), file)) continue;
        std::error_code type_ec;
        if (it->is_regular_file(type_ec)) return it->path();
    }
    return std::nullopt;
}

// The layer's autoexec/ folder (any case, as MacroQuest's Autoexec), then the layer itself.
std::optional<fs::path> find_in_layer(const fs::path& dir, std::string_view file) {
    if (dir.empty()) return std::nullopt;
    std::error_code ec;
    fs::path sub = dir / "autoexec";
    if (!fs::is_directory(sub, ec)) {
        sub.clear();
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code type_ec;
            if (iequals(path_utf8(it->path().filename()), "autoexec") && it->is_directory(type_ec)) {
                sub = it->path();
                break;
            }
        }
    }
    if (auto found = find_in(sub, file)) return found;
    return find_in(dir, file);
}

} // namespace

std::string cfg_file_name(std::string_view name) {
    std::string out(name);
    if (out.find('.') == std::string::npos) out += ".cfg";
    return out;
}

std::optional<fs::path> find_cfg(const CfgFolders& folders, std::string_view name) {
    if (!plain_name(name)) return std::nullopt;
    const std::string file = cfg_file_name(name);
    for (const fs::path* layer : {&folders.character, &folders.server, &folders.global})
        if (auto found = find_in_layer(*layer, file)) return found;
    return std::nullopt;
}

std::optional<fs::path> resolve_cfg(const CfgFolders& folders, std::string_view text, bool typed) {
    if (plain_name(text)) return find_cfg(folders, text);
    if (!typed || text.empty() || folders.data.empty()) return std::nullopt;
    for (char ch : text)
        if (static_cast<unsigned char>(ch) < 32) return std::nullopt;
    std::error_code ec;
    fs::path p = utf8_path(text);
    if (p.is_relative()) p = folders.data / p;
    if (p.extension().empty()) p += ".cfg";
    const fs::path file = fs::weakly_canonical(p, ec);
    if (ec || !iequals(path_utf8(file.extension()), ".cfg")) return std::nullopt;
    // Inside one of the cfg folders: the file's path starts with the folder's, whole components at a time.
    auto inside = [&file](const fs::path& dir) {
        if (dir.empty()) return false;
        std::error_code root_ec;
        const fs::path root = fs::weakly_canonical(dir, root_ec);
        if (root_ec) return false;
        auto f = file.begin();
        for (const fs::path& part : root) {
            if (part.empty()) continue; // (a trailing separator)
            if (f == file.end() || *f != part) return false;
            ++f;
        }
        return f != file.end();
    };
    if (!inside(folders.character) && !inside(folders.server) && !inside(folders.global)) return std::nullopt;
    if (!fs::is_regular_file(file, ec)) return std::nullopt;
    return file;
}

std::vector<std::string> cfg_lines(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    std::vector<std::string> out;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        out.emplace_back(line);
    }
    return out;
}

std::optional<CfgFile> read_cfg(const fs::path& file) {
    CfgFile out;
    std::error_code ec;
    out.file = fs::weakly_canonical(file, ec);
    if (ec) out.file = file;
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec) return std::nullopt;
    auto text = size > kMaxCfgBytes ? std::nullopt : read_text_file(file);
    if (size > kMaxCfgBytes || (text && text->size() > kMaxCfgBytes)) { // (or grown since)
        out.refused = std::format("'{}' is larger than {} KB: not run", path_utf8(file.filename()), kMaxCfgBytes / 1024);
        return out;
    }
    if (!text) return std::nullopt;
    out.lines = cfg_lines(*text);
    return out;
}

std::optional<CfgFile> load_cfg(const CfgFolders& folders, std::string_view text, bool typed) {
    if (!typed && !plain_name(text)) {
        CfgFile out;
        out.refused = "A script or plugin may /loadcfg a .cfg file by its name only, not by a path";
        return out;
    }
    const auto file = resolve_cfg(folders, text, typed);
    if (!file) return std::nullopt;
    return read_cfg(*file);
}

} // namespace hailstone::app
