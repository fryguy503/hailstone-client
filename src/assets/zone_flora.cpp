#include "assets/zone_flora.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "assets/flora.h"
#include "assets/pfs.h"
#include "assets/texture.h"
#include "assets/zone.h"
#include "core/ascii_case.h"
#include "core/float_chars.h"
#include "core/game_dir.h"
#include "core/log.h"

namespace hailstone::assets {

namespace {

std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

bool number(std::string_view s, float& out) {
    float v = 0.0f;
    auto r = float_from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = ascii_lower(c);
    return out;
}

// The file name of a material's texture: a frame renamed "<archive>/<file>" (wld_zone.cpp use_own_textures) is
// <file>.
std::string_view texture_of(const Material& m) {
    if (m.frames.empty()) return {};
    std::string_view f = m.frames.front();
    if (auto slash = f.find_last_of('/'); slash != std::string_view::npos) f = f.substr(slash + 1);
    return f;
}

float unit(MsvcRand& rand) { return static_cast<float>(rand()) / 32767.0f; }

// The zone's terrain seen from below: whether any of its triangles lies over a point (a dungeon's ceiling, a roof, a
// bridge, an overhang). The placed objects (trees, tents) are not in it, so grass still grows under a tree; nor are
// the surfaces never drawn (BlendMode::Invisible: the outdoor zones' collision ceiling some 750 units up and their
// invisible cloud layers) or that only glow (Additive).
bool casts_cover(const Material& m) { return m.blend != BlendMode::Invisible && m.blend != BlendMode::Additive; }

// A vertex past this many units from the origin is a damaged mesh's, not ground: the classic and EQG zones lie
// within some +-20,000 units. Such a triangle (or a non-finite one) neither grows plants nor covers any; it would
// otherwise reach the float-to-int casts below as NaN or split into a million pieces.
constexpr float kMaxFloraCoord = 100000.0f;

bool sane_position(const glm::vec3& p) {
    return std::abs(p.x) <= kMaxFloraCoord && std::abs(p.y) <= kMaxFloraCoord && std::abs(p.z) <= kMaxFloraCoord;
}

class CoverGrid {
public:
    explicit CoverGrid(const Zone& zone) {
        glm::vec2 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
        for (const auto& mesh : zone.terrain)
            for (const auto& part : mesh.parts) {
                if (part.material >= mesh.materials.size() || !casts_cover(mesh.materials[part.material])) continue;
                const std::size_t end =
                    std::min<std::size_t>(std::size_t{part.first_index} + part.index_count, mesh.indices.size());
                for (std::size_t i = part.first_index; i + 3 <= end; i += 3) {
                    Tri t;
                    bool ok = true;
                    for (int k = 0; k < 3; ++k) {
                        const std::uint32_t v = mesh.indices[i + static_cast<std::size_t>(k)];
                        if (v >= mesh.vertices.size() || !sane_position(mesh.vertices[v].position)) { // NaN too
                            ok = false;
                            break;
                        }
                        t.p[k] = mesh.vertices[v].position;
                    }
                    if (!ok) continue;
                    const float area2 = (t.p[1].x - t.p[0].x) * (t.p[2].y - t.p[0].y) -
                                        (t.p[2].x - t.p[0].x) * (t.p[1].y - t.p[0].y);
                    if (std::abs(area2) < 1e-4f) continue; // a wall seen from above covers nothing
                    tris_.push_back(t);
                    for (const auto& q : t.p) {
                        lo = glm::min(lo, glm::vec2(q));
                        hi = glm::max(hi, glm::vec2(q));
                    }
                }
            }
        if (tris_.empty()) return;
        origin_ = lo;
        nx_ = std::min<std::int32_t>(static_cast<std::int32_t>((hi.x - lo.x) / kCell) + 1, 4096);
        ny_ = std::min<std::int32_t>(static_cast<std::int32_t>((hi.y - lo.y) / kCell) + 1, 4096);
        struct Span {
            std::int32_t x0, x1, y0, y1;
        };
        auto span_of = [&](const Tri& t) {
            glm::vec2 a = glm::min(glm::min(glm::vec2(t.p[0]), glm::vec2(t.p[1])), glm::vec2(t.p[2]));
            glm::vec2 b = glm::max(glm::max(glm::vec2(t.p[0]), glm::vec2(t.p[1])), glm::vec2(t.p[2]));
            return Span{cell(a.x - origin_.x, nx_), cell(b.x - origin_.x, nx_), cell(a.y - origin_.y, ny_),
                        cell(b.y - origin_.y, ny_)};
        };
        // A triangle over more than kBigCells cells (a sea floor across the zone) is kept apart and tested at every
        // point, as collision.cpp's big_; the cells hold at most kMaxRefs references in all. A triangle past either
        // bound (only a damaged mesh has so many) is no cover: plants may grow under it, nothing worse.
        std::vector<std::uint32_t> counts(static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_) + 1, 0);
        std::vector<std::uint8_t> where(tris_.size(), 0); // 0 the grid, 1 big_, 2 dropped
        std::uint64_t total = 0;
        std::size_t dropped = 0;
        for (std::size_t i = 0; i < tris_.size(); ++i) {
            const Span s = span_of(tris_[i]);
            const std::uint64_t cells = std::uint64_t(s.x1 - s.x0 + 1) * std::uint64_t(s.y1 - s.y0 + 1);
            if (cells > kBigCells) {
                if (big_.size() < kMaxBig) {
                    big_.push_back(static_cast<std::uint32_t>(i));
                    where[i] = 1;
                } else {
                    where[i] = 2;
                    ++dropped;
                }
                continue;
            }
            if (total + cells > kMaxRefs) {
                where[i] = 2;
                ++dropped;
                continue;
            }
            total += cells;
            for (std::int32_t y = s.y0; y <= s.y1; ++y)
                for (std::int32_t x = s.x0; x <= s.x1; ++x) ++counts[index_of(x, y) + 1];
        }
        if (dropped > 0) log::debug("{}: zone flora: {} triangles too large to tell cover by", zone.name, dropped);
        for (std::size_t c = 1; c < counts.size(); ++c) counts[c] += counts[c - 1]; // at most kMaxRefs: no wrap
        start_ = counts;
        index_.resize(counts.back());
        for (std::uint32_t i = 0; i < tris_.size(); ++i) {
            if (where[i] != 0) continue;
            const Span s = span_of(tris_[i]);
            for (std::int32_t y = s.y0; y <= s.y1; ++y)
                for (std::int32_t x = s.x0; x <= s.x1; ++x) index_[counts[index_of(x, y)]++] = i;
        }
    }

