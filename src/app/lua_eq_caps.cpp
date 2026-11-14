// What a plugin's code in the UI needs to call each eq function (app/lua_eq_caps.h, docs/plugins.md "What plugin
// windows may do"). The groups follow plugin/policy.h's capabilities:
//   0             reads the game or the UI's state, draws, or changes only how the UI looks (a hover, a scroll)
//   kCapCommands  changes windows and options as the slash commands of that group do: shows or hides a window,
//                 an option, a chat window's filters, the hot bars' layout, a spell set's list
//   kCapActions   acts in the game: a stock window's buttons and lists (which run its handler, whatever it does:
//                 buy, trade, destroy, accept), items, the cursor, spells, abilities, hot buttons, parcels, the menus'
//                 commands
//   kCapTarget    changes the target (the Find window's row: eq.FindNpcTarget)
//   kCapMove      moves the player (the Find window's Walk there: eq.FindNpcWalk, a /nav walk)
// eq.ChatSubmit's line goes through the command filter (mq/commands.h), which holds it to the caller's capabilities
// as any command a plugin sends: it needs nothing here. The pre-game screens, the scripts' and the
// plugins' functions and the web's check plugin code themselves (app/lua_eq_pregame.cpp, lua_eq_scripts.cpp,
// lua_eq_plugins.cpp, lua_eq_web.cpp).
#include "app/lua_eq_caps.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <lua.hpp>

#include "core/log.h"
#include "plugin/policy.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

