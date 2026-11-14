// Skills and training: SkillsWindow (CSkillsWnd) and TrainWindow (CTrainWnd), over the character's skills and the
// trainer session (GameConnection::training, game/training.h).

#include <algorithm>
#include <cstdlib>
#include <array>
#include <format>
#include <string>
#include <vector>

#include "app/lua_eq_windows.h"
#include "game/eq_names.h"
#include "game/hot_button_press.h"
#include "game/leadership.h"
#include "game/mechanics/character.h"
#include "game/mechanics/tables.h"
#include "game/quantity_dialog.h"
#include "game/spell_display.h"
#include "game/item_links.h"
#include "game/task_window.h"
#include "game/tasks.h"
#include "game/zone_names.h"
#include "net/game_connection.h"
#include "ui/dev_screens.h"

namespace hailstone::app {

namespace {

std::string fmt(const net::GameConnection& c, std::uint32_t id, std::vector<std::string> args) {
    return c.strings().format(id, args);
}

// CSkillsWnd_GetSkillRowInfo @0x757710: the rank by value (0x9fa5b4: 10, 20 .. 100, 2000; names 0x9fa5e8:
// Awful .. Master), a language of 100 "Master".
std::string rank_name(const net::GameConnection& c, std::int32_t value, bool language) {
    static constexpr std::int32_t kTop[11] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 2000};
    int i = 0;
    while (i < 11 && value > kTop[i]) ++i;
    if (i == 9 && language && value == 100) i = 10;
    if (i > 10) i = 10;
    return c.strings().format(0xc98 + static_cast<std::uint32_t>(i), {});
}

// 0x5baa20: the first level the class has the skill (GetSkillCap above 0); 0 when never.
std::int32_t skill_level(const game::mechanics::Character& ch, std::uint8_t cls, std::uint32_t skill) {
    for (std::int32_t l = 1; l <= 100; ++l)
        if (ch.skill_cap(l, cls, static_cast<std::int32_t>(skill), false) > 0) return l;
    return 0;
}

struct SkillRow {
    std::string name, rank;
    std::int32_t base = 0, value = 0, cap = 0;
};

// The row CSkillsWnd shows for a skill (< 100) or language (100 + n).
std::optional<SkillRow> skill_row(const EqHost& host, std::uint32_t id) {
    if (!host.conn) return std::nullopt;
    const auto& c = *host.conn;
    const auto* ch = host.character;
    SkillRow r;
    if (id < 100) {
        r.name = c.skill_name(id);
        r.base = c.skill_value(id);
        r.value = ch ? ch->skill_with_item_mods(id, true) : r.base;
        r.cap = ch ? ch->skill_cap(ch->sheet().level, ch->sheet().class_id, static_cast<std::int32_t>(id)) : 0;
    } else if (id < 0x84) {
        r.name = c.strings().format(game::language_eqstr(id - 100), {});
        r.base = r.value = c.skill_value(id);
    } else {
        return std::nullopt;
    }
    // A skill not yet trained that the class gets at a later level: "LEVEL: n".
    if (r.base == 0 && id < 100 && ch) {
        std::uint8_t cls = ch->sheet().class_id;
        if (ch->skill_cap(100, cls, static_cast<std::int32_t>(id), false) != 0) {
            std::int32_t at = skill_level(*ch, cls, id);
            if (ch->sheet().level < at) {
                r.rank = fmt(c, 0x871, {std::to_string(at)});
                return r;
            }
        }
    }
    r.rank = rank_name(c, r.base, id >= 100);
    return r;
}

// UpdateAll's value colour: red under the trained value, green over it, yellow over the cap.
Rgb value_color(const SkillRow& r) {
    std::uint32_t argb = 0xffffffff;
    if (r.value < r.base) argb = 0xffff0000;
    else if (r.base < r.value) argb = 0xff00ff00;
    if (r.cap != 0 && r.cap < r.base) argb = 0xffffff00;
    return argb_to_rgb(argb);
}

std::string value_text(const SkillRow& r) { return r.cap == 0 ? std::to_string(r.value) : std::format("{}/{}", r.value, r.cap); }

class SkillsWindow : public StockWindow {
public:
    SkillsWindow() : StockWindow("SkillsWindow", false) {}

    // AboutToShow: every skill the character has (CharacterZoneClient::HasSkill), then each known language.
    const ListData* list(EqHost& host, std::string_view id) override {
        if (id != "SkillList" || !host.conn) return nullptr;
        list_ = {};
        list_.columns = 3;
        list_.labels = {"Skill Name", "Rank", "Value"};
        list_.widths = {130, 80, 60};
        list_.selected = selected_;
        const auto* ch = host.character;
        ids_.clear();
        for (std::uint32_t s = 0; s < 100; ++s) {
            if (ch ? ch->adjusted_skill(s) <= 0 : host.conn->skill_value(s) <= 0) continue;
            if (auto r = skill_row(host, s)) {
                list_.rows.push_back({{r->name, r->rank, value_text(*r)}, value_color(*r), std::nullopt});
                ids_.push_back(s);
            }
        }
        for (std::uint32_t l = 0; l < 0x20; ++l)
            if (host.conn->skill_value(100 + l) != 0)
                if (auto r = skill_row(host, 100 + l)) {
                    list_.rows.push_back({{r->name, r->rank, value_text(*r)}, value_color(*r), std::nullopt});
                    ids_.push_back(100 + l);
                }
        return &list_;
    }
    // CSkillsWnd::WndNotification's Make Hotkey: the selected row's skill (item data < 100: not a language) on the
    // cursor as a skill hotbutton (type 0xb attachment, the skill's name). A drag of a row makes the same.
    std::optional<game::HotButton> pick_up(EqHost& host, std::string_view id, int row) override {
        if (id == "MakeHotKeyButton") row = selected_;
        else if (id != "SkillList") return std::nullopt;
        if (!host.conn || row < 0 || row >= static_cast<int>(ids_.size()) || ids_[static_cast<std::size_t>(row)] >= 100)
            return std::nullopt;
        game::HotButton b;
        b.type = game::HotButtonType::Skill;
        b.slot = static_cast<std::int32_t>(ids_[static_cast<std::size_t>(row)]);
        b.label = host.conn->skill_name(ids_[static_cast<std::size_t>(row)]).substr(0, 63);
        return b;
    }
    bool list_click(EqHost&, std::string_view id, int row, bool, int) override {
        if (id != "SkillList") return false;
        selected_ = row;
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (id == "DoneButton" && button == "left") {
            set_shown(host, false);
            return true;
        }
        if (id == "MakeHotKeyButton" && button == "left") {
            if (auto b = host.cursor_button ? std::nullopt : pick_up(host, id, -1)) {
                host.cursor_button = *b;
                ++host.cursor_serial;
            }
            return true;
        }
        return false;
    }

private:
    ListData list_;
    std::vector<std::uint32_t> ids_; // the skill (or 100 + language) of each row
    int selected_ = -1;
};

// CTrainWnd @0x780d20: the trainer's skills (name, rank, value / cap and the cost of a point in platinum, gold,
// silver and copper), the practice points and the player's money (UpdateRight), Train for the selected row when it is
// affordable and the level allows it, Done closes (OP_GMEndTraining).
class TrainWindow : public StockWindow {
public:
    TrainWindow() : StockWindow("TrainWindow", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->training(); }
    void set_shown(EqHost& host, bool show) override {
        if (!show && host.conn && host.conn->training()) host.conn->close_trainer();
    }
    std::string title(EqHost& host) override {
        return host.conn && host.conn->training() ? game::display_name(host.conn->training()->trainer_name) : std::string();
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (id != "SkillList" || !host.conn || !host.conn->training()) return nullptr;
        const auto& t = *host.conn->training();
        list_ = {};
        list_.columns = 7;
        list_.labels = {"Skill Name", "Rank", "Value", "", "", "", ""};
        list_.widths = {125, 95, 50, 40, 40, 40, 40};
        list_.selected = selected_;
        for (std::size_t i = 0; i < t.rows.size(); ++i) {
            auto kind = static_cast<std::uint32_t>(t.rows[i].kind);
            auto r = skill_row(host, kind);
            if (!r) continue;
            std::int32_t cost = host.conn->train_cost(i);
            ListRow row;
            row.texts = {r->name, r->rank, std::format("{}/{}", r->base, t.rows[i].cap), std::to_string(cost / 1000),
                         std::to_string(cost / 100 % 10), std::to_string(cost / 10 % 10), std::to_string(cost % 10)};
            list_.rows.push_back(std::move(row));
        }
        return &list_;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool, int) override {
        if (id != "SkillList" || !host.conn || !host.conn->training()) return false;
        selected_ = row;
        // "<trainer> says, 'Interested in training in <skill> ?'" (or the language)
        const auto& t = *host.conn->training();
        if (row >= 0 && row < static_cast<int>(t.rows.size()) && host.chat) {
            auto kind = static_cast<std::uint32_t>(t.rows[static_cast<std::size_t>(row)].kind);
            std::string who = game::display_name(t.trainer_name);
            std::string text = kind < 100 ? fmt(*host.conn, 0x2f18, {who, host.conn->skill_name(kind)})
                                          : fmt(*host.conn, 0x2f17, {who, fmt(*host.conn, game::language_eqstr(kind - 100), {})});
            host.chat->add(ui::ChatChannel::System, text, 0);
        }
        return true;
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& coins = host.conn->coins();
        if (id == "PracticeCount") out.text = std::to_string(host.conn->practice_points());
        else if (id == "CoinCount0") out.text = std::to_string(coins.platinum);
        else if (id == "CoinCount1") out.text = std::to_string(coins.gold);
        else if (id == "CoinCount2") out.text = std::to_string(coins.silver);
        else if (id == "CoinCount3") out.text = std::to_string(coins.copper);
        else if (id == "TrainButton") out.enabled = can_train(host);
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        if (id == "DoneButton") host.conn->close_trainer();
        else if (id == "TrainButton") {
            if (can_train(host)) host.conn->train(static_cast<std::size_t>(selected_));
        } else return false;
        return true;
    }

private:
    bool can_train(EqHost& host) const {
        const auto& c = *host.conn;
        if (!c.training() || selected_ < 0 || selected_ >= static_cast<int>(c.training()->rows.size())) return false;
        if (c.practice_points() <= 0) return false;
        const auto& coins = c.coins();
        std::int64_t money = static_cast<std::int64_t>(coins.copper) +
                             10 * (coins.silver + 10 * (static_cast<std::int64_t>(coins.gold) + 10 * coins.platinum));
        if (c.train_cost(static_cast<std::size_t>(selected_)) > money) return false;
        auto kind = static_cast<std::uint32_t>(c.training()->rows[static_cast<std::size_t>(selected_)].kind);
        if (kind < 100 && host.character) {
            std::int32_t at = skill_level(*host.character, host.character->sheet().class_id, kind);
            if (host.character->sheet().level < at) return false;
        }
        return true;
    }
    ListData list_;
    int selected_ = -1;
};

// CAAWnd @0x6093e0: four tabs (General, Archetype, Class, Special: the abilities' types 1..4) listing each visible
// rank (sub_00608ac0): the title, "cur/max" (the rank, one less when not owned), the cost while not at the
// maximum and the category (dbstr 0x13, else the expansion, dbstr 0x14), grey when CanTrainAbility says no; the
// selected ability's description, the AA points, spent and assigned counts, the "Exp to AA" percent (- / +
// change it by 10), Train, Done.
class AAWindow : public StockWindow {
public:
    AAWindow() : StockWindow("AAWindow", false) {}

