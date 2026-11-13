#pragma once

// MacroQuest's spawn search text (Spawn[...], SpawnCount[...], NearestSpawn[...], /target; MQ's ParseSearchSpawn,
// ParseSearchSpawnArgs and SpawnMatchesSearch, used as the specification): words like "npc radius 50 zradius 20
// range 10 20 named", "pc =Soandso", "id 123", "pcpet", "class Wizard", "loc 100 -200", "xtarhater nopcnear 100".
// A word that is not a keyword is a level when it is a number, a class when it is a class's name or short name, and
// else part of the (partial, case-insensitive) name; "=name" asks for the whole name. Unknown keywords that take no
// argument are part of the name, as in MQ. docs/plugins.md "Spawn searches" lists the words.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace hailstone::game {
struct Spawn;
struct WorldState;
struct GroupState;
struct RaidState;
struct GuildState;
struct XTargetState;
} // namespace hailstone::game
namespace hailstone::net { class GameConnection; struct FellowshipState; }

namespace hailstone::mq {

struct SpawnSearch {
    // MQ's eSpawnType as a search asks for it (GetSpawnType, with PCPET / NPCPET / NPCCORPSE / PCCORPSE).
    enum class Kind : std::uint8_t {
        Any, Pc, Npc, Mount, Pet, PcPet, NpcPet, Corpse, NpcCorpse, PcCorpse, Trigger, Untargetable, Trap, Chest,
        Timer, Aura, Object, Banner, Campfire, Mercenary, Flyer
    };
    Kind kind = Kind::Any;
    std::optional<std::uint32_t> id;
    std::uint32_t not_id = 0;           // MQ's NotID: one id, the last notid's; 0 none
    int min_level = 0, max_level = 200; // MQ's MinLevel / MaxLevel (MAX_NPC_LEVEL); 0 is no limit
    // MQ's FRadius and ZRadius: a limit only below 10000 (a NaN never limits).
    double radius = 10000.0, zradius = 10000.0;
    float nopcnear = 0.0f; // MQ's Radius: no other player (outside the group) this near the spawn; 0 off
    std::string name;
    bool exact = false;
    std::string class_name, race_name, body_name;
    bool light = false;
    std::string light_name;        // one of MQ's light names, or empty for any light
    std::optional<std::int64_t> guild_id; // guild / guildname
    bool named = false, targetable = false, group = false, nogroup = false, raid = false, fellowship = false,
         noguild = false, nopet = false, gm = false, trader = false, lfg = false, merchant = false, banker = false,
         tribute = false, knight = false, tank = false, healer = false, dps = false, slower = false, xtarhater = false,
         los = false;
    std::uint32_t player_state = 0; // spawns with any of these PlayerState bits
    // MQ's alert lists: this client keeps none, so a list is never there (nearalert then matches nothing).
    bool alert = false, noalert = false, nearalert = false, notnearalert = false;
    // Hailstone's own words, not MQ's (docs/plugins.md): afk, linkdead, buyer.
    bool afk = false, linkdead = false, buyer = false;
    // MQ's loc: the server's x, y the radius is measured from; none = the player. `loc_z` is MQ's zLoc, which the
    // zradius is measured from too: none = the player's.
    std::optional<glm::vec2> loc;
    std::optional<float> loc_z;
};

// What a search reads besides the spawns. Null pieces: as if the player had none (no group, raid, guild, extended
// targets). `can_see` answers "los" (the player's own sight check); nullopt or none: not known, the spawn is kept.
struct SearchSources {
    const game::GroupState* group = nullptr;
    const game::RaidState* raid = nullptr;
    const game::GuildState* guild = nullptr;
    const game::XTargetState* xtargets = nullptr;
    const net::FellowshipState* fellowship = nullptr;
    std::function<std::optional<bool>(std::uint32_t spawn)> can_see;
};
SearchSources sources_of(const net::GameConnection& conn);

// `guild` (may be null) answers "guild" (the player's) and "guildname <name>".
SpawnSearch parse_spawn_search(std::string_view text, const game::GuildState* guild = nullptr);

// MQ's IsNamed (its default test, UseNewNamedTest off): a targetable NPC that is no GM trainer, merchant, banker or
// other service class, whose name does not start "A_" / "An_" or a guard's word, and starts with a capital or '#'.
bool is_named(const game::Spawn& s);
// The spawn's current place (its display position once the client moved it, else the last update's).
glm::vec3 spawn_position(const game::Spawn& s);

// The spawns the search matches, nearest the player first (MQ's NthNearestSpawn: 3D distance).
std::vector<const game::Spawn*> search_spawns(const SpawnSearch& search, const game::WorldState& world,
                                              const SearchSources& sources);
// With only a group (may be null).
std::vector<const game::Spawn*> search_spawns(const SpawnSearch& search, const game::WorldState& world,
                                              const game::GroupState* group);
// The usual call: the text searched in the connection's world, group, raid, guild and extended targets.
std::vector<const game::Spawn*> find_spawns(std::string_view text, const net::GameConnection& conn,
                                            std::function<std::optional<bool>(std::uint32_t)> can_see = {});

} // namespace hailstone::mq
