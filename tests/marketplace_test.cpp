// The marketplace (CMarketplaceWnd): its packets, the window's rules and the connection's open, states, wallet and
// purchase flow.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>

#include "core/byte_io.h"
#include "game/eq_strings.h"
#include "game/marketplace.h"
#include "mock_server/harness.h"
#include "net/item_packets.h"
#include "net/marketplace_packets.h"

using namespace hailstone;
using namespace hailstone::net;

namespace {

std::uint32_t u32(const AppPacket& p, std::size_t at) {
    ByteReader r(p.data.data(), p.data.size());
    r.seek(at);
    return r.le<std::uint32_t>();
}

void put32(std::vector<std::byte>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
}
void put8(std::vector<std::byte>& b, std::uint8_t v) { b.push_back(static_cast<std::byte>(v)); }
void puts0(std::vector<std::byte>& b, const std::string& s) {
    for (char c : s) b.push_back(static_cast<std::byte>(c));
    b.push_back(std::byte{0});
}

MarketplaceEntry sample_entry() {
    MarketplaceEntry e;
    e.id = 77;
    e.price = 250;
    e.icon = 3621;
    e.max_quantity = 5;
    e.name = "Fine Steel Sword";
    e.category = "Weapons/Swords";
    e.description = "A <i>very</i> sharp sword.";
    e.is_new = true;
    e.purchase_message = "Thank you!";
    e.item_of_week = true;
    e.item_of_week_rank = 3;
    e.is_featured = true;
    e.was_price = 400;
    e.available_until = 1000 + 3600;
    e.sale_until = -1;
    e.u2108 = 9;
    e.money_type = 0;
    return e;
}

} // namespace

TEST_CASE("marketplace catalog: every field round-trips; relative seconds become absolute times") {
    const std::int64_t now = 1000;
    AppPacket p = encode_marketplace_items({sample_entry(), MarketplaceEntry{}}, now);
    CHECK(p.opcode == kOpMarketplaceItems);
    auto v = decode_marketplace_items(p, now);
    REQUIRE(v);
    REQUIRE(v->size() == 2);
    const auto& e = (*v)[0];
    CHECK(e.id == 77);
    CHECK(e.price == 250);
    CHECK(e.icon == 3621);
    CHECK(e.max_quantity == 5);
    CHECK(e.name == "Fine Steel Sword");
    CHECK(e.category == "Weapons/Swords");
    CHECK(e.description == "A <i>very</i> sharp sword.");
    CHECK(e.is_new);
    CHECK(e.purchase_message == "Thank you!");
    CHECK(e.item_of_week);
    CHECK(e.item_of_week_rank == 3);
    CHECK(e.is_featured);
    CHECK(e.was_price == 400);
    CHECK(e.available_until == 4600);
    CHECK(e.sale_until == -1); // 0 on the wire: never
    CHECK(e.u2108 == 9);
    // The second, all defaults: its timestamps were 0 on the wire too.
    CHECK((*v)[1].available_until == -1);
    CHECK((*v)[1].item_of_week_rank == -1);
}

TEST_CASE("marketplace catalog: strings are cut to the node's buffers but skip their whole length") {
    std::vector<std::byte> b;
    put32(b, 1);
    put32(b, 5); // id
    put32(b, 1);
    put32(b, 0);
    put32(b, 0);
    puts0(b, std::string(300, 'n')); // name: 127 kept
    puts0(b, "Cat");
    AppPacket p(kOpMarketplaceItems, b);
    auto v = decode_marketplace_items(p, 0);
    REQUIRE(v);
    REQUIRE(v->size() == 1);
    CHECK((*v)[0].name == std::string(127, 'n'));
    CHECK((*v)[0].category == "Cat");
}

