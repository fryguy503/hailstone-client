#include "mq/spawn_search.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>

#include <glm/geometric.hpp>

#include "core/float_chars.h"
#include "game/coords.h"
#include "game/eq_names.h"
#include "game/group.h"
#include "game/social.h"
#include "game/targeting.h"
#include "game/world_state.h"
#include "mq/tlo.h"
#include "net/game_connection.h"

namespace hailstone::mq {

namespace {

using K = SpawnSearch::Kind;

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// MQ's GetArg / GetNextArg without a separator: words split at spaces and tabs, a quoted run (quotes dropped) inside
// one word; an unclosed quote runs to the end.
std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i >= text.size()) break;
        std::string w;
        bool quoted = false;
        for (; i < text.size() && (quoted || (text[i] != ' ' && text[i] != '\t')); ++i) {
            if (text[i] == '"') quoted = !quoted;
            else w += text[i];
        }
        out.push_back(std::move(w));
    }
    return out;
}

// MQ's GetIntFromString / GetDoubleFromString: the number at the start of the (trimmed) text, or `fail` when there is
// none or it does not fit ("12abc" is 12, "1e300" as an int is 1).
template <typename T> T int_from(std::string_view text, T fail) {
    text = trim(text);
    T v = fail;
    auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    return ec == std::errc() ? v : fail;
}
std::optional<double> read_double(std::string_view text) {
    text = trim(text);
    double v = 0;
    auto [p, ec] = float_from_chars(text.data(), text.data() + text.size(), v); // (core/float_chars.h: Xcode 15)
    return ec == std::errc() ? std::optional<double>(v) : std::nullopt;
}
double double_from(std::string_view text, double fail) { return read_double(text).value_or(fail); }
// GetFloatFromString: as a float, so a finite number past float's range fails as std::from_chars<float> does (and
// its cast would be undefined). NaN and the infinities are read as they are.
float float_from(std::string_view text, float fail) {
    const auto d = read_double(text);
    if (!d) return fail;
    if (std::isfinite(*d) && std::fabs(*d) > static_cast<double>(std::numeric_limits<float>::max())) return fail;
    return static_cast<float>(*d);
}
// MQ's IsNumber: the whole word std::from_chars's fixed format ("12", "-3.5", ".5", "nan", "inf"; not "1e3", "+1").
bool is_number(std::string_view w) {
    if (!w.empty() && w[0] == '-') w.remove_prefix(1);
    for (std::string_view special : {"inf", "infinity", "nan"})
        if (iequals(w, special)) return true;
    bool digit = false, dot = false;
    for (char c : w) {
        if (c >= '0' && c <= '9') digit = true;
        else if (c == '.' && !dot) dot = true;
        else return false;
    }
    return digit;
}

// MQ's light names (szLights), by the spawn's light type.
constexpr std::array<std::string_view, 14> kLights = {"NONE", "CDL", "TR",  "TGS", "SL", "SoM", "LL",
                                                      "FL",   "GOS", "LG", "LS",  "GLS", "FBE", "CL"};
std::string_view light_of(const game::Spawn& s) { return s.light_type < kLights.size() ? kLights[s.light_type] : kLights[0]; }

// MQ's class long names are the client's (GetClassDesc): ours for 1..16. ParseSearchSpawnArgs also takes the
// short names and, with a deprecation warning there, ClassInfo's old "Shadowknight" and "Mage".
std::optional<std::string> class_word(std::string_view w) {
    for (std::uint32_t c = 1; c <= 16; ++c) {
        const std::string name = game::class_name(c);
        if (iequals(w, name) || iequals(w, game::class_short_name(c))) return name;
    }
    if (iequals(w, "Shadowknight")) return game::class_name(5);
    if (iequals(w, "Mage")) return game::class_name(13);
    return std::nullopt;
}

// MQ's CleanupName without the article: '_' a space, digits gone, '#' gone unless `keep_hash` (the ForWhoList form
// that the exact name is compared with, ExactSearchCleanNames off).
std::string cleanup_name(std::string_view name, bool keep_hash) {
    std::string out;
    for (char c : name) {
        if (c == '_') out += ' ';
        else if (c >= '0' && c <= '9') continue;
        else if (c == '#' && !keep_hash) continue;
        else out += c;
    }
    return out;
}

// GetSpawnType, with corpses first (the game::Spawn version knows only players and NPCs).
game::eSpawnType spawn_type(const game::Spawn& s) {
    if (s.is_corpse) return game::eSpawnType::CORPSE;
    return game::Spawn::GetSpawnType(&s);
}

