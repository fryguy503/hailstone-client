#pragma once

// The client's side of the browser helper (docs/browser.md): the tabs, what each one shows, and the helper itself,
// started on the first tab, restarted when it dies (each tab reopened at its last address), given up after too many
// deaths. Scripts' and plugins' tabs are held to the address rules (core/web_url.h) here and, at every navigation,
// by the helper; a restricted tab that ends up elsewhere anyway is closed.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "web/browser_protocol.h"

namespace hailstone::web {

// The pipe to the helper (ProcessTransport; tests use their own).
class Transport {
public:
    virtual ~Transport() = default;
    virtual bool start(std::string& error) = 0;
    virtual void send(std::span<const std::byte> bytes) = 0;
    virtual std::size_t receive(std::span<std::byte> out) = 0; // without waiting
    virtual bool alive() = 0;
    // Bytes sent and not yet taken by the helper, and how many it has taken in all (what BrowserHost holds back for,
    // and how it tells a slow helper from a stuck one).
    virtual std::size_t pending() { return 0; }
    virtual std::uint64_t taken() { return 0; }
    // Ends it: `grace_ms` > 0 gives it that long to quit by itself after the end of its input (CEF writes its cookies
    // as it shuts down), 0 ends it at once.
    virtual void stop(int grace_ms) = 0;
};

// The helper program, args[0] its path (platform/helper_process.h).
std::unique_ptr<Transport> process_transport(std::vector<std::string> args);

struct TabOptions {
    std::uint16_t width = 800, height = 600;
    // "" for the player's own tab; else the script or plugin that opened it (it gets the page's messages), held to
    // `allow` (empty: any host).
    std::string owner;
    std::vector<std::string> allow;
    std::string profile; // the cookie and cache folder
    // The player's tab a server's link opened (OP_Weblink, WebService::open_server_link): any public site, never
    // this computer or a private network, at every navigation and request (web::TabRules::ServerLink).
    bool server_link = false;
};

// The rules a tab is held to (web/page_rules.h).
TabRules tab_rules(const TabOptions& o);

struct Image {
    std::uint16_t width = 0, height = 0;
    std::vector<std::byte> bgra;  // width * height * 4
    std::uint32_t serial = 0;     // grows with each change
    struct Rect {
        std::uint16_t x = 0, y = 0, width = 0, height = 0;
    };
    std::vector<Rect> dirty;      // changed since take_dirty()
};

struct Tab {
    std::uint32_t id = 0;
    TabOptions options;
    std::string url, title;
    bool loading = false, can_back = false, can_forward = false;
    Cursor cursor = Cursor::Arrow;
    Image view, popup;
    bool popup_shown = false;
    std::int32_t popup_x = 0, popup_y = 0;
    std::deque<std::string> messages; // from the page, oldest first
    std::size_t message_bytes = 0;    // the messages' bytes in all
    bool messages_dropped = false;    // since the queue was last empty (logged once)
    std::deque<Console> console;      // the last kConsoleLines lines, each at most kMaxConsoleText bytes
    bool closed = false;
    CloseReason close_reason = CloseReason::Asked;
    std::string close_detail;
};

class BrowserHost {
public:
    static constexpr std::size_t kConsoleLines = 200;
    static constexpr std::size_t kMessageQueue = 256;  // a page posting more than its owner takes loses the oldest
    static constexpr std::size_t kMessageQueueBytes = std::size_t{8} << 20; // and no more than this in all
    static constexpr int kMaxRestarts = 3;             // deaths within kRestartWindowMs before giving up
    static constexpr std::uint64_t kRestartWindowMs = 60'000;
    static constexpr std::uint64_t kRestartDelayMs = 1'000;
    static constexpr int kShutdownGraceMs = 3'000; // shutdown(): the helper's time to quit by itself
    // A message (other than the client's own Hello, Close, Shutdown and a restart's reopening of its tabs) is refused
    // while this much already waits for the helper, so a script's traffic is dropped instead of filling the queue
    // to HelperProcess::kMaxPending and ending a helper that is still reading. An empty queue takes any message.
    static constexpr std::size_t kSendBudget = std::size_t{32} << 20;
    // A helper with bytes waiting that takes none of them this long has stopped reading: ended and restarted.
    static constexpr std::uint64_t kStallMs = 10'000;

