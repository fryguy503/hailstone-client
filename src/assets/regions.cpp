#include "assets/regions.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include <glm/gtc/matrix_transform.hpp>

#include "core/log.h"
#include "core/saturate.h"

namespace hailstone::assets {

namespace {

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// The type byte, as the original's and/or pair (flags & (0xffffff00 | type)) | type.
std::uint32_t set_type(std::uint32_t flags, std::uint32_t type) { return (flags & 0xffffff00u) | type; }

// atof/atol on a fixed-width field: leading blanks, a sign, digits and an
// optional fraction; stops at the first other character (C library rules,
// without locale or exponent handling, which the tags never use).
double c_number(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    bool negative = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) negative = s[i++] == '-';
    double v = 0.0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) v = v * 10.0 + (s[i] - '0');
    if (i < s.size() && s[i] == '.') {
        double scale = 0.1;
        for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, scale *= 0.1) v += (s[i] - '0') * scale;
    }
    return negative ? -v : v;
}

std::string_view field(std::string_view s, std::size_t pos, std::size_t len) {
    return pos >= s.size() ? std::string_view() : s.substr(pos, len);
}

} // namespace

std::uint32_t region_flags_from_tag(std::string_view name, std::uint32_t flags) {
    // ParseRegionNameFlags @0x487530.
    if (name.size() < 4) return flags;
    if (name[0] == 'A') {
        if (starts_with(name, "AWT")) return set_type(flags, 5);
        if (starts_with(name, "ALV")) return set_type(flags, 7);
        if (starts_with(name, "AVW")) return set_type(flags, 8);
        if (starts_with(name, "APK")) return flags | kRegionPvP;
        if (starts_with(name, "ATP")) return flags | kRegionTeleport;
        if (starts_with(name, "ASL")) return flags | kRegionSlippery;
        return flags;
    }
    if (starts_with(name, "DR")) flags &= 0xffffff00u;
    else if (starts_with(name, "WT")) flags = set_type(flags, 5);
    else if (starts_with(name, "LA")) flags = set_type(flags, 7);
    else if (starts_with(name, "SL")) flags = set_type(flags, 6);
    else if (starts_with(name, "VW")) flags = set_type(flags, 8);
    else if (starts_with(name, "W2")) flags = set_type(flags, 9);
    else if (starts_with(name, "W3")) flags = set_type(flags, 10);
    if (name[2] == 'P') flags |= kRegionPvP;
    if (name[3] == 'T' && name.size() > 4 && name[4] == 'P') flags |= kRegionTeleport;
    if (name.size() <= 0x20) return flags;
    if (name[0x1f] == 'M') flags |= kRegionFlagM;
    if (name[0x20] == 'S') flags |= kRegionSlippery;
    if (name.size() > 0x21) {
        if (name[0x21] == 'P') flags |= kRegionFlagP;
        else if (name[0x21] == 'F') flags |= kRegionFlagF;
    }
    return flags;
}

std::optional<ZoneLineTarget> decode_zone_line(std::string_view tag) {
    if (!(region_flags_from_tag(tag) & kRegionTeleport)) return std::nullopt;
    ZoneLineTarget t;
    if (tag[0] == 'A') {
        // ATP: the client formats "DRNTP00255%05d000000000000000" with atoi(name + 4), so the zone point read
        // back from the text is 10 x that number (checked against the recorded vectors).
        // A number past int's range in a forged tag is clamped (atoi's overflow is undefined; docs/unknowns.md) so
        // that times 10 stays in range.
        t.reference = true;
        t.zone_point = saturate_cast<std::int32_t>(std::clamp(c_number(field(tag, 4, std::string_view::npos)),
                                                              -214748364.0, 214748364.0)) * 10;
        return t;
    }
    // Teleport_ResolveTeleportIndex @0x506630: each field is read only when the
    // name is long enough to hold it.
    std::size_t n = tag.size();
    double zone = n > 9 ? c_number(field(tag, 5, 5)) : 0.0;
    if (zone == 255.0) {
        t.reference = true;
        t.zone_point = n > 15 ? saturate_cast<std::int32_t>(c_number(field(tag, 10, 6))) : -1;
        return t;
    }
    t.zone_id = saturate_cast<std::uint32_t>(zone); // a negative zone (a forged tag) is 0, no zone
    if (n > 15) t.server_y = static_cast<float>(c_number(field(tag, 10, 6)));
    if (n > 21) t.server_x = static_cast<float>(c_number(field(tag, 16, 6)));
    if (n > 27) t.server_z = static_cast<float>(c_number(field(tag, 22, 6)));
    if (n > 30) t.heading = static_cast<float>(c_number(field(tag, 28, 3)));
    return t;
}

