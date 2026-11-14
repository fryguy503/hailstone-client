// GameConnection: the typed-line interpreter, chat, and the social, guild,
// raid and chat-server commands. What the client does and says follows the
// original's handlers (addresses at each function;
// strings are eqstr ids from eqstr_us.txt); packets are net/social_packets.h
// and net/chat_server.h, state game/social.h.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <format>

#include "core/log.h"
#include "game/coords.h"
#include "game/eq_names.h"
#include "game/item_options.h"
#include "game/leadership.h"
#include "game/option_commands.h"
#include "game/mechanics/attack_rules.h"
#include "game/mechanics/character.h"
#include "game/slash_commands.h"
#include "game/zone_names.h"
#include "net/chat_server.h"
#include "net/group_packets.h"
#include "net/game_connection.h"
#include "plugin/hooks.h"
#include "net/notice_packets.h"
#include "core/game_dir.h"
#include "core/ini_file.h"
#include "net/opcodes.h"
#include "net/packet_io.h"
#include "net/social_packets.h"

namespace hailstone::net {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::string base_name(std::string n) {
    while (!n.empty() && std::isdigit(static_cast<unsigned char>(n.back()))) n.pop_back();
    return n;
}

// "short.Name" (UCS senders and ignore entries) -> "Name".
std::string after_last_dot(const std::string& s) {
    auto dot = s.rfind('.');
    return dot == std::string::npos ? s : s.substr(dot + 1);
}

ChatEvent::Kind kind_of_channel(std::uint32_t channel) {
    using K = ChatEvent::Kind;
    switch (channel) {
    case 0: case 0x11: return K::Guild;
    case 2: return K::Group;
    case 3: return K::Shout;
    case 4: return K::Auction;
    case 5: return K::OOC;
    case 7: case 0xe: return K::Tell;
    case 8: return K::Say;
    case 15: return K::Raid;
    case 22: return K::Emote;
    default: return K::Other;
    }
}

// The user colour a channel's lines take (eqlib USERCOLOR_*): the chat windows' filters route by it.
std::uint32_t color_of_channel(std::uint32_t channel) {
    switch (channel) {
    case 0: case 0x11: return 259; // guild
    case 2: return 258;            // group
    case 3: return 262;            // shout
    case 4: return 261;            // auction
    case 5: return 260;            // ooc
    case 7: case 0xe: return 257;  // tell
    case 8: return 256;            // say
    case 15: return 327;           // raid
    case 21: return 342;           // fellowship
    case 22: return 263;           // emote
    default: return channel >= 0x100 ? channel : 0;
    }
}

using game::language_eqstr;

std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string fmt_eq(const game::EqStrings& s, std::uint32_t id, std::vector<std::string> args = {}) { return s.format(id, args); }

// Guild invitations lapse like the original's confirmation dialog (10 s, @0x4b9809).
constexpr std::int64_t kGuildInviteMs = 10000;

} // namespace

// ---------------------------------------------------------------- helpers

void GameConnection::system(std::uint32_t eqstr, std::vector<std::string> args, std::uint32_t color) {
    chat(ChatEvent::Kind::System, fmt_eq(strings_, eqstr, std::move(args)), 0, color);
}

std::string GameConnection::target_name_or(const std::string& args) const {
    std::string_view rest = args;
    std::string word = game::next_cmd_arg(rest);
    if (!word.empty()) return word;
    auto it = world_.spawns.find(world_.target_id);
    return it == world_.spawns.end() ? std::string() : base_name(it->second.name);
}

bool GameConnection::other_deity_team(const game::Spawn& s) const {
    if (pvp_rules_ != 4) return false;
    const game::Spawn* me = world_.player();
    game::mechanics::AttackSpawn a, b;
    a.deity = me ? me->deity : (profile_ ? profile_->deity : 0);
    b.deity = s.deity;
    return game::mechanics::not_on_same_deity(&a, &b, false, s.gm);
}

void GameConnection::reset_social_for_zone() {
    social_.follow_id = 0;
    raid_.inviter.clear();
}

// ---------------------------------------------------------------- the interpreter

void GameConnection::say(const std::string& text) { send_command(text); }

void GameConnection::send_command(const std::string& typed) {
    std::string line = typed;
    // Plugin commands and line filters first (plugin/hooks.h): they can take any line, the client's commands too.
    if (options_.hooks && options_.hooks->command(line)) return;
    if (!zone_session_ || line.empty()) return;
    // InterpretCmd @0x51fce0: with the interface locked (/safelock) only /safelock gets through.
    if (interface_locked() && !game::locked_line_allowed(line, fmt_eq(strings_, 14193))) return system(game::kStrInterfaceLocked);
    bool gm = profile_ && profile_->gm;
    game::CommandLine cl = game::parse_command_line(line, gm, &strings_);
    switch (cl.kind) {
    case game::CommandLine::Kind::Empty:
        return;
    case game::CommandLine::Kind::ChatServer:
        chat_server_command(cl.text);
        return;
    case game::CommandLine::Kind::Invalid: {
        // Client-side commands this client adds (not in the original's table).
        std::string word = cl.word;
        for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string_view rest = line;
        game::next_cmd_arg(rest);
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        if (group_command(word, std::string(rest)) || item_command(word, std::string(rest))) return;
        // Hailstone's own windows (the client's handlers, by a name not in the original's table).
        if (word == "/abilitybook" && client_commands_ && client_commands_("Hailstone_abilitybook", std::string(rest)))
            return;
        if (word == "/camera" && client_commands_ && client_commands_("Hailstone_camera", std::string(rest))) return;
        if ((word == "/finditem" || word == "/advi") && client_commands_ &&
            client_commands_("Hailstone_finditem", std::string(rest)))
            return;
        system(12380); // That is not a valid command.  Please use /help.
        return;
    }
    case game::CommandLine::Kind::Text:
        // InterpretCmd's default channel: the chat window's (say here).
        say_to_channel(8, cl.text);
        return;
    case game::CommandLine::Kind::Command:
        break;
    }
    const auto& cmd = game::slash_commands()[cl.index];
    // InterpretCmd @0x51fce0: most commands cannot be used while dead.
    if (pstatus_.dead && !(cmd.flags & 1)) {
        system(538); // You may not use that command while dead.
        return;
    }
    if (!run_slash_command(cl.index, cl.args))
        chat(ChatEvent::Kind::System, std::format("{} is not supported by this client yet.", cmd.name));
}

bool GameConnection::run_slash_command(std::size_t index, const std::string& args) {
    const auto& cmd = game::slash_commands()[index];
    const std::string_view h = cmd.handler;
    const std::string name = cmd.name;
    std::string_view rest = args;

    // ---- chat
    if (h == "Cmd_say") { say_command(args); return true; }
    if (h == "Cmd_gsay") { say_to_channel(2, args); return true; }
    if (h == "Cmd_shout") { say_to_channel(3, args); return true; }
    if (h == "Cmd_auction") { say_to_channel(4, args); return true; }
    if (h == "Cmd_ooc") { say_to_channel(5, args); return true; }
    if (h == "Cmd_guildsay") { say_to_channel(0, args); return true; }
    if (h == "Cmd_rsay") { say_to_channel(15, args); return true; }
    if (h == "Cmd_fsay") { say_to_channel(21, args); return true; }
    if (h == "Cmd_tell") {
        // Cmd_tell @0x4e1dc0: the first word is the recipient.
        std::string to = game::next_cmd_arg(rest);
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        send_tell(to, std::string(rest));
        return true;
    }
    if (h == "Cmd_ttell") {
        // Cmd_ttell @0x4e1e00: tells the target.
        auto it = world_.spawns.find(world_.target_id);
        if (args.empty() || it == world_.spawns.end()) return true;
        std::string to = it->second.name;
        if (!it->second.is_corpse && (to.find('#') != std::string::npos || to.find('_') != std::string::npos)) {
            system(13090);
            return true;
        }
        send_tell(base_name(to.substr(0, to.find('\''))), args);
        return true;
    }
    if (h == "Cmd_reply") { reply_command(args); return true; }
    if (h == "Cmd_emote") { emote_command(args); return true; }
    if (h == "Cmd_rtarget") {
        // Cmd_rtarget @0x4ead00: targets the argument, else the last tell's sender.
        if (!args.empty()) target_by_name(args);
        else if (social_.last_tell_from.empty()) system(13286);
        else target_by_name(after_last_dot(social_.last_tell_from));
        return true;
    }

    // ---- information
    if (h == "Cmd_who") { who_command(args); return true; }
    if (h == "Cmd_whotarget") {
        auto it = world_.spawns.find(world_.target_id);
        if (it != world_.spawns.end()) who_command(base_name(it->second.name));
        return true;
    }
    if (h == "Cmd_location") { location_command(); return true; }
    if (h == "Cmd_time") { time_command(); return true; }
    if (h == "Cmd_played") { played_command(); return true; }
    if (h == "Cmd_random") { random_command(args); return true; }
    if (h == "Cmd_consider") { consider(); return true; }
    if (h == "Cmd_yell") {
        // Cmd_yell @0x4e1cc0: eqstr 13087 (colour 0xd) and an empty OP_YellForHelp.
        system(13087, {}, 0xd);
        send_zone(make_empty(Op::YellForHelp));
        return true;
    }
    if (h == "Cmd_split") {
        // CEverQuest::Split @0x52b660: /split <platinum> <gold> <silver> <copper>.
        if (!group_.active) return system(12328), true;
        // The amounts are the original's 32-bit longs, and their sum is the 32-bit add it does (wrapping).
        std::int32_t v[4];
        for (int i = 0; i < 3; ++i) v[i] = game::cmd_atol(game::next_cmd_arg(rest));
        v[3] = game::cmd_atol(rest);
        std::uint32_t sum = 0;
        for (std::int32_t x : v) sum += static_cast<std::uint32_t>(x);
        if (static_cast<std::int32_t>(sum) < 1) {
            system(12329);
            system(12330);
            system(12331);
            return true;
        }
        const std::uint32_t have[4] = {coins_.platinum, coins_.gold, coins_.silver, coins_.copper};
        for (int i = 0; i < 4; ++i)
            if (v[i] > static_cast<std::int32_t>(have[i])) return system(12332 + static_cast<std::uint32_t>(i)), true;
        ByteWriter w;
        for (std::int32_t x : v) w.le<std::uint32_t>(static_cast<std::uint32_t>(std::max(0, x)));
        send_zone(make_packet(Op::Split, std::move(w)));
        return true;
    }
    if (h == "Cmd_motd") {
        // Cmd_motd @0x4f1290: the guild message of the day.
        system(12965, {guild_.motd});
        return true;
    }

    // ---- status
    if (h == "Cmd_anonymous") { anonymous_command(args, false); return true; }
    if (h == "Cmd_fastdrop") { fastdrop_command(args); return true; }
    if (h == "Cmd_lootnodrop") { lootnodrop_command(args); return true; }
    if (h == "Cmd_roleplay") { anonymous_command(args, true); return true; }
    if (h == "Cmd_afk") { afk_command(args); return true; }
    if (h == "Cmd_lfgroup") { lfg_command(args); return true; }
    if (h == "Cmd_consent" || h == "Cmd_deny") {
        // Cmd_consent @0x4e7330 / Cmd_deny @0x4e7480.
        std::string who = game::next_cmd_arg(rest);
        if (who.empty()) return true;
        if (h == "Cmd_consent") system(9507); // To remove consent, use /deny <playername>
        send_zone(encode_consent(h == "Cmd_deny", who));
        return true;
    }

    // ---- targeting and movement
    if (h == "Cmd_assist") { assist_command(args); return true; }
    if (h == "Cmd_follow") { follow_command(); return true; }
    if (h == "Send_OP_TargetCommand_004e9790") { target_by_name(args); return true; }

    // ---- guilds and raids
    if ((h.find("uild") != std::string_view::npos && h != "Cmd_lfguild") || h == "Send_OP_SetGuildMOTD") {
        guild_command(h, args);
        return true;
    }
    if (h == "Cmd_raidinvite") { raid_invite(args); return true; }
    if (h == "Cmd_raidaccept") { answer_raid_invite(true); return true; }
    if (h == "Cmd_raiddecline") { answer_raid_invite(false); return true; }
    if (h == "Cmd_raiddisband") { raid_leave(); return true; }
    if (h == "Cmd_raidwindow") { window_toggle_ = "raid"; return true; }
    if (h == "Cmd_setloottype") { raid_set_loot_type(args); return true; } // CRaid::SetLootType(args)
    if (h == "Cmd_addraidlooter") { raid_add_looter(); return true; }       // CRaid::AddRaidLooter, a tail call
    if (h == "Cmd_removeraidlooter") { raid_remove_looter(); return true; } // CRaid::RemoveRaidLooter
    if (h == "Cmd_makeraidleader") {
        // Cmd_makeraidleader @0x4de3e0: not in a locked raid (0x22a5, colour 0xd); no argument hands the raid to the
        // target, else to the argument with its first character capitalized (63 characters).
        if (raid_.locked) { system(8869, {}, 0xd); return true; }
        if (args.empty()) {
            auto t = world_.spawns.find(world_.target_id);
            if (t != world_.spawns.end()) { raid_send_leadership_change(base_name(t->second.name)); return true; }
        }
        std::string who = args.substr(0, 63);
        if (!who.empty()) who[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(who[0])));
        raid_send_leadership_change(who);
        return true;
    }

    // ---- the chat server
    if (h == "Cmd_join") {
        // Cmd_join @0x4ebe70: "join %s" through ChatServer_SendCommandText; without a channel the usage (the
        // exe's own strings, colour 0x111).
        if (args.empty()) {
            for (const char* line : {"Usage: /join <channel name>", "   or  /join <channel name>:<password>",
                                     "You can also specify multiple channels using commas (e.g. /join <channel "
                                     "name>,<channel name>)."})
                chat(ChatEvent::Kind::System, line, 0, 0x111);
        } else {
            chat_server_command("join " + args);
        }
        return true;
    }
    // Cmd_leave @0x4ebf10, Cmd_leaveall @0x4ebf80, Cmd_list @0x4ebfa0: ";leave %s" / ";leave", ";leaveall",
    // ";list %s" / ";list" straight to the chat server (SendChatServerMessage, viaWorld 0).
    if (h == "Cmd_leave") { send_chat_server_message(args.empty() ? ";leave" : ";leave " + args, false); return true; }
    if (h == "Cmd_leaveall") { send_chat_server_message(";leaveall", false); return true; }
    if (h == "Cmd_list") { send_chat_server_message(args.empty() ? ";list" : ";list " + args, false); return true; }
    if (h == "Cmd_announce") {
        // Cmd_announce @0x4ec010: "on" / "off" (exact) send ";announce <arg>" and set Announce; no argument sends
        // ";announce" and flips it; anything else does nothing.
        if (args.empty()) {
            send_chat_server_message(";announce", false);
            announce_ = !announce_;
        } else if (args == "on" || args == "off") {
            send_chat_server_message(";announce " + args, false);
            announce_ = args != "off";
        }
        return true;
    }
    if (h == "Cmd_chat") { chat_server_command(args); return true; } // Cmd_chat @0x4f2ad0
    if (h.size() >= 5 && h.substr(0, 4) == "Cmd_" && std::isdigit(static_cast<unsigned char>(h[4]))) {
        numbered_channel(std::atoi(std::string(h.substr(4)).c_str()), args);
        return true;
    }
    if (h == "Cmd_friends") { friends_command(args); return true; }
    if (h == "Cmd_ignore") { ignore_command(args); return true; }

    // ---- the social emotes (/smile, /wave, ...)
    if (const auto* e = game::social_for_handler(h)) {
        social_emote(*e, args);
        return true;
    }

    // ---- commands handled elsewhere in the connection
    if (h == "Cmd_camp") { camp_command(args); return true; }
    if (h == "Cmd_attack") { attack_command(args); return true; }
    if (h == "Cmd_sit") {
        // Cmd_sit @0x4e8620: "off" stands a sitting player; anything else sits.
        if (args == fmt_eq(strings_, 12946, {})) { if (sitting()) set_sitting(false); }
        else set_sitting(true);
        return true;
    }
    if (h == "Cmd_stand") { set_sitting(false); return true; }
    if (h == "Cmd_cast") {
        // Cmd_cast @0x4e8900: /cast 1..12 (the bard twist cleared first).
        int gem = std::atoi(args.c_str());
        if (gem < 1 || gem > game::kSpellGems) system(13255);
        else {
            clear_bard_twist();
            cast_gem(gem - 1);
        }
        return true;
    }
    if (h == "Cmd_stopcast") { stop_cast(); return true; }
    if (h == "Cmd_discipline") { discipline_command(args); return true; }
    if (h == "Cmd_stopdisc") { stop_discipline(); return true; }
    if (h == "cmd_autofire") { autofire_command(); return true; }
    if (h == "Cmd_melody") { melody_command(args); return true; }
    if (h == "Cmd_dismount") { dismount_command(); return true; }
    if (app_commands_ && app_commands_(h, args)) return true; // Cmd_doability (app/actions_ui.h)
    if (h == "Cmd_invite" || h == "Cmd_disband" || h == "Cmd_makeleader" || h == "cmd_pet")
        return group_command(name, args);
    if (h == "Hail") return commerce_command("/hail", args);
    if (h == "__do_loot") return item_command("/loot", args);
    if (h == "cmd_autoinventory") return item_command("/autoinventory", args);
    if (option_command(h, args)) return true;
    if (client_info_command(h, args)) return true;
    if (progress_command(h, args)) return true;
    if (dz_command(h, args)) return true;
    if (leadership_command(h, args)) return true;
    if (merc_command(h, args)) return true;
    if (player_command(h, args)) return true;
    if (market_command(h, args)) return true;
    // The commands the app implements (app/client_app_commands.cpp).
    if (client_commands_ && client_commands_(h, args)) return true;
    return false;
}

