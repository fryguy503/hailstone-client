// The map: MapViewWnd (CMapViewWnd), MapToolbarWnd (CMapToolbarWnd), EditLabelWnd (CEditLabelWnd) and the map's
// own drawing and mouse, eq.DrawMap / MapZoom / MapMouseDown / MapMouseUp / MapPanReset / MapRightClick
// (for the packs' map.lua and maptoolbar.lua). The map's
// state and rules are game/map_data.h (MapViewMap); this file reads and writes the map files, keeps the window
// settings in the character's UI ini as CMapViewWnd::LoadIniInfo / StoreIniInfo do, and draws as
// MapViewMap::Draw @0x6cd4a0 draws:
//   - the lines in their colours, yellow (0xffffff00) when hovered (editing), marked (editing) or in the lasso;
//   - the line being walked, from its last point to the player, in the current colour;
//   - the find path (PlayerPath, game/player_path.h) from the player to the destination with an X there, else a
//     failed find's X, else the selected label's X;
//   - each label's 3-pixel cross and, with Labels on, a leader line and its text where the label placement put it
//     (FontsArray[size - 1], wrapped at 300 pixels); hovered or marked, yellow and shown even with Labels off;
//   - group members as an X in the group colour (alpha 0xbf, 0xff and named when hovered);
//   - the player: with Sense Heading above 190 an arrow along the heading, else a plus;
//   - "Range = n" in black at the bottom right (FontsArray[1]).
// The map shows only these: no other players, NPCs or the target (the XML's ShowPCsButton, ShowNPCsButton and
// CustomFilterEditbox have no code behind them in RoF2's CMapViewWnd, whose 40 child lookups skip them). A server's
// plugin may add spawns (ServerExtension::map_spawn), as that server's own client does: a plus in its colour, named
// like the labels; the minimap and the zone map show them too.
//
// Pixels are the original's: the render area's size in XML pixels; DrawMap's scale argument (the window's) puts
// them on the screen.

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_eq_windows.h"
#include "core/ascii_case.h"
#include "core/atomic_file.h"
#include "core/distribution.h"
#include "core/frame_sections.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "game/eq_names.h"
#include "game/atlas.h"
#include "game/combat.h"
#include "game/coords.h"
#include "game/map_data.h"
#include "game/map_files.h"
#include "game/mechanics/character.h"
#include "game/sine_table.h"
#include "game/slash_commands.h"
#include "game/world_state.h"
#include "game/zone_names.h"
#include "game/zone_routes.h"
#include "net/game_connection.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"
#include "ui/ui_ini.h"

namespace hailstone::app {

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kHighlight = 0xffffff00; // MapViewMap::Draw's highlight colour
constexpr std::uint32_t kEqStrManualZoom = 5459; // "Manually zooming and panning is disabled ..."
constexpr std::uint32_t kEqStrRange = 6730;      // "Range = %1"
constexpr std::uint32_t kEqStrSaved = 12322;     // "%1 saved."
constexpr std::uint32_t kEqStrNoJoin = 0x3ad0;   // JoinLinesAtIntersect's refusal
constexpr int kSenseHeading = 40;
constexpr int kToggleFindWindow = 0x162;         // __ExecuteCmd: the Find window (MVW_Find)
constexpr int kToggleZoneGuide = 0x1c6;          // __ExecuteCmd: the zone guide (MVW_MapZoneGuide)

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The EQ fonts by CXWndManager::FontsArray index: the XML <Font> numbers. Font 0 is not loaded here; it is drawn
// as font 1 (docs/unknowns.md).
constexpr std::array<float, 7> kFontPixels = {10, 10, 12, 13, 14, 16, 20};

struct MapFont {
    ImFont* font = nullptr;
    float scale = 1.0f; // ImGui pixels per original pixel (the font is rasterized at the display scale)
    float height = 12.0f; // in original pixels
};

MapFont map_font(int index) {
    int n = std::clamp(index, 1, 6);
    MapFont f;
    f.font = ui::eq_font(n);
    if (f.font) {
        f.scale = f.font->FontSize / kFontPixels[static_cast<std::size_t>(n)];
        f.height = f.font->FontSize / f.scale;
    } else {
        f.height = kFontPixels[static_cast<std::size_t>(n)];
    }
    return f;
}

float text_width(const MapFont& f, std::string_view text) {
    if (!f.font || !ImGui::GetCurrentContext()) return static_cast<float>(text.size()) * f.height * 0.5f;
    return f.font->CalcTextSizeA(f.font->FontSize, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x / f.scale;
}

// GetTextExtent / GetHeight of FontsArray[size - 1].
std::pair<int, int> measure_label(std::string_view text, int size) {
    MapFont f = map_font(size - 1);
    return {static_cast<int>(std::lround(text_width(f, text))), static_cast<int>(std::lround(f.height))};
}

std::optional<std::string> read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream s;
    s << in.rdbuf();
    std::string text = s.str();
    // "rt": CR LF reads as LF.
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
        if (!(text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')) out += text[i];
    return out;
}

ImU32 color(std::uint32_t argb, int alpha = -1) {
    std::uint32_t a = alpha >= 0 ? static_cast<std::uint32_t>(alpha) : (argb >> 24);
    return IM_COL32((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff, a);
}

// A colour with its alpha scaled by Auto Z's fade (MapView::z_fade).
ImU32 faded(ImU32 c, float f) {
    if (f >= 1.0f) return c;
    auto a = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xff);
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::lround(a * f)) << IM_COL32_A_SHIFT);
}

float segment_distance(float px, float py, float ax, float ay, float bx, float by) {
    // MapViewMap::Draw's line hover (0x6cd9fb..): the projection on the segment, the nearer end past it.
    float dx = bx - ax, dy = by - ay;
    float t = (px - ax) * dx + (py - ay) * dy;
    float ex = px - ax, ey = py - ay;
    if (t > 0.0f) {
        float len = dx * dx + dy * dy;
        if (t < len) {
            ex = px - (t / len * dx + ax);
            ey = py - (t / len * dy + ay);
        } else {
            ex = px - bx;
            ey = py - by;
        }
    }
    return std::sqrt(ex * ex + ey * ey);
}

bool parse_bool(const std::optional<std::string>& v, bool fallback) {
    if (!v || v->empty()) return fallback;
    char c = (*v)[0];
    return c == 't' || c == 'T' || c == '1';
}

int parse_int(const std::optional<std::string>& v, int fallback) {
    if (!v) return fallback;
    return std::atoi(v->c_str());
}

// ---------------------------------------------------------------- the shared state

struct MapState {
    game::MapView view;
    std::string zone;      // the zone whose map is loaded (CMapViewWnd +0x260 szZoneName)
    bool box_valid = false; // the zone's box was there when it was loaded
    bool editing = false;  // +0x354: the toolbar was opened from the map (the map is saved when it changes)
    bool automap = false;  // +0x228
    // OnProcessFrame's automap memory (0xb32f00.., 0xb32ee8): the last position and slope.
    float last_x = -424242.0f, last_y = -424242.0f, last_z = -424242.0f;
    double last_slope = -5000.0;
    // The maps directory (CEQSuiteTextureLoader skin 3, "Main" "MapSkin"): "" for maps\ itself.
    std::string skin;
    ComboData skins;
    // The settings file, and where map edits are written: the server's maps folder (EqHost::maps_dir), else maps/
    // in the character's folder.
    ui::UiIni* ini = nullptr;
    fs::path user_dir;
    fs::path maps_dir;
    // The search (MVW_SearchBox / MVW_SearchList).
    std::string search;
    bool search_shown = false;
    ListData search_rows;
    std::vector<int> search_ids;
    std::string min_z_text = "10", max_z_text = "10";
    // The edit label window (CEditLabelWnd_Setup): a new label, or the marked one.
    bool label_new = true;
    int label_id = 0;
    std::string label_text;
    int label_size = 1;
    std::uint32_t label_color = 0xff000000;
    // The mouse (MapViewMap::Draw's 0xf73dfc / 0xf73e00 and the drag globals 0xf73e04..).
    game::MapLine* hover_line = nullptr;
    game::MapLabel* hover_label = nullptr;
    bool panning = false, lasso = false;
    float anchor_x = 0, anchor_y = 0;
    bool drawn = false; // DrawMap ran this frame (the mouse functions use its mapping)
    float ox = 0, oy = 0, k = 1, area_w = 0, area_h = 0;
    bool toolbar_open = false;
    bool label_window_open = false;
    std::string player_zone; // the zone the map was loaded for when the player came in (CDisplay::StartWorldDisplay)

    // ---- the atlas page (AtlasWnd in MVW_AtlasRenderArea)
    int tab = 0;                          // MVW_Subwindows: 0 the map page, 1 the atlas page
    std::string atlas_skin = "Default";   // Atlas\<skin>\ (CEQSuiteTextureLoader skin 1, "Main" "AtlasSkin")
    ComboData atlas_skins;
    std::string atlas_file;               // the page shown
    game::AtlasPage atlas;
    bool atlas_loaded = false;
    std::optional<game::AtlasIndex> atlas_index; // every page and zone link (built when first wanted)
    std::map<std::string, game::AtlasPage> atlas_pages; // the modern map's pages (eq.AtlasPageData), by lower-case file
    std::string atlas_pending;            // AtlasWnd +0x220: a page link clicked, opened on the next frame

    // CMapViewWnd's constructor: the zone box +-1000 at zoom 0 (range 200), then SetZoom(2) (range 1000), which
    // later zones keep (SetZoneExtents keeps the range while zoomed).
    MapState() {
        view.set_zone_extents(-1000, 1000, -1000, 1000);
        view.set_zoom(2.0f);
    }

    std::optional<glm::vec3> player_map(const EqHost& h) const {
        if (!h.feet_valid) return std::nullopt;
        return game::world_to_map(glm::vec3(h.feet[0], h.feet[1], h.feet[2]));
    }
    std::optional<float> feet_z(const EqHost& h) const {
        return h.feet_valid ? std::optional<float>(h.feet[2]) : std::nullopt;
    }
    bool edit_mode() const { return toolbar_open; }

    // 0x6cbfa0: the player's and the selected label's box (the original's minY, maxY, minX, maxX).
    std::optional<glm::vec4> label_box(const EqHost& h) const {
        auto p = player_map(h);
        if (!p || !view.selected_label) return std::nullopt;
        glm::vec4 b(-p->y, -p->y, -p->x, -p->x);
        float y = -view.selected_label->pos.y, x = -view.selected_label->pos.x;
        if (y < b[0]) b[0] = y;
        else if (b[1] < y) b[1] = y;
        if (x < b[2]) b[2] = x;
        else if (b[3] < x) b[3] = x;
        return b;
    }

    void say(EqHost& h, const std::string& text) {
        if (h.chat) h.chat->add(ui::ChatChannel::System, text);
    }
    std::string str(EqHost& h, std::uint32_t id, std::vector<std::string> args = {}) {
        return h.conn ? h.conn->strings().format(id, args) : std::string();
    }

    // The player's own edits on this server: empty when there is neither a server maps folder nor a character.
    fs::path user_root() const { return !maps_dir.empty() ? maps_dir : user_dir.empty() ? fs::path() : user_dir / "maps"; }
    // Where the toolbar saves: the selected maps directory in user_root().
    fs::path user_maps() const {
        fs::path p = user_root();
        return p.empty() || skin.empty() ? p : p / skin;
    }

    // The zone's four layers from one place (game/map_files.h): the player's edits on this server, the server's
    // assets, the global maps folder, then the game's.
    game::MapFiles map_files(const EqHost& h, const std::string& name) const {
        return game::find_map_files({user_root(), h.game, h.global_maps_dir}, name, skin);
    }
    void load_layers(const EqHost& h) {
        game::MapFiles files = map_files(h, zone);
        view.load(
            [&](int layer) {
                const auto& f = files.layers[static_cast<std::size_t>(layer)];
                return f ? read_text(*f) : std::nullopt;
            },
            measure_label);
        if (files.source != game::MapSource::None)
            log::debug("map: {} from the {} maps ({})", zone, game::map_source_name(files.source),
                       files.dir.empty() ? "maps" : files.dir);
    }

    // MapViewMap_SaveLayerFile @0x6cc710: the four layer files.
    bool save(EqHost& h) {
        // A zone name that is not a plain file name (from a hostile server) is never written: map_layer_file_name.
        if (zone.empty() || user_maps().empty() || game::map_layer_file_name(zone, 0).empty()) return false;
        std::error_code ec;
        fs::create_directories(user_maps(), ec);
        bool ok = true;
        for (int layer = 0; layer < 4; ++layer) {
            fs::path p = user_maps() / game::map_layer_file_name(zone, layer);
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            if (!out) {
                log::warn("Failed to open {} for writing in MapViewMap::SaveEx()", p.string());
                ok = false;
                continue;
            }
            out << view.layer_text(layer);
        }
        (void)h;
        return ok;
    }

    // CMapViewWnd_ScanMapFiles: "Default", then each directory under maps\, here in every source (game/map_files.h).
    void scan_skins(const EqHost& h) {
        skins.choices = {"Default"};
        skins.selected = 0;
        for (auto& d : game::map_skins({user_root(), h.game, h.global_maps_dir})) {
            skins.choices.push_back(d);
            if (lower(d) == lower(skin)) skins.selected = static_cast<int>(skins.choices.size()) - 1;
        }
    }

    // CDisplay_LoadWorldExtents + CMapViewWnd_LoadZoneMap for the current zone.
    void load_zone(EqHost& h, const std::string& name) {
        if (editing && !zone.empty()) save(h); // 0x6ccea0
        if (!toolbar_open) editing = false;
        if (name != zone) clear_search(); // 0x6c93a0
        zone = name;
        view.current_zone = true;
        view.clear();
        scan_skins(h);
        view.pan_x = view.pan_y = 0;
        load_layers(h);
        box_valid = h.zone_box_valid;
        if (h.zone_box_valid) {
            view.set_world_box(glm::vec3(h.zone_box_lo[0], h.zone_box_lo[1], h.zone_box_lo[2]),
                               glm::vec3(h.zone_box_hi[0], h.zone_box_hi[1], h.zone_box_hi[2]));
        } else {
            // "Failed to Get World Extents, using default values."
            view.set_world_box(glm::vec3(-2000.0f), glm::vec3(2000.0f));
        }
        log::info("map: {} ({} lines, {} labels, maps dir \"{}\")", zone, view.lines.size(), view.labels.size(), skin);
    }

