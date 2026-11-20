#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

#include <cstring>
#include <limits>
#include <numbers>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "app/zone_particles.h"
#include "assets/model_points.h"
#include "assets/pfs.h"
#include "assets/spell_effects.h"
#include "core/byte_io.h"
#include "core/game_dir.h"
#include "core/worker_pool.h"
#include "game/particles.h"
#include "game/spawn_heights.h"
#include "render/particle_renderer.h"
#include "game_files.h"

using namespace hailstone;

namespace {

std::vector<std::byte> edd_record(const char* name, const char* texture, float duration, std::uint32_t initial) {
    ByteWriter w;
    w.fixed_string(name, 64);
    w.fixed_string(texture, 32);
    for (int i = 0; i < 6; ++i) w.le<std::uint32_t>(i == 2 ? 1u : 0u); // additive at 0x68
    w.f32(duration);  // 0x78
    w.f32(-1.0f);     // 0x7c lifetime
    w.le<std::uint32_t>(initial); // 0x80
    w.le<std::uint32_t>(3);       // 0x84 per tick
    w.f32(10.0f);                 // 0x88 ticks per second
    while (w.data().size() < 0xa8) w.f32(0.0f);
    w.le<std::uint32_t>(5); // 0xa8 shape
    while (w.data().size() < 0xd4) w.f32(0.0f);
    for (std::uint32_t c : {255u, 128u, 0u, 0u, 0u, 255u}) w.le<std::uint32_t>(c); // colours
    while (w.data().size() < 0x130) w.f32(0.0f);
    w.le<std::uint32_t>(16); // frames
    while (w.data().size() < 0x144) w.f32(0.0f);
    w.le<std::uint32_t>(0); // capacity
    while (w.data().size() < 0x1a0) w.le<std::uint8_t>(0);
    return w.take();
}

} // namespace

TEST_CASE("EDD 110: header, 0x1a0-byte records, fields at their offsets") {
    ByteWriter w;
    w.fixed_string("EDD", 4);
    w.fixed_string("110", 4);
    auto none = edd_record("None", "", 0.0f, 1);
    auto fire = edd_record("Fire", "flame.dds", -5.0f, 12);
    std::vector<std::byte> data = w.take();
    data.insert(data.end(), none.begin(), none.end());
    data.insert(data.end(), fire.begin(), fire.end());
    auto defs = assets::parse_edd(data);
    REQUIRE(defs.size() == 2);
    CHECK(defs[0].name == "None");
    CHECK(defs[1].name == "Fire");
    CHECK(defs[1].texture == "flame.dds");
    CHECK(defs[1].additive == 1u);
    CHECK(defs[1].duration == -5.0f);
    CHECK(defs[1].initial_count == 12u);
    CHECK(defs[1].per_tick == 3u);
    CHECK(defs[1].ticks_per_second == 10.0f);
    CHECK(defs[1].shape == 5u);
    CHECK(defs[1].color_start == glm::uvec3(255, 128, 0));
    CHECK(defs[1].color_end == glm::uvec3(0, 0, 255));
    CHECK(defs[1].frames == 16u);
    CHECK(defs[1].texture_frames.empty()); // EDD files retain their atlas flipbooks
    // Other versions, the legacy headerless format and bad sizes are refused.
    auto bad = data;
    bad[6] = std::byte{'1'};
    CHECK_THROWS(assets::parse_edd(bad));
    CHECK_THROWS(assets::parse_edd(std::vector<std::byte>(0xc4 * 2)));
    data.pop_back();
    CHECK_THROWS(assets::parse_edd(data));
}

TEST_CASE("CreateSpellEmitter: duration, density and capacity rules") {
    // EDD +0x78 < 0 forces |value|; else a requested duration wins; else +0x78.
    CHECK(game::emitter_duration_seconds(-5.0f, 3000) == 5.0f);
    CHECK(game::emitter_duration_seconds(2.0f, 3000) == doctest::Approx(3.0f));
    CHECK(game::emitter_duration_seconds(2.0f, 0) == 2.0f);
    CHECK(game::emitter_duration_seconds(2.0f, -1) == 2.0f);
    assets::EmitterDefinition d;
    d.initial_count = 10;
    d.per_tick = 2;
    d.ticks_per_second = 5.0f;
    d.lifetime = 1.0f;
    // CEmitterDef::CreateEmitter: emitting longer than a lifetime holds initial + trunc(life x rate) x per tick.
    CHECK(game::emitter_capacity(d, 4.0f) == 10u + 5u * 2u);
    // Density: the initial count keeps at least min(16, n); fewer than 16 per tick tick less often instead.
    auto half = game::emitter_counts(d, 0.5f, 4.0f);
    CHECK(half.initial == 10);
    CHECK(half.per_tick == 2);
    CHECK(half.rate == doctest::Approx(2.5f));
    CHECK(half.capacity == 10 + 2 * 2);
    CHECK(half.length_ms == 5000); // trunc(4 x 2.5) / 2.5 + delay + lifetime
    d.per_tick = 40;
    auto many = game::emitter_counts(d, 0.5f, 4.0f);
    CHECK(many.per_tick == 20); // 16 or more per tick: trunc(n x density), the rate kept
    CHECK(many.rate == doctest::Approx(5.0f));
    d.initial_count = 100;
    CHECK(game::emitter_counts(d, 0.1f, 4.0f).initial == 16);
    d.initial_count = 10;
    d.per_tick = 2;
    d.capacity = 7; // +0x144 replaces the computed count
    CHECK(game::emitter_capacity(d, 4.0f) == 7u);
    d.capacity = 200000;
    CHECK(game::emitter_capacity(d, 4.0f) == 100000u);
    d.capacity = 0;
    d.delay = 5.0f; // its delay outlasts it: no ring
    CHECK(game::emitter_capacity(d, 4.0f) == 0u);
    CHECK(game::flipbook_grid(1) == glm::uvec2(1, 1));
    CHECK(game::flipbook_grid(3) == glm::uvec2(2, 2));
    CHECK(game::flipbook_grid(6) == glm::uvec2(4, 2));
    CHECK(game::flipbook_grid(16) == glm::uvec2(4, 4));
    CHECK(game::flipbook_grid(17) == glm::uvec2(1, 1));

    std::vector<assets::EmitterDefinition> defs(2);
    defs[1].duration = 1.5f;
    defs[1].initial_count = 4;
    game::ParticleSystem ps(defs);
    game::SpellEmitterRequest r;
    r.emitter = 1;
    r.density = 0.0f; // density <= 0 creates nothing
    CHECK(ps.create(r) == 0);
    r.density = 0.5f;
    r.duration_ms = 0;
    CHECK(ps.create(r) == 1500); // lifetime < 0: the duration
    r.emitter = 2; // out of range
    CHECK(ps.create(r) == 0);
    REQUIRE(ps.emitters().size() == 1);
    ps.update(0.01f, {});
    CHECK(ps.emitters()[0].particles.size() == 4); // the initial count: at least min(16, n) at any density
    ps.update(2.0f, {});
    REQUIRE(ps.emitters().size() == 1); // its particles (living the emitter's 1.5 s) leave the ring's head next frame
    ps.update(0.01f, {});
    CHECK(ps.emitters().empty()); // duration and particles over
}

