// The `hs` functions (app/lua_ui.h). Names, arguments and returns follow the
// original UI scripts' contracts; each function's comment gives its contract. Window functions find the stock window model by
// screen name (app/lua_ui_windows.h); a window without a model is never shown, so its script draws nothing.

#include "app/lua_ui.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <cfloat>
#include <cmath>
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
#include "app/chat_tab_bar.h"
#include "app/chat_tabs.h"
#include "app/lua_ui_menu.h"
#include "app/lua_ui_values.h"
#include "app/lua_ui_windows.h"
#include "ui/lua/lua_host.h"
#include "app/lua_ui_caps.h"
#include "app/lua_ui_api.h"
#include "app/lua_ui_hooks.h"
#include "app/lua_ui_plugins.h"
#include "app/lua_ui_record.h"
#include "app/lua_ui_extensions.h"
#include "app/lua_ui_map_overlay.h"
#include "plugin/lua_keep.h"
#include "plugin/ui_bridge.h"
#include "core/character_files.h"
#include "core/ini_file.h"
#include "core/log.h"
#include "core/saturate.h"
#include "game/eq_names.h"
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
#include "platform/paths.h"
#include "ui/layout_profile.h"
#include "ui/ui_ini.h"
#include "ui/ui_textures.h"
#include "ui/texture_ids.h"
#include "ui/lua/lua_nameplate_canvas.h"

namespace hailstone::app {

// app/lua_ui_chat.cpp: the chat windows' settings, and the user colours from the character's chat.ini in `user_dir`.
void chat_layout_loaded(ui::UiIni& ini, UiHost& host, const std::filesystem::path& user_dir, bool profile_change);
void capture_chat_layout(const ui::UiIni& active, ui::UiIni& snapshot);
void chat_host_gone(const UiHost& host, const ui::UiIni& ini);

namespace {
// map[key] on a map with a transparent comparator, without building a std::string key when it is already there (the
// per-frame lookups that would otherwise allocate one each time).
template <class Map, class Key>
typename Map::mapped_type& map_entry(Map& map, const Key& key) {
    auto it = map.find(key);
    if (it == map.end()) it = map.emplace(std::string(key), typename Map::mapped_type{}).first;
    return it->second;
}
} // namespace

struct UiApi::Impl {
    UiHost& host;
    std::shared_ptr<void> callback_lifetime = std::make_shared<int>(0);
    UiCallbackContext hook_context{callback_lifetime, [this](std::string_view tag) {
        return host.plugin_caps ? host.plugin_caps(tag) : 0u;
    }};
    UiCallbackContext extensions_context{callback_lifetime, [this](std::string_view tag) {
        return host.plugin_caps ? host.plugin_caps(tag) : 0u;
    }};
    UiCallbackContext map_overlay_context{callback_lifetime, [this](std::string_view tag) {
        return host.plugin_caps ? host.plugin_caps(tag) : 0u;
    }};
    std::vector<std::unique_ptr<StockWindow>> windows;
    // `windows` by screen name (window()): the first window of a name, as a scan from the front finds it. Built again
    // whenever the vector has grown since (the views point into each window's own name, which never changes).
    std::unordered_map<std::string_view, StockWindow*> windows_by_name;
    std::size_t windows_indexed = 0;
    std::map<std::string, int, std::less<>> replaced_at; // screen -> frame of its last hs.ReplaceWindow
    int frame = 0;
    // hs.ReplaceWindow and hs.WindowShown take any name a script gives (the packs' own windows are not all stock
    // ones): the names kept per window are bounded. Only the last few frames' replacements count (replaces()), so
    // past kMaxNamed names the older ones are dropped; a name past it while all are recent is not kept.
    static constexpr std::size_t kMaxNamed = 1024;
    bool note_replaced(std::string_view name) {
        if (auto it = replaced_at.find(name); it != replaced_at.end()) {
            it->second = frame;
            return true;
        }
        if (replaced_at.size() >= kMaxNamed)
            std::erase_if(replaced_at, [this](const auto& e) { return frame - e.second > 2; });
        if (replaced_at.size() >= kMaxNamed) return false;
        replaced_at.emplace(name, frame);
        return true;
    }
    ScreenReplacements screens{&replaced_at, &frame}; // hs.LoginScreen .. (app/lua_ui_pregame.cpp)
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
    std::optional<ui::LayoutProfile> layout_profile_preview;
    std::optional<ui::LayoutProfile> layout_profile_undo;
    int layout_profile_generation = 0;
    bool layout_profile_reloading = false;
    // Where each Lua window was last frame (screen pixels): the EQ menu opens beside the EQ window.
    std::map<std::string, std::array<int, 4>, std::less<>> screen_rects;
    CascadeMenu cascade; // the EQ button's menu (app/lua_ui_menu.h)
    // Each item slot's CInvSlotWnd cool-down (CButtonWnd +0x21c / +0x220), by where the Lua asked for the item.
    std::map<std::string, game::ButtonCoolDown, std::less<>> slot_cool_downs;
    std::string cool_down_key; // set_item_cool_down's key, kept so formatting it allocates nothing once it has grown

