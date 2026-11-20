#include "web/browser_host.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <type_traits>
#include <utility>
#include <variant>

#include "core/log.h"
#include "core/web_url.h"
#include "web/page_rules.h"
#include "platform/helper_process.h"

namespace hailstone::web {

TabRules tab_rules(const TabOptions& o) {
    if (!o.owner.empty()) return TabRules::Sites;
    return o.server_link ? TabRules::ServerLink : TabRules::Player;
}

namespace {

// What one poll reads at most, so a helper that floods the pipe cannot stall a frame.
constexpr std::size_t kReadBudget = std::size_t{16} << 20;

class ProcessTransport final : public Transport {
public:
    explicit ProcessTransport(std::vector<std::string> args) : args_(std::move(args)) {}
    bool start(std::string& error) override { return process_.start(args_, error); }
    void send(std::span<const std::byte> bytes) override { process_.write(bytes); }
    std::size_t receive(std::span<std::byte> out) override {
        process_.flush();
        return process_.read(out);
    }
    bool alive() override { return process_.running(); }
    std::size_t pending() override { return process_.pending_write(); }
    std::uint64_t taken() override { return process_.written(); }
    void stop(int grace_ms) override {
        process_.stop(false);
        if (grace_ms > 0 && process_.wait_exit(grace_ms)) return;
        process_.stop(true);
    }

private:
    std::vector<std::string> args_;
    platform::HelperProcess process_;
};

bool allowed(const std::string& url, const TabOptions& o) { return tab_may_show(url, tab_rules(o), o.allow); }

std::string refusal(const std::string& url) {
    const std::string host = web_host(url);
    if (host.empty()) return std::format("{} is not an http or https address", url);
    if (private_host(host))
        return std::format("{} is on this computer or a private network, which scripts and server links may not open", url);
    return std::format("{} is not on a site this server lets scripts open", url);
}


// Copies a frame's rectangles into the image (a new size starts it again, cleared).
void apply(Image& img, const Frame& f) {
    if (img.width != f.width || img.height != f.height) {
        img.width = f.width;
        img.height = f.height;
        img.bgra.assign(std::size_t{f.width} * f.height * 4, std::byte{0});
        img.dirty.clear();
        img.dirty.push_back({0, 0, f.width, f.height});
    }
    const std::size_t stride = std::size_t{img.width} * 4;
    for (const FrameRect& r : f.rects) {
        const std::size_t row = std::size_t{r.width} * 4;
        for (std::size_t y = 0; y < r.height; ++y)
            if (row)
                std::memcpy(img.bgra.data() + (r.y + y) * stride + std::size_t{r.x} * 4, r.bgra.data() + y * row, row);
        img.dirty.push_back({r.x, r.y, r.width, r.height});
    }
    ++img.serial;
}

} // namespace

std::unique_ptr<Transport> process_transport(std::vector<std::string> args) {
    return std::make_unique<ProcessTransport>(std::move(args));
}

BrowserHost::BrowserHost(std::function<std::unique_ptr<Transport>()> make_transport)
    : make_transport_(std::move(make_transport)) {}

BrowserHost::~BrowserHost() { shutdown(); }

bool BrowserHost::ensure_started() {
    if (gave_up_) return false;
    if (transport_ || restart_at_) return true; // running, or coming back (the tabs reopen then)
    transport_ = make_transport_ ? make_transport_() : nullptr;
    std::string error;
    if (!transport_ || !transport_->start(error)) {
        // No helper to run (not installed, or it cannot start): pages go to the system's browser instead.
        last_error_ = transport_ ? std::format("the browser helper did not start: {}", error) : "no browser helper";
        log::warn("web: {}", last_error_);
        transport_.reset();
        gave_up_ = true;
        return false;
    }
    reader_ = MessageReader();
    ready_ = false;
    taken_ = 0;
    stall_since_.reset();
    send(Hello{}, true);
    return true;
}

bool BrowserHost::send(const Message& m, bool always, std::string* why) {
    if (!transport_) return true;
    std::vector<std::byte> bytes = encode_message(m);
    // The helper takes a length over its limit as a broken stream and quits, so such a message is never sent.
    const std::size_t limit = max_message_size(static_cast<std::uint8_t>(bytes[4]));
    if (bytes.size() - 4 > limit) {
        log::warn("web: not sending a message of {} bytes (at most {})", bytes.size() - 4, limit);
        if (why) *why = "it is too long to send";
        return false;
    }
    // Held back while the helper is still taking earlier messages: dropped here, not queued until the queue's
    // limit ends a helper that is reading (a script sending several large evals in one frame).
    const std::size_t pending = transport_->pending();
    if (!always && pending != 0 && pending + bytes.size() > kSendBudget) {
        if (!refusing_) log::warn("web: the browser helper is still taking {} bytes: messages refused meanwhile", pending);
        refusing_ = true;
        if (why) *why = "the browser helper is still taking earlier messages";
        return false;
    }
    refusing_ = false;
    transport_->send(bytes);
    return true;
}

Open BrowserHost::open_message(const Tab& t) const {
    Open o;
    o.tab = t.id;
    o.width = t.options.width;
    o.height = t.options.height;
    o.rules = tab_rules(t.options);
    if (o.rules == TabRules::Sites) o.allow = t.options.allow;
    o.profile = t.options.profile;
    o.url = t.url;
    return o;
}

std::uint32_t BrowserHost::open(const std::string& url, TabOptions options, std::string* why) {
    if (url.size() > kMaxUrl) {
        if (why) *why = "the address is too long";
        return 0;
    }
    if (!allowed(url, options)) {
        if (why) *why = refusal(url);
        return 0;
    }
    if (!ensure_started()) {
        if (why) *why = last_error_;
        return 0;
    }
    Tab t;
    t.id = next_id_++;
    t.options = std::move(options);
    t.url = url;
    t.loading = true;
    const std::uint32_t id = t.id;
    auto& stored = tabs_.emplace(id, std::move(t)).first->second;
    if (!send(open_message(stored), false, why)) { // a tab the helper never opened would load for ever
        tabs_.erase(id);
        return 0;
    }
    return id;
}

Tab* BrowserHost::live(std::uint32_t id) {
    auto it = tabs_.find(id);
    return it == tabs_.end() || it->second.closed ? nullptr : &it->second;
}

bool BrowserHost::navigate(std::uint32_t id, const std::string& url, std::string* why) {
    Tab* t = live(id);
    if (!t) {
        if (why) *why = "no such tab";
        return false;
    }
    if (url.size() > kMaxUrl) {
        if (why) *why = "the address is too long";
        return false;
    }
    if (!allowed(url, t->options)) {
        if (why) *why = refusal(url);
        return false;
    }
    if (!send(Navigate{id, url}, false, why)) return false;
    t->url = url;
    return true;
}

void BrowserHost::resize(std::uint32_t id, std::uint16_t width, std::uint16_t height) {
    Tab* t = live(id);
    if (!t || width == 0 || height == 0 || (t->options.width == width && t->options.height == height)) return;
    if (!send(Resize{id, width, height})) return; // the next resize sends it again
    t->options.width = width;
    t->options.height = height;
}

// The player's input goes whatever the queue holds: a few bytes each, and a release or a focus change refused while
// a script's large message drains would leave the page holding a button or key. Scripts and plugins can call nav
// too (in a loop), so only the player's goes past the budget.
void BrowserHost::mouse(const Mouse& m) {
    if (live(m.tab)) send(m, true);
}
void BrowserHost::wheel(const Wheel& w) {
    if (live(w.tab)) send(w, true);
}
void BrowserHost::key(const Key& k) {
    if (live(k.tab)) send(k, true);
}
bool BrowserHost::text(std::uint32_t id, const std::string& s) {
    if (!live(id)) return false;
    return s.empty() || send(Text{id, s});
}
void BrowserHost::focus(std::uint32_t id, bool on) {
    if (live(id)) send(Focus{id, on}, true);
}
bool BrowserHost::nav(std::uint32_t id, NavAction action, bool player) {
    return live(id) && send(Nav{id, action}, player);
}
bool BrowserHost::eval(std::uint32_t id, const std::string& script) { return live(id) && send(Eval{id, script}); }
bool BrowserHost::post(std::uint32_t id, const std::string& message) {
    return live(id) && message.size() <= kMaxPostMessage && send(Post{id, message});
}

void BrowserHost::close(std::uint32_t id) {
    auto it = tabs_.find(id);
    if (it == tabs_.end()) return;
    if (!it->second.closed) send(Close{id}, true);
    tabs_.erase(it);
}

void BrowserHost::refuse(std::uint32_t id, std::string detail) {
    Tab* t = live(id);
    if (!t) return;
    send(Close{id}, true);
    close_tab(*t, CloseReason::Refused, std::move(detail));
}

void BrowserHost::close_tab(Tab& t, CloseReason reason, std::string detail) {
    if (t.closed) return;
    t.closed = true;
    t.close_reason = reason;
    t.close_detail = std::move(detail);
    t.loading = false;
    // A closed tab is never drawn: its images go now (the textures follow), not when its owner forgets it.
    t.view = Image{};
    t.popup = Image{};
    t.popup_shown = false;
}

void BrowserHost::handle(Message m) {
    if (auto* r = std::get_if<Ready>(&m)) {
        if (r->version != kProtocolVersion) {
            last_error_ = std::format("the browser helper speaks version {}, this client {}", r->version, kProtocolVersion);
            log::warn("web: {}", last_error_);
            gave_up_ = true;
            for (auto& [id, t] : tabs_) close_tab(t, CloseReason::Failed, last_error_);
            transport_->stop(0);
            transport_.reset();
            return;
        }
        ready_ = true;
        return;
    }
    auto tab_of = [&](std::uint32_t id) { return live(id); };
    std::visit(
        [&](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Frame>) {
                if (Tab* t = tab_of(v.tab)) apply(v.layer == Layer::Popup ? t->popup : t->view, v);
            } else if constexpr (std::is_same_v<T, Popup>) {
                if (Tab* t = tab_of(v.tab)) {
                    t->popup_shown = v.shown;
                    t->popup_x = v.x;
                    t->popup_y = v.y;
                }
            } else if constexpr (std::is_same_v<T, Address>) {
                if (Tab* t = tab_of(v.tab)) {
                    t->url = v.url;
                    // The helper refuses these itself; a tab that got there anyway goes.
                    if (!allowed(v.url, t->options)) {
                        log::warn("web: {}'s tab went to {}, outside its sites: closed",
                                  t->options.owner.empty() ? std::string("a server link") : t->options.owner, v.url);
                        send(Close{v.tab}, true);
                        close_tab(*t, CloseReason::Refused, refusal(v.url));
                    }
                }
            } else if constexpr (std::is_same_v<T, Title>) {
                if (Tab* t = tab_of(v.tab)) t->title = v.title;
            } else if constexpr (std::is_same_v<T, Loading>) {
                if (Tab* t = tab_of(v.tab)) {
                    t->loading = v.loading;
                    t->can_back = v.can_back;
                    t->can_forward = v.can_forward;
                }
            } else if constexpr (std::is_same_v<T, CursorShape>) {
                if (Tab* t = tab_of(v.tab)) t->cursor = v.cursor;
            } else if constexpr (std::is_same_v<T, Console>) {
                if (Tab* t = tab_of(v.tab)) {
                    cut_utf8(v.text, kMaxConsoleText); // the helper cuts them already
                    t->console.push_back(std::move(v));
                    if (t->console.size() > kConsoleLines) t->console.pop_front();
                }
            } else if constexpr (std::is_same_v<T, PageMessage>) {
                // Only for the script or plugin that opened the tab; the player's own tabs take none.
                if (Tab* t = tab_of(v.tab); t && !t->options.owner.empty()) queue_message(*t, std::move(v.message));
            } else if constexpr (std::is_same_v<T, Closed>) {
                if (Tab* t = tab_of(v.tab)) close_tab(*t, v.reason, std::move(v.detail));
            }
            // The client's own messages coming back are ignored.
        },
        m);
}

