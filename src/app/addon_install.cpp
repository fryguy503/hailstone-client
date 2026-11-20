#include "app/addon_install.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

#include <lua.hpp>

#include "app/update_install.h"
#include "core/addon_source.h"
#include "core/atomic_file.h"
#include "core/binary_header.h"
#include "core/log.h"
#include "core/update_manifest.h"
#include "core/zip_read.h"
#include "net/server_files.h"
#include "platform/durable_file.h"
#include "plugin/native_plugins.h"
#include "plugin/policy.h"

namespace hailstone::app {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxFiles = 4096;
constexpr std::uint64_t kMaxScriptBytes = 64ull * 1024 * 1024;   // all of a script's files, unpacked
constexpr std::uint64_t kMaxPluginBytes = 512ull * 1024 * 1024;  // all of a plugin's files, unpacked
constexpr std::uint64_t kMaxLuaFile = 4ull * 1024 * 1024;        // one Lua file, read whole to compile it
constexpr std::uint64_t kMaxLibrary = 256ull * 1024 * 1024;      // the plugin's library, read whole for its header
constexpr std::string_view kRegistryHeader = "hailstone-addons 1";
constexpr std::string_view kRegistryBackup = "installed.bak.txt"; // installed.txt as last written

fs::path rel(std::string_view name) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool iequal(std::string_view a, std::string_view b) { return lower(a) == lower(b); }

std::pair<std::string_view, std::string_view> split(std::string_view line) {
    const auto sp = line.find(' ');
    if (sp == std::string_view::npos) return {line, {}};
    return {line.substr(0, sp), line.substr(sp + 1)};
}

// A data-folder-relative path as the journal writes it.
std::string journal_path(const fs::path& data_dir, const fs::path& p) {
    const auto u8 = p.lexically_relative(data_dir).generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// Write-ahead: each line is on the disk before the change it names is made (platform/durable_file.h), so after a
// power cut every moved file has its line and the undo at the next start finds it.
class Journal {
public:
    explicit Journal(const fs::path& data_dir) : data_dir_(data_dir), path_(addons_dir(data_dir) / "journal.txt") {
        std::error_code ec;
        fs::remove(path_, ec);
        ok_ = platform::append_file_synced(path_, "");
    }
    bool ok() const { return ok_; }
    // Moves each `from` to its `to` (making their folders), all of them written to the journal first in one synced
    // write: one flush to the disk for the batch, not one per file before its move. Every `to` must be free before
    // the batch is written, and nothing else may put a file there until the moves are made: an undo takes a `to`
    // that exists for a move that was made (a line for one that never was finds nothing there and is skipped). So a
    // batch never holds a move into a place another move of the same batch empties (moves out, then moves in).
    bool moves(const std::vector<std::pair<fs::path, fs::path>>& batch, std::string* error) {
        std::string lines;
        for (const auto& [from, to] : batch) {
            std::error_code in_way;
            if (fs::exists(to, in_way)) {
                if (error) *error = "cannot move " + from.string() + " to " + to.string() + ": something is there";
                return false;
            }
            lines += "move\t" + journal_path(data_dir_, from) + "\t" + journal_path(data_dir_, to) + "\n";
        }
        if (batch.empty()) return true;
        if (!write(lines)) {
            if (error) *error = "cannot write the add-ons' journal";
            return false;
        }
        for (const auto& [from, to] : batch) {
            std::error_code ec;
            fs::create_directories(to.parent_path(), ec);
            platform::replace_file_synced(from, to, ec);
            if (ec) {
                if (error) *error = "cannot move " + from.string() + " to " + to.string() + ": " + ec.message();
                return false;
            }
        }
        return true;
    }
    // Saves installed.txt as it is to installed.prev.txt, then writes that to the journal: an install cut off after
    // installed.txt is rewritten puts the old one back with the old files. "none" when there was no installed.txt.
    bool registry(std::string* error) {
        const fs::path dir = addons_dir(data_dir_);
        std::error_code ec;
        const bool had = fs::exists(dir / "installed.txt", ec);
        bool saved = true;
        if (had) {
            auto text = read_text_file(dir / "installed.txt");
            saved = text && write_file_atomic(dir / "installed.prev.txt", *text);
        } else {
            fs::remove(dir / "installed.prev.txt", ec);
        }
        if (saved) saved = write(std::string("registry\t") + (had ? "saved" : "none") + "\n");
        if (!saved) {
            if (error) *error = "cannot save addons/installed.txt before changing it";
            return false;
        }
        return true;
    }
    void done() { write("done\n"); }
    void close() {}

private:
    bool write(const std::string& line) {
        ok_ = ok_ && platform::append_file_synced(path_, line);
        return ok_;
    }

    fs::path data_dir_;
    fs::path path_;
    bool ok_ = false;
};

void remove_quietly(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec) log::warn("Add-ons: cannot remove {}: {}", path.string(), ec.message());
}

// Undoes the journal's moves, last first, and puts back installed.txt as it was when the journal saved it.
void undo_journal(const fs::path& data_dir) {
    const fs::path dir = addons_dir(data_dir);
    auto text = read_text_file(dir / "journal.txt");
    if (!text) return;
    std::vector<std::pair<std::string, std::string>> moves;
    std::string registry; // "saved", "none" or empty (installed.txt was not changed)
    std::istringstream in(*text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "registry\tsaved" || line == "registry\tnone") {
            registry = line.substr(9);
            continue;
        }
        const auto t1 = line.find('\t');
        const auto t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
        if (line.substr(0, t1) != "move" || t2 == std::string::npos) continue;
        std::string from = line.substr(t1 + 1, t2 - t1 - 1), to = line.substr(t2 + 1);
        // Only paths inside the data folder: the journal is the client's own file, but a damaged one moves nothing
        // elsewhere.
        if (!is_safe_zip_path(from) || !is_safe_zip_path(to)) continue;
        moves.emplace_back(std::move(from), std::move(to));
    }
    std::reverse(moves.begin(), moves.end());
    for (const auto& [from, to] : moves) {
        std::error_code ec;
        if (!fs::exists(data_dir / rel(to), ec)) continue;
        fs::create_directories((data_dir / rel(from)).parent_path(), ec);
        fs::remove(data_dir / rel(from), ec); // a half-moved file in the way
        fs::rename(data_dir / rel(to), data_dir / rel(from), ec);
        if (ec) log::warn("Add-ons: undoing a cut-off install: cannot move {} back: {}", to, ec.message());
    }
    std::error_code ec;
    if (registry == "saved" && fs::exists(dir / "installed.prev.txt", ec)) {
        fs::rename(dir / "installed.prev.txt", dir / "installed.txt", ec);
        if (ec) log::warn("Add-ons: undoing a cut-off install: cannot put installed.txt back: {}", ec.message());
    } else if (registry == "none") {
        fs::remove(dir / "installed.txt", ec);
    }
    fs::remove(dir / "installed.prev.txt", ec);
    if (!registry.empty()) {
        // The backup follows installed.txt back (it was written with the installed.txt the undo takes out).
        if (auto back = read_text_file(dir / "installed.txt")) {
            if (!write_file_atomic(dir / kRegistryBackup, *back))
                log::warn("Add-ons: undoing a cut-off install: cannot write addons/{}", kRegistryBackup);
        } else {
            fs::remove(dir / kRegistryBackup, ec);
        }
    }
}