K kind_of(game::eSpawnType t) {
    switch (t) {
    case game::eSpawnType::PC: return K::Pc;
    case game::eSpawnType::MOUNT: return K::Mount;
    case game::eSpawnType::PET: return K::Pet;
    case game::eSpawnType::NPC: return K::Npc;
    case game::eSpawnType::CORPSE: return K::Corpse;
    case game::eSpawnType::TRIGGER: return K::Trigger;
    case game::eSpawnType::TRAP: return K::Trap;
    case game::eSpawnType::TIMER: return K::Timer;
    case game::eSpawnType::UNTARGETABLE: return K::Untargetable;
    case game::eSpawnType::CHEST: return K::Chest;
    case game::eSpawnType::AURA: return K::Aura;
    case game::eSpawnType::OBJECT: return K::Object;
    case game::eSpawnType::BANNER: return K::Banner;
    case game::eSpawnType::CAMPFIRE: return K::Campfire;
    case game::eSpawnType::MERCENARY: return K::Mercenary;
    case game::eSpawnType::FLYER: return K::Flyer;
    default: return K::Any; // an item: no search kind asks for it
    }
}

glm::vec3 server_pos(const game::Spawn& s) { return game::world_to_server(spawn_position(s)); }

float distance3d(const game::Spawn& a, const game::Spawn& b) { return glm::distance(spawn_position(a), spawn_position(b)); }

// MQ's IsInGroup: no group, no one (not even the player); else the player, or a member by name, or with `corpse` a
// spawn whose name starts with a member's "<name>'s corpse".
bool in_group(const game::Spawn& s, const game::WorldState& world, const game::GroupState* group, bool corpse) {
    if (!group || !group->active) return false;
    if (s.id == world.player_id) return true;
    for (const auto& m : group->slots) {
        if (m.name.empty()) continue;
        if (corpse ? istarts_with(s.name, m.name + "'s corpse") : iequals(s.name, m.name)) return true;
    }
    return false;
}

// MQ's IsInRaid: the player always; else a member of the same class by name, or by its corpse's name.
bool in_raid(const game::Spawn& s, const game::WorldState& world, const game::RaidState* raid, bool corpse) {
    if (s.id == world.player_id) return true;
    if (!raid) return false;
    for (const auto& m : raid->members) {
        if (m.name.empty() || m.class_ != s.class_id) continue;
        if (corpse ? istarts_with(s.name, m.name + "'s corpse") : iequals(s.name, m.name)) return true;
    }
    return false;
}

bool in_fellowship(const game::Spawn& s, const net::FellowshipState* fellowship, bool corpse) {
    if (!fellowship || !fellowship->active()) return false;
    for (const auto& m : fellowship->members) {
        if (m.name.empty()) continue;
        if (corpse ? istarts_with(s.name, m.name + "'s corpse") : iequals(s.name, m.name)) return true;
    }
    return false;
}

// MQ's IsPCNear: another player not in the group within `radius` (3D) of the spawn. Without a group the player
// counts too, as IsInGroup is false for everyone then.
bool pc_near(const game::Spawn& s, const game::WorldState& world, const game::GroupState* group, float radius) {
    for (const auto& [id, o] : world.spawns) {
        if (o.is_npc || o.is_corpse || id == s.id) continue;
        if (in_group(o, world, group, false)) continue;
        if (distance3d(o, s) < radius) return true;
    }
    return false;
}

bool xtarget_hater(const game::Spawn& s, const game::XTargetState* xt) {
    if (!xt) return false;
    // XTARGET_AUTO_HATER (type 1), a slot not empty (status 0), holding this spawn.
    return std::any_of(xt->slots.begin(), xt->slots.end(), [&](const game::XTargetSlot& x) {
        return x.type == 1 && x.status != 0 && x.spawn_id != 0 && x.spawn_id == s.id;
    });
}

bool class_in(std::uint8_t c, std::initializer_list<std::uint8_t> set) { return std::find(set.begin(), set.end(), c) != set.end(); }

