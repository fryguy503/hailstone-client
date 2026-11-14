#include "texpack/materials.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#include "assets/jpeg_read.h"
#include "assets/png_read.h"
#include "assets/zone.h"
#include "core/atomic_file.h"
#include "core/float_chars.h"
#include "core/game_dir.h"
#include "core/ini_file.h"
#include "platform/http_get.h"

namespace hailstone::texpack {

namespace fs = std::filesystem;

namespace {

// Bumped when compose_tile changes what it makes, so packs make their material tiles again.
constexpr std::string_view kComposeVersion = "2";

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}

std::string trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return std::string(s);
}

std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    std::istringstream in{std::string(s)};
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

std::optional<double> number(std::string_view text) {
    const std::string t = trim(text);
    double v = 0;
    auto r = float_from_chars(t.data(), t.data() + t.size(), v);
    if (r.ec != std::errc{} || r.ptr != t.data() + t.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}

bool fail(std::string* error, std::string text) {
    if (error) *error = std::move(text);
    return false;
}

// A texture name's stem, lower case: "HHDROCK.BMP" -> "hhdrock".
std::string stem_of(std::string_view name) {
    std::string s = lower(std::string(name));
    if (auto dot = s.rfind('.'); dot != std::string::npos) s.resize(dot);
    return s;
}

std::string fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

} // namespace

const MaterialClass* MaterialRules::find(std::string_view name) const {
    for (const MaterialClass& c : classes)
        if (c.name == name) return &c;
    return nullptr;
}

std::string MaterialRules::signature(const TileRule& tile) const {
    std::string s = std::string("v") + std::string(kComposeVersion) + (tile.sharp ? " sharp" : "");
    for (int k = 0; k < 2; ++k) {
        const std::string& name = k == 0 ? tile.first : tile.second;
        const MaterialClass* c = name.empty() ? nullptr : find(name);
        if (!c) continue;
        const auto& own = k == 0 ? tile.first_colour : tile.second_colour;
        const float* colour = own ? own->data() : c->colour;
        s += " " + c->name + ":" + c->asset + "," + fmt(c->metres) + "," + fmt(c->magnify) + "," + fmt(c->detail) +
             "," + fmt(c->shading) + "," + fmt(colour[0]) + "/" + fmt(colour[1]) + "/" + fmt(colour[2]);
    }
    return s;
}