    // Whether a triangle lies more than kClearance over `p` (straight up).
    bool covered(const glm::vec3& p) const {
        if (tris_.empty()) return false;
        const std::size_t c = index_of(cell(p.x - origin_.x, nx_), cell(p.y - origin_.y, ny_));
        for (std::uint32_t k = start_[c]; k < start_[c + 1]; ++k)
            if (over(tris_[index_[k]], p)) return true;
        for (std::uint32_t t : big_)
            if (over(tris_[t], p)) return true;
        return false;
    }

private:
    static constexpr float kCell = 64.0f;
    static constexpr float kClearance = 1.0f; // the ground's own triangle, and a decal over it, are not cover
    static constexpr std::uint64_t kBigCells = 1024;      // 2,048 units a side
    static constexpr std::size_t kMaxBig = 256;
    static constexpr std::uint64_t kMaxRefs = 1u << 24;  // 64 MB of references
    struct Tri {
        glm::vec3 p[3];
    };
    static bool over(const Tri& t, const glm::vec3& p) {
        // Barycentric in the plane; the height there.
        const glm::vec2 a(t.p[0]), b(t.p[1]), q(t.p[2]), pt(p);
        const float d = (b.x - a.x) * (q.y - a.y) - (q.x - a.x) * (b.y - a.y);
        const float u = ((pt.x - a.x) * (q.y - a.y) - (q.x - a.x) * (pt.y - a.y)) / d;
        const float v = ((b.x - a.x) * (pt.y - a.y) - (pt.x - a.x) * (b.y - a.y)) / d;
        if (u < 0.0f || v < 0.0f || u + v > 1.0f) return false;
        const float z = t.p[0].z + (t.p[1].z - t.p[0].z) * u + (t.p[2].z - t.p[0].z) * v;
        return z > p.z + kClearance;
    }
    // The cell of offset `d` along an axis of `n` cells, clamped; a NaN is cell 0. Clamped as a float first: a float
    // out of int32's range cast to it is undefined.
    static std::int32_t cell(float d, std::int32_t n) {
        const float c = std::floor(d / kCell);
        if (!(c > 0.0f)) return 0;
        if (c >= static_cast<float>(n - 1)) return n - 1;
        return static_cast<std::int32_t>(c);
    }
    std::size_t index_of(std::int32_t x, std::int32_t y) const {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(x);
    }
    std::vector<Tri> tris_;
    glm::vec2 origin_{0.0f};
    std::int32_t nx_ = 0, ny_ = 0;
    std::vector<std::uint32_t> start_, index_, big_;
};

// A triangle's own sequence (Hailstone's own seed, so it grows the same plants at every load).
std::uint32_t triangle_seed(std::size_t mesh, std::size_t triangle) {
    return static_cast<std::uint32_t>(mesh) * 2654435761u ^ static_cast<std::uint32_t>(triangle) * 40503u ^
           0x5bd1e995u;
}
} // namespace

namespace {

// Reads `text`'s lines into `rules` in order; an include reads the named file's in its place.
void parse_into(ZoneFloraRules& rules, std::string_view text, std::vector<std::string>* errors,
                const ZoneFloraInclude& include, int depth, std::string_view source) {
    std::size_t line_no = 0;
    auto fail = [&](std::string_view why) {
        if (errors)
            errors->push_back((source.empty() ? std::string() : std::string(source) + " ") + "line " +
                              std::to_string(line_no) + ": " + std::string(why));
    };
    while (!text.empty()) {
        ++line_no;
        std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        auto w = words(line);
        if (w.empty()) continue;
        if (iequals(w[0], "include") && w.size() == 2) {
            if (depth >= kMaxFloraIncludeDepth) { fail("include: nested too deep"); continue; }
            std::optional<std::string> other = include ? include(w[1]) : std::nullopt;
            if (!other) { fail("include: no rules named " + std::string(w[1])); continue; }
            parse_into(rules, *other, errors, include, depth + 1, w[1]);
        } else if (iequals(w[0], "donor") && w.size() == 2) {
            std::string donor = lower(w[1]);
            if (std::find(rules.donors.begin(), rules.donors.end(), donor) == rules.donors.end())
                rules.donors.push_back(std::move(donor));
        } else if (iequals(w[0], "max_slope") && w.size() == 2) {
            float v = 0.0f;
            if (!number(w[1], v)) { fail("max_slope: not a number"); continue; }
            rules.max_slope = std::clamp(v, 0.0f, 90.0f);
        } else if (iequals(w[0], "scale") && w.size() == 3) {
            float a = 0.0f, b = 0.0f;
            if (!number(w[1], a) || !number(w[2], b) || a <= 0.0f || b < a) { fail("scale: needs 0 < min <= max"); continue; }
            rules.min_scale = std::min(a, 100.0f);
            rules.max_scale = std::min(b, 100.0f);
        } else if (iequals(w[0], "lawn_height") && w.size() == 3) {
            float a = 0.0f, b = 0.0f;
            if (!number(w[1], a) || !number(w[2], b) || a <= 0.0f || b < a) { fail("lawn_height: needs 0 < min <= max"); continue; }
            rules.lawn_min = std::min(a, 50.0f);
            rules.lawn_max = std::min(b, 50.0f);
        } else if (iequals(w[0], "ground") && w.size() >= 3) {
            ZoneFloraRules::Ground g;
            g.texture = std::string(w[1]);
            if (!number(w[2], g.density) || g.density < 0.0f) { fail("ground: the density is not a number >= 0"); continue; }
            g.density = std::min(g.density, 1000.0f);
            bool ok = true;
            for (std::size_t i = 3; i < w.size(); ++i) {
                ZoneFloraRules::Kind k;
                std::string_view s = w[i];
                if (iequals(s, "+green") || iequals(s, "+greenish")) {
                    g.green_only = true;
                    g.greenish = iequals(s, "+greenish");
                    continue;
                }
                if (iequals(s, "lawn")) {
                    g.lawn = true;
                    continue;
                }
                if (auto colon = s.find(':'); colon != std::string_view::npos) {
                    if (!number(s.substr(colon + 1), k.weight) || k.weight < 0.0f) { ok = false; break; }
                    s = s.substr(0, colon);
                }
                k.definition = std::string(s);
                g.kinds.push_back(std::move(k));
            }
            if (!ok) { fail("ground: a kind's weight is not a number >= 0"); continue; }
            if (g.kinds.empty() && g.density > 0.0f) { fail("ground: a density but no kinds"); continue; }
            rules.ground.push_back(std::move(g));
        } else {
            fail("not understood");
        }
    }
}

} // namespace

