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
//                                    ground (default 1.8 3)
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
// Plants grow only on opaque or cut-out surfaces (not the translucent water) and keep off water and lava regions and
// the water's surface (a zone line's region grows them), and off ground with the zone's terrain over it (a dungeon's
// ceiling, a roof, a bridge; outdoors only within kOutdoorTerrainCover, so not under a cliff's overhang) or a placed
// object's opaque surface low over it (a crate, a tent; not a tree's crown or its leaves), and off what the server
// places when the caller says so (FloraCover: its doors and objects, a campfire spawn).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
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
    float lawn_min = 1.8f, lawn_max = 3.0f; // the lawn's blade height, units
    std::vector<Ground> ground;
    // How far over a spot the zone's terrain still covers it (no plants under it): any height, or in an outdoor zone
    // kOutdoorTerrainCover (load_zone_flora_rules). Not a rules-file directive.
    float terrain_cover = std::numeric_limits<float>::infinity();
};
// In an outdoor zone the terrain covers the ground only this far over it (Hailstone's own choice): a hut's roof or a
// low bridge's deck, not a cliff's overhang or an arch high overhead.
inline constexpr float kOutdoorTerrainCover = 20.0f;

// The text of the rules file `name` (an include), or nullopt.
using ZoneFloraInclude = std::function<std::optional<std::string>(std::string_view name)>;
inline constexpr int kMaxFloraIncludeDepth = 4;
// Includes read in one parse, nested ones counted: a file naming another many times, at every depth, would otherwise
// read it lines^depth times. Hailstone's own bound; its own rules use one or two.
inline constexpr int kMaxFloraIncludes = 64;

// Reads a rules file. A line that does not parse is skipped and described in `errors` (with its line number) when
// given; an include is read through `include` (without it, or when it finds nothing, the line is an error).
ZoneFloraRules parse_zone_flora_rules(std::string_view text, std::vector<std::string>* errors = nullptr,
                                      const ZoneFloraInclude& include = {});

// The rules for `zone`: <rules_dir>/<zone>.txt, else (when `outdoor`) <rules_dir>/default.txt, includes resolved in
// `rules_dir`; nullopt when neither applies. `source` gets the file's name ("ecommons", "default"). When `outdoor`, the
// terrain covers only within kOutdoorTerrainCover (ZoneFloraRules::terrain_cover), for the client and tools alike.
std::optional<ZoneFloraRules> load_zone_flora_rules(const std::filesystem::path& rules_dir, std::string_view zone,
                                                    bool outdoor, std::string* source = nullptr);

// The tiles the plants grow by (ZoneFlora::tiles), in units.
inline constexpr float kZoneFloraTile = 128.0f;
// The lawn's squares (a patch of blades each), in units: world-aligned, kZoneFloraTile / 40, so a tile holds 40 x 40.
inline constexpr float kLawnCell = kZoneFloraTile / 40.0f;
// The lawn tapers where it meets bare ground over this many squares: a patch's height is times
// lawn_edge_factor(the distance to the nearest bare square's centre, in squares).
inline constexpr int kLawnEdgeCells = 3;
// 0.55 beside bare ground, rising smoothly to 1 at kLawnEdgeCells + 0.5 squares and past.
float lawn_edge_factor(float squares);
// A lawn patch with fewer of its eight neighbouring squares grown is dropped: a lone tuft of a stray green texel.
inline constexpr int kLawnMinNeighbours = 3;
// How far the lawn's height wanders: its value noise's lattice, in units.
inline constexpr float kLawnNoiseCell = 48.0f;
// The lawn's height at a spot (0..1 between lawn_height's min and max): smooth value noise over the world, so the
// carpet rises and falls gently rather than patch by patch.
float lawn_height_noise(glm::vec2 p);
// The lawn under cover (a platform, a bridge, a roof) does not trace the cover's outline: a square asks whether it is
// covered at a spot moved off its centre by lawn_cover_offset, which wanders smoothly over the world by up to
// kLawnCoverJitter units, so the bare ground's edge under a straight platform wobbles in and out.
inline constexpr float kLawnCoverJitter = kLawnCell * 3.0f;
glm::vec2 lawn_cover_offset(glm::vec2 p);
// The share of a patch's blades that stand (FloraInstance::cover), by the distance to the nearest bare square's centre
// in squares (as lawn_edge_factor) and a draw of its own for the square at `p`: between 0.25 and 0.85 beside bare
// ground, all of them at kLawnEdgeCells + 0.5 squares and past, so the lawn's edge frays rather than ending in rows.
float lawn_edge_cover(float squares, glm::vec2 p);

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

