// The mail windows: MailWindow (CMailWnd) and MailCompositionWindow (CMailCompositionWnd) over GameConnection's
// UCS mail (game/mail.h, net/game_connection_mail.cpp). Rows, the message view and the reply text are built as the
// original's CMailWnd_AddMessageRow, CMailWnd_BuildMessageSTML and CMailWnd_ComposeReply build them.

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <format>
#include <set>
#include <string>
#include <vector>

#include "app/lua_ui_windows.h"
#include "core/atomic_file.h"
#include "core/character_files.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "game/mail.h"
#include "game/feature_restrictions.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

std::string eqstr(UiHost& host, std::uint32_t id, std::vector<std::string> args = {}) {
    return host.conn ? host.conn->strings().format(id, args) : std::string();
}

// ctime's text ("Wed Jun 30 21:49:08 1993") without its newline.
std::string ctime_text(std::uint32_t t) {
    std::time_t tt = static_cast<std::time_t>(t);
    char buf[64] = {};
    if (std::tm* tm = std::localtime(&tt)) std::strftime(buf, sizeof buf, "%a %b %d %H:%M:%S %Y", tm);
    return buf;
}

// The body's text as the message view shows it (CXStr_DecodeHtmlEntities; its line breaks as <BR>).
std::string body_stml(const std::string& body) {
    std::string out;
    for (char c : body) {
        if (c == '\n') out += "<BR>";
        else if (c != '\r') out += c;
    }
    return out;
}

// The composition window's state (CMailCompositionWnd): its title and the three fields.
struct Composition {
    bool open = false;
    std::string title, to, subject, text;
};
Composition& composition() {
    static Composition c;
    return c;
}

// CMailCompositionWnd::AboutToShow @0x6c1910: with GetGameFeature(6) == 0 (sending mail) the window stays hidden and
// GameFeatureRestrictions_FormatMessage(6) is printed in colour 0x14f; else it shows (fresh fields, `title`).
bool open_composition(UiHost& host, std::string title) {
    auto& c = composition();
    if (host.conn && host.conn->feature_refused(game::kFeatureSendMail)) return false;
    c = {};
    c.open = true;
    c.title = std::move(title);
    return true;
}

// The two books MailMgr keeps, each behind its own CMailAddressBookWnd: the address book (MailMgr +0x6c,
// MailAddressBookWindow) and the ignore list (MailMgr +0xa4, MailIgnoreListWindow), both in the same file format.
enum class Book { Address, Ignore };

// A book window's state (CMailAddressBookWnd): the fields being edited and where the book is saved.
struct AddressBookUi {
    bool open = false;
    std::string name, addresses, note;
    bool is_private = false;
    std::filesystem::path file; // <user dir>/AddressBook.txt or IgnoreList.txt (game/mail.h; core/character_files.h)
};
AddressBookUi& book_ui(Book b) {
    static AddressBookUi a[2];
    return a[b == Book::Address ? 0 : 1];
}
AddressBookUi& address_book_ui() { return book_ui(Book::Address); }

game::MailAddressBook& book_of(UiHost& host, Book b) {
    return b == Book::Address ? host.conn->mail_address_book() : host.conn->mail_ignore_list();
}

void save_book(UiHost& host, Book b) {
    const auto& a = book_ui(b);
    if (!host.conn || a.file.empty()) return;
    // AddressBook_Save @0x469dc0 after every change (AddressBook_AddOrUpdateEntry, AddressBook_RemoveEntry).
    std::error_code ec;
    std::filesystem::create_directories(a.file.parent_path(), ec);
    if (!write_file_atomic(a.file, book_of(host, b).save_text())) log::warn("cannot write {}", a.file.string());
}

// ---------------------------------------------------------------- MailWindow (CMailWnd)

