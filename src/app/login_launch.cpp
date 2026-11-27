#include "app/login_launch.h"

#include <charconv>
#include <stdexcept>

#include "core/ascii_case.h"

namespace hailstone::app {

bool consume_login_launch_arg(LoginLaunch& launch, std::span<const std::string_view> args, std::size_t& at,
                              const TakeEnv& env) {
    if (at >= args.size()) return false;
    const auto arg = args[at];
    if (iequals(arg, "patchme")) return true;
    auto value = [&]() -> std::string_view {
        if (at + 1 >= args.size() || args[at + 1].starts_with("--"))
            throw std::runtime_error("Launcher option needs a value.");
        return args[++at];
    };
    const bool native_ticket = istarts_with(arg, "/ticket:");
    if (arg == "--ticket" || arg == "--ticket-env" || native_ticket) {
        if (launch.token_requested) throw std::runtime_error("Supply only one login ticket.");
        launch.token_requested = true;
        std::string_view ticket;
        std::optional<std::string> from_env;
        if (native_ticket) {
            ticket = arg.substr(8);
            // RoF2 strips one trailing '"' (InitGameInstance @005fe56a, after its strncpy of 0x3f bytes): a launcher
            // that quotes the whole switch leaves it on the value.
            if (ticket.ends_with('"')) ticket.remove_suffix(1);
        } else if (arg == "--ticket-env") {
            const std::string name(value());
            if (name.empty() || name.find('=') != std::string::npos || name.find('\0') != std::string::npos)
                throw std::runtime_error("--ticket-env needs an environment variable name.");
            from_env = env(name);
            if (from_env) ticket = *from_env;
        } else ticket = value();
        // Deliberate change from RoF2: its slot holds 63 bytes (InitGameInstance 0x5fdec0 strncpy 0x3f) and a longer
        // value is cut short; Hailstone rejects it rather than send a truncated ticket. The explicit transports
        // accept larger opaque tickets as an extension.
        const bool too_long = native_ticket && ticket.size() > 63;
        const bool valid = net::login_codec::valid_login_token(ticket);
        if (valid && !too_long) launch.ticket.assign(ticket);
        if (from_env) core::wipe(*from_env);
        if (too_long)
            throw std::runtime_error("/ticket: accepts at most 63 bytes. Use --ticket for a larger ticket.");
        if (!valid) throw std::runtime_error("Login ticket is empty, contains a NUL, or exceeds the supported size.");
        return true;
    }
    if (arg == "--world") {
        if (!launch.world.empty()) throw std::runtime_error("Supply only one launcher world.");
        const auto world = value();
        if (world.empty() || world.size() > 256 || world.find('\0') != std::string_view::npos)
            throw std::runtime_error("Launcher world is empty or too long.");
        launch.world.assign(world);
        return true;
    }
    // RoF2's /name:<char> (InitGameInstance @005fe5e6, strncpy 0x3f to 0x00e66738): StartNetworkGame selects that
    // character and enters the world without a click (@0053c0a5). Hailstone's --character does the same.
    if (istarts_with(arg, "/name:")) {
        if (!launch.character.empty()) throw std::runtime_error("Supply only one /name: character.");
        const auto name = arg.substr(6);
        if (name.empty() || name.size() > 63 || name.find('\0') != std::string_view::npos)
            throw std::runtime_error("/name: needs a character name of at most 63 bytes.");
        launch.character.assign(name);
        return true;
    }
    // RoF2's /server:<host> (@005fe5ba) writes the world host into EQLogin +0x58c, which eqmain may overwrite. The
    // world's address comes from the login server's play reply here, so the switch is taken and ignored.
    if (istarts_with(arg, "/server:")) return true;
    return false;
}

void resolve_login_launch(const LoginLaunch& launch, std::string& user, std::string& password,
                          std::string& account, bool viewer) {
    if (launch.token_requested) {
        user.clear();
        core::wipe(password);
        account.clear();
    }
    if (launch.token_requested && viewer)
        throw std::runtime_error("A login ticket is for the playable client, not a viewer.");
    if (!launch.world.empty() && !launch.token_requested && user.empty() && password.empty() && account.empty())
        throw std::runtime_error("--world needs a ticket, --user, or --account login.");
}

std::vector<std::string> unknown_argument_warnings(const LoginLaunch& launch, const std::vector<std::string>& unknown) {
    std::vector<std::string> out;
    if (unknown.empty()) return out;
    if (launch.token_requested) {
        out.push_back(std::to_string(unknown.size()) + (unknown.size() == 1 ? " unknown argument" : " unknown arguments") +
                      " ignored (not shown while a login ticket is in use)");
        return out;
    }
    for (const auto& a : unknown) out.push_back("unknown argument: " + a);
    return out;
}

ServerProfile token_login_profile(std::string_view saved_name, const ServerProfile* named,
                                  const std::optional<ServerProfile>& eqhost, bool eqhost_trusted,
                                  const std::vector<ServerProfile>& profiles, std::string_view host, int port) {
    if (!saved_name.empty() && !named) throw std::runtime_error("Token login's saved server was not found.");
    if (port < 0 || port > 65535) throw std::runtime_error("Login port must be between 1 and 65535.");
    ServerProfile profile;
    if (named) profile = *named;
    else if (!host.empty()) {
        profile.host = host;
        if (port > 0) profile.port = port;
        profile.name = profile.host;
    } else if (!eqhost_trusted)
        throw std::runtime_error("A login ticket needs its server named for this run: --server, --host, or --game-dir "
                                 "(whose eqhost.txt names it), or Hailstone installed beside your game files.");
    else if (eqhost) profile = *eqhost;
    else throw std::runtime_error("A login ticket needs --server, --host, or an eqhost.txt in the game folder.");
    if (!host.empty()) profile.host = host;
    if (port > 0) profile.port = port;
    if (profile.host.empty() || profile.port < 1 || profile.port > 65535)
        throw std::runtime_error("Token login's server has no valid login address.");
    // Overrides must not accidentally use another endpoint's assets or saved identity.
    if (!named) {
        if (const auto* match = find_profile(profiles, profile.host, profile.port)) profile = *match;
    }
    return profile;
}

const net::ServerInfo* launcher_world(std::span<const net::ServerInfo> servers, std::string_view selector) {
    if (selector.empty()) return nullptr;
    std::uint32_t id = 0;
    const auto number = std::from_chars(selector.data(), selector.data() + selector.size(), id);
    const bool numeric = number.ec == std::errc() && number.ptr == selector.data() + selector.size();
    const net::ServerInfo* match = nullptr;
    for (const auto& server : servers) {
        if (!(numeric ? server.id == id : iequals(server.name, selector))) continue;
        if (match) return nullptr;
        match = &server;
    }
    return match;
}

} // namespace hailstone::app
