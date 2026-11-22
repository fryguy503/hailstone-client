// The fog and per-frame scene light (game/fog.h) against the original's vectors (recorded from eqgame.exe):
// CDisplay_CalcFogFadeMargin (1500), CDisplay::SetFog (3000) and CDisplay_UpdateSkyWeatherAndFog (3000).
//
// Each record holds the original's inputs (its arguments and the globals it read), the answers of the mocked
// callees (the CSky's virtuals, the actor's GetScaledAmbient, TotalEffect) and what it wrote, at 24- or 53-bit x87
// precision. UpdateSkyWeatherAndFog's records use the game's own hour tables; they must equal the client's
// (assets::sky_hour_ambient), which the client then reads.
#include <doctest/doctest.h>

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>

#include "game/fog.h"
#include "vector_files.h"

using namespace hailstone::game;
using hailstone::game::mechanics::Fpu;
using hailstone::game::mechanics::FpuPrecision;
using hailstone::test::Json;
using hailstone::test::f32_bits;

namespace {

Fpu fpu_of(const Json& r) {
    return Fpu(r["precision"].text == "x87-53" ? FpuPrecision::Double53 : FpuPrecision::Single24);
}

std::string hex(std::uint64_t v) {
    std::ostringstream s;
    s << "0x" << std::hex << v;
    return s.str();
}

// The callee answers "a + b, unrounded" (a loaded, b a memory operand, at the record's precision).
double plus(const Fpu& fpu, const Json& a, const Json& b) {
    using hailstone::game::mechanics::X87;
    std::uint32_t ab = f32_bits(a);
    if ((ab & 0x7f800000u) == 0x7f800000u && (ab & 0x7fffffu)) ab |= 0x400000u; // quiet on load
    return hailstone::game::mechanics::x87_add(fpu, X87(static_cast<double>(std::bit_cast<float>(ab))),
                                          X87(std::bit_cast<float>(f32_bits(b))));
}

// A double as the vectors write it: a number, "inf", "-inf", "nan" or "nan:0x<16 hex digits>".
std::uint64_t double_bits(const Json& j) {
    if (j.type == Json::Type::String && j.text.rfind("nan:0x", 0) == 0)
        return std::strtoull(j.text.c_str() + 6, nullptr, 16);
    return hailstone::test::f64_bits(j);
}

// The recorder fills CDisplay with the bytes 0xa5 + offset and reports a field still holding them as not written.
bool sentinel(std::uint32_t value, int size, int offset) {
    for (int i = 0; i < size; ++i)
        if (((value >> (8 * i)) & 0xff) != static_cast<std::uint32_t>((0xa5 + offset + i) & 0xff)) return false;
    return true;
}

FogClip clip_of(const Json& in) {
    FogClip c;
    c.clip_plane = in["clip_plane"].i32();
    c.min_clip = f32_bits(in["min_clip"]);
    c.max_clip = f32_bits(in["max_clip"]);
    return c;
}

// A float output: null when not written.
std::string cmp_f32(const char* key, const Json& want, const std::optional<std::uint32_t>& got, int offset = -1) {
    if (want.is_null() && got && offset >= 0 && sentinel(*got, 4, offset)) return {};
    if (want.is_null() != !got.has_value()) return std::string(key) + ": written " + (got ? "yes" : "no");
    if (got && f32_bits(want) != *got) return std::string(key) + ": " + hex(*got) + " instead of " + hex(f32_bits(want));
    return {};
}

std::string cmp_byte(const char* key, const Json& want, const std::optional<std::uint8_t>& got, int offset) {
    if (want.is_null() && got && sentinel(*got, 1, offset)) return {};
    if (want.is_null() != !got.has_value()) return std::string(key) + ": written " + (got ? "yes" : "no");
    if (got && want.i64() != *got) return std::string(key) + ": " + std::to_string(*got) + " instead of " + want.text;
    return {};
}

} // namespace

TEST_CASE("Fog vectors: CDisplay_CalcFogFadeMargin") {
    hailstone::test::each_vector("CalcFogFadeMargin.jsonl", [](const Json& r) -> std::string {
        const Json& in = r["in"];
        double m = calc_fog_fade_margin(fpu_of(r), f32_bits(in["fog_start"]), clip_of(in));
        std::uint64_t got = std::bit_cast<std::uint64_t>(m), want = double_bits(r["out"]["margin"]);
        if (got != want) return "margin " + hex(got) + " instead of " + hex(want);
        return {};
    });
}

