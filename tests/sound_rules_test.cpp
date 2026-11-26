#include <doctest/doctest.h>

#include <limits>

#include "game/mechanics/regions.h"
#include "game/sound_rules.h"
#include "game/spawn_states.h"

#include <map>

using namespace hailstone;
using namespace hailstone::game;

namespace {

std::unordered_map<std::int32_t, assets::RaceSoundIds> human() {
    // racedata.txt's race 1: male 0 0 0 32..40 3001 3002 98 130 129 0, female the same with 42..50.
    assets::RaceSoundIds r{};
    std::int32_t male[18] = {0, 0, 0, 32, 33, 34, 35, 36, 37, 38, 39, 40, 3001, 3002, 98, 130, 129, 0};
    std::int32_t female[18] = {0, 0, 0, 42, 43, 44, 45, 46, 47, 48, 49, 50, 3001, 3002, 98, 130, 129, 0};
    for (int i = 0; i < 18; ++i) {
        r.male[static_cast<std::size_t>(i)] = male[i];
        r.female[static_cast<std::size_t>(i)] = female[i];
    }
    return {{1, r}};
}

} // namespace

TEST_CASE("Spawn sounds: racedata ids, defaults and borrowed races") {
    auto races = human();
    SpawnSounds m = spawn_sounds(races, 1, 0);
    CHECK(m[SoundKind::Jump] == 32);
    CHECK(m[SoundKind::Hit1] == 33);
    CHECK(m[SoundKind::Death] == 39);
    CHECK(m[SoundKind::Walk] == 3001);
    CHECK(m[SoundKind::Attack1] == 130);
    CHECK(m[SoundKind::Swim] == 0x66);           // default constants
    CHECK(m[SoundKind::PrimaryStab] == 0x7a);
    CHECK(m[SoundKind::PrimaryBlunt] == 130);    // default: Attack1's id
    CHECK(m[SoundKind::JumpAcross] == 32);       // default: Jump's id
    CHECK(m[SoundKind::WalkBackwards] == 3001);  // default: Walk's id
    CHECK(m[SoundKind::CrouchWalk] == 98);
    CHECK(m[SoundKind::SecondaryPunch] == 0);    // the Punch id passed as a kind: out of range
    SpawnSounds f = spawn_sounds(races, 1, 1);
    CHECK(f[SoundKind::Jump] == 42);
    CHECK(spawn_sounds(races, 44, 0)[SoundKind::Jump] == 32); // race 44 uses race 1's sounds
    SpawnSounds none = spawn_sounds(races, 900, 0);
    CHECK(none[SoundKind::Jump] == 0);
    CHECK(none[SoundKind::Kick] == 0x7e);
}

TEST_CASE("Door sounds by switch type and state") {
    CHECK(door_sounds(0, 2) == std::vector<std::int32_t>{0x87}); // wooden door opens: DoorWd_O
    CHECK(door_sounds(0, 3) == std::vector<std::int32_t>{0x88}); // closes: DoorWd_C
    CHECK(door_sounds(0, 0).empty());
    CHECK(door_sounds(0x3c, 3) == std::vector<std::int32_t>{0xb5});
    CHECK(door_sounds(0x3e, 0) == std::vector<std::int32_t>{0xb6});
    CHECK(door_sounds(0x73, 3) == std::vector<std::int32_t>{0x77});
    CHECK(door_sounds(0x99, 2).empty());
}

TEST_CASE("Hit, block and miss sounds") {
    auto races = human();
    SpawnSounds h = spawn_sounds(races, 1, 0);
    HitInput in;
    in.damage = 10;
    in.defender_level = 10; // 300 / 10 = 30 -> FlshHit1
    CHECK(hit_sounds(h, in, 2) == std::vector<std::int32_t>{0x93, 34});
    in.damage = 40;         // 300 / 40 = 7 -> FlshHit2
    CHECK(hit_sounds(h, in, 1) == std::vector<std::int32_t>{0x94, 33});
    in.defender_is_me = true;
    in.defender_hp = 1000;  // 1000 / 40 = 25
    CHECK(hit_sounds(h, in, 4).front() == 0x93);
    in.environment = true; // an environmental hit: no FlshHit, the Hit1..4 alone
    CHECK(hit_sounds(h, in, 1) == std::vector<std::int32_t>{33});
    in.environment = false;
    in.defender_corpse = true;
    CHECK(hit_sounds(h, in, 1).empty());
    in.defender_corpse = false;
    in.damage = 0;
    CHECK(hit_sounds(h, in, 1).empty());
    CHECK(block_sound(0, 0, 0, 0) == 0x84);
    CHECK(block_sound(2, 2, 0, 1) == 0x78);
    CHECK(block_sound(2, 2, 3, 0) == 0x7d);
    CHECK(miss_sound(-4) == 0x73);
    CHECK_FALSE(miss_sound(-2).has_value());
}

