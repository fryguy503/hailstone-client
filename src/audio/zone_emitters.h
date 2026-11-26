#pragma once

// A zone's sound emitters and music, as the original's EqSoundManager keeps
// them (EqSoundManager, SoundEmitter, MusicManager):
//   - SetCurrentZone @0x4ab4c0 loads <zone>.emt (EmitterLoad/EmitterAdd) and,
//     only when that file cannot be opened, <zone>_sndbnk.eff + _sounds.eff
//     (LoadOldEmitters @0x4ab150 / CreateOldEmitter @0x4aac30);
//   - every frame SoundEmitter::GiveTime @0x5c0d80 starts, repeats and
//     releases each emitter's sound by the listener's distance and its timer;
//   - music emitters switch MusicManager entries on and off (audio/music_manager.h:
//     the active entry with the lowest priority value plays; a zone entry's is
//     int(range + 10000), so the global entries win), cross-fading over 1.5 s.
// No device here: sounds go to an EmitterSink, so all of it runs in tests.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "assets/sound_files.h"
#include "audio/music_manager.h"
#include "audio/sound_control.h"

namespace hailstone::audio {

// One SoundEmitter: the fields GiveTime reads.
struct Emitter {
    std::string wave;              // the wave's file (effect emitters)
    int music = -1;                // music entry index (music emitters), else -1
    glm::vec3 position{0.0f};      // +0x28..+0x30
    float gain = 1.0f;             // +0x08
    float echo = 0.0f;             // +0x0c
    std::int32_t fade_in_ms = 0;   // +0x18
    std::int32_t fade_out_ms = 0;  // +0x1c
    std::int32_t loop_count = 0;   // +0x20: 0 = continuous (the voice is kept), else one-shots
    float min_distance = 0.0f;     // +0x34
    float max_distance = 0.0f;     // +0x38
    float random_distance = 0.0f;  // +0x3c: > 0 plays this far around the listener at a random angle
    float range = 0.0f;            // +0x40: activation radius; <= 0 = 10,000,000
    std::int32_t delay_min = 0;    // +0x44
    std::int32_t delay_max = 0;    // +0x48
    std::int32_t environment = 0;  // +0x4c: 0 any, else the listener environment (1 outside, 2 high, 3 low)
    std::int32_t period = 0;       // +0x50: 0 always, 1 day, 2 night
    bool user_disableable = true;  // +0x54
    bool flag24 = false, flag25 = false; // +0x24 / +0x25 (EMT fields 20 / 21 for music)
    std::int32_t pool = 0;         // +0x180: the instance pool (4 for a zone's wave emitters)
    std::string source;            // "file:record" for diagnostics
};

// Wave ids of <zone>_sounds.eff: the zone bank (EMIT 1.., LOOP 162..) over the
// global soundassets.txt names.
struct WaveTable {
    std::map<std::int32_t, std::string> zone;
    const std::unordered_map<std::int32_t, std::string>* global = nullptr;
    const std::string* find(std::int32_t id) const;
};

// What GiveTime calls: start a sound (returns a handle, 0 = refused) and fade a kept one out.
class EmitterSink {
public:
    virtual ~EmitterSink() = default;
    virtual std::uint32_t play_wave(const std::string& file, const SoundControl& control) = 0;
    virtual void release(std::uint32_t voice, std::int32_t fade_ms) = 0;
    virtual bool alive(std::uint32_t voice) = 0;
    // Music: start `entry` at `volume` (if its file is not already playing), and fade the playing one.
    virtual std::uint32_t play_music(const MusicEntry& entry, float volume) = 0;
};

struct ZoneSoundSet {
    std::vector<Emitter> emitters;
    std::vector<MusicEntry> music;
};

// The emitters of a zone's EMT rows (EmitterAdd @0x4aa560). `exists` says
// whether a file can be loaded (GetAsset); rows whose file is missing make no emitter.
ZoneSoundSet emitters_from_emt(const std::vector<assets::EmtRow>& rows,
                               const std::function<bool(const std::string&)>& exists, const std::string& label = {});

// The emitters of a zone's _sounds.eff records (LoadOldEmitters / CreateOldEmitter).
// `zone_xmi`: the zone's "<zone>.xmi" when it exists (empty otherwise); `mp3index` the old music list.
ZoneSoundSet emitters_from_eff(const std::vector<assets::EffSoundRecord>& records, const WaveTable& waves,
                               const std::vector<std::string>& mp3index, const std::string& zone_xmi,
                               const std::function<bool(const std::string&)>& exists, const std::string& label = {});

// The base gain of a classic effect emitter from its level (CreateOldEmitter,
// 0x5bdb40): level <= 0 (wrapping negation) gives 0.2 for kind 0 and 1.0
// for kinds 2/3; 1..10000 gives 10^(-level/2000); larger levels are silent.
float classic_level_gain(std::int32_t level, std::uint8_t kind);

// The runtime: EqSoundManager's zone emitters and MusicManager.
class ZoneEmitters {
public:
    ZoneEmitters() = default;
    explicit ZoneEmitters(ZoneSoundSet set);

    // UpdateEmitterStates @0x4a94f0: enables each emitter by environment, time of day and the user switch.
    // night: EmitterSetNight (stored hour outside 5..18); environment: 1 outside, 2 high, 3 low.
    void set_state(bool night, std::int32_t environment, bool user_disabled);

    // One frame: SoundEmitter::GiveTime for every emitter. `effects_volume` is the
    // listener's emitter volume (+0x10), `music_volume` the MusicManager's (+0x08).
    void update(const glm::vec3& listener, std::uint32_t now_ms, float effects_volume, float music_volume,
                EmitterSink& sink);

    // Stops current and fading loops at once and removes the zone's music entries from the MusicManager.
    void clear(EmitterSink& sink);

    // The MusicManager the zone's music entries live in (the client's global one); without one the zone keeps its
    // own (tests). Entry ids are kZoneMusicId + index.
    void set_music_manager(MusicManager* music, EmitterSink& sink);
    MusicManager& music_manager() { return music_ ? *music_ : own_music_; }
    static constexpr std::int32_t kZoneMusicId = 0x10000;

    const std::vector<Emitter>& emitters() const { return set_.emitters; }
    const std::vector<MusicEntry>& music() const { return set_.music; }
    // The zone music entry playing (its index), or -1.
    int current_music() const;
    bool enabled(std::size_t i) const { return state_[i].enabled; }
    // SoundEmitter::Enable for one emitter (the global rain and wind emitters, which set_state does not cover).
    void set_enabled(std::size_t i, bool on) { state_[i].enabled = on; }

private:
    struct State {
        std::uint32_t anchor = 0;     // +0x160
        std::int32_t delay = 0;       // +0x164
        std::uint32_t voice = 0;      // +0x16c: the kept continuous voice
        bool enabled = true;          // +0x17c
        bool first = true;            // +0x26
    };
    void reset_timer(std::size_t i, std::uint32_t now_ms);

    ZoneSoundSet set_;
    std::vector<State> state_;
    std::vector<std::uint32_t> fading_loops_;
    MusicManager own_music_;
    MusicManager* music_ = nullptr;
    CrtRand rand_;
};

} // namespace hailstone::audio
