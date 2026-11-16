// The chat functions of the hs table over the client's chat
// lines (ui::ChatView), in up to 32 chat windows (the chat manager below). Lines are given to the scripts as STML in the colour the
// original uses for their channel: the user colour of the channel (eqlib ChatFilters.h USERCOLOR_*: say 256, tell
// 257, ...) as eqclient.ini [TextColors] User_<n> sets it (n = colour - 255).

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "core/character_files.h"
#include "core/game_dir.h"
#include "core/ini_file.h"
#include "core/log.h"
#include "core/path_utf8.h"
#include "game/chat_windows.h"
#include "game/combat.h"
#include "game/eq_names.h"
#include "game/item_links.h"
#include "game/spells.h"
#include "net/game_connection.h"
#include "plugin/lua_keep.h"
#include "platform/local_time.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"
#include "ui/ui_ini.h"

namespace hailstone::app {

std::uint32_t chat_index_rgb(int index);
std::uint32_t user_color_rgb(int index);

namespace {

// The default user colours (0xac2c40, {r, g, b, pad} each; __SaveColors).
constexpr std::array<std::uint32_t, 99> kRoF2DefaultUserColors{
    0xffffff, 0xbe28be, 0x00ffff, 0x28f028, 0x008000, 0x008000, 0xff0000, 0x5a5aff,
    0x5a5aff, 0xffffff, 0xff0000, 0xffffff, 0xffffff, 0xffff00, 0x5a5aff, 0xffffff,
    0xffffff, 0xffffff, 0xffffff, 0xf0f000, 0xf0f000, 0xffffff, 0xffffff, 0xffffff,
    0xffffff, 0x800080, 0xffffff, 0x5a5aff, 0xf0f000, 0x008c00, 0x5a5aff, 0xff0000,
    0x5a5aff, 0xff0000, 0xd79a42, 0x6e8fb0, 0x6e8fb0, 0x6e8fb0, 0x6e8fb0, 0x6e8fb0,
    0x6e8fb0, 0x6e8fb0, 0x6e8fb0, 0x6e8fb0, 0x6e8fb0, 0xffffff, 0xffffff, 0xff0000,
    0xff0000, 0xff0000, 0xff0000, 0xffffff, 0xffffff, 0xffffff, 0xffffff, 0xffffff,
    0xffffff, 0xffffff, 0xffffff, 0xd79a42, 0xd79a42, 0xd79a42, 0xd79a42, 0xd79a42,
    0xd79a42, 0xd79a42, 0xd79a42, 0xd79a42, 0xd79a42, 0xffff00, 0xff00ff, 0x00c8c8,
    0xffffff, 0xffffff, 0x00ffff, 0xff0000, 0xffffff, 0x5a5aff, 0xffff00, 0xffff00,
    0xffffff, 0xffffff, 0xffffff, 0xffffff, 0xffffff, 0xff9b9b, 0x5a5aff, 0xffffff,
    0xffffff, 0xffffff, 0xffffff, 0xff7f00, 0xffffff, 0xffffff, 0xffffff, 0xc00000,
    0x00ff00, 0xffff00, 0xff0000};

// Hailstone (a deliberate difference, game/chat_windows.h): the user colours 99..152 for the extended colours
// 355..408, which only a server sends. Their defaults are Hailstone's own picks from related RoF2 colours (and the
// con colours for the seven consider colours), not a later client's palette.
const auto kDefaultUserColors = [] {
    std::array<std::uint32_t, game::kChatUserColorCount> colors{};
    colors.fill(kRoF2DefaultUserColors[17]);
    std::copy(kRoF2DefaultUserColors.begin(), kRoF2DefaultUserColors.end(), colors.begin());
    auto inherit = [&](std::initializer_list<int> destinations, int source) {
        for (int color : destinations)
            colors[static_cast<std::size_t>(color - 256)] = kRoF2DefaultUserColors[static_cast<std::size_t>(source - 256)];
    };
    inherit({355}, 326); // dialog links: item links
    inherit({356}, 305); // flurries
    inherit({357, 365}, 335); // diagnostic and event messages
    inherit({358}, 278); // NPC deaths
    inherit({359, 360}, 287); // random rolls
    inherit({361, 362, 374, 375, 379, 397, 402}, 283); // spell and environmental numbers
    inherit({363, 364}, 329); // damage shields
    inherit({366, 367}, 284); // overwritten buffs
    inherit({368, 369, 370, 406}, 289); // command and ability failures
    inherit({371}, 286); // destroyed items
    inherit({372, 378}, 264); // own auras and songs
    inherit({373, 380}, 288); // others' auras and spell emotes
    inherit({376, 377, 401}, 325); // damage over time
    inherit({381}, 274); // faction links
    inherit({382}, 270); // taunt
    inherit({383}, 271); // disciplines
    inherit({384, 386}, 259); // positive changes
    inherit({385}, 262); // negative changes
    inherit({388, 407}, 273); // general information
    inherit({389}, 327); // raid victories
    inherit({408}, 269); // alerts
    inherit({398, 399, 400, 403, 404}, 302); // critical spell and healing numbers
    inherit({405}, 301); // critical melee numbers
    colors[387 - 256] = 0x808080; // locked encounters
    // Routing a consider through the editable palette retains Hailstone's
    // existing difficulty colours until the player chooses replacements.
    constexpr std::array consider{game::Con::Red, game::Con::Yellow, game::Con::White,
                                  game::Con::DarkBlue, game::Con::LightBlue, game::Con::Green, game::Con::Gray};
    for (std::size_t index = 0; index < consider.size(); ++index)
        colors[390 - 256 + index] = game::con_rgb(consider[index]);
    return colors;
}();

constexpr std::array<std::string_view, game::kChatUserColorCount - 99> kExtendedColorNames{
    "Dialog links", "Your flurries", "Debug output", "NPC deaths", "Other players' rolls", "Group and raid rolls",
    "Your environmental damage", "Others' environmental damage", "Damage shields hitting you", "Other damage shields",
    "Event messages", "Detrimental buffs replaced", "Beneficial buffs replaced", "Unavailable commands",
    "Combat ability recovery", "Alternate ability recovery", "Destroyed items", "Your auras", "Other auras",
    "Your healing", "Other healing", "Your damage over time", "Other damage over time", "Bard songs",
    "Other direct spell damage", "Spell emotes", "Faction links", "Taunts", "Other combat abilities",
    "Item stat increases", "Item stat decreases", "Attackable encounters", "Locked encounters", "Food and drink",
    "Raid victories", "Consider: red", "Consider: yellow", "Consider: white", "Consider: blue", "Consider: light blue",
    "Consider: green", "Consider: gray", "Direct spell damage", "Other critical spell damage",
    "Your critical damage over time", "Other critical damage over time", "Damage over time received", "Healing received",
    "Your critical healing", "Other critical healing", "Other critical melee hits", "Other spell failures",
    "Client Messages", "Alerts"};

struct ChatState {
    UiHost* host = nullptr;
    std::array<std::uint32_t, game::kChatUserColorCount> user{}; // RGB of colours 256..408 (index colour - 256)
    bool colors_loaded = false;
    std::filesystem::path chat_ini; // the character's chat.ini (core/character_files.h); empty: eqclient.ini alone
    std::deque<std::string> history;       // newest last
    int history_at = -1;                   // -1: not browsing
    std::optional<std::string> take_focus; // what the chat keys put into the input (request_chat_focus)
    bool replace_focus = false;            // take_focus replaces what is typed (a tell menu's name)
    int focus_window = -1;                 // the window take_focus goes to instead of the active one (a name link's
                                           // tell window)
    std::filesystem::path history_file;    // Hailstone: the character's chat_history.txt (save_chat_history)
    bool history_restored = false;         // its lines are in the chat (until the session's end saves them)
    std::optional<std::vector<std::string>> link_all; // the loot window's Link All, for the active window's input
    int page = 0;                          // the PAGEUP (-1) / PAGEDOWN (1) key not yet taken (hs.ChatPage)
    int page_window = -1;                  // the window it pages
    std::uint64_t colors = 0;              // bumped by a user colour's change (set_user_color)
};

// Hailstone: the lines each chat window (a tab is one) keeps, Hailstone's own limit (not taken from the original), so
// a busy window (combat, spells) no longer pushes the lines out of a quiet one (tells, guild).
constexpr std::size_t kWindowLines = 1000;

// Hailstone's chat history (save_chat_history): how many lines it reads back, its file's first line, and the line
// put where the restored lines end. It keeps what the windows keep (trim_windows).
constexpr std::size_t kHistoryLines = 32000;
constexpr std::string_view kHistoryHeader = "# Hailstone chat history 2";
constexpr std::string_view kHistoryHeaderV1 = "# Hailstone chat history 1"; // no time column
constexpr std::string_view kHistoryMark = "-- the chat from before --";

ChatState& state() {
    static ChatState s;
    return s;
}

void load_colors() {
    ChatState& s = state();
    if (s.colors_loaded || !s.host || !s.host->game) return;
    s.colors_loaded = true;
    // eqclient.ini [TextColors] in the original; here the character's chat.ini over the asset folder's eqclient.ini.
    IniFile ini = load_chat_settings(*s.host->game, s.chat_ini);
    // The loader @0x48a160: the three keys as strings ("*NULL*" when missing); any missing takes the default.
    for (int n = 1; n <= static_cast<int>(s.user.size()); ++n) {
        auto read = [&](const char* part) -> std::optional<int> {
            auto v = ini.get("TextColors", std::format("User_{}_{}", n, part));
            if (!v || (!v->empty() && (*v)[0] == '*')) return std::nullopt;
            return std::atoi(v->c_str());
        };
        auto r = read("Red"), g = read("Green"), b = read("Blue");
        std::size_t i = static_cast<std::size_t>(n - 1);
        if (r && g && b) s.user[i] = static_cast<std::uint32_t>((*r & 0xff) << 16 | (*g & 0xff) << 8 | (*b & 0xff));
        else s.user[i] = kDefaultUserColors[i];
    }
}

// The original's colour for a channel of this client's chat.
std::uint32_t channel_rgb(ui::ChatChannel c) {
    using C = ui::ChatChannel;
    int color = 273; // USERCOLOR_DEFAULT
    switch (c) {
    case C::Say: color = 256; break;
    case C::Tell: color = 257; break;
    case C::Group: color = 258; break;
    case C::Guild: color = 259; break;
    case C::OOC: color = 260; break;
    case C::Auction: color = 261; break;
    case C::Shout: color = 262; break;
    case C::Emote: color = 263; break;
    case C::Spell: color = 264; break;
    case C::Combat: color = 265; break; // the chat events do not keep which combat colour the line had
    case C::Raid: color = 327; break; // USERCOLOR_RAID
    default: break;
    }
    if (c == C::Error) return 0xf00000; // CONCOLOR_RED, the basic colour 13 the messages carry
    return user_color_rgb(color - 256);
}

std::string escape(std::string_view text) {
    std::string out;
    for (char ch : text) {
        if (ch == '<') out += "&lt;";
        else if (ch == '>') out += "&gt;";
        else if (ch == '&') out += "&amp;";
        else out += ch;
    }
    return out;
}

// Hailstone (the modern chat): a player's name in a line links as <a WndNotify="1001,name">; a click opens a tell
// to them (open_tell).
constexpr int kNameLink = 1001;
constexpr int kAchievementLink = 1002;

// STML's attribute parser does not decode entities. Hex keeps the exact received
// metadata out of markup syntax, including quotes and literal entity-like text.
std::string achievement_link_data(std::string_view metadata) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(metadata.size() * 2);
    for (const unsigned char ch : metadata) {
        out.push_back(digits[ch >> 4]);
        out.push_back(digits[ch & 15]);
    }
    return out;
}

std::optional<std::string> achievement_link_metadata(std::string_view data) {
    if (data.empty() || data.size() > net::kMaxAchievementLinkBytes * 2 || data.size() % 2)
        return std::nullopt;
    const auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(data.size() / 2);
    for (std::size_t i = 0; i < data.size(); i += 2) {
        const int high = digit(data[i]), low = digit(data[i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return out;
}

bool iequal(std::string_view a, std::string_view b);

// The line's player when it can be told: one word of letters (an NPC's name has spaces), not ourselves.
std::string link_name(const ui::ChatLine& l) {
    if (l.peer.empty() || l.peer.size() > 0x3f ||
        !std::all_of(l.peer.begin(), l.peer.end(), [](char ch) { return std::isalpha(static_cast<unsigned char>(ch)) != 0; }))
        return {};
    UiHost* h = state().host;
    if (h && h->conn && iequal(l.peer, h->conn->character_name())) return {};
    return l.peer;
}

// The text with the first whole-word `name` in it as a name link (once: `name` is cleared then).
std::string escape_linking(std::string_view text, std::string& name) {
    if (name.empty()) return escape(text);
    auto letter = [](char ch) { return std::isalpha(static_cast<unsigned char>(ch)) != 0; };
    for (std::size_t at = text.find(name); at != std::string_view::npos; at = text.find(name, at + 1)) {
        std::size_t end = at + name.size();
        if ((at > 0 && letter(text[at - 1])) || (end < text.size() && letter(text[end]))) continue;
        std::string out = escape(text.substr(0, at)) + std::format("<a WndNotify=\"{},{}\">{}</a>", kNameLink, name, name) +
                          escape(text.substr(end));
        name.clear();
        return out;
    }
    return escape(text);
}

// One line as STML: its colour, item links as <a WndNotify="27,data"> (the tag between the two \x12, encoded by
// game::stml_link_data); with `names`, its player's name as a name link.
// Hailstone: "[hh:mm:ss] " in grey before a line (local time), nothing for a line of unknown time.
// hs.ChatText formats every line of a window again each time a line comes, and each local time read the time zone
// again (glibc stats /etc/localtime every call): the stamps were most of a chat window's frame in a fight. Time zones
// differ from UTC, and change, by whole minutes, so a minute's hour and minute are worked out once (the last 64 kept;
// a time zone changed while the client runs shows in the minutes after).
std::string timestamp_stml(std::int64_t time) {
    if (time <= 0) return {};
    struct Minute {
        std::int64_t minute = -1;
        int hour = 0, min = 0;
    };
    static std::array<Minute, 64> minutes; // (main thread: the Lua UI's)
    const std::int64_t minute = time / 60;
    Minute& m = minutes[static_cast<std::size_t>(minute % 64)];
    if (m.minute != minute) {
        auto tm = platform::local_time(static_cast<std::time_t>(minute * 60));
        if (!tm) return {};
        m = {minute, tm->tm_hour, tm->tm_min};
    }
    return std::format("<c \"#8a8a8a\">[{:02d}:{:02d}:{:02d}] </c>", m.hour, m.min, time % 60);
}

std::string line_stml(const ui::ChatLine& l, bool names, bool stamps = false) {
    std::uint32_t rgb = l.rgb ? l.rgb : l.color ? chat_index_rgb(static_cast<int>(l.color)) : channel_rgb(l.channel);
    std::string out = (stamps ? timestamp_stml(l.time) : std::string()) + std::format("<c \"#{:06x}\">", rgb & 0xffffff);
    std::string name = names ? link_name(l) : std::string();
    std::string_view text = l.text;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t a = text.find('\x12', at);
        std::size_t b = a == std::string::npos ? std::string::npos : text.find('\x12', a + 1);
        if (b == std::string::npos) {
            out += escape_linking(text.substr(at), name);
            break;
        }
        out += escape_linking(text.substr(at, a - at), name);
        std::string tag = l.text.substr(a + 1, b - a - 1);
        if (!tag.empty() && tag.front() == '3') {
            const auto achievement = game::parse_achievement_chat_link(tag);
            std::string shown = achievement ? achievement->text : "Achievement";
            UiHost* h = state().host;
            const auto* catalog = h && h->conn && h->conn->achievements().catalog()
                ? &*h->conn->achievements().catalog() : nullptr;
            const auto parsed = achievement && catalog
                ? net::parse_achievement_link(achievement->metadata, *catalog) : std::nullopt;
            const auto* definition = parsed ? catalog->find(parsed->achievement_id) : nullptr;
            if (definition) {
                shown = definition->name;
                out += std::format("<a WndNotify=\"{},{}\">{}</a>", kAchievementLink,
                    achievement_link_data(parsed->metadata), escape(shown));
            } else out += escape(shown);
            at = b + 1;
            continue;
        }
        auto link = game::parse_item_link(tag);
        std::string shown = link ? link->text : tag;
        out += std::format("<a WndNotify=\"27,{}\">{}</a>", game::stml_link_data(tag), escape(shown));
        at = b + 1;
    }
    return out + "</c>";
}

UiHost& host(lua_State* L) { return *static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1))); }

} // namespace

