#include "audio/audio_system.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iterator>
#include <utility>

#include "assets/xmi.h"
#include "audio/sound_library.h"
#include "audio/xmi_player.h"
#include "core/config.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "platform/audio_device.h"

namespace hailstone::audio {

namespace {

std::string text_of(const std::optional<std::vector<std::byte>>& bytes) {
    if (!bytes) return {};
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

float level(int percent) { return static_cast<float>(std::clamp(percent, 0, 100)) / 100.0f; }

bool ends_with_nocase(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && lower_case(s.substr(s.size() - suffix.size())) == suffix;
}

} // namespace

AudioSettings load_audio_settings(const Config& c) {
    AudioSettings s;
    s.enabled = c.get_bool("audio.enabled", s.enabled);
    s.master = std::clamp(c.get_int("audio.master", s.master), 0, 100);
    s.music = std::clamp(c.get_int("audio.music", s.music), 0, 100);
    s.effects = std::clamp(c.get_int("audio.effects", s.effects), 0, 100);
    s.npc_hits = std::clamp(c.get_int("audio.npc_hits", s.npc_hits), 0, 100);
    for (const IndividualSound& sound : individual_sounds()) {
        int& percent = s.individual[sound.group];
        percent = std::clamp(c.get_int("audio." + std::string(sound.name), percent), kSilentPercent, kFullVolumePercent);
    }
    s.mute = c.get_bool("audio.mute", s.mute);
    s.environment = c.get_bool("audio.environment", s.environment);
    s.soundfont = c.get_string("audio.soundfont", s.soundfont);
    s.realism = std::clamp(c.get_int("audio.realism", s.realism), 0, 100);
    s.reverb = c.get_bool("audio.reverb", s.reverb);
    s.combat_music = c.get_bool("audio.combat_music", s.combat_music);
    s.reverse_sound = c.get_bool("audio.reverse_sound", s.reverse_sound);
    // loadOptions: SoundMixAhead 1..100, else 8.
    s.mix_ahead = c.get_int("audio.mix_ahead", s.mix_ahead);
    if (s.mix_ahead < 1 || s.mix_ahead > 100) s.mix_ahead = 8;
    return s;
}

void store_audio_settings(const AudioSettings& s, Config& c) {
    c.set_bool("audio.enabled", s.enabled);
    c.set_int("audio.master", s.master);
    c.set_int("audio.music", s.music);
    c.set_int("audio.effects", s.effects);
    c.set_int("audio.npc_hits", s.npc_hits);
    for (const IndividualSound& sound : individual_sounds())
        c.set_int("audio." + std::string(sound.name), s.individual[sound.group]);
    c.set_bool("audio.mute", s.mute);
    c.set_bool("audio.environment", s.environment);
    c.set("audio.soundfont", s.soundfont);
    c.set_int("audio.realism", s.realism);
    c.set_bool("audio.reverb", s.reverb);
    c.set_bool("audio.combat_music", s.combat_music);
    c.set_bool("audio.reverse_sound", s.reverse_sound);
    c.set_int("audio.mix_ahead", s.mix_ahead);
}

int mix_ahead_ms(int mix_ahead) { return std::clamp(mix_ahead, 4, 96) * 8; }

std::uint32_t mix_ahead_frames(int mix_ahead, std::uint32_t rate) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(mix_ahead_ms(mix_ahead)) * rate / 1000);
}

