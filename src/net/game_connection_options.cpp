// GameConnection: the client option commands the connection keeps (game/option_commands.h has the rules): /quit and
// /exit, /safelock, /charinfo, /log and /secondslog, /netstats, the tracking commands and /invwinlabels. The commands
// that act on the app (windows, chat windows, display and sound) are handed to it (set_client_commands;
// app/client_app_commands.cpp).

#include <ctime>
#include <cstdio>

#include "core/log.h"
#include "core/server_profile.h"
#include "game/item_links.h"
#include "game/option_commands.h"
#include "game/slash_commands.h"
#include "game/zone_names.h"
#include "net/game_connection.h"
#include "net/packet_io.h"
#include "platform/process.h"

namespace hailstone::net {

namespace {

std::string word(const game::EqStrings& s, std::uint32_t id) { return s.format(id, {}); }

} // namespace

bool GameConnection::option_command(std::string_view h, const std::string& args) {
    using O = game::ClientOption;
    // Cmd_resetwindows @0x4eae80 and Cmd_reverb @0x4e7b10 do nothing.
    if (h == "Cmd_resetwindows" || h == "Cmd_reverb") return true;
    if (h == "Cmd_quit" || h == "Cmd_exit") {
        quit_command(h == "Cmd_exit", args);
        return true;
    }
    if (h == "Cmd_safelock") {
        // Cmd_safelock @0x4ead80.
        for (std::uint32_t id : game::safelock_command(safelock_, args)) system(id);
        return true;
    }
    if (h == "Cmd_charinfo") {
        // Cmd_charinfo @0x4df330: the bind points' full zone names (PcBase_GetBindZoneID 0..4).
        if (!profile_) return true;
        std::vector<std::uint32_t> zones;
        for (const auto& b : profile_->binds) zones.push_back(b.zone_id);
        show_chat(game::charinfo_line(zones, [](std::uint32_t id) { return game::zone_long_name(id); }));
        return true;
    }
    if (h == "Cmd_log") {
        log_command(args);
        return true;
    }
    if (h == "Cmd_secondslog") {
        // Cmd_secondslog @0x4e8350: "off" / "on" / toggle; nothing reads the flag.
        switch (game::off_on_toggle(args, "off", "on")) {
        case game::Switch::Off: seconds_log_ = false; break;
        case game::Switch::On: seconds_log_ = true; break;
        case game::Switch::Toggle: seconds_log_ = !seconds_log_; break;
        }
        return true;
    }
    if (h == "Cmd_netstats") {
        netstats_command();
        return true;
    }
    if (h == "Cmd_invwinlabels") {
        // Cmd_invwinlabels @0x4e9a90: "off" / "on" / toggle, said (colour 0x111) and saved as InvWinLabels.
        int v = client_options_.get(O::InvWinLabels);
        switch (game::off_on_toggle(args, "off", "on")) {
        case game::Switch::Off: v = 0; break;
        case game::Switch::On: v = 1; break;
        case game::Switch::Toggle: v = v != 1; break;
        }
        client_options_.set(O::InvWinLabels, v);
        show_chat(v == 1 ? "Inventory Window Labels *ON*" : "Inventory Window Labels *OFF*", 0x111);
        save_client_option(O::InvWinLabels);
        return true;
    }
    // ---- tracking (the tracking window: net/game_connection_tracking.cpp)
    if (h == "Cmd_trackplayers") {
        // Cmd_trackplayers @0x4e5410: on / off / group (13148), 13140..13143; the window refiltered either way.
        std::optional<int> v;
        if (game::same_no_case(args, word(strings_, 12947))) v = 1;
        else if (game::same_no_case(args, word(strings_, 12946))) v = 0;
        else if (game::same_no_case(args, word(strings_, 13148))) v = 2;
        if (v) {
            client_options_.set(O::TrackPlayers, *v);
            system(*v == 1 ? 13140 : *v == 0 ? 13141 : 13142);
            save_client_option(O::TrackPlayers);
        } else {
            system(13143);
        }
        tracking_from_options();
        return true;
    }
    if (h == "Cmd_trackpets") {
        track_toggle_command(O::TrackPets, args);
        return true;
    }
    if (h == "Cmd_trackmercs") {
        track_toggle_command(O::TrackMercs, args);
        return true;
    }
    if (h == "Cmd_trackfilter" || h == "Cmd_tracksort") {
        // Rangers only (13155); nothing without the character (no local PcClient).
        if (!profile_) return true;
        if (profile_->class_ != 4) return system(game::kStrRangersOnly), true;
        auto eqstr = [this](std::uint32_t id) { return word(strings_, id); };
        if (h == "Cmd_trackfilter") {
            // Cmd_trackfilter @0x4f4cd0: one con colour's bit toggled (not saved), "You will (NOT) see %1 NPCs when
            // tracking." (13163 / 13164); an unknown colour 13162. The window is refiltered either way.
            if (auto c = game::trackfilter_choice(args, eqstr)) {
                auto mask = static_cast<std::uint32_t>(client_options_.get(O::TrackFilterType));
                bool was = (mask & c->bit) != 0;
                mask = was ? mask & ~c->bit : mask | c->bit;
                client_options_.set(O::TrackFilterType, static_cast<int>(mask));
                system(was ? 13163 : 13164, {word(strings_, c->name)});
            } else {
                system(13162);
            }
            tracking_from_options();
            return true;
        }
        // Cmd_tracksort @0x4f4930: the sort type, its message, TrackSortType saved and the list resorted; an unknown
        // type 13154.
        auto c = game::tracksort_choice(args, eqstr);
        if (!c) return system(13154), true;
        client_options_.set(O::TrackSortType, static_cast<int>(c->type));
        system(c->message);
        save_client_option(O::TrackSortType);
        set_track_sort(c->type);
        return true;
    }
    if (h == "Cmd_stoptracking") {
        // Cmd_stoptracking @0x4e5770: while tracking is the activity (0xddfccc == 0x35), Send_OP_TrackTarget(-1, 0)
        // (OP_TrackTarget {0}, the activity cleared) and OP_TrackTarget {0} again; then the window closes.
        if (tracking_.tracked != 0 && tracking_in_zone()) {
            for (int i = 0; i < 2; ++i) {
                ByteWriter w;
                w.le<std::uint32_t>(0);
                send_zone(make_packet(Op::TrackTarget, std::move(w)));
            }
        }
        tracking_.tracked = 0;
        close_tracking();
        return true;
    }
    return false;
}

void GameConnection::track_toggle_command(game::ClientOption id, const std::string& args) {
    // Cmd_trackpets @0x4e5550 / Cmd_trackmercs @0x4e5640: on / off saved as TRUE / FALSE, else the format line; the
    // window refiltered either way.
    const bool pets = id == game::ClientOption::TrackPets;
    std::optional<int> v;
    if (game::same_no_case(args, word(strings_, 12947))) v = 1;
    else if (game::same_no_case(args, word(strings_, 12946))) v = 0;
    if (v) {
        client_options_.set(id, *v);
        system(pets ? (*v ? 4108 : 4109) : (*v ? 4112 : 4113));
        save_client_option(id);
    } else {
        system(pets ? 4110 : 4114);
    }
    tracking_from_options();
}

void GameConnection::tracking_from_options() {
    // The globals the tracking window filters and sorts by (0xddfcb0 .. 0xddfcbc), then DoTrackFilter.
    using O = game::ClientOption;
    tracking_.filter.players = client_options_.get(O::TrackPlayers);
    tracking_.filter.pets = client_options_.on(O::TrackPets);
    tracking_.filter.mercs = client_options_.on(O::TrackMercs);
    tracking_.filter.con_mask = static_cast<std::uint32_t>(client_options_.get(O::TrackFilterType));
    tracking_.sort_order = static_cast<std::uint32_t>(client_options_.get(O::TrackSortType));
    track_filter_all();
}

// ---------------------------------------------------------------- /quit, /exit

void GameConnection::quit_command(bool exit, const std::string& args) {
    // Cmd_quit @0x4e63a0 / Cmd_exit @0x4e64c0: only without an argument. The original sends its position history
    // (Send_OP_FloatListThing) and saves (Save 0x22 / 0x23: this client keeps its settings as they change, and EQEmu
    // saves the character when the connection closes); with the interface locked 12095; a player in state 0x66 stays.
    // Then the connection closes and the game state becomes 0xff (the client unloads). /exit's free-to-play upsell
    // page (FreeToPlayClient, membership below 2) is not opened: EQEmu accounts have no such membership here.
    if (!args.empty()) return;
    if (interface_locked()) return system(game::kStrInterfaceLocked);
    if (pstatus_.stand_state == 0x66) return;
    log::info("*** DISCONNECTING: {} command received.", exit ? "Exit" : "Quit");
    send_position_history(true);
    quit_request_ = exit ? QuitRequest::Exit : QuitRequest::Quit;
}

// ---------------------------------------------------------------- /log

void GameConnection::log_command(const std::string& args) {
    // Cmd_log @0x4fc230: the name back to "eqlog", then on / off / toggle; starting takes the second word as the
    // name. The file is the character's <name>.txt (the original's .\Logs\<name>_<character>_<server>.txt); Log is
    // saved (TRUE / FALSE). Colour 0x111.
    auto c = game::log_command(args, word(strings_, 12946), word(strings_, 12947),
                               client_options_.on(game::ClientOption::Log));
    client_options_.set(game::ClientOption::Log, c.on ? 1 : 0);
    open_chat_log(c.name);
    if (!c.on) close_chat_log();
    system(c.on ? game::kStrLogOn : game::kStrLogOff, {c.name}, 0x111);
    save_client_option(game::ClientOption::Log);
}

void GameConnection::open_chat_log(const std::string& name) {
    // Cmd_log_BuildLogFilePrefix @0x4f5d40: the path from the name and the character (here its folder).
    CharacterFiles files = own_files();
    std::string file = sanitize_file_name(name);
    chat_log_path_ = files.empty() || file.empty() ? std::filesystem::path() : files.dir / (file + ".txt");
}

void GameConnection::write_chat_log(const std::string& text) {
    // CEverQuest::OutputTextToLog @0x5130c0 after __CleanItemTags: an item link is its text.
    std::string clean;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t a = text.find('\x12', at);
        std::size_t b = a == std::string::npos ? std::string::npos : text.find('\x12', a + 1);
        if (b == std::string::npos) {
            clean += text.substr(at);
            break;
        }
        clean += text.substr(at, a - at);
        std::string tag = text.substr(a + 1, b - a - 1);
        auto link = game::parse_item_link(tag);
        clean += link ? link->text : tag;
        at = b + 1;
    }
    if (!chat_log_ || std::ferror(chat_log_.get()) || chat_log_open_path_ != chat_log_path_) {
        // (A file that failed is opened again: the folder may have gone, or the disk filled.)
        chat_log_.reset();
        std::error_code ec;
        std::filesystem::create_directories(chat_log_path_.parent_path(), ec);
        chat_log_.reset(platform::open_c_file(chat_log_path_, "ab"));
        chat_log_open_path_ = chat_log_path_;
    }
    if (!chat_log_) return;
    // Flushed each line, as the closing write was: a log parser reading the file sees the line at once.
    const std::string line = game::log_line(std::time(nullptr), clean);
    std::fwrite(line.data(), 1, line.size(), chat_log_.get());
    std::fflush(chat_log_.get());
}

