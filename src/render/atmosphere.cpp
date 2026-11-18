#include "render/atmosphere.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/glm.hpp>

#include "render/scene_lighting.h"

namespace hailstone::render {

namespace {

// shaders/atmosphere.frag's constants.
constexpr float kPlanet = 6360e3f;
constexpr float kTop = 6420e3f;
constexpr glm::vec3 kRayleigh{5.8e-6f, 13.5e-6f, 33.1e-6f};
// Ozone's absorption, carried with the Rayleigh density (shaders/atmosphere.frag kOzone): without it single
// scattering leaves the horizon and the twilight sky green-yellow.
constexpr glm::vec3 kOzone{2.0e-6f, 5.6e-6f, 0.25e-6f};
constexpr float kRayleighHeight = 8000.0f;
constexpr float kMie = 21e-6f;
constexpr float kMieHeight = 1200.0f;
constexpr float kMieG = 0.76f;
constexpr float kSun = 12.0f;
constexpr float kPi = std::numbers::pi_v<float>;

float exit_sphere(const glm::vec3& o, const glm::vec3& d, float r) {
    float b = glm::dot(o, d);
    float c = glm::dot(o, o) - r * r;
    return -b + std::sqrt(std::max(b * b - c, 0.0f));
}

bool hits_planet(const glm::vec3& o, const glm::vec3& d) {
    float b = glm::dot(o, d);
    float c = glm::dot(o, o) - kPlanet * kPlanet;
    return b < 0.0f && b * b - c > 0.0f;
}

float luminance(const glm::vec3& c) { return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f)); }

