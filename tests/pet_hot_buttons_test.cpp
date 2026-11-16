// The pet window's command buttons dragged onto a hot bar (game/pet_buttons.h, app/lua_ui_misc.cpp PetWindow::pick_up,
// app/lua_ui_hud.cpp hot_button_info): the type 8 button "Pet <command>" of the button's index, saved as the other
// hot buttons are, and checked while its toggle command is on.
#include <doctest/doctest.h>

#include <array>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "app/lua_ui_windows.h"
#include "core/game_dir.h"
#include "game/hotbuttons.h"
#include "game/pet_buttons.h"
#include "net/game_connection.h"
#include "core/byte_io.h"
#include "net/packet_io.h"
#include "net/pet_packets.h"
#include "scratch_dir.h"

using namespace hailstone;

TEST_CASE("Pet hot buttons: the drop's button and SyncHotButtonStates") {
    auto b = game::pet_command_hot_button(4, "Pet", "sit");
    REQUIRE(b);
    CHECK(b->type == game::HotButtonType::PetCommand);
    CHECK(b->slot == 4);
    CHECK(b->label == "Pet sit");
    // saved as 'I' (8 + 'A'), the index, no icon ('@' -1), as SaveHotButtonsToIni writes it
    CHECK(game::format_hot_button(*b) == "I4,@-1,0000000000000000,0,Pet sit");
    auto back = game::parse_hot_button(game::format_hot_button(*b));
    CHECK(back.type == game::HotButtonType::PetCommand);
    CHECK(back.slot == 4);
    CHECK(back.label == "Pet sit");
    CHECK_FALSE(game::pet_command_hot_button(14, "Pet", "x"));
    CHECK_FALSE(game::pet_command_hot_button(-1, "Pet", "x"));
    CHECK(game::pet_command_hot_button(0, "Pet", std::string(80, 'a'))->label.size() == 63);

    CHECK(game::is_toggle_pet_command("follow"));
    CHECK(game::is_toggle_pet_command("spellhold"));
    CHECK_FALSE(game::is_toggle_pet_command("attack"));
    CHECK_FALSE(game::is_toggle_pet_command("back"));
    CHECK_FALSE(game::is_toggle_pet_command("Follow")); // strcmp

    game::PetButtonFace faces[] = {{"attack", true}, {"follow", true}, {"guard", false}, {"none", true}};
    CHECK(game::pet_hot_button_checked("Pet follow", "Pet", faces) == true);
    CHECK(game::pet_hot_button_checked("pet FOLLOW", "Pet", faces) == true); // stricmp
    CHECK(game::pet_hot_button_checked("Pet guard", "Pet", faces) == false);
    CHECK(game::pet_hot_button_checked("Pet attack", "Pet", faces) == false); // not a toggle
    CHECK_FALSE(game::pet_hot_button_checked("Pet none", "Pet", faces));      // holds no command
    CHECK_FALSE(game::pet_hot_button_checked("Who", "Pet", faces));
}

