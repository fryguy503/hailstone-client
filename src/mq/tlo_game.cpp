// The game's MacroQuest TLOs and data types (mq/tlo.h), from GameConnection's state.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <functional>
#include <optional>
#include <random>

#include <glm/geometric.hpp>

#include "game/combat.h"
#include "game/coords.h"
#include "game/eq_names.h"
#include "game/group.h"
#include "game/inventory.h"
#include "game/spells.h"
#include "game/world_state.h"
#include "mq/calc.h"
#include "mq/spawn_search.h"
#include "mq/tlo.h"
#include "nav/nav_control.h"
#include "net/game_connection.h"

namespace hailstone::mq {

namespace {

using V = Value;
using net::GameConnection;

struct Types {
    const Type *spawn, *character, *cls, *race, *heading, *spell, *buff, *timestamp, *item, *zone, *group, *member,
        *everquest, *window, *plugin, *time, *gametime, *navigation, *stick;
};

Types g; // set once by register_game_tlos (the types live in the registry that registered them)

// Which of the 16 compass points (N, NNE, ...) a heading in degrees is nearest; N for NaN or an infinity.
int heading_point(double deg) {
    if (!std::isfinite(deg)) return 0;
    const double t = std::fmod(std::fmod(deg + 11.25, 360.0) + 360.0, 360.0); // [0, 360)
    return static_cast<int>(t / 22.5) % 16;
}

const game::Spawn* spawn_of(const Context& c, std::int64_t id) {
    if (!c.conn || id <= 0) return nullptr;
    const auto& spawns = c.conn->world().spawns;
    auto it = spawns.find(static_cast<std::uint32_t>(id));
    return it == spawns.end() ? nullptr : &it->second;
}

const game::Spawn* self_spawn(const Context& c, const Value& v) { return spawn_of(c, v.i); }

V spawn_value(const game::Spawn* s) { return s ? V::object(g.spawn, s->id) : V::nil(); }

bool is_me(const Context& c, const game::Spawn& s) { return c.conn && s.id == c.conn->world().player_id; }

const game::Spawn* me(const Context& c) { return c.conn ? c.conn->world().player() : nullptr; }

// MQ's headings: degrees clockwise from north. EQ's are 0..512 counter-clockwise from north (game/coords.h).
double eq_heading_to_degrees(double h) {
    double d = 360.0 - h * 360.0 / 512.0;
    d = std::fmod(d, 360.0);
    return d < 0 ? d + 360.0 : d;
}

V heading_value(double degrees) { return V::object(g.heading, 0, degrees); }

// The heading from a to b (world coordinates; world +x is the server's +y, north).
double heading_between(const glm::vec3& a, const glm::vec3& b) {
    glm::vec3 sa = game::world_to_server(a), sb = game::world_to_server(b);
    // Server: +y north, +x west (EQ's mirrored x). Clockwise from north: east is -x.
    double dx = -(sb.x - sa.x), dy = sb.y - sa.y;
    double deg = std::atan2(dx, dy) * 180.0 / 3.14159265358979323846;
    return deg < 0 ? deg + 360.0 : deg;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

// ---------------------------------------------------------------- spells and buffs

const game::SpellData* spell_data(const Context& c, std::int64_t id) {
    if (!c.conn || id < 0 || id == game::kNoSpell) return nullptr;
    return c.conn->spell_table().get(static_cast<std::uint32_t>(id));
}

V spell_value(const Context& c, std::uint32_t id) {
    return spell_data(c, id) ? V::object(g.spell, id) : V::nil();
}

V spell_by_index(const Context& c, std::string_view index) {
    if (!c.conn) return V::nil();
    std::int64_t id = 0;
    if (parse_int(index, id)) return spell_value(c, static_cast<std::uint32_t>(id));
    const game::SpellData* s = c.conn->spell_table().find_by_name(index);
    return s ? V::object(g.spell, s->id) : V::nil();
}

// The buff slots (long buffs or songs: the spell's short buff box flag) that hold a spell, in slot order.
std::vector<int> buff_slots(const Context& c, bool songs) {
    std::vector<int> out;
    if (!c.conn) return out;
    const auto& buffs = c.conn->spell_state().buffs;
    for (int i = 0; i < game::kBuffSlots; ++i) {
        if (buffs[static_cast<std::size_t>(i)].empty()) continue;
        const game::SpellData* s = spell_data(c, buffs[static_cast<std::size_t>(i)].spell);
        bool song = s && s->duration_window;
        if (song == songs) out.push_back(i);
    }
    return out;
}

V buff_by_index(const Context& c, std::string_view index, bool songs) {
    auto slots = buff_slots(c, songs);
    std::int64_t n = 0;
    if (parse_int(index, n)) {
        if (n < 1 || n > static_cast<std::int64_t>(slots.size())) return V::nil();
        return V::object(g.buff, slots[static_cast<std::size_t>(n - 1)]);
    }
    for (int slot : slots) {
        const game::SpellData* s = spell_data(c, c.conn->spell_state().buffs[static_cast<std::size_t>(slot)].spell);
        if (s && (iequals(s->name, index) || (!index.empty() && index.front() != '=' && icontains(s->name, index))))
            return V::object(g.buff, slot);
    }
    return V::nil();
}

// A buff value is one of the player's buff slots, or (a second number, the spawn's id) an entry of the buffs
// OP_TargetBuffs last gave for that spawn (Target.Buff). Once those are another spawn's, it reads as nothing.
const game::BuffSlot* buff_of(const Context& c, const Value& v) {
    if (!c.conn || v.i < 0) return nullptr;
    const auto& st = c.conn->spell_state();
    if (v.f != 0) {
        if (st.target_buffs_entity != static_cast<std::uint32_t>(v.f) ||
            v.i >= static_cast<std::int64_t>(st.target_buffs.size()))
            return nullptr;
        const auto& b = st.target_buffs[static_cast<std::size_t>(v.i)].second;
        return b.empty() ? nullptr : &b;
    }
    if (v.i >= game::kBuffSlots) return nullptr;
    const auto& b = st.buffs[static_cast<std::size_t>(v.i)];
    return b.empty() ? nullptr : &b;
}

// A spawn's buffs as buff values, in slot order: the player's own, or what OP_TargetBuffs last gave for this spawn
// (none for any other: the client is told only its target's, and only when the server sends them).
struct SpawnBuff {
    V value;
    const game::BuffSlot* buff;
};
std::vector<SpawnBuff> spawn_buffs(const Context& c, const game::Spawn& s) {
    std::vector<SpawnBuff> out;
    if (!c.conn) return out;
    const auto& st = c.conn->spell_state();
    if (is_me(c, s)) {
        for (int i = 0; i < game::kBuffSlots; ++i)
            if (!st.buffs[static_cast<std::size_t>(i)].empty())
                out.push_back({V::object(g.buff, i), &st.buffs[static_cast<std::size_t>(i)]});
    } else if (s.id != 0 && st.target_buffs_entity == s.id) {
        for (std::size_t i = 0; i < st.target_buffs.size(); ++i)
            if (!st.target_buffs[i].second.empty())
                out.push_back({V::object(g.buff, static_cast<std::int64_t>(i), s.id), &st.target_buffs[i].second});
    }
    return out;
}

// The first of a spawn's buffs with this spell effect (SPA, EQEmu's spdat.h) whose base value passes `pass`.
template <typename Pass> V buff_with_effect(const Context& c, const game::Spawn& s, std::int32_t spa, Pass pass) {
    for (const auto& b : spawn_buffs(c, s)) {
        const game::SpellData* d = spell_data(c, b.buff->spell);
        if (!d) continue;
        for (std::size_t e = 0; e < d->effect.size(); ++e)
            if (d->effect[e] == spa && pass(d->base[e])) return b.value;
    }
    return V::nil();
}

// Buff ticks are 6 s; the countdown the client keeps (BuffSlot::tics) is as the server last sent it. A held countdown
// (game::buff_countdown_held) stays at it: the zone's buff suspension holds anyone's suspendable buffs, the player's
// buff-timer hold only the player's own.
std::int64_t buff_ms_left(const Context& c, const game::BuffSlot& b) {
    if (b.tics < 0) return -1; // permanent
    if (!c.conn) return static_cast<std::int64_t>(game::buff_ms_left(b.tics, b.received_ms, GameConnection::now_ms()));
    const auto& st = c.conn->spell_state();
    const std::less<const game::BuffSlot*> before;
    const bool own = !before(&b, st.buffs.data()) && before(&b, st.buffs.data() + st.buffs.size());
    return static_cast<std::int64_t>(game::buff_ms_left(b, spell_data(c, b.spell), own && st.buff_timers_on_hold,
                                                        st.zone_suspends_buffs, GameConnection::now_ms()));
}

const char* target_type_name(int t) {
    switch (t) {
    case 1: return "Line of Sight";
    case 2: return "AE PC v1";
    case 3: return "Group v1";
    case 4: return "PB AE";
    case 5: return "Single";
    case 6: return "Self";
    case 8: return "Targeted AE";
    case 9: return "Animal";
    case 10: return "Undead";
    case 11: return "Summoned";
    case 13: return "Life Tap";
    case 14: return "Pet";
    case 15: return "Corpse";
    case 16: return "Plant";
    case 17: return "Uber Giants";
    case 18: return "Uber Dragons";
    case 20: return "Targeted AE Tap";
    case 24: return "AE Undead";
    case 25: return "AE Summoned";
    case 32: return "Hatelist";
    case 33: return "Hatelist2";
    case 34: return "Chest";
    case 36: return "Caster PB PC";
    case 37: return "Caster PB NPC";
    case 38: return "Pet2";
    case 39: return "No Pets";
    case 40: return "AE PC v2";
    case 41: return "Group v2";
    case 42: return "Directional AE";
    case 43: return "Single in Group";
    case 44: return "Beam";
    case 45: return "Free Target";
    case 46: return "Target of Target";
    case 47: return "Pet Owner";
    default: return "Unknown";
    }
}

const char* resist_name(int r) {
    static const char* kNames[] = {"Unresistable", "Magic", "Fire", "Cold", "Poison", "Disease",
                                   "Chromatic", "Prismatic", "Physical", "Corruption"};
    return r >= 0 && r < 10 ? kNames[r] : "Unknown";
}

// ---------------------------------------------------------------- items

// An item's place packed into a value: type << 32 | slot << 16 | (sub + 1).
std::int64_t pack_slot(const net::ItemSlot& s) {
    return (static_cast<std::int64_t>(s.type) << 32) | (static_cast<std::int64_t>(static_cast<std::uint16_t>(s.slot)) << 16) |
           static_cast<std::uint16_t>(s.sub + 1);
}
net::ItemSlot unpack_slot(std::int64_t v) {
    net::ItemSlot s;
    s.type = static_cast<std::int16_t>(v >> 32);
    s.slot = static_cast<std::int16_t>(static_cast<std::uint16_t>(v >> 16));
    s.sub = static_cast<std::int16_t>(static_cast<std::int32_t>(v & 0xffff) - 1);
    return s;
}

const net::Item* item_of(const Context& c, const Value& v) {
    return c.conn ? c.conn->inventory().at(unpack_slot(v.i)) : nullptr;
}

V item_value(const Context& c, const net::ItemSlot& s) {
    return c.conn && c.conn->inventory().at(s) ? V::object(g.item, pack_slot(s)) : V::nil();
}

// The player's carried items (worn, general slots and their bags, the cursor), in slot order.
template <typename F> void each_carried(const Context& c, F&& fn) {
    if (!c.conn) return;
    for (const auto& [key, it] : c.conn->inventory().items()) {
        if (key.first != net::kInvPossessions) continue;
        net::ItemSlot top = net::ItemSlot::possessions(key.second);
        if (fn(top, it)) return;
        for (const auto& b : it.contents)
            if (fn(net::ItemSlot::possessions(key.second, static_cast<std::int16_t>(b.index)), b.item)) return;
    }
}

bool item_name_matches(const net::Item& it, std::string_view query) {
    if (!query.empty() && query.front() == '=') return iequals(it.name, query.substr(1));
    return icontains(it.name, query);
}

constexpr const char* kSlotNames[] = {"charm",     "leftear",   "head",     "face",      "rightear", "neck",
                                      "shoulder",  "arms",      "back",     "leftwrist", "rightwrist", "range",
                                      "hands",     "mainhand",  "offhand",  "leftfinger", "rightfinger", "chest",
                                      "legs",      "feet",      "waist",    "powersource", "ammo",   "pack1",
                                      "pack2",     "pack3",     "pack4",    "pack5",     "pack6",    "pack7",
                                      "pack8",     "pack9",     "pack10",   "cursor"};

int slot_by_name(std::string_view index) {
    std::int64_t n = 0;
    if (parse_int(index, n)) return static_cast<int>(n);
    for (int i = 0; i < static_cast<int>(std::size(kSlotNames)); ++i)
        if (iequals(index, kSlotNames[i])) return i;
    return -1;
}

const char* item_type_name(int t) {
    switch (t) {
    case 0: return "1H Slashing";
    case 1: return "2H Slashing";
    case 2: return "1H Piercing";
    case 3: return "1H Blunt";
    case 4: return "2H Blunt";
    case 5: return "Archery";
    case 7: return "Throwing range items";
    case 8: return "Shield";
    case 10: return "Armor";
    case 11: return "Misc";
    case 12: return "Lockpicks";
    case 14: return "Food";
    case 15: return "Drink";
    case 16: return "Light";
    case 17: return "Combinable";
    case 18: return "Bandages";
    case 19: return "Throwing";
    case 20: return "Scroll";
    case 21: return "Potion";
    case 23: return "Wind Instrument";
    case 24: return "Stringed Instrument";
    case 25: return "Brass Instrument";
    case 26: return "Percussion Instrument";
    case 27: return "Arrow";
    case 29: return "Jewelry";
    case 35: return "2H Piercing";
    case 38: return "Alcohol";
    case 45: return "Martial";
    case 52: return "Charm";
    case 54: return "Augmentation";
    default: return "Unknown";
    }
}

// ---------------------------------------------------------------- group

// MQ's group order: 0 is the player, then the other members in slot order.
std::vector<const game::GroupMember*> group_members(const Context& c) {
    std::vector<const game::GroupMember*> out;
    if (!c.conn || !c.conn->group().active) return out;
    const auto& grp = c.conn->group();
    const std::string& my_name = c.conn->character_name();
    for (const auto& m : grp.slots)
        if (iequals(m.name, my_name)) out.push_back(&m);
    if (out.empty()) out.push_back(nullptr); // the player before the server listed them
    for (const auto& m : grp.slots)
        if (!m.name.empty() && !iequals(m.name, my_name)) out.push_back(&m);
    return out;
}

const game::Spawn* spawn_named(const Context& c, std::string_view name) {
    if (!c.conn) return nullptr;
    for (const auto& [id, s] : c.conn->world().spawns)
        if (!s.is_npc && !s.is_corpse && iequals(s.name, name)) return &s;
    // Mercenaries and other virtual members go by their clean name.
    for (const auto& [id, s] : c.conn->world().spawns)
        if (!s.is_corpse && iequals(game::display_name(s.name), name)) return &s;
    return nullptr;
}

V member_value(const Context& c, int index) {
    auto members = group_members(c);
    if (index < 0 || index >= static_cast<int>(members.size())) return V::nil();
    std::string name = members[static_cast<std::size_t>(index)] ? members[static_cast<std::size_t>(index)]->name
                                                                 : c.conn->character_name();
    return V::object(g.member, index, 0, name);
}

const game::GroupMember* member_of(const Context& c, const Value& v) {
    auto members = group_members(c);
    if (v.i < 0 || v.i >= static_cast<std::int64_t>(members.size())) return nullptr;
    return members[static_cast<std::size_t>(v.i)];
}

// ---------------------------------------------------------------- registration helpers

template <typename F> void add(const Type* t, const char* name, F fn) {
    const_cast<Type*>(t)->members[name] = MemberFn(std::move(fn));
}

// A spawn member reading only the spawn.
template <typename F> void spawn_member(const char* name, F fn) {
    add(g.spawn, name, [fn](const Context& c, const Value& v, std::string_view index) -> V {
        const game::Spawn* s = self_spawn(c, v);
        return s ? fn(c, *s, index) : V::nil();
    });
}

// A character member (the player: the connection's own state).
template <typename F> void me_member(const char* name, F fn) {
    add(g.character, name, [fn](const Context& c, const Value&, std::string_view index) -> V {
        return c.conn ? fn(c, *c.conn, index) : V::nil();
    });
}

void register_spawn() {
    Type& t = const_cast<Type&>(*g.spawn);
    t.to_value = [](const Context& c, const Value& v) {
        const game::Spawn* s = self_spawn(c, v);
        return s ? V::string(game::display_name(s->name)) : V::nil();
    };
    spawn_member("ID", [](const Context&, const game::Spawn& s, std::string_view) { return V::integer(s.id); });
    spawn_member("Name", [](const Context&, const game::Spawn& s, std::string_view) { return V::string(s.name); });
    spawn_member("CleanName", [](const Context&, const game::Spawn& s, std::string_view) {
        return V::string(game::display_name(s.name));
    });
    spawn_member("DisplayName", [](const Context&, const game::Spawn& s, std::string_view) {
        return V::string(game::display_name(s.name));
    });
    spawn_member("Surname", [](const Context&, const game::Spawn& s, std::string_view) { return V::string(s.last_name); });
    spawn_member("Title", [](const Context&, const game::Spawn& s, std::string_view) { return V::string(s.title); });
    spawn_member("Suffix", [](const Context&, const game::Spawn& s, std::string_view) { return V::string(s.suffix); });
    spawn_member("Level", [](const Context&, const game::Spawn& s, std::string_view) { return V::integer(s.level); });
    spawn_member("Class", [](const Context&, const game::Spawn& s, std::string_view) { return V::object(g.cls, s.class_id); });
    spawn_member("Race", [](const Context&, const game::Spawn& s, std::string_view) { return V::object(g.race, s.race); });
    spawn_member("Gender", [](const Context&, const game::Spawn& s, std::string_view) {
        return V::string(s.gender == 0 ? "male" : s.gender == 1 ? "female" : "neuter");
    });
    spawn_member("Type", [](const Context&, const game::Spawn& s, std::string_view) {
        if (s.is_corpse) return V::string("Corpse");
        if (s.mercenary) return V::string("Mercenary");
        if (s.pet_owner_id) return V::string("Pet");
        return V::string(s.is_npc ? "NPC" : "PC");
    });
    spawn_member("PctHPs", [](const Context&, const game::Spawn& s, std::string_view) { return V::integer(s.hp_percent); });
    spawn_member("CurrentHPs", [](const Context&, const game::Spawn& s, std::string_view) { return V::integer(s.hp_percent); });
    spawn_member("MaxHPs", [](const Context&, const game::Spawn&, std::string_view) { return V::integer(100); });
    spawn_member("PctMana", [](const Context& c, const game::Spawn& s, std::string_view) {
        const auto& t2 = c.conn->group().mana_percent;
        auto it = t2.find(s.id);
        return it == t2.end() ? V::nil() : V::integer(it->second);
    });
    spawn_member("PctEndurance", [](const Context& c, const game::Spawn& s, std::string_view) {
        const auto& t2 = c.conn->group().endurance_percent;
        auto it = t2.find(s.id);
        return it == t2.end() ? V::nil() : V::integer(it->second);
    });
    // MQ's X and Y are the server's (the /loc numbers, Y first there).
    spawn_member("X", [](const Context&, const game::Spawn& s, std::string_view) {
        return V::number(game::world_to_server(spawn_position(s)).x);
    });
    spawn_member("Y", [](const Context&, const game::Spawn& s, std::string_view) {
        return V::number(game::world_to_server(spawn_position(s)).y);
    });
    spawn_member("Z", [](const Context&, const game::Spawn& s, std::string_view) { return V::number(spawn_position(s).z); });
    spawn_member("Loc", [](const Context&, const game::Spawn& s, std::string_view) {
        glm::vec3 p = game::world_to_server(spawn_position(s));
        return V::string(std::format("{:.2f}, {:.2f}, {:.2f}", p.y, p.x, p.z));
    });
    spawn_member("LocYX", [](const Context&, const game::Spawn& s, std::string_view) {
        glm::vec3 p = game::world_to_server(spawn_position(s));
        return V::string(std::format("{:.0f}, {:.0f}", p.y, p.x));
    });
    spawn_member("Heading", [](const Context& c, const game::Spawn& s, std::string_view) {
        double h = is_me(c, s) ? c.conn->player_heading() : (s.display_valid ? s.display_heading : s.heading);
        return heading_value(eq_heading_to_degrees(h));
    });
    spawn_member("HeadingTo", [](const Context& c, const game::Spawn& s, std::string_view) {
        const game::Spawn* m = me(c);
        return m ? heading_value(heading_between(spawn_position(*m), spawn_position(s))) : V::nil();
    });
    spawn_member("Distance", [](const Context& c, const game::Spawn& s, std::string_view) {
        const game::Spawn* m = me(c);
        if (!m) return V::nil();
        glm::vec3 a = spawn_position(*m), b = spawn_position(s);
        return V::number(std::hypot(a.x - b.x, a.y - b.y));
    });
    spawn_member("Distance3D", [](const Context& c, const game::Spawn& s, std::string_view) {
        const game::Spawn* m = me(c);
        return m ? V::number(glm::distance(spawn_position(*m), spawn_position(s))) : V::nil();
    });
    spawn_member("DistanceZ", [](const Context& c, const game::Spawn& s, std::string_view) {
        const game::Spawn* m = me(c);
        return m ? V::number(std::abs(spawn_position(*m).z - spawn_position(s).z)) : V::nil();
    });
    spawn_member("Moving", [](const Context& c, const game::Spawn& s, std::string_view) {
        if (is_me(c, s)) return V::boolean(c.conn->player_moving());
        return V::boolean(glm::dot(s.velocity, s.velocity) > 1e-4f);
    });
    spawn_member("StandState", [](const Context&, const game::Spawn& s, std::string_view) { return V::integer(s.stand_state); });
    spawn_member("Sitting", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.stand_state == 110); });
    spawn_member("Standing", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.stand_state == 100); });
    spawn_member("Ducking", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.stand_state == 111); });
    spawn_member("Feigning", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.stand_state == 115); });
    spawn_member("Dead", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.is_corpse); });
    spawn_member("Targetable", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.targetable); });
    // The buffs: the player's own, or the target's as OP_TargetBuffs gave them. A server sends those only for some
    // players (EQEmu: GMs, the Inspect Buffs AA or leadership, or its AlwaysSendTargetsBuffs rule), so a script asks
    // BuffsPopulated first.
    spawn_member("BuffsPopulated", [](const Context& c, const game::Spawn& s, std::string_view) {
        return V::boolean(is_me(c, s) || (c.conn && s.id != 0 && c.conn->spell_state().target_buffs_entity == s.id));
    });
    spawn_member("BuffCount", [](const Context& c, const game::Spawn& s, std::string_view) {
        return V::integer(static_cast<std::int64_t>(spawn_buffs(c, s).size()));
    });
    // Buff[n] the nth, Buff[name] by name (exact, else a part of it; "=name" exact only); MyBuff only the player's.
    auto find_buff = [](const Context& c, const game::Spawn& s, std::string_view index, bool mine) -> V {
        auto list = spawn_buffs(c, s);
        if (mine) {
            const std::uint32_t self = c.conn ? c.conn->world().player_id : 0;
            std::erase_if(list, [&](const SpawnBuff& b) { return b.buff->caster_id != self; });
        }
        std::int64_t n = 0;
        if (parse_int(index, n)) {
            if (n < 1 || n > static_cast<std::int64_t>(list.size())) return V::nil();
            return list[static_cast<std::size_t>(n - 1)].value;
        }
        const bool exact = !index.empty() && index.front() == '=';
        const std::string_view name = exact ? index.substr(1) : index;
        if (name.empty()) return V::nil();
        for (const auto& b : list)
            if (const game::SpellData* d = spell_data(c, b.buff->spell); d && iequals(d->name, name)) return b.value;
        if (!exact)
            for (const auto& b : list)
                if (const game::SpellData* d = spell_data(c, b.buff->spell); d && icontains(d->name, name))
                    return b.value;
        return V::nil();
    };
    auto duration = [](const Context& c, const V& b) -> V {
        const game::BuffSlot* s = b.kind == Value::Kind::Object ? buff_of(c, b) : nullptr;
        const std::int64_t ms = s ? buff_ms_left(c, *s) : -1;
        return ms < 0 ? V::nil() : V::object(g.timestamp, ms);
    };
    spawn_member("Buff", [find_buff](const Context& c, const game::Spawn& s, std::string_view index) -> V {
        if (index.empty()) return V::integer(static_cast<std::int64_t>(spawn_buffs(c, s).size()));
        return find_buff(c, s, index, false);
    });
    spawn_member("MyBuff", [find_buff](const Context& c, const game::Spawn& s, std::string_view index) {
        return find_buff(c, s, index, true);
    });
    spawn_member("BuffDuration", [find_buff, duration](const Context& c, const game::Spawn& s, std::string_view index) {
        return duration(c, find_buff(c, s, index, false));
    });
    spawn_member("MyBuffDuration", [find_buff, duration](const Context& c, const game::Spawn& s, std::string_view index) {
        return duration(c, find_buff(c, s, index, true));
    });
    // The state a buff puts a spawn in, as the buff (MQ's): by its spell effects (SPA, EQEmu's spdat.h).
    spawn_member("Mezzed", [](const Context& c, const game::Spawn& s, std::string_view) {
        return buff_with_effect(c, s, 31, [](std::int32_t) { return true; });
    });
    spawn_member("Charmed", [](const Context& c, const game::Spawn& s, std::string_view) {
        return buff_with_effect(c, s, 22, [](std::int32_t) { return true; });
    });
    spawn_member("Feared", [](const Context& c, const game::Spawn& s, std::string_view) {
        return buff_with_effect(c, s, 23, [](std::int32_t) { return true; });
    });
    spawn_member("Rooted", [](const Context& c, const game::Spawn& s, std::string_view) {
        return buff_with_effect(c, s, 99, [](std::int32_t) { return true; });
    });
    spawn_member("Snared", [](const Context& c, const game::Spawn& s, std::string_view) { // movement speed down
        return buff_with_effect(c, s, 3, [](std::int32_t base) { return base < 0; });
    });
    spawn_member("Slowed", [](const Context& c, const game::Spawn& s, std::string_view) { // attack speed under 100%
        return buff_with_effect(c, s, 11, [](std::int32_t base) { return base < 100; });
    });
    spawn_member("LineOfSight", [](const Context& c, const game::Spawn& s, std::string_view) -> V {
        if (is_me(c, s)) return V::boolean(true);
        const auto seen = c.can_see ? c.can_see(s.id) : std::nullopt;
        return seen ? V::boolean(*seen) : V::nil();
    });
    spawn_member("Named", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(is_named(s)); });
    spawn_member("Height", [](const Context&, const game::Spawn& s, std::string_view) { return V::number(s.size); });
    spawn_member("MaxRangeTo", [](const Context&, const game::Spawn& s, std::string_view) {
        return V::number(std::max(s.melee_radius, 14.0f));
    });
    spawn_member("AFK", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.afk); });
    spawn_member("LFG", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.lfg); });
    spawn_member("Linkdead", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.linkdead); });
    spawn_member("GM", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.gm); });
    spawn_member("Anonymous", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.anon == 1); });
    spawn_member("Roleplaying", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.anon == 2); });
    spawn_member("Trader", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.trader); });
    spawn_member("Buyer", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.buyer); });
    spawn_member("Mercenary", [](const Context&, const game::Spawn& s, std::string_view) { return V::boolean(s.mercenary); });
    spawn_member("Master", [](const Context& c, const game::Spawn& s, std::string_view) {
        return spawn_value(spawn_of(c, s.pet_owner_id));
    });
    spawn_member("Owner", [](const Context& c, const game::Spawn& s, std::string_view) {
        return spawn_value(spawn_of(c, s.pet_owner_id));
    });
    spawn_member("Pet", [](const Context& c, const game::Spawn& s, std::string_view) -> V {
        if (is_me(c, s) && c.conn->pets().focused) return spawn_value(spawn_of(c, c.conn->pets().focused));
        for (const auto& [id, o] : c.conn->world().spawns)
            if (o.pet_owner_id == s.id && !o.is_corpse) return spawn_value(&o);
        return V::nil();
    });
    spawn_member("ConColor", [](const Context& c, const game::Spawn& s, std::string_view) {
        const game::Spawn* m = me(c);
        if (!m) return V::nil();
        switch (game::con_level(s, m->level, 0)) {
        case game::Con::Gray: return V::string("GREY");
        case game::Con::Green: return V::string("GREEN");
        case game::Con::LightBlue: return V::string("LIGHT BLUE");
        case game::Con::DarkBlue: return V::string("BLUE");
        case game::Con::White: return V::string("WHITE");
        case game::Con::Yellow: return V::string("YELLOW");
        case game::Con::Red: return V::string("RED");
        case game::Con::None: break;
        }
        return V::nil();
    });
    spawn_member("Casting", [](const Context& c, const game::Spawn& s, std::string_view) -> V {
        if (is_me(c, s)) {
            const auto& cast = c.conn->spell_state().casting;
            return cast.casting() ? spell_value(c, cast.spell) : V::nil();
        }
        const auto& others = c.conn->spell_state().others;
        auto it = others.find(s.id);
        return it == others.end() ? V::nil() : spell_value(c, it->second.spell);
    });
    spawn_member("Guild", [](const Context&, const game::Spawn& s, std::string_view) {
        return s.guild_id == 0xffffffffu ? V::nil() : V::integer(s.guild_id);
    });
    spawn_member("Deity", [](const Context&, const game::Spawn& s, std::string_view) { return V::integer(s.deity); });
    spawn_member("BodyType", [](const Context&, const game::Spawn& s, std::string_view) { return V::integer(s.body_type); });
    spawn_member("xtarget", [](const Context& c, const game::Spawn& s, std::string_view) {
        for (const auto& x : c.conn->xtargets().slots)
            if (x.spawn_id == s.id) return V::boolean(true);
        return V::boolean(false);
    });
}

