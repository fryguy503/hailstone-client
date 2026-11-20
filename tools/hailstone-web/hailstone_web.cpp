// hailstone-web: the in-game browser's helper (docs/browser.md). CEF's off-screen browser behind the messages of
// src/web/browser_protocol.h: the client's on standard input, ours on standard output. The same program is CEF's
// browser process (started by the client) and its other processes (renderers, GPU, utility: started by CEF).
//
//   hailstone-web --data=<folder>   cookies and caches go under it (each tab's profile is a folder in it); CEF's log
//                                   is <folder>/cef.log
//                 [--sandbox=auto|on|off]   Chromium's sandbox for the pages' processes (auto, the default: on Linux
//                                   where unprivileged user namespaces work; off elsewhere for now)
//
// Threads: a reader thread blocks on standard input and posts each message to CEF's UI thread, where everything else
// runs (CEF calls the off-screen handlers there), so only the UI thread writes standard output. Built with CEF's
// flags: no exceptions, no RTTI (the protocol's own code is the client's, in hailstone_web_protocol).

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_request_context.h"
#include "include/cef_request_context_handler.h"
#include "include/cef_resource_request_handler.h"
#include "include/cef_task.h"
#include "include/cef_v8.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include "core/web_url.h"
#include "platform/binary_stdio.h"
#include "web/browser_protocol.h"
#include "web/page_rules.h"

namespace web = hailstone::web;

namespace {

constexpr char kPostMessage[] = "hailstone.post";

// ---- output: the UI thread only ----

void send(const web::Message& m) {
    std::vector<std::byte> b = web::encode_message(m);
    std::fwrite(b.data(), 1, b.size(), stdout);
    std::fflush(stdout);
}

// A console line for the client, cut to what it takes (web::kMaxConsoleText).
void send_console(std::uint32_t tab, std::uint8_t level, std::string text) {
    web::cut_utf8(text, web::kMaxConsoleText);
    send(web::Console{tab, level, std::move(text)});
}

class Task final : public CefTask {
public:
    explicit Task(std::function<void()> f) : f_(std::move(f)) {}
    void Execute() override { f_(); }

private:
    std::function<void()> f_;
    IMPLEMENT_REFCOUNTING(Task);
};
void on_ui(std::function<void()> f) { CefPostTask(TID_UI, new Task(std::move(f))); }

// A JavaScript string literal for any text (the page's onmessage gets it as it was posted).
std::string js_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\x%02x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    // U+2028 and U+2029 end a line in older engines' string literals; escaped, whatever the page's engine.
    for (std::size_t at; (at = out.find("\xe2\x80\xa8")) != std::string::npos;) out.replace(at, 3, "\\u2028");
    for (std::size_t at; (at = out.find("\xe2\x80\xa9")) != std::string::npos;) out.replace(at, 3, "\\u2029");
    return out + "\"";
}

web::Cursor cursor_of(cef_cursor_type_t t) {
    switch (t) {
    case CT_HAND: return web::Cursor::Hand;
    case CT_IBEAM:
    case CT_VERTICALTEXT: return web::Cursor::Text;
    case CT_WAIT:
    case CT_PROGRESS: return web::Cursor::Wait;
    case CT_CROSS: return web::Cursor::Crosshair;
    case CT_EASTRESIZE:
    case CT_WESTRESIZE:
    case CT_EASTWESTRESIZE:
    case CT_COLUMNRESIZE: return web::Cursor::ResizeEW;
    case CT_NORTHRESIZE:
    case CT_SOUTHRESIZE:
    case CT_NORTHSOUTHRESIZE:
    case CT_ROWRESIZE: return web::Cursor::ResizeNS;
    case CT_MOVE: return web::Cursor::Move;
    case CT_NOTALLOWED:
    case CT_NODROP: return web::Cursor::NotAllowed;
    default: return web::Cursor::Arrow;
    }
}

std::uint8_t console_level(cef_log_severity_t level) {
    if (level >= LOGSEVERITY_ERROR) return 3;
    if (level == LOGSEVERITY_WARNING) return 2;
    if (level == LOGSEVERITY_INFO) return 1;
    return 0;
}