AudioSystem::AudioSystem(const GameDir& game, AudioSettings settings, bool open_device)
    : game_(game), settings_(std::move(settings)) {
    library_ = std::make_unique<SoundLibrary>(game_);
    {
        std::string assets = text_of(game_.read("soundassets.txt"));
        global_waves_ = assets::parse_sound_assets(assets);
        named_waves_ = assets::parse_named_sound_assets(assets);
    }
    mp3index_ = assets::parse_mp3_index(text_of(game_.read("mp3index.txt")));
    race_ids_ = assets::parse_race_sounds(text_of(game_.read("racedata.txt")));
    zone_rooms_ = load_zone_room_presets(game_);
    waves_.global = &global_waves_;
    std::uint32_t rate = 48000;
    mixer_ = std::make_unique<Mixer>(rate);
    if (open_device && settings_.enabled) {
        Mixer* m = mixer_.get();
        device_ = platform::AudioDevice::open([m](float* out, std::size_t frames) { m->render(out, frames); }, rate,
                                              mix_ahead_frames(settings_.mix_ahead, rate));
    }
    apply_volumes();
    // LoadGlobalWaves @0x4ab640 loads the global waves up front: ids 0x20..0xa1, 0x50f..0x514 and
    // 0xe6f (and every other named id; of those only the door and switch sounds 0xa2..0xbd and 0x1d0
    // here, the rest decode on first use).
    std::vector<std::string> global;
    auto add = [&](std::int32_t first, std::int32_t last) {
        for (std::int32_t id = first; id <= last; ++id)
            if (auto it = global_waves_.find(id); it != global_waves_.end()) global.push_back(it->second);
    };
    add(0x20, 0xbd);
    add(0x1d0, 0x1d0);
    add(0x50f, 0x514);
    add(0xe6f, 0xe6f);
    library_->preload(global);
    global_wave_files_ = std::move(global);
    // LoadGlobalMusic @0x4aa860: entries 1..15, the combat theme picked by rand() after srand(Util::FastTime()).
    // The entries are set once the sound archives are indexed (set_up_global_music: its file checks would wait
    // here for the worker to open every snd*.pfs); the rand() is drawn now.
    CrtRand rand(static_cast<std::uint32_t>(std::time(nullptr)));
    global_music_rand_ = rand();
    if (library_->index_ready()) set_up_global_music();
    // LoadGlobalEmitters @0x4a9df0: the rain (+0xfd8, wave 159) and wind (+0xfdc, wave 158) emitters, created
    // disabled: SoundEmitter::Init's defaults (gain 1, echo 1, any environment and time, not user-disableable) with
    // delays, loop count, range and distances 0 (always in range, looping, 2D), fades 2000 / 3000 and gain 0.5.
    {
        ZoneSoundSet set;
        for (std::int32_t id : {kRainWave, kWindWave}) {
            Emitter e;
            if (auto it = global_waves_.find(id); it != global_waves_.end()) e.wave = it->second;
            e.gain = 0.5f;
            e.echo = 1.0f;
            e.fade_in_ms = 2000;
            e.fade_out_ms = 3000;
            e.loop_count = 0;
            e.user_disableable = false;
            e.source = id == kRainWave ? "rain" : "wind";
            set.emitters.push_back(std::move(e));
        }
        weather_ = ZoneEmitters(std::move(set));
        apply_weather();
    }
    log::info("audio: {} sound names, {} music selections, {} races", global_waves_.size(), mp3index_.size(),
              race_ids_.size());
}

void AudioSystem::set_up_global_music() {
    if (global_music_set_) return;
    global_music_set_ = true;
    for (const MusicSetCall& c : load_global_music([this](const std::string& f) { return library_->exists(f); },
                                                   global_music_rand_)) {
        MusicEntry e;
        e.id = c.entry;
        e.file = c.asset;
        e.track = c.track;
        e.volume = c.volume;
        e.priority = c.priority;
        e.fade_in_ms = c.fade_in_ms;
        e.crossfade_ms = c.fade_out_replaced_ms;
        e.fade_out_ms = c.fade_out_stopped_ms;
        if (!e.file.empty() && std::find(global_music_files_.begin(), global_music_files_.end(), e.file) ==
                                   global_music_files_.end())
            global_music_files_.push_back(e.file);
        music_.set(std::move(e));
    }
    library_->set_resident_files(global_music_files_);
    // The selections asked for meanwhile (the login screen's theme), in order.
    for (const auto& [id, on] : std::exchange(pending_selections_, {})) music_selection(id, on);
}

AudioSystem::~AudioSystem() {
    device_.reset(); // stop the callback before the mixer goes
}

void AudioSystem::apply_volumes() {
    mixer_->set_master(settings_.mute ? 0.0f : level(settings_.master));
    mixer_->set_bus_volume(Bus::Effects, level(settings_.effects));
    mixer_->set_bus_volume(Bus::Music, level(settings_.music));
    // /reversesound: RoF2 only stores ReverseSound (0xde0d48; docs/formats/audio.md), the client swaps its output.
    mixer_->set_swap_channels(settings_.reverse_sound);
}

