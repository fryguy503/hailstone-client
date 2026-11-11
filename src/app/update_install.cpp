#include "app/update_install.h"
#include "core/distribution.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#include "app/addon_install.h"
#include "core/atomic_file.h"
#include "core/log.h"
#include "core/zip_read.h"

namespace hailstone::app {

namespace fs = std::filesystem;

namespace {

// A relative '/'-separated UTF-8 name as a path.
fs::path rel(std::string_view name) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

// The first word of `line` and the rest after one space.
std::pair<std::string_view, std::string_view> split(std::string_view line) {
    const auto sp = line.find(' ');
    if (sp == std::string_view::npos) return {line, {}};
    return {line.substr(0, sp), line.substr(sp + 1)};
}

std::vector<std::string_view> words(std::string_view s) {
    std::vector<std::string_view> out;
    while (!s.empty()) {
        auto [w, rest] = split(s);
        if (!w.empty()) out.push_back(w);
        s = rest;
    }
    return out;
}

struct Staged {
    std::string version;
    std::vector<UpdatePackage> packages;
};

std::optional<Staged> read_staged(const fs::path& install_dir) {
    auto text = read_text_file(update_dir(install_dir) / "ready.txt");
    if (!text) return std::nullopt;
    Staged s;
    for (const auto& line : lines_of(*text)) {
        const auto w = words(line);
        if (w.size() == 2 && w[0] == "version") {
            s.version = std::string(w[1]);
        } else if (w.size() == 5 && w[0] == "package") {
            UpdatePackage p;
            p.component = std::string(w[1]);
            auto [end, ec] = std::from_chars(w[2].data(), w[2].data() + w[2].size(), p.size);
            auto digest = parse_sha256(w[3]);
            if (ec != std::errc{} || end != w[2].data() + w[2].size() || !digest) return std::nullopt;
            p.sha256 = *digest;
            p.file = std::string(w[4]);
            if (p.file.find('/') != std::string::npos || p.file.find('\\') != std::string::npos || p.file.empty() ||
                p.file[0] == '.')
                return std::nullopt;
            s.packages.push_back(std::move(p));
        }
    }
    if (s.version.empty()) return std::nullopt;
    return s;
}

struct State {
    std::string kind; // "installing", "pending" or ""
    std::string new_version, old_version;
    int starts = 0;
};

State read_state(const fs::path& install_dir) {
    State st;
    auto text = read_text_file(update_dir(install_dir) / "state.txt");
    if (!text) return st;
    const auto lines = lines_of(*text);
    if (lines.empty()) return st;
    const auto w = words(lines[0]);
    if (w.size() >= 3 && (w[0] == "installing" || w[0] == "pending")) {
        st.kind = std::string(w[0]);
        st.new_version = std::string(w[1]);
        st.old_version = std::string(w[2]);
        if (w.size() >= 4) std::from_chars(w[3].data(), w[3].data() + w[3].size(), st.starts);
    }
    return st;
}

bool write_state(const fs::path& install_dir, const State& st) {
    std::string line = st.kind + " " + st.new_version + " " + st.old_version;
    if (st.kind == "pending") line += " " + std::to_string(st.starts);
    return write_file_atomic(update_dir(install_dir) / "state.txt", line + "\n");
}

void remove_quietly(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec) log::warn("Update: cannot remove {}: {}", path.string(), ec.message());
}

// Moves `from` to `to` (making its folder); on Windows a running program or a loaded library may be moved, not
// deleted.
bool move_file(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (ec && error) *error = "cannot move " + from.string() + " to " + to.string() + ": " + ec.message();
    return !ec;
}

bool is_directory_entry(const fs::path& p) {
    std::error_code ec;
    return fs::symlink_status(p, ec).type() == fs::file_type::directory;
}

// Moves a file out of the way into update/trash/ (removed at the next start, when nothing has it open). Never a
// folder: an update moves files only, and a folder in the way holds files it did not put there.
void trash(const fs::path& install_dir, const fs::path& file) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(file, ec))) return;
    if (is_directory_entry(file)) {
        log::warn("Update: rolling back: {} is a folder, not a file the update moved; it stays", file.string());
        return;
    }
    // A name of its own each time: an earlier run's trash may still be there, held open by a program still running.
    static int n = 0;
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    const fs::path to =
        update_dir(install_dir) / "trash" / (std::to_string(stamp) + "-" + std::to_string(++n)) / file.filename();
    std::string why;
    if (!move_file(file, to, &why)) log::warn("Update: {}", why);
}

