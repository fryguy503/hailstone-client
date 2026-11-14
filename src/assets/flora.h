#pragma once

// The radial flora of EQG heightmap zones: grass, flowers and ferns scattered over the terrain by the ecosystems'
// *FLORAPART layers (assets/eqg.h EcoFloraLayer) with the shapes of RadialFloraDefs.rfd (FloraDefinition). The
// placement follows EQGraphicsDX9's 0x101084e0 (docs/formats/eqg.md "Radial flora"): per tile and layer, the share
// of the tile its texture layer covers (mask >= MINALPHA) times DENSITY gives how many to place, the fraction by
// chance; up to ITERATIONS random spots are tried, each kept when the mask there reaches MINALPHA and the height and
// slope lie in the layer's bands (widened by their tolerances); a kept one takes a random size of its definition, a
// random scale of the layer's and a random turn. The random numbers are the MSVC rand() sequence (0..32767), seeded
// per tile here.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "assets/eqg.h"
#include "assets/pfs.h"

namespace hailstone::assets {

class ZoneFloraGrower;

// A FloraInstance's definition for a patch of Hailstone's lawn (assets/zone_flora.h): blades built in the shaders
// over a square of `width` a side, not a definition's picture.
inline constexpr std::uint32_t kLawnDefinition = 0xffffffffu;

// One plant: its foot, its turn (degrees about the vertical), its size, the terrain's colour under it (ARGB, for
// *INHERITVERTEXCOLOR) and its definition (index into ZoneFlora::definitions, or kLawnDefinition). A lawn patch also
// keeps the ground's slope under it (dz/dx, dz/dy), so its blades stand on the ground across the whole square.
struct FloraInstance {
    glm::vec3 position{0.0f};
    float rotation = 0.0f;
    float width = 1.0f, height = 1.0f;
    std::uint32_t color = 0xffffffffu;
    std::uint32_t definition = 0;
    glm::vec2 slope{0.0f};
};

// A zone's flora, by tile (the instances of tile t are instances[first, first + count)).
struct ZoneFlora {
    std::vector<eqg::FloraDefinition> definitions;
    std::vector<FloraInstance> instances;
    struct Tile {
        glm::vec2 min{0.0f};
        std::uint32_t first = 0, count = 0;
    };
    std::vector<Tile> tiles;
    float tile_size = 0.0f;
    // The archives the definitions' textures are read from, in order, when they are not the zone's own
    // (assets/zone_flora.h: the donor EQG archives of a zone given flora by Hailstone's rules); none for an EQG zone's
    // own flora.
    std::vector<std::shared_ptr<const PfsArchive>> donors;
    // Hailstone's zone flora (assets/zone_flora.h) grows its tiles on demand: then `tiles` lists the ones that may
    // grow plants (first and count unused) and `instances` is empty. Null for an EQG zone's own flora.
    std::shared_ptr<const ZoneFloraGrower> grower;
    bool empty() const { return instances.empty() && !grower; }
};

// MSVC's rand() (0x1011060e): 0..32767.
struct MsvcRand {
    std::uint32_t state = 1;
    std::int32_t operator()() {
        state = state * 214013u + 2531011u;
        return static_cast<std::int32_t>(state >> 16 & 0x7fff);
    }
};

// One tile of one ecosystem, as the terrain loader has it: S x S texels over the tile, each texel's height and slope
// (whole degrees), the ecosystem's texture layer masks by name (0..255 per texel) and, for an ecosystem drawn over
// the first, its DAT mask (the share of the tile it takes, 0..255 per texel; empty for the first).
struct FloraTile {
    glm::vec2 origin{0.0f};
    float tile_size = 0.0f;
    std::uint32_t texels = 0;
    std::span<const float> heights;
    std::span<const std::int32_t> slopes;
    std::span<const std::string> layer_names;
    std::span<const std::vector<std::uint8_t>> layer_masks;
    std::span<const std::uint8_t> dat_mask;
    // The terrain's height and colour at a tile-local spot (0..1 each way).
    std::function<float(float u, float v)> height_at;
    std::function<std::uint32_t(float u, float v)> color_at;
};

// The seed of a tile's flora sequence for ecosystem `ecosystem` (Hailstone's own, so a tile grows the same plants at
// every load): lng x 73856093 ^ lat x 19349663 ^ ecosystem x 83492791, each product wrapped to 32 bits. Worked in
// unsigned arithmetic, so it is defined for every tile and gives the values the signed products gave where they did
// not overflow (and two's-complement wrapping where they did).
std::uint32_t flora_tile_seed(std::int32_t lng, std::int32_t lat, std::size_t ecosystem);

// The most plants one call of place_flora adds: a bound for damaged files, far above what the game's place on a tile
// (each layer at most its *ITERATIONS, 500 at most in the game's files).
inline constexpr std::size_t kMaxFloraPerTile = 65536;

// Places the flora of `layers` on the tile, appending to `out`, at most kMaxFloraPerTile of them. `definitions`
// gives the shapes by name (a layer naming none places nothing).
void place_flora(const FloraTile& tile, std::span<const eqg::EcoFloraLayer> layers,
                 std::span<const eqg::FloraDefinition> definitions, MsvcRand& rand, std::vector<FloraInstance>& out);

} // namespace hailstone::assets
