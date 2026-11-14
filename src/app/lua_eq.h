#pragma once

// The `eq` table of the Lua UI packs (ui/<pack>): the scripts are written against the original client's
// CXWnd tree; here the same functions, with the same arguments and returns,
// answer from this client's own state. A stock window is a model (app/lua_eq_windows.h) keyed by its XML screen
// name, and each of its children by its XML ScreenID: what the original window's code would have set on that
// child (text, shown, enabled, art, gauge) is computed from GameConnection, the spell, item and group state, and a
// click on a child runs the client action the original's handler does.
//
// Labels and gauges with an EQType read app/lua_eq_values.h, the original's __GetLabelFromEQ and
// __GetGaugeValueFromEQ.

#include <cstdint>
#include <functional>
#include <future>
#include <filesystem>
#include <memory>
#include <optional>
#include <array>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "app/bug_report.h"
#include "game/ability_book.h"
#include "game/char_select_camera.h"
#include "game/hotbuttons.h"
#include "game/pet_buttons.h"
#include "mq/tlo.h"
#include "plugin/consent.h"

struct lua_State;

namespace hailstone::net { class GameConnection; struct Item; struct ItemSlot; struct CharCreate; }
namespace hailstone::game { class HotButtonStore; struct HotButton; namespace mechanics { class Character; } }
namespace hailstone { class GameDir; }
namespace hailstone::assets { class CharacterLibrary; struct CharacterModel; }
namespace hailstone::mq { class ScriptHost; class Registry; struct Context; }
namespace hailstone::ui {
class UiTextures; class UiIni; struct ChatView; struct ServerSelectView; struct LoginView; struct CharacterSelectView;
struct CharacterCreateView;
}
namespace hailstone::game { struct ItemLink; struct QuantityOpen; struct Spawn; class CharCreateLook; }