// The journal's lines that were written whole: a line is written before its move, so one cut off by a crash (no
// newline yet) names a move that never began, and may be any part of a name ("added ui" of "added ui/x.lua").
std::vector<std::pair<std::string, std::string>> journal_lines(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t at = 0;
    for (std::size_t nl; (nl = text.find('\n', at)) != std::string::npos; at = nl + 1) {
        std::string_view line(text.data() + at, nl - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        auto [what, name] = split(line);
        if (what != "moved" && what != "removed" && what != "added") continue;
        // The journal is the client's own file, but a damaged one moves nothing outside the folder, nothing of the
        // player's and no folder (an update writes files only, never these).
        if (!is_safe_zip_path(name) || name.ends_with('/') || is_player_path(name)) {
            log::warn("Update: rolling back: the journal's \"{}\" is not a file an update moves; skipped", line);
            continue;
        }
        out.emplace_back(std::string(what), std::string(name));
    }
    return out;
}

// Undoes the moves the journal lists, last first, and puts installed.txt back.
void roll_back(const fs::path& install_dir) {
    const fs::path dir = update_dir(install_dir);
    auto text = read_text_file(dir / "journal.txt");
    auto lines = text ? journal_lines(*text) : std::vector<std::pair<std::string, std::string>>{};
    std::reverse(lines.begin(), lines.end());
    for (const auto& [what, name] : lines) {
        const fs::path target = install_dir / rel(name);
        if (what == "moved" || what == "removed") {
            // The line is written before its move: with no saved copy the move never happened, and the file at
            // `target` is still the original, so it stays.
            const fs::path saved = dir / "previous" / rel(name);
            std::error_code ec;
            if (!fs::exists(fs::symlink_status(saved, ec)) || is_directory_entry(saved)) continue;
            if (is_directory_entry(target)) {
                log::warn("Update: rolling back: {} is a folder now; its old file is not put back", target.string());
                continue;
            }
            trash(install_dir, target);
            std::string why;
            if (!move_file(saved, target, &why)) log::warn("Update: rolling back: {}", why);
        } else {
            trash(install_dir, target);
        }
    }
    std::error_code ec;
    if (fs::exists(dir / "installed.prev.txt", ec)) {
        fs::rename(dir / "installed.prev.txt", dir / "installed.txt", ec);
    } else {
        fs::remove(dir / "installed.txt", ec);
    }
    remove_quietly(dir / "journal.txt");
    remove_quietly(dir / "previous");
    remove_quietly(dir / "new");
    remove_quietly(dir / "state.txt");
}

void end_pending(const fs::path& install_dir) {
    const fs::path dir = update_dir(install_dir);
    remove_quietly(dir / "previous");
    remove_quietly(dir / "journal.txt");
    remove_quietly(dir / "installed.prev.txt");
    remove_quietly(dir / "state.txt");
}

bool write_installed(const fs::path& install_dir, const InstalledUpdate& in) {
    std::string text = "# The files Hailstone's updates installed (docs/updater.md).\n";
    for (const auto& [component, digest] : in.components) text += "component " + component + " " + to_hex(digest) + "\n";
    for (const auto& [component, files] : in.files)
        for (const auto& f : files) text += "file " + component + " " + f + "\n";
    return write_file_atomic(update_dir(install_dir) / "installed.txt", text);
}

} // namespace

fs::path update_dir(const fs::path& install_dir) { return install_dir / "update"; }

