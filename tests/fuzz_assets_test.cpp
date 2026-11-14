// Fuzz-style tests for the file parsers, driven by small synthetic fixtures (no EQ files needed): PFS/S3D/EQG
// archives, WLD, EQG models/zones/heightmaps/meshes/animations, DDS/BMP/TGA textures, XMI, sound and effect tables
// and ini text. Each seed is cut at many lengths and mutated with a fixed seed; parsers may report an error by
// exception but must not crash, hang or allocate on a bogus length. Run with -DHAILSTONE_SANITIZE=ON for the full value.

#include <doctest/doctest.h>

#include <cstring>
#include <functional>
#include <span>
#include <string>

#include <zlib.h>

#include "assets/character.h"
#include "assets/eqg.h"
#include "assets/eqg_character.h"
#include "assets/gltf_mesh.h"
#include "assets/pfs.h"
#include "assets/regions.h"
#include "assets/sound_files.h"
#include "assets/spell_effects.h"
#include "assets/texture.h"
#include "assets/wld.h"
#include "assets/xmi.h"
#include "core/byte_io.h"
#include "core/ini_file.h"
#include "core/server_profile.h"
#include "core/shared_file_cache.h"
#include "nav/nav_mesh.h"
#include "platform/gpu_ipc.h"
#include "render/gpu_cache_protocol.h"
#include "fuzz_support.h"
#include "pfs_fixtures.h"
#include "wld_fixtures.h"

using namespace hailstone;
using namespace hailstone::assets;
using fuzz::Bytes;
using fuzz::Rng;
using fuzz::Throws;
using test::pfs_fixtures::make_pfs;

namespace {

Bytes text_bytes(const std::string& s) {
    Bytes out;
    for (char c : s) out.push_back(static_cast<std::byte>(c));
    return out;
}

// Cuts each seed at every length up to 700, then at sparser ones, then runs seeded mutations of it, then random
// data behind the seed's first `prefix` bytes (a valid magic), so the deeper parsing is reached.
void sweep(const char* name, const std::vector<Bytes>& seeds, std::size_t prefix, int mutations,
           const std::function<void(std::span<const std::byte>)>& parse) {
    Rng rng(0xa55e7);
    for (const auto& seed : seeds) {
        for (std::size_t n = 0; n <= seed.size(); n += (n < 700 ? 1 : 1 + n / 50))
            fuzz::run_case(name, Bytes(seed.begin(), seed.begin() + static_cast<std::ptrdiff_t>(n)), Throws::Errors, parse);
        for (int i = 0; i < mutations; ++i) fuzz::run_case(name, fuzz::mutate(seed, rng), Throws::Errors, parse);
        for (int i = 0; i < mutations / 4; ++i) {
            Bytes in(seed.begin(), seed.begin() + static_cast<std::ptrdiff_t>(std::min(prefix, seed.size())));
            Bytes tail = fuzz::random_bytes(rng, rng.below(300));
            in.insert(in.end(), tail.begin(), tail.end());
            fuzz::run_case(name, in, Throws::Errors, parse);
        }
    }
}

// ---- PFS ---------------------------------------------------------------------------------------------------------

// ---- WLD ---------------------------------------------------------------------------------------------------------

// A WLD with one fragment of every type id from 0x03 to 0x37 carrying plausible random payloads, so each fragment
// decoder sees mutated bytes, not only the types the fixtures build properly.
Bytes wld_all_types(std::uint64_t seed) {
    test::wld_fixtures::WldBuilder b;
    Rng rng(seed);
    for (std::uint32_t type = 0x03; type <= 0x37; ++type) {
        ByteWriter body;
        body.le<std::int32_t>(b.str("FRAG_" + std::to_string(type)));
        Bytes payload = fuzz::random_bytes(rng, 16 + rng.below(120));
        // Small first words make counts and references plausible rather than all huge.
        for (std::size_t i = 0; i + 3 < payload.size(); i += 4)
            if (rng.chance(50)) payload[i + 1] = payload[i + 2] = payload[i + 3] = std::byte{0};
        body.bytes(payload);
        b.add_raw(type, body.take());
    }
    return b.build();
}

// One 0x12 track of `count` keys, packed (s16) or not (f32), with a 0x13 reference to it.
Bytes wld_track(bool packed, std::uint32_t count, std::uint64_t seed) {
    test::wld_fixtures::WldBuilder b;
    Rng rng(seed);
    ByteWriter body;
    body.le<std::uint32_t>(packed ? 8 : 0);
    body.le(count);
    body.bytes(fuzz::random_bytes(rng, count * (packed ? 16u : 32u)));
    b.add(0x12, b.str("FUZZ_TRACKDEF"), body);
    ByteWriter ref;
    ref.le<std::int32_t>(1);
    ref.le<std::uint32_t>(0);
    b.add(0x13, b.str("FUZZ_TRACK"), ref);
    return b.build();
}

// ---- EQG ---------------------------------------------------------------------------------------------------------

struct Strings {
    Bytes bytes;
    std::int32_t add(const std::string& s) {
        auto at = static_cast<std::int32_t>(bytes.size());
        for (char c : s) bytes.push_back(static_cast<std::byte>(c));
        bytes.push_back(std::byte{0});
        return at;
    }
};

void vec3(ByteWriter& w, float x, float y, float z) {
    w.f32(x);
    w.f32(y);
    w.f32(z);
}

Bytes eqg_model(bool terrain, std::uint32_t version) {
    Strings s;
    auto name = s.add("stone"), shader = s.add("Opaque_MPLBasic.fx"), prop = s.add("e_TextureDiffuse0"), tex = s.add("stone_c.dds");
    ByteWriter w;
    w.le<std::uint32_t>(terrain ? eqg::kTerMagic : eqg::kModMagic);
    w.le<std::uint32_t>(version);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(s.bytes.size()));
    w.le<std::uint32_t>(1);
    w.le<std::uint32_t>(4);
    w.le<std::uint32_t>(2);
    if (!terrain) w.le<std::uint32_t>(0);
    w.bytes(s.bytes);
    w.le<std::uint32_t>(10);
    w.le(name);
    w.le(shader);
    w.le<std::uint32_t>(1);
    w.le(prop);
    w.le<std::uint32_t>(2);
    w.le(tex);
    for (int i = 0; i < 4; ++i) {
        vec3(w, static_cast<float>(i), static_cast<float>(i * 2), 0);
        vec3(w, 0, 0, 1);
        if (version == 3) w.le<std::uint32_t>(0xff102030u);
        w.f32(0.5f);
        w.f32(0.25f);
        if (version == 3) {
            w.f32(0);
            w.f32(0);
        }
    }
    for (std::uint32_t t : {0u, 1u, 2u, 0u, 2u, 3u}) w.le(t);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    return w.take();
}