TEST_CASE("marketplace catalog: a short entry keeps its defaults; a missing timestamp reads the arrival time") {
    std::vector<std::byte> b;
    put32(b, 3); // three promised, one present
    put32(b, 9);
    put32(b, 10);
    AppPacket p(kOpMarketplaceItems, b);
    const std::int64_t now = 500;
    auto v = decode_marketplace_items(p, now);
    REQUIRE(v);
    REQUIRE(v->size() == 1);
    CHECK((*v)[0].id == 9);
    CHECK((*v)[0].price == 10);
    CHECK((*v)[0].icon == 0);
    CHECK((*v)[0].name.empty());
    // time(&t) then t read from the packet: with nothing there t is still the time itself.
    CHECK((*v)[0].available_until == 1000);
    CHECK((*v)[0].sale_until == 1000);
    CHECK((*v)[0].u2108 == 500);
    // No count at all is refused.
    CHECK_FALSE(decode_marketplace_items(AppPacket(kOpMarketplaceItems, {std::byte{1}}), 0));
}

TEST_CASE("marketplace catalog: an unterminated string at the end reads as empty and leaves the cursor") {
    std::vector<std::byte> b;
    put32(b, 1);
    put32(b, 1);
    put32(b, 2);
    put32(b, 3);
    put32(b, 4);
    for (char c : std::string("abc")) b.push_back(static_cast<std::byte>(c)); // no NUL
    auto v = decode_marketplace_items(AppPacket(kOpMarketplaceItems, b), 0);
    REQUIRE(v);
    REQUIRE(v->size() == 1);
    CHECK((*v)[0].name.empty());
    // The cursor stayed on the string, so the u8 fields after the strings read its bytes.
    CHECK((*v)[0].flag8e0 == 'a');
    CHECK((*v)[0].is_new);
}

TEST_CASE("marketplace catalog: a count past 10,000 entries is refused") {
    std::vector<std::byte> b;
    put32(b, 10000);
    put32(b, 7);
    auto v = decode_marketplace_items(AppPacket(kOpMarketplaceItems, b), 0);
    REQUIRE(v);
    CHECK(v->size() == 1);
    b.clear();
    put32(b, 10001);
    b.resize(1u << 20); // then a megabyte of zeros: near-empty entries
    CHECK_FALSE(decode_marketplace_items(AppPacket(kOpMarketplaceItems, b), 0));
}

TEST_CASE("marketplace funding tiers round-trip") {
    MarketplaceFund f;
    f.amount = 1000;
    f.u0c = 2;
    f.text14 = "1000 SC";
    f.text54 = "sku-1000";
    auto v = decode_marketplace_funding(encode_marketplace_funding({f}));
    REQUIRE(v);
    REQUIRE(v->size() == 1);
    CHECK((*v)[0].amount == 1000);
    CHECK((*v)[0].u0c == 2);
    CHECK((*v)[0].text14 == "1000 SC");
    CHECK((*v)[0].text54 == "sku-1000");
}

TEST_CASE("marketplace states: length-prefixed categories; a short entry keeps the previous values") {
    MarketplaceRowState a{77, 1, 1001, 0, "Sale"};
    AppPacket p = encode_marketplace_states({a});
    auto v = decode_marketplace_states(p);
    REQUIRE(v);
    REQUIRE(v->size() == 1);
    CHECK((*v)[0].entry_id == 77);
    CHECK((*v)[0].state == 1);
    CHECK((*v)[0].item_id == 1001);
    CHECK((*v)[0].category == "Sale");

    std::vector<std::byte> b;
    put32(b, 2);
    put32(b, 5);
    put32(b, 1);
    put32(b, 600);
    put32(b, 0);
    put32(b, 0); // no category
    put32(b, 6); // the second: only its entry id
    v = decode_marketplace_states(AppPacket(kOpMarketplaceStates, b));
    REQUIRE(v);
    REQUIRE(v->size() == 2);
    CHECK((*v)[1].entry_id == 6);
    CHECK((*v)[1].state == 1);
    CHECK((*v)[1].item_id == 600);
    // A missing count is 0.
    v = decode_marketplace_states(AppPacket(kOpMarketplaceStates, {}));
    REQUIRE(v);
    CHECK(v->empty());
}

