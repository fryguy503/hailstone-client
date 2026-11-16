// The quest journal's rows as CTaskWnd builds them (game/task_window.h): the task list (type letters, colours, the
// time left and the separators), its sort (the VS2010 qsort with the original's comparators), the objectives by step,
// the history, the description STML with the reward, and the compass search.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "game/eq_strings.h"
#include "game/task_window.h"
#include "game/tasks.h"
#include "app/lua_ui_windows.h"
#include "core/byte_io.h"
#include "mock_server/harness.h"
#include "net/task_packets.h"
#include "plugin/hooks.h"

using namespace hailstone;
using namespace hailstone::game;

namespace {

EqStrings strings() {
    EqStrings s;
    s.set(3474, "? ? ?");
    s.set(3475, "Unlimited");
    s.set(3476, "Done");
    s.set(3477, "Reward(s):<BR>");
    s.set(13205, "ALL");
    s.set(6115, "(Opt) ");
    s.set(138, "You gain experience!!");
    s.set(8957, "Radiant Crystals");
    s.set(8958, "Ebon Crystals");
    s.set(1215, "points");
    s.set(2409, " %1 platinum");
    s.set(2410, " %1 gold");
    s.set(2411, " %1 silver");
    s.set(2412, " %1 copper");
    return s;
}

std::string text_of(const TaskElement& e) { return e.description; }
std::string zone_of(std::uint32_t id) { return id == 1 ? "South Qeynos" : id == 2 ? "North Qeynos" : ""; }

TaskElement element(std::uint32_t slot, std::uint32_t step, std::int32_t type, std::string text, int done, int goal) {
    TaskElement e;
    e.activity_id = slot;
    e.step = step;
    e.type = type;
    e.hidden = type == -1;
    e.description = std::move(text);
    e.done_count = done;
    e.goal_count = goal;
    return e;
}

} // namespace

TEST_CASE("task window: the VS2010 qsort") {
    // A consistent comparator sorts; ties keep the CRT's own order.
    std::vector<int> v{5, 3, 9, 1, 7, 2, 8, 6, 4, 0, 11, 10};
    msvc_qsort(v, [](int a, int b) { return a < b ? -1 : a > b ? 1 : 0; });
    CHECK(std::is_sorted(v.begin(), v.end()));
    // shortsort (8 or fewer): the largest moved to the end, the later of two equal maxima only when greater.
    std::vector<std::pair<int, char>> p{{1, 'a'}, {2, 'b'}, {1, 'c'}, {2, 'd'}};
    msvc_qsort(p, [](const auto& a, const auto& b) { return a.first - b.first; });
    CHECK(p == std::vector<std::pair<int, char>>{{1, 'c'}, {1, 'a'}, {2, 'd'}, {2, 'b'}});
}