ZoneFloraRules parse_zone_flora_rules(std::string_view text, std::vector<std::string>* errors,
                                      const ZoneFloraInclude& include) {
    ZoneFloraRules rules;
    parse_into(rules, text, errors, include, 0, {});
    return rules;
}

bool is_greenish_texel(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const int R = r, G = g, B = b;
    return G >= 24 && G * 100 > R * 100 && G * 100 > B * 115;
}

bool is_green_texel(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    // Green clearly over red (not the olive of mud and moss-dark rock) and well over blue (not the classic water's
    // teal), and bright enough to tell (not a dungeon's near-black stone).
    const int R = r, G = g, B = b;
    return G >= 32 && G * 100 > R * 106 && G * 100 > B * 125;
}

struct ZoneFloraGrower::Data {
    using Tri = Piece;
    // A ground rule's kinds as definition indices with their running weight.
    struct Pick {
        std::vector<std::pair<float, std::uint32_t>> cumulative;
        float total = 0.0f;
    };
    ZoneFloraRules rules;
    std::vector<eqg::FloraDefinition> definitions;
    std::vector<Pick> picks;
    // A ground texture as +green and the tint read it: level 0 scaled down to at most kSampleSize a side, as
    // 0x00BBGGRR; empty when it could not be read.
    struct Ground {
        std::vector<std::uint32_t> rgb;
        std::uint32_t width = 0, height = 0;
        std::optional<std::uint32_t> at(glm::vec2 uv) const {
            if (rgb.empty()) return std::nullopt;
            auto wrap = [](float f, std::uint32_t n) {
                const float t = f - std::floor(f);
                return std::min(static_cast<std::uint32_t>(t * static_cast<float>(n)), n - 1);
            };
            return rgb[std::size_t{wrap(uv.y, height)} * width + wrap(uv.x, width)];
        }
    };
    static constexpr std::uint32_t kSampleSize = 128;
    std::vector<std::string> textures;
    std::vector<Ground> grounds; // by texture
    std::vector<Tri> tris;
    std::vector<glm::vec2> tiles;
    std::vector<std::uint32_t> tile_start, tile_tris; // tile t's triangles: tile_tris[tile_start[t] .. tile_start[t+1])
    ZoneRegions regions;
    std::optional<CoverGrid> cover;
};

namespace {

// A triangle larger than a quarter tile is split into four (by its edges' midpoints), again until it is not: a tile
// then walks only the plants of the ground near it, not every plant of a sea floor's one triangle across the zone.
constexpr float kMaxGrowArea = kZoneFloraTile * kZoneFloraTile / 4.0f;
constexpr int kMaxSplits = 10; // 4^10 pieces at most, for a damaged mesh's triangle across the world
// The most pieces a zone's ground is cut into. A split piece is over a quarter of kMaxGrowArea, so ground of 20,000
// units a side makes some 400,000 (plus the triangles too small to split, a few hundred thousand in the densest
// zone); only a damaged mesh asks for more, and its ground past this grows nothing.
constexpr std::size_t kMaxPieces = std::size_t{1} << 21;

void add_split(std::vector<ZoneFloraGrower::Piece>& out, const ZoneFloraGrower::Piece& t, int depth) {
    if (out.size() >= kMaxPieces) return;
    if (t.area <= kMaxGrowArea || depth >= kMaxSplits) {
        out.push_back(t);
        return;
    }
    glm::vec3 mp[3];
    glm::vec2 muv[3];
    for (int k = 0; k < 3; ++k) {
        mp[k] = (t.p[k] + t.p[(k + 1) % 3]) * 0.5f;
        muv[k] = (t.uv[k] + t.uv[(k + 1) % 3]) * 0.5f;
    }
    // Corners k, mid k, mid k-1; and the middle one.
    const glm::vec3 cp[4][3] = {{t.p[0], mp[0], mp[2]}, {mp[0], t.p[1], mp[1]}, {mp[2], mp[1], t.p[2]}, {mp[0], mp[1], mp[2]}};
    const glm::vec2 cuv[4][3] = {
        {t.uv[0], muv[0], muv[2]}, {muv[0], t.uv[1], muv[1]}, {muv[2], muv[1], t.uv[2]}, {muv[0], muv[1], muv[2]}};
    for (std::uint32_t c = 0; c < 4; ++c) {
        ZoneFloraGrower::Piece q = t;
        for (int k = 0; k < 3; ++k) {
            q.p[k] = cp[c][k];
            q.uv[k] = cuv[c][k];
        }
        q.area = t.area * 0.25f;
        q.seed = (t.seed ^ 0x9e3779b9u) * 2654435761u + c * 40503u + 1u;
        add_split(out, q, depth + 1);
    }
}

} // namespace

