// The original's vectors (recorded from eqgame.exe): the spell book,
// group, quantity dialog, trade and drop rules in src/game, checked call for
// call against the original's.
#include <doctest/doctest.h>

#include <format>
#include <string>
#include <vector>

#include "game/group_rules.h"
#include "game/quantity_dialog.h"
#include "game/trade_rules.h"
#include "game/cursor_drop.h"
#include "game/merchant.h"
#include "game/spell_book.h"
#include "vector_files.h"

using namespace hailstone::game;
using hailstone::test::Json;

namespace {

// ---- the spell book

std::vector<std::string> book_calls(const std::vector<SpellBookCall>& calls, bool spellcaster_asked) {
    using K = SpellBookCall::Kind;
    std::vector<std::string> out;
    if (spellcaster_asked) out.push_back("is_spellcaster");
    for (const auto& c : calls) {
        switch (c.kind) {
        case K::Chat:
            out.push_back(std::format("get_string {}", c.value));
            out.push_back(std::format("dsp_chat {} {}", c.value, c.color));
            break;
        case K::Chat2:
            out.push_back(std::format("get_string {}", c.value));
            out.push_back(std::format("dsp_chat_2 {}", c.value));
            break;
        case K::CastWindowHidden: out.push_back("cast_window_hidden"); break;
        case K::CastWindowShown: out.push_back("cast_window_shown"); break;
        case K::ChangePosition: out.push_back(std::format("change_position {}", c.value)); break;
        case K::SetAutoAttack: out.push_back("set_auto_attack 0"); break;
        case K::SetAutoFire: out.push_back("set_auto_range_attack 0"); break;
        case K::ResetBookState: out.push_back("reset_book_state"); break;
        case K::AddGlobalFocus: out.push_back("add_global_focus"); break;
        case K::SetFocus: out.push_back("set_focus"); break;
        case K::TipWindowActivate: out.push_back("tip_window_activate 1 0"); break;
        case K::SidlOnShow: out.push_back("sidl_on_show"); break;
        }
    }
    return out;
}

std::vector<std::string> recorded_book_calls(const Json& calls) {
    std::vector<std::string> out;
    for (const Json& c : calls.items) {
        const std::string& fn = c["fn"].text;
        if (fn == "get_string") out.push_back(std::format("get_string {}", c["id"].i32()));
        else if (fn == "dsp_chat") out.push_back(std::format("dsp_chat {} {}", c["text"].i32(), c["color"].i32()));
        else if (fn == "dsp_chat_2") out.push_back(std::format("dsp_chat_2 {}", c["text"].i32()));
        else if (fn == "change_position") out.push_back(std::format("change_position {}", c["state"].i32()));
        else if (fn == "set_auto_attack" || fn == "set_auto_range_attack") out.push_back(std::format("{} {}", fn, c["on"].i32()));
        else if (fn == "tip_window_activate") out.push_back(std::format("{} {} {}", fn, c["a"].i32(), c["b"].i32()));
        else out.push_back(fn);
    }
    return out;
}

std::string diff(const std::vector<std::string>& got, const std::vector<std::string>& want) {
    if (got == want) return {};
    std::string g, w;
    for (auto& s : got) g += s + "; ";
    for (auto& s : want) w += s + "; ";
    return "calls: got [" + g + "] want [" + w + "]";
}

SpellBookState book_state(const Json& in) {
    SpellBookState s;
    s.stand_state = static_cast<std::uint8_t>(in["stand_state"].i32());
    s.was_sitting = in["was_sitting"].i32() != 0;
    s.auto_attack = in["auto_attack"].i32() != 0;
    s.auto_fire = in["auto_fire"].i32() != 0;
    s.cast_window = in["cast_window"].truthy();
    s.tip_window = in["tip_window"].truthy();
    return s;
}

std::string check_about_to_show(const Json& rec) {
    const Json& in = rec["in"];
    const Json& out = rec["out"];
    SpellBookState s = book_state(in);
    s.profile = in["profile"].truthy();
    s.profile_340c = in["profile_340c"].i32();
    if (!in["controlled_speed"].is_null()) s.controlled_speed = hailstone::test::f32_of(in["controlled_speed"]);
    // IsSpellcaster's answer is the record's first call.
    s.spellcaster = out["calls"][0]["ret"].i32() != 0;
    std::vector<SpellBookCall> calls;
    bool r = spell_book_about_to_show(s, calls);
    if (auto d = diff(book_calls(calls, true), recorded_book_calls(out["calls"])); !d.empty()) return d;
    if (r != out["value"].truthy()) return "value";
    if (s.was_sitting != (out["was_sitting"].i32() != 0)) return "was_sitting";
    return {};
}

std::string check_on_show(const Json& rec) {
    const Json& in = rec["in"];
    SpellBookState s = book_state(in);
    std::vector<SpellBookCall> calls;
    spell_book_on_show(s, in["shown"].truthy(), calls);
    return diff(book_calls(calls, false), recorded_book_calls(rec["out"]["calls"]));
}

} // namespace

