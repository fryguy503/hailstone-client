#include "nav/nav_mesh.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>

#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include "core/byte_io.h"
#include "core/log.h"
#include "game/collision.h"
#include "game/mechanics/movement.h"

namespace hailstone::nav {

namespace {

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

void fnv(std::uint64_t& h, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        h ^= (v >> (i * 8)) & 0xffu;
        h *= kFnvPrime;
    }
}
void fnv(std::uint64_t& h, float f) { fnv(h, std::bit_cast<std::uint32_t>(f)); }

int border_cells(const BuildParams& p) { return static_cast<int>(std::ceil(p.agent_radius / p.cell_size)) + 3; }

// Detour's nodes per search: enough for a path across the largest zones (the limit is 65535).
constexpr int kQueryNodes = 32768;
constexpr int kMaxPathPolys = 4096;
constexpr int kMaxStraight = 2048;
constexpr float kStartReachDown = 100.0f; // a start off the mesh looks this far up and down for it
constexpr std::size_t kReachLimit = 1u << 20; // polygons a reachability flood visits at most

struct RecastFree {
    void operator()(rcHeightfield* p) const { rcFreeHeightField(p); }
    void operator()(rcCompactHeightfield* p) const { rcFreeCompactHeightfield(p); }
    void operator()(rcContourSet* p) const { rcFreeContourSet(p); }
    void operator()(rcPolyMesh* p) const { rcFreePolyMesh(p); }
    void operator()(rcPolyMeshDetail* p) const { rcFreePolyMeshDetail(p); }
};
template <typename T> using RcPtr = std::unique_ptr<T, RecastFree>;

// Detour's addTile reads the counts in the tile's header and walks the buffer by them unchecked, so data read back
// from the cache is measured against them first, with every polygon's vertex and detail indices, before Detour
// sees it.
//
// The memcpys of Detour's structs out of file bytes are an accepted exception to the rule against casting structs onto
// buffers (AGENTS.md): Detour takes a tile only in its native in-memory layout, so that layout is what the cache
// stores. The file is Hailstone's own cache, written by the client; its header records the native layout
// (native_layout(): Detour version, sizeof(dtPolyRef), byte order) and a checksum, so a file from another layout
// or a damaged one is rebuilt before any of it is read here (docs/navigation.md "Cache").
int align4(std::size_t n) { return static_cast<int>((n + 3) & ~std::size_t{3}); }
bool tile_data_valid(std::span<const unsigned char> data) {
    if (data.size() < sizeof(dtMeshHeader)) return false;
    dtMeshHeader h;
    std::memcpy(&h, data.data(), sizeof h);
    if (h.magic != DT_NAVMESH_MAGIC || h.version != DT_NAVMESH_VERSION) return false;
    for (int c : {h.vertCount, h.polyCount, h.maxLinkCount, h.detailMeshCount, h.detailVertCount, h.detailTriCount,
                  h.bvNodeCount, h.offMeshConCount})
        if (c < 0 || c > (1 << 24)) return false;
    // The ground polygons come first, each with a detail mesh, then one two-vertex polygon per off-mesh link
    // (find_links: drops and swims).
    if (h.maxLinkCount < 1 || h.offMeshBase < 0 || h.detailMeshCount != h.offMeshBase ||
        h.offMeshBase + h.offMeshConCount != h.polyCount)
        return false;
    auto n = [](int c) { return static_cast<std::size_t>(c); };
    std::size_t size = static_cast<std::size_t>(align4(sizeof(dtMeshHeader))) +
                       static_cast<std::size_t>(align4(sizeof(float) * 3 * n(h.vertCount))) +
                       static_cast<std::size_t>(align4(sizeof(dtPoly) * n(h.polyCount))) +
                       static_cast<std::size_t>(align4(sizeof(dtLink) * n(h.maxLinkCount))) +
                       static_cast<std::size_t>(align4(sizeof(dtPolyDetail) * n(h.detailMeshCount))) +
                       static_cast<std::size_t>(align4(sizeof(float) * 3 * n(h.detailVertCount))) +
                       static_cast<std::size_t>(align4(4 * n(h.detailTriCount))) +
                       static_cast<std::size_t>(align4(sizeof(dtBVNode) * n(h.bvNodeCount))) +
                       static_cast<std::size_t>(align4(sizeof(dtOffMeshConnection) * n(h.offMeshConCount)));
    if (size != data.size()) return false;
    std::size_t off = static_cast<std::size_t>(align4(sizeof(dtMeshHeader))) +
                      static_cast<std::size_t>(align4(sizeof(float) * 3 * n(h.vertCount)));
    const std::size_t details_off = off + static_cast<std::size_t>(align4(sizeof(dtPoly) * n(h.polyCount))) +
                                    static_cast<std::size_t>(align4(sizeof(dtLink) * n(h.maxLinkCount)));
    const std::size_t detail_tris_off = details_off +
                                        static_cast<std::size_t>(align4(sizeof(dtPolyDetail) * n(h.detailMeshCount))) +
                                        static_cast<std::size_t>(align4(sizeof(float) * 3 * n(h.detailVertCount)));
    const std::size_t bv_off = detail_tris_off + static_cast<std::size_t>(align4(4 * n(h.detailTriCount)));
    for (int i = h.offMeshBase; i < h.polyCount; ++i) {
        dtPoly poly;
        std::memcpy(&poly, data.data() + off + sizeof(dtPoly) * n(i), sizeof poly);
        if (poly.getType() != DT_POLYTYPE_OFFMESH_CONNECTION || poly.vertCount != 2 || poly.verts[0] >= h.vertCount ||
            poly.verts[1] >= h.vertCount)
            return false;
    }
    for (int i = 0; i < h.offMeshBase; ++i) {
        dtPoly poly;
        std::memcpy(&poly, data.data() + off + sizeof(dtPoly) * n(i), sizeof poly);
        if (poly.getType() != DT_POLYTYPE_GROUND || poly.vertCount < 3 || poly.vertCount > DT_VERTS_PER_POLYGON)
            return false;
        for (int v = 0; v < poly.vertCount; ++v) {
            if (poly.verts[v] >= h.vertCount) return false;
            // A neighbour inside the tile is its index + 1 (connectIntLinks); outside it carries DT_EXT_LINK.
            if (poly.neis[v] && !(poly.neis[v] & DT_EXT_LINK) && poly.neis[v] - 1 >= h.polyCount) return false;
        }
        dtPolyDetail pd;
        std::memcpy(&pd, data.data() + details_off + sizeof(dtPolyDetail) * n(i), sizeof pd);
        // The closest-point queries take the nearest detail edge and expect one: no triangles is no tile.
        if (pd.triCount == 0 || static_cast<std::size_t>(pd.vertBase) + pd.vertCount > n(h.detailVertCount) ||
            static_cast<std::size_t>(pd.triBase) + pd.triCount > n(h.detailTriCount))
            return false;
        // A detail triangle's corners are the polygon's vertices, then its detail vertices. The polygon's outline
        // is made of its triangles' boundary edges (DT_DETAIL_EDGE_BOUNDARY in each edge's two flag bits), which the
        // closest-point queries look for.
        bool boundary = false;
        for (std::size_t t = pd.triBase; t < static_cast<std::size_t>(pd.triBase) + pd.triCount; ++t) {
            for (std::size_t k = 0; k < 3; ++k)
                if (data[detail_tris_off + t * 4 + k] >= poly.vertCount + pd.vertCount) return false;
            for (int e = 0; e < 3; ++e)
                if (dtGetDetailTriEdgeFlags(data[detail_tris_off + t * 4 + 3], e) & DT_DETAIL_EDGE_BOUNDARY) boundary = true;
        }
        if (!boundary) return false;
    }
    // Every coordinate within the world (CollisionMesh's sweeps stop at 100000): a NaN or overflowing distance never
    // wins a comparison, and Detour then follows a null vertex.
    auto in_world = [](float f) { return std::isfinite(f) && std::abs(f) < 1.0e6f; };
    auto finite_floats = [&](std::size_t at, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            float f;
            std::memcpy(&f, data.data() + at + i * sizeof(float), sizeof f);
            if (!in_world(f)) return false;
        }
        return true;
    };
    const std::size_t verts_off = static_cast<std::size_t>(align4(sizeof(dtMeshHeader)));
    const std::size_t detail_verts_off =
        details_off + static_cast<std::size_t>(align4(sizeof(dtPolyDetail) * n(h.detailMeshCount)));
    if (!finite_floats(verts_off, 3 * n(h.vertCount)) || !finite_floats(detail_verts_off, 3 * n(h.detailVertCount)))
        return false;
    for (float f : {h.bmin[0], h.bmin[1], h.bmin[2], h.bmax[0], h.bmax[1], h.bmax[2], h.bvQuantFactor, h.walkableHeight,
                    h.walkableRadius, h.walkableClimb})
        if (!in_world(f)) return false;
    // The bounding-volume tree: a leaf names a polygon, an inner node's escape stays inside the tree.
    for (int i = 0; i < h.bvNodeCount; ++i) {
        dtBVNode node;
        std::memcpy(&node, data.data() + bv_off + sizeof(dtBVNode) * n(i), sizeof node);
        if (node.i >= 0 ? node.i >= h.polyCount : static_cast<long long>(i) - node.i > h.bvNodeCount) return false;
    }
    // The links: each names its own polygon, at finite places.
    const std::size_t links_off = bv_off + static_cast<std::size_t>(align4(sizeof(dtBVNode) * n(h.bvNodeCount)));
    for (int i = 0; i < h.offMeshConCount; ++i) {
        dtOffMeshConnection con;
        std::memcpy(&con, data.data() + links_off + sizeof(dtOffMeshConnection) * n(i), sizeof con);
        if (con.poly != h.offMeshBase + i || !in_world(con.rad) || con.rad < 0.0f) return false;
        for (float f : con.pos)
            if (!in_world(f)) return false;
    }
    return true;
}

} // namespace