TEST_CASE("task window: the task list's rows, colours, times and separators") {
    auto s = strings();
    TaskState st;
    Task t;
    t.type = kTaskTypeTask;
    t.task_id = 10;
    t.title = "Rat hunt";
    t.duration = 3600;
    t.elapsed = 60;
    t.received_ms = 1000000;
    st.tasks.push_back(t);
    Task q;
    q.type = kTaskTypeQuest;
    q.sequence = 1;
    q.task_id = 20;
    q.title = "Bandit";
    st.tasks.push_back(q);
    q.sequence = 0;
    q.task_id = 21;
    q.title = "Apple";
    q.duration = 100;
    q.received_ms = 1000000;
    st.tasks.push_back(q);

    std::int64_t now = 1000000 + 10'000; // 10 s after arrival
    auto tasks = collect_task_rows(st, kTaskTypeTask, now);
    auto quests = collect_task_rows(st, kTaskTypeQuest, now);
    auto shared = collect_task_rows(st, kTaskTypeShared, now);
    REQUIRE(tasks.size() == 1);
    CHECK(tasks[0].left == 3600 - 60 - 10);
    REQUIRE(quests.size() == 2);
    CHECK(quests[0].title == "Apple"); // slot order
    CHECK(quests[1].left == kTaskUntimed);
    sort_task_rows(quests, 0, true);
    CHECK(quests[0].title == "Apple");
    sort_task_rows(quests, 0, false);
    CHECK(quests[0].title == "Bandit");

    auto rows = task_list_rows(tasks, shared, quests, s);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].texts == std::vector<std::string>{"T", "Rat hunt", "00:58:50"});
    CHECK(rows[0].colors == std::vector<std::uint32_t>{0xffdcdc80, 0xffdcdc80, 0xffffffff});
    CHECK(rows[1].separator);
    CHECK(rows[2].texts == std::vector<std::string>{"Q", "Bandit", "Unlimited"});
    CHECK(rows[2].colors[0] == 0xffffffff);
    // Under 120 s left: red (columns 0 and 1).
    CHECK(rows[3].texts[2] == "00:01:30");
    CHECK(rows[3].colors == std::vector<std::uint32_t>{0xffff0000, 0xffff0000, 0xffffffff});
    // The timer: a duration under 120 s turns all three red; the time counts down.
    update_task_row_timer(rows[3], st.tasks[2], now + 30'000);
    CHECK(rows[3].texts[2] == "00:01:00");
    CHECK(rows[3].colors == std::vector<std::uint32_t>{0xffff0000, 0xffff0000, 0xffff0000});
    update_task_row_timer(rows[3], st.tasks[2], now + 300'000);
    CHECK(rows[3].texts[2] == "00:00:00");
    CHECK(task_time_text(100 * 3600 + 61, s) == "100:01:01");
}

TEST_CASE("task window: the time-left sort's comparator is the original's") {
    // 0x76f9f0: rows whose times differ compare as 1 both ways; qsort then orders as the CRT does.
    std::vector<TaskListRow> rows;
    for (int i = 0; i < 4; ++i) {
        TaskListRow r;
        r.title = std::string(1, static_cast<char>('a' + i));
        r.left = 100 * (i + 1);
        rows.push_back(r);
    }
    sort_task_rows(rows, 1, true);
    // shortsort with comp(p, max) = 1 for every different time: the last row is taken as the maximum each pass.
    std::vector<std::string> titles;
    for (const auto& r : rows) titles.push_back(r.title);
    CHECK(titles == std::vector<std::string>{"a", "b", "c", "d"});
    sort_task_rows(rows, 1, false); // -1: the first row stays the maximum and is swapped to the end
    titles.clear();
    for (const auto& r : rows) titles.push_back(r.title);
    CHECK(titles == std::vector<std::string>{"b", "c", "d", "a"});
}

TEST_CASE("task window: objectives by step") {
    auto s = strings();
    Task t;
    t.task_id = 5;
    t.elements[0] = element(0, 0, 2, "kill rats", 3, 3);
    t.elements[1] = element(1, 1, 4, "talk to Bob", 0, 1);
    t.elements[2] = element(2, 1, 3, "loot fangs", 1, 4);
    t.elements[2].optional = true;
    t.elements[2].zones = "1;2";
    t.elements[3] = element(3, 2, -1, "", 0, 0);
    t.elements[4] = element(4, 3, 0, "gone", 0, 0); // type 0: not listed
    auto rows = objective_rows(t, s, [](const TaskElement& e) { return e.type == -1 ? std::string("? ? ?") : e.description; },
                               zone_of);
    REQUIRE(rows.size() == 6);
    CHECK(rows[0].texts == std::vector<std::string>{"Kill rats", "Done", "ALL"});
    CHECK(rows[0].colors == std::vector<std::uint32_t>{0xff00ff00, 0xffffffff, 0xff00ff00});
    CHECK(rows[1].separator);
    CHECK(rows[2].texts == std::vector<std::string>{"Talk to Bob", "0/1", "ALL"});
    CHECK(rows[3].texts == std::vector<std::string>{"(Opt) Loot fangs", "1/4", "South Qeynos or North Qeynos"});
    CHECK(rows[4].separator);
    CHECK(rows[5].texts == std::vector<std::string>{"? ? ?", "", "? ? ?"});
    CHECK(rows[5].colors[0] == 0xff808080);
    CHECK(task_zone_names("1;99;0", zone_of) == "South Qeynos or Unknown Zone or Unknown Zone");
}

TEST_CASE("task window: the history") {
    auto s = strings();
    std::vector<CompletedTask> done{{7, "First", 0}, {0, "none", 0}, {8, "Second", 0}};
    auto rows = history_rows(done);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].texts[0] == "Q");
    CHECK(rows[1].texts[1] == "Second");
    CHECK(rows[1].index == 2);
    std::vector<TaskElement> els{element(0, 0, 2, "kill rats", 0, 3), element(1, 0, -1, "secret", 0, 0)};
    auto er = history_element_rows(els, s, text_of, zone_of);
    REQUIRE(er.size() == 2);
    CHECK(er[0].texts == std::vector<std::string>{"Kill rats", "Done", "ALL"});
    CHECK(er[1].texts[1].empty());
    CHECK(er[0].colors[0] == 0xff00ff00);
}

