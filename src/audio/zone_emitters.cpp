#include "audio/zone_emitters.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace hailstone::audio {

namespace {

constexpr float kNoRange = 10000000.0f;        // 0x4b189680 when +0x40 is not positive
constexpr float kPriorityBase = 10000.0f;      // music priority = int(range + 10000) (0x9c58a4)
constexpr float kNegativeRadius = 10000.0f;    // a negative EFF radius becomes 10000 (and an environment)
constexpr std::int32_t kCrossfadeMs = 1500;    // 0x5dc
constexpr std::int32_t kMaxEffFadeOutMs = 20000;

// (int)x as the original converts (cvttss2si / _ftol): a NaN or a value past int32, which C++ leaves undefined,
// gives INT_MIN there.
std::int32_t ftol(float x) {
    return std::fabs(x) < 2147483648.0f ? static_cast<std::int32_t>(x) : std::numeric_limits<std::int32_t>::min();
}
// a + b with the original's 32-bit wrap (a signed overflow is undefined in C++).
std::int32_t add32(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

bool contains_nocase(const std::string& s, const char* what) {
    std::string a(s), b(what);
    for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return a.find(b) != std::string::npos;
}

std::string label_of(const std::string& label, std::size_t i) { return label + ":" + std::to_string(i); }

} // namespace

const std::string* WaveTable::find(std::int32_t id) const {
    if (auto it = zone.find(id); it != zone.end()) return &it->second;
    if (global) {
        if (auto it = global->find(id); it != global->end()) return &it->second;
    }
    return nullptr;
}

float classic_level_gain(std::int32_t level, std::uint8_t kind) {
    // `NEG` on a 32-bit word: INT_MIN stays negative and goes to the dB helper (silent).
    std::int32_t neg = level == INT32_MIN ? INT32_MIN : -level;
    if (neg < 0) {
        if (neg < -10000) return 0.0f;
        return static_cast<float>(std::pow(10.0, static_cast<double>(neg) / 2000.0));
    }
    return kind == 0 ? 0.2f : 1.0f;
}

ZoneSoundSet emitters_from_emt(const std::vector<assets::EmtRow>& rows,
                               const std::function<bool(const std::string&)>& exists, const std::string& label) {
    ZoneSoundSet out;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const assets::EmtRow& r = rows[i];
        if (r.file.empty() || !exists(r.file)) continue;
        Emitter e;
        if (contains_nocase(r.file, ".mp3") || contains_nocase(r.file, ".xmi")) {
            MusicEntry m;
            m.file = r.file;
            m.track = r.xmi_track;
            m.volume = r.gain;
            m.priority = ftol(r.range + kPriorityBase);
            m.fade_in_ms = r.fade_in;
            m.crossfade_ms = kCrossfadeMs;
            m.fade_out_ms = r.fade_out;
            e.music = static_cast<int>(out.music.size());
            e.flag24 = r.flag29;
            e.flag25 = r.flag2a;
            m.finish_when_done = r.flag2a; // Set's last argument (EmitterAdd passes the row's +0x2a)
            out.music.push_back(std::move(m));
        } else {
            e.wave = r.file;
            e.flag24 = r.flag2a;
            e.pool = 4; // EmitterAdd @0x4aa5ee: SoundEmitter(manager, asset, 4, ...)
        }
        e.gain = r.gain;
        e.echo = r.echo;
        e.fade_in_ms = r.fade_in;
        e.fade_out_ms = r.fade_out;
        e.loop_count = r.loop_count;
        e.position = r.position;
        e.min_distance = r.full_volume_distance;
        e.max_distance = r.max_distance;
        e.random_distance = r.random_distance;
        e.range = r.range;
        e.delay_min = r.delay_min;
        e.delay_max = r.delay_max;
        e.environment = r.environment;
        e.period = r.period;
        e.user_disableable = r.user_disableable;
        e.source = label_of(label, i);
        out.emitters.push_back(std::move(e));
    }
    return out;
}

