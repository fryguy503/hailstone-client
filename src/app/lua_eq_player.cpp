// The player windows: InspectWnd (CInspectWnd), ConfirmationDialogBox (CConfirmationDialogBoxWnd), BookWindow
// (CBookWnd), NoteWindow (CNoteWnd), BandolierWnd (CBandolierWnd), TextEntryWnd (CTextEntryWnd),
// LargeDialogWindow (CLargeDialogWnd) and TipWindow (CTipWnd, the context tips) over GameConnection's duel, inspect, book, bandolier and popup state
// (net/game_connection_player.cpp). Each window's rules are its original's (addresses at each
// class); what is not known is in docs/unknowns.md.

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <functional>
#include <iterator>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/actions_ui.h"
#include "app/lua_eq_windows.h"
#include "game/combat.h"
#include "game/xtarget.h"
#include "game/hot_button_state.h"
#include "game/interact.h"
#include "game/mechanics/character.h"
#include "core/game_dir.h"
#include "ui/ui_ini.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

int index_after(std::string_view id, std::string_view prefix) {
    if (id.size() <= prefix.size() || id.substr(0, prefix.size()) != prefix) return -1;
    int n = 0;
    for (char c : id.substr(prefix.size())) {
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
    }
    return n;
}

ChildArt image(const std::optional<ui::UiTextures::Cell>& c) {
    ChildArt a;
    if (!c) {
        a.kind = ChildArt::None;
        return a;
    }
    a.kind = ChildArt::Image;
    a.cell = *c;
    return a;
}

std::string eqstr(EqHost& host, std::uint32_t id, std::vector<std::string> args = {}) {
    return host.conn ? host.conn->strings().format(id, args) : std::string();
}

// Centred on the screen by the window's XML size (CPopupWndManager's dialogs, CQuantityWnd without a point).
std::array<int, 2> centred(EqHost& host, const char* screen) {
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    auto r = host.textures ? host.textures->screen_rect(screen) : std::nullopt;
    int w = r ? r->w : 300, h = r ? r->h : 200;
    return {(static_cast<int>(ds.x) - w) / 2, (static_cast<int>(ds.y) - h) / 2};
}

// ---------------------------------------------------------------- InspectWnd (CInspectWnd)

// Handle_OP_InspectAnswer @0x68f9f0 titles the window "You are inspecting %1." (eqstr 1401), enables INSW_Edit only
// for ourselves and sets it to the player's message, and puts an item with the slot's name and icon into each of
// the 23 worn slots. WndNotification @0x68f780: an edit (notification 6) saves our message at most every 2 s
// (CInspectWnd_SaveInspectMessage), as AboutToHide does; Done closes. OnProcessFrame closes it when the player is
// over 20 away (GameConnection::update_inspect). The edit box holds 254 characters (0xfe; 0x54 on the CJK clients).
class InspectWindow : public StockWindow {
public:
    InspectWindow() : StockWindow("InspectWnd", false) {}

    bool shown(EqHost& host) override {
        const auto* v = view(host);
        if (!v) {
            for_ = 0;
            return false;
        }
        if (for_ != v->player_id || serial_text_ != v->text) {
            // A new answer replaces the slots and the message.
            for_ = v->player_id;
            serial_text_ = v->text;
            edit_ = v->text;
            for (std::size_t i = 0; i < items_.size(); ++i) {
                items_[i] = net::Item{};
                items_[i].name = v->item_names[i];
                items_[i].icon = v->item_icons[i];
            }
        }
        return true;
    }
    void set_shown(EqHost& host, bool show) override {
        if (show || !host.conn) return;
        save(host); // AboutToHide
        host.conn->close_inspect();
    }
    std::string title(EqHost& host) override {
        const auto* v = view(host);
        return v ? eqstr(host, 1401, {v->player_name}) : std::string();
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        const auto* v = view(host);
        if (!v) return;
        if (id == "INSW_Edit") {
            out.text = edit_;
            out.enabled = v->own;
        }
    }
    const net::Item* slot_item(EqHost& host, std::string_view id) override {
        const auto* v = view(host);
        int n = index_after(id, "InvSlot");
        if (!v || n < 0 || n >= static_cast<int>(items_.size()) || v->item_names[static_cast<std::size_t>(n)].empty())
            return nullptr;
        return &items_[static_cast<std::size_t>(n)];
    }
    bool set_text(EqHost& host, std::string_view id, const std::string& text) override {
        const auto* v = view(host);
        if (!v || !v->own || id != "INSW_Edit") return false;
        edit_ = text.substr(0, 254);
        auto now = std::chrono::steady_clock::now();
        if (now - last_save_ > std::chrono::seconds(2)) {
            save(host);
            last_save_ = now;
        }
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!view(host)) return false;
        if (id == "DoneButton" && button == "left") {
            set_shown(host, false);
            return true;
        }
        return button == "left" && index_after(id, "InvSlot") >= 0; // the slots hold names and icons only
    }

private:
    static const game::InspectView* view(EqHost& host) {
        return host.conn && host.conn->inspect_view() ? &*host.conn->inspect_view() : nullptr;
    }
    void save(EqHost& host) {
        const auto* v = view(host);
        if (v && v->own && edit_ != v->text) host.conn->set_inspect_message(edit_);
    }

    std::uint32_t for_ = 0;
    std::string serial_text_, edit_;
    std::array<net::Item, 23> items_;
    std::chrono::steady_clock::time_point last_save_{};
};

// ---------------------------------------------------------------- ConfirmationDialogBox (CConfirmationDialogBoxWnd)

// CConfirmationDialogBoxWnd @0x71d8f0: TextOutput (STML), and the buttons its flags show (sub_0071db50: bit 0 OK,
// bit 1 Cancel, bit 2 Yes, bit 3 No); a button closes it and answers its owner (Yes 4, No 8, OK 1, Cancel 2), a
// timeout (OnProcessFrame) answers 2. Two questions use it here: the ground drop (CEverQuest::LMouseUp:
// ConfirmationDialog(0x6e, eqstr 9057, flags 0xc, 20000 ms)), the connection's (the surname, flags 0xc assumed:
// its call's arguments are not decoded; the Options window's item and purchase questions give their own time limit
// and buttons), a window's own (EqHost::confirm) and the guild invitation. Shown centred (where CPopupWndManager puts it is not known).
class ConfirmationWindow : public StockWindow {
public:
    ConfirmationWindow() : StockWindow("ConfirmationDialogBox", false) {}
    bool pre_game() const override { return true; } // the character creation's questions

    bool shown(EqHost& host) override {
        int q = question(host);
        if (q != showing_) {
            showing_ = q;
            int timeout = q == 1   ? 20000
                          : q == 2 ? host.conn->confirmation()->timeout_ms
                          : q == 3 ? host.confirm->timeout_ms
                          : q == 4 ? 10000
                                   : 0;
            deadline_ = timeout ? std::optional(std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout)) : std::nullopt;
            if (q) move_ = centred(host, "ConfirmationDialogBox");
        }
        // OnProcessFrame: the question's time runs out (answer 2: as No).
        if (deadline_ && std::chrono::steady_clock::now() > *deadline_) {
            deadline_.reset();
            answer(host, false);
            return shown(host);
        }
        return q != 0;
    }
    void set_shown(EqHost& host, bool show) override {
        if (!show) answer(host, false);
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        int q = question(host);
        if (!q) return;
        if (id == "TextOutput")
            out.text = q == 1 ? host.conn->drop_confirm_text() : q == 2 ? host.conn->confirmation()->text
                     : q == 3 ? host.confirm->text : guild_question(host);
        else if (id == "Yes_Button" || id == "No_Button") out.shown = !ok_only(host);
        else if (id == "OK_Button") out.shown = ok_only(host);
        else if (id == "Cancel_Button") out.shown = false;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!question(host) || button != "left") return false;
        if (id == "Yes_Button") answer(host, true);
        else if (id == "No_Button" || id == "OK_Button") answer(host, false);
        else return false;
        return true;
    }
    std::optional<std::array<int, 2>> take_move(EqHost&) override {
        auto m = move_;
        move_.reset();
        return m;
    }

private:
    // 1 the drop question, 2 the connection's, 3 a window's own (EqHost::confirm), 0 none.
    static int question(EqHost& host) {
        if (!host.conn) return 0;
        if (host.drop_confirm && *host.drop_confirm) {
            if (host.conn->inventory().cursor()) return 1;
            *host.drop_confirm = false; // the item left the cursor
        }
        if (host.conn->confirmation()) return 2;
        if (host.confirm) return 3;
        return host.conn->guild().inviter.empty() ? 0 : 4;
    }
    // A connection question with flags 1 (an OK button alone).
    static bool ok_only(EqHost& host) { return question(host) == 2 && host.conn->confirmation()->ok_only; }
    void answer(EqHost& host, bool yes) {
        int q = question(host);
        showing_ = 0; // the next question (a queued one may follow at once) starts its own time limit
        if (q == 1) {
            *host.drop_confirm = false;
            if (yes) host.conn->drop_cursor_on_ground();
        } else if (q == 2) {
            host.conn->answer_confirmation(yes);
        } else if (q == 3) {
            auto done = std::move(host.confirm->done);
            host.confirm.reset();
            if (done) done(yes);
        } else if (q == 4) {
            host.conn->answer_guild_invite(yes);
        }
    }

    // Handle_OP_GuildInvite @0x4b9410: "%1 is interested in having you join %2." (eqstr 534), asked with
    // ConfirmationDialog(100, flags 0xc, 10000 ms).
    static std::string guild_question(EqHost& host) {
        const auto& g = host.conn->guild();
        return host.conn->strings().format(534, std::vector<std::string>{game::display_name(g.inviter), g.name_of(g.invite_guild)});
    }

    int showing_ = 0;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    std::optional<std::array<int, 2>> move_;
};

