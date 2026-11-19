#pragma once

// Typed RoF2 packets for doors ("switches" in the original client: EQSwitch)
// and ground objects (dropped items, tradeskill containers and other world
// objects). Layouts follow the NMS server's RoF2 translator
// (common/patches/rof2.cpp ENCODE(OP_SpawnDoor) / ENCODE(OP_GroundSpawn) /
// ENCODE(OP_ClickObjectAction) / DECODE(OP_TradeSkillCombine),
// rof2_structs.h) and the original client: the OP_SpawnDoor record is read
// field by field by EQSwitch::LoadSwitchSounds @0x511990 (the switch
// constructor from a packet), OP_ClickDoor is built by EQSwitch::UseSwitch
// @0x511d70 and OP_ClickObject by CEverQuest::LMouseUp @0x5384d0.
// Per-packet notes are in docs/protocol/rof2-packets.md "Doors and objects".
//
// Positions are server coordinates as on the wire (game/coords.h converts).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "net/item_packets.h"
#include "net/packet.h"

namespace hailstone::net {

// One OP_SpawnDoor record (100 bytes; the packet is a list of them). The
// offsets are the original's (EQSwitch::LoadSwitchSounds) with the EQSwitch
// field each one fills.
struct DoorRecord {
    std::string name;                    // +0   [32] model name; the actor is "<name>_ACTORDEF"
    float y = 0, x = 0, z = 0;           // +32 / +36 / +40 DefaultY / DefaultX / DefaultZ
    float heading = 0;                   // +44  DefaultHeading, 0..512
    std::int32_t incline = 0;            // +48  DefaultDoorAngle (as a float), 0..512
    std::uint16_t size = 100;            // +52  ScaleFactor, percent
    std::uint16_t unknown54 = 0;         // +54
    std::uint32_t unknown56 = 0;         // +56
    std::uint8_t door_id = 0;            // +60  ID
    std::uint8_t open_type = 0;          // +61  Type
    std::uint8_t state_at_spawn = 0;     // +62  State
    std::uint8_t invert_state = 0;       // +63  DefaultState
    std::int32_t door_param = -1;        // +64  SpellID (lift travel, slide distance, ...)
    std::int32_t adventure_door_id = 0;  // +68
    std::int32_t dyn_door_id = 0;        // +72
    std::int32_t real_estate_door_id = 0;// +76
    std::uint8_t has_script = 0;         // +80  bHasScript
    std::uint8_t usable = 1;             // +81  bUsable (EQEmu: 1, "must be 1 to allow clicking")
    std::uint8_t remain_open = 0;        // +82  bRemainOpen
    std::uint8_t visible = 1;            // +83  bVisible (EQEmu: 1)
    std::uint8_t heading_changed = 0;    // +84  bHeadingChanged
    std::uint8_t unknown85 = 0, unknown86 = 0, unknown87 = 0;
    float top_speed1 = 0, top_speed2 = 0;// +88 / +92 TopSpeed1 / TopSpeed2
    std::uint8_t stamp_time = 0;         // +96  nonzero: TimeStamp = now
    std::uint8_t unknown97[3]{};
};
inline constexpr std::size_t kDoorRecordSize = 100;

// OP_SpawnDoor, zone → client: size / 100 records (HandleWorldMessage case
// 0x7291 reads exactly that many; a short tail is ignored).
std::optional<std::vector<DoorRecord>> decode_spawn_doors(const AppPacket& p);
AppPacket encode_spawn_doors(const std::vector<DoorRecord>& doors); // tests and tools

// OP_ClickDoor, client → zone (16 bytes, ClickDoor_Struct). UseSwitch sends
// the switch id (one byte; the next three are stack garbage in the
// original, zero here), the lock pick skill (clamped to 254), the key item
// id (-1 = nothing on the cursor) and the clicking spawn's id as a dword.
struct ClickDoor {
    std::uint8_t door_id = 0;
    std::int32_t pick_skill = 0;
    std::uint32_t item_id = 0xffffffffu;
    std::uint32_t player_id = 0;
};
inline constexpr std::size_t kClickDoorSize = 16;
AppPacket encode(const ClickDoor& v);
std::optional<ClickDoor> decode_click_door(const AppPacket& p);

// OP_MoveDoor, zone → client (2 bytes, MoveDoor_Struct): the switch and the
// state to change to (EQSwitch::ChangeState): 2 opens, 3 closes. EQEmu sends
// 3/2 for inverted doors (zone/doors.cpp OPEN_INVDOOR / CLOSE_INVDOOR).
struct MoveDoor {
    std::uint8_t door_id = 0;
    std::uint8_t action = 0;
};
AppPacket encode(const MoveDoor& v);
std::optional<MoveDoor> decode_move_door(const AppPacket& p);

// OP_GroundSpawn, zone → client (variable: the name is NUL-terminated; RoF2
// ENCODE(OP_GroundSpawn)). Dropped items, ground spawns, tradeskill
// containers and other world objects.
struct GroundObject {
    std::uint32_t drop_id = 0;      // entity id; OP_ClickObject names it
    std::string name;               // actor, e.g. "IT63_ACTORDEF"
    std::uint16_t zone_id = 0;
    std::uint16_t zone_instance = 0;
    std::uint32_t drop_id2 = 0;     // "some unique id" (EQEmu: drop_id again)
    std::uint32_t unknown = 0;      // "same for all objects in the zone" (EQEmu: 0)
    float heading = 0;              // 0..512
    float tilt_x = 0, tilt_y = 0;
    float size = 1;                 // scale (EQEmu: size / 100)
    float y = 0, x = 0, z = 0;
    std::int32_t object_type = 0;   // EQEmu object.h: 1 = temporary (a dropped item), else a tradeskill type
};
AppPacket encode(const GroundObject& v); // tests and tools
std::optional<GroundObject> decode_ground_object(const AppPacket& p);

// OP_ClickObject, both ways (8 bytes, ClickObject_Struct). Client → zone:
// pick up / open object drop_id (LMouseUp: the ground item's serial number
// and the player's spawn id). Zone → client: the object is gone (picked up
// or decayed; EQEmu Object::HandleClick, CreateDeSpawnPacket).
struct ClickObject {
    std::uint32_t drop_id = 0;
    std::uint32_t player_id = 0;
};
inline constexpr std::size_t kClickObjectSize = 8;
AppPacket encode(const ClickObject& v);
std::optional<ClickObject> decode_click_object(const AppPacket& p);

// OP_ClickObjectAction, zone → client (92 bytes, ClickObjectAction_Struct):
// a tradeskill container opened for this player. drop_id 0xffffffff means
// someone else is using it. Client → zone: an empty packet closes it.
struct ClickObjectAction {
    std::uint32_t drop_id = 0;
    std::int32_t unknown04 = -1, unknown08 = -1;
    std::uint32_t type = 0;   // container type (EQEmu object.h)
    std::uint32_t unknown16 = 0;
    std::uint32_t icon = 0;
    std::uint32_t unknown24 = 0;
    std::string name;         // [64] display name
};
inline constexpr std::size_t kClickObjectActionSize = 92;
AppPacket encode(const ClickObjectAction& v); // tests and tools
std::optional<ClickObjectAction> decode_click_object_action(const AppPacket& p);
AppPacket encode_close_object();              // the empty client → zone packet

// OP_ClearObject, zone → client (8 bytes, Handle_OP_ClearObject @0x4b6e20): u8 clear, 3 bytes, u32 slot. Clear 0
// removes the world container's item in that slot, anything else every slot. Shorter packets are refused.
struct ClearObject {
    std::uint8_t clear = 0;
    std::int32_t slot = 0;
};
inline constexpr std::size_t kClearObjectSize = 8;
AppPacket encode(const ClearObject& v); // server direction, for tests
std::optional<ClearObject> decode_clear_object(const AppPacket& p);

// OP_TradeSkillCombine, client → zone (24 bytes, NewCombine_Struct): combine
// the container in container_slot (a world container: type kInvWorld, slot
// -1... see docs). guildtribute_slot is unused (all -1).
struct TradeSkillCombine {
    ItemSlot container_slot;
    ItemSlot guildtribute_slot = ItemSlot::invalid();
};
inline constexpr std::size_t kTradeSkillCombineSize = 24;
AppPacket encode(const TradeSkillCombine& v);

// ---------------------------------------------------------------- recipes (the tradeskill window)
//
// EQEmu's RoF2 layouts (rof2_structs.h, zone/tradeskills.cpp); the original's CTradeskillWnd senders are not decoded.
// A recipe request names the container: its type (a bag's bag type, a world container's type) and, for a bag, its
// item id (0 for a world container).

// OP_RecipesFavorite (2008 bytes, TradeskillFavorites_Struct): u32 type, u32 item id, 500 × u32 recipe ids (0 =
// none). OP_RecipesSearch (80 bytes, RecipesSearch_Struct): u32 type, u32 item id, u32 min trivial, u32 max trivial,
// char[56] partial name (EQEmu: a regular expression), u32 0x300, u32. Both are answered by one OP_RecipeReply each.
struct RecipeFavorites {
    std::uint32_t object_type = 0, container_id = 0;
    std::vector<std::uint32_t> recipes; // at most 500
};
inline constexpr std::size_t kRecipeFavoritesSize = 2008, kRecipesSearchSize = 80;
AppPacket encode(const RecipeFavorites& v);
struct RecipesSearch {
    std::uint32_t object_type = 0, container_id = 0;
    std::uint32_t min_trivial = 0, max_trivial = 0;
    std::string text;
};
AppPacket encode(const RecipesSearch& v);

// OP_RecipeReply (84 bytes, RecipeReply_Struct): u32 type, u32 item id, u32 component count, u32 recipe id, u32
// trivial, char[64] name.
struct RecipeReply {
    std::uint32_t object_type = 0, container_id = 0;
    std::uint32_t component_count = 0, recipe_id = 0, trivial = 0;
    std::string name;
};
std::optional<RecipeReply> decode_recipe_reply(const AppPacket& p);
AppPacket encode(const RecipeReply& v); // server direction, for tests

// OP_RecipeDetails: C→Z {u32 recipe id}; Z→C (SendTradeskillDetails) u32 recipe id **big-endian**, (10 − n) × u32
// 0xffffffff, n × {u32 item id, u32 icon (both big-endian), name}: the components, one entry per item needed. The
// client reads ten entries (an unused one anywhere) and nothing after them; EQEmu since #5100 (2026-06) appends a
// big-endian u32 trivial, which is skipped.
AppPacket encode_recipe_details_request(std::uint32_t recipe_id);
struct RecipeComponent {
    std::uint32_t item_id = 0, icon = 0;
    std::string name;
};
struct RecipeDetails {
    std::uint32_t recipe_id = 0;
    std::vector<RecipeComponent> components; // at most 10
};
std::optional<RecipeDetails> decode_recipe_details(const AppPacket& p);
AppPacket encode(const RecipeDetails& v); // server direction, for tests

// OP_RecipeAutoCombine, both ways (40 bytes, RoF2 RecipeAutoCombine_Struct): u32 type, u32 item id, the container's
// slot (12 B; echoed), 12 B slot, u32 recipe id, u32 reply code (the server's: 0 made, 0xfffffff5 failed). The
// server takes the components from the inventory and combines.
struct RecipeAutoCombine {
    std::uint32_t object_type = 0, container_id = 0;
    ItemSlot container_slot = ItemSlot::invalid();
    ItemSlot other_slot = ItemSlot::invalid();
    std::uint32_t recipe_id = 0;
    std::uint32_t reply_code = 0;
};
inline constexpr std::size_t kRecipeAutoCombineSize = 40;
inline constexpr std::uint32_t kAutoCombineFailed = 0xfffffff5;
AppPacket encode(const RecipeAutoCombine& v);
std::optional<RecipeAutoCombine> decode_recipe_auto_combine(const AppPacket& p);

} // namespace hailstone::net
