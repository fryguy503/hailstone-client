// The help window: HelpWindow (CHelpWnd) over the game's help\ pages, and CStmlWnd's page loading for the windows
// that show a page of the game's (the help and tip windows).

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "app/lua_ui_windows.h"
#include "core/game_dir.h"

namespace hailstone::app {

std::string load_stml_page(const GameDir* game, const std::string& path, std::string* title) {
    // CStmlWnd::LoadPage: the page's text; its <HEAD> (ParseSTMLHead: the <TITLE>) is kept apart.
    if (title) title->clear();
    std::string html;
    if (game)
        if (auto data = game->read(path)) html.assign(reinterpret_cast<const char*>(data->data()), data->size());
    auto lower = html;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (auto t0 = lower.find("<title>"); t0 != std::string::npos && title)
        if (auto t1 = lower.find("</title>", t0); t1 != std::string::npos) *title = html.substr(t0 + 7, t1 - t0 - 7);
    if (auto h1 = lower.find("</head>"); h1 != std::string::npos) {
        html.erase(0, h1 + 7);
        lower.erase(0, h1 + 7);
    }
    if (auto b0 = lower.find("<body"); b0 != std::string::npos)
        if (auto b1 = lower.find('>', b0); b1 != std::string::npos) html.erase(b0, b1 - b0 + 1);
    for (char& c : html)
        if (c == '\n' || c == '\r') c = ' ';
    return html;
}

std::optional<std::string> stml_href_path(const std::string& page, std::string_view href) {
    std::string target(href);
    if (target.rfind("file:///", 0) == 0) {
        target = target.substr(8);
    } else if (target.find(':') != std::string::npos) {
        return std::nullopt; // a web page: not followed here
    } else if (auto slash = page.find_last_of('/'); slash != std::string::npos) {
        target = page.substr(0, slash + 1) + target;
    }
    for (char& c : target)
        if (c == '\\') c = '/';
    return target;
}

namespace {

// CHelpWnd @0x54e8a0: children Display (an STML box), BackButton,
// CustomerServiceButton, TipsButton and Contents; SetFile @0x54e670 loads __HelpPath ("help\") + the file into Display
// (CStmlWnd::LoadPage without history) and the constructor shows help.html. WndNotification @0x54e740: Contents shows
// help.html again; BackButton goes back a page (CStmlWnd::GoToBackHistoryLink) and enables itself while there is
// more history (CanGoBackward); TipsButton shows the tip of the day window (CTipWnd::Activate(pinstCTipWnd_1, -1,
// force)); CustomerServiceButton runs /petition with no text. Display's notification 0xe (a link followed inside the
// page, which CStmlWnd puts in its history) sets BackButton the same way. The links are the pages' href targets:
// file:///help/... from the game folder, a bare name beside the page; the web links are not followed.
class HelpWindow : public StockWindow {
public:
    HelpWindow() : StockWindow("HelpWindow", false) {}

    void child(UiHost& host, std::string_view id, Child& out) override {
        if (id == "Display") {
            if (page_.empty()) set_file(host, "help.html");
            out.text = text_;
        } else if (id == "BackButton" && back_enabled_) {
            out.enabled = *back_enabled_;
        }
    }
    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (button != "left") return false;
        if (id == "Contents") {
            set_file(host, "help.html");
        } else if (id == "BackButton") {
            if (!history_.empty()) {
                load(host, history_.back());
                history_.pop_back();
            }
            back_enabled_ = !history_.empty();
        } else if (id == "TipsButton") {
            if (host.show_tip_of_day) host.show_tip_of_day();
        } else if (id == "CustomerServiceButton") {
            if (host.submit_chat) host.submit_chat("/petition");
        } else {
            return false;
        }
        return true;
    }
    bool stml_link(UiHost& host, std::string_view id, int message, std::string_view data) override {
        if (id != "Display" || message != -1) return false;
        auto target = stml_href_path(page_, data);
        if (!target) return true;
        history_.push_back(page_);
        // Kept to the last kMaxHistory pages: a long session of links would otherwise grow it without end.
        if (history_.size() > kMaxHistory) history_.erase(history_.begin());
        load(host, *target);
        back_enabled_ = !history_.empty();
        return true;
    }
    // SetFile(page) and Activate (a tip page's WndNotify 1000 link).
    void set_file(UiHost& host, const std::string& file) { load(host, "help/" + file); }

private:
    void load(UiHost& host, const std::string& path) {
        page_ = path;
        text_ = load_stml_page(host.game, path, nullptr);
    }

    static constexpr std::size_t kMaxHistory = 64; // Hailstone's; how far back CStmlWnd's history goes is not known
    std::string page_, text_;
    std::vector<std::string> history_;
    std::optional<bool> back_enabled_; // none: the XML's state, until the first link or Back
};

} // namespace

void add_help_windows(std::vector<std::unique_ptr<StockWindow>>& out) { out.push_back(std::make_unique<HelpWindow>()); }

void help_window_set_file(StockWindow& w, UiHost& host, const std::string& file) {
    if (auto* h = dynamic_cast<HelpWindow*>(&w)) h->set_file(host, file);
}

} // namespace hailstone::app
