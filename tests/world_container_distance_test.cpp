#include <doctest/doctest.h>

#include "game/coords.h"
#include "mock_server/harness.h"
#include "mq_test_world.h"
#include "net/door_packets.h"
#include "net/packet_io.h"

using namespace hailstone;

namespace {
constexpr std::uint32_t kStation = 6001;
constexpr std::uint32_t kOtherStation = 6002;

net::ItemSlot world_slot(std::int16_t slot = 0) { return {net::kInvWorld, slot, -1, -1, 0, 0}; }

void place_station(net::GameConnection& c, std::uint32_t id = kStation, std::uint32_t type = 16,
                   const glm::vec3& at = glm::vec3{0.0f}) {
    net::GroundObject station;
    station.drop_id = station.drop_id2 = id;
    station.name = "TestStation";
    station.object_type = static_cast<std::int32_t>(type);
    const auto server = game::world_to_server(at);
    station.x = server.x;
    station.y = server.y;
    station.z = server.z;
    c.receive_zone_packet_for_test(net::encode(station));
}

void answer_open(net::GameConnection& c, std::uint32_t id = kStation, std::uint32_t type = 16) {
    net::ClickObjectAction answer;
    answer.drop_id = id;
    answer.type = type;
    answer.name = "Test station";
    c.receive_zone_packet_for_test(net::encode(answer));
}

void open_station(net::GameConnection& c, std::uint32_t id = kStation, std::uint32_t type = 16,
                  const glm::vec3& at = glm::vec3{0.0f}) {
    place_station(c, id, type, at);
    REQUIRE(c.click_object(id));
    answer_open(c, id, type);
    REQUIRE(c.world_container());
    REQUIRE(c.world_container()->drop_id == id);
}

net::Item offering(std::uint32_t count = 1) {
    net::Item item;
    item.id = 9001;
    item.name = "Test component";
    item.stack_count = count;
    item.max_stack = count > 1 ? 20 : 1;
    return item;
}

void put_world_item(net::GameConnection& c, std::uint32_t count = 1) {
    net::GameConnectionTestAccess::inventory(c).put(world_slot(), offering(count));
}

net::Item carried_bag(std::uint8_t type = 10) {
    net::Item bag;
    bag.id = 9002;
    bag.name = "Carried test kit";
    bag.item_class = 1;
    bag.bag_type = type;
    bag.bag_slots = 4;
    bag.bag_size = 4;
    return bag;
}

int close_packets(const test::Rig& rig) {
    int count = 0;
    for (const auto& p : rig.server.zone_log)
        if (net::is_op(p, net::Op::ClickObjectAction) && p.data.empty()) ++count;
    return count;
}
} // namespace

TEST_CASE("world containers: distance closure covers recipes Experiment and hidden recipe windows") {
    for (int mode = 0; mode < 3; ++mode) {
        for (int reason = 0; reason < 5; ++reason) {
            CAPTURE(mode);
            CAPTURE(reason);
            mq_test::World world("world_container_distance");
            auto& c = *world.conn;
            open_station(c);
            REQUIRE(c.tradeskill().open);
            if (mode == 1) c.show_world_container_window();
            else if (mode == 2) c.close_tradeskill();
            CHECK(c.world_container()->window == (mode == 1));
            CHECK(c.tradeskill().open == (mode == 0));
            switch (reason) {
            case 0: c.set_player_motion({20.25f, 0, 0}, 0, {}, true); break;
            case 1: c.set_player_motion({0, 0, 20.25f}, 0, {}, true); break;
            case 2: c.receive_zone_packet_for_test(net::encode(net::ClickObject{kStation, 0})); break;
            case 3: c.world_for_test().spawns.erase(c.world().player_id); break;
            case 4: place_station(c, kStation, 16, {0, 21, 0}); break;
            }
            c.update();
            CHECK_FALSE(c.world_container());
            CHECK_FALSE(c.tradeskill().open);
        }
        mq_test::World boundary("world_container_boundary");
        auto& c = *boundary.conn;
        open_station(c);
        if (mode == 1) c.show_world_container_window();
        else if (mode == 2) c.close_tradeskill();
        c.set_player_motion({12, 0, 16}, 0, {}, true); // exactly 20 units with height
        c.update();
        REQUIRE(c.world_container());
        CHECK(c.world_container()->window == (mode == 1));
        CHECK(c.tradeskill().open == (mode == 0));
    }
}