std::uint32_t BuildParams::hash() const {
    std::uint64_t h = kFnvOffset;
    for (float f : {cell_size, cell_height, agent_height, agent_radius, agent_climb, max_slope}) fnv(h, f);
    fnv(h, static_cast<std::uint32_t>(tile_cells));
    return static_cast<std::uint32_t>(h ^ (h >> 32));
}

namespace {
// How far outside the terrain's box a triangle may lie and still be taken (InputGeometry::from_collision): Hailstone's
// own number, the zone lines' margin in app/navigator.cpp.
constexpr float kOutsideTerrainMargin = 200.0f;

// Past this many units from the origin a vertex is a damaged mesh's (as assets/ground_refine.cpp and zone_flora.cpp
// take it). A triangle out there is left out: the tile grid is sized from the vertices' bounds, so one stray vertex at
// 1e30 would ask for billions of tiles, and one past int's range would make the cast to a tile index undefined.
constexpr float kMaxNavCoord = 100000.0f;

// The most tiles a zone's grid may have: about 77000 units square at the default 76.8-unit tile. Classic zones need a
// few tens of thousands at most. Geometry spanning more (only possible with a tiny cell size) builds no mesh.
constexpr double kMaxNavTiles = double(1 << 20);

bool sane_triangle(const game::CollisionTriangle& t) {
    for (const glm::vec3* v : {&t.v0, &t.v1, &t.v2})
        if (!(std::abs(v->x) <= kMaxNavCoord && std::abs(v->y) <= kMaxNavCoord && std::abs(v->z) <= kMaxNavCoord))
            return false; // NaN fails too
    return true;
}

// A passable triangle that is a water surface: flat (either side up), water just under its middle and none just
// over it. 1.5 units each way: a surface is drawn on the region's top, and a dock's planks stand higher than that.
bool is_water_surface(const game::CollisionTriangle& t, float min_up, const InputGeometry::WaterTest& water) {
    if (!water || std::abs(t.normal.z) < min_up) return false;
    glm::vec3 c = (t.v0 + t.v1 + t.v2) / 3.0f;
    return water(c - glm::vec3(0.0f, 0.0f, 1.5f)) && !water(c + glm::vec3(0.0f, 0.0f, 1.5f));
}
} // namespace

std::shared_ptr<const InputGeometry> InputGeometry::from_collision(const game::CollisionMesh& mesh, const BuildParams& p,
                                                                   const WaterTest& water, std::vector<AreaBox> boxes) {
    auto geo = std::make_shared<InputGeometry>();
    geo->params_ = p;
    geo->boxes_ = std::move(boxes);
    geo->water_ = water;
    const float min_up = std::cos(glm::radians(p.max_slope));
    geo->verts_.reserve(mesh.triangle_count() * 9);
    geo->areas_.reserve(mesh.triangle_count());
    // Objects wholly outside the terrain's box (with a margin) are left out. Zones park the objects they do not use far
    // under the world: Field of Bone's trees (TREE101_ACTORDEF) stand at z -32768. Nothing walks there, and taken in
    // they stretch the heightfield past Recast's 13-bit span heights (8191 cells, about 2457 units), so every height
    // was clamped near the bottom and the whole mesh lay 32000 units under the zone. Movement still collides with them.
    std::optional<std::pair<glm::vec3, glm::vec3>> keep = mesh.bounds();
    if (keep) {
        keep->first -= glm::vec3(kOutsideTerrainMargin);
        keep->second += glm::vec3(kOutsideTerrainMargin);
    }
    auto outside = [&keep](const game::CollisionTriangle& t) {
        if (!keep) return false;
        glm::vec3 lo = glm::min(t.v0, glm::min(t.v1, t.v2)), hi = glm::max(t.v0, glm::max(t.v1, t.v2));
        return glm::any(glm::lessThan(hi, keep->first)) || glm::any(glm::greaterThan(lo, keep->second));
    };
    for (const game::CollisionTriangle& t : mesh.triangles()) {
        if (!sane_triangle(t) || outside(t)) continue;
        if (t.flags & game::kTriangleLadderSlab) continue; // the movement's stand-in; the mesh keeps the rungs
        // Passable triangles never stop the sweep and are not walked on, but a water surface is swum on: it is
        // ground for the mesh (Water once the areas are marked), so a path can cross water whose bottom is too
        // deep or too steep to walk.
        if (t.flags & game::kTrianglePassable) {
            if (!is_water_surface(t, min_up, water)) continue;
            for (const glm::vec3* v : {&t.v0, &t.v1, &t.v2}) {
                glm::vec3 r = to_recast(*v);
                geo->verts_.insert(geo->verts_.end(), {r.x, r.y, r.z});
            }
            geo->areas_.push_back(RC_WALKABLE_AREA);
            geo->surface_.push_back(1);
            continue;
        }
        for (const glm::vec3* v : {&t.v0, &t.v1, &t.v2}) {
            glm::vec3 r = to_recast(*v);
            geo->verts_.insert(geo->verts_.end(), {r.x, r.y, r.z});
        }
        geo->areas_.push_back(t.normal.z >= min_up ? RC_WALKABLE_AREA : RC_NULL_AREA);
        geo->surface_.push_back(0);
    }
    geo->finish();
    return geo;
}

std::shared_ptr<const InputGeometry> InputGeometry::from_world(std::span<const glm::vec3> vertices,
                                                               std::span<const std::uint8_t> solid, const BuildParams& p,
                                                               const WaterTest& water, std::vector<AreaBox> boxes) {
    std::vector<game::CollisionTriangle> tris;
    for (std::size_t i = 0; i + 2 < vertices.size(); i += 3) {
        bool blocks = i / 3 < solid.size() ? solid[i / 3] != 0 : true;
        tris.push_back(game::CollisionTriangle::make(vertices[i], vertices[i + 1], vertices[i + 2],
                                                     blocks ? std::uint8_t{0} : game::kTrianglePassable));
    }
    return from_collision(game::CollisionMesh::from_triangles(std::move(tris)), p, water, std::move(boxes));
}

