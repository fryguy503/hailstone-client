#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "core/byte_io.h"
#include "mock_server/profile_builder.h"
#include "net/door_packets.h"
#include "net/market_packets.h"
#include "net/opcodes.h"
#include "net/packet_io.h"
#include "net/player_packets.h"
#include "net/world_packets.h"
#include "net/zone_packets.h"

using namespace hailstone;
using namespace hailstone::net;

namespace {

std::uint8_t byte_at(const AppPacket& p, std::size_t i) { return std::to_integer<std::uint8_t>(p.data.at(i)); }

std::uint32_t u32_at(const AppPacket& p, std::size_t i) {
    ByteReader r(p.data);
    r.seek(i);
    return r.le<std::uint32_t>();
}

float f32_at(const AppPacket& p, std::size_t i) {
    ByteReader r(p.data);
    r.seek(i);
    return r.f32();
}

void cstr(ByteWriter& w, std::string_view s) {
    w.bytes({reinterpret_cast<const std::byte*>(s.data()), s.size()});
    w.le<std::uint8_t>(0);
}

AppPacket packet(Op op, ByteWriter& w) { return AppPacket(to_wire(op), w.take()); }

} // namespace

// ---------------------------------------------------------------- opcodes

TEST_CASE("opcode table maps names and wire values from patch_RoF2.conf") {
    CHECK(to_wire(Op::SendLoginInfo) == 0x7a09);
    CHECK(to_wire(Op::ZoneEntry) == 0x5089);
    CHECK(to_wire(Op::ClientUpdate) == 0x7dfc);
    CHECK(to_wire(Op::SendCharInfo) == 0x00d2);
    CHECK(to_wire(Op::SkillUpdate) == 0x004c); // written 0x04c in the conf
    CHECK(to_wire(Op::ReqNewZone) == 0x7887);
    CHECK(to_wire(Op::MercenarySwitch) == 0x1b37);
    CHECK(to_wire(Op::ZoneServerReady) == 0);   // no RoF2 value
    CHECK(from_wire(0x7a09) == Op::SendLoginInfo);
    CHECK(from_wire(0x345d) == Op::ClientReady);
    CHECK(from_wire(0) == Op::Unknown);
    CHECK(from_wire(0xfffe) == Op::Unknown);
    CHECK(std::string(op_name(Op::World_Client_CRC1)) == "World_Client_CRC1");
    CHECK(std::string(op_name(static_cast<Op>(60000))) == "Unknown");
    // Every non-zero value round-trips to a name with the same value.
    for (std::uint16_t i = 0; i < static_cast<std::uint16_t>(Op::Count_); ++i) {
        auto op = static_cast<Op>(i);
        if (to_wire(op)) CHECK(to_wire(from_wire(to_wire(op))) == to_wire(op));
    }
}

TEST_CASE("opcode table: both native Select Reward managers have independent stock mappings") {
    CHECK(to_wire(Op::AchievementReward) == 0x6411);
    CHECK(to_wire(Op::RewardSelection) == 0x6471);
    CHECK(from_wire(0x6411) == Op::AchievementReward);
    CHECK(from_wire(0x6471) == Op::RewardSelection);
    CHECK(std::string(op_name(Op::AchievementReward)) == "AchievementReward");
    CHECK(std::string(op_name(Op::RewardSelection)) == "RewardSelection");
}

TEST_CASE("opcode table: stock achievement managers retain their native wire identities") {
    for (const auto& [opcode, wire] : {std::pair{Op::AchievementDefinitions, 0xdab0},
         std::pair{Op::AchievementUpdate, 0x618f}, std::pair{Op::AchievementProgress, 0x2b42},
         std::pair{Op::AchievementState, 0x059d}, std::pair{Op::AchievementComparisonReply, 0x28e0},
         std::pair{Op::AchievementEarned, 0x17f8}, std::pair{Op::AchievementWindow, 0x171e},
         std::pair{Op::AchievementRequest, 0x47cb}, std::pair{Op::AchievementLinkRequest, 0x675d},
         std::pair{Op::AchievementCompareRequest, 0x1665}}) {
        CHECK(to_wire(opcode) == wire);
        CHECK(from_wire(static_cast<std::uint16_t>(wire)) == opcode);
    }
}

TEST_CASE("eq_crc32 is the client's __MemChecker0 (CRC32 without final inversion)") {
    const char* s = "123456789";
    // Standard CRC-32 of "123456789" is 0xcbf43926; without the final xor it is ~that.
    CHECK(eq_crc32({reinterpret_cast<const std::byte*>(s), 9}) == (0xcbf43926u ^ 0xffffffffu));
    CHECK(eq_crc32({}) == 0xffffffffu);
}

// ---------------------------------------------------------------- world: auth

TEST_CASE("OP_SendLoginInfo is 464 bytes with account\\0key and the zoning flag at 188") {
    LoginInfo li{"12345", "ABCDEFGHIJ", true, ""};
    AppPacket p = encode(li);
    CHECK(p.opcode == 0x7a09);
    REQUIRE(p.data.size() == 464); // RoF2 world signature (rof2.cpp Register)
    CHECK(std::memcmp(p.data.data(), "12345\0ABCDEFGHIJ\0", 17) == 0);
    CHECK(byte_at(p, 64) == 0); // empty custom-files key
    CHECK(byte_at(p, 188) == 1);
    CHECK(byte_at(p, 192) == 0xcc);
    auto d = decode_login_info(p);
    REQUIRE(d);
    CHECK(d->account_id == "12345");
    CHECK(d->session_key == "ABCDEFGHIJ");
    CHECK(d->zoning);
    li.zoning = false;
    li.custom_files_key = "key1";
    p = encode(li);
    CHECK(byte_at(p, 188) == 0);
    CHECK(decode_login_info(p)->custom_files_key == "key1");
}

TEST_CASE("OP_SendLoginInfo: a key that does not fit keeps the field's last NUL") {
    LoginInfo li{"12345", std::string(57, 'K'), false, ""}; // 5 + 1 + 57 + 1 = 64: fits
    CHECK(login_info_fits(li));
    CHECK(decode_login_info(encode(li))->session_key == li.session_key);
    li.session_key.push_back('K');
    CHECK_FALSE(login_info_fits(li));
    AppPacket p = encode(li);
    CHECK(byte_at(p, 63) == 0);
    CHECK(decode_login_info(p)->session_key == std::string(57, 'K'));
}

TEST_CASE("OP_EnterWorld client record is 72 bytes") {
    AppPacket p = encode(EnterWorld{"Gennro", 0, 1});
    CHECK(p.opcode == to_wire(Op::EnterWorld));
    REQUIRE(p.data.size() == 72);
    CHECK(u32_at(p, 68) == 1);
    auto d = decode_enter_world(p);
    REQUIRE(d);
    CHECK(d->name == "Gennro");
    CHECK(d->return_home == 1);
    // Server direction: a NUL-terminated name.
    AppPacket s(to_wire(Op::EnterWorld), {std::byte{'A'}, std::byte{'b'}, std::byte{0}});
    CHECK(decode_enter_world_name(s) == "Ab");
}

TEST_CASE("file checksum packets are 2056 bytes; the server reads checksum | size << 32") {
    std::vector<std::byte> file(4096);
    for (std::size_t i = 0; i < file.size(); ++i) file[i] = static_cast<std::byte>(i * 7);
    FileChecksum c = make_file_checksum(file, 42);
    CHECK(c.size == 4096);
    CHECK(c.checksum == eq_crc32(file));
    for (std::size_t i = 0; i < 256; ++i) {
        REQUIRE(c.index[i] < 1024);
        ByteReader r(std::span<const std::byte>(file).subspan(c.index[i] * 4, 4));
        CHECK(c.value[i] == r.le<std::uint32_t>());
    }
    AppPacket p = encode(ChecksumKind::SkillCaps, c);
    CHECK(p.opcode == 0x4b8d);
    REQUIRE(p.data.size() == 2056);
    ByteReader r(p.data);
    CHECK(r.le<std::uint64_t>() == c.server_value());
    auto d = decode_file_checksum(p);
    REQUIRE(d);
    CHECK(d->index == c.index);
    CHECK(d->value == c.value);
    CHECK(encode(ChecksumKind::Exe, c).opcode == 0x0f13);
    CHECK(encode(ChecksumKind::BaseData, c).opcode == 0x298d);
}

