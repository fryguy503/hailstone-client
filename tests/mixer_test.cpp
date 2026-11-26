#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <mutex>
#include <thread>

#include "audio/audio_system.h"
#include "audio/individual_sounds.h"
#include "audio/mixer.h"
#include "audio/pcm.h"
#include "audio/sound_control.h"
#include "audio/sound_library.h"
#include "core/config.h"
#include "core/step_clock.h"
#include "game/weather.h"
#include "game_files.h"
#include "pfs_fixtures.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::audio;

namespace {

std::shared_ptr<const Pcm> constant(std::uint32_t rate, std::size_t frames, float value) {
    auto p = std::make_shared<Pcm>();
    p->rate = rate;
    p->channels = 1;
    p->samples.assign(frames, value);
    return p;
}

// A minimal 16-bit mono WAV.
std::vector<std::byte> wav16(std::uint32_t rate, const std::vector<std::int16_t>& s) {
    std::vector<std::byte> d;
    auto put = [&](const void* p, std::size_t n) {
        auto b = static_cast<const std::byte*>(p);
        d.insert(d.end(), b, b + n);
    };
    auto u32 = [&](std::uint32_t v) { put(&v, 4); };
    auto u16 = [&](std::uint16_t v) { put(&v, 2); };
    put("RIFF", 4);
    u32(static_cast<std::uint32_t>(36 + s.size() * 2));
    put("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    put("data", 4);
    u32(static_cast<std::uint32_t>(s.size() * 2));
    put(s.data(), s.size() * 2);
    return d;
}

void write_weather_waves(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir / "sounds");
    std::ofstream(dir / "soundassets.txt") << "159^rainloop.wav\n158^wind_lp1.wav\n";
    const auto wav = wav16(48000, {16384, 16384, 16384, 16384});
    for (const char* name : {"rainloop.wav", "wind_lp1.wav"})
        std::ofstream(dir / "sounds" / name, std::ios::binary)
            .write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
}

bool mix_is_audible(Mixer& mixer) {
    std::vector<float> out(2 * 512);
    mixer.render(out.data(), out.size() / 2);
    return std::any_of(out.begin(), out.end(), [](float sample) { return sample != 0.0f; });
}

} // namespace

