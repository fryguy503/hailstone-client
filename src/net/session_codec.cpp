#include "net/session_codec.h"

#include <algorithm>

#include <zlib.h>

#include "core/byte_io.h"
#include "core/log.h"

// Formats follow EQEmu's common/net/daybreak_connection.cpp (read as a spec,
// not copied) and the original client's UdpLibrary (docs/protocol/session.md).

namespace hailstone::net::soe {

namespace {

constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
constexpr auto kCrcTable = make_crc_table();

std::uint8_t u8(std::byte b) { return std::to_integer<std::uint8_t>(b); }

std::uint32_t load_le32(const std::byte* p) {
    return static_cast<std::uint32_t>(u8(p[0])) | static_cast<std::uint32_t>(u8(p[1])) << 8 |
           static_cast<std::uint32_t>(u8(p[2])) << 16 | static_cast<std::uint32_t>(u8(p[3])) << 24;
}

void store_le32(std::byte* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xff);
}

// Encode passes leave the protocol header alone: two bytes for protocol
// packets, one (the opcode's low byte) for bare application packets.
std::size_t header_size(ByteSpan packet) {
    if (packet.empty()) return 0;
    return u8(packet[0]) == 0 ? std::min<std::size_t>(2, packet.size()) : 1;
}

} // namespace

std::uint32_t crc32(ByteSpan data, std::uint32_t key) {
    std::uint32_t crc = 0xffffffffu;
    for (int i = 0; i < 4; ++i) crc = (crc >> 8) ^ kCrcTable[(crc ^ (key >> (8 * i))) & 0xff];
    for (std::byte b : data) crc = (crc >> 8) ^ kCrcTable[(crc ^ u8(b)) & 0xff];
    return ~crc;
}

// Each whole dword is XORed with the previous ciphertext dword (the key for
// the first); the tail bytes with the low byte of the last ciphertext dword.
void xor_encode(std::span<std::byte> data, std::uint32_t key) {
    std::size_t i = 0;
    for (; i + 4 <= data.size(); i += 4) {
        key ^= load_le32(&data[i]);
        store_le32(&data[i], key);
    }
    for (; i < data.size(); ++i) data[i] ^= static_cast<std::byte>(key & 0xff);
}

void xor_decode(std::span<std::byte> data, std::uint32_t key) {
    std::size_t i = 0;
    for (; i + 4 <= data.size(); i += 4) {
        std::uint32_t cipher = load_le32(&data[i]);
        store_le32(&data[i], cipher ^ key);
        key = cipher;
    }
    for (; i < data.size(); ++i) data[i] ^= static_cast<std::byte>(key & 0xff);
}

Bytes compress_body(ByteSpan body) {
    Bytes out;
    if (body.size() > 30) {
        // Level 6, as the original client, with compress2's settings (deflateInit: zlib format, 15-bit window,
        // memory level 8, default strategy), so the bytes are the same. Every compressed datagram passes here, so
        // the deflater is kept per thread and reused (deflateReset) instead of being set up and freed each time.
        struct Deflater {
            z_stream z{};
            bool ok = deflateInit(&z, 6) == Z_OK;
            ~Deflater() {
                if (ok) deflateEnd(&z);
            }
        };
        thread_local Deflater def;
        if (def.ok && deflateReset(&def.z) == Z_OK) {
            const uLong bound = deflateBound(&def.z, static_cast<uLong>(body.size()));
            out.resize(1 + bound);
            z_stream& z = def.z;
            z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(body.data()));
            z.avail_in = static_cast<uInt>(body.size());
            z.next_out = reinterpret_cast<Bytef*>(out.data() + 1);
            z.avail_out = static_cast<uInt>(bound);
            const std::size_t len = deflate(&z, Z_FINISH) == Z_STREAM_END ? static_cast<std::size_t>(z.total_out) : 0;
            if (len != 0 && len < body.size()) {
                out[0] = std::byte{0x5a};
                out.resize(1 + len);
                return out;
            }
            out.clear();
        }
    }
    out.assign(1, std::byte{0xa5});
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

