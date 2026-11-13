#pragma once

// The original client's key bindings: its 479 mappable commands (szEQMappableCommands), two banks of 500 key
// combos (KeypressHandler: NormalKey / AltKey), the default bindings of KeypressHandler::ResetKeysToEqDefaults
// @0x5598d0, the ini format (KeypressHandler::LoadAndSetKeymappings @0x55b100: KEYMAPPING_<command>_<1|2> =
// the KeyCombo int, after the legacy KEYMAP_<command>[_2] = [SHIFT+][CONTROL+][ALT+]<key> text of
// KeypressHandler_ReadKeyMapFromIni @0x513940), the key names of KeyCombo::GetTextDescription @0x88b690 and the
// key press dispatch of KeypressHandler::HandleKeyDown @0x5594e0 / HandleKeyUp @0x559800.
//
// Tables: keymap_tables.inc (tools/gen_keymap.py, from the analysis of the original client). Keys are DirectInput scan codes
// (platform/window.h kKeyCodes).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone { class Config; }

namespace hailstone::game {

enum class KeyCommand : std::uint16_t {
#define HAILSTONE_KEY_COMMAND(index, name) name = index,
#include "game/keymap_tables.inc"
#undef HAILSTONE_KEY_COMMAND
};
inline constexpr int kKeyCommands = 479; // named, mappable commands (0..0x1de)
inline constexpr int kKeySlots = 500;    // KeyCombo slots per bank (nEQMappableCommands)

// KeyCombo {alt, ctrl, shift, scan code}; as an int: code | 0x40000000 shift | 0x20000000 ctrl | 0x10000000 alt.
struct KeyCombo {
    std::uint8_t code = 0; // 0 = unbound
    bool shift = false, ctrl = false, alt = false;

    static KeyCombo from_int(std::int32_t v);
    std::int32_t to_int() const;
    bool bound() const { return code != 0; }
    bool operator==(const KeyCombo&) const = default;
};

const char* command_name(int command);                // "FORWARD"; "" outside 0..478
std::optional<int> command_by_name(std::string_view name);
// COptionsWnd::InitKeyboardAssignments: the eqstr id of the command's label (0: not listed) and its category
// (bit flags: 1 movement, 2 commands, 4 spells, 8 targets, 0x10 camera, 0x20 chat, 0x40 windows, 0x80 << n hot
// bar n + 1, 0x20000 voice, 0x40000 real estate).
std::uint32_t command_label_eqstr(int command);
std::uint32_t command_category(int command);
// The commands in the original's options window order (without the dropped ones).
const std::vector<int>& listed_commands();
// Commands of features this client leaves out on purpose (docs/plan.md, "Not implemented, by choice"): the voice
// macro window (CMD_TOGGLEVOICEWIN), the audio triggers window (CMD_TOGGLE_AUDIO_TRIGGER_WINDOW), and Daybreak's
// services EQEmu has no part of: the Marketplace's silver / gold membership keys (CMD_MARKETPLACE_SILVER / _GOLD:
// __ExecuteCmd cases 166 / 167 jump the window to entries 0x42f / 0x427, the publisher's upgrades; CMD_MARKETPLACE
// itself stays, the window speaks the original's opcodes), the veteran reward
// claim window's key (CMD_TOGGLE_CLAIM_WIN; /claim still opens it), polls (CMD_VOTE_RESULTS),
// alerts (CMD_HISTORIC_ALERTS), real estate's trophies and help windows and
// leave key (CMD_REAL_ESTATE_TROPHIES_WINDOW, CMD_REAL_ESTATE_HELP_WINDOW, CMD_REAL_ESTATE_LEAVE; its windows and
// item placement keys stay), the "Upgrade EverQuest" help (OPEN_ETN_HELP), the
// trading card game (CMD_TOGGLE_TCG) and social sharing (CMD_SOCIAL_SHARE). They keep their index and ini name
// but get no default key, no row in the keys list and no EQ menu row.
bool key_command_dropped(int command);

// KeyCombo::GetTextDescription: "-------" when unbound, else a modifier prefix ("Shift ", "Ctrl ", "Alt ",
// "ShCtl ", ...) and the key's name ("F1", "NPAD 5", or the key's upper-case character; "?" otherwise).
std::string key_text(const KeyCombo& k);
// KeypressHandler_ReadKeyMapFromIni's value parser: -2 for "*NONE*", -1 for an unknown key, else the KeyCombo int.
std::int32_t parse_keymap_text(std::string_view value);
// KeypressHandler::IsReservedKey @0x5595c0: shift, ctrl, alt, the Windows keys, Enter, '/', Alt+Enter, Alt+Tab.
bool is_reserved_key(const KeyCombo& k);
// The ini key: KEYMAPPING_<command>_<bank + 1> (GetEqCommandSaveName @0x5596f0).
std::string keymapping_key(int command, int bank);

class KeyMap {
public:
    KeyMap() { reset_defaults(true); }

    // ResetKeysToEqDefaults: clears both banks, then the WASD (UseWASDDefault) or arrow-key movement set, the
    // common set, and the third set when the client's setting at 0xddfd54 is 3.
    void reset_defaults(bool use_wasd, bool set3 = false);
    const KeyCombo& binding(int slot, int bank) const;
    const KeyCombo& default_binding(int slot, int bank) const;
    // AttachKeyToEqCommand / AttachAltKeyToEqCommand: false (unchanged) for a reserved key. An unbound combo clears.
    bool bind(int command, int bank, const KeyCombo& combo);
    // Hailstone (the original's options window marks nothing): the combos (as KeyCombo ints, sorted) bound to two or
    // more of the listed commands, in either bank. A press of one runs each of those commands.
    std::vector<std::int32_t> shared_combos() const;

    // LoadAndSetKeymappings from a config (KEYMAP_ text first, then KEYMAPPING_ ints; others keep the default).
    void load(const Config& config);
    // Writes KEYMAPPING_ for every binding that differs from the defaults and removes the others (and any
    // legacy KEYMAP_ keys, as LoadAndSetKeymappings replaces them).
    void save(Config& config) const;

    // HandleKeyDown / HandleKeyUp: commands that start (down) and end (up) with a key.
    struct Event {
        int command = 0; // slot 0..499 (above 478: the unnamed slots)
        bool down = true;
    };
    void key_down(const KeyCombo& combo, std::vector<Event>& out);
    void key_up(std::uint8_t code, std::vector<Event>& out);
    // Ends every held command (focus lost, chat input took the keyboard).
    void release_all(std::vector<Event>& out);
    bool held(int command) const { return command >= 0 && command < kKeySlots && state_[static_cast<std::size_t>(command)]; }
    bool held(KeyCommand c) const { return held(static_cast<int>(c)); }

private:
    std::array<KeyCombo, kKeySlots> keys_[2]{};
    std::array<KeyCombo, kKeySlots> defaults_[2]{};
    std::array<bool, kKeySlots> state_{};
};

} // namespace hailstone::game
