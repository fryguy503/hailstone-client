#include "assets/zone_fires.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include "assets/zone.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "core/saturate.h"

namespace hailstone::assets {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

FlameHue flame_material_hue(const Material& m) {
    std::string name = lower(m.name);
    if (name.ends_with("_mdf")) name.resize(name.size() - 4);
    if (name == "fire1") return FlameHue::Fire;
    if (name == "bfire1" || name == "blueflame01") return FlameHue::Blue;
    if (name == "gfire1") return FlameHue::Green;
    if (name == "rfire1") return FlameHue::Red;
    if (name == "pflame01") return FlameHue::Violet;
    return FlameHue::None;
}

FlameHue flame_emitter_hue(const EmitterDefinition& d) {
    std::string name = lower(d.name);
    if (name.find("lava") != std::string::npos || name.starts_with("mml_instance_flame")) return FlameHue::None;
    std::string tex = lower(d.texture);
    // The WLD clouds' sprites.
    if (tex == "geng00.dds" || tex == "genpop00.dds") return FlameHue::Fire;
    if (tex == "geng600.dds") return FlameHue::Red;
    if (tex == "geng10.dds") return FlameHue::Blue;
    if (tex == "geng501.dds" || tex == "genpop01.dds") return FlameHue::Green;
    // The environment emitters'.
    if (tex != "flamed.dds" && tex != "new_flame.dds" && tex != "new_fire.dds" && tex != "fire01.dds" &&
        tex != "fire01_gray.dds" && tex != "flamegray.dds")
        return FlameHue::None;
    // Their colour is the definition's start colour (vald_bluetorch: FlameD.dds in 10, 10, 255), or for a grey
    // sprite in grey its name's (cob_brazier_blue).
    glm::vec3 c(d.color_start);
    if (c.r > c.g * 1.2f && c.b > c.g * 1.2f && c.r > c.b * 0.45f) return FlameHue::Violet;
    if (c.b > c.r * 1.2f && c.b > c.g * 1.2f) return FlameHue::Blue;
    if (c.g > c.r * 1.2f && c.b > c.r * 1.2f) return FlameHue::Blue; // cyan
    if (c.g > c.r * 1.2f && c.g > c.b * 1.2f) return FlameHue::Green;
    if (name.find("blue") != std::string::npos || name.find("cyan") != std::string::npos) return FlameHue::Blue;
    if (name.find("green") != std::string::npos) return FlameHue::Green;
    if (name.find("purple") != std::string::npos || name.find("violet") != std::string::npos)
        return FlameHue::Violet;
    if (name.find("_red") != std::string::npos) return FlameHue::Red;
    return FlameHue::Fire;
    return FlameHue::None;
}

std::vector<FlameBox> mesh_flames(const Mesh& mesh) {
    std::vector<FlameBox> boxes;
    for (const auto& part : mesh.parts) {
        if (part.material >= mesh.materials.size()) continue;
        FlameHue hue = flame_material_hue(mesh.materials[part.material]);
        if (hue == FlameHue::None) continue;
        for (std::uint32_t i = 0; i + 2 < part.index_count; i += 3) {
            FlameBox b{glm::vec3(1e30f), glm::vec3(-1e30f), hue};
            bool valid = true;
            for (int k = 0; k < 3; ++k) {
                std::uint32_t v = mesh.indices[part.first_index + i + k];
                if (v >= mesh.vertices.size()) { valid = false; break; }
                b.min = glm::min(b.min, mesh.vertices[v].position);
                b.max = glm::max(b.max, mesh.vertices[v].position);
            }
            if (valid) boxes.push_back(b);
        }
    }
    // Boxes that touch are one flame: merge until none do. Each round joins every touching pair (a sweep along x
    // finds them) into groups, a group keeping the box and hue of its first member; merged boxes are larger and may
    // touch others, so the rounds go on until one joins nothing. That is the one finest grouping in which no two
    // boxes touch, the same the pairwise merge it replaces reached in cubic time, in the same order.
    constexpr float kSlack = 0.25f;
    auto touch = [](const FlameBox& a, const FlameBox& b) {
        return glm::all(glm::lessThanEqual(a.min, b.max + kSlack)) && glm::all(glm::lessThanEqual(b.min, a.max + kSlack));
    };
    std::vector<std::size_t> parent, order;
    auto find = [&parent](std::size_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    for (bool merged = true; merged;) {
        merged = false;
        const std::size_t n = boxes.size();
        parent.resize(n);
        order.clear();
        for (std::size_t i = 0; i < n; ++i) {
            parent[i] = i;
            // A box with a NaN never touches anything (every comparison is false); it stays out of the sort.
            const FlameBox& b = boxes[i];
            if (!glm::any(glm::isnan(b.min)) && !glm::any(glm::isnan(b.max))) order.push_back(i);
        }
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b) { return boxes[a].min.x < boxes[b].min.x; });
        for (std::size_t a = 0; a < order.size(); ++a) {
            const FlameBox& first = boxes[order[a]];
            for (std::size_t b = a + 1; b < order.size() && boxes[order[b]].min.x <= first.max.x + kSlack; ++b) {
                if (!touch(first, boxes[order[b]])) continue;
                std::size_t ra = find(order[a]), rb = find(order[b]);
                if (ra == rb) continue;
                parent[std::max(ra, rb)] = std::min(ra, rb); // the earlier box leads
                merged = true;
            }
        }
        if (!merged) break;
        std::vector<FlameBox> next;
        std::vector<std::size_t> slot(n);
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t r = find(i);
            if (r == i) {
                slot[i] = next.size();
                next.push_back(boxes[i]);
            } else {
                FlameBox& into = next[slot[r]]; // r < i: already placed
                into.min = glm::min(into.min, boxes[i].min);
                into.max = glm::max(into.max, boxes[i].max);
            }
        }
        boxes = std::move(next);
    }
    return boxes;
}