TEST_CASE("OP_LogServer, OP_ExpansionInfo, OP_SendMaxCharacters, OP_GuildsList decode") {
    ByteWriter w;
    w.zeros(560);
    auto data = w.take();
    data[4] = std::byte{2};
    data[8] = std::byte{1};
    std::memcpy(data.data() + 36, "nms", 3);
    auto ls = decode_log_server(AppPacket(to_wire(Op::LogServer), data));
    REQUIRE(ls);
    CHECK(ls->pvp == 2);
    CHECK(ls->enable_fv == 1);
    CHECK(ls->world_short_name == "nms");

    ByteWriter e;
    e.zeros(64);
    e.le<std::uint32_t>(0x7ffff);
    CHECK(decode_expansion_info(packet(Op::ExpansionInfo, e)) == 0x7ffffu);

    ByteWriter m;
    m.le<std::uint32_t>(12);
    m.zeros(8);
    CHECK(decode_max_characters(packet(Op::SendMaxCharacters, m)) == 12u);

    ByteWriter g;
    g.zeros(64);
    g.le<std::uint32_t>(2);
    g.le<std::uint32_t>(1);
    cstr(g, "Guild One");
    g.le<std::uint32_t>(7);
    cstr(g, "Seven");
    auto gl = decode_guilds_list(packet(Op::GuildsList, g));
    REQUIRE(gl);
    REQUIRE(gl->size() == 2);
    CHECK((*gl)[1].id == 7);
    CHECK((*gl)[1].name == "Seven");
}

TEST_CASE("OP_SendMembership and OP_SendMembershipDetails as RoF2's ENCODEs write them") {
    // ENCODE(OP_SendMembership): 116 bytes, 25 entries, of which GameFeatureRestrictions_ReadEntry keeps 22.
    ByteWriter m;
    m.le<std::uint32_t>(2);
    m.le<std::uint32_t>(0x1ffff);
    m.le<std::uint32_t>(0x1ffff);
    m.le<std::uint32_t>(25);
    for (int i = 0; i < 25; ++i) m.le<std::int32_t>(i);
    REQUIRE(m.size() == 116);
    auto mem = decode_membership(packet(Op::SendMembership, m));
    REQUIRE(mem);
    CHECK(mem->level == 2);
    CHECK(mem->classes == 0x1ffffu);
    REQUIRE(mem->entries.size() == kMembershipEntryCap);
    CHECK(mem->entries.back() == 21);

    // ENCODE(OP_SendMembershipDetails): 72 settings (the last 6 zero), 15 races, 15 classes, two empty URLs.
    ByteWriter d;
    d.le<std::uint32_t>(72);
    for (std::uint32_t i = 0; i < 72; ++i) {
        d.le<std::uint32_t>(i % 3);
        d.le<std::uint32_t>(i / 3);
        d.le<std::int32_t>(i < 66 ? -1 : 0);
    }
    for (int list = 0; list < 2; ++list) {
        d.le<std::uint32_t>(15);
        for (std::uint32_t i = 0; i < 15; ++i) {
            d.le<std::uint32_t>(i == 0 ? 1 : 90286 + i);
            d.le<std::uint32_t>(i == 0 ? 0x1ffff : 4110);
        }
    }
    d.le<std::uint32_t>(0);
    d.le<std::uint32_t>(0);
    auto det = decode_membership_details(packet(Op::SendMembershipDetails, d));
    REQUIRE(det);
    REQUIRE(det->settings.size() == 72);
    CHECK(det->settings[5].level == 2);
    CHECK(det->settings[5].feature == 1);
    CHECK(det->settings[5].value == -1);
    REQUIRE(det->races.size() == 15);
    CHECK(det->classes[0].second == 0x1ffffu);
    CHECK(det->url.empty());
    // A round trip through the test encoder, and a count past the end fails.
    CHECK(decode_membership_details(encode(*det))->settings.size() == 72);
    ByteWriter bad;
    bad.le<std::uint32_t>(1000);
    CHECK_FALSE(decode_membership_details(packet(Op::SendMembershipDetails, bad)));
}

// ---------------------------------------------------------------- world: character select

TEST_CASE("OP_SendCharInfo decodes from a buffer built like ENCODE(OP_SendCharInfo)") {
    // rof2.cpp writes the name, then CharacterSelectEntry_Struct after the name
    // (its Name[1] being the terminator): 274 fixed bytes.
    ByteWriter w;
    w.le<std::uint32_t>(1);
    cstr(w, "Gennro");
    w.le<std::uint8_t>(1);      // Class
    w.le<std::uint32_t>(522);   // Race
    w.le<std::uint8_t>(50);     // Level
    w.le<std::uint8_t>(0);      // ShroudClass
    w.le<std::uint32_t>(0);     // ShroudRace
    w.le<std::uint16_t>(202);   // Zone
    w.le<std::uint16_t>(3);     // Instance
    w.le<std::uint8_t>(1);      // Gender
    w.le<std::uint8_t>(4);      // Face
    for (std::uint32_t slot = 0; slot < 9; ++slot)
        for (std::uint32_t f = 0; f < 6; ++f) w.le<std::uint32_t>(slot * 10 + f);
    w.le<std::uint8_t>(0xff);
    w.le<std::uint8_t>(0xff);
    w.le<std::uint32_t>(5);     // DrakkinTattoo
    w.le<std::uint32_t>(6);     // DrakkinDetails
    w.le<std::uint32_t>(396);   // Deity
    w.le<std::uint32_t>(10001); // Primary
    w.le<std::uint32_t>(10002); // Secondary
    for (int b : {11, 12, 13, 14, 15, 16, 1, 0}) w.le(static_cast<std::uint8_t>(b));
    w.le<std::uint32_t>(7);     // DrakkinHeritage
    w.le<std::uint8_t>(0);
    w.le<std::uint8_t>(1);      // Enabled
    w.le<std::uint32_t>(1700000000);
    w.le<std::uint8_t>(0);
    REQUIRE(w.size() == 4 + 7 + 274);
    AppPacket p = packet(Op::SendCharInfo, w);
    auto list = decode_character_select(p);
    REQUIRE(list);
    REQUIRE(list->size() == 1);
    const auto& c = (*list)[0];
    CHECK(c.name == "Gennro");
    CHECK(c.class_ == 1);
    CHECK(c.race == 522);
    CHECK(c.level == 50);
    CHECK(c.zone == 202);
    CHECK(c.instance == 3);
    CHECK(c.gender == 1);
    CHECK(c.face == 4);
    CHECK(c.equip[8].color == 85);
    CHECK(c.deity == 396);
    CHECK(c.primary_id_file == 10001);
    CHECK(c.hair_color == 11);
    CHECK(c.beard == 16);
    CHECK(c.go_home == 1);
    CHECK(c.drakkin_heritage == 7);
    CHECK(c.enabled == 1);
    CHECK(c.last_login == 1700000000u);
    // Round trip through our encoder gives the same bytes.
    CHECK(encode(*list).data == p.data);
    // Truncations and trailing bytes are rejected.
    for (std::size_t n = 0; n < p.data.size(); ++n) {
        AppPacket t(p.opcode, std::vector<std::byte>(p.data.begin(), p.data.begin() + static_cast<std::ptrdiff_t>(n)));
        CHECK_FALSE(decode_character_select(t));
    }
    p.data.push_back(std::byte{0});
    CHECK_FALSE(decode_character_select(p));
    // Zero characters: just the count.
    ByteWriter z;
    z.le<std::uint32_t>(0);
    CHECK(decode_character_select(packet(Op::SendCharInfo, z))->empty());
}

TEST_CASE("MOTD, chat server, zone server info and zone unavailable") {
    std::string motd = "Welcome to NMS";
    AppPacket m(to_wire(Op::MOTD), {});
    for (char ch : motd) m.data.push_back(static_cast<std::byte>(ch));
    m.data.push_back(std::byte{0});
    CHECK(decode_motd(m) == motd);

    std::string chat = "127.0.0.1,7778,nms.Gennro,S0000ABCD";
    AppPacket c(to_wire(Op::SetChatServer), {});
    for (char ch : chat) c.data.push_back(static_cast<std::byte>(ch));
    c.data.push_back(std::byte{0});
    auto ci = decode_chat_server(c);
    REQUIRE(ci);
    CHECK(ci->host == "127.0.0.1");
    CHECK(ci->port == 7778);
    CHECK(ci->server_short_name == "nms");
    CHECK(ci->character == "Gennro");
    CHECK(ci->connection_type == 'S');
    CHECK(ci->mail_key == "0000ABCD");

    AppPacket z = encode(ZoneServerInfo{"192.168.1.5", 7001});
    REQUIRE(z.data.size() == 130);
    CHECK(byte_at(z, 128) == 0x59); // port little-endian at +128
    CHECK(byte_at(z, 129) == 0x1b);
    auto zi = decode_zone_server_info(z);
    REQUIRE(zi);
    CHECK(zi->host == "192.168.1.5");
    CHECK(zi->port == 7001);

    ByteWriter u;
    u.fixed_string("qeynos", 16);
    u.zeros(8);
    CHECK(decode_zone_unavail(packet(Op::ZoneUnavail, u)) == "qeynos");
}