TEST_CASE("world containers: canceled pending and mismatched replies cannot reopen or replace a station") {
    mq_test::World world("world_container_late_open");
    auto& c = *world.conn;
    place_station(c);
    REQUIRE(c.click_object(kStation));
    REQUIRE_FALSE(c.world_container());
    c.set_player_motion({0, 0, 21}, 0, {}, true);
    c.update();
    c.set_player_motion({0, 0, 0}, 0, {}, false);
    answer_open(c);
    CHECK_FALSE(c.world_container()); // returning before the old answer does not renew its request

    REQUIRE(c.click_object(kStation));
    place_station(c, kOtherStation);
    answer_open(c, kOtherStation);
    CHECK_FALSE(c.world_container());
    answer_open(c);
    REQUIRE(c.world_container());
    CHECK(c.world_container()->drop_id == kStation);

    REQUIRE(c.click_object(kOtherStation));
    CHECK_FALSE(c.world_container());
    answer_open(c, 0xffffffffu); // an older busy reply has no object ID and cannot cancel the newer request
    CHECK_FALSE(c.world_container());
    answer_open(c); // the earlier station's duplicate cannot consume the newer request
    CHECK_FALSE(c.world_container());
    answer_open(c, kOtherStation);
    REQUIRE(c.world_container());
    CHECK(c.world_container()->drop_id == kOtherStation);
    answer_open(c);
    REQUIRE(c.world_container());
    CHECK(c.world_container()->drop_id == kOtherStation);
}

TEST_CASE("world containers: an open reply outside reach closes its request before showing either window") {
    mq_test::World world("world_container_late_range");
    auto& c = *world.conn;
    place_station(c);
    REQUIRE(c.click_object(kStation));
    c.set_player_motion({21, 0, 0}, 0, {}, true);
    answer_open(c); // before the next connection update
    CHECK_FALSE(c.world_container());
    CHECK_FALSE(c.tradeskill().open);
    c.set_player_motion({0, 0, 0}, 0, {}, false);
    answer_open(c);
    CHECK_FALSE(c.world_container());
}

TEST_CASE("world containers: source stack questions close and server-owned item reconciliation stays intact") {
    mq_test::World world("world_container_quantity");
    auto& c = *world.conn;
    open_station(c);
    c.show_world_container_window();
    put_world_item(c, 5);
    c.click_slot(world_slot(), false, false);
    REQUIRE(c.quantity_request());
    CHECK(c.quantity_request()->slot.type == net::kInvWorld);
    c.set_player_motion({0, 0, 21}, 0, {}, true);
    c.update();
    CHECK_FALSE(c.world_container());
    CHECK_FALSE(c.quantity_request());
    c.finish_quantity(2);
    CHECK_FALSE(c.inventory().cursor());
    REQUIRE(c.inventory().at(world_slot()));
    CHECK(c.inventory().at(world_slot())->stack_count == 5);
    c.receive_zone_packet_for_test(net::encode(net::ClearObject{1, 0}));
    CHECK_FALSE(c.inventory().at(world_slot()));
}

TEST_CASE("world containers: closing leaves carried stack questions and carried tradeskill kits usable") {
    mq_test::World world("world_container_carried_items");
    auto& c = *world.conn;
    open_station(c);
    const auto bag_slot = net::ItemSlot::possessions(30);
    net::GameConnectionTestAccess::inventory(c).put(bag_slot, carried_bag());
    c.open_tradeskill_bag(bag_slot);
    REQUIRE(c.tradeskill().open);
    REQUIRE_FALSE(c.tradeskill().world);
    const auto stack_slot = net::ItemSlot::possessions(31);
    net::GameConnectionTestAccess::inventory(c).put(stack_slot, offering(5));
    c.click_slot(stack_slot, false, false);
    REQUIRE(c.quantity_request());
    c.set_player_motion({21, 0, 0}, 0, {}, true);
    c.update();
    CHECK_FALSE(c.world_container());
    CHECK(c.tradeskill().open);
    CHECK_FALSE(c.tradeskill().world);
    REQUIRE(c.quantity_request());
    CHECK(c.quantity_request()->slot.same(stack_slot));
    c.finish_quantity(2);
    REQUIRE(c.inventory().cursor());
    CHECK(c.inventory().cursor()->stack_count == 2);
}