namespace hailstone::plugin { class UiBridge; class WebAccess; }
namespace hailstone::app {

class ActionsUi;
class Gamepad;

// What the eq.* functions read and drive; the client fills it in every frame.
struct EqHost {
    net::GameConnection* conn = nullptr;   // the world / zone connection, null before login
    bool in_game = false;                  // a character is in the world
    bool ui_ready = false;                 // in game or at character select
    int game_state = 0;                    // CEverQuest::GameState: 1 character select, 2 creation, 5 in game
    const game::mechanics::Character* character = nullptr; // the player as the original computes it
    ui::UiTextures* textures = nullptr;
    // The in-game browser (app/lua_eq_web.cpp over app/web_service.h, docs/browser.md), and a plugin's capabilities
    // by its owner tag ("plugin:<name>", "script:<name>": LuaUi::caller_tag), which its windows act with
    // (Automation::plugin_caps).
    plugin::WebAccess* web = nullptr;
    std::function<std::uint32_t(std::string_view plugin)> plugin_caps;
    game::HotButtonStore* hot_buttons = nullptr;
    // The Lua UI's layout settings (ui.GetSetting/SetSetting, "hud." keys): read and set, so a client menu can change
    // one (the hot bar's Lock layout). Empty without a Lua UI.
    std::function<std::string(std::string_view key)> layout_setting;
    std::function<void(std::string_view key, std::string_view value)> set_layout_setting;
    // The pet window's command buttons (CPetInfoWnd +0x234.., +0x26c; app/lua_eq_misc.cpp): what each holds, read by
    // the hot buttons it checks (SyncHotButtonStates).
    game::PetButtonLayout pet_buttons;
    // CHotButtonWnd::DoHotButton: bar 0..9, button 0..11 of the bar's current page; `mouse`: a click on the button (the
    // menu's Activate is not one).
    std::function<void(int bar, int button, bool mouse)> press_hot_button;
    // The text a hotbutton shows (its label, or the skill / spell / item name the original fills in).
    std::function<std::string(int bar, int button)> hot_button_label;
    // A quick right click on a hotbutton (0x54fc10: a social's stop or editor, an item's use, ...).
    std::function<void(int bar, int button)> hot_button_menu;
    // The hotbutton CCursorAttachment carries (dragged from the Actions or AA window or off a bar; a click on a
    // hotbutton puts it there), putting a button on bar 0..9, button 0..11 of the bar's current page (saved), and
    // the text a button shows wherever it is (its label, or the skill / spell / menu name).
    std::optional<game::HotButton> cursor_button;
    // A spell picked up in the spell book (CCursorAttachment type 1, its index the book slot): a click on a gem
    // memorizes it there. It goes when the memorize is sent or the book closes.
    std::optional<int> cursor_spell;
    int cursor_serial = 0; // counts what is put on the cursor
    std::function<void(int bar, int button, const game::HotButton& b)> set_hot_button;
    std::function<std::string(const game::HotButton& b)> button_label;
    // The item information window for an item (a right click on it).
    std::function<void(const net::Item& item)> inspect_item;
    // CObjectPreviewWnd: Send_OP_ItemPreviewRequest_006ffc20(item id) and the preview window shown (the guild bank's
    // Alt right click, CGuildBankWnd::sub_0067cb80): GameConnection::preview_item_id.
    std::function<void(std::uint32_t item_id)> preview_item;
    // The items the item display windows show (oldest first; ItemDisplayWindow is the first), and closing one.
    std::vector<net::Item>* item_displays = nullptr;
    // Each display's own number (it stays with the window while older ones close): the window's ItemWndIndex.
    std::vector<std::uint64_t>* item_display_ids = nullptr;
    // Runs a key command as its key does (the EQ menu's commands; game/keymap.h numbers).
    std::function<void(int command)> run_command;
    // The options the options window changes, by name ("music", "effects", "realism": 0..100; "environment",
    // "combat_music": 0 / 1): hailstone.ini's audio.* keys.
    std::function<int(std::string_view option)> get_option;
    std::function<void(std::string_view option, int value)> set_option;
    // The option by name, nullopt for one the client does not have (eq.Option), and the choices of one the client
    // lists (eq.OptionChoices: "resolution").
    std::function<std::optional<int>(std::string_view option)> find_option;
    std::function<std::vector<std::string>(std::string_view option)> option_choices;
    // Whether a book spell dropped or clicked onto gem `gem` (0-based) starts memorizing there: an empty usable gem,
    // or (Hailstone, option replace_gem_spell, on by default) one that holds another spell, which the server forgets
    // as it memorizes the new one (Client::MemSpell). The original refuses a gem that holds a spell
    // (StartSpellMemorization bReplace 0). Defined in lua_eq_spellbook.cpp.
    bool gem_takes_book_spell(int gem) const;
    // The spell display windows (SpellDisplayWindow, #2 ..): spell id, shown only while a right button is held.
    std::vector<std::pair<std::int32_t, bool>> spell_displays;
    void show_spell(std::int32_t spell, bool temporary) {
        for (auto& s : spell_displays)
            if (s.first == spell) return;
        spell_displays.emplace_back(spell, temporary);
        if (spell_displays.size() > 6) spell_displays.erase(spell_displays.begin());
    }
    std::function<void(int index)> close_item_display;
    // The spell book's toggle (CCastSpellWnd's book button, the SPELLBOOK key).
    std::function<void()> toggle_spell_book;
    const GameDir* game = nullptr;          // eqclient.ini (the chat colours)
    ui::ChatView* chat = nullptr;           // the client's chat lines (one main chat window)
    std::function<void(const std::string& line)> submit_chat;    // a typed line (CEverQuest::InterpretCmd)
    std::function<void(const game::ItemLink& link)> click_link;  // an item link clicked in chat
    float heading = 0.0f;                   // the player's heading, 0..512 (the compass and the map)
    // The map (app/lua_eq_map.cpp): the zone the player is in, the player's feet in world coordinates
    // (game/coords.h) and the zone's box, the terrain's min / max (pinstSceneGraph +0x50, game/collision.h
    // CollisionMesh::bounds).
    std::string zone_short_name;
    std::string zone_long_name;             // the zone's name for the windows (OP_NewZone)
    float game_hour = -1.0f;                // the game clock (game/sky_clock.h hour: 1.0 .. 24.99), -1 unknown
    // Where the map edits made on this server go and are read first (servers/<profile>/maps, core/server_profile.h);
    // empty: maps/ in the character's folder (the user_dir of StockWindow::ini_loaded).
    std::filesystem::path maps_dir;
    // The player's maps for every server (platform::maps_dir()), after the server's assets (game/map_files.h).
    std::filesystem::path global_maps_dir;
    // The server's own folder (servers/<profile>, core/server_profile.h): the map's zone links are kept there
    // (game/zone_routes.h).
    std::filesystem::path server_dir;
    // The current zone's exits, from its zone-line regions and zone points (game::collect_neighbour_lines), worked
    // out by the app once the zone points are in: each zone a line leads to, and where (world coordinates). For the
    // map's exits and the zone finder's "lead me there" (docs/navigation.md "Zone atlas").
    struct ZoneExit {
        std::uint32_t zone_id = 0;
        float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0}; // round the lines to that zone
        std::vector<std::array<float, 6>> cells;     // each line piece: lo xyz, hi xyz
    };
    std::vector<ZoneExit> zone_exits;
    std::uint32_t zone_exits_zone = 0; // the zone they were worked out for; 0 not yet
    bool feet_valid = false;
    float feet[3] = {0, 0, 0};
    bool zone_box_valid = false;
    float zone_box_lo[3] = {0, 0, 0}, zone_box_hi[3] = {0, 0, 0};
    // Runs a button through CHotButtonWnd::DoHotButton (the Actions window: abilities, socials, menu buttons).
    std::function<void(const game::HotButton& b)> run_button;
    // The target is within melee range (ActionsUi::target_in_melee_range; the Ability Book's autoskill).
    std::function<bool()> target_in_melee_range;
    // A script holds autoskill off (Automation::autoskill_held): it uses nothing meanwhile.
    std::function<bool()> autoskill_held;
    // The social editor (SocialEditWnd, 0x758bb0) on social 0..119; EqApi sets it.
    std::function<void(std::int32_t social)> open_social_editor;
    int social_editing = -1; // the social SocialEditWnd is open on (0..119), -1 while it is closed
    // Lua socials (app/lua_eq_luasocial.cpp): the script host they run in, MQ's TLOs for the editor's completion, and
    // a request to open the Lua social editor on social 0..119 (-1 none; the editor window takes it).
    mq::ScriptHost* scripts = nullptr;
    const mq::Registry* registry = nullptr;
    // What the plugins give the UI (plugin/ui_bridge.h; app/lua_eq_plugins.h): null without plugins.
    plugin::UiBridge* plugin_ui = nullptr;
    // The server's compiled plugins waiting for the player's word (Automation::plugin_consent; eq.PluginConsent), and
    // the answer (true allow, false refuse, nullopt put away). Empty: none.
    std::function<std::optional<plugin::ConsentPrompt>()> plugin_consent;
    std::function<void(std::optional<bool> allow)> answer_plugin_consent;
    std::function<const mq::Context&()> tlo_context;
    int lua_social_edit = -1;
    std::function<std::string(std::uint32_t skill)> skill_name;
    std::function<bool()> spell_book_open;
    // The item previews' views (app/item_preview_view.h): this frame's image at a size (an ImTextureID, null when
    // there is nothing to show), and its camera ("rotate", "pan", "zoom", "reset"). View 0 is the object preview
    // window's, 1 the marketplace's.
    std::function<void*(int width, int height, int view)> item_preview_image;
    std::function<void(std::string_view what, float x, float y, int view)> item_preview_control;
    // The Actions window's open state and page (ActionsUi keeps them for the keys).
    // The keep-C++-windows switch (config ui.cpp_windows): with it on, the C++ windows stay on next to the Lua ones.
    bool keep_cpp_windows = false;
    // The Find window (app/lua_eq_find.cpp, a Hailstone addition that replaces the original's FindLocationWnd): open
    // or not. The find key (CMD_TOGGLE_FIND_WINDOW) and the Actions window's Find toggle it; each pack draws it (the
    // modern pack as a page of the map, the gamepad pack as a window of its own).
    bool find_open = false;
    // The Find window's guide path (app/guide_path.h): starts it to an NPC (false: no such NPC), ends it, and while
    // it is shown its line (the player first) for the maps. Unset (tests): eq.FindNpcPath asks the server's find path.
    std::function<bool(std::uint32_t spawn)> guide_to;
    std::function<void()> end_guide;
    // The Find window's Walk there: the navigator walks the player to the NPC (/nav id), the guide path showing;
    // "" when the walk started, else why not.
    std::function<std::string(std::uint32_t spawn)> walk_to;
    const std::vector<glm::vec3>* guide_line = nullptr;