void InputGeometry::finish() {
    hash_ = kFnvOffset;
    const std::size_t n = areas_.size();
    if (n == 0) return;
    bmin_ = glm::vec3(std::numeric_limits<float>::max());
    bmax_ = glm::vec3(std::numeric_limits<float>::lowest());
    for (std::size_t i = 0; i < n * 3; ++i) {
        glm::vec3 v(verts_[i * 3], verts_[i * 3 + 1], verts_[i * 3 + 2]);
        bmin_ = glm::min(bmin_, v);
        bmax_ = glm::max(bmax_, v);
    }
    const float ts = params_.tile_size();
    const double span_x = std::ceil(double(bmax_.x - bmin_.x) / double(ts));
    const double span_z = std::ceil(double(bmax_.z - bmin_.z) / double(ts));
    if (!(ts > 0.0f) || !(std::max(1.0, span_x) * std::max(1.0, span_z) <= kMaxNavTiles)) {
        log::warn("nav: geometry spans {} x {} tiles, more than {}; no mesh is built", span_x, span_z, kMaxNavTiles);
        verts_.clear();
        areas_.clear();
        surface_.clear();
        bmin_ = glm::vec3(0.0f);
        bmax_ = glm::vec3(-1.0f);
        return;
    }
    for (float f : verts_) fnv(hash_, f);
    for (std::uint8_t a : areas_) fnv(hash_, static_cast<std::uint32_t>(a));
    for (const AreaBox& b : boxes_) {
        for (float f : {b.lo.x, b.lo.y, b.lo.z, b.hi.x, b.hi.y, b.hi.z}) fnv(hash_, f);
        fnv(hash_, static_cast<std::uint32_t>(b.area));
    }

    tiles_x_ = std::max(1, static_cast<int>(span_x));
    tiles_y_ = std::max(1, static_cast<int>(span_z));
    tiles_.assign(static_cast<std::size_t>(tiles_x_) * static_cast<std::size_t>(tiles_y_), {});
    const float border = static_cast<float>(border_cells(params_)) * params_.cell_size;
    for (std::size_t i = 0; i < n; ++i) {
        const float* t = &verts_[i * 9];
        float x0 = std::min({t[0], t[3], t[6]}) - border, x1 = std::max({t[0], t[3], t[6]}) + border;
        float z0 = std::min({t[2], t[5], t[8]}) - border, z1 = std::max({t[2], t[5], t[8]}) + border;
        int tx0 = std::clamp(static_cast<int>(std::floor((x0 - bmin_.x) / ts)), 0, tiles_x_ - 1);
        int tx1 = std::clamp(static_cast<int>(std::floor((x1 - bmin_.x) / ts)), 0, tiles_x_ - 1);
        int tz0 = std::clamp(static_cast<int>(std::floor((z0 - bmin_.z) / ts)), 0, tiles_y_ - 1);
        int tz1 = std::clamp(static_cast<int>(std::floor((z1 - bmin_.z) / ts)), 0, tiles_y_ - 1);
        for (int ty = tz0; ty <= tz1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx)
                tiles_[static_cast<std::size_t>(ty) * static_cast<std::size_t>(tiles_x_) + static_cast<std::size_t>(tx)]
                    .push_back(static_cast<std::uint32_t>(i));
    }
}

std::span<const std::uint32_t> InputGeometry::tile_triangles(int tx, int ty) const {
    if (tx < 0 || ty < 0 || tx >= tiles_x_ || ty >= tiles_y_) return {};
    return tiles_[static_cast<std::size_t>(ty) * static_cast<std::size_t>(tiles_x_) + static_cast<std::size_t>(tx)];
}

std::span<const std::uint32_t> InputGeometry::triangles_at(float x, float z) const {
    const float ts = params_.tile_size();
    return tile_triangles(static_cast<int>(std::floor((x - bmin_.x) / ts)), static_cast<int>(std::floor((z - bmin_.z) / ts)));
}

bool InputGeometry::in_water(const glm::vec3& r) const { return water_ && water_(to_world(&r.x)); }

namespace {
// The height of triangle t's plane over (x, z) when (x, z) is inside it seen from above (Recast axes, y up).
std::optional<float> height_in(const float* t, float x, float z) {
    float ax = t[0], az = t[2], bx = t[3], bz = t[5], cx = t[6], cz = t[8];
    float d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
    if (std::abs(d) < 1e-9f) return std::nullopt; // seen edge on: a wall
    float u = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / d;
    float v = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / d;
    float w = 1.0f - u - v;
    constexpr float e = -1e-4f;
    if (u < e || v < e || w < e) return std::nullopt;
    return u * t[1] + v * t[4] + w * t[7];
}
} // namespace

std::optional<InputGeometry::Hit> InputGeometry::cast_down(float x, float z, float y_top, float y_bottom) const {
    std::optional<Hit> best;
    for (std::uint32_t i : triangles_at(x, z)) {
        auto y = height_in(triangle(i), x, z);
        if (y && *y <= y_top && *y >= y_bottom && (!best || *y > best->y)) best = Hit{*y, i};
    }
    return best;
}

std::optional<InputGeometry::Hit> InputGeometry::cast_up(float x, float z, float y_bottom, float y_top) const {
    std::optional<Hit> best;
    for (std::uint32_t i : triangles_at(x, z)) {
        auto y = height_in(triangle(i), x, z);
        if (y && *y >= y_bottom && *y <= y_top && (!best || *y < best->y)) best = Hit{*y, i};
    }
    return best;
}

bool InputGeometry::blocked(const glm::vec3& a, const glm::vec3& b) const {
    // Moller-Trumbore against the triangles of the tiles at both ends (a short segment spans at most those).
    auto test = [&](std::span<const std::uint32_t> tris) {
        const glm::vec3 dir = b - a;
        for (std::uint32_t i : tris) {
            if (surface_[i]) continue;
            const float* t = triangle(i);
            glm::vec3 v0(t[0], t[1], t[2]), v1(t[3], t[4], t[5]), v2(t[6], t[7], t[8]);
            glm::vec3 e1 = v1 - v0, e2 = v2 - v0, pv = glm::cross(dir, e2);
            float det = glm::dot(e1, pv);
            if (std::abs(det) < 1e-9f) continue;
            float inv = 1.0f / det;
            glm::vec3 tv = a - v0;
            float u = glm::dot(tv, pv) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            glm::vec3 qv = glm::cross(tv, e1);
            float v = glm::dot(dir, qv) * inv;
            if (v < 0.0f || u + v > 1.0f) continue;
            float k = glm::dot(e2, qv) * inv;
            if (k >= 0.0f && k <= 1.0f) return true;
        }
        return false;
    };
    auto ta = triangles_at(a.x, a.z), tb = triangles_at(b.x, b.z);
    return test(ta) || (tb.data() != ta.data() && test(tb));
}