TEST_CASE("Fog vectors: CDisplay::SetFog") {
    hailstone::test::each_vector("SetFog.jsonl", [](const Json& r) -> std::string {
        const Json& in = r["in"];
        Fpu fpu = fpu_of(r);
        SetFogInput s;
        s.on = in["on"].u32();
        s.start = f32_bits(in["start"]);
        s.end = f32_bits(in["end"]);
        s.density = f32_bits(in["density"]);
        s.red = static_cast<std::uint8_t>(in["red"].i64());
        s.green = static_cast<std::uint8_t>(in["green"].i64());
        s.blue = static_cast<std::uint8_t>(in["blue"].i64());
        s.world = in["world"].truthy();
        s.yon = f32_bits(in["yon"]);
        s.raining = in["raining"].truthy();
        s.sky_flag = static_cast<std::uint8_t>(in["sky_flag"].i64());
        s.camera_region = static_cast<std::uint8_t>(in["camera_region"].i64());
        s.clip = clip_of(in);
        s.sky_option = in["sky_option"].truthy();
        s.sky_name = in["sky_name"].truthy();
        s.zone_name = in["zone_name"].truthy();
        s.zone_sky = in["zone_sky"].truthy();
        s.offline = in["offline"].truthy();
        if (in["sky_object"].truthy()) {
            SkyFogObject sky;
            sky.area_fog = in["sky_fog_override"].truthy();
            if (!in["sky_fog_colour"].is_null()) sky.area_fog_colour = in["sky_fog_colour"].u32();
            sky.area_fog_density = plus(fpu, in["sky_fog_density_a"], in["sky_fog_density_b"]);
            sky.colour = in["sky_colour"].u32();
            s.sky = sky;
        }
        SetFogResult got = set_fog(fpu, s);
        const Json& out = r["out"];
        std::string why;
        auto check = [&](std::string w) { if (why.empty()) why = std::move(w); };
        check(cmp_byte("fog_on", out["fog_on"], got.fog_on, 0x130));
        check(cmp_f32("fog_start", out["fog_start"], got.fog_start, 0x134));
        check(cmp_f32("fog_end", out["fog_end"], got.fog_end, 0x138));
        check(cmp_f32("fog_density", out["fog_density"], got.fog_density, 0x13c));
        check(cmp_byte("fog_red", out["fog_red"], got.red, 0x140));
        check(cmp_byte("fog_green", out["fog_green"], got.green, 0x141));
        check(cmp_byte("fog_blue", out["fog_blue"], got.blue, 0x142));
        if (out["sky_flag"].i64() != got.sky_flag) check("sky_flag " + std::to_string(got.sky_flag));
        const Json& rf = out["render_set_fog"];
        if (rf.is_null() != !got.render) {
            check(std::string("render_set_fog: called ") + (got.render ? "yes" : "no"));
        } else if (got.render) {
            const RenderFog& g = *got.render;
            if (rf["on"].u32() != g.on) check("render on " + std::to_string(g.on));
            if (rf["start"].i32() != g.start) check("render start " + std::to_string(g.start) + " instead of " + rf["start"].text);
            if (rf["end"].i32() != g.end) check("render end " + std::to_string(g.end) + " instead of " + rf["end"].text);
            if (f32_bits(rf["density"]) != g.density) check("render density " + hex(g.density) + " instead of " + hex(f32_bits(rf["density"])));
            if (rf["colour"].u32() != g.colour) check("render colour " + hex(g.colour) + " instead of " + hex(rf["colour"].u32()));
        }
        return why;
    });
}

