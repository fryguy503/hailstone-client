#pragma once

// The Find window's list (a Hailstone addition, docs/navigation.md "Find"): the NPCs a new player looks for in a
// zone, from what every stock EQEmu server sends. The spawn's class says merchants, bankers, guildmasters and the
// other services (EQEmu common/classes.h, game::PlayerClass); the server's findable list
// (OP_SendFindableNPCs, the npc_types `findable` flag) adds its points of interest. The original's find window
// (CFindLocationWnd) listed only the findable list; this one replaces it.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::game {

struct Spawn;
struct WorldState;

enum class NpcCategory : std::uint8_t {
    Merchant,        // class 41
    Banker,          // 40 Banker, 66 Guild Banker
    Guildmaster,     // 20..35, the class guildmasters (trainers)
    SpecialMerchant, // merchants for points and other currencies: 58, 59, 61, 67, 68, 70, 73
    Service,         // tribute (63, 64, 74), adventure recruiter (60), fellowship (69), mercenaries (71), real estate (72)
    QuestGiver,      // none of the above, with quests by Hailstone's server script (net/quest_npc_packets.h)
    PointOfInterest, // on the server's findable list, and none of the above
};
inline constexpr int kNpcCategoryCount = 7;

// An NPC's quests as the server script tells them (net::kQuestNpcItems / kQuestNpcTalks).
inline constexpr std::uint8_t kQuestItems = 1, kQuestTalks = 2;

// A stable key for UIs and settings ("merchant", "banker", "guildmaster", "special", "service", "quest", "poi") and the
// heading shown for it ("Merchants", ...).
std::string_view npc_category_key(NpcCategory c);
std::string_view npc_category_label(NpcCategory c);

// The category of a spawn by its class, else as a quest giver with `quests` (kQuest*), else as a point of interest
// when `findable`; nullopt for anything else and for what is never listed (players, corpses, pets, the LDoN objects
// of class 62).
std::optional<NpcCategory> npc_category(const Spawn& s, bool findable, std::uint8_t quests = 0);

struct UsefulNpc {
    std::uint32_t spawn_id = 0;
    NpcCategory category = NpcCategory::Merchant;
    std::string name;     // as shown (game::display_name)
    std::string subtitle; // the surname ("Armor Merchant"), else the class's name ("Warrior Guildmaster")
    glm::vec3 position{0.0f}; // world, where the spawn is drawn
    float distance = 0.0f;    // from `from`
    std::uint8_t quests = 0;  // kQuestItems / kQuestTalks (a merchant may have quests too)
};

// The server script's quest NPCs by their names as shown, ASCII lower case (GameConnection::quest_npcs).
using QuestNpcMap = std::unordered_map<std::string, std::uint8_t>;

// Every listed NPC of the zone, nearest first (ties by name). `findable`: the ids on the server's findable list.
std::vector<UsefulNpc> useful_npcs(const WorldState& world, const std::unordered_set<std::uint32_t>& findable,
                                   const glm::vec3& from, const QuestNpcMap& quests = {}, bool findable_only = false);

// Whether a search box's text matches the name or subtitle (ASCII case-insensitive; empty matches all).
bool useful_npc_matches(const UsefulNpc& n, std::string_view query);

} // namespace hailstone::game
