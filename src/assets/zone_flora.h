#pragma once

// Flora for zones that have none of their own (docs/graphics.md "Zone flora"): Hailstone's own, not the original's.
// The classic (WLD) zones carry no flora data; a rules file of this client's (flora/<zone>.txt in the install folder)
// names the ground textures that grow plants: a lawn (blades the shaders build, no game asset: an even carpet over
// the ground, a patch per kLawnCell square) and, scattered over it, plants of the kinds it names, whose shapes and
// pictures are the RadialFloraDefs.rfd and textures of EQG archives in the player's game folder (the forests take
// oldkithicor.eqg's ferns). Nothing in the zone's files changes; the plants are placed over the zone's terrain as it loads,
// into Zone::flora, and drawn by render::FloraRenderer like an EQG zone's. They are looks only: no collision, line of
// sight or picking sees them.
//
// The rules file, one directive a line, '#' to the end of a line a comment:
//   donor <archive>                  an EQG archive of the shapes and textures (oldkithicor.eqg); several donor lines
//                                    add up, their definitions named <archive stem>/<name> (oldkithicor/fern)
//   max_slope <degrees>              steeper ground grows nothing (default 40)
//   scale <min> <max>                every plant's size times a random factor in the range (default 1 1)
//   lawn_height <min> <max>          the lawn's blades, in units: their height wanders between the two slowly over the
//                                    ground (default 1.4 2.4)
//   ground <glob> <density> [+green|+greenish] [lawn] [<kind>[:<weight>]...]
//                                    ground whose texture matches the glob (case-insensitive, '*' and '?') grows the
//                                    lawn when the line says `lawn` (every kLawnCell square whose centre passes the
//                                    tests below), and <density> plants per 100 square units, each one of the kinds
//                                    (definition names
//                                    of the donors' RadialFloraDefs.rfd, the first donor's that has it, or one
//                                    donor's as <archive stem>/<name>) picked by weight (default 1); the first
//                                    matching line wins, and a line with a density of 0 and no lawn keeps a texture
//                                    bare. With +green a plant
//                                    grows only where the texel under it is green (is_green_texel): the classic
//                                    zones' ground textures blend grass into cliff, sand or a path within one picture.
//                                    +greenish (is_greenish_texel) also takes olive and dark grass: for a texture whose
//                                    name already says it is grass, where only its paths and bare patches are to go
//   include <name>                   reads flora/<name>.txt's lines here (default: the shared rules)
// Lines apply in order: a later max_slope or scale replaces an earlier one, and the first matching ground line
// wins, so a zone's own exceptions go before its include.
// A zone without a file of its own takes flora/default.txt when the server calls it outdoor (time type 1, 2 or 5).
// Plants grow only on opaque or cut-out surfaces (not the translucent water) and keep off water, lava and zone-line
// regions and the water's surface, and off ground with the zone's terrain over it (a dungeon's ceiling, a roof, a
// bridge; not the placed objects, so grass grows under trees).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "assets/eqg.h"
#include "assets/flora.h"
#include "assets/texture.h"

namespace hailstone { class GameDir; }

namespace hailstone::assets {

struct Zone;

struct ZoneFloraRules {
    struct Kind {
        std::string definition;
        float weight = 1.0f;
    };
    struct Ground {
        std::string texture; // glob
        float density = 0.0f; // plants per 100 square units
        bool green_only = false; // +green or +greenish
        bool greenish = false;   // +greenish
        bool lawn = false;       // lawn
        std::vector<Kind> kinds;
        bool grows() const { return lawn || (density > 0.0f && !kinds.empty()); }
    };
    std::vector<std::string> donors; // in order
    float max_slope = 40.0f; // degrees
    float min_scale = 1.0f, max_scale = 1.0f;
    float lawn_min = 1.4f, lawn_max = 2.4f; // the lawn's blade height, units
    std::vector<Ground> ground;
};

// The text of the rules file `name` (an include), or nullopt.
using ZoneFloraInclude = std::function<std::optional<std::string>(std::string_view name)>;
inline constexpr int kMaxFloraIncludeDepth = 4;

// Reads a rules file. A line that does not parse is skipped and described in `errors` (with its line number) when
// given; an include is read through `include` (without it, or when it finds nothing, the line is an error).
ZoneFloraRules parse_zone_flora_rules(std::string_view text, std::vector<std::string>* errors = nullptr,
                                      const ZoneFloraInclude& include = {});

// The rules for `zone`: <rules_dir>/<zone>.txt, else (when `outdoor`) <rules_dir>/default.txt, includes resolved in
// `rules_dir`; nullopt when neither applies. `source` gets the file's name ("ecommons", "default").
std::optional<ZoneFloraRules> load_zone_flora_rules(const std::filesystem::path& rules_dir, std::string_view zone,
                                                    bool outdoor, std::string* source = nullptr);

// The tiles the plants grow by (ZoneFlora::tiles), in units.
inline constexpr float kZoneFloraTile = 128.0f;
// The lawn's squares (a patch of blades each), in units: world-aligned, kZoneFloraTile / 40, so a tile holds 40 x 40.
inline constexpr float kLawnCell = kZoneFloraTile / 40.0f;
// The lawn tapers where it meets bare ground over this many squares: a patch's height is times
// lawn_edge_factor(the distance to the nearest bare square's centre, in squares).
inline constexpr int kLawnEdgeCells = 3;
// 0.3 beside bare ground, rising smoothly to 1 at kLawnEdgeCells + 0.5 squares and past.
float lawn_edge_factor(float squares);
// How far the lawn's height wanders: its value noise's lattice, in units.
inline constexpr float kLawnNoiseCell = 48.0f;
// The lawn's height at a spot (0..1 between lawn_height's min and max): smooth value noise over the world, so the
// carpet rises and falls gently rather than patch by patch.
float lawn_height_noise(glm::vec2 p);

// The most plants one tile grows: a bound for a rules file asking too much (a lawn's tile holds 1,600 patches).
inline constexpr std::size_t kMaxTileFlora = 65536;

// Whether a texel (RGB bytes) is grass for +green: green over red and well over blue, and not near black.
bool is_green_texel(std::uint8_t r, std::uint8_t g, std::uint8_t b);
// The same for +greenish: green at least as strong as red (dry, olive grass) and over blue.
bool is_greenish_texel(std::uint8_t r, std::uint8_t g, std::uint8_t b);

// A ground texture by name (lower case) in RGBA8, rows as the WLD meshes' UVs address them, or nullopt when it
// cannot be read: what +green and the plants' tint read.
using GroundTextures = std::function<std::optional<Image>(std::string_view texture)>;
// A +green texture whose share of grass texels is below this grows nothing (a sea floor of teal sand, a grey cliff):
// its ground is not walked plant by plant only to be turned down.
inline constexpr float kMinGreenShare = 0.002f;

// A zone's flora grown a tile at a time: render::FloraRenderer grows the tiles round the camera as they come into
// reach and lets them go when they leave it, so a large green zone costs memory by the reach, not by its size. At
// construction it keeps the ground triangles that may grow plants (by `rules`: the texture's rule, level enough, a
// surface that is drawn), binned by tile, with the zone's regions and its cover; it holds no reference to the zone.
// Each triangle's plants come from its own seeded sequence, so a tile grows the same plants every time.
class ZoneFloraGrower {
public:
    // `textures` gives the ground textures for +green and the tint (without it, or for a texture it cannot read,
    // +green grows nothing); each is read once, here, and kept scaled down.
    ZoneFloraGrower(const Zone& zone, const ZoneFloraRules& rules, std::vector<eqg::FloraDefinition> definitions,
                    const GroundTextures& textures);
    ~ZoneFloraGrower();
    ZoneFloraGrower(const ZoneFloraGrower&) = delete;
    ZoneFloraGrower& operator=(const ZoneFloraGrower&) = delete;

