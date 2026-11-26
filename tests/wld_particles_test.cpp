#include <doctest/doctest.h>

#include <limits>

#include <cmath>
#include <charconv>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

#include "assets/wld.h"
#include "assets/wld_particles.h"
#include "assets/character.h"
#include "assets/object_animation.h"
#include "assets/zone.h"
#include "core/byte_io.h"
#include "game/particles.h"
#include "game/appearance.h"
#include "game_files.h"
#include "pfs_fixtures.h"
#include "wld_fixtures.h"

using namespace hailstone;
using namespace hailstone::assets;
using namespace hailstone::test::wld_fixtures;

namespace {

// bazaar_obj.wld's L307_PCD body after the name (fragment 675, the
// HAVSTLIGHT300 lantern's flame): flags 4, selector 3, mode 3, bits 0x30500,
// capacity 30, life 750 ms, speed 0.7, direction (0, 1, 0), interval 30 ms,
// size 0.5, colour 0x3fffffff; the trailing word is its 0x26 (set by the test).
ByteWriter l307_body(std::int32_t sprite, std::uint32_t bits = 0x30500) {
    ByteWriter w;
    for (std::uint32_t v : {4u, 3u, 3u, bits, 30u}) w.le(v);
    for (int i = 0; i < 4; ++i) w.f32(0.0f);
    w.le(std::uint32_t{0}); // duration: indefinite
    w.f32(0.3f);            // +0x28 (not read by mode 3)
    w.f32(25.0f);           // +0x2c spread angle
    w.le(std::uint32_t{750});
    w.f32(0.7f);
    for (float f : {0.0f, 1.0f, 0.0f}) w.f32(f);
    w.le(std::uint32_t{30});
    w.f32(0.5f);
    w.le(std::uint32_t{0x3fffffff});
    w.le(sprite);
    return w;
}

// The cloud's texture chain as the archives store it: 0x03 GENG00.DDS <- 0x04
// (flags 0x18, sleep 100) <- 0x05 <- 0x26 (material 0x80000017).
std::int32_t add_sprite(WldBuilder& b) {
    auto bmp = b.add(0x03, b.str("GENG00.DDS"), bitmap_name("GENG00.DDS"));
    ByteWriter info;
    info.le(std::uint32_t{0x18});
    info.le(std::uint32_t{1});
    info.le(std::uint32_t{100});
    info.le(bmp);
    auto def = b.add(0x04, b.str("L307_SPRITE"), info);
    ByteWriter ref;
    ref.le(def);
    ref.le(std::uint32_t{0});
    auto simple = b.add(0x05, 0, ref);
    ByteWriter blit;
    blit.le(std::uint32_t{0});
    blit.le(simple);
    blit.le(std::uint32_t{0x80000017});
    return b.add(0x26, b.str("I_L307_SPB"), blit);
}

bool near(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 1e-4f; }

} // namespace

