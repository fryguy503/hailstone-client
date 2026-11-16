#include "app/addon_manager.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <set>
#include <system_error>

#include "app/update_install.h"
#include "core/addon_source.h"
#include "core/atomic_file.h"
#include "core/binary_header.h"
#include "core/config.h"
#include "core/log.h"
#include "core/version.h"
#include "net/server_files.h"
#include "plugin/native_plugins.h"
#include "plugin/policy.h"
#include "platform/crash_log.h"

namespace hailstone::app {

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kMaxManifestBytes = 64 * 1024;

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool iequal(std::string_view a, std::string_view b) { return lower(a) == lower(b); }

bool newer(const Version& a, const std::string& b) {
    auto v = parse_version(b);
    return !v || compare_versions(a, *v) > 0;
}

// Whether two source texts name the same repository (whatever tag or branch).
bool same_repository(std::string_view a, std::string_view b) {
    auto x = parse_addon_link(a), y = parse_addon_link(b);
    return x && y && iequal(x->owner, y->owner) && iequal(x->repo, y->repo);
}

// installed.txt's auto-update switch for an add-on as it is now (false: not installed).
bool auto_update_on(const fs::path& data_dir, std::string_view name) {
    const auto all = read_installed_addons(data_dir);
    const InstalledAddon* a = find_addon(all, name);
    return a && a->auto_update;
}

std::string joined(const std::vector<std::string>& words) {
    std::string out;
    for (const auto& w : words) out += (out.empty() ? "" : ",") + w;
    return out;
}

fs::path rel(std::string_view name) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
}

} // namespace

AddonSettings addon_settings(const Config& config) {
    AddonSettings s;
    s.check = config.get_bool("addons.check", true);
    s.check_hours = std::max(0, config.get_int("addons.check_hours", 6));
    s.allow_unsigned = config.get_bool("addons.allow_unsigned", true);
    return s;
}

std::string_view addon_state_word(AddonState state) {
    switch (state) {
    case AddonState::Idle: return "idle";
    case AddonState::Checking: return "checking";
    case AddonState::UpToDate: return "uptodate";
    case AddonState::Available: return "available";
    case AddonState::Downloading: return "downloading";
    case AddonState::Ready: return "ready";
    case AddonState::NeedsApproval: return "approval";
    case AddonState::Modified: return "modified";
    case AddonState::Blocked: return "blocked";
    case AddonState::Failed: return "failed";
    }
    return "idle";
}

AddonManager::AddonManager(AddonsEnv env) : env_(std::move(env)) {
    if (!env_.now)
        env_.now = [] {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        };
    {
        // The list at once (the hashes, which read every file, come from the worker).
        std::lock_guard lock(mutex_);
        for (auto& a : read_installed_addons(env_.data_dir)) {
            AddonStatus s;
            s.addon = std::move(a);
            status_.addons.push_back(std::move(s));
        }
    }
    worker_ = std::thread([this] { run(); });
    post([this] { reload_rows(); });
}

AddonManager::~AddonManager() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cancel_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void AddonManager::post(std::function<void()> job) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_all();
}

void AddonManager::run() {
    platform::CrashStack crash_stack;
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            running_job_ = true;
        }
        job();
        {
            std::lock_guard lock(mutex_);
            running_job_ = false;
        }
        cv_.notify_all();
    }
}

void AddonManager::wait_idle() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return stop_ || (jobs_.empty() && !running_job_); });
}

void AddonManager::say(std::string text, bool error) {
    if (error) log::warn("Add-ons: {}", text);
    else log::info("Add-ons: {}", text);
    std::lock_guard lock(mutex_);
    status_.message = std::move(text);
    status_.message_error = error;
    ++status_.message_serial;
}

AddonStatus* AddonManager::row(std::string_view name) {
    for (auto& r : status_.addons)
        if (iequal(r.addon.name, name)) return &r;
    return nullptr;
}

void AddonManager::set_row(const AddonStatus& s) {
    std::lock_guard lock(mutex_);
    set_row_locked(s);
}

void AddonManager::set_row_locked(const AddonStatus& s) {
    if (AddonStatus* r = row(s.addon.name)) {
        *r = s;
        return;
    }
    status_.addons.push_back(s);
    std::sort(status_.addons.begin(), status_.addons.end(),
              [](const AddonStatus& a, const AddonStatus& b) { return lower(a.addon.name) < lower(b.addon.name); });
}

