// wlddump [--game-dir DIR] ZONE        summary of a classic zone
// wlddump --wld ARCHIVE MEMBER          fragment summary of one WLD file
// wlddump [--game-dir DIR] --sweep      load every classic zone, report failures
// The game directory defaults to $EQ_GAME_DIR.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "assets/pfs.h"
#include "assets/regions.h"
#include "assets/wld.h"
#include "assets/zone.h"
#include "core/game_dir.h"

using namespace hailstone;
using namespace hailstone::assets;

namespace {

void print_histogram(const wld::WldFile& w) {
    std::map<std::uint32_t, std::size_t> hist;
    for (const auto& f : w.fragments()) ++hist[f.type];
    std::printf("  %s: %zu fragments, %s format\n   ", w.label().c_str(), w.fragments().size(),
                w.new_format() ? "new" : "old");
    for (auto [type, n] : hist) std::printf(" %02x:%zu", type, n);
    std::printf("\n");
    if (!w.unknown_types().empty()) {
        std::printf("    not decoded:");
        for (auto [type, n] : w.unknown_types()) std::printf(" %02x", type);
        std::printf("\n");
    }
    // Material flags (0x30; bit 0 is the DLL's two-sided byte), polygon flags
    // (0x36) and the 0x35 global ambient.
    std::map<std::uint32_t, std::size_t> mat_flags, poly_flags;
    std::size_t meshes = 0, uncoloured = 0, no_colours = 0;
    std::map<int, std::size_t> alpha_hist; // colour alpha / 32
    for (const auto& f : w.fragments()) {
        if (const auto* m = f.get<wld::MaterialDef>()) ++mat_flags[m->flags];
        if (const auto* md = f.get<wld::MeshDef>()) {
            ++meshes;
            bool any = false;
            for (auto c : md->colors) {
                any = any || c != 0;
                ++alpha_hist[static_cast<int>(c >> 24) / 32];
            }
            if (md->colors.empty()) ++no_colours;
            if (!any) ++uncoloured;
            for (const auto& p : md->polygons) ++poly_flags[p.flags];
        }
        if (const auto* g = f.get<wld::GlobalAmbient>())
            std::printf("    global ambient (0x35): %08x (stored B,G,R,A)\n", g->color);
    }
    if (!mat_flags.empty()) {
        std::printf("    material flags:");
        for (auto [v, n] : mat_flags) std::printf(" %#x:%zu", v, n);
        std::printf("\n");
    }
    if (meshes) {
        std::printf("    meshes: %zu, %zu without any non-zero colour (%zu with none stored); colour alpha/32:", meshes,
                    uncoloured, no_colours);
        for (auto [v, n] : alpha_hist) std::printf(" %d:%zu", v, n);
        std::printf("; polygon flags:");
        for (auto [v, n] : poly_flags) std::printf(" %#x:%zu", v, n);
        std::printf("\n");
    }
    for (const auto& e : w.errors())
        std::printf("    ERROR fragment %zu type %02x: %s\n", e.index + 1, e.type, e.what.c_str());
}

struct Check {
    std::vector<std::string> problems;
    std::size_t vertices = 0, triangles = 0, materials = 0;
};

void check_mesh(const Mesh& m, const std::string& what, Check& c) {
    c.vertices += m.vertices.size();
    c.triangles += m.indices.size() / 3;
    c.materials += m.materials.size();
    if (m.indices.size() % 3) c.problems.push_back(what + ": index count not a multiple of 3");
    for (auto i : m.indices)
        if (i >= m.vertices.size()) {
            c.problems.push_back(what + ": index out of range");
            break;
        }
    for (const auto& p : m.parts)
        if (p.material >= m.materials.size() || p.first_index + p.index_count > m.indices.size())
            c.problems.push_back(what + ": bad part");
    for (const auto& v : m.vertices)
        if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) || !std::isfinite(v.position.z)) {
            c.problems.push_back(what + ": non-finite vertex");
            break;
        }
}

