// Augments: the fit test against the original's vectors (ItemBase_CanGemFitInSlot.jsonl;
// a fixed subset of it is kept in tests/data, the whole file is read when the vectors directory is there), the
// OP_AugmentItem / OP_AugmentInfo layouts, the insert / remove / swap / destroy rules and the socket rows.
#include <doctest/doctest.h>

#include <format>
#include <fstream>
#include <map>

#include "game/augments.h"
#include "game/eq_strings.h"
#include "game/mechanics/item_stats.h"
#include "net/augment_packets.h"
#include "net/opcodes.h"
#include "ui/item_windows.h"
#include "json_lite.h"
#include "vector_files.h"

using namespace hailstone;
using hailstone::test::Json;

namespace {

// ---------------------------------------------------------------- vectors

// A vector item ("item" / "augment" / a content) as a net::Item. "type" is the item class (+0x11a), "item_class"
// the ItemType (+0x1d4).
net::Item vector_item(const Json& j) {
    net::Item it;
    it.id = j["item_number"].u32();
    it.item_class = static_cast<std::uint8_t>(j["type"].u32());
    it.classes = j["classes"].u32();
    it.races = j["races"].u32();
    it.deity = j["deity"].u32();
    it.equip_slots = j["equip_slots"].u32();
    if (j.has("item_class")) {
        it.item_type = static_cast<std::uint8_t>(j["item_class"].u32());
        it.no_transfer = static_cast<std::uint8_t>(j["no_storage"].u32());
        it.max_stack = j["stack_size"].u32();
        it.damage = j["damage"].u32();
        it.delay = static_cast<std::uint8_t>(j["delay"].u32());
        it.sub_type = j["sub_class"].i32();
        for (const Json& s : j["socket_sub_classes"].items) it.socket_subclasses.push_back(s.i32());
        it.aug_type = j["aug_type"].u32();
        it.aug_restrict2 = static_cast<std::int32_t>(j["aug_skin_mask"].u32());
        it.aug_restrict = j["aug_restriction"].u32();
        it.material = j["armor_type"].u32();
        for (std::size_t i = 0; i < 6; ++i) it.aug_sockets[i].type = j["socket_types"][i].u32();
    }
    if (j.has("location")) {
        it.slot.type = static_cast<std::int16_t>(j["location"]["location"].i32());
        it.slot.slot = static_cast<std::int16_t>(j["location"]["slots"][0].i32());
        it.slot.sub = static_cast<std::int16_t>(j["location"]["slots"][1].i32());
        it.slot.aug = static_cast<std::int16_t>(j["location"]["slots"][2].i32());
    }
    for (std::size_t i = 0; i < j["contents"].items.size(); ++i) {
        const Json& c = j["contents"][i];
        if (c.is_null()) continue;
        net::BagItem b;
        b.index = static_cast<std::uint32_t>(i);
        b.item = vector_item(c);
        it.contents.push_back(std::move(b));
    }
    return it;
}

std::string check_fit(const Json& r) {
    const Json& in = r["in"];
    const Json& out = r["out"];
    net::Item host = vector_item(in["item"]);
    std::optional<net::Item> aug;
    if (!in["augment"].is_null()) aug = vector_item(in["augment"]);
    const net::Item* a = aug ? &*aug : nullptr;
    int combined = game::check_augment_combined_restrictions(host, a);
    int value = game::can_gem_fit_in_slot(host, a, in["socket"].u32(), in["check_empty"].truthy(), in["check_dup"].truthy());
    std::uint32_t sid = game::augment_fit_string_id(value);
    if (value != out["value"].i32() || combined != out["combined"].i32() || sid != out["string_id"].u32())
        return std::format("value {} combined {} string {} (want {} {} {})", value, combined, sid, out["value"].i32(),
                           out["combined"].i32(), out["string_id"].u32());
    return {};
}

} // namespace

TEST_CASE("CanGemFitInSlot: the kept vector subset") {
    std::ifstream f(std::string(HAILSTONE_TEST_DATA_DIR) + "/ItemBase_CanGemFitInSlot.subset.jsonl");
    REQUIRE(f);
    std::string line;
    int n = 0, bad = 0;
    std::map<int, int> codes;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        test::normalise_recorded_names(line);
        ++n;
        Json r = test::parse_json(line);
        ++codes[r["out"]["value"].i32()];
        std::string why = check_fit(r);
        if (!why.empty() && ++bad <= 10) FAIL_CHECK(n << ": " << why);
    }
    MESSAGE(n << " records, " << bad << " mismatches");
    CHECK(bad == 0);
    // Every result the function returns is covered.
    for (int code : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0xb, 0xe, 0xf, 0x10}) CHECK(codes[code] > 0);
}

