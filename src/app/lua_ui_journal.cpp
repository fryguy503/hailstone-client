// The conversation journal's windows: JournalNPCWnd (CJournalTextWnd, the NPCs and what they said) and
// JournalCatWnd (CJournalCatWnd, the categories), over game/conversation_journal.h.

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "app/lua_ui_windows.h"
#include "core/ascii_case.h"
#include "game/conversation_journal.h"
#include "game/zone_names.h"
#include "net/game_connection.h"
#include "ui/ui_ini.h"

namespace hailstone::app {

namespace fs = std::filesystem;

namespace {

// __ctime32 without its new line.
std::string ctime_text(std::int32_t t) {
    std::time_t tt = static_cast<std::time_t>(t);
    char buf[64] = {};
    std::strftime(buf, sizeof buf, "%a %b %e %H:%M:%S %Y", std::localtime(&tt));
    return buf;
}

// The selection of a list that selects several rows (CListWnd style 0x20000, vf14): none held selects the row
// alone, ctrl toggles it, shift extends from the last one, both clear.
void multi_select(ListData& l, int row, int mods, int& anchor) {
    std::vector<int>& sel = l.multi;
    if (mods == 0) {
        sel = {row};
        anchor = row;
    } else if (mods == 2) {
        auto it = std::find(sel.begin(), sel.end(), row);
        if (it == sel.end()) sel.push_back(row), std::sort(sel.begin(), sel.end());
        else sel.erase(it);
        anchor = row;
    } else if (mods == 1) {
        sel.clear();
        int a = anchor < 0 ? row : anchor;
        for (int r = std::min(a, row); r <= std::max(a, row); ++r) sel.push_back(r);
    } else {
        sel.clear();
    }
    l.selected = row; // CurSel
}

class JournalCatWindow;

// CJournalTextWnd @0x6b4780 (and the CJournalTextWnd_* functions): made by
// CDisplay::InitGameUI only with the Lost Dungeons of Norrath or Gates of Discord expansion (OP_ExpansionInfo bits
// 0x10 / 0x20; TOGGLE_JOURNAL asks the same). The UI ini's JournalLog (0x6b2860) turns the logging on; then the
// journal is read at once (CJournalTextWnd_LoadJournalFiles: here the character folder's CJ.hdr / CJ.txt, the
// original's userdata\CJ_<name>_<server>) and each NPC line OP_SpecialMesg sends for the journal is kept
// (0x6b3c00: the NPC by name and zone, now, where, the line, the category window's default category).
// The window: EnableButton (logging: 6726 on, which reloads; 6727 off, which saves and empties it), SearchButton
// (a search: the text entry 3186, 32 characters; again clears it; 3185 or the search's first 7 characters and
// "..."), CategoryFilter ("ALL" 13205 and the categories), NPCList (name, zone, the newest time; sorted by name, zone
// or time, ascending first, a second click on the column reverses), DialogList (time, category or "None" 1216, the
// first 64 characters; by time or category), Text (the entry's line), Delete NPC, Delete Entry, CategorySelect (the
// entries selected get that category), Edit Categories (the category window) and Save (CJournalTextWnd_SaveJournal:
// at most once an hour the files roll to CJ1..CJ3 first). SaveGameUI saves the journal when the character leaves
// the world.
class JournalNpcWindow : public StockWindow {
public:
    JournalNpcWindow() : StockWindow("JournalNPCWnd", false) {
        npcs_.columns = 3;
        npcs_.labels = {"NPC Name", "Zone", "Most Recent"};
        npcs_.widths = {160, 120, 160};
        dialog_.columns = 3;
        dialog_.labels = {"Most Recent", "Category", "Preview"};
        dialog_.widths = {150, 100, 350};
    }
    void set_categories(JournalCatWindow* c) { cats_ = c; }
    game::ConversationJournal& journal() { return journal_; }
    bool logging() const { return logging_; }
    std::int32_t default_category() const;