    // A zone's map that is not the player's (CMapViewWnd_LoadZoneMap for another zone): its layers, the box of its
    // lines (0x6c8fc0: their ends' extremes, 0 included), no player.
    void load_other_zone(EqHost& h, const std::string& name) {
        if (editing && !zone.empty()) save(h); // 0x6ccea0
        if (!toolbar_open) editing = false;
        if (name != zone) clear_search();
        zone = name;
        view.clear();
        view.pan_x = view.pan_y = 0;
        load_layers(h);
        view.current_zone = false;
        glm::vec2 lo(0.0f), hi(0.0f);
        for (const auto& l : view.lines)
            for (const glm::vec3& p : {l.start, l.end}) {
                lo = glm::min(lo, glm::vec2(p));
                hi = glm::max(hi, glm::vec2(p));
            }
        auto ftol = [](float f) { return static_cast<int>(f); }; // __ftol2_sse: truncation
        view.set_zone_extents(ftol(lo.x), ftol(hi.x), ftol(lo.y), ftol(hi.y));
        box_valid = true;
        log::info("map: {} ({} lines, {} labels, another zone)", zone, view.lines.size(), view.labels.size());
    }

    // ---- the atlas
    std::optional<std::string> read_atlas(const EqHost& h, const std::string& file) const {
        if (!h.game) return std::nullopt;
        auto data = h.game->read("Atlas/" + atlas_skin + "/" + file);
        if (!data) return std::nullopt;
        return std::string(reinterpret_cast<const char*>(data->data()), data->size());
    }
    // AtlasWnd_LoadPage @0x620260 (clearing the page first): a page that cannot be read leaves an empty page with the
    // reader's error; any error says "Atlas Page Error" (the original also writes AtlasPageError.txt into the game
    // folder, which this client leaves alone).
    void load_atlas(EqHost& h, const std::string& file) {
        atlas_loaded = true;
        atlas_file = file;
        auto text = read_atlas(h, file);
        atlas = text ? game::parse_atlas_page(*text) : game::AtlasPage{};
        if (!text) atlas.errors = "Failed to open file";
        if (!atlas.errors.empty()) {
            say(h, "Atlas Page Error");
            log::info("atlas: {}: Page Error: {}", file, atlas.errors);
        }
    }
    // AtlasWnd_LoadDefaultPage @0x620760: the pages reachable from Default.xml through their page links, read for
    // their zone links (ClickAction 2), the first page linking a zone kept for it (game::index_atlas).
    const game::AtlasIndex& index_atlas(const EqHost& h) {
        if (!atlas_index) atlas_index = game::index_atlas([&](const std::string& f) { return read_atlas(h, f); });
        return *atlas_index;
    }
    // The atlas directory changed: everything read from the old one goes.
    void forget_atlas() {
        atlas_index.reset();
        atlas_pages.clear();
    }

    // Whether the server has the zone (docs/navigation.md "Zone atlas"): in the zones its list allows (its current
    // expansion and content flags), else, without that list, in the account's expansions (the profile's, else
    // world's OP_ExpansionInfo bits: expansion n is bit n - 1, Classic always). EQEmu fills those bits from
    // World:ExpansionSettings or the client version, not Expansion:CurrentExpansion, so without the script a server
    // that limits only its content shows every zone.
    bool zone_allowed(const EqHost& h, std::uint32_t id) const {
        if (!routes.server_zones().empty()) return routes.server_zones().contains(id);
        std::uint32_t mask = 0;
        if (h.conn) {
            const auto& p = h.conn->profile();
            mask = p && p->expansions ? p->expansions : h.conn->world_expansions();
        }
        const std::uint32_t e = game::zone_expansion(id);
        if (mask == 0 || e == 0 || e > 32) return true;
        return (mask & (1u << (e - 1))) != 0;
    }

    // ---- the zone links (game/zone_routes.h), kept in servers/<profile>/zone_links.txt
    game::ZoneRoutes routes;
    fs::path routes_dir;                 // the server folder they were read from
    bool routes_read = false;
    std::uint32_t routes_learned_zone = 0; // the zone whose zone points were last learned
    std::uint32_t routes_serial = 0;       // the connection's zone_links_serial() taken
    std::size_t routes_zones = 0;          // routes.zones().size(), kept as the links change
    void save_routes() {
        routes_zones = routes.zones().size();
        if (routes_dir.empty()) return;
        std::error_code ec;
        fs::create_directories(routes_dir, ec);
        if (!write_file_atomic(routes_dir / "zone_links.txt", routes.serialize()))
            log::warn("map: could not write {}", (routes_dir / "zone_links.txt").string());
    }
    // Each frame: the server's folder read once, the zone points of each zone entered learned, the server's list
    // taken when it comes.
    void update_routes(const EqHost& h) {
        if (!routes_read || h.server_dir != routes_dir) {
            routes_read = true;
            routes_dir = h.server_dir;
            routes = {};
            routes_learned_zone = 0;
            routes_serial = 0;
            if (!routes_dir.empty()) {
                std::ifstream in(routes_dir / "zone_links.txt", std::ios::binary);
                if (in) {
                    std::stringstream ss;
                    ss << in.rdbuf();
                    routes = game::ZoneRoutes::parse(ss.str());
                }
            }
            routes_zones = routes.zones().size();
        }
        if (!h.conn) return;
        const std::uint32_t zid = h.conn->zone_id();
        if (zid != 0 && zid != routes_learned_zone && !h.conn->zone_points().empty()) {
            routes_learned_zone = zid;
            std::vector<std::uint32_t> to;
            for (const auto& p : h.conn->zone_points()) to.push_back(p.zone_id);
            if (routes.learn(zid, to)) save_routes();
        }
        if (h.conn->zone_links_serial() != routes_serial) {
            routes_serial = h.conn->zone_links_serial();
            if (const auto& z = h.conn->zone_links()) {
                std::vector<game::ZoneRoutes::Link> links;
                links.reserve(z->links.size());
                for (const auto& [from, to] : z->links) links.emplace_back(from, to);
                bool changed = routes.set_server(std::move(links));
                if (z->has_zones)
                    changed = routes.set_server_zones(std::vector<std::uint32_t>(z->zones.begin(), z->zones.end()), z->expansion) || changed;
                if (changed) save_routes();
            }
        }
    }
    // CMapViewWnd_LoadAtlasPageForZone @0x6c9810 (the map icon button): the page with a link to the zone, and the
    // atlas page shown; else Default.xml (the tab stays). The original also tries the zones of the same zone guide
    // continent, which EQEmu does not send.
    void atlas_for_zone(EqHost& h, const std::string& short_name) {
        if (const auto* z = index_atlas(h).find_zone(short_name)) {
            load_atlas(h, z->page);
            tab = 1;
            return;
        }
        load_atlas(h, "Default.xml");
    }
    // The atlas directories (Atlas\*), the chosen one selected.
    void scan_atlas_skins(const EqHost& h) {
        atlas_skins.choices.clear();
        atlas_skins.selected = -1;
        std::vector<std::string> dirs;
        if (h.game)
            for (auto& d : h.game->list_dirs("Atlas"))
                if (!d.empty() && d[0] != '.') dirs.push_back(std::move(d));
        std::sort(dirs.begin(), dirs.end());
        for (auto& d : dirs) {
            if (lower(d) == lower(atlas_skin)) atlas_skins.selected = static_cast<int>(atlas_skins.choices.size());
            atlas_skins.choices.push_back(d);
        }
    }

    void clear_search() {
        search.clear();
        search_shown = false;
        search_rows.rows.clear();
        search_ids.clear();
    }

    // sub_006cc480 / 0x6cc1a0: the labels whose text holds the search text (all when it is empty).
    void run_search() {
        search_rows = ListData{};
        search_ids.clear();
        std::string want = lower(search);
        for (const auto& l : view.labels) {
            if (!want.empty() && lower(l.text).find(want) == std::string::npos) continue;
            ListRow r;
            r.texts = {l.text};
            search_rows.rows.push_back(r);
            search_ids.push_back(l.id);
        }
        search_shown = true;
    }

    // CMapViewWnd::StoreIniInfo @0x6ca580.
    void store_ini() {
        if (!ini) return;
        const char* s = "MapViewWnd";
        auto put_bool = [&](const char* key, bool v) { ini->put(s, key, v ? "true" : "false"); };
        auto put_int = [&](const char* key, int v) { ini->put(s, key, std::to_string(v)); };
        auto put_color = [&](const char* key, std::uint32_t c) {
            put_int((std::string(key) + ".red").c_str(), static_cast<int>((c >> 16) & 0xff));
            put_int((std::string(key) + ".green").c_str(), static_cast<int>((c >> 8) & 0xff));
            put_int((std::string(key) + ".blue").c_str(), static_cast<int>(c & 0xff));
        };
        put_bool("ShowGroup", view.show_group);
        put_color("MyColor", view.my_color);
        put_color("GroupColor", view.group_color);
        put_bool("FindAutoZoom", view.find_auto_zoom);
        put_color("FindPathColor", view.path_color);
        char layers[8];
        std::snprintf(layers, sizeof(layers), "%d%d%d%d", view.layer_filter & 1, (view.layer_filter >> 1) & 1,
                      (view.layer_filter >> 2) & 1, (view.layer_filter >> 3) & 1);
        ini->put(s, "LayersVisible", layers);
        put_int("ActiveLayer", view.active_layer);
        put_bool("LabelsActive", view.labels_active);
        put_bool("HeightFilter", view.z_filter);
        put_int("HeightLow", view.min_z);
        put_int("HeightHigh", view.max_z);
        // Not the original's: Auto Z (MapView::auto_z).
        put_bool("AutoZ", view.auto_z);
        put_int("AutoZAbove", view.auto_z_above);
        put_int("AutoZBelow", view.auto_z_below);
    }

    // CMapViewWnd::LoadIniInfo @0x6c9aa0 (each key's default is the current value).
    void load_ini() {
        if (!ini) return;
        const char* s = "MapViewWnd";
        view.show_group = parse_bool(ini->get(s, "ShowGroup"), view.show_group);
        auto get_color = [&](const char* key, int r, int g, int b) {
            r = parse_int(ini->get(s, std::string(key) + ".red"), r);
            g = parse_int(ini->get(s, std::string(key) + ".green"), g);
            b = parse_int(ini->get(s, std::string(key) + ".blue"), b);
            return 0xff000000u | (static_cast<std::uint32_t>(r & 0xff) << 16) | (static_cast<std::uint32_t>(g & 0xff) << 8) |
                   static_cast<std::uint32_t>(b & 0xff);
        };
        view.my_color = get_color("MyColor", 0, 0, 0);
        view.group_color = get_color("GroupColor", 0, 0, 0);
        view.find_auto_zoom = parse_bool(ini->get(s, "FindAutoZoom"), view.find_auto_zoom);
        view.path_color = get_color("FindPathColor", 255, 0, 0);
        if (auto v = ini->get(s, "LayersVisible")) {
            view.layer_filter = 0;
            for (int i = 0; i < 4 && i < static_cast<int>(v->size()); ++i)
                if ((*v)[static_cast<std::size_t>(i)] == '1') view.layer_filter |= 1 << i;
        }
        view.active_layer = parse_int(ini->get(s, "ActiveLayer"), view.active_layer);
        view.labels_active = parse_bool(ini->get(s, "LabelsActive"), view.labels_active);
        view.z_filter = parse_bool(ini->get(s, "HeightFilter"), view.z_filter);
        view.min_z = parse_int(ini->get(s, "HeightLow"), view.min_z);
        view.max_z = parse_int(ini->get(s, "HeightHigh"), view.max_z);
        view.auto_z = parse_bool(ini->get(s, "AutoZ"), view.auto_z);
        // AutoZRange: the single range of earlier versions, the default for both sides.
        const int range = parse_int(ini->get(s, "AutoZRange"), view.auto_z_above);
        view.auto_z_above = std::clamp(parse_int(ini->get(s, "AutoZAbove"), range), 1, game::MapView::kAutoZMaxRange);
        view.auto_z_below = std::clamp(parse_int(ini->get(s, "AutoZBelow"), range), 1, game::MapView::kAutoZMaxRange);
        min_z_text = std::to_string(view.min_z);
        max_z_text = std::to_string(view.max_z);
        view.relayout = true;
        if (auto v = ini->get("Main", "MapSkin")) skin = *v;
        if (auto v = ini->get("Main", "AtlasSkin"); v && !v->empty()) atlas_skin = *v;
    }

    // The find path while one is shown (pinstPlayerPath's +0).
    const game::PlayerPath* path(const EqHost& h) const {
        return h.conn && h.conn->player_path().active() ? &h.conn->player_path() : nullptr;
    }
    // 0x6c8760: with Auto-Zoom on, the map on the player's zone and a find path or a failed find's X shown,
    // the map follows the find: zoom and pan are held (string 5459).
    bool find_locked(const EqHost& h) const {
        return view.find_auto_zoom && view.current_zone && h.feet_valid && (path(h) || view.failed_find);
    }
    // 0x6ccef0: the box of the player and the path's points (or the failed find's point), the original's
    // (minY, maxY, minX, maxX); only while the find holds the map.
    std::optional<glm::vec4> find_box(const EqHost& h) const {
        if (!find_locked(h)) return std::nullopt;
        glm::vec4 b(h.feet[0], h.feet[0], h.feet[1], h.feet[1]); // the original's Y, X are world x, y
        auto add = [&](float y, float x) {
            if (y < b[0]) b[0] = y;
            else if (b[1] < y) b[1] = y;
            if (x < b[2]) b[2] = x;
            else if (b[3] < x) b[3] = x;
        };
        if (const auto* p = path(h)) {
            for (const auto& pt : p->points()) add(pt.x, pt.y);
        } else {
            add(view.failed_point.x, view.failed_point.y);
        }
        return b;
    }
    // The zone the failed find's X belongs to (CMapViewWnd +ZoneId: a new zone takes it away).
    std::string failed_zone;

    // 0x6c9330 / 0x6cc410: the automap (MTW_AutoMapButton) starts a line where the player stands and ends it.
    void start_automap(const EqHost& h) {
        auto p = player_map(h);
        if (automap || !p || !view.current_zone) return;
        if (!view.line_active) view.start_line(*p);
        automap = true;
        last_slope = -5000.0;
    }
    void stop_automap(const EqHost& h) {
        if (automap) {
            automap = false;
            if (auto p = player_map(h)) view.end_line(*p);
        }
    }

