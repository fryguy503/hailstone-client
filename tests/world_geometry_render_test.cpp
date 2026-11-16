#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "game/world_geometry.h"
#include "render/camera.h"
#include "render/world_geometry_renderer.h"

using namespace hailstone;

namespace {
std::array<game::WorldGeometryVertex, 3> geometry_triangle() {
    return {{{{10.0f, 20.0f, 30.0f}, {1.0f, 0.0f, 0.0f, 0.5f}},
             {{11.0f, 20.0f, 30.0f}, {0.0f, 1.0f, 0.0f, 0.5f}},
             {{10.0f, 21.0f, 30.0f}, {0.0f, 0.0f, 1.0f, 0.5f}}}};
}
} // namespace

TEST_CASE("world geometry renderer: topology and count reject whole batches") {
    using enum game::WorldGeometryValidation;
    auto vertices = geometry_triangle();
    game::WorldGeometryBatch batch{{1, 2, 3}, game::WorldGeometryPrimitive::Triangles, vertices};
    CHECK(game::validate_world_geometry(batch) == Valid);
    batch.vertices = {};
    CHECK(game::validate_world_geometry(batch) == Empty);
    batch.vertices = std::span(vertices).first(2);
    CHECK(game::validate_world_geometry(batch) == InvalidVertexCount);
    batch.primitive = game::WorldGeometryPrimitive::Lines;
    CHECK(game::validate_world_geometry(batch) == Valid);
    batch.vertices = vertices;
    CHECK(game::validate_world_geometry(batch) == InvalidVertexCount);
    batch.primitive = static_cast<game::WorldGeometryPrimitive>(255);
    CHECK(game::validate_world_geometry(batch) == InvalidPrimitive);
}

TEST_CASE("world geometry renderer: bounded positions and normalized colors reject nonfinite input") {
    using enum game::WorldGeometryValidation;
    auto vertices = geometry_triangle();
    const game::WorldGeometryBatch batch{{1, 2, 3}, game::WorldGeometryPrimitive::Triangles, vertices};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        INFO(axis);
        for (float sign : {-1.0f, 1.0f}) {
            vertices = geometry_triangle();
            vertices[0].position[axis] = sign * game::kWorldGeometryMaxCoordinate;
            CHECK(game::validate_world_geometry(batch) == Valid);
            vertices[0].position[axis] = sign * std::nextafter(game::kWorldGeometryMaxCoordinate,
                                                               std::numeric_limits<float>::infinity());
            CHECK(game::validate_world_geometry(batch) == InvalidVertex);
        }
        for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max()}) {
            vertices = geometry_triangle();
            vertices[0].position[axis] = value;
            CHECK(game::validate_world_geometry(batch) == InvalidVertex);
        }
    }
    for (std::size_t channel = 0; channel < 4; ++channel) {
        INFO(channel);
        for (float value : {-0.01f, 1.01f, std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity()}) {
            vertices = geometry_triangle();
            vertices[1].color[channel] = value;
            CHECK(game::validate_world_geometry(batch) == InvalidVertex);
        }
        for (float value : {0.0f, 1.0f}) {
            vertices = geometry_triangle();
            vertices[1].color[channel] = value;
            CHECK(game::validate_world_geometry(batch) == Valid);
        }
    }
}

TEST_CASE("world geometry renderer: atomic marker parts and full-width owner tags are scoped per frame") {
    using enum game::WorldGeometryValidation;
    const auto triangle = geometry_triangle();
    // A fill, outline triangles and glyph triangles share one admission.
    std::vector<game::WorldGeometryVertex> marker;
    for (int part = 0; part < 3; ++part) marker.insert(marker.end(), triangle.begin(), triangle.end());
    game::WorldGeometryFrameBudget frame;
    game::WorldGeometryBatch batch{{1, 7, 1}, game::WorldGeometryPrimitive::Triangles, marker};
    CHECK(frame.admit(batch) == Valid);
    CHECK(frame.reserved_vertices() == 9);
    ++batch.id.revision;
    CHECK(frame.admit(batch) == DuplicateTag);
    CHECK(frame.reserved_vertices() == 9);
    batch.id.owner += std::uint64_t{1} << 32;
    CHECK(frame.admit(batch) == Valid);
    batch.id.owner = 1;
    batch.id.tag += std::uint64_t{1} << 32;
    CHECK(frame.admit(batch) == Valid);
    CHECK(frame.reserved_vertices() == 27);
    frame = {};
    batch.id = {1, 7, 2};
    CHECK(frame.admit(batch) == Valid);
    CHECK(frame.reserved_vertices() == 9);
}

