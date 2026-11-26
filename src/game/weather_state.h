#pragma once

// Rain and snow: the original's weather state (the 0x30-byte singleton at
// 0xe64438 that OP_Weather fills) and the graphics DLL's precipitation
// manager it drives. Source: the original's weather manager (the recorded
// vectors Weather_HandleWeatherMessage, Weather_StartEffect,
// Weather_RequestRainSnowToggle), CDisplay::LightningStrike,
// CDisplay::ApplyWeatherState and CDisplay::StopWeather.
//
//   Weather_HandleWeatherMessage @0x5a95e0   OP_Weather into the pending fields
//   Weather_StartEffect @0x5a9cf0            applying it
//   Weather_Update @0x5a9f00                 each frame
//   Weather_RequestRainSnowToggle @0x5a9490  the 0x7eb9 request
//   CDisplay::StopWeather @0x5a9bc0, CDisplay::StartWeather @0x5a99c0,
//   CDisplay::ApplyWeatherState @0x5a9700, CDisplay::LightningStrike @0x5a9610
//   CPrecipitationManager (EQGraphicsDX9.dll, vtable 0x1013aa18): +0xc 0x1007f360, update 0x10081860
//
// Kinds: 0 rain, 1 snow (ApplyWeatherState's sounds call it wind), 0xff none.
// The precipitation particles themselves are game/precipitation.h's.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "game/mechanics/fpu.h"
#include "game/precipitation.h"

namespace hailstone::game {

struct WeatherState {
    std::int32_t kind = 0xff;           // +0
    std::uint8_t pending = 0;           // +4
    std::uint8_t active = 0;            // +5: fading
    std::uint8_t on = 0;                // +6
    std::uint8_t raining = 0;           // +7: the rain sound on
    std::uint8_t windy = 0;             // +8: the wind sound on
    std::uint8_t lightning = 0;         // +9: a thunder this frame
    std::int32_t pending_kind = 0xff;   // +0xc
    std::int32_t pending_intensity = 0; // +0x10
    std::int32_t fade = 0;              // +0x14: the fade count (to 0x100)
    std::uint32_t next_strike = 0;      // +0x18
    std::uint32_t next_fade = 0;        // +0x1c
    std::uint8_t thunder = 0;           // +0x20: the second strike's sound, or 0
    std::uint32_t fog_near = 0;         // +0x24 (float bits)
    std::uint32_t fog_far = 0;          // +0x28 (float bits)
};

// The part of CDisplay the weather reads and writes.
struct WeatherDisplay {
    std::uint32_t frame_time = 0; // +0x154 (__EQGetTime at the frame's sky update)
    std::uint8_t raining = 0;     // +0x158 (bRain)
    std::uint8_t weather_on = 0;  // +0x159
};

// Weather_HandleWeatherMessage: kind and intensity become pending (+0xc,
// +0x10) and +4 is set; kind 0xff means the current kind with intensity 0.
void weather_handle_message(WeatherState& w, std::int32_t kind, std::int32_t intensity);

// The precipitation manager's virtuals Weather_StartEffect calls, in order.
class PrecipitationSink {
public:
    virtual ~PrecipitationSink() = default;
    virtual void create() = 0;                                  // resource manager +0x9c(7500), then +0x30("default")
    virtual void set_kind(std::int32_t kind) = 0;               // +0x04
    virtual void wind(const std::uint32_t (&vector)[3]) = 0;    // +0x18 (float bits)
    virtual std::uint32_t camera_60() = 0;                      // the camera's +0x60
    virtual void camera_value_1c(std::uint32_t value) = 0;      // +0x1c
    virtual std::uint32_t camera_64() = 0;                      // the camera's +0x64
    virtual void camera_value_28(std::uint32_t value) = 0;      // +0x28
    virtual void stop() = 0;                                    // +0x2c(1, 1)
    virtual void set_intensity(std::uint32_t target_bits, std::uint32_t time, std::uint32_t immediate) = 0; // +0xc
};

struct StartEffectEnv {
    bool has_manager = true;     // the state's +0x2c
    std::uint32_t now = 0;       // __EQGetTime, read for the last call
    // CDisplay::StopWeather(old kind, 0) for a new kind while another is on.
    std::function<void(std::int32_t kind)> stop_weather;
};

// Weather_StartEffect(kind, intensity). The target intensity x 0.01 is
// rounded at the FPU's precision (`fpu`), then stored as a float.
void weather_start_effect(const mechanics::Fpu& fpu, WeatherState& w, WeatherDisplay& display, PrecipitationSink& sink,
                          const StartEffectEnv& env, std::int32_t kind, std::int32_t intensity);

// The 0x7eb9 request (/rain and /snow; Weather_RequestRainSnowToggle).
struct RainSnowRequest {
    std::uint32_t rain = 0, snow = 0;
    std::int32_t on = 0;
    std::uint32_t hours = 0; // on ? 23 - the world's hour byte : 0
    std::int32_t kind = 0;
    std::uint32_t message = 0; // eqstr: 13186/13187 rain off/on, 13195/13196 snow off/on
};
inline constexpr std::uint16_t kRainSnowOpcode = 0x7eb9;
inline constexpr std::size_t kRainSnowSize = 22; // the original sends 22 bytes (20 used)
RainSnowRequest rain_snow_request(std::int32_t kind, std::int32_t on, std::uint8_t world_hour);

// ---------------------------------------------------------------------------
// The DLL's precipitation manager: the intensity p moves toward p* at 0.25 per
// second, and its particles (game/precipitation.h) fall.

class Precipitation : public PrecipitationSink {
public:
    // Drop the current weather while keeping the particle pool and its definitions.
    void reset();
    void create() override { created_ = true; }
    // +0x04 (0x1007f3b0).
    void set_kind(std::int32_t kind) override {
        kind_ = kind;
        field_.set_kind(kind);
    }
    // +0x18 (0x1007fcb0).
    void wind(const std::uint32_t (&vector)[3]) override;
    std::uint32_t camera_60() override { return 0; }
    void camera_value_1c(std::uint32_t) override {}
    std::uint32_t camera_64() override { return 0; }
    void camera_value_28(std::uint32_t) override {}
    // +0x2c (0x1007fa60) stop(1, 1): every particle back to the free list; the intensity is kept.
    void stop() override { field_.stop(true, true); }
    // +0xc (0x1007f360): p* = target and the update clock (+0x50) = time; with `immediate` p too. Then p below
    // 0.001 becomes 0 and above 1 becomes 1.
    void set_intensity(std::uint32_t target_bits, std::uint32_t time, std::uint32_t immediate) override;
    // +0 (0x10081860): p moves toward p* by 0.25 x the seconds since the last update (or set_intensity), without
    // overshooting.
    void update(std::uint32_t time);
    // The rest of +0 (0x10081860): the particles, over the same seconds. `frame`'s seconds and intensity are filled
    // in here.
    void update_particles(PrecipitationField::Frame frame);
    void discard_steps() { steps_.clear(); }

