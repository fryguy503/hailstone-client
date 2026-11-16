// ClientApp: the original's client commands that act on the app's windows and files rather than the connection:
// /channel (the active chat window's default channel), /url and /www (the web
// browser), /outputfile (the inventory, spell book, raid and guild rosters and the guild bank written to files in the
// character's folder). GameConnection::run_slash_command hands them here through client_command.

#include <ctime>
#include <fstream>

#include "app/client_app_internal.h"
#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "core/character_files.h"
#include "core/log.h"
#include "core/server_profile.h"
#include "core/web_url.h"
#include "game/inventory.h"
#include "game/option_commands.h"
#include "game/output_file.h"
#include "game/slash_commands.h"
#include "game/spells.h"
#include "platform/open_folder.h"
#include "plugin/policy.h"
#include "web/page_rules.h"

namespace hailstone::app {

namespace client_internal {

namespace {

// Cmd_channel's channels (kChannels): the eqstr name, the exe's short name (none: the name alone), the original's
// channel number and the eqstr built around the second word for the two that take one.
struct ChannelName {
    std::uint32_t name;
    const char* short_name;
    std::uint8_t number;
    std::uint32_t named;
};
constexpr ChannelName kChannels[] = {
    {13070, "gu", 0, 0},           // GuildSay
    {13071, "g", 2, 0},            // gsay
    {5111, "rs", 0xf, 0},          // rsay
    {13072, "t", 7, 13073},        // tell: "Tell to %1"
    {13074, "auc", 4, 0},          // Auction
    {13075, nullptr, 5, 0},        // OOC
    {13076, nullptr, 8, 0},        // Say
    {13077, nullptr, 0x14, 13078}, // chat: "Chat to Channel %1"
};

} // namespace

bool ClientApp::client_info_command(std::string_view h, const std::string& args) {
    net::GameConnection& c = *conn_;
    if (h == "Cmd_channel") {
        // Cmd_channel @0x4f2ae0: "<channel> [name]": the channel matches an eqstr name or a short name (stricmp);
        // anything else, or nothing, prints the usage 13080..13083. Then 13079 with the channel's name (the tell's
        // and chat channel's built around the name) and the active chat window's channel set: a tell keeps the name,
        // a chat channel its index in the joined list. Colour 0x111.
        const ChannelName* ch = nullptr;
        std::string_view rest = args;
        std::string word = game::next_cmd_arg(rest);
        std::string name = game::next_cmd_arg(rest).substr(0, 0x1f);
        if (!args.empty())
            for (const auto& e : kChannels)
                if (game::same_no_case(c.strings().format(e.name, {}), word) ||
                    (e.short_name && game::same_no_case(e.short_name, word))) {
                    ch = &e;
                    break;
                }
        if (!ch) {
            for (std::uint32_t id = 13080; id <= 13083; ++id) c.show_eqstr(id, {}, 0x111);
            return true;
        }
        std::string label = ch->named ? c.strings().format(ch->named, std::vector<std::string>{name}) : c.strings().format(ch->name, {});
        c.show_eqstr(13079, {label}, 0x111);
        if (ch->number == 7) {
            set_active_chat_default(7, name);
        } else if (ch->number == 0x14) {
            // 0x7efa20: the channel's index in the chat service's list (stricmp), -1 when not joined.
            int index = -1;
            auto chans = c.chat_channels();
            for (std::size_t i = 0; i < chans.size(); ++i)
                if (game::same_no_case(chans[i], name)) {
                    index = static_cast<int>(i);
                    break;
                }
            set_active_chat_default(0x14, name, index);
        } else {
            set_active_chat_default(ch->number);
        }
        return true;
    }
    if (h == "Cmd_url" || h == "Cmd_www") {
        // Cmd_url @0x478520 / Cmd_www @0x478420: no argument: the usage (4096 / 4094); else 4095 "Opening %1" with the
        // address (/www: "http://%s") and ShellExecuteA "open" on it (here SDL_OpenURL, the system's browser). Both
        // through DisplayChatWithLineBreaks.
        bool www = h == "Cmd_www";
        if (args.empty()) {
            c.show_with_line_breaks(c.strings().format(www ? 4094 : 4096, {}));
            return true;
        }
        std::string url = www ? "http://" + args : args;
        if (!plugin::caller_name().empty()) {
            // A script or plugin (the command filter has checked its 'web' permission): http(s) pages only, on the
            // hosts the server's [web] allow names, never this computer or a private network (web/page_rules.h: the
            // system's browser has the player's own sign-ins there). The player's own /url opens whatever the system
            // opens, as ShellExecuteA does.
            static const std::vector<std::string> kAny;
            const std::vector<std::string>& allow = c.server_rules() ? c.server_rules()->web_allow : kAny;
            if (!web::script_url_allowed(url, allow)) {
                c.ui_message(std::format("[MQ] {}: {} is not a web page this server lets scripts open",
                                         plugin::caller_name(), url));
                return true;
            }
        }
        c.show_with_line_breaks(c.strings().format(4095, std::vector<std::string>{url}));
        platform::open_url(url);
        return true;
    }
    if (h == "Cmd_outputfile") {
        outputfile_command(args);
        return true;
    }
    return false;
}

void ClientApp::outputfile_command(const std::string& args) {
    // Cmd_outputfile @0x4febd0: no argument: 3487 (colour 0). The first word picks the writer, the second is the file
    // (here a name in the character's folder; none: the writer's default name). The original fopens the name as it is
    // given, in the game folder; Hailstone writes only in the character's folder and refuses the names of the files
    // it keeps there (a Hailstone difference: the original lets the dump replace its own ini files).
    net::GameConnection& c = *conn_;
    if (args.empty()) return c.show_eqstr(game::kStrOutputFileUsage);
    std::string_view rest = args;
    std::string what = game::next_cmd_arg(rest).substr(0, 0x31);
    std::string file = game::next_cmd_arg(rest).substr(0, 0xff);
    if (!file.empty() && is_kept_character_file(sanitize_file_name(file))) {
        c.ui_message(std::format("/outputfile: {} holds this character's settings; give the file another name.",
                                 sanitize_file_name(file)));
        return;
    }
    CharacterFiles files = c.own_files();
    auto write = [&](const std::string& name, const std::string& text) {
        if (files.empty()) return;
        std::filesystem::path path = files.dir / sanitize_file_name(name);
        std::ofstream out(path, std::ios::binary);
        if (out) out << text;
        else log::warn("/outputfile: cannot write {}", path.string());
    };
    auto date = [] {
        std::time_t t = std::time(nullptr);
        return game::output_file_date(*std::localtime(&t));
    };
    const std::string me = c.character_name();
    switch (game::output_file_kind(what)) {
    case game::OutputFileKind::Guild: {
        // CGuildMgmtWnd::DumpToFile @0x681430 (separator "\t"): only in a guild; the default name is
        // "<guild>-<date>.txt"; 3188 names the file; then the member list's rows with the tribute and notes lists'.
        // The column counts are the lists' (EQUI_GuildManagementWnd.xml: 10, 5, 3).
        const auto& g = c.guild();
        if (!g.in_guild()) return;
        if (file.empty()) file = g.name_of(g.guild_id) + "-" + date() + ".txt";
        c.show_eqstr(3188, {file});
        if (!ui_api_) return;
        game::DumpList members{ui_api_->list_rows("GuildManagementWnd", "MemberList"), 10};
        game::DumpList tribute{ui_api_->list_rows("GuildManagementWnd", "TributeList"), 5};
        game::DumpList notes{ui_api_->list_rows("GuildManagementWnd", "NoteList"), 3};
        write(file, game::guild_roster_file(members, tribute, notes));
        return;
    }
    case game::OutputFileKind::Raid: {
        // CRaidWnd_DumpRosterToFile @0x72c320; a member's note from the notes list (CRaidWnd +0x2c0: OP_RaidUpdate
        // 0x24's notes).
        if (!ui_api_) return;
        if (file.empty()) file = "RaidRoster-" + date() + ".txt";
        auto note = [&c](const std::string& name) {
            for (const auto& m : c.raid().members)
                if (m.name == name) return m.note;
            return std::string();
        };
        write(file, game::raid_roster_file(ui_api_->list_rows("RaidWindow", "RAID_PlayerList"),
                                           ui_api_->list_rows("RaidWindow", "RAID_NotInGroupPlayerList"),
                                           c.strings().format(6699, {}), note));
        return;
    }
    case game::OutputFileKind::Spellbook: {
        // The profile's book (PcProfile +0x2520), as scribing keeps it.
        const game::Spawn* player = c.world().player();
        if (!player) return;
        if (file.empty()) file = me + "-Spellbook.txt";
        std::uint8_t cls = player->class_id;
        write(file, game::spellbook_file(
                        c.spell_state().book, [&](std::uint32_t id) { return c.spell_table().get(id); },
                        [&](const game::SpellData& s) { return c.spell_level_needed(s, cls); }));
        return;
    }
    case game::OutputFileKind::Inventory:
        if (file.empty()) file = me + "-Inventory.txt";
        write(file, game::inventory_file(c.inventory(), c.strings()));
        return;
    case game::OutputFileKind::GuildBank:
        // EQWorldData_IsGuildHallZone and the guild bank window, else 6284; OutputFile_WriteGuildRoster @0x67e7a0
        // writes only when the bank is the player's guild's (0x67e560: GameConnection::guild_bank_allowed).
        if (!c.in_guild_hall_zone()) return c.show_eqstr(6284);
        if (!c.guild_bank_allowed()) return;
        if (file.empty()) file = me + "-GuildBank.txt";
        write(file, game::guild_bank_file(c.guild_bank().main, c.guild_bank().deposit, c.strings()));
        return;
    case game::OutputFileKind::RealEstate:
        // OutputFile_WriteRealEstate @0x5b37d0: the header and the real estate's items. The rows (0x5b3600) are not
        // traced, so only the header is written (docs/unknowns.md).
        if (file.empty()) file = me + "-RealEstate.txt";
        write(file, game::real_estate_file());
        return;
    case game::OutputFileKind::GuildHall:
        // Cmd_outputfile's guild hall: a guild (5438), standing in our guild's hall plot (5439: the real estate
        // manager's current estate), permission to modify its items (5440). The file itself is not traced: 5439
        // always (docs/unknowns.md).
        if (!c.guild().in_guild()) return c.show_eqstr(5438);
        return c.show_eqstr(5439);
    case game::OutputFileKind::None:
        return;
    }
}

} // namespace client_internal

} // namespace hailstone::app
