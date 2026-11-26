// The shipped UIs over Dear ImGui, part 2 (part 1, tests/ui_pack_windows_test.cpp, says what they cover).
#include <doctest/doctest.h>

#include "ui_pack_session.h"

TEST_CASE("UI packs: each draws the login, server select and character select screens in the client's place") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_pregame_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
            s.host.in_game = s.host.ui_ready = false;
            REQUIRE(luaL_dostring(s.lua.state(), "hs.InGame = function() return false end; hs.UI.session.InGame = hs.InGame") == LUA_OK);

            // The login screen: saved servers with accounts and characters (no keyring), then a new server, then a
            // test login's.
            ui::LoginView login;
            login.servers_dir = dir / "servers";
            login.ui_packs = {{"modern", "Modern", "", "mouse", false, {}}, {"gamepad", "Gamepad", "", "gamepad", false, {}}};
            login.ui = login.default_ui = pack;
            ServerProfile p;
            p.id = "alpha";
            p.name = "Alpha";
            p.host = "127.0.0.1";
            p.username = "first";
            p.accounts = {{"first", "Bob", "World One", {{"Bob", "World One", 12, "WAR"}}, false, false},
                          {"second", "", "", {{"Cid", "World Two", 50, "SHD/CLR"}}, false, false}};
            login.profiles = {p};
            ui::select_profile(login, 0);
            login.game_folders = {{"/games/rof2", false, true}};
            login.status = {"Could not connect.", true};
            login.version = "0.2.0-alpha (abc1234)"; // the small version line at the foot
            s.host.login = &login;
            // the modern pack's box set card open from the first frame (test.login = boxset)
            REQUIRE(luaL_dostring(s.lua.state(), "ui.SetSetting('test.login', 'boxset')") == LUA_OK);
            s.frames(3);
            CHECK(s.api.replaces("LoginScreen"));
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            if (std::string(pack) == "modern") CHECK(ImGui::FindWindowByName("###modern.pregame.login.boxset") != nullptr);
            if (std::string(pack) == "gamepad") {
                // An unavailable installed descriptor remains in the list, while controller cycling skips it.
                ui::UiPack unavailable;
                unavailable.id = "addon.example.unavailable";
                unavailable.name = "Unavailable interface";
                unavailable.available = false;
                unavailable.unavailable_reason = "No native runtime";
                login.ui_packs.insert(login.ui_packs.begin() + 1, unavailable);
                login.ui = "modern";
                REQUIRE(luaL_dostring(s.lua.state(), R"lua(
local original = ui.InvisibleButton
INTERFACE_PICKED, INTERFACE_SEEN = false, 0
ui.InvisibleButton = function(id, ...)
  local hit = original(id, ...)
  if id == '##ui' then
    INTERFACE_SEEN = INTERFACE_SEEN + 1
    if not INTERFACE_PICKED then INTERFACE_PICKED = true; return true end
  end
  return hit
end
RESTORE_INTERFACE_BUTTON = function() ui.InvisibleButton = original end
)lua") == LUA_OK);
                s.frames(2);
                CHECK(login.ui == "gamepad");
                CHECK(s.lua.run_for_test("", "return INTERFACE_SEEN > 0") == "true");
                REQUIRE(luaL_dostring(s.lua.state(), "RESTORE_INTERFACE_BUTTON()") == LUA_OK);
                login.ui_packs.erase(login.ui_packs.begin() + 1);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            }
            // A returned ticket is an optional action, usable without a password, and hidden while busy.
            lua_State* login_lua = s.lua.state();
            REQUIRE(luaL_dostring(login_lua, R"lua(
sessionButtons, clickSession = 0, false
local button, invisible = ui.Button, ui.InvisibleButton
ui.Button = function(label, ...)
  local hit = button(label, ...)
  if label == "Sign in with session" then
    sessionButtons = sessionButtons + 1
    return clickSession or hit
  end
  return hit
end
ui.InvisibleButton = function(id, ...)
  local hit = invisible(id, ...)
  if id == "##resume_token" then
    sessionButtons = sessionButtons + 1
    return clickSession or hit
  end
  return hit
end
)lua") == LUA_OK);
            auto session_buttons = [&] {
                lua_getglobal(login_lua, "sessionButtons");
                const auto n = lua_tointeger(login_lua, -1);
                lua_pop(login_lua, 1);
                return n;
            };
            s.frames(2);
            CHECK(session_buttons() == 0);
            login.session_ticket_available = true;
            REQUIRE(luaL_dostring(login_lua, "clickSession = true") == LUA_OK);
            s.frames(2);
            CHECK(session_buttons() > 0);
            CHECK(s.host.login_action == static_cast<int>(ui::LoginAction::ResumeToken));
            s.host.login_action.reset();
            login.busy = true;
            REQUIRE(luaL_dostring(login_lua, "sessionButtons = 0") == LUA_OK);
            s.frames(2);
            CHECK(session_buttons() == 0);
            CHECK_FALSE(s.host.login_action);
            login.busy = false;
            login.session_ticket_available = false;
            s.frames(2);
            CHECK(session_buttons() == 0);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            // The updater's notice in each of its states (docs/updater.md).
            login.update.version = "9.0.0";
            login.update.current = "0.1.0-alpha";
            login.update.notes = "https://example.invalid/r";
            login.update.has_package = true;
            login.update.can_restart = true;
            for (const char* state : {"available", "downloading", "ready", "failed", ""}) {
                login.update.state = state;
                login.update.progress = 0.4f;
                login.update.error = "no network";
                login.update.message = *state ? "" : "The update was cut off, so 0.1.0-alpha was put back.";
                s.frames(2);
                CHECK_MESSAGE(ImGui::FindWindowByName("UI errors") == nullptr, state);
            }
            login.update = {};
            // The corner: this build, each of the updater's words on it, What's new and the GitHub page.
            login.release_notes = {"A thing that changed", "Another, long enough to wrap over more than one line of the "
                                                            "popup that shows the release notes"};
            login.project_page = "https://example.invalid/hailstone";
            // What's new open (test.whatsnew opens it once in the modern pack; the gamepad pack's page is below)
            REQUIRE(luaL_dostring(s.lua.state(), "ui.SetSetting('test.whatsnew', '1')") == LUA_OK);
            for (const char* check : {"off", "checking", "uptodate", "unknown", "newer", "behind"}) {
                login.update.check = check;
                login.update.off_reason = "a development build";
                s.frames(2);
                CHECK_MESSAGE(ImGui::FindWindowByName("UI errors") == nullptr, check);
            }
            if (std::string(pack) == "gamepad") {
                // What's new is a page of its own there (test.login = notes, as the screen appears again)
                REQUIRE(luaL_dostring(s.lua.state(), "ui.SetSetting('test.login', 'notes')") == LUA_OK);
                s.host.login = nullptr;
                s.frames(2);
                s.host.login = &login;
                s.frames(3);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            }
            login.update = {};
            login.release_notes.clear();
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            // The add-on manager's panel (docs/addons.md): an install card, and rows of each group and state.
            login.addons.open = true;
            login.addons.available = true;
            login.addons.link = "https://github.com/owner/autoloot";
            login.addons.status = {"Looking at https://github.com/owner/autoloot...", false};
            login.addons.server = "Alpha";
            ui::AddonOfferView offer;
            offer.name = "autoloot";
            offer.kind = "plugin";
            offer.version = "1.3.0";
            offer.author = "Some One";
            offer.description = "Loots what you tell it to.";
            offer.source = "owner/autoloot";
            offer.page = "https://github.com/owner/autoloot";
            offer.key = "0123456789abcdef";
            offer.caps = {"read the game state", "draw windows"};
            offer.blocked = "an add-on named autoloot is installed already";
            login.addons.offer = offer;
            for (const char* state : {"uptodate", "available", "downloading", "ready", "approval", "modified",
                                      "blocked", "failed"}) {
                ui::AddonRowView r;
                r.name = std::string("addon-") + state;
                r.kind = "script";
                r.group = "installed";
                r.version = "1.0.0";
                r.source = "owner/" + r.name;
                r.page = "https://github.com/owner/" + r.name;
                r.state = state;
                r.state_text = std::string("State ") + state;
                r.caps = {"read the game state"};
                if (std::string(state) == "approval") r.new_caps = {"speak for you"};
                if (std::string(state) == "modified") r.modified = {"init.lua"};
                r.can_update = r.can_remove = true;
                r.auto_update = true;
                r.folder = "/x/scripts/" + r.name;
                login.addons.rows.push_back(r);
            }
            ui::AddonRowView manual;
            manual.name = "byhand";
            manual.kind = "plugin";
            manual.group = "manual";
            manual.note = "This server runs only its own plugins: it does not run here.";
            manual.folder = "/x/plugins/byhand";
            login.addons.rows.push_back(manual);
            ui::AddonRowView server = manual;
            server.name = "hud";
            server.group = "server";
            server.note = "Provided by Alpha: you allowed it, so it runs there.";
            server.consent = "allowed";
            login.addons.rows.push_back(server);
            server.name = "map";
            server.consent = "denied";
            login.addons.rows.push_back(server);
            // An installed tool (a program of its own): Start.
            ui::AddonRowView tool;
            tool.name = "texstudio";
            tool.kind = "tool";
            tool.group = "installed";
            tool.version = "1.0.0";
            tool.source = "owner/texstudio";
            tool.state = "uptodate";
            tool.can_remove = tool.can_start = true;
            tool.folder = "/x/tools/texstudio";
            login.addons.rows.push_back(tool);
            // An installed upscaler (docs/addons.md "Upscalers").
            ui::AddonRowView upscaler = tool;
            upscaler.name = "xess";
            upscaler.kind = "upscaler";
            upscaler.source = "owner/xess";
            upscaler.can_start = false;
            upscaler.folder = "/x/upscalers/xess";
            login.addons.rows.push_back(upscaler);
            // The store (docs/addons.md "Finding add-ons"): an item in each state, then the list being fetched, then
            // an empty one.
            for (const char* state : {"install", "update", "installed", "manual", "unavailable"}) {
                ui::AddonStoreItemView it;
                it.name = std::string("store-") + state;
                it.source = "owner/" + it.name;
                it.page = "https://github.com/" + it.source;
                it.kind = std::string(state) == "unavailable" ? "" : "plugin";
                it.version = "2.0.0";
                it.author = "Some One";
                it.description = "Does a thing for you, described in a sentence long enough to wrap in the card.";
                it.worlds = std::string(state) == "install" ? "Alpha World" : "";
                it.installed_version = std::string(state) == "update" || std::string(state) == "installed" ? "1.0.0" : "";
                it.state = state;
                it.problem = std::string(state) == "unavailable" ? "store-unavailable has no build for linux-x64" : "";
                login.addons.store.push_back(it);
            }
            login.addons.browsed = true;
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            // A tool's install card: no permissions, the program's warning.
            login.addons.offer->kind = "tool";
            login.addons.offer->caps.clear();
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            // An upscaler's: no permissions, its own warning.
            login.addons.offer->kind = "upscaler";
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            login.addons.offer->kind = "plugin";
            login.addons.offer->caps = {"read the game state", "draw windows"};
            // An older version than the one installed: its warning and the button that says so.
            login.addons.offer->installed_version = "1.4.0";
            login.addons.offer->older = true;
            login.addons.offer->blocked.clear();
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            login.addons.offer->older = false;
            login.addons.offer->installed_version.clear();
            login.addons.offer->blocked = "an add-on named autoloot is installed already";
            if (std::string(pack) == "modern") {
                // the list of their own, then back to the store
                REQUIRE(luaL_dostring(s.lua.state(), "ui.SetSetting('test.addontab', 'mine')") == LUA_OK);
                s.frames(3);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
                REQUIRE(luaL_dostring(s.lua.state(), "ui.SetSetting('test.addontab', 'store')") == LUA_OK);
            }
            login.addons.store.clear();
            login.addons.browsing = true;
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            login.addons.browsing = false;
            login.addons.store_error = "The list of add-ons could not be fetched.";
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            login.addons.offer.reset();
            login.addons.available = false;
            login.addons.why_off = "Add-ons cannot be fetched here.";
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            login.addons = {};
            // Suggested add-ons (docs/addons.md "Suggested add-ons"): the index's and a server's.
            login.addons.suggestions = {
                {"servplug", "owner/servplug", "Alpha World has a Hailstone add-on for its own features: servplug.", false},
                {"", "owner/extra", "Alpha suggests a Hailstone add-on: owner/extra.", true}};
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            login.addons.suggestions.front().required = true;
            login.addons.suggestions.front().text = "Required to play on this server: servplug.";
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            REQUIRE(luaL_dostring(s.lua.state(), "assert(hs.UI.pregame.LoginScreen().addons.suggestions[1].required); "
                                 "assert(not hs.UI.pregame.LoginDo('addonSuggestLater', 'owner/servplug')); "
                                 "assert(not hs.UI.pregame.LoginDo('addonSuggestNever', 'owner/servplug')); "
                                 "assert(hs.UI.pregame.LoginDo('addonSuggestLook', 'owner/servplug'))") == LUA_OK);
            login.addons = {};
            s.frames(2);
            ui::new_profile(login);
            s.frames(2);
            login.transient = true;
            login.busy = true;
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            s.host.login = nullptr;

            ui::ServerSelectView worlds;
            worlds.servers = {{"Open World", "Up", true, true}, {"Locked World", "Locked", true},
                             {"Down World", "Down", false}};
            worlds.selected = 0;
            s.host.server_select = &worlds;
            s.host.last_server = "Open World";
            s.host.login_server = "Alpha";
            s.frames(3);
            CHECK(s.api.replaces("serverselect"));
            REQUIRE(luaL_dostring(s.lua.state(), "local v=hs.ServerSelect(); assert(v.servers[2].status=='Locked' and v.servers[2].canJoin); "
                                     "assert(hs.ServerSelectDo('play',2)); assert(not hs.ServerSelectDo('play',3))") == LUA_OK);
            CHECK(s.host.server_select_action == static_cast<int>(ui::ServerSelectAction::Play));
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            worlds.addon_hint = "Open World has a Hailstone add-on: go Back and open Add-ons to look at it.";
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            s.host.server_select = nullptr;

            // Character select, with characters and without; with the scene and without.
            ui::CharacterSelectView chars;
            chars.server_name = "World One";
            chars.characters = {{"Bob", 12, 1, {1}, "North Qeynos"}, {"Cid", 50, 2, {5, 2}, "Kaladim"}};
            chars.selected = 1;
            chars.scene = true;
            s.host.ui_ready = true;
            s.host.game_state = 1;
            s.host.char_select = &chars;
            s.frames(3);
            CHECK(s.api.replaces("CharacterListWnd"));
            chars.characters.clear();
            chars.selected = -1;
            chars.scene = false;
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            s.host.char_select = nullptr;

            // Character creation, with the scene and without, waiting on the world, and while customizing.
            ui::CharacterCreateView create;
            auto opt = [](std::uint32_t race, std::uint32_t cls, std::uint32_t deity, std::uint32_t zone) {
                ui::CreateOption o{race, cls, deity, zone, {}, {}};
                o.base = {75, 75, 75, 75, 75, 75, 75};
                o.points = {10, 0, 0, 10, 0, 0, 5};
                return o;
            };
            for (std::uint32_t race : {1u, 2u, 3u, 128u})
                for (std::uint32_t cls : {1u, 2u, 3u, 11u}) create.options.push_back(opt(race, cls, 201, 2));
            create.options.push_back(opt(1, 1, 202, 9));
            create.scene = true;
            s.host.game_state = 2;
            s.host.char_create_view = &create;
            s.frames(3);
            CHECK(s.api.replaces("CharacterCreateScreen"));
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            create.name = "Bobby";
            create.scene = false;
            create.status = {"That name is taken.", true};
            s.frames(2);
            create.busy = true;
            s.frames(2);
            s.host.customization_shown = true;
            s.frames(2);
            s.host.customization_shown = false;
            create.options.clear(); // no tables from the server
            create.option = -1;
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            s.host.char_create_view = nullptr;
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: the login screen's More options pick the texture pack (hs.Option texture_pack)") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_login_texture_pack_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
            s.host.in_game = s.host.ui_ready = false;
            REQUIRE(luaL_dostring(s.lua.state(), "hs.InGame = function() return false end") == LUA_OK);
            // A new server (no saved accounts, so the gamepad pack's More page has no box set list above).
            ui::LoginView login;
            login.servers_dir = dir / "servers";
            login.ui_packs = {{"modern", "Modern", "", "mouse", false, {}}, {"gamepad", "Gamepad", "", "gamepad", false, {}}};
            login.ui = login.default_ui = pack;
            login.game_folders = {{"/games/rof2", false, true}};
            s.host.login = &login;
            int chosen = 0; // Off
            bool asked = false;
            std::vector<int> set;
            s.host.find_option = [&](std::string_view o) -> std::optional<int> {
                if (o != "texture_pack") return std::nullopt;
                asked = true;
                return chosen;
            };
            s.host.option_choices = [](std::string_view o) {
                return o == "texture_pack" ? std::vector<std::string>{"Off", "classic", "default"} : std::vector<std::string>{};
            };
            s.host.set_option = [&](std::string_view o, int v) {
                if (o != "texture_pack") return;
                set.push_back(v);
                chosen = v;
            };
            lua_State* L = s.lua.state();
            // More options open from the first frame; a click on the pack "classic": the modern pack's list entry,
            // the gamepad pack's field and then the second row of its list.
            REQUIRE(luaL_dostring(L, R"lua(
ui.SetSetting('test.login', 'more')
clickPack, rows = false, 0
local selectable, invisible = ui.Selectable, ui.InvisibleButton
ui.Selectable = function(label, ...)
  local hit = selectable(label, ...)
  return hit or (clickPack and label == "classic")
end
ui.InvisibleButton = function(id, ...)
  local hit = invisible(id, ...)
  if not clickPack then return hit end
  if id == "##texpack" then return true end
  if id == "##row" then
    rows = rows + 1
    return hit or rows == 2
  end
  return hit
end
)lua") == LUA_OK);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(asked);
            CHECK(set.empty());
            if (std::string(pack) == "modern") {
                // the list drawn as an open combo would draw it (its entries in the row itself: no popup to end)
                REQUIRE(luaL_dostring(L, R"lua(
local begin, finish, faked = ui.BeginCombo, ui.EndCombo, false
ui.BeginCombo = function(id, ...)
  if id == "##texture_pack" and clickPack then faked = true; return true end
  return begin(id, ...)
end
ui.EndCombo = function()
  if faked then faked = false; return end
  return finish()
end
clickPack = true
)lua") == LUA_OK);
                s.frames(1);
            } else {
                REQUIRE(luaL_dostring(L, "clickPack = true") == LUA_OK);
                s.frames(1); // the field opens the list
                REQUIRE(luaL_dostring(L, "rows = 0") == LUA_OK);
                s.frames(1); // its second row
            }
            REQUIRE(luaL_dostring(L, "clickPack = false") == LUA_OK);
            CHECK(set == std::vector<int>{1});
            CHECK(chosen == 1);
            s.frames(2); // the note that it is used from the next start
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: the marketplace draws its shelves, details and pages from a filled store") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_marketplace_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
            auto& m = s.conn.marketplace();
            for (std::uint32_t i = 0; i < 6; ++i) {
                net::MarketplaceEntry e;
                e.id = 100 + i;
                e.name = "Item " + std::to_string(i);
                e.category = i < 3 ? "Gear/Head" : "Fun";
                e.price = 10 * (i + 1);
                e.description = "Line one<BR>marketid=101";
                e.is_new = i == 0;
                e.is_featured = i == 4;
                e.was_price = i == 2 ? 99 : 0;
                e.available_until = i == 1 ? 4102444800 : -1;
                e.item_of_week = i == 4;
                m.catalog.push_back(e);
            }
            m.window.populate(m.catalog, true);
            for (std::uint32_t i = 0; i < 6; ++i) m.window.apply_state(m.catalog, {100 + i, 0, i == 0 ? 1001u : 0u, 0, i == 3 ? "Maker" : ""});
            m.window.rebuild_tree(true);
            m.window.refilter(m.catalog);
            m.wallet = 25;
            m.open = true;
            lua_State* L = s.lua.state();
            // The landing page, the grid, the list, then the confirmation and result pages.
            for (int step = 0; step < 6; ++step) {
                INFO(step);
                if (step == 1) m.window.click_category(m.catalog, 1);
                if (step == 2) m.window.set_view(game::MarketplaceView::List);
                if (step == 3) m.window.page = game::MarketplacePage::TaxInfo;
                if (step == 4) m.window.page = game::MarketplacePage::Confirm;
                if (step == 5) {
                    m.window.page = game::MarketplacePage::Results;
                    m.result_text = "Done.";
                    m.window.preview_tab = true;
                }
                s.frames(2);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            }
            REQUIRE(luaL_dostring(L, "local n = hs.ListInfo('MarketplaceWnd', 'MKPW_CategoriesTree') return n") == LUA_OK);
            CHECK(lua_tointeger(L, -1) >= 4);
            lua_settop(L, 0);
        }
        ImGui::DestroyContext(ctx);
    }
}

namespace {

// spells_us.txt lines: id and name (the rest 0).
std::shared_ptr<game::SpellTable> book_spells() {
    auto line = [](const char* id, const char* name) {
        std::vector<std::string> f(237, "0");
        f[0] = id;
        f[1] = name;
        std::string out;
        for (std::size_t i = 0; i < f.size(); ++i) out += (i ? "^" : "") + f[i];
        return out;
    };
    auto table = std::make_shared<game::SpellTable>();
    REQUIRE(table->parse(line("200", "Minor Healing") + "\n" + line("201", "Courage") + "\n" + line("202", "Strike")) == 3);
    return table;
}

} // namespace

TEST_CASE("UI packs: the spell book's sets and set editor draw; hs.SaveSpellSet / EditSpellSet save, memorizing nothing") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_spellsets_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
            // the book opens only in a zone (CSpellBookWnd::AboutToShow): the mock EQEmu's connection in place of
            // the session's own
            hailstone::test::Rig rig;
            REQUIRE(rig.to_zone());
            auto& conn = rig.conn;
            s.host.conn = &conn;
            conn.set_spell_table_for_test(book_spells());
            REQUIRE(conn.profile());
            net::PlayerProfile pp = *conn.profile();
            pp.class_ = 2; // a cleric: the book opens
            conn.set_profile_for_test(pp);
            for (std::uint32_t i = 0; i < 3; ++i)
                conn.receive_zone_packet_for_test(net::encode(net::MemorizeSpell{i, 200 + i, net::kMemScribe, 0}));
            REQUIRE(conn.spell_state().book[2] == 202);
            lua_State* L = s.lua.state();
            auto run = [&](const char* code) {
                INFO(code);
                REQUIRE(luaL_dostring(L, code) == LUA_OK);
            };
            auto number = [&](const char* code) -> lua_Integer {
                run(code);
                lua_Integer v = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : -100;
                lua_settop(L, 0);
                return v;
            };
            const auto& sets = conn.spell_loadouts();
            const auto gems_before = conn.spell_state().gems;
            // Saved from a table, not the gems: a spell not in the book (999) is kept; nil, -1, negatives and
            // non-numbers are empty gems.
            CHECK(number("return hs.SaveSpellSet('Heals', { 200, -1, 201, nil, 999 })") == 1);
            CHECK(sets.at(0).name == "Heals");
            CHECK(sets.at(0).spells[0] == 200);
            CHECK(sets.at(0).spells[1] == -1);
            CHECK(sets.at(0).spells[2] == 201);
            CHECK(sets.at(0).spells[3] == -1);
            CHECK(sets.at(0).spells[4] == 999);
            CHECK(number("return hs.SaveSpellSet('', { 200 }) or -1") == -1);
            CHECK(number("return hs.EditSpellSet(1, 'Alpha', { 'x', 202, -5 })") == 1);
            CHECK(sets.at(0).name == "Alpha");
            CHECK(sets.at(0).spells[0] == -1);
            CHECK(sets.at(0).spells[1] == 202);
            CHECK(sets.at(0).spells[2] == -1);
            CHECK(number("return hs.EditSpellSet(2, 'Beta', {}) or -1") == -1); // set 2 is not in use
            CHECK(number("return hs.SaveSpellSet('Pulls', { 202, 201 })") == 2);
            // nothing was memorized: no gem, no memorize, no set being memorized
            for (int g = 0; g < game::kSpellGems; ++g)
                CHECK(conn.spell_state().gems[static_cast<std::size_t>(g)].spell ==
                      gems_before[static_cast<std::size_t>(g)].spell);
            CHECK_FALSE(conn.spell_state().memorizing.has_value());
            CHECK_FALSE(conn.memorizing_set());
            run("local t = hs.SpellSets() assert(t[1].name == 'Alpha' and t[1].spells[2] == 202 and t[2].name == 'Pulls')");

            // The views: the list of sets, the editor of set 1, a new set, a new set from the gems, back.
            run("hs.ShowWindow('SpellBookWnd', true)");
            REQUIRE(conn.spell_book_open());
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            const bool modern = std::string(pack) == "modern";
            const char* views[] = {"sets", "1", "new", "gems", "2"};
            const char* gamepad_views[] = {"sets", "edit1", "editnew", "edit2", "book"};
            for (int v = 0; v < 5; ++v) {
                std::string code = modern ? std::string("ui.SetSetting('test.book.edit', '") + views[v] + "')"
                                          : std::string("ui.SetSetting('test.book.view', '") + gamepad_views[v] + "')";
                if (modern && v == 0) code = "ui.SetSetting('test.book.tab', 'sets')";
                INFO(code);
                run(code.c_str());
                s.frames(3);
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            }
            CHECK(ImGui::FindWindowByName(modern ? "###modern.SpellBookWnd" : "Spell Book") != nullptr);
            // all ten in use: a new name is refused, an existing one still saves
            for (int i = 0; i < 8; ++i) {
                std::string code = "return hs.SaveSpellSet('Set" + std::to_string(i) + "', { 200 })";
                CHECK(number(code.c_str()) > 0);
            }
            CHECK(number("return hs.SaveSpellSet('Eleventh', { 200 }) or -1") == -1);
            CHECK(number("return hs.SaveSpellSet('pulls', { 201 })") > 0);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// Two widgets of one window with the same ID make ImGui's "conflicting ID" popup show when either is hovered: the
// Memorized filter and the details pane's Memorize button were both "##mem". The mouse crosses the whole book (in
// the details view, a spell selected) and no hovered item may share its ID.
// A row's Remove in the add-ons card asks first, then removes: the question must open (it was opened from inside the
// row's ID scope while drawn at the window's, so it never showed and nothing was removed), for a script and a plugin.
TEST_CASE("UI packs (modern): an add-on's Remove asks, and its answer removes that add-on") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_addon_remove_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        s.host.in_game = s.host.ui_ready = false;
        lua_State* L = s.lua.state();
        // Every row unfolded; a button labelled `press` reports a click at its `press_at`th drawing in a frame; the
        // texts drawn are kept.
        REQUIRE(luaL_dostring(L, R"(
            hs.InGame = function() return false end
            press, press_at, seen, texts = nil, 0, 0, {}
            local tn, b, t = ui.TreeNode, ui.Button, ui.Text
            ui.TreeNode = function(l) tn(l) return true end
            ui.Button = function(l, ...)
              local r = b(l, ...)
              if l == press then
                seen = seen + 1
                if seen == press_at then return true end
              end
              return r
            end
            ui.Text = function(x, ...) texts[#texts + 1] = tostring(x) return t(x, ...) end
            ui.SetSetting('test.addontab', 'mine')
        )") == LUA_OK);
        ui::LoginView login;
        login.servers_dir = dir / "servers";
        login.ui = login.default_ui = "modern";
        login.addons.open = true;
        for (const char* kind : {"script", "plugin"}) {
            ui::AddonRowView r;
            r.name = std::string("my") + kind;
            r.kind = kind;
            r.group = "installed";
            r.version = "1.0.0";
            r.can_remove = true;
            login.addons.rows.push_back(r);
        }
        s.host.login = &login;
        s.frames(3);
        REQUIRE(ImGui::FindWindowByName("UI errors") == nullptr);
        auto asked = [&](const std::string& q) {
            REQUIRE(luaL_dostring(L, "return table.concat(texts, '|')") == LUA_OK);
            const std::string all = lua_tostring(L, -1);
            lua_settop(L, 0);
            return all.find(q) != std::string::npos;
        };
        int row = 0;
        for (const char* name : {"myscript", "myplugin"}) {
            INFO(name);
            ++row;
            s.host.login_action.reset();
            // The row's own Remove (the rows' buttons come in list order).
            REQUIRE(luaL_dostring(L, ("press, press_at, seen, texts = 'Remove', " + std::to_string(row) + ", 0, {}").c_str())
                    == LUA_OK);
            s.frames(1);
            REQUIRE(luaL_dostring(L, "press, seen, texts = nil, 0, {}") == LUA_OK);
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(asked(std::string("Remove ") + name + "?"));
            CHECK_FALSE(s.host.login_action);
            // The question's Remove: drawn after both rows' buttons.
            REQUIRE(luaL_dostring(L, "press, press_at, seen, texts = 'Remove', 3, 0, {}") == LUA_OK);
            s.frames(1);
            REQUIRE(luaL_dostring(L, "press, seen = nil, 0") == LUA_OK);
            CHECK(s.host.login_action == static_cast<int>(ui::LoginAction::AddonRemove));
            CHECK(login.addons.choice == name);
            s.frames(2);
        }
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs (modern): no two of the spell book's widgets share an ID") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_spellbook_ids_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigDebugHighlightIdConflicts = true;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        hailstone::test::Rig rig;
        REQUIRE(rig.to_zone());
        auto& conn = rig.conn;
        s.host.conn = &conn;
        conn.set_spell_table_for_test(book_spells());
        REQUIRE(conn.profile());
        net::PlayerProfile pp = *conn.profile();
        pp.class_ = 2; // a cleric: the book opens
        conn.set_profile_for_test(pp);
        for (std::uint32_t i = 0; i < 3; ++i)
            conn.receive_zone_packet_for_test(net::encode(net::MemorizeSpell{i, 200 + i, net::kMemScribe, 0}));
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('SpellBookWnd', true) ui.SetSetting('test.book.tab', 'details')"
                                 " ui.SetSetting('test.book.select', '200')") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        ImGuiWindow* book = ImGui::FindWindowByName("###modern.SpellBookWnd");
        REQUIRE(book != nullptr);
        const ImVec2 at = book->Pos, size = book->Size;
        ImGuiContext& g = *ImGui::GetCurrentContext();
        int conflicts = 0;
        for (float y = at.y + 4; y < at.y + size.y; y += 12) { // widgets here are 24 px tall or more
            for (float x = at.x + 4; x < at.x + size.x; x += 16) {
                ImGui::GetIO().AddMousePosEvent(x, y);
                s.frames(1);
                if (g.DebugDrawIdConflictsId != 0) {
                    ++conflicts;
                    MESSAGE("conflicting ID hovered at " << x - at.x << ", " << y - at.y);
                }
            }
        }
        CHECK(conflicts == 0);
    }
    ImGui::DestroyContext(ctx);
}