ZoneRegions ZoneRegions::build(std::vector<wld::BspNode> nodes, std::size_t region_count,
                               const std::vector<wld::RegionTag>& tags) {
    ZoneRegions z;
    z.nodes_ = std::move(nodes);
    z.regions_.resize(region_count);
    for (const auto& t : tags) {
        auto index = static_cast<std::int32_t>(z.tags_.size());
        z.tags_.push_back(t.tag);
        for (auto r : t.regions) {
            if (r >= region_count) {
                log::debug("region tag {} names region {} of {}", t.tag, r + 1, region_count);
                continue;
            }
            z.regions_[r].flags = region_flags_from_tag(t.tag, z.regions_[r].flags);
            z.regions_[r].tag = index;
        }
    }
    return z;
}

ZoneRegions ZoneRegions::from_boxes(const std::vector<RegionBox>& boxes) {
    ZoneRegions z;
    for (const auto& b : boxes) {
        // M = T(centre) * Rz(a0) * Ry(-a1) * Rx(a2) * S(extents), angles in
        // 512ths of a turn after truncation toward zero (0x100c27f0).
        auto turn = [](float a) {
            if (!std::isfinite(a) || std::fabs(a) > 2.0e9f) return 0.0f;
            auto q = static_cast<std::int32_t>(a) & 511;
            return static_cast<float>(q) * 2.0f * std::numbers::pi_v<float> / 512.0f;
        };
        glm::mat4 m = glm::translate(glm::mat4(1.0f), b.center);
        m = glm::rotate(m, turn(b.angles.x), glm::vec3(0, 0, 1));
        m = glm::rotate(m, turn(-b.angles.y), glm::vec3(0, 1, 0));
        m = glm::rotate(m, turn(b.angles.z), glm::vec3(1, 0, 0));
        // The shared box constructor (0x10021de0) gives AFG boxes the larger
        // horizontal extent on both axes (OpenEQ docs/EQG_AFG_REGIONS.md).
        glm::vec3 extents = b.extents;
        if (starts_with(b.name, "AFG")) extents.x = extents.y = std::max(extents.x, extents.y);
        m = glm::scale(m, extents);
        Box box;
        box.skip = starts_with(b.name, "APV");
        float det = glm::determinant(m);
        if (!std::isfinite(det) || std::fabs(det) < 1e-12f) {
            log::debug("region box {} is degenerate", b.name);
            box.skip = true;
            box.degenerate = true;
        } else {
            box.world_to_box = glm::inverse(m);
        }
        z.boxes_.push_back(box);
        Entry e;
        e.flags = region_flags_from_tag(b.name);
        e.tag = static_cast<std::int32_t>(z.tags_.size());
        z.tags_.push_back(b.name);
        z.regions_.push_back(e);
    }
    return z;
}

ZoneRegions ZoneRegions::from_wld(const wld::WldFile& wld) {
    const wld::BspTree* tree = nullptr;
    std::size_t region_count = 0;
    std::vector<wld::RegionTag> tags;
    for (const auto& f : wld.fragments()) {
        if (const auto* t = f.get<wld::BspTree>()) {
            if (!tree) tree = t;
        } else if (f.type == 0x22) {
            ++region_count; // counted even if it failed to parse, so numbering stays intact
        } else if (const auto* g = f.get<wld::RegionTag>()) {
            tags.push_back(*g);
        }
    }
    if (!tree) return {};
    return build(tree->nodes, region_count, tags);
}

std::uint32_t ZoneRegions::region_at(const glm::vec3& p, std::string_view prefix) const {
    if (!boxes_.empty()) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return 0;
        auto inside = [&](std::size_t i) {
            glm::vec3 l(boxes_[i].world_to_box * glm::vec4(p, 1.0f));
            return std::fabs(l.x) <= 1.0f && std::fabs(l.y) <= 1.0f && std::fabs(l.z) <= 1.0f;
        };
        // strncmp(name, prefix, 3) == 0: equal up to 3 characters or a shared end.
        auto prefix_matches = [&](const std::string& name) {
            for (std::size_t k = 0; k < 3; ++k) {
                char a = k < name.size() ? name[k] : '\0';
                char b = k < prefix.size() ? prefix[k] : '\0';
                if (a != b) return false;
                if (a == '\0') return true;
            }
            return true;
        };
        if (!prefix.empty())
            for (std::size_t i = 0; i < boxes_.size(); ++i) {
                if (boxes_[i].degenerate) continue;
                const auto& name = tags_[static_cast<std::size_t>(regions_[i].tag)];
                if (prefix_matches(name) && inside(i)) return static_cast<std::uint32_t>(i + 1);
            }
        for (std::size_t i = 0; i < boxes_.size(); ++i)
            if (!boxes_[i].skip && inside(i)) return static_cast<std::uint32_t>(i + 1);
        return 0;
    }
    if (nodes_.empty() || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return 0;
    std::size_t index = 0;
    // A well-formed tree is never deeper than its node count; the cap stops a cyclic one.
    for (std::size_t steps = 0; steps <= nodes_.size(); ++steps) {
        const auto& n = nodes_[index];
        if (n.front == 0 && n.back == 0) return n.region <= regions_.size() ? n.region : 0;
        float d = glm::dot(n.normal, p) + n.distance;
        if (d == 0.0f) return 0;
        std::uint32_t child = d > 0.0f ? n.front : n.back;
        if (child == 0 || child > nodes_.size()) return 0;
        index = child - 1;
    }
    return 0;
}

