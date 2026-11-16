#include <doctest/doctest.h>

#include <string>

#include "assets/zone.h"
#include "game/collision.h"
#include "game/map_data.h"
#include "game_files.h"

using namespace hailstone;
using namespace hailstone::game;

namespace {

MapMeasure fixed_measure(int per_char, int height) {
    return [per_char, height](std::string_view text, int size) {
        return std::pair<int, int>{static_cast<int>(text.size()) * per_char * size, height};
    };
}

} // namespace

TEST_CASE("Map files: L and P records as MapViewMap::Load scans them") {
    std::string buffer;
    auto p = parse_map_layer("L 678.0000, 132.0000, -14.0000,  671.0000, 125.0000, -14.0000,  255, 255, 0\n"
                             "l -10, 20.5, 3, 4,5,6, 1, 2, 3\r\n"
                             "P -392.2878, -165.4972, -0.9980,  0, 0, 240,  3,  Waypoint\n"
                             "p 1, 2, 3,  240, 240, 240,  2,  The_Silent_Fist_Clan\n"
                             "# a comment\n"
                             "\n"
                             "P 5, 6, 7,  1, 2, 3,  9,  big\n"
                             "P 5, 6, 7,  1, 2, 3,  0\n"
                             "L 1, 2\n",
                             2, buffer);
    REQUIRE(p.lines.size() == 3);
    CHECK(p.lines[0].start == glm::vec3(678, 132, -14));
    CHECK(p.lines[0].end == glm::vec3(671, 125, -14));
    CHECK(p.lines[0].color == 0xffffff00u);
    CHECK(p.lines[0].layer == 2);
    // lower case and single spaces read the same (the format's spaces match any whitespace, also none)
    CHECK(p.lines[1].start == glm::vec3(-10, 20.5f, 3));
    CHECK(p.lines[1].end == glm::vec3(4, 5, 6));
    CHECK(p.lines[1].color == 0xff010203u);
    // fields sscanf stopped before stay zero; alpha is always 0xff
    CHECK(p.lines[2].start == glm::vec3(1, 2, 0));
    CHECK(p.lines[2].end == glm::vec3(0));
    CHECK(p.lines[2].color == 0xff000000u);

    REQUIRE(p.labels.size() == 4);
    CHECK(p.labels[0].pos.x == doctest::Approx(-392.2878));
    CHECK(p.labels[0].color == 0xff0000f0u);
    CHECK(p.labels[0].size == 3);
    CHECK(p.labels[0].text == "Waypoint");
    CHECK(p.labels[1].text == "The Silent Fist Clan"); // '_' reads as ' '
    CHECK(p.labels[1].size == 2);
    CHECK(p.labels[2].size == 3); // clamped to 1..3
    CHECK(p.labels[3].size == 1);
    CHECK(p.labels[3].text == "big"); // no text field: Load's buffer keeps the last label's text

    // The extents: truncated end points, starting from 0 (layer 0 only gives them to the map).
    CHECK(p.extents.min_x == -10);
    CHECK(p.extents.max_x == 678);
    CHECK(p.extents.min_y == 0);
    CHECK(p.extents.max_y == 132);
}

TEST_CASE("Map files: layer names, long label texts and long lines") {
    CHECK(map_layer_file_name("qeynos2", 0) == "qeynos2.txt");
    CHECK(map_layer_file_name("qeynos2", 3) == "qeynos2_3.txt");
    CHECK(map_layer_file_name("Freeport_West", 1) == "Freeport_West_1.txt");
    // A server's zone name that is a path names no file: it would reach outside the maps folder.
    for (const char* bad : {"", "../x", "..", "a/b", "a\\b", "/home/user/notes", "C:x", "C:\\EQ\\spells_us", "con",
                            "LPT1", "qeynos.2", "qeynos2 "})
        CHECK_MESSAGE(map_layer_file_name(bad, 0).empty(), bad);
    std::string buffer;
    std::string longtext(300, 'a');
    auto p = parse_map_layer("P 0, 0, 0,  1, 1, 1,  1,  " + longtext + "\n", 0, buffer);
    REQUIRE(p.labels.size() == 1);
    CHECK(p.labels[0].text.size() == 255);
    // fgets reads 1021 characters at a time: a longer line goes on as a new "line".
    std::string line = "L 1, 2, 3, 4, 5, 6, 7, 8, 9" + std::string(1100, ' ') + "PX\n";
    p = parse_map_layer(line, 0, buffer);
    CHECK(p.lines.size() == 1);
    CHECK(p.labels.empty());
}

