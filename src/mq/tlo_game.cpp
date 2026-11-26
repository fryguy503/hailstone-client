// The game's MacroQuest TLOs and data types (mq/tlo.h), from GameConnection's state.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <random>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include "core/build_info.h"
#include "game/bank.h"
#include "game/combat.h"
#include "game/counter_spas.h"
#include "game/coords.h"
#include "game/doors.h"
#include "game/eq_names.h"
#include "game/group.h"
#include "game/inventory.h"
#include "game/item_links.h"
#include "game/mercenary.h"
#include "game/server_ext.h"
#include "game/spell_effect_text.h"
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
        *everquest, *macroquest, *hailstone, *window, *plugin, *time, *gametime, *navigation, *stick, *debuff, *moveto,
        *bandolier, *bandolier_item, *advpath, *target, *xtarget, *altability, *aug, *ticks, *body, *raid, *raidmember, *corpse, *pet, *invslot, *merchant, *door, *mercenary;
};

Types g; // set once by register_game_tlos (the types live in the registry that registered them)

// Which of the 16 compass points (N, NNE, ...) a heading in degrees is nearest; N for NaN or an infinity.
int heading_point(double deg) {
    if (!std::isfinite(deg)) return 0;
    const double t = std::fmod(std::fmod(deg + 11.25, 360.0) + 360.0, 360.0); // [0, 360)
    return static_cast<int>(t / 22.5) % 16;
}

const game::Spawn* spawn_of(const Context& c, std::int64_t id) {
    // (an id past 32 bits is none, not the id it wraps to)
    if (!c.conn || id <= 0 || id > std::numeric_limits<std::uint32_t>::max()) return nullptr;
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
    if (!c.conn || id < 0 || id > std::numeric_limits<std::uint32_t>::max() || id == game::kNoSpell) return nullptr;
    return c.conn->spell_table().get(static_cast<std::uint32_t>(id));
}

V spell_value(const Context& c, std::uint32_t id) {
    return spell_data(c, id) ? V::object(g.spell, id) : V::nil();
}

V spell_by_index(const Context& c, std::string_view index) {
    if (!c.conn) return V::nil();
    std::int64_t id = 0;
    if (parse_int(index, id)) return spell_data(c, id) ? V::object(g.spell, id) : V::nil(); // (no 32-bit wrap)
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
// OP_TargetBuffs last gave for that spawn (Target.Buff), or (the second number kPetBuffs) the pet window's slot
// (OP_PetBuffWindow). Once target buffs are another spawn's, it reads as nothing.
constexpr double kPetBuffs = -1;
const game::BuffSlot* buff_of(const Context& c, const Value& v) {
    if (!c.conn || v.i < 0) return nullptr;
    const auto& st = c.conn->spell_state();
    if (v.f == kPetBuffs) {
        for (const auto& [slot, b] : c.conn->pet_buffs())
            if (static_cast<std::int64_t>(slot) == v.i) return b.empty() ? nullptr : &b;
        return nullptr;
    }
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
// The player's pet: the one the pet window shows, else the first the player owns (MQ's Me.Pet and Pet).
const game::Spawn* my_pet(const Context& c) {
    if (!c.conn) return nullptr;
    if (c.conn->pets().focused) return spawn_of(c, c.conn->pets().focused);
    for (const auto& [id, o] : c.conn->world().spawns)
        if (o.pet_owner_id == c.conn->world().player_id && !o.is_corpse) return &o;
    return nullptr;
}

// The pet as MQ's pet type (a spawn whose Buff reads the pet window), NULL without one.
V pet_value(const game::Spawn* pet) { return pet ? V::object(g.pet, pet->id) : V::nil(); }

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
    } else if (const game::Spawn* pet = my_pet(c); pet && pet->id == s.id) {
        // the player's own pet, not targeted: the pet window's buffs (OP_PetBuffWindow), in slot order
        std::vector<std::pair<std::uint32_t, const game::BuffSlot*>> list;
        for (const auto& [slot, b] : c.conn->pet_buffs())
            if (!b.empty()) list.emplace_back(slot, &b);
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b2) { return a.first < b2.first; });
        for (const auto& [slot, b] : list) out.push_back({V::object(g.buff, slot, kPetBuffs), b});
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

// Debuff[...]: the disease, poison, curse and corruption counters on the player (Debuff) or the player's pet
// (Debuff[pet]), summed over every detrimental buff (spell type 0). Each counter effect with a positive base (a cure's own is negative) adds
// the counters left once the client knows them (the effect's slot data, as the buff window's counter overlay), else
// the counters the spell starts with (a zone-in list or the profile fills no slot data). The pet's buffs
// (OP_PetBuffWindow) carry no slot data, so they always count the starting counters.
struct DebuffCounters {
    std::int64_t poison = 0, disease = 0, curse = 0, corruption = 0;
    std::int64_t total() const { return poison + disease + curse + corruption; }
};
DebuffCounters debuff_counters(const Context& c, bool pet) {
    DebuffCounters out;
    if (!c.conn) return out;
    auto add_buff = [&](const game::BuffSlot& b) {
        if (b.empty()) return;
        const game::SpellData* d = spell_data(c, b.spell);
        if (!d || d->beneficial()) return; // a beneficial spell's counter effect (some buffs carry one) is no debuff
        for (std::size_t e = 0; e < d->effect.size(); ++e) {
            if (!game::is_counter_spa(d->effect[e]) || d->base[e] <= 0) continue;
            const std::int64_t n = b.counters_known && e < b.slot_data.size() ? b.slot_data[e] : d->base[e];
            switch (d->effect[e]) {
            case game::kSpaPoisonCounter: out.poison += n; break;
            case game::kSpaDiseaseCounter: out.disease += n; break;
            case game::kSpaCurseCounter: out.curse += n; break;
            default: out.corruption += n; break;
            }
        }
    };
    if (pet)
        for (const auto& [slot, b] : c.conn->pet_buffs()) add_buff(b);
    else
        for (const auto& b : c.conn->spell_state().buffs) add_buff(b);
    return out;
}