void AudioSystem::set_settings(const AudioSettings& s) {
    bool env_changed = s.environment != settings_.environment;
    bool mix_changed = s.mix_ahead != settings_.mix_ahead;
    settings_ = s;
    if (mix_changed && device_) {
        // EqSoundManager::SetMixAhead: the output opened again with the new buffer (the mixer and its voices stay).
        device_.reset();
        Mixer* m = mixer_.get();
        device_ = platform::AudioDevice::open([m](float* out, std::size_t frames) { m->render(out, frames); },
                                              mixer_->rate(), mix_ahead_frames(settings_.mix_ahead, mixer_->rate()));
    }
    apply_volumes();
    apply_room();
    if (env_changed) zone_.set_state(night_, environment_, !settings_.environment);
}

std::int32_t AudioSystem::room_type() const {
    return special_room_ ? *special_room_ : rooms_.for_environment(environment_);
}

void AudioSystem::apply_room() {
    RoomReverb r = miles_room_reverb(room_type());
    mixer_->set_reverb(settings_.reverb ? r.level : 0.0f, r.decay_s, r.damping, r.predelay_s);
}

void AudioSystem::set_listener_environment(std::int32_t environment) {
    // SetListenerEnvironmentLow / High / Outside: the room always; the emitters when the environment changed.
    bool changed = environment != environment_;
    environment_ = environment;
    if (changed) log::debug("audio: listener environment {}, room type {}", environment_, room_type());
    apply_room();
    if (changed) zone_.set_state(night_, environment_, !settings_.environment);
}

void AudioSystem::apply_weather() {
    // UpdateEmitterStates: the rain while raining and not indoors, the wind while windy and not indoors.
    weather_.set_enabled(0, raining_ && !indoors_ && !weather_.emitters()[0].wave.empty());
    weather_.set_enabled(1, windy_ && !indoors_ && !weather_.emitters()[1].wave.empty());
}

void AudioSystem::set_weather(bool raining, bool windy) {
    if (raining == raining_ && windy == windy_) return;
    raining_ = raining;
    windy_ = windy;
    log::debug("audio: raining {}, windy {}", raining_, windy_);
    apply_weather();
}

void AudioSystem::set_indoors(bool indoors) {
    if (indoors == indoors_) return;
    indoors_ = indoors;
    log::debug("audio: indoors {}", indoors_);
    apply_weather();
}

void AudioSystem::set_special_room(std::optional<std::int32_t> room) {
    special_room_ = room;
    log::debug("audio: special room {}", room ? *room : -1);
    apply_room();
}

void AudioSystem::music_selection(std::int32_t id, bool on) {
    if (!global_music_set_) {
        if (!library_->index_ready()) {
            pending_selections_.emplace_back(id, on); // applied by set_up_global_music
            return;
        }
        set_up_global_music();
    }
    if (on) music_.play(id, *this);
    else music_.stop(id, *this);
    const MusicEntry* p = music_.playing();
    log::debug("audio: music selection {} {}; playing {}", id, on ? "on" : "off",
               p ? std::to_string(p->id) + " " + p->file : std::string("none"));
}

std::shared_ptr<SoundFont> AudioSystem::font() {
    if (font_tried_) return font_;
    font_tried_ = true;
    std::optional<std::vector<std::byte>> bytes;
    if (!settings_.soundfont.empty()) bytes = read_file(settings_.soundfont);
    else bytes = game_.read("synthusr.sf2");
    if (bytes) font_ = SoundFont::load(*bytes);
    if (!font_)
        log::warn("audio: no SoundFont ({}); the XMI music is silent",
                  settings_.soundfont.empty() ? "synthusr.sf2" : settings_.soundfont);
    return font_;
}