TEST_CASE("CanGemFitInSlot: the original's vectors") {
    test::each_vector("ItemBase_CanGemFitInSlot.jsonl", check_fit);
}

TEST_CASE("AugmentFitResult_GetStringID and the restriction strings") {
    CHECK(game::augment_fit_string_id(0) == 0);
    for (int r : {1, 2, 3, 4}) CHECK(game::augment_fit_string_id(r) == 3365);
    CHECK(game::augment_fit_string_id(5) == 3366);
    CHECK(game::augment_fit_string_id(6) == 5476);
    CHECK(game::augment_fit_string_id(7) == 3367);
    CHECK(game::augment_fit_string_id(8) == 5480);
    CHECK(game::augment_fit_string_id(9) == 5480);
    CHECK(game::augment_fit_string_id(10) == 3371);
    CHECK(game::augment_fit_string_id(0xb) == 6193);
    CHECK(game::augment_fit_string_id(0xc) == 7686);
    CHECK(game::augment_fit_string_id(0xd) == 5485);
    CHECK(game::augment_fit_string_id(0xe) == 5477);
    CHECK(game::augment_fit_string_id(0xf) == 5475);
    CHECK(game::augment_fit_string_id(0x10) == 5365);
    CHECK(game::augment_fit_string_id(0x11) == 3362);
    CHECK(game::aug_restriction_string_id(0) == 0);
    CHECK(game::aug_restriction_string_id(1) == 9134);
    CHECK(game::aug_restriction_string_id(8) == 9148);
    CHECK(game::aug_restriction_string_id(14) == 8052);
    CHECK(game::aug_restriction_string_id(15) == 9200);
    CHECK(game::aug_restriction_string_id(16) == 0);
}

// ---------------------------------------------------------------- packets

TEST_CASE("OP_AugmentItem: the 0x28-byte block") {
    net::AugmentItem v;
    v.serial = 0x11223344;
    v.serial_high = 0;
    v.host = net::ItemSlot::possessions(5);
    v.socket = 1;
    v.action = net::AugmentAction::Remove;
    v.distiller = net::ItemSlot::possessions(25, 3);
    net::AppPacket p = net::encode(v);
    CHECK(p.opcode == 0x661b);
    REQUIRE(p.data.size() == net::kAugmentItemSize);
    auto b = [&](std::size_t i) { return std::to_integer<int>(p.data[i]); };
    CHECK(b(0) == 0x44);
    CHECK(b(3) == 0x11);
    // +0x08 Location 0 as an int32, slot 5, -1, -1, padding 0.
    CHECK(b(8) == 0);
    CHECK(b(11) == 0);
    CHECK(b(12) == 5);
    CHECK(b(14) == 0xff);
    CHECK(b(16) == 0xff);
    CHECK(b(18) == 0);
    CHECK(b(0x14) == 1);
    CHECK(b(0x1c) == 25); // the distiller's slot, bag index 3
    CHECK(b(0x1e) == 3);
    CHECK(b(0x24) == 1);

    // No distiller: 0xffffffff, 0xffffffff, 0xffff and two unwritten bytes (0 here).
    net::AugmentItem none;
    none.host = net::ItemSlot::possessions(23, 2);
    net::AppPacket q = net::encode(none);
    for (std::size_t i = 0x18; i < 0x22; ++i) CHECK(std::to_integer<int>(q.data[i]) == 0xff);
    CHECK(std::to_integer<int>(q.data[0x22]) == 0);
    CHECK(std::to_integer<int>(q.data[0x23]) == 0);
    auto d = net::decode_augment_item(q);
    REQUIRE(d);
    CHECK(d->host.same(none.host));
    CHECK(d->distiller.type == -1);
    CHECK(d->action == net::AugmentAction::Insert);
    // DECODE(OP_AugmentItem) wants the exact length.
    q.data.push_back(std::byte{0});
    CHECK_FALSE(net::decode_augment_item(q));
}

