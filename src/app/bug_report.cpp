#include "app/bug_report.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <span>
#include <system_error>

#include "core/file_time.h"
#include "core/log.h"
#include "core/zip_write.h"
#include "platform/local_time.h"

namespace hailstone::app {

namespace {
namespace fs = std::filesystem;

// A log larger than this goes in as its last part (they compress about tenfold).
constexpr std::size_t kLogTail = 24u << 20;
// Held back for report.txt and summary.md, which go in last (they name what did not fit).
constexpr std::size_t kTextReserve = 64u << 10;

std::string format_time(std::time_t t, bool utc) {
    const auto tm = utc ? platform::utc_time(t) : platform::local_time(t);
    if (!tm) return "(unknown time)";
    char buf[64];
    std::strftime(buf, sizeof buf, utc ? "%Y-%m-%d %H:%M:%S UTC" : "%Y-%m-%d %H:%M:%S %z", &*tm);
    return buf;
}

// The file's bytes; with `tail`, at most its last `tail` bytes (from the start of a line), after a note.
std::optional<std::string> read_file(const fs::path& path, std::size_t tail = 0) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    const auto size = static_cast<std::size_t>(in.tellg());
    std::size_t from = 0;
    if (tail && size > tail) from = size - tail;
    in.seekg(static_cast<std::streamoff>(from));
    std::string text(size - from, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!in) return std::nullopt;
    if (from) {
        const std::size_t line = text.find('\n');
        text.erase(0, line == std::string::npos ? 0 : line + 1);
        text = "[the first " + std::to_string(from) + " bytes of this log were cut to fit the report]\n" + text;
    }
    return text;
}

std::span<const std::byte> bytes(std::string_view s) { return std::as_bytes(std::span(s.data(), s.size())); }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::time_t file_time(const fs::path& path) {
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    if (ec) return 0;
    return file_time_to_time_t(t);
}

// The player's screenshots from the window before `time`, newest first.
std::vector<fs::path> recent_screenshots(const fs::path& dir, std::time_t time) {
    std::vector<std::pair<std::time_t, fs::path>> found;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return {};
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || lower(e.path().extension().string()) != ".png") continue;
        const std::time_t t = file_time(e.path());
        if (t + kBugReportScreenshotWindow >= time && t <= time + 60) found.emplace_back(t, e.path());
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<fs::path> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool name_char(char c) { return word_char(c) || c == '-' || c == '.'; }
bool separator(char c) { return c == '/' || c == '\\'; }

std::string first_line(std::string_view text, std::size_t max) {
    std::size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    text.remove_prefix(start);
    std::string line(text.substr(0, text.find_first_of("\r\n")));
    if (line.size() > max) line = line.substr(0, max - 3) + "...";
    return line;
}
} // namespace

std::string bug_report_file_name(bool crash, const std::tm& local, int n) {
    char name[64];
    std::snprintf(name, sizeof name, "hailstone-%s-%04d%02d%02d-%02d%02d%02d", crash ? "crash" : "bug",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    std::string out = name;
    if (n > 1) out += "-" + std::to_string(n);
    return out + ".zip";
}

std::string scrub_settings(std::string_view text) {
    // "auth" covers auth_key, authtoken and the like. "key" alone would also take
    // the keymap and hotkey settings a bug report wants, so only a name ending in
    // "_key" (a plugin's auth_key, private_key) or holding "apikey" counts.
    static constexpr std::string_view kSecret[] = {"password", "passwd", "token", "secret", "session", "auth", "apikey"};
    std::string out;
    out.reserve(text.size());
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        const std::size_t hs = line.find('=');
        bool secret = false;
        if (hs != std::string_view::npos) {
            const std::string key = lower(line.substr(0, hs));
            for (std::string_view s : kSecret) secret = secret || key.find(s) != std::string::npos;
            const std::string_view name = std::string_view(key).substr(0, key.find_last_not_of(" \t") + 1);
            secret = secret || name.ends_with("_key") || name.ends_with("-key") || name.ends_with(".key");
        }
        out += secret ? std::string(line.substr(0, hs + 1)) + " (removed)" : std::string(line);
        if (end != std::string_view::npos) out += '\n';
    }
    return out;
}