namespace {

// decompress_body's work, appending the plain bytes to `out` (decode_datagram builds its result in one buffer).
bool decompress_append(ByteSpan body, std::size_t max_size, Bytes& out) {
    if (body.empty()) return true;
    std::uint8_t flag = u8(body[0]);
    if (flag == 0xa5) {
        if (body.size() - 1 > max_size) return false;
        out.insert(out.end(), body.begin() + 1, body.end());
        return true;
    }
    if (flag != 0x5a) return false;

    // Every compressed datagram passes here, so the inflater and its output room are kept per thread and reused
    // (inflateReset) instead of being set up, zero-filled and freed each time; the result is copied out at its size.
    // The room is max_size + 1 so that output past the cap is seen and refused.
    struct Inflater {
        z_stream z{};
        bool ok = inflateInit(&z) == Z_OK;
        Bytes room;
        ~Inflater() {
            if (ok) inflateEnd(&z);
        }
    };
    thread_local Inflater inf;
    if (!inf.ok || inflateReset(&inf.z) != Z_OK) return false;
    if (inf.room.size() < max_size + 1) inf.room.resize(max_size + 1);
    z_stream& z = inf.z;
    z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(body.data() + 1));
    z.avail_in = static_cast<uInt>(body.size() - 1);
    z.next_out = reinterpret_cast<Bytef*>(inf.room.data());
    z.avail_out = static_cast<uInt>(max_size + 1);
    int rc = inflate(&z, Z_FINISH);
    std::size_t produced = z.total_out;
    if (rc != Z_STREAM_END || produced > max_size) return false;
    out.insert(out.end(), inf.room.begin(), inf.room.begin() + static_cast<std::ptrdiff_t>(produced));
    return true;
}

} // namespace

std::optional<Bytes> decompress_body(ByteSpan body, std::size_t max_size) {
    Bytes out;
    if (!decompress_append(body, max_size, out)) return std::nullopt;
    return out;
}

bool is_encodable(ByteSpan packet) {
    if (packet.size() < 2) return false;
    if (u8(packet[0]) != 0) return true;
    std::uint8_t opcode = u8(packet[1]);
    return opcode != op::SessionRequest && opcode != op::SessionResponse && opcode != op::OutOfSession;
}

Bytes encode_datagram(const SessionParams& params, ByteSpan packet) {
    Bytes out(packet.begin(), packet.end());
    if (!is_encodable(packet)) return out;
    std::size_t head = header_size(packet);
    for (EncodePass pass : params.passes) {
        if (pass == EncodePass::Compression) {
            Bytes body = compress_body(ByteSpan(out).subspan(head));
            out.resize(head);
            out.insert(out.end(), body.begin(), body.end());
        } else if (pass == EncodePass::Xor) {
            xor_encode(std::span(out).subspan(head), params.encode_key);
        }
    }
    std::uint32_t crc = crc32(out, params.encode_key);
    if (params.crc_bytes == 2) {
        out.push_back(static_cast<std::byte>((crc >> 8) & 0xff));
        out.push_back(static_cast<std::byte>(crc & 0xff));
    } else if (params.crc_bytes == 4) {
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::byte>((crc >> (8 * i)) & 0xff));
    }
    return out;
}

std::optional<Bytes> decode_datagram(const SessionParams& params, ByteSpan datagram) {
    if (!is_encodable(datagram)) return Bytes(datagram.begin(), datagram.end());
    std::size_t crc_bytes = params.crc_bytes;
    if (crc_bytes != 0 && crc_bytes != 2 && crc_bytes != 4) return std::nullopt;
    if (datagram.size() < crc_bytes + 2) return std::nullopt;
    ByteSpan body = datagram.first(datagram.size() - crc_bytes);
    if (crc_bytes) {
        std::uint32_t want = 0;
        for (std::size_t i = 0; i < crc_bytes; ++i) want = (want << 8) | u8(datagram[body.size() + i]);
        std::uint32_t got = crc32(body, params.encode_key);
        if (crc_bytes == 2) got &= 0xffff;
        if (got != want) return std::nullopt;
    }
    // Built in one buffer: the XOR pass works in place on a copy of the datagram, and the compression pass inflates
    // from whatever holds the bytes now (the datagram itself when it runs first) into a new buffer behind the header.
    std::size_t head = header_size(body);
    Bytes out;
    bool owned = false; // out holds the bytes so far; else they are still `body`
    for (int i = 1; i >= 0; --i) {
        EncodePass pass = params.passes[static_cast<std::size_t>(i)];
        if (pass == EncodePass::Compression) {
            ByteSpan from = owned ? ByteSpan(out) : body;
            Bytes plain;
            plain.assign(from.begin(), from.begin() + static_cast<std::ptrdiff_t>(head));
            if (!decompress_append(from.subspan(head), kMaxDecodedSize, plain)) return std::nullopt;
            out = std::move(plain);
            owned = true;
        } else if (pass == EncodePass::Xor) {
            if (!owned) out.assign(body.begin(), body.end());
            owned = true;
            xor_decode(std::span(out).subspan(head), params.encode_key);
        }
    }
    if (!owned) out.assign(body.begin(), body.end());
    return out;
}

