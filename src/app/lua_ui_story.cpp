// The story window: StoryWnd (CStoryWnd) over the game's storyline\story*.txt pages.

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "app/lua_ui_windows.h"
#include "core/file_time.h"
#include "core/game_dir.h"

namespace hailstone::app {

namespace {

// CStoryWnd @0x5c3c40: StoryList (Date 160, Title 200), Display (an STML box),
// AutomaticButton (a check box) and CloseButton. Init @0x5c3820 reads every storyline\story*.txt: its first line (to
// the new line, at most 0x7ff) is the title, its file time (_stat32's st_mtime) the date; the stories are sorted
// newest first (0x5c3190) and listed as ctime(date) and title. LoadIniInfo @0x5c3420 reads eqclient.ini [News]
// LastRead ("%08X", the newest date read) and Automatic (here hailstone.ini news.last_read and news.automatic, the
// "news_last_read" and "news_automatic" options). AboutToShow selects the oldest story newer than LastRead
// (SelectOldestNew @0x5c3510; none: no row, an empty page); SelectIndex @0x5c3290 shows the story's page
// (CStmlWnd::LoadPage of the file) and moves LastRead up to its date (SaveIni). Automatic writes its state; Close
// hides the window. CDisplay::InitGameUI shows the window when there is a story newer than LastRead, Automatic is on
// and the character is above level 1: UiApi's show_story_if_new. Display is a rich-text control, which
// follows a page's href links itself (as in the help window: file:///storyline/... from the game folder, a bare name
// beside the page, web links not followed); the story window has no Back button, so the pages' own links lead back.
class StoryWindow : public StockWindow {
public:
    StoryWindow() : StockWindow("StoryWnd", false) {
        list_.columns = 2;
        list_.labels = {"Date", "Title"};
        list_.widths = {160, 200};
    }

    void set_shown(UiHost& host, bool show) override {
        if (show && !open_) {
            init(host);
            select_oldest_new(host); // AboutToShow
        }
        open_ = show;
    }
    // ShowAuto: a story newer than LastRead, and Automatic.
    bool activate(UiHost& host, int) override {
        init(host);
        if (newest_ <= last_read(host) || !automatic(host)) return false;
        set_shown(host, true);
        return true;
    }
    void child(UiHost& host, std::string_view id, Child& out) override {
        if (id == "Display") out.text = text_;
        else if (id == "AutomaticButton") out.checked = automatic(host);
    }
    bool stml_link(UiHost& host, std::string_view id, int message, std::string_view data) override {
        if (id != "Display" || message != -1) return false;
        if (auto target = stml_href_path(page_, data)) {
            page_ = *target;
            text_ = load_stml_page(host.game, page_, nullptr);
        }
        return true;
    }
    const ListData* list(UiHost&, std::string_view id) override { return id == "StoryList" ? &list_ : nullptr; }
    bool list_click(UiHost& host, std::string_view id, int row, bool, int) override {
        if (id != "StoryList") return false;
        select(host, row);
        return true;
    }
    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (button != "left") return false;
        if (id == "AutomaticButton") {
            if (host.set_option) host.set_option("news_automatic", automatic(host) ? 0 : 1);
        } else if (id == "CloseButton") {
            open_ = false;
        } else {
            return false;
        }
        return true;
    }

private:
    struct Story {
        std::string path, title;
        std::int64_t time = 0;
    };
    static bool automatic(UiHost& host) { return host.find_option && host.find_option("news_automatic").value_or(0) != 0; }
    static std::int64_t last_read(UiHost& host) {
        return host.find_option ? static_cast<std::uint32_t>(host.find_option("news_last_read").value_or(0)) : 0;
    }

    void init(UiHost& host) {
        stories_.clear();
        newest_ = 0;
        if (!host.game) return;
        for (const auto& e : host.game->list("storyline")) {
            std::string lower = e.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.rfind("story", 0) != 0 || lower.size() < 9 || lower.substr(lower.size() - 4) != ".txt") continue;
            Story s;
            s.path = e.relative;
            if (auto data = host.game->read(e.relative)) {
                std::string text(reinterpret_cast<const char*>(data->data()), data->size());
                s.title = text.substr(0, std::min<std::size_t>(text.find('\n'), 0x7ff));
                if (!s.title.empty() && s.title.back() == '\r') s.title.pop_back();
            }
            std::error_code ec;
            auto t = std::filesystem::last_write_time(e.path, ec);
            if (!ec)
                s.time = std::chrono::duration_cast<std::chrono::seconds>(file_time_to_system(t).time_since_epoch()).count();
            newest_ = std::max(newest_, s.time);
            stories_.push_back(std::move(s));
        }
        std::stable_sort(stories_.begin(), stories_.end(), [](const Story& a, const Story& b) { return a.time > b.time; });
        list_.rows.clear();
        list_.selected = -1;
        for (const auto& s : stories_) {
            std::time_t t = static_cast<std::time_t>(s.time);
            char buf[64] = {};
            std::strftime(buf, sizeof buf, "%a %b %e %H:%M:%S %Y", std::localtime(&t)); // ctime without its new line
            list_.rows.push_back(ListRow{{buf, s.title}});
        }
    }
    void select_oldest_new(UiHost& host) {
        int row = -1;
        std::int64_t read = last_read(host);
        for (int i = static_cast<int>(stories_.size()) - 1; i >= 0; --i)
            if (stories_[static_cast<std::size_t>(i)].time > read) {
                row = i;
                break;
            }
        select(host, row);
    }
    void select(UiHost& host, int row) {
        list_.selected = row;
        if (row < 0 || row >= static_cast<int>(stories_.size())) {
            page_.clear();
            text_.clear();
            return;
        }
        const Story& s = stories_[static_cast<std::size_t>(row)];
        if (last_read(host) <= s.time && host.set_option) host.set_option("news_last_read", static_cast<int>(s.time));
        page_ = s.path;
        text_ = load_stml_page(host.game, page_, nullptr);
    }

    std::vector<Story> stories_;
    std::int64_t newest_ = 0;
    ListData list_;
    std::string page_, text_; // the page shown (a story, or a page one of its links led to) and its text
};

} // namespace

void add_story_windows(std::vector<std::unique_ptr<StockWindow>>& out) { out.push_back(std::make_unique<StoryWindow>()); }

} // namespace hailstone::app