// Whether any of the player's (or the pet's) buffs has this spell effect: Debuff.Silenced (SPA 96).
bool debuff_has_effect(const Context& c, bool pet, std::int32_t spa) {
    if (!c.conn) return false;
    auto has = [&](const game::BuffSlot& b) {
        if (b.empty()) return false;
        const game::SpellData* d = spell_data(c, b.spell);
        return d && std::find(d->effect.begin(), d->effect.end(), spa) != d->effect.end();
    };
    if (pet) {
        const auto& list = c.conn->pet_buffs();
        return std::any_of(list.begin(), list.end(), [&](const auto& sb) { return has(sb.second); });
    }
    const auto& list = c.conn->spell_state().buffs;
    return std::any_of(list.begin(), list.end(), has);
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

// ---------------------------------------------------------------- items

// An item's place packed into a value: (aug + 1) << 48 | type << 32 | slot << 16 | (sub + 1). The augment is a
// common item's content (an AugSlot); the inventory itself is looked up without it.
std::int64_t pack_slot(const net::ItemSlot& s) {
    return (static_cast<std::int64_t>(static_cast<std::uint8_t>(s.aug + 1)) << 48) |
           (static_cast<std::int64_t>(static_cast<std::uint16_t>(s.type)) << 32) |
           (static_cast<std::int64_t>(static_cast<std::uint16_t>(s.slot)) << 16) | static_cast<std::uint16_t>(s.sub + 1);
}
net::ItemSlot unpack_slot(std::int64_t v) {
    net::ItemSlot s;
    s.type = static_cast<std::int16_t>(static_cast<std::uint16_t>(v >> 32));
    s.slot = static_cast<std::int16_t>(static_cast<std::uint16_t>(v >> 16));
    s.sub = static_cast<std::int16_t>(static_cast<std::int32_t>(v & 0xffff) - 1);
    s.aug = static_cast<std::int16_t>(static_cast<std::int32_t>((v >> 48) & 0xff) - 1);
    return s;
}

const net::Item* item_of(const Context& c, const Value& v) {
    if (!c.conn) return nullptr;
    net::ItemSlot s = unpack_slot(v.i);
    if (s.type == net::kInvMerchant) { // the open merchant's stock (Merchant.Item): not in the inventory
        const auto& m = c.conn->merchant();
        if (!m || !m->open || s.sub >= 0 || s.aug >= 0) return nullptr;
        auto it = m->items.find(static_cast<std::uint32_t>(static_cast<std::uint16_t>(s.slot)));
        return it == m->items.end() ? nullptr : &it->second;
    }
    if (s.type == net::kInvCorpse) { // the open loot window's (Corpse.Item): not in the inventory
        const auto& loot = c.conn->loot_session();
        if (!loot || !loot->open || s.sub >= 0) return nullptr;
        auto it = loot->items.find(s.slot);
        if (it == loot->items.end()) return nullptr;
        if (s.aug < 0) return &it->second;
        return it->second.item_class == 0 ? it->second.content(static_cast<std::uint32_t>(s.aug)) : nullptr;
    }
    const std::int16_t aug = s.aug;
    s.aug = -1;
    const net::Item* it = c.conn->inventory().at(s);
    if (!it || aug < 0) return it;
    return it->item_class == 0 ? it->content(static_cast<std::uint32_t>(aug)) : nullptr;
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
    // A number past the slots' 16 bits is no slot (Inventory[65536] is not the charm).
    if (parse_int(index, n)) return n >= 0 && n <= std::numeric_limits<std::int16_t>::max() ? static_cast<int>(n) : -1;
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
    spawn_member("Silenced", [](const Context& c, const game::Spawn& s, std::string_view) { // MQ's SPA_SILENCE test
        return buff_with_effect(c, s, 96, [](std::int32_t) { return true; });
    });
    // What a cure takes off: a buff carrying counters of that kind (a positive base; a cure's own effect is negative).
    spawn_member("Poisoned", [](const Context& c, const game::Spawn& s, std::string_view) {
        return buff_with_effect(c, s, game::kSpaPoisonCounter, [](std::int32_t base) { return base > 0; });
    });
    spawn_member("Diseased", [](const Context& c, const game::Spawn& s, std::string_view) {
        return buff_with_effect(c, s, game::kSpaDiseaseCounter, [](std::int32_t base) { return base > 0; });
    });
    spawn_member("Cursed", [](const Context& c, const game::Spawn& s, std::string_view) {
        return buff_with_effect(c, s, game::kSpaCurseCounter, [](std::int32_t base) { return base > 0; });
    });
    spawn_member("Corrupted", [](const Context& c, const game::Spawn& s, std::string_view) {
        return buff_with_effect(c, s, game::kSpaCorruptionCounter, [](std::int32_t base) { return base > 0; });
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
        if (is_me(c, s)) return pet_value(my_pet(c));
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
    // The buff and song windows' slots: RoF2's 42 and 20, whatever the character's level or AAs.
    me_member("MaxBuffSlots", [](const Context&, const GameConnection&, std::string_view) {
        return V::integer(game::kLongBuffSlots);
    });
    me_member("MaxSongSlots", [](const Context&, const GameConnection&, std::string_view) {
        return V::integer(game::kShortBuffSlots);
    });
    me_member("FreeBuffSlots", [](const Context& ctx, const GameConnection&, std::string_view) {
        return V::integer(std::max<std::int64_t>(
            0, std::int64_t{game::kLongBuffSlots} - static_cast<std::int64_t>(buff_slots(ctx, false).size())));
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
            if (n >= 1 && n <= game::kSpellGems) gem = static_cast<int>(n) - 1; // (2147483648 is no gem)
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
        // MQ's xtarget type: the slot's spawn, and the slot for its PctAggro.
        const game::Spawn* s = spawn_of(ctx, slots[static_cast<std::size_t>(n - 1)].spawn_id);
        return s ? V::object(g.xtarget, s->id, static_cast<double>(n - 1)) : V::nil();
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
    me_member("Pet", [](const Context& ctx, const GameConnection&, std::string_view) -> V { return pet_value(my_pet(ctx)); });
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
    // MQ's Spell.Duration: ticks (a plain value of ticks, the timestamp members on their milliseconds) at the
    // player's level (GetSpellDuration), the record's cap without a connection.
    m("Duration", [](const Context& c, const game::SpellData& s, std::string_view) {
        const std::int64_t ticks = c.conn ? c.conn->spell_duration_ticks(s) : static_cast<std::int64_t>(s.duration_cap);
        return V::object(g.ticks, std::max<std::int64_t>(ticks, 0) * 6000);
    });
    m("DurationFormula", [](const Context&, const game::SpellData& s, std::string_view) { return V::integer(s.duration_formula); });
    m("DurationCap", [](const Context&, const game::SpellData& s, std::string_view) { return V::integer(s.duration_cap); });
    m("Level", [](const Context& c, const game::SpellData& s, std::string_view) -> V {
        if (!c.conn || !c.conn->profile()) return V::nil();
        int cls = c.conn->profile()->class_;
        if (cls < 1 || cls > 16) return V::nil();
        auto lvl = s.class_level[static_cast<std::size_t>(cls - 1)];
        return lvl == 255 ? V::nil() : V::integer(lvl);
    });
    m("TargetType", [](const Context&, const game::SpellData& s, std::string_view) {
        return V::string(std::string(game::spell_target_type_desc(s.target_type))); // MacroQuest's text
    });
    m("SpellType", [](const Context&, const game::SpellData& s, std::string_view) {
        return V::string(s.spell_type == 0 ? "Detrimental" : "Beneficial");
    });
    m("Beneficial", [](const Context&, const game::SpellData& s, std::string_view) { return V::boolean(s.spell_type != 0); });
    m("ResistType", [](const Context&, const game::SpellData& s, std::string_view) {
        return V::string(std::string(game::spell_resist_type_desc(s.resist))); // MacroQuest's text
    });
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
        if (v.f == 0 || v.f == kPetBuffs) return V::integer(v.i + 1); // the player's or the pet window's slot
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
        if (!parse_int(index, n) || n < 1 || n - 1 > std::numeric_limits<std::int16_t>::max() || at.sub >= 0 ||
            !it.is_bag())
            return V::nil();
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
        if (parse_int(index, n))
            return n >= 0 && n <= std::numeric_limits<int>::max() ? member_value(c, static_cast<int>(n)) : V::nil();
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

    // MacroQuest.Path[name]: MQ's folders by name. Only "config" has a counterpart here (the scripts' settings
    // folder); MQ's other names (root, macros, logs, plugins, ...) and no name answer nil until one is settled.
    // Hailstone.Path[name] is the same member under the client's own name.
    auto path = [](const Context& c, const Value&, std::string_view index) -> V {
        if (iequals(index, "config") && !c.config_path.empty()) return V::string(c.config_path);
        return V::nil();
    };
    add(g.macroquest, "Path", path);
    add(g.hailstone, "Path", path);
    // Members neither has are EverQuest's, as MQ's MacroQuest type passes them on (MQ2MacroQuestType::GetMember):
    // MacroQuest.Server is EverQuest.Server, the server the player logged in to; GameState too.
    const_cast<Type&>(*g.macroquest).parent = g.everquest;
    const_cast<Type&>(*g.hailstone).parent = g.everquest;
    // Build is MQ's build target, an int (BuildTarget: 1 Live, 2 Test, 3 Beta, 4 Emu), not a version: Hailstone is an
    // emulator client, so 4. E3 reads it as its MQBuild and takes any other value for live. BuildDate is the day the
    // client was built as yyyymmdd (MQ's is MQ2Main.dll's file date); Version Hailstone's release version
    // ("0.7.2-beta"), where MQ gives its own file version. Hailstone's members are the same.
    for (const Type* t : {g.macroquest, g.hailstone}) {
        add(t, "Build", [](const Context&, const Value&, std::string_view) { return V::integer(4); });
        add(t, "BuildDate", [](const Context&, const Value&, std::string_view) { return V::integer(build_date()); });
        add(t, "Version", [](const Context&, const Value&, std::string_view) { return V::string(release_version()); });
    }

    // Debuff (MQ2Debuffs' TLO, which E3 reads): i 0 the player, 1 the pet (debuff_counters). Its text is Count.
    Type& deb = const_cast<Type&>(*g.debuff);
    deb.to_value = [](const Context& c, const Value& v) { return V::integer(debuff_counters(c, v.i != 0).total()); };
    add(g.debuff, "Count", [](const Context& c, const Value& v, std::string_view) {
        return V::integer(debuff_counters(c, v.i != 0).total());
    });
    add(g.debuff, "Poisoned", [](const Context& c, const Value& v, std::string_view) {
        return V::integer(debuff_counters(c, v.i != 0).poison);
    });
    add(g.debuff, "Diseased", [](const Context& c, const Value& v, std::string_view) {
        return V::integer(debuff_counters(c, v.i != 0).disease);
    });
    add(g.debuff, "Cursed", [](const Context& c, const Value& v, std::string_view) {
        return V::integer(debuff_counters(c, v.i != 0).curse);
    });
    add(g.debuff, "Corrupted", [](const Context& c, const Value& v, std::string_view) {
        return V::integer(debuff_counters(c, v.i != 0).corruption);
    });
    // Silenced: TRUE while a buff silences (SPA 96, as Me.Silenced); E3 asks it before casting.
    add(g.debuff, "Silenced", [](const Context& c, const Value& v, std::string_view) {
        return V::boolean(debuff_has_effect(c, v.i != 0, 96));
    });

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

// ---------------------------------------------------------------- the members E3 reads (MacroQuest's meanings)

// A member of the player's character (Me.PctHPs and the like) read through the registry, for the group's sums.
V character_member(const Context& c, const char* name) {
    if (!c.conn || !c.conn->world().player()) return V::nil();
    auto it = g.character->members.find(name);
    return it == g.character->members.end() ? V::nil() : it->second(c, V::object(g.character, c.conn->world().player_id), {});
}

// The aggro meter's reading of entry `type` (net/aggro_packets.h: 0 the player, 1 the secondary, 2..6 the group,
// 7..26 the extended targets); 0 until the server sends one, as MQ's pAggroInfo reads.
V aggro_pct(const Context& c, int type) {
    if (!c.conn || type < 0 || type >= net::kAggroTypes) return V::integer(0);
    return V::integer(c.conn->aggro().pct[static_cast<std::size_t>(type)]);
}

// MQ's FindItemByID / FindItemByName over the carried items (worn, packs and their bags, the cursor): a number is an
// item id, "=name" the exact name, else part of the name.
std::optional<net::ItemSlot> find_carried(const Context& c, std::string_view index) {
    std::optional<net::ItemSlot> found;
    std::int64_t id = 0;
    const bool by_id = parse_int(index, id);
    // An id past uint32 is no item's (4294967296 + id would otherwise match id, -1 would match 0xffffffff).
    if (by_id && (id < 0 || id > std::numeric_limits<std::uint32_t>::max())) return std::nullopt;
    each_carried(c, [&](const net::ItemSlot& s, const net::Item& it) {
        if (by_id ? it.id == static_cast<std::uint32_t>(id) : item_name_matches(it, index)) {
            found = s;
            return true;
        }
        return false;
    });
    return found;
}

// The seconds until an item's click effect may be used again (GetItemTimer); nullopt without a click effect.
std::optional<std::int64_t> item_click_seconds(const Context& c, const net::Item& it) {
    if (!c.conn || !it.effects[net::kEffectClick].present()) return std::nullopt;
    const auto ready_at = static_cast<std::int64_t>(c.conn->item_recast_timer(&it));
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    return ready_at > now ? ready_at - now : 0;
}

// Each item in the general slots and their bags (MQ's InvSlot_FirstBagSlot .. the last), for StackCount and FreeStack.
template <typename F> void each_pack_item(const Context& c, F&& fn) {
    if (!c.conn) return;
    for (int s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s) {
        const net::Item* it = c.conn->inventory().at(net::ItemSlot::possessions(static_cast<std::int16_t>(s)));
        if (!it) continue;
        if (it->is_bag()) {
            for (const auto& b : it->contents) fn(b.item);
        } else {
            fn(*it);
        }
    }
}

// The player's owned AA rank by name (dbstr type 1, or the name the windows show) or by its ability's group id.
const game::AltAbility* owned_aa(const Context& c, std::string_view index) {
    if (!c.conn || index.empty()) return nullptr;
    const auto& aa = c.conn->alt_adv();
    std::int64_t group = 0;
    if (parse_int(index, group)) // a group id past int32 is no group (it would wrap onto one)
        return group >= 0 && group <= std::numeric_limits<std::int32_t>::max()
                   ? aa.owned_rank(static_cast<std::int32_t>(group)) : nullptr;
    for (const auto& o : aa.owned) {
        const game::AltAbility* a = o.id ? aa.get(o.id) : nullptr;
        if (a && (iequals(c.conn->aa_name(*a), index) || iequals(c.conn->aa_display_name(*a), index))) return a;
    }
    return nullptr;
}

// MQ's IsSpellTooPowerful with the player casting on `target`.
bool spell_too_powerful(const Context& c, const game::Spawn& target, const game::SpellData& s) {
    const game::Spawn* caster = me(c);
    if (!caster || caster->id == target.id) return false;
    if (s.target_type == 6 || s.spell_type == 0 || s.duration_formula == 0) return false; // self, detrimental, instant
    const std::uint8_t level = s.level_needed(caster->class_id);
    if (level == 0 || level > 254) return false;
    int target_level = target.level;
    if (target.is_npc && target.pet_owner_id)
        if (const game::Spawn* master = spawn_of(c, target.pet_owner_id)) target_level = master->level;
    if (level > 65 && target_level < 61) return true;
    return level > 50 && target_level < level / 2 + 15;
}

// MQ's GetMeleeSpeedPctFromSpell(spell, false): the slow of the spell's first SPA 11 below 100%, at the player's
// level. Hailstone leaves out MQ's look into a triggered spell's effects.
std::int64_t slow_pct(const Context& c, const game::SpellData& s) {
    for (std::size_t e = 0; e < s.effect.size(); ++e) {
        if (s.effect[e] != 11 || s.base[e] - 100 >= 0) continue;
        int min_level = 255;
        for (std::uint8_t l : s.class_level)
            if (l < min_level) min_level = l;
        const int level = c.conn ? c.conn->player_status().level : 1;
        return std::llabs(game::spell_calc_value(s.formula[e], s.base[e] - 100, s.max[e] - 100, 0,
                                                 min_level == 255 ? 1 : min_level, level));
    }
    return 0;
}

void register_e3_members() {
    // ---- Me
    const_cast<Type*>(g.character)->members["Endurance"] = g.character->members.at("CurrentEndurance"); // MQ's old name
    me_member("PctAggro", [](const Context& ctx, const GameConnection&, std::string_view) { return aggro_pct(ctx, net::kAggroPlayer); });
    me_member("SecondaryPctAggro", [](const Context& ctx, const GameConnection&, std::string_view) {
        return aggro_pct(ctx, net::kAggroSecondary);
    });
    me_member("XTargetSlots", [](const Context&, const GameConnection& c, std::string_view) {
        return V::integer(static_cast<std::int64_t>(c.xtargets().slots.size()));
    });
    me_member("AutoFire", [](const Context&, const GameConnection& c, std::string_view) { return V::boolean(c.auto_fire()); });
    me_member("AAPointsTotal", [](const Context&, const GameConnection& c, std::string_view) -> V {
        return c.profile() ? V::integer(std::int64_t{c.profile()->aa_points} + c.profile()->aa_points_spent) : V::nil();
    });
    me_member("AAPointsAssigned", [](const Context&, const GameConnection& c, std::string_view) {
        return V::integer(c.alt_adv().assigned[0]);
    });
    // MQ: the time left on the cast bar.
    me_member("CastTimeLeft", [](const Context&, const GameConnection& c, std::string_view) {
        const auto& cast = c.spell_state().casting;
        const std::int64_t left = cast.eta_ms ? cast.eta_ms - GameConnection::now_ms() : 0;
        return V::object(g.timestamp, std::max<std::int64_t>(left, 0));
    });
    // MQ's GemTimer: the longer of the gem's own recast and the spell's linked reuse timer. MQ answers the whole
    // recast instead of what is left for a timer it has just seen change; here it is always what is left.
    me_member("GemTimer", [](const Context& ctx, const GameConnection& c, std::string_view index) -> V {
        const auto& st = c.spell_state();
        std::int64_t n = 0;
        int gem = -1;
        if (parse_int(index, n)) {
            if (n >= 1 && n <= game::kSpellGems) gem = static_cast<int>(n) - 1; // as SpellReady (2147483648 is no gem)
        } else {
            for (int i = 0; i < game::kSpellGems && gem < 0; ++i) {
                const game::SpellData* s = spell_data(ctx, st.gems[static_cast<std::size_t>(i)].spell);
                if (s && iequals(s->name, index)) gem = i;
            }
        }
        if (gem < 0 || gem >= game::kSpellGems) return V::nil();
        const game::SpellData* s = spell_data(ctx, st.gems[static_cast<std::size_t>(gem)].spell);
        if (!s) return V::nil();
        const std::int64_t now = GameConnection::eq_time();
        std::int64_t left = std::max<std::int64_t>(std::int64_t{st.gems[static_cast<std::size_t>(gem)].ready} - now, 0);
        const int linked = s->reuse_timer_index;
        if (linked > 0 && linked < static_cast<int>(st.linked_ready.size())) {
            const auto fast = static_cast<std::int64_t>(std::time(nullptr));
            const std::int64_t ready = st.linked_ready[static_cast<std::size_t>(linked)];
            if (ready > fast) left = std::max(left, (ready - fast) * 1000);
        }
        return V::object(g.timestamp, left);
    });
    // MQ's ItemReady: the carried item has a click effect whose timer has run out.
    me_member("ItemReady", [](const Context& ctx, const GameConnection& c, std::string_view index) {
        auto at = find_carried(ctx, index);
        const net::Item* it = at ? c.inventory().at(*at) : nullptr;
        auto secs = it ? item_click_seconds(ctx, *it) : std::nullopt;
        return V::boolean(secs && *secs == 0);
    });
    // MQ: Ability[name] is whether the player has the skill (an activatable one, as AbilityReady finds it).
    me_member("Ability", [](const Context& ctx, const GameConnection&, std::string_view index) {
        return V::boolean(ability(ctx, AbilityKind::Skill, index).has_value());
    });
    // MQ: CombatAbility[#] is the spell of the player's n-th combat ability, CombatAbility[name] its number.
    me_member("CombatAbility", [](const Context& ctx, const GameConnection& c, std::string_view index) -> V {
        constexpr int kCombatAbilities = 300; // PcProfile's list (the profile ends sooner when it is shorter)
        std::int64_t n = 0;
        if (parse_int(index, n)) return n < 1 || n > kCombatAbilities ? V::nil() : spell_value(ctx, c.combat_ability(static_cast<std::int32_t>(n - 1)));
        for (int i = 0; i < kCombatAbilities; ++i) {
            const game::SpellData* s = spell_data(ctx, c.combat_ability(i));
            if (s && iequals(s->name, index)) return V::integer(i + 1);
        }
        return V::nil();
    });
    // MQ reads the Combat Abilities window's effect label: the discipline in the last buff slot.
    me_member("ActiveDisc", [](const Context& ctx, const GameConnection& c, std::string_view) -> V {
        if (!c.discipline_active()) return V::nil();
        return spell_value(ctx, c.spell_state().buffs[game::kBuffSlots - 1].spell);
    });
    me_member("AltAbility", [](const Context& ctx, const GameConnection&, std::string_view index) -> V {
        const game::AltAbility* a = owned_aa(ctx, index);
        return a ? V::object(g.altability, a->id) : V::nil();
    });

    // ---- altability (MQ2AltAbilityType): its plain value is the ability's group id
    Type& aa = const_cast<Type&>(*g.altability);
    auto aa_of = [](const Context& c, const Value& v) -> const game::AltAbility* {
        return c.conn ? c.conn->alt_adv().get(static_cast<std::uint32_t>(v.i)) : nullptr;
    };
    aa.to_value = [aa_of](const Context& c, const Value& v) {
        const game::AltAbility* a = aa_of(c, v);
        return a ? V::integer(a->group) : V::nil();
    };
    auto aam = [aa_of](const char* name, auto fn) {
        add(g.altability, name, [aa_of, fn](const Context& c, const Value& v, std::string_view index) -> V {
            const game::AltAbility* a = aa_of(c, v);
            return a ? fn(c, *a, index) : V::nil();
        });
    };
    aam("ID", [](const Context&, const game::AltAbility& a, std::string_view) { return V::integer(a.group); });
    aam("GroupID", [](const Context&, const game::AltAbility& a, std::string_view) { return V::integer(a.group); });
    aam("Name", [](const Context& c, const game::AltAbility& a, std::string_view) {
        std::string n = c.conn->aa_name(a);
        return V::string(n.empty() ? c.conn->aa_display_name(a) : n);
    });
    aam("Description", [](const Context& c, const game::AltAbility& a, std::string_view) -> V {
        std::string d = c.conn->aa_description(a);
        return d.empty() ? V::nil() : V::string(std::move(d));
    });
    aam("Rank", [](const Context&, const game::AltAbility& a, std::string_view) { return V::integer(a.rank); });
    aam("MaxRank", [](const Context&, const game::AltAbility& a, std::string_view) { return V::integer(a.max_rank); });
    aam("ReuseTime", [](const Context&, const game::AltAbility& a, std::string_view) { return V::integer(a.refresh); });
    aam("Cost", [](const Context&, const game::AltAbility& a, std::string_view) { return V::integer(a.cost); });
    aam("MinLevel", [](const Context&, const game::AltAbility& a, std::string_view) { return V::integer(a.level_req); });
    aam("Type", [](const Context&, const game::AltAbility& a, std::string_view) { return V::integer(a.type); });
    aam("Passive", [](const Context&, const game::AltAbility& a, std::string_view) { return V::boolean(!a.activated()); });
    aam("Spell", [](const Context& c, const game::AltAbility& a, std::string_view) -> V {
        return a.spell > 0 ? spell_value(c, static_cast<std::uint32_t>(a.spell)) : V::nil();
    });

    // ---- ticks (MQ2TicksType): a timestamp whose plain value is ticks
    Type& ticks = const_cast<Type&>(*g.ticks);
    ticks.parent = g.timestamp;
    ticks.to_value = [](const Context&, const Value& v) { return V::integer(v.i / 6000); };

    // ---- spell
    auto sm = [](const char* name, auto fn) {
        add(g.spell, name, [fn](const Context& c, const Value& v, std::string_view index) -> V {
            const game::SpellData* s = spell_data(c, v.i);
            return s ? fn(c, *s, index) : V::nil();
        });
    };
    sm("Description", [](const Context& c, const game::SpellData& s, std::string_view) -> V {
        if (!c.conn) return V::nil();
        std::string d = c.conn->strings().db_string(static_cast<std::uint32_t>(s.description_id), 6);
        return d.empty() ? V::nil() : V::string(std::move(d));
    });
    sm("Subcategory", [](const Context& c, const game::SpellData& s, std::string_view) {
        std::string d = c.conn && s.subcategory > 0 ? c.conn->strings().db_string(static_cast<std::uint32_t>(s.subcategory), 5)
                                                    : std::string();
        return V::string(d.empty() ? "Unknown" : d);
    });
    sm("SubcategoryID", [](const Context&, const game::SpellData& s, std::string_view) { return V::integer(s.subcategory); });
    sm("DurationWindow", [](const Context&, const game::SpellData& s, std::string_view) { return V::integer(s.duration_window ? 1 : 0); });
    sm("EnduranceCost", [](const Context&, const game::SpellData& s, std::string_view) { return V::integer(s.endurance_cost); });
    sm("Dispellable", [](const Context&, const game::SpellData& s, std::string_view) { return V::boolean(!s.no_dispell); });
    sm("MyRange", [](const Context& c, const game::SpellData& s, std::string_view) {
        return V::number(s.range + (c.conn ? static_cast<float>(c.conn->spell_range_focus(s)) : 0.0f));
    });
    sm("Base", [](const Context&, const game::SpellData& s, std::string_view index) -> V {
        std::int64_t n = 0;
        if (!parse_int(index, n) || n < 1 || n > static_cast<std::int64_t>(s.base.size())) return V::nil();
        return V::integer(s.base[static_cast<std::size_t>(n - 1)]);
    });
    sm("SlowPct", [](const Context& c, const game::SpellData& s, std::string_view) { return V::integer(slow_pct(c, s)); });
    // MQ's WillLand: FindAffectSlot's slot + 1 on the player's buffs (0: it would not land).
    sm("WillLand", [](const Context& c, const game::SpellData& s, std::string_view) -> V {
        if (!c.conn) return V::nil();
        return V::integer(c.conn->spell_landing_slot(s.id) + 1);
    });
    // MQ's StacksWith[name]: WillStackWith(this, that): it would land over a buff of that spell alone.
    sm("StacksWith", [](const Context& c, const game::SpellData& s, std::string_view index) -> V {
        if (!c.conn || index.empty()) return V::nil();
        const game::SpellData* other = c.conn->spell_table().find_by_name(index);
        if (!other) {
            std::int64_t id = 0;
            if (parse_int(index, id)) other = spell_data(c, id);
        }
        if (!other) return V::nil();
        return V::boolean(c.conn->spell_landing_slot(s.id, other->id) != -1);
    });
    // MQ's StacksTarget: not too powerful for the target, and stacking with each buff the target is known to have.
    sm("StacksTarget", [](const Context& c, const game::SpellData& s, std::string_view) -> V {
        if (!c.conn) return V::nil();
        const game::Spawn* t = spawn_of(c, c.conn->world().target_id);
        if (!t) return V::nil();
        if (spell_too_powerful(c, *t, s)) return V::boolean(false);
        for (const auto& b : spawn_buffs(c, *t)) {
            const game::SpellData* d = spell_data(c, b.buff->spell);
            if (d && c.conn->spell_landing_slot(s.id, d->id) == -1) return V::boolean(false);
        }
        return V::boolean(true);
    });
    // MQ's WillLandPet: 0 when a pet buff will not stack with it, else the first empty pet buff slot + 1 (the
    // buffs after that slot are not looked at, as in MQ). Without a pet, 0.
    sm("WillLandPet", [](const Context& c, const game::SpellData& s, std::string_view) -> V {
        if (!c.conn) return V::nil();
        std::vector<std::pair<std::uint32_t, std::uint32_t>> buffs; // slot, spell
        for (const auto& [slot, b] : c.conn->pet_buffs())
            if (!b.empty()) buffs.emplace_back(slot, b.spell);
        std::sort(buffs.begin(), buffs.end());
        std::uint32_t next = 0;
        for (const auto& [slot, spell] : buffs) {
            if (slot != next) break; // an empty slot before this one
            if (c.conn->spell_landing_slot(s.id, spell) == -1) return V::integer(0);
            ++next;
        }
        return V::integer(std::int64_t{next} + 1);
    });

    // ---- item
    auto im = [](const char* name, auto fn) {
        add(g.item, name, [fn](const Context& c, const Value& v, std::string_view index) -> V {
            const net::Item* it = item_of(c, v);
            return it ? fn(c, v, *it, index) : V::nil();
        });
    };
    im("NoTrade", [](const Context&, const Value&, const net::Item& it, std::string_view) { return V::boolean(it.no_trade()); });
    im("Tradeskills", [](const Context&, const Value&, const net::Item& it, std::string_view) { return V::boolean(it.tradeskills != 0); });
    im("Clicky", [](const Context& c, const Value&, const net::Item& it, std::string_view) -> V {
        const auto& e = it.effects[net::kEffectClick];
        return e.present() ? spell_value(c, static_cast<std::uint32_t>(e.effect)) : V::nil();
    });
    // MQ's EffectType: the first of scroll, click (worn / inventory), worn or focus, combat.
    im("EffectType", [](const Context&, const Value&, const net::Item& it, std::string_view) -> V {
        if (it.effects[net::kEffectScroll].present()) return V::string("Spell Scroll");
        if (it.effects[net::kEffectClick].present()) {
            switch (it.effects[net::kEffectClick].type) {
            case 4: return V::string("Click Worn");
            case 1:
            case 5: return V::string("Click Inventory");
            default: return V::string("Click Unknown");
            }
        }
        if (it.effects[net::kEffectFocus].present() || it.effects[net::kEffectWorn].present()) return V::string("Worn");
        if (it.effects[net::kEffectProc].present()) return V::string("Combat");
        return V::nil();
    });
    // MQ's Timer: ticks until the click effect is ready, rounded up; 0 for one without a recast; NULL without one.
    im("Timer", [](const Context& c, const Value&, const net::Item& it, std::string_view) -> V {
        auto secs = item_click_seconds(c, it);
        return secs ? V::object(g.ticks, (*secs + 5) / 6 * 6000) : V::nil();
    });
    im("StackCount", [](const Context& c, const Value&, const net::Item& it, std::string_view) {
        std::int64_t n = 0;
        if (it.stackable())
            each_pack_item(c, [&](const net::Item& o) {
                if (o.id == it.id) n += o.stack_count;
            });
        return V::integer(n);
    });
    im("FreeStack", [](const Context& c, const Value&, const net::Item& it, std::string_view) {
        std::int64_t n = 0;
        if (it.stackable())
            each_pack_item(c, [&](const net::Item& o) {
                if (o.id == it.id) n += std::max<std::int64_t>(std::int64_t{o.max_stack} - o.stack_count, 0);
            });
        return V::integer(n);
    });
    // MQ's WornSlot[n]: the n-th slot the item may be worn in; WornSlot[name]: whether it may be worn there.
    im("WornSlot", [](const Context&, const Value&, const net::Item& it, std::string_view index) -> V {
        std::int64_t n = 0;
        if (parse_int(index, n)) {
            if (n < 1) return V::nil();
            for (int bit = 0; bit < 32; ++bit)
                if ((it.equip_slots & (1u << bit)) && --n == 0) return V::integer(bit);
            return V::nil();
        }
        const int slot = slot_by_name(index);
        return V::boolean(slot >= 0 && slot < 32 && (it.equip_slots & (1u << slot)));
    });
    im("ItemLink", [](const Context& c, const Value&, const net::Item& it, std::string_view index) {
        std::string link = game::make_item_link(it, c.conn->server_ext().wide_item_links(*c.conn));
        // Without CLICKABLE, the link's text without the \x12 tags around it (MQ's GetItemLink).
        if (!iequals(index, "CLICKABLE") && link.size() >= 2 && link.front() == '\x12' && link.back() == '\x12')
            link = link.substr(1, link.size() - 2);
        return V::string(std::move(link));
    });
    // The augment sockets of a common item: Augs counts the visible ones, AugSlot[n] is socket n.
    im("Augs", [](const Context&, const Value&, const net::Item& it, std::string_view) {
        std::int64_t n = 0;
        if (it.item_class == 0)
            for (const auto& s : it.aug_sockets) n += s.type != 0 && s.visible;
        return V::integer(n);
    });
    im("AugSlot", [](const Context&, const Value& v, const net::Item& it, std::string_view index) -> V {
        std::int64_t n = 0;
        if (!parse_int(index, n) || n < 1 || n > static_cast<std::int64_t>(it.aug_sockets.size()) || it.item_class != 0)
            return V::nil();
        net::ItemSlot at = unpack_slot(v.i);
        if (at.aug >= 0) return V::nil(); // an augment has no sockets
        at.aug = static_cast<std::int16_t>(n - 1);
        return V::object(g.aug, pack_slot(at));
    });

    // ---- augtype (MQ2AugType): a socket; its plain value is the augment's name
    Type& aug = const_cast<Type&>(*g.aug);
    aug.to_value = [](const Context& c, const Value& v) {
        const net::Item* it = item_of(c, v);
        return it ? V::string(it->name) : V::nil();
    };
    auto socket_of = [](const Context& c, const Value& v) -> const net::Item::AugSocket* {
        net::ItemSlot at = unpack_slot(v.i);
        const std::int16_t n = at.aug;
        at.aug = -1;
        const net::Item* host = item_of(c, V::object(g.item, pack_slot(at))); // a corpse's item too
        return host && n >= 0 && n < static_cast<std::int16_t>(host->aug_sockets.size())
                   ? &host->aug_sockets[static_cast<std::size_t>(n)] : nullptr;
    };
    add(g.aug, "Name", [](const Context& c, const Value& v, std::string_view) -> V {
        const net::Item* it = item_of(c, v);
        return it ? V::string(it->name) : V::nil();
    });
    add(g.aug, "Item", [](const Context& c, const Value& v, std::string_view) -> V {
        return item_of(c, v) ? V::object(g.item, v.i) : V::nil();
    });
    add(g.aug, "Empty", [](const Context& c, const Value& v, std::string_view) { return V::boolean(item_of(c, v) == nullptr); });
    add(g.aug, "Slot", [](const Context&, const Value& v, std::string_view) { return V::integer(unpack_slot(v.i).aug + 1); });
    add(g.aug, "Type", [socket_of](const Context& c, const Value& v, std::string_view) -> V {
        const auto* s = socket_of(c, v);
        return s ? V::integer(s->type) : V::nil();
    });
    add(g.aug, "Visible", [socket_of](const Context& c, const Value& v, std::string_view) -> V {
        const auto* s = socket_of(c, v);
        return s ? V::boolean(s->visible != 0) : V::nil();
    });

    // ---- spawns
    spawn_member("Aggressive", [](const Context&, const game::Spawn& s, std::string_view) {
        return V::boolean((s.player_state & 0xcu) != 0); // PlayerState 4 or 8, as MQ tests it
    });
    spawn_member("Primary", [](const Context&, const game::Spawn& s, std::string_view) { return V::integer(s.appearance.material[7]); });
    spawn_member("Secondary", [](const Context&, const game::Spawn& s, std::string_view) { return V::integer(s.appearance.material[8]); });
    spawn_member("Body", [](const Context&, const game::Spawn& s, std::string_view) { return V::object(g.body, s.body_type); });
    Type& body = const_cast<Type&>(*g.body);
    auto body_name = [](std::int64_t id) -> V {
        if (id < 0 || id >= static_cast<std::int64_t>(std::size(game::szBodyType))) return V::nil();
        return V::string(std::string(game::szBodyType[static_cast<std::size_t>(id)]));
    };
    body.to_value = [body_name](const Context&, const Value& v) { return body_name(v.i); };
    add(g.body, "ID", [](const Context&, const Value& v, std::string_view) { return V::integer(v.i); });
    add(g.body, "Name", [body_name](const Context&, const Value& v, std::string_view) { return body_name(v.i); });

    // ---- target (MQ2TargetType) and xtarget (MQ2XTargetType): a spawn with the aggro meter's readings
    const_cast<Type&>(*g.target).parent = g.spawn;
    add(g.target, "PctAggro", [](const Context& c, const Value&, std::string_view) { return aggro_pct(c, net::kAggroPlayer); });
    add(g.target, "SecondaryPctAggro", [](const Context& c, const Value&, std::string_view) {
        return aggro_pct(c, net::kAggroSecondary);
    });
    add(g.target, "SecondaryAggroPlayer", [](const Context& c, const Value&, std::string_view) -> V {
        return c.conn && c.conn->aggro().secondary_id ? spawn_value(spawn_of(c, c.conn->aggro().secondary_id)) : V::nil();
    });
    const_cast<Type&>(*g.xtarget).parent = g.spawn;
    add(g.xtarget, "PctAggro", [](const Context& c, const Value& v, std::string_view) {
        return aggro_pct(c, net::kAggroXTarget1 + static_cast<int>(v.f));
    });

    // ---- group
    // Present: the members other than the player whose spawn is in the zone.
    add(g.group, "Present", [](const Context& c, const Value&, std::string_view) {
        auto members = group_members(c);
        std::int64_t n = 0;
        for (std::size_t i = 1; i < members.size(); ++i)
            if (members[i] && spawn_named(c, members[i]->name)) ++n;
        return V::integer(n);
    });
    // Injured[pct] and LowMana[pct]: the members in the zone (the player too) with HP or mana above 0 and under it.
    auto count_under = [](const char* mine, bool hp) {
        return [mine, hp](const Context& c, const Value&, std::string_view index) {
            std::int64_t threshold = 0, n = 0;
            if (!parse_int(index, threshold) || threshold == 0) return V::integer(0);
            auto members = group_members(c);
            for (std::size_t i = 0; i < members.size(); ++i) {
                std::int64_t pct = -1;
                if (i == 0) {
                    V v = character_member(c, mine);
                    if (v.kind == Value::Kind::Int) pct = v.i;
                } else if (members[i] && !members[i]->offline) {
                    const game::Spawn* s = spawn_named(c, members[i]->name);
                    if (s && hp) {
                        pct = s->hp_percent;
                    } else if (s) {
                        const auto& m = c.conn->group().mana_percent;
                        if (auto it = m.find(s->id); it != m.end()) pct = it->second;
                    }
                }
                if (pct > 0 && pct < threshold) ++n;
            }
            return V::integer(n);
        };
    };
    add(g.group, "Injured", count_under("PctHPs", true));
    add(g.group, "LowMana", count_under("PctMana", false));
    add(g.member, "PctAggro", [](const Context& c, const Value& v, std::string_view) -> V {
        if (v.i < 0 || v.i >= 6) return V::nil();
        return aggro_pct(c, v.i == 0 ? net::kAggroPlayer : net::kAggroGroup1 + static_cast<int>(v.i) - 1);
    });
}

// ---------------------------------------------------------------- Raid, Corpse and more of Me (MacroQuest's meanings)

// A raid member by its value's name (the members' list moves as the raid changes; the name stays).
const game::RaidMember* raid_member_of(const Context& c, const Value& v) {
    if (!c.conn || !c.conn->raid().active) return nullptr;
    for (const auto& m : c.conn->raid().members)
        if (iequals(m.name, v.s)) return &m;
    return nullptr;
}

V raid_member_value(const game::RaidMember& m, std::size_t index) {
    return V::object(g.raidmember, static_cast<std::int64_t>(index) + 1, 0, m.name);
}

// The open loot window's corpse session, or null.
const net::GameConnection::LootSession* open_loot(const Context& c) {
    if (!c.conn) return nullptr;
    const auto& l = c.conn->loot_session();
    return l && l->open ? &*l : nullptr;
}

// The spell's counter effects (SPA 35, 36, 116, 369; game/counter_spas.h): MQ's CounterType names the last, its
// CounterNumber is the first one's base.
const char* counter_name(std::int32_t spa) {
    switch (spa) {
    case 35: return "Disease";
    case 36: return "Poison";
    case 116: return "Curse";
    case 369: return "Corruption";
    default: return nullptr;
    }
}

void register_e3_members2() {
    // ---- raid (MQ2RaidType): always there; its plain value is NULL, as MQ's
    Type& raid = const_cast<Type&>(*g.raid);
    raid.to_value = [](const Context&, const Value&) { return V::nil(); };
    auto members = [](const Context& c) -> const std::vector<game::RaidMember>* {
        return c.conn && c.conn->raid().active ? &c.conn->raid().members : nullptr;
    };
    add(g.raid, "Members", [members](const Context& c, const Value&, std::string_view) {
        const auto* m = members(c);
        return V::integer(m ? static_cast<std::int64_t>(m->size()) : 0);
    });
    add(g.raid, "Member", [members](const Context& c, const Value&, std::string_view index) -> V {
        const auto* m = members(c);
        if (!m || index.empty()) return V::nil();
        std::int64_t n = 0;
        if (parse_int(index, n)) {
            if (n < 1 || n > static_cast<std::int64_t>(m->size())) return V::nil();
            return raid_member_value((*m)[static_cast<std::size_t>(n - 1)], static_cast<std::size_t>(n - 1));
        }
        for (std::size_t i = 0; i < m->size(); ++i)
            if (iequals((*m)[i].name, index)) return raid_member_value((*m)[i], i);
        return V::nil();
    });
    add(g.raid, "Leader", [members](const Context& c, const Value&, std::string_view) -> V {
        const auto* m = members(c);
        if (!m) return V::nil();
        for (std::size_t i = 0; i < m->size(); ++i)
            if (iequals((*m)[i].name, c.conn->raid().leader)) return raid_member_value((*m)[i], i);
        return V::nil();
    });
    add(g.raid, "Target", [members](const Context& c, const Value&, std::string_view) -> V {
        const auto* m = members(c);
        if (!m) return V::nil();
        for (std::size_t i = 0; i < m->size(); ++i)
            if (iequals((*m)[i].name, c.conn->raid().target)) return raid_member_value((*m)[i], i);
        return V::nil();
    });
    add(g.raid, "Locked", [](const Context& c, const Value&, std::string_view) {
        return V::boolean(c.conn && c.conn->raid().active && c.conn->raid().locked);
    });
    add(g.raid, "LootType", [](const Context& c, const Value&, std::string_view) {
        return V::integer(c.conn && c.conn->raid().active ? c.conn->raid().loot_type : 0);
    });
    add(g.raid, "TotalLevels", [members](const Context& c, const Value&, std::string_view) {
        std::int64_t sum = 0;
        if (const auto* m = members(c))
            for (const auto& r : *m) sum += r.level;
        return V::integer(sum);
    });
    add(g.raid, "AverageLevel", [members](const Context& c, const Value&, std::string_view) {
        const auto* m = members(c);
        if (!m || m->empty()) return V::number(0.0);
        double sum = 0;
        for (const auto& r : *m) sum += r.level;
        return V::number(sum / static_cast<double>(m->size()));
    });

    // ---- raidmember (MQ2RaidMemberType): its plain value is the name; other members are its spawn's (in the zone)
    Type& rm = const_cast<Type&>(*g.raidmember);
    rm.parent = g.spawn;
    rm.convert = [](const Context& c, const Value& v) { return spawn_value(spawn_named(c, v.s)); };
    rm.to_value = [](const Context& c, const Value& v) { return raid_member_of(c, v) ? V::string(v.s) : V::nil(); };
    auto rmm = [](const char* name, auto fn) {
        add(g.raidmember, name, [fn](const Context& c, const Value& v, std::string_view index) -> V {
            const game::RaidMember* m = raid_member_of(c, v);
            return m ? fn(c, *m, index) : V::nil();
        });
    };
    rmm("Name", [](const Context&, const game::RaidMember& m, std::string_view) { return V::string(m.name); });
    // 1..12; 0 ungrouped (MQ's GroupNumber + 1, with the ungrouped 0xffffffff wrapping to 0).
    rmm("Group", [](const Context&, const game::RaidMember& m, std::string_view) {
        return V::integer(m.group == 0xffffffffu ? 0 : std::int64_t{m.group} + 1);
    });
    rmm("GroupLeader", [](const Context&, const game::RaidMember& m, std::string_view) { return V::boolean(m.group_leader); });
    rmm("RaidLeader", [](const Context& c, const game::RaidMember& m, std::string_view) {
        return V::boolean(iequals(m.name, c.conn->raid().leader));
    });
    // MQ's Looter: the leader; with loot type 2 the group leaders; with 3 the named looters.
    rmm("Looter", [](const Context& c, const game::RaidMember& m, std::string_view) {
        const auto& r = c.conn->raid();
        if (iequals(m.name, r.leader)) return V::boolean(true);
        if (r.loot_type == 2) return V::boolean(m.group_leader);
        if (r.loot_type == 3)
            for (const auto& name : r.looters)
                if (iequals(name, m.name)) return V::boolean(true);
        return V::boolean(false);
    });
    rmm("Spawn", [](const Context& c, const game::RaidMember& m, std::string_view) { return spawn_value(spawn_named(c, m.name)); });
    rmm("Level", [](const Context&, const game::RaidMember& m, std::string_view) { return V::integer(m.level); });
    rmm("Class", [](const Context&, const game::RaidMember& m, std::string_view) { return V::object(g.cls, m.class_); });

    // ---- corpse (MQ2CorpseType): the loot window's corpse; its plain value is TRUE while one is open. Other members
    // are the corpse spawn's.
    Type& corpse = const_cast<Type&>(*g.corpse);
    corpse.parent = g.spawn;
    corpse.convert = [](const Context& c, const Value&) {
        const auto* l = open_loot(c);
        return l ? spawn_value(spawn_of(c, l->corpse_id)) : V::nil();
    };
    corpse.to_value = [](const Context& c, const Value&) { return V::boolean(open_loot(c) != nullptr); };
    add(g.corpse, "Open", [](const Context& c, const Value&, std::string_view) { return V::boolean(open_loot(c) != nullptr); });
    add(g.corpse, "Items", [](const Context& c, const Value&, std::string_view) -> V {
        const auto* l = open_loot(c);
        return l ? V::integer(static_cast<std::int64_t>(l->items.size())) : V::nil();
    });
    // Item[n]: loot window slot n (1-based, as the window numbers its slots); Item[name], or [=name], by name.
    add(g.corpse, "Item", [](const Context& c, const Value&, std::string_view index) -> V {
        const auto* l = open_loot(c);
        if (!l || index.empty()) return V::nil();
        std::int64_t n = 0;
        if (parse_int(index, n)) {
            if (n < 1 || n > net::kCorpseSlotEnd - net::kCorpseSlotBegin + 1) return V::nil();
            const auto slot = static_cast<std::int16_t>(net::kCorpseSlotBegin + n - 1);
            return l->items.contains(slot) ? V::object(g.item, pack_slot(net::ItemSlot::corpse(slot))) : V::nil();
        }
        for (const auto& [slot, it] : l->items)
            if (item_name_matches(it, index)) return V::object(g.item, pack_slot(net::ItemSlot::corpse(slot)));
        return V::nil();
    });

    // ---- pet (MQ2PetType): the player's pet, a spawn whose Buff and BuffDuration read the pet window (MQ's
    // pPetInfoWnd), whether or not the server sent its buffs as a target's
    Type& pet = const_cast<Type&>(*g.pet);
    pet.parent = g.spawn;
    // Buff[n]: pet window slot n (NULL when empty); Buff[name]: the slot (1-based) of the first whose name starts so.
    auto pet_buff = [](const Context& c, std::string_view index) -> V {
        if (!c.conn || index.empty()) return V::nil();
        std::int64_t n = 0;
        if (parse_int(index, n)) {
            if (n < 1) return V::nil();
            V v = V::object(g.buff, n - 1, kPetBuffs);
            return buff_of(c, v) ? v : V::nil();
        }
        std::vector<std::pair<std::uint32_t, std::uint32_t>> list; // slot, spell
        for (const auto& [slot, b] : c.conn->pet_buffs())
            if (!b.empty()) list.emplace_back(slot, b.spell);
        std::sort(list.begin(), list.end());
        for (const auto& [slot, spell] : list) {
            const game::SpellData* d = spell_data(c, spell);
            if (d && d->name.size() >= index.size() && iequals(std::string_view(d->name).substr(0, index.size()), index))
                return V::integer(std::int64_t{slot} + 1);
        }
        return V::nil();
    };
    add(g.pet, "Buff", [pet_buff](const Context& c, const Value&, std::string_view index) { return pet_buff(c, index); });
    // BuffDuration[n|name]: the time left on that pet buff; NULL for none or a permanent one.
    add(g.pet, "BuffDuration", [pet_buff](const Context& c, const Value&, std::string_view index) -> V {
        V b = pet_buff(c, index);
        if (b.kind == Value::Kind::Int) b = V::object(g.buff, b.i - 1, kPetBuffs);
        const game::BuffSlot* s = b.kind == Value::Kind::Object ? buff_of(c, b) : nullptr;
        const std::int64_t ms = s ? buff_ms_left(c, *s) : -1;
        return ms < 0 ? V::nil() : V::object(g.timestamp, ms);
    });

    // ---- Me
    // MQ: the buff that makes the player invulnerable (SPA 40), NULL when none does.
    me_member("Invulnerable", [](const Context& ctx, const GameConnection&, std::string_view) -> V {
        const game::Spawn* s = me(ctx);
        return s ? buff_with_effect(ctx, *s, 40, [](std::int32_t) { return true; }) : V::nil();
    });
    me_member("CurrentFavor", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.tribute().points); });
    me_member("CareerFavor", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.tribute().career); });
    me_member("TributeActive", [](const Context&, const GameConnection& c, std::string_view) {
        return V::boolean(c.tribute().settings.active);
    });
    me_member("SpellShieldBonus", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.spell_shield_bonus()); });
    me_member("SpellDamageBonus", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.spell_damage_bonus()); });
    // MQ: PetBuff[n] is the spell in pet buff slot n, PetBuff[name] the slot (1-based) that holds it.
    me_member("PetBuff", [](const Context& ctx, const GameConnection& c, std::string_view index) -> V {
        if (index.empty()) return V::nil();
        std::int64_t n = 0;
        if (parse_int(index, n)) {
            if (n < 1) return V::nil();
            for (const auto& [slot, b] : c.pet_buffs())
                if (static_cast<std::int64_t>(slot) == n - 1 && !b.empty()) return spell_value(ctx, b.spell);
            return V::nil();
        }
        for (const auto& [slot, b] : c.pet_buffs()) {
            const game::SpellData* s = b.empty() ? nullptr : spell_data(ctx, b.spell);
            if (s && iequals(s->name, index)) return V::integer(std::int64_t{slot} + 1);
        }
        return V::nil();
    });
    // MQ's spawn Mount: the spawn it rides.
    spawn_member("Mount", [](const Context& c, const game::Spawn& s, std::string_view) -> V {
        return s.mount_id ? spawn_value(spawn_of(c, s.mount_id)) : V::nil();
    });

    // ---- spell: the counters it carries
    add(g.spell, "CounterType", [](const Context& c, const Value& v, std::string_view) -> V {
        const game::SpellData* s = spell_data(c, v.i);
        if (!s) return V::nil();
        const char* name = "None";
        for (std::int32_t spa : s->effect)
            if (const char* n = counter_name(spa)) name = n;
        return V::string(name);
    });
    add(g.spell, "CounterNumber", [](const Context& c, const Value& v, std::string_view) -> V {
        const game::SpellData* s = spell_data(c, v.i);
        if (!s) return V::nil();
        for (std::size_t e = 0; e < s->effect.size(); ++e)
            if (counter_name(s->effect[e])) return V::integer(s->base[e]);
        return V::integer(0);
    });
}

