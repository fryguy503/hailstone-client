// Using items (net/game_connection_item_use.cpp): CInvSlot::HandleRButtonUp's request, Handle_OP_ItemVerifyReply's
// eating and casting and Cmd_useitem's arguments, against the mock EQEmu. (The NMS plugin's scribe path is tested in
// its own repository.)

#include <doctest/doctest.h>

#include <cstring>
#include <ctime>

#include "game/hot_button_menu.h"
#include "mock_server/harness.h"
#include "net/ability_packets.h"
#include "net/item_packets.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

std::uint32_t u32_at(const AppPacket& p, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, p.data.data() + at, 4);
    return v;
}

const AppPacket* last_of(const std::vector<AppPacket>& log, Op op) {
    for (auto it = log.rbegin(); it != log.rend(); ++it)
        if (from_wire(it->opcode) == op) return &*it;
    return nullptr;
}

std::vector<std::string> drain_chat(GameConnection& c) {
    std::vector<std::string> lines;
    while (auto e = c.poll_chat()) lines.push_back(e->text);
    return lines;
}

bool has_line(const std::vector<std::string>& lines, const std::string& text) {
    for (const auto& l : lines)
        if (l == text) return true;
    return false;
}

MockConfig with_items(std::vector<Item> extra) {
    MockConfig c = MockConfig::standard();
    auto& inv = c.characters[0].inventory;
    inv[1].item_type = kItemTypeFood; // the Rations in general slot 1 (23), five of them
    inv[1].cast_time_ = 30;          // FoodDuration
    for (auto& i : extra) inv.push_back(std::move(i));
    return c;
}

} // namespace

TEST_CASE("item use: a right click sends OP_ItemVerifyRequest; the reply eats the food") {
    Rig rig(with_items({}));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    const ItemSlot rations = ItemSlot::possessions(23);
    CHECK(conn.right_click_item(rations) == GameConnection::SlotRightClick::None);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::ItemVerifyRequest) == 1; }));
    const AppPacket* req = last_of(rig.server.zone_log, Op::ItemVerifyRequest);
    REQUIRE(req);
    REQUIRE(req->data.size() == kItemVerifyRequestSize);
    CHECK(u32_at(*req, 0) == 0);           // possessions
    CHECK(u32_at(*req, 4) == (23u | 0xffffu << 16)); // slot 23, no bag index
    CHECK(u32_at(*req, 12) == 0);          // no target
    CHECK(conn.spell_state().pending == 1);
    // IsItemPending: a second click waits for the answer.
    conn.right_click_item(rations);
    rig.run_for(std::chrono::milliseconds(30));
    CHECK(MockEqServer::count(rig.server.zone_log, Op::ItemVerifyRequest) == 1);

    // EQEmu's reply (spell 0): hunger 5999 + 30 x 100 / 2 caps at 6000, OP_Consume {slot, -1, 1, 1}, one fewer.
    drain_chat(conn);
    conn.receive_zone_packet_for_test(encode(ItemVerifyReply{rations, 0, 0}));
    CHECK(conn.spell_state().pending == 0);
    CHECK(conn.spell_state().hunger == 6000);
    CHECK(conn.inventory().at(rations)->stack_count == 4);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::Consume) == 1; }));
    const AppPacket* eat = last_of(rig.server.zone_log, Op::Consume);
    REQUIRE(eat->data.size() == 24);
    CHECK(u32_at(*eat, 12) == 0xffffffffu);
    CHECK(u32_at(*eat, 16) == 1);
    CHECK(u32_at(*eat, 20) == 1);

    // Full now: 12074, nothing eaten or sent.
    conn.receive_zone_packet_for_test(encode(ItemVerifyReply{rations, 0, 0}));
    CHECK(conn.inventory().at(rations)->stack_count == 4);
    rig.run_for(std::chrono::milliseconds(30));
    CHECK(MockEqServer::count(rig.server.zone_log, Op::Consume) == 1);
    // A negative spell stops at the dispatch, after taking back the pending count (none left: stays 0).
    conn.receive_zone_packet_for_test(encode(ItemVerifyReply{rations, -1, 0}));
    CHECK(conn.spell_state().pending == 0);
}