namespace {

// The highest drop a walk takes onto dry ground: the most the fall-damage formula (game/mechanics/movement.h,
// ApplyFallDamage) forgives a character carrying nothing, falling from rest at the default gravity. Our reading of
// it (docs/unknowns.md): weight shortens it, and the zone's gravity can differ.
float safe_drop() {
    static const float h = [] {
        namespace mech = game::mechanics;
        for (float d = 0.5f; d < 400.0f; d += 0.5f) {
            float speed = std::sqrt(2.0f * mech::kDefaultGravity * d);
            if (-speed > mech::kFallSpeedThreshold) continue; // too slow to land as a fall
            mech::FallDamageInput in;
            in.fall_speed = std::abs(-speed - mech::kFallSpeedThreshold);
            if (mech::apply_fall_damage(in).damage > 0) return d - 0.5f;
        }
        return 0.0f;
    }();
    return h;
}

// Into water: no fall damage (LandingAndFallCheck skips it in water), so as deep as a zone goes (Kedge Keep's way
// in is a shaft of water some 270 deep).
constexpr float kMaxWaterDrop = 2000.0f;
constexpr float kClimbOut = 5.0f;       // a ledge this little over the water is climbed out onto (the swim jump)
constexpr float kLinkSpacing = 5.0f;    // along a ledge
constexpr float kColumnSpacing = 15.0f; // between deep-water floor links
constexpr float kReachFar = 4.0f;       // how far past the agent's radius a ledge's landing is looked for first
constexpr float kReachNear = 1.5f;      // ... and then

struct Links {
    std::vector<float> verts;
    std::vector<float> rads;
    std::vector<unsigned char> dirs, areas;
    std::vector<unsigned short> flags;
    std::vector<unsigned int> ids;
    void add(const glm::vec3& a, const glm::vec3& b, float rad, bool both, Area area, std::uint16_t flag) {
        verts.insert(verts.end(), {a.x, a.y, a.z, b.x, b.y, b.z});
        rads.push_back(rad);
        dirs.push_back(both ? DT_OFFMESH_CON_BIDIR : 0);
        areas.push_back(static_cast<unsigned char>(area));
        flags.push_back(flag);
        ids.push_back(static_cast<unsigned int>(ids.size()));
    }
    int count() const { return static_cast<int>(rads.size()); }
};

// The ways between parts of the mesh that walking does not join (Recast meshes only what is walked), in Recast
// axes:
// - off a ledge (an open polygon edge): when a walk off it lands on walkable ground within the safe drop, or in
//   water (any height up to kMaxWaterDrop), a one-way link from the edge to the landing; both ways when it lands
//   in water no more than kClimbOut under the ledge;
// - from a floor under deep water up to the water's surface over it, both ways (swum).
void find_links(const InputGeometry& geo, const rcPolyMesh& pm, Links& out) {
    const BuildParams& p = geo.params();
    const int nvp = pm.nvp;
    const float r = p.agent_radius;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    auto vert = [&](unsigned short i) {
        const unsigned short* v = &pm.verts[i * 3];
        return glm::vec3(pm.bmin[0] + v[0] * pm.cs, pm.bmin[1] + v[1] * pm.ch, pm.bmin[2] + v[2] * pm.cs);
    };
    std::vector<glm::vec3> columns;
    for (int i = 0; i < pm.npolys; ++i) {
        const unsigned short* poly = &pm.polys[static_cast<std::size_t>(i) * 2 * static_cast<std::size_t>(nvp)];
        if (pm.areas[i] == static_cast<unsigned char>(Area::ZoneLine)) continue;
        int n = 0;
        glm::vec3 centre(0.0f);
        while (n < nvp && poly[n] != RC_MESH_NULL_IDX) centre += vert(poly[n++]);
        if (n < 3) continue;
        centre /= static_cast<float>(n);

        // Under deep water: a link up to the surface over the polygon's middle.
        if (geo.in_water(centre + up * 2.0f)) {
            bool near = std::any_of(columns.begin(), columns.end(), [&](const glm::vec3& c) {
                return glm::length(glm::vec2(c.x - centre.x, c.z - centre.z)) < kColumnSpacing && std::abs(c.y - centre.y) < 6.0f;
            });
            auto top = near ? std::nullopt : geo.cast_up(centre.x, centre.z, centre.y + 1.0f, centre.y + kMaxWaterDrop);
            if (top && geo.water_surface(top->tri) && top->y - centre.y > p.agent_height) {
                bool wet = true;
                for (float y = centre.y + 2.0f; y < top->y - 1.0f && wet; y += 3.0f)
                    wet = geo.in_water({centre.x, y, centre.z});
                if (wet) {
                    columns.push_back(centre);
                    out.add(centre, {centre.x, top->y, centre.z}, r, true, Area::Water, kFlagSwim);
                }
            }
        }

        // Off each open edge.
        for (int j = 0; j < n; ++j) {
            if (poly[nvp + j] != RC_MESH_NULL_IDX) continue; // a neighbour, or a portal to the next tile
            glm::vec3 a = vert(poly[j]), b = vert(poly[(j + 1) % n]);
            glm::vec2 along(b.x - a.x, b.z - a.z);
            float len = glm::length(along);
            if (len < 0.5f) continue;
            glm::vec2 out_dir(along.y / len, -along.x / len);
            glm::vec2 mid((a.x + b.x) * 0.5f - centre.x, (a.z + b.z) * 0.5f - centre.z);
            if (glm::dot(out_dir, mid) < 0.0f) out_dir = -out_dir;
            int samples = std::max(1, static_cast<int>(len / kLinkSpacing));
            for (int s = 0; s < samples; ++s) {
                float t = (static_cast<float>(s) + 0.5f) / static_cast<float>(samples);
                glm::vec3 lip = a + (b - a) * t;
                for (float reach : {r + kReachFar, r + kReachNear}) {
                    glm::vec3 probe(lip.x + out_dir.x * reach, lip.y, lip.z + out_dir.y * reach);
                    // Room to walk out there: nothing across the way at the knees or the head.
                    if (geo.blocked(lip + up * 1.5f, probe + up * 1.5f) ||
                        geo.blocked(lip + up * (p.agent_height - 1.0f), probe + up * (p.agent_height - 1.0f)))
                        continue;
                    auto hit = geo.cast_down(probe.x, probe.z, lip.y + 1.0f, lip.y - kMaxWaterDrop);
                    if (!hit) continue;
                    float drop = lip.y - hit->y;
                    if (drop <= p.agent_climb + 0.5f) break; // the ground goes on: no ledge here
                    if (geo.area(hit->tri) != RC_WALKABLE_AREA) continue;
                    glm::vec3 land(probe.x, hit->y, probe.z);
                    bool water = geo.water_surface(hit->tri) || geo.in_water(land + up * 1.0f);
                    if (!water && drop > safe_drop()) break;
                    // Room to stand where it lands.
                    if (!geo.water_surface(hit->tri) && geo.cast_up(land.x, land.z, land.y + 0.5f, land.y + p.agent_height))
                        continue;
                    // Back up too when the water comes up to within kClimbOut of the ledge: swum up and climbed
                    // out, whether it lands on the surface or on the floor under it.
                    bool both = water && (geo.water_surface(hit->tri) ? drop <= kClimbOut
                                                                      : geo.in_water({probe.x, lip.y - kClimbOut, probe.z}));
                    // Into water it is a swim (water's cost, and left out with water avoided); onto dry ground a drop.
                    if (water) out.add(lip, land, r, both, Area::Water, kFlagSwim);
                    else out.add(lip, land, r, both, Area::Jump, kFlagJump);
                    break;
                }
            }
        }
    }
}

} // namespace