bool holds_a_file(const fs::path& folder) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (!it->is_directory(ec)) return true;
    return false;
}

bool journal_done(const fs::path& data_dir) {
    auto text = read_text_file(addons_dir(data_dir) / "journal.txt");
    return text && (text->ends_with("done\n") || text->ends_with("done\r\n"));
}

// Removes the empty folders under `root` (and `root` itself when `self`).
void remove_empty_dirs(const fs::path& root, bool self) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    std::vector<fs::path> dirs;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_directory(ec)) dirs.push_back(it->path());
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return a.native().size() > b.native().size();
    });
    for (const auto& d : dirs)
        if (fs::is_empty(d, ec)) fs::remove(d, ec);
    if (self && fs::is_empty(root, ec)) fs::remove(root, ec);
}

// Removes `dir` while it is empty, then its parent the same way, up to `stop` (which stays).
void remove_empty_parents(fs::path dir, const fs::path& stop) {
    std::error_code ec;
    const fs::path top = stop.lexically_normal();
    for (dir = dir.lexically_normal(); dir != top && dir.has_relative_path(); dir = dir.parent_path()) {
        const fs::path inside = dir.lexically_relative(top);
        if (inside.empty() || *inside.begin() == "..") return;
        if (!fs::is_directory(dir, ec) || !fs::is_empty(dir, ec)) return;
        if (!fs::remove(dir, ec)) return;
    }
}

std::string file_of(std::string_view path) {
    const auto slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string extension_of(std::string_view path) {
    const std::string name = lower(file_of(path));
    const auto dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : name.substr(dot);
}

} // namespace

fs::path addons_dir(const fs::path& data_dir) { return data_dir / "addons"; }

namespace {
std::string_view kind_folder(AddonKind kind) {
    return kind == AddonKind::Script     ? "scripts"
           : kind == AddonKind::Plugin   ? "plugins"
           : kind == AddonKind::Tool     ? "tools"
           : kind == AddonKind::Upscaler ? "upscalers"
                                         : "ui-addons";
}
} // namespace

fs::path addon_folder(const fs::path& data_dir, AddonKind kind, std::string_view name) {
    return data_dir / rel(kind_folder(kind)) / rel(name);
}

std::string_view addon_kind_word(AddonKind kind) {
    return kind == AddonKind::Script     ? "script"
           : kind == AddonKind::Plugin   ? "plugin"
           : kind == AddonKind::Tool     ? "tool"
           : kind == AddonKind::Upscaler ? "upscaler"
                                         : "ui";
}

std::string addon_tool_program(std::string_view name, std::string_view platform) {
    return std::string(name) + (platform.starts_with("windows-") ? ".exe" : "");
}

namespace {

constexpr std::string_view kRegistryFamily = "hailstone-addons ";

// One registry file as read: its records that read whole, and the add-ons it names whose record does not.
struct ParsedRegistry {
    bool exists = false;
    bool header = false; // this version's header: the records below were read
    bool newer = false;  // another version's header ("hailstone-addons 2"): a newer Hailstone's, not read
    std::vector<InstalledAddon> good;
    std::vector<UnreadableAddon> bad;
};

void add_unreadable(std::vector<UnreadableAddon>& list, std::string_view name, std::optional<AddonKind> kind) {
    for (const auto& u : list)
        if (iequal(u.name, name) && u.kind == kind) return;
    list.push_back({std::string(name), kind});
}

std::optional<AddonKind> kind_of_word(std::string_view w) {
    if (w == "script") return AddonKind::Script;
    if (w == "plugin") return AddonKind::Plugin;
    if (w == "tool") return AddonKind::Tool;
    if (w == "upscaler") return AddonKind::Upscaler;
    if (w == "ui") return AddonKind::Ui;
    return std::nullopt;
}

ParsedRegistry parse_registry(const fs::path& file) {
    ParsedRegistry r;
    auto text = read_text_file(file);
    if (!text) return r;
    r.exists = true;
    std::istringstream in(*text);
    bool first = true, bad = false; // bad: the current record has a line that does not read
    bool skipping = false;          // the current record is not kept (its lines are another add-on's)
    auto close = [&] {
        if (bad && !r.good.empty()) {
            add_unreadable(r.bad, r.good.back().name, r.good.back().kind);
            r.good.pop_back();
        }
        bad = false;
    };
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (first) {
            first = false;
            r.header = line == kRegistryHeader;
            r.newer = !r.header && line.starts_with(kRegistryFamily);
            if (r.header) continue;
        }
        if (!r.header) {
            // Not a registry this reads (damaged, or a newer Hailstone's after an update was rolled back): its
            // "addon <name> <kind>" lines are still looked for, so that those add-ons' files get no capabilities
            // rather than a hand-installed file's (every one, on a server without hailstone.cfg).
            auto [what, rest] = split(line);
            if (what != "addon") continue;
            auto [name, kind] = split(rest);
            auto [kind_word, more] = split(kind);
            if (addon_name_ok(name)) add_unreadable(r.bad, name, kind_of_word(kind_word));
            continue;
        }
        auto [what, rest] = split(line);
        if (what == "addon") {
            close();
            auto [name, kind] = split(rest);
            const auto k = kind_of_word(kind);
            skipping = !addon_name_ok(name) || !k;
            // A name no folder can have names nothing to refuse; a known name of an unknown kind is refused in
            // every kind's folder.
            if (addon_name_ok(name) && !k) add_unreadable(r.bad, name, std::nullopt);
            if (skipping) continue;
            InstalledAddon a;
            a.name = std::string(name);
            a.kind = *k;
            r.good.push_back(std::move(a));
            continue;
        }
        if (skipping || r.good.empty()) continue;
        InstalledAddon& a = r.good.back();
        if (what == "source") a.source = std::string(rest);
        else if (what == "version") a.version = std::string(rest);
        else if (what == "caps") a.caps = plugin::parse_caps(rest);
        else if (what == "key") {
            a.key = hex_decode<32>(rest);
            bad = bad || !a.key;
        } else if (what == "autoupdate") a.auto_update = rest == "1";
        else if (what == "autoload" || what == "enabled") {
            if (rest != "0" && rest != "1") bad = true;
            else if (what == "autoload") a.autoload = rest == "1";
            else a.enabled = rest == "1";
        }
        // ("auto", up to 0.7.0-beta, was written as 1 for every install without asking the player: it is read as off)
        else if (what == "author") a.author = std::string(rest);
        else if (what == "description") a.description = std::string(rest);
        else if (what == "notes") a.notes = std::string(rest);
        else if (what == "file") {
            auto [hash, rest2] = split(rest);
            auto [size, path] = split(rest2);
            AddonFile f;
            auto digest = parse_sha256(hash);
            auto [end, ec] = std::from_chars(size.data(), size.data() + size.size(), f.size);
            if (!digest || ec != std::errc{} || end != size.data() + size.size() || !is_safe_zip_path(path)) {
                bad = true;
                continue;
            }
            f.sha256 = *digest;
            f.path = std::string(path);
            a.files.push_back(std::move(f));
        }
    }
    close();
    return r;
}

bool named(const std::vector<InstalledAddon>& list, std::string_view name) { return find_addon(list, name) != nullptr; }

} // namespace

