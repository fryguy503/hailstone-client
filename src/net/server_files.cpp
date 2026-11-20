#include "net/server_files.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <system_error>

#include "core/log.h"
#include "core/windows_names.h"
#include "plugin/policy.h"

namespace hailstone::net {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Programs, libraries and scripts: refused everywhere but Lua in scripts/ and this platform's libraries in plugins/
// (server_file_destination).
constexpr std::array kRefused{".exe", ".dll", ".so",  ".dylib", ".com", ".bat", ".cmd", ".ps1", ".sh", ".asi",
                              ".flt", ".lua", ".py",  ".js",    ".vbs", ".scr", ".msi", ".jar", ".app"};
constexpr std::array kLibraries{".dll", ".so", ".dylib"};
constexpr std::array kPlatforms{"windows-x64", "windows-arm64", "linux-x64", "linux-arm64", "macos-x64", "macos-arm64"};

#if defined(_WIN32)
constexpr std::string_view kLibrary = ".dll";
#define HAILSTONE_OS "windows"
#elif defined(__APPLE__)
constexpr std::string_view kLibrary = ".dylib";
#define HAILSTONE_OS "macos"
#else
constexpr std::string_view kLibrary = ".so";
#define HAILSTONE_OS "linux"
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
constexpr std::string_view kPlatform = HAILSTONE_OS "-arm64";
#else
constexpr std::string_view kPlatform = HAILSTONE_OS "-x64";
#endif
#undef HAILSTONE_OS

// The lower-cased name's extension when it is one of `list`'s, else "".
template <std::size_t N> std::string_view ext_in(std::string_view name, const std::array<const char*, N>& list) {
    for (std::string_view ext : list)
        if (name.size() > ext.size() && name.ends_with(ext)) return ext;
    return {};
}

// The shape rules of is_safe_server_file_path, without the one on programs.
bool is_safe_path_shape(std::string_view path) {
    if (path.empty() || path.size() > kServerFilesMaxPath) return false;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        std::string_view part = path.substr(start, end - start);
        // Windows drops a trailing dot or space, so "evil.exe." would be written as evil.exe past the program rules;
        // device names (NUL, com1.txt) open a device. Both refused on every platform (core/windows_names.h).
        if (part.empty() || part.front() == '.' || is_windows_aliased_part(part)) return false;
        for (char c : part)
            if (static_cast<unsigned char>(c) < 0x20 || c == ':' || c == '\\' || c == '*' || c == '?' || c == '"' ||
                c == '<' || c == '>' || c == '|')
                return false;
        start = end + 1;
    }
    return true;
}

constexpr std::string_view kRecordHeader = "# hailstone server files 2";

