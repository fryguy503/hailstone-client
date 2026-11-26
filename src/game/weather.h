#pragma once

// The weather the zone server sends (OP_Weather) and how the original plays it out, each frame of RealRender_World:
// in the open with bRain set CDisplay::StartWeather (ApplyWeatherState's fade in, 64 steps of 4, or fade out, 43
// steps of 6, which turns the rain / wind sound on or off on the frame after, else LightningStrike), then
// Weather_Update (a pending message starts, Weather_StartEffect, and the DLL's precipitation moves). The state
// machine is game/weather_state.h's port; this drives it from the app's clock. How it is drawn is the renderer's (render/weather_renderer.h).

#include <cstdint>
#include <functional>

#include "game/weather_state.h"

namespace hailstone::game {

enum class WeatherKind : std::int32_t { Rain = 0, Snow = 1, None = 0xff };

class Weather {
public:
    // Weather_HandleWeatherMessage: kept until the next update. Kind 0xff means the current kind at intensity 0.
    void handle_message(std::int32_t kind, std::int32_t intensity);

    // One frame at `now` (seconds, the display's frame time +0x154 in ms). ApplyWeatherState steps once a frame (its
    // next step time is the frame time - 1), so the fades count frames, as in the original. `outdoor`: zone time
    // types 1, 2 and 5, the only ones StartWeather runs in. `random` gives non-negative numbers (Random() and the
    // CRT's rand(); only their remainders are used). `zone_id`: the local character's zone (0x48 has its own rain
    // message).
    void update(double now, bool outdoor, const std::function<std::int32_t()>& random, std::int32_t zone_id = 0);

    // What falls (the precipitation manager's kind; None before any weather).
    WeatherKind kind() const;
    // How much is falling, 0..1: the server's intensity x 0.01 once faded in (the DLL's 0.25 a second).
    float precipitation() const { return precipitation_.intensity(); }
    // The sound manager's rain / wind emitter (EmitterSetRaining / EmitterSetWind): on once the fade in is done, off
    // once the fade out is (weather state +7 / +8; LightningStrike also turns the wind on in the snow).
    bool raining() const { return raining_; }
    bool windy() const { return windy_; }
    // The sky's StormWeather (any precipitation) rather than ClearWeather.
    bool storm() const { return precipitation() > 0.0f; }
    // A lightning strike this frame (+9; the original lights that one frame white).
    bool lightning() const { return state_.lightning != 0; }
    // The thunder of this frame's strike (0 none), played at once with WavePlay(id, 0): LightningStrike's thunder1
    // or thunder2 by Random(), and a second strike the other one.
    std::int32_t thunder() const { return thunder_; }
    // The eqstr ApplyWeatherState shows this frame when the weather has faded in (0 none): rain 0x21d (0x21c in zone
    // 0x48), snow 0x21e.
    std::uint32_t message() const { return message_; }

    // The falling drops and flakes (game/precipitation.h): the definitions (Resources/Precipitation/default.ini), and
    // once a frame after update() the particles moved over the same seconds. `frame`'s camera, heading, colours,
    // random and ground are the caller's.
    void set_precipitation_definitions(PrecipitationDefinitions defs) {
        precipitation_.field().set_definitions(std::move(defs));
    }
    void update_particles(const PrecipitationField::Frame& frame) { precipitation_.update_particles(frame); }
    // A skipped draw still advances weather but contributes no particle spawns to the next frame.
    void discard_particle_steps() { precipitation_.discard_steps(); }
    const PrecipitationField& particles() const { return precipitation_.field(); }
    // Leaving a zone drops its current and pending weather, keeping the shared particle definitions.
    void reset();

    static constexpr std::int32_t kThunder1 = 0x8f, kThunder2 = 0x90; // thunder1.wav, thunder2.wav

private:
    WeatherState state_;
    WeatherDisplay display_;
    Precipitation precipitation_;
    bool raining_ = false, windy_ = false;
    std::int32_t thunder_ = 0;
    std::uint32_t message_ = 0;
};

} // namespace hailstone::game
