// The social editor: SocialEditWnd (CSocialEditWnd @0x7588a0) over the character's socials (game/hotbuttons.h).
// 0x758bb0 opens it on social n (page n / 12, button n % 12, colour 0), 0x758730 fills the name, the five
// lines and the colour from the social, CSocialEditWnd::WndNotification @0x758c10 takes the clicks: Accept
// (sub_00758470) copies the boxes into the social (name 15 characters, lines 255, the colour), clears its cooldown,
// saves the socials (SaveSocialsToIni) and closes; Clear (sub_007585e0) empties the name and the lines; a colour
// button SEW_TC0..19Button gives the name box its text colour and becomes the social's colour.
// CSocialEditWnd::GetSocialTextColor @0x758700: a colour button's text colour, 0xff606060 out of range.
//
// For the UI packs' editors (Hailstone's, not the original's):
//   hs.SocialEditorSocial()   the social 0..119 the editor is open on, or nil
//   hs.SocialLine(text)       a line as a press runs it (game::split_social_line): {pause = steps or nil,
//                             timers = {tenths, ...}, command, kind = "empty" | "command" | "text" | "chatserver" |
//                             "invalid", name = the command's ("/assist") for kind "command"}
//   hs.SlashCommands(prefix)  the game's commands starting with prefix ("/as": {"/assist", ...}), in the order the
//                             game tries them (its first is what the prefix runs); GM commands only for a GM. The
//                             client's own and plugins' commands are not listed.
//   hs.PressSocial(n)         a press of social n, as a hot button's (ActionsUi::press_social)
//   hs.CurrentSocial()        the social a press queued and is running, or nil; hs.StopSocial(): /stopsocial

#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lua.hpp>

#include "app/actions_ui.h"
#include "app/lua_ui_windows.h"
#include "game/hot_button_press.h"
#include "game/hotbuttons.h"
#include "game/slash_commands.h"
#include "net/game_connection.h"
#include "plugin/lua_keep.h"

namespace hailstone::app {

namespace {

// The colour buttons' text colours (EQUI_SocialEditWnd.xml SEW_TC0Button .. SEW_TC19Button).
constexpr std::array<std::uint32_t, 20> kSocialColors = {
    0xf0f0f0, 0xf0b400, 0x008000, 0xb46400, 0x000080, 0xf000f0, 0x808080, 0xe0e0e0, 0x828200, 0x606060,
    0x000000, 0x8c0000, 0xa0a0a0, 0xf00000, 0x00f000, 0xf0f000, 0x0000f0, 0x0064f0, 0x00f0f0, 0x800080,
};

// n of "SEW_TC<n>Button" / "SEW_Line<n>Input", or -1.
int number_between(std::string_view id, std::string_view prefix, std::string_view suffix) {
    if (id.size() <= prefix.size() + suffix.size() || !id.starts_with(prefix) || !id.ends_with(suffix)) return -1;
    auto mid = id.substr(prefix.size(), id.size() - prefix.size() - suffix.size());
    int v = -1;
    auto [p, ec] = std::from_chars(mid.data(), mid.data() + mid.size(), v);
    return ec == std::errc() && p == mid.data() + mid.size() ? v : -1;
}

class SocialEditWindow : public StockWindow {
public:
    SocialEditWindow() : StockWindow("SocialEditWnd", false) {}

    bool shown(UiHost& h) override { return open_ && h.in_game; }
    void set_shown(UiHost& h, bool show) override {
        open_ = show;
        if (!show) h.social_editing = -1;
    }

    // 0x758bb0: the social to edit, then 0x758730 and Show(1, 1, 1).
    bool activate(UiHost& h, int social) override {
        if (!h.hot_buttons || social < 0 || social >= game::kSocialPages * game::kSocialButtons) return false;
        page_ = social / game::kSocialButtons;
        button_ = social % game::kSocialButtons;
        color_ = 0;
        load(*h.hot_buttons);
        open_ = true;
        h.social_editing = social;
        return true;
    }

