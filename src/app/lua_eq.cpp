// The `eq` functions (app/lua_eq.h). Names, arguments and returns follow the
// original UI scripts' contracts; each function's comment gives its contract. Window functions find the stock window model by
// screen name (app/lua_eq_windows.h); a window without a model is never shown, so its script draws nothing.

#include "app/lua_eq.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <cfloat>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/actions_ui.h"
#include "app/lua_eq_menu.h"
#include "app/lua_eq_values.h"
#include "app/lua_eq_windows.h"
#include "ui/lua/lua_host.h"
#include "app/lua_eq_caps.h"
#include "app/lua_eq_plugins.h"
#include "plugin/ui_bridge.h"
#include "core/ini_file.h"
#include "core/log.h"
#include "core/saturate.h"
#include "game/hot_button_state.h"
#include "game/item_sort.h"
#include "game/merchant.h"
#include "game/option_commands.h"
#include "game/mechanics/character.h"
#include "game/tradeskill.h"
#include "game/spell_gem.h"
#include "game/spells.h"
#include "game/world_state.h"
#include "net/game_connection.h"
#include "ui/ui_ini.h"
#include "ui/ui_textures.h"

namespace hailstone::app {

bool chat_window_used(int slot);  // app/lua_eq_chat.cpp: chat window 1..32 is open
void chat_window_closed(int slot); // its close button: the window goes with its tabs
// app/lua_eq_chat.cpp: the chat windows' settings, and the user colours from the character's chat.ini in `user_dir`.
void chat_layout_loaded(ui::UiIni& ini, EqHost& host, const std::filesystem::path& user_dir);
void chat_host_gone(const EqHost& host, const ui::UiIni& ini);

struct EqApi::Impl {
    EqHost& host;
    std::vector<std::unique_ptr<StockWindow>> windows;
    // `windows` by screen name (window()): the first window of a name, as a scan from the front finds it. Built again
    // whenever the vector has grown since (the views point into each window's own name, which never changes).
    std::unordered_map<std::string_view, StockWindow*> windows_by_name;
    std::size_t windows_indexed = 0;
    std::map<std::string, int, std::less<>> replaced_at; // screen -> frame of its last eq.ReplaceWindow
    int frame = 0;
    ScreenReplacements screens{&replaced_at, &frame}; // eq.LoginScreen .. (app/lua_eq_pregame.cpp)
    // The cursor's hotbutton: a left press made while it was held, and the frame such a press came up (a click
    // that put it nowhere: it is dropped the next frame, as a click on the world drops CCursorAttachment's button).
    bool pressed_holding = false;
    int stray_release = -10, stray_serial = 0;
    bool cursor_spell_memorizing = false; // a memorize was under way last frame while a book spell was on the cursor
    std::map<std::string, bool, std::less<>> unknown_logged;
    // Window places (ui/ui_ini.h): the layout generation and screen serial each window was placed under, its place
    // last frame.
    ui::UiIni layout;
    struct Placed {
        int generation = 0, screen = 0;
    };
    std::map<std::string, Placed, std::less<>> placed;
    std::map<std::string, ui::WindowIniInfo, std::less<>> ini_infos; // ui::window_ini_info by name (ini_info())
    // A window's place is kept only when the player moved or sized it: `seen` is where it was last frame (none just
    // after it was placed). A new screen size (the game window maximized, restored, made smaller) places every
    // window again from the UI ini, so a smaller screen never rewrites the places kept for the larger one.
    std::map<std::string, std::array<int, 4>, std::less<>> seen;
    int screen_serial = 0;
    ImVec2 screen{0, 0};
    std::map<std::string, std::pair<ui::WindowRect, bool>, std::less<>> last_place; // rect, sizable
    std::map<std::string, bool, std::less<>> last_shown;
    std::map<std::string, bool, std::less<>> tip_shown; // the OnShow context tips: each window's shown state last frame
    std::filesystem::path layout_defaults, layout_file, layout_seed; // load_layout's files
    // Where each Lua window was last frame (screen pixels): the EQ menu opens beside the EQ window.
    std::map<std::string, std::array<int, 4>, std::less<>> screen_rects;
    CascadeMenu cascade; // the EQ button's menu (app/lua_eq_menu.h)
    // Each item slot's CInvSlotWnd cool-down (CButtonWnd +0x21c / +0x220), by where the Lua asked for the item.
    std::map<std::string, game::ButtonCoolDown, std::less<>> slot_cool_downs;

    // CInvSlotWnd_Draw @0x69a080's recast cool-down for the slot `key` holding `item` (game::update_item_cool_down),
    // as the item table's `cooldown`: DrawCooldown's fraction (the shaded part runs from the slot's top + fraction x
    // its height down to its bottom), absent while idle. The table is at the top of the stack.
    void set_item_cool_down(lua_State* L, std::string key, const net::Item* item) {
        if (!item || !host.conn) return;
        HostButtonQueries q(host);
        game::ButtonCoolDown& cd = slot_cool_downs[key];
        game::update_item_cool_down(cd, item_recast_of(*host.conn, item), q.now(), q.fast_now());
        if (auto f = cd.overlay_fraction(q.now())) {
            lua_pushnumber(L, *f);
            lua_setfield(L, -2, "cooldown");
        }
    }

    // The windows whose OnShow (or AboutToShow) activates a context tip on the context tip window
    // (CTipWnd::Activate(pinstCTipWnd_2, id, 0)), checked as each one turns shown. The item displays pick theirs by
    // the item (CItemDisplayWnd::OnShow: an augmentation 0x21, else 0x59).
    void context_tips_on_show() {
        static constexpr std::pair<const char*, int> kOnShow[] = {
            {"InventoryWindow", 0}, {"SpellBookWnd", 1}, {"AAWindow", 2}, {"BodyTintWnd", 8},
            {"FriendsWindow", 0xb}, {"GuildManagementWnd", 0xd}, {"JournalTextWnd", 0xe}, {"SkillsWindow", 0xf},
            {"StoryWnd", 0x10}, {"MerchantWnd", 0x11}, {"BankWnd", 0x12}, {"BigBankWnd", 0x12}, {"TrainWindow", 0x13},
            {"GiveWnd", 0x14}, {"TradeWnd", 0x15}, {"LootWnd", 0x17}, {"AdventureStatsWnd", 0x24},
            {"AdventureLeaderboardWnd", 0x26}, {"AdventureRequestWnd", 0x27}, {"QuantityWnd", 0x28},
            {"LeadershipWindow", 0x29}, {"CombatAbilityWnd", 0x2e}, {"DynamicZoneWnd", 0x2f},
            {"PvPLeaderboardWnd", 0x31}, {"PvPStatsWnd", 0x32}, {"RaidWindow", 0x37}, {"TitleWnd", 0x39},
            {"NewPointMerchantWnd", 0x40}, {"TributeTrophyWnd", 0x50}, {"ZoneGuideWnd", 0x58},
            {"PlayerCustomizationWnd", 0xa}, {"ActionsWindow", -100},
            {"ItemDisplayWindow", -1}, {"ItemDisplayWindow#2", -2}, {"ItemDisplayWindow#3", -3},
            {"ItemDisplayWindow#4", -4}, {"ItemDisplayWindow#5", -5}, {"ItemDisplayWindow#6", -6},
        };
        for (auto [screen_name, tip] : kOnShow) {
            StockWindow* w = window(screen_name);
            if (!w) continue;
            bool shown = w->shown(host);
            bool& was = tip_shown[screen_name];
            if (shown && !was) {
                if (tip == -100) {
                    // CActionsWnd::OnShow @0x614a20 (0x614510 on the tab box's page): the Abilities page 4,
                    // Combat 3, Socials 5, the Main page none
                    auto page = w->tab_page(host, "ACTW_ActionsSubwindows");
                    tip = !page ? -1 : *page == 1 ? 4 : *page == 2 ? 3 : *page == 3 ? 5 : -1;
                    if (tip >= 0) activate_context_tip(tip, false);
                    was = shown;
                    continue;
                }
                if (tip < 0) {
                    std::size_t k = static_cast<std::size_t>(-tip - 1);
                    bool augment = host.item_displays && k < host.item_displays->size() &&
                                   (*host.item_displays)[k].item_type == 54;
                    tip = augment ? 0x21 : 0x59;
                }
                activate_context_tip(tip, false);
            }
            was = shown;
        }
    }
    // CLayoutCopyWnd's list (0x6b6580 / 0x6b6240): every UI ini (.\UI_*.ini; here every character's UI.ini
    // of this server, characters/<world>/<name>/UI.ini) and then defaults.ini, one row per screen size its sections
    // keep a place for (a key that starts with "XPos": the rest of the key), each pair once; not the current file's
    // own current size (0x55b4c0 / 0x55b560). This client always runs windowed ("Windowed").
    // Hailstone difference: the characters' files come newest first (the UI.ini written last: the character played
    // last), not in the original's file order, and name their world when the server has characters on several.
    std::vector<EqHost::LayoutSource> layout_sources() const {
        std::vector<EqHost::LayoutSource> out;
        auto scan = [&](const std::filesystem::path& file, const std::string& label, const std::filesystem::path& dir,
                        const std::string& world) {
            IniFile ini;
            if (!ini.load(file)) return;
            std::error_code ec;
            bool current = !layout_file.empty() && std::filesystem::equivalent(file, layout_file, ec);
            for (const std::string& section : ini.sections())
                for (const std::string& key : ini.keys(section)) {
                    if (key.size() < 4 || !game::same_no_case(std::string_view(key).substr(0, 4), "XPos")) continue;
                    std::string mode = key.substr(4);
                    if (current && game::same_no_case(mode, "Windowed")) continue;
                    bool listed = false;
                    for (const auto& o : out) listed = listed || (o.file == file && o.mode == mode);
                    if (!listed) out.push_back({file, label, mode, dir, world});
                }
        };
        if (!layout_file.empty() && layout_file.filename() == "UI.ini") {
            std::error_code ec;
            std::filesystem::path characters = layout_file.parent_path().parent_path().parent_path();
            struct Found {
                std::filesystem::path file;
                std::filesystem::file_time_type written;
            };
            std::vector<Found> files;
            std::set<std::string> worlds;
            for (const auto& world : std::filesystem::directory_iterator(characters, ec))
                for (const auto& who : std::filesystem::directory_iterator(world.path(), ec)) {
                    std::filesystem::path f = who.path() / "UI.ini";
                    if (!std::filesystem::exists(f, ec)) continue;
                    std::error_code tec;
                    auto written = std::filesystem::last_write_time(f, tec);
                    files.push_back({f, tec ? std::filesystem::file_time_type::min() : written});
                    worlds.insert(world.path().filename().string());
                }
            std::sort(files.begin(), files.end(), [](const Found& a, const Found& b) {
                return a.written != b.written ? a.written > b.written : a.file < b.file;
            });
            for (const auto& f : files) {
                std::filesystem::path dir = f.file.parent_path();
                scan(f.file, dir.filename().string(), dir,
                     worlds.size() > 1 ? dir.parent_path().filename().string() : std::string());
            }
        }
        if (!layout_defaults.empty()) scan(layout_defaults, "Defaults.ini", {}, {});
        return out;
    }
    // __CopyLayout @0x55be50: the current file and size onto
    // itself is refused (6337); a missing file says 6338; a file with no place for that size 6342. Otherwise every
    // window key kept for that size (XPos, YPos, Width, Height, Minimized and the Restore* keys + the size: the
    // table at 0xe63a98) goes to the current file for the current size, the size-free keys (Show, BGTint.*, BGType,
    // Fades, Delay, Duration, Alpha, FadeToAlpha, Locked, the Global* fades, PercentSize, ShowKeyMap, ShowSpinner:
    // 0xe63a50) as they are; from the source character's file, with the boxes, the sections HotButtons..HotButtons10
    // (then the hot buttons are read again), HBLoadouts and Socials (0x55b930). Then the windows are placed
    // again from the file (unk_ApplyWindowPositions). Not built: the skins (UISkin / AtlasSkin / MapSkin) and the
    // ViewPort section, which this client does not have, and the HB_<n> hot button files.
    // Hailstone: the Chat and Options boxes also copy the source character's chat.ini and options.ini, which hold
    // what the original keeps in the shared eqclient.ini (net::GameConnection::copy_character_settings).
    std::optional<std::pair<std::uint32_t, std::vector<std::string>>> copy_layout(const EqHost::LayoutSource& from,
                                                                              const EqHost::LayoutCopyParts& parts) {
        const bool hot = parts.hot_buttons, loadouts = parts.loadouts, socials = parts.socials;
        static constexpr std::array<std::pair<const char*, const char*>, 9> kSized = {{
            {"XPos", "XPos"}, {"YPos", "YPos"}, {"Width", "Width"}, {"Height", "Height"}, {"Minimized", "Minimized"},
            {"RestoreXPos", "RestoreXPos"}, {"RestoreYPos", "RestoreYPos"}, {"RestoreWidth", "RestoreWidth"},
            {"RestoreHeight", "RestoreHeight"},
        }};
        static constexpr const char* kPlain[] = {
            "Show", "BGTint.red", "BGTint.green", "BGTint.blue", "BGType", "Fades", "Delay", "Duration", "Alpha",
            "FadeToAlpha", "Locked", "GlobalFadeDuration", "GlobalFadeDelay", "GlobalAlpha", "GlobalFadeToAlpha",
            "PercentSize", "ShowKeyMap", "ShowSpinner",
        };
        std::error_code ec;
        bool current = !layout_file.empty() && std::filesystem::equivalent(from.file, layout_file, ec);
        if (current && game::same_no_case(from.mode, "Windowed")) return std::pair{6337u, std::vector<std::string>{}};
        IniFile ini;
        if (!ini.load(from.file)) return std::pair{6338u, std::vector<std::string>{from.label}};
        bool found = false;
        for (const std::string& section : ini.sections())
            for (const std::string& key : ini.keys(section))
                for (auto [prefix, target] : kSized) found = found || game::same_no_case(key, std::string(prefix) + from.mode);
        if (!found) return std::pair{6342u, std::vector<std::string>{from.mode, from.label}};
        layout.save();
        for (const std::string& section : ini.sections())
            for (const std::string& key : ini.keys(section)) {
                auto value = ini.get(section, key);
                if (!value || value->empty()) continue;
                for (auto [prefix, target] : kSized)
                    if (game::same_no_case(key, std::string(prefix) + from.mode)) layout.put(section, std::string(target) + "Windowed", *value);
                for (const char* plain : kPlain)
                    if (game::same_no_case(key, plain)) layout.put(section, plain, *value);
            }
        layout.save();
        if (host.actions && (hot || loadouts || socials)) {
            IniFile chars;
            chars.load(from.file.parent_path() / "hotbuttons.ini");
            if (hot)
                host.actions->copy_char_ini_sections(chars, {"HotButtons", "HotButtons2", "HotButtons3", "HotButtons4",
                    "HotButtons5", "HotButtons6", "HotButtons7", "HotButtons8", "HotButtons9", "HotButtons10"}, true);
            if (loadouts) host.actions->copy_char_ini_sections(chars, {"HBLoadouts"}, false);
            if (socials) host.actions->copy_char_ini_sections(chars, {"Socials"}, true);
        }
        // The user colours are read again with the layout (chat_layout_loaded), the filters and options at once.
        if (host.conn && !from.character_dir.empty() && (parts.chat || parts.options))
            host.conn->copy_character_settings(from.character_dir, parts.chat, parts.options);
        if (host.copy_ui_layout) host.copy_ui_layout(from.character_dir);
        reload_layout = true;
        return std::nullopt;
    }
    bool reload_layout = false; // a layout was copied: the windows are placed again before the next frame

    // CTipWnd::Activate on the context tip window; its OnShow deactivates the tip of the day window.
    bool activate_context_tip(int tip, bool force) {
        StockWindow* w = window("TipWindow");
        if (!w || !w->activate(host, force ? tip + kTipForce : tip)) return false;
        if (StockWindow* totd = window("TipWindow#2")) totd->set_shown(host, false);
        return true;
    }

    void load_cascade() {
        if (!host.game) return;
        std::uint32_t expansions = host.character ? host.character->sheet().expansions : 0xffffffffu;
        // The rows this client runs with this UI pack (menu_command_offered): a window toggle needs the pack to draw
        // the window (its script calls eq.ReplaceWindow every frame, so by the EQ button's click it has).
        auto replaces = [this](std::string_view name) {
            auto it = replaced_at.find(name);
            return it != replaced_at.end() && frame - it->second <= 2;
        };
        std::function<bool(int)> offered;
        if (!host.keep_cpp_windows) offered = [&](int command) { return menu_command_offered(command, replaces); };
        cascade.load(*host.game, expansions, host.conn ? host.conn->pvp_rules() : 0, offered);
    }