TEST_CASE("OP_AugmentInfo: request and reply") {
    net::AppPacket req = net::encode_augment_info_request(47009, 3);
    CHECK(req.opcode == 0x0afb);
    REQUIRE(req.data.size() == net::kAugmentInfoSize);
    auto r = net::decode_augment_info(req);
    REQUIRE(r);
    CHECK(r->item_id == 47009);
    CHECK(r->window == 3);
    CHECK(r->name.empty());
    for (std::size_t i = 8; i < net::kAugmentInfoSize; ++i) CHECK(std::to_integer<int>(req.data[i]) == 0);
    net::AugmentInfo reply;
    reply.window = 3;
    reply.name = "Class IX Augmentation Distiller";
    auto back = net::decode_augment_info(net::encode(reply));
    REQUIRE(back);
    CHECK(back->name == reply.name);
    CHECK(back->unknown072 == 0);
}

// ---------------------------------------------------------------- the requests

namespace {

// The test kit of the live test: Cloth Choker (neck, sockets type 1 and 2), Frozen Egg of Stamina (aug types 1|2,
// distiller 47009), a fragment of type 8 only.
net::Item choker() {
    net::Item it;
    it.id = 1003;
    it.name = "Cloth Choker";
    it.item_class = 0;
    it.item_type = 11;
    it.instance_id = 777;
    it.classes = 0xffff;
    it.races = 0xffff;
    it.equip_slots = 1u << 5;
    it.no_drop = 1;
    it.aug_sockets[0] = {1, 1, 0};
    it.aug_sockets[1] = {2, 1, 0};
    it.slot = net::ItemSlot::possessions(24);
    return it;
}
net::Item egg() {
    net::Item it;
    it.id = 24136;
    it.name = "Frozen Egg of Stamina";
    it.item_type = 54;
    it.instance_id = 778;
    it.classes = 0xffff;
    it.races = 0xffff;
    it.equip_slots = 0x7fffff;
    it.no_drop = 1;
    it.hp = 65;
    it.icon = 1234;
    it.aug_type = 3;
    it.aug_distiller = 47009;
    it.slot = net::ItemSlot::possessions(33);
    return it;
}
net::Item item_with(std::uint32_t id, std::uint8_t type, std::string name) {
    net::Item it;
    it.id = id;
    it.item_type = type;
    it.name = std::move(name);
    it.no_drop = 1;
    return it;
}

struct Kit {
    net::Item host = choker();
    std::optional<net::Item> cursor;
    std::map<std::uint32_t, game::FoundItem> by_id;
    std::map<std::uint8_t, game::FoundItem> by_type;
    std::vector<net::Item> keep; // what the found items point at
    bool feature = true;

    game::AugmentContext ctx() {
        game::AugmentContext c;
        c.cursor = cursor ? &*cursor : nullptr;
        c.item_at = [this](const net::ItemSlot& s) { return s.same(host.slot) ? &host : nullptr; };
        c.item_by_serial = [this](std::uint32_t s, std::uint32_t) { return s == host.instance_id ? &host : nullptr; };
        c.item_by_id = [this](std::uint32_t id) -> std::optional<game::FoundItem> {
            auto it = by_id.find(id);
            if (it == by_id.end()) return std::nullopt;
            return it->second;
        };
        c.item_by_type = [this](std::uint8_t t) -> std::optional<game::FoundItem> {
            auto it = by_type.find(t);
            if (it == by_type.end()) return std::nullopt;
            return it->second;
        };
        c.can_use_prestige = [](const net::Item&) { return true; };
        c.can_use = [](const net::Item&) { return true; };
        c.appearance_feature = feature;
        return c;
    }
    void socket(int i, net::Item aug) {
        net::BagItem b;
        b.index = static_cast<std::uint32_t>(i);
        b.item = std::move(aug);
        host.contents.push_back(std::move(b));
    }
};

} // namespace

