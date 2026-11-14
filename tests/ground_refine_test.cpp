// The smoothed ground (assets/ground_refine.h): smooth normals, curved and cut-up ground triangles, no cracks.
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "assets/ground_refine.h"
#include "assets/zone.h"
#include "game_files.h"

using namespace hailstone;
using namespace hailstone::assets;

namespace {

Material material(const std::string& name, BlendMode blend = BlendMode::Opaque) {
    Material m;
    m.name = name;
    m.blend = blend;
    m.frames = {name + ".bmp"};
    return m;
}

// A grid of n x n squares `cell` units wide, its heights from `height`, as WLD terrain: clockwise from above, the
// normals up. Squares with x index below `split` go in `first` (when given), the rest in `second`.
template <class Height>
void heightfield(Mesh& first, Mesh& second, int n, float cell, Height height, int split) {
    auto add = [&](Mesh& m, glm::vec3 p) {
        Vertex v;
        v.position = p;
        v.normal = {0, 0, 1};
        v.uv = {p.x / cell, p.y / cell};
        v.color = 0xff808080u;
        m.vertices.push_back(v);
        return static_cast<std::uint32_t>(m.vertices.size() - 1);
    };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            Mesh& m = x < split ? first : second;
            auto p = [&](int i, int j) {
                const float fx = static_cast<float>(i) * cell, fy = static_cast<float>(j) * cell;
                return glm::vec3(fx, fy, height(fx, fy));
            };
            const std::uint32_t a = add(m, p(x, y)), b = add(m, p(x + 1, y)), c = add(m, p(x + 1, y + 1)),
                                d = add(m, p(x, y + 1));
            // Clockwise seen from above (+z).
            m.indices.insert(m.indices.end(), {a, c, b, a, d, c});
        }
    for (Mesh* m : {&first, &second}) {
        if (m->indices.empty()) continue;
        MeshPart part;
        part.index_count = static_cast<std::uint32_t>(m->indices.size());
        m->parts = {part};
        if (m->materials.empty()) m->materials = {material("grass")};
    }
}

float hill(float x, float y) {
    const float dx = x - 80.0f, dy = y - 80.0f;
    return 60.0f * std::exp(-(dx * dx + dy * dy) / (2.0f * 45.0f * 45.0f));
}

Zone hill_zone(int split = 99) {
    Zone z;
    Mesh a, b;
    b.materials = {material("dirt")};
    heightfield(a, b, 8, 20.0f, hill, split);
    z.terrain.push_back(a);
    if (!b.indices.empty()) z.terrain.push_back(b);
    return z;
}

std::tuple<std::uint32_t, std::uint32_t, std::uint32_t> bits(const glm::vec3& p) {
    std::uint32_t x, y, zz;
    std::memcpy(&x, &p.x, 4);
    std::memcpy(&y, &p.y, 4);
    std::memcpy(&zz, &p.z, 4);
    return {x, y, zz};
}

// How many drawn triangles use each edge, by its corners' exact positions: a crack shows as an edge used once
// inside the ground.
using EdgeCount = std::map<std::pair<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>,
                                     std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>,
                           int>;
EdgeCount drawn_edges(const Zone& z) {
    EdgeCount out;
    for (const Mesh& m : z.draw_terrain)
        for (std::size_t i = 0; i + 3 <= m.indices.size(); i += 3)
            for (int k = 0; k < 3; ++k) {
                auto a = bits(m.vertices[m.indices[i + static_cast<std::size_t>(k)]].position);
                auto b = bits(m.vertices[m.indices[i + static_cast<std::size_t>((k + 1) % 3)]].position);
                if (b < a) std::swap(a, b);
                ++out[{a, b}];
            }
    return out;
}

bool on_border(const std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>& t, float lo, float hi) {
    float x, y;
    const std::uint32_t bx = std::get<0>(t), by = std::get<1>(t);
    std::memcpy(&x, &bx, 4);
    std::memcpy(&y, &by, 4);
    return x == lo || x == hi || y == lo || y == hi;
}

void check_closed(const Zone& z, float lo, float hi) {
    int inner_once = 0, border_once = 0;
    for (const auto& [edge, count] : drawn_edges(z)) {
        CHECK(count <= 2);
        if (count != 1) continue;
        if (on_border(edge.first, lo, hi) && on_border(edge.second, lo, hi))
            ++border_once;
        else
            ++inner_once;
    }
    CHECK(inner_once == 0);
    CHECK(border_once > 0);
}