TEST_CASE("Miles Fast 2D: inverse distance, linear pan, behind and mute at maximum") {
    const glm::vec3 face(1, 0, 0), right(0, -1, 0); // angle 0: facing +x, right -y
    // Straight ahead inside min: full gain split evenly.
    auto l = miles_fast2d_levels({1, 0, 0}, 5.0f, 80.0f, face, right);
    CHECK(l.left == doctest::Approx(0.5));
    CHECK(l.right == doctest::Approx(0.5));
    CHECK_FALSE(l.muted);
    // Ahead at 10: min / d.
    l = miles_fast2d_levels({10, 0, 0}, 5.0f, 80.0f, face, right);
    CHECK(l.left + l.right == doctest::Approx(0.5));
    // Hard right: all on the right; hard left: all on the left.
    l = miles_fast2d_levels({0, -2, 0}, 5.0f, 80.0f, face, right);
    CHECK(l.left == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(l.right == doctest::Approx(1.0));
    l = miles_fast2d_levels({0, 2, 0}, 5.0f, 80.0f, face, right);
    CHECK(l.left == doctest::Approx(1.0));
    // 45 degrees to the right: acos(cos 45) / pi = 0.25 to the left.
    l = miles_fast2d_levels({1, -1, 0}, 5.0f, 80.0f, face, right);
    CHECK(l.left == doctest::Approx(0.25));
    CHECK(l.right == doctest::Approx(0.75));
    // Behind: 0.75.
    l = miles_fast2d_levels({-1, 0, 0}, 5.0f, 80.0f, face, right);
    CHECK(l.left + l.right == doctest::Approx(0.75));
    // At the max distance still heard; past it muted.
    l = miles_fast2d_levels({80, 0, 0}, 5.0f, 80.0f, face, right);
    CHECK_FALSE(l.muted);
    CHECK(l.left + l.right == doctest::Approx(5.0 / 80.0));
    CHECK(miles_fast2d_levels({80.5f, 0, 0}, 5.0f, 80.0f, face, right).muted);
    // On the listener: centred.
    l = miles_fast2d_levels({0, 0, 0}, 5.0f, 80.0f, face, right);
    CHECK(l.left == doctest::Approx(0.5));
    CHECK(miles_volume_curve(0.5f) == doctest::Approx(0.31498));
    CHECK(miles_volume_curve(1.0f) == 1.0f);
    auto c = spawn_sound_control({0, 0, 0}, 6.0f, true);
    CHECK(c.min_distance == 3.0f);
    CHECK(c.max_distance == 72.0f);
    CHECK(c.volume == doctest::Approx(0.8));
}

TEST_CASE("WAV decoding: 16-bit mono at 22000 Hz") {
    auto pcm = decode_sound(wav16(22000, {0, 16384, -16384, 32767}));
    REQUIRE(pcm);
    CHECK(pcm->rate == 22000);
    CHECK(pcm->channels == 1);
    REQUIRE(pcm->frames() == 4);
    CHECK(pcm->samples[1] == doctest::Approx(0.5).epsilon(0.001));
    CHECK_FALSE(decode_wav(std::vector<std::byte>(10)));
}

TEST_CASE("WAV encoding: little-endian header and samples, read back as written") {
    const std::vector<float> stereo = {0.0f, 0.5f, -0.5f, 1.0f, 2.0f, -2.0f};
    const auto wav = encode_wav16(stereo, 48000, 2);
    REQUIRE(wav.size() == 44 + stereo.size() * 2);
    auto byte = [&](std::size_t i) { return std::to_integer<unsigned>(wav[i]); };
    CHECK(byte(4) == ((36 + 12) & 0xff)); // the RIFF size's low byte first, whatever the host's order
    CHECK(byte(24) == 0x80);              // 48000 = 0xbb80
    CHECK(byte(25) == 0xbb);
    CHECK(byte(32) == 4);                 // block align: 2 channels x 2 bytes
    CHECK(byte(46) == 0x00);              // the second sample, 0.5 -> 16384 = 0x4000, low byte first
    CHECK(byte(47) == 0x40);
    auto pcm = decode_wav(wav);
    REQUIRE(pcm);
    CHECK(pcm->rate == 48000);
    CHECK(pcm->channels == 2);
    REQUIRE(pcm->samples.size() == stereo.size());
    CHECK(pcm->samples[1] == doctest::Approx(0.5).epsilon(0.001));
    CHECK(pcm->samples[2] == doctest::Approx(-0.5).epsilon(0.001));
    CHECK(pcm->samples[4] == doctest::Approx(1.0).epsilon(0.001)); // clamped
    CHECK(pcm->samples[5] == doctest::Approx(-1.0).epsilon(0.001));
}

TEST_CASE("Mixer: volume, buses, loops and fades") {
    Mixer m(1000, 4);
    std::vector<float> out(200);
    auto id = m.play(constant(1000, 50, 0.5f), 0.5f, 0, 1, Bus::Effects);
    REQUIRE(id != 0);
    m.render(out.data(), 100);
    CHECK(out[0] == doctest::Approx(0.25));
    CHECK(out[1] == doctest::Approx(0.25));
    CHECK(out[2 * 60] == 0.0f); // past the end
    CHECK_FALSE(m.alive(id));

    // Looping forever, panned hard right, on a bus at half.
    m.set_bus_volume(Bus::Music, 0.5f);
    id = m.play(constant(1000, 10, 1.0f), 1.0f, 0, 0, Bus::Music);
    m.set_levels(id, 0.0f, 1.0f);
    m.render(out.data(), 100);
    CHECK(out[2 * 95] == 0.0f);                       // left silent
    CHECK(out[2 * 95 + 1] == doctest::Approx(0.5));   // right at the bus volume
    CHECK(m.alive(id));
    // A fade to 0 over 50 ms ends the voice; a 3D voice's volume goes through the 5/3 curve.
    m.set_volume(id, 0.0f, 50);
    m.render(out.data(), 100);
    CHECK(out[2 * 25 + 1] == doctest::Approx(0.5 * 0.31498).epsilon(0.05));
    CHECK_FALSE(m.alive(id));
    // Fade in from 0.
    id = m.play(constant(1000, 100, 1.0f), 1.0f, 100, 1, Bus::Effects);
    m.render(out.data(), 100);
    CHECK(out[0] < 0.05f);
    CHECK(out[2 * 50] == doctest::Approx(0.5).epsilon(0.05));
    // Master 0 (mute).
    m.set_master(0.0f);
    id = m.play(constant(1000, 100, 1.0f), 1.0f, 0, 1, Bus::Effects);
    m.render(out.data(), 10);
    CHECK(out[0] == 0.0f);
}

TEST_CASE("Mixer: a step longer than the sound counts every pass and stays in range") {
    // 2 frames at 10000 Hz played at 1000 Hz: each output frame passes the end five times.
    Mixer m(1000, 4);
    std::vector<float> out(20);
    auto id = m.play(constant(10000, 2, 1.0f), 1.0f, 0, 0, Bus::Effects);
    m.render(out.data(), 10);
    CHECK(out[2 * 9] == doctest::Approx(1.0));
    CHECK(m.alive(id));
    // Three plays end within the first output frames.
    id = m.play(constant(10000, 2, 1.0f), 1.0f, 0, 3, Bus::Effects);
    m.render(out.data(), 10);
    CHECK_FALSE(m.alive(id));
}

TEST_CASE("Mixer: /reversesound swaps the output's channels") {
    // RoF2 only stores ReverseSound (0xde0d48: Cmd_reversesound and the ini); the client swaps its whole output.
    Mixer m(1000, 4);
    std::vector<float> out(20);
    auto id = m.play(constant(1000, 100, 1.0f), 1.0f, 0, 0, Bus::Effects);
    m.set_levels(id, 0.0f, 1.0f); // hard right
    m.render(out.data(), 10);
    CHECK(out[0] == 0.0f);
    CHECK(out[1] == doctest::Approx(1.0));
    m.set_swap_channels(true);
    m.render(out.data(), 10);
    CHECK(out[0] == doctest::Approx(1.0));
    CHECK(out[1] == 0.0f);
    m.set_swap_channels(false);
    m.render(out.data(), 10);
    CHECK(out[0] == 0.0f);
}

namespace {
// A stream that counts its frames, at most `limit` of them, and the largest block it was asked for.
struct CountingSource final : Source {
    std::size_t at = 0, limit = 0, *largest = nullptr;
    std::size_t render(float* out, std::size_t frames, std::uint32_t) override {
        *largest = std::max(*largest, frames);
        std::size_t n = std::min(frames, limit - at);
        for (std::size_t f = 0; f < n; ++f, ++at) out[f * 2] = out[f * 2 + 1] = static_cast<float>(at % 100) / 200.0f;
        return n;
    }
};
} // namespace

TEST_CASE("Mixer: a long render is mixed in blocks, as several short ones would be") {
    // The device's callback may ask for any count; the mixer's buffers are made once, kBlockFrames long.
    constexpr std::size_t kFrames = Mixer::kBlockFrames * 2 + 123;
    std::size_t largest[2] = {0, 0};
    std::vector<float> whole(kFrames * 2), pieces(kFrames * 2);
    for (int run = 0; run < 2; ++run) {
        Mixer m(48000, 8);
        m.set_reverb(0.5f, 1.0f, 0.5f, 0.01f);
        auto id = m.play(constant(48000, kFrames, 0.25f), 1.0f, 0, 1, Bus::Effects);
        m.set_send(id, 0.5f);
        auto src = std::make_unique<CountingSource>();
        src->limit = kFrames;
        src->largest = &largest[run];
        m.play_source(std::move(src), 1.0f, 0, Bus::Music);
        if (run == 0) {
            m.render(whole.data(), kFrames);
        } else {
            for (std::size_t done = 0; done < kFrames; done += 1000)
                m.render(pieces.data() + done * 2, std::min<std::size_t>(1000, kFrames - done));
        }
    }
    CHECK(largest[0] == Mixer::kBlockFrames);
    bool same = true;
    for (std::size_t i = 0; i < whole.size(); ++i) same = same && whole[i] == doctest::Approx(pieces[i]).epsilon(1e-5);
    CHECK(same);
}

namespace {
// A stream whose render waits (at most two seconds, so a regression fails rather than hangs) for the test to let it
// go: an MP3 decode or a synthesizer taking its time on the audio thread.
struct GateSource final : Source {
    std::mutex* m = nullptr;
    std::condition_variable* cv = nullptr;
    bool* entered = nullptr;
    bool* released = nullptr;
    bool* saw_release = nullptr;
    bool* destroyed = nullptr;
    ~GateSource() override { *destroyed = true; }
    std::size_t render(float* out, std::size_t frames, std::uint32_t) override {
        std::unique_lock lock(*m);
        *entered = true;
        cv->notify_all();
        *saw_release = cv->wait_for(lock, std::chrono::seconds(2), [&] { return *released; });
        std::fill(out, out + frames * 2, 0.5f);
        return frames;
    }
};
} // namespace

TEST_CASE("Mixer: the game thread's calls do not wait for a source rendering on the audio thread") {
    std::mutex gate;
    std::condition_variable cv;
    bool entered = false, released = false, saw_release = false, destroyed = false;
    Mixer m(48000, 8);
    auto src = std::make_unique<GateSource>();
    src->m = &gate;
    src->cv = &cv;
    src->entered = &entered;
    src->released = &released;
    src->saw_release = &saw_release;
    src->destroyed = &destroyed;
    auto stream = m.play_source(std::move(src), 1.0f, 0, Bus::Music);
    REQUIRE(stream != 0);
    auto sound = m.play(constant(48000, 10000, 0.25f), 1.0f, 0, 1, Bus::Effects);
    std::vector<float> out(512 * 2);
    std::thread audio([&] { m.render(out.data(), 512); });
    {
        std::unique_lock lock(gate);
        cv.wait(lock, [&] { return entered; });
    }
    // While the source renders: these used to wait on the mixer's lock until it was done.
    CHECK(m.alive(stream));
    m.set_levels(sound, 0.5f, 0.5f);
    m.set_volume(sound, 0.8f, 0);
    CHECK(m.voice_count() == 2);
    // Stopping everything does not destroy the source under the audio thread.
    m.stop_all();
    CHECK_FALSE(destroyed);
    CHECK(m.play(constant(48000, 100, 0.25f), 1.0f, 0, 1, Bus::Effects) != 0);
    CHECK_FALSE(destroyed);
    {
        std::lock_guard lock(gate);
        released = true;
    }
    cv.notify_all();
    audio.join();
    CHECK(saw_release);
    CHECK_FALSE(m.alive(stream));
    // Stopped before its block was mixed: only the sound played meanwhile (0.25) is in it, not the stream's 0.5.
    CHECK(std::all_of(out.begin(), out.end(), [](float v) { return v <= 0.25f; }));
    // The next add reaps it, outside the lock.
    CHECK(m.play(constant(48000, 100, 0.25f), 1.0f, 0, 1, Bus::Effects) != 0);
    CHECK(destroyed);
}

TEST_CASE("Mixer: voice limit and resampling") {
    Mixer m(2000, 2);
    CHECK(m.play(constant(1000, 10, 1.0f), 1.0f, 0, 0, Bus::Effects) != 0);
    CHECK(m.play(constant(1000, 10, 1.0f), 1.0f, 0, 0, Bus::Effects) != 0);
    CHECK(m.play(constant(1000, 10, 1.0f), 1.0f, 0, 0, Bus::Effects) == 0);
    m.stop_all();
    // 10 frames at 1000 Hz last 20 frames at 2000 Hz.
    auto id = m.play(constant(1000, 10, 0.5f), 1.0f, 0, 1, Bus::Effects);
    std::vector<float> out(100);
    m.render(out.data(), 19);
    CHECK(m.alive(id));
    m.render(out.data(), 2);
    CHECK_FALSE(m.alive(id));
}

TEST_CASE("SoundLibrary: music files are read ahead on the worker, loose or in a snd*.pfs, and dropped when unnamed") {
    test::ScratchDir dir("sound_resident");
    auto bytes = [](const char* text) {
        std::vector<std::byte> b;
        for (const char* c = text; *c; ++c) b.push_back(static_cast<std::byte>(*c));
        return b;
    };
    std::filesystem::create_directories(dir.path() / "sounds");
    std::ofstream(dir.path() / "theme.mp3", std::ios::binary) << "loose mp3 bytes";
    auto pfs = test::pfs_fixtures::make_pfs({{"zone.xmi", bytes("archived xmi bytes")}});
    std::ofstream(dir.path() / "snd3.pfs", std::ios::binary)
        .write(reinterpret_cast<const char*>(pfs.data()), static_cast<std::streamsize>(pfs.size()));
    GameDir game(dir.path());
    SoundLibrary library(game);
    library.set_resident_files({"theme.mp3", "ZONE.XMI", "missing.mp3"});
    library.wait_idle();
    auto theme = library.resident_file("THEME.mp3");
    REQUIRE(theme);
    CHECK(*theme == bytes("loose mp3 bytes"));
    auto zone = library.resident_file("zone.xmi");
    REQUIRE(zone);
    CHECK(*zone == bytes("archived xmi bytes"));
    CHECK_FALSE(library.resident_file("missing.mp3"));
    CHECK_FALSE(library.resident_file("other.mp3"));
    // The next zone's list: what it does not name goes, what it keeps is not read again.
    library.set_resident_files({"theme.mp3"});
    library.wait_idle();
    CHECK(library.resident_file("theme.mp3") == theme);
    CHECK_FALSE(library.resident_file("zone.xmi"));
}

TEST_CASE("SoundLibrary: a music file is read ahead of a zone's preloads, behind a wave wanted now") {
    test::ScratchDir dir("sound_priority");
    std::filesystem::create_directories(dir.path() / "sounds");
    for (int i = 0; i < 20; ++i) std::ofstream(dir.path() / "sounds" / ("wave" + std::to_string(i) + ".wav")) << "x";
    std::ofstream(dir.path() / "theme.mp3", std::ios::binary) << "loose mp3 bytes";
    std::ofstream(dir.path() / "other.mp3", std::ios::binary) << "other mp3 bytes";
    GameDir game(dir.path());
    SoundLibrary library(game);
    std::vector<std::string> jobs;
    library.hold_worker(true);
    library.set_job_log(&jobs);
    // set_zone's order: the bulk preload, then the music files; a wave played meanwhile; a preload played.
    std::vector<std::string> preload;
    for (int i = 0; i < 20; ++i) preload.push_back("WAVE" + std::to_string(i) + ".wav");
    library.preload(preload);
    library.set_resident_files({"theme.mp3", "other.mp3"});
    library.set_resident_files({"theme.mp3", "other.mp3"}); // named again before it is read: queued once
    CHECK_FALSE(library.wave("wave7.wav"));
    // A queued file asked for now is read on the asking thread, not again on the worker.
    auto other = library.file("OTHER.mp3");
    REQUIRE(other);
    CHECK(library.resident_file("other.mp3") == other);
    library.hold_worker(false);
    library.wait_idle();
    library.set_job_log(nullptr);
    REQUIRE(jobs.size() == 21);
    CHECK(jobs[0] == "wave7.wav");
    CHECK(jobs[1] == "theme.mp3");
    CHECK(jobs[2] == "wave0.wav");
    CHECK(std::count(jobs.begin(), jobs.end(), "wave7.wav") == 1);
    CHECK(std::count(jobs.begin(), jobs.end(), "other.mp3") == 0);
    CHECK(library.resident_file("theme.mp3"));
    CHECK(library.file("theme.mp3") == library.resident_file("theme.mp3"));
}

TEST_CASE("Audio system without a device: zone emitters and sounds from the game") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    AudioSystem audio(*game, AudioSettings{}, false);
    CHECK_FALSE(audio.has_device());
    audio.set_zone("gfaydark");
    CHECK(audio.zone().emitters().size() > 19);
    audio.library().wait_idle();
    CHECK(audio.library().archive_files() > 1000);
    // An archive-only ambient sound (snd6.pfs) decodes.
    audio.library().preload({"darkwds1.wav"});
    audio.library().wait_idle();
    auto pcm = audio.library().wave("darkwds1.wav");
    REQUIRE(pcm);
    CHECK(pcm->frames() > 1000);
    // Level up (id 0x8b) resolves through soundassets.txt and plays once decoded.
    audio.play_wave_2d(game::kSoundLevelUp);
    audio.library().wait_idle();
    audio.play_wave_2d(game::kSoundLevelUp);
    CHECK(audio.wave_playing(game::kSoundLevelUp));
    // Updating with the listener in the zone starts some emitters (offline).
    for (std::uint32_t t = 1; t < 50; ++t) {
        audio.set_listener({0, 0, 0}, 0.0f);
        audio.update(t * 20);
        audio.library().wait_idle();
    }
    std::vector<float> out(2 * 512);
    audio.mixer().render(out.data(), 512);
    audio.set_zone("");
}