TEST_CASE("Map files: saved as SaveEx writes them, newest first") {
    MapView m;
    m.load(
        [](int layer) -> std::optional<std::string> {
            if (layer == 0) return std::string("L 1, 2, 3, 4, 5, 6, 10, 20, 30\nL 7, 8, 9, 10, 11, 12, 1, 2, 3\n");
            if (layer == 2) return std::string("P 1.5, -2, 3,  0, 0, 240,  2,  The_Gate\n");
            return std::nullopt;
        },
        fixed_measure(6, 12));
    CHECK(m.lines.size() == 2);
    CHECK(m.labels.size() == 1);
    CHECK(m.labels.front().id == 1);
    CHECK(m.labels.front().width == 8 * 6 * 2);
    CHECK(m.labels.front().height == 12);
    CHECK(m.layer_text(0) == "L 7.0000, 8.0000, 9.0000,  10.0000, 11.0000, 12.0000,  1, 2, 3\n"
                             "L 1.0000, 2.0000, 3.0000,  4.0000, 5.0000, 6.0000,  10, 20, 30\n");
    CHECK(m.layer_text(1).empty());
    CHECK(m.layer_text(2) == "P 1.5000, -2.0000, 3.0000,  0, 0, 240,  2,  The_Gate\n");
    // Read back, the layer is the same lines in the reverse order.
    std::string buffer;
    auto p = parse_map_layer(m.layer_text(0), 0, buffer);
    REQUIRE(p.lines.size() == 2);
    CHECK(p.lines[0].start == glm::vec3(7, 8, 9));
}

TEST_CASE("Map zoom: the constructor's extents and zoom, then a zone keeps the range") {
    MapView m;
    m.set_zone_extents(-1000, 1000, -1000, 1000); // zoom 0: range 200, zoom width / 200
    CHECK(m.range == 200);
    CHECK(m.zoom == doctest::Approx(10.0));
    m.set_zoom(2.0f);
    CHECK(m.range == 1000);
    // A wider zone keeps 1000 units across.
    m.set_zone_extents(-2000, 2000, -500, 500);
    CHECK(m.zoom == doctest::Approx(4.0));
    CHECK(m.range == 1000);
    // A narrower one is shown whole.
    m.set_zone_extents(-300, 300, -500, 500);
    CHECK(m.zoom == 1.0f);
    CHECK(m.range == 600);
    m.set_zone_extents(-400, 400, -500, 500); // at zoom 1 the range is the width
    CHECK(m.range == 800);
    // SetZoom: never under 5 units.
    m.set_zoom(1000.0f);
    CHECK(m.range == 5);
    CHECK(m.zoom == doctest::Approx(160.0));
    // The wheel: x1.5 up, x2/3 down, not below 1.
    m.set_zoom(1.2f);
    m.wheel(-1);
    CHECK(m.zoom == 1.0f);
    m.wheel(1);
    CHECK(m.zoom == doctest::Approx(1.5));
    m.zoom_in();
    CHECK(m.zoom == doctest::Approx(1.65));
    m.set_zoom(1.05f);
    m.zoom_out();
    CHECK(m.zoom == 1.0f);
}