std::optional<MaterialRules> parse_material_rules(std::string_view materials_txt,
                                                  const std::vector<std::pair<std::string, std::string>>& zone_files,
                                                  std::string* error) {
    MaterialRules rules;
    IniFile ini;
    ini.parse(materials_txt);
    for (const std::string& section : ini.sections()) {
        MaterialClass c;
        c.name = lower(section);
        c.asset = trim(ini.get_string(section, "asset"));
        if (c.asset.empty() || c.asset.find_first_of("/\\. ") != std::string::npos) {
            fail(error, "materials.txt [" + section + "]: asset must be a Poly Haven id");
            return std::nullopt;
        }
        auto num = [&](std::string_view key, double fallback, double lo, double hi) -> std::optional<double> {
            auto text = ini.get(section, key);
            if (!text) return fallback;
            auto v = number(*text);
            if (!v || *v < lo || *v > hi) {
                fail(error, "materials.txt [" + section + "]: " + std::string(key) + " must be " + fmt(lo) + ".." + fmt(hi));
                return std::nullopt;
            }
            return v;
        };
        auto metres = num("metres", 2.0, 0.1, 1000.0);
        auto magnify = num("magnify", 3.0, 0.1, 100.0);
        auto detail = num("detail", 1.0, 0.0, 4.0);
        auto shading = num("shading", 0.0, 0.0, 2.0);
        if (!metres || !magnify || !detail || !shading) return std::nullopt;
        c.metres = *metres;
        c.magnify = *magnify;
        c.detail = static_cast<float>(*detail);
        c.shading = static_cast<float>(*shading);
        if (auto text = ini.get(section, "colour")) {
            auto w = words(*text);
            bool ok = w.size() == 3;
            for (std::size_t i = 0; ok && i < 3; ++i) {
                auto v = number(w[i]);
                ok = v && *v >= 0.0 && *v <= 1.0;
                if (ok) c.colour[i] = static_cast<float>(*v);
            }
            if (!ok) {
                fail(error, "materials.txt [" + section + "]: colour is three numbers 0..1");
                return std::nullopt;
            }
        }
        rules.classes.push_back(std::move(c));
    }
    for (const auto& [zone, text] : zone_files) {
        std::map<std::string, std::array<float, 3>> colours; // the file's colour lines
        const std::size_t first_tile = rules.tiles.size();
        std::istringstream in(text);
        int n = 0;
        for (std::string line; std::getline(in, line);) {
            ++n;
            if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
            line = trim(line);
            if (line.empty()) continue;
            const std::string where = zone + ".txt:" + std::to_string(n) + ": ";
            const auto eq = line.find('=');
            if (lower(line).starts_with("tree ") && eq != std::string::npos) {
                TreeRule t;
                t.zone = lower(zone);
                t.actor = lower(trim(std::string_view(line).substr(5, eq - 5)));
                if (t.actor.ends_with("_actordef")) t.actor.resize(t.actor.size() - 9);
                auto w = words(std::string_view(line).substr(eq + 1));
                for (std::string& x : w) x = lower(x);
                static constexpr std::string_view kStyles[] = {"auto", "pine", "broadleaf", "willow"};
                if (t.actor.empty() || w.empty() || w.size() > 2 ||
                    std::find(std::begin(kStyles), std::end(kStyles), w[0]) == std::end(kStyles)) {
                    fail(error, where + "expected tree <actor> = auto|pine|broadleaf|willow [bark material]");
                    return std::nullopt;
                }
                t.style = w[0];
                if (w.size() == 2) {
                    if (!rules.find(w[1])) {
                        fail(error, where + "no material [" + w[1] + "] in materials.txt");
                        return std::nullopt;
                    }
                    t.bark = w[1];
                }
                rules.trees.push_back(std::move(t));
                continue;
            }
            if (lower(line).starts_with("colour ") && eq != std::string::npos) {
                const std::string name = lower(trim(std::string_view(line).substr(7, eq - 7)));
                auto w = words(std::string_view(line).substr(eq + 1));
                std::array<float, 3> rgb{};
                bool ok = w.size() == 3 && rules.find(name);
                for (std::size_t i = 0; ok && i < 3; ++i) {
                    auto v = number(w[i]);
                    ok = v && *v >= 0.0 && *v <= 1.0;
                    if (ok) rgb[i] = static_cast<float>(*v);
                }
                if (!ok) {
                    fail(error, where + "expected colour <material> = r g b (0..1, a material of materials.txt)");
                    return std::nullopt;
                }
                colours[name] = rgb;
                continue;
            }
            if (eq == std::string::npos) {
                fail(error, where + "expected texture = material [material] [sharp]");
                return std::nullopt;
            }
            TileRule r;
            r.zone = lower(zone);
            r.texture = stem_of(trim(std::string_view(line).substr(0, eq)));
            std::vector<std::string> mats;
            for (std::string& w : words(std::string_view(line).substr(eq + 1))) {
                w = lower(w);
                if (w == "sharp") r.sharp = true;
                else mats.push_back(w);
            }
            if (r.texture.empty() || mats.empty() || mats.size() > 2) {
                fail(error, where + "a texture takes one or two materials");
                return std::nullopt;
            }
            for (const std::string& m : mats) {
                if (!rules.find(m)) {
                    fail(error, where + "no material [" + m + "] in materials.txt");
                    return std::nullopt;
                }
            }
            r.first = mats[0];
            if (mats.size() == 2) r.second = mats[1];
            rules.tiles.push_back(std::move(r));
        }
        for (std::size_t i = first_tile; i < rules.tiles.size(); ++i) {
            TileRule& r = rules.tiles[i];
            if (auto it = colours.find(r.first); it != colours.end()) r.first_colour = it->second;
            if (auto it = colours.find(r.second); !r.second.empty() && it != colours.end()) r.second_colour = it->second;
        }
    }
    return rules;
}