TEST_CASE("CreateEmitter's counts for Burst of Flame's cast flame (spellsnew.edd 1468)") {
    assets::EmitterDefinition d; // flame: initial 10, 10 per tick at 25/s, lifetime 1.25 s
    d.initial_count = 10;
    d.per_tick = 10;
    d.ticks_per_second = 25.0f;
    d.lifetime = 1.25f;
    auto none = game::emitter_counts(d, 1.0f, 0.0f);
    CHECK(none.capacity == 10);
    CHECK(none.length_ms == 1250);
    auto three = game::emitter_counts(d, 1.0f, 3.0f);
    CHECK(three.capacity == 320); // (trunc(1.25 x 25) + 1) x 10
    CHECK(three.length_ms == 4250);
    auto half = game::emitter_counts(d, 0.5f, 3.0f);
    CHECK(half.initial == 10);
    CHECK(half.per_tick == 10);
    CHECK(half.rate == doctest::Approx(12.5f));
}

TEST_CASE("A box-surface particle draws its two free axes, then its face, as the graphics DLL does") {
    // EQGraphicsDX9.dll's shape 6 (@0x100737be): r1, r2 in -1..1, then the face u * 6.0 rounded to an integer with u in
    // 0..1; faces 0..4 are -x, +x, -y, +y, -z, anything else (5, and 6 from the rounding) +z.
    for (std::uint32_t seed : {7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u}) {
        std::vector<assets::EmitterDefinition> defs(2);
        defs[1].duration = 1.0f;
        defs[1].initial_count = 1;
        defs[1].lifetime = 1.0f;
        defs[1].shape = 6;
        defs[1].shape_size = {2.0f, 3.0f, 4.0f};
        game::ParticleSystem ps(defs, seed);
        game::SpellEmitterRequest r;
        r.emitter = 1;
        r.density = 1.0f;
        REQUIRE(ps.create(r) > 0);
        ps.update(0.0f, {});
        REQUIRE(ps.emitters().size() == 1);
        REQUIRE(ps.emitters()[0].particles.size() == 1);
        // The same draws by hand: the azimuth (drawn for every shape), r1, r2, the face.
        std::mt19937 rng(seed);
        auto draw = [&](float lo, float hi) { return lo + (hi - lo) * std::uniform_real_distribution<float>(0.0f, 1.0f)(rng); };
        draw(0.0f, 512.0f);
        const float r1 = draw(-1.0f, 1.0f);
        const float r2 = draw(-1.0f, 1.0f);
        const int face = static_cast<int>(std::nearbyint(draw(0.0f, 1.0f) * 6.0f));
        glm::vec3 v = face == 0   ? glm::vec3(-1.0f, r1, r2)
                      : face == 1 ? glm::vec3(1.0f, r1, r2)
                      : face == 2 ? glm::vec3(r1, -1.0f, r2)
                      : face == 3 ? glm::vec3(r1, 1.0f, r2)
                      : face == 4 ? glm::vec3(r1, r2, -1.0f)
                                  : glm::vec3(r1, r2, 1.0f);
        const glm::vec3 want = v * glm::vec3(2.0f, 3.0f, 4.0f);
        const auto& got = ps.emitters()[0].particles[0];
        INFO("seed " << seed << " face " << face);
        // The polar record: the radius in the transverse plane, the axial coordinate, and the radial motion's
        // direction along the surface's normal (shapes 5 and 6).
        CHECK(got.radius == doctest::Approx(std::sqrt(want.x * want.x + want.y * want.y)));
        CHECK(got.axial == doctest::Approx(want.z));
        CHECK(got.radial_dir.y == doctest::Approx(want.z / glm::length(want)));
    }
}

TEST_CASE("SpellEffect_SpawnStageEmitters: slots, levels, locations and settings") {
    assets::SpellEffect fx;
    auto& st = fx.stages[0];
    st.slots[0] = {10, 0, 1, 0};  // at the caster
    st.slots[1] = {11, 20, 1, 0}; // needs level 20
    st.slots[2] = {12, 0, 3, 4};  // at the target, bone point 3
    st.slots[3] = {0, 0, 1, 0};   // empty
    game::StageActor caster{7, {1, 2, 3}, 10, 6.0f}, target{9, {4, 5, 6}, 1, 5.0f};
    target.points = 0x3ff; // all ten spell points
    auto s = game::spell_particle_settings(0.5f, 2.0f); // opacity clamped to 1
    CHECK(s.opacity == 1.0f);
    auto out = game::spawn_stage_emitters(fx, 0, 1234, &caster, &target, {0, 0, 0}, 3000, s);
    REQUIRE(out.size() == 2);
    CHECK(out[0].emitter == 10);
    CHECK(out[0].key == ((7u << 16) | 1234u));
    CHECK(out[0].duration_ms == 3000);
    CHECK(out[0].density == 0.5f);
    CHECK(out[0].position == glm::vec3(1, 2, 3));
    CHECK(out[0].actor == 0u);
    CHECK(out[1].emitter == 12);
    CHECK(out[1].position == glm::vec3(4, 5, 6));
    CHECK(out[1].actor == 9u);
    CHECK(out[1].bone_point == 3);
    // The flag forces density and opacity to 1.0.
    auto full = game::spawn_stage_emitters(fx, 0, 1234, &caster, &target, {0, 0, 0}, 0, s, true);
    CHECK(full[0].density == 1.0f);
}

