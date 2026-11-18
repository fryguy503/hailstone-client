#include <doctest/doctest.h>

#include <string_view>

#include "net/packet_io.h"
#include "net/shroud_packets.h"
#include "net/task_packets.h"

using namespace hailstone;
using namespace hailstone::net;

namespace {

AppPacket shroud_selection_packet(std::string_view body, std::uint32_t context = 0, std::uint32_t status = 0) {
    ByteWriter w;
    w.le(context);
    w.le(status);
    w.le<std::uint32_t>(0);
    detail::write_cstring(w, body);
    return {0x2ec3, w.take()};
}

void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (auto i = 0u; i < 4; ++i) bytes.at(offset + i) = std::byte((value >> (8 * i)) & 0xff);
}

} // namespace

TEST_CASE("Shroud selection decodes task context, nested forms and the bank redirect") {
    constexpr auto valid = "PROGRESSION\a" "7\aForest\aForest roles\aBRANCH\a" "11\aScout\a\a" "40\a"
                           "TEMPLATE\aLevel 20\a" "42\a" "20\a" "1\aTEMPLATE\aLevel 25\a" "43\a" "25\a" "0";
    const auto selection = decode_shroud_selection(shroud_selection_packet(valid, 100));
    REQUIRE(selection);
    CHECK(selection->context_id == 100);
    REQUIRE(selection->categories.size() == 1);
    const auto& category = selection->categories.front();
    CHECK(category.id == 7);
    REQUIRE(category.branches.size() == 1);
    const auto& branch = category.branches.front();
    CHECK(branch.progress == 40);
    CHECK(branch.description.empty());
    REQUIRE(branch.forms.size() == 2);
    CHECK(branch.forms[0].selection_id == 42);
    CHECK(branch.forms[0].unlocked);
    CHECK_FALSE(branch.forms[1].unlocked);

    const auto redirect = decode_shroud_selection(shroud_selection_packet("", 0, 1));
    REQUIRE(redirect);
    CHECK(redirect->status == 1);
    CHECK(redirect->categories.empty());
    CHECK_FALSE(decode_shroud_selection(shroud_selection_packet("TEMPLATE\aLevel 20\a" "42\a" "20\a" "1")));
    CHECK_FALSE(decode_shroud_selection(shroud_selection_packet("PROGRESSION\a-1\aCat\aDesc")));

    auto truncated = shroud_selection_packet(valid);
    truncated.data.pop_back();
    CHECK_FALSE(decode_shroud_selection(truncated));
}

TEST_CASE("Shroud detail response preserves rank and item identity and rejects truncated counts") {
    ByteWriter w;
    w.le<std::uint32_t>(42);
    w.le<std::uint32_t>(1);
    detail::write_cstring(w, "42^Scout^Tracking^3^20^100^30^40^75^73^40^67^26^-2^23^1^3672^2^1^129^Hatchet");
    AppPacket packet(0x6c04, w.take());
    const auto details = decode_shroud_stats(packet);
    REQUIRE(details);
    CHECK(details->selection_id == 42);
    CHECK(details->has_data);
    CHECK(details->agi == -2);
    REQUIRE(details->abilities.size() == 1);
    CHECK(details->abilities.front().rank_id == 3672);
    REQUIRE(details->items.size() == 1);
    CHECK(details->items.front().name == "Hatchet");

    packet.data.pop_back();
    CHECK_FALSE(decode_shroud_stats(packet));
    packet.data.erase(packet.data.begin() + 8, packet.data.end());
    packet.data[4] = std::byte{0};
    const auto unavailable = decode_shroud_stats(packet);
    REQUIRE(unavailable);
    CHECK_FALSE(unavailable->has_data);

    Item item;
    item.id = 129;
    item.name = "Hatchet";
    const auto item_body = encode_char_inventory({item});
    w.le<std::uint32_t>(42);
    w.le<std::uint32_t>(129);
    w.bytes(std::span(item_body.data).subspan(4));
    packet = {0x2507, w.take()};
    const auto inspect = decode_shroud_item(packet);
    REQUIRE(inspect);
    CHECK(inspect->item.name == "Hatchet");
    packet.data[4] = std::byte{0};
    CHECK_FALSE(decode_shroud_item(packet));
}