namespace {

using plugin::kCapActions;
using plugin::kCapCommands;
using plugin::kCapMove;
using plugin::kCapTarget;

struct Entry {
    std::string_view name;
    std::uint32_t caps;
};

// Every eq function, by name (a sub-table's as "<table>.<name>").
constexpr Entry kCalls[] = {
    // Reading, drawing, a window's place, size, hover, menus opened (a menu's row is GameMenuClick or CascadeClick)
    {"InGame", 0}, {"UiReady", 0}, {"GameState", 0}, {"Name", 0}, {"SlotName", 0}, {"Item", 0}, {"ItemIcon", 0},
    {"Money", 0}, {"Label", 0}, {"Gauge", 0}, {"HasSlot", 0}, {"ItemJob", 0}, {"AltCurrencies", 0}, {"UiTexture", 0}, {"UiAnimation", 0},
    {"ReplaceWindow", 0}, {"WindowShown", 0}, {"WindowPlaced", 0}, {"WindowText", 0}, {"WindowTitle", 0},
    {"WindowList", 0}, {"WindowClient", 0}, {"SizeWindow", 0}, {"ChildShown", 0}, {"ChildArt", 0}, {"ChildState", 0},
    {"ChildGauge", 0}, {"ChildColor", 0}, {"ChildTooltip", 0}, {"ChildUnder", 0}, {"ChildRect", 0}, {"TabPage", 0},
    {"ComboInfo", 0}, {"ListInfo", 0}, {"ListRow", 0}, {"Option", 0}, {"OptionChoices", 0}, {"UiIni", 0},
    {"GemTint", 0}, {"GemOverlay", 0}, {"GemInfo", 0}, {"ButtonCoolDown", 0}, {"CoolDownColor", 0},
    {"ItemPreview", 0}, {"ItemPreviewImage", 0}, {"ItemPreviewControl", 0}, {"BuffSlot", 0}, {"BuffOverlay", 0},
    {"HotButton", 0}, {"HotButtonFace", 0}, {"HotButtonChecked", 0}, {"HotButtonState", 0}, {"HotButtonInfo", 0},
    {"HoldingButton", 0}, {"CursorAttachment", 0}, {"SlotItem", 0}, {"BagInfo", 0}, {"BagSlotItem", 0},
    {"FindItems", 0}, {"AllItems", 0}, {"ItemsRevision", 0}, {"BankSeen", 0}, {"ItemKept", 0}, {"ItemStats", 0}, {"TakeItemSearch", 0}, {"InventorySpace", 0}, {"Parcels", 0}, {"DebugScreens", 0}, {"DebugTree", 0}, {"VoiceChat", 0},
    {"CascadeMenus", 0}, {"CascadeHover", 0}, {"CascadeClose", 0}, {"GameMenus", 0}, {"GameMenuHover", 0},
    {"GameMenuClose", 0}, {"ContextMenus", 0}, {"WindowContextMenu", 0}, {"TakeWindowMenu", 0}, {"TakeCloseTopWindow", 0}, {"CompassStrips", 0},
    {"CompassMarkers", 0}, {"AbilityBook", 0}, {"AbilityBookState", 0}, {"Disciplines", 0}, {"UsableSkills", 0},
    {"BankItem", 0}, {"CursorCoin", 0}, {"TradeState", 0}, {"IconSelectionIcons", 0}, {"IconSelectionIcon", 0},
    {"HotBarMenu", 0}, {"ChatWindows", 0}, {"ChatText", 0}, {"ChatHistory", 0}, {"ChatInfo", 0}, {"ChatFilters", 0},
    {"ChatFilterGroups", 0},
    {"ChatDefaultChannels", 0}, {"ChatTellWindows", 0}, {"ChatHitModes", 0}, {"ChatMenu", 0}, {"CharacterStats", 0},
    {"WhoResults", 0}, {"SpellBook", 0}, {"SpellBookState", 0}, {"SpellIcon", 0}, {"SpellDetails", 0},
    {"DisplaySpellId", 0}, {"DisplayItemData", 0},
    {"RewardSelection", 0},
    {"Achievements", 0}, {"Achievement", 0},
    {"AchievementOpen", kCapCommands}, {"AchievementClearCompare", kCapCommands},
    {"AchievementRefresh", kCapActions}, {"AchievementCompare", kCapActions},
    {"AchievementReward", kCapActions}, {"AchievementLink", plugin::kCapChat},
    {"SpellSets", 0}, {"DrawMap", 0}, {"MapZoom", 0}, {"MapMouseDown", 0}, {"MapMouseUp", 0}, {"MapPanReset", 0},
    {"DrawMinimap", 0}, {"DrawZoneMap", 0}, {"DrawAtlas", 0}, {"AtlasIndex", 0}, {"AtlasPageData", 0}, {"AtlasTexture", 0}, {"ZoneLinks", 0}, {"ZoneRoute", 0},
    {"ZoneExits", 0}, {"ShowZoneMap", 0}, {"MapZoneShown", 0}, {"FindPathShown", 0}, {"ZoneName", 0}, {"GameTime", 0},
    {"FindNpcs", 0}, {"FindCategories", 0}, {"CompareItem", 0}, {"CompareSlotItem", 0}, {"CompareBagSlotItem", 0},
    {"LuaSocialRules", 0}, {"LuaSocial", 0}, {"LuaSocialFace", 0}, {"LuaSocialExport", 0},
    {"LuaSocialImport", 0}, {"LuaComplete", 0}, {"LuaSocialEditRequest", 0}, {"FreeSocial", 0}, {"SocialColors", 0},
    {"SocialEditorSocial", 0}, {"SocialLine", 0}, {"SlashCommands", 0}, {"CurrentSocial", 0},
    {"gamepad.connected", 0}, {"gamepad.type", 0}, {"gamepad.down", 0}, {"gamepad.pressed", 0},
    {"gamepad.released", 0}, {"gamepad.axis", 0}, {"gamepad.mode", 0}, {"gamepad.cross_set", 0},
    {"gamepad.cross_slot", 0}, {"gamepad.glyph", 0}, {"gamepad.binding", 0}, {"gamepad.setting", 0},
    {"gamepad.rumble", 0}, {"gamepad.request_focus", 0}, {"gamepad.takes_b", 0},
    // These check a plugin's code themselves (the file's head)
    {"Plugin", 0}, {"PluginOptions", 0}, {"SetPluginOption", 0}, {"PluginMenu", 0}, {"PluginMenuClick", 0},
    {"PluginConsent", 0}, {"AnswerPluginConsent", 0}, {"Scripts", 0}, {"ScriptRun", 0}, {"ScriptStop", 0},
    {"ScriptPause", 0}, {"SetScriptAutorun", 0}, {"LoginScreen", 0}, {"LoginSet", 0}, {"LoginDo", 0},
    {"ServerSelect", 0}, {"ServerSelectDo", 0}, {"CharacterSelect", 0}, {"CharacterSelectDo", 0},
    {"CharacterCreate", 0}, {"CharacterCreateHelp", 0}, {"CharacterCreateDo", 0}, {"WebAvailable", 0}, {"WebOpen", 0},
    {"WebTabs", 0}, {"WebState", 0}, {"WebNavigate", 0}, {"WebNav", 0}, {"WebClose", 0}, {"WebView", 0},
    {"WebText", 0}, {"WebEval", 0}, {"WebPost", 0}, {"WebMessages", 0}, {"WebOpenExternal", 0},
    // The line goes through the command filter, which holds it to the caller's capabilities (plain text: chat)
    {"ChatSubmit", 0}, {"ChatTakeFocus", 0},
    // The chat input's reply key (the input's new text; the reply index steps) and the page keys' scroll
    {"ChatCycleReply", 0}, {"ChatPage", 0},
    // Windows, options, the chat windows' setup, lists of the player's own (spell sets, the autoskill list)
    {"ShowWindow", kCapCommands}, {"SetOption", kCapCommands}, {"SetUiIni", kCapCommands},
    {"ChatLink", kCapCommands},
    {"ChatActivate", kCapCommands}, {"ChatClear", kCapCommands}, {"ChatNewWindow", kCapCommands},
    {"ChatCloseWindow", kCapCommands}, {"ChatNewTab", kCapCommands}, {"ChatCloseTab", kCapCommands},
    {"ChatSelectTab", kCapCommands}, {"ChatSetTitle", kCapCommands}, {"ChatSetFilter", kCapCommands},
    {"ChatSetFont", kCapCommands}, {"ChatSetDefault", kCapCommands}, {"ChatSetAlwaysHere", kCapCommands}, {"ChatSetTellWindows", kCapCommands},
    {"ChatSetHitMode", kCapCommands}, {"AbilityBookFavorite", kCapCommands}, {"AbilityBookAutoskill", kCapCommands},
    {"AbilityBookAutoskillMove", kCapCommands}, {"AbilityBookAutoskillRule", kCapCommands},
    {"HotBarSetting", kCapCommands}, {"ShowSpell", kCapCommands}, {"SaveSpellSet", kCapCommands},
    {"DeleteSpellSet", kCapCommands}, {"EditSpellSet", kCapCommands}, {"EndFind", kCapCommands},
    {"FindMapLocation", kCapCommands}, {"MapRightClick", kCapCommands}, {"MapAutoZ", kCapCommands}, {"ZoneExitWalk", kCapMove},
    {"FindNpcTarget", kCapTarget}, {"FindNpcPath", kCapCommands}, {"FindOpen", kCapCommands}, {"FindNpcWalk", kCapMove},
    {"StopLuaSocial", kCapCommands}, {"OpenLuaSocialEditor", kCapCommands}, {"OpenSocialEditor", kCapCommands},
    {"gamepad.set_mode", kCapCommands}, {"gamepad.set_binding", kCapCommands},
    {"gamepad.set_cross_slot", kCapCommands}, {"gamepad.set_setting", kCapCommands},
    {"RewardSelectionClose", kCapCommands},
    // Acting in the game
    {"RewardSelectionClaim", kCapActions}, {"RewardSelectionInspect", kCapActions},
    {"RestorePendingRewards", kCapActions},
    {"ClickSlot", kCapActions}, {"ClickChild", kCapActions}, {"ClickChildAt", kCapActions},
    {"WindowButton", kCapActions}, {"StmlLink", kCapActions}, {"SetWindowText", kCapActions},
    {"ListClick", kCapActions}, {"ListHeaderClick", kCapActions}, {"ComboSelect", kCapActions},
    {"SetSlider", kCapActions}, {"SetTabPage", kCapActions}, {"CascadeClick", kCapActions},
    {"GameMenuClick", kCapActions}, {"ClickHotButton", kCapActions}, {"PickUp", kCapActions},
    {"PressButtonFrom", kCapActions}, {"ClearCursorButton", kCapActions}, {"SpellGemClick", kCapActions},
    {"BagSlotClick", kCapActions}, {"BankSlotClick", kCapActions}, {"PlaceClick", kCapActions},
    {"SortItems", kCapActions}, {"SetItemKept", kCapActions}, {"SetItemsKept", kCapActions}, {"SellJunk", kCapActions}, {"BulkItems", kCapActions}, {"StopItemJob", kCapActions},
    {"SendParcel", kCapActions}, {"SendParcelMoney", kCapActions}, {"RetrieveParcel", kCapActions},
    {"ClaimAllParcels", kCapActions}, {"AbilityBookUse", kCapActions}, {"AbilityBookAutoskillEnable", kCapActions},
    {"UseSkill", kCapActions}, {"SpellBookClick", kCapActions}, {"SpellBookMemorize", kCapActions},
    {"SpellBookSwap", kCapActions}, {"MemorizeSpellSet", kCapActions}, {"SocialToCursor", kCapActions},
    {"PressLuaSocial", kCapActions}, {"PressSocial", kCapActions}, {"StopSocial", kCapActions},
    {"gamepad.drop_cursor", kCapActions},
    // A social's Lua is code the player's key press runs later; a dry run runs the code it is given, with the server's
    // Lua social capabilities
    {"SetLuaSocial", kCapCommands | kCapActions}, {"LuaSocialDryRun", kCapCommands | kCapActions},
};

// A plugin's code called a function its capabilities do not cover: it did nothing (said once per caller and name).
void refused(std::string_view name, std::uint32_t need) {
    static std::set<std::string> said;
    std::string key = std::string(ui::lua::LuaUi::caller_tag()) + " " + std::string(name);
    if (said.size() < 1000 && said.insert(key).second)
        log::warn("ui: {}: eq.{} needs the '{}' permission; it did nothing", ui::lua::LuaUi::caller_tag(), name,
                  plugin::caps_text(need));
}

// The gate: upvalue 1 the real function, 2 the capabilities it needs, 3 its name.
int gated(lua_State* L) {
    const auto need = static_cast<std::uint32_t>(lua_tointeger(L, lua_upvalueindex(2)));
    if (!ui::lua::LuaUi::caller_tag().empty() && (plugin::caller_caps() & need) != need) {
        refused(lua_tostring(L, lua_upvalueindex(3)), need);
        lua_pushnil(L);
        return 1;
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

// Gates the functions of the table at `t`; names are `prefix` + key.
void gate_table(lua_State* L, int t, const std::string& prefix, int depth) {
    t = lua_absindex(L, t);
    std::vector<std::string> keys;
    lua_pushnil(L);
    while (lua_next(L, t)) {
        if (lua_type(L, -2) == LUA_TSTRING && (lua_isfunction(L, -1) || (depth == 0 && lua_istable(L, -1))))
            keys.emplace_back(lua_tostring(L, -2));
        lua_pop(L, 1);
    }
    for (const std::string& key : keys) {
        lua_getfield(L, t, key.c_str());
        if (lua_istable(L, -1)) {
            gate_table(L, -1, prefix + key + ".", depth + 1);
            lua_pop(L, 1);
            continue;
        }
        const std::string name = prefix + key;
        const std::uint32_t need = eq_call_caps(name).value_or(kCapActions);
        if (need == 0) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushinteger(L, need);
        lua_pushstring(L, name.c_str());
        lua_pushcclosure(L, &gated, 3);
        lua_setfield(L, t, key.c_str());
    }
}

} // namespace

std::optional<std::uint32_t> eq_call_caps(std::string_view name) {
    static const std::map<std::string_view, std::uint32_t> by_name = [] {
        std::map<std::string_view, std::uint32_t> m;
        for (const Entry& e : kCalls) m.emplace(e.name, e.caps);
        return m;
    }();
    auto it = by_name.find(name);
    if (it == by_name.end()) return std::nullopt;
    return it->second;
}

void gate_eq_calls(lua_State* L) { gate_table(L, -1, "", 0); }

} // namespace hailstone::app