TEST_CASE("vectors: CSpellBookWnd_AboutToShow (opening the book sits you down)") {
    hailstone::test::each_vector("CSpellBookWnd_AboutToShow.jsonl", check_about_to_show);
}

TEST_CASE("vectors: CSpellBookWnd_OnShow (closing it stands you up if it sat you)") {
    hailstone::test::each_vector("CSpellBookWnd_OnShow.jsonl", check_on_show);
}

namespace {

// ---- the group

class LoggedGroup final : public GroupCalls {
public:
    std::vector<std::string> calls, swaps;
    void attach_spawn(int m) override { calls.push_back(std::format("attach_spawn {}", m)); }
    void message(std::uint32_t code, int m) override { calls.push_back(std::format("message {} {} -1", code, m)); }
    void set_leader(int m) override { calls.push_back(std::format("set_leader {}", m)); }
    void group_vf18(int m) override { calls.push_back(std::format("group_vf18 {}", m)); }
    void group_vf1c(int m) override { calls.push_back(std::format("group_vf1c {}", m)); }
    void member_vf18(int m) override { calls.push_back(std::format("member_vf18 {}", m)); }
    void swap_members(int m, int o) override { swaps.push_back(std::format("{} {}", m, o)); }
};

GroupSlotsModel group_of(const Json& j) {
    GroupSlotsModel g;
    for (std::size_t k = 0; k < 6; ++k) g.slots[k] = j["slots"][k].i32();
    g.leader = j["leader"].i32();
    return g;
}

std::string check_group(const Json& rec, int which) {
    const Json& in = rec["in"];
    const Json& out = rec["out"];
    GroupSlotsModel g = group_of(in["group"]);
    LoggedGroup c;
    int m = in["member"].i32();
    bool r = false;
    if (which == 0)
        r = group_add_member(g, m, static_cast<std::int16_t>(in["member_kind"].i32()), in["name_in_group"].i32() != -1,
                             in["owner_in_group"].i32(), c);
    else if (which == 1) r = group_remove_member(g, m, c);
    else r = group_set_leader(g, m, c);
    std::vector<std::string> want, want_swaps;
    for (const Json& x : out["calls"].items) {
        const std::string& fn = x["fn"].text;
        if (fn == "message") want.push_back(std::format("message {} {} {}", x["code"].i32(), x["member"].i32(), x["member2"].i32()));
        else want.push_back(std::format("{} {}", fn, x["member"].i32()));
    }
    for (const Json& x : out["swaps"].items) want_swaps.push_back(std::format("{} {}", x["member"].i32(), x["owner"].i32()));
    if (auto d = diff(c.calls, want); !d.empty()) return d;
    if (auto d = diff(c.swaps, want_swaps); !d.empty()) return "swaps " + d;
    if (r != out["value"].truthy()) return "value";
    GroupSlotsModel w = group_of(out["group"]);
    if (g.slots != w.slots || g.leader != w.leader) return "group";
    return {};
}

class LoggedWorld final : public GroupWorldCalls {
public:
    explicit LoggedWorld(const Json& in) : in_(in) {}
    std::vector<std::string> calls, group_calls;
    int new_merc = 0, new_field = 0;
    bool remove_answer = false;
    bool created = false;
    void say(std::uint32_t id) override {
        calls.push_back(std::format("build_string {}", id));
        calls.push_back(std::format("dsp_chat {}", kGroupChatColor));
    }
    void delete_group() override { group_calls.push_back("delete_group 1"); }
    void armor_filters_after_leaving() override { calls.push_back("armor_filters_after_leaving (0x597750)"); }
    int member_by_name(const std::string& name) override {
        int k = lookup(name);
        calls.push_back(std::format("get_member_by_name {}", k != -1));
        return k;
    }
    int new_member(const GroupWorldPacket& p) override {
        calls.push_back("new_member 48");
        calls.push_back("get_spawn_by_name");
        created = true;
        new_merc = p.merc;
        new_field = p.field_84;
        return 100;
    }
    bool add_member(int m) override {
        bool r = in_["add_member_result"].truthy();
        group_calls.push_back(std::format("add_member {} {}", m, r ? 1 : 0));
        return r;
    }
    void apply_armor_filter(const std::string&) override { calls.push_back("apply_armor_filter"); }
    void set_leader(int m) override { group_calls.push_back(std::format("set_leader {}", m)); }
    void pc_delete_group() override { calls.push_back("delete_group (PcClient_DeleteGroup)"); }
    bool remove_member_by_name(const std::string& name) override {
        (void)name;
        bool r = remove_answer; // the mocked callee's answer, from the record
        calls.push_back(std::format("remove_member_by_name {}", r));
        return r;
    }
    void rename(int, const std::string&) override {}
    std::optional<std::string> leader_name() override {
        if (in_["leader_name"].is_null()) return std::nullopt;
        return in_["leader_name"].text;
    }
    void set_member_role(int, std::int32_t role, std::uint8_t on) override {
        calls.push_back(std::format("set_member_role {} {}", role, on));
    }

private:
    int lookup(const std::string& name) const {
        static const char* names[] = {"Alice", "Bob", "alice", ""};
        for (std::size_t k = 0; k < 4; ++k)
            if (name == names[k]) return in_["members_by_name"][k].i32();
        return -1;
    }
    const Json& in_;
};

std::string check_group_world(const Json& rec) {
    const Json& in = rec["in"];
    const Json& out = rec["out"];
    GroupWorldPacket p;
    p.opcode = in["opcode"].u32();
    p.name = in["name"].text;
    p.name2 = in["name2"].text;
    p.merc = static_cast<std::int8_t>(in["merc"].i32());
    p.field_84 = in["field_84"].i32();
    p.role = in["role"].i32();
    p.role_on = static_cast<std::uint8_t>(in["role_on"].i32());
    bool grouped = in["grouped"].truthy();
    LoggedWorld c(in);
    for (const Json& x : out["calls"].items)
        if (x["fn"].text == "remove_member_by_name") c.remove_answer = x["ret"].truthy();
    handle_group_world_message(grouped, p, c);
    std::vector<std::string> want, want_group;
    for (const Json& x : out["calls"].items) {
        const std::string& fn = x["fn"].text;
        if (fn == "build_string") want.push_back(std::format("build_string {}", x["id"].i32()));
        else if (fn == "dsp_chat") want.push_back(std::format("dsp_chat {}", x["color"].i32()));
        else if (fn == "get_member_by_name") want.push_back(std::format("get_member_by_name {}", x["ret_nonnull"].truthy()));
        else if (fn == "new_member") want.push_back(std::format("new_member {}", x["size"].i32()));
        else if (fn == "remove_member_by_name") want.push_back(std::format("remove_member_by_name {}", x["ret"].truthy()));
        else if (fn == "set_member_role") want.push_back(std::format("set_member_role {} {}", x["role"].i32(), x["on"].i32()));
        else want.push_back(fn);
    }
    for (const Json& x : out["group_calls"].items) {
        const std::string& fn = x["fn"].text;
        if (fn == "delete_group") want_group.push_back(std::format("delete_group {}", x["flags"].i32()));
        else if (fn == "add_member") want_group.push_back(std::format("add_member {} {}", x["member"].i32(), x["ret"].i32()));
        else want_group.push_back(std::format("{} {}", fn, x["member"].i32()));
    }
    if (auto d = diff(c.calls, want); !d.empty()) return d;
    if (auto d = diff(c.group_calls, want_group); !d.empty()) return "group " + d;
    if (grouped != out["grouped"].truthy()) return "grouped";
    if (out["new_member"].is_null() == c.created) return "new member";
    if (c.created && (c.new_merc != out["new_member"]["merc"].i32() || c.new_field != out["new_member"]["field_10"].i32()))
        return "new member fields";
    return {};
}

} // namespace