void BrowserHost::queue_message(Tab& t, std::string message) {
    // A page posting faster than its owner takes loses its oldest messages: at most kMessageQueue of them and
    // kMessageQueueBytes in all. One over kMaxPageMessage (the helper drops those itself) is not kept.
    if (message.size() > kMaxPageMessage) {
        log::warn("web: a message of {} bytes from {}'s tab dropped (at most {})", message.size(), t.options.owner,
                  kMaxPageMessage);
        return;
    }
    std::size_t dropped = 0;
    while (!t.messages.empty() &&
           (t.messages.size() >= kMessageQueue || t.message_bytes + message.size() > kMessageQueueBytes)) {
        t.message_bytes -= t.messages.front().size();
        t.messages.pop_front();
        ++dropped;
    }
    if (dropped && !t.messages_dropped) {
        log::warn("web: {}'s tab posts more than it takes: its oldest messages are dropped", t.options.owner);
        t.messages_dropped = true;
    }
    t.message_bytes += message.size();
    t.messages.push_back(std::move(message));
}

void BrowserHost::helper_died(std::uint64_t now_ms, const std::string& why) {
    last_error_ = std::format("the browser helper {}", why);
    log::warn("web: {}", last_error_);
    if (transport_) transport_->stop(0);
    transport_.reset();
    reader_ = MessageReader();
    ready_ = false;
    deaths_.push_back(now_ms);
    while (!deaths_.empty() && now_ms - deaths_.front() > kRestartWindowMs) deaths_.pop_front();
    if (deaths_.size() > static_cast<std::size_t>(kMaxRestarts)) {
        gave_up_ = true;
        for (auto& [id, t] : tabs_) close_tab(t, CloseReason::Crashed, last_error_);
        return;
    }
    restart_at_ = now_ms + kRestartDelayMs;
}