TEST_CASE("Audio system: voice pools, rain and wind emitters") {
    const GameDir* game = test::game_dir();
    if (!game) return;
    AudioSystem audio(*game, AudioSettings{}, false);
    audio.set_self_advance(false);
    audio.library().preload({"LevelUp.WAV"});
    audio.library().wait_idle();
    // The local player's pool (3) has two 3D voices: a third sound at once does not start.
    for (int i = 0; i < 3; ++i) {
        SoundControl c = spawn_sound_control({0, 0, 0}, 6.0f, true);
        CHECK(c.pool == 3);
        audio.play_wave(game::kSoundLevelUp, c);
    }
    CHECK(audio.mixer().voice_count() == 2);
    // Pool 1 has no 3D voices at all.
    SoundControl p1 = location_sound_control({0, 0, 0});
    p1.pool = 1;
    audio.play_wave(game::kSoundLevelUp, p1);
    CHECK(audio.mixer().voice_count() == 2);
    // The rain emitter is on while raining outdoors, off indoors. (The NMS install's sounds\rainloop.wav and
    // wind_lp1.wav are 80-byte stubs without samples, so nothing is heard there, as in the original.)
    CHECK(audio.weather_emitters().emitters().size() == 2);
    CHECK(audio.weather_emitters().emitters()[0].wave == "rainloop.wav");
    CHECK(audio.weather_emitters().emitters()[1].wave == "wind_lp1.wav");
    CHECK_FALSE(audio.weather_emitters().enabled(0));
    audio.set_weather(true, false);
    CHECK(audio.weather_emitters().enabled(0));
    CHECK_FALSE(audio.weather_emitters().enabled(1));
    audio.update(100);
    audio.set_indoors(true);
    CHECK_FALSE(audio.weather_emitters().enabled(0));
    audio.set_indoors(false);
    audio.set_weather(false, true);
    CHECK(audio.weather_emitters().enabled(1));
}

