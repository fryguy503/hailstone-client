// The pre-game screens over the hs API (app/lua_ui_pregame.cpp): what hs.LoginScreen, hs.ServerSelect and
// hs.CharacterSelect show from the client's views, what their actions change or ask the client for, and that reading
// a view is what makes a pack's window stand in for the client's screen (UiApi::replaces).
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "core/server_profile.h"
#include "plugin/policy.h"
#include "scratch_dir.h"
#include "ui/dev_screens.h"
#include "ui/lua/lua_host.h"

using namespace hailstone;

namespace {

std::string run(lua_State* L, const std::string& code) {
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_settop(L, 0);
        return "error: " + e;
    }
    std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}

struct Ui {
    app::UiHost host;
    app::UiApi api{host};
    lua_State* L = nullptr;
    Ui() {
        L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
    }
    ~Ui() { lua_close(L); }
    std::string operator()(const std::string& code) { return run(L, code); }
};

// Runs code in a plugin folder's own globals (ui::lua::LuaUi::set_folders).
std::string in_folder(ui::lua::LuaUi& lua, const char* tag, const std::string& code) {
    lua_State* L = lua.state();
    if (luaL_loadstring(L, code.c_str()) != LUA_OK) return "error: load";
    lua.push_folder_env_for_test(tag);
    if (lua_isnil(L, -1)) {
        lua_settop(L, 0);
        return "error: no such folder";
    }
    lua_setupvalue(L, -2, 1);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_settop(L, 0);
        return "error: " + e;
    }
    std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}

ServerProfile profile(std::string id, std::string name, std::string ui = {}) {
    ServerProfile p;
    p.id = std::move(id);
    p.name = std::move(name);
    p.host = "127.0.0.1";
    p.port = 5998;
    p.ui = std::move(ui);
    return p;
}

// Two saved servers, the first with two accounts that have characters.
ui::LoginView login_view(const std::filesystem::path& servers) {
    ui::LoginView v;
    v.servers_dir = servers;
    v.default_ui = "modern";
    v.ui_packs = {{"modern", "Modern", "Mouse", "mouse", false, {}}, {"gamepad", "Gamepad", "Pad", "gamepad", false, {}}};
    ServerProfile a = profile("alpha", "Alpha", "gamepad");
    a.username = "first";
    SavedAccount first{"first", "Bob", "World One", {{"Bob", "World One", 12, "WAR"}, {"Ann", "World Two", 3, "CLR"}}};
    SavedAccount second{"second", "", "", {{"Cid", "World One", 50, "SHD/CLR"}}};
    a.accounts = {first, second};
    v.profiles = {a, profile("beta", "Beta")};
    ui::select_profile(v, 0);
    return v;
}

} // namespace

TEST_CASE("hs.LoginScreen: nil off the login screen; the view's servers, fields, accounts and characters") {
    test::ScratchDir dir("lua_ui_pregame_test");
    Ui ui;
    CHECK(ui("return hs.LoginScreen()") == "nil");
    ui.api.begin_frame();
    CHECK_FALSE(ui.api.replaces("LoginScreen"));

    ui::LoginView v = login_view(dir.path());
    ui::UiPack unavailable;
    unavailable.id = "addon.example.native";
    unavailable.name = "Unavailable Native";
    unavailable.available = false;
    unavailable.unavailable_reason = "No game asset folder is selected";
    v.ui_packs.push_back(unavailable);
    ui.host.login = &v;
    CHECK(ui("local v = hs.LoginScreen() return #v.servers .. ' ' .. v.selected .. ' ' .. v.servers[1].name .. ' ' .. "
             "v.servers[1].ui .. ' ' .. v.servers[2].ui") == "2 1 Alpha gamepad modern");
    CHECK(ui.api.replaces("LoginScreen")); // read this frame: the pack draws it
    CHECK(ui("local v = hs.LoginScreen() return v.username .. ' ' .. v.host .. ':' .. v.port .. ' ' .. v.ui") ==
          "first 127.0.0.1:5998 gamepad");
    CHECK(ui("local v = hs.LoginScreen() return #v.accounts .. ' ' .. v.accounts[1].username .. ' ' .. "
             "#v.accounts[1].characters .. ' ' .. v.accounts[1].characters[2].world") == "2 first 2 World Two");
    // The Characters list: every account's, the one entered last on the account last used picked.
    CHECK(ui("local v = hs.LoginScreen() return #v.characters .. ' ' .. v.quick .. ' ' .. v.characters[v.quick].name .. "
             "' ' .. tostring(v.characters[v.quick].last) .. ' ' .. v.characters[3].account") == "3 1 Bob true second");
    // No keyring: no saved passwords, so no box set.
    CHECK(ui("local v = hs.LoginScreen() return tostring(v.canSavePasswords) .. ' ' .. v.boxCount .. ' ' .. "
             "tostring(v.ready)") == "false 0 false");
    CHECK(ui("return #hs.LoginScreen().uiPacks") == "3");
    CHECK(ui("local p=hs.UI.pregame.LoginScreen().uiPacks "
             "return p[1].available and not p[3].available and p[3].reason=='No game asset folder is selected'") == "true");

    // Not drawn for a few frames: the client's screen comes back.
    for (int i = 0; i < 3; ++i) ui.api.begin_frame();
    CHECK_FALSE(ui.api.replaces("LoginScreen"));
}

// An interface Hailstone does not ship (an add-on's, or one put in ui/ by hand: UiHost::trusted_ui false) never sees
// the login screen: not the typed or saved password, nor the add-on manager's installs and approvals. The client
// draws that screen with the built-in pack instead (ClientApp::ui_pack_for_screen).
TEST_CASE("hs.LoginScreen, LoginSet and LoginDo: nothing for an interface Hailstone does not ship") {
    test::ScratchDir dir("lua_ui_pregame_untrusted");
    Ui ui;
    ui::LoginView v = login_view(dir.path());
    v.password = "secret";
    ui.host.login = &v;
    ui.host.trusted_ui = false;
    CHECK(ui("return hs.LoginScreen()") == "nil");
    CHECK_FALSE(ui.api.replaces("LoginScreen"));
    ui("hs.LoginSet('password', 'other') hs.LoginSet('addonLink', 'https://github.com/x/y')");
    CHECK(v.password == "secret");
    CHECK(v.addons.link.empty());
    CHECK(ui("return hs.LoginDo('addonInspect')") == "false");
    CHECK(ui("return hs.LoginDo('login')") == "false");
    ui.host.trusted_ui = true;
    CHECK(ui("return hs.LoginScreen().password") == "secret");
}