// Cover the zone's files do not hold (the server's doors and objects, a campfire spawn): true where no plant may
// stand at `at` (a plant's foot, or the spot off a lawn square's centre that it asks about, lawn_cover_offset).
using FloraCover = std::function<bool(const glm::vec3& at)>;
// A placed object (the zone's or the server's) covers the ground under it with a surface more than
// kObjectCoverClearance over it (a campfire's logs, a rug: not the ground's own triangle) and at most kObjectCoverReach
// (a crate's lid, a tent's roof; not a tree's crown). Hailstone's own choices.
inline constexpr float kObjectCoverReach = 16.0f, kObjectCoverClearance = 0.25f;
// Whether the triangle a b c lies over `at` (straight up) by more than `clearance` and at most `reach`. Seen edge-on
// from above it covers nothing.
bool covers_from_above(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& at, float clearance,
                       float reach);
// Whether a placed object is a plant (the modern look's sway, and the grass's cover): see-through leaves or blades (a
// masked material) and a name, of its own or of one of its materials or textures, that says so.
bool is_plant(std::string_view actor, const Mesh& mesh);

// Whether a placed object is a fire on the ground's kind (a campfire, its logs and coals): a flame material
// (flame_material_hue), or a name, of its own or of one of its materials or textures, with "fire", "camp" or "coal".
bool is_fire_object(std::string_view actor, const Mesh& mesh);

// A small, low fire (is_fire_object, within kSmallObjectRadius of its centre in the plane and kObjectCoverReach from
// its bottom to its top) covers its whole footprint, a kFootprintSides-sided disc round it kFootprintMargin wider
// (half a lawn square: a square's blades spread that far from its centre), not only the ground under its surfaces: a
// campfire's ring of logs has nothing over its middle, and its flame never covers. Any other object covers by its
// surfaces, so the grass grows up to a rock's foot (what grows inside a solid one is hidden). Hailstone's own choices.
inline constexpr float kSmallObjectRadius = 12.0f, kFootprintMargin = kLawnCell * 0.5f;
inline constexpr int kFootprintSides = 16;
// A mesh's triangles that may cover (not invisible or additive; only the opaque ones when `opaque_only`), through
// `transform` when given, appended to `out` (3 a triangle). A triangle with an index out of range is left out.
void cover_triangles(const Mesh& mesh, const glm::mat4* transform, bool opaque_only, std::vector<glm::vec3>& out);
// A placed object's cover from its triangles in the world (3 a triangle): `drawn` all it draws, for its size;
// `surfaces` those that cover from above (a zone object's opaque ones; none for a plant), walls left out. Appended to
// `out`: its footprint disc when `fire` and it is small and low, else its surfaces. Test them with
// kObjectCoverClearance and kObjectCoverReach.
void append_object_cover(std::span<const glm::vec3> drawn, std::span<const glm::vec3> surfaces, bool fire,
                         std::vector<glm::vec3>& out);

// A spawn that stands for an object on the ground (a campfire, race 567): its footprint round its feet covers the
// grass. Hailstone's own: the spawn's model is not measured, a disc of its size (kCoverDiscMin .. kCoverDiscMax)
// stands for it.
struct CoverDisc {
    glm::vec3 feet{0.0f};
    float radius = 0.0f;
    // `at` within `radius` of the feet in the plane and kCoverDiscHeight of them up or down.
    bool covers(const glm::vec3& at) const;
    bool operator==(const CoverDisc&) const = default;
};
inline constexpr float kCoverDiscMin = 3.0f, kCoverDiscMax = 10.0f, kCoverDiscHeight = 6.0f;
// A campfire spawn's disc: radius its size clamped to kCoverDiscMin .. kCoverDiscMax (a size of 0 or none takes the
// least), and kFootprintMargin.
CoverDisc campfire_cover(const glm::vec3& feet, float size);

