// The pre-game screens in the stock layouts: serverselect (EQLSUI_ServerSelectWnd.xml, eqmain.dll's server select)
// over the client's world list (ui::ServerSelectView, the login connection's), and CharacterCreation
// (EQUI_CharacterCreate.xml, CCharacterCreation) over world's creation tables.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <utility>

#include <imgui.h>

#include "app/customization_actor.h"
#include "app/lua_ui_windows.h"
#include "game/char_create_look.h"
#include "game/eq_names.h"
#include "game/zone_names.h"
#include "net/game_connection.h"
#include "net/world_packets.h"
#include "ui/dev_screens.h"

namespace hailstone::app {

namespace {

// eqmain.dll's server select (not analysed: the window is laid out from its XML and acts as the client's own server
// select does): ServerList (Server 400, Status 100: the worlds the login server lists, a world that cannot be joined
// in red), PlayButton (the selected world, when it can be joined), ExitButton ("LOGOUT": back to the login screen),
// and LastServerLabel / PlayLastServerButton, the world played last on this login server (hidden without one).
class ServerSelectWindow : public StockWindow {
public:
    ServerSelectWindow() : StockWindow("serverselect", false) {}

    bool shown(UiHost& host) override {
        bool now = host.server_select != nullptr;
        if (now && !was_shown_) { // the full-screen 640 x 480 page, centred
            ImVec2 ds = ImGui::GetIO().DisplaySize;
            move_ = std::array<int, 2>{(static_cast<int>(ds.x) - 640) / 2, (static_cast<int>(ds.y) - 480) / 2};
        }
        was_shown_ = now;
        return now;
    }
    void set_shown(UiHost&, bool) override {}
    bool pre_game() const override { return true; }
    std::optional<std::array<int, 2>> take_move(UiHost&) override { return std::exchange(move_, std::nullopt); }
    void child(UiHost& host, std::string_view id, Child& out) override {
        const ui::ServerSelectView* v = host.server_select;
        if (!v) return;
        if (id == "PlayButton") {
            out.enabled = !v->busy && selected(*v) && selected(*v)->can_join;
        } else if (id == "LastServerLabel") {
            out.shown = last(host) >= 0;
        } else if (id == "PlayLastServerButton") {
            int i = last(host);
            out.shown = i >= 0;
            if (i >= 0) {
                out.text = v->servers[static_cast<std::size_t>(i)].name;
                out.enabled = !v->busy && v->servers[static_cast<std::size_t>(i)].can_join;
            }
        } else if (id == "ExitButton") {
            out.enabled = true;
        }
    }
    const ListData* list(UiHost& host, std::string_view id) override {
        if (id != "ServerList" || !host.server_select) return nullptr;
        const ui::ServerSelectView& v = *host.server_select;
        list_ = {};
        list_.columns = 2;
        list_.labels = {"Server:", "Status:"};
        list_.widths = {400, 100};
        for (const auto& s : v.servers) {
            ListRow r{{s.name, s.status}}; // Up, Down or Locked: no player count
            if (!s.can_join || s.status == "Down") r.color = Rgb{1.0f, 0.3f, 0.3f};
            list_.rows.push_back(std::move(r));
        }
        list_.selected = v.selected;
        return &list_;
    }
    bool list_click(UiHost& host, std::string_view id, int row, bool right, int) override {
        if (id != "ServerList" || !host.server_select || right) return false;
        ui::ServerSelectView& v = *host.server_select;
        if (row < 0 || row >= static_cast<int>(v.servers.size()) || v.busy) return true;
        v.selected = row;
        return true;
    }
    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (button != "left" || !host.server_select) return false;
        ui::ServerSelectView& v = *host.server_select;
        if (id == "PlayButton") {
            if (!v.busy && selected(v) && selected(v)->can_join)
                host.server_select_action = static_cast<int>(ui::ServerSelectAction::Play);
        } else if (id == "PlayLastServerButton") {
            int i = last(host);
            if (i >= 0 && !v.busy && v.servers[static_cast<std::size_t>(i)].can_join) {
                v.selected = i;
                host.server_select_action = static_cast<int>(ui::ServerSelectAction::Play);
            }
        } else if (id == "ExitButton") {
            host.server_select_action = static_cast<int>(ui::ServerSelectAction::Back);
        } else {
            return false;
        }
        return true;
    }

private:
    static const ui::ServerEntry* selected(const ui::ServerSelectView& v) {
        return v.selected >= 0 && v.selected < static_cast<int>(v.servers.size()) ? &v.servers[static_cast<std::size_t>(v.selected)]
                                                                                 : nullptr;
    }
    static int last(UiHost& host) {
        if (host.last_server.empty() || !host.server_select) return -1;
        const auto& s = host.server_select->servers;
        for (std::size_t i = 0; i < s.size(); ++i)
            if (s[i].name == host.last_server) return static_cast<int>(i);
        return -1;
    }
    ListData list_;
    bool was_shown_ = false;
    std::optional<std::array<int, 2>> move_;
};

// ---------------------------------------------------------------- CharacterCreation (CCharacterCreation)

// CCharacterCreation @0x64e2e0 over
// world's creation tables (GameConnection::char_create_data, OP_CharacterCreateRequest; CharCreateCombos):
// - CCharacterCreation_InitRaceClassButtons @0x64e5b0: for each race of the combinations the buttons
//   CC_Race_<race name>_Button (female) and _Button_Male and an overlay; for each class CC_Class_<class name>_Button.
//   UpdateRaceButtons @0x64c190: the race's button for the gender shown (the other hidden), the current race's
//   checked; the overlays (races and classes the free-to-play state locks) hidden: nothing is locked here.
//   UpdateClassButtons @0x64c4a0: the classes of the race's combinations enabled, the current one checked.
// - A race clicked: CCharacterCreation_SetRace @0x64eeb0 (then RandomizeClass @0x64e060: a random class of the
//   race's that the account's expansions allow, RandomizeDeity @0x64d970: a random deity of race and class, its start
//   city kept when the combination still exists, else the first one that does (0x64bee0), and the stats reset).
//   A class clicked: the class (sub_0064dff0), RandomizeDeity, the deity combo, the stats reset. A gender clicked
//   (sub_0064d930): the gender; the race buttons follow it.
// - CC_Deity_Combo (CCharacterCreation_FillDeityCombo @0x64c800: the deities of race and class sorted by name,
//   GetDeityDesc) and CC_City_Combo (FillStartCityCombo @0x64ca10: the start zones of race, class and deity sorted by
//   their names): a choice sets the deity (and the city again, 0x64d8e0) or the city.
// - The help box: a race's dbstr type 8 text, a class's 9, a stat's 10, a deity's 14, a city's 15, with its name in
//   CC_Help_Label (sub_0064cc50, sub_0064cec0, sub_00649710, sub_00649850, sub_00649970); a right click on a race or
//   class button shows its help without choosing it.
// - The stats (CC_AdvancedScreen, shown while CC_Advanced_Button is checked; CC_Advanced_OK_Button unchecks it):
//   CCharacterCreation_ResetStatPoints / sub_0064d670 puts each stat at the allocation's base, the points to spend at
//   the sum of its default points (a stat with default points is shown in green); Up (sub_0064d770) adds one up to
//   base + 25 (at most 150), Down (sub_0064d870) takes one back down to the base, a right click on the stat's button
//   (sub_0064d7f0) spends as much as it can; CCA_Points_Left_Display_Button shows what is left (0x649d50) and the Up buttons are
//   enabled while points are left and the stat is under its cap (0x64bc00).
// - CC_Get_Name_Button sends OP_RandomNameGenerator {race, gender} and waits for the name (disabled until it comes;
//   Handle_OP_RandomNameGenerator @0x64ae20 puts it in CC_Name_Edit).
// - CC_Create_Button (SubmitNameToWorld @0x64db30): the combination checked against the expansions
//   (CheckExpansionForCombination @0x64b760: 9167 with 9168 race, 9169 class or 9170 city), then with points left 1577
//   and the stats opened, else the name sent for approval (Send_OP_ApproveName; GameConnection::create_character
//   creates on its answer). CC_Cancel_Button goes back to the character list (0x649b40).
// - The model player (the pre-game scene's, UiHost::model_player) takes the race, gender and class, and its look
//   follows CCharacterCreation's (game/char_create_look.h): a race picked ApplyNewPCToPlayer and
//   RandomizeFacialFeatures, a gender ApplyNewPCToPlayer, a class CCharacterCreation_SetHeritage. CC_Appearance_Button
//   shows CPlayerCustomizationWnd on it (vtable +0x90) and hides this window (Show(0, 1, 1)) until that one hides
//   (its AboutToHide shows this one again). The packet's face, hair, beard, eyes, heritage, tattoo and details are
//   the model's (Send_OP_CharacterCreate @0x64b560 reads pinstLocalPlayer's actor).
// Not built: the head start character (CC_CreateHeadStart_Button, hidden: EQEmu has none), the
// free-to-play locks and their buy buttons, and the tutorial box.
class CharacterCreationWindow : public StockWindow {
public:
    CharacterCreationWindow() : StockWindow("CharacterCreation", false) {}