uint32_t event_flags(std::uint32_t mods) {
    uint32_t f = 0;
    if (mods & web::kModShift) f |= EVENTFLAG_SHIFT_DOWN;
    if (mods & web::kModCtrl) f |= EVENTFLAG_CONTROL_DOWN;
    if (mods & web::kModAlt) f |= EVENTFLAG_ALT_DOWN;
    if (mods & web::kModMeta) f |= EVENTFLAG_COMMAND_DOWN;
    if (mods & web::kModLeftButton) f |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (mods & web::kModMiddleButton) f |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    if (mods & web::kModRightButton) f |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    return f;
}

// ---- the browser process ----

class Helper;

// The requests of a script's or plugin's profile, or the server links' (its own request context, docs/browser.md
// "Profiles"), that come from no tab: a service worker's. Held to the same rules as its tabs' (web::tab_may_request).
// Called on CEF's IO thread.
class RestrictedContext final : public CefRequestContextHandler, public CefResourceRequestHandler {
public:
    RestrictedContext(web::TabRules rules, std::vector<std::string> allow) : rules_(rules), allow_(std::move(allow)) {}
    void set_allow(std::vector<std::string> allow) {
        std::lock_guard<std::mutex> lock(mutex_);
        allow_ = std::move(allow);
    }
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                                                   CefRefPtr<CefRequest>, bool, bool,
                                                                   const CefString&, bool&) override {
        return this;
    }
    ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request,
                                     CefRefPtr<CefCallback>) override {
        return allowed(request->GetURL().ToString()) ? RV_CONTINUE : RV_CANCEL;
    }
    void OnResourceRedirect(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>, CefRefPtr<CefResponse>,
                            CefString& new_url) override {
        if (!allowed(new_url.ToString())) new_url = "about:blank";
    }

private:
    bool allowed(const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex_);
        return web::tab_may_request(url, rules_, allow_);
    }
    const web::TabRules rules_;
    std::mutex mutex_;
    std::vector<std::string> allow_;
    IMPLEMENT_REFCOUNTING(RestrictedContext);
};