// ---------------------------------------------------------------- world: character creation

TEST_CASE("OP_ApproveName is 80 bytes: name, race, class, deity, flag") {
    AppPacket p = encode(NameApproval{"Newbie", 1, 1, 396, 0});
    CHECK(p.opcode == 0x56a2);
    REQUIRE(p.data.size() == 80);
    CHECK(u32_at(p, 64) == 1); // world reads race_id at +64
    CHECK(u32_at(p, 68) == 1); // class_id at +68
    CHECK(u32_at(p, 72) == 396);
    CHECK(decode_name_approval(p)->name == "Newbie");
    AppPacket reply(to_wire(Op::ApproveName), {std::byte{1}});
    CHECK(decode_approve_name_reply(reply) == 1);
}

TEST_CASE("OP_CharacterCreate is CharCreate_Struct, 96 bytes") {
    CharCreate cc;
    cc.gender = 1;
    cc.race = 1;
    cc.class_ = 1;
    cc.deity = 396;
    cc.start_zone = 1;
    cc.face = 3;
    cc.str = 85;
    cc.sta = 110;
    cc.agi = 80;
    cc.dex = 75;
    cc.wis = 75;
    cc.int_ = 75;
    cc.cha = 75;
    AppPacket p = encode(cc);
    CHECK(p.opcode == 0x6bbf);
    REQUIRE(p.data.size() == 96);
    CHECK(u32_at(p, 0) == 1);    // gender
    CHECK(u32_at(p, 12) == 396); // deity
    CHECK(u32_at(p, 16) == 1);   // start_zone
    CHECK(u32_at(p, 36) == 3);   // face
    CHECK(u32_at(p, 60) == 85);  // STR
    CHECK(u32_at(p, 64) == 110); // STA
    CHECK(u32_at(p, 84) == 75);  // CHA
    auto d = decode_char_create(p);
    REQUIRE(d);
    CHECK(d->sta == 110);
    CHECK(d->int_ == 75);
}

TEST_CASE("character-creation tables decode and validate like CheckCharCreateInfoSoF") {
    // Shape of world's OP_CharacterCreateRequest answer: u8 0, allocations, combos.
    // Allocation 58 and the Human Warrior rows are from the server's
    // utils/sql/svn/2024_required_update.sql.
    ByteWriter w;
    w.le<std::uint8_t>(0);
    w.le<std::uint32_t>(1);
    w.le<std::uint32_t>(58);
    for (std::uint32_t x : {85u, 75u, 80u, 85u, 75u, 75u, 75u}) w.le(x); // STR DEX AGI STA INT WIS CHA
    for (std::uint32_t x : {0u, 0u, 0u, 25u, 0u, 0u, 0u}) w.le(x);
    w.le<std::uint32_t>(2);
    for (std::uint32_t x : {0u, 1u, 1u, 396u, 58u, 1u}) w.le(x);
    for (std::uint32_t x : {2048u, 1u, 1u, 396u, 58u, 394u}) w.le(x);
    AppPacket p = packet(Op::CharacterCreateRequest, w);
    auto data = decode_char_create_data(p);
    REQUIRE(data);
    REQUIRE(data->allocations.size() == 1);
    REQUIRE(data->combos.size() == 2);
    CHECK(data->combos[1].zone == 394);
    CHECK(encode(*data).data == p.data);
    // 0x7f2d00: every required expansion bit must be owned.
    CHECK(char_create_allowed(data->combos[0], 0));
    CHECK_FALSE(char_create_allowed(data->combos[1], 0));
    CHECK_FALSE(char_create_allowed(data->combos[1], 0x3ff));
    CHECK(char_create_allowed(data->combos[1], 0x800));
    CHECK(char_create_allowed(data->combos[1], 0xfffff));
    CHECK_FALSE(char_create_allowed({.expansion_required = 0x804}, 0x800));

    auto cc = default_char_create(*data, data->combos[0], 0);
    REQUIRE(cc);
    CHECK(cc->str == 85);
    CHECK(cc->sta == 110);
    CHECK(validate_char_create(*data, *cc));
    CharCreate bad = *cc;
    bad.deity = 201; // not listed for start zone 1 here
    CHECK_FALSE(validate_char_create(*data, bad));
    bad = *cc;
    bad.str += 1; // 26 points spent of 25
    CHECK_FALSE(validate_char_create(*data, bad));
    bad = *cc;
    bad.sta -= 25;
    bad.str += 25; // moving points is fine
    CHECK(validate_char_create(*data, bad));
    bad.agi = 79; // below base
    CHECK_FALSE(validate_char_create(*data, bad));
}

TEST_CASE("OP_DeleteCharacter and OP_RandomNameGenerator") {
    AppPacket d = encode(DeleteCharacter{"Oldie"});
    CHECK(d.opcode == 0x1808);
    CHECK(d.data.size() == 6);
    CHECK(byte_at(d, 5) == 0);
    AppPacket n = encode(RandomName{1, 0, ""});
    CHECK(n.data.size() == 72);
    auto rn = decode_random_name(encode(RandomName{3, 1, "Aruna"}));
    REQUIRE(rn);
    CHECK(rn->race == 3);
    CHECK(rn->name == "Aruna");
}

// ---------------------------------------------------------------- zone: client side

TEST_CASE("OP_ZoneEntry request is 76 bytes (RoF2 zone signature)") {
    AppPacket p = encode(ZoneEntryRequest{"Gennro"});
    CHECK(p.opcode == 0x5089);
    REQUIRE(p.data.size() == 76);
    CHECK(u32_at(p, 0) == eq_crc32({reinterpret_cast<const std::byte*>("Gennro"), 6}));
    auto d = decode_zone_entry_request(p);
    REQUIRE(d);
    CHECK(d->name == "Gennro");
}

TEST_CASE("OP_ClientUpdate client record is PlayerPositionUpdateClient_Struct, 46 bytes") {
    ClientPositionUpdate u;
    u.sequence = 9;
    u.spawn_id = 321;
    u.motion.x = 100.5f;
    u.motion.y = -200.25f;
    u.motion.z = 3.75f;
    u.motion.heading = 128.0f; // quarter turn → 512 on the wire
    u.motion.delta_x = 1.5f;
    u.motion.delta_y = -0.5f;
    u.motion.delta_z = 0.25f;
    u.motion.delta_heading = -1.5f; // -30 in 10 bits
    u.motion.animation = 27;
    AppPacket p = encode(u);
    CHECK(p.opcode == 0x7dfc);
    REQUIRE(p.data.size() == 46);
    CHECK(byte_at(p, 0) == 9);
    CHECK(u32_at(p, 2) % 0x10000 == 321);
    CHECK(f32_at(p, 10) == 1.5f);           // delta_x
    CHECK((u32_at(p, 14) & 0xfff) == 512);  // heading
    CHECK(f32_at(p, 18) == 100.5f);         // x
    CHECK(f32_at(p, 22) == 0.25f);          // delta_z
    CHECK(f32_at(p, 26) == 3.75f);          // z
    CHECK(f32_at(p, 30) == -200.25f);       // y
    CHECK((u32_at(p, 34) & 0x3ff) == 27);   // animation
    CHECK(f32_at(p, 38) == -0.5f);          // delta_y
    CHECK((u32_at(p, 42) & 0x3ff) == 0x3e2); // -30
    auto d = decode_client_position_update(p);
    REQUIRE(d);
    CHECK(d->sequence == 9);
    CHECK(d->motion.heading == 128.0f);
    CHECK(d->motion.delta_heading == -1.5f);
    CHECK(d->motion.y == -200.25f);
}

TEST_CASE("handshake packets without payload and camp") {
    for (Op op : {Op::ReqClientSpawn, Op::SendExpZonein, Op::WorldObjectsSent, Op::ClientReady, Op::Logout,
                  Op::WorldClientReady}) {
        AppPacket p = make_empty(op);
        CHECK(p.opcode == to_wire(op));
        CHECK(p.data.empty());
    }
    CHECK(make_empty(Op::ReqClientSpawn).opcode == 0x35fa);
    AppPacket c = encode_camp(0);
    CHECK(c.opcode == 0x28ec);
    CHECK(c.data.size() == 4);
}