TEST_CASE("marketplace states: a category longer than the packet takes what is left and ends the list") {
    std::vector<std::byte> b;
    put32(b, 3);
    put32(b, 5);
    put32(b, 1);
    put32(b, 600);
    put32(b, 0);
    put32(b, 1000); // a length far past the end
    for (char c : std::string("Abc")) b.push_back(static_cast<std::byte>(c));
    auto v = decode_marketplace_states(AppPacket(kOpMarketplaceStates, b));
    REQUIRE(v);
    REQUIRE(v->size() == 1);
    CHECK((*v)[0].category == "Abc");
}

TEST_CASE("marketplace states: a huge count over a few stray bytes ends at once") {
    std::vector<std::byte> b;
    put32(b, 0xffffffffu);
    put8(b, 1);
    put8(b, 2);
    auto v = decode_marketplace_states(AppPacket(kOpMarketplaceStates, b));
    REQUIRE(v);
    CHECK(v->size() == 1);
}

TEST_CASE("marketplace wallet: fixed offsets, 0x54 bytes at least") {
    MarketplaceWallet w;
    w.balance = 1234;
    w.zip_on_file = true;
    AppPacket p = encode(w);
    CHECK(p.data.size() == kMarketplaceWalletSize);
    CHECK(u32(p, 0x48) == 1234);
    CHECK(u32(p, 0x50) == 1);
    auto v = decode_marketplace_wallet(p);
    REQUIRE(v);
    CHECK(v->status == 0);
    CHECK(v->balance == 1234);
    CHECK(v->zip_on_file);
    CHECK(v->type == 1);
    p.data.resize(0x53);
    CHECK_FALSE(decode_marketplace_wallet(p));
}

TEST_CASE("marketplace purchase: 80 bytes, entry id +0x48, quantity +0x4c; the result's offsets") {
    AppPacket p = encode_marketplace_purchase(77, 3);
    CHECK(p.opcode == kOpMarketplacePurchase);
    REQUIRE(p.data.size() == kMarketplacePurchaseSize);
    CHECK(u32(p, 0x48) == 77);
    CHECK(u32(p, 0x4c) == 3);
    CHECK(u32(p, 0x40) == 0);

    MarketplacePurchaseResult r;
    r.entry_id = 77;
    r.success = true;
    r.balance = 500;
    r.placed = 2;
    r.code = 0;
    r.note_f0 = "first";
    r.note_4f0 = "second";
    auto v = decode_marketplace_purchase_result(encode(r));
    REQUIRE(v);
    CHECK(v->entry_id == 77);
    CHECK(v->success);
    CHECK_FALSE(v->station_cash);
    CHECK(v->balance == 500);
    CHECK(v->placed == 2);
    CHECK(v->note_f0 == "first");
    CHECK(v->note_4f0 == "second");
    // A short result reads zeros for what it lacks.
    AppPacket s(kOpMarketplacePurchaseResult, std::vector<std::byte>(0x44));
    v = decode_marketplace_purchase_result(s);
    REQUIRE(v);
    CHECK_FALSE(v->success);
    CHECK(v->balance == 0);
    CHECK(v->note_f0.empty());
}

TEST_CASE("marketplace requests: the bare opcodes, the 76-byte hello and the window's preview request") {
    CHECK(encode_marketplace_request(kOpMarketplaceReady).data.empty());
    AppPacket h = encode_marketplace_hello();
    REQUIRE(h.data.size() == kMarketplaceHelloSize);
    CHECK(u32(h, 0x48) == 1);
    CHECK(u32(h, 0x40) == 0);
    AppPacket q = encode_marketplace_preview_request(1001);
    REQUIRE(q.data.size() == 80);
    CHECK(u32(q, 0) == 1001);
    CHECK(u32(q, 0x40) == 0xffffffffu);
    CHECK(static_cast<std::uint8_t>(q.data[0x4c]) == 1);
    CHECK(static_cast<std::uint8_t>(q.data[0x4d]) == 0);
}

