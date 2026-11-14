#include "app/lua_eq_menu.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "core/game_dir.h"
#include "core/log.h"
#include "game/keymap.h"

namespace hailstone::app {

namespace {

constexpr int kBorder = 5;              // A_RoundedFrameLeft / Top / Right / Bottom
constexpr int kItemH = 24, kSepH = 6;   // CascadeCommandItemWnd / CascadeSeparatorItemWnd
constexpr int kCommandW = 180, kWideW = 190;

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::string f;
    for (char c : line) {
        if (c == '^') {
            out.push_back(f);
            f.clear();
        } else if (c != '\r' && c != '\n') {
            f += c;
        }
    }
    out.push_back(f);
    return out;
}

} // namespace

// ClientApp::key_command's table (the window keys and EQ menu entries of the Lua windows: the stock window's show /
// hide).
const char* toggle_command_window(int command) {
    using C = game::KeyCommand;
    static constexpr std::pair<C, const char*> kToggles[] = {
        {C::TOGGLE_PLAYERWIN, "PlayerWindow"},     {C::TOGGLE_TARGETWIN, "TargetWindow"},
        {C::TOGGLE_PARTYWIN, "GroupWindow"},       {C::TOGGLE_PETINFOWIN, "PetInfoWindow"},
        {C::TOGGLE_SPELLSWIN, "CastSpellWnd"},     {C::TOGGLE_BUFFWIN, "BuffWindow"},
        {C::TOGGLE_SDBUFFWIN, "ShortDurationBuffWindow"}, {C::TOGGLE_COMPASSWIN, "CompassWindow"},
        {C::TOGGLE_HOTBOX1WIN, "HotButtonWnd"},    {C::TOGGLE_HOTBOX2WIN, "HotButtonWnd2"},
        {C::TOGGLE_HOTBOX3WIN, "HotButtonWnd3"},   {C::TOGGLE_HOTBOX4WIN, "HotButtonWnd4"},
        {C::TOGGLE_HOTBOX5WIN, "HotButtonWnd5"},   {C::TOGGLE_HOTBOX6WIN, "HotButtonWnd6"},
        {C::TOGGLE_HOTBOX7WIN, "HotButtonWnd7"},   {C::TOGGLE_HOTBOX8WIN, "HotButtonWnd8"},
        {C::TOGGLE_HOTBOX9WIN, "HotButtonWnd9"},   {C::TOGGLE_HOTBOX10WIN, "HotButtonWnd10"},
        {C::TOGGLE_MAINMENUWIN, "ActionsWindow"},  {C::TOGGLE_OPTIONSWIN, "OptionsWindow"},
        {C::TOGGLE_SKILLS_WND, "SkillsWindow"},    {C::TOGGLE_ALTADVWIN, "AAWindow"},
        {C::CMD_TOGGLETASKWIN, "TaskWnd"},         {C::TOGGLE_GUILDMGMTWIN, "GuildManagementWnd"},
        {C::CMD_TOGGLE_REWARD_SELECTION_WIN, "RewardSelectionWnd"},
        {C::CMD_ACHIEVEMENT_WINDOW, "AchievementsWnd"},
        {C::TOGGLE_FELLOWSHIPWIN, "FellowshipWnd"},
        {C::TOGGLE_RAIDWIN, "RaidWindow"},         {C::TOGGLE_FRIENDSWIN, "FriendsWindow"},
        {C::TOGGLE_MAPWIN, "MapViewWnd"},
        {C::TOGGLE_MAILWIN, "MailWindow"},         {C::TOGGLE_MAILCOMPWIN, "MailCompositionWindow"},
        {C::TOGGLE_BAZAARWIN, "BazaarWnd"},        {C::TOGGLE_BAZAARSEARCHWIN, "BazaarSearchWnd"},
        {C::TOGGLE_BANDOLIER, "BandolierWnd"},     {C::CMD_EXTENDED_TARGET_WINDOW, "ExtendedTargetWnd"},
        {C::CMD_TOGGLE_TITLE_WND, "TitleWnd"},     {C::CMD_TOGGLE_BARTER_SEARCH, "BarterSearchWnd"},
        {C::TOGGLE_TRIBUTEBENEFITWIN, "TributeBenefitWnd"},
        {C::TOGGLE_LEADERSHIPWIN, "LeadershipWindow"},     {C::CMD_TOGGLE_BLOCKEDBUFFWIN, "BlockedBuffWnd"}, {C::CMD_TOGGLE_BLOCKEDPETBUFFWIN, "BlockedPetBuffWnd"},
        {C::CMD_TOGGLE_DYNAMIC_ZONE_WIN, "DynamicZoneWnd"}, {C::CMD_TOGGLE_AGGRO, "AggroMeterWnd"},
        {C::CMD_TOGGLE_AURAWND, "AuraWindow"},           {C::CMD_TOGGLE_COMBAT_ABILITY_WIN, "CombatAbilityWnd"},           {C::TOGGLE_LFGUILDWIN, "LFGuildWnd"},
        {C::TOGGLE_LFGROUPWIN, "GroupSearchWnd"},
        {C::CMD_TOGGLE_ALARM, "AlarmWnd"},
        {C::TOGGLE_HELPWIN, "HelpWindow"},         {C::TOGGLE_VIDEOMODEWIN, "VideoModesWindow"},
        {C::TOGGLE_STORYWIN, "StoryWnd"},
        {C::TOGGLE_JOURNAL, "JournalNPCWnd"},
        {C::CMD_TOGGLE_PVP_LEADERBOARD, "PvPStatsWnd"}, // the stats, whose Leaderboard button opens the board
        {C::CMD_MARKETPLACE, "MarketplaceWnd"}, // __ExecuteCmd case 132: Cmd_marketplace @0x4ed930, the toggle
        {C::CMD_REAL_ESTATE_ITEMS_WINDOW, "RealEstateItemsWnd"}, {C::CMD_REAL_ESTATE_MANAGE_WINDOW, "RealEstateManageWnd"},
        {C::CMD_REAL_ESTATE_PLOT_SEARCH_WINDOW, "RealEstatePlotSearchWnd"},
        // __ExecuteCmd cases 164 / 165 (no pack draws ZoneGuideWnd yet; ZonePathWnd refuses to show without a path)
        {C::TOGGLE_ZONE_GUIDE_WINDOW, "ZoneGuideWnd"}, {C::TOGGLE_ZONE_PATH_WINDOW, "ZonePathWnd"},
    };
    for (auto [c, screen] : kToggles)
        if (command == static_cast<int>(c)) return screen;
    if (command == kAbilityBookCommand) return "AbilityBookWnd";
    if (command == kInventoryManagerCommand) return "InventoryManagerWnd";
    return nullptr;
}