    // CMapViewWnd::OnProcessFrame @0x6cf530's automap: a point is added where the walked direction turned.
    void automap_step(const EqHost& h) {
        auto p = player_map(h);
        if (!automap || !p) {
            last_x = last_y = last_z = -424242.0f;
            return;
        }
        float px = p->x, py = p->y, pz = p->z;
        float lx = last_x;
        if (last_x == -424242.0f) {
            last_x = lx = px;
            last_y = py;
            last_z = pz;
        }
        double dx = std::fabs(static_cast<double>(view.current_point.x) - px);
        double dy = std::fabs(static_cast<double>(view.current_point.y) - py);
        double d2 = dx * dx + dy * dy;
        if (d2 > 4.0) {
            double tol = d2 < 100.0 ? 0.05 : d2 < 400.0 ? 0.01 : d2 < 900.0 ? 0.005 : 0.001;
            double slope = dx > 0.5 ? dy / dx : 100000000.0;
            if (last_slope == -5000.0) {
                last_slope = slope;
            } else if (tol < std::fabs(last_slope - slope)) {
                last_slope = slope;
                view.add_point(glm::vec3(lx, last_y, last_z));
            }
        }
        last_x = px;
        last_y = py;
        last_z = pz;
    }
};

// ---------------------------------------------------------------- MapViewWnd (CMapViewWnd)

class MapViewWindow : public StockWindow {
public:
    explicit MapViewWindow(std::shared_ptr<MapState> s) : StockWindow("MapViewWnd", false), s_(std::move(s)) {}
    MapState& state() { return *s_; }

    void frame(EqHost& h) override {
        MapState& s = *s_;
        ++frames_;
        if (!h.in_game) return;
        const std::string& zone = h.zone_short_name;
        if (zone.empty()) return;
        s.update_routes(h);
        // The player's zone on zone-in (and when the box comes); an emptied map (Current Zone, the maps directory)
        // loads the player's zone again. Another zone's map, chosen in the atlas, stays until then.
        if (zone != s.player_zone || s.zone.empty() || (zone == s.zone && h.zone_box_valid && !s.box_valid)) {
            s.player_zone = zone;
            s.load_zone(h, zone);
        }
        if (!s.atlas_pending.empty()) s.load_atlas(h, std::exchange(s.atlas_pending, std::string()));
        if (open_ && !s.atlas_loaded) s.load_atlas(h, "Default.xml");
        auto& v = s.view;
        if (h.conn)
            if (auto f = h.conn->take_failed_find()) {
                // 0x6c92e0: the map window shows the failed find's X.
                open_ = true;
                v.failed_find = true;
                v.failed_point = f->point;
                v.failed_kind = static_cast<int>(f->kind);
                s.failed_zone = zone;
            }
        // CMapViewWnd::OnProcessFrame: the X goes with the zone, with a find path, or once the player is within 10
        // of it (in the plane for a map location).
        if (v.failed_find && s.failed_zone != zone) v.failed_find = false;
        if (v.failed_find && !s.path(h) && h.feet_valid) {
            glm::vec3 d = glm::vec3(h.feet[0], h.feet[1], h.feet[2]) - v.failed_point;
            if (v.failed_kind == static_cast<int>(game::FindKind::MapLocation)) d.z = 0.0f;
            if (glm::length(d) < 10.0f) v.failed_find = false;
        } else if (v.failed_find && s.path(h)) {
            v.failed_find = false;
        }
        if (open_) {
            s.automap_step(h);
            // 0x6ceec0: while the find holds the map, it is zoomed to the find's box (up to x20 for a failed
            // find's X).
            if (auto b = s.find_box(h); b && s.area_w > 0 && s.area_h > 0) {
                auto me = s.player_map(h);
                v.precalc(me ? std::optional<glm::vec2>(glm::vec2(*me)) : std::nullopt, s.label_box(h), b);
                v.zoom_to_fit((*b)[0], (*b)[1], (*b)[2], (*b)[3], v.failed_find, s.area_w, s.area_h);
            }
        }
        s.drawn = false;
    }

    void ini_loaded(EqHost& h, ui::UiIni& ini, const fs::path& user_dir) override {
        s_->ini = &ini;
        s_->user_dir = user_dir;
        s_->maps_dir = h.maps_dir;
        s_->load_ini();
        s_->zone.clear(); // read again (the player's own map edits may live there)
    }

    void set_shown(EqHost& h, bool show) override {
        if (!show && open_ && s_->toolbar_open) s_->toolbar_open = false; // CMapViewWnd::OnShow hides the toolbar
        if (show && !open_) {                                              // AboutToShow
            s_->scan_skins(h);
            s_->scan_atlas_skins(h);
        }
        if (!show) s_->panning = s_->lasso = false;                       // AboutToHide
        open_ = show;
    }

    void child(EqHost& h, std::string_view id, Child& out) override {
        auto& v = s_->view;
        if (id == "MVW_ShowGroupButton") out.checked = v.show_group;
        else if (id == "MVW_FindAutoZoom") out.checked = v.find_auto_zoom;
        else if (id == "MVW_LabelsToggleButton") out.checked = v.labels_active;
        else if (id == "MVW_ZFilterButton") out.checked = v.z_filter;
        else if (id.size() == 16 && id.substr(0, 9) == "MVW_Layer" && id.substr(10) == "Button") {
            int n = id[9] - '0';
            out.checked = (v.layer_filter & (1 << n)) != 0;
        } else if (id.size() == 22 && id.substr(0, 9) == "MVW_Layer" && id.substr(10) == "ActiveButton") {
            out.checked = v.active_layer == id[9] - '0';
        } else if (id == "MVW_MinZEditBox") out.text = s_->min_z_text;
        else if (id == "MVW_MaxZEditBox") out.text = s_->max_z_text;
        else if (id == "MVW_ToolbarButton") out.enabled = pop(h); // disabled without the Planes of Power
        else if (id == "MVW_Find") out.shown = !(s_->path(h) || v.failed_find); // OnProcessFrame: Find, or
        else if (id == "MVW_EndFind") out.shown = s_->path(h) || v.failed_find;  // End Find with a find shown
        else if (id == "MVW_SearchBox") out.text = s_->search;
        // The search list, its Hide button and the layout box holding the list and its spacer show with a search
        // (sub_006cc480 shows wndSearchLayout too; hidden, its spacer would lie over the map).
        else if (id == "MVW_SearchList" || id == "MVW_SearchHideButton" || id == "MVW_Spacer") out.shown = s_->search_shown;
        else if (id == "MapPage" && !s_->zone.empty()) { // CMapViewWnd_LoadZoneMap: "%1 - Map"
            std::string name = s_->zone == h.zone_short_name ? h.zone_long_name
                                                             : game::zone_long_name(game::zone_id_by_short_name(s_->zone));
            if (!name.empty()) out.text = s_->str(h, 6729, {name});
        }
    }

    bool click(EqHost& h, std::string_view id, std::string_view button, bool) override {
        if (button != "left") return false;
        MapState& s = *s_;
        auto& v = s.view;
        auto held = [&] {
            if (!s.find_locked(h)) return false;
            s.say(h, s.str(h, kEqStrManualZoom));
            return true;
        };
        if (id == "MVW_LoadCurrent") s.zone.clear(); // read again next frame
        else if (id == "MVW_ZoomInButton") { if (!held()) v.zoom_in(); }
        else if (id == "MVW_ZoomOutButton") { if (!held()) v.zoom_out(); }
        else if (id == "MVW_PanUpButton") { if (!held()) v.pan_y += 5; }
        else if (id == "MVW_PanDownButton") { if (!held()) v.pan_y -= 5; }
        else if (id == "MVW_PanLeftButton") { if (!held()) v.pan_x += 5; }
        else if (id == "MVW_PanRightButton") { if (!held()) v.pan_x -= 5; }
        else if (id == "MVW_PanResetButton") { if (!held()) v.pan_x = v.pan_y = 0; }
        else if (id == "MVW_ShowGroupButton") v.show_group = !v.show_group;
        else if (id == "MVW_FindAutoZoom") v.find_auto_zoom = !v.find_auto_zoom;
        else if (id == "MVW_LabelsToggleButton") {
            v.labels_active = !v.labels_active;
            v.relayout = true;
        } else if (id == "MVW_ZFilterButton") {
            v.z_filter = !v.z_filter;
            v.relayout = true;
        } else if (id.size() == 16 && id.substr(0, 9) == "MVW_Layer" && id.substr(10) == "Button") {
            v.layer_filter ^= 1 << (id[9] - '0');
            v.relayout = true;
        } else if (id.size() == 22 && id.substr(0, 9) == "MVW_Layer" && id.substr(10) == "ActiveButton") {
            v.active_layer = id[9] - '0';
        } else if (id == "MVW_ToolbarButton") {
            if (!pop(h)) return true;
            s.toolbar_open = true; // CMapToolbarWnd's show (AboutToShow passes with the Planes of Power)
            s.editing = true;
        } else if (id == "MVW_Find" || id == "MVW_EndFind") {
            // A failed find's X goes; else the find key (0x162: the path stops, or the Find window toggles).
            if (v.failed_find) v.failed_find = false;
            else if (h.run_command) h.run_command(kToggleFindWindow);
        } else if (id == "MVW_MapZoneGuide" || id == "MVW_AtlasZoneGuide") {
            if (h.run_command) h.run_command(kToggleZoneGuide);
        } else if (id == "MVW_MapIconButton") {
            if (!s.zone.empty()) s.atlas_for_zone(h, s.zone); // CMapViewWnd_LoadAtlasPageForZone
        } else if (id == "MVW_SearchButton") s.run_search();
        else if (id == "MVW_SearchClearButton") s.clear_search();
        else if (id == "MVW_SearchHideButton" || id == "MVW_Spacer") {
            s.search_shown = false; // 0x6c7d80
            s.search_rows.rows.clear();
            s.search_ids.clear();
        } else {
            return false;
        }
        s.store_ini();
        return true;
    }

    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        MapState& s = *s_;
        // Notification 0xe: CXStr::ConvertToInt of the box's text.
        if (id == "MVW_MinZEditBox") {
            s.min_z_text = text;
            s.view.min_z = std::atoi(text.c_str());
        } else if (id == "MVW_MaxZEditBox") {
            s.max_z_text = text;
            s.view.max_z = std::atoi(text.c_str());
        } else if (id == "MVW_SearchBox") {
            s.search = text;
            return true;
        } else {
            return false;
        }
        s.view.relayout = true;
        s.store_ini();
        return true;
    }

    const ListData* list(EqHost&, std::string_view id) override {
        return id == "MVW_SearchList" && s_->search_shown ? &s_->search_rows : nullptr;
    }

    bool list_click(EqHost& h, std::string_view id, int row, bool right, int) override {
        MapState& s = *s_;
        if (id != "MVW_SearchList" || right || row < 0 || row >= static_cast<int>(s.search_ids.size())) return false;
        s.search_rows.selected = row;
        if (auto* l = s.view.label_by_id(s.search_ids[static_cast<std::size_t>(row)])) {
            s.view.selected_label = l;
            // 0x6ccdb0: back to the centre, zoomed to the player and the label.
            s.view.pan_x = s.view.pan_y = 0;
            if (auto b = s.label_box(h); b && s.area_w > 0 && s.area_h > 0) {
                s.view.precalc(s.player_map(h) ? std::optional<glm::vec2>(glm::vec2(*s.player_map(h))) : std::nullopt, b);
                s.view.zoom_to_fit((*b)[0], (*b)[1], (*b)[2], (*b)[3], false, s.area_w, s.area_h);
            }
        }
        return true;
    }

    const ComboData* combo(EqHost& h, std::string_view id) override {
        if (id == "MVW_AtlasDirCombobox") return &s_->atlas_skins;
        if (id != "MVW_MapsDirCombobox") return nullptr;
        // Hailstone: the Options window lists the maps directories with the map closed, so the list is read again
        // when something starts asking for it (not every frame: the game's listing walks its whole index), as the
        // map's AboutToShow does. A folder added since then is listed.
        if (!open_ && (!asked_ || *asked_ + 1 < frames_)) s_->scan_skins(h);
        asked_ = frames_;
        return &s_->skins;
    }

    std::optional<int> tab_page(EqHost&, std::string_view id) override {
        return id == "MVW_Subwindows" ? std::optional<int>(s_->tab) : std::nullopt;
    }
    void set_tab_page(EqHost&, std::string_view id, int page) override {
        if (id == "MVW_Subwindows") s_->tab = page == 1 ? 1 : 0;
    }

    bool combo_select(EqHost& h, std::string_view id, int index) override {
        MapState& s = *s_;
        if (id == "MVW_AtlasDirCombobox") {
            // The atlas directory: Default.xml of it, kept as Main AtlasSkin.
            if (index < 0 || index >= static_cast<int>(s.atlas_skins.choices.size())) return false;
            s.atlas_skins.selected = index;
            s.atlas_skin = s.atlas_skins.choices[static_cast<std::size_t>(index)];
            if (s.ini) s.ini->put("Main", "AtlasSkin", s.atlas_skin);
            s.forget_atlas();
            s.load_atlas(h, "Default.xml");
            return true;
        }
        if (id != "MVW_MapsDirCombobox" || index < 0 || index >= static_cast<int>(s.skins.choices.size())) return false;
        if (s.editing) s.save(h); // 0x6ccea0
        s.skins.selected = index;
        s.skin = index == 0 ? std::string() : s.skins.choices[static_cast<std::size_t>(index)];
        if (s.ini) s.ini->put("Main", "MapSkin", s.skin);
        s.zone.clear(); // CDisplay_LoadWorldExtents(0)
        return true;
    }

private:
    static bool pop(const EqHost& h) {
        // 0xde09c4: the Planes of Power expansion bit (CEverQuest: Expansions >> 4 & 1).
        return !h.character || (h.character->sheet().expansions & 0x10) != 0;
    }
    std::shared_ptr<MapState> s_;
    // The frames seen, and the last one the maps directories were asked for (combo).
    std::uint64_t frames_ = 0;
    std::optional<std::uint64_t> asked_;
};

// ---------------------------------------------------------------- MapToolbarWnd (CMapToolbarWnd)

class MapToolbarWindow : public StockWindow {
public:
    explicit MapToolbarWindow(std::shared_ptr<MapState> s) : StockWindow("MapToolbarWnd", false), s_(std::move(s)) {}

