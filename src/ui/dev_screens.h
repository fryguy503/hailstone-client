#pragma once

// Developer screens drawn with Dear ImGui until the stock UI engine (M5).
// Each screen draws from a view model and returns what the user asked for;
// none of them does networking. The view models also hold the UI-side state
// (text being typed, the selected row), so keep them alive across frames.
// Call these between ImGuiLayer::new_frame() and ImGuiLayer::render().

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/server_profile.h"
#include "game/item_links.h"
#include "platform/credentials.h"
#include "platform/gamepad.h"
#include "ui/screen_pictures.h"
#include "ui/ui_packs.h"

namespace hailstone { class Config; }

namespace hailstone::ui {

// The gamepad on the pre-game screens (a client extension, docs/gamepad.md), set by the app each frame. While
// ImGui's gamepad navigation is on (ImGuiLayer::set_pad_navigation), the screens show the pad's buttons, the
// navigation cursor starts on the screen's main control, Start does the screen's main action (Login, Play, Enter
// World, Create), B goes back, and a text field activated with the pad opens an on-screen keyboard.
void set_screen_gamepad(bool connected, platform::PadType type);

// Status line shared by the pre-game screens.
struct StatusLine {
    std::string text;
    bool is_error = false;
};

// ---- Login ----------------------------------------------------------------

// A game folder the login screen offers (hailstone.ini assets.path and assets.recent), with what its check found.
struct GameFolder {
    std::string path;
    bool live = false;   // a current live EverQuest install, else RoF2 (AssetFolderCheck::live)
    bool usable = true;  // it passed the check (a folder used before may be gone or changed since)
};

// What the updater (app/updater.h, docs/updater.md) has for the login screen. A Hailstone screen: the original had
// a patcher before the client, not an update notice in it.
struct UpdateNotice {
    std::string state;   // "available", "downloading", "ready", "failed"; "" nothing to offer
    std::string version; // the new release
    std::string current; // this build's
    std::string notes;   // the release's page ("" none)
    std::string error;   // failed: why
    std::string message; // a note of its own (an update rolled back at start), shown with any state
    bool has_package = false;      // available: it can be downloaded here (else from its page, by hand)
    bool install_on_quit = true;   // ready: installed when the client quits
    bool can_restart = false;      // ready: "Restart now" can go (no box set window is running)
    float progress = -1.0f;        // downloading: 0..1, -1 unknown
    // What the corner of the login screen says about this build (update_status_text): "off" (no updates here,
    // off_reason why), "checking", "uptodate", "newer" (state says what is under way), "unknown" (the check failed).
    std::string check = "off";
    std::string off_reason;
};
// The notice's sentence ("" nothing to say), as every login screen shows it.
std::string update_notice_text(const UpdateNotice& notice);
// The corner's short line about this build: "Up to date", "Checking for updates...", "Version 0.5.0 is out" ...
std::string update_status_text(const UpdateNotice& notice);

// The add-on manager's (app/addon_manager.h, docs/addons.md) part of the login screen, a Hailstone screen: a GitHub
// link pasted in, the install card of the add-on it names, and the list of scripts and plugins with what each can do.
struct AddonOfferView {
    std::string name, kind, version, author, description; // kind: "script", "plugin" or "tool"
    std::string source, page, notes;                      // owner/repo, its page, its release notes
    std::string key;               // the signing key's first 16 hex digits ("" not signed)
    std::string installed_version; // "" a new add-on; else this replaces that version
    bool older = false;            // older than installed_version: AddonInstall needs choice_on (the player's yes)
    std::string blocked;           // why it cannot be installed ("" it can)
    std::vector<std::string> caps; // what it asks to do, in plain words
    bool autoload = false;        // managed Lua addon: activate automatically when the rules allow it
};
// A suggested add-on (app/addon_suggest.h, docs/addons.md "Suggested add-ons"): the picked server has an add-on of
// its own the player does not have.
struct AddonSuggestionView {
    std::string name;   // "" a server's suggestion (only its source is known)
    std::string source; // owner/repo: what AddonSuggestLook, AddonSuggestLater and AddonSuggestNever are for
    std::string text;   // the sentence the screen shows
    bool from_server = false;
};
struct AddonRowView {
    std::string name, kind;   // kind: "script", "plugin" or "tool"
    std::string group;        // "installed" (by the manager), "manual" (by hand), "server" (the picked server's own)
    std::string version, source, page, author, description;
    std::string state;        // installed: app/addon_manager.h addon_state_word; else ""
    std::string state_text;   // a sentence for the player ("" nothing to say)
    std::string new_version;  // a newer version found
    std::vector<std::string> caps;     // what it may do, in plain words (installed: its ceiling)
    std::vector<std::string> new_caps; // what a newer version would add (state "approval")
    std::vector<std::string> modified; // the files the player changed
    std::string note;         // what this server does with it ("" nothing)
    // The server's compiled plugin: the player's word on it (plugin/consent.h): "allowed", "denied", "unasked";
    // "" for every other row.
    std::string consent;
    bool auto_update = false;
    bool signed_ = false;
    bool can_update = false;  // Update / Approve / Overwrite can go
    // What Update / Approve takes on the player's word: at most these capabilities (the ones shown), this version.
    std::uint32_t approve_caps = 0;
    std::string approve_version;
    bool can_remove = false;  // installed and not busy (the server's own never)
    bool can_start = false;   // an installed tool whose program is there: AddonStart starts it
    std::string folder;       // its folder or file, to open
    bool managed = false;    // installed Lua addon whose lifecycle the add-on manager owns
    bool enabled = false;    // the player's activation choice, separate from automatic updates
};
// An add-on of the add-on index, in the store (docs/addons.md "Finding add-ons"): what its manifest says, and what the
// player can do with it.
struct AddonStoreItemView {
    std::string name, source, page;     // the index's name, owner/repo and the repository's page
    std::string kind, version, author, description; // from its manifest ("" not fetched)
    std::string worlds;                 // the servers it is made for, as the index names them ("" any server)
    std::string installed_version;      // the version installed ("" none)
    // "install", "update" (a newer version than the installed one), "installed", "manual" (one of that name is in
    // the player's folders), "unavailable" (problem says why)
    std::string state;
    std::string problem;
};
struct AddonsView {
    bool open = false;       // the add-ons panel is shown (kept here, so a UI pack reload keeps it)
    bool available = false;  // links can be installed here (else the list only, and why_off says why)
    std::string why_off;
    std::string link;        // the link field, as typed
    bool inspecting = false; // a link's manifest is being fetched
    StatusLine status;
    std::optional<AddonOfferView> offer;
    std::vector<AddonRowView> rows; // installed, then by hand, then the server's
    std::string server;             // the picked server's name ("" none picked)
    std::vector<AddonSuggestionView> suggestions; // the picked server's suggested add-ons the player does not have
    std::string choice;             // the row an action is for (its name); a suggestion's or store item's source
    bool choice_on = false;         // AddonAuto: on or off; AddonBrowse: fetch the list again; AddonInstall: an
                                    // older version than the installed one is meant
    // The store: every add-on the add-on index lists (AddonBrowse fetches their manifests).
    bool browsing = false;          // being fetched
    bool browsed = false;           // fetched this run (store_error: why it is empty)
    std::string store_error;
    std::vector<AddonStoreItemView> store;
};

// The fields are the picked saved server's (core/server_profile.h), as the player edits them; Save or Login
// writes them back to its servers/<id>/server.ini.
struct LoginView {
    std::filesystem::path servers_dir;   // where the profiles are (platform::servers_dir(); tests use their own)
    std::vector<ServerProfile> profiles; // the saved servers, as list_profiles() sorts them
    int selected = -1;                   // index into profiles; -1 = a new server, not saved yet
    std::string name;
    std::string host;                    // empty for a new server: the player types its address
    int port = 5997;
    std::string username;
    std::string password;  // never in a file: kept only in the system keyring, with save_password
    bool remember = true;  // remember the username and keep it in the server's accounts
    bool save_password = false; // keep the password in the system keyring (with remember)
    // The picked server's saved accounts (ServerProfile::accounts) as the player edits them: the box set's ticks
    // and characters. Written with the server.
    std::vector<SavedAccount> accounts;
    // The picked server's worlds for the accounts' World lists: the ones its login server listed last
    // (listed_worlds()), then any other its accounts' characters are on.
    std::vector<std::string> worlds;
    // The system keyring the passwords go to; null or not available(): none are saved and there is no box set.
    platform::CredentialStore* credentials = nullptr;
    std::string account_choice; // ForgetAccount: whose
    // The music of the login, server and character select screens (hailstone.ini audio.pregame_music); toggled:
    // MusicChanged.
    bool pregame_music = true;
    // The Characters list's picked row (quick_characters()), -1 none; Play returns PlayCharacter.
    int quick_selected = -1;
    // The user interface used on this server (ServerProfile::ui): one of ui_packs, by id. A server without its own
    // (and a new one) shows default_ui, hailstone.ini ui.name.
    std::vector<UiPack> ui_packs;
    std::string default_ui = "modern";
    std::string ui = "modern";
    // A test login's server (--host/--port): no profile is picked or saved, and the profile controls are hidden.
    bool transient = false;
    // The game files every server's own files go over: the folder in use first, then the ones used before.
    // Picking one returns UseGameFolder with game_choice set; Browse returns BrowseGameFolder (the picker).
    // game_folder_fixed: --game-dir / EQ_GAME_DIR set it, so the row only shows it.
    std::vector<GameFolder> game_folders;
    bool game_folder_fixed = false;
    std::string game_choice;
    StatusLine status;
    bool busy = false; // request in flight: inputs disabled, spinner shown
    bool session_ticket_available = false; // the app holds a returned ticket for this login endpoint
    UpdateNotice update; // the updater's news; its buttons return the Update* actions
    AddonsView addons;   // the add-on manager's; its buttons return the Addon* actions
    // This build ("0.2.0-alpha (39b3e9da)", core/build_info.h build_version()), shown small on every login screen
    // so a player can say which version they run; "" shows nothing.
    std::string version;
    // This release's part of CHANGELOG.md (core/changelog.h), one entry a line, for What's new; and Hailstone's
    // page, which OpenProject opens.
    std::vector<std::string> release_notes;
    std::string project_page;
};

// Save, Delete (asked again first) and OpenAssets are for the picked server; picking another and New are done
// in the view (select_profile, new_profile). UseGameFolder and BrowseGameFolder change the game files.
// LoginBox logs in the box set (box_accounts()); AccountsChanged: a box tick or character was edited (save the
// server quietly); ForgetAccount: account_choice is to be forgotten (forget_account()); PlayCharacter: log in to
// quick_characters()[quick_selected]'s account, play its world and enter it. UpdateDownload downloads the release
// update.version (or tries a failed download again), UpdateRestart installs it and starts it, UpdateSkip does not
// offer it again, UpdateNotes opens its page; OpenProject opens Hailstone's page (project_page). The Addon* actions are the add-on manager's (AddonsView): AddonInspect
// fetches the link's manifest, AddonInstall says yes to the offer, AddonCancel no; AddonUpdate, AddonRemove,
// AddonAuto (choice_on), AddonFolder and AddonPage are for the row addons.choice ("" for AddonPage: the offer's),
// AddonCheck looks for newer versions now. AddonRevoke takes back the player's yes to the server's compiled plugin
// addons.choice, AddonAskAgain forgets the answer so that the next visit asks (plugin/consent.h). The AddonSuggest*
// actions are for the suggestion whose source is addons.choice: Look opens its install card, Later puts it away for
// this run, Never for good on this server. AddonBrowse fetches the store's list (again with choice_on);
// AddonStoreLook opens the install card of the store item whose source is addons.choice; AddonStart starts the
// installed tool addons.choice. ResumeToken asks the app to consume its returned ticket for this endpoint.
enum class LoginAction {
    None, Submit, Quit, Save, Delete, OpenAssets, UseGameFolder, BrowseGameFolder, LoginBox, AccountsChanged,
    ForgetAccount, PlayCharacter, MusicChanged,
    UpdateDownload, UpdateRestart, UpdateSkip, UpdateNotes, OpenProject,
    AddonInspect, AddonInstall, AddonCancel, AddonUpdate, AddonRemove, AddonAuto, AddonFolder, AddonPage, AddonCheck,
    AddonRevoke, AddonAskAgain, AddonSuggestLook, AddonSuggestLater, AddonSuggestNever, AddonBrowse, AddonStoreLook,
    AddonStart, ResumeToken, AddonEnabled
};

// A row of the login screen's Characters list: a character one of the server's accounts was seen to have.
struct QuickCharacter {
    std::string username;
    SavedCharacter character;
    bool password_saved = false;
    bool last = false; // the one entered last on the account last used
};

// "Live" or "RoF2", and the folder's name, as the Game files row shows a folder.
std::string game_folder_label(const GameFolder& folder);

// Enter in any field submits. Submit is only returned with a username and password.
LoginAction login_screen(LoginView& view);

// The picked profile's id, or "" for a new server or a test login.
std::string selected_profile_id(const LoginView& view);
// The picked server's override folder, servers/<id>/assets (empty for a new server or a test login).
std::filesystem::path selected_assets_dir(const LoginView& view);
// Picks profiles[index] (anything else: a new server) and shows its fields; the username only when remembered,
// with its saved password from the keyring.
void select_profile(LoginView& view, int index);
// Puts a saved account in the fields: its username, and its password when one is saved (else none).
void pick_account(LoginView& view, std::string_view username);
// The picked server's characters, account by account, for the Characters list.
std::vector<QuickCharacter> quick_characters(const LoginView& view);
// The row to pick at first: the character entered last on the account last used, else the first (-1: none).
int default_quick_character(const LoginView& view);
// Whether the view's keyring can keep passwords.
bool can_save_passwords(const LoginView& view);
// The box set: the accounts ticked, in the list's order.
std::vector<SavedAccount> box_accounts(const LoginView& view);
// Whether every account of the box set has a saved password (and there is one): Log in all can go.
bool box_ready(const LoginView& view);
// The picked server's address as the keyring knows it (platform::credential_server).
std::string credential_server(const LoginView& view);
// After a login the server took: keeps the typed password in the keyring when the account is to save it. False
// (with the status set) when the keyring refused.
bool save_login_password(LoginView& view);
// Takes the account out of the list and its password out of the keyring, then saves the server; false (with the
// status set) when that cannot be written.
bool forget_account(LoginView& view, Config& config, std::string_view username);
// A new server with the default fields.
void new_profile(LoginView& view);
// Picks the profile with this id, else this name (both any case), for --server; false when there is none.
bool select_profile_named(LoginView& view, std::string_view id_or_name);
// A launcher ticket's login (app/login_launch.h): the view shows its server (the saved one with profile's id, else
// a transient one) with no account in the fields, and keeps nothing (remember and save_password off).
void begin_launcher_login_view(LoginView& view, const ServerProfile& profile);
// The login screen again after a ticket's login, failed or ended: the normal view, not transient, with a saved
// server's remember, account and saved password as select_profile shows them. The saved server is profile_id's;
// else, given `endpoint`, that server as a new one (not saved until a login), so that Sign in with session still
// finds it; else the first saved server.
void end_launcher_login_view(LoginView& view, std::string_view profile_id, const ServerProfile* endpoint);
// The view's server as a profile: the picked one's id and NMS keys with the edited fields, or for a new server an
// id made from its name (the host when it has none). A test login's has no id. An empty name becomes the host.
ServerProfile profile_from_view(const LoginView& view);

// Lists the profiles in `servers`, takes default_ui from ui.name and picks login.profile (else the first, else a
// new server). When there are none, the old login.host, login.port and login.username keys become a first profile
// and are erased; returns true when `config` changed and needs saving. Still none and `eqhost` (the asset folder's
// eqhost.txt) names a login server: that becomes the first profile (profile_from_eqhost_file), as a fresh install's
// default. Only once: login.eqhost_imported is set then, or when an eqhost.txt is there while the player already has
// a saved server, so a player who deletes every server does not get it back.
bool load_login_settings(LoginView& view, Config& config,
                         const std::filesystem::path& servers = platform::servers_dir(),
                         const std::filesystem::path& eqhost = {});
// Saves the view's server (its server.ini and assets folder), lists the profiles again, picks it and names it in
// login.profile. Nullopt (with the status set) when it cannot be written; test logins are never saved.
std::optional<ServerProfile> store_login_settings(LoginView& view, Config& config);
// Deletes the picked server (its folder servers/<id>/, with its assets and characters' files, goes to
// servers/.deleted/: remove_profile()) and picks another; its saved passwords leave the keyring.
bool delete_login_profile(LoginView& view, Config& config);

// ---- Server select --------------------------------------------------------

struct ServerEntry {
    std::string name;
    std::string status; // "Up", "Locked", "Down", ...
    bool can_join = true;
    bool favorite = false; // kept per login server (hailstone.ini login.favorite_worlds.<profile>)
    // No player count: no UI shows one (the login server's figure stays in net::ServerInfo).
};

struct ServerSelectView {
    std::vector<ServerEntry> servers;
    int selected = -1;
    StatusLine status;
    bool busy = false;
    int favorite_row = -1; // the row whose favourite mark Favorite turns over
    // A world listed has an add-on the player does not have (docs/addons.md "Suggested add-ons"): a sentence that
    // says so and where it is installed (the login screen's Add-ons); "" none.
    std::string addon_hint;
};

enum class ServerSelectAction { None, Play, Back, Favorite };

// The order the server select lists the login server's worlds in (indices into `names`): the favourites first, then
// the others, each group by name ignoring case (ties keep the login server's order). A Hailstone screen: the
// original lists the worlds as the login server sends them.
std::vector<std::size_t> server_list_order(const std::vector<std::string>& names,
                                           const std::vector<std::string>& favorites);
// The favourite worlds as hailstone.ini keeps them: the names joined by '|' (no world name holds one).
std::vector<std::string> parse_favorite_worlds(std::string_view text);
std::string join_favorite_worlds(const std::vector<std::string>& names);

// Play (button, double-click or Enter) is returned for view.selected.
ServerSelectAction server_select_screen(ServerSelectView& view);

// ---- Character select -----------------------------------------------------

struct CharacterEntry {
    std::string name;
    int level = 1;
    std::uint32_t race = 0;
    std::vector<std::uint32_t> classes; // NMS is multiclass: up to three
    std::string zone;
    // Return Home is offered (CCharacterListWnd::EnableButtons: the list's go-home flag, and the character usable),
    // and the question asked first (eqstr 6137).
    bool can_go_home = false;
    std::string home_question;
};

struct CharacterSelectView {
    std::string server_name;
    std::vector<CharacterEntry> characters;
    int selected = -1;
    StatusLine status;
    bool busy = false;
    // The character select scene is drawn behind (the model in the middle of the screen): the dialog stands at the
    // left.
    bool scene = false;
};

enum class CharacterSelectAction { None, EnterWorld, Back, Logout, Create, Delete, ReturnHome };

CharacterSelectAction character_select_screen(CharacterSelectView& view);

// ---- Character creation -------------------------------------------------

// One choice the server allows; the app fills these from world's
// OP_CharacterCreateRequest tables (race, class, deity, start zone).
struct CreateOption {
    std::uint32_t race = 0, class_id = 0, deity = 0, start_zone = 0;
    // The combination's point allocation, STR DEX AGI STA INT WIS CHA: each stat's base and the points the class
    // puts in it by default (their sum is what the player spends).
    std::array<std::uint32_t, 7> base{}, points{};
};

struct CharacterCreateView {
    std::vector<CreateOption> options; // valid combinations
    std::string name;
    std::uint32_t race = 1, class_id = 1, gender = 0;
    int option = -1; // chosen row of options (deity/start zone) for race/class
    // The deity's name (the app's GetDeityDesc over the connection's strings); the id is shown when unset.
    std::function<std::string(std::uint32_t)> deity_name;
    // The stats as spent (STR..CHA) and the points still to spend, for the option stats_for.
    std::array<int, 7> stats{};
    int points_left = 0;
    int stats_for = -1;
    StatusLine status;
    bool busy = false;
    bool scene = false; // the model player is drawn behind: the dialog stands at the left, with an Appearance button
    bool name_pending = false; // a random name was asked for (OP_RandomNameGenerator) and has not come yet
};

// Appearance: the player customization window on the model (CC_Appearance_Button).
enum class CharacterCreateAction { None, Create, Cancel, Appearance };

// Create is returned with a name, a valid view.option and every point spent.
CharacterCreateAction character_create_screen(CharacterCreateView& view);

// The rules of the screen, shared with the UI packs' creation windows (app/lua_eq_pregame.cpp). create_normalize keeps
// race and class among the options and view.option on a row of them (the first of race and class when the current
// one is not), and resets the stats when the row changed. The lists: the races (by id), the race's classes (by id),
// the deities of race and class (by name), the start zones of race, class and the chosen deity (by name, as
// FillStartCityCombo). create_set_deity keeps the city when race, class, deity and city still combine, else takes the
// first that does (0x64bee0); create_set_city picks the row of that city. create_ready: Create can go.
void create_normalize(CharacterCreateView& view);
std::vector<std::uint32_t> create_races(const CharacterCreateView& view);
std::vector<std::uint32_t> create_classes(const CharacterCreateView& view);
std::vector<std::uint32_t> create_deities(const CharacterCreateView& view);
std::vector<std::uint32_t> create_cities(const CharacterCreateView& view);
std::string create_deity_label(const CharacterCreateView& view, std::uint32_t deity);
bool create_set_deity(CharacterCreateView& view, std::uint32_t deity);
bool create_set_city(CharacterCreateView& view, std::uint32_t zone);
bool create_ready(const CharacterCreateView& view);

// The stats of CCharacterCreation (CC_AdvancedScreen), as app/lua_eq_login.cpp's window keeps them:
// ResetStatPoints puts each stat at its base and the points to spend at the sum of the allocation's points; Up adds
// one while points are left, up to base + 25 and at most 150; Down takes one back, down to the base.
void create_reset_stats(CharacterCreateView& view);
int create_stat_cap(const CharacterCreateView& view, int stat);
bool create_stat_up(CharacterCreateView& view, int stat);
bool create_stat_down(CharacterCreateView& view, int stat);
// A right click on the stat's button (sub_0064d7f0): as many points as it can take.
bool create_stat_max(CharacterCreateView& view, int stat);

// ---- Loading --------------------------------------------------------------

struct LoadingView {
    std::string title = "Loading";
    std::string detail;     // e.g. "Entering Qeynos Hills"
    float progress = -1.0f; // 0..1, or < 0 for an indeterminate spinner
    bool waiting = true;    // false: the title alone, no progress or spinner (the disconnected screen)
    ScreenPicture picture;  // loading_overlay's backdrop (none: black); set by the app each frame
};

void loading_screen(const LoadingView& view);
// The same screen over everything else drawn this frame (an opaque black backdrop and the panel, in the foreground
// draw list): the world and its windows run beneath it while the zone settles (app/zone_settle.h).
void loading_overlay(const LoadingView& view);

// ---- In game --------------------------------------------------------------

struct HudView {
    std::string zone;
    float x = 0.0f, y = 0.0f, z = 0.0f; // EQ world coordinates
    float heading = 0.0f;               // degrees
    float fps = 0.0f;
    std::string connection; // "Connected", "Link dead", ...
    bool connected = true;
};

enum class ChatChannel : std::uint8_t {
    System, Say, Tell, Group, Guild, Raid, Shout, Auction, OOC, Emote, Combat, Spell, Error, Count
};

struct ChatLine {
    ChatChannel channel = ChatChannel::System;
    std::string text;
    std::uint32_t rgb = 0; // 0xRRGGBB overriding the channel colour (consider lines), 0 = none
    std::uint32_t color = 0; // the original's colour index (net::ChatEvent::color), 0 = the channel's
    std::string peer;        // the player the line is from (a tell's other player; net::ChatEvent::peer)
    std::uint32_t window = 0; // the chat window that wrote it (CChatWindow::AddOutputText; app/lua_eq_chat.cpp's
                              // serial), 0: routed by its colour
    bool restored = false;    // Hailstone: from the character's chat history (an earlier session; no tell windows)
    std::int64_t time = 0;    // Hailstone: when it came (std::time; 0 unknown), for the chat's timestamps
    std::uint64_t seq = 0;    // its place in ChatView::revision's count (1-based); lines leave from anywhere in the
                              // buffer (each chat window keeps its own last lines: app/lua_eq_chat.cpp)
};

struct ChatView {
    std::deque<ChatLine> lines;
    // Hailstone: each chat window keeps its own last 1000 lines (app/lua_eq_chat.cpp); this only bounds the whole
    // buffer (32 windows of them), for lines no window has taken yet.
    std::size_t max_lines = 32000;
    std::string input;
    bool scroll_to_bottom = true;
    // Set when the player clicks an item link in a line (lines keep the \x12 link tags).
    std::optional<game::ItemLink> clicked_link;
    std::uint64_t revision = 0; // counts added lines (the Lua chat window's eq.ChatText version)