// SpawnMatchesSearch, in its order.
bool matches(const SpawnSearch& q, const game::Spawn& s, const game::Spawn* me, const game::WorldState& world,
             const SearchSources& src) {
    const game::eSpawnType raw = spawn_type(s);
    K type = kind_of(raw);
    if (raw == game::eSpawnType::ITEM) return false;

    if (type == K::Pet) {
        if (q.nopet) return false;
        if (q.kind == K::NpcPet || q.kind == K::PcPet || q.kind == K::Npc) {
            auto master = world.spawns.find(s.pet_owner_id);
            if (master != world.spawns.end()) {
                const bool player_master = !master->second.is_npc && !master->second.is_corpse;
                if (!player_master && q.kind == K::PcPet) return false;
                if (player_master && q.kind != K::PcPet) return false;
            } else if (q.kind == K::PcPet) {
                return false;
            }
            type = q.kind;
        }
    }
    if (q.kind != K::Any && q.kind != type) {
        // An NPC's corpse has no deity; a player's has one (MQ reads the spawn's Deity).
        if (q.kind == K::NpcCorpse) {
            if (type != K::Corpse || s.deity != 0) return false;
        } else if (q.kind == K::PcCorpse) {
            if (type != K::Corpse || s.deity == 0) return false;
        } else {
            return false;
        }
    }

    if (q.min_level && s.level < q.min_level) return false;
    if (q.max_level && s.level > q.max_level) return false;
    if (q.not_id == s.id) return false;
    if (q.id && *q.id != s.id) return false;
    // The spawn's guild: none is 0xffffffff here, MQ's -1.
    const std::int64_t guild = s.guild_id == 0xffffffffu ? -1 : static_cast<std::int64_t>(s.guild_id);
    if (q.guild_id && *q.guild_id != -1 && *q.guild_id != guild) return false;

    if (q.gm && q.kind != K::Npc && !s.gm) return false;
    if (q.gm && q.kind == K::Npc && (s.class_id < 20 || s.class_id > 35)) return false; // GM trainers
    if (q.named && !is_named(s)) return false;
    if (q.merchant && s.class_id != 41) return false;
    if (q.banker && s.class_id != 40) return false;
    if (q.tribute && s.class_id != 63) return false;
    if (q.noguild && guild != -1 && guild != 0) return false;
    // Warrior 1, Cleric 2, Paladin 3, Ranger 4, Shadow Knight 5, Druid 6, Bard 8, Rogue 9, Shaman 10, Wizard 12,
    // Enchanter 14, Beastlord 15, Berserker 16. These ask nothing of an "npc" search.
    if (q.kind != K::Npc) {
        if (q.knight && !class_in(s.class_id, {3, 5})) return false;
        if (q.tank && !class_in(s.class_id, {3, 5, 1})) return false;
        if (q.healer && !class_in(s.class_id, {2, 6, 10})) return false;
        if (q.dps && !class_in(s.class_id, {4, 9, 12, 16})) return false;
        if (q.slower && !class_in(s.class_id, {10, 14, 15, 8})) return false;
    }
    if (q.lfg && !s.lfg) return false;
    if (q.trader && !s.trader) return false;
    if (q.xtarhater && !xtarget_hater(s, src.xtargets)) return false;

    const bool corpse_names = q.kind == K::PcCorpse || s.is_corpse;
    if (q.nogroup && in_group(s, world, src.group, false)) return false;
    if (q.group && !in_group(s, world, src.group, corpse_names)) return false;
    if (q.fellowship && !in_fellowship(s, src.fellowship, corpse_names)) return false;
    if (q.raid && !in_raid(s, world, src.raid, corpse_names)) return false;

    const glm::vec3 p = server_pos(s);
    const float z_from = q.loc_z ? *q.loc_z : (me ? server_pos(*me).z : 0.0f);
    if (q.loc) {
        // A spawn standing exactly at the place passes whatever the radius.
        if ((q.loc->x != p.x || q.loc->y != p.y) && q.radius < 10000.0 &&
            glm::distance(p, glm::vec3(q.loc->x, q.loc->y, z_from)) > q.radius)
            return false;
    } else if (q.radius < 10000.0 && me && distance3d(*me, s) > q.radius) {
        return false;
    }
    if (q.nopcnear > 0.0f && pc_near(s, world, src.group, q.nopcnear)) return false;
    if (q.zradius < 10000.0 && (p.z > z_from + q.zradius || p.z < z_from - q.zradius)) return false;
    if (q.light) {
        const std::string_view l = light_of(s);
        if (iequals(l, "NONE")) return false;
        if (!q.light_name.empty() && !iequals(l, q.light_name)) return false;
    }
    // No alert lists here: alert, noalert and notnearalert ask nothing; nearalert finds no alert spawn near.
    if (q.nearalert) return false;
    if (!q.class_name.empty() && !iequals(q.class_name, game::class_name(s.class_id))) return false;
    if (!q.body_name.empty()) {
        const std::string_view body = s.body_type < std::size(game::szBodyType) ? game::szBodyType[s.body_type] : "";
        if (!iequals(q.body_name, body)) return false;
    }
    if (!q.race_name.empty() && !iequals(q.race_name, game::race_name(s.race))) return false;
    if (q.los && src.can_see) {
        // Not known (no collision loaded): kept, the safest of the two (docs/unknowns.md has no row: MQ always knows).
        if (auto seen = src.can_see(s.id); seen && !*seen) return false;
    }
    // MQ's "targetable" asks the game's PlayerBase::IsTargetable, not the spawn's flag alone (Spawn.Targetable is that
    // flag): body type 11 (EQEmu's NoTarget, a server's portal and trigger NPCs) is left out too.
    if (q.targetable && !game::is_targetable(s)) return false;
    if (q.player_state && !(s.player_state & q.player_state)) return false;
    // Hailstone's own words.
    if (q.afk && !s.afk) return false;
    if (q.linkdead && !s.linkdead) return false;
    if (q.buyer && !s.buyer) return false;

    if (!q.name.empty() && !s.name.empty()) {
        if (!icontains(s.name, q.name) && !icontains(cleanup_name(s.name, false), q.name)) return false;
        if (q.exact && !iequals(cleanup_name(s.name, true), q.name)) return false;
    }
    return true;
}

} // namespace