    bool shown(EqHost& h) override { return s_->toolbar_open && h.in_game; }
    void set_shown(EqHost& h, bool show) override {
        // AboutToShow @0x6c6ab0: with the Planes of Power bit, or in a newbie zone (EQWorldData::IsNewbieZone).
        if (show && h.character && !(h.character->sheet().expansions & 0x10) &&
            !(h.conn && game::is_newbie_zone(h.conn->zone_id())))
            return;
        s_->toolbar_open = show;
    }

    void child(EqHost&, std::string_view id, Child& out) override {
        if (id == "MTW_AutoMapButton") out.checked = s_->automap;
    }

    bool click(EqHost& h, std::string_view id, std::string_view button, bool) override {
        if (button != "left") return false;
        MapState& s = *s_;
        auto& v = s.view;
        auto me = s.player_map(h);
        if (id == "MTW_StartLineButton") { if (me) v.start_line(*me); }
        else if (id == "MTW_EndLineButton") { if (me) v.end_line(*me); }
        else if (id == "MTW_AddPointButton") { if (me) v.add_point(*me); }
        else if (id == "MTW_RemoveLineButton") v.remove_marked_lines();
        else if (id == "MTW_JoinAtIntersectButton") {
            if (!v.join_lines_at_intersect(ImGui::GetIO().KeyCtrl)) s.say(h, s.str(h, kEqStrNoJoin));
        } else if (id == "MTW_AddLabelButton") {
            open_label(true, nullptr);
        } else if (id == "MTW_EditLabelButton") {
            if (v.marked_label) open_label(false, v.marked_label);
        } else if (id == "MTW_MoveLabelToMeButton") {
            if (v.marked_label && me) {
                v.marked_label->pos = *me; // 0x6c80c0
                v.relayout = true;
            }
        } else if (id == "MTW_RemoveLabelButton") {
            v.remove_marked_label();
            v.relayout = true;
        } else if (id == "MTW_SaveMapButton") {
            if (s.save(h) && !s.zone.empty()) {
                std::string zone = s.zone;
                zone[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(zone[0])));
                s.say(h, s.str(h, kEqStrSaved, {zone}));
            }
        } else if (id == "MTW_AutoMapButton") {
            if (!s.automap) s.start_automap(h);
            else s.stop_automap(h);
        } else if (id == "MTW_ClearMapButton" || id == "MTW_ClearLayerButton") {
            // ConfirmationDialog(toolbar, 0x65 / 100, string 15060 / 15061, Yes / No (0xc), no time limit); Yes is
            // the toolbar's answer: 0x6ca9e0 (the whole map) / 0x6cbee0 (the active layer).
            bool whole = id == "MTW_ClearMapButton";
            EqHost::Confirm q;
            q.text = s.str(h, whole ? 15060u : 15061u);
            auto st = s_;
            q.done = [st, whole](bool yes) {
                if (!yes) return;
                if (whole) st->view.clear();
                else st->view.clear_active_layer();
                st->view.relayout = true;
            };
            h.confirm = std::move(q);
        } else if (id == "MTW_SetCurrentColorButton") {
            // CColorPickerWnd::Open(toolbar, 0x6c8b30: the current colour); 0x25 sets it (0x6c8b50).
            auto st = s_;
            h.pick_color(v.current_color, [st](std::uint32_t c) { st->view.current_color = 0xff000000u | (c & 0xffffff); });
        } else if (id == "MTW_ChangeLineColorButton") {
            // With a marked line: Open(toolbar, 0x6c8c40: the newest marked line's colour); 0x25 colours the
            // marked lines (0x6c8bf0) and clears the marks (0x6c8b80(0)).
            if (!v.marked_lines.empty() && v.marked_lines.front()) {
                auto st = s_;
                h.pick_color(v.marked_lines.front()->color, [st](std::uint32_t c) {
                    st->view.set_marked_lines_color(c);
                    st->view.mark_line(nullptr);
                });
            }
        } else if (id == "MTW_SetMyColorButton" || id == "MTW_SetGroupColorButton" || id == "MTW_SetPathColorButton") {
            // Open(toolbar, the colour); 0x25 stores 0xff000000 | the colour.
            std::uint32_t game::MapView::*field = id == "MTW_SetMyColorButton"      ? &game::MapView::my_color
                                                  : id == "MTW_SetGroupColorButton" ? &game::MapView::group_color
                                                                                    : &game::MapView::path_color;
            auto st = s_;
            h.pick_color(v.*field, [st, field](std::uint32_t c) { st->view.*field = 0xff000000u | (c & 0xffffff); });
        } else {
            return false;
        }
        return true;
    }

private:
    // CEditLabelWnd_Setup: a new label (empty, small, black) or the marked one.
    void open_label(bool fresh, const game::MapLabel* l) {
        MapState& s = *s_;
        s.label_new = fresh;
        s.label_id = l ? l->id : 0;
        s.label_text = l ? l->text : std::string();
        s.label_size = l ? l->size : 1;
        s.label_color = l ? (0xff000000u | (l->color & 0xffffff)) : 0xff000000u;
        s.label_window_open = true;
    }
    std::shared_ptr<MapState> s_;
};

// ---------------------------------------------------------------- EditLabelWnd (CEditLabelWnd)

class EditLabelWindow : public StockWindow {
public:
    explicit EditLabelWindow(std::shared_ptr<MapState> s) : StockWindow("EditLabelWnd", false), s_(std::move(s)) {}

    bool shown(EqHost& h) override { return s_->label_window_open && h.in_game; }
    void set_shown(EqHost&, bool show) override { s_->label_window_open = show; }

    void child(EqHost&, std::string_view id, Child& out) override {
        MapState& s = *s_;
        if (id == "ELW_LabelTextEditbox") out.text = s.label_text;
        else if (id == "ELW_LabelSmallButton") out.checked = s.label_size == 1;
        else if (id == "ELW_LabelMediumButton") out.checked = s.label_size == 2;
        else if (id == "ELW_LabelLargeButton") out.checked = s.label_size == 3;
    }

    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        if (id != "ELW_LabelTextEditbox") return false;
        s_->label_text = text.substr(0, 255); // MaxChars 0xff
        return true;
    }

    bool click(EqHost& h, std::string_view id, std::string_view button, bool) override {
        if (button != "left") return false;
        MapState& s = *s_;
        if (id == "ELW_LabelSmallButton") s.label_size = 1;
        else if (id == "ELW_LabelMediumButton") s.label_size = 2;
        else if (id == "ELW_LabelLargeButton") s.label_size = 3;
        else if (id == "ELW_CancelButton") s.label_window_open = false;
        else if (id == "ELW_DoneButton") {
            done(h);
            s.label_window_open = false;
        } else if (id == "ELW_ChooseColorButton") {
            // CColorPickerWnd::Open(this window, its colour); its notification 0x25 keeps the colour as given.
            auto st = s_;
            h.pick_color(s.label_color, [st](std::uint32_t c) { st->label_color = c; });
        } else {
            return false;
        }
        return true;
    }

private:
    // CMapToolbarWnd::WndNotification's notification 6 from the edit label window.
    void done(EqHost& h) {
        MapState& s = *s_;
        if (s.label_new) {
            if (auto me = s.player_map(h)) s.view.add_label(*me, s.label_color, s.label_size, s.label_text, measure_label);
        } else if (auto* l = s.view.label_by_id(s.label_id)) {
            l->size = s.label_size;
            l->color = (l->color & 0xff000000u) | (s.label_color & 0xffffff);
            l->text = s.label_text;
            game::MapView::measure_label(*l, measure_label);
        }
        s.view.relayout = true;
    }
    std::shared_ptr<MapState> s_;
};

// ---------------------------------------------------------------- the eq functions

MapState* state_of(lua_State* L) { return static_cast<MapState*>(lua_touserdata(L, lua_upvalueindex(1))); }
EqHost* host_of(lua_State* L) { return static_cast<EqHost*>(lua_touserdata(L, lua_upvalueindex(2))); }

// The group's other members in the zone, as Draw reads them (pinstLocalPC->Group->m_groupMembers[0..5]->spawn).
std::vector<const game::Spawn*> group_spawns(const EqHost& h) {
    std::vector<const game::Spawn*> out;
    if (!h.conn) return out;
    const auto& g = h.conn->group();
    if (!g.active) return out;
    const auto& w = h.conn->world();
    const game::Spawn* me = w.player();
    for (const auto& m : g.slots) {
        if (m.name.empty()) continue;
        for (const auto& [id, sp] : w.spawns) {
            if (&sp == me || !iequals(sp.name, m.name)) continue; // (each frame: no lower-cased copies)
            out.push_back(&sp);
            break;
        }
    }
    return out;
}

glm::vec3 spawn_map(const game::Spawn& s) {
    return game::world_to_map(s.display_valid ? s.display_position : s.position);
}

// The spawns a server's plugin shows on the maps (ServerExtension::map_spawn; the stock maps show none), each with
// its place in map coordinates. The plugin starts from the con colour and the name. With `in_view`, only the spawns
// whose place it keeps are asked about: the minimap draws a mark only inside its circle, and asking the plugin about
// every spawn in a busy zone each frame cost a few milliseconds.
struct SpawnMark {
    const game::Spawn* spawn = nullptr;
    game::MapSpawnMark mark;
    glm::vec3 at{0.0f};
};
std::vector<SpawnMark> spawn_marks(const EqHost& h, const std::function<bool(const glm::vec3&)>& in_view = {}) {
    std::vector<SpawnMark> out;
    if (!h.conn) return out;
    const net::GameConnection& c = *h.conn;
    const auto& w = c.world();
    for (const auto& [id, sp] : w.spawns) {
        if (id == w.player_id) continue;
        const glm::vec3 at = spawn_map(sp);
        if (in_view && !in_view(at)) continue;
        game::MapSpawnMark d;
        game::Con con = game::con_level(sp, static_cast<int>(c.player_status().level), c.pvp_rules());
        if (con != game::Con::None) d.rgb = game::con_rgb(con);
        d.label = game::display_name(sp.name);
        if (auto m = c.server_ext().map_spawn(c, sp, d)) out.push_back({&sp, std::move(*m), at});
    }
    return out;
}

// A mark as the view shows its labels: on a shown layer and, with the height filter, near the player's feet.
bool mark_shown(const game::MapView& v, const SpawnMark& m, std::optional<float> feet) {
    if (!v.layer_shown(m.mark.layer)) return false;
    if (!v.z_filter || !feet) return true;
    return *feet - static_cast<float>(v.min_z) <= m.at.z && m.at.z <= *feet + static_cast<float>(v.max_z);
}