Bytes make_session_request(std::uint32_t connect_code, std::uint32_t max_packet_size) {
    ByteWriter w;
    w.le<std::uint8_t>(0);
    w.le<std::uint8_t>(op::SessionRequest);
    w.be<std::uint32_t>(kProtocolVersion);
    w.be<std::uint32_t>(connect_code);
    w.be<std::uint32_t>(max_packet_size);
    return w.take();
}

std::optional<SessionResponse> parse_session_response(ByteSpan packet) {
    if (packet.size() < 17 || u8(packet[0]) != 0 || u8(packet[1]) != op::SessionResponse) return std::nullopt;
    ByteReader r(packet);
    r.skip(2);
    SessionResponse s;
    s.connect_code = r.be<std::uint32_t>();
    s.params.encode_key = r.be<std::uint32_t>();
    s.params.crc_bytes = r.le<std::uint8_t>();
    s.params.passes[0] = static_cast<EncodePass>(r.le<std::uint8_t>());
    s.params.passes[1] = static_cast<EncodePass>(r.le<std::uint8_t>());
    s.params.max_packet_size = r.be<std::uint32_t>();
    return s;
}

Bytes make_disconnect(std::uint32_t connect_code) {
    ByteWriter w;
    w.le<std::uint8_t>(0);
    w.le<std::uint8_t>(op::SessionDisconnect);
    w.be<std::uint32_t>(connect_code);
    return w.take();
}

Bytes make_keepalive() {
    ByteWriter w;
    w.le<std::uint8_t>(0);
    w.le<std::uint8_t>(op::KeepAlive);
    return w.take();
}

Bytes make_ack(std::uint8_t opcode, std::uint16_t sequence) {
    ByteWriter w;
    w.le<std::uint8_t>(0);
    w.le<std::uint8_t>(opcode);
    w.be<std::uint16_t>(sequence);
    return w.take();
}

std::optional<std::vector<ByteSpan>> split_combined(ByteSpan packet) {
    std::vector<ByteSpan> parts;
    std::size_t pos = 2;
    while (pos < packet.size()) {
        std::size_t len = u8(packet[pos++]);
        if (len > packet.size() - pos) return std::nullopt;
        parts.push_back(packet.subspan(pos, len));
        pos += len;
    }
    return parts;
}

std::optional<std::vector<ByteSpan>> split_app_combined(ByteSpan packet) {
    std::vector<ByteSpan> parts;
    std::size_t pos = 2;
    auto at = [&](std::size_t i) -> std::size_t { return u8(packet[pos + i]); };
    while (pos < packet.size()) {
        std::size_t left = packet.size() - pos;
        std::size_t len = 0;
        if (at(0) != 0xff) {
            len = at(0);
            pos += 1;
        } else if (left >= 3 && at(1) == 0xff && at(2) == 0xff) {
            if (left < 7) return std::nullopt;
            len = at(3) << 24 | at(4) << 16 | at(5) << 8 | at(6);
            pos += 7;
        } else {
            if (left < 3) return std::nullopt;
            len = at(1) << 8 | at(2);
            pos += 3;
        }
        if (len > packet.size() - pos) return std::nullopt;
        parts.push_back(packet.subspan(pos, len));
        pos += len;
    }
    return parts;
}