void register_character() {
    Type& t = const_cast<Type&>(*g.character);
    t.parent = g.spawn;
    t.to_value = [](const Context& c, const Value&) { return c.conn ? V::string(c.conn->character_name()) : V::nil(); };
    me_member("Name", [](const Context&, const GameConnection& c, std::string_view) { return V::string(c.character_name()); });
    me_member("Level", [](const Context&, const GameConnection& c, std::string_view) {
        return V::integer(c.player_status().level ? c.player_status().level : (c.profile() ? c.profile()->level : 0));
    });
    me_member("Class", [](const Context&, const GameConnection& c, std::string_view) {
        return c.profile() ? V::object(g.cls, c.profile()->class_) : V::nil();
    });
    me_member("Race", [](const Context&, const GameConnection& c, std::string_view) {
        return c.profile() ? V::object(g.race, c.profile()->race) : V::nil();
    });
    me_member("Deity", [](const Context&, const GameConnection& c, std::string_view) {
        return c.profile() ? V::integer(c.profile()->deity) : V::nil();
    });
    // Cur_HP and Max_HP, the items' HP included (GameConnection::own_hp), as the windows show them.
    me_member("PctHPs", [](const Context&, const GameConnection& c, std::string_view) {
        const auto hp = c.own_hp();
        return V::integer(hp.max > 0 ? hp.cur * 100 / hp.max : 0);
    });
    me_member("CurrentHPs", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.own_hp().cur); });
    me_member("MaxHPs", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.own_hp().max); });
    me_member("PctMana", [](const Context&, const GameConnection& c, std::string_view) {
        const auto& ps = c.player_status();
        return V::integer(ps.max_mana > 0 ? std::int64_t(ps.mana) * 100 / ps.max_mana : 0);
    });
    me_member("CurrentMana", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.player_status().mana); });
    me_member("MaxMana", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.player_status().max_mana); });
    me_member("PctEndurance", [](const Context&, const GameConnection& c, std::string_view) {
        const auto& ps = c.player_status();
        return V::integer(ps.max_endurance > 0 ? std::int64_t(ps.endurance) * 100 / ps.max_endurance : 0);
    });
    me_member("CurrentEndurance", [](const Context&, const GameConnection& c, std::string_view) {
        return V::integer(c.player_status().endurance);
    });
    me_member("MaxEndurance", [](const Context&, const GameConnection& c, std::string_view) {
        return V::integer(c.player_status().max_endurance);
    });
    me_member("Exp", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.player_status().exp); });
    me_member("PctExp", [](const Context&, const GameConnection& c, std::string_view) {
        return V::number(c.player_status().exp * 100.0 / 330.0);
    });
    me_member("AAExp", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.player_status().aa_exp); });
    me_member("PctAAExp", [](const Context&, const GameConnection& c, std::string_view) {
        return V::number(c.player_status().aa_exp * 100.0 / 330.0);
    });
    me_member("AAPoints", [](const Context&, const GameConnection& c, std::string_view) {
        return c.profile() ? V::integer(c.profile()->aa_points) : V::nil();
    });
    me_member("AAPointsSpent", [](const Context&, const GameConnection& c, std::string_view) {
        return c.profile() ? V::integer(c.profile()->aa_points_spent) : V::nil();
    });
    me_member("Buff", [](const Context& ctx, const GameConnection&, std::string_view index) -> V {
        if (index.empty()) return V::integer(static_cast<std::int64_t>(buff_slots(ctx, false).size()));
        return buff_by_index(ctx, index, false);
    });
    me_member("Song", [](const Context& ctx, const GameConnection&, std::string_view index) -> V {
        if (index.empty()) return V::integer(static_cast<std::int64_t>(buff_slots(ctx, true).size()));
        return buff_by_index(ctx, index, true);
    });
    me_member("CountBuffs", [](const Context& ctx, const GameConnection&, std::string_view) {
        return V::integer(static_cast<std::int64_t>(buff_slots(ctx, false).size()));
    });
    me_member("CountSongs", [](const Context& ctx, const GameConnection&, std::string_view) {
        return V::integer(static_cast<std::int64_t>(buff_slots(ctx, true).size()));
    });
    me_member("MaxBuffSlots", [](const Context&, const GameConnection&, std::string_view) { return V::integer(42); });
    me_member("FreeBuffSlots", [](const Context& ctx, const GameConnection&, std::string_view) {
        return V::integer(std::max<std::int64_t>(0, 42 - static_cast<std::int64_t>(buff_slots(ctx, false).size())));
    });
    me_member("Gem", [](const Context& ctx, const GameConnection& c, std::string_view index) -> V {
        const auto& gems = c.spell_state().gems;
        std::int64_t n = 0;
        if (parse_int(index, n)) {
            if (n < 1 || n > game::kSpellGems) return V::nil();
            return spell_value(ctx, gems[static_cast<std::size_t>(n - 1)].spell);
        }
        for (int i = 0; i < game::kSpellGems; ++i) {
            const game::SpellData* s = spell_data(ctx, gems[static_cast<std::size_t>(i)].spell);
            if (s && iequals(s->name, index)) return V::integer(i + 1);
        }
        return V::nil();
    });
    me_member("NumGems", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.usable_gems()); });
    me_member("Book", [](const Context& ctx, const GameConnection& c, std::string_view index) -> V {
        const auto& book = c.spell_state().book;
        std::int64_t n = 0;
        if (parse_int(index, n)) {
            if (n < 1 || n > static_cast<std::int64_t>(book.size())) return V::nil();
            return spell_value(ctx, book[static_cast<std::size_t>(n - 1)]);
        }
        for (std::size_t i = 0; i < book.size(); ++i) {
            const game::SpellData* s = spell_data(ctx, book[i]);
            if (s && iequals(s->name, index)) return V::integer(static_cast<std::int64_t>(i) + 1);
        }
        return V::nil();
    });
    me_member("SpellReady", [](const Context& ctx, const GameConnection& c, std::string_view index) -> V {
        const auto& st = c.spell_state();
        std::int64_t n = 0;
        int gem = -1;
        if (parse_int(index, n)) {
            gem = static_cast<int>(n) - 1;
        } else {
            for (int i = 0; i < game::kSpellGems; ++i) {
                const game::SpellData* s = spell_data(ctx, st.gems[static_cast<std::size_t>(i)].spell);
                if (s && iequals(s->name, index)) gem = i;
            }
        }
        if (gem < 0 || gem >= game::kSpellGems || st.gems[static_cast<std::size_t>(gem)].spell == game::kNoSpell)
            return V::boolean(false);
        return V::boolean(st.gem_castable(gem, GameConnection::eq_time()) && !st.casting.casting());
    });
    me_member("Casting", [](const Context& ctx, const GameConnection& c, std::string_view) -> V {
        const auto& cast = c.spell_state().casting;
        return cast.casting() ? spell_value(ctx, cast.spell) : V::nil();
    });
    me_member("Combat", [](const Context&, const GameConnection& c, std::string_view) { return V::boolean(c.auto_attack()); });
    me_member("CombatState", [](const Context&, const GameConnection& c, std::string_view) {
        if (c.in_combat()) return V::string("COMBAT");
        if (c.resting_for_fast_regen()) return V::string("RESTING");
        return V::string("ACTIVE");
    });
    me_member("Sitting", [](const Context&, const GameConnection& c, std::string_view) {
        return V::boolean(c.player_status().stand_state == 110);
    });
    me_member("Standing", [](const Context&, const GameConnection& c, std::string_view) {
        return V::boolean(c.player_status().stand_state == 100);
    });
    me_member("Ducking", [](const Context&, const GameConnection& c, std::string_view) {
        return V::boolean(c.player_status().stand_state == 111);
    });
    me_member("Feigning", [](const Context&, const GameConnection& c, std::string_view) {
        return V::boolean(c.player_status().stand_state == 115);
    });
    me_member("Dead", [](const Context&, const GameConnection& c, std::string_view) { return V::boolean(c.player_status().dead); });
    me_member("Stunned", [](const Context&, const GameConnection& c, std::string_view) { return V::boolean(c.stunned()); });
    me_member("Invis", [](const Context&, const GameConnection& c, std::string_view) { return V::boolean(c.invisible()); });
    me_member("Sneaking", [](const Context&, const GameConnection& c, std::string_view) { return V::boolean(c.sneaking()); });
    me_member("Moving", [](const Context&, const GameConnection& c, std::string_view) { return V::boolean(c.player_moving()); });
    me_member("Hunger", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.spell_state().hunger); });
    me_member("Thirst", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.spell_state().thirst); });
    me_member("Platinum", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.coins().platinum); });
    me_member("Gold", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.coins().gold); });
    me_member("Silver", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.coins().silver); });
    me_member("Copper", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.coins().copper); });
    me_member("Cash", [](const Context&, const GameConnection& c, std::string_view) {
        const auto& m = c.coins();
        return V::integer(std::int64_t(m.platinum) * 1000 + std::int64_t(m.gold) * 100 + std::int64_t(m.silver) * 10 + m.copper);
    });
    me_member("Grouped", [](const Context&, const GameConnection& c, std::string_view) { return V::boolean(c.group().active); });
    me_member("GroupSize", [](const Context& ctx, const GameConnection&, std::string_view) {
        return V::integer(static_cast<std::int64_t>(group_members(ctx).size()));
    });
    me_member("XTarget", [](const Context& ctx, const GameConnection& c, std::string_view index) -> V {
        const auto& slots = c.xtargets().slots;
        std::int64_t n = 0;
        if (index.empty()) {
            std::int64_t count = 0;
            for (const auto& x : slots) count += x.spawn_id != 0;
            return V::integer(count);
        }
        if (!parse_int(index, n) || n < 1 || n > static_cast<std::int64_t>(slots.size())) return V::nil();
        return spawn_value(spawn_of(ctx, slots[static_cast<std::size_t>(n - 1)].spawn_id));
    });
    me_member("Inventory", [](const Context& ctx, const GameConnection&, std::string_view index) -> V {
        int slot = slot_by_name(index);
        if (slot < 0) return V::nil();
        return item_value(ctx, net::ItemSlot::possessions(static_cast<std::int16_t>(slot)));
    });
    me_member("FreeInventory", [](const Context& ctx, const GameConnection& c, std::string_view) {
        std::int64_t free = 0;
        for (int s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s) {
            const net::Item* it = c.inventory().at(net::ItemSlot::possessions(static_cast<std::int16_t>(s)));
            if (!it) {
                ++free;
            } else if (it->is_bag()) {
                for (int i = 0; i < it->bag_slots; ++i)
                    if (!it->content(static_cast<std::uint32_t>(i))) ++free;
            }
        }
        (void)ctx;
        return V::integer(free);
    });
    me_member("Pet", [](const Context& ctx, const GameConnection& c, std::string_view) -> V {
        if (c.pets().focused) return spawn_value(spawn_of(ctx, c.pets().focused));
        for (const auto& [id, o] : c.world().spawns)
            if (o.pet_owner_id == c.world().player_id && !o.is_corpse) return spawn_value(&o);
        return V::nil();
    });
}