TEST_CASE("Spell particle sources: self, other players and NPCs") {
    using game::SpellFxSource;
    const std::uint32_t me = 5;
    // The player's own cast, and any spell landing on the player, whoever cast it.
    CHECK(game::spell_fx_source(5, 5, me, false, false) == SpellFxSource::Self);
    CHECK(game::spell_fx_source(5, 40, me, false, true) == SpellFxSource::Self);
    CHECK(game::spell_fx_source(40, 5, me, true, false) == SpellFxSource::Self);
    CHECK(game::spell_fx_source(41, 5, me, false, false) == SpellFxSource::Self);
    // Another player's spell on an NPC is the players' (the caster's kind, not the struck spawn's).
    CHECK(game::spell_fx_source(41, 40, me, false, true) == SpellFxSource::Players);
    CHECK(game::spell_fx_source(40, 41, me, true, false) == SpellFxSource::Npcs);
    CHECK(game::spell_fx_source(40, 40, me, true, true) == SpellFxSource::Npcs);
    // An unknown caster: the struck spawn's kind, else the players'.
    CHECK(game::spell_fx_source(99, 40, me, std::nullopt, true) == SpellFxSource::Npcs);
    CHECK(game::spell_fx_source(99, 98, me, std::nullopt, std::nullopt) == SpellFxSource::Players);
    // Before the player's id is known, nothing is the player's.
    CHECK(game::spell_fx_source(0, 0, 0, std::nullopt, std::nullopt) == SpellFxSource::Players);
}

TEST_CASE("SpellEffect_SpawnStageEmitters: the attach ids and their spell point fallbacks") {
    auto run = [](std::int32_t attach, std::uint16_t points, std::optional<float> floor = std::nullopt) {
        assets::SpellEffect fx;
        fx.stages[0].slots[0] = {10, 0, 1, attach};
        game::StageActor caster{7, {1, 2, 3}, 10, 6.0f};
        caster.points = points;
        caster.chest = {8, 9, 10};
        caster.floor = floor;
        caster.particle_scale = 0.6f;
        return game::spawn_stage_emitters(fx, 0, 1, &caster, nullptr, {}, 0, {});
    };
    // A WLD human: default, chest, head, hands, feet and mouth; no eyes.
    const std::uint16_t human = 0x27f;
    CHECK(run(1, human)[0].bone_point == 0);
    CHECK(run(4, human)[0].bone_point == 3); // LHAND
    CHECK(run(5, human)[0].bone_point == 4); // RHAND
    CHECK(run(4, 0x1)[0].bone_point == 0);   // no hands: the default point
    CHECK(run(4, 0)[0].bone_point == -1);    // nothing: the actor alone
    CHECK(run(4, 0)[0].actor == 7u);
    CHECK(run(8, human)[0].bone_point == -1);
    CHECK(run(8, human)[0].actor == 7u);
    CHECK(run(9, human).empty()); // no eye: the slot is skipped
    CHECK(run(10, 0x3ff)[0].bone_point == 8);
    CHECK(run(11, human)[0].bone_point == 9);
    auto feet = run(12, human);
    REQUIRE(feet.size() == 1);
    CHECK(feet[0].bone_point == -1);
    CHECK(feet[0].position == glm::vec3(8, 9, 3)); // the chest's x and y
    CHECK(feet[0].scale == 0.6f);                  // the actor's particle scale (moddat.ini PESFactor)
    CHECK(run(12, human, 0.5f)[0].position == glm::vec3(8, 9, 0.5f)); // on the floor below
    CHECK(run(13, human).empty());
    CHECK(run(0, human)[0].actor == 0u);
}

TEST_CASE("Model points: the WLD table and the point matrix") {
    std::vector<std::string> bones = {"HUM_TRACK", "HUMCH_TRACK", "HUMHEAD_POINT_TRACK", "HUML_POINT_TRACK",
                                      "HUMR_POINT_TRACK", "HUMBO_L_TRACK", "HUMBO_R_TRACK"};
    auto pts = assets::wld_model_points(bones, [](std::string_view section, std::string_view key) -> std::optional<float> {
        if (section == "HUM" && key == "ZOffset") return -0.5f;
        return std::nullopt;
    });
    auto bone = [&](std::string_view n) {
        const auto* p = assets::find_point(pts, n);
        return p ? p->bone : std::string("-");
    };
    CHECK(bone("SPELLPOINT_DEFAULT") == "HUM_TRACK");
    CHECK(bone("SPELLPOINT_CHEST") == "HUMCH_TRACK");
    CHECK(bone("SPELLPOINT_HEAD") == "HUMHEAD_POINT_TRACK");
    CHECK(bone("SPELLPOINT_LHAND") == "HUML_POINT_TRACK");
    CHECK(bone("SPELLPOINT_RFOOT") == "HUMBO_R_TRACK");
    CHECK(bone("SPELLPOINT_LEYE") == "-");
    const auto* mouth = assets::find_point(pts, "SPELLPOINT_MOUTH");
    REQUIRE(mouth);
    CHECK(mouth->offset == glm::vec3(0.6f, 0.0f, -0.5f)); // XOffset's default, ZOffset from [HUM]
    // A Luclin skeleton: the mouth on HEHEAD at its fixed place.
    std::vector<std::string> luclin = {"HUM_TRACK", "HUMHEHEAD_TRACK", "HUMSPELL_POINT_TRACK"};
    auto lp = assets::wld_model_points(luclin, {});
    REQUIRE(assets::find_point(lp, "SPELLPOINT_MOUTH"));
    CHECK(assets::find_point(lp, "SPELLPOINT_MOUTH")->offset == glm::vec3(0.08f, 0.6f, 0.0f));
    CHECK(assets::find_point(lp, "SPELLPOINT_LHAND")->bone == "HUMSPELL_POINT_TRACK");
    CHECK(assets::find_point(lp, "SPELLPOINT_RHAND")->bone == "HUMSPELL_POINT_TRACK");
    // 0x1006f370: heading 128 turns +X onto +Y; the offset is the translation; the scale applies first.
    assets::ModelPoint p;
    p.offset = {1, 2, 3};
    p.rotation = {128, 0, 0};
    p.scale = {2, 2, 2};
    glm::mat4 m = assets::point_matrix(p);
    glm::vec3 x = glm::vec3(m * glm::vec4(1, 0, 0, 1));
    CHECK(x.x == doctest::Approx(1.0f));
    CHECK(x.y == doctest::Approx(4.0f));
    CHECK(x.z == doctest::Approx(3.0f));
    // Pitch: the DLL negates it, so pitch 128 turns +X onto +Z.
    p = {};
    p.rotation = {0, 128, 0};
    glm::vec3 xp = glm::vec3(assets::point_matrix(p) * glm::vec4(1, 0, 0, 0));
    CHECK(xp.z == doctest::Approx(1.0f));
}