void AddonManager::reload_rows() {
    std::vector<AddonStatus> rows;
    for (auto& a : read_installed_addons(env_.data_dir)) {
        AddonStatus s;
        s.modified = modified_addon_files(env_.data_dir, a);
        s.addon = std::move(a);
        rows.push_back(std::move(s));
    }
    std::lock_guard lock(mutex_);
    for (auto& r : rows)
        if (const AddonStatus* old = row(r.addon.name)) {
            // What this run found out about it stays.
            r.state = old->state;
            r.new_version = old->new_version;
            r.new_caps = old->new_caps;
            r.error = old->error;
        }
    // An add-on whose first install is under way stays listed.
    for (const auto& old : status_.addons)
        if (!old.installed) rows.push_back(old);
    std::sort(rows.begin(), rows.end(),
              [](const AddonStatus& a, const AddonStatus& b) { return lower(a.addon.name) < lower(b.addon.name); });
    status_.addons = std::move(rows);
}

std::optional<std::string> AddonManager::fetch_text(const std::string& url, std::string* error) {
    std::string text;
    auto got = env_.fetch(url, addon_download_allowed, [&](std::span<const std::byte> piece) {
        if (text.size() + piece.size() > kMaxManifestBytes) return false;
        text.append(reinterpret_cast<const char*>(piece.data()), piece.size());
        return !cancel_.load();
    }, [this] { return !cancel_.load(); });
    if (!got.ok) {
        if (error) *error = got.status == 404 ? "404" : got.error;
        return std::nullopt;
    }
    return text;
}

std::optional<std::pair<AddonManifest, std::string>> AddonManager::fetch_manifest(std::string_view source_text,
                                                                                   std::string* error) {
    auto source = parse_addon_link(source_text, error);
    if (!source) return std::nullopt;
    for (const std::string& url : addon_manifest_urls(*source)) {
        std::string why;
        auto text = fetch_text(url, &why);
        if (!text) {
            if (why == "404") continue; // the next address (no release: the branch's file)
            if (error) *error = "cannot reach " + addon_source_text(*source) + ": " + why;
            return std::nullopt;
        }
        auto m = parse_addon_manifest(*text, &why);
        if (!m) {
            if (error) *error = addon_source_text(*source) + ": " + why;
            return std::nullopt;
        }
        return std::make_pair(std::move(*m), url);
    }
    if (error)
        *error = addon_source_text(*source) + " has no " + std::string(kAddonManifestName) +
                 ": it is not a Hailstone add-on (or the repository is private)";
    return std::nullopt;
}

std::string AddonManager::manifest_problem(const AddonManifest& m, std::uint32_t* caps) const {
    if (m.min_client) {
        auto running = parse_version(env_.client_version);
        if (running && compare_versions(*running, *m.min_client) < 0)
            return m.name + " " + m.version_text + " needs Hailstone " + m.min_client_text + " or newer";
    }
    if (m.kind != AddonKind::Script && m.kind != AddonKind::Ui && !addon_package_for(m, env_.platform))
        return m.name + " has no build for " + (env_.platform.empty() ? std::string("this system") : env_.platform);
    // Tools and upscalers request no host capabilities. UI packs use the separately disclosed full-interface
    // trust grant, rather than a script/plugin capability ceiling.
    if (m.kind == AddonKind::Tool || m.kind == AddonKind::Upscaler || m.kind == AddonKind::Ui) {
        if (caps) *caps = 0;
        if (!m.key && !env_.settings.allow_unsigned)
            return m.name + " is not signed, and hailstone.ini addons.allow_unsigned is off";
        return {};
    }
    auto c = plugin::parse_caps_strict(joined(m.caps));
    if (!c) return m.name + " asks for a permission this Hailstone does not know: a newer Hailstone may be needed";
    if (*c == 0) return m.name + " does not say what it needs (its manifest's caps)";
    if (caps) *caps = *c;
    if (!m.key && !env_.settings.allow_unsigned)
        return m.name + " is not signed, and hailstone.ini addons.allow_unsigned is off";
    return {};
}

