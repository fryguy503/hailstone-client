#include "net/session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/byte_io.h"
#include "core/log.h"
#include "net/reliable_channel.h"
#include "net/session_codec.h"
#include "platform/crash_log.h"
#include "platform/udp_socket.h"

// Client side of the SOE session protocol. Wire format and timings:
// docs/protocol/session.md. Timings follow the original client
// where they are known,
// and EQEmu's server defaults (common/net/daybreak_connection.h) otherwise.

namespace hailstone::net {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using soe::Bytes;
using soe::ByteSpan;

// Original client: SessionRequest resend (param 0x21),
// SessionStatRequest interval (0x1a), unacknowledged-reliable timeout
// (0x12/0x13 = 180 s), reliable window (0x2b-0x2d: 100000 bytes, 400 slots),
// drain wait on close. Resends follow the original's reliable channel
// (net/reliable_channel.h): T = min(1.25 rtt + 300, 8000) ms. Param
// 0x15 (150 ms) is the multi-buffer hold time (SessionOptions::hold_time,
// UdpConnection_BufferedSend @0x8c49b0 / FlushMultiBuffer @0x8c4850); the
// service thread is the UdpManagerThread (run loop @0x8cbfb0).
constexpr milliseconds kDrainTimeout{10000};
constexpr int kMaxNesting = 4;
// An owner update() later than this after the previous one is logged as a stall.
constexpr milliseconds kStallReport{1000};

// The manager's 64-bit millisecond clock.
std::int64_t clock_ms(Clock::time_point t) {
    return std::chrono::duration_cast<milliseconds>(t.time_since_epoch()).count() + 1;
}

std::uint8_t u8(std::byte b) { return std::to_integer<std::uint8_t>(b); }

class SessionImpl final : public Session {
public:
    explicit SessionImpl(SessionOptions options) : opt_(std::move(options)) {
        std::random_device rd;
        rng_.seed((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
        init_hooks();
    }

    ~SessionImpl() override {
        stop_thread();
        if (state_ == SessionState::Connected || state_ == SessionState::Disconnecting) {
            flush_multi();
            send_now(soe::make_disconnect(connect_code_));
            report("closed by its owner");
        }
    }

    void connect(const std::string& host, std::uint16_t port) override {
        stop_thread();
        std::lock_guard lock(mutex_);
        reset();
        connect_code_ = static_cast<std::uint32_t>(rng_());
        state_ = SessionState::Connecting;
        auto now = now_();
        started_ = last_recv_ = last_stat_ = now;
        last_update_ = now;
        host_ = host;
        if (auto ep = platform::numeric_endpoint(host, port)) {
            begin(*ep, now);
        } else {
            // A name is looked up on a thread of its own: getaddrinfo can wait out a slow name server for seconds,
            // and connect() is called from the frame (login, world, zone). The session stays Connecting, and the
            // connect timeout counts the lookup too. The thread owns `lookup` alone; a lookup that ends after
            // another connect() or this session's end is not looked at.
            auto lookup = std::make_shared<Lookup>();
            lookup_ = lookup;
            platform::resolve_async(host, port, [lookup](std::vector<platform::Endpoint> endpoints, std::string error) {
                std::lock_guard l(lookup->mutex);
                lookup->endpoints = std::move(endpoints);
                lookup->error = std::move(error);
                lookup->done = true;
            });
            log::debug("session: looking up {}", host);
        }
        if (state_ == SessionState::Disconnected) return;
        if (opt_.service_interval.count() > 0) {
            stop_ = false;
            thread_ = std::thread([this] { service_loop(); });
        }
    }

    void update() override {
        std::lock_guard lock(mutex_);
        auto now = now_();
        if (state_ != SessionState::Disconnected && now - last_update_ > kStallReport) {
            auto gap = std::chrono::duration_cast<milliseconds>(now - last_update_).count();
            log::debug("session {}: no update for {} ms (the service thread kept it going: {} datagrams in, {} out)",
                       peer_.to_string(), gap, datagrams_received_ - stall_received_, datagrams_sent_ - stall_sent_);
            longest_stall_ms_ = std::max<std::int64_t>(longest_stall_ms_, gap);
        }
        last_update_ = now;
        stall_received_ = datagrams_received_;
        stall_sent_ = datagrams_sent_;
        service();
    }

    SessionState state() const override {
        std::lock_guard lock(mutex_);
        return state_;
    }
    std::string error() const override {
        std::lock_guard lock(mutex_);
        return error_;
    }

    void send(const AppPacket& packet, bool reliable) override {
        std::lock_guard lock(mutex_);
        send_locked(packet, reliable);
    }

    std::optional<AppPacket> receive() override {
        std::lock_guard lock(mutex_);
        if (received_.empty()) return std::nullopt;
        AppPacket p = std::move(received_.front());
        received_.pop_front();
        received_bytes_ -= queued_size(p);
        return p;
    }

    void simulate_link_loss() override {
        std::lock_guard lock(mutex_);
        deaf_ = true;
    }

    void simulate_socket_loss() override {
        std::lock_guard lock(mutex_);
        socket_.close();
    }

    void set_no_hold(bool on) override {
        std::lock_guard lock(mutex_);
        no_hold_ = on;
    }

    void disconnect() override {
        bool join = false;
        {
            std::lock_guard lock(mutex_);
            if (state_ == SessionState::Connecting) {
                state_ = SessionState::Disconnected;
                socket_.close();
                join = true;
            } else if (state_ == SessionState::Connected) {
                state_ = SessionState::Disconnecting;
                drain_started_ = now_();
            }
        }
        if (join) stop_thread();
    }

private:
    // A host name's lookup (connect()): filled on resolve_async's thread.
    struct Lookup {
        std::mutex mutex;
        bool done = false;
        std::vector<platform::Endpoint> endpoints;
        std::string error;
    };

    // Opens the socket to `to` and sends the first SessionRequest.
    void begin(const platform::Endpoint& to, Clock::time_point now) {
        std::string err;
        peer_ = to;
        if (!socket_.open(peer_.family, err)) return fail(err);
        send_session_request(now);
        log::debug("session: connecting to {} ({})", peer_.to_string(), host_);
    }

    // While the host's name is looked up: begins once the addresses came. True while still waiting.
    bool looking_up(Clock::time_point now) {
        if (!lookup_) return false;
        std::vector<platform::Endpoint> endpoints;
        std::string error;
        {
            std::lock_guard l(lookup_->mutex);
            if (!lookup_->done) return true;
            endpoints = std::move(lookup_->endpoints);
            error = std::move(lookup_->error);
        }
        lookup_.reset();
        if (endpoints.empty()) {
            fail(error.empty() ? std::format("cannot resolve {}", host_) : error);
            return true;
        }
        begin(endpoints.front(), now);
        return state_ == SessionState::Disconnected;
    }

    // SessionOptions::clock, else the steady clock.
    Clock::time_point now_() const { return opt_.clock ? opt_.clock() : Clock::now(); }

    // UdpManagerThread: service the session every service_interval until it closes.
    void service_loop() {
        platform::CrashStack crash_stack;
        std::unique_lock lock(mutex_);
        while (!stop_ && state_ != SessionState::Disconnected) {
            service();
            wake_.wait_for(lock, opt_.service_interval, [this] { return stop_.load(); });
        }
    }

    void stop_thread() {
        if (!thread_.joinable()) return;
        stop_ = true;
        wake_.notify_all();
        thread_.join();
    }

    // One pass over the connection (UdpConnection_InternalGiveTime): receive, time out, ack, resend, flush.
    void service() {
        if (state_ == SessionState::Disconnected) return;
        receive_all();
        if (state_ == SessionState::Disconnected) return;
        auto now = now_();

        if (state_ == SessionState::Connecting) {
            // The lookup's answer first: one that came back late still connects (or fails as no session response),
            // and only a lookup still unanswered at the timeout is "cannot resolve".
            if (looking_up(now)) {
                if (lookup_ && now - started_ > opt_.connect_timeout)
                    fail(std::format("cannot resolve {}: no answer after {:.1f} s", host_,
                                     static_cast<double>(opt_.connect_timeout.count()) / 1000.0));
                return;
            }
            if (now - started_ > opt_.connect_timeout)
                return fail(std::format("no session response from {} after {:.1f} s", peer_.to_string(),
                                        static_cast<double>(opt_.connect_timeout.count()) / 1000.0));
            if (now - last_request_ >= opt_.request_interval) send_session_request(now);
            return;
        }

        // A socket closed under the session cannot send or receive again: every datagram would fail until a timer
        // ends the link, so end it now.
        if (!socket_.is_open()) return fail("the session's socket was closed");
        // InternalGiveTime @0x8c638e: the link times out once the silence reaches the limit (not only past it).
        if (opt_.idle_timeout.count() > 0 && now - last_recv_ >= opt_.idle_timeout)
            return fail(std::format("connection to {} timed out (nothing received for {} s)", peer_.to_string(),
                                    std::chrono::duration_cast<milliseconds>(now - last_recv_).count() / 1000));

        if (state_ == SessionState::Connected && opt_.stat_interval.count() > 0 && now - last_stat_ >= opt_.stat_interval) {
            last_stat_ = now;
            buffered_send(make_stat_request(now));
        }
        if (ack_pending_) {
            ack_pending_ = false;
            send_ack(static_cast<std::uint16_t>(inbound_.next() - 1));
        }
        pump_reliable(now);
        if (state_ == SessionState::Disconnected) return;
        // UdpConnection_InternalGiveTime @0x8c60e0: the held packets go out once the oldest is hold_time old.
        if (!multi_.empty() && (no_hold_ || now - multi_started_ >= opt_.hold_time)) flush_multi();
        // InternalGiveTime @0x8c61c4: nothing sent for the keepalive delay (Params 0xe), so 00 06 goes out at once,
        // encoded like any packet but not through the multi-buffer.
        if (state_ == SessionState::Connected && opt_.keepalive_interval.count() > 0 && multi_.empty() &&
            now - last_send_ >= opt_.keepalive_interval) {
            ++keepalives_sent_;
            send_now(soe::make_keepalive());
        }

        if (state_ == SessionState::Disconnecting && (reliable_idle() || now - drain_started_ > kDrainTimeout)) {
            flush_multi();
            send_now(soe::make_disconnect(connect_code_));
            report(reliable_idle() ? "closed" : "closed with unacknowledged data");
            state_ = SessionState::Disconnected;
            socket_.close();
        }
    }

    void send_locked(const AppPacket& packet, bool reliable) {
        if (state_ == SessionState::Connecting) {
            early_.emplace_back(packet, reliable);
            return;
        }
        if (state_ != SessionState::Connected) {
            log::warn("session: dropping packet 0x{:04x}, not connected", packet.opcode);
            return;
        }
        Bytes wire = soe::encode_app(packet, opt_.opcode_bytes);
        if (!reliable && wire.size() + params_.overhead() <= params_.max_packet_size) {
            // Unsequenced (channel 0 in the original client).
            buffered_send(wire);
            return;
        }
        // Like the original's SendMessage, a packet too big for one datagram is promoted to reliable.
        // UdpReliableChannel_Send: a send to an idle channel wakes its timer.
        if (pending_.empty()) channel_.wake_time = 0;
        auto pieces = soe::make_reliable(wire, next_out_, params_);
        for (std::size_t i = 0; i < pieces.size(); ++i)
            pending_.push_back({std::move(pieces[i]), i == 0 && pieces.size() > 1});
    }

    void reset() {
        socket_.close();
        lookup_.reset();
        peer_ = {};
        state_ = SessionState::Disconnected;
        error_.clear();
        failed_sends_ = 0;
        params_ = {};
        next_out_ = 0;
        pending_.clear();
        channel_ = ReliableChannel{};
        channel_.slots.resize(static_cast<std::size_t>(opt_.window_slots));
        slot_data_.assign(static_cast<std::size_t>(opt_.window_slots), Bytes{});
        channel_.resend_delay_adjust = opt_.resend_delay_adjust;
        channel_.max_outstanding_bytes = opt_.window_bytes;
        multi_.clear();
        ack_at_ = kNoAck;
        received_.clear();
        received_bytes_ = 0;
        early_.clear();
        inbound_ = soe::InboundStream();
        ack_pending_ = false;
        datagrams_sent_ = datagrams_received_ = bad_datagrams_ = 0;
        bytes_sent_ = bytes_received_ = 0;
        duplicates_ = early_received_ = acks_sent_ = 0;
        longest_stall_ms_ = 0;
        stall_received_ = stall_sent_ = 0;
        ping_last_ = ping_low_ = ping_high_ = ping_count_ = 0;
        ping_total_ = 0;
        sync_ = {};
    }

    SessionStats stats() const override {
        std::lock_guard lock(mutex_);
        SessionStats s;
        s.datagrams_sent = datagrams_sent_;
        s.datagrams_received = datagrams_received_;
        s.bytes_sent = bytes_sent_;
        s.bytes_received = bytes_received_;
        s.acks_sent = acks_sent_;
        s.keepalives_sent = keepalives_sent_;
        s.duplicates = duplicates_;
        s.early = early_received_;
        s.bad = bad_datagrams_;
        s.timeout_resends = channel_.connection_timeout_resends;
        s.fast_resends = channel_.connection_fast_resends;
        s.rtt_ms = channel_.rtt;
        s.queued_packets = static_cast<std::int32_t>(pending_.size());
        if (channel_.oldest_unacked_seq < channel_.next_seq) {
            const auto& slot = channel_.slots[static_cast<std::size_t>(channel_.oldest_unacked_seq %
                                                                     static_cast<std::int64_t>(channel_.slots.size()))];
            if (slot.used && slot.first_send != 0)
                s.oldest_unacked_ms = reliable_elapsed(clock_ms(now_()), slot.first_send);
        }
        s.longest_stall_ms = longest_stall_ms_;
        s.ping_last = ping_last_;
        s.ping_average = ping_count_ ? static_cast<std::int32_t>(ping_total_ / ping_count_) : 0;
        s.ping_low = ping_low_;
        s.ping_high = ping_high_;
        s.ping_count = ping_count_;
        s.sync_our_sent = sync_[0];
        s.sync_our_received = sync_[1];
        s.sync_their_sent = sync_[2];
        s.sync_their_received = sync_[3];
        return s;
    }

    // A summary of the session's traffic when it ends.
    void report(std::string_view how) const {
        log::debug("session {}: {}; sent {} datagrams ({} acks, {} keepalives, {} timeout and {} fast resends), received {} "
                   "({} duplicate, {} early, {} bad), rtt {} ms, longest owner stall {} ms",
                   peer_.to_string(), how, datagrams_sent_, acks_sent_, keepalives_sent_, channel_.connection_timeout_resends,
                   channel_.connection_fast_resends, datagrams_received_, duplicates_, early_received_, bad_datagrams_,
                   channel_.rtt, longest_stall_ms_);
    }

    void fail(std::string message) {
        log::debug("session: {}", message);
        if (state_ == SessionState::Connected || state_ == SessionState::Disconnecting) report(message);
        error_ = std::move(message);
        state_ = SessionState::Disconnected;
        socket_.close();
    }

    void send_session_request(Clock::time_point now) {
        last_request_ = now;
        send_now(soe::make_session_request(connect_code_, opt_.max_packet_size));
    }

    void send_now(ByteSpan packet) {
        if (deaf_) return; // simulate_link_loss
        if (opt_.trace) opt_.trace(true, packet);
        Bytes datagram = soe::encode_datagram(params_, packet);
        if (!socket_.send_to(peer_, datagram)) {
            // Logged once an outage (the network gone while Wi-Fi drops fails every datagram), then summed up when
            // a send works again: one line, not one a datagram.
            if (failed_sends_++ == 0) log::warn("session: {} (further send errors are counted)", socket_.last_error());
        } else if (failed_sends_ != 0) {
            log::info("session: sending works again after {} failed datagrams", failed_sends_);
            failed_sends_ = 0;
        }
        last_send_ = now_();
        ++datagrams_sent_;
        bytes_sent_ += datagram.size();
    }

    // UdpConnection_BufferedSend @0x8c49b0: a protocol packet of at most 255 bytes joins the multi-buffer (00 03,
    // then a length byte and the packet for each), which goes out when the next one does not fit, when it reaches
    // the hold size, or once its oldest packet is hold_time old. Bigger packets flush the buffer and go out alone.
    // The hold size is min(max packet, Params 0x16), and UdpManager's constructor turns 0x16's default -1 into the
    // max packet size (@0x8c9ab2). Returns the packet's offset in the buffer, or kNoAck when it was sent.
    std::size_t buffered_send(ByteSpan packet) {
        std::size_t hold_size = params_.max_packet_size;
        if (packet.size() > 255 || packet.size() + 3 > hold_size) {
            flush_multi();
            send_now(packet);
            return kNoAck;
        }
        // Room in one datagram: the max packet less the encoder's overhead (compression flag, CRC).
        std::size_t room = params_.max_packet_size - params_.overhead();
        if (!multi_.empty() && multi_.size() + 1 + packet.size() > room) flush_multi();
        if (multi_.empty()) {
            multi_ = {std::byte{0}, std::byte{soe::op::Combined}};
            multi_started_ = now_();
        }
        multi_.push_back(static_cast<std::byte>(packet.size()));
        std::size_t at = multi_.size();
        multi_.insert(multi_.end(), packet.begin(), packet.end());
        if (multi_.size() >= hold_size) {
            flush_multi();
            return kNoAck;
        }
        return at;
    }

    // UdpConnection_FlushMultiBuffer @0x8c4850: one held packet goes out as itself, several as 00 03.
    void flush_multi() {
        if (multi_.size() > 2) {
            if (std::to_integer<std::size_t>(multi_[2]) + 3 == multi_.size()) send_now(ByteSpan(multi_).subspan(3));
            else send_now(multi_);
        }
        multi_.clear();
        ack_at_ = kNoAck;
    }

    // UdpReliableChannel's ack (the 00 15 path @0x8cdf0f): a newer cumulative ack replaces the one still held in the
    // multi-buffer instead of adding another.
    void send_ack(std::uint16_t sequence) {
        if (ack_at_ != kNoAck) {
            multi_[ack_at_ + 2] = static_cast<std::byte>(sequence >> 8);
            multi_[ack_at_ + 3] = static_cast<std::byte>(sequence & 0xff);
            return;
        }
        ++acks_sent_;
        ack_at_ = buffered_send(soe::make_ack(soe::op::Ack, sequence));
    }

    bool reliable_idle() const { return pending_.empty() && channel_.oldest_unacked_seq >= channel_.next_seq; }

    // The channel's window sizes from the negotiated packet size (UdpReliableChannel_Ctor @0x8cc690; config +0x24 and
    // +0x28 are 0 in the Params defaults).
    void init_channel() {
        auto fragment = static_cast<std::int32_t>(params_.max_packet_size) - static_cast<std::int32_t>(params_.overhead()) - 4;
        channel_.fragment_size = fragment;
        channel_.min_window = std::max(fragment, 0);
        channel_.start_window = std::max(std::min(std::max(fragment * 2, 0x111c), fragment * 4), 0);
        channel_.start_window = std::max(channel_.start_window, channel_.min_window);
        channel_.congestion_window = channel_.start_window;
        channel_.threshold = std::min(opt_.window_bytes, opt_.window_slots * fragment);
    }

    // Data bytes of a prebuilt reliable packet (the slot length the original counts: no header, no total length).
    static std::int32_t data_length(const Bytes& p, bool first_fragment) {
        return static_cast<std::int32_t>(p.size()) - 4 - (first_fragment ? 4 : 0);
    }

    // UdpReliableChannel_FillPhysicalSlots @0x8cd100: queued packets into slots while the window and `bytes` last.
    bool fill_slots(ReliableChannel& ch, std::int32_t bytes) {
        if (bytes <= 0) return false;
        bool filled = false;
        do {
            if (ch.next_seq - ch.oldest_unacked_seq >= opt_.window_slots || pending_.empty()) break;
            auto index = static_cast<std::size_t>(ch.next_seq % opt_.window_slots);
            ReliableSlot& slot = ch.slots[index];
            slot = ReliableSlot{};
            slot.used = true;
            slot.packet = static_cast<std::int32_t>(index);
            slot.length = data_length(pending_.front().bytes, pending_.front().first_fragment);
            slot_data_[index] = std::move(pending_.front().bytes);
            pending_.pop_front();
            ++ch.next_seq;
            filled = true;
            bytes -= slot.length;
        } while (bytes > 0);
        return filled;
    }

    // The channel's callbacks, made once: they only call back into this session, and the service thread runs
    // pump_reliable every pass.
    void init_hooks() {
        hooks_.max_unacked_age = opt_.reliable_timeout_ms;
        hooks_.fill = [this](ReliableChannel& ch, std::int32_t bytes) { return fill_slots(ch, bytes); };
        hooks_.packet_length = [this](std::int32_t index) { return channel_.slots[static_cast<std::size_t>(index)].length; };
        hooks_.send = [this](std::span<const std::uint8_t>, const ReliableSlot& slot) {
            // The packets are prebuilt with the same header (make_reliable); send them as they are.
            if (slot.send_count > 0)
                log::debug("session: resending seq {} (try {}, rtt {} ms)",
                           u8(slot_data_[static_cast<std::size_t>(slot.packet)][2]) << 8 |
                               u8(slot_data_[static_cast<std::size_t>(slot.packet)][3]),
                           slot.send_count, channel_.rtt);
            buffered_send(slot_data_[static_cast<std::size_t>(slot.packet)]);
        };
        hooks_.disconnect = [this](std::uint32_t) {
            fail(std::format("reliable data was not acknowledged for {} ms", opt_.reliable_timeout_ms));
        };
    }

    // UdpReliableChannel_GiveTime: sends new reliable packets within the window and resends overdue ones.
    void pump_reliable(Clock::time_point now) {
        channel_.queued_packets = static_cast<std::int32_t>(pending_.size());
        reliable_give_time(channel_, clock_ms(now), hooks_);
        channel_.queued_packets = static_cast<std::int32_t>(pending_.size());
    }

    // Frees the packets of the slots acknowledged among sequences [from, to]: an ack frees only slots in that range
    // (AckAll from the oldest unacknowledged to the ack, AckOne its own), so the rest of the ring is not walked.
    void release_acked(std::int64_t from, std::int64_t to) {
        from = std::max<std::int64_t>(from, 0); // an ack widened to before the first sequence
        to = std::min(to, channel_.next_seq - 1);
        for (std::int64_t sequence = from; sequence <= to; ++sequence) {
            const auto index = static_cast<std::size_t>(sequence % static_cast<std::int64_t>(channel_.slots.size()));
            if (!channel_.slots[index].used && !slot_data_[index].empty()) slot_data_[index].clear();
        }
    }

    void on_ack(std::uint16_t seq) {
        const std::int64_t from = channel_.oldest_unacked_seq;
        const std::int64_t to = reliable_widen(channel_, seq);
        reliable_ack_all(channel_, seq, clock_ms(now_()));
        release_acked(from, to);
    }

    void on_out_of_order_ack(std::uint16_t seq) {
        const std::int64_t sequence = reliable_widen(channel_, seq);
        reliable_ack_one(channel_, sequence, clock_ms(now_()));
        release_acked(sequence, sequence);
    }

    void receive_all() {
        std::array<std::byte, 65536> buf;
        for (int n = 0; n < 4096 && socket_.is_open(); ++n) {
            platform::Endpoint from;
            auto size = socket_.recv_from(buf, from);
            if (!size) {
                if (!socket_.last_error().empty()) log::debug("session: {}", socket_.last_error());
                return;
            }
            if (deaf_) continue; // simulate_link_loss
            ++datagrams_received_;
            bytes_received_ += *size;
            on_datagram(ByteSpan(buf.data(), *size), from);
            if (state_ == SessionState::Disconnected) return;
        }
    }

    void on_datagram(ByteSpan d, const platform::Endpoint& from) {
        if (d.size() < 2) return;
        bool protocol = u8(d[0]) == 0;
        std::uint8_t opcode = u8(d[1]);

        if (state_ == SessionState::Connecting) {
            if (!protocol || opcode != soe::op::SessionResponse) return;
            // The reply may come from another port than the one addressed (NAT, OpenEQ stream.rs), or another address:
            // EQEmu binds 0.0.0.0 and answers without IP_PKTINFO, so a host reached on a secondary address answers
            // from the one its route picks. The connect code is what ties the reply to the request; it went out in
            // clear, so one who sees it could answer first (docs/unknowns.md).
            auto r = soe::parse_session_response(d);
            if (!r || r->connect_code != connect_code_) return;
            if (opt_.trace) opt_.trace(false, d);
            if (!(from == peer_)) {
                log::debug("session: response came from {}, using it", from.to_string());
                peer_ = from;
            }
            // Only these sizes have a CRC the encoder and decoder know; another would drop every datagram, and a
            // large one would make overhead() exceed max_packet_size so the room left in a datagram wraps.
            if (r->params.crc_bytes != 0 && r->params.crc_bytes != 2 && r->params.crc_bytes != 4)
                return fail(std::format("the server asked for a {}-byte CRC; only 0, 2 or 4 bytes are supported",
                                        static_cast<int>(r->params.crc_bytes)));
            params_ = r->params;
            // The original adopts the server's size unclamped; keep it sane.
            params_.max_packet_size = std::clamp<std::uint32_t>(params_.max_packet_size, 64, 8192);
            init_channel();
            state_ = SessionState::Connected;
            last_recv_ = now_();
            log::debug("session: connected to {}: crc {} bytes, passes {}/{}, max packet {}", peer_.to_string(),
                       params_.crc_bytes, static_cast<int>(params_.passes[0]), static_cast<int>(params_.passes[1]),
                       params_.max_packet_size);
            auto early = std::move(early_);
            early_.clear();
            for (auto& [p, reliable] : early) send_locked(p, reliable);
            return;
        }
        if (!(from == peer_)) return;

        // The link-death timer counts from the last datagram that is the server's: a keepalive (taken, as EQEmu's own
        // peer takes it, without checking its CRC), or one that passes the CRC and decode. EQEmu's peer restarts it
        // for any datagram from the address, so garbage from the server's address and port would keep a dead session
        // up for ever; a stock server's datagrams always pass, so this changes nothing for them. Whether the
        // original's last-receive time (UdpConnection +0x1d0) skips a datagram that fails its CRC is not known
        // (docs/unknowns.md).
        if (protocol && (opcode == soe::op::KeepAlive || opcode == soe::op::OutboundPing)) {
            last_recv_ = now_();
            if (opt_.trace) opt_.trace(false, d.first(2));
            return;
        }
        // EQEmu sends OP_OutOfSession with a zero connect code (ReliableStreamConnectionManager::SendDisconnect), so
        // there is nothing to match against ours; what the original checks is not known (docs/unknowns.md).
        if (protocol && opcode == soe::op::OutOfSession) {
            if (opt_.trace) opt_.trace(false, d);
            return fail("the server does not know this session (OP_OutOfSession)");
        }
        if (protocol && (opcode == soe::op::SessionRequest || opcode == soe::op::SessionResponse)) return;

        auto packet = soe::decode_datagram(params_, d);
        if (!packet) {
            ++bad_datagrams_;
            log::debug("session: dropped a datagram that failed the CRC or decode ({} bytes)", d.size());
            return;
        }
        last_recv_ = now_();
        if (opt_.trace) opt_.trace(false, *packet);
        process(*packet, 0);
    }

    void process(ByteSpan p, int depth) {
        if (p.empty()) return;
        if (u8(p[0]) != 0 || (p.size() >= 2 && u8(p[1]) == soe::op::Padding)) {
            if (auto app = soe::decode_app(p, opt_.opcode_bytes)) deliver(std::move(*app));
            return;
        }
        if (p.size() < 2 || depth > kMaxNesting) return;
        std::uint8_t opcode = u8(p[1]);
        auto seq_of = [&]() -> std::uint16_t { return p.size() >= 4 ? static_cast<std::uint16_t>(u8(p[2]) << 8 | u8(p[3])) : 0; };

        switch (opcode) {
        case soe::op::Combined:
        case soe::op::AppCombined: {
            auto parts = opcode == soe::op::Combined ? soe::split_combined(p) : soe::split_app_combined(p);
            if (!parts) {
                log::debug("session: malformed combined packet 0x{:02x}", opcode);
                return;
            }
            for (auto part : *parts) {
                process(part, depth + 1);
                if (state_ == SessionState::Disconnected) return;
            }
            return;
        }
        case soe::op::Packet:
        case soe::op::Fragment: {
            if (p.size() < 4) return;
            std::vector<Bytes> ready;
            auto order = inbound_.push(p, ready);
            if (inbound_.broken()) {
                // Unrecoverable: the payloads after it cannot be told apart (InboundStream::broken).
                send_now(soe::make_disconnect(connect_code_));
                return fail("the server sent a first fragment without its total length; the received data cannot be "
                            "read any further");
            }
            if (order == soe::SeqOrder::Future) {
                ++early_received_;
                // An early packet the stream had no room for is not acknowledged, so the server sends it again.
                if (inbound_.holds(seq_of())) buffered_send(soe::make_ack(soe::op::OutOfOrderAck, seq_of()));
            } else {
                if (order == soe::SeqOrder::Past) ++duplicates_;
                ack_pending_ = true; // current: ack it; past: re-ack the latest, the peer missed our ack
            }
            for (auto& payload : ready) {
                process(payload, depth + 1);
                if (state_ == SessionState::Disconnected) return;
            }
            return;
        }
        case soe::op::Ack:
            if (p.size() >= 4) on_ack(seq_of());
            return;
        case soe::op::OutOfOrderAck:
            if (p.size() >= 4) on_out_of_order_ack(seq_of());
            return;
        case soe::op::KeepAlive:
        case soe::op::OutboundPing:
            return;
        case soe::op::SessionStatRequest:
            buffered_send(make_stat_response(p));
            return;
        case soe::op::SessionStatResponse:
            take_stat_response(p);
            return;
        case soe::op::SessionDisconnect:
            return fail("the server closed the session");
        default:
            log::debug("session: ignoring protocol opcode 0x{:02x} ({} bytes)", opcode, p.size());
            return;
        }
    }

    // What a received packet counts against max_received_bytes: its data and the packet itself, so that a flood of
    // empty packets is bounded too.
    static std::size_t queued_size(const AppPacket& p) { return p.data.size() + sizeof(AppPacket); }

    void deliver(AppPacket app) {
        if (state_ == SessionState::Disconnected) return;
        std::size_t size = queued_size(app);
        if (size > opt_.max_received_bytes - std::min(received_bytes_, opt_.max_received_bytes)) {
            log::warn("session {}: {} bytes of received packets are waiting to be read (at most {}); closing",
                      peer_.to_string(), received_bytes_ + size, opt_.max_received_bytes);
            send_now(soe::make_disconnect(connect_code_));
            return fail(std::format("too much received data waiting to be read ({} packets, {} bytes)",
                                    received_.size() + 1, received_bytes_ + size));
        }
        received_bytes_ += size;
        received_.push_back(std::move(app));
    }

    Bytes make_stat_request(Clock::time_point now) {
        auto ms = std::chrono::duration_cast<milliseconds>(now.time_since_epoch()).count();
        auto ping = static_cast<std::uint32_t>(channel_.rtt);
        ByteWriter w;
        w.le<std::uint8_t>(0);
        w.le<std::uint8_t>(soe::op::SessionStatRequest);
        w.be<std::uint16_t>(static_cast<std::uint16_t>(ms & 0xffff));
        for (int i = 0; i < 5; ++i) w.be<std::uint32_t>(ping); // stat, avg, min, max, last ping
        w.be<std::uint64_t>(datagrams_sent_);
        w.be<std::uint64_t>(datagrams_received_);
        return w.take();
    }

    // The server's answer to our stat request (EQEmu's DaybreakSessionStatResponse: the request's time, its own time,
    // our sent / received as the request gave them, its own sent / received, all big-endian): the round trip from the
    // echoed 16-bit time, the server's two counts, and ours now (as EQEmu's own peer pairs them, DaybreakConnection's
    // OP_SessionStatResponse: the counts the request carried are older than the server's).
    void take_stat_response(ByteSpan p) {
        if (p.size() < 40) return;
        auto be = [&](std::size_t at, std::size_t n) {
            std::uint64_t v = 0;
            for (std::size_t i = 0; i < n; ++i) v = v << 8 | std::to_integer<std::uint64_t>(p[at + i]);
            return v;
        };
        auto now = std::chrono::duration_cast<milliseconds>(now_().time_since_epoch()).count();
        auto ping = static_cast<std::int32_t>(static_cast<std::uint16_t>((now & 0xffff) - be(2, 2)));
        ping_last_ = ping;
        ping_total_ += ping;
        ping_low_ = ping_count_ ? std::min(ping_low_, ping) : ping;
        ping_high_ = ping_count_ ? std::max(ping_high_, ping) : ping;
        ++ping_count_;
        sync_ = {datagrams_sent_, datagrams_received_, be(24, 8), be(32, 8)};
    }

    // Answers a server-initiated stat request the way EQEmu's own peer does.
    Bytes make_stat_response(ByteSpan request) {
        auto ms = std::chrono::duration_cast<milliseconds>(now_().time_since_epoch()).count();
        ByteWriter w;
        w.le<std::uint8_t>(0);
        w.le<std::uint8_t>(soe::op::SessionStatResponse);
        w.bytes(request.size() >= 4 ? request.subspan(2, 2) : ByteSpan{});
        if (request.size() < 4) w.zeros(2);
        w.be<std::uint32_t>(static_cast<std::uint32_t>(ms));
        if (request.size() >= 40) w.bytes(request.subspan(24, 16));
        else w.zeros(16);
        w.be<std::uint64_t>(datagrams_sent_);
        w.be<std::uint64_t>(datagrams_received_);
        return w.take();
    }

    static constexpr std::size_t kNoAck = static_cast<std::size_t>(-1);

    SessionOptions opt_;
    // Guards everything below; the service thread holds it while it works.
    mutable std::mutex mutex_;
    bool deaf_ = false; // simulate_link_loss
    bool no_hold_ = false; // set_no_hold
    std::condition_variable wake_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    platform::UdpSocket socket_;
    platform::Endpoint peer_;
    std::string host_;               // as connect() was given it
    std::shared_ptr<Lookup> lookup_; // while host_'s name is looked up
    SessionState state_ = SessionState::Disconnected;
    std::string error_;
    std::mt19937_64 rng_;
    std::uint32_t connect_code_ = 0;
    soe::SessionParams params_;

    Clock::time_point started_, last_request_, last_recv_, last_send_, last_stat_, drain_started_, last_update_;

    std::uint16_t next_out_ = 0;
    struct Pending {
        Bytes bytes;
        bool first_fragment = false;
    };
    std::deque<Pending> pending_;    // queued reliable packets, not yet in a slot
    ReliableChannel channel_;        // the original's reliable channel 0
    ReliableChannelHooks hooks_;     // its callbacks (init_hooks)
    std::vector<Bytes> slot_data_;   // each slot's packet
    Bytes multi_;                     // the multi-buffer: 00 03 and the held packets, or empty
    Clock::time_point multi_started_; // when its first packet was added
    std::size_t ack_at_ = kNoAck;     // where our held cumulative ack is in multi_
    std::vector<std::pair<AppPacket, bool>> early_;

    soe::InboundStream inbound_;
    bool ack_pending_ = false;
    std::deque<AppPacket> received_;
    std::size_t received_bytes_ = 0; // received_'s queued_size() total

    std::uint64_t datagrams_sent_ = 0, datagrams_received_ = 0, bad_datagrams_ = 0;
    std::uint64_t bytes_sent_ = 0, bytes_received_ = 0;
    std::uint64_t duplicates_ = 0, early_received_ = 0, acks_sent_ = 0;
    std::uint64_t stall_received_ = 0, stall_sent_ = 0; // counters at the owner's last update()
    std::int64_t longest_stall_ms_ = 0;
    std::int32_t ping_last_ = 0, ping_low_ = 0, ping_high_ = 0, ping_count_ = 0;
    std::int64_t ping_total_ = 0;
    std::array<std::uint64_t, 4> sync_{}; // our sent, our received, their sent, their received
    std::uint64_t keepalives_sent_ = 0;
    std::uint64_t failed_sends_ = 0; // datagrams the socket refused since the last that went (send_now)
};

} // namespace

std::unique_ptr<Session> Session::create(SessionOptions options) {
    return std::make_unique<SessionImpl>(std::move(options));
}

} // namespace hailstone::net