// With every usable gem full, the details pane's Memorize button (and a double-click, which takes the same gem) has
// no empty gem to use. A gem picked in the strip takes the spell in place of its own when Hailstone's
// replace_gem_spell is on ("Replace gem N"); off, the button stays "No free gem" and does nothing, as the original
// memorizes only into an empty gem. (A bug report: a bard with twelve full gems could only memorize after forgetting
// one.)
TEST_CASE("UI packs (modern): the spell book's Memorize replaces the spell of a picked full gem") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    for (bool replace : {true, false}) {
        INFO("replace_gem_spell " << replace);
        hailstone::test::ScratchDir dir_scratch("ui_pack_spellbook_replace_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / "modern", fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, "modern");
            hailstone::test::Rig rig;
            REQUIRE(rig.to_zone());
            auto& conn = rig.conn;
            s.host.conn = &conn;
            s.host.get_option = [replace](std::string_view o) { return o == "replace_gem_spell" ? (replace ? 1 : 0) : 0; };
            s.host.find_option = [replace](std::string_view o) -> std::optional<int> {
                if (o == "replace_gem_spell") return replace ? 1 : 0;
                return std::nullopt;
            };
            // spells 200..212 scribed into book slots 0..12; 200.. memorized into every usable gem, 212 left over
            std::string lines;
            for (int i = 0; i <= 12; ++i) {
                std::vector<std::string> f(237, "0");
                f[0] = std::to_string(200 + i);
                f[1] = "Spell " + std::to_string(i);
                for (std::size_t k = 0; k < f.size(); ++k) lines += (k ? "^" : "") + f[k];
                lines += "\n";
            }
            auto table = std::make_shared<game::SpellTable>();
            REQUIRE(table->parse(lines) == 13);
            conn.set_spell_table_for_test(table);
            REQUIRE(conn.profile());
            net::PlayerProfile pp = *conn.profile();
            pp.class_ = 2; // a cleric: the book opens
            conn.set_profile_for_test(pp);
            for (std::uint32_t i = 0; i <= 12; ++i)
                conn.receive_zone_packet_for_test(net::encode(net::MemorizeSpell{i, 200 + i, net::kMemScribe, 0}));
            const int usable = conn.usable_gems();
            REQUIRE(usable >= 3);
            for (int g = 0; g < usable; ++g)
                conn.receive_zone_packet_for_test(net::encode(net::MemorizeSpell{
                    static_cast<std::uint32_t>(g), 200 + static_cast<std::uint32_t>(g), net::kMemMemorize, 0}));
            conn.set_sitting(true);
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, "hs.ShowWindow('SpellBookWnd', true) ui.SetSetting('test.book.tab', 'details')"
                                     " ui.SetSetting('test.book.select', '212') ui.SetSetting('test.book.gem', '3')") ==
                    LUA_OK);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            ImGuiWindow* book = ImGui::FindWindowByName("###modern.SpellBookWnd");
            REQUIRE(book != nullptr);
            // the mouse over the Memorize button, found by its ID
            ImGuiContext& g = *ImGui::GetCurrentContext();
            const ImGuiID button = book->GetID("##memorize");
            bool over = false;
            const ImVec2 at = book->Pos, size = book->Size;
            for (float y = at.y + 4; y < at.y + size.y && !over; y += 12) {
                for (float x = at.x + 4; x < at.x + size.x && !over; x += 16) {
                    ImGui::GetIO().AddMousePosEvent(x, y);
                    s.frames(1);
                    over = g.HoveredId == button;
                }
            }
            REQUIRE(over);
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            s.frames(1);
            ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            if (replace) {
                REQUIRE(conn.spell_state().memorizing.has_value());
                CHECK(conn.spell_state().memorizing->gem == 2);
                CHECK(conn.spell_state().memorizing->spell == 212);
                CHECK(conn.spell_state().gems[2].spell == 202); // kept until the server answers
            } else {
                CHECK_FALSE(conn.spell_state().memorizing.has_value());
            }
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: the inventory's alternate currencies draw, and a row selects the stock list's row") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_currency_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);

        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
            std::vector<net::AltCurrencyEntry> entries{{31, 22000, 1500, 1000, true}, {32, 22001, 1501, 500, true}};
            s.conn.receive_zone_packet_for_test(net::encode_alt_currency_populate(entries));
            s.conn.receive_zone_packet_for_test(net::encode_alt_currency_update({31, 7}));
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, "ui.SetSetting('test.inv.page', 'currency') hs.ShowWindow('InventoryWindow', true)") ==
                    LUA_OK);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            // the page keeps the stock Display Missing off with nothing held (modern: Show all off)
            REQUIRE(luaL_dostring(L, "return (select(2, hs.ChildState('InventoryWindow', 'AltCurr_DisplayMissingButton')))") ==
                    LUA_OK);
            CHECK_FALSE(lua_toboolean(L, -1));
            lua_settop(L, 0);
            REQUIRE(luaL_dostring(L, "hs.ListClick('InventoryWindow', 'AltCurr_PointList', 0, 'left') "
                                     "return (hs.ChildState('InventoryWindow', 'AltCurr_CreateItemButton'))") == LUA_OK);
            CHECK(lua_toboolean(L, -1)); // 31, held: Claim is on
            lua_settop(L, 0);
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// Hailstone's Find window (app/lua_ui_find.cpp): the find key's state opens it (modern: the map on its Find page;
// gamepad: a window of its own), it draws the zone's NPCs, and the map closing closes it.
TEST_CASE("UI packs: the Find window draws the zone's NPCs and follows hs.FindOpen") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_find_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);

        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
            auto& world = s.conn.world_for_test();
            auto add = [&](std::uint32_t id, const char* name, std::uint8_t cls, float x) {
                game::Spawn sp;
                sp.id = id;
                sp.name = name;
                sp.class_id = cls;
                sp.is_npc = true;
                sp.position = {x, 0, 0};
                world.spawns[id] = sp;
            };
            add(2, "Arms_Dealer01", 41, 10);
            add(3, "Banker_Bob00", 40, 20);
            add(4, "Wilt00", 20, 30);
            ++world.spawn_set_serial;
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, "ui.SetSetting('map.npcMarks', '1') hs.FindOpen(true)") == LUA_OK);
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            if (std::string(pack) == "modern") {
                CHECK(s.api.shown("MapViewWnd")); // the map opened on its Find page
                REQUIRE(luaL_dostring(L, "hs.ShowWindow('MapViewWnd', false)") == LUA_OK);
                s.frames(2);
                CHECK_FALSE(s.host.find_open); // the map closing closed Find
                REQUIRE(luaL_dostring(L, "hs.FindOpen(true)") == LUA_OK);
                s.frames(2);
                CHECK(s.api.shown("MapViewWnd"));
                s.host.find_open = false; // the find key again: the map goes
                s.frames(2);
                CHECK_FALSE(s.api.shown("MapViewWnd"));
            } else {
                REQUIRE(luaL_dostring(L, "hs.FindOpen(false)") == LUA_OK);
                s.frames(2);
            }
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// The modern extended target window as a library (ui/modern/lib/xtarget.lua): a window of the same name that calls
// it with a row part gets its style and draw called for each filled slot, with the slot number, and draws cleanly.
TEST_CASE("UI packs (modern): lib/xtarget.lua takes a plugin's part on each extended target row") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_xtarget_lib_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    // The test's window, a plugin's (its own folder of windows), takes the pack's place.
    fs::create_directories(dir / "plug");
    std::ofstream(dir / "plug" / "xtarget_cover.lua") << R"lua(