// Me.AbilityReady and the rest: an ability by name through Context::ability (the Ability Book's view).
std::optional<AbilityState> ability(const Context& c, AbilityKind kind, std::string_view name) {
    if (!c.ability || name.empty()) return std::nullopt;
    return c.ability(kind, name);
}

void register_abilities() {
    struct Ready { const char* name; AbilityKind kind; };
    for (Ready r : {Ready{"AbilityReady", AbilityKind::Skill}, Ready{"CombatAbilityReady", AbilityKind::Discipline},
                    Ready{"AltAbilityReady", AbilityKind::AltAbility}}) {
        add(g.character, r.name, [kind = r.kind](const Context& c, const Value&, std::string_view index) -> V {
            auto a = ability(c, kind, index);
            return V::boolean(a && a->ready);
        });
    }
    struct Timer { const char* name; AbilityKind kind; };
    for (Timer t : {Timer{"AbilityTimer", AbilityKind::Skill}, Timer{"CombatAbilityTimer", AbilityKind::Discipline},
                    Timer{"AltAbilityTimer", AbilityKind::AltAbility}}) {
        add(g.character, t.name, [kind = t.kind](const Context& c, const Value&, std::string_view index) -> V {
            auto a = ability(c, kind, index);
            return a ? V::object(g.timestamp, std::max<std::int64_t>(a->remaining_ms, 0)) : V::nil();
        });
    }
}

