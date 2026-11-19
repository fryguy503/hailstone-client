#include "net/task_packets.h"

#include "net/packet_io.h"

namespace hailstone::net {

namespace {

std::string counted(ByteReader& r) {
    auto n = r.le<std::uint32_t>();
    auto b = r.bytes(n);
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

void write_counted(ByteWriter& w, const std::string& s) {
    w.le(static_cast<std::uint32_t>(s.size()));
    w.bytes({reinterpret_cast<const std::byte*>(s.data()), s.size()});
}

// The objective block shared by OP_TaskActivity and the select window, from the target name to the zones
// (SerializeObjective / SerializeSelector's RoF branch).
void read_objective_middle(ByteReader& r, game::TaskElement& e) {
    e.target_name = r.cstring();
    e.item_list = counted(r);
    e.goal_count = r.le<std::int32_t>();
    e.skill_list = counted(r);
    e.spell_list = counted(r);
    e.zones = r.cstring();
}

void write_objective_middle(ByteWriter& w, const game::TaskElement& e) {
    detail::write_cstring(w, e.target_name);
    write_counted(w, e.item_list);
    w.le(e.goal_count);
    write_counted(w, e.skill_list);
    write_counted(w, e.spell_list);
    detail::write_cstring(w, e.zones);
}

} // namespace

std::optional<game::Task> decode_task_description(const AppPacket& p) {
    return detail::decode_with(p, [](ByteReader& r) {
        game::Task t;
        t.sequence = r.le<std::uint32_t>();
        t.task_id = r.le<std::uint32_t>();
        t.open_window = r.le<std::uint8_t>() != 0;
        t.type = r.le<std::uint32_t>();
        t.reward_type = r.le<std::uint32_t>();
        t.title = r.cstring();
        t.duration = r.le<std::uint32_t>();
        t.duration_code = r.le<std::uint32_t>();
        t.elapsed = r.le<std::uint32_t>();
        t.description = r.cstring();
        t.has_rewards = r.le<std::uint8_t>() != 0;
        t.coin_reward = r.le<std::uint32_t>();
        t.xp_reward = r.le<std::uint32_t>() != 0;
        t.faction_reward = r.le<std::uint32_t>() != 0;
        t.reward = r.cstring();
        t.reward_link = r.cstring();
        t.points = r.le<std::uint32_t>();
        // The trailer's byte (+0x3bf9 bHasRewardSet). (Without has_rewards the original reads it where the coin
        // starts; EQEmu always sends the reward block.)
        if (r.remaining() >= 1) t.reward_set = r.le<std::uint8_t>() != 0;
        if (!t.has_rewards) t.reward_set = (t.coin_reward & 0xff) != 0;
        return t;
    });
}

AppPacket encode_task_description(const game::Task& t) {
    ByteWriter w;
    w.le(t.sequence);
    w.le(t.task_id);
    w.le(std::uint8_t{t.open_window ? std::uint8_t{1} : std::uint8_t{0}});
    w.le(t.type);
    w.le(t.reward_type);
    detail::write_cstring(w, t.title);
    w.le(t.duration);
    w.le(t.duration_code);
    w.le(t.elapsed);
    detail::write_cstring(w, t.description);
    w.le(std::uint8_t{t.has_rewards ? std::uint8_t{1} : std::uint8_t{0}});
    w.le(t.coin_reward);
    w.le(std::uint32_t{t.xp_reward ? 1u : 0u});
    w.le(std::uint32_t{t.faction_reward ? 1u : 0u});
    detail::write_cstring(w, t.reward);
    detail::write_cstring(w, t.reward_link);
    w.le(t.points);
    w.le(std::uint8_t{t.reward_set ? std::uint8_t{1} : std::uint8_t{0}});
    return make_packet(Op::TaskDescription, std::move(w));
}

std::optional<TaskActivity> decode_task_activity(const AppPacket& p) {
    return detail::decode_with(p, [&](ByteReader& r) {
        TaskActivity v;
        v.sequence = r.le<std::uint32_t>();
        v.type = r.le<std::uint32_t>();
        v.task_id = r.le<std::uint32_t>();
        game::TaskElement& e = v.element;
        e.activity_id = r.le<std::uint32_t>();
        e.step = r.le<std::uint32_t>();
        auto kind = r.le<std::uint32_t>();
        if (kind == 0xffffffffu && p.data.size() == 25) { // SendTaskActivityShort: not unlocked yet
            e.hidden = true;
            e.type = -1;
            e.optional = r.le<std::uint8_t>() != 0;
            return v;
        }
        e.hidden = false;
        e.type = static_cast<std::int32_t>(kind);
        e.optional = r.le<std::uint8_t>() != 0;
        r.le<std::int32_t>(); // request type (0)
        read_objective_middle(r, e);
        e.dz_switch = r.le<std::int32_t>();
        e.description = r.cstring();
        e.done_count = r.le<std::int32_t>();
        r.le<std::uint8_t>(); // 1
        e.target_zones = r.cstring(); // +0x88 TargetZoneID
        return v;
    });
}

AppPacket encode(const TaskActivity& v) {
    ByteWriter w;
    w.le(v.sequence);
    w.le(v.type);
    w.le(v.task_id);
    const game::TaskElement& e = v.element;
    w.le(e.activity_id);
    w.le(e.step);
    if (e.hidden) {
        w.le(std::uint32_t{0xffffffffu});
        w.le(std::uint8_t{e.optional ? std::uint8_t{1} : std::uint8_t{0}});
        return make_packet(Op::TaskActivity, std::move(w));
    }
    w.le(e.type);
    w.le(std::uint8_t{e.optional ? std::uint8_t{1} : std::uint8_t{0}});
    w.le(std::int32_t{0});
    write_objective_middle(w, e);
    w.le(e.dz_switch);
    detail::write_cstring(w, e.description);
    w.le(e.done_count);
    w.le(std::uint8_t{1});
    detail::write_cstring(w, e.target_zones);
    return make_packet(Op::TaskActivity, std::move(w));
}

std::optional<std::vector<game::CompletedTask>> decode_completed_tasks(const AppPacket& p) {
    return detail::decode_with(p, [](ByteReader& r) {
        std::vector<game::CompletedTask> v;
        auto n = r.le<std::uint32_t>();
        for (std::uint32_t i = 0; i < n && r.remaining() > 0; ++i) {
            game::CompletedTask c;
            c.task_id = r.le<std::uint32_t>();
            c.title = r.cstring();
            c.completed_time = r.le<std::uint32_t>();
            v.push_back(std::move(c));
        }
        return v;
    });
}

AppPacket encode_completed_tasks(const std::vector<game::CompletedTask>& v) {
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(v.size()));
    for (const auto& c : v) {
        w.le(c.task_id);
        detail::write_cstring(w, c.title);
        w.le(c.completed_time);
    }
    return make_packet(Op::CompletedTasks, std::move(w));
}

std::optional<game::TaskOffers> decode_task_select(const AppPacket& p) {
    return detail::decode_with(p, [&](ByteReader& r) {
        game::TaskOffers v;
        v.shared = is_op(p, Op::SharedTaskSelectWindow);
        auto n = r.le<std::uint32_t>();
        v.type = r.le<std::uint32_t>();
        v.giver_id = r.le<std::uint32_t>();
        for (std::uint32_t i = 0; i < n && i < 40; ++i) { // the window holds 40 offers
            game::TaskOffer o;
            o.task_id = r.le<std::uint32_t>();
            o.reward_multiplier = r.f32();
            o.duration = r.le<std::uint32_t>();
            o.duration_code = r.le<std::uint32_t>();
            o.title = r.cstring();
            o.description = r.cstring();
            o.reward_preview = r.le<std::uint8_t>() != 0;
            auto elements = r.le<std::uint32_t>();
            for (std::uint32_t k = 0; k < elements; ++k) {
                game::TaskElement e;
                e.hidden = false;
                e.activity_id = r.le<std::uint32_t>();
                e.type = r.le<std::int32_t>();
                r.le<std::int32_t>(); // request type (0)
                read_objective_middle(r, e);
                e.description = r.cstring();
                r.cstring(); // the zones again
                o.elements.push_back(std::move(e));
            }
            v.offers.push_back(std::move(o));
        }
        return v;
    });
}

AppPacket encode_task_select(const game::TaskOffers& v, Op op) {
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(v.offers.size()));
    w.le(v.type);
    w.le(v.giver_id);
    for (const auto& o : v.offers) {
        w.le(o.task_id);
        w.f32(o.reward_multiplier);
        w.le(o.duration);
        w.le(o.duration_code);
        detail::write_cstring(w, o.title);
        detail::write_cstring(w, o.description);
        w.le(std::uint8_t{o.reward_preview ? std::uint8_t{1} : std::uint8_t{0}});
        w.le(static_cast<std::uint32_t>(o.elements.size()));
        for (const auto& e : o.elements) {
            w.le(e.activity_id);
            w.le(e.type);
            w.le(std::int32_t{0});
            write_objective_middle(w, e);
            detail::write_cstring(w, e.description);
            detail::write_cstring(w, e.zones);
        }
    }
    return make_packet(op, std::move(w));
}