void AddonManager::inspect(std::string link, std::optional<AddonExpect> expect) {
    {
        std::lock_guard lock(mutex_);
        status_.inspecting = true;
        status_.offer.reset();
        status_.message = "Looking at " + link + "...";
        status_.message_error = false;
        ++status_.message_serial;
    }
    post([this, link = std::move(link), expect = std::move(expect)] {
        std::string error;
        AddonOffer offer;
        offer.link = link;
        offer.suggested = expect.has_value();
        auto source = parse_addon_link(link, &error);
        std::optional<std::pair<AddonManifest, std::string>> got;
        if (source) {
            offer.source = addon_source_text(*source);
            offer.page = addon_repo_page(*source);
            got = fetch_manifest(offer.source, &error);
        }
        if (!got) {
            std::lock_guard lock(mutex_);
            status_.inspecting = false;
        }
        if (!got) return say(error, true);
        offer.manifest = std::move(got->first);
        offer.manifest_url = std::move(got->second);
        const AddonManifest& m = offer.manifest;
        offer.blocked = manifest_problem(m, &offer.caps);
        // A suggestion is for the add-on the index names, signed with its key: the repository's owner cannot swap
        // in another under the index's word.
        if (expect && !iequal(m.name, expect->name))
            offer.blocked = "this is " + m.name + ", not the " + expect->name + " Hailstone's add-on index names";
        else if (expect && m.key != expect->key)
            offer.blocked = m.name + " is not signed with the key Hailstone's add-on index names for it";
        const auto installed = read_installed_addons(env_.data_dir);
        if (const InstalledAddon* a = find_addon(installed, m.name)) {
            if (!same_repository(a->source, offer.source))
                offer.blocked = "an add-on named " + a->name + " is installed already, from " + a->source;
            else if (a->kind != m.kind)
                offer.blocked = a->name + " is installed as a " + std::string(addon_kind_word(a->kind)) +
                                "; remove it first";
            else if (a->key && m.key != a->key)
                offer.blocked = a->name + " is signed with another key than the one installed: remove it first to "
                                          "trust the new key";
            offer.installed_version = a->version;
            // A link may name an older release (a tag) than the one installed: the card says so, and only a second,
            // explicit yes installs it (install_offer(true)).
            if (auto v = parse_version(a->version)) offer.older = compare_versions(m.version, *v) < 0;
        } else if (offer.blocked.empty()) {
            offer.blocked = addon_name_taken(env_.data_dir, installed, m.kind, m.name);
        }
        std::lock_guard lock(mutex_);
        status_.inspecting = false;
        status_.offer = std::move(offer);
    });
}

void AddonManager::refresh_index(bool now) {
    if (env_.index_url.empty() || !env_.index_key) return;
    post([this, now] {
        const fs::path cache = addons_dir(env_.data_dir) / "index.txt";
        const fs::path stamp = addons_dir(env_.data_dir) / "index-checked.txt";
        std::string why;
        std::optional<std::uint64_t> kept_serial;
        if (auto text = read_text_file(cache)) {
            // Checked again: the file is the player's to change.
            if (auto index = parse_addon_index(*text, *env_.index_key, &why)) {
                kept_serial = index->serial;
                std::lock_guard lock(mutex_);
                status_.index = std::move(*index);
            } else {
                log::warn("Add-ons: the cached add-on index is not used: {}", why);
            }
        }
        const std::int64_t t = env_.now();
        if (!now && env_.settings.check_hours > 0) {
            if (auto text = read_text_file(stamp)) {
                std::int64_t last = 0;
                std::from_chars(text->data(), text->data() + text->size(), last);
                if (t >= last && t - last < static_cast<std::int64_t>(env_.settings.check_hours) * 3600) return;
            }
        }
        auto text = fetch_text(env_.index_url, &why);
        if (!text) {
            log::info("Add-ons: the add-on index cannot be fetched ({})", why);
            return;
        }
        auto index = parse_addon_index(*text, *env_.index_key, &why);
        if (!index) {
            log::warn("Add-ons: the add-on index is refused: {}", why);
            return;
        }
        // Signed is not enough: an older signed index, served again, would bring back what a later one took out.
        if (std::string older = addon_index_older(kept_serial, index->serial); !older.empty()) {
            log::warn("Add-ons: the fetched add-on index is refused, the one kept stays: {}", older);
            return;
        }
        std::error_code ec;
        fs::create_directories(cache.parent_path(), ec);
        write_file_atomic(cache, *text);
        write_file_atomic(stamp, std::to_string(t) + "\n");
        log::info("Add-ons: the add-on index lists {} add-ons", index->addons.size());
        std::lock_guard lock(mutex_);
        status_.index = std::move(*index);
    });
}