// ---------------------------------------------------------------- BookWindow (CBookWnd), NoteWindow (CNoteWnd)

// CBookWnd::ProcessText @0x640360: the text in words (a word ends at ' ' or '`', the book's line break), lines of
// under 31 characters (a word that would reach 31 starts a new line), 12 lines a page, at most 100 pages. (The
// original also cuts a word where its width passes the page's, measured with the page font; not done here.)
std::vector<std::string> book_pages(const std::string& text) {
    std::vector<std::string> pages(1);
    int lines = 0;
    std::size_t col = 0;
    auto newline = [&] {
        pages.back() += '\n';
        col = 0;
        if (++lines == 12) {
            lines = 0;
            if (pages.size() >= 100) return false;
            pages.emplace_back();
        }
        return true;
    };
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t end = i;
        while (end < text.size() && text[end] != ' ' && text[end] != '`') ++end;
        std::string word = text.substr(i, end - i);
        bool at_space = end < text.size() && text[end] == ' ';
        if (col + word.size() + 1 < 31) {
            if (col != 0 || (word.empty() && at_space)) {
                pages.back() += ' ';
                ++col;
            }
        } else if (!newline()) {
            break;
        }
        pages.back() += word;
        col += word.size();
        if (end < text.size() && text[end] == '`' && !newline()) break;
        if (end >= text.size()) break;
        i = end + 1;
    }
    while (pages.size() > 1 && pages.back().empty()) pages.pop_back();
    return pages;
}

class BookWindow : public StockWindow {
public:
    BookWindow() : StockWindow("BookWindow", false) {}

    bool shown(EqHost& host) override {
        const net::BookView* b = book(host);
        if (!b) {
            text_.clear();
            return false;
        }
        if (b->text != text_) {
            // SetBook: page 0, the text paged by ProcessText.
            text_ = b->text;
            pages_ = book_pages(text_);
            page_ = 0;
        }
        return true;
    }
    void set_shown(EqHost& host, bool show) override {
        if (!show && book(host)) host.conn->close_book();
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        const net::BookView* b = book(host);
        if (!b) return;
        auto page = [&](int n) { return n >= 0 && n < static_cast<int>(pages_.size()) ? pages_[static_cast<std::size_t>(n)] : std::string(); };
        // sub_006401d0: the two page numbers and texts; sub_006406d0: Right while page + 2 has text (and page < 98),
        // Left from page 2.
        if (id == "Page0Number") out.text = std::to_string(page_ + 1);
        else if (id == "Page1Number") out.text = std::to_string(page_ + 2);
        else if (id == "Page0") out.text = page(page_), out.enabled = false;
        else if (id == "Page1") out.text = page(page_ + 1), out.enabled = false;
        else if (id == "RightButton") out.enabled = page_ <= 0x61 && !page(page_ + 2).empty();
        else if (id == "LeftButton") out.enabled = page_ != 0;
        else if (id == "CastSpellButton") out.shown = b->can_cast;
        else if (id == "ScribeButton") out.shown = b->can_scribe;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!book(host) || button != "left") return false;
        if (id == "RightButton") {
            if (page_ <= 0x61 && page_ + 2 < static_cast<int>(pages_.size())) page_ += 2;
        } else if (id == "LeftButton") {
            if (page_ > 0) page_ -= 2;
        } else if (id == "DoneButton") {
            page_ = 0;
            host.conn->close_book();
        } else if (id == "CastSpellButton" || id == "ScribeButton") {
            host.conn->book_button();
        } else {
            return false;
        }
        return true;
    }

private:
    static const net::BookView* book(EqHost& host) {
        if (!host.conn || !host.conn->book() || host.conn->book()->type != 1) return nullptr;
        return &*host.conn->book();
    }
    std::string text_;
    std::vector<std::string> pages_;
    int page_ = 0;
};

// CNoteWnd: the note's text as STML in Note (AboutToShow), Done closes; Cast Spell / Scribe send OP_BookButton.
class NoteWindow : public StockWindow {
public:
    NoteWindow() : StockWindow("NoteWindow", false) {}
    bool shown(EqHost& host) override { return note(host) != nullptr; }
    void set_shown(EqHost& host, bool show) override {
        if (!show && note(host)) host.conn->close_book();
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        const net::BookView* b = note(host);
        if (!b) return;
        if (id == "Note") out.text = b->text;
        else if (id == "CastSpellButton") out.shown = b->can_cast;
        else if (id == "ScribeButton") out.shown = b->can_scribe;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!note(host) || button != "left") return false;
        if (id == "DoneButton") host.conn->close_book();
        else if (id == "CastSpellButton" || id == "ScribeButton") host.conn->book_button();
        else return false;
        return true;
    }

private:
    static const net::BookView* note(EqHost& host) {
        if (!host.conn || !host.conn->book() || host.conn->book()->type != 0) return nullptr;
        return &*host.conn->book();
    }
};

// ---------------------------------------------------------------- TextEntryWnd (CTextEntryWnd)

// CTextEntryWnd::Activate(owner, prompt, max characters, text): Prompt, the Entry box, OK (the owner's
// notification 0x26 with 1: GetEntryText) and Cancel. Here the owner is EqHost::text_entry's handler.
class TextEntryWindow : public StockWindow {
public:
    TextEntryWindow() : StockWindow("TextEntryWnd", false) {}
    void frame(EqHost& host) override {
        if (host.text_entry && host.text_entry->owner_valid && !host.text_entry->owner_valid()) host.text_entry.reset();
    }
    bool shown(EqHost& host) override {
        frame(host);
        if (host.text_entry && !was_) move_ = centred(host, "TextEntryWnd");
        was_ = host.text_entry.has_value();
        return was_;
    }
    void set_shown(EqHost& host, bool show) override {
        if (!show) host.text_entry.reset();
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        frame(host);
        if (!host.text_entry) return;
        if (id == "Prompt") out.text = host.text_entry->prompt;
        else if (id == "Entry") out.text = host.text_entry->text;
        else if (id == "OK") out.enabled = static_cast<int>(host.text_entry->text.size()) >= host.text_entry->min_chars;
    }
    bool set_text(EqHost& host, std::string_view id, const std::string& text) override {
        frame(host);
        if (!host.text_entry || id != "Entry") return false;
        int max = host.text_entry->max_chars;
        host.text_entry->text = max > 0 ? text.substr(0, static_cast<std::size_t>(max)) : text;
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        frame(host);
        if (!host.text_entry || button != "left") return false;
        if (id == "OK" || id == "Entry") {
            if (static_cast<int>(host.text_entry->text.size()) < host.text_entry->min_chars) return true;
            auto e = std::move(*host.text_entry);
            host.text_entry.reset();
            if (e.done) e.done(e.text);
        } else if (id == "Cancel") {
            host.text_entry.reset();
        } else {
            return false;
        }
        return true;
    }
    std::optional<std::array<int, 2>> take_move(EqHost&) override {
        auto m = move_;
        move_.reset();
        return m;
    }

private:
    bool was_ = false;
    std::optional<std::array<int, 2>> move_;
};

// ---------------------------------------------------------------- BandolierWnd (CBandolierWnd)