std::optional<game::TaskHistory> decode_task_history(const AppPacket& p) {
    return detail::decode_with(p, [](ByteReader& r) {
        game::TaskHistory v;
        v.index = r.le<std::uint32_t>();
        auto n = r.le<std::uint32_t>();
        for (std::uint32_t i = 0; i < n; ++i) {
            game::TaskElement e;
            e.hidden = false;
            e.activity_id = i;
            e.type = r.le<std::int32_t>();
            e.target_name = r.cstring();
            e.item_list = counted(r);
            e.goal_count = r.le<std::int32_t>();
            e.skill_list = counted(r);
            e.spell_list = counted(r);
            e.zones = r.cstring();
            e.dz_switch = r.le<std::int32_t>();
            e.description = r.cstring();
            r.le<std::uint8_t>(); // '1'
            r.cstring();          // the zones again
            e.done_count = e.goal_count; // the history lists completed elements
            v.elements.push_back(std::move(e));
        }
        return v;
    });
}

AppPacket encode(const game::TaskHistory& v) {
    ByteWriter w;
    w.le(v.index);
    w.le(static_cast<std::uint32_t>(v.elements.size()));
    for (const auto& e : v.elements) {
        w.le(e.type);
        detail::write_cstring(w, e.target_name);
        write_counted(w, e.item_list);
        w.le(e.goal_count);
        write_counted(w, e.skill_list);
        write_counted(w, e.spell_list);
        detail::write_cstring(w, e.zones);
        w.le(e.dz_switch);
        detail::write_cstring(w, e.description);
        w.le(std::uint8_t{0x31});
        detail::write_cstring(w, e.zones);
    }
    return make_packet(Op::TaskHistoryReply, std::move(w));
}