    const ListData* list(EqHost& host, std::string_view id) override {
        if (host.conn) follow_selection(host.conn->alt_adv());
        if (id == "ModernList") return host.conn ? &modern_list(host) : nullptr;
        int tab = index_of(id);
        if (tab < 0 || !host.conn) return nullptr;
        const auto& c = *host.conn;
        const auto& aa = c.alt_adv();
        ListData& d = lists_[static_cast<std::size_t>(tab)];
        auto& ids = ids_[static_cast<std::size_t>(tab)];
        d = {};
        ids.clear();
        d.columns = 4;
        d.labels = {"Title", "Cur/Max", "Cost", "Category"};
        d.widths = {210, 50, 40, 160};
        for (const game::AltAbility* a : c.aa_list(tab + 1, filter_)) {
            int cur = aa.has(a->id) ? a->rank : a->rank - 1;
            ListRow r;
            r.texts = {c.aa_name(*a), std::format("{}/{}", cur, a->max_rank), cur < a->max_rank ? std::to_string(a->cost) : "",
                       c.aa_category(*a)};
            r.color = c.aa_can_train(a->id) ? Rgb{1, 1, 1} : argb_to_rgb(0xffc0c0c0);
            d.rows.push_back(std::move(r));
            ids.push_back(a->id);
            if (a->id == selected_) d.selected = static_cast<int>(d.rows.size()) - 1;
        }
        return &d;
    }
    // Not in the stock layout: the four tabs' lists as one, for the Modern UI's AA window (ui/modern/lib/aa.lua),
    // each tab's rows in the stock order. The columns are the stock lists' (title, cur/max, cost, category) and then
    // the tab (the ability's type, 1..4), the expansion it came with (dbstr 0x14, "" none), "1" for an activated
    // ability ("0" passive) and the expansion's number (0 none: the era column sorts by it, in release order). The
    // category here is dbstr 0x13 alone: the stock column's fallback to the expansion is the expansion column of its
    // own.
    const ListData& modern_list(EqHost& host) {
        const auto& c = *host.conn;
        const auto& aa = c.alt_adv();
        ListData& d = modern_;
        d = {};
        modern_ids_.clear();
        d.columns = 8;
        d.labels = {"Title", "Cur/Max", "Cost", "Category", "Tab", "Era", "Activated", "Expansion"};
        d.widths = {210, 50, 40, 120, 60, 100, 40, 40};
        for (int tab = 1; tab <= 4; ++tab)
            for (const game::AltAbility* a : c.aa_list(tab, filter_)) {
                int cur = aa.has(a->id) ? a->rank : a->rank - 1;
                ListRow r;
                r.texts = {c.aa_name(*a),
                           std::format("{}/{}", cur, a->max_rank),
                           cur < a->max_rank ? std::to_string(a->cost) : "",
                           a->category > 0 ? c.strings().db_string(static_cast<std::uint32_t>(a->category), 0x13) : "",
                           std::to_string(tab),
                           a->expansion != 0 ? c.strings().db_string(static_cast<std::uint32_t>(a->expansion), 0x14) : "",
                           a->activated() ? "1" : "0",
                           std::to_string(a->expansion)};
                r.color = c.aa_can_train(a->id) ? Rgb{1, 1, 1} : argb_to_rgb(0xffc0c0c0);
                d.rows.push_back(std::move(r));
                modern_ids_.push_back(a->id);
                if (a->id == selected_) d.selected = static_cast<int>(d.rows.size()) - 1;
            }
        return d;
    }
    // The ids of a list's rows (the combined list's or a tab's), nullptr for another child.
    const std::vector<std::uint32_t>* row_ids(std::string_view id) const {
        if (id == "ModernList") return &modern_ids_;
        int tab = index_of(id);
        return tab < 0 ? nullptr : &ids_[static_cast<std::size_t>(tab)];
    }
    bool list_click(EqHost&, std::string_view id, int row, bool, int) override {
        const auto* list = row_ids(id);
        if (!list) return false;
        const auto& ids = *list;
        if (row >= 0 && row < static_cast<int>(ids.size())) selected_ = ids[static_cast<std::size_t>(row)];
        return true;
    }
    // Dragging an ability from the list makes its hotbutton: a social of number 120 + the ability's group, which
    // DoHotButton queues as /alt activate (game/hot_button_press.h kSocialCount), labelled with its name.
    std::optional<game::HotButton> pick_up(EqHost& host, std::string_view id, int row) override {
        if (id == "HotButton") return aa_button(host, selected_); // the Hotkey button dragged
        const auto* list = row_ids(id);
        if (!list || !host.conn) return std::nullopt;
        const auto& ids = *list;
        if (row < 0 || row >= static_cast<int>(ids.size())) return std::nullopt;
        return aa_button(host, ids[static_cast<std::size_t>(row)]);
    }
    static std::optional<game::HotButton> aa_button(EqHost& host, std::uint32_t id) {
        const game::AltAbility* a = host.conn && id ? host.conn->alt_adv().get(id) : nullptr;
        if (!a || a->group <= 0) return std::nullopt;
        game::HotButton b;
        b.type = game::HotButtonType::Social;
        b.slot = game::kSocialCount + a->group;
        b.label = host.conn->aa_name(*a).substr(0, 63);
        return b;
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& c = *host.conn;
        const auto& aa = c.alt_adv();
        follow_selection(aa);
        if (id == "CurrentCount") out.text = std::to_string(aa.points);
        else if (id == "TotalCount") out.text = std::to_string(aa.spent);
        else if (id == "AssignedCount") out.text = std::to_string(aa.assigned[0]);
        else if (id == "ExpCount") out.text = std::to_string(aa.percent) + "%";
        else if (id == "MonsterLabel" || id == "AAW_LoadSaveLabel" || id == "AAW_LoadButton" || id == "AAW_SaveButton")
            out.shown = false; // monster abilities: not on this server
        else if (id == "Description") {
            const game::AltAbility* a = selected_ ? aa.get(selected_) : nullptr;
            out.text = a ? c.aa_description(*a) : std::string();
        } else if (id == "TrainButton") out.enabled = selected_ != 0 && c.aa_can_train(selected_);
        else if (id == "HotButton" || id == "Timer") {
            // sub_00608140 (UpdateSelected, OnProcessFrame): for the selected ability's owned rank with a reuse
            // time, the timer label shows eqstr 0xc27 when ready, else "hh:mm:ss" left, and the Hotkey button is
            // enabled; otherwise the label is empty and the button disabled.
            const game::AltAbility* a = selected_ ? aa.get(selected_) : nullptr;
            const game::AltAbility* owned = a ? aa.owned_rank(a->group) : nullptr;
            bool reusable = owned && owned->refresh > 0;
            if (id == "HotButton") out.enabled = reusable && !host.cursor_button;
            else if (!reusable) out.text = std::string();
            else if (std::int32_t left = c.aa_readiness(owned->id).remaining; left < 1) out.text = c.strings().format(0xc27, {});
            else out.text = std::format("{:02d}:{:02d}:{:02d}", left / 3600, left / 60 % 60, left % 60);
        }
        else if (id == "CanPurchaseFilter") out.checked = filter_.trainable_only;
        else if (id == "TrainAllButton") out.checked = train_all_; // buy every rank (GameConnection::aa_train)
        else if (id == "LessExpButton") out.enabled = aa.percent > 0;
        else if (id == "MoreExpButton") out.enabled = aa.percent < 100;
        else if (id.starts_with("Modern")) modern_info(host, id, out);
    }
    // Not in the stock layout: the selected rank's requirements for the Modern UI's AA window (ui/modern/aa.lua),
    // from the rank CanTrainAbility tests. ModernLevel: the level it needs ("" none; checked: reached);
    // ModernPrereqs: a line per required ability, "name<TAB>rank<TAB>1|0" (held); ModernReuse: "" passive, else
    // "<reuse s><TAB><s left>" (aa_readiness); ModernSelected: "id<TAB>name<TAB>cur<TAB>max<TAB>cost" as the list's
    // row ("" none: the selection may be on another tab).
    void modern_info(EqHost& host, std::string_view id, Child& out) {
        const auto& c = *host.conn;
        const auto& aa = c.alt_adv();
        const game::AltAbility* a = selected_ ? aa.get(selected_) : nullptr;
        out.text = std::string();
        if (!a) return;
        if (id == "ModernSelected") {
            int cur = aa.has(a->id) ? a->rank : a->rank - 1;
            out.text = std::format("{}\t{}\t{}\t{}\t{}", a->id, c.aa_name(*a), cur, a->max_rank,
                                   cur < a->max_rank ? std::to_string(a->cost) : std::string());
        } else if (id == "ModernLevel") {
            std::int32_t level = c.player_status().level;
            if (a->level_req > 0) out.text = std::to_string(a->level_req);
            out.checked = level >= a->level_req;
        } else if (id == "ModernPrereqs") {
            std::string s;
            for (std::size_t i = 0; i < a->prereq_groups.size(); ++i) {
                std::int32_t g = a->prereq_groups[i];
                std::int32_t need = i < a->prereq_ranks.size() ? a->prereq_ranks[i] : 1;
                if (g <= 0 || need <= 0) continue;
                std::string name;
                for (const auto& [rid, r] : aa.table)
                    if (r.group == g) {
                        name = c.aa_name(r);
                        break;
                    }
                const game::AltAbility* held = aa.owned_rank(g);
                if (!s.empty()) s += '\n';
                s += std::format("{}\t{}\t{}", name.empty() ? std::format("Ability {}", g) : name, need,
                                 held && held->rank >= need ? 1 : 0);
            }
            out.text = s;
        } else if (id == "ModernReuse") {
            if (a->spell > 0) {
                auto r = c.aa_readiness(a->id);
                out.text = std::format("{}\t{}", r.total > 0 ? r.total : a->refresh, r.remaining);
            }
        }
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        std::uint32_t pct = c.alt_adv().percent;
        follow_selection(c.alt_adv());
        if (id == "CanPurchaseFilter") {
            // CAAWnd's filter (+0x22c): the check box sets "trainable only"; the kind, expansion and category
            // combos stay at "any" (their contents are not traced).
            filter_.trainable_only = !filter_.trainable_only;
        } else if (id == "TrainAllButton") {
            train_all_ = !train_all_;
        } else if (id == "TrainButton") {
            if (selected_ && c.aa_can_train(selected_)) c.aa_train_button(selected_, train_all_);
        } else if (id == "HotButton") {
            // CAAWnd's Hotkey button: the selected ability's hotbutton on the cursor, to put on a bar.
            if (auto b = host.cursor_button ? std::nullopt : aa_button(host, selected_)) {
                host.cursor_button = *b;
                ++host.cursor_serial;
            }
        } else if (id == "LessExpButton") {
            if (pct > 0) c.set_aa_percent(pct >= 10 ? pct - 10 : 0);
        } else if (id == "MoreExpButton") {
            if (pct < 100) c.set_aa_percent(std::min<std::uint32_t>(pct + 10, 100));
        } else if (id == "DoneButton") {
            set_shown(host, false);
        } else return false;
        return true;
    }
    // CAAWnd::ShowAbility @0x60a7a0 after the window is activated (a right click on an ability's hot button,
    // 0x54fc10): the list row of that ability group, on whichever of the four tabs lists it, is selected and its
    // tab shown; nothing changes when no list holds it.
    bool activate(EqHost& host, int group) override {
        if (!host.conn) return false;
        set_shown(host, true);
        for (int tab = 0; tab < 4; ++tab)
            for (const game::AltAbility* a : host.conn->aa_list(tab + 1, filter_))
                if (a->group == group) {
                    selected_ = a->id;
                    page_ = tab;
                    return true;
                }
        return true;
    }
    std::optional<int> tab_page(EqHost&, std::string_view id) override { return id == "Subwindows" ? std::optional<int>(page_) : std::nullopt; }
    void set_tab_page(EqHost&, std::string_view id, int page) override {
        if (id == "Subwindows") page_ = page;
    }

private:
    // The selection follows its ability: once the selected rank is bought (owned, with a next rank the table
    // knows), the next rank is selected, the one the list now shows, so that Train buys it without a click on the
    // row again. Deliberate difference: not traced in CAAWnd, whose list refill after a purchase is not settled.
    void follow_selection(const game::AltAdvState& aa) {
        for (int hops = 0; hops < 64 && selected_ && aa.has(selected_); ++hops) {
            const game::AltAbility* a = aa.get(selected_);
            if (!a || a->next_id <= 0 || !aa.get(static_cast<std::uint32_t>(a->next_id))) break;
            selected_ = static_cast<std::uint32_t>(a->next_id);
        }
    }
    static int index_of(std::string_view id) {
        if (id.size() == 5 && id.starts_with("List") && id[4] >= '1' && id[4] <= '4') return id[4] - '1';
        return -1;
    }
    std::array<ListData, 4> lists_;
    std::array<std::vector<std::uint32_t>, 4> ids_;
    ListData modern_;
    std::vector<std::uint32_t> modern_ids_;
    std::uint32_t selected_ = 0;
    int page_ = 0;
    game::AAFilter filter_;
    bool train_all_ = false; // TrainAllButton: "buy all" (the server's action 4, else the client's one rank at a time)
};

// A zone's full name (GetFullZoneNameById; "" when unknown).
std::string task_zone_name(std::uint32_t id) { return game::zone_long_name(id); }

// The task window's lists from game/task_window.h rows: the texts and each cell's colour.
ListData task_list_data(const std::vector<game::TaskWindowRow>& rows, std::vector<std::string> labels, std::vector<int> widths,
                        int selected) {
    ListData d;
    d.columns = 3;
    d.labels = std::move(labels);
    d.widths = std::move(widths);
    d.selected = selected;
    for (const auto& r : rows) {
        ListRow row;
        row.texts = r.texts;
        row.color = argb_to_rgb(r.colors.empty() ? game::kTaskListText : r.colors[0]);
        for (std::uint32_t c : r.colors) row.cell_colors.push_back(argb_to_rgb(c));
        d.rows.push_back(std::move(row));
    }
    return d;
}

// CTaskSelectWnd: the offers (title and duration), the selected one's description and objectives, Accept / Decline.
class TaskSelectWindow : public StockWindow {
public:
    TaskSelectWindow() : StockWindow("TaskSelectWnd", false) {}
    bool shown(EqHost& host) override { return host.conn && host.conn->tasks().offers; }
    void set_shown(EqHost& host, bool show) override {
        if (!show && host.conn && host.conn->tasks().offers) host.conn->decline_tasks();
    }
    const ListData* list(EqHost& host, std::string_view id) override {
        if (id != "TaskList" || !shown(host)) return nullptr;
        const auto& o = *host.conn->tasks().offers;
        list_ = {};
        list_.columns = 2;
        list_.labels = {"Task Title", "Duration"};
        list_.widths = {220, 60};
        if (selected_ >= static_cast<int>(o.offers.size())) selected_ = -1;
        if (selected_ < 0 && !o.offers.empty()) selected_ = 0;
        list_.selected = selected_;
        for (const auto& t : o.offers)
            list_.rows.push_back({{t.title, t.duration ? game::format_duration_hms(static_cast<std::int32_t>(t.duration)) : ""},
                                  {1, 1, 1},
                                  std::nullopt});
        return &list_;
    }
    bool list_click(EqHost&, std::string_view id, int row, bool, int) override {
        if (id != "TaskList") return false;
        selected_ = row;
        return true;
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (id != "Description" || !shown(host)) return;
        const auto& o = *host.conn->tasks().offers;
        if (selected_ < 0 || selected_ >= static_cast<int>(o.offers.size())) return;
        const auto& t = o.offers[static_cast<std::size_t>(selected_)];
        // CTaskSelectWnd_ShowTaskDescription @0x76b310: the offer's description through 0x481960 (the step
        // markers and %N element texts, as the task window's; game::task_description_text) over the elements the
        // offer carries. (Its reward line, eqstr 0x178d with the reward's target names, is not built.)
        game::Task task;
        task.title = t.title;
        task.description = t.description;
        for (const auto& e : t.elements) task.elements[e.activity_id] = e;
        const auto& c = *host.conn;
        out.text = game::task_description_text(task, [&c](const game::TaskElement& e) { return c.task_element_text(e); });
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!shown(host) || button != "left") return false;
        const auto& o = *host.conn->tasks().offers;
        if (id == "AcceptButton") {
            if (selected_ >= 0 && selected_ < static_cast<int>(o.offers.size()))
                host.conn->accept_task(o.offers[static_cast<std::size_t>(selected_)].task_id);
        } else if (id == "DeclineButton") {
            host.conn->decline_tasks();
        } else return false;
        return true;
    }

private:
    ListData list_;
    int selected_ = -1;
};