// CBandolierWnd (WndNotification @0x626980): WeaponSetList lists the sets the player may use
// (CBandolierWnd_FillWeaponSetList: the set's name, or eqstr 9338 for an empty one; the selection kept, row 0 by
// default); the four DisplayButtons show the selected set's items (CBandolierWnd_UpdateSetSlots: the item's icon or
// cell 336, its name as the tooltip, tinted green when the player has the item, red when not, white when the slot is
// empty). Add asks for a name (CTextEntryWnd, eqstr 9347, 30 characters) and stores the worn weapons as the
// selected set (PcZoneClient_CreateBandolierSet); Delete sends OP_Bandolier remove; Use (and a list click with Auto
// Swap checked) is PcZoneClient::BandolierSwap. Auto Swap is kept in the UI ini ([BandolierWnd] AutoSwap "1" / "0",
// read as on for '1', 't' or 'T'). Make Hot Button (CBandolierWnd_MakeHotButton @0x6261d0) stores a social named as
// the set with the line "/bandolier Activate <set>" (9305, 9310; 0x4ea9f0(name, line, 0, -1, 1)), puts it on the
// cursor and says 13282 (or 13283). The right button held on a slot sends OP_Bandolier 3 (encode_bandolier_hold).
class BandolierWindow : public StockWindow {
public:
    BandolierWindow() : StockWindow("BandolierWnd", false) {}

    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn || id != "WeaponSetList") return nullptr;
        const auto& sets = host.conn->bandolier();
        data_ = {};
        data_.columns = 1;
        data_.labels = {""};
        data_.widths = {0};
        int count = host.conn->bandolier_sets();
        for (int i = 0; i < count && i < static_cast<int>(sets.size()); ++i) {
            const auto& s = sets[static_cast<std::size_t>(i)];
            data_.rows.push_back({{s.name.empty() ? eqstr(host, 9338) : s.name}, {1, 1, 1}, std::nullopt});
        }
        if (selected_ >= static_cast<int>(data_.rows.size())) selected_ = static_cast<int>(data_.rows.size()) - 1;
        if (selected_ < 0 && !data_.rows.empty()) selected_ = 0;
        data_.selected = selected_;
        return &data_;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool, int) override {
        if (!host.conn || id != "WeaponSetList") return false;
        selected_ = row;
        if (auto_swap_) host.conn->bandolier_activate(row);
        return true;
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        if (id == "AutoSwapButton") {
            out.checked = auto_swap_;
            return;
        }
        int n = index_after(id, "DisplayButton");
        if (n < 0 || n > 3) return;
        const auto& sets = host.conn->bandolier();
        if (selected_ < 0 || selected_ >= static_cast<int>(sets.size())) {
            out.decal.kind = ChildArt::None;
            return;
        }
        const game::BeltItem& it = sets[static_cast<std::size_t>(selected_)].items[static_cast<std::size_t>(n)];
        std::uint32_t icon = it.icon == 0 || it.icon == 0xffffffffu ? 836u : it.icon; // cell 0x150 = icon 836
        if (host.textures) out.decal = image(host.textures->item_icon(icon));
        out.tooltip = it.name;
        if (it.id >= 1) {
            bool have = has_item(*host.conn, it.id);
            out.tint = have ? std::array<float, 4>{0, 1, 0, 1} : std::array<float, 4>{1, 0, 0, 1};
        }
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (button == "rightheld") return click_rightheld(host, id);
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        if (id == "AutoSwapButton") {
            auto_swap_ = !auto_swap_;
            if (ini_) ini_->put("BandolierWnd", "AutoSwap", auto_swap_ ? "1" : "0"); // sub_00626660
        } else if (id == "AddButton") {
            int at = selected_;
            EqHost::TextEntry e;
            e.prompt = eqstr(host, 9347);
            e.max_chars = 30;
            e.done = [&host, at](const std::string& name) {
                if (host.conn && at >= 0) host.conn->bandolier_create(at, name);
            };
            host.text_entry = std::move(e);
        } else if (id == "DeleteButton") {
            if (selected_ >= 0) c.bandolier_delete(selected_);
        } else if (id == "UseButton") {
            if (selected_ >= 0) c.bandolier_activate(selected_);
        } else if (id == "MkHotButton") {
            make_hot_button(host);
        } else if (index_after(id, "DisplayButton") >= 0) {
            // (a left click on a slot does nothing)
        } else {
            return false;
        }
        return true;
    }
    bool click_rightheld(EqHost& host, std::string_view id) {
        int n = index_after(id, "DisplayButton");
        if (!host.conn || n < 0 || n > 3 || selected_ < 0) return false;
        host.conn->bandolier_hold(selected_, n, ImGui::GetIO().KeyShift);
        return true;
    }
    void ini_loaded(EqHost&, ui::UiIni& ini, const std::filesystem::path&) override {
        // sub_006265c0 (the constructor): AutoSwap, default "0".
        ini_ = &ini;
        std::string v = ini.get("BandolierWnd", "AutoSwap").value_or("0");
        auto_swap_ = !v.empty() && (v[0] == '1' || v[0] == 't' || v[0] == 'T');
    }

private:
    // 0x44c500: the item is somewhere in the player's possessions (worn, general slots, bags, cursor).
    static bool has_item(const net::GameConnection& c, std::uint32_t id) {
        for (const auto& [key, item] : c.inventory().items()) {
            if (key.first != net::kInvPossessions) continue;
            if (item.id == id) return true;
            for (const auto& b : item.contents)
                if (b.item.id == id) return true;
        }
        return false;
    }

    void make_hot_button(EqHost& host) {
        auto& c = *host.conn;
        const auto& sets = c.bandolier();
        if (selected_ < 0 || selected_ >= c.bandolier_sets() || selected_ >= static_cast<int>(sets.size())) return;
        const std::string& name = sets[static_cast<std::size_t>(selected_)].name;
        std::string line = eqstr(host, 9305) + " " + eqstr(host, 9310) + " " + name;
        int index = host.actions ? host.actions->store_social(name, line, 0) : -1;
        if (index >= 0) {
            game::HotButton b;
            b.type = game::HotButtonType::Social;
            b.slot = index;
            host.cursor_button = b;
            ++host.cursor_serial;
        }
        c.show_eqstr(index >= 0 ? 13282 : 13283);
    }

    ListData data_;
    int selected_ = 0;
    bool auto_swap_ = false;
    ui::UiIni* ini_ = nullptr;
};

// ---------------------------------------------------------------- LargeDialogWindow (CLargeDialogWnd)

// CLargeDialogWnd::Open @0x6b4ee0 for OP_OnLevelMessage: the title, LDW_TextBox's text, OK alone, or Yes and No
// with the server's labels; the volume slider, its label, Mute and Replay only with the sound controls (the voice
// files are not played here). WndNotification @0x6b5690: OK, Yes and No answer (GameConnection::answer_large_dialog)
// and close; the close box and the duration's end answer "closed" (vf98).
class LargeDialogWindow : public StockWindow {
public:
    LargeDialogWindow() : StockWindow("LargeDialogWindow", false) {}

    bool shown(EqHost& host) override {
        const net::LargeDialog* d = dialog(host);
        if (d && d->close_at_ms && net::GameConnection::now_ms() > d->close_at_ms) {
            host.conn->answer_large_dialog(3);
            return false;
        }
        return d != nullptr;
    }
    void set_shown(EqHost& host, bool show) override {
        if (!show && dialog(host)) host.conn->answer_large_dialog(3);
    }
    std::string title(EqHost& host) override {
        const net::LargeDialog* d = dialog(host);
        return d ? d->title : std::string();
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        const net::LargeDialog* d = dialog(host);
        if (!d) return;
        if (id == "LDW_TextBox") out.text = d->text;
        else if (id == "LDW_OkButton") out.shown = !d->buttons;
        else if (id == "LDW_YesButton") {
            out.shown = d->buttons;
            if (d->buttons && !d->yes.empty()) out.text = d->yes;
        } else if (id == "LDW_NoButton") {
            out.shown = d->buttons;
            if (d->buttons && !d->no.empty()) out.text = d->no;
        } else if (id == "LDW_SoundVolumeSlider" || id == "LDW_SoundVolumeValueLabel" || id == "LDW_ReplayButton" ||
                   id == "LDW_MuteButton") {
            out.shown = d->sound;
            if (id == "LDW_SoundVolumeSlider") out.slider_count = 100, out.slider_value = 100;
            if (id == "LDW_SoundVolumeValueLabel") out.text = "Vol. 100 %";
        }
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!dialog(host) || button != "left") return false;
        if (id == "LDW_OkButton") host.conn->answer_large_dialog(0);
        else if (id == "LDW_YesButton") host.conn->answer_large_dialog(1);
        else if (id == "LDW_NoButton") host.conn->answer_large_dialog(2);
        else return id == "LDW_ReplayButton" || id == "LDW_MuteButton";
        return true;
    }

private:
    static const net::LargeDialog* dialog(EqHost& host) {
        return host.conn && host.conn->large_dialog() ? &*host.conn->large_dialog() : nullptr;
    }
};

// ---------------------------------------------------------------- TargetOfTargetWindow (CTargetOfTargetWnd)

// CTargetOfTargetWnd::OnProcessFrame @0x768bb0: the HP and percent labels show while the target's target
// (PlayerZoneClient::TargetOfTarget, OP_TargetHoTT) is a known spawn; the gauge and labels are EQTypes 27 and 120.
// The original opens the window from the leadership ability Health of Target's Target (not built here), so it shows
// only as the character's UI ini leaves it.
class TargetOfTargetWindow : public StockWindow {
public:
    TargetOfTargetWindow() : StockWindow("TargetOfTargetWindow", false) {}
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        if (id == "TargetOfTarget_HPPercLabel" || id == "TargetOfTarget_HPLabel") {
            std::uint32_t t = host.conn->target_of_target();
            out.shown = t != 0 && host.conn->world().spawns.count(t) != 0;
        }
    }
};

// ---------------------------------------------------------------- ExtendedTargetWnd (CExtendedTargetWnd)