// The lit side of a drawn WLD triangle (clockwise from the front).
glm::vec3 front(const Mesh& m, std::size_t i) {
    const glm::vec3 a = m.vertices[m.indices[i]].position, b = m.vertices[m.indices[i + 1]].position,
                    c = m.vertices[m.indices[i + 2]].position;
    return -glm::cross(b - a, c - a);
}

} // namespace

TEST_CASE("flat ground keeps its triangles and gets normals straight up") {
    Zone z;
    Mesh a, b;
    heightfield(a, b, 4, 20.0f, [](float, float) { return 5.0f; }, 99);
    for (auto& v : a.vertices) v.normal = glm::normalize(glm::vec3(0.3f, 0.0f, 1.0f)); // the file's, tilted
    z.terrain.push_back(a);
    const auto s = refine_ground(z);
    CHECK(s.ground_triangles == 32);
    CHECK(s.triangles == 32);
    CHECK(s.curved_edges == 0);
    CHECK(s.max_offset == 0.0f);
    REQUIRE(z.draw_terrain.size() == 1);
    CHECK(z.draw_terrain[0].indices.size() == a.indices.size());
    for (std::size_t i = 0; i < z.draw_terrain[0].indices.size(); ++i) {
        const auto& v = z.draw_terrain[0].vertices[z.draw_terrain[0].indices[i]];
        CHECK(v.normal.z == doctest::Approx(1.0f));
        CHECK(v.position.z == 5.0f);
    }
}

TEST_CASE("a hill is cut up and curved, within the offset, without cracks") {
    Zone z = hill_zone();
    GroundRefineOptions o;
    o.max_offset = 1.0f;
    const auto s = refine_ground(z, o);
    CHECK(s.ground_triangles == 128);
    CHECK(s.curved_edges > 100);
    CHECK(s.triangles > 4 * s.ground_triangles);
    CHECK(s.max_offset > 0.1f);
    CHECK(s.max_offset <= 1.0f + 1e-3f);
    check_closed(z, 0.0f, 160.0f);
    // Every drawn triangle faces the way its terrain triangle does, and maps back to it.
    const Mesh& in = z.terrain[0];
    const Mesh& out = z.draw_terrain[0];
    REQUIRE(z.draw_terrain_source[0].size() == out.indices.size() / 3);
    for (std::size_t t = 0; t < z.draw_terrain_source[0].size(); ++t) {
        const std::uint32_t src = z.draw_terrain_source[0][t];
        REQUIRE(src < in.indices.size() / 3);
        CHECK(glm::dot(front(out, t * 3), front(in, src * 3)) > 0.0f);
    }
    // Smaller offsets hold too.
    o.max_offset = 0.25f;
    CHECK(refine_ground(z, o).max_offset <= 0.25f + 1e-3f);
}

TEST_CASE("the smoothed ground meets across meshes and materials without cracks") {
    Zone z = hill_zone(3); // the squares left of x = 60 in one mesh, the rest in another
    REQUIRE(z.terrain.size() == 2);
    const auto s = refine_ground(z);
    CHECK(s.ground_triangles == 128);
    check_closed(z, 0.0f, 160.0f);
}

TEST_CASE("a crease stays sharp and straight") {
    // Two flat planes meeting at a right angle: floor and wall.
    Zone z;
    Mesh m;
    m.materials = {material("stone")};
    auto add = [&](glm::vec3 p, glm::vec3 n) {
        Vertex v;
        v.position = p;
        v.normal = n;
        m.vertices.push_back(v);
        return static_cast<std::uint32_t>(m.vertices.size() - 1);
    };
    const glm::vec3 up(0, 0, 1), side(-1, 0, 0);
    // The floor (x <= 0), clockwise from above; the wall (x = 0, rising), clockwise seen from -x.
    std::uint32_t f0 = add({-20, 0, 0}, up), f1 = add({0, 0, 0}, up), f2 = add({0, 20, 0}, up), f3 = add({-20, 20, 0}, up);
    std::uint32_t w0 = add({0, 0, 0}, side), w1 = add({0, 0, 20}, side), w2 = add({0, 20, 20}, side), w3 = add({0, 20, 0}, side);
    m.indices = {f0, f2, f1, f0, f3, f2, w0, w2, w1, w0, w3, w2};
    MeshPart part;
    part.index_count = 12;
    m.parts = {part};
    z.terrain.push_back(m);
    const auto s = refine_ground(z);
    CHECK(s.curved_edges == 0);
    CHECK(s.triangles == 4);
    CHECK(s.max_offset == 0.0f);
    for (std::size_t i = 0; i < 6; ++i)
        CHECK(z.draw_terrain[0].vertices[z.draw_terrain[0].indices[i]].normal.z == doctest::Approx(1.0f));
    for (std::size_t i = 6; i < 12; ++i)
        CHECK(z.draw_terrain[0].vertices[z.draw_terrain[0].indices[i]].normal.x == doctest::Approx(-1.0f));
}