std::uint32_t user_color_rgb(int index) {
    load_colors(); // CDisplay::GetUserDefinedColor: an index outside 0..98 (here 0..152) reads colour 0
    return state().user[static_cast<std::size_t>(index >= 0 && index < game::kChatUserColorCount ? index : 0)];
}

std::uint32_t default_user_color_rgb(int index) {
    return kDefaultUserColors[static_cast<std::size_t>(index >= 0 && index < game::kChatUserColorCount ? index : 0)];
}

std::string_view extended_chat_color_name(int index) {
    return index >= 99 && index < game::kChatUserColorCount ?
        kExtendedColorNames[static_cast<std::size_t>(index - 99)] : std::string_view{};
}

void set_user_color(UiHost& host, int index, std::uint32_t rgb) {
    if (index < 0 || index >= game::kChatUserColorCount) return; // SetUserDefinedColor ignores the rest (0..98 there)
    load_colors();
    state().user[static_cast<std::size_t>(index)] = rgb & 0xffffff;
    ++state().colors; // (the chat windows' lines in the new colour: hs.ChatText's versions change)
    (void)host;
    // COptionsWnd::SaveColors: eqclient.ini [TextColors] User_<n>_Red / _Green / _Blue, here into the character's
    // chat.ini (the asset folder is only read).
    const std::filesystem::path& chat_ini = state().chat_ini;
    if (chat_ini.empty()) return;
    std::string base = std::format("User_{}_", index + 1);
    std::string red = base + "Red", green = base + "Green", blue = base + "Blue";
    if (!save_chat_settings(chat_ini, "TextColors",
                            {{red, std::to_string((rgb >> 16) & 0xff)},
                             {green, std::to_string((rgb >> 8) & 0xff)},
                             {blue, std::to_string(rgb & 0xff)}})) log::warn("cannot write {}", path_utf8(chat_ini));
}

// CChatWindowManager::GetRGBAFromIndex @0x6508a0: the basic colours below 256, the eqclient.ini user colours from 256
// (CDisplay::GetUserDefinedColor). Index 22 returns a code address in the original (not a colour); it is taken as
// the default here.
std::uint32_t chat_index_rgb(int index) {
    if (index > 0xff) return user_color_rgb(index - 256);
    switch (index) {
    case 0:
    case 10: return 0xf0f0f0;
    case 2: return 0x008000;
    case 4: return 0x0040ff;
    case 5: return 0xf000f0;
    case 6: return 0x808080;
    case 7: return 0xe0e0e0;
    case 12: return 0xa0a0a0;
    case 13: return 0xf00000;
    case 14: return 0x00f000;
    case 15: return 0xf0f000;
    case 16: return 0x0000f0;
    case 17: return 0x0000af;
    case 18: return 0x00f0f0;
    case 20: return 0x000000;
    case 21: return 0xf0a000;
    default: return 0x606060;
    }
}

std::string stml_escape(std::string_view text) { return escape(text); }

namespace {

// ---------------------------------------------------------------- the chat windows (CChatWindowManager)
//
// Up to 32 chat windows (CChatWindowManager::ChatWnd, NumWindows +0x80), window 0 the main one. Every line goes by
// its colour to one of the 57 chat channels (game/chat_windows.h; Hailstone's extended channels 57..107 after them)
// and from there to the one window ChannelMap gives it (the main window unless its Filters menu moved it); a line a
// window wrote itself (CChatWindow::AddOutputText: the menus' answers) shows only there. Each window keeps its name, the channel its plain typed text
// goes to (ChatChannel, with the tell target or the chat channel's index), its language, scrollbar and font.
//
// This client's addition: a window can be shown as a tab of another (`dock`); every tab is still a chat window of
// its own, with its own channels. Tell windows (the Options' Use Tell Windows: CEverQuest::DoTellWindow instead of
// the routed line) are tabs of the first one.
//
// Kept in the character's UI ini as the original keeps them ([ChatManager] NumWindows, ChatWindow<n>_Name /
// _DefaultChannel / _ChatChannel / _TellTarget / _Scrollbar / _FontStyle / _LanguageId, LockedActiveWindow,
// ChannelMap<n>, HitMode<n>); the tabs as ChatWindow<n>_Dock and _Up.

constexpr int kMaxChatWindows = 32;

struct ChatWnd {
    bool used = false;
    std::uint32_t serial = 0;   // the lines it wrote itself carry it (a later window in the slot has another)
    std::string name;           // its title (_Name)
    int channel = 8;            // +0x22c ChatChannel (8 say, 7 tell, 2 group, 0 guild, 5 OOC, 4 auction, 0xf raid,
                                // 0x14 a chat channel)
    int chat_index = 0;         // +0x230: the chat channel's index in the joined list
    std::string tell_target;    // +0x234 (a tell window's player; the chat channel's name /channel gave)
    bool scrollbar = true;      // the output's WindowStyle bit 0
    int font = 3;               // CChatWindow::SetChatFont's index
    int language = 0;           // +0x274
    bool tell = false;          // bIsTellWnd
    int dock = -1;              // shown as a tab of this window (-1: a window of its own)
    int up = -1;                // a window with tabs: the one that is up (-1 itself)
    bool select = false;        // the script is to bring `up` up (it changed here)
    std::uint64_t cleared = 0, seen = 0; // ChatView::revision when cleared / last shown
    std::uint64_t tells_in = 0; // Hailstone: the tells from others routed to it (the modern tell window flashes)
    // What hs.ChatText shows of it (scan_windows): the first and last line's seq and the count, and a generation
    // that changes whenever its lines change in another way than lines added at the end or leaving from the front.
    std::uint64_t gen = 0, first = 0, last = 0;
    std::size_t count = 0;
};

struct ChatManager {
    std::array<ChatWnd, kMaxChatWindows> wnd{};
    int num_windows = 1;        // NumWindows: the slots below it are in use or freed
    int active = 0;             // CurrentActive
    int locked = 0;             // LockedActive (Always Chat Here; -1: none); the main window by default (set_locked)
    std::array<int, game::kChatChannelCount> channel_map{}; // the window of each channel
    std::array<int, game::kChatHitModeCount> hit_modes{};
    game::SpellHitModes spell_hit_modes{};
    int default_channel = 8;    // eqclient.ini [Defaults] DefaultChannel
    bool tell_windows = false;
    std::uint64_t synced = 0;   // ChatView::revision the tell windows were made up to
    std::uint64_t serial = 1;   // bumped by every change (every window's hs.ChatText version changes)
    std::uint32_t next_serial = 1;
    const ui::ChatView* scanned = nullptr; // what scan_windows last looked at
    std::uint64_t scanned_revision = 0, scanned_serial = 0, scanned_colors = 0, scanned_achievement_catalog = 0;
    std::uint64_t next_gen = 1;                             // ChatWnd::gen's source (never reset)
    ui::UiIni* ini = nullptr;

