#include "game/fog.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "assets/sky.h"

namespace hailstone::game {

namespace {

using mechanics::Fpu;
using mechanics::X87;

// .rdata constants of eqgame.exe (floats).
constexpr float kTwentieth = 0.05f;    // 0x9c7b20
constexpr float kThousandth = 0.001f;  // 0x9c4be8
constexpr float kFadeShare = 0.15f;    // 0x9c7b1c
constexpr float kTenth = 0.1f;         // 0x9c4a48
constexpr float kFifty = 50.0f;        // 0x9c42cc
constexpr float kHundredth = 0.01f;    // 0x9c4be4
constexpr float kSkyMinFog = 101.0f;   // 0x9c7b38
constexpr float k255 = 255.0f;         // 0x9c82e8
constexpr float kPer255 = 1.0f / 255.0f; // 0x9c7370 (0x3b808081)
constexpr float kPerMinute = 1.0f / 60.0f; // 0x9c82e4 (0x3c888889)
constexpr float kAmbientFloor = 0.08f; // 0x9c82e0
constexpr float kThird = 1.0f / 3.0f;  // 0x9c82dc (0x3eaaaaab)

// A float in memory loaded onto the FPU (a signalling NaN comes out
// quiet; the payload is kept).
double ld(std::uint32_t bits) {
    if ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu) != 0) bits |= 0x00400000u;
    return static_cast<double>(std::bit_cast<float>(bits));
}
double ld(float f) { return ld(std::bit_cast<std::uint32_t>(f)); }

// Stored as a float.
std::uint32_t st(double v) { return std::bit_cast<std::uint32_t>(static_cast<float>(v)); }

// A float copied through the FPU (quiet).
std::uint32_t quiet(std::uint32_t bits) { return st(ld(bits)); }

// Register-register x87 arithmetic (both operands on the FPU, so quiet).
struct Ops {
    const Fpu& f;
    double add(double a, double b) const { return mechanics::x87_add(f, X87(a), X87(b)); }
    double sub(double a, double b) const { return mechanics::x87_sub(f, X87(a), X87(b)); }
    double mul(double a, double b) const { return mechanics::x87_mul(f, X87(a), X87(b)); }
};

// The ordered compares (unordered: none of them).
bool less(double a, double b) { return a < b; }
bool greater(double a, double b) { return a > b; }
bool less_equal(double a, double b) { return a <= b; }

// An integer loaded (exact).
double fild(std::int32_t v) { return static_cast<double>(v); }

bool in_fog_region(std::uint8_t r) { return r >= 5 && r <= 10; }

// A fog distance as SetFog keeps it: the value on the FPU and the float it
// was last loaded from or stored to.
struct FogDistance {
    double value;
    std::uint32_t bits;
    static FogDistance from_bits(std::uint32_t b) { return {ld(b), b}; }
    void store(double v) {
        value = v;
        bits = st(v);
    }
    void reload() { value = ld(bits); }
};

double far_plane(const Ops& o, const FogClip& clip) {
    double min_clip = ld(clip.min_clip);
    return o.add(min_clip, o.mul(o.sub(ld(clip.max_clip), min_clip), o.mul(fild(clip.clip_plane), ld(kTwentieth))));
}

} // namespace

double calc_fog_fade_margin(const Fpu& fpu, std::uint32_t fog_start_bits, const FogClip& clip) {
    Ops o{fpu};
    double far = far_plane(o, clip);
    double diff = o.sub(far, ld(fog_start_bits));
    return o.mul(less(std::fabs(diff), ld(kThousandth)) ? far : diff, ld(kFadeShare));
}