TEST_CASE("NMS model points: dkm.pts" * doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    auto archive = game.find("dkm.eqg");
    REQUIRE(archive);
    auto pfs = assets::PfsArchive::open(*archive);
    auto data = pfs.read("dkm.pts");
    REQUIRE(data);
    auto pts = assets::parse_pts(*data);
    REQUIRE(pts.size() == 11);
    CHECK(pts[0].name == "SPELLPOINT_DEFAULT");
    CHECK(pts[0].bone == "ROOT_BONE");
    CHECK(pts[3].name == "SPELLPOINT_LHAND");
    CHECK(pts[3].bone == "ARML_FARM");
    CHECK(pts[3].offset.x == doctest::Approx(1.3f));
    CHECK(pts[3].rotation == glm::vec3(128, 256, -128));
    CHECK(pts[10].name == "DAMAGE_POINT");
}

TEST_CASE("NMS spell particle files parse" * doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    auto edd = game.read("spellsnew.edd");
    REQUIRE(edd);
    auto defs = assets::parse_edd(*edd);
    CHECK(defs.size() == 2577); // record 0 is the file's own "None" sentinel
    CHECK(defs[0].name == "None");
    auto eff = game.read("spellsnew.eff");
    REQUIRE(eff);
    auto fx = assets::parse_spell_effects(*eff);
    CHECK(fx.size() > 600);
    std::size_t refs = 0, bad = 0;
    for (const auto& e : fx)
        for (const auto& st : e.stages)
            for (const auto& slot : st.slots)
                if (slot.emitter > 0) {
                    ++refs;
                    bad += static_cast<std::size_t>(slot.emitter) >= defs.size();
                }
    CHECK(refs > 1000);
    CHECK(bad == 0);
    for (const char* name : {"actoremittersnew.edd", "environmentemittersnew.edd"})
        if (auto d = game.read(name)) CHECK_FALSE(assets::parse_edd(*d).empty());
}

TEST_CASE("Actor emitters: PRT versions widen as the EQGM loader widens them") {
    auto record = [](std::vector<std::byte>& out, std::size_t size, std::uint32_t emitter, const char* point,
                     std::initializer_list<std::int32_t> words) {
        std::vector<std::byte> r(size);
        std::memcpy(r.data(), &emitter, 4);
        std::memcpy(r.data() + 4, point, std::strlen(point));
        std::size_t o = 68;
        for (std::int32_t w : words) {
            if (o + 4 > size) break;
            std::memcpy(r.data() + o, &w, 4);
            o += 4;
        }
        out.insert(out.end(), r.begin(), r.end());
    };
    auto file = [&](std::uint32_t version, std::size_t size) {
        std::vector<std::byte> d(12);
        std::memcpy(d.data(), "PTCL", 4);
        std::uint32_t n = 1;
        std::memcpy(d.data() + 4, &n, 4);
        std::memcpy(d.data() + 8, &version, 4);
        record(d, size, 1059, "IT12753_POINT1", {0, 0, 0, 0, 480, 5000, 1, 2, 0, 77});
        return d;
    };
    auto v5 = assets::parse_prt(file(5, 0x6c));
    REQUIRE(v5.size() == 1);
    CHECK(v5[0].emitter == 1059u);
    CHECK(v5[0].point == "IT12753_POINT1");
    CHECK(v5[0].delay_ms == 480);
    CHECK(v5[0].duration_ms == 5000);
    CHECK(v5[0].floor == 1);
    CHECK(v5[0].variant == 2);
    CHECK(v5[0].alternate == 77u);
    auto v1 = assets::parse_prt(file(1, 0x5c));
    REQUIRE(v1.size() == 1);
    CHECK(v1[0].floor == 0);
    CHECK(v1[0].variant == -1);
    CHECK(v1[0].alternate == 1059u);
    auto v3 = assets::parse_prt(file(3, 0x64));
    CHECK(v3[0].variant == 2);
    CHECK(v3[0].alternate == 1059u);
    CHECK(assets::parse_prt(file(6, 0x6c)).empty());
    // A count whose records would wrap a 32-bit size: refused as truncated, as on 64 bits.
    for (const char* magic : {"PTCL", "EQPT"}) {
        std::vector<std::byte> d(12 + 0x6c);
        std::memcpy(d.data(), magic, 4);
        const std::uint32_t n = 0xffffffffu, version = magic[0] == 'P' ? 5u : 1u;
        std::memcpy(d.data() + 4, &n, 4);
        std::memcpy(d.data() + 8, &version, 4);
        if (magic[0] == 'P') CHECK_THROWS(assets::parse_prt(d));
        else CHECK_THROWS(assets::parse_pts(d));
    }
}

TEST_CASE("NMS actor emitters: a fire elemental and a glowing sword" * doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    auto archive = game.find("arxmentisweapons.eqg");
    REQUIRE(archive);
    auto pfs = assets::PfsArchive::open(*archive);
    auto prt = pfs.read("it12753.prt");
    auto pts = pfs.read("it12753.pts");
    REQUIRE(prt);
    REQUIRE(pts);
    auto e = assets::parse_prt(*prt);
    auto p = assets::parse_pts(*pts);
    REQUIRE(e.size() == 1);
    CHECK(e[0].emitter == 1059u);
    CHECK(e[0].trigger == 0);
    REQUIRE(assets::find_point(p, e[0].point));
    CHECK(assets::find_point(p, e[0].point)->bone == "ATTACH_TO_ORIGIN");
    auto edd = game.read("actoremittersnew.edd");
    REQUIRE(edd);
    // NMS ships an older ActorEmittersNew.edd (944 or 946 records with the sentinel, by install) than these items
    // expect: their emitter is out of range, and none is made. The record count is install data, so only the range
    // is checked.
    auto defs = assets::parse_edd(*edd);
    CHECK_FALSE(defs.empty());
    CHECK(e[0].emitter >= defs.size());
}