TEST_CASE("Fog vectors: CDisplay_UpdateSkyWeatherAndFog") {
    const auto tables = game_hour_ambient();
    hailstone::test::each_vector("UpdateSkyWeatherAndFog.jsonl", [&](const Json& r) -> std::string {
        const Json& in = r["in"];
        const Json& out = r["out"];
        Fpu fpu = fpu_of(r);
        SkyWeatherInput s;
        s.world = in["world"].truthy();
        s.lightning = in["lightning"].truthy();
        s.camera_region = static_cast<std::uint8_t>(in["camera_region"].i64());
        s.zone_type = static_cast<std::uint8_t>(in["zone_type"].i64());
        s.hour = in["hour"].i32();
        s.minute = in["minute"].i32();
        s.infravision_innate = in["infravision_innate"].i32();
        s.ultravision_innate = in["ultravision_innate"].i32();
        // TotalEffect's answers, from the recorded calls.
        for (const Json& c : out["calls"].items) {
            if (c["fn"].text != "total_effect") continue;
            (c["spa"].i64() == 65 ? s.infravision_effect : s.ultravision_effect) = c["ret"].i32();
        }
        if (in["sky_object"].truthy()) s.sky_ambient = in["sky_ambient"].u32();
        if (in["actor"].truthy()) s.actor_light = plus(fpu, in["actor_light_a"], in["actor_light_b"]);
        s.light_level = f32_bits(in["light_level"]);
        for (std::size_t i = 0; i < 3; ++i) {
            s.infravision_floor[i] = in["infravision_floor"][i].i32();
            s.ultravision_floor[i] = in["ultravision_floor"][i].i32();
            s.zone_fog_colour[i] = static_cast<std::uint8_t>(in["zone_fog_colour"][i].i64());
            s.fog_colour[i] = in["fog_colour"][i].i32();
        }
        s.fog_start = f32_bits(in["fog_start"]);
        s.fog_end = f32_bits(in["fog_end"]);
        s.fog_density = f32_bits(in["fog_density"]);
        s.hour_ambient = tables;
        // The record's hour-table entries are the client's.
        int now = s.hour - 1, next = s.hour % 24;
        for (std::size_t t = 0; t < 3; ++t) {
            if (f32_bits(in["hour_ambient_now"][t]) != tables[t][static_cast<std::size_t>(now)] ||
                f32_bits(in["hour_ambient_next"][t]) != tables[t][static_cast<std::size_t>(next)])
                return "hour table " + std::to_string(t) + " differs from the client's at hour " + std::to_string(s.hour);
        }

        SkyWeatherResult got = update_sky_weather_and_fog(fpu, s);
        std::string why;
        auto check = [&](std::string w) { if (why.empty()) why = std::move(w); };
        // Calls, in order.
        std::vector<std::string> calls;
        if (got.asked_infravision) calls.push_back("total_effect 65");
        if (got.asked_ultravision) calls.push_back("total_effect 66");
        if (got.apply_weather_fog) calls.push_back("apply_weather_fog");
        std::vector<std::string> want_calls;
        for (const Json& c : out["calls"].items)
            want_calls.push_back(c["fn"].text == "total_effect" ? "total_effect " + c["spa"].text : c["fn"].text);
        if (calls != want_calls) check("calls differ (" + std::to_string(calls.size()) + " instead of " + std::to_string(want_calls.size()) + ")");
        // Scene colours, in order.
        std::vector<std::pair<std::string, std::uint32_t>> scene;
        if (got.special_ambient) scene.emplace_back("set_special_ambient", *got.special_ambient);
        if (got.ambient) scene.emplace_back("set_ambient", *got.ambient);
        const Json& ws = out["scene_colours"];
        if (ws.items.size() != scene.size()) {
            check("scene colours: " + std::to_string(scene.size()) + " instead of " + std::to_string(ws.items.size()));
        } else {
            for (std::size_t i = 0; i < scene.size(); ++i) {
                if (ws[i]["fn"].text != scene[i].first) check("scene colour " + std::to_string(i) + ": " + scene[i].first);
                else if (ws[i]["colour"].u32() != scene[i].second)
                    check(scene[i].first + " " + hex(scene[i].second) + " instead of " + hex(ws[i]["colour"].u32()));
            }
        }
        const Json& sf = out["set_fog_colour"];
        if (sf.is_null() != !got.set_fog_colour) {
            check(std::string("set_fog: called ") + (got.set_fog_colour ? "yes" : "no"));
        } else if (got.set_fog_colour) {
            for (std::size_t i = 0; i < 3; ++i)
                if (sf[i].i64() != (*got.set_fog_colour)[i]) check("set_fog colour " + std::to_string(i));
        }
        if (f32_bits(out["light_level"]) != got.light_level)
            check("light_level " + hex(got.light_level) + " instead of " + hex(f32_bits(out["light_level"])));
        for (std::size_t i = 0; i < 3; ++i)
            if (out["fog_colour"][i].i32() != got.fog_colour[i])
                check("fog_colour " + std::to_string(i) + ": " + std::to_string(got.fog_colour[i]) + " instead of " + out["fog_colour"][i].text);
        check(cmp_f32("ambient_mean", out["ambient_mean"], got.ambient_mean));
        return why;
    });
}

TEST_CASE("Fog: the effects' exp2 vertex fog") {
    // Before the start: no fog; at the end f = 10, so density 0.33 leaves exp(-10.89).
    CHECK(fog_factor(50.0f, 100, 1000, 0.33f) == 1.0f);
    CHECK(fog_factor(1000.0f, 100, 1000, 0.33f) == doctest::Approx(std::exp(-10.89)).epsilon(1e-4));
    CHECK(fog_factor(5000.0f, 100, 1000, 0.33f) == doctest::Approx(std::exp(-10.89)).epsilon(1e-4)); // clamped
    CHECK(fog_factor(400.0f, 100, 1000, 0.33f) == doctest::Approx(std::exp(-(0.33 * 0.33 * 100.0 / 9.0))).epsilon(1e-5));
    CHECK(fog_factor(400.0f, 100, 1000, 0.0f) == 1.0f); // density 0: no fog
    CHECK(fog_factor(100.5f, 100, 100, 1.0f) == doctest::Approx(std::exp(-25.0)).epsilon(1e-4)); // end = start + 1
}