// ---------------------------------------------------------------- the bank, InvSlot (MacroQuest's meanings)

// RoF2's bank slots (NUM_BANK_SLOTS) and MQ's InvSlot numbers: 0..33 the player's slots, 262.. the slots in the
// packs (ten a pack), 2000.. the bank, 2032.. the slots in the bank's bags, 2500.. the shared bank, 2532.. the slots in
// its bags.
constexpr int kBankSlots = game::kBankSlotsBig;
constexpr int kInvSlotPackItems = 262, kInvSlotBank = 2000, kInvSlotBankItems = 2032, kInvSlotShared = 2500,
              kInvSlotSharedItems = 2532;

// An InvSlot number as a place in the client's inventory; nullopt for one it does not hold (the trade and world
// container slots MQ also numbers).
std::optional<net::ItemSlot> inv_slot_place(std::int64_t n) {
    auto in = [](std::int16_t type, std::int64_t slot, std::int64_t sub) {
        return net::ItemSlot{type, static_cast<std::int16_t>(slot), static_cast<std::int16_t>(sub), -1, 0, 0};
    };
    if (n >= 0 && n <= net::kSlotCursor) return in(net::kInvPossessions, n, -1);
    const std::int64_t packs = net::kSlotGeneral10 - net::kSlotGeneral1 + 1;
    if (n >= kInvSlotPackItems && n < kInvSlotPackItems + packs * 10)
        return in(net::kInvPossessions, net::kSlotGeneral1 + (n - kInvSlotPackItems) / 10, (n - kInvSlotPackItems) % 10);
    if (n >= kInvSlotBank && n < kInvSlotBank + kBankSlots) return in(net::kInvBank, n - kInvSlotBank, -1);
    if (n >= kInvSlotBankItems && n < kInvSlotBankItems + kBankSlots * 10)
        return in(net::kInvBank, (n - kInvSlotBankItems) / 10, (n - kInvSlotBankItems) % 10);
    if (n >= kInvSlotShared && n < kInvSlotShared + game::kSharedBankSlots) return in(net::kInvSharedBank, n - kInvSlotShared, -1);
    if (n >= kInvSlotSharedItems && n < kInvSlotSharedItems + game::kSharedBankSlots * 10)
        return in(net::kInvSharedBank, (n - kInvSlotSharedItems) / 10, (n - kInvSlotSharedItems) % 10);
    return std::nullopt;
}