bool menu_command_offered(int command, const std::function<bool(std::string_view)>& replaces) {
    using C = game::KeyCommand;
    if (command < 0 || game::key_command_dropped(command)) return false;
    switch (static_cast<C>(command)) {
    case C::INVENTORY:
    case C::SPELLBOOK:
    case C::CMD_MERCENARIES:
    case C::CAMP:
    // (a window of the client's own when the pack has none: ClientApp::key_command's switch)
    case C::TOGGLE_GUILDMGMTWIN:
    case C::TOGGLE_RAIDWIN:
    case C::TOGGLE_FRIENDSWIN:
    case C::TOGGLE_MAINMENUWIN:
    case C::TOGGLE_SKILLS_WND:
    case C::CMD_TOGGLE_COMBAT_ABILITY_WIN:
    case C::TOGGLE_HOTBOX1WIN:
    case C::TOGGLE_OPTIONSWIN:
    case C::CMD_TOGGLE_FIND_WINDOW: // Hailstone's Find window (app/lua_eq_find.cpp), drawn by every pack
        return true;
    // Hailstone: both packs draw the aggro meter in the target window, so it has no row of its own (a server's
    // CascadeMenu.txt row goes too); /aggrometer and its key still toggle the game's AggroMeterWnd.
    case C::CMD_TOGGLE_AGGRO: return false;
    default: break;
    }
    const char* screen = toggle_command_window(command);
    return screen && replaces && replaces(screen);
}