    explicit Impl(EqHost& h) : host(h) {
        // The chat windows (CChatWindowManager's): the main one shown in game, the others while the chat manager
        // has them (made from its menu, the Tells window); closing one of those closes it there.
        struct Chat : StockWindow {
            int slot;
            explicit Chat(int s) : StockWindow("Chat" + std::to_string(s), true), slot(s) {}
            bool shown(EqHost& h2) override { return h2.in_game && (slot == 1 ? open_ : chat_window_used(slot)); }
            void set_shown(EqHost& h2, bool show) override {
                if (slot == 1) StockWindow::set_shown(h2, show);
                else if (!show) chat_window_closed(slot);
            }
        };
        for (int slot = 1; slot <= 32; ++slot) windows.push_back(std::make_unique<Chat>(slot));
        // CEQMainWnd: the EQ button (a check box, pressed while its menu is open), the Marketplace button (SCButton:
        // the window's toggle) and the flash sale button (SCFSButton: hidden by the constructor, shown only by a flash
        // sale notice this client does not get).
        struct EqMain : StockWindow {
            Impl& m;
            explicit EqMain(Impl& impl) : StockWindow("EQMainWnd", true), m(impl) {}
            bool shown(EqHost& h2) override { return open_ && h2.in_game; }
            void child(EqHost&, std::string_view id, Child& out) override {
                if (id == "EQButton") out.checked = m.cascade.open();
                else if (id == "SCFSButton") out.shown = false;
            }
            bool click(EqHost& h2, std::string_view id, std::string_view button, bool) override {
                if (id == "SCButton" && button == "left") { // WndNotification @0x665ed0: Show(!shown)
                    if (h2.conn) h2.conn->set_marketplace_open(!h2.conn->marketplace().open);
                    return true;
                }
                if (id != "EQButton" || button != "left") return false;
                m.load_cascade();
                auto it = m.screen_rects.find("EQMainWnd");
                std::array<int, 4> r = it != m.screen_rects.end() ? it->second : std::array<int, 4>{0, 0, 53, 53};
                ImVec2 ds = ImGui::GetIO().DisplaySize;
                m.cascade.toggle(r, static_cast<int>(ds.x), static_cast<int>(ds.y));
                return true;
            }
        };
        windows.push_back(std::make_unique<EqMain>(*this));
        add_hud_windows(windows);
        add_item_windows(windows);
        add_misc_windows(windows);
        add_item_display_windows(windows);
        add_dialog_windows(windows);
        add_social_windows(windows);
        add_real_estate_windows(windows);
        add_progress_windows(windows);
        add_reward_selection_window(windows);
        add_achievement_windows(windows);
        add_group_search_windows(windows);
        add_map_windows(windows);
        add_player_windows(windows);
        add_market_windows(windows);
        add_marketplace_windows(windows);
        add_mail_windows(windows);
        add_merc_windows(windows);
        add_adventure_windows(windows);
        add_tint_windows(windows);
        add_customization_windows(windows);
        add_ability_windows(windows);
        add_ability_book_window(windows);
        add_inventory_manager_window(windows);
        add_socialedit_windows(windows);
        host.open_social_editor = [this](std::int32_t social) {
            // A Lua social opens in the Lua social editor (app/lua_eq_luasocial.cpp) instead.
            if (this->host.hot_buttons && social >= 0 && social < game::kSocialPages * game::kSocialButtons &&
                this->host.hot_buttons->social(social).is_lua()) {
                this->host.lua_social_edit = social;
                return;
            }
            if (StockWindow* w = window("SocialEditWnd")) w->activate(this->host, social);
        };
        add_name_windows(windows);
        add_help_windows(windows);
        add_layout_windows(windows);
        add_video_windows(windows);
        add_zone_guide_windows(windows);
        add_fellowship_windows(windows);
        add_story_windows(windows);
        add_journal_windows(windows);
        add_login_windows(windows);
        host.open_help = [this](const std::string& page) {
            if (StockWindow* w = window("HelpWindow")) {
                help_window_set_file(*w, this->host, page);
                w->set_shown(this->host, true);
            }
        };
        host.layout_sources = [this] { return layout_sources(); };
        host.copy_layout = [this](const EqHost::LayoutSource& from, const EqHost::LayoutCopyParts& parts) {
            return copy_layout(from, parts);
        };
        host.show_tip_of_day = [this] {
            if (StockWindow* w = window("TipWindow#2")) w->activate(this->host, -1 + kTipForce);
        };
        host.activate_tip = [this](int tip) { activate_context_tip(tip, false); };
        host.show_window = [this](std::string_view name) {
            if (StockWindow* w = window(name)) w->set_shown(this->host, true);
        };
    }

    StockWindow* window(std::string_view name) {
        if (windows_indexed != windows.size()) {
            windows_by_name.clear();
            for (auto& w : windows) windows_by_name.emplace(w->screen(), w.get()); // a later duplicate is not taken
            windows_indexed = windows.size();
        }
        auto it = windows_by_name.find(name);
        return it != windows_by_name.end() ? it->second : nullptr;
    }

    // ui::window_ini_info(name), worked out once per name: it depends on the name alone.
    const ui::WindowIniInfo& ini_info(std::string_view name) {
        auto it = ini_infos.find(name);
        if (it == ini_infos.end()) it = ini_infos.emplace(std::string(name), ui::window_ini_info(name)).first;
        return it->second;
    }

    // A window's list as eq.ListInfo, eq.ListRow and eq.WindowList read it: StockWindow::list once a frame per
    // window and list, since most windows build the list again on each call (reading n rows was n builds). What a
    // call returns points into the window, so it stays valid; a list asked for later that comes back in the same
    // ListData (a window with one buffer for several lists) drops the others' entries. Every eq call that can change a
    // list (a click, a sort, typed text, an option: all but the reads named in register_functions) and EqApi's own
    // changes drop them all (lists_changed), so a change shows in the same frame as before.
    struct ListMemo {
        const StockWindow* window;
        std::string id;
        const ListData* data;
    };
    std::vector<ListMemo> lists;
    int lists_frame = -1;
    const ListData* list(StockWindow& w, std::string_view id) {
        if (lists_frame != frame) {
            lists.clear();
            lists_frame = frame;
        }
        for (const ListMemo& e : lists)
            if (e.window == &w && e.id == id) return e.data;
        const ListData* d = w.list(host, id);
        if (d) std::erase_if(lists, [d](const ListMemo& e) { return e.data == d; });
        lists.push_back({&w, std::string(id), d});
        return d;
    }
    void lists_changed() { lists.clear(); }

    static Impl& of(lua_State* L) { return *static_cast<Impl*>(lua_touserdata(L, lua_upvalueindex(1))); }

    // The arguments are checked before the Child exists: a failed check longjmps past C++ destructors.
    Child child(lua_State* L, StockWindow*& w) {
        w = window(luaL_checkstring(L, 1));
        const char* id = w ? luaL_checkstring(L, 2) : nullptr;
        Child c;
        if (w) w->child(host, id, c);
        return c;
    }
};

namespace {

using Impl = EqApi::Impl;

void push_cell(lua_State* L, const ui::UiTextures::Cell& c) {
    lua_pushlightuserdata(L, c.id);
    lua_pushnumber(L, c.u0);
    lua_pushnumber(L, c.v0);
    lua_pushnumber(L, c.u1);
    lua_pushnumber(L, c.v1);
}

} // namespace

// The item table of eq.Item.
int push_item(lua_State* L, const net::Item* item) {
    if (!item) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 16);
    auto set_int = [&](const char* k, lua_Integer v) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, k);
    };
    lua_pushstring(L, item->name.c_str());
    lua_setfield(L, -2, "name");
    set_int("id", item->id);
    set_int("icon", item->icon);
    set_int("count", item->stackable() ? item->stack_count : 1);
    set_int("charges", static_cast<std::int32_t>(item->charges));
    set_int("stackSize", item->max_stack);
    lua_pushboolean(L, item->stackable());
    lua_setfield(L, -2, "stackable");
    set_int("bagSlots", item->bag_slots);
    lua_pushnumber(L, item->weight / 10.0);
    lua_setfield(L, -2, "weight");
    lua_pushboolean(L, item->magic != 0);
    lua_setfield(L, -2, "magic");
    lua_pushboolean(L, item->is_lore());
    lua_setfield(L, -2, "lore");
    set_int("type", item->item_type);
    set_int("size", item->size);
    set_int("ac", item->ac);
    set_int("hp", item->hp);
    set_int("mana", item->mana);
    lua_pushlstring(L, game::category_name(game::item_category(*item)).data(), game::category_name(game::item_category(*item)).size());
    lua_setfield(L, -2, "category"); // Hailstone: the bag filters' kind ("" none; game/item_sort.h)
    lua_pushboolean(L, game::sell_refusal(*item) == 0);
    lua_setfield(L, -2, "sellable"); // a merchant buys it (game::sell_refusal)
    return 1;
}