TEST_CASE("task window: the description STML and the reward") {
    auto s = strings();
    Task t;
    t.type = kTaskTypeShared;
    t.description = "Go [1,find Bob]now.";
    t.elements[0] = element(0, 0, 4, "talk", 0, 1);
    t.has_rewards = true;
    t.coin_reward = 1234;
    t.xp_reward = true;
    t.points = 50;
    t.reward_type = 4;
    CHECK(task_details_stml(t, s, text_of) ==
          "Go find Bobnow.<BR><BR>Reward(s):<BR> 1 platinum 2 gold 3 silver 4 copper<BR>You gain experience!!<BR>50 "
          "Radiant Crystals");
    t.type = kTaskTypeQuest;
    t.coin_reward = 0;
    t.xp_reward = false;
    t.description.clear();
    CHECK(task_details_stml(t, s, text_of) == "Reward(s):<BR>50 points");
    t.has_rewards = false;
    CHECK(task_details_stml(t, s, text_of).empty());
    t.has_rewards = true;
    t.points = 0;
    CHECK(task_details_stml(t, s, text_of).empty()); // nothing to show
    CHECK(task_abandon_eqstr(0) == 8995);
    CHECK(task_abandon_eqstr(1) == 8996);
    CHECK(task_abandon_eqstr(2) == 8997);
}

TEST_CASE("task window: the compass mark") {
    TaskState st;
    Task t;
    t.type = kTaskTypeTask;
    t.task_id = 9;
    TaskElement kill = element(0, 0, 2, "", 0, 1);
    kill.target_name = "Guard Bob";
    t.elements[0] = kill;
    TaskElement speak = element(1, 0, 4, "", 0, 1);
    speak.target_name = "Guard Bob;fishwife";
    speak.target_zones = "2;3";
    t.elements[1] = speak;
    st.tasks.push_back(t);
    std::vector<TaskCompassSpawn> spawns{{"Guard_Bob00", true, {1, 2, 3}}, {"fishwife01", true, {4, 5, 6}},
                                         {"fishwife02", false, {7, 8, 9}}};
    CHECK_FALSE(task_compass_mark(st, 1, spawns)); // not this zone; the kill element is not a speak / deliver one
    auto m = task_compass_mark(st, 3, spawns);
    REQUIRE(m);
    CHECK(m->task_id == 9);
    CHECK(m->slot == 1);
    CHECK(m->position.x == 4.0f); // strstr is case-sensitive: "guard_bob" is not in "Guard_Bob00"
    st.tasks[0].elements[1].done_count = 1;
    CHECK_FALSE(task_compass_mark(st, 3, spawns));
}

