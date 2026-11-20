// The browser helper's messages (web/browser_protocol.h) and the client's side of it (web/browser_host.h): against
// an in-memory helper, and against the stand-in helper program (tests/mock_web_helper.cpp) over real pipes.
#include <doctest/doctest.h>

#include <cfloat>
#include <chrono>
#include <deque>
#include <thread>

#include <imgui.h>

#include "app/web_service.h"
#include "fuzz_support.h"
#include "plugin/policy.h"
#include "platform/helper_process.h"
#include "scratch_dir.h"
#include "web/browser_host.h"
#include "web/browser_protocol.h"

using namespace hailstone;
using namespace hailstone::web;

namespace {

std::vector<Message> samples() {
    Open o;
    o.tab = 7;
    o.width = 640;
    o.height = 480;
    o.rules = TabRules::Sites;
    o.allow = {"example.org", "wiki.example.net"};
    o.profile = "/data/web/server";
    o.url = "https://example.org/";
    Frame f;
    f.tab = 7;
    f.layer = Layer::Popup;
    f.width = 4;
    f.height = 3;
    FrameRect r;
    r.x = 1;
    r.y = 1;
    r.width = 2;
    r.height = 2;
    r.bgra.assign(16, std::byte{0x5a});
    f.rects.push_back(r);
    return {Hello{1},
            o,
            Navigate{7, "https://example.org/b"},
            Resize{7, 800, 600},
            Mouse{7, MouseKind::Down, MouseButton::Right, 2, -3, 40, kModShift | kModCtrl},
            Wheel{7, 1, 2, 0, -120, kModAlt},
            Key{7, KeyKind::Char, 0x41, 0x1f600, kModMeta},
            Text{7, "h\xc3\xa9llo"},
            Focus{7, true},
            Nav{7, NavAction::Forward},
            Eval{7, "1+1"},
            Post{7, "{\"a\":1}"},
            Close{7},
            Shutdown{},
            Ready{1},
            f,
            Popup{7, true, -5, 9, 120, 200},
            Address{7, "https://example.org/c"},
            Title{7, "A page"},
            Loading{7, true, true, false},
            CursorShape{7, Cursor::Hand},
            Console{7, 3, "an error"},
            PageMessage{7, "hi"},
            Closed{7, CloseReason::Refused, "why"}};
}

// The in-memory helper: what the host sent, decoded, and bytes queued for it to receive.
struct FakeHelper {
    std::vector<Message> sent;
    std::deque<std::byte> to_client;
    bool alive = true, start_ok = true;
    int starts = 0;
    // `hold`: what is sent waits (in `pending`) instead of reaching the helper; `taken` is what it has taken in all.
    bool hold = false;
    std::size_t pending = 0;
    std::uint64_t taken = 0;

    void reply(const Message& m) {
        auto b = encode_message(m);
        to_client.insert(to_client.end(), b.begin(), b.end());
    }
    template <typename T> std::vector<T> sent_of() const {
        std::vector<T> out;
        for (const auto& m : sent)
            if (auto* v = std::get_if<T>(&m)) out.push_back(*v);
        return out;
    }
};

class FakeTransport final : public Transport {
public:
    explicit FakeTransport(FakeHelper& h) : h_(h) {}
    bool start(std::string& error) override {
        ++h_.starts;
        h_.alive = h_.start_ok;
        if (!h_.start_ok) error = "no such file";
        return h_.start_ok;
    }
    void send(std::span<const std::byte> bytes) override {
        if (h_.hold) {
            h_.pending += bytes.size();
            return;
        }
        reader_.feed(bytes);
        while (auto m = reader_.next()) h_.sent.push_back(*m);
    }
    std::size_t receive(std::span<std::byte> out) override {
        std::size_t n = std::min(out.size(), h_.to_client.size());
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = h_.to_client.front();
            h_.to_client.pop_front();
        }
        return n;
    }
    bool alive() override { return h_.alive; }
    std::size_t pending() override { return h_.pending; }
    std::uint64_t taken() override { return h_.taken; }
    void stop(int) override { h_.alive = false; }

private:
    FakeHelper& h_;
    MessageReader reader_;
};

struct Rig {
    FakeHelper helper;
    BrowserHost host{[this] { return std::make_unique<FakeTransport>(helper); }};
};

TabOptions script(std::vector<std::string> allow = {}) {
    TabOptions o;
    o.owner = "myscript";
    o.allow = std::move(allow);
    return o;
}

} // namespace

TEST_CASE("browser protocol: every message encodes and decodes to the same bytes") {
    for (const Message& m : samples()) {
        auto bytes = encode_message(m);
        MessageReader r;
        r.feed(bytes);
        auto back = r.next();
        REQUIRE_MESSAGE(back, "message type " << m.index());
        CHECK(back->index() == m.index());
        CHECK(encode_message(*back) == bytes);
        CHECK_FALSE(r.next());
    }
    const auto open_bytes = encode_message(samples()[1]);
    auto o = std::get<Open>(*decode_message(std::span<const std::byte>(open_bytes).subspan(4)));
    CHECK(o.allow == std::vector<std::string>{"example.org", "wiki.example.net"});
    CHECK(o.url == "https://example.org/");
    CHECK(o.rules == TabRules::Sites);
    Open link;
    link.tab = 3;
    link.rules = TabRules::ServerLink;
    link.url = "https://example.org/";
    const auto link_bytes = encode_message(link);
    CHECK(std::get<Open>(*decode_message(std::span<const std::byte>(link_bytes).subspan(4))).rules ==
          TabRules::ServerLink);
}

