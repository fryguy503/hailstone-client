#pragma once

// The in-game map's data and rules as the original's MapViewMap keeps them (MapViewMap, CMapViewWnd
// and the helpers at the addresses given below). Pure state: the
// files are read and written by the caller (app/lua_ui_map.cpp), the drawing is the caller's too.
//
// Map files: <maps dir>/<zone>.txt is layer 0 (the base map), <zone>_1.txt .. <zone>_3.txt layers 1..3
// (MapViewMap_BuildMapFilePath @0x6c90f0: "%s%s%s.txt" of the maps directory, the zone's short name from the zone
// table (EQWorldData::GetGeometryNameFromIndex: ShortName, EQG zones alike) and "_%d" for layers above 0). Lines:
//   L x1, y1, z1,  x2, y2, z2,  r, g, b
// labels:
//   P x, y, z,  r, g, b,  size,  text_with_underscores
// in map coordinates (see world_to_map). MapViewMap::Load @0x6cc920 / LoadEx @0x6cba60 read them with fgets (1022
// bytes) and sscanf; SaveEx @0x6cb880 / MapViewMap_SaveLayerFile @0x6cc710 write them with "%0.4f".
//
// Map coordinates are the original's (-X, -Y, Z) of a spawn (PlayerBase +0x68, +0x64, +0x6c): the server's
// (-x, -y, z), in this client's world axes (game/coords.h) (-world.y, -world.x, z). On screen map x grows to the
// right and map y down: north (+Y, /loc's first number) is up and west (+X, its second) is to the left.

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::game {

inline glm::vec3 world_to_map(const glm::vec3& w) { return {-w.y, -w.x, w.z}; }
inline glm::vec3 map_to_world(const glm::vec3& m) { return {-m.y, -m.x, m.z}; }

// MapViewLine (0x28 bytes): +0 next, +4 prev, +8 start, +0x14 end, +0x20 ARGB, +0x24 layer.
struct MapLine {
    glm::vec3 start{0.0f}, end{0.0f};
    std::uint32_t color = 0xff000000; // ARGB (D3D)
    int layer = 0;
};

// MapViewLabel (0x38 bytes): +0 id, +4 next, +8 prev, +0xc location, +0x18 ARGB, +0x1c size (1..3), +0x20 text,
// +0x24 layer, +0x28 / +0x2c text width / height in its font, +0x30 / +0x34 the text's offset from the point.
struct MapLabel {
    int id = 0;
    glm::vec3 pos{0.0f};
    std::uint32_t color = 0xff000000;
    int size = 1;
    std::string text;
    int layer = 0;
    int width = 0, height = 0;
    int offset_x = 0, offset_y = 0;
};

// The width and height of a label's text in its font (CTextureFont::GetTextExtent / GetHeight of
// CXWndManager::FontsArray[size - 1]).
using MapMeasure = std::function<std::pair<int, int>(std::string_view text, int size)>;

// The integer box of a layer's line end points (Load's extents: each compared as float against the int, then
// truncated; all start at 0, so the box always holds the origin).
struct MapExtents {
    int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
};

struct MapLayerParse {
    std::vector<MapLine> lines;   // in file order
    std::vector<MapLabel> labels; // in file order (id, width and height unset)
    MapExtents extents;
};

// One layer file as MapViewMap::Load reads it: lines read with fgets(1022 bytes); a line whose first character is
// 'L' / 'l' or 'P' / 'p' is scanned from its third character; fields sscanf leaves unread stay 0 (colours) or 0.0
// (points); the colour bytes are the low bytes of the ints, alpha 0xff; a label's text is one %256s word with '_'
// read as ' ', and a label whose text field is missing keeps the previous label's text (`label_buffer`, Load's one
// stack buffer for all layers); a label size is clamped to 1..3.
MapLayerParse parse_map_layer(std::string_view text, int layer, std::string& label_buffer);

// The file name of a layer: "<zone>.txt", "<zone>_1.txt" .., or "" when `zone` is not a zone short name
// (is_zone_short_name: a name with a separator, "..", a drive or a device name would name a file elsewhere).
std::string map_layer_file_name(std::string_view zone, int layer);

// A label's place candidates (MapLabel_PlacementRect @0x6c8290): the
// rectangle {l, t, r, b} of a w x h text at anchor 0..23 around (x, y).
std::array<int, 4> map_label_rect(unsigned anchor, int x, int y, int w, int h);

class MapView {
public:
    // ---- state (MapViewMap +0x220..; the constructor's values, CMapViewWnd's constructor @0x6cfb70)
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0; // +0x220 / +0x224 / +0x228 / +0x22c: the zone's box
    int pan_x = 0, pan_y = 0;                         // +0x230 / +0x234
    float line_offset_x = 0, line_offset_y = 0;      // +0x238 / +0x23c
    float scale_diff_x = 0, scale_diff_y = 0;        // +0x240 / +0x244
    float scale_x = 0, scale_y = 0;                  // +0x248 / +0x24c
    std::list<MapLine> lines;                        // +0x250, newest first
    std::list<MapLabel> labels;                      // +0x254, newest first
    int next_label_id = 0;                           // +0x258
    bool line_active = false;                        // +0x25c: a line is being walked
    glm::vec3 current_point{0.0f};                   // +0x260
    std::uint32_t current_color = 0xff000000;        // +0x26c: new lines
    std::uint32_t my_color = 0xff000000;             // +0x270: the player
    std::uint32_t group_color = 0xff000000;          // +0x274
    std::uint32_t path_color = 0xffff0000;           // +0x278: find path
    bool failed_find = false;                        // +0x27c: a failed find's X is shown (0x6c92e0)
    glm::vec3 failed_point{0.0f};                    // +0x280: its point (world coordinates, the original's Y, X, Z)
    int failed_kind = 0;                             // +0x28c: the find's kind (5: a map location)
    std::deque<MapLine*> marked_lines;               // +0x290, newest first
    MapLabel* marked_label = nullptr;                // +0x294
    MapLabel* selected_label = nullptr;              // +0x298: chosen in the search list
    int layer_filter = 0xf;                          // +0x29c: bit n = layer n shown
    int active_layer = 1;                            // +0x2a0: where new lines and labels go
    int min_z = 10, max_z = 10;                      // +0x2a4 / +0x2a8: the height filter's reach
    bool z_filter = false;                           // +0x2ac
    float zoom = 0.0f;                               // +0x2b0
    float relayout_zoom = 0.0f;                      // +0x2b4: the zoom labels were last placed at
    int range = 0;                                   // +0x2b8: "Range = n"
    bool show_group = true;                          // +0x2bc
    bool find_auto_zoom = true;                      // +0x2bd
    bool labels_active = true;                       // +0x2be: the Labels button
    bool relayout = false;                           // +0x2bf: place the labels again before the next draw
    bool current_zone = false;                       // +0x2c0: the map shows the zone the player is in

    // Auto Z, a client extension (the original has only the height filter above, which hides): lines, labels and
    // marks fade by their height from the player's feet. A thing above the feet is drawn whole within
    // `auto_z_above` of them, one below within `auto_z_below`; past its side's range it fades linearly to kAutoZFloor
    // at three times that range. Kept as the UI ini's AutoZ / AutoZAbove / AutoZBelow (each 1..kAutoZMaxRange).
    static constexpr float kAutoZFloor = 0.15f;
    static constexpr int kAutoZMaxRange = 1000;
    bool auto_z = false;
    int auto_z_above = 30;
    int auto_z_below = 30;

    // MapViewMap::SetZoneExtents @0x6c7b40 (min x, max x, min y, max y).
    void set_zone_extents(int min_x, int max_x, int min_y, int max_y);
    // MapViewMap::SetZoom @0x6c7c40.
    void set_zoom(float zoom);
    // MapViewMap::HandleWheelMove @0x6c9740: up x1.5, down x2/3 (not below 1).
    void wheel(int delta);
    // The zoom buttons (CMapViewWnd::WndNotification): x1.1, x(1/1.1) not below 1.
    void zoom_in();
    void zoom_out();

    // The whole map (0x6ca9e0): the marked lines and label, every line and label, the line being walked.
    void clear();
    // Load (0x6cf8a0's MapViewMap::Load): each layer 0..3 that `read` returns text for is added; layer 0's
    // extents go to set_zone_extents; labels are measured. Then the labels are placed again.
    void load(const std::function<std::optional<std::string>(int layer)>& read, const MapMeasure& measure);
    // The current zone's box (CDisplay_LoadWorldExtents -> CMapViewWnd_LoadZoneMap): the terrain's min / max in
    // this client's world axes, truncated as the original's ftol does.
    void set_world_box(const glm::vec3& lo, const glm::vec3& hi);
    // A layer's file text (MapViewMap_SaveLayerFile @0x6cc710 / SaveEx): its lines then its labels, newest first.
    std::string layer_text(int layer) const;

    // Lines and labels shown: the layer filter (layer < 4), and the height filter around `feet` when it is on.
    bool layer_shown(int layer) const { return layer >= 0 && layer < 4 && (layer_filter & (1 << layer)); }
    bool line_shown(const MapLine& l, std::optional<float> feet) const;
    bool label_shown(const MapLabel& l, std::optional<float> feet) const;
    // Auto Z's opacity (kAutoZFloor..1) for a thing spanning heights a..b: 1 with Auto Z off or no feet, and while
    // the span is within its side's range of the feet (a ramp whose span holds the feet is whole).
    float z_fade(float a, float b, std::optional<float> feet) const;
    float z_fade(float z, std::optional<float> feet) const { return z_fade(z, z, feet); }

    // ---- editing (the map toolbar)
    // MapViewMap::AddLabel @0x6cc620 (on the current zone only): on the active layer, measured, at the list's head.
    void add_label(const glm::vec3& pos, std::uint32_t argb, int size, const std::string& text, const MapMeasure& measure);
    // 0x6c7f90: start a line at p.
    void start_line(const glm::vec3& p);
    // 0x6cab70: a line from the current point to p (when the original's test passes), p the new current point.
    void add_point(const glm::vec3& p);
    // 0x6caa70: add_point, then the line ends.
    void end_line(const glm::vec3& p);
    // MapViewMap::Clear @0x6c7fd0: removes the marked lines.
    void remove_marked_lines();
    // 0x6c8040: removes the marked label.
    void remove_marked_label();
    // 0x6cbee0 (confirmation 100): every line and label of the active layer.
    void clear_active_layer();
    // 0x6c8b80: marks a line once; null clears the marks.
    void mark_line(MapLine* line);
    bool is_marked(const MapLine* line) const;
    // MapViewMap::JoinLinesAtIntersect @0x6c8db0: with exactly two marked lines meeting within 500 units, moves the
    // nearer ends to the crossing (ctrl: the first marked line's only). False: nothing joined (the caller prints
    // string 0x3ad0).
    bool join_lines_at_intersect(bool ctrl);
    // 0x6c8bf0: the marked lines' colour.
    void set_marked_lines_color(std::uint32_t rgb);
    // Re-measures a label (0x6cac60) after its text or size changed.
    static void measure_label(MapLabel& label, const MapMeasure& measure);
    MapLabel* label_by_id(int id);

    // ---- each frame
    // 0x6cd270: the scale and the centre. The zone's box is fitted into 472 x 360 pixels; zoomed, the map is
    // centred, on the current zone, on the find box while the find holds the map (`find_box`, 0x6ccef0), else on
    // the player (`player`, map coordinates) without a selected label; or on the box of the player and the selected
    // label (`label_box`). The boxes are min/max of the original's Y and X (0x6cbfa0).
    void precalc(const std::optional<glm::vec2>& player, const std::optional<glm::vec4>& label_box,
                 const std::optional<glm::vec4>& find_box = std::nullopt);
    // MapViewMap::TransformPoint @0x6c87a0: map (x, y) to pixels of a cw x ch client area.
    glm::vec2 transform(float x, float y, float cw, float ch) const;
    // MapViewMap::GetWorldCoordinates @0x6c88a0: pixels back to the original's (Y, X).
    glm::vec2 world_coordinates(float px, float py, float cw, float ch) const;
    // sub_006cafa0 and PreCalcRenderValues: the labels' text places in a cw x ch client area at origin (ox, oy).
    void place_labels(float cw, float ch, int ox, int oy, int clip_r, int clip_b);
    // 0x6cc010: zoom so the box (the original's minY, maxY, minX, maxX) fills 60..80% of the client (70..85%
    // and up to x20 for find).
    void zoom_to_fit(float min_y, float max_y, float min_x, float max_x, bool find, float cw, float ch);

    // The label place list (0xf73e10): rectangles of the labels' points (flag 0) and placed texts (flag 1).
    struct Placed {
        int l = 0, t = 0, r = 0, b = 0;
        MapLabel* label = nullptr;
        bool text = false;
    };
    const std::deque<Placed>& placed() const { return placed_; }

private:
    void add_line(const glm::vec3& a, const glm::vec3& b, std::uint32_t color, int layer);
    int count_overlaps(int l, int t, int r, int b, const Placed* exclude) const;
    int try_place(unsigned anchor, int x, int y, MapLabel& label, int cl, int ct, int cr, int cb, bool force);
    void place_pass();

    std::deque<Placed> placed_; // newest first
};

} // namespace hailstone::game