TEST_CASE("Shroud progression and request payloads match the RoF2 wire layout") {
    ByteWriter w;
    w.le<std::uint32_t>(1);
    constexpr std::string_view text = "PROGRESSION\a" "4\aFamily\a\a" "1\a" "3\aSeer\a\a" "20";
    w.bytes({reinterpret_cast<const std::byte*>(text.data()), text.size()});
    auto packet = AppPacket(0x541d, w.take());
    const auto progress = decode_shroud_progress(packet);
    REQUIRE(progress);
    REQUIRE(progress->size() == 1);
    CHECK(progress->front().category_id == 4);
    CHECK(progress->front().unlocked == 1);
    CHECK(progress->front().percent == 20);
    packet.data[0] = std::byte{2};
    CHECK_FALSE(decode_shroud_progress(packet));

    const auto stats = encode_shroud_request_stats(0x12345678);
    const auto select = encode_shroud_select(0x12345678);
    const auto cancel = encode_shroud_cancel();
    const auto ability = encode_shroud_request_ability(0x12345678, 0x9102);
    const auto item = encode_shroud_request_item(0x12345678, 0x9102);
    CHECK(stats.opcode == 0x2b37);
    CHECK(select.opcode == 0x1cd0);
    CHECK(cancel.opcode == 0x79da);
    CHECK(ability.opcode == 0x3acc);
    CHECK(item.opcode == 0x4b25);
    const std::vector<std::byte> id{std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}};
    CHECK(stats.data == id);
    CHECK(select.data == id);
    CHECK(cancel.data.empty());
    ByteReader detail(ability.data);
    detail.skip(4);
    CHECK(detail.le<std::uint32_t>() == 0x9102);
    CHECK(ability.data == item.data);
}

TEST_CASE("Shroud transition applies a fixed form profile and possession snapshot atomically") {
    Spawn player;
    player.name = "ShroudTester";
    player.spawn_id = 123;
    player.level = 20;
    player.race = 44;
    player.class_ = 7;
    const auto spawn = encode(player);
    ByteWriter w;
    w.le(player.spawn_id);
    w.le(static_cast<std::uint16_t>(6 + spawn.data.size()));
    w.bytes(spawn.data);
    const AppPacket observer(0x6562, w.take());
    const auto remote = decode_shroud_transition(observer);
    REQUIRE(remote);
    CHECK(remote->spawn.race == 44);
    CHECK_FALSE(remote->profile);

    std::vector<std::byte> profile(0x4ff8);
    write_u32(profile, 0, 2);
    write_u32(profile, 0x0c, player.race);
    profile[0x10] = std::byte(player.class_);
    profile[0x11] = std::byte(player.level);
    write_u32(profile, 0x039c, 2250);
    write_u32(profile, 0x4ff4, 1500);
    auto self = observer;
    self.data.insert(self.data.end(), profile.begin(), profile.end());
    const auto empty = decode_shroud_transition(self);
    REQUIRE(empty);
    REQUIRE(empty->profile);
    CHECK(empty->possessions.empty());

    Item item;
    item.id = 42;
    const auto items = encode_char_inventory({item});
    self.data.insert(self.data.end(), items.data.begin(), items.data.end());

    PlayerProfile previous;
    previous.character_id = 992;
    previous.zone_id = 202;
    const auto local = decode_shroud_transition(self, &previous);
    REQUIRE(local);
    REQUIRE(local->profile);
    CHECK(local->profile->character_id == 992);
    CHECK(local->profile->zone_id == 202);
    CHECK(local->profile->shroud_type == 2);
    CHECK(local->profile->cur_hp == 2250);
    CHECK(local->profile->endurance == 1500);
    REQUIRE(local->possessions.size() == 1);
    CHECK(local->possessions.front().id == 42);

    self.data.pop_back();
    CHECK_FALSE(decode_shroud_transition(self));
    self.data[4] = std::byte{0};
    self.data[5] = std::byte{0};
    CHECK_FALSE(decode_shroud_transition(self));
}

TEST_CASE("Shroud bank lists and requests keep the full 64-bit storage key") {
    constexpr std::uint64_t key = 0x123456789a;
    ByteWriter w;
    w.le<std::uint32_t>(900);
    w.le<std::uint32_t>(1);
    detail::write_cstring(w, "Water Flask");
    w.le<std::uint32_t>(28);
    w.le(key);
    w.le<std::uint32_t>(4);
    AppPacket packet(0x1115, w.take());
    const auto list = decode_alt_storage_list(packet);
    REQUIRE(list);
    CHECK(list->retention_seconds == 900);
    REQUIRE(list->entries.size() == 1);
    CHECK(list->entries.front().key == key);
    CHECK(list->entries.front().quantity == 4);
    packet.data.pop_back();
    CHECK_FALSE(decode_alt_storage_list(packet));

    const auto inspect = encode_alt_storage_inspect(key, 1);
    CHECK(inspect.opcode == 0x7b71);
    CHECK(inspect.data.size() == 24);
    const auto control = decode_alt_storage(inspect);
    REQUIRE(control);
    CHECK(control->action == AltStorageAction::Inspect);
    CHECK(control->storage_key == key);
    CHECK(control->modifier == 1);
    CHECK(encode_alt_storage_withdraw(key).data.size() == 16);
}

TEST_CASE("Shared task roster retains the Monster Mission template field") {
    const auto members = encode(std::vector<SharedTaskMember>{{"Leader", true, 5}, {"Member", false, 0}});
    const auto decoded = decode_shared_task_members(members);
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 2);
    CHECK(decoded->at(0).leader);
    CHECK(decoded->at(0).template_id == 5);
    CHECK(decoded->at(1).template_id == 0);
}