TEST_CASE("Insert: checks in order, then send or confirm") {
    Kit k;
    net::Item window = k.host;
    // Nothing on the cursor: 5479.
    auto r = game::insert_augment_request(window, 0, k.ctx());
    CHECK(r.kind == game::AugmentRequest::Refuse);
    CHECK(r.eqstr == 5479);
    // A stack: 5484.
    k.cursor = egg();
    k.cursor->stack_count = 2;
    CHECK(game::insert_augment_request(window, 0, k.ctx()).eqstr == 5484);
    // Fits, has a solvent: confirmation 0x76 (5880, augment then host).
    k.cursor->stack_count = 1;
    r = game::insert_augment_request(window, 0, k.ctx());
    CHECK(r.kind == game::AugmentRequest::Confirm);
    CHECK(r.dialog == game::kConfirmAugInsert);
    CHECK(r.eqstr == 5880);
    CHECK(r.args == std::vector<std::string>{"Frozen Egg of Stamina", "Cloth Choker"});
    CHECK(r.packet.action == net::AugmentAction::Insert);
    CHECK(r.packet.socket == 0);
    CHECK(r.packet.serial == 777);
    CHECK(r.packet.host.same(net::ItemSlot::possessions(24)));
    CHECK(r.packet.distiller.type == -1);
    // Without a solvent and not Attuneable: sent at once.
    k.cursor->aug_distiller = 0;
    CHECK(game::insert_augment_request(window, 0, k.ctx()).kind == game::AugmentRequest::Send);
    // Attuneable and not attuned: asked.
    k.cursor->attuneable = 1;
    CHECK(game::insert_augment_request(window, 0, k.ctx()).kind == game::AugmentRequest::Confirm);
    k.cursor->attuned = 1;
    CHECK(game::insert_augment_request(window, 0, k.ctx()).kind == game::AugmentRequest::Send);
    // A type-8 augment does not fit the type-1 socket: 3367.
    k.cursor = egg();
    k.cursor->aug_type = 1u << 7;
    CHECK(game::insert_augment_request(window, 0, k.ctx()).eqstr == 3367);
    // The host not in the inventory (index and serial both miss): 5473.
    window.instance_id = 1;
    window.slot = net::ItemSlot::possessions(30);
    CHECK(game::insert_augment_request(window, 0, k.ctx()).eqstr == 5473);
    // An invalid or hidden socket: nothing.
    CHECK(game::insert_augment_request(k.host, 2, k.ctx()).kind == game::AugmentRequest::Nothing);
    k.host.aug_sockets[0].visible = 0;
    CHECK(game::insert_augment_request(k.host, 0, k.ctx()).kind == game::AugmentRequest::Nothing);
}

TEST_CASE("Insert: prestige, the appearance feature and the NO TRADE rule") {
    Kit k;
    k.cursor = egg();
    auto c = k.ctx();
    c.can_use_prestige = [](const net::Item&) { return false; };
    CHECK(game::insert_augment_request(k.host, 0, c).eqstr == 7686);
    k.host.aug_sockets[0].type = 21;
    k.cursor->aug_type = 1u << 20;
    k.feature = false;
    CHECK(game::insert_augment_request(k.host, 0, k.ctx()).eqstr == 5485);
    k.feature = true;
    k.host.aug_sockets[0].type = 1;
    k.cursor->aug_type = 3;
    k.cursor->no_drop = 0; // NO TRADE and unusable
    c = k.ctx();
    c.can_use = [](const net::Item& i) { return i.id != 24136; };
    CHECK(game::insert_augment_request(k.host, 0, c).eqstr == 3364);
    c.can_use = [](const net::Item&) { return true; };
    CHECK(game::insert_augment_request(k.host, 0, c).kind == game::AugmentRequest::Confirm);
}