// CExtendedTargetWnd: a row per extended target slot (CExtendedTargetWnd_RefreshSlots: as many rows as the list
// has slots, OP_XTargetResponse's max: 5 + the AA's extra slots on EQEmu); the gauges, mana / endurance gauges and
// percent labels are EQTypes 42+n, 62+n, 82+n and 151+n (app/lua_eq_values.cpp). 0x54a270 per row: the gauge's
// text colour (CRNormal) is the spawn's con colour (PcClient::GetConLevel, GetRGBAFromIndex; the layout's for no
// spawn or con 0); the HP labels show with a spawn; the mana gauge with Mana Display on, a spawn that is a player or
// a mercenary (+0x125 0 or +0x208) and a casting class (EQ_Character::IsSpellcaster_3 @0x59fb90 on the spawn's
// class +0xeb8); the endurance gauge likewise without the class test (0x59fbd0 returns 1); the aggro label with
// the option on and a spawn. The role button (CExtendedTargetWnd_UpdateAutoHateIcon @0x549a20) shows the type's
// A_TargetRoleIcon cell with the type's name (__GetXTargetType) as its tool tip, hidden for a type without a cell
// (Empty Target). A click on a row (notification 1) makes the slot's spawn the target, none when it has none.
// This client: a slot the server extension hides (GameConnection::xtarget_slot_hidden) shows no row at all.
//
// The context menu (CreateContextMenu @0x54a410, BuildTargetTypeMenu @0x549700, WndNotification @0x54ae00): "Window"
// (the default window menu: this client's window menu), the 26 slot types (eqstr table 0xada198; the selected slot's
// type checked through the table at 0xada218, Current Target never; Current Target enabled with a target, Target's
// Target with the leadership ability 0xe or 0x1d), Auto Add Hater Targets (the list's flag, OP_XTargetAutoAddHaters
// when it changes), Mana Display / Endurance Display / Show Player Aggro Percent (the window's +0x4c0..+0x4c2,
// on by default; the last only with expansion bit 0x40000), and Save/Load target sets: Load (the used role sets,
// 0x96 + n), Save (a name, CTextEntryWnd, at most 24 characters: saved over the set of that name or into the first
// unused one, eqstr 4609, or 4611 with all ten used) and Delete (0xc8 + n, confirmed: eqstr 9065, then 4613); Load
// and Delete are disabled without a used set. A type sends OP_XTargetRequest {1, slot, type, name} unless the slot
// already has that type and name (GameConnection::xtarget_request); Current Target sends 2 (a PC target, with its
// name) or 3 (an NPC). A right click on a slot's role button selects that slot for the menu (notification 3).
//
// The window's ini keys Manabars, Endurancebars, AutoAddHaters (default true) and AggroPct (LoadIniInfo /
// StoreIniInfo). The slot types live in the character ini: [ExternalTargetRoles] Current, the role string
// "1^Autosave^..." (CExtendedTargetWnd_SaveCurrentRoles @0x54a850 at SaveGameUI; restored by
// CExtendedTargetWnd_LoadCurrentRoles @0x54b6d0 from unk_ApplyWindowPositions), and TargetSet1..10, the role sets
// (ReadCharacterIniSettings / SaveExtendedTargetRoleSetsToIni). Here the roles are restored once per zone, when the
// server has sent the slot count, and saved whenever they change after that (docs/unknowns.md).
class ExtendedTargetWindow : public StockWindow {
public:
    ExtendedTargetWindow() : StockWindow("ExtendedTargetWnd", false) {}
    void ini_loaded(EqHost&, ui::UiIni& ini, const std::filesystem::path&) override {
        ini_ = &ini;
        auto read = [&](const char* key, bool fallback) {
            auto v = ini.get("ExtendedTargetWnd", key);
            if (!v || v->empty()) return fallback;
            char c = (*v)[0];
            return c == 't' || c == 'T' || c == '1';
        };
        mana_ = read("Manabars", mana_);
        endurance_ = read("Endurancebars", endurance_);
        aggro_ = read("AggroPct", aggro_);
        auto_haters_ = read("AutoAddHaters", true); // 0x578400: sent when it differs from the list's
        zone_ = 0;                                  // sent again with the next zone-in
    }
    void frame(EqHost& host) override {
        if (!host.conn || host.conn->state() != net::ConnectionState::InZone) return;
        net::GameConnection& c = *host.conn;
        const game::XTargetState& list = c.xtargets();
        // ReadCharacterIniSettings: the role sets, once per character file.
        if (host.actions && host.actions->char_ini_path() != sets_for_) {
            sets_for_ = host.actions->char_ini_path();
            sets_.load([&](const std::string& key) { return host.actions->char_ini_string("ExternalTargetRoles", key); });
        }
        if (list.zone != zone_) {
            zone_ = list.zone;
            roles_restored_ = false;
            c.set_xtarget_auto_haters(auto_haters_);
        }
        if (!roles_restored_ && !list.slots.empty() && host.actions && !host.actions->char_ini_path().empty()) {
            // CExtendedTargetWnd_LoadCurrentRoles: the saved roles onto the list.
            roles_restored_ = true;
            std::string current = host.actions->char_ini_string("ExternalTargetRoles", "Current");
            game::xtarget_apply_role_string(current, list.slots.size(), nullptr,
                                            [&c](int slot, std::uint32_t type, const std::string& name) {
                                                c.xtarget_request(slot, type, name);
                                            });
            saved_roles_ = game::xtarget_role_string(c.xtargets(), "Autosave", 1);
        }
        if (roles_restored_ && host.actions) {
            // CExtendedTargetWnd_SaveCurrentRoles.
            std::string roles = game::xtarget_role_string(list, "Autosave", 1);
            if (roles != saved_roles_) {
                saved_roles_ = roles;
                host.actions->write_char_ini("ExternalTargetRoles", "Current", roles);
            }
        }
        if (list.auto_haters != stored_auto_haters_) store(host); // /xtarget auto stores the window's settings
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        int n = slot_of(id);
        if (n < 0) return;
        const auto& xs = host.conn->xtargets().slots;
        if (n >= static_cast<int>(xs.size()) || host.conn->xtarget_slot_hidden(n)) {
            out.shown = false; // past the list, or a slot the server extension leaves out (NMS: the player's pets)
            return;
        }
        const game::XTargetSlot& slot = xs[static_cast<std::size_t>(n)];
        auto it = host.conn->world().spawns.find(slot.spawn_id);
        const game::Spawn* spawn = it != host.conn->world().spawns.end() ? &it->second : nullptr;
        bool pc_or_merc = spawn && !spawn->is_corpse && (!spawn->is_npc || spawn->mercenary);
        if (id.rfind("ETW_HPLabel", 0) == 0 || id.rfind("ETW_HPPercLabel", 0) == 0) {
            out.shown = spawn != nullptr;
        } else if (id.rfind("ETW_ManaGauge", 0) == 0) {
            out.shown = mana_ && pc_or_merc && spellcaster_class(spawn->class_id);
        } else if (id.rfind("ETW_STAGauge", 0) == 0) {
            out.shown = endurance_ && pc_or_merc;
        } else if (id.rfind("ETW_AggroPct", 0) == 0) {
            out.shown = aggro_ && spawn;
        } else if (id.rfind("ETW_Gauge", 0) == 0 && spawn) {
            game::Con con = game::con_level(*spawn, host.conn->player_status().level, host.conn->pvp_rules());
            if (con != game::Con::None) out.color = argb_to_rgb(0xff000000u | game::con_rgb(con));
        } else if (id.rfind("ETW_Role", 0) == 0) {
            int cell = game::xtarget_role_icon_cell(slot.type);
            out.shown = cell >= 0;
            const char* name = game::xtarget_type_name(slot.type);
            out.tooltip = cell >= 0 && name ? name : "AutoHate";
            // Without the game's UI art (ui.ini game_art = icons) the pack draws the cell ("A_TargetRoleIcon#n").
            if (cell >= 0) {
                auto art = host.textures ? host.textures->grid_cell("A_TargetRoleIcon", cell) : std::nullopt;
                out.decal.kind = art ? ChildArt::Image : ChildArt::Anim;
                if (art) out.decal.cell = *art;
                else out.decal.anim = "A_TargetRoleIcon#" + std::to_string(cell);
            }
        }
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn) return false;
        if (id.rfind("ETW_Role", 0) == 0) {
            if (button == "rightup" || button == "rightupheld") {
                host.conn->select_xtarget(slot_of(id)); // notification 3: this slot for the menu, then the menu
                ImVec2 mp = ImGui::GetMousePos();
                context_menu(host, mp.x, mp.y);
            } else if (button == "left") {
                target_row(host, slot_of(id));
            }
            return true;
        }
        if (button != "left" || id.rfind("ETW_Gauge", 0) != 0) return false;
        target_row(host, slot_of(id));
        return true;
    }
    bool context_menu(EqHost& host, float x, float y) override {
        if (!host.conn) return false;
        std::vector<EqHost::MenuRow> rows;
        EqHost::MenuRow window;
        window.text = eqstr(host, 0x19f3); // Window
        std::string screen = screen_;
        window.act = [&host, screen] { host.window_menu = screen; };
        rows.push_back(std::move(window));
        rows.push_back(separator());
        static constexpr std::uint32_t kNames[30] = {6858, 6379, 6380, 6381, 6382, 6383, 6384, 6385, 6386, 6387,
                                                     6388, 6389, 6390, 6391, 6392, 6393, 6394, 6395, 6396, 6397,
                                                     6398, 6399, 6400, 6401, 6402, 6403, 6404, 7604, 7605, 13339};
        int checked = -1;
        const game::XTargetState& list = host.conn->xtargets();
        if (list.selected >= 0 && list.selected < static_cast<int>(list.slots.size())) {
            std::uint32_t t = list.slots[static_cast<std::size_t>(list.selected)].type;
            int item = t == 0 ? 0 : t == 1 ? 1 : t <= 3 ? 2 : static_cast<int>(t) - 1; // the table at 0xada218
            if (t <= 26 && item != 2) checked = item;
        }
        bool pvp_aggro = host.character && (host.character->sheet().expansions & 0x40000) != 0;
        for (int i = 0; i < 30; ++i) {
            if (i == 29 && !pvp_aggro) {
                rows.push_back(separator());
                continue;
            }
            EqHost::MenuRow r;
            r.text = eqstr(host, kNames[i]);
            if (i < 26) {
                r.check = i == checked;
                if (i == 2) r.enabled = host.conn->world().target_id != 0;
                if (i == 3) r.enabled = false; // Leadership_IsAbilityActive(0xe / 0x1d): not kept by this client
                if (!r.enabled) r.rgb = EqHost::kMenuDisabledRgb; // CContextMenu::EnableMenuItem(row, false)
                r.act = [this, &host, i] { choose_type(host, i); };
            } else if (i == 26) {
                r.check = list.auto_haters;
                r.act = [this, &host] {
                    host.conn->set_xtarget_auto_haters(!host.conn->xtargets().auto_haters);
                    store(host);
                };
            } else {
                bool& flag = i == 27 ? mana_ : i == 28 ? endurance_ : aggro_;
                r.check = flag;
                r.act = [this, &host, &flag] {
                    flag = !flag;
                    store(host);
                };
            }
            rows.push_back(std::move(r));
            if (i == 25 || i == 26 || i == 29) rows.push_back(separator());
        }
        EqHost::MenuRow sets;
        sets.text = eqstr(host, 4610); // Save/Load target sets
        sets.submenu = [this, &host] { return target_set_menu(host); };
        rows.push_back(std::move(sets));
        host.open_menu(x, y, std::move(rows));
        return true;
    }