    // CInvSlotWnd_Draw @0x69a080's recast cool-down for the slot `key` holding `item` (game::update_item_cool_down),
    // as the item table's `cooldown`: DrawCooldown's fraction (the shaded part runs from the slot's top + fraction x
    // its height down to its bottom), absent while idle. The table is at the top of the stack.
    // The key is formatted only for an item that is there.
    template <class... Args>
    void set_item_cool_down(lua_State* L, const net::Item* item, std::format_string<Args...> key, Args&&... args) {
        if (!item || !host.conn) return;
        cool_down_key.clear();
        std::format_to(std::back_inserter(cool_down_key), key, std::forward<Args>(args)...);
        HostButtonQueries q(host);
        game::ButtonCoolDown& cd = map_entry(slot_cool_downs, cool_down_key);
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
            bool& was = map_entry(tip_shown, std::string_view(screen_name));
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
    std::vector<UiHost::LayoutSource> layout_sources() const {
        std::vector<UiHost::LayoutSource> out;
        auto scan = [&](const std::filesystem::path& file, const std::string& label, const std::filesystem::path& dir,
                        const std::string& world) {
            IniFile ini;
            if (!ini.load(file)) return;
            std::error_code ec;
            bool current = !layout_file.empty() && std::filesystem::equivalent(file, layout_file, ec);
            bool has_key_bindings = !current && !dir.empty() &&
                                    std::filesystem::is_regular_file(CharacterFiles{dir}.settings(), ec);
            for (const std::string& section : ini.sections())
                for (const std::string& key : ini.keys(section)) {
                    if (key.size() < 4 || !game::same_no_case(std::string_view(key).substr(0, 4), "XPos")) continue;
                    std::string mode = key.substr(4);
                    if (current && game::same_no_case(mode, "Windowed")) continue;
                    bool listed = false;
                    for (const auto& o : out) listed = listed || (o.file == file && o.mode == mode);
                    if (!listed) out.push_back({file, label, mode, dir, world, has_key_bindings});
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
    // Key Bindings copies the source character's keyboard map (its hailstone.ini) over this one's (ActionsUi::
    // copy_key_bindings); the original's copy has no such box (0x55b930 copies only the sections above).
    std::optional<std::pair<std::uint32_t, std::vector<std::string>>> copy_layout(const UiHost::LayoutSource& from,
                                                                              const UiHost::LayoutCopyParts& parts) {
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
        if (host.actions && parts.key_bindings && !from.character_dir.empty())
            host.actions->copy_key_bindings(CharacterFiles{from.character_dir}.settings());
        if (host.copy_ui_layout) host.copy_ui_layout(from.character_dir);
        reload_layout = true;
        return std::nullopt;
    }
    bool reload_layout = false; // a layout was copied: the windows are placed again before the next frame

    bool layout_profiles_ready() const {
        return host.in_game && host.conn && layout.loaded() && !layout_file.empty() &&
               host.layout_profile_pack && host.layout_profile_pack().first;
    }

    std::filesystem::path layout_profile_directory() const {
        const auto pack = host.layout_profile_pack().second;
        return platform::data_dir() / "layout-profiles" / platform::ui_settings_file(pack).stem();
    }

    ui::LayoutProfile capture_layout_profile(std::string_view name) {
        // Project pending frame state without writing to the live character layout.
        auto snapshot = layout;
        store_visibility(&snapshot);
        const auto display = ImGui::GetCurrentContext() ? ImGui::GetIO().DisplaySize : screen;
        if (display.x >= 64 && display.y >= 64)
            for (const auto& [window_name, place] : last_place)
                snapshot.store(ini_info(window_name), place.first, number_to_int(display.x), number_to_int(display.y),
                               place.second);
        capture_chat_layout(layout, snapshot);
        ui::LayoutProfile profile;
        profile.name = name;
        profile.width = number_to_int(display.x);
        profile.height = number_to_int(display.y);
        if (!name.empty()) profile.rects = ui::lua::snapshot_pack_window_rects(profile.width, profile.height);
        profile.windows = ui::capture_layout_windows(snapshot.file());
        profile.chat = host.conn->capture_layout_chat();
        profile.pack_settings = host.layout_profile_pack().first->capture_layout_profile_settings();
        return profile;
    }

    const char* apply_layout_profile(const ui::LayoutProfile& next, const ui::LayoutProfile& previous) {
        auto* pack = host.layout_profile_pack().first;
        auto next_windows = ui::replace_layout_windows(layout.file(), next.windows);
        if (!pack->apply_layout_profile_settings(next.pack_settings)) return "Could not save interface settings.";
        const bool chat = host.conn->apply_layout_chat(next.chat);
        if (!chat || !layout.replace_file(std::move(next_windows))) {
            bool restored = pack->apply_layout_profile_settings(previous.pack_settings);
            if (chat && !host.conn->apply_layout_chat(previous.chat)) restored = false;
            return restored ? "Could not apply the profile. Your previous settings were restored."
                            : "Could not apply the profile or fully restore your previous settings.";
        }
        last_place.clear();
        reload_layout = layout_profile_reloading = true;
        return nullptr;
    }

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
        // the window (its script calls hs.ReplaceWindow every frame, so by the EQ button's click it has).
        auto replaces = [this](std::string_view name) {
            auto it = replaced_at.find(name);
            return it != replaced_at.end() && frame - it->second <= 2;
        };
        std::function<bool(int)> offered;
        if (!host.keep_cpp_windows) offered = [&](int command) { return menu_command_offered(command, replaces); };
        const bool profiles = layout_profiles_ready() && replaces("LayoutProfilesWindow");
        cascade.load(*host.game, expansions, host.conn ? host.conn->pvp_rules() : 0, offered, profiles);
    }

    explicit Impl(UiHost& h) : host(h) {
        // The chat windows (CChatWindowManager's): the main one shown in game, the others while the chat manager
        // has them (made from its menu, the Tells window); closing one of those closes it there.
        struct Chat : StockWindow {
            int slot;
            explicit Chat(int s) : StockWindow("Chat" + std::to_string(s), true), slot(s) {}
            bool shown(UiHost& h2) override {
                const auto group = chat_tabs::group(slot);
                return h2.in_game && group.slot && (!group.main || open_);
            }
            void set_shown(UiHost& h2, bool show) override {
                if (chat_tabs::group(slot).main) StockWindow::set_shown(h2, show);
                else if (!show) chat_tabs::close_group(slot);
            }
        };
        for (int slot = 1; slot <= chat_tabs::kMaxSlots; ++slot) windows.push_back(std::make_unique<Chat>(slot));
        // CEQMainWnd: the EQ button (a check box, pressed while its menu is open), the Marketplace button (SCButton:
        // the window's toggle) and the flash sale button (SCFSButton: hidden by the constructor, shown only by a flash
        // sale notice this client does not get).
        struct MainMenuWindow : StockWindow {
            Impl& m;
            explicit MainMenuWindow(Impl& impl) : StockWindow("EQMainWnd", true), m(impl) {}
            bool shown(UiHost& h2) override { return open_ && h2.in_game; }
            void child(UiHost&, std::string_view id, Child& out) override {
                if (id == "EQButton") out.checked = m.cascade.open();
                else if (id == "SCFSButton") out.shown = false;
            }
            bool click(UiHost& h2, std::string_view id, std::string_view button, bool) override {
                if (id == "SCButton" && button == "left") { // WndNotification @0x665ed0: Show(!shown)
                    if (h2.conn) h2.conn->set_marketplace_open(!h2.conn->marketplace().open);
                    return true;
                }
                if (id != "EQButton" || button != "left") return false;
                if (!m.cascade.open()) m.load_cascade();
                auto it = m.screen_rects.find("EQMainWnd");
                std::array<int, 4> r = it != m.screen_rects.end() ? it->second : std::array<int, 4>{0, 0, 53, 53};
                ImVec2 ds = ImGui::GetIO().DisplaySize;
                m.cascade.toggle(r, static_cast<int>(ds.x), static_cast<int>(ds.y));
                return true;
            }
        };
        windows.push_back(std::make_unique<MainMenuWindow>(*this));
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
            // A Lua social opens in the Lua social editor (app/lua_ui_luasocial.cpp) instead.
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
        host.copy_layout = [this](const UiHost::LayoutSource& from, const UiHost::LayoutCopyParts& parts) {
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
        if (it == ini_infos.end()) {
            if (ini_infos.size() >= kMaxNamed) return uncached_ini_info = ui::window_ini_info(name); // (bounded)
            it = ini_infos.emplace(std::string(name), ui::window_ini_info(name)).first;
        }
        return it->second;
    }
    ui::WindowIniInfo uncached_ini_info; // ini_info's answer for a name past kMaxNamed (until its next call)

    void store_visibility(ui::UiIni* snapshot = nullptr) {
        // A new connection at character select has no in-game window state to save for the last character.
        if (!host.in_game || !layout.loaded()) return;
        for (auto& w : windows) {
            const std::string& name = w->screen();
            if (name.rfind("Bag", 0) == 0) continue; // shown by its own rules
            const ui::WindowIniInfo& info = ini_info(name);
            if (!(info.flags & 0x100)) continue;
            bool shown = w->shown(host);
            if (snapshot) {
                if (auto it = last_shown.find(name); it != last_shown.end() && it->second != shown)
                    snapshot->store_shown(info, shown);
                continue;
            }
            auto [it, inserted] = last_shown.try_emplace(name, shown);
            if (!inserted && it->second != shown) {
                layout.store_shown(info, shown);
                it->second = shown;
            }
        }
    }

    // A window's list as hs.ListInfo, hs.ListRow and hs.WindowList read it: StockWindow::list once a frame per
    // window and list, since most windows build the list again on each call (reading n rows was n builds). What a
    // call returns points into the window, so it stays valid; a list asked for later that comes back in the same
    // ListData (a window with one buffer for several lists) drops the others' entries. Every hs call that can change a
    // list (a click, a sort, typed text, an option: all but the reads named in register_functions) and UiApi's own
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

using Impl = UiApi::Impl;

void push_cell(lua_State* L, const ui::UiTextures::Cell& c) {
    lua_pushlightuserdata(L, c.id);
    lua_pushnumber(L, c.u0);
    lua_pushnumber(L, c.v0);
    lua_pushnumber(L, c.u1);
    lua_pushnumber(L, c.v1);
}

} // namespace

bool slot_args(lua_State* L, int first, std::int16_t& slot, std::int16_t& bag) {
    const lua_Integer s = luaL_checkinteger(L, first);
    const lua_Integer b = lua_isnoneornil(L, first + 1) ? -1 : luaL_checkinteger(L, first + 1);
    const auto s16 = integer_as<std::int16_t>(s), b16 = integer_as<std::int16_t>(b);
    if (!s16 || !b16) return false;
    slot = *s16;
    bag = *b16;
    return true;
}

// The item table of hs.Item.
int push_item(lua_State* L, const net::Item* item) {
    if (!item) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 22);
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
    if (item->is_bag()) {
        set_int("bagType", item->bag_type);
        set_int("bagSize", item->bag_size);
    }
    lua_pushnumber(L, item->weight / 10.0);
    lua_setfield(L, -2, "weight");
    lua_pushboolean(L, item->magic != 0);
    lua_setfield(L, -2, "magic");
    lua_pushboolean(L, item->is_lore());
    lua_setfield(L, -2, "lore");
    lua_pushboolean(L, item->quest_item != 0);
    lua_setfield(L, -2, "questItem");
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

// hs.InGame() -> true when a character is loaded
int InGame(lua_State* L) {
    lua_pushboolean(L, Impl::of(L).host.in_game);
    return 1;
}

// hs.UiReady() -> true in game or at character select / creation
int UiReady(lua_State* L) {
    lua_pushboolean(L, Impl::of(L).host.ui_ready);
    return 1;
}

// hs.GameState() -> CEverQuest::GameState (1 character select, 2 character creation, 5 in game), nil before it exists
int GameState(lua_State* L) {
    int s = Impl::of(L).host.game_state;
    if (s == 0) lua_pushnil(L);
    else lua_pushinteger(L, s);
    return 1;
}

// hs.Name() -> character name or nil
int Name(lua_State* L) {
    auto& h = Impl::of(L).host;
    const game::Spawn* me = h.conn && h.in_game ? h.conn->world().player() : nullptr;
    if (me) lua_pushstring(L, me->name.c_str());
    else lua_pushnil(L);
    return 1;
}

// hs.ClassName(id [, short]) -> a class's name as the client shows it, its short name with `short`: a server's own
// where its plugin or script named the class (game/eq_names.h), else the stock one ("Warrior", "WAR").
int ClassName(lua_State* L) {
    const lua_Integer id = luaL_checkinteger(L, 1);
    const auto c = static_cast<std::uint32_t>(std::clamp<lua_Integer>(id, 0, 0xffff));
    const std::string name = lua_toboolean(L, 2) ? game::class_short_name(c) : game::class_name(c);
    lua_pushlstring(L, name.data(), name.size());
    return 1;
}

// hs.ClassNamesRevision() -> a number that changes whenever hs.ClassName's answers may have (a cache's key).
int ClassNamesRevision(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(game::class_names_revision() & 0x7fffffffffffffffull));
    return 1;
}

// hs.SlotName(slot) -> "Primary", ...
int SlotName(lua_State* L) {
    lua_pushstring(L, net::possessions_slot_name(held_int(luaL_checkinteger(L, 1))));
    return 1;
}

// hs.Item(slot [, bagSlot]) -> nil or { name, id, icon, count, charges, stackSize, stackable, bagSlots, weight,
// bagType and bagSize on containers, magic, lore, questItem, type, size, ac, hp, mana, cooldown }; cooldown: the slot's
// recast cool-down fraction (CInvSlotWnd_Draw:
// shaded from the slot's top + cooldown x its height to its bottom in hs.CoolDownColor()), absent while idle
int Item(lua_State* L) {
    auto& h = Impl::of(L).host;
    if (!h.conn || !h.in_game) return push_item(L, nullptr);
    std::int16_t slot = 0, bag = -1;
    if (!slot_args(L, 1, slot, bag)) return push_item(L, nullptr);
    const net::Item* item = h.conn->inventory().at(net::ItemSlot::possessions(slot, bag));
    push_item(L, item);
    Impl::of(L).set_item_cool_down(L, item, "inv:{}:{}", slot, bag);
    return 1;
}

// texture, u0, v0, u1, v1 = hs.ItemIcon(icon)   -- nil when the icon's sheet is missing
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

// plat, gold, silver, copper = hs.Money()
int Money(lua_State* L) {
    auto& h = Impl::of(L).host;
    net::Coins c;
    if (h.conn) c = h.conn->coins();
    for (std::uint32_t v : {c.platinum, c.gold, c.silver, c.copper}) lua_pushinteger(L, v);
    return 4;
}

// text [, r, g, b] = hs.Label(eqType [, markInvisibility])
// The text a game label with this EQType shows. markInvisibility (Hailstone): the name plate's "(Name)" / "[Name]"
// on the player's (1) and target's (28) names, for a spawn the player sees; the original's labels show them plain.
int Label(lua_State* L) {
    const bool mark_invisibility = lua_isboolean(L, 2) && lua_toboolean(L, 2);
    LabelValue v = ui_label(Impl::of(L).host, held_int(luaL_checkinteger(L, 1)), mark_invisibility);
    lua_pushstring(L, v.text.c_str());
    if (!v.argb) return 1;
    Rgb c = argb_to_rgb(*v.argb);
    for (float f : c) lua_pushnumber(L, f);
    return 4;
}

// fraction, text, value [, r, g, b] = hs.Gauge(eqType [, markInvisibility])
// fraction 0..1, nil when the gauge is empty/unavailable; markInvisibility (Hailstone) as hs.Label's, on the
// name captions of gauges 1, 6 and 27.
// value is __GetGaugeValueFromEQ's own (0..1000, -1 nothing drawn at all, -2 full in dark red, -3 empty: a corpse,
// -110..-10 and -2000..-1000 the special bands); r, g, b the fill tint CGaugeWnd::Draw gives a special value
// (gauge_fill), nil for a normal one.
int Gauge(lua_State* L) {
    const bool mark_invisibility = lua_isboolean(L, 2) && lua_toboolean(L, 2);
    GaugeValue g = ui_gauge(Impl::of(L).host, held_int(luaL_checkinteger(L, 1)), mark_invisibility);
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

// ok = hs.ClickSlot(slot, bagSlot|nil, "left"|"leftheld"|"right"|right-button phase [, outside])
// The game's slot logic: a left click picks up, drops, swaps or splits (Shift whole stack, Ctrl one; the original
// on the button's release, HandleLButtonUp); "leftheld", the left button held on the slot, puts a hotbutton for it
// on the cursor (item_left_held; false when none went there); the right button (a quick release, "rightup") uses
// the item: inspect, open a bag, cast a clicky.
// The last ImGui item (the child's hit box the script just tested) as the clicked child's screen rectangle.
void note_click_rect(UiHost& h) {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    h.click_rect = {a.x, a.y, b.x, b.y};
}

// The right button held on an item (CInvSlot::HandleRButtonHeld): its item display, whatever a quick click would
// do with it (cast, eat, open the bag, loot). Not a name and icon only (the inspect window's: OP_InspectAnswer
// carries no item data).
void inspect_held(UiHost& h, const net::Item* item) {
    if (item && item->id != 0 && h.inspect_item) h.inspect_item(*item);
}

int ClickSlot(lua_State* L) {
    auto& h = Impl::of(L).host;
    note_click_rect(h);
    if (!h.conn || !h.in_game) {
        lua_pushboolean(L, false);
        return 1;
    }
    std::int16_t slot = 0, bag = -1;
    if (!slot_args(L, 1, slot, bag)) {
        lua_pushboolean(L, false);
        return 1;
    }
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

// ok = hs.HasSlot(slot, bagSlot|nil)   -- true when ClickSlot can act on that slot now: in game, a possessions
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

// list = hs.AltCurrencies([showMissing])   -- { name, count, icon, maxStack } per alternate currency
// (OP_AltCurrency; the name is dbstr type 0x12 of the currency id, as CInventoryWnd's Alt. Currency list reads it,
// "Currency <n>" when the game folder has none); without showMissing only those held and shown. The order and filter are the InventoryWindow's AltCurr_PointList rows (hs.ListClick selects one there).
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

// texture, w, h = hs.UiTexture(file)   -- a texture from uifiles/default (window art), nil if missing
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

// texture, width, height = hs.UiImage(relativeGamePath)
// A read-only raster image in the indexed game-asset layers, independent of game_art; nil when missing or invalid.
// Supports DDS, TGA, PNG, BMP and JPEG. The caller supplies atlas UVs to ui.DrawImage; no layout metadata is read.
int UiImage(lua_State* L) {
    auto& h = Impl::of(L).host;
    std::size_t length = 0;
    const char* relative = luaL_checklstring(L, 1, &length);
    const ui::UiTextures::Texture* texture = nullptr;
    plugin::cxx_guard(L, [&] {
        const auto path = ui::ui_image_path(std::string_view(relative, length));
        if (path && h.textures) texture = h.textures->game_file(*path);
    });
    if (!texture) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, texture->id);
    lua_pushinteger(L, texture->width);
    lua_pushinteger(L, texture->height);
    return 3;
}

// texture, width, height = hs.UI.assets.InterfaceImage(relativeUiPath)
// A raster owned by the active UI, its approved bases, or the current plugin folder.
int UiInterfaceImage(lua_State* L) {
    auto& h = Impl::of(L).host;
    std::size_t length = 0;
    const char* relative = luaL_checklstring(L, 1, &length);
    const ui::UiTextures::Texture* texture = nullptr;
    plugin::cxx_guard(L, [&] {
        const auto path = ui::lua::interface_image_path(L, std::string_view(relative, length));
        if (path && h.textures) texture = h.textures->interface_file(*path);
    });
    if (!texture) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, texture->id);
    lua_pushinteger(L, texture->width);
    lua_pushinteger(L, texture->height);
    return 3;
}

// texture, u0, v0, u1, v1, width, height = hs.UiAnimation(name)
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

// shown, x, y, w, h, sized, place = hs.ReplaceWindow(name [, sizable])
// Call every frame from the replacing window's draw function. shown: whether the game has the window shown (draw
// yours then); x, y, w, h: where the stock window sits: its XML location and size, then the character's UI ini over
// defaults.ini as CSidlScreenWnd::LoadIniInfo reads them (ui/ui_ini.h; `sizable`: the XML window can be resized, so
// a stored size counts). sized: w, h come from an ini; place: the window should be put there now (a character's
// layout was just loaded) rather than only the first time it shows.
// The client's own prompt for the server's compiled plugins ("PluginConsent") and the dialogs that act when answered (a
// confirmation, among them a destroy's; a trade, a give, a quantity, a bazaar purchase's, a group invitation, a
// respawn choice, a large dialog, a task offer) give way only to the pack's own windows: a plugin's code could
// otherwise put the client's away and draw a question of its own in its place.
// Consent, add-on policy warnings and Server Management are reserved to the client's own interfaces (UiHost::trusted_ui):
// under an add-on or hand-placed interface the client draws them, as that interface cannot act on them
// (hs.AnswerPluginConsent, hs.AddonPolicyAction).
bool may_replace(std::string_view name, const UiHost& host) {
    static constexpr std::string_view kPackOnly[] = {
        "PluginConsent", "AddonPolicy", "ServerManagement", "ConfirmationDialogBox", "TradeWnd", "GiveWnd", "QuantityWnd",
        "BazaarConfirmationWnd", "GroupInvite", "RespawnWnd", "LargeDialogWindow", "TaskSelectWnd", "LayoutProfilesWindow"};
    if (!host.trusted_ui && (name == "PluginConsent" || name == "AddonPolicy" || name == "ServerManagement")) return false;
    if (ui::lua::LuaUi::caller_tag().empty()) return true;
    return std::find(std::begin(kPackOnly), std::end(kPackOnly), name) == std::end(kPackOnly);
}

int ReplaceWindow(lua_State* L) {
    auto& m = Impl::of(L);
    std::string name = luaL_checkstring(L, 1);
    if (may_replace(name, m.host)) m.note_replaced(name);
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
        // (bounded as replaced_at: a name past Impl::kMaxNamed is placed each time but not remembered)
        auto it = m.placed.find(name);
        if (it == m.placed.end() && m.placed.size() < Impl::kMaxNamed) it = m.placed.emplace(name, Impl::Placed{}).first;
        Impl::Placed none;
        Impl::Placed& placed = it == m.placed.end() ? none : it->second;
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

// shown = hs.WindowShown(name)   -- hs.ReplaceWindow's first return alone: whether the game has the window shown,
// with the window counted as replaced this frame as hs.ReplaceWindow counts it (UiApi::replaces: the C++ window stays
// off, the EQ menu offers its toggle), but no place worked out. For a script's check before it draws: a window it
// draws still calls hs.ReplaceWindow while shown, since that call is what places it and keeps its moves.
int WindowShown(lua_State* L) {
    auto& m = Impl::of(L);
    const char* name = luaL_checkstring(L, 1);
    if (may_replace(name, m.host)) m.note_replaced(name);
    StockWindow* w = m.window(name);
    lua_pushboolean(L, w && (m.host.ui_ready || w->pre_game()) && w->shown(m.host));
    return 1;
}

// hs.WindowPlaced(name, x, y, w, h [, sizable])   -- where the replacing window is this frame (after ui.Begin): kept
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
    if (auto at = m.screen_rects.find(name); at != m.screen_rects.end()) at->second = now;
    else if (m.screen_rects.size() < Impl::kMaxNamed) m.screen_rects.emplace(name, now); // (bounded: Impl::kMaxNamed)
    if (!m.layout.loaded()) return 0;
    // (only a window hs.ReplaceWindow placed under this layout: one it never placed is not made an entry here)
    auto pl = m.placed.find(name);
    if (pl == m.placed.end() || pl->second.generation != m.layout.generation()) return 0;
    const Impl::Placed& placed = pl->second;
    ui::WindowRect r{now[0], now[1], now[2], now[3]};
    auto it = m.seen.find(name);
    bool moved = it != m.seen.end() && it->second != now && placed.screen == m.screen_serial;
    m.seen[name] = now;
    if (moved) m.last_place[name] = {r, sizable};
    return 0;
}

// A click on ConfirmationDialogBox's Yes from code other than the pack's own (a script, a plugin window, another Lua
// state with the hs API) while that box asks a destroy's question (destructive_confirmation_up): refused, and logged.
// The pack draws the box and its own code passes the player's click on; a script pressing it would destroy an item or
// disband a guild without the player's word. Any other question, and No, stay open to scripts (an accepted rez, a
// trade's question). A Hailstone guard of its own.
bool script_destroy_yes(lua_State* L, const UiHost& host, std::string_view window, std::string_view child) {
    if (window != "ConfirmationDialogBox" || child != "Yes_Button" || ui::lua::pack_code(L) ||
        !destructive_confirmation_up(host))
        return false;
    log::warn("A script pressed Yes on the destroy question (ConfirmationDialogBox); refused: only the player answers it");
    return true;
}

// hs.ShowWindow(name, show)   -- shows or hides the game window (and with it the replacement)
int ShowWindow(lua_State* L) {
    auto& m = Impl::of(L);
    if (StockWindow* w = m.window(luaL_checkstring(L, 1))) w->set_shown(m.host, lua_toboolean(L, 2) != 0);
    return 0;
}

// ok = hs.WindowButton(name, child [, message])   -- the window's handler for that child (message 1, a click, by
// default; 3 is a buff button's right click, which removes the buff)
int WindowButton(lua_State* L) {
    auto& m = Impl::of(L);
    const char* name = luaL_checkstring(L, 1);
    StockWindow* w = m.window(name);
    int message = held_int(luaL_optinteger(L, 3, 1));
    const char* child = luaL_checkstring(L, 2);
    bool ok = w && !script_destroy_yes(L, m.host, name, child) &&
              w->click(m.host, child, message == 3 ? "remove" : "left", false);
    lua_pushboolean(L, ok);
    return 1;
}

// ok = hs.ClickChild(name, child [, button [, outside]])   -- a click on the child: "left", "right" or a
// right-button phase ("rightdown", "rightheld", "rightup", "rightupheld")
int ClickChild(lua_State* L) {
    auto& m = Impl::of(L);
    note_click_rect(m.host);
    const char* name = luaL_checkstring(L, 1);
    StockWindow* w = m.window(name);
    const char* child_arg = luaL_checkstring(L, 2);
    if (script_destroy_yes(L, m.host, name, child_arg)) {
        lua_pushboolean(L, 0);
        return 1;
    }
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

// ok = hs.StmlLink(name, child, message, data)   -- a <a WndNotify="message,data"> link clicked in the window's STML
// box (CStmlWnd sends the window the message with the data, CItemDisplayWnd's XWM_LINK 27); message -1 is an href link
// (`data` its target), which the STML box follows itself (the help window's pages)
int StmlLink(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    int message = lua_isnumber(L, 3) ? held_int(lua_tointeger(L, 3)) : 0;
    const char* data = lua_isstring(L, 4) ? lua_tostring(L, 4) : "";
    lua_pushboolean(L, w && w->stml_link(m.host, luaL_checkstring(L, 2), message, data));
    return 1;
}

// ok = hs.ClickChildAt(name, child, clientX, clientY [, button])   -- as ClickChild (no original to place)
int ClickChildAt(lua_State* L) {
    auto& m = Impl::of(L);
    const char* name = luaL_checkstring(L, 1);
    StockWindow* w = m.window(name);
    const char* child = w ? luaL_checkstring(L, 2) : nullptr;
    std::string button = lua_isstring(L, 5) ? lua_tostring(L, 5) : "left";
    lua_pushboolean(L, w && !script_destroy_yes(L, m.host, name, child) && w->click(m.host, child, button, false));
    return 1;
}

// text, set = hs.WindowText(name, child)   -- the text the game set on the window's child ("" for none); set is
// true when the window set it (an empty text then replaces the XML's)
int WindowText(lua_State* L) {
    StockWindow* w = nullptr;
    Child c = Impl::of(L).child(L, w);
    lua_pushstring(L, c.text ? c.text->c_str() : "");
    lua_pushboolean(L, c.text.has_value());
    return 2;
}

// ok = hs.SetWindowText(name, child, text)   -- typing into the window's edit box
int SetWindowText(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    // Both checked before the std::string the call takes exists (the arguments' order of evaluation is not fixed).
    const char* child = w ? luaL_checkstring(L, 2) : nullptr;
    const char* text = w ? luaL_checkstring(L, 3) : nullptr;
    lua_pushboolean(L, w && w->set_text(m.host, child, text));
    return 1;
}

// shown = hs.ChildShown(name, child)   -- whether the game shows that child (nil when the window leaves it as the
// XML has it)
int ChildShown(lua_State* L) {
    StockWindow* w = nullptr;
    Child c = Impl::of(L).child(L, w);
    if (c.shown) lua_pushboolean(L, *c.shown);
    else lua_pushnil(L);
    return 1;
}

// under = hs.ChildUnder(name, child)   -- whether the window draws that child itself in its own Draw, under its
// other children (CTargetWnd::Draw's target box, CPlayerWnd::Draw's attack indicator)
int ChildUnder(lua_State* L) {
    StockWindow* w = nullptr;
    Child c = Impl::of(L).child(L, w);
    lua_pushboolean(L, c.under);
    return 1;
}

// enabled, checked, value, count, action = hs.ChildState(name, child)   -- action: a disabled button that still
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

// fraction, text [, r, g, b] = hs.ChildGauge(name, child)   -- a gauge the window fills itself; nil, "" otherwise (the
// EQType value is used); r, g, b the special value's fill tint as hs.Gauge gives it
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

// r, g, b [, a] = hs.ChildColor(name, child [, "hover"|"decal"])   -- the colour the window set on the child: its
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

// text = hs.ChildTooltip(name, child)   -- the tooltip the window set on the child, "" when none
int ChildTooltip(lua_State* L) {
    StockWindow* w = nullptr;
    Child c = Impl::of(L).child(L, w);
    lua_pushstring(L, c.tooltip ? c.tooltip->c_str() : "");
    return 1;
}

// texture, u0, v0, u1, v1 = hs.ChildArt(name, child, part)   -- what the child shows now; false when it shows
// nothing (an empty buff slot), nil when the window keeps the XML's art; the animation's name (a string) when it is
// one the UI draws itself (UiTextures::set_all_art). part: "background", "decal", "spellicon".
// Pushes hs.ChildArt's results for `a`; returns how many.
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

void set_number(lua_State* L, const char* field, double v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, field);
}
void set_nil(lua_State* L, const char* field) {
    lua_pushnil(L);
    lua_setfield(L, -2, field);
}

// r, g, b, a = hs.GemTint(name, child)   -- the colour the game tints a spell gem with (by spell category)
int GemTint(lua_State* L) {
    StockWindow* w = nullptr;
    Child c = Impl::of(L).child(L, w);
    std::array<float, 4> t = c.gem_tint.value_or(std::array<float, 4>{1, 1, 1, 1});
    for (float f : t) lua_pushnumber(L, f);
    return 4;
}

// top, r, g, b, a = hs.GemOverlay(name, child, y0, y1)   -- a spell gem's cool-down rectangle (CSpellGemWnd::Draw):
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

// info = hs.GemInfo(name, child [, into [, y0, y1]])   -- a spell gem's looks in one call, as hs.ChildShown,
// hs.ChildTooltip, hs.ChildArt(name, child, "spellicon"), hs.GemTint and hs.GemOverlay give them (each of those works
// the child out again): the table `into` (kept by the caller from frame to frame) filled, or a new one. Fields: shown
// (as hs.ChildShown), tooltip, tex, u0, v0, u1, v1 (hs.ChildArt's results, nil past the first when it gives one),
// tintR, tintG, tintB, tintA (hs.GemTint's), overlay (the cool-down rectangle's fraction, nil while idle) and with it
// coolR, coolG, coolB, coolA (its colour) and, given y0 and y1, top (hs.GemOverlay's).
int GemInfo(lua_State* L) {
    auto& m = Impl::of(L);
    const bool span = lua_isnumber(L, 4) && lua_isnumber(L, 5);
    const std::int32_t y0 = span ? number_to_int(lua_tonumber(L, 4)) : 0, y1 = span ? number_to_int(lua_tonumber(L, 5)) : 0;
    // (as m.child, the arguments first; the child's looks in a box across the pushes: plugin/lua_keep.h)
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const char* id = w ? luaL_checkstring(L, 2) : nullptr;
    // One box for every call (plugin::LuaScratch): a window draws a gem a frame each, and a new box a call was most
    // of the garbage the gems made. Given back before the return.
    plugin::LuaScratch<Child> box(L);
    Child& c = *box;
    if (w) w->child(m.host, id, c);
    record::push(L, 3, 16);
    if (c.shown) lua_pushboolean(L, *c.shown);
    else lua_pushnil(L);
    lua_setfield(L, -2, "shown");
    plugin::set_string_field(L, "tooltip", c.tooltip ? c.tooltip->c_str() : ""); // (no new string while unchanged)
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
    box.release();
    return 1;
}

// top, r, g, b, a, remainingMs, reuseMs = hs.ButtonCoolDown(name, child, y0, y1) -- a button's cool-down rectangle:
// from y0 + ftol(fraction * (y1 - y0)) down to y1, the button's width, in
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

// preview = hs.ItemPreview()   -- the item preview window's state (CObjectPreviewWnd, game/item_preview.h): nil while
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

// texture = hs.ItemPreviewImage(w, h [, view])   -- the preview's 3D view for this frame at w x h pixels (draw it
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

// hs.ItemPreviewControl(what [, x [, y [, view]]])   -- moves the preview's camera: "rotate" (radians across, up),
// "pan" (fractions of the view across, up), "zoom" (a distance factor), "reset" (fitted again); view as
// hs.ItemPreviewImage's. NaN, an infinity or a number past float's range moves nothing (any code may call it, and
// the camera's matrices would carry it).
int ItemPreviewControl(lua_State* L) {
    auto& h = Impl::of(L).host;
    const char* what = luaL_checkstring(L, 1);
    const float x = static_cast<float>(luaL_optnumber(L, 2, 0.0)), y = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    const int view = preview_view(L, 4);
    if (h.item_preview_control && std::isfinite(x) && std::isfinite(y)) h.item_preview_control(what, x, y, view);
    return 0;
}

// r, g, b, a = hs.CoolDownColor()   -- the cool-down rectangle's colour (GetUserColor(0x15f) at alpha 0x40), for an
// item's `cooldown` (hs.Item, hs.BagSlotItem, hs.SlotItem)
int CoolDownColor(lua_State* L) {
    const std::uint32_t argb = cool_down_color();
    for (int shift : {16, 8, 0, 24}) lua_pushnumber(L, static_cast<double>((argb >> shift) & 0xffu) / 255.0);
    return 4;
}

// Pushes an overlay's text, r, g, b (hs.BuffOverlay's results), or four nils for none.
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

// spellId, msLeft, name, beneficial, fade [, counter, cr, cg, cb, limit, lr, lg, lb] [, petId, slot] =
// hs.BuffSlot(name, child [, overlays [, removalDetails]])   -- the buff the button shows, nil if none
// msLeft is negative for a permanent buff; fade is
// the buff / song window's alpha factor for it (under 19 s left it fades out once a second,
// CBuffWindow::OnProcessFrame; 1 otherwise). With overlays true also the button's counter and limit overlays as
// hs.BuffOverlay(name, child, "counter") and (..., "limit") give them (each text, r, g, b; nils for none), in the one
// call; without it they are not worked out. A true fourth argument appends the entity and server slot of a
// removable pet buff (two nils otherwise).
int BuffSlot(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const char* child = w ? luaL_checkstring(L, 2) : nullptr;
    const bool overlays = lua_toboolean(L, 3) != 0;
    const bool removal_details = lua_toboolean(L, 4) != 0;
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
    if (overlays) {
        push_overlay(L, b->counter);
        push_overlay(L, b->limit);
    }
    if (!removal_details) return overlays ? 13 : 5;
    if (b->pet) {
        lua_pushinteger(L, b->pet);
        lua_pushinteger(L, b->pet_slot);
    } else {
        lua_pushnil(L);
        lua_pushnil(L);
    }
    return overlays ? 15 : 7;
}

// text, r, g, b = hs.BuffOverlay(name, child, which)   -- what a buff window draws over the button, nil if
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

// kind, label [, texture, u0, v0, u1, v1] = hs.HotButton(name, child)   -- what a hotbutton holds: "spell",
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

// face = hs.HotButtonFace(name, child)   -- a social's extras, nil for other buttons: {color = {r, g, b}, lua,
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

// checked = hs.HotButtonChecked(name, child)   -- the hotbutton drawn pressed: a pet command that is on (the pet
// window's SyncHotButtonStates)
int HotButtonChecked(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    auto hb = w ? w->hot_button(m.host, luaL_checkstring(L, 2)) : std::nullopt;
    lua_pushboolean(L, hb && hb->checked);
    return 1;
}

// st = hs.HotButtonState(name, child [, y0, y1])   -- a hotbutton's check and cool-down (CHotButtonWnd_UpdateButtonStates,
// game/hot_button_state.h), nil for an empty one: {checked, left (ms of the cool-down left, 0 idle), cooldown (the
// rectangle's fraction, nil idle), top (with y0 / y1: the rectangle's top, as hs.ButtonCoolDown), color = {r, g, b,
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

// info = hs.HotButtonInfo(name, child [, into [, y0, y1]])   -- what hs.HotButton, hs.HotButtonFace and
// hs.HotButtonState give, in one call (each of those works the button out again): the table `into` (kept by the caller
// from frame to frame) filled, or a new one. Fields: kind, label (hs.HotButton's), tex, u0, v0, u1, v1 (its icon, nil
// when none); stackCount (total carried for a stackable item link, current stack for an inventory slot, nil
// otherwise); social (true where hs.HotButtonFace gives a face) and with it color = {r, g, b}, lua, running, enabled,
// faceCooldown (the face's cooldown), overlay, tooltip (nil otherwise); state (true where hs.HotButtonState gives
// one: not empty) and with it checked, left, cooldown, top (with y0 / y1), and the spell gem's tint = {r, g, b,
// iconAlpha} with iconAlpha (nil otherwise). The cool-down's colour is hs.CoolDownColor()'s. The color and tint
// tables are refilled in place.
int HotButtonInfoFn(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const char* child = w ? luaL_checkstring(L, 2) : nullptr;
    const bool span = lua_isnumber(L, 4) && lua_isnumber(L, 5);
    const std::int32_t y0 = span ? number_to_int(lua_tonumber(L, 4)) : 0, y1 = span ? number_to_int(lua_tonumber(L, 5)) : 0;
    // Across the pushes, in one box for every call (plugin::LuaScratch): a bar asks for each button each frame.
    // Given back before the return.
    plugin::LuaScratch<std::optional<HotButtonInfo>> box(L);
    auto& hb = *box;
    if (w) hb = w->hot_button(m.host, child);
    const HotButtonInfo empty;
    const HotButtonInfo& info = hb ? *hb : empty;
    record::push(L, 3, 25);
    // (the strings only when changed: no new ones each frame for a record the caller keeps)
    plugin::set_string_field(L, "kind", info.kind.c_str());
    plugin::set_string_field(L, "label", info.label.c_str());
    if (info.stack_count > 0) {
        lua_pushinteger(L, static_cast<lua_Integer>(info.stack_count));
        lua_setfield(L, -2, "stackCount");
    } else {
        set_nil(L, "stackCount");
    }
    if (info.icon) {
        push_cell(L, *info.icon);
        set_art_fields(L, 5);
    } else {
        set_art_fields(L, 0);
    }
    // Fills the table in field `name` with `n` numbers (a new table when the field holds none).
    auto set_list = [L](const char* name, const float* v, int n) {
        lua_getfield(L, -1, name); // (the record has no metatable: record::push; the list is written raw)
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
        plugin::set_string_field(L, "overlay", hb->overlay.c_str());
        plugin::set_string_field(L, "tooltip", hb->tooltip.c_str());
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
    box.release();
    return 1;
}

// ok = hs.ClickHotButton(name, child [, "left"|"right"|"rightheld" [, x, y]])   -- left: uses what the hotbutton
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

// ok = hs.PickUp(window, child [, row])   -- a drag from that child (a list's row) puts the hotbutton it makes on
// the cursor (the Actions window's buttons, an ability of the AA window, a bar's button)
int PickUp(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    int row = lua_isinteger(L, 3) ? held_index(lua_tointeger(L, 3)) : -1;
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

// ok = hs.PressButtonFrom(window, child [, row])   -- runs the hotbutton a drag from that child (a list's row) would
// make, as pressing it on a hot bar does (DoHotButton), without putting it on the cursor: a pad's "use" for the
// Actions, AA, skills and combat ability windows' entries. Not for the hot bars (a drag takes their button off).
int PressButtonFrom(lua_State* L) {
    auto& m = Impl::of(L);
    const std::string_view name = luaL_checkstring(L, 1);
    StockWindow* w = name.starts_with("HotButtonWnd") ? nullptr : m.window(name);
    const char* child = w && m.host.in_game ? luaL_checkstring(L, 2) : nullptr;
    int row = lua_isinteger(L, 3) ? held_index(lua_tointeger(L, 3)) : -1;
    auto b = child ? w->pick_up(m.host, child, row) : std::nullopt;
    if (b && m.host.run_button) m.host.run_button(*b);
    lua_pushboolean(L, b && m.host.run_button);
    return 1;
}

// held = hs.HoldingButton()   -- a hotbutton is on the cursor
int HoldingButton(lua_State* L) {
    lua_pushboolean(L, Impl::of(L).host.cursor_button.has_value());
    return 1;
}

// hs.ClearCursorButton()   -- drops the cursor's hotbutton
int ClearCursorButton(lua_State* L) {
    Impl::of(L).host.cursor_button.reset();
    return 0;
}

// ok = hs.SpellGemClick(child [, "left"|"right"])   -- a click on that gem of the spell gem window: cast, or
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

// item = hs.SlotItem(name, child)   -- the item in an item slot of a window (loot, bank, trade, give, ...)
int SlotItem(lua_State* L) {
    auto& m = Impl::of(L);
    const char* name = luaL_checkstring(L, 1);
    const char* child = luaL_checkstring(L, 2);
    StockWindow* w = m.window(name);
    const net::Item* item = w ? w->slot_item(m.host, child) : nullptr;
    push_item(L, item);
    m.set_item_cool_down(L, item, "win:{}:{}", name, child);
    return 1;
}

// name, slots, combine, where = hs.BagInfo(bagWindow)   -- the bag's name, how many slots its window has, whether it
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

// err = hs.SortItems(where, mode [, options])   -- where: "inventory", "bank" or a bag window ("Bag3"); mode: "sort",
// "stack" (fill partial stacks from the later ones) or "tidy" (items into their filtered bags); options: { locked =
// { place, ... } (left alone), filters = { [bag place] = category } (hs.Item's category names) }. nil when it started
// (or had nothing to do), else why not
int SortItems(lua_State* L) {
    auto& m = Impl::of(L);
    const char* where_arg = luaL_checkstring(L, 1);
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
    // The options' fields onto the stack first (lua_getfield runs a table's __index, which may raise), then the
    // options and the answer in a box the state owns across the reads and the push (plugin/lua_keep.h): a place
    // that is a number becomes text on the stack, which allocates.
    lua_settop(L, 3);
    ui::lua::unview(L, 3);
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "locked");
        ui::lua::unview(L, 4);
        lua_getfield(L, 3, "filters");
        ui::lua::unview(L, 5);
    } else {
        lua_pushnil(L);
        lua_pushnil(L);
    }
    struct Job {
        net::GameConnection::ItemJobOptions opts;
        std::string err;
    };
    Job& job = plugin::lua_keep<Job>(L);
    auto& opts = job.opts;
    if (lua_istable(L, 4))
        for (lua_Integer i = 1;; ++i) {
            lua_rawgeti(L, 4, i);
            bool more = lua_isstring(L, -1);
            if (more)
                if (auto at = parse_place(lua_tostring(L, -1)))
                    plugin::cxx_guard(L, [&] { opts.locked.push_back(*at); });
            lua_pop(L, 1);
            if (!more) break;
        }
    if (lua_istable(L, 5)) {
        lua_pushnil(L);
        while (lua_next(L, 5)) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1))
                if (auto at = parse_place(lua_tostring(L, -2)))
                    if (auto c = game::category_by_name(lua_tostring(L, -1)); c != game::ItemCategory::None)
                        plugin::cxx_guard(L, [&] { opts.filters.push_back({at->parent(), c}); });
            lua_pop(L, 1);
        }
    }
    using S = net::GameConnection::ItemJobScope;
    plugin::cxx_guard(L, [&] {
        const std::string_view where = where_arg;
        if (where == "inventory") {
            job.err = m.host.conn->start_item_job(S::Inventory, {}, mode, opts);
        } else if (where == "bank") {
            job.err = m.host.conn->start_item_job(S::Bank, {}, mode, opts);
        } else {
            StockWindow* w = m.window(where);
            const net::ItemSlot* at = w ? w->bag_slot(m.host) : nullptr;
            job.err = at ? m.host.conn->start_item_job(S::Bag, *at, mode, opts) : "That bag is not open.";
        }
    });
    if (job.err.empty()) return 0;
    lua_pushstring(L, job.err.c_str());
    return 1;
}

// err = hs.SellJunk({ itemId, ... })   -- sells what you carry with those ids to the open merchant, one at a time
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

// done, total, mode, where = hs.ItemJob()   -- the job running (mode: "sort", "stack", "tidy", "sell", "destroy",
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

// hs.StopItemJob()
int StopItemJob(lua_State* L) {
    auto& h = Impl::of(L).host;
    if (h.conn) h.conn->stop_item_job();
    return 0;
}

// The bank's slot count: the open bank's, else every slot the bank can have.
int bank_slot_count(UiHost& h) { return h.conn->bank() ? h.conn->bank()->slots : 24; }

// count, slots, list = hs.FindItems(where, text)   -- the items whose name holds text (any case) in the general slots
// and their bags ("inventory"), the bank's slots and bags ("bank") or both ("all"); slots: { [top-level slot] =
// matches there, the bag's contents included } (the first place's kind); list: { { item (hs.Item's), place
// ("i:29:3"), bag (the bag's name, or nil), stale }, ... } in place order. Until the server has sent the bank in this
// zone (EQEmu's Inventory:LazyLoadBank sends it only near a banker) the bank's matches come from the bank as last
// seen (game/bank_snapshot.h; hs.BankSeen), marked stale.
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

// free, total = hs.InventorySpace()   -- the empty places in the general slots and their bags (a general slot with a
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

// parcels, claiming, busy, sending, picked, money = hs.Parcels()   -- the parcels waiting at the open parcel merchant (EQEmu's), by slot order:
// { { slot, item (hs.Item's), from, note, sent (Unix seconds) } }; claiming: how many are still to be taken (Claim
// all's queue and the one being taken; 0 when none is); busy: a retrieve is waiting for its answer; sending: a parcel
// sent is; picked: the item picked in the bags (hs.Item's), the one Send sends; money: the last money picked for
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

// err = hs.SendParcel(to, note, count [, place])   -- sends the item picked in the bags (or the one at the place,
// "i:29:3") by parcel, `count` of a stack; err = hs.SendParcelMoney(to, note, pp, gp, sp, cp)   -- sends money.
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
    // (held to a count's range: a cast would wrap 2^32 to 0)
    const auto n = static_cast<std::uint32_t>(std::clamp<lua_Integer>(count, 1, std::numeric_limits<std::uint32_t>::max()));
    std::string why = h.conn->send_parcel(*at, n, to, note);
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

// hs.RetrieveParcel(slot)   -- takes one parcel;  hs.ClaimAllParcels()   -- takes them all, one after another
int RetrieveParcel(lua_State* L) {
    auto& h = Impl::of(L).host;
    const auto slot = integer_as<std::uint32_t>(luaL_checkinteger(L, 1)); // (not wrapped into another parcel's)
    if (h.conn && slot) h.conn->retrieve_parcel(*slot);
    return 0;
}

int ClaimAllParcels(lua_State* L) {
    auto& h = Impl::of(L).host;
    if (h.conn) h.conn->claim_all_parcels();
    return 0;
}

// ok = hs.PlaceClick(place, button)   -- the slot logic at a place ("i:29:3", "b:0:2"; a bank place needs the bank
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

// item = hs.BagSlotItem(bagWindow, index)   -- index from 0
int BagSlotItem(lua_State* L) {
    auto& m = Impl::of(L);
    const char* name = luaL_checkstring(L, 1);
    const int index = held_int(luaL_checkinteger(L, 2));
    StockWindow* w = m.window(name);
    const net::Item* item = w ? w->bag_item(m.host, index) : nullptr;
    push_item(L, item);
    m.set_item_cool_down(L, item, "bag:{}:{}", name, index);
    return 1;
}

// ok = hs.BagSlotClick(bagWindow, index [, button [, outside]])   -- the slot logic on that bag slot ("leftheld" as
// hs.ClickSlot takes it: false when no hotbutton went on the cursor)
int BagSlotClick(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    int index = held_int(luaL_checkinteger(L, 2));
    std::string button = lua_isstring(L, 3) ? lua_tostring(L, 3) : "left";
    if (button == "rightheld" && w && w->bag_item(m.host, index) && index >= 0) {
        inspect_held(m.host, w->bag_item(m.host, index));
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, w && w->bag_click(m.host, index, button, lua_toboolean(L, 4) != 0));
    return 1;
}

// rows, columns, selected, labels, widths = hs.ListInfo(name, child)   -- nil when the child is not a list
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

// texts, r, g, b [, texture, u0, v0, u1, v1] = hs.ListRow(name, child, row)   -- row from 0; texts.altRight,
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

// ok = hs.ListClick(name, child, row [, "left"|"right"|"double" [, column [, mods]]])   -- "double": a double
// click, after the click's own "left"; mods (1 shift, 2 ctrl) for a list that selects several rows
int ListClick(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    const std::string_view button = lua_isstring(L, 4) ? lua_tostring(L, 4) : "left"; // (nothing to destroy)
    bool right = button == "right";
    int column = held_int(luaL_optinteger(L, 5, 0));
    if (button == "double") {
        lua_pushboolean(L, w && w->list_double_click(m.host, luaL_checkstring(L, 2), held_int(luaL_checkinteger(L, 3)), column));
        return 1;
    }
    if (!right && w && lua_isinteger(L, 6) &&
        w->list_select(m.host, luaL_checkstring(L, 2), held_int(luaL_checkinteger(L, 3)), held_int(lua_tointeger(L, 6)))) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (right) note_click_rect(m.host);
    lua_pushboolean(L, w && w->list_click(m.host, luaL_checkstring(L, 2), held_int(luaL_checkinteger(L, 3)),
                                          right, column));
    return 1;
}

// ok = hs.ListHeaderClick(name, child, column)   -- a click on a list's column header (0-based)
int ListHeaderClick(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    lua_pushboolean(L, w && w->list_header_click(m.host, luaL_checkstring(L, 2), held_int(luaL_checkinteger(L, 3))));
    return 1;
}

// choices, selected = hs.ComboInfo(name, child)   -- the combo box's choices and the chosen one (0-based); nil when
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

// value = hs.Option(name)   -- a client option (ui/<pack>/options.lua; ClientApp::option_value); nil for one the
// client does not have
int Option(lua_State* L) {
    auto& m = Impl::of(L);
    std::optional<int> v = m.host.find_option ? m.host.find_option(luaL_checkstring(L, 1)) : std::nullopt;
    if (v) lua_pushinteger(L, *v);
    else lua_pushnil(L);
    return 1;
}

// value = hs.UiIni(section, key)   -- a value of the character's UI ini (over defaults.ini), nil when neither has it
int UiIniGet(lua_State* L) {
    auto& m = Impl::of(L);
    const char* section = luaL_checkstring(L, 1);
    const char* key = luaL_checkstring(L, 2);
    auto& v = plugin::lua_keep<std::optional<std::string>>(L); // (across the push: lua_keep.h)
    plugin::cxx_guard(L, [&] { v = m.layout.get(section, key); });
    if (v) lua_pushlstring(L, v->data(), v->size());
    else lua_pushnil(L);
    return 1;
}

// hs.SetUiIni(section, key, value)   -- writes a value to the character's UI ini (UIIni_WriteString). The file is
// written with the window places (UiIni::save_if_due, at most every 2 s) and at once on a quit, a camp, a return to
// the login screen or another character's layout (UiApi::save_layout, load_layout), not on each call: a slider
// dragged calls it every frame.
int UiIniSet(lua_State* L) {
    auto& m = Impl::of(L);
    const char* section = luaL_checkstring(L, 1);
    const char* key = luaL_checkstring(L, 2);
    const char* value = luaL_checkstring(L, 3);
    plugin::cxx_guard(L, [&] { m.layout.put(section, key, value); });
    return 0;
}

void push_interface_strings(lua_State* L, const std::vector<std::string>& strings) {
    lua_createtable(L, static_cast<int>(strings.size()), 0);
    for (std::size_t i = 0; i < strings.size(); ++i) {
        lua_pushlstring(L, strings[i].data(), strings[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
}

void push_interface_pack(lua_State* L, const ui::UiPack& pack) {
    lua_createtable(L, 0, 12);
    const auto text = [&](const char* key, const std::string& value) {
        lua_pushlstring(L, value.data(), value.size()); lua_setfield(L, -2, key);
    };
    text("id", pack.id); text("name", pack.name); text("description", pack.description);
    text("input", pack.input); text("source", pack.source); text("addon", pack.addon);
    text("base", pack.base); text("runtime", pack.runtime);
    text("reason", pack.unavailable_reason); text("version", pack.version);
    lua_pushboolean(L, pack.available); lua_setfield(L, -2, "available");
    push_interface_strings(L, pack.base_ids); lua_setfield(L, -2, "bases");
}

// hs.InterfaceList() -> { {id,name,description,input,source,addon,base,bases,runtime,available,reason,version}, ... }
// Installed interface records include unavailable backends, so selection and actual loading remain distinguishable.
int InterfaceList(lua_State* L) {
    auto& packs = plugin::lua_keep<std::vector<ui::UiPack>>(L);
    plugin::cxx_guard(L, [&] { if (Impl::of(L).host.interface_list) packs = Impl::of(L).host.interface_list(); });
    lua_createtable(L, static_cast<int>(packs.size()), 0);
    for (std::size_t i = 0; i < packs.size(); ++i) {
        push_interface_pack(L, packs[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// hs.InterfaceLoadBase(relativeLuaFile) -> all results returned by the nearest approved base's text Lua file.
// Imports skip the selected UI root and preserve the caller's environment, capability scope and presenter owner.
int InterfaceLoadBase(lua_State* L) { return ui::lua::load_base_file(L); }

// hs.InterfaceLoadData(relativeLuaFile [, baseOnly]) -> all results from a shared presentation-data Lua module.
// Searches approved selected/base data IDs; true skips selected data. Caller environment and permissions remain.
int InterfaceLoadData(lua_State* L) { return ui::lua::load_data_file(L); }

// hs.InterfaceStatus() -> {selected,loaded,state,bases,diagnostics,scriptCount,loadedScriptCount,errorCount,windowCount};
// selected/loaded have InterfaceList's record shape.
// state is loading, ready, pending, fallback or error. Counts report actual Lua loading, not metadata acceptance.
int InterfaceStatus(lua_State* L) {
    auto& status = plugin::lua_keep<ui::InterfaceStatus>(L);
    plugin::cxx_guard(L, [&] { if (Impl::of(L).host.interface_status) status = Impl::of(L).host.interface_status(); });
    lua_createtable(L, 0, 9);
    push_interface_pack(L, status.selected); lua_setfield(L, -2, "selected");
    push_interface_pack(L, status.loaded); lua_setfield(L, -2, "loaded");
    lua_pushlstring(L, status.state.data(), status.state.size()); lua_setfield(L, -2, "state");
    push_interface_strings(L, status.bases); lua_setfield(L, -2, "bases");
    push_interface_strings(L, status.diagnostics); lua_setfield(L, -2, "diagnostics");
    lua_pushinteger(L, static_cast<lua_Integer>(status.script_count)); lua_setfield(L, -2, "scriptCount");
    lua_pushinteger(L, static_cast<lua_Integer>(status.loaded_script_count)); lua_setfield(L, -2, "loadedScriptCount");
    lua_pushinteger(L, static_cast<lua_Integer>(status.error_count)); lua_setfield(L, -2, "errorCount");
    lua_pushinteger(L, static_cast<lua_Integer>(status.window_count)); lua_setfield(L, -2, "windowCount");
    return 1;
}

// hs.SetOption(name, value)   -- changes, applies and saves a client option
int SetOption(lua_State* L) {
    auto& m = Impl::of(L);
    if (m.host.set_option) m.host.set_option(luaL_checkstring(L, 1), held_int(luaL_checkinteger(L, 2)));
    return 0;
}

// played = hs.PlaySoundTest(name)   -- plays one of the Individual Sounds options ("own_footsteps", "thunder", ...) once
// at its volume; false for a name that is not one
int PlaySoundTest(lua_State* L) {
    auto& m = Impl::of(L);
    const char* name = luaL_checkstring(L, 1);
    lua_pushboolean(L, m.host.play_sound_test && m.host.play_sound_test(name));
    return 1;
}

// choices = hs.OptionChoices(name)   -- the choices the client lists for an option ("resolution"), else empty
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

// ok = hs.ComboSelect(name, child, index)   -- picks choice `index` (0-based), as the stock drop-down does
int ComboSelect(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    lua_pushboolean(L, w && w->combo_select(m.host, luaL_checkstring(L, 2), held_int(luaL_checkinteger(L, 3))));
    return 1;
}

// rows = hs.WindowList(name, child)   -- each row a table of column texts
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

// index = hs.TabPage(name, child)   -- the page the window's tab box shows (0-based), nil if it is not one
int TabPage(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    auto p = w ? w->tab_page(m.host, luaL_checkstring(L, 2)) : std::nullopt;
    if (p) lua_pushinteger(L, *p);
    else lua_pushnil(L);
    return 1;
}

// hs.SetTabPage(name, child, index)
int SetTabPage(lua_State* L) {
    auto& m = Impl::of(L);
    if (StockWindow* w = m.window(luaL_checkstring(L, 1)))
        w->set_tab_page(m.host, luaL_checkstring(L, 2), held_int(luaL_checkinteger(L, 3)));
    return 0;
}

// text = hs.WindowTitle(name)   -- the title the game shows on the window (a merchant's name); "" for the XML's
int WindowTitle(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    std::string t = w ? w->title(m.host) : std::string();
    lua_pushstring(L, t.c_str());
    return 1;
}

// x, y, w, h, relative [, right, bottom] = hs.ChildRect(window, child)   -- where the window's code placed the
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

// w, h = hs.WindowClient(window [, frameW, frameH])   -- the client size of a window that sizes itself in code (the
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

// hs.SizeWindow(window, w, h [, frameW, frameH])   -- the user resized such a window to a w x h client area
int SizeWindow(lua_State* L) {
    auto& m = Impl::of(L);
    if (StockWindow* w = m.window(luaL_checkstring(L, 1)))
        w->size_window(m.host, saturate_cast<int>(luaL_checknumber(L, 2) + luaL_optnumber(L, 4, 0)),
                       saturate_cast<int>(luaL_checknumber(L, 3) + luaL_optnumber(L, 5, 0)));
    return 0;
}

// ok = hs.SetSlider(window, child, value)   -- drags the window's slider to value
int SetSlider(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    lua_pushboolean(L, w && w->set_slider(m.host, luaL_checkstring(L, 2), held_int(luaL_checkinteger(L, 3))));
    return 1;
}

// a = hs.CursorAttachment()   -- what CCursorAttachment carries on the mouse (an item), nil when nothing:
// { icon, iconRect, rect, quantity, font, spell (a book spell's name), coin (money: 0 platinum .. 3 copper, its
// count in quantity, no icon: the pack draws the coin) }; images { texture, u0, v0, u1, v1 }, rects { x, y, w, h } on the screen.
// The window (EQUI_CursorAttachment.xml, 50 x 50) sits at the mouse with the icon (CA_Anim2) at 5, 5, 40 x 40;
// DrawQuantity writes a stack's count in a dark box at its bottom right.
int CursorAttachment(lua_State* L) {
    auto& m = Impl::of(L);
    map_entry(m.replaced_at, std::string_view("CursorAttachment")) = m.frame;
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

// menus = hs.CascadeMenus()   -- the open EQ menu and submenus: { x, y, w, h, items = { { kind, text, rect, icon,
// decalW, decalH, textX, enabled, arrow, arrowRect, background } } } (app/lua_ui_menu.h)
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

// hs.CascadeClick(menu, item) / hs.CascadeHover(menu, item) / hs.CascadeClose()   -- 1-based menu and item
int CascadeClick(lua_State* L) {
    auto& m = Impl::of(L);
    auto cmd = m.cascade.click(held_index(luaL_checkinteger(L, 1)), held_index(luaL_checkinteger(L, 2)));
    if (cmd && *cmd == kMenuReportBug) {
        if (m.host.conn) m.host.conn->set_bug_report_open(true);
    } else if (cmd && *cmd == kMenuScripts) {
        if (StockWindow* w = m.window("ScriptsWnd")) w->set_shown(m.host, true);
    } else if (cmd && *cmd == kMenuWebBrowser) {
        if (StockWindow* w = m.window("BrowserWnd")) w->set_shown(m.host, true);
    } else if (cmd && *cmd == kMenuLayoutProfiles) {
        if (m.layout_profiles_ready())
            if (StockWindow* w = m.window("LayoutProfilesWindow")) w->set_shown(m.host, true);
    } else if (cmd && *cmd >= kMenuPluginFirst) { // a plugin's entry: its command line, as the plugin
        run_plugin_menu(m.host, static_cast<std::size_t>(*cmd - kMenuPluginFirst));
    } else if (cmd && m.host.run_command) m.host.run_command(*cmd);
    return 0;
}
int CascadeHover(lua_State* L) {
    auto& m = Impl::of(L);
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    m.cascade.hover(held_index(luaL_checkinteger(L, 1)), held_index(luaL_checkinteger(L, 2)),
                    static_cast<int>(ds.x), static_cast<int>(ds.y));
    return 0;
}
int CascadeClose(lua_State* L) {
    Impl::of(L).cascade.close();
    return 0;
}

// menus = hs.GameMenus([scale [, width [, row_height [, separator_height]]]])   -- { x, y, w, h, rows }
// Rows have text, r, g, b, check, enabled, separator and submenu. The default sizes use EQ font 2 at `scale`
// (1 by default, 0.5..3). Fixed dimensions are in pixels. Width includes padding. Row heights exclude spacing.
int GameMenus(lua_State* L) {
    auto& m = Impl::of(L);
    const float scale = std::clamp(static_cast<float>(luaL_optnumber(L, 1, 1.0)), 0.5f, 3.0f);
    ImFont* font = ui::ui_font(2);
    const float fs = (font ? font->LegacySize : ImGui::GetFontSize()) * scale;
    const float width = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    const float row_height = static_cast<float>(luaL_optnumber(L, 3, fs));
    const float separator_height = static_cast<float>(luaL_optnumber(L, 4, 1.0));
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const auto& menus = m.host.menus;
    float parent_left = 0.0f, parent_right = 0.0f, opener_y = 0.0f;
    bool opens_left = false;
    lua_newtable(L);
    int n = 0;
    for (const auto& menu : menus) {
        const std::size_t next = static_cast<std::size_t>(n) + 1;
        const std::size_t child_row = next < menus.size() ? menus[next].parent_row : menu.rows.size();
        std::size_t row = 0;
        float text_width = 0.0f, content_bottom = st.WindowPadding.y, child_offset = 0.0f;
        for (const auto& r : menu.rows) {
            if (row++ == child_row) child_offset = content_bottom;
            if (r.separator) {
                content_bottom += separator_height + st.ItemSpacing.y;
                continue;
            }
            if (width <= 0.0f) {
                const float tw = font ? font->CalcTextSizeA(fs, FLT_MAX, 0.0f, r.text.c_str()).x
                                      : ImGui::CalcTextSize(r.text.c_str()).x * scale;
                text_width = std::max(text_width, tw);
            }
            content_bottom += row_height + st.ItemSpacing.y;
        }
        // Leave room for the check and the submenu arrow beside the text.
        const float w = width > 0.0f ? width : std::max(40.0f, text_width + fs * 3.0f + st.WindowPadding.x * 2);
        const float h = std::max(10.0f, content_bottom + st.WindowPadding.y);
        float x = menu.x, y = menu.y;
        if (n > 0) {
            // Stay on the same side until the next menu needs more room. If neither side fits, use the larger gap.
            if (ds.x > 0.0f) {
                const float left = parent_left, right = ds.x - parent_right;
                const float preferred = opens_left ? left : right;
                const float opposite = opens_left ? right : left;
                if (w > preferred && opposite > preferred) opens_left = !opens_left;
            }
            x = opens_left ? parent_left - w : parent_right;
            y = opener_y;
        }
        // CContextMenuManager::PopupMenu moves a menu that would leave the screen.
        if (ds.x > 0.0f) x = std::clamp(x, 0.0f, std::max(0.0f, ds.x - w));
        if (ds.y > 0.0f) y = std::clamp(y, 0.0f, std::max(0.0f, ds.y - h));
        parent_left = x;
        parent_right = x + w;
        opener_y = y + child_offset;
        lua_createtable(L, 0, 5);
        auto num = [&](const char* k, double v) {
            lua_pushnumber(L, v);
            lua_setfield(L, -2, k);
        };
        num("x", x), num("y", y), num("w", w), num("h", h);
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
void menu_hover(UiHost& h, std::size_t mi, std::size_t ri) {
    if (mi >= h.menus.size() || ri >= h.menus[mi].rows.size()) return;
    h.menus.resize(mi + 1);
    const auto& row = h.menus[mi].rows[ri];
    if (!row.submenu || !row.enabled) return;
    UiHost::GameMenu sub;
    sub.parent_row = ri;
    sub.rows = row.submenu();
    h.menus.push_back(std::move(sub));
}

// Chosen: runs the row and closes the menus (a submenu row opens its submenu).
bool menu_choose(UiHost& h, std::size_t mi, std::size_t ri) {
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

// hs.GameMenuHover(menu, row)   -- 1-based menu, 0-based row
int GameMenuHover(lua_State* L) {
    menu_hover(Impl::of(L).host, static_cast<std::size_t>(luaL_checkinteger(L, 1) - 1),
               static_cast<std::size_t>(luaL_checkinteger(L, 2)));
    return 0;
}

// hs.GameMenuClick(menu, row)   -- 1-based menu, 0-based row
int GameMenuClick(lua_State* L) {
    menu_choose(Impl::of(L).host, static_cast<std::size_t>(luaL_checkinteger(L, 1) - 1),
                static_cast<std::size_t>(luaL_checkinteger(L, 2)));
    return 0;
}

int GameMenuClose(lua_State* L) {
    Impl::of(L).host.menus.clear();
    return 0;
}

// taken = hs.WindowContextMenu(name)   -- a right click on the window: its context menu at the mouse, if it has one
// (taken, and nothing opens, while context menus are off: the window's own menu is one too)
int WindowContextMenu(lua_State* L) {
    auto& m = Impl::of(L);
    StockWindow* w = m.window(luaL_checkstring(L, 1));
    ImVec2 mp = ImGui::GetMousePos();
    lua_pushboolean(L, !m.host.context_menus() || (w && w->context_menu(m.host, mp.x, mp.y)));
    return 1;
}

// on = hs.ContextMenus()   -- whether a right click may open a context menu (UiHost::context_menus)
int ContextMenus(lua_State* L) {
    lua_pushboolean(L, Impl::of(L).host.context_menus());
    return 1;
}

// yes = hs.TakeWindowMenu(name)   -- a context menu's "Window" row asked for the window's own menu (once)
int TakeWindowMenu(lua_State* L) {
    auto& h = Impl::of(L).host;
    bool yes = !h.window_menu.empty() && h.window_menu == luaL_checkstring(L, 1);
    if (yes) h.window_menu.clear();
    lua_pushboolean(L, yes);
    return 1;
}
// yes = hs.TakeCloseTopWindow()   -- the close-top-window key (Escape) went down this frame and is the caller's (once)
int TakeCloseTopWindow(lua_State* L) {
    lua_pushboolean(L, std::exchange(Impl::of(L).host.close_top_window, false));
    return 1;
}
int ReturnFalse(lua_State* L) {
    lua_pushboolean(L, false);
    return 1;
}
bool layout_profiles_allowed() { return ui::lua::LuaUi::caller_tag().empty(); }

int layout_profile_result(lua_State* L, const char* error, const char* code = nullptr) {
    lua_pushboolean(L, error == nullptr);
    if (!error) return 1;
    lua_pushstring(L, error);
    if (!code) return 2;
    lua_pushstring(L, code);
    return 3;
}

int LayoutProfilesList(lua_State* L) {
    auto& m = Impl::of(L);
    std::vector<std::string> names;
    if (layout_profiles_allowed() && m.layout_profiles_ready())
        plugin::cxx_guard(L, [&] { names = ui::list_layout_profiles(m.layout_profile_directory()); });
    lua_createtable(L, static_cast<int>(names.size()), 0);
    int row = 0;
    for (const auto& name : names) {
        lua_pushlstring(L, name.data(), name.size());
        lua_rawseti(L, -2, ++row);
    }
    return 1;
}

int LayoutProfilesSave(lua_State* L) {
    std::size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    auto& m = Impl::of(L);
    if (!layout_profiles_allowed() || !m.layout_profiles_ready())
        return layout_profile_result(L, "Layout profiles are unavailable.");
    if (!ui::valid_layout_profile_name({name, length}))
        return layout_profile_result(L, "Use up to 48 letters, numbers, spaces, hyphens or underscores.");
    if (m.reload_layout) return layout_profile_result(L, "Wait for the layout change to finish.");
    const bool overwrite = lua_toboolean(L, 2) != 0;
    ui::LayoutProfileSave result = ui::LayoutProfileSave::Failed;
    plugin::cxx_guard(L, [&] {
        result = ui::save_layout_profile(m.layout_profile_directory(), m.capture_layout_profile({name, length}), overwrite);
    });
    if (result == ui::LayoutProfileSave::Exists)
        return layout_profile_result(L, "A profile with that name already exists.", "exists");
    if (result == ui::LayoutProfileSave::Failed) return layout_profile_result(L, "Could not save the layout profile.");
    return layout_profile_result(L, nullptr);
}

int LayoutProfilesPreview(lua_State* L) {
    std::size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    auto& m = Impl::of(L);
    if (!layout_profiles_allowed() || !m.layout_profiles_ready()) {
        lua_pushnil(L);
        lua_pushliteral(L, "Layout profiles are unavailable.");
        return 2;
    }
    auto& profile = m.layout_profile_preview;
    plugin::cxx_guard(L, [&] { profile = ui::read_layout_profile(m.layout_profile_directory(), {name, length}); });
    if (!profile) {
        lua_pushnil(L);
        lua_pushliteral(L, "Layout profile is unavailable.");
        return 2;
    }
    std::set<std::string> windows;
    std::size_t chat = 0;
    plugin::cxx_guard(L, [&] {
        for (const auto& section : profile->windows.sections())
            if (!game::same_no_case(section, "ChatManager")) windows.insert(section);
        std::string_view settings = profile->pack_settings;
        while (!settings.empty()) {
            const auto end = settings.find('\n');
            const auto line = settings.substr(0, end);
            const auto key = line.substr(0, line.find('='));
            if (key.starts_with("hud.pos.")) windows.emplace(key.substr(8));
            if (key.starts_with("hud.size.")) windows.emplace(key.substr(9));
            settings.remove_prefix(end == std::string_view::npos ? settings.size() : end + 1);
        }
        chat = profile->windows.keys("ChatManager").size();
        for (const auto& section : profile->chat.sections()) chat += profile->chat.keys(section).size();
    });
    const auto& rects = profile->rects;
    lua_createtable(L, 0, 6);
    lua_pushlstring(L, profile->name.data(), profile->name.size());
    lua_setfield(L, -2, "name");
    auto number = [L](const char* key, lua_Integer value) {
        lua_pushinteger(L, value);
        lua_setfield(L, -2, key);
    };
    number("width", profile->width);
    number("height", profile->height);
    number("windows", static_cast<lua_Integer>(windows.size()));
    number("chat", static_cast<lua_Integer>(chat));
    lua_createtable(L, static_cast<int>(rects.size()), 0);
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const auto& rect = rects[i];
        lua_createtable(L, 0, 5);
        lua_pushlstring(L, rect.name.data(), rect.name.size());
        lua_setfield(L, -2, "name");
        number("x", rect.x);
        number("y", rect.y);
        number("w", rect.w);
        number("h", rect.h);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "rects");
    return 1;
}

int LayoutProfilesApply(lua_State* L) {
    std::size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    auto& m = Impl::of(L);
    if (!layout_profiles_allowed() || !m.layout_profiles_ready())
        return layout_profile_result(L, "Layout profiles are unavailable.");
    if (m.reload_layout) return layout_profile_result(L, "Wait for the layout change to finish.");
    const auto& preview = m.layout_profile_preview;
    if (!preview || !game::same_no_case(preview->name, std::string_view(name, length)))
        return layout_profile_result(L, "Select a saved profile to preview it first.");
    const bool discard_unsaved = lua_toboolean(L, 2) != 0;
    const char* error = nullptr;
    bool unsaved = false;
    std::optional<ui::LayoutProfile> profile;
    // Read the file again: another client may have overwritten the profile since it was previewed.
    plugin::cxx_guard(L, [&] { profile = ui::read_layout_profile(m.layout_profile_directory(), {name, length}); });
    if (!profile) return layout_profile_result(L, "Layout profile is unavailable.");
    plugin::cxx_guard(L, [&] {
        auto previous = m.capture_layout_profile({});
        if (!discard_unsaved && !ui::has_saved_layout_profile(m.layout_profile_directory(), previous)) {
            unsaved = true;
            return;
        }
        error = m.apply_layout_profile(*profile, previous);
        if (!error) m.layout_profile_undo = std::move(previous);
    });
    if (unsaved) return layout_profile_result(L, "Save your current layout before applying?", "unsaved");
    return layout_profile_result(L, error);
}

int LayoutProfilesCanUndo(lua_State* L) {
    auto& m = Impl::of(L);
    lua_pushboolean(L, layout_profiles_allowed() && m.layout_profiles_ready() && !m.reload_layout &&
                           m.layout_profile_undo.has_value());
    return 1;
}

int LayoutProfilesUndo(lua_State* L) {
    auto& m = Impl::of(L);
    if (!layout_profiles_allowed() || !m.layout_profiles_ready() || !m.layout_profile_undo)
        return layout_profile_result(L, "There is no layout change to undo.");
    if (m.reload_layout) return layout_profile_result(L, "Wait for the layout change to finish.");
    const char* error = nullptr;
    plugin::cxx_guard(L, [&] {
        auto previous = m.capture_layout_profile({});
        error = m.apply_layout_profile(*m.layout_profile_undo, previous);
        if (!error) m.layout_profile_undo.reset();
    });
    return layout_profile_result(L, error);
}

int LayoutProfilesGeneration(lua_State* L) {
    lua_pushinteger(L, Impl::of(L).layout_profile_generation);
    return 1;
}

int ReturnEmptyTable(lua_State* L) {
    lua_newtable(L);
    return 1;
}

// list = hs.DebugScreens()   -- every stock window model: { name, shown, replaced }
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

UiApi::UiApi(UiHost& host) : impl_(std::make_unique<Impl>(host)) {
    // The open bags live in a process-wide table (lua_ui_items.cpp): a new UiApi must not inherit the last one's.
    reset_containers();
}
UiApi::~UiApi() {
    UiHost& h = impl_->host;
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

void UiApi::begin_frame() {
    Impl& m = *impl_;
    ++m.frame;
    if (m.reload_layout) { // a copied layout (__CopyLayout's unk_ApplyWindowPositions)
        m.reload_layout = false;
        load_layout(m.layout_defaults, m.layout_file, m.layout_seed);
        if (std::exchange(m.layout_profile_reloading, false)) ++m.layout_profile_generation;
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
    m.store_visibility();
    m.layout.save_if_due(std::chrono::steady_clock::now());
}

void UiApi::load_layout(const std::filesystem::path& defaults, const std::filesystem::path& file,
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
        if (!(info.flags & 0x100)) continue;
        w->set_shown(m.host, m.layout.shown(info).value_or((info.flags & 0x200) != 0)); // bInitVisibility
        m.last_shown.emplace(name, w->shown(m.host));
    }
    for (auto& w : m.windows) w->ini_loaded(m.host, m.layout, file.parent_path());
    chat_layout_loaded(m.layout, m.host, file.parent_path(), m.layout_profile_reloading);
    log::info("ui: window places from {} (defaults {})", file.string(), defaults.string());
}

void UiApi::save_layout() {
    impl_->store_visibility();
    impl_->layout.save();
}

void UiApi::clear_layout_profile_undo() {
    auto& m = *impl_;
    m.layout_profile_preview.reset();
    m.layout_profile_undo.reset();
    if (std::exchange(m.layout_profile_reloading, false)) m.reload_layout = false;
    show("LayoutProfilesWindow", false);
}

const std::filesystem::path& UiApi::layout_file() const { return impl_->layout_file; }

void UiApi::reload_game_files() { impl_->cascade.reload(); }

bool UiApi::click_child(std::string_view screen, std::string_view child, std::string_view button) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->click(impl_->host, child, button, false);
}

bool UiApi::click_hot_button(std::string_view screen, std::string_view child, std::string_view button) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->click_hot_button(impl_->host, child, button);
}

bool UiApi::select_combo(std::string_view screen, std::string_view child, int index) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->combo_select(impl_->host, child, index);
}

std::vector<std::vector<std::string>> UiApi::list_rows(std::string_view screen, std::string_view child) {
    std::vector<std::vector<std::string>> out;
    StockWindow* w = impl_->window(screen);
    if (const ListData* d = w ? w->list(impl_->host, child) : nullptr)
        for (const auto& r : d->rows) out.push_back(r.texts);
    return out;
}

bool UiApi::header_click_list(std::string_view screen, std::string_view child, int column) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->list_header_click(impl_->host, child, column);
}

bool UiApi::select_list(std::string_view screen, std::string_view child, int row, int mods) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->list_select(impl_->host, child, row, mods);
}

bool UiApi::double_click_list(std::string_view screen, std::string_view child, int row) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->list_double_click(impl_->host, child, row, 0);
}

bool UiApi::click_list(std::string_view screen, std::string_view child, int row, int column) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    if (!w || !w->list(impl_->host, child)) return false;
    return w->list_click(impl_->host, child, row, false, column);
}

bool UiApi::set_child_text(std::string_view screen, std::string_view child, const std::string& text) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->set_text(impl_->host, child, text);
}

std::optional<std::string> UiApi::child_text(std::string_view screen, std::string_view child) {
    StockWindow* w = impl_->window(screen);
    if (!w) return std::nullopt;
    Child c;
    w->child(impl_->host, child, c);
    return c.text;
}

std::optional<bool> UiApi::child_shown(std::string_view screen, std::string_view child) {
    StockWindow* w = impl_->window(screen);
    if (!w) return std::nullopt;
    Child c;
    w->child(impl_->host, child, c);
    return c.shown;
}

std::optional<bool> UiApi::child_checked(std::string_view screen, std::string_view child) {
    StockWindow* w = impl_->window(screen);
    if (!w) return std::nullopt;
    Child c;
    w->child(impl_->host, child, c);
    return c.checked;
}

std::optional<bool> UiApi::child_state(std::string_view screen, std::string_view child) {
    StockWindow* w = impl_->window(screen);
    if (!w) return std::nullopt;
    Child c;
    w->child(impl_->host, child, c);
    return c.enabled;
}

bool UiApi::stml_link(std::string_view screen, std::string_view child, int message, std::string_view data) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->stml_link(impl_->host, child, message, data);
}

std::vector<std::string> UiApi::list_texts(std::string_view screen, std::string_view child) {
    std::vector<std::string> out;
    StockWindow* w = impl_->window(screen);
    const ListData* d = w ? w->list(impl_->host, child) : nullptr;
    if (d)
        for (const auto& r : d->rows) out.push_back(r.texts.empty() ? std::string() : r.texts[0]);
    return out;
}

void UiApi::cascade_hover(int menu, int item) {
    impl_->lists_changed();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    impl_->cascade.hover(menu, item, static_cast<int>(ds.x), static_cast<int>(ds.y));
}

bool UiApi::context_menu(std::string_view screen, float x, float y) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return impl_->host.context_menus() && w && w->context_menu(impl_->host, x, y);
}

std::optional<int> UiApi::window_setting(std::string_view screen, std::string_view name) {
    StockWindow* w = impl_->window(screen);
    return w ? w->setting(name) : std::nullopt;
}

bool UiApi::set_window_setting(std::string_view screen, std::string_view name, int value) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->set_setting(impl_->host, name, value);
}

bool UiHost::context_menus() const {
    if (!conn) return true;
    return game::context_menus_open(conn->client_option(game::ClientOption::ContextMenus), in_game && conn->player_down());
}

bool UiApi::menu_click(int menu, int row) {
    impl_->lists_changed();
    if (menu < 0 || row < 0) return false;
    return menu_choose(impl_->host, static_cast<std::size_t>(menu), static_cast<std::size_t>(row));
}

bool UiApi::activate_tip(int tip) {
    impl_->lists_changed();
    return impl_->activate_context_tip(tip, false);
}

bool UiApi::activate_window(std::string_view screen, int arg) {
    impl_->lists_changed();
    StockWindow* w = impl_->window(screen);
    return w && w->activate(impl_->host, arg);
}

bool UiApi::click_gem(int gem, bool right) {
    impl_->lists_changed();
    StockWindow* w = impl_->window("CastSpellWnd");
    return w && w->gem_click(impl_->host, "CSPW_Spell" + std::to_string(gem), right);
}

bool UiApi::activate_tip_of_day(bool force) {
    impl_->lists_changed();
    StockWindow* w = impl_->window("TipWindow#2");
    return w && w->activate(impl_->host, force ? -1 + kTipForce : -1);
}

bool UiApi::replaces(std::string_view screen) const {
    if (impl_->host.keep_cpp_windows) return false;
    auto it = impl_->replaced_at.find(screen);
    return it != impl_->replaced_at.end() && impl_->frame - it->second <= 2;
}

bool UiApi::shown(std::string_view screen) const {
    StockWindow* w = impl_->window(screen);
    return w && w->shown(impl_->host);
}

void UiApi::show(std::string_view screen, bool show) {
    impl_->lists_changed();
    if (StockWindow* w = impl_->window(screen)) w->set_shown(impl_->host, show);
}

void UiApi::toggle(std::string_view screen) { show(screen, !shown(screen)); }

void UiApi::set_tab_page(std::string_view screen, std::string_view child, int page) {
    impl_->lists_changed();
    if (StockWindow* w = impl_->window(screen)) w->set_tab_page(impl_->host, child, page);
}

std::vector<std::uint32_t> UiApi::map_command(std::string_view args) {
    impl_->lists_changed();
    StockWindow* w = impl_->window("MapViewWnd");
    return w ? app::map_command(*w, impl_->host, args) : std::vector<std::uint32_t>{};
}

bool UiApi::add_player_note(std::string_view text) {
    impl_->lists_changed();
    StockWindow* w = impl_->window("PlayerNotesWindow");
    return w && player_notes_add(*w, text);
}

void UiApi::actions_key(int page) {
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

void UiApi::right_click_item(const net::ItemSlot& slot) { item_right_click(impl_->host, slot); }
void UiApi::right_click_item(const net::ItemSlot& slot, net::SlotClickKeys keys) {
    item_right_click(impl_->host, slot, keys);
}
bool UiApi::open_player_menu(std::uint32_t spawn_id, float x, float y) {
    return app::open_player_menu(impl_->host, spawn_id, x, y);
}
void UiApi::open_all_bags() { app::open_all_bags(impl_->host); }
void UiApi::close_all_bags() { app::close_all_bags(impl_->host); }
void UiApi::toggle_all_bags() { app::toggle_all_bags(impl_->host); }
void UiApi::reset_bags() { reset_containers(); }
void UiApi::toggle_all_bank_bags() { app::toggle_all_bank_bags(impl_->host); }

void register_chat_api(lua_State* L, UiHost& host); // app/lua_ui_chat.cpp
void register_gamepad_api(lua_State* L, UiHost& host); // app/lua_ui_gamepad.cpp
void register_spellbook_api(lua_State* L, UiHost& host); // app/lua_ui_spellbook.cpp
void register_abilities_api(lua_State* L, UiHost& host); // app/lua_ui_abilities.cpp
void register_ability_book_api(lua_State* L, UiHost& host); // app/lua_ui_abilitybook.cpp
void register_inventory_manager_api(lua_State* L, UiHost& host); // app/lua_ui_inventory_manager.cpp
void register_item_transfer_api(lua_State* L, UiHost& host); // app/lua_ui_itemdisplay.cpp
void register_reward_selection_api(lua_State* L, UiHost& host); // app/lua_ui_rewards.cpp
void register_achievement_api(lua_State* L, UiHost& host); // app/lua_ui_achievements.cpp
void register_exchange_api(lua_State* L, UiHost& host);  // app/lua_ui_exchange.cpp
void register_social_api(lua_State* L, UiHost& host);    // app/lua_ui_social.cpp
void register_stats_api(lua_State* L, UiHost& host);     // app/lua_ui_stats.cpp
void register_socialedit_api(lua_State* L, UiHost& host); // app/lua_ui_socialedit.cpp
void register_luasocial_api(lua_State* L, UiHost& host);  // app/lua_ui_luasocial.cpp
void register_hotbar_api(lua_State* L, UiHost& host);     // app/lua_ui_hotbar.cpp
void register_web_api(lua_State* L, UiHost& host);        // app/lua_ui_web.cpp
void register_find_api(lua_State* L, UiHost& host);       // app/lua_ui_find.cpp
void register_server_management_api(lua_State* L, UiHost& host); // app/lua_ui_server_management.cpp
void register_compare_api(lua_State* L, UiHost& host, std::function<StockWindow*(std::string_view)> find_window); // app/lua_ui_compare.cpp
void register_task_api(lua_State* L, UiHost& host, std::vector<std::unique_ptr<StockWindow>>& windows); // app/lua_ui_progress.cpp
int CompassStrips(lua_State* L);                     // app/lua_ui_misc.cpp
int CompassMarkers(lua_State* L);                    // app/lua_ui_misc.cpp

int call_nameplate_method(lua_State* L) {
    const int args=lua_gettop(L);lua_pushvalue(L,lua_upvalueindex(1));lua_insert(L,1);lua_call(L,args,LUA_MULTRET);return lua_gettop(L);
}
// NameplateRegister([name,] callback) -> opaque handle, or nil, reason. The owner-scoped callback receives a
// current candidate frame and returns bounded local-coordinate drawing models; nil preserves stock captions.
int NameplateRegister(lua_State* L) { return call_nameplate_method(L); }
// NameplateUnregister(handle) -> bool. Only the registration's live source owner can remove it.
int NameplateUnregister(lua_State* L) { return call_nameplate_method(L); }
// NameplateMeasureText(text [, size=16 [, font=0]]) -> width/height and ink-bounds table, or nil.
// Size is 4..256; font 0 is the nameplate font and 1..6 selects a host UI font.
int NameplateMeasureText(lua_State* L) { return call_nameplate_method(L); }

void UiApi::register_functions(lua_State* L) {
    // Re-registering on the same state replaces the reserved facade before gating the fresh bindings. Any Lua
    // reference to the old facade keeps its original guarded closures; it must not acquire another set of gates.
    lua_pushnil(L);
    lua_setfield(L, -2, "UI");
    register_ui_hooks_api(L, impl_->hook_context);
    ui::lua::NameplateCanvasLuaEnv canvas;
    canvas.registry=impl_->host.nameplates;
    canvas.owner_lifetime=impl_->callback_lifetime;
    canvas.texture_valid=[](std::uintptr_t texture){return ui::texture_id_live(reinterpret_cast<void*>(texture));};
    canvas.owner=[context=impl_->hook_context](lua_State* state){
        ui::lua::NameplateCanvasLuaOwner owner;
        auto source=ui::lua::LuaUi::callback_owner(state);
        if(!source)return owner;
        constexpr auto required=plugin::kCapRead|plugin::kCapUi|plugin::kCapExtend;
        OwnedUiCallback held{std::move(*source),context,plugin::caller_caps()};
        if(!held.can_register(required)||!held.ready_for_call(required))return owner;
        owner.key=held.source.key;
        owner.valid=[held]{
            try {
                if(!held.ready(required))return false;
                return !held.source.tag.starts_with("plugin:") ||
                    (held.context.capabilities(held.source.tag)&~held.ceiling)==0;
            } catch (...) { return false; }
        };
        owner.ready=[held]{
            try { return held.ready_for_call(required); } catch (...) { return false; }
        };
        owner.invoke=[held](lua_State* callback,int args,int results){
            try {
                if(!held.ready_for_call(required))return false;
                plugin::CallerScope ceiling(held.ceiling,held.source.tag);
                return held.source.invoke&&held.source.invoke(callback,args,results);
            } catch (...) { return false; }
        };
        return owner;
    };
    ui::lua::push_nameplate_canvas(L,std::move(canvas));
    lua_getfield(L,-1,"Register");lua_pushcclosure(L,NameplateRegister,1);lua_setfield(L,-3,"NameplateRegister");
    lua_getfield(L,-1,"Unregister");lua_pushcclosure(L,NameplateUnregister,1);lua_setfield(L,-3,"NameplateUnregister");
    lua_getfield(L,-1,"MeasureText");lua_pushcclosure(L,NameplateMeasureText,1);lua_setfield(L,-3,"NameplateMeasureText");
    lua_pop(L,1);
    lua_pushinteger(L,1);lua_setfield(L,-2,"NameplateCanvasVersion");
    register_chat_api(L, impl_->host);
    register_modern_chat_tabs(L, impl_->host);
    register_gamepad_api(L, impl_->host);
    register_spellbook_api(L, impl_->host);
    register_abilities_api(L, impl_->host);
    register_ability_book_api(L, impl_->host);
    register_inventory_manager_api(L, impl_->host);
    register_item_transfer_api(L, impl_->host);
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
    register_server_management_api(L, impl_->host);
    register_compare_api(L, impl_->host, [impl = impl_.get()](std::string_view name) { return impl->window(name); });
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
    register_map_overlay_api(L, impl_->map_overlay_context);
    register_plugins_api(L, impl_->host);
    register_ui_extensions(L, impl_->extensions_context);
    register_scripts_api(L, impl_->host);
    register_pregame_api(L, impl_->host, impl_->screens);
    static const luaL_Reg fns[] = {
        {"InGame", InGame}, {"UiReady", UiReady}, {"GameState", GameState}, {"Name", Name}, {"SlotName", SlotName},
        {"ClassName", ClassName}, {"ClassNamesRevision", ClassNamesRevision},
        {"Item", Item}, {"ItemIcon", ItemIcon}, {"Money", Money}, {"Label", Label}, {"Gauge", Gauge},
        {"ClickSlot", ClickSlot}, {"HasSlot", HasSlot}, {"AltCurrencies", AltCurrencies}, {"UiTexture", UiTexture}, {"UiImage", UiImage}, {"UiInterfaceImage", UiInterfaceImage}, {"UiAnimation", UiAnimation},
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
        {"PlaySoundTest", PlaySoundTest},
        {"InterfaceList", InterfaceList}, {"InterfaceLoadBase", InterfaceLoadBase}, {"InterfaceLoadData", InterfaceLoadData}, {"InterfaceStatus", InterfaceStatus},
        {"ComboSelect", ComboSelect}, {"BagInfo", BagInfo}, {"SortItems", SortItems}, {"SellJunk", SellJunk}, {"Parcels", Parcels}, {"RetrieveParcel", RetrieveParcel}, {"ClaimAllParcels", ClaimAllParcels}, {"SendParcel", SendParcel}, {"SendParcelMoney", SendParcelMoney}, {"ItemJob", ItemJob}, {"StopItemJob", StopItemJob}, {"FindItems", FindItems},
        {"InventorySpace", InventorySpace}, {"PlaceClick", PlaceClick}, {"BagSlotItem", BagSlotItem}, {"BagSlotClick", BagSlotClick},
        // The EQ button's menu (CascadeMenu.txt) and the windows' context menus (UiHost::menus).
        {"CascadeMenus", CascadeMenus}, {"CascadeClick", CascadeClick}, {"CascadeHover", CascadeHover},
        {"CascadeClose", CascadeClose}, {"GameMenus", GameMenus}, {"GameMenuClick", GameMenuClick},
        {"GameMenuHover", GameMenuHover}, {"GameMenuClose", GameMenuClose},
        {"WindowContextMenu", WindowContextMenu}, {"TakeWindowMenu", TakeWindowMenu},
        {"TakeCloseTopWindow", TakeCloseTopWindow}, {"ContextMenus", ContextMenus},
        {"WindowShown", WindowShown},
        {"LayoutProfilesApply", LayoutProfilesApply}, {"LayoutProfilesCanUndo", LayoutProfilesCanUndo},
        {"LayoutProfilesGeneration", LayoutProfilesGeneration}, {"LayoutProfilesList", LayoutProfilesList},
        {"LayoutProfilesPreview", LayoutProfilesPreview}, {"LayoutProfilesSave", LayoutProfilesSave},
        {"LayoutProfilesUndo", LayoutProfilesUndo},
        {nullptr, nullptr}};
    lua_pushlightuserdata(L, impl_.get());
    luaL_setfuncs(L, fns, 1); // every function gets Impl as its upvalue
    // The functions that only read: the rest may change what a window's lists hold, so each drops the lists read
    // this frame (Impl::list) before it runs. A function missing here costs only a list built again.
    static constexpr std::string_view kReads[] = {
        "InGame", "UiReady", "GameState", "Name", "SlotName", "Item", "ItemIcon", "Money", "Label", "Gauge", "HasSlot",
        "AltCurrencies", "UiTexture", "UiImage", "UiInterfaceImage", "UiAnimation", "ReplaceWindow", "WindowShown", "WindowPlaced", "WindowText", "ChildShown",
        "WindowList", "ChildArt", "GemTint", "GemOverlay", "ButtonCoolDown", "CoolDownColor", "ItemPreviewImage",
        "BuffSlot", "BuffOverlay", "HotButton", "HotButtonFace", "HotButtonChecked", "HotButtonState", "HotButtonInfo",
        "GemInfo", "HoldingButton", "SlotItem", "ListInfo", "ListRow", "ChildState", "WindowTitle", "ChildGauge",
        "ChildColor", "TabPage", "ItemPreview",
        "ChildTooltip", "DebugScreens", "ChildUnder", "VoiceChat", "ChildRect", "WindowClient", "CursorAttachment",
        "DebugTree", "ComboInfo", "Option", "UiIni", "OptionChoices", "PlaySoundTest", "BagInfo", "Parcels", "FindItems",
        "InventorySpace", "BagSlotItem", "CascadeMenus", "GameMenus", "ContextMenus", "ItemJob", "InterfaceList", "InterfaceLoadBase", "InterfaceLoadData", "InterfaceStatus",
        "LayoutProfilesCanUndo", "LayoutProfilesGeneration", "LayoutProfilesList", "LayoutProfilesPreview"};
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
    // Last: the functions that act check a plugin's capabilities first (app/lua_ui_caps.h).
    gate_ui_calls(L);
    for(const char* name:{"NameplateRegister","NameplateUnregister","NameplateMeasureText","UiHookRegister","UiHookRemove","UiHookList","ExtensionRegisterAction","ExtensionRegisterSection","MapOverlayRegister"}){lua_getfield(L,-1,name);ui::lua::LuaUi::source_generation_function(L,-1);lua_pop(L,1);}
    register_ui_api(L);
}

} // namespace hailstone::app