TEST_CASE("browser protocol: a stream in pieces; broken lengths, types and frames stop it") {
    std::vector<std::byte> stream;
    for (const Message& m : samples()) {
        auto b = encode_message(m);
        stream.insert(stream.end(), b.begin(), b.end());
    }
    MessageReader r;
    std::size_t got = 0;
    for (std::byte b : stream) { // a byte at a time
        r.feed(std::span(&b, 1));
        while (r.next()) ++got;
    }
    CHECK(got == samples().size());
    CHECK_FALSE(r.failed());

    auto broken = [](std::vector<std::byte> b) {
        MessageReader x;
        x.feed(b);
        CHECK_FALSE(x.next());
        return x.failed();
    };
    CHECK(broken({std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}}));             // length 0
    CHECK(broken({std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x10}, std::byte{1}})); // 256 MB
    CHECK(broken({std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0xee}})); // unknown type
    auto extra = encode_message(Close{1});
    extra.push_back(std::byte{0});
    extra[0] = static_cast<std::byte>(std::to_integer<int>(extra[0]) + 1); // a byte left over
    CHECK(broken(extra));
    CHECK_FALSE(broken({std::byte{9}, std::byte{0}})); // only part of a length: waits

    Frame f;
    f.width = f.height = 2;
    FrameRect outside;
    outside.x = 1;
    outside.width = 2;
    outside.height = 1;
    outside.bgra.assign(8, std::byte{0});
    f.rects.push_back(outside);
    CHECK(broken(encode_message(f)));
    Loading bad_flag{1, true, false, false};
    auto lb = encode_message(bad_flag);
    lb[9] = std::byte{2};
    CHECK(broken(lb));
    // An Open's rules byte (after length, type, tab, width and height) past the last rules there are.
    Open bad_rules;
    auto ob = encode_message(bad_rules);
    CHECK(std::to_integer<int>(ob[13]) == 0);
    ob[13] = std::byte{3};
    CHECK(broken(ob));
}

TEST_CASE("fuzz: browser protocol decoding never throws or allocates without bound") {
    fuzz::QuietLog quiet;
    fuzz::Rng rng(0xb10c);
    std::vector<fuzz::Bytes> seeds;
    for (const Message& m : samples()) {
        auto b = encode_message(m);
        seeds.emplace_back(b.begin() + 4, b.end());
    }
    auto body = [](std::span<const std::byte> in) {
        (void)decode_message(in);
        MessageReader r;
        r.feed(in);
        while (r.next()) {
        }
    };
    for (const auto& s : seeds) {
        for (std::size_t n = 0; n <= s.size(); ++n)
            CHECK(fuzz::run_case("decode_message", fuzz::Bytes(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n)),
                                 fuzz::Throws::Never, body));
        for (int i = 0; i < 300; ++i) CHECK(fuzz::run_case("decode_message", fuzz::mutate(s, rng), fuzz::Throws::Never, body));
    }
    for (int i = 0; i < 500; ++i)
        CHECK(fuzz::run_case("decode_message", fuzz::random_bytes(rng, rng.below(256)), fuzz::Throws::Never, body));
}

TEST_CASE("browser host: what may open; Hello first, then the tab with its rules") {
    Rig r;
    std::string why;
    CHECK(r.host.open("file:///etc/passwd", {}, &why) == 0);
    CHECK(why.find("not an http") != std::string::npos);
    CHECK(r.helper.starts == 0); // refused before the helper starts
    CHECK(r.host.open("https://evil.com/", script({"example.org"}), &why) == 0);
    CHECK(why.find("not on a site") != std::string::npos);
    CHECK(r.host.open("http://127.0.0.1:8080/", script(), &why) == 0); // never this computer or the network's
    CHECK(why.find("private network") != std::string::npos);
    CHECK(r.host.open("http://192.168.1.1/", script({"192.168.1.1"}), &why) == 0);

    std::uint32_t mine = r.host.open("https://anything.net/", {});
    std::uint32_t theirs = r.host.open("https://www.example.org/", script({"example.org"}));
    CHECK(mine != 0);
    CHECK(theirs != 0);
    CHECK(r.helper.starts == 1);
    REQUIRE(r.helper.sent.size() == 3);
    CHECK(std::holds_alternative<Hello>(r.helper.sent[0]));
    auto opens = r.helper.sent_of<Open>();
    CHECK(opens[0].rules == TabRules::Player);
    CHECK(opens[0].allow.empty());
    CHECK(opens[1].rules == TabRules::Sites);
    CHECK(opens[1].allow == std::vector<std::string>{"example.org"});

    CHECK_FALSE(r.host.navigate(theirs, "https://evil.com/"));
    CHECK(r.host.navigate(theirs, "https://example.org/x"));
    CHECK(r.host.navigate(mine, "http://elsewhere.org/"));
    CHECK(r.host.navigate(theirs, "about:blank")); // an empty page, for any tab
    CHECK(r.helper.sent_of<Navigate>().size() == 3);
}

TEST_CASE("browser host: no helper: nothing opens, and it is not tried again") {
    Rig r;
    r.helper.start_ok = false;
    std::string why;
    CHECK(r.host.open("https://example.org/", {}, &why) == 0);
    CHECK(why.find("did not start") != std::string::npos);
    CHECK_FALSE(r.host.available());
    CHECK(r.host.open("https://example.org/", {}) == 0);
    CHECK(r.helper.starts == 1);
}

TEST_CASE("browser host: frames, the page's state and its messages") {
    Rig r;
    auto id = r.host.open("https://example.org/", script());
    auto mine = r.host.open("https://example.org/", {});
    r.helper.reply(Ready{});
    Frame full;
    full.tab = id;
    full.width = 3;
    full.height = 2;
    FrameRect all;
    all.width = 3;
    all.height = 2;
    all.bgra.assign(24, std::byte{1});
    full.rects.push_back(all);
    r.helper.reply(full);
    Frame part;
    part.tab = id;
    part.width = 3;
    part.height = 2;
    FrameRect px;
    px.x = 2;
    px.y = 1;
    px.width = px.height = 1;
    px.bgra.assign(4, std::byte{9});
    part.rects.push_back(px);
    r.helper.reply(part);
    r.helper.reply(Title{id, "Example"});
    r.helper.reply(Loading{id, false, true, false});
    r.helper.reply(CursorShape{id, Cursor::Text});
    r.helper.reply(PageMessage{id, "one"});
    r.helper.reply(PageMessage{id, "two"});
    r.helper.reply(PageMessage{mine, "not for the player"});
    r.helper.reply(Title{999, "no such tab"});
    r.host.poll(0);

    const Tab* t = r.host.tab(id);
    REQUIRE(t);
    CHECK(t->view.width == 3);
    CHECK(t->view.height == 2);
    CHECK(t->view.serial == 2);
    CHECK(t->view.bgra[0] == std::byte{1});
    CHECK(t->view.bgra[(1 * 3 + 2) * 4] == std::byte{9});
    auto dirty = r.host.take_dirty(id, Layer::View);
    REQUIRE(dirty.size() == 3); // the new size's whole image, the full rect, the pixel
    CHECK(dirty[2].x == 2);
    CHECK(r.host.take_dirty(id, Layer::View).empty());
    CHECK(t->title == "Example");
    CHECK(t->can_back);
    CHECK(t->cursor == Cursor::Text);
    CHECK(r.host.take_messages(id) == std::vector<std::string>{"one", "two"});
    CHECK(r.host.take_messages(id).empty());
    CHECK(r.host.take_messages(mine).empty());

    for (int i = 0; i < 300; ++i) r.helper.reply(PageMessage{id, std::to_string(i)});
    r.host.poll(0);
    auto many = r.host.take_messages(id);
    CHECK(many.size() == BrowserHost::kMessageQueue);
    CHECK(many.front() == "44"); // the oldest went

    r.host.post(id, "hello");
    CHECK(r.helper.sent_of<Post>().back().message == "hello");
    r.host.close(id);
    CHECK(r.host.tab(id) == nullptr);
    CHECK(r.helper.sent_of<Close>().back().tab == id);
}

