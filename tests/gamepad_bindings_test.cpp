// The gamepad's bindings, chords, settings and labels (game/gamepad_bindings.h), its dead zone (platform/gamepad.h),
// the backward NPC cycle (game/targeting.h) and hs.gamepad (app/lua_ui_gamepad.cpp). A client extension: docs/gamepad.md is the specification.
#include <doctest/doctest.h>

#include <limits>

#include <lua.hpp>

#include "app/gamepad.h"
#include "app/lua_ui.h"
#include "core/config.h"
#include "game/gamepad_bindings.h"
#include "game/keymap.h"
#include "game/targeting.h"

using namespace hailstone;
using namespace hailstone::game;
using platform::GamepadState;
using platform::PadButton;
using Kind = PadAction::Kind;

namespace hailstone::app {
void register_gamepad_api(lua_State* L, UiHost& host); // app/lua_ui_gamepad.cpp
}

namespace {

constexpr std::size_t idx(PadButton b) { return static_cast<std::size_t>(b); }

// One frame of the pad: `down` held now; pressed / released from the last frame's `was`.
struct Pad {
    GamepadState s;
    GamepadState& frame(std::initializer_list<PadButton> down) {
        GamepadState next;
        next.connected = true;
        for (PadButton b : down) next.down[idx(b)] = true;
        for (std::size_t i = 0; i < platform::kPadButtons; ++i) {
            next.pressed[i] = next.down[i] && !s.down[i];
            next.released[i] = !next.down[i] && s.down[i];
        }
        s = next;
        return s;
    }
};

int cmd(KeyCommand c) { return static_cast<int>(c); }

} // namespace

TEST_CASE("Pad inputs and actions parse and print as hailstone.ini writes them") {
    auto in = parse_pad_input("lt+a");
    REQUIRE(in);
    CHECK(in->button == PadButton::A);
    CHECK(in->chord == kChordLT);
    CHECK(pad_input_text(*in) == "lt+a");
    in = parse_pad_input("RT+LT+x");
    REQUIRE(in);
    CHECK(in->chord == (kChordLT | kChordRT));
    CHECK(pad_input_text(*in) == "lt+rt+x");
    in = parse_pad_input("back+start");
    REQUIRE(in);
    CHECK(pad_input_text(*in) == "back+start");
    CHECK_FALSE(parse_pad_input("back+a"));   // Back is a modifier only for Start
    CHECK_FALSE(parse_pad_input("lt+rt"));    // the triggers take no chord
    CHECK_FALSE(parse_pad_input("lt+lt+a"));
    CHECK_FALSE(parse_pad_input("lt+start"));
    CHECK_FALSE(parse_pad_input("q"));

    auto a = parse_pad_action("AUTOPRIM");
    REQUIRE(a);
    CHECK(*a == PadAction::key(cmd(KeyCommand::AUTOPRIM)));
    // docs/gamepad.md's CMD_ names for the movement commands.
    CHECK(parse_pad_action("CMD_FORWARD") == PadAction::key(cmd(KeyCommand::FORWARD)));
    CHECK(parse_pad_action("stop_cast") == PadAction::key(cmd(KeyCommand::CMD_STOP_CAST)));
    CHECK(parse_pad_action("hot:1:8") == PadAction::hot(1, 8));
    CHECK_FALSE(parse_pad_action("hot:11:1"));
    CHECK_FALSE(parse_pad_action("hot:1:13"));
    CHECK(parse_pad_action("camera_toggle") == PadAction::of(Kind::CameraToggle));
    CHECK(parse_pad_action("camera_iso") == PadAction::of(Kind::CameraIso));
    CHECK(pad_action_text(PadAction::of(Kind::CameraIso)) == "camera_iso");
    CHECK(parse_pad_action("none") == PadAction{});
    CHECK(parse_pad_action("") == PadAction{});
    CHECK_FALSE(parse_pad_action("NO_SUCH_COMMAND"));
    CHECK(pad_action_text(PadAction::hot(2, 5)) == "hot:2:5");
    CHECK(pad_action_text(PadAction::key(cmd(KeyCommand::JUMP))) == "JUMP");
    CHECK(pad_action_text(PadAction::of(Kind::TargetPrev)) == "target_prev");
}

