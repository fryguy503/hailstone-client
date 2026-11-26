#pragma once

// The client's sound: the device, the mixer, the sound files, the zone's
// emitters and music, and the event sounds the game plays (EqSoundManager's
// role). Without a device (headless, --no-audio, settings) everything still
// runs and simply makes no sound.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "assets/xmi.h"
#include "audio/individual_sounds.h"
#include "audio/listener.h"
#include "audio/mixer.h"
#include "audio/music_manager.h"
#include "audio/sound_control.h"
#include "audio/zone_emitters.h"
#include "game/sound_rules.h"

namespace hailstone {
class Config;
class GameDir;
namespace platform {
class AudioDevice;
}
} // namespace hailstone

namespace hailstone::audio {

class SoundLibrary;
class SoundFont;

// hailstone.ini keys audio.*.
struct AudioSettings {
    bool enabled = true;        // audio.enabled: open the device at all
    int master = 80;            // audio.master, 0..100
    int music = 60;             // audio.music
    int effects = 80;           // audio.effects (event sounds and zone emitters)
    int npc_hits = 100;         // audio.npc_hits, 0..100: Hailstone, not in the original: the sounds of an NPC being
                                // hit, blocking or dodging, scaled on top of audio.effects (app/sound_events.cpp)
    // Hailstone, not in the original: audio.own_footsteps, pet_footsteps, other_footsteps, open_bag, open_bank, money,
    // thunder, equip and creature_ambient (audio/individual_sounds.h), each 0..100 on top of audio.effects
    // (app/sound_events.cpp).
    GroupVolumes individual;
    bool mute = false;          // audio.mute
    bool environment = true;    // audio.environment: the zone's ambient emitters (EmitterSetUserDisabled)
    std::string soundfont;      // audio.soundfont: SF2 for the XMI music; empty = <game>/synthusr.sf2
    // audio.realism, 0..100: the original's "Realism" option (eqclient.ini Realism 0..20, defaults.ini 1), which
    // EqSoundManager::WavePlay multiplies into each sound's reverb send (SetWaveVolume, +0xfec).
    int realism = 5;
    bool reverb = true;         // audio.reverb: the listener room's reverb at all (docs/unknowns.md)
    bool combat_music = true;   // audio.combat_music: the CombatMusic option (0xde0ae0)
    bool reverse_sound = false; // audio.reverse_sound: the left and right channels swapped (ReverseSound, 0xde0d48;
                                // /reversesound)
    int mix_ahead = 8;          // audio.mix_ahead, 1..100: SoundMixAhead (/mixahead), the output's buffer (mix_ahead_ms)
};
// The sound mixed ahead of the output for a SoundMixAhead (SoundManager::SetMixAhead @0x5bdc10: clamped to 4..96 and
// set as Miles' preference 0x2a, DIG_DS_MIX_FRAGMENT_CNT: that many DirectSound fragments of Miles' default 8 ms;
// docs/unknowns.md), in milliseconds; and in frames at `rate`.
int mix_ahead_ms(int mix_ahead);
std::uint32_t mix_ahead_frames(int mix_ahead, std::uint32_t rate);
AudioSettings load_audio_settings(const Config& config);
void store_audio_settings(const AudioSettings& s, Config& config);

class AudioSystem final : private EmitterSink {
public:
    // open_device = false never touches the audio subsystem (tests, --no-audio).
    AudioSystem(const GameDir& game, AudioSettings settings, bool open_device);
    ~AudioSystem() override;

    const AudioSettings& settings() const { return settings_; }
    void set_settings(const AudioSettings& s);
    bool has_device() const { return device_ != nullptr; }

    // SetCurrentZone: the zone's emitters and music; "" unloads.
    void set_zone(const std::string& short_name);
    const ZoneEmitters& zone() const { return zone_; }

    // The listener: the local player's position and the camera's angle in radians (audio/listener.h's
    // eq_listener_location); and the time of day: stored hour 1..24 (night outside 5..18).
    void set_listener(const glm::vec3& position, float angle);
    const glm::vec3& listener() const { return listener_; }
    void set_hour(int stored_hour);