TEST_CASE("hs.LoginSet and hs.LoginDo: the fields change, and the buttons ask the client for their actions") {
    test::ScratchDir dir("lua_ui_pregame_test");
    Ui ui;
    ui::LoginView v = login_view(dir.path());
    ui.host.login = &v;
    using A = ui::LoginAction;

    CHECK(ui("return hs.LoginDo('login')") == "false"); // no password yet
    CHECK_FALSE(ui.host.login_action);
    ui("hs.LoginSet('password', 'secret')");
    CHECK(v.password == "secret");
    CHECK(ui("return hs.LoginScreen().ready") == "true");
    CHECK(ui("return hs.LoginDo('login')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::Submit));
    ui.host.login_action.reset();

    ui("hs.LoginSet('host', ' 10.0.0.1 ') hs.LoginSet('port', 70000) hs.LoginSet('name', 'Gamma')");
    CHECK(v.host == "10.0.0.1"); // no blanks, as the client's field
    CHECK(v.port == 65535);
    CHECK(v.name == "Gamma");
    ui("hs.LoginSet('ui', 'modern') hs.LoginSet('ui', 'nonesuch')");
    CHECK(v.ui == "modern"); // only a pack the client has
    ui("hs.LoginSet('music', false)");
    CHECK_FALSE(v.pregame_music);
    CHECK(ui.host.login_action == static_cast<int>(A::MusicChanged));
    ui("hs.LoginSet('savePassword', true)");
    CHECK_FALSE(v.save_password); // no keyring
    CHECK(ui("return (pcall(hs.LoginSet, 'nonesuch', 1))") == "false");

    // Servers: another picked, a new one, back.
    ui("hs.LoginDo('select', 2)");
    CHECK(v.selected == 1);
    CHECK(v.ui == "modern"); // the server without its own: the default
    ui("hs.LoginDo('new')");
    CHECK(v.selected == -1);
    CHECK(ui("return hs.LoginScreen().selected") == "0");
    CHECK(ui("return hs.LoginDo('delete')") == "false"); // nothing picked to delete
    ui("hs.LoginDo('select', 1)");
    CHECK(ui("return hs.LoginDo('delete')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::Delete));

    // Accounts: picked into the fields; the box set's world and character.
    ui("hs.LoginDo('account', 'second')");
    CHECK(v.username == "second");
    CHECK(ui("return hs.LoginDo('world', 'first', 'World Two')") == "true");
    CHECK(v.accounts[0].world == "World Two");
    CHECK(v.accounts[0].character == "Ann"); // its character there
    CHECK(ui.host.login_action == static_cast<int>(A::AccountsChanged));
    ui("hs.LoginDo('character', 'first', 'Bob')");
    CHECK(v.accounts[0].character == "Bob");
    CHECK(v.accounts[0].world == "World One");
    CHECK(ui("return hs.LoginDo('box', 'first', true)") == "false"); // no saved password: not in a box set
    CHECK(ui("return hs.LoginDo('forget', 'nobody')") == "false");
    CHECK(ui("return hs.LoginDo('forget', 'second')") == "true");
    CHECK(v.account_choice == "second");
    CHECK(ui.host.login_action == static_cast<int>(A::ForgetAccount));

    // The Characters list's row played.
    CHECK(ui("return hs.LoginDo('play', 3)") == "true");
    CHECK(v.quick_selected == 2);
    CHECK(ui.host.login_action == static_cast<int>(A::PlayCharacter));
    CHECK(ui("return hs.LoginDo('play', 9)") == "false");

    // The game files: the one in use is not picked again.
    v.game_folders = {{"/games/rof2", false, true}, {"/games/live", true, true}, {"/games/gone", false, false}};
    CHECK(ui("return hs.LoginScreen().gameFolders[2].label") == "Live: live");
    CHECK(ui("return hs.LoginDo('gameFolder', 1)") == "false");
    CHECK(ui("return hs.LoginDo('gameFolder', 3)") == "false"); // not usable
    CHECK(ui("return hs.LoginDo('gameFolder', 2)") == "true");
    CHECK(v.game_choice == "/games/live");
    CHECK(ui.host.login_action == static_cast<int>(A::UseGameFolder));

    // While a login is under way only Quit goes.
    v.busy = true;
    ui.host.login_action.reset();
    CHECK(ui("return hs.LoginDo('save')") == "false");
    ui("hs.LoginSet('username', 'x')");
    CHECK(v.username == "second");
    CHECK(ui("return hs.LoginDo('quit')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::Quit));
}

TEST_CASE("Launcher ticket view: after a failed ticket the saved server's remember survives a password login, "
          "and select works") {
    test::ScratchDir dir("lua_ui_pregame_test");
    Ui ui;
    ui::LoginView v = login_view(dir.path());
    v.profiles[1].remember = false; // Beta keeps no username: that must survive too
    ui.host.login = &v;
    REQUIRE(v.remember);

    // The ticket went to Alpha's address: its view keeps nothing while the ticket is in use.
    ui::begin_launcher_login_view(v, v.profiles[0]);
    CHECK_FALSE(v.remember);
    CHECK(v.username.empty());
    // The ticket failed: back at the login screen (ClientApp::back_to_login) the saved server is as saved.
    ui::end_launcher_login_view(v, "alpha", nullptr);
    CHECK_FALSE(v.transient);
    CHECK(v.selected == 0);
    CHECK(v.remember);
    CHECK(v.username == "first"); // its remembered account, as after any return to the login screen
    // A password login now saves Alpha with its own remember, and the account stays in its list.
    ui("hs.LoginSet('password', 'secret')");
    const ServerProfile saved = ui::profile_from_view(v);
    CHECK(saved.id == "alpha");
    CHECK(saved.remember);
    CHECK(saved.username == "first");
    CHECK(saved.accounts.size() == 2);
    CHECK(ui("return hs.LoginDo('select', 2)") == "true");
    CHECK(v.selected == 1);
    CHECK_FALSE(v.remember); // Beta's own

    // A ticket to an address no saved server has: a transient view, whose server list is locked meanwhile.
    ServerProfile elsewhere;
    elsewhere.name = elsewhere.host = "ticket.example";
    elsewhere.port = 5999;
    ui::begin_launcher_login_view(v, elsewhere);
    CHECK(v.transient);
    CHECK(ui("return hs.LoginDo('select', 1)") == "false");
    // Back with no session to resume: the server picked before the ticket, and select works again.
    ui::end_launcher_login_view(v, "beta", nullptr);
    CHECK_FALSE(v.transient);
    CHECK(v.selected == 1);
    CHECK_FALSE(v.remember);
    CHECK(ui("return hs.LoginDo('select', 1)") == "true");
    CHECK(v.selected == 0);
    CHECK(v.remember);

    // Back with a session for that address: it stays in the fields as a new server (Sign in with session finds
    // it), not transient, so the list works and nothing is saved until a login.
    ui::begin_launcher_login_view(v, elsewhere);
    ui::end_launcher_login_view(v, "", &elsewhere);
    CHECK_FALSE(v.transient);
    CHECK(v.selected == -1);
    CHECK(v.host == "ticket.example");
    CHECK(v.port == 5999);
    CHECK(v.remember); // a new server's default
    CHECK(ui("return hs.LoginDo('select', 2)") == "true");
    CHECK(v.selected == 1);
}

TEST_CASE("hs.LoginScreen: a session ticket offers an explicit sign-in without exposing its secret") {
    Ui ui;
    ui::LoginView v;
    v.host = "127.0.0.1";
    ui.host.login = &v;
    CHECK(ui("held = hs.LoginScreen() return tostring(held.sessionTicketAvailable)") == "false");
    CHECK(ui("return hs.LoginDo('resume_token')") == "false");
    CHECK_FALSE(ui.host.login_action);

    v.session_ticket_available = true;
    CHECK(ui("held = hs.LoginScreen(held.revision) return tostring(held.sessionTicketAvailable)") == "true");
    CHECK(ui("return hs.LoginScreen(held.revision)") == "true");
    CHECK(ui("return tostring(held.ready) .. ' ' .. tostring(held.token) .. ' ' .. tostring(held.sessionTicket)") ==
          "false nil nil");
    CHECK(ui("return hs.LoginDo('resume_token')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(ui::LoginAction::ResumeToken));
    ui.host.login_action.reset();

    v.busy = true;
    CHECK(ui("return hs.LoginDo('resume_token')") == "false");
    CHECK_FALSE(ui.host.login_action);
    v.busy = false;
    v.session_ticket_available = false;
    CHECK(ui("held = hs.LoginScreen(held.revision) return tostring(held.sessionTicketAvailable)") == "false");
    CHECK(ui("return hs.LoginDo('resume_token')") == "false");
    CHECK_FALSE(ui.host.login_action);
}

TEST_CASE("hs.LoginSet ui: a saved server keeps the interface at once, and only it; a new server keeps it once saved") {
    test::ScratchDir dir("lua_ui_pregame_test");
    Ui ui;
    ui::LoginView v = login_view(dir.path());
    REQUIRE(save_profile(v.profiles[0], dir.path()));
    ui.host.login = &v;

    ui("hs.LoginSet('name', 'Renamed') hs.LoginSet('ui', 'modern')");
    CHECK(v.ui == "modern");
    auto saved = load_profile("alpha", dir.path());
    REQUIRE(saved);
    CHECK(saved->ui == "modern");
    CHECK(saved->name == "Alpha"); // the other fields wait for Save or Log in
    CHECK(ui("return hs.LoginScreen().servers[1].ui") == "modern");
    // picking another server and back shows the kept interface
    ui::select_profile(v, 1);
    ui::select_profile(v, 0);
    CHECK(v.ui == "modern");

    // a new server: the view's interface only (Save keeps it with the rest)
    ui::new_profile(v);
    ui("hs.LoginSet('ui', 'gamepad')");
    CHECK(v.ui == "gamepad");
    CHECK(load_profile("alpha", dir.path())->ui == "modern");
}

TEST_CASE("hs.LoginScreen: a test login's server has no saved servers to pick, save or delete") {
    test::ScratchDir dir("lua_ui_pregame_test");
    Ui ui;
    ui::LoginView v;
    v.servers_dir = dir.path();
    v.transient = true;
    ui.host.login = &v;
    CHECK(ui("local v = hs.LoginScreen() return tostring(v.transient) .. ' ' .. #v.servers .. ' ' .. #v.characters") ==
          "true 0 0");
    CHECK(ui("return hs.LoginDo('save')") == "false");
    CHECK(ui("return hs.LoginDo('new')") == "false");
}

TEST_CASE("hs.ServerSelect: the worlds, the one played last, and Play only for a world that can be joined") {
    Ui ui;
    CHECK(ui("return hs.ServerSelect()") == "nil");
    ui::ServerSelectView v;
    v.servers = {{"Open World", "Up", true}, {"Locked World", "Locked", true}, {"Down World", "Down", false}};
    ui.host.server_select = &v;
    ui.host.last_server = "Locked World";
    ui.host.login_server = "Alpha";
    CHECK(ui("local v = hs.ServerSelect() return v.server .. ' ' .. #v.servers .. ' ' .. v.selected .. ' ' .. v.last .. "
             "' ' .. tostring(v.servers[1].players) .. ' ' .. tostring(v.servers[2].canJoin)") ==
          "Alpha 3 0 2 nil true"); // no player count reaches any UI
    CHECK(ui.api.replaces("serverselect"));
    CHECK(ui("return hs.ServerSelectDo('play')") == "false"); // nothing picked
    CHECK(ui("return hs.ServerSelectDo('play', 3)") == "false");
    CHECK(v.selected == 2); // picked, but the host marked it unjoinable
    CHECK_FALSE(ui.host.server_select_action);
    CHECK(ui("return hs.ServerSelectDo('play', 2)") == "true");
    CHECK(v.selected == 1);
    CHECK(ui.host.server_select_action == static_cast<int>(ui::ServerSelectAction::Play));
    CHECK(ui("return hs.ServerSelectDo('play', 1)") == "true");
    CHECK(v.selected == 0);
    CHECK(ui.host.server_select_action == static_cast<int>(ui::ServerSelectAction::Play));
    CHECK(ui("return hs.ServerSelectDo('back')") == "true");
    CHECK(ui.host.server_select_action == static_cast<int>(ui::ServerSelectAction::Back));
}

TEST_CASE("Server select order: the favourite worlds first, then the others, each by name ignoring case") {
    const std::vector<std::string> names = {"zeta", "Beta", "alpha", "Gamma", "delta", "Alpha"};
    std::vector<std::string> sorted;
    for (std::size_t i : ui::server_list_order(names, {})) sorted.push_back(names[i]);
    CHECK(sorted == std::vector<std::string>{"Alpha", "alpha", "Beta", "delta", "Gamma", "zeta"});
    sorted.clear();
    for (std::size_t i : ui::server_list_order(names, {"zeta", "Gamma", "Not Listed"})) sorted.push_back(names[i]);
    CHECK(sorted == std::vector<std::string>{"Gamma", "zeta", "Alpha", "alpha", "Beta", "delta"});
    CHECK(ui::server_list_order({}, {"x"}).empty());

    CHECK(ui::parse_favorite_worlds("").empty());
    CHECK(ui::parse_favorite_worlds("A World|B||A World|C") == std::vector<std::string>{"A World", "B", "C"});
    CHECK(ui::join_favorite_worlds({"A World", "B|C", "", "D"}) == "A World|D"); // a name with the separator is not kept
    CHECK(ui::parse_favorite_worlds(ui::join_favorite_worlds({"One", "Two"})) == std::vector<std::string>{"One", "Two"});
}

TEST_CASE("hs.ServerSelect: each world's favourite mark, and ServerSelectDo favorite asks the client to turn it over") {
    Ui ui;
    ui::ServerSelectView v;
    v.servers = {{"Fav World", "Up", true, true}, {"Other World", "Up", true, false}};
    ui.host.server_select = &v;
    CHECK(ui("local v = hs.ServerSelect() return tostring(v.servers[1].favorite) .. ' ' .. tostring(v.servers[2].favorite)") ==
          "true false");
    CHECK(ui("return hs.ServerSelectDo('favorite', 3)") == "false"); // no such row
    CHECK_FALSE(ui.host.server_select_action);
    CHECK(ui("return hs.ServerSelectDo('favorite', 2)") == "true");
    CHECK(v.favorite_row == 1);
    CHECK(v.selected == -1); // the selection is not changed
    CHECK(ui.host.server_select_action == static_cast<int>(ui::ServerSelectAction::Favorite));
}

// The client's own question (app::draw_client_confirm), asked for a permanent action by code other than a shipped
// pack's own: drawn whatever the interface draws, and its handler runs only on the player's click of Yes (No and
// the close box answer no).
TEST_CASE("draw_client_confirm: the client's own question acts only on its Yes") {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigInputTrickleEventQueue = false;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        app::UiHost host;
        host.trusted_ui = false;
        std::vector<bool> answers;
        auto ask = [&] {
            app::UiHost::Confirm c;
            c.text = "Delete Cid permanently? This cannot be undone.";
            c.done = [&answers](bool yes) { answers.push_back(yes); };
            app::ask_client_confirm(host, std::move(c));
        };
        auto frame = [&](std::optional<ImVec2> mouse = std::nullopt, bool down = false) {
            auto& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(800, 600);
            io.DeltaTime = 1.0f / 60.0f;
            if (mouse) io.AddMousePosEvent(mouse->x, mouse->y);
            io.AddMouseButtonEvent(0, down);
            ImGui::NewFrame();
            app::draw_client_confirm(host);
            ImGui::Render();
        };
        // Where the question's button `n` (0 Yes, 1 No) sits: the last line of the auto-sized window, 6 em each.
        auto button = [&](int n) {
            const ImGuiWindow* w = ImGui::FindWindowByName("Confirm###hailstone_client_confirm");
            REQUIRE(w);
            const ImGuiStyle& st = ImGui::GetStyle();
            const float em = ImGui::GetFontSize();
            return ImVec2(w->Pos.x + st.WindowPadding.x + em * 3.0f + static_cast<float>(n) * (em * 6.0f + st.ItemSpacing.x),
                          w->Pos.y + w->Size.y - st.WindowPadding.y - ImGui::GetFrameHeight() * 0.5f);
        };
        auto click = [&](ImVec2 at) {
            frame(at, false);
            frame(at, true);
            frame(at, false);
        };

        frame(); // nothing asked: no window
        const ImGuiWindow* none = ImGui::FindWindowByName("Confirm###hailstone_client_confirm");
        CHECK((none == nullptr || !none->Active));
        ask();
        REQUIRE(host.client_confirm);
        CHECK_FALSE(host.confirm); // not the interface's ConfirmationDialogBox
        frame();
        frame();
        const ImGuiWindow* w = ImGui::FindWindowByName("Confirm###hailstone_client_confirm");
        REQUIRE(w);
        CHECK(w->Active);
        CHECK(answers.empty()); // shown frame after frame, no answer of its own
        click(button(1));
        REQUIRE(answers.size() == 1);
        CHECK_FALSE(answers[0]);
        CHECK_FALSE(host.client_confirm);
        ask();
        frame();
        frame();
        click(button(0));
        REQUIRE(answers.size() == 2);
        CHECK(answers[1]);
        CHECK_FALSE(host.client_confirm);

        // Under a pack Hailstone ships too (a script's request): the client's own window, not the pack's box.
        host.trusted_ui = true;
        ask();
        CHECK_FALSE(host.confirm);
        REQUIRE(host.client_confirm);
        CHECK(app::confirm_open(host));
        frame();
        frame();
        CHECK(answers.size() == 2);
        click(button(0));
        REQUIRE(answers.size() == 3);
        CHECK(answers[2]);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("hs.CharacterSelect: the characters with their race and classes, and its buttons") {
    Ui ui;
    CHECK(ui("return hs.CharacterSelect()") == "nil");
    ui::CharacterSelectView v;
    v.server_name = "World One";
    v.characters = {{"Bob", 12, 1, {1}, "North Qeynos"}, {"Cid", 50, 2, {5, 2}, "Kaladim"}};
    v.scene = true;
    ui.host.char_select = &v;
    CHECK(ui("local v = hs.CharacterSelect() return v.world .. ' ' .. #v.characters .. ' ' .. v.selected .. ' ' .. "
             "tostring(v.scene)") == "World One 2 0 true");
    CHECK(ui.api.replaces("CharacterListWnd"));
    CHECK(ui("local c = hs.CharacterSelect().characters[2] return c.raceName .. '|' .. c.classShort .. '|' .. "
             "c.className .. '|' .. #c.classes .. '|' .. c.zone") == "Barbarian|SHD/CLR|Shadow Knight / Cleric|2|Kaladim");
    using A = ui::CharacterSelectAction;
    CHECK(ui("return hs.CharacterSelectDo('delete')") == "false"); // none picked
    CHECK(ui("return hs.CharacterSelectDo('enter')") == "false");
    CHECK(ui("return hs.CharacterSelectDo('select', 2)") == "true");
    CHECK(v.selected == 1);
    CHECK_FALSE(ui.host.char_select_action); // the scene follows the pick; nothing to ask
    CHECK(ui("return hs.CharacterSelectDo('enter')") == "true");
    CHECK(ui.host.char_select_action == static_cast<int>(A::EnterWorld));
    // Delete from code other than a shipped pack's own (a script, under the shipped pack): the client asks in its own
    // window (UiHost::client_confirm), naming the character, never in the pack's ConfirmationDialogBox, whose Yes a
    // script could press; only the player's Yes deletes
    CHECK(ui("return hs.CharacterSelectDo('delete')") == "true");
    CHECK(ui.host.char_select_action == static_cast<int>(A::EnterWorld));
    CHECK_FALSE(ui.host.confirm);
    REQUIRE(ui.host.client_confirm);
    CHECK(ui.host.client_confirm->text == "Delete Cid permanently? This cannot be undone.");
    CHECK(ui("return hs.CharacterSelectDo('delete')") == "false"); // the question is already up
    CHECK(ui("return hs.WindowButton('ConfirmationDialogBox', 'Yes_Button')") == "false"); // not the box's question
    app::answer_client_confirm(ui.host, false);
    CHECK(ui.host.char_select_action == static_cast<int>(A::EnterWorld));
    ui("hs.CharacterSelectDo('delete')");
    REQUIRE(ui.host.client_confirm);
    CHECK(ui("return hs.CharacterSelectDo('select', 1)") == "true"); // the pick moved before the answer
    app::answer_client_confirm(ui.host, true);
    CHECK(ui.host.char_select_action == static_cast<int>(A::EnterWorld));
    ui("hs.CharacterSelectDo('select', 2) hs.CharacterSelectDo('delete')");
    REQUIRE(ui.host.client_confirm);
    app::answer_client_confirm(ui.host, true);
    CHECK(ui.host.char_select_action == static_cast<int>(A::Delete));
    // A shipped pack's own code asks in its own look first (ui/*/charselect.lua): its call deletes at once
    ui.host.char_select_action.reset();
    lua_pushboolean(ui.L, 1);
    lua_setfield(ui.L, LUA_REGISTRYINDEX, ui::lua::kPackStateKey);
    ui.host.trusted_ui = false; // ... but not an interface Hailstone does not ship
    // whose question is the client's own window (UiHost::client_confirm), not the interface's ConfirmationDialogBox:
    // that interface may never draw one, and its code could press that box's Yes itself
    CHECK(ui("return hs.CharacterSelectDo('delete')") == "true");
    CHECK_FALSE(ui.host.char_select_action);
    CHECK_FALSE(ui.host.confirm);
    REQUIRE(ui.host.client_confirm);
    CHECK(ui.host.client_confirm->text == "Delete Cid permanently? This cannot be undone.");
    CHECK(ui("return hs.CharacterSelectDo('delete')") == "false"); // the question is already up
    app::answer_client_confirm(ui.host, false);
    CHECK_FALSE(ui.host.client_confirm);
    CHECK_FALSE(ui.host.char_select_action);
    ui("hs.CharacterSelectDo('delete')");
    REQUIRE(ui.host.client_confirm);
    app::answer_client_confirm(ui.host, true);
    CHECK_FALSE(ui.host.client_confirm);
    CHECK(ui.host.char_select_action == static_cast<int>(A::Delete));
    ui.host.char_select_action.reset();
    app::answer_client_confirm(ui.host, true); // nothing is up: nothing happens
    CHECK_FALSE(ui.host.char_select_action);
    ui.host.trusted_ui = true;
    CHECK(ui("return hs.CharacterSelectDo('delete')") == "true");
    CHECK_FALSE(ui.host.confirm);
    CHECK(ui.host.char_select_action == static_cast<int>(A::Delete));
    ui("hs.CharacterSelectDo('create')");
    CHECK(ui.host.char_select_action == static_cast<int>(A::Create));
    // Return Home: only for a character the list lets go home
    ui.host.char_select_action.reset();
    CHECK(ui("local c = hs.CharacterSelect().characters[2] return tostring(c.canGoHome)") == "false");
    CHECK(ui("return hs.CharacterSelectDo('home')") == "false");
    CHECK_FALSE(ui.host.char_select_action);
    v.characters[1].can_go_home = true;
    v.characters[1].home_question = "Home, Cid?";
    CHECK(ui("local c = hs.CharacterSelect().characters[2] return tostring(c.canGoHome) .. ' ' .. c.homeQuestion") ==
          "true Home, Cid?");
    CHECK(ui("return hs.CharacterSelectDo('home')") == "true");
    CHECK(ui.host.char_select_action == static_cast<int>(A::ReturnHome));
    v.busy = true; // waiting on the world: only the way back
    ui.host.char_select_action.reset();
    CHECK(ui("return hs.CharacterSelectDo('enter', 1)") == "false");
    CHECK(ui("return hs.CharacterSelectDo('home')") == "false");
    CHECK(ui("return hs.CharacterSelectDo('logout')") == "true");
    CHECK(ui.host.char_select_action == static_cast<int>(A::Logout));
}

TEST_CASE("hs.CharacterCreate: the choices, the stats and the buttons of the client's creation screen") {
    Ui ui;
    CHECK(ui("return hs.CharacterCreate()") == "nil");
    ui::CharacterCreateView v;
    auto opt = [](std::uint32_t race, std::uint32_t cls, std::uint32_t deity, std::uint32_t zone) {
        ui::CreateOption o{race, cls, deity, zone, {}, {}};
        o.base = {70, 70, 70, 70, 70, 70, 70};
        o.points = {5, 0, 0, 5, 0, 0, 0};
        return o;
    };
    // Human Warrior or Cleric, Barbarian Warrior
    v.options = {opt(1, 1, 201, 2), opt(1, 1, 202, 2), opt(1, 2, 201, 2), opt(2, 1, 201, 2)};
    v.deity_name = [](std::uint32_t d) { return d == 201 ? std::string("Zeta") : std::string("Alpha"); };
    v.scene = true;
    ui.host.char_create_view = &v;
    CHECK(ui("local v = hs.CharacterCreate() return v.raceName .. '|' .. v.className .. '|' .. #v.races .. '|' .. "
             "#v.classes .. '|' .. v.pointsLeft .. '|' .. tostring(v.ready) .. '|' .. tostring(v.scene)") ==
          "Human|Warrior|2|2|10|false|true");
    CHECK(ui.api.replaces("CharacterCreateScreen"));
    // the deities by name; the stats with their base, cap and the class's
    CHECK(ui("local v = hs.CharacterCreate() return v.deities[1].name .. ',' .. v.deities[2].name .. '|' .. v.deityName") ==
          "Alpha,Zeta|Zeta");
    CHECK(ui("local s = hs.CharacterCreate().stats return #s .. s[1].short .. s[1].value .. '/' .. s[1].cap .. "
             "tostring(s[1].favored) .. tostring(s[2].favored)") == "7STR70/95truefalse");
    using A = ui::CharacterCreateAction;
    CHECK(ui("return hs.CharacterCreateDo('race', 99)") == "false"); // not offered
    CHECK(ui("return hs.CharacterCreateDo('class', 2)") == "true");
    CHECK(v.class_id == 2);
    CHECK(ui("return hs.CharacterCreateDo('race', 2)") == "true"); // Barbarians are Warriors alone here
    CHECK(ui("local v = hs.CharacterCreate() return v.race .. ' ' .. v.class .. ' ' .. #v.classes") == "2 1 1");
    CHECK(ui("return hs.CharacterCreateDo('race', 1)") == "true");
    CHECK(ui("return hs.CharacterCreateDo('deity', 202)") == "true");
    CHECK(ui("return hs.CharacterCreate().deityIndex") == "1");
    CHECK(ui("return hs.CharacterCreateDo('statMax', 1)") == "true");
    CHECK(ui("return hs.CharacterCreateDo('statUp', 1)") == "false"); // spent
    CHECK(ui("return hs.CharacterCreateDo('statDown', 1)") == "true");
    CHECK(ui("return hs.CharacterCreateDo('statUp', 8)") == "false");
    CHECK(ui("hs.CharacterCreateDo('statUp', 4) return hs.CharacterCreate().pointsLeft") == "0");
    CHECK(ui("return hs.CharacterCreateDo('create')") == "false"); // no name yet
    CHECK(ui("return hs.CharacterCreateDo('name', 'Bobby') and hs.CharacterCreate().ready") == "true");
    CHECK(ui("return hs.CharacterCreateDo('randomName')") == "false"); // no connection to ask
    CHECK_FALSE(ui.host.char_create_action);
    CHECK(ui("return hs.CharacterCreateDo('create')") == "true");
    CHECK(ui.host.char_create_action == static_cast<int>(A::Create));
    CHECK(ui("return hs.CharacterCreateDo('appearance')") == "true");
    CHECK(ui.host.char_create_action == static_cast<int>(A::Appearance));
    CHECK(ui("return hs.CharacterCreateDo('resetStats') and hs.CharacterCreate().pointsLeft") == "10");
    CHECK(ui("local t, title = hs.CharacterCreateHelp('stat', 1) return t .. '|' .. title") == "|Strength");
    CHECK(ui("return select(2, hs.CharacterCreateHelp('race', 2))") == "Barbarian");
    CHECK(ui("return hs.CharacterCreateHelp('nope', 1)").starts_with("error:"));
    v.busy = true; // sent: nothing changes, not even Cancel
    ui.host.char_create_action.reset();
    CHECK(ui("return hs.CharacterCreateDo('class', 2)") == "false");
    CHECK(ui("return hs.CharacterCreateDo('cancel')") == "false");
    v.busy = false;
    CHECK(ui("return hs.CharacterCreateDo('cancel')") == "true");
    CHECK(ui.host.char_create_action == static_cast<int>(A::Cancel));
    ui.host.customization_shown = true; // the Appearance window is up: hidden
    CHECK(ui("return hs.CharacterCreate()") == "nil");
    CHECK(ui.api.replaces("CharacterCreateScreen"));
}

TEST_CASE("hs.LoginScreen and the other screens: a plugin's window gets nothing (the view holds the password)") {
    namespace fs = std::filesystem;
    test::ScratchDir dir("lua_ui_pregame_test");
    const fs::path pack = dir.path() / "pack", plug = dir.path() / "plug" / "ui" / "pack";
    fs::create_directories(pack);
    fs::create_directories(plug);
    const char* probe = "local v = hs.LoginScreen() %s = v and v.password or 'nil' "
                        "%s_do = tostring(hs.LoginDo('quit')) "
                        "%s_chars = tostring(hs.CharacterSelect() ~= nil)\n";
    auto window = [&](const char* name) {
        char code[512];
        std::snprintf(code, sizeof code, probe, name, name, name);
        return std::string("ui.window('") + name + "', function() " + code + " end)\n";
    };
    std::ofstream(pack / "a.lua") << window("pack");
    std::ofstream(plug / "p.lua") << window("plugin");

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        app::UiHost host;
        app::UiApi api(host);
        ui::LoginView v = login_view(dir.path());
        v.password = "secret";
        ui::CharacterSelectView chars;
        host.login = &v;
        host.char_select = &chars;
        ui::lua::LuaUi lua(pack, dir.path() / "ui.txt");
        lua.add_ui_api([&](lua_State* L) { api.register_functions(L); });
        lua.set_folders({{"plug", plug}});
        ImGui::GetIO().DisplaySize = ImVec2(800, 600);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        lua.frame();
        ImGui::Render();
        lua_State* L = lua.state();
        CHECK(run(L, "return pack") == "secret");
        CHECK(run(L, "return pack_chars") == "true");
        CHECK(in_folder(lua, "plug", "return plugin") == "nil");
        CHECK(in_folder(lua, "plug", "return plugin_do") == "false");
        CHECK(in_folder(lua, "plug", "return plugin_chars") == "false");
        CHECK(run(L, "return pack_do") == "true"); // the pack's own Quit
        CHECK(host.login_action == static_cast<int>(ui::LoginAction::Quit));
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("hs.LoginScreen update and hs.LoginDo update*: the updater's notice and its buttons") {
    test::ScratchDir dir("lua_ui_pregame_test");
    Ui ui;
    ui::LoginView v = login_view(dir.path());
    ui.host.login = &v;
    using A = ui::LoginAction;
    CHECK(ui("return hs.LoginScreen().update") == "nil");
    CHECK(ui("return hs.LoginDo('updateDownload')") == "false");
    // The running build, for the screens' small version line; "" without one.
    CHECK(ui("return hs.LoginScreen().version") == "");
    v.version = "0.2.0-alpha (abc1234)";
    CHECK(ui("return hs.LoginScreen().version") == "0.2.0-alpha (abc1234)");

    v.update.state = "available";
    v.update.version = "0.2.0";
    v.update.current = "0.1.0";
    v.update.notes = "https://example.invalid/r";
    v.update.has_package = true;
    CHECK(ui("local u = hs.LoginScreen().update return u.text .. '|' .. tostring(u.canDownload) .. ' ' .. "
             "tostring(u.canRestart) .. ' ' .. tostring(u.canSkip) .. ' ' .. tostring(u.hasNotes)") ==
          "Hailstone 0.2.0 is out (this is 0.1.0).|true false true true");
    CHECK(ui("return hs.LoginDo('updateDownload')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::UpdateDownload));
    CHECK(ui("return hs.LoginDo('updateSkip')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::UpdateSkip));
    CHECK(ui("return hs.LoginDo('updateRestart')") == "false");

    // Ready: Restart only without the box set's windows; it works while the screen is busy too.
    v.update.state = "ready";
    v.busy = true;
    CHECK(ui("return hs.LoginDo('updateRestart')") == "false");
    v.update.can_restart = true;
    CHECK(ui("return hs.LoginScreen().update.text") == "Hailstone 0.2.0 is downloaded: it is installed when you quit.");
    CHECK(ui("return hs.LoginDo('updateRestart')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::UpdateRestart));
    CHECK(ui("return hs.LoginDo('updateNotes')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::UpdateNotes));
    CHECK(ui("return hs.LoginDo('updateSkip')") == "false");

    // No package for this system: only the page.
    v.update = {};
    v.update.state = "available";
    v.update.version = "0.2.0";
    v.update.current = "0.1.0";
    CHECK(ui("local u = hs.LoginScreen().update return u.text .. '|' .. tostring(u.canDownload) .. ' ' .. "
             "tostring(u.hasNotes)") == "Hailstone 0.2.0 is out (this is 0.1.0). Download it from its release page.|false false");
    // A message alone (a rollback at start).
    v.update = {};
    v.update.message = "Put back.";
    CHECK(ui("return hs.LoginScreen().update.text .. ' ' .. hs.LoginScreen().update.state") == "Put back. ");
}

TEST_CASE("hs.LoginScreen about and hs.LoginDo openProject: the corner's build, update word, notes and page") {
    test::ScratchDir dir("lua_ui_pregame_about");
    Ui ui;
    ui::LoginView v = login_view(dir.path());
    ui.host.login = &v;
    using A = ui::LoginAction;
    // Always there, also with nothing to offer (update is nil then).
    v.update.current = "0.4.0-alpha";
    v.version = "0.4.0-alpha (abc1234)";
    v.update.off_reason = "a development build";
    CHECK(ui("local a = hs.LoginScreen().about return a.version .. '|' .. a.build .. '|' .. a.status .. '|' .. a.text "
             ".. '|' .. #a.notes .. '|' .. a.page") ==
          "0.4.0-alpha|0.4.0-alpha (abc1234)|off|Updates are off: a development build|0|");
    CHECK(ui("return hs.LoginDo('openProject')") == "false"); // no page
    v.update.check = "checking";
    CHECK(ui("return hs.LoginScreen().about.text") == "Checking for updates...");
    v.update.check = "uptodate";
    CHECK(ui("return hs.LoginScreen().about.text") == "Up to date");
    v.update.check = "unknown";
    CHECK(ui("return hs.LoginScreen().about.text") == "Could not check for updates");
    v.update.check = "behind";
    v.update.version = "0.3.0";
    v.update.seen = "0.4.0";
    CHECK(ui("local a = hs.LoginScreen().about return a.status .. '|' .. a.text") ==
          "behind|Not up to date: the update feed went back to 0.3.0 (0.4.0 was out)");
    v.update.check = "newer";
    v.update.version = "0.5.0";
    CHECK(ui("return hs.LoginScreen().about.text") == "Version 0.5.0 is out");
    v.display_version = "0.4.0-bastion-preview-01";
    v.version = "0.4.0-bastion-preview-01 (abc1234)";
    v.update.state = "available";
    CHECK(ui("local login = hs.LoginScreen() "
             "return login.about.version .. '|' .. login.about.build .. '|' .. login.update.current") ==
          "0.4.0-bastion-preview-01|0.4.0-bastion-preview-01 (abc1234)|0.4.0-alpha");
    v.update.state = "downloading";
    CHECK(ui("return hs.LoginScreen().about.text") == "Downloading 0.5.0...");
    v.update.state = "ready";
    CHECK(ui("return hs.LoginScreen().about.text") == "0.5.0 is ready: restart to update");

    v.release_notes = {"One", "Two"};
    v.project_page = "https://example.invalid/hailstone";
    CHECK(ui("local a = hs.LoginScreen().about return a.notes[2] .. '|' .. a.page") ==
          "Two|https://example.invalid/hailstone");
    CHECK(ui("return hs.LoginDo('openProject')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::OpenProject));
    // it works while a login is under way too
    v.busy = true;
    CHECK(ui("return hs.LoginDo('openProject')") == "true");
}

TEST_CASE("hs.LoginScreen: the add-on manager's view and its actions (docs/addons.md)") {
    test::ScratchDir dir("lua_pregame_addons");
    Ui ui;
    ui::LoginView v = login_view(dir.path());
    ui.host.login = &v;
    using A = ui::LoginAction;
    ui::AddonRowView installed;
    installed.name = "autoloot";
    installed.kind = "script";
    installed.group = "installed";
    installed.version = "1.0.0";
    installed.page = "https://github.com/owner/autoloot";
    installed.state = "approval";
    installed.caps = {"read the game state"};
    installed.new_caps = {"speak for you"};
    installed.can_update = installed.can_remove = true;
    installed.managed = installed.enabled = true;
    installed.folder = "/x/scripts/autoloot";
    ui::AddonRowView server;
    server.name = "hud";
    server.kind = "script";
    server.group = "server";
    server.folder = "/x/servers/alpha/scripts";
    v.addons.rows = {installed, server};
    v.addons.server = "Alpha";

    CHECK(ui("return hs.LoginScreen().addons.available") == "false");
    CHECK(ui("return #hs.LoginScreen().addons.rows") == "2");
    CHECK(ui("return hs.LoginScreen().addons.rows[1].newCaps[1]") == "speak for you");
    CHECK(ui("return hs.LoginScreen().addons.rows[1].managed") == "true");
    CHECK(ui("return hs.LoginScreen().addons.rows[1].enabled") == "true");
    CHECK(ui("return hs.LoginScreen().addons.rows[2].group") == "server");
    CHECK(ui("return hs.LoginScreen().addons.offer") == "nil");

    // The link and the panel are the client's to keep.
    ui("hs.LoginSet('addonLink', 'https://github.com/owner/autoloot')");
    CHECK(v.addons.link == "https://github.com/owner/autoloot");
    ui("hs.LoginSet('addonsOpen', true)");
    CHECK(v.addons.open);
    // Nothing can be fetched here: no inspecting.
    CHECK(ui("return hs.LoginDo('addonInspect')") == "false");
    v.addons.available = true;
    CHECK(ui("return hs.LoginDo('addonInspect')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonInspect));
    ui.host.login_action.reset();

    // An offer: Install only when nothing blocks it.
    ui::AddonOfferView offer;
    offer.name = "autoloot";
    offer.blocked = "needs Hailstone 9.0.0 or newer";
    offer.caps = {"read the game state"};
    offer.autoload = true;
    v.addons.offer = offer;
    CHECK(ui("return hs.LoginScreen().addons.offer.blocked") == "needs Hailstone 9.0.0 or newer");
    CHECK(ui("return hs.LoginScreen().addons.offer.autoload") == "true");
    CHECK(ui("return hs.LoginDo('addonInstall')") == "false");
    v.addons.offer->blocked.clear();
    CHECK(ui("return hs.LoginDo('addonInstall')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonInstall));
    CHECK_FALSE(v.addons.choice_on);
    // An older version than the installed one: only with "older", the player's yes to going back.
    ui.host.login_action.reset();
    v.addons.offer->older = true;
    CHECK(ui("return hs.LoginScreen().addons.offer.older") == "true");
    CHECK(ui("return hs.LoginDo('addonInstall')") == "false");
    CHECK_FALSE(ui.host.login_action);
    CHECK(ui("return hs.LoginDo('addonInstall', 'older')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonInstall));
    CHECK(v.addons.choice_on);
    v.addons.offer->older = false;

    // A row's actions are for that row; the server's own cannot be removed, updated or switched.
    CHECK(ui("return hs.LoginDo('addonUpdate', 'autoloot')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonUpdate));
    CHECK(v.addons.choice == "autoloot");
    CHECK(ui("return hs.LoginDo('addonEnabled', 'autoloot', false)") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonEnabled));
    CHECK_FALSE(v.addons.choice_on);
    CHECK(ui("return hs.LoginDo('addonEnabled', 'autoloot', true)") == "true");
    CHECK(v.addons.choice_on);
    CHECK(ui("return hs.LoginDo('addonAuto', 'autoloot', false)") == "true");
    CHECK_FALSE(v.addons.choice_on);
    CHECK(ui("return hs.LoginDo('addonRemove', 'autoloot')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonRemove));
    ui.host.login_action.reset();
    CHECK(ui("return hs.LoginDo('addonRemove', 'hud')") == "false");
    CHECK(ui("return hs.LoginDo('addonUpdate', 'hud')") == "false");
    CHECK(ui("return hs.LoginDo('addonAuto', 'hud', true)") == "false");
    CHECK(ui("return hs.LoginDo('addonEnabled', 'hud', true)") == "false");
    CHECK(ui("return hs.LoginDo('addonEnabled', 'nothing', true)") == "false");
    CHECK(ui("return hs.LoginDo('addonEnabled', 'autoloot', 'yes')") == "false");
    CHECK(ui("return hs.LoginDo('addonRemove', 'nothing')") == "false");
    CHECK_FALSE(ui.host.login_action);
    CHECK(ui("return hs.LoginDo('addonFolder', 'hud')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonFolder));
    CHECK(v.addons.choice == "hud");
    CHECK(ui("return hs.LoginDo('addonPage', 'autoloot')") == "true");
    CHECK(ui("return hs.LoginDo('addonCheck')") == "true");
    // Only an installed tool starts.
    ui.host.login_action.reset();
    CHECK(ui("return hs.LoginDo('addonStart', 'autoloot')") == "false");
    CHECK_FALSE(ui.host.login_action);
    ui::AddonRowView tool = installed;
    tool.name = "texstudio";
    tool.kind = "tool";
    tool.can_start = true;
    v.addons.rows.push_back(tool);
    CHECK(ui("return hs.LoginScreen().addons.rows[3].canStart") == "true");
    CHECK(ui("return hs.LoginScreen().addons.rows[1].canStart") == "false");
    CHECK(ui("return hs.LoginDo('addonStart', 'texstudio')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonStart));
    CHECK(v.addons.choice == "texstudio");
    v.addons.rows.pop_back();
    // While a login is under way nothing is removed.
    v.busy = true;
    CHECK(ui("return hs.LoginDo('addonRemove', 'autoloot')") == "false");
    CHECK(ui("return hs.LoginDo('addonEnabled', 'autoloot', false)") == "false");
    v.busy = false;

    // The server's compiled plugin: the player's word on it, taken back when given, asked again when refused.
    ui::AddonRowView plug = server;
    plug.name = "srvplug";
    plug.kind = "plugin";
    plug.consent = "allowed";
    v.addons.rows.push_back(plug);
    CHECK(ui("return hs.LoginScreen().addons.rows[3].consent") == "allowed");
    CHECK(ui("return hs.LoginScreen().addons.rows[2].consent") == "");
    ui.host.login_action.reset();
    CHECK(ui("return hs.LoginDo('addonAskAgain', 'srvplug')") == "false");
    CHECK(ui("return hs.LoginDo('addonRevoke', 'srvplug')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonRevoke));
    CHECK(v.addons.choice == "srvplug");
    ui.host.login_action.reset();
    v.addons.rows.back().consent = "denied";
    CHECK(ui("return hs.LoginDo('addonRevoke', 'srvplug')") == "false");
    CHECK(ui("return hs.LoginDo('addonAskAgain', 'srvplug')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonAskAgain));
    ui.host.login_action.reset();
    // Not for the server's scripts nor the player's add-ons.
    CHECK(ui("return hs.LoginDo('addonRevoke', 'hud')") == "false");
    CHECK(ui("return hs.LoginDo('addonAskAgain', 'autoloot')") == "false");
    CHECK_FALSE(ui.host.login_action);
    // A required installed native plugin also exposes its per-server consent, so a refusal can be reviewed.
    plug.name = "requiredplug";
    plug.group = "installed";
    plug.consent = "denied";
    v.addons.rows.push_back(plug);
    CHECK(ui("return hs.UI.pregame.LoginDo('addonAskAgain', 'requiredplug')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonAskAgain));
    v.addons.rows.back().consent = "allowed";
    CHECK(ui("return hs.UI.pregame.LoginDo('addonRevoke', 'requiredplug')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonRevoke));
    v.addons.rows.pop_back();
    ui.host.login_action.reset();

    // A suggested add-on (docs/addons.md "Suggested add-ons"): by its source; not while a login is under way.
    v.addons.suggestions = {{"servplug", "owner/servplug", "Alpha World has a Hailstone add-on.", false}};
    CHECK(ui("return hs.LoginScreen().addons.suggestions[1].source") == "owner/servplug");
    CHECK(ui("return hs.LoginScreen().addons.suggestions[1].fromServer") == "false");
    CHECK(ui("return hs.LoginScreen().addons.suggestions[1].required") == "false");
    CHECK(ui("return hs.LoginDo('addonSuggestLook', 'owner/servplug')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonSuggestLook));
    CHECK(v.addons.choice == "owner/servplug");
    CHECK(ui("return hs.LoginDo('addonSuggestLater', 'owner/servplug')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonSuggestLater));
    CHECK(ui("return hs.LoginDo('addonSuggestNever', 'owner/servplug')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonSuggestNever));
    ui.host.login_action.reset();
    CHECK(ui("return hs.LoginDo('addonSuggestLook', 'owner/other')") == "false");
    v.busy = true;
    CHECK(ui("return hs.LoginDo('addonSuggestLook', 'owner/servplug')") == "false");
    v.busy = false;
    CHECK_FALSE(ui.host.login_action);
    v.addons.suggestions.front().required = true;
    CHECK(ui("return hs.UI.pregame.LoginScreen().addons.suggestions[1].required") == "true");
    CHECK(ui("return hs.UI.pregame.LoginDo('addonSuggestLater', 'owner/servplug')") == "false");
    CHECK(ui("return hs.UI.pregame.LoginDo('addonSuggestNever', 'owner/servplug')") == "false");
    CHECK_FALSE(ui.host.login_action);
    CHECK(ui("return hs.UI.pregame.LoginDo('addonSuggestLook', 'owner/servplug')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonSuggestLook));
    ui.host.login_action.reset();

    // The store (docs/addons.md "Finding add-ons"): fetched on the pack's word, and Install opens an item's card
    // only for one that can be installed or updated.
    v.addons.available = false;
    CHECK(ui("return hs.LoginDo('addonBrowse')") == "false"); // nothing can be fetched here
    v.addons.available = true;
    CHECK(ui("return hs.LoginDo('addonBrowse')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonBrowse));
    CHECK_FALSE(v.addons.choice_on);
    CHECK(ui("return hs.LoginDo('addonBrowse', true)") == "true");
    CHECK(v.addons.choice_on);
    v.addons.browsing = true;
    ui.host.login_action.reset();
    CHECK(ui("return hs.LoginDo('addonBrowse', true)") == "false");
    CHECK_FALSE(ui.host.login_action);
    v.addons.browsing = false;
    v.addons.browsed = true;
    ui::AddonStoreItemView item;
    item.name = "autoloot";
    item.source = "owner/autoloot";
    item.kind = "script";
    item.version = "1.3.0";
    item.worlds = "Loot World";
    item.state = "install";
    v.addons.store = {item};
    item.name = "hud";
    item.source = "owner/hud";
    item.state = "installed";
    item.installed_version = "1.3.0";
    v.addons.store.push_back(item);
    CHECK(ui("local a = hs.LoginScreen().addons return tostring(a.browsed) .. '|' .. #a.store .. '|' .. "
             "a.store[1].worlds .. '|' .. a.store[2].installedVersion") == "true|2|Loot World|1.3.0");
    v.addons.offer.reset();
    CHECK(ui("return hs.LoginDo('addonStoreLook', 'owner/autoloot')") == "true");
    CHECK(ui.host.login_action == static_cast<int>(A::AddonStoreLook));
    CHECK(v.addons.choice == "owner/autoloot");
    ui.host.login_action.reset();
    CHECK(ui("return hs.LoginDo('addonStoreLook', 'owner/hud')") == "false"); // installed already
    CHECK(ui("return hs.LoginDo('addonStoreLook', 'owner/none')") == "false");
    v.addons.inspecting = true;
    CHECK(ui("return hs.LoginDo('addonStoreLook', 'owner/autoloot')") == "false");
    CHECK_FALSE(ui.host.login_action);
    v.addons.inspecting = false;

    // A plugin's window gets none of it (hs.LoginScreen is nil there): checked with the other pre-game functions.
    CHECK(ui("return (pcall(hs.LoginDo, 'addonWhatever'))") == "false");
}

// A plugin's code (an add-on's ui/ folder) cannot reach the password through the pack: it cannot replace hs.* for the
// pack's windows, its own hs.LoginScreen gives nil wherever it is called from (also when the pack calls a function
// the plugin left in a table they share), and its hs.ChatSubmit runs with the plugin's capabilities.
TEST_CASE("hs API in a plugin's code: no password through the pack, hs.ChatSubmit under its capabilities") {
    namespace fs = std::filesystem;
    test::ScratchDir dir("lua_ui_pregame_confine");
    const fs::path pack = dir.path() / "pack", plug = dir.path() / "plug";
    fs::create_directories(pack);
    fs::create_directories(plug);
    std::ofstream(pack / "login.lua") << "hooks = {}\n"
                                         "ui.window('Login', function()\n"
                                         "  local v = hs.LoginScreen()\n"
                                         "  pack_pw = v and v.password\n"
                                         "  for _, f in ipairs(hooks) do f(v) end\n"
                                         "  hs.ChatSubmit(0, '/pack')\n"
                                         "end)\n";
    std::ofstream(plug / "steal.lua")
        << "replaced = pcall(function() local real = hs.LoginScreen\n"
           "  hs.LoginScreen = function() local v = real() stolen = v and v.password return v end end)\n"
           "at_load = (hs.LoginScreen() or {}).password\n"
           "local submit = hs.ChatSubmit\n"
           "hooks[#hooks + 1] = function(v)\n"            // the pack's table: the pack calls it with its view
           "  via_hook = (hs.LoginScreen() or {}).password\n"
           "  submit(0, '/lua run evil')\n"
           "end\n"
           "ui.window('Plug', function() in_draw = (hs.LoginScreen() or {}).password  hs.ChatSubmit(0, '/plug') end)\n";

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        app::UiHost host;
        app::UiApi api(host);
        ui::LoginView v = login_view(dir.path());
        v.password = "secret";
        host.login = &v;
        struct Line {
            std::string text;
            std::uint32_t caps;
            std::string by;
        };
        std::vector<Line> lines;
        host.submit_chat = [&](const std::string& t) {
            lines.push_back({t, plugin::caller_caps(), std::string(plugin::caller_name())});
        };
        const std::uint32_t plug_caps = plugin::kCapRead | plugin::kCapUi;
        ui::lua::LuaUi lua(pack, dir.path() / "ui.txt");
        lua.add_ui_api([&](lua_State* L) { api.register_functions(L); });
        lua.set_caller_scope([&](std::string_view tag) -> std::shared_ptr<void> {
            return std::make_shared<plugin::CallerScope>(tag == "plug" ? plug_caps : 0u, tag);
        });
        lua.set_folders({{"plug", plug}});
        ImGui::GetIO().DisplaySize = ImVec2(800, 600);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        lua.frame();
        ImGui::Render();
        lua_State* L = lua.state();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK(run(L, "return pack_pw") == "secret"); // the pack's own window still has it
        CHECK(in_folder(lua, "plug", "return replaced") == "false");
        CHECK(in_folder(lua, "plug", "return stolen") == "nil");
        CHECK(in_folder(lua, "plug", "return at_load") == "nil");
        CHECK(in_folder(lua, "plug", "return in_draw") == "nil");
        CHECK(in_folder(lua, "plug", "return via_hook") == "nil");
        CHECK(run(L, "return stolen") == "nil");
        // The lines: each with its sender's capabilities.
        REQUIRE(lines.size() == 3);
        CHECK(lines[0].text == "/lua run evil"); // the plugin's, called by the pack's window
        CHECK(lines[0].caps == plug_caps);
        CHECK(lines[0].by == "plug");
        CHECK(lines[1].text == "/pack");
        CHECK(lines[1].caps == plugin::kCapAll);
        CHECK(lines[1].by.empty());
        CHECK(lines[2].text == "/plug");
        CHECK(lines[2].caps == plug_caps);
        CHECK(plugin::caller_caps() == plugin::kCapAll); // every scope ended
        // Scripts: the player's to run, stop and set to autorun, not a plugin's.
        CHECK(in_folder(lua, "plug", "return select(2, hs.ScriptRun('x'))") == "scripts are the player's to run");
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("hs.LoginScreen(revision) and the other screens: true while nothing changed, a new view when anything did") {
    test::ScratchDir dir("lua_ui_pregame_test");
    Ui ui;
    ui::LoginView v = login_view(dir.path());
    ui.host.login = &v;
    ui.api.begin_frame();
    CHECK(ui("held = hs.LoginScreen() return math.type(held.revision)") == "integer");
    // Unchanged: true, the script keeps its table; the screen is still the pack's this frame.
    for (int i = 0; i < 3; ++i) ui.api.begin_frame();
    CHECK_FALSE(ui.api.replaces("LoginScreen"));
    CHECK(ui("return hs.LoginScreen(held.revision)") == "true");
    CHECK(ui.api.replaces("LoginScreen"));
    // Without the argument (or with another revision) the whole view, as before.
    CHECK(ui("return hs.LoginScreen().username") == "first");
    CHECK(ui("return hs.LoginScreen(held.revision + 1).username") == "first");
    CHECK(ui("return hs.LoginScreen().revision == held.revision") == "true");
    // Any field the client changes: a field, a nested list, the updater's progress, an add-on row.
    v.username = "other";
    CHECK(ui("local n = hs.LoginScreen(held.revision) local r = n ~= true and n.username held = n return r") == "other");
    CHECK(ui("return hs.LoginScreen(held.revision)") == "true");
    v.update.state = "downloading";
    v.update.version = "0.2.0";
    v.update.progress = 0.25f;
    CHECK(ui("held = hs.LoginScreen(held.revision) return type(held)") == "table");
    v.update.progress = 0.5f;
    CHECK(ui("held = hs.LoginScreen(held.revision) return held.update.progress") == "0.5");
    v.accounts[0].characters[1].level = 4;
    CHECK(ui("held = hs.LoginScreen(held.revision) return held.accounts[1].characters[2].level") == "4");
    v.addons.rows.push_back({});
    v.addons.rows.back().name = "one";
    CHECK(ui("held = hs.LoginScreen(held.revision) return #held.addons.rows") == "1");
    v.addons.rows.back().caps = {"speak for you"};
    CHECK(ui("held = hs.LoginScreen(held.revision) return held.addons.rows[1].caps[1]") == "speak for you");
    CHECK(ui("return hs.LoginScreen(held.revision)") == "true");
    // Off the login screen: nil, whatever the revision.
    ui.host.login = nullptr;
    CHECK(ui("return hs.LoginScreen(held.revision)") == "nil");

    ui::ServerSelectView sv;
    sv.servers = {{"Open World", "Up", true}};
    ui.host.server_select = &sv;
    CHECK(ui("held = hs.ServerSelect() return hs.ServerSelect(held.revision)") == "true");
    ui.api.begin_frame();
    for (int i = 0; i < 3; ++i) ui.api.begin_frame();
    CHECK(ui("return hs.ServerSelect(held.revision)") == "true");
    CHECK(ui.api.replaces("serverselect"));
    ui.host.last_server = "Open World"; // from the host, not the view
    CHECK(ui("held = hs.ServerSelect(held.revision) return held.last") == "1");
    sv.servers[0].status = "Down";
    CHECK(ui("held = hs.ServerSelect(held.revision) return held.servers[1].status") == "Down");

    ui::CharacterSelectView cv;
    cv.characters = {{"Bob", 12, 1, {1}, "North Qeynos"}};
    ui.host.char_select = &cv;
    CHECK(ui("held = hs.CharacterSelect() return hs.CharacterSelect(held.revision)") == "true");
    for (int i = 0; i < 4; ++i) ui.api.begin_frame();
    CHECK(ui("return hs.CharacterSelect(held.revision)") == "true");
    CHECK(ui.api.replaces("CharacterListWnd"));
    cv.characters[0].classes = {5, 2};
    CHECK(ui("held = hs.CharacterSelect(held.revision) return held.characters[1].classShort") == "SHD/CLR");
    cv.selected = 0;
    CHECK(ui("held = hs.CharacterSelect(held.revision) return held.selected") == "1");
}