TEST_CASE("The default bindings are section 3's") {
    GamepadBindings b;
    auto at = [&](PadButton p) { return b.action({p, 0}); };
    CHECK(at(PadButton::A) == PadAction::of(Kind::Interact));
    CHECK(at(PadButton::B) == PadAction::of(Kind::Cancel));
    CHECK(at(PadButton::X) == PadAction::key(cmd(KeyCommand::AUTOPRIM)));
    CHECK(at(PadButton::Y) == PadAction::key(cmd(KeyCommand::JUMP)));
    CHECK(at(PadButton::LB) == PadAction::of(Kind::TargetPrev));
    CHECK(at(PadButton::RB) == PadAction::of(Kind::TargetNext));
    CHECK(at(PadButton::Up) == PadAction::of(Kind::TargetGroup));
    CHECK(at(PadButton::Down) == PadAction::key(cmd(KeyCommand::SIT_STAND)));
    CHECK(at(PadButton::Left) == PadAction::of(Kind::HotPagePrev));
    CHECK(at(PadButton::Right) == PadAction::of(Kind::HotPageNext));
    CHECK(at(PadButton::LS) == PadAction::key(cmd(KeyCommand::AUTORUN)));
    CHECK(at(PadButton::RS) == PadAction::of(Kind::CameraToggle));
    CHECK(at(PadButton::Start) == PadAction::of(Kind::Menu));
    CHECK(at(PadButton::Back) == PadAction::of(Kind::Cursor));
    CHECK(b.action({PadButton::Start, kChordBack}) == PadAction::of(Kind::Chat));
    CHECK(at(PadButton::Guide) == PadAction{});
}

TEST_CASE("Chords: LT, RT and both make the D-pad and face buttons cross hotbar slots 1..8") {
    GamepadBindings b;
    CHECK(cross_slot_of(PadButton::Left) == 1);
    CHECK(cross_slot_of(PadButton::Up) == 2);
    CHECK(cross_slot_of(PadButton::Right) == 3);
    CHECK(cross_slot_of(PadButton::Down) == 4);
    CHECK(cross_slot_of(PadButton::X) == 5);
    CHECK(cross_slot_of(PadButton::Y) == 6);
    CHECK(cross_slot_of(PadButton::B) == 7);
    CHECK(cross_slot_of(PadButton::A) == 8);
    CHECK(cross_slot_of(PadButton::LB) == 0);
    CHECK(b.action({PadButton::A, kChordLT}) == PadAction::hot(1, 8));
    CHECK(b.action({PadButton::Up, kChordLT}) == PadAction::hot(1, 2));
    CHECK(b.action({PadButton::X, kChordRT}) == PadAction::hot(2, 5));
    CHECK(b.action({PadButton::Left, kChordLT | kChordRT}) == PadAction::hot(3, 1));
    // Buttons outside the cross hotbar keep their plain action under a chord.
    CHECK(b.action({PadButton::LB, kChordLT}) == PadAction::of(Kind::TargetPrev));
    // A chord's own binding wins over its slot.
    b.set_binding({PadButton::A, kChordLT}, PadAction::key(cmd(KeyCommand::CONSIDER)));
    CHECK(b.action({PadButton::A, kChordLT}) == PadAction::key(cmd(KeyCommand::CONSIDER)));
    b.set_binding({PadButton::A, kChordLT}, std::nullopt);
    CHECK(b.action({PadButton::A, kChordLT}) == PadAction::hot(1, 8));
    // A cross slot pointed elsewhere.
    CHECK(b.set_cross(2, 5, {4, 11}));
    CHECK(b.action({PadButton::X, kChordRT}) == PadAction::hot(4, 11));
    CHECK_FALSE(b.set_cross(4, 1, {1, 1}));
    CHECK_FALSE(b.set_cross(1, 1, {11, 1}));
    CHECK(b.cross(0, 1) == CrossSlot{0, 0});
}

TEST_CASE("update: a press fires its chord's action, its release ends that same action") {
    GamepadBindings b;
    Pad pad;
    std::vector<PadEvent> out;
    // Y: JUMP held as long as the button.
    b.update(pad.frame({PadButton::Y}), out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].action == PadAction::key(cmd(KeyCommand::JUMP)));
    CHECK(out[0].down);
    out.clear();
    b.update(pad.frame({PadButton::Y}), out);
    CHECK(out.empty());
    b.update(pad.frame({}), out);
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].down);
    out.clear();

    // LT held, then Y: the cross slot; LT let go before Y: Y's release still ends the slot, not JUMP.
    b.update(pad.frame({PadButton::LT}), out);
    CHECK(out.empty()); // LT alone does nothing by default
    b.update(pad.frame({PadButton::LT, PadButton::Y}), out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].action == PadAction::hot(1, 6));
    out.clear();
    b.update(pad.frame({PadButton::Y}), out);
    CHECK(out.empty());
    b.update(pad.frame({}), out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].action == PadAction::hot(1, 6));
    CHECK_FALSE(out[0].down);
    out.clear();

    // Pressed and let go within one frame: down and up at once.
    GamepadState tap;
    tap.pressed[idx(PadButton::X)] = tap.released[idx(PadButton::X)] = true;
    b.update(tap, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].down);
    CHECK_FALSE(out[1].down);
    out.clear();

    // release_all ends what is held.
    b.update(pad.frame({PadButton::Y}), out);
    out.clear();
    b.release_all(out);
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].down);
}