// ---------------------------------------------------------------- zone: positions

TEST_CASE("PlayerPositionUpdateServer bitfields: 19-bit coords, 12-bit heading") {
    // Hand-packed as in eq_packet_structs.h (MakeSpawnUpdate: FloatToEQ19 etc.).
    const std::int32_t x = -2280, y = -1184, z = 984; // ×8
    const std::int32_t dx = 64, dy = -100, dz = -128;  // ×64
    const std::uint32_t heading = 1024;                // ×4 → 256.0
    const std::int32_t dh = -320, anim = 33;
    ByteWriter w;
    w.le<std::uint16_t>(77);
    w.le<std::uint16_t>(0);
    w.le<std::uint32_t>((static_cast<std::uint32_t>(y) & 0x7ffff) << 12);
    w.le<std::uint32_t>((static_cast<std::uint32_t>(dz) & 0x1fff) | ((static_cast<std::uint32_t>(dx) & 0x1fff) << 13));
    w.le<std::uint32_t>((static_cast<std::uint32_t>(x) & 0x7ffff) | (heading << 19));
    w.le<std::uint32_t>((static_cast<std::uint32_t>(dh) & 0x3ff) | ((static_cast<std::uint32_t>(z) & 0x7ffff) << 10));
    w.le<std::uint32_t>((static_cast<std::uint32_t>(anim) & 0x3ff) | ((static_cast<std::uint32_t>(dy) & 0x1fff) << 10));
    AppPacket p = packet(Op::ClientUpdate, w);
    REQUIRE(p.data.size() == 24);
    auto u = decode_spawn_position_update(p);
    REQUIRE(u);
    CHECK(u->spawn_id == 77);
    CHECK(u->motion.x == -285.0f);
    CHECK(u->motion.y == -148.0f);
    CHECK(u->motion.z == 123.0f);
    CHECK(u->motion.heading == 256.0f);
    CHECK(u->motion.delta_x == 1.0f);
    CHECK(u->motion.delta_y == -100.0f / 64.0f);
    CHECK(u->motion.delta_z == -2.0f);
    CHECK(u->motion.delta_heading == -16.0f);
    CHECK(u->motion.animation == 33);
    CHECK(encode(*u).data == p.data);
}

// ---------------------------------------------------------------- zone: spawns

namespace {

// Builds one spawn exactly as rof2.cpp ENCODE(OP_ZoneSpawns) does for a
// regular (non-destructible) spawn.
AppPacket build_spawn(const std::string& name, std::uint8_t npc, std::uint32_t race, const std::string& title,
                      std::size_t* position_offset = nullptr) {
    ByteWriter w;
    cstr(w, name);
    w.le<std::uint32_t>(1234); // spawnId
    w.le<std::uint8_t>(30);    // level
    w.f32(6.0f - 0.7f);        // size - 0.7
    w.le<std::uint8_t>(npc);
    // Bitfields: gender 1, anon 2, invis 5, showhelm, targetable, showname.
    w.le<std::uint32_t>(1u | (2u << 4) | (5u << 10) | (1u << 23) | (1u << 27) | (1u << 29));
    w.le<std::uint8_t>(title.empty() ? 0 : 16); // OtherData
    w.f32(-1.0f);
    w.f32(0.0f);
    w.le<std::uint8_t>(1);       // properties count
    w.le<std::uint32_t>(1);      // body type
    w.le<std::uint8_t>(87);      // curHp
    for (int b : {1, 2, 3, 4, 5, 6}) w.le(static_cast<std::uint8_t>(b));
    w.le<std::uint32_t>(11);     // drakkin heritage
    w.le<std::uint32_t>(12);
    w.le<std::uint32_t>(13);
    w.le<std::uint8_t>(0xff);    // equip_chest2
    w.le<std::uint8_t>(0);
    w.le<std::uint8_t>(0);
    w.le<std::uint8_t>(2);       // helm
    w.f32(6.0f);                 // size
    w.le<std::uint8_t>(3);       // face
    w.f32(0.46f);
    w.f32(1.25f);
    w.le<std::uint32_t>(race);
    w.le<std::uint8_t>(0);       // holding
    w.le<std::uint32_t>(396);    // deity
    if (npc) {
        w.le<std::uint32_t>(0xffffffff);
        w.le<std::uint32_t>(0);
    } else {
        w.le<std::uint32_t>(42);
        w.le<std::uint32_t>(1);
    }
    w.le<std::uint8_t>(9);       // class
    w.le<std::uint8_t>(0);       // pvp
    w.le<std::uint8_t>(100);     // standstate
    w.le<std::uint8_t>(0);       // light
    w.le<std::uint8_t>(0);       // flymode
    cstr(w, "Lastname");
    w.le<std::uint32_t>(0);
    w.le<std::uint8_t>(1);       // guild_show
    w.le<std::uint8_t>(0);
    w.le<std::uint32_t>(0);      // pet owner
    w.le<std::uint8_t>(0);
    w.le<std::uint32_t>(0);      // PlayerState
    w.le<std::uint32_t>(0);      // npc tint
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0xffffffff);
    w.le<std::uint32_t>(0xffffffff);
    if (!npc || race <= 12 || race == 128 || race == 130 || race == 330 || race == 522) {
        for (std::uint32_t k = 0; k < 9; ++k) w.le<std::uint32_t>(0xff000000u | k);
        for (std::uint32_t k = 0; k < 9; ++k) {
            w.le<std::uint32_t>(k + 1);
            w.zeros(16);
        }
    } else {
        w.zeros(20);
        w.le<std::uint32_t>(10001); // primary
        w.zeros(16);
        w.le<std::uint32_t>(10002); // secondary
        w.zeros(16);
    }
    if (position_offset) *position_offset = w.size();
    // Spawn_Struct_Position: x=-285 y=-148 z=123 heading 256 (1024), dx=1 dy=-2 dz=0.5,
    // dheading=2 (40), animation 66.
    // Same bit layout as PlayerPositionUpdateServer (confirmed on the live server).
    w.le<std::uint32_t>((static_cast<std::uint32_t>(-1184) & 0x7ffff) << 12);
    w.le<std::uint32_t>(32u | (64u << 13));
    w.le<std::uint32_t>((static_cast<std::uint32_t>(-2280) & 0x7ffff) | (1024u << 19));
    w.le<std::uint32_t>(40u | (984u << 10));
    w.le<std::uint32_t>(66u | ((static_cast<std::uint32_t>(-128) & 0x1fff) << 10));
    if (!title.empty()) cstr(w, title);
    w.zeros(8);
    w.le<std::uint8_t>(0);
    cstr(w, "0000000000000000");
    w.le<std::uint32_t>(0xffffffff);
    w.le<std::uint32_t>(0xffffffff);
    w.zeros(29);
    return AppPacket(to_wire(Op::ZoneEntry), w.take());
}

} // namespace

TEST_CASE("spawn decodes from a buffer built like ENCODE(OP_ZoneSpawns), PC form") {
    AppPacket p = build_spawn("Gennro", 0, 1, "the Brave");
    // Size check from the encoder: 206 + name + last name + 216 + title+1.
    CHECK(p.data.size() == 206 + 6 + 8 + 216 + 10);
    auto s = decode_spawn(p);
    REQUIRE(s);
    CHECK(s->name == "Gennro");
    CHECK(s->spawn_id == 1234);
    CHECK(s->level == 30);
    CHECK(s->npc == 0);
    CHECK(s->gender == 1);
    CHECK(s->anon == 2);
    CHECK(s->invis == 5);
    CHECK(s->show_helm);
    CHECK(s->targetable);
    CHECK(s->show_name);
    CHECK_FALSE(s->targetable_with_hotkey);
    CHECK(s->body_type == 1);
    CHECK(s->hp_percent == 87);
    CHECK(s->beard == 6);
    CHECK(s->drakkin_details == 13);
    CHECK(s->texture_type == 0xff);
    CHECK(s->head_type == 2);
    CHECK(s->size == 6.0f);
    CHECK(s->run_speed == 1.25f);
    CHECK(s->race == 1);
    CHECK(s->deity == 396);
    CHECK(s->guild_id == 42);
    CHECK(s->class_ == 9);
    CHECK(s->stand_state == 100);
    CHECK(s->last_name == "Lastname");
    CHECK(s->long_equipment);
    CHECK(s->equipment_tint[8] == 0xff000008u);
    CHECK(s->equipment[4].material == 5);
    CHECK(s->motion.x == -285.0f);
    CHECK(s->motion.y == -148.0f);
    CHECK(s->motion.z == 123.0f);
    CHECK(s->motion.heading == 256.0f);
    CHECK(s->motion.delta_x == 1.0f);
    CHECK(s->motion.delta_y == -2.0f);
    CHECK(s->motion.delta_z == 0.5f);
    CHECK(s->motion.delta_heading == 2.0f);
    CHECK(s->motion.animation == 66);
    CHECK(s->title == "the Brave");
    CHECK(s->suffix.empty());
    CHECK(s->real_estate_guid == "0000000000000000");
    CHECK(s->real_estate_id == 0xffffffffu);
    // Our encoder reproduces the server bytes.
    CHECK(encode(*s).data == p.data);
}

