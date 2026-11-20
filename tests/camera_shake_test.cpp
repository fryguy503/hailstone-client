// The camera shake of OP_CameraEffect (game/camera_shake.h), from the original's camera effect (Set @0x7965d0,
// Update @0x796660, Reset @0x7965b0), and the two zone packets that drive it and OP_Marquee.

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "game/camera_shake.h"
#include "game/text_overlay.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/notice_packets.h"

using namespace hailstone;
using namespace hailstone::net;

namespace {

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

// rand() gives `values` in order, then 0.
class FixedRand {
public:
    explicit FixedRand(std::vector<std::int32_t> values) : values_(std::move(values)) {}
    std::int32_t next() { return at_ < values_.size() ? values_[at_++] : 0; }
    std::size_t drawn() const { return at_; }

private:
    std::vector<std::int32_t> values_;
    std::size_t at_ = 0;
};

} // namespace

TEST_CASE("camera shake: a stronger effect replaces, an equal one extends, a weaker one is ignored") {
    FixedRand rng({32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767});
    game::CameraShake shake([&] { return rng.next(); });
    const glm::vec3 p(10.0f, 20.0f, 30.0f);

    CHECK_FALSE(shake.active());
    CHECK(shake.offset(p, 1000) == p); // nothing set
    CHECK(rng.drawn() == 0);

    shake.set(2.0f, 1000, 1000);
    CHECK(shake.active());
    CHECK(shake.strength() == 2.0f);
    CHECK(shake.remaining_ms() == 1000);
    // The first frame draws the full strength each way: (32767 / 32767 - 0.5) x 2 = 1.
    glm::vec3 o = shake.offset(p, 1000);
    CHECK(near(o.x, p.x + 1.0f));
    CHECK(near(o.y, p.y + 1.0f));
    CHECK(near(o.z, p.z + 1.0f));
    CHECK(shake.remaining_ms() == 1000); // no time was taken off at the set's own time

    o = shake.offset(p, 1040);
    CHECK(shake.remaining_ms() == 960);
    CHECK(near(o.x, p.x + 1.0f));

    // A weaker one changes nothing, not even a longer duration.
    shake.set(1.0f, 9000, 1050);
    CHECK(shake.strength() == 2.0f);
    CHECK(shake.remaining_ms() == 960);
    // Nor does a NaN (the original's unordered compare skips the update).
    shake.set(std::numeric_limits<float>::quiet_NaN(), 9000, 1050);
    CHECK(shake.strength() == 2.0f);
    CHECK(shake.remaining_ms() == 960);

    // An equal one with a longer duration: the strength stays, the duration and its start move.
    shake.set(2.0f, 2000, 1060);
    CHECK(shake.strength() == 2.0f);
    CHECK(shake.remaining_ms() == 2000);
    // An equal one with a shorter duration leaves what is left alone.
    shake.set(2.0f, 100, 1070);
    CHECK(shake.strength() == 2.0f);
    CHECK(shake.remaining_ms() == 2000);

    o = shake.offset(p, 1070); // 10 ms on from the set at 1060
    CHECK(shake.remaining_ms() == 1990);
    CHECK(near(o.y, p.y + 1.0f));

    // Past the end: this frame still shakes, then the state is gone.
    o = shake.offset(p, 4000);
    CHECK(near(o.z, p.z + 1.0f));
    CHECK_FALSE(shake.active());
    CHECK(shake.strength() == 0.0f);
    CHECK(shake.remaining_ms() == 0);
    CHECK(shake.offset(p, 5000) == p);
}

TEST_CASE("camera shake: a really stronger effect replaces a weaker one") {
    game::CameraShake shake([] { return 0; });
    shake.set(1.0f, 500, 100);
    REQUIRE(shake.strength() == 1.0f);
    REQUIRE(shake.remaining_ms() == 500);
    // Stronger and longer: both are taken.
    shake.set(3.0f, 800, 200);
    CHECK(shake.strength() == 3.0f);
    CHECK(shake.remaining_ms() == 800);
    // Stronger but shorter: the strength is taken, the duration stays.
    shake.set(5.0f, 100, 300);
    CHECK(shake.strength() == 5.0f);
    CHECK(shake.remaining_ms() == 800);
}

TEST_CASE("camera shake: the clock, the three draws and a clock that goes backwards") {
    // A set at time 0 leaves its start at 0 (unset); the first update starts the clock and keeps the duration.
    {
        FixedRand rng({16383, 32767, 0});
        game::CameraShake shake([&] { return rng.next(); });
        const glm::vec3 p(0.0f);
        shake.set(2.0f, 1000, 0);
        const glm::vec3 o = shake.offset(p, 7000);
        CHECK(shake.remaining_ms() == 1000);
        CHECK(near(o.x, (16383.0f / 32767.0f - 0.5f) * 2.0f));
        CHECK(near(o.y, 1.0f));
        CHECK(near(o.z, -1.0f));
        CHECK(rng.drawn() == 3); // one draw an axis, x then y then z
    }
    // A clock that went backwards wipes the state and leaves the camera alone, without drawing.
    {
        FixedRand rng({0, 0, 0});
        game::CameraShake shake([&] { return rng.next(); });
        const glm::vec3 p(1.0f, 2.0f, 3.0f);
        shake.set(4.0f, 500, 2000);
        CHECK(shake.offset(p, 1999) == p);
        CHECK(rng.drawn() == 0);
        CHECK_FALSE(shake.active());
        CHECK(shake.strength() == 0.0f);
        CHECK(shake.offset(p, 2500) == p);
    }
    // The zone-in's reset.
    {
        game::CameraShake shake([] { return 0; });
        shake.set(2.0f, 1000, 100);
        REQUIRE(shake.active());
        shake.reset();
        CHECK_FALSE(shake.active());
        CHECK(shake.strength() == 0.0f);
        CHECK(shake.offset(glm::vec3(1.0f), 200) == glm::vec3(1.0f));
    }
}