TEST_CASE("Map transform: the zone box fitted into 472 x 360, zoomed around the player") {
    MapView m;
    m.set_zone_extents(-1000, 1000, -1000, 1000);
    m.set_zoom(1.0f);
    m.current_zone = true;
    m.precalc(glm::vec2(100, -50), std::nullopt);
    // 2000 x 2000 into 472 x 360: 0.18 a unit, centred across.
    CHECK(m.scale_x == doctest::Approx(0.18));
    CHECK(m.scale_y == doctest::Approx(0.18));
    CHECK(m.scale_diff_x == doctest::Approx(56.0));
    glm::vec2 p = m.transform(-1000, -1000, 500, 400);
    CHECK(p.x == doctest::Approx(56.0));
    CHECK(p.y == doctest::Approx(0.0));
    p = m.transform(1000, 1000, 500, 400);
    CHECK(p.x == doctest::Approx(416.0));
    CHECK(p.y == doctest::Approx(360.0));
    // Zoomed: the player in the middle of the client area, plus the pan.
    m.set_zoom(4.0f);
    m.pan_x = 3;
    m.precalc(glm::vec2(100, -50), std::nullopt);
    p = m.transform(100, -50, 500, 400);
    CHECK(p.x == doctest::Approx(253.0));
    CHECK(p.y == doctest::Approx(200.0));
    p = m.transform(110, -50, 500, 400);
    CHECK(p.x == doctest::Approx(253.0 + 10 * 4 * 0.18));
    // And back: GetWorldCoordinates gives the original's (Y, X) = (-map y, -map x).
    glm::vec2 w = m.world_coordinates(p.x, p.y, 500, 400);
    CHECK(w.x == doctest::Approx(50.0));
    CHECK(w.y == doctest::Approx(-110.0));
}

TEST_CASE("Map world box: the terrain's box in the original's axes") {
    MapView m;
    m.set_zoom(1.0f);
    m.set_world_box(glm::vec3(-300.7f, -1000.2f, -50), glm::vec3(900.9f, 200.5f, 80));
    // min / max x are the original's X (world y), y its Y (world x), truncated toward zero.
    CHECK(m.min_x == -1000.0f);
    CHECK(m.max_x == 200.0f);
    CHECK(m.min_y == -300.0f);
    CHECK(m.max_y == 900.0f);
    // world_to_map puts the box's corners at 0 and its size before scaling.
    glm::vec3 lo = world_to_map(glm::vec3(900, 200, 0)), hi = world_to_map(glm::vec3(-300, -1000, 0));
    CHECK(lo.x + m.max_x == doctest::Approx(0.0));
    CHECK(lo.y + m.max_y == doctest::Approx(0.0));
    CHECK(hi.x + m.max_x == doctest::Approx(1200.0));
    CHECK(hi.y + m.max_y == doctest::Approx(1200.0));
}

TEST_CASE("Map labels: placement rectangles and the first free place") {
    auto r = map_label_rect(0, 100, 50, 40, 10); // centred above, 2 pixels up
    CHECK(r == std::array<int, 4>{80, 38, 120, 48});
    r = map_label_rect(1, 100, 50, 40, 10); // centred below
    CHECK(r == std::array<int, 4>{80, 52, 120, 62});
    r = map_label_rect(2, 100, 50, 40, 10); // right, middle
    CHECK(r == std::array<int, 4>{104, 45, 144, 55});
    r = map_label_rect(5, 100, 50, 40, 10); // left, middle
    CHECK(r == std::array<int, 4>{56, 45, 96, 55});
    r = map_label_rect(24, 100, 50, 40, 10); // past the table: unmoved
    CHECK(r == std::array<int, 4>{100, 50, 140, 60});

    MapView m;
    m.set_zone_extents(-1000, 1000, -1000, 1000);
    m.set_zoom(1.0f);
    m.current_zone = true;
    m.precalc(glm::vec2(0, 0), std::nullopt);
    auto measure = fixed_measure(5, 10);
    m.add_label(glm::vec3(0, 0, 0), 0xffffffff, 1, "abcd", measure);          // 20 x 10
    m.add_label(glm::vec3(0, 0, 0), 0xffffffff, 1, "efgh", measure);          // the same point
    m.add_label(glm::vec3(-1000, -1000, 0), 0xffffffff, 1, "corner", measure); // at the box's corner
    m.place_labels(500, 400, 0, 0, 500, 400);
    auto it = m.labels.begin();
    const MapLabel& corner = *it++;
    const MapLabel& second = *it++;
    const MapLabel& first = *it;
    // The newest label is placed first: above its point, centred. The next one at the same point goes below.
    CHECK(second.offset_x == -10);
    CHECK(second.offset_y == -12);
    CHECK(first.offset_x == -10);
    CHECK(first.offset_y == 2);
    // The corner label (at pixel 56, 0) still goes above: the placement does not keep texts inside the area.
    CHECK(corner.offset_x == -15);
    CHECK(corner.offset_y == -12);
}

