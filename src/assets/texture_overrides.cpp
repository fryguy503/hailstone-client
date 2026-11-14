#include "assets/texture_overrides.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <system_error>

#include "core/game_dir.h"
#include "core/log.h"

namespace hailstone::assets {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The name without its folders and extension.
std::string stem_of(std::string_view name) {
    if (auto slash = name.find_last_of("/\\"); slash != std::string_view::npos) name = name.substr(slash + 1);
    if (auto dot = name.rfind('.'); dot != std::string_view::npos && dot > 0) name = name.substr(0, dot);
    return lower(name);
}

bool image_extension(const std::filesystem::path& p) {
    std::string ext = lower(p.extension().string());
    return ext == ".png" || ext == ".dds" || ext == ".bmp" || ext == ".tga";
}

void flip_rows(Image& img) {
    std::size_t row = std::size_t(img.width) * 4;
    auto& level = img.mips[0];
    for (std::uint32_t y = 0; y < img.height / 2; ++y)
        std::swap_ranges(level.begin() + static_cast<std::ptrdiff_t>(y * row),
                         level.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                         level.begin() + static_cast<std::ptrdiff_t>((img.height - 1 - y) * row));
}

bool starts_bm(std::span<const std::byte> d) {
    return d.size() >= 2 && d[0] == std::byte{'B'} && d[1] == std::byte{'M'};
}

std::string hex16(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::uint64_t hash_of(const std::optional<std::span<const std::byte>>& original) {
    return original && texture_overrides().has_hashed() ? texture_content_hash(*original) : 0;
}

// Whether the original's rows are a BMP's, bottom-origin (a name ending .bmp when the game has no file).
bool bmp_rows(std::string_view name, const std::optional<std::span<const std::byte>>& original) {
    return original ? starts_bm(*original) : lower(name).ends_with(".bmp");
}

// A built pack's file (its own mips) is used as stored, its rows the WLD meshes'; for a caller wanting a BMP's rows
// upright it is decoded and turned, losing its mips and compression (a rare use: particles).
std::optional<Image> as_stored(Image img, bool wld_bmp_rows, bool bmp) {
    if (wld_bmp_rows || !bmp) return img;
    Image out = to_rgba8(img);
    out.mips.resize(1);
    flip_rows(out);
    return out;
}

std::optional<Image> read_override(const std::filesystem::path& file, std::string& why) {
    auto bytes = read_file(file);
    if (!bytes) {
        why = "cannot be read";
        return std::nullopt;
    }
    return decode_texture(*bytes, {}, &why);
}

} // namespace

void TextureOverrides::set_folder(const std::filesystem::path& folder) {
    auto index = std::make_shared<Index>();
    index->folder = folder;
    std::error_code ec;
    if (!folder.empty() && std::filesystem::is_directory(folder, ec)) {
        auto& files = index->files;
        // Of several files for one key the shallowest wins, then the first by path: never the order the folder
        // happens to be listed in, which differs between file systems (NTFS lists gfaydark/ before kbarkd.png, ext4
        // need not).
        auto depth = [&](const std::filesystem::path& p) {
            const std::filesystem::path rel = p.lexically_relative(folder);
            return std::distance(rel.begin(), rel.end());
        };
        auto keep = [&](const std::string& key, const std::filesystem::path& p) {
            auto [slot, added] = files.try_emplace(key, p);
            if (added) return true;
            const auto d = depth(p), held = depth(slot->second);
            if (d < held || (d == held && p < slot->second)) slot->second = p;
            return false;
        };
        for (auto it = std::filesystem::recursive_directory_iterator(folder, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec) || !image_extension(it->path())) continue;
            std::string stem = stem_of(it->path().filename().string());
            // A zone's subfolder keys its files apart, and they stand for the bare name too unless a shallower file
            // does.
            auto parent = it->path().parent_path();
            std::string sub = parent != folder ? lower(parent.filename().string()) : std::string();
            if (sub == "by-hash") {
                index->hashed += keep("#" + stem, it->path());
                continue;
            }
            if (!sub.empty()) keep(sub + "/" + stem, it->path());
            keep(stem, it->path());
        }
        if (!files.empty()) log::info("texture overrides: {} names in {}", files.size(), folder.string());
    }
    std::lock_guard lock(mutex_);
    index_ = std::move(index);
}

std::optional<std::filesystem::path> TextureOverrides::find(std::string_view name, std::string_view zone,
                                                            std::uint64_t hash, std::string_view suffix) const {
    const auto index = this->index();
    const auto& files = index->files;
    if (files.empty()) return std::nullopt;
    if (hash != 0 && index->hashed != 0)
        if (auto it = files.find("#" + hex16(hash) + lower(suffix)); it != files.end()) return it->second;
    std::string stem = stem_of(name) + lower(suffix);
    if (!zone.empty())
        if (auto it = files.find(lower(zone) + "/" + stem); it != files.end()) return it->second;
    auto it = files.find(stem);
    if (it == files.end()) return std::nullopt;
    return it->second;
}

