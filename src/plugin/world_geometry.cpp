#include "plugin/world_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <utility>

namespace hailstone::plugin {

namespace {
bool finite_position(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}
}

void WorldGeometryHost::begin(HailstoneWorldFrame frame, WorldQueries queries) {
    cancel();
    if (!frame.frame || !frame.epoch || !(frame.capabilities & HAILSTONE_WORLD_GEOMETRY)) return;
    frame.size = sizeof(frame);
    frame.maximum_batches = game::kWorldGeometryMaxBatches;
    frame.maximum_vertices = game::kWorldGeometryMaxVertices;
    frame.remaining_queries = std::min(frame.remaining_queries, kMaximumQueries);
    if (!queries.floor) frame.capabilities &= ~static_cast<std::uint32_t>(HAILSTONE_WORLD_TERRAIN);
    if (!queries.anchor) frame.capabilities &= ~static_cast<std::uint32_t>(HAILSTONE_WORLD_DRAWN_ANCHORS);
    if (!queries.surface_patch) frame.capabilities &= ~static_cast<std::uint32_t>(HAILSTONE_WORLD_SURFACE_PATCH);
    frame_ = frame;
    queries_ = std::move(queries);
    capabilities_ = frame_.capabilities;
    active_ = accepting_ = true;
}

void WorldGeometryHost::cancel() {
    active_ = accepting_ = false;
    capabilities_ = 0;
    frame_ = {};
    queries_ = {};
    budget_ = {};
    attempts_ = inspected_ = 0;
    batches_.clear();
}

void WorldGeometryHost::retire(std::uint64_t owner) {
    std::erase_if(batches_, [&](const OwnedWorldBatch& batch) { return batch.id.owner == owner; });
    // Retiring an owner never replenishes this frame's spent validation budget.
}

bool WorldGeometryHost::current(std::uint64_t frame, std::uint32_t epoch) const {
    return active_ && accepting_ && frame == frame_.frame && epoch == frame_.epoch;
}

int WorldGeometryHost::floor(std::uint64_t frame, std::uint32_t epoch, float x, float y, float z, float* out) {
    if (!current(frame, epoch) || !out || !finite_position(x, y, z) || !queries_.floor ||
        !frame_.remaining_queries) return HAILSTONE_WORLD_DEFERRED;
    --frame_.remaining_queries;
    float height = 0;
    try {
        const int result = queries_.floor(x, y, z, height);
        if (result == HAILSTONE_WORLD_MISSING) return result;
        if (result != HAILSTONE_WORLD_RESOLVED || !std::isfinite(height)) return HAILSTONE_WORLD_DEFERRED;
    } catch (...) {
        return HAILSTONE_WORLD_DEFERRED;
    }
    *out = height;
    return HAILSTONE_WORLD_RESOLVED;
}

int WorldGeometryHost::anchor(std::uint64_t frame, std::uint32_t epoch, std::uint32_t id,
                              HailstoneWorldAnchor* out) {
    if (!current(frame, epoch) || !out || out->size < sizeof(out->size) || !id || !queries_.anchor ||
        !frame_.remaining_queries) return HAILSTONE_WORLD_DEFERRED;
    --frame_.remaining_queries;
    HailstoneWorldAnchor value{};
    value.size = sizeof(value);
    value.spawn_id = id;
    try {
        const int result = queries_.anchor(id, value);
        if (result == HAILSTONE_WORLD_MISSING) return result;
        if (result != HAILSTONE_WORLD_RESOLVED || !(value.flags & HAILSTONE_WORLD_DRAWN_POSE) ||
            !finite_position(value.position[0], value.position[1], value.position[2]) ||
            !std::isfinite(value.heading) ||
            ((value.flags & HAILSTONE_WORLD_CACHED_FLOOR) && !std::isfinite(value.floor_z)))
            return HAILSTONE_WORLD_DEFERRED;
    } catch (...) {
        return HAILSTONE_WORLD_DEFERRED;
    }
    value.spawn_id = id;
    const auto copied = std::min<std::size_t>(out->size, sizeof(value));
    value.size = static_cast<std::uint32_t>(copied);
    std::memcpy(out, &value, copied);
    return HAILSTONE_WORLD_RESOLVED;
}

int WorldGeometryHost::surface_patch(const HailstoneWorldSurfacePatch* query,
                                      HailstoneWorldSurfacePatchResult* out) {
    if (!query || query->size < sizeof(*query) || !out || out->size < sizeof(out->size) ||
        !current(query->frame, query->epoch) || !queries_.surface_patch ||
        !(capabilities_ & HAILSTONE_WORLD_SURFACE_PATCH) || !frame_.remaining_queries)
        return HAILSTONE_WORLD_PATCH_DEFERRED;
    const auto input = *query;
    for (const float coordinate : input.vertices)
        if (!std::isfinite(coordinate)) return HAILSTONE_WORLD_PATCH_DEFERRED;
    if (!std::isfinite(input.maximum_rise) || input.maximum_rise < 0 ||
        !std::isfinite(input.tolerance) || input.tolerance < 0)
        return HAILSTONE_WORLD_PATCH_DEFERRED;
    const double ax = input.vertices[0], ay = input.vertices[1];
    const double bx = input.vertices[3], by = input.vertices[4];
    const double cx = input.vertices[6], cy = input.vertices[7];
    const double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (!std::isfinite(area) || std::abs(area) <= 1.0e-8) return HAILSTONE_WORLD_PATCH_DEFERRED;
    --frame_.remaining_queries;
    const auto remaining = frame_.remaining_queries;
    HailstoneWorldSurfacePatchResult value{};
    value.size = sizeof(value);
    int result = HAILSTONE_WORLD_PATCH_DEFERRED;
    try {
        result = queries_.surface_patch(input, value, frame_.remaining_queries);
    } catch (...) {
        frame_.remaining_queries = 0; // incomplete work cannot replenish the shared budget
        return HAILSTONE_WORLD_PATCH_DEFERRED;
    }
    frame_.remaining_queries = std::min(frame_.remaining_queries, remaining);
    if (result != HAILSTONE_WORLD_PATCH_PLANAR && result != HAILSTONE_WORLD_PATCH_MISMATCH &&
        result != HAILSTONE_WORLD_PATCH_MISSING) return HAILSTONE_WORLD_PATCH_DEFERRED;
    if (value.flags & ~static_cast<std::uint32_t>(HAILSTONE_WORLD_PATCH_HEIGHT_KNOWN | HAILSTONE_WORLD_PATCH_SOURCE_FACE |
                                               HAILSTONE_WORLD_PATCH_REGION))
        return HAILSTONE_WORLD_PATCH_DEFERRED;
    if (result != HAILSTONE_WORLD_PATCH_PLANAR) {
        const double x = value.witness[0], y = value.witness[1];
        if (!std::isfinite(x) || !std::isfinite(y) ||
            ((value.flags & HAILSTONE_WORLD_PATCH_HEIGHT_KNOWN) && !std::isfinite(value.witness[2])) ||
            (result == HAILSTONE_WORLD_PATCH_MISMATCH && !(value.flags & HAILSTONE_WORLD_PATCH_HEIGHT_KNOWN)))
            return HAILSTONE_WORLD_PATCH_DEFERRED;
        const double u = ((bx - x) * (cy - y) - (by - y) * (cx - x)) / area;
        const double v = ((cx - x) * (ay - y) - (cy - y) * (ax - x)) / area;
        const double w = 1.0 - u - v;
        const bool strict_witness = u > 0 && v > 0 && w > 0;
        bool region_interior = false;
        double region_x = 0, region_y = 0;
        if (!(value.flags & HAILSTONE_WORLD_PATCH_HEIGHT_KNOWN)) value.witness[2] = 0;
        if (value.flags & HAILSTONE_WORLD_PATCH_REGION) {
            if (value.region_count < 3 || value.region_count > HAILSTONE_WORLD_PATCH_MAX_REGION_VERTICES)
                return HAILSTONE_WORLD_PATCH_DEFERRED;
            const double ox = value.region_xy[0], oy = value.region_xy[1];
            double twice_area = 0;
            double center_dx = 0, center_dy = 0;
            for (std::uint32_t i = 0; i < value.region_count; ++i) {
                const double rx = value.region_xy[i * 2], ry = value.region_xy[i * 2 + 1];
                if (!std::isfinite(rx) || !std::isfinite(ry)) return HAILSTONE_WORLD_PATCH_DEFERRED;
                const double ru = ((bx - rx) * (cy - ry) - (by - ry) * (cx - rx)) / area;
                const double rv = ((cx - rx) * (ay - ry) - (cy - ry) * (ax - rx)) / area;
                const double rw = 1.0 - ru - rv;
                if (!(ru >= -1.0e-7 && rv >= -1.0e-7 && rw >= -1.0e-7))
                    return HAILSTONE_WORLD_PATCH_DEFERRED;
                center_dx += rx - ox;
                center_dy += ry - oy;
                const auto next = (i + 1) % value.region_count;
                twice_area += (rx - ox) * (value.region_xy[next * 2 + 1] - oy) -
                              (ry - oy) * (value.region_xy[next * 2] - ox);
            }
            if (!std::isfinite(twice_area) || twice_area == 0) return HAILSTONE_WORLD_PATCH_DEFERRED;
            region_x = ox + center_dx / value.region_count;
            region_y = oy + center_dy / value.region_count;
            const double center_u = ((bx - region_x) * (cy - region_y) - (by - region_y) * (cx - region_x)) / area;
            const double center_v = ((cx - region_x) * (ay - region_y) - (cy - region_y) * (ax - region_x)) / area;
            const double center_w = 1.0 - center_u - center_v;
            region_interior = center_u > 0 && center_v > 0 && center_w > 0;
            const double direction = twice_area > 0 ? 1.0 : -1.0;
            for (std::uint32_t i = 0; i < value.region_count; ++i) {
                const auto next = (i + 1) % value.region_count;
                const auto after = (i + 2) % value.region_count;
                const double dx = value.region_xy[next * 2] - value.region_xy[i * 2];
                const double dy = value.region_xy[next * 2 + 1] - value.region_xy[i * 2 + 1];
                const double ex = value.region_xy[after * 2] - value.region_xy[next * 2];
                const double ey = value.region_xy[after * 2 + 1] - value.region_xy[next * 2 + 1];
                const double tolerance = 1.0e-12 * std::max(1.0, std::abs(dx * ey) + std::abs(dy * ex));
                if (direction * (dx * ey - dy * ex) < -tolerance) return HAILSTONE_WORLD_PATCH_DEFERRED;
            }
        } else {
            value.region_count = 0;
            std::fill_n(value.region_xy, HAILSTONE_WORLD_PATCH_MAX_REGION_VERTICES * 2, 0.0);
        }
        const bool precise_region = (value.flags & HAILSTONE_WORLD_PATCH_SOURCE_FACE) &&
                                    (value.flags & HAILSTONE_WORLD_PATCH_REGION) && region_interior;
        // A skinny triangle's genuine interior can round outside in float. Such a hint is usable only when it is
        // exactly the float representation of the independently validated double region centroid.
        if (!strict_witness && (!precise_region || value.witness[0] != static_cast<float>(region_x) ||
                                value.witness[1] != static_cast<float>(region_y)))
            return HAILSTONE_WORLD_PATCH_DEFERRED;
        if (value.flags & HAILSTONE_WORLD_PATCH_SOURCE_FACE) {
            if (!(value.flags & HAILSTONE_WORLD_PATCH_HEIGHT_KNOWN)) return HAILSTONE_WORLD_PATCH_DEFERRED;
            for (const float coordinate : value.source_face)
                if (!std::isfinite(coordinate)) return HAILSTONE_WORLD_PATCH_DEFERRED;
            const double at_x = precise_region ? region_x : x, at_y = precise_region ? region_y : y;
            const double fx = value.source_face[0], fy = value.source_face[1];
            const double gx = value.source_face[3], gy = value.source_face[4];
            const double hx = value.source_face[6], hy = value.source_face[7];
            const double determinant = (gx - fx) * (hy - fy) - (gy - fy) * (hx - fx);
            // Source order follows CollisionTriangle's (v0-v1)x(v2-v1) upward-facing convention.
            if (!std::isfinite(determinant) || determinant >= -1.0e-12) return HAILSTONE_WORLD_PATCH_DEFERRED;
            const double fu = ((gx - at_x) * (hy - at_y) - (gy - at_y) * (hx - at_x)) / determinant;
            const double fv = ((hx - at_x) * (fy - at_y) - (hy - at_y) * (fx - at_x)) / determinant;
            const double fw = 1.0 - fu - fv;
            const double height = fu * value.source_face[2] + fv * value.source_face[5] + fw * value.source_face[8];
            if (!(fu >= -1.0e-6 && fv >= -1.0e-6 && fw >= -1.0e-6) || !std::isfinite(height) ||
                std::abs(height - value.witness[2]) > std::max(1.0e-4, std::abs(height) * 1.0e-6))
                return HAILSTONE_WORLD_PATCH_DEFERRED;
        } else std::fill_n(value.source_face, 9, 0.0f);
    } else {
        value.flags = 0;
        std::fill_n(value.witness, 3, 0.0f);
        std::fill_n(value.source_face, 9, 0.0f);
        value.region_count = 0;
        std::fill_n(value.region_xy, HAILSTONE_WORLD_PATCH_MAX_REGION_VERTICES * 2, 0.0);
    }
    const auto copied = std::min<std::size_t>(out->size, sizeof(value));
    if (copied < offsetof(HailstoneWorldSurfacePatchResult, source_face) + sizeof(value.source_face))
        value.flags &= ~static_cast<std::uint32_t>(HAILSTONE_WORLD_PATCH_SOURCE_FACE);
    if (copied < offsetof(HailstoneWorldSurfacePatchResult, region_xy) + sizeof(value.region_xy))
        value.flags &= ~static_cast<std::uint32_t>(HAILSTONE_WORLD_PATCH_REGION);
    value.size = static_cast<std::uint32_t>(copied);
    std::memcpy(out, &value, copied);
    return result;
}

bool WorldGeometryHost::submit(std::uint64_t owner, const HailstoneWorldSubmission* submission) {
    if (!owner || !submission || submission->size < sizeof(HailstoneWorldSubmission) ||
        !current(submission->frame, submission->epoch) || !submission->tag ||
        !submission->vertices || !submission->vertex_count ||
        submission->vertex_count > game::kWorldGeometryMaxVertices ||
        submission->primitive > HAILSTONE_WORLD_LINES) return false;
    if (attempts_ == game::kWorldGeometryMaxBatches) return false;
    ++attempts_;
    if (submission->vertex_count > game::kWorldGeometryMaxVertices - inspected_) return false;
    inspected_ += static_cast<std::uint32_t>(submission->vertex_count);
    OwnedWorldBatch batch;
    batch.id = {owner, submission->tag, submission->revision};
    batch.primitive = submission->primitive == HAILSTONE_WORLD_LINES
                          ? game::WorldGeometryPrimitive::Lines : game::WorldGeometryPrimitive::Triangles;
    try {
        batch.vertices.reserve(submission->vertex_count);
        for (std::size_t i = 0; i < submission->vertex_count; ++i) {
            const auto& input = submission->vertices[i];
            game::WorldGeometryVertex vertex;
            std::copy_n(input.position, vertex.position.size(), vertex.position.begin());
            std::copy_n(input.color, vertex.color.size(), vertex.color.begin());
            batch.vertices.push_back(vertex);
        }
        if (budget_.admit(batch.view()) != game::WorldGeometryValidation::Valid) return false;
        batches_.push_back(std::move(batch));
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

void WorldGeometryHost::recorded(std::size_t index, std::uint32_t vertices) {
    if (!active_ || accepting_ || index >= batches_.size()) return;
    auto& batch = batches_[index];
    batch.recorded = vertices != 0 && vertices == batch.vertices.size();
}

std::vector<OwnedWorldReceipt> WorldGeometryHost::finish(bool submitted, bool presented) {
    std::vector<OwnedWorldReceipt> result;
    if (!active_) return result;
    result.reserve(batches_.size());
    for (const auto& batch : batches_) {
        HailstoneWorldReceipt receipt{};
        receipt.size = sizeof(receipt);
        receipt.status = HAILSTONE_WORLD_BATCH_FINISHED;
        receipt.epoch = frame_.epoch;
        receipt.frame = frame_.frame;
        receipt.tag = batch.id.tag;
        receipt.revision = batch.id.revision;
        if (batch.recorded) {
            receipt.flags |= HAILSTONE_WORLD_RECORDED;
            receipt.vertex_count = static_cast<std::uint32_t>(batch.vertices.size());
            if (submitted) receipt.flags |= HAILSTONE_WORLD_SUBMITTED;
            if (submitted && presented) receipt.flags |= HAILSTONE_WORLD_PRESENTED;
        }
        result.push_back({batch.id.owner, receipt});
    }
    active_ = accepting_ = false;
    queries_ = {};
    batches_.clear();
    return result;
}

} // namespace hailstone::plugin