TEST_CASE("Audio system: unloading a zone stops rain and wind loops until new weather") {
    test::ScratchDir dir("weather_audio_unload");
    write_weather_waves(dir.path());
    GameDir game(dir.path());

    auto check_unload = [&](bool rain, bool fading) {
        CAPTURE(rain);
        AudioSystem audio(game, AudioSettings{}, false);
        audio.set_self_advance(false);
        audio.set_zone("greatdivide");
        audio.library().wait_idle();
        audio.set_weather(rain, !rain);
        audio.update(10);
        REQUIRE(audio.mixer().voice_count() == 1);
        REQUIRE(mix_is_audible(audio.mixer()));

        if (fading) {
            audio.set_weather(false, false);
            audio.update(20);
            REQUIRE(audio.mixer().voice_count() == 1);
            REQUIRE(mix_is_audible(audio.mixer()));
        }

        audio.set_zone("");
        CHECK(audio.mixer().voice_count() == 0);
        audio.update(30);
        CHECK_FALSE(mix_is_audible(audio.mixer()));

        audio.set_zone("eastwastes");
        audio.update(40);
        CHECK_FALSE(mix_is_audible(audio.mixer()));
        audio.set_weather(rain, !rain);
        audio.update(50);
        CHECK(audio.mixer().voice_count() == 1);
        CHECK(mix_is_audible(audio.mixer()));
    };

    SUBCASE("active loops") {
        check_unload(true, false);
        check_unload(false, false);
    }
    SUBCASE("loops fading after the weather clears") {
        check_unload(true, true);
        check_unload(false, true);
    }
}