local xtarget = dofile("lib/xtarget.lua")
drawn, styled = {}, {}
ui.window("Extended Target", function()
  xtarget.window({ width = 262, row = { width = 52,
    style = function(r) styled[#styled + 1] = r.n return r.n == 1 and { alpha = 0.45, strike = true, tag = { "IGNORED" } } or nil end,
    draw = function(r, x0, y0, x1, y1, s) drawn[#drawn + 1] = string.format("%d:%s:%d", r.n, r.name, math.floor(x1 - x0 + 0.5)) end,
  } })
end)
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        for (auto [slot, id, name] : {std::tuple{0u, 101u, std::string("a gnoll")}, std::tuple{1u, 102u, std::string("a pup")}}) {
            ByteWriter b;
            b.le<std::uint32_t>(5);
            b.le<std::uint32_t>(1);
            b.le<std::uint32_t>(slot);
            b.le<std::uint8_t>(1);
            b.le<std::uint32_t>(id);
            b.fixed_string(name, name.size() + 1);
            s.conn.receive_zone_packet_for_test(net::AppPacket(net::to_wire(net::Op::XTargetResponse), b.take()));
            game::Spawn& m = s.conn.world_for_test().spawns[id];
            m.id = id;
            m.name = name;
            m.is_npc = true;
            m.hp_percent = 80;
        }
        s.lua.set_folders({{"plug", dir / "plug"}});
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('ExtendedTargetWnd', true)") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        // (the plugin's globals are its own: run in them)
        REQUIRE(luaL_loadstring(L, "return table.concat(drawn, ','), #styled") == LUA_OK);
        s.lua.push_folder_env_for_test("plug");
        lua_setupvalue(L, -2, 1);
        REQUIRE(lua_pcall(L, 0, 2, 0) == LUA_OK);
        std::string out = lua_tostring(L, -2) ? lua_tostring(L, -2) : "";
        INFO(out);
        CHECK(out.find("0:a gnoll:") != std::string::npos);
        CHECK(out.find("1:a pup:") != std::string::npos);
        CHECK(out.find("0:a gnoll:52") != std::string::npos); // the part's width, at UI scale 1
        CHECK(out.find("2:") == std::string::npos);          // an empty slot gets no part
        CHECK(lua_tointeger(L, -1) >= 2);
        lua_settop(L, 0);
    }
    ImGui::DestroyContext(ctx);
}

