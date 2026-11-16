// The item display's text (game/item_display.h): CItemDisplayWnd_BuildItemInfo's lines and grid.
#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "core/game_dir.h"
#include "game/eq_strings.h"
#include "game/item_display.h"
#include "game/server_ext.h"
#include "game/spells.h"
#include "net/game_connection.h"
#include "scratch_dir.h"
#include "ui/item_windows.h"

using namespace hailstone;

namespace {
game::EqStrings strings() {
    game::EqStrings s;
    for (auto [id, t] : std::initializer_list<std::pair<std::uint32_t, const char*>>{
             {0xbf8, "Class: "}, {0xbe9, "Race: "}, {0x3395, "ALL"}, {0x4c0, "None"}, {3065, "WAR"}, {3067, "PAL"},
             {0xbea, "HUM"}, {0xbeb, "BAR"}, {569, "Primary"}, {570, "Secondary"}, {0x32cb, "Size:"}, {3130, "MEDIUM"},
             {12999, "Weight:"}, {0x32cf, "Base Dmg:"}, {0x32d1, "Delay:"}, {0x32dc, "Strength:"}, {0x32ce, "AC:"},
             {0xc16, "Magic"}, {0xc1b, "No Trade"}, {0x32f5, "Skill:"}, {0x3411, "Unmodified"}})
        s.set(id, t);
    return s;
}

struct PresentationExtension final : game::ServerExtension {
    bool enabled = false;
    std::uint64_t revision = 1;
    std::string flags;
    std::string description;
    std::function<std::string(const net::Item&)> configuration_flags, configuration_description;

    std::optional<std::string> item_flags(const net::GameConnection&, const net::Item& item,
                                          std::string_view) const override {
        return enabled && item.id == 4242
            ? std::optional(configuration_flags ? configuration_flags(item) : flags) : std::nullopt;
    }
    std::optional<std::string> item_description_append(const net::GameConnection&,
                                                       const net::Item& item) const override {
        return enabled && item.id == 4242
            ? std::optional(configuration_description ? configuration_description(item) : description) : std::nullopt;
    }
    std::uint64_t item_presentation_revision(const net::GameConnection&) const override { return revision; }
};

struct PresentationGui {
    ImGuiContext* context = ImGui::CreateContext();
    ~PresentationGui() { ImGui::DestroyContext(context); }
};

struct PresentationLua {
    lua_State* state = luaL_newstate();
    explicit PresentationLua(app::UiApi& api) {
        luaL_openlibs(state);
        lua_newtable(state);
        api.register_functions(state);
        lua_setglobal(state, "hs");
    }
    ~PresentationLua() { lua_close(state); }
    std::string text(const char* code) {
        INFO(code);
        REQUIRE(luaL_dostring(state, code) == LUA_OK);
        REQUIRE(lua_isstring(state, -1));
        const std::string value = lua_tostring(state, -1);
        lua_settop(state, 0);
        return value;
    }
    lua_Integer integer(const char* code) {
        INFO(code);
        REQUIRE(luaL_dostring(state, code) == LUA_OK);
        REQUIRE(lua_isinteger(state, -1));
        const auto value = lua_tointeger(state, -1);
        lua_settop(state, 0);
        return value;
    }
};
} // namespace

TEST_CASE("Item presentation: extension flags change only the dedicated status line") {
    auto s = strings();
    s.set(0x1717, "Attunable");
    s.set(0xbdc, "Effect: %1 %2");
    net::Item item;
    item.id = 4242;
    item.name = "Attunable Sword";
    item.magic = 1;
    item.no_drop = 0;
    item.no_rent = 1;
    item.attuneable = 1;
    item.effects[net::kEffectClick].effect = 200;
    item.effects[net::kEffectClick].name = "Attunable Spell";
    game::SpellData effect;
    effect.id = 200;
    effect.name = "Attunable Spell";
    game::ItemDisplayContext context;
    context.strings = &s;
    context.spell = [&](std::int32_t id) { return id == 200 ? &effect : nullptr; };
    const auto stock = game::build_item_display(item, context);
    const auto native_stock = ui::item_info_lines(item);
    REQUIRE(stock.info.size() >= 2);
    CHECK(stock.info[1].text == "Magic, No Trade, Attunable");
    CHECK(stock.description.find("Attunable Spell") != std::string::npos);

    context.item_flags = [&](const net::Item& displayed, std::string_view original) -> std::optional<std::string> {
        CHECK(displayed.id == 4242);
        CHECK(original == stock.info[1].text);
        return "Magic, Kinbound, No Trade";
    };
    const auto changed = game::build_item_display(item, context);
    CHECK(changed.title == stock.title);
    CHECK(changed.title == "Attunable Sword");
    CHECK(changed.info.size() == stock.info.size());
    CHECK(changed.info[1].text == "Magic, Kinbound, No Trade");
    CHECK(changed.measured[1] == changed.info[1].text);
    CHECK(changed.description == stock.description);
    for (std::size_t index = 0; index < stock.info.size(); ++index)
        if (index != 1) CHECK(changed.info[index].text == stock.info[index].text);
    for (std::size_t index = 0; index < stock.measured.size(); ++index)
        if (index != 1) CHECK(changed.measured[index] == stock.measured[index]);
    const auto native_changed = ui::item_info_lines(item, context.item_flags);
    REQUIRE(native_changed.size() == native_stock.size());
    CHECK(native_changed.front() == "Magic, Kinbound, No Trade");
    for (std::size_t index = 1; index < native_stock.size(); ++index) CHECK(native_changed[index] == native_stock[index]);

    context.item_flags = [](const net::Item&, std::string_view) -> std::optional<std::string> { return std::nullopt; };
    CHECK(game::build_item_display(item, context).info[1].text == stock.info[1].text);
    CHECK(ui::item_info_lines(item, context.item_flags) == native_stock);
    context.item_flags = [](const net::Item&, std::string_view) { return std::optional(std::string()); };
    CHECK(game::build_item_display(item, context).info[1].text.empty());
}

