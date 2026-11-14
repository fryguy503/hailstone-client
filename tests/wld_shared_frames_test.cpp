#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "assets/wld.h"
#include "core/byte_io.h"
#include "core/shared_file_cache.h"
#include "scratch_dir.h"
#include "wld_fixtures.h"

using namespace hailstone;
using namespace hailstone::assets;

static_assert(std::random_access_iterator<wld::TrackFrames::const_iterator>);

namespace {
std::vector<std::byte> track_wld(bool packed, std::uint32_t declared = 2, std::size_t written = 2) {
    test::wld_fixtures::WldBuilder builder;
    ByteWriter body;
    body.le<std::uint32_t>(packed ? 8 : 0);
    body.le(declared);
    for (std::size_t i = 0; i < written; ++i) {
        if (packed) {
            for (const auto n : std::array<std::int16_t, 7>{32767, -32768, 4096, -8192, -32768, 32767,
                                                          static_cast<std::int16_t>(i == 0 ? -1234 : 5678)})
                body.le(n);
            body.le<std::uint16_t>(i == 0 ? 65535 : 0); // scale is unsigned, including values greater than 1
        } else {
            body.f32(i == 0 ? -0.0f : 1.25f);
            for (float f : {-123.5f, 0.25f, 4096.0f}) body.f32(f);
            for (float f : {0.5f, -0.25f, 0.75f, i == 0 ? 1.0f : std::bit_cast<float>(0x7fc12345u)}) body.f32(f);
        }
    }
    builder.add(0x12, builder.str("AUDIT_TRACKDEF"), body);
    return builder.build();
}

const wld::TrackDef& track(const wld::WldFile& file) {
    const auto* value = file.get<wld::TrackDef>(1);
    REQUIRE(value);
    return *value;
}

void same_frame(const wld::TrackFrame& a, const wld::TrackFrame& b) {
    for (int i = 0; i < 4; ++i)
        CHECK(std::bit_cast<std::uint32_t>(a.rotation[i]) == std::bit_cast<std::uint32_t>(b.rotation[i]));
    for (int i = 0; i < 3; ++i)
        CHECK(std::bit_cast<std::uint32_t>(a.translation[i]) == std::bit_cast<std::uint32_t>(b.translation[i]));
    CHECK(std::bit_cast<std::uint32_t>(a.scale) == std::bit_cast<std::uint32_t>(b.scale));
}
} // namespace

TEST_CASE("shared WLD tracks: packed and unpacked immutable snapshots match decoded keys bit for bit") {
    test::ScratchDir dir("wld-shared-frames");
    SharedFileCache cache({std::filesystem::canonical(dir.path()) / "cache"});
    for (bool packed : {true, false}) {
        CAPTURE(packed);
        const auto bytes = track_wld(packed);
        const auto decoded = wld::WldFile::parse(bytes);
        auto owner = cache.store(bytes);
        REQUIRE(owner.mapped());
        const auto viewed = wld::WldFile::parse(std::move(owner));
        REQUIRE(viewed.errors().empty());
        const auto& want = track(decoded).frames;
        const auto& got = track(viewed).frames;
        CHECK_FALSE(want.encoded());
        CHECK(got.encoded());
        CHECK(got.mapped());
        CHECK(got.decoded_capacity() == 0);
        REQUIRE(got.size() == 2);
        for (std::size_t i = 0; i < got.size(); ++i) same_frame(got[i], want[i]);
        same_frame(got.front(), want.front());
        same_frame(got.back(), want.back());
        same_frame(got.at(1), want.at(1));
        CHECK_THROWS_AS(got.at(2), std::out_of_range);
        const std::vector<wld::TrackFrame> iterated(got.begin(), got.end());
        REQUIRE(iterated.size() == want.size());
        for (std::size_t i = 0; i < iterated.size(); ++i) same_frame(iterated[i], want[i]);
        CHECK(got.end() - got.begin() == 2);
        same_frame(*(1 + got.begin()), want[1]);
        same_frame(got.begin()[1], want[1]);
        CHECK(got.decoded_capacity() == 0);
    }
}

TEST_CASE("shared WLD tracks: copied files and fragments retain backing after cache and input destruction") {
    test::ScratchDir dir("wld-shared-lifetime");
    const auto directory = std::filesystem::canonical(dir.path()) / "cache";
    auto cache = std::make_unique<SharedFileCache>(SharedFileCache::Options{directory});
    auto bytes = track_wld(true);
    auto owner = cache->store(bytes);
    REQUIRE(owner.mapped());
    auto original = wld::WldFile::parse(std::move(owner));
    auto copied = original;
    auto fragment = original.fragments().front();
    const auto expected = track(original).frames.front();
    original = {};
    bytes.clear();
    bytes.shrink_to_fit();
    cache.reset();
    SharedFileCache cleanup({directory});
    CHECK_FALSE(cleanup.trim());
    same_frame(track(copied).frames.front(), expected);
    copied = {};
    auto moved = std::move(fragment.as<wld::TrackDef>().frames);
    CHECK(fragment.as<wld::TrackDef>().frames.empty());
    const auto& view = moved;
    CHECK(view.mapped());
    same_frame(view.front(), expected);
    CHECK_FALSE(cleanup.trim());
    moved.clear();
    CHECK(cleanup.trim());
}