TEST_CASE("Map editing: lines walked, marked, joined and cleared") {
    MapView m;
    m.set_zone_extents(-1000, 1000, -1000, 1000);
    m.current_zone = true;
    m.active_layer = 2;
    m.current_color = 0xff102030;
    m.start_line(glm::vec3(0, 0, 0));
    // 0x6cab70 compares the current point with max + p (here 1000 + p).
    m.add_point(glm::vec3(-1000, 0, 0)); // |0 - 0| and |0 - 1000|: added
    REQUIRE(m.lines.size() == 1);
    CHECK(m.lines.front().layer == 2);
    CHECK(m.lines.front().color == 0xff102030u);
    CHECK(m.lines.front().end == glm::vec3(-1000, 0, 0));
    m.end_line(glm::vec3(-1000, -1000, 5)); // |-1000 - 0| > 0.9: added, then the line ends
    CHECK(m.lines.size() == 2);
    CHECK_FALSE(m.line_active);
    m.add_point(glm::vec3(5, 5, 5)); // no line being walked
    CHECK(m.lines.size() == 2);

    // Two crossing lines meet at their crossing.
    m.lines.clear();
    m.lines.push_front(MapLine{glm::vec3(0, 0, 0), glm::vec3(90, 0, 0), 0xff000000, 1});
    MapLine* a = &m.lines.front();
    m.lines.push_front(MapLine{glm::vec3(100, -50, 0), glm::vec3(100, 50, 0), 0xff000000, 1});
    MapLine* b = &m.lines.front();
    m.mark_line(a);
    m.mark_line(b);
    m.mark_line(b); // once
    CHECK(m.marked_lines.size() == 2);
    CHECK(m.join_lines_at_intersect(false));
    CHECK(a->end == glm::vec3(100, 0, 0));
    CHECK(b->end == glm::vec3(100, 0, 0)); // a tie (equal distances) moves the end
    m.mark_line(nullptr);
    CHECK_FALSE(m.join_lines_at_intersect(false));
    m.mark_line(b);
    m.remove_marked_lines();
    CHECK(m.lines.size() == 1);

    // Labels: added on the active layer, removed when marked, the active layer cleared.
    auto measure = fixed_measure(5, 10);
    m.add_label(glm::vec3(1, 2, 3), 0xff00ff00, 2, "here", measure);
    m.active_layer = 3;
    m.add_label(glm::vec3(1, 2, 3), 0xff00ff00, 2, "there", measure);
    CHECK(m.labels.size() == 2);
    m.marked_label = &m.labels.front();
    m.remove_marked_label();
    CHECK(m.labels.size() == 1);
    CHECK(m.labels.front().text == "here");
    m.active_layer = 2;
    m.clear_active_layer();
    CHECK(m.labels.empty());
    CHECK(m.lines.size() == 1); // layer 1
    // Not on the current zone: nothing is added.
    m.current_zone = false;
    m.add_label(glm::vec3(0), 0xff000000, 1, "x", measure);
    CHECK(m.labels.empty());
}