TEST_CASE("Pet command menu: CanUsePetCommand and the window's button layout") {
    // CharacterZoneClient_CanUsePetCommand (the original's pushes in its disassembly:
    // TotalEffect(0x10b, 1, command, 1, 1), (0x119, 1, 0, 1, 1), (0x4a, 1, 0, 1, 1)).
    std::vector<std::pair<std::int32_t, std::int32_t>> asked;
    std::set<std::pair<std::int32_t, std::int32_t>> have;
    game::PetCommandQuery q;
    q.effect = [&](std::int32_t spa, std::int32_t sub) {
        asked.emplace_back(spa, sub);
        return have.count({spa, sub}) ? 1 : 0;
    };
    CHECK_FALSE(game::can_use_pet_command(q, 2)); // no pet
    q.has_pet = true;
    q.race = 75; // a summoned elemental
    q.summoned = true;
    for (std::uint32_t c : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 9u, 0xbu, 0xcu, 0x1cu, 0x1du, 0x1eu}) CHECK(game::can_use_pet_command(q, c));
    for (std::uint32_t c : {0xfu, 0x12u, 0x15u, 0x18u, 0x1bu, 0x1fu, 0x22u}) CHECK_FALSE(game::can_use_pet_command(q, c));
    have = {{0x10b, 0xf}, {0x10b, 0x15}, {0x10b, 0x18}, {0x119, 0}};
    CHECK(game::can_use_pet_command(q, 0xf));   // hold
    CHECK(game::can_use_pet_command(q, 0x15));  // spellhold
    CHECK(game::can_use_pet_command(q, 0x18));  // focus
    CHECK(game::can_use_pet_command(q, 0x1b));  // feign (SPA 281)
    CHECK_FALSE(game::can_use_pet_command(q, 0x12)); // ghold: not granted
    // a charmed pet (not summoned): no taunt, no get lost, no spellhold / focus / feign whatever the AAs
    q.summoned = false;
    CHECK_FALSE(game::can_use_pet_command(q, 0xc));
    CHECK_FALSE(game::can_use_pet_command(q, 0x1d));
    CHECK_FALSE(game::can_use_pet_command(q, 0x15));
    CHECK_FALSE(game::can_use_pet_command(q, 0x1b));
    CHECK(game::can_use_pet_command(q, 0xf)); // hold needs only the AA
    // property 0x1f: only get lost
    q.property_1f = true;
    CHECK(game::can_use_pet_command(q, 0x1d));
    CHECK_FALSE(game::can_use_pet_command(q, 2));
    // a summoned race 127 pet: get lost always, feign on SPA 74, the rest on SPA 267 for the command
    q = {true, 0x7f, true, false, q.effect};
    have = {{0x10b, 2}, {0x4a, 0}};
    CHECK(game::can_use_pet_command(q, 0x1d));
    CHECK(game::can_use_pet_command(q, 2));
    CHECK(game::can_use_pet_command(q, 0x1b));
    CHECK_FALSE(game::can_use_pet_command(q, 0));
    CHECK_FALSE(game::can_use_pet_command(q, 0x1c));

    // PetButtonIndexToPetCommand and the toggle bytes
    CHECK(game::pet_command_of_index(7) == 9);
    CHECK(game::pet_command_of_index(8) == 0x1f);
    CHECK(game::pet_command_of_index(17) == 0x1e);
    CHECK(game::pet_command_of_index(18) == 0x22);
    CHECK(game::pet_command_toggle_state(10) == 6); // hold: OP_PetCommandState 6 (+0x919)
    CHECK(game::pet_command_toggle_state(12) == 9); // spellhold: 9 (+0x91c)
    CHECK(game::pet_command_toggle_state(13) == 8); // focus: 8 (+0x91b)
    CHECK(game::pet_command_toggle_state(2) == -1);
    CHECK(game::pet_command_index("ghold") == 11);
    CHECK(game::pet_command_index("Hold") == game::kPetCommandNone);
    CHECK(game::pet_command_words(15) == "back off");

    // The constructor's layout, saved as SavePetButtonState writes it.
    game::PetButtonLayout l;
    CHECK(l.save() == "attack follow guard taunt sit stop regroup back leave none none none none none ");
    CHECK(l.button_of(2) == 0);
    CHECK(l.command_of(8) == 16); // leave
    CHECK_FALSE(l.enabled(0));    // all disabled until SyncHotButtonStates
    l.sync_enabled();
    CHECK(l.enabled(0));
    CHECK_FALSE(l.enabled(9));
    // the menu: the usable commands (rows 1..18), then none
    REQUIRE(l.menu().size() == 1);
    CHECK(l.menu()[0].item == game::kPetCommandNone);
    std::array<bool, game::kPetCommandCount> usable{};
    usable[2] = usable[10] = usable[13] = true;
    l.set_usable(usable);
    auto rows = l.menu();
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].item == 3);
    CHECK(rows[0].name == "attack");
    CHECK(rows[1].name == "hold");
    CHECK(rows[2].name == "focus");
    CHECK(rows[3].name == "none");
    // hold onto Pet9; then attack onto Pet9 takes it from Pet0 (none, disabled)
    l.choose(9, 11);
    CHECK(l.text(9) == "hold");
    CHECK(l.button_of(10) == 9);
    l.sync_enabled();
    CHECK(l.enabled(9));
    l.choose(9, 3);
    CHECK(l.text(9) == "attack");
    CHECK(l.text(0) == "none");
    CHECK_FALSE(l.enabled(0));
    CHECK(l.button_of(2) == 9);
    CHECK(l.button_of(10) == -1);
    l.choose(9, game::kPetCommandNone);
    CHECK(l.text(9) == "none");
    CHECK_FALSE(l.enabled(9));
    CHECK(l.button_of(2) == -1);
    // LoadPetButtonState: words in order (strtok), fewer words leave the rest as they are
    game::PetButtonLayout m;
    CHECK_FALSE(m.load(""));
    CHECK(m.load("hold  focus attack"));
    CHECK(m.text(0) == "hold");
    CHECK(m.text(1) == "focus");
    CHECK(m.text(2) == "attack");
    CHECK(m.text(3) == "taunt");
    CHECK(m.button_of(2) == 2);
    CHECK(m.button_of(4) == -1); // follow is on no button now
    CHECK(m.button_of(13) == 1);
}

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

} // namespace