TEST_CASE("update: Back and Start fire as let go; together they are back+start") {
    GamepadBindings b;
    Pad pad;
    std::vector<PadEvent> out;
    b.update(pad.frame({PadButton::Back}), out);
    CHECK(out.empty());
    b.update(pad.frame({}), out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].action == PadAction::of(Kind::Cursor));
    out.clear();

    b.update(pad.frame({PadButton::Back}), out);
    b.update(pad.frame({PadButton::Back, PadButton::Start}), out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].action == PadAction::of(Kind::Chat));
    out.clear();
    b.update(pad.frame({PadButton::Start}), out);
    b.update(pad.frame({}), out);
    CHECK(out.empty()); // neither fires on its own after the pair

    b.update(pad.frame({PadButton::Start}), out);
    b.update(pad.frame({}), out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].action == PadAction::of(Kind::Menu));
}

TEST_CASE("Bindings and cross slots go to hailstone.ini only when they differ from the defaults") {
    GamepadBindings b;
    b.set_binding({PadButton::X, 0}, PadAction::key(cmd(KeyCommand::JUMP)));
    b.set_binding({PadButton::Y, 0}, PadAction::key(cmd(KeyCommand::JUMP))); // its default: no key
    b.set_binding({PadButton::A, kChordLT}, PadAction::hot(5, 9));
    b.set_cross(1, 1, {4, 5});
    Config c;
    c.parse("gamepad.bind.b = cancel\ngamepad.cross.3.8 = 9:9\nother = 1\n");
    b.save(c);
    CHECK(c.get("gamepad.bind.x") == "JUMP");
    CHECK_FALSE(c.get("gamepad.bind.y"));
    CHECK_FALSE(c.get("gamepad.bind.b")); // an old key now at its default goes
    CHECK(c.get("gamepad.bind.lt+a") == "hot:5:9");
    CHECK(c.get("gamepad.cross.1.1") == "4:5");
    CHECK_FALSE(c.get("gamepad.cross.3.8"));
    CHECK(c.get("other") == "1");

    GamepadBindings loaded;
    loaded.load(c);
    CHECK(loaded.action({PadButton::X, 0}) == PadAction::key(cmd(KeyCommand::JUMP)));
    CHECK(loaded.action({PadButton::A, kChordLT}) == PadAction::hot(5, 9));
    CHECK(loaded.cross(1, 1) == CrossSlot{4, 5});
    CHECK(loaded.cross(3, 8) == CrossSlot{3, 8});

    Config typed;
    typed.parse("gamepad.bind.x = AUTOPRIM\ngamepad.bind.lt+a = hot:1:8\ngamepad.bind.y = nonsense\n"
                "gamepad.cross.2.3 = 7:12\n");
    GamepadBindings t;
    t.load(typed);
    CHECK(t.binding({PadButton::A, kChordLT}) == PadAction::hot(1, 8));
    CHECK_FALSE(t.binding({PadButton::X, 0})); // the default, written out
    CHECK(t.action({PadButton::Y, 0}) == PadAction::key(cmd(KeyCommand::JUMP)));
    CHECK(t.cross(2, 3) == CrossSlot{7, 12});
}

TEST_CASE("The left stick gives the digital movement commands past their thresholds") {
    CHECK(stick_move(0.0f, 0.0f) == StickMove{});
    CHECK(stick_move(0.0f, -0.36f).forward);
    CHECK_FALSE(stick_move(0.0f, -0.34f).forward);
    CHECK(stick_move(0.0f, 0.5f).back);
    CHECK(stick_move(-0.51f, 0.0f).left);
    CHECK_FALSE(stick_move(-0.49f, 0.0f).left);
    CHECK(stick_move(0.9f, -0.9f) == StickMove{true, false, false, true});
    CHECK(stick_move(0.3f, -0.3f, 0.2f, 0.25f) == StickMove{true, false, false, true});
}