AddonRegistry read_addon_registry(const fs::path& data_dir) {
    const fs::path dir = addons_dir(data_dir);
    AddonRegistry reg;
    ParsedRegistry p = parse_registry(dir / "installed.txt");
    if (p.exists && p.header && p.bad.empty()) {
        reg.addons = std::move(p.good);
        return reg;
    }
    ParsedRegistry b = parse_registry(dir / kRegistryBackup);
    if (!p.exists && !b.exists) return reg; // no add-on was ever installed
    reg.newer = p.newer || (!p.exists && b.newer);
    if (!p.exists) reg.problem = "addons/installed.txt is gone";
    else if (p.newer) reg.problem = "addons/installed.txt is a newer Hailstone's";
    else if (!p.header) reg.problem = "addons/installed.txt is damaged";
    else reg.problem = "addons/installed.txt has records that do not read";
    if (!p.header) {
        // Nothing of installed.txt reads: the backup's records stand in, and what installed.txt names beyond them
        // gets nothing.
        if (b.header) {
            reg.addons = std::move(b.good);
            reg.from_backup = true;
            for (const auto& u : b.bad) add_unreadable(reg.unreadable, u.name, u.kind);
        } else {
            for (const auto& u : b.bad) add_unreadable(reg.unreadable, u.name, u.kind);
        }
        for (const auto& u : p.bad)
            if (!named(reg.addons, u.name)) add_unreadable(reg.unreadable, u.name, u.kind);
    } else {
        // Only some records of installed.txt are damaged: each is taken from the backup when it reads there.
        reg.addons = std::move(p.good);
        for (const auto& u : p.bad) {
            const InstalledAddon* saved = b.header ? find_addon(b.good, u.name) : nullptr;
            if (saved && !named(reg.addons, u.name) && (!u.kind || saved->kind == *u.kind)) {
                reg.addons.push_back(*saved);
                reg.from_backup = true;
            } else {
                add_unreadable(reg.unreadable, u.name, u.kind);
            }
        }
        std::sort(reg.addons.begin(), reg.addons.end(),
                  [](const auto& x, const auto& y) { return lower(x.name) < lower(y.name); });
    }
    return reg;
}

std::vector<InstalledAddon> read_installed_addons(const fs::path& data_dir) {
    return read_addon_registry(data_dir).addons;
}

std::vector<mq::ManagedScript> managed_addon_scripts(const fs::path& data_dir) {
    std::vector<mq::ManagedScript> out;
    for (const auto& addon : read_installed_addons(data_dir)) {
        if (addon.kind != AddonKind::Script || !addon.autoload) continue;
        mq::ManagedScript script;
        script.name = lower(addon.name);
        script.entry = addon_folder(data_dir, addon.kind, addon.name) / "init.lua";
        script.enabled = addon.enabled;
        bool have_entry = false;
        std::string revision = addon.version + "\n" + std::to_string(addon.caps) + "\n";
        for (const auto& file : addon.files) {
            if (file.path == "init.lua") { script.hash = file.sha256; have_entry = true; }
            revision += file.path + " " + to_hex(file.sha256) + "\n";
        }
        script.revision = sha256(revision);
        script.verified = have_entry && modified_addon_files(data_dir, addon).empty();
        out.push_back(std::move(script));
    }
    return out;
}

bool set_addon_enabled(const fs::path& data_dir, std::string_view name, bool enabled) {
    auto registry = read_addon_registry(data_dir);
    if (!registry.writable()) return false;
    for (auto& addon : registry.addons) {
        if (!iequal(addon.name, name) || addon.kind != AddonKind::Script || !addon.autoload) continue;
        addon.enabled = enabled;
        return write_installed_addons(data_dir, registry.addons);
    }
    return false;
}

namespace {
std::string registry_text(const std::vector<InstalledAddon>& addons) {
    std::string text = "# The add-ons Hailstone installed (docs/addons.md). Written by the client.\n";
    text += kRegistryHeader;
    text += "\n";
    for (const auto& a : addons) {
        text += "\naddon " + a.name + " " + std::string(addon_kind_word(a.kind)) + "\n";
        text += "source " + a.source + "\n";
        text += "version " + a.version + "\n";
        text += "caps " + plugin::caps_text(a.caps) + "\n";
        if (a.key) text += "key " + hex_encode(*a.key) + "\n";
        text += std::string("autoupdate ") + (a.auto_update ? "1" : "0") + "\n";
        text += std::string("autoload ") + (a.autoload ? "1" : "0") + "\n";
        text += std::string("enabled ") + (a.enabled ? "1" : "0") + "\n";
        if (!a.author.empty()) text += "author " + a.author + "\n";
        if (!a.description.empty()) text += "description " + a.description + "\n";
        if (!a.notes.empty()) text += "notes " + a.notes + "\n";
        for (const auto& f : a.files) text += "file " + to_hex(f.sha256) + " " + std::to_string(f.size) + " " + f.path + "\n";
    }
    return text;
}

// Why installed.txt may not be written now ("" it may): a newer Hailstone's registry is never written over, and one
// whose records do not all read would lose those (their files would then get a hand-installed file's capabilities).
std::string registry_locked(const AddonRegistry& reg) {
    if (reg.writable()) return {};
    return reg.problem + ": add-ons cannot be installed, updated or removed until it is put right (docs/addons.md)";
}
} // namespace

bool write_installed_addons(const fs::path& data_dir, const std::vector<InstalledAddon>& addons) {
    if (std::string why = registry_locked(read_addon_registry(data_dir)); !why.empty()) {
        log::warn("Add-ons: {}", why);
        return false;
    }
    const std::string text = registry_text(addons);
    if (!write_file_atomic(addons_dir(data_dir) / "installed.txt", text)) return false;
    // The copy a lost or damaged installed.txt is read from (read_addon_registry).
    if (!write_file_atomic(addons_dir(data_dir) / kRegistryBackup, text))
        log::warn("Add-ons: cannot write addons/{}", kRegistryBackup);
    return true;
}

const InstalledAddon* find_addon(const std::vector<InstalledAddon>& addons, std::string_view name) {
    for (const auto& a : addons)
        if (iequal(a.name, name)) return &a;
    return nullptr;
}

const InstalledAddon* find_pinned_addon(const std::vector<InstalledAddon>& addons, std::string_view name,
                                        AddonKind kind, std::string_view repo, const Ed25519PublicKey& key) {
    const InstalledAddon* a = find_addon(addons, name);
    if (!a || a->kind != kind || !a->key || *a->key != key) return nullptr;
    auto have = parse_addon_link(a->source), want = parse_addon_link(repo);
    if (!have || !want || !iequal(have->owner, want->owner) || !iequal(have->repo, want->repo)) return nullptr;
    return a;
}

namespace {
// addon_file_sha256's: by the file's path, its size and modification time then, and its SHA-256.
struct KnownHash {
    std::uint64_t size = 0;
    fs::file_time_type::rep stamp{};
    Sha256Digest sha256{};
};
std::mutex g_known_hashes_mutex;
std::map<std::u8string, KnownHash> g_known_hashes;
constexpr std::size_t kKnownHashesMax = 16384; // a whole add-on library many times over; emptied when reached
} // namespace

