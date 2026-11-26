#include "game/weather_state.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace hailstone::game {

namespace {

constexpr float kWindZ = -1.0f;          // 0x9c4ec4
constexpr float kPercent = 0.01f;        // 0x9c4be4
constexpr float kFogNear = 100.0f;       // 0x9c3988
constexpr float kFogFar = 1000.0f;       // 0x9c57f0
constexpr float kFogStep = 0.015625f;    // 0x9ce668
constexpr float kPerSecond = 0.25f;      // the precipitation manager's rate
constexpr std::int32_t kThunder = 0x8f;  // the thunder sounds 0x8f / 0x90

float f(std::uint32_t bits) { return std::bit_cast<float>(bits); }
std::uint32_t bits(float v) { return std::bit_cast<std::uint32_t>(v); }

// A float copied through the FPU: a signalling NaN comes out quiet.
std::uint32_t through_fpu(std::uint32_t b) {
    if ((b & 0x7f800000u) == 0x7f800000u && (b & 0x7fffffu)) b |= 0x400000u;
    return b;
}

// The fade count turned around (StopWeather, Weather_StartEffect's fade-out).
std::int32_t turned(std::int32_t fade) { return fade <= -1 || fade == 0x100 ? 0 : 0x100 - fade; }

// The global the thunder sound is kept in.
std::int32_t g_last_thunder = 0;

} // namespace

void weather_handle_message(WeatherState& w, std::int32_t kind, std::int32_t intensity) {
    w.pending_kind = kind;
    w.pending_intensity = intensity;
    w.pending = 1;
    if (kind == 0xff) {
        w.pending_kind = w.kind;
        w.pending_intensity = 0;
    }
}

void weather_start_effect(const mechanics::Fpu& fpu, WeatherState& w, WeatherDisplay& display, PrecipitationSink& sink,
                          const StartEffectEnv& env, std::int32_t kind, std::int32_t intensity) {
    const std::uint32_t wind[3] = {0, 0, bits(kWindZ)};
    if (!env.has_manager) sink.create(); // with the definition "default"
    sink.set_kind(kind);
    sink.wind(wind);
    sink.camera_value_1c(sink.camera_60());
    sink.camera_value_28(sink.camera_64());
    if (intensity == 0) {
        if (w.kind != 0xff) {
            std::int32_t fade = w.fade;
            w.kind = kind;
            w.active = 1; // the word +5 = 1
            w.on = 0;
            w.fade = turned(fade);
        }
    } else if (intensity > 0) {
        bool start = true;
        if (w.kind != 0xff) {
            if (w.kind == kind) {
                start = false;
            } else {
                if (env.stop_weather) env.stop_weather(w.kind);
                sink.stop();
            }
        }
        if (start) {
            w.kind = kind;
            display.raining = 1;
            w.fog_near = 0;
            w.fog_far = 0;
            w.active = 1; // the word +5 = 0x101
            w.on = 1;
            w.fade = 0;
            w.next_fade = display.frame_time;
            w.next_strike = display.frame_time;
            w.thunder = 0;
        }
    }
    // intensity * 0.01f, stored as a float.
    auto target = static_cast<float>(fpu.mul(static_cast<double>(intensity), static_cast<double>(kPercent)));
    sink.set_intensity(bits(target), env.now, 0);
}

RainSnowRequest rain_snow_request(std::int32_t kind, std::int32_t on, std::uint8_t world_hour) {
    RainSnowRequest r;
    bool snow = kind == 1;
    r.rain = snow ? 0 : 1;
    r.snow = snow ? 1 : 0;
    r.on = on;
    r.kind = kind;
    if (on == 0) {
        r.message = snow ? 13195 : 13186; // 0x338b / 0x3382
        r.hours = 0;
    } else {
        r.message = snow ? 13196 : 13187; // 0x338c / 0x3383
        r.hours = 0x17u - static_cast<std::uint32_t>(world_hour);
    }
    return r;
}

// ---------------------------------------------------------------------------

void Precipitation::reset() {
    set_kind(0xff);
    current_ = target_ = seconds_ = 0.0f;
    steps_.clear();
    last_time_.reset();
}

void Precipitation::wind(const std::uint32_t (&vector)[3]) {
    field_.set_wind(glm::vec3(f(vector[0]), f(vector[1]), f(vector[2])));
}

void Precipitation::set_intensity(std::uint32_t target_bits, std::uint32_t time, std::uint32_t immediate) {
    target_ = f(target_bits);
    last_time_ = time;
    if (immediate) current_ = target_;
    if (!(current_ >= 0.001f))
        current_ = 0.0f;
    else if (current_ > 1.0f)
        current_ = 1.0f;
}

void Precipitation::update(std::uint32_t time) {
    float seconds = last_time_ ? static_cast<float>(time - *last_time_) * 0.001f : 0.0f;
    last_time_ = time;
    seconds_ = seconds;
    if (steps_.size() < 64) steps_.push_back(seconds); // taken by update_particles (a frame's steps: at most 15)
    float step = kPerSecond * seconds;
    if (current_ < target_)
        current_ = std::min(target_, current_ + step);
    else if (current_ > target_)
        current_ = std::max(target_, current_ - step);
}