TEST_CASE("Fog: ApplyWeatherFog's distances by camera region") {
    WeatherFogInput w;
    w.fog_colour = {300, 0, 40};
    w.zone_fog_colour = {10, 20, 30};
    w.zone_fog_min = std::bit_cast<std::uint32_t>(100.0f);
    w.zone_fog_max = std::bit_cast<std::uint32_t>(900.0f);
    w.density = std::bit_cast<std::uint32_t>(0.33f);
    w.camera_region = 5;
    auto a = apply_weather_fog(w);
    CHECK(std::bit_cast<float>(a.start) == 30.0f);
    CHECK(std::bit_cast<float>(a.end) == 800.0f);
    CHECK(a.red == 255); // clamped to 1..255
    CHECK(a.green == 1);
    CHECK(a.blue == 40);
    w.camera_region = 7;
    CHECK(std::bit_cast<float>(apply_weather_fog(w).end) == 50.0f);
    w.camera_region = 0;
    w.zone_type = 0; // indoor time type: the zone's own colour and clips
    a = apply_weather_fog(w);
    CHECK(a.red == 10);
    CHECK(std::bit_cast<float>(a.start) == 100.0f);
    w.zone_type = 2; // outdoor: the computed colour
    CHECK(apply_weather_fog(w).red == 255);
}

TEST_CASE("Fog: infravision's and ultravision's raise of the ambient over the plain floor") {
    SkyWeatherInput in;
    in.zone_type = 3; // indoors: 0.1 grey
    in.hour = 1;
    in.hour_ambient = game_hour_ambient();
    auto none = update_sky_weather_and_fog(mechanics::Fpu{}, in);
    CHECK(none.vision_lift == std::array<float, 3>{0, 0, 0});
    in.infravision_innate = 0;
    auto infra = update_sky_weather_and_fog(mechanics::Fpu{}, in);
    CHECK(infra.vision_lift[0] == doctest::Approx(65.0 / 255.0 - 0.1).epsilon(1e-4));
    CHECK(infra.vision_lift[1] == doctest::Approx(50.0 / 255.0 - 0.1).epsilon(1e-4));
    CHECK(infra.ambient != none.ambient);
    in.infravision_innate = 0xff;
    in.ultravision_effect = 1; // SPA 66 from a buff
    auto ultra = update_sky_weather_and_fog(mechanics::Fpu{}, in);
    CHECK(ultra.vision_lift[2] == doctest::Approx(120.0 / 255.0 - 0.1).epsilon(1e-4));
}

TEST_CASE("Fog: infravision's and ultravision's strength eases their floor toward the plain one") {
    SkyWeatherInput in;
    in.zone_type = 3; // indoors: 0.1 grey
    in.hour = 1;
    in.hour_ambient = game_hour_ambient();
    auto none = update_sky_weather_and_fog(mechanics::Fpu{}, in);
    in.infravision_innate = 0;
    auto full = update_sky_weather_and_fog(mechanics::Fpu{}, in);
    // Full strength is the original's, bit for bit.
    in.infravision_strength = 1.0;
    CHECK(update_sky_weather_and_fog(mechanics::Fpu{}, in).ambient == full.ambient);
    // Half: half way from the plain 0.08 floor to infravision's.
    in.infravision_strength = 0.5;
    auto half = update_sky_weather_and_fog(mechanics::Fpu{}, in);
    CHECK(half.vision_lift[0] == doctest::Approx(0.08 + 0.5 * (65.0 / 255.0 - 0.08) - 0.1).epsilon(1e-4));
    CHECK(half.vision_lift[0] < full.vision_lift[0]);
    // None: as without infravision.
    in.infravision_strength = 0.0;
    auto off = update_sky_weather_and_fog(mechanics::Fpu{}, in);
    CHECK(off.vision_lift == std::array<float, 3>{0, 0, 0});
    CHECK(off.ambient == none.ambient);
    CHECK(off.special_ambient == none.special_ambient);
    // Each has its own: ultravision ignores infravision's.
    in.infravision_innate = 0xff;
    in.ultravision_effect = 1;
    auto ultra = update_sky_weather_and_fog(mechanics::Fpu{}, in);
    CHECK(ultra.vision_lift[2] == doctest::Approx(120.0 / 255.0 - 0.1).epsilon(1e-4));
    in.ultravision_strength = 0.0;
    CHECK(update_sky_weather_and_fog(mechanics::Fpu{}, in).vision_lift == std::array<float, 3>{0, 0, 0});
}