TEST_CASE("Audio system: clearing rain or snow fades its loop while staying in the zone") {
    test::ScratchDir dir("weather_audio_clear");
    write_weather_waves(dir.path());
    GameDir game(dir.path());

    for (const bool snow : {false, true}) {
        CAPTURE(snow);
        AudioSystem audio(game, AudioSettings{}, false);
        audio.set_zone("greatdivide");
        audio.library().wait_idle();
        game::Weather weather;
        core::StepClock clock;
        std::uint32_t now_ms = 100;
        // An unfocused client can tick at 10 Hz with no graphics frames.
        auto advance_ticks = [&](int count) {
            for (int i = 0; i < count; ++i, now_ms += 100) {
                for (const auto& step : clock.advance(static_cast<std::uint64_t>(now_ms) * 1000))
                    weather.update(static_cast<double>(step.ms) / 1000.0, true, [] { return 50; });
                audio.set_weather(weather.raining(), weather.windy());
                audio.update(now_ms);
            }
        };
        weather.handle_message(snow ? 1 : 0, 10);
        advance_ticks(14);
        REQUIRE(weather.raining() == !snow);
        REQUIRE(weather.windy() == snow);
        REQUIRE(audio.mixer().voice_count() == 1);
        REQUIRE(mix_is_audible(audio.mixer()));

        weather.handle_message(0, 0); // EQEmu's clear command uses a zero-filled packet for either kind.
        advance_ticks(10);
        CHECK_FALSE(weather.raining());
        CHECK_FALSE(weather.windy());
        CHECK(audio.mixer().voice_count() == 1); // normal three-second audio fade after weather clears
        CHECK(mix_is_audible(audio.mixer()));

        advance_ticks(35);
        CHECK(audio.mixer().voice_count() == 0);
        CHECK_FALSE(mix_is_audible(audio.mixer()));
    }
}

TEST_CASE("Audio settings: SoundMixAhead (SoundManager::SetMixAhead's 4..96 fragments of 8 ms)") {
    CHECK(mix_ahead_ms(8) == 64);
    CHECK(mix_ahead_ms(1) == 32);   // below 4: 4
    CHECK(mix_ahead_ms(100) == 768); // above 96: 96
    CHECK(mix_ahead_frames(8, 48000) == 3072);
    Config c;
    CHECK(load_audio_settings(c).mix_ahead == 8);
    c.set_int("audio.mix_ahead", 0); // loadOptions: outside 1..100 is 8
    CHECK(load_audio_settings(c).mix_ahead == 8);
    c.set_int("audio.mix_ahead", 20);
    AudioSettings s = load_audio_settings(c);
    CHECK(s.mix_ahead == 20);
    Config out;
    store_audio_settings(s, out);
    CHECK(out.get_int("audio.mix_ahead", 0) == 20);
}

TEST_CASE("Audio settings: the individual sound volumes (audio.own_footsteps ... audio.equip)") {
    constexpr int kPetSteps = 35, kTooLoud = 400, kBelowZero = -3;
    // One entry for each group but None and Count, with a name of its own, and every volume starts at full.
    std::set<std::string_view> names;
    std::set<SoundGroup> groups;
    for (const IndividualSound& sound : individual_sounds()) {
        CHECK(names.insert(sound.name).second);
        CHECK(groups.insert(sound.group).second);
        CHECK(sound.group != SoundGroup::None);
        CHECK(sound.group != SoundGroup::Count);
        CHECK_FALSE(sound.tip.empty());
        CHECK(find_individual_sound(sound.name) == &sound);
        CHECK(find_individual_sound(sound.group) == &sound);
    }
    constexpr std::size_t kGroupsWithoutNone = kSoundGroupCount - 1;
    CHECK(names.size() == kGroupsWithoutNone);
    CHECK(find_individual_sound("npc_hits") == nullptr);
    Config c;
    AudioSettings s = load_audio_settings(c);
    for (const IndividualSound& sound : individual_sounds()) CHECK(s.individual[sound.group] == kFullVolumePercent);

    c.set_int("audio.pet_footsteps", kPetSteps);
    c.set_int("audio.thunder", kTooLoud);  // clamped to 0..100
    c.set_int("audio.money", kBelowZero);
    s = load_audio_settings(c);
    CHECK(s.individual[SoundGroup::PetFootsteps] == kPetSteps);
    CHECK(s.individual[SoundGroup::Thunder] == kFullVolumePercent);
    CHECK(s.individual[SoundGroup::Money] == kSilentPercent);
    CHECK(s.individual[SoundGroup::OwnFootsteps] == kFullVolumePercent);
    CHECK(s.individual[SoundGroup::None] == kFullVolumePercent);

    Config out;
    store_audio_settings(s, out);
    CHECK(out.get_int("audio.pet_footsteps", -1) == kPetSteps);
    CHECK(out.get_int("audio.money", -1) == kSilentPercent);
    CHECK(out.get_int("audio.own_footsteps", -1) == kFullVolumePercent);
    CHECK(out.get_int("audio.equip", -1) == kFullVolumePercent);
}