// CTaskWnd (game/task_window.h builds the rows): the active tasks with their type, title and time left, the selected
// task's objectives and description, Remove (asked first), the shared task's members and the leader's buttons, and the
// quest history with a completed task's objectives.
class TaskWindow : public StockWindow {
public:
    TaskWindow() : StockWindow("TaskWnd", false) {}

    void set_shown(EqHost& host, bool show) override {
        if (show && !StockWindow::shown(host)) rebuild_ = true; // AboutToShow: CTaskWnd_RebuildTaskLists
        StockWindow::set_shown(host, show);
    }

    // CTaskWnd::OnProcessFrame @0x7721e0: while shown, the lists built again once a task changed, else the timers.
    void frame(EqHost& host) override {
        if (!host.conn || !StockWindow::shown(host)) return;
        net::GameConnection& c = *host.conn;
        if (rebuild_ || built_revision_ != c.tasks().revision) rebuild(c);
        else update_timers(c);
        if (c.take_task_abandon_sent()) abandon_enabled_ = false;
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& c = *host.conn;
        const auto& ts = c.tasks();
        auto text = [&c](const game::TaskElement& e) { return c.task_element_text(e); };
        if (id == "TASK_TaskList") {
            tasks_ = task_list_data(rows_, {"", "Task Title", "Time Left"}, {25, 220, 60}, selected_);
            return &tasks_;
        }
        if (id == "TASK_TaskElementList") {
            // CTaskWnd_FillObjectiveList @0x76f2c0 for the selected row's entry.
            std::vector<game::TaskWindowRow> rows;
            if (const game::Task* t = selected_task(c)) rows = game::objective_rows(*t, c.strings(), text, task_zone_name);
            elements_ = task_list_data(rows, {"Objective Instructions", "Status", "Zone"}, {180, 60, 160}, -1);
            return &elements_;
        }
        if (id == "QUEST_TaskList") {
            // CTaskWnd_FillHistoryList @0x76fb90: the list's row is the history index (0x480060(row)).
            auto rows = game::history_rows(ts.completed);
            if (history_selected_ >= static_cast<int>(rows.size())) history_selected_ = -1;
            history_ = task_list_data(rows, {"", "Quest Title", "Completion"}, {40, 220, 200}, history_selected_);
            return &history_;
        }
        if (id == "QUEST_TaskElementList") {
            // CTaskWnd_FillHistoryElements @0x76f700: the selected history entry's elements once received.
            std::vector<game::TaskWindowRow> rows;
            if (history_selected_ >= 0)
                if (auto it = ts.history.find(static_cast<std::uint32_t>(history_selected_)); it != ts.history.end())
                    rows = game::history_element_rows(it->second, c.strings(), text, task_zone_name);
            history_elements_ = task_list_data(rows, {"Objective Instructions", "Status", "Zone"}, {210, 60, 160}, -1);
            return &history_elements_;
        }
        if (id == "STASK_MemberList") {
            // CTaskWnd_FillMemberList @0x770470: each member, "Leader" (0x9e9988) in the role column and the row yellow
            // (0xffff ff00) for the leader. The Template column (the member's monster mission template) stays empty:
            // EQEmu sends no templates.
            members_ = {};
            members_.columns = 3;
            members_.labels = {"Member", "Role", "Template"};
            members_.widths = {130, 100, 200};
            if (member_selected_ >= static_cast<int>(ts.shared_members.size())) member_selected_ = -1;
            members_.selected = member_selected_;
            for (const auto& n : ts.shared_members) {
                bool leader = !ts.shared_leader.empty() && n == ts.shared_leader;
                Rgb col = argb_to_rgb(leader ? 0xffffff00 : 0xffffffff);
                members_.rows.push_back({{n, leader ? "Leader" : "", ""}, col, std::nullopt, {col, col, col}});
            }
            return &members_;
        }
        return nullptr;
    }