TEST_CASE("spawn decodes the short equipment form for non-PC-model NPC races") {
    AppPacket p = build_spawn("a_gnoll01", 1, 39, "");
    CHECK(p.data.size() == 206 + 9 + 8 + 60);
    auto s = decode_spawn(p);
    REQUIRE(s);
    CHECK(s->npc == 1);
    CHECK(s->race == 39);
    CHECK(s->guild_id == 0xffffffffu);
    CHECK_FALSE(s->long_equipment);
    CHECK(s->equipment[7].material == 10001);
    CHECK(s->equipment[8].material == 10002);
    CHECK(s->motion.z == 123.0f);
    CHECK(encode(*s).data == p.data);
    // An NPC of a PC-model race uses the long form.
    AppPacket h = build_spawn("Guard_Bob", 1, 1, "");
    auto g = decode_spawn(h);
    REQUIRE(g);
    CHECK(g->long_equipment);
    // Truncated spawns are rejected.
    p.data.resize(p.data.size() - 10);
    CHECK_FALSE(decode_spawn(p));
}

TEST_CASE("spawn with an interactive (LDoN chest) block round-trips") {
    Spawn s;
    s.name = "a_chest";
    s.npc = 1;
    s.race = 378;
    s.class_ = 62;
    s.long_equipment = false;
    s.interactive = InteractiveObject{"CHEST", "name2", "str", 1, 2, {}, 1, 3};
    s.interactive->sound_ids[9] = 99;
    AppPacket p = encode(s);
    CHECK((byte_at(p, 8 + 4 + 1 + 4 + 1 + 4) & 0x04) != 0);
    auto d = decode_spawn(p);
    REQUIRE(d);
    REQUIRE(d->interactive);
    CHECK(d->interactive->model == "CHEST");
    CHECK(d->interactive->sound_ids[9] == 99);
    CHECK(d->interactive->object_type == 3);
    CHECK(d->class_ == 62);
}

// ---------------------------------------------------------------- zone: player profile

TEST_CASE("OP_PlayerProfile decodes from a buffer built like ENCODE(OP_PlayerProfile)") {
    test::ProfileSpec spec;
    spec.combat_ability_timers[2] = 1700000002;
    spec.linked_timers[5] = 1700000005;
    spec.item_recast_timers[99] = 1700000099;
    spec.adventure_points = {101, 202, 303, 404, 505};
    spec.adventure_points_available = 1515;
    AppPacket p = test::build_player_profile(spec);
    auto pp = decode_player_profile(p);
    REQUIRE(pp);
    CHECK(pp->trailing_bytes == 0);
    CHECK(pp->bandoliers.size() == 20);
    CHECK(pp->potion_belt.size() == 5);
    CHECK(pp->adventure_points == spec.adventure_points);
    CHECK(pp->adventure_points_available == 1515);
    CHECK(pp->bandoliers[0].items[0].icon == 0xffffffffu);
    CHECK(pp->checksum_size == p.data.size() - 9);
    CHECK(pp->checksum == eq_crc32(std::span<const std::byte>(p.data).subspan(8, p.data.size() - 9)));
    CHECK(pp->gender == 1);
    CHECK(pp->race == 522);
    CHECK(pp->class_ == 16);
    CHECK(pp->level == 65);
    REQUIRE(pp->binds.size() == 5);
    CHECK(pp->binds[4].zone_id == 206);
    CHECK(pp->deity == 396);
    REQUIRE(pp->equipment.size() == 22);
    CHECK(pp->equipment[8].material == 108);
    CHECK(pp->equipment_tint.size() == 9);
    CHECK(pp->hair_color == 4);
    CHECK(pp->face == 10);
    CHECK(pp->drakkin_details == 3);
    CHECK(pp->height == 5.0f);
    CHECK(pp->mana == 1234);
    CHECK(pp->cur_hp == 4321);
    CHECK(pp->str == 101);
    CHECK(pp->wis == 107);
    CHECK(pp->aa.size() == 300);
    CHECK(pp->aa[0][0] == 7);
    CHECK(pp->skills.size() == 100);
    CHECK(pp->skills[99] == 99);
    REQUIRE(pp->combat_ability_timers.size() == 20);
    CHECK(pp->combat_ability_timers[2] == 1700000002);
    REQUIRE(pp->recast_timers.size() == 20);
    CHECK(pp->recast_timers[5] == 1700000005);
    REQUIRE(pp->item_recast_timers.size() == 100);
    CHECK(pp->item_recast_timers[99] == 1700000099);
    CHECK(pp->spell_book.size() == 720);
    CHECK(pp->spell_book[0] == 200);
    CHECK(pp->mem_spells.size() == 16);
    REQUIRE(pp->buffs.size() == 42);
    CHECK(pp->buffs[0].spell_id == 278);
    CHECK(pp->buffs[0].duration == 60);
    CHECK(pp->buffs[0].level == 50);
    CHECK(pp->platinum == 1);
    CHECK(pp->copper_cursor == 8);
    CHECK(pp->hunger == 5999);
    CHECK(pp->aa_points == 33);
    CHECK(pp->endurance == 777);
    CHECK(pp->inventory_slot_mask == 0xffffffffu); // EQEmu: -1, every slot
    CHECK(pp->base_hp_total == 123);
    CHECK(pp->base_mana_total == 345);
    CHECK(pp->base_save_cold == 0x19);
    CHECK(pp->base_save_corruption == 0xf);
    CHECK(pp->base_endurance_regen == 20);
    CHECK(pp->profile_3464[0] == 0);
    CHECK(pp->name == "Gennro");
    CHECK(pp->last_name == "Smith");
    CHECK(pp->expansions == 0x7ffffu);
    CHECK(pp->languages.size() == 32);
    CHECK(pp->zone_id == 202);
    CHECK(pp->x == -285.0f);
    CHECK(pp->y == -148.0f);
    CHECK(pp->heading == 128.0f);
    CHECK(pp->gm == 1);
    CHECK(pp->guild_id == 42);
    CHECK(pp->exp == 123456789ull);
    CHECK(pp->platinum_bank == 11);
    CHECK(pp->platinum_shared == 15);
    CHECK(pp->career_tribute_points == 1200);
    CHECK(pp->leadership_exp_on);
    CHECK(pp->group_leadership_exp == 750.0);
    CHECK(pp->raid_leadership_exp == 1500.0);
    CHECK(pp->group_leadership_points == 3);
    CHECK(pp->raid_leadership_points == 7);
    REQUIRE(pp->leadership_ranks.size() == 64);
    CHECK(pp->leadership_ranks[0] == 2);
    CHECK(pp->leadership_ranks[16] == 1);
    CHECK(pp->tribute_points == 345);
    REQUIRE(pp->tributes.size() == 5);
    CHECK(pp->tributes[0] == std::pair<std::uint32_t, std::uint32_t>{7, 2});
    CHECK(pp->tributes[1].first == 0xffffffffu);
    CHECK(pp->character_id == 9876);
    CHECK(pp->aa_exp == 55555);
    CHECK(pp->show_helm == 1);
    CHECK(pp->rest_timer == 30);
    // Truncated profiles are rejected.
    p.data.resize(p.data.size() / 2);
    CHECK_FALSE(decode_player_profile(p));
}

// ---------------------------------------------------------------- zone: environment and misc