Bytes make_combined(std::span<const Bytes> parts) {
    Bytes out{std::byte{0}, std::byte{op::Combined}};
    for (const auto& p : parts) {
        out.push_back(static_cast<std::byte>(p.size()));
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

Bytes encode_app(const AppPacket& packet, int opcode_bytes) {
    ByteWriter w;
    if (opcode_bytes == 1) {
        w.le<std::uint8_t>(static_cast<std::uint8_t>(packet.opcode));
        w.bytes(packet.data);
        return w.take();
    }
    if ((packet.opcode & 0xff) == 0) w.le<std::uint8_t>(0);
    w.le<std::uint16_t>(packet.opcode);
    w.bytes(packet.data);
    return w.take();
}

std::optional<AppPacket> decode_app(ByteSpan wire, int opcode_bytes) {
    if (opcode_bytes == 1) {
        if (wire.empty()) return std::nullopt;
        AppPacket p;
        p.opcode = u8(wire[0]);
        p.data.assign(wire.begin() + 1, wire.end());
        return p;
    }
    if (wire.size() < 2) return std::nullopt;
    if (u8(wire[0]) == 0) {
        // 00 00 hi: the escaped form of an opcode whose low byte is zero.
        if (wire.size() < 3 || u8(wire[1]) != 0) return std::nullopt;
        wire = wire.subspan(1);
    }
    ByteReader r(wire);
    AppPacket p;
    p.opcode = r.le<std::uint16_t>();
    auto rest = r.bytes(r.remaining());
    p.data.assign(rest.begin(), rest.end());
    return p;
}

std::optional<AppPacket> decode_app(Bytes&& wire, int opcode_bytes) {
    std::size_t head = 0;
    AppPacket p;
    if (opcode_bytes == 1) {
        if (wire.empty()) return std::nullopt;
        p.opcode = u8(wire[0]);
        head = 1;
    } else {
        if (wire.size() < 2) return std::nullopt;
        if (u8(wire[0]) == 0) {
            // 00 00 hi: the escaped form of an opcode whose low byte is zero.
            if (wire.size() < 3 || u8(wire[1]) != 0) return std::nullopt;
            head = 1;
        }
        ByteReader r(ByteSpan(wire).subspan(head));
        p.opcode = r.le<std::uint16_t>();
        head += 2;
    }
    wire.erase(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(head));
    p.data = std::move(wire);
    return p;
}

std::vector<Bytes> make_reliable(ByteSpan payload, std::uint16_t& next_sequence, const SessionParams& params) {
    std::vector<Bytes> out;
    std::size_t budget = params.max_packet_size > params.overhead() + 16 ? params.max_packet_size - params.overhead() : 16;
    if (payload.size() + 4 <= budget) {
        ByteWriter w;
        w.le<std::uint8_t>(0);
        w.le<std::uint8_t>(op::Packet);
        w.be<std::uint16_t>(next_sequence++);
        w.bytes(payload);
        out.push_back(w.take());
        return out;
    }
    std::size_t used = 0;
    bool first = true;
    while (used < payload.size()) {
        ByteWriter w;
        w.le<std::uint8_t>(0);
        w.le<std::uint8_t>(op::Fragment);
        w.be<std::uint16_t>(next_sequence++);
        std::size_t room = budget - 4;
        if (first) {
            w.be<std::uint32_t>(static_cast<std::uint32_t>(payload.size()));
            room -= 4;
            first = false;
        }
        std::size_t n = std::min(room, payload.size() - used);
        w.bytes(payload.subspan(used, n));
        used += n;
        out.push_back(w.take());
    }
    return out;
}

SeqOrder compare_sequence(std::uint16_t expected, std::uint16_t actual) {
    auto diff = static_cast<std::int16_t>(static_cast<std::uint16_t>(actual - expected));
    if (diff == 0) return SeqOrder::Current;
    return diff > 0 ? SeqOrder::Future : SeqOrder::Past;
}

bool sequence_at_or_before(std::uint16_t a, std::uint16_t b) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(b - a)) >= 0;
}