namespace {

// ---------------------------------------------------------------- game state

// eq.InGame() -> true when a character is loaded
int InGame(lua_State* L) {
    lua_pushboolean(L, Impl::of(L).host.in_game);
    return 1;
}

// eq.UiReady() -> true in game or at character select / creation
int UiReady(lua_State* L) {
    lua_pushboolean(L, Impl::of(L).host.ui_ready);
    return 1;
}

// eq.GameState() -> CEverQuest::GameState (1 character select, 2 character creation, 5 in game), nil before it exists
int GameState(lua_State* L) {
    int s = Impl::of(L).host.game_state;
    if (s == 0) lua_pushnil(L);
    else lua_pushinteger(L, s);
    return 1;
}

// eq.Name() -> character name or nil
int Name(lua_State* L) {
    auto& h = Impl::of(L).host;
    const game::Spawn* me = h.conn && h.in_game ? h.conn->world().player() : nullptr;
    if (me) lua_pushstring(L, me->name.c_str());
    else lua_pushnil(L);
    return 1;
}

// eq.SlotName(slot) -> "Primary", ...
int SlotName(lua_State* L) {
    lua_pushstring(L, net::possessions_slot_name(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}

// eq.Item(slot [, bagSlot]) -> nil or { name, id, icon, count, charges, stackSize, stackable, bagSlots, weight,
// magic, lore, type, size, ac, hp, mana, cooldown }; cooldown: the slot's recast cool-down fraction (CInvSlotWnd_Draw:
// shaded from the slot's top + cooldown x its height to its bottom in eq.CoolDownColor()), absent while idle
int Item(lua_State* L) {
    auto& h = Impl::of(L).host;
    if (!h.conn || !h.in_game) return push_item(L, nullptr);
    auto slot = static_cast<std::int16_t>(luaL_checkinteger(L, 1));
    auto bag = static_cast<std::int16_t>(lua_isnoneornil(L, 2) ? -1 : luaL_checkinteger(L, 2));
    const net::Item* item = h.conn->inventory().at(net::ItemSlot::possessions(slot, bag));
    push_item(L, item);
    Impl::of(L).set_item_cool_down(L, std::format("inv:{}:{}", slot, bag), item);
    return 1;
}

// texture, u0, v0, u1, v1 = eq.ItemIcon(icon)   -- nil when the icon's sheet is missing
int ItemIcon(lua_State* L) {
    auto& h = Impl::of(L).host;
    auto icon = h.textures ? h.textures->item_icon(static_cast<std::uint32_t>(luaL_checkinteger(L, 1))) : std::nullopt;
    if (!icon) {
        lua_pushnil(L);
        return 1;
    }
    push_cell(L, *icon);
    return 5;
}

// plat, gold, silver, copper = eq.Money()
int Money(lua_State* L) {
    auto& h = Impl::of(L).host;
    net::Coins c;
    if (h.conn) c = h.conn->coins();
    for (std::uint32_t v : {c.platinum, c.gold, c.silver, c.copper}) lua_pushinteger(L, v);
    return 4;
}

// text [, r, g, b] = eq.Label(eqType)   -- the text a game label with this EQType shows
int Label(lua_State* L) {
    LabelValue v = eq_label(Impl::of(L).host, static_cast<int>(luaL_checkinteger(L, 1)));
    lua_pushstring(L, v.text.c_str());
    if (!v.argb) return 1;
    Rgb c = argb_to_rgb(*v.argb);
    for (float f : c) lua_pushnumber(L, f);
    return 4;
}

// fraction, text, value [, r, g, b] = eq.Gauge(eqType)   -- fraction 0..1, nil when the gauge is empty/unavailable;
// value is __GetGaugeValueFromEQ's own (0..1000, -1 nothing drawn at all, -2 full in dark red, -3 empty: a corpse,
// -110..-10 and -2000..-1000 the special bands); r, g, b the fill tint CGaugeWnd::Draw gives a special value
// (gauge_fill), nil for a normal one.
int Gauge(lua_State* L) {
    GaugeValue g = eq_gauge(Impl::of(L).host, static_cast<int>(luaL_checkinteger(L, 1)));
    GaugeFill f = gauge_fill(g.value);
    if (f.fill < 0) lua_pushnil(L);
    else lua_pushnumber(L, std::min(f.fill, 1000) / 1000.0);
    lua_pushstring(L, g.text.c_str());
    lua_pushinteger(L, g.value);
    if (!f.tint) return 3;
    Rgb c = argb_to_rgb(*f.tint);
    for (float v : c) lua_pushnumber(L, v);
    return 6;
}

// ok = eq.ClickSlot(slot, bagSlot|nil, "left"|"leftheld"|"right"|right-button phase [, outside])
// The game's slot logic: a left click picks up, drops, swaps or splits (Shift whole stack, Ctrl one; the original
// on the button's release, HandleLButtonUp); "leftheld", the left button held on the slot, puts a hotbutton for it
// on the cursor (item_left_held; false when none went there); the right button (a quick release, "rightup") uses
// the item: inspect, open a bag, cast a clicky.
// The last ImGui item (the child's hit box the script just tested) as the clicked child's screen rectangle.
void note_click_rect(EqHost& h) {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    h.click_rect = {a.x, a.y, b.x, b.y};
}

// The right button held on an item (CInvSlot::HandleRButtonHeld): its item display, whatever a quick click would
// do with it (cast, eat, open the bag, loot). Not a name and icon only (the inspect window's: OP_InspectAnswer
// carries no item data).
void inspect_held(EqHost& h, const net::Item* item) {
    if (item && item->id != 0 && h.inspect_item) h.inspect_item(*item);
}

int ClickSlot(lua_State* L) {
    auto& h = Impl::of(L).host;
    note_click_rect(h);
    if (!h.conn || !h.in_game) {
        lua_pushboolean(L, false);
        return 1;
    }
    auto slot = static_cast<std::int16_t>(luaL_checkinteger(L, 1));
    auto bag = static_cast<std::int16_t>(lua_isnoneornil(L, 2) ? -1 : luaL_checkinteger(L, 2));
    std::string button = lua_isstring(L, 3) ? lua_tostring(L, 3) : "left";
    bool outside = lua_toboolean(L, 4) != 0;
    net::ItemSlot s = net::ItemSlot::possessions(slot, bag);
    if (button == "leftheld") {
        lua_pushboolean(L, item_left_held(h, s));
        return 1;
    }
    if (button == "left") {
        const ImGuiIO& io = ImGui::GetIO();
        h.conn->click_slot(s, io.KeyShift, io.KeyCtrl);
    } else if ((button == "right" || button == "rightup") && !outside) {
        item_right_click(h, s);
    } else if (button == "rightheld") {
        inspect_held(h, h.conn->inventory().at(s));
    }
    lua_pushboolean(L, true);
    return 1;
}

// ok = eq.HasSlot(slot, bagSlot|nil)   -- true when ClickSlot can act on that slot now: in game, a possessions
// slot (worn 0..22, general 23..32, the cursor 33), and with bagSlot a place (0-based) inside the bag in that
// general slot. An empty slot or bag place counts: ClickSlot drops the cursor's item there.
int HasSlot(lua_State* L) {
    auto& h = Impl::of(L).host;
    const lua_Integer slot = luaL_checkinteger(L, 1);
    const lua_Integer bag = lua_isnoneornil(L, 2) ? -1 : luaL_checkinteger(L, 2);
    bool ok = h.conn && h.in_game && slot >= 0 && slot <= net::kSlotCursor;
    if (ok && bag != -1) {
        const net::Item* it = slot >= net::kSlotGeneral1 && slot <= net::kSlotGeneral10
                                  ? h.conn->inventory().at(net::ItemSlot::possessions(static_cast<std::int16_t>(slot)))
                                  : nullptr;
        ok = it && it->is_bag() && bag >= 0 && bag < it->bag_slots;
    }
    lua_pushboolean(L, ok);
    return 1;
}

// list = eq.AltCurrencies([showMissing])   -- { name, count, icon, maxStack } per alternate currency
// (OP_AltCurrency; the name is dbstr type 0x12 of the currency id, as CInventoryWnd's Alt. Currency list reads it,
// "Currency <n>" when the game folder has none); without showMissing only those held and shown. The order and filter are the InventoryWindow's AltCurr_PointList rows (eq.ListClick selects one there).
int AltCurrencies(lua_State* L) {
    auto& h = Impl::of(L).host;
    bool missing = lua_toboolean(L, 1) != 0;
    lua_newtable(L);
    if (!h.conn) return 1;
    int n = 0;
    for (const auto& [id, a] : h.conn->alt_currencies()) {
        if (!missing && (a.amount == 0 || !a.entry.shown)) continue;
        lua_newtable(L);
        std::string name = h.conn->strings().db_string(id, 0x12);
        if (name.empty()) name = "Currency " + std::to_string(id);
        lua_pushstring(L, name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, a.amount);
        lua_setfield(L, -2, "count");
        lua_pushinteger(L, a.entry.icon);
        lua_setfield(L, -2, "icon");
        lua_pushinteger(L, a.entry.stack_size);
        lua_setfield(L, -2, "maxStack");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// texture, w, h = eq.UiTexture(file)   -- a texture from uifiles/default (window art), nil if missing
int UiTexture(lua_State* L) {
    auto& h = Impl::of(L).host;
    const auto* t = h.textures ? h.textures->file(luaL_checkstring(L, 1)) : nullptr;
    if (!t) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlightuserdata(L, t->id);
    lua_pushinteger(L, t->width);
    lua_pushinteger(L, t->height);
    return 3;
}

// texture, u0, v0, u1, v1, width, height = eq.UiAnimation(name)
// Explicitly requested stock XML art retains the frame's original aspect.
int UiAnimation(lua_State* L) {
    auto& h = Impl::of(L).host;
    std::size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    const auto image = h.textures ? h.textures->animation_image(std::string_view(name, length)) : std::nullopt;
    if (!image) { lua_pushnil(L); return 1; }
    push_cell(L, image->cell);
    lua_pushinteger(L, image->width); lua_pushinteger(L, image->height);
    return 7;
}

// ---------------------------------------------------------------- stock windows

// shown, x, y, w, h, sized, place = eq.ReplaceWindow(name [, sizable])
// Call every frame from the replacing window's draw function. shown: whether the game has the window shown (draw
// yours then); x, y, w, h: where the stock window sits: its XML location and size, then the character's UI ini over
// defaults.ini as CSidlScreenWnd::LoadIniInfo reads them (ui/ui_ini.h; `sizable`: the XML window can be resized, so
// a stored size counts). sized: w, h come from an ini; place: the window should be put there now (a character's
// layout was just loaded) rather than only the first time it shows.
// The client's own prompt for the server's compiled plugins ("PluginConsent") gives way only to the pack's own window:
// a plugin's code could otherwise put it away and draw a question of its own.
bool may_replace(std::string_view name) { return name != "PluginConsent" || ui::lua::LuaUi::caller_tag().empty(); }

int ReplaceWindow(lua_State* L) {
    auto& m = Impl::of(L);
    std::string name = luaL_checkstring(L, 1);
    if (may_replace(name)) m.replaced_at[name] = m.frame;
    StockWindow* w = m.window(name);
    bool shown = w && (m.host.ui_ready || w->pre_game()) && w->shown(m.host);
    lua_pushboolean(L, shown);
    // Windows made from one screen several times: the chat windows ("Chat1".."Chat32", ChatWindow) and the
    // containers ("Bag1".."Bag36", ContainerWindow; each further one 20 pixels down and right: where CContainerMgr
    // puts a bag that has no saved place is not known, docs/unknowns.md).
    std::string_view screen = name;
    int cascade = 0;
    if (name.rfind("Chat", 0) == 0 && name.size() > 4 && std::isdigit(static_cast<unsigned char>(name[4]))) screen = "ChatWindow";
    if (name.rfind("Bag", 0) == 0 && name.size() > 3 && std::isdigit(static_cast<unsigned char>(name[3]))) {
        screen = "ContainerWindow";
        cascade = (std::atoi(name.c_str() + 3) - 1) * 20;
    }
    // The item displays ("ItemDisplayWindow", "ItemDisplayWindow#2" ..): CItemDisplayManager::CreateWindowInstance
    // @006a1ae0 moves each new one 20 pixels down and right per display made before it, then loads its UI ini place.
    if (name.rfind("ItemDisplayWindow#", 0) == 0) {
        screen = "ItemDisplayWindow";
        cascade = (std::atoi(name.c_str() + 18) - 1) * 20;
    }
    auto r = m.host.textures ? m.host.textures->screen_rect(screen) : std::nullopt;
    if (r) r->x += cascade, r->y += cascade;
    ui::UiTextures::Rect rect = r ? *r : ui::UiTextures::Rect{100, 100, 200, 100};
    // CContainerWnd keeps two size children, StandardSize (+0x270) and CombineSize (+0x274): a tradeskill container's
    // window takes the combine size (EQUI_Container.xml Container_CombineSize, 230 x 170: Combine and Done above
    // the slots), any other bag the standard one (the screen's 115 x 240).
    if (screen == "ContainerWindow" && w)
        if (const net::Item* bag = w->bag_item(m.host, -1); bag && game::is_combine_container(bag->bag_type))
            rect.w = 230, rect.h = 170;
    ui::WindowRect place{rect.x, rect.y, rect.w, rect.h};
    bool sized = false, reset = false;
    // (a bag window keeps its place in a section of its own, "Bag<n>", once it was moved; until then it cascades)
    if (m.layout.loaded()) {
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        ui::WindowRect p = m.layout.place(m.ini_info(name), place, static_cast<int>(ds.x),
                                          static_cast<int>(ds.y), lua_toboolean(L, 2) != 0);
        sized = p.w != place.w || p.h != place.h;
        place = p;
        Impl::Placed& placed = m.placed[name];
        int& gen = placed.generation;
        int& scr = placed.screen;
        reset = gen != m.layout.generation() || scr != m.screen_serial;
        if (shown) {
            gen = m.layout.generation(); // placed when it shows
            scr = m.screen_serial;
            if (reset) m.seen.erase(name);
        }
    }
    // The window's code moved it (CQuantityWnd::Open at its caller's button, ...): put it there now.
    if (shown && w)
        if (auto mv = w->take_move(m.host)) {
            place.x = (*mv)[0];
            place.y = (*mv)[1];
            reset = true;
        }
    lua_pushinteger(L, place.x);
    lua_pushinteger(L, place.y);
    lua_pushinteger(L, place.w);
    lua_pushinteger(L, place.h);
    lua_pushboolean(L, sized);
    lua_pushboolean(L, reset);
    return 7;
}

// shown = eq.WindowShown(name)   -- eq.ReplaceWindow's first return alone: whether the game has the window shown,
// with the window counted as replaced this frame as eq.ReplaceWindow counts it (EqApi::replaces: the C++ window stays
// off, the EQ menu offers its toggle), but no place worked out. For a script's check before it draws: a window it
// draws still calls eq.ReplaceWindow while shown, since that call is what places it and keeps its moves.
int WindowShown(lua_State* L) {
    auto& m = Impl::of(L);
    const char* name = luaL_checkstring(L, 1);
    if (may_replace(name)) {
        if (auto it = m.replaced_at.find(std::string_view(name)); it != m.replaced_at.end()) it->second = m.frame;
        else m.replaced_at.emplace(name, m.frame);
    }
    StockWindow* w = m.window(name);
    lua_pushboolean(L, w && (m.host.ui_ready || w->pre_game()) && w->shown(m.host));
    return 1;
}

// eq.WindowPlaced(name, x, y, w, h [, sizable])   -- where the replacing window is this frame (after ui.Begin): kept
// in the character's UI ini as CSidlScreenWnd::StoreIniInfo does
int WindowPlaced(lua_State* L) {
    auto& m = Impl::of(L);
    // Every argument before any C++ object: a failed check longjmps past destructors (and would leave a
    // default screen_rects entry behind).
    const char* name_arg = luaL_checkstring(L, 1);
    const std::array<int, 4> now{number_to_int(luaL_checknumber(L, 2)), number_to_int(luaL_checknumber(L, 3)),
                                 number_to_int(luaL_checknumber(L, 4)), number_to_int(luaL_checknumber(L, 5))};
    const bool sizable = lua_toboolean(L, 6) != 0;
    std::string name = name_arg;
    m.screen_rects[name] = now;
    if (!m.layout.loaded()) return 0;
    const Impl::Placed& placed = m.placed[name];
    if (placed.generation != m.layout.generation()) return 0;
    ui::WindowRect r{now[0], now[1], now[2], now[3]};
    auto it = m.seen.find(name);
    bool moved = it != m.seen.end() && it->second != now && placed.screen == m.screen_serial;
    m.seen[name] = now;
    if (moved) m.last_place[name] = {r, sizable};
    return 0;
}

// eq.ShowWindow(name, show)   -- shows or hides the game window (and with it the replacement)
int ShowWindow(lua_State* L) {
    auto& m = Impl::of(L);
    if (StockWindow* w = m.window(luaL_checkstring(L, 1))) w->set_shown(m.host, lua_toboolean(L, 2) != 0);
    return 0;
}

// ok = eq.WindowButton(name, child [, message])   -- the window's handler for that child (message 1, a click, by
// default; 3 is a buff button's right click, which removes the buff)
int WindowButton(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    int message = static_cast<int>(luaL_optinteger(L, 3, 1));
    bool ok = w && w->click(m.host, luaL_checkstring(L, 2), message == 3 ? "remove" : "left", false);
    lua_pushboolean(L, ok);
    return 1;
}

// ok = eq.ClickChild(name, child [, button [, outside]])   -- a click on the child: "left", "right" or a
// right-button phase ("rightdown", "rightheld", "rightup", "rightupheld")
int ClickChild(lua_State* L) {
    auto& m = Impl::of(L);
    note_click_rect(m.host);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const char* child_arg = luaL_checkstring(L, 2);
    std::string button = lua_isstring(L, 3) ? lua_tostring(L, 3) : "left";
    std::string child = child_arg;
    bool outside = lua_toboolean(L, 4) != 0;
    // An item slot of any window (loot, bank, trade, merchant, inspect, ...): the held right button shows the
    // item; a quick right click the window does nothing with shows it too.
    const net::Item* item = w ? w->slot_item(m.host, child) : nullptr;
    bool ok = false;
    if (item && button == "rightheld") {
        inspect_held(m.host, item);
        ok = true;
    } else {
        ok = w && w->click(m.host, child, button, outside);
        if (!ok && item && (button == "right" || button == "rightup") && !outside) {
            inspect_held(m.host, item);
            ok = true;
        }
    }
    lua_pushboolean(L, ok);
    return 1;
}

// ok = eq.StmlLink(name, child, message, data)   -- a <a WndNotify="message,data"> link clicked in the window's STML
// box (CStmlWnd sends the window the message with the data, CItemDisplayWnd's XWM_LINK 27); message -1 is an href link
// (`data` its target), which the STML box follows itself (the help window's pages)
int StmlLink(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    int message = lua_isnumber(L, 3) ? static_cast<int>(lua_tointeger(L, 3)) : 0;
    const char* data = lua_isstring(L, 4) ? lua_tostring(L, 4) : "";
    lua_pushboolean(L, w && w->stml_link(m.host, luaL_checkstring(L, 2), message, data));
    return 1;
}

// ok = eq.ClickChildAt(name, child, clientX, clientY [, button])   -- as ClickChild (no original to place)
int ClickChildAt(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const char* child = w ? luaL_checkstring(L, 2) : nullptr;
    std::string button = lua_isstring(L, 5) ? lua_tostring(L, 5) : "left";
    lua_pushboolean(L, w && w->click(m.host, child, button, false));
    return 1;
}

// text, set = eq.WindowText(name, child)   -- the text the game set on the window's child ("" for none); set is
// true when the window set it (an empty text then replaces the XML's)
int WindowText(lua_State* L) {
    StockWindow* w = nullptr;
    Child c = Impl::of(L).child(L, w);
    lua_pushstring(L, c.text ? c.text->c_str() : "");
    lua_pushboolean(L, c.text.has_value());
    return 2;
}

// ok = eq.SetWindowText(name, child, text)   -- typing into the window's edit box
int SetWindowText(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    // Both checked before the std::string the call takes exists (the arguments' order of evaluation is not fixed).
    const char* child = w ? luaL_checkstring(L, 2) : nullptr;
    const char* text = w ? luaL_checkstring(L, 3) : nullptr;
    lua_pushboolean(L, w && w->set_text(m.host, child, text));
    return 1;
}

// shown = eq.ChildShown(name, child)   -- whether the game shows that child (nil when the window leaves it as the
// XML has it)
int ChildShown(lua_State* L) {
    StockWindow* w = nullptr;
    Child c = Impl::of(L).child(L, w);
    if (c.shown) lua_pushboolean(L, *c.shown);
    else lua_pushnil(L);
    return 1;
}

// under = eq.ChildUnder(name, child)   -- whether the window draws that child itself in its own Draw, under its
// other children (CTargetWnd::Draw's target box, CPlayerWnd::Draw's attack indicator)
int ChildUnder(lua_State* L) {
    StockWindow* w = nullptr;
    Child c = Impl::of(L).child(L, w);
    lua_pushboolean(L, c.under);
    return 1;
}

// enabled, checked, value, count, action = eq.ChildState(name, child)   -- action: a disabled button that still
// takes the right button down (CXWnd bAction)
int ChildState(lua_State* L) {
    StockWindow* w = nullptr;
    Child c = Impl::of(L).child(L, w);
    if (!w) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushboolean(L, c.enabled.value_or(true));
    if (c.checked) lua_pushboolean(L, *c.checked);
    else lua_pushnil(L);
    if (c.slider_value) lua_pushinteger(L, *c.slider_value);
    else lua_pushnil(L);
    if (c.slider_count) lua_pushinteger(L, *c.slider_count);
    else lua_pushnil(L);
    lua_pushboolean(L, c.action);
    return 5;
}

// fraction, text [, r, g, b] = eq.ChildGauge(name, child)   -- a gauge the window fills itself; nil, "" otherwise (the
// EQType value is used); r, g, b the special value's fill tint as eq.Gauge gives it
int ChildGauge(lua_State* L) {
    StockWindow* w = nullptr;
    Child c = Impl::of(L).child(L, w);
    GaugeFill f = c.has_gauge ? gauge_fill(c.gauge) : GaugeFill{};
    if (f.fill < 0) lua_pushnil(L);
    else lua_pushnumber(L, std::min(f.fill, 1000) / 1000.0);
    lua_pushstring(L, c.gauge_text.c_str());
    if (!f.tint) return 2;
    Rgb rgb = argb_to_rgb(*f.tint);
    for (float v : rgb) lua_pushnumber(L, v);
    return 5;
}

// r, g, b [, a] = eq.ChildColor(name, child [, "hover"|"decal"])   -- the colour the window set on the child: its
// text colour, its button's mouse-over colour, or the tint of its decal / animation; nil when it sets none
int ChildColor(lua_State* L) {
    StockWindow* w = nullptr;
    Child c = Impl::of(L).child(L, w);
    std::string part = lua_isstring(L, 3) ? lua_tostring(L, 3) : "";
    if (part == "decal") {
        if (!c.tint) return 0;
        for (float f : *c.tint) lua_pushnumber(L, f);
        return 4;
    }
    const auto& col = part == "hover" ? (c.hover_color ? c.hover_color : c.color) : c.color;
    if (!col) return 0;
    for (float f : *col) lua_pushnumber(L, f);
    return 3;
}

// text = eq.ChildTooltip(name, child)   -- the tooltip the window set on the child, "" when none
int ChildTooltip(lua_State* L) {
    StockWindow* w = nullptr;
    Child c = Impl::of(L).child(L, w);
    lua_pushstring(L, c.tooltip ? c.tooltip->c_str() : "");
    return 1;
}

// texture, u0, v0, u1, v1 = eq.ChildArt(name, child, part)   -- what the child shows now; false when it shows
// nothing (an empty buff slot), nil when the window keeps the XML's art; the animation's name (a string) when it is
// one the UI draws itself (UiTextures::set_all_art). part: "background", "decal", "spellicon".
// Pushes eq.ChildArt's results for `a`; returns how many.
int push_art(Impl& m, lua_State* L, const ChildArt& a) {
    switch (a.kind) {
    case ChildArt::None: lua_pushboolean(L, false); return 1;
    case ChildArt::Image: push_cell(L, a.cell); return 5;
    case ChildArt::Anim:
        if (auto cell = m.host.textures ? m.host.textures->animation(a.anim) : std::nullopt) {
            push_cell(L, *cell);
            return 5;
        }
        if (!a.anim.empty()) lua_pushstring(L, a.anim.c_str());
        else lua_pushboolean(L, false);
        return 1;
    default: lua_pushnil(L); return 1;
    }
}

int ChildArtFn(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = nullptr;
    const std::string_view part = luaL_checkstring(L, 3);
    Child c = m.child(L, w);
    const ChildArt& a = part == "background" ? c.background : part == "decal" ? c.decal : c.spellicon;
    return push_art(m, L, a);
}

// Sets the record's fields tex, u0, v0, u1, v1 from `n` values push_art left on the stack (nil for those it gave no
// value), the record just under them; pops them.
void set_art_fields(lua_State* L, int n) {
    static constexpr const char* kFields[] = {"tex", "u0", "v0", "u1", "v1"};
    for (int k = n; k < 5; ++k) lua_pushnil(L);
    for (int k = 4; k >= 0; --k) lua_setfield(L, -2 - k, kFields[k]);
}

// The record table an eq.*Info call fills: the argument at `arg` when it is a table (a caller's, kept from frame to
// frame), else a new one; left on the top of the stack.
void push_record(lua_State* L, int arg, int fields) {
    if (lua_istable(L, arg)) lua_pushvalue(L, arg);
    else lua_createtable(L, 0, fields);
}

void set_number(lua_State* L, const char* field, double v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, field);
}
void set_nil(lua_State* L, const char* field) {
    lua_pushnil(L);
    lua_setfield(L, -2, field);
}

// r, g, b, a = eq.GemTint(name, child)   -- the colour the game tints a spell gem with (by spell category)
int GemTint(lua_State* L) {
    StockWindow* w = nullptr;
    Child c = Impl::of(L).child(L, w);
    std::array<float, 4> t = c.gem_tint.value_or(std::array<float, 4>{1, 1, 1, 1});
    for (float f : t) lua_pushnumber(L, f);
    return 4;
}

// top, r, g, b, a = eq.GemOverlay(name, child, y0, y1)   -- a spell gem's cool-down rectangle (CSpellGemWnd::Draw):
// from `top` to y1 in the colour given; nil while the gem has none
int GemOverlay(lua_State* L) {
    std::optional<double> overlay;
    std::uint32_t argb = 0;
    { // the Child gone before the checks below (a failed one longjmps past its destructor)
        StockWindow* w = nullptr;
        Child c = Impl::of(L).child(L, w);
        overlay = c.gem_overlay;
        argb = c.gem_overlay_color;
    }
    if (!overlay) {
        lua_pushnil(L);
        return 1;
    }
    const std::int32_t y0 = number_to_int(luaL_checknumber(L, 3)), y1 = number_to_int(luaL_checknumber(L, 4));
    lua_pushinteger(L, game::spell_gem_overlay_top(*overlay, y0, y1));
    for (int shift : {16, 8, 0, 24}) lua_pushnumber(L, static_cast<double>((argb >> shift) & 0xffu) / 255.0);
    return 5;
}

// info = eq.GemInfo(name, child [, into [, y0, y1]])   -- a spell gem's looks in one call, as eq.ChildShown,
// eq.ChildTooltip, eq.ChildArt(name, child, "spellicon"), eq.GemTint and eq.GemOverlay give them (each of those works
// the child out again): the table `into` (kept by the caller from frame to frame) filled, or a new one. Fields: shown
// (as eq.ChildShown), tooltip, tex, u0, v0, u1, v1 (eq.ChildArt's results, nil past the first when it gives one),
// tintR, tintG, tintB, tintA (eq.GemTint's), overlay (the cool-down rectangle's fraction, nil while idle) and with it
// coolR, coolG, coolB, coolA (its colour) and, given y0 and y1, top (eq.GemOverlay's).
int GemInfo(lua_State* L) {
    auto& m = Impl::of(L);
    const bool span = lua_isnumber(L, 4) && lua_isnumber(L, 5);
    const std::int32_t y0 = span ? number_to_int(lua_tonumber(L, 4)) : 0, y1 = span ? number_to_int(lua_tonumber(L, 5)) : 0;
    StockWindow* w = nullptr;
    Child c = m.child(L, w);
    push_record(L, 3, 16);
    if (c.shown) lua_pushboolean(L, *c.shown);
    else lua_pushnil(L);
    lua_setfield(L, -2, "shown");
    lua_pushstring(L, c.tooltip ? c.tooltip->c_str() : "");
    lua_setfield(L, -2, "tooltip");
    set_art_fields(L, push_art(m, L, c.spellicon));
    const std::array<float, 4> t = c.gem_tint.value_or(std::array<float, 4>{1, 1, 1, 1});
    set_number(L, "tintR", t[0]);
    set_number(L, "tintG", t[1]);
    set_number(L, "tintB", t[2]);
    set_number(L, "tintA", t[3]);
    static constexpr const char* kCool[] = {"coolR", "coolG", "coolB", "coolA"};
    if (c.gem_overlay) {
        set_number(L, "overlay", *c.gem_overlay);
        int k = 0;
        for (int shift : {16, 8, 0, 24})
            set_number(L, kCool[k++], static_cast<double>((c.gem_overlay_color >> shift) & 0xffu) / 255.0);
    } else {
        set_nil(L, "overlay");
        for (const char* f : kCool) set_nil(L, f);
    }
    if (c.gem_overlay && span) {
        lua_pushinteger(L, game::spell_gem_overlay_top(*c.gem_overlay, y0, y1));
        lua_setfield(L, -2, "top");
    } else {
        set_nil(L, "top");
    }
    return 1;
}

// top, r, g, b, a, remainingMs, reuseMs = eq.ButtonCoolDown(name, child, y0, y1) -- a button's cool-down (CButtonWnd's DrawCooldown
// @0x8970a0, the spell gem's rectangle): from y0 + ftol(fraction * (y1 - y0)) down to y1, the button's width, in
// user colour 0x15f at alpha 0x40; nil while idle. The window may also supply remaining/reuse milliseconds
// (combat ability buttons); both are 0 while idle or when the window has no timing detail.
int ButtonCoolDown(lua_State* L) {
    std::optional<double> cool_down;
    std::uint64_t left = 0, duration = 0;
    { // the Child gone before the checks below
        StockWindow* w = nullptr;
        const Child child = Impl::of(L).child(L, w);
        cool_down = child.cool_down;
        left = child.cool_down_left;
        duration = child.cool_down_duration;
    }
    if (!cool_down) {
        for (int i = 0; i < 5; ++i) lua_pushnil(L);
        lua_pushinteger(L, 0);
        lua_pushinteger(L, 0);
        return 7;
    }
    const std::int32_t y0 = number_to_int(luaL_checknumber(L, 3)), y1 = number_to_int(luaL_checknumber(L, 4));
    lua_pushinteger(L, game::spell_gem_overlay_top(*cool_down, y0, y1));
    const std::uint32_t argb = cool_down_color();
    for (int shift : {16, 8, 0, 24}) lua_pushnumber(L, static_cast<double>((argb >> shift) & 0xffu) / 255.0);
    lua_pushinteger(L, static_cast<lua_Integer>(left));
    lua_pushinteger(L, static_cast<lua_Integer>(duration));
    return 7;
}

// preview = eq.ItemPreview()   -- the item preview window's state (CObjectPreviewWnd, game/item_preview.h): nil while
// it is closed, else { open = true, show (the view, else "No item preview available."), name, onPlayer, serial (counts
// the answers: the window comes to the front on a new one, CXWndManager::BringWndToTop) }
int ItemPreview(lua_State* L) {
    auto& h = Impl::of(L).host;
    const auto* p = h.conn ? &h.conn->item_preview() : nullptr;
    if (!p || !p->open) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "open");
    lua_pushboolean(L, p->show);
    lua_setfield(L, -2, "show");
    lua_pushboolean(L, p->on_player);
    lua_setfield(L, -2, "onPlayer");
    lua_pushstring(L, p->item ? p->item->name.c_str() : "");
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, p->serial);
    lua_setfield(L, -2, "serial");
    return 1;
}

// texture = eq.ItemPreviewImage(w, h [, view])   -- the preview's 3D view for this frame at w x h pixels (draw it
// with ui.DrawImage / ui.Image, u and v 0..1), nil while there is nothing to show; view "marketplace" is the
// marketplace window's preview, anything else the object preview window's
int preview_view(lua_State* L, int arg) {
    const char* v = luaL_optstring(L, arg, "");
    return std::string_view(v) == "marketplace" ? 1 : 0;
}

int ItemPreviewImage(lua_State* L) {
    auto& h = Impl::of(L).host;
    void* tex = h.item_preview_image ? h.item_preview_image(number_to_int(luaL_checknumber(L, 1)),
                                                            number_to_int(luaL_checknumber(L, 2)), preview_view(L, 3))
                                     : nullptr;
    if (tex) lua_pushlightuserdata(L, tex);
    else lua_pushnil(L);
    return 1;
}

// eq.ItemPreviewControl(what [, x [, y [, view]]])   -- moves the preview's camera: "rotate" (radians across, up),
// "pan" (fractions of the view across, up), "zoom" (a distance factor), "reset" (fitted again); view as
// eq.ItemPreviewImage's
int ItemPreviewControl(lua_State* L) {
    auto& h = Impl::of(L).host;
    if (h.item_preview_control)
        h.item_preview_control(luaL_checkstring(L, 1), static_cast<float>(luaL_optnumber(L, 2, 0.0)),
                               static_cast<float>(luaL_optnumber(L, 3, 0.0)), preview_view(L, 4));
    return 0;
}

// r, g, b, a = eq.CoolDownColor()   -- the cool-down rectangle's colour (GetUserColor(0x15f) at alpha 0x40), for an
// item's `cooldown` (eq.Item, eq.BagSlotItem, eq.SlotItem)
int CoolDownColor(lua_State* L) {
    const std::uint32_t argb = cool_down_color();
    for (int shift : {16, 8, 0, 24}) lua_pushnumber(L, static_cast<double>((argb >> shift) & 0xffu) / 255.0);
    return 4;
}

// Pushes an overlay's text, r, g, b (eq.BuffOverlay's results), or four nils for none.
void push_overlay(lua_State* L, const std::optional<game::BuffOverlayText>& o) {
    if (!o) {
        for (int k = 0; k < 4; ++k) lua_pushnil(L);
        return;
    }
    const std::uint32_t c = o->argb;
    lua_pushstring(L, o->text.c_str());
    lua_pushnumber(L, static_cast<double>((c >> 16) & 0xff) / 255.0);
    lua_pushnumber(L, static_cast<double>((c >> 8) & 0xff) / 255.0);
    lua_pushnumber(L, static_cast<double>(c & 0xff) / 255.0);
}

// spellId, msLeft, name, beneficial, fade [, counter, cr, cg, cb, limit, lr, lg, lb] = eq.BuffSlot(name, child
// [, overlays])   -- the buff a buff window's button shows, nil if none; msLeft is negative for a permanent buff; fade
// the buff / song window's alpha factor for it (under 19 s left it fades out once a second,
// CBuffWindow::OnProcessFrame; 1 otherwise). With overlays true also the button's counter and limit overlays as
// eq.BuffOverlay(name, child, "counter") and (..., "limit") give them (each text, r, g, b; nils for none), in the one
// call; without it they are not worked out.
int BuffSlot(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const char* child = w ? luaL_checkstring(L, 2) : nullptr;
    const bool overlays = lua_toboolean(L, 3) != 0;
    auto b = w ? w->buff(m.host, child, overlays ? kBuffCounterOverlay | kBuffLimitOverlay : 0u) : std::nullopt;
    if (!b) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, b->spell);
    lua_pushinteger(L, b->ms_left);
    lua_pushstring(L, b->name.c_str());
    lua_pushboolean(L, b->beneficial);
    lua_pushnumber(L, b->fade);
    if (!overlays) return 5;
    push_overlay(L, b->counter);
    push_overlay(L, b->limit);
    return 13;
}

// text, r, g, b = eq.BuffOverlay(name, child, which)   -- what a buff window draws over the button, nil if
// nothing: which "timer" (bottom right), "counter" (top right) or "limit" (bottom left); CBuffWindow::PostDraw
int BuffOverlay(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const char* child = w ? luaL_checkstring(L, 2) : nullptr;
    std::string_view which = luaL_checkstring(L, 3); // (checked before `b` holds anything to destroy)
    const unsigned mask = which == "timer" ? kBuffTimerOverlay : which == "counter" ? kBuffCounterOverlay
                          : which == "limit"                   ? kBuffLimitOverlay
                                                               : 0u;
    auto b = w && mask ? w->buff(m.host, child, mask) : std::nullopt;
    const std::optional<game::BuffOverlayText>* o = !b                             ? nullptr
                                                    : mask == kBuffTimerOverlay   ? &b->timer
                                                    : mask == kBuffCounterOverlay ? &b->counter
                                                                                  : &b->limit;
    if (!o || !*o) {
        lua_pushnil(L);
        return 1;
    }
    push_overlay(L, *o);
    return 4;
}

// kind, label [, texture, u0, v0, u1, v1] = eq.HotButton(name, child)   -- what a hotbutton holds: "spell",
// "item", "button" or "empty"
int HotButton(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    auto hb = w ? w->hot_button(m.host, luaL_checkstring(L, 2)) : std::nullopt;
    HotButtonInfo info = hb.value_or(HotButtonInfo{});
    lua_pushstring(L, info.kind.c_str());
    lua_pushstring(L, info.label.c_str());
    if (!info.icon) return 2;
    push_cell(L, *info.icon);
    return 7;
}

// face = eq.HotButtonFace(name, child)   -- a social's extras, nil for other buttons: {color = {r, g, b}, lua,
// running, enabled, cooldown (0..1 of a sweep left), overlay (corner text), tooltip}
int HotButtonFace(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    auto hb = w ? w->hot_button(m.host, luaL_checkstring(L, 2)) : std::nullopt;
    if (!hb || !hb->color) return 0;
    lua_createtable(L, 0, 7);
    lua_createtable(L, 3, 0);
    for (int i = 0; i < 3; ++i) {
        lua_pushnumber(L, (*hb->color)[static_cast<std::size_t>(i)]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "color");
    lua_pushboolean(L, hb->lua);
    lua_setfield(L, -2, "lua");
    lua_pushboolean(L, hb->running);
    lua_setfield(L, -2, "running");
    lua_pushboolean(L, hb->enabled);
    lua_setfield(L, -2, "enabled");
    lua_pushnumber(L, hb->cooldown);
    lua_setfield(L, -2, "cooldown");
    lua_pushstring(L, hb->overlay.c_str());
    lua_setfield(L, -2, "overlay");
    lua_pushstring(L, hb->tooltip.c_str());
    lua_setfield(L, -2, "tooltip");
    return 1;
}

// checked = eq.HotButtonChecked(name, child)   -- the hotbutton drawn pressed: a pet command that is on (the pet
// window's SyncHotButtonStates)
int HotButtonChecked(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    auto hb = w ? w->hot_button(m.host, luaL_checkstring(L, 2)) : std::nullopt;
    lua_pushboolean(L, hb && hb->checked);
    return 1;
}

// st = eq.HotButtonState(name, child [, y0, y1])   -- a hotbutton's check and cool-down (CHotButtonWnd_UpdateButtonStates,
// game/hot_button_state.h), nil for an empty one: {checked, left (ms of the cool-down left, 0 idle), cooldown (the
// rectangle's fraction, nil idle), top (with y0 / y1: the rectangle's top, as eq.ButtonCoolDown), color = {r, g, b,
// a}, tint = {r, g, b, iconAlpha} (a spell gem's stage, as the gem bar shows it)}
int HotButtonState(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    auto hb = w ? w->hot_button(m.host, luaL_checkstring(L, 2)) : std::nullopt;
    if (!hb || hb->kind == "empty") return 0;
    lua_createtable(L, 0, 6);
    lua_pushboolean(L, hb->checked);
    lua_setfield(L, -2, "checked");
    lua_pushinteger(L, hb->cool_down ? hb->cool_down_left : 0);
    lua_setfield(L, -2, "left");
    if (hb->cool_down) {
        lua_pushnumber(L, *hb->cool_down);
        lua_setfield(L, -2, "cooldown");
        if (lua_isnumber(L, 3) && lua_isnumber(L, 4)) {
            lua_pushinteger(L, game::spell_gem_overlay_top(*hb->cool_down, saturate_cast<std::int32_t>(lua_tonumber(L, 3)),
                                                           saturate_cast<std::int32_t>(lua_tonumber(L, 4))));
            lua_setfield(L, -2, "top");
        }
    }
    const std::uint32_t argb = cool_down_color();
    lua_createtable(L, 4, 0);
    int k = 0;
    for (int shift : {16, 8, 0, 24}) {
        lua_pushnumber(L, static_cast<double>((argb >> shift) & 0xffu) / 255.0);
        lua_rawseti(L, -2, ++k);
    }
    lua_setfield(L, -2, "color");
    if (hb->gem_tint) {
        lua_createtable(L, 4, 0);
        for (int i = 0; i < 4; ++i) {
            lua_pushnumber(L, (*hb->gem_tint)[static_cast<std::size_t>(i)]);
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "tint");
    }
    return 1;
}

// info = eq.HotButtonInfo(name, child [, into [, y0, y1]])   -- what eq.HotButton, eq.HotButtonFace and
// eq.HotButtonState give, in one call (each of those works the button out again): the table `into` (kept by the caller
// from frame to frame) filled, or a new one. Fields: kind, label (eq.HotButton's), tex, u0, v0, u1, v1 (its icon, nil
// when none); social (true where eq.HotButtonFace gives a face) and with it color = {r, g, b}, lua, running, enabled,
// faceCooldown (the face's cooldown), overlay, tooltip (nil otherwise); state (true where eq.HotButtonState gives
// one: not empty) and with it checked, left, cooldown, top (with y0 / y1), and the spell gem's tint = {r, g, b,
// iconAlpha} with iconAlpha (nil otherwise). The cool-down's colour is eq.CoolDownColor()'s. The color and tint
// tables are refilled in place.
int HotButtonInfoFn(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const char* child = w ? luaL_checkstring(L, 2) : nullptr;
    const bool span = lua_isnumber(L, 4) && lua_isnumber(L, 5);
    const std::int32_t y0 = span ? number_to_int(lua_tonumber(L, 4)) : 0, y1 = span ? number_to_int(lua_tonumber(L, 5)) : 0;
    auto hb = w ? w->hot_button(m.host, child) : std::nullopt;
    const HotButtonInfo empty;
    const HotButtonInfo& info = hb ? *hb : empty;
    push_record(L, 3, 24);
    lua_pushstring(L, info.kind.c_str());
    lua_setfield(L, -2, "kind");
    lua_pushstring(L, info.label.c_str());
    lua_setfield(L, -2, "label");
    if (info.icon) {
        push_cell(L, *info.icon);
        set_art_fields(L, 5);
    } else {
        set_art_fields(L, 0);
    }
    // Fills the table in field `name` with `n` numbers (a new table when the field holds none).
    auto set_list = [L](const char* name, const float* v, int n) {
        lua_getfield(L, -1, name);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_createtable(L, n, 0);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, name);
        }
        for (int i = 0; i < n; ++i) {
            lua_pushnumber(L, v[i]);
            lua_rawseti(L, -2, i + 1);
        }
        lua_pop(L, 1);
    };
    const bool social = hb && hb->color;
    lua_pushboolean(L, social);
    lua_setfield(L, -2, "social");
    if (social) {
        set_list("color", hb->color->data(), 3);
        lua_pushboolean(L, hb->lua);
        lua_setfield(L, -2, "lua");
        lua_pushboolean(L, hb->running);
        lua_setfield(L, -2, "running");
        lua_pushboolean(L, hb->enabled);
        lua_setfield(L, -2, "enabled");
        set_number(L, "faceCooldown", hb->cooldown);
        lua_pushstring(L, hb->overlay.c_str());
        lua_setfield(L, -2, "overlay");
        lua_pushstring(L, hb->tooltip.c_str());
        lua_setfield(L, -2, "tooltip");
    } else {
        for (const char* f : {"color", "lua", "running", "enabled", "faceCooldown", "overlay", "tooltip"}) set_nil(L, f);
    }
    const bool state = hb && hb->kind != "empty";
    lua_pushboolean(L, state);
    lua_setfield(L, -2, "state");
    if (state) {
        lua_pushboolean(L, hb->checked);
        lua_setfield(L, -2, "checked");
        lua_pushinteger(L, hb->cool_down ? hb->cool_down_left : 0);
        lua_setfield(L, -2, "left");
    } else {
        set_nil(L, "checked");
        set_nil(L, "left");
    }
    if (state && hb->cool_down) set_number(L, "cooldown", *hb->cool_down);
    else set_nil(L, "cooldown");
    if (state && hb->cool_down && span) {
        lua_pushinteger(L, game::spell_gem_overlay_top(*hb->cool_down, y0, y1));
        lua_setfield(L, -2, "top");
    } else {
        set_nil(L, "top");
    }
    if (state && hb->gem_tint) {
        set_list("tint", hb->gem_tint->data(), 4);
        set_number(L, "iconAlpha", (*hb->gem_tint)[3]);
    } else {
        set_nil(L, "tint");
        set_nil(L, "iconAlpha");
    }
    return 1;
}