// ---------------------------------------------------------------- chat out

void GameConnection::send_channel(std::uint32_t channel, const std::string& target, const std::string& text) {
    // CEverQuest::SendNewText @0x51dcd0.
    ChannelMessage m;
    m.sender = character_;
    m.target = target;
    // The chat window's language (Cmd_language) and the player's skill in it, less a quarter of the
    // drunkenness, at least 0.
    std::uint32_t lang = chat_language_ < 32 ? chat_language_ : 0;
    std::int32_t skill = profile_ && lang < profile_->languages.size() ? profile_->languages[lang] : (lang == 0 ? 100 : 0);
    skill -= drunkenness_ >> 2;
    m.language = lang;
    m.language_skill = static_cast<std::uint32_t>(std::max(0, skill));
    m.channel = channel;
    m.message = text;
    send_zone(encode(m));
}

void GameConnection::say_to_channel(std::uint32_t channel, const std::string& text) {
    if (text.empty()) return;
    // send_chat, send_gsay, send_shout, send_auction, send_ooc, GuildSay and CRaid::SendRaidChat send nothing
    // while the chat block (0xddf70d) is set.
    if (chat_blocked_) return;
    switch (channel) {
    case 2:
        // send_gsay @0x51f690.
        if (!group_.active) return system(12345); // You are not in a group. Talking to yourself again???
        break;
    case 0:
        // CEverQuest::GuildSay @0x524240.
        if (!guild_.in_guild()) return system(12242); // You are not a member of any guild.
        break;
    case 15:
        // CRaid::SendRaidChat @0x47c560.
        if (!raid_.active) return system(5108);
        break;
    case 21:
        // FellowshipSay @0x524330: without a fellowship, eqstr 4573 in colour 342.
        if (!fellowship_.active()) return system(4573, {}, 342); // You are not a member of a fellowship.
        break;
    default:
        break;
    }
    // The original sends the text, then echoes it in its own colour; the
    // server's copy of our own line is dropped on arrival (msg_new_text).
    send_channel(channel, {}, text);
    // the echo colours: 307 say, 309 group, 310 guild, 311 ooc, 312 auction, 313 shout (raid has none: 327)
    std::uint32_t c = color_of_channel(channel);
    std::uint32_t echo_color = c == 256 ? 307 : c >= 258 && c <= 262 ? c + 51 : c;
    if (std::uint32_t echo = game::own_chat_echo(channel))
        chat(kind_of_channel(channel), fmt_eq(strings_, echo, {text}), 0, echo_color);
}

void GameConnection::say_command(const std::string& text) {
    // Cmd_say @0x4fd2d0: to an NPC target (+0x125 == 1) in sight the text is said to it (SendNewText 8 with its
    // name, +0xa4) and shown as eqstr 13086 (colour 0x133) unless the voice is grafted (TotalEffect SPA 0x4b);
    // otherwise send_chat says it. In sight: __CastRay2 from our position (z + 1 for races 8, 11 and 12) to the
    // target's, its z raised by 1.0 for the same races.
    auto t = world_.spawns.find(world_.target_id);
    const game::Spawn* me = world_.player();
    if (t == world_.spawns.end() || !t->second.is_npc || t->second.is_corpse || !me) return say_to_channel(8, text);
    auto raised = [](std::uint16_t race) { return race == 8 || race == 11 || race == 12; };
    glm::vec3 to = t->second.position;
    if (raised(t->second.race)) to.z += 1.0f;
    glm::vec3 from = position_;
    if (raised(me->race)) from.z += 1.0f;
    bool seen = !line_of_sight_ || line_of_sight_(from, to);
    log::debug("say: target {} {}", t->second.name, seen ? "in sight: said to it" : "out of sight: a plain say");
    if (!seen) return say_to_channel(8, text);
    if (text.empty()) return;
    send_channel(8, t->second.name, text);
    std::int32_t graft = 0;
    with_character([&](game::mechanics::Character& ch) { graft = ch.total_effect(0x4b, true, 0, true, true); });
    if (graft == 0) chat(ChatEvent::Kind::Say, fmt_eq(strings_, 13086, {text}), 0, 0x133);
}

void GameConnection::send_tell(const std::string& name, const std::string& text) {
    // CEverQuest::send_tell @0x52b4b0: a tell to ourselves is refused; the
    // server echoes a sent tell (channel 14), which prints "You told ...".
    if (name.empty() || text.empty()) return;
    if (chat_blocked_) return; // @0x52b4c5
    if (iequals(name, character_)) return system(12323); // Talking to yourself again?
    send_channel(7, name, text);
}

void GameConnection::reply_command(const std::string& args) {
    // Cmd_reply @0x4e2080.
    if (social_.last_tell_from.empty()) return system(12364);
    const std::string& to = social_.last_tell_from;
    if (to[0] == ';' || to[0] == '[') {
        // "%c%s %s %s": the prefix, "tell" (eqstr 13974 past its slash), the name as kept, the text; straight to
        // the chat server
        std::string word = fmt_eq(strings_, 13974, {});
        if (!word.empty() && word[0] == '/') word.erase(0, 1);
        send_chat_server_message(std::format("{}{} {} {}", to[0], word, to.substr(1), args), false);
        return;
    }
    send_tell(to, args);
}

void GameConnection::emote_command(const std::string& args) {
    // Cmd_emote @0x4dfc60: refuses empty or overlong emotes, emotes that
    // contain "says" (eqstr 13066) or doubled whitespace (13067, 13068).
    std::string lower = args;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (args.empty() || args.size() > 0x1ff || lower.find(fmt_eq(strings_, 13066, {})) != std::string::npos ||
        args.find("  ") != std::string::npos) {
        system(13067);
        system(13068);
        return;
    }
    std::string text = args;
    if (chat_blocked_) return; // CEverQuest::Emote @0x51f406
    if (std::isalnum(static_cast<unsigned char>(text.back()))) text += '.';
    // CEverQuest::Emote @0x51f400: channel 22, echoed as "<name> <text>" (colour 0x13a).
    send_channel(22, {}, text);
    chat(ChatEvent::Kind::Emote, character_ + " " + text, 0, 0x13a);
}

void GameConnection::social_emote(const game::SocialEmote& e, const std::string& args) {
    // The four standing socials need the player standing still.
    const game::Spawn* me = world_.player();
    if (chat_blocked_) return; // send_social @0x526935: nothing while 0xddf70d is set
    if (e.needs_standing && (pstatus_.stand_state != 100 || moving_)) return system(13069);
    // CEverQuest::send_social @0x526910: the first word names the target
    // ("say" or nothing: the current target); one farther than 100 units is not seen.
    std::string_view rest = args;
    std::string word = game::next_cmd_arg(rest);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    const game::Spawn* target = nullptr;
    if (!word.empty() && !iequals(word, fmt_eq(strings_, 12295))) {
        // PlayerManagerClient::GetPlayerFromPartialName over the spawn list (game/social.h).
        std::vector<std::string> names;
        std::vector<const game::Spawn*> spawns;
        for (const auto& [id, sp] : world_.spawns) {
            names.push_back(sp.name);
            spawns.push_back(&sp);
        }
        auto found = game::player_from_partial_name(word, names);
        if (!found) return system(12296);
        target = spawns[*found];
    } else if (auto it = world_.spawns.find(world_.target_id); it != world_.spawns.end()) {
        target = &it->second;
    }
    if (target && target->id == world_.player_id) target = nullptr;
    if (target && glm::distance(target->position, position_) > 100.0f) return system(12321);
    std::string target_name = target ? game::display_name(target->name) : std::string();
    auto text = game::build_social(strings_, e, target ? &target_name : nullptr, me ? me->gender : 0,
                                   target ? target->gender : 0, std::string(rest));
    if (!text.others.empty()) send_zone(encode_social_emote(text.others));
    // The animation, when standing still (OP_Animation, speed 10).
    if (e.animation >= 0 && pstatus_.stand_state == 100 && !moving_ && world_.player_id) {
        Animation a;
        a.spawn_id = static_cast<std::uint16_t>(world_.player_id);
        a.action = static_cast<std::uint8_t>(e.animation);
        a.speed = 10;
        send_zone(encode(a));
    }
    if (!text.echo.empty()) chat(ChatEvent::Kind::Emote, text.echo);
}

// ---------------------------------------------------------------- chat in

// DisplaySpecialMesg @0x4be4b0 (OP_SpecialMesg): the line by its speak mode (0 the text as it is; 2 shouts 1034 /
// 1420; 3 the text alone 1035; 4 an emote 1036, 7580 when the text starts with ' or @; 5 tells the group 1414; else
// says 1032 / 1410), a language other than common named in it (1411 "in %1", 1412 "in an unknown tongue" below 25 in
// it) and garbled by the listener's skill; type 0x157 is 0x158 when aimed at the player. A line not garbled from a
// named speaker goes to the conversation journal when its journal mode is 2, or 1 and it is aimed at the player
// (CJournalTextWnd's 0x6b3c00, while the journal window exists: take_journal_lines()).
void GameConnection::handle_special_message(const SpecialMessage& m) {
    log::debug("special message: speak mode {} journal mode {} language {} type {} target {} from '{}'", m.speak_mode,
               m.journal_mode, m.language, m.type, m.target_spawn_id, m.sayer);
    std::uint32_t language = m.language > 0x1f ? 0 : m.language;
    std::string text = m.message;
    std::string lang;
    bool garbled = false;
    if (language != 0) {
        std::uint8_t skill = profile_ && language < profile_->languages.size() ? profile_->languages[language] : 0;
        lang = skill < 25 ? fmt_eq(strings_, 1412, {}) : fmt_eq(strings_, 1411, {fmt_eq(strings_, language_eqstr(language), {})});
        text = game::garble_by_language_skill(m.message, skill, garbling_on_, chat_random_, &garbled);
    }
    std::string line;
    switch (m.speak_mode) {
    case 0: line = text; break;
    case 2: line = language == 0 ? fmt_eq(strings_, 1034, {m.sayer, text}) : fmt_eq(strings_, 1420, {m.sayer, lang, text}); break;
    case 3: line = fmt_eq(strings_, 1035, {m.sayer, text}); break;
    case 4:
        line = !text.empty() && (text[0] == '\'' || text[0] == '@') ? fmt_eq(strings_, 7580, {m.sayer, text})
                                                                    : fmt_eq(strings_, 1036, {m.sayer, text});
        break;
    case 5: line = fmt_eq(strings_, 1414, {m.sayer, lang, text}); break;
    default: line = language == 0 ? fmt_eq(strings_, 1032, {m.sayer, text}) : fmt_eq(strings_, 1410, {m.sayer, lang, text}); break;
    }
    const game::Spawn* me = world_.player();
    bool at_me = me && m.target_spawn_id == me->id;
    std::uint32_t type = m.type == 0x157 && at_me ? 0x158 : m.type;
    chat(ChatEvent::Kind::System, line, 0, type);
    if (!garbled && !m.sayer.empty() && (m.journal_mode == 2 || (m.journal_mode == 1 && at_me))) {
        if (journal_lines_.size() < 256)
            journal_lines_.push_back({m.sayer, zone_id(), static_cast<float>(static_cast<std::int32_t>(m.loc[0])),
                                      static_cast<float>(static_cast<std::int32_t>(m.loc[1])),
                                      static_cast<float>(static_cast<std::int32_t>(m.loc[2])), line});
    }
}

