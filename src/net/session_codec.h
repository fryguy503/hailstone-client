#pragma once

// Pure framing functions of the SOE ("daybreak") session protocol, kept
// separate from the socket and timer logic in session.cpp so they can be
// unit-tested. Wire format: docs/protocol/session.md.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "net/packet.h"

namespace hailstone::net::soe {

using Bytes = std::vector<std::byte>;
using ByteSpan = std::span<const std::byte>;

// Protocol opcodes: the second byte of a packet whose first byte is 0.
namespace op {
constexpr std::uint8_t Padding = 0x00; // 00 00: application packet whose opcode's low byte is 0
constexpr std::uint8_t SessionRequest = 0x01;
constexpr std::uint8_t SessionResponse = 0x02;
constexpr std::uint8_t Combined = 0x03;
constexpr std::uint8_t SessionDisconnect = 0x05;
constexpr std::uint8_t KeepAlive = 0x06;
constexpr std::uint8_t SessionStatRequest = 0x07;
constexpr std::uint8_t SessionStatResponse = 0x08;
constexpr std::uint8_t Packet = 0x09;   // reliable stream 0 (0x0a-0x0c: streams 1-3)
constexpr std::uint8_t Fragment = 0x0d; // stream 0 (0x0e-0x10: streams 1-3)
constexpr std::uint8_t OutOfOrderAck = 0x11;
constexpr std::uint8_t Ack = 0x15;
constexpr std::uint8_t AppCombined = 0x19;
constexpr std::uint8_t OutboundPing = 0x1c;
constexpr std::uint8_t OutOfSession = 0x1d;
} // namespace op

enum class EncodePass : std::uint8_t { None = 0, Compression = 1, Xor = 4 };

constexpr std::uint32_t kProtocolVersion = 3;
// The original client caps a decoded packet at 8192 bytes.
constexpr std::size_t kMaxDecodedSize = 8192;

// What the server chose in its SessionResponse.
struct SessionParams {
    std::uint32_t encode_key = 0;
    std::uint8_t crc_bytes = 0;
    std::array<EncodePass, 2> passes{EncodePass::None, EncodePass::None};
    std::uint32_t max_packet_size = 512;