std::string redact_private(std::string_view text, const std::vector<std::string>& accounts, const fs::path& home) {
    // Every name is looked for in one pass over one lowered copy: a report's logs run to tens of MB.
    enum class Rule { Folder, FolderName, Word };
    struct Needle {
        std::string low;
        std::string_view with;
        Rule rule;
    };
    std::vector<Needle> needles;
    std::string h = home.string();
    while (h.size() > 1 && separator(h.back())) h.pop_back();
    if (h.size() > 1) {
        // The folder itself, then its name: /home/<name> is the usual, but the name also shows in other folders.
        std::string other = h;
        for (char& c : other) if (separator(c)) c = c == '/' ? '\\' : '/';
        needles.push_back({lower(h), "~", Rule::Folder});
        if (other != h) needles.push_back({lower(other), "~", Rule::Folder});
        const std::string name = home.filename().string();
        if (!name.empty() && name != "." && name != "..") needles.push_back({lower(name), "<user>", Rule::FolderName});
    }
    // The longest first, so a name holding another goes whole.
    std::vector<std::string> names;
    for (const std::string& a : accounts)
        if (!a.empty()) names.push_back(lower(a));
    std::stable_sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    for (std::string& n : names) needles.push_back({std::move(n), "<account>", Rule::Word});
    if (needles.empty()) return std::string(text);

    const std::string low = lower(text);
    auto fits = [&](const Needle& n, std::size_t at) {
        const std::size_t end = at + n.low.size();
        switch (n.rule) {
        case Rule::Folder: return end == text.size() || !name_char(text[end]);
        case Rule::FolderName:
            return at > 0 && separator(text[at - 1]) && (end == text.size() || !name_char(text[end]));
        case Rule::Word:
            return (at == 0 || !word_char(text[at - 1])) && (end == text.size() || !word_char(text[end]));
        }
        return false;
    };
    std::string out;
    out.reserve(text.size());
    std::size_t done = 0;
    for (std::size_t at = 0; at < low.size();) {
        const Needle* hit = nullptr;
        for (const Needle& n : needles)
            if (low[at] == n.low[0] && std::string_view(low).substr(at, n.low.size()) == n.low && fits(n, at)) {
                hit = &n;
                break;
            }
        if (!hit) {
            ++at;
            continue;
        }
        out.append(text.substr(done, at - done));
        out += hit->with;
        at += hit->low.size();
        done = at;
    }
    out.append(text.substr(done));
    return out;
}

std::string redact_chat(std::string_view text) {
    // GameConnection::chat logs each line as "[info] chat: <the line>" (core/log.h), the crash report's last lines
    // the same way: tells, guild and group chat are other players' words, and the report is posted in public. A
    // chat text with a newline in it (a server's or a plugin's can have one) goes on over lines with no level's tag:
    // those go too, up to the next tagged line.
    static constexpr std::string_view kMark = "] chat: ";
    static constexpr std::string_view kWith = "(chat taken out of the report)";
    // Where a line's level tag ("[info]") ends, npos when it does not start with one.
    auto tag_end = [](std::string_view line) {
        const std::size_t close = line.starts_with('[') ? line.find("] ") : std::string_view::npos;
        if (close == std::string_view::npos || line.substr(1, close - 1).find_first_of("[] ") != std::string_view::npos)
            return std::string_view::npos;
        return close;
    };
    std::string out;
    out.reserve(text.size());
    bool in_chat = false;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = text.substr(0, end == std::string_view::npos ? text.size() : end + 1);
        text.remove_prefix(line.size());
        const std::size_t close = tag_end(line);
        if (close == std::string_view::npos) {
            if (!in_chat) out.append(line);
            continue;
        }
        // The level's tag only: a line that merely holds "] chat: " further in stays.
        in_chat = line.substr(close).starts_with(kMark);
        if (in_chat) {
            out.append(line.substr(0, close + kMark.size()));
            out += kWith;
            out += line.ends_with("\r\n") ? "\r\n" : line.ends_with('\n') ? "\n" : "";
        } else {
            out.append(line);
        }
    }
    return out;
}

