#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <limits>

#include "plugin/world_geometry.h"

using namespace hailstone;

namespace {
HailstoneWorldFrame world_frame(std::uint64_t token = 17, std::uint32_t epoch = 3) {
    HailstoneWorldFrame frame{};
    frame.size = sizeof(frame);
    frame.frame = token;
    frame.epoch = epoch;
    frame.capabilities = HAILSTONE_WORLD_GEOMETRY | HAILSTONE_WORLD_TERRAIN |
                         HAILSTONE_WORLD_DRAWN_ANCHORS | HAILSTONE_WORLD_PRESENT_RECEIPTS;
    frame.remaining_queries = 8;
    return frame;
}
std::array<HailstoneWorldVertex, 3> triangle() {
    return {{{{0, 0, 0}, {1, 0, 0, 0.5f}}, {{1, 0, 0}, {1, 0, 0, 0.5f}},
             {{0, 1, 0}, {1, 0, 0, 0.5f}}}};
}
HailstoneWorldSubmission submission(const std::array<HailstoneWorldVertex, 3>& vertices) {
    HailstoneWorldSubmission out{};
    out.size = sizeof(out);
    out.frame = 17;
    out.epoch = 3;
    out.tag = 10;
    out.revision = 4;
    out.primitive = HAILSTONE_WORLD_TRIANGLES;
    out.vertices = vertices.data();
    out.vertex_count = vertices.size();
    return out;
}
}

TEST_CASE("world geometry host: frame-scoped queries distinguish missing from deferred and preserve outputs") {
    plugin::WorldGeometryHost host;
    unsigned floors = 0;
    plugin::WorldQueries queries;
    queries.floor = [&](float x, float, float, float& height) {
        ++floors;
        if (x == 0) return HAILSTONE_WORLD_MISSING;
        height = 0;
        return HAILSTONE_WORLD_RESOLVED;
    };
    queries.anchor = [](std::uint32_t id, HailstoneWorldAnchor& out) {
        if (id != 5) return HAILSTONE_WORLD_MISSING;
        out.flags = HAILSTONE_WORLD_DRAWN_POSE | HAILSTONE_WORLD_CACHED_FLOOR;
        out.position[2] = 6;
        out.heading = 128;
        out.floor_z = 0;
        return HAILSTONE_WORLD_RESOLVED;
    };
    auto frame = world_frame();
    frame.remaining_queries = 4;
    host.begin(frame, queries);
    float height = 123;
    CHECK(host.floor(16, 3, 1, 2, 3, &height) == HAILSTONE_WORLD_DEFERRED);
    CHECK(floors == 0);
    CHECK(host.floor(17, 3, 0, 2, 3, &height) == HAILSTONE_WORLD_MISSING);
    CHECK(height == 123);
    CHECK(host.floor(17, 3, 1, 2, 3, &height) == HAILSTONE_WORLD_RESOLVED);
    CHECK(height == 0);
    HailstoneWorldAnchor anchor{};
    anchor.size = sizeof(anchor);
    CHECK(host.anchor(17, 3, 5, &anchor) == HAILSTONE_WORLD_RESOLVED);
    CHECK(anchor.spawn_id == 5);
    CHECK(anchor.floor_z == 0);
    CHECK(anchor.flags == (HAILSTONE_WORLD_DRAWN_POSE | HAILSTONE_WORLD_CACHED_FLOOR));
    anchor.size = sizeof(anchor.size);
    anchor.spawn_id = 99;
    CHECK(host.anchor(17, 3, 5, &anchor) == HAILSTONE_WORLD_RESOLVED);
    CHECK(anchor.size == sizeof(anchor.size));
    CHECK(anchor.spawn_id == 99); // a short caller's unknown tail is untouched
    CHECK(host.floor(17, 3, 1, 2, 3, &height) == HAILSTONE_WORLD_DEFERRED);
    host.seal();
    CHECK(host.anchor(17, 3, 5, &anchor) == HAILSTONE_WORLD_DEFERRED);
    host.cancel();
    CHECK(host.capabilities() == 0);
}

TEST_CASE("world geometry host: copied batches retain owner identity and cannot cross a frame or epoch") {
    plugin::WorldGeometryHost host;
    host.begin(world_frame(), {});
    auto vertices = triangle();
    auto batch = submission(vertices);
    CHECK_FALSE(host.submit(0, &batch));
    CHECK(host.submit(1, &batch));
    vertices[0].position[0] = 90;
    CHECK(host.batches().front().vertices.front().position[0] == 0);
    ++batch.revision;
    CHECK_FALSE(host.submit(1, &batch)); // same tag cannot replace already admitted geometry
    CHECK(host.submit(2, &batch)); // another owner has its own tag namespace
    batch.epoch = 2;
    CHECK_FALSE(host.submit(3, &batch));
    batch.epoch = 3;
    vertices[0].color[3] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(host.submit(3, &batch));
    host.retire(2);
    REQUIRE(host.batches().size() == 1);
    host.seal();
    CHECK_FALSE(host.submit(3, &batch));
    host.begin(world_frame(18, 4), {});
    CHECK(host.batches().empty());
    CHECK_FALSE(host.submit(1, &batch));
}