TEST_CASE("The sticks' dead zone is radial and rescaled") {
    float x = 0.1f, y = 0.15f;
    platform::radial_dead_zone(x, y, 0.2f);
    CHECK(x == 0.0f);
    CHECK(y == 0.0f);
    x = 0.6f, y = 0.0f;
    platform::radial_dead_zone(x, y, 0.2f);
    CHECK(x == doctest::Approx(0.5f));
    x = 1.0f, y = 1.0f; // past the circle: full deflection, the diagonal kept
    platform::radial_dead_zone(x, y, 0.2f);
    CHECK(x == doctest::Approx(0.7071f).epsilon(0.001));
    CHECK(y == doctest::Approx(0.7071f).epsilon(0.001));
}

TEST_CASE("cross_set: the held triggers") {
    Pad pad;
    CHECK(cross_set(pad.frame({})) == 0);
    CHECK(cross_set(pad.frame({PadButton::LT})) == 1);
    CHECK(cross_set(pad.frame({PadButton::RT})) == 2);
    CHECK(cross_set(pad.frame({PadButton::LT, PadButton::RT})) == 3);
}

TEST_CASE("Gamepad settings: section 4's defaults, clamped, by name with or without the prefix") {
    GamepadSettings s;
    CHECK(s.get("deadzone") == doctest::Approx(0.2f));
    CHECK(s.get("gamepad.trigger_threshold") == doctest::Approx(0.5f));
    CHECK(s.get("camera_speed") == doctest::Approx(2.5f));
    CHECK(s.get("enabled") == 1.0f);
    CHECK(s.get("invert_y") == 0.0f);
    CHECK_FALSE(s.get("nonsense"));
    CHECK(s.set("deadzone", 5.0f));
    CHECK(s.deadzone == doctest::Approx(0.9f));
    CHECK(s.set("invert_y", 1.0f));
    CHECK(s.invert_y);
    CHECK_FALSE(s.set("nonsense", 1.0f));
    Config c;
    s.save(c);
    CHECK(c.get("gamepad.invert_y") == "1");
    CHECK(c.get("gamepad.deadzone") == "0.9");
    Config typed;
    typed.parse("gamepad.camera_speed = 4\ngamepad.rumble = 0\n");
    GamepadSettings t;
    t.load(typed);
    CHECK(t.camera_speed == doctest::Approx(4.0f));
    CHECK_FALSE(t.rumble);
    CHECK(t.deadzone == doctest::Approx(0.2f));
    // NaN would pass the clamp and be saved; it and the infinities are refused, and a saved one loads as the default.
    CHECK_FALSE(s.set("deadzone", std::numeric_limits<float>::quiet_NaN()));
    CHECK_FALSE(s.set("camera_speed", std::numeric_limits<float>::infinity()));
    CHECK_FALSE(s.set("invert_y", std::numeric_limits<float>::quiet_NaN()));
    CHECK(s.deadzone == doctest::Approx(0.9f));
    CHECK(s.camera_speed == doctest::Approx(2.5f));
    CHECK(s.invert_y);
    Config bad;
    bad.parse("gamepad.deadzone = nan\ngamepad.camera_speed = inf\n");
    GamepadSettings u;
    u.load(bad);
    CHECK(u.deadzone == doctest::Approx(0.2f));
    CHECK(u.camera_speed == doctest::Approx(2.5f));
}

TEST_CASE("Gamepad rumble: strengths clamped, NaN refused, the length capped") {
    Config config;
    app::Gamepad gp(config, {});
    float got_low = -1, got_high = -1;
    int got_ms = -1, calls = 0;
    gp.set_rumble([&](float l, float h, int ms) {
        got_low = l;
        got_high = h;
        got_ms = ms;
        ++calls;
        return true;
    });
    CHECK(gp.rumble(2.0f, -1.0f, 100000000));
    CHECK(got_low == 1.0f);
    CHECK(got_high == 0.0f);
    CHECK(got_ms == app::Gamepad::kMaxRumbleMs);
    CHECK(gp.rumble(0.5f, 0.5f, -5));
    CHECK(got_ms == 0);
    CHECK_FALSE(gp.rumble(std::numeric_limits<float>::quiet_NaN(), 0.5f, 100));
    CHECK_FALSE(gp.rumble(0.5f, std::numeric_limits<float>::infinity(), 100));
    CHECK(calls == 2);
}