namespace {

// CreateOldEmitter @0x4aac30 for one side of a record.
std::optional<Emitter> old_emitter(ZoneSoundSet& set, std::uint8_t kind, const glm::vec3& pos, std::int32_t level,
                                   std::int32_t cooldown, std::int32_t random, float radius, std::int32_t param,
                                   std::int32_t fade_out, std::int32_t sound, const WaveTable& waves,
                                   const std::vector<std::string>& mp3index, const std::string& zone_xmi,
                                   const std::function<bool(const std::string&)>& exists) {
    std::int32_t environment = 0;
    if (radius < 0.0f) {
        environment = ftol(-radius);
        radius = kNegativeRadius;
    }
    Emitter e;
    e.position = pos;
    e.environment = environment;
    e.period = 0;
    e.user_disableable = true;
    e.pool = 4; // @0x4aaea7: SoundEmitter(manager, asset, 4, 0, 0) for the wave kinds
    e.fade_out_ms = fade_out;
    e.range = radius;
    switch (kind) {
    case 1: {
        MusicEntry m;
        if (sound < 0) {
            // OldMp3GetSelection(-sound): mp3index.txt, one-based.
            std::int64_t index = -static_cast<std::int64_t>(sound) - 1;
            if (index < 0 || index >= static_cast<std::int64_t>(mp3index.size())) return std::nullopt;
            m.file = mp3index[static_cast<std::size_t>(index)];
            if (!exists(m.file)) return std::nullopt;
            m.track = 0;
        } else {
            if (zone_xmi.empty()) return std::nullopt;
            m.file = zone_xmi;
            m.track = sound;
        }
        fade_out = std::min(fade_out, kMaxEffFadeOutMs);
        m.volume = 1.0f;
        m.priority = ftol(radius + kPriorityBase);
        m.fade_in_ms = param;
        m.crossfade_ms = kCrossfadeMs;
        m.fade_out_ms = fade_out;
        e.music = static_cast<int>(set.music.size());
        set.music.push_back(std::move(m));
        e.loop_count = level;
        e.gain = 1.0f;
        e.delay_min = cooldown;
        e.delay_max = add32(cooldown, random);
        e.min_distance = e.max_distance = 0.0f;
        e.fade_in_ms = param;
        e.fade_out_ms = fade_out;
        return e;
    }
    case 0:
    case 2:
    case 3:
        break;
    default:
        return std::nullopt;
    }
    const std::string* wave = waves.find(sound);
    if (!wave || !exists(*wave)) return std::nullopt;
    e.wave = *wave;
    e.gain = classic_level_gain(level, kind);
    if (cooldown < 1) {
        e.loop_count = 0;
        e.delay_min = e.delay_max = 0;
    } else {
        e.loop_count = 1;
        if (random > 0) {
            e.delay_min = add32(cooldown, 500);
            e.delay_max = add32(e.delay_min, random);
        } else {
            e.delay_min = e.delay_max = cooldown;
        }
    }
    if (kind == 3) {
        e.random_distance = static_cast<float>(param) + 150.0f;
        e.min_distance = 8.0f;
        e.max_distance = 100.0f;
    } else if (kind == 2) {
        e.min_distance = param < 3 ? 3.0f : static_cast<float>(param);
        e.max_distance = 100.0f;
    }
    return e;
}

} // namespace

ZoneSoundSet emitters_from_eff(const std::vector<assets::EffSoundRecord>& records, const WaveTable& waves,
                               const std::vector<std::string>& mp3index, const std::string& zone_xmi,
                               const std::function<bool(const std::string&)>& exists, const std::string& label) {
    ZoneSoundSet out;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const assets::EffSoundRecord& r = records[i];
        // Both sides become one always-on emitter when kind, level, cooldown and
        // sound agree, or when the first kind is 2. Both calls take the first kind.
        bool one = (r.kind[0] == r.kind[1] && r.level[0] == r.level[1] && r.cooldown[0] == r.cooldown[1] &&
                    r.sound[0] == r.sound[1]) ||
                   r.kind[0] == 2;
        for (int side = 0; side < (one ? 1 : 2); ++side) {
            auto e = old_emitter(out, r.kind[0], r.position, r.level[side], r.cooldown[side], r.random_delay, r.radius,
                                 r.param[side], r.fade_out[side], r.sound[side], waves, mp3index, zone_xmi, exists);
            if (!e) continue;
            e->period = one ? 0 : side + 1;
            e->source = label_of(label, i) + (one ? "" : side == 0 ? "/day" : "/night");
            out.emitters.push_back(std::move(*e));
        }
    }
    return out;
}

ZoneEmitters::ZoneEmitters(ZoneSoundSet set) : set_(std::move(set)), state_(set_.emitters.size()) {
    // The day side starts enabled and the night side disabled (LoadOldEmitters);
    // set_state() sets them properly.
    for (std::size_t i = 0; i < state_.size(); ++i) state_[i].enabled = set_.emitters[i].period != 2;
    for (std::size_t i = 0; i < set_.music.size(); ++i) {
        set_.music[i].id = kZoneMusicId + static_cast<std::int32_t>(i);
        own_music_.set(set_.music[i]);
    }
}

void ZoneEmitters::set_music_manager(MusicManager* music, EmitterSink& sink) {
    MusicManager& from = music_manager();
    MusicManager& to = music ? *music : own_music_;
    if (&from == &to) return;
    for (std::size_t i = 0; i < set_.music.size(); ++i) {
        std::int32_t id = kZoneMusicId + static_cast<std::int32_t>(i);
        if (const MusicEntry* e = from.find(id)) to.set(*e);
        from.remove(id, sink);
    }
    music_ = music;
}

int ZoneEmitters::current_music() const {
    const MusicEntry* p = (music_ ? *music_ : own_music_).playing();
    if (!p || p->id < kZoneMusicId) return -1;
    std::int32_t i = p->id - kZoneMusicId;
    return i < static_cast<std::int32_t>(set_.music.size()) ? i : -1;
}