TEST_CASE("marketplace rules: CanBuy, the quantity buttons, prices and funds") {
    MarketplaceEntry e;
    e.price = 100;
    CHECK(game::marketplace_can_buy(e, 100));
    CHECK_FALSE(game::marketplace_can_buy(e, 99));
    CHECK(game::marketplace_can_buy(e, game::kMarketplaceFundsUnknown)); // compared as held
    e.kind = 4;
    CHECK(game::marketplace_can_buy(e, 0));
    e.kind = 6;
    CHECK(game::marketplace_can_buy(e, 0));
    e.kind = 0;

    // Up: (qty + 1) * price within the wallet, under 10 and the entry's max; never for kind 6.
    CHECK(game::marketplace_can_raise_quantity(e, 1, 200));
    CHECK_FALSE(game::marketplace_can_raise_quantity(e, 2, 200));
    CHECK_FALSE(game::marketplace_can_raise_quantity(e, 10, 0xfffffff0u));
    CHECK(game::marketplace_can_raise_quantity(e, 9, 0xfffffff0u));
    e.max_quantity = 3;
    CHECK(game::marketplace_can_raise_quantity(e, 2, 1000));
    CHECK_FALSE(game::marketplace_can_raise_quantity(e, 3, 1000));
    e.max_quantity = 0;
    e.kind = 6;
    CHECK_FALSE(game::marketplace_can_raise_quantity(e, 1, 1000));
    e.kind = 0;
    // A 32-bit product, wrapping as the original's does.
    e.price = 0x80000000u;
    CHECK(game::marketplace_can_raise_quantity(e, 1, 0)); // 2 * 0x80000000 wraps to 0
    CHECK_FALSE(game::marketplace_can_lower_quantity(1));
    CHECK(game::marketplace_can_lower_quantity(2));

    CHECK(game::marketplace_price_text(250, 0) == "250");
    CHECK(game::marketplace_price_text(3000, 1) == "30.00");
    CHECK(game::marketplace_price_text(5, 2) == "N/A");
    CHECK(game::marketplace_funds_text(game::kMarketplaceFundsUnknown) == "0");
    CHECK(game::marketplace_funds_text(1234) == "1234");
}

TEST_CASE("marketplace rules: the item of the week is the highest rank, the first on a tie") {
    std::vector<MarketplaceEntry> c(4);
    CHECK(game::marketplace_item_of_week(c) == -1);
    c[1].item_of_week = true;
    c[1].item_of_week_rank = 5;
    c[2].item_of_week = true;
    c[2].item_of_week_rank = 5;
    c[3].item_of_week_rank = 99; // not eligible
    CHECK(game::marketplace_item_of_week(c) == 1);
    c[2].item_of_week_rank = 6;
    CHECK(game::marketplace_item_of_week(c) == 2);
}

TEST_CASE("marketplace rules: a purchase's answer text") {
    game::EqStrings s; // no table: an id formats as "[id]"
    MarketplaceEntry e;
    e.purchase_message = "Enjoy";
    MarketplacePurchaseResult r;
    r.success = true;
    r.placed = 1;
    std::string t = game::marketplace_purchase_text(s, r, &e);
    CHECK(t == "Enjoy\n\n[5361] An item has been placed in your inventory!");
    r.station_cash = true;
    r.placed = 0;
    r.note_f0 = "A";
    r.note_4f0 = "B";
    t = game::marketplace_purchase_text(s, r, nullptr);
    CHECK(t == "[7621]<BR><BR>\n\nA<BR><BR>\n\nB");
    CHECK(game::marketplace_strip_breaks(t) == "[7621]\n\nA\n\nB");
    // Failures by code; the success byte decides, not the code.
    r = {};
    for (auto [code, id] : std::vector<std::pair<std::uint32_t, std::string>>{
             {1, "[7622]"}, {4, "[7622]"}, {2, "[7623]"}, {3, "[7624]"}, {6, "[7632]"}, {8, "[714]"}, {5, "[7626]"},
             {0, "[5361]"}, {99, "[7626]"}}) {
        r.code = code;
        CHECK(game::marketplace_purchase_text(s, r, nullptr) == id);
    }
}

// ---------------------------------------------------------------- the window's model

