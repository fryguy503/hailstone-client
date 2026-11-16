#include <doctest/doctest.h>

#include <algorithm>
#include <memory>

#include <imgui.h>
#include <lua.hpp>

#include "achievement_test_packets.h"
#include "app/lua_ui.h"
#include "game/item_links.h"
#include "mock_server/harness.h"
#include "ui/dev_screens.h"

using namespace hailstone;
namespace fixture = hailstone::test::achievement_fixture;

namespace {
struct Context {
    ImGuiContext* value = ImGui::CreateContext();
    Context() { ImGui::GetIO().IniFilename = nullptr; }
    ~Context() { ImGui::DestroyContext(value); }
};
struct Chat {
    Context context;
    ui::ChatView view;
    app::UiHost host;
    app::UiApi api{host};
    std::unique_ptr<lua_State, decltype(&lua_close)> lua{luaL_newstate(), &lua_close};
    explicit Chat(test::Rig& rig) {
        host.conn = &rig.conn; host.game = &rig.game; host.chat = &view;
        host.in_game = host.ui_ready = true;
        api.load_layout(rig.dir.path() / "defaults.ini", rig.dir.path() / "UI_fixture.ini", {});
        luaL_openlibs(lua.get()); lua_newtable(lua.get()); api.register_functions(lua.get());
        lua_setglobal(lua.get(), "hs");
    }
    struct Lines { lua_Integer version = 0; std::vector<std::string> text; bool returned = false; };
    Lines lines(std::optional<lua_Integer> held = std::nullopt) {
        auto* L = lua.get(); lua_settop(L, 0);
        lua_getglobal(L, "hs"); lua_getfield(L, -1, "ChatText"); lua_remove(L, -2);
        lua_pushinteger(L, 1);
        if (held) lua_pushinteger(L, *held); else lua_pushnil(L);
        const int status = lua_pcall(L, 2, 2, 0);
        REQUIRE_MESSAGE(status == LUA_OK, lua_tostring(L, -1));
        Lines out; out.version = lua_tointeger(L, -2); out.returned = lua_istable(L, -1);
        if (out.returned) for (lua_Integer i = 1; i <= static_cast<lua_Integer>(lua_rawlen(L, -1)); ++i) {
            lua_rawgeti(L, -1, i); std::size_t n = 0; const char* text = lua_tolstring(L, -1, &n);
            REQUIRE(text); out.text.emplace_back(text, n); lua_pop(L, 1);
        }
        lua_settop(L, 0); return out;
    }
    void click(std::string_view data, int message = 1002) {
        auto* L = lua.get(); lua_settop(L, 0);
        lua_getglobal(L, "hs"); lua_getfield(L, -1, "ChatLink"); lua_remove(L, -2);
        lua_pushinteger(L, 1); lua_pushinteger(L, message); lua_pushlstring(L, data.data(), data.size());
        const int status = lua_pcall(L, 3, 0, 0);
        REQUIRE_MESSAGE(status == LUA_OK, lua_tostring(L, -1));
    }
};
std::string link_data(const std::string& stml) {
    constexpr std::string_view prefix = "WndNotify=\"1002,";
    const auto at = stml.find(prefix);
    if (at == std::string::npos) return {};
    const auto end = stml.find('"', at + prefix.size());
    return stml.substr(at + prefix.size(), end - at - prefix.size());
}
std::size_t requests(const test::Rig& rig) {
    return static_cast<std::size_t>(std::count_if(rig.server.zone_log.begin(), rig.server.zone_log.end(),
        [](const net::AppPacket& packet) { return packet.opcode == net::kOpAchievementLinkRequest; }));
}
} // namespace