// One tab: its browser and the handlers CEF calls for it.
class TabClient final : public CefClient,
                        public CefRenderHandler,
                        public CefDisplayHandler,
                        public CefLoadHandler,
                        public CefLifeSpanHandler,
                        public CefRequestHandler,
                        public CefResourceRequestHandler {
public:
    TabClient(Helper& helper, const web::Open& o)
        : helper_(helper), tab_(o.tab), width_(o.width), height_(o.height), rules_(o.rules), allow_(o.allow) {}

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    CefRefPtr<CefBrowser> browser() const { return browser_; }
    std::uint32_t tab() const { return tab_; }
    void resize(std::uint16_t w, std::uint16_t h) {
        width_ = w;
        height_ = h;
        if (browser_) browser_->GetHost()->WasResized();
    }
    // What the client sends before CEF has made the browser waits for it.
    void when_created(std::function<void(CefBrowser&)> f) {
        if (browser_) f(*browser_);
        else waiting_.push_back(std::move(f));
    }
    // A main-frame address this tab may show: http(s) (about:blank and CEF's own error pages too); a server link's
    // tab never this computer or a private network, a script's or plugin's only its sites and never those either
    // (web/page_rules.h).
    bool may_show(const std::string& url) const {
        if (url.rfind("chrome-error://", 0) == 0) return true;
        return web::tab_may_show(url, rules_, allow_);
    }
    // What a frame inside the page, or any request the page makes, may reach.
    bool may_request(const std::string& url) const {
        return url.rfind("chrome-error://", 0) == 0 || web::tab_may_request(url, rules_, allow_);
    }
    bool held() const { return rules_ != web::TabRules::Player; }

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        rect = CefRect(0, 0, width_ ? width_ : 1, height_ ? height_ : 1);
    }
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirty, const void* buffer, int width,
                 int height) override {
        if (width <= 0 || height <= 0 || width > 0xffff || height > 0xffff) return;
        web::Frame f;
        f.tab = tab_;
        f.layer = type == PET_POPUP ? web::Layer::Popup : web::Layer::View;
        f.width = static_cast<std::uint16_t>(width);
        f.height = static_cast<std::uint16_t>(height);
        const auto* px = static_cast<const std::byte*>(buffer);
        const std::size_t stride = static_cast<std::size_t>(width) * 4;
        for (const CefRect& r : dirty) {
            const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
            const int x1 = std::min(width, r.x + r.width), y1 = std::min(height, r.y + r.height);
            if (x1 <= x0 || y1 <= y0) continue;
            web::FrameRect fr;
            fr.x = static_cast<std::uint16_t>(x0);
            fr.y = static_cast<std::uint16_t>(y0);
            fr.width = static_cast<std::uint16_t>(x1 - x0);
            fr.height = static_cast<std::uint16_t>(y1 - y0);
            const std::size_t row = std::size_t{fr.width} * 4;
            fr.bgra.resize(row * fr.height);
            for (std::size_t y = 0; y < std::size_t{fr.height}; ++y)
                std::memcpy(fr.bgra.data() + y * row, px + (y0 + y) * stride + static_cast<std::size_t>(x0) * 4, row);
            f.rects.push_back(std::move(fr));
        }
        // A full paint of the largest tab (4096x4096) is over kMaxMessage, which the client takes as a broken stream.
        if (!f.rects.empty())
            for (const web::Frame& part : web::split_frame(std::move(f))) send(part);
    }
    void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override {
        popup_shown_ = show;
        send(web::Popup{tab_, show, popup_.x, popup_.y, static_cast<std::uint16_t>(std::max(0, popup_.width)),
                        static_cast<std::uint16_t>(std::max(0, popup_.height))});
    }
    void OnPopupSize(CefRefPtr<CefBrowser> b, const CefRect& rect) override {
        popup_ = rect;
        if (popup_shown_) OnPopupShow(b, true);
    }

    // CefDisplayHandler
    void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, const CefString& url) override {
        if (frame->IsMain()) send(web::Address{tab_, url.ToString()});
    }
    void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override {
        send(web::Title{tab_, title.ToString()});
    }
    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level, const CefString& message,
                          const CefString& source, int line) override {
        send_console(tab_, console_level(level),
                     message.ToString() + " (" + source.ToString() + ":" + std::to_string(line) + ")");
        return true; // not into CEF's log as well
    }
    bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle, cef_cursor_type_t type,
                        const CefCursorInfo&) override {
        send(web::CursorShape{tab_, cursor_of(type)});
        return true;
    }

    // CefLoadHandler
    void OnLoadingStateChange(CefRefPtr<CefBrowser>, bool loading, bool back, bool forward) override {
        send(web::Loading{tab_, loading, back, forward});
    }
    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode code, const CefString& text,
                     const CefString& url) override {
        if (frame->IsMain() && code != ERR_ABORTED)
            send_console(tab_, 3, "could not load " + url.ToString() + ": " + text.ToString());
    }

    // CefRequestHandler: every navigation (links, redirects, scripts) meets the tab's rules: the main frame's always,
    // a frame's inside a server link's, script's or plugin's page too.
    bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool,
                        bool) override {
        const std::string url = request->GetURL().ToString();
        if (frame->IsMain() ? may_show(url) : may_request(url)) return false;
        send_console(tab_, 2, "refused " + url + ": not a page this tab may show");
        return true;
    }
    // A server link's, script's or plugin's page: each request it makes (images, scripts, styles, fetch, a form's
    // post, frames) meets the rules too, here on CEF's IO thread (allow_ and rules_ do not change after the tab is
    // made). The player's own tabs get CEF's default handling.
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                                                   CefRefPtr<CefRequest>, bool, bool,
                                                                   const CefString&, bool&) override {
        return held() ? this : nullptr;
    }
    ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request,
                                     CefRefPtr<CefCallback>) override {
        const std::string url = request->GetURL().ToString();
        if (may_request(url)) return RV_CONTINUE;
        const std::uint32_t tab = tab_;
        on_ui([tab, url] { send_console(tab, 2, "refused a request to " + url + ": not a site this tab may reach"); });
        return RV_CANCEL;
    }
    void OnResourceRedirect(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>, CefRefPtr<CefResponse>,
                            CefString& new_url) override {
        if (!may_request(new_url.ToString())) new_url = "about:blank"; // nothing loads from there
    }

    // CefLifeSpanHandler
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, int, const CefString& target_url,
                       const CefString&, CefLifeSpanHandler::WindowOpenDisposition, bool, const CefPopupFeatures&, CefWindowInfo&,
                       CefRefPtr<CefClient>&, CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) override {
        // No second window: the tab goes there itself (OnBeforeBrowse checks it).
        browser->GetMainFrame()->LoadURL(target_url);
        return true;
    }
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus status, int,
                                   const CefString& error) override;

    // CefClient: window.hailstone.post() from the page's renderer.
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefProcessId,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (message->GetName() != kPostMessage || !frame->IsMain()) return false;
        std::string text = message->GetArgumentList()->GetString(0).ToString();
        // The renderer refuses longer ones already (PostHandler); a page's process is not trusted to.
        if (text.size() > web::kMaxPageMessage)
            send_console(tab_, 2, "dropped a hailstone.post() message of " + std::to_string(text.size()) + " bytes");
        else
            send(web::PageMessage{tab_, std::move(text)});
        return true;
    }

    bool closed_by_client = false;
    web::CloseReason close_reason = web::CloseReason::Asked;
    std::string close_detail;