    bool pre_game() const override { return true; }
    bool shown(UiHost& host) override {
        bool now = host.char_create_open && host.conn && host.conn->char_create_data();
        if (now && !was_shown_) start(host);
        was_shown_ = now;
        if (now)
            if (auto name = host.conn->take_random_name()) {
                name_ = *name;
                name_pending_ = false;
            }
        // Hidden while the customization window shows (the Appearance button's Show(0, 1, 1)).
        return now && !host.customization_shown;
    }
    void set_shown(UiHost&, bool) override {}
    std::optional<std::array<int, 2>> take_move(UiHost&) override { return std::exchange(move_, std::nullopt); }

    void child(UiHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        if (id.starts_with("CC_Race_") || id.starts_with("CC_Class_")) return race_class_child(id, out);
        if (id == "CC_Male_Button") out.checked = gender_ == 0;
        else if (id == "CC_Female_Button") out.checked = gender_ == 1;
        else if (id == "CC_Name_Edit") out.text = name_;
        else if (id == "CC_Get_Name_Button") out.enabled = !name_pending_;
        else if (id == "CC_Help") out.text = help_;
        else if (id == "CC_Help_Label") out.text = help_label_;
        else if (id == "CC_Advanced_Button") out.checked = advanced_;
        else if (id == "CC_AdvancedScreen") out.shown = advanced_;
        else if (id == "CC_CreateHeadStart_Button" || id == "CC_Buy_Race_Button" || id == "CC_Buy_Class_Button") out.shown = false;
        else if (id == "CCA_Points_Left_Display_Button") out.text = std::to_string(points_left_);
        else if (int s = stat_index(id, "_Display_Button"); s >= 0) out.text = std::to_string(stats_[static_cast<std::size_t>(s)]);
        else if (int s2 = stat_index(id, "_Up_Button"); s2 >= 0) out.enabled = points_left_ > 0 && stats_[static_cast<std::size_t>(s2)] < cap(s2);
        else if (int s3 = stat_index(id, "_Down_Button"); s3 >= 0) out.enabled = stats_[static_cast<std::size_t>(s3)] > base(s3);
        else if (int s4 = stat_index(id, "_Button"); s4 >= 0 && alloc_ && alloc_->points[static_cast<std::size_t>(s4)] > 0)
            out.color = Rgb{0.0f, 1.0f, 0.0f}; // 0xff00ff00: a stat the class puts points in
    }
    bool set_text(UiHost&, std::string_view id, const std::string& text) override {
        if (id != "CC_Name_Edit") return false;
        name_ = text.substr(0, 64);
        return true;
    }
    const ComboData* combo(UiHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        if (id == "CC_Deity_Combo") {
            deity_combo_ = {};
            for (std::uint32_t d : deities_) deity_combo_.choices.push_back(deity_desc(*host.conn, d));
            for (std::size_t i = 0; i < deities_.size(); ++i)
                if (deities_[i] == deity_) deity_combo_.selected = static_cast<int>(i);
            return &deity_combo_;
        }
        if (id == "CC_City_Combo") {
            city_combo_ = {};
            for (std::uint32_t z : cities_) city_combo_.choices.push_back(game::zone_long_name(z));
            for (std::size_t i = 0; i < cities_.size(); ++i)
                if (cities_[i] == city_) city_combo_.selected = static_cast<int>(i);
            return &city_combo_;
        }
        return nullptr;
    }
    bool combo_select(UiHost& host, std::string_view id, int index) override {
        if (id == "CC_Deity_Combo" && index >= 0 && index < static_cast<int>(deities_.size())) {
            set_deity(deities_[static_cast<std::size_t>(index)]); // 0x64d8e0
            help(host, 14, deity_, deity_desc(*host.conn, deity_));
            return true;
        }
        if (id == "CC_City_Combo" && index >= 0 && index < static_cast<int>(cities_.size())) {
            city_ = cities_[static_cast<std::size_t>(index)];
            help(host, 15, city_, game::zone_long_name(city_));
            check_expansion(host);
            return true;
        }
        return false;
    }
    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn) return false;
        bool right = button == "right" || button == "rightup";
        if (button != "left" && !right) return false;
        if (auto r = race_of(id)) {
            if (right) return help(host, 8, *r, game::race_name(*r)), true;
            set_race(*r);
            dress(host, Change::Race);
            help(host, 8, race_, game::race_name(race_));
            check_expansion(host);
            return true;
        }
        if (auto c = class_of(id)) {
            if (right) return help(host, 9, *c, game::class_name(*c)), true;
            if (!has(race_, *c)) return true; // a disabled button
            set_class(*c);
            dress(host, Change::Class);
            help(host, 9, class_, game::class_name(class_));
            check_expansion(host);
            return true;
        }
        if (right) { // the stat's button: as many points as it takes (sub_0064d7f0), and its help
            if (int s = stat_index(id, "_Button"); s >= 0) spend_max(s), help_stat(host, s);
            return true;
        }
        if (id == "CC_Male_Button" || id == "CC_Female_Button") {
            gender_ = id == "CC_Male_Button" ? 0 : 1;
            dress(host, Change::Gender);
        } else if (id == "CC_Appearance_Button") {
            if (host.show_window) host.show_window("PlayerCustomizationWnd");
        } else if (id == "CC_Get_Name_Button") {
            if (!name_pending_) {
                host.conn->request_random_name(race_, static_cast<std::uint32_t>(gender_));
                name_pending_ = true;
            }
        } else if (id == "CC_Create_Button") {
            submit(host);
        } else if (id == "CC_Cancel_Button") {
            if (host.char_create_cancel) host.char_create_cancel();
        } else if (id == "CC_Advanced_Button") {
            advanced_ = !advanced_;
        } else if (id == "CC_Advanced_OK_Button") {
            advanced_ = false;
        } else if (int s = stat_index(id, "_Up_Button"); s >= 0) {
            if (points_left_ > 0 && stats_[static_cast<std::size_t>(s)] + 1 <= cap(s)) ++stats_[static_cast<std::size_t>(s)], --points_left_;
            help_stat(host, s);
        } else if (int s2 = stat_index(id, "_Down_Button"); s2 >= 0) {
            if (stats_[static_cast<std::size_t>(s2)] - 1 >= base(s2)) --stats_[static_cast<std::size_t>(s2)], ++points_left_;
            help_stat(host, s2);
        } else if (int s3 = stat_index(id, "_Button"); s3 >= 0) {
            help_stat(host, s3);
        } else {
            return false;
        }
        return true;
    }