    void child(UiHost&, std::string_view id, Child& out) override {
        if (id == "SEW_NameInput") {
            out.text = name_;
            // The name box's CRNormal: the chosen colour button's (0x758730 / WndNotification); unset until a
            // social's colour 0..19 or a button set it.
            if (name_color_) out.color = argb_to_rgb(*name_color_);
        } else if (int n = number_between(id, "SEW_Line", "Input"); n >= 0 && n < game::kSocialLines) {
            out.text = lines_[static_cast<std::size_t>(n)];
        }
    }

    bool set_text(UiHost&, std::string_view id, const std::string& text) override {
        if (id == "SEW_NameInput") {
            // The name box's limit: 15 characters (5 when the client mode global 0xddfd54 is 1..3 or 6, which this
            // client has no counterpart of).
            name_ = text.substr(0, 15);
        } else if (int n = number_between(id, "SEW_Line", "Input"); n >= 0 && n < game::kSocialLines) {
            lines_[static_cast<std::size_t>(n)] = text.substr(0, 255);
        } else {
            return false;
        }
        return true;
    }

    bool click(UiHost& h, std::string_view id, std::string_view button, bool) override {
        if (button != "left") return false;
        if (id == "SEW_Accept_Button") {
            accept(h);
        } else if (id == "SEW_Clear_Button") {
            name_.clear();
            for (auto& l : lines_) l.clear();
        } else if (int n = number_between(id, "SEW_TC", "Button"); n >= 0 && n < static_cast<int>(kSocialColors.size())) {
            name_color_ = kSocialColors[static_cast<std::size_t>(n)];
            color_ = n;
        } else {
            return false;
        }
        return true;
    }

private:
    int social() const { return page_ * game::kSocialButtons + button_; }

    // 0x758730.
    void load(const game::HotButtonStore& store) {
        const game::Social& s = store.social(social());
        name_ = s.name;
        lines_ = s.lines;
        color_ = s.color;
        name_color_.reset();
        if (color_ >= 0 && color_ < static_cast<int>(kSocialColors.size()))
            name_color_ = kSocialColors[static_cast<std::size_t>(color_)];
    }

    // sub_00758470.
    void accept(UiHost& h) {
        if (h.hot_buttons) {
            game::Social& s = h.hot_buttons->social(social());
            s.name = name_.substr(0, 15);
            for (std::size_t i = 0; i < lines_.size(); ++i) s.lines[i] = lines_[i].substr(0, 255);
            s.color = color_;
            s.cooldown_start = s.cooldown_length = 0;
            if (h.actions) h.actions->socials_changed(); // SaveSocialsToIni(1)
        }
        open_ = false; // Deactivate
        h.social_editing = -1;
    }

    int page_ = 0, button_ = 0, color_ = 0;
    std::string name_;
    std::array<std::string, game::kSocialLines> lines_;
    std::optional<std::uint32_t> name_color_;
};

UiHost* host_of(lua_State* L) { return static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1))); }

// hs.OpenSocialEditor(index): the social editor on social 0..119.
int OpenSocialEditor(lua_State* L) {
    UiHost& h = *host_of(L);
    auto social = held_int(luaL_checkinteger(L, 1));
    if (h.open_social_editor) h.open_social_editor(social);
    return 0;
}

// hs.SocialEditorSocial(): the social the editor is open on, or nil.
int SocialEditorSocial(lua_State* L) {
    UiHost& h = *host_of(L);
    if (h.social_editing < 0 || !h.in_game) return 0;
    lua_pushinteger(L, h.social_editing);
    return 1;
}