TEST_CASE("Animation sounds: footsteps every 500 ms, jumps, attacks, loops of others") {
    auto races = human();
    SpawnSounds h = spawn_sounds(races, 1, 0);
    AnimationSoundState st;
    AnimationSoundInput in;
    in.local = true;
    in.race = 1;
    int calls = 0;
    auto rnd = [&](std::int32_t low, std::int32_t) { ++calls; return low; };

    in.action = 0x12; // run
    auto a = animation_sounds(h, in, st, 1000, rnd);
    REQUIRE(a.play.size() == 1);
    CHECK(a.play[0] == 0x5f); // StepRunR (random 0)
    CHECK(a.loop == 0);        // animation changed: back to the Loop3d sound
    CHECK(animation_sounds(h, in, st, 1400, rnd).play.empty()); // within 500 ms
    CHECK(animation_sounds(h, in, st, 1501, rnd).play.size() == 1);
    in.sneaking = true;
    CHECK(animation_sounds(h, in, st, 3000, rnd).play.empty());
    in.sneaking = false;

    in.action = 0x13; // jump
    a = animation_sounds(h, in, st, 4000, rnd);
    CHECK(a.play == std::vector<std::int32_t>{32});

    in.action = 5;    // one-handed weapon, holding something: Swing then Attack1
    in.right_holding = 1;
    a = animation_sounds(h, in, st, 5000, rnd);
    CHECK(a.play == std::vector<std::int32_t>{0x76}); // Attack1 is Punch1 (0x82), which the original drops here
    in.right_holding = 0; // bare hands: Attack1 (local always)
    a = animation_sounds(h, in, st, 5100, rnd);
    CHECK(a.play == std::vector<std::int32_t>{130});

    // Another spawn walking: its walk sound becomes its loop sound, nothing is played.
    AnimationSoundState other;
    AnimationSoundInput o;
    o.race = 1;
    o.action = 0x11;
    a = animation_sounds(h, o, other, 6000, rnd);
    CHECK(a.play.empty());
    CHECK(a.loop == 3001);
    CHECK(a.loop_is_step); // its walk loop is a footstep (the Individual Sounds volumes)

    // Race 0x7f makes no animation sounds.
    in.race = 0x7f;
    in.action = 4;
    CHECK(animation_sounds(h, in, st, 7000, rnd).play.empty());
}

TEST_CASE("Individual sounds: which animations are footsteps, and which loops") {
    // OP_Animation numbers: walk, run, the two jumps, crouch walk, climb, swim, tread water.
    constexpr std::int32_t kMovement[] = {0x11, 0x12, 0x13, 0x14, 0x16, 0x17, 0x19, 0x25};
    // Not movement: nothing, attacks, drown, death, idle, stand, sit, a kick.
    constexpr std::int32_t kOther[] = {0, 1, 5, 0x0f, 0x10, 0x1a, 0x20, 0x21, 0x2a};
    for (std::int32_t action : kMovement) CHECK(is_movement_animation(action));
    for (std::int32_t action : kOther) CHECK_FALSE(is_movement_animation(action));

    constexpr std::int32_t kHumanRace = 1, kIdle = 0x1a, kWalk = 0x11;
    constexpr std::uint32_t kFirstMs = 1000, kSecondMs = 2000, kThirdMs = 3000;
    auto races = human();
    SpawnSounds h = spawn_sounds(races, kHumanRace, 0);
    auto rnd = [](std::int32_t low, std::int32_t) { return low; };
    AnimationSoundState st;
    AnimationSoundInput in;
    in.race = kHumanRace;
    in.action = kIdle; // the loop is the ambient one, not a step
    CHECK_FALSE(animation_sounds(h, in, st, kFirstMs, rnd).loop_is_step);
    in.action = kWalk; // another spawn walking: its walk sound is the loop that replaces Loop3d
    AnimationSoundState walker;
    CHECK(animation_sounds(h, in, walker, kSecondMs, rnd).loop_is_step);
    in.local = true;   // the local player's steps are played, never looped
    AnimationSoundState local;
    CHECK_FALSE(animation_sounds(h, in, local, kThirdMs, rnd).loop_is_step);
}