std::optional<Sha256Digest> addon_file_sha256(const fs::path& file) {
    std::error_code ec;
    const std::uint64_t size = fs::file_size(file, ec);
    if (ec) return std::nullopt;
    const auto time = fs::last_write_time(file, ec);
    if (ec) return std::nullopt;
    const auto stamp = time.time_since_epoch().count();
    const std::u8string key = file.lexically_normal().generic_u8string();
    // A file written in the last moments may be written again within the same tick of the file system's clock
    // (coarse on some): it is read each time until its time is safely past.
    const bool settled = fs::file_time_type::clock::now() - time > std::chrono::seconds(3);
    if (settled) {
        std::lock_guard lock(g_known_hashes_mutex);
        if (auto it = g_known_hashes.find(key); it != g_known_hashes.end() && it->second.size == size &&
                                                it->second.stamp == stamp)
            return it->second.sha256;
    }
    // Read outside the lock (the add-on manager's worker asks too). Stamped with the size and time taken before the
    // read: a file that changes while it is read has another time at the next look, and is read again.
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    Sha256 h;
    std::vector<char> buf(1 << 16);
    std::uint64_t read = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<std::size_t>(in.gcount());
        read += n;
        if (n) h.update(std::as_bytes(std::span(buf.data(), n)));
    }
    if (in.bad() || read != size) return std::nullopt;
    const Sha256Digest digest = h.finish();
    std::lock_guard lock(g_known_hashes_mutex);
    if (g_known_hashes.size() >= kKnownHashesMax) g_known_hashes.clear();
    g_known_hashes[key] = {size, stamp, digest};
    return digest;
}

bool addon_file_matches(const fs::path& file, std::uint64_t size, const Sha256Digest& sha256) {
    std::error_code ec;
    if (fs::file_size(file, ec) != size || ec) return false;
    const auto digest = addon_file_sha256(file);
    return digest && *digest == sha256;
}

std::vector<std::string> modified_addon_files(const fs::path& data_dir, const InstalledAddon& addon) {
    std::vector<std::string> out;
    const fs::path folder = addon_folder(data_dir, addon.kind, addon.name);
    for (const auto& f : addon.files)
        if (!addon_file_matches(folder / rel(f.path), f.size, f.sha256)) out.push_back(f.path);
    return out;
}

namespace {
// Why `name` is taken in the data folder's own folder for `kind` ("" it is not): a folder there that is not empty
// (a plugin's that holds no library is only the settings a plugin left beside it), or a file of that name.
std::string taken_on_disk(const fs::path& data_dir, AddonKind kind, std::string_view name) {
    std::error_code ec;
    const fs::path folder = addon_folder(data_dir, kind, name);
    const std::string where = std::string(kind_folder(kind)) + "/";
    if (kind == AddonKind::Ui) {
        for (const auto& entry : fs::directory_iterator(data_dir / "ui-addons", ec))
            if (iequal(entry.path().filename().string(), name))
                return where + entry.path().filename().string() + " is there already";
        return {};
    }
    // Nothing but add-ons is put in tools/ or upscalers/ (but the example provider a build from source puts there):
    // any folder or file of the name is in the way.
    if (kind == AddonKind::Tool || kind == AddonKind::Upscaler)
        return fs::exists(folder, ec) ? where + std::string(name) + " is there already" : std::string();
    // A plugin's folder that holds no library is only the settings a plugin left beside it (host->config_dir), as
    // after an update took out a plugin an older release shipped: the install keeps them, as an update does.
    bool holds_library = kind == AddonKind::Script;
    if (kind == AddonKind::Plugin && fs::is_directory(folder, ec)) {
        const std::string_view ext = plugin_extension(update_platform());
        for (const auto& e : fs::directory_iterator(folder, ec))
            if (e.is_regular_file(ec) && (ext.empty() || iequal(e.path().extension().string(), ext)))
                holds_library = true;
    }
    if (fs::exists(folder, ec) && !(fs::is_directory(folder, ec) && (fs::is_empty(folder, ec) || !holds_library)))
        return where + std::string(name) + "/ is there already (installed by hand, or with Hailstone)";
    // A script file of that name would be found before the add-on's folder, and a plugin file of that name is the
    // same plugin.
    const fs::path parent = folder.parent_path();
    for (const auto& e : fs::directory_iterator(parent, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string file = e.path().filename().string();
        if (kind == AddonKind::Script && iequal(file, std::string(name) + ".lua"))
            return where + file + " is there already (installed by hand)";
        if (kind == AddonKind::Plugin && iequal(plugin::plugin_name(e.path()), name) &&
            !plugin_extension(update_platform()).empty() && iequal(e.path().extension().string(), plugin_extension(update_platform())))
            return where + file + " is there already (installed by hand)";
    }
    return {};
}
} // namespace

std::string addon_name_taken(const fs::path& data_dir, const std::vector<InstalledAddon>& addons, AddonKind kind,
                             std::string_view name) {
    if (!addon_name_ok(name)) return "\"" + std::string(name) + "\" is not a name an add-on can have";
    if (kind == AddonKind::Script && iequal(name, "config"))
        return "\"config\" is the scripts' settings folder (scripts/config/)";
    if (const InstalledAddon* a = find_addon(addons, name))
        return "the add-on \"" + a->name + "\" is installed already";
    if (std::string why = taken_on_disk(data_dir, kind, name); !why.empty()) return why;
    if (kind == AddonKind::Tool || kind == AddonKind::Upscaler || kind == AddonKind::Ui) return {};
    // Nor a script and a compiled plugin of one name: their windows are told apart by kind
    // (plugin::UiBridge::owner_tag), but the player would not tell them apart.
    const AddonKind other = kind == AddonKind::Script ? AddonKind::Plugin : AddonKind::Script;
    if (std::string why = taken_on_disk(data_dir, other, name); !why.empty())
        return why + ": a script and a plugin may not share a name";
    return {};
}

bool addon_owns_folder(const fs::path& data_dir, std::string_view folder) {
    const AddonRegistry reg = read_addon_registry(data_dir);
    for (const auto& a : reg.addons) {
        const std::string mine = std::string(kind_folder(a.kind)) + "/" + a.name;
        if (iequal(mine, folder)) return true;
    }
    // One whose record does not read is still taken for an add-on's: the updater leaves it alone.
    for (const auto& u : reg.unreadable)
        for (AddonKind k : {AddonKind::Script, AddonKind::Plugin, AddonKind::Tool, AddonKind::Upscaler, AddonKind::Ui})
            if ((!u.kind || *u.kind == k) && iequal(std::string(kind_folder(k)) + "/" + u.name, folder)) return true;
    return false;
}

bool lua_compiles(std::string_view text, std::string_view chunk_name, std::string* error) {
    lua_State* L = luaL_newstate();
    if (!L) {
        if (error) *error = "out of memory";
        return false;
    }
    const std::string chunk = "=" + std::string(chunk_name);
    const bool ok = luaL_loadbufferx(L, text.data(), text.size(), chunk.c_str(), "t") == LUA_OK;
    if (!ok && error) {
        const char* msg = lua_tostring(L, -1);
        *error = msg ? msg : "does not compile";
    }
    lua_close(L);
    return ok;
}

namespace {

// The bytes of a zip entry of at most `limit` bytes, or nullopt with *error why not.
std::optional<std::string> read_entry(ZipReader& zip, const ZipEntry& e, std::uint64_t limit, std::string* error) {
    if (e.size > limit) {
        if (error) *error = e.name + " is too large";
        return std::nullopt;
    }
    std::string bytes;
    bytes.reserve(e.size);
    if (!zip.extract(e, [&](std::span<const std::byte> piece) {
            bytes.append(reinterpret_cast<const char*>(piece.data()), piece.size());
            return true;
        })) {
        if (error) *error = e.name + ": " + zip.error();
        return std::nullopt;
    }
    return bytes;
}

// Writes `files` into addons/new/<name>/, each hashed as it is written; `runnable` are made runnable (not on
// Windows, which has no such bit).
std::optional<std::vector<AddonFile>> write_unpacked(const fs::path& data_dir, std::string_view name, ZipReader& zip,
                                                     const std::vector<const ZipEntry*>& files,
                                                     [[maybe_unused]] const std::vector<const ZipEntry*>& runnable,
                                                     std::string* error) {
    const fs::path dir = addons_dir(data_dir) / "new" / rel(name);
    remove_quietly(dir);
    std::vector<AddonFile> out;
    for (const ZipEntry* e : files) {
        const fs::path to = dir / rel(e->name);
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        std::ofstream file(to, std::ios::binary | std::ios::trunc);
        if (!file) {
            if (error) *error = "cannot write " + to.string();
            return std::nullopt;
        }
        Sha256 h;
        AddonFile f;
        f.path = e->name;
        f.size = e->size;
        if (!zip.extract(*e, [&](std::span<const std::byte> piece) {
                h.update(piece);
                file.write(reinterpret_cast<const char*>(piece.data()), static_cast<std::streamsize>(piece.size()));
                return static_cast<bool>(file);
            })) {
            file.close();
            remove_quietly(dir);
            if (error) *error = e->name + ": " + zip.error();
            return std::nullopt;
        }
        file.close();
        // On the disk before the journal's moves put it in place: a power cut must not leave an installed add-on's
        // file empty (platform/durable_file.h).
        if (!file || !platform::sync_file(to)) {
            remove_quietly(dir);
            if (error) *error = "cannot write " + to.string();
            return std::nullopt;
        }
#ifndef _WIN32
        if (std::find(runnable.begin(), runnable.end(), e) != runnable.end())
            fs::permissions(to, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, ec);
#endif
        f.sha256 = h.finish();
        out.push_back(std::move(f));
    }
    return out;
}

// A library file name of `platform`'s: <x>.dll, <x>.dylib, <x>.so or a versioned <x>.so.<n>[.<n>...].
bool library_name(std::string_view lower_name, std::string_view platform) {
    const std::string_view ext = plugin_extension(platform);
    if (ext.empty()) return false;
    if (lower_name.size() > ext.size() && lower_name.ends_with(ext)) return true;
    if (ext != ".so") return false;
    const auto at = lower_name.find(".so.");
    if (at == std::string_view::npos || at == 0) return false;
    for (char c : lower_name.substr(at + 4))
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    return true;
}

// Every name a tool's package may not hold, whatever its place: what runs on its own or through a shell (a program
// other than the tool's, a script for a system interpreter). Lua is the tool's own business: it runs nowhere else.
constexpr std::string_view kToolRefused[] = {".exe", ".com", ".bat", ".cmd", ".ps1", ".sh",  ".asi", ".flt",
                                             ".py",  ".js",  ".vbs", ".scr", ".msi", ".jar", ".app"};

std::optional<std::vector<AddonFile>> unpack_tool(const fs::path& data_dir, const AddonManifest& m, ZipReader& zip,
                                                  std::string_view platform, std::string* error) {
    auto fail = [&](std::string why) -> std::optional<std::vector<AddonFile>> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };
    const std::string program_name = addon_tool_program(m.name, platform);
    std::vector<const ZipEntry*> files, libraries;
    std::map<std::string, std::string> seen;
    std::uint64_t total = 0;
    const ZipEntry* program = nullptr;
    for (const auto& e : zip.entries()) {
        if (e.is_directory()) continue;
        if (files.size() >= kMaxFiles) return fail("the package holds too many files");
        total += e.size;
        if (total > kMaxPluginBytes) return fail("the package is too large");
        if (!net::is_safe_file_path_shape(e.name)) return fail(e.name + " is not a name a tool may hold");
        if (!seen.emplace(lower(e.name), e.name).second) return fail(e.name + " is in the package twice");
        const bool top = e.name.find('/') == std::string::npos;
        const std::string name = lower(file_of(e.name));
        if (top && name == lower(program_name)) {
            if (e.name != program_name) return fail(e.name + ": the program is named " + program_name);
            program = &e;
        } else if (library_name(name, platform)) {
            if (!top) return fail(e.name + ": a tool's libraries go at the top of its package, beside its program");
            libraries.push_back(&e);
        } else if (library_name(name, "linux-x64") || library_name(name, "windows-x64") ||
                   library_name(name, "macos-x64")) {
            return fail(e.name + " is a library for another platform");
        } else {
            for (std::string_view ext : kToolRefused)
                if (name.size() > ext.size() && name.ends_with(ext))
                    return fail(e.name + " is not something a tool may hold (one program, named after it, and its "
                                         "libraries for " + std::string(platform) + ")");
        }
        files.push_back(&e);
    }
    if (!program) return fail("the package has no program for " + std::string(platform) + " (" + program_name + ")");

