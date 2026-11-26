#include <doctest/doctest.h>

#include <utility>
#include <vector>

#include "core/ini_file.h"
#include "game/precipitation.h"
#include "game/weather.h"

using namespace hailstone;
using namespace hailstone::game;

namespace {

// Resources/Precipitation/default.ini's snow and rain.
constexpr const char* kIni = R"(
[Falling1]
MaxSpeed=64.304464
Acceleration=32.152232
Width=0.01
Height=1.0
Rate=1.0
Frames=0
Orientation=1
File=rain_drop02

[Falling2]
MaxSpeed=8.038058
Acceleration=32.152232
Width=0.15
Height=0.15
Rate=16.0
Frames=16
Orientation=0
File=snowflake02

[Splashing4]
Life=1.0
Chance=0.1
Width=0.12
Height=0.12
Rate=16
Frames=16
Orientation=2
File=snow_hit

[Settings]
Valid=1
RainFalling1=1
SnowFalling1=2
SnowSplashing1=4
)";

PrecipitationDefinitions defs() {
    IniFile ini;
    ini.parse(kIni);
    return PrecipitationDefinitions::parse(ini);
}

// rand() always 0x4000: r = 0.50002, so the angle's a = 3e-5 (straight ahead), 100 units out, 65 up.
PrecipitationField::Frame frame(float seconds, float intensity) {
    PrecipitationField::Frame f;
    f.seconds = seconds;
    f.intensity = intensity;
    f.camera = glm::vec3(10.0f, 20.0f, 5.0f);
    f.heading = 0.0f;
    f.random = [] { return std::int32_t{0x4000}; };
    // Flat ground at z = 0.
    f.ground = [](const glm::vec3& a, const glm::vec3& b, bool) -> std::optional<PrecipitationHit> {
        if (a.z < 0.0f || b.z > 0.0f) return std::nullopt;
        return PrecipitationHit{a.z / (a.z - b.z), 1.0f};
    };
    return f;
}

} // namespace

TEST_CASE("precipitation: default.ini's definitions by kind") {
    auto d = defs();
    REQUIRE(d.kinds[1].falling.size() == 1);
    CHECK(d.kinds[1].falling[0].max_speed == doctest::Approx(8.038058f));
    CHECK(d.kinds[1].falling[0].frames == 16);
    CHECK(d.kinds[1].falling[0].orientation == 0);
    CHECK(d.kinds[1].falling[0].file == "snowflake02");
    REQUIRE(d.kinds[1].splashing.size() == 1);
    CHECK(d.kinds[1].splashing[0].chance == doctest::Approx(0.1f));
    CHECK(d.kinds[1].splashing[0].orientation == 2);
    REQUIRE(d.kinds[0].falling.size() == 1);
    CHECK(d.kinds[0].falling[0].orientation == 1);
    CHECK(d.kinds[0].splashing.empty());
    CHECK(d.kinds[2].falling.empty()); // no HailFalling1 here
    CHECK(d.kind(0xff) == nullptr);
}

TEST_CASE("precipitation: round(count x dt x p) a frame, at most 10 new ones, round the camera ahead of it") {
    PrecipitationField field;
    field.set_definitions(defs());
    field.set_kind(1);
    // 7500 x 0.1 x 0.1 = 75: ten new particles, the rest would fall on landed columns, of which there are none.
    field.update(frame(0.1f, 0.1f));
    REQUIRE(field.active().size() == 10);
    const auto& p = field.particles()[field.active()[0]];
    CHECK(p.state == PrecipitationParticle::Falling);
    CHECK(p.position.x == doctest::Approx(10.0f + 100.0f).epsilon(0.001));
    CHECK(p.position.y == doctest::Approx(20.0f).epsilon(0.01));
    // 65 above the camera, then moved in the same update: 0.1 s at (2.01 + 3.22).
    CHECK(p.position.z == doctest::Approx(5.0f + 65.0f - 0.1f * (8.038058f * 0.25f + 3.2152232f)).epsilon(0.001));
    CHECK(p.ground.z == doctest::Approx(0.05f).epsilon(0.01));
    // (1 - r)^2 of MaxSpeed to start with.
    CHECK(p.start_speed == doctest::Approx(8.038058f * 0.25f).epsilon(0.001));
    CHECK(p.max_speed == doctest::Approx(8.038058f));
    // r = 0.50002 > Chance 0.1: no splash.
    CHECK(p.splash_life == 0.0f);

    // Kind 0xff: nothing more, and everything stops.
    field.set_kind(0xff);
    CHECK(field.active().empty());
    CHECK(field.free_count() == PrecipitationField::kParticles);
}

TEST_CASE("precipitation: a flake lands, its column is reused, and let go beyond 200 units") {
    PrecipitationField field(4);
    field.set_definitions(defs());
    field.set_kind(1);
    field.update(frame(0.1f, 0.1f)); // 7500 is the count's, not 4: n = round(4 x 0.01) = 0
    CHECK(field.active().empty());
    field.update(frame(1.0f, 1.0f)); // round(4 x 1 x 1) = 4 new
    REQUIRE(field.active().size() == 4);
    // At most 8.04 a second from 70 units up: down within 10 s.
    for (int i = 0; i < 100; ++i) field.update(frame(0.1f, 0.0f));
    CHECK(field.active().empty());
    CHECK(field.cached_count() == 4);
    // Reused from 40 to 140 above its own ground, at its first speed.
    auto f = frame(0.25f, 1.0f);
    field.update(f); // round(4 x 0.25) = 1: no free particle, so the new one takes a column
    REQUIRE(field.active().size() == 1);
    // The camera far away: the columns are let go.
    auto far = frame(0.0f, 0.0f);
    far.camera = glm::vec3(1000.0f, 0.0f, 0.0f);
    field.update(far);
    CHECK(field.cached_count() == 0);
}