ZoneFloraGrower::ZoneFloraGrower(const Zone& zone, const ZoneFloraRules& rules,
                                 std::vector<eqg::FloraDefinition> definitions, const GroundTextures& textures)
    : d_(std::make_unique<Data>()) {
    Data& d = *d_;
    d.rules = rules;
    d.definitions = std::move(definitions);
    d.picks.resize(rules.ground.size());
    for (std::size_t g = 0; g < rules.ground.size(); ++g)
        for (const auto& k : rules.ground[g].kinds) {
            auto it = std::find_if(d.definitions.begin(), d.definitions.end(), [&](const eqg::FloraDefinition& def) {
                return flora_kind_matches(k.definition, def.name);
            });
            if (it == d.definitions.end()) {
                log::debug("{}: zone flora: no definition {}", zone.name, k.definition);
                continue;
            }
            if (k.weight <= 0.0f) continue;
            d.picks[g].total += k.weight;
            d.picks[g].cumulative.emplace_back(d.picks[g].total, static_cast<std::uint32_t>(it - d.definitions.begin()));
        }

    // The triangles that may grow plants: their texture's rule grows some, level enough, on a drawn surface.
    const float min_up = std::cos(glm::radians(rules.max_slope));
    std::unordered_map<std::string, int> rule_by_texture; // lower-case texture -> ground rule (-1 none)
    std::unordered_map<std::string, std::uint32_t> texture_index;
    std::vector<bool> barren; // by texture: +green finds (almost) no grass in it, so its ground grows nothing
    // A texture read once, scaled down, and how much of it +green (or +greenish) takes.
    auto read_ground = [&](const std::string& tex, const ZoneFloraRules::Ground& rule) {
        Data::Ground g;
        std::optional<Image> image = textures ? textures(tex) : std::nullopt;
        std::size_t green = 0;
        if (image && image->format == PixelFormat::RGBA8 && !image->mips.empty() && image->width > 0 && image->height > 0 &&
            image->mips[0].size() >= std::size_t{image->width} * image->height * 4) {
            g.width = std::min(image->width, Data::kSampleSize);
            g.height = std::min(image->height, Data::kSampleSize);
            g.rgb.resize(std::size_t{g.width} * g.height);
            const auto& px = image->mips[0];
            for (std::uint32_t y = 0; y < g.height; ++y)
                for (std::uint32_t x = 0; x < g.width; ++x) {
                    const std::size_t sx = std::size_t{x} * image->width / g.width, sy = std::size_t{y} * image->height / g.height;
                    const std::size_t at = (sy * image->width + sx) * 4;
                    const auto r = static_cast<std::uint8_t>(px[at]), gr = static_cast<std::uint8_t>(px[at + 1]),
                               b = static_cast<std::uint8_t>(px[at + 2]);
                    g.rgb[std::size_t{y} * g.width + x] = std::uint32_t{r} | std::uint32_t{gr} << 8 | std::uint32_t{b} << 16;
                    if (rule.greenish ? is_greenish_texel(r, gr, b) : is_green_texel(r, gr, b)) ++green;
                }
        }
        // +green on a texture that cannot be read, or that holds next to no grass, grows nothing there.
        const bool none = rule.green_only && (g.rgb.empty() || static_cast<float>(green) <
                                                                   kMinGreenShare * static_cast<float>(g.rgb.size()));
        return std::pair<Data::Ground, bool>(std::move(g), none);
    };
    for (std::size_t mi = 0; mi < zone.terrain.size(); ++mi) {
        const Mesh& mesh = zone.terrain[mi];
        for (const auto& part : mesh.parts) {
            if (part.material >= mesh.materials.size()) continue;
            // Plants grow on solid ground: an opaque or cut-out surface. Not on one that is never drawn (the invisible
            // collision floors and ceilings), nor on a translucent one (the classic water and its falls).
            if (const BlendMode blend = mesh.materials[part.material].blend;
                blend != BlendMode::Opaque && blend != BlendMode::Masked)
                continue;
            std::string tex = lower(texture_of(mesh.materials[part.material]));
            if (tex.empty()) continue;
            auto found = rule_by_texture.find(tex);
            if (found == rule_by_texture.end()) {
                int rule = -1;
                for (std::size_t g = 0; g < rules.ground.size() && rule < 0; ++g)
                    if (iglob(rules.ground[g].texture, tex)) rule = static_cast<int>(g);
                found = rule_by_texture.emplace(tex, rule).first;
            }
            if (found->second < 0) continue;
            const auto rule = static_cast<std::uint32_t>(found->second);
            const auto& ground_rule = rules.ground[rule];
            if (!ground_rule.lawn && (ground_rule.density <= 0.0f || d.picks[rule].total <= 0.0f)) continue;
            auto [ti, added] = texture_index.emplace(tex, static_cast<std::uint32_t>(d.textures.size()));
            if (added) {
                d.textures.push_back(tex);
                auto [ground, none] = read_ground(tex, rules.ground[rule]);
                d.grounds.push_back(std::move(ground));
                barren.push_back(none);
            }
            if (barren[ti->second]) continue;
            const std::uint32_t end = std::min<std::uint32_t>(part.first_index + part.index_count,
                                                              static_cast<std::uint32_t>(mesh.indices.size()));
            for (std::uint32_t i = part.first_index; i + 3 <= end; i += 3) {
                const std::uint32_t ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
                if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() || ic >= mesh.vertices.size()) continue;
                const Vertex &a = mesh.vertices[ia], &b = mesh.vertices[ib], &c = mesh.vertices[ic];
                if (!sane_position(a.position) || !sane_position(b.position) || !sane_position(c.position)) continue;
                glm::vec3 n = glm::cross(b.position - a.position, c.position - a.position);
                const float len = glm::length(n);
                if (!(len > 1e-6f)) continue;
                n /= len;
                // The face's side from its vertex normals (the lit side), whatever the winding.
                if (glm::dot(n, a.normal + b.normal + c.normal) < 0.0f) n = -n;
                if (n.z < min_up) continue;
                Data::Tri t;
                t.p[0] = a.position;
                t.p[1] = b.position;
                t.p[2] = c.position;
                t.uv[0] = a.uv;
                t.uv[1] = b.uv;
                t.uv[2] = c.uv;
                t.seed = triangle_seed(mi, i / 3);
                t.texture = ti->second;
                t.rule = rule;
                t.area = len * 0.5f;
                add_split(d.tris, t, 0);
            }
        }
    }
    if (d.tris.empty()) return;

    // Binned by the tiles their bounds touch (a plant belongs to the tile its foot is in). The pieces' corners are
    // within kMaxFloraCoord, so the tile numbers fit an int32.
    auto tile_of = [](float v) { return static_cast<std::int32_t>(std::floor(v / kZoneFloraTile)); };
    std::map<std::pair<std::int32_t, std::int32_t>, std::vector<std::uint32_t>> bins;
    for (std::uint32_t i = 0; i < d.tris.size(); ++i) {
        const auto& t = d.tris[i];
        // A margin more: a tile grows the lawn of the squares round it too (soften_lawn_edges).
        const float margin = kLawnCell * static_cast<float>(kLawnEdgeCells);
        const glm::vec2 lo = glm::min(glm::min(glm::vec2(t.p[0]), glm::vec2(t.p[1])), glm::vec2(t.p[2])) - margin;
        const glm::vec2 hi = glm::max(glm::max(glm::vec2(t.p[0]), glm::vec2(t.p[1])), glm::vec2(t.p[2])) + margin;
        for (std::int32_t y = tile_of(lo.y); y <= tile_of(hi.y); ++y)
            for (std::int32_t x = tile_of(lo.x); x <= tile_of(hi.x); ++x) bins[{x, y}].push_back(i);
    }
    d.tile_start.push_back(0);
    for (const auto& [k, list] : bins) {
        d.tiles.emplace_back(static_cast<float>(k.first) * kZoneFloraTile, static_cast<float>(k.second) * kZoneFloraTile);
        d.tile_tris.insert(d.tile_tris.end(), list.begin(), list.end());
        d.tile_start.push_back(static_cast<std::uint32_t>(d.tile_tris.size()));
    }
    d.regions = zone.regions;
    d.cover.emplace(zone);
}