SearchSources sources_of(const net::GameConnection& conn) {
    SearchSources s;
    s.group = &conn.group();
    s.raid = &conn.raid();
    s.guild = &conn.guild();
    s.xtargets = &conn.xtargets();
    s.fellowship = &conn.fellowship();
    return s;
}

SpawnSearch parse_spawn_search(std::string_view text, const game::GuildState* guild) {
    SpawnSearch s;
    // A class's long name may hold a space ("Shadow Knight", and "knight" is a keyword): the whole text one is a class.
    if (const std::string whole(trim(text)); !whole.empty()) {
        for (std::uint32_t c = 1; c <= 16; ++c)
            if (iequals(whole, game::class_name(c))) {
                s.class_name = game::class_name(c);
                return s;
            }
    }
    const auto w = words(text);
    std::size_t i = 0;
    // a(k): the k-th word after the keyword (empty past the end); take(n): those n words are the keyword's.
    auto a = [&](std::size_t k) { return i + k < w.size() ? std::string_view(w[i + k]) : std::string_view(); };
    auto take = [&](std::size_t n) { i = std::min(i + n, w.size()); };
    while (i < w.size()) {
        const std::string& word = w[i++];
        static constexpr std::pair<std::string_view, K> kKinds[] = {
            {"pc", K::Pc},           {"npc", K::Npc},         {"mount", K::Mount},
            {"pet", K::Pet},         {"pcpet", K::PcPet},     {"npcpet", K::NpcPet},
            {"corpse", K::Corpse},   {"npccorpse", K::NpcCorpse}, {"pccorpse", K::PcCorpse},
            {"trigger", K::Trigger}, {"untargetable", K::Untargetable}, {"trap", K::Trap},
            {"chest", K::Chest},     {"timer", K::Timer},     {"aura", K::Aura},
            {"object", K::Object},   {"banner", K::Banner},   {"campfire", K::Campfire},
            {"mercenary", K::Mercenary}, {"flyer", K::Flyer}, {"any", K::Any},
            {"merc", K::Mercenary}}; // (merc: Hailstone's own short form)
        bool is_kind = false;
        for (auto [name, kind] : kKinds)
            if (iequals(word, name)) {
                s.kind = kind;
                is_kind = true;
                break;
            }
        if (is_kind) continue;
        static constexpr std::pair<std::string_view, bool SpawnSearch::*> kFlags[] = {
            {"xtarhater", &SpawnSearch::xtarhater}, {"nopet", &SpawnSearch::nopet},
            {"lfg", &SpawnSearch::lfg},             {"gm", &SpawnSearch::gm},
            {"group", &SpawnSearch::group},         {"fellowship", &SpawnSearch::fellowship},
            {"nogroup", &SpawnSearch::nogroup},     {"raid", &SpawnSearch::raid},
            {"noguild", &SpawnSearch::noguild},     {"trader", &SpawnSearch::trader},
            {"named", &SpawnSearch::named},         {"merchant", &SpawnSearch::merchant},
            {"banker", &SpawnSearch::banker},       {"tribute", &SpawnSearch::tribute},
            {"knight", &SpawnSearch::knight},       {"tank", &SpawnSearch::tank},
            {"healer", &SpawnSearch::healer},       {"dps", &SpawnSearch::dps},
            {"slower", &SpawnSearch::slower},       {"los", &SpawnSearch::los},
            {"targetable", &SpawnSearch::targetable},
            // Hailstone's own (not MQ's): flags the spawn records carry.
            {"afk", &SpawnSearch::afk},             {"linkdead", &SpawnSearch::linkdead},
            {"buyer", &SpawnSearch::buyer},         {"exact", &SpawnSearch::exact}};
        bool is_flag = false;
        for (auto [name, member] : kFlags)
            if (iequals(word, name)) {
                s.*member = true;
                is_flag = true;
                break;
            }
        if (is_flag) continue;
        if (iequals(word, "next") || iequals(word, "prev")) continue; // MQ's /target cycling: not here yet
        if (iequals(word, "name")) {
            std::string_view n = a(0);
            if (!n.empty() && n[0] == '=') {
                s.exact = true;
                n.remove_prefix(1);
            }
            s.name = std::string(n);
            take(1);
        } else if (iequals(word, "range")) {
            s.min_level = int_from(a(0), s.min_level);
            s.max_level = int_from(a(1), s.max_level);
            take(2);
        } else if (iequals(word, "level")) { // Hailstone's own: one level, as a bare number is
            s.min_level = s.max_level = int_from(a(0), s.min_level);
            take(1);
        } else if (iequals(word, "loc")) {
            // MQ's loc is "x y [z]" in its coordinates (the server's: game/coords.h). A z of 0 (or none) is the
            // player's, and then only two words are taken, as in MQ.
            const float x = float_from(a(0), 0.0f), y = float_from(a(1), 0.0f), z = float_from(a(2), 0.0f);
            s.loc = glm::vec2(x, y);
            if (z == 0.0f) {
                s.loc_z.reset();
                take(2);
            } else {
                s.loc_z = z;
                take(3);
            }
        } else if (iequals(word, "id")) {
            // An id read as MQ's GetIntFromString: "-5" is 0xfffffffb, past 32 bits or not a number 0 (no spawn).
            s.id = static_cast<std::uint32_t>(int_from(a(0), static_cast<std::int32_t>(s.id.value_or(0))));
            take(1);
        } else if (iequals(word, "notid")) {
            s.not_id = static_cast<std::uint32_t>(int_from(a(0), static_cast<std::int32_t>(s.not_id)));
            take(1);
        } else if (iequals(word, "radius")) {
            s.radius = double_from(a(0), 0.0);
            take(1);
        } else if (iequals(word, "zradius")) {
            s.zradius = double_from(a(0), 0.0);
            take(1);
        } else if (iequals(word, "body")) {
            s.body_name = std::string(a(0));
            take(1);
        } else if (iequals(word, "class")) {
            s.class_name = std::string(a(0)); // compared with the long name only, as MQ's szClass
            take(1);
        } else if (iequals(word, "race")) {
            s.race_name = std::string(a(0));
            take(1);
        } else if (iequals(word, "light")) {
            s.light = true;
            s.light_name.clear();
            for (std::string_view l : kLights)
                if (!a(0).empty() && iequals(a(0), l)) s.light_name = std::string(l);
            if (!s.light_name.empty()) take(1);
        } else if (iequals(word, "guild")) {
            if (guild) s.guild_id = guild->guild_id == 0xffffffffu ? -1 : static_cast<std::int64_t>(guild->guild_id);
        } else if (iequals(word, "guildname")) {
            // A guild the client knows by name (OP_GuildsList); an unknown one is no filter and its word stays.
            std::int64_t id = -1;
            if (guild && !a(0).empty())
                for (const auto& [gid, gname] : guild->names)
                    if (iequals(gname, a(0))) id = gid;
            if (id != -1 && id != 0 && id != 0xffffffff) {
                s.guild_id = id;
                take(1);
            }
        } else if (iequals(word, "alert")) {
            s.alert = true;
            take(1); // the list's number
        } else if (iequals(word, "noalert")) {
            s.noalert = true;
            take(1);
        } else if (iequals(word, "nearalert")) {
            s.nearalert = true;
            take(1);
        } else if (iequals(word, "notnearalert")) {
            s.notnearalert = true;
            take(1);
        } else if (iequals(word, "nopcnear")) {
            const float r = a(0).empty() ? 0.0f : float_from(a(0), 0.0f);
            if (r == 0.0f) {
                s.nopcnear = 200.0f; // MQ's default, and the word after is not taken
            } else {
                s.nopcnear = r;
                take(1);
            }
        } else if (iequals(word, "playerstate")) {
            s.player_state |= static_cast<std::uint32_t>(int_from(a(0), 0));
            take(1);
        } else if (is_number(word)) {
            s.min_level = s.max_level = int_from(std::string_view(word), s.min_level);
        } else if (auto c = class_word(word)) {
            s.class_name = *c;
        } else if (!s.name.empty()) {
            s.name += ' ';
            s.name += word;
        } else if (!word.empty() && word[0] == '=') {
            s.exact = true;
            s.name = word.substr(1);
        } else {
            s.name = word;
        }
    }
    return s;
}