TEST_CASE("Held item kinds (SetHeldModel's RightHolding)") {
    CHECK(held_type(0) == 0);
    CHECK(held_type(1) == 2);
    CHECK(held_type(4) == 4);
    CHECK(held_type(6) == 1);
    CHECK(held_type(61) == 5);
    CHECK(held_type(200) == 3);
    CHECK(held_type(301) == 1);
    CHECK(held_type(10778) == 5);
}

TEST_CASE("Landing sounds") {
    CHECK(landing_sounds(false, std::nullopt).empty());
    CHECK(landing_sounds(true, std::nullopt) == std::vector<std::int32_t>{kSoundJumpLand});
    CHECK(landing_sounds(false, 0) == std::vector<std::int32_t>{kSoundJumpLand});
    CHECK(landing_sounds(true, 25) == std::vector<std::int32_t>{kSoundJumpLand, kSoundFallHurt});
}

TEST_CASE("Container sounds: the bag's own sound or the shared container sound") {
    // The open (0x65cda8) and close (0x65d2d4) tables hold the same 17 answers for bag types 6..22.
    const std::uint8_t own[] = {0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0};
    for (std::uint32_t bag_type = 6; bag_type <= 22; ++bag_type) {
        const bool own_sound = own[bag_type - 6] == 0;
        CHECK(container_sound(bag_type, true) == (own_sound ? kSoundBagOpen : kSoundContainer));
        CHECK(container_sound(bag_type, false) == (own_sound ? kSoundBagClose : kSoundContainer));
    }
    // Outside the table: the shared sound, for open and close.
    CHECK(container_sound(0, true) == kSoundContainer);
    CHECK(container_sound(5, false) == kSoundContainer);
    CHECK(container_sound(23, true) == kSoundContainer);
    CHECK(container_sound(0xffffffffu, false) == kSoundContainer);
}

TEST_CASE("Feet wetness: entering and leaving a liquid region (0x59ec30)") {
    mechanics::FeetWetness w;
    CHECK(w.type() == 0);
    CHECK_FALSE(w.change(0, 0.0f));                    // no change: nothing
    CHECK_FALSE(w.change(0x10000000u, 0.0f));          // type 0 too: the high ice bit is ignored
    CHECK_FALSE(w.change(5, 0.5f));                    // into type 5, too slow
    CHECK(w.type() == 5);
    CHECK_FALSE(w.change(5, 3.0f));                    // the same type: nothing
    CHECK(w.change(6, 1.0f) == kSoundWaterIn);         // exactly 1 counts
    CHECK(w.change(8, -1.0f) == kSoundWaterIn);        // exactly -1 counts
    CHECK_FALSE(w.change(10, -0.99f));                 // just short of -1
    CHECK_FALSE(w.change(10, std::numeric_limits<float>::quiet_NaN())); // unchanged, NaN
    CHECK_FALSE(w.change(9, std::numeric_limits<float>::quiet_NaN()));  // into 9, NaN: neither compare holds
    CHECK(w.change(7, 0.0f) == kSoundWaterOut);        // type 7 always plays out
    CHECK_FALSE(w.change(7, 0.0f));
    CHECK(w.change(0, 0.0f) == kSoundWaterOut);        // out of the water
    CHECK_FALSE(w.change(0, 0.0f));
    CHECK_FALSE(w.change(3, 5.0f));                    // any other type: nothing
    CHECK(w.change(0, 0.0f) == kSoundWaterOut);        // 3 to 0 is still a step out
    // A move between two wet types asks the speed again.
    mechanics::FeetWetness v;
    CHECK_FALSE(v.change(6, 0.0f));
    CHECK(v.change(5, 2.0f) == kSoundWaterIn);
    CHECK(v.change(0, 0.0f) == kSoundWaterOut);
}

TEST_CASE("per-spawn sound state: a new spawn under an old id starts afresh, gone ids are dropped") {
    struct State {
        std::uint32_t action_seq = 0;
        bool seq_seen = false;
    };
    SpawnStates<State> states;
    State& rat = states.get(7, 100);
    rat.action_seq = 5;
    rat.seq_seen = true;
    CHECK(states.get(7, 100).seq_seen); // the same spawn keeps its state
    // Despawned and another spawned under id 7 within one frame: the size is the same, the state is not the rat's.
    const State& guard = states.get(7, 101);
    CHECK_FALSE(guard.seq_seen);
    CHECK(guard.action_seq == 0);
    CHECK(states.size() == 1);
    states.get(8, 1);
    std::map<std::uint32_t, int> alive{{8, 0}};
    states.prune(alive);
    CHECK(states.size() == 1);
    CHECK_FALSE(states.get(8, 1).seq_seen);
}