TEST_CASE("browser host: a page's messages and console lines are held to sizes, not only counts") {
    // The protocol: a console line or a page message longer than its own limit is a broken message, seen from the
    // first bytes (nothing waits for 64 MB of it).
    auto broken = [](const Message& m, std::size_t keep = 0) {
        auto b = encode_message(m);
        if (keep) b.resize(keep);
        MessageReader x;
        x.feed(b);
        while (x.next()) {
        }
        return x.failed();
    };
    CHECK_FALSE(broken(Console{1, 0, std::string(kMaxConsoleText, 'c')}));
    CHECK(broken(Console{1, 0, std::string(kMaxConsoleText + 1, 'c')}));
    CHECK_FALSE(broken(PageMessage{1, std::string(kMaxPageMessage, 'm')}));
    CHECK(broken(PageMessage{1, std::string(kMaxPageMessage + 1, 'm')}, 5));
    CHECK_FALSE(broken(Title{1, std::string(kMaxPageMessage + 1, 't')})); // a title is kept once, not queued
    const auto long_line = encode_message(Console{1, 0, std::string(kMaxConsoleText + 1, 'c')});
    CHECK_FALSE(decode_message(std::span<const std::byte>(long_line).subspan(4)));
    std::string cut = "a\xc3\xa9";
    cut_utf8(cut, 2);
    CHECK(cut == "a"); // not half of the e-acute
    cut = "abc";
    cut_utf8(cut, 5);
    CHECK(cut == "abc");

    // The host: the oldest messages go once the queue holds kMessageQueueBytes.
    Rig r;
    auto id = r.host.open("https://example.org/", script());
    r.helper.reply(Ready{});
    const std::size_t n = BrowserHost::kMessageQueueBytes / kMaxPageMessage + 3;
    for (std::size_t i = 0; i < n; ++i) r.helper.reply(PageMessage{id, std::string(kMaxPageMessage, static_cast<char>('a' + i))});
    for (int i = 0; i < 8; ++i) r.host.poll(0); // a poll reads at most 16 MB
    REQUIRE(r.helper.to_client.empty());
    REQUIRE(r.host.tab(id));
    CHECK(r.host.tab(id)->message_bytes <= BrowserHost::kMessageQueueBytes);
    CHECK_FALSE(r.host.tab(id)->closed);
    auto got = r.host.take_messages(id);
    CHECK(got.size() == BrowserHost::kMessageQueueBytes / kMaxPageMessage);
    CHECK(got.front()[0] == static_cast<char>('a' + 3)); // the first three went
    CHECK(got.back()[0] == static_cast<char>('a' + n - 1));
    CHECK(r.host.tab(id)->message_bytes == 0);
    r.helper.reply(PageMessage{id, "small"});
    r.host.poll(0);
    CHECK(r.host.take_message(id) == "small");
    CHECK(r.host.tab(id)->message_bytes == 0);
}

TEST_CASE("web service: each script and plugin has a cookie folder of its own; the player's stays the server's") {
    using app::WebService;
    const std::string base = "/data/web/myserver";
    CHECK(WebService::owner_profile(base, "") == base);
    const std::string s1 = WebService::owner_profile(base, "script:Foo");
    CHECK(s1.rfind(base + "/hailstone-addons/script_foo-", 0) == 0);
    CHECK(WebService::owner_profile(base, "script:foo") == s1); // names match without case
    CHECK(WebService::owner_profile(base + "/", "script:foo") == s1);
    const std::string p1 = WebService::owner_profile(base, "plugin:foo");
    const std::string odd1 = WebService::owner_profile(base, "script:a/b");
    const std::string odd2 = WebService::owner_profile(base, "script:a_b");
    CHECK(p1 != s1);
    CHECK(odd1 != odd2); // the same folder-safe name, told apart by the hash
    CHECK(odd1.find("/b") == std::string::npos);
    CHECK(WebService::owner_profile(base, "script:../../x").find("..") == std::string::npos);
    CHECK(WebService::owner_profile("", "script:foo").empty());

    // The tabs WebService opens carry them to the helper.
    Rig r;
    WebService web({[&] { return &r.host; }, [] { return nullptr; }, [&] { return base; },
                    [] { return std::vector<std::string>{}; }, [] {}});
    CHECK(web.open("", plugin::kCapAll, "https://example.org/", 64, 64) != 0);
    CHECK(web.open("script:Foo", plugin::kCapAll, "https://example.org/", 64, 64) != 0);
    CHECK(web.open("plugin:foo", plugin::kCapAll, "https://example.org/", 64, 64) != 0);
    auto opens = r.helper.sent_of<Open>();
    REQUIRE(opens.size() == 3);
    CHECK(opens[0].profile == base);
    CHECK(opens[1].profile == s1);
    CHECK(opens[2].profile == p1);

    // A server's link (OP_Weblink): the player's tab, in a folder of its own, not the player's sign-ins.
    const std::uint32_t link = web.open_server_link("https://example.org/");
    REQUIRE(link != 0);
    CHECK(web.tabs("") == std::vector<std::uint32_t>{r.helper.sent_of<Open>()[0].tab, link});
    opens = r.helper.sent_of<Open>();
    REQUIRE(opens.size() == 4);
    CHECK(opens[3].profile == base + "/server-links");
    // Not held to a script's sites, but the helper keeps it off this computer and the player's network at every
    // navigation and request, not only the link's own address.
    CHECK(opens[3].rules == TabRules::ServerLink);
    CHECK(opens[3].allow.empty());
    CHECK(opens[3].profile != s1);
    CHECK(WebService::server_link_profile(base + "/") == base + "/server-links");
    std::string why;
    CHECK(web.navigate("", plugin::kCapAll, link, "https://elsewhere.org/", &why));
    CHECK_FALSE(web.navigate("", plugin::kCapAll, link, "http://192.168.1.1/", &why));
    CHECK(why.find("private network") != std::string::npos);
    CHECK(web.navigate("", plugin::kCapAll, opens[0].tab, "http://192.168.1.1/", &why)); // the player's own tab
}

