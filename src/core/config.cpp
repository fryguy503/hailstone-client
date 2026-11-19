#include "core/config.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>

#include "core/atomic_file.h"
#include "platform/paths.h"

namespace hailstone {

namespace {
std::string_view trim(std::string_view s) {
    const char* ws = " \t\r\n";
    std::size_t b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    std::size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}
} // namespace

bool Config::load(const std::filesystem::path& path) {
    entries_.clear();
    changed_.clear();
    disk_path_ = path;
    disk_text_ = read_text_file(path);
    if (!disk_text_) return false;
    parse(*disk_text_);
    changed_.clear();
    return true;
}

void Config::parse(std::string_view text) {
    entries_.clear();
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::string_view l = trim(text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
        pos = nl == std::string_view::npos ? text.size() : nl + 1;
        if (l.empty() || l.front() == '#' || l.front() == ';' || l.front() == '[') continue;
        std::size_t eq = l.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view key = trim(l.substr(0, eq));
        if (!key.empty()) store(key, trim(l.substr(eq + 1)));
    }
}

std::string Config::to_string() const {
    std::string out = "# Hailstone settings\n";
    for (const auto& [k, v] : entries_) out += k + '=' + v + '\n';
    return out;
}

bool Config::save(const std::filesystem::path& path) {
    bool layer_ok = true;
    if (layer_ && !layer_->changed_.empty()) layer_ok = layer_->save(layer_path_);
    // With a character's layer, hailstone.ini is written only when one of its own keys changed.
    if (layer_ && changed_.empty() && path == disk_path_ && disk_text_) return layer_ok;
    if (path == disk_path_) {
        auto now = read_text_file(path);
        if (now && now != disk_text_) {
            // Another program wrote the file since: its keys, with this config's changes over them.
            Config merged;
            merged.parse(*now);
            for (const std::string& key : changed_) {
                if (auto v = stored(key)) merged.set(key, *v);
                else merged.erase(key);
            }
            entries_ = std::move(merged.entries_);
        }
    }
    std::string text = to_string();
    if (!write_file_atomic(path, text)) return false;
    disk_path_ = path;
    disk_text_ = std::move(text);
    changed_.clear();
    return layer_ok;
}

std::vector<std::pair<std::string, std::string>> Config::entries() const {
    if (!layer_) return entries_;
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& e : entries_)
        if (is_shared_key(e.first)) out.push_back(e);
    for (const auto& e : layer_->entries_)
        if (!is_shared_key(e.first)) out.push_back(e);
    return out;
}

bool Config::attach_character(const std::filesystem::path& path) {
    detach_character();
    if (path.empty()) return true;
    auto layer = std::make_unique<Config>();
    bool ok = true;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        // One that cannot be read is left as it is: the character plays with what hailstone.ini says, and keeps it
        // in memory only until the file can be written again.
        if (!layer->load(path)) {
            ok = false;
            for (const auto& [k, v] : entries_)
                if (!is_shared_key(k)) layer->set(k, v);
        } else {
            // A key shared since the file was written (display.upscaler, once saved per character) comes
            // into hailstone.ini when it has none of its own, so the choice made then is not lost.
            for (const auto& [k, v] : layer->entries_)
                if (is_shared_key(k) && !stored(k)) set(k, v);
        }
    } else {
        // The character's first entry: it starts with the shared settings as they are now.
        for (const auto& [k, v] : entries_)
            if (!is_shared_key(k)) layer->set(k, v);
        ok = layer->save(path);
    }
    layer_ = std::move(layer);
    layer_path_ = path;
    return ok;
}