std::uint64_t texture_content_hash(std::span<const std::byte> data) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::byte b : data) {
        h ^= static_cast<std::uint8_t>(b);
        h *= 0x100000001b3ull;
    }
    return h;
}

std::filesystem::path texture_folder_setting(std::string_view value, const std::filesystem::path& packs) {
    if (value.empty()) return packs / "default";
    if (lower(value) == "off") return {};
    std::filesystem::path p(value);
    return p.is_absolute() ? p : packs / p;
}

std::vector<std::string> texture_pack_names(const std::filesystem::path& packs) {
    std::vector<std::string> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(packs, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) out.push_back(it->path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

int texture_pack_choice(std::string_view value, const std::vector<std::string>& names) {
    if (lower(value) == "off") return 0;
    const std::string name = value.empty() ? std::string("default") : std::string(value);
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return static_cast<int>(i + 1);
    return -1;
}

std::optional<std::string> texture_pack_value(int choice, const std::vector<std::string>& names) {
    if (choice < 0 || choice > static_cast<int>(names.size())) return std::nullopt;
    if (choice == 0) return std::string("off");
    const std::string& name = names[static_cast<std::size_t>(choice - 1)];
    return name == "default" ? std::string() : name;
}

TextureOverrides& texture_overrides() {
    static TextureOverrides overrides;
    return overrides;
}

std::optional<Image> decode_game_texture(std::string_view name, const std::optional<std::span<const std::byte>>& original,
                                         const TextureOptions& options, std::string* error, std::string_view zone) {
    TextureOverrides& overrides = texture_overrides();
    const std::uint64_t hash = hash_of(original);
    const bool bmp = bmp_rows(name, original);
    std::optional<std::filesystem::path> file;
    if (options.palette_index0_transparent && bmp) file = overrides.find(name, zone, hash, ".m");
    if (!file && options.reads_alpha) file = overrides.find(name, zone, hash, ".k");
    if (!file) file = overrides.find(name, zone, hash);
    if (file) {
        std::string why;
        std::optional<Image> img = read_override(*file, why);
        if (img && img->mips.size() > 1) {
            overrides.count_applied();
            return as_stored(std::move(*img), options.wld_bmp_rows, bmp);
        }
        if (img) {
            Image out = to_rgba8(*img);
            out.mips.resize(1);
            // The original's rows: a BMP's bottom-origin ones for the WLD meshes.
            if (options.wld_bmp_rows && bmp) flip_rows(out);
            // A cut-out without its cut: the original's transparent texels, scaled up.
            if (options.palette_index0_transparent && original) {
                bool opaque = true;
                for (std::size_t i = 3; i < out.mips[0].size() && opaque; i += 4) opaque = out.mips[0][i] == std::byte{0xff};
                if (opaque)
                    if (auto src = decode_texture(*original, options)) {
                        Image a = to_rgba8(*src);
                        for (std::uint32_t y = 0; y < out.height; ++y)
                            for (std::uint32_t x = 0; x < out.width; ++x) {
                                std::size_t sx = std::size_t(x) * a.width / out.width;
                                std::size_t sy = std::size_t(y) * a.height / out.height;
                                out.mips[0][(std::size_t(y) * out.width + x) * 4 + 3] =
                                    a.mips[0][(sy * a.width + sx) * 4 + 3];
                            }
                    }
            }
            overrides.count_applied();
            return out;
        }
        log::warn("texture override {}: {}; the game's is used", file->string(), why);
    }
    if (!original) {
        if (error) *error = "not found";
        return std::nullopt;
    }
    return decode_texture(*original, options, error);
}

std::optional<Image> decode_texture_map(std::string_view name, const std::optional<std::span<const std::byte>>& original,
                                        TextureMap map, bool wld_bmp_rows, std::string_view zone) {
    TextureOverrides& overrides = texture_overrides();
    if (!overrides.material_maps()) return std::nullopt;
    const std::optional<std::filesystem::path> file =
        overrides.find(name, zone, hash_of(original), map == TextureMap::Normal ? ".n" : ".r");
    if (!file) return std::nullopt;
    std::string why;
    std::optional<Image> img = read_override(*file, why);
    if (!img) {
        log::warn("texture map {}: {}", file->string(), why);
        return std::nullopt;
    }
    const bool bmp = bmp_rows(name, original);
    if (img->mips.size() > 1) return as_stored(std::move(*img), wld_bmp_rows, bmp);
    Image out = to_rgba8(*img);
    out.mips.resize(1);
    if (wld_bmp_rows && bmp) flip_rows(out);
    return out;
}

} // namespace hailstone::assets