// eq.DrawMap(x0, y0, x1, y1 [, hovered [, scale]])   -- draws the map into that screen rectangle of the current
// window as MapViewMap::Draw draws it; scale: screen pixels per original pixel (the window's), default 1
int DrawMap(lua_State* L) {
    MapState& s = *state_of(L);
    EqHost& h = *host_of(L);
    float x0 = static_cast<float>(luaL_checknumber(L, 1)), y0 = static_cast<float>(luaL_checknumber(L, 2));
    float x1 = static_cast<float>(luaL_checknumber(L, 3)), y1 = static_cast<float>(luaL_checknumber(L, 4));
    bool hovered = lua_toboolean(L, 5) != 0;
    float k = static_cast<float>(luaL_optnumber(L, 6, 1.0));
    if (k <= 0.0f) k = 1.0f;
    s.hover_line = nullptr;
    s.hover_label = nullptr;
    if (!h.in_game || x1 <= x0 || y1 <= y0) return 0;
    auto& v = s.view;
    float cw = (x1 - x0) / k, ch = (y1 - y0) / k; // the client area in original pixels
    s.drawn = true;
    s.ox = x0;
    s.oy = y0;
    s.k = k;
    s.area_w = cw;
    s.area_h = ch;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    auto S = [&](float gx, float gy) { return ImVec2(x0 + gx * k, y0 + gy * k); };
    auto line = [&](float ax, float ay, float bx, float by, ImU32 col) { dl->AddLine(S(ax, ay), S(bx, by), col, std::max(1.0f, k)); };
    auto cross = [&](float gx, float gy, ImU32 col) { // sub_006cb4e0: the X a find ends with
        float cx = std::trunc(gx), cy = std::trunc(gy);
        const float d[6][4] = {{6, -6, -6, 6}, {-6, -6, 6, 6}, {5, -6, -7, 6}, {-7, -6, 5, 6}, {7, -6, -5, 6}, {-5, -6, 7, 6}};
        for (const auto& o : d) line(cx + o[0], cy + o[1], cx + o[2], cy + o[3], col);
    };

    auto me = s.player_map(h);
    auto feet = s.feet_z(h);
    v.precalc(me ? std::optional<glm::vec2>(glm::vec2(*me)) : std::nullopt, s.label_box(h), s.find_box(h));
    auto T = [&](float x, float y) { return v.transform(x, y, cw, ch); };
    bool edit = s.edit_mode();

    // The mouse in the original's pixels; the drag's pan follows it (CMapViewWnd::OnProcessFrame).
    ImVec2 mouse = ImGui::GetMousePos();
    float mx = (mouse.x - x0) / k, my = (mouse.y - y0) / k;
    bool mouse_in = hovered && mx >= 0 && my >= 0 && mx < cw && my < ch;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) s.panning = s.lasso = false;
    if (s.panning) {
        v.pan_x = static_cast<int>(s.anchor_x + mx);
        v.pan_y = static_cast<int>(s.anchor_y + my);
        v.precalc(me ? std::optional<glm::vec2>(glm::vec2(*me)) : std::nullopt, s.label_box(h), s.find_box(h));
    }

    // Labels are placed again when asked (zoom, filters, a load): in the client area at the origin.
    if (!v.labels.empty() && v.relayout) {
        v.place_labels(cw, ch, 0, 0, static_cast<int>(cw), static_cast<int>(ch));
        v.relayout_zoom = v.zoom;
        v.relayout = false;
    }

    // Hover: a group member within 5 pixels first; else the nearest label within 20 and, editing, the nearest line
    // within 3 (the label wins a tie).
    auto members = v.current_zone && v.show_group ? group_spawns(h) : std::vector<const game::Spawn*>{};
    const game::Spawn* hover_member = nullptr;
    auto marks = v.current_zone && me ? spawn_marks(h) : std::vector<SpawnMark>{};
    const SpawnMark* hover_mark = nullptr;
    if (mouse_in) {
        float best = 5.0f;
        for (const auto* sp : members) {
            glm::vec3 m = spawn_map(*sp);
            glm::vec2 p = T(m.x, m.y);
            float d = std::hypot(p.x - mx, p.y - my);
            if (d < best) {
                best = d;
                hover_member = sp;
            }
        }
        // Then a plugin's spawn within 5 pixels.
        for (const auto& mk : marks) {
            if (hover_member) break;
            if (!mark_shown(v, mk, feet)) continue;
            glm::vec2 p = T(mk.at.x, mk.at.y);
            float d = std::hypot(p.x - mx, p.y - my);
            if (d < best) {
                best = d;
                hover_mark = &mk;
            }
        }
    }
    if (mouse_in && !hover_member && !hover_mark) {
        float best_label = 20.0f, best_line = 3.0f;
        for (auto& l : v.labels) {
            if (!v.label_shown(l, feet)) continue;
            glm::vec2 p = T(l.pos.x, l.pos.y);
            float d = std::hypot(mx - p.x, my - p.y);
            if (d < best_label) {
                best_label = d;
                s.hover_label = &l;
            }
        }
        if (edit) {
            for (auto& l : v.lines) {
                if (!v.line_shown(l, feet)) continue;
                glm::vec2 a = T(l.start.x, l.start.y), b = T(l.end.x, l.end.y);
                float d = segment_distance(mx, my, a.x, a.y, b.x, b.y);
                if (d < best_line) {
                    best_line = d;
                    s.hover_line = &l;
                }
            }
            if (s.hover_line && s.hover_label) {
                if (best_label <= best_line) s.hover_line = nullptr;
                else s.hover_label = nullptr;
            }
        }
    }

    // The lasso (Ctrl + drag while editing).
    bool lasso = s.lasso && edit;
    float lx0 = std::min(s.anchor_x, mx), lx1 = std::max(s.anchor_x, mx);
    float ly0 = std::min(s.anchor_y, my), ly1 = std::max(s.anchor_y, my);
    auto in_lasso = [&](glm::vec2 p) {
        float x = std::trunc(p.x), y = std::trunc(p.y);
        return x >= lx0 && x < lx1 && y >= ly0 && y < ly1;
    };

    // The lines.
    for (auto& l : v.lines) {
        if (!v.line_shown(l, feet)) continue;
        glm::vec2 a = T(l.start.x, l.start.y), b = T(l.end.x, l.end.y);
        bool lit = &l == s.hover_line || (edit && v.is_marked(&l)) || (lasso && in_lasso(a) && in_lasso(b));
        line(a.x, a.y, b.x, b.y, lit ? color(kHighlight) : faded(color(l.color), v.z_fade(l.start.z, l.end.z, feet)));
    }
    // The line being walked.
    if (v.line_active && me) {
        glm::vec2 a = T(v.current_point.x, v.current_point.y), b = T(me->x, me->y);
        line(a.x, a.y, b.x, b.y, color(v.current_color));
    }
    // The find path, from the player to its last point and on to the destination, with an X there; else a failed
    // find's X; else the selected label's X. The path and the failed X only on the player's zone.
    const auto* path = s.path(h);
    if ((path || v.failed_find) && !v.current_zone) {
        // another zone's map: neither (nor the label's X)
    } else if (path && me) {
        const auto& pts = path->points();
        glm::vec2 prev = T(me->x, me->y);
        for (std::size_t i = pts.size(); i-- > 0;) {
            glm::vec3 m = game::world_to_map(pts[i]);
            glm::vec2 p = T(m.x, m.y);
            line(prev.x, prev.y, p.x, p.y, color(v.path_color));
            prev = p;
            if (i == 0) cross(p.x, p.y, color(v.path_color));
        }
    } else if (v.failed_find) {
        glm::vec3 m = game::world_to_map(v.failed_point);
        glm::vec2 p = T(m.x, m.y);
        cross(p.x, p.y, color(v.path_color));
    } else if (v.selected_label) {
        glm::vec2 p = T(v.selected_label->pos.x, v.selected_label->pos.y);
        cross(p.x, p.y, color(v.path_color));
    }

    // The labels.
    game::MapLabel* marked = edit ? v.marked_label : nullptr;
    for (auto& l : v.labels) {
        if (!v.label_shown(l, feet)) continue;
        glm::vec2 p = T(l.pos.x, l.pos.y);
        bool lit = &l == s.hover_label || &l == marked;
        ImU32 col = lit ? color(kHighlight) : faded(color(l.color), v.z_fade(l.pos.z, feet));
        line(p.x - 1, p.y, p.x + 2, p.y, col);
        line(p.x, p.y - 1, p.x, p.y + 2, col);
        float lh = static_cast<float>(l.height);
        float offx = static_cast<float>(l.offset_x), offy = static_cast<float>(l.offset_y);
        if (v.labels_active) {
            float ex, ey;
            if (std::abs(l.offset_x) < 5) {
                ex = p.x + 4;
                ey = p.y + offy + static_cast<float>(l.height / 2);
            } else if (l.width < std::abs(l.offset_x)) {
                ex = p.x - 4;
                ey = p.y + offy + static_cast<float>(l.height / 2);
            } else {
                ex = p.x;
                ey = l.offset_y < 1 ? p.y - 5 : p.y + 5;
            }
            line(p.x, p.y, ex, ey, col);
        }
        if (!(v.labels_active || lit) || l.text.empty()) continue;
        float tx = p.x + offx, ty = p.y + offy;
        if (lit && ((l.offset_x == 0 && l.offset_y == 0) || !v.labels_active)) {
            tx = p.x - static_cast<float>(l.width / 2);
            ty = p.y - lh - 2;
        }
        MapFont f = map_font(l.size - 1);
        float fs = f.font ? f.font->FontSize / f.scale * k : ImGui::GetFontSize();
        ImVec2 at = S(tx, ty);
        dl->AddText(f.font, fs, ImVec2(std::trunc(at.x), std::trunc(at.y)), col, l.text.c_str(), nullptr, 300.0f * k);
    }

    // A plugin's spawns: a small plus in the mark's colour, named as the labels are (the Labels button, or hovered).
    for (const auto& mk : marks) {
        if (!mark_shown(v, mk, feet)) continue;
        glm::vec2 p = T(mk.at.x, mk.at.y);
        bool lit = &mk == hover_mark;
        ImU32 col = lit ? color(kHighlight, 0xff) : faded(color(mk.mark.rgb, 0xff), v.z_fade(mk.at.z, feet));
        float gx = std::trunc(p.x), gy = std::trunc(p.y);
        line(gx - 2, gy, gx + 3, gy, col);
        line(gx, gy - 2, gx, gy + 3, col);
        if (!(v.labels_active || lit) || mk.mark.label.empty()) continue;
        MapFont f = map_font(1);
        float fs = f.font ? f.font->FontSize / f.scale * k : ImGui::GetFontSize();
        ImVec2 tp = S(gx + 4, gy - std::trunc(f.height / 2));
        dl->AddText(f.font, fs, ImVec2(std::trunc(tp.x), std::trunc(tp.y)), col, mk.mark.label.c_str(), nullptr,
                    300.0f * k);
    }

    if (v.current_zone && me) {
        // Group members: a thick X in the group colour, opaque and named when hovered.
        for (const auto* sp : members) {
            glm::vec3 m = spawn_map(*sp);
            glm::vec2 p = T(m.x, m.y);
            bool lit = sp == hover_member;
            ImU32 col = color(v.group_color, lit ? 0xff : 0xbf);
            float gx = std::trunc(p.x), gy = std::trunc(p.y);
            const float d[4][4] = {{3, -3, -4, 4}, {-3, -3, 4, 4}, {4, -3, -3, 4}, {-2, -3, 5, 4}};
            for (const auto& o : d) line(gx + o[0], gy + o[1], gx + o[2], gy + o[3], col);
            if (lit) {
                MapFont f = map_font(1);
                std::string name = game::display_name(sp->name);
                float extent = text_width(f, sp->name);
                float fs = f.font ? f.font->FontSize / f.scale * k : ImGui::GetFontSize();
                dl->AddText(f.font, fs, S(gx - std::trunc(extent / 2), gy - f.height - 2), col, name.c_str(), nullptr, 500.0f * k);
            }
        }
        // The compass's points (the expedition switches, the adventure's way in): a green diamond, this client's
        // mark (the original's is not decoded).
        if (h.conn)
            for (const auto& at : h.conn->compass_points()) {
                glm::vec3 m = game::world_to_map(at);
                glm::vec2 p = T(m.x, m.y);
                float cx = std::trunc(p.x), cy = std::trunc(p.y);
                ImU32 dc = IM_COL32(64, 255, 64, 255);
                line(cx - 4, cy, cx, cy - 4, dc);
                line(cx, cy - 4, cx + 4, cy, dc);
                line(cx + 4, cy, cx, cy + 4, dc);
                line(cx, cy + 4, cx - 4, cy, dc);
            }
        // The player: an arrow along the heading (512 a turn, 0 up) with Sense Heading above 190, else a plus.
        glm::vec2 pp = T(me->x, me->y);
        float px = std::trunc(pp.x), py = std::trunc(pp.y);
        ImU32 col = color(v.my_color, 0xff);
        int sense = h.character ? h.character->sheet().skills[kSenseHeading] : 0;
        if (sense < 191) {
            line(px - 2, py, px + 3, py, col);
            line(px, py - 2, px, py + 3, col);
        } else {
            float hd = h.heading;
            auto sn = [](float u) { return game::cmath_sin(u); };
            auto cs = [](float u) { return game::cmath_cos(u); };
            float tx = px - 4 * sn(hd), ty = py - 4 * cs(hd);
            line(px + 6 * sn(hd + 48), py + 6 * cs(hd + 48), tx, ty, col);
            line(px + 6 * sn(hd + 464), py + 6 * cs(hd + 464), tx, ty, col);
            line(px + 8 * sn(hd), py + 8 * cs(hd), tx, ty, col);
        }
    }

    if (lasso) dl->AddRect(S(lx0, ly0), S(lx1, ly1), IM_COL32(255, 255, 255, 255), 0.0f, 0, std::max(1.0f, k));

    // "Range = n" in black at the bottom right.
    std::string range = s.str(h, kEqStrRange, {std::to_string(v.range)});
    if (!range.empty()) {
        MapFont f = map_font(1);
        float extent = std::trunc(text_width(f, range));
        float fs = f.font ? f.font->FontSize / f.scale * k : ImGui::GetFontSize();
        dl->AddText(f.font, fs, S(std::trunc(cw) - extent, std::trunc(ch) - std::round(f.height)), IM_COL32(0, 0, 0, 255),
                    range.c_str());
    }
    dl->PopClipRect();
    return 0;
}

// ---------------------------------------------------------------- the minimap and the zone map (client extensions)
// The original has neither: its only map is MapViewWnd above. These draw the same map (its lines, layers and height
// filter, the group, the /find path, the compass points and the player) in the modern UIs' own views: a circle
// around the player, and a large view fitted to the zone.

// Where and how one of them draws: map coordinates to the screen (P, relative to the centre), and the clip (a
// circle of radius `circle` around the centre, else the rectangle pushed on the draw list).
struct ZoneView {
    ImVec2 c;            // the screen centre
    float circle = 0.0f; // > 0: clip to this circle
    float width = 1.0f;  // line width
    float turn = 0.0f;   // the view's rotation, in heading units (the heading up)
    bool labels = false;
    std::function<glm::vec2(float x, float y)> P;
    // > 0: the map points that can show lie within this many map units of `reach_from` (the minimap's circle), so a
    // line wholly outside the square round it is passed over without projecting it.
    float reach = 0.0f;
    glm::vec2 reach_from{0.0f};
};

// The map files' colours are chosen for the map window's light background; on a dark view the dark ones are
// lightened toward white, keeping their hue.
ImU32 ink(std::uint32_t argb, int alpha = 230) {
    float rr = static_cast<float>((argb >> 16) & 0xff), gg = static_cast<float>((argb >> 8) & 0xff),
          bb = static_cast<float>(argb & 0xff);
    float lum = (0.299f * rr + 0.587f * gg + 0.114f * bb) / 255.0f;
    if (lum >= 0.45f) return color(argb, alpha);
    float t = 0.75f - lum; // the darker, the more
    auto mix = [&](float c) { return static_cast<int>(c + (225.0f - c) * t); };
    return IM_COL32(mix(rr), mix(gg), mix(bb), alpha);
}

