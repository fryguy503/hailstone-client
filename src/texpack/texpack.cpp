#include "texpack/texpack.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <thread>

#include <bc7enc.h>
#include <rgbcx.h>

#include "assets/pfs.h"
#include "assets/texture_overrides.h"
#include "core/byte_io.h"

namespace hailstone::texpack {

namespace fs = std::filesystem;
using assets::Image;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool has_texture_extension(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    return ext == "bmp" || ext == "dds" || ext == "tga";
}

bool has_texture_magic(const std::vector<std::byte>& d) {
    auto at = [&](std::size_t i) { return std::to_integer<char>(d[i]); };
    return (d.size() >= 4 && at(0) == 'D' && at(1) == 'D' && at(2) == 'S' && at(3) == ' ') ||
           (d.size() >= 2 && at(0) == 'B' && at(1) == 'M');
}

std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out += c;
    }
    return out + "\"";
}

template <typename List> std::string json_list(const List& items) {
    std::string out = "[";
    for (const auto& s : items) out += (out.size() > 1 ? "," : "") + json_string(s);
    return out + "]";
}

// A flat JSON object as manifest_line writes it: strings (with \" and \\), numbers, true/false and lists of strings.
struct JsonValue {
    std::string text;               // a string's contents, a number's or a literal's text
    std::vector<std::string> list;  // a list's strings
    bool is_list = false;
};
std::optional<std::map<std::string, JsonValue, std::less<>>> parse_flat_json(std::string_view s) {
    std::size_t i = 0;
    auto skip = [&] {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    };
    auto read_string = [&](std::string& out) -> bool {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\') {
                if (++i >= s.size()) return false;
            }
            out += s[i++];
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    };
    std::map<std::string, JsonValue, std::less<>> out;
    skip();
    if (i >= s.size() || s[i] != '{') return std::nullopt;
    ++i;
    for (;;) {
        skip();
        if (i < s.size() && s[i] == '}') return out;
        std::string key;
        if (!read_string(key)) return std::nullopt;
        skip();
        if (i >= s.size() || s[i] != ':') return std::nullopt;
        ++i;
        skip();
        JsonValue v;
        if (i < s.size() && s[i] == '"') {
            if (!read_string(v.text)) return std::nullopt;
        } else if (i < s.size() && s[i] == '[') {
            v.is_list = true;
            ++i;
            for (;;) {
                skip();
                if (i < s.size() && s[i] == ']') {
                    ++i;
                    break;
                }
                std::string item;
                if (!read_string(item)) return std::nullopt;
                v.list.push_back(std::move(item));
                skip();
                if (i < s.size() && s[i] == ',') ++i;
            }
        } else {
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ' ') v.text += s[i++];
            if (v.text.empty()) return std::nullopt;
        }
        out[key] = std::move(v);
        skip();
        if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
        }
        if (i < s.size() && s[i] == '}') return out;
        return std::nullopt;
    }
}

template <typename T> std::optional<T> parse_number(std::string_view s) {
    T v{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
    return v;
}

} // namespace

std::string_view alpha_name(AlphaKind kind) {
    switch (kind) {
    case AlphaKind::None: return "none";
    case AlphaKind::Cutout: return "cutout";
    case AlphaKind::Blend: return "blend";
    }
    return "none";
}

std::optional<AlphaKind> alpha_from_name(std::string_view name) {
    if (name == "none") return AlphaKind::None;
    if (name == "cutout") return AlphaKind::Cutout;
    if (name == "blend") return AlphaKind::Blend;
    return std::nullopt;
}

AlphaKind alpha_kind(const Image& rgba) {
    bool clear = false, partial = false;
    const auto& px = rgba.mips[0];
    for (std::size_t i = 3; i < px.size(); i += 4) {
        auto a = std::to_integer<std::uint8_t>(px[i]);
        if (a == 0) clear = true;
        else if (a != 255) partial = true;
    }
    return partial ? AlphaKind::Blend : clear ? AlphaKind::Cutout : AlphaKind::None;
}

std::string hash_name(std::uint64_t hash) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(hash));
    return buf;
}

std::optional<std::uint64_t> parse_hash_name(std::string_view text) {
    if (text.size() != 16) return std::nullopt;
    std::uint64_t v = 0;
    auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), v, 16);
    if (ec != std::errc() || p != text.data() + text.size()) return std::nullopt;
    return v;
}