bool ZoneRegions::in_world(const glm::vec3& p) const {
    if (nodes_.empty()) return true;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    std::size_t index = 0;
    for (std::size_t steps = 0; steps <= nodes_.size(); ++steps) {
        const auto& n = nodes_[index];
        if (n.front == 0 && n.back == 0) return n.region != 0;
        std::uint32_t child = glm::dot(n.normal, p) + n.distance >= 0.0f ? n.front : n.back;
        if (child == 0 || child > nodes_.size()) return false;
        index = child - 1;
    }
    return false;
}

RegionInfo ZoneRegions::info(std::uint32_t region) const {
    RegionInfo info;
    if (region == 0 || region > regions_.size()) return info;
    const Entry& e = regions_[region - 1];
    info.region = region;
    info.flags = e.flags;
    if (e.tag >= 0) {
        info.tag = tags_[static_cast<std::size_t>(e.tag)];
        if (e.flags & kRegionTeleport) info.zone_line = decode_zone_line(info.tag);
    }
    return info;
}

namespace {

// A convex polytope as its faces' polygons, cut by planes one after another (ZoneRegions::leaf_cells).
using Face = std::vector<glm::dvec3>;

std::vector<Face> box_polytope(const glm::dvec3& lo, const glm::dvec3& hi) {
    auto c = [&](int i) { return glm::dvec3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z); };
    return {{c(0), c(2), c(3), c(1)}, {c(4), c(5), c(7), c(6)}, {c(0), c(1), c(5), c(4)},
            {c(2), c(6), c(7), c(3)}, {c(0), c(4), c(6), c(2)}, {c(1), c(3), c(7), c(5)}};
}

// Keeps the part with n . p + d >= 0; false when nothing of it is left.
bool cut_polytope(std::vector<Face>& faces, const glm::dvec3& n, double d) {
    const double len = glm::length(n);
    if (!(len > 0.0)) return true;
    const glm::dvec3 un = n / len;
    const double ud = d / len;
    constexpr double kEps = 1e-4;
    std::vector<Face> out;
    std::vector<glm::dvec3> cap;
    bool cut_any = false;
    for (const Face& f : faces) {
        Face kept;
        const std::size_t m = f.size();
        for (std::size_t i = 0; i < m; ++i) {
            const glm::dvec3& a = f[i];
            const glm::dvec3& b = f[(i + 1) % m];
            const double da = glm::dot(un, a) + ud, db = glm::dot(un, b) + ud;
            if (da >= -kEps) kept.push_back(a);
            if (std::abs(da) <= kEps) cap.push_back(a);
            if ((da > kEps && db < -kEps) || (da < -kEps && db > kEps)) {
                glm::dvec3 x = a + (b - a) * (da / (da - db));
                kept.push_back(x);
                cap.push_back(x);
                cut_any = true;
            }
            if (da < -kEps) cut_any = true;
        }
        if (kept.size() >= 3) out.push_back(std::move(kept));
    }
    if (cut_any && cap.size() >= 3) {
        // The cap: the points on the plane, ordered round their middle.
        std::vector<glm::dvec3> unique;
        for (const auto& p : cap)
            if (std::none_of(unique.begin(), unique.end(), [&](const glm::dvec3& q) { return glm::length(p - q) < 1e-3; }))
                unique.push_back(p);
        if (unique.size() >= 3) {
            glm::dvec3 mid(0.0);
            for (const auto& p : unique) mid += p;
            mid /= static_cast<double>(unique.size());
            glm::dvec3 u = std::abs(un.x) < 0.9 ? glm::normalize(glm::cross(un, glm::dvec3(1, 0, 0)))
                                                : glm::normalize(glm::cross(un, glm::dvec3(0, 1, 0)));
            glm::dvec3 v = glm::cross(un, u);
            std::sort(unique.begin(), unique.end(), [&](const glm::dvec3& a, const glm::dvec3& b) {
                return std::atan2(glm::dot(a - mid, v), glm::dot(a - mid, u)) <
                       std::atan2(glm::dot(b - mid, v), glm::dot(b - mid, u));
            });
            out.push_back(std::move(unique));
        }
    }
    faces = std::move(out);
    return faces.size() >= 4;
}

} // namespace