std::string megabytes(std::uint64_t bytes) {
    return std::format("{:.1f}", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

} // namespace

bool is_safe_server_file_path(std::string_view path) {
    if (!is_safe_path_shape(path)) return false;
    return ext_in(lower(path.substr(path.rfind('/') == std::string_view::npos ? 0 : path.rfind('/') + 1)), kRefused).empty();
}

bool is_safe_file_path_shape(std::string_view path) { return is_safe_path_shape(path); }

std::string_view plugin_platform() { return kPlatform; }

std::optional<std::string> server_file_destination(std::string_view path, bool* other_platform) {
    if (other_platform) *other_platform = false;
    if (!is_safe_path_shape(path)) return std::nullopt;
    std::string low = lower(path);
    std::string name = low.substr(low.rfind('/') == std::string::npos ? 0 : low.rfind('/') + 1);
    std::string_view code = ext_in(name, kRefused);
    if (low == "hailstone.cfg") return std::string(path);
    if (low.starts_with("scripts/")) {
        if (!code.empty() && code != ".lua") return std::nullopt;
        return std::string(path);
    }
    if (low.starts_with("plugins/")) {
        std::string rest(path.substr(8));
        std::string folder = lower(rest.substr(0, rest.find('/')));
        if (rest.find('/') != std::string::npos &&
            std::find(kPlatforms.begin(), kPlatforms.end(), std::string_view(folder)) != kPlatforms.end()) {
            if (folder != kPlatform) {
                if (other_platform) *other_platform = true;
                return std::nullopt;
            }
            rest.erase(0, folder.size() + 1);
        }
        if (std::string_view lib = ext_in(name, kLibraries); !lib.empty()) {
            if (lib == kLibrary) return "plugins/" + rest;
            if (other_platform) *other_platform = true;
            return std::nullopt;
        }
        // A plugin's Lua windows: plugins/<name>/ui/<pack>/... (plugin::UiBridge).
        if (code == ".lua" && lower(rest).find("/ui/") != std::string::npos &&
            lower(rest).find("/ui/") == rest.find('/'))
            return "plugins/" + rest;
        if (!code.empty()) return std::nullopt;
        return "plugins/" + rest;
    }
    if (!code.empty()) return std::nullopt;
    if (low.starts_with("assets/")) return "assets/" + std::string(path.substr(7));
    return "assets/" + std::string(path);
}

plugin::Rules cached_server_rules(const fs::path& profile_dir) {
    plugin::Rules rules;
    if (!profile_dir.empty()) {
        ServerFileSync record(profile_dir, {});
        std::vector<plugin::ListedFile> files;
        for (const auto& f : record.installed()) files.push_back({f.path, f.sha256});
        rules = plugin::server_rules(profile_dir, files);
    }
    rules.provisional = true;
    return rules;
}

std::optional<std::set<std::string>> server_asset_only(const fs::path& profile_dir) {
    ServerFileSync record(profile_dir, {});
    std::vector<plugin::ListedFile> files;
    for (const auto& f : record.installed()) files.push_back({f.path, f.sha256});
    plugin::Rules rules = plugin::server_rules(profile_dir, files);
    if (!rules.configured || rules.player_assets) return std::nullopt;
    std::set<std::string> only;
    for (const auto& f : files)
        if (lower(f.path).starts_with("assets/")) only.insert(lower(f.path.substr(7)));
    return only;
}

bool asset_counts(const std::set<std::string>& only, std::string_view normalized) {
    constexpr std::array kTextures{".dds", ".bmp", ".tga", ".png", ".jpg", ".jpeg"};
    return only.contains(std::string(normalized)) || !ext_in(normalized, kTextures).empty();
}

ServerFileSync::ServerFileSync(fs::path profile_dir, Send send, Limits limits)
    : dir_(std::move(profile_dir)), send_(std::move(send)), limits_(limits) {}

ServerFileSync::~ServerFileSync() = default;

void ServerFileSync::start(Clock::time_point now) {
    state_ = State::Listing;
    checks_.clear();
    next_check_ = 0;
    hashing_.reset();
    rehash_.clear();
    list_.clear();
    list_id_ = total_ = 0;
    last_reply_ = now;
    std::error_code ec;
    list_timeout_ = fs::exists(record_file(), ec) ? Clock::duration(kKnownListTimeout) : Clock::duration(kListTimeout);
    send_(encode_server_files_list_request(0));
}

void ServerFileSync::fail(std::string message) {
    log::warn("server files: {}", message);
    error_ = std::move(message);
    state_ = State::Failed;
    incoming_.clear(); // closes the files; what came stays in .download/ for the next try
    asks_.clear();
    hashing_.reset();
    rehash_.clear();
}

void ServerFileSync::handle(const ServerFilesReply& reply, Clock::time_point now) {
    if (finished() || state_ == State::Idle) return;
    last_reply_ = now;
    if (const auto* page = std::get_if<ServerFilesPage>(&reply)) {
        if (state_ != State::Listing) return;
        if (page->total == 0) {
            log::info("server files: the server has none to send");
            load_record();
            list_id_ = page->list_id;
            plan(now); // deletes what an earlier list brought
            return;
        }
        if (list_.empty()) {
            list_id_ = page->list_id;
            total_ = page->total;
            list_.reserve(total_);
        }
        if (page->list_id != list_id_ || page->total != total_ || page->first != list_.size()) {
            // The server's files changed while the list came: start again.
            if (++restarts_ > 3) return fail("the server's list of files keeps changing");
            start(now);
            return;
        }
        if (page->entries.empty() && list_.size() < total_) return fail("the server sent an empty page of its list");
        for (const auto& e : page->entries) list_.push_back(e);
        if (list_.size() < total_)
            send_(encode_server_files_list_request(static_cast<std::uint32_t>(list_.size())));
        else
            plan(now);
        return;
    }
    if (const auto* err = std::get_if<ServerFilesError>(&reply)) {
        // The server answers each request it holds for an old list with this error, up to kWindow of them: only
        // the first one for the current list lists again; the others, and any that come while listing, are left.
        if (state_ != State::Downloading || err->list_id != list_id_) return;
        if (err->code == kServerFilesStale && ++restarts_ <= 3) {
            log::info("server files: the server's files changed during the download; listing them again");
            incoming_.clear();
            asks_.clear();
            outstanding_ = 0;
            start(now);
            return;
        }
        std::string name = err->index < list_.size() ? list_[err->index].path : std::to_string(err->index);
        return fail(err->code == kServerFilesUnreadable ? "the server could not read " + name
                                                        : "the server's files keep changing");
    }
    const auto& chunk = std::get<ServerFilesChunk>(reply);
    if (state_ != State::Downloading || chunk.list_id != list_id_) return;
    auto it = incoming_.find(chunk.index);
    if (it == incoming_.end()) return;
    Incoming& in = *it->second;
    const ServerFileEntry& e = list_[chunk.index];
    std::uint32_t want = std::min<std::uint32_t>(kServerFilesChunkMax, e.size - std::min(e.size, chunk.offset));
    if (chunk.offset >= e.size || chunk.data.size() != want)
        return fail(std::format("{}: a piece at {} of {} bytes, {} expected", e.path, chunk.offset, chunk.data.size(), want));
    std::size_t piece = chunk.offset / kServerFilesChunkMax;
    if (chunk.offset % kServerFilesChunkMax != 0 || piece >= in.pending.size() || in.pending[piece] == 0) {
        // Also the first answer to a piece asked for again after all.
        log::debug("server files: {}: ignored a piece at {} that was not asked for", e.path, chunk.offset);
        return;
    }
    const std::uint64_t number = in.pending[piece];
    in.pending[piece] = 0;
    asks_.erase(number);
    --outstanding_;
    last_piece_ = now;
    in.out.seekp(chunk.offset);
    in.out.write(reinterpret_cast<const char*>(chunk.data.data()), static_cast<std::streamsize>(chunk.data.size()));
    if (!in.out) return fail("could not write " + (staging_dir() / dest_[chunk.index]).string());
    if (chunk.offset == in.next && in.in_order)
        in.hash.update(std::span<const std::byte>(chunk.data));
    else
        in.in_order = false;
    in.next = chunk.offset + want;
    in.received += want;
    bytes_done_ += want;
    rate_bytes_ += want;
    // A file that came out of order is hashed by update() alone, once a frame: the frame drains every chunk that came,
    // so a budget spent here would be spent once a chunk, not once a frame.
    if (in.received == e.size) received_file(chunk.index);
    // The server answers in the order asked: the pieces asked before this one that have not come were skipped.
    while (state_ == State::Downloading && !asks_.empty() && asks_.begin()->first < number)
        ask_again(asks_.begin()->first);
    if (state_ == State::Downloading) ask_more();
}

void ServerFileSync::ask(std::uint32_t index, std::uint32_t offset, std::uint32_t length, int tries) {
    std::uint64_t number = ++ask_number_;
    incoming_.at(index)->pending[offset / kServerFilesChunkMax] = number;
    asks_[number] = Ask{index, offset, length, tries};
    send_(encode_server_files_chunk_request(list_id_, index, offset, length));
}

void ServerFileSync::ask_again(std::uint64_t number) {
    auto it = asks_.find(number);
    if (it == asks_.end()) return;
    Ask a = it->second;
    asks_.erase(it);
    if (a.tries >= kChunkRetries)
        return fail(std::format("{}: the piece at {} did not come after {} requests", list_[a.index].path, a.offset,
                                a.tries));
    log::debug("server files: {}: asking again for the piece at {}", list_[a.index].path, a.offset);
    ask(a.index, a.offset, a.length, a.tries + 1);
}

void ServerFileSync::received_file(std::uint32_t index) {
    auto node = incoming_.extract(index);
    Incoming& in = *node.mapped();
    in.out.close();
    if (!in.in_order) {
        // A piece came again after later ones: the file is hashed from .download/ by update(), kHashBytesPerUpdate
        // a call, not all at once here (a large file would stop the frame).
        rehash_.push_back(index);
        return;
    }
    checked_file(index, in.hash.finish());
}

void ServerFileSync::checked_file(std::uint32_t index, const std::optional<Sha256Digest>& got) {
    const ServerFileEntry& e = list_[index];
    if (!got || *got != e.sha256) {
        std::error_code ec;
        fs::remove(staging_dir() / dest_[index], ec);
        return fail(e.path + " arrived damaged (its SHA-256 does not match the server's list)");
    }
    ready_.push_back(index);
    staged_[index] = true;
    if (ready_.size() == needed_.size()) commit();
}

void ServerFileSync::rehash() {
    std::vector<char> buf;
    std::uint64_t budget = hash_budget_;
    while (state_ == State::Downloading && !rehash_.empty() && budget > 0) {
        std::optional<Sha256Digest> got;
        if (!hash_more(staging_dir() / dest_[rehash_.front()], budget, buf, got)) return;
        const std::uint32_t index = rehash_.front();
        rehash_.erase(rehash_.begin());
        checked_file(index, got);
    }
}

void ServerFileSync::load_record() {
    record_.clear();
    std::ifstream in(record_file());
    std::string line;
    // A record without the header line came before scripts and plugins: its paths are in assets/.
    bool v2 = false;
    while (std::getline(in, line)) {
        if (line.starts_with(kRecordHeader)) v2 = true;
        // <sha256 hex> <size> <path>
        auto a = line.find(' ');
        auto b = a == std::string::npos ? a : line.find(' ', a + 1);
        if (b == std::string::npos) continue;
        auto sha = parse_sha256(std::string_view(line).substr(0, a));
        if (!sha) continue;
        Recorded r;
        r.sha256 = *sha;
        r.size = static_cast<std::uint32_t>(std::strtoul(line.c_str() + a + 1, nullptr, 10));
        r.path = line.substr(b + 1);
        if (!r.path.empty() && r.path.back() == '\r') r.path.pop_back();
        if (!v2) r.path = "assets/" + r.path;
        if (is_safe_path_shape(r.path)) record_[lower(r.path)] = std::move(r);
    }
}

void ServerFileSync::save_record() const {
    fs::path tmp = record_file();
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << kRecordHeader << "\n# The files Hailstone downloaded from this server: SHA-256, size, path.\n";
        for (const auto& [key, r] : record_) out << to_hex(r.sha256) << ' ' << r.size << ' ' << r.path << '\n';
    }
    std::error_code ec;
    fs::rename(tmp, record_file(), ec);
    if (ec) log::warn("server files: could not write {}: {}", record_file().string(), ec.message());
}