// Level planes at heights `zs`: one with up > 0 faces up (a falling line meets its front), else down (its back, met
// only with back faces).
PrecipitationGround planes(std::vector<std::pair<float, bool>> zs) {
    return [zs](const glm::vec3& a, const glm::vec3& b, bool back_faces) -> std::optional<PrecipitationHit> {
        std::optional<PrecipitationHit> best;
        for (auto [z, up] : zs) {
            if (!(a.z >= z && b.z <= z) || (!up && !back_faces)) continue;
            const float t = (a.z - z) / (a.z - b.z);
            if (!best || t < best->t) best = PrecipitationHit{t, up ? 1.0f : -1.0f};
        }
        return best;
    };
}

TEST_CASE("precipitation: a ceiling facing down over the drops is no ground; a back face under one is") {
    PrecipitationField field;
    field.set_definitions(defs());
    field.set_kind(1);
    // The ground at 0 and a zone's ceiling at 500, facing down (East Commonlands' at z 719): the probe from 1200
    // units above the drop (at 70) meets the ceiling's back first. The drops must fall to the ground all the same.
    auto f = frame(0.1f, 0.1f);
    f.ground = planes({{0.0f, true}, {500.0f, false}});
    field.update(f);
    REQUIRE(field.active().size() == 10);
    CHECK(field.particles()[field.active()[0]].ground.z == doctest::Approx(0.05f).epsilon(0.01));
    CHECK(field.particles()[field.active()[0]].state == PrecipitationParticle::Falling);

    // A roof seen only from above (its back, facing down) at 30, under the drop: the drop stops on it.
    PrecipitationField roofed;
    roofed.set_definitions(defs());
    roofed.set_kind(1);
    f.ground = planes({{0.0f, true}, {30.0f, false}, {500.0f, false}});
    roofed.update(f);
    REQUIRE(roofed.active().size() == 10);
    const auto& p = roofed.particles()[roofed.active()[0]];
    CHECK(p.ground.z == doctest::Approx(30.05f).epsilon(0.001));
    CHECK_FALSE(p.may_splash); // the back face's normal points down

    // A roof facing up over the drop (it started under it) is its ground, as in the original: it lands at once.
    PrecipitationField under;
    under.set_definitions(defs());
    under.set_kind(1);
    f.ground = planes({{0.0f, true}, {200.0f, true}});
    under.update(f);
    CHECK(under.active().empty());
}

TEST_CASE("precipitation: the colour is a byte-wise blend, rounded") {
    CHECK(blend_precipitation_colour(0xff000000u, 0xff0000ffu, 0.5f) == 0xff000080u);
    CHECK(blend_precipitation_colour(0x10203040u, 0x10203040u, 0.3f) == 0x10203040u);
}

TEST_CASE("precipitation: Weather_StartEffect drives the field: kind, wind, and another kind clears it") {
    Weather w;
    w.set_precipitation_definitions(defs());
    auto rnd = [] { return std::int32_t{50}; };
    w.handle_message(1, 10);
    w.update(0.0, true, rnd);
    CHECK(w.particles().kind() == 1);
    CHECK(w.particles().wind().z == -1.0f);
    w.update(1.0, true, rnd);
    w.update_particles(frame(0.0f, 0.0f));
    CHECK_FALSE(w.particles().active().empty());
    w.handle_message(0, 5); // rain: StopWeather, stop(1, 1)
    w.update(1.0, true, rnd);
    CHECK(w.particles().kind() == 0);
    CHECK(w.particles().active().empty());
}

TEST_CASE("precipitation: leaving a zone drops its weather but keeps the particle definitions") {
    Weather w;
    w.set_precipitation_definitions(defs());
    const auto random = [] { return std::int32_t{50}; };
    w.handle_message(0, 20);
    for (int i = 0; i <= 66; ++i) w.update(i * 0.02, true, random);
    w.update_particles(frame(0.0f, 0.0f));
    REQUIRE(w.raining());
    REQUIRE(w.lightning());
    REQUIRE(w.thunder() != 0);
    REQUIRE_FALSE(w.particles().active().empty());

    w.handle_message(1, 20); // still pending when the old zone is unloaded
    w.reset();
    CHECK(w.kind() == WeatherKind::None);
    CHECK(w.precipitation() == 0.0f);
    CHECK_FALSE(w.raining());
    CHECK_FALSE(w.windy());
    CHECK_FALSE(w.lightning());
    CHECK(w.thunder() == 0);
    CHECK(w.message() == 0);
    CHECK(w.particles().active().empty());
    CHECK(w.particles().cached_count() == 0);
    w.update(5.0, true, random);
    CHECK(w.kind() == WeatherKind::None);

    w.handle_message(1, 20);
    for (int i = 0; i <= 65; ++i) w.update(6.0 + i * 0.02, true, random);
    w.update_particles(frame(0.0f, 0.0f));
    CHECK(w.kind() == WeatherKind::Snow);
    CHECK(w.windy());
    CHECK_FALSE(w.particles().active().empty());
}