std::vector<ZoneRegions::LeafCell> ZoneRegions::leaf_cells(const glm::vec3& lo, const glm::vec3& hi,
                                                           const std::function<bool(std::uint32_t)>& want) const {
    std::vector<LeafCell> out;
    if (!boxes_.empty()) {
        for (std::size_t i = 0; i < boxes_.size(); ++i) {
            if (boxes_[i].degenerate || !want(static_cast<std::uint32_t>(i + 1))) continue;
            glm::mat4 to_world = glm::inverse(boxes_[i].world_to_box);
            LeafCell c;
            c.region = static_cast<std::uint32_t>(i + 1);
            c.lo = glm::vec3(std::numeric_limits<float>::max());
            c.hi = -c.lo;
            for (int k = 0; k < 8; ++k) {
                glm::vec3 p(to_world * glm::vec4(k & 1 ? 1.0f : -1.0f, k & 2 ? 1.0f : -1.0f, k & 4 ? 1.0f : -1.0f, 1.0f));
                c.lo = glm::min(c.lo, p);
                c.hi = glm::max(c.hi, p);
            }
            out.push_back(c);
        }
        return out;
    }
    if (nodes_.empty()) return out;
    // Which subtrees hold a wanted leaf (post-order, iteratively; a cyclic tree is cut where it loops).
    const std::size_t n = nodes_.size();
    std::vector<std::uint8_t> state(n, 0); // 0 unseen, 1 open, 2 done
    std::vector<std::uint8_t> holds(n, 0);
    std::vector<std::size_t> stack{0};
    while (!stack.empty()) {
        std::size_t i = stack.back();
        const auto& node = nodes_[i];
        const bool leaf = node.front == 0 && node.back == 0;
        if (state[i] == 0) {
            state[i] = 1;
            if (leaf) {
                holds[i] = node.region != 0 && node.region <= regions_.size() && want(node.region);
                state[i] = 2;
                stack.pop_back();
                continue;
            }
            for (std::uint32_t child : {node.front, node.back})
                if (child != 0 && child <= n && state[child - 1] == 0) stack.push_back(child - 1);
            continue;
        }
        if (state[i] == 1) {
            for (std::uint32_t child : {node.front, node.back})
                if (child != 0 && child <= n && state[child - 1] == 2) holds[i] |= holds[child - 1];
            state[i] = 2;
        }
        stack.pop_back();
    }
    if (!holds[0]) return out;
    struct Item {
        std::size_t node;
        std::vector<Face> cell;
    };
    std::vector<Item> work;
    work.push_back({0, box_polytope(glm::dvec3(lo), glm::dvec3(hi))});
    std::size_t visits = 0;
    while (!work.empty() && visits++ < 4 * n) {
        Item item = std::move(work.back());
        work.pop_back();
        const auto& node = nodes_[item.node];
        if (node.front == 0 && node.back == 0) {
            LeafCell c;
            c.region = node.region;
            // Float bounds, as the box branch: a double one would overflow the float LeafCell if the cell were empty.
            glm::dvec3 a(std::numeric_limits<float>::max()), b(-std::numeric_limits<float>::max());
            for (const auto& f : item.cell)
                for (const auto& p : f) {
                    a = glm::min(a, p);
                    b = glm::max(b, p);
                }
            c.lo = glm::vec3(a);
            c.hi = glm::vec3(b);
            out.push_back(c);
            continue;
        }
        const glm::dvec3 normal(node.normal);
        const double d = node.distance;
        if (node.front != 0 && node.front <= n && holds[node.front - 1]) {
            std::vector<Face> front = item.cell;
            if (cut_polytope(front, normal, d)) work.push_back({node.front - 1, std::move(front)});
        }
        if (node.back != 0 && node.back <= n && holds[node.back - 1]) {
            std::vector<Face> back = std::move(item.cell);
            if (cut_polytope(back, -normal, -d)) work.push_back({node.back - 1, std::move(back)});
        }
    }
    return out;
}

bool ZoneRegions::box_contains(std::uint32_t region, const glm::vec3& p) const {
    if (region == 0 || region > boxes_.size()) return false;
    const auto& box = boxes_[region - 1];
    if (box.degenerate || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    glm::vec3 l(box.world_to_box * glm::vec4(p, 1.0f));
    return std::fabs(l.x) <= 1.0f && std::fabs(l.y) <= 1.0f && std::fabs(l.z) <= 1.0f;
}

RegionInfo ZoneRegions::lookup(const glm::vec3& world_pos) const { return info(region_at(world_pos)); }

std::vector<RegionInfo> ZoneRegions::tagged() const {
    std::vector<RegionInfo> out;
    for (std::size_t i = 0; i < regions_.size(); ++i)
        if (regions_[i].tag >= 0) out.push_back(info(static_cast<std::uint32_t>(i + 1)));
    return out;
}

} // namespace hailstone::assets