TEST_CASE("Pet hot buttons: a pet window button dragged to a bar, lit while its command is on") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        hailstone::test::ScratchDir scratch("pet_hot_buttons");
        GameDir game(scratch.path());
        net::GameConnection conn(game, {});
        game::HotButtonStore store;
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        host.hot_buttons = &store;
        host.set_hot_button = [&](int bar, int button, const game::HotButton& b) { store.current(bar, button) = b; };
        app::UiApi api(host);
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        const std::string pet = conn.strings().format(game::kStrPet, {});
        api.begin_frame(); // SyncHotButtonStates: the buttons holding a command are enabled

        // Pet1_Button is "follow" (the constructor's table): its drag puts "Pet follow", value 1, on the cursor.
        CHECK(run(L, "return hs.PickUp('PetInfoWindow', 'Pet1_Button')") == "true");
        REQUIRE(host.cursor_button);
        CHECK(host.cursor_button->type == game::HotButtonType::PetCommand);
        CHECK(host.cursor_button->slot == 1);
        CHECK(host.cursor_button->label == pet + " follow");
        CHECK(run(L, "return hs.ClickHotButton('HotButtonWnd', 'HB_Button2', 'left')") == "true");
        CHECK(store.current(0, 1).type == game::HotButtonType::PetCommand);
        CHECK(run(L, "local k, l = hs.HotButton('HotButtonWnd', 'HB_Button2'); return k .. '|' .. l") ==
              "button|" + pet + " follow");
        // a button holding no command ("none": Pet9_Button ..) is disabled and gives nothing
        CHECK(run(L, "return hs.PickUp('PetInfoWindow', 'Pet9_Button')") == "false");
        CHECK(run(L, "return (hs.ChildState('PetInfoWindow', 'Pet9_Button'))") == "false");
        // attack (not a toggle) on button 3
        CHECK(run(L, "return hs.PickUp('PetInfoWindow', 'Pet0_Button')") == "true");
        run(L, "hs.ClickHotButton('HotButtonWnd', 'HB_Button3', 'left')");

        // OP_PetCommandState: follow (EQEmu PET_BUTTON_FOLLOW 3) on: the follow hot button is checked.
        CHECK(run(L, "return hs.HotButtonChecked('HotButtonWnd', 'HB_Button2')") == "false");
        conn.receive_zone_packet_for_test(net::encode(net::PetCommandState{3, 1}));
        CHECK(run(L, "return hs.HotButtonChecked('HotButtonWnd', 'HB_Button2')") == "true");
        CHECK(run(L, "return hs.HotButtonChecked('HotButtonWnd', 'HB_Button3')") == "false");
        conn.receive_zone_packet_for_test(net::encode(net::PetCommandState{3, 0}));
        CHECK(run(L, "return hs.HotButtonChecked('HotButtonWnd', 'HB_Button2')") == "false");

        // The right button down on a "none" button: the command menu (no pet seen yet: only "none").
        CHECK(run(L, "return hs.ClickChild('PetInfoWindow', 'Pet9_Button', 'rightdown')") == "true");
        REQUIRE(host.menus.size() == 1);
        REQUIRE(host.menus[0].rows.size() == 1);
        CHECK(host.menus[0].rows[0].text == "none");
        CHECK(run(L, "return select(5, hs.ChildState('PetInfoWindow', 'Pet9_Button'))") == "true"); // bAction
        // with hold and focus usable (CanUsePetCommand while the pet was there), hold goes on Pet9 and is saved
        const auto ini = scratch.path() / "UI_test.ini";
        api.load_layout(scratch.path() / "defaults.ini", ini, {});
        CHECK(host.pet_buttons.save() == "attack follow guard taunt sit stop regroup back leave none none none none none ");
        std::array<bool, game::kPetCommandCount> usable{};
        usable[10] = usable[13] = true;
        host.pet_buttons.set_usable(usable);
        CHECK(run(L, "return hs.ClickChild('PetInfoWindow', 'Pet9_Button', 'rightdown')") == "true");
        REQUIRE(host.menus.size() == 1);
        REQUIRE(host.menus[0].rows.size() == 3);
        CHECK(host.menus[0].rows[0].text == "hold");
        CHECK(host.menus[0].rows[1].text == "focus");
        host.menus[0].rows[0].act();
        api.begin_frame(); // SyncHotButtonStates enables it
        CHECK(run(L, "return (hs.WindowText('PetInfoWindow', 'Pet9_Button'))") == "hold");
        CHECK(run(L, "return (hs.ChildState('PetInfoWindow', 'Pet9_Button'))") == "true");
        {
            std::ifstream f(ini);
            std::string all((std::istreambuf_iterator<char>(f)), {});
            CHECK(all.find("PetButtonState=attack follow guard taunt sit stop regroup back leave hold none none none none ") !=
                  std::string::npos);
        }
        // dragged to a bar: "Pet hold", lit while OP_PetCommandState hold (6) is on
        CHECK(run(L, "return hs.PickUp('PetInfoWindow', 'Pet9_Button')") == "true");
        REQUIRE(host.cursor_button);
        CHECK(host.cursor_button->slot == 9);
        CHECK(host.cursor_button->label == pet + " hold");
        run(L, "hs.ClickHotButton('HotButtonWnd', 'HB_Button5', 'left')");
        conn.receive_zone_packet_for_test(net::encode(net::PetCommandState{6, 1}));
        CHECK(run(L, "return hs.HotButtonChecked('HotButtonWnd', 'HB_Button5')") == "true");
        CHECK(run(L, "return select(2, hs.ChildState('PetInfoWindow', 'Pet9_Button'))") == "true");
        conn.receive_zone_packet_for_test(net::encode(net::PetCommandState{6, 0}));
        CHECK(run(L, "return hs.HotButtonChecked('HotButtonWnd', 'HB_Button5')") == "false");
        // the next login reads it back
        host.pet_buttons = {};
        api.load_layout(scratch.path() / "defaults.ini", ini, {});
        CHECK(host.pet_buttons.text(9) == "hold");
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Pet toggles: the ini keys, GetPetCommandAvailable and ReissueToggledPetCommands") {
    CHECK(game::pet_toggle_ini_key(4) == "PetButtonToggleState0"); // follow
    CHECK(game::pet_toggle_ini_key(9) == "PetButtonToggleState5"); // taunt
    CHECK(game::pet_toggle_ini_key(10) == "PetButtonToggleState6"); // hold
    CHECK(game::pet_toggle_ini_key(13) == "PetButtonToggleState9"); // focus
    CHECK(game::pet_toggle_ini_key(3).empty());
    CHECK(game::pet_toggle_ini_key(14).empty());
    // the bytes by PET_BUTTON_*: hold (index 10) is button 6, spellhold (12) 9, focus (13) 8
    game::PetToggleStates st{};
    game::set_pet_toggle(st, 10, true);
    CHECK(st[6]);
    game::set_pet_toggle(st, 12, true);
    CHECK(st[9]);
    CHECK(game::pet_toggle(st, 12));
    CHECK_FALSE(game::pet_toggle(st, 13));
    CHECK_FALSE(game::pet_toggle(st, 2));

    game::PetButtonLayout layout;
    CHECK_FALSE(game::pet_command_available(layout, 0xf)); // no pet seen: nothing usable
    CHECK(game::pet_command_available(layout, 0x23));      // past the table: always
    CHECK_FALSE(game::pet_command_available(layout, 0x22));
    std::array<bool, game::kPetCommandCount> usable{};
    usable[10] = usable[8] = usable[17] = true; // hold, regroup, target
    layout.set_usable(usable);
    CHECK(game::pet_command_available(layout, 0xf));
    CHECK(game::pet_command_available(layout, 0x10));
    CHECK(game::pet_command_available(layout, 0x11));
    CHECK_FALSE(game::pet_command_available(layout, 0x12));
    CHECK(game::pet_command_available(layout, 0x20)); // regroup on
    CHECK(game::pet_command_available(layout, 0x1e)); // target
    CHECK_FALSE(game::pet_command_available(layout, 0x1d));

    // on: follow, taunt, hold, spellhold; spellhold not usable: follow 4, taunt 0xc, hold 0xf in index order
    game::PetToggleStates on{};
    for (int i : {4, 9, 10, 12}) game::set_pet_toggle(on, i, true);
    auto cmds = game::pet_commands_to_reissue(on, [](std::uint32_t c) { return c != 0x15; });
    CHECK(cmds == std::vector<std::uint32_t>{4, 0xc, 0xf});
    CHECK(game::pet_commands_to_reissue(on, {}).empty());

    // the /pet words' toggle actions (cmd_pet's table)
    auto w = [](const char* s) { return *game::parse_pet_command(s); };
    CHECK(w("hold").toggle == game::PetToggleAction::Flip);
    CHECK(w("hold").toggle_index == 10);
    CHECK(w("hold on").toggle == game::PetToggleAction::On);
    CHECK(w("hold off").toggle == game::PetToggleAction::Off);
    CHECK(w("no taunt").toggle == game::PetToggleAction::Off);
    CHECK(w("no taunt").toggle_index == 9);
    CHECK(w("no cast").toggle == game::PetToggleAction::On);
    CHECK(w("no cast").toggle_index == 12);
    CHECK(w("focus").toggle_index == 13);
    CHECK(w("as you were").toggle_index == 7);
    auto f = w("follow");
    CHECK(f.toggle == game::PetToggleAction::OnOff);
    CHECK(f.toggle_index == 4);
    CHECK(f.toggle_index2 == 5);
    CHECK(w("guard").toggle_index == 5);
    CHECK(w("guard").toggle_index2 == 4);
    CHECK(w("attack").toggle == game::PetToggleAction::None);
    CHECK(w("back off").toggle == game::PetToggleAction::None);
}