Bytes eqg_zon() {
    Strings s;
    auto ter = s.add("TER_test.TER"), mod = s.add("OBJ_box.MOD"), pname = s.add("OBJ_box01"), region = s.add("AWT_pool"),
         light = s.add("LIT_lamp");
    ByteWriter w;
    w.le<std::uint32_t>(eqg::kZonMagic);
    w.le<std::uint32_t>(2);
    w.le<std::uint32_t>(static_cast<std::uint32_t>(s.bytes.size()));
    w.le<std::uint32_t>(2);
    w.le<std::uint32_t>(1);
    w.le<std::uint32_t>(1);
    w.le<std::uint32_t>(1);
    w.bytes(s.bytes);
    w.le(ter);
    w.le(mod);
    w.le<std::int32_t>(1);
    w.le(pname);
    vec3(w, 100, 200, 30);
    vec3(w, 1.5f, 0, 0);
    w.f32(2.0f);
    w.le<std::uint32_t>(2);
    w.le<std::uint32_t>(0x11223344u);
    w.le<std::uint32_t>(0x55667788u);
    w.le(region);
    vec3(w, 10, 20, 30);
    vec3(w, -1.5f, 0, 0);
    vec3(w, 5, -6, 7);
    w.le(light);
    vec3(w, 1, 2, 3);
    vec3(w, 1, 0.5f, 0.25f);
    w.f32(40);
    return w.take();
}

const std::string kHeightmapZon = "EQTZP\n*NAME test\n*MINLNG -1 *MAXLNG 0\n*MINLAT 0 *MAXLAT 0\n*UNITSPERVERT 8.0\n*QUADSPERTILE 2\n";