TEST_CASE("Remove: the free remove, the distiller search and its refusals") {
    Kit k;
    CHECK(game::remove_augment_request(k.host, 0, k.ctx()).eqstr == 3363);
    k.socket(0, egg());
    k.cursor = item_with(5, 11, "Rock");
    CHECK(game::remove_augment_request(k.host, 0, k.ctx()).eqstr == 5478);
    k.cursor.reset();
    // No distiller anywhere: 5474.
    CHECK(game::remove_augment_request(k.host, 0, k.ctx()).eqstr == 5474);
    // A type-55 match is discarded, and nothing else: 5474.
    k.keep.reserve(4);
    k.keep.push_back(item_with(47009, 55, "Old Solvent"));
    k.by_id[47009] = {&k.keep.back(), net::ItemSlot::possessions(26)};
    CHECK(game::remove_augment_request(k.host, 0, k.ctx()).eqstr == 5474);
    // ... then the Perfected distiller is used.
    k.keep.push_back(item_with(47100, 64, "Perfected Augmentation Distiller"));
    k.by_type[64] = {&k.keep.back(), net::ItemSlot::possessions(27, 1)};
    auto r = game::remove_augment_request(k.host, 0, k.ctx());
    CHECK(r.kind == game::AugmentRequest::Confirm);
    CHECK(r.dialog == game::kConfirmAugRemove);
    CHECK(r.eqstr == 5879);
    CHECK(r.args == std::vector<std::string>{"Perfected Augmentation Distiller", "Frozen Egg of Stamina", "Cloth Choker"});
    CHECK(r.packet.distiller.same(net::ItemSlot::possessions(27, 1)));
    CHECK(r.packet.action == net::AugmentAction::Remove);
    // The matching distiller (type 56) comes first.
    k.keep.push_back(item_with(47009, 56, "Class IX Augmentation Distiller"));
    k.by_id[47009] = {&k.keep.back(), net::ItemSlot::possessions(28)};
    r = game::remove_augment_request(k.host, 0, k.ctx());
    CHECK(r.args[0] == "Class IX Augmentation Distiller");
    CHECK(r.packet.distiller.same(net::ItemSlot::possessions(28)));
    // An augment without a solvent: sent at once, no distiller.
    k.host.contents[0].item.aug_distiller = 0;
    r = game::remove_augment_request(k.host, 0, k.ctx());
    CHECK(r.kind == game::AugmentRequest::Send);
    CHECK(r.packet.distiller.type == -1);
}

TEST_CASE("Swap: keeps a type-55 match; confirms with 5481 or 5482") {
    Kit k;
    k.socket(0, egg());
    k.cursor = egg();
    k.cursor->id = 24137;
    k.cursor->name = "Frozen Egg of Health";
    // The old augment needs a solvent and there is none: 5474.
    CHECK(game::swap_augment_request(k.host, 0, k.ctx()).eqstr == 5474);
    k.keep.reserve(2);
    k.keep.push_back(item_with(47009, 55, "Old Solvent"));
    k.by_id[47009] = {&k.keep.back(), net::ItemSlot::possessions(26)};
    auto r = game::swap_augment_request(k.host, 0, k.ctx());
    CHECK(r.kind == game::AugmentRequest::Confirm);
    CHECK(r.dialog == game::kConfirmAugSwap);
    CHECK(r.eqstr == 5481);
    CHECK(r.args == std::vector<std::string>{"Old Solvent", "Frozen Egg of Stamina", "Frozen Egg of Health", "Cloth Choker"});
    CHECK(r.packet.action == net::AugmentAction::Swap);
    // The same augment again: the duplicate is in this socket, so it passes; in the other socket it is 3371.
    k.cursor->id = 24136;
    CHECK(game::swap_augment_request(k.host, 0, k.ctx()).kind == game::AugmentRequest::Confirm);
    k.host.aug_sockets[1].type = 2;
    k.socket(1, item_with(9999, 54, "Other"));
    k.host.contents[1].item.aug_type = 3;
    k.host.contents[1].item.classes = k.host.contents[1].item.races = 0xffff;
    k.host.contents[1].item.equip_slots = 0x7fffff;
    CHECK(game::swap_augment_request(k.host, 1, k.ctx()).eqstr == 3371);
    // An old augment without a solvent and a plain cursor augment: sent at once (5482 when it must ask).
    k.host.contents[0].item.aug_distiller = 0;
    k.cursor->id = 24137;
    k.cursor->aug_distiller = 0;
    CHECK(game::swap_augment_request(k.host, 0, k.ctx()).kind == game::AugmentRequest::Send);
    k.cursor->aug_distiller = 47009;
    r = game::swap_augment_request(k.host, 0, k.ctx());
    CHECK(r.eqstr == 5482);
    CHECK(r.args == std::vector<std::string>{"Frozen Egg of Stamina", "Frozen Egg of Health", "Cloth Choker"});
}

