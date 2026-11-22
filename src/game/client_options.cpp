#include "game/client_options.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "core/ini_file.h"
#include "core/msvc_atoi.h"
#include "game/option_commands.h"

namespace hailstone::game {

namespace {

using O = ClientOption;

} // namespace

// The defaults are loadOptions' (defaults.ini's fallbacks), ReadClientINIBool's (TradeskillWnd CtrlBypass 1, read
// with the UI ini's), ReadUIINIBool's (CDisplay @0x48xxxx: ContextMenus 1, UseTellWindows 0), CSpellDisplayManager's (the buff and song boxes, 1) and the mail page's
// (sub_00700dc0: MailOptions in the character ini).
const std::array<ClientOptionInfo, kClientOptionCount> kClientOptions = {{
    {O::FastItemDestroy, "fast_item_destroy", "Options", "Destroy", 0, 0, 1},
    {O::GuildInvites, "guild_invites", "Options", "GuildInvites", 1, 0, 1},
    {O::Anonymous, "anonymous", "Options", "Anonymous", 0, 0, 1},
    {O::Roleplaying, "roleplaying", "Options", "Anonymous", 0, 0, 1},
    {O::TradeWith, "trade_with", "Options", "Trade", 2, 0, 2},
    {O::LootNoDrop, "loot_no_drop", "Options", "LootNoDrop", 0, 0, 2},
    {O::FastDrop, "fast_drop", "Options", "NoSafeDrop", 0, -1, 1},
    {O::PetOwnerNames, "pet_owner_names", "Options", "PetOwnerNames", 1, 0, 1},
    {O::ClickThroughMask, "click_through", "Options", "ClickThroughMask", 0, 0, 255}, // atoi into the byte
    {O::LootAllConfirm, "loot_all_confirm", "Options", "LootAllConfirm", 1, 0, 1},
    {O::DismissMercenaryConfirm, "dismiss_merc_confirm", "Options", "DismissMercenaryConfirm", 1, 0, 1, true},
    {O::RaidInviteConfirm, "raid_invite_confirm", "Options", "RaidInviteConfirm", 1, 0, 1, true},
    {O::OfflineModeConfirm, "offline_mode_confirm", "Options", "OfflineModeConfirm", 1, 0, 1, true},
    {O::AANoConfirm, "aa_no_confirm", "Options", "AANoConfirm", 0, 0, 1},
    {O::PointMerchantNoConfirm, "point_merchant_no_confirm", "Options", "pointMerchantNoConfirm", 0, 0, 1},
    {O::LeadershipNoConfirm, "leadership_no_confirm", "Options", "LeadershipNoConfirm", 0, 0, 1},
    {O::LeftClickTargetHelp, "target_click_help", "Defaults", "LeftClickTargetHelp", 1, 0, 1, true},
    {O::AllowAutoDuck, "allow_auto_duck", "Defaults", "AllowAutoDuck", 1, 0, 1, true},
    {O::AllowAutoStand, "allow_auto_stand", "Defaults", "AllowAutoStand", 1, 0, 1, true},
    {O::AutojoinHelp, "autojoin_help", "Defaults", "AutojoinHelp", 1, 0, 1, true},
    {O::AcceptKickRequests, "accept_kick_requests", "Defaults", "AcceptKickRequests", 0, 0, 1},
    {O::TradeskillCtrlClick, "tradeskill_ctrl_click", "TradeskillWnd", "CtrlBypass", 1, 0, 1, true},
    {O::ContextMenus, "context_menus", "Main", "ContextMenus", 1, 0, 1, true},
    {O::UseTellWindows, "use_tell_windows", "Main", "UseTellWindows", 0, 0, 1},
    {O::ShowTargetRing, "show_target_ring", "Options", "Show3dTargetIndicator", 1, 0, 1, true},
    {O::ShowBuffTimer, "show_buff_timer", "Defaults", "ShowBuffTimer", 1, 0, 1, true},
    {O::ShowBuffCounters, "show_buff_counters", "Defaults", "ShowBuffCounters", 1, 0, 1, true},
    {O::ShowBuffLimitUse, "show_buff_limit_use", "Defaults", "ShowBuffLimitUse", 1, 0, 1, true},
    {O::ShowSongTimer, "show_song_timer", "Defaults", "ShowSongTimer", 1, 0, 1, true},
    {O::ShowSongCounters, "show_song_counters", "Defaults", "ShowSongCounters", 1, 0, 1, true},
    {O::ShowSongLimitUse, "show_song_limit_use", "Defaults", "ShowSongLimitUse", 1, 0, 1, true},
    {O::LookSpring, "look_spring", "Options", "LookSpring", 0, 0, 1},
    {O::MouseLook, "mouse_look", "Options", "MouseActive", 1, 0, 1},
    {O::MouseTurnZoom, "mouse_turn_zoom", "Options", "MouseTurnZoom", 1, 0, 1},
    {O::MouseRightHanded, "mouse_right_handed", "Options", "MouseRightHanded", 1, 0, 1, true},
    {O::MailForwarding, "mail_forwarding", "MailOptions", "ForwardingToEmailAddressInStationAccount", 0, 0, 1},
    {O::NewMailNotify, "new_mail_notify", "MailOptions", "NewMailNotificationInChatWindow", 0, 0, 1},
    {O::NewMailSound, "new_mail_sound", "MailOptions", "NewMailAudioNotification", 1, 0, 1},
    {O::AddressBookConfirmDelete, "address_book_confirm_delete", "MailOptions", "ConfirmDeletionFromAddressBook", 1, 0, 1},
    // CEverQuest's constructor @0x5338a0 reads these: Log missing is FALSE; the tracking ones missing are TRUE (sort
    // NORMAL, filter 0); InvWinLabels missing TRUE.
    {O::Log, "log", "Defaults", "Log", 0, 0, 1, false, ClientOptionInfo::Text::TrueFalse},
    {O::TrackPlayers, "track_players", "Defaults", "TrackPlayers", 1, 0, 2, false, ClientOptionInfo::Text::TrackPlayers},
    {O::TrackPets, "track_pets", "Defaults", "TrackPets", 1, 0, 1, false, ClientOptionInfo::Text::TrueFalse},
    {O::TrackMercs, "track_mercs", "Defaults", "TrackMercs", 1, 0, 1, false, ClientOptionInfo::Text::TrueFalse},
    {O::TrackSortType, "track_sort", "Defaults", "TrackSortType", 0, 0, 6, false, ClientOptionInfo::Text::TrackSort},
    {O::TrackFilterType, "track_filter", "Defaults", "TrackFilterType", 0, 0, 0x7fffffff},
    {O::TrackAutoUpdate, "track_auto_update", "Defaults", "TrackAutoUpdate", 0, 0, 1, false,
     ClientOptionInfo::Text::TrueFalse},
    {O::InvWinLabels, "inv_win_labels", "Defaults", "InvWinLabels", 1, 0, 1, false, ClientOptionInfo::Text::TrueFalse},
    {O::TargetGroupBuff, "target_group_buff", "Defaults", "TargetGroupBuff", kTargetGroupBuffUnset, kTargetGroupBuffUnset,
     0x7fffffff},
    {O::HideCorpses, "hide_corpses", "Hailstone", "HideCorpses", static_cast<int>(HideCorpseMode::None),
     static_cast<int>(HideCorpseMode::None), static_cast<int>(HideCorpseMode::Always)},
}};

const ClientOptionInfo& client_option_info(ClientOption id) { return kClientOptions[static_cast<std::size_t>(id)]; }

std::optional<ClientOption> find_client_option(std::string_view name) {
    for (const ClientOptionInfo& o : kClientOptions)
        if (name == o.name) return o.id;
    return std::nullopt;
}

ClientOptions::ClientOptions() {
    for (const ClientOptionInfo& o : kClientOptions) values_[static_cast<std::size_t>(o.id)] = o.def;
}

int ClientOptions::set(ClientOption id, int value) {
    const ClientOptionInfo& o = client_option_info(id);
    if (id == O::HideCorpses && (value < o.min || value > o.max)) value = o.def;
    return values_[static_cast<std::size_t>(id)] = std::clamp(value, o.min, o.max);
}

void ClientOptions::load(const IniFile& ini) {
    for (const ClientOptionInfo& o : kClientOptions) {
        // LeadershipNoConfirm is saved (GeneralPageWndNotification) but loadOptions never reads it back: every
        // session starts with the confirmation.
        if (o.id == O::LeadershipNoConfirm) {
            set(o.id, o.def);
            continue;
        }
        int v = o.ini_bool ? (ini_bool_value(ini.get(o.section, o.key), o.def != 0) ? 1 : 0)
                           : ini.get_int(o.section, o.key, o.def);
        if (o.text != ClientOptionInfo::Text::None) {
            auto text = ini.get(o.section, o.key);
            bool missing = !text || (!text->empty() && (*text)[0] == '*'); // GetEQClientInt's "*NULL*"
            if (o.text == ClientOptionInfo::Text::TrackPlayers) v = track_players_value(text);
            else if (o.text == ClientOptionInfo::Text::TrackSort) v = static_cast<int>(track_sort_value(text));
            else if (missing) v = o.def;
            else v = text->empty() ? 0 : ((*text)[0] == 'T' || (*text)[0] == 't' || (*text)[0] == '1') ? 1 : 0;
        } else if (o.id == O::TargetGroupBuff) {
            // DoMainLoop @0x53d964: "*NULL*" (missing) or any text starting with '*' is not sent; else atoi.
            auto text = ini.get(o.section, o.key);
            v = !text || (!text->empty() && (*text)[0] == '*') ? kTargetGroupBuffUnset : core::msvc_atoi(*text);
        } else if (o.id == O::AcceptKickRequests) {
            // The startup's AcceptKickRequests (unk_InitCameras @0x5338a0): 'T', 't' or '1' true, anything else and a
            // missing key false.
            auto text = ini.get(o.section, o.key);
            char c = text && !text->empty() ? (*text)[0] : '\0';
            v = c == 'T' || c == 't' || c == '1' ? 1 : 0;
        } else if (o.id == O::Anonymous) v &= 1;
        else if (o.id == O::Roleplaying) v = (v >> 1) & 1;
        else if (o.id == O::FastDrop && v != 0 && v != 1) v = -1; // /fastdrop never
        else if (o.id == O::LootNoDrop && (v < 0 || v > 2)) v = 0; // CLootWnd / Cmd_lootnodrop: out of range is 0
        set(o.id, v);
    }
}

void ClientOptions::store(ClientOption id, IniFile& ini) const {
    const ClientOptionInfo& o = client_option_info(id);
    int v = get(id);
    if (id == O::Anonymous || id == O::Roleplaying) v = get(O::Anonymous) | (get(O::Roleplaying) << 1);
    switch (o.text) {
    case ClientOptionInfo::Text::TrueFalse: ini.set(o.section, o.key, v ? "TRUE" : "FALSE"); return;
    case ClientOptionInfo::Text::TrackPlayers: ini.set(o.section, o.key, v == 2 ? "GROUP" : v ? "TRUE" : "FALSE"); return;
    case ClientOptionInfo::Text::TrackSort: {
        static constexpr const char* kSorts[] = {"NORMAL", "ALPHA", "DISTANCE", "CONSIDER", "RALPHA", "RDISTANCE", "RCONSIDER"};
        ini.set(o.section, o.key, kSorts[std::clamp(v, 0, 6)]);
        return;
    }
    case ClientOptionInfo::Text::None: break;
    }
    ini.set_int(o.section, o.key, v);
}

} // namespace hailstone::game