    ChatManager() { reset(); }
    void reset() {
        wnd = {};
        wnd[0].used = true;
        wnd[0].serial = next_serial++;
        num_windows = 1;
        active = 0;
        locked = 0;
        channel_map.fill(0);
        hit_modes.fill(0);
        spell_hit_modes.fill(0);
        tell_windows = false;
        synced = 0;
        ++serial;
    }
};

ChatManager& manager() {
    static ChatManager m;
    return m;
}

ChatWnd& wnd(int w) { return manager().wnd[static_cast<std::size_t>(w)]; }
bool used(int w) { return w >= 0 && w < kMaxChatWindows && wnd(w).used; }

UiHost* chat_host() { return state().host; }

std::string eqstr(std::uint32_t id, std::vector<std::string> args = {}) {
    UiHost* h = chat_host();
    return h && h->conn ? h->conn->strings().format(id, args) : std::string();
}

// Tells in their own windows: UI [Main] UseTellWindows (0xde0d58, CDisplay's ReadUIINIBool, off by default), the
// Options window's box, which msg_new_text @0x4c0080 reads before CEverQuest::DoTellWindow. The character's option
// once a character is chosen; the layout's [ChatManager] TellWindows before.
net::GameConnection* character_conn() {
    UiHost* h = chat_host();
    return h && h->conn && !h->conn->character_name().empty() ? h->conn : nullptr;
}
bool tell_windows_on() {
    if (net::GameConnection* c = character_conn()) return c->client_option(game::ClientOption::UseTellWindows);
    return manager().tell_windows;
}

bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// The window's settings under ChatWindow<n>_ (CChatWindow_SaveSettings @0x655d10) and the tabs' keys.
void save_window(int w) {
    ChatManager& m = manager();
    const ChatWnd& c = wnd(w);
    std::string k = std::format("ChatWindow{}_", w);
    auto put = [&](const std::string& key, const std::string& v) { m.ini->put("ChatManager", k + key, v); };
    put("LanguageId", std::to_string(c.language));
    put("DefaultChannel", std::to_string(c.channel));
    put("ChatChannel", std::to_string(c.chat_index));
    put("TellTarget", c.tell_target);
    put("Scrollbar", c.scrollbar ? "1" : "0");
    put("FontStyle", std::to_string(c.font));
    put("Name", c.name);
    put("Dock", std::to_string(c.dock));
    put("Up", std::to_string(c.up));
}

// CChatWindowManager::SaveChatSettings @0x64fe40. The original counts NumWindows less its tell windows, which then
// load in the place of the windows after them; tell windows are not kept here, and an unused slot's keys go.
void save_manager() {
    ChatManager& m = manager();
    if (!m.ini) return;
    m.ini->put("ChatManager", "TellWindows", tell_windows_on() ? "1" : "0");
    int last = 0;
    for (int w = 1; w < m.num_windows; ++w)
        if (used(w) && !wnd(w).tell) last = w;
    m.ini->put("ChatManager", "NumWindows", std::to_string(last));
    m.ini->put("ChatManager", "LockedActiveWindow", std::to_string(used(m.locked) && !wnd(m.locked).tell ? m.locked : -1));
    for (int w = 0; w < kMaxChatWindows; ++w) {
        if (used(w) && !wnd(w).tell) {
            save_window(w);
            continue;
        }
        if (w == 0) continue;
        m.ini->erase("ChatManager", std::format("ChatWindow{}_Language", w));
        m.ini->erase("ChatManager", std::format("ChatWindow{}_LanguageId", w));
    }
    for (int ch = 0; ch < game::kChatChannelCount; ++ch) {
        int w = m.channel_map[static_cast<std::size_t>(ch)];
        m.ini->put("ChatManager", std::format("ChannelMap{}", ch), std::to_string(used(w) && !wnd(w).tell ? w : 0));
    }
    for (int i = 0; i < game::kChatHitModeCount; ++i)
        m.ini->put("ChatManager", std::format("HitMode{}", i), std::to_string(m.hit_modes[static_cast<std::size_t>(i)]));
    for (std::size_t i = 0; i < m.spell_hit_modes.size(); ++i)
        m.ini->put("ChatManager", std::format("SpellHitMode{}", i), std::to_string(m.spell_hit_modes[i]));
}

void changed() {
    ++manager().serial;
    save_manager();
}

// The categories ReportHit_* reads (the first six native modes) and the separate spell modes go to the connection.
void push_hit_modes() {
    UiHost* h = chat_host();
    if (!h || !h->conn) return;
    game::HitModes modes{};
    for (std::size_t i = 0; i < modes.size(); ++i) modes[i] = manager().hit_modes[i];
    h->conn->set_hit_modes(modes);
    h->conn->set_spell_hit_modes(manager().spell_hit_modes);
}

// Keep Lua's existing category 7 = ALL; Hailstone's spell categories start at 8.
std::string hit_category_name(int category) {
    if (category < 6) return eqstr(game::hit_category_eqstr(category));
    if (category == 6) return eqstr(game::kStrAll);
    return std::string(game::spell_hit_category_name(category - 7));
}

int hit_mode_value(int category) {
    const auto& m = manager();
    if (category < 6) return m.hit_modes[static_cast<std::size_t>(category)];
    if (category > 6) return m.spell_hit_modes[static_cast<std::size_t>(category - 7)];
    const int mode = m.hit_modes[0];
    for (std::size_t i = 1; i < game::HitModes{}.size(); ++i)
        if (m.hit_modes[i] != mode) return -1;
    for (auto spell_mode : m.spell_hit_modes)
        if (spell_mode != mode) return -1;
    return mode;
}

void set_hit_mode(int category, int mode) {
    auto& m = manager();
    if (category < 6) m.hit_modes[static_cast<std::size_t>(category)] = mode;
    else if (category > 6) m.spell_hit_modes[static_cast<std::size_t>(category - 7)] = mode;
    else {
        m.hit_modes.fill(mode);
        m.spell_hit_modes.fill(mode);
    }
    push_hit_modes();
    changed();
}

// The colour a line goes by: its own, else its channel's user colour.
std::uint32_t color_of(const ui::ChatLine& l) {
    if (l.color) return l.color;
    using C = ui::ChatChannel;
    switch (l.channel) {
    case C::Say: return 256;
    case C::Tell: return 257;
    case C::Group: return 258;
    case C::Guild: return 259;
    case C::OOC: return 260;
    case C::Auction: return 261;
    case C::Shout: return 262;
    case C::Emote: return 263;
    case C::Spell: return 264;
    case C::Combat: return 265;
    case C::Raid: return 327;
    case C::Error: return 13;
    default: return 273;
    }
}

int tell_window_of(const std::string& peer) {
    for (int w = 1; w < kMaxChatWindows; ++w)
        if (used(w) && wnd(w).tell && iequal(wnd(w).tell_target, peer)) return w;
    return -1;
}

bool is_tell_line(const ui::ChatLine& l) {
    return !l.peer.empty() && game::chat_channel_of_color(static_cast<std::int32_t>(color_of(l))) == 1;
}

// A tell from another player: not our own tell's echo, "You told %1, '%2'" in colour 308 (0x134;
// GameConnection::handle_channel_message).
bool is_incoming_tell(const ui::ChatLine& l) { return is_tell_line(l) && color_of(l) != 308; }

// The window a line went to (its serial, sync_lines); the lines from before the layout, the main window. -1: its
// window was closed.
int window_of_line(const ui::ChatLine& l) {
    if (!l.window) return 0;
    for (int w = 0; w < kMaxChatWindows; ++w)
        if (used(w) && wnd(w).serial == l.window) return w;
    return -1;
}

// The sequence number (1-based, ChatView::revision's count) of line i of the buffer.
std::uint64_t line_seq(const ui::ChatView& v, std::size_t i) { return v.lines[i].seq; }

// Hailstone: each window's oldest lines past kWindowLines leave the buffer (lines still to be routed stay).
void trim_windows(ui::ChatView& v) {
    std::unordered_map<std::uint32_t, std::size_t> count;
    for (const auto& l : v.lines)
        if (l.window) ++count[l.window];
    std::unordered_map<std::uint32_t, std::size_t> drop;
    for (const auto& [serial, n] : count)
        if (n > kWindowLines) drop[serial] = n - kWindowLines;
    if (drop.empty()) return;
    std::size_t out = 0;
    for (std::size_t i = 0; i < v.lines.size(); ++i) {
        auto d = v.lines[i].window ? drop.find(v.lines[i].window) : drop.end();
        if (d != drop.end() && d->second > 0) {
            --d->second;
            continue;
        }
        if (out != i) v.lines[out] = std::move(v.lines[i]);
        ++out;
    }
    v.lines.resize(out);
}

// CChatWindow::AddOutputText: a line in this window alone.
void window_text(int w, const std::string& text, std::uint32_t color) {
    UiHost* h = chat_host();
    if (!h || !h->chat || !used(w)) return;
    h->chat->add(ui::ChatChannel::System, text, 0, color);
    h->chat->lines.back().window = wnd(w).serial;
}

// CEverQuest::dsp_chat: a routed line.
void dsp_chat(const std::string& text, std::uint32_t color) {
    UiHost* h = chat_host();
    if (h && h->chat) h->chat->add(ui::ChatChannel::System, text, 0, color);
}

int conn_language() {
    UiHost* h = chat_host();
    return h && h->conn ? static_cast<int>(h->conn->chat_language()) : 0;
}

// CChatWindowManager_CreateNewChatWindow @0x6546f0: the first free slot, else a new one up to 32 (else eqstr 6648 in
// the main window, colour 13, unless `quiet`); "Chat <n>", the manager's default channel and the language in use.
// -1 when full.
int create_window(bool quiet = false) {
    ChatManager& m = manager();
    int w = -1;
    for (int i = 1; i < m.num_windows; ++i)
        if (!used(i)) {
            w = i;
            break;
        }
    if (w < 0) {
        if (m.num_windows >= kMaxChatWindows) {
            if (!quiet) window_text(0, eqstr(game::kStrNoMoreChatWindows), 13);
            return -1;
        }
        w = m.num_windows++;
    }
    ChatWnd& c = wnd(w);
    c = {};
    c.used = true;
    c.serial = m.next_serial++;
    c.name = std::format("{} {}", eqstr(game::kStrChat), w);
    c.channel = m.default_channel;
    c.language = conn_language();
    return w;
}

// The window whose tabs `w` is in.
int host_of(int w) { return used(w) && wnd(w).dock >= 0 && used(wnd(w).dock) ? wnd(w).dock : w; }

// A window's tabs: itself, then the windows docked in it.
std::vector<int> tabs_of(int host) {
    std::vector<int> out{host};
    for (int w = 0; w < kMaxChatWindows; ++w)
        if (w != host && used(w) && wnd(w).dock == host) out.push_back(w);
    return out;
}

// The tab that is up in a window.
int up_of(int host) {
    if (!used(host)) return -1;
    int up = wnd(host).up;
    return used(up) && (up == host || wnd(up).dock == host) ? up : host;
}

// CChatWindow::Deactivate @0x655780 (not the main window): its channels back to the main window (ClearChannelMaps),
// the main window active, the slot freed (FreeChatWindow: Always Chat Here dropped). Its tabs go with it.
void free_window(int w) {
    ChatManager& m = manager();
    if (w <= 0 || !used(w)) return;
    for (int t : tabs_of(w))
        if (t != w) free_window(t);
    for (int& c : m.channel_map)
        if (c == w) c = 0;
    m.active = 0;
    if (m.locked == w) m.locked = 0; // (the original: -1; Hailstone's default lock, set_locked)
    int host = wnd(w).dock;
    wnd(w) = {};
    if (used(host) && wnd(host).up == w) {
        wnd(host).up = -1;
        wnd(host).select = true;
    }
    changed();
}

// GetActiveChatWindow @0x650460: the locked one, else the current one.
int active_window() {
    ChatManager& m = manager();
    int w = m.locked >= 0 ? m.locked : m.active;
    return used(w) ? w : 0;
}

// SetActiveChatWindow (OnSetFocus, the input's focus).
void set_active(int w) {
    if (used(w)) manager().active = w;
}

// Always Chat Here (menu id 0x2b) ticked: the window is the locked one, GetActiveChatWindow's first choice, so Enter
// and the chat keys start a line there whichever window was clicked last (a click into another window's input still
// types there). Toggling says eqstr 15011 / 15010 in the window (colour 10).
// A Hailstone difference: the original has no lock until one is ticked (LockedActiveWindow -1), and unticking drops
// it. Here the main window is the locked one by default (a character with no LockedActiveWindow key, a freed locked
// window, another window unticked), so Enter goes to the main window until another is picked; unticking the main
// window itself drops the lock, which leaves the original's last-clicked window.
void set_locked(int w, bool on) {
    ChatManager& m = manager();
    if (!used(w) || (m.locked == w) == on) return;
    m.locked = on ? w : (w == 0 ? -1 : 0);
    window_text(w, eqstr(on ? game::kStrChatHereOn : game::kStrChatHereOff), 10);
    changed();
}

// The player's tell window, made when missing as a tab of the first one (the tab that is up stays; this one shows it
// has lines). -1 when no window is free.
int tell_window(const std::string& peer) {
    int w = tell_window_of(peer);
    if (w >= 0 || (w = create_window(true)) < 0) return w;
    int first = -1;
    for (int t = 1; t < kMaxChatWindows && first < 0; ++t)
        if (t != w && used(t) && wnd(t).tell && wnd(t).dock < 0) first = t;
    ChatWnd& c = wnd(w);
    c.tell = true;
    c.name = peer;
    c.channel = 7;
    c.tell_target = peer;
    c.dock = first;
    changed();
    return w;
}

// The lines that came since the last look go to their windows (CChatWindowManager::AddText: ChannelMap
// [GetChannelFromColor(colour)]); with tell windows on, a tell goes to its player's tell window instead (CEverQuest::
// DoTellWindow), made when missing as a tab of the first one. A line keeps its window.
void sync_lines(ui::ChatView& v) {
    ChatManager& m = manager();
    if (v.revision == m.synced) return;
    std::uint64_t from = m.synced;
    m.synced = v.revision;
    for (std::size_t i = 0; i < v.lines.size(); ++i) {
        auto& l = v.lines[i];
        if (line_seq(v, i) <= from || l.window) continue; // (restored lines came with their window)
        if (tell_windows_on() && is_tell_line(l)) {
            int w = tell_window(l.peer);
            if (w >= 0) {
                l.window = wnd(w).serial;
                if (is_incoming_tell(l)) ++wnd(w).tells_in;
                continue;
            }
        }
        int w = m.channel_map[static_cast<std::size_t>(game::chat_channel_of_color(static_cast<std::int32_t>(color_of(l))))];
        l.window = wnd(used(w) ? w : 0).serial;
    }
    trim_windows(v);
}

// slot (1-based) -> window, or -1.
int window_of(lua_State* L, int arg) {
    lua_Integer slot = luaL_checkinteger(L, arg);
    if (slot < 1 || slot > kMaxChatWindows || !used(static_cast<int>(slot - 1))) return -1;
    return static_cast<int>(slot - 1);
}

// The tab that is up in the window of a slot (its own window for a slot that is not a tab), or -1.
int up_window(lua_State* L, int arg) {
    int w = window_of(L, arg);
    return w < 0 ? -1 : up_of(host_of(w));
}

bool unread(const ui::ChatView& v, int w) {
    for (std::size_t i = v.lines.size(); i-- > 0;) {
        std::uint64_t seq = line_seq(v, i);
        if (seq <= wnd(w).seen || seq <= wnd(w).cleared) return false;
        if (window_of_line(v.lines[i]) == w) return true;
    }
    return false;
}

// Each window's first and last line, count and generation (ChatWnd::gen) as hs.ChatText shows them, worked out again
// when lines came or went or the windows changed: one pass over the buffer for every window, where hs.ChatText used
// to send every window all its lines again whenever a line came to any of them. Lines leave a window only from the
// front (trim_windows, ChatView::max_lines, a cleared buffer, hs.ChatClear's mark), so a window whose last line is
// the same but whose first is not lost lines without getting any: a new generation, as for a change of the windows
// (ChatManager::serial: a window freed or made, cleared, the history restored) or of a user colour.
// `w`: a window that must have a generation (one made since the last pass has none).
void scan_windows(const ui::ChatView& v, int w_asked) {
    ChatManager& m = manager();
    const auto* h = state().host;
    const auto catalog = h && h->conn ? h->conn->achievements().catalog_token() : 0;
    bool fresh = m.scanned != &v || m.scanned_serial != m.serial || m.scanned_colors != state().colors ||
        m.scanned_achievement_catalog != catalog;
    if (!fresh && m.scanned_revision == v.revision && wnd(w_asked).gen) return;
    std::array<std::uint64_t, kMaxChatWindows> first{}, last{};
    std::array<std::size_t, kMaxChatWindows> count{};
    std::unordered_map<std::uint32_t, int> by_serial;
    for (int w = 0; w < kMaxChatWindows; ++w)
        if (used(w)) by_serial[wnd(w).serial] = w;
    for (const auto& l : v.lines) {
        int w = 0; // (window_of_line)
        if (l.window) {
            auto it = by_serial.find(l.window);
            if (it == by_serial.end()) continue;
            w = it->second;
        }
        auto i = static_cast<std::size_t>(w);
        if (l.seq <= wnd(w).cleared) continue;
        if (!count[i]) first[i] = l.seq;
        last[i] = l.seq;
        ++count[i];
    }
    for (int w = 0; w < kMaxChatWindows; ++w) {
        ChatWnd& c = wnd(w);
        if (!c.used) continue;
        auto i = static_cast<std::size_t>(w);
        if (fresh || !c.gen || last[i] < c.last || (last[i] == c.last && (first[i] != c.first || count[i] != c.count)))
            c.gen = m.next_gen++;
        c.first = first[i];
        c.last = last[i];
        c.count = count[i];
    }
    m.scanned = &v;
    m.scanned_revision = v.revision;
    m.scanned_serial = m.serial;
    m.scanned_colors = state().colors;
    m.scanned_achievement_catalog = catalog; // existing links revalidate after catalog replacement/retirement
}

// ---------------------------------------------------------------- the menus (CChatWindowManager::InitContextMenu)

using Row = UiHost::MenuRow;

Row separator() {
    Row r;
    r.separator = true;
    return r;
}

Row disabled(Row r) {
    r.enabled = false;
    r.rgb = UiHost::kMenuDisabledRgb;
    return r;
}

// A menu row acts on window `w` only while that window is still the one it was opened for.
std::function<void()> on_window(int w, std::function<void(int)> fn) {
    std::uint32_t serial = wnd(w).serial;
    return [w, serial, fn = std::move(fn)] {
        if (used(w) && wnd(w).serial == serial) fn(w);
    };
}

std::vector<std::string> joined_channels() {
    UiHost* h = chat_host();
    std::vector<std::string> out = h && h->conn ? h->conn->chat_channels() : std::vector<std::string>{};
    if (out.size() > 10) out.resize(10);
    return out;
}

std::string filter_label(std::uint32_t id, std::string_view fallback) {
    UiHost* h = chat_host();
    if (id && h && h->conn && h->conn->strings().has(id)) return eqstr(id);
    return fallback.empty() ? eqstr(id) : std::string(fallback);
}

std::string filter_group_text(game::ChatFilterMenu menu) {
    if (menu == game::ChatFilterMenu::Filters) return filter_label(game::kStrFilters, "Filters");
    for (const auto& group : game::chat_filter_groups())
        if (group.menu == menu) return filter_label(group.eqstr, group.label);
    return {};
}

game::ChatFilterMenu filter_group_parent(game::ChatFilterMenu menu) {
    for (const auto& group : game::chat_filter_groups())
        if (group.menu == menu) return group.parent;
    return game::ChatFilterMenu::Filters;
}

// An item's localized or native label, or the joined chat channel's name.
std::string filter_text(const game::ChatFilterItem& it, const std::vector<std::string>& joined) {
    if (it.eqstr || !it.label.empty()) return filter_label(it.eqstr, it.label);
    std::size_t k = static_cast<std::size_t>(it.channel - game::kChatChannelFirstServer);
    return k < joined.size() ? joined[k] : eqstr(game::kStrFilterNumbered, {std::to_string(it.channel)});
}

// WndNotification ids 100..0x9c: the channel moves to the window, or back to the main window when it is there;
// "<item> will go to this window." / "... the main window." in the window.
void toggle_channel(int w, int channel, const std::string& text) {
    ChatManager& m = manager();
    int& c = m.channel_map[static_cast<std::size_t>(channel)];
    bool here = c == w;
    c = here ? 0 : w;
    window_text(w, text + " " + eqstr(here ? game::kStrFilterMain : game::kStrFilterHere), 10);
    changed();
}

// ALL (0x9d..0xa0): every channel of the menu to the window.
void all_channels(int w, game::ChatFilterMenu menu) {
    ChatManager& m = manager();
    std::size_t joined = joined_channels().size();
    for (const auto& it : game::chat_filter_items()) {
        if (it.menu != menu) continue;
        if (menu == game::ChatFilterMenu::ChatChannel &&
            static_cast<std::size_t>(it.channel - game::kChatChannelFirstServer) >= joined)
            continue;
        m.channel_map[it.channel] = w;
    }
    std::uint32_t message = game::chat_filter_all_eqstr(menu);
    window_text(w, message ? eqstr(message) :
        std::format("{} channels will now filter to this window.", filter_group_text(menu)), 10);
    changed();
}

std::vector<Row> filter_menu(int w, game::ChatFilterMenu menu) {
    std::vector<Row> rows;
    auto joined = joined_channels();
    const ChatManager& m = manager();
    auto add_items = [&](auto pred) {
        for (const auto& it : game::chat_filter_items()) {
            if (it.menu != menu || !pred(it)) continue;
            if (menu == game::ChatFilterMenu::ChatChannel &&
                static_cast<std::size_t>(it.channel - game::kChatChannelFirstServer) >= joined.size())
                continue;
            Row r;
            r.text = filter_text(it, joined);
            r.check = m.channel_map[it.channel] == w;
            int channel = it.channel;
            std::string text = r.text;
            r.act = on_window(w, [channel, text](int win) { toggle_channel(win, channel, text); });
            rows.push_back(std::move(r));
        }
    };
    auto submenu = [&](game::ChatFilterMenu sub, bool enabled) {
        Row r;
        r.text = filter_group_text(sub);
        if (!enabled) return disabled(std::move(r));
        r.submenu = [w, sub] { return used(w) ? filter_menu(w, sub) : std::vector<Row>{}; };
        return r;
    };
    auto add_submenus = [&](game::ChatFilterMenu parent) {
        for (const auto& group : game::chat_filter_groups())
            if (group.parent == parent && group.menu != game::ChatFilterMenu::Extended)
                rows.push_back(submenu(group.menu, group.menu != game::ChatFilterMenu::ChatChannel || !joined.empty()));
    };
    if (menu == game::ChatFilterMenu::Filters) {
        add_items([](const game::ChatFilterItem& it) { return it.channel <= 8 || it.channel == 49; });
        add_submenus(menu); // Chat Channel, Melee, Spells
        add_items([](const game::ChatFilterItem& it) { return it.channel > 8 && it.channel != 49; });
    } else {
        add_submenus(menu); // (Hailstone's extended submenus)
        add_items([](const game::ChatFilterItem&) { return true; });
    }
    Row all;
    all.text = eqstr(game::kStrAll);
    all.act = on_window(w, [menu](int win) { all_channels(win, menu); });
    rows.push_back(std::move(all));
    // Hailstone: the extended channels' submenu after the original menu (its ALL included), so the rows above are
    // InitContextMenu's.
    if (menu == game::ChatFilterMenu::Filters) rows.push_back(submenu(game::ChatFilterMenu::Extended, true));
    return rows;
}

// Ids 200..0xd9: the window's channel; "<item> is now the default channel for this window."
void set_default(int w, int channel, int index, const std::string& text) {
    ChatWnd& c = wnd(w);
    c.channel = channel;
    if (channel == 0x14) {
        c.chat_index = index;
        auto joined = joined_channels();
        c.tell_target = index >= 0 && static_cast<std::size_t>(index) < joined.size() ? joined[static_cast<std::size_t>(index)] : "";
    }
    if (channel == 7) c.tell_target = "a"; // as the original sets it (the item is disabled)
    window_text(w, text + eqstr(game::kStrDefaultChannelSet), 10);
    changed();
}

std::vector<Row> channel_menu(int w) {
    std::vector<Row> rows;
    const ChatWnd& c = wnd(w);
    bool checked = false;
    for (const auto& it : game::chat_default_items()) {
        Row r;
        r.text = eqstr(it.eqstr);
        // CheckKind: the item of the window's channel (the first match)
        r.check = !checked && c.channel == it.channel;
        checked = checked || r.check;
        int channel = it.channel;
        std::string text = r.text;
        r.act = on_window(w, [channel, text](int win) { set_default(win, channel, 0, text); });
        rows.push_back(it.enabled ? std::move(r) : disabled(std::move(r)));
    }
    auto joined = joined_channels();
    Row sub;
    sub.text = eqstr(game::kStrFilterChatChannel);
    if (joined.empty()) {
        rows.push_back(disabled(std::move(sub)));
        return rows;
    }
    sub.submenu = [w] {
        std::vector<Row> out;
        if (!used(w)) return out;
        auto names = joined_channels();
        for (std::size_t k = 0; k < names.size(); ++k) {
            Row r;
            r.text = names[k];
            r.check = wnd(w).channel == 0x14 && wnd(w).chat_index == static_cast<int>(k);
            std::string text = r.text;
            int index = static_cast<int>(k);
            r.act = on_window(w, [index, text](int win) { set_default(win, 0x14, index, text); });
            out.push_back(std::move(r));
        }
        return out;
    };
    rows.push_back(std::move(sub));
    return rows;
}

// CChatWindow_SetLanguage(language, save, announce) from the menu: eqstr 3133 in the window.
void set_language(int w, int language) {
    wnd(w).language = language;
    window_text(w, eqstr(3133, {eqstr(game::language_eqstr(static_cast<std::uint32_t>(language)))}), 10);
    changed();
}

std::vector<Row> language_menu(int w) {
    std::vector<Row> rows;
    UiHost* h = chat_host();
    if (!h || !h->conn || !h->conn->profile()) return rows;
    const auto& langs = h->conn->profile()->languages;
    for (std::size_t i = 0; i < langs.size() && i < 32; ++i) {
        if (langs[i] == 0) continue;
        Row r;
        r.text = eqstr(game::language_eqstr(static_cast<std::uint32_t>(i)));
        r.check = wnd(w).language == static_cast<int>(i);
        int language = static_cast<int>(i);
        r.act = on_window(w, [language](int win) { set_language(win, language); });
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<Row> hit_modes_menu(int w) {
    std::vector<Row> rows;
    // ALL stays last in the menu while retaining its public API index (7).
    for (int cat : {0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 6}) {
        Row r;
        r.text = hit_category_name(cat);
        r.submenu = [w, cat] {
            std::vector<Row> out;
            for (int mode = 0; mode < 3; ++mode) {
                Row m;
                m.text = eqstr(game::hit_mode_eqstr(cat, mode));
                m.check = hit_mode_value(cat) == mode;
                if (cat != 6) {
                    // The original categories use ids 0x190 + 10 cat + mode; spell categories use Hailstone's labels.
                    m.act = [cat, mode] {
                        set_hit_mode(cat, mode);
                        dsp_chat(std::format("{} {}", hit_category_name(cat),
                                             eqstr(game::hit_mode_eqstr(cat, mode))), 0x111);
                    };
                } else {
                    // 0x1cc..0x1ce: all eight native modes, plus Hailstone's spell modes.
                    m.act = on_window(w, [mode](int win) {
                        set_hit_mode(6, mode);
                        window_text(win, eqstr(game::hit_mode_all_eqstr(mode)), 10);
                    });
                }
                out.push_back(std::move(m));
            }
            return out;
        };
        rows.push_back(std::move(r));
    }
    return rows;
}

// The tell menus (UpdateTellMenus @0x6509e0): the names put "/tell <name> " into the active window's input.
Row tell_names_row(std::uint32_t text, std::vector<std::string> names) {
    Row r;
    r.text = eqstr(text);
    if (names.empty()) return disabled(std::move(r));
    r.submenu = [names] {
        std::vector<Row> out;
        for (const auto& n : names) {
            Row x;
            x.text = n;
            x.act = [n] {
                std::string word = eqstr(13974); // /tell
                if (!word.empty() && word[0] == '/') word.erase(0, 1);
                ChatState& s = state();
                s.take_focus = game::tell_menu_text(n, word);
                s.replace_focus = true;
                s.focus_window = -1;
                set_active(active_window());
            };
            out.push_back(std::move(x));
        }
        return out;
    };
    return r;
}

// Hailstone: a name link clicked (the modern chat). With tell windows on, the player's tell window comes up with the
// keys (made when missing); else the active window's input holds "/tell <name> ", as a Reply to... menu's name.
void open_tell(const std::string& name) {
    ChatState& s = state();
    s.replace_focus = true;
    s.focus_window = -1;
    if (tell_windows_on()) {
        if (int w = tell_window(name); w >= 0) {
            int host = host_of(w);
            if (up_of(host) != w) {
                wnd(host).up = w;
                changed();
            }
            s.take_focus = std::string();
            s.focus_window = w;
            return;
        }
    }
    std::string word = eqstr(13974); // /tell
    if (!word.empty() && word[0] == '/') word.erase(0, 1);
    s.take_focus = game::tell_menu_text(name, word);
}

std::vector<Row> chat_menu(int w) {
    std::vector<Row> rows;
    UiHost* h = chat_host();
    ChatManager& m = manager();
    int host = host_of(w);
    {
        Row r; // Window: the window's own menu (CContextMenuManager's default menu; sidl.lua's window menu here)
        r.text = eqstr(game::kStrWindow);
        std::string screen = std::format("Chat{}", host + 1);
        r.act = [screen] {
            if (UiHost* hh = chat_host()) hh->window_menu = screen;
        };
        rows.push_back(std::move(r));
    }
    {
        Row r; // 0x2d: the output's scrollbar
        r.text = eqstr(game::kStrScrollbar);
        r.check = wnd(w).scrollbar;
        r.act = on_window(w, [](int win) {
            wnd(win).scrollbar = !wnd(win).scrollbar;
            changed();
        });
        rows.push_back(std::move(r));
    }
    rows.push_back(separator());
    {
        Row r; // 0x2a
        r.text = eqstr(game::kStrNewChatWindow);
        r.act = [] {
            if (create_window() >= 0) changed();
        };
        rows.push_back(std::move(r));
    }
    {
        Row r; // 0x2c: CTextEntryWnd::Activate(this, 0, eqstr 3141, 1, 15, the name); OK renames it
        r.text = eqstr(game::kStrRenameWindow);
        r.act = on_window(w, [](int win) {
            UiHost* hh = chat_host();
            if (!hh) return;
            UiHost::TextEntry e;
            e.prompt = eqstr(game::kStrRenamePrompt);
            e.max_chars = 15;
            e.text = wnd(win).name;
            std::uint32_t serial = wnd(win).serial;
            e.done = [win, serial](const std::string& text) {
                if (!used(win) || wnd(win).serial != serial || text.empty()) return;
                wnd(win).name = text;
                changed();
            };
            hh->text_entry = std::move(e);
        });
        rows.push_back(std::move(r));
    }
    {
        Row r; // 0x2b: Always Chat Here (checked while it is the locked window)
        r.text = eqstr(game::kStrAlwaysChatHere);
        r.check = m.locked == w;
        r.act = on_window(w, [](int win) { set_locked(win, manager().locked != win); });
        rows.push_back(std::move(r));
    }
    rows.push_back(separator());
    if (h && h->conn) {
        rows.push_back(tell_names_row(game::kStrReplyTo, h->conn->social().last_tells));
        rows.push_back(tell_names_row(game::kStrTellFriend, h->conn->social().friends));
        std::vector<std::string> raid;
        for (const auto& mem : h->conn->raid().members)
            if (!mem.name.empty() && raid.size() < 0x48) raid.push_back(mem.name);
        rows.push_back(tell_names_row(game::kStrTellRaidMember, std::move(raid)));
    }
    rows.push_back(separator());
    auto sub = [&](std::uint32_t text, std::function<std::vector<Row>(int)> fn) {
        Row r;
        r.text = eqstr(text);
        r.submenu = [w, fn = std::move(fn)] { return used(w) ? fn(w) : std::vector<Row>{}; };
        rows.push_back(std::move(r));
    };
    sub(game::kStrFilters, [](int win) { return filter_menu(win, game::ChatFilterMenu::Filters); });
    sub(game::kStrChannel, channel_menu);
    sub(game::kStrLanguage, language_menu);
    sub(game::kStrHitModes, hit_modes_menu);
    if (!wnd(host).tell) {
        // this client's tabs
        rows.push_back(separator());
        Row r;
        r.text = "New Tab";
        r.act = [host] {
            if (!used(host)) return;
            int t = create_window();
            if (t < 0) return;
            wnd(t).dock = host;
            wnd(host).up = t;
            wnd(host).select = true;
            changed();
        };
        rows.push_back(std::move(r));
        if (w != host) {
            Row c;
            c.text = "Close Tab";
            c.act = on_window(w, [](int win) { free_window(win); });
            rows.push_back(std::move(c));
        }
    }
    return rows;
}

// ---------------------------------------------------------------- the hs functions

// windows = hs.ChatWindows()   -- per chat window shown (not the tabs of another) { slot, title (the tab's that is
// up), main, tells, tab (the one up, 1-based), select (bring that tab up), tellsIn (Hailstone: how many tells from
// others its tabs have had; it grows with each), tabs = { { title, unread, tell } } }
int ChatWindows(lua_State* L) {
    UiHost& h = host(L);
    lua_newtable(L);
    if (!h.chat || !h.in_game) return 1;
    sync_lines(*h.chat);
    lua_Integer n = 0;
    for (int w = 0; w < kMaxChatWindows; ++w) {
        if (!used(w) || host_of(w) != w) continue;
        auto tabs = tabs_of(w);
        int up = up_of(w);
        ChatWnd& c = wnd(w);
        lua_createtable(L, 0, 7);
        lua_pushinteger(L, w + 1);
        lua_setfield(L, -2, "slot");
        lua_pushstring(L, wnd(up).name.c_str());
        lua_setfield(L, -2, "title");
        lua_pushboolean(L, w == 0);
        lua_setfield(L, -2, "main");
        lua_pushboolean(L, c.tell);
        lua_setfield(L, -2, "tells");
        int at = static_cast<int>(std::find(tabs.begin(), tabs.end(), up) - tabs.begin());
        lua_pushinteger(L, at + 1);
        lua_setfield(L, -2, "tab");
        lua_pushboolean(L, c.select);
        lua_setfield(L, -2, "select");
        c.select = false;
        std::uint64_t tells_in = 0;
        for (int tw : tabs) tells_in += wnd(tw).tells_in;
        lua_pushinteger(L, static_cast<lua_Integer>(tells_in));
        lua_setfield(L, -2, "tellsIn");
        lua_createtable(L, static_cast<int>(tabs.size()), 0);
        for (std::size_t t = 0; t < tabs.size(); ++t) {
            int tw = tabs[t];
            lua_createtable(L, 0, 3);
            lua_pushstring(L, wnd(tw).name.c_str());
            lua_setfield(L, -2, "title");
            lua_pushboolean(L, tw != up && unread(*h.chat, tw));
            lua_setfield(L, -2, "unread");
            lua_pushboolean(L, wnd(tw).tell);
            lua_setfield(L, -2, "tell");
            lua_rawseti(L, -2, static_cast<lua_Integer>(t + 1));
        }
        lua_setfield(L, -2, "tabs");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// version, lines = hs.ChatText(slot [, version [, names [, stamps]]])   -- the lines of the window's tab that is up, as
// STML, oldest first; only the version when the script already has it. names (Hailstone): players' names as name
// links; stamps (Hailstone): each line's local time first, "[hh:mm:ss] " (another version than without). The version
// changes only when this window's lines do (a line in another window leaves it as it is).
// version, lines, reset, count = hs.ChatText(slot, version, names, stamps, true)   -- Hailstone: the lines added since
// `version` (one this call gave for the same slot, names and stamps) when the script can keep what it has: reset
// false, add `lines` at the end and drop lines from the front until `count` are left. Else reset true and `lines`
// are all of them, as without the last argument.
int ChatText(lua_State* L) {
    UiHost& h = host(L);
    int w = up_window(L, 1);
    if (!h.chat || w < 0) {
        lua_pushnil(L);
        return 1;
    }
    sync_lines(*h.chat);
    scan_windows(*h.chat, w);
    wnd(w).seen = h.chat->revision;
    const ChatWnd& c = wnd(w);
    bool names = lua_toboolean(L, 3) != 0;
    bool stamps = lua_toboolean(L, 4) != 0;
    bool since = lua_toboolean(L, 5) != 0;
    // bits 0..39 the last line's seq, 40..60 the generation (unique to the window), 61 names, 62 stamps. The
    // stamped text is another version of the window's; marked before the comparison, or a stamped window never
    // matches the version it holds and gets its whole history again every frame.
    constexpr std::uint64_t kSeqMask = (std::uint64_t{1} << 40) - 1, kGenMask = (std::uint64_t{1} << 21) - 1;
    auto version = static_cast<lua_Integer>((c.last & kSeqMask) | ((c.gen & kGenMask) << 40) |
                                            (std::uint64_t{names} << 61) | (std::uint64_t{stamps} << 62));
    int given = 0;
    lua_Integer held = lua_tointegerx(L, 2, &given);
    bool have = given && held == version; // before the result goes on the stack
    lua_pushinteger(L, version);
    if (have) return 1;
    // the lines after the script's (its version: the same generation and flags, a last line not past this one's)
    std::uint64_t after = 0;
    bool append = since && given && held >= 0 &&
                  (static_cast<std::uint64_t>(held) >> 40) == (static_cast<std::uint64_t>(version) >> 40) &&
                  (static_cast<std::uint64_t>(held) & kSeqMask) <= (c.last & kSeqMask);
    if (append) after = (static_cast<std::uint64_t>(held) & kSeqMask) | (c.last & ~kSeqMask);
    std::size_t from = 0;
    if (append) {
        from = h.chat->lines.size();
        while (from > 0 && h.chat->lines[from - 1].seq > after) --from;
    }
    lua_newtable(L);
    lua_Integer i = 0;
    for (std::size_t k = from; k < h.chat->lines.size(); ++k) {
        const auto& l = h.chat->lines[k];
        if (line_seq(*h.chat, k) <= c.cleared || window_of_line(l) != w) continue;
        plugin::push_made(L, line_stml(l, names, stamps)); // (out of this frame before the push: plugin/lua_keep.h)
        lua_rawseti(L, -2, ++i);
    }
    if (!since) return 2;
    lua_pushboolean(L, !append);
    lua_pushinteger(L, static_cast<lua_Integer>(c.count));
    return 4;
}

// The command plain typed text becomes in a window (InterpretCmd's default channel switch @0x520052 on the
// window's ChatChannel, which WndNotification makes the default while it runs the line).
std::string plain_text_command(const ChatWnd& c, const std::string& text) {
    switch (c.channel) {
    case 0: return "/guildsay " + text;
    case 2: return "/gsay " + text;
    case 4: return "/auction " + text;
    case 5: return "/ooc " + text;
    case 7: return "/tell " + c.tell_target + " " + text;
    case 0xf: return "/rsay " + text;
    case 0x15: return "/fsay " + text;
    case 0x14: return "/chat #" + c.tell_target + " " + text; // ChatServer_SendCommandText("#<channel> text")
    default: return text; // say (shout too)
    }
}

// hs.ChatSubmit(slot, text)   -- a typed line: kept in the history and interpreted (say, /command, ...) with the
// window's channel and language as the defaults
void submit_line(UiHost& h, int w, std::string text) {
    ChatState& s = state();
    s.history.push_back(text);
    while (s.history.size() > 40) s.history.pop_front();
    s.history_at = -1;
    if (w < 0) {
        if (h.submit_chat) h.submit_chat(text);
        return;
    }
    set_active(w);
    // (':' and '\'' are InterpretCmd's emote and say commands, CMDMODE_EMOTE's and CMDMODE_SAY's: not the channel's)
    bool plain = !text.empty() && text[0] != '/' && text[0] != ';' && text[0] != '[' && text[0] != '#' &&
                 text[0] != ':' && text[0] != '\'';
    // '#' lines go to the default channel too (InterpretCmd: not a command)
    if (!text.empty() && (plain || text[0] == '#')) text = plain_text_command(wnd(w), text);
    std::uint32_t saved = 0;
    if (h.conn) {
        saved = h.conn->chat_language();
        h.conn->use_chat_language(static_cast<std::uint32_t>(wnd(w).language));
    }
    std::uint32_t serial = wnd(w).serial;
    if (h.submit_chat) h.submit_chat(text);
    if (h.conn) {
        // /language typed here sets this window's language (CChatWindow_SetLanguage on the active window)
        std::uint32_t now = h.conn->chat_language();
        if (used(w) && wnd(w).serial == serial && now != static_cast<std::uint32_t>(wnd(w).language)) {
            wnd(w).language = static_cast<int>(now);
            changed();
        }
        h.conn->use_chat_language(saved);
    }
}

int ChatSubmit(lua_State* L) {
    submit_line(host(L), up_window(L, 1), luaL_checkstring(L, 2));
    return 0;
}

// The one-shot key and focus state below (the chat keys' text, a pending Link All, the page keys, the input's history
// step) is the pack's chat input's alone: a plugin's code in the UI that took it would swallow the player's key, or
// send the loot window's links from its own call. It gets nil and changes nothing.
bool plugin_code() { return !ui::lua::LuaUi::caller_tag().empty(); }

// text, replace = hs.ChatTakeFocus(slot [, typed])   -- when the keyboard goes to the chat input (the chat keys: Enter, ';',
// '/', '['; game/chat_input.h; a tell menu's name): what goes into the active window's input (replace: instead of
// what is typed); nil otherwise and for other windows
//
// The loot window's Link All (request_chat_link_all) goes to the active window too: with `typed` (what its input
// holds, links as their tags) the lines that fill up are sent from here and the text returned replaces the input;
// without it the links are added after what is typed (game::link_all_into_chat).
int ChatTakeFocus(lua_State* L) {
    ChatState& s = state();
    int w = window_of(L, 1);
    if (plugin_code()) return 0;
    int to = used(s.focus_window) ? s.focus_window : active_window();
    if (s.link_all && w >= 0 && host_of(active_window()) == w) {
        bool have_typed = lua_type(L, 2) == LUA_TSTRING;
        std::string typed = have_typed ? lua_tostring(L, 2) : std::string();
        game::LinkAllChat r = game::link_all_into_chat(typed, *s.link_all);
        s.link_all.reset();
        for (const std::string& line : r.sent) submit_line(host(L), up_of(w), line);
        if (!have_typed) r.text.erase(0, typed.size());
        lua_pushlstring(L, r.text.data(), r.text.size());
        lua_pushboolean(L, have_typed);
        return 2;
    }
    if (!s.take_focus || w < 0 || host_of(to) != w) return 0;
    lua_pushlstring(L, s.take_focus->data(), s.take_focus->size());
    lua_pushboolean(L, s.replace_focus);
    s.take_focus.reset();
    s.replace_focus = false;
    s.focus_window = -1;
    return 2;
}

// One step through the lines sent before (older: one further back): the line, "" past the newest, nullopt without
// any.
std::optional<std::string> step_history(bool older) {
    ChatState& s = state();
    if (s.history.empty()) return std::nullopt;
    int n = static_cast<int>(s.history.size());
    if (older) s.history_at = s.history_at < 0 ? n - 1 : std::max(0, s.history_at - 1);
    else if (s.history_at >= 0) s.history_at = s.history_at + 1 < n ? s.history_at + 1 : -1;
    return s.history_at < 0 ? std::string() : s.history[static_cast<std::size_t>(s.history_at)];
}

// text = hs.ChatHistory(slot, older)   -- steps through the lines sent before (older: one further back)
int ChatHistory(lua_State* L) {
    if (plugin_code()) return 0;
    auto line = step_history(lua_toboolean(L, 2) != 0);
    if (!line) return 0;
    lua_pushlstring(L, line->data(), line->size());
    return 1;
}

// A slash command's word by its eqstr ("/tell", "/rtarget"), `fallback` without the game's strings.
std::string slash_word(std::uint32_t id, const char* fallback) {
    std::string w = eqstr(id);
    return !w.empty() && w[0] == '/' ? w : std::string(fallback);
}

// text = hs.ChatCycleReply(text, back)   -- CYCLEREPLY (Tab) / CYCLEREPLY_BACK (Shift+Tab) in a chat input holding
// `text` (game::cycle_reply): its new text, nil when it stays
int ChatCycleReply(lua_State* L) {
    UiHost& h = host(L);
    const char* text = luaL_checkstring(L, 1);
    const bool back = lua_toboolean(L, 2) != 0;
    if (!h.conn) return 0;
    // The reply in a box the state owns across its push (plugin/lua_keep.h).
    game::CycleReply& r = plugin::lua_keep<game::CycleReply>(L);
    plugin::cxx_guard(L, [&] {
        r = game::cycle_reply(text, h.conn->social().last_tells, h.conn->last_tell_index(), back,
                              slash_word(13974, "/tell"), slash_word(14202, "/rtarget"));
        for (std::uint32_t id : r.messages) h.conn->system_message(id);
    });
    if (r.text.empty()) return 0;
    lua_pushlstring(L, r.text.data(), r.text.size());
    return 1;
}

// dir = hs.ChatPage(slot)   -- the PAGEUP (-1) / PAGEDOWN (1) key for this window's output, once; nil otherwise
int ChatPage(lua_State* L) {
    ChatState& s = state();
    int w = window_of(L, 1);
    if (plugin_code()) return 0;
    if (s.page == 0 || w < 0 || host_of(s.page_window) != w) return 0;
    lua_pushinteger(L, s.page);
    s.page = 0;
    return 1;
}

// The name of a window's default channel (the Channel menu's item; a tell's "Tell <player>").
std::string default_name(const ChatWnd& c) {
    if (c.channel == 7) return eqstr(6631) + " " + c.tell_target;
    if (c.channel == 0x14) return c.tell_target;
    for (const auto& it : game::chat_default_items())
        if (it.channel == c.channel) return eqstr(it.eqstr);
    return eqstr(6630);
}

// font, active, default, scrollbar, here = hs.ChatInfo(slot)   -- the font of the tab that is up (CChatWindow::
// SetChatFont's index), whether it is the active window, its default channel's name, whether its output has a
// scrollbar, and whether it is the Always Chat Here window
int ChatInfo(lua_State* L) {
    int w = up_window(L, 1);
    if (w < 0) {
        lua_pushinteger(L, 3);
        lua_pushboolean(L, false);
        lua_pushstring(L, "Say");
        lua_pushboolean(L, true);
        lua_pushboolean(L, false);
        return 5;
    }
    lua_pushinteger(L, wnd(w).font);
    lua_pushboolean(L, active_window() == w);
    lua_pushstring(L, default_name(wnd(w)).c_str());
    lua_pushboolean(L, wnd(w).scrollbar);
    lua_pushboolean(L, manager().locked == w);
    return 5;
}

// hs.ChatSetAlwaysHere(slot, on)   -- the menu's Always Chat Here for the tab that is up (set_locked)
int ChatSetAlwaysHere(lua_State* L) {
    set_locked(up_window(L, 1), lua_toboolean(L, 2) != 0);
    return 0;
}

// hs.ChatActivate(slot)   -- the window's tab that is up becomes the active window (where Enter and the chat keys go)
int ChatActivate(lua_State* L) {
    set_active(up_window(L, 1));
    return 0;
}

// hs.ChatMenu(slot)   -- the chat window's context menu at the mouse (CChatWindow::HandleRButtonDown: the menus
// brought up to date for the tab that is up)
int ChatMenu(lua_State* L) {
    UiHost& h = host(L);
    int w = up_window(L, 1);
    if (w < 0) return 0;
    ImVec2 mp = ImGui::GetMousePos();
    h.open_menu(mp.x, mp.y, chat_menu(w));
    return 0;
}

// slot = hs.ChatNewWindow([title])   -- a new chat window (New Chat Window); nil when all 32 are in use
int ChatNewWindow(lua_State* L) {
    int w = create_window();
    if (w < 0) return 0;
    if (lua_isstring(L, 1)) wnd(w).name = lua_tostring(L, 1);
    changed();
    lua_pushinteger(L, w + 1);
    return 1;
}

// hs.ChatCloseWindow(slot)   -- closes a window (not the main one) with its tabs
int ChatCloseWindow(lua_State* L) {
    free_window(window_of(L, 1));
    return 0;
}

// tab = hs.ChatNewTab(slot [, title])   -- a new chat window as a tab of this one, brought up; nil when all 32 are in
// use
int ChatNewTab(lua_State* L) {
    int w = window_of(L, 1);
    if (w < 0) return 0;
    int host = host_of(w);
    if (wnd(host).tell) return 0;
    int t = create_window();
    if (t < 0) return 0;
    if (lua_isstring(L, 2)) wnd(t).name = lua_tostring(L, 2);
    wnd(t).dock = host;
    wnd(host).up = t;
    wnd(host).select = true;
    changed();
    auto tabs = tabs_of(host);
    lua_pushinteger(L, static_cast<lua_Integer>(std::find(tabs.begin(), tabs.end(), t) - tabs.begin()) + 1);
    return 1;
}

// hs.ChatCloseTab(slot, tab)   -- closes a tab's window (the window's own tab: the window with its tabs; the main
// window stays)
int ChatCloseTab(lua_State* L) {
    int w = window_of(L, 1);
    const lua_Integer tab = luaL_checkinteger(L, 2);
    const lua_Integer t = tab < 1 ? -1 : tab - 1; // (math.mininteger - 1 would overflow)
    if (w < 0) return 0;
    auto tabs = tabs_of(host_of(w));
    if (t < 0 || t >= static_cast<lua_Integer>(tabs.size())) return 0;
    free_window(tabs[static_cast<std::size_t>(t)]);
    return 0;
}

// hs.ChatSelectTab(slot, tab)   -- the tab that is up (a click on it)
int ChatSelectTab(lua_State* L) {
    int w = window_of(L, 1);
    const lua_Integer tab = luaL_checkinteger(L, 2);
    const lua_Integer t = tab < 1 ? -1 : tab - 1; // (math.mininteger - 1 would overflow)
    if (w < 0) return 0;
    int host = host_of(w);
    auto tabs = tabs_of(host);
    if (t < 0 || t >= static_cast<lua_Integer>(tabs.size())) return 0;
    int up = tabs[static_cast<std::size_t>(t)];
    if (up_of(host) == up) return 0;
    wnd(host).up = up;
    changed();
    return 0;
}

// hs.ChatSetTitle(slot, title)   -- renames the tab that is up (Rename Window)
int ChatSetTitle(lua_State* L) {
    int w = up_window(L, 1);
    std::string title = luaL_checkstring(L, 2);
    if (w < 0 || title.empty()) return 0;
    wnd(w).name = title.substr(0, 64);
    changed();
    return 0;
}

// filters = hs.ChatFilters(slot)   -- the Filters menu for the tab that is up: { id (the channel), group ("Filters",
// "Chat Channel", "Melee", "Spells"; eqstr names; Hailstone's extended submenus by their names), name, on (the
// channel goes to this window), menu and parent (the ChatFilterMenu of the item's menu and of the menu that holds
// it; hs.ChatFilterGroups) } in the menu's order
int ChatFilters(lua_State* L) {
    int w = up_window(L, 1);
    lua_newtable(L);
    if (w < 0) return 1;
    auto joined = joined_channels();
    lua_Integer n = 0;
    for (const auto& it : game::chat_filter_items()) {
        if (it.menu == game::ChatFilterMenu::ChatChannel &&
            static_cast<std::size_t>(it.channel - game::kChatChannelFirstServer) >= joined.size())
            continue;
        lua_createtable(L, 0, 6);
        lua_pushinteger(L, it.channel);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, filter_group_text(it.menu).c_str());
        lua_setfield(L, -2, "group");
        lua_pushinteger(L, static_cast<lua_Integer>(it.menu));
        lua_setfield(L, -2, "menu");
        lua_pushinteger(L, static_cast<lua_Integer>(filter_group_parent(it.menu)));
        lua_setfield(L, -2, "parent");
        lua_pushstring(L, filter_text(it, joined).c_str());
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, manager().channel_map[it.channel] == w);
        lua_setfield(L, -2, "on");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// groups = hs.ChatFilterGroups()   -- the Filters menu's submenus: { id (ChatFilterMenu), parent (0: the Filters
// menu), name }; Chat Channel, Melee and Spells, then Hailstone's Extended (server) and its submenus
int ChatFilterGroups(lua_State* L) {
    auto groups = game::chat_filter_groups();
    lua_createtable(L, static_cast<int>(groups.size()), 0);
    lua_Integer index = 0;
    for (const auto& group : groups) {
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, static_cast<lua_Integer>(group.menu));
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, static_cast<lua_Integer>(group.parent));
        lua_setfield(L, -2, "parent");
        lua_pushstring(L, filter_group_text(group.menu).c_str());
        lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, ++index);
    }
    return 1;
}

// hs.ChatSetFilter(slot, id, on)   -- the channel to the tab that is up (on) or back to the main window; id may be a
// group's name (hs.ChatFilters' group): its ALL
int ChatSetFilter(lua_State* L) {
    int w = up_window(L, 1);
    if (w < 0) return 0;
    bool on = lua_toboolean(L, 3) != 0;
    if (lua_type(L, 2) == LUA_TSTRING) {
        std::string group = lua_tostring(L, 2);
        auto apply_group = [&](game::ChatFilterMenu menu) {
            if (group != filter_group_text(menu)) return false;
            if (on) {
                all_channels(w, menu);
            } else {
                for (const auto& it : game::chat_filter_items())
                    if (it.menu == menu && manager().channel_map[it.channel] == w) manager().channel_map[it.channel] = 0;
                changed();
            }
            return true;
        };
        if (apply_group(game::ChatFilterMenu::Filters)) return 0;
        for (const auto& descriptor : game::chat_filter_groups())
            if (apply_group(descriptor.menu)) break;
        return 0;
    }
    lua_Integer ch = luaL_checkinteger(L, 2);
    if (ch < 0 || ch >= game::kChatChannelCount) return 0;
    auto joined = joined_channels(); // (after the check: a failed one longjmps past destructors)
    for (const auto& it : game::chat_filter_items()) {
        if (it.channel != ch) continue;
        bool here = manager().channel_map[it.channel] == w;
        if (here != on) toggle_channel(w, it.channel, filter_text(it, joined));
    }
    return 0;
}

// hs.ChatSetFont(slot, font)   -- the font of the tab that is up (CChatWindow::SetChatFont's index)
int ChatSetFont(lua_State* L) {
    int w = up_window(L, 1);
    if (w < 0) return 0;
    wnd(w).font = static_cast<int>(std::clamp<lua_Integer>(luaL_checkinteger(L, 2), 0, 10));
    changed();
    return 0;
}

// names = hs.ChatDefaultChannels()   -- the Channel menu's items (hs.ChatSetDefault's order, 0-based)
int ChatDefaultChannels(lua_State* L) {
    auto items = game::chat_default_items();
    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        lua_pushstring(L, eqstr(items[i].eqstr).c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// hs.ChatSetDefault(slot, index)   -- the Channel menu's item for the tab that is up (a disabled one does nothing)
int ChatSetDefault(lua_State* L) {
    int w = up_window(L, 1);
    lua_Integer d = luaL_checkinteger(L, 2);
    auto items = game::chat_default_items();
    if (w < 0 || d < 0 || d >= static_cast<lua_Integer>(items.size()) || !items[static_cast<std::size_t>(d)].enabled) return 0;
    const auto& it = items[static_cast<std::size_t>(d)];
    set_default(w, it.channel, 0, eqstr(it.eqstr));
    return 0;
}

// hs.ChatClear(slot)   -- empties the tab that is up (CChatWindow::Clear: its lines so far are not shown again)
int ChatClear(lua_State* L) {
    UiHost& h = host(L);
    int w = up_window(L, 1);
    if (w < 0 || !h.chat) return 0;
    wnd(w).cleared = h.chat->revision;
    ++manager().serial;
    return 0;
}

// on = hs.ChatTellWindows()   /   hs.ChatSetTellWindows(on)   -- tells in a window per player
int ChatTellWindows(lua_State* L) {
    lua_pushboolean(L, tell_windows_on());
    return 1;
}
int ChatSetTellWindows(lua_State* L) {
    ChatManager& m = manager();
    if (UiHost* h = chat_host(); h && h->chat) sync_lines(*h->chat); // from the next tell on
    m.tell_windows = lua_toboolean(L, 1) != 0;
    // The Options window's UseTellWindows box is the same setting (kept in the character's options.ini).
    if (net::GameConnection* c = character_conn())
        c->change_client_option(game::ClientOption::UseTellWindows, m.tell_windows ? 1 : 0);
    changed();
    return 0;
}

// modes = hs.ChatHitModes()   -- { id, name, mode, modes = { names } } per melee report category (CChatWindowManager
// +0x224..: 0 Normal, 1 Abbreviated, 2 Number Only / Miss Only)   /   hs.ChatSetHitMode(index, mode)  (1-based
// index; 7: ALL, 8..11 Hailstone's spell categories). ALL's mode is -1 when the categories differ.
int ChatHitModes(lua_State* L) {
    lua_newtable(L);
    for (int cat = 0; cat < 7 + static_cast<int>(game::SpellHitModes{}.size()); ++cat) {
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, cat + 1);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, hit_category_name(cat).c_str());
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, hit_mode_value(cat));
        lua_setfield(L, -2, "mode");
        lua_createtable(L, 3, 0);
        for (int mode = 0; mode < 3; ++mode) {
            lua_pushstring(L, eqstr(game::hit_mode_eqstr(cat, mode)).c_str());
            lua_rawseti(L, -2, mode + 1);
        }
        lua_setfield(L, -2, "modes");
        lua_rawseti(L, -2, cat + 1);
    }
    return 1;
}
int ChatSetHitMode(lua_State* L) {
    lua_Integer index = luaL_checkinteger(L, 1);
    lua_Integer mode = luaL_checkinteger(L, 2);
    if (index < 1 || index > 7 + static_cast<lua_Integer>(game::SpellHitModes{}.size()) || mode < 0 || mode > 2) return 0;
    set_hit_mode(static_cast<int>(index - 1), static_cast<int>(mode));
    return 0;
}

// hs.ChatLink(slot, message, data)   -- a link clicked in the output: 27 an item link (asks the server for it), 1001
// a name link (open_tell), 1002 a validated achievement snapshot.
int ChatLink(lua_State* L) {
    UiHost& h = host(L);
    if (luaL_checkinteger(L, 2) == kAchievementLink) {
        std::size_t length = 0;
        const char* data = luaL_checklstring(L, 3, &length);
        if (const auto metadata = achievement_link_metadata(std::string_view(data, length)); metadata && h.conn)
            h.conn->inspect_achievement_link(*metadata);
        return 0;
    }
    if (luaL_checkinteger(L, 2) == kNameLink) {
        open_tell(luaL_checkstring(L, 3));
        return 0;
    }
    if (luaL_checkinteger(L, 2) != 27) return 0;
    if (auto link = game::parse_item_link(game::stml_link_tag(luaL_checkstring(L, 3))); link && h.click_link)
        h.click_link(*link);
    return 0;
}

} // namespace