TEST_CASE("Destroy, the click and the context menu") {
    Kit k;
    CHECK(game::destroy_augment_request(k.host, 0, k.ctx()).kind == game::AugmentRequest::Nothing);
    CHECK(game::augment_socket_menu(k.host, 0, k.ctx(), nullptr).empty());
    k.socket(0, egg());
    auto r = game::destroy_augment_request(k.host, 0, k.ctx());
    CHECK(r.dialog == game::kConfirmAugDestroy);
    CHECK(r.eqstr == 5878);
    CHECK(r.packet.action == net::AugmentAction::Destroy);
    CHECK(r.packet.distiller.type == -1);
    // A click: filled with an empty cursor removes, with a cursor swaps; an empty socket inserts.
    CHECK(game::augment_socket_click(k.host, 0, k.ctx()).eqstr == 5474);
    CHECK(game::augment_socket_click(k.host, 1, k.ctx()).eqstr == 5479);
    k.cursor = item_with(5, 11, "Rock");
    CHECK(game::augment_socket_click(k.host, 0, k.ctx()).eqstr == 5475); // not an augment
    // The menu: title, Swap (cursor held), Destroy (the augment has a solvent).
    auto rows = game::augment_socket_menu(k.host, 0, k.ctx(), nullptr);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].text == "Frozen Egg of Stamina");
    CHECK_FALSE(rows[0].enabled);
    CHECK(rows[0].argb == 0xff00ff00);
    CHECK(rows[1].id == game::kAugMenuSwap);
    CHECK(rows[2].id == game::kAugMenuDestroy);
    CHECK(rows[2].argb == 0xffc08080);
    k.cursor.reset();
    k.host.contents[0].item.aug_distiller = 0;
    rows = game::augment_socket_menu(k.host, 0, k.ctx(), nullptr);
    REQUIRE(rows.size() == 2);
    CHECK(rows[1].id == game::kAugMenuRemove);
    CHECK(rows[1].argb == 0xff8aa3ff);
    // Only for possessions items.
    net::Item bank = k.host;
    bank.slot.type = net::kInvBank;
    CHECK(game::augment_socket_menu(bank, 0, k.ctx(), nullptr).empty());
}

// ---------------------------------------------------------------- the display

TEST_CASE("Socket rows: text, highlight, icons, tooltips, the appearance socket") {
    game::EqStrings s;
    s.set(3355, "Slot %1, type %2: %3");
    s.set(6699, "empty");
    s.set(4670, "Drop augment here to install.");
    s.set(4671, "Click to remove augment (Right-click for other options)");
    s.set(4672, "Open Marketplace to purchase special ornamentation ability");
    s.set(4673, "Drop ornamentation augment here to install.");
    game::AugmentStrings as;
    as.strings = &s;
    as.db_string = [](std::uint32_t id, std::uint32_t type) {
        return type == 16 && id == 1 ? std::string("1 (Stats)") : std::string();
    };
    Kit k;
    k.socket(0, egg());
    auto rows = game::build_augment_sockets(k.host, nullptr, as, true);
    CHECK(rows.rows[0].shown);
    CHECK(rows.rows[0].text == "Slot 1, type 1 (Stats): <a WndNotify=\"27,0^0\">Frozen Egg of Stamina</a>");
    CHECK(rows.rows[0].icon == 1234);
    CHECK(rows.rows[0].tooltip == "Click to remove augment (Right-click for other options)");
    CHECK(rows.rows[1].text == "Slot 2, type 2: empty"); // no dbstr name: the number
    CHECK(rows.rows[1].tooltip == "Drop augment here to install.");
    CHECK(rows.rows[1].icon == 0);
    CHECK_FALSE(rows.rows[2].shown);
    CHECK_FALSE(rows.appearance_shown);
    // A fitting cursor item: every socket it fits is gold (occupied ones too).
    net::Item cursor = egg();
    cursor.id = 1;
    rows = game::build_augment_sockets(k.host, &cursor, as, true);
    CHECK(rows.rows[0].text.starts_with("<C \"#D6B228\">Slot 1"));
    CHECK(rows.rows[1].text == "<C \"#D6B228\">Slot 2, type 2: empty</C>");
    cursor.aug_type = 1;
    rows = game::build_augment_sockets(k.host, &cursor, as, true);
    CHECK(rows.rows[1].text == "Slot 2, type 2: empty");
    // An ornament socket of type 21, empty, the feature missing: the appearance area with the Buy button.
    k.host.aug_sockets[3] = {21, 1, 0};
    rows = game::build_augment_sockets(k.host, nullptr, as, false);
    CHECK(rows.appearance_shown);
    CHECK(rows.appearance_socket == 3);
    CHECK_FALSE(rows.rows[3].shown);
    CHECK(rows.appearance.tooltip == "Drop ornamentation augment here to install.");
    CHECK(rows.buy_shown);
    CHECK(rows.buy_tooltip == "Open Marketplace to purchase special ornamentation ability");
    rows = game::build_augment_sockets(k.host, nullptr, as, true);
    CHECK_FALSE(rows.buy_shown);
    CHECK(rows.buy_tooltip.empty());
    // The description copy: every valid visible socket, ornaments included, <BR> after each.
    std::string d = game::augment_description_rows(k.host, as);
    CHECK(d.find("Slot 4, type 21: empty<BR>") != std::string::npos);
    CHECK(d.starts_with("Slot 1, type 1 (Stats): <a"));
    CHECK(game::is_augmented(k.host));
}