Check check_zone(const Zone& z) {
    Check c;
    if (z.terrain.empty()) c.problems.push_back("no terrain meshes");
    for (const auto& m : z.terrain) check_mesh(m, "terrain " + m.name, c);
    for (const auto& [name, m] : z.objects) check_mesh(m, "object " + name, c);
    for (const auto& p : z.placements) {
        auto it = z.objects.find(p.actor);
        if (it == z.objects.end()) c.problems.push_back("placement of unknown " + p.actor);
        else if (!p.vertex_colors.empty() && p.vertex_colors.size() != it->second.vertices.size())
            c.problems.push_back("placement colours mismatch " + p.actor);
    }
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(z.bounds_min[i]) || !std::isfinite(z.bounds_max[i]) || z.bounds_min[i] > z.bounds_max[i])
            c.problems.push_back("bad bounds");
    return c;
}

// Parses each WLD the loader reads and returns its parse errors.
std::vector<std::string> wld_errors(const Zone& z, std::map<std::uint32_t, std::size_t>* unknown) {
    std::vector<std::string> out;
    for (const auto& a : z.archives)
        for (const auto& member : a->names()) {
            if (!member.ends_with(".wld")) continue;
            auto data = a->read(member);
            auto w = wld::WldFile::parse(*data, a->label() + ":" + member);
            for (const auto& e : w.errors())
                out.push_back(w.label() + " fragment " + std::to_string(e.index + 1) + ": " + e.what);
            if (unknown)
                for (auto [t, n] : w.unknown_types()) (*unknown)[t] += n;
        }
    return out;
}

// Placed actors (outside the -32768 parking depth) the loader skipped
// because no static mesh has that name.
std::map<std::string, std::size_t> unplaced(const Zone& z) {
    std::map<std::string, std::size_t> out;
    if (z.archives.empty() || !z.archives.front()->contains("objects.wld")) return out;
    auto w = wld::WldFile::parse(*z.archives.front()->read("objects.wld"), "objects.wld");
    for (const auto& f : w.fragments()) {
        const auto* inst = f.get<wld::ActorInstance>();
        if (!inst || !inst->has_location || inst->position.z < -32767.0f) continue;
        auto name = w.string_at(inst->actor);
        if (!z.objects.contains(name)) ++out[name];
    }
    return out;
}

void summary(const GameDir& game, const std::string& name) {
    auto z = load_classic_zone(game, name);
    auto c = check_zone(z);
    std::size_t blend[8] = {};
    std::size_t animated = 0;
    for (const auto& m : z.terrain)
        for (const auto& mat : m.materials) {
            ++blend[static_cast<int>(mat.blend)];
            animated += mat.frames.size() > 1;
        }
    std::size_t placed_colors = 0;
    for (const auto& p : z.placements) placed_colors += !p.vertex_colors.empty();
    std::printf("zone %s\n", z.name.c_str());
    Check terrain;
    for (const auto& m : z.terrain) check_mesh(m, m.name, terrain);
    std::printf("  terrain meshes %zu, vertices %zu, triangles %zu, materials %zu\n", z.terrain.size(),
                terrain.vertices, terrain.triangles, terrain.materials);
    std::printf("  object meshes %zu, vertices %zu, triangles %zu\n", z.objects.size(), c.vertices - terrain.vertices,
                c.triangles - terrain.triangles);
    std::printf("  terrain blend: opaque %zu masked %zu t25 %zu t50 %zu t75 %zu additive %zu invisible %zu; animated %zu\n",
                blend[0], blend[1], blend[2], blend[3], blend[4], blend[5], blend[6], animated);
    std::printf("  placements %zu (%zu with vertex colours), lights %zu\n", z.placements.size(), placed_colors,
                z.lights.size());
    std::printf("  bounds (%.1f %.1f %.1f) - (%.1f %.1f %.1f)\n", z.bounds_min.x, z.bounds_min.y, z.bounds_min.z,
                z.bounds_max.x, z.bounds_max.y, z.bounds_max.z);
    for (const auto& a : z.archives)
        for (const auto& member : a->names())
            if (member.ends_with(".wld")) print_histogram(wld::WldFile::parse(*a->read(member), a->label() + ":" + member));
    for (const auto& [actor, n] : unplaced(z)) std::printf("  skipped %zu placements of %s (no static mesh)\n", n, actor.c_str());
    for (const auto& p : c.problems) std::printf("  PROBLEM %s\n", p.c_str());
}