// An InvSlot index: a number, or a name (MQ's ItemSlotMap: the worn slots' names, pack1..pack10, cursor, bank1..bank24,
// sharedbank1..2); nullopt for neither.
std::optional<std::int64_t> inv_slot_number(std::string_view index) {
    std::int64_t n = 0;
    if (parse_int(index, n)) return n;
    if (int s = slot_by_name(index); s >= 0) return s;
    auto numbered = [&](std::string_view prefix, std::int64_t first, std::int64_t count) -> std::optional<std::int64_t> {
        if (index.size() <= prefix.size() || !iequals(index.substr(0, prefix.size()), prefix)) return std::nullopt;
        std::int64_t k = 0;
        if (!parse_int(index.substr(prefix.size()), k) || k < 1 || k > count) return std::nullopt;
        return first + k - 1;
    };
    if (auto v = numbered("sharedbank", kInvSlotShared, game::kSharedBankSlots)) return v;
    if (auto v = numbered("bank", kInvSlotBank, kBankSlots)) return v;
    return std::nullopt;
}

void register_e3_members3() {
    // ---- Me: the bank
    // MQ's Bank[n]: bank slot n (1-based) while within the slots the account has; past RoF2's 24, the shared bank's
    // (25 the first). NULL for an empty slot.
    me_member("Bank", [](const Context& ctx, const GameConnection& c, std::string_view index) -> V {
        std::int64_t n = 0;
        if (!parse_int(index, n) || n < 1) return V::nil();
        const int available = game::bank_slot_count(c.profile() ? c.profile()->expansions : 0, c.inventory());
        if (n - 1 < available) return item_value(ctx, {net::kInvBank, static_cast<std::int16_t>(n - 1), -1, -1, 0, 0});
        if (n - 1 >= kBankSlots && n - 1 - kBankSlots < game::kSharedBankSlots)
            return item_value(ctx, {net::kInvSharedBank, static_cast<std::int16_t>(n - 1 - kBankSlots), -1, -1, 0, 0});
        return V::nil();
    });
    me_member("SharedBank", [](const Context& ctx, const GameConnection&, std::string_view index) -> V {
        std::int64_t n = 0;
        if (!parse_int(index, n) || n < 1 || n > game::kSharedBankSlots) return V::nil();
        return item_value(ctx, {net::kInvSharedBank, static_cast<std::int16_t>(n - 1), -1, -1, 0, 0});
    });
    me_member("PlatinumBank", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.bank_coins().platinum); });
    me_member("GoldBank", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.bank_coins().gold); });
    me_member("SilverBank", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.bank_coins().silver); });
    me_member("CopperBank", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.bank_coins().copper); });
    me_member("PlatinumShared", [](const Context&, const GameConnection& c, std::string_view) {
        return V::integer(c.bank_coins().shared_platinum);
    });
    me_member("CashBank", [](const Context&, const GameConnection& c, std::string_view) {
        const auto& b = c.bank_coins();
        return V::integer(std::int64_t{b.platinum} * 1000 + std::int64_t{b.gold} * 100 + std::int64_t{b.silver} * 10 + b.copper);
    });

    // ---- invslot (MQ2InvSlotType): an InvSlot number; its plain value is the number
    Type& inv = const_cast<Type&>(*g.invslot);
    inv.to_value = [](const Context&, const Value& v) { return V::integer(v.i); };
    add(g.invslot, "ID", [](const Context&, const Value& v, std::string_view) { return V::integer(v.i); });
    add(g.invslot, "Item", [](const Context& c, const Value& v, std::string_view) -> V {
        auto place = inv_slot_place(v.i);
        return place ? item_value(c, *place) : V::nil();
    });
    // Pack: the slot (as an InvSlot) holding the bag this slot is in; 0 for a slot not in a bag. Slot: its place in that
    // bag (0-based).
    add(g.invslot, "Pack", [](const Context&, const Value& v, std::string_view) -> V {
        auto place = inv_slot_place(v.i);
        if (!place || place->sub < 0) return V::object(g.invslot, 0);
        if (place->type == net::kInvBank) return V::object(g.invslot, kInvSlotBank + place->slot);
        if (place->type == net::kInvSharedBank) return V::object(g.invslot, kInvSlotShared + place->slot);
        return V::object(g.invslot, place->slot);
    });
    add(g.invslot, "Slot", [](const Context&, const Value& v, std::string_view) -> V {
        auto place = inv_slot_place(v.i);
        return V::integer(place && place->sub >= 0 ? place->sub : 0);
    });
    add(g.invslot, "Name", [](const Context&, const Value& v, std::string_view) -> V {
        auto place = inv_slot_place(v.i);
        if (!place || place->sub >= 0) return V::nil();
        if (place->type == net::kInvBank) return V::string(std::format("bank{}", place->slot + 1));
        if (place->type == net::kInvSharedBank) return V::string(std::format("sharedbank{}", place->slot + 1));
        if (place->slot >= 0 && place->slot < static_cast<std::int16_t>(std::size(kSlotNames)))
            return V::string(kSlotNames[place->slot]);
        return V::nil();
    });
}