float smoothstep(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

glm::vec3 sky_radiance(const glm::vec3& dir_in, const glm::vec3& to_sun, float mie_scale) {
    glm::vec3 dir = glm::normalize(glm::vec3(dir_in.x, dir_in.y, std::max(dir_in.z, 0.002f)));
    glm::vec3 sun = glm::normalize(to_sun);
    glm::vec3 o(0.0f, 0.0f, kPlanet + 50.0f);
    float len = exit_sphere(o, dir, kTop);
    constexpr int kSteps = 16;
    constexpr int kLightSteps = 6;
    float seg = len / static_cast<float>(kSteps);
    float mu = glm::dot(dir, sun);
    float phase_r = 3.0f / (16.0f * kPi) * (1.0f + mu * mu);
    float g2 = kMieG * kMieG;
    float phase_m = 3.0f / (8.0f * kPi) * ((1.0f - g2) * (1.0f + mu * mu)) /
                    ((2.0f + g2) * std::pow(1.0f + g2 - 2.0f * kMieG * mu, 1.5f));
    glm::vec3 sum_r(0.0f), sum_m(0.0f);
    float depth_r = 0.0f, depth_m = 0.0f;
    for (int i = 0; i < kSteps; ++i) {
        glm::vec3 p = o + dir * (seg * (static_cast<float>(i) + 0.5f));
        float h = glm::length(p) - kPlanet;
        float hr = std::exp(-h / kRayleighHeight) * seg;
        float hm = std::exp(-h / kMieHeight) * seg;
        depth_r += hr;
        depth_m += hm;
        if (hits_planet(p, sun)) continue;
        float lseg = exit_sphere(p, sun, kTop) / static_cast<float>(kLightSteps);
        float light_r = 0.0f, light_m = 0.0f;
        for (int j = 0; j < kLightSteps; ++j) {
            glm::vec3 q = p + sun * (lseg * (static_cast<float>(j) + 0.5f));
            float hq = std::max(glm::length(q) - kPlanet, 0.0f);
            light_r += std::exp(-hq / kRayleighHeight) * lseg;
            light_m += std::exp(-hq / kMieHeight) * lseg;
        }
        glm::vec3 tau = (kRayleigh + kOzone) * (depth_r + light_r) + glm::vec3(kMie * mie_scale * 1.1f * (depth_m + light_m));
        glm::vec3 att = glm::exp(-tau);
        sum_r += att * hr;
        sum_m += att * hm;
    }
    return kSun * (sum_r * kRayleigh * phase_r + sum_m * (kMie * mie_scale * phase_m));
}

SkyLight sky_light(const glm::vec3& to_sun, float mie_scale) {
    thread_local glm::vec3 last_sun(2.0f);
    thread_local float last_mie = -1.0f;
    thread_local SkyLight last;
    // The day clock moves the sun a little every frame: a step of a thousandth (under a tenth of a degree) is
    // the same sky.
    const glm::vec3 key = glm::round(to_sun * 1000.0f);
    if (key == last_sun && mie_scale == last_mie) return last;
    SkyLight out;
    // The hemisphere in rings of zenith angle, eight azimuths each, weighted by cos x sin (cosine-weighted mean).
    constexpr int kAzimuths = 8;
    constexpr float kRings[] = {0.15f, 0.45f, 0.75f, 1.05f, 1.35f};
    float weight = 0.0f;
    glm::vec3 sum(0.0f);
    for (float theta : kRings) {
        float w = std::cos(theta) * std::sin(theta);
        for (int k = 0; k < kAzimuths; ++k) {
            float phi = (static_cast<float>(k) + 0.5f) * 2.0f * kPi / kAzimuths;
            glm::vec3 d(std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta));
            sum += sky_radiance(d, to_sun, mie_scale) * w;
            weight += w;
        }
    }
    out.ambient = sum / weight * kSkyAmbientScale;
    // A few degrees over the horizon, round it and towards the sun.
    constexpr float kHorizonZ = 0.05f;
    float r = std::sqrt(1.0f - kHorizonZ * kHorizonZ);
    glm::vec3 horizon(0.0f);
    for (int k = 0; k < kAzimuths; ++k) {
        float phi = (static_cast<float>(k) + 0.5f) * 2.0f * kPi / kAzimuths;
        horizon += sky_radiance(glm::vec3(r * std::cos(phi), r * std::sin(phi), kHorizonZ), to_sun, mie_scale);
    }
    out.horizon = horizon / static_cast<float>(kAzimuths);
    glm::vec2 flat(to_sun.x, to_sun.y);
    flat = glm::dot(flat, flat) > 1e-8f ? glm::normalize(flat) : glm::vec2(1.0f, 0.0f);
    out.horizon_sun = sky_radiance(glm::vec3(r * flat, kHorizonZ), to_sun, mie_scale);
    last_sun = key;
    last_mie = mie_scale;
    last = out;
    return out;
}