// Classic zones: every X.s3d that contains X.wld, minus character, object,
// light, armour (globalNN_amr) and equipment (gequip, ldonequip, ...) archives.
std::vector<std::string> classic_zones(const GameDir& game) {
    std::set<std::string> out;
    for (const auto& entry : std::filesystem::directory_iterator(game.root())) {
        auto file = entry.path().filename().string();
        std::string lower;
        for (char ch : file) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!lower.ends_with(".s3d")) continue;
        std::string stem = lower.substr(0, lower.size() - 4);
        if (stem.find("_chr") != std::string::npos || stem.find("_obj") != std::string::npos ||
            stem.find("_lit") != std::string::npos || stem.find("_amr") != std::string::npos ||
            stem.find("equip") != std::string::npos)
            continue;
        try {
            if (PfsArchive::open(entry.path()).contains(stem + ".wld")) out.insert(stem);
        } catch (const std::exception&) {
        }
    }
    return {out.begin(), out.end()};
}

int sweep(const GameDir& game) {
    auto zones = classic_zones(game);
    std::size_t pass = 0;
    std::vector<std::string> failed;
    std::map<std::uint32_t, std::size_t> unknown;
    std::size_t placements = 0, triangles = 0;
    std::map<std::string, std::size_t> unplaced_total;
    for (const auto& name : zones) {
        try {
            auto z = load_classic_zone(game, name);
            auto c = check_zone(z);
            auto errs = wld_errors(z, &unknown);
            c.problems.insert(c.problems.end(), errs.begin(), errs.end());
            placements += z.placements.size();
            auto missing = unplaced(z);
            for (const auto& [actor, n] : missing) unplaced_total[actor] += n;
            triangles += c.triangles;
            if (c.problems.empty()) {
                ++pass;
                continue;
            }
            std::string msg = name + ": " + c.problems.front();
            if (c.problems.size() > 1) msg += " (+" + std::to_string(c.problems.size() - 1) + " more)";
            failed.push_back(msg);
        } catch (const std::exception& e) {
            failed.push_back(name + ": " + e.what());
        }
    }
    std::printf("%zu classic zones: %zu pass, %zu fail (%zu mesh triangles, %zu placements)\n", zones.size(), pass,
                failed.size(), triangles, placements);
    for (const auto& f : failed) std::printf("  FAIL %s\n", f.c_str());
    std::size_t dropped = 0;
    std::vector<std::pair<std::size_t, std::string>> top;
    for (const auto& [actor, n] : unplaced_total) {
        dropped += n;
        top.emplace_back(n, actor);
    }
    std::sort(top.rbegin(), top.rend());
    std::printf("placements without a static mesh (skipped): %zu of %zu actors, e.g.", dropped, unplaced_total.size());
    for (std::size_t i = 0; i < top.size() && i < 8; ++i) std::printf(" %s:%zu", top[i].second.c_str(), top[i].first);
    std::printf("\n");
    std::printf("fragment types not decoded:");
    for (auto [t, n] : unknown) std::printf(" %02x:%zu", t, n);
    std::printf("\n");
    return failed.empty() ? 0 : 1;
}

// Regions of one zone WLD (only the zone's own WLD is parsed).
assets::ZoneRegions load_regions(const GameDir& game, const std::string& zone) {
    auto archive = PfsArchive::open(game.root() / (zone + ".s3d"));
    auto data = archive.read(zone + ".wld");
    if (!data) throw std::runtime_error(zone + ".s3d has no " + zone + ".wld");
    return assets::ZoneRegions::from_wld(wld::WldFile::parse(*data, zone + ".wld"));
}