TEST_CASE("OP_NewZone round-trips at the rof2_structs.h offsets") {
    NewZone z;
    z.char_name = "Gennro";
    z.zone_short_name = "qeynos2";
    z.zone_long_name = "North Qeynos";
    z.zone_type = 1;
    z.fog_red = {10, 20, 30, 40};
    z.fog_blue = {1, 2, 3, 4};
    z.fog_minclip = {100, 200, 300, 400};
    z.fog_maxclip = {500, 600, 700, 800};
    z.gravity = 0.4f;
    z.time_type = 2;
    z.sky = 5;
    z.safe_x = 1;
    z.safe_y = 2;
    z.safe_z = 3;
    z.max_z = 1000;
    z.underworld = -1000;
    z.minclip = 450;
    z.maxclip = 1800;
    z.zone_short_name2 = "qeynos2";
    z.zone_id = 2;
    z.zone_instance = 5;
    z.fog_density = 0.33f;
    AppPacket p = encode(z);
    REQUIRE(p.data.size() == 948);
    CHECK(byte_at(p, 470) == 1);
    CHECK(byte_at(p, 471) == 10);
    CHECK(byte_at(p, 482) == 4);
    CHECK(f32_at(p, 484) == 100);
    CHECK(f32_at(p, 512) == 800);
    CHECK(f32_at(p, 516) == 0.4f);
    CHECK(byte_at(p, 570) == 5);
    CHECK(f32_at(p, 588) == 2); // safe_y
    CHECK(f32_at(p, 592) == 1); // safe_x
    CHECK(f32_at(p, 612) == 450);
    CHECK(f32_at(p, 616) == 1800);
    CHECK(u32_at(p, 852) == (2u | (5u << 16)));
    CHECK(f32_at(p, 916) == 0.33f);
    auto d = decode_new_zone(p);
    REQUIRE(d);
    CHECK(d->zone_long_name == "North Qeynos");
    CHECK(d->fog_red == z.fog_red);
    CHECK(d->fog_maxclip == z.fog_maxclip);
    CHECK(d->safe_z == 3);
    CHECK(d->underworld == -1000);
    CHECK(d->zone_instance == 5);
    CHECK(encode(*d).data == p.data);
}

TEST_CASE("the game clock runs on from OP_TimeOfDay as EQWorldData::AdvanceTime") {
    const TimeOfDay ref{13, 45, 2, 3, 3170}; // 12:45 PM on the 2nd of March 3170
    auto at = [&](std::uint32_t seconds) { return advance_time_of_day(ref, seconds); };
    auto same = [](const TimeOfDay& a, const TimeOfDay& b) {
        return a.hour == b.hour && a.minute == b.minute && a.day == b.day && a.month == b.month && a.year == b.year;
    };
    CHECK(same(at(0), ref));
    CHECK(same(at(2), ref));                                 // a game minute is 3 real seconds
    CHECK(same(at(3), TimeOfDay{13, 46, 2, 3, 3170}));
    CHECK(same(at(15 * 3), TimeOfDay{14, 0, 2, 3, 3170}));   // minutes carry into the hour
    CHECK(same(at(180), TimeOfDay{14, 45, 2, 3, 3170}));     // an hour is 3 real minutes
    // Hour 24 is midnight; past it the day turns (11 game hours from 13:45 is 00:45, hour 24; 12 is hour 1 of the 3rd).
    CHECK(same(at(11 * 180), TimeOfDay{24, 45, 2, 3, 3170}));
    CHECK(same(at(12 * 180), TimeOfDay{1, 45, 3, 3, 3170}));
    // Day 28 carries into the next month, month 12 into the next year.
    CHECK(same(advance_time_of_day(TimeOfDay{1, 0, 28, 12, 3170}, 24 * 180), TimeOfDay{1, 0, 1, 1, 3171}));
    CHECK(same(advance_time_of_day(TimeOfDay{1, 0, 1, 1, 3170}, 12 * 28 * 24 * 180), TimeOfDay{1, 0, 1, 1, 3171}));
    CHECK(advance_time_of_day(TimeOfDay{1, 0, 28, 12, 9999}, 24 * 180).year == 0u); // past 9999 it is cleared
    // The 32-bit product wraps as the original's: 2^32 / 20 s on, it starts again from the reference.
    CHECK(at(214748364u).year == 3170u + 148u); // 147 years and 11 months, which carry past month 12 from March
    CHECK(same(at(214748365u), ref));           // 20 times this wraps to 4 game seconds
}

TEST_CASE("small zone packets round-trip") {
    auto tod = decode_time_of_day(encode(TimeOfDay{13, 45, 2, 3, 3170}));
    REQUIRE(tod);
    CHECK(tod->hour == 13);
    CHECK(tod->year == 3170u);
    CHECK(encode(TimeOfDay{}).data.size() == 8);

    AppPacket wx = encode(Weather{0xff, 0x31, 0});
    CHECK(wx.data.size() == 12);
    CHECK(decode_weather(wx)->kind == 0xff);
    CHECK(decode_weather(wx)->intensity == 0x31);
    wx.data.resize(8); // EQEmu's weather changes: {kind, intensity} only
    wx.data[0] = std::byte{0};
    wx.data[4] = std::byte{1};
    REQUIRE(decode_weather(wx));
    CHECK(decode_weather(wx)->kind == 0);
    CHECK(decode_weather(wx)->intensity == 1);
    wx.data.resize(7);
    CHECK_FALSE(decode_weather(wx));

    AppPacket ds = encode_delete_spawn(99);
    CHECK(ds.data.size() == 5);
    CHECK(byte_at(ds, 4) == 1);
    CHECK(decode_delete_spawn(ds) == 99u);

    AppPacket sa = encode(SpawnAppearance{12, 14, 100});
    CHECK(sa.data.size() == 8);
    CHECK(decode_spawn_appearance(sa)->parameter == 100u);

    AppPacket hp = encode(HpUpdate{12, 500, 1000});
    CHECK(hp.data.size() == 10);
    CHECK(decode_hp_update(hp)->max_hp == 1000);

    AppPacket mh(to_wire(Op::MobHealth), {std::byte{5}, std::byte{1}, std::byte{50}});
    CHECK(decode_mob_health(mh)->spawn_id == 261);
    CHECK(decode_mob_health(mh)->percent == 50);

    ByteWriter cs;
    cs.le<std::uint32_t>(7);
    cs.f32(2.5f);
    cs.le<std::uint32_t>(0);
    cs.f32(1.0f);
    CHECK(decode_change_size(packet(Op::ChangeSize, cs))->size == 2.5f);
}

TEST_CASE("OP_ChannelMessage follows ENCODE/DECODE(OP_ChannelMessage)") {
    ChannelMessage m{"Gennro", "Friend", 0, 7, 100, "hello there"};
    AppPacket p = encode(m);
    // size = sender + target + message + 39 (rof2.cpp ENCODE).
    CHECK(p.data.size() == 6 + 6 + 11 + 39);
    CHECK(u32_at(p, 14 + 4) == 0); // language
    CHECK(u32_at(p, 14 + 8) == 7); // channel
    auto d = decode_channel_message(p);
    REQUIRE(d);
    CHECK(d->sender == "Gennro");
    CHECK(d->target == "Friend");
    CHECK(d->channel == 7);
    CHECK(d->language_skill == 100);
    CHECK(d->message == "hello there");
}

TEST_CASE("sent names and text fit stock EQEmu's fields: a NUL inside every one") {
    const std::string long_name(100, 'n');
    // OP_ChannelMessage: SendNewText's 0x40 and 0x800 (EQEmu's DECODE strcpy's the names into char[64]).
    AppPacket c = encode(ChannelMessage{long_name, long_name, 0, 7, 100, std::string(5000, 'm')});
    CHECK(c.data.size() == 63 + 1 + 63 + 1 + 0x7ff + 1 + 4 + 4 + 4 + 5 + 4 + 15);
    CHECK(c.data[63] == std::byte{0});
    CHECK(c.data[127] == std::byte{0});
    // Fixed 64-byte names: the last byte stays the terminator.
    auto nul_at = [](const AppPacket& p, std::size_t at) { return p.data.size() > at && p.data[at] == std::byte{0}; };
    CHECK(nul_at(encode(EnterWorld{long_name, 0, 0}), 63));
    NameApproval na;
    na.name = long_name;
    CHECK(nul_at(encode(na), 63));
    RandomName rn;
    rn.name = long_name;
    CHECK(nul_at(encode(rn), 8 + 63));
    ZoneEntryRequest ze;
    ze.name = long_name;
    CHECK(nul_at(encode(ze), 4 + 63));
    ZoneChange zc;
    zc.char_name = long_name;
    CHECK(nul_at(encode(zc), 63));
    ClickObjectAction co;
    co.name = long_name;
    CHECK(nul_at(encode(co), 28 + 63));
    // A buy line's names: 63 characters (EQEmu's DECODE(OP_BuyerItems) strcpy's a sold line's into char[64]).
    BuyLine bl;
    bl.enabled = true;
    bl.item_name = long_name;
    AppPacket ml = encode_modify_buy_line(bl);
    CHECK(ml.data[13 + 62] == std::byte{'n'});
    CHECK(nul_at(ml, 13 + 63));
    // The bandolier name: 31 characters and a NUL in char[32] (EQEmu's CreateBandolier strcpy's it).
    AppPacket b = encode_bandolier(kBandolierCreate, 1, std::string(40, 'b'));
    CHECK(b.data[5 + 30] == std::byte{'b'});
    CHECK(b.data[5 + 31] == std::byte{0});
}