TEST_CASE("particle billboard modes: facing and turning clockwise, streaking along the axis, flat on the ground") {
    game::Particle p;
    p.position = {10, 20, 30};
    p.width = 2.0f;
    p.height = 4.0f;
    const glm::vec3 right(0, 1, 0), up(0, 0, 1);
    std::array<glm::vec3, 4> c;
    // Mode 0 faces the camera: the texture's top-left first.
    REQUIRE(render::particle_corners(p, 0, right, up, c));
    CHECK(glm::length(c[0] - glm::vec3(10, 19, 32)) < 1e-5f);
    CHECK(glm::length(c[2] - glm::vec3(10, 21, 28)) < 1e-5f);
    // A quarter turn (128 512ths) turns it clockwise on the screen: its top edge now on the right.
    p.rotation = 128.0f;
    REQUIRE(render::particle_corners(p, 0, right, up, c));
    CHECK(glm::length(c[0] - glm::vec3(10, 22, 31)) < 1e-5f);
    // Mode 2 lies on the world's horizontal plane, its top towards +x at no rotation, whatever the camera.
    p.rotation = 0.0f;
    REQUIRE(render::particle_corners(p, 2, right, up, c));
    for (const auto& v : c) CHECK(v.z == doctest::Approx(30.0f));
    CHECK(glm::length(c[0] - glm::vec3(12, 21, 30)) < 1e-5f);
    // Mode 1 streaks along the particle's axis, its length the particle's, widened on the screen's x only.
    p.axis = {0, 0, 1};
    p.length = 6.0f;
    REQUIRE(render::particle_corners(p, 1, right, up, c));
    CHECK(c[0].z == doctest::Approx(33.0f));
    CHECK(c[3].z == doctest::Approx(27.0f));
    CHECK(std::abs(c[1].y - c[0].y) == doctest::Approx(2.0f)); // the full width across a streak upright on screen
    p.axis = {0, 1, 0}; // lying across the screen: no width
    REQUIRE(render::particle_corners(p, 1, right, up, c));
    CHECK(glm::length(c[1] - c[0]) < 1e-5f);
    // Below the axis without +0x198: not drawn.
    p.hidden = true;
    CHECK_FALSE(render::particle_corners(p, 0, right, up, c));
}

TEST_CASE("Generated terrain ribbons keep their sloped world basis as the camera moves") {
    game::Particle p;
    p.position = {10, 20, 30};
    p.width = 2.0f;
    p.height = 4.0f;
    p.rotation = 128.0f; // generated ribbons ignore camera-facing rotation
    p.axes[0] = {0, 3, 0};
    p.axes[1] = {4, 0, 3};
    std::array<glm::vec3, 4> first, moved;
    REQUIRE(render::particle_corners(p, game::kWorldBasisBillboard, {0, 1, 0}, {0, 0, 1}, first));
    REQUIRE(render::particle_corners(p, game::kWorldBasisBillboard, {1, 0, 0}, {0, 0, -1}, moved));
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(glm::length(first[i] - moved[i]) < 1e-5f);
        CHECK(first[i].z - 30.0f == doctest::Approx(0.75f * (first[i].x - 10.0f)));
    }
    CHECK(glm::length(first[1] - first[0]) == doctest::Approx(2.0f));
    CHECK(glm::length(first[3] - first[0]) == doctest::Approx(4.0f));
    CHECK(glm::length(first[0] - glm::vec3(11.6f, 19.0f, 31.2f)) < 1e-5f);

    // The original's unrecognised values retain its camera-facing fallback, including mode 3.
    std::array<glm::vec3, 4> facing;
    REQUIRE(render::particle_corners(p, 0, {0, 1, 0}, {0, 0, 1}, facing));
    REQUIRE(render::particle_corners(p, 3, {0, 1, 0}, {0, 0, 1}, moved));
    for (std::size_t i = 0; i < facing.size(); ++i) CHECK(glm::length(facing[i] - moved[i]) < 1e-5f);

    p.axes[0] = {0, 0, 0};
    CHECK_FALSE(render::particle_corners(p, game::kWorldBasisBillboard, {0, 1, 0}, {0, 0, 1}, moved));
    p.axes[0] = {1, 0, 0};
    p.axes[1].x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(render::particle_corners(p, game::kWorldBasisBillboard, {0, 1, 0}, {0, 0, 1}, moved));
}

TEST_CASE("Find strand segments keep their 3D ends and constant width while facing the camera") {
    game::Particle p;
    p.position = {10, 20, 30};
    p.width = 0.08f;
    p.height = 10.0f;
    p.axes[1] = {4, 0, 3};
    p.rotation = 128.0f;
    const glm::vec3 tangent = glm::normalize(p.axes[1]);
    std::array<glm::vec3, 4> corners;
    for (const auto& camera : {std::pair{glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)},
                               std::pair{glm::vec3(1, 0, 0), glm::vec3(0, 0, 1)}}) {
        const auto& [right, up] = camera;
        REQUIRE(render::particle_corners(p, game::kTrailStrandBillboard, right, up, corners));
        CHECK(glm::length((corners[0] + corners[1]) * 0.5f - glm::vec3(14, 20, 33)) < 1e-5f);
        CHECK(glm::length((corners[2] + corners[3]) * 0.5f - glm::vec3(6, 20, 27)) < 1e-5f);
        const glm::vec3 across = corners[1] - corners[0];
        CHECK(glm::length(across) == doctest::Approx(0.08f).epsilon(0.001));
        CHECK(std::abs(glm::dot(across, tangent)) < 1e-5f);
        CHECK(std::abs(glm::dot(across, glm::cross(up, right))) < 1e-5f);
    }
    // A segment pointing straight into the camera has no visible ribbon plane.
    p.axes[1] = {0, 1, 0};
    CHECK_FALSE(render::particle_corners(p, game::kTrailStrandBillboard, {1, 0, 0}, {0, 0, 1}, corners));
    p.axes[1] = {0, 0, 0};
    CHECK_FALSE(render::particle_corners(p, game::kTrailStrandBillboard, {1, 0, 0}, {0, 0, 1}, corners));
    p.axes[1] = {1, 0, std::numeric_limits<float>::infinity()};
    CHECK_FALSE(render::particle_corners(p, game::kTrailStrandBillboard, {1, 0, 0}, {0, 0, 1}, corners));
    p.axes[1] = {1, 0, 0};
    CHECK_FALSE(render::particle_corners(p, game::kTrailStrandBillboard,
                                       {std::numeric_limits<float>::quiet_NaN(), 0, 0}, {0, 0, 1}, corners));
}

namespace {

// A flame cloud rising from a point: `lifetime` s particles at 60 a second, rising at 0.5..2 units/s and speeding up
// by `accel` units/s², 1 unit wide and 1..6 tall.
hailstone::assets::EmitterDefinition rising_flame(float lifetime, float accel) {
    hailstone::assets::EmitterDefinition d;
    d.name = "test_flame";
    d.texture = "new_fire.dds";
    d.additive = 1;
    d.lifetime = lifetime;
    d.per_tick = 6;
    d.ticks_per_second = 10.0f;
    d.motion[0] = {0.5f, 2.0f, accel};
    d.height_start = 1.0f;
    d.height_end = 6.0f;
    d.width_end = 1.0f;
    return d;
}

} // namespace