TEST_CASE("item use: a food duration past 2^31 / 100 wraps as the original's 32-bit FoodDuration x 100 / 2") {
    // 0x02000000 x 100 is 0xc8000000 in 32 bits, -939524096, so half of it is -469762048: the hunger goes down from
    // 5999 to -469756049 (stored as the u32 the meter keeps), as the original's wrapped product and signed division
    // give, on every compiler.
    MockConfig config = with_items({});
    config.characters[0].inventory[1].cast_time_ = 0x02000000;
    Rig rig(config);
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    const ItemSlot rations = ItemSlot::possessions(23);
    conn.receive_zone_packet_for_test(encode(ItemVerifyReply{rations, 0, 0}));
    CHECK(conn.spell_state().hunger == static_cast<std::uint32_t>(5999 - 469762048));
}

TEST_CASE("item use: the meal's add wraps in 32 bits from any level the server sent") {
    // OP_Stamina sets the hunger to 0x80000000 (INT_MIN as the signed level the original compares): the wrapping
    // FoodDuration's -469762048 takes it to 0x64000000 in 32 bits, so the meter caps at 6000, with no signed overflow.
    MockConfig config = with_items({});
    config.characters[0].inventory[1].cast_time_ = 0x02000000;
    Rig rig(config);
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    ByteWriter st;
    st.le<std::uint32_t>(0x80000000u);
    st.le<std::uint32_t>(6000);
    conn.receive_zone_packet_for_test(AppPacket(to_wire(Op::Stamina), st.take()));
    REQUIRE(conn.spell_state().hunger == 0x80000000u);
    conn.receive_zone_packet_for_test(encode(ItemVerifyReply{ItemSlot::possessions(23), 0, 0}));
    CHECK(conn.spell_state().hunger == 6000);
}

TEST_CASE("item use: a bag toggles its container, the bank and corpse paths send no request") {
    Item bag = make_item(1003, "Backpack", 24);
    bag.item_class = 1;
    bag.bag_slots = 8;
    Rig rig(with_items({bag}));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    CHECK(conn.right_click_item(ItemSlot::possessions(24)) == GameConnection::SlotRightClick::ToggleContainer);
    // Alt: the item preview (not built), no request.
    CHECK(conn.right_click_item(ItemSlot::possessions(13), {false, false, true}) ==
          GameConnection::SlotRightClick::ItemPreview);
    // An empty slot: nothing.
    CHECK(conn.right_click_item(ItemSlot::possessions(30)) == GameConnection::SlotRightClick::None);
    rig.run_for(std::chrono::milliseconds(30));
    CHECK(MockEqServer::count(rig.server.zone_log, Op::ItemVerifyRequest) == 0);
    // The sword (a common item, no click spell) still goes to the server.
    conn.right_click_item(ItemSlot::possessions(13));
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::ItemVerifyRequest) == 1; }));
}

