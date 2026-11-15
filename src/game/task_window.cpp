#include "game/task_window.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <string_view>

#include "core/msvc_atoi.h"
#include "game/eq_strings.h"
#include "game/item_links.h"
#include "game/merchant.h"
#include "platform/local_time.h"

namespace hailstone::game {

namespace {

constexpr std::uint32_t kRed = 0xffff0000, kGreen = 0xff00ff00, kGrey = 0xff808080;
constexpr std::uint32_t kStrUnlimited = 3475, kStrDone = 3476, kStrHidden = 3474, kStrAll = 13205, kStrOptional = 6115;
constexpr std::uint32_t kStrRewards = 3477, kStrExperience = 138, kStrRadiant = 8957, kStrEbon = 8958, kStrPoints = 1215;

// The slots of a type (0x480080).
std::uint32_t slot_count(std::uint32_t type) {
    if (type == kTaskTypeTask || type == kTaskTypeShared) return 1;
    if (type == kTaskTypeQuest || type == 3) return 29;
    return 0;
}

// The type letters (the pointer table at 0xb4db84).
const char* type_letter(std::uint32_t type) {
    static constexpr const char* kLetters[] = {"T", "S", "Q", "E"};
    return type < 4 ? kLetters[type] : "";
}

// strcmp's sign over the titles (the comparators' byte loop).
int compare_titles(const std::string& a, const std::string& b) {
    int c = std::strcmp(a.c_str(), b.c_str());
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

// The first character upper-cased (CXStr::GetUnicode(0), iswctype(_UPPER | _LOWER ... 2 = _LOWER), towupper).
std::string upper_first(std::string s) {
    if (!s.empty() && std::islower(static_cast<unsigned char>(s[0])))
        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

std::string zone_column(const TaskElement& e, const EqStrings& strings,
                        const std::function<std::string(std::uint32_t)>& zone_name, std::uint32_t empty_eqstr) {
    std::string out = strings.format(empty_eqstr, {});
    if (!e.zones.empty()) out = task_zone_names(e.zones, zone_name).substr(0, 255); // steve_strncpy 0x100
    return out;
}

// __ConvertItemTags(text, 1) @0x4ed110 as the task reads the reward's item tag: each \x12 tag an STML link
// (XWM_LINK 27 with the tag), showing the item's name.
std::string item_tags_stml(const std::string& text) {
    std::string out;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t a = text.find('\x12', at);
        std::size_t b = a == std::string::npos ? std::string::npos : text.find('\x12', a + 2);
        if (b == std::string::npos) {
            out += text.substr(at);
            break;
        }
        out += text.substr(at, a - at);
        std::string tag = text.substr(a + 1, b - a - 1);
        auto link = parse_item_link(tag);
        // (the link's data and text are the server's: encoded and escaped so they cannot end the tag)
        out += std::format("<a WndNotify=\"27,{}\">{}</a>", stml_link_data(tag), escape_stml(link ? link->text : tag));
        at = b + 1;
    }
    return out;
}

} // namespace

std::int32_t task_seconds_left(const Task& t, std::int64_t now_ms) {
    std::int64_t start = t.received_ms / 1000 - static_cast<std::int64_t>(t.elapsed);
    std::int64_t now = now_ms / 1000;
    std::int64_t end = start + static_cast<std::int64_t>(t.duration);
    return now < end ? static_cast<std::int32_t>(end - now) : 0;
}

std::vector<TaskListRow> collect_task_rows(const TaskState& s, std::uint32_t type, std::int64_t now_ms) {
    std::vector<TaskListRow> rows;
    for (std::uint32_t i = 0; i < slot_count(type); ++i) {
        const Task* t = s.find(type, i);
        if (!t || t->task_id == 0) continue;
        TaskListRow r;
        r.index = i;
        r.type = type;
        r.task_id = t->task_id;
        r.left = t->duration != 0 ? task_seconds_left(*t, now_ms) : kTaskUntimed;
        r.title = t->title.substr(0, 63);
        rows.push_back(std::move(r));
    }
    return rows;
}

void sort_task_rows(std::vector<TaskListRow>& rows, int key, bool ascending) {
    if (rows.size() < 2) return;
    if (key == 0) {
        msvc_qsort(rows, [ascending](const TaskListRow& a, const TaskListRow& b) {
            int c = compare_titles(a.title, b.title);
            return ascending ? c : -c;
        });
    } else if (key == 1) {
        msvc_qsort(rows, [ascending](const TaskListRow& a, const TaskListRow& b) {
            if (a.left != b.left) return ascending ? 1 : -1;
            int c = compare_titles(a.title, b.title);
            return ascending ? c : -c;
        });
    }
}

std::string task_time_text(std::int32_t left, const EqStrings& strings) {
    if (left == kTaskUntimed) return strings.format(kStrUnlimited, {});
    std::uint32_t u = static_cast<std::uint32_t>(left);
    return std::format("{:02}:{:02}:{:02}", static_cast<int>(u / 3600), static_cast<int>((u % 3600) / 60),
                       static_cast<int>(u % 60));
}

std::vector<TaskWindowRow> task_list_rows(const std::vector<TaskListRow>& tasks, const std::vector<TaskListRow>& shared,
                                          const std::vector<TaskListRow>& quests, const EqStrings& strings) {
    std::vector<TaskWindowRow> out;
    auto add = [&](const TaskListRow& r) {
        std::uint32_t c = kTaskListText;
        switch (r.type) {
        case 0: c = 0xffdcdc80; break;
        case 1: c = 0xffdc80dc; break;
        case 2: c = 0xffffffff; break;
        case 3: c = 0xff80ffff; break;
        }
        if (static_cast<std::uint32_t>(r.left) < 120) c = kRed;
        TaskWindowRow row;
        row.texts = {type_letter(r.type), r.title, task_time_text(r.left, strings)};
        row.colors = {c, c, kTaskListText};
        row.index = r.index;
        row.type = r.type;
        row.task_id = r.task_id;
        out.push_back(std::move(row));
    };
    auto separator = [&] {
        TaskWindowRow row;
        row.texts = {"", "", ""};
        row.colors = {kTaskListText, kTaskListText, kTaskListText};
        row.separator = true;
        out.push_back(std::move(row));
    };
    for (const auto& r : tasks) add(r);
    if (!shared.empty()) {
        if (!out.empty()) separator();
        for (const auto& r : shared) add(r);
    }
    if (!quests.empty()) {
        if (!out.empty()) separator();
        for (const auto& r : quests) add(r);
    }
    return out;
}

void update_task_row_timer(TaskWindowRow& row, const Task& t, std::int64_t now_ms) {
    if (row.separator || t.duration == 0) return; // untimed: 3475 written again
    if (static_cast<std::int32_t>(t.duration) < 120) row.colors = {kRed, kRed, kRed};
    std::int32_t left = task_seconds_left(t, now_ms);
    std::uint32_t u = static_cast<std::uint32_t>(left);
    if (row.texts.size() > 2)
        row.texts[2] = std::format("{:02}:{:02}:{:02}", static_cast<int>(u / 3600), static_cast<int>((u % 3600) / 60),
                                   static_cast<int>(u % 60));
}

std::string task_zone_names(const std::string& zones, const std::function<std::string(std::uint32_t)>& zone_name) {
    std::string list = zones.substr(0, 63); // steve_strncpy into the 0x40-byte strtok buffer
    std::string out;
    std::size_t p = 0;
    while (p < list.size()) {
        std::size_t q = list.find(';', p);
        if (q == std::string::npos) q = list.size();
        if (q > p) {
            int id = core::msvc_atoi(list.substr(p, q - p));
            std::uint32_t z = static_cast<std::uint32_t>(id);
            if (id > 1000) z = static_cast<std::uint32_t>(id) & 0x7fff;
            std::string name = id > 0 && z > 0 && z < 1001 && zone_name ? zone_name(z) : std::string();
            if (name.empty()) name = "Unknown Zone";
            out += out.empty() ? name : " or " + name;
        }
        p = q + 1;
    }
    return out;
}

std::vector<TaskWindowRow> objective_rows(const Task& t, const EqStrings& strings,
                                          const std::function<std::string(const TaskElement&)>& element_text,
                                          const std::function<std::string(std::uint32_t)>& zone_name) {
    std::vector<TaskWindowRow> out;
    for (std::uint32_t step = 0; step < 20; ++step) {
        int shown = 0;
        for (std::uint32_t slot = 0; slot < 20; ++slot) {
            auto it = t.elements.find(slot);
            if (it == t.elements.end()) continue;
            const TaskElement& e = it->second;
            if (e.step != step || e.type == 0) continue;
            if (shown == 0 && step != 0) {
                TaskWindowRow sep;
                sep.texts = {"", "", ""};
                sep.colors = {kTaskListText, kTaskListText, kTaskListText};
                sep.separator = true;
                out.push_back(std::move(sep));
            }
            ++shown;
            std::uint32_t color = kTaskListText;
            std::string zone = strings.format(kStrAll, {});
            if (e.type == -1) {
                color = kGrey;
                zone = strings.format(kStrHidden, {});
            } else if (e.goal_count <= e.done_count) {
                color = kGreen;
            }
            std::string text = upper_first(element_text(e));
            if (e.optional) text = strings.format(kStrOptional, {}) + text;
            if (!e.zones.empty()) zone = task_zone_names(e.zones, zone_name).substr(0, 255);
            TaskWindowRow row;
            std::string status;
            if (e.type != -1)
                status = color == kGreen ? strings.format(kStrDone, {}) : std::format("{}/{}", e.done_count, e.goal_count);
            row.texts = {std::move(text), std::move(status), std::move(zone)};
            row.colors = {color, kTaskListText, color};
            out.push_back(std::move(row));
        }
    }
    return out;
}

std::string task_ctime_text(std::uint32_t t) {
    std::time_t tt = static_cast<std::time_t>(t);
    char buf[64] = {};
    if (auto tm = platform::local_time(tt)) std::strftime(buf, sizeof buf, "%a %b %d %H:%M:%S %Y", &*tm);
    return buf;
}

std::vector<TaskWindowRow> history_rows(const std::vector<CompletedTask>& completed) {
    std::vector<TaskWindowRow> out;
    for (std::size_t i = 0; i < completed.size() && i < 50; ++i) {
        const CompletedTask& c = completed[i];
        if (static_cast<std::int32_t>(c.task_id) <= 0) continue;
        TaskWindowRow row;
        row.texts = {type_letter(kTaskTypeQuest), c.title, task_ctime_text(c.completed_time)};
        row.colors = {kTaskListText, kTaskListText, kTaskListText};
        row.index = static_cast<std::uint32_t>(i);
        row.task_id = c.task_id;
        out.push_back(std::move(row));
    }
    return out;
}

std::vector<TaskWindowRow> history_element_rows(const std::vector<TaskElement>& elements, const EqStrings& strings,
                                                const std::function<std::string(const TaskElement&)>& element_text,
                                                const std::function<std::string(std::uint32_t)>& zone_name) {
    std::vector<TaskWindowRow> out;
    for (std::size_t i = 0; i < elements.size() && i < 20; ++i) {
        const TaskElement& e = elements[i];
        if (e.type == 0) continue;
        TaskWindowRow row;
        std::string status = e.type != -1 ? strings.format(kStrDone, {}) : std::string();
        row.texts = {upper_first(element_text(e)), std::move(status), zone_column(e, strings, zone_name, kStrAll)};
        row.colors = {kGreen, e.type != -1 ? kGreen : kTaskListText, kGreen};
        out.push_back(std::move(row));
    }
    return out;
}

std::string task_details_stml(const Task& t, const EqStrings& strings,
                              const std::function<std::string(const TaskElement&)>& element_text) {
    std::string out;
    bool any = static_cast<std::int32_t>(t.coin_reward) > 0 || t.xp_reward || !t.reward_link.empty() || t.points != 0;
    if (!t.description.empty()) out = task_description_text(t, element_text) + "<BR><BR>";
    if (t.has_rewards && any) {
        out += strings.format(kStrRewards, {});
        if (static_cast<std::int32_t>(t.coin_reward) > 0) out += money_words(static_cast<std::int32_t>(t.coin_reward)) + "<BR>";
        if (t.xp_reward) out += strings.format(kStrExperience, {}) + "<BR>";
        if (!t.reward_link.empty()) out += item_tags_stml(t.reward_link) + "<BR>";
        if (t.points != 0) {
            out += std::to_string(static_cast<std::int32_t>(t.points)) + " ";
            std::uint32_t id = kStrPoints;
            if (t.type == kTaskTypeShared) id = t.reward_type == 4 ? kStrRadiant : kStrEbon;
            out += strings.format(id, {});
        }
    }
    return out;
}

std::uint32_t task_abandon_eqstr(std::uint32_t type) {
    if (type == kTaskTypeTask) return 8995;
    return type == kTaskTypeShared ? 8996 : 8997;
}

std::string task_offer_details_stml(const TaskOffer& offer, const EqStrings& strings,
                                    const std::function<std::string(const TaskElement&)>& element_text,
                                    const std::function<std::string(std::uint32_t)>& zone_name) {
    Task task;
    task.description = offer.description;
    for (const auto& e : offer.elements) task.elements[e.activity_id] = e;
    std::string out = task_description_text(task, element_text);
    if (const auto it = task.elements.find(0); it != task.elements.end()) {
        const auto& e = it->second;
        if (e.type != 0 && e.type != -1 && !e.zones.empty()) {
            const std::string args[]{task_zone_names(e.zones, zone_name)};
            out += strings.format(6029, args);
        }
    }
    return out;
}

std::optional<TaskCompassMark> task_compass_mark(const TaskState& s, std::uint32_t zone_id,
                                                 const std::vector<TaskCompassSpawn>& spawns) {
    for (std::uint32_t type = 0; type < 2; ++type) {
        const Task* t = s.find(type, 0);
        if (!t || t->task_id == 0) continue;
        for (std::uint32_t slot = 0; slot < 20; ++slot) {
            auto it = t->elements.find(slot);
            if (it == t->elements.end()) continue;
            const TaskElement& e = it->second;
            if (e.type != 1 && e.type != 4) continue;
            if (e.done_count >= e.goal_count) continue;
            // 0x76fed0: the TargetZoneID list (atoi of each ';' token) holds the zone.
            if (!e.target_zones.empty()) {
                bool here = false;
                std::string list = e.target_zones.substr(0, 63);
                std::size_t p = 0;
                while (p < list.size() && !here) {
                    std::size_t q = list.find(';', p);
                    if (q == std::string::npos) q = list.size();
                    if (q > p && static_cast<std::uint32_t>(core::msvc_atoi(list.substr(p, q - p))) == zone_id) here = true;
                    p = q + 1;
                }
                if (!here) continue;
            }
            // Utf8ToLower into 0x100 bytes, ' ' -> '_'.
            std::string name = e.target_name.substr(0, 255);
            for (char& c : name) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (c == ' ') c = '_';
            }
            if (name.empty()) continue;
            std::string list = name.substr(0, 63); // 0x76fe50's 0x40-byte strtok buffer
            std::vector<std::string> tokens;
            std::size_t p = 0;
            while (p < list.size()) {
                std::size_t q = list.find(';', p);
                if (q == std::string::npos) q = list.size();
                if (q > p) tokens.push_back(list.substr(p, q - p));
                p = q + 1;
            }
            for (const auto& sp : spawns) {
                if (!sp.npc) continue;
                for (const auto& tok : tokens)
                    if (sp.name.find(tok) != std::string::npos) return TaskCompassMark{t->task_id, slot, sp.position};
            }
        }
    }
    return std::nullopt;
}

} // namespace hailstone::game