ZoneFire place_flame(const FlameBox& box, const glm::mat4& transform) {
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (int c = 0; c < 8; ++c) {
        glm::vec3 p((c & 1) ? box.max.x : box.min.x, (c & 2) ? box.max.y : box.min.y,
                    (c & 4) ? box.max.z : box.min.z);
        p = glm::vec3(transform * glm::vec4(p, 1.0f));
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    ZoneFire f;
    f.base = glm::vec3((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, lo.z);
    // The crossed quads stand wider than the flame their texture shows.
    f.width = std::max(hi.x - lo.x, hi.y - lo.y) * 0.7f;
    f.height = (hi.z - lo.z) * 1.1f;
    f.hue = box.hue;
    return f;
}

std::vector<ZoneFire> placement_fires(const Zone& zone) {
    std::unordered_map<std::string, std::vector<FlameBox>> by_actor;
    std::vector<ZoneFire> out;
    for (const auto& p : zone.placements) {
        auto it = by_actor.find(p.actor);
        if (it == by_actor.end()) {
            auto mesh = zone.objects.find(p.actor);
            it = by_actor.emplace(p.actor, mesh == zone.objects.end() ? std::vector<FlameBox>{}
                                                                       : mesh_flames(mesh->second)).first;
        }
        for (const auto& box : it->second) out.push_back(place_flame(box, p.transform));
    }
    return out;
}

namespace {

// The fires (or lights) by 16-unit cell, for the neighbour searches.
struct FireGrid {
    static constexpr float kCell = 16.0f;
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> cells;
    static std::int64_t key(std::int64_t x, std::int64_t y) { return (x << 32) ^ (y & 0xffffffffLL); }
    // Saturated to 32 bits (the key's halves): a far or non-finite position must not cast out of range.
    static std::int64_t at(float v) { return saturate_cast<std::int32_t>(std::floor(v / kCell)); }
    void add(const glm::vec3& p, std::uint32_t i) { cells[key(at(p.x), at(p.y))].push_back(i); }
    // Calls f(i) for everything within `r` of p's cell span.
    template <class F> void near(const glm::vec3& p, float r, F&& f) const {
        for (auto x = at(p.x - r); x <= at(p.x + r); ++x)
            for (auto y = at(p.y - r); y <= at(p.y + r); ++y)
                if (auto it = cells.find(key(x, y)); it != cells.end())
                    for (std::uint32_t i : it->second) f(i);
    }
};

} // namespace

namespace {
// The largest flame kept, in world units: stock flames stand a few units to a few tens; a placement scaled past this
// (or a non-finite one) is a broken file, and its search span would walk billions of grid cells.
constexpr float kMaxFireSize = 1000.0f;
bool plausible(const ZoneFire& f) {
    return std::isfinite(f.base.x) && std::isfinite(f.base.y) && std::isfinite(f.base.z) && std::isfinite(f.width) &&
           std::isfinite(f.height) && std::abs(f.width) <= kMaxFireSize && std::abs(f.height) <= kMaxFireSize;
}
} // namespace

std::vector<ZoneFire> merge_fires(std::vector<ZoneFire> a, const std::vector<ZoneFire>& b) {
    a.insert(a.end(), b.begin(), b.end());
    std::erase_if(a, [](const ZoneFire& f) { return !plausible(f); });
    // The biggest first: each later one is kept unless it stands within a kept one's flame.
    std::stable_sort(a.begin(), a.end(),
                     [](const ZoneFire& x, const ZoneFire& y) { return x.width * x.height > y.width * y.height; });
    std::vector<ZoneFire> out;
    FireGrid grid;
    float widest = 0.0f;
    for (const auto& f : a) {
        glm::vec3 mid = f.base + glm::vec3(0.0f, 0.0f, f.height * 0.5f);
        bool inside = false;
        grid.near(mid, widest, [&](std::uint32_t i) {
            const ZoneFire& k = out[i];
            glm::vec2 d = glm::vec2(mid) - glm::vec2(k.base);
            float r = std::max(k.width * 0.6f, 0.75f);
            inside |= glm::dot(d, d) <= r * r && mid.z >= k.base.z - 1.0f && mid.z <= k.base.z + k.height + 1.0f;
        });
        if (inside) continue;
        grid.add(f.base, static_cast<std::uint32_t>(out.size()));
        widest = std::max(widest, std::max(f.width * 0.6f, 0.75f));
        out.push_back(f);
    }
    return out;
}

std::size_t add_fire_lights(Zone& zone, const std::vector<ZoneFire>& fires, float reach) {
    FireGrid grid;
    for (std::uint32_t i = 0; i < zone.lights.size(); ++i) grid.add(zone.lights[i].position, i);
    std::vector<PointLight> added;
    FireGrid added_grid;
    for (const auto& f : fires) {
        glm::vec3 at = f.base + glm::vec3(0.0f, 0.0f, f.height * 0.5f);
        bool lit = false;
        auto check = [&](const std::vector<PointLight>& lights) {
            return [&](std::uint32_t i) {
                glm::vec3 d = lights[i].position - at;
                lit |= glm::dot(d, d) <= reach * reach;
            };
        };
        grid.near(at, reach, check(zone.lights));
        if (!lit) added_grid.near(at, reach, check(added));
        if (lit) continue;
        PointLight l;
        l.position = at;
        switch (f.hue) {
        case FlameHue::Blue: l.color = {0.25f, 0.4f, 0.8f}; break;
        case FlameHue::Green: l.color = {0.3f, 0.75f, 0.25f}; break;
        case FlameHue::Violet: l.color = {0.6f, 0.25f, 0.75f}; break;
        default: l.color = {0.8f, 0.5f, 0.2f}; break;
        }
        l.radius = std::clamp(f.height * 15.0f, 30.0f, 120.0f);
        added_grid.add(at, static_cast<std::uint32_t>(added.size()));
        added.push_back(l);
    }
    zone.lights.insert(zone.lights.end(), added.begin(), added.end());
    return added.size();
}

std::vector<EnvironmentEmitter> parse_environment_emitters(std::string_view text) {
    std::vector<EnvironmentEmitter> out;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::vector<std::string> fields;
        for (std::size_t f = 0; f <= line.size();) {
            std::size_t caret = line.find('^', f);
            if (caret == std::string_view::npos) caret = line.size();
            fields.emplace_back(line.substr(f, caret - f));
            f = caret + 1;
        }
        if (fields.size() < 5) continue;
        auto number = [](const std::string& s, float& v) {
            char* stop = nullptr;
            v = std::strtof(s.c_str(), &stop);
            return !s.empty() && stop && *stop == '\0';
        };
        EnvironmentEmitter e;
        e.name = fields[0];
        float def = 0.0f;
        if (!number(fields[1], def) || !number(fields[2], e.position.x) || !number(fields[3], e.position.y) ||
            !number(fields[4], e.position.z))
            continue; // the header, or a broken line
        e.definition = saturate_cast<std::int32_t>(def);
        out.push_back(std::move(e));
    }
    return out;
}

std::size_t add_environment_emitters(const GameDir& game, Zone& zone) {
    // A revamped zone loaded from its RoF2 .s3d: the emitters file beside it is the live .eqg's (nektulos' torches
    // and campfires), placed for the new zone and burning in mid-air in the old one.
    if (is_revamped_zone_name(zone.name) && !is_eqg_zone(game, zone.name)) return 0;
    auto text = game.read(lower(zone.name) + "_environmentemitters.txt");
    if (!text) return 0;
    auto emitters = parse_environment_emitters(
        std::string_view(reinterpret_cast<const char*>(text->data()), text->size()));
    if (emitters.empty()) return 0;
    auto edd_data = game.read("environmentemittersnew.edd");
    if (!edd_data) return 0;
    std::vector<EmitterDefinition> edd;
    try {
        edd = parse_edd(*edd_data);
    } catch (const std::exception& e) {
        log::warn("environmentemittersnew.edd: {}", e.what());
        return 0;
    }
    // Index 0 is the EDD's "None" record, as the zone's own list starts with one.
    const auto offset = static_cast<std::uint32_t>(zone.particle_definitions.size());
    zone.particle_definitions.insert(zone.particle_definitions.end(), edd.begin(), edd.end());
    std::size_t added = 0, missing = 0;
    for (const auto& e : emitters) {
        if (e.definition < 1 || static_cast<std::size_t>(e.definition) >= edd.size()) { ++missing; continue; }
        ParticleFixture f;
        f.definition = offset + static_cast<std::uint32_t>(e.definition);
        f.transform[3] = glm::vec4(e.position, 1.0f);
        zone.particle_fixtures.push_back(f);
        ++added;
    }
    if (missing) log::warn("{}: {} environment emitters name EDD records past its end", zone.name, missing);
    return added;
}

} // namespace hailstone::assets