namespace {

// ---------------------------------------------------------------- the chat history (Hailstone)
// chat_history.txt: a header, then one line per chat line, oldest first: channel, rgb, colour, time (std::time, 0
// unknown), peer and text, tab-separated (\\, \t, \n, \r escaped in the text and peer). Version 1 had no time.

std::string history_escape(std::string_view v) {
    std::string out;
    for (char ch : v) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '\t') out += "\\t";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else out += ch;
    }
    return out;
}

std::string history_unescape(std::string_view v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '\\' || i + 1 == v.size()) {
            out += v[i];
            continue;
        }
        char n = v[++i];
        out += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
    }
    return out;
}

// The history's lines into the chat ahead of this session's, each in the window its colour goes to now (a tell
// makes no tell window), then a mark where the session starts. Once per entry into the world.
void restore_chat_history(UiHost& host, const std::filesystem::path& file) {
    ChatState& s = state();
    if (s.history_file != file) {
        s.history_file = file;
        s.history_restored = false;
    }
    if (s.history_restored || !host.chat || file.empty()) return;
    s.history_restored = true;
    std::ifstream in(file);
    std::string line;
    if (!in || !std::getline(in, line) || (line != kHistoryHeader && line != kHistoryHeaderV1)) return;
    const bool timed = line == kHistoryHeader;
    const int fields = timed ? 5 : 4;
    std::deque<ui::ChatLine> old;
    while (std::getline(in, line)) {
        std::vector<std::string_view> f;
        std::size_t at = 0;
        for (int k = 0; k < fields; ++k) {
            std::size_t tab = line.find('\t', at);
            if (tab == std::string::npos) break;
            f.push_back(std::string_view(line).substr(at, tab - at));
            at = tab + 1;
        }
        if (static_cast<int>(f.size()) != fields) continue;
        ui::ChatLine l;
        l.channel = static_cast<ui::ChatChannel>(std::atoi(std::string(f[0]).c_str()));
        l.rgb = static_cast<std::uint32_t>(std::strtoul(std::string(f[1]).c_str(), nullptr, 16));
        l.color = static_cast<std::uint32_t>(std::strtoul(std::string(f[2]).c_str(), nullptr, 10));
        if (timed) l.time = std::strtoll(std::string(f[3]).c_str(), nullptr, 10);
        l.peer = history_unescape(f[timed ? 4 : 3]);
        l.text = history_unescape(std::string_view(line).substr(at));
        l.restored = true;
        old.push_back(std::move(l));
        if (old.size() > kHistoryLines) old.pop_front();
    }
    if (old.empty()) return;
    ChatManager& m = manager();
    ui::ChatLine mark;
    mark.text = std::string(kHistoryMark);
    mark.rgb = 0x8a8a8a; // grey
    mark.restored = true;
    mark.window = wnd(0).serial;
    const std::size_t n = old.size();
    host.chat->lines.push_front(std::move(mark));
    for (auto it = old.rbegin(); it != old.rend(); ++it) {
        int w = m.channel_map[static_cast<std::size_t>(game::chat_channel_of_color(static_cast<std::int32_t>(color_of(*it))))];
        it->window = wnd(used(w) ? w : 0).serial;
        host.chat->lines.push_front(std::move(*it));
    }
    // (counted as lines added, and every line numbered again in order: the restored ones come before the rest)
    host.chat->revision += n + 1;
    auto& lines = host.chat->lines;
    for (std::size_t i = 0; i < lines.size(); ++i) lines[i].seq = host.chat->revision - lines.size() + i + 1;
    trim_windows(*host.chat);
    while (lines.size() > host.chat->max_lines) lines.pop_front();
    log::info("chat history: {} lines from {}", n, file.string());
}

} // namespace