// CMailWnd: the mailbox combo (CMailWnd_FillMailboxCombo: the account's mailboxes; a pick selects the mailbox),
// the inbox and the trash (CMailWnd_AddMessageRow: From, Subject, Date as ctime; new and unread rows in 0xffc896;
// each row's body is asked for), the selected message's view (CMailWnd_BuildMessageSTML: From, To, Subject, Date
// and Status labels in 0x8aa3ff, then the body), Compose, Reply, Reply All, Forward (CMailWnd_ComposeReply),
// Delete (to the trash; from the trash for good), Undelete and Empty. Opening the window selects the current
// mailbox again (AboutToShow: GameConnection::mail_open); the address book button opens the address book. Export,
// Report, Spam and the options are not built here.
class MailWindow : public StockWindow {
public:
    MailWindow() : StockWindow("MailWindow", false) {}

    bool shown(UiHost& host) override { return host.conn && host.conn->mail().window_open; }
    void set_shown(UiHost& host, bool show) override {
        if (!host.conn || show == host.conn->mail().window_open) return;
        if (show) host.conn->mail_open();
        else host.conn->mail_close();
    }

    const ComboData* combo(UiHost& host, std::string_view id) override {
        if (!host.conn || id != "CharacterBoxSelector") return nullptr;
        const auto& m = host.conn->mail();
        boxes_.choices.clear();
        for (const auto& b : m.mailboxes) boxes_.choices.push_back(game::mail_short_name(b));
        boxes_.selected = m.current;
        return &boxes_;
    }
    bool combo_select(UiHost& host, std::string_view id, int index) override {
        if (!host.conn || id != "CharacterBoxSelector") return false;
        host.conn->mail_select_mailbox(index);
        inbox_sel_ = trash_sel_ = 0;
        return true;
    }

    const ListData* list(UiHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& m = host.conn->mail();
        bool trash = id == "TrashList";
        if (!trash && id != "InboxList") return nullptr;
        ListData& d = trash ? trash_ : inbox_;
        std::vector<std::uint32_t>& ids = trash ? trash_ids_ : inbox_ids_;
        d = {};
        d.columns = 3;
        d.labels = {"From", "Subject", "Date"};
        d.widths = {200, 200, 200};
        ids.clear();
        for (const auto* h : trash ? m.trash() : m.inbox()) {
            ListRow r;
            r.texts = {game::mail_short_name(h->from), h->subject, ctime_text(h->timestamp)};
            if (h->status == game::MailStatus::New || h->status == game::MailStatus::Unread) r.color = argb_to_rgb(0xffffc896);
            // MailMgr_RequestMessageBody for every listed row (asked once here).
            if (!h->body && asked_.insert(h->id).second) host.conn->mail_request_body(h->id);
            ids.push_back(h->id);
            d.rows.push_back(std::move(r));
        }
        std::uint32_t sel = trash ? trash_sel_ : inbox_sel_;
        for (std::size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == sel) d.selected = static_cast<int>(i);
        return &d;
    }
    bool list_click(UiHost& host, std::string_view id, int row, bool, int) override {
        if (!host.conn) return false;
        bool trash = id == "TrashList";
        if (!trash && id != "InboxList") return false;
        auto& ids = trash ? trash_ids_ : inbox_ids_;
        if (row < 0 || row >= static_cast<int>(ids.size())) return true;
        std::uint32_t mid = ids[static_cast<std::size_t>(row)];
        (trash ? trash_sel_ : inbox_sel_) = mid;
        // CMailWnd_OpenSelectedMessage: a new or unread message becomes read.
        auto it = host.conn->mail().headers.find(mid);
        if (it != host.conn->mail().headers.end()) {
            host.conn->mail_request_body(mid);
            if (!trash && (it->second.status == game::MailStatus::New || it->second.status == game::MailStatus::Unread))
                host.conn->mail_set_status({mid}, game::MailStatus::Read);
        }
        return true;
    }