private:
    // CC_Race_<name>_Button[_Male] / CC_Class_<name>_Button (not the overlays).
    std::optional<std::uint32_t> race_of(std::string_view id) const {
        for (std::uint32_t r : races_) {
            std::string b = "CC_Race_" + game::race_name(r) + "_Button";
            if (id == b || id == b + "_Male") return r;
        }
        return std::nullopt;
    }
    std::optional<std::uint32_t> class_of(std::string_view id) const {
        for (std::uint32_t c = 1; c <= 16; ++c)
            if (id == "CC_Class_" + game::stock_class_name(c) + "_Button") return c; // the layout's names
        return std::nullopt;
    }
    void race_class_child(std::string_view id, Child& out) const {
        if (id.find("_Overlay_Button") != std::string_view::npos) {
            out.shown = false;
            return;
        }
        if (auto r = race_of(id)) {
            bool male = id.ends_with("_Male");
            out.shown = male == (gender_ == 0);
            out.checked = *r == race_;
        } else if (auto c = class_of(id)) {
            out.enabled = has(race_, *c);
            out.checked = *c == class_;
        }
    }
    static int stat_index(std::string_view id, std::string_view suffix) {
        if (!id.starts_with("CCA_Stat") || !id.ends_with(suffix) || id.size() != 8 + 1 + suffix.size()) return -1;
        char c = id[8];
        return c >= '0' && c <= '6' ? c - '0' : -1;
    }
    std::uint32_t expansions(UiHost& host) const { return host.conn ? host.conn->world_expansions() : 0; }
    static bool allowed(const net::CharCreateCombo& c, std::uint32_t owned) { return net::char_create_allowed(c, owned); }
    bool has(std::uint32_t race, std::uint32_t cls) const {
        for (const auto& c : data_.combos)
            if (c.race == race && c.class_ == cls) return true;
        return false;
    }
    bool has_combination(std::uint32_t owned) const {
        for (const auto& c : data_.combos)
            if (c.race == race_ && c.class_ == class_ && c.deity == deity_ && c.zone == city_ && allowed(c, owned)) return true;
        return false;
    }

    void start(UiHost& host) {
        data_ = *host.conn->char_create_data();
        owned_ = expansions(host);
        races_.clear();
        for (const auto& c : data_.combos) // CharCreateCombos_GetRaces: in the tables' order, each once
            if (std::find(races_.begin(), races_.end(), c.race) == races_.end()) races_.push_back(c.race);
        name_.clear();
        name_pending_ = false;
        advanced_ = false;
        gender_ = 0;
        help_.clear();
        help_label_.clear();
        UiHost* hp = &host;
        deity_names_ = [hp](std::uint32_t d) { return hp->conn ? deity_desc(*hp->conn, d) : std::string(); };
        if (host.char_create_look) *host.char_create_look = {};
        if (!races_.empty()) set_race(races_.front());
        dress(host, Change::Race);
        if (race_) help(host, 8, race_, game::race_name(race_));
        ImVec2 ds = ImGui::GetIO().DisplaySize; // the 800 x 600 page, centred
        move_ = std::array<int, 2>{(static_cast<int>(ds.x) - 800) / 2, (static_cast<int>(ds.y) - 600) / 2};
    }
    // The model player takes the window's race, gender and class (CharSelect_SwitchPlayerModel), and its look
    // follows the change.
    enum class Change { Race, Gender, Class };
    void dress(UiHost& host, Change change) {
        game::Spawn* m = host.model_player;
        if (!m || !host.char_create_look) return;
        game::CharCreateLook& look = *host.char_create_look;
        m->race = static_cast<std::uint16_t>(race_);
        m->gender = static_cast<std::uint8_t>(gender_);
        m->class_id = static_cast<std::uint8_t>(class_);
        m->appearance.class_id = m->class_id;
        ModelPlayerActor a(host, rng_);
        switch (change) {
        case Change::Race: look.race_changed(a); break;
        case Change::Gender: look.gender_changed(a); break;
        case Change::Class: look.class_changed(a); break;
        }
        a.commit();
    }
    void set_race(std::uint32_t race) {
        race_ = race;
        // RandomizeClass: a class of the race's combinations the expansions allow.
        std::vector<std::uint32_t> classes;
        for (const auto& c : data_.combos)
            if (c.race == race && allowed(c, owned_) && std::find(classes.begin(), classes.end(), c.class_) == classes.end())
                classes.push_back(c.class_);
        set_class(classes.empty() ? 0 : classes[static_cast<std::size_t>(classes.size() > 1 ? std::rand() % classes.size() : 0)]);
    }
    void set_class(std::uint32_t cls) {
        class_ = cls;
        // RandomizeDeity and FillDeityCombo: the deities of race and class (by name).
        deities_.clear();
        for (const auto& c : data_.combos)
            if (c.race == race_ && c.class_ == class_ && std::find(deities_.begin(), deities_.end(), c.deity) == deities_.end())
                deities_.push_back(c.deity);
        sort_deities();
        set_deity(deities_.empty() ? 0 : deities_[static_cast<std::size_t>(deities_.size() > 1 ? std::rand() % deities_.size() : 0)]);
        reset_stats();
    }
    void set_deity(std::uint32_t deity) {
        deity_ = deity;
        // 0x64bee0: the city stays when race, class, deity and city still combine; else the first start zone
        // that does. FillStartCityCombo: the start zones by name.
        cities_.clear();
        for (const auto& c : data_.combos)
            if (c.race == race_ && c.class_ == class_ && c.deity == deity_ &&
                std::find(cities_.begin(), cities_.end(), c.zone) == cities_.end())
                cities_.push_back(c.zone);
        if (!has_combination(owned_)) {
            std::uint32_t pick = 0;
            for (std::uint32_t z : cities_) {
                pick = z;
                std::uint32_t keep = city_;
                city_ = z;
                bool ok = has_combination(owned_);
                city_ = keep;
                if (ok) break;
            }
            city_ = pick;
        }
        std::sort(cities_.begin(), cities_.end(),
                  [](std::uint32_t a, std::uint32_t b) { return game::zone_long_name(a) < game::zone_long_name(b); });
        // the allocation of this combination
        alloc_.reset();
        for (const auto& c : data_.combos)
            if (c.race == race_ && c.class_ == class_ && c.deity == deity_ && c.zone == city_)
                for (const auto& a : data_.allocations)
                    if (a.index == c.allocation_index) alloc_ = a;
    }
    void sort_deities() {
        // 0x51a020 compares GetDeityDesc's names; the deity ids stand in for them before a connection names them
        std::sort(deities_.begin(), deities_.end(), [this](std::uint32_t a, std::uint32_t b) {
            return deity_names_ ? deity_names_(a) < deity_names_(b) : a < b;
        });
    }
    void reset_stats() {
        points_left_ = 0;
        for (int i = 0; i < 7; ++i) {
            stats_[static_cast<std::size_t>(i)] = base(i);
            points_left_ += alloc_ ? static_cast<int>(alloc_->points[static_cast<std::size_t>(i)]) : 0;
        }
    }
    int base(int s) const { return alloc_ ? static_cast<int>(alloc_->base[static_cast<std::size_t>(s)]) : 0; }
    int cap(int s) const { return std::min(base(s) + 25, 150); }
    void spend_max(int s) {
        int add = std::min(cap(s) - stats_[static_cast<std::size_t>(s)], points_left_);
        if (add <= 0) return;
        stats_[static_cast<std::size_t>(s)] += add;
        points_left_ -= add;
    }
    void help(UiHost& host, std::uint32_t type, std::uint32_t id, std::string label) {
        help_ = host.conn ? host.conn->strings().db_string(id, type) : std::string();
        help_label_ = std::move(label);
    }
    void help_stat(UiHost& host, int s) {
        static constexpr const char* kNames[7] = {"STRENGTH", "DEXTERITY", "AGILITY", "STAMINA", "INTELLIGENCE", "WISDOM", "CHARISMA"};
        help(host, 10, static_cast<std::uint32_t>(s), kNames[s]); // the stat button's text
    }
    bool check_expansion(UiHost& host) {
        owned_ = expansions(host);
        if (has_combination(owned_)) return true;
        bool race_ok = false, class_ok = false;
        for (const auto& c : data_.combos) {
            race_ok = race_ok || (c.race == race_ && allowed(c, owned_));
            class_ok = class_ok || (c.race == race_ && c.class_ == class_ && allowed(c, owned_));
        }
        std::uint32_t why = !race_ok ? 9168 : !class_ok ? 9169 : 9170;
        UiHost::Confirm c;
        c.text = host.conn->strings().format(9167, {}) + host.conn->strings().format(why, {});
        c.timeout_ms = 5000;
        host.confirm = std::move(c);
        return false;
    }
    void submit(UiHost& host) {
        if (!check_expansion(host)) return;
        if (points_left_ != 0) { // 1577 (+ 1578), the stats opened
            advanced_ = true;
            UiHost::Confirm c;
            c.text = host.conn->strings().format(1577, {}) + " " + host.conn->strings().format(1578, {});
            c.timeout_ms = 5000;
            host.confirm = std::move(c);
            return;
        }
        net::CharCreate cc;
        cc.gender = static_cast<std::uint32_t>(gender_);
        cc.race = race_;
        cc.class_ = class_;
        cc.deity = deity_;
        cc.start_zone = city_;
        // STR, DEX, AGI, STA, INT, WIS, CHA
        cc.str = static_cast<std::uint32_t>(stats_[0]), cc.dex = static_cast<std::uint32_t>(stats_[1]);
        cc.agi = static_cast<std::uint32_t>(stats_[2]), cc.sta = static_cast<std::uint32_t>(stats_[3]);
        cc.int_ = static_cast<std::uint32_t>(stats_[4]), cc.wis = static_cast<std::uint32_t>(stats_[5]);
        cc.cha = static_cast<std::uint32_t>(stats_[6]);
        if (const game::Spawn* m = host.model_player) { // the model's features (Send_OP_CharacterCreate)
            const game::Appearance& a = m->appearance;
            cc.face = a.face, cc.hair_style = a.hair_style, cc.hair_color = a.hair_color;
            cc.beard = a.beard, cc.beard_color = a.beard_color, cc.eye_color1 = a.eye_color1, cc.eye_color2 = a.eye_color2;
            cc.drakkin_heritage = a.drakkin_heritage, cc.drakkin_tattoo = a.drakkin_tattoo;
            cc.drakkin_details = a.drakkin_details;
        }
        if (host.char_create_submit) host.char_create_submit(name_, cc);
    }

    net::CharCreateData data_;
    std::uint32_t owned_ = 0;
    std::vector<std::uint32_t> races_, deities_, cities_;
    std::uint32_t race_ = 0, class_ = 0, deity_ = 0, city_ = 0;
    int gender_ = 0;
    std::array<int, 7> stats_{};
    int points_left_ = 0;
    std::optional<net::CharCreateAllocation> alloc_;
    std::string name_, help_, help_label_;
    bool name_pending_ = false, advanced_ = false, was_shown_ = false;
    ComboData deity_combo_, city_combo_;
    std::function<std::string(std::uint32_t)> deity_names_;
    std::optional<std::array<int, 2>> move_;
    std::mt19937 rng_{std::random_device{}()};
};

} // namespace

void add_login_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<ServerSelectWindow>());
    out.push_back(std::make_unique<CharacterCreationWindow>());
}

} // namespace hailstone::app
