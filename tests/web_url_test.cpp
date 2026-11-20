// The web addresses scripts, plugins and the server may open (core/web_url.h, web/page_rules.h).
#include <doctest/doctest.h>

#include "core/web_url.h"
#include "fuzz_support.h"
#include "web/page_rules.h"

using namespace hailstone;
using namespace hailstone::web;

TEST_CASE("web_host: http(s) only, lower-cased, without user info, port or trailing dot") {
    CHECK(web_host("https://Example.org/path?q=1#x") == "example.org");
    CHECK(web_host("HTTP://example.org") == "example.org");
    CHECK(web_host("http://user:pw@example.org:8080/") == "example.org");
    CHECK(web_host("http://evil.org@example.org/") == "example.org"); // the host is after the last '@'
    CHECK(web_host("https://example.org./") == "example.org");
    CHECK(web_host("http://[::1]:80/") == "[::1]");
    CHECK(web_host("http://127.0.0.1/") == "127.0.0.1");
    CHECK(web_host("file:///etc/passwd").empty());
    CHECK(web_host("javascript:alert(1)").empty());
    CHECK(web_host("data:text/html,hi").empty());
    CHECK(web_host("example.org").empty());
    CHECK(web_host("http://").empty());
    CHECK(web_host("http:///path").empty());
    CHECK(web_host("http://:80/").empty());
    CHECK(web_host("http://[::1/").empty());
    CHECK(web_host("http://example.org/a b").empty()); // spaces and control characters: not an address
    CHECK(web_host("http://example.org/\n").empty());
    CHECK(web_host("").empty());
}

TEST_CASE("web_host: '\\' ends the host as browsers read it; a host name holds only name characters") {
    CHECK(web_host("https://evil.example\\.good.org/") == "evil.example");
    CHECK(web_host("https://evil.example\\@good.org/") == "evil.example");
    CHECK(web_host("http://example.org\\path") == "example.org");
    CHECK_FALSE(url_allowed("https://evil.example\\.good.org/", {"good.org"}));
    CHECK_FALSE(url_allowed("https://evil.example\\@good.org/", {"good.org"}));
    CHECK(web_host("https://my_host-1.Example.org/") == "my_host-1.example.org");
    CHECK(web_host("https://xn--bcher-kva.example/") == "xn--bcher-kva.example");
    CHECK(web_host("https://evil%2egood.org/").empty()); // a browser decodes the escape
    CHECK(web_host("https://evil^good.org/").empty());
    CHECK(web_host("https://evil|good.org/").empty());
    CHECK(web_host("https://b\xc3\xbc" "cher.example/").empty()); // non-ASCII: the xn-- form only
    CHECK(web_host("http://[::1]evil.org/").empty());
    CHECK(web_host("http://[::g]/").empty());
    CHECK(web_host("http://[2001:db8::1]:443/") == "[2001:db8::1]");
    CHECK(web_host("http://[::ffff:127.0.0.1]/") == "[::ffff:127.0.0.1]");
}

TEST_CASE("host_allowed: an empty list any host; else the host or one below it") {
    CHECK(host_allowed("anything.net", {}));
    const std::vector<std::string> allow{"example.org", "wiki.example.net"};
    CHECK(host_allowed("example.org", allow));
    CHECK(host_allowed("www.example.org", allow));
    CHECK(host_allowed("wiki.example.net", allow));
    CHECK_FALSE(host_allowed("example.net", allow));
    CHECK_FALSE(host_allowed("badexample.org", allow));
    CHECK_FALSE(host_allowed("example.org.evil.com", allow));
    CHECK(url_allowed("https://www.example.org/x", allow));
    CHECK_FALSE(url_allowed("file:///example.org", allow));
    CHECK_FALSE(url_allowed("https://evil.com/?example.org", allow));
}

TEST_CASE("private_host: this computer and private networks, in every form a browser reads") {
    for (const char* h : {"localhost", "foo.localhost", "router", "nas.local", "printer.lan", "db.internal",
                          "box.home.arpa", "127.0.0.1", "127.1", "2130706433", "0x7f000001", "0x7f.1", "017700000001",
                          "0177.0.0.1", "10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.1.1", "169.254.169.254",
                          "100.64.0.1", "0.0.0.0", "0", "224.0.0.1", "255.255.255.255", "1.2.3.4.5", "1.256.3.4",
                          "4294967296", "0x", "[::1]", "[::]", "[fe80::1]", "[fd00::1]", "[fc12:3456::7]", "[ff02::1]",
                          "[::ffff:127.0.0.1]", "[::ffff:7f00:1]", "[::ffff:192.168.0.1]", "[64:ff9b::a00:1]",
                          "[::127.0.0.1]", "[1:2:3]", "[::1::2]", "[12345::]"})
        CHECK_MESSAGE(private_host(h), h);
    for (const char* h : {"example.org", "www.example.org", "8.8.8.8", "134744072", "172.32.0.1", "192.169.0.1",
                          "11.0.0.1", "[2001:db8::1]", "[2606:4700::1111]", "[::ffff:8.8.8.8]", "localhost.example.org",
                          "1.2.3.example.org"})
        CHECK_FALSE_MESSAGE(private_host(h), h);
}