private:
    Helper& helper_;
    std::uint32_t tab_;
    std::uint16_t width_, height_;
    web::TabRules rules_;
    std::vector<std::string> allow_;
    CefRefPtr<CefBrowser> browser_;
    std::vector<std::function<void(CefBrowser&)>> waiting_;
    CefRect popup_;
    bool popup_shown_ = false;

    IMPLEMENT_REFCOUNTING(TabClient);
};

// The tabs and the client's messages, on the UI thread.
class Helper {
public:
    explicit Helper(std::string data) : data_(std::move(data)) {}

    void handle(web::Message m) {
        if (std::get_if<web::Hello>(&m)) {
            send(web::Ready{});
        } else if (auto* o = std::get_if<web::Open>(&m)) {
            open(*o);
        } else if (auto* n = std::get_if<web::Navigate>(&m)) {
            with(n->tab, [url = n->url](CefBrowser& b) { b.GetMainFrame()->LoadURL(url); });
        } else if (auto* r = std::get_if<web::Resize>(&m)) {
            if (auto* t = find(r->tab)) t->resize(r->width, r->height);
        } else if (auto* mo = std::get_if<web::Mouse>(&m)) {
            mouse(*mo);
        } else if (auto* w = std::get_if<web::Wheel>(&m)) {
            CefMouseEvent ev;
            ev.x = w->x;
            ev.y = w->y;
            ev.modifiers = event_flags(w->mods);
            const int dx = w->dx, dy = w->dy;
            with(w->tab, [ev, dx, dy](CefBrowser& b) { b.GetHost()->SendMouseWheelEvent(ev, dx, dy); });
        } else if (auto* k = std::get_if<web::Key>(&m)) {
            CefKeyEvent ev;
            ev.type = k->kind == web::KeyKind::Down ? KEYEVENT_RAWKEYDOWN
                      : k->kind == web::KeyKind::Up ? KEYEVENT_KEYUP
                                                    : KEYEVENT_CHAR;
            ev.windows_key_code = static_cast<int>(k->kind == web::KeyKind::Char ? k->character : k->code);
            ev.character = ev.unmodified_character = static_cast<char16_t>(k->character);
            ev.modifiers = event_flags(k->mods);
            with(k->tab, [ev](CefBrowser& b) { b.GetHost()->SendKeyEvent(ev); });
        } else if (auto* t = std::get_if<web::Text>(&m)) {
            with(t->tab, [text = t->text](CefBrowser& b) {
                b.GetHost()->ImeCommitText(text, CefRange::InvalidRange(), 0);
            });
        } else if (auto* f = std::get_if<web::Focus>(&m)) {
            with(f->tab, [on = f->on](CefBrowser& b) { b.GetHost()->SetFocus(on); });
        } else if (auto* nav = std::get_if<web::Nav>(&m)) {
            with(nav->tab, [a = nav->action](CefBrowser& b) {
                switch (a) {
                case web::NavAction::Back: b.GoBack(); break;
                case web::NavAction::Forward: b.GoForward(); break;
                case web::NavAction::Reload: b.Reload(); break;
                case web::NavAction::Stop: b.StopLoad(); break;
                }
            });
        } else if (auto* e = std::get_if<web::Eval>(&m)) {
            with(e->tab, [script = e->script](CefBrowser& b) {
                CefRefPtr<CefFrame> fr = b.GetMainFrame();
                fr->ExecuteJavaScript(script, fr->GetURL(), 0);
            });
        } else if (auto* p = std::get_if<web::Post>(&m)) {
            const std::string js = "(function(m){var h=window.hailstone;if(h&&typeof h.onmessage==='function')"
                                   "h.onmessage(m);})(" + js_string(p->message) + ");";
            with(p->tab, [js](CefBrowser& b) {
                CefRefPtr<CefFrame> fr = b.GetMainFrame();
                fr->ExecuteJavaScript(js, fr->GetURL(), 0);
            });
        } else if (auto* c = std::get_if<web::Close>(&m)) {
            if (auto* tab = find(c->tab)) {
                tab->closed_by_client = true;
                tab->when_created([](CefBrowser& b) { b.GetHost()->CloseBrowser(true); });
            }
        } else if (std::get_if<web::Shutdown>(&m)) {
            shutdown();
        }
        // The helper's own messages coming in are ignored.
    }