std::vector<unsigned char> build_tile(const InputGeometry& geo, int tx, int ty) {
    auto tris = geo.tile_triangles(tx, ty);
    if (tris.empty()) return {};
    const BuildParams& p = geo.params();

    rcConfig cfg{};
    cfg.cs = p.cell_size;
    cfg.ch = p.cell_height;
    cfg.walkableSlopeAngle = p.max_slope;
    cfg.walkableHeight = static_cast<int>(std::ceil(p.agent_height / p.cell_height));
    cfg.walkableClimb = static_cast<int>(std::floor(p.agent_climb / p.cell_height));
    cfg.walkableRadius = static_cast<int>(std::ceil(p.agent_radius / p.cell_size));
    cfg.maxEdgeLen = static_cast<int>(12.0f / p.cell_size);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = 6;
    cfg.tileSize = p.tile_cells;
    cfg.borderSize = border_cells(p);
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    cfg.detailSampleDist = p.cell_size * 6.0f;
    cfg.detailSampleMaxError = p.cell_height;
    const float ts = p.tile_size();
    const float border = static_cast<float>(cfg.borderSize) * p.cell_size;
    cfg.bmin[0] = geo.bmin().x + static_cast<float>(tx) * ts - border;
    cfg.bmin[1] = geo.bmin().y;
    cfg.bmin[2] = geo.bmin().z + static_cast<float>(ty) * ts - border;
    cfg.bmax[0] = geo.bmin().x + static_cast<float>(tx + 1) * ts + border;
    cfg.bmax[1] = geo.bmax().y;
    cfg.bmax[2] = geo.bmin().z + static_cast<float>(ty + 1) * ts + border;

    rcContext ctx(false);
    RcPtr<rcHeightfield> solid(rcAllocHeightfield());
    if (!solid || !rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) return {};
    std::vector<float> verts;
    std::vector<unsigned char> areas;
    verts.reserve(tris.size() * 9);
    areas.reserve(tris.size());
    for (std::uint32_t i : tris) {
        verts.insert(verts.end(), geo.triangle(i), geo.triangle(i) + 9);
        areas.push_back(geo.area(i));
    }
    if (!rcRasterizeTriangles(&ctx, verts.data(), areas.data(), static_cast<int>(areas.size()), *solid, cfg.walkableClimb))
        return {};
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

    RcPtr<rcCompactHeightfield> chf(rcAllocCompactHeightfield());
    if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf)) return {};
    solid.reset();
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) return {};
    // The area boxes (zone lines), exactly: the polygons split along their edges.
    for (const AreaBox& b : geo.boxes()) {
        float lo[3] = {b.lo.x, b.lo.z, -b.hi.y}, hi[3] = {b.hi.x, b.hi.z, -b.lo.y}; // to_recast of the corners
        if (hi[0] < cfg.bmin[0] || lo[0] > cfg.bmax[0] || hi[2] < cfg.bmin[2] || lo[2] > cfg.bmax[2]) continue;
        rcMarkBoxArea(&ctx, lo, hi, static_cast<unsigned char>(b.area), *chf);
    }
    if (!rcBuildDistanceField(&ctx, *chf)) return {};
    if (!rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea)) return {};

    RcPtr<rcContourSet> cset(rcAllocContourSet());
    if (!cset || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset)) return {};
    if (cset->nconts == 0) return {};
    RcPtr<rcPolyMesh> pmesh(rcAllocPolyMesh());
    if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)) return {};
    RcPtr<rcPolyMeshDetail> dmesh(rcAllocPolyMeshDetail());
    if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh))
        return {};
    if (pmesh->npolys == 0 || pmesh->nverts >= 0xffff) return {};
    for (int i = 0; i < pmesh->npolys; ++i) {
        if (pmesh->areas[i] == static_cast<unsigned char>(Area::ZoneLine)) {
            pmesh->flags[i] = kFlagZoneLine;
            continue;
        }
        pmesh->areas[i] = static_cast<unsigned char>(Area::Ground);
        pmesh->flags[i] = kFlagWalk;
    }

    Links links;
    find_links(geo, *pmesh, links);

    dtNavMeshCreateParams params{};
    if (links.count() > 0) {
        params.offMeshConVerts = links.verts.data();
        params.offMeshConRad = links.rads.data();
        params.offMeshConDir = links.dirs.data();
        params.offMeshConAreas = links.areas.data();
        params.offMeshConFlags = links.flags.data();
        params.offMeshConUserID = links.ids.data();
        params.offMeshConCount = links.count();
    }
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.walkableHeight = p.agent_height;
    params.walkableRadius = p.agent_radius;
    params.walkableClimb = p.agent_climb;
    params.tileX = tx;
    params.tileY = ty;
    params.tileLayer = 0;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;
    unsigned char* data = nullptr;
    int size = 0;
    if (!dtCreateNavMeshData(&params, &data, &size) || !data) return {};
    std::vector<unsigned char> out(data, data + size);
    dtFree(data);
    return out;
}

NavMesh::NavMesh() = default;

NavMesh::~NavMesh() {
    dtFreeNavMeshQuery(query_);
    dtFreeNavMesh(mesh_);
}

bool NavMesh::init(const glm::vec3& rc_origin, float tile_size, int tiles_x, int tiles_y, const BuildParams& params) {
    dtFreeNavMeshQuery(query_);
    dtFreeNavMesh(mesh_);
    query_ = nullptr;
    mesh_ = nullptr;
    tiles_ = 0;
    params_ = params;
    if (tiles_x <= 0 || tiles_y <= 0 || static_cast<long long>(tiles_x) * tiles_y >= (1ll << DT_TILE_BITS)) return false;
    dtNavMeshParams np{};
    np.orig[0] = rc_origin.x;
    np.orig[1] = rc_origin.y;
    np.orig[2] = rc_origin.z;
    np.tileWidth = tile_size;
    np.tileHeight = tile_size;
    np.maxTiles = tiles_x * tiles_y;
    np.maxPolys = 1 << DT_POLY_BITS;
    dtNavMesh* mesh = dtAllocNavMesh();
    if (!mesh || dtStatusFailed(mesh->init(&np))) {
        dtFreeNavMesh(mesh);
        return false;
    }
    dtNavMeshQuery* query = dtAllocNavMeshQuery();
    if (!query || dtStatusFailed(query->init(mesh, kQueryNodes))) {
        dtFreeNavMeshQuery(query);
        dtFreeNavMesh(mesh);
        return false;
    }
    mesh_ = mesh;
    query_ = query;
    return true;
}

bool NavMesh::add_tile(std::span<const unsigned char> data) {
    if (!mesh_ || data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) || !tile_data_valid(data))
        return false;
    // Detour keeps and frees the buffer (DT_TILE_FREE_DATA), so it is Detour's own allocation.
    auto* copy = static_cast<unsigned char*>(dtAlloc(data.size(), DT_ALLOC_PERM));
    if (!copy) return false;
    std::memcpy(copy, data.data(), data.size());
    if (dtStatusFailed(mesh_->addTile(copy, static_cast<int>(data.size()), DT_TILE_FREE_DATA, 0, nullptr))) {
        dtFree(copy);
        return false;
    }
    ++tiles_;
    return true;
}

bool NavMesh::has_tile(int tx, int ty) const { return mesh_ && mesh_->getTileAt(tx, ty, 0) != nullptr; }

void NavMesh::mark_areas(int tx, int ty, const std::function<Area(const glm::vec3&)>& classify, float lift) {
    if (!mesh_) return;
    const dtMeshTile* tile = mesh_->getTileAt(tx, ty, 0);
    if (!tile || !tile->header) return;
    const dtPolyRef base = mesh_->getPolyRefBase(tile);
    for (int i = 0; i < tile->header->polyCount; ++i) {
        const dtPoly& poly = tile->polys[i];
        if (poly.getType() != DT_POLYTYPE_GROUND || poly.vertCount == 0) continue;
        if (poly.getArea() == static_cast<unsigned char>(Area::ZoneLine)) continue; // the builder's, from an AreaBox
        glm::vec3 c(0.0f);
        for (int v = 0; v < poly.vertCount; ++v) c += to_world(&tile->verts[poly.verts[v] * 3]);
        c /= static_cast<float>(poly.vertCount);
        Area a = classify(c + glm::vec3(0.0f, 0.0f, lift));
        // A water surface's polygon has air over it and water just under it.
        if (a == Area::Ground && classify(c - glm::vec3(0.0f, 0.0f, 1.5f)) == Area::Water) a = Area::Water;
        std::uint16_t flags = a == Area::Water      ? kFlagSwim
                              : a == Area::Lava     ? kFlagHazard
                              : a == Area::ZoneLine ? kFlagZoneLine
                              : a == Area::PvP      ? std::uint16_t(kFlagWalk | kFlagPvP)
                                                    : kFlagWalk;
        dtPolyRef ref = base | static_cast<dtPolyRef>(i);
        mesh_->setPolyArea(ref, static_cast<unsigned char>(a));
        mesh_->setPolyFlags(ref, flags);
    }
}

void NavMesh::make_filter(const PathOptions& opt, dtQueryFilter& f) const {
    std::uint16_t include = kFlagWalk | kFlagZoneLine | kFlagJump;
    if (!opt.avoid_water) include |= kFlagSwim;
    if (opt.allow_hazards) include |= kFlagHazard;
    std::uint16_t exclude = kFlagDisabled;
    if (opt.avoid_pvp) exclude |= kFlagPvP;
    f.setIncludeFlags(include);
    f.setExcludeFlags(exclude);
    f.setAreaCost(static_cast<int>(Area::Ground), 1.0f);
    f.setAreaCost(static_cast<int>(Area::PvP), 1.0f);
    f.setAreaCost(static_cast<int>(Area::Water), 3.0f);
    f.setAreaCost(static_cast<int>(Area::Lava), 20.0f);
    f.setAreaCost(static_cast<int>(Area::Jump), 2.0f); // a drop: taken when it saves the long way round
    // A zone line's volume is crossed only when there is no other way (or allowed): stepping in leaves the zone.
    f.setAreaCost(static_cast<int>(Area::ZoneLine), opt.allow_zone_lines ? 1.0f : 1000.0f);
}

