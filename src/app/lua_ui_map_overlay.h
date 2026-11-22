#pragma once

#include <string_view>

struct lua_State;

namespace hailstone::app {

struct UiCallbackContext;

// The geometry a map view was drawn with (hs.DrawZoneMap, hs.DrawMinimap), as a map overlay is given it: screen
// pixels and map units. `minimap` picks the disc's fields (the rectangle is its bounding square).
struct MapOverlayView {
    bool minimap = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // screen: the area drawn
    float cx = 0, cy = 0;                 // screen: the view's centre
    float radius = 0;                     // screen: the minimap's disc; 0 for the zone map
    float scale = 1;                      // screen pixels per map unit
    float turn = 0;                       // the view's rotation, in heading units (0: north up)
    float center_x = 0, center_y = 0;     // map units at the centre
    bool have_player = false;             // false: another zone's map, which has no player
    float player_x = 0, player_y = 0;     // the player's map position, when have_player
    std::string_view zone;                // the loaded map's short name
    bool current_zone = true;             // the loaded map is the player's zone
};

// Draws what the add-ons registered for the map (hs.UI.map.RegisterOverlay) onto the view just drawn: by ascending
// priority, ties by registration order. Called at the end of hs.DrawZoneMap and hs.DrawMinimap, so nothing runs for a
// map or minimap that is not drawn. The zone map's rectangle is clipped (a callback draws on the map window's own
// list, an inherited ui.Foreground(true) notwithstanding); every callback is protected and one that fails is
// retired. A callback the drawing code's own time ran out on is skipped instead, and stays registered. A callback of
// another add-on than the code that drew the map runs as itself, with the capabilities it registered under. Each
// callback gets its own table, built from a snapshot of the view, so nothing it does to the table reaches another
// callback. No Lua error leaves this function: the map draw's own objects and clip stay intact.
void dispatch_map_overlays(lua_State* L, const MapOverlayView& view);

// The hs table is on top; call after register_map_api so the bindings join the same table.
void register_map_overlay_api(lua_State* L, const UiCallbackContext& context);

} // namespace hailstone::app