Bytes heightmap_dat() {
    ByteWriter w;
    w.le<std::uint32_t>(20);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.fixed_string("base_c.dds", 11);
    w.le<std::uint32_t>(1);
    w.le<std::int32_t>(99999);
    w.le<std::int32_t>(100000);
    w.le<std::int32_t>(7);
    for (int i = 0; i < 9; ++i) w.f32(static_cast<float>(i));
    for (int i = 0; i < 18; ++i) w.le<std::uint32_t>(0xff808080u);
    for (std::uint8_t f : std::initializer_list<std::uint8_t>{0x00, 0x80, 0x01, 0x00}) w.le(f);
    w.f32(-5.0f);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(2);
    w.fixed_string("grass", 6);
    w.fixed_string("rock", 5);
    w.le<std::uint32_t>(2);
    for (std::uint8_t b : std::initializer_list<std::uint8_t>{0, 255, 255, 0}) w.le(b);
    w.le<std::uint32_t>(1);
    w.fixed_string("OBJ_Rock", 9);
    w.fixed_string("grass", 6);
    w.zeros(8);
    vec3(w, 4, 4, 1);
    vec3(w, 0, 0, 90);
    vec3(w, 1, 1, 1);
    w.le<std::uint8_t>(0);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(1);
    w.fixed_string("camp", 5);
    w.zeros(8);
    vec3(w, 8, 8, 10);
    vec3(w, 0, 0, 0);
    vec3(w, 1, 1, 2);
    w.f32(3.0f);
    return w.take();
}

// ---- textures ----------------------------------------------------------------------------------------------------

Bytes dds(std::uint32_t w, std::uint32_t h, std::uint32_t mips, const char* fourcc, std::uint32_t bits, std::size_t payload) {
    ByteWriter b;
    b.bytes(text_bytes("DDS "));
    b.le<std::uint32_t>(124);
    b.le<std::uint32_t>(0x1007);
    b.le(h);
    b.le(w);
    b.le<std::uint32_t>(0);
    b.le<std::uint32_t>(0);
    b.le(mips);
    b.zeros(44);
    b.le<std::uint32_t>(32);
    if (fourcc) {
        b.le<std::uint32_t>(4);
        b.bytes(text_bytes(fourcc));
        b.zeros(20);
    } else {
        b.le<std::uint32_t>(0x41);
        b.le<std::uint32_t>(0);
        b.le(bits);
        b.le<std::uint32_t>(0x00ff0000);
        b.le<std::uint32_t>(0x0000ff00);
        b.le<std::uint32_t>(0x000000ff);
        b.le<std::uint32_t>(0xff000000);
    }
    b.le<std::uint32_t>(0x1000);
    b.zeros(16);
    Rng rng(w * 31 + h);
    b.bytes(fuzz::random_bytes(rng, payload));
    return b.take();
}

Bytes bmp(std::uint32_t w, std::uint32_t h, std::uint16_t bits, std::uint32_t compression, std::size_t payload) {
    ByteWriter b;
    b.bytes(text_bytes("BM"));
    b.le<std::uint32_t>(0);
    b.le<std::uint32_t>(0);
    std::uint32_t palette = bits <= 8 ? (1u << bits) * 4 : 0;
    b.le<std::uint32_t>(54 + palette);
    b.le<std::uint32_t>(40);
    b.le(w);
    b.le(h);
    b.le<std::uint16_t>(1);
    b.le(bits);
    b.le(compression);
    b.le<std::uint32_t>(0);
    b.zeros(8);
    b.le<std::uint32_t>(bits <= 8 ? 1u << bits : 0);
    b.le<std::uint32_t>(0);
    b.zeros(palette);
    Rng rng(w + h);
    b.bytes(fuzz::random_bytes(rng, payload));
    return b.take();
}