    bool compressed() const { return passes[0] == EncodePass::Compression || passes[1] == EncodePass::Compression; }
    // Bytes the encoder may add to a packet: the compression flag and the CRC.
    std::size_t overhead() const { return (compressed() ? 1u : 0u) + crc_bytes; }
};

struct SessionResponse {
    std::uint32_t connect_code = 0;
    SessionParams params;
};

// CRC-32 (reflected 0xEDB88320) over the 4 key bytes, little-endian, then the data.
std::uint32_t crc32(ByteSpan data, std::uint32_t key);

// The rolling XOR pass (encode pass 4), applied in place.
void xor_encode(std::span<std::byte> data, std::uint32_t key);
void xor_decode(std::span<std::byte> data, std::uint32_t key);

// Compression pass: 0x5a + zlib stream when that is smaller (bodies over 30
// bytes), otherwise 0xa5 + the bytes unchanged.
Bytes compress_body(ByteSpan body);
// Reverses compress_body; nullopt on an unknown flag, bad zlib data, or a
// result larger than max_size.
std::optional<Bytes> decompress_body(ByteSpan body, std::size_t max_size = kMaxDecodedSize);

// SessionRequest/Response/OutOfSession are sent without encode passes or CRC;
// everything else, SessionDisconnect included, is encoded (EQEmu's
// PacketCanBeEncoded exempts only those three).
bool is_encodable(ByteSpan packet);

// Applies the encode passes and appends the CRC.
Bytes encode_datagram(const SessionParams& params, ByteSpan packet);
// Checks and strips the CRC and reverses the passes; nullopt when the CRC
// does not match or a pass fails.
std::optional<Bytes> decode_datagram(const SessionParams& params, ByteSpan datagram);

Bytes make_session_request(std::uint32_t connect_code, std::uint32_t max_packet_size);
std::optional<SessionResponse> parse_session_response(ByteSpan packet);
Bytes make_disconnect(std::uint32_t connect_code);
Bytes make_keepalive(); // 00 06
Bytes make_ack(std::uint8_t opcode, std::uint16_t sequence);

// Splits 00 03 (one length byte per sub-packet) and 00 19 (length byte, or
// ff + u16 BE, or ff ff ff + u32 BE). nullopt when a length runs past the end.
std::optional<std::vector<ByteSpan>> split_combined(ByteSpan packet);
std::optional<std::vector<ByteSpan>> split_app_combined(ByteSpan packet);
// Builds a 00 03 packet; every part must be at most 255 bytes.
Bytes make_combined(std::span<const Bytes> parts);

// Application packet <-> wire bytes: opcode little-endian, with an extra 0x00
// in front when the opcode's low byte is 0. The chat server (UCS) uses one-byte
// opcodes (EQEmu ucs/clientlist.cpp: opcode_size = 1): opcode_bytes 1.
Bytes encode_app(const AppPacket& packet, int opcode_bytes = 2);
std::optional<AppPacket> decode_app(ByteSpan wire, int opcode_bytes = 2);
// The same from a buffer the caller gives up (a reassembled reliable payload): the opcode is cut off in place, so
// the body is not copied again.
std::optional<AppPacket> decode_app(Bytes&& wire, int opcode_bytes = 2);

// Reliable packets for one application payload: one OP_Packet when it fits
// in max_packet_size (after the encoder's overhead), otherwise OP_Fragment
// pieces, the first of which carries the total length. Consumes sequence
// numbers from next_sequence.
std::vector<Bytes> make_reliable(ByteSpan payload, std::uint16_t& next_sequence, const SessionParams& params);

enum class SeqOrder { Current, Future, Past };
// Half-space comparison of a received sequence with the expected one.
SeqOrder compare_sequence(std::uint16_t expected, std::uint16_t actual);
// True when sequence a is at or before b (16-bit wraparound).
bool sequence_at_or_before(std::uint16_t a, std::uint16_t b);

// The most a fragmented payload may claim to be. The largest RoF2 application packets (the player profile, a zone's
// spawns, item lists, a 32 KiB server-file piece) are well under 1 MiB; 8 MiB leaves a wide margin while keeping a
// first fragment that claims gigabytes from making the stream gather without bound. A larger payload's pieces are
// counted and dropped, not kept.
inline constexpr std::uint32_t kMaxFragmentedPayload = 8u << 20;

// The most early (out-of-order) packets the stream holds, and the most bytes they may take together. A packet from
// a datagram decodes to at most kMaxDecodedSize bytes, but a reassembled payload that is itself an early 00 09 can be
// up to kMaxFragmentedPayload, so the count alone does not bound the memory. A stock server sends at most its
// negotiated packet size (512) a datagram, so 4096 of them are 2 MiB, and EQEmu throttles its resends to an observed
// client window of 300 packets / 140 KiB (common/net/reliable_stream_connection.cpp); 4 MiB is twice the first and
// far above the second. An early packet past either limit is dropped and not acknowledged, so the server sends it
// again; the expected packet is never dropped, so the stream still advances and frees room.
inline constexpr std::size_t kMaxBufferedPackets = 4096;
inline constexpr std::size_t kMaxBufferedBytes = 4u << 20;

// Receiving side of reliable stream 0: orders OP_Packet/OP_Fragment packets,
// buffers early ones and reassembles fragments.
class InboundStream {
public:
    explicit InboundStream(std::uint16_t first_sequence = 0) : next_(first_sequence) {}

    // packet is the whole protocol packet (00 09/00 0d, sequence, body).
    // Complete payloads that are now in order are appended to out. An early packet (Future) is buffered unless that
    // would pass kMaxBufferedPackets or kMaxBufferedBytes; holds() tells which.
    SeqOrder push(ByteSpan packet, std::vector<Bytes>& out);

    // The next sequence expected; next() - 1 is the latest one to ack.
    std::uint16_t next() const { return next_; }
    std::size_t buffered() const { return pending_.size(); }
    std::size_t buffered_bytes() const { return pending_bytes_; }
    // Whether an early packet with this sequence is buffered (and may be acknowledged out of order).
    bool holds(std::uint16_t sequence) const { return pending_.contains(sequence); }
    // A first fragment too short to carry its total length arrived: where that payload ends is unknown, so the
    // stream cannot be read any further (nothing more is delivered). The session ends on it.
    bool broken() const { return broken_; }

private:
    void consume(ByteSpan packet, std::vector<Bytes>& out);
    void reset_fragment();

    std::uint16_t next_ = 0;
    std::map<std::uint16_t, Bytes> pending_;
    std::size_t pending_bytes_ = 0;
    Bytes fragment_;
    std::uint32_t fragment_total_ = 0;
    std::uint64_t skipped_ = 0; // bytes of a payload over kMaxFragmentedPayload, counted and dropped
    bool discarding_ = false;
    bool in_fragment_ = false;
    bool broken_ = false;
};

} // namespace hailstone::net::soe