TEST_CASE("WLD actor and held-item clouds survive model building, dressing and rest-pose attachment") {
    WldBuilder b;
    auto sprite = add_sprite(b);
    auto cloud = b.add(0x34, b.str("FIXTURE_PCD"), l307_body(sprite));
    auto bad_cloud = b.add(0x34, b.str("MISSING_TEXTURE_PCD"), l307_body(0));
    auto mat = b.add(0x30, b.str("FIXTURE_MDF"), material(0x80000001, 0));
    auto palette = b.add(0x31, 0, refs({mat, mat, mat}));
    auto geometry = b.add(0x36, b.str("FIXTURE_DMSPRITEDEF"), mesh(palette, false));
    ByteWriter ref;
    ref.le(geometry);
    ref.le<std::uint32_t>(0);
    auto mesh_ref = b.add(0x2d, 0, ref);
    auto actor = [&](const std::string& code, bool host, bool geometry_present) {
        ByteWriter skeleton;
        skeleton.le<std::uint32_t>(0);
        skeleton.le<std::uint32_t>(host ? 6 : 4);
        skeleton.le<std::int32_t>(0);
        for (int i = 0; i < (host ? 6 : 4); ++i) {
            ByteWriter frame;
            frame.le<std::uint32_t>(0);
            frame.le<std::uint32_t>(i == 1 ? 2u : 1u);
            for (int key = 0; key < (i == 1 ? 2 : 1); ++key) {
                frame.f32(1.0f); // scale
                for (float v : {i == 0 ? 10.0f : 0.0f, static_cast<float>(i + 2 * key), 0.0f,
                                1.0f, 0.0f, 0.0f, 0.0f}) frame.f32(v);
            }
            auto track_def = b.add(0x12, 0, frame);
            ByteWriter track;
            track.le(track_def);
            track.le<std::uint32_t>(0);
            const std::string suffix = i == 4 ? "R_POINT_TRACK" : i == 5 ? "L_POINT_TRACK" : "B" + std::to_string(i) + "_TRACK";
            auto track_ref = b.add(0x13, b.str(code + suffix), track);
            skeleton.le<std::int32_t>(0);
            skeleton.le<std::uint32_t>(0);
            skeleton.le(track_ref);
            skeleton.le<std::int32_t>(i == 0 ? (geometry_present ? mesh_ref : 0) : i < 3 ? cloud : i == 3 ? bad_cloud : 0);
            skeleton.le<std::uint32_t>(i == 0 ? (host ? 5 : 3) : 0);
            if (i == 0)
                for (int child = 1; child < (host ? 6 : 4); ++child) skeleton.le<std::int32_t>(child);
        }
        auto sk = b.add(0x10, b.str(code + "_HS_DEF"), skeleton);
        ByteWriter sr;
        sr.le(sk);
        sr.le<std::uint32_t>(0);
        auto sk_ref = b.add(0x11, 0, sr);
        ByteWriter a;
        for (std::uint32_t value : {0u, 0u, 0u, 1u, 0u}) a.le(value);
        a.le(sk_ref);
        b.add(0x14, b.str(code + "_ACTORDEF"), a);
    };
    actor("TST", true, true);
    actor("IT1", false, true);
    actor("IT2", false, false); // a held effect may have no triangles at all
    auto archive = std::make_shared<CharacterArchive>();
    archive->archive = std::make_shared<PfsArchive>(PfsArchive::from_memory(
        test::pfs_fixtures::make_pfs({{"fixture.wld", b.build()}}), "fixture.s3d"));
    archive->wlds.push_back(wld::WldFile::parse(*archive->archive->read("fixture.wld")));
    CharacterLibrary lib({archive});
    auto model = lib.build("TST");
    REQUIRE(model.clouds.size() == 2);
    CHECK(model.clouds[0].bone == 1);
    CHECK(model.clouds[1].bone == 2);
    REQUIRE(model.clouds[0].cloud);
    CHECK(model.clouds[0].cloud == model.clouds[1].cloud);
    CHECK(model.clouds[0].cloud->texture == "GENG00.DDS");
    // Built again, the model hands out the same definition (app::ActorEmitters registers each one once).
    CHECK(lib.build("TST").clouds[0].cloud == model.clouds[0].cloud);
    CHECK(model.default_mesh().emitters.size() == 2);
    auto item = lib.item_mesh(1);
    REQUIRE(item);
    REQUIRE(item->mesh.emitters.size() == 2);
    CHECK(item->mesh.emitters[0].bone == 0);
    CHECK(near(glm::vec3(item->mesh.emitters[0].local[3]), {10.0f, 1.0f, 0.0f}));
    CHECK(near(glm::vec3(item->mesh.emitters[1].local[3]), {10.0f, 2.0f, 0.0f}));
    REQUIRE(item->mesh.emitters[0].cloud_animation);
    CHECK(item->mesh.emitters[0].cloud_animation_node == 1);
    CHECK_FALSE(item->mesh.emitters[1].cloud_animation);
    std::vector<glm::mat4> cloud_pose;
    item->mesh.emitters[0].cloud_animation->pose(100, cloud_pose);
    CHECK(near(glm::vec3((cloud_pose[1] * item->mesh.emitters[0].local)[3]), {10.0f, 3.0f, 0.0f}));
    auto effect_only = lib.item_mesh(2);
    REQUIRE(effect_only);
    CHECK(effect_only->mesh.indices.empty());
    CHECK(effect_only->mesh.emitters.size() == 2);
    game::Appearance appearance;
    appearance.material[game::kSlotPrimary] = 1;
    appearance.material[game::kSlotSecondary] = 2;
    auto dressed = lib.dress(model, appearance);
    REQUIRE(dressed);
    REQUIRE(dressed->emitters.size() == 6);
    CHECK(dressed->emitters[0].cloud == model.clouds[0].cloud);
    CHECK(dressed->emitters[2].bone == 4);
    CHECK(dressed->emitters[4].bone == 5);
    CHECK(dressed->emitters[2].cloud == item->mesh.emitters[0].cloud);
    CHECK(dressed->emitters[2].cloud_animation == item->mesh.emitters[0].cloud_animation);
    CHECK(near(glm::vec3(dressed->emitters[2].local[3]), {10.0f, 1.0f, 0.0f}));
    CHECK(lib.dress(model, appearance) == dressed);
    appearance.material[game::kSlotPrimary] = 0;
    appearance.material[game::kSlotSecondary] = 0;
    auto unarmed = lib.dress(model, appearance);
    REQUIRE(unarmed);
    CHECK(unarmed->emitters.size() == 2);
}