glm::vec3 linear_color(const glm::vec3& c) {
    auto linear = [](float v) {
        v = std::max(v, 0.0f);
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    return {linear(c.r), linear(c.g), linear(c.b)};
}

const HazeTarget& HazeBlend::update(const HazeTarget& target, float dt) {
    if (!valid_ || !(dt >= 0.0f)) {
        current_ = target;
        valid_ = true;
        return current_;
    }
    // About 95 % of the way in kHazeEaseSeconds.
    float k = 1.0f - std::exp(-std::min(dt, 1.0f) * 3.0f / kHazeEaseSeconds);
    current_.color = glm::mix(current_.color, target.color, k);
    current_.length = glm::mix(current_.length, target.length, k);
    current_.extend = glm::mix(current_.extend, target.extend, k);
    // Close enough: settle, so a steady haze is exactly its target.
    if (std::abs(current_.extend - target.extend) < 1e-3f) current_.extend = target.extend;
    if (std::abs(current_.length - target.length) < 0.5f) current_.length = target.length;
    return current_;
}

glm::vec3 haze_sky_color(const glm::vec3& fog, const glm::vec3& horizon, bool sky_drawn, float fog_end) {
    float share = (sky_drawn ? kHazeSkyShare : kHazeSkyShareNoSky) * smoothstep(400.0f, kClearFogEnd, fog_end);
    // No brighter than a few times the fog: a zone that keeps a dim fog by day gets no glare.
    glm::vec3 sky = horizon;
    float fl = luminance(fog), sl = luminance(sky);
    float most = 2.5f * fl + 0.02f;
    if (sl > most) sky *= most / sl;
    return glm::mix(fog, sky, share);
}

void weather_haze(HazeTarget& haze, float overcast, float snow) {
    overcast = std::clamp(overcast, 0.0f, 1.0f);
    snow = std::clamp(snow, 0.0f, 1.0f);
    haze.length *= 1.0f - overcast * (kStormHazeReach + 0.15f * snow);
    float l = luminance(haze.color);
    glm::vec3 rain_grey = glm::vec3(0.86f, 0.92f, 1.0f) * l * 0.75f;
    glm::vec3 snow_white = glm::vec3(0.97f, 0.99f, 1.04f) * l * 1.2f;
    haze.color = glm::mix(haze.color, glm::mix(rain_grey, snow_white, snow), 0.8f * overcast);
}

glm::vec3 lightning_haze(const glm::vec3& color, float flash, float overcast) {
    return color + glm::vec3(0.5f, 0.55f, 0.7f) * (std::max(flash, 0.0f) * 0.35f * std::max(overcast, 0.3f));
}

HazeTarget haze_target(const game::ViewDistance& original, float multiplier, bool extend, bool outdoor, bool sky_drawn,
                       const glm::vec3& to_sun, float overcast, float snow, bool in_the_sky) {
    HazeTarget out;
    const glm::vec3 fog = linear_color(original.fog_color);
    out.extend = extend ? 1.0f : 0.0f;
    out.length = modern_view(original, multiplier, extend).haze_length;
    out.color = in_the_sky ? sky_light(to_sun).horizon
              : outdoor    ? haze_sky_color(fog, sky_light(to_sun).horizon, sky_drawn, original.fog_end)
                           : fog;
    weather_haze(out, overcast, snow);
    return out;
}

glm::vec3 open_sky_below(const glm::vec3& to_sun) { return sky_radiance(glm::vec3(0.0f, 0.0f, 1.0f), to_sun); }

ColorGrade color_grade(const Daylight& d, bool outdoor, const glm::vec3& fog, float overcast) {
    ColorGrade g;
    glm::vec3 tint(1.0f);
    float saturation = 1.0f;
    if (outdoor) {
        float s = d.to_sun.z;
        // The low sun's warmth, either side of sunrise and sunset; the night's cool and its paler colour.
        float golden = std::exp(-((s - 0.06f) / 0.13f) * ((s - 0.06f) / 0.13f));
        float night = smoothstep(0.02f, -0.18f, s);
        float day = smoothstep(0.15f, 0.5f, s);
        tint = glm::mix(tint, glm::vec3(1.07f, 1.0f, 0.9f), golden * 0.8f);
        tint = glm::mix(tint, glm::vec3(0.9f, 0.97f, 1.1f), night);
        saturation = 1.0f + 0.05f * day + 0.06f * golden - 0.2f * night;
    } else {
        saturation = 0.94f;
    }
    // The zone's mood: a touch of its fog's hue.
    float fl = luminance(fog);
    if (fl > 1e-5f) {
        glm::vec3 hue = glm::clamp(fog / fl, glm::vec3(0.6f), glm::vec3(1.6f));
        tint *= glm::mix(glm::vec3(1.0f), hue, 0.07f);
    }
    // A storm: greyer and a little cooler.
    overcast = std::clamp(overcast, 0.0f, 1.0f);
    tint = glm::mix(tint, glm::vec3(0.96f, 0.99f, 1.04f), overcast * 0.6f);
    saturation *= 1.0f - 0.22f * overcast;
    g.tint = tint / std::max(luminance(tint), 1e-4f);
    g.saturation = saturation;
    return g;
}

} // namespace hailstone::render
