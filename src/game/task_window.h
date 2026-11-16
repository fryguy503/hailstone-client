#pragma once

// The quest journal's contents as CTaskWnd builds them (CTaskWnd and the CTaskWnd_* functions;
// CTaskEntry / CTaskElement / PCTaskStatus as eqlib-rof2's TaskSystem.h lays them out):
//   - the task list (TASK_TaskList; CTaskWnd_RebuildTaskLists @0x7720e0, CTaskWnd_CollectTaskRows @0x76ed40,
//     CTaskWnd_SortAndFillTaskList @0x771600, CTaskWnd_AddTaskRow @0x76f080, the timers @0x76ee20 / @0x76ef90);
//   - the objectives (TASK_TaskElementList; CTaskWnd_FillObjectiveList @0x76f2c0);
//   - the description (TASK_Description; CTaskWnd_BuildTaskDetailsSTML @0x770200);
//   - the quest history (QUEST_TaskList / QUEST_TaskElementList; CTaskWnd_FillHistoryList @0x76fb90,
//     CTaskWnd_FillHistoryElements @0x76f700);
//   - the compass mark for a task's NPC (0x76ff70).
// Pure functions over game::TaskState; app/lua_ui_progress.cpp's TaskWindow keeps the window's own state.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

#include "game/tasks.h"

