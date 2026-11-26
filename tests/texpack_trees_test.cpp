// Generated trees (src/texpack/trees.h): a game tree measured (its trunk's skeleton, its leaves' shape and colour),
// a new one grown in its place, written to a pack and read back as a replacement model; the rules' tree lines.

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>

#include <doctest/doctest.h>

#include "assets/gltf_mesh.h"
#include "assets/zone.h"
#include "core/game_dir.h"
#include "game_files.h"
#include "scratch_dir.h"
#include "texpack/trees.h"

using namespace hailstone;
using namespace hailstone::texpack;
namespace fs = std::filesystem;

namespace {

constexpr float kPi = 3.14159265f;

assets::Vertex vtx(glm::vec3 p, glm::vec2 uv = {}) {
    assets::Vertex v;
    v.position = p;
    v.uv = uv;
    return v;
}

// A three-sided prism trunk through rings of (z, corner distance) round `foot` (a single point last: the tip), in
// `part` of `mesh`, wound as a WLD mesh is (either way: only the edges count).
void add_trunk(assets::Mesh& mesh, glm::vec3 foot, glm::vec3 lean, const std::vector<std::pair<float, float>>& rings) {
    std::vector<std::vector<std::uint32_t>> ids;
    for (const auto& [z, r] : rings) {
        ids.emplace_back();
        const int corners = r > 0.0f ? 3 : 1;
        for (int k = 0; k < corners; ++k) {
            const float a = 2.0f * kPi * float(k) / 3.0f;
            ids.back().push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
            mesh.vertices.push_back(vtx(foot + lean * z + glm::vec3(r * std::cos(a), r * std::sin(a), z)));
        }
    }
    assets::MeshPart part;
    part.first_index = static_cast<std::uint32_t>(mesh.indices.size());
    part.material = 0;
    for (std::size_t i = 1; i < ids.size(); ++i)
        for (int k = 0; k < 3; ++k) {
            const auto& lo = ids[i - 1];
            const auto& hi = ids[i];
            const std::uint32_t a = lo[std::size_t(k)], b = lo[std::size_t((k + 1) % 3)];
            const std::uint32_t c = hi[std::size_t(k) % hi.size()], d = hi[std::size_t((k + 1) % 3) % hi.size()];
            for (std::uint32_t x : {a, b, c}) mesh.indices.push_back(x);
            if (hi.size() > 1)
                for (std::uint32_t x : {b, d, c}) mesh.indices.push_back(x);
        }
    part.index_count = static_cast<std::uint32_t>(mesh.indices.size()) - part.first_index;
    mesh.parts.push_back(part);
}

// A game-style tree: a trunk to z 26 under two crossed cards from z 10 to 40, 30 across, the leaf texture's v rising
// with z.
assets::Mesh game_tree() {
    assets::Mesh mesh;
    assets::Material bark, leaf;
    bark.name = "BARK_MDF";
    bark.frames = {"bark.bmp"};
    leaf.name = "LEAF_MDF";
    leaf.frames = {"leaf.bmp"};
    leaf.blend = assets::BlendMode::Masked;
    mesh.materials = {bark, leaf};
    add_trunk(mesh, {0, 0, 0}, {0, 0, 0}, {{0.0f, 2.0f}, {10.0f, 1.5f}, {20.0f, 1.2f}, {26.0f, 0.0f}});
    assets::MeshPart cards;
    cards.first_index = static_cast<std::uint32_t>(mesh.indices.size());
    cards.material = 1;
    cards.solid = false;
    for (glm::vec3 across : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)}) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(vtx(-across * 15.0f + glm::vec3(0, 0, 10), {0, 0}));
        mesh.vertices.push_back(vtx(across * 15.0f + glm::vec3(0, 0, 10), {1, 0}));
        mesh.vertices.push_back(vtx(across * 15.0f + glm::vec3(0, 0, 40), {1, 1}));
        mesh.vertices.push_back(vtx(-across * 15.0f + glm::vec3(0, 0, 40), {0, 1}));
        for (std::uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(base + k);
    }
    cards.index_count = static_cast<std::uint32_t>(mesh.indices.size()) - cards.first_index;
    mesh.parts.push_back(cards);
    return mesh;
}

