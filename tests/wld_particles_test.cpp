#include <doctest/doctest.h>

#include <limits>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

#include "assets/wld.h"
#include "assets/wld_particles.h"
#include "assets/zone.h"
#include "core/byte_io.h"
#include "game/particles.h"
#include "game_files.h"
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
    CHECK_FALSE(particle_texture(w, cloud)); // not a 0x26
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