// The modern extended target window: a row for every slot the server gives, an empty one too (its right click is how
// an empty slot gets a type, Current Target among them), sized to its rows until the player sizes the frame; a frame
// shorter than its rows scrolls them.
TEST_CASE("UI packs (modern): the extended target window shows empty slots and scrolls when sized short") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_xtarget_rows_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        // Five slots, the first holding a gnoll; the other four empty.
        ByteWriter b;
        b.le<std::uint32_t>(5);
        b.le<std::uint32_t>(1);
        b.le<std::uint32_t>(0);
        b.le<std::uint8_t>(1);
        b.le<std::uint32_t>(101);
        b.fixed_string("a gnoll", 8);
        s.conn.receive_zone_packet_for_test(net::AppPacket(net::to_wire(net::Op::XTargetResponse), b.take()));
        game::Spawn& m = s.conn.world_for_test().spawns[101];
        m.id = 101;
        m.name = "a gnoll";
        m.is_npc = true;
        m.hp_percent = 80;
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('ExtendedTargetWnd', true)") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        auto rows = []() -> ImGuiWindow* {
            for (ImGuiWindow* win : GImGui->Windows)
                if (std::string_view(win->Name).find("/##xtrows") != std::string_view::npos) return win;
            return nullptr;
        };
        ImGuiWindow* area = rows();
        REQUIRE(area != nullptr);
        // Five rows of 30 + 3 pixels at scale 1: the four empty slots are drawn too, and the frame fits them.
        CHECK(area->ContentSize.y >= 5 * 33 - 4);
        CHECK(area->ScrollMax.y == doctest::Approx(0.0f));
        // A frame the player made short: the same rows, scrolling.
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.ExtendedTargetWnd', '210,90')") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        area = rows();
        REQUIRE(area != nullptr);
        CHECK(area->ContentSize.y >= 5 * 33 - 4);
        CHECK(area->ScrollMax.y > 0.0f);
    }
    ImGui::DestroyContext(ctx);
}