void GameConnection::handle_channel_message(const ChannelMessage& m) {
    // msg_new_text @0x4c0080: the spam flag under filter 31 first.
    if (m.spam && chat_filters_.mode(game::kFilterSpam) == 1) return;
    if (iequals(m.sender, character_) && m.channel != 20) {
        // Our own line: the original echoed it when sending. Tells come back from the server as the echo (7 / 14):
        // "You told %1, '%2'" (colour 0x134); to a name ending in a digit (an NPC's) both the name and the text
        // through trimName; else, with tell windows, in the player's tell window (DoTellWindow).
        if (m.channel == 7 || m.channel == 0xe) {
            bool digit = !m.target.empty() && std::isdigit(static_cast<unsigned char>(m.target.back()));
            std::string to = digit ? game::display_name(m.target) : m.target;
            chat(ChatEvent::Kind::Tell, fmt_eq(strings_, 1400, {to, digit ? game::display_name(m.message) : m.message}),
                 0, 308, digit ? std::string() : to);
        }
        return;
    }
    const game::Spawn* me = world_.player();
    const game::Spawn* sender = nullptr; // PlayerManagerClient::GetSpawnByName (exact, ignoring case)
    for (const auto& [id, sp] : world_.spawns)
        if (iequals(sp.name, m.sender)) {
            sender = &sp;
            break;
        }
    game::ChatLineFacts f;
    f.channel = m.channel;
    f.spam = m.spam;
    f.listener_gm = me && me->gm;
    f.guild_permission = has_guild_permission(game::kGuildPermGuildChatSee); // LocalPC_HasGuildPermission(0x13)
    f.sender_gm = sender && sender->gm;
    f.sender_ignored = std::any_of(social_.ignored.begin(), social_.ignored.end(),
                                   [&](const std::string& ig) { return iequals(after_last_dot(ig), m.sender); });
    if (!game::chat_line_shown(f, chat_filters_)) return;
    if (m.channel == 20) {
        // the chat server's text as it is, in the colour the packet gives (ChatMessage +0xa4)
        // (Hailstone: "<name> tells <channel>:<n>, '...'" keeps its speaker, for the modern chat's name links)
        std::string speaker;
        if (auto at = m.message.find(" tells "); at != std::string::npos && at > 0 &&
            std::all_of(m.message.begin(), m.message.begin() + static_cast<std::ptrdiff_t>(at),
                        [](char ch) { return std::isalpha(static_cast<unsigned char>(ch)) != 0; }))
            speaker = m.message.substr(0, at);
        chat(ChatEvent::Kind::Other, m.message, 0, m.color, speaker);
        return;
    }
    // GetLanguageSkill: the listener's skill in the language (the profile's languages).
    auto listener_skill = [&]() -> std::uint8_t {
        if (!profile_ || m.language >= profile_->languages.size()) return 0;
        return profile_->languages[m.language];
    };
    std::string text = m.message;
    if (game::chat_line_filtered(m.channel, f.listener_gm, f.sender_gm)) {
        // ChatText_FilterProfanityAndSlur (the bad word filter 6 and the speaker's skill), then
        // ChatText_GarbleByLanguageSkill (the listener's).
        text = game::filter_profanity_and_slur(text, static_cast<std::uint8_t>(m.language_skill),
                                               chat_filters_.mode(game::kFilterBadWord), chat_random_);
        text = game::garble_by_language_skill(text, listener_skill(), garbling_on_, chat_random_);
    }
    std::string language;
    if (m.language != 0) {
        std::uint32_t id = game::language_prefix_eqstr(m.channel, listener_skill(), f.sender_gm);
        language = id == 1411 ? fmt_eq(strings_, 1411, {fmt_eq(strings_, language_eqstr(m.language), {})})
                              : fmt_eq(strings_, 1412, {});
    }
    std::string sender_name = game::display_name(m.sender);
    if (m.channel == 7 || m.channel == 0xe) {
        social_.last_tell_from = m.sender;
        game::add_last_tell(social_, m.sender); // LastTell_Add
        if (social_.afk) away_reply(m);
    }
    auto fmt = game::channel_format(m.channel);
    std::string line = fmt.has_language_slot ? fmt_eq(strings_, fmt.eqstr, {sender_name, language, text})
                                             : fmt_eq(strings_, fmt.eqstr, {sender_name, text});
    chat(kind_of_channel(m.channel), std::move(line), 0, color_of_channel(m.channel), sender_name);
}

void GameConnection::away_reply(const ChannelMessage& m) {
    // msg_new_text's tell branch while we are away (the spawn's AFK): unless the tell is an away reply itself
    // (it starts with eqstr 1417 "Sorry, I am A.F.K." or 1418 "AFK Message: "), a tell back to its sender (channel 7,
    // the chat window's language, our skill in it, 100 for a GM): 1418 and the /afk message (0xde0884) when there
    // is one, else 1419.
    std::string sorry = fmt_eq(strings_, 1417, {}), prefix = fmt_eq(strings_, 1418, {});
    if (m.message.compare(0, sorry.size(), sorry) == 0 || m.message.compare(0, prefix.size(), prefix) == 0) return;
    ChannelMessage r;
    r.sender = character_;
    r.target = m.sender;
    r.message = afk_message_.empty() ? fmt_eq(strings_, 1419, {}) : (prefix + afk_message_).substr(0, 0x7ff);
    r.channel = 7;
    r.language = chat_language_ < 32 ? chat_language_ : 0;
    std::uint32_t skill = profile_ && r.language < profile_->languages.size() ? profile_->languages[r.language] : 0;
    if (profile_ && profile_->gm) skill = 100;
    r.language_skill = skill & 0xff;
    send_zone(encode(r));
}

// ---------------------------------------------------------------- /who

void GameConnection::who_command(const std::string& args) {
    game::WhoContext ctx;
    ctx.own_guild_id = guild_.in_guild() ? guild_.guild_id : 0xffffffff;
    ctx.guild_by_name = [this](const std::string& n) { return guild_.id_of(n); };
    ctx.is_zone_short_name = [](const std::string& n) {
        for (std::uint32_t id = 1; id < 1000; ++id)
            if (iequals(game::zone_short_name(id), n)) return true;
        return false;
    };
    auto w = game::parse_who_args(args, ctx, strings_);
    if (w.error) return system(*w.error);
    if (w.friends) {
        // OP_FriendsWho (the friends list as text): not ported yet (docs/unknowns.md).
        if (social_.friends.empty()) return system(12308);
        system(12307);
        for (const auto& f : social_.friends) chat(ChatEvent::Kind::System, f);
        return;
    }
    WhoRequest r;
    r.name = w.name;
    r.race = w.race;
    r.class_ = w.class_;
    r.level_low = w.level_low;
    r.level_high = w.level_high;
    r.gm = w.gm;
    r.guild = w.guild;
    r.type = w.type;
    send_zone(encode(r));
}

// ---------------------------------------------------------------- small commands

void GameConnection::random_command(const std::string& args) {
    auto r = game::parse_random_args(args);
    if (r.error) return system(*r.error);
    send_zone(encode_random_request(r.low, r.high));
}

void GameConnection::location_command() {
    // Cmd_location @0x4f5b00: eqstr 13177 with the spawn's +0x64 / +0x68 /
    // +0x6c (the server's y, x, z), "%1.2f" each.
    glm::vec3 s = game::world_to_server(position_);
    system(13177, {std::format("{:.2f}", s.y), std::format("{:.2f}", s.x), std::format("{:.2f}", s.z)});
}

void GameConnection::time_command() {
    // @0x5207a0: the game time (eqstr 6706), then the local time (12389, "%#c").
    if (const auto time = current_time_of_day()) {
        chat(ChatEvent::Kind::System, game::format_game_time(strings_, static_cast<int>(time->year), time->month,
                                                            time->day, time->hour));
    }
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    system(12389, {game::long_date_time(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec)});
}

void GameConnection::played_command() {
    // Cmd_played @0x4f1ed0 → @0x4f1ac0: the birthdate (12968, "%#c"), the
    // total time playing (12975) and this session (6683).
    if (!profile_) return;
    std::time_t born = profile_->birthday;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &born);
#else
    localtime_r(&born, &tm);
#endif
    system(12968, {character_, game::long_date_time(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                                                    tm.tm_sec)});
    system(12975, {character_, game::format_duration(strings_, std::uint64_t{profile_->time_played_min} * 60)});
    std::int64_t session = social_.session_start_ms ? (steady_ms() - social_.session_start_ms) / 1000 : 0;
    system(6683, {game::format_duration(strings_, static_cast<std::uint64_t>(std::max<std::int64_t>(session, 0)))});
}

void GameConnection::anonymous_command(const std::string& args, bool roleplay) {
    // Cmd_anonymous @0x4f60e0 / Cmd_roleplay @0x4e83d0 on the player's flags (+0x2b8): the line (0x111), then the
    // spawn appearance 0x15 and the save, which writes the flags as Options/Anonymous (the Options window's boxes).
    const std::string off = fmt_eq(strings_, 12946, {}), on = fmt_eq(strings_, 12947, {});
    int flags = anon_flags();
    game::AnonChange c = roleplay ? game::roleplay_command(flags, args, off, on) : game::anonymous_command(flags, args, off, on);
    system(c.eqstr, {}, 0x111);
    if (c.changed) set_anon_flags(c.flags);
}

int GameConnection::anon_flags() const {
    if (const game::Spawn* me = world_.player()) return me->anon;
    return client_options_.get(game::ClientOption::Anonymous) | (client_options_.get(game::ClientOption::Roleplaying) << 1);
}

void GameConnection::keep_anon_flags(int flags) {
    // Options/Anonymous (0xde0ba8) = the flags, WriteIntToClientINI (both boxes share the key).
    client_options_.set(game::ClientOption::Anonymous, flags & game::kAnonFlag);
    client_options_.set(game::ClientOption::Roleplaying, (flags & game::kRoleplayFlag) ? 1 : 0);
    save_client_option(game::ClientOption::Anonymous);
}

void GameConnection::set_anon_flags(int flags) {
    // The spawn's +0x2b8 (the name tint reads it), OP_SpawnAppearance {me, 0x15, flags}, then Options/Anonymous.
    if (auto it = world_.spawns.find(world_.player_id); it != world_.spawns.end())
        it->second.anon = static_cast<std::uint8_t>(flags);
    SpawnAppearance a;
    a.spawn_id = static_cast<std::uint16_t>(world_.player_id);
    a.type = 21; // AppearanceType::Anonymous
    a.parameter = static_cast<std::uint32_t>(flags);
    send_zone(encode(a));
    keep_anon_flags(flags);
}

void GameConnection::afk_command(const std::string& args) {
    // Cmd_afk @0x4e7690: "off" clears, a message sets away and keeps it, no
    // argument toggles. Told to the chat server and the zone (appearance 24).
    if (args == fmt_eq(strings_, 12946, {})) social_.afk = false;
    else if (!args.empty()) {
        social_.afk = true;
        if (args.size() < 0x100) afk_message_ = args;
    } else social_.afk = !social_.afk;
    afk_manual_ = social_.afk;
    afk_keepalive_.zone_afk_since_ms = -1;
    system(social_.afk ? 13199 : 13200);
    send_chat_server_message(social_.afk ? ";afk on silent" : ";afk off silent", false);
    set_own_afk(social_.afk);
}

void GameConnection::set_own_afk(bool afk) {
    // OP_SpawnAppearance {me, 24 (AppearanceType::AFK), on}, and our spawn's flag (the original's one field).
    if (auto it = world_.spawns.find(world_.player_id); it != world_.spawns.end()) it->second.afk = afk;
    SpawnAppearance a;
    a.spawn_id = static_cast<std::uint16_t>(world_.player_id);
    a.type = 24;
    a.parameter = afk ? 1 : 0;
    send_zone(encode(a));
}

void GameConnection::update_afk() {
    // Hailstone (game/social.h AfkKeepalive): input keeps the zone's auto AFK away and clears one it set.
    switch (game::afk_keepalive_step(afk_keepalive_, now_ms(), social_.afk, afk_manual_)) {
    case game::AfkAction::None: break;
    case game::AfkAction::Keepalive:
        log::debug("afk: keepalive");
        set_own_afk(false);
        break;
    case game::AfkAction::Clear:
        social_.afk = false;
        system(13200);
        set_own_afk(false);
        log::info("afk: input after the zone's auto AFK; cleared");
        break;
    }
}

void GameConnection::lfg_command(const std::string& args) {
    // Cmd_lfgroup @0x4e52e0: "on" / "off" say 13134 / 13135 and send {on, levels 0..100, no comments}; then the
    // group finder posts its own fields (0x678df0, checked) or takes its post back (0x678250). Anything
    // else shows the group finder.
    const bool on = args == fmt_eq(strings_, 12947, {});
    if (!on && args != fmt_eq(strings_, 12946, {})) {
        group_search_.window_open = true;
        return;
    }
    system(on ? 13134 : 13135);
    LfgCommand c;
    c.on = on;
    c.to_level = 100;
    send_lfg(c);
    if (on) group_search_post_player(false);
    else group_search_remove_player();
}