std::vector<fs::path> list_archives(const std::vector<fs::path>& folders, const std::vector<std::string>& only) {
    std::vector<fs::path> out;
    for (const fs::path& folder : folders) {
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string name = lower(it->path().filename().string());
            if (!name.ends_with(".s3d") && !name.ends_with(".eqg")) continue;
            if (!only.empty() &&
                std::none_of(only.begin(), only.end(), [&](const std::string& p) { return name.starts_with(lower(p)); }))
                continue;
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<Sources> decode_sources(std::span<const std::byte> data, std::string* error) {
    auto image = assets::decode_texture(data, {}, error);
    if (!image) return std::nullopt;
    Sources s;
    s.color = assets::to_rgba8(*image);
    s.color.mips.resize(1);
    const std::string stored = assets::describe_texture(data);
    if (stored.starts_with("BMP 8-bit")) {
        if (auto keyed = assets::decode_texture(data, {.palette_index0_transparent = true})) {
            Image m = assets::to_rgba8(*keyed);
            m.mips.resize(1);
            if (alpha_kind(m) != AlphaKind::None) s.masked = std::move(m);
        }
    }
    // Painted on a colour key: the variant for materials that read the alpha, the key taken out before the upscaler
    // can smear it into the edges (the plain file keeps the colour under alpha 0 for opaque materials).
    if (!stored.starts_with("BMP 8-bit") && alpha_kind(s.color) != AlphaKind::None) {
        Image k = s.color;
        if (assets::remove_color_key(k)) s.keyed = std::move(k);
    }
    return s;
}

// The archives are the player's game folder's, which nothing writes (docs/layout.md), so they are mapped rather than
// each read whole into every scan thread's heap (PfsArchive::Storage; an archive that cannot be mapped is read).
constexpr auto kArchiveStorage = assets::PfsArchive::Storage::Mapped;

ScanResult scan(const std::vector<fs::path>& archives, const ScanOptions& options) {
    ScanResult result;
    result.archives = archives.size();
    std::mutex mutex;
    std::map<std::uint64_t, TextureInfo> entries;
    std::atomic<std::size_t> next{0}, done{0};
    std::atomic<bool> stop{false};
    auto worker = [&] {
        for (std::size_t i; (i = next++) < archives.size();) {
            if (stop || (options.cancelled && options.cancelled())) {
                stop = true;
                return;
            }
            const fs::path& path = archives[i];
            const std::string label = lower(path.filename().string());
            try {
                auto archive = assets::PfsArchive::open(path, kArchiveStorage);
                for (const auto& name : archive.names()) {
                    // A member without a texture's extension is sniffed by its first bytes before it is inflated
                    // whole: most are meshes, WLDs and sounds.
                    if (!has_texture_extension(name)) {
                        auto head = archive.read_prefix(name, 4);
                        if (!head || !has_texture_magic(*head)) continue;
                    }
                    auto data = archive.read(name);
                    if (!data) continue;
                    const std::uint64_t hash = assets::texture_content_hash(*data);
                    {
                        std::lock_guard lock(mutex);
                        auto [it, fresh] = entries.try_emplace(hash);
                        it->second.names.insert(name);
                        it->second.archives.insert(label);
                        if (!fresh) {
                            // The first archive by path holds the source, whichever thread found it first, and
                            // even while that thread is still decoding it.
                            if (it->second.source_name.empty() || path < it->second.source_archive) {
                                it->second.source_archive = path;
                                it->second.source_name = name;
                            }
                            continue;
                        }
                    }
                    std::string why;
                    auto sources = decode_sources(*data, &why);
                    std::lock_guard lock(mutex);
                    if (!sources) {
                        result.errors.push_back(label + "/" + name + ": " + why);
                        entries.erase(hash);
                        continue;
                    }
                    TextureInfo& e = entries[hash];
                    e.hash = hash;
                    e.width = sources->color.width;
                    e.height = sources->color.height;
                    e.stored = assets::describe_texture(*data);
                    e.bmp = std::to_integer<char>((*data)[0]) == 'B';
                    e.alpha = alpha_kind(sources->color);
                    e.masked_variant = sources->masked.has_value();
                    e.keyed_variant = sources->keyed.has_value();
                    if (e.source_name.empty() || path < e.source_archive) {
                        e.source_archive = path;
                        e.source_name = name;
                    }
                }
            } catch (const std::exception& ex) {
                std::lock_guard lock(mutex);
                result.errors.push_back(label + ": " + ex.what());
            }
            std::size_t d = ++done;
            if (options.progress) options.progress(d, archives.size());
        }
    };
    unsigned n = options.threads ? options.threads : std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < n; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    result.cancelled = stop;
    // A texture whose archive failed before it was decoded has no size: dropped.
    for (auto& [hash, e] : entries)
        if (e.width) result.textures.push_back(std::move(e));
    std::sort(result.errors.begin(), result.errors.end());
    return result;
}

struct ArchiveCache {
    fs::path path;
    std::optional<assets::PfsArchive> archive;
};
void ArchiveCacheDeleter::operator()(ArchiveCache* c) const { delete c; }
std::unique_ptr<ArchiveCache, ArchiveCacheDeleter> make_archive_cache() {
    return std::unique_ptr<ArchiveCache, ArchiveCacheDeleter>(new ArchiveCache);
}

std::optional<Sources> read_sources(const TextureInfo& info, ArchiveCache* archive_cache, std::string* error) {
    try {
        std::optional<assets::PfsArchive> local;
        const assets::PfsArchive* archive = nullptr;
        if (archive_cache) {
            if (!archive_cache->archive || archive_cache->path != info.source_archive) {
                archive_cache->archive.reset();
                archive_cache->archive = assets::PfsArchive::open(info.source_archive, kArchiveStorage);
                archive_cache->path = info.source_archive;
            }
            archive = &*archive_cache->archive;
        } else {
            local = assets::PfsArchive::open(info.source_archive, kArchiveStorage);
            archive = &*local;
        }
        auto data = archive->read(info.source_name);
        if (!data) {
            if (error) *error = info.source_name + " is no longer in " + info.source_archive.filename().string();
            return std::nullopt;
        }
        if (assets::texture_content_hash(*data) != info.hash) {
            if (error) *error = info.source_archive.filename().string() + " changed since it was scanned";
            return std::nullopt;
        }
        return decode_sources(*data, error);
    } catch (const std::exception& ex) {
        if (archive_cache) archive_cache->archive.reset();
        if (error) *error = ex.what();
        return std::nullopt;
    }
}

std::string_view suffix(FileKind kind) {
    switch (kind) {
    case FileKind::Color: return "";
    case FileKind::Masked: return ".m";
    case FileKind::Keyed: return ".k";
    case FileKind::Normal: return ".n";
    case FileKind::Rough: return ".r";
    }
    return "";
}

std::optional<FileKind> kind_from_suffix(std::string_view s) {
    for (FileKind k : {FileKind::Color, FileKind::Masked, FileKind::Keyed, FileKind::Normal, FileKind::Rough})
        if (suffix(k) == s) return k;
    return std::nullopt;
}

bool is_data(const TextureInfo& info) {
    if (info.stored.find("Q8W8V8U8") != std::string::npos || info.stored.find("CxV8U8") != std::string::npos) return true;
    // Names (the stem, without the extension) that are data, not pictures: EQG terrain blend maps and the like.
    static const std::regex data_names(
        "(coverage|texturepalette|_mask|mask_|lightmap|_lm$|^fx_|_nm$|_nrm$|_normal|_n$|_bump)");
    for (const std::string& n : info.names) {
        std::string stem = lower(fs::path(n).stem().string());
        if (std::regex_search(stem, data_names)) return true;
    }
    return false;
}

bool reflects_edges(const TextureInfo& info) {
    if (info.archives.empty()) return false;
    return std::all_of(info.archives.begin(), info.archives.end(), [](const std::string& a) {
        return a.find("_chr") != std::string::npos || a.starts_with("chequip");
    });
}

bool uses_models(const TextureInfo& info) { return !is_data(info) && std::max(info.width, info.height) > kTinySide; }

std::vector<FileKind> pack_files(const TextureInfo& info, bool maps) {
    std::vector<FileKind> out{FileKind::Color};
    if (info.masked_variant) out.push_back(FileKind::Masked);
    if (info.keyed_variant) out.push_back(FileKind::Keyed);
    if (maps && uses_models(info)) {
        out.push_back(FileKind::Normal);
        out.push_back(FileKind::Rough);
    }
    return out;
}

// ---- encoding ------------------------------------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kDxgiBc4 = 80, kDxgiBc5 = 83, kDxgiBc7 = 98;

std::uint32_t level_dim(std::uint32_t d, std::size_t level) { return std::max<std::uint32_t>(1, d >> level); }

// A DX10 DDS of the given block-compressed levels.
std::vector<std::byte> dds_file(std::uint32_t w, std::uint32_t h, std::uint32_t dxgi,
                                const std::vector<std::vector<std::byte>>& levels) {
    ByteWriter out;
    for (char c : std::string_view("DDS ")) out.le(static_cast<std::uint8_t>(c));
    out.le<std::uint32_t>(124);
    out.le<std::uint32_t>(0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000); // caps, height, width, pixel format, mips, linear size
    out.le<std::uint32_t>(h);
    out.le<std::uint32_t>(w);
    out.le<std::uint32_t>(static_cast<std::uint32_t>(levels[0].size()));
    out.le<std::uint32_t>(0);
    out.le<std::uint32_t>(static_cast<std::uint32_t>(levels.size()));
    out.zeros(44);
    out.le<std::uint32_t>(32);
    out.le<std::uint32_t>(0x4); // FourCC
    for (char c : std::string_view("DX10")) out.le(static_cast<std::uint8_t>(c));
    out.zeros(20);
    out.le<std::uint32_t>(0x1000 | 0x400000 | 0x8); // texture, mipmap, complex
    out.zeros(16);
    out.le<std::uint32_t>(dxgi);
    out.le<std::uint32_t>(3); // TEXTURE2D
    out.le<std::uint32_t>(0);
    out.le<std::uint32_t>(1);
    out.le<std::uint32_t>(0);
    for (const auto& level : levels)
        for (std::byte b : level) out.le(std::to_integer<std::uint8_t>(b));
    return out.take();
}

enum class Codec { Bc7, Bc5, Bc4 };

// One RGBA8 level in 4x4 blocks, edge texels repeated into partial blocks.
std::vector<std::byte> encode_level(std::span<const std::byte> rgba, std::uint32_t w, std::uint32_t h, Codec codec) {
    const std::uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    const std::size_t block_bytes = codec == Codec::Bc4 ? 8 : 16;
    std::vector<std::byte> out(std::size_t(bw) * bh * block_bytes);
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    params.m_max_partitions = 16;
    std::uint8_t px[64];
    for (std::uint32_t by = 0; by < bh; ++by)
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            for (std::uint32_t t = 0; t < 16; ++t) {
                std::uint32_t x = std::min(bx * 4 + t % 4, w - 1), y = std::min(by * 4 + t / 4, h - 1);
                for (std::uint32_t c = 0; c < 4; ++c)
                    px[t * 4 + c] = std::to_integer<std::uint8_t>(rgba[(std::size_t(y) * w + x) * 4 + c]);
            }
            void* dst = out.data() + (std::size_t(by) * bw + bx) * block_bytes;
            switch (codec) {
            case Codec::Bc7: bc7enc_compress_block(dst, px, &params); break;
            case Codec::Bc5: rgbcx::encode_bc5_hq(dst, px); break;
            case Codec::Bc4: rgbcx::encode_bc4_hq(dst, px); break;
            }
        }
    return out;
}

// Box-filtered levels below level 0 (`normal`: as unit vectors in rgb, renormalised), down to 1x1.
void box_mips(Image& img, bool normal) {
    while (img.mips.size() < static_cast<std::size_t>(std::bit_width(std::max(img.width, img.height)))) {
        const std::size_t l = img.mips.size() - 1;
        const std::uint32_t sw = level_dim(img.width, l), sh = level_dim(img.height, l);
        const std::uint32_t dw = level_dim(img.width, l + 1), dh = level_dim(img.height, l + 1);
        const auto& src = img.mips[l];
        std::vector<std::byte> dst(std::size_t(dw) * dh * 4);
        for (std::uint32_t y = 0; y < dh; ++y)
            for (std::uint32_t x = 0; x < dw; ++x) {
                float sum[4] = {}, n = 0.0f;
                for (std::uint32_t yy = y * 2; yy < std::min(y * 2 + 2, sh); ++yy)
                    for (std::uint32_t xx = x * 2; xx < std::min(x * 2 + 2, sw); ++xx, n += 1.0f)
                        for (std::size_t c = 0; c < 4; ++c) {
                            float v = std::to_integer<std::uint8_t>(src[(std::size_t(yy) * sw + xx) * 4 + c]) / 255.0f;
                            sum[c] += normal && c < 3 ? v * 2.0f - 1.0f : v;
                        }
                if (normal) {
                    float len = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                    for (std::size_t c = 0; c < 3; ++c) sum[c] = len > 0 ? (sum[c] / len * 0.5f + 0.5f) * n : 0.5f * n;
                }
                for (std::size_t c = 0; c < 4; ++c)
                    dst[(std::size_t(y) * dw + x) * 4 + c] =
                        std::byte(static_cast<std::uint8_t>(std::lround(std::clamp(sum[c] / n, 0.0f, 1.0f) * 255.0f)));
            }
        img.mips.push_back(std::move(dst));
    }
}

void flip_rows(Image& img) {
    const std::size_t row = std::size_t(img.width) * 4;
    auto& level = img.mips[0];
    for (std::uint32_t y = 0; y < img.height / 2; ++y)
        std::swap_ranges(level.begin() + static_cast<std::ptrdiff_t>(y * row),
                         level.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                         level.begin() + static_cast<std::ptrdiff_t>((img.height - 1 - y) * row));
}

} // namespace