std::optional<glm::vec3> NavMesh::nearest(const glm::vec3& world, float reach, float reach_up) const {
    if (!query_) return std::nullopt;
    dtQueryFilter f;
    f.setIncludeFlags(0xffff);
    f.setExcludeFlags(kFlagDisabled);
    glm::vec3 c = to_recast(world);
    const float ext[3] = {reach, reach_up, reach};
    dtPolyRef ref = 0;
    float pt[3];
    if (dtStatusFailed(query_->findNearestPoly(&c.x, ext, &f, &ref, pt)) || ref == 0) return std::nullopt;
    return to_world(pt);
}

namespace {
// How far a goal is from a candidate point on the mesh (Recast axes, y up). Height counts four times as much as
// distance across, so the floor a spawn stands on wins over the one under it, or over it, even where that one is
// nearer in a straight line (a balcony's edge with the hall floor below).
float goal_score(const float* goal, const float* pt) {
    float dx = pt[0] - goal[0], dy = pt[1] - goal[1], dz = pt[2] - goal[2];
    return dx * dx + dz * dz + 16.0f * dy * dy;
}

struct EndCandidate {
    dtPolyRef ref = 0;
    float pt[3] = {};
    float score = 0.0f;
};

// The polygons round a goal, best first by goal_score.
std::vector<EndCandidate> end_candidates(const dtNavMeshQuery& q, const float* goal, const float* ext,
                                         const dtQueryFilter& f) {
    constexpr int kMax = 2048;
    std::vector<dtPolyRef> refs(kMax);
    int n = 0;
    if (dtStatusFailed(q.queryPolygons(goal, ext, &f, refs.data(), &n, kMax))) return {};
    std::vector<EndCandidate> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        EndCandidate c;
        c.ref = refs[static_cast<std::size_t>(i)];
        if (dtStatusFailed(q.closestPointOnPoly(c.ref, goal, c.pt, nullptr))) continue;
        c.score = goal_score(goal, c.pt);
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const EndCandidate& a, const EndCandidate& b) { return a.score < b.score; });
    return out;
}

// Every polygon a walk from `start` can reach under the filter (the mesh's links, flooded), at most `limit` of them.
std::unordered_set<dtPolyRef> reachable_from(const dtNavMesh& mesh, dtPolyRef start, const dtQueryFilter& f,
                                             std::size_t limit) {
    std::unordered_set<dtPolyRef> seen{start};
    std::vector<dtPolyRef> todo{start};
    while (!todo.empty() && seen.size() < limit) {
        dtPolyRef ref = todo.back();
        todo.pop_back();
        const dtMeshTile* tile = nullptr;
        const dtPoly* poly = nullptr;
        if (dtStatusFailed(mesh.getTileAndPolyByRef(ref, &tile, &poly))) continue;
        for (unsigned int l = poly->firstLink; l != DT_NULL_LINK;) {
            // Copied out byte by byte: Detour packs a tile's sections on 4-byte boundaries, so with 64-bit references
            // its links are not aligned for dtLink.
            dtLink link;
            std::memcpy(&link, reinterpret_cast<const unsigned char*>(tile->links) + l * sizeof(dtLink), sizeof link);
            l = link.next;
            dtPolyRef nref = link.ref;
            if (!nref || seen.contains(nref)) continue;
            const dtMeshTile* nt = nullptr;
            const dtPoly* np = nullptr;
            if (dtStatusFailed(mesh.getTileAndPolyByRef(nref, &nt, &np))) continue;
            // dtQueryFilter::passFilter's test (it is not exported when the filter is not virtual).
            if (!(np->flags & f.getIncludeFlags()) || (np->flags & f.getExcludeFlags())) continue;
            seen.insert(nref);
            todo.push_back(nref);
        }
    }
    return seen;
}
} // namespace

PathResult NavMesh::find_path(const glm::vec3& from, const glm::vec3& to, const PathOptions& opt) const {
    PathResult r;
    if (!query_) return r;
    dtQueryFilter f;
    make_filter(opt, f);
    const float ext[3] = {4.0f, 12.0f, 4.0f};
    glm::vec3 s = to_recast(from), e = to_recast(to);
    dtPolyRef sref = 0;
    float spt[3];
    if (dtStatusFailed(query_->findNearestPoly(&s.x, ext, &f, &sref, spt)) || !sref) {
        // Off the mesh at the start (swimming high over the floor, or falling): from the nearest polygon further
        // off, mostly under it. Kedge Keep's arrivals float some 26 over the floor of its entry room.
        const float far[3] = {8.0f, kStartReachDown, 8.0f};
        if (dtStatusFailed(query_->findNearestPoly(&s.x, far, &f, &sref, spt)) || !sref) return r;
    }

    // Where the goal stands: the best polygon round it, by goal_score. Off the mesh (nothing in the near box), the
    // walk goes as near as it can.
    const float wide[3] = {32.0f, 64.0f, 32.0f};
    std::vector<EndCandidate> ends = end_candidates(*query_, &e.x, ext, f);
    bool ends_wide = ends.empty();
    if (ends_wide) {
        ends = end_candidates(*query_, &e.x, wide, f);
        if (ends.empty()) return r;
        r.partial = true;
    }
    EndCandidate end = ends.front();

    std::vector<dtPolyRef> polys(kMaxPathPolys);
    int npolys = 0;
    dtStatus st = query_->findPath(sref, end.ref, spt, end.pt, &f, polys.data(), &npolys, kMaxPathPolys);
    if (dtStatusFailed(st) || npolys == 0) return r;
    if (polys[static_cast<std::size_t>(npolys - 1)] != end.ref) {
        // The goal's floor is not joined to the start's. Detour's own fallback stops at the polygon nearest the goal
        // in a straight line, which in a building is often the floor right under (or over) it, where the walk would
        // stand below the spawn and never get to it. The reachable polygon round the goal that scores best is
        // usually on the right floor, short of it, or else at least the way up.
        r.partial = true;
        std::unordered_set<dtPolyRef> reach = reachable_from(*mesh_, sref, f, kReachLimit);
        if (!ends_wide) ends = end_candidates(*query_, &e.x, wide, f);
        auto alt = std::find_if(ends.begin(), ends.end(), [&](const EndCandidate& c) { return reach.contains(c.ref); });
        if (alt != ends.end()) {
            end = *alt;
            st = query_->findPath(sref, end.ref, spt, end.pt, &f, polys.data(), &npolys, kMaxPathPolys);
            if (dtStatusFailed(st) || npolys == 0) return r;
        }
    }
    float stop[3] = {end.pt[0], end.pt[1], end.pt[2]};
    if (polys[static_cast<std::size_t>(npolys - 1)] != end.ref) {
        r.partial = true;
        query_->closestPointOnPoly(polys[static_cast<std::size_t>(npolys - 1)], end.pt, stop, nullptr);
    }
    if (st & DT_PARTIAL_RESULT) r.partial = true;
    std::vector<float> straight(static_cast<std::size_t>(kMaxStraight) * 3);
    std::vector<unsigned char> sflags(static_cast<std::size_t>(kMaxStraight));
    std::vector<dtPolyRef> srefs(static_cast<std::size_t>(kMaxStraight));
    int nstraight = 0;
    if (dtStatusFailed(query_->findStraightPath(spt, stop, polys.data(), npolys, straight.data(), sflags.data(),
                                                srefs.data(), &nstraight, kMaxStraight)))
        return r;
    r.points.reserve(static_cast<std::size_t>(nstraight) + 8);
    // Which points are bends on the ground, free to move off a wall: not the ends, not either end of a link (a drop,
    // a swim), not the points added for a swim.
    std::vector<bool> movable;
    movable.reserve(static_cast<std::size_t>(nstraight) + 8);
    for (int i = 0; i < nstraight; ++i) {
        const auto k = static_cast<std::size_t>(i);
        glm::vec3 p = to_world(&straight[k * 3]);
        r.points.push_back(p);
        bool link = (sflags[k] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) || (i > 0 && (sflags[k - 1] & DT_STRAIGHTPATH_OFFMESH_CONNECTION));
        movable.push_back(i > 0 && i + 1 < nstraight && !link);
        // A swim link (between a ledge and the water beside or under it, or a deep floor and its surface): by way of
        // the open column it was found in, straight up or down there, then across. The straight line between its
        // ends runs under the ledge, into the floor of the room over the water.
        if ((sflags[k] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) && i + 1 < nstraight) {
            const dtMeshTile* tile = nullptr;
            const dtPoly* poly = nullptr;
            if (dtStatusSucceed(mesh_->getTileAndPolyByRef(srefs[k], &tile, &poly)) &&
                poly->getArea() == static_cast<unsigned char>(Area::Water)) {
                glm::vec3 q = to_world(&straight[(k + 1) * 3]);
                if (std::abs(q.z - p.z) > params_.agent_climb && glm::length(glm::vec2(q - p)) > 0.5f) {
                    const glm::vec3& low = q.z < p.z ? q : p;
                    const glm::vec3& high = q.z < p.z ? p : q;
                    r.points.push_back({low.x, low.y, high.z});
                    movable.push_back(false);
                }
            }
        }
    }
    if (opt.wall_clearance > 0.0f) keep_from_walls(r.points, movable, opt.wall_clearance, f);
    return r;
}