void BrowserHost::poll(std::uint64_t now_ms) {
    if (restart_at_ && now_ms >= *restart_at_) {
        restart_at_.reset();
        bool any = std::any_of(tabs_.begin(), tabs_.end(), [](const auto& e) { return !e.second.closed; });
        if (any && ensure_started())
            for (auto& [id, t] : tabs_)
                if (!t.closed) send(open_message(t), true);
    }
    if (!transport_) return;
    std::vector<std::byte> buf(std::size_t{1} << 16);
    std::size_t total = 0;
    auto drain = [&] {
        while (total < kReadBudget) {
            std::size_t n = transport_->receive(buf);
            if (n == 0) break;
            total += n;
            reader_.feed(std::span<const std::byte>(buf).first(n));
            while (auto m = reader_.next()) {
                handle(std::move(*m));
                if (!transport_) return;
            }
            if (reader_.failed()) return;
        }
    };
    const bool alive = transport_->alive();
    drain(); // what it wrote before it exited too
    if (!transport_) return;
    if (reader_.failed()) {
        helper_died(now_ms, "sent a message this client cannot read");
        return;
    }
    if (!alive) {
        helper_died(now_ms, "exited");
        return;
    }
    // Alive, but taking none of what waits for it (its reader stuck): ended, so it restarts as a dead one does.
    const std::uint64_t taken = transport_->taken();
    const bool progress = taken != taken_;
    taken_ = taken;
    if (transport_->pending() == 0) stall_since_.reset();
    else if (progress || !stall_since_) stall_since_ = now_ms;
    else if (now_ms - *stall_since_ >= kStallMs) helper_died(now_ms, "stopped reading its input");
}