void GameConnection::CloseChatLog::operator()(std::FILE* f) const noexcept {
    if (f) std::fclose(f);
}

void GameConnection::close_chat_log() {
    chat_log_.reset();
    chat_log_open_path_.clear();
}

// ---------------------------------------------------------------- /netstats

void GameConnection::netstats_command() {
    // Cmd_netstats @0x4e6a70: the game connection's statistics (UdpConnection::GetStats): the stat request round
    // trips (net/session.h; the master ping is the last one here, docs/unknowns.md), the reliable channel's round
    // trip, and the delivered shares from the counts of the server's last stat reply (1 before one came).
    auto stats = state_ == ConnectionState::InZone ? zone_session_stats() : world_session_stats();
    game::NetStats n;
    if (stats) {
        n.master = stats->ping_last;
        n.average = stats->ping_average;
        n.low = stats->ping_low;
        n.high = stats->ping_high;
        n.last = stats->ping_last;
        n.reliable_average = stats->rtt_ms;
        if (stats->sync_our_sent)
            n.sent_success = static_cast<float>(static_cast<double>(stats->sync_their_received) /
                                                static_cast<double>(stats->sync_our_sent));
        if (stats->sync_their_sent)
            n.received_success = static_cast<float>(static_cast<double>(stats->sync_our_received) /
                                                    static_cast<double>(stats->sync_their_sent));
    }
    for (auto& l : game::netstats_lines(n)) show_chat(std::move(l.text), l.color);
}

} // namespace hailstone::net