ZoneFloraGrower::~ZoneFloraGrower() = default;

const std::vector<eqg::FloraDefinition>& ZoneFloraGrower::definitions() const { return d_->definitions; }
const std::vector<glm::vec2>& ZoneFloraGrower::tiles() const { return d_->tiles; }
std::size_t ZoneFloraGrower::triangles() const { return d_->tris.size(); }

namespace {

// Whether a plant may stand at `p` with the texel `under` beneath it: +green's test (a texture that cannot be read
// grows nothing with +green: no telling grass from stone), out of water, lava and zone lines, and not under cover.
bool green_enough(const ZoneFloraRules::Ground& ground, const std::optional<std::uint32_t>& under) {
    if (!under) return false;
    const auto r = static_cast<std::uint8_t>(*under & 0xff), g = static_cast<std::uint8_t>(*under >> 8 & 0xff),
               b = static_cast<std::uint8_t>(*under >> 16 & 0xff);
    return ground.greenish ? is_greenish_texel(r, g, b) : is_green_texel(r, g, b);
}

// `green` false: the caller has made +green's test (the lawn's, by a square's majority).
bool may_grow(const ZoneFloraGrower::Data& d, const ZoneFloraRules::Ground& ground, const std::optional<std::uint32_t>& under,
              const glm::vec3& p, bool green = true) {
    if (green && ground.green_only && !green_enough(ground, under)) return false;
    if (!d.regions.empty()) {
        // In the water or lava, or standing on its surface (the region just under the foot), or in a zone line.
        const RegionInfo r = d.regions.lookup(p + glm::vec3(0.0f, 0.0f, 1.0f));
        if (r.is_water() || r.is_lava() || r.is_zone_line()) return false;
        const RegionInfo below = d.regions.lookup(p - glm::vec3(0.0f, 0.0f, 1.0f));
        if (below.is_water() || below.is_lava()) return false;
    }
    return !(d.cover && d.cover->covered(p));
}

// Whether `q` lies in the triangle a b c seen from above, a point on an edge going to exactly one of the two
// triangles sharing it: each edge's side is worked from its ends in a fixed order (the same bits in both triangles),
// and a point on it belongs to the triangle on its positive side.
bool inside_from_above(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 q) {
    auto side = [](glm::vec2 e0, glm::vec2 e1, glm::vec2 opposite, glm::vec2 p) {
        if (e1.x < e0.x || (e1.x == e0.x && e1.y < e0.y)) std::swap(e0, e1);
        auto f = [&](glm::vec2 r) { return (e1.x - e0.x) * (r.y - e0.y) - (e1.y - e0.y) * (r.x - e0.x); };
        const float inner = f(opposite), at = f(p);
        if (inner == 0.0f) return false;
        return inner > 0.0f ? at >= 0.0f : at < 0.0f;
    };
    return side(a, b, c, q) && side(b, c, a, q) && side(c, a, b, q);
}

// The lawn's patches of piece `t` within the tile [lo, hi) and a margin of kLawnEdgeCells squares round it (soften_lawn_edges looks at
// them, then drops them): one per kLawnCell square whose centre lies over the piece (seen from above), at the piece's
// height there, so the squares of neighbouring pieces meet without gaps or overlaps. A patch takes the ground's colour
// at its centre (a grey-green where the texture cannot be read). `by_texture` counts those within the tile.
void grow_lawn(const ZoneFloraGrower::Data& d, const ZoneFloraGrower::Piece& t, glm::vec2 tile_lo, glm::vec2 tile_hi,
               std::vector<FloraInstance>& out, std::unordered_map<std::string, std::size_t>* by_texture) {
    const glm::vec2 margin(kLawnCell * static_cast<float>(kLawnEdgeCells));
    const glm::vec2 lo = tile_lo - margin, hi = tile_hi + margin;
    const auto& ground = d.rules.ground[t.rule];
    const glm::vec2 a(t.p[0]), b(t.p[1]), c(t.p[2]);
    const float det = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (!(std::abs(det) > 1e-6f)) return; // edge-on from above
    const glm::vec3 n = glm::cross(t.p[1] - t.p[0], t.p[2] - t.p[0]);
    const glm::vec2 slope = std::abs(n.z) > 1e-6f ? glm::vec2(-n.x / n.z, -n.y / n.z) : glm::vec2(0.0f);
    const glm::vec2 box_lo = glm::max(glm::min(glm::min(a, b), c), lo);
    const glm::vec2 box_hi = glm::min(glm::max(glm::max(a, b), c), hi);
    if (box_lo.x >= box_hi.x || box_lo.y >= box_hi.y) return;
    const auto x0 = static_cast<std::int32_t>(std::floor(box_lo.x / kLawnCell - 0.5f));
    const auto x1 = static_cast<std::int32_t>(std::ceil(box_hi.x / kLawnCell - 0.5f));
    const auto y0 = static_cast<std::int32_t>(std::floor(box_lo.y / kLawnCell - 0.5f));
    const auto y1 = static_cast<std::int32_t>(std::ceil(box_hi.y / kLawnCell - 0.5f));
    const auto& texels = d.grounds[t.texture];
    for (std::int32_t y = y0; y <= y1; ++y)
        for (std::int32_t x = x0; x <= x1; ++x) {
            const glm::vec2 q((static_cast<float>(x) + 0.5f) * kLawnCell, (static_cast<float>(y) + 0.5f) * kLawnCell);
            if (q.x < lo.x || q.x >= hi.x || q.y < lo.y || q.y >= hi.y) continue; // another tile's
            if (!inside_from_above(a, b, c, q)) continue;
            const bool own = q.x >= tile_lo.x && q.x < tile_hi.x && q.y >= tile_lo.y && q.y < tile_hi.y;
            const float u = ((q.x - a.x) * (c.y - a.y) - (c.x - a.x) * (q.y - a.y)) / det;
            const float v = ((b.x - a.x) * (q.y - a.y) - (q.x - a.x) * (b.y - a.y)) / det;
            const float w = 1.0f - u - v;
            const glm::vec3 p = t.p[0] * w + t.p[1] * u + t.p[2] * v;
            const std::optional<std::uint32_t> under = texels.at(t.uv[0] * w + t.uv[1] * u + t.uv[2] * v);
            // +green by the square's majority: its centre and four spots half a square out (the piece's texture
            // mapping carried past its edges), so a texture blending grass into dirt texel by texel does not grow a
            // speckle of lone patches.
            if (ground.green_only) {
                int green = green_enough(ground, under) ? 1 : 0;
                const float h = kLawnCell * 0.5f;
                for (const glm::vec2 o : {glm::vec2(h, 0.0f), glm::vec2(-h, 0.0f), glm::vec2(0.0f, h), glm::vec2(0.0f, -h)}) {
                    const glm::vec2 r = q + o;
                    const float ou = ((r.x - a.x) * (c.y - a.y) - (c.x - a.x) * (r.y - a.y)) / det;
                    const float ov = ((b.x - a.x) * (r.y - a.y) - (r.x - a.x) * (b.y - a.y)) / det;
                    if (green_enough(ground, texels.at(t.uv[0] * (1.0f - ou - ov) + t.uv[1] * ou + t.uv[2] * ov))) ++green;
                }
                if (green < 3) continue;
            }
            if (!may_grow(d, ground, under, p, false)) continue;
            FloraInstance patch;
            patch.position = p;
            patch.width = kLawnCell;
            patch.height = d.rules.lawn_min + (d.rules.lawn_max - d.rules.lawn_min) * lawn_height_noise(q);
            patch.color = under ? 0xff000000u | (*under & 0xffffffu) : 0xff3f6b3au;
            patch.definition = kLawnDefinition;
            patch.slope = slope;
            out.push_back(patch);
            if (by_texture && own) ++(*by_texture)[d.textures[t.texture]];
        }
}

} // namespace

