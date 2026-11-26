#pragma once

// The plugin hook bus: the places compiled plugins (plugin/native_plugins.h), MacroQuest Lua scripts (mq/) and the
// client itself can watch or change what the client does. Everything that registers a hook is an owner; removing the
// owner removes all of its hooks, so a plugin or script that unloads leaves nothing behind.
//
// The events follow MacroQuest's plugin callbacks: OnPulse, SetGameState, OnBeginZone / OnEndZone / OnZoned,
// OnAddSpawn / OnRemoveSpawn, OnIncomingChat, OnUpdateImGui, and commands (AddCommand). The packet hooks are this
// client's own: every application packet to and from the world and zone servers passes them and can be changed or
// dropped. What an owner may hook is the server policy's business (plugin/policy.h); the bus itself checks nothing.
//
// Single threaded: hooks run on the client's thread, in registration order. A hook may add or remove hooks (its own
// or other owners') while running; removed hooks stop at once, added ones run from the next event.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/packet.h"

namespace hailstone::plugin {

enum class Stream : std::uint8_t { World, Zone };
enum class Direction : std::uint8_t { In, Out };
// What a hook does with the event: let it go on to the next hook and the client, or stop it here.
enum class Flow : std::uint8_t { Continue, Drop };
// MacroQuest's zoning callbacks: OnBeginZone (the zone change started), OnEndZone (the new zone's data is in), OnZoned.
enum class ZonePhase : std::uint8_t { Begin, End, Zoned };

// MacroQuest's GAMESTATE_* values (SetGameState).
inline constexpr int kGameStatePreCharSelect = -1, kGameStateCharSelect = 1, kGameStateCharCreate = 2,
                     kGameStateInGame = 5, kGameStateLoggingIn = 253, kGameStateUnloading = 255;

// A chat line on its way to the chat window: the text and colour may be changed.
struct ChatLine {
    std::string text;
    std::uint32_t color = 0; // 0xRRGGBB, 0 the system colour
    int kind = 0;            // net::ChatEvent::Kind
    // Whose line it is: a script's or plugin's (written while its code ran: mq.print, /echo, WriteChatf, a command's
    // own reply) or not (the server's, the client's, the player's). Such a line runs another script's mq.event
    // handler with no more than `origin_caps` (plugin::caller_caps() when it was written), and compiled plugins'
    // OnIncomingChat and OnFilterChat do not get it: it is not incoming chat, and its text could be anything.
    bool from_code = false;
    std::uint32_t origin_caps = ~0u;
};

class Hooks {
public:
    using Owner = std::uint32_t;
    static constexpr Owner kClient = 0; // the client's own hooks

    Hooks();
    ~Hooks();
    Hooks(const Hooks&) = delete;
    Hooks& operator=(const Hooks&) = delete;

    Owner new_owner(std::string name);
    // An owner for a plugin's or a script's code (a guest): it may not take a name the client answers itself
    // (reserved_command), so it cannot shadow /say, /tell, /lua or /plugin.
    Owner new_guest(std::string name);
    // Removes every hook and command of `owner`.
    void remove(Owner owner);
    // Removes one command of `owner` (MQ's unbind).
    void remove_command(Owner owner, std::string_view name);
    const std::string& owner_name(Owner owner) const;

    // ---- registration
    using PacketFn = std::function<Flow(net::AppPacket&)>;
    // `opcode` 0 = every opcode.
    void on_packet(Owner owner, Stream stream, Direction dir, std::uint16_t opcode, PacketFn fn);
    void on_chat(Owner owner, std::function<Flow(ChatLine&)> fn);
    // A typed or scripted line before the command interpreter: may rewrite it (MacroQuest's ${} parsing) or take it.
    void on_command_line(Owner owner, std::function<Flow(std::string& line)> fn);
    // A slash command ("/name", case-insensitive). The latest registration of a name wins, over the client's own
    // commands too; removing it brings back the one before. A guest's registration of a reserved name is refused
    // (false): the game's commands and the client's own stay the client's.
    // `required`: the capabilities (plugin/policy.h) a plugin or script needs to run it; the command filter checks.
    bool add_command(Owner owner, std::string name, std::function<void(std::string_view args)> fn,
                     std::uint32_t required = 0);
    // Whether `name` is the client's: a word the game's command table takes (game/slash_commands.h, abbreviations
    // and GM commands included: "/g" is /gsay), or a command an owner that is not a guest registered (/lua, /plugin,
    // MQ's /echo ...).
    bool reserved_command(std::string_view name) const;
    void on_pulse(Owner owner, std::function<void()> fn);
    void on_game_state(Owner owner, std::function<void(int state)> fn);
    void on_zone(Owner owner, std::function<void(ZonePhase phase)> fn);
    void on_spawn(Owner owner, std::function<void(std::uint32_t spawn_id, bool added)> fn);
    void on_imgui(Owner owner, std::function<void()> fn);

    // ---- raising (the client)
    bool has_packet_hooks(Stream stream, Direction dir) const;
    Flow packet(Stream stream, Direction dir, net::AppPacket& p);
    Flow chat(ChatLine& line);
    // Runs the line filters, then checks policy for the matched registration before invoking it. True when the
    // line was taken, including a denial, so a denied registration cannot fall through to an RoF2 abbreviation.
    // `line` holds the rewritten text when it was not taken.
    bool command(std::string& line, const std::function<bool(std::string_view name)>& allow = {});
    bool has_command(std::string_view name) const;
    // The owner of the latest live registration of `name`, and whether it is a guest (a plugin's or script's).
    std::optional<Owner> command_owner(std::string_view name) const;
    bool is_guest(Owner owner) const;
    // The latest registration's `required` (0 when there is none).
    std::uint32_t command_required(std::string_view name) const;
    std::vector<std::string> commands() const;
    // Changes when a command is registered or removed, including an add-on unloading.
    std::uint32_t command_catalog_serial() const;
    void pulse();
    void game_state(int state);
    void zone(ZonePhase phase);
    void spawn(std::uint32_t spawn_id, bool added);
    bool has_spawn_hooks() const;
    void imgui();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace hailstone::plugin