TEST_CASE("Audio settings: audio.npc_hits and the volume factor of a volume option") {
    Config c;
    CHECK(load_audio_settings(c).npc_hits == 100);
    c.set_int("audio.npc_hits", 150);
    CHECK(load_audio_settings(c).npc_hits == 100); // clamped to 0..100
    c.set_int("audio.npc_hits", 40);
    AudioSettings s = load_audio_settings(c);
    CHECK(s.npc_hits == 40);
    Config out;
    store_audio_settings(s, out);
    CHECK(out.get_int("audio.npc_hits", 0) == 40);
    // A 2D sound's volume is its amplitude; a positional one's goes through the 5/3 curve, so the factor undoes it.
    CHECK(option_volume_factor(100, false) == 1.0f);
    CHECK(option_volume_factor(50, false) == doctest::Approx(0.5f));
    CHECK(option_volume_factor(0, true) == 0.0f);
    CHECK(option_volume_factor(-5, true) == 0.0f);
    CHECK(option_volume_factor(100, true) == doctest::Approx(1.0f));
    for (int p : {10, 25, 50, 75})
        CHECK(miles_volume_curve(option_volume_factor(p, true)) == doctest::Approx(static_cast<float>(p) / 100.0f));
}

TEST_CASE("Mixer: a NaN or infinite volume or sample never reaches the reverb or the device") {
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    std::vector<float> out(2 * 4800);
    auto all_finite = [&] { return std::all_of(out.begin(), out.end(), [](float x) { return std::isfinite(x); }); };
    Mixer m(48000, 8);
    m.set_reverb(0.5f, 1.0f, 0.5f, 0.01f);
    // An EMT gain of "nan" or "inf" (strtof): refused, with or without a fade.
    const float levels[2] = {1.0f, 1.0f};
    CHECK(m.play(constant(48000, 4800, 0.5f), nan, 0, 0, Bus::Effects, levels) == 0);
    CHECK(m.play(constant(48000, 4800, 0.5f), inf, 0, 0, Bus::Effects, levels) == 0);
    CHECK(m.play(constant(48000, 4800, 0.5f), nan, 100, 0, Bus::Effects, levels) == 0);
    // A playing positional voice with a send, its volume, levels and send set non-finite: silent, not NaN.
    auto id = m.play(constant(48000, 4800, 0.5f), 1.0f, 0, 0, Bus::Effects, levels);
    REQUIRE(id != 0);
    m.set_send(id, nan);
    m.set_levels(id, inf, nan);
    m.set_volume(id, inf, 50);
    m.render(out.data(), 4800);
    CHECK(all_finite());
    m.stop_all();
    // A NaN sample (an IEEE-float WAV): it does not poison the reverb, so a later voice still sounds.
    auto bad = std::make_shared<Pcm>();
    bad->rate = 48000;
    bad->channels = 1;
    bad->samples.assign(480, nan);
    id = m.play(bad, 1.0f, 0, 1, Bus::Effects, levels);
    REQUIRE(id != 0);
    m.set_send(id, 1.0f);
    m.render(out.data(), 4800);
    CHECK(all_finite());
    id = m.play(constant(48000, 4800, 0.5f), 1.0f, 0, 0, Bus::Effects, levels);
    m.set_send(id, 1.0f);
    m.render(out.data(), 4800);
    CHECK(all_finite());
    CHECK(out[2 * 4000] != 0.0f);
    // A reverb configured from non-finite numbers stays finite.
    m.set_reverb(0.5f, nan, inf, nan);
    m.render(out.data(), 4800);
    CHECK(all_finite());
}

TEST_CASE("WAV decoding: an IEEE-float WAV's NaN and infinite samples are silence") {
    const float samples[4] = {0.25f, std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::infinity(),
                              -0.5f};
    std::vector<std::byte> d;
    auto put = [&](const void* p, std::size_t n) {
        auto b = static_cast<const std::byte*>(p);
        d.insert(d.end(), b, b + n);
    };
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::byte>(v >> (8 * i)));
    };
    auto u16 = [&](std::uint16_t v) {
        d.push_back(static_cast<std::byte>(v & 0xff));
        d.push_back(static_cast<std::byte>(v >> 8));
    };
    put("RIFF", 4);
    u32(36 + 16);
    put("WAVEfmt ", 8);
    u32(16);
    u16(3); // WAVE_FORMAT_IEEE_FLOAT
    u16(1);
    u32(22050);
    u32(22050 * 4);
    u16(4);
    u16(32);
    put("data", 4);
    u32(16);
    for (float s : samples) {
        std::uint32_t bits;
        std::memcpy(&bits, &s, 4);
        u32(bits);
    }
    auto pcm = decode_wav(d);
    REQUIRE(pcm);
    CHECK(pcm->samples == std::vector<float>{0.25f, 0.0f, 0.0f, -0.5f});
}

TEST_CASE("SoundLibrary: a snd<N>.pfs whose number does not fit an int is not one of the set, and does not throw") {
    test::ScratchDir dir("sound_bignum");
    std::ofstream(dir.path() / "snd99999999999.pfs", std::ios::binary) << "x";
    std::ofstream(dir.path() / "snd-1.pfs", std::ios::binary) << "x";
    GameDir game(dir.path());
    CHECK_NOTHROW(SoundLibrary{game});
}

TEST_CASE("SoundLibrary: a music file that failed to read is not read again") {
    // A music emitter in range asks for its file every frame while no voice plays it.
    test::ScratchDir dir("sound_missing");
    std::ofstream(dir.path() / "theme.mp3", std::ios::binary) << "x";
    GameDir game(dir.path()); // indexed with the file
    SoundLibrary library(game);
    std::filesystem::remove(dir.path() / "theme.mp3");
    CHECK_FALSE(library.file("theme.mp3"));
    std::ofstream(dir.path() / "theme.mp3", std::ios::binary) << "readable now";
    CHECK_FALSE(library.file("theme.mp3")); // remembered, not read
}

