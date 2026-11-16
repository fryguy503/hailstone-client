#pragma once

// CChatWindowManager's routing tables (and CChatWindow's WndNotification): every line a chat window shows
// goes by its colour (dsp_chat's) to one of 57 chat channels (GetChannelFromColor @0x6504c0), and the channel to one
// window (ChannelMap, +0x8c; the main window unless the Filters menu moved it). The tables here are the original's
// data; app/lua_ui_chat.cpp keeps the windows.
// Hailstone (a deliberate difference, docs/mechanics.md "Extended chat routing"): channels 57..107 and the user
// colours 355..408 extend the tables for a server that sends a later client's extended chat colours. Stock EQEmu
// sends none of them and the client makes none itself, so they change nothing on a stock server.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::game {

inline constexpr int kChatChannelCountRoF2 = 57; // ChannelMap's size (SetChannelMap's bound 0x39)
inline constexpr int kChatChannelCount = 108;    // Hailstone: with the extended channels 57..107
inline constexpr int kChatUserColorCountRoF2 = 99; // CDisplay's user colours (GetUserDefinedColor's bound 0x62)
inline constexpr int kChatUserColorCount = 153;    // Hailstone: with the extended colours 355..408 (indices 99..152)
inline constexpr int kChatChannelOther = 22;  // GetChannelFromColor's answer for any colour it does not list
inline constexpr int kChatChannelFirstServer = 12; // the chat server's channels 1..10 are channels 12..21
inline constexpr int kChatHitModeCount = 8;   // CChatWindowManager::HitModes (+0x224; [ChatManager] HitMode0..7)

// CChatWindowManager::GetChannelFromColor @0x6504c0: the chat channel of a colour (the user colours 0x100..0x15e
// by the table at 0x650780; any other colour, the basic ones below 256 too, is channel 22). Hailstone adds the
// extended colours 356..408 (chat_windows.cpp).
int chat_channel_of_color(std::int32_t color);

// The Filters menu (CChatWindowManager::InitContextMenu @0x6513c0): the menu (+0x180) and its Chat Channel (+0x1a4),
// Melee (+0x194) and Spells (+0x19c) submenus, each item a channel (item id 100 + channel) by its eqstr name, in
// the menu's order. An item moves its channel to the window it was picked in, or back to the main window when it
// was there already (CChatWindow::WndNotification @0x656640, ids 100..0x9c); each menu's "ALL" (13205; ids 0x9d,
// 0x9e, 0x9f, 0xa0) moves every channel of that menu to the window.
// Hailstone: Extended (server) and its submenus hold the extended channels 57..107, after the original menu's ALL.
enum class ChatFilterMenu : std::uint8_t {
    Filters, ChatChannel, Melee, Spells,
    Extended, DirectDamage, DamageOverTime, Healing, DamageShields, EnvironmentalDamage, Randoms, Other
};
struct ChatFilterItem {
    ChatFilterMenu menu;
    std::uint8_t channel;
    std::uint32_t eqstr; // its name; 0 for the chat channels, named by the chat server's joined list
    std::string_view label{}; // Hailstone: the name of an extended channel (eqstr 0), which RoF2's strings lack
};
std::span<const ChatFilterItem> chat_filter_items();
struct ChatFilterGroup {
    ChatFilterMenu menu;
    ChatFilterMenu parent;
    std::uint32_t eqstr;
    std::string_view label;
};
// Each submenu once, with the menu it is in (Filters for the original three). ALL moves a menu's own items only.
std::span<const ChatFilterGroup> chat_filter_groups();
// The submenu rows of the Filters menu (after System Messages): Chat Channel 6638, Melee 6639, Spells 6640.
inline constexpr std::uint32_t kStrFilterChatChannel = 6638, kStrFilterMelee = 6639, kStrFilterSpells = 6640;
inline constexpr std::uint32_t kStrAll = 13205;
// What a menu's ALL says in the window (eqstr 6040..6043, colour 10); 0 for Hailstone's extended submenus.
std::uint32_t chat_filter_all_eqstr(ChatFilterMenu menu);
// What moving a channel says after its item's text (" " and eqstr 3135 / 3136, colour 10); 3134 "Filter %1" names
// an item that is not in the menu.
inline constexpr std::uint32_t kStrFilterHere = 3135, kStrFilterMain = 3136, kStrFilterNumbered = 3134;

// The Channel menu (+0x1ac): where plain typed text goes (CChatWindow +0x22c, ChatChannel; the number InterpretCmd's
// default channel switch takes). Tell (0xc9) is in the menu but disabled; the chat channels (0xcf + k, channel 0x14
// with the index k) are its Chat Channel submenu. Picking one says its text and eqstr 3138 (colour 10).
struct ChatDefaultItem {
    std::uint32_t id;      // the menu item id
    std::uint8_t channel;  // the ChatChannel it sets
    std::uint32_t eqstr;
    bool enabled;
};
std::span<const ChatDefaultItem> chat_default_items();
inline constexpr std::uint32_t kStrDefaultChannelSet = 3138, kStrDefaultChannelNumbered = 3137;