TEST_CASE("vectors: CGroupBase AddMember / RemoveMember / SetLeader") {
    hailstone::test::each_vector("CGroupBase_AddMember.jsonl", [](const Json& r) { return check_group(r, 0); });
    hailstone::test::each_vector("CGroupBase_RemoveMember.jsonl", [](const Json& r) { return check_group(r, 1); });
    hailstone::test::each_vector("CGroupBase_SetLeader.jsonl", [](const Json& r) { return check_group(r, 2); });
}

TEST_CASE("vectors: HandleWorldMessage_Group") {
    hailstone::test::each_vector("HandleWorldMessage_Group.jsonl", check_group_world);
}

namespace {

std::string check_target(const Json& rec, bool self_or_pet, bool holder = true) {
    const Json& in = rec["in"];
    GroupTargetWorld w;
    for (std::size_t k = 0; k < 6; ++k)
        if (!in["slots"][k].is_null()) w.members[k] = in["slots"][k]["spawn"].i32();
    w.selected = in["selected_member"].i32();
    w.target = in["target"].i32();
    w.player = 100;
    // The recorder's target fields describe the owner's PetID at +0x2b4.
    w.pet_id_of = [&](int spawn) { return in["spawn_targets"][static_cast<std::size_t>(spawn)].i32(); };
    w.spawn_by_id = [](std::int32_t id) { return id >= 100 && id < 106 ? id - 100 : -1; };
    std::int32_t n = in["n"].i32();
    bool target_pet = in["target_of_target"].truthy();
    if (self_or_pet) target_self_or_pet(w, holder && in["grouped"].truthy(), n, target_pet, in["own_spawn"].i32());
    else group_target_member(w, n, target_pet);
    const Json& out = rec["out"];
    if (w.selected != out["selected_member"].i32() || w.target != out["target"].i32())
        return std::format("selected {} target {}; want {} {}", w.selected, w.target, out["selected_member"].i32(),
                           out["target"].i32());
    return {};
}

} // namespace

