#include "render/actor_layers.h"

#include <algorithm>
#include <cmath>
#include <format>

#include <glm/gtc/quaternion.hpp>

#include "game/anim_layers.h"
#include "game/mechanics/anim_select.h"

namespace hailstone::render {

namespace mech = game::mechanics;

namespace {

// The follow-up sentinel (+0x10c4): 0x20 (P01) means none.
constexpr std::uint8_t kNoFollowUp = static_cast<std::uint8_t>(hailstone::AnimationType::Stand);
} // namespace

bool GroupTrack::start(const assets::AnimationClip* c, std::uint8_t code, float speed, bool loops, bool reversed,
                       float blend_s, bool restart, std::int32_t style_column) {
    if (!c) return false;
    if (!std::isfinite(speed) || speed < 0.0f) speed = 0.0f;
    if (c == clip && reversed == reverse && column == style_column && !restart) {
        // BlendToAnimation leaves the animation it plays alone; the caller's speed still reaches it
        // (SetCurrentAnimationParameters, 0x4b15b0's "keep" path / 0x58ac90's mode 3).
        rate = speed;
        loop = loops;
        anim = code;
        return false;
    }
    if (clip && blend_s > 0.0f) {
        previous = clip;
        previous_time = time;
        previous_reverse = reverse;
        previous_loop = loop;
        blend = 0.0f;
        blend_seconds = blend_s;
    } else {
        previous = nullptr;
        blend = 1.0f;
    }
    clip = c;
    anim = code;
    column = style_column;
    rate = speed;
    loop = loops;
    reverse = reversed;
    time = 0.0f;
    elapsed = 0.0f;
    return true;
}

void GroupTrack::advance(float dt) {
    if (!clip) return;
    time += dt * rate;
    const float d = clip->duration_seconds();
    if (loop && d > 0.0f) time = std::fmod(time, d);
    elapsed += dt;
    if (blend < 1.0f) {
        blend = blend_seconds > 0.0f ? std::min(1.0f, blend + dt / blend_seconds) : 1.0f;
        previous_time += dt;
    }
    if (blend >= 1.0f) previous = nullptr;
}

bool GroupTrack::completed() const {
    if (!clip) return true;
    const float d = clip->duration_seconds();
    if (d <= 0.0f) return true;
    if (rate == 0.0f) return false;
    return elapsed >= d / std::fabs(rate);
}

void GroupTrack::sync_to(const GroupTrack& other) {
    if (clip && clip == other.clip && column == other.column) time = other.time;
}

void GroupTrack::finish() {
    if (!clip) return;
    const float d = clip->duration_seconds();
    time = d;
    elapsed = rate > 0.0f ? d / rate : d;
    previous = nullptr;
    blend = 1.0f;
}

namespace {
float shown(const assets::AnimationClip* c, float t, bool reversed) {
    if (!reversed || !c) return t;
    const float d = c->duration_seconds();
    return d > 0.0f ? std::max(0.0f, d - std::min(t, d)) : 0.0f;
}
} // namespace

float GroupTrack::shown_time() const { return shown(clip, time, reverse); }
float GroupTrack::shown_previous_time() const { return shown(previous, previous_time, previous_reverse); }

namespace {
// The cross-fade into `to` on a group that plays `from`: classic models always 100 ms (0x9c3988), new-style ones
// the mean of the old sequence's blend-out and the new one's blend-in (0x5890a0 / 0x589410, per group).
float blend_seconds(const LayerRequest& req, std::uint8_t from, std::uint8_t to, float classic_ms) {
    return (req.new_style ? mech::new_style_blend_ms(from, to) : classic_ms) / 1000.0f;
}
} // namespace

bool ActorLayers::start_action(const assets::AnimationClip* clip, std::uint8_t requested, std::uint8_t chosen,
                               float rate, const LayerRequest& req) {
    if (!clip || req.corpse) return false;
    GroupTrack& up = group[game::kUpperBones];
    GroupTrack& lo = group[game::kLowerBones];
    bool upper_only = false;
    if (req.new_style) {
        // 0x58afa0: each group's sequence and style mode decides. A rider plays only sequences of layer kind 1, on the upper
        // body (0x588e20); a kind 1 sequence plays there alone too while the legs walk, run or turn (kind 2).
        const std::uint8_t kind = mech::new_style_layer_kind(chosen);
        auto mode_of = [&](const GroupTrack& g) {
            game::LayerModeInput li;
            li.wanted = chosen;
            li.column = req.action_column;
            li.playing = g.anim;
            li.playing_column = g.column;
            li.completed = g.completed();
            return game::new_style_layer_mode(li);
        };
        const std::uint8_t upper_mode = mode_of(up);
        if (req.mounted) {
            if (kind != 1) return false;
            upper_only = true;
        } else {
            upper_only = kind == 1 && mech::new_style_layer_kind(lo.anim) == 2;
        }
        if (upper_only) {
            // 0x588e20: nothing while the upper body plays a sequence of layer kind 0 (and no mount answer).
            if (upper_mode != 2) return false;
            if (!req.mounted && mech::new_style_layer_kind(up.anim) == 0) return false;
        } else {
            const std::uint8_t lower_mode = mode_of(lo);
            if (kind == 2) {
                // 0x5890a0: nothing without a lower mode, then each group by its own.
                if (lower_mode == 0 || (lower_mode != 2 && upper_mode != 2)) return false;
                upper_only = lower_mode != 2;
                if (upper_mode != 2) {
                    lo.start(clip, chosen, rate, false, false, blend_seconds(req, lo.anim, chosen, game::kClassicBlendMs), false, req.action_column);
                    return false;
                }
            } else {
                // 0x589410: nothing when either mode is 0, both 3 continue, neither 2 queues.
                if (lower_mode == 0 || upper_mode == 0) return false;
                if (lower_mode != 2 && upper_mode != 2) return false;
            }
        }
    } else {
        // 0x4b15b0 with key 1: an action that may not interrupt waits for the upper body's cycle.
        if (!game::classic_may_interrupt(requested, up.anim) && up.clip && !up.completed()) {
            if (game::classic_queues(chosen)) {
                follow_up = chosen;
                follow_clip = clip;
                follow_rate = rate;
            } else if (game::classic_clears_queue(chosen)) {
                follow_up = kNoFollowUp;
                follow_clip = nullptr;
            }
            return false;
        }
        upper_only = game::classic_action_upper_only(chosen, req.mounted, req.humanoid, req.speed_run);
    }
    // BlendToAnimation: a group already playing the clip keeps it.
    up.start(clip, chosen, rate, false, false, blend_seconds(req, up.anim, chosen, game::kClassicBlendMs), false, req.action_column);
    if (!upper_only) {
        lo.start(clip, chosen, rate, false, false, blend_seconds(req, lo.anim, chosen, game::kClassicBlendMs), false, req.action_column);
        up.sync_to(lo);
    }
    action = clip;
    action_anim = chosen;
    action_on_legs = !upper_only;
    legs_took_motion = false;
    return true;
}

bool ActorLayers::stand_request(std::uint8_t requested, const LayerRequest& req) {
    if (req.corpse) return false;
    GroupTrack& up = group[game::kUpperBones];
    // 0x4b0900: an unfinished action the stand may not interrupt keeps the upper body.
    if (action && up.clip && !up.completed() && !game::classic_may_interrupt(requested, up.anim)) return false;
    action = nullptr;
    action_on_legs = false;
    legs_took_motion = false;
    return true;
}

bool ActorLayers::start_stand(const assets::AnimationClip* clip, std::uint8_t requested, std::uint8_t anim,
                              float rate, LayerRequest& req) {
    if (!clip || !stand_request(requested, req)) return false;
    req.base = clip;
    req.base_anim = anim;
    req.stand_request = true;
    req.loop = true;
    req.reverse = false;
    req.rate = rate;
    req.restart = false;
    return true;
}

bool ActorLayers::step(const LayerRequest& req, float dt) {
    GroupTrack& up = group[game::kUpperBones];
    GroupTrack& lo = group[game::kLowerBones];
    if (req.corpse) action = nullptr;
    // The action ends once the upper body has played it through (or something else took the upper body).
    if (action && (up.clip != action || up.completed())) action = nullptr;
    if (!action) {
        action_on_legs = false;
        legs_took_motion = false;
    }
    bool followed = false;

    if (!req.new_style && !req.corpse) {
        if (action && game::classic_clears_queue(req.base_anim)) {
            // A movement request while the action plays resets the follow-up.
            follow_up = kNoFollowUp;
            follow_clip = nullptr;
        } else if (!action && req.stand_request && follow_up != kNoFollowUp && follow_clip) {
            // The stand request plays the follow-up (0x4b15b0's idle path: +0x10c4 becomes the request, and
            // then 0x20 or 0x47 by 0x4b0860 on the animation playing).
            const assets::AnimationClip* c = follow_clip;
            const std::uint8_t a = follow_up;
            follow_up = req.follow_up_after;
            follow_clip = nullptr;
            followed = start_action(c, a, a, follow_rate, req);
        }
    }
    bool legs_free = !action;
    if (action && req.new_style) {
        const std::uint8_t kind = mech::new_style_layer_kind(req.base_anim);
        if (!action_on_legs && kind == 2) {
            // 0x5890a0: the legs take the walk / run / turn (their mode 2 or 3); the upper body's unfinished
            // action queues it (mode 1).
            legs_free = true;
        } else if (!action_on_legs && !req.mounted) {
            // 0x589410: another sequence starts on both groups when either group's mode is 2, cutting the
            // action; otherwise nothing changes.
            game::LayerModeInput li;
            li.wanted = req.base_anim;
            li.column = req.base_column;
            li.playing = lo.anim;
            li.playing_column = lo.column;
            li.completed = lo.completed();
            const std::uint8_t lower_mode = game::new_style_layer_mode(li);
            li.playing = up.anim;
            li.playing_column = up.column;
            li.completed = up.completed();
            const std::uint8_t upper_mode = game::new_style_layer_mode(li);
            if (lower_mode != 0 && upper_mode != 0 && (lower_mode == 2 || upper_mode == 2)) {
                action = nullptr;
                action_on_legs = false;
                legs_free = true;
            }
        }
    } else if (action && game::classic_legs_take_motion(action_anim, req.base_anim, req.humanoid)) {
        // 0x4b15b0's busy path: a walk or run under an unfinished action of table 0xdced38 starts on the
        // legs alone; other requests wait for the action.
        legs_free = true;
        action_on_legs = false;
        legs_took_motion = true;
    }
    bool started = false;
    // A corpse that already lies (its clip played through) takes a changed clip at its end too: a new appearance
    // must not make it fall again.
    const bool settled = req.corpse && corpse_settled;
    if (legs_free) {
        started = lo.start(req.base, req.base_anim, req.rate, req.loop, req.reverse,
                           blend_seconds(req, lo.anim, req.base_anim, req.blend_ms), req.restart, req.base_column);
    }
    if (!action) {
        // Back on the base clip, the upper body takes the legs' position when they play the same
        // (the lower group's SyncToBoneGroup call after both are started).
        if (up.start(req.base, req.base_anim, req.rate, req.loop, req.reverse,
                     blend_seconds(req, up.anim, req.base_anim, req.blend_ms), req.restart, req.base_column)) {
            up.sync_to(lo);
            started = true;
        }
    }
    up.advance(dt);
    lo.advance(dt);
    // A spawn already dead when this state first drew it shows the death clip's last frame at once, as the original's
    // actor build does (BuildActorForSpawn's slot 0x20 call at 0x490366: D05 reversed at speed 0, held at its end):
    // the fall is not played again. One that dies while drawn starts the clip normally.
    if (started && req.corpse && (req.corpse_at_end || settled)) {
        lo.finish();
        up.finish();
    }
    corpse_settled = req.corpse && lo.clip && lo.completed();
    return followed;
}

void pose_actor(const assets::CharacterModel& model, const PoseInput& in, std::vector<assets::BonePose>& local,
                std::vector<assets::BonePose>& previous, std::vector<glm::mat4>& world) {
    // One bone group's tracks: the clip, cross-faded from the previous one by the linear weight.
    auto sample = [&](const assets::AnimationClip& clip, float time, bool loop, const assets::AnimationClip* from,
                      float from_time, bool from_loop, float blend, std::vector<assets::BonePose>& out) {
        model.sample_local(clip, time, loop, out);
        if (from && blend < 1.0f) {
            // A played-out one-shot holds its last frame; only a looping clip wraps (0x1003fcc0's old track).
            model.sample_local(*from, from_time, from_loop, previous);
            float t = std::clamp(blend, 0.0f, 1.0f);
            for (std::size_t b = 0; b < out.size() && b < previous.size(); ++b) {
                out[b].rotation = glm::normalize(glm::slerp(previous[b].rotation, out[b].rotation, t));
                out[b].translation = glm::mix(previous[b].translation, out[b].translation, t);
                out[b].scale = glm::mix(previous[b].scale, out[b].scale, t);
            }
        }
    };
    sample(*in.clip, in.time, in.loop, in.previous, in.previous_time, in.previous_loop, in.blend, local);
    if (in.upper_clip && in.bone_groups) {
        thread_local std::vector<assets::BonePose> upper;
        sample(*in.upper_clip, in.upper_time, in.upper_loop, in.upper_previous, in.upper_previous_time,
               in.upper_previous_loop, in.upper_blend, upper);
        const auto& groups = *in.bone_groups;
        for (std::size_t b = 0; b < local.size() && b < upper.size() && b < groups.size(); ++b)
            if (groups[b] == game::kUpperBones) local[b] = upper[b];
    }
    model.compose(local, world);
}

std::vector<std::string> describe_pose(const assets::CharacterModel& model, const PoseInput& in) {
    std::vector<std::string> lines;
    lines.push_back(std::format("model {} from {}{}, clips lent by {}", model.code, model.source,
                                model.eqg ? " (EQG)" : "",
                                model.animation_donor.empty() ? "none" : model.animation_donor));
    // A clip by its pointer: the draw holds pointers into the model's clips, so one the model does not hold is
    // itself a finding.
    auto name = [&](const assets::AnimationClip* clip) -> std::string {
        if (!clip) return "none";
        for (const auto& [code, c] : model.clips)
            if (&c == clip) return std::format("{}{}", code.empty() ? "rest" : code, c.borrowed ? " (lent)" : "");
        return "?";
    };
    auto group = [&](const char* label, const assets::AnimationClip* clip, float time, bool loop,
                     const assets::AnimationClip* from, float from_time, float blend) {
        std::string line = std::format("{}: {} at {:.2f} of {:.2f} s{}", label, name(clip), time,
                                       clip ? clip->duration_seconds() : 0.0f, loop ? ", looping" : "");
        if (from && blend < 1.0f) line += std::format(", {:.0f}% faded in from {} at {:.2f} s", blend * 100.0f,
                                                      name(from), from_time);
        lines.push_back(std::move(line));
    };
    const bool split = in.upper_clip && in.bone_groups;
    group(split ? "lower body" : "whole body", in.clip, in.time, in.loop, in.previous, in.previous_time, in.blend);
    if (split)
        group("upper body", in.upper_clip, in.upper_time, in.upper_loop, in.upper_previous, in.upper_previous_time,
              in.upper_blend);
    if (!in.clip) return lines;

    std::vector<assets::BonePose> local, previous;
    std::vector<glm::mat4> world;
    pose_actor(model, in, local, previous, world);
    const auto rest_clip = model.clips.find("");
    std::string at_rest;
    if (rest_clip != model.clips.end()) {
        const auto& rest = rest_clip->second.tracks;
        constexpr float kSameRotation = 0.99999f, kSameTranslation = 1e-4f;
        for (std::size_t b = 1; b < local.size() && b < rest.size() && b < model.bone_names.size(); ++b) {
            if (rest[b].empty()) continue;
            std::string bone = model.bone_names[b];
            if (bone.find("POINT") != std::string::npos) continue;
            const assets::BonePose r = rest[b].front();
            if (std::abs(glm::dot(local[b].rotation, r.rotation)) < kSameRotation ||
                glm::length(local[b].translation - r.translation) > kSameTranslation)
                continue;
            if (bone.starts_with(model.code)) bone.erase(0, model.code.size());
            if (bone.ends_with("_TRACK")) bone.resize(bone.size() - 6);
            at_rest += (at_rest.empty() ? "" : " ") + bone;
        }
    }
    lines.push_back("bones at rest: " + (at_rest.empty() ? std::string("none") : at_rest));
    return lines;
}

bool ActorLayers::split() const {
    const GroupTrack& up = group[game::kUpperBones];
    const GroupTrack& lo = group[game::kLowerBones];
    return up.clip != lo.clip || up.column != lo.column || up.reverse != lo.reverse || up.time != lo.time ||
           up.previous != lo.previous || up.blend != lo.blend ||
           (up.previous && (up.previous_time != lo.previous_time || up.previous_reverse != lo.previous_reverse ||
                            up.previous_loop != lo.previous_loop));
}

} // namespace hailstone::render