TEST_CASE("world geometry renderer: 256 complete markers fit and the next batch is rejected") {
    using enum game::WorldGeometryValidation;
    static_assert(game::kWorldGeometryMaxBatches >= 256);
    game::WorldGeometryFrameBudget frame;
    const auto vertices = geometry_triangle();
    game::WorldGeometryBatch batch{{1, 1, 1}, game::WorldGeometryPrimitive::Triangles, vertices};
    for (std::uint32_t index = 0; index < game::kWorldGeometryMaxBatches; ++index) {
        batch.id.tag = index + 1;
        REQUIRE(frame.admit(batch) == Valid);
    }
    ++batch.id.tag;
    CHECK(frame.admit(batch) == BatchLimit);
    CHECK(frame.reserved_vertices() == game::kWorldGeometryMaxBatches * 3);
}

TEST_CASE("world geometry renderer: vertex and validation work budgets never partially reserve a batch") {
    using enum game::WorldGeometryValidation;
    const auto triangle = geometry_triangle();
    std::vector<game::WorldGeometryVertex> vertices(game::kWorldGeometryMaxVertices, triangle.front());
    game::WorldGeometryFrameBudget frame;
    game::WorldGeometryBatch batch{{1, 1, 1}, game::WorldGeometryPrimitive::Triangles, vertices};
    CHECK(frame.admit(batch) == Valid);
    CHECK(frame.reserved_vertices() == game::kWorldGeometryMaxVertices);
    batch.id.tag = 2;
    batch.vertices = triangle;
    CHECK(frame.admit(batch) == VertexLimit);
    CHECK(frame.reserved_vertices() == game::kWorldGeometryMaxVertices);

    frame = {};
    batch.vertices = std::span(vertices).first(vertices.size() - 3);
    CHECK(frame.admit(batch) == Valid);
    const auto used = frame.reserved_vertices();
    ++batch.id.tag;
    batch.vertices = std::span(vertices).first(6);
    CHECK(frame.admit(batch) == VertexLimit);
    CHECK(frame.reserved_vertices() == used);
    ++batch.id.tag;
    batch.vertices = triangle;
    CHECK(frame.admit(batch) == Valid);
    CHECK(frame.reserved_vertices() == game::kWorldGeometryMaxVertices);

    frame = {};
    vertices.front().color[0] = std::numeric_limits<float>::quiet_NaN();
    ++batch.id.tag;
    batch.vertices = vertices;
    CHECK(frame.admit(batch) == InvalidVertex);
    CHECK(frame.reserved_vertices() == 0);
    ++batch.id.tag;
    batch.vertices = triangle;
    CHECK(frame.admit(batch) == VertexLimit); // invalid batches consume bounded validation work too
    CHECK(frame.reserved_vertices() == 0);
    frame = {};
    CHECK(frame.admit(batch) == Valid);
    CHECK(frame.reserved_vertices() == 3);
}

TEST_CASE("world geometry renderer: scene coordinates pass through the camera basis and clip products stay finite") {
    render::FlyCamera camera;
    camera.position = {10.0f, 20.0f, 30.0f};
    camera.yaw = 0.0f; // +X in zone-file/HailstoneSpawn coordinates, Z up
    const auto vp = camera.projection(1280, 720, 1000.0f) * camera.view();
    CHECK(render::world_geometry_view_valid(vp));
    const glm::vec4 front = vp * glm::vec4(20.0f, 20.0f, 30.0f, 1.0f);
    CHECK(front.x == doctest::Approx(0.0f).epsilon(0.00001));
    CHECK(front.y == doctest::Approx(0.0f).epsilon(0.00001));
    CHECK(front.w > 0.0f);
    CHECK(front.z / front.w > 0.0f);
    CHECK(front.z / front.w < 1.0f);
    const glm::vec4 above = vp * glm::vec4(20.0f, 20.0f, 31.0f, 1.0f);
    CHECK(above.y > front.y);
    auto invalid = vp;
    invalid[2][0] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(render::world_geometry_view_valid(invalid));
    invalid = glm::mat4(1.0f);
    invalid[0][0] = std::numeric_limits<float>::max();
    CHECK_FALSE(render::world_geometry_view_valid(invalid));
    invalid = glm::mat4(1.0f);
    invalid[3][2] = std::numeric_limits<float>::max();
    CHECK_FALSE(render::world_geometry_view_valid(invalid));
}
