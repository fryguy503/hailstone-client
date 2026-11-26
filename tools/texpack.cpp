// texpack: builds a texture pack (docs/graphics.md, "Texture packs") from the game's archives.
//
//   texpack export ASSETDIR WORKDIR [--only PREFIX,...]
//     Every texture in the .s3d/.eqg archives (those whose names start with a PREFIX), once per content:
//     WORKDIR/src/<hash>.png upright as in an image viewer, <hash>.m.png the masked variant of an 8-bit BMP (palette
//     index 0 transparent) when it has such texels, <hash>.k.png the variant without its colour key of a texture
//     painted on one (assets::remove_color_key), and WORKDIR/manifest.jsonl, one line per texture.
//   texpack build WORKDIR PACKDIR [--up DIR]
//     WORKDIR/up/ (or DIR)<hash>[.m|.k|.n|.r].png (tools/texpack/upscale.py's output) encoded with their mips into
//     PACKDIR: colour BC7, normals BC5, roughness BC4, as DX10 DDS files PACKDIR/by-hash/<hash>[.m|.k|.n|.r].dds: keyed by the
//     game file's content, never its name (w1 is a different image in each city), so a server's changed file keeps
//     its own look.
//
// <hash> is texture_content_hash of the game's file (assets/texture_overrides.h). The work is texpack/texpack.h's,
// shared with the texture studio (tools/texstudio), which does all three steps itself without the PNGs between.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/atomic_file.h"
#include "core/game_dir.h"
#include "core/png_write.h"
#include "texpack/texpack.h"

using namespace hailstone::texpack;
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int run_export(const fs::path& assets, const fs::path& work, const std::vector<std::string>& only) {
    const auto archives = list_archives({assets}, only);
    fs::create_directories(work / "src");
    ScanResult scanned = scan(archives);
    for (const std::string& e : scanned.errors) std::fprintf(stderr, "%s\n", e.c_str());

    // The images, each archive opened once (the textures in their source's order).
    std::vector<const TextureInfo*> order;
    for (const TextureInfo& t : scanned.textures) order.push_back(&t);
    std::sort(order.begin(), order.end(), [](const TextureInfo* a, const TextureInfo* b) {
        return a->source_archive != b->source_archive ? a->source_archive < b->source_archive : a->hash < b->hash;
    });
    // Contiguous runs of one archive, shared out among the threads.
    std::vector<std::pair<std::size_t, std::size_t>> runs;
    for (std::size_t i = 0; i < order.size();) {
        std::size_t j = i;
        while (j < order.size() && order[j]->source_archive == order[i]->source_archive) ++j;
        runs.emplace_back(i, j);
        i = j;
    }
    std::atomic<std::size_t> next{0}, failures{scanned.errors.size()};
    auto worker = [&] {
        auto cache = make_archive_cache();
        for (std::size_t r; (r = next++) < runs.size();)
            for (std::size_t i = runs[r].first; i < runs[r].second; ++i) {
                const TextureInfo& t = *order[i];
                std::string why;
                auto s = read_sources(t, cache.get(), &why);
                if (!s) {
                    ++failures;
                    std::fprintf(stderr, "%s: %s\n", t.source_name.c_str(), why.c_str());
                    continue;
                }
                const std::string base = hash_name(t.hash);
                auto png = [&](const char* sfx, const hailstone::assets::Image& img) {
                    const fs::path path = work / "src" / (base + sfx);
                    if (hailstone::write_png_rgba(path, img.mips[0], img.width, img.height)) return;
                    ++failures;
                    std::fprintf(stderr, "%s: cannot write\n", path.string().c_str());
                };
                png(".png", s->color);
                if (s->masked) png(".m.png", *s->masked);
                if (s->keyed) png(".k.png", *s->keyed);
            }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    FILE* out = std::fopen((work / "manifest.jsonl").string().c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "texpack: cannot write %s\n", (work / "manifest.jsonl").string().c_str());
        return 1;
    }
    for (const TextureInfo& t : scanned.textures) std::fprintf(out, "%s\n", manifest_line(t).c_str());
    if (std::ferror(out) != 0 || std::fclose(out) != 0) {
        std::fprintf(stderr, "texpack: cannot write %s\n", (work / "manifest.jsonl").string().c_str());
        return 1;
    }
    std::printf("texpack: %zu archives, %zu textures, %zu failures\n", archives.size(), scanned.textures.size(),
                failures.load());
    return failures ? 1 : 0;
}

