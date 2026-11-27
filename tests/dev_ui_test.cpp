#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "core/server_profile.h"
#include "core/config.h"
#include "game/eq_names.h"
#include "platform/open_folder.h"
#include "ui/dev_screens.h"
#include "ui/imgui_layer.h"
#include "ui/lua/lua_host.h"

namespace {
struct VisibilityUi {
    std::unique_ptr<ImGuiContext, void (*)(ImGuiContext*)> context{ImGui::CreateContext(), ImGui::DestroyContext};
    hailstone::ui::detail::ImGuiVisibility visibility;
    char text[96] = "saved";
    bool shown = true;
    ImGuiID edit = 0;
    ImVec2 position{}, size{};
    VisibilityUi() {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    }
    void begin() {
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(800, 600); io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(140, 140);
        visibility.prepare_input();
        ImGui::NewFrame();
        visibility.prepare_input();
    }
    void editor(bool focus = false) {
        ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(300, 180), ImGuiCond_Once);
        ImGui::Begin("Persistent editor", &shown);
        if (focus) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("Text", text, sizeof(text));
        edit = ImGui::GetItemID();
        position = ImGui::GetWindowPos(); size = ImGui::GetWindowSize();
        ImGui::End();
    }
    void frame(bool focus = false) { begin(); editor(focus); (void)visibility.finish_frame(true); }
};
}

TEST_CASE("UI visibility: hiding discards every draw list while preserving the window and modal state") {
    VisibilityUi ui;
    ui.begin(); ui.editor();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(1, 1), ImVec2(20, 20), IM_COL32_WHITE);
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(25, 1), ImVec2(45, 20), IM_COL32_WHITE);
    REQUIRE(ui.visibility.finish_frame(true));
    REQUIRE(ImGui::GetDrawData());
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    CHECK(ImGui::GetDrawData()->CmdLists.Size >= 3);
    const auto position = ui.position, size = ui.size;

    ui.begin(); ui.editor();
    ImGui::OpenPopup("Still open");
    REQUIRE(ImGui::BeginPopupModal("Still open"));
    ImGui::TextUnformatted("Keep this confirmation"); ImGui::EndPopup();
    const auto popups = GImGui->OpenPopupStack.Size;
    REQUIRE(popups > 0);
    ui.visibility.set_visible(false); // also tests hiding after content was already emitted in this frame
    CHECK(GImGui->OpenPopupStack.Size == popups);
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(1, 1), ImVec2(20, 20), IM_COL32_WHITE);
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(25, 1), ImVec2(45, 20), IM_COL32_WHITE);
    CHECK_FALSE(ui.visibility.finish_frame(true));
    CHECK(ImGui::GetDrawData() == nullptr);
    CHECK(ui.shown);

    ui.visibility.set_visible(true);
    ui.begin(); ui.editor();
    REQUIRE(ImGui::BeginPopupModal("Still open"));
    ImGui::TextUnformatted("Still a confirmation"); ImGui::EndPopup();
    REQUIRE(ui.visibility.finish_frame(true));
    REQUIRE(ImGui::GetDrawData());
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    CHECK(ui.position.x == position.x); CHECK(ui.position.y == position.y);
    CHECK(ui.size.x == size.x); CHECK(ui.size.y == size.y);
    CHECK(ui.shown);
}