    // The checks that read a file's contents: each Lua file compiles, the program runs here, each library loads here.
    for (const ZipEntry* e : files) {
        const bool lua = extension_of(e->name) == ".lua";
        const bool lib = std::find(libraries.begin(), libraries.end(), e) != libraries.end();
        if (!lua && !lib && e != program) continue;
        auto bytes = read_entry(zip, *e, lua ? kMaxLuaFile : kMaxLibrary, error);
        if (!bytes) return std::nullopt;
        if (lua) {
            std::string why;
            if (!lua_compiles(*bytes, m.name + "/" + e->name, &why)) return fail(e->name + " does not compile: " + why);
            continue;
        }
        const BinaryHeader h = read_binary_header(std::as_bytes(std::span(bytes->data(), bytes->size())));
        if (e == program ? !program_fits_platform(h, platform) : !binary_fits_platform(h, platform))
            return fail(e->name + " is not a " + std::string(e == program ? "program" : "library") + " for " +
                        std::string(platform));
    }
    return write_unpacked(data_dir, m.name, zip, files, {program}, error);
}

// An upscaler's package (docs/addons.md "Upscalers"): at its top its provider, a library for this platform named after
// it, and the libraries it loads (the vendor's runtime) beside it; data anywhere (licences, a vendor's notices).
// Nothing that runs on its own, and no library in a folder or for another platform.
std::optional<std::vector<AddonFile>> unpack_upscaler(const fs::path& data_dir, const AddonManifest& m, ZipReader& zip,
                                                      std::string_view platform, std::string* error) {
    auto fail = [&](std::string why) -> std::optional<std::vector<AddonFile>> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };
    const std::string provider_name = m.name + std::string(plugin_extension(platform));
    std::vector<const ZipEntry*> files, libraries;
    std::map<std::string, std::string> seen;
    std::uint64_t total = 0;
    const ZipEntry* provider = nullptr;
    for (const auto& e : zip.entries()) {
        if (e.is_directory()) continue;
        if (files.size() >= kMaxFiles) return fail("the package holds too many files");
        total += e.size;
        if (total > kMaxPluginBytes) return fail("the package is too large");
        if (!net::is_safe_file_path_shape(e.name)) return fail(e.name + " is not a name an upscaler may hold");
        if (!seen.emplace(lower(e.name), e.name).second) return fail(e.name + " is in the package twice");
        const bool top = e.name.find('/') == std::string::npos;
        const std::string name = lower(file_of(e.name));
        if (library_name(name, platform)) {
            if (!top) return fail(e.name + ": an upscaler's libraries go at the top of its package");
            if (name == lower(provider_name)) {
                if (e.name != provider_name) return fail(e.name + ": the provider is named " + provider_name);
                provider = &e;
            }
            libraries.push_back(&e);
        } else if (library_name(name, "linux-x64") || library_name(name, "windows-x64") ||
                   library_name(name, "macos-x64")) {
            return fail(e.name + " is a library for another platform");
        } else {
            for (std::string_view ext : kToolRefused)
                if (name.size() > ext.size() && name.ends_with(ext))
                    return fail(e.name + " is not something an upscaler may hold (its provider, named after it, and "
                                         "the libraries it loads, for " + std::string(platform) + ")");
        }
        files.push_back(&e);
    }
    if (!provider) return fail("the package has no provider for " + std::string(platform) + " (" + provider_name + ")");
    // Each library must be one this platform loads.
    for (const ZipEntry* e : libraries) {
        auto bytes = read_entry(zip, *e, kMaxLibrary, error);
        if (!bytes) return std::nullopt;
        const BinaryHeader h = read_binary_header(std::as_bytes(std::span(bytes->data(), bytes->size())));
        if (!binary_fits_platform(h, platform)) return fail(e->name + " is not a library for " + std::string(platform));
    }
    return write_unpacked(data_dir, m.name, zip, files, libraries, error);
}