std::optional<MaterialRules> load_material_rules(const fs::path& dir, std::string* error) {
    auto materials = read_text_file(dir / "materials.txt");
    if (!materials) {
        fail(error, "cannot read " + (dir / "materials.txt").string());
        return std::nullopt;
    }
    std::vector<std::pair<std::string, std::string>> zones;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.extension() != ".txt" || p.filename() == "materials.txt") continue;
        if (auto text = read_text_file(p)) zones.emplace_back(p.stem().string(), std::move(*text));
    }
    std::sort(zones.begin(), zones.end());
    return parse_material_rules(*materials, zones, error);
}

std::map<std::string, double> measure_tile_sizes(const assets::Zone& zone) {
    std::map<std::string, std::vector<std::pair<double, double>>> samples; // name -> (units per repeat, area)
    for (const assets::Mesh& m : zone.terrain) {
        for (const assets::MeshPart& p : m.parts) {
            if (p.material >= m.materials.size() || m.materials[p.material].frames.empty()) continue;
            auto& out = samples[stem_of(m.materials[p.material].frames[0])];
            const std::size_t end = std::min<std::size_t>(std::size_t(p.first_index) + p.index_count, m.indices.size());
            for (std::size_t i = p.first_index; i + 2 < end; i += 3) {
                if (m.indices[i] >= m.vertices.size() || m.indices[i + 1] >= m.vertices.size() ||
                    m.indices[i + 2] >= m.vertices.size())
                    continue;
                const assets::Vertex& a = m.vertices[m.indices[i]];
                const assets::Vertex& b = m.vertices[m.indices[i + 1]];
                const assets::Vertex& c = m.vertices[m.indices[i + 2]];
                const double world = 0.5 * static_cast<double>(glm::length(glm::cross(b.position - a.position, c.position - a.position)));
                const glm::vec2 e1 = b.uv - a.uv, e2 = c.uv - a.uv;
                const double uv = 0.5 * std::abs(static_cast<double>(e1.x) * e2.y - static_cast<double>(e1.y) * e2.x);
                if (world < 1e-3 || uv < 1e-9) continue;
                out.emplace_back(std::sqrt(world / uv), world);
            }
        }
    }
    std::map<std::string, double> sizes;
    for (auto& [name, v] : samples) {
        if (v.empty()) continue;
        std::sort(v.begin(), v.end());
        double total = 0;
        for (const auto& s : v) total += s.second;
        double acc = 0;
        for (const auto& s : v) {
            acc += s.second;
            if (acc >= total / 2) {
                sizes[name] = s.first;
                break;
            }
        }
    }
    return sizes;
}

const MaterialTile* MaterialPlan::find(std::uint64_t hash) const {
    auto it = tiles.find(hash);
    return it == tiles.end() ? nullptr : &it->second;
}

