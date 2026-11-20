// ClientApp: the updater (app/updater.h, docs/updater.md). Started with a player's own run, it checks the release
// feed in the background; its news goes into the login view, whose buttons download, skip, open the release's page
// or restart into the new version. A downloaded update is installed when the client has quit (run_client).

#include "app/client_app_internal.h"
#include "app/update_install.h"
#include "core/build_info.h"
#include "core/atomic_file.h"
#include "core/changelog.h"
#include "platform/http_get.h"
#include "platform/open_folder.h"

namespace hailstone::app {

namespace client_internal {

void ClientApp::start_updater() {
    ui::UpdateNotice& n = login_view_.update;
    n.current = release_version();
    const std::filesystem::path install = platform::install_dir();
    // What's new: this release's part of the CHANGELOG.md shipped with it.
    if (auto text = read_text_file(install / std::string(kChangelogName)))
        login_view_.release_notes = changelog_section(*text, release_version());
    login_view_.project_page = default_project_page();
    n.check = "off";
    if (!opts_.updates) {
        n.off_reason = opts_.box_child ? "a box set's window (the first window updates)" : "a test run";
        return;
    }
    n.message = opts_.update_note;
    update_settings_ = update_settings(config_);
    if (!update_settings_.check) {
        log::info("Updates: not checked (hailstone.ini update.check=0)");
        n.off_reason = "update.check is 0 in hailstone.ini";
        return;
    }
    if (std::string off = updates_off_reason(install); !off.empty()) {
        log::info("Updates: off ({})", off);
        n.off_reason = off;
        return;
    }
    std::string why;
    if (!platform::http_available(&why)) {
        log::warn("Updates: off ({})", why);
        n.off_reason = "no HTTPS on this system";
        return;
    }
    n.check = "checking";
    UpdaterEnv env;
    env.install_dir = install;
    env.feed = update_settings_.feed.empty() ? default_update_feed() : update_settings_.feed;
    env.version = release_version();
    env.platform = std::string(update_platform());
    env.key = *update_release_key();
    env.auto_download = update_settings_.download;
    env.skip = update_settings_.skip;
    const std::string agent = std::string("Hailstone/") + release_version();
    env.fetch = [agent](const std::string& url, const std::function<bool(std::span<const std::byte>)>& sink,
                        const std::function<bool(std::uint64_t, std::uint64_t)>& progress,
                        const std::function<bool()>& cancelled) {
        platform::HttpGetOptions options;
        options.user_agent = agent;
        options.cancelled = cancelled;
        return platform::http_get(url, options, sink, progress);
    };
    log::info("Updates: checking {} for a release newer than {}", env.feed, env.version);
    updater_ = std::make_unique<Updater>(std::move(env));
    updater_->check();
}

void ClientApp::update_updater() {
    if (!opts_.updates) return;
    // The login screen is up: an update installed at the last quit started fine.
    if (!update_confirmed_ && screen_ == Screen::Login) {
        update_confirmed_ = true;
        update_started_fine(platform::install_dir(), release_version());
        retire_old_release_files(platform::install_dir());
    }
    if (!updater_) return;
    const UpdateStatus s = updater_->status();
    ui::UpdateNotice& n = login_view_.update;
    n.version = s.version;
    n.notes = s.notes;
    n.error = s.error;
    n.has_package = s.has_package;
    n.install_on_quit = update_installs_at_quit();
    n.can_restart = box_clients_.running() == 0;
    n.progress = s.total ? static_cast<float>(static_cast<double>(s.done) / static_cast<double>(s.total)) : -1.0f;
    switch (s.state) {
    case UpdateState::Available: n.state = s.skipped ? "" : "available"; break;
    case UpdateState::Downloading: n.state = "downloading"; break;
    case UpdateState::Ready: n.state = "ready"; break;
    // A check that failed (offline, no manifest yet) is only logged; a download that failed can be tried again.
    case UpdateState::Failed: n.state = s.has_package ? "failed" : ""; break;
    default: n.state.clear(); break;
    }
    switch (s.state) {
    case UpdateState::Idle:
    case UpdateState::Checking: n.check = "checking"; break;
    case UpdateState::UpToDate: n.check = "uptodate"; break;
    // A failed check has no version; a failed download does.
    case UpdateState::Failed: n.check = s.version.empty() ? "unknown" : "newer"; break;
    // (a skipped release is still newer than this build: the corner says so, with no buttons, as its state is "")
    default: n.check = "newer"; break;
    }
}

void ClientApp::update_login_action(ui::LoginAction action) {
    if (!updater_) return;
    const UpdateStatus s = updater_->status();
    switch (action) {
    case ui::LoginAction::UpdateDownload:
        update_asked_ = true;
        updater_->download();
        break;
    case ui::LoginAction::UpdateRestart:
        if (s.state == UpdateState::Ready && box_clients_.running() == 0) {
            update_restart_ = true;
            quit_ = true;
        }
        break;
    case ui::LoginAction::UpdateSkip:
        updater_->skip();
        config_.set("update.skip", s.version);
        config_.save(config_path_);
        break;
    case ui::LoginAction::UpdateNotes:
        if (!s.notes.empty() && !platform::open_url(s.notes))
            login_view_.status = {"Could not open " + s.notes, true};
        break;
    default: break;
    }
}

bool ClientApp::update_installs_at_quit() const { return app::update_installs_at_quit(update_settings_, update_asked_); }

ClientApp::UpdateExit ClientApp::update_exit() {
    UpdateExit e;
    if (!updater_ || updater_->status().state != UpdateState::Ready) return e;
    box_clients_.poll();
    // The box set's other windows run this folder's files too: the update waits for a quit without them.
    if (box_clients_.running() > 0) {
        log::info("Update: not installed while the box set's other windows run; it is, at a later quit");
        return e;
    }
    e.restart = update_restart_;
    e.install = update_restart_ || update_installs_at_quit();
    return e;
}

} // namespace client_internal

void finish_update_at_exit(bool restart) {
    const std::filesystem::path install = platform::install_dir();
    const InstallResult r = install_staged_update(install, release_version());
    if (!r.ok) {
        log::warn("Update: not installed: {}", r.error);
        return;
    }
    if (restart) start_installed_client();
}

void start_installed_client() {
    const std::filesystem::path install = platform::install_dir();
#ifdef _WIN32
    const std::filesystem::path exe = install / "hailstone.exe";
#else
    const std::filesystem::path exe = install / "hailstone";
#endif
    platform::ChildProcesses child; // forgotten, not stopped, as it goes out of scope
    std::string error;
    if (!child.start({exe.string()}, error)) log::warn("Update: cannot start {}: {}", exe.string(), error);
}

} // namespace hailstone::app