    bool list_click(EqHost& host, std::string_view id, int row, bool right, int) override {
        if (right || !host.conn) return false;
        net::GameConnection& c = *host.conn;
        if (id == "TASK_TaskList") {
            // WndNotification 1: a separator selects the row after it; the row's entry fills the objectives and the
            // description, enables Remove, Monster Select (when it has templates) and Preview Reward (bHasRewardSet).
            if (row >= 0 && row < static_cast<int>(rows_.size()) && rows_[static_cast<std::size_t>(row)].separator) ++row;
            if (row < 0 || row >= static_cast<int>(rows_.size())) return true;
            selected_ = row;
            const auto& r = rows_[static_cast<std::size_t>(selected_)];
            c.set_task_window_selection(std::make_pair(r.type, r.index));
            abandon_enabled_ = true;
            const game::Task* t = c.tasks().find(r.type, r.index);
            preview_enabled_ = t && t->reward_set;
            return true;
        }
        if (id == "STASK_MemberList") {
            member_selected_ = row;
            return true;
        }
        if (id == "QUEST_TaskList") {
            // A received entry shows its elements, else OP_TaskHistoryRequest with the row.
            history_selected_ = row;
            if (row >= 0 && !c.tasks().history.contains(static_cast<std::uint32_t>(row)))
                c.request_task_history(static_cast<std::uint32_t>(row));
            return true;
        }
        return false;
    }

    bool list_header_click(EqHost& host, std::string_view id, int column) override {
        // WndNotification 0xf on TASK_TaskList: column 1 sorts by title, 2 by time left, 0 keeps the order; the same
        // order again flips its direction; then CTaskWnd_SortAndFillTaskList.
        if (id != "TASK_TaskList" || !host.conn) return false;
        int key = column == 1 ? 0 : column == 2 ? 1 : sort_key_;
        if (key == sort_key_) sort_ascending_ = !sort_ascending_;
        sort_key_ = key;
        sort_and_fill(*host.conn);
        return true;
    }