void GameConnection::assist_command(const std::string& args) {
    // Cmd_assist @0x4e9230.
    const std::string on = fmt_eq(strings_, 12947, {}), off = fmt_eq(strings_, 12946, {});
    if (args == on || args == off) {
        social_.attack_on_assist = args == on;
        return system(social_.attack_on_assist ? 13263 : 13264);
    }
    std::uint32_t who = 0;
    if (args.empty()) {
        if (world_.target_id == 0 || !world_.spawns.count(world_.target_id)) {
            system(13265);
            return system(13266);
        }
        if (world_.target_id == world_.player_id) return system(1403); // You cannot assist yourself!
        who = world_.target_id;
    } else {
        // The first player whose name starts with the argument.
        for (const auto& [id, s] : world_.spawns) {
            if (s.is_npc || s.is_corpse) continue;
            std::string n = base_name(s.name);
            if (n.size() >= args.size() && iequals(n.substr(0, args.size()), args)) {
                who = id;
                break;
            }
        }
        if (!who) return;
    }
    send_zone(encode_assist(who));
}

void GameConnection::follow_command() {
    // Cmd_follow @0x4f5ef0.
    auto it = world_.spawns.find(world_.target_id);
    // The target must be a player of our group (13225).
    if (it == world_.spawns.end() || it->second.is_npc || it->second.is_corpse ||
        (it->first != world_.player_id && !group_.has_member(base_name(it->second.name))))
        return system(13225);
    if (it->first == world_.player_id) return system(13226);
    if (pstatus_.stand_state != 100) return system(13229); // Stand up first!
    system(13230, {game::display_name(it->second.name)});
    system(13231);
    system(13232);
    social_.follow_id = it->first;
}

void GameConnection::stop_follow(bool announce) {
    std::uint32_t id = social_.follow_id;
    if (!id) return;
    social_.follow_id = 0;
    auto it = world_.spawns.find(id);
    if (announce && it != world_.spawns.end()) system(12694, {game::display_name(it->second.name)}); // You are no longer auto-following %1.
}

void GameConnection::stop_follow_with(std::uint32_t eqstr) {
    auto it = world_.spawns.find(social_.follow_id);
    if (it != world_.spawns.end()) system(eqstr, {game::display_name(it->second.name)});
    stop_follow(true); // EQPlayer::TurnOffAutoFollow: "You are no longer auto-following %1." (12694)
}

void GameConnection::change_chat_filter(int filter, std::int32_t value) {
    if (filter < 0 || filter >= game::kChatFilterCount) return;
    set_chat_filter(filter, value);
    // The original writes eqclient.ini [Options]; here the character's chat.ini (the asset folder is only read).
    std::filesystem::path chat_ini = own_files().chat_ini();
    if (!chat_ini.empty() &&
        !save_chat_setting(chat_ini, "Options", game::ChatFilters::ini_key(filter),
                           std::to_string(chat_filters_.settings[static_cast<std::size_t>(filter)])))
        log::warn("cannot write {}", chat_ini.string());
    send_server_filter();
}

void GameConnection::send_server_filter() {
    // Send_OP_SetServerFilter (SetServerFilter_Struct, RoF2: 35 x u32): eqclient.ini [Defaults] ServerFilter (0 or 1,
    // else 0), then each filter's setting as eqclient.ini keeps it (EQEmu's FilterGuildChat is 1, FilterNone 0).
    if (state_ != ConnectionState::InZone) return;
    IniFile ini;
    if (auto path = game_.find("eqclient.ini")) ini.load(*path);
    int server_filter = ini.get_int("Defaults", "ServerFilter", 0);
    ByteWriter w;
    w.le<std::uint32_t>(server_filter == 1 ? 1u : 0u);
    for (int f = 0; f < 34; ++f)
        w.le<std::uint32_t>(f < game::kChatFilterCount ? static_cast<std::uint32_t>(chat_filters_.settings[static_cast<std::size_t>(f)]) : 0u);
    send_zone(make_packet(Op::SetServerFilter, std::move(w)));
}

void GameConnection::load_chat_filters() {
    // ChatFilters_LoadFromIni @0x543e50: eqclient.ini [Options], one key per filter; here the character's chat.ini
    // first, eqclient.ini giving the defaults (before a character is chosen, the defaults alone).
    IniFile ini = load_chat_settings(game_, own_files().chat_ini());
    for (int f = 0; f < game::kChatFilterCount; ++f)
        set_chat_filter(f, ini.get_int("Options", game::ChatFilters::ini_key(f), -1));
}

void GameConnection::load_client_options() {
    // CEverQuest::loadOptions @0x544be0 (eqclient.ini over defaults.ini): here the character's options.ini over the
    // asset folder's eqclient.ini, the defaults where both lack a key (before a character is chosen, eqclient.ini's).
    client_options_.load(load_option_settings(game_, own_files().options_ini()));
    tracking_from_options();
    // Logging that was on goes on in the default log ("eqlog") once the character is known.
    open_chat_log("eqlog");
}

void GameConnection::copy_character_settings(const std::filesystem::path& from_dir, bool chat, bool options) {
    CharacterFiles own = own_files();
    if (own.empty() || from_dir.empty()) return;
    std::error_code ec;
    if (std::filesystem::equivalent(from_dir, own.dir, ec)) return;
    // Every key the other character's file has, over this one's; what it lacks stays as this character set it.
    auto merge = [&](const std::filesystem::path& from, const std::filesystem::path& to, auto skip) {
        IniFile src, dst;
        if (!src.load(from)) return false;
        dst.load(to);
        for (const std::string& section : src.sections())
            for (const std::string& key : src.keys(section))
                if (!skip(section, key))
                    if (auto v = src.get(section, key)) dst.set(section, key, *v);
        std::error_code dec;
        std::filesystem::create_directories(to.parent_path(), dec);
        if (!dst.save(to)) log::warn("cannot write {}", to.string());
        return true;
    };
    if (chat && merge(from_dir / own.chat_ini().filename(), own.chat_ini(), [](const std::string&, const std::string&) { return false; })) {
        load_chat_filters();
        send_server_filter();
    }
    if (options) {
        // Anonymous and Roleplaying share [Options] Anonymous: the character's public flags, not a preference.
        const game::ClientOptionInfo& anon = game::client_option_info(game::ClientOption::Anonymous);
        auto skip = [&](const std::string& section, const std::string& key) {
            return game::same_no_case(section, anon.section) && game::same_no_case(key, anon.key);
        };
        if (merge(from_dir / own.options_ini().filename(), own.options_ini(), skip)) {
            game::ClientOptions before = client_options_;
            client_options_.load(load_option_settings(game_, own.options_ini()));
            tracking_from_options();
            for (const game::ClientOptionInfo& o : game::kClientOptions)
                if (int old_value = before.get(o.id); client_options_.get(o.id) != old_value) apply_client_option(o.id, old_value);
        }
    }
}

void GameConnection::change_client_option(game::ClientOption id, int value) {
    int old_value = client_options_.get(id);
    if (client_options_.set(id, value) == old_value) return;
    save_client_option(id);
    apply_client_option(id, old_value);
}

void GameConnection::save_client_option(game::ClientOption id) {
    // The original writes eqclient.ini (or the UI / character ini); here the character's options.ini.
    std::filesystem::path path = own_files().options_ini();
    IniFile ini;
    ini.load(path);
    client_options_.store(id, ini);
    if (path.empty() || !ini.save(path)) log::warn("cannot write {}", path.string());
}

void GameConnection::apply_client_option(game::ClientOption id, int old_value) {
    // COptionsWnd's page handlers (GeneralPageWndNotification @0x702640, DisplayPageWndNotification,
    // MousePageWndNotification @0x703a30, MailPageWndNotification @0x703e40): what a change does at once, beyond
    // being kept. Options read where they act need nothing here.
    (void)old_value;
    using O = game::ClientOption;
    const int v = client_options_.get(id);
    const bool on = v != 0;
    switch (id) {
    case game::ClientOption::FastItemDestroy: // "Fast Item Destroy is ON." / "... OFF." (colour 0xf)
        system(v ? 12032 : 12031, {}, 0xf);
        break;
    case game::ClientOption::LootAllConfirm: // ToggleInt: the on / off line (0x111)
        system(v ? 7578 : 7579, {}, 0x111);
        break;
    case game::ClientOption::AANoConfirm:
        system(v ? 4040 : 4039, {}, 0x111);
        break;
    case game::ClientOption::PointMerchantNoConfirm:
        system(v ? 8555 : 8554, {}, 0x111);
        break;
    case game::ClientOption::LeadershipNoConfirm:
        system(v ? 8581 : 8580, {}, 0x111);
        break;
    case game::ClientOption::FastDrop: // the combo runs /fastdrop with its word: Cmd_fastdrop's line
        system(game::fastdrop_message(v), {}, 0x111);
        break;
    case game::ClientOption::LootNoDrop: // the combo runs /lootnodrop with its word: "New Setting: ..."
        say_lootnodrop_setting(v);
        break;
    case O::Anonymous:
    case O::Roleplaying:
        // +0x23c / +0x240: the flags +1 / +2 (or less), 0x2f02 / 0x2f03 or 0x33b1 / 0x33b2 (0x111), SendAnonymous
        // (the player's +0x2b8, spawn appearance 0x15, the save), for roleplay SetNameSpriteTint, Options/Anonymous.
        if (id == O::Anonymous) system(on ? 12034 : 12035, {}, 0x111);
        else system(on ? 13233 : 13234, {}, 0x111);
        set_anon_flags(client_options_.get(O::Anonymous) | (client_options_.get(O::Roleplaying) << 1));
        break;
    case O::DismissMercenaryConfirm:
        system(on ? 7610 : 7611, {}, 0x111); // +0x2a0: 0x1dba / 0x1dbb
        break;
    case O::RaidInviteConfirm:
        system(on ? 706 : 707, {}, 0x111); // +0x2a4: 0x2c2 / 0x2c3
        break;
    case O::OfflineModeConfirm:
        system(on ? 13347 : 13348, {}, 0x111); // +0x2a8: 0x3423 / 0x3424
        break;
    case O::AcceptKickRequests:
        // +0x288: the literal lines (0x9f3b28 / 0x9f3ab8, 0x111), WriteBoolToClientINI.
        chat(ChatEvent::Kind::System,
             on ? "You will now automatically accept any kick requests made by another player logging into your account."
                : "You will no longer automatically accept any kick requests made by another player logging into your account.",
             0, 0x111);
        break;
    case O::MailForwarding: // MailPageWndNotification: the box tells MailMgr_SetMailForwarding at once
        mail_set_forwarding(on);
        break;
    default: break;
    }
}

void GameConnection::fastdrop_command(const std::string& args) {
    // Cmd_fastdrop @0x4e5200: "on" 1, "off" 0, "never" -1 (game::fastdrop_choice), said (13116 / 13117 / 13118) and
    // saved to NoSafeDrop; anything else 13119. The Options window's Fast Drop combo shows the same setting.
    auto v = game::fastdrop_choice(args, fmt_eq(strings_, 12947), fmt_eq(strings_, 12946), fmt_eq(strings_, 12989));
    if (!v) return system(game::kStrFastDropUsage, {}, 0x111);
    if (client_options_.get(game::ClientOption::FastDrop) == *v) system(game::fastdrop_message(*v), {}, 0x111);
    else change_client_option(game::ClientOption::FastDrop, *v); // saved, then said (apply_client_option)
}

void GameConnection::say_lootnodrop_setting(std::int32_t value) {
    chat(ChatEvent::Kind::System, fmt_eq(strings_, game::kStrLootNoDropNew) + fmt_eq(strings_, game::lootnodrop_set_name(value)),
         0, 0x111);
}

void GameConnection::lootnodrop_command(const std::string& args) {
    // Cmd_lootnodrop @0x4f2700 (game::lootnodrop_choice): without an argument the usage (12990 over "always",
    // "usable", "never") and "Your Current Setting: " with the setting's name (the original's swapped names,
    // game::lootnodrop_shown_name); with one the new setting, said ("New Setting: ...") and saved to LootNoDrop. The
    // Options window's Loot No Drop combo shows the same setting.
    const int mode = client_options_.get(game::ClientOption::LootNoDrop);
    if (args.empty()) {
        system(game::kStrLootNoDropUsage, {fmt_eq(strings_, 12987), fmt_eq(strings_, 13925), fmt_eq(strings_, 12989)}, 0x111);
        chat(ChatEvent::Kind::System,
             fmt_eq(strings_, game::kStrLootNoDropCurrent) + fmt_eq(strings_, game::lootnodrop_shown_name(mode)), 0, 0x111);
        return;
    }
    auto v = game::lootnodrop_choice(args, fmt_eq(strings_, 12989), fmt_eq(strings_, 12988), fmt_eq(strings_, 13925),
                                     fmt_eq(strings_, 12987));
    if (!v) return; // (the original writes the unchanged setting back)
    if (mode == *v) say_lootnodrop_setting(*v);
    else change_client_option(game::ClientOption::LootNoDrop, *v); // saved, then said (apply_client_option)
}

std::optional<int> GameConnection::character_setting(std::string_view name) const {
    if (!profile_) return std::nullopt;
    if (name == "loot_autosplit") return autosplit_ ? 1 : 0;
    if (name == "auto_consent_group") return profile_->auto_consent[0] ? 1 : 0;
    if (name == "auto_consent_raid") return profile_->auto_consent[1] ? 1 : 0;
    if (name == "auto_consent_guild") return profile_->auto_consent[2] ? 1 : 0;
    if (name == "show_helm") return profile_->show_helm ? 1 : 0;
    return std::nullopt;
}