void AddonManager::browse(bool again) {
    {
        std::lock_guard lock(mutex_);
        if (status_.browsing || (status_.browsed && !again)) return;
        status_.browsing = true;
    }
    post([this] {
        std::optional<AddonIndex> index;
        {
            std::lock_guard lock(mutex_);
            index = status_.index;
        }
        std::vector<AddonCatalogEntry> catalog;
        std::string error;
        if (!index) {
            error = "The list of add-ons could not be fetched. Check your connection and try again.";
            index.emplace();
        }
        for (const AddonIndexAddon& a : index->addons) {
            if (cancel_) return;
            AddonCatalogEntry e;
            e.addon = a;
            for (const auto& w : index->worlds)
                if (w.addon == a.name && std::find(e.worlds.begin(), e.worlds.end(), w.world) == e.worlds.end())
                    e.worlds.push_back(w.world);
            std::string why;
            auto got = fetch_manifest(a.source, &why);
            if (!got) {
                e.problem = why;
            } else if (!iequal(got->first.name, a.name)) {
                e.problem = a.source + " holds " + got->first.name + ", not the " + a.name + " the index names";
            } else if (got->first.key != a.key) {
                e.problem = a.name + " is not signed with the key the index names for it";
            } else {
                e.problem = manifest_problem(got->first, nullptr);
                e.manifest = std::move(got->first);
            }
            catalog.push_back(std::move(e));
        }
        log::info("Add-ons: the store lists {} add-ons", catalog.size());
        std::lock_guard lock(mutex_);
        status_.catalog = std::move(catalog);
        status_.catalog_error = std::move(error);
        status_.browsing = false;
        status_.browsed = true;
    });
}

void AddonManager::cancel_offer() {
    std::lock_guard lock(mutex_);
    status_.offer.reset();
}

std::optional<AddonManager::Staged> AddonManager::download(const AddonManifest& m, const std::string& manifest_url,
                                                           const std::string& source, std::uint32_t caps,
                                                           bool auto_update, std::string* error) {
    const AddonPackage* p = addon_package_for(m, m.kind == AddonKind::Script || m.kind == AddonKind::Ui ? "any" : env_.platform);
    if (!p) {
        if (error) *error = "no package for " + env_.platform;
        return std::nullopt;
    }
    const fs::path dir = addons_dir(env_.data_dir) / "download";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / (to_hex(p->sha256) + ".zip");
    if (!package_file_matches(file, p->size, p->sha256)) {
        const fs::path part = dir / (to_hex(p->sha256) + ".part");
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write " + part.string();
            return std::nullopt;
        }
        std::uint64_t received = 0;
        bool too_large = false;
        log::info("Add-ons: downloading {} {} ({} bytes)", m.name, m.version_text, p->size);
        auto got = env_.fetch(addon_url_beside(manifest_url, p->file), addon_download_allowed,
                              [&](std::span<const std::byte> piece) {
                                  received += piece.size();
                                  if (received > p->size) {
                                      too_large = true;
                                      return false;
                                  }
                                  out.write(reinterpret_cast<const char*>(piece.data()),
                                            static_cast<std::streamsize>(piece.size()));
                                  return static_cast<bool>(out) && !cancel_.load();
                              },
                              [this] { return !cancel_.load(); });
        out.close();
        if (!got.ok || !package_file_matches(part, p->size, p->sha256)) {
            fs::remove(part, ec);
            if (error)
                *error = got.ok || too_large ? "the package does not match its manifest (size or SHA-256)" : got.error;
            return std::nullopt;
        }
        fs::rename(part, file, ec);
        if (ec) {
            if (error) *error = "cannot keep " + file.string() + ": " + ec.message();
            return std::nullopt;
        }
    }
    auto files = unpack_addon(env_.data_dir, m, file, env_.platform, error);
    if (!files) return std::nullopt;
    InstalledAddon a;
    a.name = m.name;
    a.kind = m.kind;
    a.source = source;
    a.version = m.version_text;
    a.caps = caps;
    a.key = m.key;
    a.auto_update = auto_update;
    a.autoload = m.autoload;
    a.enabled = m.autoload;
    a.author = m.author;
    a.description = m.description;
    a.notes = m.notes;
    a.files = std::move(*files);
    return Staged{std::move(a), file};
}