void register_small_types() {
    Type& cls = const_cast<Type&>(*g.cls);
    cls.to_value = [](const Context&, const Value& v) { return V::string(game::class_name(static_cast<std::uint32_t>(v.i))); };
    add(g.cls, "ID", [](const Context&, const Value& v, std::string_view) { return V::integer(v.i); });
    add(g.cls, "Name", [](const Context&, const Value& v, std::string_view) {
        return V::string(game::class_name(static_cast<std::uint32_t>(v.i)));
    });
    add(g.cls, "ShortName", [](const Context&, const Value& v, std::string_view) {
        return V::string(game::class_short_name(static_cast<std::uint32_t>(v.i)));
    });
    Type& race = const_cast<Type&>(*g.race);
    race.to_value = [](const Context&, const Value& v) { return V::string(game::race_name(static_cast<std::uint32_t>(v.i))); };
    add(g.race, "ID", [](const Context&, const Value& v, std::string_view) { return V::integer(v.i); });
    add(g.race, "Name", [](const Context&, const Value& v, std::string_view) {
        return V::string(game::race_name(static_cast<std::uint32_t>(v.i)));
    });

    Type& heading = const_cast<Type&>(*g.heading);
    auto short_name = [](double deg) {
        static const char* kNames[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                       "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
        return kNames[heading_point(deg)];
    };
    heading.to_value = [short_name](const Context&, const Value& v) { return V::string(short_name(v.f)); };
    add(g.heading, "Degrees", [](const Context&, const Value& v, std::string_view) { return V::number(v.f); });
    add(g.heading, "DegreesCCW", [](const Context&, const Value& v, std::string_view) {
        return V::number(v.f == 0 ? 0 : 360.0 - v.f);
    });
    add(g.heading, "ShortName", [short_name](const Context&, const Value& v, std::string_view) { return V::string(short_name(v.f)); });
    add(g.heading, "Name", [](const Context&, const Value& v, std::string_view) {
        static const char* kNames[] = {"north", "north by northeast", "northeast", "east by northeast",
                                       "east", "east by southeast", "southeast", "south by southeast",
                                       "south", "south by southwest", "southwest", "west by southwest",
                                       "west", "west by northwest", "northwest", "north by northwest"};
        return V::string(kNames[heading_point(v.f)]);
    });
    add(g.heading, "Clock", [](const Context&, const Value& v, std::string_view) {
        int h = static_cast<int>(std::lround(v.f / 30.0)) % 12;
        return V::integer(h == 0 ? 12 : h);
    });

    Type& ts = const_cast<Type&>(*g.timestamp);
    ts.to_value = [](const Context&, const Value& v) { return V::integer(v.i); };
    add(g.timestamp, "Raw", [](const Context&, const Value& v, std::string_view) { return V::integer(v.i); });
    add(g.timestamp, "TotalSeconds", [](const Context&, const Value& v, std::string_view) { return V::integer(v.i / 1000); });
    add(g.timestamp, "TotalMinutes", [](const Context&, const Value& v, std::string_view) { return V::integer(v.i / 60000); });
    add(g.timestamp, "Seconds", [](const Context&, const Value& v, std::string_view) { return V::integer(v.i / 1000 % 60); });
    add(g.timestamp, "Minutes", [](const Context&, const Value& v, std::string_view) { return V::integer(v.i / 60000 % 60); });
    add(g.timestamp, "Hours", [](const Context&, const Value& v, std::string_view) { return V::integer(v.i / 3600000); });
    add(g.timestamp, "Ticks", [](const Context&, const Value& v, std::string_view) { return V::integer((v.i + 5999) / 6000); });
    add(g.timestamp, "TimeHMS", [](const Context&, const Value& v, std::string_view) {
        std::int64_t s = v.i / 1000;
        return V::string(std::format("{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60));
    });
    add(g.timestamp, "Time", [](const Context&, const Value& v, std::string_view) {
        std::int64_t s = v.i / 1000;
        return V::string(std::format("{}:{:02}", s / 60, s % 60));
    });
}

void register_spell() {
    Type& t = const_cast<Type&>(*g.spell);
    t.to_value = [](const Context& c, const Value& v) {
        const game::SpellData* s = spell_data(c, v.i);
        return s ? V::string(s->name) : V::nil();
    };
    auto m = [](const char* name, auto fn) {
        add(g.spell, name, [fn](const Context& c, const Value& v, std::string_view index) -> V {
            const game::SpellData* s = spell_data(c, v.i);
            return s ? fn(c, *s, index) : V::nil();
        });
    };
    m("ID", [](const Context&, const game::SpellData& s, std::string_view) { return V::integer(s.id); });
    m("Name", [](const Context&, const game::SpellData& s, std::string_view) { return V::string(s.name); });
    m("Range", [](const Context&, const game::SpellData& s, std::string_view) { return V::number(s.range); });
    m("AERange", [](const Context&, const game::SpellData& s, std::string_view) { return V::number(s.ae_range); });
    m("Mana", [](const Context&, const game::SpellData& s, std::string_view) { return V::integer(s.mana); });
    m("CastTime", [](const Context&, const game::SpellData& s, std::string_view) { return V::number(s.cast_time / 1000.0); });
    m("MyCastTime", [](const Context&, const game::SpellData& s, std::string_view) { return V::object(g.timestamp, s.cast_time); });
    m("RecastTime", [](const Context&, const game::SpellData& s, std::string_view) { return V::number(s.recast_time / 1000.0); });
    m("RecoveryTime", [](const Context&, const game::SpellData& s, std::string_view) {
        return V::number(s.recovery_time / 1000.0);
    });
    m("Duration", [](const Context&, const game::SpellData& s, std::string_view) {
        return V::object(g.timestamp, static_cast<std::int64_t>(s.duration_cap) * 6000);
    });
    m("Level", [](const Context& c, const game::SpellData& s, std::string_view) -> V {
        if (!c.conn || !c.conn->profile()) return V::nil();
        int cls = c.conn->profile()->class_;
        if (cls < 1 || cls > 16) return V::nil();
        auto lvl = s.class_level[static_cast<std::size_t>(cls - 1)];
        return lvl == 255 ? V::nil() : V::integer(lvl);
    });
    m("TargetType", [](const Context&, const game::SpellData& s, std::string_view) { return V::string(target_type_name(s.target_type)); });
    m("SpellType", [](const Context&, const game::SpellData& s, std::string_view) {
        return V::string(s.spell_type == 0 ? "Detrimental" : "Beneficial");
    });
    m("Beneficial", [](const Context&, const game::SpellData& s, std::string_view) { return V::boolean(s.spell_type != 0); });
    m("ResistType", [](const Context&, const game::SpellData& s, std::string_view) { return V::string(resist_name(s.resist)); });
    m("ResistAdj", [](const Context&, const game::SpellData& s, std::string_view) { return V::integer(s.resist_adj); });
    m("Skill", [](const Context&, const game::SpellData& s, std::string_view) { return V::integer(s.skill); });
    m("SpellIcon", [](const Context&, const game::SpellData& s, std::string_view) { return V::integer(s.spell_icon); });
    m("GemIcon", [](const Context&, const game::SpellData& s, std::string_view) { return V::integer(s.gem_icon); });
    m("Category", [](const Context&, const game::SpellData& s, std::string_view) { return V::integer(s.category); });

    Type& b = const_cast<Type&>(*g.buff);
    b.parent = g.spell;
    b.convert = [](const Context& c, const Value& v) {
        const game::BuffSlot* s = buff_of(c, v);
        return s ? V::object(g.spell, s->spell) : V::nil();
    };
    b.to_value = [](const Context& c, const Value& v) {
        const game::BuffSlot* s = buff_of(c, v);
        const game::SpellData* d = s ? spell_data(c, s->spell) : nullptr;
        return d ? V::string(d->name) : V::nil();
    };
    auto bm = [](const char* name, auto fn) {
        add(g.buff, name, [fn](const Context& c, const Value& v, std::string_view index) -> V {
            const game::BuffSlot* s = buff_of(c, v);
            return s ? fn(c, v, *s, index) : V::nil();
        });
    };
    bm("ID", [](const Context& c, const Value& v, const game::BuffSlot&, std::string_view) -> V {
        if (v.f == 0) return V::integer(v.i + 1);
        return V::integer(std::int64_t{c.conn->spell_state().target_buffs[static_cast<std::size_t>(v.i)].first} + 1);
    });
    bm("Spell", [](const Context& c, const Value&, const game::BuffSlot& s, std::string_view) { return spell_value(c, s.spell); });
    bm("Level", [](const Context&, const Value&, const game::BuffSlot& s, std::string_view) { return V::integer(s.level); });
    bm("Caster", [](const Context&, const Value&, const game::BuffSlot& s, std::string_view) { return V::string(s.caster_name); });
    bm("Counters", [](const Context&, const Value&, const game::BuffSlot& s, std::string_view) { return V::integer(s.num_hits); });
    bm("HitCount", [](const Context&, const Value&, const game::BuffSlot& s, std::string_view) { return V::integer(s.num_hits); });
    bm("Mod", [](const Context&, const Value&, const game::BuffSlot& s, std::string_view) { return V::number(s.modifier); });
    bm("Duration", [](const Context& c, const Value&, const game::BuffSlot& s, std::string_view) -> V {
        std::int64_t ms = buff_ms_left(c, s);
        return ms < 0 ? V::nil() : V::object(g.timestamp, ms);
    });
    bm("Dur", [](const Context& c, const Value&, const game::BuffSlot& s, std::string_view) -> V {
        std::int64_t ms = buff_ms_left(c, s);
        return ms < 0 ? V::nil() : V::object(g.timestamp, ms);
    });
}

void register_item() {
    Type& t = const_cast<Type&>(*g.item);
    t.to_value = [](const Context& c, const Value& v) {
        const net::Item* it = item_of(c, v);
        return it ? V::string(it->name) : V::nil();
    };
    auto m = [](const char* name, auto fn) {
        add(g.item, name, [fn](const Context& c, const Value& v, std::string_view index) -> V {
            const net::Item* it = item_of(c, v);
            return it ? fn(c, v, *it, index) : V::nil();
        });
    };
    m("ID", [](const Context&, const Value&, const net::Item& it, std::string_view) { return V::integer(it.id); });
    m("Name", [](const Context&, const Value&, const net::Item& it, std::string_view) { return V::string(it.name); });
    m("Lore", [](const Context&, const Value&, const net::Item& it, std::string_view) { return V::boolean(!it.lore.empty() && it.lore[0] == '*'); });
    m("Stack", [](const Context&, const Value&, const net::Item& it, std::string_view) { return V::integer(it.stack_count); });
    m("Stackable", [](const Context&, const Value&, const net::Item& it, std::string_view) { return V::boolean(it.stackable()); });
    m("StackSize", [](const Context&, const Value&, const net::Item& it, std::string_view) { return V::integer(it.max_stack); });
    m("Charges", [](const Context&, const Value&, const net::Item& it, std::string_view) {
        return V::integer(it.charges == 0xffffffffu ? -1 : static_cast<std::int64_t>(it.charges));
    });
    m("TimerReady", [](const Context& c, const Value&, const net::Item& it, std::string_view) -> V {
        // MQ2ItemType's TimerReady: an int, the seconds left (GetItemTimer: GetItemRecastTimer less the fast time,
        // 0 once passed), NULL for an item without a click effect. MQ gives 0 for a click without a recast type
        // (TimerID -1); here the item's own timer still counts, as GetItemRecastTimer reads it.
        if (!c.conn || !it.effects[net::kEffectClick].present()) return V::nil();
        const auto ready_at = static_cast<std::int64_t>(c.conn->item_recast_timer(&it));
        const auto now = static_cast<std::int64_t>(std::time(nullptr));
        return V::integer(ready_at > now ? ready_at - now : 0);
    });
    m("Icon", [](const Context&, const Value&, const net::Item& it, std::string_view) { return V::integer(it.icon); });
    m("Weight", [](const Context&, const Value&, const net::Item& it, std::string_view) { return V::number(it.weight / 10.0); });
    m("NoDrop", [](const Context&, const Value&, const net::Item& it, std::string_view) { return V::boolean(it.no_drop == 0); });
    m("NoRent", [](const Context&, const Value&, const net::Item& it, std::string_view) { return V::boolean(it.no_rent == 0); });
    m("Value", [](const Context&, const Value&, const net::Item& it, std::string_view) { return V::integer(it.value); });
    m("Type", [](const Context&, const Value&, const net::Item& it, std::string_view) {
        return V::string(it.is_bag() ? "Container" : item_type_name(it.item_type));
    });
    m("Container", [](const Context&, const Value&, const net::Item& it, std::string_view) {
        return V::integer(it.is_bag() ? it.bag_slots : 0);
    });
    m("Items", [](const Context&, const Value&, const net::Item& it, std::string_view) {
        return V::integer(static_cast<std::int64_t>(it.contents.size()));
    });
    m("Item", [](const Context& c, const Value& v, const net::Item& it, std::string_view index) -> V {
        std::int64_t n = 0;
        net::ItemSlot at = unpack_slot(v.i);
        if (!parse_int(index, n) || n < 1 || at.sub >= 0 || !it.is_bag()) return V::nil();
        net::ItemSlot inner = at;
        inner.sub = static_cast<std::int16_t>(n - 1);
        return item_value(c, inner);
    });
    m("ItemSlot", [](const Context&, const Value& v, const net::Item&, std::string_view) { return V::integer(unpack_slot(v.i).slot); });
    m("ItemSlot2", [](const Context&, const Value& v, const net::Item&, std::string_view) { return V::integer(unpack_slot(v.i).sub); });
}

void register_group() {
    Type& grp = const_cast<Type&>(*g.group);
    grp.to_value = [](const Context& c, const Value&) {
        auto n = static_cast<std::int64_t>(group_members(c).size());
        return V::integer(n ? n - 1 : 0);
    };
    add(g.group, "Members", [](const Context& c, const Value&, std::string_view) {
        auto n = static_cast<std::int64_t>(group_members(c).size());
        return V::integer(n ? n - 1 : 0);
    });
    add(g.group, "GroupSize", [](const Context& c, const Value&, std::string_view) {
        return V::integer(static_cast<std::int64_t>(group_members(c).size()));
    });
    add(g.group, "Member", [](const Context& c, const Value&, std::string_view index) -> V {
        std::int64_t n = 0;
        if (parse_int(index, n)) return member_value(c, static_cast<int>(n));
        auto members = group_members(c);
        for (std::size_t i = 0; i < members.size(); ++i) {
            std::string name = members[i] ? members[i]->name : c.conn->character_name();
            if (iequals(name, index)) return V::integer(static_cast<std::int64_t>(i));
        }
        return V::nil();
    });
    add(g.group, "Leader", [](const Context& c, const Value&, std::string_view) -> V {
        auto members = group_members(c);
        for (std::size_t i = 0; i < members.size(); ++i) {
            std::string name = members[i] ? members[i]->name : c.conn->character_name();
            if (c.conn->group().is_leader(name)) return member_value(c, static_cast<int>(i));
        }
        return V::nil();
    });
    auto role = [](bool game::GroupMember::*flag) {
        return [flag](const Context& c, const Value&, std::string_view) -> V {
            auto members = group_members(c);
            for (std::size_t i = 0; i < members.size(); ++i)
                if (members[i] && members[i]->*flag) return member_value(c, static_cast<int>(i));
            return V::nil();
        };
    };
    add(g.group, "MainTank", role(&game::GroupMember::main_tank));
    add(g.group, "MainAssist", role(&game::GroupMember::main_assist));
    add(g.group, "Puller", role(&game::GroupMember::puller));

    Type& mem = const_cast<Type&>(*g.member);
    mem.parent = g.spawn;
    mem.convert = [](const Context& c, const Value& v) { return spawn_value(spawn_named(c, v.s)); };
    mem.to_value = [](const Context&, const Value& v) { return V::string(v.s); };
    add(g.member, "Name", [](const Context&, const Value& v, std::string_view) { return V::string(v.s); });
    add(g.member, "Index", [](const Context&, const Value& v, std::string_view) { return V::integer(v.i); });
    add(g.member, "Spawn", [](const Context& c, const Value& v, std::string_view) { return spawn_value(spawn_named(c, v.s)); });
    add(g.member, "Leader", [](const Context& c, const Value& v, std::string_view) {
        return V::boolean(c.conn && c.conn->group().is_leader(v.s));
    });
    add(g.member, "Level", [](const Context& c, const Value& v, std::string_view) -> V {
        if (const game::Spawn* s = spawn_named(c, v.s)) return V::integer(s->level);
        const game::GroupMember* m = member_of(c, v);
        return m ? V::integer(m->level) : V::nil();
    });
    add(g.member, "Mercenary", [](const Context& c, const Value& v, std::string_view) {
        const game::GroupMember* m = member_of(c, v);
        return V::boolean(m && m->merc);
    });
    add(g.member, "Offline", [](const Context& c, const Value& v, std::string_view) {
        const game::GroupMember* m = member_of(c, v);
        return V::boolean(m && m->offline);
    });
    add(g.member, "OtherZone", [](const Context& c, const Value& v, std::string_view) {
        const game::GroupMember* m = member_of(c, v);
        return V::boolean(m && !m->offline && !spawn_named(c, v.s));
    });
    add(g.member, "MainTank", [](const Context& c, const Value& v, std::string_view) {
        const game::GroupMember* m = member_of(c, v);
        return V::boolean(m && m->main_tank);
    });
    add(g.member, "MainAssist", [](const Context& c, const Value& v, std::string_view) {
        const game::GroupMember* m = member_of(c, v);
        return V::boolean(m && m->main_assist);
    });
    add(g.member, "Puller", [](const Context& c, const Value& v, std::string_view) {
        const game::GroupMember* m = member_of(c, v);
        return V::boolean(m && m->puller);
    });
}

void register_misc_types() {
    Type& zone = const_cast<Type&>(*g.zone);
    zone.to_value = [](const Context& c, const Value&) {
        return c.conn ? V::string(c.conn->world().zone_long_name) : V::nil();
    };
    add(g.zone, "ID", [](const Context& c, const Value&, std::string_view) { return c.conn ? V::integer(c.conn->zone_id()) : V::nil(); });
    add(g.zone, "Name", [](const Context& c, const Value&, std::string_view) {
        return c.conn ? V::string(c.conn->world().zone_long_name) : V::nil();
    });
    add(g.zone, "ShortName", [](const Context& c, const Value&, std::string_view) {
        return c.conn ? V::string(c.conn->world().zone_short_name) : V::nil();
    });

    Type& eq = const_cast<Type&>(*g.everquest);
    eq.to_value = [](const Context&, const Value&) { return V::string("EverQuest"); };
    add(g.everquest, "GameState", [](const Context& c, const Value&, std::string_view) -> V {
        if (!c.conn) return V::string("PRECHARSELECT");
        switch (c.conn->state()) {
        case net::ConnectionState::InZone:
        case net::ConnectionState::Camping:
        case net::ConnectionState::Zoning:
        case net::ConnectionState::ZoneConnecting:
        case net::ConnectionState::ZoneEntering: return V::string("INGAME");
        case net::ConnectionState::CharacterSelect:
        case net::ConnectionState::EnteringWorld:
        case net::ConnectionState::Creating: return V::string("CHARSELECT");
        default: return V::string("PRECHARSELECT");
        }
    });
    add(g.everquest, "Server", [](const Context& c, const Value&, std::string_view) { return V::string(c.server_name); });
    add(g.everquest, "UIScale", [](const Context&, const Value&, std::string_view) { return V::number(1.0); });

    Type& win = const_cast<Type&>(*g.window);
    win.to_value = [](const Context& c, const Value& v) {
        return V::string(c.window_open && c.window_open(v.s) ? "TRUE" : "FALSE");
    };
    add(g.window, "Open", [](const Context& c, const Value& v, std::string_view) {
        return V::boolean(c.window_open && c.window_open(v.s));
    });
    add(g.window, "Name", [](const Context&, const Value& v, std::string_view) { return V::string(v.s); });

    Type& plug = const_cast<Type&>(*g.plugin);
    plug.to_value = [](const Context& c, const Value& v) {
        return c.plugin_loaded && c.plugin_loaded(v.s) ? V::string(v.s) : V::nil();
    };
    add(g.plugin, "IsLoaded", [](const Context& c, const Value& v, std::string_view) {
        return V::boolean(c.plugin_loaded && c.plugin_loaded(v.s));
    });
    add(g.plugin, "Name", [](const Context& c, const Value& v, std::string_view) {
        return c.plugin_loaded && c.plugin_loaded(v.s) ? V::string(v.s) : V::nil();
    });

    // Time: the computer's local time (MQ's Time TLO); i = Unix seconds.
    auto tm_of = [](const Value& v) {
        std::time_t t = static_cast<std::time_t>(v.i);
        std::tm out{};
#ifdef _WIN32
        localtime_s(&out, &t);
#else
        localtime_r(&t, &out);
#endif
        return out;
    };
    Type& time = const_cast<Type&>(*g.time);
    time.to_value = [tm_of](const Context&, const Value& v) {
        std::tm t = tm_of(v);
        return V::string(std::format("{:02}:{:02}:{:02}", t.tm_hour, t.tm_min, t.tm_sec));
    };
    auto tf = [tm_of](const char* name, auto fn) {
        add(g.time, name, [tm_of, fn](const Context&, const Value& v, std::string_view) { return fn(tm_of(v)); });
    };
    tf("Hour", [](const std::tm& t) { return V::integer(t.tm_hour); });
    tf("Minute", [](const std::tm& t) { return V::integer(t.tm_min); });
    tf("Second", [](const std::tm& t) { return V::integer(t.tm_sec); });
    tf("Day", [](const std::tm& t) { return V::integer(t.tm_mday); });
    tf("Month", [](const std::tm& t) { return V::integer(t.tm_mon + 1); });
    tf("Year", [](const std::tm& t) { return V::integer(t.tm_year + 1900); });
    tf("DayOfWeek", [](const std::tm& t) { return V::integer(t.tm_wday + 1); });
    tf("Time24", [](const std::tm& t) { return V::string(std::format("{:02}:{:02}:{:02}", t.tm_hour, t.tm_min, t.tm_sec)); });
    tf("Date", [](const std::tm& t) { return V::string(std::format("{:02}/{:02}/{:04}", t.tm_mon + 1, t.tm_mday, t.tm_year + 1900)); });
    tf("SecondsSinceMidnight", [](const std::tm& t) { return V::integer(t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec); });
    add(g.time, "Night", [tm_of](const Context&, const Value& v, std::string_view) {
        std::tm t = tm_of(v);
        return V::boolean(t.tm_hour < 6 || t.tm_hour >= 19);
    });

    // GameTime: Norrath's clock, advancing between OP_TimeOfDay packets.
    Type& gt = const_cast<Type&>(*g.gametime);
    gt.to_value = [](const Context& c, const Value&) -> V {
        const auto t = c.conn ? c.conn->current_time_of_day() : std::nullopt;
        if (!t) return V::nil();
        return V::string(std::format("{:02}:{:02}", t->hour, t->minute));
    };
    auto gf = [](const char* name, auto fn) {
        add(g.gametime, name, [fn](const Context& c, const Value&, std::string_view) -> V {
            const auto t = c.conn ? c.conn->current_time_of_day() : std::nullopt;
            return t ? fn(*t) : V::nil();
        });
    };
    gf("Hour", [](const net::TimeOfDay& t) { return V::integer(t.hour); });
    gf("Minute", [](const net::TimeOfDay& t) { return V::integer(t.minute); });
    gf("Day", [](const net::TimeOfDay& t) { return V::integer(t.day); });
    gf("Month", [](const net::TimeOfDay& t) { return V::integer(t.month); });
    gf("Year", [](const net::TimeOfDay& t) { return V::integer(t.year); });
    gf("Night", [](const net::TimeOfDay& t) { return V::boolean(t.hour < 6 || t.hour >= 19); });
    gf("Time24", [](const net::TimeOfDay& t) { return V::string(std::format("{:02}:{:02}", t.hour, t.minute)); });
}

} // namespace

// MQ2Nav's destination syntax in an index: "id <#>", "target", "loc <y> <x> <z>", "locxyz <x> <y> <z>", or a spawn
// search. World coordinates.
std::optional<glm::vec3> nav_destination(const Context& c, std::string_view index) {
    if (!c.conn) return std::nullopt;
    std::vector<std::string> w;
    for (std::size_t i = 0; i < index.size();) {
        while (i < index.size() && (index[i] == ' ' || index[i] == ',')) ++i;
        std::size_t j = i;
        while (j < index.size() && index[j] != ' ' && index[j] != ',') ++j;
        if (j > i) w.emplace_back(index.substr(i, j - i));
        i = j;
    }
    if (w.empty()) return std::nullopt;
    auto read = [&](std::size_t i) -> std::optional<double> {
        double d = 0;
        if (i >= w.size() || !parse_float(w[i], d)) return std::nullopt;
        return d;
    };
    auto num = [&](std::size_t i) { return read(i) ? in_range<float>(*read(i)) : std::nullopt; };
    const game::Spawn* s = nullptr;
    if (iequals(w[0], "loc") || iequals(w[0], "locyxz") || iequals(w[0], "locxyz")) {
        auto a = num(1), b = num(2), z = num(3);
        if (!a || !b || !z) return std::nullopt;
        return game::server_to_world(iequals(w[0], "locxyz") ? glm::vec3(*a, *b, *z) : glm::vec3(*b, *a, *z));
    }
    if (iequals(w[0], "id")) {
        if (auto id = read(1) ? in_range<std::uint32_t>(*read(1)) : std::nullopt) s = spawn_of(c, *id);
    } else if (iequals(w[0], "target")) {
        s = spawn_of(c, c.conn->world().target_id);
    } else {
        for (const game::Spawn* f : find_spawns(index, *c.conn, c.can_see))
            if (!is_me(c, *f)) {
                s = f;
                break;
            }
    }
    if (!s) return std::nullopt;
    return spawn_position(*s) - glm::vec3(0.0f, 0.0f, s->heights.avatar_height);
}

// The Navigation TLO (MQ2Nav's members, over the client's navigation: nav/nav_control.h).
void register_navigation() {
    auto status = [](const Context& c) { return c.nav ? std::optional(c.nav->status()) : std::nullopt; };
    add(g.navigation, "Active", [status](const Context& c, const Value&, std::string_view) {
        auto s = status(c);
        return V::boolean(s && s->active);
    });
    add(g.navigation, "Paused", [status](const Context& c, const Value&, std::string_view) {
        auto s = status(c);
        return V::boolean(s && s->paused);
    });
    add(g.navigation, "MeshLoaded", [status](const Context& c, const Value&, std::string_view) {
        auto s = status(c);
        return V::boolean(s && s->mesh_loaded);
    });
    // Hailstone's own: the mesh's build and the walk's goal.
    add(g.navigation, "MeshComplete", [status](const Context& c, const Value&, std::string_view) {
        auto s = status(c);
        return V::boolean(s && s->mesh_complete);
    });
    add(g.navigation, "TilesBuilt", [status](const Context& c, const Value&, std::string_view) -> V {
        auto s = status(c);
        return s ? V::integer(static_cast<std::int64_t>(s->tiles_done)) : V::nil();
    });
    add(g.navigation, "TilesTotal", [status](const Context& c, const Value&, std::string_view) -> V {
        auto s = status(c);
        return s ? V::integer(static_cast<std::int64_t>(s->tiles_total)) : V::nil();
    });
    add(g.navigation, "Destination", [status](const Context& c, const Value&, std::string_view) -> V {
        auto s = status(c);
        return s && s->active ? V::string(s->goal) : V::nil();
    });
    add(g.navigation, "Remaining", [status](const Context& c, const Value&, std::string_view) -> V {
        auto s = status(c);
        return s && s->active ? V::number(s->remaining) : V::nil();
    });
    add(g.navigation, "PathExists", [](const Context& c, const Value&, std::string_view index) {
        auto to = nav_destination(c, index);
        return V::boolean(c.nav && to && c.nav->path_length(*to).has_value());
    });
    add(g.navigation, "PathLength", [](const Context& c, const Value&, std::string_view index) -> V {
        auto to = nav_destination(c, index);
        std::optional<float> len = c.nav && to ? c.nav->path_length(*to) : std::nullopt;
        // MQ2Nav answers -1 when there is no path.
        return V::number(len ? *len : -1.0);
    });
    Type& t = const_cast<Type&>(*g.navigation);
    t.to_value = [status](const Context& c, const Value&) {
        auto s = status(c);
        return V::boolean(s && s->active);
    };
}

// The Stick TLO (MQ2MoveUtils' members, over NavControl::stick): Active, Status ("ON", "PAUSED", "OFF"), Distance,
// StickTarget (the spawn's id), StickTargetName.
void register_stick() {
    auto status = [](const Context& c) { return c.nav ? std::optional(c.nav->status()) : std::nullopt; };
    auto on = [status](const Context& c) {
        auto s = status(c);
        return s && s->stick_spawn != 0 ? s : std::nullopt;
    };
    add(g.stick, "Active", [on](const Context& c, const Value&, std::string_view) { return V::boolean(on(c).has_value()); });
    add(g.stick, "Status", [on](const Context& c, const Value&, std::string_view) {
        auto s = on(c);
        return V::string(!s ? "OFF" : s->paused ? "PAUSED" : "ON");
    });
    add(g.stick, "Distance", [on](const Context& c, const Value&, std::string_view) -> V {
        auto s = on(c);
        return s ? V::number(s->stick_distance) : V::nil();
    });
    add(g.stick, "StickTarget", [on](const Context& c, const Value&, std::string_view) -> V {
        auto s = on(c);
        return s ? V::integer(s->stick_spawn) : V::integer(0);
    });
    add(g.stick, "StickTargetName", [on](const Context& c, const Value&, std::string_view) -> V {
        auto s = on(c);
        const game::Spawn* sp = s ? spawn_of(c, s->stick_spawn) : nullptr;
        return sp ? V::string(sp->name) : V::nil();
    });
    Type& t = const_cast<Type&>(*g.stick);
    t.to_value = [on](const Context& c, const Value&) { return V::string(on(c) ? "ON" : "OFF"); };
}

// MQ's DegToRad is 180/pi: Sin[x] takes degrees, Asin gives them.
constexpr double kDegToRad = 57.295779513082320876846364344191;

// MQ's GetVec3FromString: up to three numbers split by commas (or, with no comma, by spaces); a missing one is the
// default's, an empty one gives back the whole default.
static glm::vec3 vec3_from_text(std::string_view s, const glm::vec3& fail) {
    const auto trim_blank = [](std::string_view t) {
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.remove_prefix(1);
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.remove_suffix(1);
        return t;
    };
    s = trim_blank(s);
    glm::vec3 out = fail;
    const char delim = s.find(',') != std::string_view::npos ? ',' : ' ';
    std::size_t start = 0;
    for (int k = 0; k < 3 && start != std::string_view::npos; ++k) {
        const std::size_t end = s.find(delim, start);
        const std::string_view part = trim_blank(s.substr(start, end == std::string_view::npos ? s.npos : end - start));
        if (part.empty()) return fail;
        out[k] = static_cast<float>(double_from_text(part, fail[k]));
        if (end == std::string_view::npos) break;
        start = s.find_first_not_of(' ', end + 1);
    }
    return out;
}

// MQ's Math, If and Range (MQ2MathType.cpp, MQ2Data.cpp dataIf, MQ2BasicTypes.cpp MQ2RangeType as the
// specification), over mq/calc.h. Math's and Range's members are NULL without an index.
static void register_math(Registry& r) {
    Type& math = r.type("math");
    const Type* math_type = &math;
    math.to_value = [](const Context&, const Value&) { return V::nil(); }; // ${Math} alone is NULL
    // A Calculate result as MQ's float type keeps it (Dest.Float = (float)result).
    const auto calc_member = [&math](const char* name, double (*fn)(double)) {
        math.members[name] = [fn](const Context&, const Value&, std::string_view index) -> V {
            if (index.empty()) return V::nil();
            const auto v = calculate(index);
            return v ? V::number(static_cast<double>(static_cast<float>(fn(*v)))) : V::nil();
        };
    };
    calc_member("Calc", [](double x) { return x; });
    calc_member("Sqrt", [](double x) { return std::sqrt(x); });
    calc_member("Sin", [](double x) { return std::sin(x / kDegToRad); });
    calc_member("Cos", [](double x) { return std::cos(x / kDegToRad); });
    calc_member("Tan", [](double x) { return std::tan(x / kDegToRad); });
    calc_member("Asin", [](double x) { return std::asin(x) * kDegToRad; });
    calc_member("Acos", [](double x) { return std::acos(x) * kDegToRad; });
    calc_member("Atan", [](double x) { return std::atan(x) * kDegToRad; });
    math.members["Abs"] = [](const Context&, const Value&, std::string_view index) -> V {
        if (index.empty()) return V::nil();
        const auto v = calculate(index);
        if (!v) return V::nil();
        float f = static_cast<float>(*v);
        if (f < 0) f = -f; // (-0 stays -0, as in MQ)
        return V::number(f);
    };
    // Rand[max] or Rand[min,max]: a whole number from min (0) to max, both included; NULL when min > max.
    math.members["Rand"] = [](const Context&, const Value&, std::string_view index) -> V {
        if (index.empty()) return V::nil();
        int lo = 0, hi = 0;
        if (const auto comma = index.find(','); comma != std::string_view::npos) {
            lo = int_from_text(index.substr(0, comma), 0);
            hi = int_from_text(index.substr(comma + 1), 0);
        } else {
            hi = int_from_text(index, 0);
        }
        if (lo > hi) return V::nil();
        thread_local std::mt19937 rng{std::random_device{}()};
        return V::integer(std::uniform_int_distribution<int>(lo, hi)(rng));
    };
    math.members["Not"] = [](const Context&, const Value&, std::string_view index) -> V {
        if (index.empty()) return V::nil();
        return V::integer(~int_from_text(index, 0));
    };
    math.members["Hex"] = [](const Context&, const Value&, std::string_view index) -> V {
        if (index.empty()) return V::nil();
        return V::string(std::format("0x{:X}", static_cast<std::uint32_t>(int_from_text(index, 0))));
    };
    // Dec[hex]: MQ's sscanf "%x": blanks, a sign, an optional 0x, hex digits; NULL when there are none. Past 32 bits
    // it is held at 0xFFFFFFFF, strtoul's way (docs/unknowns.md: what MSVC's scanf does there is unconfirmed).
    math.members["Dec"] = [](const Context&, const Value&, std::string_view index) -> V {
        const auto hex = [&index](std::size_t at) {
            return at < index.size() && std::isxdigit(static_cast<unsigned char>(index[at])) != 0;
        };
        std::size_t i = 0;
        while (i < index.size() && std::isspace(static_cast<unsigned char>(index[i]))) ++i;
        bool negative = false;
        if (i < index.size() && (index[i] == '-' || index[i] == '+')) negative = index[i++] == '-';
        if (i + 1 < index.size() && index[i] == '0' && (index[i + 1] == 'x' || index[i + 1] == 'X') && hex(i + 2)) i += 2;
        std::uint64_t v = 0;
        bool any = false;
        for (; hex(i); ++i) {
            const int ch = std::tolower(static_cast<unsigned char>(index[i]));
            const auto digit = static_cast<std::uint64_t>(ch <= '9' ? ch - '0' : ch - 'a' + 10);
            v = std::min<std::uint64_t>(v * 16 + digit, 0x1'0000'0000ull);
            any = true;
        }
        if (!any) return V::nil();
        auto u = static_cast<std::uint32_t>(std::min<std::uint64_t>(v, 0xFFFF'FFFFull));
        if (negative) u = 0u - u;
        return V::integer(static_cast<std::int32_t>(u));
    };
    // Clamp[n,min,max]: arguments split by commas or blanks; min wins when min > max.
    math.members["Clamp"] = [](const Context&, const Value&, std::string_view index) -> V {
        if (index.empty()) return V::nil();
        const int n = int_from_text(get_arg(index, 1, true), 0);
        const int lo = int_from_text(get_arg(index, 2, true), 0);
        const int hi = int_from_text(get_arg(index, 3, true), 0);
        return V::integer(std::max(lo, std::min(n, hi)));
    };
    // Distance[y,x,z:y,x,z]: 3D, in MQ's Loc order (the server's coordinates); a missing coordinate, or the whole
    // second point, is the player's.
    math.members["Distance"] = [](const Context& c, const Value&, std::string_view index) -> V {
        const game::Spawn* m = me(c);
        if (index.empty() || !m) return V::nil();
        const glm::vec3 sp = game::world_to_server(spawn_position(*m));
        const glm::vec3 player{sp.y, sp.x, sp.z};
        const auto colon = index.find(':');
        const glm::vec3 a = vec3_from_text(index.substr(0, colon), player);
        const glm::vec3 b = colon == std::string_view::npos ? player : vec3_from_text(index.substr(colon + 1), player);
        return V::number(glm::distance(a, b));
    };
    r.add_tlo("Math", [math_type](const Context&, std::string_view) { return V::object(math_type, 0); });

    // If[condition,whentrue,whenfalse], or with exactly two '~' If[condition~whentrue~whenfalse]: the text of one
    // branch (the false one runs to the end, commas included); NULL when the condition does not calculate.
    r.add_tlo("If", [](const Context&, std::string_view index) -> V {
        if (index.empty()) return V::nil();
        const char sep = std::count(index.begin(), index.end(), '~') == 2 ? '~' : ',';
        const auto first = index.find(sep);
        if (first == std::string_view::npos) return V::nil();
        const auto second = index.find(sep, first + 1);
        if (second == std::string_view::npos) return V::nil();
        const auto cond = calculate(index.substr(0, first));
        if (!cond) return V::nil();
        return V::string(std::string(*cond != 0.0 ? index.substr(first + 1, second - first - 1) : index.substr(second + 1)));
    });

    // Range.Between[a,b:n] (a <= n <= b) and Range.Inside[a,b:n] (a < n < b); FALSE without the ':' or ','.
    Type& range = r.type("range");
    const Type* range_type = &range;
    range.to_value = [](const Context&, const Value&) { return V::nil(); };
    const auto range_member = [&range](const char* name, bool inclusive) {
        range.members[name] = [inclusive](const Context&, const Value&, std::string_view index) -> V {
            if (index.empty()) return V::nil();
            const auto colon = index.find(':');
            if (colon == std::string_view::npos) return V::boolean(false);
            const std::string_view ends = index.substr(0, colon);
            const auto comma = ends.find(',');
            if (comma == std::string_view::npos) return V::boolean(false);
            const std::int64_t n = int64_from_text(index.substr(colon + 1), 0);
            const std::int64_t lo = int64_from_text(ends.substr(0, comma), 0);
            const std::int64_t hi = int64_from_text(ends.substr(comma + 1), 0);
            return V::boolean(inclusive ? (n >= lo && n <= hi) : (n > lo && n < hi));
        };
    };
    range_member("Between", true);
    range_member("Inside", false);
    r.add_tlo("Range", [range_type](const Context&, std::string_view) { return V::object(range_type, 0); });
}

void register_game_tlos(Registry& r) {
    g.spawn = &r.type("spawn");
    g.character = &r.type("character");
    g.cls = &r.type("class");
    g.race = &r.type("race");
    g.heading = &r.type("heading");
    g.spell = &r.type("spell");
    g.buff = &r.type("buff");
    g.timestamp = &r.type("timestamp");
    g.item = &r.type("item");
    g.zone = &r.type("zone");
    g.group = &r.type("group");
    g.member = &r.type("groupmember");
    g.everquest = &r.type("everquest");
    g.window = &r.type("window");
    g.plugin = &r.type("plugin");
    g.time = &r.type("time");
    g.gametime = &r.type("gametime");
    g.navigation = &r.type("navigation");
    g.stick = &r.type("stick");
    register_spawn();
    register_character();
    register_abilities();
    register_small_types();
    register_spell();
    register_item();
    register_group();
    register_misc_types();
    register_navigation();
    register_stick();

    // MQ's LineOfSight[y,x,z:y,x,z]: two points in the server's coordinates (MQ's Loc order).
    r.add_tlo("LineOfSight", [](const Context& c, std::string_view index) -> V {
        if (!c.line_of_sight) return V::nil();
        const auto colon = index.find(':');
        if (colon == std::string_view::npos) return V::nil();
        auto point = [](std::string_view text) -> std::optional<glm::vec3> {
            double v[3] = {};
            int n = 0;
            for (std::size_t i = 0; i <= text.size() && n < 3;) {
                std::size_t j = text.find(',', i);
                if (j == std::string_view::npos) j = text.size();
                std::string_view part = text.substr(i, j - i);
                while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
                while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
                // A point past the world (as nav_mesh bounds it), inf or nan has no sight line: nil.
                if (!parse_float(part, v[n]) || !std::isfinite(v[n]) || std::abs(v[n]) >= 1.0e6) return std::nullopt;
                ++n;
                i = j + 1;
            }
            if (n != 3) return std::nullopt;
            // Finite and within float (a cast past it is undefined; the collision grid takes no infinities).
            const auto y = in_range<float>(v[0]), x = in_range<float>(v[1]), z = in_range<float>(v[2]);
            if (!x || !y || !z) return std::nullopt;
            return game::server_to_world(glm::vec3(*x, *y, *z));
        };
        const auto a = point(index.substr(0, colon)), b = point(index.substr(colon + 1));
        if (!a || !b) return V::nil();
        const auto seen = c.line_of_sight({a->x, a->y, a->z}, {b->x, b->y, b->z});
        return seen ? V::boolean(*seen) : V::nil();
    });
    r.add_tlo("Navigation", [](const Context& c, std::string_view) -> V {
        return c.nav ? V::object(g.navigation, 0) : V::nil();
    });
    r.add_tlo("Stick", [](const Context& c, std::string_view) -> V { return c.nav ? V::object(g.stick, 0) : V::nil(); });
    r.add_tlo("Me", [](const Context& c, std::string_view) -> V {
        return c.conn && c.conn->world().player() ? V::object(g.character, c.conn->world().player_id) : V::nil();
    });
    r.add_tlo("Target", [](const Context& c, std::string_view) -> V {
        return c.conn ? spawn_value(spawn_of(c, c.conn->world().target_id)) : V::nil();
    });
    r.add_tlo("Spawn", [](const Context& c, std::string_view index) -> V {
        if (!c.conn) return V::nil();
        std::int64_t id = 0;
        if (parse_int(index, id)) return spawn_value(spawn_of(c, id));
        auto found = find_spawns(index, *c.conn, c.can_see);
        return found.empty() ? V::nil() : spawn_value(found.front());
    });
    r.add_tlo("SpawnCount", [](const Context& c, std::string_view index) -> V {
        if (!c.conn) return V::integer(0);
        return V::integer(static_cast<std::int64_t>(
            find_spawns(index, *c.conn, c.can_see).size()));
    });
    r.add_tlo("NearestSpawn", [](const Context& c, std::string_view index) -> V {
        if (!c.conn) return V::nil();
        // "N,search", "N" or "search".
        std::int64_t n = 1;
        std::string_view search = index;
        if (auto comma = index.find(','); comma != std::string_view::npos && parse_int(index.substr(0, comma), n)) {
            search = index.substr(comma + 1);
        } else if (parse_int(index, n)) {
            search = {};
        } else {
            n = 1;
        }
        auto found = find_spawns(search, *c.conn, c.can_see);
        if (n < 1 || n > static_cast<std::int64_t>(found.size())) return V::nil();
        return spawn_value(found[static_cast<std::size_t>(n - 1)]);
    });
    r.add_tlo("Group", [](const Context& c, std::string_view) -> V {
        return c.conn && c.conn->group().active ? V::object(g.group, 0) : V::nil();
    });
    r.add_tlo("Zone", [](const Context& c, std::string_view) -> V { return c.conn ? V::object(g.zone, c.conn->zone_id()) : V::nil(); });
    r.add_tlo("Spell", [](const Context& c, std::string_view index) { return spell_by_index(c, index); });
    r.add_tlo("Cursor", [](const Context& c, std::string_view) {
        return item_value(c, net::ItemSlot::possessions(net::kSlotCursor));
    });
    r.add_tlo("FindItem", [](const Context& c, std::string_view index) -> V {
        V found;
        each_carried(c, [&](const net::ItemSlot& s, const net::Item& it) {
            if (!item_name_matches(it, index)) return false;
            found = V::object(g.item, pack_slot(s));
            return true;
        });
        return found;
    });
    r.add_tlo("FindItemCount", [](const Context& c, std::string_view index) {
        std::int64_t n = 0;
        each_carried(c, [&](const net::ItemSlot&, const net::Item& it) {
            if (item_name_matches(it, index)) n += it.stack_count ? it.stack_count : 1;
            return false;
        });
        return V::integer(n);
    });
    r.add_tlo("Window", [](const Context&, std::string_view index) {
        return index.empty() ? V::nil() : V::object(g.window, 0, 0, std::string(index));
    });
    r.add_tlo("Plugin", [](const Context&, std::string_view index) {
        return index.empty() ? V::nil() : V::object(g.plugin, 0, 0, std::string(index));
    });
    r.add_tlo("Time", [](const Context&, std::string_view) {
        return V::object(g.time, static_cast<std::int64_t>(std::time(nullptr)));
    });
    r.add_tlo("GameTime", [](const Context& c, std::string_view) -> V {
        return c.conn && c.conn->time_of_day() ? V::object(g.gametime, 0) : V::nil();
    });
    r.add_tlo("EverQuest", [](const Context&, std::string_view) { return V::object(g.everquest, 0); });
    r.add_tlo("Heading", [](const Context&, std::string_view index) -> V {
        double d = 0;
        return parse_float(index, d) && std::isfinite(d) ? heading_value(std::fmod(std::fmod(d, 360.0) + 360.0, 360.0)) : V::nil();
    });
    r.add_tlo("Int", [](const Context&, std::string_view index) -> V {
        double d = 0;
        const auto n = parse_float(index, d) ? whole_number(d) : std::nullopt;
        return n ? V::integer(*n) : V::nil();
    });
    r.add_tlo("Float", [](const Context&, std::string_view index) -> V {
        double d = 0;
        return parse_float(index, d) ? V::number(d) : V::nil();
    });
    r.add_tlo("Bool", [](const Context&, std::string_view index) {
        std::string l = lower(index);
        return V::boolean(!(l.empty() || l == "false" || l == "null" || l == "0"));
    });
    r.add_tlo("String", [](const Context&, std::string_view index) { return V::string(std::string(index)); });
    register_math(r);
    r.add_tlo("Select", [](const Context&, std::string_view index) {
        // Select[x,a,b,c]: the 1-based position of x among the rest, 0 when absent.
        auto a = split_index(index);
        for (std::size_t i = 1; i < a.size(); ++i)
            if (iequals(a[0], a[i])) return V::integer(static_cast<std::int64_t>(i));
        return V::integer(0);
    });
    r.add_tlo("Pet", [](const Context& c, std::string_view) -> V {
        if (!c.conn) return V::nil();
        if (c.conn->pets().focused) return spawn_value(spawn_of(c, c.conn->pets().focused));
        for (const auto& [id, o] : c.conn->world().spawns)
            if (o.pet_owner_id == c.conn->world().player_id && !o.is_corpse) return spawn_value(&o);
        return V::nil();
    });
}

} // namespace hailstone::mq