// ok = eq.ClickHotButton(name, child [, "left"|"right"|"rightheld" [, x, y]])   -- left: uses what the hotbutton
// holds (an empty one: its menu); right: a quick right click (an empty one: its menu); rightheld: the right button
// held 500 ms (its menu). The menu opens at the mouse, or at x, y when given (a pad's slot).
int ClickHotButton(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const char* child = w ? luaL_checkstring(L, 2) : nullptr;
    const std::string button = lua_isstring(L, 3) ? lua_tostring(L, 3) : "left";
    if (lua_isnumber(L, 4) && lua_isnumber(L, 5)) {
        const auto x = static_cast<float>(lua_tonumber(L, 4)), y = static_cast<float>(lua_tonumber(L, 5));
        m.host.click_rect = {x, y, x, y};
    } else {
        m.host.click_rect = {-1, -1, -1, -1};
    }
    lua_pushboolean(L, w && w->click_hot_button(m.host, child, button));
    return 1;
}

// ok = eq.PickUp(window, child [, row])   -- a drag from that child (a list's row) puts the hotbutton it makes on
// the cursor (the Actions window's buttons, an ability of the AA window, a bar's button)
int PickUp(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    int row = lua_isinteger(L, 3) ? static_cast<int>(lua_tointeger(L, 3)) - 1 : -1;
    auto b = w && !m.host.cursor_button && m.host.in_game ? w->pick_up(m.host, luaL_checkstring(L, 2), row) : std::nullopt;
    if (b) {
        m.host.cursor_button = *b;
        m.host.cursor_spell.reset();
        ++m.host.cursor_serial;
        m.pressed_holding = false;
    }
    lua_pushboolean(L, b.has_value());
    return 1;
}