bool Config::is_shared_key(std::string_view key) {
    // The client's own (asset folders, add-ons, updates, news, the browser, plugins and scripts it loads), the login
    // screen's (servers, worlds, the music there), the window's place and mode (one window, whoever plays in it), the
    // computer's graphics tier (render/graphics_tier.h), and what is taken only at start (the sample count, the
    // upscaler, the UI pack and scale, the sound device, the navigation threads, the extra shields).
    static constexpr std::string_view kPrefixes[] = {"assets.", "addons.", "update.", "news.", "login.", "browser.",
                                                      "window.", "plugins.", "scripts."};
    static constexpr std::string_view kKeys[] = {
        "display.window_mode", "display.resolution", "display.msaa", "display.upscaler", "display.gpu_tier", "ui.name", "ui.scale", "ui.cpp_windows",
        "ui.dev_windows", "ui.perf_window", "ui.perf_log_hitches", "game.fixed_step", "audio.pregame_music",
        "audio.enabled", "audio.soundfont", "audio.mix_ahead", "nav.threads", "nav.prebuild", "display.extra_shields"};
    for (std::string_view p : kPrefixes)
        if (key.starts_with(p)) return true;
    return std::find(std::begin(kKeys), std::end(kKeys), key) != std::end(kKeys);
}

Config* Config::layer_for(std::string_view key) const {
    return layer_ && !is_shared_key(key) ? layer_.get() : nullptr;
}

std::optional<std::string> Config::get(std::string_view key) const {
    for (const auto& [k, v] : run_overrides_)
        if (k == key) return v;
    return stored(key);
}

std::optional<std::string> Config::stored(std::string_view key) const {
    if (Config* l = layer_for(key)) return l->stored(key);
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& e) { return e.first == key; });
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

std::string Config::get_string(std::string_view key, std::string_view fallback) const {
    auto v = get(key);
    return v ? *v : std::string(fallback);
}

int Config::get_int(std::string_view key, int fallback) const {
    auto v = get(key);
    if (!v) return fallback;
    int out = 0;
    auto [p, ec] = std::from_chars(v->data(), v->data() + v->size(), out);
    return (ec == std::errc() && p == v->data() + v->size()) ? out : fallback;
}

float Config::get_float(std::string_view key, float fallback) const {
    auto v = get(key);
    if (!v || v->empty()) return fallback;
    char* end = nullptr;
    const float f = std::strtof(v->c_str(), &end);
    return end != v->c_str() && std::isfinite(f) ? f : fallback;
}

bool Config::get_bool(std::string_view key, bool fallback) const {
    auto v = get(key);
    if (!v) return fallback;
    if (*v == "1" || *v == "true" || *v == "yes" || *v == "on") return true;
    if (*v == "0" || *v == "false" || *v == "no" || *v == "off") return false;
    return fallback;
}

void Config::set_run_override(std::string_view key, std::string_view value) {
    drop_run_override(key);
    run_overrides_.emplace_back(std::string(key), std::string(value));
}

void Config::drop_run_override(std::string_view key) {
    std::erase_if(run_overrides_, [&](const auto& e) { return e.first == key; });
}

void Config::set(std::string_view key, std::string_view value) {
    drop_run_override(key);
    store(key, value);
}

void Config::store(std::string_view key, std::string_view value) {
    if (Config* l = layer_for(key)) return l->store(key, value);
    // Newlines would break the file format.
    std::string v(value);
    std::replace(v.begin(), v.end(), '\n', ' ');
    std::replace(v.begin(), v.end(), '\r', ' ');
    auto changed = [&] {
        if (std::find(changed_.begin(), changed_.end(), key) == changed_.end()) changed_.emplace_back(key);
    };
    for (auto& e : entries_)
        if (e.first == key) {
            if (e.second != v) changed();
            e.second = std::move(v);
            return;
        }
    changed();
    entries_.emplace_back(std::string(key), std::move(v));
}

void Config::erase(std::string_view key) {
    drop_run_override(key);
    if (Config* l = layer_for(key)) return l->erase(key);
    if (std::erase_if(entries_, [&](const auto& e) { return e.first == key; }) > 0 &&
        std::find(changed_.begin(), changed_.end(), key) == changed_.end())
        changed_.emplace_back(key);
}

std::filesystem::path default_config_path() { return platform::config_file(); }

} // namespace hailstone