TEST_CASE("task offers: description appends the starting zones of element slot zero") {
    auto s = strings();
    s.set(6029, "<BR><BR>Starts in %1");
    TaskOffer offer;
    offer.description = "Story [1,%1]";
    auto later = element(1, 0, 4, "Later objective", 0, 1);
    later.zones = "2";
    auto first = element(0, 0, 4, "Talk to the guard", 0, 1);
    first.zones = "1;2";
    first.target_zones = "999";
    offer.elements = {later, first}; // packet order need not be element slot order
    CHECK(task_offer_details_stml(offer, s, text_of, zone_of) ==
          "Story Talk to the guard<BR><BR>Starts in South Qeynos or North Qeynos");

    offer.description = "Story";
    for (const int type : {0, -1}) {
        offer.elements.back().type = type;
        CHECK(task_offer_details_stml(offer, s, text_of, zone_of) == "Story");
    }
    offer.elements.back().type = 4;
    offer.elements.back().zones.clear();
    CHECK(task_offer_details_stml(offer, s, text_of, zone_of) == "Story");
    offer.elements.pop_back();
    CHECK(task_offer_details_stml(offer, s, text_of, zone_of) == "Story"); // no slot 0; do not use slot 1
    first.zones = "1;999";
    offer.elements.push_back(first);
    offer.description.clear();
    CHECK(task_offer_details_stml(offer, s, text_of, zone_of) == "<BR><BR>Starts in South Qeynos or Unknown Zone");
    first.zones = "2";
    offer.elements.push_back(first); // repeated slot assignment follows the native array's last value
    offer.reward_preview = true;
    offer.reward_multiplier = 3.0f;
    CHECK(task_offer_details_stml(offer, s, text_of, zone_of) == "<BR><BR>Starts in North Qeynos");
    offer.elements.clear();
    CHECK(task_offer_details_stml(offer, s, text_of, zone_of).empty());
}