// A PNG of `w` x `h` with random scanlines (filter bytes 0..4) of the given depth and colour type.
Bytes png(std::uint32_t w, std::uint32_t h, std::uint8_t depth, std::uint8_t color, std::uint8_t interlace) {
    ByteWriter b;
    b.bytes(Bytes{std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'}, std::byte{'\r'}, std::byte{'\n'},
                  std::byte{0x1a}, std::byte{'\n'}});
    auto chunk = [&](const char* type, const Bytes& body) {
        ByteWriter c;
        c.bytes(text_bytes(type));
        c.bytes(body);
        Bytes typed = c.take();
        b.be<std::uint32_t>(static_cast<std::uint32_t>(body.size()));
        b.bytes(typed);
        b.be<std::uint32_t>(static_cast<std::uint32_t>(
            crc32(0, reinterpret_cast<const Bytef*>(typed.data()), static_cast<uInt>(typed.size()))));
    };
    ByteWriter ihdr;
    ihdr.be(w);
    ihdr.be(h);
    ihdr.le(depth);
    ihdr.le(color);
    ihdr.zeros(2);
    ihdr.le(interlace);
    chunk("IHDR", ihdr.take());
    Rng rng(w * 7 + h + color);
    if (color == 3) chunk("PLTE", fuzz::random_bytes(rng, 48));
    const int channels = color == 2 ? 3 : color == 4 ? 2 : color == 6 ? 4 : 1;
    // The passes' scanlines (Adam7's seven when interlaced), each led by a filter byte 0..4.
    static constexpr std::uint32_t kAdam7[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                                   {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    static constexpr std::uint32_t kPlain[1][4] = {{0, 0, 1, 1}};
    Bytes raw;
    for (const auto& p : interlace ? std::span<const std::uint32_t[4]>(kAdam7) : std::span<const std::uint32_t[4]>(kPlain)) {
        const std::uint32_t pw = w > p[0] ? (w - p[0] + p[2] - 1) / p[2] : 0;
        const std::uint32_t rows = h > p[1] ? (h - p[1] + p[3] - 1) / p[3] : 0;
        if (!pw || !rows) continue;
        const std::size_t bytes = (std::size_t{pw} * static_cast<std::size_t>(channels * depth) + 7) / 8;
        for (std::uint32_t y = 0; y < rows; ++y) {
            raw.push_back(static_cast<std::byte>(rng.next() % 5));
            Bytes line = fuzz::random_bytes(rng, bytes);
            raw.insert(raw.end(), line.begin(), line.end());
        }
    }
    uLongf cap = compressBound(static_cast<uLong>(raw.size()));
    Bytes packed(cap);
    REQUIRE(compress(reinterpret_cast<Bytef*>(packed.data()), &cap, reinterpret_cast<const Bytef*>(raw.data()),
                     static_cast<uLong>(raw.size())) == Z_OK);
    packed.resize(cap);
    chunk("IDAT", packed);
    chunk("IEND", {});
    return b.take();
}

Bytes tga(std::uint8_t type, std::uint16_t w, std::uint16_t h, std::uint8_t bits, std::size_t payload) {
    ByteWriter b;
    b.le<std::uint8_t>(0);
    b.le<std::uint8_t>(0);
    b.le(type);
    b.zeros(5);
    b.le<std::uint16_t>(0);
    b.le<std::uint16_t>(0);
    b.le(w);
    b.le(h);
    b.le(bits);
    b.le<std::uint8_t>(0x20);
    Rng rng(w + h + type);
    b.bytes(fuzz::random_bytes(rng, payload));
    return b.take();
}

// ---- XMI ---------------------------------------------------------------------------------------------------------

void chunk(Bytes& out, const std::string& id, const Bytes& body) {
    auto idb = text_bytes(id);
    out.insert(out.end(), idb.begin(), idb.end());
    ByteWriter len;
    len.be<std::uint32_t>(static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), len.data().begin(), len.data().end());
    out.insert(out.end(), body.begin(), body.end());
    if (body.size() & 1u) out.push_back(std::byte{0});
}

Bytes xmi_file() {
    Bytes events;
    for (int i : {0x90, 60, 100, 8, 0x80, 60, 0, 0x90, 64, 100, 0x81, 0x00, 0xff, 0x2f, 0}) events.push_back(static_cast<std::byte>(i));
    Bytes form = text_bytes("XMID");
    chunk(form, "TIMB", {std::byte{0}, std::byte{0}});
    chunk(form, "EVNT", events);
    Bytes xdir = text_bytes("XDIR");
    chunk(xdir, "INFO", {std::byte{1}, std::byte{0}});
    Bytes cat = text_bytes("XMID");
    Bytes inner;
    chunk(inner, "FORM", form);
    cat.insert(cat.end(), inner.begin(), inner.end());
    Bytes out;
    chunk(out, "FORM", xdir);
    chunk(out, "CAT ", cat);
    return out;
}

} // namespace

TEST_CASE("fuzz: PFS archives") {
    fuzz::QuietLog quiet;
    Rng rng(1);
    Bytes big = fuzz::random_bytes(rng, 9000);
    Bytes zeros(5000, std::byte{0});
    Bytes archive = make_pfs({{"alpha.wld", big}, {"beta.bmp", zeros}, {"gamma.txt", text_bytes("hello")}});
    // Sanity: the fixture itself is valid.
    auto ok = PfsArchive::from_memory(archive);
    REQUIRE(ok.names().size() == 3);
    REQUIRE(ok.read("alpha.wld").value() == big);
    sweep("PfsArchive", {archive, make_pfs({{"a", {}}}), make_pfs({{"x.dds", big}}, 100)}, 12, 600, [](std::span<const std::byte> d) {
        auto a = PfsArchive::from_memory(Bytes(d.begin(), d.end()));
        for (const auto& n : a.names()) (void)a.read(n);
        (void)a.read("missing");
    });
}