std::optional<TaskActivityComplete> decode_task_activity_complete(const AppPacket& p) {
    if (p.data.size() < 24) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        TaskActivityComplete v;
        v.index = r.le<std::uint32_t>();
        v.type = r.le<std::uint32_t>();
        v.task_id = r.le<std::uint32_t>();
        v.activity_id = r.le<std::uint32_t>();
        v.completed = r.le<std::uint32_t>();
        v.stage_complete = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const TaskActivityComplete& v) {
    ByteWriter w;
    w.le(v.index);
    w.le(v.type);
    w.le(v.task_id);
    w.le(v.activity_id);
    w.le(v.completed);
    w.le(v.stage_complete);
    return make_packet(Op::TaskActivityComplete, std::move(w));
}

AppPacket encode_cancel_task(std::uint32_t sequence, std::uint32_t type) {
    ByteWriter w;
    w.le(sequence);
    w.le(type);
    return make_packet(Op::CancelTask, std::move(w));
}

std::optional<CancelTask> decode_cancel_task(const AppPacket& p) {
    if (p.data.size() < 8) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        CancelTask v;
        v.sequence = r.le<std::uint32_t>();
        v.type = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode_accept_task(std::uint32_t type, std::uint32_t task_id, std::uint32_t giver_id) {
    ByteWriter w;
    w.le(type);
    w.le(task_id);
    w.le(giver_id);
    return make_packet(Op::AcceptNewTask, std::move(w));
}

AppPacket encode_shared_task_accept(std::uint32_t giver_id, std::uint32_t task_id, float multiplier) {
    ByteWriter w;
    w.le(std::int32_t{0});
    w.le(std::int32_t{0});
    w.le(giver_id);
    w.le(task_id);
    w.f32(multiplier);
    return make_packet(Op::SharedTaskAcceptNew, std::move(w));
}

AppPacket encode_task_history_request(std::uint32_t index) {
    ByteWriter w;
    w.le(index);
    return make_packet(Op::TaskHistoryRequest, std::move(w));
}

std::optional<std::uint32_t> decode_task_request_timer(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) { return r.le<std::uint32_t>(); });
}