SeqOrder InboundStream::push(ByteSpan packet, std::vector<Bytes>& out) {
    if (packet.size() < 4) return SeqOrder::Past;
    std::uint16_t seq = static_cast<std::uint16_t>(u8(packet[2]) << 8 | u8(packet[3]));
    SeqOrder order = compare_sequence(next_, seq);
    if (order == SeqOrder::Future) {
        // Bounded in count and bytes, so a misbehaving peer cannot grow it without limit (kMaxBufferedBytes).
        if (!pending_.contains(seq)) {
            if (pending_.size() >= kMaxBufferedPackets || packet.size() > kMaxBufferedBytes - pending_bytes_) {
                log::debug("session: no room for early packet {} ({} bytes; {} held, {} bytes); it will come again", seq,
                           packet.size(), pending_.size(), pending_bytes_);
                return order;
            }
            pending_.try_emplace(seq, packet.begin(), packet.end());
            pending_bytes_ += packet.size();
        }
        return order;
    }
    if (order == SeqOrder::Past) return order;

    consume(packet, out);
    ++next_;
    for (auto it = pending_.find(next_); it != pending_.end(); it = pending_.find(next_)) {
        Bytes p = std::move(it->second);
        pending_bytes_ -= p.size();
        pending_.erase(it);
        consume(p, out);
        ++next_;
    }
    return order;
}

void InboundStream::reset_fragment() {
    fragment_ = {};
    fragment_total_ = 0;
    skipped_ = 0;
    discarding_ = false;
    in_fragment_ = false;
}

void InboundStream::consume(ByteSpan packet, std::vector<Bytes>& out) {
    if (broken_) return;
    std::uint8_t opcode = u8(packet[1]);
    if (opcode != op::Fragment) {
        out.emplace_back(packet.begin() + 4, packet.end());
        return;
    }
    ByteSpan body = packet.subspan(4);
    if (!in_fragment_) {
        if (body.size() < 4) {
            // Without its total length the payload's end is unknown: reading the next piece as a first one would
            // reassemble garbage, and skipping pieces until the next whole packet would silently lose every
            // fragmented payload in between (a zone-in burst), leaving the game state out of step with the server.
            // So the stream stops here and the session ends (Session checks broken()). A stock server never sends
            // this (EQEmu always writes the 4-byte total); EQEmu's own peer would misread the next piece.
            log::warn("session: a first fragment of {} bytes has no total length; the stream cannot continue",
                      body.size());
            reset_fragment();
            broken_ = true;
            return;
        }
        fragment_total_ = static_cast<std::uint32_t>(u8(body[0])) << 24 | static_cast<std::uint32_t>(u8(body[1])) << 16 |
                          static_cast<std::uint32_t>(u8(body[2])) << 8 | u8(body[3]);
        body = body.subspan(4);
        fragment_.clear();
        skipped_ = 0;
        discarding_ = fragment_total_ > kMaxFragmentedPayload;
        if (discarding_)
            log::warn("session: dropping a fragmented packet that claims {} bytes (at most {})", fragment_total_,
                      kMaxFragmentedPayload);
        else
            fragment_.reserve(std::min<std::size_t>(fragment_total_, 1u << 20));
        in_fragment_ = true;
    }
    // A piece that runs past the declared total (a first one longer than its own total, say) leaves the payload's
    // end unknown just as a missing total does: cutting it there and reading the next piece as a new first one would
    // reassemble garbage. A stock server never sends one (EQEmu's pieces add up to the total exactly).
    const std::uint64_t had = discarding_ ? skipped_ : fragment_.size();
    if (body.size() > fragment_total_ - had) {
        log::warn("session: a fragment runs {} bytes past its payload's total of {}; the stream cannot continue",
                  body.size() - (fragment_total_ - had), fragment_total_);
        reset_fragment();
        broken_ = true;
        return;
    }
    if (discarding_) {
        // Its pieces are counted, not kept, so the stream stays in step for the payload after it.
        skipped_ += body.size();
        if (skipped_ < fragment_total_) return;
    } else {
        fragment_.insert(fragment_.end(), body.begin(), body.end());
        if (fragment_.size() < fragment_total_) return;
        out.push_back(std::move(fragment_));
    }
    reset_fragment();
}

} // namespace hailstone::net::soe