// The modern extended target window with the most slots (20): six rows tall by default, the rest scrolling; Auto
// expand (hud.expand.ExtendedTargetWnd, the slot menu's row and the frame's) grows it down to the last filled slot.
TEST_CASE("UI packs (modern): the extended target window starts six rows tall and Auto expand grows it to its targets") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_xtarget_expand_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        // Twenty slots: slot 0 and slot 15 hold a spawn.
        ByteWriter b;
        b.le<std::uint32_t>(20);
        b.le<std::uint32_t>(2);
        for (auto [slot, id, name] : {std::tuple{0u, 101u, std::string("a gnoll")}, std::tuple{15u, 102u, std::string("a pup")}}) {
            b.le<std::uint32_t>(slot);
            b.le<std::uint8_t>(1);
            b.le<std::uint32_t>(id);
            b.fixed_string(name, name.size() + 1);
            game::Spawn& m = s.conn.world_for_test().spawns[id];
            m.id = id;
            m.name = name;
            m.is_npc = true;
            m.hp_percent = 80;
        }
        s.conn.receive_zone_packet_for_test(net::AppPacket(net::to_wire(net::Op::XTargetResponse), b.take()));
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, "hs.UI.windows.Show('ExtendedTargetWnd', true)") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        auto rows = []() -> ImGuiWindow* {
            for (ImGuiWindow* win : GImGui->Windows)
                if (std::string_view(win->Name).find("/##xtrows") != std::string_view::npos) return win;
            return nullptr;
        };
        ImGuiWindow* area = rows();
        REQUIRE(area != nullptr);
        // Every slot is a row (30 + 3 pixels at scale 1); the frame shows about six of them and scrolls the rest.
        CHECK(area->ContentSize.y >= 20 * 33 - 4);
        CHECK(area->Size.y >= 6 * 33 - 4);
        CHECK(area->Size.y < 7 * 33);
        CHECK(area->ScrollMax.y > 0.0f);
        // Auto expand: tall enough for the rows down to slot 15 (16 rows); the four after it still scroll.
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.expand.ExtendedTargetWnd', '1')") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        area = rows();
        REQUIRE(area != nullptr);
        CHECK(area->Size.y >= 16 * 33 - 4);
        CHECK(area->Size.y < 17 * 33);
        CHECK(area->ScrollMax.y > 0.0f);
        // A size of the player's own taller than the targets need is kept.
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.ExtendedTargetWnd', '210,800')") == LUA_OK);
        s.frames(3);
        area = rows();
        REQUIRE(area != nullptr);
        CHECK(area->Size.y > 17 * 33);
        // Off again: the player's size, here a short one, and the rows scroll.
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.ExtendedTargetWnd', '210,120') "
                                 "ui.SetSetting('hud.expand.ExtendedTargetWnd', '0')") == LUA_OK);
        s.frames(3);
        area = rows();
        REQUIRE(area != nullptr);
        CHECK(area->Size.y < 3 * 33);
        CHECK(area->ScrollMax.y > 0.0f);
    }
    ImGui::DestroyContext(ctx);
}

// The modern AA window as a library (ui/modern/lib/aa.lua): a window of the same name that calls it with a row part,
// a details part and a banner gets each called, the row's with the ability's name, ranks and cost, and draws cleanly.
// Its one list (AAWindow's ModernList) holds every tab's abilities, with the tab, era and activated columns.
TEST_CASE("UI packs (modern): lib/aa.lua takes a script's part on each AA row, in the details and as a banner") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_aa_lib_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    std::ofstream(dir / "game" / "dbstr_us.txt")
        << "9001^1^Innate Strength^0^\n9002^1^Mystical Shielding^0^\n5^20^The Planes of Power^0^\n7^19^Defense^0^\n";
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    fs::create_directories(dir / "plug");
    std::ofstream(dir / "plug" / "aa_cover.lua") << R"lua(
