#pragma once

// The eqgame side of mouse picking and of what a left click on a spawn or a
// door does. The ray and the
// actor tests (EQGraphicsDX9.dll) are game/picking.h.
//
// Checked against the original's vectors: CDisplay_PickActorAtPoint,
// CEverQuest_LeftClickedOnPlayer and CEverQuest_ClickedSwitch.

#include <cstdint>
#include <string>
#include <vector>

namespace hailstone::game {

// ---------------------------------------------------------------- masks

// An actor's collision restriction mask (actor vtable +0x50).
inline constexpr std::uint32_t kPickMaskPickable = 2;     // every spawn; actors from CDisplay::CreateActor
inline constexpr std::uint32_t kPickMaskNpc = 4;
inline constexpr std::uint32_t kPickMaskClickThrough = 8; // "unclickable"
// CDisplay::GetClickedActor @0x48b6b0 (clicks) and TargetManager_UpdateHoverTarget @0x5c6180 (hover).
inline constexpr std::uint32_t kClickRequired = 2, kClickSkip = 8;
inline constexpr std::uint32_t kHoverRequired = 2, kHoverSkip = 0;

// The spawn fields unk_BuildActorInstance reads.
struct MaskSpawn {
    std::uint8_t type = 0;        // +0x125: 0 player, 1 NPC, 2+ corpse
    bool click_through = false;   // +0x162 bClickThrough (spawn bitfield bit 31)
    bool mercenary = false;       // +0x208
    bool owner_is_player = false; // +0x38c names a spawn whose +0x125 is 0
};
// unk_BuildActorInstance: NPCs 6, every other spawn 2; | 8 for bClickThrough;
// a mercenary with /clickthrough bit 2 (4) gets | 8; otherwise an NPC owned by
// a player gets | 8 with /clickthrough bit 1 (2) and loses bit 8 without it.
// `click_through_option` is the /clickthrough mask (0xddfce4,
// Options/ClickThroughMask).
std::uint32_t spawn_pick_mask(const MaskSpawn& s, std::uint8_t click_through_option);

// ---------------------------------------------------------------- the pick wrapper

// 0x48b5c0 (CDisplay_PickActorAtPoint): what goes to the DLL.
struct PickCallInput {
    std::int32_t world_state = 1;      // CDisplay +0x2d04: the DLL is called only when it is 1
    std::int32_t camera_type = 0;      // 0xd1fd9c: 0 is first person
    std::uint8_t click_through = 0;    // /clickthrough: bit 0 excludes the view actor in every camera
    bool have_local_player = true;
    bool view_actor_is_local = true;   // the view actor is the local player's actor (+0x101c)
    bool view_actor_nonnull = true;
    bool have_mount = false;           // 0x58cf90: the local player's mount
    bool mount_has_actor = false;
    std::int32_t x = 0, y = 0;
    std::int32_t screen_left = 0, screen_top = 0; // 0xddf620 / 0xddf624
};
struct PickCall {
    bool call_dll = false;
    bool exclude_view = false;  // the view actor is passed as excluded
    bool exclude_mount = false; // and its mount's actor
    int mount_lookups = 0;      // 0x58cf90 calls (0..3)
    std::int32_t x = 0, y = 0;  // the point relative to the screen rectangle
};
PickCall pick_call(const PickCallInput& in);
// The wrapper's result: the DLL's actor only when it returned 0.
inline bool pick_call_found(std::int32_t dll_result, bool actor_written) { return dll_result == 0 && actor_written; }

// ---------------------------------------------------------------- left click on a spawn

// CEverQuest::LeftClickedOnPlayer @0x536310: inputs the original reads.
struct LeftClickSpawn {
    bool is_local_player = false;
    std::uint8_t type = 0;        // +0x125
    std::uint8_t class_id = 0;    // +0xeb8
    std::uint8_t eb9 = 0;         // +0xeb9 (0, 1 or other: one of three strings)
    std::int32_t race = 0;        // virtual +0x88
    bool targetable = true;       // PlayerBase::IsTargetable
    bool virtual_58 = false;      // spawn virtual +0x58 (the name is left out of the line)
    bool guildmaster_for_my_class = false; // PcClient_IsGuildmasterForMyClass
};
struct LeftClickInput {
    bool character_list_open = false; // pinstCCharacterListWnd: nothing happens
    bool click_lines = true;          // 0xde0d52, [Defaults] LeftClickTargetHelp (default 1)
    bool has_old_target = false;
    bool old_target_is_clicked = false;
    LeftClickSpawn spawn;
    bool cursor_holds_something = false; // an item on the cursor (slot 0x21) or money (CDisplay +0x11)
};
// The calls the original makes, in order (as the vectors log them).
struct LeftClickCall {
    enum class Fn : std::uint8_t {
        TargetManagerGet,     // CTargetManager::Get (then +0x168 = the old target's id)
        IsTargetable,         // PlayerBase::IsTargetable
        TipActivate,          // CTipWnd::Activate(tip, a1)
        IsGuildmasterForMyClass,
        GuildmasterString,    // 0x5151b0(class)
        TrimName,             // CEverQuest::trimName
        GetString,            // StringTable::getString(id, 0)
        BuildString,          // BuildStringFromEQSTR(id, ...)
        DspChat,              // dsp_chat(color 15, 1, 1)
        DropItemOrMoney,      // CEverQuest::DropItemOrMoneyOnPlayer
    };
    Fn fn;
    std::int32_t a = 0, b = 0; // id / tip / class / color, and the second argument
};
struct LeftClickResult {
    bool handled = false;        // false: the character list was open
    bool target_is_clicked = false; // the target is the clicked spawn; false: the old one stays (or none)
    bool save_old_target = false;   // CTargetManager +0x168 = the old target's id
    bool show_tip = false;          // CTipWnd tip 0x22 (class 62)
    bool lines = false;             // the two "who is this" chat lines
    bool named = true;              // the first line has the spawn's name (string 0x1a7c, else 0x1a87)
    std::uint32_t kind = 0, what = 0; // their string ids: kind is the parameter, what the second line
    bool drop = false;              // DropItemOrMoneyOnPlayer
    std::vector<LeftClickCall> calls;
};
// `guildmaster_string` is 0x5151b0 (guildmaster_string_id below, or a test's value).
LeftClickResult left_clicked_on_player(const LeftClickInput& in, std::uint32_t guildmaster_string);
// 0x5151b0: the "guildmaster of class" string id (classes 1..16, 20..35, 40, 41, 66; else 0x998).
std::uint32_t guildmaster_string_id(std::int32_t class_id);
// Races from 2253 on are targetable whatever IsTargetable says.
inline constexpr std::int32_t kRaceAlwaysClickTargetable = 0x8cd;

// ---------------------------------------------------------------- doors

// EQSwitch::GetSwitchDamage @0x50f4d0: a trap type's damage (+0x26), else 0.
std::uint32_t switch_damage(std::uint8_t type);

inline constexpr std::int32_t kSkillDisarmTraps = 17;
inline constexpr std::int32_t kSkillPickLock = 35;
inline constexpr std::int32_t kItemTypeLockpick = 12;

// CEverQuest::LMouseUp @0x5384d0 with an item on the cursor inside the 3D view: item types 12, 0x21 and 0x27
// (lockpicks and keys) go to ClickedSwitch, whatever is under the cursor, and are never dropped.
constexpr bool cursor_item_is_switch_key(std::int32_t item_type) {
    return item_type == kItemTypeLockpick || item_type == 0x21 || item_type == 0x27;
}

// CEverQuest::ClickedSwitch @0x51ecb0 after the switch is found: the key and
// skill EQSwitch::UseSwitch gets, and the calls in order.
struct SwitchClickInput {
    std::uint32_t switch_damage = 0;
    bool item_on_cursor = false;
    std::int32_t item_base_type = 0; // ItemBase_GetType (0 common)
    std::int32_t item_type = 0;      // ItemClient::GetItemType (12 lockpick)
    std::int32_t item_id = 0;
};
struct SwitchClickCall {
    enum class Fn : std::uint8_t { GetSwitchDamage, ItemBaseGetType, GetItemType, ItemBaseGetId, GetAdjustedSkill };
    Fn fn;
    std::int32_t arg = 0;
};
struct SwitchClick {
    std::int32_t key = -1;
    std::int32_t skill = 0; // the adjusted skill value (0 without one)
    std::int32_t skill_id = -1; // which skill was asked (-1: none)
    std::vector<SwitchClickCall> calls;
};
// `adjusted_skill` answers CharacterZoneClient::GetAdjustedSkill.
template <typename AdjustedSkill>
SwitchClick clicked_switch(const SwitchClickInput& in, AdjustedSkill&& adjusted_skill) {
    using F = SwitchClickCall::Fn;
    SwitchClick out;
    out.calls.push_back({F::GetSwitchDamage, 0});
    auto skill = [&](std::int32_t id) {
        out.calls.push_back({F::GetAdjustedSkill, id});
        out.skill_id = id;
        out.skill = adjusted_skill(id);
    };
    if (in.switch_damage != 0) {
        skill(kSkillDisarmTraps);
        return out;
    }
    if (!in.item_on_cursor) return out;
    out.calls.push_back({F::ItemBaseGetType, 0});
    if (in.item_base_type == 0) {
        out.calls.push_back({F::GetItemType, 0});
        out.calls.push_back({F::ItemBaseGetId, 0});
        out.key = in.item_id;
        if (in.item_type == kItemTypeLockpick) skill(kSkillPickLock);
    } else {
        out.calls.push_back({F::ItemBaseGetId, 0});
        out.key = in.item_id;
    }
    return out;
}

} // namespace hailstone::game
