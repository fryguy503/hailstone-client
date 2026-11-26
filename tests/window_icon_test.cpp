// app/window_icon.h: the window's icon, read from icons/hailstone.png; core/png_write.h's write_png_rgba.
#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app/window_icon.h"
#include "core/png_write.h"
#include "scratch_dir.h"

using hailstone::app::read_window_icon;

namespace {
std::vector<std::byte> pixels(std::uint32_t w, std::uint32_t h) {
    std::vector<std::byte> px(std::size_t{w} * h * 4);
    for (std::size_t i = 0; i < px.size(); ++i) px[i] = static_cast<std::byte>(i % 251);
    return px;
}
} // namespace

TEST_CASE("window icon: a PNG reads back as RGBA8 with its pixels") {
    hailstone::test::ScratchDir dir("window_icon");
    const auto px = pixels(32, 16);
    REQUIRE(hailstone::write_png_rgba(dir.path() / "hailstone.png", px, 32, 16));
    std::string why;
    auto img = read_window_icon(dir.path() / "hailstone.png", &why);
    REQUIRE_MESSAGE(img, why);
    CHECK(img->width == 32);
    CHECK(img->height == 16);
    CHECK(img->format == hailstone::assets::PixelFormat::RGBA8);
    CHECK(img->mips[0] == px);
}

TEST_CASE("PNG writing: a write that fails at the last flush is reported") {
#if defined(__linux__)
    // /dev/full takes the open and refuses every write: a 2 x 2 image is still in the stream's buffer until it closes.
    if (std::filesystem::exists("/dev/full")) CHECK_FALSE(hailstone::write_png_rgba("/dev/full", pixels(2, 2), 2, 2));
#endif
    // A folder where the file goes: the open fails.
    hailstone::test::ScratchDir dir("png_write_fail");
    std::filesystem::create_directories(dir.path() / "taken.png");
    CHECK_FALSE(hailstone::write_png_rgba(dir.path() / "taken.png", pixels(2, 2), 2, 2));
}

TEST_CASE("window icon: a missing, broken or oversized file is refused") {
    hailstone::test::ScratchDir dir("window_icon_bad");
    std::string why;
    CHECK_FALSE(read_window_icon(dir.path() / "none.png", &why));
    CHECK(!why.empty());

    std::ofstream(dir.path() / "text.png", std::ios::binary) << "not a png";
    why.clear();
    CHECK_FALSE(read_window_icon(dir.path() / "text.png", &why));
    CHECK(!why.empty());

    const std::uint32_t wide = hailstone::app::kMaxWindowIconSide + 1;
    REQUIRE(hailstone::write_png_rgba(dir.path() / "wide.png", pixels(wide, 1), wide, 1));
    why.clear();
    CHECK_FALSE(read_window_icon(dir.path() / "wide.png", &why));
    CHECK(why == "larger than 1024 pixels");
}

TEST_CASE("window icon: the shipped icon is a square RGBA image with transparent corners") {
    const auto file = std::filesystem::path(HAILSTONE_TEST_DATA_DIR).parent_path().parent_path() / "icons" / "hailstone.png";
    std::string why;
    auto img = read_window_icon(file, &why);
    REQUIRE_MESSAGE(img, why);
    CHECK(img->width == 256);
    CHECK(img->height == 256);
    // The top corners lie outside the crest's frame (the cut-out background).
    CHECK(img->mips[0][3] == std::byte{0});
    CHECK(img->mips[0][(255) * 4 + 3] == std::byte{0});
}