void Precipitation::update_particles(PrecipitationField::Frame frame) {
    frame.seconds = seconds_;
    if (frame.motion_seconds >= 0.0f) frame.spawn_steps = std::move(steps_);
    steps_.clear();
    frame.intensity = frame.eased_intensity >= 0.0f ? frame.eased_intensity : current_;
    field_.update(frame);
}

// ---------------------------------------------------------------------------

void weather_stop(WeatherState& w, WeatherDisplay& display, WeatherEffects& fx, std::int32_t value, bool quiet) {
    w.kind = value;
    std::int32_t fade = w.fade;
    w.active = 1; // the word +5 = 1
    w.on = 0;
    w.fade = turned(fade);
    if (quiet) return;
    fx.emitter_raining(false);
    fx.emitter_wind(false);
    w.fade = 0x100;
    fx.update_sky_weather_and_fog();
    if (w.active)
        weather_apply_state(w, display, fx);
    else
        weather_lightning_strike(w, display, fx);
}

void weather_start(WeatherState& w, WeatherDisplay& display, WeatherEffects& fx) {
    fx.update_sky_weather_and_fog();
    if (w.active)
        weather_apply_state(w, display, fx);
    else
        weather_lightning_strike(w, display, fx);
}

void weather_apply_state(WeatherState& w, WeatherDisplay& display, WeatherEffects& fx) {
    std::int32_t count = w.fade;
    if (count < 0x100) {
        if (display.frame_time < w.next_fade) return;
        w.next_fade = display.frame_time - 1;
        count += w.on ? 4 : 6;
        w.fade = count;
        if (w.on) {
            // The fog band moves in (FogBand, the original's x87 at 24 bits: float arithmetic).
            float near = fx.fog_near(), far = fx.fog_far();
            if (count == 4) {
                w.fog_near = bits(kFogNear < near ? (near - kFogNear) * kFogStep : 0.0f);
                w.fog_far = bits(kFogFar < far ? (far - kFogFar) * kFogStep : 0.0f);
            }
            auto band = [](float value, float limit, float offset) {
                if (!(limit < value)) return value; // not above the limit (or NaN): unchanged
                float moved = value - offset;
                return moved < limit ? limit : moved; // NaN keeps the moved value
            };
            float n = band(near, kFogNear, f(w.fog_near));
            float r = band(far, kFogFar, f(w.fog_far));
            fx.set_fog(bits(n), bits(r));
            return;
        }
        if (w.fade == 6) {
            w.fog_near = through_fpu(w.fog_near);
            w.fog_far = through_fpu(w.fog_far);
        }
        float far = fx.fog_far() + f(w.fog_far);
        float near = fx.fog_near() + f(w.fog_near);
        fx.set_fog(bits(near), bits(far));
        return;
    }
    std::int32_t kind = w.kind;
    if (w.on == 0) {
        display.weather_on = 0;
        display.raining = 0;
        if (kind == 0) {
            w.raining = 0;
            fx.emitter_raining(false);
            w.kind = 0xff;
        } else if (kind == 1) {
            w.windy = 0;
            fx.emitter_wind(false);
            w.kind = 0xff;
        }
        w.active = 0;
        return;
    }
    display.weather_on = 1;
    if (kind == 0) {
        fx.chat(fx.zone_id() == 0x48 ? 0x21c : 0x21d);
        w.raining = 1;
        fx.emitter_raining(true);
    } else if (kind == 1) {
        fx.chat(0x21e);
        w.windy = 1;
        fx.emitter_wind(true);
    }
    w.active = 0;
}

void weather_lightning_strike(WeatherState& w, const WeatherDisplay& display, WeatherEffects& fx) {
    std::uint32_t now = display.frame_time;
    if (now < w.next_strike) return;
    if (w.kind == 0) {
        std::int32_t sound;
        if (w.thunder)
            sound = kThunder + (w.thunder == kThunder ? 1 : 0);
        else
            sound = kThunder + (fx.random() % 100 + 1 > 0x32 ? 1 : 0);
        g_last_thunder = sound;
        fx.play_wave(sound);
        w.lightning = 1;
    } else if (w.kind == 1) {
        fx.emitter_wind(true);
    }
    if (w.thunder == 0) {
        std::int32_t r = fx.rand();
        w.thunder = r % 100 >= 0x1e ? 0 : static_cast<std::uint8_t>(g_last_thunder);
    } else {
        w.thunder = 0;
    }
    if (w.thunder == 0) {
        std::int32_t r = fx.rand() % 10000;
        w.next_strike = now + static_cast<std::uint32_t>(r >= 10000 ? r : r + 10000);
        return;
    }
    w.next_strike = now + static_cast<std::uint32_t>(fx.rand() % 1000);
}

} // namespace hailstone::game