// Placed objects' triangles (3 a triangle, in the world) binned over the square [lo, hi] for covered(): what a
// tile's cover from the server's objects is tested against. Triangles outside the square are not kept; a point
// outside it is never covered.
class ObjectCoverTris {
public:
    ObjectCoverTris(const std::vector<glm::vec3>& tris, glm::vec2 lo, glm::vec2 hi);
    // Whether one of them covers `at` (covers_from_above with kObjectCoverClearance and kObjectCoverReach).
    bool covered(const glm::vec3& at) const;
    bool empty() const { return tris_.empty(); }

private:
    static constexpr float kCell = 4.0f;
    static constexpr std::int32_t kMaxSide = 256;
    std::vector<glm::vec3> tris_;
    glm::vec2 lo_{0.0f};
    std::int32_t nx_ = 0, ny_ = 0;
    std::vector<std::uint32_t> start_, index_;
};

// How far past a tile's square such cover may change the tile's plants: the lawn's edge softening looks this far
// (kLawnEdgeCells squares) and a lawn square asks about a spot up to kLawnCoverJitter off its centre.
inline constexpr float kFloraCoverMargin = kLawnCell * static_cast<float>(kLawnEdgeCells) + kLawnCoverJitter;

// A zone's flora grown a tile at a time: render::FloraRenderer grows the tiles round the camera as they come into
// reach and lets them go when they leave it, so a large green zone costs memory by the reach, not by its size. At
// construction it keeps the ground triangles that may grow plants (by `rules`: the texture's rule, level enough, a
// surface that is drawn), binned by tile, with the zone's regions and its cover; it holds no reference to the zone.
// Each triangle's plants come from its own seeded sequence, so a tile grows the same plants every time.
// Ground past the zone's own that may grow its flora (Hailstone's own: the land round an outdoor zone near its edge,
// render::horizon_lawn_ground), taken as the zone's ground of `texture` is: triangles (three corners each, any
// winding), their texture coordinates the position times `density` (texture units a world unit), as the land draws it.
struct FloraExtraGround {
    std::vector<glm::vec3> triangles;
    std::string texture;
    float density = 0.0f;
    bool empty() const { return triangles.size() < 3 || texture.empty() || !(density > 0.0f); }
};

class ZoneFloraGrower {
public:
    // `textures` gives the ground textures for +green and the tint (without it, or for a texture it cannot read,
    // +green grows nothing); each is read once, here, and kept scaled down. `extra`: more ground (FloraExtraGround).
    ZoneFloraGrower(const Zone& zone, const ZoneFloraRules& rules, std::vector<eqg::FloraDefinition> definitions,
                    const GroundTextures& textures, const FloraExtraGround* extra = nullptr);
    ~ZoneFloraGrower();
    ZoneFloraGrower(const ZoneFloraGrower&) = delete;
    ZoneFloraGrower& operator=(const ZoneFloraGrower&) = delete;

    const std::vector<eqg::FloraDefinition>& definitions() const;
    // The tiles that may grow plants, by their corner (kZoneFloraTile on a side).
    const std::vector<glm::vec2>& tiles() const;
    std::size_t triangles() const;
    // Tile `tile`'s plants (an index into tiles()), appended to `out`, at most kMaxTileFlora. `by_texture`
    // (tools/florascan) counts them by ground texture (lower case). `extra`: more cover, as the zone's own is tested.
    // Reads the grower only (render::FloraRenderer calls it on a thread of its own).
    void grow(std::size_t tile, std::vector<FloraInstance>& out,
              std::unordered_map<std::string, std::size_t>* by_texture = nullptr, const FloraCover& extra = {}) const;

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
std::size_t add_zone_flora(const GameDir& game, Zone& zone, const std::filesystem::path& rules_dir, bool outdoor,
                           const FloraExtraGround* extra = nullptr);
// The zone's flora set up again with `extra` ground too (the land near its edge, once the zone renderer has built
// it): only a zone whose flora Hailstone's rules grow (ZoneFlora::grower), not an EQG zone's own. Returns whether it
// was; the plants a renderer has grown are then out of date (render::FloraRenderer::restart).
bool add_zone_flora_ground(const GameDir& game, Zone& zone, const std::filesystem::path& rules_dir, bool outdoor,
                           const FloraExtraGround& extra);

} // namespace hailstone::assets