MaterialPlan plan_materials(const MaterialRules& rules, const GameDir& game, const std::vector<TextureInfo>& textures,
                            std::uint32_t size, std::vector<std::string>* notes) {
    MaterialPlan plan;
    plan.rules = rules;
    plan.size = size;
    auto note = [&](std::string text) {
        if (notes) notes->push_back(std::move(text));
    };
    std::map<std::string, std::vector<const TileRule*>> by_zone;
    for (const TileRule& r : rules.tiles) by_zone[r.zone].push_back(&r);
    for (const auto& [zone, zone_rules] : by_zone) {
        std::map<std::string, double> sizes;
        try {
            if (assets::is_eqg_zone(game, zone)) {
                note(zone + ": an EQG zone; photo materials are for the classic zones");
                continue;
            }
            sizes = measure_tile_sizes(assets::load_classic_zone(game, zone));
        } catch (const std::exception& e) {
            note(zone + ": " + e.what());
            continue;
        }
        const std::string archive = zone + ".s3d";
        for (const TileRule* r : zone_rules) {
            auto units = sizes.find(r->texture);
            if (units == sizes.end()) {
                note(zone + ": " + r->texture + " is not on the zone's ground");
                continue;
            }
            bool found = false;
            for (const TextureInfo& t : textures) {
                if (!t.archives.contains(archive)) continue;
                bool named = false;
                for (const std::string& n : t.names) named = named || stem_of(n) == r->texture;
                if (!named) continue;
                found = true;
                if (t.alpha != AlphaKind::None || t.masked_variant || t.keyed_variant || t.width != t.height) {
                    note(zone + ": " + r->texture + " has alpha or is not square; it is upscaled instead");
                    continue;
                }
                MaterialTile tile;
                tile.rule = *r;
                tile.units = units->second;
                tile.signature = rules.signature(*r) + " " + std::to_string(size) + " " + fmt(std::round(units->second));
                plan.tiles[t.hash] = std::move(tile);
            }
            if (!found) note(zone + ": no texture " + r->texture + " in " + archive);
        }
    }
    return plan;
}

fs::path material_file(const fs::path& dir, std::string_view asset, std::string_view map) {
    const std::string a(asset);
    return dir / a / (a + "_" + std::string(map) + "_1k.jpg");
}

std::string material_url(std::string_view asset, std::string_view map) {
    // Poly Haven's download host (the CC0 files themselves; api.polyhaven.com lists the same addresses).
    const std::string a(asset);
    return "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/" + a + "/" + a + "_" + std::string(map) + "_1k.jpg";
}

std::vector<std::pair<std::string, std::string>> missing_materials(const MaterialRules& rules, const fs::path& dir) {
    std::vector<std::pair<std::string, std::string>> out;
    std::vector<std::string> seen;
    for (const MaterialClass& c : rules.classes) {
        if (std::find(seen.begin(), seen.end(), c.asset) != seen.end()) continue;
        seen.push_back(c.asset);
        for (std::string_view map : kMaterialMaps) {
            std::error_code ec;
            if (!fs::is_regular_file(material_file(dir, c.asset, map), ec)) out.emplace_back(c.asset, std::string(map));
        }
    }
    return out;
}

bool fetch_materials(const MaterialRules& rules, const fs::path& dir, const FetchOptions& options, std::string* error) {
    const auto missing = missing_materials(rules, dir);
    std::size_t done = 0;
    if (options.progress) options.progress(0, missing.size());
    for (const auto& [asset, map] : missing) {
        if (options.cancelled && options.cancelled()) return fail(error, "cancelled");
        const std::string url = material_url(asset, map);
        std::vector<std::byte> body;
        std::string why;
        bool ok = false;
        if (options.get) {
            ok = options.get(url, body, why);
        } else {
            platform::HttpGetOptions o;
            o.user_agent = options.user_agent;
            o.max_bytes = 64ull << 20;
            o.cancelled = options.cancelled;
            o.allow_url = [](std::string_view u) { return u.starts_with("https://dl.polyhaven.org/"); };
            auto r = platform::http_get(
                url, o,
                [&](std::span<const std::byte> piece) {
                    body.insert(body.end(), piece.begin(), piece.end());
                    return true;
                },
                {});
            ok = r.ok;
            why = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
        }
        if (!ok) return fail(error, url + ": " + why);
        if (!assets::is_jpeg(body)) return fail(error, url + ": not a JPEG");
        const fs::path file = material_file(dir, asset, map);
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        const std::string text(reinterpret_cast<const char*>(body.data()), body.size());
        if (!write_file_atomic(file, text)) return fail(error, "cannot write " + file.string());
        if (options.progress) options.progress(++done, missing.size());
    }
    return true;
}