    void child(UiHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& m = host.conn->mail();
        auto header = [&](std::uint32_t mid) -> const game::MailHeader* {
            auto it = m.headers.find(mid);
            return it == m.headers.end() ? nullptr : &it->second;
        };
        const game::MailHeader* in = header(inbox_sel_);
        const game::MailHeader* tr = header(trash_sel_);
        if (in && (in->status == game::MailStatus::Trash)) in = nullptr;
        if (tr && tr->status != game::MailStatus::Trash) tr = nullptr;
        if (id == "Body") out.text = in ? view(host, *in) : std::string();
        else if (id == "TrashBody") out.text = tr ? view(host, *tr) : std::string();
        else if (id == "ButtonDelete" || id == "ButtonReply" || id == "ButtonReplyAll" || id == "ButtonForward")
            out.enabled = in != nullptr;
        else if (id == "ButtonTrashUndelete" || id == "ButtonTrashReply" || id == "ButtonTrashReplyAll" ||
                 id == "ButtonTrashForward")
            out.enabled = tr != nullptr;
        else if (id == "ButtonTrashEmpty") out.enabled = !m.trash().empty();
        else if (id == "ButtonExport" || id == "ButtonReport" || id == "ButtonSpam" || id == "ButtonNotSpam" ||
                 id == "OptionsButton")
            out.enabled = false;
    }

    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        if (id == "ButtonCompose" || id == "ButtonTrashCompose") {
            open_composition(host, eqstr(host, 4518));
        } else if (id == "ButtonReply" || id == "ButtonReplyAll" || id == "ButtonForward") {
            reply(host, inbox_sel_, id == "ButtonReply" ? 0 : id == "ButtonReplyAll" ? 1 : 2);
        } else if (id == "ButtonTrashReply" || id == "ButtonTrashReplyAll" || id == "ButtonTrashForward") {
            reply(host, trash_sel_, id == "ButtonTrashReply" ? 0 : id == "ButtonTrashReplyAll" ? 1 : 2);
        } else if (id == "ButtonDelete") {
            if (inbox_sel_) c.mail_delete({inbox_sel_});
            inbox_sel_ = 0;
        } else if (id == "ButtonTrashUndelete") {
            if (trash_sel_) c.mail_undelete({trash_sel_});
            trash_sel_ = 0;
        } else if (id == "ButtonTrashEmpty") {
            c.mail_empty_trash();
            trash_sel_ = 0;
        } else if (id == "AddressBookButton") {
            address_book_ui().open = !address_book_ui().open;
        } else {
            return id == "ButtonExport" || id == "ButtonReport" || id == "ButtonSpam" || id == "ButtonNotSpam" ||
                   id == "OptionsButton";
        }
        return true;
    }

private:
    // CMailWnd_BuildMessageSTML @0x6c36e0.
    std::string view(UiHost& host, const game::MailHeader& h) {
        auto label = [&](std::uint32_t s, const std::string& v) {
            // "<BR>\n" in the original; the line break alone here (lib/stml.lua breaks lines at '\n' too).
            return std::format("<C \"#8AA3FF\">{}</C> {}<BR>", stml_escape(eqstr(host, s)), stml_escape(v));
        };
        std::string out = label(4505, game::mail_short_name(h.from));
        if (!h.to.empty()) out += label(4506, h.to);
        out += label(4507, h.subject);
        out += label(4508, ctime_text(h.timestamp));
        std::uint32_t status = 4515;
        switch (h.status) {
        case game::MailStatus::New: status = 4510; break;
        case game::MailStatus::Unread: status = 4511; break;
        case game::MailStatus::Read: status = 4512; break;
        case game::MailStatus::Trash: status = 4513; break;
        case game::MailStatus::Deleted: status = 4514; break;
        }
        out += label(4509, eqstr(host, status));
        out += "<BR>";
        if (h.body) out += body_stml(*h.body);
        return out;
    }
    // CMailWnd_ComposeReply @0x6c5090: kind 0 reply, 1 reply to all, 2 forward.
    void reply(UiHost& host, std::uint32_t mid, int kind) {
        const auto& m = host.conn->mail();
        auto it = m.headers.find(mid);
        if (it == m.headers.end()) {
            host.conn->ui_message(eqstr(host, 4517));
            return;
        }
        const game::MailHeader& h = it->second;
        if (!open_composition(host, eqstr(host, kind == 2 ? 4519 : 4520))) return;
        auto& comp = composition();
        if (kind == 0) {
            comp.to = game::mail_short_name(h.from);
        } else if (kind == 1) {
            comp.to = game::mail_short_name(h.from);
            if (!h.to.empty()) comp.to += ", " + h.to;
        }
        comp.subject = eqstr(host, kind == 2 ? 4522 : 4521) + ": " + h.subject;
        comp.text = "\n\n" + eqstr(host, 4523) + "\n" + (h.body ? *h.body : std::string());
    }

    ComboData boxes_;
    ListData inbox_, trash_;
    std::vector<std::uint32_t> inbox_ids_, trash_ids_;
    std::uint32_t inbox_sel_ = 0, trash_sel_ = 0;
    std::set<std::uint32_t> asked_;
};