void ServerFileSync::plan(Clock::time_point now) {
    if (record_.empty()) load_record();
    needed_.clear();
    ready_.clear();
    staged_.assign(list_.size(), false);
    remove_.clear();
    bytes_total_ = bytes_done_ = 0;
    std::unordered_map<std::string, fs::path> present = present_files();
    std::error_code ec;
    std::unordered_map<std::string, bool>& listed = listed_;
    listed.clear();
    std::size_t& refused = refused_;
    std::size_t& skipped = skipped_;
    refused = skipped = 0;
    checks_.clear();
    next_check_ = 0;
    hashing_.reset();
    dest_.assign(list_.size(), std::string());
    for (std::uint32_t i = 0; i < list_.size(); ++i) {
        const ServerFileEntry& e = list_[i];
        bool other_platform = false;
        auto dest = server_file_destination(e.path, &other_platform);
        if (!dest) {
            if (other_platform) {
                ++skipped;
            } else {
                log::warn("server files: refused {}", e.path);
                ++refused;
            }
            continue;
        }
        if (e.size > limits_.max_file) {
            // A size in the list is the server's word alone (up to 4 GB a file, 65536 files): the bound is Hailstone's.
            log::warn("server files: refused {}: {} MB, more than the limit of {} MB a file", e.path, megabytes(e.size),
                      megabytes(limits_.max_file));
            ++refused;
            continue;
        }
        dest_[i] = *dest;
        std::string key = lower(*dest);
        if (listed.contains(key)) continue; // the same place twice (another case, or assets/x and x): the first
        listed[key] = true;
        Check c;
        c.index = i;
        c.key = key;
        if (auto p = present.find(key); p != present.end() && fs::file_size(p->second, ec) == e.size && !ec) {
            auto r = record_.find(key);
            if (r != record_.end() && r->second.sha256 == e.sha256 && r->second.size == e.size) continue;
            c.present = p->second;
        }
        fs::path staged = staging_dir() / *dest;
        if (fs::exists(staged, ec) && fs::file_size(staged, ec) == e.size) c.staged = staged;
        checks_.push_back(std::move(c));
    }
    // The hashing, kHashBytesPerUpdate at a time: a folder of large files the record does not know (deleted, or
    // copied in by hand) would otherwise stop the frame for as long as reading them takes.
    if (!verify()) {
        state_ = State::Verifying;
        return;
    }
    finish_plan(now);
}