TEST_CASE("vectors: CGroup_TargetMember and LocalPlayer_TargetSelfOrPet (F1..F6, the group window)") {
    hailstone::test::each_vector("CGroup_TargetMember.jsonl", [](const Json& r) { return check_target(r, false); });
    // The recorder leaves out whether the player's +0x2cc (the character holder) is set (20% of the cases it is
    // null, and then the ungrouped path runs although "grouped" says the character has a group), so a grouped
    // record passes when either path gives the recorded result; ungrouped records are checked exactly.
    hailstone::test::each_vector("LocalPlayer_TargetSelfOrPet.jsonl", [](const Json& r) {
        std::string why = check_target(r, true);
        if (!why.empty() && r["in"]["grouped"].truthy() && check_target(r, true, false).empty()) return std::string();
        return why;
    });
}

namespace {

std::string check_quantity(const Json& rec) {
    const Json& in = rec["in"];
    QuantityOpen q;
    q.min = in["min"].i32();
    q.max = in["max"].i32();
    q.start = in["start"].i32();
    q.x = in["x"].i32();
    q.y = in["y"].i32();
    q.use_keys = in["use_keys"].truthy();
    q.shift = in["shift"].truthy();
    q.ctrl = in["ctrl"].truthy();
    q.has_title = in["title"].truthy();
    q.screen_width = in["screen_width"].i32();
    q.screen_height = in["screen_height"].i32();
    for (std::size_t k = 0; k < 4; ++k) q.rect[k] = in["rect"][k].i32();
    QuantityResult r = quantity_dialog_open(q, in["quantity_before"].i32());
    using K = QuantityCall::Kind;
    std::vector<std::string> got, want;
    for (const auto& c : r.calls) {
        switch (c.kind) {
        case K::Close: got.push_back("close"); break;
        case K::SliderTicks: got.push_back(std::format("slider_ticks {}", c.a)); break;
        case K::SliderValue: got.push_back(std::format("slider_value {}", c.a)); break;
        case K::SetTitle: got.push_back(std::format("set_title {}", c.a)); break;
        case K::SliderGetValue: got.push_back("slider_get_value"); break;
        case K::OwnerNotified: got.push_back(std::format("owner_notified {} {}", c.a, c.b)); break;
        case K::UpdateText: got.push_back("update_text"); break;
        case K::Move: got.push_back(std::format("move {} {}", c.a, c.b)); break;
        case K::Center: got.push_back("center"); break;
        case K::FocusEdit: got.push_back("focus_edit"); break;
        case K::Show: got.push_back("show"); break;
        }
    }
    for (const Json& c : rec["out"]["calls"].items) {
        const std::string& fn = c["fn"].text;
        if (fn == "slider_ticks") want.push_back(std::format("slider_ticks {}", c["ticks"].i32()));
        else if (fn == "slider_value") want.push_back(std::format("slider_value {}", c["value"].i32()));
        else if (fn == "set_title") want.push_back(std::format("set_title {}", c["default"].i32()));
        else if (fn == "owner_notified") want.push_back(std::format("owner_notified {} {}", c["message"].i32(), c["quantity"].i32()));
        else if (fn == "move") want.push_back(std::format("move {} {}", c["x"].i32(), c["y"].i32()));
        else want.push_back(fn);
    }
    if (auto d = diff(got, want); !d.empty()) return d;
    if (r.quantity != rec["out"]["quantity"].i32()) return std::format("quantity {} want {}", r.quantity, rec["out"]["quantity"].i32());
    return {};
}

} // namespace