bool GameConnection::set_character_setting(std::string_view name, int value) {
    bool checked = value != 0;
    auto appearance = [&](std::uint16_t type, std::uint32_t parameter) {
        SpawnAppearance a;
        a.spawn_id = static_cast<std::uint16_t>(world_.player_id);
        a.type = type;
        a.parameter = parameter;
        send_zone(encode(a));
    };
    if (name == "loot_autosplit") {
        // +0x238: CharacterBase +0x46c, 0x2f04 / 0x2f05 (0x111), Send_OP_SpawnAppearance_00516c00 {me, 0x1c, flag}.
        autosplit_ = checked;
        system(autosplit_ ? 12036 : 12037, {}, 0x111);
        appearance(28, autosplit_ ? 1 : 0);
        return true;
    }
    int consent = name == "auto_consent_group" ? 0 : name == "auto_consent_raid" ? 1 : name == "auto_consent_guild" ? 2 : -1;
    if (consent >= 0) {
        // +0x278 / +0x27c / +0x280: the PcClient's virtual +0x14 / +0x18 / +0x1c (checked) sets +0x2921.. ; the line
        // 0x1f40 / 0x1f42 / 0x1f44 (on) or the next id (off), 0x111; spawn appearance 0x28 / 0x29 / 0x2a with the
        // byte (EQEmu: ConsentCorpses).
        if (!profile_) return true;
        profile_->auto_consent[static_cast<std::size_t>(consent)] = checked ? 1 : 0;
        system(8000u + 2u * static_cast<std::uint32_t>(consent) + (checked ? 0u : 1u), {}, 0x111);
        appearance(static_cast<std::uint16_t>(40 + consent), checked ? 1 : 0);
        return true;
    }
    if (name == "show_helm") {
        // SetHelm (DisplayPageWndNotification +0x310): the PcClient's +0x2964, the player's +0xf20, spawn appearance
        // 0x2b (check != 0) and the head slot's model refreshed (here the spawn's show_helm, which the model reads).
        // No line is printed. Nothing without a PcClient.
        if (!profile_) return true;
        profile_->show_helm = checked ? 1 : 0;
        if (auto it = world_.spawns.find(world_.player_id); it != world_.spawns.end())
            it->second.appearance.show_helm = checked;
        appearance(43, checked ? 1 : 0);
        return true;
    }
    return false;
}

void GameConnection::toggle_mouse_look_option() {
    change_client_option(game::ClientOption::MouseLook, client_option(game::ClientOption::MouseLook) ? 0 : 1);
    system(client_option(game::ClientOption::MouseLook) ? 12373 : 12372, {}, 0x111);
}

// ---------------------------------------------------------------- guilds

bool GameConnection::has_guild_permission(std::int32_t permission) const {
    const game::Spawn* me = world_.player();
    return game::local_pc_has_guild_permission(profile_.has_value(), me && me->gm, static_cast<std::int32_t>(guild_.rank),
                                               permission, guild_.permissions);
}

std::uint32_t GameConnection::received_guild_permission_context() const {
    return pending_guild_permissions_ ? pending_guild_permissions_->guild_id : guild_.guild_id;
}

void GameConnection::reset_guild_permissions(std::uint32_t next_guild_id) {
    guild_.permissions.clear();
    guild_.rank_names = {};
    if (pending_guild_permissions_ && next_guild_id != 0 && next_guild_id != 0xffffffff &&
        pending_guild_permissions_->guild_id == next_guild_id) {
        guild_.permissions = pending_guild_permissions_->permissions;
        guild_.rank_names = std::move(pending_guild_permissions_->rank_names);
    }
    pending_guild_permissions_.reset();
}

void GameConnection::guild_command(std::string_view h, const std::string& args) {
    std::string_view rest = args;
    if (h == "Cmd_guildinvite") {
        // Send_OP_GuildInvite @0x424370: not in a guild 12242, then a GM or permission 7, else 5410.
        if (!guild_.in_guild()) return system(12242);
        if (!has_guild_permission(game::kGuildPermInvite)) return system(5410);
        if (std::string_view r = args; game::next_cmd_arg(r).empty())
            if (auto t = world_.spawns.find(world_.target_id); t != world_.spawns.end() && other_deity_team(t->second))
                return system(12244);
        std::string who = target_name_or(args);
        if (who.empty()) return system(12248);
        // EQEmu looks for the invitee in this zone only. A player not here goes to server/'s script when the zone
        // has it (OP_HailstoneGuild), which takes the invitation to the player's zone; without it the server's answer is
        // its "must be in zone".
        bool here = std::any_of(world_.spawns.begin(), world_.spawns.end(), [&](const auto& e) {
            return !e.second.is_npc && iequals(base_name(e.second.name), who);
        });
        if (!here && guild_world_invites_) {
            HailstoneGuild g;
            g.action = HailstoneGuildAction::Invite;
            g.name = who;
            g.guild_id = guild_.guild_id;
            g.value = 8;
            send_zone(encode_hailstone_guild(g));
        } else {
            GuildCommand c;
            c.other_name = who;
            c.my_name = character_;
            c.guild_id = guild_.guild_id;
            c.rank = 8; // recruit
            send_zone(encode_guild_invite(c));
        }
        system(12245, {who});
        return;
    }
    if (h == "Cmd_guildremove") {
        // CEverQuest::GuildRemove @0x523a10.
        if (!guild_.in_guild()) return system(12242);
        std::string who = target_name_or(args);
        if (who.empty()) return system(12255);
        bool self = iequals(who, character_);
        if (self && guild_.rank == 1) return system(12251);
        GuildCommand c;
        c.other_name = who;
        c.my_name = character_;
        c.guild_id = guild_.guild_id;
        send_zone(encode_guild_remove(c));
        if (self) system(12252);
        else system(12254, {who});
        return;
    }
    if (h == "Cmd_guildleader") {
        // Send_OP_GuildLeader @0x524070.
        if (!guild_.in_guild()) return system(12242);
        if (guild_.rank != 1) return system(12261);
        std::string who = target_name_or(args);
        if (who.empty()) return;
        system(12262, {who});
        send_zone(encode_guild_leader(character_, who));
        return;
    }
    if (h == "Send_OP_SetGuildMOTD") {
        // Send_OP_SetGuildMOTD @0x4f1310.
        if (!guild_.in_guild()) return system(12966);
        GuildMotd m;
        m.name = character_;
        m.set_by = character_;
        m.motd = args;
        send_zone(encode_set_guild_motd(m));
        return;
    }
    if (h == "Cmd_getguildmotd") {
        if (!guild_.in_guild() || guild_.guild_id >= 50000) return system(12966);
        send_zone(encode_get_guild_motd(guild_.guild_id, character_));
        return;
    }
    if (h == "Cmd_guilddemote") {
        // Cmd_guilddemote @0x4df510: the named member (else a player target's name), then Send_OP_GuildDemote
        // @0x424940 on the guild member record (CGuild::FindMemberByName); nothing without one.
        std::string who = game::next_cmd_arg(rest);
        if (who.empty()) {
            auto t = world_.spawns.find(world_.target_id);
            if (t == world_.spawns.end() || t->second.is_npc) return;
            who = t->second.name;
            if (who.empty()) return;
        }
        auto member = [&](const std::string& n) -> const game::GuildMemberInfo* {
            for (const auto& m : guild_.members)
                if (iequals(m.name, n)) return &m;
            return nullptr;
        };
        const game::GuildMemberInfo* target = member(who);
        if (!target) return;
        const game::GuildMemberInfo* me = member(character_);
        const game::Spawn* self = world_.player();
        bool gm = self && self->gm; // the local player's virtual +0xdc
        auto rank = static_cast<std::int32_t>(target->rank);
        if (me != target) {
            if (!has_guild_permission(game::kGuildPermDemote)) return system(5410);
            std::int32_t mine = me ? static_cast<std::int32_t>(me->rank) : 0;
            if (!gm && !game::guild_member_rank_at_least(me ? &mine : nullptr, &rank)) return system(516);
        } else {
            if (rank == 1) return;
            if (!has_guild_permission(game::kGuildPermDemoteSelf)) return system(5410);
        }
        if (!gm && game::guild_rank_at_least(rank + 1, rank)) return;
        ByteWriter w; // {your member name, its name, rank + 1}, cleared first (0x84 bytes)
        w.fixed_string((me ? me->name : std::string()).substr(0, 63), 64);
        w.fixed_string(target->name.substr(0, 63), 64);
        w.le<std::int32_t>(rank + 1);
        send_zone(make_packet(Op::GuildDemote, std::move(w)));
        return;
    }
    if (h == "Cmd_guildpromote") {
        // Send_OP_GuildPromote @0x424640 (Cmd_guildpromote passes the whole argument): not in a guild 12242, then
        // a GM or permission 8 (5410). Without an argument the target, a player other than yourself (12248
        // otherwise); your own name only for a GM. The new rank is the member's rank - 1, or 8 for a name not on
        // the member list (12245 then); unless a GM, a new rank above your own refuses (516, default colour).
        // Sends {name, own name, guild id, new rank} (0x88 bytes).
        if (!guild_.in_guild()) return system(12242, {}, 0x111);
        if (!has_guild_permission(game::kGuildPermPromote)) return system(5410, {}, 0x111);
        const game::Spawn* self = world_.player();
        bool gm = self && self->gm; // the local player's virtual +0xdc
        std::string who;
        if (args.empty()) {
            auto t = world_.spawns.find(world_.target_id);
            if (t == world_.spawns.end() || t->second.is_npc || t->second.is_corpse || t->first == world_.player_id)
                return system(12248, {}, 0x111);
            who = t->second.name;
        } else if (!iequals(args, character_) || gm) {
            who = args;
        }
        if (who.empty()) return;
        who = who.substr(0, 63);
        const game::GuildMemberInfo* member = nullptr;
        for (const auto& m : guild_.members)
            if (iequals(m.name, who)) member = &m;
        auto promoted = member ? static_cast<std::int32_t>(member->rank) - 1 : 8;
        if (!gm && game::guild_rank_above(promoted, static_cast<std::int32_t>(guild_.rank))) return system(516);
        if (!member) system(12245, {who}, 0x111);
        GuildPromote p;
        p.target = who;
        p.name = character_;
        p.guild_id = guild_.guild_id;
        p.new_rank = static_cast<std::uint32_t>(promoted);
        send_zone(encode(p));
        return;
    }
    if (h == "Send_OP_GuildCreate") {
        // Send_OP_GuildCreate @0x4e3350: 4..27 characters (counted before leading blanks are skipped), refused on a
        // shrouded character (profile +0x340c; 0 here), {name} (0x40 bytes). Its lines go out in colour 0xf.
        constexpr std::uint32_t kColor = 0xf;
        constexpr const char* kMissing = "You must specify a guild name to create!";
        if (args.empty()) return chat(ChatEvent::Kind::System, kMissing, 0, kColor);
        if (args.size() < 4) return system(6369, {}, kColor);
        if (args.size() > 0x1b) return system(6370, {}, kColor);
        if (!profile_) return system(6371, {}, kColor);
        std::size_t start = args.find_first_not_of(" \t");
        if (start == std::string::npos) return chat(ChatEvent::Kind::System, kMissing, 0, kColor);
        ByteWriter w;
        w.fixed_string(args.substr(start, 63), 64);
        send_zone(make_packet(Op::GuildCreate, std::move(w)));
        return;
    }
    if (h == "Cmd_guildwar") {
        // Cmd_guildwar @0x4df440: the first word (31 characters; the target's name without one) and the number after
        // it, 1..14 days (anything else 14). Send_OP_GuildWar @0x525ed0 checks nothing: {name, own name, guild id,
        // days, 1}; a leader (rank 1) then sees 12276.
        std::string who = game::next_cmd_arg(rest).substr(0, 31);
        int days = std::atoi(std::string(rest).c_str());
        if (days <= 0 || days > 14) days = 14;
        if (who.empty())
            if (auto t = world_.spawns.find(world_.target_id); t != world_.spawns.end()) who = t->second.name.substr(0, 63);
        send_zone(encode_guild_war(GuildWarPeace{who, character_, guild_.guild_id, days}));
        if (guild_.rank == 1) system(12276, {who, std::to_string(days)}, 0x111);
        return;
    }
    if (h == "Cmd_guildpeace") {
        // Cmd_guildpeace @0x4df4b0: the whole argument, else the target's name. Send_OP_GuildPeace @0x526040: only
        // a leader (12278 otherwise) sees 12277 and sends {name, own name, guild id, 0, 1}.
        std::string who = args;
        if (who.empty())
            if (auto t = world_.spawns.find(world_.target_id); t != world_.spawns.end()) who = t->second.name;
        if (guild_.rank != 1) return system(12278, {}, 0x111);
        who = who.substr(0, 63);
        system(12277, {who}, 0x111);
        send_zone(encode_guild_peace(GuildWarPeace{who, character_, guild_.guild_id, 0}));
        return;
    }
    if (h == "Cmd_guilddelete") {
        // CEverQuest_ConfirmGuildDelete @0x523ee0: only the leader (rank 1; 9059 otherwise); confirmation dialog
        // 0x6d with 9060, whose yes sends OP_GuildDelete and shows 9061.
        if (guild_.rank != 1) return system(9059);
        ask(Confirmation{kConfirmGuildDelete, strings_.format(9060, std::vector<std::string>{}), {}});
        return;
    }
    if (h == "Cmd_guildstatus") {
        // Send_OP_GuildStatus @0x523f60: GuildStatus_Struct, 140 bytes: the name (steve_strncpy 0x40) and 76
        // bytes the original leaves as they were on its stack (zeros here). EQEmu's DECODE(OP_GuildStatus) wants
        // exactly 140. The server answers in text.
        std::string who = target_name_or(args);
        if (who.empty()) return system(12260);
        ByteWriter w;
        w.fixed_string(who.substr(0, 63), 64);
        w.zeros(76);
        send_zone(make_packet(Op::GuildStatus, std::move(w)));
        return;
    }
    (void)rest;
    chat(ChatEvent::Kind::System, std::format("{} is not supported by this client yet.", h));
}

namespace {
RaidGeneral raid_general(std::uint32_t action, const std::string& player, const std::string& leader,
                         std::uint32_t parameter = 0, std::uint32_t raid_id = 0) {
    RaidGeneral g;
    g.raid_id = raid_id;
    g.action = action;
    g.player_name = player.substr(0, 63);
    g.leader_name = leader.substr(0, 63);
    g.parameter = parameter;
    return g;
}
} // namespace

void GameConnection::raid_lock(bool lock) {
    // CRaid_SendLockRaid / SendUnlockRaid: in a raid, its leader, and the lock not already as asked.
    if (!zone_session_ || !raid_.active || !iequals(raid_.leader, character_) || raid_.locked == lock) return;
    send_zone(encode(raid_general(lock ? kRaidCmdLock : kRaidCmdUnlock, raid_.leader, raid_.leader, 0, raid_.id)));
}

void GameConnection::raid_move_group(const std::string& member, std::int32_t group) {
    if (!zone_session_ || !raid_.active || !iequals(raid_.leader, character_) || member.empty()) return;
    send_zone(encode(raid_general(kRaidMoveGroup, raid_.leader, member, static_cast<std::uint32_t>(group), raid_.id)));
}