// Leaves of one colour in a cone (widest at the bottom, a pine's) or a disc (a broadleaf crown); bark of another.
assets::Image picture(bool cone, glm::vec3 colour) {
    assets::Image img;
    img.width = img.height = 64;
    img.mips.emplace_back(64 * 64 * 4);
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) {
            const float u = (float(x) + 0.5f) / 64.0f, v = (float(y) + 0.5f) / 64.0f;
            const bool in = cone ? std::abs(u - 0.5f) * 2.0f < 1.0f - v
                                 : (u - 0.5f) * (u - 0.5f) + (v - 0.6f) * (v - 0.6f) < 0.16f;
            std::byte* p = &img.mips[0][(std::size_t(y) * 64 + x) * 4];
            for (int c = 0; c < 3; ++c) p[c] = std::byte(static_cast<std::uint8_t>(colour[c] * 255.0f));
            p[3] = std::byte(in ? 255 : 0);
        }
    return img;
}

struct Textures {
    assets::Image leaf, bark;
    explicit Textures(bool cone) : leaf(picture(cone, {0.2f, 0.5f, 0.1f})), bark(picture(true, {0.4f, 0.3f, 0.2f})) {
        for (std::size_t i = 3; i < bark.mips[0].size(); i += 4) bark.mips[0][i] = std::byte{255};
    }
    TreeTextureLookup lookup() const {
        return [this](const assets::Material& m) { return m.blend == assets::BlendMode::Masked ? &leaf : &bark; };
    }
};

void write_text(const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; }

} // namespace