local aa = dofile("lib/aa.lua")
drawn, styled, details, banners = {}, {}, 0, 0
ui.window("Alternate Advancement", function()
  aa.window({
    row = { width = 40,
      style = function(r) styled[#styled + 1] = r.name return { ring = { 1, 0.8, 0.2, 1 }, tag = { "AUTO" } } end,
      draw = function(r, x0, y0, x1, y1, s)
        drawn[#drawn + 1] = string.format("%s:%d/%d:%s:%d", r.name, r.rank, r.max, r.cost, math.floor(x1 - x0 + 0.5))
      end },
    details = function(sel, x, y, w, s) details = details + 1 return 20 * s end,
    banner = function(x, y, w, s) banners = banners + 1 return 24 * s end,
  })
end)
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        net::PlayerProfile pp;
        pp.class_ = 2;
        pp.level = 60;
        s.conn.set_profile_for_test(pp);
        for (auto [id, title, cost] : {std::tuple{100u, 9001, 2}, std::tuple{200u, 9002, 5}}) {
            game::AltAbility a;
            a.id = id;
            a.group = static_cast<std::int32_t>(id / 10);
            a.rank = 1;
            a.max_rank = 3;
            a.cost = cost;
            a.level_req = 10;
            a.classes = 1u << 2;
            a.type = id == 100 ? 1 : 3; // General, Class
            a.title_sid = title;
            if (id == 200) {
                a.spell = 1234; // activated
                a.refresh = 60;
                a.expansion = 5;
                a.category = 7;
            }
            a.prereq_groups = {0};
            a.prereq_ranks = {0};
            s.conn.receive_zone_packet_for_test(net::encode_send_aa_table(a));
        }
        s.lua.set_folders({{"plug", dir / "plug"}});
        lua_State* L = s.lua.state();
        // the one list: both tabs' abilities, each with its tab, era and activated
        REQUIRE(luaL_dostring(L, R"lua(
local out = {}
for _, t in ipairs(hs.WindowList('AAWindow', 'ModernList') or {}) do out[#out + 1] = table.concat(t, '|') end
return table.concat(out, ';'))lua") == LUA_OK);
        CHECK(std::string(lua_tostring(L, -1)) ==
              "Innate Strength|0/3|2||1||0|0;Mystical Shielding|0/3|5|Defense|3|The Planes of Power|1|5");
        lua_settop(L, 0);
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('AAWindow', true)") == LUA_OK);
        s.frames(3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(luaL_loadstring(L, "return table.concat(drawn, ','), #styled, banners") == LUA_OK);
        s.lua.push_folder_env_for_test("plug");
        lua_setupvalue(L, -2, 1);
        REQUIRE(lua_pcall(L, 0, 3, 0) == LUA_OK);
        std::string out = lua_tostring(L, -3) ? lua_tostring(L, -3) : "";
        INFO(out);
        CHECK(out.find("Innate Strength:0/3:2:40") != std::string::npos); // the part's width, at UI scale 1
        CHECK(out.find("Mystical Shielding:0/3:5:40") != std::string::npos);
        CHECK(lua_tointeger(L, -2) >= 2);
        CHECK(lua_tointeger(L, -1) >= 1);
        lua_settop(L, 0);
        // A selected ability brings the details part; a row of the one list selects that ability (the second row:
        // the Class tab's), and the list and the stock tab's list both show it selected.
        REQUIRE(luaL_dostring(L, "hs.ListClick('AAWindow', 'ModernList', 1, 'left')") == LUA_OK);
        s.frames(2);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        REQUIRE(luaL_dostring(L, "local _, _, a = hs.ListInfo('AAWindow', 'ModernList') local _, _, b = "
                                 "hs.ListInfo('AAWindow', 'List3') return a, b, (hs.WindowText('AAWindow', "
                                 "'ModernSelected'))") == LUA_OK);
        CHECK(lua_tointeger(L, -3) == 1);
        CHECK(lua_tointeger(L, -2) == 0);
        CHECK(std::string(lua_tostring(L, -1)).starts_with("200\tMystical Shielding\t"));
        lua_settop(L, 0);
        REQUIRE(luaL_loadstring(L, "return details") == LUA_OK);
        s.lua.push_folder_env_for_test("plug");
        lua_setupvalue(L, -2, 1);
        REQUIRE(lua_pcall(L, 0, 1, 0) == LUA_OK);
        CHECK(lua_tointeger(L, -1) >= 1);
        lua_settop(L, 0);
    }
    ImGui::DestroyContext(ctx);
}

// Once the selected rank is bought, the AA window selects the ability's next rank (the one its list now shows), so
// that Train is ready for it without a click on the row again.
TEST_CASE("UI packs (modern): the AA window's selection follows its ability to the next rank once bought") {
    hailstone::test::ScratchDir dir_scratch("ui_pack_aa_follow_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern", dir / "modern",
             fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        Session s(dir, "modern");
        net::PlayerProfile pp;
        pp.class_ = 2;
        pp.level = 60;
        s.conn.set_profile_for_test(pp);
        for (auto [id, r, prev, next] : {std::tuple{100u, 1, -1, 101}, std::tuple{101u, 2, 100, 102},
                                         std::tuple{102u, 3, 101, -1}}) {
            game::AltAbility a;
            a.id = id;
            a.group = 10;
            a.rank = r;
            a.prev_id = prev;
            a.next_id = next;
            a.max_rank = 3;
            a.cost = 1;
            a.classes = 1u << 2;
            a.type = 1;
            a.prereq_groups = {0};
            a.prereq_ranks = {0};
            s.conn.receive_zone_packet_for_test(net::encode_send_aa_table(a));
        }
        lua_State* L = s.lua.state();
        auto selected = [&] {
            REQUIRE(luaL_dostring(L, "hs.WindowList('AAWindow', 'ModernList') "
                                     "return (hs.WindowText('AAWindow', 'ModernSelected'))") == LUA_OK);
            std::string out = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return out.substr(0, out.find('\t'));
        };
        REQUIRE(luaL_dostring(L, "hs.WindowList('AAWindow', 'ModernList') "
                                 "hs.ListClick('AAWindow', 'ModernList', 0, 'left')") == LUA_OK);
        lua_settop(L, 0);
        CHECK(selected() == "100");
        auto own = [&](std::uint32_t id) {
            net::RespondAA r;
            r.owned = {{id, 1, 0}};
            s.conn.receive_zone_packet_for_test(net::encode(r));
        };
        own(100);
        CHECK(selected() == "101");
        own(101);
        CHECK(selected() == "102");
        own(102); // the last rank bought: it stays selected
        CHECK(selected() == "102");
    }
    ImGui::DestroyContext(ctx);
}

// The modern AA window's list starts sorted by name, A to Z (not the game's order); a click on a column's head sorts
// by that column and a second click the other way; dragging a head moves the column. Both are kept as settings.
TEST_CASE("UI packs (modern): the AA list sorts by name, by a clicked head, and a head drags to move its column") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_aa_sort_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    std::ofstream(dir / "game" / "dbstr_us.txt") << "9001^1^Innate Strength^0^\n9002^1^Mystical Shielding^0^\n";
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    fs::create_directories(dir / "plug");
    // the row part records the rows in the order drawn, the part's left edge and the first row's top
    std::ofstream(dir / "plug" / "aa_sort.lua") << R"lua(
local aa = dofile("lib/aa.lua")
drawn, px, top = {}, 0, 1e9
ui.window("Alternate Advancement", function()
  drawn, top = {}, 1e9
  aa.window({ row = { width = 20, draw = function(r, x0, y0)
    drawn[#drawn + 1] = r.name
    px, top = x0, math.min(top, y0)
  end } })
end)
)lua";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        net::PlayerProfile pp;
        pp.class_ = 2;
        pp.level = 60;
        s.conn.set_profile_for_test(pp);
        // the game's order: Mystical Shielding (General) before Innate Strength (Class)
        for (auto [id, title, cost, type] : {std::tuple{100u, 9002, 2, 1}, std::tuple{200u, 9001, 5, 3}}) {
            game::AltAbility a;
            a.id = id;
            a.group = static_cast<std::int32_t>(id / 10);
            a.rank = 1;
            a.max_rank = 3;
            a.cost = cost;
            a.level_req = 10;
            a.classes = 1u << 2;
            a.type = type;
            a.title_sid = title;
            a.prereq_groups = {0};
            a.prereq_ranks = {0};
            s.conn.receive_zone_packet_for_test(net::encode_send_aa_table(a));
        }
        s.lua.set_folders({{"plug", dir / "plug"}});
        lua_State* L = s.lua.state();
        REQUIRE(luaL_dostring(L, "hs.ShowWindow('AAWindow', true)") == LUA_OK);
        s.frames(3);
        REQUIRE(ImGui::FindWindowByName("UI errors") == nullptr);
        auto read = [&](double& part_x, double& top_y) {
            REQUIRE(luaL_loadstring(L, "return table.concat(drawn, ','), px, top") == LUA_OK);
            s.lua.push_folder_env_for_test("plug");
            lua_setupvalue(L, -2, 1);
            REQUIRE(lua_pcall(L, 0, 3, 0) == LUA_OK);
            std::string out = lua_tostring(L, -3) ? lua_tostring(L, -3) : "";
            part_x = lua_tonumber(L, -2);
            top_y = lua_tonumber(L, -1);
            lua_settop(L, 0);
            return out;
        };
        // (the window is the script's, so its settings are the script's own: "plugin.<tag>.<key>")
        auto setting = [&](const char* key) {
            REQUIRE(luaL_dostring(L, (std::string("return ui.GetSetting('plugin.plug.") + key + "') or ''").c_str()) ==
                    LUA_OK);
            std::string v = lua_tostring(L, -1);
            lua_settop(L, 0);
            return v;
        };
        double part_x = 0, top_y = 0;
        CHECK(read(part_x, top_y) == "Innate Strength,Mystical Shielding");
        // the cost head: right-aligned at the rows' right (short of the part), its text over the rows' first line
        const float cost_x = static_cast<float>(part_x) - 8.0f - 19.0f;
        const float head_y = static_cast<float>(top_y) - 10.0f;
        auto click = [&](float x, float y) {
            ImGui::GetIO().AddMousePosEvent(x, y);
            s.frames(1);
            ImGui::GetIO().AddMouseButtonEvent(0, true);
            s.frames(1);
            ImGui::GetIO().AddMouseButtonEvent(0, false);
            s.frames(2);
        };
        click(cost_x, head_y);
        CHECK(setting("aa.sort") == "cost:asc");
        CHECK(read(part_x, top_y) == "Mystical Shielding,Innate Strength");
        click(cost_x, head_y);
        CHECK(setting("aa.sort") == "cost:desc");
        CHECK(read(part_x, top_y) == "Innate Strength,Mystical Shielding");
        // dragged left of every head, the cost column goes first; the drag does not sort
        ImGui::GetIO().AddMousePosEvent(cost_x, head_y);
        s.frames(1);
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        s.frames(1);
        for (int i = 1; i <= 10; ++i) {
            ImGui::GetIO().AddMousePosEvent(cost_x - static_cast<float>(i) * 60.0f, head_y);
            s.frames(1);
        }
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        s.frames(2);
        CHECK(setting("aa.columns").starts_with("cost,name,"));
        CHECK(setting("aa.sort") == "cost:desc");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// Every row the EQ menu can show (RoF2's CascadeMenu.txt, other skins' and later clients' names for the same
// windows, the rows this client adds) has a line icon of its own in both packs, not the fallback dot, and draws.
TEST_CASE("UI packs: every EQ menu row has a menu icon") {
    constexpr const char* kScript = R"lua(
local icons = dofile("lib/menuicons.lua")
local rows = {
  "Navigation", "Compass", "Find", "Map", "Atlas/Map",
  "Players", "Group Status", "Group Window", "Guild", "Raid", "Friends/Ignore", "Looking for Group",
  "Looking for Guild",
  "Actions", "Actions Window", "Combat Abilities", "Ability Book", "Target", "Extended Target", "Hotkeys",
  "Hotkeys 1", "Hotkeys 10", "Bandolier", "Alarm", "Alarm Window", "Aggro Meter",
  "Quests", "Expeditions", "Instance Information", "Quest Journal", "NPC Journal", "Storyline",
  "Character", "Player Information", "Player Info Window", "Pet", "Spell Gems", "Spell Book",
  "Alternate Advancement", "Buffs", "Short Duration Buffs", "Buffs (Short Duration)", "Leadership", "Skills",
  "Blocked Buffs", "Blocked Pet Buffs", "Auras",
  "Miscellaneous", "Bazaar Search", "Barter Search", "Tribute", "PvP Stats", "Mail", "Mercenaries",
  "Inventory", "Options", "Help", "Everquest Help", "Lua Scripts", "Web Browser", "Report a Bug", "Camp & Quit",
  "Marketplace", "Real Estate", "Manage", "Plot Search", "Real Estate Items", "Plugins",
}
local missing = {}
ui.Begin("menu icons test")
for _, t in ipairs(rows) do
  if icons.Name(t) == "dot" then missing[#missing + 1] = t end
  icons.Draw(t, 40, 40, 18, { 1, 1, 1 }, 1)
end
ui.End()
return table.concat(missing, ", ")
)lua";
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_menu_icons_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
            s.frames(1);
            ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
            s.api.begin_frame();
            ImGui::NewFrame();
            lua_State* L = s.lua.state();
            int rc = luaL_dostring(L, kScript);
            std::string out = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
            INFO(out);
            CHECK(rc == LUA_OK);
            CHECK(out.empty()); // (on success: the rows with no icon of their own)
            lua_settop(L, 0);
            ImGui::Render();
        }
        ImGui::DestroyContext(ctx);
    }
}

#ifdef HAILSTONE_MOCK_WEB_HELPER
// The browser window in both packs with a browser behind it (the stand-in helper, tests/mock_web_helper.cpp, over
// real pipes): hs.Web* from the window's Lua, and the window drawing the tabs without script errors.
TEST_CASE("UI packs: the Lua Scripts window and hs.Scripts*: list, run, pause, stop, autorun") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_scripts_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::create_directories(dir / "scripts" / "other");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        std::ofstream(dir / "scripts" / "loop.lua", std::ios::binary) << "while true do require('mq').delay(10) end\n";
        std::ofstream(dir / "scripts" / "other" / "init.lua", std::ios::binary) << "print(1)\n";
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            plugin::Hooks hooks;
            plugin::Policy policy;
            mq::Registry reg;
            mq::Context mctx;
            mq::ScriptEnv se;
            se.hooks = &hooks;
            se.policy = &policy;
            se.registry = &reg;
            se.context = [&]() -> const mq::Context& { return mctx; };
            se.dirs = {dir / "scripts"};
            se.config_dir = dir / "scripts" / "config";
            mq::ScriptHost scripts(se);
            Session s(dir, pack);
            s.host.scripts = &scripts;
            lua_State* L = s.lua.state();
            auto run = [&](const char* code) {
                const bool ok = luaL_dostring(L, code) == LUA_OK;
                if (!ok) std::fprintf(stderr, "lua: %s: %s\n", code, lua_tostring(L, -1));
                REQUIRE(ok);
            };
            // Before the zone-in's rules: listed, not allowed, with the reason; no character's list yet.
            run("local l, info = hs.Scripts(true); assert(#l == 2 and l[1].name == 'loop' and l[2].name == 'other');"
                "assert(not l[1].allowed and l[1].why:find('zone%-in') and l[1].status == '' and l[1].found);"
                "assert(not info.autorun and info.folders[1]:find('scripts'));"
                "local ok, why = hs.ScriptRun('loop'); assert(not ok and why:find('may not run'))");
            run("hs.ShowWindow('ScriptsWnd', true)");
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            policy.set(plugin::Rules{}); // no hailstone.cfg: the player's scripts run
            run("local l = hs.Scripts(true); assert(l[1].allowed and l[1].caps ~= '')");
            // A plugin's code: a script of its own name alone, and no folders.
            fs::create_directories(dir / "pf" / "a");
            fs::create_directories(dir / "pf" / "b");
            std::ofstream(dir / "pf" / "a" / "w.lua") << "x = 1\n";
            std::ofstream(dir / "pf" / "b" / "w.lua") << "x = 1\n";
            s.lua.set_folders({{"loop", dir / "pf" / "a"}, {"plugin:x", dir / "pf" / "b"}});
            CHECK(s.lua.run_for_test("loop", "local l, info = hs.Scripts() return #l .. ' ' .. l[1].name .. ' ' .. "
                                             "#info.folders") == "1 loop 0");
            CHECK(s.lua.run_for_test("plugin:x", "local l, info = hs.Scripts() return #l .. ' ' .. #info.folders") ==
                  "0 0");
            run("local l, info = hs.Scripts(); assert(#l == 2 and #info.folders >= 1)"); // the pack: all, as before
            run("assert(hs.ScriptRun('loop', 'a \\\"b c\\\"'))");
            CHECK(scripts.running("loop"));
            run("local l = hs.Scripts(); assert(l[1].status == 'running' and not l[1].kept)");
            s.frames(2);
            bool drawn = false; // the window is there (not only free of errors)
            for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows)
                drawn |= win->Active && std::string(win->Name).find("Lua Scripts") != std::string::npos;
            CHECK(drawn);
            run("assert(hs.ScriptPause('loop')); assert(hs.Scripts()[1].status == 'paused')");
            s.frames(2);
            run("assert(hs.ScriptStop('loop')); assert(not hs.ScriptStop('nope'))");
            CHECK_FALSE(scripts.running("loop"));
            // The character's autorun list: refused before a character, kept in its scripts.ini after.
            run("assert(not hs.SetScriptAutorun('other', true))");
            scripts.set_autorun_file(dir / "chars" / "scripts.ini");
            run("local _, info = hs.Scripts(); assert(info.autorun); assert(hs.SetScriptAutorun('other', true));"
                "local l = hs.Scripts(true); assert(l[2].autorun and not l[1].autorun)");
            CHECK(scripts.autorun("other"));
            CHECK(fs::exists(dir / "chars" / "scripts.ini"));
            s.frames(2);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            run("hs.ShowWindow('ScriptsWnd', false)");
            s.host.scripts = nullptr;
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: the web browser window and hs.Web* with a browser") {
    for (const char* pack : {"modern", "gamepad"}) {
        INFO(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_browser_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            web::BrowserHost browser([] { return web::process_transport({HAILSTONE_MOCK_WEB_HELPER}); });
            Session s(dir, pack);
            int wants_text = 0;
            app::WebService web({[&] { return &browser; }, [] { return nullptr; },
                                 [&] { return (dir / "web" / "server").string(); }, [] { return std::vector<std::string>{}; },
                                 [&] { ++wants_text; }});
            s.host.web = &web;
            lua_State* L = s.lua.state();
            auto run = [&](const char* code) {
                const bool ok = luaL_dostring(L, code) == LUA_OK;
                if (!ok) std::fprintf(stderr, "lua: %s: %s\n", code, lua_tostring(L, -1));
                REQUIRE(ok);
            };
            run("assert(hs.WebAvailable())");
            run("local id, why = hs.WebOpen('file:///etc/passwd'); assert(id == nil and why:find('not an http'))");
            run("TAB = hs.WebOpen('example.org', 64, 48); assert(TAB)");
            // A script's tab is not the player's: the window does not list it.
            web::TabOptions theirs;
            theirs.owner = "somescript";
            REQUIRE(browser.open("https://example.org/", theirs) != 0);
            run("assert(#hs.WebTabs() == 1 and hs.WebTabs()[1] == TAB)");
            run("hs.ShowWindow('BrowserWnd', true)");
            const web::Tab* t = browser.tab(1);
            REQUIRE(t);
            for (int i = 0; i < 2000 && t->title.empty(); ++i) {
                browser.poll(0);
                s.frames(1);
            }
            CHECK(t->title == "Mock https://example.org");
            CHECK(t->options.profile == (dir / "web" / "server").string());
            run("local st = hs.WebState(TAB); assert(st.url == 'https://example.org' and not st.closed)");
            run("local ok, why = hs.WebNavigate(TAB, 'file:///etc/passwd'); assert(not ok and why)");
            // Typed text without a scheme gets https:// in front: "javascript:..." is a (bad) web address, no script.
            run("assert(hs.WebNavigate(TAB, 'javascript:alert(1)'))");
            CHECK(t->url == "https://javascript:alert(1)");
            run("assert(hs.WebNavigate(TAB, 'https://example.org/b'))");
            run("hs.WebNav(TAB, 'reload'); hs.WebText(TAB, 'hi')");
            s.frames(3);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            CHECK(wants_text == 0); // the page was never clicked: it has not the keyboard
            run("hs.ShowWindow('BrowserWnd', false)");
            // The page's input (hs.WebView): a click reaches the page (the stand-in paints the pixel white) and gives
            // it the keyboard; a click elsewhere takes the keyboard back.
            run("ui.window('ViewTest', function() ui.SetNextWindowPos(0, 0); ui.Begin('ViewTest');"
                " hs.WebView(TAB, 64, 48); VX, VY = ui.GetItemRect(); ui.End() end)");
            s.frames(1);
            lua_getglobal(L, "VX");
            lua_getglobal(L, "VY");
            const float vx = static_cast<float>(lua_tonumber(L, -2)), vy = static_cast<float>(lua_tonumber(L, -1));
            lua_pop(L, 2);
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(vx + 10, vy + 5);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            io.AddInputCharacter('z');
            s.frames(1);
            CHECK(wants_text > 0);
            // The clicked pixel (10, 5), once the page has the view's size (the browser window had it bigger).
            auto clicked = [&] {
                const std::size_t at = (5 * std::size_t{t->view.width} + 10) * 4;
                return t->view.width == 64 && t->view.bgra.size() > at && t->view.bgra[at] == std::byte{0xff};
            };
            for (int i = 0; i < 20000 && !clicked(); ++i) {
                browser.poll(0);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            CHECK(clicked());
            io.AddMousePosEvent(vx + 400, vy + 400);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            io.AddMouseButtonEvent(0, false);
            const int before = wants_text;
            s.frames(2);
            CHECK(wants_text == before); // the keyboard is back with the game
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            run("hs.WebClose(TAB); assert(hs.WebState(TAB) == nil)");

            // A plugin's window (its folder's ui/<pack>/): hs.Web* as "plugin:<name>", with the plugin's permissions.
            std::uint32_t plug_caps = plugin::kCapAll & ~plugin::kCapWeb;
            s.host.plugin_caps = [&](std::string_view name) { return name == "plugin:Plug" ? plug_caps : 0u; };
            fs::create_directories(dir / "plug");
            std::ofstream(dir / "plug" / "pw.lua")
                << "ui.window('PlugWeb', function()\n"
                   "  if PW_DONE then return end\n"
                   "  PW_DONE = true\n"
                   "  PW_TAB, PW_WHY = hs.WebOpen('https://example.org/')\n"
                   "  PW_PLAYER_TABS = #hs.WebTabs()\n"
                   "  PW_EXTERNAL = hs.WebOpenExternal('https://example.org/')\n"
                   "end)\n";
            s.lua.set_folders({{"plugin:Plug", dir / "plug"}});
            s.frames(2);
            auto run_plug = [&](const char* code) { // in the plugin's own globals
                bool ok = luaL_loadstring(L, code) == LUA_OK;
                if (ok) {
                    s.lua.push_folder_env_for_test("plugin:Plug");
                    lua_setupvalue(L, -2, 1);
                    ok = lua_pcall(L, 0, 0, 0) == LUA_OK;
                }
                if (!ok) std::fprintf(stderr, "lua: %s: %s\n", code, lua_tostring(L, -1));
                REQUIRE(ok);
            };
            run_plug("assert(PW_TAB == nil and PW_WHY:find('web'), tostring(PW_WHY))");
            run_plug("assert(PW_EXTERNAL == false)");
            CHECK(web.tabs("plugin:Plug").empty());
            plug_caps = plugin::kCapAll;
            run_plug("PW_DONE = nil");
            s.frames(2);
            run_plug("assert(PW_TAB ~= nil and PW_PLAYER_TABS == 1)");
            CHECK(web.tabs("plugin:Plug").size() == 1);
            CHECK(web.tabs("").empty()); // the pack's own window sees none of the plugin's tabs
            s.lua.set_folders({});
            browser.shutdown();
        }
        ImGui::DestroyContext(ctx);
    }
}
#endif
namespace {

// The most opaque vertex a window drew last frame (0..255), or -1 when the window is not there.
int max_alpha(const char* name) {
    ImGuiWindow* w = ImGui::FindWindowByName(name);
    if (!w || !w->WasActive) return -1;
    int out = 0;
    for (const ImDrawVert& v : w->DrawList->VtxBuffer) out = std::max(out, static_cast<int>((v.col >> IM_COL32_A_SHIFT) & 0xff));
    return out;
}

} // namespace