// The Hit Modes menu (+0x1f8): the six categories (eqstr 15014..15019), each a submenu of Normal 15034,
// Abbreviated 15035 and Number Only 15036 (Miss Only 15037 for the miss categories), then ALL (13205) over all
// eight modes. Picking a category's mode says "%s %s" (the category, the mode; dsp_chat colour 0x111); ALL says
// eqstr 6044..6046 in the window (colour 10).
// Hailstone adds four spell categories before ALL; their settings are separate SpellHitMode0..3 keys.
inline constexpr std::uint32_t kStrHitModes = 15038;
std::uint32_t hit_category_eqstr(int category);
std::uint32_t hit_mode_eqstr(int category, int mode); // category 6: ALL's submenu
std::uint32_t hit_mode_all_eqstr(int mode);
// Hailstone's spell hit modes, in SpellHitReport order. These have no RoF2 menu/string-table entries.
std::string_view spell_hit_category_name(int category);

// The rest of the chat window's menu (InitContextMenu's last part): Window 6643, Scrollbar 6047, New Chat Window
// 6644, Rename Window 3140 (CTextEntryWnd with eqstr 3141, 15 characters), Always Chat Here 15009 (eqstr 15010 /
// 15011 in the window when toggled, colour 10), Reply to... 3147, Tell a friend... 3148, Tell a raidmember... 3149,
// Filters 6645, Channel 6646, Language 6647, Hit Modes 15038. "Cannot create any more chat windows!" 6648 (colour
// 13 in the main window).
inline constexpr std::uint32_t kStrWindow = 6643, kStrScrollbar = 6047, kStrNewChatWindow = 6644,
                               kStrRenameWindow = 3140, kStrRenamePrompt = 3141, kStrAlwaysChatHere = 15009,
                               kStrChatHereOff = 15010, kStrChatHereOn = 15011, kStrReplyTo = 3147,
                               kStrTellFriend = 3148, kStrTellRaidMember = 3149, kStrFilters = 6645,
                               kStrChannel = 6646, kStrLanguage = 6647, kStrNoMoreChatWindows = 6648,
                               kStrMainChat = 6623, kStrChat = 6624;

// The text a tell menu's name puts into the active window's input (WndNotification, ids 0x1f4.. reply, 0x226..
// raid member, 0x28a.. friend): "%c%s %s " of '/', "tell" (eqstr 13974 past its slash) and the name; a name with a
// '.' (a chat server name) takes ';' and loses a leading '.', and a name starting with '[' or ';' takes that
// character instead and loses it.
std::string tell_menu_text(std::string name, const std::string& tell_word);

// The reply keys' list (__LastTell: 50 names, the newest first) and its index (0xde21d8, back to 0 with each
// LastTell_Add).
inline constexpr std::uint32_t kStrNoTellsToReply = 12364, kStrNoTellsToTarget = 12365;
// LastTell_CycleIndex @0x4e1fe0: with no names false (the caller shows 12364) and the index stays. `newer` (its
// argument 1, CYCLEREPLY_BACK): one newer, from the newest round to the oldest; else one older while there is one,
// else back to the newest.
bool cycle_last_tell(const std::vector<std::string>& names, int& index, bool newer);
// RTARGET's text (__ExecuteCmd case 69 @0x4d7c18): "/rtarget <name>" (eqstr 14202), a chat server name losing its
// leading ';' or '['.
std::string reply_target_text(const std::string& name, const std::string& rtarget_word);
// __ExecuteCmd case 36 (CYCLEREPLY / CYCLEREPLY_BACK, `newer`), run while the chat input has the keys on what it
// holds: past its first character it must start with "rtarget" or else "tell" (eqstr 14202 / 13974 past the slash,
// case-sensitive: CXStr::CompareN mode 0). Then the index steps (cycle_last_tell, its 12364 when there are no names)
// and the input becomes the name at the index as "/rtarget <name>" (reply_target_text) or "/tell <name> "
// (tell_menu_text), or 12365 shows when there is no name there. Otherwise nothing happens.
// A Hailstone difference: for a name not of the chat server the original's "rtarget" branch passes "rtarget" as
// sprintf's format (@0x4d7aa8; the chat server branch @0x4d7ac2 has "/%s %s"), so its input becomes "rtarget"; here
// it is "/rtarget <name>" as in that other branch.
struct CycleReply {
    std::vector<std::uint32_t> messages; // eqstr ids shown, in order
    std::string text;                    // the input's new text; empty: it stays
};
CycleReply cycle_reply(std::string_view input, const std::vector<std::string>& names, int& index, bool newer,
                       const std::string& tell_word, const std::string& rtarget_word);

} // namespace hailstone::game