TEST_CASE("RoF2 warrior epic models retain their authored red and blue clouds" *
          doctest::skip(!hailstone::test::game_dir())) {
    auto archive = load_character_archive(*hailstone::test::game_dir(), "gequip.s3d");
    REQUIRE(archive);
    CharacterLibrary lib({archive});
    for (std::uint32_t number : {141u, 142u, 146u}) {
        CAPTURE(number);
        auto item = lib.item_mesh(number);
        REQUIRE(item);
        REQUIRE(item->mesh.emitters.size() == (number == 146 ? 2u : 1u));
        for (const auto& e : item->mesh.emitters) {
            REQUIRE(e.cloud);
            CHECK(e.bone == 0);
            CHECK(e.cloud->additive == 1);
            CHECK_FALSE(e.cloud->texture.empty());
            CHECK(e.cloud->texture_frames.size() == 5);
            CHECK(static_cast<bool>(e.cloud_animation) == (number == 146));
            CHECK(glm::length(glm::vec3(e.local[3])) > 0.0f);
        }
    }
}

TEST_CASE("equipment WLD cloud attachments and every separate texture frame resolve from the game folder" *
          doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    auto globals = CharacterLibrary::load(game, "");
    std::size_t items = 0, attachments = 0;
    for (const auto& name : global_character_archives(game)) {
        if (name.find("equip") == std::string::npos || !name.ends_with(".s3d")) continue;
        auto archive = load_character_archive(game, name);
        if (!archive) continue;
        CharacterLibrary lib({archive});
        wld::WldFile::ReadScope reads;
        for (const auto& w : archive->wlds)
            for (const auto& f : w.fragments()) {
                const auto* actor = f.get<wld::ActorDef>();
                if (!actor || !f.name.starts_with("IT") || !f.name.ends_with("_ACTORDEF")) continue;
                std::uint32_t number = 0;
                const auto parsed = std::from_chars(f.name.data() + 2, f.name.data() + f.name.size() - 9, number);
                if (parsed.ec != std::errc{} || parsed.ptr != f.name.data() + f.name.size() - 9) continue;
                std::size_t expected = 0;
                for (const auto ref : actor->sprites)
                    if (const auto* skeleton_ref = w.get<wld::SkeletonRef>(ref))
                        if (const auto* skeleton = w.get<wld::Skeleton>(skeleton_ref->skeleton))
                            for (const auto& node : skeleton->nodes)
                                if (w.get<wld::ParticleCloud>(node.sprite)) ++expected;
                if (expected == 0) continue;
                CAPTURE(name);
                CAPTURE(number);
                auto item = lib.item_mesh(number);
                REQUIRE(item);
                REQUIRE(item->mesh.emitters.size() == expected);
                ++items;
                attachments += expected;
                for (const auto& e : item->mesh.emitters) {
                    REQUIRE(e.cloud);
                    REQUIRE_FALSE(e.cloud->texture_frames.empty());
                    for (const auto& file : e.cloud->texture_frames) {
                        CAPTURE(file);
                        CHECK(globals.read_file(file).has_value());
                    }
                }
            }
    }
    CHECK(items > 0);
    CHECK(attachments >= items);
}