void NavMesh::keep_from_walls(std::vector<glm::vec3>& points, const std::vector<bool>& movable, float clearance,
                              const dtQueryFilter& f) const {
    const float ext[3] = {1.0f, 4.0f, 1.0f};
    // The polygon under a point, and whether the straight line from it to `to` stays on the mesh.
    auto on_mesh = [&](const glm::vec3& w, dtPolyRef& ref, float* pt) {
        glm::vec3 r = to_recast(w);
        return dtStatusSucceed(query_->findNearestPoly(&r.x, ext, &f, &ref, pt)) && ref != 0;
    };
    // A path's points lie on the mesh's edge, and a ray that starts on an edge can stop at once; a wall in the way
    // stops it from either end, so the line is clear when it passes one way or the other.
    auto one_way = [&](const glm::vec3& a, const glm::vec3& b) {
        dtPolyRef ref = 0;
        float pt[3];
        if (!on_mesh(a, ref, pt)) return false;
        glm::vec3 e = to_recast(b);
        float t = 0.0f, normal[3];
        dtPolyRef visited[256];
        int nvisited = 0;
        return dtStatusSucceed(query_->raycast(ref, pt, &e.x, &f, &t, normal, visited, &nvisited, 256)) && t >= 1.0f;
    };
    auto clear_line = [&](const glm::vec3& a, const glm::vec3& b) { return one_way(a, b) || one_way(b, a); };
    auto wall = [&](dtPolyRef ref, const float* pt, float& dist, float* normal) {
        float hit[3];
        return dtStatusSucceed(query_->findDistanceToWall(ref, pt, clearance, &f, &dist, hit, normal));
    };
    // The turns are read from the path as found: a corner the mesh rounds off into two bends close together moves
    // out as a whole, the second not left on the edge because the first has moved.
    const std::vector<glm::vec3> found = points;
    for (std::size_t i = 1; i + 1 < points.size(); ++i) {
        if (i >= movable.size() || !movable[i]) continue;
        dtPolyRef ref = 0;
        float pt[3];
        if (!on_mesh(points[i], ref, pt)) continue;
        float dist = 0.0f, normal[3];
        if (!wall(ref, pt, dist, normal) || dist >= clearance) continue;
        // Out from the turn: a bend wraps round what is on the inside of the turn, so away from it is the turn's
        // outside (the bisector of the way in and the way out, level). The point is often on the mesh's edge, where
        // the nearest wall gives no direction.
        glm::vec2 in = glm::vec2(found[i] - found[i - 1]), out = glm::vec2(found[i + 1] - found[i]);
        if (glm::length(in) < 0.01f || glm::length(out) < 0.01f) continue;
        glm::vec2 away = glm::normalize(in) - glm::normalize(out);
        if (glm::length(away) < 0.05f) continue; // hardly a bend
        away = glm::normalize(away);
        glm::vec3 dir = to_recast(glm::vec3(away, 0.0f)); // (a direction: the same axis swap)
        // The whole way, or half.
        for (float share : {1.0f, 0.5f}) {
            float push = (clearance - dist) * share;
            float c[3] = {pt[0] + dir.x * push, pt[1], pt[2] + dir.z * push};
            dtPolyRef cref = 0;
            float cpt[3];
            glm::vec3 cw = to_world(c);
            if (!on_mesh(cw, cref, cpt)) continue;
            glm::vec3 moved = to_world(cpt);
            float cdist = 0.0f, cnormal[3];
            if (!wall(cref, cpt, cdist, cnormal)) continue;
            if (cdist + 0.01f < dist) continue; // nearer another wall (a narrow way): no better
            if (!clear_line(points[i - 1], moved) || !clear_line(moved, points[i + 1])) continue;
            points[i] = moved;
            break;
        }
    }
}

bool NavMesh::walkable_line(const glm::vec3& from, const glm::vec3& to, const PathOptions& opt) const {
    if (!query_) return false;
    dtQueryFilter f;
    make_filter(opt, f);
    const float ext[3] = {4.0f, 12.0f, 4.0f};
    glm::vec3 s = to_recast(from), e = to_recast(to);
    dtPolyRef sref = 0;
    float spt[3];
    if (dtStatusFailed(query_->findNearestPoly(&s.x, ext, &f, &sref, spt)) || !sref) return false;
    float t = 0.0f, normal[3];
    dtPolyRef visited[256];
    int nvisited = 0;
    if (dtStatusFailed(query_->raycast(sref, spt, &e.x, &f, &t, normal, visited, &nvisited, 256))) return false;
    return t >= 1.0f;
}

std::optional<float> NavMesh::walkable_part(const glm::vec3& from, const glm::vec3& to, const PathOptions& opt) const {
    if (!query_) return std::nullopt;
    dtQueryFilter f;
    make_filter(opt, f);
    const float ext[3] = {4.0f, 12.0f, 4.0f};
    glm::vec3 s = to_recast(from), e = to_recast(to);
    dtPolyRef sref = 0;
    float spt[3];
    if (dtStatusFailed(query_->findNearestPoly(&s.x, ext, &f, &sref, spt)) || !sref) return std::nullopt;
    float t = 0.0f, normal[3];
    dtPolyRef visited[256];
    int nvisited = 0;
    if (dtStatusFailed(query_->raycast(sref, spt, &e.x, &f, &t, normal, visited, &nvisited, 256))) return std::nullopt;
    return std::min(t, 1.0f);
}

void NavMesh::for_each_link(const std::function<void(const glm::vec3&, const glm::vec3&, bool)>& fn) const {
    if (!mesh_) return;
    const dtNavMesh& mesh = *mesh_;
    for (int t = 0; t < mesh.getMaxTiles(); ++t) {
        const dtMeshTile* tile = mesh.getTile(t);
        if (!tile || !tile->header) continue;
        for (int i = 0; i < tile->header->offMeshConCount; ++i) {
            const dtOffMeshConnection& c = tile->offMeshCons[i];
            fn(to_world(&c.pos[0]), to_world(&c.pos[3]), (c.flags & DT_OFFMESH_CON_BIDIR) != 0);
        }
    }
}

void NavMesh::for_each_polygon(const std::function<void(std::span<const glm::vec3>, Area)>& fn) const {
    if (!mesh_) return;
    const dtNavMesh& mesh = *mesh_;
    std::vector<glm::vec3> outline;
    for (int t = 0; t < mesh.getMaxTiles(); ++t) {
        const dtMeshTile* tile = mesh.getTile(t);
        if (!tile || !tile->header) continue;
        for (int i = 0; i < tile->header->polyCount; ++i) {
            const dtPoly& poly = tile->polys[i];
            if (poly.getType() != DT_POLYTYPE_GROUND) continue;
            outline.clear();
            for (int v = 0; v < poly.vertCount; ++v) outline.push_back(to_world(&tile->verts[poly.verts[v] * 3]));
            fn(outline, static_cast<Area>(poly.getArea()));
        }
    }
}