TEST_CASE("trees: a game tree's trunk, leaves and colours measured") {
    const assets::Mesh mesh = game_tree();
    const Textures pine(true), round(false);
    std::string why;
    auto shape = measure_tree(mesh, pine.lookup(), &why);
    REQUIRE_MESSAGE(shape, why);
    // The trunk's rings bottom to tip, each the one below's child, at the prism corners' distance.
    REQUIRE(shape->trunk.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) CHECK(shape->trunk[i].parent == int(i) - 1);
    CHECK(shape->trunk[0].radius == doctest::Approx(2.0f).epsilon(0.01));
    CHECK(shape->trunk[1].centre.z == doctest::Approx(10.0f));
    CHECK(shape->trunk[3].radius == 0.0f);
    // The leaves where the texture is opaque: a cone, widest low, on a trunk up into it.
    CHECK(shape->foliage.size() > 500);
    CHECK(shape->foliage_min.z >= 10.0f);
    CHECK(shape->foliage_max.z < 40.0f);
    CHECK(shape->foliage_max.x <= 15.0f);
    CHECK(shape->conifer);
    CHECK(shape->leaf_colour.g == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(shape->bark_colour.r == doctest::Approx(0.4f).epsilon(0.02));
    CHECK_FALSE(shape->leaf_palette.empty());
    // A round crown is a broadleaf tree's.
    auto broad = measure_tree(mesh, round.lookup());
    REQUIRE(broad);
    CHECK_FALSE(broad->conifer);

    // Not trees: no solid trunk, no leaves, no texture to see the leaves through.
    assets::Mesh no_trunk = mesh;
    no_trunk.parts.erase(no_trunk.parts.begin());
    CHECK_FALSE(measure_tree(no_trunk, pine.lookup(), &why));
    CHECK(why == "no solid trunk");
    assets::Mesh no_leaves = mesh;
    no_leaves.parts.pop_back();
    CHECK_FALSE(measure_tree(no_leaves, pine.lookup(), &why));
    CHECK(why == "no cut-out leaves");
    CHECK_FALSE(measure_tree(mesh, [](const assets::Material&) { return nullptr; }, &why));
}

TEST_CASE("trees: NaN and infinite leaf UVs read a texel, not past the image") {
    assets::Mesh mesh = game_tree();
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const auto first_leaf = mesh.vertices.size() - 8;
    mesh.vertices[first_leaf].uv = {nan, 0.5f};
    mesh.vertices[first_leaf + 1].uv = {inf, -inf};
    const Textures pine(true);
    measure_tree(mesh, pine.lookup()); // whatever it makes of them, defined
    CHECK(true);
}

TEST_CASE("trees: a trunk or leaf point that is not a number or too far out is refused") {
    // generate_tree divides each trunk length into pieces: a point at 1e30 made millions, a NaN an undefined count.
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const Textures pine(true);
    const std::size_t first_leaf = game_tree().vertices.size() - 8;
    for (float bad : {nan, inf, -inf, 1e30f, 20001.0f, -25000.0f}) {
        CAPTURE(bad);
        for (std::size_t vertex : {std::size_t{4}, first_leaf + 2}) { // a trunk ring's corner, a leaf card's corner
            assets::Mesh mesh = game_tree();
            mesh.vertices[vertex].position.z = bad;
            std::string why;
            CHECK_FALSE(measure_tree(mesh, pine.lookup(), &why));
            CHECK(why == "a point is not a number or too far out");
        }
    }
    // Within the bound, a tall tree is measured.
    assets::Mesh tall = game_tree();
    tall.vertices[4].position.z = 19000.0f;
    CHECK(measure_tree(tall, pine.lookup()));
}

TEST_CASE("trees: a tall trunk's short bottom prism is its own ring, so the tree stands on its foot") {
    // As Greater Faydark's NEKPINE1: rings at -8, 15 and 215 under leaves from 60 to 260. A first ring taken as a share
    // of the trunk's length swallowed the second ring, and the bark began halfway up the first prism, in the air.
    assets::Mesh mesh = game_tree();
    mesh.indices.resize(mesh.parts[0].first_index);
    mesh.parts.clear();
    add_trunk(mesh, {0, 0, 0}, {0, 0, 0}, {{-8.0f, 17.0f}, {15.0f, 13.0f}, {215.0f, 2.0f}, {225.0f, 0.0f}});
    assets::MeshPart cards;
    cards.first_index = static_cast<std::uint32_t>(mesh.indices.size());
    cards.material = 1;
    cards.solid = false;
    for (glm::vec3 across : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)}) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(vtx(-across * 100.0f + glm::vec3(0, 0, 60), {0, 0}));
        mesh.vertices.push_back(vtx(across * 100.0f + glm::vec3(0, 0, 60), {1, 0}));
        mesh.vertices.push_back(vtx(across * 100.0f + glm::vec3(0, 0, 260), {1, 1}));
        mesh.vertices.push_back(vtx(-across * 100.0f + glm::vec3(0, 0, 260), {0, 1}));
        for (std::uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(base + k);
    }
    cards.index_count = static_cast<std::uint32_t>(mesh.indices.size()) - cards.first_index;
    mesh.parts.push_back(cards);
    const Textures pine(true);
    std::string why;
    auto shape = measure_tree(mesh, pine.lookup(), &why);
    REQUIRE_MESSAGE(shape, why);
    REQUIRE(shape->trunk.size() == 4);
    CHECK(shape->trunk[0].centre.z == doctest::Approx(-8.0f));
    CHECK(shape->trunk[1].centre.z == doctest::Approx(15.0f));
    TreeOptions opt;
    opt.style = TreeStyle::Pine;
    const GeneratedTree tree = generate_tree(*shape, opt);
    // Every level's bark reaches down to the prism's foot.
    for (const auto& part : tree.mesh.parts) {
        if (part.material != 0) continue;
        float low = 1e9f;
        for (std::uint32_t i = part.first_index; i < part.first_index + part.index_count; ++i)
            low = std::min(low, tree.mesh.vertices[tree.mesh.indices[i]].position.z);
        CHECK(low <= -7.0f);
    }
}