void CascadeMenu::load(const GameDir& game, std::uint32_t expansions, std::int32_t pvp_rules,
                       const std::function<bool(int)>& offered) {
    if (loaded_) return;
    // Hailstone: a server's own CascadeMenu.txt lays the menu out; the player's game folder's does not (its rows
    // and their order change with the client version), Hailstone's layout does.
    constexpr std::string_view kFile = "uifiles/default/CascadeMenu.txt";
    auto path = game.find(kFile);
    if (path && game.from_overlay(kFile)) {
        std::ifstream in(*path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        log::info("ui: EQ menu from the server's {}", path->string());
        load_text(ss.str(), expansions, pvp_rules, offered);
        return;
    }
    load_text(hailstone_layout(), expansions, pvp_rules, offered);
}

// Hailstone's EQ menu. Its own grouping, not a client's file: the windows by what they are about, the ones a key
// opens too (inventory, spell book, options) a click away; arrange() puts it in alphabetical order. A row's first field gates it on an expansion bit as the
// original's rows for the same windows are (the extended target and search windows on bit 15, the blocked buff
// and aura windows on 10, real estate on 16); rows this client does not offer go as load_text says.
std::string_view CascadeMenu::hailstone_layout() {
    return "0^1^14^Marketplace^CMD_MARKETPLACE\n"
           "0^2\n"
           "0^3\n"
           "0^4^-1^Player Information^TOGGLE_PLAYERWIN\n"
           "0^4^-1^Skills^TOGGLE_SKILLS_WND\n"
           "0^4^-1^Alternate Advancement^TOGGLE_ALTADVWIN\n"
           "0^4^-1^Leadership^TOGGLE_LEADERSHIPWIN\n"
           "0^4^-1^Tribute^TOGGLE_TRIBUTEBENEFITWIN\n"
           "0^4^-1^Spell Gems^TOGGLE_SPELLSWIN\n"
           "0^4^-1^Spell Book^SPELLBOOK\n"
           "0^4^-1^Buffs^TOGGLE_BUFFWIN\n"
           "0^4^-1^Short Duration Buffs^TOGGLE_SDBUFFWIN\n"
           "11^4^-1^Blocked Buffs^CMD_TOGGLE_BLOCKEDBUFFWIN\n"
           "11^4^-1^Blocked Pet Buffs^CMD_TOGGLE_BLOCKEDPETBUFFWIN\n"
           "11^4^-1^Auras^CMD_TOGGLE_AURAWND\n"
           "0^4^-1^Pet^TOGGLE_PETINFOWIN\n"
           "0^4^-1^PvP Stats^CMD_TOGGLE_PVP_LEADERBOARD\n"
           "16^4^-1^Achievements^CMD_ACHIEVEMENT_WINDOW\n"
           "0^5^5^Character\n"
           "0^3\n"
           "0^4^-1^Inventory^INVENTORY\n"
           "0^4^-1^Bandolier^TOGGLE_BANDOLIER\n"
           "16^4^-1^Bazaar Search^TOGGLE_BAZAARSEARCHWIN\n"
           "16^4^-1^Barter Search^CMD_TOGGLE_BARTER_SEARCH\n"
           "0^5^3^Inventory\n"
           "0^3\n"
           "0^4^-1^Actions^TOGGLE_MAINMENUWIN\n"
           "0^4^-1^Combat Abilities^CMD_TOGGLE_COMBAT_ABILITY_WIN\n"
           "0^4^-1^Target^TOGGLE_TARGETWIN\n"
           "16^4^-1^Extended Target^CMD_EXTENDED_TARGET_WINDOW\n"
           "0^4^-1^Alarm^CMD_TOGGLE_ALARM\n"
           "0^5^8^Actions\n"
           "0^3\n"
           "0^4^-1^Hotkeys 1^TOGGLE_HOTBOX1WIN\n"
           "0^4^-1^Hotkeys 2^TOGGLE_HOTBOX2WIN\n"
           "0^4^-1^Hotkeys 3^TOGGLE_HOTBOX3WIN\n"
           "0^4^-1^Hotkeys 4^TOGGLE_HOTBOX4WIN\n"
           "0^4^-1^Hotkeys 5^TOGGLE_HOTBOX5WIN\n"
           "0^4^-1^Hotkeys 6^TOGGLE_HOTBOX6WIN\n"
           "0^4^-1^Hotkeys 7^TOGGLE_HOTBOX7WIN\n"
           "0^4^-1^Hotkeys 8^TOGGLE_HOTBOX8WIN\n"
           "0^4^-1^Hotkeys 9^TOGGLE_HOTBOX9WIN\n"
           "0^4^-1^Hotkeys 10^TOGGLE_HOTBOX10WIN\n"
           "0^5^8^Hotkeys\n"
           "0^3\n"
           "0^4^-1^Group Status^TOGGLE_PARTYWIN\n"
           "0^4^-1^Raid^TOGGLE_RAIDWIN\n"
           "0^4^-1^Guild^TOGGLE_GUILDMGMTWIN\n"
           "0^4^-1^Friends/Ignore^TOGGLE_FRIENDSWIN\n"
           "0^4^-1^Fellowship^TOGGLE_FELLOWSHIPWIN\n"
           "0^4^-1^Looking for Group^TOGGLE_LFGROUPWIN\n"
           "0^4^-1^Looking for Guild^TOGGLE_LFGUILDWIN\n"
           "0^4^-1^Mercenaries^CMD_MERCENARIES\n"
           "0^5^9^Players\n"
           "0^3\n"
           "0^4^-1^Quest Journal^CMD_TOGGLETASKWIN\n"
           "0^4^-1^NPC Journal^TOGGLE_JOURNAL\n"
           "0^4^-1^Expeditions^CMD_TOGGLE_DYNAMIC_ZONE_WIN\n"
           "0^4^-1^Adventures^TOGGLE_ADVREQUESTWIN\n"
           "0^4^-1^Rewards^CMD_TOGGLE_REWARD_SELECTION_WIN\n"
           "0^4^-1^Storyline^TOGGLE_STORYWIN\n"
           "0^5^7^Quests\n"
           "0^3\n"
           "0^4^-1^Atlas/Map^TOGGLE_MAPWIN\n"
           "0^4^-1^Compass^TOGGLE_COMPASSWIN\n"
           "0^4^-1^Find^CMD_TOGGLE_FIND_WINDOW\n"
           "0^4^-1^Zone Guide^TOGGLE_ZONE_GUIDE_WINDOW\n"
           "0^4^-1^Zone Path^TOGGLE_ZONE_PATH_WINDOW\n"
           "0^5^10^Navigation\n"
           "17^3\n"
           "17^4^-1^Items^CMD_REAL_ESTATE_ITEMS_WINDOW\n"
           "17^4^-1^Manage^CMD_REAL_ESTATE_MANAGE_WINDOW\n"
           "17^4^-1^Trophies^CMD_REAL_ESTATE_TROPHIES_WINDOW\n"
           "17^4^-1^Plot Search^CMD_REAL_ESTATE_PLOT_SEARCH_WINDOW\n"
           "17^4^-1^Leave Real Estate^CMD_REAL_ESTATE_LEAVE\n"
           "17^4^-1^Help^CMD_REAL_ESTATE_HELP_WINDOW\n"
           "17^5^17^Real Estate\n"
           "0^2\n"
           "0^1^4^Mail^TOGGLE_MAILWIN\n"
           "0^1^11^Claims^CMD_TOGGLE_CLAIM_WIN\n"
           "0^1^2^Options^TOGGLE_OPTIONSWIN\n"
           "0^2\n"
           "0^1^1^Help^TOGGLE_HELPWIN\n"
           "0^1^0^Camp & Quit^CAMP\n";
}

void CascadeMenu::load_text(std::string_view file, std::uint32_t expansions, std::int32_t pvp_rules,
                            const std::function<bool(int)>& offered) {
    loaded_ = true;
    sorted_ = file == hailstone_layout();
    menus_.assign(1, {});
    std::vector<std::string> left_out;
    std::istringstream in{std::string(file)};
    std::string line;
    int current = -1; // the list kind-4 lines go into (a kind-3 line starts it)
    while (std::getline(in, line)) {
        auto f = split(line);
        if (f.size() < 2 || f[0].empty()) continue;
        int flag = std::atoi(f[0].c_str()), kind = std::atoi(f[1].c_str());
        int icon = f.size() > 2 ? std::atoi(f[2].c_str()) : -1;
        std::string text = f.size() > 3 ? f[3] : std::string();
        std::string cmd = f.size() > 4 ? f[4] : std::string();
        if (flag != 0 && !(expansions & (1u << (flag - 1)))) continue;
        int command = cmd.empty() ? -1 : game::command_by_name(cmd).value_or(-1);
        if (command == 0x16f && pvp_rules == 0) continue; // __CreateCascadeMenuItems: 0xdcee70
        if (command == 0x17f) continue;                    // 0xddfd42 (not sent by EQEmu)
        // Hailstone: a row whose command this client does not run (a later client's, or a window it does not have)
        if ((kind == 1 || kind == 4) && (command < 0 || (offered && !offered(command)))) {
            left_out.push_back(text);
            continue;
        }
        // Hailstone: the Ability Book after Combat Abilities and the Advanced Inventory after Inventory, with their
        // icons, when the UI pack draws them.
        auto add_row = [&](std::vector<Item>& items) {
            items.push_back({1, icon, text, command, -1});
            if (command == static_cast<int>(game::KeyCommand::CMD_TOGGLE_COMBAT_ABILITY_WIN) &&
                (!offered || offered(kAbilityBookCommand)))
                items.push_back({1, icon, "Ability Book", kAbilityBookCommand, -1});
            if (command == static_cast<int>(game::KeyCommand::INVENTORY) && (!offered || offered(kInventoryManagerCommand)))
                items.push_back({1, icon, "Advanced Inventory", kInventoryManagerCommand, -1});
        };
        switch (kind) {
        case 1: add_row(menus_[0].items); break;
        case 2: menus_[0].items.push_back({2, -1, {}, -1, -1}); break;
        case 3:
            menus_.push_back({});
            current = static_cast<int>(menus_.size()) - 1;
            break;
        case 4:
            if (current > 0) add_row(menus_[static_cast<std::size_t>(current)].items);
            break;
        case 5:
            // Hailstone: a submenu left with no rows goes
            if (current > 0 && menus_[static_cast<std::size_t>(current)].items.empty()) left_out.push_back(text);
            else menus_[0].items.push_back({5, icon, text, -1, current});
            current = -1;
            break;
        default: break;
        }
    }
    // the separators that no longer stand between two rows
    auto& top = menus_[0].items;
    std::vector<Item> kept;
    for (const Item& it : top) {
        if (it.kind == 2 && (kept.empty() || kept.back().kind == 2)) continue;
        kept.push_back(it);
    }
    while (!kept.empty() && kept.back().kind == 2) kept.pop_back();
    top = std::move(kept);
    // Hailstone: the bug report window's row (Help's icon), before Help, else before Camp & Quit, else last
    auto at = [&](game::KeyCommand c) {
        return std::find_if(top.begin(), top.end(), [&](const Item& it) { return it.command == static_cast<int>(c); });
    };
    auto where = at(game::KeyCommand::TOGGLE_HELPWIN);
    if (where == top.end()) where = at(game::KeyCommand::CAMP);
    const int icon = where != top.end() ? where->icon : -1;
    where = top.insert(where, {1, icon, "Report a Bug", kMenuReportBug, -1});
    where = top.insert(where, {1, icon, "Web Browser", kMenuWebBrowser, -1});
    top.insert(where, {1, icon, "Lua Scripts", kMenuScripts, -1});
    plugin_menu_ = -1;
    apply_plugin_entries();
    if (!left_out.empty()) {
        std::string list;
        for (const std::string& t : left_out) list += (list.empty() ? "" : ", ") + t;
        log::info("ui: EQ menu: {} rows left out (not offered by this client): {}", left_out.size(), list);
    }
}

std::vector<std::string> CascadeMenu::row_texts(int menu) const {
    std::vector<std::string> out;
    if (menu < 0 || menu >= static_cast<int>(menus_.size())) return out;
    for (const Item& it : menus_[static_cast<std::size_t>(menu)].items)
        out.push_back(it.kind == 2 ? std::string("-") : it.kind == 5 ? it.text + " >" : it.text);
    return out;
}

std::array<int, 2> CascadeMenu::size(int menu) const {
    int w = 0, h = 0;
    for (const Item& it : menus_[static_cast<std::size_t>(menu)].items) {
        w = std::max(w, it.kind == 1 ? kCommandW : kWideW);
        h += it.kind == 2 ? kSepH : kItemH;
    }
    return {w + 2 * kBorder, h + 2 * kBorder};
}

void CascadeMenu::show(int menu, const std::array<int, 4>& parent, bool parent_flipped, int sw, int sh) {
    // parent: x0, y0, x1, y1 (CascadeMenuMgr_ShowSubmenu's rectangle)
    auto [w, h] = size(menu);
    Open o;
    o.menu = menu;
    o.w = w;
    o.h = h;
    bool flip = parent_flipped;
    int x = flip ? parent[0] - w : parent[2];
    if (x < 0) {
        x = parent[2];
        flip = false;
    } else if (sw < w + x) {
        flip = true;
        x = parent[0] - w;
    }
    int y = parent[1];
    if (sh < h + y) y = sh - h;
    if (y < 0) y = 0;
    o.x = x;
    o.y = y;
    o.flipped = flip;
    open_.push_back(o);
}

void CascadeMenu::toggle(const std::array<int, 4>& owner, int sw, int sh) {
    if (open()) {
        close();
        return;
    }
    if (menus_.empty() || menus_[0].items.empty()) return;
    show(0, {owner[0], owner[1], owner[0] + owner[2], owner[1] + owner[3]}, false, sw, sh);
}

std::vector<CascadeMenuView> CascadeMenu::views(ui::UiTextures* textures) const {
    std::vector<CascadeMenuView> out;
    const ui::UiTextures::Texture* icons = textures ? textures->file("menuicons01.tga") : nullptr;
    for (const Open& o : open_) {
        CascadeMenuView v{static_cast<float>(o.x), static_cast<float>(o.y), static_cast<float>(o.w), static_cast<float>(o.h), {}};
        float y = static_cast<float>(o.y + kBorder);
        for (const Item& it : menus_[static_cast<std::size_t>(o.menu)].items) {
            CascadeItemView iv;
            float ih = static_cast<float>(it.kind == 2 ? kSepH : kItemH);
            float iw = static_cast<float>(it.kind == 1 ? kCommandW : kWideW);
            iv.rect = {static_cast<float>(o.x + kBorder), y, iw, ih};
            iv.text = it.text;
            if (it.kind == 2) {
                iv.kind = "separator";
                // CSepIW_Button: A_CascadeMenuSeparator, 2 pixels in from the top and the bottom
                if (textures) iv.background = textures->animation("A_CascadeMenuSeparator");
                iv.rect = {iv.rect[0], y + 2, iw, ih - 4};
            } else {
                iv.kind = it.kind == 5 ? "submenu" : "command";
                if (icons && it.icon >= 0) { // A_MenuIcons: 24 x 24 cells on menuicons01.tga, down each column first
                    int per_col = icons->height / 24;
                    if (per_col > 0) {
                        float u = static_cast<float>((it.icon / per_col) * 24), vv = static_cast<float>((it.icon % per_col) * 24);
                        float w = static_cast<float>(icons->width), h = static_cast<float>(icons->height);
                        iv.icon = ui::UiTextures::Cell{icons->id, u / w, vv / h, (u + 24) / w, (vv + 24) / h};
                    }
                }
                if (it.kind == 5 && textures) { // CSubIW_Arrow: A_CascadeSubmenuIndicator at 180, 6 (10 x 11)
                    iv.arrow = textures->animation("A_CascadeSubmenuIndicator");
                    iv.arrow_rect = {iv.rect[0] + 180, y + 6, 10, 11};
                }
            }
            v.items.push_back(std::move(iv));
            y += ih;
        }
        out.push_back(std::move(v));
    }
    return out;
}

void CascadeMenu::hover(int menu, int item, int sw, int sh) {
    if (menu < 0 || menu >= static_cast<int>(open_.size())) return;
    const Open o = open_[static_cast<std::size_t>(menu)];
    const auto& items = menus_[static_cast<std::size_t>(o.menu)].items;
    if (item < 0 || item >= static_cast<int>(items.size())) return;
    open_.resize(static_cast<std::size_t>(menu) + 1); // this menu's submenus close
    const Item& it = items[static_cast<std::size_t>(item)];
    if (it.kind != 5 || it.submenu < 0 || menus_[static_cast<std::size_t>(it.submenu)].items.empty()) return;
    int y = o.y + kBorder;
    for (int k = 0; k < item; ++k) y += items[static_cast<std::size_t>(k)].kind == 2 ? kSepH : kItemH;
    int x0 = o.x + kBorder;
    show(it.submenu, {x0, y, x0 + kWideW, y + kItemH}, o.flipped, sw, sh);
}

void CascadeMenu::set_plugin_entries(std::vector<std::string> labels) {
    if (labels == plugin_labels_) return;
    plugin_labels_ = std::move(labels);
    if (loaded_) apply_plugin_entries();
}

void CascadeMenu::apply_plugin_entries() {
    if (menus_.empty()) return;
    close(); // the open menus' indexes may change
    auto& top = menus_[0].items;
    if (plugin_menu_ >= 0) {
        std::erase_if(top, [&](const Item& it) { return it.kind == 5 && it.submenu == plugin_menu_; });
        menus_.erase(menus_.begin() + plugin_menu_);
        plugin_menu_ = -1;
    }
    if (plugin_labels_.empty()) {
        if (sorted_) arrange();
        return;
    }
    Menu m;
    for (std::size_t i = 0; i < plugin_labels_.size(); ++i)
        m.items.push_back({1, -1, plugin_labels_[i], kMenuPluginFirst + static_cast<int>(i), -1});
    plugin_menu_ = static_cast<int>(menus_.size());
    menus_.push_back(std::move(m));
    auto& rows = menus_[0].items; // (push_back may have moved menus_)
    // Before Hailstone's own rows (Lua Scripts, Web Browser, Report a Bug).
    auto where = std::find_if(rows.begin(), rows.end(), [](const Item& it) {
        return it.command == kMenuScripts || it.command == kMenuWebBrowser || it.command == kMenuReportBug;
    });
    rows.insert(where, {5, -1, "Plugins", -1, plugin_menu_});
    if (sorted_) arrange();
}

// Hailstone's layout in order: each submenu's rows alphabetical (the plugins' in their own order); the top menu's
// submenus first, then its other rows, each alphabetical, then Help, Report a Bug and Camp & Quit at the bottom.
void CascadeMenu::arrange() {
    auto key = [](const std::string& t) {
        std::string k;
        for (char c : t) k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return k;
    };
    auto by_text = [&](const Item& a, const Item& b) { return key(a.text) < key(b.text); };
    for (std::size_t m = 1; m < menus_.size(); ++m) {
        if (static_cast<int>(m) == plugin_menu_) continue;
        auto& items = menus_[m].items;
        std::erase_if(items, [](const Item& it) { return it.kind == 2; });
        std::stable_sort(items.begin(), items.end(), by_text);
    }
    auto bottom_rank = [](const Item& it) {
        if (it.kind != 1) return -1;
        if (it.command == static_cast<int>(game::KeyCommand::TOGGLE_HELPWIN)) return 0;
        if (it.command == kMenuReportBug) return 1;
        if (it.command == static_cast<int>(game::KeyCommand::CAMP)) return 2;
        return -1;
    };
    std::vector<Item> submenus, rows, bottom;
    for (Item& it : menus_[0].items) {
        if (it.kind == 2) continue;
        if (it.kind == 5) submenus.push_back(std::move(it));
        else if (bottom_rank(it) >= 0) bottom.push_back(std::move(it));
        else rows.push_back(std::move(it));
    }
    std::stable_sort(submenus.begin(), submenus.end(), by_text);
    std::stable_sort(rows.begin(), rows.end(), by_text);
    std::stable_sort(bottom.begin(), bottom.end(),
                     [&](const Item& a, const Item& b) { return bottom_rank(a) < bottom_rank(b); });
    auto& top = menus_[0].items;
    top.clear();
    for (auto* group : {&submenus, &rows, &bottom}) {
        if (group->empty()) continue;
        if (!top.empty()) top.push_back({2, -1, {}, -1, -1});
        for (Item& it : *group) top.push_back(std::move(it));
    }
}

std::optional<int> CascadeMenu::click(int menu, int item) {
    if (menu < 0 || menu >= static_cast<int>(open_.size())) return std::nullopt;
    const auto& items = menus_[static_cast<std::size_t>(open_[static_cast<std::size_t>(menu)].menu)].items;
    if (item < 0 || item >= static_cast<int>(items.size())) return std::nullopt;
    const Item& it = items[static_cast<std::size_t>(item)];
    if (it.kind != 1 || it.command < 0) return std::nullopt;
    close();
    return it.command;
}

} // namespace hailstone::app