TEST_CASE("OP_SpecialMesg and OP_FormattedMessage decode") {
    ByteWriter s;
    s.le<std::uint8_t>(0);
    s.le<std::uint8_t>(0);
    s.le<std::uint8_t>(0);
    s.le<std::uint32_t>(10);
    s.le<std::uint32_t>(55);
    cstr(s, "Guard Bob");
    s.zeros(12);
    cstr(s, "Halt!");
    auto sm = decode_special_message(packet(Op::SpecialMesg, s));
    REQUIRE(sm);
    CHECK(sm->type == 10);
    CHECK(sm->target_spawn_id == 55);
    CHECK(sm->sayer == "Guard Bob");
    CHECK(sm->message == "Halt!");

    ByteWriter f;
    f.le<std::uint32_t>(0);
    f.le<std::uint32_t>(1234);
    f.le<std::uint32_t>(15);
    cstr(f, "arg1");
    cstr(f, "arg2");
    f.le<std::uint8_t>(0);
    auto fm = decode_formatted_message(packet(Op::FormattedMessage, f));
    REQUIRE(fm);
    CHECK(fm->string_id == 1234);
    CHECK(fm->args == std::vector<std::string>{"arg1", "arg2"});
}

TEST_CASE("OP_ZoneChange is 100 bytes; OP_RequestClientZoneChange decodes") {
    ZoneChange z;
    z.char_name = "Gennro";
    z.zone_id = 2;
    z.x = 10;
    z.y = 20;
    z.z = 30;
    AppPacket p = encode(z);
    REQUIRE(p.data.size() == 100);
    CHECK(f32_at(p, 76) == 20); // y first
    CHECK(f32_at(p, 80) == 10);
    CHECK(u32_at(p, 92) == 0);  // success
    z.success = -2;
    auto d = decode_zone_change(encode(z));
    REQUIRE(d);
    CHECK(d->success == -2);
    CHECK(d->zone_id == 2);

    ByteWriter r;
    r.le<std::uint16_t>(29);
    r.le<std::uint16_t>(0);
    r.le<std::uint32_t>(0xffffffff);
    r.f32(1);
    r.f32(2);
    r.f32(3);
    r.f32(4);
    r.le<std::uint32_t>(0x0b);
    r.zeros(144);
    r.le<std::uint32_t>(0x0168b500);
    AppPacket rp = packet(Op::RequestClientZoneChange, r);
    CHECK(rp.data.size() == 176);
    auto rc = decode_request_client_zone_change(rp);
    REQUIRE(rc);
    CHECK(rc->zone_id == 29);
    CHECK(rc->x == 2);
    CHECK(rc->heading == 4);
    CHECK(rc->type == 0x0b);
}

TEST_CASE("OP_SendZonepoints: count, 32-byte entries (y before x), trailing entry ignored") {
    ByteWriter w;
    w.le<std::uint32_t>(2);
    for (std::uint32_t n : {33u, 1u}) {
        w.le(n);                       // number
        w.f32(-301.0f);                // y
        w.f32(217.0f);                 // x
        w.f32(-38.0f);                 // z
        w.f32(390.0f);                 // heading
        w.le<std::uint16_t>(n == 33 ? 45 : 2); // zone
        w.le<std::uint16_t>(0);        // instance
        w.zeros(8);
    }
    w.zeros(32); // the extra entry EQEmu always appends
    AppPacket p = packet(Op::SendZonepoints, w);
    CHECK(p.data.size() == 4 + 3 * 32);
    auto zp = decode_zone_points(p);
    REQUIRE(zp);
    REQUIRE(zp->size() == 2);
    CHECK((*zp)[0].number == 33);
    CHECK((*zp)[0].y == -301.0f);
    CHECK((*zp)[0].x == 217.0f);
    CHECK((*zp)[0].z == -38.0f);
    CHECK((*zp)[0].heading == 390.0f);
    CHECK((*zp)[0].zone_id == 45);
    CHECK((*zp)[1].number == 1);
    CHECK((*zp)[1].zone_id == 2);

    ByteWriter empty;
    empty.le<std::uint32_t>(0);
    empty.zeros(32);
    auto none = decode_zone_points(packet(Op::SendZonepoints, empty));
    REQUIRE(none);
    CHECK(none->empty());

    ByteWriter bad;
    bad.le<std::uint32_t>(5);
    bad.zeros(64);
    CHECK_FALSE(decode_zone_points(packet(Op::SendZonepoints, bad)));
}

TEST_CASE("dye: OP_Dye is the nine tints (EQ::TintProfile, 36 bytes)") {
    std::array<std::uint32_t, 9> t{};
    t[1] = 0xff102030;
    hailstone::net::AppPacket p = hailstone::net::encode_dye(t);
    CHECK(p.opcode == hailstone::net::to_wire(hailstone::net::Op::Dye));
    REQUIRE(p.data.size() == 36);
    CHECK(p.data[4] == std::byte{0x30}); // blue, green, red, use-tint
    CHECK(p.data[7] == std::byte{0xff});
}

TEST_CASE("saves: OP_Save and OP_SaveOnZoneReq are 484 bytes with a checksum of the 476 that follow the kind") {
    hailstone::net::SaveRequest v;
    v.kind = 0x21;
    v.y = 10.5f;
    v.x = -20.0f;
    v.z = 3.0f;
    v.heading = 255;
    v.armor[1].material = 7;
    v.armor[21].material2 = 9;
    v.minutes_played = 1234;
    v.primary = 10652;
    v.air_supply = 60;
    hailstone::net::AppPacket p = hailstone::net::encode_save(v);
    CHECK(p.opcode == 0x4a39);
    REQUIRE(p.data.size() == 484);
    CHECK(p.data[4] == std::byte{0x21});
    std::uint32_t sum = 0;
    std::memcpy(&sum, p.data.data(), 4);
    CHECK(sum == hailstone::net::eq_crc32({p.data.data() + 5, 476}));
    std::int32_t heading = 0;
    std::memcpy(&heading, p.data.data() + 5 + 12, 4);
    CHECK(heading == 255);
    std::uint32_t minutes = 0;
    std::memcpy(&minutes, p.data.data() + 5 + 16 + 440 + 4, 4); // after the armour and the vehicle byte's dword
    CHECK(minutes == 1234);
    auto back = hailstone::net::decode_save(p);
    REQUIRE(back);
    CHECK(back->y == 10.5f);
    CHECK(back->x == -20.0f);
    CHECK(back->armor[1].material == 7);
    CHECK(back->armor[21].material2 == 9);
    CHECK(back->primary == 10652);
    CHECK(back->air_supply == 60);
    CHECK(hailstone::net::encode_save_on_zone(v).opcode == 0x600d);
}

TEST_CASE("bug report: EQEmu's 8740-byte BugReport_Struct") {
    hailstone::net::BugReport b;
    b.category = 4;
    b.reporter = "Me";
    b.x = 1.5f;
    b.target_id = 99;
    b.target_name = "a rat";
    b.flags = hailstone::net::kBugRepeatable | hailstone::net::kBugTargetInfo;
    b.text = "It broke.";
    hailstone::net::AppPacket p = hailstone::net::encode(b);
    CHECK(p.opcode == hailstone::net::to_wire(hailstone::net::Op::Bug));
    REQUIRE(p.data.size() == hailstone::net::kBugReportSize); // size checked exactly
    auto at = [&](std::size_t o) { return std::string(reinterpret_cast<const char*>(p.data.data() + o)); };
    CHECK(at(4) == "Quest");
    CHECK(at(68) == "Me");
    CHECK(at(472) == "a rat");
    CHECK(at(2592) == "It broke.");
    std::uint32_t flags = 0, target = 0;
    std::memcpy(&flags, p.data.data() + 536, 4);
    std::memcpy(&target, p.data.data() + 324, 4);
    CHECK(flags == 5);
    CHECK(target == 99);
}