    void shutdown() {
        quitting_ = true;
        if (tabs_.empty()) {
            CefQuitMessageLoop();
            return;
        }
        // A copy: CloseBrowser can close at once, and OnBeforeClose takes the tab out of tabs_.
        std::vector<CefRefPtr<TabClient>> open;
        for (auto& [id, t] : tabs_) open.push_back(t);
        for (auto& t : open) {
            t->closed_by_client = true;
            t->when_created([](CefBrowser& b) { b.GetHost()->CloseBrowser(true); });
        }
    }

    void created(TabClient&) {}
    // Before CefShutdown: CEF's objects may not be released after it.
    void release() {
        tabs_.clear();
        contexts_.clear();
    }
    void closed(TabClient& t) {
        if (!t.closed_by_client) send(web::Closed{t.tab(), t.close_reason, t.close_detail});
        tabs_.erase(t.tab());
        if (quitting_ && tabs_.empty()) CefQuitMessageLoop();
    }

private:
    TabClient* find(std::uint32_t tab) {
        auto it = tabs_.find(tab);
        return it == tabs_.end() ? nullptr : it->second.get();
    }
    void with(std::uint32_t tab, std::function<void(CefBrowser&)> f) {
        if (auto* t = find(tab)) t->when_created(std::move(f));
    }

    // A profile's request context: its own cookies and cache, in a folder under --data (CEF requires that). The
    // client gives each script and plugin a profile of its own, and the server links one (app/web_service.h); a
    // context made for such a tab holds the requests that come from no tab (service workers) to that tab's rules.
    CefRefPtr<CefRequestContext> context(const web::Open& o) {
        const std::string& profile = o.profile;
        if (profile.empty() || profile.rfind(data_, 0) != 0) return CefRequestContext::GetGlobalContext();
        auto it = contexts_.find(profile);
        if (it != contexts_.end()) {
            if (it->second.rules && o.rules == web::TabRules::Sites) it->second.rules->set_allow(o.allow);
            return it->second.context;
        }
        CefRequestContextSettings s;
        CefString(&s.cache_path) = profile;
        CefRefPtr<RestrictedContext> rules =
            o.rules != web::TabRules::Player ? new RestrictedContext(o.rules, o.allow) : nullptr;
        CefRefPtr<CefRequestContext> c = CefRequestContext::CreateContext(s, rules.get());
        contexts_.emplace(profile, Context{c, rules});
        return c;
    }

    void open(const web::Open& o) {
        if (quitting_ || find(o.tab)) return;
        CefRefPtr<TabClient> client = new TabClient(*this, o);
        if (!client->may_show(o.url)) {
            send(web::Closed{o.tab, web::CloseReason::Refused, o.url + " is not a page this tab may show"});
            return;
        }
        CefWindowInfo info;
        info.SetAsWindowless(0);
        CefBrowserSettings settings;
        settings.windowless_frame_rate = 30;
        tabs_.emplace(o.tab, client);
        if (!CefBrowserHost::CreateBrowser(info, client, o.url, settings, nullptr, context(o))) {
            tabs_.erase(o.tab);
            send(web::Closed{o.tab, web::CloseReason::Failed, "CEF could not make the browser"});
        }
    }