void chat_open_tell(const std::string& name) { open_tell(name); }

void save_chat_history(bool ending) {
    ChatState& s = state();
    UiHost* h = s.host;
    if (!h || !h->chat || s.history_file.empty() || h->chat->lines.empty()) return;
    const auto& lines = h->chat->lines;
    std::size_t first = lines.size() > kHistoryLines ? lines.size() - kHistoryLines : 0;
    std::filesystem::path tmp = s.history_file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        out << kHistoryHeader << '\n';
        for (std::size_t i = first; i < lines.size(); ++i) {
            const ui::ChatLine& l = lines[i];
            if (l.restored && l.text == kHistoryMark) continue;
            out << static_cast<int>(l.channel) << '\t' << std::format("{:x}", l.rgb) << '\t' << l.color << '\t'
                << l.time << '\t' << history_escape(l.peer) << '\t' << history_escape(l.text) << '\n';
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, s.history_file, ec);
    if (ec) log::warn("chat history: {}", ec.message());
    if (ending) s.history_restored = false;
}

// The chat state keeps the host it was last given: forget one that goes away (a test's, a closed session's), so the
// user colours do not read through it later (a label's colour without an UiApi).
void chat_host_gone(const UiHost& host, const ui::UiIni& ini) {
    if (state().host == &host) {
        state().host = nullptr;
        state().colors_loaded = false;
    }
    if (manager().ini == &ini) manager().ini = nullptr; // the layout it pointed into goes with the API
}

