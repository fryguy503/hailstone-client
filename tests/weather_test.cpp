#include <doctest/doctest.h>

#include "game/weather.h"

using namespace hailstone::game;

namespace {
// Random() and rand(): 50 % 100 + 1 > 50 (thunder2), 50 % 100 >= 30 (no second strike), 50 % 10000 + 10000 ms.
std::int32_t fifty() { return 50; }
constexpr double kFrame = 0.01; // 100 frames a second
} // namespace

TEST_CASE("weather: OP_Weather fades the rain in and out at 0.25 a second") {
    Weather w;
    w.update(0.0, true, fifty);
    CHECK(w.kind() == WeatherKind::None);
    w.handle_message(0, 10); // rain, intensity 10: 0.10
    CHECK(w.kind() == WeatherKind::None); // pending until the next frame
    w.update(0.1, true, fifty); // starts: +0xc sets the manager's clock to this frame, so nothing moves yet
    CHECK(w.kind() == WeatherKind::Rain);
    CHECK(w.precipitation() == 0.0f);
    w.update(0.3, true, fifty);
    CHECK(w.precipitation() == doctest::Approx(0.05f));
    w.update(1.0, true, fifty);
    CHECK(w.precipitation() == doctest::Approx(0.10f));
    CHECK(w.storm());

    // Kind 0xff: the current kind at intensity 0, a fade out.
    w.handle_message(0xff, 0x31);
    w.update(1.2, true, fifty);
    CHECK(w.precipitation() == doctest::Approx(0.10f));
    w.update(1.4, true, fifty);
    CHECK(w.precipitation() == doctest::Approx(0.05f));
    w.update(2.0, true, fifty);
    CHECK(w.precipitation() == 0.0f);
    CHECK_FALSE(w.storm());
}

TEST_CASE("weather: another kind replaces the current one at once") {
    Weather w;
    w.handle_message(0, 5);
    w.update(0.0, true, fifty);
    w.update(1.0, true, fifty);
    CHECK(w.precipitation() == doctest::Approx(0.05f));
    w.handle_message(1, 8); // snow
    w.update(1.0, true, fifty);
    CHECK(w.kind() == WeatherKind::Snow);
    // stop(1, 1) (0x1007fa60) takes the particles away and leaves the intensity, which moves on toward 0.08.
    CHECK(w.precipitation() == doctest::Approx(0.05f));
    w.update(2.0, true, fifty);
    CHECK(w.precipitation() == doctest::Approx(0.08f));
}

TEST_CASE("weather: the rain sound starts after the 64-frame fade in and stops after the fade out") {
    Weather w;
    w.handle_message(0, 10);
    double t = 0.0;
    std::uint32_t message = 0;
    int frames = 0;
    for (; !w.raining() && frames < 200; ++frames, t += kFrame) {
        w.update(t, true, fifty, 1);
        message = message ? message : w.message();
    }
    CHECK(frames == 66); // the message's frame (it starts at the frame's end), 64 steps of 4, the frame turning it on
    CHECK(message == 0x21d);
    CHECK_FALSE(w.windy());

    w.handle_message(0xff, 0);
    frames = 0;
    for (; w.raining() && frames < 200; ++frames, t += kFrame) w.update(t, true, fifty, 1);
    CHECK(frames == 45); // the message's frame, 43 steps of 6 from 0, the frame turning it off
    CHECK(w.message() == 0);

    // Zone 0x48 has its own start; snow is the wind.
    Weather other;
    other.handle_message(0, 10);
    for (int i = 0; i < 70; ++i) {
        other.update(i * kFrame, true, fifty, 0x48);
        if (other.message()) CHECK(other.message() == 0x21c);
    }
    Weather snow;
    snow.handle_message(1, 10);
    for (int i = 0; i < 70; ++i) snow.update(i * kFrame, true, fifty);
    CHECK(snow.windy());
    CHECK_FALSE(snow.raining());

    // Indoors StartWeather never runs: no sound, though the precipitation still moves.
    Weather indoors;
    indoors.handle_message(0, 10);
    for (int i = 0; i < 200; ++i) indoors.update(i * kFrame, false, fifty);
    CHECK_FALSE(indoors.raining());
    CHECK(indoors.precipitation() > 0.0f);
}