namespace {
constexpr std::uint32_t kCacheMagic = 0x4d4e5348; // "HSNM" read little-endian
// The file's layout (4: the builder's fingerprint). A change in what the builder makes is caught by the
// fingerprint (builder_fingerprint); raising this as well does no harm when a change might slip past its scene.
constexpr std::uint32_t kCacheVersion = 4;
// FNV-1a over the bytes before it: a damaged file is refused before any tile reaches Detour.
std::uint64_t checksum(std::span<const std::byte> b) {
    std::uint64_t h = kFnvOffset;
    for (std::byte x : b) {
        h ^= static_cast<std::uint64_t>(x);
        h *= kFnvPrime;
    }
    return h;
}
// The native layout Detour's tile data is in: its version, the polygon reference size and the byte order.
std::uint32_t native_layout() {
    std::uint32_t little = std::endian::native == std::endian::little ? 1u : 0u;
    return static_cast<std::uint32_t>(DT_NAVMESH_VERSION) | static_cast<std::uint32_t>(sizeof(dtPolyRef)) << 16 |
           little << 24;
}
} // namespace

std::shared_ptr<const InputGeometry> fingerprint_scene() {
    std::vector<glm::vec3> v;
    std::vector<std::uint8_t> solid;
    auto tri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, bool blocks = true) {
        // Floors face up (CollisionTriangle::make's normal is cross(a - b, c - b)).
        if (glm::cross(a - b, c - b).z < 0.0f) std::swap(b, c);
        v.insert(v.end(), {a, b, c});
        solid.push_back(blocks ? 1 : 0);
    };
    auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
        v.insert(v.end(), {a, b, c, a, c, d});
        solid.insert(solid.end(), {1, 1});
    };
    auto floor = [&](float x0, float y0, float x1, float y1, float z, bool blocks = true) {
        tri({x0, y0, z}, {x1, y0, z}, {x1, y1, z}, blocks);
        tri({x0, y0, z}, {x1, y1, z}, {x0, y1, z}, blocks);
    };
    auto box = [&](float x0, float y0, float x1, float y1, float z0, float z1) {
        quad({x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1});
        quad({x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1});
        quad({x1, y1, z0}, {x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1});
        quad({x0, y1, z0}, {x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1});
        floor(x0, y0, x1, y1, z1);
    };
    auto pit = [&](float x0, float x1, float bottom) { // sheer sides from 0 down to `bottom`, y 0..40
        floor(x0, 0, x1, 40, bottom);
        quad({x0, 0, bottom}, {x0, 40, bottom}, {x0, 40, 0}, {x0, 0, 0});
        quad({x1, 0, bottom}, {x1, 40, bottom}, {x1, 40, 0}, {x1, 0, 0});
    };
    floor(0, 0, 60, 40, 0);
    box(5, 5, 20, 20, 0, 12);    // a ledge: dropped off
    box(30, 5, 40, 15, 0, 3.6f); // a step a little over the old climb
    pit(60, 100, -30);           // a deep pool with a drawn surface: its floor joined to the surface
    floor(100, 0, 140, 40, 0);
    pit(140, 180, -10);          // a flooded room with no surface: dropped into, climbed out of
    floor(180, 0, 220, 40, 0);
    floor(60, 0, 100, 40, -1, false); // the pool's drawn surface (passable)
    auto water = [](const glm::vec3& p) {
        return p.y >= 0 && p.y <= 40 && p.z <= -1 && ((p.x >= 60 && p.x <= 100 && p.z >= -30) || (p.x >= 140 && p.x <= 180 && p.z >= -10));
    };
    std::vector<AreaBox> boxes{{glm::vec3(45, 25, -2), glm::vec3(55, 35, 10), Area::ZoneLine}};
    return InputGeometry::from_world(v, solid, BuildParams{}, water, std::move(boxes));
}

std::uint64_t builder_fingerprint() {
    static const std::uint64_t fingerprint = [] {
        auto geo = fingerprint_scene();
        std::uint64_t h = kFnvOffset;
        fnv(h, kCacheVersion);
        // The links' numbers too: a small change to one may not show in the scene and still change real zones.
        for (float f : {safe_drop(), kMaxWaterDrop, kClimbOut, kLinkSpacing, kColumnSpacing, kReachFar, kReachNear})
            fnv(h, f);
        for (int ty = 0; ty < geo->tiles_y(); ++ty)
            for (int tx = 0; tx < geo->tiles_x(); ++tx)
                for (unsigned char b : build_tile(*geo, tx, ty)) fnv(h, static_cast<std::uint32_t>(b));
        return h;
    }();
    return fingerprint;
}

std::vector<std::byte> encode_cache(const CacheHeader& h, std::span<const CachedTile> tiles) {
    ByteWriter w;
    w.le<std::uint32_t>(kCacheMagic);
    w.le<std::uint32_t>(kCacheVersion);
    w.le<std::uint32_t>(native_layout());
    w.le<std::uint64_t>(h.geometry_hash);
    w.le<std::uint32_t>(h.params_hash);
    w.le<std::uint64_t>(h.builder);
    w.f32(h.origin.x);
    w.f32(h.origin.y);
    w.f32(h.origin.z);
    w.f32(h.tile_size);
    w.le<std::int32_t>(h.tiles_x);
    w.le<std::int32_t>(h.tiles_y);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(tiles.size()));
    for (const CachedTile& t : tiles) {
        w.le<std::int32_t>(t.tx);
        w.le<std::int32_t>(t.ty);
        w.le<std::uint32_t>(static_cast<std::uint32_t>(t.data.size()));
        w.bytes(std::as_bytes(std::span(t.data)));
    }
    w.le<std::uint64_t>(checksum(w.data()));
    return w.take();
}

std::optional<std::vector<CachedTile>> decode_cache(std::span<const std::byte> bytes, const CacheHeader& expect) {
    try {
        if (bytes.size() < 8) return std::nullopt;
        ByteReader sum(bytes.data() + bytes.size() - 8, 8);
        if (sum.le<std::uint64_t>() != checksum(bytes.first(bytes.size() - 8))) return std::nullopt;
        bytes = bytes.first(bytes.size() - 8);
        ByteReader r(bytes.data(), bytes.size());
        if (r.le<std::uint32_t>() != kCacheMagic || r.le<std::uint32_t>() != kCacheVersion ||
            r.le<std::uint32_t>() != native_layout())
            return std::nullopt;
        CacheHeader h;
        h.geometry_hash = r.le<std::uint64_t>();
        h.params_hash = r.le<std::uint32_t>();
        h.builder = r.le<std::uint64_t>();
        h.origin.x = r.f32();
        h.origin.y = r.f32();
        h.origin.z = r.f32();
        h.tile_size = r.f32();
        h.tiles_x = r.le<std::int32_t>();
        h.tiles_y = r.le<std::int32_t>();
        if (h.geometry_hash != expect.geometry_hash || h.params_hash != expect.params_hash || h.builder != expect.builder ||
            h.origin != expect.origin ||
            h.tile_size != expect.tile_size || h.tiles_x != expect.tiles_x || h.tiles_y != expect.tiles_y)
            return std::nullopt;
        std::uint32_t count = r.le<std::uint32_t>();
        if (count > static_cast<std::uint64_t>(h.tiles_x) * static_cast<std::uint64_t>(h.tiles_y)) return std::nullopt;
        std::vector<CachedTile> tiles;
        tiles.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            CachedTile t;
            t.tx = r.le<std::int32_t>();
            t.ty = r.le<std::int32_t>();
            if (t.tx < 0 || t.ty < 0 || t.tx >= h.tiles_x || t.ty >= h.tiles_y) return std::nullopt;
            std::uint32_t size = r.le<std::uint32_t>();
            auto data = r.bytes(size);
            t.data.resize(size);
            if (size) std::memcpy(t.data.data(), data.data(), size);
            tiles.push_back(std::move(t));
        }
        if (!r.empty()) return std::nullopt;
        return tiles;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace hailstone::nav
