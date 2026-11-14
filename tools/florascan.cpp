// florascan GAME_DIR RULES_DIR [--indoor] [--masks OUT_DIR] [--sample N] ZONE...
// Hailstone's zone flora (assets/zone_flora.h) over each zone as the client would place it: the rules file it takes
// (its own, else default.txt; --indoor: only its own, as a zone the server calls indoor), how many plants, and how
// many on each ground texture with the texture's flat area. For tuning flora/*.txt.
// --masks writes, per zone, each texture that grew plants as <zone>_<texture>.png: the texture with the texels +green
// rejects darkened, to see what counts as grass. --sample prints N plants' feet spread over the zone (for the viewer's
// --pos).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "assets/flora.h"
#include "assets/zone.h"
#include "assets/zone_flora.h"
#include "core/game_dir.h"
#include "core/png_write.h"

using namespace hailstone;
using namespace hailstone::assets;

namespace {

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Each terrain texture's level ground (normal within 45 degrees of up, as default.txt's max_slope), square units.
std::map<std::string, double> flat_area(const Zone& zone) {
    std::map<std::string, double> out;
    for (const auto& mesh : zone.terrain)
        for (const auto& part : mesh.parts) {
            if (part.material >= mesh.materials.size() || mesh.materials[part.material].frames.empty()) continue;
            std::string tex = mesh.materials[part.material].frames.front();
            if (auto slash = tex.find_last_of('/'); slash != std::string::npos) tex = tex.substr(slash + 1);
            tex = lower(tex);
            const std::uint32_t end = std::min<std::uint32_t>(part.first_index + part.index_count,
                                                              static_cast<std::uint32_t>(mesh.indices.size()));
            for (std::uint32_t i = part.first_index; i + 3 <= end; i += 3) {
                const auto &a = mesh.vertices[mesh.indices[i]], &b = mesh.vertices[mesh.indices[i + 1]],
                           &c = mesh.vertices[mesh.indices[i + 2]];
                glm::vec3 n = glm::cross(b.position - a.position, c.position - a.position);
                const float len = glm::length(n);
                if (!(len > 1e-6f)) continue;
                if (glm::dot(n, a.normal + b.normal + c.normal) < 0.0f) n = -n;
                if (n.z / len >= std::cos(glm::radians(45.0f))) out[tex] += len * 0.5;
            }
        }
    return out;
}

void write_mask(const Zone& zone, const std::string& tex, const std::filesystem::path& out) {
    auto image = zone_ground_texture(zone, tex);
    if (!image) return;
    std::vector<std::byte> px = image->mips[0];
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        auto r = static_cast<std::uint8_t>(px[i]), g = static_cast<std::uint8_t>(px[i + 1]),
             b = static_cast<std::uint8_t>(px[i + 2]);
        if (!is_green_texel(r, g, b)) {
            px[i] = static_cast<std::byte>(r / 5);
            px[i + 1] = static_cast<std::byte>(g / 5);
            px[i + 2] = static_cast<std::byte>(b / 5);
        }
        px[i + 3] = std::byte{0xff};
    }
    write_png_rgba(out, px, image->width, image->height);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: florascan GAME_DIR RULES_DIR [--indoor] [--masks OUT_DIR] [--sample N] ZONE...\n");
        return 2;
    }
    GameDir game{std::filesystem::path(argv[1])};
    const std::filesystem::path rules_dir = argv[2];
    bool outdoor = true;
    std::filesystem::path masks;
    std::size_t sample = 0;
    std::vector<std::string> zones;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--indoor") outdoor = false;
        else if (a == "--masks" && i + 1 < argc) masks = argv[++i];
        else if (a == "--sample" && i + 1 < argc) sample = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        else zones.push_back(a);
    }
    if (!masks.empty()) std::filesystem::create_directories(masks);
    int failures = 0;
    for (const auto& name : zones) {
        try {
            Zone zone = load_zone(game, name);
            if (!zone.flora.empty()) {
                std::printf("%s: has its own flora (%zu plants)\n", name.c_str(), zone.flora.instances.size());
                continue;
            }
            std::string source;
            auto rules = load_zone_flora_rules(rules_dir, name, outdoor, &source);
            if (!rules) {
                std::printf("%s: no rules\n", name.c_str());
                continue;
            }
            // The lawn needs no donor; the other kinds do.
            FloraDonors donor;
            if (!rules->donors.empty()) {
                if (auto opened = open_flora_donors(game, rules->donors)) donor = std::move(*opened);
                else std::printf("%s: flora/%s.txt: no donor opens (the lawn alone)\n", name.c_str(), source.c_str());
            }
            ZoneFlora flora;
            std::unordered_map<std::string, std::size_t> by_texture;
            const auto t0 = std::chrono::steady_clock::now();
            place_zone_flora(zone, *rules, std::move(donor.definitions), flora, zone_ground_textures(zone), &by_texture);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
            std::printf("%s: flora/%s.txt: %zu plants on %zu tiles (%lld ms to grow them all)\n", name.c_str(),
                        source.c_str(), flora.instances.size(), flora.tiles.size(), static_cast<long long>(ms.count()));
            for (std::size_t k = 0; k < sample && !flora.instances.empty(); ++k) {
                const auto& p = flora.instances[(k * 2 + 1) * flora.instances.size() / (sample * 2)];
                std::printf("  plant at %.0f %.0f %.0f\n", p.position.x, p.position.y, p.position.z);
            }
            const auto area = flat_area(zone);
            std::vector<std::pair<std::size_t, std::string>> sorted;
            for (const auto& [tex, n] : by_texture) sorted.emplace_back(n, tex);
            std::sort(sorted.rbegin(), sorted.rend());
            for (const auto& [n, tex] : sorted) {
                auto it = area.find(tex);
                std::printf("  %-24s %8zu plants %10.0f flat\n", tex.c_str(), n, it == area.end() ? 0.0 : it->second);
                if (!masks.empty()) write_mask(zone, tex, masks / (name + "_" + tex + ".png"));
            }
        } catch (const std::exception& e) {
            std::printf("%s: %s\n", name.c_str(), e.what());
            ++failures;
        }
    }
    return failures ? 1 : 0;
}