TEST_CASE("measure_flame_cloud: a long-lived cloud is measured once its oldest particles have risen") {
    using namespace hailstone;
    // Record 0 is the EDD's "None".
    game::ParticleSystem sim({assets::EmitterDefinition{}, rising_flame(5.0f, 2.0f), rising_flame(0.5f, 0.0f)});
    const glm::mat4 at = glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, 200.0f, 300.0f));

    // Rising for 5 s: 2.5..10 units at the start speed and 25 more from the acceleration. After only a second or two
    // the cloud would be under 10 units, and potranquility's emitters stacked 10 apart would burn as separate flames.
    auto tall = app::measure_flame_cloud(sim, 1, at);
    CHECK(tall.height > 20.0f);
    CHECK(tall.height < 40.0f);
    // Relative to the node, its translation left out: the foot is the newest particles' quads, up to 6 units tall.
    CHECK(tall.base.z > -3.0f);
    CHECK(tall.base.z < 0.5f);
    CHECK(std::abs(tall.base.x) < 1.0f);

    // A short-lived one is no taller than its particles go: 1 unit of rise and a 6-unit quad at most.
    auto small = app::measure_flame_cloud(sim, 2, at);
    CHECK(small.height > 0.5f);
    CHECK(small.height < 5.0f);
}

TEST_CASE("an environment emitter's fixture holds a lifetime of particles") {
    using namespace hailstone;
    // Duration 0, as in most of environmentemittersnew.edd: created for 9999 s, its capacity covers 5 s of 6 per tick
    // at 10 ticks a second. With its own duration of 0 it held 7, and burnt in puffs.
    game::ParticleSystem ps({assets::EmitterDefinition{}, rising_flame(5.0f, 2.0f)});
    REQUIRE(ps.add_fixture(1, glm::mat4(1.0f)));
    CHECK(ps.emitters()[0].counts.capacity == 6 * 51); // (trunc(5 x 10) + 1) x 6
    for (int i = 0; i < 100; ++i) ps.update(0.05f, {});
    CHECK(ps.emitters()[0].particles.size() > 250u);
}

TEST_CASE("a long frame fills an emitter to its capacity at once, its leftover ticks skipped") {
    using namespace hailstone;
    game::ParticleSystem ps({assets::EmitterDefinition{}, rising_flame(5.0f, 2.0f)});
    REQUIRE(ps.add_fixture(1, glm::mat4(1.0f)));
    const auto cap = static_cast<std::size_t>(ps.emitters()[0].counts.capacity);
    ps.update(0.05f, {}); // the initial count
    ps.update(1.0e6f, {}); // ten million ticks, counted from the next frame (the time before this step's)
    ps.update(0.05f, {});  // what does not fit is dropped at once
    CHECK(ps.emitters()[0].particles.size() <= cap);
    CHECK(ps.emitters()[0].emitted == 10000000);
    // The glow test is worked out per definition.
    CHECK(ps.glows(1));
    CHECK_FALSE(ps.glows(0));
    CHECK_FALSE(ps.glows(7));
}

TEST_CASE("the particle update as EQGraphicsDX9's: offsets, radius, drift, wind, ramps and the actor's frame") {
    using namespace hailstone;
    auto one = [](assets::EmitterDefinition d) {
        d.initial_count = 1;
        if (d.lifetime == -1.0f) d.lifetime = 2.0f;
        return game::ParticleSystem({assets::EmitterDefinition{}, d});
    };
    auto start = [](game::ParticleSystem& ps, std::uint32_t actor = 0, std::int32_t point = -1) {
        game::SpellEmitterRequest r;
        r.emitter = 1;
        r.duration_ms = 1000;
        r.position = {10, 20, 30};
        r.actor = actor;
        r.bone_point = point;
        REQUIRE(ps.create(r) > 0);
    };
    {
        // +0xb8 is along the axis, +0xbc transverse 1 (+X), +0xc0 transverse 2 (+Y without a matrix).
        assets::EmitterDefinition d;
        d.offset = {1.0f, 2.0f, 3.0f};
        auto ps = one(d);
        start(ps);
        ps.update(0.01f, {});
        const auto& p = ps.emitters()[0].particles.at(0);
        CHECK(glm::length(p.position - glm::vec3(12, 23, 31)) < 1e-4f);
    }
    {
        // A disk pulled in (radial -4 /s from radius 2): through the axis after half a second, then not drawn.
        assets::EmitterDefinition d;
        d.shape = 9;
        d.shape_size = {2.0f, 0.0f, 0.0f};
        d.motion[3] = {-4.0f, -4.0f, 0.0f};
        auto ps = one(d);
        start(ps);
        ps.update(0.25f, {});
        CHECK_FALSE(ps.emitters()[0].particles.at(0).hidden);
        CHECK(ps.emitters()[0].particles.at(0).radius == doctest::Approx(1.0f));
        ps.update(0.5f, {});
        CHECK(ps.emitters()[0].particles.at(0).radius == doctest::Approx(-1.0f));
        CHECK(ps.emitters()[0].particles.at(0).hidden);
    }
    {
        // The wind is a steady velocity, gravity an acceleration; transverse motion drifts in the world.
        assets::EmitterDefinition d;
        d.wind_x = 2.0f;
        d.gravity = 4.0f;
        d.motion[2] = {1.0f, 1.0f, 0.0f};
        auto ps = one(d);
        start(ps);
        ps.update(0.5f, {}); // born and moved half a second
        ps.update(0.5f, {});
        const auto& p = ps.emitters()[0].particles.at(0);
        CHECK(p.position.x == doctest::Approx(12.0f));      // 2 /s for 1 s
        CHECK(p.position.y == doctest::Approx(21.0f));      // transverse 2 at 1 /s
        CHECK(p.position.z == doctest::Approx(27.0f)); // semi-implicit: (2 + 4) x 0.5 s
    }
    {
        // Ramps near the end: +0x190 on the height, +0x194 on the width; the flipbook rounds.
        assets::EmitterDefinition d;
        d.lifetime = 1.0f;
        d.shrink_x = 0.5f;
        d.shrink_y = 0.25f;
        d.frames = 4;
        d.frames_per_second = 4.0f;
        auto ps = one(d);
        start(ps);
        ps.update(0.8f, {});
        const auto& p = ps.emitters()[0].particles.at(0);
        CHECK(p.ramp_height == doctest::Approx(0.4f)); // 0.2 s left of 0.5
        CHECK(p.ramp_width == doctest::Approx(0.8f));  // of 0.25
        CHECK(p.frame == 3u);                          // round(4 x 0.8) = 3 (floor would agree; 0.9 s would not)
    }
    {
        // A spell emitter on an actor takes the actor's turn every frame (axial = its +Z, transverse 1 its +X,
        // transverse 2 its -Y); one without a point stays where it was made.
        assets::EmitterDefinition d;
        d.offset = {0.0f, 1.0f, 0.0f};
        auto ps = one(d);
        start(ps, 7, -1);
        const glm::mat3 turned(glm::vec3(0, 1, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1)); // a quarter turn left
        ps.update(0.01f, [&](std::uint32_t, std::int32_t) {
            game::EmitterAttachment a;
            a.actor_axes = turned;
            return std::optional<game::EmitterAttachment>(a);
        });
        const auto& p = ps.emitters()[0].particles.at(0);
        CHECK(glm::length(p.position - glm::vec3(10, 21, 30)) < 1e-4f); // +0xbc along the actor's +X (world +Y)
    }
    {
        // A lifetime below 0: each particle lives what is left of its emitter.
        assets::EmitterDefinition d;
        d.lifetime = -1.0f;
        d.per_tick = 1;
        d.ticks_per_second = 2.0f;
        game::ParticleSystem ps({assets::EmitterDefinition{}, d});
        start(ps);
        ps.update(0.6f, {});
        ps.update(0.01f, {});
        REQUIRE(ps.emitters()[0].particles.size() == 2);
        CHECK(ps.emitters()[0].particles[1].lifetime == doctest::Approx(0.4f));
    }
}