// ---------------------------------------------------------------- Merchant, DoorTarget, Mercenary and the rest

// The open merchant's session, or null.
const net::MerchantSession* open_merchant(const Context& c) {
    if (!c.conn) return nullptr;
    const auto& m = c.conn->merchant();
    return m && m->open ? &*m : nullptr;
}

// The player's mercenary spawn: the group's mercenary member the player owns, else a mercenary spawn whose owner
// is the player.
const game::Spawn* my_merc(const Context& c) {
    if (!c.conn) return nullptr;
    const std::string& me_name = c.conn->character_name();
    for (const auto& m : c.conn->group().slots)
        if (m.merc && !m.name.empty() && iequals(m.owner_name, me_name))
            if (const game::Spawn* s = spawn_named(c, m.name)) return s;
    for (const auto& [id, s] : c.conn->world().spawns)
        if (s.mercenary && !s.is_corpse && s.pet_owner_id == c.conn->world().player_id) return &s;
    return nullptr;
}

// MQ's GetMercenaryStateString: NONE without one (SUSPENDED while one is chosen), else DEAD, ACTIVE or SUSPENDED.
const char* merc_state(const GameConnection& c) {
    const auto& m = c.merc();
    if (!m.has_mercenary) return m.current_index != -1 ? "SUSPENDED" : "NONE";
    if (m.state == game::kMercDead) return "DEAD";
    if (m.state == game::kMercActive) return "ACTIVE";
    return "SUSPENDED";
}