TEST_CASE("Map filters: layers and the height filter around the feet") {
    MapView m;
    MapLine l{glm::vec3(0, 0, 0), glm::vec3(10, 0, 30), 0xff000000, 1};
    CHECK(m.line_shown(l, 100.0f)); // filter off
    m.z_filter = true;
    CHECK_FALSE(m.line_shown(l, 100.0f)); // both ends below 90
    CHECK(m.line_shown(l, 35.0f));         // one end inside 25..45
    m.layer_filter = 0xd;
    CHECK_FALSE(m.line_shown(l, 35.0f));
    MapLabel lab;
    lab.layer = 0;
    lab.pos.z = 46;
    CHECK_FALSE(m.label_shown(lab, 35.0f));
    lab.pos.z = 45;
    CHECK(m.label_shown(lab, 35.0f));
    lab.layer = 4;
    CHECK_FALSE(m.label_shown(lab, 35.0f));
}

TEST_CASE("Map Auto Z: what is far above or below the feet fades, a span holding the feet stays whole") {
    MapView m;
    CHECK(m.z_fade(500.0f, 0.0f) == 1.0f); // off
    m.auto_z = true;
    m.auto_z_above = m.auto_z_below = 30;
    CHECK(m.z_fade(500.0f, std::nullopt) == 1.0f); // no feet
    CHECK(m.z_fade(30.0f, 0.0f) == 1.0f);           // within the range
    CHECK(m.z_fade(-30.0f, 0.0f) == 1.0f);
    CHECK(m.z_fade(60.0f, 0.0f) == doctest::Approx(1.0f - 0.85f * 0.5f)); // halfway to three ranges
    CHECK(m.z_fade(-90.0f, 0.0f) == doctest::Approx(MapView::kAutoZFloor));
    CHECK(m.z_fade(1000.0f, 0.0f) == doctest::Approx(MapView::kAutoZFloor));
    CHECK(m.z_fade(-200.0f, 200.0f, 0.0f) == 1.0f); // a ramp past the feet
    CHECK(m.z_fade(100.0f, 130.0f, 0.0f) < m.z_fade(70.0f, 130.0f, 0.0f)); // the nearer end counts
    // The height filter still hides; Auto Z only fades what it shows.
    m.z_filter = true;
    MapLine l{glm::vec3(0, 0, 200), glm::vec3(10, 0, 200), 0xff000000, 0};
    CHECK_FALSE(m.line_shown(l, 0.0f));
}

TEST_CASE("Map Auto Z: the ranges above and below the feet are set apart") {
    MapView m;
    m.auto_z = true;
    m.auto_z_above = 10;
    m.auto_z_below = 50;
    CHECK(m.z_fade(10.0f, 0.0f) == 1.0f);
    CHECK(m.z_fade(20.0f, 0.0f) == doctest::Approx(1.0f - 0.85f * 0.5f)); // halfway to three times 10
    CHECK(m.z_fade(30.0f, 0.0f) == doctest::Approx(MapView::kAutoZFloor));
    CHECK(m.z_fade(-50.0f, 0.0f) == 1.0f);
    CHECK(m.z_fade(-100.0f, 0.0f) == doctest::Approx(1.0f - 0.85f * 0.5f)); // halfway to three times 50
    CHECK(m.z_fade(-150.0f, 0.0f) == doctest::Approx(MapView::kAutoZFloor));
    CHECK(m.z_fade(-20.0f, 20.0f, 0.0f) == 1.0f); // a span holding the feet
}