std::optional<std::vector<AddonFile>> unpack_ui(const fs::path& data_dir, const AddonManifest& m, ZipReader& zip,
                                               std::string* error) {
    auto fail = [&](std::string why) -> std::optional<std::vector<AddonFile>> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };
    std::vector<const ZipEntry*> files;
    std::map<std::string, std::string> seen, packs;
    std::map<std::string, std::pair<std::string, bool>> paths;
    std::set<std::string> manifests;
    std::uint64_t total = 0;
    for (const auto& entry : zip.entries()) {
        if (entry.is_directory()) continue;
        if (files.size() >= kMaxFiles) return fail("the UI package holds too many files");
        total += entry.size;
        if (total > kMaxScriptBytes) return fail("the UI package is too large");
        if (!net::is_safe_file_path_shape(entry.name)) return fail(entry.name + ": unsafe UI package path");
        if (!seen.emplace(lower(entry.name), entry.name).second) return fail(entry.name + " is in the package twice");
        // Portable bundles use one spelling and one file/directory role for each path component.
        for (std::size_t end = 0;;) {
            end = entry.name.find('/', end);
            const bool file = end == std::string::npos;
            const std::string prefix = entry.name.substr(0, end);
            const auto [at, inserted] = paths.emplace(lower(prefix), std::pair{prefix, file});
            if (!inserted && at->second.first != prefix)
                return fail(entry.name + ": UI path components differ only in case");
            if (!inserted && at->second.second != file)
                return fail(entry.name + ": a UI path is both a file and a directory");
            if (file) break;
            ++end;
        }
        const auto slash = entry.name.find('/', 3);
        const bool notice = entry.name == "LICENSE" || entry.name == "NOTICE" || entry.name == "NOTICE.md" ||
                            entry.name == "README.md";
        if (!notice) {
            if (!entry.name.starts_with("ui/") || slash == std::string::npos)
                return fail(entry.name + ": UI files go in ui/<pack>/");
            const std::string pack = entry.name.substr(3, slash - 3);
            if (!addon_name_ok(pack)) return fail(entry.name + ": invalid UI pack name");
            const auto [found, inserted] = packs.emplace(lower(pack), pack);
            if (!inserted && found->second != pack) return fail(entry.name + ": UI pack folders differ only in case");
            if (entry.name.substr(slash + 1) == "ui.ini") {
                if (!entry.size || entry.size > 64 * 1024) return fail(entry.name + ": invalid UI manifest size");
                manifests.insert(lower(pack));
            }
        }
        const std::string ext = extension_of(entry.name), name = lower(file_of(entry.name));
        if (std::find(std::begin(kToolRefused), std::end(kToolRefused), ext) != std::end(kToolRefused) ||
            ext == ".dll" || ext == ".dylib" || ext == ".so" || name.find(".so.") != std::string::npos)
            return fail(entry.name + ": a UI package holds presentation Lua and assets, not programs");
        if (ext == ".lua") {
            auto bytes = read_entry(zip, entry, kMaxLuaFile, error);
            if (!bytes) return std::nullopt;
            std::string why;
            if (!lua_compiles(*bytes, m.name + "/" + entry.name, &why))
                return fail(entry.name + " does not compile: " + why);
        }
        files.push_back(&entry);
    }
    if (packs.empty() || manifests.size() != packs.size())
        return fail("every UI pack has ui/<pack>/ui.ini");
    return write_unpacked(data_dir, m.name, zip, files, {}, error);
}

} // namespace

std::optional<std::vector<AddonFile>> unpack_addon(const fs::path& data_dir, const AddonManifest& m,
                                                   const fs::path& zip_path, std::string_view platform,
                                                   std::string* error) {
    auto fail = [&](std::string why) -> std::optional<std::vector<AddonFile>> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };
    ZipReader zip;
    if (!zip.open(zip_path)) return fail("the package is not a zip this reads: " + zip.error());
    if (m.kind == AddonKind::Tool) return unpack_tool(data_dir, m, zip, platform, error);
    if (m.kind == AddonKind::Upscaler) return unpack_upscaler(data_dir, m, zip, platform, error);
    if (m.kind == AddonKind::Ui) return unpack_ui(data_dir, m, zip, error);
    const bool script = m.kind == AddonKind::Script;
    const std::string top = std::string(script ? "scripts/" : "plugins/") + m.name + "/";
    const std::string_view lib_ext = plugin_extension(platform);
    const std::uint64_t max_bytes = script ? kMaxScriptBytes : kMaxPluginBytes;

    // First the whole list, before anything is written.
    std::vector<const ZipEntry*> files;
    std::map<std::string, std::string> seen; // lower-case path -> as written: names only case apart are one file on
                                             // Windows and macOS
    std::uint64_t total = 0;
    const ZipEntry* library = nullptr;
    bool init = false;
    for (const auto& e : zip.entries()) {
        if (e.is_directory()) continue;
        if (files.size() >= kMaxFiles) return fail("the package holds too many files");
        total += e.size;
        if (total > max_bytes) return fail("the package is too large");
        const std::string full = top + e.name;
        bool other_platform = false;
        const auto dest = net::server_file_destination(full, &other_platform);
        if (!dest || *dest != full) {
            if (other_platform) return fail(e.name + " is for another platform");
            return fail(e.name + " is not something " + std::string(script ? "a script" : "a plugin") + " may hold" +
                        (script ? "" : " (Lua goes in ui/<pack>/, programs are refused)"));
        }
        if (!seen.emplace(lower(e.name), e.name).second) return fail(e.name + " is in the package twice");
        const std::string ext = extension_of(e.name);
        if (!script && !lib_ext.empty() && ext == lib_ext) {
            if (e.name.find('/') != std::string::npos)
                return fail(e.name + ": the plugin's library goes at the top of its package");
            if (library) return fail("the package holds more than one library");
            if (!iequal(plugin::plugin_name(rel(e.name)), m.name))
                return fail(e.name + ": the library is named after the plugin (" + m.name + std::string(lib_ext) + ")");
            library = &e;
        }
        if (script && iequal(e.name, "init.lua")) init = true;
        files.push_back(&e);
    }
    if (script && !init) return fail("a script's package has init.lua at its top");
    if (!script && !library) return fail("the package has no library for " + std::string(platform));

    // The checks that read a file's contents: each Lua file compiles, and the library is one this platform loads.
    for (const ZipEntry* e : files) {
        const bool lua = extension_of(e->name) == ".lua";
        if (!lua && e != library) continue;
        auto bytes = read_entry(zip, *e, lua ? kMaxLuaFile : kMaxLibrary, error);
        if (!bytes) return std::nullopt;
        if (lua) {
            std::string why;
            if (!lua_compiles(*bytes, m.name + "/" + e->name, &why)) return fail(e->name + " does not compile: " + why);
        } else {
            const BinaryHeader h = read_binary_header(std::as_bytes(std::span(bytes->data(), bytes->size())));
            if (!binary_fits_platform(h, platform)) return fail(e->name + " is not a library for " + std::string(platform));
        }
    }

    // Then out into new/<name>/, each file hashed as it is written; a plugin's library as the loader wants it, and
    // nothing else made runnable.
    std::vector<const ZipEntry*> runnable;
    if (library) runnable.push_back(library);
    return write_unpacked(data_dir, m.name, zip, files, runnable, error);
}