bool ServerFileSync::hash_more(const fs::path& path, std::uint64_t& budget, std::vector<char>& buf,
                               std::optional<Sha256Digest>& got) {
    if (!hashing_) {
        hashing_ = std::make_unique<Hashing>();
        hashing_->in.open(path, std::ios::binary);
    }
    got.reset();
    if (hashing_->in) {
        if (buf.empty()) buf.resize(1 << 16);
        while (hashing_->in && budget > 0) {
            const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), budget));
            hashing_->in.read(buf.data(), static_cast<std::streamsize>(want));
            auto n = static_cast<std::size_t>(hashing_->in.gcount());
            if (n) hashing_->hash.update(std::string_view(buf.data(), n));
            budget -= std::min<std::uint64_t>(budget, std::max<std::size_t>(n, 1));
        }
        if (hashing_->in) return false; // more to read
        got = hashing_->hash.finish();
    }
    hashing_.reset();
    return true;
}

bool ServerFileSync::verify() {
    std::vector<char> buf;
    std::uint64_t budget = hash_budget_;
    while (next_check_ < checks_.size()) {
        Check& c = checks_[next_check_];
        const ServerFileEntry& e = list_[c.index];
        if (c.step == 0 && c.present.empty()) c.step = 1;
        if (c.step == 1 && c.staged.empty()) c.step = 2;
        if (c.step == 2) {
            needed_.push_back(c.index);
            ++next_check_;
            continue;
        }
        std::optional<Sha256Digest> got;
        if (!hash_more(c.step == 0 ? c.present : c.staged, budget, buf, got)) return false; // the next call goes on
        const bool match = got && *got == e.sha256;
        if (c.step == 0) {
            if (match) {
                record_[c.key] = Recorded{dest_[c.index], e.size, e.sha256}; // the player's copy is the server's
                ++next_check_;
            } else {
                c.step = 1;
            }
            continue;
        }
        if (match) {
            ready_.push_back(c.index); // came complete in an earlier download
            staged_[c.index] = true;
        }
        c.step = 2;
    }
    return true;
}