void register_e3_members4() {
    // ---- merchant (MQ2MerchantType): the merchant open; its plain value TRUE while one is. Its other members are the
    // merchant spawn's.
    Type& merchant = const_cast<Type&>(*g.merchant);
    merchant.parent = g.spawn;
    merchant.convert = [](const Context& c, const Value&) {
        const auto* m = open_merchant(c);
        return m ? spawn_value(spawn_of(c, m->npc_id)) : V::nil();
    };
    merchant.to_value = [](const Context& c, const Value&) { return V::boolean(open_merchant(c) != nullptr); };
    add(g.merchant, "Open", [](const Context& c, const Value&, std::string_view) { return V::boolean(open_merchant(c) != nullptr); });
    // MQ: TRUE once the merchant window's list has been filled (PurchasePageHandler::UpdateList), FALSE when it closes.
    // Here: the merchant is open and its stock has come. A merchant with nothing for sale stays FALSE (docs/unknowns.md).
    add(g.merchant, "ItemsReceived", [](const Context& c, const Value&, std::string_view) {
        const auto* m = open_merchant(c);
        return V::boolean(m && !m->items.empty());
    });
    add(g.merchant, "Items", [](const Context& c, const Value&, std::string_view) -> V {
        const auto* m = open_merchant(c);
        return m ? V::integer(static_cast<std::int64_t>(m->items.size())) : V::nil();
    });
    add(g.merchant, "Markup", [](const Context& c, const Value&, std::string_view) -> V {
        const auto* m = open_merchant(c);
        return m ? V::number(m->rate) : V::nil();
    });
    // Item[n]: the nth item of the stock, in the list's order; Item[name], [=name] exact, by name.
    add(g.merchant, "Item", [](const Context& c, const Value&, std::string_view index) -> V {
        const auto* m = open_merchant(c);
        if (!m || index.empty()) return V::nil();
        auto value = [](std::uint32_t slot) {
            return V::object(g.item, pack_slot({net::kInvMerchant, static_cast<std::int16_t>(slot), -1, -1, 0, 0}));
        };
        std::int64_t n = 0;
        if (parse_int(index, n)) {
            if (n < 1 || n > static_cast<std::int64_t>(m->items.size())) return V::nil();
            auto it = m->items.begin();
            std::advance(it, n - 1);
            return value(it->first);
        }
        for (const auto& [slot, it] : m->items)
            if (item_name_matches(it, index)) return value(slot);
        return V::nil();
    });

    // ---- switch (MQ2SwitchType): a door of this zone; its plain value the id
    Type& sw = const_cast<Type&>(*g.door);
    auto door_of = [](const Context& c, const Value& v) -> const game::Door* {
        return c.conn && v.i >= 0 && v.i <= 255 ? c.conn->door(static_cast<std::uint8_t>(v.i)) : nullptr;
    };
    sw.to_value = [door_of](const Context& c, const Value& v) { return door_of(c, v) ? V::integer(v.i) : V::nil(); };
    auto dm = [door_of](const char* name, auto fn) {
        add(g.door, name, [door_of, fn](const Context& c, const Value& v, std::string_view index) -> V {
            const game::Door* d = door_of(c, v);
            return d ? fn(c, *d, index) : V::nil();
        });
    };
    // MQ's X, Y, Z are the server's (the /loc numbers), as a spawn's.
    dm("ID", [](const Context&, const game::Door& d, std::string_view) { return V::integer(d.id); });
    dm("Name", [](const Context&, const game::Door& d, std::string_view) { return V::string(d.name); });
    dm("X", [](const Context&, const game::Door& d, std::string_view) { return V::number(d.x); });
    dm("Y", [](const Context&, const game::Door& d, std::string_view) { return V::number(d.y); });
    dm("Z", [](const Context&, const game::Door& d, std::string_view) { return V::number(d.z); });
    dm("DefaultX", [](const Context&, const game::Door& d, std::string_view) { return V::number(d.default_x); });
    dm("DefaultY", [](const Context&, const game::Door& d, std::string_view) { return V::number(d.default_y); });
    dm("DefaultZ", [](const Context&, const game::Door& d, std::string_view) { return V::number(d.default_z); });
    dm("Heading", [](const Context&, const game::Door& d, std::string_view) { return heading_value(eq_heading_to_degrees(d.heading)); });
    dm("Open", [](const Context&, const game::Door& d, std::string_view) { return V::boolean(d.state == game::kSwitchOpen); });
    dm("State", [](const Context&, const game::Door& d, std::string_view) { return V::integer(d.state); });
    dm("IsTargeted", [](const Context& c, const game::Door& d, std::string_view) {
        return V::boolean(c.conn->door_target() == d.id);
    });
    // MQ's Distance: on the flat from the player, less half the door's bounding radius. MQ takes the actor's; here the
    // door's scale stands for it, as MQ does for a door without an actor.
    auto door_distance = [](const Context& c, const game::Door& d, bool flat) -> V {
        const game::Spawn* p = me(c);
        if (!p) return V::nil();
        glm::vec3 to = d.world_position() - spawn_position(*p);
        if (flat) to.z = 0;
        return V::number(std::max(0.0f, glm::length(to) - d.scale() / 2.0f));
    };
    dm("Distance", [door_distance](const Context& c, const game::Door& d, std::string_view) { return door_distance(c, d, true); });
    dm("Distance3D", [door_distance](const Context& c, const game::Door& d, std::string_view) { return door_distance(c, d, false); });
    dm("HeadingTo", [](const Context& c, const game::Door& d, std::string_view) -> V {
        const game::Spawn* p = me(c);
        return p ? heading_value(heading_between(spawn_position(*p), d.world_position())) : V::nil();
    });

    // ---- mercenary (MQ2MercenaryType): the player's; its plain value its state. Other members are its spawn's.
    Type& merc = const_cast<Type&>(*g.mercenary);
    merc.parent = g.spawn;
    merc.convert = [](const Context& c, const Value&) { return spawn_value(my_merc(c)); };
    merc.to_value = [](const Context& c, const Value&) { return c.conn ? V::string(merc_state(*c.conn)) : V::nil(); };
    add(g.mercenary, "State", [](const Context& c, const Value&, std::string_view) { return V::string(merc_state(*c.conn)); });
    add(g.mercenary, "StateID", [](const Context& c, const Value&, std::string_view) { return V::integer(c.conn->merc().state); });
    add(g.mercenary, "Index", [](const Context& c, const Value&, std::string_view) {
        return V::integer(std::int64_t{c.conn->merc().current_index} + 1);
    });
    // MQ's Name: the spawn's (raw) name, else the state.
    add(g.mercenary, "Name", [](const Context& c, const Value&, std::string_view) {
        const game::Spawn* s = my_merc(c);
        return V::string(s ? s->name : merc_state(*c.conn));
    });

    // ---- Me, EverQuest, Navigation
    me_member("TargetOfTarget", [](const Context& ctx, const GameConnection& c, std::string_view) -> V {
        return c.target_of_target() ? spawn_value(spawn_of(ctx, c.target_of_target())) : V::nil();
    });
    me_member("Instance", [](const Context&, const GameConnection& c, std::string_view) { return V::integer(c.zone_instance()); });
    // MQ's Ping: the connection's average ping (the stat request round trips, as /netstats shows them).
    add(g.everquest, "Ping", [](const Context& c, const Value&, std::string_view) -> V {
        if (!c.conn) return V::integer(0);
        auto stats = c.conn->in_zone() ? c.conn->zone_session_stats() : c.conn->world_session_stats();
        return V::integer(stats ? stats->ping_average : 0);
    });
    add(g.everquest, "Foreground", [](const Context& c, const Value&, std::string_view) {
        return V::boolean(c.foreground && c.foreground());
    });
    // MQ2Nav's Velocity: the player's speed, units a second (its speed per 50 ms tick x 20), rounded.
    add(g.navigation, "Velocity", [](const Context& c, const Value&, std::string_view) {
        return V::integer(c.conn ? std::lround(glm::length(c.conn->player_velocity()) * 20.0f) : 0);
    });
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
    // Behind and Stopped (MQ2MoveUtils'): about the stick's spawn while sticking, else the target, as MQ2MoveUtils
    // reads its held spawn or the target. Stopped: within the stick's distance on the flat (without a stick, the
    // target's MaxRangeTo, where MQ2MoveUtils falls back on the melee range). Behind: Stopped, and the player within
    // 45 degrees of the spawn's back by where it stands, the arc /stick behind holds to (MQ2MoveUtils compares the
    // two headings, within 45 of its 512 units: Hailstone's own measure).
    struct Place {
        float away = 0.0f, reach = 0.0f;
        bool behind = false;
    };
    auto place = [status](const Context& c) -> std::optional<Place> {
        auto s = status(c);
        if (!s || !c.conn) return std::nullopt;
        const game::Spawn* me = c.conn->world().player();
        const game::Spawn* sp = spawn_of(c, s->stick_spawn ? s->stick_spawn : c.conn->world().target_id);
        if (!me || !sp || sp == me) return std::nullopt;
        Place p;
        p.reach = s->stick_spawn ? s->stick_distance : std::max(sp->melee_radius, 14.0f);
        const glm::vec2 to_me = glm::vec2(spawn_position(*me) - spawn_position(*sp));
        p.away = glm::length(to_me);
        const float yaw = game::heading_to_yaw(sp->display_valid ? sp->display_heading : sp->heading);
        const glm::vec2 facing(std::cos(yaw), std::sin(yaw));
        p.behind = p.away > 0.01f && glm::dot(to_me / p.away, -facing) >= std::cos(glm::radians(45.0f));
        return p;
    };
    add(g.stick, "Stopped", [place](const Context& c, const Value&, std::string_view) {
        auto p = place(c);
        return V::boolean(p && p->away <= p->reach);
    });
    add(g.stick, "Behind", [place](const Context& c, const Value&, std::string_view) {
        auto p = place(c);
        return V::boolean(p && p->away <= p->reach && p->behind);
    });
    Type& t = const_cast<Type&>(*g.stick);
    t.to_value = [on](const Context& c, const Value&) { return V::string(on(c) ? "ON" : "OFF"); };
}