bool apply_addon(const fs::path& data_dir, const InstalledAddon& addon, std::string* error) {
    const fs::path dir = addons_dir(data_dir);
    const fs::path src = dir / "new" / rel(addon.name);
    const fs::path prev = dir / "previous" / rel(addon.name);
    const fs::path target = addon_folder(data_dir, addon.kind, addon.name);
    const AddonRegistry reg = read_addon_registry(data_dir);
    if (std::string why = registry_locked(reg); !why.empty()) {
        if (error) *error = why;
        return false;
    }
    std::vector<InstalledAddon> all = reg.addons;
    const InstalledAddon* old = find_addon(all, addon.name);
    if (old && old->kind != addon.kind) {
        if (error) *error = "an add-on cannot change its kind (" + std::string(addon_kind_word(old->kind)) + " to " +
                            std::string(addon_kind_word(addon.kind)) + ")";
        return false;
    }
    std::error_code ec;
    for (const auto& f : addon.files)
        if (!fs::is_regular_file(src / rel(f.path), ec)) {
            if (error) *error = "the unpacked package is gone: download it again";
            return false;
        }
    remove_quietly(prev);
    fs::create_directories(dir, ec);
    Journal journal(data_dir);
    if (!journal.ok()) {
        if (error) *error = "cannot write the add-ons' journal";
        return false;
    }
    auto undo = [&](const std::string& why) {
        journal.close();
        undo_journal(data_dir);
        remove_quietly(dir / "journal.txt");
        remove_quietly(prev);
        log::warn("Add-ons: {} not installed: {}", addon.name, why);
        if (error) *error = why;
        return false;
    };
    // Out: every file the earlier version shipped and every file this one ships that is there now.
    std::vector<std::string> out;
    if (old)
        for (const auto& f : old->files) out.push_back(f.path);
    for (const auto& f : addon.files)
        if (std::find(out.begin(), out.end(), f.path) == out.end()) out.push_back(f.path);
    std::string why;
    // Files only: a folder that is the name of a file now (lib/util.lua before, lib now) is left, emptied, and
    // taken out below so the file can go there; one the player put files in stays, and the move into it fails.
    // The moves go in two batches, each written to the journal in one synced write before its moves (Journal::moves):
    // the old files out, then the new ones in, whose places the first batch empties.
    std::vector<std::string> moved_out;
    std::vector<std::pair<fs::path, fs::path>> batch;
    for (const auto& p : out) {
        if (!fs::exists(target / rel(p), ec) || fs::is_directory(target / rel(p), ec)) continue;
        batch.emplace_back(target / rel(p), prev / rel(p));
        moved_out.push_back(p);
    }
    if (!journal.moves(batch, &why)) return undo(why);
    // Only the folders in a new file's way: any other empty folder (one a script made for its saves, or the
    // player's) is not this install's to take.
    for (const auto& f : addon.files)
        if (fs::is_directory(target / rel(f.path), ec)) remove_empty_dirs(target / rel(f.path), true);
    batch.clear();
    for (const auto& f : addon.files) batch.emplace_back(src / rel(f.path), target / rel(f.path));
    if (!journal.moves(batch, &why)) return undo(why);
    InstalledAddon entry = addon;
    // An update keeps the player's auto-update switch as installed.txt has it now (they may have changed it while
    // the update downloaded).
    if (old) {
        entry.auto_update = old->auto_update;
        // Once installed as managed, updates retain the player's activation control until removal/reinstallation.
        // Dropping a manifest flag must not turn a disabled addon into an ordinary autorun script.
        entry.autoload = entry.autoload || old->autoload;
        // An update cannot silently activate a formerly ordinary script or undo the player's disabled switch.
        entry.enabled = entry.autoload && old->autoload && old->enabled;
    }
    std::erase_if(all, [&](const InstalledAddon& a) { return iequal(a.name, addon.name); });
    all.push_back(std::move(entry));
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return lower(a.name) < lower(b.name); });
    if (!journal.registry(&why)) return undo(why);
    if (!write_installed_addons(data_dir, all)) return undo("cannot write addons/installed.txt");
    journal.done();
    journal.close();
    remove_quietly(dir / "journal.txt");
    remove_quietly(dir / "installed.prev.txt");
    remove_quietly(prev);
    remove_quietly(src);
    // The earlier version's folders its files left empty, and only those.
    for (const auto& p : moved_out) remove_empty_parents((target / rel(p)).parent_path(), target);
    log::info("Add-ons: {} {} installed into {}", addon.name, addon.version, target.string());
    return true;
}

bool remove_addon(const fs::path& data_dir, std::string_view name, std::string* error) {
    const fs::path dir = addons_dir(data_dir);
    const AddonRegistry reg = read_addon_registry(data_dir);
    if (std::string why = registry_locked(reg); !why.empty()) {
        if (error) *error = why;
        return false;
    }
    std::vector<InstalledAddon> all = reg.addons;
    const InstalledAddon* found = find_addon(all, name);
    if (!found) {
        if (error) *error = "no add-on is named " + std::string(name);
        return false;
    }
    const InstalledAddon addon = *found;
    const fs::path prev = dir / "previous" / rel(addon.name);
    const fs::path target = addon_folder(data_dir, addon.kind, addon.name);
    remove_quietly(prev);
    Journal journal(data_dir);
    if (!journal.ok()) {
        if (error) *error = "cannot write the add-ons' journal";
        return false;
    }
    std::string why;
    std::error_code ec;
    std::vector<std::pair<fs::path, fs::path>> batch;
    for (const auto& f : addon.files)
        if (fs::exists(target / rel(f.path), ec)) batch.emplace_back(target / rel(f.path), prev / rel(f.path));
    bool ok = journal.moves(batch, &why);
    std::erase_if(all, [&](const InstalledAddon& a) { return iequal(a.name, addon.name); });
    if (ok && !journal.registry(&why)) ok = false;
    if (ok && !write_installed_addons(data_dir, all)) {
        ok = false;
        why = "cannot write addons/installed.txt";
    }
    if (!ok) {
        journal.close();
        undo_journal(data_dir);
        remove_quietly(dir / "journal.txt");
        remove_quietly(prev);
        if (error) *error = why;
        return false;
    }
    journal.done();
    journal.close();
    remove_quietly(dir / "journal.txt");
    remove_quietly(dir / "installed.prev.txt");
    remove_quietly(prev);
    remove_empty_dirs(target, true);
    log::info("Add-ons: {} removed", addon.name);
    return true;
}