TEST_CASE("UI packs (modern): the Options window's Window Transparency and a window's Fades") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_fade_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        const char* player = "Player###modern.PlayerWindow";
        ImGui::GetIO().MousePos = ImVec2(-1000, -1000); // away from every window
        s.frames(3);
        REQUIRE(max_alpha(player) == 255);
        // Window Transparency: every window at most that opaque (eased over the fade's half second).
        REQUIRE(luaL_dostring(L, "hs.SetUiIni('Main', 'GlobalAlpha', '128')") == LUA_OK);
        s.frames(40);
        CHECK(max_alpha(player) <= 128);
        CHECK(max_alpha(player) >= 120);
        // Fades: after the delay (2 s) and the fade (0.5 s) with the mouse away, the Fade-to Transparency.
        REQUIRE(luaL_dostring(L, "hs.SetUiIni('Main', 'GlobalAlpha', '255') hs.SetUiIni('Main', 'GlobalFadeToAlpha', '64') "
                                 "ui.SetSetting('hud.fades.PlayerWindow', '1')") == LUA_OK);
        s.frames(60); // back to full first (the mouse counts as over at the start)
        CHECK(max_alpha(player) == 255); // within the delay
        s.frames(150);
        CHECK(max_alpha(player) <= 64);
        // The mouse back over it: full again after the fade.
        ImGuiWindow* win = ImGui::FindWindowByName(player);
        REQUIRE(win);
        ImGui::GetIO().MousePos = ImVec2(win->Pos.x + 5, win->Pos.y + 5);
        s.frames(40);
        CHECK(max_alpha(player) == 255);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs (modern): a resizable dialog's Scale zooms the whole window") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_dialog_scale_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        const char* guild = "Guild###modern.GuildManagementWnd";
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.GuildManagementWnd', '600,450') "
                                 "hs.ShowWindow('GuildManagementWnd', true)") == LUA_OK);
        s.frames(3);
        ImGuiWindow* win = ImGui::FindWindowByName(guild);
        REQUIRE(win);
        const ImVec2 before = win->Size;
        CHECK(before.x == doctest::Approx(600).epsilon(0.02));
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.scale.GuildManagementWnd', '1.5')") == LUA_OK);
        s.frames(3);
        CHECK(win->Size.x == doctest::Approx(before.x * 1.5f).epsilon(0.02));
        CHECK(win->Size.y == doctest::Approx(before.y * 1.5f).epsilon(0.02));
        // its size in design pixels is kept
        REQUIRE(luaL_dostring(L, "return ui.GetSetting('hud.size.GuildManagementWnd')") == LUA_OK);
        CHECK(std::string(lua_tostring(L, -1)) == "600,450");
        lua_settop(L, 0);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs (modern): an invitation to a group pops up with who sent it") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_group_invite_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        const char* popup = "Group Invite###modern.Group Invite";
        auto shown = [&] {
            ImGuiWindow* win = ImGui::FindWindowByName(popup);
            return win && win->WasActive;
        };
        s.frames(2);
        CHECK_FALSE(shown());
        net::GroupInvite inv;
        inv.invitee = "Tester";
        inv.inviter = "Bob";
        s.conn.receive_zone_packet_for_test(net::encode(inv));
        s.frames(2);
        CHECK(shown());
        CHECK(s.api.replaces("GroupInvite")); // the client's own prompt stays off: one popup, not two
        REQUIRE(luaL_dostring(L, "return (hs.WindowText('GroupWindow', 'Hailstone_Inviter'))") == LUA_OK);
        CHECK(std::string(lua_tostring(L, -1)) == "Bob");
        lua_settop(L, 0);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// A server's compiled plugins ask before they load (docs/plugins.md): each pack draws the question in the client's
// place (hs.PluginConsent; "PluginConsent" replaced), and nothing while there is none.
// A window key reaches a pack's window only while the pack claims it (hs.ReplaceWindow, each frame): the achievement
// and reward keys must work while their windows are hidden.
TEST_CASE("UI packs: each claims the achievement and reward windows while they are hidden, so their keys open them") {
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch(std::string("ui_pack_progress_keys_") + pack);
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
            s.frames(2);
            CHECK_FALSE(s.api.shown("AchievementsWnd"));
            CHECK_FALSE(s.api.shown("RewardSelectionWnd"));
            CHECK(s.api.replaces("AchievementsWnd"));
            CHECK(s.api.replaces("RewardSelectionWnd"));
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}