private:
    static EqHost::MenuRow separator() {
        EqHost::MenuRow r;
        r.separator = true;
        return r;
    }
    // IsSpellcaster_3's classes: CLR PAL RNG SHD DRU BRD SHM NEC WIZ MAG ENC BST.
    static bool spellcaster_class(std::uint8_t c) {
        return (c >= 2 && c <= 6) || c == 8 || (c >= 10 && c <= 15);
    }
    // WndNotification 1: pinstTarget = the slot's spawn (null without one).
    static void target_row(EqHost& host, int slot) {
        if (host.conn->xtarget_slot_hidden(slot)) return; // no row
        const auto& xs = host.conn->xtargets().slots;
        std::uint32_t id = slot >= 0 && slot < static_cast<int>(xs.size()) ? xs[static_cast<std::size_t>(slot)].spawn_id : 0;
        host.conn->set_target(host.conn->world().spawns.count(id) ? id : 0);
    }
    // WndNotification 0x14 with 0x32 + i, on the list's selected slot.
    void choose_type(EqHost& host, int item) {
        if (!host.conn) return;
        std::uint32_t type = item <= 1 ? static_cast<std::uint32_t>(item) : static_cast<std::uint32_t>(item) + 1;
        std::string name;
        if (item == 2) {
            auto tit = host.conn->world().spawns.find(host.conn->world().target_id);
            if (tit == host.conn->world().spawns.end() || tit->second.is_corpse) return;
            type = tit->second.is_npc ? game::kXTargetSpecificNpc : game::kXTargetSpecificPc;
            name = tit->second.name;
        }
        host.conn->xtarget_request(host.conn->xtargets().selected, type, name);
    }
    // The Save/Load target sets submenu (CreateContextMenu's +0x4bc: Load +0x4b4, Save 0xfa, Delete +0x4b8).
    std::vector<EqHost::MenuRow> target_set_menu(EqHost& host) {
        auto list_sets = [this, &host](bool remove) {
            std::vector<EqHost::MenuRow> sub;
            for (int i = 0; i < game::XTargetRoleSets::kCount; ++i) {
                if (!sets_.sets[static_cast<std::size_t>(i)].used) continue;
                EqHost::MenuRow r;
                r.text = sets_.sets[static_cast<std::size_t>(i)].name;
                r.act = [this, &host, i, remove] { remove ? ask_delete_set(host, i) : apply_set(host, i); };
                sub.push_back(std::move(r));
            }
            return sub;
        };
        bool any = !list_sets(false).empty();
        std::vector<EqHost::MenuRow> sub(3);
        sub[0].text = eqstr(host, 0x11fc); // Load target set
        sub[1].text = eqstr(host, 0x11fd); // Save target set
        sub[2].text = eqstr(host, 0x11fe); // Delete target set
        for (int k : {0, 2}) {
            sub[static_cast<std::size_t>(k)].enabled = any;
            if (!any) sub[static_cast<std::size_t>(k)].rgb = EqHost::kMenuDisabledRgb;
            else sub[static_cast<std::size_t>(k)].submenu = [list_sets, k] { return list_sets(k == 2); };
        }
        sub[1].act = [this, &host] {
            // 0xfa: CTextEntryWnd::Activate(this, 0, eqstr 0x11ff, 1, 0x18, "").
            EqHost::TextEntry e;
            e.prompt = eqstr(host, 0x11ff);
            e.max_chars = 0x18;
            e.done = [this, &host](const std::string& text) { save_set(host, text); };
            host.text_entry = std::move(e);
        };
        return sub;
    }
    std::function<void(const std::string&, const std::string&)> ini_writer(EqHost& host) {
        return [&host](const std::string& key, const std::string& value) {
            if (host.actions) host.actions->write_char_ini("ExternalTargetRoles", key, value);
        };
    }
    // WndNotification 0x26: the typed name saved as a role set with the current roles.
    void save_set(EqHost& host, const std::string& text) {
        if (!host.conn) return;
        int found = sets_.slot_for_name(text);
        if (found < 0) {
            host.conn->show_eqstr(0x1203, {"10"}, 0xd);
            return;
        }
        game::XTargetRoleSet& set = sets_.sets[static_cast<std::size_t>(found)];
        set.used = set.dirty = true;
        set.name = text;
        set.roles = game::xtarget_role_string(host.conn->xtargets(), text, 1);
        std::string name = set.name;
        sets_.sort_and_save(ini_writer(host));
        host.conn->show_eqstr(0x1201, {name}, 0xf);
    }
    // Role set items 0x96..0x9f: the set's roles onto the list, then 0x1200.
    void apply_set(EqHost& host, int index) {
        if (!host.conn) return;
        const game::XTargetRoleSet& set = sets_.sets[static_cast<std::size_t>(index)];
        if (!set.used) return;
        net::GameConnection& c = *host.conn;
        game::xtarget_apply_role_string(set.roles, c.xtargets().slots.size(), nullptr,
                                        [&c](int slot, std::uint32_t type, const std::string& name) {
                                            c.xtarget_request(slot, type, name);
                                        });
        c.show_eqstr(0x1200, {set.name}, 0xf);
    }
    // Role set items 0xc8..0xd1: ConfirmationDialog 0x64 (eqstr 0x2369, 10 s); yes (vftable_off220 @0x54a1c0):
    // 0x1205 with the name, the set unused, the sets sorted and saved.
    void ask_delete_set(EqHost& host, int index) {
        if (!host.conn || !sets_.sets[static_cast<std::size_t>(index)].used) return;
        std::string name = sets_.sets[static_cast<std::size_t>(index)].name;
        EqHost::Confirm c;
        c.text = eqstr(host, 0x2369, {name});
        c.timeout_ms = 10000;
        c.done = [this, &host, index, name](bool yes) {
            if (!yes || !host.conn) return;
            game::XTargetRoleSet& set = sets_.sets[static_cast<std::size_t>(index)];
            if (set.name != name) return; // the sets were sorted since
            set.used = false;
            set.dirty = true;
            sets_.sort_and_save(ini_writer(host));
            host.conn->show_eqstr(0x1205, {name}, 0xf);
        };
        host.confirm = std::move(c);
    }
    void store(EqHost& host) {
        stored_auto_haters_ = host.conn ? host.conn->xtargets().auto_haters : auto_haters_;
        auto_haters_ = stored_auto_haters_;
        if (!ini_) return;
        ini_->put("ExtendedTargetWnd", "Manabars", mana_ ? "true" : "false");
        ini_->put("ExtendedTargetWnd", "Endurancebars", endurance_ ? "true" : "false");
        ini_->put("ExtendedTargetWnd", "AutoAddHaters", stored_auto_haters_ ? "true" : "false");
        ini_->put("ExtendedTargetWnd", "AggroPct", aggro_ ? "true" : "false");
    }
    // The row a child belongs to: its name's trailing number.
    static int slot_of(std::string_view id) {
        if (id.rfind("ETW_", 0) != 0) return -1;
        std::size_t end = id.size(), start = end;
        while (start > 0 && id[start - 1] >= '0' && id[start - 1] <= '9') --start;
        if (start == end) return -1;
        return std::atoi(std::string(id.substr(start)).c_str());
    }

    ui::UiIni* ini_ = nullptr;
    // The ctor's +0x4c0..+0x4c2 = 1, 1, 1 (LoadIniInfo reads them as the defaults).
    bool mana_ = true, endurance_ = true, aggro_ = true;
    bool auto_haters_ = true;        // the ini's AutoAddHaters, sent to the list each zone
    bool stored_auto_haters_ = false; // the list's flag as last stored (set at the first frame in zone)
    std::uint32_t zone_ = 0;         // the list's zone count the settings were sent for
    bool roles_restored_ = false;
    std::string saved_roles_;
    game::XTargetRoleSets sets_;
    std::filesystem::path sets_for_;
};

} // namespace

