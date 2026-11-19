// ClientApp: bug reports (app/bug_report.h). The bug report window's screenshot and saved report, and the report
// made at the start from the last run's crash.

#include "app/client_app_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>

#include "core/atomic_file.h"
#include "core/build_info.h"
#include "core/file_time.h"
#include "game/coords.h"
#include "game/eq_names.h"
#include "platform/process.h"

namespace hailstone::app::client_internal {

namespace {
namespace fs = std::filesystem;

// The log the crashed run wrote, from the crash report's "log:" line: this start's log moved that file to
// <name>.1<ext> when it reopened the same name (core/log.h).
fs::path crashed_run_log(const fs::path& crash) {
    auto text = read_text_file(crash);
    if (!text) return {};
    const std::size_t at = text->find("\nlog: ");
    if (at == std::string::npos) return {};
    const fs::path log = text->substr(at + 6, text->find('\n', at + 6) - at - 6);
    std::error_code ec;
    const fs::path rotated = log.parent_path() / (log.stem().string() + ".1" + log.extension().string());
    if (fs::equivalent(log, log::file_path(), ec) && fs::exists(rotated, ec)) return rotated;
    return fs::exists(log, ec) ? log : fs::path();
}

std::string first_line_after(const std::string& text, std::string_view key) {
    const std::size_t at = text.find(key);
    if (at == std::string::npos) return {};
    const std::size_t from = at + key.size();
    return text.substr(from, text.find('\n', from) - from);
}
} // namespace

void ClientApp::wire_bug_reports(UiHost& h) {
    if (!h.save_bug_report) {
        h.bug_report_capture = [this] {
            // The frame the window hides for; a smaller copy, so the report keeps room for more.
            bug_capture_ = platform::cache_dir() / "bug-report-capture.png";
            std::error_code ec;
            fs::remove(bug_capture_, ec); // never last report's picture
            vk_.request_screenshot(bug_capture_, 1920);
        };
        h.bug_report_now = [this](BugReport& r) { fill_bug_report(r); };
        h.save_bug_report = [this](BugReport r) { return save_bug_report(std::move(r)); };
        h.crash_report_note = [this](const std::string& note) {
            if (!crash_report_ || !crash_saved_) return;
            crash_report_->text = note;
            crash_report_->file = crash_saved_->zip;
            if (auto saved = app::save_bug_report(*crash_report_, platform::bug_reports_dir())) crash_saved_ = std::move(saved);
        };
        h.crash_report_close = [this] {
            crash_saved_.reset();
            ui_host_.crash_report = nullptr; // not left pointing at the freed report until the next frame
        };
    }
    h.crash_report = crash_saved_ ? &*crash_saved_ : nullptr;
}

void ClientApp::add_private_names(BugReport& r) const {
    // Every saved server's accounts: a log can name one played earlier, or a box window's.
    std::vector<std::string>& names = r.accounts;
    for (const ServerProfile& p : list_profiles()) {
        names.push_back(p.username);
        for (const SavedAccount& a : p.accounts) names.push_back(a.username);
    }
    names.push_back(login_view_.username);
    for (const SavedAccount& a : login_view_.accounts) names.push_back(a.username);
    names.push_back(opts_.account);
    names.push_back(opts_.auto_user);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    r.home = platform::home_dir();
}

std::future<std::optional<SavedBugReport>> ClientApp::save_bug_report(BugReport r) {
    if (r.time == 0) fill_bug_report(r); // not filled at its Send (bug_report_now)
    // The files are copied, cleaned and packed on a thread of their own.
    return save_bug_report_async(std::move(r), platform::bug_reports_dir());
}

void ClientApp::fill_bug_report(BugReport& r) {
    r.time = std::time(nullptr);
    std::vector<std::pair<std::string, std::string>> facts = {
        {"Build", std::string("Hailstone ") + build_version()},
        {"System", platform::system_description()},
        {"GPU", vk_.gpu_description()},
        {"Display", std::format("{}x{}, {:.0f} fps", vk_.swapchain_extent().width, vk_.swapchain_extent().height, fps_)},
        {"UI", ui_name_},
        {"Server", server_profile_.name.empty() || server_profile_.name == server_profile_.host
                       ? std::format("{}:{}", server_profile_.host, server_profile_.port)
                       : std::format("{} ({}:{})", server_profile_.name, server_profile_.host,
                                     server_profile_.port)},
        {"World", char_view_.server_name},
    };
    if (conn_) {
        const auto& w = conn_->world();
        if (const game::Spawn* me = w.player()) {
            facts.emplace_back("Character", std::format("{}, level {} {} {}", conn_->character_name(), me->level,
                                                         game::race_name(me->race), game::stock_class_name(me->class_id)));
            facts.emplace_back("Zone", std::format("{} ({}, id {}, instance {})", w.zone_long_name, w.zone_short_name,
                                                   conn_->zone_id(), conn_->zone_instance()));
            const glm::vec3 at = game::world_to_server(me->position);
            facts.emplace_back("Location", std::format("{:.1f}, {:.1f}, {:.1f}, heading {:.0f}", at.x, at.y, at.z, me->heading));
        }
        if (auto it = w.spawns.find(w.target_id); it != w.spawns.end()) {
            const game::Spawn& t = it->second;
            facts.emplace_back("Target", std::format("{} (id {}, level {} {} {})", t.name, t.id, t.level,
                                                     game::race_name(t.race), game::stock_class_name(t.class_id)));
        }
    }
    // The window's own (the place it took) after the client's.
    facts.insert(facts.end(), r.facts.begin(), r.facts.end());
    r.facts = std::move(facts);
    const fs::path log = log::file_path();
    if (!log.empty()) {
        r.logs.push_back(log);
        r.logs.push_back(log.parent_path() / (log.stem().string() + ".1" + log.extension().string())); // the last run
    }
    r.settings = {config_path_, platform::ui_settings_file(ui_name_)};
    if (!ui_character_dir_.empty()) r.settings.push_back(CharacterFiles{ui_character_dir_}.ui_settings(ui_name_));
    r.capture = bug_capture_;
    // Its bytes now: the window opened again deletes and writes that file while this report may still be saved.
    if (!bug_capture_.empty()) r.capture_png = read_text_file(bug_capture_).value_or(std::string());
    r.screenshots = platform::screenshots_dir();
    add_private_names(r);
}

void ClientApp::report_last_crash() {
    const std::vector<fs::path> crashes = unreported_crashes(platform::logs_dir());
    if (crashes.empty()) return;
    // The newest crash is reported with its run's log; older ones left behind are only marked.
    const fs::path crash = crashes.back();
    BugReport r;
    r.crash = true;
    r.category = "Crash";
    std::error_code ec;
    r.time = file_time_to_time_t(fs::last_write_time(crash, ec));
    if (ec) r.time = std::time(nullptr);
    const std::string text = read_text_file(crash).value_or("");
    r.facts = {
        {"What", first_line_after(text, "\nwhat: ")},
        {"Build", first_line_after(text, "\nbuild: ")},
        {"Now running", std::string("Hailstone ") + build_version()},
        {"System", platform::system_description()},
        {"GPU", vk_.gpu_description()},
        {"UI", ui_name_},
    };
    if (fs::path log = crashed_run_log(crash); !log.empty()) r.logs.push_back(log);
    r.settings = {config_path_};
    fs::path stem = crash;
    stem.replace_extension();
    r.crash_files.push_back(crash);
    if (fs::path dump = fs::path(stem.string() + ".dmp"); fs::exists(dump, ec)) r.crash_files.push_back(dump);
    add_private_names(r);
    crash_saved_ = app::save_bug_report(r, platform::bug_reports_dir());
    // Marked only once the report is on disk: a failed save offers the crash again next start.
    if (!crash_saved_) return;
    log::info("the last run crashed: report saved to {}", crash_saved_->zip.string());
    for (const fs::path& c : crashes) mark_crash_reported(c);
    // Its files under their new names, for a note added later.
    r.crash_files.clear();
    r.crash_files.push_back(fs::path(stem.string() + ".reported.txt"));
    if (fs::path dump = fs::path(stem.string() + ".reported.dmp"); fs::exists(dump, ec)) r.crash_files.push_back(dump);
    crash_report_ = std::move(r);
}

} // namespace hailstone::app::client_internal