    explicit BrowserHost(std::function<std::unique_ptr<Transport>()> make_transport);
    ~BrowserHost();
    BrowserHost(const BrowserHost&) = delete;
    BrowserHost& operator=(const BrowserHost&) = delete;

    // A new tab, 0 when it may not open (`why` says so): not an http(s) address, a host outside its allow list, an
    // address longer than kMaxUrl, no helper, or the helper's queue full (kSendBudget).
    std::uint32_t open(const std::string& url, TabOptions options, std::string* why = nullptr);
    bool navigate(std::uint32_t tab, const std::string& url, std::string* why = nullptr);
    void resize(std::uint32_t tab, std::uint16_t width, std::uint16_t height);
    void mouse(const Mouse& m);
    void wheel(const Wheel& w);
    void key(const Key& k);
    // False for a tab not live, text too long to send (kMaxMessage; a Post kMaxPostMessage), since the helper quits
    // on a message over its limit, or the helper's queue full (kSendBudget): never sent.
    bool text(std::uint32_t tab, const std::string& text);
    void focus(std::uint32_t tab, bool on);
    // `player`: the player's own toolbar, sent whatever the queue holds as their other input is; a script's or
    // plugin's is held to kSendBudget like its other messages. False when not sent.
    bool nav(std::uint32_t tab, NavAction action, bool player);
    bool eval(std::uint32_t tab, const std::string& script);
    bool post(std::uint32_t tab, const std::string& message);
    // Closes and forgets it (the tabs the helper or the rules close stay until take_closed).
    void close(std::uint32_t tab);
    // Closes a live tab as Refused, `detail` saying why: it stays (closed) until its owner takes it (take_closed).
    void refuse(std::uint32_t tab, std::string detail);

    // Reads what the helper sent and restarts a dead one; once a frame. `now_ms` is any steady clock.
    void poll(std::uint64_t now_ms);

    const Tab* tab(std::uint32_t id) const;
    // The parts of a tab's image changed since the last call (the texture copies them), and clears the list.
    std::vector<Image::Rect> take_dirty(std::uint32_t tab, Layer layer);
    std::vector<std::uint32_t> tabs() const;
    std::vector<std::string> take_messages(std::uint32_t tab);
    std::optional<std::string> take_message(std::uint32_t tab); // the oldest
    // A closed tab's last state, then forgotten.
    std::optional<Tab> take_closed(std::uint32_t tab);
    // The helper is running or can still be started.
    bool available() const { return !gave_up_; }
    const std::string& last_error() const { return last_error_; }
    void shutdown();

private:
    bool ensure_started();
    // False (logged, `why` set): too long to send, or the queue full and not `always`.
    bool send(const Message& m, bool always = false, std::string* why = nullptr);
    void handle(Message m);
    void helper_died(std::uint64_t now_ms, const std::string& why);
    void close_tab(Tab& t, CloseReason reason, std::string detail);
    void queue_message(Tab& t, std::string message);
    Tab* live(std::uint32_t id);
    Open open_message(const Tab& t) const;

    std::function<std::unique_ptr<Transport>()> make_transport_;
    std::unique_ptr<Transport> transport_;
    MessageReader reader_;
    std::map<std::uint32_t, Tab> tabs_;
    std::uint32_t next_id_ = 1;
    bool ready_ = false;
    bool gave_up_ = false;
    std::deque<std::uint64_t> deaths_;
    std::optional<std::uint64_t> restart_at_;
    std::uint64_t taken_ = 0;                 // the helper's taken() at the last poll
    std::optional<std::uint64_t> stall_since_; // since when bytes wait and none are taken
    bool refusing_ = false;                   // a full queue's refusal is logged (once until a send goes)
    std::string last_error_;
};

} // namespace hailstone::web