TEST_CASE("vectors: CQuantityWnd_Open (the quantity dialog)") {
    hailstone::test::each_vector("CQuantityWnd_Open.jsonl", check_quantity);
}

TEST_CASE("quantity dialog: the merchant's limits") {
    CHECK(merchant_buy_limits(-1, 20, false).max == 20);
    CHECK(merchant_buy_limits(5, 20, false).start == 5);
    CHECK(merchant_buy_limits(5, 20, true).start == 1);
    CHECK(merchant_sell_limits(7).start == 7);
}

namespace {

TradeFlags flags_of(const Json& j) {
    return {static_cast<std::uint8_t>(j["his_ready"].i32()), static_cast<std::uint8_t>(j["my_ready"].i32())};
}

std::string check_trade(const Json& rec, int which) {
    const Json& in = rec["in"];
    const Json& out = rec["out"];
    TradeFlags f = flags_of(in["flags"]);
    std::vector<std::string> got, want;
    std::array<std::int32_t, 4> cursor{};
    for (std::size_t k = 0; k < 4 && which >= 3; ++k) cursor[k] = in["cursor_coins"][k].i32();
    TradeMoneyMove mv;
    switch (which) {
    case 0:
        trade_set_his_ready(f, static_cast<std::uint8_t>(in["ready"].i32()));
        got.push_back("update_trade_display");
        break;
    case 1:
        trade_set_his_money(f);
        if (in["amount"].i32() != out["his_money"].i32()) return "his_money";
        got.push_back("update_trade_display");
        break;
    case 2:
        if (trade_item_moved(f, in["from"]["location"].i32(), in["to"]["location"].i32()))
            got.push_back("update_trade_display");
        break;
    default: {
        auto type = static_cast<std::uint8_t>(in["cursor_coin_type"].i32());
        bool moved = which == 3 ? trade_clicked_money_button(f, type, cursor, in["coin"].u32(), mv)
                                : trade_drop_cursor_money(f, type, cursor, mv);
        if (moved) {
            got.push_back(std::format("move_money 0 3 {} {} {} 0", mv.from_coin, mv.to_coin, mv.amount));
            got.push_back("update_trade_display");
        }
    }
    }
    for (const Json& c : out["calls"].items) {
        if (c["fn"].text == "move_money")
            want.push_back(std::format("move_money {} {} {} {} {} {}", c["from_location"].i32(), c["to_location"].i32(),
                                       c["from_coin"].i32(), c["to_coin"].i32(), c["amount"].i32(), c["flag"].i32()));
        else want.push_back(c["fn"].text);
    }
    if (auto d = diff(got, want); !d.empty()) return d;
    TradeFlags w = flags_of(out["flags"]);
    if (f.his_ready != w.his_ready || f.my_ready != w.my_ready) return "flags";
    return {};
}

} // namespace