// ok = eq.PressButtonFrom(window, child [, row])   -- runs the hotbutton a drag from that child (a list's row) would
// make, as pressing it on a hot bar does (DoHotButton), without putting it on the cursor: a pad's "use" for the
// Actions, AA, skills and combat ability windows' entries. Not for the hot bars (a drag takes their button off).
int PressButtonFrom(lua_State* L) {
    auto& m = Impl::of(L);
    const std::string_view name = luaL_checkstring(L, 1);
    StockWindow* w = name.starts_with("HotButtonWnd") ? nullptr : m.window(name);
    const char* child = w && m.host.in_game ? luaL_checkstring(L, 2) : nullptr;
    int row = lua_isinteger(L, 3) ? static_cast<int>(lua_tointeger(L, 3)) - 1 : -1;
    auto b = child ? w->pick_up(m.host, child, row) : std::nullopt;
    if (b && m.host.run_button) m.host.run_button(*b);
    lua_pushboolean(L, b && m.host.run_button);
    return 1;
}

// held = eq.HoldingButton()   -- a hotbutton is on the cursor
int HoldingButton(lua_State* L) {
    lua_pushboolean(L, Impl::of(L).host.cursor_button.has_value());
    return 1;
}

// eq.ClearCursorButton()   -- drops the cursor's hotbutton
int ClearCursorButton(lua_State* L) {
    Impl::of(L).host.cursor_button.reset();
    return 0;
}

// ok = eq.SpellGemClick(child [, "left"|"right"])   -- a click on that gem of the spell gem window: cast, or
// (right) forget the spell
int SpellGemClick(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window("CastSpellWnd");
    const char* id_arg = luaL_checkstring(L, 1);
    std::string phase = lua_isstring(L, 2) ? lua_tostring(L, 2) : "left";
    std::string id = id_arg;
    if (phase == "rightdown") {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (phase == "rightheld" || phase == "rightupheld") {
        // lbl_CCastSpellWnd_gem_rclickhold: the gem's spell in a spell display (CSpellDisplayManager::ShowSpell),
        // gone again when the button comes up.
        int g = id.rfind("CSPW_Spell", 0) == 0 ? std::atoi(id.c_str() + 10) : -1;
        if (phase == "rightupheld") {
            std::erase_if(m.host.spell_displays, [](const auto& s) { return s.second; });
        } else if (m.host.conn && g >= 0 && g < game::kSpellGems) {
            std::uint32_t spell = m.host.conn->spell_state().gems[static_cast<std::size_t>(g)].spell;
            if (spell != game::kNoSpell) m.host.show_spell(static_cast<std::int32_t>(spell), true);
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    bool right = phase == "right" || phase == "rightup";
    // The empty gem's spell select menu opens at the gem (HandleSpellRightClick): the gem's hit box the script just
    // tested, not the rectangle some earlier ClickChild or ClickSlot left.
    if (right) note_click_rect(m.host);
    lua_pushboolean(L, w && w->gem_click(m.host, id, right));
    return 1;
}

// item = eq.SlotItem(name, child)   -- the item in an item slot of a window (loot, bank, trade, give, ...)
int SlotItem(lua_State* L) {
    auto& m = Impl::of(L);
    const char* name = luaL_checkstring(L, 1);
    const char* child = luaL_checkstring(L, 2);
    StockWindow* w = m.window(name);
    const net::Item* item = w ? w->slot_item(m.host, child) : nullptr;
    push_item(L, item);
    m.set_item_cool_down(L, std::format("win:{}:{}", name, child), item);
    return 1;
}

// name, slots, combine, where = eq.BagInfo(bagWindow)   -- the bag's name, how many slots its window has, whether it
// is a tradeskill container, where it is ("inventory", "bank", "other") and its slot there; bagWindow "Bag1".."Bag36"
// or "WorldContainer" (the open tradeskill object in the zone)
int BagInfo(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    auto info = w ? w->bag_info(m.host) : std::nullopt;
    if (!info) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, info->first.c_str());
    lua_pushinteger(L, info->second);
    // ItemBase_IsCombineContainer @0x7b0300: a tradeskill container (CContainerWnd's ContainerType 2: Combine); a
    // world container by its object type
    const net::Item* bag = w->bag_item(m.host, -1);
    const net::ItemSlot* at = w->bag_slot(m.host);
    const bool world = at && at->type == net::kInvWorld && m.host.conn && m.host.conn->world_container();
    lua_pushboolean(L, (bag && game::is_combine_container(bag->bag_type)) ||
                           (world && game::is_combine_container(m.host.conn->world_container()->type)));
    lua_pushstring(L, !at ? "" : at->type == net::kInvPossessions ? "inventory" : at->type == net::kInvBank ? "bank" : "other");
    lua_pushinteger(L, at ? at->slot : -1);
    return 5;
}

// Hailstone: sorting, stacking, tidying and selling junk (net::GameConnection::start_item_job / start_sell_job).
// Places are written "i:<slot>[:<sub>]" (carried: possessions) and "b:<slot>[:<sub>]" (the bank's slot 0..).
std::optional<net::ItemSlot> parse_place(std::string_view text) {
    if (text.size() < 3 || (text[0] != 'i' && text[0] != 'b') || text[1] != ':') return std::nullopt;
    int slot = -1, sub = -1;
    std::string rest(text.substr(2));
    if (std::sscanf(rest.c_str(), "%d:%d", &slot, &sub) < 1 || slot < 0) return std::nullopt;
    return net::ItemSlot{text[0] == 'i' ? net::kInvPossessions : net::kInvBank, static_cast<std::int16_t>(slot),
                         static_cast<std::int16_t>(sub), -1, 0, 0};
}

const char* mode_name(net::GameConnection::ItemJobMode m) {
    using M = net::GameConnection::ItemJobMode;
    return m == M::Stack     ? "stack"
           : m == M::Tidy    ? "tidy"
           : m == M::Sell    ? "sell"
           : m == M::Destroy ? "destroy"
           : m == M::Tribute ? "tribute"
           : m == M::Withdraw ? "withdraw"
                             : "sort";
}

// err = eq.SortItems(where, mode [, options])   -- where: "inventory", "bank" or a bag window ("Bag3"); mode: "sort",
// "stack" (fill partial stacks from the later ones) or "tidy" (items into their filtered bags); options: { locked =
// { place, ... } (left alone), filters = { [bag place] = category } (eq.Item's category names) }. nil when it started
// (or had nothing to do), else why not
int SortItems(lua_State* L) {
    auto& m = Impl::of(L);
    std::string where = luaL_checkstring(L, 1);
    using M = net::GameConnection::ItemJobMode;
    M mode = M::Sort;
    if (lua_isboolean(L, 2)) mode = lua_toboolean(L, 2) ? M::Stack : M::Sort;
    else if (lua_isstring(L, 2)) {
        std::string_view v = lua_tostring(L, 2);
        mode = v == "stack" ? M::Stack : v == "tidy" ? M::Tidy : M::Sort;
    }
    if (!m.host.conn || !m.host.in_game) {
        lua_pushstring(L, "Not in game.");
        return 1;
    }
    net::GameConnection::ItemJobOptions opts;
    ui::lua::unview(L, 3);
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "locked");
        ui::lua::unview(L, -1);
        if (lua_istable(L, -1))
            for (lua_Integer i = 1;; ++i) {
                lua_rawgeti(L, -1, i);
                bool more = lua_isstring(L, -1);
                if (more)
                    if (auto at = parse_place(lua_tostring(L, -1))) opts.locked.push_back(*at);
                lua_pop(L, 1);
                if (!more) break;
            }
        lua_pop(L, 1);
        lua_getfield(L, 3, "filters");
        ui::lua::unview(L, -1);
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1))
                    if (auto at = parse_place(lua_tostring(L, -2)))
                        if (auto c = game::category_by_name(lua_tostring(L, -1)); c != game::ItemCategory::None)
                            opts.filters.push_back({at->parent(), c});
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    using S = net::GameConnection::ItemJobScope;
    std::string err;
    if (where == "inventory") {
        err = m.host.conn->start_item_job(S::Inventory, {}, mode, opts);
    } else if (where == "bank") {
        err = m.host.conn->start_item_job(S::Bank, {}, mode, opts);
    } else {
        StockWindow* w = m.window(where);
        const net::ItemSlot* at = w ? w->bag_slot(m.host) : nullptr;
        err = at ? m.host.conn->start_item_job(S::Bag, *at, mode, opts) : "That bag is not open.";
    }
    if (err.empty()) return 0;
    lua_pushstring(L, err.c_str());
    return 1;
}

// err = eq.SellJunk({ itemId, ... })   -- sells what you carry with those ids to the open merchant, one at a time
int SellJunk(lua_State* L) {
    auto& h = Impl::of(L).host;
    ui::lua::unview(L, 1);
    std::vector<std::uint32_t> ids;
    if (lua_istable(L, 1))
        for (lua_Integer i = 1;; ++i) {
            lua_rawgeti(L, 1, i);
            bool more = lua_isinteger(L, -1);
            if (more) ids.push_back(static_cast<std::uint32_t>(lua_tointeger(L, -1)));
            lua_pop(L, 1);
            if (!more) break;
        }
    std::string err = h.conn && h.in_game ? h.conn->start_sell_job(ids) : "Not in game.";
    if (err.empty()) return 0;
    lua_pushstring(L, err.c_str());
    return 1;
}

// done, total, mode, where = eq.ItemJob()   -- the job running (mode: "sort", "stack", "tidy", "sell", "destroy",
// "tribute", "withdraw"; where:
// "inventory", "bank" or "bag"); nil when none is
int ItemJob(lua_State* L) {
    auto& h = Impl::of(L).host;
    auto job = h.conn ? h.conn->item_job() : std::nullopt;
    if (!job) return 0;
    using S = net::GameConnection::ItemJobScope;
    lua_pushinteger(L, static_cast<lua_Integer>(job->done));
    lua_pushinteger(L, static_cast<lua_Integer>(job->total));
    lua_pushstring(L, mode_name(job->mode));
    lua_pushstring(L, job->scope == S::Inventory ? "inventory" : job->scope == S::Bank ? "bank" : "bag");
    return 4;
}

// eq.StopItemJob()
int StopItemJob(lua_State* L) {
    auto& h = Impl::of(L).host;
    if (h.conn) h.conn->stop_item_job();
    return 0;
}

// The bank's slot count: the open bank's, else every slot the bank can have.
int bank_slot_count(EqHost& h) { return h.conn->bank() ? h.conn->bank()->slots : 24; }

// count, slots, list = eq.FindItems(where, text)   -- the items whose name holds text (any case) in the general slots
// and their bags ("inventory"), the bank's slots and bags ("bank") or both ("all"); slots: { [top-level slot] =
// matches there, the bag's contents included } (the first place's kind); list: { { item (eq.Item's), place
// ("i:29:3"), bag (the bag's name, or nil), stale }, ... } in place order. Until the server has sent the bank in this
// zone (EQEmu's Inventory:LazyLoadBank sends it only near a banker) the bank's matches come from the bank as last
// seen (game/bank_snapshot.h; eq.BankSeen), marked stale.
int FindItems(lua_State* L) {
    auto& h = Impl::of(L).host;
    const char* where_arg = luaL_checkstring(L, 1);
    const char* text_arg = luaL_checkstring(L, 2);
    std::string where = where_arg, text = text_arg;
    auto lower = [](std::string v) {
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    text = lower(text);
    lua_Integer total = 0, listed = 0;
    lua_newtable(L); // slots
    lua_newtable(L); // list
    if (h.conn && h.in_game && !text.empty()) {
        std::optional<game::Inventory> seen; // the bank as last seen, while the bank is not known in this zone
        if (!h.conn->bank_live()) {
            seen.emplace();
            seen->load(h.conn->bank_seen().items);
        }
        bool stale = false;
        auto row = [&](const net::Item& it, const net::ItemSlot& at, const net::Item* bag) {
            lua_createtable(L, 0, 3);
            push_item(L, &it);
            lua_setfield(L, -2, "item");
            std::string place = std::format("{}:{}", at.type == net::kInvBank ? 'b' : 'i', at.slot) +
                                (at.sub >= 0 ? std::format(":{}", at.sub) : std::string());
            lua_pushstring(L, place.c_str());
            lua_setfield(L, -2, "place");
            if (bag) {
                lua_pushstring(L, bag->name.c_str());
                lua_setfield(L, -2, "bag");
            }
            if (stale) {
                lua_pushboolean(L, 1);
                lua_setfield(L, -2, "stale");
            }
            lua_rawseti(L, -2, ++listed);
        };
        auto scan = [&](std::int16_t type, int first, int last, bool count_slots) {
            stale = type == net::kInvBank && seen.has_value();
            const game::Inventory& inv = stale ? *seen : h.conn->inventory();
            for (int s = first; s <= last; ++s) {
                net::ItemSlot top{type, static_cast<std::int16_t>(s), -1, -1, 0, 0};
                const net::Item* it = inv.at(top);
                if (!it) continue;
                lua_Integer n = 0;
                if (lower(it->name).find(text) != std::string::npos) {
                    ++n;
                    row(*it, top, nullptr);
                }
                if (it->is_bag())
                    for (int i = 0; i < it->bag_slots; ++i) {
                        net::ItemSlot in = top;
                        in.sub = static_cast<std::int16_t>(i);
                        if (const net::Item* c = inv.at(in); c && lower(c->name).find(text) != std::string::npos) {
                            ++n;
                            row(*c, in, it);
                        }
                    }
                if (n == 0) continue;
                total += n;
                if (count_slots) {
                    lua_pushinteger(L, n);
                    lua_rawseti(L, -3, s);
                }
            }
        };
        if (where != "bank") scan(net::kInvPossessions, net::kSlotGeneral1, net::kSlotGeneral10, true);
        if (where != "inventory") scan(net::kInvBank, 0, bank_slot_count(h) - 1, where == "bank");
    }
    lua_pushinteger(L, total);
    lua_insert(L, -3);
    return 3;
}

// free, total = eq.InventorySpace()   -- the empty places in the general slots and their bags (a general slot with a
// bag counts as the bag's slots)
int InventorySpace(lua_State* L) {
    auto& h = Impl::of(L).host;
    lua_Integer free = 0, total = 0;
    if (h.conn && h.in_game) {
        const auto& inv = h.conn->inventory();
        for (int s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s) {
            if (!h.conn->slot_available(s)) continue;
            const net::Item* it = inv.at(net::ItemSlot::possessions(static_cast<std::int16_t>(s)));
            if (!it || !it->is_bag()) {
                ++total;
                if (!it) ++free;
                continue;
            }
            for (int i = 0; i < it->bag_slots; ++i) {
                ++total;
                if (!inv.at(net::ItemSlot::possessions(static_cast<std::int16_t>(s), static_cast<std::int16_t>(i)))) ++free;
            }
        }
    }
    lua_pushinteger(L, free);
    lua_pushinteger(L, total);
    return 2;
}

// parcels, claiming, busy, sending, picked, money = eq.Parcels()   -- the parcels waiting at the open parcel merchant (EQEmu's), by slot order:
// { { slot, item (eq.Item's), from, note, sent (Unix seconds) } }; claiming: how many are still to be taken (Claim
// all's queue and the one being taken; 0 when none is); busy: a retrieve is waiting for its answer; sending: a parcel
// sent is; picked: the item picked in the bags (eq.Item's), the one Send sends; money: the last money picked for
// sending with the inventory's coin buttons on the Parcels page, { coin = 0 platinum .. 3 copper, amount, serial
// (new for each pick) }, or nil (GameConnection::set_parcel_money). nil without a merchant
int Parcels(lua_State* L) {
    auto& h = Impl::of(L).host;
    const auto* m = h.conn ? &h.conn->merchant() : nullptr;
    if (!m || !*m || !(*m)->open) return 0;
    const auto& ms = **m;
    lua_createtable(L, static_cast<int>(ms.parcels.size()), 0);
    lua_Integer n = 0;
    for (const auto& [slot, p] : ms.parcels) {
        lua_createtable(L, 0, 5);
        lua_pushinteger(L, slot);
        lua_setfield(L, -2, "slot");
        push_item(L, &p.item);
        lua_setfield(L, -2, "item");
        lua_pushstring(L, p.from.c_str());
        lua_setfield(L, -2, "from");
        lua_pushstring(L, p.note.c_str());
        lua_setfield(L, -2, "note");
        lua_pushinteger(L, p.sent);
        lua_setfield(L, -2, "sent");
        lua_rawseti(L, -2, ++n);
    }
    lua_pushinteger(L, static_cast<lua_Integer>(ms.claiming.size()) + (ms.parcel_pending ? 1 : 0));
    lua_pushboolean(L, ms.parcel_pending);
    lua_pushboolean(L, ms.parcel_sending);
    // the item picked in the bags (the merchant's considered one), the one Send sends
    const net::Item* picked = ms.selected ? h.conn->inventory().at(*ms.selected) : nullptr;
    if (picked) push_item(L, picked);
    else lua_pushnil(L);
    if (const auto& pm = ms.parcel_money) {
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, pm->coin);
        lua_setfield(L, -2, "coin");
        lua_pushinteger(L, pm->amount);
        lua_setfield(L, -2, "amount");
        lua_pushinteger(L, pm->serial);
        lua_setfield(L, -2, "serial");
    } else {
        lua_pushnil(L);
    }
    return 6;
}

// err = eq.SendParcel(to, note, count [, place])   -- sends the item picked in the bags (or the one at the place,
// "i:29:3") by parcel, `count` of a stack; err = eq.SendParcelMoney(to, note, pp, gp, sp, cp)   -- sends money.
// err: why not (nil when sent; the server's answer comes in the chat)
int SendParcel(lua_State* L) {
    auto& h = Impl::of(L).host;
    const auto* m = h.conn ? &h.conn->merchant() : nullptr;
    if (!m || !*m || !(*m)->open) {
        lua_pushstring(L, "Open a parcel merchant first.");
        return 1;
    }
    std::optional<net::ItemSlot> at = lua_isstring(L, 4) ? parse_place(lua_tostring(L, 4)) : (*m)->selected;
    if (!at) {
        lua_pushstring(L, "Pick an item from your bags to send.");
        return 1;
    }
    // (every argument checked before a C++ object is made: a Lua error longjmps past destructors)
    const lua_Integer count = luaL_optinteger(L, 3, 1);
    const char* to = luaL_checkstring(L, 1);
    const char* note = luaL_optstring(L, 2, "");
    std::string why = h.conn->send_parcel(*at, static_cast<std::uint32_t>(std::max<lua_Integer>(1, count)), to, note);
    if (why.empty()) return 0;
    lua_pushstring(L, why.c_str());
    return 1;
}