TEST_CASE("OP_Marquee shows the large overlay text and OP_CameraEffect sets the shake") {
    test::Rig rig;

    constexpr std::int32_t kColour = 15, kPriority = 10, kLowerPriority = 5, kHigherPriority = 11;
    constexpr std::int32_t kAlpha = 200;
    constexpr std::uint32_t kFadeInMs = 1000, kFadeOutMs = 2000, kHoldMs = 4000;
    constexpr std::uint32_t kHalfFadeInMs = kFadeInMs / 2, kHalfFadeOutMs = kFadeOutMs / 2;
    constexpr std::uint32_t kInHoldMs = kFadeInMs + (kHoldMs - kFadeInMs) / 2;
    // The fade-out starts at the first frame past the hold (the margin covers the clock between `before` and display).
    constexpr std::uint32_t kPastHoldMs = kHoldMs + 100;
    const auto alpha_of = [](const game::TextOverlay::Frame& f) { return static_cast<std::int32_t>(f.argb >> 24); };

    Marquee m;
    m.colour = kColour;
    m.priority = kPriority;
    m.alpha = kAlpha;
    m.fade_in_ms = kFadeInMs;
    m.fade_out_ms = kFadeOutMs;
    m.hold_ms = kHoldMs;
    m.text = "YOU HAVE FOUND AN ECHO OF MEMORY!";
    const auto before = static_cast<std::uint32_t>(GameConnection::now_ms());
    rig.conn.receive_zone_packet_for_test(encode(m));
    REQUIRE(rig.conn.text_overlay().active());
    CHECK(rig.chat().empty()); // only a debug log, no chat line

    // A lower priority while this shows is ignored (the text and its timing stay).
    Marquee lower = m;
    lower.priority = kLowerPriority;
    lower.text = "LOWER";
    rig.conn.receive_zone_packet_for_test(encode(lower));

    // Mid fade-in: part of the alpha, in the colour.
    auto frame = rig.conn.text_overlay().frame(before + kHalfFadeInMs);
    REQUIRE(frame);
    CHECK(frame->text == m.text);
    CHECK((frame->argb & 0xffffffu) == game::text_overlay_rgb(kColour));
    CHECK(alpha_of(*frame) > 0);
    CHECK(alpha_of(*frame) < kAlpha);
    // After the fade-in, in the hold: the full alpha.
    frame = rig.conn.text_overlay().frame(before + kInHoldMs);
    REQUIRE(frame);
    CHECK(alpha_of(*frame) == kAlpha);
    // The first frame past the hold starts the fade-out, at the full alpha; half the fade-out on, part of it is left.
    frame = rig.conn.text_overlay().frame(before + kPastHoldMs);
    REQUIRE(frame);
    CHECK(alpha_of(*frame) == kAlpha);
    frame = rig.conn.text_overlay().frame(before + kPastHoldMs + kHalfFadeOutMs);
    REQUIRE(frame);
    CHECK(alpha_of(*frame) > 0);
    CHECK(alpha_of(*frame) < kAlpha);
    // Past the fade-out: gone.
    CHECK_FALSE(rig.conn.text_overlay().frame(before + kPastHoldMs + kFadeOutMs));
    CHECK_FALSE(rig.conn.text_overlay().active());

    // A higher priority replaces what shows.
    rig.conn.receive_zone_packet_for_test(encode(m));
    Marquee higher = m;
    higher.priority = kHigherPriority;
    higher.text = "HIGHER";
    rig.conn.receive_zone_packet_for_test(encode(higher));
    frame = rig.conn.text_overlay().frame(static_cast<std::uint32_t>(GameConnection::now_ms()));
    REQUIRE(frame);
    CHECK(frame->text == higher.text);

    CameraEffect c;
    c.duration_ms = 2000;
    c.intensity = 3.0f;
    CHECK_FALSE(rig.conn.camera_shake().active());
    rig.conn.receive_zone_packet_for_test(encode(c));
    CHECK(rig.conn.camera_shake().active());
    CHECK(rig.conn.camera_shake().strength() == 3.0f);
    CHECK(rig.conn.camera_shake().remaining_ms() == 2000);
}

TEST_CASE("camera shake: the zone-in clears it") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    CameraEffect c;
    c.duration_ms = 2000;
    c.intensity = 3.0f;
    rig.conn.receive_zone_packet_for_test(encode(c));
    REQUIRE(rig.conn.camera_shake().active());
    GameConnectionTestAccess::finish_zone_in(rig.conn);
    CHECK_FALSE(rig.conn.camera_shake().active());
}