bool is_named(const game::Spawn& s) {
    if (spawn_type(s) != game::eSpawnType::NPC || !game::is_targetable(s)) return false;
    if (s.class_id >= 20 && s.class_id <= 35) return false; // GM trainers
    switch (s.class_id) {
    // The service classes MQ's IsNamed passes by: banker, merchant, point merchants, adventure, object, tribute
    // masters, guild banker, fellowship, mercenary, loyalty, real estate, task and PvP merchants.
    case game::PlayerClass::Class_Banker:
    case game::PlayerClass::Class_Merchant:
    case game::PlayerClass::Class_PointMerchant:
    case game::PlayerClass::Class_Adventure:
    case game::PlayerClass::Class_AdventureMerchant:
    case game::PlayerClass::Class_Object:
    case game::PlayerClass::Class_TributeMaster:
    case game::PlayerClass::Class_GuildTributeMaster:
    case game::PlayerClass::Class_TributeMaster2:
    case game::PlayerClass::Class_GuildBanker:
    case game::PlayerClass::Class_GoodPointMerchant:
    case game::PlayerClass::Class_EvilPointMerchant:
    case game::PlayerClass::Class_FellowshipMaster:
    case game::PlayerClass::Class_MercenaryMerchant:
    case game::PlayerClass::Class_LoyaltyMerchant:
    case game::PlayerClass::Class_RealEstateMerchant:
    case game::PlayerClass::Class_TaskMerchant:
    case game::PlayerClass::Class_PvPMerchant: return false;
    default: break;
    }
    // The first word of the name as sent ("a_rat01", "#Lord_Nagafen").
    std::string_view name = s.name;
    const std::size_t start = name.find_first_not_of(' ');
    if (start == std::string_view::npos) return false;
    name = name.substr(start, name.find(' ', start) - start);
    if (name.starts_with("A_") || name.starts_with("An_")) return false;
    for (std::string_view guard : {"Guard", "Defender", "Soulbinder", "Aura", "Sage", "Ward", "Eye of",
                                   "Imperial_Crypt", "Diaku"})
        if (istarts_with(name, guard)) return false;
    return name[0] == '#' || std::isupper(static_cast<unsigned char>(name[0])) != 0;
}