namespace {

// ---------------------------------------------------------------- TitleWnd (CTitleWnd)

// The title window. CTitleWnd is not decoded; this follows its XML over EQEmu's titles: the current and the proposed
// name ("<prefix> <name> <suffix>"), the titles (Title_AvailableTitles: the server's list with a prefix) and the
// suffixes (Title_AvailableSuffixes) with Change and Clear (OP_SetTitle), and the new-titles notice check box.
// Opening it asks the server for the list (OP_RequestTitles).
class TitleWindow : public StockWindow {
public:
    TitleWindow() : StockWindow("TitleWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->titles().open; }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn && show != host.conn->titles().open) host.conn->set_titles_open(show);
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const game::Spawn* me = host.conn->world().player();
        std::string name = me ? game::display_name(me->name) : std::string();
        std::string title = me ? me->title : std::string(), suffix = me ? me->suffix : std::string();
        if (id == "Title_CurrentName") out.text = full(title, name, suffix);
        else if (id == "Title_ProposedName") {
            const auto* p = entry(host, prefix_sel_, false);
            const auto* s = entry(host, suffix_sel_, true);
            out.text = full(p ? p->prefix : title, name, s ? s->suffix : suffix);
        } else if (id == "Title_NotifyNewTitles") out.checked = host.conn->titles().notify;
        else if (id == "Title_ChangeTitlePrefix") out.enabled = entry(host, prefix_sel_, false) != nullptr;
        else if (id == "Title_ChangeTitleSuffix") out.enabled = entry(host, suffix_sel_, true) != nullptr;
        else if (id == "Title_ClearTitlePrefix") out.enabled = !title.empty();
        else if (id == "Title_ClearTitleSuffix") out.enabled = !suffix.empty();
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        bool suffixes = id == "Title_AvailableSuffixes";
        if (!suffixes && id != "Title_AvailableTitles") return nullptr;
        ListData& d = suffixes ? suffixes_ : prefixes_;
        d = {};
        d.labels = {suffixes ? "Suffix" : "Title"};
        d.widths = {200};
        for (const auto* e : entries(host, suffixes)) d.rows.push_back(ListRow{{suffixes ? e->suffix : e->prefix}});
        d.selected = suffixes ? suffix_sel_ : prefix_sel_;
        return &d;
    }
    bool list_click(EqHost&, std::string_view id, int row, bool, int) override {
        if (id == "Title_AvailableTitles") prefix_sel_ = row;
        else if (id == "Title_AvailableSuffixes") suffix_sel_ = row;
        else return false;
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        if (id == "Title_NotifyNewTitles") c.set_titles_notify(!c.titles().notify);
        else if (id == "Title_ChangeTitlePrefix") {
            if (const auto* e = entry(host, prefix_sel_, false)) c.set_title(false, e->id);
        } else if (id == "Title_ChangeTitleSuffix") {
            if (const auto* e = entry(host, suffix_sel_, true)) c.set_title(true, e->id);
        } else if (id == "Title_ClearTitlePrefix") c.set_title(false, 0);
        else if (id == "Title_ClearTitleSuffix") c.set_title(true, 0);
        else return false;
        return true;
    }

private:
    static std::string full(const std::string& title, const std::string& name, const std::string& suffix) {
        std::string out = title.empty() ? name : title + " " + name;
        if (!suffix.empty()) out += " " + suffix;
        return out;
    }
    static std::vector<const net::TitleEntry*> entries(EqHost& host, bool suffixes) {
        std::vector<const net::TitleEntry*> out;
        for (const auto& e : host.conn->titles().available)
            if (!(suffixes ? e.suffix : e.prefix).empty()) out.push_back(&e);
        return out;
    }
    static const net::TitleEntry* entry(EqHost& host, int row, bool suffixes) {
        auto e = entries(host, suffixes);
        return row >= 0 && row < static_cast<int>(e.size()) ? e[static_cast<std::size_t>(row)] : nullptr;
    }

