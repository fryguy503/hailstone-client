// The mail address book window over the hs API (app/lua_ui_mail.cpp): entries typed into its fields are saved to
// the character's address book file and read back with the layout.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <imgui.h>

#include "app/lua_ui.h"
#include "core/game_dir.h"
#include "net/game_connection.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

TEST_CASE("Mail address book window: add, modify, delete, saved and read back") {
    ImGuiContext* ctx = ImGui::CreateContext();
    hailstone::test::ScratchDir dir_scratch("mail_windows_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    GameDir game(dir / "game");
    {
        net::GameConnection conn(game);
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});

        const char* W = "MailAddressBookWindow";
        CHECK_FALSE(api.shown(W));
        CHECK(api.click_child("MailWindow", "AddressBookButton"));
        CHECK(api.shown(W));
        CHECK(api.list_texts(W, "NameList").empty());
        CHECK(api.child_text(W, "AddModifyButton") == "Add");

        CHECK(api.set_child_text(W, "NameField", "Friends"));
        CHECK(api.set_child_text(W, "FieldOfAddresses", "Ann, Bob\nnms2.Cid"));
        CHECK(api.set_child_text(W, "NoteField", "the usual"));
        CHECK(api.click_child(W, "NameFieldIsPrivateCheckbox"));
        CHECK(api.child_checked(W, "NameFieldIsPrivateCheckbox") == true);
        CHECK(api.click_child(W, "AddModifyButton"));
        CHECK(api.child_text(W, "AddModifyButton") == "Modify");
        CHECK(api.click_child(W, "ClearButton"));
        CHECK(api.child_text(W, "NameField") == "");
        CHECK(api.set_child_text(W, "NameField", "alts"));
        CHECK(api.set_child_text(W, "FieldOfAddresses", "Dee"));
        CHECK(api.click_child(W, "AddModifyButton"));
        CHECK(api.list_texts(W, "NameList") == std::vector<std::string>{"alts", "Friends"});

        // A click on a row fills the fields; a right click (not testable here) adds it to the composition.
        CHECK(api.click_list(W, "NameList", 1, 0));
        CHECK(api.child_text(W, "FieldOfAddresses") == "Ann, Bob, nms2.Cid");
        CHECK(api.child_text(W, "NoteField") == "the usual");
        CHECK(conn.mail_address_book().find("friends")->is_private);

        // ConfirmDeletionFromAddressBook (on by default) asks first (eqstr 4546); no keeps the entry.
        CHECK(api.click_list(W, "NameList", 0, 0));
        CHECK(api.click_child(W, "DeleteButton"));
        REQUIRE(host.confirm);
        CHECK(host.confirm->text == conn.strings().format(4546, {}));
        auto no = std::move(host.confirm->done);
        host.confirm.reset();
        no(false);
        CHECK(api.list_texts(W, "NameList") == std::vector<std::string>{"alts", "Friends"});
        CHECK(api.click_child(W, "DeleteButton"));
        REQUIRE(host.confirm);
        auto yes = std::move(host.confirm->done);
        host.confirm.reset();
        yes(true);
        CHECK(api.list_texts(W, "NameList") == std::vector<std::string>{"Friends"});
        // Without it the entry goes at once.
        conn.change_client_option(game::ClientOption::AddressBookConfirmDelete, 0);
        CHECK(api.set_child_text(W, "NameField", "temp"));
        CHECK(api.set_child_text(W, "FieldOfAddresses", "Eve"));
        CHECK(api.click_child(W, "AddModifyButton"));
        CHECK(api.click_child(W, "DeleteButton"));
        CHECK_FALSE(host.confirm);
        CHECK(api.list_texts(W, "NameList") == std::vector<std::string>{"Friends"});
    }
    {
        // A new session reads the file back with the layout.
        net::GameConnection conn(game);
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        REQUIRE(conn.mail_address_book().find("Friends"));
        CHECK(conn.mail_address_book().find("Friends")->addresses.size() == 3);
        CHECK(conn.mail_address_book().entries.size() == 1);
    }
    ImGui::DestroyContext(ctx);
    fs::remove_all(dir);
}

TEST_CASE("Mail ignore list window: the address book's Ignore button, its own book and IgnoreList.txt") {
    ImGuiContext* ctx = ImGui::CreateContext();
    hailstone::test::ScratchDir dir_scratch("mail_ignore_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game" / "userdata");
    {
        // The asset folder's userdata/IgnoreList.txt is where a character without one starts from.
        std::ofstream(dir / "game" / "userdata" / "IgnoreList.txt", std::ios::binary)
            << "EverQuest Address Book v1.0\n# c\n\nName: pest\nAddresses: SOE.EQ.srv.Pest\nFlags: \nNote: \n\n";
    }
    GameDir game(dir / "game");
    {
        net::GameConnection conn(game);
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        CHECK(conn.mail_ignore_list().find("pest"));
        CHECK(fs::exists(dir / "IgnoreList.txt")); // made from the shared one

        const char* A = "MailAddressBookWindow";
        const char* I = "MailIgnoreListWindow";
        CHECK(api.click_child("MailWindow", "AddressBookButton"));
        CHECK_FALSE(api.shown(I));
        CHECK(api.click_child(A, "IgnoreListButton"));
        CHECK(api.shown(I));
        CHECK(api.list_texts(I, "NameList") == std::vector<std::string>{"pest"});

        // An entry goes into the ignore list only, not the address book.
        CHECK(api.set_child_text(I, "NameField", "spammer"));
        CHECK(api.set_child_text(I, "FieldOfAddresses", "Spammy"));
        CHECK(api.click_child(I, "AddModifyButton"));
        CHECK(api.list_texts(I, "NameList") == std::vector<std::string>{"pest", "spammer"});
        CHECK(api.list_texts(A, "NameList").empty());
        CHECK(conn.mail_address_book().entries.empty());

        // Its delete always asks (its book's flag stays at AddressBook_Ctor's 1), the address book option or not.
        conn.change_client_option(game::ClientOption::AddressBookConfirmDelete, 0);
        CHECK(api.click_list(I, "NameList", 0, 0));
        CHECK(api.click_child(I, "DeleteButton"));
        REQUIRE(host.confirm);
        auto yes = std::move(host.confirm->done);
        host.confirm.reset();
        yes(true);
        CHECK(api.list_texts(I, "NameList") == std::vector<std::string>{"spammer"});
    }
    {
        // Saved in the original's format, read back by a new session.
        std::ifstream in(dir / "IgnoreList.txt", std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        CHECK(text.str().starts_with("EverQuest Address Book v1.0\n"));
        CHECK(text.str().find("Name: spammer\nAddresses: ") != std::string::npos);
        net::GameConnection conn(game);
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.game = &game;
        host.conn = &conn;
        app::UiApi api(host);
        api.load_layout(dir / "defaults.ini", dir / "UI_Test_Server.ini", {});
        CHECK(conn.mail_ignore_list().find("spammer"));
        CHECK_FALSE(conn.mail_ignore_list().find("pest"));
    }
    ImGui::DestroyContext(ctx);
    fs::remove_all(dir);
}