TEST_CASE("item use: Cmd_useitem's arguments") {
    Item bag = make_item(1003, "Backpack", 24);
    bag.item_class = 1;
    bag.bag_slots = 4;
    BagItem in;
    in.index = 2;
    in.item = make_item(1004, "Gem", 24);
    bag.contents.push_back(in);
    Rig rig(with_items({bag}));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    drain_chat(conn);
    conn.send_command("/useitem");
    auto lines = drain_chat(conn);
    CHECK(has_line(lines, "usage: /useitem <slot> <subindex>"));
    CHECK(has_line(lines, "       Power Source = 21"));
    CHECK(has_line(lines, "       Bag Slot 10 = 32"));
    CHECK(has_line(lines, "or: /useitem <item name>, /useitem \"<item name>\" or /useitem <item id>"));
    conn.send_command("/useitem 40");
    CHECK(has_line(drain_chat(conn), "That is an invalid slot index."));
    // Not a number and no item's name: still RoF2's usage text.
    conn.send_command("/useitem -1");
    CHECK(has_line(drain_chat(conn), "usage: /useitem <slot> <subindex>"));
    conn.send_command("/useitem '40'"); // single quotes, as RoF2's GetNextCmdArg takes them: a slot
    CHECK(has_line(drain_chat(conn), "That is an invalid slot index."));
    // A bad bag index keeps the bag itself; a good one the item in it.
    conn.send_command("/useitem 24 9");
    CHECK(has_line(drain_chat(conn), "That is an invalid bag slot index."));
    conn.send_command("/useitem 24 2"); // (the bag itself is no common item: nothing went for "24 9")
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::ItemVerifyRequest) == 1; }));
    const AppPacket* req = last_of(rig.server.zone_log, Op::ItemVerifyRequest);
    CHECK(u32_at(*req, 4) == (24u | 2u << 16));
}

TEST_CASE("item use: /useitem by name, by whole name in quotes, or by item id (MacroQuest's and Hailstone's)") {
    Item bag = make_item(1003, "Backpack", 24);
    bag.item_class = 1;
    bag.bag_slots = 4;
    BagItem in;
    in.index = 2;
    in.item = make_item(1004, "Gem of Sight", 24);
    bag.contents.push_back(in);
    Rig rig(with_items({bag}));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    drain_chat(conn);
    auto request_slot = [&](int n) { // the slot field of the n-th OP_ItemVerifyRequest
        REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::ItemVerifyRequest) == n; }));
        return u32_at(*last_of(rig.server.zone_log, Op::ItemVerifyRequest), 4);
    };
    auto clear_pending = [&](const ItemSlot& s) { conn.receive_zone_packet_for_test(encode(ItemVerifyReply{s, -1, 0})); };
    // Part of the name, any case: the Rations in general slot 1 (23).
    conn.send_command("/useitem ratio");
    CHECK(request_slot(1) == (23u | 0xffffu << 16));
    clear_pending(ItemSlot::possessions(23));
    // An item in a bag, by its whole name in quotes, any case.
    conn.send_command("/useitem \"gem of sight\"");
    CHECK(request_slot(2) == (24u | 2u << 16));
    clear_pending(ItemSlot::possessions(24, 2));
    // Quoted means the whole name: part of it finds nothing, and nothing is said.
    conn.send_command("/useitem \"Gem\"");
    rig.run_for(std::chrono::milliseconds(30));
    CHECK(MockEqServer::count(rig.server.zone_log, Op::ItemVerifyRequest) == 2);
    CHECK(drain_chat(conn).empty());
    // An item id past the slots: the Short Sword (1001) in the primary slot (13).
    conn.send_command("/useitem 1001");
    CHECK(request_slot(3) == (13u | 0xffffu << 16));
    clear_pending(ItemSlot::possessions(13));
    // An id no carried item has stays RoF2's invalid slot; an unquoted name no item has shows RoF2's usage text.
    conn.send_command("/useitem 5555");
    CHECK(has_line(drain_chat(conn), "That is an invalid slot index."));
    conn.send_command("/useitem Nothing Like It");
    rig.run_for(std::chrono::milliseconds(30));
    CHECK(MockEqServer::count(rig.server.zone_log, Op::ItemVerifyRequest) == 3);
    CHECK(has_line(drain_chat(conn), "usage: /useitem <slot> <subindex>"));
    // A slot is still a slot (13 is the sword's slot, not an item id).
    conn.send_command("/useitem 13");
    CHECK(request_slot(4) == (13u | 0xffffu << 16));
}