void AddonManager::install_offer(bool older_ok) {
    std::optional<AddonOffer> offer;
    {
        std::lock_guard lock(mutex_);
        // An older version than the one installed only on the player's word that it is the older one they want: the
        // card stays up until they give it or cancel.
        if (status_.offer && status_.offer->older && !older_ok) {
            const std::string why = status_.offer->manifest.name + " " + status_.offer->manifest.version_text +
                                    " is older than the one installed (" + status_.offer->installed_version +
                                    "): confirm that you want to go back to it";
            status_.message = why;
            status_.message_error = true;
            ++status_.message_serial;
            return;
        }
        offer = std::move(status_.offer);
        status_.offer.reset();
    }
    if (!offer || !offer->blocked.empty()) return;
    const AddonManifest& m = offer->manifest;
    // Its row at once, as it downloads.
    AddonStatus s;
    bool busy = false;
    {
        std::lock_guard lock(mutex_);
        if (const AddonStatus* r = row(m.name)) {
            // One downloading, or staged for install_ready (which moves it out of addons/new/<name>/ while a new
            // download would unpack there): this one waits until that is done, as check_one does.
            busy = r->state == AddonState::Ready || r->state == AddonState::Downloading;
            s = *r;
        }
        if (!busy) removed_.erase(lower(m.name)); // the player installs it again
    }
    if (busy)
        return say(m.name + " is already being installed: install it again once that is done", true);
    const bool update = s.installed && !s.addon.name.empty();
    if (!update) {
        s.installed = false;
        s.addon.name = m.name;
        s.addon.kind = m.kind;
        s.addon.source = offer->source;
        s.addon.version = m.version_text;
        s.addon.caps = offer->caps;
        s.addon.author = m.author;
        s.addon.description = m.description;
    }
    s.state = AddonState::Downloading;
    s.new_version = m.version_text;
    s.error.clear();
    set_row(s);
    post([this, offer = std::move(*offer), s, update]() mutable {
        std::string error;
        auto a = download(offer.manifest, offer.manifest_url, offer.source, offer.caps,
                          update && s.addon.auto_update, &error);
        if (!a) {
            s.state = AddonState::Failed;
            s.error = error;
            set_row(s);
            return say(offer.manifest.name + " was not installed: " + error, true);
        }
        s.state = AddonState::Ready;
        set_row(s);
        std::lock_guard lock(mutex_);
        std::erase_if(staged_, [&](const Staged& st) { return iequal(st.addon.name, a->addon.name); });
        staged_.push_back(std::move(*a));
    });
}

void AddonManager::check_updates(bool now) {
    if (!now && !env_.settings.check) return;
    if (now) say("Looking for newer versions...", false);
    post([this, now] {
        const fs::path stamp = addons_dir(env_.data_dir) / "checked.txt";
        const std::int64_t t = env_.now();
        if (!now && env_.settings.check_hours > 0) {
            if (auto text = read_text_file(stamp)) {
                std::int64_t last = 0;
                std::from_chars(text->data(), text->data() + text->size(), last);
                if (t >= last && t - last < static_cast<std::int64_t>(env_.settings.check_hours) * 3600) {
                    log::info("Add-ons: checked less than {} hours ago (addons.check_hours)", env_.settings.check_hours);
                    return;
                }
            }
        }
        const auto installed = read_installed_addons(env_.data_dir);
        if (installed.empty()) {
            if (now) say("No add-ons are installed", false);
            return;
        }
        write_file_atomic(stamp, std::to_string(t) + "\n");
        int found = 0;
        for (const auto& a : installed)
            if (now || a.auto_update) found += check_one(a, std::nullopt);
        // (the check at start says nothing: each add-on's row does)
        if (now)
            say(found == 0 ? "Every add-on is up to date"
                           : found == 1 ? "A newer version of 1 add-on was found"
                                        : "Newer versions of " + std::to_string(found) + " add-ons were found",
                false);
    });
}

void AddonManager::update(std::string name, std::uint32_t approved_caps, std::string approved_version) {
    post([this, name = std::move(name), approval = Approval{approved_caps, std::move(approved_version)}] {
        const auto installed = read_installed_addons(env_.data_dir);
        if (const InstalledAddon* a = find_addon(installed, name)) (void)check_one(*a, approval);
    });
}

