#include "render/gpu_cache_protocol.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace hailstone::render::gpu_cache {
namespace {

constexpr std::uint32_t kRecipeMagic = 0x31434748;
constexpr std::size_t kModelBytes = 256;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Sha256Digest read_digest(ByteReader& in) {
    Sha256Digest digest{};
    const auto bytes = in.bytes(digest.size());
    std::memcpy(digest.data(), bytes.data(), digest.size());
    return digest;
}

void write_digest(ByteWriter& out, const Sha256Digest& digest) { out.bytes(std::as_bytes(std::span(digest))); }

ByteWriter reply(Status status) {
    ByteWriter out;
    out.le(kProtocolVersion);
    out.le(static_cast<std::uint32_t>(status));
    return out;
}

} // namespace

void write_identity(ByteWriter& out, const Identity& identity) {
    require(!identity.model.empty() && identity.model.size() < kModelBytes, "GPU cache identity is invalid");
    out.fixed_string(identity.model, kModelBytes);
    out.le(identity.api);
    out.le(identity.driver);
    write_digest(out, identity.memory);
}

Identity read_identity(ByteReader& in) {
    Identity identity;
    identity.model = in.fixed_string(kModelBytes);
    identity.api = in.le<std::uint32_t>();
    identity.driver = in.le<std::uint32_t>();
    identity.memory = read_digest(in);
    require(!identity.model.empty() && identity.model.size() < kModelBytes, "GPU cache identity is invalid");
    return identity;
}

bool validate_texture(const assets::Image& image, bool generate_mips) noexcept {
    if (!image.width || !image.height || image.width > 16384 || image.height > 16384 || image.mips.empty() ||
        image.mips.size() > static_cast<std::size_t>(std::bit_width(std::max(image.width, image.height))) ||
        static_cast<unsigned>(image.format) > static_cast<unsigned>(assets::PixelFormat::BC7) ||
        (generate_mips && (image.format != assets::PixelFormat::RGBA8 || image.mips.size() != 1)))
        return false;
    std::uint64_t total = 0;
    auto width = image.width, height = image.height;
    for (const auto& mip : image.mips) {
        const auto expected = assets::mip_size(image.format, width, height);
        if (mip.size() != expected || mip.size() > kMaximumTextureBytes - total) return false;
        total += mip.size();
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    return total > 0;
}

std::vector<std::byte> encode_texture(const assets::Image& image, bool generate_mips) {
    require(validate_texture(image, generate_mips), "GPU cache texture recipe is invalid");
    ByteWriter out;
    out.le(kRecipeMagic);
    out.le(kProtocolVersion);
    out.le(image.width);
    out.le(image.height);
    out.le<std::uint32_t>(static_cast<std::uint32_t>(image.format));
    out.le<std::uint32_t>(generate_mips ? 1 : 0);
    out.le<std::uint32_t>(static_cast<std::uint32_t>(image.mips.size()));
    for (const auto& mip : image.mips) {
        out.le<std::uint64_t>(mip.size());
        out.bytes(mip);
    }
    return out.take();
}

TextureRecipe decode_texture(std::span<const std::byte> packet) {
    require(packet.size() <= kMaximumTextureBytes + 1024, "GPU cache recipe is too large");
    ByteReader in(packet);
    require(in.le<std::uint32_t>() == kRecipeMagic && in.le<std::uint32_t>() == kProtocolVersion,
            "GPU cache recipe version mismatch");
    TextureRecipe out;
    out.image.width = in.le<std::uint32_t>();
    out.image.height = in.le<std::uint32_t>();
    const auto format = in.le<std::uint32_t>(), generate = in.le<std::uint32_t>(), count = in.le<std::uint32_t>();
    require(format <= static_cast<std::uint32_t>(assets::PixelFormat::BC7) && generate <= 1 && count && count <= 15,
            "GPU cache recipe fields are invalid");
    out.image.format = static_cast<assets::PixelFormat>(format);
    out.generate_mips = generate != 0;
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto length = in.le<std::uint64_t>();
        require(length <= kMaximumTextureBytes - total && length <= in.remaining(), "GPU cache mip length is invalid");
        auto bytes = in.bytes(static_cast<std::size_t>(length));
        out.image.mips.emplace_back(bytes.begin(), bytes.end());
        total += length;
    }
    require(in.empty() && validate_texture(out.image, out.generate_mips), "GPU cache texture recipe is invalid");
    return out;
}

Sha256Digest texture_key(const assets::Image& image, bool generate_mips) {
    require(validate_texture(image, generate_mips), "GPU cache texture recipe is invalid");
    ByteWriter header;
    header.le(kRecipeMagic);
    header.le(kProtocolVersion);
    header.le(image.width);
    header.le(image.height);
    header.le<std::uint32_t>(static_cast<std::uint32_t>(image.format));
    header.le<std::uint32_t>(generate_mips ? 1 : 0);
    header.le<std::uint32_t>(static_cast<std::uint32_t>(image.mips.size()));
    Sha256 hash;
    hash.update("Hailstone immutable GPU texture v1");
    hash.update(header.data());
    for (const auto& mip : image.mips) {
        ByteWriter length;
        length.le<std::uint64_t>(mip.size());
        hash.update(length.data());
        hash.update(mip);
    }
    return hash.finish();
}