TEST_CASE("world geometry host: presentation proof requires complete recording and actual submission") {
    for (const bool submitted : {false, true})
        for (const bool presented : {false, true}) {
            plugin::WorldGeometryHost host;
            host.begin(world_frame(), {});
            auto vertices = triangle();
            auto first = submission(vertices);
            auto second = first;
            second.tag = 11;
            REQUIRE(host.submit(7, &first));
            REQUIRE(host.submit(7, &second));
            host.recorded(0, 3); // producers cannot certify recording before the admission phase ends
            host.seal();
            host.recorded(0, 3);
            host.recorded(1, 2); // incomplete recording never counts
            const auto receipts = host.finish(submitted, presented);
            REQUIRE(receipts.size() == 2);
            CHECK(receipts[0].owner == 7);
            CHECK(receipts[0].receipt.frame == 17);
            CHECK(receipts[0].receipt.epoch == 3);
            CHECK(receipts[0].receipt.tag == 10);
            CHECK(receipts[0].receipt.revision == 4);
            CHECK(receipts[0].receipt.vertex_count == 3);
            CHECK(bool(receipts[0].receipt.flags & HAILSTONE_WORLD_RECORDED));
            CHECK(bool(receipts[0].receipt.flags & HAILSTONE_WORLD_SUBMITTED) == submitted);
            CHECK(bool(receipts[0].receipt.flags & HAILSTONE_WORLD_PRESENTED) == (submitted && presented));
            CHECK(receipts[1].receipt.flags == 0);
            CHECK(receipts[1].receipt.vertex_count == 0);
            CHECK(host.batches().empty());
            CHECK(host.finish(true, true).empty());
        }
}

TEST_CASE("world geometry host: batch budgets bound work and cancellation retires all receipts") {
    plugin::WorldGeometryHost host;
    host.begin(world_frame(), {});
    auto vertices = triangle();
    auto batch = submission(vertices);
    for (std::uint64_t index = 0; index < game::kWorldGeometryMaxBatches; ++index) {
        batch.tag = index + 1;
        REQUIRE(host.submit(1, &batch));
    }
    ++batch.tag;
    CHECK_FALSE(host.submit(1, &batch));
    host.cancel();
    CHECK(host.finish(true, true).empty());
    CHECK_FALSE(host.submit(1, &batch));
    host.begin(world_frame(18), {});
    batch.frame = 18;
    CHECK(host.submit(1, &batch));
}

namespace {
HailstoneWorldSurfacePatch surface_patch() {
    HailstoneWorldSurfacePatch query{};
    query.size = sizeof(query);
    query.frame = 17;
    query.epoch = 3;
    const std::array<float, 9> vertices{0, 0, 1, 1, 0, 2, 0, 1, 3};
    std::copy(vertices.begin(), vertices.end(), query.vertices);
    query.maximum_rise = 6;
    query.tolerance = 0.04f;
    return query;
}
}

TEST_CASE("world geometry host: surface proof validates input tokens capability and current acceptance") {
    plugin::WorldGeometryHost host;
    auto frame = world_frame();
    frame.capabilities |= HAILSTONE_WORLD_SURFACE_PATCH;
    unsigned calls = 0;
    plugin::WorldQueries queries;
    queries.surface_patch = [&](const HailstoneWorldSurfacePatch& input,
                                HailstoneWorldSurfacePatchResult&, std::uint32_t&) {
        ++calls;
        CHECK(input.vertices[5] == 2); // the candidate is a slope, not a constant reference height
        CHECK(input.maximum_rise == 6);
        return HAILSTONE_WORLD_PATCH_PLANAR;
    };
    auto query = surface_patch();
    HailstoneWorldSurfacePatchResult out{};
    out.size = sizeof(out);
    out.flags = 99;
    out.witness[0] = 123;
    host.begin(frame, {});
    CHECK((host.capabilities() & HAILSTONE_WORLD_SURFACE_PATCH) == 0);
    CHECK(host.surface_patch(&query, &out) == HAILSTONE_WORLD_PATCH_DEFERRED);
    host.begin(frame, queries);
    CHECK((host.capabilities() & HAILSTONE_WORLD_SURFACE_PATCH) != 0);
    --query.frame;
    CHECK(host.surface_patch(&query, &out) == HAILSTONE_WORLD_PATCH_DEFERRED);
    ++query.frame;
    query.size = sizeof(query) - 1;
    CHECK(host.surface_patch(&query, &out) == HAILSTONE_WORLD_PATCH_DEFERRED);
    query = surface_patch();
    query.vertices[6] = 1;
    query.vertices[7] = 0;
    CHECK(host.surface_patch(&query, &out) == HAILSTONE_WORLD_PATCH_DEFERRED);
    query = surface_patch();
    query.maximum_rise = -1;
    CHECK(host.surface_patch(&query, &out) == HAILSTONE_WORLD_PATCH_DEFERRED);
    query = surface_patch();
    query.tolerance = std::numeric_limits<float>::quiet_NaN();
    CHECK(host.surface_patch(&query, &out) == HAILSTONE_WORLD_PATCH_DEFERRED);
    CHECK(calls == 0);
    CHECK(out.flags == 99);
    CHECK(out.witness[0] == 123);
    query = surface_patch();
    CHECK(host.surface_patch(&query, &out) == HAILSTONE_WORLD_PATCH_PLANAR);
    CHECK(calls == 1);
    CHECK(out.flags == 0);
    CHECK(out.witness[0] == 0);
    CHECK(host.frame().remaining_queries == frame.remaining_queries - 1);
    host.seal();
    CHECK(host.surface_patch(&query, &out) == HAILSTONE_WORLD_PATCH_DEFERRED);
    CHECK(calls == 1);
}