std::string redact_crash_report(std::string_view text) {
    // crash_log.cpp's stack, written after the last log lines and only on builds with backtrace(): its marker, then
    // backtrace_symbols_fd's frames ("/opt/hailstone(+0x1a) [0x5555...]") to the end. Neither Windows nor a build
    // without it writes one, so a chat line's continuation that reads "stack:" is told apart by what follows it.
    static constexpr std::string_view kStack = "\nstack:\n";
    static constexpr std::string_view kLog = "last log lines:\n";
    auto frames_only = [](std::string_view rest) {
        while (!rest.empty()) {
            const std::size_t end = rest.find('\n');
            std::string_view line = rest.substr(0, end);
            rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
            if (line.ends_with('\r')) line.remove_suffix(1);
            const std::size_t open = line.rfind(" [0x");
            if (line.empty()) continue;
            if (open == std::string_view::npos || !line.ends_with(']')) return false;
            for (char c : line.substr(open + 4, line.size() - open - 5))
                if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    };
    const std::size_t log = text.find(kLog);
    const std::size_t at = text.rfind(kStack);
    if (at == std::string_view::npos || (log != std::string_view::npos && at < log) ||
        !frames_only(text.substr(at + kStack.size())))
        return redact_chat(text);
    return redact_chat(text.substr(0, at)) + std::string(text.substr(at));
}

std::string bug_report_text(const BugReport& r, const std::vector<std::string>& included,
                            const std::vector<std::string>& left_out) {
    std::string out = r.crash ? "Hailstone crash report\n" : "Hailstone bug report\n";
    out += std::string(out.size() - 1, '=') + "\n\n";
    out += "Time:        " + format_time(r.time, false) + " (" + format_time(r.time, true) + ")\n";
    out += "Category:    " + (r.category.empty() ? std::string("(none)") : r.category) + "\n";
    if (!r.flags.empty()) {
        out += "Marked:      ";
        for (std::size_t i = 0; i < r.flags.size(); ++i) out += (i ? ", " : "") + r.flags[i];
        out += "\n";
    }
    out += "\n";
    for (const auto& [label, value] : r.facts) {
        if (value.empty()) continue;
        std::string l = label + ":";
        if (l.size() < 13) l.resize(13, ' ');
        out += l + value + "\n";
    }
    out += "\nWhat happened\n-------------\n";
    out += r.text.empty() ? std::string("(nothing written)\n") : r.text + (r.text.ends_with('\n') ? "" : "\n");
    out += "\nIn this archive\n---------------\n";
    for (const auto& f : included) out += "  " + f + "\n";
    if (!left_out.empty()) {
        out += "\nLeft out to stay under the upload limit\n---------------------------------------\n";
        for (const auto& f : left_out) out += "  " + f + "\n";
    }
    return out;
}

std::string bug_report_summary(const BugReport& r, std::string_view zip_name) {
    constexpr std::size_t kMax = 1900; // a Discord message holds 2000
    std::string title = first_line(r.text, 80);
    if (title.empty() && r.crash) // the crash's cause ("signal 11 (SIGSEGV, ...)")
        for (const auto& [label, value] : r.facts)
            if (label == "What") title = value;
    if (title.empty()) title = r.crash ? "Crash" : "Bug report";
    std::string out = "**[" + (r.category.empty() ? std::string(r.crash ? "Crash" : "Bug") : r.category) + "] " + title +
                      "**\n";
    for (const auto& [label, value] : r.facts)
        if (!value.empty()) out += "- **" + label + ":** " + value + "\n";
    if (!r.flags.empty()) {
        out += "- **Marked:** ";
        for (std::size_t i = 0; i < r.flags.size(); ++i) out += (i ? ", " : "") + r.flags[i];
        out += "\n";
    }
    std::string attached = "\nAttached: `" + std::string(zip_name) + "`\n";
    if (!r.text.empty()) {
        std::string quote = "\n";
        std::string_view rest = r.text;
        while (!rest.empty()) {
            const std::size_t end = rest.find('\n');
            quote += "> " + std::string(rest.substr(0, end)) + "\n";
            rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        }
        const std::size_t room = kMax > out.size() + attached.size() ? kMax - out.size() - attached.size() : 0;
        static constexpr std::string_view kCut = "...\n(the rest is in report.txt)\n";
        if (quote.size() > room) quote = room > kCut.size() + 10 ? quote.substr(0, room - kCut.size()) + std::string(kCut) : "";
        out += quote;
    }
    out += attached;
    if (out.size() > kMax) out = out.substr(0, kMax - 4) + "...\n";
    return out;
}

std::optional<SavedBugReport> save_bug_report(const BugReport& report, const fs::path& dir, std::size_t limit) {
    auto clean = [&](std::string_view text) { return redact_private(text, report.accounts, report.home); };
    // The player's words and the facts too: a fact may hold a path, the words an account name.
    BugReport r = report;
    r.category = clean(r.category);
    for (std::string& f : r.flags) f = clean(f);
    r.text = clean(r.text);
    for (auto& fact : r.facts) fact.second = clean(fact.second);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::tm local = platform::local_time(r.time).value_or(std::tm{});
    SavedBugReport saved;
    saved.zip = r.file;
    for (int n = 1; saved.zip.empty(); ++n)
        if (fs::path p = dir / bug_report_file_name(r.crash, local, n); !fs::exists(p, ec)) saved.zip = p;
    const std::size_t room = limit - std::min(kTextReserve, limit / 4);

    ZipWriter zip;
    std::vector<std::string> included = {"report.txt", "summary.md"};
    auto put = [&](const std::string& name, std::string_view data) {
        if (zip.add_within(name, bytes(data), local, room)) {
            included.push_back(name);
            return true;
        }
        return false;
    };
    for (const fs::path& f : r.crash_files) {
        // crash-<date>.txt, also once marked reported (crash-<date>.reported.txt).
        std::string name = "crash/" + f.filename().string();
        if (const std::size_t at = name.find(".reported."); at != std::string::npos) name.erase(at, 9);
        // A Windows minidump is binary: a changed length would break it, so it goes in as it is.
        const bool binary = lower(f.extension().string()) == ".dmp";
        if (auto text = read_file(f); text && !put(name, binary ? *text : clean(redact_crash_report(*text))))
            saved.left_out.push_back(name);
    }
    for (const fs::path& f : r.settings)
        if (auto text = read_file(f))
            put("settings/" + f.filename().string(), clean(scrub_settings(*text)));
    for (const fs::path& f : r.logs) {
        const std::string name = "logs/" + f.filename().string();
        auto text = read_file(f, kLogTail);
        if (!text) continue;
        // Too big even packed: the last part only.
        if (!put(name, clean(redact_chat(*text))) &&
            !((text = read_file(f, 2u << 20)) && put(name, clean(redact_chat(*text)))))
            saved.left_out.push_back(name);
    }
    if (r.capture_png || !r.capture.empty())
        if (auto png = r.capture_png ? r.capture_png : read_file(r.capture); png && !png->empty() &&
                                                                              !put("screenshots/at-report.png", *png))
            saved.left_out.push_back("screenshots/at-report.png");
    for (const fs::path& f : recent_screenshots(r.screenshots, r.time)) {
        const std::string name = "screenshots/" + f.filename().string();
        if (auto png = read_file(f); png && !put(name, *png)) saved.left_out.push_back(name);
    }

    saved.summary = bug_report_summary(r, saved.zip.filename().string());
    zip.add("report.txt", bug_report_text(r, included, saved.left_out), local);
    zip.add("summary.md", saved.summary, local);
    if (!zip.write(saved.zip)) return std::nullopt;
    saved.size = zip.finished_size();
    log::info("bug report saved to {} ({} KiB{})", saved.zip.string(), saved.size / 1024,
              saved.left_out.empty() ? "" : ", some files left out to fit");
    prune_bug_reports(saved.zip.parent_path());
    return saved;
}

std::future<std::optional<SavedBugReport>> save_bug_report_async(BugReport report, fs::path dir, std::size_t limit) {
    return std::async(std::launch::async, [report = std::move(report), dir = std::move(dir), limit] {
        auto saved = save_bug_report(report, dir, limit);
        if (!saved) log::error("could not save the bug report in {}", dir.string());
        return saved;
    });
}

void prune_bug_reports(const fs::path& dir, std::size_t keep) {
    std::vector<std::pair<fs::file_time_type, fs::path>> zips;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (e.is_regular_file(ec) && name.starts_with("hailstone-") && name.ends_with(".zip"))
            zips.emplace_back(e.last_write_time(ec), e.path());
    }
    if (zips.size() <= keep) return;
    std::sort(zips.begin(), zips.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = keep; i < zips.size(); ++i) fs::remove(zips[i].second, ec);
}

std::vector<fs::path> unreported_crashes(const fs::path& logs) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(logs, ec)) {
        const std::string name = e.path().filename().string();
        if (e.is_regular_file(ec) && name.starts_with("crash-") && name.ends_with(".txt") &&
            !name.ends_with(".reported.txt"))
            out.push_back(e.path());
    }
    std::sort(out.begin(), out.end()); // the names hold the date
    return out;
}

void mark_crash_reported(const fs::path& crash) {
    std::error_code ec;
    fs::path stem = crash;
    stem.replace_extension();
    fs::rename(crash, fs::path(stem.string() + ".reported.txt"), ec);
    fs::path dump = fs::path(stem.string() + ".dmp");
    if (fs::exists(dump, ec)) fs::rename(dump, fs::path(stem.string() + ".reported.dmp"), ec);
}

} // namespace hailstone::app