    ListData prefixes_, suffixes_;
    int prefix_sel_ = -1, suffix_sel_ = -1;
};

// ---------------------------------------------------------------- PotionBeltWnd (CPotionBeltWnd)

// CPotionBeltWnd over GameConnection's belt (the profile's). min(5, 2 + SPA 362
// + PcClient +0x2ef0) usable slots (PcZoneClient_GetMaxPotionBeltSlots; +0x2ef0's source is not known: taken as 0).
// UpdateSlots @0x71f050 for each slot: beyond the usable ones or empty, no icon and empty texts; else the icon (cell
// 0x150 for icon 0), the item's name as the tooltip, and the count label: the items with that id the player
// carries (GameConnection::potion_belt_count). WndNotification @0x71f580: a left click with an item on the cursor
// (no Ctrl / Alt) stores it in the slot (Send_OP_PotionBelt @0x5853b0: {0, slot, item id}) and puts the cursor item
// away (cmd_autoinventory); a right click uses it (PcZoneClient_UsePotionBeltSlot: the item's clicky,
// GameConnection::potion_belt_use); a right button held (XWM_RCLICKHOLD, 0x18) sends {3, slot, Shift ? 2 : 0}. Not
// built: a left button held's hot button (the cursor's hot button attachment).
class PotionBeltWindow : public StockWindow {
public:
    PotionBeltWindow() : StockWindow("PotionBeltWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->potion_belt_open(); }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_potion_belt_open(show);
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        bool label = slot_of(id, "PotionSlotLabel") >= 0;
        int n = label ? slot_of(id, "PotionSlotLabel") : slot_of(id, "PotionSlot");
        if (n < 0) return;
        const net::ProfileBeltItem* b = n < max_slots(host) ? host.conn->potion_belt_item(n) : nullptr;
        if (label) {
            out.text = b ? std::to_string(host.conn->potion_belt_count(n)) : std::string();
            return;
        }
        out.tooltip = b ? b->name : std::string();
        // PostDraw @0x71ee90: the slot's recast cool-down (game::update_potion_belt_cool_down), drawn by the button.
        HostButtonQueries q(host);
        game::ButtonCoolDown& cd = cool_downs_[static_cast<std::size_t>(n)];
        if (const net::Item* it = b ? host.conn->potion_belt_first_item(n) : nullptr)
            game::update_potion_belt_cool_down(cd, item_recast_of(*host.conn, it), q.now(), q.fast_now());
        out.cool_down = cd.overlay_fraction(q.now());
        if (!b) {
            out.decal.kind = ChildArt::None;
        } else if (host.textures) {
            if (auto cell = host.textures->item_icon(b->icon == 0 ? 836u : b->icon)) { // cell 0x150
                out.decal.kind = ChildArt::Image;
                out.decal.cell = *cell;
            }
        }
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool outside) override {
        int n = slot_of(id, "PotionSlot");
        if (!host.conn || n < 0) return false;
        const ImGuiIO& io = ImGui::GetIO();
        if (button == "left") {
            if (!host.conn->inventory().cursor() || io.KeyCtrl || io.KeyAlt || n >= max_slots(host)) return true;
            if (host.conn->potion_belt_set(n)) host.conn->auto_inventory();
        } else if ((button == "right" || button == "rightup") && !outside) {
            if (n < max_slots(host)) host.conn->potion_belt_use(n);
        } else if (button == "rightheld") {
            host.conn->potion_belt_hold(n, io.KeyShift);
        }
        return true;
    }

private:
    std::array<game::ButtonCoolDown, 5> cool_downs_{}; // the slot buttons' CButtonWnd +0x21c / +0x220
    static int max_slots(EqHost& host) {
        int spa = host.character ? host.character->total_effect(0x16a, true, 0, true, true) : 0;
        return std::min(5, 2 + spa);
    }
    // "PotionSlot3" → 3; the label ids ("PotionSlotLabel3") are not slots.
    static int slot_of(std::string_view id, std::string_view prefix) {
        if (id.rfind(prefix, 0) != 0 || id.size() != prefix.size() + 1) return -1;
        char c = id.back();
        return c >= '0' && c <= '4' ? c - '0' : -1;
    }
};

// ---------------------------------------------------------------- TipWindow (CTipWnd, the context tips)

// The context tips' table (0xb503ac, stride 0x58): the page in help\tips\ (+0), the UI ini key of its "show" flag
// (+0x30; [Tips], default true, +0x50). Tips 0..89, every entry kept so the ids stay the original's (the voice macro
// 0x38 and audio trigger 0x46 tips have no window here); Activate takes ids below 0x5b.
struct ContextTip {
    const char* page;
    const char* key;
};
constexpr ContextTip kContextTips[] = {
    {"ctxinventory.html", "Inventory"}, {"ctxspellbook.html", "Spellbook"}, {"ctxaa.html", "AAWindow"},
    {"ctxactionmelee.html", "ActionMelee"}, {"ctxactionskills.html", "ActionSkill"},
    {"ctxactionsocials.html", "ActionSocial"}, {"ctxdisplayoptions.html", "OptDisplay"},
    {"ctxkeyboardoptions.html", "OptKeys"}, {"ctxdye.html", "DyeWindow"}, {"ctxexperience.html", "ExpUp"},
    {"ctxplayercustomization.html", "PlayerCustomization"}, {"ctxfriends.html", "Friends"},
    {"ctxgroupinvite.html", "GrpInvite"}, {"ctxguild.html", "GuildWindow"}, {"ctxjournal.html", "JournalWindow"},
    {"ctxskills.html", "SkillsWindow"}, {"ctxstory.html", "StoryWindow"}, {"ctxmerchant.html", "Merchant"},
    {"ctxbank.html", "Bank"}, {"ctxtrainer.html", "Trainer"}, {"ctxgive.html", "GiveNPC"}, {"ctxtrade.html", "Trade"},
    {"ctxsit.html", "Sit"}, {"ctxlooting.html", "Loot"}, {"ctxlevelten.html", "Level10"},
    {"ctxguildinvite.html", "GuildInvited"}, {"ctxduel.html", "DuelInvited"}, {"ctxcontainer.html", "WorldContainer"},
    {"ctxconsent.html", "DragConsent"}, {"ctxcon.html", "Consider"}, {"ctxfaction.html", "Faction"},
    {"ctxdeath.html", "Death"}, {"ctxredcon.html", "RedCon"}, {"ctxaugmentation.html", "Augment"},
    {"ctxinteractive.html", "InterObj"}, {"ctxsealer.html", "Sealer"}, {"ctxadvstats.html", "AdvStats"},
    {"ctxadvmerchant.html", "AdvMerchant"}, {"ctxadvleader.html", "AdvLeader"}, {"ctxadventure.html", "AdvRequest"},
    {"ctxquantity.html", "Quantity"}, {"ctxleadership.html", "Leadership"},
    {"ctxtributebenefit.html", "TributeBenefits"}, {"ctxtributemaster.html", "TributeMaster"},
    {"ctxtradeskillinterface.html", "TradeskillWnd"}, {"ctxgamma.html", "Gamma"},
    {"ctxcombatabilities.html", "CombatAbilities"}, {"ctxexpedition.html", "Expedition"},
    {"ctxpvpmerchant.html", "PvPMerchant"}, {"ctxpvpleader.html", "PvpLeader"}, {"ctxpvpstats.html", "PvpStats"},
    {"ctxtask.html", "TaskWnd"}, {"ctxtaskmerchant.html", "TaskMerchantWnd"},
    {"ctxguildtributemaster.html", "GuildTributeMaster"}, {"ctxtaskselect.html", "TaskSelect"},
    {"ctxraidwindow.html", "RaidWnd"}, {"ctxvoicemacro.html", "VoiceMacroWnd"}, {"ctxtitles.html", "Titles"},
    {"ctxbarter.html", "BarterBuyer"}, {"ctxbartersearch.html", "BarterSearch"},
    {"ctxbartermerchant.html", "BarterMerchant"}, {"ctxlfguild.html", "LFGuild"}, {"ctxguildbank.html", "GuildBank"},
    {"ctxcharitymerchant.html", "CharityMerchantWnd"}, {"ctxpointmerchant.html", "PointMerchantWnd"},
    {"ctxbandolier.html", "BandolierWnd"}, {"ctxpotionbelt.html", "PotionBeltWnd"}, {"ctxmail.html", "MailWindow"},
    {"ctxmailcomposition.html", "MailCompositionWindow"}, {"ctxaddressbook.html", "MailAddressBookWindow"},
    {"ctxaudiotrigger.html", "AudioTrigger"}, {"ctxtasktemplateselect.html", "TaskTemplateSelectWnd"},
    {"ctxitemexptransfer.html", "ItemExpTransferWnd"}, {"ctxclaims.html", "ClaimWindow"},
    {"ctxpetinfownd.html", "PetInfoWnd"}, {"ctxrealestateitems.html", "RealEstateItemsWnd"},
    {"ctxrealestatemanage.html", "RealEstateManageWnd"}, {"ctxrealestateneighborhood.html", "RealEstateNeighborhoodWnd"},
    {"ctxrealestatepurchase.html", "RealEstatePurchaseWnd"}, {"ctxrealestateplotsearch.html", "RealEstatePlotSearchWnd"},
    {"ctxtributetrophy.html", "TributeTrophyWnd"}, {"ctxrealestateoverview.html", "RealEstateOverviewWnd"},
    {"ctxvote.html", "VoteWnd"}, {"ctxvoteresults.html", "VoteResultsWnd"},
    {"ctxrealestatelayout.html", "RealEstateLayoutDetailsWnd"}, {"ctxsendmoney.html", "SendMoneyWnd"},
    {"ctxtributeguildtrophy.html", "GuildTributeTrophyWnd"}, {"ctxhotbarhelp.html", "HotButtonWnd"},
    {"ctxzoneguide.html", "ZoneGuideWnd"}, {"ctxitemdisplay.html", "ItemDisplayWnd"},
};
constexpr int kContextTipCount = static_cast<int>(std::size(kContextTips));

// CTipWnd: one class, two windows on the same screen. The tip of the day window
// (pinstCTipWnd_1, mode 0; here "TipWindow#2") and the context tip window (pinstCTipWnd_2, mode 1; "TipWindow").
// The constructor @0x776ab0 shows Show Another Tip in mode 0 and History and the "don't show this" pair in mode 1;
// mode 0 loads help\tips\totd.txt (LoadDayTips @0x776780). sub_00775e00 reads [Tips] ShowTips, then ShowTOTD (mode 0)
// or every context tip's flag (mode 1; the table at 0xb503ac, default true). Activate @0x776ef0 (not in a tutorial
// zone):
//   mode 0: with `force` or ShowTOTD, tip -1 a random one (SelectRandomTip @0x775f00: rand() % count, moved on past
//     the last five shown when there are more than five), else tip n; SetTOTD @0x775f50 puts its text in Display.
//   mode 1: an id below 0x5b, with `force` or ShowTips and the tip's own flag: AddContextTip @0x776410 moves the id to
//     the front of the history, loads help\tips\<page> into Display (CStmlWnd::LoadPage) and keeps the page's title
//     (the <TITLE> ParseSTMLHead reads; the page name when it has none) for the History combo, rebuilt newest first;
//     ShowHistoryTip @0x775fd0 shows history entry n: the combo's choice, the page, the title as the window's text.
// UpdateButtons @0x775c00 keeps the boxes checked as the flags say. WndNotification @0x776ff0: Show Another Tip a new
// random tip; Close deactivates; NoShowButton sets ShowTOTD (mode 0) or ShowTips (mode 1) to !checked,
// NoShowThisButton the shown tip's flag (WriteBoolToUIINI "Tips"); History shows its choice; a page's link
// WndNotify 0x1b activates tip <data> (forced), WndNotify 1000 opens help page <data> in the help window.
// Not built: the tutorial zones' check (EQWorldData_IsTutorialZone: the zone's flag 0x1000000); the pages' new lines
// are taken as spaces (ParseSTML has no case for them).
class TipWindow : public StockWindow {
public:
    // Activate's `force` argument: activate(tip + kForce) (the help window's Tips button, a page's 0x1b link).
    static constexpr int kForce = 0x100000;

    TipWindow(std::string screen, int mode) : StockWindow(std::move(screen), false), mode_(mode) {}