SetFogResult set_fog(const Fpu& fpu, const SetFogInput& in) {
    Ops o{fpu};
    SetFogResult out;
    out.sky_flag = in.sky_flag;
    if (!in.world) return out;
    const bool adjust = !in_fog_region(in.camera_region);
    const double tenth = ld(kTenth), fifty = ld(kFifty), one = 1.0, zero = 0.0;
    FogDistance start = FogDistance::from_bits(in.start);
    FogDistance end = FogDistance::from_bits(in.end);
    // Both under a tenth: the fog runs from 50 short of the yon plane to 1 short of it.
    if (less(start.value, tenth) && less(end.value, tenth)) {
        start.store(o.sub(ld(in.yon), fifty));
        end.store(o.sub(ld(in.yon), one));
    }
    if (less(start.value, zero)) start.store(zero);
    if (less(end.value, start.value)) end.store(o.add(fifty, start.value));
    std::uint8_t r = in.red, g = in.green, b = in.blue;
    if (r == 0 && g == 0 && b == 0) r = g = b = 0xc0;
    if (!in.sky_option) {
        out.sky_flag = 0;
    } else {
        // SkyString2, or else zone_short_name2, non-empty, and the sky byte.
        bool name = in.sky_name || in.zone_name;
        out.sky_flag = name && in.zone_sky ? 1 : 0;
        if (in.offline) out.sky_flag = 1;
    }
    if ((in.on & 0xff) != 0) {
        double far = far_plane(o, in.clip);
        std::uint32_t far_bits = st(far);
        if (adjust && !in.raining) {
            const bool at_far_plane = less(std::fabs(o.sub(end.value, far)), ld(kHundredth)) || less(far, end.value);
            if (at_far_plane) {
                const double margin = calc_fog_fade_margin(fpu, start.bits, in.clip);
                end.store(o.sub(ld(far_bits), margin));
                start.reload();
            }
            if (less(start.value, zero)) start.store(zero);
            if (less_equal(end.value, start.value)) start.store(o.sub(end.value, one));
        }
        out.fog_on = static_cast<std::uint8_t>(in.on);
        if (adjust) {
            out.fog_start = st(start.value);
            out.fog_end = st(end.value);
            out.fog_density = quiet(in.density);
        }
        out.red = r;
        out.green = g;
        out.blue = b;
        if (greater(ld(kSkyMinFog), end.value) || in.camera_region >= 5) out.sky_flag = 0;
    }
    std::uint32_t colour = 0xff000000u | static_cast<std::uint32_t>(r) << 16 | static_cast<std::uint32_t>(g) << 8 | b;
    std::uint32_t density = quiet(in.density);
    if (in.sky) {
        if (in.sky->area_fog) {
            density = st(in.sky->area_fog_density);
            if (in.sky->area_fog_colour) colour = *in.sky->area_fog_colour;
        } else if (in.sky_option && out.sky_flag != 0 && in.camera_region < 5) {
            colour = (colour & 0xff000000u) | (in.sky->colour & 0xffffffu);
        }
        start.reload();
        end.reload();
    }
    RenderFog render;
    render.on = in.on;
    render.end = Fpu::ftol(end.value);
    render.start = Fpu::ftol(start.value);
    render.density = quiet(density);
    render.colour = colour;
    out.render = render;
    out.start = start.value;
    out.end = end.value;
    return out;
}

