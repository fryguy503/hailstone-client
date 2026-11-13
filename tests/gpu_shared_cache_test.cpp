#include <doctest/doctest.h>

#include "core/byte_io.h"
#include "render/gpu_shared_cache.h"

using namespace hailstone;
using namespace hailstone::render;
namespace {
assets::Image texture() {
    assets::Image image; image.width = image.height = 2; image.mips.emplace_back(16, std::byte{42}); return image;
}
}

TEST_CASE("GPU shared cache: final texture recipe round trips and hashes all preparation data") {
    auto image = texture(); const auto original = gpu_cache_texture_key(image, true);
    const auto wire = encode_gpu_cache_texture(image, true); const auto decoded = decode_gpu_cache_texture(wire);
    CHECK(decoded.generate_mips); CHECK(decoded.image.width == image.width); CHECK(decoded.image.height == image.height);
    CHECK(decoded.image.format == image.format); CHECK(decoded.image.mips == image.mips);
    CHECK(gpu_cache_texture_key(decoded.image, decoded.generate_mips) == original);
    CHECK(gpu_cache_texture_key(image, false) != original);
    image.mips.front().back() = std::byte{43}; CHECK(gpu_cache_texture_key(image, true) != original);
    image = texture(); image.width = 1; image.height = 4; CHECK(gpu_cache_texture_key(image, true) != original);
}
TEST_CASE("GPU shared cache: byte parser rejects truncation trailing bytes and forged large mip lengths") {
    const auto encoded = encode_gpu_cache_texture(texture(), true);
    for (std::size_t length = 0; length < encoded.size(); ++length)
        CHECK_THROWS(decode_gpu_cache_texture(std::span(encoded).first(length)));
    auto extra = encoded; extra.push_back(std::byte{0}); CHECK_THROWS(decode_gpu_cache_texture(extra));
    auto bad = encoded; bad[0] ^= std::byte{1}; CHECK_THROWS(decode_gpu_cache_texture(bad));
    bad = encoded; for (std::size_t i = 28; i < 36; ++i) bad[i] = std::byte{0xff}; CHECK_THROWS(decode_gpu_cache_texture(bad));
    bad = encoded; bad[20] = std::byte{2}; CHECK_THROWS(decode_gpu_cache_texture(bad));
}
TEST_CASE("GPU shared cache: texture geometry and format bounds checked before Vulkan") {
    auto image = texture(); CHECK(validate_gpu_cache_texture(image, true));
    image.width = 0; CHECK_FALSE(validate_gpu_cache_texture(image, true));
    image = texture(); image.height = 16385; CHECK_FALSE(validate_gpu_cache_texture(image, false));
    image = texture(); image.format = static_cast<assets::PixelFormat>(255); CHECK_FALSE(validate_gpu_cache_texture(image, false));
    image = texture(); image.mips.front().pop_back(); CHECK_FALSE(validate_gpu_cache_texture(image, false));
    image = texture(); image.mips.push_back(std::vector<std::byte>(4)); CHECK_FALSE(validate_gpu_cache_texture(image, true));
    CHECK(validate_gpu_cache_texture(image, false));
}
TEST_CASE("GPU shared cache: valid compressed tail mips can have equal block sizes") {
    assets::Image image; image.width = image.height = 4; image.format = assets::PixelFormat::BC1;
    image.mips = {std::vector<std::byte>(8), std::vector<std::byte>(8), std::vector<std::byte>(8)};
    CHECK(validate_gpu_cache_texture(image, false)); CHECK_FALSE(validate_gpu_cache_texture(image, true));
    CHECK(decode_gpu_cache_texture(encode_gpu_cache_texture(image, false)).image.mips == image.mips);
    image.mips.push_back(std::vector<std::byte>(8)); CHECK_FALSE(validate_gpu_cache_texture(image, false));
}
TEST_CASE("GPU shared cache: one GPU owner at a time and serials never silently reuse") {
    GpuCacheLeaseState state; state.register_peer(1); state.register_peer(2);
    CHECK_FALSE(state.acquire(3)); CHECK(state.peer_count() == 2);
    const auto first = state.acquire(1); REQUIRE(first); CHECK(state.owner() == 1); CHECK_FALSE(state.acquire(2));
    CHECK(state.release(1, *first)); CHECK(state.owner() == 0);
    const auto next = state.acquire(2); REQUIRE(next); CHECK(*next > *first); CHECK(state.release(2, *next)); CHECK_FALSE(state.poisoned());
}
TEST_CASE("GPU shared cache: disconnect with GPU lease poisons epoch rather than steals ownership") {
    GpuCacheLeaseState state; state.register_peer(1); state.register_peer(2); REQUIRE(state.acquire(1));
    state.disconnect(1); CHECK(state.poisoned()); CHECK(state.owner() == 1); CHECK_FALSE(state.acquire(2));
    CHECK_FALSE(state.release(1, state.serial()));
}
TEST_CASE("GPU shared cache: idle disconnect is safe but wrong release or duplicate acquisition poisons") {
    GpuCacheLeaseState state; state.register_peer(1); state.register_peer(2); state.disconnect(2);
    CHECK_FALSE(state.poisoned()); CHECK(state.peer_count() == 1); REQUIRE(state.acquire(1));
    CHECK_FALSE(state.release(2, state.serial())); CHECK(state.poisoned());
    GpuCacheLeaseState duplicate; duplicate.register_peer(1); REQUIRE(duplicate.acquire(1));
    CHECK_FALSE(duplicate.acquire(1)); CHECK(duplicate.poisoned());
}
TEST_CASE("GPU shared cache: duplicate peer and zero identity cannot register") {
    GpuCacheLeaseState state; CHECK_THROWS(state.register_peer(0)); state.register_peer(9);
    CHECK(state.has_peer(9)); CHECK_THROWS(state.register_peer(9)); state.disconnect(9);
    CHECK_FALSE(state.has_peer(9)); CHECK_FALSE(state.acquire(9));
}