std::vector<std::byte> encode_request(const Request& request) {
    ByteWriter out;
    out.le(kProtocolVersion);
    out.le(static_cast<std::uint32_t>(request.op));
    switch (request.op) {
    case Op::Hello: write_identity(out, request.identity); break;
    case Op::Texture:
        write_digest(out, request.digest);
        out.bytes(request.recipe);
        break;
    case Op::Acquire:
    case Op::Ping: out.le(request.epoch); break;
    case Op::Release:
        out.le(request.epoch);
        out.le(request.serial);
        break;
    case Op::RetireTexture:
        out.le(request.epoch);
        write_digest(out, request.digest);
        break;
    case Op::Status:
    case Op::StopIfIdle: break;
    default: throw std::invalid_argument("GPU cache operation is invalid");
    }
    return out.take();
}

Request decode_request(std::span<const std::byte> body) {
    ByteReader in(body);
    require(in.le<std::uint32_t>() == kProtocolVersion, "GPU cache request version mismatch");
    Request out;
    const auto op = in.le<std::uint32_t>();
    require(op >= static_cast<std::uint32_t>(Op::Hello) && op <= static_cast<std::uint32_t>(Op::Ping),
            "GPU cache operation is invalid");
    out.op = static_cast<Op>(op);
    switch (out.op) {
    case Op::Hello: out.identity = read_identity(in); break;
    case Op::Texture:
        out.digest = read_digest(in);
        out.recipe = in.bytes(in.remaining());
        require(!out.recipe.empty(), "GPU cache texture request is empty");
        break;
    case Op::Acquire:
    case Op::Ping: out.epoch = in.le<std::uint64_t>(); break;
    case Op::Release:
        out.epoch = in.le<std::uint64_t>();
        out.serial = in.le<std::uint64_t>();
        break;
    case Op::RetireTexture:
        out.epoch = in.le<std::uint64_t>();
        out.digest = read_digest(in);
        break;
    case Op::Status:
    case Op::StopIfIdle: break;
    }
    require(in.empty(), "GPU cache request has trailing bytes");
    return out;
}

std::vector<std::byte> encode_status(Status status) { return reply(status).take(); }

Status read_status(ByteReader& in) {
    require(in.le<std::uint32_t>() == kProtocolVersion, "GPU cache protocol version mismatch");
    const auto status = in.le<std::uint32_t>();
    require(status <= static_cast<std::uint32_t>(Status::Invalid), "GPU cache reply status is invalid");
    return static_cast<Status>(status);
}

std::vector<std::byte> encode_hello_reply(const HelloReply& value) {
    auto out = reply(Status::Ok);
    write_identity(out, value.identity);
    out.le(value.epoch);
    return out.take();
}

HelloReply read_hello_reply(ByteReader& in) {
    HelloReply out;
    out.identity = read_identity(in);
    out.epoch = in.le<std::uint64_t>();
    require(out.epoch && in.empty(), "GPU cache handshake is invalid");
    return out;
}

std::vector<std::byte> encode_texture_grant(const TextureGrant& grant) {
    auto out = reply(Status::Ok);
    out.le(grant.epoch);
    out.le(grant.allocation_size);
    out.le(grant.memory_index);
    out.le(grant.levels);
    out.le<std::uint32_t>(grant.hit ? 1 : 0);
    write_digest(out, grant.digest);
    return out.take();
}

TextureGrant read_texture_grant(ByteReader& in) {
    TextureGrant out;
    out.epoch = in.le<std::uint64_t>();
    out.allocation_size = in.le<std::uint64_t>();
    out.memory_index = in.le<std::uint32_t>();
    out.levels = in.le<std::uint32_t>();
    const auto hit = in.le<std::uint32_t>();
    out.digest = read_digest(in);
    require(hit <= 1 && out.allocation_size && out.allocation_size <= kMaximumBudget && out.levels && out.levels <= 15 &&
                in.empty(),
            "GPU cache texture grant is invalid");
    out.hit = hit != 0;
    return out;
}

std::vector<std::byte> encode_lease_grant(const LeaseGrant& grant) {
    auto out = reply(Status::Ok);
    out.le(grant.epoch);
    out.le(grant.serial);
    return out.take();
}

LeaseGrant read_lease_grant(ByteReader& in) {
    LeaseGrant out;
    out.epoch = in.le<std::uint64_t>();
    out.serial = in.le<std::uint64_t>();
    require(out.serial && in.empty(), "GPU cache frame lease is invalid");
    return out;
}

std::vector<std::byte> encode_stats(Status status, const BrokerStats& stats) {
    auto out = reply(status);
    out.le(stats.epoch);
    out.le(stats.consumers);
    out.le(stats.owner);
    out.le(stats.allocated);
    out.le(stats.entries);
    out.le(stats.requests);
    out.le(stats.hits);
    out.le(stats.grants);
    out.le(stats.releases);
    out.le(stats.failed_requests);
    out.le(stats.validation_errors);
    return out.take();
}

BrokerStats read_stats(ByteReader& in) {
    BrokerStats out;
    out.epoch = in.le<std::uint64_t>();
    out.consumers = in.le<std::uint32_t>();
    out.owner = in.le<std::uint64_t>();
    out.allocated = in.le<std::uint64_t>();
    out.entries = in.le<std::uint32_t>();
    out.requests = in.le<std::uint64_t>();
    out.hits = in.le<std::uint64_t>();
    out.grants = in.le<std::uint64_t>();
    out.releases = in.le<std::uint64_t>();
    out.failed_requests = in.le<std::uint64_t>();
    out.validation_errors = in.le<std::uint64_t>();
    require(in.empty(), "GPU cache status reply is invalid");
    return out;
}

} // namespace hailstone::render::gpu_cache