    bool stml_link(EqHost& host, std::string_view id, int message, std::string_view data) override {
        // XWM_LINK (27) on TASK_Description: an item link (the reward's) asks the server for the item
        // (ParseItemLinkBody, OP_ItemLinkClick).
        if (id != "TASK_Description" || message != 27 || !host.conn) return false;
        if (auto link = game::parse_item_link(data)) host.conn->request_item(*link);
        return true;
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& c = *host.conn;
        const auto& ts = c.tasks();
        if (id == "TASK_Description") {
            // CTaskWnd_BuildTaskDetailsSTML @0x770200.
            const game::Task* t = selected_task(c);
            out.text = t ? game::task_details_stml(*t, c.strings(),
                                                   [&c](const game::TaskElement& e) { return c.task_element_text(e); })
                         : std::string();
        } else if (id == "TASK_AbandonButton") {
            out.enabled = abandon_enabled_ && selected_task(c) != nullptr;
        } else if (id == "TASK_MonsterButton") {
            out.enabled = false; // the entry's monster mission templates (+0x3be8): EQEmu sends none
        } else if (id == "TASK_PreviewRewardButton") {
            const game::Task* t = selected_task(c);
            out.enabled = preview_enabled_ && t && t->reward_set;
        } else if (id.starts_with("STASK_") && id.ends_with("Button")) {
            // CTaskWnd_FillMemberList: the five buttons are enabled for the task's leader.
            out.enabled = host.conn->shared_task_is_leader();
        } else if (id == "TASK_Summary") {
            // Not a child of the XML: the selected task's kind, timer and reward (OP_TaskDescription) as key=value
            // lines, for UIs that show them beside the objectives (the stock window shows the reward in the
            // description).
            out.text = std::string();
            if (const game::Task* t = selected_task(c)) {
                std::int64_t left = t->duration ? game::task_seconds_left(*t, net::GameConnection::now_ms()) : 0;
                std::string s = std::format("type={}\nduration={}\nleft={}\n", t->type, t->duration, left);
                if (t->has_rewards) {
                    s += std::format("coin={}\npoints={}\n", t->coin_reward, t->points);
                    if (t->xp_reward) s += "xp=1\n";
                    if (t->faction_reward) s += "faction=1\n";
                    if (!t->reward.empty()) s += "item=" + t->reward + "\n";
                }
                out.text = s;
            }
        }
        (void)ts;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        net::GameConnection& c = *host.conn;
        const auto& ts = c.tasks();
        if (id == "TASK_AbandonButton") {
            if (const game::Task* t = selected_task(c)) c.ask_abandon_task(t->type, t->sequence);
            return true;
        }
        if (id == "TASK_PreviewRewardButton") {
            // The row stores a task ID as well as its slot: a replacement must not inherit the previous task's click.
            if (const game::Task* t = selected_task(c); preview_enabled_ && t && t->reward_set)
                c.preview_task_reward(t->task_id);
            return true;
        }
        if (id == "TASK_MonsterButton") {
            // Monster Select needs the entry's monster mission templates, which EQEmu does not send here.
            return true;
        }
        // The shared task page (CTaskWnd::WndNotification @0x7716b0).
        std::optional<std::string> member;
        if (member_selected_ >= 0 && member_selected_ < static_cast<int>(ts.shared_members.size()))
            member = ts.shared_members[static_cast<std::size_t>(member_selected_)];
        if (id == "STASK_AddPlayerButton") c.shared_task_add_target();
        else if (id == "STASK_RemovePlayerButton") c.shared_task_remove_selected(member);
        else if (id == "STASK_MakeLeaderButton") c.shared_task_leader_selected(member);
        else if (id == "STASK_InviteRaidButton") c.shared_task_invite_raid();
        else if (id == "STASK_InviteExpedButton") c.shared_task_invite_expedition();
        else return false;
        return true;
    }
    std::optional<int> tab_page(EqHost&, std::string_view id) override { return id == "TaskSubwindows" ? std::optional<int>(page_) : std::nullopt; }
    void set_tab_page(EqHost&, std::string_view id, int page) override {
        if (id == "TaskSubwindows") page_ = page;
    }

private:
    // The selected row's entry (CTaskManager::GetEntry(index, type)).
    const game::Task* selected_task(const net::GameConnection& c) const {
        if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) return nullptr;
        const auto& r = rows_[static_cast<std::size_t>(selected_)];
        if (r.separator) return nullptr;
        const game::Task* t = c.tasks().find(r.type, r.index);
        return t && t->task_id && t->task_id == r.task_id ? t : nullptr;
    }

    // CTaskWnd_RebuildTaskLists @0x7720e0: the four types' rows collected again, then sorted and listed.
    void rebuild(net::GameConnection& c) {
        std::int64_t now = net::GameConnection::now_ms();
        task_rows_ = game::collect_task_rows(c.tasks(), game::kTaskTypeTask, now);
        quest_rows_ = game::collect_task_rows(c.tasks(), game::kTaskTypeQuest, now);
        shared_rows_ = game::collect_task_rows(c.tasks(), game::kTaskTypeShared, now);
        sort_and_fill(c);
        built_revision_ = c.tasks().revision;
        rebuild_ = false;
        last_timer_s_ = now / 1000;
    }

    // CTaskWnd_SortAndFillTaskList @0x771600: sorted, listed, the selected task found again by its id (else the first
    // row), Remove enabled with a selection, Preview Reward by the entry's flag.
    void sort_and_fill(net::GameConnection& c) {
        game::sort_task_rows(task_rows_, sort_key_, sort_ascending_);
        game::sort_task_rows(quest_rows_, sort_key_, sort_ascending_);
        game::sort_task_rows(shared_rows_, sort_key_, sort_ascending_);
        std::uint32_t was = 0;
        bool had = selected_ >= 0 && selected_ < static_cast<int>(rows_.size());
        if (had) was = rows_[static_cast<std::size_t>(selected_)].task_id;
        rows_ = game::task_list_rows(task_rows_, shared_rows_, quest_rows_, c.strings());
        selected_ = -1;
        if (had)
            for (std::size_t i = 0; i < rows_.size(); ++i)
                if (!rows_[i].separator && rows_[i].task_id == was) selected_ = static_cast<int>(i);
        if (selected_ == -1 && !rows_.empty()) selected_ = 0;
        abandon_enabled_ = selected_ != -1;
        preview_enabled_ = false;
        if (selected_ != -1) {
            const auto& r = rows_[static_cast<std::size_t>(selected_)];
            c.set_task_window_selection(std::make_pair(r.type, r.index));
            const game::Task* t = c.tasks().find(r.type, r.index);
            preview_enabled_ = t && t->reward_set;
        } else {
            c.set_task_window_selection(std::nullopt);
        }
    }

    // CTaskWnd::UpdateTaskTimers @0x76ef90, when Util::FastTime (seconds) has moved on.
    void update_timers(const net::GameConnection& c) {
        std::int64_t now = net::GameConnection::now_ms();
        if (now / 1000 <= last_timer_s_) return;
        last_timer_s_ = now / 1000;
        for (auto& r : rows_) {
            if (r.separator) continue;
            if (const game::Task* t = c.tasks().find(r.type, r.index)) game::update_task_row_timer(r, *t, now);
        }
    }

    std::vector<game::TaskListRow> task_rows_, quest_rows_, shared_rows_; // +0x278, +0x2c8, +0xbd8
    std::vector<game::TaskWindowRow> rows_;
    ListData tasks_, elements_, history_, history_elements_, members_;
    int selected_ = -1, history_selected_ = -1, page_ = 0, member_selected_ = -1;
    int sort_key_ = 0;            // +0x1548: 0 title, 1 time left
    bool sort_ascending_ = true;  // +0x154c
    bool rebuild_ = true;         // +0x154d
    std::uint32_t built_revision_ = 0;
    std::int64_t last_timer_s_ = 0; // +0x1e8 of the last block
    bool abandon_enabled_ = false, preview_enabled_ = false;
};

} // namespace