    void ini_loaded(UiHost& host, ui::UiIni& ini, const fs::path& user_dir) override {
        if (!base_.empty() && logging_) save(host); // the character before
        ini_ = &ini;
        base_ = user_dir.empty() ? fs::path() : user_dir / "CJ";
        std::string v = ini.get("JournalNPCWnd", "JournalLog").value_or("0");
        logging_ = !v.empty() && (v[0] == '1' || v[0] == 't' || v[0] == 'T');
        write_backups_ = true;
        next_backup_ms_ = 0;
        journal_.clear();
        clear_dialog();
        npc_rows_.clear();
        cats_reset(host);
        if (logging_) load(host);
    }
    void set_shown(UiHost& host, bool show) override {
        if (show && !open_) {
            if (!exists(host)) return;
            search_.clear(); // AboutToShow
            refresh_all(host, false);
        }
        open_ = show;
    }
    void frame(UiHost& host) override {
        if (host.conn) {
            auto lines = host.conn->take_journal_lines();
            if (exists(host) && logging_) {
                for (const auto& l : lines)
                    journal_.add(l.npc, l.zone, static_cast<std::int32_t>(std::time(nullptr)), l.x, l.y, l.z, l.text,
                                 default_category());
                if (!lines.empty()) refresh_all(host, true);
            }
        }
        if (was_in_game_ && !host.in_game && logging_) save(host); // SaveGameUI
        was_in_game_ = host.in_game;
    }
    void child(UiHost& host, std::string_view id, Child& out) override {
        if (id == "EnableButton") out.checked = logging_;
        else if (id == "DeleteNPCButton") out.enabled = !npcs_.multi.empty();
        else if (id == "DeleteTextButton") out.enabled = !dialog_.multi.empty();
        else if (id == "SearchButton") {
            out.checked = !search_.empty();
            if (search_.empty()) out.text = host.conn ? host.conn->strings().format(3185, {}) : std::string("Search");
            else out.text = search_.size() > 8 ? search_.substr(0, 7) + "..." : search_;
        } else if (id == "Text") {
            out.text = text_;
        }
    }
    const ListData* list(UiHost&, std::string_view id) override {
        if (id == "NPCList") return &npcs_;
        if (id == "DialogList") return &dialog_;
        return nullptr;
    }
    bool list_select(UiHost& host, std::string_view id, int row, int mods) override {
        if (id == "NPCList" && row >= 0 && row < static_cast<int>(npc_rows_.size())) {
            multi_select(npcs_, row, mods, npc_anchor_);
            select_npc(host, row);
        } else if (id == "DialogList" && row >= 0 && row < static_cast<int>(entry_rows_.size())) {
            multi_select(dialog_, row, mods, dialog_anchor_);
            show_text(host, row);
        } else {
            return false;
        }
        return true;
    }
    bool list_click(UiHost& host, std::string_view id, int row, bool right, int) override {
        return !right && list_select(host, id, row, 0);
    }
    bool list_header_click(UiHost& host, std::string_view id, int column) override {
        if (id == "NPCList") {
            int sort = column == 0 ? 0 : column == 1 ? 2 : 3;
            npc_asc_ = npc_sort_ == sort ? !npc_asc_ : true;
            npc_sort_ = sort;
            sort_npcs();
            fill_npc_list(host, false);
        } else if (id == "DialogList") {
            int sort = column == 1 ? 2 : 0;
            dialog_asc_ = dialog_sort_ == sort ? !dialog_asc_ : true;
            dialog_sort_ = sort;
            sort_entries();
            fill_dialog_list(host, false);
        } else {
            return false;
        }
        return true;
    }
    const ComboData* combo(UiHost&, std::string_view id) override {
        if (id == "CategoryFilter") return &filter_combo_;
        if (id == "CategorySelect") return &select_combo_;
        return nullptr;
    }
    bool combo_select(UiHost& host, std::string_view id, int index) override {
        if (id == "CategoryFilter") {
            filter_combo_.selected = index;
            if (index <= 0) {
                filter_ = -1;
            } else {
                const auto& cats = journal_.categories();
                const std::string& name = index - 1 < static_cast<int>(cats.size()) ? cats[static_cast<std::size_t>(index - 1)]->name : std::string();
                game::JournalCategory* c = journal_.category_named(name);
                filter_ = c ? c->id : -1;
            }
            refresh_all(host, false);
            return true;
        }
        if (id == "CategorySelect") {
            select_combo_.selected = index;
            // 0x47a640(choice - 1): the category whose id is the choice's place
            game::JournalCategory* c = journal_.category(index - 1);
            std::int32_t cat = c ? c->id : -1;
            for (int r : dialog_.multi)
                if (r >= 0 && r < static_cast<int>(entry_rows_.size())) entry_rows_[static_cast<std::size_t>(r)]->category = cat;
            rebuild_entries(); // sub_006b3800(1)
            sort_entries();
            fill_dialog_list(host, true);
            return true;
        }
        return false;
    }
    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override;

