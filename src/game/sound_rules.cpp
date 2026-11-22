#include "game/sound_rules.h"

namespace hailstone::game {

namespace {

// PlayerClient::SetSounds' jump table: races that use another race's sounds.
std::int32_t sound_race(std::int32_t race) {
    switch (race) {
    case 44: case 55: case 67: case 71: return 1;
    case 77: return 6;
    case 78: return 3;
    case 81: return 11;
    case 90: return 2;
    case 92: return 9;
    case 93: return 10;
    case 94: return 8;
    case 106: return 5;
    case 112: return 4;
    default: return race;
    }
}

// The default each kind gets when racedata.txt has 0 (SetSounds' fourth
// argument): a constant, or (negative here) another kind's id.
constexpr std::int32_t kOf = -1000; // kOf - k: "the id of kind k"
constexpr std::array<std::int32_t, kSoundKinds> kDefaults = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,         // 0..17
    0x66, 0x65, 0x61, 0x62, 0x7e, 0x7c, 0x74, 0x82, 0x82, 0,      // 18..27
    kOf - 15, kOf - 15, 0x7a, kOf - 15, 0x80, 0x76, 0x76, 0x76,   // 28..35
    kOf - 15, 0x77, 0x77, 0x79,                                   // 36..39
    0,                                                            // 40: see below
    kOf - 3, kOf - 12, kOf - 14,                                  // 41..43
};

std::int32_t pick_swing(std::int32_t r) {
    switch (r) {
    case 1: return 0x7e;
    case 2: return 0x7a;
    case 3: return 0x82;
    case 4: return 0x81;
    case 5: return 0x76;
    default: return 0;
    }
}

} // namespace

SpawnSounds spawn_sounds(const std::unordered_map<std::int32_t, assets::RaceSoundIds>& races, std::int32_t race,
                         std::uint8_t gender) {
    const assets::RaceSoundIds* row = nullptr;
    if (auto it = races.find(sound_race(race)); it != races.end()) row = &it->second;
    // RaceManager_GetSoundID(race, gender, kind, default).
    auto get = [&](std::int64_t kind, std::int32_t fallback) {
        if (!row || kind < 0 || kind >= static_cast<std::int64_t>(kSoundKinds)) return fallback;
        std::int32_t v = gender == 1 ? row->female[static_cast<std::size_t>(kind)] : row->male[static_cast<std::size_t>(kind)];
        return v != 0 ? v : fallback;
    };
    SpawnSounds s;
    for (std::size_t k = 0; k < kSoundKinds; ++k) {
        std::int32_t d = kDefaults[k];
        if (d <= kOf) d = s.ids[static_cast<std::size_t>(kOf - d)];
        if (k == static_cast<std::size_t>(SoundKind::SecondaryPunch)) {
            // SetSounds passes the Punch id as the kind (and 0 as the default).
            s.ids[k] = get(s[SoundKind::Punch], 0);
            continue;
        }
        s.ids[k] = get(static_cast<std::int64_t>(k), d);
    }
    return s;
}

namespace {

// The door types that share a set of sounds, by the opening sound's wave id. A sound of 0 is none. `closing` plays
// when the door goes to state 3 and `closed` when it reaches 0; `opening` plays at state 2, and for some sets at
// state 3 too (before `closing`).
struct DoorSoundSet {
    std::int32_t opening, closing, closed;
    bool opening_at_3;
    std::uint8_t type_count;
    std::array<std::uint8_t, 16> types;
};
constexpr DoorSoundSet kDoorSounds[] = {
    {0x77, 0x77, 0, false, 4, {0x73, 0x74, 0x82, 0x87}},
    {0x87, 0x88, 0, false, 2, {0x00, 0x05}},
    {0x87, 0xbd, 0, false, 2, {0x23, 0x24}},
    {0xac, 0, 0, false, 1, {0x6e}},
    {0xad, 0, 0xae, true, 1, {0x1e}},
    {0xb0, 0xaf, 0, false, 2, {0x01, 0x06}},
    {0xb3, 0xb2, 0, false, 9, {0x02, 0x07, 0x08, 0x8c, 0x91, 0x92, 0x96, 0x97, 0x98}},
    {0xb4, 0, 0, true, 1, {0x28}},
    {0xb5, 0, 0xb6, true, 10, {0x3c, 0x3d, 0x41, 0x42, 0x46, 0x47, 0x4b, 0x4c, 0x52, 0x53}},
    {0xb8, 0xb7, 0, false, 16,
     {0x0a, 0x0b, 0x0c, 0x0f, 0x10, 0x11, 0x14, 0x15, 0x16, 0x19, 0x1a, 0x1b, 0x4e, 0x4f, 0x50, 0x51}},
    {0xb8, 0xb7, 0xb6, false, 5, {0x3e, 0x43, 0x48, 0x4d, 0x54}},
    {0xb9, 0, 0, true, 1, {0x3b}},
    {0xbb, 0, 0, false, 1, {0x78}},
    {0xbc, 0, 0, false, 1, {0x7d}},
};

const DoorSoundSet* door_sound_set(std::uint8_t type) {
    for (const DoorSoundSet& set : kDoorSounds)
        for (std::size_t i = 0; i < set.type_count; ++i)
            if (set.types[i] == type) return &set;
    return nullptr;
}

} // namespace