void draw_zone_view(MapState& s, EqHost& h, const ZoneView& zv) {
    auto me = s.player_map(h);
    if (!me) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = zv.circle, w = zv.width;
    auto at = [&](glm::vec2 p) { return ImVec2(zv.c.x + p.x, zv.c.y + p.y); };
    auto inside = [&](glm::vec2 p) { return r <= 0.0f || glm::dot(p, p) <= r * r; };
    // A segment's part inside the circle (|a + t (b - a)| <= r, t in 0..1); the rectangle clips by itself.
    auto seg = [&](glm::vec2 a, glm::vec2 b, ImU32 col, float width) {
        glm::vec2 d = b - a;
        float t0 = 0.0f, t1 = 1.0f;
        if (r > 0.0f) {
            float qa = glm::dot(d, d), qb = 2.0f * glm::dot(a, d), qc = glm::dot(a, a) - r * r;
            if (qa > 0.0f) {
                float disc = qb * qb - 4.0f * qa * qc;
                if (disc <= 0.0f) return;
                float sq = std::sqrt(disc);
                t0 = std::max(0.0f, (-qb - sq) / (2.0f * qa));
                t1 = std::min(1.0f, (-qb + sq) / (2.0f * qa));
                if (t0 >= t1) return;
            } else if (qc > 0.0f) {
                return;
            }
        }
        dl->AddLine(at(a + d * t0), at(a + d * t1), col, width);
    };
    // Its parts' time in the frame's sections (the hitch log), inside the window's own.
    core::frame_sections::Lap lap;
    const auto& v = s.view;
    // Another zone's map (an atlas zone link, eq.ShowZoneMap) is drawn without the player, the find path and the
    // height fade, which belong to the player's zone.
    auto feet = v.current_zone ? s.feet_z(h) : std::nullopt;
    const glm::vec2 here = zv.P(me->x, me->y);
    // The minimap's square: in a busy zone's thousands of lines, most are outside it.
    const float reach = zv.reach > 0.0f ? zv.reach + 1.0f : 0.0f;
    auto out_of_reach = [&](const glm::vec3& a, const glm::vec3& b) {
        return reach > 0.0f && (std::max(a.x, b.x) < zv.reach_from.x - reach || std::min(a.x, b.x) > zv.reach_from.x + reach ||
                                std::max(a.y, b.y) < zv.reach_from.y - reach || std::min(a.y, b.y) > zv.reach_from.y + reach);
    };
    for (const auto& l : v.lines)
        if (!out_of_reach(l.start, l.end) && v.line_shown(l, feet))
            seg(zv.P(l.start.x, l.start.y), zv.P(l.end.x, l.end.y), faded(ink(l.color), v.z_fade(l.start.z, l.end.z, feet)), w);
    if (v.line_active) seg(zv.P(v.current_point.x, v.current_point.y), here, ink(v.current_color), w);
    lap.mark("map view lines");
    if (zv.labels) {
        ImFont* f = ImGui::GetFont();
        float fs = ImGui::GetFontSize();
        for (const auto& l : v.labels) {
            if (!v.label_shown(l, feet) || l.text.empty()) continue;
            glm::vec2 p = zv.P(l.pos.x, l.pos.y);
            ImVec2 q = at(p);
            std::string text = l.text;
            for (char& ch : text)
                if (ch == '_') ch = ' '; // the map files' spaces
            const float fade = v.z_fade(l.pos.z, feet);
            dl->AddCircleFilled(q, 2.0f * w, faded(ink(l.color), fade));
            ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, text.c_str());
            ImVec2 tp(q.x - ts.x / 2, q.y - ts.y - 2.0f * w);
            dl->AddText(f, fs, ImVec2(tp.x + 1, tp.y + 1), faded(IM_COL32(0, 0, 0, 200), fade), text.c_str());
            dl->AddText(f, fs, tp, faded(ink(l.color, 255), fade), text.c_str());
        }
    }
    if (v.current_zone && h.conn && !h.conn->find_path().empty()) {
        // The find path from the player to the destination (its points run the other way).
        const auto& pts = h.conn->find_path();
        glm::vec2 prev = here;
        for (std::size_t i = pts.size(); i-- > 0;) {
            glm::vec3 m = game::world_to_map(pts[i]);
            glm::vec2 p = zv.P(m.x, m.y);
            seg(prev, p, IM_COL32(255, 255, 64, 255), w * 2.0f);
            prev = p;
        }
    }
    if (v.current_zone && h.guide_line && h.guide_line->size() >= 2) {
        // The Find window's guide path (app/guide_path.h), the player first, gold.
        const auto& pts = *h.guide_line;
        glm::vec2 prev = here;
        for (std::size_t i = 1; i < pts.size(); ++i) {
            glm::vec3 m = game::world_to_map(pts[i]);
            glm::vec2 p = zv.P(m.x, m.y);
            seg(prev, p, IM_COL32(255, 204, 90, 255), w * 2.0f);
            prev = p;
        }
    }
    if (v.current_zone && h.conn)
        for (const auto& pt : h.conn->compass_points()) {
            glm::vec3 m = game::world_to_map(pt);
            glm::vec2 p = zv.P(m.x, m.y);
            if (!inside(p)) continue;
            float d = 5.0f * w;
            ImVec2 c = at(p);
            dl->AddQuadFilled(ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y),
                              IM_COL32(64, 255, 64, 255));
        }
    lap.mark("map view labels, path, compass");
    // A plugin's spawns: a dot in the mark's colour, named with the labels.
    // In a circle a mark shows only inside it, so the others are not asked about; a rectangle clips by itself (a
    // label may reach in from a mark outside), so all are.
    auto in_circle = [&](const glm::vec3& m) { return inside(zv.P(m.x, m.y)); };
    if (v.current_zone)
        for (const auto& mk : spawn_marks(h, r > 0.0f ? std::function<bool(const glm::vec3&)>(in_circle) : nullptr)) {
            if (!mark_shown(v, mk, feet)) continue;
            glm::vec2 p = zv.P(mk.at.x, mk.at.y);
            if (!inside(p)) continue;
            ImVec2 q = at(p);
            const float fade = v.z_fade(mk.at.z, feet);
            dl->AddCircleFilled(q, 2.5f * w, faded(color(mk.mark.rgb, 0xff), fade));
            dl->AddCircle(q, 2.5f * w, faded(IM_COL32(0, 0, 0, 160), fade), 0, w * 0.5f);
            if (!zv.labels || mk.mark.label.empty()) continue;
            ImFont* f = ImGui::GetFont();
            float fs = ImGui::GetFontSize();
            ImVec2 tp(q.x + 4.0f * w, q.y - fs / 2);
            dl->AddText(f, fs, ImVec2(tp.x + 1, tp.y + 1), faded(IM_COL32(0, 0, 0, 200), fade), mk.mark.label.c_str());
            dl->AddText(f, fs, tp, faded(color(mk.mark.rgb, 255), fade), mk.mark.label.c_str());
        }
    lap.mark("map view spawn marks");
    if (v.current_zone && v.show_group)
        for (const auto* sp : group_spawns(h)) {
            glm::vec3 m = spawn_map(*sp);
            glm::vec2 p = zv.P(m.x, m.y);
            if (!inside(p)) continue;
            dl->AddCircleFilled(at(p), 4.0f * w, color(v.group_color, 0xff));
            dl->AddCircle(at(p), 4.0f * w, IM_COL32(0, 0, 0, 200), 0, w);
        }
    // The player: an arrow along the heading, as the map window's with Sense Heading above 190, else a dot.
    if (!v.current_zone) return;
    const ImVec2 pc = at(here);
    const float ph = h.heading - zv.turn;
    auto dir = [&](float u, float len) {
        return ImVec2(pc.x - len * game::cmath_sin(u), pc.y - len * game::cmath_cos(u));
    };
    int sense = h.character ? h.character->sheet().skills[kSenseHeading] : 0;
    if (sense < 191) {
        dl->AddCircleFilled(pc, 3.5f * w, IM_COL32(255, 255, 255, 255));
        dl->AddCircle(pc, 3.5f * w, IM_COL32(0, 0, 0, 220), 0, w);
    } else {
        ImVec2 tip = dir(ph, 9.0f * w), left = dir(ph + 208, 7.0f * w), right = dir(ph + 304, 7.0f * w);
        ImVec2 back = dir(ph + 256, 3.0f * w);
        dl->AddTriangleFilled(tip, left, back, IM_COL32(255, 255, 255, 255));
        dl->AddTriangleFilled(tip, back, right, IM_COL32(255, 255, 255, 255));
        dl->AddPolyline(std::array<ImVec2, 4>{tip, left, back, right}.data(), 4, IM_COL32(0, 0, 0, 220),
                        ImDrawFlags_Closed, w);
    }
}

// eq.DrawMinimap(cx, cy, radius, range [, rotate])   -- the map in a circle centred on the player, `range` world
// units from the centre to the edge, north up or, with rotate, the player's heading up; without the labels.
// Returns the screen direction of north (dx, dy) for the ring's N and, in the world, the player's map position and the
// screen pixels per map unit (for a pack's own marks: a point's offset from the player times that, turned so north is
// (dx, dy)).
int DrawMinimap(lua_State* L) {
    MapState& s = *state_of(L);
    EqHost& h = *host_of(L);
    const float cx = static_cast<float>(luaL_checknumber(L, 1)), cy = static_cast<float>(luaL_checknumber(L, 2));
    const float r = static_cast<float>(luaL_checknumber(L, 3));
    const float range = std::max(1.0f, static_cast<float>(luaL_checknumber(L, 4)));
    const bool rotate = lua_toboolean(L, 5) != 0;
    // Map coordinates are screen-aligned (x right, y down, world_to_map); the heading turns counter-clockwise from
    // north, so turning the map by it puts the facing up.
    const float hd = rotate ? h.heading : 0.0f;
    const float sn = game::cmath_sin(hd), cs = game::cmath_cos(hd);
    lua_pushnumber(L, sn);
    lua_pushnumber(L, -cs);
    auto me = s.player_map(h);
    if (!h.in_game || !me || r <= 0.0f) return 2;
    const float k = r / range;
    ZoneView zv;
    zv.c = ImVec2(cx, cy);
    zv.circle = r;
    zv.width = std::max(1.5f, r / 90.0f);
    zv.turn = hd;
    zv.P = [&](float x, float y) {
        float dx = (x - me->x) * k, dy = (y - me->y) * k;
        return glm::vec2(dx * cs - dy * sn, dx * sn + dy * cs);
    };
    zv.reach = range; // the circle's radius r is `range` map units, however the map turns
    zv.reach_from = glm::vec2(me->x, me->y);
    draw_zone_view(s, h, zv);
    lua_pushnumber(L, me->x);
    lua_pushnumber(L, me->y);
    lua_pushnumber(L, k);
    return 5;
}

// scale, cx, cy = eq.DrawZoneMap(x0, y0, x1, y1, zoom [, cx, cy [, labels]])   -- the map in that rectangle, north
// up: zoom 1 fits the whole zone (its lines' extent), more zooms in around (cx, cy), a point in map units (default:
// the zone's middle). Returns the screen pixels per map unit and the zone's middle (for panning and centring), and
// with the player: its map position.
int DrawZoneMap(lua_State* L) {
    MapState& s = *state_of(L);
    EqHost& h = *host_of(L);
    const float x0 = static_cast<float>(luaL_checknumber(L, 1)), y0 = static_cast<float>(luaL_checknumber(L, 2));
    const float x1 = static_cast<float>(luaL_checknumber(L, 3)), y1 = static_cast<float>(luaL_checknumber(L, 4));
    const float zoom = std::max(0.1f, static_cast<float>(luaL_checknumber(L, 5)));
    const auto& v = s.view;
    if (!h.in_game || x1 <= x0 || y1 <= y0) return 0;
    auto feet = s.feet_z(h);
    glm::vec2 lo(FLT_MAX), hi(-FLT_MAX);
    for (const auto& l : v.lines) {
        if (!v.line_shown(l, feet)) continue;
        lo = glm::min(lo, glm::min(glm::vec2(l.start), glm::vec2(l.end)));
        hi = glm::max(hi, glm::max(glm::vec2(l.start), glm::vec2(l.end)));
    }
    auto me = s.player_map(h);
    if (lo.x > hi.x) { // no lines: a 1000-unit square around the player
        glm::vec2 m = me ? glm::vec2(*me) : glm::vec2(0.0f);
        lo = m - 500.0f;
        hi = m + 500.0f;
    }
    const glm::vec2 mid = (lo + hi) * 0.5f, size = glm::max(hi - lo, glm::vec2(1.0f));
    const float k = std::min((x1 - x0) / size.x, (y1 - y0) / size.y) * 0.94f * zoom;
    const float cx = static_cast<float>(luaL_optnumber(L, 6, mid.x)), cy = static_cast<float>(luaL_optnumber(L, 7, mid.y));
    lua_pushnumber(L, k);
    lua_pushnumber(L, mid.x);
    lua_pushnumber(L, mid.y);
    int out = 3;
    if (me && s.view.current_zone) { // (another zone's map: no player)
        lua_pushnumber(L, me->x);
        lua_pushnumber(L, me->y);
        out = 5;
    }
    if (!me) return out;
    ZoneView zv;
    zv.c = ImVec2((x0 + x1) / 2, (y0 + y1) / 2);
    zv.width = std::max(1.5f, (y1 - y0) / 400.0f);
    zv.labels = lua_isnoneornil(L, 8) || lua_toboolean(L, 8);
    zv.P = [&](float x, float y) { return glm::vec2((x - cx) * k, (y - cy) * k); };
    ImGui::GetWindowDrawList()->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    draw_zone_view(s, h, zv);
    ImGui::GetWindowDrawList()->PopClipRect();
    return out;
}