TEST_CASE("vectors: CTradeWnd ready flags and coin") {
    hailstone::test::each_vector("CTradeWnd_SetHisReadyTrade.jsonl", [](const Json& r) { return check_trade(r, 0); });
    hailstone::test::each_vector("CTradeWnd_SetHisMoney.jsonl", [](const Json& r) { return check_trade(r, 1); });
    hailstone::test::each_vector("CTradeWnd_OnTradeItemMoved.jsonl", [](const Json& r) { return check_trade(r, 2); });
    hailstone::test::each_vector("CTradeWnd_ClickedMoneyButton.jsonl", [](const Json& r) { return check_trade(r, 3); });
    hailstone::test::each_vector("CTradeWnd_DropCursorMoneyIntoTrade.jsonl", [](const Json& r) { return check_trade(r, 4); });
}

namespace {

std::string check_drop(const Json& rec) {
    const Json& in = rec["in"];
    const Json& out = rec["out"];
    DropInput d;
    d.announce = in["announce"].truthy();
    d.cursor_item = in["cursor_item"].truthy();
    d.quiet_net_log = in["quiet_net_log"].truthy();
    d.skip_packet = in["skip_packet"].truthy();
    d.connected = in["connected"].truthy();
    for (const Json& c : out["calls"].items)
        if (c["fn"].text == "send") d.send_ok = c["ret"].truthy();
    std::vector<std::string> got, want;
    for (const auto& s : drop_held_item_steps(d)) {
        switch (s.kind) {
        case DropStep::Kind::Chat: got.push_back(std::format("chat {} 15", s.value)); break;
        case DropStep::Kind::RemoveCursorItem: got.push_back(std::format("remove_cursor_item {}", s.value)); break;
        case DropStep::Kind::Send:
            got.push_back(std::format("hton {}", s.value));
            got.push_back("send 4 2"); // channel 4, the opcode alone
            break;
        case DropStep::Kind::Log: got.push_back(std::format("log {}", s.value)); break;
        case DropStep::Kind::CloseConnection: got.push_back("close_connection"); break;
        }
    }
    for (const Json& c : out["calls"].items) {
        const std::string& fn = c["fn"].text;
        if (fn == "chat") want.push_back(std::format("chat {} {}", c["id"].i32(), c["color"].i32()));
        else if (fn == "remove_cursor_item") want.push_back(std::format("remove_cursor_item {}", c["slot"].i32()));
        else if (fn == "hton" || fn == "log") want.push_back(std::format("{} {}", fn, c["opcode"].i32()));
        else if (fn == "send") want.push_back(std::format("send {} {}", c["channel"].i32(), c["length"].i32()));
        else want.push_back(fn);
    }
    if (auto e = diff(got, want); !e.empty()) return e;
    // CDisplay +0x10 cleared and PcClient +0x2dd4 set exactly when there was an item.
    int display = d.cursor_item ? 0 : in["display_10"].i32();
    int pc = d.cursor_item ? 1 : in["pc_2dd4"].i32();
    if (display != out["display_10"].i32() || pc != out["pc_2dd4"].i32()) return "display/pc fields";
    return {};
}

} // namespace