namespace hailstone::game {

class EqStrings;

// The list colours (CListWnd::SetColors in the constructor @0x770940): normal white, highlight yellow, selected grey.
inline constexpr std::uint32_t kTaskListText = 0xffffffff;
// A row record's "time left" for a task with no limit (CTaskWnd_CollectTaskRows).
inline constexpr std::int32_t kTaskUntimed = 100000000;
// A blank row between the list's groups (and between objective steps): its item data 0x40000000; a click on it
// selects the row after it.
inline constexpr std::uint32_t kTaskSeparatorData = 0x40000000;

// CTaskWnd's 0x50-byte row record: the task's index and type (CTaskManager::GetEntry's), its id, the seconds left
// (kTaskUntimed without a limit) and its title.
struct TaskListRow {
    std::uint32_t index = 0, type = 0, task_id = 0;
    std::int32_t left = kTaskUntimed;
    std::string title;
};

// The seconds left of a timed task (PCTaskStatus +4 MovingStartTime = FastTime() - the packet's elapsed seconds at
// arrival; Util::FastTime counts seconds): start + duration - now, 0 once passed.
std::int32_t task_seconds_left(const Task& t, std::int64_t now_ms);

// CTaskWnd_CollectTaskRows @0x76ed40: the rows of one type (the type's slots in index order: 1 for types 0 and 1,
// 29 for 2 and 3), the time left read at `now_ms`.
std::vector<TaskListRow> collect_task_rows(const TaskState& s, std::uint32_t type, std::int64_t now_ms);

// The list's sort (WndNotification 0xf, a click on a column's header: column 1 sorts by title (key 0), column 2 by
// time left (key 1), column 0 keeps the key; the same key again flips the direction): qsort (the CRT's, VS2010:
// msvc_qsort below) with 0x76f970 / 0x76f9b0 (titles, strcmp, ascending / descending) or 0x76f9f0 /
// 0x76fa50 (key 1: two rows whose times differ compare as 1 / -1 whatever their order, equal times by title).
void sort_task_rows(std::vector<TaskListRow>& rows, int key, bool ascending);

// The VS2010 CRT's qsort (@0x8dd9d0, shortsort @0x8dd940): its exact swaps, so an inconsistent comparator (the
// time-left ones) orders as the original's does. `comp(a, b)` returns <0, 0, >0.
template <typename T, typename Comp> void msvc_qsort(std::vector<T>& v, Comp comp);

// One row of a list as CListWnd shows it: the texts, each cell's colour, and whether it is a separator.
struct TaskWindowRow {
    std::vector<std::string> texts;
    std::vector<std::uint32_t> colors; // ARGB per column
    bool separator = false;
    std::uint32_t index = 0, type = 0, task_id = 0; // a task row's record (TASK_TaskList)
};

// The task list's rows (CTaskWnd_SortAndFillTaskList): the tasks (type 0), a separator and the shared task (type 1),
// a separator and the quests (type 2), each through CTaskWnd_AddTaskRow: column 0 the type's letter (the table at
// 0xb4db84: "T", "S", "Q", "E"), 1 the title, 2 the time left ("%02d:%02d:%02d", eqstr 3475 "Unlimited" without a
// limit); columns 0 and 1 coloured by type (0xffdcdc80, 0xffdc80dc, white, 0xff80ffff), red (0xffff0000) under 120 s
// left. Column 2 keeps the list's colour. (Type 3 is collected but not listed.)
std::vector<TaskWindowRow> task_list_rows(const std::vector<TaskListRow>& tasks, const std::vector<TaskListRow>& shared,
                                          const std::vector<TaskListRow>& quests, const EqStrings& strings);
// The timers (CTaskWnd::UpdateTaskTimers @0x76ef90 through @0x76ee20, once a second while shown): a timed task's
// row gets its time left again, and all three columns red when its duration (not its time left) is under 120 s.
void update_task_row_timer(TaskWindowRow& row, const Task& t, std::int64_t now_ms);
std::string task_time_text(std::int32_t left, const EqStrings& strings);

// The zones of an element's ZoneID list (TaskElement_FormatTargetNames @0x480780): each ";"-separated id's full name
// (GetFullZoneNameById: "Unknown Zone" for an id it does not know; ids over 1000 masked with 0x7fff), the first as
// is, the rest after " or ".
std::string task_zone_names(const std::string& zones, const std::function<std::string(std::uint32_t)>& zone_name);

// CTaskWnd_FillObjectiveList @0x76f2c0: for each step 0..19 the task's elements of that step (in slot order, type not
// 0), a separator before every step but the first one shown... (precisely: before the first element of a step other
// than 0). Each row: the description (CTaskManager::GetElementDescription, `element_text`) with its first letter
// upper-cased, eqstr 6115 "(Opt) " before it when optional; the status "Done" (3476) once the count reaches the goal,
// else "%d/%d"; the zones (task_zone_names, or 13205 "ALL"). Colours: grey (0xff808080) and "? ? ?" (3474) in the
// zone column with no status for an element not unlocked yet (type -1), green (0xff00ff00) when done, else white;
// the status column keeps the list's colour.
std::vector<TaskWindowRow> objective_rows(const Task& t, const EqStrings& strings,
                                          const std::function<std::string(const TaskElement&)>& element_text,
                                          const std::function<std::string(std::uint32_t)>& zone_name);

// CTaskWnd_FillHistoryList @0x76fb90: each completed task with an id: "Q" (0xb4db8c), the title, its completion time
// (__ctime32, local time, without ctime's newline); all white.
std::vector<TaskWindowRow> history_rows(const std::vector<CompletedTask>& completed);
std::string task_ctime_text(std::uint32_t t);
// CTaskWnd_FillHistoryElements @0x76f700: a history entry's elements (type not 0): the description (first letter
// upper-cased), "Done" unless type -1, the zones or "ALL"; all green.
std::vector<TaskWindowRow> history_element_rows(const std::vector<TaskElement>& elements, const EqStrings& strings,
                                                const std::function<std::string(const TaskElement&)>& element_text,
                                                const std::function<std::string(std::uint32_t)>& zone_name);

// CTaskWnd_BuildTaskDetailsSTML @0x770200: the description (0x481960's text: task_description_text) and
// "<BR><BR>" when there is one; then, when the task shows its reward (+0xff0) and has any (coin, experience, an
// item, points): 3477 "Reward(s):<BR>", the coin in words (FormatMoneyWords) + "<BR>", 138 "You gain
// experience!!" + "<BR>", the reward item's link + "<BR>", and "<points> " followed by, for a shared task
// (TaskSystem 1), 8957 "Radiant Crystals" (point type 4) or 8958 "Ebon Crystals", else 1215 "points".
std::string task_details_stml(const Task& t, const EqStrings& strings,
                              const std::function<std::string(const TaskElement&)>& element_text);

// CTaskSelectWnd_ShowTaskDescription @0x76b310: the offer's processed description, then eqstr 6029
// ("This task begins in %1") with element slot 0's zone names when its type is neither 0 nor -1 and it has zones.
std::string task_offer_details_stml(const TaskOffer& offer, const EqStrings& strings,
                                    const std::function<std::string(const TaskElement&)>& element_text,
                                    const std::function<std::string(std::uint32_t)>& zone_name);

// The abandon question (CTaskWnd::WndNotification, TASK_AbandonButton): 8995 for a task, 8996 for the shared task,
// 8997 for a quest, with the title; ConfirmationDialog(100, Yes / No, 10 s, the task id).
std::uint32_t task_abandon_eqstr(std::uint32_t type);
inline constexpr int kTaskAbandonTimeoutMs = 10000;

// 0x76ff70: the compass mark for the first unfinished deliver (1) or speak-with (4) element of the task or the
// shared task (types 0 and 1, in slot order) whose TargetZoneID list is empty or holds the current zone and whose
// target name (lower-cased, spaces as '_', split at ';') is part of an NPC's name (strstr over spawn names, the
// first NPC in spawn order); the element found is (task id, slot).
struct TaskCompassSpawn {
    std::string name; // PlayerBase +0xa4 Name
    bool npc = false; // +0x125 Type == 1
    glm::vec3 position{0.0f};
};
struct TaskCompassMark {
    std::uint32_t task_id = 0;
    std::uint32_t slot = 0;
    glm::vec3 position{0.0f};
};
std::optional<TaskCompassMark> task_compass_mark(const TaskState& s, std::uint32_t zone_id,
                                                 const std::vector<TaskCompassSpawn>& spawns);

// ------------------------------------------------------------------------------------------------------------------

template <typename T, typename Comp> void msvc_qsort(std::vector<T>& v, Comp comp) {
    using std::swap;
    const int num = static_cast<int>(v.size());
    if (num < 2) return;
    int lostk[32], histk[32];
    int stkptr = 0;
    int lo = 0, hi = num - 1;
    for (;;) {
        int size = hi - lo + 1;
        if (size <= 8) {
            // shortsort: the largest of [lo, hi] (a later one only when strictly greater) swapped to hi.
            int h = hi;
            while (h > lo) {
                int max = lo;
                for (int p = lo + 1; p <= h; ++p)
                    if (comp(v[p], v[max]) > 0) max = p;
                if (max != h) swap(v[max], v[h]);
                --h;
            }
        } else {
            int mid = lo + (size / 2);
            if (comp(v[lo], v[mid]) > 0) swap(v[lo], v[mid]);
            if (comp(v[lo], v[hi]) > 0) swap(v[lo], v[hi]);
            if (comp(v[mid], v[hi]) > 0) swap(v[mid], v[hi]);
            int loguy = lo, higuy = hi;
            for (;;) {
                if (mid > loguy) {
                    do {
                        ++loguy;
                    } while (loguy < mid && comp(v[loguy], v[mid]) <= 0);
                }
                if (mid <= loguy) {
                    do {
                        ++loguy;
                    } while (loguy <= hi && comp(v[loguy], v[mid]) <= 0);
                }
                do {
                    --higuy;
                } while (higuy > mid && comp(v[higuy], v[mid]) > 0);
                if (higuy < loguy) break;
                swap(v[loguy], v[higuy]);
                if (mid == higuy) mid = loguy;
            }
            ++higuy;
            if (mid < higuy) {
                do {
                    --higuy;
                } while (higuy > mid && comp(v[higuy], v[mid]) == 0);
            }
            if (mid >= higuy) {
                do {
                    --higuy;
                } while (higuy > lo && comp(v[higuy], v[mid]) == 0);
            }
            if (higuy - lo >= hi - loguy) {
                if (lo < higuy) lostk[stkptr] = lo, histk[stkptr] = higuy, ++stkptr;
                if (loguy < hi) {
                    lo = loguy;
                    continue;
                }
            } else {
                if (loguy < hi) lostk[stkptr] = loguy, histk[stkptr] = hi, ++stkptr;
                if (lo < higuy) {
                    hi = higuy;
                    continue;
                }
            }
        }
        if (--stkptr < 0) return;
        lo = lostk[stkptr];
        hi = histk[stkptr];
    }
}

} // namespace hailstone::game