TEST_CASE("trees: a limb that is a prism of its own hangs from the ring nearest its root, and is no pine") {
    assets::Mesh mesh = game_tree();
    // A leaning limb from beside the second ring, its rings at right angles to it.
    const glm::vec3 lean = glm::normalize(glm::vec3(1, 0, 1));
    assets::Mesh limb;
    limb.materials = mesh.materials;
    add_trunk(limb, {0, 0, 0}, {0, 0, 0}, {{0.0f, 1.0f}, {8.0f, 0.8f}, {12.0f, 0.0f}});
    // Turned to lean, moved beside the trunk at z 11: the rings tilt with it.
    const glm::mat3 turn = glm::mat3(glm::vec3(lean.z, 0, -lean.x), glm::vec3(0, 1, 0), lean);
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    for (auto v : limb.vertices) mesh.vertices.push_back(vtx(turn * v.position + glm::vec3(1.5f, 0, 11)));
    assets::MeshPart part = limb.parts[0];
    part.first_index = static_cast<std::uint32_t>(mesh.indices.size());
    for (std::uint32_t i : limb.indices) mesh.indices.push_back(base + i);
    mesh.parts.insert(mesh.parts.begin() + 1, part);

    const Textures pine(true);
    auto shape = measure_tree(mesh, pine.lookup());
    REQUIRE(shape);
    REQUIRE(shape->trunk.size() == 7);
    // Its first ring is all three tilted corners (not the lowest two), hung from the trunk's second ring.
    CHECK(shape->trunk[4].radius == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(shape->trunk[4].parent == 1);
    CHECK(shape->trunk[5].parent == 4);
    CHECK(shape->trunk[6].parent == 5);
    CHECK_FALSE(shape->conifer); // a fork
}

TEST_CASE("trees: a grown tree stands on the game trunk, inside its leaves, the same each time") {
    const assets::Mesh mesh = game_tree();
    for (bool cone : {true, false}) {
        const Textures tex(cone);
        auto shape = measure_tree(mesh, tex.lookup());
        REQUIRE(shape);
        for (TreeStyle style : {TreeStyle::Auto, TreeStyle::Pine, TreeStyle::Broadleaf, TreeStyle::Willow,
                                TreeStyle::Dead, TreeStyle::Palm}) {
            CAPTURE(cone);
            CAPTURE(int(style));
            TreeOptions opt;
            opt.seed = 7;
            opt.style = style;
            opt.leaf_size = opt.bark_size = 64;
            opt.bark_name = "b.dds";
            opt.leaf_name = "l.dds";
            opt.far_name = "f.dds";
            opt.far_width = 64;
            opt.far_height = 64;
            const GeneratedTree a = generate_tree(*shape, opt);
            const GeneratedTree b = generate_tree(*shape, opt);
            REQUIRE(a.mesh.vertices.size() == b.mesh.vertices.size());
            CHECK(a.mesh.indices == b.mesh.indices);
            for (std::size_t i = 0; i < a.mesh.vertices.size(); i += 37) CHECK(a.mesh.vertices[i].position == b.mesh.vertices[i].position);
            CHECK(a.leaf.mips[0] == b.leaf.mips[0]);
            // Three levels: near bark and leaves, the middle's, and the far picture's cards, their bands end to end
            // (200 and 600 units for a tree this small) and the last reaching everywhere past the middle.
            REQUIRE(a.mesh.parts.size() == 5);
            const float bands[][2] = {{0, 200}, {0, 200}, {200, 600}, {200, 600}, {600, 1.0e6f}};
            const std::uint32_t materials[] = {0, 1, 0, 1, 2};
            std::size_t tris[3] = {0, 0, 0};
            for (std::size_t i = 0; i < 5; ++i) {
                CAPTURE(i);
                CHECK(a.mesh.parts[i].material == materials[i]);
                CHECK(a.mesh.parts[i].lod_near == bands[i][0]);
                CHECK(a.mesh.parts[i].lod_far == bands[i][1]);
                tris[i < 2 ? 0 : i < 4 ? 1 : 2] += a.mesh.parts[i].index_count / 3;
            }
            CHECK(a.mesh.materials[0].frames[0] == "b.dds");
            CHECK(a.mesh.materials[1].frames[0] == "l.dds");
            CHECK(a.mesh.materials[2].frames[0] == "f.dds");
            for (std::size_t m : {1u, 2u}) {
                CHECK(a.mesh.materials[m].blend == assets::BlendMode::Masked);
                CHECK(a.mesh.materials[m].two_sided);
            }
            CHECK(a.mesh.front_ccw);
            // Sway heights: the trunk's own, the leaves' their branch's, so each branch's cards move as one piece.
            CHECK(a.mesh.sway_heights);
            std::set<float> leaf_heights;
            std::size_t leaf_vertices = 0;
            for (std::uint32_t i = a.mesh.parts[1].first_index; i < a.mesh.parts[1].first_index + a.mesh.parts[1].index_count; ++i) {
                leaf_heights.insert(a.mesh.vertices[a.mesh.indices[i]].uv2.x);
                ++leaf_vertices;
            }
            CHECK(leaf_heights.size() * 8 < leaf_vertices);
            bool trunk_own = false;
            for (std::uint32_t i = a.mesh.parts[0].first_index; i < a.mesh.parts[0].first_index + a.mesh.parts[0].index_count; ++i)
                trunk_own |= a.mesh.vertices[a.mesh.indices[i]].uv2.x == a.mesh.vertices[a.mesh.indices[i]].position.z &&
                             a.mesh.vertices[a.mesh.indices[i]].position.z > 5.0f;
            CHECK(trunk_own);
            // A budget a zone of a few hundred trees can draw; the middle level under the near one (the same cards,
            // straight, and thinner bark); the far eight triangles (three crossed cards and a level one).
            CHECK(tris[0] < 20000);
            CHECK(tris[1] * 10 < tris[0] * 8);
            CHECK(tris[2] == 8);
            // The far picture: the tree's shape cut out, its leaves and bark in it.
            REQUIRE(a.far.width == 64);
            REQUIRE(a.far.height == 64);
            int opaque = 0;
            for (std::size_t i = 3; i < a.far.mips[0].size(); i += 4) opaque += std::to_integer<int>(a.far.mips[0][i]) == 255;
            CHECK(opaque > 64 * 64 / 10);
            CHECK(opaque < 64 * 64 * 9 / 10);
            CHECK(a.far.mips[0] == b.far.mips[0]);
            // The bark at the foot is round the game's prism: past its faces (half the corner distance), inside its
            // corners, where a player stopped by it stands.
            const auto& bark = a.mesh.parts[0];
            int at_foot = 0;
            for (std::uint32_t k = bark.first_index; k < bark.first_index + bark.index_count; ++k) {
                const glm::vec3 p = a.mesh.vertices[a.mesh.indices[k]].position;
                if (std::abs(p.z) > 0.01f) continue;
                ++at_foot;
                const float r = glm::length(glm::vec2(p));
                CHECK(r > 1.0f);
                CHECK(r < 2.0f);
            }
            CHECK(at_foot > 0);
            // The leaves within the game leaves' reach (a little past it: cards are drawn round their points).
            const auto& leaves = a.mesh.parts[1];
            for (std::uint32_t k = leaves.first_index; k < leaves.first_index + leaves.index_count; k += 3) {
                const glm::vec3 p = a.mesh.vertices[a.mesh.indices[k]].position;
                CHECK(glm::length(glm::vec2(p)) < 15.0f * 1.35f);
                CHECK(p.z > 10.0f - 8.0f);
                CHECK(p.z < 40.0f + 8.0f);
            }
            // Its leaves average the game's colour where they are opaque.
            glm::dvec3 sum(0.0);
            double n = 0;
            for (std::size_t i = 0; i < a.leaf.mips[0].size(); i += 4)
                if (std::to_integer<int>(a.leaf.mips[0][i + 3]) == 255) {
                    for (int c = 0; c < 3; ++c) sum[c] += std::to_integer<int>(a.leaf.mips[0][i + std::size_t(c)]) / 255.0;
                    n += 1;
                }
            REQUIRE(n > 0);
            CHECK(sum.g / n == doctest::Approx(0.5).epsilon(0.05));
            CHECK(a.bark.width == 64);
            CHECK(a.bark_normal.width == 0); // no photo: a drawn bark, no maps
        }
    }
    CHECK(tree_style_from_name("willow") == TreeStyle::Willow);
    CHECK(tree_style_from_name("dead") == TreeStyle::Dead);
    CHECK(tree_style_from_name("palm") == TreeStyle::Palm);
    CHECK_FALSE(tree_style_from_name("oak"));
}

TEST_CASE("trees: a pine's needles keep close in shade when the game's leaves are stark") {
    // The game's leaves in stripes of a dark and a near-white green: the palette the needles pick from spans both.
    Textures tex(true);
    for (std::size_t i = 0; i < tex.leaf.mips[0].size(); i += 4) {
        const bool light = (i / 4) % 2 == 0;
        const glm::vec3 c = light ? glm::vec3(0.55f, 0.85f, 0.45f) : glm::vec3(0.1f, 0.25f, 0.05f);
        for (int k = 0; k < 3; ++k) tex.leaf.mips[0][i + std::size_t(k)] = std::byte(static_cast<std::uint8_t>(c[k] * 255.0f));
    }
    auto shape = measure_tree(game_tree(), tex.lookup());
    REQUIRE(shape);
    TreeOptions opt;
    opt.seed = 7;
    opt.style = TreeStyle::Pine;
    opt.leaf_size = opt.bark_size = 128;
    opt.bark_name = "b.dds";
    opt.leaf_name = "l.dds";
    opt.far_name = "f.dds";
    opt.far_width = opt.far_height = 64;
    const GeneratedTree tree = generate_tree(*shape, opt);
    // The spread of the opaque texels' green about their mean: stark needles turned over every pixel they cover as
    // the tree swayed or the camera moved (a flicker standing still, a shimmer walking through a forest).
    double sum = 0.0, sq = 0.0, n = 0.0;
    const auto& px = tree.leaf.mips[0];
    for (std::size_t i = 0; i < px.size(); i += 4)
        if (std::to_integer<int>(px[i + 3]) == 255) {
            const double g = std::to_integer<int>(px[i + 1]) / 255.0;
            sum += g, sq += g * g, n += 1.0;
        }
    REQUIRE(n > 0.0);
    const double mean = sum / n, spread = std::sqrt(std::max(0.0, sq / n - mean * mean)) / mean;
    CHECK(spread < 0.35); // 0.55 with each needle the palette's own shade
}

TEST_CASE("trees: a pine's needle cards lie along its branches, none standing upright below its top") {
    const Textures tex(true);
    auto shape = measure_tree(game_tree(), tex.lookup());
    REQUIRE(shape);
    TreeOptions opt;
    opt.seed = 7;
    opt.style = TreeStyle::Pine;
    opt.leaf_size = opt.bark_size = 64;
    opt.bark_name = "b.dds";
    opt.leaf_name = "l.dds";
    opt.far_name = "f.dds";
    opt.far_width = opt.far_height = 64;
    const GeneratedTree tree = generate_tree(*shape, opt);
    const auto& m = tree.mesh;
    const auto& leaves = m.parts[1];
    float top = 0.0f;
    for (const auto& v : m.vertices) top = std::max(top, v.position.z);
    // An upright card (the clump once at each branch's end) showed as a flat slab from beside the tree; the sprays
    // along a branch tilt at most about 50 degrees from level. The crown's sprays round the leader stand upright.
    int cards = 0;
    for (std::uint32_t k = leaves.first_index; k + 2 < leaves.first_index + leaves.index_count; k += 3) {
        const glm::vec3 a = m.vertices[m.indices[k]].position, b = m.vertices[m.indices[k + 1]].position,
                        c = m.vertices[m.indices[k + 2]].position;
        if (std::max({a.z, b.z, c.z}) > top * 0.8f) continue;
        const glm::vec3 n = glm::cross(b - a, c - a);
        if (glm::length(n) < 1e-6f) continue;
        ++cards;
        CHECK(std::abs(glm::normalize(n).z) > 0.4f);
    }
    CHECK(cards > 0);
}

TEST_CASE("trees: a written tree is a pack's replacement model for that game mesh") {
    test::ScratchDir dir("trees_write");
    const assets::Mesh mesh = game_tree();
    const Textures tex(true);
    auto shape = measure_tree(mesh, tex.lookup());
    REQUIRE(shape);
    TreeOptions opt;
    opt.leaf_size = opt.bark_size = 64;
    opt.bark_name = tree_texture_name("TREE1_ACTORDEF", "bark");
    opt.leaf_name = tree_texture_name("TREE1_ACTORDEF", "leaf");
    opt.far_name = tree_texture_name("TREE1_ACTORDEF", "far");
    CHECK(opt.leaf_name == "hstree_tree1_leaf.dds");
    const GeneratedTree tree = generate_tree(*shape, opt);
    std::string error;
    REQUIRE_MESSAGE(write_tree(dir.path(), "QeyToQrg", "TREE1_ACTORDEF", tree, assets::object_mesh_hash(mesh), &error), error);
    const fs::path models = dir.path() / "models" / "qeytoqrg";
    CHECK(fs::exists(models / "hstree_tree1_leaf.dds"));
    CHECK(fs::exists(models / "hstree_tree1_bark.dds"));
    CHECK(fs::exists(models / "hstree_tree1_far.dds"));
    CHECK_FALSE(fs::exists(models / "hstree_tree1_bark.n.dds")); // a drawn bark has no maps
    auto bytes = read_file(models / "tree1.glb");
    REQUIRE(bytes);
    auto model = assets::read_glb(*bytes, &error);
    REQUIRE_MESSAGE(model, error);
    CHECK(model->source_hash == assets::object_mesh_hash(mesh));
    CHECK(model->mesh.materials[1].frames[0] == "hstree_tree1_leaf.dds");
    CHECK(model->mesh.vertices.size() == tree.mesh.vertices.size()); // the levels share one vertex list
    REQUIRE(model->mesh.parts.size() == tree.mesh.parts.size());
    for (std::size_t i = 0; i < tree.mesh.parts.size(); ++i) {
        CHECK(model->mesh.parts[i].lod_near == doctest::Approx(tree.mesh.parts[i].lod_near));
        CHECK(model->mesh.parts[i].lod_far == doctest::Approx(tree.mesh.parts[i].lod_far));
    }
    auto leaf = assets::decode_texture(*read_file(models / "hstree_tree1_leaf.dds"));
    REQUIRE(leaf);
    CHECK(leaf->mips.size() == 7); // 64 down to 1
}

TEST_CASE("trees: the rules' tree lines") {
    const std::string materials = "[bark]\nasset = bark_brown_02\nmetres = 1\n";
    std::string error;
    auto rules = parse_material_rules(materials, {{"qeytoqrg", "tree TREE71_ACTORDEF = willow bark\ntree bpine1 = auto # pines\n"}}, &error);
    REQUIRE_MESSAGE(rules, error);
    REQUIRE(rules->trees.size() == 2);
    CHECK(rules->trees[0].zone == "qeytoqrg");
    CHECK(rules->trees[0].actor == "tree71");
    CHECK(rules->trees[0].style == "willow");
    CHECK(rules->trees[0].bark == "bark");
    CHECK(rules->trees[1].actor == "bpine1");
    CHECK(rules->trees[1].bark.empty());
    CHECK(rules->tiles.empty());
    CHECK_FALSE(parse_material_rules(materials, {{"z", "tree x = oak"}}, &error));
    CHECK(error == "z.txt:1: expected tree <actor> = auto|pine|broadleaf|willow|dead|palm [bark material]");
    CHECK_FALSE(parse_material_rules(materials, {{"z", "\ntree x = pine birch"}}, &error));
    CHECK(error == "z.txt:2: no material [birch] in materials.txt");
    // The actor names the tree's files: a path is refused.
    for (const char* line : {"tree ../../evil = pine", "tree a/b = pine", "tree c:\\x = pine", "tree .. = pine"}) {
        CHECK_FALSE(parse_material_rules(materials, {{"z", line}}, &error));
        CHECK(error == "z.txt:1: a tree's actor is a name of letters, digits, '_' and '-'");
    }
}

TEST_CASE("trees: a rules file or a tree whose zone is not a zone name is refused") {
    // A rules file named "...txt" is zone "..": its trees would go to models/../, the pack folder itself.
    const std::string materials = "[bark]\nasset = bark_brown_02\nmetres = 1\n";
    std::string error;
    for (const char* zone : {"..", ".", "", "a/b", "a\\b", "c:x", "con", "zone name"}) {
        CAPTURE(zone);
        CHECK_FALSE(parse_material_rules(materials, {{zone, "tree tree1 = pine\n"}}, &error));
        CHECK(error.find("not a zone's rules") != std::string::npos);
    }
    CHECK(parse_material_rules(materials, {{"QeyToQrg_2", "tree tree1 = pine\n"}}, &error));

    test::ScratchDir dir("trees_zone_name");
    write_text(dir.path() / "materials.txt", materials);
    write_text(dir.path() / "...txt", "tree tree1 = pine\n");
    CHECK_FALSE(load_material_rules(dir.path(), &error));
    CHECK(error == "...txt: not a zone's rules (<zone>.txt, the zone's short name of letters, digits and '_')");

    const assets::Mesh mesh = game_tree();
    const Textures tex(true);
    auto shape = measure_tree(mesh, tex.lookup());
    REQUIRE(shape);
    TreeOptions opt;
    opt.leaf_size = opt.bark_size = 64;
    const GeneratedTree tree = generate_tree(*shape, opt);
    const fs::path pack = dir.path() / "pack";
    for (const char* zone : {"..", "../..", "a/b"}) {
        CAPTURE(zone);
        CHECK_FALSE(write_tree(pack, zone, "TREE1_ACTORDEF", tree, 1, &error));
        CHECK(error == std::string("not a zone name: ") + zone);
    }
    CHECK_FALSE(fs::exists(pack));
}

TEST_CASE("trees: Qeynos Hills' trees measured from the game files") {
    const GameDir* game = test::game_dir();
    if (!game || !fs::exists(game->root() / "qeytoqrg.s3d")) return;
    const assets::Zone zone = assets::load_classic_zone(*game, "qeytoqrg");
    std::map<std::string, assets::Image> images;
    auto lookup = [&](const assets::Material& m) -> const assets::Image* {
        const bool masked = m.blend == assets::BlendMode::Masked;
        auto [it, added] = images.try_emplace(m.frames[0] + (masked ? "#m" : ""));
        if (added) {
            auto img = assets::decode_texture(*zone.read_file(m.frames[0]), {.palette_index0_transparent = masked, .wld_bmp_rows = true});
            REQUIRE(img);
            it->second = assets::to_rgba8(*img);
        }
        return &it->second;
    };
    auto tips = [](const TreeShape& s) {
        std::vector<int> children(s.trunk.size(), 0);
        for (const auto& n : s.trunk)
            if (n.parent >= 0) ++children[std::size_t(n.parent)];
        return std::count(children.begin(), children.end(), 0);
    };
    for (const char* name : {"TREE1_ACTORDEF", "BPINE1_ACTORDEF", "TREETRY_ACTORDEF", "TREE71_ACTORDEF"}) {
        CAPTURE(name);
        auto shape = measure_tree(zone.objects.at(name), lookup);
        REQUIRE(shape);
        const std::string n = name;
        CHECK(shape->conifer == (n == "TREE1_ACTORDEF" || n == "BPINE1_ACTORDEF"));
        CHECK(tips(*shape) == (n == "TREETRY_ACTORDEF" ? 2 : 1)); // TREETRY's second limb is a prism of its own
    }
}

TEST_CASE("trees: a leaf card is cut to its texture's outline") {
    // A disc of leaves in the middle of a 64 x 64 card.
    assets::Image leaf;
    leaf.width = leaf.height = 64;
    leaf.mips.emplace_back(std::size_t(64) * 64 * 4);
    std::size_t opaque = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            if (glm::distance(glm::vec2(float(x) + 0.5f, float(y) + 0.5f), glm::vec2(32.0f, 40.0f)) < 14.0f) {
                leaf.mips[0][(std::size_t(y) * 64 + std::size_t(x)) * 4 + 3] = std::byte{255};
                ++opaque;
            }
    const auto outline = card_outline(leaf, 6, 2.0f);
    REQUIRE(outline.size() >= 3);
    CHECK(outline.size() <= 6);
    float area = 0.0f;
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const glm::vec2 p = outline[i], q = outline[(i + 1) % outline.size()];
        area += p.x * q.y - q.x * p.y;
        CHECK(p.x >= 0.0f);
        CHECK(p.y <= 1.0f);
    }
    area = std::abs(area) * 0.5f;
    CHECK(area < 0.4f); // the disc is about 0.15 of the card
    CHECK(area > float(opaque) / (64.0f * 64.0f));
    // Every opaque texel's centre inside it.
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            if (leaf.mips[0][(std::size_t(y) * 64 + std::size_t(x)) * 4 + 3] == std::byte{0}) continue;
            const glm::vec2 c((float(x) + 0.5f) / 64.0f, (float(y) + 0.5f) / 64.0f);
            float sign = 0.0f;
            bool inside = true;
            for (std::size_t i = 0; i < outline.size(); ++i) {
                const glm::vec2 p = outline[i], q = outline[(i + 1) % outline.size()];
                const float side = (q.x - p.x) * (c.y - p.y) - (q.y - p.y) * (c.x - p.x);
                if (sign == 0.0f) sign = side;
                inside &= side * sign >= 0.0f;
            }
            CHECK(inside);
        }
    // Nothing opaque: no outline.
    assets::Image empty;
    empty.width = empty.height = 8;
    empty.mips.emplace_back(std::size_t(8) * 8 * 4);
    CHECK(card_outline(empty).empty());
}