    // A quantity dialog a window opened (CQuantityWnd::Open with that window as the owner; the stack split's
    // dialog is the connection's quantity_request()): the slider's range and start, where it shows (none:
    // centred), and the owner's handler for the accepted quantity (open_quantity() in app/lua_eq_dialogs.cpp).
    struct UiQuantity {
        std::int32_t min = 0, max = 0, slider = 0;
        std::optional<std::pair<int, int>> position;
        std::function<void(std::int32_t quantity)> done;
        std::function<bool()> owner_valid;
        int serial = 0;
    };
    std::optional<UiQuantity> quantity;
    std::int32_t air = -1, max_air = 0; // the air supply (PlayerMovement; -1 not yet counted) and its maximum
    // A search for the Advanced Inventory (/finditem <text>), until its window takes it (eq.TakeItemSearch).
    std::optional<std::string> item_search;
        bool body_tint_open = false; // BodyTintWnd (the inventory's Tint button opens it; app/lua_eq_tint.cpp)
    // The player customization window (PlayerCustomizationWnd, app/lua_eq_customization.cpp): the character library
    // and the model the player is drawn with (the window tries features on it), and the window's camera: -1 while
    // it is closed, else CharSelect_PositionCameraForModel's zoom (100 the face, 0 the whole body).
    const assets::CharacterLibrary* char_library = nullptr;
    std::shared_ptr<const assets::CharacterModel> player_model;
    int customization_zoom = -1;
    int customization_feature = 0; // its +0x2b4 (1 face .. 10 facial attachment; creation's camera turns on 2 / 3)
    bool customization_shown = false;
    // Its camera calls (FaceCamera, CharSelect_PositionCameraForModel) not yet replayed on Cameras[7], in order: the
    // pre-game scene's camera at creation, the client's customization camera in game.
    std::vector<game::CustomizationCameraCall> customization_camera;
    int quantity_serial = 0;
    // The screen rectangle {x0, y0, x1, y1} of the child the last click went to (the stock callers place the quantity
    // dialog at their button or slot).
    std::array<float, 4> click_rect{-1, -1, -1, -1};
    // The ground drop's confirmation (CEverQuest::LMouseUp's ConfirmationDialog 0x6e, eqstr 9057): the client's
    // flag, set when GameConnection::request_drop_cursor() asked.
    bool* drop_confirm = nullptr;
    // A text entry question (CTextEntryWnd::Activate): the prompt, the longest answer and the owner's handler.
    struct TextEntry {
        std::string prompt, text;
        int min_chars = 0; // CTextEntryWnd::UpdateButtons: OK only with at least this many
        int max_chars = 0;
        std::function<void(const std::string& text)> done;
        std::function<bool()> owner_valid;
    };
    std::optional<TextEntry> text_entry;
    // A window's own ConfirmationDialog question (the options window's Default Keys, ...): Yes / No, answered
    // with the handler; `timeout_ms` (from when it shows; 0 none) answers no.
    struct Confirm {
        std::string text;
        int timeout_ms = 0;
        std::function<void(bool yes)> done;
    };
    std::optional<Confirm> confirm;
    // CColorPickerWnd::Open(caller, colour): the colour to start from (0xRRGGBB) and the caller's notification 0x25
    // with the accepted colour (0xffRRGGBB). `serial` tells a new request from the shown one; `caller` lets a
    // closing caller take its request back.
    struct ColorPick {
        std::uint32_t rgb = 0;
        std::function<void(std::uint32_t argb)> done;
        int serial = 0;
        const void* caller = nullptr;
    };
    std::optional<ColorPick> color_pick;
    int color_pick_serial = 0;
    void pick_color(std::uint32_t rgb, std::function<void(std::uint32_t argb)> done, const void* caller = nullptr) {
        color_pick = ColorPick{rgb & 0xffffffu, std::move(done), ++color_pick_serial, caller};
    }
    // The key map and its capture (the options window's Keys page).
    ActionsUi* actions = nullptr;
    // The gamepad's state, mode and bindings (app/gamepad.h; eq.gamepad, app/lua_eq_gamepad.cpp).
    Gamepad* gamepad = nullptr;