TEST_CASE("SoundLibrary: decoded waves not asked for over two zone changes are dropped, unless pinned or playing") {
    test::ScratchDir dir("sound_trim");
    std::filesystem::create_directories(dir.path() / "sounds");
    auto wav = wav16(22050, {1, 2, 3, 4});
    for (const char* n : {"old.wav", "global.wav", "held.wav", "used.wav"})
        std::ofstream(dir.path() / "sounds" / n, std::ios::binary)
            .write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
    GameDir game(dir.path());
    SoundLibrary library(game);
    library.preload({"old.wav", "global.wav", "held.wav", "used.wav"});
    library.wait_idle();
    REQUIRE(library.cached_waves() == 4);
    auto held = library.wave("held.wav"); // a voice still playing it
    REQUIRE(held);
    library.drop_idle_waves({"global.wav"}); // into a zone: everything was asked for in the last one
    CHECK(library.cached_waves() == 4);
    CHECK(library.wave("used.wav"));
    held = library.wave("held.wav");
    library.drop_idle_waves({"global.wav"}); // the next zone: old.wav was not asked for in the one before
    CHECK(library.cached_waves() == 3);
    library.drop_idle_waves({"global.wav"});
    // used.wav is idle now too; held.wav still plays, global.wav is pinned.
    CHECK(library.cached_waves() == 2);
    CHECK(library.wave("global.wav"));
    CHECK(library.wave("held.wav") == held);
    // A dropped wave asked for again is decoded again.
    CHECK_FALSE(library.wave("old.wav"));
    library.wait_idle();
    CHECK(library.wave("old.wav"));
}

TEST_CASE("Spawn sounds: a spawn beyond the reach of its steps and loop is out of hearing") {
    // Height 6: steps to 72, the loop to 72; height 2: steps to 24, the loop to 48 (its minimum 2 x 24).
    CHECK_FALSE(spawn_out_of_hearing({0, 0, 0}, {72, 0, 0}, 6.0f, 0.0f));
    CHECK(spawn_out_of_hearing({0, 0, 0}, {72.5f, 0, 0}, 6.0f, 0.0f));
    CHECK_FALSE(spawn_out_of_hearing({0, 0, 0}, {0, 48, 0}, 2.0f, 0.0f));
    CHECK(spawn_out_of_hearing({0, 0, 0}, {0, 49, 0}, 2.0f, 0.0f));
    CHECK_FALSE(spawn_out_of_hearing({10, 10, 10}, {10, 10, 130}, 6.0f, 50.0f));
    CHECK(spawn_out_of_hearing({0, 0, 0}, {std::nanf(""), 0, 0}, 6.0f, 50.0f));
}

TEST_CASE("Mixer: the reverb's lines made outside the lock: a new pre-delay sounds, and the same room keeps its tail") {
    std::vector<float> out(2 * 4800);
    Mixer m(48000, 8);
    // A short click into the room, then silence: what follows is the reverb's tail.
    m.set_reverb(1.0f, 2.0f, 0.2f, 0.01f);
    auto id = m.play(constant(48000, 480, 0.5f), 1.0f, 0, 1, Bus::Effects);
    REQUIRE(id != 0);
    m.set_send(id, 1.0f);
    m.render(out.data(), 4800);
    // The same room again (another decay): the tail goes on, the lines are not made again.
    m.set_reverb(1.0f, 3.0f, 0.2f, 0.01f);
    m.render(out.data(), 4800);
    auto energy = [&] {
        float e = 0.0f;
        for (float x : out) e += x * x;
        return e;
    };
    CHECK(energy() > 0.0f);
    // A longer pre-delay: a new pre-delay line (the combs keep their tail); a new click is heard through it.
    m.set_reverb(1.0f, 2.0f, 0.2f, 0.05f);
    m.render(out.data(), 4800);
    CHECK(std::all_of(out.begin(), out.end(), [](float x) { return std::isfinite(x); }));
    id = m.play(constant(48000, 480, 0.5f), 1.0f, 0, 1, Bus::Effects);
    REQUIRE(id != 0);
    m.set_send(id, 1.0f);
    m.render(out.data(), 4800);
    std::fill(out.begin(), out.end(), 0.0f);
    m.render(out.data(), 4800);
    CHECK(energy() > 0.0f);
    CHECK(std::all_of(out.begin(), out.end(), [](float x) { return std::isfinite(x); }));
    // Off and on again keeps working.
    m.set_reverb(0.0f, 2.0f, 0.2f, 0.05f);
    m.render(out.data(), 4800);
    m.set_reverb(0.5f, 1.0f, 0.5f, 0.0f);
    m.render(out.data(), 4800);
    CHECK(std::all_of(out.begin(), out.end(), [](float x) { return std::isfinite(x); }));
}

TEST_CASE("Mixer: finished voices are reaped in one pass, the live ones kept in order") {
    std::vector<float> out(2 * 512);
    Mixer m(48000, 16);
    std::vector<std::uint32_t> ids;
    for (int i = 0; i < 16; ++i) ids.push_back(m.play(constant(48000, 48000, 0.01f), 1.0f, 0, 0, Bus::Effects));
    for (auto id : ids) REQUIRE(id != 0);
    CHECK(m.play(constant(48000, 48000, 0.01f), 1.0f, 0, 0, Bus::Effects) == 0); // full
    // Every other one stopped: the next play reaps them all and finds room.
    for (std::size_t i = 0; i < ids.size(); i += 2) m.stop(ids[i]);
    m.render(out.data(), 512);
    CHECK(m.voice_count() == 8);
    std::vector<std::uint32_t> more;
    for (int i = 0; i < 8; ++i) more.push_back(m.play(constant(48000, 48000, 0.01f), 1.0f, 0, 0, Bus::Effects));
    for (auto id : more) CHECK(id != 0);
    CHECK(m.voice_count() == 16);
    for (std::size_t i = 1; i < ids.size(); i += 2) CHECK(m.alive(ids[i]));
    for (std::size_t i = 0; i < ids.size(); i += 2) CHECK_FALSE(m.alive(ids[i]));
}

