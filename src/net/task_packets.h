#pragma once

// Task packets. Layouts from EQEmu's task serialisers for RoF2
// (zone/task_manager.cpp SendActiveTaskDescription, SendTaskActivityLong /
// Short, SendCompletedTasksToClient, SendTaskSelector; common/tasks.h
// SerializeSelector / SerializeObjective with the RoF branch; the RoF2
// translator's ENCODE(OP_TaskDescription) and ENCODE(OP_TaskHistoryReply);
// zone/tasks.cpp SendTaskActivityComplete) and the original's CTaskManager::HandleMessage
// @0x482080. Strings are NUL-terminated unless noted "counted" (a u32 length,
// no terminator: SerializeBuffer::WriteLengthString). docs/protocol/rof2-packets.md "Tasks".

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "game/tasks.h"
#include "net/opcodes.h"
#include "net/packet.h"

namespace hailstone::net {

// OP_TaskDescription: u32 sequence, u32 task id, u8 open window, u32 type, u32 reward type; title;
// u32 duration, u32 duration code, u32 elapsed seconds; description; u8 has rewards, u32 coin,
// u32 xp (bool), u32 faction (bool); reward text; reward item link; u32 points; u8 reward selection.
std::optional<game::Task> decode_task_description(const AppPacket& p);
AppPacket encode_task_description(const game::Task& t); // server direction, for tests

// OP_TaskActivity: u32 sequence, u32 type, u32 task id, u32 activity id, u32 step; then either the
// short form (25 bytes: u32 0xffffffff, u8 optional: a hidden element) or the objective: i32 type,
// u8 optional, i32 0, target name, counted item list, i32 goal, counted skill list, counted spell
// list, zones, i32 dz switch, description, i32 done count, u8 1, zones again.
struct TaskActivity {
    std::uint32_t sequence = 0, type = 0, task_id = 0;
    game::TaskElement element;
};
std::optional<TaskActivity> decode_task_activity(const AppPacket& p);
AppPacket encode(const TaskActivity& v); // server direction, for tests

// OP_CompletedTasks: u32 count, then {u32 task id, title, u32 completed time}.
std::optional<std::vector<game::CompletedTask>> decode_completed_tasks(const AppPacket& p);
AppPacket encode_completed_tasks(const std::vector<game::CompletedTask>& v); // tests

// OP_TaskSelectWindow / OP_SharedTaskSelectWindow: u32 count, u32 type, u32 giver; per task: u32 id,
// f32 reward multiplier, u32 duration, u32 duration code, title, description, u8 reward preview,
// u32 element count; per element: i32 index, i32 type, i32 0, target name, counted items, i32 goal,
// counted skills, counted spells, zones, description, zones again.
std::optional<game::TaskOffers> decode_task_select(const AppPacket& p);
AppPacket encode_task_select(const game::TaskOffers& v, Op op); // tests

// OP_TaskHistoryReply (RoF2 encoding): u32 index, u32 count; per element: u32 type, target name,
// counted items, u32 goal, counted skills ("-1"), counted spells ("-1"), zones, u32 0, description,
// u8 ('1'), zones.
std::optional<game::TaskHistory> decode_task_history(const AppPacket& p);
AppPacket encode(const game::TaskHistory& v); // tests

// OP_TaskActivityComplete (Z→C, 24 bytes): index, type, task id, activity id, completed (0: failed),
// stage completed.
struct TaskActivityComplete {
    std::uint32_t index = 0, type = 0, task_id = 0, activity_id = 0, completed = 0, stage_complete = 0;
};
std::optional<TaskActivityComplete> decode_task_activity_complete(const AppPacket& p);
AppPacket encode(const TaskActivityComplete& v); // tests

// OP_CancelTask (both ways, 8 bytes): sequence, type.
AppPacket encode_cancel_task(std::uint32_t sequence, std::uint32_t type);
struct CancelTask {
    std::uint32_t sequence = 0, type = 0;
};
std::optional<CancelTask> decode_cancel_task(const AppPacket& p);

// OP_AcceptNewTask (C→Z, 12 bytes): the selection's type, task id (0 declines), giver spawn id.
AppPacket encode_accept_task(std::uint32_t type, std::uint32_t task_id, std::uint32_t giver_id);
// OP_SharedTaskAcceptNew (C→Z, 20 bytes, SharedTaskAccept_Struct): 0, 0, giver, task id, reward multiplier.
AppPacket encode_shared_task_accept(std::uint32_t giver_id, std::uint32_t task_id, float multiplier);
// OP_TaskHistoryRequest (C→Z, 4 bytes): the index in the completed list.
AppPacket encode_task_history_request(std::uint32_t index);
// OP_SharedTaskMemberList (Z→C, EQEmu shared_task_zone_messaging.cpp): u32, u32, u32 count, count × {name, u32
// mission class, u8 leader}; OP_SharedTaskMemberChange: u32, u32, u8 added (0 removed), name.
struct SharedTaskMember {
    std::string name;
    bool leader = false;
    std::uint32_t template_id = 0;
};
std::optional<std::vector<SharedTaskMember>> decode_shared_task_members(const AppPacket& p);
struct SharedTaskMemberChange {
    bool added = false;
    std::string name;
};
std::optional<SharedTaskMemberChange> decode_shared_task_member_change(const AppPacket& p);
AppPacket encode(const std::vector<SharedTaskMember>& v);  // server direction, for tests
AppPacket encode(const SharedTaskMemberChange& v);         // server direction, for tests

// The shared task commands (C→Z; /taskaddplayer Send_OP_AvaliableTask @0x4ec970, /taskremoveplayer @0x4eca60,
// /taskmakeleader @0x4ecc00, the task window's STASK_ buttons): 8 bytes the original leaves unset (zero here), then
// the name in 64 bytes; OP_SharedTaskPlayerList (@0x4ecb50) 8 such bytes; OP_SharedTaskQuit (@0x4ecd00) those 8 and
// a zero u32.
AppPacket encode_shared_task_add_player(std::string_view name);
AppPacket encode_shared_task_remove_player(std::string_view name);
AppPacket encode_shared_task_make_leader(std::string_view name);
AppPacket encode_shared_task_player_list();
AppPacket encode_shared_task_quit();

// OP_TaskRequestTimer (Z→C, 4 bytes): milliseconds.
std::optional<std::uint32_t> decode_task_request_timer(const AppPacket& p);

} // namespace hailstone::net