    // The game's context menus (CContextMenuManager) a window opened: menus.lua draws them (eq.GameMenus). A row runs
    // `act` when chosen and closes the menus; a row with `submenu` opens it beside the row when hovered.
    // A disabled row keeps its colour, as CContextMenu::AddMenuItem's (enabled 0) does; EnableMenuItem(row, false)
    // @0085bae0 also sets kMenuDisabledRgb.
    static constexpr std::uint32_t kMenuDisabledRgb = 0x808080;
    struct MenuRow {
        std::string text;
        std::uint32_t rgb = 0xffffff;
        bool check = false, enabled = true, separator = false;
        std::function<void()> act;
        std::function<std::vector<MenuRow>()> submenu;
    };
    struct GameMenu {
        float x = 0, y = 0, w = 0, h = 0;
        std::vector<MenuRow> rows;
        std::vector<float> row_y; // each row's top inside the menu (GameMenus fills it)
    };
    std::vector<GameMenu> menus;
    // 0x761f10, CContextMenuManager::PopupMenu @0x862220's test: context menus open only with the ContextMenus
    // option (0xde0d51) and, in game, the player not down (game::context_menus_open).
    bool context_menus() const;
    void open_menu(float x, float y, std::vector<MenuRow> rows) {
        if (!context_menus()) return;
        menus.clear();
        GameMenu m;
        m.x = x, m.y = y, m.rows = std::move(rows);
        menus.push_back(std::move(m));
    }
    // The connection is gone (a camp, a logout, a link death): the open menus and dialogs go with it, so no handler
    // of theirs runs against the next session.
    void drop_session_dialogs() {
        menus.clear();
        quantity.reset();
        text_entry.reset();
        confirm.reset();
        color_pick.reset();
    }
    // A menu row that opens the window's own menu (the stock "Window" submenu: CContextMenuManager's default menu,
    // which this client's window menu stands for): sidl.lua opens it for `window_menu` (eq.TakeWindowMenu).
    std::string window_menu;
    // CLOSE_TOP_WINDOW (Escape) went down and no window of the client's own took it: the UI pack may close one of
    // its windows (eq.TakeCloseTopWindow, once). Cleared after each UI frame, so a press nothing took is not kept.
    bool close_top_window = false;
    // CHelpWnd::SetFile(page) and Activate: the help window on help\\<page> (a tip page's WndNotify 1000 link).
    std::function<void(const std::string& page)> open_help;
    // The help window's Tips button: CTipWnd::Activate(pinstCTipWnd_1, -1, 1) (EqApi::activate_tip_of_day).
    std::function<void()> show_tip_of_day;
    // CTipWnd::Activate(tip, 0) on the context tip window (EqApi::activate_tip), for the windows that show one.
    std::function<void(int tip)> activate_tip;
    // A window's Activate on another stock window (the inventory's Face button: CPlayerCustomizationWnd).
    std::function<void(std::string_view screen)> show_window;
    // The layout copy window (app/lua_eq_layout.cpp, EqApi's __CopyLayout): the layouts there are to copy
    // (a UI ini and the resolution its keys are stored for: "1280x1024", "Windowed"), and the copy, which answers
    // the eqstr of a refusal and its arguments (none: copied).
    struct LayoutSource {
        std::filesystem::path file;
        std::string label, mode;
        std::filesystem::path character_dir; // the character's folder (core/character_files.h); empty: defaults.ini
        std::string world;                   // its world's folder, when this server has characters on several
    };
    // What is copied with the window places: the original's three boxes, and Hailstone's two for the files this
    // client keeps outside the UI ini (chat.ini's filters and colours, options.ini's game options).
    struct LayoutCopyParts {
        bool hot_buttons = false, loadouts = false, socials = false;
        bool chat = false, options = false;
    };
    std::function<std::vector<LayoutSource>()> layout_sources;
    // Hailstone: the copy also brings the UI's window layout (the character's ui-<pack>.txt keys, LuaUi::
    // copy_character_layout) from that character's folder; an empty folder (defaults.ini) the shared layout.
    std::function<void(const std::filesystem::path& character_dir)> copy_ui_layout;
    std::function<std::optional<std::pair<std::uint32_t, std::vector<std::string>>>(
        const LayoutSource& from, const LayoutCopyParts& parts)> copy_layout;
    // The server select screen (eqmain's serverselect, app/lua_eq_login.cpp) while the client shows it: the client's
    // view of the world list, the world played last on this login server (empty: none), and what the window asked
    // for (ui::ServerSelectAction as an int; the client takes it).
    ui::ServerSelectView* server_select = nullptr;
    std::string last_server;
    std::optional<int> server_select_action;
    // Hailstone: the client's own login and character select screens drawn by a UI pack (app/lua_eq_pregame.cpp):
    // their views while the client shows them, and what the pack's window asked for (ui::LoginAction and
    // ui::CharacterSelectAction as ints; the client takes them). login_server: the saved server's name after the
    // login (server and character select).
    ui::LoginView* login = nullptr;
    std::optional<int> login_action;
    ui::CharacterSelectView* char_select = nullptr;
    std::optional<int> char_select_action;
    // Hailstone: the client's own character creation (ui::character_create_screen) drawn by a UI pack, the same way:
    // its view while the client shows it, and what the pack's window asked for (ui::CharacterCreateAction as an int).
    ui::CharacterCreateView* char_create_view = nullptr;
    std::optional<int> char_create_action;
    std::string login_server;
    // The character creation (CCharacterCreation, app/lua_eq_login.cpp) while the client shows it, and its two ends:
    // the name and character sent for approval (Send_OP_ApproveName), and Cancel (back to the character list).
    bool char_create_open = false;
    std::function<void(const std::string& name, const net::CharCreate& request)> char_create_submit;
    std::function<void()> char_create_cancel;
    // Character select and creation (GameState 1 / 2): the model player the scene shows (null: none; app/
    // pre_game_scene.h), the model a spawn is drawn with, and what creation keeps of the model's look (the window
    // character's copy, game/char_create_look.h). The customization window works on the model player there.
    game::Spawn* model_player = nullptr;
    std::function<std::shared_ptr<const assets::CharacterModel>(const game::Spawn&)> model_for;
    game::CharCreateLook* char_create_look = nullptr;
    // Bug reports (app/bug_report.h). The screenshot for one, taken this frame (the bug report window hides for it);
    // a report saved with the client's own facts, logs and screenshots added (nullopt: it could not be written). The
    // report is written on a thread of its own (its logs run to tens of MB): the future is ready once it is.
    // `bug_report_now` fills a report at its Send (the facts: zone, place, target; the screenshot's bytes), so a
    // Send that waits its turn to be written still says what was so when it was made (null: save_bug_report does).
    std::function<void()> bug_report_capture;
    std::function<void(BugReport& report)> bug_report_now;
    std::function<std::future<std::optional<SavedBugReport>>(BugReport report)> save_bug_report;
    // The report made at this start from the last run's crash (null: none, or the player closed its window), a note
    // added to it (the report is written again), and the window closed.
    const SavedBugReport* crash_report = nullptr;
    std::function<void(const std::string& note)> crash_report_note;
    std::function<void()> crash_report_close;
};

