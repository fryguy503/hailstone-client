#pragma once

// Temporary ImGui debug windows for duels, inspect, the bazaar and mail, each
// off while its stock Lua window runs (app/lua_ui_player.cpp, lua_ui_market.cpp,
// lua_ui_mail.cpp; ui.cpp_windows=1 keeps them): the inspect window, the
// confirmation question, a book's text, the trader window (/trader), a
// trader's shop, the bazaar search (/bazaar) and the barter window appear
// while their state is open; the mail window with HAILSTONE_MAIL_WINDOW=1 (or
// once mail arrives).

#include <cstdint>
#include <string>

namespace hailstone::net { class GameConnection; }

namespace hailstone::app {

class MarketUi {
public:
    MarketUi();
    // dev_windows: the duel state window (no stock window shows it).
    void draw(net::GameConnection& conn, bool dev_windows);

private:
    void draw_trader(net::GameConnection& conn);
    void draw_search(net::GameConnection& conn);
    void draw_mail(net::GameConnection& conn);

    bool mail_open_ = false;
    char search_name_[64] = {};
    char mail_to_[128] = {}, mail_subject_[128] = {}, mail_body_[1024] = {};
    std::uint32_t mail_selected_ = 0;
    int price_[4] = {};
};

} // namespace hailstone::app