    void refresh_all(UiHost& host, bool keep);
    void fill_category_combos(UiHost& host);
    void save(UiHost& host);

private:
    bool exists(UiHost& host) const { return host.conn && (host.conn->world_expansions() & 0x30) != 0; }
    void load(UiHost& host) {
        if (base_.empty()) return;
        int r = journal_.load(base_);
        if (r < -1) write_backups_ = false;
        refresh_all(host, false);
        cats_reset(host); // 0x6b1580(0)
    }
    void cats_reset(UiHost& host);
    // 0x6b3360: the NPCs with an entry in the filter's category and, with a search, one holding it.
    void rebuild_npcs() {
        npc_rows_.clear();
        std::string upper = game::journal_upper(search_.substr(0, 0x7f));
        for (const auto& n : journal_.npcs()) {
            if (filter_ != -1 && !n->entry_in_category(filter_)) continue;
            if (!search_.empty() && journal_.find_text(*n, upper, 0) == -1) continue;
            npc_rows_.push_back(n.get());
        }
    }
    // 0x6b2bf0: by name (0), zone id (1), zone name (2) or the newest time (3).
    void sort_npcs() {
        auto key_less = [this](const game::JournalNpc* a, const game::JournalNpc* b) {
            switch (npc_sort_) {
            case 0: return icompare(a->name, b->name) < 0;
            case 1: return a->zone < b->zone;
            case 2: return icompare(game::zone_long_name(static_cast<std::uint32_t>(a->zone)),
                                    game::zone_long_name(static_cast<std::uint32_t>(b->zone))) < 0;
            default: return a->latest < b->latest;
            }
        };
        if (npc_asc_) std::stable_sort(npc_rows_.begin(), npc_rows_.end(), key_less);
        else std::stable_sort(npc_rows_.begin(), npc_rows_.end(), [&](auto* a, auto* b) { return key_less(b, a); });
    }
    // 0x6b3480: the current NPC's entries, with a search those holding it.
    void rebuild_entries() {
        entry_rows_.clear();
        if (!current_) return;
        std::string upper = game::journal_upper(search_.substr(0, 0x7f));
        for (std::size_t i = 0; i < current_->entries.size(); ++i) {
            if (!search_.empty()) {
                int f = journal_.find_text(*current_, upper, i);
                if (f == -1) break;
                i = static_cast<std::size_t>(f);
            }
            entry_rows_.push_back(current_->entries[i].get());
        }
    }
    // 0x6b2c90: by time (0) or category (1, 2).
    void sort_entries() {
        auto key_less = [this](const game::JournalEntry* a, const game::JournalEntry* b) {
            return dialog_sort_ == 0 ? a->time < b->time : a->category < b->category;
        };
        if (dialog_asc_) std::stable_sort(entry_rows_.begin(), entry_rows_.end(), key_less);
        else std::stable_sort(entry_rows_.begin(), entry_rows_.end(), [&](auto* a, auto* b) { return key_less(b, a); });
    }
    void fill_npc_list(UiHost& host, bool keep) {
        npcs_.rows.clear();
        npcs_.multi.clear();
        npcs_.selected = -1;
        int sel = -1;
        for (std::size_t i = 0; i < npc_rows_.size(); ++i) {
            const auto* n = npc_rows_[i];
            npcs_.rows.push_back(ListRow{{n->name, game::zone_long_name(static_cast<std::uint32_t>(n->zone)),
                                          n->latest == 0 ? std::string() : ctime_text(n->latest)}});
            if (keep && n == current_) sel = static_cast<int>(i);
        }
        if (sel != -1) npcs_.selected = sel, npcs_.multi = {sel};
        select_npc(host, sel);
    }
    // 0x6b3840
    void select_npc(UiHost& host, int row) {
        if (row == -1) {
            current_ = nullptr;
            dialog_.rows.clear();
            dialog_.multi.clear();
            dialog_.selected = -1;
            entry_rows_.clear();
            text_.clear();
            selected_entry_ = nullptr;
            return;
        }
        game::JournalNpc* n = npc_rows_[static_cast<std::size_t>(row)];
        if (n != current_) {
            current_ = n;
            rebuild_entries();
            sort_entries();
            fill_dialog_list(host, false);
        }
    }
    void fill_dialog_list(UiHost& host, bool keep) {
        dialog_.rows.clear();
        dialog_.multi.clear();
        dialog_.selected = -1;
        int sel = -1;
        std::string none = host.conn ? host.conn->strings().format(1216, {}) : std::string("None");
        for (std::size_t i = 0; i < entry_rows_.size(); ++i) {
            const auto* e = entry_rows_[i];
            const game::JournalCategory* c = journal_.category(e->category);
            dialog_.rows.push_back(ListRow{{ctime_text(e->time), c ? c->name : none, journal_.text(e->text).substr(0, 0x40)}});
            if (keep && e == selected_entry_) sel = static_cast<int>(i);
        }
        if (sel != -1) dialog_.selected = sel, dialog_.multi = {sel};
        if (keep) show_text(host, sel);
    }
    // CJournalTextWnd_ShowDialogText
    void show_text(UiHost& host, int row) {
        if (row == -1) {
            selected_entry_ = nullptr;
            return;
        }
        game::JournalEntry* e = entry_rows_[static_cast<std::size_t>(row)];
        text_ = journal_.text(e->text);
        int choice = 0;
        const auto& cats = journal_.categories();
        for (std::size_t i = 0; i < cats.size(); ++i)
            if (cats[i]->id == e->category) choice = static_cast<int>(i) + 1;
        select_combo_.selected = choice;
        selected_entry_ = e;
        (void)host;
    }
    // sub_006b2950: the dialog list and text emptied.
    void clear_dialog() {
        dialog_.multi.clear();
        dialog_.rows.clear();
        dialog_.selected = -1;
        text_.clear();
        current_ = nullptr;
        selected_entry_ = nullptr;
        entry_rows_.clear();
    }

