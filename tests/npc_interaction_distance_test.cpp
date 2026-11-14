#include <doctest/doctest.h>

#include "core/byte_io.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/market_packets.h"
#include "net/merchant_packets.h"
#include "net/packet_io.h"
#include "net/training_packets.h"
#include "net/trade_packets.h"
#include "net/tribute_packets.h"

using namespace hailstone;

namespace {
constexpr std::uint32_t kNpc = 5002;
enum class Service { Merchant, Bank, Points, AdventureMerchant, Tribute, GuildTribute, Trainer, Recruiter, Mercenary, Barter, Handin };
constexpr Service services[] = {Service::Merchant, Service::Bank, Service::Points, Service::AdventureMerchant,
                                Service::Tribute, Service::GuildTribute, Service::Trainer, Service::Recruiter,
                                Service::Mercenary, Service::Barter, Service::Handin};

void open_service(net::GameConnection& c, Service service) {
    auto npc = *c.world().player();
    npc.id = kNpc;
    npc.name = "Service00";
    npc.is_npc = true;
    npc.is_corpse = false;
    switch (service) {
    case Service::Merchant: npc.class_id = 41; break;
    case Service::Bank: npc.class_id = 40; break;
    case Service::Points: npc.class_id = 70; break;
    case Service::AdventureMerchant: npc.class_id = 61; break;
    case Service::Tribute: npc.class_id = 63; break;
    case Service::GuildTribute: npc.class_id = 64; break;
    case Service::Trainer: npc.class_id = static_cast<std::uint8_t>(c.profile()->class_ + 19); break;
    case Service::Recruiter: npc.class_id = 60; break;
    case Service::Mercenary: npc.class_id = game::kMercLiaisonClass; break;
    case Service::Barter: npc.buyer = true; break;
    case Service::Handin: npc.class_id = 1; npc.heights.avatar_height = 0; break;
    }
    c.world_for_test().spawns[kNpc] = npc;
    switch (service) {
    case Service::Merchant: {
        c.open_merchant(kNpc);
        net::MerchantClick reply;
        reply.npc_id = kNpc;
        reply.player_id = c.world().player_id;
        c.receive_zone_packet_for_test(net::encode(reply));
        break;
    }
    case Service::Bank: c.open_bank(kNpc); break;
    case Service::Points: REQUIRE(c.open_point_merchant(kNpc)); break;
    case Service::AdventureMerchant: REQUIRE(c.open_adventure_merchant(kNpc)); break;
    case Service::Tribute:
    case Service::GuildTribute: {
        auto reply = net::encode(net::StartTribute{c.world().player_id, kNpc, 1});
        if (service == Service::GuildTribute) reply.opcode = net::to_wire(net::Op::OpenGuildTributeMaster);
        c.receive_zone_packet_for_test(reply);
        break;
    }
    case Service::Trainer: {
        REQUIRE(c.open_trainer(kNpc));
        net::GMTraining reply;
        reply.trainer_id = kNpc;
        reply.success = 1;
        c.receive_zone_packet_for_test(net::encode(reply));
        break;
    }
    case Service::Recruiter: REQUIRE(c.open_adventure_recruiter(kNpc)); break;
    case Service::Mercenary: REQUIRE(c.open_merc_liaison(kNpc)); break;
    case Service::Barter: {
        REQUIRE(c.open_buyer(kNpc));
        ByteWriter w;
        w.le(net::kBuyerInspectBegin);
        w.le(kNpc);
        w.le(std::uint32_t{1});
        c.receive_zone_packet_for_test(net::AppPacket(net::to_wire(net::Op::Barter), w.take()));
        break;
    }
    case Service::Handin:
        c.give_to(kNpc);
        c.receive_zone_packet_for_test(net::encode_trade_request_ack({c.world().player_id, kNpc}));
        break;
    }
}

bool active(const net::GameConnection& c, Service service) {
    switch (service) {
    case Service::Merchant: return c.merchant() && c.merchant()->open;
    case Service::Bank: return c.bank().has_value();
    case Service::Points: return c.point_merchant().npc != 0;
    case Service::AdventureMerchant: return c.adventure_merchant().npc != 0;
    case Service::Tribute: return c.tribute().master_id != 0;
    case Service::GuildTribute: return c.guild_tribute().master_id != 0;
    case Service::Trainer: return c.training().has_value();
    case Service::Recruiter: return c.adventure().recruiter != 0;
    case Service::Mercenary: return c.merc_window_open(0);
    case Service::Barter: return c.barter_merchant().buyer_id != 0;
    case Service::Handin: return c.trade() && c.trade()->open;
    }
    return false;
}
} // namespace

