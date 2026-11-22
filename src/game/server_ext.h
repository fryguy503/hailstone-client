#pragma once

// Server extensions: where a server's own client (a patched eqgame, a DLL the server ships) differs from the stock
// RoF2 client. The core is a stock RoF2 / EQEmu client; it asks the connection's extension
// (net::GameConnection::server_ext()) at every point such a client changes something, and each hook's default
// here is the stock client's answer. An extension overrides only what its server's client does.
//
// The connection asks one chain of extensions (game/server_ext_chain.h): the plugins' (net::ConnectionOptions::
// plugins; compiled plugins, plugin/native_plugins.h, and Lua plugins, mq/lua_hailstone.cpp), each while it says it
// is active. With no active member the chain gives every hook's default: the stock client. A plugin's packets,
// commands, chat filter and per-frame work are the plugin interface's own callbacks (sdk/hailstone_plugin.h), not hooks
// here. docs/plugins.md.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game/tracking.h"
#include "game/follow.h"

namespace hailstone::net {
class GameConnection;
struct Item;
struct ItemSlot;
struct WhoResponse;
} // namespace hailstone::net

namespace hailstone::game {

struct SpellData;
struct Spawn;

// How a spawn shows on the game's maps when an extension shows it (ServerExtension::map_spawn).
struct MapSpawnMark {
    std::uint32_t rgb = 0xffffff;
    int layer = 0;     // the map's layer 0..3
    std::string label; // "" the dot alone
};

enum MapPolicyFlags : std::uint32_t {
    MapPolicyCustomSpawns = 1,
    MapPolicyHideStockNpcs = 2,
    MapPolicyDisableNpcFind = 4,
    MapPolicyServerNpcFindOnly = 8,
};
struct MapPolicy {
    std::uint32_t flags = 0;
    std::string name;
    std::uint64_t revision = 0; // provider content revision; the chain publishes an effective lifetime revision
    bool npc_find_enabled() const { return !(flags & MapPolicyDisableNpcFind); }
    bool npc_find_server_only() const { return npc_find_enabled() && (flags & MapPolicyServerNpcFindOnly); }
};

// The skill rules an extension changes (CharacterZoneClient::HasSkill / CSkillMgr::IsAvailable and the tracking
// check). Plain functions with a context, so a character sheet can keep a pointer to them
// (mechanics::CharacterSheet::skill_rules) past the connection: the chain's stay valid as long as the app runs
// (game/server_ext_chain.h). A null function is the stock rule; each gets `ctx`.
struct SkillRules {
    const void* ctx = nullptr;
    // The gate before the original: false = no skill, true = has it; nullopt = go on. Asked only with a non-zero
    // class mask on the sheet.
    std::optional<bool> (*gate)(const void* ctx, std::int32_t skill, std::uint32_t class_mask,
                                std::int32_t profile_skill) = nullptr;
    // The class GetAdjustedSkill then runs with for `skill`; 0 = the profile's.
    std::uint32_t (*spoof_class)(const void* ctx, std::int32_t skill, std::uint32_t class_mask) = nullptr;
    // The tracking skill check (UseSkill(53) and the window); null: game::tracking_allowed().
    bool (*tracking_allowed)(const void* ctx, std::uint8_t class_id, std::uint32_t class_mask,
                             std::uint32_t tracking_skill) = nullptr;
};

// The local character's Max_HP / Cur_HP / Max_Mana / Cur_Mana / Max_Endurance / Cur_Endurance as an extension's
// client replaces them: a value is what the function returns, whatever its arguments (the NMS patch's detours);
// nullopt = the original's formula. Kept on the character sheet (mechanics::CharacterSheet::vitals).
struct CharacterVitals {
    std::optional<std::int32_t> max_hp, cur_hp, max_mana, cur_mana, max_endurance, cur_endurance;
    bool any() const { return max_hp || cur_hp || max_mana || cur_mana || max_endurance || cur_endurance; }
};

// The item display's "Level Needed" line for a spell scroll as an extension's client writes it; nullopt for a spell
// = the stock line.
using LevelNeededLine = std::function<std::optional<std::string>(const SpellData&)>;

class ServerExtension {
public:
    virtual ~ServerExtension() = default;
    virtual std::optional<FollowProfile> follow_profile(const net::GameConnection&) const { return std::nullopt; }
    virtual std::optional<MountProfile> mount_profile(const net::GameConnection&) const { return std::nullopt; }
    virtual bool map_visible(const net::GameConnection&, const Spawn&, std::uint32_t) const { return true; }
    virtual MapPolicy map_policy(const net::GameConnection&) const { return {}; }
    // Internal provider lifetime, independent of map flags/content; changes retire source-owned interactions.
    virtual std::uint64_t lifecycle_revision(const net::GameConnection&) const { return 0; }
    virtual bool merchant_right_click(net::GameConnection&, std::uint32_t, const net::ItemSlot&,
                                      std::uint32_t, std::uint32_t) { return false; }
    virtual const char* name() const { return "stock"; }

