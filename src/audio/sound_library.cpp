#include "audio/sound_library.h"

#include <algorithm>
#include <cctype>
#include <charconv>

#include "assets/pfs.h"
#include "core/log.h"
#include "platform/crash_log.h"

namespace hailstone::audio {

namespace {
constexpr std::size_t kOpenArchives = 2;
}

std::string lower_case(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

SoundLibrary::SoundLibrary(const GameDir& game) : game_(game) {
    // snd<N>.pfs in the game directory, by number, through every layer (a server's assets folder may replace one or
    // add its own).
    std::vector<std::pair<int, std::filesystem::path>> found;
    for (const GameDir::Entry& entry : game_.list("")) {
        std::string name = lower_case(entry.name);
        if (name.size() > 7 && name.starts_with("snd") && name.ends_with(".pfs")) {
            std::string digits = name.substr(3, name.size() - 7);
            // A number past int (snd99999999999.pfs) is not an archive of the set; std::stoi threw out of here.
            int n = 0;
            const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
            const bool all_digits = std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
            if (!digits.empty() && all_digits && ec == std::errc{} && end == digits.data() + digits.size())
                found.emplace_back(n, entry.path);
        }
    }
    std::sort(found.begin(), found.end());
    for (auto& [n, p] : found) archive_paths_.push_back(p);
    thread_ = std::thread([this] { worker(); });
}

SoundLibrary::~SoundLibrary() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void SoundLibrary::build_index() {
    std::unordered_map<std::string, int> index;
    std::size_t files = 0;
    for (std::size_t i = 0; i < archive_paths_.size(); ++i) {
        {
            std::lock_guard lock(mutex_);
            if (stop_) return;
        }
        try {
            auto a = assets::PfsArchive::open(archive_paths_[i],
                assets::PfsArchive::storage_for(game_, archive_paths_[i]), game_.shared_file_cache());
            for (const auto& n : a.names()) {
                index[lower_case(n)] = static_cast<int>(i); // higher numbers override
                ++files;
            }
        } catch (const std::exception& e) {
            log::warn("audio: {}: {}", archive_paths_[i].string(), e.what());
        }
    }
    std::lock_guard lock(mutex_);
    archive_index_ = std::move(index);
    index_ready_ = true;
    log::debug("audio: {} sound archives, {} files", archive_paths_.size(), files);
    cv_.notify_all();
}

std::size_t SoundLibrary::archive_files() const {
    std::lock_guard lock(mutex_);
    return archive_index_.size();
}

std::optional<SoundLibrary::Location> SoundLibrary::locate(const std::string& lower) {
    // Only plain names: no directories, drives or traversal.
    if (lower.empty() || lower.find_first_of("/\\:") != std::string::npos) return std::nullopt;
    if (auto p = game_.find("sounds/" + lower)) return Location{-1, *p};
    if (auto p = game_.find("voice/default/" + lower)) return Location{-1, *p};
    if (auto p = game_.find(lower)) return Location{-1, *p};
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return index_ready_ || stop_; });
    if (auto it = archive_index_.find(lower); it != archive_index_.end()) return Location{it->second, {}};
    return std::nullopt;
}

bool SoundLibrary::exists(const std::string& name) { return locate(lower_case(name)).has_value(); }

bool SoundLibrary::index_ready() const {
    std::lock_guard lock(mutex_);
    return index_ready_ || stop_;
}

std::shared_ptr<const assets::PfsArchive> SoundLibrary::archive(int index) {
    std::lock_guard lock(archive_mutex_);
    for (auto it = open_.begin(); it != open_.end(); ++it)
        if (it->first == index) {
            auto a = it->second;
            open_.erase(it);
            open_.emplace_back(index, a);
            return a;
        }
    auto a = std::make_shared<const assets::PfsArchive>(
        assets::PfsArchive::open(archive_paths_[static_cast<std::size_t>(index)],
            assets::PfsArchive::storage_for(game_, archive_paths_[static_cast<std::size_t>(index)]),
            game_.shared_file_cache()));
    open_.emplace_back(index, a);
    if (open_.size() > kOpenArchives) open_.erase(open_.begin());
    return a;
}

std::optional<std::vector<std::byte>> SoundLibrary::read(const std::string& name) {
    auto loc = locate(lower_case(name));
    if (!loc) return std::nullopt;
    try {
        if (loc->archive < 0) return read_file(loc->path);
        return archive(loc->archive)->read(lower_case(name));
    } catch (const std::exception& e) {
        log::warn("audio: cannot read {}: {}", name, e.what());
        return std::nullopt;
    }
}

std::shared_ptr<const Pcm> SoundLibrary::wave(const std::string& name) {
    std::string key = lower_case(name);
    std::lock_guard lock(mutex_);
    if (auto it = cache_.find(key); it != cache_.end()) {
        it->second.used = generation_;
        return it->second.pcm;
    }
    if (failed_.contains(key)) return nullptr;
    if (queued_.contains(key)) {
        // Wanted now: one still waiting behind the preloads moves to the front.
        if (auto it = std::find(preload_queue_.begin(), preload_queue_.end(), key); it != preload_queue_.end()) {
            preload_queue_.erase(it);
            queue_.push_back(key);
        }
        return nullptr;
    }
    queued_.insert(key);
    queue_.push_back(key);
    cv_.notify_all();
    return nullptr;
}