TEST_CASE("NPC services: connection updates end interactions outside 3D reach or without the NPC") {
    for (auto service : services) {
        for (int reason = 0; reason < 5; ++reason) {
            CAPTURE(static_cast<int>(service));
            CAPTURE(reason);
            mq_test::World world("npc_interaction_distance");
            auto& c = *world.conn;
            open_service(c, service);
            REQUIRE(active(c, service));
            switch (reason) {
            case 0: c.set_player_motion({20.25f, 0, 0}, 0, {}, true); break;
            case 1: c.set_player_motion({0, 0, 20.25f}, 0, {}, true); break;
            case 2: c.world_for_test().spawns.erase(kNpc); break;
            case 3: c.world_for_test().spawns[kNpc].position = {0, 21, 0}; break;
            case 4: c.world_for_test().spawns.erase(c.world().player_id); break;
            }
            c.update();
            CHECK_FALSE(active(c, service));
        }
        mq_test::World boundary("npc_interaction_boundary");
        auto& c = *boundary.conn;
        open_service(c, service);
        c.set_player_motion({12, 0, 16}, 0, {}, true); // exactly 20 units, including height
        c.update();
        CHECK(active(c, service));
    }
}

TEST_CASE("NPC services: closing point merchants discards their active and queued purchase questions") {
    mq_test::World world("npc_interaction_questions");
    auto& c = *world.conn;
    for (auto service : {Service::Points, Service::AdventureMerchant}) {
        c.set_player_motion({0, 0, 0}, 0, {}, false);
        open_service(c, service);
        if (service == Service::Points) {
            auto& m = const_cast<net::GameConnection::PointMerchant&>(c.point_merchant());
            m.list.items.push_back(net::PointMerchantItem{"Test item", 1001, 10});
            c.point_merchant_purchase(1001);
            c.point_merchant_purchase(1001);
        } else {
            auto& m = const_cast<net::GameConnection::AdventureMerchant&>(c.adventure_merchant());
            m.items.push_back(net::AdventureMerchantItem{"Test item", 1001, 10, 1});
            c.adventure_merchant_purchase(1001);
            c.adventure_merchant_purchase(1001);
        }
        REQUIRE(c.confirmation());
        c.set_player_motion({0, 0, 21}, 0, {}, true);
        c.update();
        CHECK_FALSE(c.confirmation());
        c.set_player_motion({0, 0, 0}, 0, {}, false);
        open_service(c, service);
        c.answer_confirmation(true);
        CHECK_FALSE(c.confirmation());
    }
}

TEST_CASE("NPC services: late tribute and trainer opens cannot reopen an out-of-range window") {
    for (auto service : {Service::Tribute, Service::GuildTribute, Service::Trainer}) {
        mq_test::World world("npc_interaction_late_open");
        auto& c = *world.conn;
        open_service(c, service);
        c.set_player_motion({0, 0, 21}, 0, {}, true);
        c.update();
        if (service == Service::Trainer) {
            net::GMTraining reply;
            reply.trainer_id = kNpc;
            reply.success = 1;
            c.receive_zone_packet_for_test(net::encode(reply));
        } else {
            auto reply = net::encode(net::StartTribute{c.world().player_id, kNpc, 1});
            if (service == Service::GuildTribute) reply.opcode = net::to_wire(net::Op::OpenGuildTributeMaster);
            c.receive_zone_packet_for_test(reply);
        }
        CHECK_FALSE(active(c, service));
    }
}

TEST_CASE("NPC services: distance closure sends normal session end packets once") {
    for (auto service : {Service::Merchant, Service::Trainer, Service::Barter, Service::Handin}) {
        test::Rig rig;
        REQUIRE(rig.to_zone());
        auto& c = rig.conn;
        const auto at = c.world().player()->position;
        open_service(c, service);
        REQUIRE(active(c, service));
        const auto op = service == Service::Merchant ? net::Op::ShopEnd :
                        service == Service::Trainer ? net::Op::GMEndTraining :
                        service == Service::Handin ? net::Op::CancelTrade : net::Op::Barter;
        auto end_count = [&] {
            int count = 0;
            for (const auto& p : rig.server.zone_log) {
                if (!net::is_op(p, op)) continue;
                ByteReader r(p.data);
                if (service != Service::Barter || r.le<std::uint32_t>() == net::kBuyerInspectEnd) ++count;
            }
            return count;
        };
        const auto before = end_count();
        c.set_player_motion(at + glm::vec3{0, 0, 21}, 0, {}, true);
        REQUIRE(rig.until([&] { return !active(c, service) && end_count() > before; }));
        for (int i = 0; i < 4; ++i) rig.step();
        const net::AppPacket* last = nullptr;
        int ends = 0;
        for (const auto& p : rig.server.zone_log) {
            if (!net::is_op(p, op)) continue;
            ByteReader r(p.data);
            if (service == Service::Barter && r.le<std::uint32_t>() != net::kBuyerInspectEnd) continue;
            ++ends;
            last = &p;
        }
        REQUIRE(last);
        CHECK(ends == 1);
        ByteReader r(last->data);
        if (service == Service::Barter) CHECK(r.le<std::uint32_t>() == net::kBuyerInspectEnd);
        CHECK(r.le<std::uint32_t>() == kNpc);
    }
}