TEST_CASE("veteran rewards: the list, a claim and its reply") {
    hailstone::net::AppPacket rq = hailstone::net::encode_vet_claim(hailstone::net::kVetClaimList);
    REQUIRE(rq.data.size() == 80); // RoF2 VeteranClaim, size checked exactly
    std::vector<hailstone::net::VetReward> v{{7, 1, true, {{"Lucy's Tome", 1234, 1}, {"Cape", 55, 0}}}};
    auto d = hailstone::net::decode_vet_rewards(hailstone::net::encode(v));
    REQUIRE(d);
    REQUIRE(d->size() == 1);
    CHECK((*d)[0].claim_id == 7);
    REQUIRE((*d)[0].items.size() == 2);
    CHECK((*d)[0].items[0].name == "Lucy's Tome");
    CHECK((*d)[0].items[1].item_id == 55);
    hailstone::net::AppPacket reply = hailstone::net::encode_vet_claim(7);
    auto r = hailstone::net::decode_vet_claim_reply(reply);
    REQUIRE(r);
    CHECK(r->claim_id == 7);
    CHECK(r->claimed); // action 0
}

#include "net/movement_packets.h"

TEST_CASE("find: the request is 40 bytes; the reply's points come in order") {
    hailstone::net::FindPersonRequest fr;
    fr.kind = 5;
    fr.id = 0xffff;
    fr.at = {1.0f, 2.0f, 3.0f};
    fr.point = {7.0f, 8.0f, 9.0f};
    hailstone::net::AppPacket rq = hailstone::net::encode(fr);
    REQUIRE(rq.data.size() == 40); // RoF2 FindPersonRequest_Struct, size checked exactly
    hailstone::ByteReader r(rq.data);
    CHECK(r.le<std::uint32_t>() == 5u);
    CHECK(r.le<std::uint32_t>() == 0xffffu);
    CHECK(r.le<std::uint32_t>() == 0xffffffffu);
    CHECK(r.le<std::uint32_t>() == 0xffffffffu);
    CHECK(std::bit_cast<float>(r.le<std::uint32_t>()) == 1.0f); // FindPerson_Point order = world x, y, z
    r.skip(8);
    CHECK(std::bit_cast<float>(r.le<std::uint32_t>()) == 7.0f);
    std::vector<glm::vec3> path{{0, 0, 0}, {10, 5, 0}, {20, 5, 1}};
    hailstone::net::AppPacket reply = hailstone::net::encode_find_person_reply(path);
    CHECK(reply.data.size() == 12 * 4);
    auto d = hailstone::net::decode_find_person_reply(reply);
    REQUIRE(d);
    REQUIRE(d->size() == 4);
    CHECK((*d)[0].x == doctest::Approx(20.0f)); // the destination first
    CHECK((*d)[3].x == doctest::Approx(20.0f));
    auto none = hailstone::net::decode_find_person_reply(hailstone::net::AppPacket(hailstone::net::to_wire(hailstone::net::Op::FindPersonReply)));
    REQUIRE(none);
    CHECK(none->empty());
    // A point that is not a number or is infinite is dropped; the others stay in order.
    std::vector<glm::vec3> bad{{1, 2, 3}, {std::nanf(""), 0, 0}, {0, INFINITY, 0}, {4, 5, 6}};
    auto kept = hailstone::net::decode_find_person_reply(hailstone::net::encode_find_person_reply(bad));
    REQUIRE(kept);
    REQUIRE(kept->size() == 3); // the destination (4, 5, 6) first, then (1, 2, 3) and (4, 5, 6)
    CHECK((*kept)[0] == glm::vec3(4, 5, 6));
    CHECK((*kept)[1] == glm::vec3(1, 2, 3));
    CHECK((*kept)[2] == glm::vec3(4, 5, 6));
}

TEST_CASE("pvp: stats (4612 bytes), the leaderboard (8024) and the crystal counts") {
    hailstone::net::PvPStats s;
    s.kills = 3;
    s.points = 40;
    s.current_streak = 2;
    s.last_kill.name = "Victim";
    s.recent_kills = {{"Victim", 50, 1, 1, 0, 0, 5}};
    hailstone::net::AppPacket p = hailstone::net::encode(s);
    REQUIRE(p.data.size() == 4612);
    auto d = hailstone::net::decode_pvp_stats(p);
    REQUIRE(d);
    CHECK(d->kills == 3);
    CHECK(d->points == 40);
    CHECK(d->last_kill.name == "Victim");
    REQUIRE(d->recent_kills.size() == 1);
    CHECK(d->recent_kills[0].points == 5);
    CHECK(hailstone::net::encode_pvp_leaderboard_request(2).data.size() == 4);
    // EQEmu's reply is zeroed: no rows.
    hailstone::net::AppPacket lb(hailstone::net::to_wire(hailstone::net::Op::PVPLeaderBoardReply), std::vector<std::byte>(8024));
    auto l = hailstone::net::decode_pvp_leaderboard(lb);
    REQUIRE(l);
    CHECK(l->rows.empty());
    std::vector<std::byte> c(16);
    c[0] = std::byte{7};  // radiant now
    c[8] = std::byte{9};  // ebon now (RoF2 order)
    auto cc = hailstone::net::decode_crystal_counts(hailstone::net::AppPacket(hailstone::net::to_wire(hailstone::net::Op::CrystalCountUpdate), c));
    REQUIRE(cc);
    CHECK(cc->radiant == 7);
    CHECK(cc->ebon == 9);
}

TEST_CASE("consume: OP_Consume is RoF2's 24-byte Consume_Struct") {
    hailstone::net::AppPacket p = hailstone::net::encode_consume(hailstone::net::ItemSlot::possessions(25, 3), true, true);
    CHECK(p.opcode == hailstone::net::to_wire(hailstone::net::Op::Consume));
    REQUIRE(p.data.size() == 24); // size checked exactly
    std::uint32_t automatic = 0, type = 0;
    std::int16_t slot = 0, sub = 0;
    std::memcpy(&slot, p.data.data() + 4, 2);
    std::memcpy(&sub, p.data.data() + 6, 2);
    std::memcpy(&automatic, p.data.data() + 12, 4);
    std::memcpy(&type, p.data.data() + 16, 4);
    CHECK(slot == 25);
    CHECK(sub == 3);
    CHECK(automatic == 0xffffffffu);
    CHECK(type == 2);
    std::memcpy(&automatic, hailstone::net::encode_consume(hailstone::net::ItemSlot::possessions(23), false, false).data.data() + 12, 4);
    CHECK(automatic == 999u); // a right click
}

#include "net/group_packets.h"

TEST_CASE("OP_InspectBuffs minutes: the original's 32-bit arithmetic") {
    using namespace hailstone::net;
    // (ticks x 3 + 15) x 2 / 60 in 32 bits: 10 ticks are 1 minute, and a count past 2^31 / 6 wraps as the
    // original's wrapped product and signed division do: 0x20000000 x 6 + 30 is 0xc000001e, -1073741794 / 60.
    CHECK(inspect_buff_minutes(1) == 0);
    CHECK(inspect_buff_minutes(10) == 1);
    CHECK(inspect_buff_minutes(0x20000000) == -17895696);
    CHECK(inspect_buff_minutes(0x7fffffff) == 0);
}

#include "net/ability_packets.h"

TEST_CASE("the OP_Track reply's header and OP_ClearObject decoders") {
    using namespace hailstone::net;
    auto bytes = [](std::initializer_list<std::uint8_t> v) {
        std::vector<std::byte> out;
        for (auto b : v) out.push_back(std::byte{b});
        return out;
    };
    const auto track = to_wire(Op::Track);
    CHECK_FALSE(decode_track_reply_header(AppPacket(track, {})));
    CHECK_FALSE(decode_track_reply_header(AppPacket(track, bytes({0x4f, 0x34, 0x8b}))));
    CHECK(decode_track_reply_header(AppPacket(track, bytes({0x4f, 0x34, 0x8b, 0xff, 1}))) == 0xff8b344fu);

    auto c = decode_clear_object(encode(ClearObject{0, 7}));
    REQUIRE(c);
    CHECK(c->clear == 0);
    CHECK(c->slot == 7);
    CHECK(encode(ClearObject{1, -1}).data.size() == kClearObjectSize);
    CHECK_FALSE(decode_clear_object(AppPacket(to_wire(Op::ClearObject), bytes({1, 0, 0, 0, 2, 0, 0}))));
}