void init_encoders() {
    static std::once_flag once;
    std::call_once(once, [] {
        rgbcx::init();
        bc7enc_compress_block_init();
    });
}

std::vector<std::byte> encode_pack_file(Image img, FileKind kind, bool bmp) {
    init_encoders();
    img.mips.resize(1);
    if (bmp) {
        flip_rows(img);
        if (kind == FileKind::Normal)
            for (std::size_t p = 1; p < img.mips[0].size(); p += 4)
                img.mips[0][p] = std::byte(static_cast<std::uint8_t>(255 - std::to_integer<std::uint8_t>(img.mips[0][p])));
    }
    Codec codec = Codec::Bc7;
    std::uint32_t dxgi = kDxgiBc7;
    if (kind == FileKind::Normal) {
        box_mips(img, true);
        codec = Codec::Bc5;
        dxgi = kDxgiBc5;
    } else if (kind == FileKind::Rough) {
        box_mips(img, false);
        codec = Codec::Bc4;
        dxgi = kDxgiBc4;
    } else {
        // Only the masked and keyed variants' transparent texels are a colour key; a DDS's alpha 0 keeps the colour
        // an opaque material draws (generic_plate_blue_c.dds, the hero's forge plate).
        assets::build_color_mips(img, 0.5f, kind == FileKind::Masked || kind == FileKind::Keyed);
    }
    std::vector<std::vector<std::byte>> levels;
    for (std::size_t l = 0; l < img.mips.size(); ++l)
        levels.push_back(encode_level(img.mips[l], level_dim(img.width, l), level_dim(img.height, l), codec));
    return dds_file(img.width, img.height, dxgi, levels);
}

