// Evolving item inspection and received progression: packet bytes, the display bridge and an offline zone peer.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "app/lua_ui.h"
#include "game/item_display.h"
#include "game/item_links.h"
#include "mock_server/harness.h"
#include "net/item_packets.h"
#include "net/opcodes.h"
#include "ui/item_windows.h"

using namespace hailstone;
using namespace hailstone::net;
using namespace hailstone::test;

namespace {

Item evolving(std::uint64_t key = 0x100000055ULL, ItemSlot slot = ItemSlot::possessions(23)) {
    Item item;
    item.id = 65401;
    item.name = "Test evolving blade";
    item.serial = std::format("{:016}", key);
    item.instance_id = static_cast<std::uint32_t>(key);
    item.parcel_item_id = static_cast<std::uint32_t>(key >> 32);
    item.slot = slot;
    item.is_evolving = 1;
    item.evolving = {65405, 2, 17.0, 1, 5, {}};
    return item;
}

AppPacket progress(std::uint64_t key, std::uint32_t percent, std::uint32_t active = 1) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le(key);
    w.le(percent);
    w.le(active);
    return AppPacket(to_wire(Op::EvolveItem), w.take());
}

std::string final_link(const Item& item) {
    const auto request = game::evolving_final_result(item);
    REQUIRE(request);
    return std::format("1^{}^{}^{}^{}", request->item_id, request->final_item_id, request->level, request->hash);
}

std::size_t requests(const Rig& rig) {
    return static_cast<std::size_t>(std::count_if(rig.server.zone_log.begin(), rig.server.zone_log.end(),
        [](const AppPacket& p) { return p.opcode == to_wire(Op::EvolveItem); }));
}

void barrier(Rig& rig) {
    const auto count = [&] {
        return std::count_if(rig.server.zone_log.begin(), rig.server.zone_log.end(),
            [](const AppPacket& p) { return p.opcode == 0x7f01; });
    };
    const auto before = count();
    rig.conn.send_zone_packet(AppPacket(0x7f01));
    REQUIRE(rig.until([&] { return count() > before; }));
}

struct ImGuiScope {
    ImGuiContext* context = ImGui::CreateContext();
    ~ImGuiScope() { ImGui::DestroyContext(context); }
};

struct Display {
    ImGuiScope context;
    ui::ItemWindowsState windows;
    app::UiHost host;
    app::UiApi api;

    explicit Display(GameConnection& conn) : api(host) {
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        host.item_displays = &windows.info;
        host.item_display_ids = &windows.info_ids;
    }
    void refresh(GameConnection& conn) {
        while (auto item = conn.take_item_refresh()) ui::refresh_item_info(windows, *item);
    }
};

} // namespace

TEST_CASE("evolving items: native Final Result bytes and bounded progress decoder") {
    EvolvingFinalResult request{0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00};
    const auto packet = encode(request);
    CHECK(packet.opcode == 0x7cfb);
    const std::array<unsigned char, 24> golden{
        4, 0, 0, 0, 0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55,
        0xcc, 0xbb, 0xaa, 0x99, 0, 0xff, 0xee, 0xdd, 1, 0, 0, 0};
    REQUIRE(packet.data.size() == golden.size());
    for (std::size_t i = 0; i < golden.size(); ++i) CHECK(std::to_integer<unsigned char>(packet.data[i]) == golden[i]);
    request.display_flags = 0;
    CHECK(encode(request).data[20] == std::byte{0});
    CHECK_FALSE(decode_evolving_item_update(packet)); // action 4 cannot mutate an instance

    const std::array<unsigned char, 24> received{
        0, 0, 0, 0, 0xef, 0xbe, 0xad, 0xde, 0x78, 0x56, 0x34, 0x12,
        0xef, 0xcd, 0xab, 0x89, 100, 0, 0, 0, 1, 0, 0, 0};
    std::vector<std::byte> bytes;
    for (const auto value : received) bytes.push_back(static_cast<std::byte>(value));
    const AppPacket update(0x7cfb, bytes);
    const auto decoded = decode_evolving_item_update(update);
    REQUIRE(decoded);
    CHECK(decoded->unique_id == 0x89abcdef12345678ULL);
    CHECK(decoded->percentage == 100);
    CHECK(decoded->activated == 1);
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        const std::vector<std::byte> truncated(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
        CHECK_FALSE(decode_evolving_item_update(AppPacket(0x7cfb, truncated)));
    }
    auto bad = bytes;
    bad.push_back(std::byte{0});
    CHECK_FALSE(decode_evolving_item_update(AppPacket(0x7cfb, bad)));
    for (const auto& [offset, value] : {std::pair{0u, 1u}, {20u, 2u}}) {
        bad = bytes;
        bad[offset] = static_cast<std::byte>(value);
        CHECK_FALSE(decode_evolving_item_update(AppPacket(0x7cfb, bad)));
    }
    bad = bytes;
    std::fill(bad.begin() + 8, bad.begin() + 16, std::byte{0});
    CHECK_FALSE(decode_evolving_item_update(AppPacket(0x7cfb, bad)));
    CHECK(decode_evolving_item_update(progress(1, 0, 0)).has_value());
    const auto overshoot = decode_evolving_item_update(progress(1, 125));
    REQUIRE(overshoot);
    CHECK(overshoot->percentage == 125);
    CHECK_FALSE(decode_evolving_item_update(progress(1, 0x80000000u)));
    CHECK_FALSE(decode_evolving_item_update(progress(1, 0xffffffffu)));
}