TEST_CASE("achievement chat: envelopes expose only the visible suffix and preserve ordinary item links") {
    const auto tag = fixture::tag("Peer", "Visible <name> & text");
    const auto parsed = game::parse_achievement_chat_link(tag);
    REQUIRE(parsed);
    CHECK(parsed->metadata == fixture::metadata());
    CHECK(parsed->text == "Visible <name> & text");
    const auto line = "Earned " + std::string(1, '\x12') + tag + '\x12' + " today";
    CHECK(game::strip_links(line) == "Earned Visible <name> & text today");
    const auto segments = game::split_links(line);
    REQUIRE(segments.size() == 3);
    REQUIRE(segments[1].achievement);
    CHECK(segments[1].achievement->metadata == fixture::metadata());
    CHECK_FALSE(segments[1].link);
    CHECK_FALSE(game::parse_achievement_chat_link("3Peer^900^1^0^0^4^4^"));
    CHECK_FALSE(game::parse_achievement_chat_link("3Peer^900'Name"));
    CHECK_FALSE(game::parse_achievement_chat_link("3Peer^900^'\nName"));
    CHECK_FALSE(game::parse_achievement_chat_link("3" + std::string(net::kMaxAchievementLinkBytes + 1, 'x') + "^'Name"));
    CHECK(game::strip_links(std::string("\x12") + "3malformed^metadata\x12") == "Achievement");
    CHECK(game::strip_links(game::make_item_link(1234, "Ordinary item", false)) == "Ordinary item");
    CHECK(game::strip_links(std::string("\x12") + "1Peer\x12") == "Peer");

    const auto full_tag = std::string(1, '\x12') + fixture::tag() + '\x12';
    std::string ten;
    for (int i = 0; i < 10; ++i) ten += full_tag;
    const auto limited = game::link_all_into_chat(ten, {game::make_item_link(1234, "Item", false)}, 4096);
    CHECK(limited.sent.empty());
    CHECK(limited.text == ten); // achievement tags use the same ten-tag edit limit
    const auto character_limited = game::link_all_into_chat("prefix", {full_tag}, full_tag.size() + 5);
    CHECK(character_limited.text == "prefix");
    CHECK(character_limited.sent.empty()); // a tag that never fit does not send an empty retry loop
}

TEST_CASE("achievement chat: current catalog supplies escaped labels and exact manual link inspection") {
    test::Rig rig;
    REQUIRE(rig.to_zone());
    Chat chat(rig);
    chat.view.add(ui::ChatChannel::System, std::string("Earned \x12") +
        fixture::tag("Peer", "Spoofed <label>") + '\x12');
    const auto before = chat.lines();
    REQUIRE(before.text.size() == 1);
    CHECK(before.text[0].find("Spoofed &lt;label&gt;") != std::string::npos);
    CHECK(link_data(before.text[0]).empty());
    CHECK(requests(rig) == 0);
    rig.conn.receive_zone_packet_for_test(fixture::catalog());
    REQUIRE(rig.conn.achievements().catalog());
    const auto received = chat.lines(before.version);
    REQUIRE(received.returned);
    REQUIRE(received.text.size() == 1);
    CHECK(received.version != before.version); // catalog arrival invalidates already displayed plain links
    CHECK(received.text[0].find("Second &amp; &lt;Achievement&gt;") != std::string::npos);
    CHECK(received.text[0].find("Spoofed") == std::string::npos);
    CHECK(received.text[0].find("Peer^") == std::string::npos);
    const auto data = link_data(received.text[0]);
    REQUIRE_FALSE(data.empty());
    CHECK(data.starts_with("506565725e3930305e")); // exact Peer^900^ bytes, not entity substitution
    CHECK(std::all_of(data.begin(), data.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    }));
    CHECK_FALSE(chat.lines(received.version).returned);
    CHECK(requests(rig) == 0); // rendering and cached reads never send an inspection
    chat.click("not hex"); chat.click(data + "0"); chat.click("00");
    CHECK_FALSE(rig.conn.achievements().link_pending());
    chat.click(data);
    CHECK(rig.conn.achievements().detail_shown());
    CHECK_FALSE(rig.conn.achievements().link_pending());
    CHECK(requests(rig) == 0); // absent players still have a readable received snapshot
    rig.server.add_spawn(test::make_spawn("Peer", 77, 0, 1, 0, 0, 0));
    const bool peer_arrived = rig.until([&] { return rig.conn.world().spawns.contains(77); });
    REQUIRE(peer_arrived);
    chat.click(data);
    REQUIRE(rig.until([&] { return requests(rig) == 1; }));
    const auto request = std::find_if(rig.server.zone_log.begin(), rig.server.zone_log.end(),
        [](const net::AppPacket& packet) { return packet.opcode == net::kOpAchievementLinkRequest; });
    REQUIRE(request != rig.server.zone_log.end());
    ByteReader reader(request->data);
    CHECK(reader.cstring() == "Peer"); CHECK(reader.le<std::uint32_t>() == 900);
    CHECK(reader.le<std::int16_t>() == 1);
    CHECK(reader.le<std::uint16_t>() == 0); CHECK(reader.le<std::uint16_t>() == 0);
    CHECK(reader.le<std::uint32_t>() == 4); CHECK(reader.le<std::uint32_t>() == 4);
    CHECK(reader.le<std::uint8_t>() == 0); // stock inspection trailer
    CHECK(reader.empty());
    rig.conn.disconnect();
    chat.click(data);
    CHECK(requests(rig) == 1);
    CHECK_FALSE(rig.conn.achievements().link_pending());
    const auto retired = chat.lines(received.version);
    REQUIRE(retired.returned);
    REQUIRE(retired.text.size() == 1);
    CHECK(link_data(retired.text[0]).empty());
}