void AudioSystem::set_zone(const std::string& short_name) {
    zone_.clear(*this);
    zone_ = ZoneEmitters();
    std::erase_if(xmi_, [&](const auto& kv) { return kv.first == zone_xmi_; });
    zone_xmi_.clear();
    // SetCurrentZone: the room presets back to their defaults, then the zone's row of the table.
    rooms_ = short_name.empty() ? RoomPresets{} : zone_room_presets(zone_rooms_, short_name);
    apply_room();
    waves_.zone.clear();
    for (auto& [id, l] : loops_)
        if (l.voice) mixer_->stop(l.voice);
    loops_.clear();
    placed_.clear();
    zone_name_ = short_name;
    // The waves of the zones before the last go, the global ones stay (LoadGlobalWaves keeps those for the session).
    library_->drop_idle_waves(global_wave_files_);
    if (short_name.empty()) return;
    // The global entries before the zone's (as LoadGlobalMusic ran at start): this waits for the archives' index,
    // long built by the time a zone is entered.
    set_up_global_music();

    auto exists = [this](const std::string& f) { return library_->exists(f); };
    // SetCurrentZone: "<zone>.emt" when it opens, else the old files.
    std::string emt = short_name + ".emt";
    ZoneSoundSet set;
    if (auto text = game_.read(emt)) {
        set = emitters_from_emt(assets::parse_emt(text_of(text)), exists, emt);
    } else {
        waves_.zone = assets::parse_sound_bank(text_of(game_.read(short_name + "_sndbnk.eff")));
        if (game_.exists(short_name + ".xmi")) zone_xmi_ = short_name + ".xmi";
        auto records = game_.read(short_name + "_sounds.eff");
        if (records)
            set = emitters_from_eff(assets::parse_sounds_eff(*records), waves_, mp3index_, zone_xmi_, exists,
                                    short_name + "_sounds.eff");
    }
    std::vector<std::string> preload;
    for (const auto& e : set.emitters)
        if (!e.wave.empty()) preload.push_back(e.wave);
    for (const auto& [id, name] : waves_.zone) preload.push_back(name);
    library_->preload(preload);
    std::vector<std::string> music_files = global_music_files_;
    for (const auto& m : set.music)
        if (!m.file.empty()) music_files.push_back(m.file);
    library_->set_resident_files(music_files);
    log::info("audio: {}: {} emitters, {} music entries", short_name, set.emitters.size(), set.music.size());
    zone_ = ZoneEmitters(std::move(set));
    zone_.set_music_manager(&music_, *this);
    zone_.set_state(night_, environment_, !settings_.environment);
}

void AudioSystem::set_listener(const glm::vec3& position, float angle) {
    // The original works in Miles space, z negated: SetListenerLocation's (y, x, -z), WavePlay's control z and
    // EmitterAdd / CreateOldEmitter's emitter z (each negated), so distances are the game's; this client keeps game z.
    listener_ = position;
    yaw_ = angle;
}

void AudioSystem::set_hour(int stored_hour) {
    // CDisplay::sub_0048a500 (every 500 ms): EmitterSetNight(false) while the stored hour is 5..18.
    bool night = !(stored_hour > 4 && stored_hour < 19);
    if (night == night_) return;
    night_ = night;
    zone_.set_state(night_, environment_, !settings_.environment);
}

const std::string* AudioSystem::wave_name(std::int32_t id) const { return waves_.find(id); }

MilesLevels AudioSystem::levels(const SoundControl& c) const {
    // SoundManager::SetListenerLocation faces the listener along (cos a, sin a, 0) with up (0, 0, -1), so its right
    // (up x face) is (sin a, -cos a, 0).
    glm::vec3 face(std::cos(yaw_), std::sin(yaw_), 0.0f);
    glm::vec3 right(std::sin(yaw_), -std::cos(yaw_), 0.0f);
    return miles_fast2d_levels(c.position - listener_, c.min_distance, c.max_distance, face, right);
}

void AudioSystem::place(std::uint32_t voice, const SoundControl& c) {
    MilesLevels l = levels(c);
    mixer_->set_levels(voice, l.left, l.right);
}

