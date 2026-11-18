#pragma once

#include "net/item_packets.h"
#include "net/zone_packets.h"

namespace hailstone::net {

struct ShroudForm {
    std::uint32_t selection_id = 0, level = 0;
    std::string name;
    bool unlocked = false;
    std::uint32_t minimum_players = 0, maximum_players = 0, assigned_players = 0;
};

struct ShroudBranch {
    std::uint32_t id = 0, progress = 0;
    std::string name, description;
    std::vector<ShroudForm> forms;
};

struct ShroudCategory {
    std::uint32_t id = 0;
    std::string name, description;
    std::vector<ShroudBranch> branches;
};

struct ShroudSelection {
    // On the wire this is the shroudkeeper spawn ID. The EQEmu draft uses a task ID for its mission catalog.
    // GameConnection normalizes Spirit Shrouds to zero before presenting the selection.
    std::uint32_t context_id = 0;
    std::uint32_t status = 0; // A nonzero value redirects the player to the Shroud Bank.
    std::vector<ShroudCategory> categories;
    bool native_mission = false;
};

struct ShroudAbility {
    std::uint32_t rank_id = 0, rank_value = 0;
};

struct ShroudItem {
    std::uint32_t item_id = 0;
    std::string name;
};

struct ShroudStats {
    std::uint32_t selection_id = 0;
    bool has_data = false;
    std::string name, description;
    std::uint32_t class_ = 0, level = 0, hp = 0, mana = 0, endurance = 0;
    std::int32_t str = 0, sta = 0, cha = 0, dex = 0, int_ = 0, agi = 0, wis = 0;
    std::vector<ShroudAbility> abilities;
    std::vector<ShroudItem> items;
};

struct ShroudProgress {
    std::uint32_t category_id = 0;
    std::string name, description;
    std::uint32_t unlocked = 0, total = 0;
    std::string next_name, next_description;
    std::uint32_t percent = 0;
};

struct ShroudAbilityReply {
    std::uint32_t selection_id = 0, rank_id = 0;
};

struct ShroudItemReply {
    std::uint32_t selection_id = 0, item_id = 0;
    Item item;
};

struct ShroudTransition {
    Spawn spawn;
    std::optional<PlayerProfile> profile; // Absent for a transition seen by another player.
    std::vector<Item> possessions;
};

std::optional<ShroudSelection> decode_shroud_selection(const AppPacket& p);
std::optional<ShroudStats> decode_shroud_stats(const AppPacket& p);
std::optional<std::vector<ShroudProgress>> decode_shroud_progress(const AppPacket& p);
std::optional<ShroudAbilityReply> decode_shroud_ability(const AppPacket& p);
std::optional<ShroudItemReply> decode_shroud_item(const AppPacket& p);
std::optional<ShroudTransition> decode_shroud_transition(const AppPacket& p, const PlayerProfile* previous = nullptr);

// Native sends at 0x41375f, 0x413842, 0x413912, 0x720951 and 0x720677 include
// the opcode in the send length. AppPacket stores that opcode separately.
AppPacket encode_shroud_request_stats(std::uint32_t selection_id);
AppPacket encode_shroud_select(std::uint32_t selection_id);
AppPacket encode_shroud_cancel();
AppPacket encode_shroud_request_ability(std::uint32_t selection_id, std::uint32_t rank_id);
AppPacket encode_shroud_request_item(std::uint32_t selection_id, std::uint32_t item_id);

struct MonsterMissionTemplate {
    std::uint32_t id = 0;
    std::int32_t minimum = 0, maximum = 0; // Maximum <= 0 means unlimited.
    bool selectable = false;
    std::string name;
};

struct MonsterMissionCatalog {
    std::uint32_t task_id = 0;
    bool selection_locked = false;
    std::vector<MonsterMissionTemplate> templates;
};

struct MonsterMissionAssignment {
    std::string name;
    std::uint32_t template_id = 0;
};

struct MonsterMissionResult {
    std::uint32_t template_id = 0, result = 0;
    std::uint32_t message_id() const;
};

std::optional<MonsterMissionCatalog> decode_monster_mission_catalog(const AppPacket& p);
std::optional<ShroudStats> decode_monster_mission_stats(const AppPacket& p);
std::optional<ShroudAbilityReply> decode_monster_mission_ability(const AppPacket& p);
std::optional<ShroudItemReply> decode_monster_mission_item(const AppPacket& p); // No selection ID on the wire.
std::optional<MonsterMissionAssignment> decode_monster_mission_assignment(const AppPacket& p);
std::optional<MonsterMissionResult> decode_monster_mission_result(const AppPacket& p);

AppPacket encode_monster_mission_catalog_request();
AppPacket encode_monster_mission_stats_request(std::uint32_t template_id);
AppPacket encode_monster_mission_ability_request(std::uint32_t template_id, std::uint32_t rank_id);
AppPacket encode_monster_mission_item_request(std::uint32_t template_id, std::uint32_t item_id);
AppPacket encode_monster_mission_select(std::uint32_t template_id);

enum class AltStorageAction : std::uint32_t { Open, Close, Withdraw, Refresh, Remove, Inspect, Currency };

struct AltStorageControl {
    AltStorageAction action = AltStorageAction::Open;
    std::uint32_t entity_id = 0;
    std::uint64_t storage_key = 0;
    std::uint32_t modifier = 0;
    std::uint32_t platinum = 0, gold = 0, silver = 0, copper = 0;
};

struct AltStorageEntry {
    std::string name;
    std::uint32_t icon = 0;
    std::uint64_t key = 0;
    std::uint32_t quantity = 0;
};

struct AltStorageList {
    std::uint32_t retention_seconds = 0;
    std::vector<AltStorageEntry> entries;
};

std::optional<AltStorageControl> decode_alt_storage(const AppPacket& p);
std::optional<AltStorageList> decode_alt_storage_list(const AppPacket& p);
AppPacket encode_alt_storage_open(std::uint32_t entity_id);
AppPacket encode_alt_storage_withdraw(std::uint64_t key);
AppPacket encode_alt_storage_inspect(std::uint64_t key, std::uint32_t modifier = 0);

} // namespace hailstone::net