TEST_CASE("moddat.ini PESFactor: the particle scale of a model") {
    auto t = hailstone::game::parse_height_tables({}, "[BAS]\nPESFactor=0.5\nROffset=0.577\n[HUM]\nROffset=3\n");
    CHECK(t.pes_factor("BAS") == 0.5f);
    CHECK(t.pes_factor("bas") == 0.5f);
    CHECK(t.pes_factor("HUM") == 1.0f);
}

TEST_CASE("an actor's own emitter is made for 5,000 s, not its entry's 5 s") {
    // CActor::vf126 passes 5,000,000 ms whatever the .prt entry's +0x58 (5000) says: a lifetime below 0 then lasts
    // the emitter's time, and the ring holds a lifetime of ticks, not five seconds' worth.
    hailstone::assets::EmitterDefinition d;
    d.initial_count = 1;
    d.per_tick = 4;
    d.ticks_per_second = 10.0f;
    d.lifetime = 8.0f;
    const float duration = hailstone::game::emitter_duration_seconds(0.0f, hailstone::game::kActorEmitterDurationMs);
    CHECK(duration == doctest::Approx(5000.0f));
    CHECK(hailstone::game::emitter_counts(d, 1.0f, duration).capacity == (80 + 1) * 4);
    CHECK(hailstone::game::emitter_counts(d, 1.0f, 5.0f).capacity == 1 + 50 * 4);
}

TEST_CASE("SpellEffect_SpawnStageEmitters: an actor whose model is not known yet keeps the point a slot asks for") {
    // A nimbus sent as the spawn zones in, before its model has loaded (Hailstone; the original's actor has its
    // points from the start).
    auto run = [](std::int32_t attach) {
        assets::SpellEffect fx;
        fx.stages[2].slots[0] = {10, 0, 1, attach};
        game::StageActor caster{7, {1, 2, 3}, 10, 6.0f};
        caster.model_known = false;
        return game::spawn_stage_emitters(fx, 2, 468, &caster, &caster, {1, 2, 3}, 0, {});
    };
    auto chest = run(2);
    REQUIRE(chest.size() == 1);
    CHECK(chest[0].bone_point == 1);
    CHECK(chest[0].actor == 7u);
    CHECK(run(10)[0].bone_point == 8); // an eye: kept too, found or not once the model is drawn
    auto at12 = run(12);
    REQUIRE(at12.size() == 1);
    CHECK(at12[0].position == glm::vec3(1, 2, 3)); // no chest known: the spawn's place
}

TEST_CASE("the particle update draws the same particles on the worker threads as on one thread") {
    // The ends and the motion are split over the workers; the attachments and the emission stay on the calling
    // thread, so a seed draws the same particles however the work is split, from the main thread or from a task.
    auto build = [] {
        assets::EmitterDefinition spread = rising_flame(1.5f, 2.0f);
        spread.shape = 5;
        spread.shape_size = {1.0f, 0.0f, 0.0f};
        spread.motion[3] = {0.5f, 1.5f, 0.0f};
        spread.initial_count = 20;
        game::ParticleSystem ps({assets::EmitterDefinition{}, rising_flame(2.0f, 1.0f), spread}, 7);
        for (int i = 0; i < 40; ++i)
            REQUIRE(ps.add_fixture(1 + static_cast<std::size_t>(i % 2),
                                   glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(i), 0.0f, 0.0f))));
        for (std::uint32_t actor = 1; actor <= 12; ++actor) {
            game::SpellEmitterRequest r;
            r.emitter = 2;
            r.key = actor;
            r.duration_ms = game::kActorEmitterDurationMs;
            r.actor = actor;
            r.bone_point = 0;
            r.point_only = true;
            r.persistent = true;
            ps.create(r); // 0: its particles show past 600 s (EmitterCounts::length_ms), not that none was made
        }
        REQUIRE(ps.emitters().size() == 52);
        return ps;
    };
    auto run = [](game::ParticleSystem& ps, std::thread::id caller, int& foreign) {
        game::AttachmentFn attachment = [&](std::uint32_t actor, std::int32_t) {
            if (std::this_thread::get_id() != caller) ++foreign;
            game::EmitterAttachment at;
            at.position = glm::vec3(0.0f, static_cast<float>(actor), 5.0f);
            at.axes = glm::mat3(1.0f);
            return std::optional<game::EmitterAttachment>(at);
        };
        for (int frame = 0; frame < 60; ++frame) {
            ps.set_viewer(glm::vec3(0.0f, 0.0f, static_cast<float>(frame)));
            ps.update(1.0f / 30.0f, attachment);
        }
    };
    auto same = [](const game::ParticleSystem& a, const game::ParticleSystem& b) {
        REQUIRE(a.emitters().size() == b.emitters().size());
        std::size_t particles = 0;
        for (std::size_t i = 0; i < a.emitters().size(); ++i) {
            const auto& pa = a.emitters()[i].particles;
            const auto& pb = b.emitters()[i].particles;
            REQUIRE(pa.size() == pb.size());
            for (std::size_t j = 0; j < pa.size(); ++j) {
                CHECK(pa[j].position == pb[j].position);
                CHECK(pa[j].color == pb[j].color);
                CHECK(pa[j].size_factor == pb[j].size_factor);
            }
            particles += pa.size();
        }
        CHECK(particles > 1000u);
    };

    core::set_workers(nullptr);
    game::ParticleSystem alone = build();
    int foreign = 0;
    run(alone, std::this_thread::get_id(), foreign);
    CHECK(foreign == 0);

    core::WorkerPool pool(3);
    core::set_workers(&pool);
    game::ParticleSystem split = build();
    run(split, std::this_thread::get_id(), foreign);
    CHECK(foreign == 0); // the attachments are asked on the calling thread only
    same(alone, split);

    // From a background task (the actor effects' step): its loops spread over the other workers too.
    game::ParticleSystem tasked = build();
    auto task = pool.run([&] { run(tasked, std::this_thread::get_id(), foreign); });
    task.wait();
    CHECK(foreign == 0);
    same(alone, tasked);
    core::set_workers(nullptr);
}