// The newer version of an installed add-on: checked, and downloaded when nothing stands in its way (`approved`: the
// player asked, which approves the capabilities they saw and overwrites their changes). A manifest that now asks for
// more than they saw, or names another version, is not taken on that approval: its row shows what it is now.
int AddonManager::check_one(const InstalledAddon& listed, const std::optional<Approval>& approved) {
    bool force = approved.has_value();
    AddonStatus s;
    {
        // The row turns Checking under the same lock remove() looks at it with: from here on a removal waits, and an
        // add-on removed since `listed` was read is left alone.
        std::lock_guard lock(mutex_);
        if (removed_.contains(lower(listed.name))) return 0;
        if (const AddonStatus* r = row(listed.name)) {
            if (r->state == AddonState::Ready || r->state == AddonState::Downloading) return 1;
            s = *r;
        }
        s.addon = listed;
        s.installed = true;
        s.state = AddonState::Checking;
        s.error.clear();
        set_row_locked(s);
    }
    // installed.txt as it is now (the list the caller read may be older: auto-update switched since, say).
    const auto now_installed = read_installed_addons(env_.data_dir);
    const InstalledAddon* current = find_addon(now_installed, listed.name);
    if (!current) {
        std::lock_guard lock(mutex_);
        std::erase_if(status_.addons, [&](const AddonStatus& r) { return iequal(r.addon.name, listed.name); });
        return 0;
    }
    const InstalledAddon a = *current;
    s.addon = a;
    std::string error;
    auto got = fetch_manifest(a.source, &error);
    if (!got) {
        s.state = AddonState::Failed;
        s.error = error;
        set_row(s);
        return 0;
    }
    const AddonManifest& m = got->first;
    s.modified = modified_addon_files(env_.data_dir, a);
    if (!iequal(m.name, a.name) || m.kind != a.kind) {
        s.state = AddonState::Blocked;
        s.error = a.source + " now holds another add-on (" + m.name + ")";
        set_row(s);
        return 0;
    }
    if (!newer(m.version, a.version)) {
        s.state = AddonState::UpToDate;
        s.new_version.clear();
        set_row(s);
        return 0;
    }
    s.new_version = m.version_text;
    std::uint32_t caps = 0;
    if (std::string problem = manifest_problem(m, &caps); !problem.empty()) {
        s.state = AddonState::Blocked;
        s.error = problem;
        set_row(s);
        return 1;
    }
    if (a.key && m.key != a.key) {
        s.state = AddonState::Blocked;
        s.error = m.key ? "signed with another key than the one installed: not updated (remove it and install it "
                          "again to trust the new key)"
                        : "no longer signed: not updated";
        set_row(s);
        return 1;
    }
    s.new_caps = caps;
    if (force && ((caps & ~approved->caps) != 0 || (!approved->version.empty() && approved->version != m.version_text))) {
        force = false;
        say(a.name + " changed since you looked at it (" + m.version_text +
                (caps & ~approved->caps ? ", asking for more than you approved" : "") +
                "): review it again before you update it",
            true);
    }
    if (!force) {
        if (caps & ~a.caps) {
            s.state = AddonState::NeedsApproval;
            set_row(s);
            return 1;
        }
        if (!s.modified.empty()) {
            s.state = AddonState::Modified;
            set_row(s);
            return 1;
        }
        // (read again: the player may have switched it off while the manifest was fetched)
        if (!auto_update_on(env_.data_dir, a.name)) {
            s.state = AddonState::Available;
            set_row(s);
            return 1;
        }
    }
    s.state = AddonState::Downloading;
    set_row(s);
    // The capabilities the player has now seen and accepted: the newer version's (an update that asks for less
    // lowers the ceiling with it).
    auto next = download(m, got->second, a.source, caps, a.auto_update, &error);
    if (!next) {
        s.state = AddonState::Failed;
        s.error = error;
        set_row(s);
        say(a.name + " was not updated: " + error, true);
        return 1;
    }
    next->automatic = !force;
    std::lock_guard lock(mutex_);
    s.state = AddonState::Ready;
    set_row_locked(s);
    std::erase_if(staged_, [&](const Staged& st) { return iequal(st.addon.name, next->addon.name); });
    staged_.push_back(std::move(*next));
    return 1;
}

void AddonManager::set_auto_update(std::string_view name, bool on) {
    auto all = read_installed_addons(env_.data_dir);
    for (auto& a : all)
        if (iequal(a.name, name)) a.auto_update = on;
    if (!write_installed_addons(env_.data_dir, all)) return;
    std::lock_guard lock(mutex_);
    if (AddonStatus* r = row(name)) r->addon.auto_update = on;
}

bool AddonManager::set_enabled(std::string_view name, bool on) {
    if (!set_addon_enabled(env_.data_dir, name, on)) return false;
    std::lock_guard lock(mutex_);
    if (AddonStatus* r = row(name)) r->addon.enabled = on;
    ++status_.serial;
    return true;
}