bool package_file_matches(const fs::path& path, std::uint64_t size, const Sha256Digest& sha256) {
    std::error_code ec;
    const auto actual = fs::file_size(path, ec);
    if (ec || actual != size) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    Sha256 h;
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<std::size_t>(in.gcount());
        if (n) h.update(std::as_bytes(std::span(buf.data(), n)));
    }
    return h.finish() == sha256;
}

InstalledUpdate read_installed_update(const fs::path& install_dir) {
    InstalledUpdate out;
    auto text = read_text_file(update_dir(install_dir) / "installed.txt");
    if (!text) return out;
    for (const auto& line : lines_of(*text)) {
        auto [what, rest] = split(line);
        auto [component, value] = split(rest);
        if (component.empty() || value.empty()) continue;
        if (what == "component") {
            if (auto d = parse_sha256(value)) out.components[std::string(component)] = *d;
        } else if (what == "file" && is_safe_zip_path(value)) {
            out.files[std::string(component)].emplace_back(value);
        }
    }
    return out;
}

bool is_player_path(std::string_view path) {
    const std::string p = lower(path);
    const std::string_view first = std::string_view(p).substr(0, p.find('/'));
    static const std::set<std::string_view> folders = {"update", "servers", "logs",  "cache", "screenshots",
                                                       "bug-reports", "maps", "scripts", "textures", "web",
                                                       "addons"};
    static const std::set<std::string_view> files = {"hailstone.ini", "hailstone-windows.ini", "notes.txt"};
    if (first.size() < p.size() && folders.count(first)) return true;
    if (files.count(p)) return true;
    return p.starts_with("ui/settings/") || p.starts_with("ui/loadingbackgrounds/");
}

bool stage_update(const fs::path& install_dir, const std::string& version, const std::vector<UpdatePackage>& packages,
                  std::string* error) {
    if (distribution::managed()) {
        if (error) *error = "updates are supplied by this managed distribution";
        return false;
    }
    std::string text = "version " + version + "\n";
    for (const auto& p : packages)
        text += "package " + p.component + " " + std::to_string(p.size) + " " + to_hex(p.sha256) + " " + p.file + "\n";
    if (write_file_atomic(update_dir(install_dir) / "ready.txt", text)) return true;
    if (error) *error = "cannot write " + (update_dir(install_dir) / "ready.txt").string();
    return false;
}

std::string staged_update_version(const fs::path& install_dir) {
    auto s = read_staged(install_dir);
    return s ? s->version : std::string();
}

bool staged_update_ok(const fs::path& install_dir) {
    auto s = read_staged(install_dir);
    if (!s) return false;
    for (const auto& p : s->packages)
        if (!package_file_matches(update_dir(install_dir) / "download" / p.file, p.size, p.sha256)) return false;
    return true;
}

void clear_staged_update(const fs::path& install_dir) {
    std::error_code ec;
    fs::remove(update_dir(install_dir) / "ready.txt", ec);
}

std::string failed_update_version(const fs::path& install_dir) {
    auto text = read_text_file(update_dir(install_dir) / "failed.txt");
    if (!text) return {};
    auto lines = lines_of(*text);
    return lines.empty() ? std::string() : lines[0];
}