TEST_CASE("Item presentation: appended plaintext is escaped once in STML and remains plain in the fallback") {
    net::Item item;
    item.id = 4242;
    item.name = "Ordinary item";
    item.no_drop = 1;
    item.no_rent = 1;
    game::ItemDisplayContext context;
    const auto stock = game::build_item_display(item, context);
    const auto native_stock = ui::item_info_lines(item);
    const std::string extra = "First <a WndNotify=\"27,test\">&lt;</a>\r\nUse \"x\" & 'y'\nLast\rEnd";
    context.item_description_append = [&](const net::Item& displayed) -> std::optional<std::string> {
        CHECK(displayed.id == 4242);
        return extra;
    };
    const auto changed = game::build_item_display(item, context);
    const std::string escaped = "First &lt;a WndNotify=&quot;27,test&quot;&gt;&amp;lt;&lt;/a&gt;<BR>"
                                "Use &quot;x&quot; &amp; &apos;y&apos;<BR>Last<BR>End<BR>";
    const auto separator = !stock.description.empty() && !stock.description.ends_with("<BR>") ? "<BR>" : "";
    CHECK(changed.description == stock.description + separator + escaped);
    CHECK(changed.title == stock.title);
    CHECK(changed.info[1].text == stock.info[1].text);
    CHECK(game::build_item_display(item, context).description == changed.description);
    CHECK(changed.description.find("<a WndNotify") == std::string::npos);

    const auto native = ui::item_info_lines(item, {}, context.item_description_append);
    REQUIRE(native.size() == native_stock.size() + 4);
    CHECK(std::equal(native_stock.begin(), native_stock.end(), native.begin()));
    CHECK(native[native_stock.size()] == "First <a WndNotify=\"27,test\">&lt;</a>");
    CHECK(native[native_stock.size() + 1] == "Use \"x\" & 'y'");
    CHECK(native[native_stock.size() + 2] == "Last");
    CHECK(native[native_stock.size() + 3] == "End");
    context.item_description_append = [](const net::Item&) -> std::optional<std::string> { return std::nullopt; };
    CHECK(game::build_item_display(item, context).description == stock.description);
    CHECK(ui::item_info_lines(item, {}, context.item_description_append) == native_stock);
    context.item_description_append = [](const net::Item&) { return std::optional(std::string()); };
    CHECK(game::build_item_display(item, context).description == stock.description);
    CHECK(ui::item_info_lines(item, {}, context.item_description_append) == native_stock);
}