void ServerFileSync::finish_plan(Clock::time_point now) {
    checks_.clear();
    next_check_ = 0;
    const std::unordered_map<std::string, bool>& listed = listed_;
    const std::size_t refused = refused_, skipped = skipped_;
    // Checked in the list's order whether hashed at once or over several calls.
    std::sort(needed_.begin(), needed_.end());
    std::sort(ready_.begin(), ready_.end());
    for (std::uint32_t i : needed_) bytes_total_ += list_[i].size;
    for (const auto& [key, r] : record_)
        if (!listed.contains(key)) remove_.push_back(key);
    for (std::uint32_t i : ready_) bytes_done_ += list_[i].size;
    if (bytes_total_ > limits_.max_total)
        return fail(std::format("the server's files to download come to {} MB, more than the limit of {} MB",
                                megabytes(bytes_total_), megabytes(limits_.max_total)));
    log::info("server files: {} listed, {} to download ({} MB), {} already downloaded, {} to delete{}{}", list_.size(),
              needed_.size() - ready_.size(), megabytes(bytes_total_ - bytes_done_), ready_.size(), remove_.size(),
              refused ? std::format(", {} refused", refused) : std::string(),
              skipped ? std::format(", {} for other platforms", skipped) : std::string());
    if (needed_.size() == ready_.size()) {
        commit();
        return;
    }
    state_ = State::Downloading;
    last_reply_ = now; // the chunk timeout counts from here, not from the list's last page before the hashing
    download_started_ = Clock::now();
    next_file_ = 0;
    next_offset_ = 0;
    outstanding_ = 0;
    asks_.clear();
    last_piece_ = now;
    rate_since_ = Clock::now();
    ask_more();
}