namespace {

// ---------------------------------------------------------------- tribute (CTributeMasterWnd, CTributeBenefitWnd)

// One row per chosen benefit: its name and tier.
std::string tribute_row(const std::map<std::uint32_t, net::TributeAbility>& abilities, std::uint32_t id, std::uint32_t tier) {
    auto it = abilities.find(id);
    std::string name = it == abilities.end() ? std::format("Tribute {}", id) : it->second.name;
    return std::format("{} (tier {})", name, tier + 1);
}

std::string tribute_description(const std::map<std::uint32_t, std::string>& descriptions, std::uint32_t id) {
    auto it = descriptions.find(id);
    return it == descriptions.end() ? std::string() : it->second;
}

// The guild's two benefits in the personal settings' shape (the rest none).
net::TributeSettings guild_settings(const net::GameConnection::GuildTribute& g) {
    net::TributeSettings s;
    s.active = g.active.enabled;
    for (int i = 0; i < net::kGuildTributeSlots; ++i) {
        s.tributes[static_cast<std::size_t>(i)] = g.active.tributes[static_cast<std::size_t>(i)];
        s.tiers[static_cast<std::size_t>(i)] = g.active.tiers[static_cast<std::size_t>(i)];
    }
    return s;
}

// The tribute master's window (a right click on a tribute master, class 63, or a guild tribute master, class 64;
// the server's OP_OpenTributeMaster / OP_OpenGuildTributeMaster answer opens it). CTributeMasterWnd is not decoded;
// this follows its XML over EQEmu's tribute packets: the available benefits (TMW_AvailableBenefitList: name, the
// first tier's cost and level) and the chosen ones (TMW_ActiveBenefitList: five, or the guild's two), a selected
// one's description (OP_SelectTribute / OP_GuildSelectTribute); Modify Benefits starts changing them, Upgrade adds
// the available benefit or raises the chosen one's tier, Downgrade lowers or drops it, and Modify Benefits again
// sends the change (OP_TributeUpdate; the guild's OP_GuildSaveActiveTributes); the status box turns tribute on or
// off (OP_TributeToggle / OP_GuildTributeToggleReq); donations: an item put in the slot from the cursor
// (OP_TributeItem / OP_GuildTributeDonateItem) or platinum through the coin button (OP_TributeMoney /
// OP_GuildTributeDonatePlat). In guild mode the TMW_Guild* labels show the guild's favour, time and cost.
class TributeMasterWindow : public StockWindow {
public:
    TributeMasterWindow() : StockWindow("TributeMasterWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && (host.conn->tribute().master_id != 0 || guild(host)); }
    void set_shown(EqHost& host, bool show) override {
        if (!show && host.conn) {
            host.conn->close_tribute_master();
            host.conn->close_guild_tribute_master();
            editing_ = false;
        }
    }
    // Closed by the server or by zoning: an unsent edit is dropped, so the window reopens on the server's state.
    void frame(EqHost& host) override {
        const auto serial = !shown(host) ? 0 : guild(host) ? host.conn->guild_tribute().master_serial
                                                                       : host.conn->tribute().master_serial;
        if (serial != master_serial_) {
            master_serial_ = serial;
            editing_ = false;
            available_sel_ = active_sel_ = -1;
            described_ = net::kTributeNone;
            platinum_ = 0;
        }
        if (!shown(host)) editing_ = false;
        if (host.conn && guild(host) != was_guild_) {
            was_guild_ = guild(host);
            editing_ = false;
            available_sel_ = active_sel_ = -1;
            described_ = net::kTributeNone;
        }
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        bool g = guild(host);
        const auto& gt = host.conn->guild_tribute();
        net::TributeSettings now = settings(host);
        const auto& set = editing_ ? edit_ : now;
        std::int32_t donation = g ? gt.last_donation : host.conn->tribute().last_donation;
        if (id == "TMW_Description") out.text = stml_escape(tribute_description(descriptions(host), described_));
        else if (id == "TMW_ValueLabel") out.text = std::to_string(donation);
        else if (id == "TMW_CoinButton") out.text = std::to_string(platinum_);
        else if (id == "TMW_StatusText") out.text = now.active ? "Active" : "Inactive";
        else if (id == "TMW_StatusButton") out.checked = now.active;
        else if (id == "TMW_LockButton") out.text = editing_ ? "Save Benefits" : "Modify Benefits";
        else if (id == "TMW_UpgradeButton") out.enabled = editing_ && (available_sel_ >= 0 || active_sel_ >= 0);
        else if (id == "TMW_DowngradeButton") out.enabled = editing_ && active_sel_ >= 0 && active_sel_ < slots(host) &&
                                                       set.tributes[static_cast<std::size_t>(active_sel_)] != net::kTributeNone;
        else if (id == "TMW_DonateButton") out.enabled = host.conn->inventory().cursor() != nullptr || platinum_ > 0;
        else if (id == "TMW_SelectedItem") {
            const net::Item* held = host.conn->inventory().cursor();
            if (held && host.textures)
                if (auto cell = host.textures->item_icon(held->icon)) {
                    out.decal.kind = ChildArt::Image;
                    out.decal.cell = *cell;
                }
            out.tooltip = held ? held->name : std::string("Hold an item to donate it");
        } else if (id == "TMW_GuildCostLabel") {
            out.text = g ? std::to_string(gt.active_cost()) : std::string();
        } else if (id == "TMW_GuildTimeLabel") {
            std::int64_t left = gt.time_left(net::GameConnection::now_ms());
            out.text = g ? std::format("{}:{:02}", left / 60000, left / 1000 % 60) : std::string();
        } else if (id == "TMW_GuildCurrentPointsLabel") {
            out.text = g ? std::to_string(gt.active.favor) : std::string();
        }
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& abilities = this->abilities(host);
        if (id == "TMW_AvailableBenefitList") {
            available_ = {};
            available_.columns = 3;
            available_.labels = {"Benefit", "Cost", "Level"};
            available_.widths = {170, 50, 40};
            ids_.clear();
            for (const auto& [tid, a] : abilities) {
                available_.rows.push_back(ListRow{{a.name, std::to_string(a.tiers[0].cost), std::to_string(a.tiers[0].level)}});
                ids_.push_back(tid);
            }
            available_.selected = available_sel_;
            return &available_;
        }
        if (id == "TMW_ActiveBenefitList") {
            net::TributeSettings now = settings(host);
            const auto& set = editing_ ? edit_ : now;
            active_ = {};
            active_.labels = {"Benefit"};
            active_.widths = {240};
            for (int i = 0; i < slots(host); ++i) {
                std::uint32_t tid = set.tributes[static_cast<std::size_t>(i)];
                active_.rows.push_back(ListRow{{tid == net::kTributeNone ? std::string("(none)")
                                                                          : tribute_row(abilities, tid, set.tiers[static_cast<std::size_t>(i)])}});
            }
            active_.selected = active_sel_;
            return &active_;
        }
        return nullptr;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool, int) override {
        if (!host.conn) return false;
        if (id == "TMW_AvailableBenefitList") {
            available_sel_ = row;
            active_sel_ = -1;
            if (row >= 0 && row < static_cast<int>(ids_.size())) describe(host, ids_[static_cast<std::size_t>(row)], 0);
            return true;
        }
        if (id == "TMW_ActiveBenefitList") {
            active_sel_ = row;
            available_sel_ = -1;
            net::TributeSettings now = settings(host);
            const auto& set = editing_ ? edit_ : now;
            if (row >= 0 && row < slots(host) && set.tributes[static_cast<std::size_t>(row)] != net::kTributeNone)
                describe(host, set.tributes[static_cast<std::size_t>(row)], set.tiers[static_cast<std::size_t>(row)]);
            return true;
        }
        return false;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        bool g = guild(host);
        net::TributeSettings now = settings(host);
        if (id == "TMW_LockButton") {
            if (editing_) save(c, g);
            else edit_ = now;
            editing_ = !editing_;
        } else if (id == "TMW_UpgradeButton" && editing_) {
            upgrade(host);
        } else if (id == "TMW_DowngradeButton" && editing_) {
            if (active_sel_ >= 0 && active_sel_ < slots(host)) {
                auto k = static_cast<std::size_t>(active_sel_);
                if (edit_.tributes[k] == net::kTributeNone) return true;
                if (edit_.tiers[k] > 0) --edit_.tiers[k];
                else edit_.tributes[k] = net::kTributeNone;
            }
        } else if (id == "TMW_StatusButton") {
            if (g) c.guild_tribute_toggle(!now.active);
            else c.tribute_toggle(!now.active);
        } else if (id == "TMW_DonateButton") {
            if (c.inventory().cursor()) {
                if (g) c.guild_tribute_donate_cursor();
                else c.tribute_donate_cursor();
            } else if (platinum_ > 0 && (g ? c.guild_tribute_donate_platinum(platinum_) : c.tribute_donate_platinum(platinum_))) {
                platinum_ = 0;
            }
        } else if (id == "TMW_CoinButton") {
            game::QuantityOpen q;
            q.max = static_cast<std::int32_t>(std::min<std::uint32_t>(c.coins().platinum, 1000000));
            q.start = static_cast<std::int32_t>(std::min<std::uint32_t>(platinum_, c.coins().platinum));
            const auto* conn = host.conn;
            const auto serial = g ? c.guild_tribute().master_serial : c.tribute().master_serial;
            open_quantity(host, q, [this](std::int32_t v) { platinum_ = static_cast<std::uint32_t>(std::max(0, v)); },
                          [&host, conn, g, serial] {
                              if (host.conn != conn || !conn) return false;
                              return g ? conn->guild_tribute().master_id && conn->guild_tribute().master_serial == serial
                                       : conn->tribute().master_id && conn->tribute().master_serial == serial;
                          });
        } else if (id == "TMW_SelectedItem") {
        } else {
            return false;
        }
        return true;
    }

private:
    static bool guild(EqHost& host) { return host.conn && host.conn->guild_tribute().master_id != 0; }
    static int slots(EqHost& host) { return guild(host) ? net::kGuildTributeSlots : net::kTributeSlots; }
    static const std::map<std::uint32_t, net::TributeAbility>& abilities(EqHost& host) {
        return guild(host) ? host.conn->guild_tribute().abilities : host.conn->tribute().abilities;
    }
    static const std::map<std::uint32_t, std::string>& descriptions(EqHost& host) {
        return guild(host) ? host.conn->guild_tribute().descriptions : host.conn->tribute().descriptions;
    }
    static net::TributeSettings settings(EqHost& host) {
        return guild(host) ? guild_settings(host.conn->guild_tribute()) : host.conn->tribute().settings;
    }
    void describe(EqHost& host, std::uint32_t tid, std::uint32_t tier) {
        described_ = tid;
        if (guild(host)) host.conn->guild_tribute_describe(tid, tier);
        else host.conn->tribute_describe(tid);
    }
    void save(net::GameConnection& c, bool g) {
        if (!g) {
            c.tribute_change(edit_);
            return;
        }
        net::GuildTributeActive a = c.guild_tribute().active;
        for (int i = 0; i < net::kGuildTributeSlots; ++i) {
            a.tributes[static_cast<std::size_t>(i)] = edit_.tributes[static_cast<std::size_t>(i)];
            a.tiers[static_cast<std::size_t>(i)] = edit_.tiers[static_cast<std::size_t>(i)];
        }
        c.guild_tribute_change(a);
    }

    void upgrade(EqHost& host) {
        const auto& abilities = this->abilities(host);
        int n = slots(host);
        if (active_sel_ >= 0 && active_sel_ < n) {
            // A chosen benefit's next tier.
            auto k = static_cast<std::size_t>(active_sel_);
            auto it = abilities.find(edit_.tributes[k]);
            if (it != abilities.end() && edit_.tiers[k] + 1 < it->second.tiers_used())
                ++edit_.tiers[k];
            return;
        }
        if (available_sel_ < 0 || available_sel_ >= static_cast<int>(ids_.size())) return;
        std::uint32_t tid = ids_[static_cast<std::size_t>(available_sel_)];
        for (int k = 0; k < n; ++k)
            if (edit_.tributes[static_cast<std::size_t>(k)] == tid) return; // already chosen
        for (int k = 0; k < n; ++k)
            if (edit_.tributes[static_cast<std::size_t>(k)] == net::kTributeNone) {
                edit_.tributes[static_cast<std::size_t>(k)] = tid;
                edit_.tiers[static_cast<std::size_t>(k)] = 0;
                return;
            }
    }

    bool editing_ = false, was_guild_ = false;
    net::TributeSettings edit_;
    ListData available_, active_;
    std::vector<std::uint32_t> ids_;
    int available_sel_ = -1, active_sel_ = -1;
    std::uint32_t described_ = net::kTributeNone;
    std::uint32_t platinum_ = 0;
    std::uint64_t master_serial_ = 0;
};

// The tribute benefit window (TOGGLE_TRIBUTEBENEFITWIN): the Personal page's chosen benefits (ActiveBenefitList), a
// selected one's description and Activate (OP_TributeToggle: on / off); the labels are EQTypes 116..119
// (app/lua_eq_values.cpp). The Guild page: the guild's benefits (GuildBenefitList) and a description, Opted In?
// (OP_GuildOptInOut's answer for us), Opt Out / Opt In, Activate (OP_GuildTributeToggleReq) and EQTypes 121..123.
// The trophy pages are not built (EQEmu has no RoF2 trophy tribute).
class TributeBenefitWindow : public StockWindow {
public:
    TributeBenefitWindow() : StockWindow("TributeBenefitWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->tribute().benefit_open; }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_tribute_benefit_open(show);
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& t = host.conn->tribute();
        const auto& g = host.conn->guild_tribute();
        bool in_guild = host.conn->guild().in_guild();
        if (id == "ActivateButton") out.text = t.settings.active ? "Deactivate" : "Activate";
        else if (id == "Description") out.text = stml_escape(tribute_description(t.descriptions, described_));
        else if (id == "GuildDescription") out.text = stml_escape(tribute_description(g.descriptions, guild_described_));
        else if (id == "GuildStatusLabel") out.text = g.opted_in.value_or(false) ? "Yes" : "No";
        else if (id == "OptOutButton") {
            out.text = g.opted_in.value_or(false) ? "Opt Out" : "Opt In";
            out.enabled = in_guild;
        } else if (id == "GuildActivateButton") {
            out.text = g.active.enabled ? "Deactivate" : "Activate";
            out.enabled = in_guild;
        } else if (id == "TrophyActivateButton" || id == "TrophyOptOutButton" || id == "GuildTrophyActivateButton")
            out.enabled = false;
    }
    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        bool guild = id == "GuildBenefitList";
        if (!guild && id != "ActiveBenefitList") return nullptr;
        net::TributeSettings set = guild ? guild_settings(host.conn->guild_tribute()) : host.conn->tribute().settings;
        const auto& abilities = guild ? host.conn->guild_tribute().abilities : host.conn->tribute().abilities;
        ListData& l = guild ? guild_list_ : list_;
        auto& ids = guild ? guild_ids_ : ids_;
        std::uint32_t described = guild ? guild_described_ : described_;
        l = {};
        l.labels = {"Benefit"};
        l.widths = {240};
        ids.clear();
        for (std::size_t i = 0; i < set.tributes.size(); ++i) {
            if (set.tributes[i] == net::kTributeNone) continue;
            l.rows.push_back(ListRow{{tribute_row(abilities, set.tributes[i], set.tiers[i])}});
            ids.push_back(set.tributes[i]);
        }
        for (std::size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == described) l.selected = static_cast<int>(i);
        return &l;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool, int) override {
        if (!host.conn) return false;
        if (id == "ActiveBenefitList") {
            if (row >= 0 && row < static_cast<int>(ids_.size())) {
                described_ = ids_[static_cast<std::size_t>(row)];
                host.conn->tribute_describe(described_);
            }
            return true;
        }
        if (id == "GuildBenefitList") {
            if (row >= 0 && row < static_cast<int>(guild_ids_.size())) {
                guild_described_ = guild_ids_[static_cast<std::size_t>(row)];
                host.conn->guild_tribute_describe(guild_described_);
            }
            return true;
        }
        return false;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        if (id == "ActivateButton") host.conn->tribute_toggle(!host.conn->tribute().settings.active);
        else if (id == "GuildActivateButton") host.conn->guild_tribute_toggle(!host.conn->guild_tribute().active.enabled);
        // A deliberate difference: CTributeBenefitWnd @0x784f13 always sends 0 (opt out), so against EQEmu, which sets
        // the state it is sent, the original could never opt back in; this button sends the opposite of our state.
        else if (id == "OptOutButton") host.conn->guild_tribute_opt(!host.conn->guild_tribute().opted_in.value_or(false));
        else return false;
        return true;
    }

private:
    ListData list_, guild_list_;
    std::vector<std::uint32_t> ids_, guild_ids_;
    std::uint32_t described_ = net::kTributeNone, guild_described_ = net::kTributeNone;
};

// The leadership window (TOGGLE_LEADERSHIPWIN). CLeadershipWnd is not decoded; this follows its XML over EQEmu's
// leadership (game/leadership.h): the group and raid abilities (LEList1 / LEList2: name, rank / ranks, the next
// rank's cost), the points to spend (LEW_CurrentGroupCount / LEW_CurrentRaidCount), the experience toward the next
// point (the gauges), earning leadership experience on or off, Purchase for the selected ability, its three hotkey
// buttons (CLeadershipWnd::WndNotification's hot buttons of type 11) and group mentoring.
class LeadershipWindow : public StockWindow {
public:
    LeadershipWindow() : StockWindow("LeadershipWindow", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->leadership().window_open; }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_leadership_window_open(show);
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& l = host.conn->leadership();
        auto gauge = [&](double exp, double per) {
            out.has_gauge = true;
            out.gauge = game::leadership_gauge(exp, per);
            out.gauge_text = std::format("{:.0f} / {:.0f}", exp, per);
        };
        if (id == "LEW_CurrentGroupCount") out.text = std::to_string(l.group_points);
        else if (id == "LEW_CurrentRaidCount") out.text = std::to_string(l.raid_points);
        else if (id == "LEW_GroupExpGauge") gauge(l.group_exp, game::kGroupLeadershipExpPerPoint);
        else if (id == "LEW_RaidExpGauge") gauge(l.raid_exp, game::kRaidLeadershipExpPerPoint);
        else if (id == "LEW_OnButton") out.checked = l.exp_on;
        else if (id == "LEW_OffButton") out.checked = !l.exp_on;
        else if (id == "LEW_PurchaseButton") {
            std::uint32_t cost = selected_ >= 0 ? game::leadership_next_cost(selected_, l.ranks[static_cast<std::size_t>(selected_)]) : 0;
            std::uint32_t points = selected_ >= game::kFirstRaidAbility ? l.raid_points : l.group_points;
            out.enabled = cost > 0 && cost <= points;
        } else if (id == "LEW_Description") {
            out.text = selected_ >= 0 ? description(l, selected_) : std::string();
        } else if (id == "LEW_GroupMentoring") {
            // CLeadershipWnd::AboutToShow: enabled for the group's leader.
            out.enabled = host.conn->group().active && host.conn->group().is_leader(host.conn->character_name());
        } else if (int n = hotkey_button(id)) {
            // CLeadershipWnd_UpdateAbilityDescription: no ability selected disables the three.
            out.enabled = selected_ >= 0 && host.conn->leadership_hot_button_enabled(selected_, n);
        }
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        bool raid = id == "LEList2";
        if (!raid && id != "LEList1") return nullptr;
        const auto& l = host.conn->leadership();
        ListData& d = raid ? raid_ : group_;
        auto& ids = raid ? raid_ids_ : group_ids_;
        d = {};
        d.columns = 3;
        d.labels = {"Ability", "Rank", "Cost"};
        d.widths = {180, 50, 50};
        ids.clear();
        int first = raid ? game::kFirstRaidAbility : 0;
        for (int a = first; a < first + 16; ++a) {
            const auto& ab = game::leadership_ability(a);
            if (ab.name.empty()) continue;
            std::uint32_t rank = l.ranks[static_cast<std::size_t>(a)];
            std::uint32_t cost = game::leadership_next_cost(a, rank);
            d.rows.push_back(ListRow{{std::string(ab.name), std::format("{} / {}", rank, game::leadership_max_rank(a)),
                                      cost ? std::to_string(cost) : std::string("-")}});
            if (rank == 0) d.rows.back().color = argb_to_rgb(0xffa0a0a0);
            if (a == selected_) d.selected = static_cast<int>(ids.size());
            ids.push_back(a);
        }
        return &d;
    }
    bool list_click(EqHost&, std::string_view id, int row, bool, int) override {
        const auto& ids = id == "LEList2" ? raid_ids_ : group_ids_;
        if (id != "LEList1" && id != "LEList2") return false;
        if (row >= 0 && row < static_cast<int>(ids.size())) selected_ = ids[static_cast<std::size_t>(row)];
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        if (id == "LEW_OnButton") c.set_leadership_exp(true);
        else if (id == "LEW_OffButton") c.set_leadership_exp(false);
        else if (id == "LEW_PurchaseButton") {
            if (selected_ >= 0) c.leadership_purchase_button(static_cast<std::uint32_t>(selected_));
        } else if (id == "LEW_DoneButton") c.set_leadership_window_open(false);
        else if (id == "LEW_GroupMentoring") c.set_group_mentor_window_open(true); // the mentor window's Show
        else if (int n = hotkey_button(id)) {
            // CLeadershipWnd::WndNotification @0x6b7bf0: the selected ability as a hot button on the cursor (type 11,
            // ability * 8 + n, its text).
            if (selected_ < 0 || host.cursor_button) return true;
            game::HotButton b;
            b.type = game::HotButtonType::Leadership;
            b.slot = game::leadership_hot_button_value(selected_, n);
            b.label = c.leadership_hot_button_label(selected_, n).substr(0, 63);
            host.cursor_button = b;
            ++host.cursor_serial;
        } else return false;
        return true;
    }

private:
    // LEW_HotkeyButton1..3: 1..3; 0 for another child.
    static int hotkey_button(std::string_view id) {
        if (id.size() != 17 || id.rfind("LEW_HotkeyButton", 0) != 0) return 0;
        int n = id[16] - '0';
        return n >= 1 && n <= 3 ? n : 0;
    }

