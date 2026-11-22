#pragma once

// Turns what happens in the zone into sounds (game/sound_rules.h picks the
// wave, audio/ plays it): the connection's sound cues (hits, level up, doors,
// casting), the spawns' animations (OP_Animation and their walking), and the
// local player's steps, jumps and landings.

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "assets/sound_files.h"
#include "audio/individual_sounds.h"
#include "audio/sound_control.h"
#include "game/sound_rules.h"
#include "game/world_state.h"

namespace hailstone {
class GameDir;
}
namespace hailstone::net {
class GameConnection;
struct SoundCue;
}
namespace hailstone::audio {
class AudioSystem;
}

namespace hailstone::app {

// Hailstone: the Individual Sounds volume group (audio/individual_sounds.h) of
//   - a spawn's footsteps: the local player's, a pet's (an NPC with an owner), anyone else's;
//   - a Wave cue of the connection: the coin wave, and the cue's context (a bag opening, an item put on or off);
// and the wave the Test button of a group plays.
audio::SoundGroup footstep_group(const game::Spawn& s, bool local);
audio::SoundGroup wave_cue_group(const net::SoundCue& cue);
// `pick` varies the footstep (the Test button counts up).
std::int32_t test_wave(audio::SoundGroup group, std::uint32_t pick = 0);

class SoundEvents {
public:
    SoundEvents(audio::AudioSystem& audio, const GameDir& game);

    // What the local player's body did this frame (app/player_movement.h's Frame).
    struct LocalMotion {
        bool moving = false, running = true, wet = false, flying = false, sneaking = false;
        bool jumped = false, land_sound = false;
        std::vector<std::int32_t> water_sounds; // the feet's region type changed (0x59ec30)
        std::optional<std::int32_t> fall_damage;
    };

    // Once a frame in the zone, after the connection's update.
    void update(net::GameConnection& conn, const LocalMotion& me, std::uint32_t now_ms);
    void reset(); // zone change

private:
    struct SpawnState {
        std::uint32_t action_seq = 0;
        bool seq_seen = false;
        game::AnimationSoundState anim;
        std::int32_t loop = 0;
        bool loop_is_step = false; // `loop` is the spawn's own walk sound: on its current footstep group's volume
        glm::vec3 last_position{0.0f};
        double last_time = 0.0;
    };
    // `volume_percent`: a volume option on top of the sound's own (audio::option_volume_factor).
    void play_at(const game::Spawn& s, std::int32_t wave, bool local, int volume_percent = audio::kFullVolumePercent);
    void animation(const game::Spawn& s, std::int32_t action, bool local, const LocalMotion* me, std::uint32_t now_ms,
                   SpawnState& st);
    // update()'s parts: one sound cue of the connection, and one spawn's sounds this frame.
    void play_cue(net::GameConnection& conn, const net::SoundCue& cue);
    void hit_cue(net::GameConnection& conn, const net::SoundCue& cue);
    void spell_cue(net::GameConnection& conn, const net::SoundCue& cue);
    void update_spawn(std::uint32_t id, const game::Spawn& s, bool local, const LocalMotion& me, const glm::vec3& listener,
                      std::uint32_t now_ms);
    void update_local(const game::Spawn& s, const LocalMotion& me, std::uint32_t now_ms, SpawnState& st);
    void update_remote(const game::Spawn& s, const glm::vec3& listener, std::uint32_t now_ms, SpawnState& st);

    audio::AudioSystem& audio_;
    std::vector<assets::SpellEffectSounds> spell_effects_;           // SpellsNew.eff
    std::vector<std::array<std::int32_t, 3>> old_effects_;           // spells.eff's stage sounds
    std::unordered_map<std::uint32_t, SpawnState> spawns_;
    std::uint32_t my_id_ = 0; // the local player's spawn id, this frame
    std::vector<std::uint32_t> alive_; // the spawns with a loop this frame (kept for its capacity)
    audio::CrtRand rand_{static_cast<std::uint32_t>(0x5eed)};
};

} // namespace hailstone::app