    // ---- the connection (net/game_connection*.cpp) ----
    // Each frame in the zone: true runs the zone-in's last step again.
    virtual bool retry_zone_entry(net::GameConnection&) { return false; }
    // An original command by its handler name (game/commands.h), before the client's port; true = handled.
    virtual bool handler_command(net::GameConnection&, std::string_view, const std::string&) { return false; }
    // OP_WhoAllResponse: the lines to print in place of the stock reply; nullopt = the stock reply.
    virtual std::optional<std::vector<std::string>> who_lines(const net::GameConnection&,
                                                              const net::WhoResponse&) const {
        return std::nullopt;
    }

    // ---- classes ----
    // The class masks themselves are the core's (Hailstone's multiclass support): an extension sets them with
    // GameConnection::set_player_classes / set_character_classes, and the core shows them ("War/Clr").
    // The class the original's checks see in place of the profile's (a cast spoof); 0 = the profile's.
    virtual std::uint8_t cast_class(const net::GameConnection&) const { return 0; }
    // EQ_Character::IsSpellcaster for the local character; nullopt = the stock rule on the visible class.
    virtual std::optional<bool> is_spellcaster(const net::GameConnection&) const { return std::nullopt; }
    // EQ_Spell::GetSpellLevelNeeded(class_id); nullopt = the spell's own byte.
    virtual std::optional<std::uint8_t> spell_level_needed(const net::GameConnection&, const SpellData&,
                                                           std::uint32_t) const {
        return std::nullopt;
    }
    // CharacterZoneClient::BardCastBard: true = never a song (`raw_bard_level` the record's own Bard byte).
    virtual bool song_blocked(const net::GameConnection&, std::uint8_t) const { return false; }
    // CEverQuest::StartCasting for the local player's own cast while its spawn shows as a Bard: true = print 12205
    // "You begin casting %1." here in place of the spell's CastByMe text (stock: the Bard's text).
    virtual bool own_begin_cast_line(const net::GameConnection&, const SpellData&) const { return false; }
    // CharacterZoneClient::CanUseMemorizedSpellSlot(gem); nullopt = the stock rule.
    virtual std::optional<bool> can_use_gem(const net::GameConnection&, int) const { return std::nullopt; }
    // The spell's CARecastTimerID (+0x198) as the client holds it; stock: the file's.
    virtual std::int32_t combat_ability_timer(const net::GameConnection&, const SpellData& s) const;
    // GetDisciplineReuseTimer(id): the ready time in Unix seconds; nullopt = the stock rule (ids 0..19, else 0).
    virtual std::optional<std::uint32_t> discipline_reuse_timer(const net::GameConnection&, std::int32_t) const {
        return std::nullopt;
    }
    // Around PcZoneClient::doCombatAbility: before, a value answers without running it (false: refused); after,
    // `used` is the original's answer.
    virtual std::optional<bool> before_combat_ability(net::GameConnection&, const SpellData&) { return std::nullopt; }
    virtual void after_combat_ability(net::GameConnection&, const SpellData&, bool) {}
    // Before CharacterZoneClient::UseSkill: a value answers without running it (false: refused).
    virtual std::optional<bool> before_use_skill(net::GameConnection&, std::uint32_t) { return std::nullopt; }
    // CCombatAbilityWnd's CAW_CombatEffectLabel after its OnProcessFrame; nullopt = the stock text (the effect in
    // the discipline slot, or "No Effect").
    virtual std::optional<std::string> combat_effect_label(const net::GameConnection&) const { return std::nullopt; }
    // CharacterZoneClient::GetFocusReuseMod gives 0 for the spell: no reuse focus shortens the cool-down the client
    // shows (the server keeps that timer).
    virtual bool reuse_focus_dropped(const net::GameConnection&, const SpellData&) const { return false; }
    // The game's maps (app/lua_ui_map.cpp), which on their own show only the group: a spawn other than the player's
    // shown as the mark says; nullopt = not shown. `defaults` is the mark a plugin starts from.
    virtual std::optional<MapSpawnMark> map_spawn(const net::GameConnection&, const Spawn&,
                                                  const MapSpawnMark&) const {
        return std::nullopt;
    }
    // CharacterZoneClient::CanUseItem / CanEquipItem answer true for everything (only the server checks).
    virtual bool can_use_all_items(const net::GameConnection&) const { return false; }
    // The item links the client writes (game/item_links.h make_item_link): the wide form with 8-digit ids, else
    // the stock one.
    virtual bool wide_item_links(const net::GameConnection&) const { return false; }
    // A guild member's classes from the class value the server stores for it, as a class mask the guild windows
    // show (GameConnection::guild_member_class); 0 = a plain class id (stock).
    virtual std::uint32_t guild_member_classes(const net::GameConnection&, std::uint32_t) const { return 0; }
    // DoPassageOfTime's hide check: the class it tests (9 = Rogue: sneaking keeps Hide); stock: the profile's.
    virtual std::uint8_t hide_check_class(const net::GameConnection&, std::uint8_t class_id) const { return class_id; }
    // The AA window: abilities from level 1.
    virtual bool aa_any_level(const net::GameConnection&) const { return false; }
    // The AA window's "Train All" box sends OP_AAAction 4 (buy all ranks) in place of 3.
    virtual bool aa_train_all(const net::GameConnection&) const { return false; }
    // Class checks over the player's classes (GameConnection::player_class_mask) in place of the profile's class:
    // an augment for a worn item (ItemBase_CheckAugmentCombinedRestrictions), the research containers
    // (GetContainerTradeskill, HandleCombine) and focus (CheckFocusLimit SPA 411, CollectAAFocusEffects'
    // AltAbility_IsClassAllowed, and no class filter in GetSpellFocusPct / GetAAFocusPct).
    virtual bool augment_classes(const net::GameConnection&) const { return false; }
    virtual bool research_classes(const net::GameConnection&) const { return false; }
    virtual bool focus_classes(const net::GameConnection&) const { return false; }
    // DoAttack's Backstab check (@0x5a55c4: a piercer in the primary, else eqstr 12874) also takes a piercer in the
    // secondary, for a server whose backstab takes the better hand (the NMS server source does when multiclassing
    // is on). Stock and the Project Triune patch check the primary alone, so this is a deliberate difference.
    virtual bool backstab_either_hand(const net::GameConnection&) const { return false; }
    // An adventure recruiter's Request goes out from a player in no group or raid, for a server that takes solo
    // adventures (the Project Triune client's patch sets the adventure group size to 1). Stock EQEmu drops such a
    // request unanswered, so the client stops it with its own line.
    virtual bool solo_adventure(const net::GameConnection&) const { return false; }
    // The classes whose max HP / mana / endurance formulas the server takes the best of (the stats window's
    // comparison); stock: the profile's class.
    virtual std::vector<std::uint8_t> max_vitals_classes(const net::GameConnection&, std::uint8_t class_id) const {
        return {class_id};
    }
    // EQ_Character's HP, mana and endurance functions for the local character (CharacterVitals); stock: none.
    virtual CharacterVitals character_vitals(const net::GameConnection&) const { return {}; }