std::string describe(const assets::RegionInfo& r) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "flags %08x type %u%s%s", r.flags, r.flags & 0xff, r.is_pvp() ? " pvp" : "",
                  r.is_slippery() ? " ice" : "");
    std::string out = buf;
    if (r.zone_line) {
        const auto& z = *r.zone_line;
        if (z.reference) std::snprintf(buf, sizeof buf, " -> zone point %d", z.zone_point);
        else std::snprintf(buf, sizeof buf, " -> zone %u at server (x %.0f, y %.0f, z %.0f) heading %.0f", z.zone_id,
                           z.server_x, z.server_y, z.server_z, z.heading);
        out += buf;
    }
    return out;
}

// wlddump --regions ZONE: every tag with its regions and decoded meaning.
int print_regions(const GameDir& game, const std::string& zone) {
    auto regions = load_regions(game, zone);
    std::printf("%s: %zu BSP nodes, %zu regions\n", zone.c_str(), regions.node_count(), regions.region_count());
    std::map<std::string, std::vector<std::uint32_t>> by_tag;
    for (const auto& r : regions.tagged()) by_tag[r.tag].push_back(r.region);
    for (const auto& [tag, list] : by_tag) {
        auto info = regions.info(list.front());
        std::printf("  %-40s %4zu regions  %s\n", tag.c_str(), list.size(), describe(info).c_str());
    }
    return 0;
}

// The prefix used for counting: up to the first '_' or digit, at most 5 characters.
std::string tag_prefix(const std::string& tag) {
    std::string p;
    for (char c : tag) {
        if (c == '_' || (c >= '0' && c <= '9') || p.size() == 5) break;
        p += c;
    }
    return p;
}

// wlddump --region-survey: tag prefixes and zone lines over every classic zone.
int region_survey(const GameDir& game) {
    struct Count { std::size_t zones = 0, tags = 0, regions = 0; std::string example; };
    std::map<std::string, Count> counts;
    std::size_t zones = 0, with_tree = 0, failed = 0, reference = 0, absolute = 0;
    std::map<std::string, std::size_t> bits;
    std::map<std::uint32_t, std::size_t> types;
    for (const auto& zone : classic_zones(game)) {
        ++zones;
        try {
            auto regions = load_regions(game, zone);
            if (!regions.empty()) ++with_tree;
            std::map<std::string, std::size_t> tags;
            for (const auto& r : regions.tagged()) {
                ++tags[r.tag];
                for (auto [bit, name] : {std::pair{assets::kRegionPvP, "pvp"}, std::pair{assets::kRegionSlippery, "ice"},
                                         std::pair{assets::kRegionFlagM, "M"}, std::pair{assets::kRegionFlagP, "P"},
                                         std::pair{assets::kRegionFlagF, "F"}})
                    if (r.flags & bit) ++bits[name];
                ++types[r.flags & 0xff];
            }
            std::set<std::string> seen;
            for (const auto& [tag, n] : tags) {
                auto p = tag_prefix(tag);
                auto& c = counts[p];
                if (seen.insert(p).second) ++c.zones;
                ++c.tags;
                c.regions += n;
                if (c.example.empty()) c.example = zone + ":" + tag;
                if (auto zl = assets::decode_zone_line(tag)) {
                    if (zl->reference) ++reference;
                    else ++absolute;
                    assets::RegionInfo info;
                    info.flags = assets::region_flags_from_tag(tag);
                    info.zone_line = zl;
                    std::printf("  %-12s %-36s %s\n", zone.c_str(), tag.c_str(), describe(info).c_str());
                }
            }
        } catch (const std::exception& e) {
            ++failed;
            std::printf("  FAIL %s: %s\n", zone.c_str(), e.what());
        }
    }
    std::printf("%zu classic zones, %zu with a BSP tree, %zu failed; zone-line tags: %zu reference, %zu absolute\n",
                zones, with_tree, failed, reference, absolute);
    std::printf("tagged regions by type:");
    for (auto [t, n] : types) std::printf(" %u:%zu", t, n);
    std::printf("; flag bits:");
    for (const auto& [b, n] : bits) std::printf(" %s:%zu", b.c_str(), n);
    std::printf("\n%-6s %6s %6s %8s  example\n", "prefix", "zones", "tags", "regions");
    for (const auto& [p, c] : counts)
        std::printf("%-6s %6zu %6zu %8zu  %s\n", p.c_str(), c.zones, c.tags, c.regions, c.example.c_str());
    return failed ? 1 : 0;
}