// CQuantityWnd::Open (game/quantity_dialog.h) for a window: accepted at once calls `done`, else shows the dialog.
void open_quantity(EqHost& host, const game::QuantityOpen& in, std::function<void(std::int32_t)> done,
                   std::function<bool()> owner_valid = {});

class EqApi {
public:
    explicit EqApi(EqHost& host);
    ~EqApi();
    EqApi(const EqApi&) = delete;
    EqApi& operator=(const EqApi&) = delete;

    // Adds the functions to the `eq` table on the top of the stack.
    void register_functions(lua_State* L);
    // Once per frame before the scripts run (frame count, per-frame caches).
    void begin_frame();

    // CTipWnd::Activate(tip, 0) on the context tip window (help\\tips\\ctx*.html; the table at 0xb503ac): false when
    // its options keep it hidden.
    bool activate_tip(int tip);
    // CTipWnd::Activate(-1, force) on the tip of the day window (help\\tips\\totd.txt; at each zone-in, and the help
    // window's Tips button with `force`).
    bool activate_tip_of_day(bool force);
    // StockWindow::activate on a window (the story window's ShowAuto: CDisplay::InitGameUI).
    bool activate_window(std::string_view screen, int arg);
    // A click on spell gem `gem` (0-based) of the spell bar (CCastSpellWnd::HandleSpellLeftClick / RightClick).
    bool click_gem(int gem, bool right);
    // Whether a Lua window stands in for the stock window `screen` (its script asked for it with eq.ReplaceWindow
    // during the last frames), so the matching temporary C++ window stays off.
    bool replaces(std::string_view screen) const;
    // The stock window's open state, as the game's show / hide calls change it (keys, buttons).
    bool shown(std::string_view screen) const;
    void show(std::string_view screen, bool show);
    void toggle(std::string_view screen);
    // A right click on an inventory item: a bag opens or closes its container window, anything else is inspected.
    void right_click_item(const net::ItemSlot& slot);
    // Hailstone's menu on another player at x, y (app::open_player_menu); false when it did not open.
    bool open_player_menu(std::uint32_t spawn_id, float x, float y);
    // The OPEN_INV_BAGS / CLOSE_INV_BAGS / TOGGLE_INV_BAGS keys: every bag in the general slots opens or closes.
    void open_all_bags();
    void close_all_bags();
    void toggle_all_bags();
    // The CMD_TOGGLE_BANK_BAGS key: the bank's and the shared bank's bags open (the bank window showing) or close.
    void toggle_all_bank_bags();
    // The MAIN / ABILITIES / COMBAT / SOCIALS keys: the Actions window on that page, closed when already showing it.
    void actions_key(int page);
    // A left click on a child of a stock window, as the Lua window's click would make it (testing aids).
    // `button` as StockWindow::click takes it ("left", "rightup", "rightheld", ...).
    bool click_child(std::string_view screen, std::string_view child, std::string_view button = "left");
    // A hot bar button's click (CHotButtonWnd: the cursor's hot button goes there, else it is pressed; "right" and
    // "rightheld" as StockWindow::click_hot_button takes them; testing).
    bool click_hot_button(std::string_view screen, std::string_view child, std::string_view button = "left");
    // A left click on row `row` (column `column`) of a stock window's list (testing aids).
    bool click_list(std::string_view screen, std::string_view child, int row, int column);
    // The same with the keys held (1 shift, 2 ctrl) on a list that selects several rows (testing aids).
    bool select_list(std::string_view screen, std::string_view child, int row, int mods);
    bool double_click_list(std::string_view screen, std::string_view child, int row);
    bool header_click_list(std::string_view screen, std::string_view child, int column);
    // A combo box's choice picked, and a list's rows (all their texts) (testing aids).
    bool select_combo(std::string_view screen, std::string_view child, int index);
    std::vector<std::vector<std::string>> list_rows(std::string_view screen, std::string_view child);
    // Typing `text` into an edit box of a stock window, and the text / checked state the window gives a child
    // (testing aids).
    bool set_child_text(std::string_view screen, std::string_view child, const std::string& text);
    std::optional<std::string> child_text(std::string_view screen, std::string_view child);
    std::optional<bool> child_checked(std::string_view screen, std::string_view child);
    std::optional<bool> child_shown(std::string_view screen, std::string_view child);
    // The enabled state the window gives a child (nullopt: the XML's), and a link clicked in an STML child (testing
    // aids).
    std::optional<bool> child_state(std::string_view screen, std::string_view child);
    bool stml_link(std::string_view screen, std::string_view child, int message, std::string_view data);
    // The rows' first texts of a stock window's list (testing aids).
    std::vector<std::string> list_texts(std::string_view screen, std::string_view child);
    // A right click on a stock window at x, y (its context menu), and a click on row `row` of open context menu
    // `menu` (0-based; a submenu row opens its submenu) (testing aids).
    bool context_menu(std::string_view screen, float x, float y);
    bool menu_click(int menu, int row);
    // A setting a stock window keeps (StockWindow::setting: the pet window's "popup"), nullopt when the window has
    // no such setting; and setting it (false when it has none).
    std::optional<int> window_setting(std::string_view screen, std::string_view name);
    bool set_window_setting(std::string_view screen, std::string_view name, int value);
    // A tab box's page on a stock window (COptionsWnd_SetTabPage: /filter's chat page 4).
    void set_tab_page(std::string_view screen, std::string_view child, int page);
    // /map (Cmd_map) on the map window: the eqstr lines it says (none while the window is closed).
    std::vector<std::uint32_t> map_command(std::string_view args);
    // /note <text> (Cmd_note_AddNote): false when it failed (13224).
    bool add_player_note(std::string_view text);
    // Hovers row `item` of open EQ menu `menu` (0-based; testing aids).
    void cascade_hover(int menu, int item);
    // What was read from the game files once (the EQ menu's CascadeMenu.txt) is read again when next needed: the
    // layers changed (another server's assets folder).
    void reload_game_files();

