#include "net/chat_server.h"

#include <mutex>
#include <algorithm>

#include "core/byte_io.h"
#include "core/log.h"
#include "net/packet_io.h"
#include "net/session.h"
#include "platform/udp_socket.h"

namespace hailstone::net {

namespace {

std::size_t event_bytes(const ChatServerEvent& e) {
    std::size_t bytes = sizeof(e) + e.channel.size() + e.sender.size() + e.text.size();
    for (const auto& f : e.fields) bytes += sizeof(f) + f.size();
    return bytes;
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t end = s.find(sep, start);
        if (end == std::string_view::npos) end = s.size();
        out.emplace_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::string trim(std::string s) {
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

AppPacket text_packet(std::uint16_t op, std::initializer_list<std::string_view> strings) {
    ByteWriter w;
    w.le<std::uint8_t>(0); // EQEmu's UCS skips the first byte of OP_MailLogin and OP_Mail
    for (auto s : strings) detail::write_cstring(w, s);
    return AppPacket(op, w.take());
}

} // namespace

// UniversalChatProxy_Connect @0x7f0040 builds its own UdpManager Params on top of Params::Params(0) @0x426e70
// (the defaults) and connects with UdpManager_EstablishConnection(host, port, 30000). Checked in the
// disassembly; "Params n" is the n-th dword:
//   0xe  keepalive 44000 ms, or 14800 ms (0x39d0) with eqclient.ini [Defaults] ChatKeepAlive nonzero;
//   0x12 no-data timeout: not set, the default 0 (none); Hailstone sets 60 s, a deliberate difference (below);
//   0x13 unacknowledged-reliable timeout: not set, the default 120000 ms;
//   0x15 multi-buffer hold: set to 0 (the default is 50 ms; the 50 ms this file once cited was that default);
//   0x1a SessionStatRequest interval: set to 0 (none);
//   0x21 SessionRequest resend: 5000 ms; 0x2b reliable window bytes: 10000; 0x2c/0x2d slots: 24.
SessionOptions ucs_session_options(bool short_keepalive) {
    SessionOptions opt;
    opt.connect_timeout = std::chrono::milliseconds(30000);
    opt.request_interval = std::chrono::milliseconds(5000);
    // Hailstone's own, not the original's (which has none): 60 s with nothing from the chat server ends the link, and
    // ChatServer retries it. Channel text goes through the zone, the client sends nothing reliable on this link
    // between commands and its keepalives are unreliable, so the 30 s unacknowledged-data check alone never sees a
    // link that has stopped delivering; a stock EQEmu UCS sends a keepalive after 9 s of its own silence
    // (ReliableStreamConnectionManager keepalive_delay_ms), so 60 s is several missed in a row.
    opt.idle_timeout = std::chrono::milliseconds(60000);
    opt.keepalive_interval = std::chrono::milliseconds(short_keepalive ? 14800 : 44000);
    opt.opcode_bytes = 1;
    opt.hold_time = std::chrono::milliseconds(0);
    opt.stat_interval = std::chrono::milliseconds(0);
    opt.window_slots = 24;
    opt.window_bytes = 10000;
    opt.reliable_timeout_ms = 120000;
    return opt;
}

AppPacket encode_mail_login(const ChatServerInfo& info) {
    std::string key = std::string(1, info.connection_type) + info.mail_key;
    return text_packet(ucs::kMailLogin, {"SOE.EQ." + info.server_short_name + "." + info.character, key});
}

AppPacket encode_mail_command(const std::string& text) { return text_packet(ucs::kMail, {text}); }

AppPacket encode_mail_bracket_command(const std::string& text) {
    ByteWriter w;
    std::string_view t = text;
    if (!t.empty() && t.front() == '[') {
        w.le<std::uint8_t>(';');
        t.remove_prefix(1);
    }
    detail::write_cstring(w, t);
    w.le<std::uint8_t>(0); // the empty argument string
    return AppPacket(ucs::kMail, w.take());
}

AppPacket encode_ucs_strings(std::uint16_t op, const std::vector<std::string>& strings) {
    ByteWriter w;
    for (const auto& s : strings) detail::write_cstring(w, s);
    return AppPacket(op, w.take());
}

std::optional<ChatServerEvent> decode_chat_server_packet(const AppPacket& p) {
    return detail::decode_with(p, [&](ByteReader& r) -> ChatServerEvent {
        ChatServerEvent e;
        switch (p.opcode) {
        case ucs::kChannelMessage: {
            e.kind = ChatServerEvent::Kind::Message;
            e.channel = r.cstring();
            e.sender = r.cstring();
            e.text = r.cstring();
            if (r.remaining()) e.fields.push_back(r.cstring()); // what follows ("SPAM:0:")
            return e;
        }
        case ucs::kMail:
            e.kind = ChatServerEvent::Kind::Channels;
            e.text = r.remaining() ? r.fixed_string(r.remaining()) : std::string();
            return e;
        case ucs::kBuddy:
            e.kind = ChatServerEvent::Kind::Buddy;
            e.added = r.le<std::uint8_t>() == 1;
            e.text = r.cstring();
            return e;
        case ucs::kIgnore:
            e.kind = ChatServerEvent::Kind::Ignore;
            e.added = r.le<std::uint8_t>() == 0;
            e.text = r.cstring();
            return e;
        case ucs::kMailLogin: {
            // Client::SendMailBoxes: u8 1, u32 count, u32 0, the comma list, a NUL.
            e.kind = ChatServerEvent::Kind::MailboxList;
            e.accepted = r.le<std::uint8_t>() != 0;
            if (!e.accepted) return e;
            auto count = r.le<std::uint32_t>();
            r.le<std::uint32_t>();
            std::string list = r.cstring();
            if (count > 128) throw std::out_of_range("too many UCS mailboxes");
            if (!list.empty()) {
                for (auto& m : split(list, ',')) {
                    auto name = trim(m);
                    if (name.empty()) throw std::out_of_range("empty UCS mailbox");
                    e.fields.push_back(std::move(name));
                }
            }
            if (e.fields.size() != count) throw std::out_of_range("UCS mailbox count does not match the list");
            return e;
        }
        case ucs::kMailHeaderCount:
        case ucs::kMailHeader:
        case ucs::kMailSendBody:
        case ucs::kMailNew:
        case ucs::kMailDeliveryStatus:
        case ucs::kMailboxChange: {
            e.kind = p.opcode == ucs::kMailHeaderCount     ? ChatServerEvent::Kind::MailHeaderCount
                     : p.opcode == ucs::kMailHeader        ? ChatServerEvent::Kind::MailHeader
                     : p.opcode == ucs::kMailSendBody      ? ChatServerEvent::Kind::MailBody
                     : p.opcode == ucs::kMailNew           ? ChatServerEvent::Kind::MailNew
                     : p.opcode == ucs::kMailDeliveryStatus ? ChatServerEvent::Kind::MailDelivery
                                                           : ChatServerEvent::Kind::MailboxChange;
            // NUL-separated strings; the last one may lack its NUL (the body's "TO:" line).
            while (!r.empty()) {
                std::string f;
                while (!r.empty()) {
                    auto c = static_cast<char>(r.le<std::uint8_t>());
                    if (c == 0) break;
                    f.push_back(c);
                }
                e.fields.push_back(std::move(f));
            }
            return e;
        }
        case ucs::kAnnounceJoin:
        case ucs::kAnnounceLeave:
            e.kind = p.opcode == ucs::kAnnounceJoin ? ChatServerEvent::Kind::AnnounceJoin
                                                    : ChatServerEvent::Kind::AnnounceLeave;
            e.channel = r.cstring();
            e.sender = r.cstring();
            return e;
        default:
            throw std::out_of_range("unhandled chat server opcode");
        }
    });
}

struct ChatServer::Resolve {
    std::mutex mutex;
    bool done = false;
    std::vector<platform::Endpoint> endpoints;
    std::string error;
};

ChatServer::ChatServer() : ChatServer(Options{}) {}
ChatServer::ChatServer(Options options) : options_(std::move(options)) {}
ChatServer::~ChatServer() { disconnect(); }

ChatServer::Clock::time_point ChatServer::now() const {
    return options_.session.clock ? options_.session.clock() : Clock::now();
}

void ChatServer::connect(const ChatServerInfo& info) {
    disconnect();
    info_ = info;
    error_.clear();
    const auto mailbox = "SOE.EQ." + info.server_short_name + "." + info.character;
    // The stock UCS copies the mailbox and key into char[64]. Refuse an identity it cannot safely read rather
    // than truncating a character's name into another identity. Embedded NULs would change the fields on wire.
    if (info.host.empty() || info.port == 0 || info.character.empty() || mailbox.size() > 63 ||
        info.mail_key.size() > 62 || info.host.find('\0') != std::string::npos ||
        mailbox.find('\0') != std::string::npos || info.mail_key.find('\0') != std::string::npos) {
        error_ = "invalid chat server endpoint or identity";
        return;
    }
    active_ = true;
    retries_ = 0;
    begin_attempt();
}

void ChatServer::begin_attempt() {
    retry_pending_ = false;
    channels_until_disconnect_polled_ = false;
    attempt_started_ = now();
    last_probe_ = attempt_started_;
    login_sent_ = false;
    logged_in_ = false;
    // CEverQuest::ChatServerConnect runs while the player is in the zone: the name (from OP_SetChatServer2) is looked
    // up on a thread, and update() opens the session on its address. A lookup given up on (disconnect, another
    // connect) finishes on its own and is dropped; one still running at exit is left to the process's end
    // (resolve_async).
    auto r = std::make_shared<Resolve>();
    resolving_ = r;
    platform::resolve_async(info_.host, info_.port, [r](std::vector<platform::Endpoint> endpoints, std::string error) {
        std::lock_guard lock(r->mutex);
        r->endpoints = std::move(endpoints);
        r->error = std::move(error);
        r->done = true;
    });
    log::info("chat server: connecting to {}:{} as {}.{}", info_.host, info_.port, info_.server_short_name, info_.character);
}

void ChatServer::start_session(const std::string& address, std::uint16_t port) {
    session_ = Session::create(options_.session);
    session_->connect(address, port); // a numeric address: no lookup on this thread
}

bool ChatServer::connected() const { return session_ && logged_in_ && session_->state() == SessionState::Connected; }

bool ChatServer::failed() const {
    if (resolving_) return false;
    return !session_ || session_->state() == SessionState::Disconnected;
}

void ChatServer::fail(std::string error, bool retry) {
    error_ = std::move(error);
    log::warn("chat server: {}", error_);
    resolving_.reset();
    session_.reset();
    login_sent_ = logged_in_ = false;
    pending_.clear();
    pending_bytes_ = 0;
    // Only the unauthenticated rows are dropped. Events accepted after the login (tells, channel text received
    // earlier in this update) are still delivered, before the Disconnected that retires the session; the channel
    // list they are numbered against stays until that Disconnected is polled.
    before_login_.clear();
    before_login_bytes_ = 0;
    if (events_.empty()) channels_.clear();
    else channels_until_disconnect_polled_ = true;
    ChatServerEvent event;
    event.kind = ChatServerEvent::Kind::Disconnected;
    events_.push_back(std::move(event));
    retry_pending_ = active_ && retry && options_.reconnect_delay.count() > 0 && retries_ < options_.max_reconnect_attempts;
    if (retry_pending_) {
        // Hailstone's back-off: 5, 10, 20, 40 s ... capped, so a server that keeps failing is not hammered.
        auto delay = options_.reconnect_delay;
        for (std::size_t i = 0; i < retries_ && delay < options_.max_reconnect_delay; ++i) delay *= 2;
        retry_at_ = now() + std::min(delay, std::max(options_.max_reconnect_delay, options_.reconnect_delay));
        ++retries_;
    } else {
        active_ = false;
    }
}

void ChatServer::accept_event(ChatServerEvent event) {
    if (event.kind == ChatServerEvent::Kind::Channels) {
        channels_.clear();
        channels_until_disconnect_polled_ = false;
        if (!event.text.empty())
            for (auto& c : split(event.text, ',')) channels_.push_back(trim(c));
    }
    events_.push_back(std::move(event));
}

void ChatServer::update() {
    if (retry_pending_) {
        if (now() < retry_at_) return;
        begin_attempt();
    }
    if (resolving_) {
        if (now() - attempt_started_ >= options_.session.connect_timeout)
            return fail("chat server lookup timed out", true);
        std::vector<platform::Endpoint> endpoints;
        std::string error;
        {
            std::lock_guard lock(resolving_->mutex);
            if (!resolving_->done) return;
            endpoints = std::move(resolving_->endpoints);
            error = std::move(resolving_->error);
        }
        resolving_.reset();
        if (endpoints.empty()) {
            return fail(error.empty() ? "could not resolve the chat server" : error, true);
        }
        start_session(endpoints.front().address_string(), endpoints.front().port);
    }
    if (!session_) return;
    session_->update();
    if (session_->state() == SessionState::Disconnected) {
        // Stock UCS answers a key or address mismatch by closing the stream, never with a refusal packet
        // (EQEmu ucs/clientlist.cpp Clientlist::Process: KeyValid = false, then ClientStream->Close()). A close after
        // the login went out and before any reply is therefore a refusal: retrying would only repeat it.
        if (login_sent_ && !logged_in_) return fail("the chat server closed the link before answering the login", false);
        return fail(session_->error().empty() ? "the chat server disconnected" : session_->error(), true);
    }
    if (!login_sent_ && session_->state() == SessionState::Connecting &&
        now() - attempt_started_ >= options_.session.connect_timeout)
        return fail("chat server connection timed out", true);
    if (!login_sent_ && session_->state() == SessionState::Connected) {
        session_->send(encode_mail_login(info_));
        login_sent_ = true;
        login_started_ = now();
    }
    while (auto p = session_->receive()) {
        auto e = decode_chat_server_packet(*p);
        if (!e) {
            log::debug("chat server: opcode 0x{:02x} ({} bytes)", p->opcode, p->data.size());
            continue;
        }
        if (e->kind == ChatServerEvent::Kind::MailboxList) {
            if (!login_sent_) continue;
            if (!e->accepted) return fail("the chat server refused the login", false);
            if (logged_in_) continue; // a repeated application reply does not replay commands or reset mail
            logged_in_ = true;
            retries_ = 0;
            error_.clear();
            accept_event(std::move(*e));
            while (!before_login_.empty()) {
                accept_event(std::move(before_login_.front()));
                before_login_.pop_front();
            }
            before_login_bytes_ = 0;
            while (!pending_.empty()) {
                session_->send(pending_.front());
                pending_.pop_front();
            }
            pending_bytes_ = 0;
        } else if (!logged_in_) {
            const auto bytes = event_bytes(*e);
            if (before_login_.size() >= options_.max_before_login_events ||
                bytes > options_.max_before_login_bytes - before_login_bytes_)
                return fail("too many chat events before the login reply", true);
            before_login_bytes_ += bytes;
            before_login_.push_back(std::move(*e));
        } else {
            accept_event(std::move(*e));
        }
    }
    if (login_sent_ && !logged_in_ && now() - login_started_ >= options_.login_timeout)
        return fail("the chat server did not answer the login", true);
    // UniversalChatProxy checks the age of the oldest sent, unacknowledged packet, not time since any traffic.
    // Keepalives and unrelated inbound messages therefore cannot hide a stuck command.
    if (login_sent_ && now() - last_probe_ > std::chrono::milliseconds(5000)) {
        last_probe_ = now();
        if (session_->stats().oldest_unacked_ms > 30000)
            return fail("the chat server did not acknowledge reliable data", true);
    }
}

bool ChatServer::send_command_packet(AppPacket packet) {
    if (!active_ || retry_pending_ || (!session_ && !resolving_)) return false;
    if (!logged_in_) {
        const auto bytes = packet.data.size() + sizeof(packet);
        if (pending_.size() >= options_.max_pending_packets || bytes > options_.max_pending_bytes - pending_bytes_) return false;
        pending_bytes_ += bytes;
        pending_.push_back(std::move(packet));
        return true;
    }
    if (session_->state() != SessionState::Connected) return false;
    session_->send(packet);
    return true;
}

bool ChatServer::send_command(const std::string& text) {
    if (text.size() > options_.max_pending_bytes || text.find('\0') != std::string::npos) return false;
    return send_command_packet(encode_mail_command(text));
}

bool ChatServer::send_mail_command(const std::string& text) {
    if (text.size() > options_.max_pending_bytes || text.find('\0') != std::string::npos) return false;
    return send_command_packet(encode_mail_bracket_command(text));
}

std::optional<ChatServerEvent> ChatServer::poll() {
    if (events_.empty()) return std::nullopt;
    auto e = std::move(events_.front());
    events_.pop_front();
    if (e.kind == ChatServerEvent::Kind::Disconnected && channels_until_disconnect_polled_) {
        channels_.clear();
        channels_until_disconnect_polled_ = false;
    }
    return e;
}

void ChatServer::simulate_link_loss() {
    if (session_) session_->simulate_link_loss();
}

void ChatServer::simulate_socket_loss() {
    if (session_) session_->simulate_socket_loss();
}

void ChatServer::disconnect() {
    active_ = retry_pending_ = false;
    resolving_.reset();
    // Destroy the old socket and its reliable queue before accepting another endpoint or identity.
    session_.reset();
    login_sent_ = logged_in_ = false;
    pending_.clear();
    pending_bytes_ = 0;
    before_login_.clear();
    before_login_bytes_ = 0;
    events_.clear();
    channels_.clear();
    channels_until_disconnect_polled_ = false;
}

} // namespace hailstone::net