SkyWeatherResult update_sky_weather_and_fog(const Fpu& fpu, const SkyWeatherInput& in) {
    Ops o{fpu};
    SkyWeatherResult out;
    out.light_level = in.light_level;
    out.fog_colour = in.fog_colour;
    if (!in.world) return out;
    const std::uint8_t region = in.camera_region;
    if (in.lightning && region != 7 && region != 8) {
        out.ambient = 0xffffffffu;
        std::int32_t light = 0xff;
        if (in.actor_light) light = Fpu::ftol(o.mul(*in.actor_light, ld(k255)));
        std::array<std::uint8_t, 3> c{};
        for (int i = 0; i < 3; ++i)
            c[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(
                light > in.fog_colour[static_cast<std::size_t>(i)] ? light : in.fog_colour[static_cast<std::size_t>(i)]);
        out.set_fog_colour = c;
        return out;
    }
    bool infravision = false, ultravision = false;
    if (in.infravision_innate != 0xff) {
        infravision = true;
    } else {
        out.asked_infravision = true;
        if (in.infravision_effect != 0) infravision = true;
    }
    if (in.ultravision_innate != 0xff) {
        ultravision = true;
    } else {
        out.asked_ultravision = true;
        if (in.ultravision_effect != 0) ultravision = true;
    }
    if (ultravision) infravision = false;

    const double per255 = ld(kPer255), one = 1.0;
    // The ambient: values while they stay on the FPU (red, green) and the
    // float slots (ambient[]).
    double red, green;
    std::array<std::uint32_t, 3> ambient{};
    const std::uint8_t zone_type = in.zone_type;
    if (zone_type == 1 || zone_type == 2 || zone_type == 5) {
        double blue;
        if (in.sky_ambient) {
            std::uint32_t c = *in.sky_ambient;
            red = o.mul(fild(static_cast<std::int32_t>(c >> 16 & 0xff)), per255);
            ambient[0] = st(red);
            green = o.mul(fild(static_cast<std::int32_t>(c >> 8 & 0xff)), per255);
            ambient[1] = st(green);
            blue = o.mul(fild(static_cast<std::int32_t>(c & 0xff)), per255);
            ambient[2] = st(blue);
        } else {
            auto at = static_cast<std::uint8_t>(in.hour);
            auto next = static_cast<std::uint8_t>(at + 1);
            if (next > 0x18) next = 1;
            next = static_cast<std::uint8_t>(next - 1);
            auto now = static_cast<std::uint8_t>(at - 1);
            double minutes = fild(in.minute);
            double per_minute = ld(kPerMinute);
            auto entry = [&](int table, std::uint8_t index) {
                // The tables are 24 entries; the original reads past them for
                // an hour outside 1..24 (not reproduced: entry 0).
                return index < 24 ? ld(in.hour_ambient[static_cast<std::size_t>(table)][index]) : 0.0;
            };
            auto interpolate = [&](int table) {
                double from = entry(table, now);
                return o.add(o.mul(o.mul(o.sub(entry(table, next), from), minutes), per_minute), from);
            };
            // Each channel goes to its slot as soon as it is worked out.
            auto channel = [&](int table) {
                const double value = interpolate(table);
                ambient[static_cast<std::size_t>(table)] = st(value);
                return value;
            };
            red = channel(0);
            green = channel(1);
            blue = channel(2);
        }
        // Over 1, green and then red come down to 1 on the FPU and in their
        // slots; blue only in its slot.
        auto cap = [&](double& value, std::size_t slot) {
            if (!greater(value, one)) return;
            value = one;
            ambient[slot] = st(one);
        };
        cap(green, 1);
        cap(red, 0);
        if (greater(blue, one)) ambient[2] = st(one);
    } else {
        red = ld(zone_type == 4 ? 0.5f : kTenth);
        green = red;
        ambient[0] = ambient[1] = ambient[2] = st(red);
    }
    double light = one;
    if (in.actor_light) {
        light = *in.actor_light;
        green = ld(ambient[1]);
        red = ld(ambient[0]);
    }
    std::uint32_t slot = in.light_level;
    double level = ld(slot);
    if (level == -1.0) { // equal, not NaN
        level = light;
        slot = st(light);
    }
    if (less(level, light)) {
        level = o.add(level, ld(kHundredth));
        slot = st(level);
        if (greater(level, light)) {
            level = light;
            slot = st(light);
        }
    } else if (greater(level, light)) {
        level = o.sub(level, ld(kHundredth));
        slot = st(level);
        if (less(level, light)) {
            level = light;
            slot = st(light);
        }
    }
    out.light_level = slot;
    std::array<double, 3> floor{};
    if (infravision || ultravision) {
        const auto& f = infravision ? in.infravision_floor : in.ultravision_floor;
        for (std::size_t i = 0; i < 3; ++i) floor[i] = o.mul(fild(f[i]), per255);
        // Hailstone's vision strength: the floor eased toward the plain one (at full strength left as computed).
        const double strength = std::clamp(infravision ? in.infravision_strength : in.ultravision_strength, 0.0, 1.0);
        if (strength != 1.0) {
            const double plain = ld(kAmbientFloor);
            for (std::size_t i = 0; i < 3; ++i) floor[i] = o.add(plain, o.mul(o.sub(floor[i], plain), strength));
        }
    } else {
        floor[0] = floor[1] = floor[2] = ld(kAmbientFloor);
    }
    if (infravision || ultravision) {
        const double plain = ld(kAmbientFloor);
        const std::array<double, 3> raw{red, green, ld(ambient[2])};
        for (std::size_t i = 0; i < 3; ++i)
            out.vision_lift[i] = static_cast<float>(std::max(std::max(floor[i], raw[i]) - std::max(plain, raw[i]), 0.0));
    }
    if (less(red, floor[0])) ambient[0] = st(floor[0]);
    if (less(green, floor[1])) ambient[1] = st(floor[1]);
    if (greater(floor[2], ld(ambient[2]))) ambient[2] = st(floor[2]);
    double dark = o.sub(one, level);
    std::array<double, 3> special{};
    std::array<std::uint32_t, 3> special_bits{};
    for (std::size_t i = 0; i < 3; ++i) {
        special[i] = o.mul(floor[i], dark);
        special_bits[i] = st(special[i]);
    }
    const double full = ld(k255);
    // Stored as an integer with truncation (out of range: 0x80000000).
    auto chop = [](double v) { return static_cast<std::uint32_t>(Fpu::ftol(v)) & 0xff; };
    out.special_ambient = 0xff000000u | chop(o.mul(special[0], full)) << 16 | chop(o.mul(special[1], full)) << 8 |
                          chop(o.mul(special[2], full));
    out.ambient = 0xff000000u | chop(o.mul(ld(ambient[0]), full)) << 16 | chop(o.mul(ld(ambient[1]), full)) << 8 |
                  chop(o.mul(ld(ambient[2]), full));
    const double third = ld(kThird);
    std::uint32_t ambient_mean = st(o.mul(o.add(o.add(ld(ambient[1]), ld(ambient[0])), ld(ambient[2])), third));
    std::uint32_t special_mean =
        st(o.mul(o.add(o.add(ld(special_bits[1]), ld(special_bits[0])), ld(special_bits[2])), third));
    std::array<double, 3> scale{};
    switch (region) {
    case 5: scale = {240.0, 570.0, 370.0}; break;
    case 6: scale = {57.0, 57.0, 0.0}; break;
    case 7: scale = {2550.0, 0.0, 0.0}; break;
    case 8: scale = {0.0, 0.0, 125.0}; break;
    case 9: scale = {49.0, 52.0, 62.0}; break;
    case 10: scale = {179.0, 193.0, 227.0}; break;
    default:
        for (std::size_t i = 0; i < 3; ++i) scale[i] = fild(in.zone_fog_colour[i]); // the zone's fog colour
        break;
    }
    std::array<std::int32_t, 3> lit{}, dim{};
    for (std::size_t i = 0; i < 3; ++i) lit[i] = Fpu::ftol(o.mul(ld(ambient[i]), scale[i]));
    for (std::size_t i = 0; i < 3; ++i) dim[i] = Fpu::ftol(o.mul(ld(special_bits[i]), scale[i]));
    const double mean = ld(ambient_mean), level_now = ld(slot), mean_dark = ld(special_mean);
    for (std::size_t i = 0; i < 3; ++i) {
        std::int32_t a = Fpu::ftol(o.mul(o.mul(fild(lit[i]), mean), level_now));
        std::int32_t d = Fpu::ftol(o.mul(fild(dim[i]), mean_dark));
        out.fog_colour[i] = d > a ? d : a;
    }
    out.apply_weather_fog = true;
    out.ambient_mean = quiet(ambient_mean);
    return out;
}

std::array<std::array<std::uint32_t, 24>, 3> game_hour_ambient() {
    std::array<std::array<std::uint32_t, 24>, 3> t{};
    for (int c = 0; c < 3; ++c)
        for (int h = 0; h < 24; ++h)
            t[static_cast<std::size_t>(c)][static_cast<std::size_t>(h)] = std::bit_cast<std::uint32_t>(assets::sky_hour_ambient(c, h));
    return t;
}

SetFogArgs apply_weather_fog(const WeatherFogInput& in) {
    SetFogArgs out;
    std::int32_t red = in.fog_colour[0], green = in.fog_colour[1], blue = in.fog_colour[2];
    switch (in.camera_region) {
    case 5:
        out.start = quiet(std::bit_cast<std::uint32_t>(30.0f));  // 0x9c59f0
        out.end = quiet(std::bit_cast<std::uint32_t>(800.0f));   // 0x9d0cfc
        break;
    case 6: case 7: case 8:
        out.start = 0;
        out.end = quiet(std::bit_cast<std::uint32_t>(50.0f));    // 0x9c42cc
        break;
    case 9:
        out.start = quiet(std::bit_cast<std::uint32_t>(90.0f));  // 0x9c735c
        out.end = quiet(std::bit_cast<std::uint32_t>(100.0f));   // 0x9c3988
        break;
    case 10:
        out.start = 0;
        out.end = quiet(std::bit_cast<std::uint32_t>(400.0f));   // 0x9c7b40
        break;
    default:
        if (in.zone_type == 1 || in.zone_type == 2 || in.zone_type == 5) {
            if (in.raining) {
                out.start = quiet(in.display_start);
                out.end = quiet(in.display_end);
            } else {
                out.start = quiet(in.zone_fog_min);
                out.end = quiet(in.zone_fog_max);
            }
        } else {
            red = in.zone_fog_colour[0];
            green = in.zone_fog_colour[1];
            blue = in.zone_fog_colour[2];
            out.start = quiet(in.zone_fog_min);
            out.end = quiet(in.zone_fog_max);
        }
        break;
    }
    auto clamp = [](std::int32_t v) { return static_cast<std::uint8_t>(v < 1 ? 1 : v > 0xff ? 0xff : v); };
    out.red = clamp(red);
    out.green = clamp(green);
    out.blue = clamp(blue);
    out.density = quiet(in.density);
    out.on = in.fog_enabled ? 1 : 0;
    return out;
}

float fog_factor(float distance, std::int32_t start, std::int32_t end, float density) {
    if (end <= start) end = start + 1; // CRender::SetFog
    float range_inv = 1.0f / static_cast<float>(end - start);
    float f = std::clamp((distance - static_cast<float>(start)) * range_inv * kFogRange, 0.0f, kFogRange);
    float x = f * density;
    return std::exp(-(x * x));
}

} // namespace hailstone::game