// The MoveTo TLO (MQ2MoveUtils' members, over the client's /moveto: NavStatus::moveto): Moving, Stopped (the last
// /moveto arrived; cleared by the next one), ArrivalDist, Broken; "ON", "PAUSED" or "OFF" as its text. Broken is
// MQ2MoveUtils' walk ended by aggro or a hit, which Hailstone's never is: always FALSE. UseWalk, ArrivalDistY and
// ArrivalDistX are not here.
void register_moveto();

// Me.Bandolier[name|#] (MQ's CharacterMembers::Bandolier and MQ2BandolierType): a set by its name, any case, or its
// number (1..20, clamped as MQ clamps it); NULL for a name no set has. A set without a name answers NULL to every
// member, as MQ's GetMember fails on it. Its text is its name. i: the set's index (0..19).
//  - Active: each of its four items (primary, secondary, range, ammo) is the one worn in that slot, an empty one
//    matching an empty slot, as MQ compares the ids;
//  - Index (1..20), Name, Item[1..4] (clamped): a bandolieritem (ID, IconID, Name; its text the name). i: set * 4 +
//    item.
// MQ's Activate method is not here: /bandolier activate <name> (the game's own command) does it.
const game::BandolierSet* bandolier_set(const Context& c, std::int64_t index) {
    if (!c.conn || index < 0 || index >= game::kBandolierSets) return nullptr;
    const auto& sets = c.conn->bandolier();
    if (index >= static_cast<std::int64_t>(sets.size())) return nullptr;
    const game::BandolierSet& s = sets[static_cast<std::size_t>(index)];
    return s.name.empty() ? nullptr : &s;
}