TEST_CASE("Loading presentation: over every window, or beneath the plugins' question alone while it waits") {
    for (const bool question : {false, true}) {
        CAPTURE(question);
        VisibilityUi ui;
        hailstone::ui::LoadingView loading;
        loading.title = "Checking required add-ons";
        loading.detail = "Review the plugin's permission request.";
        loading.progress = 0.5f;
        hailstone::ui::lua::set_consent_guard(question);
        for (int frame = 0; frame < 2; ++frame) {
            ui.begin();
            // As ClientApp::run: without a question, the overlay (order does not matter); with one, the cover after the
            // frame's other windows, then the question's windows raised above it.
            if (!question) hailstone::ui::loading_overlay(loading);
            ImGui::SetNextWindowPos(ImVec2(100, 100));
            ImGui::SetNextWindowSize(ImVec2(360, 180));
            ImGui::Begin("Server plugin permission", nullptr, ImGuiWindowFlags_NoSavedSettings);
            hailstone::ui::lua::guard_current_window();
            ImGui::TextUnformatted("Allow this server's plugin?");
            ImGui::Button("Allow");
            const ImDrawList* asked = ImGui::GetWindowDrawList();
            ImGui::End();
            // Another window, begun after the question (it would be in front of it without the raise).
            ImGui::SetNextWindowPos(ImVec2(50, 50));
            ImGui::SetNextWindowSize(ImVec2(200, 120));
            ImGui::Begin("Chat", nullptr, ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextUnformatted("hello");
            const ImDrawList* other = ImGui::GetWindowDrawList();
            ImGui::End();
            if (question) {
                hailstone::ui::loading_overlay(loading, false);
                hailstone::ui::lua::raise_guarded_windows();
            }
            REQUIRE(ui.visibility.finish_frame(true));
            const ImDrawData* data = ImGui::GetDrawData();
            REQUIRE(data);
            auto position = [&](const ImDrawList* list) {
                for (int i = 0; i < data->CmdLists.Size; ++i)
                    if (data->CmdLists[i] == list) return i;
                return -1;
            };
            REQUIRE(position(asked) >= 0);
            REQUIRE(position(other) >= 0);
            const ImGuiWindow* cover = ImGui::FindWindowByName(hailstone::ui::kLoadingCoverWindow);
            if (!question) {
                const ImDrawList* fg = ImGui::GetForegroundDrawList();
                CHECK(fg->VtxBuffer.Size > 0);
                CHECK(position(asked) < position(fg));
                CHECK(position(other) < position(fg));
                CHECK(cover == nullptr);
                continue;
            }
            REQUIRE(cover);
            CHECK(cover->DrawList->VtxBuffer.Size > 0);
            CHECK(position(other) < position(cover->DrawList));
            CHECK(position(cover->DrawList) < position(asked));
            CHECK(ImGui::GetForegroundDrawList()->VtxBuffer.Size == 0);
            CHECK(ImGui::GetBackgroundDrawList()->VtxBuffer.Size == 0);
        }
        hailstone::ui::lua::set_consent_guard(false);
    }
}

TEST_CASE("UI visibility: text and mouse input from hidden frames cannot replay into the restored editor") {
    VisibilityUi ui;
    ui.frame(true); ui.frame(); ui.frame();
    REQUIRE(GImGui->ActiveId == ui.edit);
    CHECK(ui.visibility.wants_keyboard(-1));
    auto& io = ImGui::GetIO();
    io.AddInputCharactersUTF8("kept");
    ui.visibility.set_visible(true); // an idempotent visible update must not discard ordinary input
    ui.frame();
    const std::string kept = ui.text;
    CHECK(kept != "saved");
    io.AddInputCharactersUTF8("discarded");
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    REQUIRE(GImGui->InputEventsQueue.Size > 0);
    ui.visibility.set_visible(false);
    CHECK(GImGui->ActiveId == 0);
    CHECK_FALSE(io.WantTextInput);
    CHECK(GImGui->InputEventsQueue.empty());
    // Even stale capture flags cannot claim the game's input while presentation is hidden.
    io.WantCaptureMouse = io.WantCaptureKeyboard = true;
    CHECK_FALSE(ui.visibility.wants_mouse()); CHECK_FALSE(ui.visibility.wants_keyboard(1));
    for (int n = 0; n < 2; ++n) {
        io.AddInputCharactersUTF8("hidden"); io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        ui.begin();
        CHECK_FALSE(io.WantTextInput);
        CHECK_FALSE(ui.visibility.finish_frame(true));
    }
    // A queue written just before restoration must be discarded too, not only in the hidden-frame path.
    io.SetAppAcceptingEvents(true);
    io.AddInputCharactersUTF8("late hidden"); io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    REQUIRE(GImGui->InputEventsQueue.Size > 0);
    ui.visibility.set_visible(true);
    CHECK(GImGui->InputEventsQueue.empty());
    ui.frame();
    CHECK(std::string(ui.text) == kept);
    CHECK(GImGui->ActiveId == 0);
    CHECK_FALSE(io.MouseDown[ImGuiMouseButton_Left]);
    ui.frame(true); ui.frame();
    REQUIRE(GImGui->ActiveId == ui.edit);
    io.AddInputCharactersUTF8("!"); ui.frame();
    CHECK(std::string(ui.text) != kept);
    CHECK(std::string(ui.text).find('!') != std::string::npos);
}

TEST_CASE("UI visibility: hiding cancels an actual drag payload and skipped frames expose no old draw data") {
    VisibilityUi ui;
    ui.begin(); ui.editor();
    REQUIRE(ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern));
    const int value = 17;
    ImGui::SetDragDropPayload("TEST_ITEM", &value, sizeof(value));
    ImGui::EndDragDropSource();
    REQUIRE(ImGui::GetDragDropPayload());
    ui.visibility.set_visible(false);
    CHECK(ImGui::GetDragDropPayload() == nullptr);
    CHECK_FALSE(GImGui->DragDropActive);
    CHECK_FALSE(ui.visibility.finish_frame(true));
    ui.visibility.set_visible(true);
    ui.frame();
    REQUIRE(ImGui::GetDrawData());
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    ui.begin(); ui.editor();
    CHECK_FALSE(ui.visibility.finish_frame(false));
    CHECK(ImGui::GetDrawData() == nullptr);
}