void SoundLibrary::preload(const std::vector<std::string>& names) {
    std::lock_guard lock(mutex_);
    for (const auto& n : names) {
        std::string key = lower_case(n);
        if (auto it = cache_.find(key); it != cache_.end()) {
            it->second.used = generation_;
            continue;
        }
        if (failed_.contains(key) || queued_.contains(key)) continue;
        queued_.insert(key);
        preload_queue_.push_back(std::move(key));
    }
    cv_.notify_all();
}

void SoundLibrary::drop_idle_waves(const std::vector<std::string>& pinned) {
    std::unordered_set<std::string> keep;
    for (const auto& n : pinned) keep.insert(lower_case(n));
    std::vector<std::shared_ptr<const Pcm>> dropped; // freed after the lock is released
    std::lock_guard lock(mutex_);
    ++generation_;
    for (auto it = cache_.begin(); it != cache_.end();) {
        const bool idle = static_cast<std::uint32_t>(generation_ - it->second.used) >= 2;
        if (idle && it->second.pcm.use_count() == 1 && !keep.contains(it->first)) {
            dropped.push_back(std::move(it->second.pcm));
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

std::size_t SoundLibrary::cached_waves() const {
    std::lock_guard lock(mutex_);
    return cache_.size();
}

void SoundLibrary::set_resident_files(const std::vector<std::string>& names) {
    std::lock_guard lock(mutex_);
    std::unordered_set<std::string> keep;
    for (const auto& n : names)
        if (!n.empty()) keep.insert(lower_case(n));
    std::erase_if(resident_, [&](const auto& kv) { return !keep.contains(kv.first); });
    std::erase_if(file_queue_, [&](const std::string& n) { return !keep.contains(n); });
    for (const auto& n : keep)
        if (!resident_.contains(n) && !missing_files_.contains(n) && n != file_in_flight_ &&
            std::find(file_queue_.begin(), file_queue_.end(), n) == file_queue_.end())
            file_queue_.push_back(n);
    resident_names_ = std::move(keep);
    cv_.notify_all();
}

std::shared_ptr<const std::vector<std::byte>> SoundLibrary::resident_file(const std::string& name) const {
    std::lock_guard lock(mutex_);
    auto it = resident_.find(lower_case(name));
    return it == resident_.end() ? nullptr : it->second;
}

std::shared_ptr<const std::vector<std::byte>> SoundLibrary::file(const std::string& name) {
    std::string key = lower_case(name);
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return stop_ || file_in_flight_ != key; });
        if (auto it = resident_.find(key); it != resident_.end()) return it->second;
        // Failed before: a music emitter in range asks again every frame, which re-read (and logged) it each time.
        if (missing_files_.contains(key)) return nullptr;
        // Still queued: read here, so it is not read twice.
        if (auto it = std::find(file_queue_.begin(), file_queue_.end(), key); it != file_queue_.end())
            file_queue_.erase(it);
    }
    auto bytes = read(key);
    std::lock_guard lock(mutex_);
    if (!bytes) {
        missing_files_.insert(key);
        return nullptr;
    }
    auto shared = std::make_shared<const std::vector<std::byte>>(std::move(*bytes));
    if (resident_names_.contains(key)) resident_[key] = shared;
    return shared;
}

void SoundLibrary::wait_idle() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] {
        return stop_ || (index_ready_ && queue_.empty() && file_queue_.empty() && preload_queue_.empty() && !busy_);
    });
}

void SoundLibrary::hold_worker(bool held) {
    std::lock_guard lock(mutex_);
    held_ = held;
    cv_.notify_all();
}

void SoundLibrary::set_job_log(std::vector<std::string>* log) {
    std::lock_guard lock(mutex_);
    job_log_ = log;
}

void SoundLibrary::worker() {
    platform::CrashStack crash_stack;
    build_index();
    for (;;) {
        std::string key;
        bool file = false;
        {
            std::unique_lock lock(mutex_);
            busy_ = false;
            cv_.notify_all();
            cv_.wait(lock, [&] {
                return stop_ || (!held_ && (!queue_.empty() || !file_queue_.empty() || !preload_queue_.empty()));
            });
            if (stop_) return;
            // A wave wanted now first, then a music file (one may start on zone entry), then the preloads.
            file = queue_.empty() && !file_queue_.empty();
            std::deque<std::string>& from = file ? file_queue_ : !queue_.empty() ? queue_ : preload_queue_;
            key = std::move(from.front());
            from.pop_front();
            if (file) file_in_flight_ = key;
            if (job_log_) job_log_->push_back(key);
            busy_ = true;
        }
        if (file) {
            std::optional<std::vector<std::byte>> bytes = read(key);
            std::lock_guard lock(mutex_);
            file_in_flight_.clear();
            cv_.notify_all();
            if (!resident_names_.contains(key)) continue; // dropped meanwhile (set_resident_files)
            if (bytes) resident_[key] = std::make_shared<const std::vector<std::byte>>(std::move(*bytes));
            else missing_files_.insert(key);
            continue;
        }
        std::shared_ptr<const Pcm> pcm;
        if (auto bytes = read(key)) {
            if (auto decoded = decode_sound(*bytes)) pcm = std::make_shared<const Pcm>(std::move(*decoded));
        }
        std::lock_guard lock(mutex_);
        queued_.erase(key);
        if (pcm) {
            cache_[key] = Cached{std::move(pcm), generation_};
        } else {
            failed_.insert(key);
            log::debug("audio: no sound {}", key);
        }
    }
}

} // namespace hailstone::audio