TEST_CASE("browser host: a server link's tab that lands on a private address is closed") {
    Rig r;
    TabOptions o;
    o.server_link = true;
    CHECK(tab_rules(o) == TabRules::ServerLink);
    o.owner = "script:x";
    CHECK(tab_rules(o) == TabRules::Sites); // an owner's rules win
    o = {};
    CHECK(tab_rules(o) == TabRules::Player);
    o.server_link = true;
    std::string why;
    CHECK(r.host.open("http://localhost:8080/", o, &why) == 0);
    auto id = r.host.open("https://example.org/", o);
    REQUIRE(id != 0);
    r.helper.reply(Ready{});
    r.helper.reply(Address{id, "https://anywhere.example.net/"});
    r.host.poll(0);
    CHECK_FALSE(r.host.tab(id)->closed);
    r.helper.reply(Address{id, "http://10.0.0.1/admin"}); // a redirect the helper should have refused
    r.host.poll(0);
    REQUIRE(r.host.tab(id));
    CHECK(r.host.tab(id)->closed);
}

TEST_CASE("browser host: a script's tab that lands off its sites is closed") {
    Rig r;
    auto id = r.host.open("https://example.org/", script({"example.org"}));
    r.helper.reply(Ready{});
    r.helper.reply(Address{id, "https://www.example.org/ok"});
    r.host.poll(0);
    CHECK_FALSE(r.host.tab(id)->closed);
    r.helper.reply(Address{id, "https://evil.com/"});
    r.host.poll(0);
    REQUIRE(r.host.tab(id));
    CHECK(r.host.tab(id)->closed);
    CHECK(r.helper.sent_of<Close>().size() == 1);
    r.host.post(id, "ignored"); // a closed tab takes nothing
    CHECK(r.helper.sent_of<Post>().empty());
    auto t = r.host.take_closed(id);
    REQUIRE(t);
    CHECK(t->close_reason == CloseReason::Refused);
    CHECK(r.host.tab(id) == nullptr);
}

// The helper quits on a message over its limit (a broken stream), so the client never sends one: an Eval, Text or
// Post (held to kMaxPostMessage, what the page may post back) too long, or an address over kMaxUrl, is refused and
// the helper keeps running.
TEST_CASE("browser host: messages too long for the helper are refused, not sent") {
    Rig r;
    auto id = r.host.open("https://example.org/", script());
    r.helper.reply(Ready{});
    r.host.poll(0);
    const std::string huge(kMaxMessage, 'x');
    CHECK_FALSE(r.host.eval(id, huge));
    CHECK_FALSE(r.host.text(id, huge));
    CHECK_FALSE(r.host.post(id, std::string(kMaxPostMessage + 1, 'x')));
    CHECK(r.host.post(id, std::string(kMaxPostMessage, 'x')));
    CHECK(r.host.eval(id, "1+1"));
    CHECK(r.host.text(id, "hi"));
    std::string why;
    const std::string long_url = "https://example.org/" + std::string(kMaxUrl, 'a');
    CHECK_FALSE(r.host.navigate(id, long_url, &why));
    CHECK(why == "the address is too long");
    CHECK(r.host.open(long_url, script(), &why) == 0);
    CHECK(r.helper.sent_of<Eval>().size() == 1);
    CHECK(r.helper.sent_of<Text>().size() == 1);
    CHECK(r.helper.sent_of<Post>().size() == 1);
    CHECK(r.helper.sent_of<Navigate>().empty());
    CHECK(r.helper.sent_of<Open>().size() == 1);
    CHECK_FALSE(r.host.tab(id)->closed);
    CHECK(r.host.available());

    // Through the service, as mq.web and hs.Web* reach it: false, and nothing sent.
    app::WebService web({[&] { return &r.host; }, [] { return nullptr; }, [] { return std::string(); },
                         [] { return std::vector<std::string>{}; }, [] {}});
    auto mine = web.open("script:big", plugin::kCapWeb, "https://example.org/", 64, 64);
    REQUIRE(mine != 0);
    CHECK_FALSE(web.eval("script:big", plugin::kCapWeb, mine, huge));
    CHECK_FALSE(web.post("script:big", plugin::kCapWeb, mine, huge));
    CHECK(web.post("script:big", plugin::kCapWeb, mine, "ok"));
    CHECK(r.helper.sent_of<Post>().size() == 2);
}

// While kSendBudget waits for the helper, what a script sends is refused (false), never queued until the queue's limit
// ends a helper that is still reading; an empty queue takes even the largest message, and the client's own Close
// always goes.
TEST_CASE("browser host: a full queue refuses messages instead of ending the helper") {
    Rig r;
    auto id = r.host.open("https://example.org/", script());
    r.helper.reply(Ready{});
    r.host.poll(0);
    r.helper.hold = true;
    const std::string big(std::size_t{60} << 20, 'x');
    CHECK(r.host.eval(id, big)); // the queue was empty
    CHECK_FALSE(r.host.eval(id, big));
    CHECK_FALSE(r.host.text(id, big));
    std::string why;
    CHECK_FALSE(r.host.navigate(id, "https://example.org/b", &why));
    CHECK(why == "the browser helper is still taking earlier messages");
    CHECK(r.host.tab(id)->url == "https://example.org/"); // not where it never went
    CHECK(r.host.open("https://example.org/c", script(), &why) == 0);
    CHECK(r.host.tabs().size() == 1); // no tab left loading for an Open never sent
    r.host.resize(id, 100, 100);
    CHECK(r.host.tab(id)->options.width == 800); // sent again at the next resize
    const std::size_t held = r.helper.pending;
    auto other = r.host.open("https://example.org/", script());
    CHECK(other == 0);
    r.host.close(id);
    CHECK(r.helper.pending > held); // the Close went
    // The helper takes it: messages go again, and nothing died.
    r.helper.pending = 0;
    r.helper.taken += std::size_t{60} << 20;
    r.helper.hold = false;
    auto again = r.host.open("https://example.org/", script());
    REQUIRE(again != 0);
    CHECK(r.host.eval(again, "1+1"));
    r.host.poll(BrowserHost::kStallMs * 2);
    CHECK(r.helper.starts == 1);
    CHECK(r.helper.alive);
    CHECK(r.host.last_error().empty());
}