TEST_CASE("evolving items: Final Result uses the unaugmented item and only an incomplete evolution") {
    Item item = evolving();
    const auto bare = game::evolving_final_result(item);
    REQUIRE(bare);
    Item augment;
    augment.id = 1234;
    augment.charm_file_id = 99;
    item.contents.push_back({0, augment});
    item.contents.push_back({5, augment});
    const auto augmented = game::evolving_final_result(item);
    REQUIRE(augmented);
    CHECK(augmented->hash == bare->hash);
    CHECK(augmented->hash == game::item_link_hash(game::item_link_hash_input(evolving())));
    CHECK(augmented->item_id == item.id);
    CHECK(augmented->final_item_id == 65405);
    CHECK(augmented->level == 2);
    item.evolving.final_item_id = 0; // the native link still permits the server to resolve from the source
    REQUIRE(game::evolving_final_result(item));
    CHECK(game::evolving_final_result(item)->final_item_id == 0);
    item.evolving.evolve_level = 5;
    CHECK_FALSE(game::evolving_final_result(item));
    item.evolving.evolve_level = -1;
    CHECK_FALSE(game::evolving_final_result(item));
    item = evolving();
    item.id = 0;
    CHECK_FALSE(game::evolving_final_result(item));
    item = evolving();
    item.is_evolving = 0;
    CHECK_FALSE(game::evolving_final_result(item));
    EmptyGameDir dir;
    GameDir game(dir.path());
    GameConnection disconnected(game, {});
    CHECK_FALSE(disconnected.request_evolving_final_result(evolving()));
}

