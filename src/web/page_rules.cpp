#include "web/page_rules.h"

#include <array>
#include <cstdint>
#include <optional>

#include "core/web_url.h"

namespace hailstone::web {

namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }
bool hex(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int hex_value(char c) { return digit(c) ? c - '0' : (c | 0x20) - 'a' + 10; }

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    for (;;) {
        std::size_t at = s.find(sep);
        out.push_back(s.substr(0, at));
        if (at == std::string_view::npos) return out;
        s.remove_prefix(at + 1);
    }
}

// One part of an IPv4 address as the WHATWG URL standard reads it: 0x.. hex, 0.. octal, else decimal.
std::optional<std::uint64_t> ipv4_number(std::string_view p) {
    if (p.empty()) return std::nullopt;
    unsigned radix = 10;
    if (p.size() >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        radix = 16;
        p.remove_prefix(2);
    } else if (p.size() >= 2 && p[0] == '0') {
        radix = 8;
        p.remove_prefix(1);
    }
    std::uint64_t v = 0;
    for (char c : p) {
        unsigned d = 0;
        if (radix == 16 && hex(c)) d = static_cast<unsigned>(hex_value(c));
        else if (digit(c) && static_cast<unsigned>(c - '0') < radix) d = static_cast<unsigned>(c - '0');
        else return std::nullopt;
        v = v * radix + d;
        if (v > 0xffffffffull) return std::nullopt;
    }
    return v;
}

// Whether the WHATWG parser takes the host for an IPv4 address: its last part is a number.
bool ends_in_number(std::string_view host) {
    std::vector<std::string_view> parts = split(host, '.');
    if (parts.size() > 1 && parts.back().empty()) parts.pop_back();
    std::string_view last = parts.back();
    if (last.empty()) return false;
    bool all_digits = true;
    for (char c : last) all_digits = all_digits && digit(c);
    if (all_digits) return true;
    return ipv4_number(last).has_value() && last.size() >= 2 && last[0] == '0' && (last[1] == 'x' || last[1] == 'X');
}

std::optional<std::uint32_t> parse_ipv4(std::string_view host) {
    std::vector<std::string_view> parts = split(host, '.');
    if (parts.size() > 1 && parts.back().empty()) parts.pop_back();
    if (parts.size() > 4) return std::nullopt;
    std::vector<std::uint64_t> n;
    for (std::string_view p : parts) {
        auto v = ipv4_number(p);
        if (!v) return std::nullopt;
        n.push_back(*v);
    }
    for (std::size_t i = 0; i + 1 < n.size(); ++i)
        if (n[i] > 255) return std::nullopt;
    const unsigned rest_bits = static_cast<unsigned>(8 * (5 - n.size()));
    if (rest_bits < 64 && n.back() >= (std::uint64_t{1} << rest_bits)) return std::nullopt;
    std::uint64_t ip = n.back();
    for (std::size_t i = 0; i + 1 < n.size(); ++i) ip += n[i] << (8 * (3 - i));
    return static_cast<std::uint32_t>(ip);
}

bool private_ipv4(std::uint32_t ip) {
    auto in = [ip](std::uint32_t net, int bits) {
        const std::uint32_t mask = bits == 0 ? 0 : ~std::uint32_t{0} << (32 - bits);
        return (ip & mask) == net;
    };
    return in(0x00000000, 8) ||   // 0/8: "this network"
           in(0x0a000000, 8) ||   // 10/8
           in(0x64400000, 10) ||  // 100.64/10: shared (carrier NAT)
           in(0x7f000000, 8) ||   // 127/8: loopback
           in(0xa9fe0000, 16) ||  // 169.254/16: link-local
           in(0xac100000, 12) ||  // 172.16/12
           in(0xc0000000, 24) ||  // 192.0.0/24: protocol assignments
           in(0xc0a80000, 16) ||  // 192.168/16
           in(0xc6120000, 15) ||  // 198.18/15: benchmarking
           in(0xe0000000, 4) ||   // 224/4: multicast
           in(0xf0000000, 4);     // 240/4: reserved, broadcast
}