double bytes_per_texel(FileKind kind) {
    // BC7 and BC5 are a byte a texel, BC4 half; the mips add a third.
    return (kind == FileKind::Rough ? 0.5 : 1.0) * 4.0 / 3.0;
}

// ---- manifest and scan cache ---------------------------------------------------------------------------------------

std::string manifest_line(const TextureInfo& e) {
    char head[160];
    std::snprintf(head, sizeof head, "{\"hash\":\"%s\",\"w\":%u,\"h\":%u,", hash_name(e.hash).c_str(), e.width, e.height);
    std::string out = head;
    out += "\"stored\":" + json_string(e.stored) + ",\"bmp\":" + (e.bmp ? "true" : "false") + ",\"alpha\":\"" +
           std::string(alpha_name(e.alpha)) + "\",\"masked_variant\":" + (e.masked_variant ? "true" : "false") +
           ",\"keyed_variant\":" + (e.keyed_variant ? "true" : "false") + ",\"names\":" + json_list(e.names) +
           ",\"archives\":" + json_list(e.archives) + "}";
    return out;
}

std::optional<TextureInfo> parse_manifest_line(std::string_view line) {
    auto obj = parse_flat_json(line);
    if (!obj) return std::nullopt;
    auto get = [&](std::string_view k) -> const JsonValue* {
        auto it = obj->find(k);
        return it == obj->end() ? nullptr : &it->second;
    };
    TextureInfo e;
    const JsonValue* hash = get("hash");
    const JsonValue* w = get("w");
    const JsonValue* h = get("h");
    if (!hash || !w || !h) return std::nullopt;
    auto hv = parse_hash_name(hash->text);
    auto wv = parse_number<std::uint32_t>(w->text);
    auto hh = parse_number<std::uint32_t>(h->text);
    if (!hv || !wv || !hh) return std::nullopt;
    e.hash = *hv;
    e.width = *wv;
    e.height = *hh;
    if (auto* v = get("stored")) e.stored = v->text;
    if (auto* v = get("bmp")) e.bmp = v->text == "true";
    if (auto* v = get("alpha")) e.alpha = alpha_from_name(v->text).value_or(AlphaKind::None);
    if (auto* v = get("masked_variant")) e.masked_variant = v->text == "true";
    if (auto* v = get("keyed_variant")) e.keyed_variant = v->text == "true";
    if (auto* v = get("names")) e.names.insert(v->list.begin(), v->list.end());
    if (auto* v = get("archives")) e.archives.insert(v->list.begin(), v->list.end());
    if (auto* v = get("src_name")) e.source_name = v->text;
    // A texture is known by its names (the pack's files, the messages): a record without one is damaged.
    if (e.names.empty() || e.names.contains(std::string{})) return std::nullopt;
    return e;
}