    game::ConversationJournal journal_;
    JournalCatWindow* cats_ = nullptr;
    ui::UiIni* ini_ = nullptr;
    fs::path base_;
    bool logging_ = false, write_backups_ = true, was_in_game_ = false;
    std::uint64_t next_backup_ms_ = 0;
    static constexpr int kBackupMinutes = 60, kBackups = 3;
    std::vector<game::JournalNpc*> npc_rows_;
    std::vector<game::JournalEntry*> entry_rows_;
    game::JournalNpc* current_ = nullptr;
    game::JournalEntry* selected_entry_ = nullptr;
    int npc_sort_ = 3, dialog_sort_ = 0;
    bool npc_asc_ = true, dialog_asc_ = true;
    std::int32_t filter_ = -1;
    std::string search_, last_search_, text_;
    ListData npcs_, dialog_;
    ComboData filter_combo_, select_combo_;
    int npc_anchor_ = -1, dialog_anchor_ = -1;
};

// CJournalCatWnd @0x6b0f80: CategoryList (Name 100, Description 250; sorted by
// name or, its second column clicked, description; again reverses), DefaultChooser ("None" 1216 and the categories:
// the category new lines get, the UI ini's DefaultCategory), NameInput (15 characters), DescInput (255), Add (a new
// category with the lowest free id; 3182 when the name is taken), Update (the selected one's name and
// description), Delete (the selected ones; their entries keep no category). AboutToShow empties the inputs and the
// selection. The chooser takes its choice's place in the sorted list (as the original does), and shows "None" each
// time it is filled unless its text names a category.
class JournalCatWindow : public StockWindow {
public:
    explicit JournalCatWindow(JournalNpcWindow* text) : StockWindow("JournalCatWnd", false), text_(text) {
        list_.columns = 2;
        list_.labels = {"Name", "Description"};
        list_.widths = {100, 250};
    }
    std::int32_t default_category() const { return default_; }
    // The journal changed under it (emptied or loaded): 0x6b0690 / 0x6b1580(0).
    void reset(UiHost& host) {
        selected_ = nullptr;
        list_.multi.clear();
        list_.selected = -1;
        rebuild(host, false);
    }