InstallResult install_staged_update(const fs::path& install_dir, const std::string& running,
                                    const std::function<void(float)>& progress) {
    InstallResult r;
    if (distribution::managed()) {
        r.error = "updates are supplied by this managed distribution";
        return r;
    }
    const fs::path dir = update_dir(install_dir);
    auto staged = read_staged(install_dir);
    if (!staged) {
        r.error = "no update is staged";
        return r;
    }
    r.version = staged->version;
    auto report = [&](float f) {
        if (progress) progress(f);
    };

    // 1. Unpack every package into new/, checking each again first: nothing in the folder changes yet.
    remove_quietly(dir / "new");
    std::map<std::string, std::vector<std::string>> new_files;
    std::set<std::string> all_new; // lower-cased: one file of two components, or of two cases, is refused
    std::set<std::string> skipped;
    for (const auto& p : staged->packages) {
        const std::string only = p.component.starts_with("plugin-") ? "plugins/" + p.component.substr(7) + "/" : "";
        // An add-on installed into the plugin's folder since the download (docs/addons.md) owns it now: the release's
        // copy would overwrite it. The updater checks this when it downloads; the player may have installed one since.
        if (!only.empty() && addon_owns_folder(install_dir, std::string_view(only).substr(0, only.size() - 1))) {
            log::info("Update: {} is left out: an add-on owns {}", p.component, only);
            skipped.insert(p.component);
            continue;
        }
        const fs::path zip = dir / "download" / p.file;
        if (!package_file_matches(zip, p.size, p.sha256)) {
            r.error = p.file + " is missing or changed since it was downloaded";
            remove_quietly(dir / "new");
            return r;
        }
        ZipReader reader;
        if (!reader.open(zip)) {
            r.error = p.file + ": " + reader.error();
            remove_quietly(dir / "new");
            return r;
        }
        auto& files = new_files[p.component];
        for (const auto& e : reader.entries()) {
            if (e.is_directory()) continue;
            if (is_player_path(e.name) || (!only.empty() && !lower(e.name).starts_with(lower(only))) ||
                !all_new.insert(lower(e.name)).second) {
                r.error = p.file + " holds " + e.name + ", which an update may not write";
                remove_quietly(dir / "new");
                return r;
            }
            if (!reader.extract_to_file(e, dir / "new" / rel(e.name))) {
                r.error = p.file + ": " + reader.error();
                remove_quietly(dir / "new");
                return r;
            }
            files.push_back(e.name);
        }
    }
    report(0.5f);

    // 2. Move the old files into previous/ and the new ones into place, each move written to the journal first.
    const InstalledUpdate old = read_installed_update(install_dir);
    remove_quietly(dir / "previous");
    remove_quietly(dir / "journal.txt");
    std::error_code ec;
    fs::remove(dir / "installed.prev.txt", ec);
    // Saved before anything moves: a roll-back without installed.prev.txt takes it there was no registry and
    // deletes installed.txt, which still describes the files it puts back.
    if (fs::exists(dir / "installed.txt", ec) &&
        !fs::copy_file(dir / "installed.txt", dir / "installed.prev.txt", ec)) {
        r.error = "cannot save " + (dir / "installed.txt").string() + (ec ? ": " + ec.message() : std::string());
        remove_quietly(dir / "new");
        return r;
    }
    if (!write_state(install_dir, {"installing", staged->version, running, 0})) {
        r.error = "cannot write " + (dir / "state.txt").string();
        remove_quietly(dir / "new");
        return r;
    }
    std::ofstream journal(dir / "journal.txt", std::ios::binary | std::ios::trunc);
    auto note = [&](std::string_view what, std::string_view name) {
        journal << what << ' ' << name << '\n';
        journal.flush();
        return static_cast<bool>(journal);
    };
    auto fail = [&](std::string why) {
        journal.close();
        roll_back(install_dir);
        r.error = std::move(why);
        log::warn("Update: {}; the folder was put back as it was", r.error);
        return r;
    };
    std::size_t total = 0, done = 0;
    for (const auto& [component, files] : new_files) total += files.size();
    for (const auto& [component, files] : new_files) {
        for (const auto& name : files) {
            const fs::path target = install_dir / rel(name);
            std::string why;
            if (fs::exists(target, ec)) {
                if (!note("moved", name)) return fail("cannot write the update journal");
                if (!move_file(target, dir / "previous" / rel(name), &why)) return fail(why);
            } else if (!note("added", name)) {
                return fail("cannot write the update journal");
            }
            if (!move_file(dir / "new" / rel(name), target, &why)) return fail(why);
            report(0.5f + 0.5f * static_cast<float>(++done) / static_cast<float>(std::max<std::size_t>(total, 1)));
        }
    }
    // 3. Files these components shipped before and no longer do go into previous/ too.
    for (const auto& [component, files] : new_files) {
        auto it = old.files.find(component);
        if (it == old.files.end()) continue;
        for (const auto& name : it->second) {
            if (all_new.count(lower(name)) || is_player_path(name)) continue;
            const fs::path target = install_dir / rel(name);
            if (!fs::exists(target, ec)) continue;
            std::string why;
            if (!note("removed", name)) return fail("cannot write the update journal");
            if (!move_file(target, dir / "previous" / rel(name), &why)) return fail(why);
        }
    }
    journal.close();

    InstalledUpdate now = old;
    for (const auto& p : staged->packages) {
        if (skipped.count(p.component)) continue;
        now.components[p.component] = p.sha256;
        now.files[p.component] = new_files[p.component];
    }
    if (!write_installed(install_dir, now)) return fail("cannot write " + (dir / "installed.txt").string());
    if (!write_state(install_dir, {"pending", staged->version, running, 0}))
        return fail("cannot write " + (dir / "state.txt").string());
    remove_quietly(dir / "new");
    remove_quietly(dir / "download");
    clear_staged_update(install_dir);
    log::info("Update: Hailstone {} is installed (it was {})", staged->version, running);
    r.ok = true;
    return r;
}