void GameConnection::raid_remove_group_leader(const std::string& member) {
    if (!zone_session_ || !raid_.active || !iequals(raid_.leader, character_) || member.empty()) return;
    send_zone(encode(raid_general(kRaidCmdRemoveGroupLeader, raid_.leader, member, 0, raid_.id)));
}

void GameConnection::raid_send_leadership_change(const std::optional<std::string>& name) {
    // CRaid::SendLeadershipChange: no name is the raid target (the raid window's Make Leader); a name not in the raid
    // (an empty one too) prints 5082.
    if (!zone_session_ || !raid_.active || !iequals(raid_.leader, character_)) return;
    if (!name) {
        if (!raid_.target.empty() && raid_.is_member(raid_.target))
            send_zone(encode(raid_general(kRaidCmdMakeLeader, character_, raid_.target, 0, raid_.id)));
        return;
    }
    if (name->empty() || !raid_.is_member(*name)) return system(5082, {*name});
    send_zone(encode(raid_general(kRaidCmdMakeLeader, character_, *name, 0, raid_.id)));
}

void GameConnection::raid_add_looter() {
    // CRaid::AddRaidLooter: the raid target, for the leader with loot type 3; a looter already is not sent again.
    if (!zone_session_ || !raid_.active) return;
    if (!iequals(raid_.leader, character_) || raid_.loot_type != 3 || raid_.target.empty() || !raid_.is_member(raid_.target))
        return system(5095);
    if (std::any_of(raid_.looters.begin(), raid_.looters.end(), [&](const std::string& n) { return iequals(n, raid_.target); }))
        return;
    send_zone(encode(raid_general(kRaidCmdAddLooter, raid_.leader, raid_.target, 0, raid_.id)));
    // EQEmu never sends OP_RaidUpdate 0x21 back (only its chat line): the looter is kept here as sent.
    raid_.looters.push_back(raid_.target);
}

void GameConnection::raid_remove_looter() {
    // CRaid::RemoveRaidLooter: the same checks (5096), without the looter check.
    if (!zone_session_ || !raid_.active) return;
    if (!iequals(raid_.leader, character_) || raid_.loot_type != 3 || raid_.target.empty() || !raid_.is_member(raid_.target))
        return system(5096);
    send_zone(encode(raid_general(kRaidCmdRemoveLooter, raid_.leader, raid_.target, 0, raid_.id)));
    std::erase_if(raid_.looters, [&](const std::string& n) { return iequals(n, raid_.target); }); // (as for 0x21)
}

void GameConnection::raid_set_loot_type(const std::string& text) {
    // CRaid::SetLootType @0x47d9b0: in a raid; the leader and a
    // single digit, else 5094 (dsp_chat_2); 1..4 other than the current type becomes it and is sent {0x20, me, me}.
    if (!zone_session_ || !raid_.active) return;
    if (!iequals(raid_.leader, character_) || text.size() != 1 || !std::isdigit(static_cast<unsigned char>(text[0])))
        return system(5094);
    std::uint32_t type = static_cast<std::uint32_t>(text[0] - '0');
    if (type < 1 || type > 4 || type == raid_.loot_type) return;
    raid_.loot_type = type;
    send_zone(encode(raid_general(kRaidCmdLootType, character_, character_, type, raid_.id)));
}

void GameConnection::set_raid_ability_ranks(const std::array<std::uint32_t, 64>& block) {
    // Leadership_SetRaidAbilityRanks @0x55e410: the block over the character's +0x24c, where Leadership_HasAbilityRank
    // reads the raid abilities (16..31) at 4 bytes each.
    std::string ranks;
    for (int i = game::kFirstRaidAbility; i < game::kLeadershipAbilities; ++i) {
        leadership_.active_ranks[static_cast<std::size_t>(i)] = block[static_cast<std::size_t>(i)];
        if (block[static_cast<std::size_t>(i)]) ranks += std::format(" {}:{}", i, block[static_cast<std::size_t>(i)]);
    }
    log::debug("raid leadership ranks:{}", ranks.empty() ? std::string(" none") : ranks);
}

void GameConnection::set_raid_target(const std::string& name) {
    // CRaid::SetTargetRaidPlayer @0x47cd80: a named member becomes the raid target and is targeted
    // (Send_OP_TargetCommand_004e9790 by name: the nearest spawn of that name, here the player of that name).
    if (name.empty()) {
        raid_.target.clear();
        return;
    }
    const game::RaidMember* m = raid_.find(name);
    if (!m) return;
    raid_.target = m->name;
    for (const auto& [id, sp] : world_.spawns)
        if (!sp.is_npc && !sp.is_corpse && iequals(base_name(sp.name), m->name)) {
            set_target(id);
            break;
        }
}

void GameConnection::raid_delegate_ability(std::uint32_t ability, const std::string& member) {
    // Send_OP_RaidDelegateAbility @0x55e5b0: no member prints 0x222c (colour 0xd).
    if (state_ != ConnectionState::InZone || (ability != 3 && ability != 4)) return;
    if (interface_locked()) return system(game::kStrInterfaceLocked, {}, game::kLockedColor);
    if (member.empty()) return system(0x222c, {}, 0xd);
    // A window or hot button may have been enabled before leadership, ranks or membership changed.
    if (!raid_.active || !iequals(raid_.leader, character_)) return system(0x21c9, {}, 0xd);
    const game::RaidMember* selected = raid_.find(member);
    if (!selected) return system(0x222c, {}, 0xd);
    if (!leadership_ability_active(static_cast<int>(ability) + 16, 1)) return system(0x21c1, {}, 0xd);
    DelegateAbility d;
    d.ability = ability;
    d.name = selected->name.substr(0, 63);
    send_zone(encode(d, Op::RaidDelegateAbility));
}

void GameConnection::raid_find_path_to_target() { find_path_to_player(raid_.target); }

void GameConnection::raid_set_note(const std::string& member, const std::string& note) {
    // CRaidWnd::WndNotification's NoteUpdateButton: the note list's selected member and the note input's text
    // (Send_OP_RaidInvite_0047c430); the note shows when the server sends it back (OP_RaidUpdate 0x24).
    if (!zone_session_ || !raid_.active || member.empty()) return;
    send_zone(encode_set_raid_note(character_, raid_.id, member, note));
}

void GameConnection::raid_set_motd(const std::string& motd) {
    if (!zone_session_ || !raid_.active) return;
    send_zone(encode_set_raid_motd(character_, motd));
}

void GameConnection::guild_set_public_note(const std::string& member, const std::string& note) {
    if (!zone_session_ || !guild_.in_guild()) return;
    send_zone(encode_guild_public_note(guild_.guild_id, character_, member.substr(0, 63), note.substr(0, 255)));
}

void GameConnection::guild_set_alt(const std::string& member, bool on) {
    if (!zone_session_) return;
    const game::GuildMemberInfo* m = nullptr;
    for (const auto& g : guild_.members)
        if (g.name == member) m = &g;
    if (!m) return;
    bool self = member.substr(0, 64) == character_.substr(0, 64);
    if (!self && !has_guild_permission(game::kGuildPermBanker)) return system(516);
    std::uint32_t flags = (m->banker ? 1u : 0u) | (m->alt ? 2u : 0u);
    flags = on ? flags | 2u : flags & ~2u;
    send_zone(encode_guild_manage_banker(m->name, flags));
}

void GameConnection::guild_set_url(const std::string& url) {
    // 0x423b00 keeps it; sent only when it differs from the guild's.
    if (!zone_session_ || url == guild_.url) return;
    guild_.url = url;
    send_zone(encode_guild_update_text(0, url));
}

void GameConnection::guild_set_channel(const std::string& channel) {
    if (!zone_session_ || channel == guild_.channel) return;
    guild_.channel = channel; // 0x423b30
    send_zone(encode_guild_update_text(1, channel));
}

void GameConnection::guild_rename_rank(std::uint32_t rank, const std::string& name) {
    if (zone_session_) send_zone(encode_guild_rank_name(rank, name));
}

void GameConnection::guild_set_rank_permission(std::uint32_t rank, std::uint32_t permission, bool on) {
    if (zone_session_) send_zone(encode_guild_rank_permission(rank, permission, on));
}

bool GameConnection::guild_change_member_rank(const std::string& member, std::uint32_t rank) {
    if (!zone_session_) return false;
    const game::GuildMemberInfo* m = nullptr;
    for (const auto& g : guild_.members)
        if (iequals(g.name, member)) { // CGuild::FindMemberByName (__strnicmp, the first)
            m = &g;
            break;
        }
    if (!m) return false;
    auto old_rank = static_cast<std::int32_t>(m->rank), new_rank = static_cast<std::int32_t>(rank);
    if (game::guild_rank_above(new_rank, old_rank) && has_guild_permission(game::kGuildPermPromote)) {
        GuildPromote p;
        p.target = m->name.substr(0, 63);
        p.name = character_;
        p.guild_id = guild_.guild_id;
        p.new_rank = rank;
        send_zone(encode(p));
        return true;
    }
    bool self = m->name == character_;
    if (game::guild_rank_above(old_rank, new_rank) &&
        ((self && has_guild_permission(game::kGuildPermDemoteSelf)) || has_guild_permission(game::kGuildPermDemote))) {
        ByteWriter w; // {own name, the member, the new rank}, cleared first (0x84 bytes)
        w.fixed_string(character_.substr(0, 63), 64);
        w.fixed_string(m->name.substr(0, 63), 64);
        w.le<std::uint32_t>(rank);
        send_zone(make_packet(Op::GuildDemote, std::move(w)));
        return true;
    }
    return false;
}

void GameConnection::guild_invite_tick() {
    // The invitation's confirmation dialog times out after 10 s and answers as a decline (@0x4b9809).
    if (!guild_.inviter.empty() && steady_ms() - guild_.invite_ms >= kGuildInviteMs) answer_guild_invite(false);
}

void GameConnection::answer_guild_invite(bool accept) {
    if (guild_.inviter.empty()) return;
    std::uint32_t response = accept ? guild_.invite_rank : kGuildInviteDecline;
    if (guild_.invite_world) {
        HailstoneGuild g;
        g.action = HailstoneGuildAction::Answer;
        g.name = guild_.inviter;
        g.guild_id = guild_.invite_guild;
        g.value = response;
        send_zone(encode_hailstone_guild(g));
    } else {
        GuildInviteAccept a;
        a.inviter = guild_.inviter;
        a.new_member = character_;
        a.response = response;
        a.guild_id = guild_.invite_guild;
        send_zone(encode(a));
    }
    guild_.inviter.clear();
    guild_.invite_world = false;
}

void GameConnection::handle_hailstone_guild(const HailstoneGuild& g) {
    if (g.action == HailstoneGuildAction::Hello) {
        guild_world_invites_ = (g.value & kHailstoneGuildWorldInvites) != 0;
        log::info("server: guild invitations {}", guild_world_invites_ ? "world-wide" : "in the zone only");
        return;
    }
    if (g.action != HailstoneGuildAction::Invite) return;
    // An invitation from another zone, asked as OP_GuildInvite's (Handle_OP_GuildInvite @0x4b9410) and answered
    // to the script.
    if (!client_option(game::ClientOption::GuildInvites)) {
        HailstoneGuild a;
        a.action = HailstoneGuildAction::Answer;
        a.name = g.name;
        a.guild_id = g.guild_id;
        a.value = kGuildInviteAutoDecline;
        send_zone(encode_hailstone_guild(a));
        system(543, {g.name, guild_.name_of(g.guild_id)}, 0x111);
        system(546, {}, 0x111);
        return;
    }
    guild_.inviter = g.name;
    guild_.invite_guild = g.guild_id;
    guild_.invite_rank = g.value;
    guild_.invite_ms = steady_ms();
    guild_.invite_world = true;
    system(534, {g.name, guild_.name_of(g.guild_id)});
}

// ---------------------------------------------------------------- raids

void GameConnection::raid_invite(const std::string& args) {
    // Cmd_raidinvite @0x4de470 → Send_OP_RaidInvite_0047d410.
    if (raid_.locked) return system(8869);
    if (raid_.active && !iequals(raid_.leader, character_)) return system(5073);
    if (raid_.members.size() >= 72) return system(5048);
    std::string who;
    std::string_view rest = args;
    who = game::next_cmd_arg(rest);
    if (who.empty()) {
        auto it = world_.spawns.find(world_.target_id);
        if (it == world_.spawns.end()) return system(5074);
        if (it->second.is_npc || it->second.is_corpse) return system(5075);
        if (it->first == world_.player_id) return system(5076);
        if (other_deity_team(it->second)) return system(395);
        who = base_name(it->second.name);
    }
    if (iequals(who, character_)) return system(5076);
    if (raid_.is_member(who)) return system(5077);
    if (group_.active && group_.has_member(who)) return system(12272);
    RaidGeneral g;
    g.action = raid_.active ? kRaidAddMember : kRaidInvite;
    g.player_name = who;
    g.leader_name = character_;
    send_zone(encode(g));
    system(5064, {who});
}

void GameConnection::raid_invite_expedition() {
    if (state_ != ConnectionState::InZone || !expedition_.info.assigned || raid_.locked ||
        (raid_.active && !iequals(raid_.leader, character_)) || raid_.members.size() >= 72) return;
    if (interface_locked()) return system(game::kStrInterfaceLocked, {}, game::kLockedColor);
    // CRaidWnd::WndNotification @0x72d180: invite each expedition member missing from the raid through the normal
    // raid invitation. No expedition invitation or local roster change is involved.
    for (const auto& member : expedition_.members)
        if (!member.name.empty() && !iequals(member.name, character_) && !raid_.is_member(member.name))
            raid_invite(member.name);
}

void GameConnection::raid_invite_shared_task() {
    if (state_ != ConnectionState::InZone || raid_.locked ||
        (raid_.active && !iequals(raid_.leader, character_)) || raid_.members.size() >= 72) return;
    if (interface_locked()) return system(game::kStrInterfaceLocked, {}, game::kLockedColor);
    // The same notification handles the shared task's roster through the ordinary raid invitation sender.
    for (const auto& name : tasks_.shared_members)
        if (!name.empty() && !iequals(name, character_) && !raid_.is_member(name)) raid_invite(name);
}

