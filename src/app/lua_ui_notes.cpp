// PlayerNotesWindow (CPlayerNotesWnd): one multi-line edit box
// (PlayerNotesInput, at most 10000 characters) over notes.txt, which /note (Cmd_note @0x4f5e50) adds dated lines to.
// The original keeps notes.txt in the game folder; here it is in the data folder (docs/layout.md), read when first
// needed, written when the window hides and after each /note (CPlayerNotesWnd::SaveNotes).

#include <ctime>
#include <fstream>
#include <sstream>

#include "app/lua_ui_windows.h"
#include "core/log.h"
#include "game/option_commands.h"
#include "platform/paths.h"

namespace hailstone::app {

namespace {

constexpr std::size_t kNotesLimit = 10000; // the edit box's text limit (0x2710)

class PlayerNotesWindow : public StockWindow {
public:
    PlayerNotesWindow() : StockWindow("PlayerNotesWindow", false) {}

    void child(UiHost&, std::string_view id, Child& out) override {
        if (id == "PlayerNotesInput") out.text = text();
    }
    bool set_text(UiHost&, std::string_view id, const std::string& text) override {
        if (id != "PlayerNotesInput") return false;
        text_ = text.substr(0, kNotesLimit);
        return true;
    }
    void set_shown(UiHost&, bool show) override {
        if (open_ && !show) save(); // AboutToHide: SaveNotes
        open_ = show;
    }

    // Cmd_note_AddNote @0x717650: "[<ctime> ] text\n" after the notes (ctime's newline made a space; an item link's
    // 8-character head and its closing \x12 dropped); false when the notes would reach 10000 characters (13224).
    bool add(std::string_view note) {
        std::string stamp = game::ctime_text(std::time(nullptr)) + " ";
        std::string line = " " + std::string(note) + "\n";
        for (std::size_t at = line.find('\x12'); at != std::string::npos;) {
            if (at + 1 < line.size() && line[at + 1] == '0') {
                line.erase(at, 8);
                if (std::size_t end = line.find('\x12', at); end != std::string::npos) line.erase(end, 1);
                at = line.find('\x12');
            } else {
                at = line.find('\x12', at + 1); // the original finds the same one again forever
            }
        }
        std::string all = text() + "[" + stamp + "]" + line;
        if (all.size() >= kNotesLimit) return false;
        text_ = std::move(all);
        return save();
    }

private:
    const std::string& text() {
        if (!loaded_) {
            // The constructor: notes.txt read whole into the edit box (cut at the limit).
            loaded_ = true;
            std::ifstream in(platform::data_dir() / "notes.txt", std::ios::binary);
            std::ostringstream s;
            s << in.rdbuf();
            text_ = s.str().substr(0, kNotesLimit);
        }
        return text_;
    }
    bool save() {
        text();
        std::ofstream out(platform::data_dir() / "notes.txt", std::ios::binary | std::ios::trunc);
        if (!out) {
            log::warn("cannot write notes.txt");
            return false;
        }
        out << text_;
        return static_cast<bool>(out);
    }

    std::string text_;
    bool loaded_ = false;
};

} // namespace

std::unique_ptr<StockWindow> make_player_notes_window() { return std::make_unique<PlayerNotesWindow>(); }

bool player_notes_add(StockWindow& w, std::string_view note) {
    auto* notes = dynamic_cast<PlayerNotesWindow*>(&w);
    return notes && notes->add(note);
}

} // namespace hailstone::app