void chat_layout_loaded(ui::UiIni& ini, UiHost& host, const std::filesystem::path& user_dir) {
    state().host = &host; // the host the layout is for (the Lua functions may not be registered yet)
    // The user colours are the character's: read again from its chat.ini.
    state().chat_ini = CharacterFiles{user_dir}.chat_ini();
    state().colors_loaded = false;
    ChatManager& m = manager();
    m.reset();
    m.ini = &ini;
    auto num = [&](const std::string& key, int def) {
        auto v = ini.get("ChatManager", key);
        return v && !v->empty() ? std::atoi(v->c_str()) : def;
    };
    auto text = [&](const std::string& key, const std::string& def) { return ini.get("ChatManager", key).value_or(def); };
    // CChatWindowManager::CChatWindowManager @0x654c80.
    m.default_channel = 8; // eqclient.ini [Defaults] DefaultChannel
    if (host.game) {
        IniFile eqclient;
        if (auto path = host.game->find("eqclient.ini")) eqclient.load(*path);
        if (auto v = eqclient.get("Defaults", "DefaultChannel"); v && !v->empty()) m.default_channel = std::atoi(v->c_str());
    }
    m.tell_windows = num("TellWindows", 0) != 0;
    // CChatWindowManager_LoadLanguageSettings: the language if the character knows it, else 0.
    auto language = [&](int id) {
        if (id < 0 || id > 31) return 0;
        if (host.conn && host.conn->profile()) {
            const auto& langs = host.conn->profile()->languages;
            if (static_cast<std::size_t>(id) >= langs.size() || langs[static_cast<std::size_t>(id)] == 0) return 0;
        }
        return id;
    };
    ChatWnd& main = m.wnd[0];
    main.name = text("ChatWindow0_Name", eqstr(game::kStrMainChat));
    if (main.name.empty()) main.name = "Main Chat";
    main.language = language(num("ChatWindow0_LanguageId", 0));
    main.channel = num("ChatWindow0_DefaultChannel", m.default_channel);
    main.tell_target = text("ChatWindow0_TellTarget", "").substr(0, 0x3f);
    main.chat_index = num("ChatWindow0_ChatChannel", 0);
    main.scrollbar = num("ChatWindow0_Scrollbar", 1) != 0;
    main.font = num("ChatWindow0_FontStyle", 3);
    main.up = num("ChatWindow0_Up", -1);
    // Hailstone (a deliberate change from RoF2): a character whose file keeps no chat windows of its own yet (no
    // NumWindows) takes defaults.ini's, as tabs of the main window; the game's defaults.ini makes "Other Chat"
    // (ChatWindow1) a window of its own. Once the file has its own (save_manager writes them at every change), only
    // they are read: read through defaults.ini, a window the player closed, whose keys save_manager erases, came back
    // at the next start.
    const bool own_windows = ini.get_own("ChatManager", "NumWindows").has_value();
    auto has = [&](const std::string& key) {
        return own_windows ? ini.get_own("ChatManager", key).has_value() : ini.get("ChatManager", key).has_value();
    };
    int count = std::clamp(num("NumWindows", own_windows ? 0 : 1), 0, kMaxChatWindows - 1);
    for (int w = 1; w <= count; ++w) {
        std::string k = std::format("ChatWindow{}_", w);
        m.num_windows = w + 1;
        // a window is there when it has a language key (the slot of a closed one has none)
        if (!has(k + "LanguageId") && !has(k + "Language")) continue;
        ChatWnd& c = m.wnd[static_cast<std::size_t>(w)];
        c.used = true;
        c.serial = m.next_serial++;
        c.language = language(num(k + "LanguageId", 0));
        c.channel = num(k + "DefaultChannel", m.default_channel);
        c.chat_index = num(k + "ChatChannel", 0);
        c.tell_target = text(k + "TellTarget", "").substr(0, 0x3f);
        c.name = text(k + "Name", std::format("ChatWindow{}", w));
        c.scrollbar = num(k + "Scrollbar", 1) != 0;
        int font = num(k + "FontStyle", 3);
        c.font = font < 0 || font > 7 ? 3 : font;
        c.dock = own_windows ? num(k + "Dock", -1) : 0;
        c.up = num(k + "Up", -1);
    }
    for (int w = 1; w < kMaxChatWindows; ++w) {
        ChatWnd& c = m.wnd[static_cast<std::size_t>(w)];
        if (c.used && (c.dock == w || !used(c.dock) || wnd(c.dock).dock >= 0)) c.dock = -1; // only one level of tabs
    }
    for (int w = 0; w < kMaxChatWindows; ++w) m.wnd[static_cast<std::size_t>(w)].select = true;
    int locked = num("LockedActiveWindow", 0); // (the original's default: -1; set_locked)
    m.locked = locked >= 0 && locked < m.num_windows && used(locked) ? locked : (locked < 0 ? -1 : 0);
    for (int ch = 0; ch < game::kChatChannelCount; ++ch) {
        int w = num(std::format("ChannelMap{}", ch), -1);
        if (w >= 0 && w < m.num_windows && used(w)) m.channel_map[static_cast<std::size_t>(ch)] = w;
    }
    for (int i = 0; i < game::kChatHitModeCount; ++i) {
        int mode = num(std::format("HitMode{}", i), 0);
        if (mode >= 0 && mode < 3) m.hit_modes[static_cast<std::size_t>(i)] = mode;
    }
    for (std::size_t i = 0; i < m.spell_hit_modes.size(); ++i) {
        int mode = num(std::format("SpellHitMode{}", i), 0);
        if (mode >= 0 && mode < 3) m.spell_hit_modes[i] = mode;
    }
    restore_chat_history(host, CharacterFiles{user_dir}.chat_history());
    if (host.chat) {
        m.synced = host.chat->revision;
        for (auto& c : m.wnd) c.seen = host.chat->revision;
    }
    push_hit_modes();
    ++m.serial;
}

