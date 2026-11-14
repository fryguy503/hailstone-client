#include "game/keymap.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <utility>

#include "core/config.h"

namespace hailstone::game {

namespace {

constexpr std::uint32_t kShift = 0x40000000u, kCtrl = 0x20000000u, kAlt = 0x10000000u;

const char* const kNames[kKeyCommands] = {
#define HAILSTONE_KEY_COMMAND(index, name) #name,
#include "game/keymap_tables.inc"
#undef HAILSTONE_KEY_COMMAND
};

struct DefaultBinding {
    int set, bank, slot;
    std::uint32_t code;
};
constexpr DefaultBinding kDefaults[] = {
#define HAILSTONE_KEY_DEFAULT(set, bank, slot, code) {set, bank, slot, code},
#include "game/keymap_tables.inc"
#undef HAILSTONE_KEY_DEFAULT
};

struct CharKey {
    char c;
    std::uint8_t code;
};
constexpr CharKey kCharKeys[] = {
#define HAILSTONE_KEY_CHAR(c, code) {c, code},
#include "game/keymap_tables.inc"
#undef HAILSTONE_KEY_CHAR
};

struct NamedKey {
    std::int32_t code;
    const char* name;
};
constexpr NamedKey kNamedKeys[] = {
#define HAILSTONE_KEY_NAMED(code, name) {code, name},
#include "game/keymap_tables.inc"
#undef HAILSTONE_KEY_NAMED
};

struct KeyText {
    std::uint8_t code;
    const char* text;
};
constexpr KeyText kKeyTexts[] = {
#define HAILSTONE_KEY_TEXT(code, text) {code, text},
#include "game/keymap_tables.inc"
#undef HAILSTONE_KEY_TEXT
};

struct Label {
    int command;
    std::uint32_t eqstr, category;
};
constexpr Label kLabels[] = {
#define HAILSTONE_KEY_LABEL(command, eqstr, category) {command, eqstr, category},
#include "game/keymap_tables.inc"
#undef HAILSTONE_KEY_LABEL
};

bool iequal_prefix(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(s[i])) != std::toupper(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

} // namespace

KeyCombo KeyCombo::from_int(std::int32_t v) {
    auto u = static_cast<std::uint32_t>(v);
    KeyCombo k;
    k.code = static_cast<std::uint8_t>(u & 0xff);
    k.shift = (u & kShift) != 0;
    k.ctrl = (u & kCtrl) != 0;
    k.alt = (u & kAlt) != 0;
    return k;
}

std::int32_t KeyCombo::to_int() const {
    std::uint32_t u = code;
    if (shift) u |= kShift;
    if (ctrl) u |= kCtrl;
    if (alt) u |= kAlt;
    return static_cast<std::int32_t>(u);
}

const char* command_name(int command) {
    return command >= 0 && command < kKeyCommands ? kNames[command] : "";
}

std::optional<int> command_by_name(std::string_view name) {
    for (int i = 0; i < kKeyCommands; ++i)
        if (name == kNames[i]) return i;
    return std::nullopt;
}

std::uint32_t command_label_eqstr(int command) {
    for (const Label& l : kLabels)
        if (l.command == command) return l.eqstr;
    return 0;
}

std::uint32_t command_category(int command) {
    for (const Label& l : kLabels)
        if (l.command == command) return l.category;
    return 0;
}

bool key_command_dropped(int command) {
    using C = KeyCommand;
    // Real estate: the items, manage and plot search windows and the item placement keys are kept; the trophies and
    // help windows (none here) and the leave key (its __ExecuteCmd arm not traced) are not.
    if (command == static_cast<int>(C::CMD_REAL_ESTATE_TROPHIES_WINDOW) ||
        command == static_cast<int>(C::CMD_REAL_ESTATE_HELP_WINDOW) || command == static_cast<int>(C::CMD_REAL_ESTATE_LEAVE))
        return true;
    switch (static_cast<C>(command)) {
    case C::CMD_TOGGLEVOICEWIN:
    case C::CMD_TOGGLE_AUDIO_TRIGGER_WINDOW:
    case C::OPEN_ETN_HELP:
    case C::CMD_TOGGLE_CLAIM_WIN:
    case C::CMD_MARKETPLACE_SILVER:
    case C::CMD_MARKETPLACE_GOLD:
    case C::CMD_VOTE_RESULTS:
    case C::CMD_TOGGLE_TCG:
    case C::CMD_HISTORIC_ALERTS:
    case C::CMD_SOCIAL_SHARE:
    // The customer service (ticket) key runs Cmd_petition with no text (__ExecuteCmd case 108, as OPEN_ETN_HELP),
    // which opens Daybreak's petition web page; the welcome screen (case 133, Cmd_welcomescreen @0x4fd4a0) is
    // Daybreak's news page in the HTML window. Neither has a server side here.
    case C::CMD_TOGGLE_TICKET_WND:
    case C::CMD_WELCOMESCREEN: return true;
    default: return false;
    }
}

const std::vector<int>& listed_commands() {
    static const std::vector<int> list = [] {
        std::vector<int> v;
        for (const Label& l : kLabels)
            if (l.command >= 0 && l.command < kKeyCommands && !key_command_dropped(l.command) &&
                std::find(v.begin(), v.end(), l.command) == v.end())
                v.push_back(l.command);
        return v;
    }();
    return list;
}

std::string key_text(const KeyCombo& k) {
    if (k.code == 0) return "-------";
    std::string s;
    if (k.shift && k.ctrl && k.alt) s = "ShCtlAlt ";
    else if (k.shift && k.ctrl) s = "ShCtl ";
    else if (k.shift && k.alt) s = "ShAlt ";
    else if (k.ctrl && k.alt) s = "CtlAlt ";
    else if (k.shift) s = "Shift ";
    else if (k.ctrl) s = "Ctrl ";
    else if (k.alt) s = "Alt ";
    for (const KeyText& t : kKeyTexts)
        if (t.code == k.code) return s + t.text;
    // ToAscii with no modifiers held (a US layout), upper-cased; tab, enter, backspace and escape excluded.
    if (k.code != 0x0f && k.code != 0x1c && k.code != 0x0e && k.code != 0x01) {
        for (const CharKey& c : kCharKeys)
            if (c.code == k.code) return s + static_cast<char>(std::toupper(static_cast<unsigned char>(c.c)));
        if (k.code == 0x39) return s + " ";
    }
    return s + "?";
}

std::int32_t parse_keymap_text(std::string_view value) {
    if (value == "*NONE*") return -2;
    std::uint32_t code = 0;
    std::string v(value);
    for (int pass = 0; pass < 3; ++pass) {
        for (auto [prefix, flag] : {std::pair{std::string_view("SHIFT+"), kShift}, std::pair{std::string_view("CONTROL+"), kCtrl},
                                    std::pair{std::string_view("ALT+"), kAlt}}) {
            if (!iequal_prefix(v, prefix)) continue;
            code |= flag;
            v.erase(0, prefix.size());
            break;
        }
    }
    if (v.size() == 1) {
        for (const CharKey& c : kCharKeys)
            if (c.c == v[0]) return static_cast<std::int32_t>(code + c.code);
        return -1;
    }
    for (const NamedKey& n : kNamedKeys) {
        std::string_view name(n.name);
        if (name.size() != v.size() || !iequal_prefix(v, name)) continue;
        if (n.code == -1) return -1; // NOKEY: code | 0xffffffff
        return static_cast<std::int32_t>(code + static_cast<std::uint32_t>(n.code));
    }
    return -1;
}

bool is_reserved_key(const KeyCombo& k) {
    static const std::int32_t reserved[] = {0x2a, 0x36, 0x1d, 0x9d, 0x38, 0xb8, 0xdb, 0xdc, 0x1c, 0x35, 0x1000001c, 0x1000000f};
    std::int32_t v = k.to_int();
    return std::find(std::begin(reserved), std::end(reserved), v) != std::end(reserved);
}

std::string keymapping_key(int command, int bank) {
    return std::string("KEYMAPPING_") + command_name(command) + "_" + std::to_string(bank + 1);
}

void KeyMap::reset_defaults(bool use_wasd, bool set3) {
    for (auto& bank : keys_) bank.fill(KeyCombo{});
    auto apply = [&](int set) {
        for (const DefaultBinding& d : kDefaults)
            if (d.set == set && !key_command_dropped(static_cast<int>(d.slot))) keys_[d.bank][static_cast<std::size_t>(d.slot)] = KeyCombo::from_int(static_cast<std::int32_t>(d.code));
    };
    apply(use_wasd ? 0 : 1);
    apply(2);
    if (set3) apply(3);
    // A Hailstone change: Tab targets the nearest NPC (TARGETNPC, F8 its second key) instead of the original's
    // TOGGLETARGET, which is left unbound.
    keys_[0][static_cast<std::size_t>(KeyCommand::TOGGLETARGET)] = KeyCombo{};
    keys_[0][static_cast<std::size_t>(KeyCommand::TARGETNPC)] = KeyCombo{0x0f};  // Tab
    keys_[1][static_cast<std::size_t>(KeyCommand::TARGETNPC)] = KeyCombo{0x42};  // F8
    defaults_[0] = keys_[0];
    defaults_[1] = keys_[1];
    state_.fill(false);
}

const KeyCombo& KeyMap::binding(int slot, int bank) const {
    static const KeyCombo none{};
    if (slot < 0 || slot >= kKeySlots || bank < 0 || bank > 1) return none;
    return keys_[bank][static_cast<std::size_t>(slot)];
}

const KeyCombo& KeyMap::default_binding(int slot, int bank) const {
    static const KeyCombo none{};
    if (slot < 0 || slot >= kKeySlots || bank < 0 || bank > 1) return none;
    return defaults_[bank][static_cast<std::size_t>(slot)];
}

std::vector<std::int32_t> KeyMap::shared_combos() const {
    std::vector<std::pair<std::int32_t, int>> uses; // {combo, command}
    for (int cmd : listed_commands())
        for (int bank = 0; bank < 2; ++bank)
            if (const KeyCombo& k = binding(cmd, bank); k.bound()) uses.emplace_back(k.to_int(), cmd);
    std::sort(uses.begin(), uses.end());
    uses.erase(std::unique(uses.begin(), uses.end()), uses.end()); // one command on a key in both banks is no clash
    std::vector<std::int32_t> out;
    for (std::size_t i = 1; i < uses.size(); ++i)
        if (uses[i].first == uses[i - 1].first && (out.empty() || out.back() != uses[i].first)) out.push_back(uses[i].first);
    return out;
}

bool KeyMap::bind(int command, int bank, const KeyCombo& combo) {
    if (command < 0 || command >= kKeySlots || bank < 0 || bank > 1) return false;
    if (combo.bound() && is_reserved_key(combo)) return false;
    keys_[bank][static_cast<std::size_t>(command)] = combo;
    return true;
}

void KeyMap::load(const Config& config) {
    for (int cmd = 0; cmd < kKeyCommands; ++cmd) {
        for (int bank = 0; bank < 2; ++bank) {
            std::string legacy = std::string("KEYMAP_") + command_name(cmd) + (bank == 1 ? "_2" : "");
            std::int32_t v = parse_keymap_text(config.get_string(legacy, "*NONE*"));
            if (v > 0 || v == -1) {
                keys_[bank][static_cast<std::size_t>(cmd)] = KeyCombo::from_int(v == -1 ? 0 : v);
                continue;
            }
            std::int32_t stored = config.get_int(keymapping_key(cmd, bank), -1);
            if (stored != -1) keys_[bank][static_cast<std::size_t>(cmd)] = KeyCombo::from_int(stored);
        }
    }
}

void KeyMap::save(Config& config) const {
    for (int cmd = 0; cmd < kKeyCommands; ++cmd) {
        for (int bank = 0; bank < 2; ++bank) {
            config.erase(std::string("KEYMAP_") + command_name(cmd) + (bank == 1 ? "_2" : ""));
            const KeyCombo& k = keys_[bank][static_cast<std::size_t>(cmd)];
            if (k == defaults_[bank][static_cast<std::size_t>(cmd)]) config.erase(keymapping_key(cmd, bank));
            else config.set_int(keymapping_key(cmd, bank), k.to_int());
        }
    }
}

void KeyMap::key_down(const KeyCombo& combo, std::vector<Event>& out) {
    for (int bank = 0; bank < 2; ++bank)
        for (int cmd = 0; cmd < kKeySlots; ++cmd) {
            auto i = static_cast<std::size_t>(cmd);
            if (keys_[bank][i] == combo && !state_[i]) {
                out.push_back({cmd, true});
                state_[i] = true;
            }
        }
}

void KeyMap::key_up(std::uint8_t code, std::vector<Event>& out) {
    for (int cmd = 0; cmd < kKeySlots; ++cmd) {
        auto i = static_cast<std::size_t>(cmd);
        if (!state_[i]) continue;
        if (keys_[0][i].code != code && keys_[1][i].code != code) continue;
        out.push_back({cmd, false});
        state_[i] = false;
    }
}

void KeyMap::release_all(std::vector<Event>& out) {
    for (int cmd = 0; cmd < kKeySlots; ++cmd)
        if (state_[static_cast<std::size_t>(cmd)]) {
            out.push_back({cmd, false});
            state_[static_cast<std::size_t>(cmd)] = false;
        }
}

} // namespace hailstone::game