// ---------------------------------------------------------------- MailCompositionWindow (CMailCompositionWnd)

// CMailCompositionWnd (WndNotification @0x6c24f0): From is the current mailbox's name (0x6c1590 on AboutToShow
// and on MailMgr's mailbox change: MailMgr_GetMailboxName(MailMgr +0x1c), OP_MailLogin's address of that mailbox);
// Send, enabled by OnProcessFrame @0x6c1190 with To and Subject filled and GetGameFeature(6) not 0 (MailMgr_SendMail with To, Subject and
// the text; without the chat service eqstr 4529) and Cancel ("Message cancelled and not sent.", 4525); the address
// book button opens the address book (names it holds expand when sent, game::mail_expand_recipients). The sync
// button is not built here.
class MailCompositionWindow : public StockWindow {
public:
    MailCompositionWindow() : StockWindow("MailCompositionWindow", false) {}

    bool shown(UiHost&) override { return composition().open; }
    void set_shown(UiHost& host, bool show) override {
        auto& c = composition();
        if (show && !c.open) open_composition(host, eqstr(host, 4518));
        else if (!show) c.open = false;
    }
    std::string title(UiHost&) override { return composition().title; }
    void child(UiHost& host, std::string_view id, Child& out) override {
        const auto& c = composition();
        if (id == "FromAddressLabel" && host.conn) {
            // MailMgr_GetMailboxName gives "" for an index past the list; before the mail login nothing sets it.
            const auto& m = host.conn->mail();
            if (!m.mailboxes.empty())
                out.text = m.current >= 0 && m.current < static_cast<int>(m.mailboxes.size())
                               ? m.mailboxes[static_cast<std::size_t>(m.current)]
                               : std::string();
        } else if (id == "SendButton" && host.conn)
            out.enabled = !c.to.empty() && !c.subject.empty() && host.conn->game_feature(game::kFeatureSendMail) != 0;
        else if (id == "ToField") out.text = c.to;
        else if (id == "SubjectField") out.text = c.subject;
        else if (id == "TextField") out.text = c.text;
        else if (id == "SyncWithAddressBookButton") out.enabled = false;
    }
    bool set_text(UiHost&, std::string_view id, const std::string& text) override {
        auto& c = composition();
        if (id == "ToField") c.to = text;
        else if (id == "SubjectField") c.subject = text;
        else if (id == "TextField") c.text = text;
        else return false;
        return true;
    }
    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        auto& c = composition();
        if (!host.conn || button != "left") return false;
        if (id == "SendButton") {
            if (host.conn->mail_send(c.to, c.subject, c.text)) c.open = false;
        } else if (id == "CancelButton") {
            host.conn->ui_message(eqstr(host, 4525));
            c.open = false;
        } else if (id == "AddressBookButton") {
            address_book_ui().open = !address_book_ui().open;
        } else {
            return id == "SyncWithAddressBookButton";
        }
        return true;
    }
};

