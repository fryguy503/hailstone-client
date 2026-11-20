#pragma once

// Which clip a character model plays each frame, as the original's two
// animation objects choose (game/mechanics/anim_select.h): new-style (EQG)
// models through PlayerAnimation_SelectMovementAnim or, still,
// PlayerAnimation_ChooseIdleAnimation, their clips found as 0x58afa0
// finds a sequence's track (new_style_track); classic models through
// 0x4afe60 with 0x4b15b0's stand-state substitutions and
// 0x4afc90's rate. The posture changes:
// sitting down plays P02 (STNG on new-style models) once, then P07 (NSIT)
// is held with O03 (SIDL) as the fidget; standing up plays P02 reversed
// (STDG on new-style models). Crouching plays the duck (L08, CRCH) once and
// standing up from it plays it backward (ChangePosition @0x5a0bc0), so a
// spawn ducks without moving.
//
// Pure: the renderer gives the spawn's state and which clips its model has,
// and gets the clip code, direction and rate.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

#include "game/animation_types.h"

namespace hailstone::game {

// StandState (+0x35c) as SpawnAnimInput::stand_state uses it.
inline constexpr std::uint8_t kStandStateStanding = 100, kStandStateSitting = 110, kStandStateDucking = 111;

struct SpawnAnimInput {
    glm::vec3 velocity{0.0f};   // units per tick (+0x70 / +0x74 / +0x78)
    float speed_run = 0.0f;     // +0x7c: signed forward speed
    float walk_speed = 0.0f;    // MyWalkSpeed +0x330
    float run_speed = 0.0f;     // +0x350
    std::uint8_t feet_wet = 0;  // +0xa1
    std::uint8_t body_wet = 0;  // +0xa2
    std::uint8_t stand_state = 100; // 100 stand, 110 sit, 111 duck
    bool npc = false;
    std::uint16_t race = 0;
    std::uint8_t gender = 0;
    bool new_style = false;     // a new-style (EQG, race table flag 0x08) model
    bool new_style_race = false; // NpcManager_IsNewStyleModelRace
    bool mount_race = false;
    float model_scale = 1.0f;   // SetHeights' +0x31c
    float anim_scale = 1.0f;    // SetHeights' +0x228 (OP_ChangeSize; 1.0)
    bool local_player = false;
    std::uint32_t now_ms = 0;   // the display clock (CDisplay +0x154)
    // New-style models only.
    std::uint32_t player_state = 0; // +0x14c PlayerState (0x20 stunned, 0x40 / 0x80 primary / secondary weapon)
    float speed_heading = 0.0f;     // +0x8c, heading units per tick
    bool turning = false;           // the update's argument (sub_00509340: turning)
    bool attacking = false;         // the global auto attack switch (0xe635aa)
    std::uint32_t owner_id = 0;     // +0x38c (a pet's master)
    bool first_person = false;      // the local player with camera type 0
    std::int32_t colliding_type = 0; // +0x24 CollidingType: 6 on a ladder (climbing)
    // Classic models only.
    std::uint8_t follow_up = 0x20;  // +0x10c4: the follow-up the stand request plays (0x20 none; 0x47 P08, or an
                                    // action the layers keep, render/actor_layers.h)
    bool mounted = false;
    // 0x4afc90's two flags. display_129 is +0x129 (eqclient.ini [Defaults] UseLuclinElementals): races
    // 209..212 play at twice the rate while it is set. new_model is the animation set's +0x34, which
    // InitAnimationTracks @0x407800 sets for an actor with a TUNIC_POINT dag (the Luclin player models, frogloks):
    // D05 (0x10) and a styled model's gender-1 animations 2 and 4 (race 2's 5) play at twice the rate.
    bool luclin_elementals = true;
    bool tunic_point = false;
};

struct SpawnAnimState {
    bool started = false;
    std::uint8_t anim = 0xfe;         // the animation playing (+0x10c0): old- or new-style code (0xfe none, as
                                      // EQPlayer's constructor sets it)
    std::uint32_t still_since = 0;    // +0x360
    std::uint8_t stand_state = 100;   // the last frame's
    bool posture = false;             // a sit-down / stand-up clip is playing
    std::string clip;                 // the clip playing
    std::string track;                // new-style models: the sequence and style playing ("STND_1H"), whose
                                      // variant is kept while it is asked again
    bool reverse = false;
    float rate = 1.0f;
    bool loop = true;
    std::int32_t column = 0;
};

struct SpawnAnimChoice {
    std::string clip;      // classic clip code ("L01") or an EQG tag the model keeps ("BWLK", "STDG")
    bool reverse = false;
    float rate = 1.0f;     // playback rate (1 = the clip's own frame time)
    bool loop = true;
    bool restart = false;  // a new clip (or the same one requested again from its start)
    float blend_ms = 100.0f; // the cross-fade into it
    std::uint8_t anim = 0;   // its animation number (classic) or sequence code (new-style)
    // Classic models: what +0x10c4 becomes once a stand request plays the follow-up (in.follow_up when it plays
    // none): 0x20, or 0x47 after an animation game::classic_plain_stand_after does not name.
    std::uint8_t follow_up = 0x20;
    std::int32_t column = 0; // new-style track's resolved style, after fallbacks
};

// `has_clip(code)`: the model has that clip. `clip_done`: the playing clip
// reached its end (a non-looping clip). `roll()`: RandomPercentRoll, 1..100;
// `rand()`: the C rand() a new-style track's variant takes (0 when empty).
SpawnAnimChoice choose_spawn_animation(SpawnAnimState& state, const SpawnAnimInput& in,
                                       const std::function<bool(std::string_view)>& has_clip, bool clip_done,
                                       const std::function<int()>& roll, const std::function<int()>& rand = {});

// What finding a new-style track reads.
struct NewStyleTrackInput {
    std::uint32_t player_state = 0; // +0x14c
    std::uint8_t current = 0;       // the animation playing (+0x10c0)
    std::int32_t forced_column = -1; // +0x324 (-1: none)
    bool mounted = false;
};
struct NewStyleTrack {
    std::string track; // "<tag>_<style>" ("SLPR_1H"), empty for none
    std::string clip;  // track + "_<variant>" ("SLPR_1H_2")
    std::uint8_t code = 0; // the sequence found (after fallbacks)
    std::int32_t column = 0;
};
// The clip a new-style code plays on an EQG model whose tracks are named
// "<tag>_<style>_<n>" (assets/eqg_character.h is_track_name), as
// 0x58afa0 finds it: 0x589770 follows the sequence fallbacks (SLSC ->
// SLPR -> STND) until a sequence has a variant in the column, column 1 or
// column 0; 0x588730's column is 0 (BA) without a primary weapon (0x40),
// 3 (DW) with a secondary too (0x80), else 2 (2H) for SL2H/BL2H, 4 (ST) for
// ST2H, 1 (1H) for the rest; 0x5885e0 plays STDA / IDLA for STND / IDLE
// while aggressive (0x4 / 0x8) or after an attack (+0x19 of the playing
// sequence); 0x588470 picks the variant at random among those the
// column has (rand() % count). A rider uses column 5 (MT), with the
// sequence's mount-fallback flag controlling whether BA is allowed.
NewStyleTrack new_style_track(std::uint8_t code, const NewStyleTrackInput& in,
                              const std::function<bool(std::string_view)>& has_clip,
                              const std::function<int()>& rand);
// A resolved EQG track or clip's style column; 0 for a classic/unknown name.
std::int32_t new_style_clip_column(std::string_view clip);
// The base style's column (BA): what a classic alias resolves to (assets/eqg_character.h classic_aliases), and the
// column of a sequence without a weapon style.
inline constexpr std::int32_t kBaseStyleColumn = 0;

// The clip an OP_Animation action (or any animation the client starts: an
// old-style number below 0x4f, else a new-style code) plays, or empty for
// none. Classic models: a new-style code goes to its old-style number (the
// map at 0xaaf428, 0x4b15b0), whose classic code a model without it
// replaces as 0x4b1210 does: the attacks 1..4 and 6..9 become 5 (C05),
// so EQEmu's NPC swings (8 hand to hand, 6 the off hand) play C05 on models
// that have only that; 11 (round kick) becomes 1, 10 (swim attack) swimming
// (P06). A replacement that is the stand (0x20), or that the model lacks too,
// is 0x4b15b0's stand request: nothing starts here, `stand_request` is set and
// the movement choice plays P01 / the follow-up. New-style models: an
// old-style number goes to its sequence (the map at 0xaaf5c8, the object's
// slot 0x14), found as new_style_track does.
// `playing` gets what becomes the animation playing (+0x10c0): the old-style
// number (after 0x4b1210's replacement), or the sequence found.
// The instrument songs' animations (0x27/0x28/0x29): drum, lute and horn. The singing songs use the ordinary cast
// animations (0x2a/0x2b/0x2c), which always play.
inline constexpr std::uint8_t kAnimDrum = 0x27, kAnimLute = 0x28, kAnimHorn = 0x29;
// The bard class (PlayerBase +0xeb8).
inline constexpr std::uint8_t kClassBard = 8;

// The classic/Luclin model block for an instrument song: a sitting spawn plays none of the three, and a bard plays
// none without an instrument in hand. The new-style (EQG) path has no such block, so `action_clip` applies this only
// after it has normalized a classic action.
bool instrument_song_animation(std::uint8_t anim, std::uint8_t class_id, std::uint8_t stand_state,
                               bool holds_instrument);

// What the classic instrument-song block reads from the spawn.
struct ActionGate {
    std::uint8_t class_id = 0;
    std::uint8_t stand_state = 0;
    bool holds_instrument = false;
};

std::string action_clip(std::uint8_t action, const std::function<bool(std::string_view)>& has_clip,
                        bool new_style = false, const NewStyleTrackInput& track_in = {},
                        const std::function<int()>& rand = {}, std::uint8_t* playing = nullptr,
                        bool* stand_request = nullptr, std::uint8_t* normalized = nullptr,
                        const ActionGate* gate = nullptr);

// 0x4b15b0's playback rate for an OP_Animation action on a classic model: the packet's speed / 10 through
// 0x4afc90, so a Luclin elemental's action and a styled model's D05 play at twice the rate. New-style models
// (0x58afa0) have no such call and keep the packet's rate.
float classic_action_rate(std::uint8_t anim, float speed, std::uint16_t race, std::uint8_t gender,
                          bool tunic_point, bool luclin_elementals);

// 0x4b15b0's stand path: P01, or P08 after an animation 0x4b0860 does not name (classic_plain_stand_after). A
// direct stand request from a sitting actor becomes the sit clip before the lookup (0x4b1917); without P07 nothing
// plays, and with it the sit clip keeps the packet's speed (it is not the stand fallback); an actor that is not
// standing plays nothing for an idle request (0x4b1b97). `normalized` is the
// request after the new-to-old map (a new-style STND is P01 too). A model without P08 plays P01's clip and code,
// and one without P01 too plays nothing (an empty clip).
struct ClassicStand {
    std::string clip;   // "P01", "P07" or "P08", empty for none
    std::uint8_t anim = static_cast<std::uint8_t>(AnimationType::Stand);
    bool sit = false;   // the sitting conversion: the packet's speed, not the stand's 1.0
};
// What else 0x4b15b0's stand path reads: the packet's own number (0x4b1b97's idle-request test reads it before the
// new-to-old map; nullopt: the normalized one), the race (0x4b194b: race 0x3c never takes the sit clip) and whether
// this is the local player in first person (0x4b195b: neither).
struct ClassicStandContext {
    std::optional<std::uint8_t> requested;
    std::uint16_t race = 0;
    bool local_first_person = false;
};
inline constexpr std::uint16_t kRaceNoSitConversion = 0x3c;
// 0x4b1ba0's idle requests: P01, P03, P08, O01, O02 and 0xc..0xf. An actor that is not standing plays nothing
// for one of them unless it became the sit clip.
bool classic_idle_request(std::uint8_t requested);
ClassicStand classic_stand_request(std::uint8_t normalized, std::uint8_t playing, std::uint8_t stand_state,
                                   const std::function<bool(std::string_view)>& has_clip,
                                   const ClassicStandContext& ctx = {});

// 0x4b1a7e: after a stand request plays the follow-up, +0x10c4 becomes P01 or P08 from the animation playing
// (+0x10c0), never the action just played.
std::uint8_t classic_follow_up_after(std::uint8_t playing);

} // namespace hailstone::game