// wlddump --zone-lines ZONE [STEP]: samples the zone's bounds on a grid and
// prints, per zone-line tag, the box of sample points inside it (world and
// server coordinates).
int print_zone_lines(const GameDir& game, const std::string& zone, float step) {
    auto z = load_classic_zone(game, zone);
    const auto& regions = z.regions;
    struct Box { glm::vec3 lo{1e30f}, hi{-1e30f}; std::size_t n = 0; };
    std::map<std::string, Box> boxes;
    for (float x = z.bounds_min.x; x <= z.bounds_max.x; x += step)
        for (float y = z.bounds_min.y; y <= z.bounds_max.y; y += step)
            for (float h = z.bounds_min.z; h <= z.bounds_max.z; h += step) {
                glm::vec3 p(x, y, h);
                auto info = regions.lookup(p);
                if (!info.is_zone_line()) continue;
                auto& b = boxes[info.tag];
                b.lo = glm::min(b.lo, p);
                b.hi = glm::max(b.hi, p);
                ++b.n;
            }
    std::printf("%s: bounds (%.0f %.0f %.0f)-(%.0f %.0f %.0f), step %.1f\n", zone.c_str(), z.bounds_min.x,
                z.bounds_min.y, z.bounds_min.z, z.bounds_max.x, z.bounds_max.y, z.bounds_max.z, step);
    for (const auto& [tag, b] : boxes) {
        auto info = regions.lookup((b.lo + b.hi) * 0.5f);
        assets::RegionInfo d;
        d.flags = assets::region_flags_from_tag(tag);
        d.zone_line = assets::decode_zone_line(tag);
        std::printf("  %s\n    world (%.0f %.0f %.0f)-(%.0f %.0f %.0f), %zu samples, centre %s; server x %.0f..%.0f y %.0f..%.0f\n    %s\n",
                    tag.c_str(), b.lo.x, b.lo.y, b.lo.z, b.hi.x, b.hi.y, b.hi.z, b.n,
                    info.tag == tag ? "inside" : "outside", b.lo.y, b.hi.y, b.lo.x, b.hi.x, describe(d).c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    const char* env = std::getenv("EQ_GAME_DIR");
    std::string game_dir = env ? env : "";
    if (args.size() >= 2 && args[0] == "--game-dir") {
        game_dir = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }
    try {
        if (args.size() == 3 && args[0] == "--wld") {
            auto archive = PfsArchive::open(args[1]);
            auto data = archive.read(args[2]);
            if (!data) {
                std::fprintf(stderr, "%s: no file %s\n", args[1].c_str(), args[2].c_str());
                return 1;
            }
            print_histogram(wld::WldFile::parse(*data, args[2]));
            return 0;
        }
        if (args.size() >= 2 && args.size() <= 3 && args[0] == "--zone-lines" && !game_dir.empty())
            return print_zone_lines(GameDir(game_dir), args[1], args.size() == 3 ? std::strtof(args[2].c_str(), nullptr) : 4.0f);
        if (args.size() == 2 && args[0] == "--regions" && !game_dir.empty())
            return print_regions(GameDir(game_dir), args[1]);
        if (args.size() != 1 || game_dir.empty()) {
            std::fprintf(stderr, "usage: wlddump [--game-dir DIR] ZONE | --sweep | --region-survey | --regions ZONE | --zone-lines ZONE [STEP]\n"
                                 "       wlddump --wld ARCHIVE MEMBER\n");
            return 2;
        }
        GameDir game(game_dir);
        if (args[0] == "--sweep") return sweep(game);
        if (args[0] == "--region-survey") return region_survey(game);
        summary(game, args[0]);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wlddump: %s\n", e.what());
        return 1;
    }
}