TEST_CASE("Race and class names") {
    using namespace hailstone::game;
    CHECK(race_name(1) == "Human");
    CHECK(race_name(128) == "Iksar");
    CHECK(race_name(130) == "Vah Shir");
    CHECK(race_name(330) == "Froglok");
    CHECK(race_name(522) == "Drakkin");
    CHECK(race_name(9999) == "Race 9999");
    CHECK(is_playable_race(12));
    CHECK_FALSE(is_playable_race(13));
    CHECK(class_name(5) == "Shadow Knight");
    CHECK(class_name(25) == "Druid Guildmaster");
    CHECK(class_name(41) == "Merchant");
    std::vector<std::uint32_t> multi{5, 2, 0, 12};
    CHECK(class_list_name(multi) == "Shadow Knight / Cleric / Wizard");
    CHECK(class_list_name(multi, true) == "SHD/CLR/WIZ");
    CHECK(class_list_name({}) == "None");
    // The multiclass display of a class mask (bit c-1 = class c).
    CHECK(class_mask_name((1u << 0) | (1u << 1)) == "War/Clr");
    CHECK(class_mask_name((1u << 4) | (1u << 15)) == "Shd/Ber");
    CHECK(class_mask_name(0).empty());
    CHECK(class_mask_name(1u << 20).empty()); // only classes 1..16
}