int SendParcelMoney(lua_State* L) {
    auto& h = Impl::of(L).host;
    if (!h.conn) return 0;
    std::array<std::uint32_t, 4> coins{};
    for (int i = 0; i < 4; ++i) coins[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(std::max<lua_Integer>(0, luaL_optinteger(L, 3 + i, 0)));
    const char* to = luaL_checkstring(L, 1);
    const char* note = luaL_optstring(L, 2, "");
    std::string why = h.conn->send_parcel_money(coins, to, note);
    if (why.empty()) return 0;
    lua_pushstring(L, why.c_str());
    return 1;
}

// eq.RetrieveParcel(slot)   -- takes one parcel;  eq.ClaimAllParcels()   -- takes them all, one after another
int RetrieveParcel(lua_State* L) {
    auto& h = Impl::of(L).host;
    if (h.conn) h.conn->retrieve_parcel(static_cast<std::uint32_t>(luaL_checkinteger(L, 1)));
    return 0;
}

int ClaimAllParcels(lua_State* L) {
    auto& h = Impl::of(L).host;
    if (h.conn) h.conn->claim_all_parcels();
    return 0;
}

// ok = eq.PlaceClick(place, button)   -- the slot logic at a place ("i:29:3", "b:0:2"; a bank place needs the bank
// open): "left" picks up (Shift: the whole stack) or puts down, "open" opens the bag holding it (or the bag itself)
int PlaceClick(lua_State* L) {
    auto& h = Impl::of(L).host;
    auto at = parse_place(luaL_checkstring(L, 1));
    std::string button = lua_isstring(L, 2) ? lua_tostring(L, 2) : "left";
    if (!at || !h.conn || !h.in_game || (at->type == net::kInvBank && !h.conn->bank())) {
        lua_pushboolean(L, false);
        return 1;
    }
    if (button == "open") {
        const net::ItemSlot top = at->parent();
        const net::Item* bag = h.conn->inventory().at(top);
        if (bag && bag->is_bag()) open_container(h, top);
    } else {
        h.conn->click_slot(*at, ImGui::GetIO().KeyShift, ImGui::GetIO().KeyCtrl);
    }
    lua_pushboolean(L, true);
    return 1;
}

// item = eq.BagSlotItem(bagWindow, index)   -- index from 0
int BagSlotItem(lua_State* L) {
    auto& m = Impl::of(L);
    const char* name = luaL_checkstring(L, 1);
    const int index = static_cast<int>(luaL_checkinteger(L, 2));
    StockWindow* w = m.window(name);
    const net::Item* item = w ? w->bag_item(m.host, index) : nullptr;
    push_item(L, item);
    m.set_item_cool_down(L, std::format("bag:{}:{}", name, index), item);
    return 1;
}

// ok = eq.BagSlotClick(bagWindow, index [, button [, outside]])   -- the slot logic on that bag slot ("leftheld" as
// eq.ClickSlot takes it: false when no hotbutton went on the cursor)
int BagSlotClick(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    int index = static_cast<int>(luaL_checkinteger(L, 2));
    std::string button = lua_isstring(L, 3) ? lua_tostring(L, 3) : "left";
    if (button == "rightheld" && w && w->bag_item(m.host, index) && index >= 0) {
        inspect_held(m.host, w->bag_item(m.host, index));
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, w && w->bag_click(m.host, index, button, lua_toboolean(L, 4) != 0));
    return 1;
}

// rows, columns, selected, labels, widths = eq.ListInfo(name, child)   -- nil when the child is not a list
int ListInfo(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const ListData* d = w ? m.list(*w, luaL_checkstring(L, 2)) : nullptr;
    if (!d) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(d->rows.size()));
    lua_pushinteger(L, d->columns);
    lua_pushinteger(L, d->selected);
    lua_createtable(L, d->columns, 0);
    for (int i = 0; i < d->columns; ++i) {
        lua_pushstring(L, i < static_cast<int>(d->labels.size()) ? d->labels[static_cast<std::size_t>(i)].c_str() : "");
        lua_rawseti(L, -2, i + 1);
    }
    lua_createtable(L, d->columns, 0);
    for (int i = 0; i < d->columns; ++i) {
        lua_pushinteger(L, i < static_cast<int>(d->widths.size()) ? d->widths[static_cast<std::size_t>(i)] : 0);
        lua_rawseti(L, -2, i + 1);
    }
    if (d->multi.empty()) return 5;
    lua_createtable(L, 0, static_cast<int>(d->multi.size())); // multi[row] = true (rows from 0)
    for (int r : d->multi) {
        lua_pushboolean(L, 1);
        lua_rawseti(L, -2, r);
    }
    return 6;
}