namespace {

MarketplaceEntry item(std::uint32_t id, const std::string& name, const std::string& path, std::uint32_t price) {
    MarketplaceEntry e;
    e.id = id;
    e.name = name;
    e.category = path;
    e.price = price;
    return e;
}

std::vector<std::string> tree_names(const game::MarketplaceWindow& w) {
    std::vector<std::string> out;
    for (const auto& t : w.tree) out.push_back(std::string(static_cast<std::size_t>(t.depth) * 2, ' ') + t.name);
    return out;
}

std::vector<std::uint32_t> shown_ids(const game::MarketplaceWindow& w, const std::vector<MarketplaceEntry>& c) {
    std::vector<std::uint32_t> out;
    for (int i : w.visible) out.push_back(c[static_cast<std::size_t>(w.rows[static_cast<std::size_t>(i)].entry)].id);
    return out;
}

void feed(game::MarketplaceWindow& w, const std::vector<MarketplaceEntry>& c, const std::vector<MarketplaceRowState>& s) {
    bool changed = false, cats = false;
    for (const auto& r : s) {
        auto x = w.apply_state(c, r);
        changed |= x.changed;
        cats |= x.categories;
    }
    if (cats) w.rebuild_tree(true);
    if (changed || !w.selected_entry(c)) {
        w.refilter(c);
        w.sync_shelf(c, true);
    }
}

} // namespace

TEST_CASE("marketplace window: the catalog alone shows nothing; the state feed builds the tree and the rows") {
    std::vector<MarketplaceEntry> c = {item(10, "Helm", "Armor/Head", 50), item(11, "Boots", "Armor/Feet", 30),
                                       item(12, "Torch", "Light", 5), item(13, "Hidden", "Armor/Head", 1),
                                       item(14, "Studio Hat", "Player Studio/Armor/Head", 9)};
    game::MarketplaceWindow w;
    w.populate(c, true);
    // Built-ins first: New and Featured (id 0), Player Studio (id 2), All Items (id 1); then the paths, from id 3.
    REQUIRE(w.categories.size() >= 7);
    CHECK(w.categories[0].name == "New and Featured");
    CHECK(w.categories[1].id == 2);
    CHECK(w.categories[2].id == 1);
    CHECK(w.categories[3].path == "Armor");
    CHECK(w.categories[3].parent == 2); // under All Items
    CHECK(w.categories[4].path == "Armor/Head");
    // The leading "Player Studio/" is stripped: entry 14 is in Armor/Head too.
    CHECK(w.rows[4].category == w.rows[0].category);
    // Every row is in state 2: nothing shown, only New and Featured in the tree.
    CHECK(w.visible.empty());
    CHECK(tree_names(w) == std::vector<std::string>{"New and Featured"});
    CHECK(w.selected == -1);

    feed(w, c, {{10, 0, 1001, 0, ""}, {11, 0, 0, 0, ""}, {12, 1, 0, 0, ""}, {14, 0, 0, 0, "Maker"}});
    CHECK(tree_names(w) == std::vector<std::string>{"New and Featured", "Player Studio", "  Armor", "    Head",
                                                    "All Items", "  Armor", "    Head", "    Feet", "  Light"});
    CHECK(w.expanded.count(1) == 1); // All Items open
    // No category: everything not in state 2, in catalog order; the first is selected with quantity 1.
    CHECK(shown_ids(w, c) == std::vector<std::uint32_t>{10, 11, 12, 14});
    REQUIRE(w.selected_entry(c));
    CHECK(w.selected_entry(c)->id == 10);
    CHECK(w.quantity == 1);
    REQUIRE(w.preview_request);
    CHECK(*w.preview_request == 1001);
    CHECK(w.rows[4].maker == "Maker");

    // A category shows its descendants; the Player Studio mirror shows the maker's row there too.
    w.click_category(c, 3); // Armor (under All Items)
    CHECK(shown_ids(w, c) == std::vector<std::uint32_t>{10, 11, 14});
    CHECK(w.view == game::MarketplaceView::Icons);
    w.click_category(c, 2); // Player Studio
    CHECK(shown_ids(w, c) == std::vector<std::uint32_t>{14});
    w.click_category(c, 0); // New and Featured: no rows of its own, the landing page
    CHECK(w.visible.empty());
    CHECK(w.view == game::MarketplaceView::Landing);

    // Back to state 2: the row hides and an emptied category leaves the tree.
    w.click_category(c, 1);
    feed(w, c, {{12, 2, 0, 0, ""}});
    CHECK(shown_ids(w, c) == std::vector<std::uint32_t>{10, 11, 14});
    bool light = false;
    for (const auto& t : w.tree) light |= t.name == "Light";
    CHECK_FALSE(light);
}