bool chat_window_used(int slot) {
    int w = slot - 1;
    return used(w) && host_of(w) == w;
}

void chat_window_closed(int slot) { free_window(slot - 1); }

void request_chat_link_all(std::vector<std::string> tags) {
    if (tags.empty()) return;
    state().link_all = std::move(tags);
}

void request_chat_focus(const std::string& text) {
    ChatState& s = state();
    s.take_focus = s.take_focus.value_or(std::string()) + text;
    s.focus_window = -1;
}

void put_chat_input(const std::string& text) {
    ChatState& s = state();
    s.take_focus = text;
    s.replace_focus = true;
    s.focus_window = -1;
}

namespace {

// The player at the reply keys' index (CYCLEREPLY steps it, each new tell puts it back on the newest), or null.
const std::string* reply_name(const net::GameConnection& c, int index) {
    const auto& tells = c.social().last_tells;
    if (index < 0 || static_cast<std::size_t>(index) >= tells.size() || tells[static_cast<std::size_t>(index)].empty())
        return nullptr;
    return &tells[static_cast<std::size_t>(index)];
}

} // namespace

void reply_to_last_tell() {
    // __ExecuteCmd case 0x23 (REPLY): with no last tell eqstr 12364; else the active window's input holds
    // "/tell <the player at the reply index> " and has the keys. __LastTell holds the players who told us and whom
    // we told (msg_new_text's LastTell_Add on channels 7 and 14), the newest first.
    UiHost* h = chat_host();
    if (!h || !h->conn) return;
    const std::string* name = reply_name(*h->conn, h->conn->last_tell_index());
    if (!name) return h->conn->system_message(game::kStrNoTellsToReply);
    put_chat_input(game::tell_menu_text(*name, slash_word(13974, "/tell").substr(1)));
}