TEST_CASE("Map zoom to fit: the selected label's box") {
    MapView m;
    m.set_zone_extents(-1000, 1000, -1000, 1000);
    m.set_zoom(2.0f);
    m.current_zone = true;
    m.precalc(glm::vec2(0, 0), std::nullopt);
    // A 100-unit box at zoom 2 covers 36 / 400 of the height: zoomed in to 60%, capped at 10.
    m.zoom_to_fit(-50, 50, -50, 50, false, 500, 400);
    CHECK(m.zoom == doctest::Approx(10.0));
    // A box larger than the view: zoomed out to 80%, not below 1.
    m.precalc(glm::vec2(0, 0), std::nullopt);
    m.zoom_to_fit(-1000, 1000, -1000, 1000, false, 500, 400);
    CHECK(m.zoom == 1.0f);
}

TEST_CASE("Map files of the game: every layer parses, and the maps match the zones' geometry") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    // The two kinds of zone, classic WLD (South Qeynos) and EQG (the Moors of Nokk, moors.eqg): the base map's lines
    // lie inside the zone's box once moved to world axes (game/coords.h), with a margin for map lines drawn past
    // the walls.
    for (std::string zone : {"qeynos2", "moors"}) {
        auto text = game->read("maps/" + map_layer_file_name(zone, 0));
        if (!text && zone == "moors") {
            MESSAGE("maps/moors.txt: optional server map is absent; its EQG geometry check was not exercised");
            continue;
        }
        REQUIRE(text);
        std::string buffer;
        auto p = parse_map_layer(std::string_view(reinterpret_cast<const char*>(text->data()), text->size()), 0, buffer);
        CHECK(p.lines.size() > 100);
        auto z = assets::load_zone(*game, zone);
        auto mesh = CollisionMesh::build(z);
        REQUIRE(mesh.bounds());
        auto [lo, hi] = *mesh.bounds();
        int inside = 0;
        for (const auto& l : p.lines) {
            glm::vec3 w = map_to_world(l.start);
            if (w.x >= lo.x - 50 && w.x <= hi.x + 50 && w.y >= lo.y - 50 && w.y <= hi.y + 50) ++inside;
        }
        INFO(zone);
        CHECK(inside >= static_cast<int>(p.lines.size()) * 95 / 100);
        // Mirrored (world (x, y) read as map (x, y)) most would fall outside.
        int mirrored = 0;
        for (const auto& l : p.lines) {
            glm::vec3 w = l.start;
            if (w.x >= lo.x - 50 && w.x <= hi.x + 50 && w.y >= lo.y - 50 && w.y <= hi.y + 50) ++mirrored;
        }
        CHECK(mirrored < inside);
    }
}

#include "game/zone_names.h"

TEST_CASE("EQWorldData::IsNewbieZone: zone flag 0x2 of the zone table (the map toolbar's AboutToShow)") {
    CHECK(is_newbie_zone(1));            // qeynos
    CHECK(is_newbie_zone(4));            // qeytoqrg
    CHECK_FALSE(is_newbie_zone(0x3a));   // crushbone
    CHECK(is_newbie_zone(1 | 0x8000));   // ids above 1000 are masked with 0x7fff
    CHECK_FALSE(is_newbie_zone(65000));  // unknown
    CHECK(zone_flags(0xbd) == 0x1000006); // tutorialb, a RegisterZone2 row
    // arena is registered twice; the first row fills the slot (RegisterZone2 skips a taken one).
    CHECK(zone_flags(77) == 0x400004);
}

TEST_CASE("EQWorldData::ExpansionZone: EQZoneInfo +8 of the zone table") {
    CHECK(zone_expansion(1) == 0);        // qeynos
    CHECK(zone_expansion(78) == 1);       // fieldofbone (Kunark)
    CHECK(zone_expansion(229) == 6);      // guka, the Lost Dungeons of Norrath
    CHECK(zone_expansion(77) == 0);       // arena: its classic row, not the LDoN one
    CHECK(zone_expansion(229 | 0x8000) == 6);
    CHECK(zone_expansion(65000) == 0);
}
