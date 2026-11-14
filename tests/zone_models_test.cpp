// Replacement models in the zone renderer's build (render/zone_renderer.h): each mesh once, drawn instanced at its
// placements, its levels of detail kept apart.

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

#include "assets/zone.h"
#include "render/zone_renderer.h"

using namespace hailstone;

namespace {

// A quad of two triangles in `parts` levels of detail, each its own copy of the quad.
assets::Mesh quad_model(int levels) {
    assets::Mesh m;
    assets::Material mat;
    mat.name = "bark";
    mat.frames = {"bark.dds"};
    m.materials.push_back(mat);
    for (int l = 0; l < levels; ++l) {
        const auto base = static_cast<std::uint32_t>(m.vertices.size());
        for (glm::vec3 p : {glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(1, 0, 5), glm::vec3(0, 0, 5)}) {
            assets::Vertex v;
            v.position = p;
            v.normal = {0, -1, 0};
            m.vertices.push_back(v);
        }
        assets::MeshPart part;
        part.first_index = static_cast<std::uint32_t>(m.indices.size());
        part.index_count = 6;
        part.lod_near = l == 0 ? 0.0f : 100.0f * static_cast<float>(l);
        part.lod_far = l + 1 == levels ? 1e6f : 100.0f * static_cast<float>(l + 1);
        if (levels == 1) part.lod_near = part.lod_far = 0.0f;
        m.parts.push_back(part);
        for (std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
    }
    return m;
}

assets::Zone zone_with(int placements, int levels) {
    assets::Zone zone;
    zone.objects["TREE_ACTORDEF"] = quad_model(1);
    zone.draw_objects["TREE_ACTORDEF"] = quad_model(levels);
    for (int i = 0; i < placements; ++i) {
        assets::Placement p;
        p.actor = "TREE_ACTORDEF";
        p.transform = glm::translate(glm::mat4(1.0f), glm::vec3(300.0f * static_cast<float>(i), 0.0f, 0.0f));
        p.vertex_colors.assign(4, 0xff808080u);
        zone.placements.push_back(p);
    }
    return zone;
}

} // namespace

TEST_CASE("zone models: a replacement mesh is stored once and drawn at each placement") {
    const auto prepared = render::ZoneRenderer::prepare(zone_with(5, 3), {}, 16);
    REQUIRE(prepared);
    const auto n = render::ZoneRenderer::prepared_counts(*prepared);
    CHECK(n.vertices == 12); // three levels of four corners, once
    CHECK(n.indices == 18);
    CHECK(n.instances == 5);
    CHECK(n.triangles == 30); // each level's two triangles at each of the five
    CHECK(n.chunks == 15);    // a chunk for each level at each placement
}

TEST_CASE("zone models: a neighbour zone keeps the placements on its side of the line whole") {
    render::ZoneRenderOptions options;
    options.offset = glm::vec3(1000.0f, 0.0f, 0.0f);
    options.keep = {glm::vec4(1.0f, 0.0f, 0.0f, -1500.0f)}; // x >= 1500 after the move: the placements at 600 and 900
    const auto prepared = render::ZoneRenderer::prepare(zone_with(4, 1), options, 16);
    REQUIRE(prepared);
    const auto n = render::ZoneRenderer::prepared_counts(*prepared);
    CHECK(n.instances == 2);
    CHECK(n.vertices == 4); // not cut: the mesh is in its own space
    CHECK(n.triangles == 4);
}