void GameConnection::answer_raid_invite(bool accept) {
    // CRaid::SendInviteResponse @0x47d700.
    if (raid_.inviter.empty()) return;
    if (accept && raid_.locked) return system(8869);
    RaidGeneral g;
    g.player_name = raid_.inviter;
    g.leader_name = character_;
    if (accept) {
        system(5079, {raid_.inviter});
        g.action = kRaidAccept;
    } else {
        system(5080, {raid_.inviter});
        g.action = kRaidInviteFail;
        g.parameter = 5081; // "%1 rejected your offer to join the raid." for the inviter
    }
    send_zone(encode(g));
    raid_.inviter.clear();
}

void GameConnection::raid_leave() {
    // CRaid::RemoveRaidMember @0x47d910 (/raiddisband is a tail call to it, Cmd_raiddisband @0x4de510; the raid
    // window's Leave button): the leader with a raid target removes the target (and the raid target is cleared),
    // anyone else leaves: {5, me, member} (EQEmu's RaidCommandDisband: leader_name is the member to remove).
    if (!raid_.active) return;
    RaidGeneral g;
    g.raid_id = raid_.id;
    g.action = kRaidDisband;
    g.player_name = character_;
    g.leader_name = character_;
    if (iequals(raid_.leader, character_) && !raid_.target.empty() && raid_.is_member(raid_.target)) {
        g.leader_name = raid_.target;
        raid_.target.clear();
    }
    send_zone(encode(g));
}

// ---------------------------------------------------------------- the chat server

std::optional<std::string> GameConnection::take_window_toggle() {
    auto t = std::move(window_toggle_);
    window_toggle_.reset();
    return t;
}

bool GameConnection::chat_server_connected() const { return chat_server_ && chat_server_->connected(); }

std::vector<std::string> GameConnection::chat_channels() const {
    return chat_server_ ? chat_server_->channels() : std::vector<std::string>{};
}

void GameConnection::send_chat_server_message(const std::string& text, bool via_world) {
    // SendChatServerMessage @0x5369e0. viaWorld: OP_ChannelMessage channel 20 through the zone (our name, the text;
    // EQEmu's world hands it to the chat server, which takes ";<n> text" and "[command"); else straight to the
    // chat server (UniversalChatProxy_SendCommand: the text, a leading '[' made ';', and an empty target), or
    // eqstr 4529 without one.
    if (via_world) {
        ChannelMessage m;
        m.sender = character_;
        m.channel = 20;
        m.message = text;
        send_zone(encode(m));
        return;
    }
    if (!chat_server_ || chat_server_->failed()) return system(4529); // Please wait until we reconnect you ...
    if (!chat_server_->send_mail_command(text)) system(4529);
}

void GameConnection::chat_server_command(const std::string& text) {
    // ChatServer_SendCommandText @0x4dfa50 (';' and '[' lines, /chat, /join): nothing: eqstr 13065; the text with a
    // '[' in front unless it has one, through the zone.
    if (text.empty()) return chat(ChatEvent::Kind::System, fmt_eq(strings_, 13065, {}), 0, 0x111);
    send_chat_server_message(text[0] == '[' ? text : "[" + text, true);
}

void GameConnection::numbered_channel(int n, const std::string& text) {
    // Cmd_1..Cmd_10 (@0x4ec160 with the number): without text "Usage: /%d <msg>" (colour 0x111); else the bad word
    // filter (the client language 0: ChatText_FilterProfanityAndSlur at skill 100) and ";%d %s" through the zone.
    if (text.empty()) return chat(ChatEvent::Kind::System, std::format("Usage: /{} <msg>", n), 0, 0x111);
    std::string said = game::filter_profanity_and_slur(text, 100, chat_filters_.mode(game::kFilterBadWord), chat_random_);
    send_chat_server_message(std::format(";{} {}", n, said), true);
}

void GameConnection::friends_command(const std::string& args) {
    // Cmd_friends @0x4f2240.
    if (!chat_server_ || chat_server_->failed()) {
        system(4529);
        return;
    }
    std::string_view rest = args;
    std::string who = game::next_cmd_arg(rest);
    if (who.empty()) {
        if (social_.friends.empty()) {
            system(12977);
            system(12978);
            return;
        }
        system(12979);
        chat(ChatEvent::Kind::System, "---------------");
        for (const auto& f : social_.friends) chat(ChatEvent::Kind::System, f);
        system(12980, {std::to_string(social_.friends.size())});
        return;
    }
    if (who.size() >= 0x30) return system(12981);
    bool known = std::any_of(social_.friends.begin(), social_.friends.end(), [&](const std::string& f) { return iequals(f, who); });
    if (known) {
        if (!chat_server_->send_command("buddy -" + who)) return system(4529);
        system(12982, {who});
    } else {
        if (!chat_server_->send_command("buddy " + who)) return system(4529);
        system(12984, {who});
    }
}

void GameConnection::ignore_command(const std::string& args) {
    // Cmd_ignore @0x4f63a0.
    if (!chat_server_ || chat_server_->failed()) {
        system(4529);
        return;
    }
    std::string_view rest = args;
    std::string who = game::next_cmd_arg(rest);
    if (who.empty()) {
        system(13240);
        for (const auto& i : social_.ignored) chat(ChatEvent::Kind::System, after_last_dot(i));
        return;
    }
    for (const auto& entry : social_.ignored) {
        if (iequals(after_last_dot(entry), who)) {
            if (!chat_server_->send_command("ignoreplayer -" + entry)) return system(4529);
            system(13241, {who});
            return;
        }
    }
    for (const auto& [id, s] : world_.spawns)
        if (s.is_npc && iequals(base_name(s.name), who)) return system(13242); // That person cannot be ignored.
    if (!chat_server_->send_command("ignoreplayer " + who)) system(4529);
}

void GameConnection::update_chat_server() {
    if (chat_server_info_ && state_ == ConnectionState::InZone) {
        // CEverQuest::ChatServerConnect @0x5135b0 keeps a connection to the same server and name.
        ChatServerInfo info = *chat_server_info_;
        // The dev stack may name a host this machine cannot reach (a
        // container name); the world server's host is the fallback.
        if (info.host == "localhost" || info.host.find('.') == std::string::npos) info.host = ticket_.world_host;
        if (!chat_server_ || chat_server_->failed() || !(chat_server_target_ == info.host + ":" + std::to_string(info.port) + "/" + info.character)) {
            if (!chat_server_) {
                IniFile ini;
                if (auto path = game_.find("eqclient.ini")) ini.load(*path);
                ChatServer::Options chat_options;
                chat_options.session = ucs_session_options(ini.get_int("Defaults", "ChatKeepAlive", 0) != 0);
                chat_server_ = std::make_unique<ChatServer>(std::move(chat_options));
            }
            chat_server_->connect(info);
            autojoined_ = false;
            chat_server_target_ = info.host + ":" + std::to_string(info.port) + "/" + info.character;
        }
        chat_server_info_.reset();
        social_.session_start_ms = social_.session_start_ms ? social_.session_start_ms : steady_ms();
    }
    if (!chat_server_) return;
    chat_server_->update();
    while (auto e = chat_server_->poll()) handle_chat_server_event(*e);
    autojoin_channels();
}

void GameConnection::handle_chat_server_event(const ChatServerEvent& e) {
    // HandleUniversalChatMessage @0x5215a0.
    using K = ChatServerEvent::Kind;
    switch (e.kind) {
    case K::Disconnected:
        // CEverQuest::ChatServerGiveTime @0x5382d0 deletes a proxy whose link is down (GetStatus @0x7ef820 == 0), and
        // ~UniversalChatProxy @0x427db0 frees the buddy list with it; the ignore list BeingIgnored @0x516a00 walks is
        // the proxy's too, so with no proxy nothing is ignored. Friends, ignores and mail all go with the session.
        social_.friends.clear();
        social_.ignored.clear();
        autojoined_ = false;
        handle_mail_event(e);
        break;
    case K::MailboxList:
        // The new authenticated proxy replaces the old per-session lists. EQEmu's friend rows were held until
        // this reply, so they arrive after the clear even though it sent them before the reply on the wire.
        social_.friends.clear();
        social_.ignored.clear();
        handle_mail_event(e);
        break;
    case K::Message: {
        // UniversalChatProxy::vf1's channel message (channel, sender, text, then "SPAM:<n>:"), sorted into the
        // channel number HandleUniversalChatMessage takes: -1 the server's own text (no channel), -4 a tell to us
        // and -3 a tell we sent ("$<name>"), n the joined channel n, -2 a channel not joined. The text then goes to
        // msg_new_text as channel 20 in the colour picked here (0x122; 0x123 + n, our own 0x13b + n; tells 0x101,
        // our echo 0x134). The original sends it through the zone instead when neither "SPAM:" came, nor the sender
        // is empty or us; EQEmu's chat server always sends "SPAM:0:".
        std::string sender = after_last_dot(e.sender);
        bool me = iequals(sender, character_);
        std::uint32_t color = 0x122;
        std::string line;
        auto chans = chat_server_->channels();
        if (e.channel.empty()) {
            line = e.text; // -1
        } else if (e.channel[0] == '$') {
            std::string to = after_last_dot(e.channel.substr(1));
            if (iequals(to, character_)) { // -4
                if (is_ignored(sender)) break;
                game::add_last_tell(social_, "[" + sender); // LastTell_Add
                social_.last_tell_from = "[" + sender;
                color = 0x101;
                line = fmt_eq(strings_, 6721, {sender, e.text});
            } else if (me) { // -3
                color = 0x134;
                line = fmt_eq(strings_, 6719, {to, e.text});
            } else {
                line = fmt_eq(strings_, 6720, {sender, to, e.text});
            }
        } else {
            std::string name = after_last_dot(e.channel);
            auto it = std::find_if(chans.begin(), chans.end(), [&](const std::string& c) { return iequals(c, name); });
            if (it == chans.end()) { // -2
                line = fmt_eq(strings_, 6723, {sender, "#" + name, e.text});
            } else {
                int n = static_cast<int>(it - chans.begin());
                std::string label = std::format("{}:{}", name, n + 1);
                color = 0x123 + static_cast<std::uint32_t>(n);
                if (sender.empty()) line = e.text;
                else if (is_ignored(sender)) break;
                else if (me) {
                    color = 0x13b + static_cast<std::uint32_t>(n);
                    line = fmt_eq(strings_, 6722, {label, e.text});
                } else {
                    line = fmt_eq(strings_, 6723, {sender, label, e.text});
                }
            }
        }
        if (!line.empty()) chat(ChatEvent::Kind::Other, line, 0, color);
        break;
    }
    case K::Channels:
        break;
    case K::Buddy: {
        auto it = std::find_if(social_.friends.begin(), social_.friends.end(),
                               [&](const std::string& f) { return iequals(f, e.text); });
        if (e.added && it == social_.friends.end()) social_.friends.push_back(e.text);
        if (!e.added && it != social_.friends.end()) social_.friends.erase(it);
        break;
    }
    case K::Ignore: {
        auto it = std::find_if(social_.ignored.begin(), social_.ignored.end(),
                               [&](const std::string& f) { return iequals(after_last_dot(f), after_last_dot(e.text)); });
        if (e.added && it == social_.ignored.end()) social_.ignored.push_back(e.text);
        if (!e.added && it != social_.ignored.end()) social_.ignored.erase(it);
        break;
    }
    case K::AnnounceJoin:
    case K::AnnounceLeave:
        chat(ChatEvent::Kind::Other, std::format("{} has {} channel {}.", after_last_dot(e.sender),
                                                 e.kind == K::AnnounceJoin ? "entered" : "left", e.channel));
        break;
    default:
        handle_mail_event(e); // net/game_connection_mail.cpp
        break;
    }
}

// ---------------------------------------------------------------- packets

void GameConnection::handle_world_social(Op op, const AppPacket& p) {
    switch (op) {
    case Op::GuildsList:
        if (auto list = decode_guilds_list(p)) {
            guild_.names.clear();
            for (auto& g : *list) guild_.names[g.id] = std::move(g.name);
            log::debug("guilds list: {} guilds", guild_.names.size());
        }
        break;
    case Op::SetChatServer2: {
        // Handle_OP_SetChatServer2 @0x4b82b0.
        chat_server_info_ = decode_chat_server(p);
        if (!chat_server_info_) log::warn("could not read OP_SetChatServer2 ({} bytes)", p.data.size());
        break;
    }
    default:
        break;
    }
}