TEST_CASE("world containers: only the closing station's mold question is discarded") {
    for (bool world_mold : {true, false}) {
        CAPTURE(world_mold);
        mq_test::World world("world_container_mold_question");
        auto& c = *world.conn;
        open_station(c, kStation, world_mold ? 0x37 : 16);
        if (world_mold) {
            put_world_item(c);
            c.combine_world_container();
        } else {
            auto bag = carried_bag(0x37);
            bag.contents.push_back({0, offering()});
            net::GameConnectionTestAccess::inventory(c).put(net::ItemSlot::possessions(30), bag);
            REQUIRE(c.combine_container(30));
        }
        REQUIRE(c.confirmation());
        REQUIRE(c.confirmation()->id == net::kConfirmMoldCombine);
        CHECK(c.confirmation()->slot.type == (world_mold ? net::kInvWorld : net::kInvPossessions));
        c.set_player_motion({0, 21, 0}, 0, {}, true);
        c.update();
        CHECK_FALSE(c.world_container());
        CHECK(c.confirmation().has_value() == !world_mold);
        c.answer_confirmation(false);
        c.set_player_motion({0, 0, 0}, 0, {}, false);
        open_station(c, kStation, 0x37);
        put_world_item(c);
        c.combine_world_container(); // discarding the old question also releases its pending combine fence
        REQUIRE(c.confirmation());
        CHECK(c.confirmation()->slot.type == net::kInvWorld);
    }
}

TEST_CASE("world containers: leaving stops a real recipe batch before another combine") {
    mq_test::World world("world_container_batch_close");
    auto& c = *world.conn;
    open_station(c);
    c.receive_zone_packet_for_test(net::encode(net::RecipeReply{16, 0, 1, 500, 20, "Test recipe"}));
    c.tradeskill_select(500);
    net::RecipeDetails detail;
    detail.recipe_id = 500;
    detail.components.push_back({5001, 1, "Healing Potion"});
    c.receive_zone_packet_for_test(net::encode(detail));
    REQUIRE(c.tradeskill_start_batch(2).empty());
    REQUIRE(c.tradeskill().batch);
    c.set_player_motion({21, 0, 0}, 0, {}, true);
    c.update();
    CHECK_FALSE(c.world_container());
    CHECK_FALSE(c.tradeskill().open);
    CHECK_FALSE(c.tradeskill().batch);
    CHECK_FALSE(c.tradeskill().combining);
    CHECK_FALSE(c.tradeskill_combine());
}

TEST_CASE("world containers: distance sends one normal close and a stale reply cannot close a newer station") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    auto& c = rig.conn;
    const auto at = c.world().player()->position;
    open_station(c, kStation, 16, at);
    c.set_player_motion(at + glm::vec3{0, 0, 21}, 0, {}, true);
    REQUIRE(rig.until([&] { return !c.world_container() && close_packets(rig) == 1; }));
    for (int i = 0; i < 4; ++i) rig.step();
    CHECK(close_packets(rig) == 1);

    c.set_player_motion(at, 0, {}, false);
    open_station(c, kOtherStation, 16, at);
    answer_open(c, kStation);
    REQUIRE(c.world_container());
    CHECK(c.world_container()->drop_id == kOtherStation);
    const int messages = test::MockEqServer::count(rig.server.zone_log, net::Op::ChannelMessage);
    c.say("world container late-reply marker");
    REQUIRE(rig.until([&] { return test::MockEqServer::count(rig.server.zone_log, net::Op::ChannelMessage) > messages; }));
    CHECK(close_packets(rig) == 1); // an ID-less duplicate close would release the newer server container
    REQUIRE(c.world_container());
    CHECK(c.world_container()->drop_id == kOtherStation);
}