namespace {
// Every part of a built mesh lies within its indices and every index within its vertices: the consumers
// (game/object_collision.cpp, zone_fires, wld_detail) read parts without checking them again.
void check_parts(const Mesh& m) {
    bool ok = true;
    for (const auto& p : m.parts) ok = ok && std::uint64_t{p.first_index} + p.index_count <= m.indices.size();
    for (auto i : m.indices) ok = ok && i < m.vertices.size();
    if (!ok) FAIL_CHECK("a mesh part or index out of range: " << m.name);
}
} // namespace

TEST_CASE("fuzz: WLD files and the meshes built from them") {
    fuzz::QuietLog quiet;
    std::vector<Bytes> seeds = {test::wld_fixtures::mesh_wld(false), test::wld_fixtures::mesh_wld(true), wld_all_types(1), wld_all_types(2)};
    sweep("WldFile", seeds, 28, 700, [](std::span<const std::byte> d) {
        wld::WldFile::ReadScope reads;
        auto w = wld::WldFile::deferred([d] { return std::vector<std::byte>(d.begin(), d.end()); }, "fuzz");
        for (const auto& f : w.fragments()) {
            (void)w.resolve(static_cast<wld::Ref>(f.offset));
            if (const auto* m = f.get<wld::MeshDef>()) check_parts(wld::build_mesh(w, *m, f.name));
            if (const auto* sk = f.get<wld::Skeleton>()) (void)wld::build_skeleton_pose(w, *sk, f.name);
            if (const auto* inst = f.get<wld::ActorInstance>()) (void)wld::placement_transform(*inst);
        }
        (void)w.string_at(-1000000);
        (void)w.find("x");
    });
}

// The encoded-key path the shared cache's mapped WLDs take: tracks stay views into the file's bytes and each key is
// decoded on read, by the WLD's own TrackFrames and by the animation tracks built from them.
TEST_CASE("fuzz: WLD track keys read from the file's bytes") {
    fuzz::QuietLog quiet;
    std::vector<Bytes> seeds = {wld_track(true, 3, 1), wld_track(false, 3, 2), wld_track(true, 1, 3), wld_all_types(3)};
    sweep("WldFile encoded tracks", seeds, 28, 700, [](std::span<const std::byte> d) {
        const auto w = wld::WldFile::parse(ImmutableFileBytes::owned(Bytes(d.begin(), d.end())), "fuzz", true);
        for (const auto& f : w.fragments()) {
            const auto* def = f.get<wld::TrackDef>();
            if (!def) continue;
            float sum = 0.0f;
            for (const auto& key : def->frames) sum += key.scale + key.translation.x + key.rotation.w;
            if (def->frames.size() != 0) sum += def->frames.back().scale;
            const auto track = AnimationTrack::from_wld(def->frames, glm::vec3(0.5f));
            CHECK(track.size() == def->frames.size());
            for (std::size_t i = 0; i < track.size(); ++i) sum += track[i].scale.x;
            const auto bones = BoneFrames::from_wld(def->frames);
            for (const auto& pose : bones) sum += pose.translation.y;
            (void)sum;
        }
    });
}

