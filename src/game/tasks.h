#pragma once

// Tasks and quests as the client knows them (CTaskManager): the active tasks
// (OP_TaskDescription @0x4812b0 and OP_TaskActivity @0x481550 per element),
// the completed list (OP_CompletedTasks @0x480c50) and a completed task's
// history (OP_TaskHistoryReply @0x480ac0), the offers of a task giver
// (OP_TaskSelectWindow / OP_SharedTaskSelectWindow, CTaskSelectWnd_ReadTaskSelectPacket
// @0x76bda0) and the element descriptions (CTaskManager::GetElementDescription
// @0x4800c0). Pure state; the packets are handled in net/game_connection_progress.cpp.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hailstone::game {

class EqStrings;

// Task types (EQEmu TaskType; the original's CTaskManager keeps one task, one shared task and the quests).
inline constexpr std::uint32_t kTaskTypeTask = 0, kTaskTypeShared = 1, kTaskTypeQuest = 2;

// One objective (activity / element).
struct TaskElement {
    std::uint32_t activity_id = 0;
    std::uint32_t step = 0;          // list_group: the window's step (CTaskElement +0xd0 ElementGroup)
    bool hidden = true;              // sent "short" (not unlocked yet: shown as ? ? ?; type -1)
    std::int32_t type = 0;           // CTaskElement +0 (EQEmu TaskActivityType): 1 deliver, 2 kill, 3 loot, 4 speak,
                                     // 5 explore, ...; 0 none, -1 not unlocked yet
    bool optional = false;
    std::string target_name;         // "|"-free name text the server builds
    std::string item_list;
    std::int32_t goal_count = 0;
    std::string skill_list;          // ";"-separated skill ids, "-1" none
    std::string spell_list;          // ";"-separated spell ids, "0" none
    std::string zones;               // ";"-separated zone ids ("" all; +0x48 ZoneID)
    std::string target_zones;        // the second zone list (+0x88 TargetZoneID): where the compass looks
    std::int32_t dz_switch = 0;
    std::string description;         // the server's override, else built from the type
    std::int32_t done_count = 0;
    bool done() const { return !hidden && done_count >= goal_count; }
};

struct Task {
    std::uint32_t sequence = 0;      // the slot (0 for the task / shared task, the quest index)
    std::uint32_t type = kTaskTypeQuest;
    std::uint32_t task_id = 0;
    std::string title;
    std::uint32_t duration = 0;      // seconds, 0 none (then duration_code: 1 short, 2 medium, 3 long)
    std::uint32_t duration_code = 0;
    std::uint32_t elapsed = 0;       // seconds since accepted when the description came (RoF2 sends elapsed time)
    std::int64_t received_ms = 0;    // when (the client's clock, for counting down)
    std::string description;
    bool has_rewards = false;
    std::uint32_t coin_reward = 0;   // copper
    bool xp_reward = false, faction_reward = false;
    std::string reward;              // the reward text
    std::string reward_link;         // the reward's item link (RoF2 link format)
    std::uint32_t points = 0;
    std::uint32_t reward_type = 0;   // CTaskEntry +0x2c3c PointType
    bool open_window = false;
    bool reward_set = false;         // the trailer's byte (+0x3bf9 bHasRewardSet): Preview Reward is enabled
    std::map<std::uint32_t, TaskElement> elements; // by activity id
};

struct CompletedTask {
    std::uint32_t task_id = 0;
    std::string title;
    std::uint32_t completed_time = 0; // unix time
};

// One offer in the task select window.
struct TaskOffer {
    std::uint32_t task_id = 0;
    float reward_multiplier = 1.0f;
    std::uint32_t duration = 0, duration_code = 0;
    std::string title;
    std::string description;
    bool reward_preview = false;
    std::vector<TaskElement> elements; // the initially active elements (activity_id = the element index)
};

struct TaskOffers {
    std::uint32_t type = kTaskTypeQuest; // the selection's type (1: shared, answered with OP_SharedTaskAcceptNew)
    std::uint32_t giver_id = 0;          // the task giver's spawn id
    bool shared = false;                 // came as OP_SharedTaskSelectWindow
    std::vector<TaskOffer> offers;
};

struct TaskHistory {
    std::uint32_t index = 0; // the completed list index the client asked about
    std::vector<TaskElement> elements;
};

struct TaskState {
    std::vector<Task> tasks;              // active, in arrival order
    std::vector<CompletedTask> completed; // OP_CompletedTasks (the 50 most recent)
    std::optional<TaskOffers> offers;     // an open task select window
    // OP_TaskHistoryReply by history index (CTaskManager +0x74388 QuestHistoryEntries, their +0x3be1
    // bElementsReceived): kept until the next OP_CompletedTasks empties them (CTaskManager_ClearAllTaskEntries).
    std::map<std::uint32_t, std::vector<TaskElement>> history;
    // Bumped where the original marks the task window for a rebuild (0x76ff50: OP_TaskDescription,
    // OP_TaskActivity, OP_CancelTask, OP_CompletedTasks, entering a zone).
    std::uint32_t revision = 0;
    std::int64_t request_timer_until_ms = 0; // OP_TaskRequestTimer: no new shared task requests before this
    std::vector<std::string> shared_members; // OP_SharedTaskMemberList / MemberChange: the shared task's members
    std::string shared_leader;               // the member the list flags as leader (STPR_Leader), "" none
    std::map<std::string, std::uint32_t, std::less<>> shared_member_templates;

    Task* find(std::uint32_t type, std::uint32_t sequence);
    const Task* find(std::uint32_t type, std::uint32_t sequence) const;
    const Task* find_id(std::uint32_t task_id) const;
    // OP_TaskDescription: a task (re)described; replaces the one in its slot and clears its elements
    // when the task changed or the packet opens the window.
    Task& describe(Task t);
    // OP_TaskActivity: one element of the task in (type, sequence) whose id matches.
    bool update_element(std::uint32_t type, std::uint32_t sequence, std::uint32_t task_id, TaskElement e);
    // OP_CancelTask from the server: the task in that slot is gone.
    bool remove(std::uint32_t type, std::uint32_t sequence);
};

// CTaskManager::GetElementDescription: the server's description, else the element type's template
// (the original's table at 0xabf484: eqstr with and without a target), with %1 the goal count, %2 the
// target name, %3 the items, %4 the spell names, %5 the skill names (";"-separated ids joined with
// " or "). `spell_name` gives a spell's name (empty when unknown).
std::string element_description(const TaskElement& e, const EqStrings& strings,
                                const std::function<std::string(std::uint32_t)>& spell_name);

// The description a task window shows (0x481960, which CTaskWnd_BuildTaskDetailsSTML runs once on a
// task's description): text in "[n,m,...,text]" markers shows only while the elements n, m, ... (1-based activity
// numbers, each 1..20) all exist (type not 0 or -1) and are not all done; a marker without numbers ("[text]") never
// shows; text outside markers always shows. Then each "%N" (N = 20 down to 1, the first occurrence) becomes element
// N's description (`element_text`) when the element exists. At most 4000 characters.
std::string task_description_text(const Task& task, const std::function<std::string(const TaskElement&)>& element_text);

} // namespace hailstone::game

