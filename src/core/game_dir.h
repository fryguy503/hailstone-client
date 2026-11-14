#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hailstone {

class SharedFileCache;

// The player's RoF2 installation. EQ treats file names case-insensitively,
// so lookups go through a lower-cased index of the directory tree, which
// also makes them work on case-sensitive file systems.
//
// The files come in layers. The base is the asset folder the player picked; it is only ever read. Above it an
// overlay, a server's assets folder (servers/<profile>/assets), may replace any file by name: a whole file, so an
// archive in the overlay replaces the whole archive. A file found only in the overlay (a new zone) is found too.
// Below the base a classic folder (another install with older files, such as EverQuest Legends) fills in files the
// base lacks; for the names its `prefer` rule picks, it comes before the base (never before the overlay).
//
// The holders of a const GameDir& stay valid when the overlay changes: set_overlay() rebuilds the index in place.
// What they loaded and cached from the old layers is theirs to reload. The index is guarded, so lookups from other
// threads are safe while it is rebuilt; root() and base_root() return references and change only in set_base().
class GameDir {
public:
    // A file seen through the layers: its name as on disk, its relative path (with '/'), and where it is.
    struct Entry {
        std::string name;
        std::string relative;
        std::filesystem::path path;
    };

    // An empty GameDir: no layers, nothing is found. The asset-folder picker starts from this and calls set_base().
    GameDir();
    // The base asset folder alone; throws std::runtime_error when it is not a directory.
    explicit GameDir(std::filesystem::path root);
    // The base and an overlay (see set_overlay()).
    GameDir(std::filesystem::path root, std::optional<std::filesystem::path> overlay);

    GameDir(const GameDir& other);
    GameDir& operator=(const GameDir& other);
    GameDir(GameDir&& other) noexcept;
    GameDir& operator=(GameDir&& other) noexcept;

    // The base asset folder (empty for an empty GameDir). root() is the same as base_root().
    const std::filesystem::path& root() const { return base_; }
    const std::filesystem::path& base_root() const { return base_; }
    bool has_base() const { return !base_.empty(); }
    std::optional<std::filesystem::path> overlay() const;
    std::optional<std::filesystem::path> classic() const;
    // The layers, highest priority first: the overlay (when set), the base (when set), then the classic folder.
    std::vector<std::filesystem::path> roots() const;

    // Optional immutable archive backing, shared by copies and retained when asset layers change.
    void set_shared_file_cache(std::shared_ptr<SharedFileCache> cache);
    std::shared_ptr<SharedFileCache> shared_file_cache() const;

    // Replaces the base folder and rebuilds the index; throws std::runtime_error when it is not a directory (the
    // GameDir is then unchanged).
    void set_base(std::filesystem::path root);
    // Sets or clears the overlay folder and rebuilds the index. An overlay folder that does not exist (yet) is an
    // empty layer; it is kept and seen by the next rescan().
    void set_overlay(std::optional<std::filesystem::path> overlay);
    // Sets or clears the classic folder and rebuilds the index. `prefer` gets a normalized relative name and says
    // whether the classic folder's copy comes before the base's; empty: it only fills in.
    using PreferRule = std::function<bool(std::string_view normalized)>;
    void set_classic(std::optional<std::filesystem::path> classic, PreferRule prefer = {});
    // Which of the overlay's files count (a normalized relative name; empty: all), for a server that uses only its
    // own files there (net::server_asset_filter). Rebuilds the merged view; no rescan.
    void set_overlay_filter(PreferRule keep);
    // Walks every layer again, to see files added or removed since.
    void rescan();

    // Path relative to the root, with '/' or '\\' separators, any case.
    std::optional<std::filesystem::path> find(std::string_view relative) const;
    bool exists(std::string_view relative) const { return find(relative).has_value(); }
    // The same lookup in the base folder only, ignoring the overlay (for example the install's own defaults).
    std::optional<std::filesystem::path> find_base(std::string_view relative) const;
    // True when `relative` is found and comes from the overlay.
    bool from_overlay(std::string_view relative) const;
    // True when `relative` is found and comes from the classic folder.
    bool from_classic(std::string_view relative) const;

    // Whole file contents; nullopt when the file is missing.
    std::optional<std::vector<std::byte>> read(std::string_view relative) const;

    // The files directly in a folder ("" is the top), across all layers, case-insensitively: one entry per name,
    // the highest layer's, sorted by lower-cased name. Use this, not directory_iterator over root().
    std::vector<Entry> list(std::string_view relative_dir) const;
    // The subfolders directly in a folder across all layers, by name as on disk (the highest layer's spelling),
    // sorted by lower-cased name. Folders holding no files at any depth are not seen.
    std::vector<std::string> list_dirs(std::string_view relative_dir) const;

    static std::string normalize(std::string_view relative);

    // The folder directly in a game folder (the base or the classic folder) that may hold Hailstone's own files, as
    // where a server's patcher installs it (<EverQuest>/hailstone/, core/asset_folder_choice.h data_folder_conflict).
    // Its name's case is ignored. It is never indexed: those are not game files, and they can be many.
    static constexpr std::string_view kOwnFolder = "hailstone";

private:
    struct Indexed {
        std::string relative; // as on disk, '/' separated
        std::filesystem::path path;
        bool overlay = false;
        bool classic = false;
    };
    using Index = std::unordered_map<std::string, Indexed>;

    static std::shared_ptr<const Index> scan(const std::filesystem::path& root, bool overlay, bool classic = false);
    void merge_locked();
    std::shared_ptr<const Index> index() const;

    std::filesystem::path base_;
    mutable std::mutex mutex_; // guards everything below
    std::shared_ptr<SharedFileCache> shared_file_cache_;
    std::optional<std::filesystem::path> overlay_;
    std::shared_ptr<const Index> base_index_;
    std::shared_ptr<const Index> overlay_index_;
    std::optional<std::filesystem::path> classic_;
    std::shared_ptr<const Index> classic_index_;
    PreferRule classic_prefer_;
    PreferRule overlay_keep_;
    std::shared_ptr<const Index> index_; // the merged view, never null
};

std::optional<std::vector<std::byte>> read_file(const std::filesystem::path& path);

} // namespace hailstone