TEST_CASE("shared WLD tracks: fixture mutation detaches encoded keys without changing another reader") {
    const auto bytes = track_wld(true);
    const auto file = wld::WldFile::parse(ImmutableFileBytes::owned(bytes));
    const auto& source = track(file).frames;
    REQUIRE(source.encoded());
    auto changed = source;
    CHECK_THROWS_AS(changed.at(changed.size()), std::out_of_range);
    CHECK(changed.encoded());
    CHECK(changed.decoded_capacity() == 0);
    changed[0].scale = 3.5f;
    CHECK_FALSE(changed.encoded());
    CHECK(changed.decoded_capacity() >= 2);
    CHECK(std::as_const(changed)[0].scale == 3.5f);
    CHECK(source[0].scale == 65535.0f / 256.0f);
    changed.begin()->translation.x = 42.0f;
    CHECK(std::as_const(changed).front().translation.x == 42.0f);
    CHECK(source.front().translation.x == -128.0f);
    changed.push_back(source.back());
    changed.emplace_back();
    CHECK(changed.size() == 4);
    changed.resize(5, source.front());
    changed.reserve(10);
    CHECK(changed.capacity() >= 10);
    changed.data()[4].scale = 9.0f;
    CHECK(std::as_const(changed).back().scale == 9.0f);
    changed.assign({source.front(), source.back()});
    CHECK(changed.size() == 2);
    changed = std::vector<wld::TrackFrame>{source.back()};
    CHECK(changed.size() == 1);
    same_frame(std::as_const(changed).front(), source.back());
    changed.clear();
    CHECK(changed.empty());
    CHECK(source.encoded());
    CHECK(source.decoded_capacity() == 0);
}

TEST_CASE("shared WLD tracks: private cache fallback keeps immutable encoded storage alive") {
    test::ScratchDir dir("wld-shared-fallback");
    SharedFileCache disabled({std::filesystem::canonical(dir.path()) / "cache", 0});
    auto input = disabled.store(track_wld(false));
    CHECK_FALSE(input.mapped());
    const auto file = wld::WldFile::parse(std::move(input));
    const auto& frames = track(file).frames;
    CHECK(frames.encoded());
    CHECK_FALSE(frames.mapped());
    CHECK(frames.decoded_capacity() == 0);
    CHECK(frames.size() == 2);
    CHECK(frames.front().translation.x == -123.5f);
    CHECK(std::bit_cast<std::uint32_t>(frames.back().rotation.z) == 0x7fc12345u);
}

TEST_CASE("shared WLD tracks: truncated payloads and impossible views retain parser bounds") {
    for (bool packed : {true, false}) {
        const auto malformed = track_wld(packed, 2, 1);
        const auto owning = wld::WldFile::parse(malformed);
        const auto viewed = wld::WldFile::parse(ImmutableFileBytes::owned(malformed));
        REQUIRE(owning.errors().size() == 1);
        REQUIRE(viewed.errors().size() == 1);
        CHECK(viewed.errors()[0].index == owning.errors()[0].index);
        CHECK(viewed.errors()[0].type == owning.errors()[0].type);
        CHECK(viewed.errors()[0].what == owning.errors()[0].what);
        CHECK(viewed.fragments()[0].holds<std::monostate>());
        auto truncated = track_wld(packed);
        truncated.pop_back();
        CHECK_THROWS(wld::WldFile::parse(truncated));
        CHECK_THROWS(wld::WldFile::parse(ImmutableFileBytes::owned(truncated)));
        const auto empty = wld::WldFile::parse(ImmutableFileBytes::owned(track_wld(packed, 0, 0)));
        CHECK(track(empty).frames.empty());
        CHECK(track(empty).frames.begin() == track(empty).frames.end());
    }
    auto tiny = ImmutableFileBytes::owned(std::vector<std::byte>(16));
    CHECK_THROWS_AS(wld::TrackFrames::from_encoded(tiny, 0, 2, true), std::out_of_range);
    CHECK_THROWS_AS(wld::TrackFrames::from_encoded(tiny, 17, 0, true), std::out_of_range);
    CHECK_THROWS_AS(wld::TrackFrames::from_encoded(tiny, 0, std::numeric_limits<std::size_t>::max(), false),
                    std::out_of_range);
}

TEST_CASE("shared WLD tracks: concurrent const reads keep keys encoded and independent") {
    const auto file = wld::WldFile::parse(ImmutableFileBytes::owned(track_wld(true)));
    const auto& frames = track(file).frames;
    std::array<std::uint64_t, 4> sums{};
    std::vector<std::thread> readers;
    for (std::size_t t = 0; t < sums.size(); ++t)
        readers.emplace_back([&, t] {
            for (int repeat = 0; repeat < 512; ++repeat)
                for (const auto& frame : frames) sums[t] += std::bit_cast<std::uint32_t>(frame.translation.z);
        });
    for (auto& reader : readers) reader.join();
    std::uint64_t expected = 0;
    for (const auto& frame : frames) expected += std::bit_cast<std::uint32_t>(frame.translation.z);
    expected *= 512;
    for (const auto sum : sums) CHECK(sum == expected);
    CHECK(frames.encoded());
    CHECK(frames.decoded_capacity() == 0);
}
