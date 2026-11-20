// Per-frame clip choice for character models (game/spawn_animation.h).
// Sources: the original's movement animation choice, 0x4afe60,
// 0x4b15b0's stand-state substitutions and 0x4b1210's replacements; the
// old-style object's slot 0x38 (0x4b07e0: may an idle start from this
// animation) read from the exe; PlayerAnimation_ChooseIdleAnimation and
// 0x589770, 0x588730, 0x5885e0, 0x588470, 0x58afa0 for the new-style
// tracks.
#include "game/spawn_animation.h"

#include <array>
#include <optional>

#include "assets/eqg_character.h"
#include "game/anim_layers.h"
#include "game/animation_types.h"
#include "game/mechanics/anim_select.h"

namespace hailstone::game {

namespace mech = hailstone::game::mechanics;

namespace {

// The old-style object's slot 0x38 (0x4b07e0): true for 0x20, 0x26, 0x47, 0x4a.
bool old_can_idle(std::int32_t anim) { return anim == 0x20 || anim == 0x26 || anim == 0x47 || anim == 0x4a; }

// 0x4b1210's fixed replacement for an old-style animation the model lacks, by animation (0x20, the idle, for any
// not listed).
constexpr std::array<std::uint8_t, 0x50> kOldReplacement = {
    0x20, 0x05, 0x05, 0x05, 0x05, 0x20, 0x05, 0x05, 0x05, 0x05, 0x25, 0x01, 0x0d, 0x0c, 0x0d, 0x20, // 0x00..
    0x20, 0x12, 0x11, 0x20, 0x20, 0x25, 0x20, 0x20, 0x20, 0x25, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, // 0x10..
    0x20, 0x20, 0x20, 0x20, 0x20, 0x11, 0x20, 0x28, 0x20, 0x20, 0x2b, 0x20, 0x2b, 0x20, 0x20, 0x20, // 0x20..
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, // 0x30..
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x1a, 0x20, 0x20, 0x11, 0x12, 0x20, 0x4a, 0x20, // 0x40..
};
constexpr std::uint8_t num(AnimationType a) { return static_cast<std::uint8_t>(a); }

AnimationType old_replacement(AnimationType a) {
    return num(a) < kOldReplacement.size() ? static_cast<AnimationType>(kOldReplacement[num(a)]) : AnimationType::Stand;
}

constexpr std::uint32_t kAggressive = 0x4, kForcedAggressive = 0x8, kPrimaryEquipped = 0x40,
                        kSecondaryEquipped = 0x80; // 0x9d77a4 / 0x9d77a8 / 0x9d77b4 / 0x9d77b8

// A sequence's track name in a column ("SLPR" + "_1H").
std::string track_of(std::uint8_t code, int column) {
    return std::string(mech::new_style_tag(code)) + "_" + std::string(assets::eqg_character::kTrackStyles[column]);
}

// The player's animation table as 0x58afa0 reads it: a cell is variant k (0..3) of a column. One track choice asks
// for the same cells and rows several times; each answer is kept for the call (0 not asked, 1 no, 2 yes), so a clip
// name is built and looked up once. The answers are the same as asking again: the model does not change mid-call.
struct Rows {
    static constexpr std::uint8_t kFirst = 0x50, kLast = 0xb6; // the codes new_style_track walks
    static constexpr std::size_t kCodes = kLast - kFirst + 1;
    const std::function<bool(std::string_view)>& has;
    mutable std::array<std::uint8_t, kCodes * 6 * 4> cells{};
    mutable std::array<std::uint8_t, kCodes> rows{};
    static bool kept(std::uint8_t code) { return code >= kFirst && code <= kLast; }
    bool look(std::uint8_t code, int column, int k) const {
        return has(track_of(code, column) + "_" + std::to_string(k + 1));
    }
    bool cell(std::uint8_t code, int column, int k) const {
        if (!kept(code)) return look(code, column, k);
        std::uint8_t& m = cells[(static_cast<std::size_t>(code - kFirst) * 6 + static_cast<std::size_t>(column)) * 4 +
                                static_cast<std::size_t>(k)];
        if (m == 0) m = static_cast<std::uint8_t>(look(code, column, k) ? 2 : 1);
        return m == 2;
    }
    bool row(std::uint8_t code) const {
        auto scan = [&] {
            for (int c = 0; c < 6; ++c)
                for (int k = 0; k < 4; ++k)
                    if (cell(code, c, k)) return true;
            return false;
        };
        if (!kept(code)) return scan();
        std::uint8_t& m = rows[static_cast<std::size_t>(code - kFirst)];
        if (m == 0) m = static_cast<std::uint8_t>(scan() ? 2 : 1);
        return m == 2;
    }
};

// 0x588730: a rider's mount column, otherwise the equipped-weapon style.
int column_for(std::uint8_t code, const NewStyleTrackInput& in) {
    if (in.mounted) return 5;
    if (!(in.player_state & kPrimaryEquipped)) return 0;
    if (in.player_state & kSecondaryEquipped) return 3;
    int column = code == 0x56 || code == 0x57 ? 2 : code == 0x58 ? 4 : 1;
    return in.forced_column >= 0 && in.forced_column < 6 ? in.forced_column : column;
}

// 0x5885e0: STDA / IDLA for STND / IDLE while aggressive or after an attack (the playing sequence's
// +0x19 with CEverQuest +0x5c8, the game state, in game: not 1 or 2). CDisplay +0x12c's 0x7f4100 on the
// player's +0xfd0 / +0xfbc is taken as false.
std::uint8_t alternate_row(std::uint8_t code, int column, const NewStyleTrackInput& in, const Rows& rows) {
    const bool flagged = mech::new_style_attack_flag(mech::old_to_new_anim(in.current));
    const bool in_state = (in.player_state & kAggressive) || (in.player_state & kForcedAggressive);
    if (!in_state && !flagged) return code;
    std::uint8_t alt;
    if (code == mech::kAnimStand) alt = 0x70;
    else if (code == mech::kAnimIdle) alt = 0x71;
    else return code;
    if (!rows.row(alt)) return code;
    return rows.cell(alt, column, 0) || rows.cell(alt, 0, 0) ? alt : code;
}

} // namespace

NewStyleTrack new_style_track(std::uint8_t code, const NewStyleTrackInput& in,
                              const std::function<bool(std::string_view)>& has_clip,
                              const std::function<int()>& rand) {
    // 0x589770: the row and column, following the sequence fallbacks.
    const Rows rows{has_clip};
    std::uint8_t track = code;
    int column = 0;
    bool found = false;
    for (int guard = 0; guard < 0x68 && !found; ++guard) {
        if (track <= 0x4f || track > 0xb6) return {};
        if (rows.row(track)) {
            int col = column_for(track, in);
            std::uint8_t alt = alternate_row(track, col, in, rows);
            if (rows.row(alt) && (alt == track || rows.cell(alt, col, 0))) track = alt;
            if (rows.cell(track, col, 0)) {
                column = col;
                found = true;
                break;
            }
            bool try_zero = true;
            if (col == 5) {
                try_zero = mech::new_style_mount_fallback_flag(track);
            } else if (col != 0 && rows.cell(track, 1, 0)) {
                column = 1;
                found = true;
                break;
            }
            if (try_zero && rows.cell(track, 0, 0)) {
                column = 0;
                found = true;
                break;
            }
        }
        track = mech::new_style_fallback(track);
    }
    if (!found) return {};
    // 0x588470 with extra 0xff: rand() % (1 + the column's variants 2..4 present).
    int count = 1;
    for (int k = 1; k <= 3; ++k)
        if (rows.cell(track, column, k)) ++count;
    int variant = count == 1 ? 0 : (rand ? rand() : 0) % count;
    NewStyleTrack out;
    out.code = track;
    out.column = column;
    out.track = track_of(track, column);
    out.clip = out.track + "_" + std::to_string(variant + 1);
    if (!has_clip(out.clip)) out.clip = out.track + "_1"; // the variants are taken as numbered from 1 without gaps
    return out;
}

std::int32_t new_style_clip_column(std::string_view clip) {
    const auto split = clip.find('_');
    if (split == std::string_view::npos) return 0;
    auto style = clip.substr(split + 1);
    style = style.substr(0, style.find('_'));
    for (std::int32_t column = 0; column < 6; ++column)
        if (style == assets::eqg_character::kTrackStyles[column]) return column;
    return 0;
}

namespace {

} // namespace

SpawnAnimChoice choose_spawn_animation(SpawnAnimState& st, const SpawnAnimInput& in,
                                       const std::function<bool(std::string_view)>& has_clip, bool clip_done,
                                       const std::function<int()>& roll, const std::function<int()>& rand) {
    const mech::Fpu fpu(mech::FpuPrecision::Single24);
    SpawnAnimChoice keep{st.clip, st.reverse, st.rate, st.loop, false, 100.0f, st.anim, in.follow_up};
    keep.column = st.column;
    auto start = [&](const std::string& clip, std::uint8_t anim, bool reverse, float rate, bool loop,
                     float blend_ms) {
        // The same animation asked again does not restart it (0x4b0900); a
        // one-shot clip then holds its last frame.
        bool same = st.started && clip == st.clip && reverse == st.reverse;
        SpawnAnimChoice c{clip, reverse, rate, loop, !same, blend_ms, anim, in.follow_up};
        st.started = true;
        st.track.clear();
        st.column = 0;
        st.anim = anim;
        st.clip = clip;
        st.reverse = reverse;
        st.rate = rate;
        st.loop = loop;
        return c;
    };

    // A new-style code on its track (new_style_track); the same sequence and column asked again keeps its
    // variant (0x58ac90's mode 3: nothing changes).
    NewStyleTrackInput track_in{in.player_state, st.anim, -1, in.mounted};
    auto start_new = [&](std::uint8_t code, bool reverse, float rate, bool loop) -> std::optional<SpawnAnimChoice> {
        NewStyleTrack t = new_style_track(code, track_in, has_clip, rand);
        if (t.clip.empty()) return std::nullopt;
        // 0x58afa0: BWLK found on WALK's row plays it backward.
        if (code == mech::kAnimBackWalk && t.code == mech::kAnimWalk) reverse = true;
        // The layers store the sequence found (Base + row) as the animation playing (+0x10c0).
        const float blend = mech::new_style_blend_ms(st.anim, t.code);
        std::string clip = st.started && t.track == st.track ? st.clip : t.clip;
        SpawnAnimChoice c = start(clip, t.code, reverse, rate, loop, blend);
        st.track = t.track;
        st.column = c.column = t.column;
        return c;
    };

    // Posture changes: sitting down / standing up.
    if (st.started && in.stand_state != st.stand_state) {
        const std::uint8_t from = st.stand_state;
        st.stand_state = in.stand_state;
        if (in.stand_state == 110 && from == 100) {
            if (in.new_style) {
                if (auto c = start_new(mech::kAnimSitDown, false, 1.0f, false)) {
                    st.posture = true;
                    return *c;
                }
            } else if (has_clip("P02")) {
                st.posture = true;
                return start("P02", num(AnimationType::SitStand), false, 1.0f, false, 100.0f);
            }
        } else if (in.stand_state == 100 && from == 110) {
            // 0x58afa0: STNG asked reversed plays STDG forward.
            if (in.new_style) {
                if (auto c = start_new(mech::kAnimStandUp, false, 1.0f, false)) {
                    st.posture = true;
                    return *c;
                }
            } else if (has_clip("P02")) {
                st.posture = true;
                return start("P02", num(AnimationType::SitStand), true, 1.0f, false, 100.0f);
            }
        } else if (in.stand_state == 111 || (in.stand_state == 100 && from == 111)) {
            // PlayerZoneClient::ChangePosition @0x5a0bc0: crouching plays the duck (0x18) once, unless it or the
            // duck walk (0x16) is playing; standing up from a crouch plays it backward. A plain Play, not a held
            // posture: the choice below takes over when there is something else to play (a walk, a jump), and
            // otherwise leaves it (a still ducking spawn asks nothing, nor does a still one after the duck).
            const bool down = in.stand_state == 111;
            if (in.new_style) {
                const std::uint8_t duck = mech::old_to_new_anim(num(AnimationType::Duck));
                const std::uint8_t walk = mech::old_to_new_anim(num(AnimationType::DuckWalk));
                if (!down || (st.anim != duck && st.anim != walk))
                    if (auto c = start_new(duck, !down, 1.0f, false)) return *c;
            } else if (has_clip(mech::old_anim_clip(num(AnimationType::Duck))) &&
                       (!down || (st.anim != num(AnimationType::Duck) && st.anim != num(AnimationType::DuckWalk)))) {
                return start(std::string(mech::old_anim_clip(num(AnimationType::Duck))), num(AnimationType::Duck),
                             !down, 1.0f, false, 100.0f);
            }
        }
    }
    st.stand_state = in.stand_state;
    if (st.posture) {
        if (!clip_done) return keep;
        st.posture = false;
    }

    // The original's speeds: +0x70 SpeedY, +0x74 SpeedX (only their squares
    // and the horizontal stillness matter here), +0x78 SpeedZ.
    const float vy = in.velocity.y, vx = in.velocity.x, vz = in.velocity.z;

    if (in.new_style) {
        // 0x58af10: a still player (and no mount) goes to ChooseIdleAnimation, a moving one to
        // SelectMovementAnim (which clears +0x360).
        mech::AnimStart r;
        if (!in.mounted && mech::movement_still(vy, vx, vz, fpu)) {
            mech::IdleAnimInput idle;
            idle.turning = in.turning;
            idle.player_state = in.player_state;
            idle.display_time = in.now_ms;
            idle.still_since = st.still_since;
            idle.stand_state = static_cast<std::uint8_t>(in.stand_state == 111 ? 0x6f : in.stand_state);
            idle.body_wet = in.body_wet;
            idle.animation = st.anim;
            idle.speed_heading = in.speed_heading;
            idle.owner_38c = in.owner_id;
            idle.attacking = in.attacking;
            idle.local_first_person = in.local_player && in.first_person;
            idle.random_percent = roll;
            // +0x1229 (a nonzero value stops the fidget) and the spell book's slot 0x94 stay at their defaults:
            // their origin is not confirmed (docs/unknowns.md "Animation fields not traced").
            mech::IdleAnimResult ir = mech::choose_idle_animation(idle);
            if (ir.set_still_since) st.still_since = in.now_ms;
            r = ir.start;
        } else {
            mech::MovementAnimInput m;
            m.mounted = in.mounted;
            m.speed_y = vy;
            m.speed_x = vx;
            m.speed_z = vz;
            m.speed_run = in.speed_run;
            m.colliding_type = in.colliding_type;
            m.feet_wet = in.feet_wet;
            m.body_wet = in.body_wet;
            m.spawn_type = in.npc ? 1 : 0;
            m.stand_state = static_cast<std::uint8_t>(in.stand_state == 111 ? 0x6f : in.stand_state);
            m.animation = st.anim;
            m.walk_speed = in.walk_speed;
            m.run_speed = in.run_speed;
            m.model_scale = in.model_scale;
            m.anim_scale = in.anim_scale;
            m.local_player = in.local_player;
            m.mount_race = in.mount_race;
            m.new_style_race = in.new_style_race;
            r = mech::select_movement_anim(m, fpu).start;
            st.still_since = 0;
        }
        if (!r.played) return keep;
        // The play (0x58afa0) does nothing for a sequence the model has no track of (nor its fallbacks).
        if (auto c = start_new(r.anim, r.reverse, r.speed, !r.play_once)) return *c;
        return keep;
    }

    // Old-style (classic) models.
    mech::OldStyleAnimInput o;
    o.speed_y = vy;
    o.speed_x = vx;
    o.speed_z = vz;
    o.speed_run = in.speed_run;
    o.colliding_type = in.colliding_type;
    o.display_time = in.now_ms;
    o.still_since = st.still_since;
    o.stand_state = static_cast<std::uint8_t>(in.stand_state == 111 ? 0x6f : in.stand_state);
    o.animation = st.anim;
    o.feet_wet = in.feet_wet;
    o.body_wet = in.body_wet;
    o.race = in.race;
    o.walk_speed = in.walk_speed;
    o.spawn_type = in.npc ? 1 : 0;
    o.local_player = in.local_player;
    o.random_percent = roll;
    o.can_idle = old_can_idle;
    // 0x4afe60 reads the same fields the new-style update does: +0x38c (a pet's master: the fidget needs a roll
    // above 99), the update's turning argument and +0x8c (0x58af10 passes the argument to ChooseIdleAnimation too)
    // and the global auto attack switch (0xe635aa, 0x4b01dd).
    o.owner_38c = in.owner_id;
    o.flag = in.turning;
    o.speed_heading = in.speed_heading;
    o.attacking = in.attacking;
    // +0x1229, the spell book's slot 0x94, the mount fields beyond GetMount's motion, +0x1198 and the jump / fall
    // fields (+0x10, +0x163, +0x180) stay at their defaults: their origin is not confirmed (docs/unknowns.md
    // "Animation fields not traced").
    mech::OldStyleAnimResult r = mech::select_old_style_anim(o, fpu);
    if (r.set_still_since) st.still_since = r.still_since;
    if (!r.start.played) return keep;
    AnimationType anim = static_cast<AnimationType>(r.start.anim);
    // 0x4b15b0: while sitting, standing becomes sitting and the idle the
    // seated fidget; other stances drop idle requests.
    if (in.stand_state == 110) {
        if (anim == AnimationType::Stand || anim == AnimationType::StandAfter) anim = AnimationType::Sit;
        else if (anim == AnimationType::Idle || anim == AnimationType::IdleAfter) anim = AnimationType::SitIdle;
        else if (anim >= AnimationType::NoEmote_39 && anim <= AnimationType::NoEmote_41) return keep;
    }
    if (in.stand_state != 100) {
        const AnimationType asked = static_cast<AnimationType>(r.start.anim);
        // 12..15 (the emotes ShakeHeadNo_12..FallingBackwards) count as idle requests too.
        bool idle_request = asked == AnimationType::Stand || asked == AnimationType::Turn ||
                            asked == AnimationType::StandAfter || asked == AnimationType::Idle ||
                            asked == AnimationType::IdleAfter || static_cast<std::uint16_t>(r.start.anim - 0xc) <= 3;
        if (idle_request && anim != AnimationType::Sit && anim != AnimationType::SitIdle) return keep;
    }
    auto has_anim = [&](AnimationType a) {
        std::string_view c = mech::old_anim_clip(num(a));
        return !c.empty() && has_clip(c);
    };
    // 0x4b15b0: an animation the model lacks takes 0x4b1210's replacement (the seated ones play nothing).
    if (!has_anim(anim)) {
        if (anim == AnimationType::Sit || anim == AnimationType::SitIdle) return keep; // nothing starts
        anim = old_replacement(anim);
    }
    // Its idle block: what is still missing is a stand request. A stand request plays the follow-up (+0x10c4)
    // when there is one, else P01, or P08 when the animation playing (+0x10c0) is not one 0x4b0860 names; an
    // idle (O01) after such an animation is O02 (0x48), 0x4b1210's O01 for a model without it.
    AnimationType follow_up = static_cast<AnimationType>(in.follow_up);
    const bool plain = classic_plain_stand_after(st.anim);
    if (!has_anim(anim) || anim == AnimationType::Stand) {
        anim = AnimationType::Stand;
        // 0x4b1a70: the stand input is 1.0 and looping, whatever the missing animation asked; the reverse flag
        // is not cleared (0x4b1ab3 touches only the restart flag and the speed).
        r.start.speed = 1.0f;
        r.start.play_once = false;
        if (in.follow_up == num(AnimationType::Stand)) {
            if (!plain) anim = AnimationType::StandAfter;
        } else {
            // An action as the follow-up is the layers' to play (render/actor_layers.h); the base stays P01.
            if (in.follow_up == num(AnimationType::StandAfter)) anim = AnimationType::StandAfter;
            follow_up = plain ? AnimationType::Stand : AnimationType::StandAfter;
        }
    } else if (anim == AnimationType::Idle && !plain) {
        anim = AnimationType::IdleAfter;
        if (!has_anim(anim)) anim = old_replacement(anim);
    }
    // Still missing (P08 on most models): the stand; a model without P01 plays nothing.
    if (!has_anim(anim)) {
        anim = AnimationType::Stand;
        if (!has_anim(anim)) return keep;
        r.start.speed = 1.0f;
        r.start.play_once = false;
    }
    const std::string clip(mech::old_anim_clip(num(anim)));
    // The movement's rate takes the same route as an action's: 0x4b15b0's pre-processing, then 0x4afc90.
    double rate = classic_action_rate(num(anim), r.start.speed, in.race, in.gender, in.tunic_point,
                                      in.luclin_elementals);
    // 0x4b15b0 starts the layers with a 100 ms blend (0x9c3988) whatever the animation; the tag table's
    // blend times are the new-style object's.
    SpawnAnimChoice c =
        start(clip, num(anim), r.start.reverse, static_cast<float>(rate), !r.start.play_once, kClassicBlendMs);
    c.follow_up = num(follow_up);
    return c;
}

bool instrument_song_animation(std::uint8_t anim, std::uint8_t class_id, std::uint8_t stand_state,
                               bool holds_instrument) {
    if (anim != kAnimDrum && anim != kAnimLute && anim != kAnimHorn) return true;
    if (stand_state == kStandStateSitting) return false;
    if (class_id == kClassBard && !holds_instrument) return false;
    return true;
}

std::string action_clip(std::uint8_t action, const std::function<bool(std::string_view)>& has_clip, bool new_style,
                        const NewStyleTrackInput& track_in, const std::function<int()>& rand,
                        std::uint8_t* playing, bool* stand_request, std::uint8_t* normalized,
                        const ActionGate* gate) {
    if (playing) *playing = action;
    if (new_style) {
        // The new-style object's slot 0x14: an old-style number through the map at 0xaaf5c8.
        std::uint8_t code = mech::old_to_new_anim(action);
        if (code == 0x4f || code == mech::kNoAnimation) return {};
        NewStyleTrack t = new_style_track(code, track_in, has_clip, rand);
        if (playing) *playing = t.code;
        return t.clip;
    }
    // 0x4b15b0: a new-style code becomes its old-style number (0xaaf428); 0x4f and above play nothing.
    if (action >= static_cast<std::uint8_t>(AnimationType::End) && action <= mech::kLastNewStyleCode)
        action = mech::new_to_old_anim(action);
    if (normalized) *normalized = action;
    if (action == 0 || action >= static_cast<std::uint8_t>(AnimationType::End)) {
        if (playing) *playing = action;
        return {};
    }
    // The classic/Luclin instrument-song block, after the new-to-old map and before the clip or stand fallback.
    if (gate && !instrument_song_animation(action, gate->class_id, gate->stand_state, gate->holds_instrument)) {
        if (playing) *playing = action;
        return {};
    }
    std::string code(mech::old_anim_clip(action));
    // 0x4b15b0's stand path: a P01 request (0x20) is the stand, not a one-shot action.
    if (action == static_cast<std::uint8_t>(AnimationType::Stand)) {
        if (playing) *playing = action;
        if (stand_request) *stand_request = true;
        return {};
    }
    if (has_clip(code)) {
        if (playing) *playing = action;
        return code;
    }
    // 0x4b1210's replacement; the stand it falls back to is 0x4b15b0's stand request, which the movement choice
    // plays (P01, or the follow-up +0x10c4).
    AnimationType r = old_replacement(static_cast<AnimationType>(action));
    if (r == AnimationType::Stand) {
        if (playing) *playing = num(r);
        if (stand_request) *stand_request = true;
        return {};
    }
    std::string alt(mech::old_anim_clip(num(r)));
    if (!alt.empty() && has_clip(alt)) {
        if (playing) *playing = num(r);
        return alt;
    }
    // The replacement is missing too: 0x4b15b0 asks for the stand.
    if (playing) *playing = num(AnimationType::Stand);
    if (stand_request) *stand_request = true;
    return {};
}

float classic_action_rate(std::uint8_t anim, float speed, std::uint16_t race, std::uint8_t gender,
                          bool tunic_point, bool luclin_elementals) {
    const float pre = mech::old_action_speed(anim, speed, race, gender, tunic_point);
    return static_cast<float>(mech::old_model_anim_speed(anim, pre, race, gender, tunic_point, luclin_elementals));
}

bool classic_idle_request(std::uint8_t requested) {
    // 0x4b1ba0: P01, P03, P08, O01, O02 and 0xc..0xf.
    return requested == num(AnimationType::Stand) || requested == num(AnimationType::Turn) ||
           requested == num(AnimationType::StandAfter) || requested == num(AnimationType::Idle) ||
           requested == num(AnimationType::IdleAfter) || (requested >= 0xc && requested <= 0xf);
}

ClassicStand classic_stand_request(std::uint8_t normalized, std::uint8_t playing, std::uint8_t stand_state,
                                   const std::function<bool(std::string_view)>& has_clip,
                                   const ClassicStandContext& ctx) {
    // 0x4b1917: a direct stand request from a sitting actor becomes the sit clip before the lookup. Without P07
    // nothing plays (0x4b1a06); with it the sit clip keeps the packet's speed, not the stand fallback's 1.0.
    // Race 0x3c and the local player in first person (CDisplay::cameraType 0) play nothing (0x4b194b, 0x4b195b).
    if (stand_state == kStandStateSitting && normalized == num(AnimationType::Stand)) {
        if (ctx.race == kRaceNoSitConversion || ctx.local_first_person) return {};
        if (!has_clip("P07")) return {};
        return {"P07", num(AnimationType::Sit), true};
    }
    // 0x4b1b97: an actor that is not standing (+0x35c not 100) plays nothing for an idle request (the packet's
    // own number), unless it became the sit clip above.
    if (stand_state != kStandStateStanding && classic_idle_request(ctx.requested.value_or(normalized))) return {};
    const bool plain = classic_plain_stand_after(playing);
    std::uint8_t anim = plain ? num(AnimationType::Stand) : num(AnimationType::StandAfter);
    std::string_view c = mech::old_anim_clip(anim);
    if (!c.empty() && has_clip(c)) return {std::string(c), anim};
    // A model without P08 plays P01's clip and code; without P01 too, nothing.
    anim = num(AnimationType::Stand);
    c = mech::old_anim_clip(anim);
    if (!c.empty() && has_clip(c)) return {std::string(c), anim};
    return {};
}

std::uint8_t classic_follow_up_after(std::uint8_t playing) {
    return classic_plain_stand_after(playing) ? num(AnimationType::Stand) : num(AnimationType::StandAfter);
}

} // namespace hailstone::game