    // The listener environment (CDisplay_UpdateListenerEnvironment: 1 outside, 2 high, 3 low): the emitters of
    // other environments stop and the room becomes the zone's preset for it, unless a special room (underwater
    // 0x16, stunned 0x17; 0x48a480) is set.
    void set_listener_environment(std::int32_t environment);
    void set_special_room(std::optional<std::int32_t> room);
    std::int32_t listener_environment() const { return environment_; }
    std::int32_t room_type() const;

    // The global rain and wind emitters (EqSoundManager::LoadGlobalEmitters @0x4a9df0: rainloop.wav, wave 159, and
    // wind_lp1.wav, 158; 2D, volume 0.5, looping, fading in over 2 s and out over 3 s): EmitterSetRaining /
    // EmitterSetWind from CDisplay::ApplyWeatherState and EmitterSetIndoors from CDisplay::SetupEmitterEnvironment
    // (every 500 ms: the player actor's scaled ambient below 0.48). UpdateEmitterStates plays each while its weather
    // is on and the listener is not indoors.
    void set_weather(bool raining, bool windy);
    void set_indoors(bool indoors);
    bool indoors() const { return indoors_; }
    const ZoneEmitters& weather_emitters() const { return weather_; }
    static constexpr std::int32_t kRainWave = 159, kWindWave = 158;

    // EqSoundManager::SetMusicSelection(id, on): a global music entry (audio/music_manager.h) on or off. Asked
    // before the global music is set up (global_music_ready), it is kept and applied then.
    void music_selection(std::int32_t id, bool on);
    // LoadGlobalMusic's entries are set: once the sound archives' index is built (update(), or set_zone, which
    // waits for it). Not in the constructor, where the checks for the files waited on the index's build.
    bool global_music_ready() const { return global_music_set_; }
    const MusicManager& music() const { return music_; }

    // Once a frame: emitters, music and the 3D voices.
    void update(std::uint32_t now_ms);

    // WavePlay(id, control): a wave of the sound table (the zone bank over soundassets.txt).
    void play_wave(std::int32_t id, const SoundControl& control);
    void play_wave_2d(std::int32_t id) { play_wave(id, SoundControl{}); }
    // A 2D wave at the volume of an individual sound (audio.<name>); SoundGroup::None plays it as it is. Also the
    // Test button of the Individual Sounds window, with the wave app::test_wave names.
    void play_group_2d(std::int32_t id, SoundGroup group);
    // A named sound of soundassets.txt ("YouveGotMail"), 2D: 0x4ab700's id, then WavePlay(id, 0). Nothing
    // when the name is not there.
    void play_named_2d(const std::string& name);
    // Hailstone: the same at an individual sound's volume: a named sound in any case, or any wave file
    // soundassets.txt lists (a named or a numbered entry's: "mail1.wav", "LevelUp.WAV"); false when the game folder
    // has no such sound (nothing played).
    bool play_named_group_2d(std::string_view name, SoundGroup group);
    // soundassets.txt's sound names, sorted.
    std::vector<std::string> named_sounds() const;
    // Every wave file soundassets.txt lists, once each in any case, sorted in any case; its "Unknown" entries left
    // out (the Beep sound's search).
    std::vector<std::string> sound_files() const;
    // Hailstone: a sine tone at an individual sound's volume, on the effects bus (make_tone; MQ's /beep with no
    // sound). The last tone's samples are kept for the next of the same kind.
    void play_tone_2d(double hz, std::uint32_t ms, SoundGroup group);
    bool wave_playing(std::int32_t id);

    // A spawn's sound ids (PlayerClient::SetSounds), cached by race and gender.
    const game::SpawnSounds& spawn_sounds(std::int32_t race, std::uint8_t gender);

    // A spawn's looping sound (EqMobileEmitter, SetCurrentLoopSound): `wave`
    // 0 = none. Call each frame with its position; drop_loops_except() removes the rest.
    // `volume_percent`: an individual sound's volume (a footstep loop), 100 as it is.
    void set_spawn_loop(std::uint32_t spawn_id, std::int32_t wave, const glm::vec3& position, float height,
                        int volume_percent = kFullVolumePercent);
    void drop_spawn_loops_except(const std::vector<std::uint32_t>& alive);