    static std::string description(const net::GameConnection::Leadership& l, int a) {
        const auto& ab = game::leadership_ability(a);
        std::uint32_t rank = l.ranks[static_cast<std::size_t>(a)];
        std::uint32_t cost = game::leadership_next_cost(a, rank);
        std::string s = std::format("{} ({} leadership)<br>Rank {} of {}", ab.name, a >= game::kFirstRaidAbility ? "raid" : "group",
                                    rank, game::leadership_max_rank(a));
        if (cost) s += std::format("<br>The next rank costs {} {} points.", cost, a >= game::kFirstRaidAbility ? "raid" : "group");
        return s;
    }

    ListData group_, raid_;
    std::vector<int> group_ids_, raid_ids_;
    int selected_ = -1;
};

// Group mentoring (CGroupMentorWnd @0x6746e0, "GroupMentorWindow"): the protege (GM_SelectPlayer: "None" (0x1815), then
// the group's other members that are not mercenaries; CGroupMentorWnd_FillPlayerCombo), the share of experience
// (GM_Slider 0..100 with 101 ticks, GM_SliderInput its number; a number over 100 is put back to 100), and Accept (or
// Enter in the box): the box's number into the slider, the window hidden, then Send_OP_GroupMentor. Showing the
// window refills the list and resets it (CGroupMentorWnd_ResetControls: "None" and 50, or the current protege and
// share).
class GroupMentorWindow : public StockWindow {
public:
    GroupMentorWindow() : StockWindow("GroupMentorWindow", false) {}