// eq.DrawAtlas(x0, y0, x1, y1, scale)   -- the atlas page (AtlasWnd) in that rectangle of the map window, scrolled
// when it is larger: the background tiled, the pictures, the labels on their dark boxes, the links' tooltips. A page
// link opens its page (AtlasWnd's WndNotification: +0x220, read on the next frame); a zone link shows that zone's
// map on the map page (CMapViewWnd::WndNotification: the player's zone loads again, another zone is loaded).
int DrawAtlas(lua_State* L) {
    MapState& s = *state_of(L);
    EqHost& h = *host_of(L);
    const float x0 = static_cast<float>(luaL_checknumber(L, 1)), y0 = static_cast<float>(luaL_checknumber(L, 2));
    const float x1 = static_cast<float>(luaL_checknumber(L, 3)), y1 = static_cast<float>(luaL_checknumber(L, 4));
    float k = static_cast<float>(luaL_optnumber(L, 5, 1.0));
    if (k <= 0.0f) k = 1.0f;
    if (x1 <= x0 || y1 <= y0 || !h.textures) return 0;
    if (!s.atlas_loaded) s.load_atlas(h, "Default.xml");
    const game::AtlasPage& page = s.atlas;
    const float pw = static_cast<float>(page.width()) * k, ph = static_cast<float>(page.height()) * k;
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    if (!ImGui::BeginChild("##atlaspage", ImVec2(x1 - x0, y1 - y0), ImGuiChildFlags_None,
                           ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBackground)) {
        ImGui::EndChild();
        return 0;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 view = ImGui::GetContentRegionAvail();
    auto tex = [&](const std::string& file) { return h.textures->game_file("Atlas/" + s.atlas_skin + "/" + file); };
    auto P = [&](float x, float y) { return ImVec2(o.x + x * k, o.y + y * k); };
    // The background, tiled over the page and the view.
    if (page.background && page.background_cx > 0 && page.background_cy > 0)
        if (const auto* t = tex(*page.background)) {
            float w = std::max(pw, view.x), hh = std::max(ph, view.y);
            float tw = static_cast<float>(page.background_cx) * k, th = static_cast<float>(page.background_cy) * k;
            for (float y = 0; y < hh; y += th)
                for (float x = 0; x < w; x += tw) dl->AddImage(reinterpret_cast<ImTextureID>(t->id), ImVec2(o.x + x, o.y + y),
                                                              ImVec2(o.x + x + tw, o.y + y + th));
        }
    // A picture: the file's top left (CX x CY of it) in its rectangle.
    auto picture = [&](const game::AtlasImage& im) {
        const auto* t = tex(im.file);
        if (!t || t->width <= 0 || t->height <= 0) return;
        float cw = static_cast<float>(im.rect.right - im.rect.left), chh = static_cast<float>(im.rect.bottom - im.rect.top);
        dl->AddImage(reinterpret_cast<ImTextureID>(t->id), P(static_cast<float>(im.rect.left), static_cast<float>(im.rect.top)),
                     P(static_cast<float>(im.rect.right), static_cast<float>(im.rect.bottom)), ImVec2(0, 0),
                     ImVec2(std::min(1.0f, cw / static_cast<float>(t->width)), std::min(1.0f, chh / static_cast<float>(t->height))));
    };
    for (const auto& im : page.images) picture(im);
    for (const auto& l : page.links)
        if (l.image) picture(*l.image);
    // The labels: the text's extent in its font on a 0x64000000 box, white.
    for (const auto& l : page.labels) {
        MapFont f = map_font(l.font);
        float tw = text_width(f, l.text), th = f.height;
        ImVec2 a = P(static_cast<float>(l.x), static_cast<float>(l.y));
        dl->AddRectFilled(a, ImVec2(a.x + tw * k, a.y + th * k), IM_COL32(0, 0, 0, 0x64));
        float fs = f.font ? f.font->FontSize / f.scale * k : ImGui::GetFontSize();
        dl->AddText(f.font, fs, a, IM_COL32(255, 255, 255, 255), l.text.c_str());
    }
    ImGui::Dummy(ImVec2(std::max(pw, 1.0f), std::max(ph, 1.0f)));
    // The links: the last one under the mouse takes it.
    if (ImGui::IsWindowHovered()) {
        ImVec2 m = ImGui::GetMousePos();
        for (auto it = page.links.rbegin(); it != page.links.rend(); ++it) {
            ImVec2 a = P(static_cast<float>(it->rect.left), static_cast<float>(it->rect.top));
            ImVec2 b = P(static_cast<float>(it->rect.right), static_cast<float>(it->rect.bottom));
            if (m.x < a.x || m.y < a.y || m.x >= b.x || m.y >= b.y) continue;
            if (!it->tooltip.empty()) ImGui::SetTooltip("%s", it->tooltip.c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (it->action == 1) {
                    s.atlas_pending = it->target;
                } else if (it->action == 2) {
                    std::uint32_t id = game::zone_id_by_short_name(it->target);
                    if (id != 0) {
                        if (lower(it->target) == lower(h.zone_short_name)) s.zone.clear(); // CDisplay_LoadWorldExtents(1)
                        else s.load_other_zone(h, game::zone_short_name(id));
                        s.tab = 0; // SetPage(pageMap)
                    }
                }
            }
            break;
        }
    }
    ImGui::EndChild();
    return 0;
}

// eq.FindMapLocation(x, y)   -- the find path to a point of the zone's map (map units), as the stock map's right
// click (PlayerPath_FindMapLocation, at the player's height); for the modern and gamepad maps.
int FindMapLocation(lua_State* L) {
    EqHost& h = *host_of(L);
    const float x = static_cast<float>(luaL_checknumber(L, 1)), y = static_cast<float>(luaL_checknumber(L, 2));
    if (!h.in_game || !h.conn || !h.feet_valid) return 0;
    auto me = h.conn->world().player();
    float z = me ? me->position.z : h.feet[2];
    glm::vec3 w = game::map_to_world(glm::vec3(x, y, z));
    h.conn->find_map_location(w);
    return 0;
}

// active = eq.FindPathShown()   -- a find path is shown;  eq.EndFind()   -- it ends (PlayerPath_Stop)
// (and the Find window's guide path, app/guide_path.h, likewise)
int FindPathShown(lua_State* L) {
    EqHost& h = *host_of(L);
    lua_pushboolean(L, (h.conn && h.conn->player_path().active()) || h.guide_line);
    return 1;
}
int EndFind(lua_State* L) {
    EqHost& h = *host_of(L);
    if (h.conn && h.conn->player_path().active()) h.conn->stop_find_path();
    if (h.guide_line && h.end_guide) h.end_guide();
    return 0;
}

// name, short_name = eq.ZoneName()   -- the zone the player is in ("" outside the world)
int ZoneName(lua_State* L) {
    EqHost& h = *host_of(L);
    lua_pushstring(L, h.in_game ? h.zone_long_name.c_str() : "");
    lua_pushstring(L, h.in_game ? h.zone_short_name.c_str() : "");
    return 2;
}

// hour, minute = eq.GameTime()   -- the game clock, 0..23 and 0..59 (EQ hour h is h - 1 o'clock); nil before the
// server sent the time
int GameTime(lua_State* L) {
    EqHost& h = *host_of(L);
    if (h.game_hour < 1.0f) return 0;
    float t = h.game_hour - 1.0f;
    int hour = static_cast<int>(t), minute = static_cast<int>((t - static_cast<float>(hour)) * 60.0f);
    lua_pushinteger(L, hour % 24);
    lua_pushinteger(L, std::min(minute, 59));
    return 2;
}

// on, above, below = eq.MapAutoZ([on [, above [, below]]])   -- Auto Z (MapView::auto_z, not the original's): with
// arguments sets it and stores the map's settings; `above` and `below` are how far above and below the feet things
// stay whole (world units, 1..1000), and `above` alone sets both. Returns the setting either way.
int MapAutoZ(lua_State* L) {
    MapState& s = *state_of(L);
    if (!lua_isnoneornil(L, 1)) {
        auto range = [L](int i) {
            return static_cast<int>(std::clamp<lua_Integer>(luaL_checkinteger(L, i), 1, game::MapView::kAutoZMaxRange));
        };
        s.view.auto_z = lua_toboolean(L, 1) != 0;
        if (!lua_isnoneornil(L, 2)) {
            s.view.auto_z_above = range(2);
            s.view.auto_z_below = lua_isnoneornil(L, 3) ? s.view.auto_z_above : range(3);
        }
        s.store_ini();
    }
    lua_pushboolean(L, s.view.auto_z);
    lua_pushinteger(L, s.view.auto_z_above);
    lua_pushinteger(L, s.view.auto_z_below);
    return 3;
}

bool drawn_now(const MapState& s) { return s.drawn; }

// eq.MapZoom(steps)   -- the mouse wheel over the map (HandleWheelMove: x1.5 a step up, x2/3 down, not below 1)
int MapZoom(lua_State* L) {
    MapState& s = *state_of(L);
    EqHost& h = *host_of(L);
    double steps = luaL_checknumber(L, 1);
    if (steps == 0 || !h.in_game) return 0;
    if (s.find_locked(h)) {
        s.say(h, s.str(h, kEqStrManualZoom));
        return 0;
    }
    s.view.wheel(steps > 0 ? 1 : -1);
    return 0;
}

// eq.MapMouseDown()   -- the left button went down on the map (MapViewMap::HandleLButtonDown @0x6c9430): editing, a
// hovered label becomes the marked one and a hovered line is marked (a click on nothing clears the marks);
// otherwise dragging pans. Ctrl while editing starts a lasso.
int MapMouseDown(lua_State* L) {
    MapState& s = *state_of(L);
    if (!drawn_now(s) || s.find_locked(*host_of(L))) return 0;
    ImVec2 mouse = ImGui::GetMousePos();
    float mx = (mouse.x - s.ox) / s.k, my = (mouse.y - s.oy) / s.k;
    bool edit = s.edit_mode();
    if (!ImGui::GetIO().KeyCtrl) {
        if (edit) {
            if (s.hover_label) {
                s.view.marked_label = s.hover_label;
                return 0;
            }
            s.view.marked_label = nullptr;
            if (s.hover_line) {
                s.view.mark_line(s.hover_line);
                return 0;
            }
            s.view.mark_line(nullptr);
        }
        s.anchor_x = static_cast<float>(s.view.pan_x) - mx;
        s.anchor_y = static_cast<float>(s.view.pan_y) - my;
        s.panning = true;
    } else if (!s.lasso && edit) {
        s.anchor_x = mx;
        s.anchor_y = my;
        s.lasso = true;
    }
    return 0;
}

// eq.MapMouseUp()   -- the left button came up: a pan ends; a lasso marks the shown lines inside it
int MapMouseUp(lua_State* L) {
    MapState& s = *state_of(L);
    EqHost& h = *host_of(L);
    if (s.panning) {
        s.panning = false;
        return 0;
    }
    // HandleLButtonUpAfterHeld: a click while the find holds the map says so.
    if (drawn_now(s) && s.find_locked(h)) s.say(h, s.str(h, kEqStrManualZoom));
    if (!s.lasso) return 0;
    s.lasso = false;
    if (!drawn_now(s) || !s.edit_mode()) return 0;
    ImVec2 mouse = ImGui::GetMousePos();
    float mx = (mouse.x - s.ox) / s.k, my = (mouse.y - s.oy) / s.k;
    float lx0 = std::min(s.anchor_x, mx), lx1 = std::max(s.anchor_x, mx);
    float ly0 = std::min(s.anchor_y, my), ly1 = std::max(s.anchor_y, my);
    auto in = [&](float x, float y) {
        glm::vec2 p = s.view.transform(x, y, s.area_w, s.area_h);
        float ix = std::trunc(p.x), iy = std::trunc(p.y);
        return ix >= lx0 && ix < lx1 && iy >= ly0 && iy < ly1;
    };
    auto feet = s.feet_z(h);
    for (auto& l : s.view.lines)
        if (s.view.line_shown(l, feet) && in(l.start.x, l.start.y) && in(l.end.x, l.end.y)) s.view.mark_line(&l);
    return 0;
}

// eq.MapPanReset()   -- back to the map's centre
int MapPanReset(lua_State* L) {
    MapState& s = *state_of(L);
    s.view.pan_x = s.view.pan_y = 0;
    return 0;
}

// eq.MapRightClick(x, y)   -- the map's right click (MapViewMap::HandleRButtonDown @0x6c9680): on the player's zone,
// the point under the mouse (GetWorldCoordinates, at the player's height) is found (PlayerPath_FindMapLocation).
int MapRightClick(lua_State* L) {
    MapState& s = *state_of(L);
    EqHost& h = *host_of(L);
    const float x = static_cast<float>(luaL_checknumber(L, 1)), y = static_cast<float>(luaL_checknumber(L, 2));
    if (!drawn_now(s) || !h.conn || !h.feet_valid || !s.view.current_zone) return 0;
    if (s.area_w < 1.0f || s.area_h < 1.0f) return 0;
    // The point in the original's pixels, the integer mouse as the original's.
    float px = std::trunc((x - s.ox) / s.k), py = std::trunc((y - s.oy) / s.k);
    glm::vec2 yx = s.view.world_coordinates(px, py, s.area_w, s.area_h); // the original's (Y, X) = world (x, y)
    auto me = h.conn->world().player();
    float z = me ? me->position.z : h.feet[2];
    h.conn->find_map_location(glm::vec3(yx.x, yx.y, z));
    return 0;
}

} // namespace

std::vector<std::uint32_t> map_command(StockWindow& w, EqHost& h, std::string_view args) {
    // Cmd_map @0x4e5960: with the map window open. "color r g b" (15057, its length; each clamped to 0..255) sets
    // the current colour (0x6c8b50: 15058, or 15059 with a value missing); "addpoint" / "startline" / "endline"
    // (15043..15045, whole words) add a point, start a line or end it at the player (0x6cab70 / 0x6c7f90 /
    // 0x6caa70: 15050 / 15048 / 15049); "automap" (15053) ends or starts the automap (15052 / 15051); anything
    // else 15046 and 15047. The player is where the toolbar's buttons put points (the feet, map coordinates).
    auto* map = dynamic_cast<MapViewWindow*>(&w);
    if (!map || !map->shown(h) || !h.conn) return {};
    MapState& s = map->state();
    const auto& strings = h.conn->strings();
    auto str = [&](std::uint32_t id) { return strings.format(id, {}); };
    auto same = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    };
    std::string color = str(15057);
    auto me = s.player_map(h);
    if (args.size() >= color.size() && same(args.substr(0, color.size()), color)) {
        std::string words[4];
        std::string_view rest = args;
        for (auto& word : words) word = game::next_cmd_arg(rest);
        if (words[1].empty() || words[2].empty() || words[3].empty()) return {15059};
        auto clamp = [](const std::string& t) { return static_cast<std::uint32_t>(std::clamp(std::atoi(t.c_str()), 0, 255)); };
        s.view.current_color = 0xff000000u | clamp(words[1]) << 16 | clamp(words[2]) << 8 | clamp(words[3]);
        return {15058};
    }
    if (same(args, str(15043))) {
        if (me) s.view.add_point(*me);
        return {15050};
    }
    if (same(args, str(15044))) {
        if (me) s.view.start_line(*me);
        return {15048};
    }
    if (same(args, str(15045))) {
        if (me) s.view.end_line(*me);
        return {15049};
    }
    if (same(args, str(15053))) {
        if (s.automap) {
            s.stop_automap(h);
            return {15052};
        }
        s.start_automap(h);
        return {15051};
    }
    return {15046, 15047};
}

void add_map_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    auto s = std::make_shared<MapState>();
    out.push_back(std::make_unique<MapViewWindow>(s));
    out.push_back(std::make_unique<MapToolbarWindow>(s));
    out.push_back(std::make_unique<EditLabelWindow>(s));
}

// ---------------------------------------------------------------- the modern map's atlas and zone finder
// Hailstone's, not the original's (docs/navigation.md "Zone atlas"): the atlas pages as data for the pack to draw,
// the zone links (game/zone_routes.h) and routes over them, and the current zone's exits.

void set_field(lua_State* L, const char* key, lua_Number v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, key);
}
void set_field(lua_State* L, const char* key, int v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
}
void set_field(lua_State* L, const char* key, std::uint32_t v) {
    lua_pushinteger(L, static_cast<lua_Integer>(v));
    lua_setfield(L, -2, key);
}
void set_field(lua_State* L, const char* key, std::string_view v) {
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, key);
}
void set_rect(lua_State* L, const game::AtlasRect& r) {
    set_field(L, "x0", r.left);
    set_field(L, "y0", r.top);
    set_field(L, "x1", r.right);
    set_field(L, "y1", r.bottom);
}
// A zone's name for the finder: the zone table's long name, else the short name.
std::string zone_title(std::uint32_t id) {
    std::string n = game::zone_long_name(id);
    return n.empty() ? game::zone_short_name(id) : n;
}
void push_zone(lua_State* L, std::uint32_t id) {
    lua_createtable(L, 0, 4);
    set_field(L, "id", id);
    set_field(L, "zone", game::zone_short_name(id));
    set_field(L, "name", zone_title(id));
    set_field(L, "expansion", game::zone_expansion(id));
}

