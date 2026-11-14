#pragma once

// Replacement textures (docs/graphics.md, "Texture overrides"): a folder of image files, PNG, DDS, BMP or TGA, each
// standing in for the game texture of the same name whatever its extension (KBARKD.BMP <- kbarkd.png), at any size.
// A file in a subfolder named after a zone (textures/gfaydark/kbarkd.png) is used in that zone before one elsewhere.
// Not the original's: the look only, never a file the game reads for anything else.
//
// An override is drawn as the original looks in an image viewer: decode_game_texture turns it the way the original's
// rows go (a BMP's bottom-origin rows for the WLD meshes, a DDS's top-origin ones), so a pack exported upright fits.
//
// A built pack (tools/texpack) adds, per texture:
// - its own mip chain, usually block compressed (BC7): such a file is taken as stored, its rows already the WLD
//   meshes' and its cut-out in its alpha, and keeps its compression on the GPU;
// - `<stem>.m.*`, the variant for masked materials (an 8-bit BMP's palette index 0 transparent);
// - `<stem>.k.*`, the variant for materials that read the alpha of a texture painted on a colour key
//   (assets::remove_color_key: dstree312.dds's leaves on cyan): the key taken out before the upscale, which would
//   otherwise smear it into the edges. The plain file keeps the colour under alpha 0 for opaque materials;
// - `<stem>.n.*` and `<stem>.r.*`, its normal and roughness maps (decode_texture_map);
// - `by-hash/<hash>.*`: a game name that stands for different images in different archives (w1 in each city), keyed
//   by texture_content_hash of the game's file, looked up before the name.

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "assets/texture.h"

namespace hailstone::assets {

class TextureOverrides {
public:
    // Indexes the image files under `folder` (any depth); an empty or missing folder turns the overrides off. Safe
    // while textures load on other threads (a character's own settings change the pack at zone-in): the new index
    // is built aside and swapped in, and a lookup keeps the index it started with.
    void set_folder(const std::filesystem::path& folder);
    std::filesystem::path folder() const { return index()->folder; }
    std::size_t size() const { return index()->files.size(); }

    // The file standing in for the game texture `name` (a file name, any case, any extension): the one for the game
    // file's content `hash` first (0: none), then the zone's own, then any. `suffix` picks a variant or map (".n").
    std::optional<std::filesystem::path> find(std::string_view name, std::string_view zone = {}, std::uint64_t hash = 0,
                                              std::string_view suffix = {}) const;
    // Whether any file is keyed by content hash, so that a lookup has to hash the game's file.
    bool has_hashed() const { return index()->hashed != 0; }
    // hailstone.ini display.material_maps: off, decode_texture_map finds nothing (the surfaces stay flat).
    void set_material_maps(bool on) { material_maps_ = on; }
    bool material_maps() const { return material_maps_; }

    // How many textures have been replaced since the start.
    std::size_t applied() const { return applied_.load(std::memory_order_relaxed); }
    void count_applied() { applied_.fetch_add(1, std::memory_order_relaxed); }

private:
    struct Index {
        std::filesystem::path folder;
        // "<stem>", "<subfolder>/<stem>" and "#<hash>" (by-hash/), lower case.
        std::unordered_map<std::string, std::filesystem::path> files;
        std::size_t hashed = 0;
    };
    std::shared_ptr<const Index> index() const {
        std::lock_guard lock(mutex_);
        return index_;
    }
    mutable std::mutex mutex_;
    std::shared_ptr<const Index> index_ = std::make_shared<const Index>();
    std::atomic<bool> material_maps_{true};
    std::atomic<std::size_t> applied_{0};
};

// The process's overrides (the client's and the viewer's).
TextureOverrides& texture_overrides();

// hailstone.ini display.texture_folder, a texture pack: a folder name under `packs` (the data folder's textures/,
// platform::textures_dir()), empty for the pack "default", "off" for none, or an absolute path to any folder.
std::filesystem::path texture_folder_setting(std::string_view value, const std::filesystem::path& packs);

// The Options window's texture pack choice: "Off", then the folders of `packs` (texture_pack_names, sorted). The
// choice display.texture_folder `value` is (0 off, -1 a folder given by path or not there), and the value a choice
// stores (nullopt out of range; the pack "default" is stored as empty, as texture_folder_setting reads it).
std::vector<std::string> texture_pack_names(const std::filesystem::path& packs);
int texture_pack_choice(std::string_view value, const std::vector<std::string>& names);
std::optional<std::string> texture_pack_value(int choice, const std::vector<std::string>& names);

// decode_texture for the game texture `name`: its override when there is one, oriented as `original` (the game's
// file, or nullopt when the game has none) would decode with `options`; else `original` decoded. A masked texture's
// fully opaque override takes its cut-out from the original's transparent texels.
std::optional<Image> decode_game_texture(std::string_view name, const std::optional<std::span<const std::byte>>& original,
                                         const TextureOptions& options = {}, std::string* error = nullptr,
                                         std::string_view zone = {});
// The by-hash/ key of a game texture file: FNV-1a 64 over its bytes, written as 16 lower-case hex digits.
std::uint64_t texture_content_hash(std::span<const std::byte> data);

enum class TextureMap { Normal, Roughness };

// A pack's normal (".n") or roughness (".r") map for the game texture `name`, found as its colour is; nullopt when the
// pack has none. Oriented as decode_game_texture's colour with the same `wld_bmp_rows`.
std::optional<Image> decode_texture_map(std::string_view name, const std::optional<std::span<const std::byte>>& original,
                                        TextureMap map, bool wld_bmp_rows, std::string_view zone = {});
inline std::optional<Image> decode_texture_map(std::string_view name, const std::optional<std::vector<std::byte>>& original,
                                               TextureMap map, bool wld_bmp_rows, std::string_view zone = {}) {
    std::optional<std::span<const std::byte>> bytes;
    if (original) bytes = std::span<const std::byte>(*original);
    return decode_texture_map(name, bytes, map, wld_bmp_rows, zone);
}

inline std::optional<Image> decode_game_texture(std::string_view name, const std::optional<std::vector<std::byte>>& original,
                                                const TextureOptions& options = {}, std::string* error = nullptr,
                                                std::string_view zone = {}) {
    std::optional<std::span<const std::byte>> bytes;
    if (original) bytes = std::span<const std::byte>(*original);
    return decode_game_texture(name, bytes, options, error, zone);
}

} // namespace hailstone::assets