TEST_CASE("marketplace window: sort, find, quantity, navigate and the open's reset") {
    std::vector<MarketplaceEntry> c = {item(1, "beta", "A", 30), item(2, "Alpha", "A", 10), item(3, "gamma", "B", 20)};
    c[2].is_featured = true;
    c[1].max_quantity = 4;
    game::MarketplaceWindow w;
    w.populate(c, true);
    feed(w, c, {{1, 0, 0, 0, ""}, {2, 0, 0, 0, ""}, {3, 0, 0, 0, ""}});

    // Default: new first, then featured, then catalog order.
    w.sort(c, 0);
    CHECK(shown_ids(w, c) == std::vector<std::uint32_t>{3, 1, 2});
    w.sort(c, 1); // names, byte order ("Alpha" < "beta" < "gamma")
    CHECK(shown_ids(w, c) == std::vector<std::uint32_t>{2, 1, 3});
    w.sort(c, 2);
    CHECK(shown_ids(w, c) == std::vector<std::uint32_t>{3, 1, 2});
    w.sort(c, 3); // price up
    CHECK(shown_ids(w, c) == std::vector<std::uint32_t>{2, 3, 1});
    w.sort(c, 4);
    CHECK(shown_ids(w, c) == std::vector<std::uint32_t>{1, 3, 2});

    // Find: case-insensitive on the name, 500 ms apart.
    CHECK(w.find(c, "ALP", 1000));
    CHECK(shown_ids(w, c) == std::vector<std::uint32_t>{2});
    CHECK_FALSE(w.highlight);
    CHECK_FALSE(w.find(c, "", 1400)); // debounced
    CHECK(w.find(c, "", 1600));
    CHECK(w.visible.size() == 3);
    CHECK(*w.highlight == 1);

    // SetQuantity on entry 2 (max 4): clamped to 1..4; unsigned, so a negative typed number takes the max.
    w.navigate(c, 2, true);
    REQUIRE(w.selected_entry(c));
    CHECK(w.selected_entry(c)->id == 2);
    w.set_quantity(c, 9);
    CHECK(w.quantity == 4);
    w.set_quantity(c, 0);
    CHECK(w.quantity == 1);
    w.set_quantity(c, static_cast<std::uint32_t>(-3));
    CHECK(w.quantity == 4);

    // A featured entry navigates to the landing page in game.
    w.navigate(c, 3, true);
    CHECK(w.view == game::MarketplaceView::Landing);
    CHECK(w.current == 0);

    // The open: the Default sort selects the featured entry first, so the landing page.
    w.reset_view(c, true);
    CHECK(w.view == game::MarketplaceView::Landing);
    CHECK(w.selected_entry(c)->id == 3);
    c[2].is_featured = false;
    w.reset_view(c, true);
    CHECK(w.view == game::MarketplaceView::Icons);
    CHECK(w.categories[static_cast<std::size_t>(w.current)].name == "All Items"); // (the original: array[1])
    CHECK(*w.highlight == 1);
}

TEST_CASE("marketplace rules: countdowns, tile timers, totals, the description, links") {
    CHECK_FALSE(game::marketplace_countdown(-1, 100));
    CHECK_FALSE(game::marketplace_countdown(100, 100));
    CHECK(*game::marketplace_countdown(100 + 3661, 100) == "01:01:01");
    CHECK(*game::marketplace_countdown(100 + 90061, 100) == "1:01:01:01");
    MarketplaceEntry e;
    e.available_until = 500;
    e.sale_until = 400;
    CHECK(game::marketplace_tile_timers(e) == std::pair<std::int64_t, std::int64_t>{-1, 400});
    e.sale_until = 500;
    CHECK(game::marketplace_tile_timers(e) == std::pair<std::int64_t, std::int64_t>{500, -1});
    e.price = 7;
    CHECK(game::marketplace_total_text(e, 3) == "21");
    e.money_type = 1;
    e.price = 250;
    CHECK(game::marketplace_total_text(e, 2) == "5.00");
    e.money_type = 3;
    CHECK(game::marketplace_total_text(e, 2) == "N/A");
    e.description = "Text";
    CHECK(game::marketplace_details_text(e, "Head") == "Category: Head<BR>Text<BR>");
    CHECK(*game::marketplace_link_entry("x?marketid=42&y=1") == 42);
    CHECK_FALSE(game::marketplace_link_entry("item=3"));
}