    // Without a device update() advances the mixer by the elapsed time; tools that render
    // the mixer themselves turn that off.
    void set_self_advance(bool on) { self_advance_ = on; }

    SoundLibrary& library() { return *library_; }
    Mixer& mixer() { return *mixer_; }
    std::size_t emitter_voices() const { return played_; }

private:
    // EmitterSink
    std::uint32_t play_wave(const std::string& file, const SoundControl& control) override;
    void release(std::uint32_t voice, std::int32_t fade_ms) override;
    bool alive(std::uint32_t voice) override;
    std::uint32_t play_music(const MusicEntry& entry, float volume) override;

    const std::string* wave_name(std::int32_t id) const;
    void apply_volumes();
    void apply_room();
    MilesLevels levels(const SoundControl& c) const;
    void place(std::uint32_t voice, const SoundControl& c);
    std::shared_ptr<SoundFont> font();
    // LoadGlobalMusic @0x4aa860 (waits for the sound archives' index), then the selections asked for before it.
    void set_up_global_music();

    const GameDir& game_;
    AudioSettings settings_;
    std::unique_ptr<SoundLibrary> library_;
    std::unique_ptr<Mixer> mixer_;
    std::unique_ptr<platform::AudioDevice> device_;

    std::unordered_map<std::int32_t, std::string> global_waves_;
    std::unordered_map<std::string, std::string> named_waves_; // soundassets.txt's "name^file" lines
    struct Tone {
        double hz = 0;
        std::uint32_t ms = 0;
        std::shared_ptr<const Pcm> pcm;
    } tone_; // play_tone_2d's last
    std::vector<std::string> mp3index_;
    std::unordered_map<std::int32_t, assets::RaceSoundIds> race_ids_;
    std::unordered_map<std::uint32_t, game::SpawnSounds> spawn_sounds_;

    std::string zone_name_;
    WaveTable waves_;
    ZoneEmitters zone_;
    ZoneEmitters weather_; // the rain (0) and wind (1) emitters
    bool raining_ = false, windy_ = false, indoors_ = false;
    void apply_weather();
    std::string zone_xmi_;
    std::unordered_map<std::string, std::vector<assets::XmiSequence>> xmi_; // .xmi files, parsed on first use
    // The global music entries' files: with the zone's, read ahead on the sound library's worker (play_music).
    std::vector<std::string> global_music_files_;
    bool global_music_set_ = false;
    std::int32_t global_music_rand_ = 0; // rand() after srand(FastTime()), at start as in the original
    std::vector<std::pair<std::int32_t, bool>> pending_selections_; // music_selection's, before set_up_global_music
    std::vector<std::string> global_wave_files_; // LoadGlobalWaves' (kept by SoundLibrary::drop_idle_waves)
    MusicManager music_;
    RoomPresets rooms_;
    std::vector<ZoneRoomPreset> zone_rooms_; // SetCurrentZone's table, from the player's eqgame.exe
    std::int32_t environment_ = kEnvironmentOutside;
    std::optional<std::int32_t> special_room_;
    std::shared_ptr<SoundFont> font_;
    bool font_tried_ = false;

    glm::vec3 listener_{0.0f};
    float yaw_ = 0.0f;
    bool night_ = false;
    struct Placed {
        std::uint32_t voice;
        SoundControl control;
    };
    std::vector<Placed> placed_;                         // 3D voices
    struct Pooled {
        std::uint32_t voice;
        bool positional;
        std::int32_t pool;
    };
    std::vector<Pooled> pooled_; // the voices holding a pool instance
    std::vector<std::pair<std::int32_t, std::uint32_t>> wave_voices_; // wave id -> voice (WaveIsPlaying)
    struct Loop {
        std::int32_t wave = 0;
        std::uint32_t voice = 0;
        glm::vec3 position{0.0f};
        float min = 3.0f, max = 72.0f;
        int volume_percent = kFullVolumePercent;
    };
    std::unordered_map<std::uint32_t, Loop> loops_;
    std::size_t played_ = 0;
    std::uint32_t last_update_ms_ = 0;
    bool self_advance_ = true;
    std::vector<float> scratch_; // mixing without a device
};

} // namespace hailstone::audio