void ServerFileSync::ask_more() {
    while (outstanding_ < kWindow && next_file_ < needed_.size()) {
        std::uint32_t index = needed_[next_file_];
        const ServerFileEntry& e = list_[index];
        if (staged_[index]) {
            ++next_file_;
            next_offset_ = 0;
            continue;
        }
        if (next_offset_ == 0) {
            fs::path staged = staging_dir() / dest_[index];
            std::error_code ec;
            fs::create_directories(staged.parent_path(), ec);
            auto in = std::make_unique<Incoming>();
            in->pending.assign((std::size_t{e.size} + kServerFilesChunkMax - 1) / kServerFilesChunkMax, 0);
            in->out.open(staged, std::ios::binary | std::ios::trunc);
            if (!in->out) return fail("could not write " + staged.string());
            current_ = e.path;
            incoming_[index] = std::move(in);
            if (e.size == 0) {
                ++next_file_;
                received_file(index);
                if (state_ != State::Downloading) return;
                continue;
            }
        }
        std::uint32_t len = std::min(kServerFilesChunkMax, e.size - next_offset_);
        ask(index, next_offset_, len, 1);
        ++outstanding_;
        next_offset_ += len;
        if (next_offset_ >= e.size) {
            ++next_file_;
            next_offset_ = 0;
        }
    }
}

void ServerFileSync::commit() {
    std::error_code ec;
    std::unordered_map<std::string, fs::path> present = present_files();
    // Only the asset layer is read again (ClientApp::reload_server_files); scripts and plugins are read when run.
    auto asset = [](std::string_view key) { return key.starts_with("assets/"); };
    for (std::uint32_t i : ready_) {
        const ServerFileEntry& e = list_[i];
        std::string key = lower(dest_[i]);
        fs::path dst = dir_ / dest_[i];
        fs::create_directories(dst.parent_path(), ec);
        // The same name in another case would be a second file to GameDir's case-insensitive index.
        if (auto p = present.find(key); p != present.end() && p->second != dst) fs::remove(p->second, ec);
        ec.clear();
        fs::rename(staging_dir() / dest_[i], dst, ec);
        if (ec) return fail("could not move " + e.path + " into " + dst.string() + ": " + ec.message());
        record_[key] = Recorded{dest_[i], e.size, e.sha256};
        if (asset(key)) changed_ = true;
    }
    for (const auto& key : remove_) {
        if (auto p = present.find(key); p != present.end()) {
            fs::remove(p->second, ec);
            if (asset(key)) changed_ = true;
        }
        record_.erase(key);
    }
    save_record();
    fs::remove_all(staging_dir(), ec);
    double secs = state_ == State::Downloading ? std::chrono::duration<double>(Clock::now() - download_started_).count() : 0;
    log::info("server files: {} installed, {} deleted{}", ready_.size(), remove_.size(),
              secs > 0 ? std::format(" ({} MB in {:.1f} s, {} MB/s)", megabytes(bytes_total_), secs,
                                     megabytes(static_cast<std::uint64_t>(static_cast<double>(bytes_total_) / secs)))
                       : std::string());
    state_ = State::Done;
}