    const std::vector<eqg::FloraDefinition>& definitions() const;
    // The tiles that may grow plants, by their corner (kZoneFloraTile on a side).
    const std::vector<glm::vec2>& tiles() const;
    std::size_t triangles() const;
    // Tile `tile`'s plants (an index into tiles()), appended to `out`, at most kMaxTileFlora. `by_texture`
    // (tools/florascan) counts them by ground texture (lower case). Not thread-safe (the texel lookup caches).
    void grow(std::size_t tile, std::vector<FloraInstance>& out,
              std::unordered_map<std::string, std::size_t>* by_texture = nullptr) const;

    // A ground triangle that may grow plants (or a piece of a large one).
    struct Piece {
        glm::vec3 p[3];
        glm::vec2 uv[3];
        std::uint32_t seed = 0;
        std::uint32_t texture = 0; // index into the textures it grows on
        std::uint32_t rule = 0;    // index into the rules' ground lines
        float area = 0.0f;
    };

    struct Data; // opaque (zone_flora.cpp)

private:
    std::unique_ptr<Data> d_;
};

// Every tile of `zone`'s flora by `rules` grown at once into `out` (definitions, instances and tiles replaced): for
// tests and tools; the client grows them lazily (ZoneFloraGrower).
void place_zone_flora(const Zone& zone, const ZoneFloraRules& rules, std::vector<eqg::FloraDefinition> definitions,
                      ZoneFlora& out, const GroundTextures& textures = {},
                      std::unordered_map<std::string, std::size_t>* by_texture = nullptr);

// A zone's ground texture as +green reads it: the game's own file (not a texture pack's) in RGBA8, rows as the WLD
// meshes' UVs address them; nullopt when it cannot be read.
std::optional<Image> zone_ground_texture(const Zone& zone, std::string_view name);
// zone_ground_texture for each name; `zone` must outlive it (a ZoneFloraGrower reads them at construction only).
GroundTextures zone_ground_textures(const Zone& zone);

// A donor archive and its RadialFloraDefs.rfd; nullopt (logged) when it is missing or has none.
struct FloraDonor {
    std::shared_ptr<const PfsArchive> archive;
    std::vector<eqg::FloraDefinition> definitions;
};
std::optional<FloraDonor> open_flora_donor(const GameDir& game, std::string_view donor);

// The donors of a rules file, their definitions together, each named <archive stem>/<name>; nullopt when none opens.
struct FloraDonors {
    std::vector<std::shared_ptr<const PfsArchive>> archives;
    std::vector<eqg::FloraDefinition> definitions;
};
std::optional<FloraDonors> open_flora_donors(const GameDir& game, std::span<const std::string> donors);
// Whether a rules file's kind names the definition (as open_flora_donors names them): "fern" any donor's fern,
// "oldkithicor/fern" that donor's alone.
bool flora_kind_matches(std::string_view kind, std::string_view definition);

// For a zone without flora of its own: takes its rules (load_zone_flora_rules), opens their donor archives from
// `game` and sets zone.flora up to grow (ZoneFlora::grower). Returns how many tiles may grow plants (0 when the zone
// has flora already, no rules apply, or nothing would grow: no lawn and no donor's definitions; each logged). The
// lawn needs no donor.
std::size_t add_zone_flora(const GameDir& game, Zone& zone, const std::filesystem::path& rules_dir, bool outdoor);

} // namespace hailstone::assets
