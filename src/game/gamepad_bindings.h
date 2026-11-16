#pragma once

// The gamepad's bindings (docs/gamepad.md sections 3 and 4): which key command or pad action each button and
// trigger chord gives in game mode, the cross hotbar's slots, the left stick's digital movement, the gamepad.*
// settings of hailstone.ini and each pad's button labels. A client extension: RoF2 has no gamepad scheme. The pad
// only presses the original's key commands (game/keymap.h, run by ClientApp::key_command as a key's are), so every
// game rule stays the original's.
//
// Pure logic, no SDL (tests/gamepad_bindings_test.cpp); app/client_app_gamepad.cpp runs what it resolves.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/gamepad.h"

namespace hailstone { class Config; }

namespace hailstone::game {

using platform::GamepadState;
using platform::PadAxis;
using platform::PadButton;
using platform::PadType;

// What a pad input does: a key command (game/keymap.h slot), a hot button, or one of the pad's own actions.
struct PadAction {
    enum class Kind : std::uint8_t {
        None,
        Command,     // a KeyCommand by name ("AUTOPRIM", "JUMP"; the CMD_ prefix may be left out or added)
        Hot,         // "hot:<bar>:<button>": ActionsUi::hot_button, bar 1..10, button 1..12 of the bar's page
        Menu,        // "menu": the pad to menu mode (the gamepad pack opens its main menu)
        Cursor,      // "cursor": cursor mode on / off
        CameraToggle,// "camera_toggle": first person / the camera behind the player
        Interact,    // "interact": the target's right-click action in range, else USE
        Cancel,      // "cancel": stop casting, else close the top window, else clear the target
        TargetPrev,  // "target_prev": the NPC cycle backward (game/targeting.h target_previous_command)
        TargetNext,  // "target_next": CYCLENPCTARGETS
        TargetGroup, // "target_group": the next of yourself and the group members (TARGETME, PARTY1..5)
        HotPagePrev, // "hotpage_prev" / "hotpage_next": hot bar 1's page before / after (HOTPAGE1_*)
        HotPageNext,
        Chat,        // "chat": the chat input (the chat key 488, game/chat_input.h)
        CameraIso,   // "camera_iso": the isometric camera on / off (docs/isometric.md)
    };
    Kind kind = Kind::None;
    int command = 0;          // Command: the key map slot
    int bar = 0, button = 0;  // Hot: 1-based
    bool operator==(const PadAction&) const = default;

    static PadAction of(Kind k) { return PadAction{k, 0, 0, 0}; }
    static PadAction key(int command) { return PadAction{Kind::Command, command, 0, 0}; }
    static PadAction hot(int bar, int button) { return PadAction{Kind::Hot, 0, bar, button}; }
};
// "none" (or empty) is Kind::None; nullopt for text that is no action.
std::optional<PadAction> parse_pad_action(std::string_view text);
std::string pad_action_text(const PadAction& a);

// An input: a button alone, or with modifiers held: LT, RT (the cross hotbar's sets) or Back (only with Start).
inline constexpr std::uint8_t kChordLT = 1, kChordRT = 2, kChordBack = 4;
struct PadInput {
    PadButton button = PadButton::A;
    std::uint8_t chord = 0;
    auto operator<=>(const PadInput&) const = default;
};
// "x", "lt+a", "rt+up", "lt+rt+x", "back+start"; nullopt otherwise.
std::optional<PadInput> parse_pad_input(std::string_view text);
std::string pad_input_text(PadInput in);

// The cross hotbar (section 3): 3 sets (1 LT, 2 RT, 3 LT+RT) of 8 slots, D-pad left, up, right, down, then X, Y,
// B, A; each slot is a hot button. The slot a button is (1..8), 0 for the others.
inline constexpr int kCrossSets = 3, kCrossSlots = 8;
int cross_slot_of(PadButton b);
struct CrossSlot {
    int bar = 1, button = 1; // 1-based
    bool operator==(const CrossSlot&) const = default;
};
// 0 none, 1 LT, 2 RT, 3 both: the set the held triggers pick.
int cross_set(const GamepadState& pad);

// The bindings with their defaults (section 3).
PadAction default_pad_action(PadInput in);

// One resolved press or release.
struct PadEvent {
    PadAction action;
    bool down = true;
};

class GamepadBindings {
public:
    GamepadBindings() { reset_defaults(); }
    void reset_defaults();

    // What the input gives: its own binding when set, else for a chord a cross hotbar slot (D-pad and face buttons)
    // or the button's plain action, else the default.
    PadAction action(PadInput in) const;
    // The input's own binding (none: the default applies; a chord's default is its cross slot or the plain action).
    std::optional<PadAction> binding(PadInput in) const;
    // nullopt puts the default back.
    void set_binding(PadInput in, std::optional<PadAction> a);

    CrossSlot cross(int set, int slot) const; // set 1..3, slot 1..8 (out of range: {0, 0})
    bool set_cross(int set, int slot, CrossSlot s);

    // gamepad.bind.<input> = <action> and gamepad.cross.<set>.<slot> = <bar>:<button>: only the ones that differ
    // from the defaults are written (the others' keys are removed).
    void load(const Config& config);
    void save(Config& config) const;

    // The frame's presses and releases (the pad in game mode): a press fires the action its chord picks then, and
    // its release ends that same action (a held key command). Back and Start fire when let go, unless they were
    // pressed together (back+start).
    void update(const GamepadState& pad, std::vector<PadEvent>& out);
    // Ends every held action (the pad left game mode or was unplugged).
    void release_all(std::vector<PadEvent>& out);

private:
    std::map<PadInput, PadAction> bindings_; // only those set (defaults are default_pad_action)
    std::array<std::array<CrossSlot, kCrossSlots>, kCrossSets> cross_{};
    std::array<std::optional<PadAction>, platform::kPadButtons> held_{};
    bool back_start_ = false; // Back and Start were pressed together since both were last up
};

// The left stick as the digital movement commands (FORWARD, BACK, STRAFE_LEFT, STRAFE_RIGHT): the original has no
// analog speed. Forward / back past `move` (stick up is negative y), strafe past `strafe`.
struct StickMove {
    bool forward = false, back = false, left = false, right = false;
    bool operator==(const StickMove&) const = default;
};
StickMove stick_move(float lx, float ly, float move = 0.35f, float strafe = 0.5f);

// The gamepad.* numbers (section 4).
struct GamepadSettings {
    bool enabled = true;
    float deadzone = 0.2f;
    float trigger_threshold = 0.5f;
    float camera_speed = 2.5f;      // radians per second at full right-stick deflection
    bool invert_y = false;
    bool rumble = true;
    float move_threshold = 0.35f;   // the left stick's forward / back
    float strafe_threshold = 0.5f;  // and strafe
    float cursor_speed = 900.0f;    // cursor mode: pixels per second at full left-stick deflection (at 100% scale)

    void load(const Config& config);
    void save(Config& config) const;
    // By name ("deadzone" or "gamepad.deadzone"; booleans 0 / 1); nullopt / false for an unknown name. set clamps,
    // and refuses NaN and the infinities (false, the setting unchanged; load keeps the default for one).
    std::optional<float> get(std::string_view name) const;
    bool set(std::string_view name, float value);
    static const std::vector<std::string_view>& names();
};

// The label the connected pad has for a button (hs.gamepad.glyph): the Xbox names, PlayStation's Cross, Circle,
// Square, Triangle, L1 ..., and Nintendo's, whose face buttons are lettered the other way round (the bottom one is B).
const char* pad_glyph(PadType type, PadButton b);

} // namespace hailstone::game