// A paint larger than a message may be goes as several Frames: each fits, and together they draw the same image.
TEST_CASE("browser protocol: split_frame cuts a large paint into frames that fit") {
    Frame f;
    f.tab = 3;
    f.width = 10;
    f.height = 9;
    FrameRect big;
    big.width = 10;
    big.height = 9;
    big.bgra.resize(10 * 9 * 4);
    for (std::size_t i = 0; i < big.bgra.size(); ++i) big.bgra[i] = static_cast<std::byte>(i * 7);
    FrameRect small;
    small.x = 1;
    small.y = 2;
    small.width = small.height = 2;
    small.bgra.assign(16, std::byte{0xee});
    f.rects = {big, small};
    const std::size_t max_body = 14 + 8 + 10 * 4 * 4; // four rows of the big one a frame
    auto parts = split_frame(f, max_body);
    REQUIRE(parts.size() == 3);
    Image want, got;
    Rig r;
    auto id = r.host.open("https://example.org/", {});
    r.helper.reply(Ready{});
    r.helper.reply(f);
    r.host.poll(0);
    want = r.host.tab(id)->view;
    for (const Frame& p : parts) {
        CHECK(frame_body_size(p) <= max_body);
        CHECK(encode_message(p).size() - 4 == frame_body_size(p));
        CHECK(p.tab == 3);
        CHECK(p.width == 10);
        CHECK(p.height == 9);
    }
    CHECK(parts[0].rects.size() == 1);
    CHECK(parts[0].rects[0].height == 4);
    CHECK(parts[2].rects.size() == 2); // the last band and the small rect share a frame
    Rig r2;
    auto id2 = r2.host.open("https://example.org/", {});
    r2.helper.reply(Ready{});
    for (auto& p : parts) r2.helper.reply(p);
    r2.host.poll(0);
    got = r2.host.tab(id2)->view;
    CHECK(got.bgra == want.bgra);
    // One that fits stays one frame.
    CHECK(split_frame(f).size() == 1);
}

// The largest tab (4096x4096) paints a body just over kMaxMessage; split, each part is read and the helper lives.
TEST_CASE("browser host: a 4096x4096 full paint, split, reaches the tab") {
    Frame f;
    f.tab = 1;
    f.width = f.height = 4096;
    FrameRect all;
    all.width = all.height = 4096;
    all.bgra.assign(std::size_t{4096} * 4096 * 4, std::byte{0x11});
    f.rects.push_back(std::move(all));
    REQUIRE(frame_body_size(f) > kMaxMessage);
    Rig r;
    auto id = r.host.open("https://example.org/", {});
    REQUIRE(id == 1);
    r.helper.reply(Ready{});
    auto parts = split_frame(std::move(f));
    CHECK(parts.size() == 2);
    for (auto& p : parts) {
        CHECK(frame_body_size(p) <= kMaxMessage);
        r.helper.reply(p);
    }
    parts.clear();
    for (int i = 0; i < 1000 && !r.helper.to_client.empty(); ++i) r.host.poll(0); // kReadBudget a poll
    CHECK(r.helper.to_client.empty());
    CHECK(r.helper.alive);
    CHECK(r.helper.starts == 1);
    REQUIRE(r.host.tab(id));
    REQUIRE(r.host.tab(id)->view.width == 4096);
    CHECK(r.host.tab(id)->view.bgra.back() == std::byte{0x11});
}

// A script's tab is closed (Refused, the reason kept for its owner) once its owner lost 'web' or the server's
// [web] allow changed from the one it opened with: the helper keeps a tab's sites for its life.
TEST_CASE("web service: tabs whose owner lost 'web', or whose sites changed, are closed") {
    Rig r;
    std::vector<std::string> allow;
    app::WebServiceEnv env;
    env.browser = [&] { return &r.host; };
    env.allow = [&] { return allow; };
    app::WebService web(env);
    const auto a = web.open("script:a", plugin::kCapAll, "https://example.org/", 100, 100);
    const auto b = web.open("plugin:b", plugin::kCapAll, "https://example.org/", 100, 100);
    const auto mine = web.open("", plugin::kCapAll, "https://example.org/", 100, 100);
    REQUIRE((a && b && mine));
    r.helper.reply(Ready{});
    r.host.poll(0);
    std::uint32_t b_caps = plugin::kCapAll;
    auto caps_of = [&](const std::string& owner) { return owner == "plugin:b" ? b_caps : plugin::kCapAll; };
    web.recheck(caps_of);
    CHECK_FALSE(r.host.tab(a)->closed);
    CHECK_FALSE(r.host.tab(b)->closed);
    b_caps = plugin::kCapRead | plugin::kCapUi;
    web.recheck(caps_of);
    CHECK(r.host.tab(b)->closed);
    CHECK(r.host.tab(b)->close_reason == CloseReason::Refused);
    CHECK(web.state("plugin:b", b)->why == "it no longer has the server's 'web' permission");
    CHECK_FALSE(r.host.tab(a)->closed);
    allow = {"wiki.example.org"};
    web.recheck(caps_of);
    CHECK(r.host.tab(a)->closed);
    CHECK(web.state("script:a", a)->why == "the server changed the sites scripts may open");
    CHECK_FALSE(r.host.tab(mine)->closed); // the player's own are not the server's to limit
    CHECK(r.helper.sent_of<Close>().size() == 2);
}

// ImGui's position with no mouse is -FLT_MAX, which no int32 holds: the page is told it left at the last place it
// had, not at a converted -FLT_MAX.
TEST_CASE("web service: the mouse leaving the game's window leaves the page at its last place") {
    Rig r;
    app::WebServiceEnv env;
    env.browser = [&] { return &r.host; };
    app::WebService web(env);
    const auto id = web.open("", plugin::kCapAll, "https://example.org/", 200, 100);
    REQUIRE(id);
    r.helper.reply(Ready{});
    r.host.poll(0);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char* px = nullptr;
    int fw = 0, fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
    auto frame = [&](float x, float y) {
        io.DisplaySize = ImVec2(800, 600);
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(x, y);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(400, 300));
        ImGui::Begin("Page", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration);
        web.view("", plugin::kCapAll, id, 200, 100);
        ImGui::End();
        ImGui::Render();
    };
    frame(50, 40);
    frame(60, 45);
    frame(-FLT_MAX, -FLT_MAX);
    frame(-FLT_MAX, -FLT_MAX);
    ImGui::DestroyContext(ctx);
    const auto mice = r.helper.sent_of<Mouse>();
    REQUIRE(mice.size() >= 2);
    const Mouse& leave = mice.back();
    CHECK(leave.kind == MouseKind::Leave);
    const Mouse& move = mice[mice.size() - 2];
    CHECK(move.kind == MouseKind::Move);
    CHECK(leave.x == move.x);
    CHECK(leave.y == move.y);
    CHECK(leave.x > -1000);
}