std::vector<std::int32_t> door_sounds(std::uint8_t type, std::uint8_t state) {
    std::vector<std::int32_t> out;
    const DoorSoundSet* set = door_sound_set(type);
    if (!set) return out;
    auto play = [&](std::int32_t wave) {
        if (wave) out.push_back(wave);
    };
    if (state == 2) {
        play(set->opening);
    } else if (state == 3) {
        if (set->opening_at_3) play(set->opening);
        play(set->closing);
    } else if (state == 0) {
        play(set->closed);
    }
    return out;
}

std::vector<std::int32_t> hit_sounds(const SpawnSounds& d, const HitInput& in, std::int32_t pick) {
    std::vector<std::int32_t> out;
    if (in.damage <= 0 || in.defender_corpse) return out;
    std::int32_t hit1 = d[SoundKind::Hit1];
    if (!in.environment && in.has_animation_set && hit1 > 0x1f && hit1 < 0x5a) {
        std::int32_t hp = in.defender_is_me ? in.defender_hp : (in.defender_level & 0xff) * 30;
        out.push_back(hp / in.damage < 10 ? 0x94 : 0x93);
    }
    switch (pick) {
    case 1: out.push_back(d[SoundKind::Hit1]); break;
    case 2: out.push_back(d[SoundKind::Hit2]); break;
    case 3: out.push_back(d[SoundKind::Hit3]); break;
    case 4: out.push_back(d[SoundKind::Hit4]); break;
    default: break;
    }
    std::erase(out, 0);
    return out;
}

std::int32_t block_sound(std::int32_t attacker_right, std::int32_t defender_right, std::int32_t defender_left,
                         std::int32_t coin) {
    std::int32_t s = 0x84;
    if (attacker_right == 2 && defender_right == 2) s = 0x78;
    if (defender_left == 3 && (coin & 1) == 0) return 0x7d;
    return s;
}

std::optional<std::int32_t> miss_sound(std::int32_t damage) {
    if (damage == -4) return 0x73;
    if (damage == -6) return 0x84;
    return std::nullopt;
}

std::int32_t held_type(std::uint32_t it) {
    // The byte table @0x489f30 for IT1..IT62 picks a case: 0 -> 2, 1 -> 4, 2 -> 5, 3 -> the general rule.
    static constexpr std::uint8_t kTable[62] = {0, 0, 0, 1, 0, 3, 0, 3, 0, 3, 3, 3, 3, 3, 3, 0, 0, 3, 0, 0, 3,
                                                3, 0, 0, 0, 3, 3, 3, 3, 0, 3, 3, 3, 0, 0, 3, 0, 3, 0, 0, 0, 0,
                                                0, 0, 3, 3, 3, 3, 3, 0, 0, 3, 0, 3, 3, 3, 0, 0, 0, 0, 2, 0};
    if (it == 10778) return 5;
    if (it >= 1 && it <= 62) {
        switch (kTable[it - 1]) {
        case 0: return 2;
        case 1: return 4;
        case 2: return 5;
        default: break;
        }
    }
    if (it < 1) return 0;
    if (it >= 200 && it <= 300) return 3;
    return 1;
}

bool is_movement_animation(std::int32_t action) {
    // OP_Animation numbers: walk, run, the two jumps, crouch walk, climb, swim, tread water.
    constexpr std::int32_t kWalk = 0x11, kRun = 0x12, kJump = 0x13, kJumpAcross = 0x14, kCrouchWalk = 0x16,
                           kClimb = 0x17, kSwim = 0x19, kTread = 0x25;
    switch (action) {
    case kWalk: case kRun: case kJump: case kJumpAcross: case kCrouchWalk: case kClimb: case kSwim: case kTread:
        return true;
    default: return false;
    }
}

