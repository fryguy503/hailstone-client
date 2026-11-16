// The zone guide's windows over the zone guide data the server sends: ZonePathWnd (CZonePathWnd). EQEmu sends no
// zone guide, so it shows what the original shows without one. The original's FindLocationWnd is replaced by
// Hailstone's Find window (app/lua_ui_find.cpp, docs/navigation.md "Find").

#include <string>

#include "app/lua_ui_windows.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

// CZonePathWnd @0x6073c0: ZPW_ZoneList, the steps of the zone guide's active path (ZoneGuideManagerClient +0x8438).
// AboutToShow @0x6079a0 refuses to show without one (5468 in colour 0xd), and OnProcessFrame hides the window when
// the path goes. EQEmu sends no zone guide, so there is never an active path.
class ZonePathWindow : public StockWindow {
public:
    ZonePathWindow() : StockWindow("ZonePathWnd", false) {}

    void set_shown(UiHost& host, bool show) override {
        if (show && !open_) {
            if (host.conn) host.conn->show_eqstr(5468, {}, 0xd);
            return;
        }
        open_ = show;
    }
};

} // namespace

void add_zone_guide_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<ZonePathWindow>());
}

} // namespace hailstone::app