// index = eq.AtlasIndex()   -- the atlas (Atlas\<skin>\, read once): index.pages = {{file, title, parent}, ...} in
// the order found from Default.xml (parent "" for it), index.zones = {{zone, name, page, x0, y0, x1, y1, id,
// expansion, allowed}, ...}: every zone link, page by page (a zone may be on several pages; id 0 for a zone the zone
// table does not know); allowed: the server has it (MapState::zone_allowed; true for an unknown zone). Call it again
// when eq.ZoneLinks() changes.
int AtlasIndex(lua_State* L) {
    MapState& s = *state_of(L);
    EqHost& h = *host_of(L);
    const game::AtlasIndex& index = s.index_atlas(h);
    lua_createtable(L, 0, 2);
    lua_createtable(L, static_cast<int>(index.pages.size()), 0);
    for (std::size_t i = 0; i < index.pages.size(); ++i) {
        const auto& p = index.pages[i];
        lua_createtable(L, 0, 3);
        set_field(L, "file", p.file);
        set_field(L, "title", p.title);
        set_field(L, "parent", p.parent);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "pages");
    lua_createtable(L, static_cast<int>(index.zones.size()), 0);
    for (std::size_t i = 0; i < index.zones.size(); ++i) {
        const auto& z = index.zones[i];
        const std::uint32_t id = game::zone_id_by_short_name(z.zone);
        lua_createtable(L, 0, 9);
        set_field(L, "zone", id ? game::zone_short_name(id) : z.zone);
        set_field(L, "name", z.name);
        set_field(L, "page", z.page);
        set_rect(L, z.rect);
        set_field(L, "id", id);
        set_field(L, "expansion", game::zone_expansion(id));
        lua_pushboolean(L, id == 0 || s.zone_allowed(h, id));
        lua_setfield(L, -2, "allowed");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "zones");
    return 1;
}

// page = eq.AtlasPageData(file)   -- an atlas page (game::parse_atlas_page, read once): {width, height, background,
// bgw, bgh, images = {{file, x0, y0, x1, y1}, ...}, labels = {{text, x, y}, ...}, links = {{action, target,
// tip, x0, y0, x1, y1}, ...}, errors}; a link's own picture is among the images. nil when it cannot be read.
int AtlasPageData(lua_State* L) {
    MapState& s = *state_of(L);
    EqHost& h = *host_of(L);
    const std::string file = luaL_checkstring(L, 1);
    auto it = s.atlas_pages.find(lower(file));
    if (it == s.atlas_pages.end()) {
        auto text = s.read_atlas(h, file);
        if (!text) {
            lua_pushnil(L);
            return 1;
        }
        it = s.atlas_pages.emplace(lower(file), game::parse_atlas_page(*text)).first;
    }
    const game::AtlasPage& page = it->second;
    lua_createtable(L, 0, 9);
    set_field(L, "width", page.width());
    set_field(L, "height", page.height());
    if (page.background) {
        set_field(L, "background", *page.background);
        set_field(L, "bgw", page.background_cx);
        set_field(L, "bgh", page.background_cy);
    }
    lua_createtable(L, static_cast<int>(page.images.size()), 0);
    int n = 0;
    auto image = [&](const game::AtlasImage& im) {
        lua_createtable(L, 0, 5);
        set_field(L, "file", im.file);
        set_rect(L, im.rect);
        lua_rawseti(L, -2, ++n);
    };
    for (const auto& im : page.images) image(im);
    for (const auto& l : page.links)
        if (l.image) image(*l.image);
    lua_setfield(L, -2, "images");
    lua_createtable(L, static_cast<int>(page.labels.size()), 0);
    for (std::size_t i = 0; i < page.labels.size(); ++i) {
        lua_createtable(L, 0, 3);
        set_field(L, "text", page.labels[i].text);
        set_field(L, "x", page.labels[i].x);
        set_field(L, "y", page.labels[i].y);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "labels");
    lua_createtable(L, static_cast<int>(page.links.size()), 0);
    for (std::size_t i = 0; i < page.links.size(); ++i) {
        const auto& l = page.links[i];
        lua_createtable(L, 0, 7);
        set_field(L, "action", l.action);
        const std::uint32_t id = l.action == 2 ? game::zone_id_by_short_name(l.target) : 0;
        set_field(L, "target", id ? game::zone_short_name(id) : l.target);
        set_field(L, "tip", l.tooltip);
        set_rect(L, l.rect);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "links");
    set_field(L, "errors", page.errors);
    return 1;
}

// tex, w, h = eq.AtlasTexture(file)   -- a picture of the atlas directory (nil when it cannot be read)
int AtlasTexture(lua_State* L) {
    MapState& s = *state_of(L);
    EqHost& h = *host_of(L);
    const auto* t = h.textures ? h.textures->game_file("Atlas/" + s.atlas_skin + "/" + luaL_checkstring(L, 1)) : nullptr;
    if (!t) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlightuserdata(L, t->id);
    lua_pushinteger(L, t->width);
    lua_pushinteger(L, t->height);
    return 3;
}

// server, zones, limit, expansion = eq.ZoneLinks()   -- what is known of how the zones connect on this server: server
// is true when its list came (Hailstone's server script), zones how many zones a link starts or ends at; limit what
// decides which zones it has: "server" (its list of zones, with expansion its current one, -1 all), "account" (the
// account's expansion bits leave some out) or nil (every zone). Asks the server for
// its list the first time it can in a session (GameConnection::request_zone_links), so call it while the zone
// finder is shown.
int ZoneLinks(lua_State* L) {
    MapState& s = *state_of(L);
    EqHost& h = *host_of(L);
    if (h.in_game && h.conn) h.conn->request_zone_links();
    lua_pushboolean(L, s.routes.has_server());
    lua_pushinteger(L, static_cast<lua_Integer>(s.routes_zones));
    if (!s.routes.server_zones().empty()) {
        lua_pushstring(L, "server");
        lua_pushinteger(L, s.routes.server_expansion());
        return 4;
    }
    std::uint32_t mask = 0;
    if (h.conn) {
        const auto& p = h.conn->profile();
        mask = p && p->expansions ? p->expansions : h.conn->world_expansions();
    }
    // Bits for every expansion RoF2 knows (Kunark .. Rain of Fear, 19): nothing is left out.
    if (mask != 0 && (mask & 0x7ffffu) != 0x7ffffu) lua_pushstring(L, "account");
    else lua_pushnil(L);
    return 3;
}

// route = eq.ZoneRoute(to [, from])   -- the fewest zone changes from `from` (the player's zone) to `to` (short
// names): {zone, ...} from the start to the destination, each {id, zone, name, expansion}, with route.assumed
// when it uses a zone line seen only from the far side. It passes only through zones the server has
// (MapState::zone_allowed). nil when no way is known, the server lacks the destination or a name is unknown.
int ZoneRoute(lua_State* L) {
    MapState& s = *state_of(L);
    EqHost& h = *host_of(L);
    const std::uint32_t to = game::zone_id_by_short_name(luaL_checkstring(L, 1));
    const std::uint32_t from = lua_isstring(L, 2) ? game::zone_id_by_short_name(lua_tostring(L, 2))
                               : h.conn                ? h.conn->zone_id()
                                                       : 0u;
    game::ZoneRoute r = s.routes.route(from, to, [&](std::uint32_t z) { return s.zone_allowed(h, z); });
    if (r.zones.empty()) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, static_cast<int>(r.zones.size()), 1);
    for (std::size_t i = 0; i < r.zones.size(); ++i) {
        push_zone(L, r.zones[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_pushboolean(L, r.assumed);
    lua_setfield(L, -2, "assumed");
    return 1;
}

// The current zone's exits, once the app has worked them out for this zone (EqHost::zone_exits).
const std::vector<EqHost::ZoneExit>* current_exits(const EqHost& h) {
    if (!h.in_game || !h.conn || h.zone_exits_zone == 0 || h.zone_exits_zone != h.conn->zone_id()) return nullptr;
    return &h.zone_exits;
}

// exits = eq.ZoneExits()   -- where the current zone's zone lines lead: {{id, zone, name, expansion, x, y, x0, y0,
// x1, y1}, ...} in map units (the middle and the box round the lines to that zone); empty before the zone points
// come and in a zone without lines.
int ZoneExits(lua_State* L) {
    EqHost& h = *host_of(L);
    const auto* exits = current_exits(h);
    lua_createtable(L, exits ? static_cast<int>(exits->size()) : 0, 0);
    if (!exits) return 1;
    int n = 0;
    for (const auto& e : *exits) {
        push_zone(L, e.zone_id);
        glm::vec3 a = game::world_to_map(glm::vec3(e.lo[0], e.lo[1], e.lo[2]));
        glm::vec3 b = game::world_to_map(glm::vec3(e.hi[0], e.hi[1], e.hi[2]));
        set_field(L, "x0", std::min(a.x, b.x));
        set_field(L, "y0", std::min(a.y, b.y));
        set_field(L, "x1", std::max(a.x, b.x));
        set_field(L, "y1", std::max(a.y, b.y));
        set_field(L, "x", (a.x + b.x) / 2.0f);
        set_field(L, "y", (a.y + b.y) / 2.0f);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// x, y = eq.ZoneExitWalk(zone)   -- walks to the current zone's nearest zone line to `zone` (short name) and
// through it: a /nav walk (docs/navigation.md; its goal inside the line, so the walk does not stop at it) to the
// point of the nearest line piece closest to the player, kept 2 units inside it. Returns where it goes in map
// units, or nil when no line leads there (or the player's place is not known yet).
int ZoneExitWalk(lua_State* L) {
    if (distribution::managed()) {
        lua_pushnil(L);
        return 1;
    }
    EqHost& h = *host_of(L);
    const std::uint32_t to = game::zone_id_by_short_name(luaL_checkstring(L, 1));
    const auto* exits = current_exits(h);
    if (!exits || to == 0 || !h.feet_valid || !h.submit_chat) {
        lua_pushnil(L);
        return 1;
    }
    const glm::vec3 me(h.feet[0], h.feet[1], h.feet[2]);
    std::optional<glm::vec3> best;
    float best_d = FLT_MAX;
    for (const auto& e : *exits) {
        if (e.zone_id != to) continue;
        for (const auto& c : e.cells) {
            glm::vec3 lo(c[0], c[1], c[2]), hi(c[3], c[4], c[5]);
            glm::vec3 p;
            for (int i = 0; i < 3; ++i) {
                // Inside by 2 where the piece is wide enough, else its middle.
                float a = lo[i] + 2.0f, b = hi[i] - 2.0f;
                p[i] = a <= b ? std::clamp(me[i], a, b) : (lo[i] + hi[i]) / 2.0f;
            }
            float d = glm::length(p - me);
            if (d < best_d) {
                best_d = d;
                best = p;
            }
        }
    }
    if (!best) {
        lua_pushnil(L);
        return 1;
    }
    const glm::vec3 server = game::world_to_server(*best); // /nav locxyz takes the server's x, y, z
    char line[128];
    std::snprintf(line, sizeof line, "/nav locxyz %.1f %.1f %.1f", static_cast<double>(server.x),
                  static_cast<double>(server.y), static_cast<double>(server.z));
    h.submit_chat(line);
    glm::vec3 m = game::world_to_map(*best);
    lua_pushnumber(L, m.x);
    lua_pushnumber(L, m.y);
    return 2;
}

// eq.ShowZoneMap([zone])   -- the map window shows another zone's map (short name), as an atlas zone link does;
// without one (or the player's own zone), the player's zone again.
// long, short, own = eq.MapZoneShown()   -- the zone whose map the map window shows; own: it is the player's.
int ShowZoneMap(lua_State* L) {
    MapState& s = *state_of(L);
    EqHost& h = *host_of(L);
    if (!h.in_game) return 0;
    const std::uint32_t id = lua_isstring(L, 1) ? game::zone_id_by_short_name(lua_tostring(L, 1)) : 0;
    if (id == 0 || lower(game::zone_short_name(id)) == lower(h.zone_short_name)) s.zone.clear(); // reloaded next frame
    else s.load_other_zone(h, game::zone_short_name(id));
    return 0;
}
int MapZoneShown(lua_State* L) {
    MapState& s = *state_of(L);
    EqHost& h = *host_of(L);
    const bool own = s.zone.empty() || lower(s.zone) == lower(h.zone_short_name);
    if (own) {
        lua_pushstring(L, h.zone_long_name.c_str());
        lua_pushstring(L, h.zone_short_name.c_str());
    } else {
        lua_pushstring(L, zone_title(game::zone_id_by_short_name(s.zone)).c_str());
        lua_pushstring(L, s.zone.c_str());
    }
    lua_pushboolean(L, own);
    return 3;
}

void register_map_api(lua_State* L, EqHost& host, std::vector<std::unique_ptr<StockWindow>>& windows) {
    MapState* state = nullptr;
    for (auto& w : windows)
        if (auto* m = dynamic_cast<MapViewWindow*>(w.get())) state = &m->state();
    if (!state) return;
    static const luaL_Reg fns[] = {{"DrawMap", DrawMap},         {"MapZoom", MapZoom},         {"MapMouseDown", MapMouseDown},
                                   {"MapMouseUp", MapMouseUp},   {"MapPanReset", MapPanReset}, {"MapRightClick", MapRightClick},
                                   {"DrawMinimap", DrawMinimap}, {"DrawZoneMap", DrawZoneMap},
                                   {"FindMapLocation", FindMapLocation}, {"DrawAtlas", DrawAtlas}, {"FindPathShown", FindPathShown}, {"EndFind", EndFind}, {"ZoneName", ZoneName}, {"MapAutoZ", MapAutoZ},       {"GameTime", GameTime},
                                   {"AtlasIndex", AtlasIndex}, {"AtlasPageData", AtlasPageData}, {"AtlasTexture", AtlasTexture},
                                   {"ZoneLinks", ZoneLinks}, {"ZoneRoute", ZoneRoute}, {"ZoneExits", ZoneExits},
                                   {"ZoneExitWalk", ZoneExitWalk}, {"ShowZoneMap", ShowZoneMap}, {"MapZoneShown", MapZoneShown},
                                   {nullptr, nullptr}};
    lua_pushlightuserdata(L, state);
    lua_pushlightuserdata(L, &host);
    luaL_setfuncs(L, fns, 2);
}

} // namespace hailstone::app