bool AddonManager::remove(std::string_view name, std::string* error) {
    {
        std::lock_guard lock(mutex_);
        const AddonStatus* r = row(name);
        if (r && (r->state == AddonState::Downloading || r->state == AddonState::Checking)) {
            if (error) *error = r->addon.name + " is being downloaded: wait a moment";
            return false;
        }
        // Marked before the files go, under the lock check_one() turns a row Checking with: a check already under way
        // with an older list skips it.
        removed_.insert(lower(name));
    }
    if (!remove_addon(env_.data_dir, name, error)) {
        std::lock_guard lock(mutex_);
        removed_.erase(lower(name));
        return false;
    }
    std::lock_guard lock(mutex_);
    std::erase_if(staged_, [&](const Staged& st) { return iequal(st.addon.name, name); });
    std::erase_if(status_.addons, [&](const AddonStatus& r) { return iequal(r.addon.name, name); });
    ++status_.serial;
    status_.message = std::string(name) + " is removed";
    status_.message_error = false;
    ++status_.message_serial;
    return true;
}

bool AddonManager::has_ready() const {
    std::lock_guard lock(mutex_);
    return !staged_.empty();
}

std::vector<std::string> AddonManager::install_ready() {
    std::vector<Staged> staged;
    {
        std::lock_guard lock(mutex_);
        staged.swap(staged_);
    }
    std::vector<std::string> done;
    for (auto& st : staged) {
        {
            std::lock_guard lock(mutex_);
            if (removed_.contains(lower(st.addon.name))) continue; // removed after it was staged
        }
        if (st.automatic) {
            // Auto-update switched off after the download began: offered, not installed.
            const auto all = read_installed_addons(env_.data_dir);
            const InstalledAddon* cur = find_addon(all, st.addon.name);
            if (!cur) continue; // no longer installed
            if (!cur->auto_update) {
                std::lock_guard lock(mutex_);
                if (AddonStatus* r = row(st.addon.name)) r->state = AddonState::Available;
                continue;
            }
        }
        std::string error;
        {
            // The row shows the switch as installed.txt has it (apply_addon keeps the installed entry's).
            const auto all = read_installed_addons(env_.data_dir);
            if (const InstalledAddon* cur = find_addon(all, st.addon.name)) {
                st.addon.auto_update = cur->auto_update;
                // inspect()'s pins again, against installed.txt as it is now: one of that name installed from
                // elsewhere since the card was made (two first installs, the other applied first) is not replaced.
                if (!same_repository(cur->source, st.addon.source))
                    error = "an add-on named " + cur->name + " is installed already, from " + cur->source;
                else if (cur->key && st.addon.key != cur->key)
                    error = cur->name + " is signed with another key than the one installed: remove it first to "
                                        "trust the new key";
            }
        }
        const bool ok = error.empty() && apply_addon(env_.data_dir, st.addon, &error);
        std::lock_guard lock(mutex_);
        AddonStatus s;
        if (const AddonStatus* r = row(st.addon.name)) s = *r;
        if (ok) {
            s.addon = st.addon;
            const auto installed = read_installed_addons(env_.data_dir);
            if (const auto* current = find_addon(installed, st.addon.name)) s.addon = *current;
            s.installed = true;
            s.state = AddonState::UpToDate;
            s.new_version.clear();
            s.new_caps = 0;
            s.modified.clear();
            s.error.clear();
            done.push_back(st.addon.name);
            ++status_.serial;
            status_.message = st.addon.name + " " + st.addon.version + " is installed";
            status_.message_error = false;
            std::error_code ec;
            fs::remove(st.package, ec); // (kept until now: a cut-off run unpacks it again instead of downloading)
        } else {
            s.state = AddonState::Failed;
            s.error = error;
            status_.message = st.addon.name + " was not installed: " + error;
            status_.message_error = true;
        }
        ++status_.message_serial;
        if (AddonStatus* r = row(st.addon.name)) *r = s;
    }
    return done;
}

AddonsStatus AddonManager::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

std::uint32_t AddonManager::serial() const {
    std::lock_guard lock(mutex_);
    return status_.serial;
}

std::vector<InstalledAddon> AddonManager::installed() const { return read_installed_addons(env_.data_dir); }

// ---------------------------------------------------------------- the lists beside the add-ons

