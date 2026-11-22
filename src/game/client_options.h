#pragma once

// The game options of the original's Options window (EQUI_OptionsWindow.xml; COptionsWnd's General, Display, Mouse
// and Mail pages): the settings CEverQuest::loadOptions @0x544be0 reads from eqclient.ini [Options] / [Defaults]
// (defaults.ini giving the defaults), and the few the UI ini keeps ([Main] ContextMenus, UseTellWindows). Each has a
// name for the Options window (hs.Option / hs.SetOption) and an id for the code that acts on it.
//
// The asset folder's eqclient.ini gives the values a character has not set; every change goes to the character's
// options.ini (core/character_files.h), as the chat filters go to chat.ini. Window, frame, audio, name and mouse
// sensitivity settings are the client's own (hailstone.ini; app/client_app_options.cpp), not these.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hailstone {
class IniFile;
}

namespace hailstone::game {

enum class ClientOption : std::uint8_t {
    // General page
    FastItemDestroy,         // Options Destroy (0xde0bf4): destroy without the confirmation
    GuildInvites,            // Options GuildInvites (0xde0bb4)
    Anonymous,               // Options Anonymous bit 0 (0xde0ba8)
    Roleplaying,             // Options Anonymous bit 1
    TradeWith,               // Options Trade (0xde0bb0): 2 everyone, 1 party members, 0 no one
    LootNoDrop,              // Options LootNoDrop (0xde0d44): 0 always, 1 not usable, 2 never (/lootnodrop)
    FastDrop,                // Options NoSafeDrop (0xde0d40): 0 with confirmation, 1 without, -1 never (/fastdrop)
    PetOwnerNames,           // Options PetOwnerNames (0xde0bd0)
    ClickThroughMask,        // Options ClickThroughMask (0xddfce4, a byte): 1 self, 2 pets, 4 mercenaries
    LootAllConfirm,          // Options LootAllConfirm (0xde0d28)
    DismissMercenaryConfirm, // Options DismissMercenaryConfirm (0xde0d2c)
    RaidInviteConfirm,       // Options RaidInviteConfirm (0xde0d2d)
    OfflineModeConfirm,      // Options OfflineModeConfirm (0xde0d2e)
    AANoConfirm,             // Options AANoConfirm (0xde0d04)
    PointMerchantNoConfirm,  // Options pointMerchantNoConfirm (0xde0d08)
    LeadershipNoConfirm,     // Options LeadershipNoConfirm (0xde0d0c; saved, never loaded: 0 each session)
    LeftClickTargetHelp,     // Defaults LeftClickTargetHelp (0xde0d52)
    AllowAutoDuck,           // Defaults AllowAutoDuck (0xde0d5a)
    AllowAutoStand,          // Defaults AllowAutoStand (0xde0d5b)
    AutojoinHelp,            // Defaults AutojoinHelp (0xde0d5c)
    AcceptKickRequests,      // Defaults AcceptKickRequests (0xde0d5f)
    TradeskillCtrlClick,     // TradeskillWnd CtrlBypass (0xde0d59): 1 a right click opens a tradeskill container in
                             // the tradeskill window, ctrl in its own; 0 the other way (the box shows it inverted)
    ContextMenus,            // UI [Main] ContextMenus (0xde0d51)
    UseTellWindows,          // UI [Main] UseTellWindows (0xde0d58)
    // Display page
    ShowTargetRing,          // Options Show3dTargetIndicator
    ShowBuffTimer,           // Defaults ShowBuffTimer (CSpellDisplayManager)
    ShowBuffCounters,        // Defaults ShowBuffCounters
    ShowBuffLimitUse,        // Defaults ShowBuffLimitUse
    ShowSongTimer,           // Defaults ShowSongTimer
    ShowSongCounters,        // Defaults ShowSongCounters
    ShowSongLimitUse,        // Defaults ShowSongLimitUse
    // Mouse page
    LookSpring,              // Options LookSpring (0xde0bec)
    MouseLook,               // Options MouseActive (0xde0bf0)
    MouseTurnZoom,           // Options MouseTurnZoom (0xde0d14)
    MouseRightHanded,        // Options MouseRightHanded (0xddf744)
    // Mail page
    MailForwarding,          // Mail ForwardingToEmailAddressInStationAccount
    NewMailNotify,           // Mail NewMailNotification
    NewMailSound,            // Mail NewMailAudioNotification
    AddressBookConfirmDelete,// Mail ConfirmAddressBookDeletion
    // Set by slash commands only (not on the Options window's pages)
    Log,                     // Defaults Log (0xde0984): chat to the log file (/log; TRUE / FALSE)
    TrackPlayers,            // Defaults TrackPlayers (0xddfcb0; /trackplayers): 0 off, 1 all, 2 group (TRUE / FALSE /
                             // GROUP)
    TrackPets,               // Defaults TrackPets (0xddfcb5; /trackpets; TRUE / FALSE)
    TrackMercs,              // Defaults TrackMercs (0xddfcb4; /trackmercs; TRUE / FALSE)
    TrackSortType,           // Defaults TrackSortType (0xddfcb8; /tracksort): 0 normal .. 6 reverse consider (NORMAL ...)
    TrackFilterType,         // Defaults TrackFilterType (0xddfcbc): the con colours a ranger hides (/trackfilter
                             // changes it without saving)
    TrackAutoUpdate,         // Defaults TrackAutoUpdate (the tracking window's check box, CTrackingWnd +0x230: written
                             // TRUE / FALSE when clicked, read back as the window opens; missing FALSE)
    InvWinLabels,            // Defaults InvWinLabels (0xde09e4; /invwinlabels; TRUE / FALSE)
    TargetGroupBuff,         // Defaults TargetGroupBuff (/targetgroupbuff writes "0" / "1"; DoMainLoop @0x53d936 reads it
                             // with GetEQClientInt, default "*NULL*", and sends its atoi unless it starts with '*'):
                             // kTargetGroupBuffUnset when missing
    HideCorpses,             // Hailstone HideCorpses (/hidecorpses, kept per character)
    Count
};

// Stable values in options.ini and the Lua UI settings API. The wire actions differ.
enum class HideCorpseMode : int { None = 0, All = 1, AllButGroup = 2, Looted = 3, Npc = 4, Always = 5 };

inline constexpr int kTargetGroupBuffUnset = -0x7fffffff - 1;

constexpr std::size_t kClientOptionCount = static_cast<std::size_t>(ClientOption::Count);

struct ClientOptionInfo {
    ClientOption id;
    const char* name;    // the Options window's name (hs.Option)
    const char* section; // eqclient.ini's (or the UI ini's) section and key
    const char* key;
    int def;             // defaults.ini's default
    int min, max;
    // Read as CEverQuest::ReadClientINIBool @0x543410 / ReadUIINIBool @0x543880 read it: a value starting with F, f
    // or 0 is off, anything else (an empty value too) on.
    bool ini_bool = false;
    // Kept as text the way the original's constructor reads it back (game/option_commands.h): T/t/1 on
    // (TRUE / FALSE), the tracking players' TRUE / FALSE / GROUP, the sort's NORMAL .. RCONSIDER.
    enum class Text : std::uint8_t { None, TrueFalse, TrackPlayers, TrackSort };
    Text text = Text::None;
};

// Every option, in ClientOption order.
extern const std::array<ClientOptionInfo, kClientOptionCount> kClientOptions;

const ClientOptionInfo& client_option_info(ClientOption id);
std::optional<ClientOption> find_client_option(std::string_view name);

// CEverQuest::ReadClientINIBool @0x543410 / ReadUIINIBool @0x543880 on a value: missing gives the default; one
// starting with F, f or 0 is false, anything else (an empty one too) true.
inline bool ini_bool_value(const std::optional<std::string>& text, bool fallback) {
    if (!text) return fallback;
    char c = text->empty() ? '\0' : (*text)[0];
    return !(c == 'F' || c == 'f' || c == '0');
}

// The pet window's PopupOnPet (CPetInfoWnd +0x765, the UI ini's PopupOnPet in its section, default on), which the
// Options window shows as "pet_window_popup" but the window keeps: ReadUIINIBool(section, "PopupOnPet", 1).
inline bool pet_window_popup_setting(const std::optional<std::string>& text) { return ini_bool_value(text, true); }
// CPetInfoWnd_RefreshPetState @0x7104a0: the pet state the window saw (+0x764) becomes `has_pet`; when it changed
// and PopupOnPet is on, the window shows (a pet came) or hides (it went): true, the window's shown state to become
// `has_pet`.
inline bool pet_window_refresh(bool& had_pet, bool has_pet, bool popup) {
    if (has_pet == had_pet) return false;
    had_pet = has_pet;
    return popup;
}

// 0x761f10, CContextMenuManager::PopupMenu @0x862220's test: a context menu (a window's own, the default window
// menu among them) opens only with ContextMenus (0xde0d51) on and the player not down (Cur_HP below 1, or the PC's
// state 3 / 4: knocked out, waiting to respawn).
inline bool context_menus_open(bool context_menus_option, bool player_down) {
    return context_menus_option && !player_down;
}

class ClientOptions {
public:
    ClientOptions();

    int get(ClientOption id) const { return values_[static_cast<std::size_t>(id)]; }
    bool on(ClientOption id) const { return get(id) != 0; }
    // Clamped to the option's range, or the default for an invalid corpse mode. Returns the value kept.
    int set(ClientOption id, int value);

    // The values of `ini` (the asset folder's eqclient.ini with the character's options.ini over it); missing keys
    // take the defaults. Anonymous and Roleplaying share the Anonymous key's bits.
    void load(const IniFile& ini);
    // Writes `id` to `ini` (its section and key, as the original saves it).
    void store(ClientOption id, IniFile& ini) const;

private:
    std::array<int, kClientOptionCount> values_{};
};

} // namespace hailstone::game