TEST_CASE("Native Monster Mission catalog preserves the task identity and role availability") {
    ByteWriter w;
    w.zeros(8);
    w.le<std::uint32_t>(677);
    w.le<std::uint32_t>(0);
    w.le<std::uint8_t>(1);
    w.le<std::uint32_t>(2);
    for (const auto id : {987u, 988u}) {
        w.le(id);
        w.le<std::int32_t>(id == 987 ? 1 : 0);
        w.le<std::int32_t>(id == 987 ? 2 : -1);
        w.le<std::uint32_t>(0);
        w.le<std::uint8_t>(id == 987 ? 1 : 0);
        detail::write_cstring(w, id == 987 ? "Scout" : "Guard");
    }
    AppPacket packet(0x3c54, w.take());
    const auto catalog = decode_monster_mission_catalog(packet);
    REQUIRE(catalog);
    CHECK(catalog->task_id == 677);
    CHECK(catalog->selection_locked);
    REQUIRE(catalog->templates.size() == 2);
    CHECK(catalog->templates[0].name == "Scout");
    CHECK(catalog->templates[0].minimum == 1);
    CHECK(catalog->templates[0].maximum == 2);
    CHECK(catalog->templates[0].selectable);
    CHECK(catalog->templates[1].maximum == -1);
    CHECK_FALSE(catalog->templates[1].selectable);
    packet.data.pop_back();
    CHECK_FALSE(decode_monster_mission_catalog(packet));
}

TEST_CASE("Native Monster Mission requests keep stock payloads and selection is acknowledged separately") {
    const auto catalog = encode_monster_mission_catalog_request();
    CHECK(catalog.opcode == 0x3c54);
    CHECK(catalog.data == std::vector<std::byte>(8));

    const auto stats = encode_monster_mission_stats_request(234);
    CHECK(stats.opcode == 0x3d20);
    REQUIRE(stats.data.size() == 4112);
    CHECK(stats.data[8] == std::byte{234});
    CHECK(stats.data.back() == std::byte{0});

    const auto ability = encode_monster_mission_ability_request(234, 345);
    CHECK(ability.opcode == 0x34f4);
    REQUIRE(ability.data.size() == 16);
    const auto ack = decode_monster_mission_ability(ability);
    REQUIRE(ack);
    CHECK(ack->selection_id == 234);
    CHECK(ack->rank_id == 345);

    const auto item = encode_monster_mission_item_request(234, 345);
    CHECK(item.opcode == 0x5f08);
    CHECK(item.data == ability.data);
    const auto select = encode_monster_mission_select(234);
    CHECK(select.opcode == 0x4eea);
    REQUIRE(select.data.size() == 12);
    CHECK(select.data[8] == std::byte{234});

    ByteWriter w;
    w.bytes(select.data);
    w.le<std::uint32_t>(4);
    const auto refused = decode_monster_mission_result({0x5a67, w.take()});
    REQUIRE(refused);
    CHECK(refused->template_id == 234);
    CHECK(refused->result == 4);
    CHECK(refused->message_id() == 3612);

    w.zeros(8);
    w.fixed_string("PartyMember", 64);
    w.le<std::uint32_t>(234);
    const auto assignment = decode_monster_mission_assignment({0x2e8d, w.take()});
    REQUIRE(assignment);
    CHECK(assignment->name == "PartyMember");
    CHECK(assignment->template_id == 234);
}

TEST_CASE("Native Monster Mission previews decode padded stats, hex detail IDs and item-only replies") {
    ByteWriter w;
    w.zeros(16);
    detail::write_cstring(w, "42^Scout^Tracking^3^20^100^30^40^75^73^40^67^26^52^23^1^0xe58^0x2^1^0x81^Hatchet");
    w.zeros(40);
    AppPacket packet(0x3d20, w.take());
    const auto preview = decode_monster_mission_stats(packet);
    REQUIRE(preview);
    CHECK(preview->selection_id == 42);
    REQUIRE(preview->abilities.size() == 1);
    CHECK(preview->abilities.front().rank_id == 3672);
    CHECK(preview->abilities.front().rank_value == 2);
    REQUIRE(preview->items.size() == 1);
    CHECK(preview->items.front().item_id == 129);
    packet.data.back() = std::byte{1};
    CHECK_FALSE(decode_monster_mission_stats(packet));

    Item item;
    item.id = 129;
    item.name = "Hatchet";
    const auto body = encode_char_inventory({item});
    w.le(item.id);
    w.bytes(std::span(body.data).subspan(4));
    const auto reply = decode_monster_mission_item({0x5f08, w.take()});
    REQUIRE(reply);
    CHECK(reply->selection_id == 0);
    CHECK(reply->item.name == "Hatchet");
}