TEST_CASE("Item presentation: metadata revision refreshes cached Lua labels, description and measured layout") {
    PresentationGui gui;
    test::ScratchDir scratch("item_presentation_revision");
    {
        std::ofstream file(scratch.path() / "eqstr_us.txt");
        file << "EQST0002\n3094 Magic\n3099 No Trade\n5911 Attunable\n";
    }
    GameDir files(scratch.path());
    auto extension = std::make_shared<PresentationExtension>();
    net::ConnectionOptions options;
    options.plugins = extension;
    net::GameConnection connection(files, options);
    std::vector<net::Item> displays(1);
    std::vector<std::uint64_t> ids{1};
    auto& item = displays.front();
    item.id = 4242;
    item.serial = "preview-copy"; // it need not be present in the inventory
    item.name = "Attunable Sword";
    item.magic = 1;
    item.no_drop = 0;
    item.no_rent = 1;
    item.attuneable = 1;
    app::UiHost host;
    host.conn = &connection;
    host.game = &files;
    host.ui_ready = host.in_game = true;
    host.item_displays = &displays;
    host.item_display_ids = &ids;
    app::UiApi api(host);
    api.load_layout(scratch.path() / "defaults.ini", scratch.path() / "UI_Test_Server.ini", {});
    PresentationLua lua(api);
    // WindowText returns text, is-set; parentheses select its text result.
    const char* label = "return (hs.UI.windows.Text('ItemDisplayWindow', 'IDW_ItemInfo2'))";
    const char* name = "return (hs.UI.windows.Text('ItemDisplayWindow', 'IDW_ItemInfo1'))";
    const char* description = "return (hs.UI.windows.Text('ItemDisplayWindow', 'ItemDescription'))";
    const char* width = "local w = hs.UI.windows.Client('ItemDisplayWindow'); return w";
    const auto stock_flags = lua.text(label);
    const auto stock_description = lua.text(description);
    const auto stock_width = lua.integer(width); // warm both text and layout caches
    REQUIRE(stock_flags == "Magic, No Trade, Attunable");

    extension->enabled = true;
    extension->flags = "Kinbound " + std::string(160, 'W');
    extension->description = "Metadata <notice> & detail\nSecond line";
    ++extension->revision;
    // Ask for layout first: it must invalidate even without frame(), a new
    // item serial, a Modified-button click, or waiting for the periodic cache.
    CHECK(lua.integer(width) > stock_width);
    CHECK(lua.text(label) == extension->flags);
    CHECK(lua.text(name) == "Attunable Sword");
    CHECK(lua.text(description).find("Metadata &lt;notice&gt; &amp; detail<BR>Second line<BR>") != std::string::npos);

    extension->enabled = false;
    ++extension->revision;
    CHECK(lua.integer(width) == stock_width);
    CHECK(lua.text(label) == stock_flags);
    CHECK(lua.text(description) == stock_description);
    CHECK(lua.text(name) == "Attunable Sword");
}

TEST_CASE("Item presentation: installed configuration refreshes cached text and layout without a metadata revision") {
    PresentationGui gui;
    test::ScratchDir scratch("item_configuration_presentation");
    GameDir files(scratch.path());
    auto extension = std::make_shared<PresentationExtension>();
    extension->enabled = true;
    extension->configuration_flags = [](const net::Item& item) {
        return item.contents.empty() || item.item_class == 1 || item.item_class > 2
            ? std::string("Base") : "Configured " + std::string(160, 'W');
    };
    extension->configuration_description = [](const net::Item& item) {
        return item.contents.empty() || item.item_class == 1 || item.item_class > 2 ? std::string("Base item level: 90")
            : "Combined item level: 95, installed position " + std::to_string(item.contents.front().index);
    };
    net::ConnectionOptions options;
    options.plugins = extension;
    net::GameConnection connection(files, options);
    std::vector<net::Item> displays(1);
    std::vector<std::uint64_t> ids{1};
    auto& item = displays.front();
    item.id = 4242;
    item.serial = "same-instance";
    item.name = "Configuration test item";
    app::UiHost host;
    host.conn = &connection;
    host.game = &files;
    host.ui_ready = host.in_game = true;
    host.item_displays = &displays;
    host.item_display_ids = &ids;
    app::UiApi api(host);
    api.load_layout(scratch.path() / "defaults.ini", scratch.path() / "UI_Test_Server.ini", {});
    PresentationLua lua(api);
    const char* width = "local w = hs.UI.windows.Client('ItemDisplayWindow'); return w";
    const char* description = "return (hs.UI.windows.Text('ItemDisplayWindow', 'ItemDescription'))";
    const auto base_width = lua.integer(width);
    CHECK(lua.text(description).find("Base item level: 90") != std::string::npos);
    net::Item augment;
    augment.id = 123;
    item.contents.push_back({0, augment});
    CHECK(lua.integer(width) > base_width);
    CHECK(lua.text(description).find("Combined item level: 95, installed position 0") != std::string::npos);
    item.contents.front().index = 5;
    CHECK(lua.text(description).find("installed position 5") != std::string::npos);
    item.item_class = 1; // same id, serial and children now describe a bag
    CHECK(lua.integer(width) == base_width);
    CHECK(lua.text(description).find("Base item level: 90") != std::string::npos);
    item.item_class = 0;
    CHECK(lua.integer(width) > base_width);
    item.item_class = 3; // unknown configuration must use base-only metadata
    CHECK(lua.integer(width) == base_width);
    item.item_class = 2;
    CHECK(lua.integer(width) > base_width);
    item.contents.clear();
    CHECK(lua.integer(width) == base_width);
    CHECK(lua.text(description).find("Base item level: 90") != std::string::npos);
    CHECK(extension->revision == 1);
}