TEST_CASE("only solid opaque classic ground is smoothed; its edge with the rest stays as it was") {
    // The hill, its middle squares a cut-out and a passable surface: those are drawn as loaded, and the ground
    // round them keeps its edges with them straight and uncut.
    Zone z = hill_zone();
    Mesh& m = z.terrain[0];
    m.materials.push_back(material("fence", BlendMode::Masked));
    const std::uint32_t whole = m.parts[0].index_count;
    // Squares 27..28 (row 3, x 3..4): triangles 54..57.
    MeshPart before = m.parts[0], cut = m.parts[0], water = m.parts[0], after = m.parts[0];
    before.index_count = 54 * 3;
    cut.first_index = 54 * 3;
    cut.index_count = 6;
    cut.material = 1;
    water.first_index = 56 * 3;
    water.index_count = 6;
    water.solid = false;
    after.first_index = 58 * 3;
    after.index_count = whole - after.first_index;
    m.parts = {before, cut, water, after};
    const auto s = refine_ground(z);
    CHECK(s.ground_triangles == 124);
    const Mesh& out = z.draw_terrain[0];
    REQUIRE(out.parts.size() == 4);
    CHECK(out.parts[1].index_count == 6);
    CHECK(out.parts[2].index_count == 6);
    CHECK_FALSE(out.parts[2].solid);
    for (std::uint32_t i = 0; i < 6; ++i) {
        CHECK(out.indices[out.parts[1].first_index + i] == m.indices[cut.first_index + i]);
        CHECK(out.indices[out.parts[2].first_index + i] == m.indices[water.first_index + i]);
    }
    const auto& source = z.draw_terrain_source[0];
    CHECK(source[out.parts[1].first_index / 3] == 54);
    CHECK(source[out.parts[2].first_index / 3 + 1] == 57);
    check_closed(z, 0.0f, 160.0f);
}

TEST_CASE("EQG meshes and zones without ground are left alone") {
    Zone z = hill_zone();
    z.terrain[0].front_ccw = true;
    const auto s = refine_ground(z);
    CHECK(s.ground_triangles == 0);
    CHECK(z.draw_terrain.empty());
    CHECK(z.draw_terrain_source.empty());
}

TEST_CASE("qeytoqrg's smoothed ground has no cracks" * doctest::skip(!hailstone::test::game_dir())) {
    Zone z = load_classic_zone(*hailstone::test::game_dir(), "qeytoqrg");
    const auto s = refine_ground(z);
    MESSAGE("qeytoqrg: ", s.ground_triangles, " ground triangles drawn as ", s.triangles, ", ", s.curved_edges,
            " curved edges, at most ", s.max_offset, " units off");
    CHECK(s.ground_triangles > 1000);
    CHECK(s.triangles > s.ground_triangles);
    CHECK(s.max_offset <= 1.0f + 1e-3f);
    // Every edge the refinement made inside a triangle is shared; the cracks would be curved edges used once.
    std::size_t once = 0, total = 0;
    for (const auto& [edge, count] : drawn_edges(z)) {
        ++total;
        if (count == 1) ++once;
    }
    // The terrain's own open edges (its border, seams with cut-outs and walls) stay, as in the file.
    Zone flat = load_classic_zone(*hailstone::test::game_dir(), "qeytoqrg");
    flat.draw_terrain = flat.terrain;
    std::size_t flat_once = 0;
    for (const auto& [edge, count] : drawn_edges(flat))
        if (count == 1) ++flat_once;
    MESSAGE("edges used once: ", once, " of ", total, " (", flat_once, " in the file)");
    CHECK(once <= flat_once);
}