    // The window places of a character (ui/ui_ini.h): the game's defaults.ini under the character's UI.ini (`file`,
    // in the character's folder, core/character_files.h; `seed`, the game's UI_<name>_<server>.ini, is read while
    // `file` does not exist). The windows are placed again and take their stored shown states; the windows' own
    // files (chat.ini, AddressBook.txt) are read from the folder of `file`.
    void load_layout(const std::filesystem::path& defaults, const std::filesystem::path& file,
                     const std::filesystem::path& seed);
    // Writes the character's file when a window moved or was shown or hidden.
    void save_layout();
    // The character's UI ini load_layout read (empty before one).
    const std::filesystem::path& layout_file() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// The chat keys (game/chat_input.h; app/lua_eq_chat.cpp): the active chat window's input takes the keyboard with
// `text` added (eq.ChatTakeFocus), and the chat buffer is cleared (slots 479 / 480).
void request_chat_focus(const std::string& text);
// Hailstone: the chat's lines to the character's chat_history.txt (what the windows keep, each its last 1000; on
// camping, quitting, going back to the login and every few minutes). `ending`: the session's lines go now, so the next entry into the world puts them
// back (chat_layout_loaded) instead of keeping them.
void save_chat_history(bool ending);
// The loot window's Link All (CLootWnd_LinkItemToChat): these item tags into the active chat window's input, which
// sends each line that fills up (eq.ChatTakeFocus).
void request_chat_link_all(std::vector<std::string> tags);
// The REPLY key: the active chat window's input holds "/tell <the last tell's player> " and takes the keyboard.
void reply_to_last_tell();
// The active chat window's input holds `text` instead of what was typed and takes the keyboard (the TELL,
// CMDMODE_SAY and CMDMODE_EMOTE keys).
void put_chat_input(const std::string& text);
// The chat keys of ClientApp::key_command (app/lua_eq_chat.cpp): TELL ("/tell "), RTARGET ("/rtarget <the player at
// the reply index>"), HISTORY_UP / HISTORY_DOWN (a line sent before into the input) and PAGEUP / PAGEDOWN (the
// active window's output scrolls a page; dir -1 up, 1 down).
void tell_key();
void reply_target_key();
void chat_history_key(bool older);
void chat_page_key(int dir);
void clear_chat_buffer();
// /chatfontsize: the active chat window's font (0..10; false without one). /clearchat, /clearallchat: the active
// window's lines, or every window's, are not shown again.
bool set_active_chat_font(int font);
// /channel: the active chat window's channel for plain typed text (CChatWindow +0x22c: 8 say, 7 a tell to `name`,
// 0x14 chat channel `name`, `index` in the joined list, -1 when not joined, ...).
bool set_active_chat_default(int channel, const std::string& name = {}, int index = -1);
// /report: the chat windows' visible text (CChatWindowManager::GetAllVisibleText).
std::string chat_visible_text();
// The character's UI ini the chat windows keep [ChatManager] in (null before one is loaded).
ui::UiIni* chat_ui_ini();
void clear_chat_windows(bool all);
// One of the player's abilities as the Ability Book sees it, for MQ's Me.AbilityReady and the rest
// (app/lua_eq_abilitybook.cpp): by its name (a discipline's also without its " Rk. II"), an AA's group or a skill's
// number. nullopt when the player has no such ability (a passive AA counts as none).
std::optional<mq::AbilityState> ability_state(EqHost& h, mq::AbilityKind kind, std::string_view name);
// The character's autoskill list in order, with each ability's name ("" for one the player no longer has), for
// scripts (hailstone.autoskill_list).
std::vector<std::pair<game::AutoskillRule, std::string>> autoskill_list(EqHost& h);
// Uses one of the player's abilities as its hot button does (the Ability Book's path; hailstone.use_ability): false
// when the player has no such ability or is not in the world. Readiness is the server's and the game's to refuse.
bool use_ability(EqHost& h, mq::AbilityKind kind, std::string_view name);

} // namespace hailstone::app