    void ini_loaded(EqHost& host, ui::UiIni& ini, const std::filesystem::path&) override {
        ini_ = &ini;
        auto read = [&](const char* key, bool fallback) {
            auto v = ini.get("Tips", key);
            if (!v || v->empty()) return fallback;
            char c = (*v)[0];
            return c == 't' || c == 'T' || c == '1';
        };
        show_tips_ = read("ShowTips", show_tips_);
        if (mode_ == 0) {
            show_totd_ = read("ShowTOTD", show_totd_);
            load_day_tips(host);
        } else {
            for (int i = 0; i < kContextTipCount; ++i)
                flags_[static_cast<std::size_t>(i)] = read(kContextTips[i].key, flags_[static_cast<std::size_t>(i)]);
        }
    }
    bool activate(EqHost& host, int arg) override {
        bool force = arg >= kForce / 2;
        int tip = force ? arg - kForce : arg;
        if (mode_ == 0) {
            if (!force && !show_totd_) return false;
            if (tip == -1) tip = select_random_tip();
            else if (tip >= static_cast<int>(day_tips_.size())) return false;
            set_totd(tip);
            open_ = true;
            return true;
        }
        if (tip < 0 || tip >= kContextTipCount ||
            (!force && (!show_tips_ || !flags_[static_cast<std::size_t>(tip)])))
            return false;
        // AddContextTip: to the front of the history (its old entry and title removed).
        std::string title;
        for (std::size_t i = 0; i < history_.size(); ++i)
            if (history_[i].first == tip) {
                title = history_[i].second;
                history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        std::string page = load_page(host, tip);
        if (title.empty()) title = page_title_;
        if (title.empty()) title = kContextTips[tip].page;
        history_.insert(history_.begin(), {tip, title});
        show(0, page);
        open_ = true;
        return true;
    }
    std::string title(EqHost&) override {
        return mode_ == 1 && open_ && current_ >= 0 ? history_[static_cast<std::size_t>(choice_)].second : std::string();
    }
    void child(EqHost&, std::string_view id, Child& out) override {
        if (id == "Display") out.text = text_;
        else if (id == "NextTipButton") out.shown = mode_ == 0;
        else if (id == "History" || id == "NoShowThisButton" || id == "NoShowThisLabel") out.shown = mode_ == 1;
        if (id == "NoShowButton") out.checked = mode_ == 0 ? !show_totd_ : !show_tips_;
        else if (id == "NoShowThisButton") out.checked = current_ >= 0 && !flags_[static_cast<std::size_t>(current_)];
    }
    const ComboData* combo(EqHost&, std::string_view id) override {
        if (id != "History" || mode_ != 1) return nullptr;
        combo_ = {};
        for (const auto& h : history_) combo_.choices.push_back(h.second);
        combo_.selected = choice_;
        return &combo_;
    }
    bool combo_select(EqHost& host, std::string_view id, int index) override {
        if (id != "History" || mode_ != 1 || index < 0 || index >= static_cast<int>(history_.size())) return false;
        show(index, load_page(host, history_[static_cast<std::size_t>(index)].first));
        return true;
    }
    bool click(EqHost&, std::string_view id, std::string_view button, bool) override {
        if (button != "left") return false;
        if (id == "NextTipButton" && mode_ == 0) {
            set_totd(select_random_tip());
        } else if (id == "CloseButton") {
            open_ = false;
        } else if (id == "NoShowButton") {
            if (mode_ == 0) {
                show_totd_ = !show_totd_;
                write("ShowTOTD", show_totd_);
            } else {
                show_tips_ = !show_tips_;
                write("ShowTips", show_tips_);
            }
        } else if (id == "NoShowThisButton" && mode_ == 1) {
            if (current_ < 0) return true;
            bool& f = flags_[static_cast<std::size_t>(current_)];
            f = !f;
            write(kContextTips[current_].key, f);
        } else {
            return false;
        }
        return true;
    }
    bool stml_link(EqHost& host, std::string_view id, int message, std::string_view data) override {
        if (id != "Display") return false;
        if (message == 0x1b) return activate(host, std::atoi(std::string(data).c_str()) + kForce);
        if (message == 1000 && host.open_help) {
            host.open_help(std::string(data));
            return true;
        }
        return false;
    }

private:
    // LoadDayTips: the entries between "###" lines, each line ended by "<BR>" (an entry of 10 characters or less is
    // dropped; at most 0x27ff).
    void load_day_tips(EqHost& host) {
        day_tips_.clear();
        if (!host.game) return;
        auto data = host.game->read("help/tips/totd.txt");
        if (!data) return;
        std::string file(reinterpret_cast<const char*>(data->data()), data->size());
        std::vector<std::string> lines;
        for (std::size_t pos = 0; pos < file.size();) {
            std::size_t nl = file.find('\n', pos);
            if (nl == std::string::npos) break; // fgets' last line, read with the end of the file: not taken
            lines.push_back(file.substr(pos, nl - pos));
            pos = nl + 1;
        }
        std::size_t i = 0;
        auto marker = [&](std::size_t k) { return lines[k].rfind("###", 0) == 0; };
        while (i < lines.size() && !marker(i)) ++i;
        while (i < lines.size()) {
            ++i; // past the "###"
            std::string tip;
            for (; i < lines.size() && !marker(i); ++i) {
                std::string line = lines[i];
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (tip.size() + line.size() > 0x27fe) line.resize(0x27fe - std::min<std::size_t>(tip.size(), 0x27fe));
                tip += line;
                if (tip.size() + 4 < 0x27ff) tip += "<BR>";
            }
            if (tip.size() > 10) day_tips_.push_back(std::move(tip));
        }
    }
    int select_random_tip() const {
        int count = static_cast<int>(day_tips_.size());
        if (count < 1) return -1;
        int tip = std::rand() % count;
        if (count > 5)
            while (std::find(recent_.begin(), recent_.end(), tip) != recent_.end())
                if (++tip >= count) tip = 0;
        return tip;
    }
    void set_totd(int tip) {
        if (tip < 0) return;
        for (std::size_t k = recent_.size() - 1; k > 0; --k) recent_[k] = recent_[k - 1];
        recent_[0] = tip;
        text_ = day_tips_[static_cast<std::size_t>(tip)];
    }
    std::string load_page(EqHost& host, int tip) {
        return load_stml_page(host.game, std::string("help/tips/") + kContextTips[tip].page, &page_title_);
    }
    void show(int index, std::string page) {
        choice_ = index;
        current_ = history_[static_cast<std::size_t>(index)].first;
        text_ = std::move(page);
    }
    void write(const char* key, bool v) {
        if (ini_) ini_->put("Tips", key, v ? "true" : "false");
    }

    int mode_;
    ui::UiIni* ini_ = nullptr;
    bool show_tips_ = true, show_totd_ = true;
    std::array<bool, kContextTipCount> flags_ = [] {
        std::array<bool, kContextTipCount> a{};
        a.fill(true);
        return a;
    }();
    std::vector<std::pair<int, std::string>> history_;
    std::vector<std::string> day_tips_;
    std::array<int, 5> recent_{-1, -1, -1, -1, -1};
    std::string page_title_, text_;
    int current_ = -1, choice_ = 0;
    ComboData combo_;
};

} // namespace

// Hailstone's player menu (a deliberate difference; game::opens_player_menu). The rows' words are this client's own:
// the original has no such menu to take them from.
std::vector<EqHost::MenuRow> player_menu_rows(EqHost& host, std::uint32_t spawn_id) {
    std::vector<EqHost::MenuRow> rows;
    if (!host.conn) return rows;
    const auto& world = host.conn->world();
    auto it = world.spawns.find(spawn_id);
    if (it == world.spawns.end() || !game::opens_player_menu(it->second, spawn_id == world.player_id)) return rows;
    const std::string name = it->second.name;
    EqHost::MenuRow title; // the player's name first, as the real estate item menu shows its item's
    title.text = game::display_name(name);
    title.rgb = 0x00ff00;
    title.enabled = false;
    rows.push_back(std::move(title));
    // A row acts only while the player is still here; the invites go through the target, as /invite and /raidinvite
    // without a name do, so their checks (an NPC, ourselves, the other deity team) all apply.
    auto on_player = [&host, spawn_id](std::function<void(net::GameConnection&)> fn) {
        return [&host, spawn_id, fn = std::move(fn)] {
            if (host.conn && host.conn->world().spawns.count(spawn_id)) fn(*host.conn);
        };
    };
    auto add = [&rows](const char* text, std::function<void()> act, bool enabled = true) {
        EqHost::MenuRow r;
        r.text = text;
        r.enabled = enabled;
        if (!enabled) r.rgb = EqHost::kMenuDisabledRgb;
        else r.act = std::move(act);
        rows.push_back(std::move(r));
    };
    add("Send Tell", [&host, spawn_id, name] {
        if (host.conn && host.conn->world().spawns.count(spawn_id)) chat_open_tell(name);
    });
    // While we are invited ourselves /invite accepts that invitation; the menu does not offer it then.
    add("Invite to Group", on_player([spawn_id](net::GameConnection& c) {
            c.set_target(spawn_id);
            c.group_invite();
        }),
        host.conn->group().inviter.empty());
    add("Invite to Raid", on_player([spawn_id](net::GameConnection& c) {
        c.set_target(spawn_id);
        c.raid_invite("");
    }));
    // As a left click on the player: a trade request (an item on the cursor goes into the trade when it opens).
    add("Trade", on_player([spawn_id](net::GameConnection& c) { c.give_to(spawn_id); }));
    // Whatever /toggleinspect says: it is asked for here.
    add("Inspect", on_player([spawn_id](net::GameConnection& c) { c.inspect(spawn_id); }));
    return rows;
}

bool open_player_menu(EqHost& host, std::uint32_t spawn_id, float x, float y) {
    auto rows = player_menu_rows(host, spawn_id);
    if (rows.empty() || !host.context_menus()) return false;
    host.open_menu(x, y, std::move(rows));
    return !host.menus.empty();
}

void add_player_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<TitleWindow>());
    out.push_back(std::make_unique<PotionBeltWindow>());
    out.push_back(std::make_unique<ExtendedTargetWindow>());
    out.push_back(std::make_unique<TargetOfTargetWindow>());
    out.push_back(std::make_unique<InspectWindow>());
    out.push_back(std::make_unique<ConfirmationWindow>());
    out.push_back(std::make_unique<BookWindow>());
    out.push_back(std::make_unique<NoteWindow>());
    out.push_back(std::make_unique<TextEntryWindow>());
    out.push_back(std::make_unique<BandolierWindow>());
    out.push_back(std::make_unique<LargeDialogWindow>());
    out.push_back(std::make_unique<TipWindow>("TipWindow", 1));
    out.push_back(std::make_unique<TipWindow>("TipWindow#2", 0));
}

} // namespace hailstone::app