TEST_CASE("evolving items: item-display links request native Final Result and show the server preview read-only") {
    Rig rig;
    REQUIRE(rig.to_zone());
    const Item item = evolving();
    rig.conn.receive_zone_packet_for_test(encode_char_inventory({item}));
    Display display(rig.conn);
    ui::show_item_info(display.windows, item);
    const auto link = final_link(item);
    const auto revision = rig.conn.inventory().revision();
    const auto inventory_size = rig.conn.inventory().items().size();
    REQUIRE(display.api.child_shown("ItemDisplayWindow", "Hailstone_EvolvingFinalResult") == true);
    CHECK(display.api.child_state("ItemDisplayWindow", "Hailstone_EvolvingFinalResult") == true);
    REQUIRE(display.api.stml_link("ItemDisplayWindow", "ItemDescription", 27, link));
    REQUIRE(rig.until([&] { return requests(rig) == 1; }));
    const auto sent = std::find_if(rig.server.zone_log.begin(), rig.server.zone_log.end(),
        [](const AppPacket& p) { return p.opcode == 0x7cfb; });
    REQUIRE(sent != rig.server.zone_log.end());
    REQUIRE(sent->data.size() == 24);
    ByteReader reader(sent->data);
    const auto request = game::evolving_final_result(item);
    REQUIRE(request);
    for (const auto value : {4u, item.id, 65405u, 2u, request->hash, 1u})
        CHECK(reader.le<std::uint32_t>() == value);
    CHECK_FALSE(rig.conn.take_item_display()); // requesting alone never synthesizes a final item

    Item final = item;
    final.id = 65405;
    final.name = "Authoritative final blade";
    final.evolving.evolve_level = 5;
    final.evolving.progress = 100;
    ItemPacket response;
    response.type = kItemPacketViewLink;
    response.item = final;
    rig.server.zone.send_reliable(encode_item_packet(true, response));
    std::optional<Item> shown;
    REQUIRE(rig.until([&] { shown = rig.conn.take_item_display(); return shown.has_value(); }));
    REQUIRE(shown);
    CHECK(shown->id == final.id);
    CHECK(shown->name == final.name);
    CHECK(rig.conn.inventory().revision() == revision);
    CHECK(rig.conn.inventory().items().size() == inventory_size);
    CHECK(rig.conn.inventory().at(item.slot)->id == item.id);

    for (const auto& bad : std::array<std::string, 6>{link + "^0", link + "x", "1^0^65405^2^0",
                                                  "1^-1^65405^2^0", "1^1^2", "2^0"})
        CHECK_FALSE(display.api.stml_link("ItemDisplayWindow", "ItemDescription", 27, bad));
    CHECK_FALSE(display.api.stml_link("ItemDisplayWindow", "ItemDescription", 26, link));
    CHECK_FALSE(display.api.stml_link("ItemDisplayWindow", "IDW_ItemInfo1", 27, link));
    display.windows.info[0].evolving.evolve_level = 3;
    CHECK_FALSE(display.api.stml_link("ItemDisplayWindow", "ItemDescription", 27, link));
    display.windows.info[0] = item;
    ImGui::GetIO().KeyShift = true;
    CHECK(display.api.click_child("ItemDisplayWindow", "Hailstone_EvolvingFinalResult"));
    ImGui::GetIO().KeyShift = false;
    REQUIRE(rig.until([&] { return requests(rig) == 2; }));
    const auto shifted = std::find_if(rig.server.zone_log.rbegin(), rig.server.zone_log.rend(),
        [](const AppPacket& p) { return p.opcode == 0x7cfb; });
    REQUIRE(shifted != rig.server.zone_log.rend());
    CHECK(shifted->data[20] == std::byte{0});

    Item augment;
    augment.id = 1234;
    display.windows.info[0].contents.push_back({0, augment});
    std::optional<Item> inspected;
    display.host.inspect_item = [&](const Item& value) { inspected = value; };
    CHECK(display.api.stml_link("ItemDisplayWindow", "IDW_Socket_Slot_1_Description", 27, "0^0"));
    REQUIRE(inspected);
    CHECK(inspected->id == augment.id);
    CHECK(inspected->slot.aug == 0);
    CHECK_FALSE(display.api.stml_link("ItemDisplayWindow", "ItemDescription", 27, "0^0junk"));
    display.windows.info[0].evolving.evolve_level = display.windows.info[0].evolving.evolve_max_level;
    CHECK(display.api.child_shown("ItemDisplayWindow", "Hailstone_EvolvingFinalResult") == false);
    CHECK(display.api.child_state("ItemDisplayWindow", "Hailstone_EvolvingFinalResult") == false);
    CHECK_FALSE(display.api.click_child("ItemDisplayWindow", "Hailstone_EvolvingFinalResult"));
    CHECK_FALSE(display.api.stml_link("ItemDisplayWindow", "ItemDescription", 27, link));
    CHECK_FALSE(rig.conn.request_evolving_final_result(display.windows.info[0]));
    barrier(rig);
    CHECK(requests(rig) == 2);
}

TEST_CASE("evolving items: display refresh keeps evolving instance ids separate from ordinary serials") {
    Item evolved = evolving(0x55);
    Item ordinary = evolved;
    ordinary.id = 3001;
    ordinary.name = "Ordinary item with the same numeric serial";
    ordinary.is_evolving = 0;
    ui::ItemWindowsState windows;
    windows.info = {evolved, ordinary};

    evolved.id = 65402;
    evolved.name = "Received evolving replacement";
    ui::refresh_item_info(windows, evolved);
    CHECK(windows.info[0].id == evolved.id);
    CHECK(windows.info[0].name == evolved.name);
    CHECK(windows.info[1].id == ordinary.id);
    CHECK(windows.info[1].name == ordinary.name);

    ordinary.id = 3002;
    ordinary.name = "Received ordinary replacement";
    ui::refresh_item_info(windows, ordinary);
    CHECK(windows.info[0].id == evolved.id);
    CHECK(windows.info[0].name == evolved.name);
    CHECK(windows.info[0].is_evolving == 1);
    CHECK(windows.info[1].id == ordinary.id);
    CHECK(windows.info[1].name == ordinary.name);
    CHECK(windows.info[1].is_evolving == 0);
}