void ZoneEmitters::set_state(bool night, std::int32_t environment, bool user_disabled) {
    std::int32_t time = night ? 2 : 1;
    for (std::size_t i = 0; i < state_.size(); ++i) {
        const Emitter& e = set_.emitters[i];
        bool off = (e.environment != 0 && e.environment != environment) || (e.period != 0 && e.period != time) ||
                   (e.user_disableable && user_disabled);
        state_[i].enabled = !off;
    }
}

void ZoneEmitters::reset_timer(std::size_t i, std::uint32_t now_ms) {
    // SoundEmitter::ResetTimer @0x5bed30.
    State& s = state_[i];
    const Emitter& e = set_.emitters[i];
    s.anchor = now_ms;
    s.delay = e.delay_min;
    // max - min wraps in 32 bits as the original's sub (-2e9 .. 2e9 from a forged file overflows int32); it is not 0
    // here, and the remainder of a non-negative rand() by it is defined whatever its sign.
    if (e.delay_min < e.delay_max) {
        const std::int32_t span = add32(e.delay_max, static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(e.delay_min)));
        s.delay = add32(s.delay, static_cast<std::int32_t>(static_cast<std::int64_t>(rand_()) % span));
    }
}

void ZoneEmitters::update(const glm::vec3& listener, std::uint32_t now_ms, float effects_volume, float music_volume,
                          EmitterSink& sink) {
    std::erase_if(fading_loops_, [&](std::uint32_t voice) { return !sink.alive(voice); });
    MusicManager& music = music_manager();
    music.set_volume(music_volume);
    for (std::size_t i = 0; i < set_.emitters.size(); ++i) {
        const Emitter& e = set_.emitters[i];
        State& s = state_[i];
        if (s.anchor == 0) reset_timer(i, now_ms);
        bool due = false, music_due = false;
        if (s.enabled) {
            glm::vec3 d = listener - e.position;
            float r = e.range > 0.0f ? e.range : kNoRange;
            if (e.max_distance != 0.0f && e.max_distance < r) r = e.max_distance;
            if (r * r <= glm::dot(d, d)) {
                if (e.music >= 0) {
                    // Out of range: the entry stops (MusicManager::Stop), unless it only plays
                    // on entering and another emitter of the same music is in range (not modelled).
                    if (!e.flag24 || !s.first) music.stop(kZoneMusicId + e.music, sink);
                    s.first = true;
                }
            } else {
                auto elapsed = static_cast<std::int32_t>(now_ms - s.anchor);
                if (s.delay == 0 || s.delay < elapsed || e.music >= 0) {
                    due = true;
                    music_due = (!e.flag24 && !e.flag25) || s.first;
                }
            }
        }
        if (e.music >= 0) {
            if (!music_due) continue;
            MusicEntry* m = music.find(kZoneMusicId + e.music);
            if (!m) continue;
            if (e.flag24 || e.flag25) m->loops = e.loop_count;
            music.play(m->id, sink);
            if (e.flag24 || e.flag25) m->playable = false;
            s.first = false;
            continue;
        }
        if (!due) {
            // ReleaseLoopingSound: the kept voice fades out over +0x1c.
            if (s.voice) {
                sink.release(s.voice, e.fade_out_ms);
                if (e.fade_out_ms > 0) fading_loops_.push_back(s.voice);
                s.voice = 0;
            }
            continue;
        }
        reset_timer(i, now_ms);
        if (s.voice) {
            if (sink.alive(s.voice)) continue;
            s.voice = 0; // the device dropped it; the original's instance would still be held
        }
        SoundControl c;
        c.volume = effects_volume * e.gain;
        c.fade_ms = e.fade_in_ms;
        c.loops = e.loop_count;
        c.echo = e.echo;
        c.min_distance = e.min_distance;
        c.max_distance = e.max_distance;
        c.position = e.position;
        c.pool = e.pool;
        if (e.random_distance > 0.0f) {
            float angle = static_cast<float>(rand_() % 360) * 6.2831855f * 0.0027777778f;
            float dist = static_cast<float>(rand_() % 10000) * e.random_distance * 0.0001f;
            c.position = {std::sin(angle) * dist + listener.x, std::cos(angle) * dist + listener.y, listener.z};
        }
        if ((e.delay_min != 0 || e.delay_max != 0) && e.loop_count != 0) {
            sink.play_wave(e.wave, c);
        } else {
            c.loops = 0;
            s.voice = sink.play_wave(e.wave, c);
        }
        s.first = false;
    }
}

void ZoneEmitters::clear(EmitterSink& sink) {
    for (auto voice : fading_loops_) sink.release(voice, 0);
    fading_loops_.clear();
    for (auto& s : state_)
        if (s.voice) {
            sink.release(s.voice, 0);
            s.voice = 0;
        }
    MusicManager& music = music_manager();
    for (std::size_t i = 0; i < set_.music.size(); ++i) music.remove(kZoneMusicId + static_cast<std::int32_t>(i), sink);
    if (music_) music.select(sink); // a global entry may take over
}

} // namespace hailstone::audio