TEST_CASE("task offers: Preview Reward uses the selected received offer and refuses stale rows") {
    for (const auto op : {net::Op::TaskSelectWindow, net::Op::SharedTaskSelectWindow}) {
        CAPTURE(op);
        plugin::Hooks hooks;
        const auto owner = hooks.new_owner("task offer preview fixture");
        std::vector<net::AppPacket> requests;
        hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, 0x6471, [&](net::AppPacket& p) {
            requests.push_back(p);
            return plugin::Flow::Continue;
        });
        test::ManualClock clock;
        auto options = test::quick_options();
        options.session.clock = clock.source();
        options.hooks = &hooks;
        test::Rig rig(test::MockConfig::standard(), options);
        REQUIRE(rig.to_zone());
        clock.hold();

        TaskOffers offers;
        offers.giver_id = 123;
        offers.type = op == net::Op::SharedTaskSelectWindow ? kTaskTypeShared : kTaskTypeQuest;
        TaskOffer first;
        first.task_id = 9001;
        first.title = "First offer";
        first.description = "First description";
        TaskOffer second = first;
        second.task_id = 70000;
        second.title = "Second offer";
        second.description = "Second description";
        second.reward_preview = true;
        offers.offers = {first, second};
        rig.conn.receive_zone_packet_for_test(net::encode_task_select(offers, op));

        app::UiHost host;
        host.conn = &rig.conn;
        host.game = &rig.game;
        host.in_game = host.ui_ready = true;
        std::vector<std::unique_ptr<app::StockWindow>> windows;
        app::add_progress_windows(windows);
        const auto found = std::find_if(windows.begin(), windows.end(), [](const auto& w) { return w->screen() == "TaskSelectWnd"; });
        REQUIRE(found != windows.end());
        auto& window = **found;
        app::Child button;
        window.child(host, "TSEL_PreviewRewardButton", button);
        CHECK(button.enabled == false); // no displayed rows yet
        REQUIRE(window.click(host, "TSEL_PreviewRewardButton", "left", false));
        REQUIRE(window.list(host, "TaskList"));
        window.frame(host);
        CHECK(requests.empty());
        button = {};
        window.child(host, "TSEL_PreviewRewardButton", button);
        CHECK(button.enabled == false);
        REQUIRE(window.list_click(host, "TaskList", 1, false, 0));
        app::Child description;
        window.child(host, "Description", description);
        CHECK(description.text == second.description);
        button = {};
        window.child(host, "TSEL_PreviewRewardButton", button);
        REQUIRE(button.enabled == true);
        CHECK_FALSE(window.click(host, "TSEL_PreviewRewardButton", "right", false));
        CHECK(requests.empty());
        REQUIRE(window.click(host, "TSEL_PreviewRewardButton", "left", false));
        REQUIRE(requests.size() == 1);
        ByteReader reader(requests.front().data);
        CHECK(reader.le<std::uint32_t>() == 4u);
        CHECK(reader.le<std::uint32_t>() == second.task_id);
        CHECK(reader.empty());
        CHECK(rig.conn.tasks().offers.has_value()); // preview never accepts or declines the offer
        CHECK(rig.conn.rewards().lane(RewardLane::Preview).shown);

        clock.advance(std::chrono::milliseconds{501});
        offers.offers[1].task_id = 80000;
        rig.conn.receive_zone_packet_for_test(net::encode_task_select(offers, op));
        button = {};
        window.child(host, "TSEL_PreviewRewardButton", button);
        CHECK(button.enabled == false);
        description = {};
        window.child(host, "Description", description);
        CHECK(description.text == "");
        REQUIRE(window.click(host, "TSEL_PreviewRewardButton", "left", false));
        REQUIRE(window.click(host, "AcceptButton", "left", false));
        CHECK(requests.size() == 1);
        REQUIRE(rig.conn.tasks().offers.has_value());

        REQUIRE(window.list(host, "TaskList"));
        ++offers.giver_id; // same task ID from a different giver is also a new offer view
        rig.conn.receive_zone_packet_for_test(net::encode_task_select(offers, op));
        button = {};
        window.child(host, "TSEL_PreviewRewardButton", button);
        CHECK(button.enabled == false);
        REQUIRE(window.click(host, "TSEL_PreviewRewardButton", "left", false));
        CHECK(requests.size() == 1);

        REQUIRE(window.list(host, "TaskList"));
        offers.offers[1].reward_preview = false;
        rig.conn.receive_zone_packet_for_test(net::encode_task_select(offers, op));
        Task active;
        active.task_id = offers.offers[1].task_id;
        active.has_rewards = active.reward_set = true;
        rig.conn.receive_zone_packet_for_test(net::encode_task_description(active));
        REQUIRE(rig.conn.tasks().find_id(active.task_id));
        REQUIRE(rig.conn.tasks().find_id(active.task_id)->reward_set);
        button = {};
        window.child(host, "TSEL_PreviewRewardButton", button);
        CHECK(button.enabled == false);
        REQUIRE(window.click(host, "TSEL_PreviewRewardButton", "left", false));
        CHECK(requests.size() == 1); // an active copy must not override this offer's disabled control
        REQUIRE(window.list_click(host, "TaskList", 99, false, 0));
        REQUIRE(window.click(host, "TSEL_PreviewRewardButton", "left", false));
        CHECK(requests.size() == 1);
        window.set_shown(host, false);
        CHECK_FALSE(window.shown(host));
        CHECK_FALSE(window.click(host, "TSEL_PreviewRewardButton", "left", false));
        button = {};
        window.child(host, "TSEL_PreviewRewardButton", button);
        CHECK(button.enabled == false);
    }
}

