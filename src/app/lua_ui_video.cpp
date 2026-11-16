// The video modes window: VideoModesWindow (CVideoModesWnd) over this client's display modes (the "resolution",
// "window_mode" options, app/client_app_options.cpp).

#include <cstdio>
#include <string>
#include <vector>

#include "app/lua_ui_windows.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

// CVideoModesWnd @0x78a6c0: VIDW_VideoResolutionsList (one column
// "Resolution"), VIDW_RefreshRateCombobox, VIDW_BitDepthCombobox and VIDW_AcceptButton. Update @0x789a50: the render
// interface's modes of at least 640 x 480 (in a window: no larger than the desktop) in 16 or 32 bit formats, each
// width x height once ("%dx%d"), the current one selected; in full screen the refresh rates of the current one
// ("%1Hz" 6628, 0: "Default" 6629) and its bit depths (32 bit 6627, 16 bit 6625 / 6626) fill the two combos, the
// current ones chosen. A click on a row refills the combos for it (sub_0078a090, full screen only). Accept
// (WndNotification @0x78a370): not while the give, trade or send-money window is open (3192, colour 0xf); otherwise
// the mode is set (message 0x4644 to the render window) and, in full screen, 6682 ("%1" = 30) asks to keep it for 30
// seconds; No or no answer goes back to the mode before (0x78a600). The modes are this client's display modes
// (the "resolution" option's choices, "W x H (R Hz)"), which are 32 bit.
class VideoModesWindow : public StockWindow {
public:
    VideoModesWindow() : StockWindow("VideoModesWindow", false) {}

    void set_shown(UiHost& host, bool show) override {
        if (show && !open_) update(host);
        open_ = show;
    }
    const ListData* list(UiHost&, std::string_view id) override {
        return id == "VIDW_VideoResolutionsList" ? &list_ : nullptr;
    }
    bool list_click(UiHost& host, std::string_view id, int row, bool, int) override {
        if (id != "VIDW_VideoResolutionsList" || row < 0 || row >= static_cast<int>(list_.rows.size())) return false;
        list_.selected = row;
        fill_combos(host, row, -1);
        return true;
    }
    const ComboData* combo(UiHost&, std::string_view id) override {
        if (id == "VIDW_RefreshRateCombobox") return &rates_;
        if (id == "VIDW_BitDepthCombobox") return &depths_;
        return nullptr;
    }
    bool combo_select(UiHost&, std::string_view id, int index) override {
        ComboData* c = id == "VIDW_RefreshRateCombobox" ? &rates_ : id == "VIDW_BitDepthCombobox" ? &depths_ : nullptr;
        if (!c || index < 0 || index >= static_cast<int>(c->choices.size())) return false;
        c->selected = index;
        return true;
    }
    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (button != "left" || id != "VIDW_AcceptButton") return false;
        if (!host.conn || list_.selected < 0 || !host.set_option || !host.find_option) return true;
        if (host.conn->trade().has_value()) { // the give / trade window (no send-money window here)
            host.conn->show_eqstr(3192, {}, 0xf);
            return true;
        }
        const Size& size = sizes_[static_cast<std::size_t>(list_.selected)];
        int mode = size.modes.front();
        if (rates_.selected >= 0 && rates_.selected < static_cast<int>(size.modes.size()))
            mode = size.modes[static_cast<std::size_t>(rates_.selected)];
        int before = host.find_option("resolution").value_or(-1);
        host.set_option("resolution", mode);
        if (fullscreen(host)) {
            UiHost::Confirm c;
            c.text = host.conn->strings().format(6682, std::vector<std::string>{"30"});
            c.timeout_ms = 30000;
            c.done = [&host, before](bool yes) {
                if (!yes && before >= 0 && host.set_option) host.set_option("resolution", before);
            };
            host.confirm = std::move(c);
        }
        return true;
    }

private:
    struct Size {
        int w = 0, h = 0;
        std::vector<int> modes;  // the option's choice indices of this size
        std::vector<float> rates; // their refresh rates (0: unknown)
    };
    static bool fullscreen(UiHost& host) { return host.find_option && host.find_option("window_mode").value_or(0) == 2; }

    void update(UiHost& host) {
        sizes_.clear();
        list_ = {};
        list_.columns = 1;
        list_.labels = {"Resolution"};
        std::vector<std::string> choices = host.option_choices ? host.option_choices("resolution") : std::vector<std::string>{};
        for (std::size_t i = 0; i < choices.size(); ++i) {
            int w = 0, h = 0;
            float rate = 0.0f;
            if (std::sscanf(choices[i].c_str(), "%d x %d (%f", &w, &h, &rate) < 2 || w < 640 || h < 480) continue;
            Size* s = nullptr;
            for (auto& t : sizes_)
                if (t.w == w && t.h == h) s = &t;
            if (!s) {
                sizes_.push_back({w, h, {}, {}});
                s = &sizes_.back();
                list_.rows.push_back(ListRow{{std::to_string(w) + "x" + std::to_string(h)}});
            }
            s->modes.push_back(static_cast<int>(i));
            s->rates.push_back(rate);
        }
        int at = host.find_option ? host.find_option("resolution").value_or(-1) : -1;
        int rate_at = -1;
        for (std::size_t r = 0; r < sizes_.size(); ++r)
            for (std::size_t k = 0; k < sizes_[r].modes.size(); ++k)
                if (sizes_[r].modes[k] == at) {
                    list_.selected = static_cast<int>(r);
                    rate_at = static_cast<int>(k);
                }
        fill_combos(host, list_.selected, rate_at);
    }
    void fill_combos(UiHost& host, int row, int rate_at) {
        rates_ = {};
        depths_ = {};
        if (row < 0 || !fullscreen(host) || !host.conn) return;
        const Size& s = sizes_[static_cast<std::size_t>(row)];
        for (float r : s.rates)
            rates_.choices.push_back(r > 0.0f ? host.conn->strings().format(6628, std::vector<std::string>{std::to_string(static_cast<int>(r + 0.5f))})
                                              : host.conn->strings().format(6629, {}));
        rates_.selected = rate_at >= 0 ? rate_at : 0;
        depths_.choices.push_back(host.conn->strings().format(6627, {}));
        depths_.selected = 0;
    }

    std::vector<Size> sizes_;
    ListData list_;
    ComboData rates_, depths_;
};

} // namespace

void add_video_windows(std::vector<std::unique_ptr<StockWindow>>& out) { out.push_back(std::make_unique<VideoModesWindow>()); }

} // namespace hailstone::app
