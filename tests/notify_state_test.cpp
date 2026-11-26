// The selections /notify and the stock windows share (net/game_connection.h): the task select window's offer, the
// respawn window's option, the merchant list's item and Show All, the Alt. Currency page's currency and Display
// Missing. A script's click with no UI acts on what the window would show.
#include <doctest/doctest.h>

#include "mq_test_world.h"

using namespace hailstone;
using A = net::GameConnectionTestAccess;

TEST_CASE("notify state: the task select window's offer") {
    mq_test::World w{"hailstone_notify_tasks"};
    auto& c = *w.conn;
    auto& tasks = A::tasks(c);
    c.select_task_offer(0); // no offers: nothing
    CHECK_FALSE(tasks.offers);
    game::TaskOffers o;
    o.giver_id = 11;
    o.offers.resize(2);
    o.offers[0].task_id = 100;
    o.offers[1].task_id = 200;
    tasks.offers = o;
    // The first row to start with, as the window shows it.
    REQUIRE(net::GameConnection::selected_task_offer(*tasks.offers));
    CHECK(net::GameConnection::selected_task_offer(*tasks.offers)->task_id == 100);
    c.select_task_offer(1);
    CHECK(net::GameConnection::selected_task_offer(*tasks.offers)->task_id == 200);
    c.select_task_offer(2); // past the end: kept
    c.select_task_offer(-1);
    CHECK(tasks.offers->selected == 1);
    // A new offer list starts again at its first row.
    tasks.offers = o;
    CHECK(net::GameConnection::selected_task_offer(*tasks.offers)->task_id == 100);
}

TEST_CASE("notify state: the respawn window's option") {
    mq_test::World w{"hailstone_notify_respawn"};
    auto& c = *w.conn;
    CHECK(c.respawn_selected_option() == 0); // no window
    c.set_respawn_choice(2);
    net::RespawnWindow rw;
    rw.selected = 1;
    rw.options.resize(2);
    rw.options[0].id = 1;
    rw.options[1].id = 2;
    A::respawn(c) = rw;
    CHECK(c.respawn_selected_option() == 1); // the server's default (a choice made with no window is not kept)
    c.set_respawn_choice(2);
    CHECK(c.respawn_selected_option() == 2);
    // Once one is sent the list is shut: a later pick changes nothing.
    A::respawn_chosen(c) = true;
    c.set_respawn_choice(1);
    CHECK(c.respawn_selected_option() == 2);
}

TEST_CASE("notify state: the merchant list's item and Show All") {
    mq_test::World w{"hailstone_notify_merchant"};
    auto& c = *w.conn;
    CHECK_FALSE(c.merchant_show_all()); // the list starts filtered, as MW_UsableButton starts checked
    c.set_merchant_show_all(true);
    CHECK(c.merchant_show_all());
    auto& m = A::merchant(c).emplace();
    m.open = true;
    net::Item bread;
    bread.name = "Loaf of Bread";
    m.items[3] = bread;
    m.items[7] = bread;
    m.selected = net::ItemSlot::possessions(net::kSlotGeneral1); // an inventory item picked for selling
    c.merchant_consider(7);
    REQUIRE(m.considered);
    CHECK(*m.considered == 7);
    CHECK_FALSE(m.selected); // the list's click lets the inventory item go
    c.merchant_consider(5); // not the merchant's: kept
    CHECK(*m.considered == 7);
    c.merchant_drop_considered();
    CHECK_FALSE(m.considered);
    m.open = false;
    c.merchant_consider(3); // closed: nothing
    CHECK_FALSE(m.considered);
}

TEST_CASE("notify state: the Alt. Currency page's list and pick") {
    mq_test::World w{"hailstone_notify_altcurrency"};
    auto& c = *w.conn;
    auto& cur = A::alt_currencies(c);
    auto add = [&](std::uint32_t id, std::uint32_t amount, bool shown) {
        net::GameConnection::AltCurrency a;
        a.entry.id = id;
        a.entry.shown = shown;
        a.amount = amount;
        cur[id] = a;
    };
    add(1, 5, true);
    add(2, 0, true);  // none held
    add(3, 2, false); // not listed by the server
    CHECK(c.alt_currency_list() == std::vector<std::uint32_t>{1});
    c.set_alt_currency_show_missing(true);
    CHECK(c.alt_currency_list() == std::vector<std::uint32_t>{1, 2, 3});
    CHECK_FALSE(c.selected_alt_currency());
    c.select_alt_currency(2);
    REQUIRE(c.selected_alt_currency());
    CHECK(c.selected_alt_currency()->entry.id == 2);
    c.select_alt_currency(9); // not held
    CHECK_FALSE(c.selected_alt_currency());
}