    void ini_loaded(UiHost&, ui::UiIni& ini, const fs::path&) override {
        ini_ = &ini;
        default_ = std::atoi(ini.get("JournalCatWnd", "DefaultCategory").value_or("-1").c_str());
    }
    void set_shown(UiHost& host, bool show) override {
        if (show && !open_) { // AboutToShow: 0x6b0690, 0x6b1580(0)
            selected_ = nullptr;
            list_.multi.clear();
            chooser_ = {};
            name_.clear();
            desc_.clear();
            rebuild(host, false);
        }
        open_ = show;
    }
    void child(UiHost&, std::string_view id, Child& out) override {
        if (id == "DeleteButton") out.enabled = !list_.multi.empty();
        else if (id == "UpdateButton") out.enabled = selected_ != nullptr;
        else if (id == "AddButton") out.enabled = !name_.empty();
        else if (id == "NameInput") out.text = name_;
        else if (id == "DescInput") out.text = desc_;
    }
    bool set_text(UiHost&, std::string_view id, const std::string& text) override {
        if (id == "NameInput") name_ = text.substr(0, 0xf);
        else if (id == "DescInput") desc_ = text.substr(0, 0xff);
        else return false;
        return true;
    }
    const ListData* list(UiHost&, std::string_view id) override { return id == "CategoryList" ? &list_ : nullptr; }
    bool list_select(UiHost&, std::string_view id, int row, int mods) override {
        if (id != "CategoryList" || row < 0 || row >= static_cast<int>(rows_.size())) return false;
        multi_select(list_, row, mods, anchor_);
        selected_ = rows_[static_cast<std::size_t>(row)];
        name_ = selected_->name; // 0x6b0940
        desc_ = selected_->description;
        return true;
    }
    bool list_click(UiHost& host, std::string_view id, int row, bool right, int) override {
        return !right && list_select(host, id, row, 0);
    }
    bool list_header_click(UiHost& host, std::string_view id, int column) override {
        if (id != "CategoryList") return false;
        int sort = column == 1 ? 1 : 0;
        asc_ = sort_ == sort ? !asc_ : true;
        sort_ = sort;
        sort_rows();
        fill(host, true);
        return true;
    }
    const ComboData* combo(UiHost&, std::string_view id) override { return id == "DefaultChooser" ? &chooser_ : nullptr; }
    bool combo_select(UiHost&, std::string_view id, int index) override {
        if (id != "DefaultChooser") return false;
        chooser_.selected = index;
        if (index <= 0) default_ = -1;
        else if (index - 1 < static_cast<int>(rows_.size())) default_ = rows_[static_cast<std::size_t>(index - 1)]->id;
        if (ini_) { // sub_006b0880
            ini_->put("JournalCatWnd", "DefaultCategory", std::to_string(default_));
            ini_->save();
        }
        return true;
    }
    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (button != "left" || !text_) return false;
        game::ConversationJournal& j = text_->journal();
        if (id == "DeleteButton") {
            for (int r : list_.multi)
                if (r >= 0 && r < static_cast<int>(rows_.size())) j.remove_category(rows_[static_cast<std::size_t>(r)]->id);
            selected_ = nullptr;
            rebuild(host, false);
            text_->refresh_all(host, true);
        } else if (id == "AddButton") {
            std::string name = name_.empty() ? std::string("Blank") : name_;
            if (!j.category_named(name)) {
                j.set_category(name, desc_, -1);
                rebuild(host, false);
                text_->fill_category_combos(host);
            } else if (host.conn) {
                host.conn->notice(host.conn->strings().format(3182, {})); // ConfirmationDialog(0, 0xb, 3182, OK)
            }
        } else if (id == "UpdateButton") {
            if (selected_) {
                selected_->name = (name_.empty() ? std::string("Blank") : name_).substr(0, 0xf);
                selected_->description = desc_.substr(0, 0xff);
                rebuild(host, true);
                text_->fill_category_combos(host);
            }
        } else {
            return false;
        }
        return true;
    }

private:
    // 0x6b1580: the categories sorted, the list and chooser filled.
    void rebuild(UiHost& host, bool keep) {
        rows_.clear();
        if (text_)
            for (const auto& c : text_->journal().categories()) rows_.push_back(c.get());
        sort_rows();
        fill(host, keep);
    }
    void sort_rows() {
        auto key_less = [this](const game::JournalCategory* a, const game::JournalCategory* b) {
            return sort_ == 1 ? icompare(a->description, b->description) < 0 : icompare(a->name, b->name) < 0;
        };
        if (asc_) std::stable_sort(rows_.begin(), rows_.end(), key_less);
        else std::stable_sort(rows_.begin(), rows_.end(), [&](auto* a, auto* b) { return key_less(b, a); });
    }
    // CJournalCatWnd_FillCategoryList
    void fill(UiHost& host, bool keep) {
        int sel = keep ? list_.selected : -1;
        list_.rows.clear();
        list_.multi.clear();
        for (const auto* c : rows_) list_.rows.push_back(ListRow{{c->name, c->description}});
        list_.selected = sel < static_cast<int>(rows_.size()) ? sel : -1;
        if (list_.selected >= 0) {
            list_.multi = {list_.selected};
            name_ = rows_[static_cast<std::size_t>(list_.selected)]->name;
            desc_ = rows_[static_cast<std::size_t>(list_.selected)]->description;
        } else if (keep) {
            name_.clear();
            desc_.clear();
        }
        std::string current = chooser_.selected >= 0 && chooser_.selected < static_cast<int>(chooser_.choices.size())
                                  ? chooser_.choices[static_cast<std::size_t>(chooser_.selected)]
                                  : std::string();
        chooser_.choices = {host.conn ? host.conn->strings().format(1216, {}) : std::string("None")};
        chooser_.selected = 0;
        if (text_) {
            const auto& cats = text_->journal().categories();
            for (std::size_t i = 0; i < cats.size(); ++i) {
                chooser_.choices.push_back(cats[i]->name);
                if (cats[i]->name == current) chooser_.selected = static_cast<int>(i) + 1;
            }
        }
    }

