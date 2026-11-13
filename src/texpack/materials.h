#pragma once

// Photo materials (docs/graphics.md "Photo materials"): a classic zone's hand-painted ground tiles rebuilt from
// tiling photo textures (Poly Haven's, CC0), as a texture pack's files in place of the upscaler's. Each tile is one
// material, or two the original blends (grass into rock, grass into a path); the original's painted layout and
// colour are kept and the photos give the detail below a few of its texels, so a rebuilt tile still meets the
// tiles round it. The rules (which material each texture is) are data, read from a folder the texture studio ships.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "assets/texture.h"
#include "texpack/texpack.h"
#include "texpack/upscale.h"

namespace hailstone {
class GameDir;
namespace assets { struct Zone; }
} // namespace hailstone

namespace hailstone::texpack {

// One photo material: materials.txt's [<name>] section.
struct MaterialClass {
    std::string name;  // "grass"
    std::string asset; // the Poly Haven asset id ("leafy_grass")
    double metres = 2.0;  // the ground one repeat of the photo shows
    double magnify = 3.0; // drawn this many times its real size (EQ's ground reads better a little coarse)
    float detail = 1.0f;  // strength of the photo's detail (a power of its luminance ratio)
    float shading = 0.0f; // how much of the original's own painted shading under the detail radius stays
    // The colour the material has in the art (0..1): where a two-material tile's split starts.
    float colour[3] = {0.3f, 0.3f, 0.3f};
};

// One game texture: a line of a <zone>.txt.
struct TileRule {
    std::string zone;    // the zone's archive stem ("qeytoqrg"): the rule takes that zone's texture of the name
    std::string texture; // lower case, no extension ("hhdrockgrass1")
    std::string first, second; // the materials; `second` empty: the tile is all `first`
    bool sharp = false;  // small shapes (a path's flagstones): the split and the colour follow the original closely
};

struct MaterialRules {
    std::vector<MaterialClass> classes;
    std::vector<TileRule> tiles;
    const MaterialClass* find(std::string_view name) const;
    // Changes whenever anything that shapes a tile's files changes (a pack keeps it to know what to make again).
    std::string signature(const TileRule& tile) const;
};

// materials.txt (the classes, an ini file) and <zone>.txt (one "texture = material [material] [sharp]" a line, '#'
// comments). Errors name the file and line; a rule naming no known material is an error.
std::optional<MaterialRules> parse_material_rules(std::string_view materials_txt,
                                                  const std::vector<std::pair<std::string, std::string>>& zone_files,
                                                  std::string* error = nullptr);
// Every <zone>.txt beside materials.txt in `dir`.
std::optional<MaterialRules> load_material_rules(const std::filesystem::path& dir, std::string* error = nullptr);

// World units per repeat of each texture on a zone's geometry: the area-weighted median over its triangles, by lower
// case name without extension.
std::map<std::string, double> measure_tile_sizes(const assets::Zone& zone);

// A texture the build makes from materials.
struct MaterialTile {
    TileRule rule;
    double units = 0;      // world units per repeat in its zone
    std::string signature; // MaterialRules::signature, with the size
};
struct MaterialPlan {
    MaterialRules rules;
    std::unordered_map<std::uint64_t, MaterialTile> tiles; // by texture hash
    std::uint32_t size = 1024; // the tiles' side in the pack
    const MaterialTile* find(std::uint64_t hash) const;
};
// The textures of `textures` the rules name, in the zones the rules have (loaded from `game`, to measure them). A
// texture with alpha or a variant is left to the upscaler; `notes` says what was left and why.
MaterialPlan plan_materials(const MaterialRules& rules, const GameDir& game, const std::vector<TextureInfo>& textures,
                            std::uint32_t size, std::vector<std::string>* notes = nullptr);

// The maps a material is made from, as Poly Haven names them, and where they are kept and fetched from.
inline constexpr std::string_view kMaterialMaps[] = {"diff", "nor_dx", "rough", "disp"};
std::filesystem::path material_file(const std::filesystem::path& dir, std::string_view asset, std::string_view map);
std::string material_url(std::string_view asset, std::string_view map);

// The files of `rules`' materials that `dir` lacks.
std::vector<std::pair<std::string, std::string>> missing_materials(const MaterialRules& rules,
                                                                   const std::filesystem::path& dir);
struct FetchOptions {
    std::string user_agent = "Hailstone-TextureStudio";
    std::function<bool()> cancelled;
    std::function<void(std::size_t done, std::size_t total)> progress; // files
    // Fetches one URL (tests give one; empty: platform::http_get). False and `error` when it failed.
    std::function<bool(const std::string& url, std::vector<std::byte>& body, std::string& error)> get;
};
// Fetches the missing files into `dir`. False and `error` on the first failure (the files fetched stay).
bool fetch_materials(const MaterialRules& rules, const std::filesystem::path& dir, const FetchOptions& options,
                     std::string* error = nullptr);

// The materials' images, loaded once and tiled on demand (thread-safe: the build's encoders share one).
class MaterialLibrary {
public:
    // Loads every class's maps from `dir`; nullopt and `error` when one is missing or unreadable.
    static std::shared_ptr<MaterialLibrary> load(const MaterialRules& rules, const std::filesystem::path& dir,
                                                 std::string* error = nullptr);
    struct Maps {
        Planes diff;  // RGB
        Planes nor;   // RGB, DirectX's way (green down the picture), upright
        Planes rough; // one channel
        Planes disp;  // one channel, mean 0 and deviation 1
        float mean[3] = {0.5f, 0.5f, 0.5f}; // diff's mean colour (filled by tiled())
    };
    // Tests and tools give maps directly.
    void add(const std::string& material, Maps maps);
    // The material `repeats` times across a square of `size`.
    std::shared_ptr<const Maps> tiled(const std::string& material, std::uint32_t repeats, std::uint32_t size);

private:
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const Maps>> base_;
    std::map<std::string, std::shared_ptr<const Maps>> tiled_;
};

// How many times `cls` repeats across a tile of `units`: a power of two, so tiles of sizes a factor 2 apart (EQ's
// 56- and 108-unit grass) show it at one scale.
std::uint32_t material_repeats(const MaterialClass& cls, double units);

// One tile's pack images (upright, RGBA8, `size` square): colour, and with `maps` the normal and roughness maps.
Outputs compose_tile(const MaterialTile& tile, const MaterialRules& rules, const assets::Image& original,
                     MaterialLibrary& library, std::uint32_t size, bool maps);

// A pack's record of the textures it made from materials (PACK/materials.txt: "<hash> <signature>" lines), so a
// build makes a tile again when its rules change, and gives a texture back to the upscaler when they drop it.
inline constexpr const char* kMaterialRecordFile = "materials.txt";
std::map<std::uint64_t, std::string> read_material_record(const std::filesystem::path& pack_dir);
bool write_material_record(const std::filesystem::path& pack_dir, const std::map<std::uint64_t, std::string>& record);

} // namespace hailstone::texpack