StartNote update_at_start(const fs::path& install_dir, const std::string& running) {
    StartNote note;
    if (distribution::managed()) return note;
    const fs::path dir = update_dir(install_dir);
    remove_quietly(dir / "trash");
    // A rollback's news, left by the version that was rolled back for this one to show.
    if (auto text = read_text_file(dir / "note.txt")) {
        note.message = lines_of(*text).empty() ? std::string() : lines_of(*text)[0];
        remove_quietly(dir / "note.txt");
    }
    const State st = read_state(install_dir);
    if (st.kind == "installing") {
        roll_back(install_dir);
        note.message = "The update to Hailstone " + st.new_version + " was cut off, so " + st.old_version +
                       " was put back.";
        note.restart = running != st.old_version;
        if (!note.restart) log::warn("Update: {}", note.message);
    } else if (st.kind == "pending") {
        if (running != st.new_version) {
            end_pending(install_dir); // another version is here: the player's own
        } else if (st.starts >= 2) {
            roll_back(install_dir);
            write_file_atomic(update_dir(install_dir) / "failed.txt", st.new_version + "\n");
            note.message = "Hailstone " + st.new_version + " did not start properly, so " + st.old_version +
                           " was put back.";
            note.restart = true;
        } else {
            State next = st;
            ++next.starts;
            write_state(install_dir, next);
        }
    }
    if (note.restart) {
        log::warn("Update: {}", note.message);
        write_file_atomic(dir / "note.txt", note.message + "\n");
    }
    return note;
}

void update_started_fine(const fs::path& install_dir, const std::string& running) {
    if (distribution::managed()) return;
    const State st = read_state(install_dir);
    if (st.kind == "pending" && st.new_version == running) {
        end_pending(install_dir);
        log::info("Update: Hailstone {} started fine; the files it replaced are removed", running);
    }
}

