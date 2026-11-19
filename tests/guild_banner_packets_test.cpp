#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <numeric>
#include <string>
#include <string_view>

#include "core/byte_io.h"
#include "net/guild_banner_packets.h"
#include "net/opcodes.h"

using namespace hailstone;
using namespace hailstone::net;

namespace {

constexpr std::uint32_t guild_id = 0x1234;
constexpr std::uint32_t character_id = 0x10203040;

AppPacket kit_list(std::uint32_t count = 2) {
    ByteWriter w;
    w.le<std::uint32_t>(0xfedcba98);
    w.le(count);
    w.le<std::uint32_t>(17);
    w.le<std::uint32_t>(59);
    w.le<std::uint32_t>(50021);
    w.fixed_string("Standard", 128);
    w.le<std::uint8_t>(1);
    w.le<std::uint32_t>(40300);
    w.le<std::uint32_t>(50022);
    w.fixed_string("Greater Standard", 128);
    w.le<std::uint8_t>(0);
    w.le<std::uint32_t>(40301);
    return {0x0001, w.take()};
}

AppPacket banner_status(std::uint32_t mode = 2, std::uint32_t kit = 50021, std::uint32_t planted = 50022) {
    ByteWriter w;
    w.le<std::uint32_t>(0xfedcba98);
    w.le(mode);
    w.le<std::uint32_t>(0x98765432);
    w.le(kit);
    w.le<std::uint32_t>(40300);
    w.le<std::int32_t>(-1234);
    w.fixed_string("Active Standard", 128);
    w.zeros(16);
    w.le(planted);
    w.le<std::uint32_t>(16);
    w.le<std::uint32_t>(58);
    w.le<std::uint32_t>(0x01223344);
    w.le<std::uint32_t>(0x89667788);
    return {0x56c9, w.take()};
}

AppPacket modification_token(std::uint32_t id, std::string_view name) {
    ByteWriter w;
    w.le<std::uint32_t>(0xfedcba98);
    w.le<std::uint32_t>(0x98765432);
    w.le(id);
    w.le<std::uint32_t>(0x12345678);
    w.fixed_string(name, 64);
    return {0x1e50, w.take()};
}

AppPacket prefix(const AppPacket& packet, std::size_t size) {
    return {packet.opcode, {packet.data.begin(), packet.data.begin() + static_cast<std::ptrdiff_t>(size)}};
}

}

TEST_CASE("guild banners: window open, status request and removal use native wire layouts") {
    const auto open = encode_guild_banner_open(guild_id, character_id);
    CHECK(open.opcode == 0x0276);
    REQUIRE(open.data.size() == 8);
    ByteReader open_fields(open.data);
    CHECK(open_fields.le<std::uint32_t>() == guild_id);
    CHECK(open_fields.le<std::uint32_t>() == character_id);

    const auto status = encode_guild_banner_status_request(guild_id, character_id);
    CHECK(status.opcode == 0x4924);
    REQUIRE(status.data.size() == 80);
    ByteReader status_fields(status.data);
    CHECK(status_fields.le<std::uint32_t>() == character_id);
    CHECK(status_fields.le<std::uint32_t>() == guild_id);
    CHECK(std::all_of(status.data.begin() + 8, status.data.end(), [](auto byte) { return byte == std::byte{}; }));

    const auto remove = encode_guild_banner_remove(guild_id);
    CHECK(remove.opcode == 0x9e18);
    REQUIRE(remove.data.size() == 12);
    ByteReader remove_fields(remove.data);
    CHECK(remove_fields.le<std::uint32_t>() == 0xffffffffu);
    CHECK(remove_fields.le<std::uint32_t>() == guild_id);
    CHECK(remove_fields.le<std::uint32_t>() == 0);
}