TEST_CASE("vectors: CEverQuest_DropHeldItemOnGround") {
    hailstone::test::each_vector("CEverQuest_DropHeldItemOnGround.jsonl", check_drop);
    CHECK(ground_drop_decision(true, 1) == DropDecision::Refused);
    CHECK(ground_drop_decision(false, 1) == DropDecision::DropNow);
    CHECK(ground_drop_decision(false, -1) == DropDecision::Disabled);
    CHECK(ground_drop_decision(false, 0) == DropDecision::Confirm);
    CHECK(ground_drop_decision(false, 7) == DropDecision::Confirm);
}

TEST_CASE("ItemClient::CanDrop(1, 1, 0, 1): LMouseUp's check before a ground drop") {
    hailstone::net::Item plain;
    plain.no_drop = 255; // tradeable
    plain.name = "Rusty Dagger";
    CHECK(can_drop(plain, false, true, true) == CanDropResult::Droppable);
    hailstone::net::Item no_trade = plain;
    no_trade.no_drop = 0;
    CHECK(can_drop(no_trade, false, true, true) == CanDropResult::NoTrade);
    CHECK(can_drop_message(CanDropResult::NoTrade) == 12921);
    // A real estate item: only with no_real_estate.
    hailstone::net::Item estate = plain;
    estate.unknowna4 = 12;
    CHECK(can_drop(estate, false, true, true) == CanDropResult::NoTrade);
    CHECK(can_drop(estate, false, true, false) == CanDropResult::Droppable);
    // A bag holding a no trade item: 12922; any other item (an augment) 5293; without contents only the item counts.
    hailstone::net::Item bag = plain;
    bag.item_class = 1;
    bag.contents.push_back({0, no_trade});
    CHECK(can_drop(bag, false, true, true) == CanDropResult::BagContentNoTrade);
    CHECK(can_drop_message(CanDropResult::BagContentNoTrade) == 12922);
    CHECK(can_drop(bag, false, false, true) == CanDropResult::Droppable);
    hailstone::net::Item augmented = plain;
    augmented.contents.push_back({0, no_trade});
    CHECK(can_drop(augmented, false, true, true) == CanDropResult::AugmentNoTrade);
    CHECK(can_drop_message(CanDropResult::AugmentNoTrade) == 5293);
    // The FV rules: the FV no-drop byte decides, NoDrop does not.
    CHECK(can_drop(no_trade, true, true, true) == CanDropResult::Droppable);
    hailstone::net::Item fv = plain;
    fv.fv_no_drop = 1;
    CHECK(can_drop(fv, true, true, true) == CanDropResult::NoTrade);
    hailstone::net::Item fv_bag = bag;
    fv_bag.contents[0].item = fv;
    CHECK(can_drop(fv_bag, true, true, true) == CanDropResult::BagContentNoTrade);
    CHECK(can_drop(estate, true, true, true) == CanDropResult::NoTrade);
}