TEST_CASE("item use: a poison applies through OP_ApplyPoison (CEverQuest::ApplyPoison)") {
    Item poison = make_item(14001, "Spider Venom", 25, 5);
    poison.item_type = kItemTypePoison;
    Rig rig(with_items({poison}));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    const ItemSlot at = ItemSlot::possessions(25);
    conn.receive_zone_packet_for_test(encode_skill_update(SkillUpdate{6, 0, 0})); // (the mock's skill n is n)
    drain_chat(conn);
    // No Apply Poison skill: 12383, still standing.
    conn.receive_zone_packet_for_test(encode(ItemVerifyReply{at, 0, 0}));
    CHECK(has_line(drain_chat(conn), "[12383]"));
    CHECK_FALSE(conn.applying_poison());
    CHECK_FALSE(conn.sitting());

    conn.receive_zone_packet_for_test(encode_skill_update(SkillUpdate{6, 50, 0}));
    drain_chat(conn);
    // Standing: sits down, then begins.
    conn.receive_zone_packet_for_test(encode(ItemVerifyReply{at, 0, 0}));
    CHECK(has_line(drain_chat(conn), "[12382]"));
    CHECK(conn.sitting());
    CHECK(conn.applying_poison());
    // Standing up cancels it.
    conn.set_stand_state(100);
    CHECK(has_line(drain_chat(conn), "[12497]"));
    CHECK_FALSE(conn.applying_poison());

    // Again, and let the time run out: OP_ApplyPoison {25, -1, -1, 0, 0}, both counters raised.
    conn.receive_zone_packet_for_test(encode(ItemVerifyReply{at, 0, 0}));
    REQUIRE(conn.applying_poison());
    conn.update();
    CHECK(MockEqServer::count(rig.server.zone_log, Op::ApplyPoison) == 0);
    conn.expire_poison_timer_for_test();
    conn.update();
    CHECK_FALSE(conn.applying_poison());
    CHECK(conn.spell_state().pending == 1);
    CHECK(conn.transact_state().currently_interacting == 1);
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::ApplyPoison) == 1; }));
    const AppPacket* sent = last_of(rig.server.zone_log, Op::ApplyPoison);
    REQUIRE(sent->data.size() == kApplyPoisonSize);
    CHECK(u32_at(*sent, 0) == (25u | 0xffffu << 16));
    CHECK(u32_at(*sent, 4) == 0xffffu);
    CHECK(u32_at(*sent, 8) == 0);
    // While it is out, another poison does not start.
    conn.receive_zone_packet_for_test(encode(ItemVerifyReply{at, 0, 0}));
    CHECK_FALSE(conn.applying_poison());

    // The reply ends the interaction and reports.
    drain_chat(conn);
    conn.receive_zone_packet_for_test(encode(ApplyPoison{25, -1, -1, 0, 1}));
    CHECK(has_line(drain_chat(conn), "[12905]"));
    CHECK(conn.spell_state().pending == 0);
    CHECK(conn.transact_state().currently_interacting == 0);
    conn.receive_zone_packet_for_test(encode(ApplyPoison{25, -1, -1, 0, 0}));
    CHECK(has_line(drain_chat(conn), "[12906]"));
}

