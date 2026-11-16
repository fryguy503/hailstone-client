// The name change windows: NameChangeWnd, NameChangePetWnd and NameChangeMercWnd (CNameChangeWnd and its two copies)
// over GameConnection's name change state (net/game_connection_names.cpp).

#include <string>

#include "app/lua_ui_windows.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

// CNameChangeWnd @0x6fb1b0 (the pet and mercenary windows @0x6fa630 / @0x6f9ab0
// differ only in their child prefix, strings and ini key). Children: <prefix>NameEdit (15 characters), Prompt,
// SubmitButton, CloseButton, DisableReminderButton. Open(remind) @0x6fae40 (GameConnection::NameChangeWindow):
// Disable Reminder shown by `remind`; the close button reads "Remind Me Later" with the reminder's prompt, else
// "Close" with the manual one. 0x6fb020(1) after a change of the pending flag: Submit Name enabled while no
// request is out and reading "Submit Name" or "Request Pending". WndNotification @0x6fb470: Submit Name (or Enter in
// the box, message 6, while Submit Name is enabled) runs the window's command with the typed name; the close button
// and the close box (message 0xa) hide it; Disable Reminder asks first. AboutToHide lets it hide.
class NameChangeWindow : public StockWindow {
public:
    NameChangeWindow(std::string screen, std::string prefix, int kind, std::uint32_t remind_prompt,
                     std::uint32_t open_prompt)
        : StockWindow(std::move(screen), false), prefix_(std::move(prefix)), kind_(kind), remind_prompt_(remind_prompt),
          open_prompt_(open_prompt) {}

    bool shown(UiHost& host) override { return host.conn && host.conn->name_change(kind_).open; }
    void set_shown(UiHost& host, bool show) override {
        if (host.conn) host.conn->set_name_change_open(kind_, show);
    }
    void child(UiHost& host, std::string_view id, Child& out) override {
        if (!host.conn || id.substr(0, prefix_.size()) != prefix_) return;
        const auto& w = host.conn->name_change(kind_);
        std::string_view name = id.substr(prefix_.size());
        if (name == "NameEdit") {
            out.text = text_;
        } else if (name == "Prompt") {
            out.text = host.conn->strings().format(w.remind ? remind_prompt_ : open_prompt_, {});
        } else if (name == "CloseButton") {
            out.text = w.remind ? "Remind Me Later" : "Close";
        } else if (name == "DisableReminderButton") {
            out.shown = w.remind;
        } else if (name == "SubmitButton") {
            out.enabled = !w.pending;
            out.text = w.pending ? "Request Pending" : "Submit Name";
        }
    }
    bool set_text(UiHost&, std::string_view id, const std::string& text) override {
        if (id != prefix_ + "NameEdit") return false;
        text_ = text.substr(0, 15); // the edit box's limit (+0x1e4 / +0x1e8 = 0xf)
        return true;
    }
    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left" || id.substr(0, prefix_.size()) != prefix_) return false;
        std::string_view name = id.substr(prefix_.size());
        const auto& w = host.conn->name_change(kind_);
        if (name == "SubmitButton" || name == "NameEdit") {
            // message 6 from the box only while Submit Name is enabled
            if (name == "NameEdit" && w.pending) return true;
            host.conn->name_change_command(kind_, text_);
        } else if (name == "CloseButton") {
            host.conn->set_name_change_open(kind_, false);
        } else if (name == "DisableReminderButton") {
            host.conn->ask_disable_name_reminder(kind_);
        } else {
            return false;
        }
        return true;
    }

private:
    std::string prefix_;
    int kind_;
    std::uint32_t remind_prompt_, open_prompt_;
    std::string text_;
};

} // namespace

void add_name_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<NameChangeWindow>("NameChangeWnd", "NCW_", net::GameConnection::kNameCharacter, 5194, 5195));
    out.push_back(std::make_unique<NameChangeWindow>("NameChangePetWnd", "NCPW_", net::GameConnection::kNamePet, 4627, 4625));
    out.push_back(std::make_unique<NameChangeWindow>("NameChangeMercWnd", "NCMW_", net::GameConnection::kNameMerc, 4628, 4629));
}

} // namespace hailstone::app