namespace {

constexpr std::string_view kScanMagic = "hailstone-texpack-scan 1";

// "<size> <time> <path>" of an archive: what says it is the file scanned.
std::string archive_stamp(const fs::path& p) {
    std::error_code ec;
    auto size = fs::file_size(p, ec);
    if (ec) size = 0;
    auto time = fs::last_write_time(p, ec);
    long long ticks = ec ? 0 : static_cast<long long>(time.time_since_epoch().count());
    return std::to_string(size) + " " + std::to_string(ticks) + " " + p.generic_string();
}

} // namespace

bool save_scan(const fs::path& file, const std::vector<fs::path>& archives, const std::vector<TextureInfo>& textures) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::map<fs::path, std::size_t> index;
    for (std::size_t i = 0; i < archives.size(); ++i) index[archives[i]] = i;
    const fs::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return false;
        out << kScanMagic << "\n";
        for (const fs::path& a : archives) out << "A " << archive_stamp(a) << "\n";
        for (const TextureInfo& t : textures) {
            auto it = index.find(t.source_archive);
            if (it == index.end()) continue;
            std::string line = manifest_line(t);
            line.pop_back();
            line += ",\"src\":" + std::to_string(it->second) + ",\"src_name\":" + json_string(t.source_name) + "}";
            out << "T " << line << "\n";
        }
        if (!out) return false;
    }
    fs::rename(tmp, file, ec);
    return !ec;
}

std::optional<std::vector<TextureInfo>> load_scan(const fs::path& file, const std::vector<fs::path>& archives) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::string line;
    if (!std::getline(in, line) || line != kScanMagic) return std::nullopt;
    std::size_t a = 0;
    std::vector<TextureInfo> out;
    while (std::getline(in, line)) {
        if (line.starts_with("A ")) {
            if (a >= archives.size() || line.substr(2) != archive_stamp(archives[a])) return std::nullopt;
            ++a;
        } else if (line.starts_with("T ")) {
            if (a != archives.size()) return std::nullopt;
            auto obj = parse_flat_json(std::string_view(line).substr(2));
            auto t = parse_manifest_line(std::string_view(line).substr(2));
            if (!obj || !t) return std::nullopt;
            auto src = obj->find("src");
            auto idx = src == obj->end() ? std::nullopt : parse_number<std::size_t>(src->second.text);
            if (!idx || *idx >= archives.size() || t->source_name.empty()) return std::nullopt;
            t->source_archive = archives[*idx];
            out.push_back(std::move(*t));
        } else if (!line.empty()) {
            return std::nullopt;
        }
    }
    if (a != archives.size()) return std::nullopt;
    return out;
}

} // namespace hailstone::texpack