    bool shown(EqHost& host) override {
        bool open = host.conn && host.conn->group_mentor().window_open;
        if (open && !was_open_) about_to_show(host);
        was_open_ = open;
        return open;
    }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_group_mentor_window_open(show);
    }
    void child(EqHost&, std::string_view id, Child& out) override {
        if (id == "GM_Slider") {
            out.slider_count = 101;
            out.slider_value = value_;
        } else if (id == "GM_SliderInput") {
            out.text = text_;
        }
    }
    bool set_slider(EqHost&, std::string_view id, int value) override {
        if (id != "GM_Slider") return false;
        value_ = std::clamp(value, 0, 100);
        text_ = std::to_string(value_); // sub_006743e0
        return true;
    }
    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        if (id != "GM_SliderInput") return false;
        text_ = text;
        if (std::atoi(text_.c_str()) > 100) { // sub_006745e0
            value_ = 100;
            text_ = "100";
        }
        return true;
    }
    const ComboData* combo(EqHost&, std::string_view id) override { return id == "GM_SelectPlayer" ? &players_ : nullptr; }
    bool combo_select(EqHost&, std::string_view id, int index) override {
        if (id != "GM_SelectPlayer") return false;
        if (index >= 0 && index < static_cast<int>(players_.choices.size())) players_.selected = index;
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left" || (id != "GM_AcceptButton" && id != "GM_SliderInput")) return false;
        // sub_00674490, Deactivate, Send_OP_GroupMentor.
        value_ = std::clamp(std::atoi(text_.c_str()), 0, 100);
        host.conn->set_group_mentor_window_open(false);
        std::string name = players_.selected > 0 ? players_.choices[static_cast<std::size_t>(players_.selected)] : std::string();
        if (!host.conn->send_group_mentor(name, std::atoi(text_.c_str()))) reset(host);
        return true;
    }

private:
    void about_to_show(EqHost& host) {
        // CGroupMentorWnd::AboutToShow: FillPlayerCombo, ResetControls, the box from the slider.
        players_ = {};
        players_.choices.push_back(host.conn->strings().format(0x1815, {}));
        const auto& g = host.conn->group();
        if (g.active)
            for (const auto& m : g.slots)
                if (!m.name.empty() && !m.merc && m.name != host.conn->character_name()) players_.choices.push_back(m.name);
        reset(host);
    }
    void reset(EqHost& host) {
        players_.selected = 0;
        value_ = 50;
        const auto& mentor = host.conn->group_mentor();
        if (!mentor.name.empty()) {
            for (int i = 0; i < 5 && i < static_cast<int>(players_.choices.size()); ++i)
                if (players_.choices[static_cast<std::size_t>(i)] == mentor.name) {
                    players_.selected = i;
                    break;
                }
            value_ = std::clamp(mentor.percent, 0, 100);
        }
        text_ = std::to_string(value_);
    }

    ComboData players_;
    int value_ = 50;
    std::string text_ = "50";
    bool was_open_ = false;
};

} // namespace

void add_progress_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<TaskSelectWindow>());
    out.push_back(std::make_unique<TaskWindow>());
    out.push_back(std::make_unique<SkillsWindow>());
    out.push_back(std::make_unique<TrainWindow>());
    out.push_back(std::make_unique<AAWindow>());
    out.push_back(std::make_unique<TributeMasterWindow>());
    out.push_back(std::make_unique<TributeBenefitWindow>());
    out.push_back(std::make_unique<LeadershipWindow>());
    out.push_back(std::make_unique<GroupMentorWindow>());
}

} // namespace hailstone::app
