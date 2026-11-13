#pragma once

// Generated trees (docs/graphics.md "Generated trees"): a classic zone's trees (a trunk of a few solid prisms and
// crossed cut-out cards) rebuilt as modern models, written to a texture pack as replacement models
// (assets/model_overrides.h). Each tree is measured from the game's: the new trunk follows the old one's solid prisms,
// which the collision keeps, at about their width; the branches and leaves fill the shape the old cards' leaves show
// (sampled through their texture's cut-out), in their colour. Pines grow whorls of needle sprays up a leader;
// broadleaf trees grow branches into that shape by space colonisation (Runions, Lane and Prusinkiewicz, "Modeling
// Trees with a Space Colonization Algorithm", 2007). The leaf and needle pictures are drawn here; the bark is a photo
// material (texpack/materials.h) when one is given.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "assets/model.h"
#include "assets/texture.h"
#include "texpack/materials.h"

namespace hailstone::texpack {

// What the game's tree is: measured by measure_tree.
struct TreeShape {
    // The trunk's skeleton from its solid prisms: each node a ring's centre and its corners' distance from it (0 at a
    // tip), parents before children, -1 for a root.
    struct Node {
        glm::vec3 centre{0.0f};
        float radius = 0.0f;
        int parent = -1;
    };
    std::vector<Node> trunk;
    // Points on the cards where their texture is opaque: the shape the leaves fill.
    std::vector<glm::vec3> foliage;
    glm::vec3 foliage_min{0.0f}, foliage_max{0.0f};
    // Mean colours of the leaves' opaque texels and of the bark texture (sRGB, 0..1).
    glm::vec3 leaf_colour{0.2f, 0.3f, 0.1f}, bark_colour{0.35f, 0.28f, 0.2f};
    // Colours picked from the leaves' opaque texels: the new leaves are drawn in them, so the art's mix (an autumn
    // tree's yellow among its green) stays.
    std::vector<glm::vec3> leaf_palette;
    bool conifer = false;
};

// A material's picture as its UVs read it (RGBA8, level 0, row 0 at v = 0; the cut-out in alpha), or null.
using TreeTextureLookup = std::function<const assets::Image*(const assets::Material&)>;

// nullopt and `why` when the mesh is not a tree this can rebuild (no solid trunk, no cut-out leaves).
std::optional<TreeShape> measure_tree(const assets::Mesh& mesh, const TreeTextureLookup& texture,
                                      std::string* why = nullptr);

// How the tree grows: Auto takes TreeShape::conifer (a pine or a broadleaf tree); a willow is not told from its shape
// and is asked for by name.
enum class TreeStyle : std::uint8_t { Auto, Pine, Broadleaf, Willow };
std::optional<TreeStyle> tree_style_from_name(std::string_view name); // "auto", "pine", "broadleaf", "willow"

struct TreeOptions {
    std::uint32_t seed = 1;
    TreeStyle style = TreeStyle::Auto;
    std::uint32_t leaf_size = 512; // the leaf picture's side
    std::uint32_t bark_size = 512;
    // The bark photo (MaterialLibrary maps, one repeat) and the world units one repeat covers; none: a drawn bark.
    std::shared_ptr<const MaterialLibrary::Maps> bark;
    double bark_units = 12.0;
    // The texture names the model's materials use (the files the pack keeps beside it).
    std::string bark_name = "bark", leaf_name = "leaf";
};

struct GeneratedTree {
    assets::Mesh mesh; // material 0 bark, 1 leaves; counter-clockwise from the front
    assets::Image leaf;                       // RGBA8, upright, cut-out in alpha
    assets::Image bark, bark_normal, bark_rough; // RGBA8, upright; the maps empty without a photo
};

GeneratedTree generate_tree(const TreeShape& shape, const TreeOptions& options);

// Writes a generated tree into `pack_dir`/models/<zone>/: <actor>.glb (made for `source_hash`, assets::object_mesh_hash
// of the game's mesh) and its textures as pack files (BC7 with mips; the bark's normal and roughness maps as .n, .r).
bool write_tree(const std::filesystem::path& pack_dir, const std::string& zone, const std::string& actor,
                const GeneratedTree& tree, std::uint64_t source_hash, std::string* error = nullptr);

// The texture names write_tree gives a tree's files (lower case, ".dds").
std::string tree_texture_name(const std::string& actor, const char* what);

struct TreeBuildOptions {
    std::uint32_t leaf_size = 512, bark_size = 512;
    std::function<bool()> cancelled;
    std::function<void(std::size_t done, std::size_t total)> progress; // trees
};
// Grows the trees `rules` name (MaterialRules::trees) from `game`'s zones into `pack_dir` (write_tree), each fitted to
// the game's tree of that name; their barks from `library` (none: drawn). How many were written; `notes` say which
// were left and why. Throws what loading a zone throws.
std::size_t build_trees(const MaterialRules& rules, const GameDir& game, const std::filesystem::path& pack_dir,
                        MaterialLibrary* library, const TreeBuildOptions& options,
                        std::vector<std::string>* notes = nullptr);

} // namespace hailstone::texpack
