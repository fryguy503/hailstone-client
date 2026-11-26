// MacroQuest's colour codes in the plugins' and scripts' chat lines (game/mq_colors.h): the codes MQToSTML reads,
// kept only on lines that code wrote, and drawn by the chat windows as STML colours.

#include <doctest/doctest.h>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "game/mq_colors.h"
#include "mock_server/harness.h"
#include "plugin/policy.h"
#include "ui/dev_screens.h"

using namespace hailstone;

namespace {

using V = std::vector<std::string>;

// The pieces as text: "T:words", "O:rrggbb", "C"; with `finish`, the closes finish_mq_colors adds.
V pieces(std::string_view text, std::optional<std::uint32_t> line_rgb = std::nullopt, bool finish = true) {
    game::MqColorState state(line_rgb);
    V out;
    auto add = [&](const std::vector<game::MqColorPiece>& ps) {
        for (const auto& p : ps) {
            if (p.kind == game::MqColorPiece::Kind::Text) {
                out.push_back("T:" + std::string(p.text));
            } else if (p.kind == game::MqColorPiece::Kind::Open) {
                char hex[8];
                std::snprintf(hex, sizeof hex, "%06x", static_cast<unsigned>(p.rgb));
                out.push_back(std::string("O:") + hex);
            } else {
                out.push_back("C");
            }
        }
    };
    add(game::parse_mq_colors(text, state));
    if (finish) add(game::finish_mq_colors(state));
    return out;
}

// The tags in an STML string, in order ("c \"#00ff00\"", "/c", ...).
V tags(const std::string& stml) {
    V out;
    for (std::size_t at = stml.find('<'); at != std::string::npos; at = stml.find('<', at + 1)) {
        const auto end = stml.find('>', at);
        if (end == std::string::npos) break;
        out.push_back(stml.substr(at + 1, end - at - 1));
    }
    return out;
}

V chat_texts(net::GameConnection& c) {
    V out;
    while (auto e = c.poll_chat()) out.push_back(e->text);
    return out;
}

struct Context {
    ImGuiContext* value = ImGui::CreateContext();
    Context() { ImGui::GetIO().IniFilename = nullptr; }
    ~Context() { ImGui::DestroyContext(value); }
};

} // namespace

TEST_CASE("MQ colours: MQToSTML's letters, dark shades, hex colours and \\ax") {
    CHECK(pieces("plain") == V{"T:plain"});
    CHECK(pieces("\agGo\ax back") == V{"O:00ff00", "T:Go", "C", "T: back"});
    // Every letter, bright and dark (MQToSTML's table); black has no dark shade.
    const struct { char letter; const char* bright; const char* dark; } table[] = {
        {'y', "O:ffff00", "O:999900"}, {'o', "O:ff9900", "O:996600"}, {'g', "O:00ff00", "O:009900"},
        {'u', "O:0000ff", "O:000099"}, {'r', "O:ff0000", "O:990000"}, {'t', "O:00ffff", "O:009999"},
        {'b', "O:000000", "O:000000"}, {'m', "O:ff00ff", "O:990099"}, {'p', "O:9900ff", "O:660099"},
        {'w', "O:ffffff", "O:999999"},
    };
    for (const auto& t : table) {
        CAPTURE(t.letter);
        CHECK(pieces(std::string("\a") + t.letter + "x") == V{t.bright, "T:x", "C"});
        CHECK(pieces(std::string("\a-") + t.letter + "x") == V{t.dark, "T:x", "C"});
    }
    // E3's prefix: a hex colour, then a dark white; both still open at the end are closed.
    CHECK(pieces("\a#336699[E3]\a-w text") == V{"O:336699", "T:[E3]", "O:999999", "T: text", "C", "C"});
    CHECK(pieces("\a#ABCDEFx") == V{"O:abcdef", "T:x", "C"});
}