TEST_CASE("item use: OP_DeleteCharge keeps the item and its hotbutton until the server removes it") {
    Item bag = make_item(1100, "Backpack", 24);
    bag.item_class = 1;
    bag.bag_slots = 4;
    Item charged = make_item(1101, "Charged Rod", 24);
    charged.charges = 3;
    bag.contents.push_back({2, charged});
    Item rechargeable = make_item(1102, "Rechargeable Rod", 27);
    rechargeable.charges = 1;
    Item unlimited = make_item(1103, "Unlimited Rod", 28);
    Rig rig(with_items({bag, rechargeable, unlimited}));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());

    const auto spend_charge = [&](const ItemSlot& slot) {
        // EQEmu uses the move layout with an invalid destination and a count of -1 for each charge spent.
        auto packet = encode(MoveItem{slot, ItemSlot::invalid(), 0xffffffffu});
        packet.opcode = to_wire(Op::DeleteCharge);
        conn.receive_zone_packet_for_test(packet);
    };
    const auto bag_slot = ItemSlot::possessions(24, 2);
    const Item* item = conn.inventory().at(bag_slot);
    REQUIRE(item);
    const auto button = game::inventory_hot_button(bag_slot, item, false);
    REQUIRE(button);

    for (std::uint32_t charges : {2u, 1u}) {
        spend_charge(bag_slot);
        const Item* remaining = conn.inventory().at(bag_slot);
        REQUIRE(remaining);
        CHECK(remaining->instance_id == charged.instance_id);
        CHECK(remaining->charges == charges);
        const auto hotbar_slot = game::hot_button_item_slot(conn.inventory(), *button);
        REQUIRE(hotbar_slot);
        CHECK(hotbar_slot->same(bag_slot));
        const auto refresh = conn.take_item_refresh();
        REQUIRE(refresh);
        CHECK(refresh->instance_id == charged.instance_id);
        CHECK(refresh->charges == charges);
    }

    // EQEmu sends OP_MoveItem with the same payload when an expendable item is used up.
    conn.receive_zone_packet_for_test(encode(MoveItem{bag_slot, ItemSlot::invalid(), 0xffffffffu}));
    CHECK(conn.inventory().at(bag_slot) == nullptr);
    CHECK_FALSE(game::hot_button_item_slot(conn.inventory(), *button));

    // The server can keep a rechargeable item at zero. Neither zero nor unlimited charges should underflow.
    spend_charge(rechargeable.slot);
    spend_charge(rechargeable.slot);
    const Item* empty = conn.inventory().at(rechargeable.slot);
    REQUIRE(empty);
    CHECK(empty->charges == 0);
    spend_charge(unlimited.slot);
    const Item* infinite = conn.inventory().at(unlimited.slot);
    REQUIRE(infinite);
    CHECK(infinite->charges == 0xffffffffu);
}

TEST_CASE("item recast timers: the item's own and the type's (GetItemRecastTimer), OP_ItemRecastDelay") {
    // Two clickies: a recast type 5 one and one without a type (-1), whose header carries its own timer.
    const auto now = static_cast<std::uint32_t>(std::time(nullptr));
    Item typed = make_item(1100, "Typed Clicky", 27);
    typed.effects[kEffectClick].effect = 200;
    typed.effects[kEffectClick].extra[2] = 30; // recast delay (s)
    typed.effects[kEffectClick].extra[3] = 5;  // recast type
    Item own = make_item(1101, "Own Clicky", 28);
    own.effects[kEffectClick].effect = 201;
    own.effects[kEffectClick].extra[2] = 60;
    own.effects[kEffectClick].extra[3] = 0xffffffffu;
    own.last_cast_time = now + 40; // ItemSerializationHeader last_cast_time
    Rig rig(with_items({typed, own}));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    const Item* t = conn.inventory().at(ItemSlot::possessions(27));
    const Item* o = conn.inventory().at(ItemSlot::possessions(28));
    REQUIRE(t);
    REQUIRE(o);
    CHECK(conn.item_recast_timer(t) == 0);
    CHECK(conn.item_recast_timer(o) == now + 40);
    CHECK(conn.item_recast_timer(nullptr) == 0);

    // No item cast under way: the packet is taken only with ignore_casting_requirement, then only the type's.
    AppPacket d(to_wire(Op::ItemRecastDelay), std::vector<std::byte>(12));
    const std::uint32_t delay = 30, type = 5;
    std::memcpy(d.data.data(), &delay, 4);
    std::memcpy(d.data.data() + 4, &type, 4);
    conn.receive_zone_packet_for_test(d);
    CHECK(conn.item_recast_timer(t) == 0);
    d.data[8] = std::byte{1};
    conn.receive_zone_packet_for_test(d);
    const std::uint32_t got = conn.item_recast_timer(t);
    CHECK(got >= now + 30);
    CHECK(got <= now + 32);
    CHECK(conn.item_recast_type_timer(5) == got);
    CHECK(conn.item_recast_timer(o) == now + 40); // type -1 keeps only its own
}