// ---------------------------------------------------------------- MailAddressBookWindow / MailIgnoreListWindow

// CMailAddressBookWnd (@0x6bf010, one class for both windows, InitGameUI @0x4973f0 makes them on MailMgr's two
// books; only the address book's has the Ignore button, IgnoreListButton, which brings the ignore list window up):
// the name list (the book's entries; the address book's then the built-in Guild and Group lists in grey), the
// Name, Addresses and Note fields, Private, Add (Modify when the name is in the book), Delete, New (clears the
// fields). A click on a row shows it in the fields; a right click adds its name to an open composition's To field.
class MailAddressBookWindow : public StockWindow {
public:
    explicit MailAddressBookWindow(Book book)
        : StockWindow(book == Book::Address ? "MailAddressBookWindow" : "MailIgnoreListWindow", false), book_(book) {}

    bool shown(UiHost&) override { return book_ui(book_).open; }
    void set_shown(UiHost&, bool show) override { book_ui(book_).open = show; }

    void ini_loaded(UiHost& host, ui::UiIni&, const std::filesystem::path& user_dir) override {
        if (!host.conn) return;
        auto& a = book_ui(book_);
        a = {};
        // MailMgr_LoadUserFiles @0x46dd20 reads "<current directory>\userdata\IgnoreList.txt" and
        // "...\userdata\AddressBook.txt" (or the character ini's MailOptions IgnoreList / AddressBook), one of each
        // for every character; here IgnoreList.txt and AddressBook.txt in the character's folder, in the same format.
        // Without one, the asset folder's shared file is where it starts from (read only, as its other ini files).
        // AddressBook_LoadFromFile @0x46dc80: a missing file is made (that book saved).
        const char* name = book_ == Book::Address ? "userdata/AddressBook.txt" : "userdata/IgnoreList.txt";
        a.file = book_ == Book::Address ? CharacterFiles{user_dir}.address_book() : CharacterFiles{user_dir}.ignore_list();
        std::error_code ec;
        std::filesystem::path from = a.file;
        if (!std::filesystem::exists(a.file, ec)) {
            auto shared = host.game ? host.game->find_base(name) : std::nullopt;
            if (!shared) {
                book_of(host, book_) = {};
                save_book(host, book_);
                return;
            }
            from = *shared;
        }
        std::ifstream in(from, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        book_of(host, book_) = game::MailAddressBook::parse(text.str());
        if (from != a.file) save_book(host, book_);
    }

    const ListData* list(UiHost& host, std::string_view id) override {
        if (!host.conn || id != "NameList") return nullptr;
        const auto& a = book_ui(book_);
        const auto& book = book_of(host, book_);
        names_ = {};
        names_.columns = 1;
        names_.labels = {"Name"};
        names_.widths = {200};
        rows_.clear();
        for (const auto& e : book.entries) {
            ListRow r;
            r.texts = {e.name};
            names_.rows.push_back(std::move(r));
            rows_.push_back(e);
        }
        // The transient Guild and group entries are the address book's (AddressBook_AddTransientEntry on MailMgr
        // +0x6c).
        if (book_ == Book::Address) {
            for (auto& e : host.conn->mail_builtin_lists()) {
                if (book.find(e.name)) continue; // a saved entry of that name wins
                ListRow r;
                r.texts = {e.name};
                r.color = argb_to_rgb(0xffa0a0a0);
                names_.rows.push_back(std::move(r));
                rows_.push_back(std::move(e));
            }
        }
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (!a.name.empty() && rows_[i].name == a.name) names_.selected = static_cast<int>(i);
        return &names_;
    }
    bool list_click(UiHost&, std::string_view id, int row, bool right, int) override {
        if (id != "NameList") return false;
        if (row < 0 || row >= static_cast<int>(rows_.size())) return true;
        const auto& e = rows_[static_cast<std::size_t>(row)];
        if (right) {
            auto& c = composition();
            if (c.open) {
                if (!c.to.empty()) c.to += ", ";
                c.to += e.name;
            }
            return true;
        }
        auto& a = book_ui(book_);
        a.name = e.name;
        a.note = e.note;
        a.is_private = e.is_private;
        a.addresses.clear();
        for (const auto& ad : e.addresses) a.addresses += (a.addresses.empty() ? "" : ", ") + ad;
        return true;
    }

    void child(UiHost& host, std::string_view id, Child& out) override {
        const auto& a = book_ui(book_);
        bool in_book = host.conn && book_of(host, book_).find(a.name);
        if (id == "NameField") out.text = a.name;
        else if (id == "FieldOfAddresses") out.text = a.addresses;
        else if (id == "NoteField") out.text = a.note;
        else if (id == "NameFieldIsPrivateCheckbox") out.checked = a.is_private;
        else if (id == "AddModifyButton") {
            out.text = in_book ? "Modify" : "Add";
            out.enabled = !a.name.empty() && !game::mail_split_addresses(a.addresses).empty();
        } else if (id == "DeleteButton") out.enabled = in_book;
        else if (id == "IgnoreListButton") out.shown = book_ == Book::Address;
    }
    bool set_text(UiHost&, std::string_view id, const std::string& text) override {
        auto& a = book_ui(book_);
        if (id == "NameField") a.name = text;
        else if (id == "FieldOfAddresses") a.addresses = text;
        else if (id == "NoteField") a.note = text;
        else return false;
        return true;
    }
    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& a = book_ui(book_);
        auto& book = book_of(host, book_);
        Book which = book_;
        if (id == "NameFieldIsPrivateCheckbox") {
            a.is_private = !a.is_private;
        } else if (id == "AddModifyButton") {
            if (book.put({a.name, game::mail_split_addresses(a.addresses), a.is_private, a.note}, host.conn->mail_server()))
                save_book(host, book_);
        } else if (id == "DeleteButton") {
            // CMailAddressBookWnd::WndNotification @0x6c0960: with the book's ConfirmDeletion flag (+0x34) a
            // ConfirmationDialog (eqstr 4546 "Do you want to delete this address book entry?", type 0xc) asks first;
            // without it the answer handler runs at once as for a yes. The address book's flag is the option
            // ConfirmDeletionFromAddressBook (MailMgr +0xa0); the ignore list's stays at AddressBook_Ctor's 1.
            auto remove = [&host, which] {
                auto& ab = book_ui(which);
                if (host.conn && book_of(host, which).remove(ab.name)) save_book(host, which);
                ab.name.clear(), ab.addresses.clear(), ab.note.clear(), ab.is_private = false;
            };
            if (book_ == Book::Ignore || host.conn->client_option(game::ClientOption::AddressBookConfirmDelete)) {
                UiHost::Confirm c;
                c.text = eqstr(host, 4546);
                c.done = [remove](bool yes) {
                    if (yes) remove();
                };
                host.confirm = std::move(c);
            } else {
                remove();
            }
        } else if (id == "ClearButton") {
            a.name.clear(), a.addresses.clear(), a.note.clear(), a.is_private = false;
        } else if (id == "IgnoreListButton" && book_ == Book::Address) {
            // Activate and BringToTop on the ignore list window (shown; a newly shown window is drawn on top).
            book_ui(Book::Ignore).open = true;
        } else {
            return false;
        }
        return true;
    }

private:
    Book book_;
    ListData names_;
    std::vector<game::MailAddressEntry> rows_;
};

} // namespace

void add_mail_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<MailWindow>());
    out.push_back(std::make_unique<MailCompositionWindow>());
    out.push_back(std::make_unique<MailAddressBookWindow>(Book::Address));
    out.push_back(std::make_unique<MailAddressBookWindow>(Book::Ignore));
}

} // namespace hailstone::app