int run_build(const fs::path& work, const fs::path& pack, const std::string& up_name) {
    std::vector<TextureInfo> jobs;
    std::ifstream manifest(work / "manifest.jsonl");
    for (std::string line; std::getline(manifest, line);)
        if (auto t = parse_manifest_line(line)) jobs.push_back(std::move(*t));
    const fs::path up = work / up_name, out_dir = pack / "by-hash";
    fs::create_directories(out_dir);
    init_encoders();

    std::atomic<std::size_t> next{0}, written{0}, failures{0};
    auto worker = [&] {
        for (std::size_t i; (i = next++) < jobs.size();) {
            const TextureInfo& job = jobs[i];
            const std::string hash = hash_name(job.hash);
            for (FileKind kind : {FileKind::Color, FileKind::Masked, FileKind::Keyed, FileKind::Normal, FileKind::Rough}) {
                const std::string sfx(suffix(kind));
                const fs::path src = up / (hash + sfx + ".png"), dst = out_dir / (hash + sfx + ".dds");
                std::error_code ec;
                if (!fs::exists(src, ec)) continue;
                if (fs::exists(dst, ec) && fs::last_write_time(dst, ec) >= fs::last_write_time(src, ec)) continue;
                auto bytes = hailstone::read_file(src);
                std::string why;
                auto decoded = bytes ? hailstone::assets::decode_texture(*bytes, {}, &why) : std::nullopt;
                if (!decoded) {
                    ++failures;
                    std::fprintf(stderr, "%s: %s\n", src.string().c_str(), bytes ? why.c_str() : "cannot be read");
                    continue;
                }
                auto file = encode_pack_file(hailstone::assets::to_rgba8(*decoded), kind, job.bmp);
                // A failed write or rename leaves the old file (or none), never a short one a later run would skip.
                if (!hailstone::write_file_atomic(dst, std::string_view(reinterpret_cast<const char*>(file.data()), file.size()))) {
                    ++failures;
                    std::fprintf(stderr, "%s: cannot write\n", dst.string().c_str());
                    continue;
                }
                ++written;
            }
            if ((i + 1) % 1000 == 0) std::printf("%zu/%zu textures\n", i + 1, jobs.size()), std::fflush(stdout);
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    std::printf("texpack: %zu textures, %zu files written, %zu failures\n", jobs.size(), written.load(), failures.load());
    return failures ? 1 : 0;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    for (std::size_t p = 0; p <= s.size();) {
        std::size_t q = std::min(s.find(',', p), s.size());
        if (q > p) out.push_back(lower(s.substr(p, q - p)));
        p = q + 1;
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::vector<std::string> args(argv + 1, argv + argc);
        if (args.size() >= 3 && args[0] == "export") {
            std::vector<std::string> only;
            if (args.size() == 5 && args[3] == "--only") only = split(args[4]);
            else if (args.size() != 3) goto usage;
            return run_export(args[1], args[2], only);
        }
        if (args.size() >= 3 && args[0] == "build") {
            std::string up = "up";
            if (args.size() == 5 && args[3] == "--up") up = args[4];
            else if (args.size() != 3) goto usage;
            return run_build(args[1], args[2], up);
        }
    usage:
        std::fprintf(stderr, "usage: texpack export ASSETDIR WORKDIR [--only PREFIX,...]\n"
                             "       texpack build WORKDIR PACKDIR [--up DIR]\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "texpack: %s\n", e.what());
        return 1;
    }
}