TEST_CASE("Pet toggles: /pet words save PetButtonToggleState, a new pet gets the toggles that are on") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        hailstone::test::ScratchDir scratch("pet_toggles");
        GameDir game(scratch.path());
        net::GameConnection conn(game, {});
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        app::UiApi api(host);
        const auto ini = scratch.path() / "char" / "UI_test.ini";
        std::filesystem::create_directories(ini.parent_path());
        {
            std::ofstream f(ini);
            f << "[PetInfoWindow]\nPetButtonToggleState5=1\n"; // taunt on from an earlier session
        }
        api.load_layout(scratch.path() / "defaults.ini", ini, {});
        api.begin_frame(); // the window links itself to the connection and reads the toggles
        CHECK(conn.pet_window_linked());
        CHECK(conn.pet_toggles()[5]);       // taunt
        CHECK_FALSE(conn.pet_toggles()[3]); // follow: ReadUIINIBool's default 0

        // /pet hold flips hold's byte and saves its key, without a pet too
        conn.pet_command_words("hold");
        CHECK(conn.pet_toggles()[6]);
        conn.pet_command_words("no taunt");
        CHECK_FALSE(conn.pet_toggles()[5]);
        conn.pet_command_words("guard"); // guard on, follow off
        CHECK(conn.pet_toggles()[4]);
        CHECK_FALSE(conn.pet_toggles()[3]);
        auto text = [&] {
            std::ifstream f(ini);
            return std::string((std::istreambuf_iterator<char>(f)), {});
        };
        CHECK(text().find("PetButtonToggleState6=1") != std::string::npos);
        CHECK(text().find("PetButtonToggleState5=0") != std::string::npos);
        CHECK(text().find("PetButtonToggleState1=1") != std::string::npos);
        CHECK(text().find("PetButtonToggleState0=0") != std::string::npos);
        // the server's state changes the byte (the lit state) but not the ini
        conn.receive_zone_packet_for_test(net::encode(net::PetCommandState{6, 0}));
        CHECK_FALSE(conn.pet_toggles()[6]);
        CHECK(text().find("PetButtonToggleState6=1") != std::string::npos);
        conn.receive_zone_packet_for_test(net::encode(net::PetCommandState{6, 1}));

        // A pet comes (here by OP_Charm): ReissueToggledPetCommands asks CanUsePetCommand for the toggles that are
        // on, in index order (guard 5, hold 0xf); a second spawn with the pet there already does not.
        std::vector<std::uint32_t> asked;
        net::GameConnection::PetWindowLink link;
        link.usable = [&](std::uint32_t c) {
            asked.push_back(c);
            return true;
        };
        link.saved = [](int, bool) {};
        conn.link_pet_window(link);
        auto& world = conn.world_for_test();
        world.player_id = 10;
        world.spawns[10].id = 10;
        world.spawns[11].id = 11;
        world.spawns[12].id = 12;
        auto charm = [&](std::uint32_t pet) {
            ByteWriter b;
            b.le<std::uint32_t>(10);
            b.le<std::uint32_t>(pet);
            b.le<std::uint32_t>(1);
            conn.receive_zone_packet_for_test(net::make_packet(net::Op::Charm, std::move(b)));
        };
        charm(11);
        CHECK(conn.pets().focused == 11);
        CHECK(asked == std::vector<std::uint32_t>{5, 0xf});
        asked.clear();
        charm(12);
        CHECK(asked.empty());
        // gone (the spawn deleted), then a new one
        conn.receive_zone_packet_for_test(net::encode(net::PetCommandState{4, 0}));
        world.spawns.erase(11);
        world.spawns.erase(12);
        ByteWriter del;
        del.le<std::uint32_t>(12);
        del.le<std::uint8_t>(0);
        conn.receive_zone_packet_for_test(net::make_packet(net::Op::DeleteSpawn, std::move(del)));
        CHECK(conn.pets().focused == 0);
        world.spawns[13].id = 13;
        charm(13);
        CHECK(asked == std::vector<std::uint32_t>{0xf});

        // Before the window is made (no link yet) a pet's arrival sends nothing; the link's first refresh does.
        net::GameConnection conn2(game, {});
        auto& w2 = conn2.world_for_test();
        w2.player_id = 10;
        w2.spawns[10].id = 10;
        w2.spawns[11].id = 11;
        ByteWriter b2;
        b2.le<std::uint32_t>(10);
        b2.le<std::uint32_t>(11);
        b2.le<std::uint32_t>(1);
        conn2.receive_zone_packet_for_test(net::make_packet(net::Op::Charm, std::move(b2)));
        game::PetToggleStates taunt{};
        game::set_pet_toggle(taunt, 9, true);
        conn2.load_pet_toggles(taunt);
        asked.clear();
        conn2.link_pet_window(link);
        CHECK(asked == std::vector<std::uint32_t>{0xc});
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Pet window: a pet there before the UI ini is read still pops the window up over a saved hidden state") {
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        hailstone::test::ScratchDir scratch("pet_popup");
        GameDir game(scratch.path());
        net::GameConnection conn(game, {});
        app::UiHost host;
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        app::UiApi api(host);
        auto& world = conn.world_for_test();
        world.player_id = 10;
        world.spawns[10].id = 10;
        world.spawns[11].id = 11;
        ByteWriter b;
        b.le<std::uint32_t>(10);
        b.le<std::uint32_t>(11);
        b.le<std::uint32_t>(1);
        conn.receive_zone_packet_for_test(net::make_packet(net::Op::Charm, std::move(b)));
        api.begin_frame(); // a frame sees the pet before the character's UI ini is loaded
        const auto ini = scratch.path() / "char" / "UI_test.ini";
        std::filesystem::create_directories(ini.parent_path());
        {
            std::ofstream f(ini);
            f << "[PetInfoWindow]\nShow=0\n";
        }
        api.load_layout(scratch.path() / "defaults.ini", ini, {});
        lua_State* L = luaL_newstate();
        luaL_openlibs(L);
        lua_newtable(L);
        api.register_functions(L);
        lua_setglobal(L, "hs");
        CHECK(run(L, "return (hs.ReplaceWindow('PetInfoWindow'))") == "false"); // the saved state
        api.begin_frame(); // RefreshPetState: a pet, the window's first look
        CHECK(run(L, "return (hs.ReplaceWindow('PetInfoWindow'))") == "true");
        // a copied layout read again does not make the window anew
        api.load_layout(scratch.path() / "defaults.ini", ini, {});
        run(L, "hs.ShowWindow('PetInfoWindow', false)");
        api.begin_frame();
        CHECK(run(L, "return (hs.ReplaceWindow('PetInfoWindow'))") == "false");
        lua_close(L);
    }
    ImGui::DestroyContext(ctx);
}