// ---------------------------------------------------------------- the connection

namespace {

std::vector<AppPacket> sent_raw(const test::MockEqServer& server, std::uint16_t op) {
    std::vector<AppPacket> out;
    for (const auto& p : server.zone_log)
        if (p.opcode == op) out.push_back(p);
    return out;
}

} // namespace

TEST_CASE("mock server: the marketplace's requests, catalog, states, wallet, purchase and answer") {
    test::Rig rig(test::MockConfig::standard(), test::quick_options());
    auto& conn = rig.conn;
    auto& server = rig.server;
    REQUIRE(rig.to_zone());
    // The window's constructor at the game UI's build: hello, catalog, funding, Ready.
    REQUIRE(rig.until([&] { return !sent_raw(server, kOpMarketplaceReady).empty(); }));
    CHECK(sent_raw(server, kOpMarketplaceHello).size() == 1);
    CHECK(sent_raw(server, kOpMarketplaceRequestItems).size() == 1);
    CHECK(sent_raw(server, kOpMarketplaceRequestFunding).size() == 1);

    // A catalog while hidden waits for the next show.
    std::vector<MarketplaceEntry> c = {item(77, "Fine Sword", "Weapons", 100), item(78, "Shield", "Armor", 40)};
    c[0].max_quantity = 3;
    server.zone.send_reliable(encode_marketplace_items(c));
    REQUIRE(rig.until([&] { return conn.marketplace().catalog.size() == 2; }));
    CHECK(conn.marketplace().window.stale);

    // /marketplace shows it: Ready, the rows (all state 2 until the states come).
    conn.send_command("/marketplace");
    CHECK(conn.marketplace().open);
    REQUIRE(rig.until([&] { return sent_raw(server, kOpMarketplaceReady).size() == 2; }));
    CHECK_FALSE(conn.marketplace().window.stale);
    CHECK(conn.marketplace().window.visible.empty());
    REQUIRE(rig.until([&] { return sent_raw(server, kOpMarketplaceHello).size() >= 2; })); // the first frame's keep-alive

    MarketplaceWallet wallet;
    wallet.balance = 250;
    wallet.zip_on_file = true;
    server.zone.send_reliable(encode(wallet));
    server.zone.send_reliable(encode_marketplace_states({{77, 0, 5001, 0, ""}, {78, 0, 0, 0, ""}}));
    REQUIRE(rig.until([&] { return conn.marketplace().window.visible.size() == 2; }));
    CHECK(conn.marketplace().wallet == 250);
    // The first row selected, its preview asked with the window's context.
    REQUIRE(conn.marketplace().window.selected_entry(conn.marketplace().catalog));
    CHECK(conn.marketplace().window.selected_entry(conn.marketplace().catalog)->id == 77);
    REQUIRE(rig.until([&] {
        for (const auto& p : server.zone_log)
            if (p.opcode == to_wire(Op::ItemPreviewRequest) && p.data.size() == 80 && u32(p, 0) == 5001 &&
                static_cast<std::uint8_t>(p.data[0x4c]) == 1)
                return true;
        return false;
    }));

    // Two of them: 200 within 250; a third would not fit ((2 + 1) * 100 > 250).
    conn.marketplace_step_quantity(1);
    CHECK(conn.marketplace().window.quantity == 2);
    CHECK_FALSE(conn.marketplace_quantity_up_enabled());
    CHECK(conn.marketplace_buy_enabled());
    conn.marketplace_buy();
    CHECK(conn.marketplace().window.page == game::MarketplacePage::Confirm); // the zip is on file
    conn.marketplace_confirm();
    CHECK(conn.marketplace().busy);
    CHECK(conn.marketplace().window.page == game::MarketplacePage::Results);
    CHECK(conn.marketplace().result_text == "Processing...");
    CHECK_FALSE(conn.marketplace_buy_enabled());
    REQUIRE(rig.until([&] { return !sent_raw(server, kOpMarketplacePurchase).empty(); }));
    const AppPacket buy = sent_raw(server, kOpMarketplacePurchase)[0];
    CHECK(u32(buy, 0x48) == 77);
    CHECK(u32(buy, 0x4c) == 2);

    MarketplacePurchaseResult r;
    r.entry_id = 77;
    r.success = true;
    r.balance = 50;
    r.placed = 1;
    server.zone.send_reliable(encode(r));
    REQUIRE(rig.until([&] { return conn.marketplace().wallet == 50; }));
    REQUIRE(conn.confirmation());
    CHECK(conn.confirmation()->ok_only);
    CHECK(conn.confirmation()->text.find("An item has been placed in your inventory!") != std::string::npos);
    CHECK(conn.marketplace().dialog);
    conn.answer_confirmation(true);
    CHECK(conn.marketplace().window.page == game::MarketplacePage::Main);
    CHECK_FALSE(conn.marketplace().busy);
    CHECK_FALSE(conn.marketplace().dialog);
    // 100 is now more than the wallet holds.
    CHECK_FALSE(conn.marketplace_buy_enabled());

    // The marketplace's own preview reply turns the details to the preview page.
    ItemPreviewReply pr;
    pr.item.id = 5001;
    pr.item.name = "Fine Sword";
    pr.item.id_file = "IT10";
    AppPacket preview = encode(pr);
    preview.opcode = kOpMarketplacePreview;
    server.zone.send_reliable(preview);
    REQUIRE(rig.until([&] { return conn.marketplace().preview.serial > 0; }));
    CHECK(conn.marketplace().preview.show);
    CHECK(conn.marketplace().window.preview_tab);

    // Unavailable: closed.
    server.zone.send_reliable(AppPacket(kOpMarketplaceUnavailable));
    REQUIRE(rig.until([&] { return !conn.marketplace().open; }));
}