// The files under plugins/nms/ and plugins/projecttriune/ in 0.2.0-alpha's two zips and 0.1.0-alpha's NMS plugin
// downloads (sizes and SHA-256 taken from the published assets). Both plugins are add-ons since 0.3.0-alpha.
const std::vector<RetiredFile>& retired_release_files() {
    static const std::vector<RetiredFile> files = {
    {"plugins/nms/nms.dll", 520192, "124ea6473e93b41623f65f390fc6aa7dad4f9667fb76c2679dd406ded57bd6db"},
    {"plugins/nms/nms.dll", 520192, "4218816b9d254b8bd2ed2e79da97586663b9daf333400b1d5349badd001e90ee"},
    {"plugins/nms/nms.so", 478816, "a90e30b90c234b5fcd5a9009c22e5a3b0588278bbe6aaa298c9eedcf1aadb3b5"},
    {"plugins/nms/ui/gamepad/nmsloot.lua", 26659, "7aa4fac26587fa366105b8b514b96beeecbec288dc2bf25b2b3345e7fd7bb43e"},
    {"plugins/nms/ui/gamepad/nmsvault.lua", 9650, "6ed8cfd483c153d6b130ac4da64d03a5355fb0a0c1a6f2d89e039f67605c9757"},
    {"plugins/nms/ui/gamepad/nmswaypoints.lua", 5778, "a17c346f1acd0450a69b8d9b58395f811ef0bc5851418307d02e0889fe396b0c"},
    {"plugins/nms/ui/gamepad/pet.lua", 4896, "ab52393519ba4819c3b92acbf89e0bf1b239ca172b6cd79355b2ece94fc92f18"},
    {"plugins/nms/ui/modern/lib/nmsloot.lua", 17946, "748cd3b29ad60a73b773e7bdec54b983055b4c86d8d07024693a4cee75fd6518"},
    {"plugins/nms/ui/modern/lib/nmsvault.lua", 6906, "768d1173dcff3f703d578f98d86a890583a5d6fd3df455e930e09554cf674e42"},
    {"plugins/nms/ui/modern/nmsloot.lua", 1082, "bf1cb1678e4751eb8b0446de64a4b3f205cf717f647f9ce81b6e7c8d301d0c20"},
    {"plugins/nms/ui/modern/nmsvault.lua", 773, "6cca6fedc7a2a8a2e6532224110f6effefe04b258113576a78c4a9fb8750326f"},
    {"plugins/nms/ui/modern/nmswaypoints.lua", 5661, "5222c9950289bc10b9ee158007290733b5316b1580914461b76892049f10bdf2"},
    {"plugins/nms/ui/modern/pet.lua", 4782, "fde5bc586f6acb68e75f459a92aea2ffb62405db9d50c2c1e84cfe5678402348"},
    {"plugins/projecttriune/projecttriune.dll", 675840, "f1442b91c36eb8f88dc3f03f2f43030ab28e72cb1e17cbb8f8a60c2e1e002799"},
    {"plugins/projecttriune/projecttriune.so", 584952, "bf9605039be4bdf740ca57cf8186c48f3de7b85bdbc2b69328e30798694952ab"},
    {"plugins/projecttriune/ui/gamepad/lib/triunewindows.lua", 12359, "2c5aac7c7a1534bbfee8fcb7de0485f47493a8267fc7947d6c8f40b9c5e96499"},
    {"plugins/projecttriune/ui/gamepad/pet.lua", 4930, "39d941ff37a305dd8e29d4444c62a6685af3bacbe85f6aff4fcb9d2959cb1783"},
    {"plugins/projecttriune/ui/gamepad/triuneloot.lua", 16102, "59059a9215c174020c6da354e1806b71f97abd1f4fa2f9f2935343a8a2c54cef"},
    {"plugins/projecttriune/ui/gamepad/triunevault.lua", 9769, "2eeeb2b94d78bf085284fe24a40f5a00b198b626fdb1144db85982d1e5ebb927"},
    {"plugins/projecttriune/ui/gamepad/triunewaypoints.lua", 5848, "aef518a32470768a15654e1557e4f39ba8b49c1eebdd0fc79f35bb63440dba9b"},
    {"plugins/projecttriune/ui/gamepad/triunewindows.lua", 287, "a7dde89e4d0395d2c4059ecd0934176790fd119e35d7c3f04c327d1050df8f2b"},
    {"plugins/projecttriune/ui/modern/lib/triuneloot.lua", 16602, "92a49dedff72da9f1f0358b089381b7ba90835a34fc706c4b22ab1a0454110a5"},
    {"plugins/projecttriune/ui/modern/lib/triunevault.lua", 6978, "a39bb205438fb771c42491f44596dc7759168a8b9540396d1d0a8a7050c947c6"},
    {"plugins/projecttriune/ui/modern/lib/triunewindows.lua", 12643, "879f876136941f46f3ada6b0d41073da7843d7bd329df94e181fb2d55452babf"},
    {"plugins/projecttriune/ui/modern/pet.lua", 4813, "3bddfc5f52b443380362128f7b107e447ebad43e19142e6e14979467b5170068"},
    {"plugins/projecttriune/ui/modern/triuneloot.lua", 1585, "013003b49ad9860e1d2764b12575d70b0d970efc21253aae7b39f233f93ef5dc"},
    {"plugins/projecttriune/ui/modern/triunevault.lua", 831, "1e29e1be0ffd8850cadafa1655d811ef9905a7d3c502e0ec2dda76e2fbfa4f0e"},
    {"plugins/projecttriune/ui/modern/triunewaypoints.lua", 5725, "78b3fadaec4bade4fcedcfc6f9c52eb04d3470ddf0747f3c80867eb03ed2d05e"},
    {"plugins/projecttriune/ui/modern/triunewindows.lua", 287, "a7dde89e4d0395d2c4059ecd0934176790fd119e35d7c3f04c327d1050df8f2b"},
    };
    return files;
}

