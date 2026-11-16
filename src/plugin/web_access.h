#pragma once

// The in-game browser as the UI packs, plugins and scripts reach it (docs/browser.md): the app implements it over
// web::BrowserHost (app/web_service.h). Every call names its caller, an owner:
//   ""                  the player (the UI pack's own windows): any http(s) address, no messages from the pages
//   "script:<name>"     a Lua script or Lua plugin (mq.web)
//   "plugin:<name>"     a compiled plugin (the host table's web_*) or a plugin's window in the UI pack (hs.Web*)
// A caller sees and changes only its own tabs. One that is not the player needs kCapWeb in `caps`, and its addresses
// must be http(s) on a host the server's [web] allow names (core/web_url.h; the helper checks every navigation too).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hailstone::plugin {

struct WebState {
    std::string url, title;
    bool loading = false, can_back = false, can_forward = false;
    bool closed = false; // by the helper or the rules (why says which)
    std::string why;
};

class WebAccess {
public:
    virtual ~WebAccess() = default;

    // The helper is there (or not tried yet) and has not been given up.
    virtual bool available() = 0;
    // A new tab, 0 when it may not open (`why` says so).
    virtual std::uint32_t open(const std::string& owner, std::uint32_t caps, const std::string& url, int width,
                               int height, std::string* why = nullptr) = 0;
    virtual bool navigate(const std::string& owner, std::uint32_t caps, std::uint32_t tab, const std::string& url,
                          std::string* why = nullptr) = 0;
    // "back", "forward", "reload", "stop"; false for another word, a tab not the caller's, or (a script's or
    // plugin's) the helper's queue full: the player's own (owner "") always goes.
    virtual bool nav(const std::string& owner, std::uint32_t caps, std::uint32_t tab, const std::string& action) = 0;
    virtual void close(const std::string& owner, std::uint32_t tab) = 0;
    // Runs script in the page's main frame; posts to its window.hailstone.onmessage; types text where its cursor is.
    virtual bool eval(const std::string& owner, std::uint32_t caps, std::uint32_t tab, const std::string& script) = 0;
    virtual bool post(const std::string& owner, std::uint32_t caps, std::uint32_t tab, const std::string& message) = 0;
    virtual bool text(const std::string& owner, std::uint32_t caps, std::uint32_t tab, const std::string& text) = 0;
    // The page's next window.hailstone.post(); never for the player's tabs.
    virtual std::optional<std::string> take_message(const std::string& owner, std::uint32_t tab) = 0;
    // The next one's length, left queued; nullopt for none (as take_message).
    virtual std::optional<std::size_t> next_message_size(const std::string& owner, std::uint32_t tab) = 0;
    virtual std::optional<WebState> state(const std::string& owner, std::uint32_t tab) = 0;
    virtual std::vector<std::uint32_t> tabs(const std::string& owner) = 0;
    // Inside an ImGui window: draws the page width x height (the tab takes that size) and gives it the mouse, and the
    // keyboard once clicked. True while hovered.
    virtual bool view(const std::string& owner, std::uint32_t caps, std::uint32_t tab, float width, float height) = 0;
    // Closes every tab of an owner (a script that stopped, a plugin unloaded).
    virtual void close_owner(const std::string& owner) = 0;
};

} // namespace hailstone::plugin