TEST_CASE("guild banners: kit requests keep 32 item slots and an independent category mask") {
    const std::array<std::uint32_t, 2> owned{50021, 50022};
    const auto request = encode_guild_banner_kit_request(character_id, false, 7, owned);
    CHECK(request.opcode == 0x0d92);
    REQUIRE(request.data.size() == 141);
    ByteReader fields(request.data);
    CHECK(fields.le<std::uint32_t>() == character_id);
    CHECK(fields.le<std::uint8_t>() == 1);
    for (const auto id : owned) CHECK(fields.le<std::uint32_t>() == id);
    for (std::size_t slot = owned.size(); slot < 32; ++slot) CHECK(fields.le<std::uint32_t>() == 0);
    CHECK(fields.le<std::uint32_t>() == 2);
    CHECK(fields.le<std::uint32_t>() == 0x80);

    std::array<std::uint32_t, 34> inventory{};
    std::iota(inventory.begin(), inventory.end(), 50021u);
    const auto capped = encode_guild_banner_kit_request(character_id, true, 31, inventory);
    REQUIRE(capped.data.size() == 141);
    ByteReader capped_fields(capped.data);
    capped_fields.skip(4);
    CHECK(capped_fields.le<std::uint8_t>() == 0);
    for (std::size_t slot = 0; slot < 32; ++slot) CHECK(capped_fields.le<std::uint32_t>() == inventory[slot]);
    CHECK(capped_fields.le<std::uint32_t>() == 32);
    CHECK(capped_fields.le<std::uint32_t>() == 0x80000000u);

    for (const unsigned filter : {0u, 32u}) {
        CAPTURE(filter);
        const auto all = encode_guild_banner_kit_request(character_id, true, filter, {});
        REQUIRE(all.data.size() == 141);
        ByteReader all_fields(all.data);
        all_fields.seek(133);
        CHECK(all_fields.le<std::uint32_t>() == 0);
        CHECK(all_fields.le<std::uint32_t>() == 0xffffffffu);
    }
}

TEST_CASE("guild banners: save and plant distinguish design token, chosen kit and reapply") {
    const auto save = encode_guild_banner_save(character_id, {16, 58, 0xcc223344, 0x88667788}, 60012);
    CHECK(save.opcode == 0x4377);
    REQUIRE(save.data.size() == 24);
    ByteReader design(save.data);
    CHECK(design.le<std::uint32_t>() == character_id);
    CHECK(design.le<std::uint32_t>() == 16);
    CHECK(design.le<std::uint32_t>() == 58);
    CHECK(design.le<std::uint32_t>() == 0x00223344);
    CHECK(design.le<std::uint32_t>() == 0x00667788);
    CHECK(design.le<std::uint32_t>() == 60012);

    const auto plant = encode_guild_banner_plant(50021, true, 100.25f, -20.5f, 32.0f);
    CHECK(plant.opcode == 0x765b);
    REQUIRE(plant.data.size() == 87);
    ByteReader placement(plant.data);
    CHECK(placement.le<std::uint32_t>() == 50021);
    CHECK(std::all_of(plant.data.begin() + 4, plant.data.begin() + 68, [](auto byte) { return byte == std::byte{}; }));
    placement.seek(68);
    CHECK(placement.f32() == 100.25f);
    CHECK(placement.f32() == -20.5f);
    CHECK(placement.f32() == 32.0f);
    CHECK(placement.le<std::uint8_t>() == 1);
    CHECK(placement.le<std::uint8_t>() == 0);
    CHECK(std::all_of(plant.data.begin() + 82, plant.data.end(), [](auto byte) { return byte == std::byte{}; }));

    const auto reapply = encode_guild_banner_plant(50022, false, 0, 0, 0, true);
    CHECK(reapply.opcode == 0x765b);
    REQUIRE(reapply.data.size() == 87);
    ByteReader refresh(reapply.data);
    CHECK(refresh.le<std::uint32_t>() == 50022);
    CHECK(std::all_of(reapply.data.begin() + 4, reapply.data.begin() + 81, [](auto byte) { return byte == std::byte{}; }));
    CHECK(reapply.data[81] == std::byte{1});
    CHECK(std::all_of(reapply.data.begin() + 82, reapply.data.end(), [](auto byte) { return byte == std::byte{}; }));
}