std::uint32_t AudioSystem::play_wave(const std::string& file, const SoundControl& c) {
    if (c.volume <= 0.0f) return 0;
    if (c.positional()) {
        // SoundAssistPlay: a 3D sound beyond its max distance does not start.
        glm::vec3 d = c.position - listener_;
        if (c.max_distance * c.max_distance <= glm::dot(d, d)) return 0;
    }
    // SoundAssistPlay: the first free instance of the control's pool, else nothing.
    std::erase_if(pooled_, [&](const Pooled& p) { return !mixer_->alive(p.voice); });
    auto busy = std::count_if(pooled_.begin(), pooled_.end(),
                              [&](const Pooled& p) { return p.positional == c.positional() && p.pool == c.pool; });
    if (busy >= pool_voices(c.pool, c.positional())) {
        log::debug("audio: {} not started: pool {} ({}) busy", file, c.pool, c.positional() ? "3D" : "2D");
        return 0;
    }
    auto pcm = library_->wave(file);
    if (!pcm) return 0; // still decoding (or missing): this play is skipped
    MilesLevels l = c.positional() ? levels(c) : MilesLevels{};
    float start[2] = {l.left, l.right};
    std::uint32_t v =
        mixer_->play(std::move(pcm), c.volume, c.fade_ms, c.loops, Bus::Effects, c.positional() ? start : nullptr);
    if (!v) return 0;
    ++played_;
    pooled_.push_back({v, c.positional(), c.pool});
    if (c.positional()) {
        placed_.push_back({v, c});
        mixer_->set_send(v, c.echo); // the 3D provider's sample reverb level
    }
    return v;
}

void AudioSystem::release(std::uint32_t voice, std::int32_t fade_ms) { mixer_->set_volume(voice, 0.0f, fade_ms); }

bool AudioSystem::alive(std::uint32_t voice) { return mixer_->alive(voice); }

std::uint32_t AudioSystem::play_music(const MusicEntry& entry, float volume) {
    // The zone's and the global music are read ahead on the library's worker (set_resident_files), ahead of the
    // zone's wave preloads; one asked for before that read is done is read here (or its read on the worker waited
    // for), as it always was.
    // A voice the mixer would refuse (an EMT row's gain of 0) is not built: its decoder was made and thrown away
    // every frame the emitter is in range.
    if (!(volume > 0.0f) || !std::isfinite(volume)) return 0;
    auto bytes_of = [&] { return library_->file(entry.file); };
    std::unique_ptr<Source> src;
    if (ends_with_nocase(entry.file, ".mp3")) {
        if (auto bytes = bytes_of()) src = make_mp3_source(std::move(bytes), entry.loops);
    } else if (ends_with_nocase(entry.file, ".xmi")) {
        auto it = xmi_.find(entry.file);
        if (it == xmi_.end()) {
            it = xmi_.emplace(entry.file, std::vector<assets::XmiSequence>{}).first;
            if (auto bytes = bytes_of()) {
                try {
                    it->second = assets::parse_xmi(*bytes);
                } catch (const std::exception& e) {
                    log::warn("audio: {}: {}", entry.file, e.what());
                }
            }
        }
        // AIL_init_sequence fails for an index past the file's sequences: no music.
        if (entry.track >= 0 && static_cast<std::size_t>(entry.track) < it->second.size())
            src = make_xmi_source(font(), it->second[static_cast<std::size_t>(entry.track)], entry.loops);
    }
    if (!src) return 0;
    std::uint32_t v = mixer_->play_source(std::move(src), volume, entry.fade_in_ms, Bus::Music);
    if (v) log::debug("audio: music {} track {}", entry.file, entry.track);
    return v;
}

void AudioSystem::update(std::uint32_t now_ms) {
    if (!global_music_set_ && library_->index_ready()) set_up_global_music();
    // The music level gates FadeIn (MusicManager +0x08 > 0); the mixer's buses do the rest.
    float music = settings_.mute ? 0.0f : level(settings_.music);
    music_.set_volume(music);
    music_.reap(*this);
    zone_.update(listener_, now_ms == 0 ? 1 : now_ms, 1.0f, music, *this);
    weather_.update(listener_, now_ms == 0 ? 1 : now_ms, 1.0f, music, *this);

    // Spawn loops (EqMobileEmitter: GiveTime with loop count 0 and no delay).
    for (auto& [id, l] : loops_) {
        glm::vec3 d = listener_ - l.position;
        bool in_range = l.wave != 0 && l.volume_percent > kSilentPercent && glm::dot(d, d) < l.max * l.max;
        if (!in_range) {
            if (l.voice) {
                release(l.voice, 0);
                l.voice = 0;
            }
            continue;
        }
        if (l.voice && mixer_->alive(l.voice)) {
            for (auto& p : placed_)
                if (p.voice == l.voice) p.control.position = l.position;
            continue;
        }
        const std::string* name = wave_name(l.wave);
        if (!name) continue;
        SoundControl c;
        c.loops = 0;
        c.position = l.position;
        c.min_distance = l.min;
        c.max_distance = l.max;
        if (l.volume_percent < kFullVolumePercent) c.volume *= option_volume_factor(l.volume_percent, true);
        l.voice = play_wave(*name, c);
    }

    // Without a device nothing pulls the mixer: advance it here so sounds still end on time.
    if (!device_ && self_advance_) {
        std::uint32_t elapsed = last_update_ms_ ? now_ms - last_update_ms_ : 0;
        auto frames = static_cast<std::size_t>(std::min<std::uint32_t>(elapsed, 250) * mixer_->rate() / 1000);
        scratch_.resize(frames * 2);
        if (frames) mixer_->render(scratch_.data(), frames);
    }
    last_update_ms_ = now_ms;

    std::erase_if(placed_, [&](const Placed& p) { return !mixer_->alive(p.voice); });
    for (const auto& p : placed_) place(p.voice, p.control);
    std::erase_if(wave_voices_, [&](const auto& w) { return !mixer_->alive(w.second); });
}

