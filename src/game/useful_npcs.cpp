#include "game/useful_npcs.h"

#include <algorithm>

#include "core/ascii_case.h"
#include "game/eq_names.h"
#include "game/world_state.h"

namespace hailstone::game {

std::string_view npc_category_key(NpcCategory c) {
    switch (c) {
    case NpcCategory::Merchant: return "merchant";
    case NpcCategory::Banker: return "banker";
    case NpcCategory::Guildmaster: return "guildmaster";
    case NpcCategory::SpecialMerchant: return "special";
    case NpcCategory::Service: return "service";
    case NpcCategory::QuestGiver: return "quest";
    case NpcCategory::PointOfInterest: return "poi";
    }
    return "poi";
}

std::string_view npc_category_label(NpcCategory c) {
    switch (c) {
    case NpcCategory::Merchant: return "Merchants";
    case NpcCategory::Banker: return "Bankers";
    case NpcCategory::Guildmaster: return "Guildmasters";
    case NpcCategory::SpecialMerchant: return "Special merchants";
    case NpcCategory::Service: return "Other services";
    case NpcCategory::QuestGiver: return "Quest givers";
    case NpcCategory::PointOfInterest: return "Points of interest";
    }
    return "Points of interest";
}

std::optional<NpcCategory> npc_category(const Spawn& s, bool findable, std::uint8_t quests) {
    if (!s.is_npc || s.is_corpse || s.summoned || s.pet_owner_id != 0) return std::nullopt;
    const std::uint8_t c = s.class_id;
    if (c >= PlayerClass::GM_First && c <= PlayerClass::GM_Last) return NpcCategory::Guildmaster;
    switch (c) {
    case PlayerClass::Class_Merchant: return NpcCategory::Merchant;
    case PlayerClass::Class_Banker:
    case PlayerClass::Class_GuildBanker: return NpcCategory::Banker;
    case PlayerClass::Class_TaskMerchant:
    case PlayerClass::Class_PvPMerchant:
    case PlayerClass::Class_AdventureMerchant:
    case PlayerClass::Class_GoodPointMerchant:
    case PlayerClass::Class_EvilPointMerchant:
    case PlayerClass::Class_PointMerchant:
    case PlayerClass::Class_LoyaltyMerchant: return NpcCategory::SpecialMerchant;
    case PlayerClass::Class_TributeMaster:
    case PlayerClass::Class_GuildTributeMaster:
    case PlayerClass::Class_TributeMaster2:
    case PlayerClass::Class_Adventure:
    case PlayerClass::Class_FellowshipMaster:
    case PlayerClass::Class_MercenaryMerchant:
    case PlayerClass::Class_RealEstateMerchant: return NpcCategory::Service;
    case PlayerClass::Class_Object: return std::nullopt; // LDoN chests and the like
    default: break;
    }
    if (quests != 0) return NpcCategory::QuestGiver;
    if (findable) return NpcCategory::PointOfInterest;
    return std::nullopt;
}

std::vector<UsefulNpc> useful_npcs(const WorldState& world, const std::unordered_set<std::uint32_t>& findable,
                                   const glm::vec3& from, const QuestNpcMap& quests, bool findable_only) {
    std::vector<UsefulNpc> out;
    std::string key;
    for (const auto& [id, s] : world.spawns) {
        if (id == world.player_id) continue;
        const bool advertised = findable.contains(id);
        if (findable_only && !advertised) continue;
        std::uint8_t q = 0;
        if (!quests.empty() && s.is_npc) {
            key = display_name(s.name);
            for (char& ch : key) ch = ascii_lower(ch);
            if (auto it = quests.find(key); it != quests.end()) q = it->second;
        }
        auto cat = npc_category(s, advertised, q);
        if (!cat && findable_only && s.is_npc && !s.is_corpse) cat = NpcCategory::PointOfInterest;
        if (!cat) continue;
        UsefulNpc n;
        n.spawn_id = id;
        n.category = *cat;
        n.name = display_name(s.name);
        n.quests = q;
        n.subtitle = !s.last_name.empty()                     ? s.last_name
                     : *cat == NpcCategory::PointOfInterest   ? std::string()
                     : *cat == NpcCategory::QuestGiver        ? std::string((q & kQuestItems) ? "Takes items" : "Has something to say")
                                                              : class_name(s.class_id);
        n.position = s.display_valid ? s.display_position : s.position;
        n.distance = glm::length(n.position - from);
        out.push_back(std::move(n));
    }
    std::sort(out.begin(), out.end(), [](const UsefulNpc& a, const UsefulNpc& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        if (int c = icompare(a.name, b.name); c != 0) return c < 0;
        return a.spawn_id < b.spawn_id;
    });
    return out;
}

bool useful_npc_matches(const UsefulNpc& n, std::string_view query) {
    const auto first = query.find_first_not_of(' ');
    if (first == std::string_view::npos) return true;
    const auto last = query.find_last_not_of(' ');
    query = query.substr(first, last - first + 1);
    auto contains = [&](std::string_view text) {
        if (query.size() > text.size()) return false;
        for (std::size_t i = 0; i + query.size() <= text.size(); ++i)
            if (iequals(text.substr(i, query.size()), query)) return true;
        return false;
    };
    return contains(n.name) || contains(n.subtitle);
}

} // namespace hailstone::game