TEST_CASE("guild banners: kit replies decode each fixed record and reject inconsistent lengths and counts") {
    CHECK(from_wire(0x0001) == Op::GuildBannerKitList);
    const auto packet = kit_list();
    const auto list = decode_guild_banner_kits(packet);
    REQUIRE(list);
    CHECK(list->background_count == 17);
    CHECK(list->emblem_count == 59);
    REQUIRE(list->kits.size() == 2);
    CHECK(list->kits[0].id == 50021);
    CHECK(list->kits[0].name == "Standard");
    CHECK(list->kits[0].description_id == 40300);
    CHECK(list->kits[0].available);
    CHECK(list->kits[1].id == 50022);
    CHECK(list->kits[1].name == "Greater Standard");
    CHECK(list->kits[1].description_id == 40301);
    CHECK_FALSE(list->kits[1].available);

    const auto empty = decode_guild_banner_kits(prefix(kit_list(0), 16));
    REQUIRE(empty);
    CHECK(empty->kits.empty());
    for (std::size_t size = 0; size < packet.data.size(); ++size) {
        CAPTURE(size);
        CHECK_FALSE(decode_guild_banner_kits(prefix(packet, size)));
    }
    // The original reads fixed offsets: rows past the count and trailing bytes are ignored.
    const auto shorter = decode_guild_banner_kits(kit_list(1));
    REQUIRE(shorter);
    REQUIRE(shorter->kits.size() == 1);
    CHECK(shorter->kits[0].id == 50021);
    auto extra = packet;
    extra.data.push_back(std::byte{});
    const auto padded = decode_guild_banner_kits(extra);
    REQUIRE(padded);
    CHECK(padded->kits.size() == 2);
    CHECK_FALSE(decode_guild_banner_kits(kit_list(3)));
    CHECK_FALSE(decode_guild_banner_kits(kit_list(0xffffffffu)));
}

TEST_CASE("guild banners: a kit reply keeps the original's first 1023 rows") {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(1500);
    w.le<std::uint32_t>(1);
    w.le<std::uint32_t>(1);
    for (std::uint32_t i = 0; i < kMaxGuildBannerKits; ++i) {
        w.le(i + 1);
        w.fixed_string("Kit", 128);
        w.le<std::uint8_t>(1);
        w.le<std::uint32_t>(0);
    }
    const AppPacket full{0x0001, w.take()};
    const auto list = decode_guild_banner_kits(full);
    REQUIRE(list);
    REQUIRE(list->kits.size() == kMaxGuildBannerKits);
    CHECK(list->kits.back().id == kMaxGuildBannerKits);
    CHECK_FALSE(decode_guild_banner_kits(prefix(full, full.data.size() - 1)));
}

TEST_CASE("guild banners: status decodes selection, remaining time and opaque tints") {
    CHECK(from_wire(0x56c9) == Op::GuildBannerStatus);
    const auto packet = banner_status();
    const auto status = decode_guild_banner_status(packet);
    REQUIRE(status);
    CHECK(status->kind == 2);
    CHECK(status->kit_id == 50021);
    CHECK(status->planted_kit_id == 50022);
    CHECK(status->description_id == 40300);
    CHECK(status->remaining_ms == -1234);
    CHECK(status->name == "Active Standard");
    CHECK(status->style == GuildBannerStyle{16, 58, 0xff223344, 0xff667788});
    CHECK(status->active());
    CHECK(status->can_remove());

    const auto inactive = decode_guild_banner_status(banner_status(0, kNoBannerKit, kNoBannerKit));
    REQUIRE(inactive);
    CHECK_FALSE(inactive->active());
    CHECK_FALSE(inactive->can_remove());
    CHECK_FALSE(decode_guild_banner_status(banner_status(1)));
    for (std::size_t size = 0; size < packet.data.size(); ++size) {
        CAPTURE(size);
        CHECK_FALSE(decode_guild_banner_status(prefix(packet, size)));
    }
    auto extra = packet;
    extra.data.push_back(std::byte{});
    const auto padded = decode_guild_banner_status(extra);
    REQUIRE(padded);
    CHECK(padded->kit_id == 50021);
}

TEST_CASE("guild banners: server token controls saving and its name stays bounded") {
    CHECK(from_wire(0x1e50) == Op::GuildBannerModificationToken);
    const auto packet = modification_token(60012, "Banner appearance token");
    const auto token = decode_guild_banner_token(packet);
    REQUIRE(token);
    CHECK(token->item_id == 60012);
    CHECK(token->item_name == "Banner appearance token");

    const auto missing = decode_guild_banner_token(modification_token(kNoBannerKit, "Required token"));
    REQUIRE(missing);
    CHECK(missing->item_id == kNoBannerKit);
    CHECK(missing->item_name == "Required token");
    for (std::size_t size = 0; size <= 16; ++size) {
        CAPTURE(size);
        CHECK_FALSE(decode_guild_banner_token(prefix(packet, size)));
    }
    auto overlong = modification_token(60012, "");
    std::fill(overlong.data.begin() + 16, overlong.data.end(), std::byte{'A'});
    const auto bounded = decode_guild_banner_token(overlong);
    REQUIRE(bounded);
    CHECK(bounded->item_name == std::string(63, 'A'));
}