glm::vec3 spawn_position(const game::Spawn& s) { return s.display_valid ? s.display_position : s.position; }

std::vector<const game::Spawn*> search_spawns(const SpawnSearch& q, const game::WorldState& world,
                                              const SearchSources& sources) {
    const game::Spawn* me = world.player();
    std::vector<std::pair<float, const game::Spawn*>> found;
    for (const auto& [id, s] : world.spawns) {
        if (!matches(q, s, me, world, sources)) continue;
        found.emplace_back(me ? distance3d(*me, s) : 0.0f, &s);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second->id < b.second->id;
    });
    std::vector<const game::Spawn*> out;
    out.reserve(found.size());
    for (auto& f : found) out.push_back(f.second);
    return out;
}

std::vector<const game::Spawn*> search_spawns(const SpawnSearch& q, const game::WorldState& world,
                                              const game::GroupState* group) {
    SearchSources src;
    src.group = group;
    return search_spawns(q, world, src);
}

std::vector<const game::Spawn*> find_spawns(std::string_view text, const net::GameConnection& conn,
                                            std::function<std::optional<bool>(std::uint32_t)> can_see) {
    SearchSources src = sources_of(conn);
    src.can_see = std::move(can_see);
    return search_spawns(parse_spawn_search(text, src.guild), conn.world(), src);
}

} // namespace hailstone::mq