// The AdvPath TLO (MQ2AdvPath's members, over the client's /afollow: NavStatus::afollow_*): its text is TRUE, as
// MQ2AdvPath's. Active: TRUE (the follow is there). State: 1 following, 0 not (MQ2AdvPath's 2 playing and 3 recording
// are paths, not here). Status: 0 off, 1 on, 2 paused. Following, Paused, WaitingWarp; Playing and Recording FALSE.
// Waypoints: the trail's points left; Length: its flat length with the gap to the spawn; Monitor: the spawn followed.
// MQ2AdvPath's path members (Path, CheckPoint, X/Y/Z, Flag1-9 ...) are not here.
void register_advpath() {
    auto status = [](const Context& c) { return c.nav ? std::optional(c.nav->status()) : std::nullopt; };
    auto on = [status](const Context& c) {
        auto s = status(c);
        return s && s->afollow_spawn != 0 ? s : std::nullopt;
    };
    add(g.advpath, "Active", [](const Context&, const Value&, std::string_view) { return V::boolean(true); });
    add(g.advpath, "State", [on](const Context& c, const Value&, std::string_view) {
        return V::integer(on(c) ? 1 : 0);
    });
    add(g.advpath, "Status", [on](const Context& c, const Value&, std::string_view) {
        auto s = on(c);
        return V::integer(!s ? 0 : s->paused ? 2 : 1);
    });
    add(g.advpath, "Following", [on](const Context& c, const Value&, std::string_view) {
        return V::boolean(on(c).has_value());
    });
    add(g.advpath, "Paused", [on](const Context& c, const Value&, std::string_view) {
        auto s = on(c);
        return V::boolean(s && s->paused);
    });
    add(g.advpath, "WaitingWarp", [on](const Context& c, const Value&, std::string_view) {
        auto s = on(c);
        return V::boolean(s && s->afollow_waiting_warp);
    });
    add(g.advpath, "Playing", [](const Context&, const Value&, std::string_view) { return V::boolean(false); });
    add(g.advpath, "Recording", [](const Context&, const Value&, std::string_view) { return V::boolean(false); });
    add(g.advpath, "Waypoints", [on](const Context& c, const Value&, std::string_view) {
        auto s = on(c);
        return V::integer(s ? static_cast<std::int64_t>(s->afollow_waypoints) : 0);
    });
    add(g.advpath, "Length", [on](const Context& c, const Value&, std::string_view) {
        auto s = on(c);
        return V::number(s ? s->afollow_length : 0.0);
    });
    add(g.advpath, "Monitor", [on](const Context& c, const Value&, std::string_view) -> V {
        auto s = on(c);
        return s ? spawn_value(spawn_of(c, s->afollow_spawn)) : V::nil();
    });
    Type& t = const_cast<Type&>(*g.advpath);
    t.to_value = [](const Context&, const Value&) { return V::string("TRUE"); };
}

void register_bandolier() {
    me_member("Bandolier", [](const Context&, const GameConnection& c, std::string_view index) -> V {
        std::int64_t n = 0;
        if (parse_int(index, n))
            return V::object(g.bandolier, std::clamp<std::int64_t>(n, 1, game::kBandolierSets) - 1);
        if (index.empty()) return V::nil();
        const int found = c.bandolier_find(std::string(index));
        return found >= 0 ? V::object(g.bandolier, found) : V::nil();
    });
    Type& t = const_cast<Type&>(*g.bandolier);
    t.to_value = [](const Context& c, const Value& v) -> V {
        const game::BandolierSet* s = bandolier_set(c, v.i);
        return s ? V::string(s->name) : V::nil();
    };
    add(g.bandolier, "Active", [](const Context& c, const Value& v, std::string_view) -> V {
        const game::BandolierSet* s = bandolier_set(c, v.i);
        if (!s) return V::nil();
        for (std::size_t k = 0; k < game::kBandolierSlots.size(); ++k) {
            const net::Item* worn = c.conn->inventory().at(net::ItemSlot::possessions(game::kBandolierSlots[k]));
            if (s->items[k].id != (worn ? worn->id : 0u)) return V::boolean(false);
        }
        return V::boolean(true);
    });
    add(g.bandolier, "Index", [](const Context& c, const Value& v, std::string_view) -> V {
        return bandolier_set(c, v.i) ? V::integer(v.i + 1) : V::nil();
    });
    add(g.bandolier, "Name", [](const Context& c, const Value& v, std::string_view) -> V {
        const game::BandolierSet* s = bandolier_set(c, v.i);
        return s ? V::string(s->name) : V::nil();
    });
    add(g.bandolier, "Item", [](const Context& c, const Value& v, std::string_view index) -> V {
        std::int64_t n = 0;
        if (!bandolier_set(c, v.i) || index.empty()) return V::nil();
        if (!parse_int(index, n)) n = 0; // MQ's GetIntFromString(Index, 0)
        n = std::clamp<std::int64_t>(n - 1, 0, static_cast<std::int64_t>(game::kBandolierSlots.size()) - 1);
        return V::object(g.bandolier_item, v.i * 4 + n);
    });
    auto item = [](const Context& c, const Value& v) -> const game::BeltItem* {
        const game::BandolierSet* s = bandolier_set(c, v.i / 4);
        return s ? &s->items[static_cast<std::size_t>(v.i % 4)] : nullptr;
    };
    Type& it = const_cast<Type&>(*g.bandolier_item);
    it.to_value = [item](const Context& c, const Value& v) -> V {
        const game::BeltItem* b = item(c, v);
        return b ? V::string(b->name) : V::nil();
    };
    add(g.bandolier_item, "ID", [item](const Context& c, const Value& v, std::string_view) -> V {
        const game::BeltItem* b = item(c, v);
        return b ? V::integer(b->id) : V::nil();
    });
    add(g.bandolier_item, "IconID", [item](const Context& c, const Value& v, std::string_view) -> V {
        const game::BeltItem* b = item(c, v);
        return b ? V::integer(b->icon) : V::nil();
    });
    add(g.bandolier_item, "Name", [item](const Context& c, const Value& v, std::string_view) -> V {
        const game::BeltItem* b = item(c, v);
        return b ? V::string(b->name) : V::nil();
    });
}

void register_moveto() {
    auto status = [](const Context& c) { return c.nav ? std::optional(c.nav->status()) : std::nullopt; };
    add(g.moveto, "Moving", [status](const Context& c, const Value&, std::string_view) {
        auto s = status(c);
        return V::boolean(s && s->moveto);
    });
    add(g.moveto, "Stopped", [status](const Context& c, const Value&, std::string_view) {
        auto s = status(c);
        return V::boolean(s && s->moveto_arrived);
    });
    add(g.moveto, "ArrivalDist", [](const Context& c, const Value&, std::string_view) -> V {
        return c.nav ? V::number(c.nav->moveto_distance()) : V::nil();
    });
    add(g.moveto, "Broken", [](const Context&, const Value&, std::string_view) { return V::boolean(false); });
    Type& t = const_cast<Type&>(*g.moveto);
    t.to_value = [status](const Context& c, const Value&) {
        auto s = status(c);
        return V::string(!s || !s->moveto ? "OFF" : s->paused ? "PAUSED" : "ON");
    };
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
    g.macroquest = &r.type("macroquest");
    g.hailstone = &r.type("hailstone");
    g.window = &r.type("window");
    g.plugin = &r.type("plugin");
    g.time = &r.type("time");
    g.gametime = &r.type("gametime");
    g.navigation = &r.type("navigation");
    g.stick = &r.type("stick");
    g.moveto = &r.type("moveto");
    g.bandolier = &r.type("bandolier");
    g.bandolier_item = &r.type("bandolieritem");
    g.advpath = &r.type("advpath");
    g.debuff = &r.type("debuff");
    g.target = &r.type("target");
    g.xtarget = &r.type("xtarget");
    g.altability = &r.type("altability");
    g.aug = &r.type("augtype");
    g.ticks = &r.type("ticks");
    g.body = &r.type("body");
    g.raid = &r.type("raid");
    g.raidmember = &r.type("raidmember");
    g.corpse = &r.type("corpse");
    g.pet = &r.type("pet");
    g.invslot = &r.type("invslot");
    g.merchant = &r.type("merchant");
    g.door = &r.type("switch");
    g.mercenary = &r.type("mercenary");
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
    register_moveto();
    register_bandolier();
    register_advpath();
    register_e3_members();
    register_e3_members2();
    register_e3_members3();
    register_e3_members4();

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
    r.add_tlo("MoveTo", [](const Context& c, std::string_view) -> V { return c.nav ? V::object(g.moveto, 0) : V::nil(); });
    r.add_tlo("AdvPath", [](const Context& c, std::string_view) -> V {
        return c.nav ? V::object(g.advpath, 0) : V::nil();
    });
    r.add_tlo("Me", [](const Context& c, std::string_view) -> V {
        return c.conn && c.conn->world().player() ? V::object(g.character, c.conn->world().player_id) : V::nil();
    });
    r.add_tlo("Target", [](const Context& c, std::string_view) -> V {
        const game::Spawn* s = c.conn ? spawn_of(c, c.conn->world().target_id) : nullptr;
        return s ? V::object(g.target, s->id) : V::nil();
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
    r.add_tlo("MacroQuest", [](const Context&, std::string_view) { return V::object(g.macroquest, 0); });
    r.add_tlo("Hailstone", [](const Context&, std::string_view) { return V::object(g.hailstone, 0); });
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
    // MQ's Raid and Corpse are always there (Raid's members say whether there is a raid, Corpse.Open whether a
    // corpse is open).
    // MQ's InvSlot[n|name]: a slot by MQ's number or name (an unknown name is slot 0, as in MQ).
    r.add_tlo("InvSlot", [](const Context& c, std::string_view index) -> V {
        if (!c.conn || index.empty()) return V::nil();
        return V::object(g.invslot, inv_slot_number(index).value_or(0));
    });
    // MQ's Merchant (always there: Open says whether one is), DoorTarget (/doortarget's door, NULL for none) and
    // Mercenary (always there: its state says whether there is one).
    r.add_tlo("Merchant", [](const Context& c, std::string_view) -> V { return c.conn ? V::object(g.merchant, 0) : V::nil(); });
    r.add_tlo("DoorTarget", [](const Context& c, std::string_view) -> V {
        if (!c.conn) return V::nil();
        auto id = c.conn->door_target();
        return id && c.conn->door(*id) ? V::object(g.door, *id) : V::nil();
    });
    r.add_tlo("Mercenary", [](const Context& c, std::string_view) -> V { return c.conn ? V::object(g.mercenary, 0) : V::nil(); });
    r.add_tlo("Raid", [](const Context& c, std::string_view) -> V { return c.conn ? V::object(g.raid, 0) : V::nil(); });
    r.add_tlo("Corpse", [](const Context& c, std::string_view) -> V { return c.conn ? V::object(g.corpse, 0) : V::nil(); });
    r.add_tlo("Pet", [](const Context& c, std::string_view) -> V { return pet_value(my_pet(c)); });
    // Debuff: the player's counters; Debuff[pet] the pet's, NULL without a pet (as the Pet TLO finds it). Other
    // indexes are NULL.
    r.add_tlo("Debuff", [](const Context& c, std::string_view index) -> V {
        if (!c.conn) return V::nil();
        if (index.empty()) return V::object(g.debuff, 0);
        if (!iequals(index, "pet")) return V::nil();
        const V pet = c.registry ? c.registry->tlo(c, "Pet") : V::nil();
        return pet.kind == Value::Kind::Object ? V::object(g.debuff, 1) : V::nil();
    });
}

} // namespace hailstone::mq