TEST_CASE("Augment description lines: slot types, restrictions") {
    game::EqStrings s;
    s.set(3357, "This Augmentation fits in slot types: %1");
    s.set(3369, "Fits in all slot types.");
    s.set(3358, "Restrictions");
    s.set(9134, "Armor Only");
    s.set(9147, "Delay: %1");
    game::AugmentStrings as;
    as.strings = &s;
    as.db_string = [](std::uint32_t id, std::uint32_t type) {
        return type == 16 && id == 1 ? std::string("1 (Stats)") : std::string();
    };
    net::Item aug = egg();
    CHECK(game::augment_slot_types_line(aug, as) == "This Augmentation fits in slot types:  1 (Stats), 2");
    aug.aug_type = 0x7fffffff;
    CHECK(game::augment_slot_types_line(aug, as) == "This Augmentation fits in slot types: Fits in all slot types.");
    CHECK(game::augment_effects_restriction(aug, as).empty());
    aug.aug_restrict = 1;
    CHECK(game::augment_effects_restriction(aug, as) == "<BR>Restrictions -<BR>Armor Only<BR>");
    aug.delay = 30;
    CHECK(game::augment_restrictions_block(aug, as) == "<BR>Restrictions -<BR>Armor Only<BR>Delay: 30<BR>");
}

TEST_CASE("Augments travel as children; a type-0x67 packet refreshes displays of the same serial") {
    net::Item host = choker();
    net::BagItem b;
    b.index = 1; // socket 2
    b.item = egg();
    host.contents.push_back(b);
    net::ItemPacket ip;
    ip.type = net::kItemPacketTrade;
    ip.item = host;
    auto back = net::decode_item_packet(net::encode_item_packet(false, ip));
    REQUIRE(back);
    REQUIRE(back->item.contents.size() == 1);
    CHECK(back->item.contents[0].index == 1);
    CHECK(game::augment_in(back->item, 1)->id == 24136);
    CHECK(game::augment_in(back->item, 0) == nullptr);

    ui::ItemWindowsState st;
    net::Item plain = choker();
    ui::show_item_info(st, plain);
    net::Item other = egg();
    ui::show_item_info(st, other);
    ui::refresh_item_info(st, back->item);
    REQUIRE(st.info.size() == 2);
    CHECK(game::is_augmented(st.info[0]));
    CHECK(st.info[1].id == 24136);
}

TEST_CASE("stats-disabled bytes: 0x7b28a0 and the stat sums' disabled children") {
    using namespace hailstone::game;
    net::Item host;
    host.item_class = 0;
    host.hp = 10;
    host.prestige = 0;
    host.aug_sockets[0].type = 21; // the special ornament socket
    host.aug_sockets[1].type = 7;
    net::Item orn;
    orn.hp = 5;
    net::Item plain;
    plain.hp = 3;
    plain.prestige = 2;
    host.contents.push_back({0, orn});
    host.contents.push_back({1, plain});
    // EQEmu's gold membership: UseItem -1, Ornament 1: nothing disabled.
    update_stats_disabled(host, -1, 1, true);
    CHECK_FALSE(stats_disabled(*host.content(0)));
    CHECK_FALSE(stats_disabled(*host.content(1)));
    CHECK(mechanics::item_stat(host, [](const net::Item& i) { return i.hp; }) == 18);
    // Without the ornament feature the type 21 socket's augment is disabled; a UseItem limit below an item's
    // prestige disables that one (rank).
    update_stats_disabled(host, 1, 0, true);
    CHECK(host.content(0)->aug_texture_disabled);
    CHECK_FALSE(host.content(0)->rank_disabled);
    CHECK(host.content(1)->rank_disabled);
    CHECK_FALSE(host.aug_texture_disabled); // a top-level item clears its own
    CHECK_FALSE(stats_disabled(host));
    // the getters count a disabled child only under a disabled owner
    CHECK(mechanics::item_stat(host, [](const net::Item& i) { return i.hp; }) == 10);
    host.rank_disabled = true;
    CHECK(mechanics::item_stat(host, [](const net::Item& i) { return i.hp; }) == 18);
}