// texts, r, g, b [, texture, u0, v0, u1, v1] = eq.ListRow(name, child, row)   -- row from 0; texts.altRight,
// texts.comboColumn, texts.disabled, texts.iconColumn
// (from 0) when the icon is not in the first column
int ListRowFn(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const ListData* d = w ? m.list(*w, luaL_checkstring(L, 2)) : nullptr;
    auto row = luaL_checkinteger(L, 3);
    if (!d || row < 0 || row >= static_cast<lua_Integer>(d->rows.size())) {
        lua_pushnil(L);
        return 1;
    }
    const ListRow& r = d->rows[static_cast<std::size_t>(row)];
    lua_createtable(L, static_cast<int>(r.texts.size()), 0);
    for (std::size_t i = 0; i < r.texts.size(); ++i) {
        lua_pushstring(L, r.texts[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    if (!r.cell_colors.empty()) { // texts.colors[column] = {r, g, b}
        lua_createtable(L, static_cast<int>(r.cell_colors.size()), 0);
        for (std::size_t i = 0; i < r.cell_colors.size(); ++i) {
            if (!r.cell_colors[i]) continue;
            lua_createtable(L, 3, 0);
            for (int k = 0; k < 3; ++k) {
                lua_pushnumber(L, (*r.cell_colors[i])[static_cast<std::size_t>(k)]);
                lua_rawseti(L, -2, k + 1);
            }
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        lua_setfield(L, -2, "colors");
    }
    if (r.icon_column != 0) { // texts.iconColumn: the icon's column, from 0
        lua_pushinteger(L, r.icon_column);
        lua_setfield(L, -2, "iconColumn");
    }
    if (d->combo_column >= 0) { // texts.comboColumn: the column holding a combo box, the child "<list>:<row>"
        lua_pushinteger(L, d->combo_column);
        lua_setfield(L, -2, "comboColumn");
    }
    if (d->alt_right_click) { // texts.altRight: an Alt right click on the row is the window's
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "altRight");
    }
    if (r.disabled) { // texts.disabled: the window left the line disabled
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "disabled");
    }
    for (float f : r.color) lua_pushnumber(L, f);
    if (!r.icon) return 4;
    push_cell(L, *r.icon);
    return 9;
}

// ok = eq.ListClick(name, child, row [, "left"|"right"|"double" [, column [, mods]]])   -- "double": a double
// click, after the click's own "left"; mods (1 shift, 2 ctrl) for a list that selects several rows
int ListClick(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const std::string_view button = lua_isstring(L, 4) ? lua_tostring(L, 4) : "left"; // (nothing to destroy)
    bool right = button == "right";
    int column = static_cast<int>(luaL_optinteger(L, 5, 0));
    if (button == "double") {
        lua_pushboolean(L, w && w->list_double_click(m.host, luaL_checkstring(L, 2), static_cast<int>(luaL_checkinteger(L, 3)), column));
        return 1;
    }
    if (!right && w && lua_isinteger(L, 6) &&
        w->list_select(m.host, luaL_checkstring(L, 2), static_cast<int>(luaL_checkinteger(L, 3)), static_cast<int>(lua_tointeger(L, 6)))) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, w && w->list_click(m.host, luaL_checkstring(L, 2), static_cast<int>(luaL_checkinteger(L, 3)),
                                          right, column));
    return 1;
}

// ok = eq.ListHeaderClick(name, child, column)   -- a click on a list's column header (0-based)
int ListHeaderClick(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    lua_pushboolean(L, w && w->list_header_click(m.host, luaL_checkstring(L, 2), static_cast<int>(luaL_checkinteger(L, 3))));
    return 1;
}

// choices, selected = eq.ComboInfo(name, child)   -- the combo box's choices and the chosen one (0-based); nil when
// the window does not fill it
int ComboInfo(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const ComboData* d = w ? w->combo(m.host, luaL_checkstring(L, 2)) : nullptr;
    if (!d) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, static_cast<int>(d->choices.size()), 0);
    for (std::size_t i = 0; i < d->choices.size(); ++i) {
        lua_pushstring(L, d->choices[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_pushinteger(L, d->selected);
    return 2;
}

// value = eq.Option(name)   -- a client option (ui/<pack>/options.lua; ClientApp::option_value); nil for one the
// client does not have
int Option(lua_State* L) {
    auto& m = Impl::of(L);
    std::optional<int> v = m.host.find_option ? m.host.find_option(luaL_checkstring(L, 1)) : std::nullopt;
    if (v) lua_pushinteger(L, *v);
    else lua_pushnil(L);
    return 1;
}

// value = eq.UiIni(section, key)   -- a value of the character's UI ini (over defaults.ini), nil when neither has it
int UiIniGet(lua_State* L) {
    auto& m = Impl::of(L);
    if (auto v = m.layout.get(luaL_checkstring(L, 1), luaL_checkstring(L, 2))) lua_pushstring(L, v->c_str());
    else lua_pushnil(L);
    return 1;
}

// eq.SetUiIni(section, key, value)   -- writes a value to the character's UI ini (UIIni_WriteString) and saves it
int UiIniSet(lua_State* L) {
    auto& m = Impl::of(L);
    m.layout.put(luaL_checkstring(L, 1), luaL_checkstring(L, 2), luaL_checkstring(L, 3));
    m.layout.save();
    return 0;
}

// eq.SetOption(name, value)   -- changes, applies and saves a client option
int SetOption(lua_State* L) {
    auto& m = Impl::of(L);
    if (m.host.set_option) m.host.set_option(luaL_checkstring(L, 1), static_cast<int>(luaL_checkinteger(L, 2)));
    return 0;
}

// choices = eq.OptionChoices(name)   -- the choices the client lists for an option ("resolution"), else empty
int OptionChoices(lua_State* L) {
    auto& m = Impl::of(L);
    const char* name = luaL_checkstring(L, 1);
    std::vector<std::string> choices;
    if (m.host.option_choices) choices = m.host.option_choices(name);
    lua_createtable(L, static_cast<int>(choices.size()), 0);
    for (std::size_t i = 0; i < choices.size(); ++i) {
        lua_pushstring(L, choices[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// ok = eq.ComboSelect(name, child, index)   -- picks choice `index` (0-based), as the stock drop-down does
int ComboSelect(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    lua_pushboolean(L, w && w->combo_select(m.host, luaL_checkstring(L, 2), static_cast<int>(luaL_checkinteger(L, 3))));
    return 1;
}

// rows = eq.WindowList(name, child)   -- each row a table of column texts
int WindowList(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const ListData* d = w ? m.list(*w, luaL_checkstring(L, 2)) : nullptr;
    lua_newtable(L);
    if (!d) return 1;
    for (std::size_t i = 0; i < d->rows.size(); ++i) {
        lua_newtable(L);
        for (std::size_t c = 0; c < d->rows[i].texts.size(); ++c) {
            lua_pushstring(L, d->rows[i].texts[c].c_str());
            lua_rawseti(L, -2, static_cast<lua_Integer>(c + 1));
        }
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// index = eq.TabPage(name, child)   -- the page the window's tab box shows (0-based), nil if it is not one
int TabPage(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    auto p = w ? w->tab_page(m.host, luaL_checkstring(L, 2)) : std::nullopt;
    if (p) lua_pushinteger(L, *p);
    else lua_pushnil(L);
    return 1;
}

// eq.SetTabPage(name, child, index)
int SetTabPage(lua_State* L) {
    auto& m = Impl::of(L);
    if (StockWindow* w = m.window(luaL_checkstring(L, 1)))
        w->set_tab_page(m.host, luaL_checkstring(L, 2), static_cast<int>(luaL_checkinteger(L, 3)));
    return 0;
}

// text = eq.WindowTitle(name)   -- the title the game shows on the window (a merchant's name); "" for the XML's
int WindowTitle(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    std::string t = w ? w->title(m.host) : std::string();
    lua_pushstring(L, t.c_str());
    return 1;
}

// x, y, w, h, relative [, right, bottom] = eq.ChildRect(window, child)   -- where the window's code placed the
// child, or nil; relative: inside the child's parent piece (the models here place children as the original does, in
// their parent); right, bottom: the child's right and bottom stay that far inside its parent's (w and h unused)
int ChildRect(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    std::string id = luaL_checkstring(L, 2);
    auto r = w ? w->child_rect(m.host, id) : std::nullopt;
    if (!r) {
        lua_pushnil(L);
        return 1;
    }
    for (int v : *r) lua_pushinteger(L, v);
    lua_pushboolean(L, 1);
    auto a = w->child_anchor(m.host, id);
    if (!a) return 5;
    for (int v : *a) lua_pushinteger(L, v);
    return 7;
}

// w, h = eq.WindowClient(window [, frameW, frameH])   -- the client size of a window that sizes itself in code (the
// item display), or nil: its own size (CXWnd::Resize, frame included) less the frame the caller draws for it
int WindowClient(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    auto s = w ? w->window_size(m.host) : std::nullopt;
    if (!s) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (*s)[0] - static_cast<int>(luaL_optinteger(L, 2, 0)));
    lua_pushinteger(L, (*s)[1] - static_cast<int>(luaL_optinteger(L, 3, 0)));
    return 2;
}

// eq.SizeWindow(window, w, h [, frameW, frameH])   -- the user resized such a window to a w x h client area
int SizeWindow(lua_State* L) {
    auto& m = Impl::of(L);
    if (StockWindow* w = m.window(luaL_checkstring(L, 1)))
        w->size_window(m.host, saturate_cast<int>(luaL_checknumber(L, 2) + luaL_optnumber(L, 4, 0)),
                       saturate_cast<int>(luaL_checknumber(L, 3) + luaL_optnumber(L, 5, 0)));
    return 0;
}

// ok = eq.SetSlider(window, child, value)   -- drags the window's slider to value
int SetSlider(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    lua_pushboolean(L, w && w->set_slider(m.host, luaL_checkstring(L, 2), static_cast<int>(luaL_checkinteger(L, 3))));
    return 1;
}

// a = eq.CursorAttachment()   -- what CCursorAttachment carries on the mouse (an item), nil when nothing:
// { icon, iconRect, rect, quantity, font, spell (a book spell's name), coin (money: 0 platinum .. 3 copper, its
// count in quantity, no icon: the pack draws the coin) }; images { texture, u0, v0, u1, v1 }, rects { x, y, w, h } on the screen.
// The window (EQUI_CursorAttachment.xml, 50 x 50) sits at the mouse with the icon (CA_Anim2) at 5, 5, 40 x 40;
// DrawQuantity writes a stack's count in a dark box at its bottom right.
int CursorAttachment(lua_State* L) {
    auto& m = Impl::of(L);
    m.replaced_at["CursorAttachment"] = m.frame;
    const net::Item* held = m.host.conn && m.host.in_game ? m.host.conn->inventory().cursor() : nullptr;
    auto icon = held && m.host.textures ? m.host.textures->item_icon(held->icon) : std::nullopt;
    const game::HotButton* button = !held && m.host.in_game && m.host.cursor_button ? &*m.host.cursor_button : nullptr;
    // A spell from the book (type 1): AttachToCursor puts the book slot's icon in CA_Anim2, with no background.
    const game::SpellData* spell = nullptr;
    if (!held && !button && m.host.in_game && m.host.cursor_spell && m.host.conn) {
        int slot = *m.host.cursor_spell;
        std::uint32_t id = slot >= 0 && slot < game::kSpellBookSize ? m.host.conn->spell_state().book[static_cast<std::size_t>(slot)] : game::kNoSpell;
        spell = id == game::kNoSpell ? nullptr : m.host.conn->spell_table().get(id);
        if (spell && m.host.textures) icon = m.host.textures->spell_icon(spell->spell_icon);
    }
    // Money (CCursorAttachment type 3): the coin MoveMoney left on the cursor, the first held counter from platinum
    // (CDisplay +0x11), and its count (+0x244), drawn as a stack's quantity is.
    int coin = -1;
    std::uint32_t coin_count = 0;
    if (!held && !button && !spell && m.host.in_game && m.host.conn) {
        const auto& cc = m.host.conn->cursor_coins(); // by wire type, 0 copper .. 3 platinum
        for (int t = 3; t >= 0 && coin < 0; --t)
            if (cc[static_cast<std::size_t>(t)]) coin = 3 - t, coin_count = cc[static_cast<std::size_t>(t)];
    }
    if (!held && !button && !spell && coin < 0) {
        lua_pushnil(L);
        return 1;
    }
    // (without a mouse over the window, the middle of the screen)
    ImVec2 mouse = ImGui::IsMousePosValid() ? ImGui::GetMousePos() : ImGui::GetMainViewport()->GetCenter();
    auto rect = [&](float x, float y, float w, float h) {
        lua_createtable(L, 4, 0);
        float v[4] = {x, y, w, h};
        for (int i = 0; i < 4; ++i) {
            lua_pushnumber(L, v[i]);
            lua_rawseti(L, -2, i + 1);
        }
    };
    lua_createtable(L, 0, 6);
    if (button) {
        // A hotbutton (CCursorAttachment type 2): the button's face, 40 x 40 at the mouse, its icon and text.
        HotButtonInfo info = hot_button_info(m.host, *button, m.host.button_label ? m.host.button_label(*button) : button->label);
        icon = info.icon;
        lua_pushstring(L, info.label.c_str());
        lua_setfield(L, -2, "text");
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "button");
        rect(mouse.x, mouse.y, 40, 40);
        lua_setfield(L, -2, "bgRect");
    }
    if (icon) {
        lua_createtable(L, 5, 0);
        push_cell(L, *icon);
        for (int i = 5; i >= 1; --i) lua_rawseti(L, -1 - i, i);
        lua_setfield(L, -2, "icon");
        rect(mouse.x + 5, mouse.y + 5, 40, 40);
        lua_setfield(L, -2, "iconRect");
    }
    rect(mouse.x, mouse.y, 50, 50);
    lua_setfield(L, -2, "rect");
    if (held && held->stackable() && held->stack_count >= 1) {
        lua_pushinteger(L, held->stack_count);
        lua_setfield(L, -2, "quantity");
    }
    if (coin >= 0) {
        lua_pushinteger(L, coin);
        lua_setfield(L, -2, "coin");
        lua_pushinteger(L, static_cast<lua_Integer>(coin_count));
        lua_setfield(L, -2, "quantity");
        rect(mouse.x + 5, mouse.y + 5, 40, 40);
        lua_setfield(L, -2, "iconRect");
    }
    lua_pushinteger(L, 2);
    lua_setfield(L, -2, "font");
    if (spell) { // (a spell from the book: its name, for the windows that say what is held)
        lua_pushstring(L, spell->name.c_str());
        lua_setfield(L, -2, "spell");
    }
    return 1;
}

// menus = eq.CascadeMenus()   -- the open EQ menu and submenus: { x, y, w, h, items = { { kind, text, rect, icon,
// decalW, decalH, textX, enabled, arrow, arrowRect, background } } } (app/lua_eq_menu.h)
int CascadeMenus(lua_State* L) {
    auto& m = Impl::of(L);
    if (!m.host.in_game) m.cascade.close();
    std::vector<std::string> plugin_rows;
    if (m.host.plugin_ui)
        for (const auto& e : m.host.plugin_ui->menu()) plugin_rows.push_back(e.label);
    m.cascade.set_plugin_entries(std::move(plugin_rows));
    auto views = m.cascade.views(m.host.textures);
    auto rect = [&](const std::array<float, 4>& r) {
        lua_createtable(L, 4, 0);
        for (int i = 0; i < 4; ++i) {
            lua_pushnumber(L, r[static_cast<std::size_t>(i)]);
            lua_rawseti(L, -2, i + 1);
        }
    };
    auto cell = [&](const ui::UiTextures::Cell& c) {
        lua_createtable(L, 5, 0);
        push_cell(L, c);
        for (int i = 5; i >= 1; --i) lua_rawseti(L, -1 - i, i);
    };
    lua_createtable(L, static_cast<int>(views.size()), 0);
    for (std::size_t k = 0; k < views.size(); ++k) {
        const auto& v = views[k];
        lua_createtable(L, 0, 5);
        lua_pushnumber(L, v.x), lua_setfield(L, -2, "x");
        lua_pushnumber(L, v.y), lua_setfield(L, -2, "y");
        lua_pushnumber(L, v.w), lua_setfield(L, -2, "w");
        lua_pushnumber(L, v.h), lua_setfield(L, -2, "h");
        lua_createtable(L, static_cast<int>(v.items.size()), 0);
        for (std::size_t i = 0; i < v.items.size(); ++i) {
            const auto& it = v.items[i];
            lua_createtable(L, 0, 10);
            lua_pushstring(L, it.kind.c_str()), lua_setfield(L, -2, "kind");
            lua_pushstring(L, it.text.c_str()), lua_setfield(L, -2, "text");
            rect(it.rect), lua_setfield(L, -2, "rect");
            if (it.icon) cell(*it.icon), lua_setfield(L, -2, "icon");
            if (it.background) cell(*it.background), lua_setfield(L, -2, "background");
            lua_pushnumber(L, 24), lua_setfield(L, -2, "decalW");
            lua_pushnumber(L, 24), lua_setfield(L, -2, "decalH");
            lua_pushboolean(L, it.enabled), lua_setfield(L, -2, "enabled");
            if (it.arrow) {
                cell(*it.arrow), lua_setfield(L, -2, "arrow");
                rect(it.arrow_rect), lua_setfield(L, -2, "arrowRect");
            }
            lua_rawseti(L, -2, static_cast<int>(i) + 1);
        }
        lua_setfield(L, -2, "items");
        lua_rawseti(L, -2, static_cast<int>(k) + 1);
    }
    return 1;
}

// eq.CascadeClick(menu, item) / eq.CascadeHover(menu, item) / eq.CascadeClose()   -- 1-based menu and item
int CascadeClick(lua_State* L) {
    auto& m = Impl::of(L);
    auto cmd = m.cascade.click(static_cast<int>(luaL_checkinteger(L, 1)) - 1, static_cast<int>(luaL_checkinteger(L, 2)) - 1);
    if (cmd && *cmd == kMenuReportBug) {
        if (m.host.conn) m.host.conn->set_bug_report_open(true);
    } else if (cmd && *cmd == kMenuScripts) {
        if (StockWindow* w = m.window("ScriptsWnd")) w->set_shown(m.host, true);
    } else if (cmd && *cmd == kMenuWebBrowser) {
        if (StockWindow* w = m.window("BrowserWnd")) w->set_shown(m.host, true);
    } else if (cmd && *cmd >= kMenuPluginFirst) { // a plugin's entry: its command line, as the plugin
        run_plugin_menu(m.host, static_cast<std::size_t>(*cmd - kMenuPluginFirst));
    } else if (cmd && m.host.run_command) m.host.run_command(*cmd);
    return 0;
}
int CascadeHover(lua_State* L) {
    auto& m = Impl::of(L);
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    m.cascade.hover(static_cast<int>(luaL_checkinteger(L, 1)) - 1, static_cast<int>(luaL_checkinteger(L, 2)) - 1,
                    static_cast<int>(ds.x), static_cast<int>(ds.y));
    return 0;
}
int CascadeClose(lua_State* L) {
    Impl::of(L).cascade.close();
    return 0;
}

// menus = eq.GameMenus([scale])   -- the open context menus: { x, y, w, h, rows = { { text, r, g, b, check,
// enabled, separator, submenu } } } (EqHost::menus; sized for EQ font 2 at `scale`, 1 by default, 0.5..3, as
// menus.lua draws them: a pack that zooms its menus draws the font at that scale)
int GameMenus(lua_State* L) {
    auto& m = Impl::of(L);
    const float scale = std::clamp(static_cast<float>(luaL_optnumber(L, 1, 1.0)), 0.5f, 3.0f);
    lua_newtable(L);
    ImFont* font = ui::eq_font(2);
    float fs = (font ? font->FontSize : ImGui::GetFontSize()) * scale;
    const ImGuiStyle& st = ImGui::GetStyle();
    int n = 0;
    for (auto& menu : m.host.menus) {
        float w = 0, y = st.WindowPadding.y;
        menu.row_y.clear();
        for (const auto& r : menu.rows) {
            menu.row_y.push_back(y);
            if (r.separator) {
                y += st.ItemSpacing.y + 1;
                continue;
            }
            float tw = font ? font->CalcTextSizeA(fs, FLT_MAX, 0.0f, r.text.c_str()).x : ImGui::CalcTextSize(r.text.c_str()).x * scale;
            w = std::max(w, tw);
            y += fs + st.ItemSpacing.y;
        }
        menu.w = w + fs * 3.0f + st.WindowPadding.x * 2; // the check and the submenu arrow beside the text
        menu.h = y + st.WindowPadding.y;
        // kept on the screen (CContextMenuManager::PopupMenu moves a menu that would leave it)
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        if (ds.x > 0 && menu.x + menu.w > ds.x) menu.x = std::max(0.0f, ds.x - menu.w);
        if (ds.y > 0 && menu.y + menu.h > ds.y) menu.y = std::max(0.0f, ds.y - menu.h);
        lua_createtable(L, 0, 5);
        auto num = [&](const char* k, double v) {
            lua_pushnumber(L, v);
            lua_setfield(L, -2, k);
        };
        num("x", menu.x), num("y", menu.y), num("w", menu.w), num("h", menu.h);
        lua_createtable(L, static_cast<int>(menu.rows.size()), 0);
        int i = 0;
        for (const auto& r : menu.rows) {
            lua_createtable(L, 0, 8);
            lua_pushstring(L, r.text.c_str());
            lua_setfield(L, -2, "text");
            num("r", ((r.rgb >> 16) & 0xff) / 255.0), num("g", ((r.rgb >> 8) & 0xff) / 255.0), num("b", (r.rgb & 0xff) / 255.0);
            lua_pushboolean(L, r.check);
            lua_setfield(L, -2, "check");
            lua_pushboolean(L, r.enabled);
            lua_setfield(L, -2, "enabled");
            lua_pushboolean(L, r.separator);
            lua_setfield(L, -2, "separator");
            lua_pushboolean(L, r.submenu != nullptr);
            lua_setfield(L, -2, "submenu");
            lua_rawseti(L, -2, ++i);
        }
        lua_setfield(L, -2, "rows");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// A row of an open context menu hovered (0-based menu and row): the menus after it close; a submenu row opens its
// submenu beside the row.
void menu_hover(EqHost& h, std::size_t mi, std::size_t ri) {
    if (mi >= h.menus.size() || ri >= h.menus[mi].rows.size()) return;
    h.menus.resize(mi + 1);
    const auto& row = h.menus[mi].rows[ri];
    if (!row.submenu || !row.enabled) return;
    EqHost::GameMenu sub;
    sub.x = h.menus[mi].x + h.menus[mi].w;
    sub.y = h.menus[mi].y + (ri < h.menus[mi].row_y.size() ? h.menus[mi].row_y[ri] : 0.0f);
    sub.rows = row.submenu();
    h.menus.push_back(std::move(sub));
}

// Chosen: runs the row and closes the menus (a submenu row opens its submenu).
bool menu_choose(EqHost& h, std::size_t mi, std::size_t ri) {
    if (mi >= h.menus.size() || ri >= h.menus[mi].rows.size()) return false;
    const auto& row = h.menus[mi].rows[ri];
    if (row.separator || !row.enabled) return false;
    if (row.submenu) {
        menu_hover(h, mi, ri);
        return true;
    }
    auto act = row.act;
    h.menus.clear();
    if (act) act();
    return true;
}

// eq.GameMenuHover(menu, row)   -- 1-based menu, 0-based row
int GameMenuHover(lua_State* L) {
    menu_hover(Impl::of(L).host, static_cast<std::size_t>(luaL_checkinteger(L, 1) - 1),
               static_cast<std::size_t>(luaL_checkinteger(L, 2)));
    return 0;
}

// eq.GameMenuClick(menu, row)   -- 1-based menu, 0-based row
int GameMenuClick(lua_State* L) {
    menu_choose(Impl::of(L).host, static_cast<std::size_t>(luaL_checkinteger(L, 1) - 1),
                static_cast<std::size_t>(luaL_checkinteger(L, 2)));
    return 0;
}

int GameMenuClose(lua_State* L) {
    Impl::of(L).host.menus.clear();
    return 0;
}

// taken = eq.WindowContextMenu(name)   -- a right click on the window: its context menu at the mouse, if it has one
// (taken, and nothing opens, while context menus are off: the window's own menu is one too)
int WindowContextMenu(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    ImVec2 mp = ImGui::GetMousePos();
    lua_pushboolean(L, !m.host.context_menus() || (w && w->context_menu(m.host, mp.x, mp.y)));
    return 1;
}

// on = eq.ContextMenus()   -- whether a right click may open a context menu (EqHost::context_menus)
int ContextMenus(lua_State* L) {
    lua_pushboolean(L, Impl::of(L).host.context_menus());
    return 1;
}

// yes = eq.TakeWindowMenu(name)   -- a context menu's "Window" row asked for the window's own menu (once)
int TakeWindowMenu(lua_State* L) {
    auto& h = Impl::of(L).host;
    bool yes = !h.window_menu.empty() && h.window_menu == luaL_checkstring(L, 1);
    if (yes) h.window_menu.clear();
    lua_pushboolean(L, yes);
    return 1;
}
// yes = eq.TakeCloseTopWindow()   -- the close-top-window key (Escape) went down this frame and is the caller's (once)
int TakeCloseTopWindow(lua_State* L) {
    lua_pushboolean(L, std::exchange(Impl::of(L).host.close_top_window, false));
    return 1;
}
int ReturnFalse(lua_State* L) {
    lua_pushboolean(L, false);
    return 1;
}
int ReturnEmptyTable(lua_State* L) {
    lua_newtable(L);
    return 1;
}

// list = eq.DebugScreens()   -- every stock window model: { name, shown, replaced }
int DebugScreens(lua_State* L) {
    auto& m = Impl::of(L);
    lua_newtable(L);
    int i = 0;
    for (auto& w : m.windows) {
        lua_newtable(L);
        lua_pushstring(L, w->screen().c_str());
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, w->shown(m.host));
        lua_setfield(L, -2, "shown");
        auto it = m.replaced_at.find(w->screen());
        lua_pushboolean(L, it != m.replaced_at.end() && m.frame - it->second < 3);
        lua_setfield(L, -2, "replaced");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

} // namespace

EqApi::EqApi(EqHost& host) : impl_(std::make_unique<Impl>(host)) {}
EqApi::~EqApi() {
    EqHost& h = impl_->host;
    chat_host_gone(h, impl_->layout);
    // The handlers the windows installed on the host reach into this object: the host outlives it (a /loadskin).
    h.open_social_editor = nullptr;
    h.open_help = nullptr;
    h.layout_sources = nullptr;
    h.copy_layout = nullptr;
    h.show_tip_of_day = nullptr;
    h.activate_tip = nullptr;
    h.show_window = nullptr;
}

void EqApi::begin_frame() {
    Impl& m = *impl_;
    ++m.frame;
    if (m.reload_layout) { // a copied layout (__CopyLayout's unk_ApplyWindowPositions)
        m.reload_layout = false;
        load_layout(m.layout_defaults, m.layout_file, m.layout_seed);
    }
    for (auto& w : m.windows) w->frame(m.host);
    if (m.host.in_game) m.context_tips_on_show();
    if (!m.host.in_game) m.host.cursor_button.reset();
    if (m.host.cursor_button) {
        // Gone with a right click or Escape, or a left click that put it nowhere (last frame's release).
        if ((m.stray_release == m.frame - 1 && m.stray_serial == m.host.cursor_serial) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            m.host.cursor_button.reset();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m.pressed_holding = true;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && m.pressed_holding) {
            m.stray_release = m.frame;
            m.stray_serial = m.host.cursor_serial;
        }
    }
    if (!m.host.cursor_button) m.pressed_holding = false;
    if (m.host.cursor_spell) {
        // Deactivated when the memorize is sent (CSpellBookWnd::GetSpellMemTicksLeft @0x75db20) and when the book
        // hides (unk_CSpellBookWnd_HideWorker: a type 1 attachment); a right click (mouse-look) or Escape too.
        bool memorizing = m.host.conn && m.host.conn->spell_state().memorizing.has_value();
        if (!m.host.in_game || !m.host.conn || !m.host.conn->spell_book_open() || (m.cursor_spell_memorizing && !memorizing) ||
            ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            m.host.cursor_spell.reset();
        m.cursor_spell_memorizing = memorizing;
    } else {
        m.cursor_spell_memorizing = false;
    }
    if (!m.layout.loaded()) return;
    // StoreIniInfo / StoreIniVis: what changed since the last frame goes to the file, written at most every 2 s.
    // Not against a screen that is not there (minimized: 0 x 0), which would put every place off the screen.
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    if (ds.x < 64.0f || ds.y < 64.0f) m.last_place.clear();
    else if (ds.x != m.screen.x || ds.y != m.screen.y) { // a new screen size: every window placed again
        m.screen = ds;
        ++m.screen_serial;
        m.last_place.clear();
        m.seen.clear();
    }
    for (auto& [name, place] : m.last_place)
        m.layout.store(m.ini_info(name), place.first, static_cast<int>(ds.x), static_cast<int>(ds.y), place.second);
    m.last_place.clear();
    for (auto& w : m.windows) {
        const std::string& name = w->screen();
        if (name.rfind("Bag", 0) == 0) continue; // shown by its own rules
        const ui::WindowIniInfo& info = m.ini_info(name);
        if (!(info.flags & 0x100)) continue;
        bool shown = w->shown(m.host);
        auto it = m.last_shown.find(name);
        if (it != m.last_shown.end() && it->second != shown) m.layout.store_shown(info, shown);
        m.last_shown[name] = shown;
    }
    m.layout.save_if_due(std::chrono::steady_clock::now());
}

void EqApi::load_layout(const std::filesystem::path& defaults, const std::filesystem::path& file,
                        const std::filesystem::path& seed) {
    Impl& m = *impl_;
    m.lists_changed();
    m.layout.save();
    m.layout.load(defaults, file, seed);
    m.layout_defaults = defaults, m.layout_file = file, m.layout_seed = seed;
    m.last_place.clear();
    m.last_shown.clear();
    for (auto& w : m.windows) {
        const std::string& name = w->screen();
        if (name.rfind("Bag", 0) == 0) continue;
        const ui::WindowIniInfo& info = m.ini_info(name);
        if (auto s = m.layout.shown(info)) w->set_shown(m.host, *s);
        else if (info.flags & 0x100) w->set_shown(m.host, (info.flags & 0x200) != 0); // bInitVisibility
    }
    for (auto& w : m.windows) w->ini_loaded(m.host, m.layout, file.parent_path());
    chat_layout_loaded(m.layout, m.host, file.parent_path());
    log::info("ui: window places from {} (defaults {})", file.string(), defaults.string());
}

void EqApi::save_layout() { impl_->layout.save(); }

const std::filesystem::path& EqApi::layout_file() const { return impl_->layout_file; }

void EqApi::reload_game_files() { impl_->cascade.reload(); }

bool EqApi::click_child(std::string_view screen, std::string_view child, std::string_view button) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->click(impl_->host, child, button, false);
}

bool EqApi::click_hot_button(std::string_view screen, std::string_view child, std::string_view button) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->click_hot_button(impl_->host, child, button);
}

bool EqApi::select_combo(std::string_view screen, std::string_view child, int index) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->combo_select(impl_->host, child, index);
}

std::vector<std::vector<std::string>> EqApi::list_rows(std::string_view screen, std::string_view child) {
    std::vector<std::vector<std::string>> out;
    StockWindow* w = impl_->window(screen);
    if (const ListData* d = w ? w->list(impl_->host, child) : nullptr)
        for (const auto& r : d->rows) out.push_back(r.texts);
    return out;
}

bool EqApi::header_click_list(std::string_view screen, std::string_view child, int column) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->list_header_click(impl_->host, child, column);
}

bool EqApi::select_list(std::string_view screen, std::string_view child, int row, int mods) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->list_select(impl_->host, child, row, mods);
}

bool EqApi::double_click_list(std::string_view screen, std::string_view child, int row) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->list_double_click(impl_->host, child, row, 0);
}

bool EqApi::click_list(std::string_view screen, std::string_view child, int row, int column) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    if (!w || !w->list(impl_->host, child)) return false;
    return w->list_click(impl_->host, child, row, false, column);
}

bool EqApi::set_child_text(std::string_view screen, std::string_view child, const std::string& text) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->set_text(impl_->host, child, text);
}

std::optional<std::string> EqApi::child_text(std::string_view screen, std::string_view child) {
    StockWindow* w = impl_->window(screen);
    if (!w) return std::nullopt;
    Child c;
    w->child(impl_->host, child, c);
    return c.text;
}

std::optional<bool> EqApi::child_shown(std::string_view screen, std::string_view child) {
    StockWindow* w = impl_->window(screen);
    if (!w) return std::nullopt;
    Child c;
    w->child(impl_->host, child, c);
    return c.shown;
}

std::optional<bool> EqApi::child_checked(std::string_view screen, std::string_view child) {
    StockWindow* w = impl_->window(screen);
    if (!w) return std::nullopt;
    Child c;
    w->child(impl_->host, child, c);
    return c.checked;
}

std::optional<bool> EqApi::child_state(std::string_view screen, std::string_view child) {
    StockWindow* w = impl_->window(screen);
    if (!w) return std::nullopt;
    Child c;
    w->child(impl_->host, child, c);
    return c.enabled;
}

bool EqApi::stml_link(std::string_view screen, std::string_view child, int message, std::string_view data) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->stml_link(impl_->host, child, message, data);
}

std::vector<std::string> EqApi::list_texts(std::string_view screen, std::string_view child) {
    std::vector<std::string> out;
    StockWindow* w = impl_->window(screen);
    const ListData* d = w ? w->list(impl_->host, child) : nullptr;
    if (d)
        for (const auto& r : d->rows) out.push_back(r.texts.empty() ? std::string() : r.texts[0]);
    return out;
}

void EqApi::cascade_hover(int menu, int item) {
    impl_->lists_changed();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    impl_->cascade.hover(menu, item, static_cast<int>(ds.x), static_cast<int>(ds.y));
}

bool EqApi::context_menu(std::string_view screen, float x, float y) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return impl_->host.context_menus() && w && w->context_menu(impl_->host, x, y);
}