TEST_CASE("fuzz: EQG models, ZON, heightmap, ECO, TOG, water and character files") {
    fuzz::QuietLog quiet;
    sweep("parse_model", {eqg_model(false, 1), eqg_model(false, 3), eqg_model(true, 1), eqg_model(true, 3)}, 8, 500,
          [](std::span<const std::byte> d) {
              for (bool terrain : {false, true}) {
                  auto m = eqg::parse_model(d, terrain);
                  eqg::MeshStats stats;
                  check_parts(eqg::build_mesh(m, "OBJ", stats));
              }
          });
    sweep("parse_zon", {eqg_zon()}, 8, 800, [](std::span<const std::byte> d) { (void)eqg::parse_zon(d); });
    {
        auto options = eqg::parse_heightmap_zon(text_bytes(kHeightmapZon));
        sweep("parse_heightmap", {heightmap_dat()}, 4, 800, [&](std::span<const std::byte> d) { (void)eqg::parse_heightmap(options, d); });
    }
    sweep("parse_heightmap_zon", {text_bytes(kHeightmapZon)}, 6, 300, [](std::span<const std::byte> d) {
        (void)eqg::is_heightmap_zon(d);
        (void)eqg::parse_heightmap_zon(d);
    });
    sweep("parse_eco", {text_bytes("*TEXTUREPART\n*LAYER\n*DETAILMAP Textures\\Grass_C.dds\n*DETAILREPEAT 4\n*END_LAYER\n*LAYER\n*MINSLOPE 30\n")}, 4, 300,
          [](std::span<const std::byte> d) { (void)eqg::parse_eco(d); });
    sweep("parse_tog", {text_bytes("*BEGIN_OBJECT\n*NAME Tent\n*POSITION 1 2 3\n*ROTATION 0 0 90\n*SCALE 2\n*END_OBJECT\n")}, 4, 300,
          [](std::span<const std::byte> d) { (void)eqg::parse_tog(d); });
    sweep("parse_water", {text_bytes("*WATERSHEETDATA 0\n*INDEX 0\n*MINX 0 *MINY 0 *MAXX 10 *MAXY 10 *ZHEIGHT 5\n*END\n")}, 4, 300,
          [](std::span<const std::byte> d) {
              (void)eqg::parse_water_file(d);
              (void)eqg::parse_water_sheets(d);
          });
    sweep("light_definition_color", {text_bytes("*COLOR 4294934528\n*INTENSITY 1.5\n")}, 4, 300,
          [](std::span<const std::byte> d) {
              (void)eqg::light_definition_color(std::string_view(reinterpret_cast<const char*>(d.data()), d.size()));
          });
    // Character meshes and animations: only a magic is known here, so random data behind "EQGS"/"EQGM" and "EQGA".
    Rng rng(9);
    std::vector<Bytes> mesh_seeds, anim_seeds;
    for (const char* magic : {"EQGS", "EQGM"}) {
        Bytes b = text_bytes(magic);
        Bytes tail = fuzz::random_bytes(rng, 400);
        for (std::size_t i = 0; i + 3 < tail.size(); i += 4) tail[i + 1] = tail[i + 2] = tail[i + 3] = std::byte{0};
        b.insert(b.end(), tail.begin(), tail.end());
        mesh_seeds.push_back(b);
    }
    for (int i = 0; i < 2; ++i) {
        Bytes b = fuzz::random_bytes(rng, 300);
        for (std::size_t j = 0; j + 3 < b.size(); j += 4) b[j + 1] = b[j + 2] = b[j + 3] = std::byte{0};
        anim_seeds.push_back(b);
    }
    sweep("parse_mesh", mesh_seeds, 4, 600, [](std::span<const std::byte> d) { (void)eqg_character::parse_mesh(d, "x"); });
    sweep("parse_animation", anim_seeds, 4, 600, [](std::span<const std::byte> d) { (void)eqg_character::parse_animation(d); });
}

TEST_CASE("fuzz: textures (DDS, BMP, TGA, PNG)") {
    fuzz::QuietLog quiet;
    std::vector<Bytes> seeds = {dds(16, 16, 3, "DXT1", 0, 8 * 16 + 8 * 4 + 8), dds(8, 8, 1, "DXT5", 0, 16 * 4), dds(4, 4, 1, nullptr, 32, 64),
                                dds(4, 4, 1, nullptr, 24, 48), bmp(8, 8, 8, 0, 64), bmp(8, 8, 24, 0, 8 * 8 * 3), bmp(8, 8, 8, 1, 90),
                                bmp(8, 8, 4, 2, 90), bmp(8, 8, 32, 0, 256), tga(2, 8, 8, 24, 192), tga(10, 8, 8, 32, 120), tga(1, 8, 8, 8, 80),
                                png(8, 8, 8, 6, 0), png(8, 8, 1, 0, 0), png(8, 8, 4, 3, 1), png(4, 4, 16, 2, 0)};
    sweep("decode_texture", seeds, 24, 600, [](std::span<const std::byte> d) {
        std::string err;
        for (bool transparent : {false, true}) {
            TextureOptions opt;
            opt.palette_index0_transparent = transparent;
            auto img = decode_texture(d, opt, &err);
            if (img) (void)to_rgba8(*img);
        }
        (void)describe_texture(d);
    });
}