TEST_CASE("CheckAugmentCombinedRestrictions over a server extension's classes (worn hosts)") {
    // A Warrior-only breastplate and a Wizard-only augment that otherwise fit (races, deity, slots, type).
    net::Item host;
    host.classes = 1u << 0; // Warrior
    host.races = host.equip_slots = 0xffffffffu;
    host.slot = net::ItemSlot::possessions(17); // worn: chest
    host.max_stack = 1;
    net::Item aug;
    aug.aug_type = 1;
    aug.classes = 1u << 11; // Wizard
    aug.races = aug.equip_slots = 0xffffffffu;
    aug.max_stack = 1;
    CHECK(game::check_augment_combined_restrictions(host, &aug) == 2); // stock: no common class
    // A Warrior / Wizard: the host takes the player's classes, so the Wizard augment fits.
    const std::uint32_t war_wiz = (1u << 0) | (1u << 11);
    CHECK(game::check_augment_combined_restrictions(host, &aug, war_wiz) == 0);
    // A Warrior / Cleric: the augment shares none of them.
    CHECK(game::check_augment_combined_restrictions(host, &aug, (1u << 0) | (1u << 1)) == 2);
    // An augment for every class (0) is not refused by that test.
    net::Item any = aug;
    any.classes = 0;
    CHECK(game::check_augment_combined_restrictions(host, &any, 1u << 1) == 2); // 0 & host: the stock test still runs
    // Not worn (a bag slot): the stock check.
    net::Item bagged = host;
    bagged.slot = net::ItemSlot::possessions(23);
    CHECK(game::check_augment_combined_restrictions(bagged, &aug, war_wiz) == 2);
    CHECK(game::can_gem_fit_in_slot(host, &aug, 0, false, false, war_wiz) == 6); // passes the class test; no socket
}

TEST_CASE("worn_look: the ornament a worn item shows on its wearer") {
    auto with_aug = [](net::Item host, int socket, net::Item aug) {
        net::BagItem b;
        b.index = static_cast<std::uint32_t>(socket);
        b.item = std::move(aug);
        host.contents.push_back(std::move(b));
        return host;
    };
    net::Item chest;
    chest.material = 3;
    chest.color = 0xff112233;
    chest.aug_sockets[0] = {1, 1, 0};
    chest.aug_sockets[5] = {20, 1, 0};

    auto w = game::worn_look(chest, 1);
    CHECK(w.material == 3);
    CHECK(w.hero_forge == 0);
    CHECK(w.tint == 0xff112233);

    net::Item robe; // a robe glamour: only a Material, no hero's forge model
    robe.material = 11;
    net::Item stats;
    stats.material = 7;
    net::Item worn = with_aug(with_aug(chest, 0, stats), 5, robe);
    w = game::worn_look(worn, 1);
    CHECK(w.material == 11); // the ornament socket's, not the type-1 socket's
    CHECK(w.tint == 0xff112233);

    worn.contents.back().item.aug_texture_disabled = true; // a type-21 ornament without game feature 0x15
    CHECK(game::worn_look(worn, 1).material == 3);

    net::Item forge = with_aug(chest, 5, net::Item{});
    forge.ornament_hero_model = 1501;
    w = game::worn_look(forge, 1);
    CHECK(w.material == 3); // the ornament has no Material of its own
    CHECK(w.hero_forge == 1501);

    net::Item sword;
    sword.id_file = "IT5";
    CHECK(game::worn_look(sword, 7).material == 5);
    sword.ornament_main = "IT10652";
    sword.ornament_off = "IT10653";
    CHECK(game::worn_look(sword, 7).material == 10652);
    CHECK(game::worn_look(sword, 8).material == 10653);
    sword.ornament_off.clear();
    CHECK(game::worn_look(sword, 8).material == 10652);
}