TEST_CASE("world geometry host: surface witnesses and clipping share a finite frame work budget") {
    plugin::WorldGeometryHost host;
    auto frame = world_frame();
    frame.capabilities |= HAILSTONE_WORLD_SURFACE_PATCH;
    frame.remaining_queries = 6;
    unsigned calls = 0;
    plugin::WorldQueries queries;
    queries.surface_patch = [&](const HailstoneWorldSurfacePatch&,
                                HailstoneWorldSurfacePatchResult& result, std::uint32_t& remaining) {
        ++calls;
        result.witness[0] = 0.2f;
        result.witness[1] = 0.3f;
        if (calls == 1) {
            CHECK(remaining == 5);
            remaining -= 3;
            result.flags = HAILSTONE_WORLD_PATCH_HEIGHT_KNOWN | HAILSTONE_WORLD_PATCH_SOURCE_FACE | HAILSTONE_WORLD_PATCH_REGION;
            result.witness[2] = 4;
            const std::array<float, 9> face{0, 0, 4, 0, 1, 4, 1, 0, 4};
            std::copy(face.begin(), face.end(), result.source_face);
            result.region_count = 3;
            const std::array<double, 6> region{0, 0, 1, 0, 0, 1};
            std::copy(region.begin(), region.end(), result.region_xy);
            return HAILSTONE_WORLD_PATCH_MISMATCH;
        }
        CHECK(remaining == 1);
        --remaining;
        result.witness[2] = std::numeric_limits<float>::quiet_NaN(); // no asserted height for a coverage hole
        return HAILSTONE_WORLD_PATCH_MISSING;
    };
    host.begin(frame, queries);
    const auto query = surface_patch();
    HailstoneWorldSurfacePatchResult out{};
    out.size = sizeof(out);
    CHECK(host.surface_patch(&query, &out) == HAILSTONE_WORLD_PATCH_MISMATCH);
    CHECK(out.flags == (HAILSTONE_WORLD_PATCH_HEIGHT_KNOWN | HAILSTONE_WORLD_PATCH_SOURCE_FACE | HAILSTONE_WORLD_PATCH_REGION));
    CHECK(out.witness[2] == 4);
    CHECK(out.source_face[2] == 4);
    CHECK(out.region_count == 3);
    CHECK(out.region_xy[2] == 1);
    CHECK(host.frame().remaining_queries == 2);
    CHECK(host.surface_patch(&query, &out) == HAILSTONE_WORLD_PATCH_MISSING);
    CHECK(out.flags == 0);
    CHECK(out.witness[2] == 0);
    CHECK(host.frame().remaining_queries == 0);
    CHECK(host.surface_patch(&query, &out) == HAILSTONE_WORLD_PATCH_DEFERRED);
    CHECK(calls == 2);
}