float lawn_edge_factor(float squares) {
    const float t = std::clamp((squares - 1.0f) / (static_cast<float>(kLawnEdgeCells) - 0.5f), 0.0f, 1.0f);
    return 0.55f + 0.45f * t * t * (3.0f - 2.0f * t);
}

float lawn_height_noise(glm::vec2 p) {
    auto lattice = [](std::int32_t x, std::int32_t y) {
        std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return static_cast<float>(h & 0xffffu) / 65535.0f;
    };
    const glm::vec2 g = p / kLawnNoiseCell;
    const glm::vec2 f = glm::floor(g);
    glm::vec2 t = g - f;
    t = t * t * (3.0f - 2.0f * t);
    const auto x = static_cast<std::int32_t>(f.x), y = static_cast<std::int32_t>(f.y);
    const float bottom = lattice(x, y) + (lattice(x + 1, y) - lattice(x, y)) * t.x;
    const float top = lattice(x, y + 1) + (lattice(x + 1, y + 1) - lattice(x, y + 1)) * t.x;
    return std::clamp(bottom + (top - bottom) * t.y, 0.0f, 1.0f);
}

namespace {

void grow_tile(const ZoneFloraGrower::Data& d, std::size_t tile, std::vector<FloraInstance>& out,
               std::unordered_map<std::string, std::size_t>* by_texture) {
    if (tile >= d.tiles.size()) return;
    const glm::vec2 lo = d.tiles[tile], hi = lo + glm::vec2(kZoneFloraTile);
    const std::size_t first = out.size();
    for (std::uint32_t k = d.tile_start[tile]; k < d.tile_start[tile + 1]; ++k) {
        const ZoneFloraGrower::Piece& t = d.tris[d.tile_tris[k]];
        const auto& ground = d.rules.ground[t.rule];
        const ZoneFloraGrower::Data::Pick& pick = d.picks[t.rule];
        const std::string& tex = d.textures[t.texture];
        const ZoneFloraGrower::Data::Ground& texels = d.grounds[t.texture];
        if (ground.lawn) {
            grow_lawn(d, t, lo, hi, out, by_texture);
            if (out.size() - first >= kMaxTileFlora) {
                out.resize(first + kMaxTileFlora);
                return;
            }
        }
        if (pick.cumulative.empty() || ground.density <= 0.0f) continue;
        MsvcRand rand{t.seed};
        const float expected = t.area * ground.density / 100.0f;
        auto count = static_cast<std::uint32_t>(std::min(expected, 1.0e6f));
        if (unit(rand) < expected - static_cast<float>(count)) ++count;
        for (std::uint32_t n = 0; n < count; ++n) {
            // Every plant takes all its draws (spot, kind, scale, width, height, turn) whatever is kept, so the
            // plants after it stay where they were: the same in every tile the triangle crosses.
            float u = unit(rand), v = unit(rand);
            const float choose = unit(rand) * pick.total;
            const float scale_draw = unit(rand), width_draw = unit(rand), height_draw = unit(rand);
            const auto turn = static_cast<float>(rand() % 360);
            if (u + v > 1.0f) {
                u = 1.0f - u;
                v = 1.0f - v;
            }
            const float w = 1.0f - u - v;
            const glm::vec3 p = t.p[0] * w + t.p[1] * u + t.p[2] * v;
            if (p.x < lo.x || p.x >= hi.x || p.y < lo.y || p.y >= hi.y) continue; // another tile's
            // The ground's colour under the plant: the classic zones' outdoor vertices carry no colour (black), so the
            // plant takes its ground texel's for *INHERITVERTEXCOLOR, 0x80 a neutral grey.
            const std::optional<std::uint32_t> under = texels.at(t.uv[0] * w + t.uv[1] * u + t.uv[2] * v);
            if (!may_grow(d, ground, under, p)) continue;
            std::uint32_t def = pick.cumulative.back().second;
            for (const auto& [upto, index] : pick.cumulative)
                if (choose <= upto) {
                    def = index;
                    break;
                }
            const auto& fd = d.definitions[def];
            const float scale = d.rules.min_scale + (d.rules.max_scale - d.rules.min_scale) * scale_draw;
            FloraInstance plant;
            plant.position = p;
            plant.width = (fd.min_width + (fd.max_width - fd.min_width) * width_draw) * scale;
            plant.height = (fd.min_height + (fd.max_height - fd.min_height) * height_draw) * scale;
            plant.rotation = turn;
            plant.color = under ? 0xff000000u | (*under & 0xffffffu) : 0xff808080u;
            plant.definition = def;
            out.push_back(plant);
            if (by_texture) ++(*by_texture)[tex];
            if (out.size() - first >= kMaxTileFlora) return;
        }
    }
}

// The lawn's edge where it meets bare ground (a path, a road, a texture +green turns down) tapers: a patch is the
// shorter the nearer the nearest bare square within kLawnEdgeCells (lawn_edge_factor), so the carpet does not end in
// a wall along a texture's straight border. `plants` holds the tile's and its margin's patches
// (grow_lawn); the margin's are dropped after.
void soften_lawn_edges(std::vector<FloraInstance>& out, std::size_t first, glm::vec2 lo, glm::vec2 hi) {
    std::span<FloraInstance> plants = std::span<FloraInstance>(out).subspan(first);
    auto cell_of = [](float v) { return static_cast<std::int64_t>(std::floor(v / kLawnCell)); };
    auto key = [](std::int64_t x, std::int64_t y) {
        return static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32 | static_cast<std::uint32_t>(y);
    };
    std::unordered_set<std::uint64_t> grown;
    for (const auto& p : plants)
        if (p.definition == kLawnDefinition) grown.insert(key(cell_of(p.position.x), cell_of(p.position.y)));
    // A patch with fewer than kLawnMinNeighbours of its eight neighbours grown stands alone (a stray green texel's):
    // it goes, so the lawn's edge is a margin of grass, not a stubble of lone tufts.
    std::vector<std::uint64_t> lone;
    for (const auto& p : plants) {
        if (p.definition != kLawnDefinition) continue;
        const std::int64_t cx = cell_of(p.position.x), cy = cell_of(p.position.y);
        int around = 0;
        for (std::int64_t dy = -1; dy <= 1; ++dy)
            for (std::int64_t dx = -1; dx <= 1; ++dx)
                if ((dx != 0 || dy != 0) && grown.contains(key(cx + dx, cy + dy))) ++around;
        if (around < kLawnMinNeighbours) lone.push_back(key(cx, cy));
    }
    for (const auto k : lone) grown.erase(k);
    for (auto& p : plants) {
        if (p.definition != kLawnDefinition) continue;
        const std::int64_t cx = cell_of(p.position.x), cy = cell_of(p.position.y);
        if (!grown.contains(key(cx, cy))) {
            p.height = 0.0f; // dropped below
            continue;
        }
        constexpr std::int64_t r = kLawnEdgeCells;
        std::int64_t nearest2 = (r + 1) * (r + 1);
        for (std::int64_t dy = -r; dy <= r; ++dy)
            for (std::int64_t dx = -r; dx <= r; ++dx) {
                const std::int64_t d2 = dx * dx + dy * dy;
                if (d2 == 0 || d2 >= nearest2) continue;
                if (!grown.contains(key(cx + dx, cy + dy))) nearest2 = d2;
            }
        p.height *= lawn_edge_factor(std::sqrt(static_cast<float>(nearest2)));
    }
    out.erase(std::remove_if(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
                             [&](const FloraInstance& p) {
                                 return p.definition == kLawnDefinition &&
                                        (p.height <= 0.0f || p.position.x < lo.x || p.position.x >= hi.x ||
                                         p.position.y < lo.y || p.position.y >= hi.y);
                             }),
              out.end());
}

} // namespace