namespace {
// A fresh servers/ folder under the system temp folder, removed again at the end of the test.
struct TempServers {
    std::filesystem::path path;
    TempServers() {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() / ("hailstone_dev_ui_servers_" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempServers() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace

TEST_CASE("Login profiles: saved and picked again, never the password") {
    using namespace hailstone;
    TempServers servers;
    Config cfg;
    ui::LoginView in;
    CHECK_FALSE(ui::load_login_settings(in, cfg, servers.path));
    CHECK(in.profiles.empty());
    CHECK(in.selected == -1); // a new server with the defaults
    CHECK(in.host.empty()); // no address of anyone's own network as the default
    in.name = "My Server!";
    in.host = " 10.0.0.2 ";
    in.port = 5998;
    in.username = "tester";
    in.password = "secret";
    auto saved = ui::store_login_settings(in, cfg);
    REQUIRE(saved);
    CHECK(saved->id == "my-server");
    CHECK(saved->host == "10.0.0.2");
    CHECK(cfg.get_string("login.profile") == "my-server");
    CHECK(std::filesystem::is_directory(servers.path / "my-server" / "assets"));
    CHECK(in.selected == 0);
    CHECK(in.password == "secret"); // what was typed stays for the login
    CHECK(ui::selected_assets_dir(in) == servers.path / "my-server" / "assets");

    // A second one; the last saved is picked at the next start.
    ui::new_profile(in);
    CHECK(in.username.empty());
    in.host = "play.example.org"; // no name: the host names it
    in.username = "other";
    in.remember = false;
    saved = ui::store_login_settings(in, cfg);
    REQUIRE(saved);
    CHECK(saved->name == "play.example.org");
    CHECK(saved->id == "play-example-org");

    ui::LoginView out;
    CHECK_FALSE(ui::load_login_settings(out, cfg, servers.path));
    REQUIRE(out.profiles.size() == 2);
    CHECK(ui::selected_profile_id(out) == "play-example-org");
    CHECK(out.username.empty()); // not remembered
    CHECK(out.password.empty());
    CHECK(out.remember == false);
    // Picking the other shows its fields.
    ui::select_profile(out, 0);
    CHECK(out.name == "My Server!");
    CHECK(out.host == "10.0.0.2");
    CHECK(out.port == 5998);
    CHECK(out.username == "tester");
    // Editing keeps the folder: a renamed server is still servers/my-server.
    out.name = "Renamed";
    saved = ui::store_login_settings(out, cfg);
    REQUIRE(saved);
    CHECK(saved->id == "my-server");
    CHECK(out.profiles.size() == 2);
    CHECK(out.profiles[static_cast<std::size_t>(out.selected)].name == "Renamed");

    // --server by id or by name, any case.
    CHECK(ui::select_profile_named(out, "PLAY-EXAMPLE-ORG"));
    CHECK(out.host == "play.example.org");
    CHECK(ui::select_profile_named(out, "renamed"));
    CHECK(ui::selected_profile_id(out) == "my-server");
    CHECK_FALSE(ui::select_profile_named(out, "nothing"));
    CHECK(ui::selected_profile_id(out) == "my-server");

    // Delete removes its folder and picks another.
    REQUIRE(ui::delete_login_profile(out, cfg));
    CHECK_FALSE(std::filesystem::exists(servers.path / "my-server"));
    REQUIRE(out.profiles.size() == 1);
    CHECK(ui::selected_profile_id(out) == "play-example-org");
}

TEST_CASE("Login profiles: eqhost.txt's login server") {
    using namespace hailstone;
    // [LoginServer]'s first Host line, with its port.
    auto p = profile_from_eqhost("[LoginServer]\r\nHost=login.example.org:5999\r\nHost=other:1\r\n");
    REQUIRE(p);
    CHECK(p->host == "login.example.org");
    CHECK(p->port == 5999);
    CHECK(p->name == "login.example.org");
    CHECK(p->id.empty());
    // No port: ServerProfile's default, as a new server in the login screen gets.
    auto bare = profile_from_eqhost("[LoginServer]\nhost = 10.1.2.3 \n");
    REQUIRE(bare);
    CHECK(bare->host == "10.1.2.3");
    CHECK(bare->port == ServerProfile{}.port);
    // A byte order mark, and a Host before any section.
    auto bom = profile_from_eqhost("\xEF\xBB\xBFHost=bom.example.org:6000\n");
    REQUIRE(bom);
    CHECK(bom->host == "bom.example.org");
    CHECK(bom->port == 6000);
    // IPv6: a bare address has no port; "[v6]:port" has one.
    auto v6 = profile_from_eqhost("[LoginServer]\nHost=2001:db8::1\n");
    REQUIRE(v6);
    CHECK(v6->host == "2001:db8::1");
    CHECK(v6->port == ServerProfile{}.port);
    auto v6port = profile_from_eqhost("[LoginServer]\nHost=[2001:db8::1]:5999\n");
    REQUIRE(v6port);
    CHECK(v6port->host == "2001:db8::1");
    CHECK(v6port->port == 5999);
    // Only Host in [LoginServer]: Host1, other sections and broken lines are skipped for a later good one.
    auto later = profile_from_eqhost("[Other]\nHost=no:1\n[LoginServer]\nHost1=no:2\nHost=x:0\nHost=x:65536\n"
                                     "Host=x:12ab\nHost=x:\nHost=[::1]x\nHost=:5999\nHost=good:65535\n");
    REQUIRE(later);
    CHECK(later->host == "good");
    CHECK(later->port == 65535);
    CHECK_FALSE(profile_from_eqhost("[LoginServer]\n"));
    CHECK_FALSE(profile_from_eqhost("Host=:5999\nHost=x:99999\nHost=x:-1\nHost=x: \n"));
    CHECK_FALSE(profile_from_eqhost(""));

    // The file: only its first kMaxEqhostBytes are read.
    TempServers dir;
    const std::filesystem::path big = dir.path / "big.txt";
    {
        std::ofstream out(big, std::ios::binary);
        out << std::string(kMaxEqhostBytes, '\n') << "[LoginServer]\nHost=late:5999\n";
    }
    CHECK_FALSE(profile_from_eqhost_file(big));
    CHECK_FALSE(profile_from_eqhost_file(dir.path / "missing.txt"));
}

TEST_CASE("Login profiles: a fresh install starts on the server its eqhost.txt names, once") {
    using namespace hailstone;
    TempServers servers;
    const std::filesystem::path eqhost = servers.path / "eqhost.txt";
    std::ofstream(eqhost) << "[LoginServer]\nHost=login.example.org:5999\n";
    // No saved server: the eqhost.txt one is made, picked and named in login.profile.
    Config cfg;
    ui::LoginView view;
    CHECK(ui::load_login_settings(view, cfg, servers.path, eqhost));
    REQUIRE(view.profiles.size() == 1);
    CHECK(view.selected == 0);
    CHECK(view.host == "login.example.org");
    CHECK(view.port == 5999);
    CHECK(cfg.get_string("login.profile") == view.profiles[0].id);
    CHECK(cfg.get_bool("login.eqhost_imported", false));
    // Loaded again: nothing more is added.
    ui::LoginView again;
    CHECK_FALSE(ui::load_login_settings(again, cfg, servers.path, eqhost));
    CHECK(again.profiles.size() == 1);
    // The player deletes it: it does not come back.
    REQUIRE(remove_profile(view.profiles[0].id, servers.path));
    ui::LoginView emptied;
    CHECK_FALSE(ui::load_login_settings(emptied, cfg, servers.path, eqhost));
    CHECK(emptied.profiles.empty());

    // A player who already has a server when the file is first seen: it is never imported, not even after they
    // delete theirs.
    TempServers other;
    ServerProfile mine;
    mine.host = "mine.example.org";
    mine.name = mine.host;
    mine.id = make_profile_id(mine.name, other.path);
    REQUIRE(save_profile(mine, other.path));
    Config cfg2;
    ui::LoginView had;
    CHECK(ui::load_login_settings(had, cfg2, other.path, eqhost));
    CHECK(had.profiles.size() == 1);
    CHECK(had.host == "mine.example.org");
    CHECK(cfg2.get_bool("login.eqhost_imported", false));
    REQUIRE(remove_profile(mine.id, other.path));
    ui::LoginView none;
    CHECK_FALSE(ui::load_login_settings(none, cfg2, other.path, eqhost));
    CHECK(none.profiles.empty());
}

TEST_CASE("Login profiles: each server keeps its own user interface") {
    using namespace hailstone;
    TempServers servers;
    Config cfg;
    cfg.set("ui.name", "modern");
    ui::LoginView view;
    ui::load_login_settings(view, cfg, servers.path);
    CHECK(view.default_ui == "modern");
    CHECK(view.ui == "modern"); // a new server starts with ui.name
    view.host = "one.example.org";
    view.ui = "gamepad";
    REQUIRE(ui::store_login_settings(view, cfg));
    ui::new_profile(view);
    CHECK(view.ui == "modern");
    view.host = "two.example.org";
    REQUIRE(ui::store_login_settings(view, cfg));

    ui::LoginView again;
    ui::load_login_settings(again, cfg, servers.path);
    REQUIRE(ui::select_profile_named(again, "one.example.org"));
    CHECK(again.ui == "gamepad");
    CHECK(ui::profile_from_view(again).ui == "gamepad");
    REQUIRE(ui::select_profile_named(again, "two.example.org"));
    CHECK(again.ui == "modern");
    // A server.ini without one shows ui.name.
    auto p = load_profile("two-example-org", servers.path);
    REQUIRE(p);
    p->ui.clear();
    REQUIRE(save_profile(*p, servers.path));
    CHECK(load_profile("two-example-org", servers.path)->ui.empty());
    cfg.set("ui.name", "classic");
    ui::LoginView third;
    ui::load_login_settings(third, cfg, servers.path);
    REQUIRE(ui::select_profile_named(third, "two.example.org"));
    CHECK(third.ui == "classic");
}

TEST_CASE("Login profiles: the old login keys become the first profile") {
    using namespace hailstone;
    TempServers servers;
    Config cfg;
    cfg.set("login.host", "10.0.0.9");
    cfg.set_int("login.port", 6000);
    cfg.set("login.username", "olduser");
    cfg.set_bool("login.remember", true);
    ui::LoginView view;
    CHECK(ui::load_login_settings(view, cfg, servers.path));
    REQUIRE(view.profiles.size() == 1);
    CHECK(view.selected == 0);
    CHECK(view.host == "10.0.0.9");
    CHECK(view.port == 6000);
    CHECK(view.username == "olduser");
    CHECK(view.name == "10.0.0.9");
    CHECK_FALSE(cfg.get("login.host"));
    CHECK_FALSE(cfg.get("login.username"));
    CHECK(cfg.get_string("login.profile") == view.profiles[0].id);
    // Done once: with a profile there, nothing is migrated again.
    cfg.set("login.host", "10.0.0.10");
    ui::LoginView again;
    CHECK_FALSE(ui::load_login_settings(again, cfg, servers.path));
    CHECK(again.profiles.size() == 1);
    CHECK(again.host == "10.0.0.9");
}

TEST_CASE("Login profiles: a test login's server is never saved") {
    using namespace hailstone;
    TempServers servers;
    Config cfg;
    ui::LoginView view;
    ui::load_login_settings(view, cfg, servers.path);
    view.transient = true;
    view.host = "127.0.0.1";
    view.username = "test";
    view.password = "pw";
    ServerProfile p = ui::profile_from_view(view);
    CHECK(p.id.empty());
    CHECK(p.host == "127.0.0.1");
    CHECK_FALSE(ui::store_login_settings(view, cfg));
    CHECK(ui::selected_assets_dir(view).empty());
    CHECK(hailstone::list_profiles(servers.path).empty());
    CHECK_FALSE(cfg.get("login.profile"));
}

TEST_CASE("Login accounts: kept per server, passwords only in the keyring, the box set") {
    using namespace hailstone;
    TempServers servers;
    Config cfg;
    platform::MemoryCredentials keyring;
    ui::LoginView in;
    in.credentials = &keyring;
    ui::load_login_settings(in, cfg, servers.path);
    in.name = "Box Server";
    in.host = "Play.Example.org";
    in.port = 5998;

    // The first account: its password is kept once the login takes it, never in server.ini.
    in.username = "main";
    in.password = "hunter2";
    in.save_password = true;
    REQUIRE(ui::store_login_settings(in, cfg));
    CHECK(keyring.size() == 0); // not before the server took it
    CHECK(ui::save_login_password(in));
    CHECK(keyring.load("play.example.org:5998", "main") == std::optional<std::string>("hunter2"));
    // A second, its password not kept.
    in.username = "alt";
    in.password = "pw2";
    in.save_password = false;
    REQUIRE(ui::store_login_settings(in, cfg));
    CHECK(ui::save_login_password(in));
    CHECK_FALSE(keyring.load("play.example.org:5998", "alt"));
    REQUIRE(in.accounts.size() == 2);
    CHECK(in.accounts[0].username == "main");
    CHECK(in.accounts[0].save_password);
    CHECK_FALSE(in.accounts[1].save_password);
    std::stringstream ini;
    ini << std::ifstream(servers.path / "box-server" / "server.ini").rdbuf();
    CHECK(ini.str().find("hunter2") == std::string::npos);
    CHECK(ini.str().find("pw2") == std::string::npos);
    CHECK(ini.str().find("[Account:main]") != std::string::npos);

    // The box set: only accounts with a saved password can go.
    in.accounts[0].box = true;
    in.accounts[0].character = "Mainchar";
    CHECK(ui::box_ready(in));
    in.accounts[1].box = true;
    CHECK_FALSE(ui::box_ready(in)); // alt has no saved password
    in.accounts[1].box = false;
    REQUIRE(ui::store_login_settings(in, cfg));
    CHECK(note_account_character("box-server", "ALT", "World A", "Altchar", servers.path)); // any case
    CHECK_FALSE(note_account_character("box-server", "alt", "World A", "Altchar", servers.path)); // no change
    CHECK_FALSE(note_account_character("box-server", "nobody", "World A", "X", servers.path));
    // The characters each world's character select lists, for the Characters list.
    CHECK(note_account_characters("box-server", "alt", "World A",
                                  {{"Altchar", "", 12, "WAR"}, {"Second", "", 3, "SHD/CLR"}}, servers.path));
    CHECK(note_account_characters("box-server", "alt", "World B", {{"Faraway", "", 50, "WIZ"}}, servers.path));
    CHECK_FALSE(note_account_characters("box-server", "alt", "World B", {{"Faraway", "", 50, "WIZ"}}, servers.path));
    CHECK(note_account_characters("box-server", "main", "World A", {{"Mainchar", "", 60, "CLR"}}, servers.path));

    // Read again: the last account, the accounts, and the saved password filled in on picking one.
    ui::LoginView out;
    out.credentials = &keyring;
    ui::load_login_settings(out, cfg, servers.path);
    CHECK(out.username == "alt");
    CHECK(out.password.empty()); // alt's is not kept
    REQUIRE(out.accounts.size() == 2);
    CHECK(out.accounts[0].box);
    CHECK(out.accounts[0].character == "Mainchar");
    CHECK(out.accounts[1].character == "Altchar");
    CHECK(out.accounts[1].world == "World A");
    REQUIRE(out.accounts[1].characters.size() == 3);
    CHECK(out.accounts[1].characters[1].name == "Second");
    CHECK(out.accounts[1].characters[1].level == 3);
    CHECK(out.accounts[1].characters[1].classes == "SHD/CLR");
    CHECK(out.accounts[1].characters[2].world == "World B");
    // The Characters list: account by account, the last account's last character picked first.
    auto quick = ui::quick_characters(out);
    REQUIRE(quick.size() == 4);
    CHECK(quick[0].character.name == "Mainchar");
    CHECK(quick[0].password_saved);
    CHECK(quick[1].username == "alt");
    CHECK_FALSE(quick[1].password_saved);
    CHECK(quick[1].last); // alt was used last, and entered Altchar on World A
    CHECK(out.quick_selected == 1);
    CHECK(ui::default_quick_character(out) == 1);
    REQUIRE(ui::box_accounts(out).size() == 1);
    CHECK(ui::box_accounts(out)[0].username == "main");
    ui::pick_account(out, "MAIN");
    CHECK(out.username == "main");
    CHECK(out.password == "hunter2");
    CHECK(out.save_password);

    // Unticking Save password takes it out of the keyring at the next login.
    out.save_password = false;
    REQUIRE(ui::store_login_settings(out, cfg));
    CHECK_FALSE(keyring.load("play.example.org:5998", "main"));
    out.save_password = true;
    REQUIRE(ui::store_login_settings(out, cfg));
    CHECK(ui::save_login_password(out));

    // Forget takes the account and its password away.
    REQUIRE(ui::forget_account(out, cfg, "main"));
    CHECK_FALSE(keyring.load("play.example.org:5998", "main"));
    REQUIRE(out.accounts.size() == 1);
    CHECK(out.accounts[0].username == "alt");
    CHECK(out.username.empty());

    // Without a keyring nothing is kept and there is no box set.
    ui::LoginView none;
    ui::load_login_settings(none, cfg, servers.path);
    CHECK_FALSE(ui::can_save_passwords(none));
    none.username = "alt";
    none.password = "pw2";
    none.save_password = true;
    REQUIRE(ui::store_login_settings(none, cfg));
    CHECK(ui::save_login_password(none)); // nothing to do
    CHECK_FALSE(none.accounts[0].save_password);

    // Deleting the server takes its saved passwords.
    REQUIRE(ui::store_login_settings(out, cfg));
    out.username = "main";
    out.password = "hunter2";
    out.save_password = true;
    REQUIRE(ui::store_login_settings(out, cfg));
    CHECK(ui::save_login_password(out));
    CHECK(keyring.size() == 1);
    REQUIRE(ui::delete_login_profile(out, cfg));
    CHECK(keyring.size() == 0);
}

TEST_CASE("Login accounts: the worlds the login server listed, for the World lists") {
    using namespace hailstone;
    TempServers servers;
    Config cfg;
    ui::LoginView view;
    ui::load_login_settings(view, cfg, servers.path);
    view.name = "Triune";
    view.host = "hs.example.org";
    view.username = "me";
    REQUIRE(ui::store_login_settings(view, cfg));
    CHECK(listed_worlds("triune", servers.path).empty());
    // Another key of a world's section, which the listed worlds' writes keep.
    std::filesystem::create_directories(profile_worlds_ini("triune", servers.path).parent_path());
    std::ofstream(profile_worlds_ini("triune", servers.path), std::ios::binary) << "[Tunaria]\nkeep=1\n";
    REQUIRE(set_listed_worlds("triune", {"Project Triune", "Tunaria"}, servers.path));
    CHECK(listed_worlds("triune", servers.path) == std::vector<std::string>{"Project Triune", "Tunaria"});
    {
        std::ifstream in(profile_worlds_ini("triune", servers.path), std::ios::binary);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(all.find("keep=1") != std::string::npos); // its other keys stay
    }
    REQUIRE(set_listed_worlds("triune", {"Tunaria"}, servers.path));
    CHECK(listed_worlds("triune", servers.path) == std::vector<std::string>{"Tunaria"});
    // The view: the listed ones, then any other world an account has characters on.
    CHECK(note_account_characters("triune", "me", "Old World", {{"Oldie", "", 5, "WAR"}}, servers.path));
    ui::LoginView out;
    ui::load_login_settings(out, cfg, servers.path);
    CHECK(out.worlds == std::vector<std::string>{"Tunaria", "Old World"});
}

TEST_CASE("Server profile: a world name with '=' keeps one set of characters, written once") {
    using namespace hailstone;
    TempServers servers;
    Config cfg;
    ui::LoginView view;
    ui::load_login_settings(view, cfg, servers.path);
    view.name = "Odd";
    view.host = "odd.example.org";
    view.username = "me";
    REQUIRE(ui::store_login_settings(view, cfg));
    CHECK(saved_character_key_part("A=B\r\nC") == "A_B__C");
    CHECK(note_account_characters("odd", "me", "A=B", {{"One", "", 5, "WAR"}, {"Two", "", 6, "CLR"}}, servers.path));
    CHECK_FALSE(note_account_characters("odd", "me", "A=B", {{"One", "", 5, "WAR"}, {"Two", "", 6, "CLR"}},
                                        servers.path)); // the same list: nothing written
    // A character deleted on the server leaves the saved list.
    CHECK(note_account_characters("odd", "me", "A=B", {{"One", "", 5, "WAR"}}, servers.path));
    auto p = load_profile("odd", servers.path);
    REQUIRE(p);
    const SavedAccount* a = find_account(*p, "me");
    REQUIRE(a);
    REQUIRE(a->characters.size() == 1);
    CHECK(a->characters[0].name == "One");
    CHECK(a->characters[0].world == saved_character_key_part("A=B"));
}

TEST_CASE("Keyring keys: the address in lower case and the port") {
    CHECK(hailstone::platform::credential_server("Play.Example.ORG", 5999) == "play.example.org:5999");
}

TEST_CASE("Folder URLs for the file manager") {
    using hailstone::platform::file_url;
    CHECK(file_url("/home/me/hailstone/servers/a b/assets") == "file:///home/me/hailstone/servers/a%20b/assets");
    CHECK(file_url("/tmp/100%") == "file:///tmp/100%25");
    CHECK(file_url(std::filesystem::path("C:/EQ/servers")) == "file:///C:/EQ/servers");
}

TEST_CASE("Login: the Game files row names a folder RoF2, Live or not usable") {
    CHECK(hailstone::ui::game_folder_label({"/games/Older files", false, true}) == "Legacy: Older files");
    CHECK(hailstone::ui::game_folder_label({"/games/Game files/", true, true}) == "Current: Game files");
    CHECK(hailstone::ui::game_folder_label({"/mnt/old", true, false}) == "Not usable: old");
}

TEST_CASE("Character creation: stats start at the base with the class's points to spend") {
    using namespace hailstone::ui;
    CharacterCreateView v;
    CreateOption o{1, 1, 201, 2, {}, {}};
    o.base = {75, 75, 75, 75, 75, 75, 75};
    o.points = {10, 0, 5, 10, 0, 0, 0};
    CreateOption tall = o;
    tall.deity = 202;
    tall.base[0] = 140;
    v.options = {o, tall};
    v.option = 0;
    create_reset_stats(v);
    CHECK(v.stats[0] == 75);
    CHECK(v.points_left == 25);
    CHECK_FALSE(create_stat_down(v, 0)); // not under the base
    for (int i = 0; i < 25; ++i) CHECK(create_stat_up(v, 1));
    CHECK(v.stats[1] == 100);
    CHECK(v.points_left == 0);
    CHECK_FALSE(create_stat_up(v, 2)); // nothing left
    CHECK(create_stat_down(v, 1));
    CHECK(v.points_left == 1);
    CHECK(create_stat_cap(v, 1) == 100); // base + 25
    v.option = 1;
    CHECK_FALSE(create_stat_up(v, 0)); // the stats are the other combination's until reset
    create_reset_stats(v);
    CHECK(v.stats[0] == 140);
    CHECK(create_stat_cap(v, 0) == 150); // never over 150
    for (int i = 0; i < 10; ++i) CHECK(create_stat_up(v, 0));
    CHECK_FALSE(create_stat_up(v, 0));
    CHECK(v.points_left == 15);
    // A world server's absurd allocation: no signed overflow in the sum or the cap.
    CreateOption huge = o;
    huge.base.fill(0xffffffffu);
    huge.points.fill(0x7fffffffu);
    v.options = {huge};
    v.option = 0;
    create_reset_stats(v);
    CHECK(v.stats[0] == 255);
    CHECK(v.points_left == 7 * 255);
    CHECK(create_stat_cap(v, 0) == 150);
    CHECK_FALSE(create_stat_up(v, 0));
    CHECK_FALSE(create_stat_down(v, 0));
}

TEST_CASE("Character creation: race and class stay among the options; a deity keeps the city that still goes with it") {
    using namespace hailstone::ui;
    CharacterCreateView v;
    // race 1 class 1: deity 201 starts in zones 2 and 9, deity 202 only in 9; race 2 has class 5 alone
    auto opt = [](std::uint32_t race, std::uint32_t cls, std::uint32_t deity, std::uint32_t zone, std::uint32_t pts) {
        CreateOption o{race, cls, deity, zone, {}, {}};
        o.base.fill(70);
        o.points = {pts, 0, 0, 0, 0, 0, 0};
        return o;
    };
    v.options = {opt(1, 1, 201, 2, 20), opt(1, 1, 201, 9, 20), opt(1, 1, 202, 9, 30), opt(2, 5, 203, 2, 25)};
    v.race = 7; // not offered
    v.class_id = 3;
    create_normalize(v);
    CHECK(v.race == 1);
    CHECK(v.class_id == 1);
    CHECK(v.option == 0);
    CHECK(v.points_left == 20);
    CHECK(create_races(v) == std::vector<std::uint32_t>{1, 2});
    CHECK(create_classes(v) == std::vector<std::uint32_t>{1});
    CHECK(create_deities(v).size() == 2);
    CHECK(create_cities(v).size() == 2);
    CHECK(create_set_city(v, 9));
    CHECK(v.option == 1);
    CHECK(create_stat_max(v, 0));
    CHECK(v.points_left == 0);
    CHECK(v.stats[0] == 90);
    CHECK_FALSE(create_stat_max(v, 0)); // nothing left
    CHECK(create_set_deity(v, 202)); // zone 9 goes with deity 202 too: the city stays
    CHECK(v.option == 2);
    CHECK(v.points_left == 30); // a new combination: the stats again
    CHECK(create_cities(v) == std::vector<std::uint32_t>{9});
    CHECK(create_set_deity(v, 201)); // back: zone 9 still goes with it
    CHECK(v.option == 1);
    CHECK_FALSE(create_set_city(v, 77));
    CHECK_FALSE(create_set_deity(v, 999));
    CHECK(create_set_deity(v, 202));
    v.race = 2; // the class follows the race
    create_normalize(v);
    CHECK(v.class_id == 5);
    CHECK(v.option == 3);
    CHECK(create_deity_label(v, 203) == "Deity 203"); // no names: the id
    v.name = "Bob";
    create_stat_max(v, 0);
    create_stat_up(v, 1);
    while (create_stat_up(v, 1)) {}
    CHECK(v.points_left == 0);
    CHECK_FALSE(create_ready(v)); // a name of 3 letters
    v.name = "Bobby";
    CHECK(create_ready(v));
    v.busy = true;
    CHECK_FALSE(create_ready(v));
}

TEST_CASE("Login: the client's own screen draws the corner, What's new's button and the add-on store") {
    using namespace hailstone;
    TempServers servers;
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    auto frame = [](ui::LoginView& v) {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1280, 800);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        const ui::LoginAction a = ui::login_screen(v);
        ImGui::Render();
        return a;
    };
    ui::LoginView v;
    v.servers_dir = servers.path;
    v.version = "0.4.0-alpha (abc1234)";
    v.update.current = "0.4.0-alpha";
    v.release_notes = {"One thing", "Another"};
    v.project_page = "https://example.invalid/hailstone";
    for (const char* check : {"off", "checking", "uptodate", "unknown", "newer", "behind"}) {
        v.update.check = check;
        frame(v);
        CHECK(ImGui::FindWindowByName("##hailstone_about") != nullptr);
    }
    v.update.state = "available";
    v.update.version = "0.5.0";
    v.update.has_package = true;
    frame(v);
    v.update = {};

    // The add-ons window opens on the store, which asks for its list once it can be fetched.
    v.addons.open = true;
    CHECK(frame(v) != ui::LoginAction::AddonBrowse); // nothing can be fetched here
    v.addons.available = true;
    CHECK(frame(v) == ui::LoginAction::AddonBrowse);
    v.addons.browsing = true;
    CHECK(frame(v) == ui::LoginAction::None);
    v.addons.browsing = false;
    v.addons.browsed = true;
    for (const char* state : {"install", "update", "installed", "manual", "unavailable"}) {
        ui::AddonStoreItemView it;
        it.name = std::string("store-") + state;
        it.source = "owner/" + it.name;
        it.kind = "script";
        it.version = "2.0.0";
        it.worlds = "Alpha World";
        it.state = state;
        it.problem = "no build for this system";
        v.addons.store.push_back(it);
    }
    ui::AddonOfferView offer;
    offer.name = "store-install";
    offer.caps = {"read the game state"};
    v.addons.offer = offer;
    CHECK(frame(v) == ui::LoginAction::None);
    ImGui::DestroyContext(ctx);
}