TEST_CASE("WLD 0x34 particle clouds and their 0x26 sprites parse") {
    WldBuilder b;
    auto blit = add_sprite(b);
    auto cloud = b.add(0x34, b.str("L307_PCD"), l307_body(blit));
    auto w = wld::WldFile::parse(b.build());
    REQUIRE(w.errors().empty());
    const auto& c = w.at(cloud)->as<wld::ParticleCloud>();
    CHECK(c.flags == 4u);
    CHECK(c.mode == 3u);
    CHECK(c.bits == 0x30500u);
    CHECK(c.capacity == 30u);
    CHECK(c.angle == 25.0f);
    CHECK(c.lifetime_ms == 750u);
    CHECK(c.speed == doctest::Approx(0.7f));
    CHECK(c.direction == glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(c.interval_ms == 30u);
    CHECK(c.size == 0.5f);
    CHECK(c.color == 0x3fffffffu);
    CHECK(c.sprite == blit);
    auto tex = particle_texture(w, c.sprite);
    REQUIRE(tex);
    CHECK(tex->file == "GENG00.DDS");
    CHECK(tex->frames == 1u);
    CHECK(tex->timing_ms == 100u);
    CHECK(tex->material == 0x80000017u);
    CHECK(tex->files == std::vector<std::string>{"GENG00.DDS"});
    CHECK_FALSE(particle_texture(w, cloud)); // not a 0x26
}

TEST_CASE("WLD cloud sprite frames retain separate files and authored repeats") {
    WldBuilder b;
    auto first = b.add(0x03, 0, bitmap_name("first.bmp"));
    auto second = b.add(0x03, 0, bitmap_name("second.bmp"));
    ByteWriter info;
    info.le<std::uint32_t>(8);
    info.le<std::uint32_t>(3);
    info.le<std::uint32_t>(40);
    info.le(first);
    info.le(second);
    info.le(first);
    auto sprite = b.add(0x04, 0, info);
    ByteWriter ref;
    ref.le(sprite);
    ref.le<std::uint32_t>(0);
    auto sprite_ref = b.add(0x05, 0, ref);
    ByteWriter blit;
    blit.le<std::uint32_t>(0);
    blit.le(sprite_ref);
    blit.le<std::uint32_t>(0x80000017);
    auto cloud_sprite = b.add(0x26, 0, blit);
    auto w = wld::WldFile::parse(b.build());
    auto texture = particle_texture(w, cloud_sprite);
    REQUIRE(texture);
    CHECK(texture->frames == 3);
    CHECK(texture->files == std::vector<std::string>{"first.bmp", "second.bmp", "first.bmp"});
    auto definition = particle_cloud_definition({}, *texture, "fixture");
    CHECK(definition.texture_frames == texture->files);
    CHECK(definition.frames == 3);
    CHECK(definition.frames_per_second == doctest::Approx(25));
    // A missing later bitmap does not silently shorten the sequence: the first bitmap is drawn alone, as before the
    // sequences were read, so the cloud does not vanish.
    info = {};
    info.le<std::uint32_t>(0);
    info.le<std::uint32_t>(2);
    info.le(first);
    info.le<std::int32_t>(9999);
    auto broken = b.add(0x04, b.str("broken"), info);
    ref = {};
    ref.le(broken);
    ref.le<std::uint32_t>(0);
    auto broken_ref = b.add(0x05, 0, ref);
    blit = {};
    blit.le<std::uint32_t>(0);
    blit.le(broken_ref);
    blit.le<std::uint32_t>(0x80000017);
    auto broken_sprite = b.add(0x26, 0, blit);
    w = wld::WldFile::parse(b.build());
    auto still = particle_texture(w, broken_sprite);
    REQUIRE(still);
    CHECK(still->frames == 1);
    CHECK(still->files == std::vector<std::string>{"first.bmp"});
    // Only a sprite whose first bitmap is missing has no texture.
    info = {};
    info.le<std::uint32_t>(0);
    info.le<std::uint32_t>(2);
    info.le<std::int32_t>(9999);
    info.le(first);
    auto headless = b.add(0x04, b.str("headless"), info);
    ref = {};
    ref.le(headless);
    ref.le<std::uint32_t>(0);
    auto headless_ref = b.add(0x05, 0, ref);
    blit = {};
    blit.le<std::uint32_t>(0);
    blit.le(headless_ref);
    blit.le<std::uint32_t>(0x80000017);
    auto headless_sprite = b.add(0x26, 0, blit);
    w = wld::WldFile::parse(b.build());
    CHECK_FALSE(particle_texture(w, headless_sprite));
}

TEST_CASE("particle_cloud_definition follows the DLL's 0x34 converter") {
    wld::ParticleCloud c;
    c.selector = 3;
    c.mode = 3;
    c.bits = 0x30500;
    c.capacity = 30;
    c.radius = 0.3f;
    c.angle = 25.0f;
    c.lifetime_ms = 750;
    c.speed = 0.7f;
    c.direction = {0.0f, 1.0f, 0.0f};
    c.interval_ms = 30;
    c.size = 0.5f;
    c.color = 0x3f204060; // B 0x60, G 0x40, R 0x20
    ParticleTexture tex{"GENG00.DDS", 1, 100, 0x80000017u};
    auto d = particle_cloud_definition(c, tex, "L307_PCD");
    CHECK(d.name == "L307_PCD");
    CHECK(d.texture == "GENG00.DDS");
    CHECK(d.unknown_13c == 3u);
    CHECK(d.unknown_140 == 0x30500u);
    CHECK(d.bone_orientation == 1u);
    CHECK(d.follow_attachment == 0u);
    CHECK(d.actor_scales_size == 1u); // bit 0x10000
    CHECK(d.capacity == 30u);
    // Mode 3: a point (no bit 0x80) spraying along +Y: +0xc8 = -128.
    CHECK(d.shape == 4u);
    CHECK(d.shape_size.x == 0.0f);
    CHECK(d.angle1 == 0.0f);
    CHECK(d.angle2 == -128.0f);
    CHECK(d.motion[0].x == doctest::Approx(0.7f));
    CHECK(d.motion[0].y == doctest::Approx(0.7f));
    CHECK(d.motion[3].x == 0.0f);
    CHECK(d.motion[3].y == doctest::Approx(0.7f * std::tan(25.0f * 2.0f * std::numbers::pi_v<float> / 512.0f)));
    CHECK(d.duration == -9999.0f);
    CHECK(d.lifetime == doctest::Approx(0.75f));
    CHECK(d.fade_out == doctest::Approx(0.75f)); // bit 0x100
    CHECK(d.max_opacity == 1.0f);
    CHECK(d.scale2 == 1.0f);
    CHECK(d.initial_count == 1u);
    CHECK(d.per_tick == 1u);
    CHECK(d.ticks_per_second == doctest::Approx(1000.0f / 30.0f));
    CHECK(d.scale1 == 0.5f);
    CHECK(d.width == 1.0f); // +0xcc keeps SetDefaults' value
    CHECK(d.color_start == glm::uvec3(0x20, 0x40, 0x60));
    CHECK(d.color_end == d.color_start);
    CHECK(d.frames == 1u);
    CHECK(d.frames_per_second == doctest::Approx(10.0f));
    CHECK(d.additive == 1u); // 0x80000017 -> 0x011b0507: bit 24 set, bit 20 set
    CHECK(d.no_zwrite == 1u);

    SUBCASE("blood sprite's installed material alias uses alpha blending") {
        tex.material = 0x80000018u;
        CHECK(particle_material_alias(tex.material) == 0x01080507u);
        CHECK(particle_cloud_definition(c, tex, "BloodFixture").additive == 0u);
    }

    SUBCASE("the installed masked sprite alias is alpha blended and unknown aliases stay unresolved") {
        tex.material = 0x80000019u;
        CHECK(particle_material_alias(tex.material) == 0x00000587u);
        CHECK(particle_cloud_definition(c, tex, "MaskedFixture").additive == 0u);
        CHECK_FALSE(particle_material_alias(0x80000031u));
    }

    SUBCASE("without bit 0x100: no fade, half opacity") {
        c.bits = 0x30400;
        auto e = particle_cloud_definition(c, tex, "X");
        CHECK(e.fade_out == 0.0f);
        CHECK(e.max_opacity == 0.5f);
        CHECK(e.scale2 == 0.5f);
        CHECK(e.actor_scales_size == 1u);
    }
    SUBCASE("follow bits and the 0.1 disk") {
        c.bits = 0x4080;
        auto e = particle_cloud_definition(c, tex, "X");
        CHECK(e.follow_attachment == 1u);
        CHECK(e.actor_scales_size == 1u); // follows, so it scales
        CHECK(e.shape_size.x == doctest::Approx(0.1f));
    }
    SUBCASE("mode 3 directions: exact cardinal compares in the converter's order") {
        auto angles = [&](glm::vec3 dir) {
            c.direction = dir;
            auto e = particle_cloud_definition(c, tex, "X");
            return glm::vec2(e.angle1, e.angle2);
        };
        CHECK(angles({0, 0, 1}) == glm::vec2(0, 0));
        CHECK(angles({1, 0, 0}) == glm::vec2(128, 0));
        CHECK(angles({0, 0, -1}) == glm::vec2(256, 0));
        CHECK(angles({-1, 0, 0}) == glm::vec2(-128, 0));
        CHECK(angles({0, -1, 0}) == glm::vec2(0, 128));
        CHECK(angles({0.5f, 0.5f, 0}) == glm::vec2(0, 0));
    }
    SUBCASE("modes 0, 1, 2, 4, 5") {
        c.direction = {0.0f, 0.0f, 1.0f};
        c.speed = 2.0f;
        c.mode = 4;
        auto e = particle_cloud_definition(c, tex, "X");
        CHECK(e.shape == 4u);
        CHECK(e.shape_size.x == doctest::Approx(0.3f));
        CHECK(e.motion[0].x == 2.0f);
        CHECK(e.motion[1].x == 0.0f);
        c.mode = 1;
        e = particle_cloud_definition(c, tex, "X");
        CHECK(e.shape == 5u);
        CHECK(e.motion[3].x == doctest::Approx(0.5f));
        CHECK(e.motion[3].y == 2.0f);
        c.mode = 2;
        CHECK(particle_cloud_definition(c, tex, "X").shape == 9u);
        c.mode = 5;
        CHECK(particle_cloud_definition(c, tex, "X").shape == 9u);
        c.mode = 0;
        c.ranges = {glm::vec3(-1, -2, 0), glm::vec3(3, 2, 4)};
        e = particle_cloud_definition(c, tex, "X");
        CHECK(e.shape == 6u);
        CHECK(e.shape_size == glm::vec3(2, 2, 2));
    }
    SUBCASE("a TFIRE2 name gets the fixed small fire") {
        auto e = particle_cloud_definition(c, tex, "tfire2_pcd");
        CHECK(e.lifetime == doctest::Approx(0.08f));
        CHECK(e.capacity == 8u);
        CHECK(e.shape_size.x == doctest::Approx(0.05f));
        CHECK(e.ticks_per_second == doctest::Approx(33.333332f));
    }
    SUBCASE("another material: an unknown alias keeps INVSRCALPHA, a direct word decides") {
        tex.material = 0x80000005u;
        CHECK(particle_cloud_definition(c, tex, "X").additive == 0u);
        tex.material = 0x010b0507u; // bit 20 clear
        CHECK(particle_cloud_definition(c, tex, "X").additive == 0u);
        tex.material = 0x011b0507u;
        CHECK(particle_cloud_definition(c, tex, "X").additive == 1u);
    }
}

TEST_CASE("particle_tan: 512 per turn, truncated and masked to a quarter turn") {
    CHECK(particle_tan(0.0f) == 0.0f);
    CHECK(particle_tan(64.0f) == doctest::Approx(1.0f));
    // A forged angle past int32 (or NaN) is not cast: index 0, as _ftol's integer indefinite gives.
    CHECK(particle_tan(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    CHECK(particle_tan(-1e30f) == 0.0f);
    CHECK(particle_tan(std::numeric_limits<float>::infinity()) == 0.0f);
    CHECK(particle_tan(25.9f) == particle_tan(25.0f));
    CHECK(particle_tan(128.0f) == 0.0f);
    CHECK(particle_tan(15.0f) == doctest::Approx(std::tan(15.0 * 2.0 * std::numbers::pi / 512.0)));
}

TEST_CASE("skeleton_particles finds the particle node and its rest matrix") {
    WldBuilder b;
    auto blit = add_sprite(b);
    auto cloud = b.add(0x34, b.str("L307_PCD"), l307_body(blit));
    auto track = [&](std::int16_t w, std::int16_t x, std::int16_t tz) {
        ByteWriter def;
        def.le(std::uint32_t{0x8});
        def.le(std::uint32_t{1});
        for (std::int16_t v : {w, x, std::int16_t{0}, std::int16_t{0}, std::int16_t{0}, std::int16_t{0}, tz}) def.le(v);
        def.le(std::uint16_t{256});
        auto d = b.add(0x12, 0, def);
        ByteWriter ref;
        ref.le(d);
        ref.le(std::uint32_t{0});
        return b.add(0x13, 0, ref);
    };
    auto root = track(16384, 0, 8 * 256);
    auto flame = track(11585, 11585, 256); // a quarter turn about X (as the lanterns' flame nodes), z + 1
    ByteWriter sk;
    sk.le(std::uint32_t{0});
    sk.le(std::uint32_t{2});
    sk.le(std::int32_t{0});
    sk.le(b.str("LAMP_DAG"));
    sk.le(std::uint32_t{0});
    sk.le(root);
    sk.le(std::int32_t{0});
    sk.le(std::uint32_t{1});
    sk.le(std::int32_t{1});
    sk.le(b.str("LAMPP1F_DAG"));
    sk.le(std::uint32_t{0});
    sk.le(flame);
    sk.le(cloud);
    sk.le(std::uint32_t{0});
    auto skeleton = b.add(0x10, b.str("LAMP_HS_DEF"), sk);
    auto w = wld::WldFile::parse(b.build());
    REQUIRE(w.errors().empty());
    const auto& def = w.at(skeleton)->as<wld::Skeleton>();
    auto found = wld::skeleton_particles(w, def);
    REQUIRE(found.size() == 1);
    CHECK(found[0].cloud == w.at(cloud));
    CHECK(near(glm::vec3(found[0].node[3]), {0, 0, 9}));
    // The node's +Y turned onto +Z: the cloud's (0, 1, 0) direction sprays up.
    CHECK(near(glm::vec3(found[0].node[1]), {0, 0, 1}));
    auto d = particle_cloud_definition(w.at(cloud)->as<wld::ParticleCloud>(), *particle_texture(w, blit),
                                       "L307_PCD");
    glm::mat3 axes = game::emitter_axes(glm::mat3(found[0].node), d.angle1, d.angle2);
    CHECK(near(axes[2], {0, 0, 1}));
    // A skeleton without particle nodes has none.
    auto plain = def;
    plain.nodes[1].sprite = 0;
    CHECK(wld::skeleton_particles(w, plain).empty());
}

TEST_CASE("emitter_axes: the owner basis and the converter's turns") {
    // No owner rotation, no turn: the spell emitters' (+X, -Y, +Z).
    glm::mat3 id = game::emitter_axes(glm::mat3(1.0f), 0.0f, 0.0f);
    CHECK(near(id[0], {1, 0, 0}));
    CHECK(near(id[1], {0, -1, 0}));
    CHECK(near(id[2], {0, 0, 1}));
    // OpenEQ's placement witnesses (native world rows = these columns), +0xc8
    // = -128: final axes (axial, radial cosine, radial sine) = rows 1, 0, 2.
    glm::mat3 torch(glm::vec3(1, 0, 0), glm::vec3(0, 0, 1), glm::vec3(0, -1, 0));
    auto t = game::emitter_axes(torch, 0.0f, -128.0f);
    CHECK(near(t[2], {0, 0, 1}));
    CHECK(near(t[0], {1, 0, 0}));
    CHECK(near(t[1], {0, -1, 0}));
    glm::mat3 lamp(glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0));
    auto l = game::emitter_axes(lamp, 0.0f, -128.0f);
    CHECK(near(l[2], {0, 0, 1}));
    CHECK(near(l[0], {-1, 0, 0}));
    CHECK(near(l[1], {0, 1, 0}));
    // +0xc4 picks +X, -Z and -X for the axis; +0xc8 = 128 picks -Y.
    CHECK(near(game::emitter_axes(glm::mat3(1.0f), 128.0f, 0.0f)[2], {1, 0, 0}));
    CHECK(near(game::emitter_axes(glm::mat3(1.0f), 256.0f, 0.0f)[2], {0, 0, -1}));
    CHECK(near(game::emitter_axes(glm::mat3(1.0f), -128.0f, 0.0f)[2], {-1, 0, 0}));
    CHECK(near(game::emitter_axes(glm::mat3(1.0f), 0.0f, 128.0f)[2], {0, -1, 0}));
    // A scaled owner is normalised.
    CHECK(near(game::emitter_axes(glm::mat3(2.5f), 0.0f, 0.0f)[2], {0, 0, 1}));
}

TEST_CASE("emission_distance_factor: +0xa0 / distance past +0xa0, floored at the batch floor") {
    CHECK(game::emission_distance_factor(80.0f, 0.4f, 10.0f) == 1.0f);
    CHECK(game::emission_distance_factor(80.0f, 0.4f, 160.0f) == doctest::Approx(0.5f));
    CHECK(game::emission_distance_factor(80.0f, 0.4f, 1000.0f) == doctest::Approx(0.4f));
    CHECK(game::emission_distance_factor(80.0f, 1.0f, 1000.0f) == 1.0f);
    CHECK(game::emission_distance_factor(80.0f, 0.4f, 0.0f) == 1.0f);
}

TEST_CASE("a zone fixture emitter: persistent, along its axis, spreading all round") {
    wld::ParticleCloud c;
    c.mode = 3;
    c.bits = 0x30500;
    c.capacity = 40;
    c.angle = 60.0f;
    c.lifetime_ms = 650;
    c.speed = 3.0f;
    c.direction = {0.0f, 1.0f, 0.0f};
    c.interval_ms = 60;
    c.size = 1.0f;
    c.color = 0xffffff;
    std::vector<EmitterDefinition> defs(1);
    defs.push_back(particle_cloud_definition(c, {"GENG00.DDS", 1, 100, 0x80000017u}, "L301_PCD"));
    game::ParticleSystem ps(defs, 7);
    CHECK_FALSE(ps.add_fixture(0, glm::mat4(1.0f)));
    CHECK_FALSE(ps.add_fixture(2, glm::mat4(1.0f)));
    // A node whose +Y points up, at (10, 20, 30), scaled 1.5 by its placement.
    glm::mat4 node(1.0f);
    node[0] = glm::vec4(1.5f, 0, 0, 0);
    node[1] = glm::vec4(0, 0, 1.5f, 0);
    node[2] = glm::vec4(0, -1.5f, 0, 0);
    node[3] = glm::vec4(10, 20, 30, 1);
    REQUIRE(ps.add_fixture(1, node));
    REQUIRE(ps.emitters().size() == 1);
    const auto& e = ps.emitters()[0];
    CHECK(e.persistent);
    CHECK(e.scale == doctest::Approx(1.5f));
    CHECK(e.counts.capacity == 40);
    bool left = false, right = false;
    for (int i = 0; i < 100; ++i) {
        ps.update(0.05f, {});
        for (const auto& p : ps.emitters()[0].particles) {
            CHECK(p.position.z >= 30.0f - 1e-4f); // rising from the node
            if (p.position.x < 10.0f - 0.01f) left = true;
            if (p.position.x > 10.0f + 0.01f) right = true;
        }
    }
    CHECK((left && right)); // the radial spray goes all round, not one way
    CHECK(ps.emitters()[0].particles.size() <= 40u);
    // Far past the 9999 s an EDD emitter would last, it still emits.
    for (int i = 0; i < 12; ++i) ps.update(1000.0f, {});
    ps.update(0.5f, {});
    REQUIRE(ps.emitters().size() == 1);
    CHECK_FALSE(ps.emitters()[0].particles.empty());
    // Far from the viewer it emits at the floor rate, still something.
    ps.set_viewer(glm::vec3(10000.0f, 0.0f, 0.0f));
    for (int i = 0; i < 20; ++i) ps.update(0.05f, {});
    CHECK_FALSE(ps.emitters()[0].particles.empty());
}

TEST_CASE("bazaar's lanterns and torches carry their flames" * doctest::skip(!hailstone::test::game_dir())) {
    if (!hailstone::test::game_file("bazaar.s3d")) return; // a live install has the EQG bazaar only
    auto z = load_classic_zone(*hailstone::test::game_dir(), "bazaar");
    // L301 and CSMOKE (FTORCH301) and L307 (HAVSTLIGHT300..302), after "None".
    REQUIRE(z.particle_definitions.size() == 4);
    // 16 torches x (flame + smoke) + 13 + 10 + 14 x 2 lantern flames.
    CHECK(z.particle_fixtures.size() == 83);
    std::size_t up = 0;
    for (const auto& f : z.particle_fixtures) {
        const auto& d = z.particle_definitions[f.definition];
        auto axes = game::emitter_axes(glm::mat3(f.transform), d.angle1, d.angle2);
        if (axes[2].z > 0.99f) ++up;
    }
    CHECK(up == z.particle_fixtures.size());
}