std::unordered_map<std::string, fs::path> ServerFileSync::present_files() const {
    // By lower-cased place: EQ's names are case-insensitive (core/game_dir.h).
    std::unordered_map<std::string, fs::path> present;
    std::error_code ec;
    for (const char* sub : {"assets", "plugins", "scripts"})
        for (fs::recursive_directory_iterator it(dir_ / sub, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec)) present[lower(fs::relative(it->path(), dir_, ec).generic_string())] = it->path();
    ec.clear();
    for (const auto& e : fs::directory_iterator(dir_, ec))
        if (lower(e.path().filename().string()) == "hailstone.cfg" && e.is_regular_file(ec))
            present["hailstone.cfg"] = e.path();
    return present;
}

std::vector<ServerFileSync::Installed> ServerFileSync::installed() {
    if (record_.empty()) load_record();
    std::vector<Installed> out;
    out.reserve(record_.size());
    for (const auto& [key, r] : record_) out.push_back({r.path, r.sha256});
    return out;
}

void ServerFileSync::update(Clock::time_point now) {
    if (state_ == State::Verifying) {
        if (verify()) finish_plan(now);
        return;
    }
    if (state_ == State::Listing && now - last_reply_ > list_timeout_) {
        log::info("server files: no answer (a server without Hailstone's script)");
        state_ = State::NoAnswer;
    }
    if (state_ == State::Downloading && !rehash_.empty()) {
        rehash();
        if (asks_.empty()) last_reply_ = now; // nothing asked for: the hashing is what is left, not a silent server
    }
    if (state_ == State::Downloading && now - last_reply_ > kChunkTimeout) {
        fail("the server stopped sending its files");
    } else if (state_ == State::Downloading && !asks_.empty() && now - last_piece_ >= kChunkRetry) {
        // Nothing for kChunkRetry: what is outstanding was lost (the last requests, with none after them to show
        // it). Asked for again in the order first asked. A piece asked kChunkRetries times is not asked again after
        // a silence, nor failed for it: a silent server is kChunkTimeout's to end, so it waits that out.
        last_piece_ = now;
        std::vector<std::uint64_t> silent;
        for (const auto& [number, a] : asks_)
            if (a.tries < kChunkRetries) silent.push_back(number);
        for (std::uint64_t number : silent) {
            ask_again(number);
            if (state_ != State::Downloading) break;
        }
    }
    if (state_ == State::Downloading && now - rate_since_ >= std::chrono::seconds(1)) {
        double secs = std::chrono::duration<double>(now - rate_since_).count();
        double r = static_cast<double>(rate_bytes_) / secs;
        rate_ = rate_ == 0 ? r : rate_ * 0.6 + r * 0.4;
        rate_bytes_ = 0;
        rate_since_ = now;
    }
}

std::string ServerFileSync::status() const {
    switch (state_) {
    case State::Listing: return "Checking the server's files";
    case State::Verifying:
        return std::format("Checking the server's files ({} of {})", next_check_, checks_.size());
    case State::Downloading:
        return std::format("{} of {} files, {} of {} MB{}", ready_.size(), needed_.size(), megabytes(bytes_done_),
                           megabytes(bytes_total_), rate_ > 0 ? std::format(" ({} MB/s)", megabytes(static_cast<std::uint64_t>(rate_))) : "");
    default: return {};
    }
}

float ServerFileSync::progress() const {
    if (state_ != State::Downloading || bytes_total_ == 0) return -1.0f;
    return static_cast<float>(static_cast<double>(bytes_done_) / static_cast<double>(bytes_total_));
}

} // namespace hailstone::net