std::vector<ServerAddon> server_addons(const fs::path& profile_dir) {
    std::vector<ServerAddon> out;
    if (profile_dir.empty()) return out;
    const plugin::Rules rules = net::cached_server_rules(profile_dir);
    std::set<std::string> seen;
    const std::string lib = std::string(plugin_extension(update_platform()));
    for (const auto& f : rules.server_files) {
        const std::string_view path = f.path;
        const std::size_t slash = path.find('/');
        if (slash == std::string_view::npos) continue;
        const std::string_view rest = path.substr(slash + 1);
        const std::size_t next = rest.find('/');
        std::string name;
        if (f.kind == plugin::FileKind::Script) {
            if (next != std::string_view::npos) name = std::string(rest.substr(0, next));
            else if (lower(rest).ends_with(".lua")) name = std::string(rest.substr(0, rest.size() - 4));
            if (iequal(name, "config")) name.clear();
        } else {
            const std::string_view file = rest.substr(rest.rfind('/') == std::string_view::npos ? 0 : rest.rfind('/') + 1);
            if (!lib.empty() && lower(file).ends_with(lib)) name = plugin::plugin_name(rel(file));
        }
        const AddonKind kind = f.kind == plugin::FileKind::Script ? AddonKind::Script : AddonKind::Plugin;
        if (name.empty()) continue;
        if (seen.insert(std::string(addon_kind_word(kind)) + ":" + lower(name)).second)
            out.push_back({name, kind,
                           kind == AddonKind::Plugin ? plugin::plugin_consent(profile_dir, f.sha256) : plugin::Consent::Unasked,
                           {}});
        if (kind == AddonKind::Plugin)
            for (auto& a : out)
                if (a.kind == kind && iequal(a.name, name)) a.hashes.push_back(f.sha256);
    }
    std::sort(out.begin(), out.end(), [](const ServerAddon& a, const ServerAddon& b) { return lower(a.name) < lower(b.name); });
    return out;
}

std::vector<ManualAddon> manual_addons(const std::vector<fs::path>& dirs, const std::vector<InstalledAddon>& installed) {
    std::vector<ManualAddon> out;
    std::set<std::string> seen;
    auto add = [&](std::string name, AddonKind kind, const fs::path& path) {
        if (name.empty()) return;
        for (const auto& a : installed)
            if (a.kind == kind && iequal(a.name, name)) return;
        if (seen.insert(std::string(addon_kind_word(kind)) + ":" + lower(name)).second) out.push_back({name, kind, path});
    };
    const std::string lib = std::string(plugin_extension(update_platform()));
    auto is_lib = [&](const fs::path& p) { return !lib.empty() && lower(p.extension().string()) == lib; };
    std::error_code ec;
    for (const auto& d : dirs) {
        std::vector<fs::directory_entry> entries;
        for (const auto& e : fs::directory_iterator(d / "scripts", ec)) entries.push_back(e);
        std::sort(entries.begin(), entries.end());
        for (const auto& e : entries) {
            const std::string file = e.path().filename().string();
            if (e.is_regular_file(ec) && lower(e.path().extension().string()) == ".lua")
                add(e.path().stem().string(), AddonKind::Script, e.path());
            else if (e.is_directory(ec) && !iequal(file, "config") && fs::is_regular_file(e.path() / "init.lua", ec))
                add(file, AddonKind::Script, e.path());
        }
        entries.clear();
        for (const auto& e : fs::directory_iterator(d / "plugins", ec)) entries.push_back(e);
        std::sort(entries.begin(), entries.end());
        for (const auto& e : entries) {
            if (e.is_regular_file(ec) && is_lib(e.path())) {
                add(plugin::plugin_name(e.path()), AddonKind::Plugin, e.path());
            } else if (e.is_directory(ec)) {
                for (const auto& f : fs::directory_iterator(e.path(), ec))
                    if (f.is_regular_file(ec) && is_lib(f.path())) add(plugin::plugin_name(f.path()), AddonKind::Plugin, e.path());
            }
        }
        // The upscaler providers put in upscalers/ by hand or by a build (render/upscaler_provider.h): a folder with
        // a library of its name.
        entries.clear();
        for (const auto& e : fs::directory_iterator(d / "upscalers", ec)) entries.push_back(e);
        std::sort(entries.begin(), entries.end());
        for (const auto& e : entries) {
            const std::string name = e.path().filename().string();
            if (e.is_directory(ec) && !lib.empty() && fs::is_regular_file(e.path() / (name + lib), ec))
                add(name, AddonKind::Upscaler, e.path());
        }
    }
    std::sort(out.begin(), out.end(), [](const ManualAddon& a, const ManualAddon& b) { return lower(a.name) < lower(b.name); });
    return out;
}

} // namespace hailstone::app
