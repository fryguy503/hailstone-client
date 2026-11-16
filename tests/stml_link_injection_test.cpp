// Item links and server text inside STML: a link's visible text is whatever the sender typed, so a crafted link
// must not close the <a> tag and add markup (colours, fake text) to everyone's chat; the link still clicks.

#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <string>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "game/eq_strings.h"
#include "game/item_links.h"
#include "game/task_window.h"
#include "game/tasks.h"
#include "mock_server/harness.h"
#include "ui/dev_screens.h"

using namespace hailstone;

namespace {

// A sender's link: the wide body (76 hex digits after '0'), then text that tries to end the <a> tag.
// (item id 1, six augments, the evolving fields and ornament 0, hash 0xABCD)
const std::string kBody = "0" + std::string("00000001") + std::string(6 * 8 + 12, '0') + "0000ABCD";
const std::string kEvil = "X\"><c '#ff0000'>[GM] fake</c><a WndNotify=\"1001,Bob\">&amp;";
const std::string kTag = kBody + kEvil;

// The tags in an STML string, in order ("a ...", "/a", "c ...").
std::vector<std::string> tags(const std::string& stml) {
    std::vector<std::string> out;
    for (std::size_t at = stml.find('<'); at != std::string::npos; at = stml.find('<', at + 1)) {
        const auto end = stml.find('>', at);
        if (end == std::string::npos) break;
        out.push_back(stml.substr(at + 1, end - at - 1));
    }
    return out;
}

std::string notify_data(const std::string& stml, std::string_view prefix = "WndNotify=\"27,") {
    const auto at = stml.find(prefix);
    if (at == std::string::npos) return {};
    const auto end = stml.find('"', at + prefix.size());
    return stml.substr(at + prefix.size(), end - at - prefix.size());
}

struct Context {
    ImGuiContext* value = ImGui::CreateContext();
    Context() { ImGui::GetIO().IniFilename = nullptr; }
    ~Context() { ImGui::DestroyContext(value); }
};

} // namespace

TEST_CASE("stml links: link data round-trips as hex and escape_stml escapes markup") {
    REQUIRE(game::parse_item_link(kTag));
    const std::string data = game::stml_link_data(kTag);
    CHECK(data.front() == 'x');
    CHECK(data.find_first_not_of("x0123456789abcdef") == std::string::npos);
    CHECK(game::stml_link_tag(data) == kTag);
    CHECK(game::stml_link_tag(kBody) == kBody); // raw tags (a script's) pass through
    CHECK(game::stml_link_tag("x0") == "x0");   // odd hex: not ours
    CHECK(game::stml_link_tag("xzz") == "xzz");
    CHECK(game::stml_link_tag("") == "");
    CHECK(game::escape_stml("a<b>&c") == "a&lt;b&gt;&amp;c");
}

TEST_CASE("stml links: a crafted chat item link injects no markup and still asks for the item") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    Context context;
    ui::ChatView view;
    app::UiHost host;
    host.conn = &rig.conn;
    host.game = &rig.game;
    host.chat = &view;
    host.in_game = host.ui_ready = true;
    std::optional<game::ItemLink> clicked;
    host.click_link = [&](const game::ItemLink& l) { clicked = l; };
    app::UiApi api{host};
    api.load_layout(rig.dir.path() / "defaults.ini", rig.dir.path() / "UI_fixture.ini", {});
    std::unique_ptr<lua_State, decltype(&lua_close)> lua{luaL_newstate(), &lua_close};
    auto* L = lua.get();
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "hs");

    view.add(ui::ChatChannel::Say, "Look: \x12" + kTag + "\x12 ok");
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

    // Only the line's colour, the link and their ends: nothing from the sender's text.
    const auto t = tags(line);
    REQUIRE(t.size() == 4);
    CHECK(t[0].starts_with("c "));
    CHECK(t[1].starts_with("a WndNotify=\"27,x"));
    CHECK(t[2] == "/a");
    CHECK(t[3] == "/c");
    CHECK(line.find("[GM] fake") != std::string::npos); // shown as text
    CHECK(line.find("&amp;amp;") != std::string::npos);

    const std::string data = notify_data(line);
    REQUIRE_FALSE(data.empty());
    lua_getglobal(L, "hs");
    lua_getfield(L, -1, "ChatLink");
    lua_remove(L, -2);
    lua_pushinteger(L, 1);
    lua_pushinteger(L, 27);
    lua_pushlstring(L, data.data(), data.size());
    REQUIRE_MESSAGE(lua_pcall(L, 3, 0, 0) == LUA_OK, lua_tostring(L, -1));
    REQUIRE(clicked);
    CHECK(clicked->item_id == 1);
    CHECK(clicked->hash == 0xABCD);
    CHECK(clicked->text == kEvil);
}

TEST_CASE("stml links: a task reward link's text is escaped and its data encoded") {
    game::EqStrings s;
    s.set(3477, "Reward(s):<BR>");
    game::Task t;
    t.type = game::kTaskTypeQuest;
    t.has_rewards = true;
    t.reward_link = "\x12" + kTag + "\x12";
    const std::string out = game::task_details_stml(t, s, [](const game::TaskElement&) { return std::string(); });
    std::vector<std::string> link_tags;
    for (const auto& tag : tags(out))
        if (!(tag == "BR" || tag == "br")) link_tags.push_back(tag);
    REQUIRE(link_tags.size() == 2);
    CHECK(link_tags[0].starts_with("a WndNotify=\"27,x"));
    CHECK(link_tags[1] == "/a");
    CHECK(game::stml_link_tag(notify_data(out)) == kTag);
}