    void mouse(const web::Mouse& m) {
        CefMouseEvent ev;
        ev.x = m.x;
        ev.y = m.y;
        ev.modifiers = event_flags(m.mods);
        const cef_mouse_button_type_t button = m.button == web::MouseButton::Middle  ? MBT_MIDDLE
                                               : m.button == web::MouseButton::Right ? MBT_RIGHT
                                                                                     : MBT_LEFT;
        const web::MouseKind kind = m.kind;
        const int clicks = m.clicks ? m.clicks : 1;
        with(m.tab, [ev, button, kind, clicks](CefBrowser& b) {
            CefRefPtr<CefBrowserHost> h = b.GetHost();
            switch (kind) {
            case web::MouseKind::Move: h->SendMouseMoveEvent(ev, false); break;
            case web::MouseKind::Leave: h->SendMouseMoveEvent(ev, true); break;
            case web::MouseKind::Down: h->SendMouseClickEvent(ev, button, false, clicks); break;
            case web::MouseKind::Up: h->SendMouseClickEvent(ev, button, true, clicks); break;
            }
        });
    }

    std::string data_;
    std::map<std::uint32_t, CefRefPtr<TabClient>> tabs_;
    struct Context {
        CefRefPtr<CefRequestContext> context;
        CefRefPtr<RestrictedContext> rules; // a script's or plugin's profile
    };
    std::map<std::string, Context> contexts_;
    bool quitting_ = false;
};

void TabClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    browser_ = browser;
    for (auto& f : std::exchange(waiting_, {})) f(*browser_);
    helper_.created(*this);
}

void TabClient::OnBeforeClose(CefRefPtr<CefBrowser>) {
    CefRefPtr<TabClient> keep(this); // the helper's map holds the last reference
    browser_ = nullptr;
    helper_.closed(*this);
}

void TabClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus, int,
                                          const CefString& error) {
    close_reason = web::CloseReason::Crashed;
    close_detail = "the page's process ended: " + error.ToString();
    browser->GetHost()->CloseBrowser(true);
}

// ---- every process: the command line, and window.hailstone in the renderers ----

class PostHandler final : public CefV8Handler {
public:
    bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& args, CefRefPtr<CefV8Value>&,
                 CefString& exception) override {
        if (args.size() != 1 || !args[0]->IsString()) {
            exception = "hailstone.post takes one string";
            return true;
        }
        if (args[0]->GetStringValue().ToString().size() > web::kMaxPageMessage) {
            exception = "hailstone.post: a message is at most " + std::to_string(web::kMaxPageMessage) + " bytes";
            return true;
        }
        CefRefPtr<CefV8Context> ctx = CefV8Context::GetCurrentContext();
        CefRefPtr<CefFrame> frame = ctx ? ctx->GetFrame() : nullptr;
        if (!frame || !frame->IsMain()) return true; // frames inside the page cannot post
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create(kPostMessage);
        msg->GetArgumentList()->SetString(0, args[0]->GetStringValue());
        frame->SendProcessMessage(PID_BROWSER, msg);
        return true;
    }

private:
    IMPLEMENT_REFCOUNTING(PostHandler);
};

class App final : public CefApp, public CefRenderProcessHandler {
public:
    void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> line) override {
        if (!process_type.empty()) return;
        // Software rendering: the pixels come back to the client through OnPaint anyway.
        line->AppendSwitch("disable-gpu");
        line->AppendSwitch("disable-gpu-compositing");
        line->AppendSwitch("disable-extensions");
        line->AppendSwitch("mute-audio");
#if defined(__linux__)
        // No display (a CI runner, a session without X or Wayland): Chromium's headless platform. With one, the
        // default keeps the system's clipboard and keyboard layout for the pages.
        if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
            line->AppendSwitchWithValue("ozone-platform", "headless");
#endif
    }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    // window.hailstone = { post(text), onmessage: null } in each page's main frame.
    void OnContextCreated(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> ctx) override {
        if (!frame->IsMain()) return;
        CefRefPtr<CefV8Value> obj = CefV8Value::CreateObject(nullptr, nullptr);
        obj->SetValue("post", CefV8Value::CreateFunction("post", new PostHandler), V8_PROPERTY_ATTRIBUTE_READONLY);
        obj->SetValue("onmessage", CefV8Value::CreateNull(), V8_PROPERTY_ATTRIBUTE_NONE);
        ctx->GetGlobal()->SetValue("hailstone", obj, V8_PROPERTY_ATTRIBUTE_READONLY);
    }

private:
    IMPLEMENT_REFCOUNTING(App);
};