std::optional<std::vector<SharedTaskMember>> decode_shared_task_members(const AppPacket& p) {
    return detail::decode_with(p, [](ByteReader& r) {
        r.skip(8);
        std::uint32_t n = r.le<std::uint32_t>();
        std::vector<SharedTaskMember> v;
        for (std::uint32_t i = 0; i < n; ++i) {
            SharedTaskMember m;
            m.name = r.cstring();
            m.template_id = r.le<std::uint32_t>();
            m.leader = r.le<std::uint8_t>() != 0;
            v.push_back(std::move(m));
        }
        return v;
    });
}

std::optional<SharedTaskMemberChange> decode_shared_task_member_change(const AppPacket& p) {
    return detail::decode_with(p, [](ByteReader& r) {
        r.skip(8);
        SharedTaskMemberChange v;
        v.added = r.le<std::uint8_t>() != 0;
        v.name = r.cstring();
        return v;
    });
}

AppPacket encode(const std::vector<SharedTaskMember>& v) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le(static_cast<std::uint32_t>(v.size()));
    for (const auto& m : v) {
        detail::write_cstring(w, m.name);
        w.le(m.template_id);
        w.le<std::uint8_t>(m.leader ? 1 : 0);
    }
    return make_packet(Op::SharedTaskMemberList, std::move(w));
}

AppPacket encode(const SharedTaskMemberChange& v) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le<std::uint8_t>(v.added ? 1 : 0);
    detail::write_cstring(w, v.name);
    return make_packet(Op::SharedTaskMemberChange, std::move(w));
}

namespace {

AppPacket shared_task_name(Op op, std::string_view name) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.fixed_string(name.substr(0, 63), 64);
    return make_packet(op, std::move(w));
}

} // namespace

AppPacket encode_shared_task_add_player(std::string_view name) { return shared_task_name(Op::SharedTaskAddPlayer, name); }
AppPacket encode_shared_task_remove_player(std::string_view name) {
    return shared_task_name(Op::SharedTaskRemovePlayer, name);
}
AppPacket encode_shared_task_make_leader(std::string_view name) { return shared_task_name(Op::SharedTaskMakeLeader, name); }

AppPacket encode_shared_task_player_list() {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    return make_packet(Op::SharedTaskPlayerList, std::move(w));
}

AppPacket encode_shared_task_quit() {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    return make_packet(Op::SharedTaskQuit, std::move(w));
}

} // namespace hailstone::net