// ---- images

namespace {

// A box blur of radius r along x or y. Wrapping (a photo, which tiles) or with the edge texels extended (a game
// tile: its neighbour on the ground is another tile, closer to its own edge than to its far side).
void box_pass(Planes& img, std::uint32_t r, bool along_x, bool wrap) {
    const std::uint32_t len = along_x ? img.width : img.height;
    const std::uint32_t lines = along_x ? img.height : img.width;
    if (r == 0 || len == 0) return;
    const std::uint32_t ch = img.channels;
    std::vector<float> line(std::size_t(len) * ch), sum(ch);
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    const auto index = [&](std::int64_t i) -> std::uint32_t {
        if (wrap) return static_cast<std::uint32_t>(((i % len) + len) % len);
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(i, 0, std::int64_t(len) - 1));
    };
    for (std::uint32_t l = 0; l < lines; ++l) {
        auto at = [&](std::uint32_t i, std::uint32_t c) -> float& {
            return along_x ? img.at(i, l, c) : img.at(l, i, c);
        };
        for (std::uint32_t c = 0; c < ch; ++c) {
            double s = 0;
            for (std::int64_t k = -std::int64_t(r); k <= std::int64_t(r); ++k) s += at(index(k), c);
            sum[c] = static_cast<float>(s);
        }
        for (std::uint32_t i = 0; i < len; ++i) {
            for (std::uint32_t c = 0; c < ch; ++c) {
                line[std::size_t(i) * ch + c] = sum[c] * inv;
                sum[c] += at(index(std::int64_t(i) + r + 1), c) - at(index(std::int64_t(i) - r), c);
            }
        }
        for (std::uint32_t i = 0; i < len; ++i)
            for (std::uint32_t c = 0; c < ch; ++c) at(i, c) = line[std::size_t(i) * ch + c];
    }
}

// About a Gaussian of deviation `sigma`: three box passes each way.
Planes blur(Planes img, float sigma, bool wrap) {
    if (sigma <= 0.0f) return img;
    // three boxes of radius r have variance 3 * ((2r + 1)^2 - 1) / 12
    const float r = std::max(0.0f, (std::sqrt(4.0f * sigma * sigma + 1.0f) - 1.0f) / 2.0f);
    const auto ri = static_cast<std::uint32_t>(std::lround(r));
    if (ri == 0) return img;
    for (int i = 0; i < 3; ++i) {
        box_pass(img, ri, true, wrap);
        box_pass(img, ri, false, wrap);
    }
    return img;
}

// Lanczos to w x h with the edges wrapped first, so a tiling image stays seamless.
Planes resize_wrap(const Planes& img, std::uint32_t w, std::uint32_t h) {
    if (img.width == w && img.height == h) return img;
    const std::uint32_t p = 8;
    const Planes padded = pad(img, p, Edge::Wrap);
    const double sx = double(w) / img.width, sy = double(h) / img.height;
    const auto pw = static_cast<std::uint32_t>(std::lround((img.width + 2 * p) * sx));
    const auto ph = static_cast<std::uint32_t>(std::lround((img.height + 2 * p) * sy));
    const Planes big = resize_lanczos(padded, pw, ph);
    return crop(big, static_cast<std::uint32_t>(std::lround(p * sx)), static_cast<std::uint32_t>(std::lround(p * sy)), w, h);
}

std::optional<Planes> load_map(const fs::path& file, std::uint32_t channels, std::string* error) {
    auto bytes = read_file(file);
    if (!bytes) {
        fail(error, "missing " + file.string());
        return std::nullopt;
    }
    std::string why;
    auto img = assets::is_jpeg(*bytes) ? assets::decode_jpeg(*bytes, &why) : assets::decode_png(*bytes, &why);
    if (!img) {
        fail(error, file.string() + ": " + why);
        return std::nullopt;
    }
    return to_planes(*img, channels);
}

