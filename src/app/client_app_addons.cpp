// ClientApp: the add-on manager (app/addon_manager.h, docs/addons.md). At start a cut-off install is undone and the
// installed add-ons' capability ceilings go to the policy; with HTTPS (and on a player's own run) the manager looks
// for newer versions. Its news and the list of scripts and plugins (the installed ones, the ones put in by hand, the
// picked server's own) go into the login view; what is downloaded is installed while the login screen is up and
// nothing else of this folder runs (no login under way, no box set window).

#include "app/addon_install.h"
#include "app/addon_manager.h"
#include "app/addon_suggest.h"
#include "app/client_app_internal.h"
#include "core/addon_source.h"
#include "core/build_info.h"
#include "core/distribution.h"
#include "core/server_profile.h"
#include "core/update_manifest.h"
#include "core/version.h"
#include "net/server_files.h"
#include "platform/http_get.h"
#include "platform/open_folder.h"
#include "plugin/consent.h"

namespace hailstone::app {

namespace client_internal {

namespace {

std::string state_text(const AddonStatus& s) {
    const std::string v = s.new_version.empty() ? std::string("A newer version") : "Version " + s.new_version;
    switch (s.state) {
    case AddonState::Checking: return "Looking for a newer version...";
    case AddonState::Available: return v + " is out.";
    case AddonState::Downloading: return s.installed ? "Downloading " + s.new_version + "..." : "Downloading...";
    case AddonState::Ready: return v + " is downloaded: it is installed at the login screen.";
    case AddonState::NeedsApproval: return v + " asks to do more (below): approve it to update.";
    case AddonState::Modified:
        return v + " is out, but you changed files of this one: updating replaces them.";
    case AddonState::Blocked: return v + " is not installed: " + s.error;
    case AddonState::Failed: return s.error;
    default: return {};
    }
}

std::string repo_page(const std::string& source) {
    auto s = parse_addon_link(source);
    return s ? addon_repo_page(*s) : std::string();
}

// Add-on names compare in any case (addon_name_ok: ASCII letters, digits, '-' and '_').
bool same_name(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto low = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (low(a[i]) != low(b[i])) return false;
    }
    return true;
}

std::string suggest_never_key(std::string_view profile) { return "addons.suggest_never." + std::string(profile); }

std::string path_text(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// An installed tool whose program is in its folder.
bool tool_program_there(const std::filesystem::path& data, const InstalledAddon& a) {
    if (a.kind != AddonKind::Tool) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(
        addon_folder(data, a.kind, a.name) / addon_tool_program(a.name, update_platform()), ec);
}

} // namespace

void ClientApp::start_addons() {
    if (distribution::managed()) {
        login_view_.addons.available = false;
        login_view_.addons.open = false;
        login_view_.addons.why_off = "This distribution includes its required add-on; user add-ons are disabled.";
        return;
    }
    const std::filesystem::path data = platform::data_dir();
    addons_at_start(data);
    if (automation_) automation_->set_addon_ceilings(addon_ceilings(data));
    ui::AddonsView& v = login_view_.addons;
    if (!opts_.updates) {
        v.why_off = "Add-ons are not fetched on a test run.";
        return;
    }
    std::string why;
    if (!platform::http_available(&why)) {
        log::warn("Add-ons: none fetched ({})", why);
        v.why_off = "Add-ons cannot be fetched here: " + why + ".";
        return;
    }
    AddonsEnv env;
    env.data_dir = data;
    env.client_version = release_version();
    env.platform = std::string(update_platform());
    env.settings = addon_settings(config_);
    // The add-on index (docs/addons.md), signed with the release key: the store lists it, and the suggestions come
    // from it (hailstone.ini addons.suggest = 0 turns those off). addons.index names another index (on GitHub).
    env.index_url = config_.get_string("addons.index", default_addon_index());
    env.index_key = update_release_key();
    const std::string agent = std::string("Hailstone/") + release_version();
    env.fetch = [agent](const std::string& url, const std::function<bool(std::string_view)>& allow_url,
                        const std::function<bool(std::span<const std::byte>)>& sink,
                        const std::function<bool()>& keep_going) {
        platform::HttpGetOptions options;
        options.user_agent = agent;
        options.allow_url = allow_url;
        options.cancelled = [&keep_going] { return !keep_going(); }; // quitting does not wait out a stalled fetch
        return platform::http_get(url, options, sink,
                                  [&keep_going](std::uint64_t, std::uint64_t) { return keep_going(); });
    };
    addons_ = std::make_unique<AddonManager>(std::move(env));
    v.available = true;
    addons_->check_updates(false);
    addons_->refresh_index(false);
}

std::vector<AddonSuggestion> ClientApp::suggestions_for(std::string_view profile, std::string_view host,
                                                        const std::vector<std::string>& worlds,
                                                        const AddonIndex* index, const plugin::Rules& rules,
                                                        const std::vector<ServerAddon>& server_own) {
    if (distribution::managed()) return {};
    if (!addons_ || profile.empty() || !config_.get_bool("addons.suggest", true)) return {};
    AddonsHad had;
    for (const auto& a : read_installed_addons(platform::data_dir())) {
        had.names.push_back(a.name);
        had.sources.push_back(a.source);
    }
    for (const auto& m : manual_addons_) had.names.push_back(m.name);
    for (const auto& a : server_own) had.names.push_back(a.name);
    std::vector<std::string> dismissed = parse_dismissed_suggestions(config_.get_string(suggest_never_key(profile)));
    const std::string later_prefix = std::string(profile) + "\n";
    for (const auto& k : suggest_later_)
        if (k.starts_with(later_prefix)) dismissed.push_back(k.substr(later_prefix.size()));
    return addon_suggestions(index, host, worlds, rules.suggest_addons, had, dismissed);
}

void ClientApp::update_server_select_hint() {
    server_view_.addon_hint.clear();
    if (distribution::managed()) return;
    if (!addons_) return;
    std::vector<std::string> worlds;
    for (const auto& s : servers_) worlds.push_back(s.name);
    const AddonsStatus st = addons_->status();
    const std::filesystem::path dir =
        server_profile_.id.empty() ? std::filesystem::path() : login_view_.servers_dir / server_profile_.id;
    const auto list =
        dir.empty() ? std::vector<AddonSuggestion>{}
                    : suggestions_for(server_profile_.id, server_profile_.host, worlds, st.index ? &*st.index : nullptr,
                                      net::cached_server_rules(dir), server_addons(dir));
    if (list.empty()) return;
    // The add-on is installed from the login screen (nothing of this folder may run while its files move).
    server_view_.addon_hint = addon_suggestion_text(list.front(), server_profile_.name) +
                              " Go Back and open Add-ons to look at it before you play.";
}

void ClientApp::update_addons() {
    if (distribution::managed()) return;
    const std::filesystem::path data = platform::data_dir();
    const bool at_login = screen_ == Screen::Login;
    // Nothing of this folder runs now: the files can move.
    if (addons_ && at_login && !login_view_.busy && !login_ && !conn_ && box_clients_.running() == 0 &&
        addons_->has_ready())
        addons_->install_ready();
    // Each frame in the world too: only the serial, not a copy of the whole status (the index, the store).
    const std::uint32_t serial = addons_ ? addons_->serial() : 0;
    if (serial != addons_serial_ || addons_dirty_) {
        addons_serial_ = serial;
        addons_dirty_ = false;
        if (automation_) automation_->set_addon_ceilings(addon_ceilings(data));
        addons_listed_ = {};
    }
    if (!at_login) return;
    const AddonsStatus st = addons_ ? addons_->status() : AddonsStatus{};

    // The lists beside the add-ons read folders: when the picked server changes, and every few seconds.
    const std::string profile = ui::selected_profile_id(login_view_);
    const auto now = Clock::now();
    if (profile != addons_profile_ || now - addons_listed_ > std::chrono::seconds(3)) {
        addons_profile_ = profile;
        addons_listed_ = now;
        const std::filesystem::path profile_dir =
            profile.empty() ? std::filesystem::path() : login_view_.servers_dir / profile;
        server_addons_ = server_addons(profile_dir);
        addons_rules_ = profile_dir.empty() ? plugin::Rules{} : net::cached_server_rules(profile_dir);
        std::vector<std::filesystem::path> dirs{data};
        if (platform::install_dir() != data) dirs.push_back(platform::install_dir());
        manual_addons_ = manual_addons(dirs, read_installed_addons(data));
        addons_listed_worlds_ = profile.empty() ? std::vector<std::string>{}
                                                : listed_worlds(profile, login_view_.servers_dir);
        suggestions_ = suggestions_for(profile, login_view_.host, addons_listed_worlds_,
                                       st.index ? &*st.index : nullptr, addons_rules_, server_addons_);
    }

    ui::AddonsView& v = login_view_.addons;
    v.inspecting = st.inspecting;
    // The manager's news when it has new news (the client's own messages, an unopened folder, stay till then).
    if (st.message_serial != addons_message_serial_) {
        addons_message_serial_ = st.message_serial;
        v.status = {st.message, st.message_error};
    }
    v.server = profile.empty() ? std::string() : login_view_.name;
    v.suggestions.clear();
    for (const AddonSuggestion& sg : suggestions_)
        v.suggestions.push_back({sg.name, sg.source, addon_suggestion_text(sg, v.server), sg.from_server});
    v.offer.reset();
    if (st.offer) {
        const AddonOffer& o = *st.offer;
        ui::AddonOfferView ov;
        ov.name = o.manifest.name;
        ov.kind = std::string(addon_kind_word(o.manifest.kind));
        ov.version = o.manifest.version_text;
        ov.author = o.manifest.author;
        ov.description = o.manifest.description;
        ov.source = o.source;
        ov.page = o.page;
        ov.notes = o.manifest.notes;
        if (o.manifest.key) ov.key = hex_encode(*o.manifest.key).substr(0, 16);
        ov.installed_version = o.installed_version;
        ov.older = o.older;
        ov.blocked = o.blocked;
        ov.caps = plugin::caps_described(o.caps);
        v.offer = std::move(ov);
    }

    // The store: each add-on of the index, and whether the player has it.
    v.browsing = st.browsing;
    v.browsed = st.browsed;
    v.store_error = st.catalog_error;
    v.store.clear();
    for (const AddonCatalogEntry& e : st.catalog) {
        ui::AddonStoreItemView item;
        item.name = e.addon.name;
        item.source = e.addon.source;
        item.page = repo_page(e.addon.source);
        for (const auto& w : e.worlds) item.worlds += (item.worlds.empty() ? "" : ", ") + w;
        item.problem = e.problem;
        if (e.manifest) {
            item.kind = std::string(addon_kind_word(e.manifest->kind));
            item.version = e.manifest->version_text;
            item.author = e.manifest->author;
            item.description = e.manifest->description;
        }
        const InstalledAddon* had = nullptr;
        const std::string repo = addon_repo_key(e.addon.source);
        for (const AddonStatus& a : st.addons)
            if (a.installed && ((!repo.empty() && addon_repo_key(a.addon.source) == repo) ||
                                same_name(a.addon.name, e.addon.name)))
                had = &a.addon;
        bool manual = false;
        for (const ManualAddon& m : manual_addons_) manual = manual || same_name(m.name, e.addon.name);
        if (had) {
            item.installed_version = had->version;
            const auto mine = parse_version(had->version);
            const auto theirs = e.manifest ? parse_version(e.manifest->version_text) : std::nullopt;
            item.state = mine && theirs && compare_versions(*theirs, *mine) > 0 && e.problem.empty() ? "update"
                                                                                                    : "installed";
        } else if (manual) {
            item.state = "manual";
        } else {
            item.state = e.problem.empty() ? "install" : "unavailable";
        }
        v.store.push_back(std::move(item));
    }

    // What this server does with the player's own: its hailstone.cfg as the last login left it.
    const plugin::Rules& rules = addons_rules_;
    auto server_note = [&](AddonKind kind, const std::string& name, std::uint32_t caps) -> std::string {
        if (profile.empty() || kind == AddonKind::Tool) return {}; // a tool is no server's business: it is not run in it
        for (const auto& s : server_addons_)
            if (s.kind == kind && s.name == name) return "This server has its own " + name + ": that one runs there.";
        if (!rules.configured) return {};
        const std::uint32_t allowed = kind == AddonKind::Script ? rules.player_script_caps : rules.player_plugin_caps;
        const char* what = kind == AddonKind::Script ? "scripts" : "plugins";
        if (allowed == 0) return std::string("This server runs only its own ") + what + ": it does not run here.";
        if (caps & ~allowed) return "This server allows only part of what it may do.";
        return {};
    };

    v.rows.clear();
    for (const AddonStatus& s : st.addons) {
        const InstalledAddon& a = s.addon;
        ui::AddonRowView r;
        r.name = a.name;
        r.kind = std::string(addon_kind_word(a.kind));
        r.group = "installed";
        r.version = s.installed ? a.version : std::string();
        r.source = a.source;
        r.page = repo_page(a.source);
        r.author = a.author;
        r.description = a.description;
        r.state = std::string(addon_state_word(s.state));
        r.state_text = state_text(s);
        r.new_version = s.new_version;
        r.caps = plugin::caps_described(a.caps);
        if (s.state == AddonState::NeedsApproval) r.new_caps = plugin::caps_described(s.new_caps & ~a.caps);
        r.approve_caps = s.state == AddonState::NeedsApproval ? s.new_caps : a.caps;
        r.approve_version = s.new_version;
        r.modified = s.modified;
        r.note = server_note(a.kind, a.name, a.caps);
        r.auto_update = a.auto_update;
        r.signed_ = a.key.has_value();
        r.can_update = addons_ != nullptr && s.installed &&
                       (s.state == AddonState::Available || s.state == AddonState::NeedsApproval ||
                        s.state == AddonState::Modified || s.state == AddonState::Failed);
        r.can_remove = s.installed && s.state != AddonState::Downloading && s.state != AddonState::Checking;
        r.folder = path_text(addon_folder(data, a.kind, a.name));
        r.can_start = s.installed && tool_program_there(data, a);
        v.rows.push_back(std::move(r));
    }
    // Without the manager the installed ones are still listed (and can be removed).
    if (!addons_)
        for (const auto& a : read_installed_addons(data)) {
            ui::AddonRowView r;
            r.name = a.name;
            r.kind = std::string(addon_kind_word(a.kind));
            r.group = "installed";
            r.version = a.version;
            r.source = a.source;
            r.page = repo_page(a.source);
            r.author = a.author;
            r.description = a.description;
            r.caps = plugin::caps_described(a.caps);
            r.note = server_note(a.kind, a.name, a.caps);
            r.auto_update = a.auto_update;
            r.signed_ = a.key.has_value();
            r.can_remove = true;
            r.folder = path_text(addon_folder(data, a.kind, a.name));
            r.can_start = tool_program_there(data, a);
            v.rows.push_back(std::move(r));
        }
    for (const ManualAddon& m : manual_addons_) {
        ui::AddonRowView r;
        r.name = m.name;
        r.kind = std::string(addon_kind_word(m.kind));
        r.group = "manual";
        r.note = server_note(m.kind, m.name, 0);
        r.folder = path_text(m.path);
        v.rows.push_back(std::move(r));
    }
    for (const ServerAddon& s : server_addons_) {
        ui::AddonRowView r;
        r.name = s.name;
        r.kind = std::string(addon_kind_word(s.kind));
        r.group = "server";
        const std::string by = "Provided by " + (v.server.empty() ? std::string("this server") : v.server);
        if (s.kind == AddonKind::Plugin) {
            // A compiled plugin runs only with the player's word (plugin/consent.h).
            const plugin::Consent c = s.consent;
            r.consent = c == plugin::Consent::Allowed ? "allowed" : c == plugin::Consent::Denied ? "denied" : "unasked";
            r.note = by + (c == plugin::Consent::Allowed ? ": you allowed it, so it runs there."
                           : c == plugin::Consent::Denied
                               ? ": you did not allow it, so it does not run. The server's features that need it are missing."
                               : ": you are asked whether it may run at your next visit.");
        } else {
            r.note = by + ": it always runs there, and cannot be removed or turned off.";
        }
        r.folder = path_text(login_view_.servers_dir / profile / (s.kind == AddonKind::Script ? "scripts" : "plugins"));
        v.rows.push_back(std::move(r));
    }
}

bool ClientApp::texture_studio_installed() const {
    if (distribution::managed()) return false;
    // Its program only (the Options window asks each frame): starting it checks the add-on's record too.
    InstalledAddon a;
    a.name = std::string(kTextureStudio);
    a.kind = AddonKind::Tool;
    return tool_program_there(platform::data_dir(), a);
}

bool ClientApp::start_texture_studio(std::string* error) {
    if (distribution::managed()) {
        if (error) *error = "User add-on tools are disabled in this distribution.";
        return false;
    }
    const auto all = read_installed_addons(platform::data_dir());
    const auto key = hex_decode<32>(kTextureStudioKey);
    if (find_addon(all, kTextureStudio) && (!key || !find_pinned_addon(all, kTextureStudio, AddonKind::Tool,
                                                                       kTextureStudioRepo, *key))) {
        if (error)
            *error = "The add-on named " + std::string(kTextureStudio) + " is not Hailstone Texture Studio (" +
                     std::string(kTextureStudioRepo) + ", signed with its key): Options does not start it. Start "
                     "it from its row in Add-ons if you trust it.";
        return false;
    }
    return start_addon_tool(kTextureStudio, error);
}

bool ClientApp::start_addon_tool(std::string_view name, std::string* error) {
    if (distribution::managed()) {
        if (error) *error = "User add-on tools are disabled in this distribution.";
        return false;
    }
    const std::filesystem::path data = platform::data_dir();
    const auto all = read_installed_addons(data);
    const InstalledAddon* a = find_addon(all, name);
    if (!a || a->kind != AddonKind::Tool || !tool_program_there(data, *a)) {
        if (error) *error = std::string(name) + " is not installed (Add-ons, on the login screen)";
        return false;
    }
    const std::filesystem::path program =
        addon_folder(data, a->kind, a->name) / addon_tool_program(a->name, update_platform());
    tools_.poll();
    std::string why;
    // The data folder by name: the tool reads and writes the player's (settings, packs, the game folder chosen).
    if (!tools_.start({path_text(program), "--data-dir", path_text(data)}, why)) {
        log::warn("Add-ons: cannot start {}: {}", path_text(program), why);
        if (error) *error = "Could not start " + a->name + ": " + why;
        return false;
    }
    log::info("Add-ons: started {}", path_text(program));
    return true;
}

void ClientApp::addons_login_action(ui::LoginAction action) {
    if (distribution::managed()) return;
    using A = ui::LoginAction;
    ui::AddonsView& v = login_view_.addons;
    const std::filesystem::path data = platform::data_dir();
    const ui::AddonRowView* row = nullptr;
    for (const auto& r : v.rows)
        if (r.name == v.choice && (!row || r.group == "installed")) row = &r;
    switch (action) {
    case A::AddonInspect:
        if (addons_ && !v.link.empty()) addons_->inspect(v.link);
        break;
    case A::AddonInstall:
        if (addons_) {
            addons_->install_offer(v.choice_on);
            v.link.clear();
        }
        break;
    case A::AddonCancel:
        if (addons_) addons_->cancel_offer();
        break;
    case A::AddonUpdate:
        if (addons_ && row && row->can_update) addons_->update(row->name, row->approve_caps, row->approve_version);
        break;
    case A::AddonCheck:
        if (addons_) addons_->check_updates(true);
        break;
    case A::AddonRemove: {
        // Only the manager's own; never the server's.
        if (!row || row->group != "installed" || !row->can_remove) break;
        if (login_ || conn_ || box_clients_.running() > 0) {
            v.status = {"Add-ons are removed only while nothing else of Hailstone runs (no box set window).", true};
            break;
        }
        std::string why;
        const bool ok = addons_ ? addons_->remove(row->name, &why) : remove_addon(data, row->name, &why);
        if (ok) {
            v.status = {row->name + " is removed.", false};
            addons_dirty_ = true;
        } else {
            v.status = {why, true};
        }
        break;
    }
    case A::AddonAuto:
        if (!row || row->group != "installed") break;
        if (addons_) {
            addons_->set_auto_update(row->name, v.choice_on);
        } else {
            auto all = read_installed_addons(data);
            for (auto& a : all)
                if (a.name == row->name) a.auto_update = v.choice_on;
            write_installed_addons(data, all);
        }
        break;
    case A::AddonRevoke:
    case A::AddonAskAgain: {
        // The picked server's own compiled plugin: the player's word on it (plugin/consent.h).
        const std::string profile = ui::selected_profile_id(login_view_);
        bool found = false;
        for (const auto& r : v.rows) found = found || (r.group == "server" && r.kind == "plugin" && r.name == v.choice);
        if (!found || profile.empty()) break;
        const std::filesystem::path dir = login_view_.servers_dir / profile;
        // By the plugin's bytes as the server has them now as well as by its name: the answers are kept by hash, and
        // the same bytes sent under another name would otherwise stay allowed.
        std::vector<Sha256Digest> hashes;
        for (const auto& s : server_addons(dir))
            if (s.kind == AddonKind::Plugin && s.name == v.choice) hashes = s.hashes;
        const bool revoke = action == A::AddonRevoke;
        bool ok = revoke ? plugin::revoke_plugin_consent(dir, v.choice, hashes)
                         : plugin::forget_plugin_consent(dir, v.choice, hashes);
        // Said only when the file now says so for every one of its files.
        const plugin::Consent want = revoke ? plugin::Consent::Denied : plugin::Consent::Unasked;
        for (const auto& h : hashes) ok = ok && plugin::plugin_consent(dir, h) == want;
        if (!ok) v.status = {"Could not change your answer in " + path_text(dir / plugin::kApprovalsName), true};
        else
            v.status = {revoke ? v.choice + " does not run on this server any more."
                               : v.choice + ": you are asked again at your next visit.",
                        false};
        addons_listed_ = {}; // the rows read the answers again
        break;
    }
    case A::AddonSuggestLook:
    case A::AddonSuggestLater:
    case A::AddonSuggestNever: {
        const AddonSuggestion* sg = nullptr;
        for (const auto& x : suggestions_)
            if (x.source == v.choice) sg = &x;
        const std::string profile = ui::selected_profile_id(login_view_);
        if (!sg || profile.empty()) break;
        const std::string key = addon_repo_key(sg->source);
        if (action == A::AddonSuggestLook) {
            if (!addons_) break;
            // The install card, held to the index's name and key when the index lists it.
            std::optional<AddonExpect> expect;
            if (sg->key) expect = AddonExpect{sg->name, *sg->key};
            v.open = true;
            v.link = sg->source;
            addons_->inspect(sg->source, std::move(expect));
            break;
        }
        if (action == A::AddonSuggestNever) {
            auto keys = parse_dismissed_suggestions(config_.get_string(suggest_never_key(profile)));
            keys.push_back(key);
            config_.set(suggest_never_key(profile), join_dismissed_suggestions(keys));
            config_.save(config_path_);
        } else {
            suggest_later_.insert(profile + "\n" + key);
        }
        addons_listed_ = {}; // the suggestions again
        break;
    }
    case A::AddonBrowse:
        if (addons_) addons_->browse(v.choice_on);
        break;
    case A::AddonStoreLook: {
        // The install card, held to the index's name and key as a suggestion's is.
        if (!addons_) break;
        const AddonsStatus st = addons_->status();
        for (const AddonCatalogEntry& e : st.catalog)
            if (e.addon.source == v.choice) {
                v.link = e.addon.source;
                addons_->inspect(e.addon.source, AddonExpect{e.addon.name, e.addon.key});
            }
        break;
    }
    case A::AddonFolder: {
        if (!row || row->folder.empty()) break;
        std::filesystem::path p(std::u8string(reinterpret_cast<const char8_t*>(row->folder.data()), row->folder.size()));
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) p = p.parent_path(); // a script or plugin that is one file
        if (!platform::open_folder(p)) v.status = {"Could not open " + row->folder, true};
        break;
    }
    case A::AddonStart: {
        if (!row || !row->can_start) break;
        std::string why;
        if (start_addon_tool(row->name, &why)) v.status = {row->name + " is starting.", false};
        else v.status = {why, true};
        break;
    }
    case A::AddonPage: {
        const std::string page = v.choice.empty() ? (v.offer ? v.offer->page : std::string()) : (row ? row->page : "");
        if (!page.empty() && !platform::open_url(page)) v.status = {"Could not open " + page, true};
        break;
    }
    default: break;
    }
}

} // namespace client_internal

} // namespace hailstone::app