// The shared GPU texture protocol (render/gpu_cache_protocol.h, platform/gpu_ipc.h): what one client process reads
// from another. The transport's frame header, the broker's request parsing and the texture recipe it decodes.
TEST_CASE("fuzz: GPU texture broker frames, requests and recipes") {
    namespace gpu = render::gpu_cache;
    sweep("gpu_ipc_wire::decode_header", {platform::gpu_ipc_wire::encode_header({platform::gpu_ipc_wire::Kind::Request, 64, 1, false})},
          4, 400, [](std::span<const std::byte> d) { (void)platform::gpu_ipc_wire::decode_header(d); });

    assets::Image image;
    image.width = image.height = 4;
    image.format = assets::PixelFormat::BC1;
    image.mips = {Bytes(8), Bytes(8), Bytes(8)};
    const auto recipe = gpu::encode_texture(image, false);
    sweep("decode_gpu_cache_texture", {recipe}, 8, 600,
          [](std::span<const std::byte> d) { (void)gpu::decode_texture(d); });

    std::vector<Bytes> requests;
    gpu::Request r;
    r.op = gpu::Op::Hello;
    r.identity.model = "GPU";
    requests.push_back(gpu::encode_request(r));
    r = {};
    r.op = gpu::Op::Texture;
    r.digest = gpu::texture_key(image, false);
    r.recipe = recipe;
    requests.push_back(gpu::encode_request(r));
    r = {};
    r.op = gpu::Op::Release;
    r.epoch = 3;
    r.serial = 4;
    requests.push_back(gpu::encode_request(r));
    r = {};
    r.op = gpu::Op::RetireTexture;
    requests.push_back(gpu::encode_request(r));
    sweep("gpu broker request", requests, 8, 600, [](std::span<const std::byte> d) {
        const auto request = gpu::decode_request(d);
        if (request.op == gpu::Op::Texture) (void)gpu::decode_texture(request.recipe);
    });
}

TEST_CASE("fuzz: binary glTF models") {
    fuzz::QuietLog quiet;
    Mesh m;
    m.vertices.resize(4);
    for (std::size_t i = 0; i < 4; ++i) m.vertices[i].position = glm::vec3(float(i), float(i % 2), 1.0f);
    m.indices = {0, 1, 2, 1, 2, 3};
    Material mat;
    mat.frames = {"leaf.png"};
    mat.blend = BlendMode::Masked;
    m.materials = {mat};
    m.parts = {{0, 6, 0, true}};
    sweep("read_glb", {write_glb(m, 1)}, 24, 600, [](std::span<const std::byte> d) { (void)read_glb(d); });
}

TEST_CASE("fuzz: XMI, sound and effect tables, ini and tag text") {
    fuzz::QuietLog quiet;
    sweep("parse_xmi", {xmi_file()}, 12, 800, [](std::span<const std::byte> d) { (void)parse_xmi(d); });

    Rng rng(5);
    Bytes eff = fuzz::random_bytes(rng, kEffSoundRecordSize * 3 + 11);
    sweep("parse_sounds_eff", {eff}, 4, 300, [](std::span<const std::byte> d) { (void)parse_sounds_eff(d); });
    sweep("parse_spell_effect_sounds", {fuzz::random_bytes(rng, 800)}, 4, 300, [](std::span<const std::byte> d) { (void)parse_spell_effect_sounds(d); });
    sweep("parse_spell_effects", {fuzz::random_bytes(rng, 1200)}, 4, 300, [](std::span<const std::byte> d) { (void)parse_spell_effects(d); });
    {
        Bytes edd = text_bytes("EDD110");
        edd.resize(8 + 0x1a0, std::byte{0});
        sweep("parse_edd", {edd}, 8, 500, [](std::span<const std::byte> d) { (void)parse_edd(d); });
        sweep("parse_edd_record", {Bytes(0x1a0, std::byte{0x3f})}, 0, 300, [](std::span<const std::byte> d) { (void)parse_edd_record(d); });
    }

    auto as_text = [](std::span<const std::byte> d) { return std::string(reinterpret_cast<const char*>(d.data()), d.size()); };
    sweep("sound text", {text_bytes("EMIT\nfoo bar\nbaz.wav\nLOOP\nloop1\n"), text_bytes("1^Alpha^\n2^Unknown^\n3^Gamma\n999999999999^x")}, 0, 300,
          [&](std::span<const std::byte> d) {
              (void)parse_sound_bank(as_text(d));
              (void)parse_sound_assets(as_text(d));
              (void)parse_mp3_index(as_text(d));
              (void)parse_emt(as_text(d));
              (void)parse_race_sounds(as_text(d));
          });
    sweep("ini", {text_bytes("[HotButtons]\nPage1Button1=B0,@-1,0000,0,Melee\n[Socials]\nX=1\n; comment\n=novalue\n[\nkey\n"), text_bytes("[a]\nk=99999999999\nk=-5\n")}, 0, 400,
          [&](std::span<const std::byte> d) {
              IniFile ini;
              ini.parse(as_text(d));
              (void)ini.get_int("a", "k", 1);
              (void)ini.get_int("HotButtons", "Page1Button1", 2);
              (void)ini.to_string();
          });
    sweep("eqhost.txt", {text_bytes("\xEF\xBB\xBF[LoginServer]\r\nHost=login.example.org:5999\r\n"), text_bytes("[LoginServer]\nHost=[2001:db8::1]:65535\nHost=2001:db8::1\nHost=x:99999999999\n")}, 0, 400,
          [&](std::span<const std::byte> d) { (void)profile_from_eqhost(as_text(d)); });
    sweep("zone line tags", {text_bytes("DRNTP00TOP_ZONE_NEKTULOS_000000000000_000000000000"), text_bytes("WTN_pool"), text_bytes("DRP_x")}, 0, 300,
          [&](std::span<const std::byte> d) {
              (void)decode_zone_line(as_text(d));
              (void)region_flags_from_tag(as_text(d));
          });
}