// hs.SocialLine(text): the line's waits and cooldowns, and what the interpreter makes of the rest (InterpretCmd's
// split, game::parse_command_line; a plugin's or script's command reads as "invalid", as they are not the game's).
int SocialLine(lua_State* L) {
    UiHost& h = *host_of(L);
    const char* text = luaL_checkstring(L, 1);
    // The parts in a box the state owns across the pushes (plugin/lua_keep.h).
    struct Line {
        game::SocialLineParts parts;
        game::CommandLine cl;
    };
    Line& line = plugin::lua_keep<Line>(L);
    plugin::cxx_guard(L, [&] {
        line.parts = game::split_social_line(text);
        const bool gm = h.conn && h.conn->profile() && h.conn->profile()->gm;
        line.cl = game::parse_command_line(line.parts.command, gm, h.conn ? &h.conn->strings() : nullptr);
    });
    const game::SocialLineParts& parts = line.parts;
    const game::CommandLine& cl = line.cl;
    lua_createtable(L, 0, 5);
    if (parts.pause) {
        lua_pushinteger(L, *parts.pause);
        lua_setfield(L, -2, "pause");
    }
    lua_createtable(L, static_cast<int>(parts.timers.size()), 0);
    for (std::size_t i = 0; i < parts.timers.size(); ++i) {
        lua_pushinteger(L, parts.timers[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "timers");
    lua_pushlstring(L, parts.command.data(), parts.command.size());
    lua_setfield(L, -2, "command");
    const char* kind = "empty";
    switch (cl.kind) {
    case game::CommandLine::Kind::Empty: kind = "empty"; break;
    case game::CommandLine::Kind::ChatServer: kind = "chatserver"; break;
    case game::CommandLine::Kind::Command: kind = "command"; break;
    case game::CommandLine::Kind::Invalid: kind = "invalid"; break;
    case game::CommandLine::Kind::Text: kind = "text"; break;
    }
    lua_pushstring(L, kind);
    lua_setfield(L, -2, "kind");
    if (cl.kind == game::CommandLine::Kind::Command) {
        lua_pushstring(L, game::slash_commands()[cl.index].name);
        lua_setfield(L, -2, "name");
    }
    return 1;
}

// hs.SlashCommands(prefix): the commands whose built-in name starts with prefix (case ignored), in table order.
int SlashCommands(lua_State* L) {
    UiHost& h = *host_of(L);
    // (Compared in place: no std::string alive across the pushes, which may raise.)
    std::size_t len = 0;
    const char* p = luaL_checklstring(L, 1, &len);
    const std::string_view prefix(p, len);
    const bool gm = h.conn && h.conn->profile() && h.conn->profile()->gm;
    lua_newtable(L);
    lua_Integer n = 0;
    for (const game::SlashCommand& c : game::slash_commands()) {
        if (c.restriction > 0 && !gm) continue;
        std::string_view name = c.name;
        if (name.size() < prefix.size()) continue;
        bool match = true;
        for (std::size_t i = 0; i < prefix.size() && match; ++i)
            match = std::tolower(static_cast<unsigned char>(name[i])) ==
                    std::tolower(static_cast<unsigned char>(prefix[i]));
        if (!match) continue;
        lua_pushlstring(L, name.data(), name.size());
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int PressSocial(lua_State* L) {
    UiHost& h = *host_of(L);
    auto social = held_int(luaL_checkinteger(L, 1));
    if (!h.actions || !h.in_game || social < 0 || social >= game::kSocialPages * game::kSocialButtons) return 0;
    h.actions->press_social(social);
    lua_pushboolean(L, 1);
    return 1;
}

int CurrentSocial(lua_State* L) {
    UiHost& h = *host_of(L);
    if (!h.actions || h.actions->current_social() < 0) return 0;
    lua_pushinteger(L, h.actions->current_social());
    return 1;
}

int StopSocial(lua_State* L) {
    UiHost& h = *host_of(L);
    if (h.actions) h.actions->stop_social();
    return 0;
}

} // namespace

Rgb social_text_color(int color) {
    if (color >= 0 && color < static_cast<int>(kSocialColors.size()))
        return argb_to_rgb(kSocialColors[static_cast<std::size_t>(color)]);
    return argb_to_rgb(0xff606060);
}

void add_socialedit_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<SocialEditWindow>());
}

void register_socialedit_api(lua_State* L, UiHost& h) {
    static const luaL_Reg fns[] = {{"OpenSocialEditor", OpenSocialEditor},
                                   {"SocialEditorSocial", SocialEditorSocial},
                                   {"SocialLine", SocialLine},
                                   {"SlashCommands", SlashCommands},
                                   {"PressSocial", PressSocial},
                                   {"CurrentSocial", CurrentSocial},
                                   {"StopSocial", StopSocial},
                                   {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h);
    luaL_setfuncs(L, fns, 1);
}

} // namespace hailstone::app