void ZoneFloraGrower::grow(std::size_t tile, std::vector<FloraInstance>& out,
                           std::unordered_map<std::string, std::size_t>* by_texture) const {
    const std::size_t first = out.size();
    grow_tile(*d_, tile, out, by_texture);
    if (tile < d_->tiles.size())
        soften_lawn_edges(out, first, d_->tiles[tile], d_->tiles[tile] + glm::vec2(kZoneFloraTile));
}

void place_zone_flora(const Zone& zone, const ZoneFloraRules& rules, std::vector<eqg::FloraDefinition> definitions,
                      ZoneFlora& out, const GroundTextures& textures,
                      std::unordered_map<std::string, std::size_t>* by_texture) {
    ZoneFloraGrower grower(zone, rules, std::move(definitions), textures);
    out = ZoneFlora{};
    out.definitions = grower.definitions();
    out.tile_size = kZoneFloraTile;
    for (std::size_t t = 0; t < grower.tiles().size(); ++t) {
        ZoneFlora::Tile tile;
        tile.min = grower.tiles()[t];
        tile.first = static_cast<std::uint32_t>(out.instances.size());
        grower.grow(t, out.instances, by_texture);
        tile.count = static_cast<std::uint32_t>(out.instances.size()) - tile.first;
        if (tile.count > 0) out.tiles.push_back(tile);
    }
}