TEST_CASE("item clicks: CastSpell's attune and CanUseItem checks refuse quietly (@0x43b088, @0x43b0b6)") {
    // A self-only instant spell, castable in and out of combat (Sympathetic Strike II's shape).
    std::vector<std::string> f(237, "0");
    f[0] = "500";
    f[1] = "Test Click";
    f[9] = "100";
    f[86] = "254";
    f[98] = "6";   // self
    f[213] = "1";  // in combat
    f[214] = "1";  // out of combat
    std::string line;
    for (std::size_t i = 0; i < f.size(); ++i) line += (i ? "^" : "") + f[i];
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(line) == 1);

    // A 30 s recast of its own (type -1): an instant item cast that gets through sets it (@0x43c5b0).
    auto clicky = [](std::uint32_t id, const char* name, std::int16_t slot, std::uint32_t type) {
        Item it = make_item(id, name, slot);
        it.effects[kEffectClick].effect = 500;
        it.effects[kEffectClick].type = type;
        it.effects[kEffectClick].extra[2] = 30;
        it.effects[kEffectClick].extra[3] = 0xffffffffu;
        it.charges = 1;
        return it;
    };
    // The Ancient Sensate Eye (Legendary) case: attuneable, click type 5, not attuned.
    Item eye = clicky(1200, "Attuneable Eye", 27, 5);
    eye.attuneable = 1;
    eye.attuned = 0;
    // Click type 5 with a class the character (a warrior) is not.
    Item wand = clicky(1201, "Cleric Wand", 28, 5);
    wand.classes = 1u << 1;
    // Click type 4 (must equip) from a general slot.
    Item pick = clicky(1202, "Must Equip Pick", 29, 4);
    // The eye attuned (NoDropFlag +0x80, the header's inst_nodrop).
    Item attuned = eye;
    attuned.slot = ItemSlot::possessions(30);
    attuned.attuned = 1;
    Rig rig(with_items({eye, wand, pick, attuned}));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    conn.set_spell_table_for_test(table);
    auto recast = [&](std::int16_t slot) { return conn.item_recast_timer(conn.inventory().at(ItemSlot::possessions(slot))); };
    auto reply = [&](std::int16_t slot) {
        drain_chat(conn);
        conn.receive_zone_packet_for_test(encode(ItemVerifyReply{ItemSlot::possessions(slot), 0, 0}));
        rig.run_for(std::chrono::milliseconds(30));
        return drain_chat(conn);
    };

    // Each refusal is silent for an item cast ([esp+0x1c] is 0 with an item): no 7586, 179 or CanUseItem line.
    CHECK(reply(27).empty());
    CHECK(reply(28).empty());
    CHECK(reply(29).empty());
    CHECK(recast(27) == 0);
    CHECK(recast(28) == 0);
    CHECK(recast(29) == 0);

    // Attuned, the eye casts from a general slot: locally only, its recast timer set, and nothing sent.
    reply(30);
    CHECK(recast(30) >= static_cast<std::uint32_t>(std::time(nullptr)) + 29);
    CHECK(MockEqServer::count(rig.server.zone_log, Op::CastSpell) == 0);
}