// ---- The navigation cache (nav/nav_mesh.h) -------------------------------------------------------------------------
// decode_cache, then every tile it gives to Detour through NavMesh::add_tile's checks, then a search over what was
// added: a broken file must be refused before Detour walks it.

TEST_CASE("fuzz: navigation cache files and their tiles") {
    fuzz::QuietLog quiet;
    std::vector<glm::vec3> v;
    auto quad = [&](float x0, float y0, float x1, float y1, float z) {
        v.insert(v.end(), {{x0, y0, z}, {x1, y1, z}, {x1, y0, z}, {x0, y0, z}, {x0, y1, z}, {x1, y1, z}});
    };
    quad(0, 0, 100, 100, 0);
    quad(40, 40, 60, 60, 10);
    auto geo = nav::InputGeometry::from_world(v, {}, {});
    nav::CacheHeader h;
    h.geometry_hash = geo->hash();
    h.params_hash = geo->params().hash();
    h.origin = geo->bmin();
    h.tile_size = geo->params().tile_size();
    h.tiles_x = geo->tiles_x();
    h.tiles_y = geo->tiles_y();
    std::vector<nav::CachedTile> tiles;
    for (int ty = 0; ty < h.tiles_y; ++ty)
        for (int tx = 0; tx < h.tiles_x; ++tx)
            if (auto d = nav::build_tile(*geo, tx, ty); !d.empty()) tiles.push_back({tx, ty, std::move(d)});
    REQUIRE_FALSE(tiles.empty());
    Bytes seed = nav::encode_cache(h, tiles);
    // The file header (52 bytes) and the first tile's place and size (12): mutations reach the tile data.
    sweep("decode_cache", {seed}, 64, 1500, [&](std::span<const std::byte> d) {
        auto got = nav::decode_cache(d, h);
        if (!got) return;
        nav::NavMesh mesh;
        if (!mesh.init(h.origin, h.tile_size, h.tiles_x, h.tiles_y, geo->params())) return;
        for (const nav::CachedTile& t : *got) (void)mesh.add_tile(t.data);
        (void)mesh.find_path({10, 10, 0}, {90, 90, 0});
        (void)mesh.nearest({50, 50, 10});
    });
    // The file's checksum stops nearly every mutation above, so the tiles' own checks get mutated tiles directly:
    // Detour's tile header (100 bytes or so) kept, the rest cut, changed and replaced.
    sweep("add_tile", {Bytes(reinterpret_cast<const std::byte*>(tiles[0].data.data()),
                             reinterpret_cast<const std::byte*>(tiles[0].data.data()) + tiles[0].data.size())},
          100, 3000, [&](std::span<const std::byte> d) {
              nav::NavMesh mesh;
              if (!mesh.init(h.origin, h.tile_size, h.tiles_x, h.tiles_y, geo->params())) return;
              std::vector<unsigned char> tile(d.size());
              if (!d.empty()) std::memcpy(tile.data(), d.data(), d.size());
              if (!mesh.add_tile(tile)) return;
              for (glm::vec3 p : {glm::vec3(10, 10, 0), glm::vec3(50, 50, 10), glm::vec3(90, 90, 0)}) {
                  (void)mesh.nearest(p);
                  (void)mesh.find_path({10, 10, 0}, p);
                  (void)mesh.walkable_line({10, 10, 0}, p);
              }
              mesh.mark_areas(tiles[0].tx, tiles[0].ty, [](const glm::vec3&) { return nav::Area::Water; }, 2.0f);
              mesh.for_each_polygon([](std::span<const glm::vec3>, nav::Area) {});
          });
}