    void add(ChatChannel channel, std::string text, std::uint32_t rgb = 0, std::uint32_t color = 0, std::string peer = {});
};

struct SpawnEntry {
    std::uint32_t id = 0;
    std::string name;
    int level = 0;
    float distance = 0.0f;
};

struct SpawnListView {
    std::vector<SpawnEntry> spawns;
    std::uint32_t selected_id = 0;
    std::string filter;
};

// The player window: name, level, HP / mana / endurance and the experience bar.
struct PlayerView {
    std::string name;
    int level = 0;
    std::int64_t hp = 0, max_hp = 0;
    std::uint32_t mana = 0, max_mana = 0;
    std::uint32_t endurance = 0, max_endurance = 0;
    std::uint32_t exp = 0; // 0..330 into the level
    bool sitting = false;
    bool attacking = false;
    bool dead = false;
};

// The target window; has_target false hides the details.
struct TargetView {
    bool has_target = false;
    std::string name;
    int level = 0;
    int hp_percent = 0;
    std::uint32_t con_rgb = 0xffffff; // consider colour of the name
    bool is_corpse = false;
    bool is_self = false;
    float distance = 0.0f;
};

// Respawn choices (hover death window); empty options hide it.
struct RespawnView {
    std::vector<std::pair<std::uint32_t, std::string>> options; // id, label
    std::uint32_t seconds_left = 0;
    bool ready = true; // CRespawnWnd: Select waits for the delay, and is disabled again once chosen
};

struct InGameView {
    HudView hud;
    ChatView chat;
    SpawnListView spawns;
    PlayerView player;
    TargetView target;
    RespawnView respawn;
    bool show_chat = true;
    bool show_spawns = true;
    // The developer windows (the info panel and the spawn list): off unless ui.dev_windows=1 in hailstone.ini
    // or Ctrl+Shift+D turns them on.
    bool dev_windows = false;
};

struct InGameActions {
    bool camp = false;
    bool disconnect = false;
    std::optional<std::string> chat_submit;    // a line typed into the chat input
    std::optional<std::uint32_t> target_spawn; // spawn id clicked in the spawn list
    bool toggle_attack = false;                // Attack button
    bool consider = false;                     // Consider button
    bool clear_target = false;
    bool toggle_sit = false;                   // Sit / Stand button
    std::optional<std::uint32_t> respawn;      // chosen respawn option
};

// Info panel (top left), chat window and spawn list.
InGameActions in_game_overlay(InGameView& view);

// The parts of the overlay, for screens that need only some of them.
void hud_panel(const HudView& view, bool& show_chat, bool& show_spawns, InGameActions& actions);
std::optional<std::string> chat_window(ChatView& view, bool* open = nullptr);
std::optional<std::uint32_t> spawn_list_window(SpawnListView& view, bool* open = nullptr);
void player_window(const PlayerView& view, InGameActions& actions);
void target_window(const TargetView& view, bool attacking, InGameActions& actions);
void respawn_window(const RespawnView& view, InGameActions& actions);

} // namespace hailstone::ui