    bool created() const { return created_; }
    std::int32_t kind() const { return kind_; }
    float intensity() const { return current_; } // vtable +0x10, field +0x10
    float target() const { return target_; }
    // The seconds the last update covered.
    float seconds() const { return seconds_; }
    PrecipitationField& field() { return field_; }
    const PrecipitationField& field() const { return field_; }

private:
    bool created_ = false;
    std::int32_t kind_ = 0;
    float current_ = 0.0f;
    float target_ = 0.0f;
    float seconds_ = 0.0f;
    std::vector<float> steps_; // the updates' seconds since the last update_particles (the game clock's steps)
    std::optional<std::uint32_t> last_time_;
    PrecipitationField field_;
};

// ---------------------------------------------------------------------------
// The per-frame side (RealRender_World): what the weather asks the display,
// the sound manager and the chat for.

class WeatherEffects {
public:
    virtual ~WeatherEffects() = default;
    // CDisplay::SetFog(1, near, far, CDisplay +0x13c, +0x140..+0x142).
    virtual void set_fog(std::uint32_t near_bits, std::uint32_t far_bits) = 0;
    virtual float fog_near() = 0; // CDisplay +0x134
    virtual float fog_far() = 0;  // CDisplay +0x138
    // CDisplay_UpdateSkyWeatherAndFog(sky level) (StartWeather, StopWeather).
    virtual void update_sky_weather_and_fog() = 0;
    virtual void play_wave(std::int32_t id) = 0;          // EqSoundManager::WavePlay(id, 0)
    virtual void emitter_raining(bool on) = 0;            // EqSoundManager::EmitterSetRaining
    virtual void emitter_wind(bool on) = 0;               // EqSoundManager::EmitterSetWind
    virtual void chat(std::uint32_t eqstr) = 0;           // dsp_chat(StringTable::getString(id))
    virtual std::int32_t zone_id() = 0;                   // the local character's +0x108
    virtual std::int32_t random() = 0;                    // Random()
    virtual std::int32_t rand() = 0;                      // the CRT's rand()
};

// CDisplay::StopWeather(value, quiet).
void weather_stop(WeatherState& w, WeatherDisplay& display, WeatherEffects& fx, std::int32_t value, bool quiet);
// CDisplay::StartWeather: the sky and fog, then ApplyWeatherState while +5
// is set, else LightningStrike. RealRender_World runs it each frame in zone
// types 1, 2 and 5 while CDisplay +0x158 is set.
void weather_start(WeatherState& w, WeatherDisplay& display, WeatherEffects& fx);
// CDisplay::ApplyWeatherState: the fog fade and the start / stop messages and sounds.
void weather_apply_state(WeatherState& w, WeatherDisplay& display, WeatherEffects& fx);
// CDisplay::LightningStrike: rain only; a thunder (sound 0x8f or 0x90, +9
// set) when its time has come, the next one 10..20 s on, or with a 30 %
// chance within a second.
void weather_lightning_strike(WeatherState& w, const WeatherDisplay& display, WeatherEffects& fx);

// The CRT's rand() (MSVC: seed x 214013 + 2531011, bits 16..30), seeded with 1.
class MsvcRand {
public:
    std::int32_t next() {
        state_ = state_ * 214013u + 2531011u;
        return static_cast<std::int32_t>(state_ >> 16 & 0x7fff);
    }
private:
    std::uint32_t state_ = 1;
};

} // namespace hailstone::game
