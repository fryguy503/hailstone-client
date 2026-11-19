#pragma once

// Client side of the SOE ("daybreak") UDP session protocol used by the login,
// world and zone servers. Contract shared by the net and app code; the
// implementation is in session.cpp.
//
// Threading: like the original's UdpManagerThread (UdpLibrary in eqgame.exe,
// started by NetMgr_CreateUdpManager; docs/protocol/session.md), a service thread receives,
// acknowledges, resends and flushes every 20 ms, so a long frame (zone
// loading) does not stall the session. The public functions may be called from
// one owner thread; update() services the session as well and is cheap. The
// trace callback runs on either thread, under the session's lock.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "net/packet.h"

namespace hailstone::net {

enum class SessionState : std::uint8_t {
    Connecting,   // SessionRequest sent, waiting for SessionResponse
    Connected,
    Disconnecting,
    Disconnected, // closed by either side, or timed out; see Session::error()
};

struct SessionOptions {
    std::chrono::milliseconds connect_timeout{10000};
    std::chrono::milliseconds request_interval{1000}; // Params 0x21; UCS uses 5000 ms
    // No packet from the server for this long counts as link death. The original's no-data timeout is UdpManager
    // Params 0x12 (manager +0x58 → UdpConnection +0x15c @0x8c4197, checked in UdpConnection_InternalGiveTime
    // @0x8c633c..0x8c6398 against the time since the last receive, +0x1d0: that long or longer ends the link with
    // reason 2, Timeout): NetMgr +0x50 = 180 s for world and zone (NetMgr_Ctor @0x462a8f, NetMgr_CreateUdpManager
    // @0x4620f1), the default here. Zero disables this timer, as in the UCS profile; login keeps its safety limit.
    std::chrono::milliseconds idle_timeout{180000};
    // A KeepAlive (00 06) goes out once nothing has been sent for this long (0: never). The original's is Params 0xe
    // (manager +0x48 → UdpConnection +0x230; InternalGiveTime @0x8c61c4 sends the 2 bytes encoded, at once, when the
    // time since the last datagram sent, +0x1c8, reaches it): 15 s for world and zone, 14.8 s for login, 44 s for
    // the chat server (14.8 s with [Defaults] ChatKeepAlive).
    std::chrono::milliseconds keepalive_interval{15000};
    std::uint32_t max_packet_size = 512;
    // Bytes per application opcode: 2 (login, world, zone) or 1 (the chat server).
    int opcode_bytes = 2;
    // The multi-buffer hold (UdpManager Params 0x15): protocol packets of at
    // most 255 bytes, acks included, are combined into one datagram that is
    // sent once its oldest packet is this old, or at once when it is full.
    // 150 ms for world and zone (NetMgr_CreateUdpManager), 0 for the login
    // server (eqmain.dll's UdpManagers).
    std::chrono::milliseconds hold_time{150};
    // The service thread's sleep between passes (UdpManagerThread, Params
    // +0x88 = 20 ms); 0 runs no thread, update() alone services the session.
    std::chrono::milliseconds service_interval{20};
    // SessionStatRequest interval (Params 0x1a): 30 s for world and zone, 0 (never) for login.
    std::chrono::milliseconds stat_interval{30000};
    // The reliable channel's resend adjust (Params 0x31; T = min(1.25 rtt + adjust, 8000) ms): 300, or 2000
    // for the login server (eqmain's LoginServerAPI::CreateUdpManager).
    std::int32_t resend_delay_adjust = 300;
    // Reliable packets in flight (Params 0x2c): 400, or 100 for the login server.
    std::int32_t window_slots = 400;
    std::int32_t window_bytes = 100000; // Params 0x2b; UCS uses 10000
    std::int32_t reliable_timeout_ms = 180000; // Params 0x13; UCS uses 120000
    // The most the received application packets waiting for receive() may hold (their data plus a packet's own
    // size each). The service thread goes on receiving while the owner is busy, so a stalled owner or a server that
    // floods would otherwise grow the queue without bound. Dropping a reliable packet would leave the game state out
    // of step with the server, so passing the limit ends the session. A zone-in, the largest burst a stock server
    // sends, is a few MiB; 64 MiB is eight of the largest fragmented payloads (soe::kMaxFragmentedPayload). Not the
    // original's (it has no such queue); docs/protocol/session.md.
    std::size_t max_received_bytes = 64u << 20;
    // Tests: the session's clock (connect and idle timeouts, the hold, resends, stat requests) instead of the
    // steady clock, so a timing test does not depend on how promptly a loaded machine schedules it. The service
    // thread still wakes every service_interval of real time.
    std::function<std::chrono::steady_clock::time_point()> clock;
    // Optional packet trace for debugging tools: every protocol packet as
    // sent (before the encode passes and CRC) and as received (after them).
    std::function<void(bool sent, std::span<const std::byte> packet)> trace;
};

// The session's traffic since it connected (the performance window; the same counters as the closing report).
struct SessionStats {
    std::uint64_t datagrams_sent = 0, datagrams_received = 0;
    std::uint64_t bytes_sent = 0, bytes_received = 0; // datagrams on the wire (after the encode passes and CRC)
    std::uint64_t acks_sent = 0, duplicates = 0, early = 0, bad = 0;
    std::uint64_t keepalives_sent = 0;
    std::int64_t timeout_resends = 0, fast_resends = 0;
    std::int32_t rtt_ms = 0;
    std::int32_t queued_packets = 0;  // reliable packets waiting for a slot
    std::int32_t oldest_unacked_ms = 0; // age since its first send; zero when no sent packet remains
    std::int64_t longest_stall_ms = 0; // the longest gap between the owner's update() calls
    // The SessionStatRequest round trips (the server's SessionStatResponse echoes the request's 16-bit time): the
    // last, the average, the lowest and the highest; 0 before the first reply. /netstats (Cmd_netstats) shows them
    // as UdpConnection::GetStats' ping times.
    std::int32_t ping_last = 0, ping_average = 0, ping_low = 0, ping_high = 0;
    std::int32_t ping_count = 0;
    // At the last reply: our datagram counts then, and the server's own as it carried them.
    std::uint64_t sync_our_sent = 0, sync_our_received = 0, sync_their_sent = 0, sync_their_received = 0;
};

class Session {
public:
    static std::unique_ptr<Session> create(SessionOptions options = {});
    virtual ~Session() = default;

    // Starts connecting; host may be a name or an IPv4/IPv6 address.
    virtual void connect(const std::string& host, std::uint16_t port) = 0;
    virtual void update() = 0;

    virtual SessionState state() const = 0;
    virtual std::string error() const = 0;

    // Queues a packet; reliable packets are sequenced, acked and resent,
    // fragmented when larger than one datagram, and may be combined.
    virtual void send(const AppPacket& packet, bool reliable = true) = 0;
    virtual std::optional<AppPacket> receive() = 0;

    virtual void disconnect() = 0;

    virtual SessionStats stats() const { return {}; }

    // A bulk download (net/server_files.h): the small packets out, acks and requests, go at the next service pass
    // instead of waiting out the multi-buffer's hold_time, so the server's pieces are asked for and acknowledged
    // at once. Off: the original's hold.
    virtual void set_no_hold(bool) {}

    // Testing (app/input_script.h's droplink): from now on nothing goes out or comes in, as when the network goes
    // away, so the link dies by the idle timeout as a real link death would.
    virtual void simulate_link_loss() {}
    // Testing: the session's socket is closed under it, so every send fails (as when its handle went bad); the
    // session ends at its next pass.
    virtual void simulate_socket_loss() {}
};

} // namespace hailstone::net
