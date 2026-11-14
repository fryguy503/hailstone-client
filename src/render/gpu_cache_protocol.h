#pragma once

// The wire format between a client and the GPU texture broker (tools/gpu_lab, docs/graphics.md "Shared GPU
// textures"): request and reply bodies inside platform/gpu_ipc.h's frames, and the texture recipe a client sends.
// Vulkan-free, so malformed bodies are tested and fuzzed directly. Every decoder throws on malformed input.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "assets/texture.h"
#include "core/byte_io.h"
#include "core/sha256.h"

namespace hailstone::render::gpu_cache {

inline constexpr std::uint32_t kProtocolVersion = 2;
inline constexpr std::uint64_t kMaximumTextureBytes = std::uint64_t{128} << 20;
inline constexpr std::uint64_t kMaximumBudget = std::uint64_t{2} << 30;
inline constexpr std::uint32_t kMaximumEntries = 2048;

enum class Op : std::uint32_t {
    Hello = 1, Texture = 2, Acquire = 3, Release = 4, Status = 5, StopIfIdle = 6, RetireTexture = 7, Ping = 8
};
// Private: the broker will not share this (a texture it cannot or will not hold, a GPU that differs); the client
// uploads it itself and carries on sharing. Busy: the frame lease was not free in time. Invalid: the request was
// malformed or out of turn; only that request fails.
enum class Status : std::uint32_t { Ok = 0, Private = 1, Busy = 2, Invalid = 3 };

// The GPU both sides must have: the same model, Vulkan and driver version and memory layout.
struct Identity {
    std::string model;
    std::uint32_t api = 0, driver = 0;
    Sha256Digest memory{};
    bool operator==(const Identity&) const = default;
};
void write_identity(ByteWriter& out, const Identity& identity);
Identity read_identity(ByteReader& in);

// The final image after colour preparation, mip dropping and format fallback, keyed by its content.
struct TextureRecipe {
    assets::Image image;
    bool generate_mips = false;
};
bool validate_texture(const assets::Image& image, bool generate_mips) noexcept;
Sha256Digest texture_key(const assets::Image& image, bool generate_mips);
std::vector<std::byte> encode_texture(const assets::Image& image, bool generate_mips);
TextureRecipe decode_texture(std::span<const std::byte> packet);

struct Request {
    Op op = Op::Ping;
    Identity identity;                 // Hello
    std::uint64_t epoch = 0;           // Acquire, Release, RetireTexture, Ping
    std::uint64_t serial = 0;          // Release
    Sha256Digest digest{};             // Texture, RetireTexture
    std::span<const std::byte> recipe; // Texture: the encoded recipe, a view into the decoded body
};
std::vector<std::byte> encode_request(const Request& request);
// Checks everything but the recipe's contents (decode_texture).
Request decode_request(std::span<const std::byte> body);

// Replies. Every one starts with the version and a Status; Ok replies carry what follows.
std::vector<std::byte> encode_status(Status status);
Status read_status(ByteReader& in);
struct HelloReply { Identity identity; std::uint64_t epoch = 0; };
std::vector<std::byte> encode_hello_reply(const HelloReply& reply);
HelloReply read_hello_reply(ByteReader& in);
struct TextureGrant {
    std::uint64_t epoch = 0, allocation_size = 0;
    std::uint32_t memory_index = 0, levels = 0;
    bool hit = false;
    Sha256Digest digest{};
};
std::vector<std::byte> encode_texture_grant(const TextureGrant& grant);
TextureGrant read_texture_grant(ByteReader& in);
struct LeaseGrant { std::uint64_t epoch = 0, serial = 0; };
std::vector<std::byte> encode_lease_grant(const LeaseGrant& grant);
LeaseGrant read_lease_grant(ByteReader& in);
struct BrokerStats {
    std::uint64_t epoch = 0;
    std::uint32_t consumers = 0;
    std::uint64_t owner = 0, allocated = 0;
    std::uint32_t entries = 0;
    std::uint64_t requests = 0, hits = 0, grants = 0, releases = 0, failed_requests = 0, validation_errors = 0;
};
std::vector<std::byte> encode_stats(Status status, const BrokerStats& stats);
BrokerStats read_stats(ByteReader& in);

} // namespace hailstone::render::gpu_cache
