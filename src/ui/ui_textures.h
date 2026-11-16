#pragma once

// The stock UI's art as ImGui textures, for the Lua windows (src/app/lua_ui.h): any file of uifiles/default
// (window pieces, gauges, buttons; the layouts name them through EQUI_Animations.xml), plus the icon grids the
// game's widgets draw from:
//   item icons   A_DragItem   dragitem1.dds.. : 40 x 40 cells, 6 x 6 per sheet, down each column first; icon n >= 500
//   spell icons  A_SpellIcons spells01.tga..  : 40 x 40 cells, 6 x 6 per sheet, across each row first
//   gem icons    A_SpellGems  gemicons01.tga..: 24 x 24 cells, 10 x 10 per sheet, across each row first (the spell's
//                                              SpellIcon, EQ_Spell +0x168: CSpellGemWnd::Init / SetSpellIconIndex)
// Files load on first use and stay loaded, up to kMaxFiles: past that the one asked for least recently goes (a
// script may ask hs.UiTexture for any file), its texture once the frames that may draw it are done.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone { class GameDir; }
namespace hailstone::render { class VkContext; }

namespace hailstone::ui {

class UiTextures {
public:
    struct Texture {
        void* id = nullptr; // ImTextureID
        int width = 0, height = 0;
    };
    struct Cell {
        void* id = nullptr;
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    };
    struct AnimationFrame {
        std::string file;
        int x = 0, y = 0, w = 0, h = 0;
        bool valid_metadata = true; // malformed/overflowed XML integers never become a zero-origin crop
    };
    struct AnimationImage {
        Cell cell;
        int width = 0, height = 0; // original XML frame pixels, not atlas size
    };

    // The files (and game_file pictures) held at most, loaded or not found: well past the stock UI's art and icon
    // sheets, so a UI does not load the same file again and again.
    static constexpr std::size_t kMaxFiles = 1024;

    UiTextures(render::VkContext& vk, const GameDir& game);
    ~UiTextures();
    UiTextures(const UiTextures&) = delete;
    UiTextures& operator=(const UiTextures&) = delete;

    // Whether every uifiles/default file is used, or only the icon sheets (is_icon_file): a UI whose ui.ini has
    // game_art = icons draws its windows itself (ui/ui_packs.h). All by default.
    void set_all_art(bool all) { all_art_ = all; }
    bool all_art() const { return all_art_; }
    // A file of uifiles/default (any case); null when it is missing, cannot be decoded or is not used (set_all_art).
    const Texture* file(std::string_view name);
    // Any image of the game folder by its relative path (any case; the atlas pages' pictures), whatever
    // set_all_art says; null when it is missing or cannot be decoded.
    const Texture* game_file(std::string_view relative);
    // A picture file of the disk (PNG, JPEG, BMP, TGA, DDS: the pre-game screens' pictures, app/screen_pictures.h);
    // null when it cannot be read or decoded. A few stay loaded; older ones are let go (VkContext::retire).
    const Texture* picture(const std::filesystem::path& file);
    std::optional<Cell> item_icon(std::uint32_t icon);
    std::optional<Cell> spell_icon(std::int32_t icon);
    std::optional<Cell> gem_icon(std::int32_t icon);
    // The first frame of a stock animation (<Ui2DAnimation item="name"> in any uifiles/default XML file, as
    // CSidlManagerBase::FindAnimation finds it).
    std::optional<Cell> animation(std::string_view name);
    // An explicit request for named XML art, independent of game_art=icons.
    // Only a relative atlas filename and a positive, wholly contained frame
    // are admitted. Missing/invalid art leaves the caller its text fallback.
    std::optional<AnimationImage> animation_image(std::string_view name);
    // A grid animation (<Grid>true</Grid>: A_DragItem, A_SpellGems, A_MenuIcons ...): its cell count (0x87a820:
    // the first frame's columns x rows x the frames; nullopt: no such grid animation) and cell `index` (frame by
    // frame; down each column first when <Vertical>, else across each row), as IconCache::GetIcon draws it.
    std::optional<int> grid_cells(std::string_view name);
    std::optional<Cell> grid_cell(std::string_view name, int index);
    // A top-level screen's stock <Location> and <Size> (x, y, w, h) from its EQUI_*.xml file.
    struct Rect {
        int x = 0, y = 0, w = 0, h = 0;
    };
    std::optional<Rect> screen_rect(std::string_view screen);

private:
    struct Entry;
    std::optional<Cell> grid(const char* pattern, int sheet, int cell_size, int x, int y);
    const Texture* load(const std::string& key, const std::string& path);
    // Decodes and uploads; null (with a warning naming `what`) when it cannot.
    std::unique_ptr<Entry> make(std::span<const std::byte> data, const std::string& what);
    // Lets the entry's texture go once no frame may draw it (VkContext::retire); the Lua bindings stop drawing its
    // ID at once.
    void retire(std::unique_ptr<Entry> e);

    render::VkContext& vk_;
    const GameDir& game_;
    void* sampler_ = nullptr; // VkSampler
    struct File {
        std::unique_ptr<Entry> entry; // null: missing or not decoded
        std::uint64_t used = 0;       // lookups_ when last asked for
    };
    std::map<std::string, File> files_;
    std::uint64_t lookups_ = 0;
    std::map<std::string, std::unique_ptr<Entry>> pictures_; // by path
    std::vector<std::string> picture_order_;                 // the oldest first
    using Frame = AnimationFrame;
    std::map<std::string, Frame> animations_; // by item name
    struct Grid {
        int cell_w = 0, cell_h = 0;
        bool vertical = false;
        std::vector<Frame> frames;
    };
    std::map<std::string, Grid> grids_; // the grid animations, by item name
    bool animations_loaded_ = false;
    bool all_art_ = true;
    std::map<std::string, Rect> screens_;
    void load_skin(); // animations_ and screens_, once
};

// Pure metadata/crop helpers shared by explicit animation drawing and tests.
// No asset bytes or GPU handles are owned by the returned descriptors.
std::optional<UiTextures::AnimationFrame> find_ui_animation_frame(const std::string& xml, std::string_view name);
std::optional<UiTextures::AnimationImage> ui_animation_crop(const UiTextures::Texture& texture,
                                                          const UiTextures::AnimationFrame& frame);

// A raster asset's relative GameDir path, normalized for its index. Absolute paths, ambiguous components,
// traversal, control characters and non-image extensions are refused. This grants no arbitrary disk access.
std::optional<std::string> ui_image_path(std::string_view relative);

// Grid arithmetic (tested): the sheet (1-based) and the top-left pixel of an icon's cell.
struct GridCell {
    int sheet = 0;
    int x = 0, y = 0;
};
std::optional<GridCell> gem_icon_cell(std::int32_t icon);
// Cell `index` of a grid of frames, each frame_w x frame_h cut into cell_w x cell_h cells (frame = sheet - 1).
std::optional<GridCell> grid_index_cell(int index, int frames, int frame_w, int frame_h, int cell_w, int cell_h,
                                        bool vertical);
// The icon sheets of uifiles/default (item icons dragitem*, spell icons spells*, gem icons gemicons*), by file name.
bool is_icon_file(std::string_view name);

// The cache's rule (tested): the entry of `map` whose `used(value)` is lowest, the one asked for least recently; end()
// for an empty map.
template <class Map, class Used> typename Map::iterator least_recently_used(Map& map, Used&& used) {
    auto oldest = map.end();
    for (auto it = map.begin(); it != map.end(); ++it)
        if (oldest == map.end() || used(it->second) < used(oldest->second)) oldest = it;
    return oldest;
}

} // namespace hailstone::ui