TEST_CASE("evolving items: received progress matches the full instance key and waits for authoritative level replacement") {
    Rig rig;
    REQUIRE(rig.to_zone());
    constexpr std::uint64_t first_key = 0x100000055ULL, other_key = 0x200000055ULL, bag_key = 0x300000055ULL;
    Item first = evolving(first_key);
    Item other = evolving(other_key, ItemSlot::possessions(24));
    other.serial = first.serial; // neither the template nor a truncated serial string identifies an instance
    Item bag;
    bag.id = 9000;
    bag.name = "Test satchel";
    bag.item_class = 1;
    bag.bag_slots = 4;
    bag.slot = ItemSlot::possessions(25);
    Item inside = evolving(bag_key, ItemSlot::possessions(25, 0));
    bag.contents.push_back({0, inside});
    rig.conn.receive_zone_packet_for_test(encode_char_inventory({first, other, bag}));
    Display display(rig.conn);
    Item link_only = first;
    link_only.instance_id = link_only.parcel_item_id = 0;
    display.windows.info = {first, other, inside, link_only};
    display.windows.info_ids = {1, 2, 3, 4};
    const auto before = display.api.child_text("ItemDisplayWindow", "ItemDescription");
    REQUIRE(before);
    CHECK(before->find("[6146] 2 5 17 ") != std::string::npos);
    display.windows.info[0].evolving.progress = 18;
    const auto changed = display.api.child_text("ItemDisplayWindow", "ItemDescription");
    REQUIRE(changed);
    CHECK(changed->find("[6146] 2 5 18 ") != std::string::npos);
    display.windows.info[0].evolving.progress = 17;

    rig.server.zone.send_reliable(progress(first_key, 125, 0));
    REQUIRE(rig.until([&] { return rig.conn.inventory().at(first.slot)->evolving.progress == 125; }));
    display.refresh(rig.conn);
    CHECK(display.windows.info[0].evolving.progress == 125);
    CHECK(display.windows.info[0].evolving.activated == 0);
    CHECK(display.windows.info[1].evolving.progress == 17);
    CHECK(display.windows.info[2].evolving.progress == 17);
    CHECK(display.windows.info[3].evolving.progress == 17);
    CHECK(display.windows.info[0].id == first.id);
    CHECK(display.windows.info[0].evolving.evolve_level == 2);
    CHECK(display.windows.info[0].evolving.evolve_max_level == 5);
    const auto after = display.api.child_text("ItemDisplayWindow", "ItemDescription");
    REQUIRE(after);
    CHECK(after->find("[6146] 2 5 125 ") != std::string::npos); // server reward overshoot does not grant a level
    const auto unchanged_revision = rig.conn.inventory().revision();
    rig.conn.receive_zone_packet_for_test(progress(first_key, 125, 0));
    CHECK(rig.conn.inventory().revision() == unchanged_revision);
    CHECK_FALSE(rig.conn.take_item_refresh());

    rig.server.zone.send_reliable(progress(bag_key, 63));
    REQUIRE(rig.until([&] { return rig.conn.inventory().at(inside.slot)->evolving.progress == 63; }));
    display.refresh(rig.conn);
    CHECK(display.windows.info[2].evolving.progress == 63);
    CHECK(display.windows.info[0].evolving.progress == 125);
    CHECK(display.windows.info[1].evolving.progress == 17);
    const auto revision = rig.conn.inventory().revision();
    rig.conn.receive_zone_packet_for_test(progress(0x400000055ULL, 75));
    rig.conn.receive_zone_packet_for_test(progress(first_key, 0x80000000u));
    rig.conn.receive_zone_packet_for_test(progress(first_key, 25, 2));
    CHECK(rig.conn.inventory().revision() == revision);
    CHECK_FALSE(rig.conn.take_item_refresh());

    Item replacement = first;
    replacement.id = 65402;
    replacement.name = "Server evolved blade";
    replacement.evolving.evolve_level = 3;
    replacement.evolving.progress = 0;
    rig.server.item_packet(kItemPacketTrade, replacement);
    REQUIRE(rig.until([&] { return rig.conn.inventory().at(first.slot)->id == replacement.id; }));
    display.refresh(rig.conn);
    CHECK(display.windows.info[0].id == replacement.id);
    CHECK(display.windows.info[0].evolving.evolve_level == 3);
    CHECK(display.windows.info[0].evolving.progress == 0);
    CHECK(display.windows.info[1].id == other.id);
    CHECK(display.windows.info[3].id == first.id);
    CHECK(requests(rig) == 0); // progression reception makes no outgoing requests
}