// The player's input is never refused for a script's large message still draining: a lost button or key release
// would leave the page holding it.
TEST_CASE("browser host: input and focus messages go while the queue is over its budget") {
    Rig r;
    auto id = r.host.open("https://example.org/", script());
    r.helper.reply(Ready{});
    r.host.poll(0);
    r.helper.hold = true;
    REQUIRE(r.host.eval(id, std::string(std::size_t{40} << 20, 'x')));
    REQUIRE(r.helper.pending > BrowserHost::kSendBudget);
    std::size_t held = r.helper.pending;
    auto went = [&] {
        const bool grew = r.helper.pending > held;
        held = r.helper.pending;
        return grew;
    };
    r.host.mouse(Mouse{id, MouseKind::Up, MouseButton::Left, 1, 5, 5, 0});
    CHECK(went());
    r.host.key(Key{id, KeyKind::Up, 0x41, 0, 0});
    CHECK(went());
    r.host.wheel(Wheel{id, 5, 5, 0, -120, 0});
    CHECK(went());
    r.host.focus(id, false);
    CHECK(went());
    CHECK(r.host.nav(id, NavAction::Stop, true));
    CHECK(went());
    CHECK_FALSE(r.host.eval(id, "1+1")); // a script's message is still refused
    CHECK_FALSE(r.host.nav(id, NavAction::Reload, false)); // and so is a script's nav
    CHECK_FALSE(went());
}

