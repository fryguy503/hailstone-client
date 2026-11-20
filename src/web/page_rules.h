#pragma once

// What a script's or plugin's page may reach (docs/browser.md "Rules"), beyond core/web_url.h's http(s) and
// [web] allow: never this computer or the player's own network (a router's admin page, a NAS, a dev server on
// localhost), and, inside the page, only the allowed sites for frames and every request (images, scripts, fetch,
// WebSockets) as well as for the page itself. Built into the helper too (hailstone_web_protocol).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::web {

// A host (core/web_url.h's web_host form: lower case, an IPv6 literal in brackets) that names this computer or a
// private network: localhost and *.localhost, a name without a dot or under .local, .lan, .internal, .home.arpa,
// and IPv4 / IPv6 literals in the loopback, private, link-local, shared (100.64/10), multicast and reserved ranges
// (IPv4 in any form a browser reads: "2130706433", "0x7f.1", "017700000001"; IPv6 with an IPv4 inside). An address
// that does not parse counts as private. A public name that resolves to a private address is not caught here.
bool private_host(std::string_view host);

// An address a script's or plugin's tab may show or open: http(s), on `allow` (empty: any host), not private_host.
bool script_url_allowed(std::string_view url, const std::vector<std::string>& allow);

// A request a script's or plugin's page makes (a frame, an image, a script, fetch, a WebSocket): what
// script_url_allowed lets through (ws:// and wss:// as http:// and https://), and the browser's own local data
// (data:, blob:, about:blank, about:srcdoc), which reach no network.
bool script_request_allowed(std::string_view url, const std::vector<std::string>& allow);

// Which rules a tab follows (the helper and BrowserHost alike):
// - Player: the player's own tab: any http(s) page; what the page loads gets the browser's default handling.
// - ServerLink: a tab a server's link (OP_Weblink) opened: any public http(s) site, but never this computer or a
//   private network, at every navigation, redirect and request the page makes (a fetch, a form's post, an image),
//   not only the link's own address: a server's page would otherwise reach the player's router from inside it.
// - Sites: a script's or plugin's tab: script_url_allowed / script_request_allowed on its `allow`.
enum class TabRules : std::uint8_t { Player = 0, ServerLink = 1, Sites = 2 };
// A main-frame address the tab may show (about:blank, a new tab's, always).
bool tab_may_show(std::string_view url, TabRules rules, const std::vector<std::string>& allow);
// A frame inside the page, or a request the page makes. Player: anything (the browser's default handling).
bool tab_may_request(std::string_view url, TabRules rules, const std::vector<std::string>& allow);

} // namespace hailstone::web