const Tab* BrowserHost::tab(std::uint32_t id) const {
    auto it = tabs_.find(id);
    return it == tabs_.end() ? nullptr : &it->second;
}

std::vector<std::uint32_t> BrowserHost::tabs() const {
    std::vector<std::uint32_t> ids;
    for (const auto& [id, t] : tabs_) ids.push_back(id);
    return ids;
}

std::vector<Image::Rect> BrowserHost::take_dirty(std::uint32_t id, Layer layer) {
    auto it = tabs_.find(id);
    if (it == tabs_.end()) return {};
    Image& img = layer == Layer::Popup ? it->second.popup : it->second.view;
    return std::exchange(img.dirty, {});
}

std::vector<std::string> BrowserHost::take_messages(std::uint32_t id) {
    auto it = tabs_.find(id);
    if (it == tabs_.end()) return {};
    std::vector<std::string> out(std::make_move_iterator(it->second.messages.begin()),
                                 std::make_move_iterator(it->second.messages.end()));
    it->second.messages.clear();
    it->second.message_bytes = 0;
    it->second.messages_dropped = false;
    return out;
}

std::optional<std::string> BrowserHost::take_message(std::uint32_t id) {
    auto it = tabs_.find(id);
    if (it == tabs_.end() || it->second.messages.empty()) return std::nullopt;
    std::string m = std::move(it->second.messages.front());
    it->second.messages.pop_front();
    it->second.message_bytes -= m.size();
    if (it->second.messages.empty()) it->second.messages_dropped = false;
    return m;
}

std::optional<Tab> BrowserHost::take_closed(std::uint32_t id) {
    auto it = tabs_.find(id);
    if (it == tabs_.end() || !it->second.closed) return std::nullopt;
    Tab t = std::move(it->second);
    tabs_.erase(it);
    return t;
}

void BrowserHost::shutdown() {
    if (transport_) {
        send(Shutdown{}, true);
        transport_->stop(kShutdownGraceMs);
        transport_.reset();
    }
    restart_at_.reset();
}

} // namespace hailstone::web