std::vector<std::string> retire_old_release_files(const fs::path& install_dir) {
    return retire_old_release_files(install_dir, retired_release_files());
}

std::vector<std::string> retire_old_release_files(const fs::path& install_dir,
                                                  const std::vector<RetiredFile>& retired) {
    std::vector<std::string> removed;
    if (read_state(install_dir).kind == "pending") return removed;
    const fs::path plugins = install_dir / "plugins";
    std::error_code ec;
    // A plugin's folder (plugins/<name>/) whose library is another build than the ones shipped (the add-on's, or a
    // build of its own repository) holds that plugin now: none of its files goes, also the ones it still has
    // unchanged since the old release (a window file nobody edited), or the add-on would lose them at every start.
    auto matches_retired = [&](const fs::path& file, const std::string& path) {
        for (const RetiredFile& f : retired) {
            if (f.path != path) continue;
            const auto digest = parse_sha256(f.sha256);
            if (digest && package_file_matches(file, f.size, *digest)) return true;
        }
        return false;
    };
    std::set<fs::path> current;
    for (const RetiredFile& f : retired) {
        const fs::path rf = rel(f.path);
        auto it = rf.begin();
        if (it == rf.end() || *it != "plugins" || ++it == rf.end()) continue;
        const fs::path folder = plugins / *it;
        if (current.contains(folder) || !fs::is_directory(folder, ec)) continue;
        for (const auto& e : fs::directory_iterator(folder, ec)) {
            const std::string ext = e.path().extension().string();
            if (!e.is_regular_file(ec) || (ext != ".so" && ext != ".dll" && ext != ".dylib")) continue;
            const std::string path = "plugins/" + it->string() + "/" + e.path().filename().string();
            if (!matches_retired(e.path(), path)) {
                current.insert(folder);
                break;
            }
        }
    }
    std::set<fs::path> dirs; // the folders the files were in, up to (not with) plugins/ itself
    for (const RetiredFile& f : retired) {
        const fs::path target = install_dir / rel(f.path);
        bool kept = false;
        for (const fs::path& c : current)
            if (std::mismatch(c.begin(), c.end(), target.begin(), target.end()).first == c.end()) kept = true;
        if (kept) continue;
        const auto digest = parse_sha256(f.sha256);
        if (!digest || !fs::is_regular_file(target, ec) || !package_file_matches(target, f.size, *digest)) continue;
        if (!fs::remove(target, ec)) continue;
        removed.emplace_back(f.path);
        for (fs::path d = target.parent_path(); d != plugins && d != install_dir && d.has_relative_path();
             d = d.parent_path())
            dirs.insert(d);
    }
    // Deepest first (a subfolder sorts after its parent), so a folder emptied by removing its empty subfolder goes too.
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
        if (fs::is_directory(*it, ec) && fs::is_empty(*it, ec)) fs::remove(*it, ec);
    if (!removed.empty())
        log::info("Update: removed {} file(s) of the server plugins an earlier release shipped; they are add-ons now",
                  removed.size());
    return removed;
}

} // namespace hailstone::app