void reply_target_key() {
    // __ExecuteCmd case 69 (RTARGET) @0x4d7c18: with no player at the reply index eqstr 12365; else the active
    // window's input holds "/rtarget <player>" and has the keys (Enter runs it).
    UiHost* h = chat_host();
    if (!h || !h->conn) return;
    const std::string* name = reply_name(*h->conn, h->conn->last_tell_index());
    if (!name) return h->conn->system_message(game::kStrNoTellsToTarget);
    put_chat_input(game::reply_target_text(*name, slash_word(14202, "/rtarget")));
}

void tell_key() {
    // __ExecuteCmd case 15 (TELL) @0x4d76d8: the active window's input holds "/tell " (eqstr 13974) and has the keys.
    // A GM with a petitioner in CPetitionQWnd gets "/tell <petitioner> "; this client has no petition queue.
    put_chat_input(slash_word(13974, "/tell") + " ");
}

void chat_history_key(bool older) {
    // __ExecuteCmd cases 53 / 54 (HISTORY_UP / HISTORY_DOWN), while the keys go to the game: the active window's
    // HistoryBack (0x655930) / HistoryForward fills its input, which then has the keys. The steps are the chat
    // input's Up / Down (hs.ChatHistory); without a line sent before the input only takes the keys.
    auto line = step_history(older);
    ChatState& s = state();
    s.take_focus = line.value_or(std::string());
    s.replace_focus = line.has_value();
    s.focus_window = -1;
}

void chat_page_key(int dir) {
    // Page Up / Page Down scroll the active output by its height less 8 pixels through hs.ChatPage.
    ChatState& s = state();
    s.page = dir < 0 ? -1 : 1;
    s.page_window = active_window();
}

bool set_active_chat_default(int channel, const std::string& name, int index) {
    // Cmd_channel @0x4f2ae0: CChatWindowManager::GetActiveChatWindow's channel.
    int w = active_window();
    if (!used(w)) return false;
    ChatWnd& c = wnd(w);
    c.channel = channel;
    if (channel == 7 || channel == 0x14) c.tell_target = name.substr(0, 0x3f);
    if (channel == 0x14) c.chat_index = index;
    changed();
    return true;
}

std::string chat_visible_text() {
    // CChatWindowManager::GetAllVisibleText @0x650c60: each chat window's CStmlWnd::GetVisibleText (at most 20 lines
    // from the first one in view) and "\n===================================\n". The scripts' scroll position is not
    // known here: the last 20 lines of each window.
    ChatState& s = state();
    std::string out;
    if (!s.host || !s.host->chat) return out;
    sync_lines(*s.host->chat);
    const ui::ChatView& v = *s.host->chat;
    for (int w = 0; w < kMaxChatWindows; ++w) {
        if (!used(w)) continue;
        std::vector<std::string> lines;
        for (std::size_t k = 0; k < v.lines.size(); ++k)
            if (line_seq(v, k) > wnd(w).cleared && window_of_line(v.lines[k]) == w)
                lines.push_back(game::strip_links(v.lines[k].text));
        std::size_t from = lines.size() > 20 ? lines.size() - 20 : 0;
        for (std::size_t i = from; i < lines.size(); ++i) out += lines[i] + "\n";
        out += "\n===================================\n";
    }
    return out;
}

ui::UiIni* chat_ui_ini() { return manager().ini; }

bool set_active_chat_font(int font) {
    // CChatWindow::SetChatFont @0x656370 on the active window: the font index as given (0..10, the window manager's
    // fonts; the scripts draw it with ui.PushChatFont).
    int w = active_window();
    if (!used(w)) return false;
    wnd(w).font = font;
    changed();
    return true;
}

void clear_chat_windows(bool all) {
    // CChatWindow::Clear on the active window (/clearchat), CChatWindowManager::ClearAllWindows (/clearallchat).
    ChatState& s = state();
    if (!s.host || !s.host->chat) return;
    int active = active_window();
    for (int w = 0; w < kMaxChatWindows; ++w)
        if (used(w) && (all || w == active)) wnd(w).cleared = s.host->chat->revision;
    ++manager().serial;
}

void clear_chat_buffer() {
    ChatState& s = state();
    if (!s.host || !s.host->chat) return;
    s.host->chat->lines.clear();
    ++s.host->chat->revision;
}

void register_chat_api(lua_State* L, UiHost& h) {
    state().host = &h;
    static const luaL_Reg fns[] = {{"ChatWindows", ChatWindows}, {"ChatText", ChatText}, {"ChatSubmit", ChatSubmit},
                                   {"ChatTakeFocus", ChatTakeFocus}, {"ChatHistory", ChatHistory},
                                   {"ChatCycleReply", ChatCycleReply}, {"ChatPage", ChatPage},
                                   {"ChatInfo", ChatInfo}, {"ChatLink", ChatLink}, {"ChatActivate", ChatActivate},
                                   {"ChatSetAlwaysHere", ChatSetAlwaysHere},
                                   {"ChatNewWindow", ChatNewWindow}, {"ChatCloseWindow", ChatCloseWindow},
                                   {"ChatSetTitle", ChatSetTitle}, {"ChatFilters", ChatFilters},
                                   {"ChatFilterGroups", ChatFilterGroups},
                                   {"ChatSetFilter", ChatSetFilter}, {"ChatSetFont", ChatSetFont},
                                   {"ChatDefaultChannels", ChatDefaultChannels}, {"ChatSetDefault", ChatSetDefault},
                                   {"ChatClear", ChatClear}, {"ChatNewTab", ChatNewTab},
                                   {"ChatCloseTab", ChatCloseTab}, {"ChatSelectTab", ChatSelectTab},
                                   {"ChatTellWindows", ChatTellWindows}, {"ChatSetTellWindows", ChatSetTellWindows},
                                   {"ChatHitModes", ChatHitModes}, {"ChatSetHitMode", ChatSetHitMode},
                                   {"ChatMenu", ChatMenu},
                                   {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h);
    luaL_setfuncs(L, fns, 1);
}

} // namespace hailstone::app