void GameConnection::handle_social(Op op, const AppPacket& p) {
    const std::string me = character_;
    switch (op) {
    case Op::ChannelMessage:
        if (auto m = decode_channel_message(p)) handle_channel_message(*m);
        break;
    case Op::Emote:
        // Handle_OP_Emote @0x4b89f0.
        if (auto e = decode_emote(p)) chat(ChatEvent::Kind::Emote, e->text);
        break;
    case Op::WhoAllResponse:
        if (auto r = decode_who_response(p)) {
            // A server's client may replace the whole reply with its own lines (the NMS patch: 0x10198eb0).
            auto ext_lines = server_ext().who_lines(*this, *r);
            auto lines = ext_lines ? std::move(*ext_lines) : game::format_who_response(*r, strings_, false);
            for (auto& line : lines)
                chat(ChatEvent::Kind::System, std::move(line), 0, 281); // the Who filter
            last_who_ = std::move(*r);
            ++who_count_;
        } else {
            log::warn("could not decode OP_WhoAllResponse ({} bytes)", p.data.size());
        }
        break;
    case Op::RandomReply:
        // HandleWorldMessage case 0x106b (@0x4cf325): unless the roller is
        // ignored, eqstr 1086 and 1087 (colour 0x11f).
        if (auto r = decode_random_reply(p)) {
            bool ignored = std::any_of(social_.ignored.begin(), social_.ignored.end(),
                                       [&](const std::string& i) { return iequals(after_last_dot(i), r->name); });
            if (!ignored) {
                system(1086, {r->name}, 0x11f);
                system(1087, {std::to_string(r->low), std::to_string(r->high), std::to_string(r->result)}, 0x11f);
            }
        }
        break;
    case Op::Assist:
        // The server's answer: the assisted player's target.
        if (auto id = decode_assist(p); id && *id) {
            set_target(*id);
            if (social_.attack_on_assist && *id != world_.player_id) set_auto_attack(true);
        }
        break;
    case Op::GuildsList:
        handle_world_social(op, p);
        break;
    case Op::GuildMOTD:
    case Op::GetGuildMOTDReply:
        // Handle_OP_GuildMOTD @0x423a20: "GUILD MOTD: %1 - %2", printed each time; OP_GetGuildMOTDReply (the answer
        // to /getguildmotd) carries the same struct. The server sends OP_GuildMOTD on every zone in; it is shown
        // once a login and again when it changes (not the original's), the reply to /getguildmotd always.
        if (auto m = decode_guild_motd(p)) {
            const bool changed = m->motd != guild_.motd || m->set_by != guild_.motd_set_by;
            guild_.motd = m->motd;
            guild_.motd_set_by = m->set_by;
            if (!m->motd.empty() && (changed || op == Op::GetGuildMOTDReply)) system(468, {m->set_by, m->motd});
        }
        break;
    case Op::GuildMemberList:
        if (auto list = decode_guild_member_list(p)) {
            guild_.members.clear();
            for (const auto& m : list->members) {
                game::GuildMemberInfo g;
                g.name = m.name;
                g.level = m.level;
                g.class_ = m.class_;
                g.rank = m.rank;
                g.last_on = m.last_on;
                g.zone_id = m.zone_id;
                g.banker = (m.banker & 1) != 0; // EQEmu: banker + alt * 2
                g.alt = (m.banker & 2) != 0;
                g.public_note = m.public_note;
                g.tribute_on = m.tribute_enable != 0;
                g.trophy_on = m.trophy_enable != 0;
                g.donations = m.total_tribute;
                g.last_donation = m.last_tribute;
                g.tribute_flag = m.tribute_flag != 0;
                if (iequals(m.name, me)) guild_.rank = m.rank;
                guild_.members.push_back(std::move(g));
            }
        } else {
            log::warn("could not decode OP_GuildMemberList ({} bytes)", p.data.size());
        }
        break;
    case Op::GuildMemberAdd:
    case Op::GuildMemberDelete:
    case Op::GuildMemberLevel:
    case Op::GuildMemberPublicNote:
    case Op::SetGuildRank:
    case Op::GuildMemberRankAltBanker:
    case Op::GuildMemberDetails: {
        // EQEmu's live roster changes (net/notice_packets.h).
        auto c = decode_guild_roster_change(p);
        if (!c || !guild_.in_guild() || c->guild_id != guild_.guild_id) break;
        using K = GuildRosterChange;
        auto it = std::find_if(guild_.members.begin(), guild_.members.end(),
                               [&](const game::GuildMemberInfo& m) { return iequals(m.name, c->name); });
        if (c->kind == K::Delete) {
            if (it != guild_.members.end()) guild_.members.erase(it);
            break;
        }
        if (it == guild_.members.end()) {
            if (c->kind != K::Add) break;
            guild_.members.push_back({});
            it = guild_.members.end() - 1;
            it->name = c->name;
        }
        auto& m = *it;
        switch (c->kind) {
        case K::Add:
            m.level = c->level;
            m.class_ = c->class_;
            m.rank = c->rank;
            m.zone_id = static_cast<std::uint16_t>(c->zone_id);
            m.last_on = c->last_on;
            break;
        case K::Level: m.level = c->level; break;
        case K::Note: m.public_note = c->note; break;
        case K::Rank:
            m.rank = c->rank;
            m.alt = c->alt;
            m.banker = c->banker;
            if (iequals(c->name, character_)) guild_.rank = m.rank;
            break;
        case K::Details:
            m.zone_id = c->offline ? 0 : static_cast<std::uint16_t>(c->zone_id);
            m.last_on = c->last_on;
            break;
        default: break;
        }
        break;
    }
    case Op::GuildMemberUpdate:
        if (auto u = decode_guild_member_update(p)) {
            for (auto& m : guild_.members)
                if (iequals(m.name, u->member)) {
                    m.zone_id = u->zone_id;
                    m.last_on = u->last_seen;
                }
        }
        break;
    case Op::GuildUpdate:
        // HandleWorldMessage_Guild @0x425d30, OP_GuildUpdate: action 5 (@0x426220): 0x7d95e0(+0x50 rank, +0x54
        // permission, byte +0x58) sets or clears the rank's permission bit (EQEmu sends every rank's permissions
        // this way on zoning in, Client::SendGuildRanks, and on a change); 4: 0x7d9520(+0x50 rank, +0x54 name)
        // names a rank; 0 and 1: the guild's web page and chat channel (+0x50).
        if (auto u = decode_guild_update(p)) {
            if (u->action == 5) {
                // Source SendGuildRanks tags the new guild before its own-member GuildID appearance. Keep
                // that table separate until membership confirms it; a foreign table never grants permission.
                if (u->guild_id && u->guild_id != 0xffffffff && u->guild_id != guild_.guild_id) {
                    if (!pending_guild_permissions_ || pending_guild_permissions_->guild_id != u->guild_id)
                        pending_guild_permissions_ = PendingGuildPermissions{u->guild_id, {}, {}};
                    pending_guild_permissions_->permissions.set(static_cast<std::int32_t>(u->rank),
                                                                 static_cast<std::int32_t>(u->permission), u->on);
                } else if (guild_.in_guild() && (!u->guild_id || u->guild_id == guild_.guild_id)) {
                    if (u->guild_id) pending_guild_permissions_.reset();
                    guild_.permissions.set(static_cast<std::int32_t>(u->rank), static_cast<std::int32_t>(u->permission), u->on);
                }
            } else if (u->action == 4) {
                // SendGuildRankNames has no tag, and follows that guild's tagged permission rows. This assumes a
                // staged table is confirmed by the GuildID appearance in the same burst; one never confirmed would
                // take the current guild's later untagged names, so a zone change drops it (the zone-in resends
                // the current guild's rows and names) and a tagged row for the current guild drops it too.
                if (pending_guild_permissions_) {
                    if (u->rank < pending_guild_permissions_->rank_names.size())
                        pending_guild_permissions_->rank_names[u->rank] = std::move(u->text);
                } else if (guild_.in_guild() && u->rank < guild_.rank_names.size()) {
                    guild_.rank_names[u->rank] = std::move(u->text);
                }
            } else if (u->action == 0) {
                guild_.url = std::move(u->text);
            } else if (u->action == 1) {
                guild_.channel = std::move(u->text);
            }
        }
        break;
    case Op::ConsentResponse:
    case Op::DenyResponse:
        // Handle_OP_DenyResponse @0x4ba520 (both opcodes): to the corpse's owner "You have given / denied %1
        // permission to drag your corpse in %2." (1427 / 1428); to anyone else, unless the owner is ignored
        // (CEverQuest::BeingIgnored), "You have been given / denied permission to drag %1's corpse in %2."
        // (2080 + the consent's context tip / 2103); colour 0x111.
        if (auto c = decode_consent_response(p)) {
            if (iequals(c->owner_name, me)) {
                system(c->permission ? 1427 : 1428, {c->grant_name, c->zone}, 0x111);
            } else if (!is_ignored(c->owner_name)) {
                system(c->permission ? 2080 : 2103, {c->owner_name, c->zone}, 0x111);
                if (c->permission) context_tip(0x1c);
            }
        }
        break;
    case Op::GuildInvite:
        // Handle_OP_GuildInvite @0x4b9410: "%1 is interested in having you join %2." and a 10 s prompt.
        if (auto c = decode_guild_command(p)) {
            // first, for a packet naming us (_stricmp @0x4b9440): the guild invitation's context tip
            if (iequals(c->other_name, me)) context_tip(0x19);
            if (guild_.in_guild() && c->guild_id == guild_.guild_id) {
                guild_.rank = c->rank; // a rank change for a member
                break;
            }
            if (c->guild_id != guild_.guild_id && !client_option(game::ClientOption::GuildInvites)) {
                // Options/GuildInvites (0xde0bb4) off: another guild's invitation is answered at once with 0xb
                // (EQEmu: >= GUILD_INVITE_DECLINE, declined), then 0x21f (inviter, guild) and 0x222, 0x111.
                GuildInviteAccept a;
                a.inviter = c->my_name;
                a.new_member = character_;
                a.response = kGuildInviteAutoDecline;
                a.guild_id = c->guild_id;
                send_zone(encode(a));
                system(543, {c->my_name, guild_.name_of(c->guild_id)}, 0x111);
                system(546, {}, 0x111);
                break;
            }
            guild_.inviter = c->my_name;
            guild_.invite_guild = c->guild_id;
            guild_.invite_rank = c->rank;
            guild_.invite_ms = steady_ms();
            guild_.invite_world = false;
            system(534, {c->my_name, guild_.name_of(c->guild_id)});
        }
        break;
    case Op::RaidUpdate:
        if (auto u = decode_raid_update(p)) {
            const auto& g = u->general;
            switch (g.action) {
            case kRaidAddMember: {
                // CRaid::AddRaidMember @0x47e4e0: "%1 joined the raid."
                bool had = raid_.active && !raid_.members.empty();
                raid_.active = true;
                game::RaidMember m;
                m.name = g.player_name;
                m.group = g.parameter;
                m.class_ = u->class_;
                m.level = u->level;
                m.group_leader = u->group_leader != 0;
                // EQEmu moves a member between groups with a remove/add pair, retaining its raid delegates.
                m.main_assist = std::any_of(leadership_.raid_assist_names.begin(), leadership_.raid_assist_names.end(),
                                           [&](const std::string& name) { return iequals(name, m.name); });
                m.marker = std::any_of(leadership_.raid_marker_names.begin(), leadership_.raid_marker_names.end(),
                                      [&](const std::string& name) { return iequals(name, m.name); });
                if (raid_.add(m) && had && !iequals(m.name, me)) system(5059, {m.name});
                break;
            }
            case kRaidRemoveMember:
                // CRaid::DeleteRaidMember @0x47e6c0: "%1 has left the raid."
                // A group move uses this same remove before its add. Delegate-clear packets revoke actual raid
                // roles; Disband/NoRaid below clear the local character's raid state.
                if (raid_.remove(g.player_name) && !iequals(g.player_name, me)) system(5063, {g.player_name});
                break;
            case kRaidDisband:
                if (raid_.active) system(5062); // You were removed from the raid.
                raid_.clear();
                reset_raid_leadership();
                break;
            case kRaidCreate:
                ++raid_.generation;
                raid_.active = true;
                raid_.leader = g.leader_name.empty() ? g.player_name : g.leader_name;
                raid_.id = g.raid_id; // CRaid::RaidCreated: raidId from +0x44
                break;
            case kRaidNoRaid:
                raid_.clear();
                reset_raid_leadership();
                break;
            case kRaidLock:
                raid_.locked = true;
                break;
            case kRaidUnlock:
                raid_.locked = false;
                break;
            case kRaidInviteSent:
                if (client_option(game::ClientOption::RaidInviteConfirm)) {
                    // Handle_OP_RaidUpdate 0x14 with Options/RaidInviteConfirm (0xde0d2d) on: the invitation is
                    // kept (flags 2) without CRaid_HandleInvite's checks, 0x2c4 with the inviter (colour 0xd) and
                    // CRaid's confirmation dialog with the same line (100 s); its answer is SendInviteResponse's.
                    raid_.inviter = g.leader_name;
                    std::string text = fmt_eq(strings_, 708, {g.leader_name});
                    system(708, {g.leader_name}, 0xd);
                    Confirmation c{kConfirmRaidInvite, std::move(text), {}};
                    c.timeout_ms = 100000;
                    ask(std::move(c));
                    break;
                }
                // CRaid_HandleInvite @0x47dc30.
                raid_.inviter = g.leader_name;
                system(5065, {g.leader_name});
                system(5067);
                break;
            case kRaidSetLeaderAbilities:
            case kRaidSetLeaderData:
                // Leadership_SetRaidAbilityRanks (14) / Leadership_ApplyRaidLeadershipUpdate (15): the raid leader's
                // ranks.
                set_raid_ability_ranks(u->leadership);
                break;
            case kRaidSetLeader:
                // CRaid_HandleSetLeader @0x47e850: CRaid::ChangeLeadership, the raid leader's ranks, then "You are now
                // the leader of the raid." (5069) or "%1 is now the leader of your raid." (5070), colour 0x111, every
                // time (EQEmu sends it at every zone-in).
                raid_.leader = g.leader_name;
                set_raid_ability_ranks(u->leadership);
                if (iequals(g.leader_name, me)) system(5069, {}, 0x111);
                else system(5070, {g.leader_name}, 0x111);
                break;
            case kRaidSetLootType:
                // CRaid::HandleSetLootType @0x47d270: the type (the parameter); types 1, 2 and 4 clear the looters.
                raid_.loot_type = g.parameter;
                if (g.parameter == 1 || g.parameter == 2 || g.parameter == 4) raid_.looters.clear();
                break;
            case kRaidAddLooter:
                // CRaid::HandleAddLooter @0x47c230: the name at +0x48 into the first free of 19 looters.
                if (raid_.looters.size() < 19) raid_.looters.push_back(g.leader_name);
                break;
            case kRaidRemoveLooter:
                // CRaid::HandleRemoveLooter @0x47d2b0 (the name at +0x48).
                std::erase_if(raid_.looters, [&](const std::string& n) { return iequals(n, g.leader_name); });
                break;
            case kRaidSetMotd:
                // Handle_OP_RaidUpdate 0x23: the MOTD (1023 characters) and "Raid MOTD:  %1" (6349, colour 0x111).
                raid_.motd = u->motd.substr(0, 1023);
                system(6349, {raid_.motd}, 0x111);
                break;
            case kRaidSetNote:
                // Handle_OP_RaidUpdate 0x24: the member named at +0x48 (CRaid::FindPlayerIndex) takes the note
                // (CRaidWnd_SetMemberNote: its note list row, and the note input when that row is selected).
                if (game::RaidMember* m = raid_.find(g.leader_name)) m->note = u->note;
                break;
            default:
                log::debug("raid update action {}", g.action);
                break;
            }
        }
        break;
    default:
        break;
    }
}

} // namespace hailstone::net