TEST_CASE("script addresses and requests: the allowed sites, never private, local data kept") {
    const std::vector<std::string> any, allow{"example.org"};
    CHECK(script_url_allowed("https://example.org/", any));
    CHECK_FALSE(script_url_allowed("http://127.0.0.1:9/", any));
    CHECK_FALSE(script_url_allowed("http://localhost/", any));
    CHECK_FALSE(script_url_allowed("http://192.168.1.1/", {"192.168.1.1"})); // not even when the server lists it
    CHECK_FALSE(script_url_allowed("https://evil.com/", allow));
    CHECK(script_url_allowed("https://cdn.example.org/a.js", allow));

    CHECK(script_request_allowed("https://cdn.example.org/a.js", allow));
    CHECK_FALSE(script_request_allowed("https://tracker.net/p.gif", allow));
    CHECK_FALSE(script_request_allowed("http://10.0.0.1/admin", any));
    CHECK(script_request_allowed("wss://example.org/socket", allow));
    CHECK_FALSE(script_request_allowed("ws://127.0.0.1:8080/", any));
    CHECK_FALSE(script_request_allowed("wss://evil.com/", allow));
    CHECK(script_request_allowed("data:image/png;base64,AAAA", allow));
    CHECK(script_request_allowed("blob:https://example.org/1234", allow));
    CHECK(script_request_allowed("about:blank", allow));
    CHECK(script_request_allowed("about:srcdoc", allow));
    CHECK_FALSE(script_request_allowed("file:///etc/passwd", any));
    CHECK_FALSE(script_request_allowed("ftp://example.org/", any));
    CHECK_FALSE(script_request_allowed("javascript:alert(1)", any));
    CHECK_FALSE(script_request_allowed("chrome://settings", any));
}

TEST_CASE("fuzz: private_host and the request rules take any text") {
    fuzz::Rng rng(0x9a9e);
    const std::vector<std::string> allow{"example.org"};
    for (const char* seed : {"[::ffff:127.0.0.1]", "0x7f.0.0.1", "192.168.001.1", "[64:ff9b::a00:1]", "a.b.localhost",
                             "https://user@[fe80::1]:8080/x", "wss://example.org/s", "data:,x"}) {
        const std::string s = seed;
        fuzz::Bytes b(reinterpret_cast<const std::byte*>(s.data()), reinterpret_cast<const std::byte*>(s.data()) + s.size());
        for (int i = 0; i < 500; ++i) {
            const fuzz::Bytes m = fuzz::mutate(b, rng);
            const std::string t(reinterpret_cast<const char*>(m.data()), m.size());
            (void)private_host(t);
            (void)script_request_allowed(t, allow);
            (void)script_url_allowed(t, {});
        }
    }
}

TEST_CASE("web: a tab's rules, at every navigation and request") {
    const std::vector<std::string> none, sites{"example.org"};
    // The player's own tab: any http(s) page, private ones too; what it loads is the browser's business.
    CHECK(tab_may_show("http://192.168.1.1/", TabRules::Player, none));
    CHECK(tab_may_show("https://example.org/", TabRules::Player, none));
    CHECK_FALSE(tab_may_show("file:///etc/passwd", TabRules::Player, none));
    CHECK(tab_may_request("http://127.0.0.1/", TabRules::Player, none));
    // A server link's: any public site, never this computer or the player's network, for the page and inside it.
    CHECK(tab_may_show("https://anywhere.example.net/", TabRules::ServerLink, none));
    CHECK(tab_may_show("about:blank", TabRules::ServerLink, none));
    CHECK_FALSE(tab_may_show("http://192.168.1.1/", TabRules::ServerLink, none));
    CHECK_FALSE(tab_may_show("http://localhost:8080/", TabRules::ServerLink, none));
    CHECK_FALSE(tab_may_show("http://router/", TabRules::ServerLink, none));
    CHECK_FALSE(tab_may_show("ftp://example.org/", TabRules::ServerLink, none));
    CHECK(tab_may_request("https://cdn.example.com/a.js", TabRules::ServerLink, none));
    CHECK(tab_may_request("wss://chat.example.com/", TabRules::ServerLink, none));
    CHECK(tab_may_request("data:image/png;base64,AA", TabRules::ServerLink, none));
    CHECK_FALSE(tab_may_request("http://192.168.0.10/api", TabRules::ServerLink, none));
    CHECK_FALSE(tab_may_request("ws://127.0.0.1:9000/", TabRules::ServerLink, none));
    CHECK_FALSE(tab_may_request("http://[::1]/", TabRules::ServerLink, none));
    CHECK_FALSE(tab_may_request("http://2130706433/", TabRules::ServerLink, none));
    // A script's or plugin's: only its sites, and never a private one either.
    CHECK(tab_may_show("https://www.example.org/", TabRules::Sites, sites));
    CHECK_FALSE(tab_may_show("https://example.net/", TabRules::Sites, sites));
    CHECK_FALSE(tab_may_request("https://example.net/x.js", TabRules::Sites, sites));
    CHECK(tab_may_request("https://example.net/x.js", TabRules::Sites, none));
}