AnimationSounds animation_sounds(const SpawnSounds& s, const AnimationSoundInput& in, AnimationSoundState& st,
                                 std::uint32_t now_ms, const RandomLowHigh& random) {
    AnimationSounds out;
    const std::int32_t a = in.action;
    if (a != st.animation) out.loop = s[SoundKind::Loop3d];
    st.animation = a;
    if (a == 0x20 || a == 0x47 || in.race == 0x7f) return out;

    enum class Go { None, Main, Swim, Tread, Timed };
    auto since = [&] { return now_ms - st.last_walk_ms; };
    Go go = Go::None;
    if (in.local) {
        if (a == 0x11 || a == 0x12 || a == 0x16 || a == 0x17) go = Go::Timed;
        else if (a == 0x25) {
            if (since() > 0x226) { st.last_walk_ms = now_ms; go = Go::Tread; }
        } else if (a != 0x19) go = Go::Main;
        else if (since() > 600) { st.last_walk_ms = now_ms; go = Go::Swim; }
    } else {
        if (a == 0x16 || a == 0x17) go = Go::Timed;
        else if (a == 0x25) {
            if (!in.legal_player_race) { if (s[SoundKind::Walk] != 0) go = Go::Tread; }
            else if (since() > 0x226) { st.last_walk_ms = now_ms; go = Go::Tread; }
        } else if (a != 0x19) go = Go::Main;
        else if (!in.legal_player_race) { if (s[SoundKind::Walk] != 0) go = Go::Swim; }
        else if (since() > 600) { st.last_walk_ms = now_ms; go = Go::Swim; }
    }
    if (go == Go::Timed) {
        if (since() > 500 && !in.sneaking) {
            st.last_walk_ms = now_ms;
            go = Go::Main;
        } else {
            return out;
        }
    }

    std::int32_t sound = 0;
    bool check = false; // CHECK: played only when above 0x1f
    auto steps = [&](const std::int32_t (&table)[4], SoundKind kind, std::int32_t first) {
        if (in.flying || in.sneaking) return;
        auto pick = [&] { std::int32_t r = random(0, 3); sound = r >= 0 && r <= 3 ? table[r] : first; };
        if (in.local) { pick(); return; }
        std::int32_t id = s[kind];
        if (id == 0) return;
        if (id == first) { pick(); return; }
        out.loop = id;
        out.loop_is_step = true;
    };
    // Animations 5, 8 and 10 (case 10 has no whip case and always picks a swing bare-handed).
    auto weapon = [&](bool five_way_when_empty_hand) {
        std::int32_t r = random(1, 3);
        if (r == 1 || in.local) sound = s[SoundKind::Attack1];
        if (!five_way_when_empty_hand && in.right_holding == 5) { sound = 0x96; return; }
        if (in.right_holding == 0) {
            if (five_way_when_empty_hand || sound == 0) sound = pick_swing(random(1, 5));
            else check = true;
            return;
        }
        out.play.push_back(0x76);
        if (sound == 0x82) sound = 0;
        else check = true;
    };

    switch (go) {
    case Go::Swim:
        if (in.legal_player_race) sound = 0x65;
        break;
    case Go::Tread:
        if (in.legal_player_race) sound = 0x66;
        break;
    case Go::Main: {
        static constexpr std::int32_t kRun[4] = {0x5f, 0x5e, 0x99, 0x9a};
        switch (a) {
        case 0x12: steps(kRun, SoundKind::Run, 0x5f); break;
        case 0x11: steps(kWalkSteps, SoundKind::Walk, kWalkSteps[0]); break;
        case 0x1a: out.loop = random(1, 100) < 0x32 ? s[SoundKind::Idle1] : s[SoundKind::Idle2]; break;
        case 0x13:
        case 0x14: sound = s[SoundKind::Jump]; check = true; break;
        case 0x16: if (!in.sneaking) { sound = s[SoundKind::Crouch]; check = true; } break;
        case 0x17:
            if (!in.sneaking) {
                sound = st.ladder_left ? 0x61 : 0x60;
                st.ladder_left = !st.ladder_left;
            }
            break;
        case 0x21: if (!in.sneaking) sound = in.race == 0x3c ? 0x1d0 : 0x62; break;
        case 0x24: if (!in.sneaking) sound = 0x62; break;
        case 0x0f: sound = s[SoundKind::Drown]; check = true; break;
        case 0x10:
            if (in.spawn_type == 0 ? (in.stand_state == 0x78 || in.hp > 0) : true) {
                sound = s[SoundKind::Death];
                check = true;
            }
            break;
        case 1: sound = 0x7e; break;
        case 2: sound = in.right_holding == 5 ? 0x96 : 0x7a; break;
        case 3: sound = in.right_holding == 5 ? 0x96 : 0x77; break;
        case 4: sound = 0x79; break;
        case 5:
            weapon(false);
            if (in.race == 0x31 || in.race == 0x7a) sound = s[SoundKind::Attack1];
            check = true;
            break;
        case 6:
            sound = in.left_holding == 5 ? 0x96 : in.left_holding == 3 ? 0x7c : in.left_holding != 0 ? 0x76 : 0x81;
            break;
        case 7: sound = in.left_holding == 3 ? 0x7c : 0x77; break;
        case 8: weapon(false); break;
        case 9: sound = 0x74; break;
        case 10: weapon(true); break;
        case 0x0b: sound = 0x80; break;
        case 0x2a: case 0x2b: case 0x2c: sound = s[SoundKind::Attack3]; check = true; break;
        case 0x2d: sound = 0x80; break;
        case 0x2e: case 0x2f: sound = 0x82; break;
        default: break;
        }
        break;
    }
    default: break;
    }
    if (check && sound <= 0x1f) sound = 0;
    if (sound != 0) out.play.push_back(sound);
    out.only_if_not_playing = in.local;
    return out;
}

