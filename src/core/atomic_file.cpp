#include "core/atomic_file.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

#include "core/log.h"
#include "core/path_utf8.h"
#include "platform/durable_file.h"

namespace hailstone {

bool write_file_atomic(const std::filesystem::path& path, std::string_view text) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    // A name no other writer (another thread, a second client) picks at the same time.
    static std::atomic<unsigned> serial{0};
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path tmp = path;
    tmp += ".tmp-" + std::to_string(stamp) + "-" + std::to_string(serial++);
    // On the disk before the rename (platform/durable_file.h): a power cut must leave the old file or the new one,
    // never an empty one the rename reached the disk before.
    if (!platform::write_file_synced(tmp, text)) {
        fs::remove(tmp, ec);
        log::warn("could not save {}: the write failed (disk full, or the folder is not writable)", path_utf8(path));
        return false;
    }
    platform::replace_file_synced(tmp, path, ec);
#ifdef _WIN32
    // Windows refuses the replace while another program has the file open without delete sharing (a second client
    // reading it, an editor, a virus scanner), which is usually over in a moment: a few more tries.
    for (int attempt = 0; ec && attempt < 5; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20 << attempt));
        platform::replace_file_synced(tmp, path, ec);
    }
#endif
    if (ec) {
        log::warn("could not save {}: {}", path_utf8(path), ec.message());
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        return false;
    }
    return true;
}

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream s;
    s << in.rdbuf();
    return std::move(s).str();
}

} // namespace hailstone