TEST_CASE("Each pad's labels") {
    CHECK(std::string(pad_glyph(PadType::Xbox, PadButton::A)) == "A");
    CHECK(std::string(pad_glyph(PadType::PlayStation, PadButton::A)) == "Cross");
    CHECK(std::string(pad_glyph(PadType::PlayStation, PadButton::LB)) == "L1");
    CHECK(std::string(pad_glyph(PadType::Nintendo, PadButton::LB)) == "L");
    CHECK(std::string(pad_glyph(PadType::Nintendo, PadButton::A)) == "B"); // the bottom face button
    CHECK(std::string(pad_glyph(PadType::Generic, PadButton::Start)) == "Start");
}

TEST_CASE("The backward NPC cycle walks the forward cycle's list the other way") {
    TargetManager tm;
    std::vector<CycleCandidate> offered = {{1, 30.0f}, {2, 10.0f}, {3, 20.0f}};
    // Nearest first: 2, 3, 1. No target in the list: the nearest.
    CHECK(get_previous_cycle_target(tm, 1, 0, offered) == 2);
    CHECK(get_previous_cycle_target(tm, 1, 2, offered) == 1); // before the first: the last (the farthest)
    CHECK(get_previous_cycle_target(tm, 1, 1, offered) == 3);
    // The forward cycle goes on after the entry reached backward.
    CHECK(get_next_cycle_target(tm, 1, false, offered) == 1);
    CHECK(get_previous_cycle_target(tm, 1, 99, {}) == 0);
    CHECK(get_previous_cycle_target(tm, 5, 0, offered) == 0);
}

TEST_CASE("hs.gamepad: section 5's calls over the pad, its mode and its bindings") {
    Config config;
    app::Gamepad gp(config, {});
    GamepadState s;
    s.connected = true;
    s.name = "Test Pad";
    s.type = PadType::PlayStation;
    s.down[idx(PadButton::LT)] = s.pressed[idx(PadButton::LT)] = true;
    s.axes[static_cast<std::size_t>(platform::PadAxis::LX)] = -0.5f;
    gp.set_state(&s);
    app::UiHost host;
    host.gamepad = &gp;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    app::register_gamepad_api(L, host);
    lua_setglobal(L, "hs");
    const char* script = R"(
        local g = hs.gamepad
        local ok, name = g.connected()
        assert(ok and name == "Test Pad")
        assert(g.type() == "playstation")
        assert(g.down("lt") and g.pressed("lt") and not g.released("lt") and not g.down("a"))
        assert(g.down("nonsense") == false)
        assert(math.abs(g.axis("lx") + 0.5) < 1e-6 and g.axis("ry") == 0)
        assert(g.mode() == "game" and g.cross_set() == 1)
        local bar, button = g.cross_slot(2, 5)
        assert(bar == 2 and button == 5)
        assert(g.cross_slot(4, 1) == nil)
        assert(g.glyph("a") == "Cross" and g.glyph("lb") == "L1")
        assert(g.binding("x") == "AUTOPRIM" and g.binding("lt+a") == "" and g.binding("q") == nil)
        assert(g.set_binding("lt+a", "JUMP") and g.binding("lt+a") == "JUMP")
        assert(g.set_binding("lt+a", "") and g.binding("lt+a") == "")
        assert(not g.set_binding("x", "NO_SUCH"))
        assert(math.abs(g.setting("deadzone") - 0.2) < 1e-6 and g.setting("gamepad.camera_speed") == 2.5)
        assert(g.set_setting("invert_y", true) and g.setting("invert_y") == 1)
        assert(g.setting("nonsense") == nil)
        assert(g.rumble(1, 1, 100) == false) -- no rumble here
        assert(g.set_mode("cursor") == "cursor" and g.cross_set() == 0)
        g.request_focus("Main Menu")
        assert(g.mode() == "menu")
        -- two windows appearing in one frame (the merchant, a Tip over it): both are kept, the last on top
        g.request_focus("Merchant")
        g.request_focus("Tip")
        g.request_focus("Merchant")
        g.request_focus("Tip")
    )";
    bool ran = luaL_dostring(L, script) == LUA_OK;
    if (!ran) MESSAGE(lua_tostring(L, -1));
    CHECK(ran);
    lua_close(L);
    CHECK(gp.take_focus_requests() == std::vector<std::string>{"Main Menu", "Merchant", "Tip"});
    CHECK(gp.take_focus_requests().empty());
    CHECK(config.get("gamepad.invert_y") == "1");
}