float lum(const Planes& p, std::size_t i) {
    return 0.2126f * p.px[i * p.channels] + 0.7152f * p.px[i * p.channels + 1] + 0.0722f * p.px[i * p.channels + 2];
}

} // namespace

std::shared_ptr<MaterialLibrary> MaterialLibrary::load(const MaterialRules& rules, const fs::path& dir, std::string* error) {
    auto lib = std::make_shared<MaterialLibrary>();
    for (const MaterialClass& c : rules.classes) {
        auto diff = load_map(material_file(dir, c.asset, "diff"), 3, error);
        auto nor = diff ? load_map(material_file(dir, c.asset, "nor_dx"), 3, error) : std::nullopt;
        auto rough = nor ? load_map(material_file(dir, c.asset, "rough"), 1, error) : std::nullopt;
        auto disp = rough ? load_map(material_file(dir, c.asset, "disp"), 1, error) : std::nullopt;
        if (!disp) return nullptr;
        lib->add(c.name, {std::move(*diff), std::move(*nor), std::move(*rough), std::move(*disp)});
    }
    return lib;
}

void MaterialLibrary::add(const std::string& material, Maps maps) {
    std::lock_guard lock(mutex_);
    base_[material] = std::make_shared<const Maps>(std::move(maps));
}

std::shared_ptr<const MaterialLibrary::Maps> MaterialLibrary::tiled(const std::string& material, std::uint32_t repeats,
                                                                    std::uint32_t size) {
    const std::uint32_t side = std::max(1u, size / std::max(1u, repeats));
    const std::string key = material + "/" + std::to_string(side);
    std::shared_ptr<const Maps> base;
    {
        std::lock_guard lock(mutex_);
        if (auto it = tiled_.find(key); it != tiled_.end()) return it->second;
        auto b = base_.find(material);
        if (b == base_.end()) return nullptr;
        base = b->second;
    }
    // (made outside the lock: two encoders may both make it, and the first kept wins)
    auto m = std::make_shared<Maps>();
    m->diff = resize_wrap(base->diff, side, side);
    m->nor = resize_wrap(base->nor, side, side);
    m->rough = resize_wrap(base->rough, side, side);
    m->disp = resize_wrap(base->disp, side, side);
    double mean = 0, sq = 0;
    for (float v : m->disp.px) mean += v;
    mean /= double(m->disp.px.size());
    for (float v : m->disp.px) sq += (v - mean) * (v - mean);
    const double dev = std::sqrt(sq / double(m->disp.px.size())) + 1e-6;
    for (float& v : m->disp.px) v = static_cast<float>((v - mean) / dev);
    for (std::uint32_t c = 0; c < 3; ++c) {
        double t = 0;
        for (std::size_t i = c; i < m->diff.px.size(); i += 3) t += m->diff.px[i];
        m->mean[c] = static_cast<float>(t / double(m->diff.px.size() / 3));
    }
    std::lock_guard lock(mutex_);
    return tiled_.try_emplace(key, std::move(m)).first->second;
}

std::uint32_t material_repeats(const MaterialClass& cls, double units) {
    // An EQ unit is about a foot.
    const double r = units / (cls.metres / 0.3048 * cls.magnify);
    const double e = std::floor(std::log2(std::max(r, 1.0)) + 0.3);
    return 1u << static_cast<std::uint32_t>(std::clamp(e, 0.0, 10.0));
}

