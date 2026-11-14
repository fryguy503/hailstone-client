#include <doctest/doctest.h>

#include <algorithm>

#include "core/byte_io.h"
#include "render/gpu_cache_protocol.h"

using namespace hailstone;
using namespace hailstone::render::gpu_cache;

namespace {

assets::Image texture() {
    assets::Image image;
    image.width = image.height = 2;
    image.mips.emplace_back(16, std::byte{42});
    return image;
}

Identity identity() {
    Identity out;
    out.model = "Test GPU";
    out.api = 1;
    out.driver = 2;
    out.memory[0] = 7;
    return out;
}

} // namespace

TEST_CASE("GPU texture recipe: round trips and hashes all preparation data") {
    auto image = texture();
    const auto original = texture_key(image, true);
    const auto decoded = decode_texture(encode_texture(image, true));
    CHECK(decoded.generate_mips);
    CHECK(decoded.image.width == image.width);
    CHECK(decoded.image.height == image.height);
    CHECK(decoded.image.format == image.format);
    CHECK(decoded.image.mips == image.mips);
    CHECK(texture_key(decoded.image, decoded.generate_mips) == original);
    CHECK(texture_key(image, false) != original);
    image.mips.front().back() = std::byte{43};
    CHECK(texture_key(image, true) != original);
    image = texture();
    image.width = 1;
    image.height = 4;
    CHECK(texture_key(image, true) != original);
}

TEST_CASE("GPU texture recipe: truncation, trailing bytes and forged mip lengths are rejected") {
    const auto encoded = encode_texture(texture(), true);
    for (std::size_t length = 0; length < encoded.size(); ++length)
        CHECK_THROWS(decode_texture(std::span(encoded).first(length)));
    auto extra = encoded;
    extra.push_back(std::byte{0});
    CHECK_THROWS(decode_texture(extra));
    auto bad = encoded;
    bad[0] ^= std::byte{1};
    CHECK_THROWS(decode_texture(bad));
    bad = encoded;
    for (std::size_t i = 28; i < 36; ++i) bad[i] = std::byte{0xff};
    CHECK_THROWS(decode_texture(bad));
    bad = encoded;
    bad[20] = std::byte{2};
    CHECK_THROWS(decode_texture(bad));
}

TEST_CASE("GPU texture recipe: geometry and format bounds are checked before Vulkan") {
    auto image = texture();
    CHECK(validate_texture(image, true));
    image.width = 0;
    CHECK_FALSE(validate_texture(image, true));
    image = texture();
    image.height = 16385;
    CHECK_FALSE(validate_texture(image, false));
    image = texture();
    image.format = static_cast<assets::PixelFormat>(255);
    CHECK_FALSE(validate_texture(image, false));
    image = texture();
    image.mips.front().pop_back();
    CHECK_FALSE(validate_texture(image, false));
    image = texture();
    image.mips.push_back(std::vector<std::byte>(4));
    CHECK_FALSE(validate_texture(image, true));
    CHECK(validate_texture(image, false));
}

TEST_CASE("GPU texture recipe: compressed tail mips can have equal block sizes") {
    assets::Image image;
    image.width = image.height = 4;
    image.format = assets::PixelFormat::BC1;
    image.mips = {std::vector<std::byte>(8), std::vector<std::byte>(8), std::vector<std::byte>(8)};
    CHECK(validate_texture(image, false));
    CHECK_FALSE(validate_texture(image, true));
    CHECK(decode_texture(encode_texture(image, false)).image.mips == image.mips);
    image.mips.push_back(std::vector<std::byte>(8));
    CHECK_FALSE(validate_texture(image, false));
}

TEST_CASE("GPU broker requests: every operation round trips") {
    const auto recipe = encode_texture(texture(), false);
    Request texture_request;
    texture_request.op = Op::Texture;
    texture_request.digest = texture_key(texture(), false);
    texture_request.recipe = recipe;
    Request hello;
    hello.op = Op::Hello;
    hello.identity = identity();
    Request release;
    release.op = Op::Release;
    release.epoch = 5;
    release.serial = 9;
    Request retire;
    retire.op = Op::RetireTexture;
    retire.epoch = 5;
    retire.digest[3] = 1;
    for (const Request& sent : {texture_request, hello, release, retire}) {
        const auto body = encode_request(sent);
        const auto got = decode_request(body);
        CHECK(got.op == sent.op);
        CHECK(got.identity == sent.identity);
        CHECK(got.epoch == sent.epoch);
        CHECK(got.serial == sent.serial);
        CHECK(got.digest == sent.digest);
        CHECK(std::equal(got.recipe.begin(), got.recipe.end(), sent.recipe.begin(), sent.recipe.end()));
    }
}

TEST_CASE("GPU broker requests: malformed bodies are rejected") {
    Request release;
    release.op = Op::Release;
    release.epoch = 5;
    release.serial = 9;
    const auto body = encode_request(release);
    for (std::size_t n = 0; n < body.size(); ++n) CHECK_THROWS(decode_request(std::span(body).first(n)));
    auto extra = body;
    extra.push_back(std::byte{0});
    CHECK_THROWS(decode_request(extra));
    auto version = body;
    version[0] = std::byte{1};
    CHECK_THROWS(decode_request(version));
    for (std::uint32_t op : {0u, 9u, 0xffffffffu}) {
        ByteWriter out;
        out.le(kProtocolVersion);
        out.le(op);
        CHECK_THROWS(decode_request(out.data()));
    }
    Request empty_texture;
    empty_texture.op = Op::Texture;
    CHECK_THROWS(decode_request(encode_request(empty_texture)));
}

TEST_CASE("GPU broker replies: status and grants are bounded") {
    {
        const auto body = encode_status(Status::Busy);
        ByteReader in(body);
        CHECK(read_status(in) == Status::Busy);
        CHECK(in.empty());
    }
    ByteWriter bad;
    bad.le(kProtocolVersion);
    bad.le<std::uint32_t>(4);
    ByteReader bad_in(bad.data());
    CHECK_THROWS(read_status(bad_in));

    TextureGrant grant;
    grant.epoch = 3;
    grant.allocation_size = 4096;
    grant.memory_index = 1;
    grant.levels = 2;
    grant.hit = true;
    const auto body = encode_texture_grant(grant);
    ByteReader in(body);
    REQUIRE(read_status(in) == Status::Ok);
    const auto got = read_texture_grant(in);
    CHECK(got.allocation_size == 4096);
    CHECK(got.levels == 2);
    CHECK(got.hit);
    grant.allocation_size = 0;
    const auto zero = encode_texture_grant(grant);
    ByteReader zero_in(zero);
    (void)read_status(zero_in);
    CHECK_THROWS(read_texture_grant(zero_in));

    const auto lease = encode_lease_grant({3, 0});
    ByteReader lease_in(lease);
    (void)read_status(lease_in);
    CHECK_THROWS(read_lease_grant(lease_in));
}