    // ---- items (net/game_connection_item_use.cpp) ----
    // CInvSlot::HandleRButtonUp, before the original: true = taken (the extension runs the original itself later,
    // GameConnection::right_click_item_original).
    virtual bool item_right_click(net::GameConnection&, const net::ItemSlot&) { return false; }

    // ---- skills (game/mechanics/abilities.cpp, game/tracking.h) ----
    // Null = the stock rules.
    virtual const SkillRules* skill_rules(const net::GameConnection&) const { return nullptr; }
    // The tracking skill check through skill_rules(), else game::tracking_allowed().
    bool tracking_allowed(const net::GameConnection& c, std::uint8_t class_id, std::uint32_t class_mask,
                          std::uint32_t tracking_skill) const;

    // ---- item display, lists and labels (app/lua_ui_*.cpp) ----
    // The item display's "Level Needed" line for a spell scroll; empty = the stock line.
    virtual LevelNeededLine level_needed_line(const net::GameConnection&) const { return {}; }
    // The merchant list's Lvl column for an item; nullopt = the stock column.
    virtual std::optional<std::string> merchant_level(const net::GameConnection&, const net::Item&) const {
        return std::nullopt;
    }
    virtual std::optional<std::string> zone_display_name(const net::GameConnection&, std::uint32_t,
                                                         std::uint32_t) const { return std::nullopt; }
    virtual std::optional<std::string> item_flags(const net::GameConnection&, const net::Item&,
                                                  std::string_view) const { return std::nullopt; }
    // Plain text: escaped once by the common item presentation builder.
    virtual std::optional<std::string> item_description_append(const net::GameConnection&,
                                                               const net::Item&) const { return std::nullopt; }
    virtual std::uint64_t item_presentation_revision(const net::GameConnection&) const { return 0; }
    virtual std::optional<std::string> loot_confirmation(const net::GameConnection&, const net::Item&,
        std::span<const std::uint32_t>, bool) const { return std::nullopt; }
    // __GetLabelFromEQ(type): may replace the stock text (in game only).
    virtual void label_text(const net::GameConnection&, int, std::string&) const {}

    // The text of a world nameplate, after its stock name and marks have been assembled.
    virtual void nameplate_text(const net::GameConnection&, const Spawn&, std::string&) const {}

    // ---- the extended target list (net/game_connection_group.cpp, app/lua_ui_player.cpp) ----
    // True = the slot is left out on this side: no row in the window, not targeted by a click, the slot keys or
    // the cycle key, not taken by /xtarget add (GameConnection::xtarget_slot_hidden). The slot keeps its number.
    virtual bool hide_xtarget_slot(const net::GameConnection&, int) const { return false; }
};

// The stock RoF2 client: every hook's default.
ServerExtension& stock_server_extension();

} // namespace hailstone::game