TEST_CASE("weather: a zero intensity packet clears the active rain or snow") {
    for (const auto kind : {WeatherKind::Rain, WeatherKind::Snow}) {
        const bool rain = kind == WeatherKind::Rain;
        CAPTURE(rain);
        Weather w;
        int frame = 0;
        const auto step = [&] { w.update(frame++ * kFrame, true, fifty); };
        const auto sound_on = [&] { return rain ? w.raining() : w.windy(); };
        w.handle_message(static_cast<std::int32_t>(kind), 10);
        for (int i = 0; i < 66; ++i) step();
        REQUIRE(sound_on());

        // A clear can carry the other kind, such as EQEmu's all-zero clear during snow.
        w.handle_message(rain ? 1 : 0, 0);
        step();
        CHECK(w.kind() == kind);
        for (int i = 0; i < 43; ++i) step();
        CHECK(sound_on());
        step();
        CHECK_FALSE(w.raining());
        CHECK_FALSE(w.windy());
        CHECK(w.kind() == WeatherKind::None);
        CHECK(w.precipitation() == 0.0f);
    }
}

TEST_CASE("weather: repeated clears leave the fade out on schedule") {
    Weather w;
    int frame = 0;
    const auto step = [&] { w.update(frame++ * kFrame, true, fifty); };
    w.handle_message(0, 10);
    for (int i = 0; i < 66; ++i) step();
    REQUIRE(w.raining());

    for (int i = 0; i < 44; ++i) {
        w.handle_message(i % 2 == 0 ? 0 : 0xff, 0);
        step();
        CHECK(w.raining());
    }
    w.handle_message(0xff, 0);
    step();
    CHECK_FALSE(w.raining());
    CHECK(w.kind() == WeatherKind::None);
    CHECK(w.precipitation() == 0.0f);
}

TEST_CASE("weather: rain can resume while the same storm is fading out") {
    for (const int start_frames : {30, 66}) {
        CAPTURE(start_frames);
        Weather w;
        int frame = 0;
        const auto step = [&] { w.update(frame++ * kFrame, true, fifty); };
        w.handle_message(0, 10);
        for (int i = 0; i < start_frames; ++i) step();
        const bool sound_started = start_frames == 66;
        REQUIRE(w.raining() == sound_started);

        w.handle_message(0xff, 0);
        for (int i = 0; i < 5; ++i) step();
        w.handle_message(0, 15);
        for (int i = 0; i < 70; ++i) {
            step();
            if (sound_started) CHECK(w.raining());
        }
        CHECK(w.raining());
        CHECK(w.kind() == WeatherKind::Rain);
        CHECK(w.precipitation() == doctest::Approx(0.15f));

        w.handle_message(0xff, 0);
        for (int i = 0; i < 65; ++i) step();
        CHECK_FALSE(w.raining());
        CHECK(w.kind() == WeatherKind::None);
    }
}

TEST_CASE("weather: lightning once faded in, then every 10 to 20 seconds, the thunder with it") {
    Weather w;
    w.handle_message(0, 10);
    int frame = 0;
    for (; frame < 66; ++frame) {
        w.update(frame * kFrame, true, fifty);
        CHECK_FALSE(w.lightning()); // not while fading in
    }
    // The first strike as soon as the fade is done (its time was the start's frame time).
    w.update(frame++ * kFrame, true, fifty);
    CHECK(w.lightning());
    CHECK(w.thunder() == Weather::kThunder2);
    const double struck = (frame - 1) * kFrame;
    w.update(frame++ * kFrame, true, fifty);
    CHECK_FALSE(w.lightning()); // one frame
    CHECK(w.thunder() == 0);
    // The next 10.05 s on (rand() % 10000 + 10000 ms).
    w.update(struck + 10.04, true, fifty);
    CHECK_FALSE(w.lightning());
    w.update(struck + 10.06, true, fifty);
    CHECK(w.lightning());

    // Not indoors.
    Weather indoors;
    indoors.handle_message(0, 10);
    for (int i = 0; i < 4000; ++i) {
        indoors.update(i * kFrame, false, fifty);
        CHECK_FALSE(indoors.lightning());
    }
}

TEST_CASE("weather: a 30 % chance of a second strike within a second, with the other thunder") {
    // Random() 50 (thunder2), then rand() 10 (< 30: a second strike), rand() 600 (600 ms later).
    std::int32_t values[] = {50, 10, 600};
    int next = 0;
    auto random = [&] { return next < 3 ? values[next++] : 50; };
    Weather w;
    w.handle_message(0, 10);
    int frame = 0;
    for (; frame < 66; ++frame) w.update(frame * kFrame, true, random);
    const double t = frame * kFrame;
    w.update(t, true, random);
    CHECK(w.lightning());
    CHECK(w.thunder() == Weather::kThunder2);
    w.update(t + 0.59, true, random);
    CHECK_FALSE(w.lightning());
    w.update(t + 0.61, true, random);
    CHECK(w.lightning());
    CHECK(w.thunder() == Weather::kThunder1);
}