std::int32_t container_sound(std::uint32_t bag_type, bool open) {
    // The open and the close table are the same bytes; their index is the bag
    // type less 6 and a type outside the 17 entries takes the shared sound.
    static constexpr std::array<std::uint8_t, kOwnBagTypeCount> kOwn = {0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0};
    const bool shared = bag_type < kFirstOwnBagType ||
                        bag_type - kFirstOwnBagType >= kOwn.size() || kOwn[bag_type - kFirstOwnBagType] != 0;
    if (shared) return kSoundContainer;
    return open ? kSoundBagOpen : kSoundBagClose;
}

std::vector<std::int32_t> landing_sounds(bool land_sound, std::optional<std::int32_t> fall_damage) {
    std::vector<std::int32_t> out;
    if (land_sound) out.push_back(kSoundJumpLand);
    if (fall_damage) out.push_back(*fall_damage > 0 ? kSoundFallHurt : kSoundJumpLand);
    return out;
}

SpellEffectStage spell_effect_stage(const SpellEffectStageInput& in) {
    SpellEffectStage r;
    if (!in.have_caster || !in.have_spell || !in.have_effect || (!in.have_old_record && !in.have_new_record) ||
        in.stage > 2)
        return r;
    std::int32_t sound = in.stage_sounds[in.stage];
    if (in.stage != 1 && sound != -1) {
        bool at_target = in.stage == 2;
        if (at_target ? in.have_target : in.have_caster) {
            r.sound = sound;
            r.sound_at_target = at_target;
        }
    }
    r.value = 1;
    r.filter_asked = true;
    if (in.particles_filtered) return r;
    if (in.have_new_record) {
        r.spawn_stage_emitters = 0.0f < in.particle_density && 0.0f < in.particle_opacity && in.spell_effects_option != 0;
        return r;
    }
    if (in.spell_effects_option != 0 || in.stage == 1) {
        r.do_particle_effect = true;
        return r;
    }
    r.dragon_breath_asked = true;
    r.do_particle_effect = in.dragon_breath == 1;
    return r;
}

std::array<std::int32_t, 3> old_spell_effect_sounds(std::span<const std::byte> record) {
    std::array<std::int32_t, 3> out{-1, -1, -1};
    for (std::size_t k = 0; k < 3; ++k) {
        std::size_t at = 0x224 + k * 0x3a0;
        if (at + 4 > record.size()) break;
        std::uint32_t v = 0;
        for (int b = 3; b >= 0; --b) v = (v << 8) | static_cast<std::uint8_t>(record[at + static_cast<std::size_t>(b)]);
        out[k] = static_cast<std::int32_t>(v);
    }
    return out;
}

} // namespace hailstone::game
