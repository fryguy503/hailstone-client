#include "game/scene_fog.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace hailstone::game {

namespace {

std::uint32_t bits(float f) { return std::bit_cast<std::uint32_t>(f); }

std::uint8_t byte_of(float c) { return static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f)); }

} // namespace

void SceneFog::set_zone(const ZoneFogInput& zone, bool zone_sky) {
    zone_ = new_zone_fixes(zone);
    zone_sky_ = zone_sky;
    sky_flag_ = 1; // CDisplay_SetSkyEnabled / StartWorldDisplay
}

SceneFogFrame SceneFog::frame(const SceneFogInput& in) {
    if (in.steps < 0) return step(in);
    if (in.steps == 0) {
        SceneFog look = *this; // where the state is, unmoved
        return look.step(in);
    }
    SceneFogFrame out;
    for (int i = 0; i < in.steps; ++i) out = step(in);
    return out;
}

SceneFogFrame SceneFog::step(const SceneFogInput& in) {
    const mechanics::Fpu fpu; // the game's 24-bit precision
    SceneFogFrame out;
    const float far = zone_far_plane(zone_, in.clip_plane);
    const std::array<std::uint8_t, 3> zone_rgb{byte_of(zone_.fog_color.r), byte_of(zone_.fog_color.g),
                                               byte_of(zone_.fog_color.b)};

    // CDisplay_UpdateSkyWeatherAndFog.
    SkyWeatherInput w;
    w.lightning = in.lightning;
    w.camera_region = in.camera_region;
    w.zone_type = zone_.time_type;
    w.hour = std::clamp(in.hour, 1, 24);
    w.minute = in.minute;
    w.infravision_innate = in.infravision_innate ? 0 : 0xff;
    w.ultravision_innate = in.ultravision_innate ? 0 : 0xff;
    w.infravision_effect = in.infravision_effect;
    w.ultravision_effect = in.ultravision_effect;
    w.infravision_strength = static_cast<double>(in.infravision_strength);
    w.ultravision_strength = static_cast<double>(in.ultravision_strength);
    // The client has no CSky: the hour tables give the outdoor ambient.
    if (in.actor_light) w.actor_light = static_cast<double>(*in.actor_light);
    w.light_level = light_level_;
    w.zone_fog_colour = zone_rgb;
    w.fog_colour = fog_colour_;
    w.fog_start = display_start_;
    w.fog_end = display_end_;
    w.fog_density = display_density_;
    static const auto tables = game_hour_ambient();
    w.hour_ambient = tables;
    SkyWeatherResult sw = update_sky_weather_and_fog(fpu, w);
    light_level_ = sw.light_level;
    fog_colour_ = sw.fog_colour;
    out.light_level = std::bit_cast<float>(light_level_);
    out.ambient = sw.ambient.value_or(0xff000000u);
    out.special_ambient = sw.special_ambient.value_or(0xff000000u);
    out.vision_lift = glm::vec3(sw.vision_lift[0], sw.vision_lift[1], sw.vision_lift[2]);

    // Then CDisplay_ApplyWeatherFog's SetFog, or the lightning path's.
    SetFogInput s;
    if (sw.set_fog_colour) {
        s.on = 1;
        s.start = display_start_;
        s.end = display_end_;
        s.density = display_density_;
        s.red = (*sw.set_fog_colour)[0];
        s.green = (*sw.set_fog_colour)[1];
        s.blue = (*sw.set_fog_colour)[2];
    } else {
        WeatherFogInput a;
        a.camera_region = in.camera_region;
        a.fog_colour = fog_colour_;
        a.zone_type = zone_.time_type;
        a.raining = in.raining;
        a.display_start = display_start_;
        a.display_end = display_end_;
        a.zone_fog_colour = zone_rgb;
        a.zone_fog_min = bits(zone_.fog_minclip);
        a.zone_fog_max = bits(zone_.fog_maxclip);
        a.fog_enabled = zone_.fog_enabled;
        a.density = bits(zone_.fog_density);
        SetFogArgs args = apply_weather_fog(a);
        s.on = args.on;
        s.start = args.start;
        s.end = args.end;
        s.density = args.density;
        s.red = args.red;
        s.green = args.green;
        s.blue = args.blue;
    }
    s.yon = bits(far);
    s.raining = in.raining;
    s.sky_flag = sky_flag_;
    s.camera_region = in.camera_region;
    s.clip = {std::clamp(in.clip_plane, 0, 20), bits(zone_.minclip), bits(zone_.maxclip)};
    s.zone_sky = zone_sky_;
    SetFogResult r = set_fog(fpu, s);
    sky_flag_ = r.sky_flag;
    if (r.fog_start) display_start_ = *r.fog_start;
    if (r.fog_end) display_end_ = *r.fog_end;
    if (r.fog_density) display_density_ = *r.fog_density;
    out.sky = sky_flag_ != 0;
    out.view = render_view(*r.render, far);
    return out;
}

} // namespace hailstone::game