std::optional<int> EqApi::window_setting(std::string_view screen, std::string_view name) {
    StockWindow* w = impl_->window(screen);
    return w ? w->setting(name) : std::nullopt;
}

bool EqApi::set_window_setting(std::string_view screen, std::string_view name, int value) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->set_setting(impl_->host, name, value);
}

bool EqHost::context_menus() const {
    if (!conn) return true;
    return game::context_menus_open(conn->client_option(game::ClientOption::ContextMenus), in_game && conn->player_down());
}

bool EqApi::menu_click(int menu, int row) {
    impl_->lists_changed();
    if (menu < 0 || row < 0) return false;
    auto& h = impl_->host;
    if (static_cast<std::size_t>(menu) < h.menus.size() && h.menus[static_cast<std::size_t>(menu)].row_y.empty()) {
        // not drawn yet: the rows' places as GameMenus lays them out are unknown; submenus open at the menu's top
        h.menus[static_cast<std::size_t>(menu)].row_y.assign(h.menus[static_cast<std::size_t>(menu)].rows.size(), 0.0f);
    }
    return menu_choose(h, static_cast<std::size_t>(menu), static_cast<std::size_t>(row));
}

bool EqApi::activate_tip(int tip) {
    impl_->lists_changed();
    return impl_->activate_context_tip(tip, false);
}

bool EqApi::activate_window(std::string_view screen, int arg) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->activate(impl_->host, arg);
}

bool EqApi::click_gem(int gem, bool right) {
    impl_->lists_changed();
    StockWindow* w = impl_->window("CastSpellWnd");
    return w && w->gem_click(impl_->host, "CSPW_Spell" + std::to_string(gem), right);
}

bool EqApi::activate_tip_of_day(bool force) {
    impl_->lists_changed();
    StockWindow* w = impl_->window("TipWindow#2");
    return w && w->activate(impl_->host, force ? -1 + kTipForce : -1);
}

bool EqApi::replaces(std::string_view screen) const {
    if (impl_->host.keep_cpp_windows) return false;
    auto it = impl_->replaced_at.find(screen);
    return it != impl_->replaced_at.end() && impl_->frame - it->second <= 2;
}

bool EqApi::shown(std::string_view screen) const {
    StockWindow* w = impl_->window(screen);
    return w && w->shown(impl_->host);
}

void EqApi::show(std::string_view screen, bool show) {
    impl_->lists_changed();
    if (StockWindow* w = impl_->window(screen)) w->set_shown(impl_->host, show);
}

void EqApi::toggle(std::string_view screen) { show(screen, !shown(screen)); }

void EqApi::set_tab_page(std::string_view screen, std::string_view child, int page) {
    impl_->lists_changed();
    if (StockWindow* w = impl_->window(screen)) w->set_tab_page(impl_->host, child, page);
}

std::vector<std::uint32_t> EqApi::map_command(std::string_view args) {
    impl_->lists_changed();
    StockWindow* w = impl_->window("MapViewWnd");
    return w ? app::map_command(*w, impl_->host, args) : std::vector<std::uint32_t>{};
}

bool EqApi::add_player_note(std::string_view text) {
    impl_->lists_changed();
    StockWindow* w = impl_->window("PlayerNotesWindow");
    return w && player_notes_add(*w, text);
}

void EqApi::actions_key(int page) {
    impl_->lists_changed();
    StockWindow* w = impl_->window("ActionsWindow");
    if (!w) return;
    bool same = w->tab_page(impl_->host, "ACTW_ActionsSubwindows").value_or(-1) == page;
    if (w->shown(impl_->host) && same) {
        w->set_shown(impl_->host, false);
        return;
    }
    w->set_tab_page(impl_->host, "ACTW_ActionsSubwindows", page);
    w->set_shown(impl_->host, true);
}

void EqApi::right_click_item(const net::ItemSlot& slot) { item_right_click(impl_->host, slot); }
bool EqApi::open_player_menu(std::uint32_t spawn_id, float x, float y) {
    return app::open_player_menu(impl_->host, spawn_id, x, y);
}
void EqApi::open_all_bags() { app::open_all_bags(impl_->host); }
void EqApi::close_all_bags() { app::close_all_bags(impl_->host); }
void EqApi::toggle_all_bags() { app::toggle_all_bags(impl_->host); }
void EqApi::toggle_all_bank_bags() { app::toggle_all_bank_bags(impl_->host); }

void register_chat_api(lua_State* L, EqHost& host); // app/lua_eq_chat.cpp
void register_gamepad_api(lua_State* L, EqHost& host); // app/lua_eq_gamepad.cpp
void register_spellbook_api(lua_State* L, EqHost& host); // app/lua_eq_spellbook.cpp
void register_abilities_api(lua_State* L, EqHost& host); // app/lua_eq_abilities.cpp
void register_ability_book_api(lua_State* L, EqHost& host); // app/lua_eq_abilitybook.cpp
void register_inventory_manager_api(lua_State* L, EqHost& host); // app/lua_eq_inventory_manager.cpp
void register_reward_selection_api(lua_State* L, EqHost& host); // app/lua_eq_rewards.cpp
void register_achievement_api(lua_State* L, EqHost& host); // app/lua_eq_achievements.cpp
void register_exchange_api(lua_State* L, EqHost& host);  // app/lua_eq_exchange.cpp
void register_social_api(lua_State* L, EqHost& host);    // app/lua_eq_social.cpp
void register_stats_api(lua_State* L, EqHost& host);     // app/lua_eq_stats.cpp
void register_socialedit_api(lua_State* L, EqHost& host); // app/lua_eq_socialedit.cpp
void register_luasocial_api(lua_State* L, EqHost& host);  // app/lua_eq_luasocial.cpp
void register_hotbar_api(lua_State* L, EqHost& host);     // app/lua_eq_hotbar.cpp
void register_web_api(lua_State* L, EqHost& host);        // app/lua_eq_web.cpp
void register_find_api(lua_State* L, EqHost& host);       // app/lua_eq_find.cpp
void register_compare_api(lua_State* L, EqHost& host, std::vector<std::unique_ptr<StockWindow>>& windows); // app/lua_eq_compare.cpp
void register_task_api(lua_State* L, EqHost& host, std::vector<std::unique_ptr<StockWindow>>& windows); // app/lua_eq_progress.cpp
int CompassStrips(lua_State* L);                     // app/lua_eq_misc.cpp
int CompassMarkers(lua_State* L);                    // app/lua_eq_misc.cpp

void EqApi::register_functions(lua_State* L) {
    register_chat_api(L, impl_->host);
    register_gamepad_api(L, impl_->host);
    register_spellbook_api(L, impl_->host);
    register_abilities_api(L, impl_->host);
    register_ability_book_api(L, impl_->host);
    register_inventory_manager_api(L, impl_->host);
    register_reward_selection_api(L, impl_->host);
    register_achievement_api(L, impl_->host);
    register_exchange_api(L, impl_->host);
    register_social_api(L, impl_->host);
    register_stats_api(L, impl_->host);
    register_socialedit_api(L, impl_->host);
    register_luasocial_api(L, impl_->host);
    register_hotbar_api(L, impl_->host);
    register_web_api(L, impl_->host);
    register_find_api(L, impl_->host);
    register_compare_api(L, impl_->host, impl_->windows);
    register_task_api(L, impl_->host, impl_->windows);
    // Opening a tracker entry changes TaskWnd's selected objective list in this frame. Its next ListInfo/ListRow
    // read must not retain the previously selected task's cached list.
    lua_pushlightuserdata(L, impl_.get());
    lua_getfield(L, -2, "OpenTask");
    lua_pushcclosure(L, [](lua_State* state) -> int {
        Impl::of(state).lists_changed();
        lua_pushvalue(state, lua_upvalueindex(2));
        lua_insert(state, 1);
        lua_call(state, lua_gettop(state) - 1, LUA_MULTRET);
        return lua_gettop(state);
    }, 2);
    lua_setfield(L, -2, "OpenTask");
    lua_pushlightuserdata(L, &impl_->host);
    lua_pushcclosure(L, CompassStrips, 1);
    lua_setfield(L, -2, "CompassStrips");
    lua_pushlightuserdata(L, &impl_->host);
    lua_pushcclosure(L, CompassMarkers, 1);
    lua_setfield(L, -2, "CompassMarkers");
    register_map_api(L, impl_->host, impl_->windows);
    register_plugins_api(L, impl_->host);
    register_scripts_api(L, impl_->host);
    register_pregame_api(L, impl_->host, impl_->screens);
    static const luaL_Reg fns[] = {
        {"InGame", InGame}, {"UiReady", UiReady}, {"GameState", GameState}, {"Name", Name}, {"SlotName", SlotName},
        {"Item", Item}, {"ItemIcon", ItemIcon}, {"Money", Money}, {"Label", Label}, {"Gauge", Gauge},
        {"ClickSlot", ClickSlot}, {"HasSlot", HasSlot}, {"AltCurrencies", AltCurrencies}, {"UiTexture", UiTexture}, {"UiAnimation", UiAnimation},
        {"ReplaceWindow", ReplaceWindow}, {"WindowPlaced", WindowPlaced}, {"ShowWindow", ShowWindow}, {"WindowButton", WindowButton},
        {"WindowText", WindowText}, {"ChildShown", ChildShown}, {"WindowList", WindowList}, {"ChildArt", ChildArtFn},
        {"GemTint", GemTint}, {"GemOverlay", GemOverlay}, {"GemInfo", GemInfo}, {"ButtonCoolDown", ButtonCoolDown}, {"CoolDownColor", CoolDownColor}, {"ItemPreview", ItemPreview}, {"ItemPreviewImage", ItemPreviewImage}, {"ItemPreviewControl", ItemPreviewControl}, {"BuffSlot", BuffSlot}, {"BuffOverlay", BuffOverlay}, {"ClickChild", ClickChild}, {"ClickChildAt", ClickChildAt},
        {"HotButton", HotButton}, {"HotButtonFace", HotButtonFace}, {"HotButtonChecked", HotButtonChecked}, {"HotButtonState", HotButtonState}, {"HotButtonInfo", HotButtonInfoFn}, {"ClickHotButton", ClickHotButton}, {"PickUp", PickUp}, {"PressButtonFrom", PressButtonFrom},
        {"HoldingButton", HoldingButton}, {"ClearCursorButton", ClearCursorButton}, {"SpellGemClick", SpellGemClick},
        {"SlotItem", SlotItem}, {"ListInfo", ListInfo}, {"ListRow", ListRowFn}, {"ListClick", ListClick},
        {"SetWindowText", SetWindowText}, {"ChildState", ChildState}, {"WindowTitle", WindowTitle},
        {"ChildGauge", ChildGauge}, {"ChildColor", ChildColor}, {"TabPage", TabPage}, {"ChildTooltip", ChildTooltip},
        {"SetTabPage", SetTabPage}, {"DebugScreens", DebugScreens}, {"ChildUnder", ChildUnder},
        {"VoiceChat", ReturnFalse}, // no voice chat (Vivox): its pieces stay hidden
        // Where windows that lay out in code put their children and how large they make themselves (else nil: the
        // scripts lay out from the XML).
        {"ChildRect", ChildRect}, {"WindowClient", WindowClient}, {"SizeWindow", SizeWindow},
        {"CursorAttachment", CursorAttachment}, {"StmlLink", StmlLink}, {"DebugTree", ReturnEmptyTable},
        {"SetSlider", SetSlider}, {"ComboInfo", ComboInfo}, {"ListHeaderClick", ListHeaderClick}, {"Option", Option}, {"SetOption", SetOption}, {"UiIni", UiIniGet}, {"SetUiIni", UiIniSet}, {"OptionChoices", OptionChoices},
        {"ComboSelect", ComboSelect}, {"BagInfo", BagInfo}, {"SortItems", SortItems}, {"SellJunk", SellJunk}, {"Parcels", Parcels}, {"RetrieveParcel", RetrieveParcel}, {"ClaimAllParcels", ClaimAllParcels}, {"SendParcel", SendParcel}, {"SendParcelMoney", SendParcelMoney}, {"ItemJob", ItemJob}, {"StopItemJob", StopItemJob}, {"FindItems", FindItems},
        {"InventorySpace", InventorySpace}, {"PlaceClick", PlaceClick}, {"BagSlotItem", BagSlotItem}, {"BagSlotClick", BagSlotClick},
        // The EQ button's menu (CascadeMenu.txt) and the windows' context menus (EqHost::menus).
        {"CascadeMenus", CascadeMenus}, {"CascadeClick", CascadeClick}, {"CascadeHover", CascadeHover},
        {"CascadeClose", CascadeClose}, {"GameMenus", GameMenus}, {"GameMenuClick", GameMenuClick},
        {"GameMenuHover", GameMenuHover}, {"GameMenuClose", GameMenuClose},
        {"WindowContextMenu", WindowContextMenu}, {"TakeWindowMenu", TakeWindowMenu},
        {"TakeCloseTopWindow", TakeCloseTopWindow}, {"ContextMenus", ContextMenus},
        {"WindowShown", WindowShown},
        {nullptr, nullptr}};
    lua_pushlightuserdata(L, impl_.get());
    luaL_setfuncs(L, fns, 1); // every function gets Impl as its upvalue
    // The functions that only read: the rest may change what a window's lists hold, so each drops the lists read
    // this frame (Impl::list) before it runs. A function missing here costs only a list built again.
    static constexpr std::string_view kReads[] = {
        "InGame", "UiReady", "GameState", "Name", "SlotName", "Item", "ItemIcon", "Money", "Label", "Gauge", "HasSlot",
        "AltCurrencies", "UiTexture", "UiAnimation", "ReplaceWindow", "WindowShown", "WindowPlaced", "WindowText", "ChildShown",
        "WindowList", "ChildArt", "GemTint", "GemOverlay", "ButtonCoolDown", "CoolDownColor", "ItemPreviewImage",
        "BuffSlot", "BuffOverlay", "HotButton", "HotButtonFace", "HotButtonChecked", "HotButtonState", "HotButtonInfo",
        "GemInfo", "HoldingButton", "SlotItem", "ListInfo", "ListRow", "ChildState", "WindowTitle", "ChildGauge",
        "ChildColor", "TabPage", "ItemPreview",
        "ChildTooltip", "DebugScreens", "ChildUnder", "VoiceChat", "ChildRect", "WindowClient", "CursorAttachment",
        "DebugTree", "ComboInfo", "Option", "UiIni", "OptionChoices", "BagInfo", "Parcels", "FindItems",
        "InventorySpace", "BagSlotItem", "CascadeMenus", "GameMenus", "ContextMenus", "ItemJob"};
    auto changes_lists = [](lua_State* L2) -> int {
        Impl::of(L2).lists_changed();
        return fns[static_cast<std::size_t>(lua_tointeger(L2, lua_upvalueindex(2)))].func(L2);
    };
    for (std::size_t i = 0; fns[i].name; ++i) {
        if (std::ranges::find(kReads, std::string_view(fns[i].name)) != std::end(kReads)) continue;
        lua_pushlightuserdata(L, impl_.get());
        lua_pushinteger(L, static_cast<lua_Integer>(i));
        lua_pushcclosure(L, changes_lists, 2);
        lua_setfield(L, -2, fns[i].name);
    }
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "FirstWornSlot");
    lua_pushinteger(L, 22);
    lua_setfield(L, -2, "LastWornSlot");
    lua_pushinteger(L, 23);
    lua_setfield(L, -2, "FirstBagSlot");
    lua_pushinteger(L, 32);
    lua_setfield(L, -2, "LastBagSlot");
    lua_pushinteger(L, 33);
    lua_setfield(L, -2, "CursorSlot");
    // Last: the functions that act check a plugin's capabilities first (app/lua_eq_caps.h).
    gate_eq_calls(L);
}

} // namespace hailstone::app