TEST_CASE("stop_keys and stop_actors take many emitters in one pass; set_held marks the attached ones") {
    game::ParticleSystem ps({assets::EmitterDefinition{}, rising_flame(1.0f, 1.0f)});
    for (std::uint32_t i = 1; i <= 6; ++i) {
        game::SpellEmitterRequest r;
        r.emitter = 1;
        r.key = i;
        r.duration_ms = 1000;
        r.actor = i <= 4 ? 10 + (i % 2) : 0; // keys 1, 3 on actor 11; 2, 4 on actor 10; 5, 6 on none
        REQUIRE(ps.create(r) > 0);
    }
    ps.set_held([](std::uint32_t actor) { return actor == 11; });
    std::size_t held = 0;
    for (const auto& e : ps.emitters()) held += e.held ? 1u : 0u;
    CHECK(held == 2);
    const std::vector<std::uint32_t> keys{2, 5};
    ps.stop_keys(keys);
    CHECK(ps.emitters().size() == 4);
    const std::vector<std::uint32_t> actors{0, 11}; // 0 is no actor: the unattached ones stay
    ps.stop_actors(actors);
    REQUIRE(ps.emitters().size() == 2);
    CHECK(ps.emitters()[0].key == 4);
    CHECK(ps.emitters()[1].key == 6);
}

TEST_CASE("A held emitter keeps its actor's scale and alpha, and one held when made keeps its burst for the view") {
    auto make = [](game::ParticleSystem& ps) {
        game::SpellEmitterRequest r;
        r.emitter = 1;
        r.key = 1;
        r.duration_ms = 60000;
        r.actor = 11;
        REQUIRE(ps.create(r) > 0);
    };
    {
        assets::EmitterDefinition d = rising_flame(5.0f, 0.0f);
        d.actor_scales_size = 1;
        game::ParticleSystem ps({assets::EmitterDefinition{}, d});
        make(ps);
        int asked = 0;
        auto attachment = [&](std::uint32_t, std::int32_t) {
            ++asked;
            game::EmitterAttachment a;
            a.alpha = 0.5f;
            a.actor_scale = 2.0f;
            return std::optional<game::EmitterAttachment>(a);
        };
        ps.update(0.2f, attachment);
        REQUIRE_FALSE(ps.emitters()[0].particles.empty());
        const float size = ps.emitters()[0].particles[0].size_factor;
        CHECK(ps.emitters()[0].particles[0].color.a == doctest::Approx(0.5f));
        ps.set_held([](std::uint32_t actor) { return actor == 11; });
        asked = 0;
        ps.update(0.1f, attachment);
        CHECK(asked == 0); // its pose is not asked for
        CHECK(ps.emitters()[0].particles[0].color.a == doctest::Approx(0.5f));
        CHECK(ps.emitters()[0].particles[0].size_factor == doctest::Approx(size));
    }
    {
        assets::EmitterDefinition d = rising_flame(5.0f, 0.0f);
        d.initial_count = 5;
        d.per_tick = 1;
        game::ParticleSystem ps({assets::EmitterDefinition{}, d});
        make(ps);
        ps.set_held([](std::uint32_t) { return true; });
        for (int i = 0; i < 10; ++i) ps.update(0.1f, {}); // a second out of view: ten ticks
        CHECK(ps.emitters()[0].particles.empty());
        CHECK(ps.emitters()[0].emitted < 0);
        ps.set_held([](std::uint32_t) { return false; });
        ps.update(0.1f, {});
        // The burst, and the ticks from now on, not the second's ten at once.
        CHECK(ps.emitters()[0].particles.size() >= 5u);
        CHECK(ps.emitters()[0].particles.size() <= 6u);
    }
}

TEST_CASE("ParticleSystem::set_still from a span: the same emitter as from a vector, its list replaced in place") {
    game::ParticleSystem ps({assets::EmitterDefinition{}, assets::EmitterDefinition{}});
    std::vector<game::Particle> quads(3);
    for (std::size_t i = 0; i < quads.size(); ++i) quads[i].position = glm::vec3(static_cast<float>(i), 0, 0);
    ps.set_still(7, 0, std::span<const game::Particle>(quads));
    REQUIRE(ps.emitters().size() == 1);
    CHECK(ps.emitters()[0].still);
    CHECK(ps.emitters()[0].key == 7);
    REQUIRE(ps.emitters()[0].particles.size() == 3);
    CHECK(ps.emitters()[0].particles[2].position.x == 2.0f);
    // Set again (fewer, another definition): the same emitter, with the new list.
    quads.resize(1);
    quads[0].position.x = 9.0f;
    ps.set_still(7, 1, std::span<const game::Particle>(quads));
    REQUIRE(ps.emitters().size() == 1);
    CHECK(ps.emitters()[0].definition == 1);
    REQUIRE(ps.emitters()[0].particles.size() == 1);
    CHECK(ps.emitters()[0].particles[0].position.x == 9.0f);
    ps.set_still(8, 5, std::span<const game::Particle>(quads)); // a definition out of range: nothing
    CHECK(ps.emitters().size() == 1);
}
