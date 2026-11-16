#pragma once

// How the zones connect, for the map's zone finder (docs/navigation.md "Zone atlas"): which zones each zone's zone
// lines and zone points lead to, and the shortest way between two zones. Not the original's: RoF2's zone guide
// path (ZonePathWnd) came from server data EQEmu does not send.
//
// Two sources, kept apart so the file says where each link came from:
//   * the server's list (Hailstone's server script answers OP_HsZoneLinksRequest with the zone_points table the
//     server's expansion and content flags allow; net/zone_link_packets.h);
//   * learned: the zone points (OP_SendZonepoints) of each zone the player enters on this server.
// A route follows links in their own direction. When there is none that way, a route may also use a learned link
// backwards (a zone line seen from the far side only). Most zone lines go both ways, but not all, so such a route
// is marked `assumed`.
//
// The zones the server allows (its current expansion and content flags) come with its list; a route passes only
// through allowed zones (game::ZoneRoutes::route's `allowed`, app/lua_ui_map.cpp: the server's zones, else the
// account's expansions).

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hailstone::game {

struct ZoneRoute {
    std::vector<std::uint32_t> zones; // from the start to the destination, both included; empty: no route
    bool assumed = false;             // it uses a learned link backwards
};

class ZoneRoutes {
public:
    using Link = std::pair<std::uint32_t, std::uint32_t>; // from, to

    // A zone's zone points lead to `to` (zone 0 and the zone itself are left out). True when a link was new.
    bool learn(std::uint32_t from, const std::vector<std::uint32_t>& to);
    // The server's list, replacing an earlier one. True when it changed.
    bool set_server(std::vector<Link> links);
    bool has_server() const { return !server_.empty(); }
    // The zones the server allows, with its current expansion (-1 all), replacing earlier ones. True when changed.
    bool set_server_zones(std::vector<std::uint32_t> zones, int expansion);
    const std::set<std::uint32_t>& server_zones() const { return server_zones_; } // empty: not known
    int server_expansion() const { return server_expansion_; }
    bool empty() const { return server_.empty() && learned_.empty(); }

    // The fewest zone changes from `from` to `to` (links in their direction first, then with learned ones
    // backwards); the lowest zone ids first among equals. `from == to` gives that zone alone. With `allowed`, a route
    // never enters a zone it refuses (the start may be any).
    ZoneRoute route(std::uint32_t from, std::uint32_t to,
                    const std::function<bool(std::uint32_t)>& allowed = nullptr) const;
    // Where a zone's links lead (both sources), ascending.
    std::vector<std::uint32_t> neighbours(std::uint32_t zone) const;
    // Every zone a link starts or ends at, ascending.
    std::vector<std::uint32_t> zones() const;

    // The file kept per server (servers/<profile>/zone_links.txt): a line per link, "s <from> <to>" from the
    // server's list or "l <from> <to>" learned, "z <zone>" a zone the server allows, "e <expansion>" its current
    // expansion; '#' lines and anything unreadable are skipped.
    std::string serialize() const;
    static ZoneRoutes parse(std::string_view text);

private:
    std::set<Link> server_, learned_;
    std::set<std::uint32_t> server_zones_;
    int server_expansion_ = -1;
    std::map<std::uint32_t, std::set<std::uint32_t>> forward_, backward_; // forward: both sources; backward: learned
    void rebuild();
};

} // namespace hailstone::game
