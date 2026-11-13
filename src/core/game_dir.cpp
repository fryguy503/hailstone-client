#include "core/game_dir.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <stdexcept>

#include "core/ascii_case.h"

namespace hailstone {

namespace {
void require_directory(const std::filesystem::path& root) {
    if (!std::filesystem::is_directory(root))
        throw std::runtime_error("game directory not found: " + root.string());
}

// The folder part of a normalized relative path, with a trailing '/' ("" for the top).
std::string dir_prefix(std::string_view relative_dir) {
    std::string prefix = GameDir::normalize(relative_dir);
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    while (!prefix.empty() && prefix.front() == '/') prefix.erase(prefix.begin());
    if (!prefix.empty()) prefix.push_back('/');
    return prefix;
}
} // namespace

GameDir::GameDir() : index_(std::make_shared<const Index>()) {}

GameDir::GameDir(std::filesystem::path root) : GameDir(std::move(root), std::nullopt) {}

GameDir::GameDir(std::filesystem::path root, std::optional<std::filesystem::path> overlay)
    : base_(std::move(root)), overlay_(std::move(overlay)) {
    require_directory(base_);
    base_index_ = scan(base_, false);
    if (overlay_) overlay_index_ = scan(*overlay_, true);
    merge_locked();
}

GameDir::GameDir(const GameDir& other) {
    std::lock_guard lock(other.mutex_);
    shared_file_cache_ = other.shared_file_cache_;
    base_ = other.base_;
    overlay_ = other.overlay_;
    base_index_ = other.base_index_;
    overlay_index_ = other.overlay_index_;
    classic_ = other.classic_;
    classic_index_ = other.classic_index_;
    classic_prefer_ = other.classic_prefer_;
    overlay_keep_ = other.overlay_keep_;
    index_ = other.index_;
}

GameDir& GameDir::operator=(const GameDir& other) {
    if (this == &other) return *this;
    std::scoped_lock lock(mutex_, other.mutex_);
    shared_file_cache_ = other.shared_file_cache_;
    base_ = other.base_;
    overlay_ = other.overlay_;
    base_index_ = other.base_index_;
    overlay_index_ = other.overlay_index_;
    classic_ = other.classic_;
    classic_index_ = other.classic_index_;
    classic_prefer_ = other.classic_prefer_;
    overlay_keep_ = other.overlay_keep_;
    index_ = other.index_;
    return *this;
}

GameDir::GameDir(GameDir&& other) noexcept {
    std::lock_guard lock(other.mutex_);
    shared_file_cache_ = std::move(other.shared_file_cache_);
    base_ = std::move(other.base_);
    overlay_ = std::move(other.overlay_);
    base_index_ = std::move(other.base_index_);
    overlay_index_ = std::move(other.overlay_index_);
    classic_ = std::move(other.classic_);
    classic_index_ = std::move(other.classic_index_);
    classic_prefer_ = std::move(other.classic_prefer_);
    overlay_keep_ = std::move(other.overlay_keep_);
    index_ = std::move(other.index_);
    other.index_ = std::make_shared<const Index>();
}

GameDir& GameDir::operator=(GameDir&& other) noexcept {
    if (this == &other) return *this;
    std::scoped_lock lock(mutex_, other.mutex_);
    shared_file_cache_ = std::move(other.shared_file_cache_);
    base_ = std::move(other.base_);
    overlay_ = std::move(other.overlay_);
    base_index_ = std::move(other.base_index_);
    overlay_index_ = std::move(other.overlay_index_);
    classic_ = std::move(other.classic_);
    classic_index_ = std::move(other.classic_index_);
    classic_prefer_ = std::move(other.classic_prefer_);
    overlay_keep_ = std::move(other.overlay_keep_);
    index_ = std::move(other.index_);
    other.index_ = std::make_shared<const Index>();
    return *this;
}

void GameDir::set_shared_file_cache(std::shared_ptr<SharedFileCache> cache) {
    std::lock_guard lock(mutex_);
    shared_file_cache_ = std::move(cache);
}

std::shared_ptr<SharedFileCache> GameDir::shared_file_cache() const {
    std::lock_guard lock(mutex_);
    return shared_file_cache_;
}

std::shared_ptr<const GameDir::Index> GameDir::scan(const std::filesystem::path& root, bool overlay, bool classic) {
    auto index = std::make_shared<Index>();
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return index;
    auto opts = std::filesystem::directory_options::skip_permission_denied;
    for (auto it = std::filesystem::recursive_directory_iterator(root, opts, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code type_ec;
        // Hailstone's own folder inside a game folder (kOwnFolder) is not walked. An overlay is a folder of
        // Hailstone's already (servers/<id>/assets), so it is walked whole.
        if (!overlay && it.depth() == 0 && it->is_directory(type_ec)) {
            const std::u8string name = it->path().filename().u8string();
            if (iequals(std::string_view(reinterpret_cast<const char*>(name.data()), name.size()), kOwnFolder)) {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (!it->is_regular_file(type_ec)) continue;
        auto rel = std::filesystem::relative(it->path(), root, type_ec).generic_string();
        if (type_ec) continue;
        index->emplace(normalize(rel), Indexed{rel, it->path(), overlay, classic});
    }
    return index;
}

// Rebuilds index_ from the layer indexes; the caller holds mutex_ (or is the constructor).
void GameDir::merge_locked() {
    auto merged = std::make_shared<Index>();
    if (overlay_index_) {
        if (overlay_keep_) {
            for (const auto& [key, entry] : *overlay_index_)
                if (overlay_keep_(key)) merged->emplace(key, entry);
        } else {
            *merged = *overlay_index_;
        }
    }
    // emplace keeps what a higher layer put there first.
    if (classic_index_ && classic_prefer_)
        for (const auto& [key, entry] : *classic_index_)
            if (classic_prefer_(key)) merged->emplace(key, entry);
    if (base_index_)
        for (const auto& [key, entry] : *base_index_) merged->emplace(key, entry);
    if (classic_index_)
        for (const auto& [key, entry] : *classic_index_) merged->emplace(key, entry);
    index_ = std::move(merged);
}

std::shared_ptr<const GameDir::Index> GameDir::index() const {
    std::lock_guard lock(mutex_);
    return index_;
}

std::optional<std::filesystem::path> GameDir::overlay() const {
    std::lock_guard lock(mutex_);
    return overlay_;
}

std::optional<std::filesystem::path> GameDir::classic() const {
    std::lock_guard lock(mutex_);
    return classic_;
}

std::vector<std::filesystem::path> GameDir::roots() const {
    std::lock_guard lock(mutex_);
    std::vector<std::filesystem::path> out;
    if (overlay_) out.push_back(*overlay_);
    if (!base_.empty()) out.push_back(base_);
    if (classic_) out.push_back(*classic_);
    return out;
}

void GameDir::set_base(std::filesystem::path root) {
    require_directory(root);
    auto index = scan(root, false);
    std::lock_guard lock(mutex_);
    base_ = std::move(root);
    base_index_ = std::move(index);
    merge_locked();
}

void GameDir::set_overlay(std::optional<std::filesystem::path> overlay) {
    std::shared_ptr<const Index> index;
    if (overlay) index = scan(*overlay, true);
    std::lock_guard lock(mutex_);
    overlay_ = std::move(overlay);
    overlay_index_ = std::move(index);
    merge_locked();
}

void GameDir::set_classic(std::optional<std::filesystem::path> classic, PreferRule prefer) {
    std::shared_ptr<const Index> index;
    if (classic) index = scan(*classic, false, true);
    std::lock_guard lock(mutex_);
    classic_ = std::move(classic);
    classic_index_ = std::move(index);
    classic_prefer_ = std::move(prefer);
    merge_locked();
}

void GameDir::set_overlay_filter(PreferRule keep) {
    std::lock_guard lock(mutex_);
    overlay_keep_ = std::move(keep);
    merge_locked();
}

void GameDir::rescan() {
    std::filesystem::path base;
    std::optional<std::filesystem::path> overlay, classic;
    {
        std::lock_guard lock(mutex_);
        base = base_;
        overlay = overlay_;
        classic = classic_;
    }
    std::shared_ptr<const Index> base_index = base.empty() ? nullptr : scan(base, false);
    std::shared_ptr<const Index> overlay_index = overlay ? scan(*overlay, true) : nullptr;
    std::shared_ptr<const Index> classic_index = classic ? scan(*classic, false, true) : nullptr;
    std::lock_guard lock(mutex_);
    base_index_ = std::move(base_index);
    overlay_index_ = std::move(overlay_index);
    classic_index_ = std::move(classic_index);
    merge_locked();
}

std::string GameDir::normalize(std::string_view relative) {
    std::string out;
    out.reserve(relative.size());
    for (char c : relative) {
        if (c == '\\') c = '/';
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::optional<std::filesystem::path> GameDir::find(std::string_view relative) const {
    auto idx = index();
    auto it = idx->find(normalize(relative));
    if (it == idx->end()) return std::nullopt;
    return it->second.path;
}

std::optional<std::filesystem::path> GameDir::find_base(std::string_view relative) const {
    std::shared_ptr<const Index> idx;
    {
        std::lock_guard lock(mutex_);
        idx = base_index_;
    }
    if (!idx) return std::nullopt;
    auto it = idx->find(normalize(relative));
    if (it == idx->end()) return std::nullopt;
    return it->second.path;
}

bool GameDir::from_overlay(std::string_view relative) const {
    auto idx = index();
    auto it = idx->find(normalize(relative));
    return it != idx->end() && it->second.overlay;
}

bool GameDir::from_classic(std::string_view relative) const {
    auto idx = index();
    auto it = idx->find(normalize(relative));
    return it != idx->end() && it->second.classic;
}

std::optional<std::vector<std::byte>> GameDir::read(std::string_view relative) const {
    auto path = find(relative);
    if (!path) return std::nullopt;
    return read_file(*path);
}

std::vector<GameDir::Entry> GameDir::list(std::string_view relative_dir) const {
    const std::string prefix = dir_prefix(relative_dir);
    std::map<std::string, Entry> sorted;
    // Held here: a temporary in the loop's range would die before the walk (and a rebuild could free the map).
    const auto idx = index();
    for (const auto& [key, entry] : *idx) {
        if (!key.starts_with(prefix)) continue;
        if (key.find('/', prefix.size()) != std::string::npos) continue;
        sorted.emplace(key, Entry{entry.relative.substr(prefix.size()), entry.relative, entry.path});
    }
    std::vector<Entry> out;
    out.reserve(sorted.size());
    for (auto& [key, entry] : sorted) out.push_back(std::move(entry));
    return out;
}

std::vector<std::string> GameDir::list_dirs(std::string_view relative_dir) const {
    const std::string prefix = dir_prefix(relative_dir);
    // lower-cased name -> spelling; the overlay's spelling wins over the base's
    std::map<std::string, std::pair<bool, std::string>> sorted;
    // Held here: a temporary in the loop's range would die before the walk (and a rebuild could free the map).
    const auto idx = index();
    for (const auto& [key, entry] : *idx) {
        if (!key.starts_with(prefix)) continue;
        auto slash = key.find('/', prefix.size());
        if (slash == std::string::npos) continue;
        std::string lower = key.substr(prefix.size(), slash - prefix.size());
        std::string name = entry.relative.substr(prefix.size(), slash - prefix.size());
        auto [it, added] = sorted.emplace(lower, std::pair{entry.overlay, name});
        if (!added && entry.overlay && !it->second.first) it->second = {true, name};
    }
    std::vector<std::string> out;
    out.reserve(sorted.size());
    for (auto& [lower, named] : sorted) out.push_back(std::move(named.second));
    return out;
}

std::optional<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return std::nullopt;
    auto size = static_cast<std::size_t>(f.tellg());
    std::vector<std::byte> data(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!f) return std::nullopt;
    return data;
}

} // namespace hailstone