TEST_CASE("marketplace connection: the 60 s give-up and the keep-alive run on the frame clock") {
    test::Rig rig(test::MockConfig::standard(), test::quick_options());
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    std::vector<MarketplaceEntry> c = {item(5, "Thing", "Misc", 1)};
    rig.server.zone.send_reliable(encode_marketplace_items(c));
    REQUIRE(rig.until([&] { return conn.marketplace().catalog.size() == 1; }));
    conn.set_marketplace_open(true);
    rig.server.zone.send_reliable(encode_marketplace_states({{5, 0, 0, 0, ""}}));
    REQUIRE(rig.until([&] { return conn.marketplace().window.visible.size() == 1; }));
    conn.marketplace_buy(); // no zip on file: the zip page first
    CHECK(conn.marketplace().window.page == game::MarketplacePage::TaxInfo);
    conn.marketplace().zip_text = "abc";
    conn.marketplace_tax_next(); // refused: a popup, the window disabled
    REQUIRE(conn.confirmation());
    CHECK(conn.marketplace().busy);
    conn.answer_confirmation(true);
    CHECK_FALSE(conn.marketplace().busy);
    conn.marketplace().zip_text = "12345";
    conn.marketplace_tax_next();
    CHECK(conn.marketplace().window.page == game::MarketplacePage::Confirm);
    REQUIRE(rig.until([&] { return !sent_raw(rig.server, 0x3289).empty(); }));
    CHECK(static_cast<char>(sent_raw(rig.server, 0x3289)[0].data[0]) == '1');
    conn.marketplace_confirm();
    const std::int64_t at = conn.marketplace().processing_ms;
    conn.marketplace_frame(at + 59000);
    CHECK_FALSE(conn.confirmation());
    conn.marketplace_frame(at + 60001);
    REQUIRE(conn.confirmation());
    CHECK(conn.confirmation()->text == "[7626]"); // no eqstr table in the test
    CHECK(conn.marketplace().window.page == game::MarketplacePage::Main);
}