std::string arg_value(const std::vector<std::string>& args, const std::string& name) {
    for (std::size_t i = 1; i < args.size(); ++i)
        if (args[i].size() > name.size() && args[i].compare(0, name.size(), name) == 0 && args[i][name.size()] == '=')
            return args[i].substr(name.size() + 1);
    return {};
}

// Whether Chromium's sandbox can run: on Linux through unprivileged user namespaces (the setuid chrome-sandbox is not
// shipped), which a kernel switch, a zero namespace limit or Ubuntu's AppArmor rule can keep from a program like this
// one; Chromium would then exit with "No usable sandbox". Windows and macOS need CEF's sandbox library and helper
// bundles, not set up yet (docs/browser.md).
bool sandbox_possible() {
#if defined(__linux__)
    auto read_int = [](const char* path, int fallback) {
        std::ifstream f(path);
        int v = fallback;
        return f >> v ? v : fallback;
    };
    if (read_int("/proc/sys/kernel/unprivileged_userns_clone", 1) == 0) return false;
    if (read_int("/proc/sys/user/max_user_namespaces", 1) == 0) return false;
    if (read_int("/proc/sys/kernel/apparmor_restrict_unprivileged_userns", 0) == 1) return false;
    return true;
#else
    return false;
#endif
}

// False once the message loop has ended: the reader posts nothing to a CEF that is shutting down.
std::atomic<bool> g_running{true};

int run(const CefMainArgs& main_args, const std::vector<std::string>& args) {
    CefRefPtr<App> app = new App;
    // CEF's other processes are this program too: they run here and end.
    const int code = CefExecuteProcess(main_args, app, nullptr);
    if (code >= 0) return code;

    hailstone::platform::binary_stdio();
    std::string data = arg_value(args, "--data");
    if (data.empty()) {
        std::fprintf(stderr, "hailstone-web: --data=<folder> is required\n");
        return 2;
    }
    while (data.size() > 1 && (data.back() == '/' || data.back() == '\\')) data.pop_back();

    const std::string sandbox = arg_value(args, "--sandbox");
    const bool sandboxed = sandbox == "on" ? true : sandbox == "off" ? false : sandbox_possible();
    if (!sandboxed) std::fprintf(stderr, "hailstone-web: running without Chromium's sandbox\n");

    CefSettings settings;
    settings.windowless_rendering_enabled = true;
    settings.no_sandbox = !sandboxed;
    settings.multi_threaded_message_loop = false;
    settings.persist_session_cookies = true;
    settings.log_severity = LOGSEVERITY_WARNING;
    CefString(&settings.root_cache_path) = data;
    CefString(&settings.log_file) = data + "/cef.log";
    CefString(&settings.user_agent_product) = "Hailstone";
    if (!CefInitialize(main_args, settings, app, nullptr)) return CefGetExitCode();

    Helper helper(data);
    // The reader: whole messages to the UI thread; the end of input (the client gone) shuts the helper down.
    std::thread reader([&helper] {
        for (;;) {
            unsigned char len_bytes[4];
            if (std::fread(len_bytes, 1, 4, stdin) != 4) break;
            const std::uint32_t len = std::uint32_t{len_bytes[0]} | std::uint32_t{len_bytes[1]} << 8 |
                                      std::uint32_t{len_bytes[2]} << 16 | std::uint32_t{len_bytes[3]} << 24;
            if (len == 0 || len > web::kMaxMessage) break;
            std::vector<std::byte> body(len);
            if (std::fread(body.data(), 1, len, stdin) != len) break;
            auto m = web::decode_message(body);
            if (!m) break;
            if (!g_running) return;
            on_ui([&helper, msg = std::move(*m)]() mutable { helper.handle(std::move(msg)); });
        }
        if (g_running) on_ui([&helper] { helper.shutdown(); });
    });
    reader.detach(); // it may still be blocked on standard input when CEF has shut down

    CefRunMessageLoop();
    g_running = false;
    helper.release();
    CefShutdown();
    return 0;
}

} // namespace

#if defined(_WIN32)
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    std::vector<std::string> args;
    for (int i = 0; i < __argc; ++i) args.push_back(CefString(__wargv[i]).ToString()); // UTF-16 to UTF-8
    return run(CefMainArgs(instance), args);
}
#else
int main(int argc, char** argv) { return run(CefMainArgs(argc, argv), std::vector<std::string>(argv, argv + argc)); }
#endif