void AudioSystem::play_wave(std::int32_t id, const SoundControl& control) {
    const std::string* name = wave_name(id);
    if (!name) return;
    SoundControl c = control;
    // EqSoundManager::WavePlay: the reverb send times the Realism option (+0xfec).
    c.echo *= static_cast<float>(settings_.realism) / 100.0f;
    std::uint32_t v = play_wave(*name, c);
    if (v) wave_voices_.emplace_back(id, v);
    log::debug("audio: wave {} {}{}", id, *name, v ? "" : " (not started)");
}

void AudioSystem::play_group_2d(std::int32_t id, SoundGroup group) {
    SoundControl c;
    const int percent = settings_.individual[group];
    if (percent <= kSilentPercent) return;
    if (percent < kFullVolumePercent) c.volume *= option_volume_factor(percent, c.positional());
    play_wave(id, c);
}

void AudioSystem::play_named_2d(const std::string& name) {
    auto it = named_waves_.find(name);
    if (it == named_waves_.end()) return;
    SoundControl c;
    c.echo *= static_cast<float>(settings_.realism) / 100.0f;
    std::uint32_t v = play_wave(it->second, c);
    log::debug("audio: {} {}{}", name, it->second, v ? "" : " (not started)");
}

bool AudioSystem::wave_playing(std::int32_t id) {
    for (const auto& [w, v] : wave_voices_)
        if (w == id && mixer_->alive(v)) return true;
    return false;
}

const game::SpawnSounds& AudioSystem::spawn_sounds(std::int32_t race, std::uint8_t gender) {
    std::uint32_t key = (static_cast<std::uint32_t>(race) << 8) | gender;
    auto it = spawn_sounds_.find(key);
    if (it == spawn_sounds_.end()) {
        it = spawn_sounds_.emplace(key, game::spawn_sounds(race_ids_, race, gender)).first;
        // SetSounds preloads kinds 0..43's waves.
        std::vector<std::string> names;
        for (std::int32_t id : it->second.ids)
            if (const std::string* n = wave_name(id)) names.push_back(*n);
        library_->preload(names);
    }
    return it->second;
}

void AudioSystem::set_spawn_loop(std::uint32_t spawn_id, std::int32_t wave, const glm::vec3& position, float height,
                                 int volume_percent) {
    Loop& l = loops_[spawn_id];
    // A loop already sounding restarts at its new volume.
    if ((l.wave != wave || l.volume_percent != volume_percent) && l.voice) {
        release(l.voice, 0);
        l.voice = 0;
    }
    l.wave = wave;
    l.volume_percent = volume_percent;
    l.position = position;
    // EqMobileEmitter::SetNpcHeight @0x4a9b10: min max(h/2, 2), max and range min x 24.
    l.min = std::max(height * 0.5f, 2.0f);
    l.max = l.min * 24.0f;
    if (const std::string* name = wave != 0 ? wave_name(wave) : nullptr) library_->wave(*name);
}

void AudioSystem::drop_spawn_loops_except(const std::vector<std::uint32_t>& alive_ids) {
    for (auto it = loops_.begin(); it != loops_.end();) {
        if (std::find(alive_ids.begin(), alive_ids.end(), it->first) == alive_ids.end()) {
            if (it->second.voice) release(it->second.voice, 0);
            it = loops_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace hailstone::audio