    JournalNpcWindow* text_;
    ui::UiIni* ini_ = nullptr;
    std::vector<game::JournalCategory*> rows_;
    game::JournalCategory* selected_ = nullptr;
    ListData list_;
    ComboData chooser_;
    std::string name_, desc_;
    std::int32_t default_ = -1;
    int sort_ = 0, anchor_ = -1;
    bool asc_ = true;
    friend class JournalNpcWindow;
};

std::int32_t JournalNpcWindow::default_category() const { return cats_ ? cats_->default_category() : -1; }

void JournalNpcWindow::cats_reset(UiHost& host) {
    if (cats_) cats_->reset(host);
}

void JournalNpcWindow::refresh_all(UiHost& host, bool keep) {
    rebuild_npcs();
    sort_npcs();
    fill_npc_list(host, keep);
    rebuild_entries();
    sort_entries();
    fill_dialog_list(host, keep);
    fill_category_combos(host);
}

// CJournalTextWnd_FillCategoryCombos
void JournalNpcWindow::fill_category_combos(UiHost& host) {
    const auto& cats = journal_.categories();
    filter_combo_.choices = {host.conn ? host.conn->strings().format(13205, {}) : std::string("ALL")};
    select_combo_.choices = {host.conn ? host.conn->strings().format(1216, {}) : std::string("None")};
    int pick = 0;
    for (std::size_t i = 0; i < cats.size(); ++i) {
        filter_combo_.choices.push_back(cats[i]->name);
        select_combo_.choices.push_back(cats[i]->name);
        if (selected_entry_ && cats[i]->id == selected_entry_->category) pick = static_cast<int>(i) + 1;
    }
    filter_combo_.selected = filter_ + 1; // SetChoice(filter + 1)
    select_combo_.selected = pick;
}

// CJournalTextWnd_SaveJournal / _WriteJournalFiles
void JournalNpcWindow::save(UiHost&) {
    if (base_.empty()) return;
    std::uint64_t now = static_cast<std::uint64_t>(std::time(nullptr)) * 1000;
    if (write_backups_ && next_backup_ms_ < now) {
        std::error_code ec;
        // <base><n><ext> joined as a path (base_.string() throws on Windows for a name outside the ANSI code page).
        auto file = [&](const std::string& n, const char* ext) {
            fs::path p = base_;
            p += n + ext;
            return p;
        };
        std::string to = std::to_string(kBackups);
        fs::remove(file(to, ".txt"), ec);
        fs::remove(file(to, ".hdr"), ec);
        for (int i = kBackups - 1; i >= 0; --i) {
            std::string from = i == 0 ? std::string() : std::to_string(i);
            fs::rename(file(from, ".txt"), file(to, ".txt"), ec);
            fs::rename(file(from, ".hdr"), file(to, ".hdr"), ec);
            to = from;
        }
        next_backup_ms_ = now + static_cast<std::uint64_t>(kBackupMinutes) * 60000;
    }
    journal_.save(base_);
}

bool JournalNpcWindow::click(UiHost& host, std::string_view id, std::string_view button, bool) {
    if (button != "left") return false;
    if (id == "EnableButton") {
        logging_ = !logging_;
        if (ini_) {
            ini_->put("JournalNPCWnd", "JournalLog", logging_ ? "1" : "0"); // sub_006b28f0
            ini_->save();
        }
        if (!logging_) {
            if (host.conn) host.conn->show_eqstr(6727, {}, 0xf);
            logging_ = true; // saved while still logging
            save(host);
            logging_ = false;
            clear_dialog();
            journal_.clear();
            cats_reset(host); // 0x6b0690
            refresh_all(host, false);
        } else {
            if (host.conn) host.conn->show_eqstr(6726, {}, 0xf);
            journal_.clear();
            clear_dialog();
            cats_reset(host);
            load(host);
        }
    } else if (id == "SaveButton") {
        if (logging_) save(host);
    } else if (id == "DeleteNPCButton") {
        for (int r : npcs_.multi)
            if (r >= 0 && r < static_cast<int>(npc_rows_.size())) {
                const game::JournalNpc* n = npc_rows_[static_cast<std::size_t>(r)];
                journal_.remove_npc(std::string(n->name), n->zone);
            }
        clear_dialog();
        refresh_all(host, false);
    } else if (id == "DeleteTextButton") {
        if (current_)
            for (int r : dialog_.multi)
                if (r >= 0 && r < static_cast<int>(entry_rows_.size())) current_->remove_entry(entry_rows_[static_cast<std::size_t>(r)]->text);
        dialog_.multi.clear();
        selected_entry_ = nullptr;
        refresh_all(host, false);
    } else if (id == "CategoryButton") {
        if (cats_) cats_->set_shown(host, true);
    } else if (id == "SearchButton") {
        if (!search_.empty()) {
            last_search_ = search_;
            search_.clear();
            refresh_all(host, true);
        } else {
            UiHost::TextEntry e;
            e.prompt = host.conn ? host.conn->strings().format(3186, {}) : std::string();
            e.text = last_search_;
            e.max_chars = 0x20;
            e.done = [this, &host](const std::string& text) {
                search_ = text;
                refresh_all(host, false);
            };
            host.text_entry = std::move(e);
        }
    } else {
        return false;
    }
    return true;
}

} // namespace

void add_journal_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    auto text = std::make_unique<JournalNpcWindow>();
    auto cats = std::make_unique<JournalCatWindow>(text.get());
    text->set_categories(cats.get());
    out.push_back(std::move(text));
    out.push_back(std::move(cats));
}

} // namespace hailstone::app