namespace {
bool library_file(std::string_view path) {
    const std::string ext = extension_of(path);
    return ext == ".so" || ext == ".dll" || ext == ".dylib";
}
} // namespace

std::vector<plugin::Ceiling> addon_ceilings(const fs::path& data_dir, bool scan_for_enforcement) {
    const AddonRegistry reg = read_addon_registry(data_dir);
    std::vector<plugin::Ceiling> out = addon_ceilings(reg.addons);
    // Changed or extra code in a managed folder still belongs to that source for explicit blocks. Its bytes are
    // not manifest-verified, so it cannot satisfy a closed allowlist or a requirement. With enforcement off it
    // keeps the existing behavior of a player-edited file, rather than gaining the original manifest's ceiling.
    if (scan_for_enforcement) {
        for (const auto& a : reg.addons) {
            if (a.kind != AddonKind::Script && a.kind != AddonKind::Plugin) continue;
            const fs::path folder = addon_folder(data_dir, a.kind, a.name);
            std::error_code ec;
            if (!fs::is_directory(folder, ec)) continue;
            auto take = [&](const fs::path& file, plugin::FileKind kind) {
                const auto digest = addon_file_sha256(file);
                if (!digest) return;
                const auto hash = *digest;
                const auto relative = file.lexically_relative(folder).generic_string();
                if (std::any_of(a.files.begin(), a.files.end(), [&](const AddonFile& f) {
                        return iequal(f.path, relative) && f.sha256 == hash;
                    })) return;
                out.push_back({kind, hash, 0, a.name, a.source, false, false});
            };
            if (a.kind == AddonKind::Script) {
                for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator();
                     it.increment(ec))
                    if (it->is_regular_file(ec) && extension_of(it->path().filename().string()) == ".lua")
                        take(it->path(), plugin::FileKind::Script);
            } else {
                for (const auto& e : fs::directory_iterator(folder, ec))
                    if (e.is_regular_file(ec) && library_file(e.path().filename().string()))
                        take(e.path(), plugin::FileKind::Plugin);
            }
        }
    }
    // An add-on whose record does not read: what it was allowed is not known, so its files may do nothing (not what
    // a file put in by hand may). Every Lua file of its folder, and every library at its top, by their bytes now.
    for (const auto& u : reg.unreadable) {
        std::size_t refused = 0;
        for (AddonKind k : {AddonKind::Script, AddonKind::Plugin}) {
            if (u.kind && *u.kind != k) continue;
            const fs::path folder = addon_folder(data_dir, k, u.name);
            std::error_code ec;
            if (!fs::is_directory(folder, ec)) continue;
            auto take = [&](const fs::path& file, plugin::FileKind fk) {
                if (auto bytes = read_text_file(file)) {
                    out.push_back({fk, sha256(*bytes), 0, u.name, {}, true, false});
                    ++refused;
                }
            };
            if (k == AddonKind::Script) {
                for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator();
                     it.increment(ec))
                    if (it->is_regular_file(ec) && extension_of(it->path().filename().string()) == ".lua")
                        take(it->path(), plugin::FileKind::Script);
            } else {
                for (const auto& e : fs::directory_iterator(folder, ec))
                    if (e.is_regular_file(ec) && library_file(e.path().filename().string()))
                        take(e.path(), plugin::FileKind::Plugin);
            }
        }
        log::warn("Add-ons: {} is named in addons/installed.txt, but its record cannot be read ({}): its {} file(s) "
                  "get no permissions until it is put right",
                  u.name, reg.problem, refused);
    }
    if (reg.from_backup && reg.unreadable.empty())
        log::warn("Add-ons: {}: its add-ons are read from addons/{}", reg.problem, kRegistryBackup);
    return out;
}

std::vector<plugin::Ceiling> addon_ceilings(const std::vector<InstalledAddon>& addons) {
    std::vector<plugin::Ceiling> out;
    for (const auto& a : addons)
        for (const auto& f : a.files) {
            const std::string ext = extension_of(f.path);
            if (a.kind == AddonKind::Script && ext == ".lua")
                out.push_back({plugin::FileKind::Script, f.sha256, a.caps, a.name, a.source, true, iequal(f.path, "init.lua")});
            else if (a.kind == AddonKind::Plugin && (ext == ".so" || ext == ".dll" || ext == ".dylib") &&
                     f.path.find('/') == std::string::npos &&
                     iequal(plugin::plugin_name(rel(f.path)), a.name))
                out.push_back({plugin::FileKind::Plugin, f.sha256, a.caps, a.name, a.source});
        }
    return out;
}

void addons_at_start(const fs::path& data_dir) {
    const fs::path dir = addons_dir(data_dir);
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    bool keep_previous = false;
    if (fs::exists(dir / "journal.txt", ec)) {
        if (!journal_done(data_dir)) {
            log::warn("Add-ons: an install was cut off; it is undone");
            undo_journal(data_dir);
            // Each file the undo knew of went back: one still in previous/ is a copy the journal does not name (a
            // damaged journal, a move back that failed), maybe the only one of a replaced file. It is kept aside.
            keep_previous = holds_a_file(dir / "previous");
        }
        remove_quietly(dir / "journal.txt");
    }
    remove_quietly(dir / "installed.prev.txt");
    if (keep_previous) {
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        const fs::path kept = dir / ("kept-" + std::to_string(stamp));
        fs::rename(dir / "previous", kept, ec);
        if (ec)
            log::warn("Add-ons: addons/previous/ still holds files the journal does not name, and cannot be moved "
                      "aside: {}; it stays",
                      ec.message());
        else
            log::warn("Add-ons: addons/previous/ still held files the journal does not name; they are kept in {}",
                      kept.string());
    } else {
        remove_quietly(dir / "previous");
    }
    remove_quietly(dir / "new");
    const AddonRegistry reg = read_addon_registry(data_dir);
    if (reg.problem.empty()) {
        // An installed.txt from before the backup: it gets its copy now.
        if (fs::exists(dir / "installed.txt", ec) && !fs::exists(dir / kRegistryBackup, ec))
            if (auto text = read_text_file(dir / "installed.txt")) write_file_atomic(dir / kRegistryBackup, *text);
        return;
    }
    if (reg.from_backup && reg.writable()) {
        // The backup has every add-on installed.txt names (or installed.txt is gone): it is written again from it.
        if (write_installed_addons(data_dir, reg.addons))
            log::warn("Add-ons: {}; it was written again from addons/{}", reg.problem, kRegistryBackup);
        return;
    }
    log::error("Add-ons: {}: the add-ons it names that cannot be read get no permissions, and no add-on can be "
               "installed, updated or removed until it is put right (or taken out, with those add-ons' folders)",
               reg.problem);
}

} // namespace hailstone::app