TEST_CASE("MQ colours: what changes nothing, and \\ax only closes a colour of the text's own") {
    // The colour already current opens nothing (MacroQuest's CurrentColor != LastColor), the line's own included.
    CHECK(pieces("\agA\agB") == V{"O:00ff00", "T:A", "T:B", "C"});
    CHECK(pieces("\agA", 0x00ff00u) == V{"T:A"});
    // After \ax the colour is not known, so the same letter opens again.
    CHECK(pieces("\agA\axB\agC") == V{"O:00ff00", "T:A", "C", "T:B", "O:00ff00", "T:C", "C"});
    // An unknown letter is taken and dropped; a \a, or a \a-, at the very end is dropped.
    CHECK(pieces("\azZ") == V{"T:Z"});
    CHECK(pieces("end\a") == V{"T:end"});
    CHECK(pieces("end\a-") == V{"T:end"});
    // \a# takes six characters, as MacroQuest; without six hex digits no colour opens.
    CHECK(pieces("\a#12zz56rest") == V{"T:rest"});
    CHECK(pieces("\a#12") == V{});
    // \ax with nothing open closes nothing (MacroQuest's </c> would close the line's own colour).
    CHECK(pieces("\axA") == V{"T:A"});
    CHECK(pieces("\agA\ax\axB") == V{"O:00ff00", "T:A", "C", "T:B"});
    // The state carries over: a colour opened in one part of a line is closed by \ax in the next.
    game::MqColorState state(0xffffffu);
    CHECK(game::parse_mq_colors("\arred ", state).size() == 2);
    CHECK(state.open == 1);
    CHECK(game::parse_mq_colors(" more\ax", state).size() == 2);
    CHECK(state.open == 0);
    CHECK(game::finish_mq_colors(state).empty());
}

TEST_CASE("MQ colours: strip_mq_colors leaves the text a reader sees") {
    CHECK(game::strip_mq_colors("no codes") == "no codes");
    CHECK(game::strip_mq_colors("\ar Warning: driver missing in:\ay/path \ar. Disabled!") ==
          " Warning: driver missing in:/path . Disabled!");
    CHECK(game::strip_mq_colors("\a#336699[E3]\a-w - \axhi") == "[E3] - hi");
}

TEST_CASE("MQ colours: a plugin's or script's line keeps its codes; anyone else's loses the BEL") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    chat_texts(rig.conn);
    rig.conn.ui_message("\agHi\ax there"); // the player's, a server's: no caller
    {
        plugin::CallerScope scope(plugin::kCapAll, "colour-test");
        rig.conn.ui_message("\agHi\ax there");
    }
    CHECK(chat_texts(rig.conn) == V{"gHix there", "\agHi\ax there"});
    // The client's own coded line (a MacroQuest command's reply, /popupecho's "Popup::") keeps its codes with no
    // caller too.
    rig.conn.client_message("\ayPopup\aw:: CANNOT CHARM");
    CHECK(chat_texts(rig.conn) == V{"\ayPopup\aw:: CANNOT CHARM"});
}

TEST_CASE("MQ colours: hs.ChatText draws a line's codes as STML colours, across its item links") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    Context context;
    ui::ChatView view;
    app::UiHost host;
    host.conn = &rig.conn;
    host.game = &rig.game;
    host.chat = &view;
    host.in_game = host.ui_ready = true;
    app::UiApi api{host};
    api.load_layout(rig.dir.path() / "defaults.ini", rig.dir.path() / "UI_fixture.ini", {});
    std::unique_ptr<lua_State, decltype(&lua_close)> lua{luaL_newstate(), &lua_close};
    auto* L = lua.get();
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "hs");

    view.add(ui::ChatChannel::System, "\a#336699[E3]\a-w <cast> \agdone");
    lua_getglobal(L, "hs");
    lua_getfield(L, -1, "ChatText");
    lua_remove(L, -2);
    lua_pushinteger(L, 1);
    lua_pushnil(L);
    REQUIRE_MESSAGE(lua_pcall(L, 2, 2, 0) == LUA_OK, lua_tostring(L, -1));
    REQUIRE(lua_istable(L, -1));
    REQUIRE(lua_rawlen(L, -1) == 1);
    lua_rawgeti(L, -1, 1);
    const std::string line = lua_tostring(L, -1);
    lua_settop(L, 0);

    // The line's colour, the three codes' colours, then each closed, the line's last.
    const V t = tags(line);
    REQUIRE(t.size() == 8);
    CHECK(t[0].starts_with("c "));
    CHECK(t[1] == "c \"#336699\"");
    CHECK(t[2] == "c \"#999999\"");
    CHECK(t[3] == "c \"#00ff00\"");
    CHECK(t[4] == "/c");
    CHECK(t[5] == "/c");
    CHECK(t[6] == "/c");
    CHECK(t[7] == "/c");
    CHECK(line.find("[E3]") != std::string::npos);
    CHECK(line.find("&lt;cast&gt;") != std::string::npos); // the text is still escaped
    CHECK(line.find('\a') == std::string::npos);
}