Outputs compose_tile(const MaterialTile& tile, const MaterialRules& rules, const assets::Image& original,
                     MaterialLibrary& library, std::uint32_t size, bool maps) {
    const MaterialClass* a = rules.find(tile.rule.first);
    const MaterialClass* b = tile.rule.second.empty() ? nullptr : rules.find(tile.rule.second);
    if (!a || (!tile.rule.second.empty() && !b)) throw std::runtime_error("unknown material");
    auto ma = library.tiled(a->name, material_repeats(*a, tile.units), size);
    auto mb = b ? library.tiled(b->name, material_repeats(*b, tile.units), size) : nullptr;
    if (!ma || (b && !mb)) throw std::runtime_error("material images not loaded");
    // The materials' colours in this zone's art (TileRule::first_colour), else their own.
    const float* colour_a = tile.rule.first_colour ? tile.rule.first_colour->data() : a->colour;
    const float* colour_b = b ? (tile.rule.second_colour ? tile.rule.second_colour->data() : b->colour) : nullptr;

    const Planes orig = to_planes(original, 3);
    const std::size_t n = std::size_t(size) * size;
    const float scale = float(size) / float(orig.width);

    // The second material's weight per texel: a 2-means on the original's blurred chromaticity (its painted shadows
    // must not read as a material), seeded with the two materials' colours, then sharpened by their heights.
    std::vector<float> w(n, 0.0f);
    if (b) {
        const Planes blurred = blur(orig, tile.rule.sharp ? 0.8f : 2.0f, false);
        const std::size_t m = std::size_t(orig.width) * orig.height;
        auto feature = [](const float* rgb, float* f) {
            const float s = rgb[0] + rgb[1] + rgb[2] + 0.03f;
            for (int c = 0; c < 3; ++c) f[c] = rgb[c] / s;
            f[3] = 0.15f * (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
        };
        std::vector<float> feat(m * 4);
        for (std::size_t i = 0; i < m; ++i) feature(&blurred.px[i * 3], &feat[i * 4]);
        float centre[2][4];
        feature(colour_a, centre[0]);
        feature(colour_b, centre[1]);
        std::vector<std::uint8_t> label(m);
        auto dist = [&](std::size_t i, int k) {
            float d = 0;
            for (int c = 0; c < 4; ++c) d += (feat[i * 4 + c] - centre[k][c]) * (feat[i * 4 + c] - centre[k][c]);
            return std::sqrt(d);
        };
        for (int iter = 0; iter < 12; ++iter) {
            double sum[2][4] = {};
            std::size_t count[2] = {};
            for (std::size_t i = 0; i < m; ++i) {
                label[i] = dist(i, 1) < dist(i, 0) ? 1 : 0;
                ++count[label[i]];
                for (int c = 0; c < 4; ++c) sum[label[i]][c] += feat[i * 4 + c];
            }
            for (int k = 0; k < 2; ++k)
                if (count[k])
                    for (int c = 0; c < 4; ++c) centre[k][c] = static_cast<float>(sum[k][c] / double(count[k]));
        }
        Planes soft(orig.width, orig.height, 1);
        for (std::size_t i = 0; i < m; ++i) {
            const float d0 = dist(i, 0), d1 = dist(i, 1);
            soft.px[i] = 0.5f + 0.5f * (d0 - d1) / (d0 + d1 + 1e-6f); // 0 first .. 1 second
        }
        const Planes up = resize_lanczos(soft, size, size);
        const std::uint32_t sa = ma->diff.width, sb = mb->diff.width;
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const std::size_t i = std::size_t(y) * size + x;
                const float logit = (up.px[i] * 2.0f - 1.0f) * 8.0f;
                const float dh = mb->disp.at(x % sb, y % sb, 0) - ma->disp.at(x % sa, y % sa, 0);
                w[i] = 1.0f / (1.0f + std::exp(-2.0f * (logit + 0.6f * dh)));
            }
        }
    }

    // Each photo scaled to its material's colour in the art: the composite's brightness steps between materials
    // then match the original's, which the detail transfer below relies on (else an edge gets a bright halo).
    float ga[3], gb[3] = {1.0f, 1.0f, 1.0f};
    for (int c = 0; c < 3; ++c) {
        ga[c] = colour_a[c] / std::max(ma->mean[c], 1e-3f);
        if (b) gb[c] = colour_b[c] / std::max(mb->mean[c], 1e-3f);
    }
    Planes color(size, size, 3), normal(size, size, 3), rough(size, size, 1);
    {
        const std::uint32_t sa = ma->diff.width, sb = mb ? mb->diff.width : 1;
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const std::size_t i = std::size_t(y) * size + x;
                const float t = w[i];
                for (std::uint32_t c = 0; c < 3; ++c) {
                    const float da = ma->diff.at(x % sa, y % sa, c) * ga[c], na = ma->nor.at(x % sa, y % sa, c);
                    const float db = mb ? mb->diff.at(x % sb, y % sb, c) * gb[c] : 0.0f;
                    const float nb = mb ? mb->nor.at(x % sb, y % sb, c) : 0.0f;
                    color.px[i * 3 + c] = da + (db - da) * t;
                    normal.px[i * 3 + c] = na + (nb - na) * t;
                }
                const float ra = ma->rough.at(x % sa, y % sa, 0), rb = mb ? mb->rough.at(x % sb, y % sb, 0) : 0.0f;
                rough.px[i] = ra + (rb - ra) * t;
            }
        }
    }

    // Detail transfer: the original's colour above `radius` of its texels; under it the photos' luminance detail
    // and some of the original's own painted shading, weighted between the two materials.
    {
        const float radius = tile.rule.sharp ? 1.5f : 6.0f;
        const Planes low_o = resize_lanczos(blur(orig, radius, false), size, size);
        const Planes full_o = resize_lanczos(orig, size, size);
        const Planes low_d = blur(color, radius * scale, false);
        const float bd = b ? b->detail : 0.0f, bs = b ? b->shading : 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            const float r_photo = std::clamp((lum(color, i) + 0.02f) / (lum(low_d, i) + 0.02f), 0.25f, 3.0f);
            const float r_orig = std::clamp((lum(full_o, i) + 0.02f) / (lum(low_o, i) + 0.02f), 0.25f, 3.0f);
            const float gp = a->detail + (bd - a->detail) * w[i];
            const float keep = a->shading + (bs - a->shading) * w[i];
            const float ratio = std::clamp(std::pow(r_photo, gp) * std::pow(r_orig, keep), 0.2f, 4.0f);
            for (std::uint32_t c = 0; c < 3; ++c) color.px[i * 3 + c] = std::clamp(low_o.px[i * 3 + c] * ratio, 0.0f, 1.0f);
        }
    }

    Outputs out;
    out.color = to_image(color);
    if (maps) {
        for (std::size_t i = 0; i < n; ++i) {
            float v[3];
            for (int c = 0; c < 3; ++c) v[c] = normal.px[i * 3 + c] * 2.0f - 1.0f;
            const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-6f;
            for (int c = 0; c < 3; ++c) normal.px[i * 3 + c] = v[c] / len * 0.5f + 0.5f;
        }
        out.normal = to_image(normal);
        out.rough = to_image(rough);
    }
    return out;
}

std::map<std::uint64_t, std::string> read_material_record(const fs::path& pack_dir) {
    std::map<std::uint64_t, std::string> record;
    auto text = read_text_file(pack_dir / kMaterialRecordFile);
    if (!text) return record;
    std::istringstream in(*text);
    for (std::string line; std::getline(in, line);) {
        const auto space = line.find(' ');
        if (space == std::string::npos) continue;
        if (auto hash = parse_hash_name(std::string_view(line).substr(0, space)))
            record[*hash] = trim(std::string_view(line).substr(space + 1));
    }
    return record;
}

bool write_material_record(const fs::path& pack_dir, const std::map<std::uint64_t, std::string>& record) {
    std::string text = "# Textures this pack made from photo materials (Texture Studio): <hash> <rules>\n";
    for (const auto& [hash, sig] : record) text += hash_name(hash) + " " + sig + "\n";
    return write_file_atomic(pack_dir / kMaterialRecordFile, text);
}

} // namespace hailstone::texpack