TEST_CASE("world geometry host: incomplete or invalid surface witnesses never escape as a proof") {
    for (const int invalid : {0, 1, 2, 3, 4, 5, 6, 7}) {
        plugin::WorldGeometryHost host;
        auto frame = world_frame();
        frame.capabilities |= HAILSTONE_WORLD_SURFACE_PATCH;
        plugin::WorldQueries queries;
        queries.surface_patch = [invalid](const HailstoneWorldSurfacePatch&,
                                         HailstoneWorldSurfacePatchResult& result, std::uint32_t& remaining) {
            result.witness[0] = 0.2f;
            result.witness[1] = invalid == 1 ? 0.0f : 0.3f; // boundary witnesses cannot refine an interior
            result.witness[2] = invalid == 2 ? std::numeric_limits<float>::infinity() : 4;
            result.flags = invalid == 3 ? 0 : HAILSTONE_WORLD_PATCH_HEIGHT_KNOWN;
            if (invalid == 4) result.flags |= HAILSTONE_WORLD_PATCH_SOURCE_FACE; // degenerate source face
            if (invalid >= 5) {
                result.flags |= HAILSTONE_WORLD_PATCH_REGION;
                result.region_count = invalid == 5 ? HAILSTONE_WORLD_PATCH_MAX_REGION_VERTICES + 1 : 3;
                const std::array<double, 6> outside{0, 0, 2, 0, 0, 2};
                std::copy(outside.begin(), outside.end(), result.region_xy);
                if (invalid == 7) {
                    result.region_count = 4;
                    const std::array<double, 8> concave{0, 0, 0.8, 0, 0.1, 0.1, 0, 0.8};
                    std::copy(concave.begin(), concave.end(), result.region_xy);
                }
            }
            if (invalid == 0) {
                remaining = 0;
                return HAILSTONE_WORLD_PATCH_DEFERRED;
            }
            return HAILSTONE_WORLD_PATCH_MISMATCH;
        };
        host.begin(frame, queries);
        const auto query = surface_patch();
        HailstoneWorldSurfacePatchResult out{};
        out.size = sizeof(out);
        out.flags = 99;
        out.witness[0] = 123;
        CHECK(host.surface_patch(&query, &out) == HAILSTONE_WORLD_PATCH_DEFERRED);
        CHECK(out.flags == 99);
        CHECK(out.witness[0] == 123);
        CHECK(host.frame().remaining_queries <= frame.remaining_queries - 1);
    }
}

TEST_CASE("world geometry host: rounded boundary witness needs a valid source face and interior convex region") {
    for (const int variant : {0, 1, 2, 3, 4, 5, 6}) {
        plugin::WorldGeometryHost host;
        auto frame = world_frame();
        frame.capabilities |= HAILSTONE_WORLD_SURFACE_PATCH;
        plugin::WorldQueries queries;
        queries.surface_patch = [variant](const HailstoneWorldSurfacePatch&,
                                         HailstoneWorldSurfacePatchResult& result, std::uint32_t&) {
            result.flags = HAILSTONE_WORLD_PATCH_HEIGHT_KNOWN | HAILSTONE_WORLD_PATCH_SOURCE_FACE | HAILSTONE_WORLD_PATCH_REGION;
            result.witness[0] = result.witness[1] = 1;
            result.witness[2] = 4; // XY rounded to the producer's first vertex
            const std::array<float, 9> face{1, 1, 4, 1, 2, 4, 2, 1, 4};
            std::copy(face.begin(), face.end(), result.source_face);
            result.region_count = 3;
            const std::array<double, 6> region{1, 1, 1.000000001, 1, 1, 1.000000001};
            std::copy(region.begin(), region.end(), result.region_xy);
            if (variant == 1) result.flags &= ~static_cast<std::uint32_t>(HAILSTONE_WORLD_PATCH_SOURCE_FACE);
            if (variant == 2) result.flags &= ~static_cast<std::uint32_t>(HAILSTONE_WORLD_PATCH_REGION);
            if (variant == 3) result.region_xy[2] = 3; // outside the producer triangle
            if (variant == 4) result.region_xy[5] = 1; // zero-area region/centroid on producer edge
            if (variant == 5) {
                const std::array<float, 9> wrong_face{1.5f, 1.5f, 4, 1.5f, 1.6f, 4, 1.6f, 1.5f, 4};
                std::copy(wrong_face.begin(), wrong_face.end(), result.source_face);
            }
            if (variant == 6) result.witness[0] = 0.999f; // arbitrary outside point, not float(region centroid)
            return HAILSTONE_WORLD_PATCH_MISMATCH;
        };
        host.begin(frame, queries);
        auto query = surface_patch();
        for (std::size_t i = 0; i < 3; ++i) {
            query.vertices[i * 3] += 1;
            query.vertices[i * 3 + 1] += 1;
        }
        HailstoneWorldSurfacePatchResult out{};
        out.size = sizeof(out);
        out.flags = 99;
        const auto status = host.surface_patch(&query, &out);
        if (variant == 0) {
            CHECK(status == HAILSTONE_WORLD_PATCH_MISMATCH);
            CHECK(out.witness[0] == 1);
            CHECK(out.witness[1] == 1);
            CHECK(out.region_count == 3);
        } else {
            CHECK(status == HAILSTONE_WORLD_PATCH_DEFERRED);
            CHECK(out.flags == 99);
        }
    }
}