TEST_CASE("Audio system: the global music is set once the sound archives are indexed; a selection made before stays") {
    test::ScratchDir dir("audio_global_music");
    std::filesystem::create_directories(dir.path() / "sounds");
    std::ofstream(dir.path() / "eqtheme.mp3", std::ios::binary) << "not an mp3";
    auto pfs = test::pfs_fixtures::make_pfs({{"gl.xmi", std::vector<std::byte>(16, std::byte{0})}});
    std::ofstream(dir.path() / "snd2.pfs", std::ios::binary)
        .write(reinterpret_cast<const char*>(pfs.data()), static_cast<std::streamsize>(pfs.size()));
    GameDir game(dir.path());
    AudioSystem audio(game, AudioSettings{}, false);
    // The login screen's theme, asked for at once: kept until the entries are set if the index is still being built.
    audio.music_selection(kMusicLogin, true);
    audio.library().wait_idle(); // the index is built
    audio.update(20);
    CHECK(audio.global_music_ready());
    const MusicEntry* playing = audio.music().playing();
    REQUIRE(playing);
    CHECK(playing->id == kMusicLogin);
    CHECK(playing->file == "eqtheme.mp3");
    // gl.xmi, found only in the archive, gives the other entries theirs.
    audio.music_selection(kMusicBank, true);
    audio.music_selection(kMusicLogin, false);
    playing = audio.music().playing();
    REQUIRE(playing);
    CHECK(playing->id == kMusicBank);
    CHECK(playing->file == "gl.xmi");
}

// MQ's /beep with no sound is Windows' Beep(0x500, 250): the tone the Beep sound "Tone" plays (app/client_app_options.cpp).
TEST_CASE("Audio: make_tone gives a 1280 Hz tone of 250 ms, faded at both ends") {
    constexpr std::uint32_t kRate = 48000, kMs = 250, kFadeMs = 5;
    constexpr double kHz = 1280.0;
    constexpr float kAmplitude = 0.5f;
    const Pcm p = make_tone(kRate, kHz, kMs, kFadeMs, kAmplitude);
    CHECK(p.rate == kRate);
    CHECK(p.channels == 1);
    REQUIRE(p.frames() == kRate * kMs / 1000);
    CHECK(p.samples.front() == 0.0f);                   // faded in from silence
    CHECK(std::fabs(p.samples.back()) < 0.01f);         // and out to it
    float peak = 0.0f;
    int crossings = 0;
    for (std::size_t i = 0; i < p.samples.size(); ++i) {
        peak = std::max(peak, std::fabs(p.samples[i]));
        if (i > 0 && (p.samples[i - 1] < 0.0f) != (p.samples[i] < 0.0f)) ++crossings;
    }
    CHECK(peak <= kAmplitude);
    CHECK(peak > kAmplitude * 0.99f);
    // Two sign changes a cycle: 1280 cycles a second for a quarter of one.
    CHECK(std::abs(crossings - static_cast<int>(2 * kHz * kMs / 1000)) <= 2);
}

TEST_CASE("Audio system: /beep's sounds, a game folder's named sound or the tone, at the Beep volume") {
    test::ScratchDir dir("audio_beep");
    std::filesystem::create_directories(dir.path() / "sounds");
    std::ofstream(dir.path() / "soundassets.txt")
        << "YouveGotMail^mail.wav\nAlertBell^bell.wav\n1^Unknown\n139^LevelUp.WAV\n140^levelup.wav\n142^Button_1.WAV\n";
    GameDir game(dir.path());
    AudioSystem audio(game, AudioSettings{}, false);
    audio.set_self_advance(false);
    // The Beep sound's choices after the tone: the named sounds, sorted.
    CHECK(audio.named_sounds() == std::vector<std::string>{"AlertBell", "YouveGotMail"});
    // A named sound by its name in any case, or by its wave file; a name the game folder lacks plays nothing.
    CHECK(audio.play_named_group_2d("youvegotmail", SoundGroup::Beep));
    CHECK(audio.play_named_group_2d("BELL.WAV", SoundGroup::Beep));
    CHECK_FALSE(audio.play_named_group_2d("NoSuchSound", SoundGroup::Beep));
    // A numbered entry's wave file too, in any case; "Unknown" is no sound.
    CHECK(audio.play_named_group_2d("levelup.wav", SoundGroup::Beep));
    CHECK_FALSE(audio.play_named_group_2d("Unknown", SoundGroup::Beep));
    // Every file once (in any case), sorted in any case, without "Unknown": the Beep sound's search.
    CHECK(audio.sound_files() == std::vector<std::string>{"bell.wav", "Button_1.WAV", "LevelUp.WAV", "mail.wav"});
    // The tone is a voice on the mixer at once (no file to decode), played once: gone after its 250 ms.
    const std::size_t before = audio.mixer().voice_count();
    audio.play_tone_2d(1280.0, 250, SoundGroup::Beep);
    CHECK(audio.mixer().voice_count() == before + 1);
    const std::size_t frames = audio.mixer().rate() * 300 / 1000; // past the tone's end
    std::vector<float> out(2 * frames);
    audio.mixer().render(out.data(), frames);
    CHECK(audio.mixer().voice_count() == before);
    // At a Beep volume of 0 nothing plays.
    AudioSettings quiet;
    quiet.individual[SoundGroup::Beep] = kSilentPercent;
    AudioSystem silent(game, quiet, false);
    silent.set_self_advance(false);
    const std::size_t none = silent.mixer().voice_count();
    silent.play_tone_2d(1280.0, 250, SoundGroup::Beep);
    CHECK(silent.mixer().voice_count() == none);
}