std::optional<Image> zone_ground_texture(const Zone& zone, std::string_view name) {
    std::optional<Image> image;
    if (auto data = zone.read_file(name)) image = decode_texture(*data, {.wld_bmp_rows = true});
    if (image && image->format != PixelFormat::RGBA8) image = to_rgba8(*image); // DDS ground in Luclin zones
    if (image && (image->format != PixelFormat::RGBA8 || image->mips.empty() || image->width == 0 ||
                  image->height == 0 || image->mips[0].size() < std::size_t{image->width} * image->height * 4))
        image.reset();
    return image;
}

GroundTextures zone_ground_textures(const Zone& zone) {
    return [&zone](std::string_view name) { return zone_ground_texture(zone, name); };
}

namespace {

std::optional<std::string> read_rules(const std::filesystem::path& dir, std::string_view name) {
    // A rules name is a zone's short name or a shared file's ("default"): letters, digits, '_' and '-' only, so it
    // cannot reach outside the folder.
    if (name.empty() || name.size() > 64) return std::nullopt;
    for (char ch : name)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
            return std::nullopt;
    std::ifstream in(dir / (lower(name) + ".txt"), std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

std::optional<ZoneFloraRules> load_zone_flora_rules(const std::filesystem::path& rules_dir, std::string_view zone,
                                                    bool outdoor, std::string* source) {
    std::string name = lower(zone);
    std::optional<std::string> text = read_rules(rules_dir, name);
    if (!text && outdoor) {
        name = "default";
        text = read_rules(rules_dir, name);
    }
    if (!text) return std::nullopt;
    if (source) *source = name;
    std::vector<std::string> errors;
    ZoneFloraRules rules = parse_zone_flora_rules(*text, &errors,
                                                  [&](std::string_view other) { return read_rules(rules_dir, other); });
    for (const auto& e : errors) log::warn("flora/{}.txt: {}", name, e);
    return rules;
}

bool flora_kind_matches(std::string_view kind, std::string_view definition) {
    if (kind.find('/') != std::string_view::npos) return iequals(kind, definition);
    const auto slash = definition.find('/');
    return iequals(kind, slash == std::string_view::npos ? definition : definition.substr(slash + 1));
}

std::optional<FloraDonors> open_flora_donors(const GameDir& game, std::span<const std::string> donors) {
    FloraDonors out;
    for (const auto& name : donors) {
        auto one = open_flora_donor(game, name);
        if (!one) continue;
        std::string stem = name.substr(0, name.find_last_of('.'));
        for (auto& def : one->definitions) {
            def.name = stem + "/" + def.name;
            out.definitions.push_back(std::move(def));
        }
        out.archives.push_back(one->archive);
    }
    if (out.archives.empty()) return std::nullopt;
    return out;
}

std::optional<FloraDonor> open_flora_donor(const GameDir& game, std::string_view donor) {
    if (donor.empty()) return std::nullopt;
    auto found = game.find(donor);
    if (!found) {
        log::info("zone flora: the donor {} is not in the game folder", donor);
        return std::nullopt;
    }
    FloraDonor out;
    try {
        out.archive = std::make_shared<const PfsArchive>(PfsArchive::open(
            *found, PfsArchive::storage_for(game, *found), game.shared_file_cache()));
    } catch (const std::exception& e) {
        log::warn("zone flora: {}: {}", donor, e.what());
        return std::nullopt;
    }
    auto rfd = out.archive->read("radialfloradefs.rfd");
    if (!rfd) {
        log::info("zone flora: {} has no radialfloradefs.rfd", donor);
        return std::nullopt;
    }
    out.definitions = eqg::parse_radial_flora(*rfd);
    return out;
}

std::size_t add_zone_flora(const GameDir& game, Zone& zone, const std::filesystem::path& rules_dir, bool outdoor) {
    if (!zone.flora.empty()) return 0;
    std::string source;
    auto rules = load_zone_flora_rules(rules_dir, zone.name, outdoor, &source);
    if (!rules) return 0;
    // The lawn needs no donor; the other kinds do, and are left out (logged) when theirs is missing.
    FloraDonors donor;
    if (!rules->donors.empty()) {
        if (auto opened = open_flora_donors(game, rules->donors)) donor = std::move(*opened);
    }
    const bool lawn = std::any_of(rules->ground.begin(), rules->ground.end(), [](const auto& g) { return g.lawn; });
    if (!lawn && donor.definitions.empty()) {
        log::info("{}: zone flora: flora/{}.txt grows no lawn and no donor of it opens", zone.name, source);
        return 0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto grower = std::make_shared<const ZoneFloraGrower>(zone, *rules, std::move(donor.definitions),
                                                          zone_ground_textures(zone));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    zone.flora = ZoneFlora{};
    zone.flora.definitions = grower->definitions();
    zone.flora.tile_size = kZoneFloraTile;
    for (const auto& corner : grower->tiles()) zone.flora.tiles.push_back({corner, 0, 0});
    zone.flora.donors = donor.archives;
    const std::size_t tiles = grower->tiles().size();
    if (tiles == 0) {
        zone.flora = ZoneFlora{};
        log::info("{}: zone flora: nothing grows by flora/{}.txt", zone.name, source);
        return 0;
    }
    zone.flora.grower = std::move(grower);
    log::info("{}: zone flora: {} tiles ({} pieces of ground) may grow plants by flora/{}.txt ({}, {} donor archives); {} ms",
              zone.name, tiles, zone.flora.grower->triangles(), source, lawn ? "a lawn" : "no lawn", donor.archives.size(), ms);
    return tiles;
}

} // namespace hailstone::assets