// A helper with bytes waiting is ended only when it takes none of them for kStallMs: one still taking them, however
// slowly, is left alone.
TEST_CASE("browser host: a helper that takes nothing of its queue for a while is ended and restarted") {
    Rig r;
    auto id = r.host.open("https://example.org/", script());
    r.helper.reply(Ready{});
    r.host.poll(0);
    r.helper.hold = true;
    REQUIRE(r.host.eval(id, "1+1"));
    r.host.poll(1'000);
    r.helper.taken += 1; // slow, but reading
    r.host.poll(1'000 + BrowserHost::kStallMs);
    CHECK(r.helper.alive);
    r.host.poll(1'000 + 2 * BrowserHost::kStallMs - 1);
    CHECK(r.helper.alive);
    r.host.poll(1'000 + 2 * BrowserHost::kStallMs);
    CHECK_FALSE(r.helper.alive);
    CHECK(r.host.last_error().find("stopped reading") != std::string::npos);
    r.helper.hold = false;
    r.helper.pending = 0;
    r.host.poll(1'000 + 2 * BrowserHost::kStallMs + BrowserHost::kRestartDelayMs);
    CHECK(r.helper.starts == 2);
    CHECK(r.helper.sent_of<Open>().size() == 2); // the tab again
    CHECK_FALSE(r.host.tab(id)->closed);
}

// A script or plugin holds at most kMaxTabsPerOwner tabs (closed ones included, until it closes them); the player's
// own are not limited, and a closed tab's images are let go at once.
TEST_CASE("web service: a script's tabs are capped; a closed tab gives up its images") {
    using app::WebService;
    Rig r;
    WebService web({[&] { return &r.host; }, [] { return nullptr; }, [] { return std::string(); },
                    [] { return std::vector<std::string>{}; }, [] {}});
    std::vector<std::uint32_t> ids;
    for (std::size_t i = 0; i < WebService::kMaxTabsPerOwner; ++i)
        ids.push_back(web.open("script:many", plugin::kCapWeb, "https://example.org/", 64, 64));
    for (auto id : ids) CHECK(id != 0);
    std::string why;
    CHECK(web.open("script:many", plugin::kCapWeb, "https://example.org/", 64, 64, &why) == 0);
    CHECK(why.find("at most") != std::string::npos);
    CHECK(web.open("script:other", plugin::kCapWeb, "https://example.org/", 64, 64) != 0); // another owner's own
    for (std::size_t i = 0; i <= WebService::kMaxTabsPerOwner; ++i)
        CHECK(web.open("", 0, "https://example.org/", 64, 64) != 0); // the player's

    // A frame, then the helper closes the tab: its image goes; it still counts until its owner closes it.
    r.helper.reply(Ready{});
    Frame f;
    f.tab = ids[0];
    f.width = 2;
    f.height = 2;
    FrameRect all;
    all.width = all.height = 2;
    all.bgra.assign(16, std::byte{1});
    f.rects.push_back(all);
    r.helper.reply(f);
    r.host.poll(0);
    CHECK(r.host.tab(ids[0])->view.bgra.size() == 16);
    r.helper.reply(Closed{ids[0], CloseReason::Failed, "gone"});
    r.host.poll(0);
    REQUIRE(r.host.tab(ids[0]));
    CHECK(r.host.tab(ids[0])->closed);
    CHECK(r.host.tab(ids[0])->view.bgra.empty());
    CHECK(r.host.tab(ids[0])->view.width == 0);
    CHECK(web.open("script:many", plugin::kCapWeb, "https://example.org/", 64, 64) == 0);
    web.close("script:many", ids[0]);
    CHECK(web.open("script:many", plugin::kCapWeb, "https://example.org/", 64, 64) != 0);
}

TEST_CASE("browser host: a dead helper is restarted after a second with the tabs; after four deaths a minute, not") {
    Rig r;
    auto id = r.host.open("https://example.org/", {});
    r.helper.reply(Ready{});
    r.helper.reply(Address{id, "https://example.org/later"});
    r.host.poll(0);
    r.helper.alive = false;
    r.host.poll(100);
    CHECK(r.helper.starts == 1);
    r.host.poll(100 + BrowserHost::kRestartDelayMs - 1);
    CHECK(r.helper.starts == 1);
    r.helper.sent.clear();
    r.host.poll(100 + BrowserHost::kRestartDelayMs);
    CHECK(r.helper.starts == 2);
    REQUIRE(r.helper.sent_of<Open>().size() == 1);
    CHECK(r.helper.sent_of<Open>()[0].url == "https://example.org/later"); // where it was
    CHECK(r.helper.sent_of<Open>()[0].tab == id);

    std::uint64_t now = 2000;
    for (int death = 2; death <= BrowserHost::kMaxRestarts + 1; ++death) {
        r.helper.alive = false;
        r.host.poll(now);
        now += BrowserHost::kRestartDelayMs;
        r.host.poll(now);
    }
    CHECK_FALSE(r.host.available());
    CHECK(r.host.tab(id)->closed);
    CHECK(r.host.tab(id)->close_reason == CloseReason::Crashed);
    CHECK(r.host.open("https://example.org/", {}) == 0);
}

TEST_CASE("browser host: deaths a minute apart are restarted every time") {
    Rig r;
    r.host.open("https://example.org/", {});
    std::uint64_t now = 0;
    for (int i = 0; i < 6; ++i) {
        r.helper.alive = false;
        r.host.poll(now);
        now += BrowserHost::kRestartWindowMs + BrowserHost::kRestartDelayMs;
        r.host.poll(now);
    }
    CHECK(r.host.available());
    CHECK(r.helper.starts == 7);
}

TEST_CASE("browser host: garbage from the helper counts as a death; another version gives up") {
    Rig r;
    r.host.open("https://example.org/", {});
    for (std::byte b : {std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0xee}, std::byte{0}})
        r.helper.to_client.push_back(b);
    r.host.poll(0);
    CHECK_FALSE(r.helper.alive);
    r.host.poll(BrowserHost::kRestartDelayMs);
    CHECK(r.helper.starts == 2);

    Rig v;
    auto id = v.host.open("https://example.org/", {});
    v.helper.reply(Ready{kProtocolVersion + 1});
    v.host.poll(0);
    CHECK_FALSE(v.host.available());
    CHECK(v.host.tab(id)->closed);
    CHECK(v.host.last_error().find("version") != std::string::npos);
}

#ifdef HAILSTONE_MOCK_WEB_HELPER
namespace {

// Polls until `done` or 10 s (a real process answers through real pipes).
template <typename F> bool pump(BrowserHost& host, std::uint64_t now, F&& done) {
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < until) {
        host.poll(now);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

TEST_CASE("browser host: the stand-in helper over real pipes: a tab, a click, messages, a crash and a restart") {
    BrowserHost host([] { return process_transport({HAILSTONE_MOCK_WEB_HELPER}); });
    TabOptions o = script();
    o.width = 64;
    o.height = 32;
    auto id = host.open("https://example.org/", o);
    REQUIRE(id != 0);
    REQUIRE(pump(host, 0, [&] { return host.tab(id)->view.width == 64; }));
    const Tab* t = host.tab(id);
    CHECK(t->title == "Mock https://example.org/");
    CHECK(t->view.bgra[(5 * 64 + 7) * 4 + 0] == std::byte{7}); // blue = x
    CHECK(t->view.bgra[(5 * 64 + 7) * 4 + 1] == std::byte{5}); // green = y

    host.mouse(Mouse{id, MouseKind::Down, MouseButton::Left, 1, 7, 5, 0});
    REQUIRE(pump(host, 0, [&] { return t->view.bgra[(5 * 64 + 7) * 4] == std::byte{0xff}; }));

    host.post(id, "ping");
    std::vector<std::string> got;
    REQUIRE(pump(host, 0, [&] {
        for (auto& m : host.take_messages(id)) got.push_back(m);
        return !got.empty();
    }));
    CHECK(got == std::vector<std::string>{"echo:ping"});

    host.resize(id, 16, 16);
    REQUIRE(pump(host, 0, [&] { return t->view.width == 16; }));

    const std::uint32_t before = t->view.serial;
    host.eval(id, "crash");
    REQUIRE(pump(host, 0, [&] { return !host.last_error().empty(); }));
    CHECK(host.last_error().find("exited") != std::string::npos);
    // The restart (a second later by the host's clock) opens the tab again, at its size now.
    REQUIRE(pump(host, BrowserHost::kRestartDelayMs, [&] { return t->view.serial > before; }));
    CHECK(t->view.width == 16);
    host.post(id, "again");
    got.clear();
    REQUIRE(pump(host, BrowserHost::kRestartDelayMs, [&] {
        for (auto& m : host.take_messages(id)) got.push_back(m);
        return !got.empty();
    }));
    CHECK(got == std::vector<std::string>{"echo:again"});

    auto off = host.open("https://example.org/?redirect=https://evil.com/", script({"example.org"}));
    REQUIRE(off != 0);
    REQUIRE(pump(host, BrowserHost::kRestartDelayMs, [&] { return host.tab(off)->closed; }));
    CHECK(host.tab(off)->close_reason == CloseReason::Refused);
    host.shutdown();
}

// Several large evals in one frame (each under kMaxMessage, together past HelperProcess::kMaxPending): the first is
// queued, the rest refused, and the helper, still reading, keeps running and answers afterwards.
TEST_CASE("browser host: the stand-in helper: large evals in one frame leave it running") {
    BrowserHost host([] { return process_transport({HAILSTONE_MOCK_WEB_HELPER}); });
    auto id = host.open("https://example.org/", script());
    REQUIRE(id != 0);
    REQUIRE(pump(host, 0, [&] { return host.tab(id)->view.width != 0; }));
    const std::string big(std::size_t{60} << 20, 'x');
    int sent = 0;
    for (int i = 0; i < 3; ++i) sent += host.eval(id, big) ? 1 : 0;
    CHECK(sent == 1);
    REQUIRE(pump(host, 0, [&] { return host.post(id, "after"); }));
    std::vector<std::string> got;
    REQUIRE(pump(host, 0, [&] {
        for (auto& m : host.take_messages(id)) got.push_back(m);
        return !got.empty();
    }));
    CHECK(got == std::vector<std::string>{"echo:after"});
    CHECK(host.last_error().empty());
    CHECK_FALSE(host.tab(id)->closed);
    host.shutdown();
}

TEST_CASE("helper process: Shutdown and the end of its input let it quit by itself") {
    platform::HelperProcess p;
    std::string error;
    REQUIRE(p.start({HAILSTONE_MOCK_WEB_HELPER}, error));
    p.write(encode_message(Shutdown{}));
    p.stop(false);
    CHECK(p.wait_exit(10'000));
    CHECK(p.exit_code() == 0);
}

// A helper that stops reading its input: what the client queues for it is held to a limit, past which the helper is
// ended (and so, in a BrowserHost, seen to die and restarted) rather than the queue growing.
TEST_CASE("helper process: a helper that does not read is ended once its queue is full") {
    platform::HelperProcess p(std::size_t{1} << 20);
    std::string error;
    REQUIRE(p.start({HAILSTONE_MOCK_WEB_HELPER, "--stall", "1"}, error));
    const std::vector<std::byte> chunk(std::size_t{64} << 10, std::byte{0x55});
    std::size_t written = 0;
    // (Past the pipe's own buffer and the limit, a queue that is empty again is one the helper's end took.)
    while (written < (std::size_t{64} << 20)) {
        p.write(chunk);
        written += chunk.size();
        if (written > (std::size_t{2} << 20) && p.pending_write() == 0) break;
    }
    CHECK(written < (std::size_t{64} << 20));
    CHECK(p.pending_write() == 0);
    CHECK(p.wait_exit(10'000)); // ended
}

TEST_CASE("browser host: a helper of another version is given up on") {
    BrowserHost host([] { return process_transport({HAILSTONE_MOCK_WEB_HELPER, "--version", "99"}); });
    auto id = host.open("https://example.org/", {});
    REQUIRE(id != 0);
    REQUIRE(pump(host, 0, [&] { return !host.available(); }));
    CHECK(host.tab(id)->closed);
}

TEST_CASE("browser host: a helper that cannot start") {
    BrowserHost host([] { return process_transport({"/nonexistent/hailstone-web-helper"}); });
    std::string why;
    std::uint32_t id = host.open("https://example.org/", {}, &why);
    // SDL reports a missing program at once on some systems and as an early exit on others.
    if (id == 0) {
        CHECK_FALSE(host.available());
    } else {
        REQUIRE(pump(host, 0, [&] { return !host.last_error().empty(); }));
    }
}
#endif

#ifdef HAILSTONE_WEB_HELPER
// The real helper (tools/hailstone-web, CEF). A script's tab on about:blank, coloured by its own script: the whole
// path (start, a tab, painting, the page's script, the bridge, the rules, shutdown) runs without a network or a
// server. (A script's tab may not open 127.0.0.1; the player's error page test is the next one.)
TEST_CASE("browser host: the CEF helper draws a page and passes messages both ways") {
    test::ScratchDir data("hailstone-web");
    BrowserHost host([&] { return process_transport({HAILSTONE_WEB_HELPER, "--data=" + data.path().string()}); });
    TabOptions o = script({"example.net"});
    o.width = 320;
    o.height = 200;
    o.profile = (data.path() / "server").string();
    auto id = host.open("about:blank", o);
    REQUIRE(id != 0);
    const Tab* t = host.tab(id);
    REQUIRE(pump(host, 0, [&] { return t->view.width == 320 && !t->loading; }));
    host.eval(id, "document.documentElement.style.background = '#c03';"
                  "document.body.innerHTML = '<p style=\"color:#fff\">a page</p>';");
    // Painted: something other than a blank image.
    auto drawn = [&] {
        for (std::size_t i = 0; i + 4 <= t->view.bgra.size(); i += 4)
            if (t->view.bgra[i] != std::byte{0xff} || t->view.bgra[i + 1] != std::byte{0xff}) return true;
        return false;
    };
    REQUIRE(pump(host, 0, drawn));
    CHECK(t->view.height == 200);
    CHECK_FALSE(t->closed);

    host.eval(id, "hailstone.onmessage = function(m) { hailstone.post('got ' + m); }; hailstone.post('ready');");
    std::vector<std::string> got;
    REQUIRE(pump(host, 0, [&] {
        for (auto& m : host.take_messages(id)) got.push_back(m);
        return !got.empty();
    }));
    CHECK(got.front() == "ready");
    host.post(id, "a \"quoted\" line\nand more");
    got.clear();
    REQUIRE(pump(host, 0, [&] {
        for (auto& m : host.take_messages(id)) got.push_back(m);
        return !got.empty();
    }));
    CHECK(got.front() == "got a \"quoted\" line\nand more");

    host.resize(id, 160, 100);
    REQUIRE(pump(host, 0, [&] { return t->view.width == 160; }));
    // A script's tab is held to its sites by the helper too: the navigation is refused, the tab stays where it is.
    host.eval(id, "location.href = 'https://example.org/';");
    bool refused = false;
    REQUIRE(pump(host, 0, [&] {
        for (const auto& c : t->console) refused |= c.text.find("refused https://example.org/") != std::string::npos;
        return refused;
    }));
    CHECK_FALSE(t->closed);
    // Frames inside the page and its requests are held to the sites too, and never reach this computer.
    host.eval(id, "document.body.innerHTML += '<iframe src=\"https://example.org/frame\"></iframe>';"
                  "fetch('http://127.0.0.1:9/probe').catch(function() {});"
                  "var i = new Image(); i.src = 'https://tracker.example.com/p.gif';");
    auto console_has = [&](const std::string& text) {
        for (const auto& c : t->console)
            if (c.text.find(text) != std::string::npos) return true;
        return false;
    };
    REQUIRE(pump(host, 0, [&] {
        return console_has("refused https://example.org/frame") &&
               console_has("refused a request to http://127.0.0.1:9/probe") &&
               console_has("refused a request to https://tracker.example.com/p.gif");
    }));
    CHECK_FALSE(t->closed);
    host.shutdown();
}

TEST_CASE("browser host: the CEF helper quits by itself at Shutdown, with a tab open (its cookies written)") {
    test::ScratchDir data("hailstone-web-quit");
    platform::HelperProcess p;
    std::string error;
    REQUIRE(p.start({HAILSTONE_WEB_HELPER, "--data=" + data.path().string()}, error));
    Open o;
    o.tab = 1;
    o.width = 64;
    o.height = 64;
    o.profile = (data.path() / "server").string();
    o.url = "http://127.0.0.1:9/";
    p.write(encode_message(Hello{}));
    p.write(encode_message(o));
    // Until the page has drawn: the browser is up.
    MessageReader reader;
    std::vector<std::byte> buf(1 << 16);
    bool drawn = false;
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!drawn && std::chrono::steady_clock::now() < until) {
        p.flush();
        const std::size_t n = p.read(buf);
        reader.feed(std::span<const std::byte>(buf).first(n));
        while (auto m = reader.next()) drawn |= std::holds_alternative<Frame>(*m);
        if (!n) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(drawn);
    p.write(encode_message(Shutdown{}));
    p.stop(false);
    CHECK(p.wait_exit(BrowserHost::kShutdownGraceMs * 3));
    CHECK(p.exit_code() == 0);
    CHECK(std::filesystem::exists(data.path() / "server")); // the profile's folder, where its cookies go
}
#endif