TEST_CASE("task offers: selecting an offer without a preview hides a shown preview; a replaced list starts at row 0") {
    test::ManualClock clock;
    auto options = test::quick_options();
    options.session.clock = clock.source();
    test::Rig rig(test::MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    clock.hold();

    TaskOffers offers;
    offers.giver_id = 123;
    offers.type = kTaskTypeQuest;
    TaskOffer plain;
    plain.task_id = 9001;
    plain.title = "Plain offer";
    TaskOffer previewed = plain;
    previewed.task_id = 9002;
    previewed.title = "Previewed offer";
    previewed.reward_preview = true;
    previewed.description = "Previewed story";
    TaskOffer other = previewed;
    other.task_id = 9003;
    offers.offers = {plain, previewed, other};
    rig.conn.receive_zone_packet_for_test(net::encode_task_select(offers, net::Op::TaskSelectWindow));

    app::UiHost host;
    host.conn = &rig.conn;
    host.game = &rig.game;
    host.in_game = host.ui_ready = true;
    std::vector<std::unique_ptr<app::StockWindow>> windows;
    app::add_progress_windows(windows);
    const auto found = std::find_if(windows.begin(), windows.end(), [](const auto& w) { return w->screen() == "TaskSelectWnd"; });
    REQUIRE(found != windows.end());
    auto& window = **found;
    REQUIRE(window.list(host, "TaskList"));
    REQUIRE(window.list_click(host, "TaskList", 1, false, 0));
    REQUIRE(window.click(host, "TSEL_PreviewRewardButton", "left", false));
    REQUIRE(rig.conn.rewards().lane(RewardLane::Preview).shown);

    // CTaskSelectWnd_SelectTask @0x76b5c0: another offer with a preview leaves the window up...
    REQUIRE(window.list_click(host, "TaskList", 2, false, 0));
    CHECK(rig.conn.rewards().lane(RewardLane::Preview).shown);
    // ...one without a preview hides it.
    REQUIRE(window.list_click(host, "TaskList", 0, false, 0));
    CHECK_FALSE(rig.conn.rewards().lane(RewardLane::Preview).shown);
    CHECK(rig.conn.tasks().offers.has_value());

    // The same list redrawn keeps the selection; a replaced list starts again at its first row.
    REQUIRE(window.list_click(host, "TaskList", 2, false, 0));
    const auto* rows = window.list(host, "TaskList");
    REQUIRE(rows);
    CHECK(rows->selected == 2);
    offers.offers = {previewed, plain, other};
    rig.conn.receive_zone_packet_for_test(net::encode_task_select(offers, net::Op::TaskSelectWindow));
    rows = window.list(host, "TaskList");
    REQUIRE(rows);
    CHECK(rows->selected == 0);
    app::Child description;
    window.child(host, "Description", description);
    CHECK(description.text == previewed.description);
}

TEST_CASE("task window: Preview Reward sends the selected task ID and refuses stale slots") {
    plugin::Hooks hooks;
    const auto owner = hooks.new_owner("task preview fixture");
    std::vector<net::AppPacket> requests;
    hooks.on_packet(owner, plugin::Stream::Zone, plugin::Direction::Out, 0, [&](net::AppPacket& p) {
        if (p.opcode == net::to_wire(net::Op::RewardSelection) || p.opcode == net::to_wire(net::Op::AchievementReward))
            requests.push_back(p);
        return plugin::Flow::Continue;
    });
    test::ManualClock clock;
    auto options = test::quick_options();
    options.session.clock = clock.source();
    options.hooks = &hooks;
    test::Rig rig(test::MockConfig::standard(), options);
    REQUIRE(rig.to_zone());
    clock.hold();

    Task alpha;
    alpha.type = kTaskTypeQuest;
    alpha.sequence = 3;
    alpha.task_id = 9001;
    alpha.title = "Alpha";
    alpha.has_rewards = alpha.reward_set = true;
    Task zebra = alpha;
    zebra.sequence = 0;
    zebra.task_id = 70000;
    zebra.title = "Zebra";
    rig.server.zone.send_reliable(net::encode_task_description(alpha));
    rig.server.zone.send_reliable(net::encode_task_description(zebra));
    REQUIRE(rig.until([&] { return rig.conn.tasks().find_id(alpha.task_id) && rig.conn.tasks().find_id(zebra.task_id); }));

    app::UiHost host;
    host.conn = &rig.conn;
    host.game = &rig.game;
    host.in_game = host.ui_ready = true;
    std::vector<std::unique_ptr<app::StockWindow>> windows;
    app::add_progress_windows(windows);
    auto found = std::find_if(windows.begin(), windows.end(), [](const auto& window) { return window->screen() == "TaskWnd"; });
    REQUIRE(found != windows.end());
    auto& window = **found;
    REQUIRE(window.click(host, "TASK_PreviewRewardButton", "left", false));
    CHECK(requests.empty()); // no row has been selected/built yet
    window.set_shown(host, true);
    window.frame(host);
    CHECK(requests.empty()); // drawing/opening the journal does not ask for any rewards
    const auto* list = window.list(host, "TASK_TaskList");
    REQUIRE(list);
    const auto row = std::find_if(list->rows.begin(), list->rows.end(), [](const auto& value) {
        return value.texts.size() > 1 && value.texts[1] == "Zebra";
    });
    REQUIRE(row != list->rows.end());
    REQUIRE(window.list_click(host, "TASK_TaskList", static_cast<int>(row - list->rows.begin()), false, 0));
    app::Child button;
    window.child(host, "TASK_PreviewRewardButton", button);
    REQUIRE(button.enabled == true);
    CHECK_FALSE(window.click(host, "TASK_PreviewRewardButton", "right", false));
    CHECK(requests.empty());
    REQUIRE(window.click(host, "TASK_PreviewRewardButton", "left", false));
    REQUIRE(requests.size() == 1);
    CHECK(requests.front().opcode == net::to_wire(net::Op::RewardSelection));
    ByteReader reader(requests.front().data);
    CHECK(reader.le<std::uint32_t>() == 4u);
    CHECK(reader.le<std::uint32_t>() == zebra.task_id); // neither sequence 0 nor the sorted list's row
    CHECK(reader.empty());

    // Reuse the selected slot before the journal has processed its rebuild. The new task is itself eligible,
    // so only the row's captured identity prevents an unintended query for the replacement.
    zebra.task_id = 80000;
    zebra.title = "Replacement";
    rig.server.zone.send_reliable(net::encode_task_description(zebra));
    REQUIRE(rig.until([&] { return rig.conn.tasks().find_id(zebra.task_id) != nullptr; }));
    clock.advance(std::chrono::milliseconds{501});
    button = {};
    window.child(host, "TASK_PreviewRewardButton", button);
    CHECK(button.enabled == false);
    REQUIRE(window.click(host, "TASK_PreviewRewardButton", "left", false));
    CHECK(requests.size() == 1);

    window.frame(host); // selects the first current task after the old identity disappeared
    alpha.reward_set = false;
    rig.server.zone.send_reliable(net::encode_task_description(alpha));
    REQUIRE(rig.until([&] { const auto* task = rig.conn.tasks().find_id(alpha.task_id); return task && !task->reward_set; }));
    clock.advance(std::chrono::milliseconds{501});
    button = {};
    window.child(host, "TASK_PreviewRewardButton", button);
    CHECK(button.enabled == false);
    REQUIRE(window.click(host, "TASK_PreviewRewardButton", "left", false));
    app::Child monster;
    window.child(host, "TASK_MonsterButton", monster);
    CHECK(monster.enabled == false);
    REQUIRE(window.click(host, "TASK_MonsterButton", "left", false));
    window.frame(host);
    clock.advance(std::chrono::milliseconds{501});
    window.frame(host);
    CHECK(requests.size() == 1);
}
