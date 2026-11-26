#pragma once

// Minimal PNG writer (tools, screenshots): 8-bit RGBA, no interlacing, filter 0 on
// every row, one IDAT chunk compressed with zlib.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#include <zlib.h>

namespace hailstone {

namespace png_detail {

inline void put_be32(std::vector<unsigned char>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<unsigned char>((v >> shift) & 0xff));
}

inline void put_chunk(std::vector<unsigned char>& out, std::string_view type, std::span<const unsigned char> body) {
    put_be32(out, static_cast<std::uint32_t>(body.size()));
    std::size_t start = out.size();
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), body.begin(), body.end());
    uLong crc = crc32(0L, out.data() + start, static_cast<uInt>(out.size() - start));
    put_be32(out, static_cast<std::uint32_t>(crc));
}

} // namespace png_detail

// Encodes width x height RGBA8 pixels (top row first). Empty on failure.
inline std::vector<unsigned char> encode_png_rgba(std::span<const std::byte> rgba, std::uint32_t width,
                                                  std::uint32_t height) {
    using namespace png_detail;
    std::size_t row = std::size_t(width) * 4;
    if (width == 0 || height == 0 || rgba.size() < row * height) return {};

    std::vector<unsigned char> raw;
    raw.reserve((row + 1) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); // filter type None
        auto src = rgba.subspan(y * row, row);
        for (std::byte b : src) raw.push_back(std::to_integer<unsigned char>(b));
    }
    uLongf packed_size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> packed(packed_size);
    if (compress2(packed.data(), &packed_size, raw.data(), static_cast<uLong>(raw.size()), 9) != Z_OK) return {};
    packed.resize(packed_size);

    std::vector<unsigned char> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<unsigned char> ihdr;
    put_be32(ihdr, width);
    put_be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit, RGBA, deflate, filter 0, no interlace
    put_chunk(out, "IHDR", ihdr);
    put_chunk(out, "IDAT", packed);
    put_chunk(out, "IEND", {});
    return out;
}

inline bool write_png_rgba(const std::filesystem::path& path, std::span<const std::byte> rgba, std::uint32_t width,
                           std::uint32_t height) {
    auto png = encode_png_rgba(rgba, width, height);
    if (png.empty()) return false;
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec); // screenshots/
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    // Closed and checked here: a small image is still in the stream's buffer, and its write can fail at the flush.
    out.close();
    return !out.fail();
}

} // namespace hailstone