TEST_CASE("item clicks: the reply casts without OP_CastSpell; the server cast it on the request (@0x4bff71)") {
    // EQEmu casts the click on OP_ItemVerifyRequest and answers with spell 0. The original's CastSpell from the reply
    // passes 0 as its send argument, so an instant clicky without a recast is not cast a second time.
    std::vector<std::string> f(237, "0");
    f[0] = "500";
    f[1] = "Test Click";
    f[9] = "100";
    f[86] = "254";
    f[98] = "6";   // self
    f[213] = "1";  // in combat
    f[214] = "1";  // out of combat
    std::string line;
    for (std::size_t i = 0; i < f.size(); ++i) line += (i ? "^" : "") + f[i];
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(line) == 1);

    Item instant = make_item(1300, "Instant Clicky", 27);
    instant.effects[kEffectClick].effect = 500;
    instant.effects[kEffectClick].type = 1; // click from anywhere
    instant.charges = 1;                    // no recast
    Item slow = instant;
    slow.id = 1301;
    slow.name = "Slow Clicky";
    slow.slot = ItemSlot::possessions(28);
    slow.effects[kEffectClick].extra[1] = 3000; // a 3 s item cast time
    Rig rig(with_items({instant, slow}));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    conn.set_spell_table_for_test(table);
    auto count = [&](Op op) { return MockEqServer::count(rig.server.zone_log, op); };

    // The instant one: one request, the reply, no OP_CastSpell and nothing left pending; it can be clicked again.
    for (int n = 1; n <= 2; ++n) {
        conn.right_click_item(ItemSlot::possessions(27));
        REQUIRE(rig.until([&] { return count(Op::ItemVerifyRequest) == n; }));
        conn.receive_zone_packet_for_test(encode(ItemVerifyReply{ItemSlot::possessions(27), 0, 0}));
        rig.run_for(std::chrono::milliseconds(30));
        CHECK(count(Op::CastSpell) == 0);
        CHECK(conn.spell_state().pending == 0);
        CHECK_FALSE(conn.spell_state().casting.casting());
    }

    // The 3 s one: the cast bar runs locally, still without OP_CastSpell.
    conn.right_click_item(ItemSlot::possessions(28));
    REQUIRE(rig.until([&] { return count(Op::ItemVerifyRequest) == 3; }));
    conn.receive_zone_packet_for_test(encode(ItemVerifyReply{ItemSlot::possessions(28), 0, 0}));
    rig.run_for(std::chrono::milliseconds(30));
    CHECK(count(Op::CastSpell) == 0);
    CHECK(conn.spell_state().casting.casting());
    CHECK(conn.spell_state().casting.spell == 500);
    CHECK(conn.spell_state().casting.cast_time == 3000);
}

TEST_CASE("auto inventory: worn stacks first, then a worn slot by the class check alone (AutoInventoryCursorItem)") {
    Item worn;
    worn.id = 8005;
    worn.name = "Field Point Arrow";
    worn.item_type = 27;
    worn.max_stack = 100;
    worn.stack_count = 90;
    worn.classes = 0xffff;
    worn.equip_slots = 1u << kSlotAmmo;
    worn.slot = ItemSlot::possessions(kSlotAmmo);
    Item held = worn;
    held.stack_count = 15;
    held.slot = ItemSlot::possessions(kSlotCursor);
    Rig rig(with_items({worn, held}));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    REQUIRE(conn.inventory().cursor());
    CHECK(conn.auto_inventory());
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::MoveItem) >= 2; }));
    std::vector<MoveItem> moves;
    for (const auto& p : rig.server.zone_log)
        if (from_wire(p.opcode) == Op::MoveItem)
            if (auto mv = decode_move_item(p)) moves.push_back(*mv);
    REQUIRE(moves.size() == 2);
    CHECK(moves[0].to.slot == kSlotAmmo);
    CHECK(moves[0].count == 10); // the 10 that fit on the 90
    CHECK(moves[1].to.slot >= kSlotGeneral1); // the other 5 to a free general slot
    CHECK(moves[1].to.slot <= kSlotGeneral10);
    CHECK_FALSE(conn.inventory().cursor());
}

TEST_CASE("the inventory doll: a held item is auto-inventoried (CInventoryWnd::HandleLButtonUp)") {
    Item held;
    held.id = 1001;
    held.name = "Cloth Cap";
    held.slot = ItemSlot::possessions(kSlotCursor);
    Rig rig(with_items({held}));
    auto& conn = rig.conn;
    REQUIRE(rig.to_zone());
    REQUIRE(conn.inventory().cursor());
    conn.drop_on_doll();
    CHECK_FALSE(conn.inventory().cursor());
    REQUIRE(rig.until([&] { return MockEqServer::count(rig.server.zone_log, Op::MoveItem) >= 1; }));
}