// "[...]" to its 16 bytes.
std::optional<std::array<std::uint8_t, 16>> parse_ipv6(std::string_view s) {
    if (s.size() < 2 || s.front() != '[' || s.back() != ']') return std::nullopt;
    s = s.substr(1, s.size() - 2);
    std::array<std::uint16_t, 8> g{};
    std::vector<std::uint16_t> head, tail;
    auto groups = [](std::string_view part, std::vector<std::uint16_t>& out, bool last) -> bool {
        if (part.empty()) return true;
        std::vector<std::string_view> fields = split(part, ':');
        for (std::size_t i = 0; i < fields.size(); ++i) {
            std::string_view f = fields[i];
            if (last && i + 1 == fields.size() && f.find('.') != std::string_view::npos) {
                // An IPv4 address in the last two groups: dotted decimal only.
                std::vector<std::string_view> q = split(f, '.');
                if (q.size() != 4) return false;
                std::uint32_t v = 0;
                for (std::string_view d : q) {
                    if (d.empty() || d.size() > 3) return false;
                    unsigned n = 0;
                    for (char c : d) {
                        if (!digit(c)) return false;
                        n = n * 10 + static_cast<unsigned>(c - '0');
                    }
                    if (n > 255) return false;
                    v = v << 8 | n;
                }
                out.push_back(static_cast<std::uint16_t>(v >> 16));
                out.push_back(static_cast<std::uint16_t>(v & 0xffff));
                continue;
            }
            if (f.empty() || f.size() > 4) return false;
            unsigned v = 0;
            for (char c : f) {
                if (!hex(c)) return false;
                v = v * 16 + static_cast<unsigned>(hex_value(c));
            }
            out.push_back(static_cast<std::uint16_t>(v));
        }
        return true;
    };
    const std::size_t gap = s.find("::");
    if (gap != std::string_view::npos) {
        if (s.find("::", gap + 1) != std::string_view::npos) return std::nullopt;
        if (!groups(s.substr(0, gap), head, false) || !groups(s.substr(gap + 2), tail, true)) return std::nullopt;
        if (head.size() + tail.size() > 7) return std::nullopt;
    } else {
        if (!groups(s, head, true) || head.size() != 8) return std::nullopt;
    }
    for (std::size_t i = 0; i < head.size(); ++i) g[i] = head[i];
    for (std::size_t i = 0; i < tail.size(); ++i) g[8 - tail.size() + i] = tail[i];
    std::array<std::uint8_t, 16> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        out[2 * i] = static_cast<std::uint8_t>(g[i] >> 8);
        out[2 * i + 1] = static_cast<std::uint8_t>(g[i] & 0xff);
    }
    return out;
}

bool private_ipv6(const std::array<std::uint8_t, 16>& a) {
    bool first10_zero = true;
    for (std::size_t i = 0; i < 10; ++i) first10_zero = first10_zero && a[i] == 0;
    const std::uint32_t low = std::uint32_t{a[12]} << 24 | std::uint32_t{a[13]} << 16 | std::uint32_t{a[14]} << 8 | a[15];
    // ::/96 (unspecified, loopback, the old IPv4-compatible form) and ::ffff:0:0/96 (IPv4-mapped): the IPv4 inside.
    if (first10_zero && ((a[10] == 0 && a[11] == 0) || (a[10] == 0xff && a[11] == 0xff)))
        return low <= 1 || private_ipv4(low);
    // 64:ff9b::/96: NAT64, an IPv4 address inside.
    if (a[0] == 0x00 && a[1] == 0x64 && a[2] == 0xff && a[3] == 0x9b) {
        bool zero = true;
        for (std::size_t i = 4; i < 12; ++i) zero = zero && a[i] == 0;
        if (zero) return private_ipv4(low);
    }
    return (a[0] & 0xfe) == 0xfc ||                  // fc00::/7: unique local
           (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) || // fe80::/10: link-local
           (a[0] == 0xfe && (a[1] & 0xc0) == 0xc0) || // fec0::/10: the old site-local
           a[0] == 0xff;                             // ff00::/8: multicast
}

bool ends_with_label(std::string_view host, std::string_view suffix) {
    return host == suffix || (host.size() > suffix.size() && host.ends_with(suffix) &&
                              host[host.size() - suffix.size() - 1] == '.');
}

} // namespace

bool private_host(std::string_view host) {
    while (!host.empty() && host.back() == '.') host.remove_suffix(1);
    if (host.empty()) return true;
    if (host.front() == '[') {
        auto a = parse_ipv6(host);
        return !a || private_ipv6(*a);
    }
    if (ends_in_number(host)) {
        auto ip = parse_ipv4(host);
        return !ip || private_ipv4(*ip);
    }
    if (host.find('.') == std::string_view::npos) return true; // "localhost", "router", "nas": the local network's
    for (std::string_view s : {"localhost", "local", "lan", "internal", "home.arpa"})
        if (ends_with_label(host, s)) return true;
    return false;
}

bool script_url_allowed(std::string_view url, const std::vector<std::string>& allow) {
    const std::string host = web_host(url);
    return !host.empty() && host_allowed(host, allow) && !private_host(host);
}

bool script_request_allowed(std::string_view url, const std::vector<std::string>& allow) {
    auto starts = [url](std::string_view p) {
        if (url.size() < p.size()) return false;
        for (std::size_t i = 0; i < p.size(); ++i) {
            char c = url[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != p[i]) return false;
        }
        return true;
    };
    if (starts("data:") || starts("blob:") || url == "about:blank" || url == "about:srcdoc") return true;
    if (starts("wss://")) return script_url_allowed("https://" + std::string(url.substr(6)), allow);
    if (starts("ws://")) return script_url_allowed("http://" + std::string(url.substr(5)), allow);
    return script_url_allowed(url, allow);
}

bool tab_may_show(std::string_view url, TabRules rules, const std::vector<std::string>& allow) {
    if (url == "about:blank") return true;
    static const std::vector<std::string> kAny;
    switch (rules) {
    case TabRules::Player: return url_allowed(url, kAny);
    case TabRules::ServerLink: return script_url_allowed(url, kAny);
    case TabRules::Sites: return script_url_allowed(url, allow);
    }
    return false;
}

bool tab_may_request(std::string_view url, TabRules rules, const std::vector<std::string>& allow) {
    static const std::vector<std::string> kAny;
    switch (rules) {
    case TabRules::Player: return true;
    case TabRules::ServerLink: return script_request_allowed(url, kAny);
    case TabRules::Sites: return script_request_allowed(url, allow);
    }
    return false;
}

} // namespace hailstone::web
