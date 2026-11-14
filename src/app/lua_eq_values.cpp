#include "app/lua_eq_values.h"
#include "app/lua_eq_windows.h"

#include <algorithm>
#include <cctype>
#include <format>

#include "game/eq_names.h"
#include "game/item_moves.h"
#include "game/mechanics/character.h"
#include "game/mechanics/item_stats.h"
#include "game/mechanics/shown_stats.h"
#include "game/spells.h"
#include "game/world_state.h"
#include "net/game_connection.h"

// Each case names the original's switch case (the label function's byte-table index, or the gauge type) and what
// it reads (__GetLabelFromEQ, __GetGaugeValueFromEQ).

namespace hailstone::app {

namespace mechanics = game::mechanics;

namespace {

constexpr std::uint32_t kGrey = 0xffc0c0c0;  // -0x3f3f40
constexpr std::uint32_t kGreen = 0xff00ff00; // -0xff0100
constexpr std::uint32_t kOfflineGrey = 0xff808080;
constexpr std::uint32_t kMercOwner = 0xffe1b511; // -0x1e4aef

int vitality_fill(std::uint64_t value, std::uint64_t cap) {
    if (!cap) return 0;
    if (value >= cap) return 1000;
    // Find floor(value * 1000 / cap) without overflowing a 64-bit pool or
    // rounding UINT64_MAX - 1 to a full gauge on platforms where long double
    // is double. The threshold is ceil(cap * middle / 1000), also bounded.
    const auto whole = cap / 1000, remainder = cap % 1000;
    int low = 0, high = 1000;
    while (low + 1 < high) {
        const auto middle = static_cast<std::uint64_t>((low + high) / 2);
        const auto threshold = whole * middle + (remainder * middle + 999) / 1000;
        if (value >= threshold) low = static_cast<int>(middle);
        else high = static_cast<int>(middle);
    }
    return low;
}

// LabelFromEQ_FormatIntValue @0x761b50: "%d" of value (capped at max unless max is -9999); with compare, coloured
// against base.
LabelValue int_value(std::int64_t value, std::int64_t base = 0, bool compare = false, std::int64_t max = -9999) {
    if (max != -9999 && max < value) value = max;
    LabelValue v{std::to_string(value), std::nullopt};
    if (compare) v.argb = compare_color(value, base);
    return v;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const game::Spawn* spawn_by_name(const game::WorldState& world, const std::string& name) {
    if (name.empty()) return nullptr;
    std::string want = lower(name);
    for (const auto& [id, s] : world.spawns)
        if (lower(s.name) == want || lower(game::display_name(s.name)) == want) return &s;
    return nullptr;
}

const game::Spawn* spawn(const game::WorldState& world, std::uint32_t id) {
    if (id == 0) return nullptr;
    auto it = world.spawns.find(id);
    return it == world.spawns.end() ? nullptr : &it->second;
}

// EQ_Character::IsSpellcaster_3 @0x59fb90: classes 2..15 except Monk (7) and Rogue (9).
bool is_spellcaster(std::uint32_t class_id) { return class_id >= 2 && class_id <= 15 && class_id != 7 && class_id != 9; }

std::string my_name(const net::GameConnection& c) {
    if (const auto* me = c.world().player()) return me->name;
    return c.profile() ? c.profile()->name : std::string();
}

// CGroup_GetNthOtherMember @0x54d950: the n-th (0-based) filled slot that is not the player, in slot order.
const game::GroupMember* nth_other_member(const net::GameConnection& c, int n) {
    const auto& g = c.group();
    if (!g.active) return nullptr;
    std::string me = lower(my_name(c));
    int seen = 0;
    for (const auto& m : g.slots) {
        if (m.name.empty() || lower(m.name) == me) continue;
        if (seen++ == n) return &m;
    }
    return nullptr;
}

bool in_group(const net::GameConnection& c) { return c.group().active; }
bool am_leader(const net::GameConnection& c) { return in_group(c) && c.group().is_leader(my_name(c)); }

std::string spell_name(const net::GameConnection& c, std::uint32_t id) {
    if (id == game::kNoSpell || id == 0) return {};
    const auto* s = c.spell_table().get(id);
    return s ? s->name : std::string();
}

// A buff slot's spell (CharacterZoneClient_GetEffect(slot) +8).
std::uint32_t buff_spell(const net::GameConnection& c, int slot) {
    if (slot < 0 || slot >= game::kBuffSlots) return 0;
    const auto& b = c.spell_state().buffs[static_cast<std::size_t>(slot)];
    return b.empty() ? 0 : b.spell;
}

// The memorized spell of gem 0..11 (CharacterBase_GetMemorizedSpell).
std::uint32_t gem_spell(const net::GameConnection& c, int gem) {
    if (gem < 0 || gem >= game::kSpellGems) return game::kNoSpell;
    return c.spell_state().gems[static_cast<std::size_t>(gem)].spell;
}

// A spawn's HP percent as the original stores it for others (HPCurrent of 100), or the player's own points (Cur_HP /
// Max_HP, the items' HP included: net::GameConnection::own_hp).
int spawn_hp_percent(const EqHost&, const net::GameConnection& c, const game::Spawn& s) {
    if (s.id == c.world().player_id) {
        const auto hp = c.own_hp();
        return eq_percent(hp.cur, hp.max);
    }
    return eq_percent(s.hp_percent, 100);
}

std::int64_t max_mana(const EqHost&, const net::GameConnection& c) { return c.player_status().max_mana; }
std::int64_t max_endurance(const EqHost&, const net::GameConnection& c) { return c.player_status().max_endurance; }

// GetUserColor @0x761c00 for the user colours (index 0x100 + n): CDisplay's colour n, 0xf0f0f0 when it is 0.
std::uint32_t user_color(int n) {
    std::uint32_t rgb = user_color_rgb(n);
    return 0xff000000u | (rgb == 0 ? 0xf0f0f0u : rgb);
}

// AggroMeter_GetColorStringForPercent @0x410d30 / AggroMeterManagerClient::GetEntryColorString @0x411090: user
// colour 0x162 at 100 % and above, 0x161 above 75 %, else 0x160.
std::uint32_t aggro_color(int pct) { return user_color(pct > 99 ? 0x62 : pct > 75 ? 0x61 : 0x60); }

// An aggro meter percent label (cases 0x96..0x9b): "%d" capped at 999 in the percent's colour, empty below 1.
LabelValue aggro_pct(int pct) {
    if (pct < 1) return {};
    LabelValue v = int_value(pct, 0, false, 999);
    v.argb = aggro_color(pct);
    return v;
}

// 0x761560: the meter is locked on a spawn other than its target.
bool aggro_locked_elsewhere(const net::GameConnection::Aggro& a) { return a.lock_id != 0 && a.lock_id != a.target_id; }

} // namespace

Rgb argb_to_rgb(std::uint32_t argb) {
    return {static_cast<float>((argb >> 16) & 0xff) / 255.0f, static_cast<float>((argb >> 8) & 0xff) / 255.0f,
            static_cast<float>(argb & 0xff) / 255.0f};
}

int eq_percent(std::int64_t cur, std::int64_t max) {
    if (max == 0) return 0;
    // FILD cur, FILD max, FMUL dword 0.01f, FDIVP, __ftol2_sse (truncation); x87 extended precision.
    long double v = static_cast<long double>(cur) / (static_cast<long double>(max) * static_cast<long double>(0.01f));
    return static_cast<int>(v);
}

std::uint32_t compare_color(std::int64_t value, std::int64_t base) {
    if (value == base) return kGrey;
    return value > base ? kGreen : 0xffff0000;
}

std::string trim_name(const std::string& name) { return game::display_name(name); }

LabelValue stock_label(const EqHost& host, int type) {
    const net::GameConnection* cp = host.conn;
    if (!cp || !host.in_game) return {};
    const auto& c = *cp;
    const auto& ps = c.player_status();
    const auto& world = c.world();
    const game::mechanics::Character* ch = host.character;
    // Buff names: 45..59 and 500..541 the long buffs, 80..85 and 600..619 the songs; blocked buffs 550..579 and
    // 650..679 (PcBase_GetBlockedBuffSpellID: the player's and the pet's OP_BlockedBuffs lists).
    if ((type >= 45 && type <= 59) || (type >= 500 && type < 542)) {
        int slot = type >= 500 ? type - 500 : type - 45;
        return {spell_name(c, buff_spell(c, slot)), std::nullopt};
    }
    if ((type >= 80 && type <= 85) || (type >= 600 && type < 620)) {
        int slot = type >= 600 ? type - 600 : type - 80;
        return {spell_name(c, buff_spell(c, game::kLongBuffSlots + slot)), std::nullopt};
    }
    if ((type > 0x225 && type < 0x244) || (type > 0x289 && type < 0x2a8)) {
        bool pet = type > 0x289;
        const auto& list = c.blocked_buffs(pet);
        std::size_t i = static_cast<std::size_t>(type - (pet ? 0x28a : 0x226));
        std::int32_t id = i < list.size() ? list[i] : 0;
        return {id < 1 ? std::string() : spell_name(c, static_cast<std::uint32_t>(id)), std::nullopt};
    }
    if (type < 1 || type > 334) return {std::format("Unknown\tLabel{}", type), std::nullopt};
    switch (type) {
    case 1: { // case 0: the character's name, green for a group leader
        LabelValue v{my_name(c), std::nullopt};
        if (am_leader(c)) v.argb = kGreen;
        return v;
    }
    case 2: return int_value(ps.level);                       // case 1: GetLevel
    case 3: {                                                 // case 2: KunarkClass(-1, 0, 0, 0)
        const auto* me = world.player();
        return {game::class_name(me ? me->class_id : (c.profile() ? c.profile()->class_ : 0)), std::nullopt};
    }
    case 4: { // case 3: CEverQuest::GetDeityDesc
        std::uint32_t deity = c.profile() ? c.profile()->deity : 0;
        static constexpr std::pair<std::uint32_t, std::uint32_t> kDeity[] = {
            {0xc9, 0xcb3}, {0xca, 0xcbe}, {0xcb, 0xcb5}, {0xcc, 0xcb8}, {0xcd, 0xcb4}, {0xce, 0xcb6}, {0xcf, 0xcb7},
            {0xd0, 0xcb9}, {0xd1, 0xcbb}, {0xd2, 0xcbc}, {0xd3, 0xcc2}, {0xd4, 0xcba}, {0xd5, 0xcbd}, {0xd6, 0xcbf},
            {0xd7, 0xcc0}, {0xd8, 0xcc1}};
        std::uint32_t id = 0xcb2;
        for (auto [d, s2] : kDeity)
            if (d == deity) id = s2;
        return {c.strings().format(id, {}), std::nullopt};
    }
    case 24: { // case 0x17: carried weight, red over the limit (the strength)
        if (!ch) return int_value(0);
        LabelValue v = int_value(ch->stats().weight);
        v.argb = ch->stats().weight > ch->stats().stats.str ? 0xffff0000 : kGrey;
        return v;
    }
    case 25: return int_value(ch ? ch->stats().stats.str : 0); // case 0x18: 0x442bf0, the weight limit
    case 26: return int_value(static_cast<std::int64_t>(static_cast<long double>(ps.exp) * 0.303031), 0, false, 100); // case 0x19
    case 27: return int_value(static_cast<std::int64_t>(static_cast<long double>(ps.aa_exp) * 0.303031), 0, false, 100); // case 0x1a
    case 73: { // case 0x26: the last name (green for a group leader)
        LabelValue v{world.player() ? world.player()->last_name : std::string(), std::nullopt};
        if (am_leader(c)) v.argb = kGreen;
        return v;
    }
    case 74:                                                  // case 0x27: the title (PlayerBase +0x3dd)
        return {world.player() ? world.player()->title : std::string(), std::nullopt};
    case 22: { // case 0x15: the displayed AC (0x580330), green / red against it without AC buffs
        if (!ch) return int_value(0);
        std::int64_t q = mechanics::shown_ac(*ch);
        std::int64_t buffs = ch->total_effect(1, true, 0, false, true) + ch->total_effect(0x1a0, true, 0, false, true) +
                             ch->total_effect(0x37, true, 0, false, true);
        return int_value(q, q - buffs, true);
    }
    case 23: { // case 0x16: ATK, 0x584f60(1): (offense + attack) * 1000 / 744, against it without SPA 2
        if (!ch) return int_value(0);
        bool spell_2434 = std::ranges::any_of(c.spell_state().buffs,
                                              [](const auto& b) { return !b.empty() && b.spell == 2434; });
        std::int64_t atk = mechanics::shown_atk(*ch, spell_2434);
        return int_value(atk, atk - ch->total_effect(2, true, 0, false, true), true);
    }
    // Cases 0x4a..0x4c: the combat regeneration (GetHPRegen / GetManaRegen / GetEnduranceRegen without the resting
    // bonus) with items plus the buffs' cached SPA 0 (mana 15 and 417, endurance 189), against it without items.
    case 212: {
        if (!ch) return int_value(0);
        return int_value(static_cast<std::int64_t>(ch->hp_regen(true, nullptr, true)) + ch->cached_buff(0, 0),
                         ch->hp_regen(false, nullptr, true), true);
    }
    case 213: {
        if (!ch) return int_value(0);
        return int_value(static_cast<std::int64_t>(ch->mana_regen(true, true)) + ch->cached_buff(15, 0) + ch->cached_buff(0x1a1, 0),
                         ch->mana_regen(false, true), true);
    }
    case 214: {
        if (!ch) return int_value(0);
        return int_value(static_cast<std::int64_t>(ch->endurance_regen(true, true)) + ch->cached_buff(0xbd, 0),
                         ch->endurance_regen(false, true), true);
    }
    case 86: case 87: case 88: case 89: case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
    case 98: case 99: case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107: case 108:
    case 109: case 110: case 111: case 112: case 113: case 114: case 115: { // case 0x2e: the pet's buff slot n
        std::uint32_t slot = static_cast<std::uint32_t>(type - 86);
        for (const auto& [s, b] : c.pet_buffs())
            if (s == slot && !b.empty() && b.spell >= 1 && b.spell <= 45000) return {spell_name(c, b.spell), std::nullopt};
        return {};
    }
    case 135: { // case 0x41: the target of target's name (nothing for a corpse)
        const auto* t = spawn(world, c.target_of_target());
        if (!t || t->is_corpse) return {};
        return {trim_name(t->name), std::nullopt};
    }
    case 139: case 140: case 141: case 142: case 143:   // case 0x44: group members' mana percent
    case 144: case 145: case 146: case 147: case 148: { // case 0x45: and endurance (0 for a corpse)
        bool mana = type <= 143;
        const auto* m = nth_other_member(c, type - (mana ? 139 : 144));
        const auto* s = m ? spawn_by_name(world, m->name) : nullptr;
        if (!s) return int_value(0);
        if (s->is_corpse) return int_value(0, 0, false, 100);
        const auto& table = mana ? c.group().mana_percent : c.group().endurance_percent;
        auto it = table.find(s->id);
        return int_value(it == table.end() ? 0 : it->second, 0, false, 100);
    }
    case 171: case 172: case 173: case 174: case 175: case 176: case 177: case 178: case 179: case 180:
    case 181: case 182: case 183: case 184: case 185: case 186: case 187: case 188: case 189: case 190:
    case 191: case 192: case 193: case 194: case 195: case 196: case 197: case 198: case 199: case 200:
    case 201: case 202: case 203: case 204: case 205: case 206: case 207: case 208: case 209: case 210: {
        // Cases 0x47 / 0x48: extended target slot n's mana (171+n) or endurance (191+n) percent for a player or a
        // mercenary (expansion flag 0x8000), 0 otherwise or for a corpse.
        if (host.character && !(host.character->sheet().expansions & 0x8000)) return {};
        bool mana = type <= 190;
        std::size_t n = static_cast<std::size_t>(type - (mana ? 171 : 191));
        const auto& xs = c.xtargets().slots;
        const auto* t = n < xs.size() ? spawn(world, xs[n].spawn_id) : nullptr;
        int pct = 0;
        if (t && !t->is_corpse && (!t->is_npc || t->mercenary)) {
            if (t->id == world.player_id) {
                pct = eq_percent(mana ? ps.mana : ps.endurance, mana ? max_mana(host, c) : max_endurance(host, c));
            } else {
                const auto& table = mana ? c.group().mana_percent : c.group().endurance_percent;
                auto it = table.find(t->id);
                pct = it == table.end() ? 0 : it->second;
            }
        }
        return int_value(std::max(0, pct), 0, false, 100);
    }
    case 17: return int_value(c.own_hp().cur);                // case 0x10: Cur_HP
    case 18: {                                                // case 0x11: Max_HP vs Max_HP minus HP buffs
        const std::int64_t max = c.own_hp().max;
        std::int64_t buffs = host.character ? host.character->total_effect(0) + host.character->total_effect(0x45) : 0;
        return int_value(max, max - buffs, host.character != nullptr);
    }
    case 19: {                                                // case 0x12
        const auto hp = c.own_hp();
        return int_value(std::max(0, eq_percent(hp.cur, hp.max)), 0, false, 100);
    }
    case 20: return int_value(eq_percent(ps.mana, max_mana(host, c)), 0, false, 100);       // case 0x13
    case 21: return int_value(eq_percent(ps.endurance, max_endurance(host, c)), 0, false, 100); // case 0x14
    case 28: {                                                // case 0x1b: the target's name
        if (const auto* t = spawn(world, world.target_id)) return {trim_name(t->name), std::nullopt};
        return {};
    }
    case 29: {                                                // case 0x1c: the target's HP percent ("0" for a corpse)
        const auto* t = spawn(world, world.target_id);
        if (!t || t->is_corpse) return int_value(0);
        return int_value(spawn_hp_percent(host, c, *t), 0, false, 100);
    }
    case 30: case 31: case 32: case 33: case 34: {           // case 0x1d: group member names
        const auto* m = nth_other_member(c, type - 30);
        if (!m) return {};
        LabelValue v{trim_name(m->name), std::nullopt};
        if (c.group().is_leader(m->name)) v.argb = kGreen;
        else if (m->offline) v.argb = kOfflineGrey;
        else if (m->merc && c.group().has_member(m->owner_name)) v.argb = kMercOwner;
        return v;
    }
    case 35: case 36: case 37: case 38: case 39: {           // case 0x1e: group member HP percent
        const auto* m = nth_other_member(c, type - 35);
        const auto* s = m ? spawn_by_name(world, m->name) : nullptr;
        if (!s) return int_value(0);
        int hp = s->is_corpse ? 0 : spawn_hp_percent(host, c, *s);
        return int_value(std::max(0, hp), 0, false, 100);
    }
    case 40: case 41: case 42: case 43: case 44: {           // case 0x1f: group members' pets' HP percent
        const auto* m = nth_other_member(c, type - 40);
        const auto* s = m ? spawn_by_name(world, m->name) : nullptr;
        if (s) {
            for (const auto& [id, p] : world.spawns)
                if (p.pet_owner_id == s->id && p.is_npc) return int_value(std::max(0, spawn_hp_percent(host, c, p)), 0, false, 100);
        }
        return int_value(0);
    }
    case 60: case 61: case 62: case 63: case 64: case 65: case 66: case 67: case 133: case 138: case 149: case 150: {
        // case 0x20: memorized spell names, gems 1..8 then 9 (133), 10 (138), 11 (149), 12 (150)
        int gem = type == 133 ? 8 : type == 138 ? 9 : type == 149 ? 10 : type == 150 ? 11 : type - 60;
        return {spell_name(c, gem_spell(c, gem)), std::nullopt};
    }
    case 68: {                                                // case 0x21: the pet's name
        const auto* p = spawn(world, c.pets().focused);
        return {p ? trim_name(p->name) : std::string(), std::nullopt};
    }
    case 69: {                                                // case 0x22: the pet's HP percent
        const auto* p = spawn(world, c.pets().focused);
        if (!p) return int_value(0);
        return int_value(std::max(0, spawn_hp_percent(host, c, *p)), 0, false, 100);
    }
    case 70: {                                                // case 0x23: "cur/max" HP, green when buffed
        const auto hp = c.own_hp();
        LabelValue v{std::format("{}/{}", hp.cur, hp.max), std::nullopt};
        std::int64_t buffs = host.character ? host.character->total_effect(0) + host.character->total_effect(0x45) : 0;
        v.argb = compare_color(buffs, 0);
        return v;
    }
    case 71: return {std::to_string(c.profile() ? c.profile()->aa_points : 0), std::nullopt}; // case 0x24
    // The tribute labels (TributeBenefitWnd / TributeMasterWnd, by their XML names; the original's cases are not
    // decoded): 116 the time left, 117 favour, 118 career favour, 119 the active cost.
    case 116: {
        std::int64_t left = c.tribute().time_left(net::GameConnection::now_ms());
        return {std::format("{}:{:02}", left / 60000, left / 1000 % 60), std::nullopt};
    }
    case 117: return {std::to_string(c.tribute().points), std::nullopt};
    case 118: return {std::to_string(c.tribute().career), std::nullopt};
    case 119: return {std::to_string(c.tribute().active_cost()), std::nullopt};
    // The guild tribute labels (TributeBenefitWnd's Guild page, by their XML names): 121 the time left, 122 the
    // guild's favour, 123 the active cost.
    case 121: {
        std::int64_t left = c.guild_tribute().time_left(net::GameConnection::now_ms());
        return {std::format("{}:{:02}", left / 60000, left / 1000 % 60), std::nullopt};
    }
    case 122: return {std::to_string(c.guild_tribute().active.favor), std::nullopt};
    case 123: return {std::to_string(c.guild_tribute().active_cost()), std::nullopt};
    // The aggro meter's labels (cases 0x93..0x9b, Rain of Fear: expansion flag 0x40000). 302 the meter's spawn (the
    // lock, else the target), 303 the secondary (most hated besides us, or the tank) in its entry's colour, 304 the
    // same but empty while the meter is locked on another spawn (the player window's copy); 305 / 307 our and the
    // secondary's percent, 306 / 308 the same, 0 while locked elsewhere; 309..313 the group members' (entries 2..6
    // by their group slot), 314..333 the extended target slots' (entries 7..26). The percents are capped at 999,
    // empty below 1 and coloured by AggroMeter_GetColorStringForPercent.
    case 302: case 303: case 304: case 305: case 306: case 307: case 308:
    case 309: case 310: case 311: case 312: case 313:
    case 314: case 315: case 316: case 317: case 318: case 319: case 320: case 321: case 322: case 323:
    case 324: case 325: case 326: case 327: case 328: case 329: case 330: case 331: case 332: case 333: {
        if (host.character && !(host.character->sheet().expansions & 0x40000)) return {};
        const auto& a = c.aggro();
        bool elsewhere = aggro_locked_elsewhere(a);
        switch (type) {
        case 302: {
            const auto* t = spawn(world, a.lock_id ? a.lock_id : a.target_id);
            return {t ? trim_name(t->name) : std::string(), std::nullopt};
        }
        case 303: case 304: {
            const auto* t = type == 304 && elsewhere ? nullptr : spawn(world, a.secondary_id);
            if (!t) return {};
            return {trim_name(t->name), aggro_color(a.pct[net::kAggroSecondary])};
        }
        case 305: return aggro_pct(a.pct[net::kAggroPlayer]);
        case 306: return aggro_pct(elsewhere ? 0 : a.pct[net::kAggroPlayer]);
        case 307: return aggro_pct(a.pct[net::kAggroSecondary]);
        case 308: return aggro_pct(elsewhere ? 0 : a.pct[net::kAggroSecondary]);
        default: break;
        }
        if (type >= 314) return aggro_pct(a.pct[static_cast<std::size_t>(type - 307)]);
        // Group member n (0-based among the others): its group slot, less one when the player's slot comes before it.
        const auto* m = nth_other_member(c, type - 309);
        if (!m || !c.group().active) return {};
        int slot = -1, mine = -1;
        std::string me = lower(my_name(c));
        for (std::size_t i = 0; i < c.group().slots.size(); ++i) {
            if (&c.group().slots[i] == m) slot = static_cast<int>(i);
            if (!c.group().slots[i].name.empty() && lower(c.group().slots[i].name) == me) mine = static_cast<int>(i);
        }
        if (slot < 0 || slot >= 6 || mine < 0 || mine >= 6) return {};
        int entry = slot + 2 - (mine < slot ? 1 : 0);
        return aggro_pct(a.pct[static_cast<std::size_t>(entry)]);
    }
    case 124: return int_value(ps.mana);                      // case 0x37: Cur_Mana
    case 125: return int_value(max_mana(host, c));            // case 0x38 (no buff comparison without the sheet)
    case 126: return int_value(ps.endurance);                 // case 0x39
    case 127: return int_value(max_endurance(host, c));       // case 0x3a
    case 128: {                                               // case 0x3b: "cur/max" mana
        return {std::format("{}/{}", ps.mana, max_mana(host, c)), kGrey};
    }
    case 129: return {std::format("{}/{}", ps.endurance, max_endurance(host, c)), kGrey}; // case 0x3c
    case 134: {                                               // case 0x40: the spell being cast
        return {spell_name(c, c.spell_state().casting.spell), std::nullopt};
    }
    case 5: case 6: case 7: case 8: case 9: case 10: case 11: // cases 4..10: the stats, against base + AA
    case 238: case 239: case 240: case 241: case 242: case 243: case 244: // cases 100..106: without heroic bonuses
    case 251: case 252: case 253: case 254: case 255: case 256: case 257: // cases 0x71..0x77: heroic bonuses
    case 264: case 265: case 266: case 267: case 268: case 269: case 270: { // cases 0x7e..0x84: caps less heroic
        if (!ch) return int_value(0);
        const auto& d = ch->stats();
        const auto& sh = ch->sheet();
        // STR STA DEX AGI WIS INT CHA (the label order), their SPA for the AA part and the stat cap index
        struct S { std::int32_t mechanics::StatBlock::*v; std::int32_t mechanics::DerivedStats::*heroic; std::int32_t base; int spa, cap; };
        const S stats[] = {{&mechanics::StatBlock::str, &mechanics::DerivedStats::heroic_str, sh.base_str, 4, 0},
                           {&mechanics::StatBlock::sta, &mechanics::DerivedStats::heroic_sta, sh.base_sta, 7, 1},
                           {&mechanics::StatBlock::dex, &mechanics::DerivedStats::heroic_dex, sh.base_dex, 5, 3},
                           {&mechanics::StatBlock::agi, &mechanics::DerivedStats::heroic_agi, sh.base_agi, 6, 2},
                           {&mechanics::StatBlock::wis, &mechanics::DerivedStats::heroic_wis, sh.base_wis, 9, 4},
                           {&mechanics::StatBlock::int_, &mechanics::DerivedStats::heroic_int, sh.base_int, 8, 5},
                           {&mechanics::StatBlock::cha, &mechanics::DerivedStats::heroic_cha, sh.base_cha, 10, 6}};
        int group = type <= 11 ? 0 : type <= 244 ? 1 : type <= 257 ? 2 : 3;
        // the window's order for 5..11: STR STA DEX AGI WIS INT CHA (case 4 STR .. case 10 CHA)
        int i = group == 0 ? type - 5 : group == 1 ? type - 238 : group == 2 ? type - 251 : type - 264;
        const S& st = stats[i];
        std::int64_t base = st.base + ch->cached_aa(st.spa, 0);
        std::int32_t heroic = ch->heroic(st.heroic);
        switch (group) {
        case 0: return int_value(d.stats.*st.v, base, true);
        case 1: return int_value(d.uncapped.*st.v - heroic, base, true);
        case 2: return int_value(heroic);
        default: return int_value(ch->stat_cap(st.cap) - heroic);
        }
    }
    case 12: case 13: case 14: case 15: case 16: case 136:  // cases 0xb..0xf, 0x42: the saves, against base + AA
    case 245: case 246: case 247: case 248: case 249: case 250: // cases 0x6b..0x70: without heroic bonuses
    case 258: case 259: case 260: case 261: case 262: case 263: // cases 0x78..0x7d: heroic save bonuses
    case 271: case 272: case 273: case 274: case 275: case 276: { // cases 0x85..0x8a: caps less heroic
        if (!ch) return int_value(0);
        const auto& d = ch->stats();
        // poison disease fire cold magic corruption
        struct S { std::int32_t mechanics::StatBlock::*v; std::int32_t mechanics::DerivedStats::*heroic; int aa, cap; };
        const S saves[] = {{&mechanics::StatBlock::sv_poison, &mechanics::DerivedStats::heroic_sv_poison, -1, 10},
                           {&mechanics::StatBlock::sv_disease, &mechanics::DerivedStats::heroic_sv_disease, 0x31, 11},
                           {&mechanics::StatBlock::sv_fire, &mechanics::DerivedStats::heroic_sv_fire, 0x2e, 9},
                           {&mechanics::StatBlock::sv_cold, &mechanics::DerivedStats::heroic_sv_cold, 0x2f, 8},
                           {&mechanics::StatBlock::sv_magic, &mechanics::DerivedStats::heroic_sv_magic, 0x32, 7},
                           {&mechanics::StatBlock::sv_corruption, &mechanics::DerivedStats::heroic_sv_corruption, 0x172, 13}};
        int group, i;
        if (type == 136) group = 0, i = 5;
        else if (type <= 16) group = 0, i = type - 12;
        else if (type <= 250) group = 1, i = type - 245;
        else if (type <= 263) group = 2, i = type - 258;
        else group = 3, i = type - 271;
        const S& sv = saves[i];
        std::int64_t basesave = i == 0 ? ch->basesave_poison() : i == 1 ? ch->basesave_disease() : i == 2 ? ch->basesave_fire()
                              : i == 3 ? ch->basesave_cold() : i == 4 ? ch->basesave_magic() : ch->sheet().base_save_corruption;
        std::int64_t base = basesave + (sv.aa >= 0 ? ch->cached_aa(sv.aa, 0) : 0);
        std::int32_t heroic = ch->heroic(sv.heroic);
        switch (group) {
        case 0: return int_value(d.stats.*sv.v, base, true);
        case 1: return int_value(d.uncapped.*sv.v - heroic, base, true);
        case 2: return int_value(heroic);
        default: return int_value(ch->stat_cap(sv.cap) - heroic);
        }
    }
    case 211: { // case 0x49: haste (100 + %; 100 without)
        std::int64_t h = ch ? ch->total_effect(0xb, true, 0, true, true) : 0;
        return int_value(h == 0 ? 100 : h, 100, true);
    }
    case 215: case 216: case 217: case 218: case 219: case 220: case 221: case 222: case 223: case 224:
    case 225: case 226: case 227: { // cases 0x4d..0x59: the item bonuses (CharacterZoneClient *Bonus2 fields)
        if (!ch) return int_value(0);
        const auto& d = ch->stats();
        const std::int32_t v[] = {d.spell_shield, d.combat_effects, d.shielding, d.damage_shield, d.dot_shielding,
                                  d.ds_mitigation, d.avoidance, d.accuracy, d.stun_resist, d.strike_through,
                                  d.heal_amount, d.spell_damage, d.clairvoyance};
        return int_value(v[type - 215], 0, true);
    }
    case 237: { // case 99: "weight/limit"
        if (!ch) return {"0/0", kGrey};
        LabelValue v{std::format("{}/{}", ch->stats().weight, ch->stats().stats.str), std::nullopt};
        v.argb = compare_color(ch->stats().stats.str, ch->stats().weight);
        return v;
    }
    case 120: { // case 0x33: the target of target's HP percent; empty for a corpse (spawn type +0x125 above 1)
        const auto* t = spawn(world, c.target_of_target());
        if (!t || t->is_corpse) return {};
        return int_value(spawn_hp_percent(host, c, *t), 0, false, 100);
    }
    case 151: case 152: case 153: case 154: case 155: case 156: case 157: case 158: case 159: case 160:
    case 161: case 162: case 163: case 164: case 165: case 166: case 167: case 168: case 169: case 170: {
        // case 0x46: extended target slot n's HP percent (expansion flag 0x8000), 0 without a spawn or for a corpse.
        if (host.character && !(host.character->sheet().expansions & 0x8000)) return {};
        const auto& xs = c.xtargets().slots;
        std::size_t n = static_cast<std::size_t>(type - 151);
        const auto* t = n < xs.size() ? spawn(world, xs[n].spawn_id) : nullptr;
        int pct = t && !t->is_corpse ? spawn_hp_percent(host, c, *t) : 0;
        return int_value(std::max(0, pct), 0, false, 100);
    }
    case 137: { // case 0x43: the combat timer (PcClient vtable +0x84 @0x577dd0: OP_RestState's downtime left, seconds)
                // as "%02d:%02d", empty when none is left
        std::uint32_t left = c.rest_downtime_left();
        if (left == 0) return {};
        return {std::format("{:02}:{:02}", left / 60, left % 60), std::nullopt};
    }
    case 132: { // case 0x3f: the task request timer (PcBase::TaskRequestTimer, OP_TaskRequestTimer) as "%02d:%02d",
                // the seconds rounded up; the list's colour
        std::int64_t left = c.tasks().request_timer_until_ms - net::GameConnection::now_ms();
        int s = left > 0 ? static_cast<int>((left + 999) / 1000) : 0;
        return {std::format("{:02}:{:02}", s / 60, s % 60), std::nullopt};
    }
    default:
        // The rest (docs/unknowns.md "UI"): not set, the label stays empty.
        return {};
    }
}

LabelValue eq_label(const EqHost& host, int type) {
    LabelValue v = stock_label(host, type);
    const net::GameConnection* c = host.conn;
    if (!c || !host.in_game) return v;
    // A server's client may replace the text (the NMS patch's __GetLabelFromEQ hook: the NMS plugin's labels).
    c->server_ext().label_text(*c, type, v.text);
    return v;
}

namespace {

GaugeValue stock_gauge(const EqHost& host, int type) {
    const net::GameConnection* cp = host.conn;
    if (!cp || !host.in_game) return {-1, {}};
    const auto& c = *cp;
    const auto& ps = c.player_status();
    const auto& world = c.world();
    switch (type) {
    case 1: { // the player's HP; the name ("name\nlast name") over it
        const auto hp = c.own_hp();
        GaugeValue g{eq_percent(hp.cur, hp.max) * 10, {}};
        const auto* me = world.player();
        std::string name = me ? me->name : my_name(c);
        std::string last = me ? me->last_name : std::string();
        g.text = last.empty() ? name : name + "\n" + last;
        return g;
    }
    case 8: { // the air supply (BreathWindow): PcClient's air over MaxAirSupply (not decoded: the gauge's case)
        if (host.air < 0 || host.max_air <= 0) return {-1, {}};
        return {std::clamp(host.air * 1000 / host.max_air, 0, 1000), {}};
    }
    case 149: { // the aggro meter: our percent of the target's top hate (not decoded)
        if (!c.aggro().target_id && !c.aggro().lock_id) return {-1, {}};
        return {std::min(1000, c.aggro().pct[net::kAggroPlayer] * 10), {}};
    }
    case 147: case 148: {
        // __GetGaugeValueFromEQ @0x762410, cases 147/148: min(pool, cap) *
        // 1000 / cap, or zero for a zero cap. Deliberate safety: keep extreme
        // unsigned wire values and the product from wrapping. Exact amounts
        // stay strings for Lua's numeric precision.
        const auto& vitality = c.fellowship().vitality;
        const std::uint64_t value = type == 147 ? vitality.normal : vitality.aa;
        const std::uint64_t cap = type == 147 ? vitality.normal_cap : vitality.aa_cap;
        return {vitality_fill(value, cap), value || cap ? std::format("{} / {}", value, cap) : std::string()};
    }
    case 2: { // mana: -1 for non-casters without mana, 0 for casters
        std::int64_t mx = max_mana(host, c);
        if (mx < 1) {
            const auto* me = world.player();
            return {me && c.is_spellcaster() ? 0 : -1, {}};
        }
        return {eq_percent(ps.mana, mx) * 10, {}};
    }
    case 3: { // endurance
        std::int64_t mx = max_endurance(host, c);
        if (mx < 1) return {world.player() ? 0 : -1, {}};
        return {eq_percent(ps.endurance, mx) * 10, {}};
    }
    case 62: case 63: case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71:
    case 72: case 73: case 74: case 75: case 76: case 77: case 78: case 79: case 80: case 81:
    case 82: case 83: case 84: case 85: case 86: case 87: case 88: case 89: case 90: case 91:
    case 92: case 93: case 94: case 95: case 96: case 97: case 98: case 99: case 100: case 101: {
        // Cases 0x3e..0x65: extended target slot n's mana (62+n) or endurance (82+n), expansion flag 0x8000 (else
        // -2): -1 without a spawn or for an NPC that is not a mercenary, -3 a corpse, else the spawn's current over
        // its max. Other spawns' come from OP_MobManaUpdate / OP_MobEnduranceUpdate (0 before one), the player's
        // own from its profile. (Its GetThisField_0x194 test, -1 when set, is not kept: docs/unknowns.md.)
        if (host.character && !(host.character->sheet().expansions & 0x8000)) return {-2, {}};
        bool mana = type <= 81;
        std::size_t n = static_cast<std::size_t>(type - (mana ? 62 : 82));
        const auto& xs = c.xtargets().slots;
        const auto* t = n < xs.size() ? spawn(world, xs[n].spawn_id) : nullptr;
        if (!t || (t->is_npc && !t->mercenary)) return {-1, {}};
        if (t->is_corpse) return {-3, {}};
        if (t->id == world.player_id) {
            std::int64_t mx = mana ? max_mana(host, c) : max_endurance(host, c);
            return {mx < 1 ? 0 : eq_percent(mana ? ps.mana : ps.endurance, mx) * 10, {}};
        }
        const auto& table = mana ? c.group().mana_percent : c.group().endurance_percent;
        auto it = table.find(t->id);
        return {it == table.end() ? 0 : it->second * 10, {}};
    }
    case 4: // experience: FILD qword Exp, FMUL 3.03031 (0..330 scale)
        return {static_cast<int>(static_cast<long double>(ps.exp) * 3.03031), {}};
    case 5: // AA experience
        return {static_cast<int>(static_cast<long double>(ps.aa_exp) * 3.03031), {}};
    case 6: { // the target's HP; its name
        const auto* t = spawn(world, world.target_id);
        if (!t) return {-1, {}};
        GaugeValue g{t->is_corpse ? -3 : spawn_hp_percent(host, c, *t) * 10, trim_name(t->name)};
        return g;
    }
    case 7: { // GetCastingGaugeValue @0x761fb0: (ETA - now) * 1000 / the spell's cast time
        const auto& cast = c.spell_state().casting;
        if (!cast.casting() || cast.eta_ms == 0) return {-1, {}};
        const auto* s = c.spell_table().get(cast.spell);
        std::int64_t now = net::GameConnection::now_ms();
        if (!s || cast.eta_ms < now) return {-1, {}};
        std::int64_t len = s->cast_time > 0 ? s->cast_time : cast.cast_time;
        if (len < 1) return {-1, {}};
        std::int64_t v = (cast.eta_ms - now) * 1000 / len;
        return {static_cast<int>(std::clamp<std::int64_t>(v, 0, 1000)), {}};
    }
    case 9: { // CSpellBookWnd::GetSpellMemTicksLeft
        // Its value: ticks_left * 1000 / 92 (0 when no memorise runs); the tick itself is
        // GameConnection::memorize_tick, once a frame while the book is shown.
        const auto& m = c.spell_state().memorizing;
        if (!m) return {0, {}};
        return {static_cast<int>(static_cast<std::uint32_t>(m->ticks_left) * 1000u) / game::kMemorizeTicks, {}};
    }
    case 10: { // GetSpellScribeTicksLeft
        const auto& s = c.spell_state().scribing;
        if (!s) return {-1, {}};
        return {(game::kMemorizeTicks - s->ticks_left) * 1000 / game::kMemorizeTicks, {}};
    }
    case 11: case 12: case 13: case 14: case 15: { // group members' HP; their names
        const auto* m = nth_other_member(c, type - 11);
        if (!m) return {-1, {}};
        const auto* s = spawn_by_name(world, m->name);
        GaugeValue g{-2, trim_name(m->name)};
        if (s) g.value = s->is_corpse ? -3 : spawn_hp_percent(host, c, *s) * 10;
        return g;
    }
    case 16: { // the pet's HP; its name
        const auto* p = spawn(world, c.pets().focused);
        if (!p) return {-1, {}};
        return {spawn_hp_percent(host, c, *p) * 10, trim_name(p->name)};
    }
    case 17: case 18: case 19: case 20: case 21: { // group members' pets
        const auto* m = nth_other_member(c, type - 17);
        const auto* s = m ? spawn_by_name(world, m->name) : nullptr;
        if (!s) return {-1, {}};
        for (const auto& [id, p] : world.spawns)
            if (p.pet_owner_id == s->id && p.is_npc) return {spawn_hp_percent(host, c, p) * 10, trim_name(p.name)};
        return {-1, {}};
    }
    case 31: case 32: case 33: case 34: case 35:   // group members' mana
    case 36: case 37: case 38: case 39: case 40: { // and endurance (OP_MobManaUpdate / OP_MobEnduranceUpdate)
        bool mana = type <= 35;
        const auto* m = nth_other_member(c, type - (mana ? 31 : 36));
        if (!m) return {-1, {}};
        GaugeValue g{-1, trim_name(m->name)};
        const auto* s = spawn_by_name(world, m->name);
        if (!s) {
            g.value = -2;
            return g;
        }
        if (s->is_corpse) {
            g.value = -3;
            return g;
        }
        if (mana && !(s->id == c.world().player_id ? c.is_spellcaster() : is_spellcaster(s->class_id))) return g;
        const auto& table = mana ? c.group().mana_percent : c.group().endurance_percent;
        auto it = table.find(s->id);
        g.value = it == table.end() ? 0 : it->second * 10;
        return g;
    }
    case 41: { // the pet's target's HP; its name (case 0x29: no pet -2, a pet with no target -1, a corpse nothing
               // drawn with its name)
        if (!c.pets().focused) return {-2, {}};
        if (!spawn(world, c.pets().focused)) return {-1, {}};
        const auto* t = spawn(world, c.pets().pet_target);
        if (!t) return {-1, {}};
        return {t->is_corpse ? -1 : spawn_hp_percent(host, c, *t) * 10, trim_name(t->name)};
    }
    case 25: return {100, {}};
    case 26: return eq_discipline_gauge(host, net::GameConnection::now_ms());
    case 42: case 43: case 44: case 45: case 46: case 47: case 48: case 49: case 50: case 51:
    case 52: case 53: case 54: case 55: case 56: case 57: case 58: case 59: case 60: case 61: {
        // Extended target slot n (expansion flag 0x8000, else -2): its spawn's HP and name (-3 a corpse); without a
        // spawn, nothing for an auto hater or an empty slot, else the slot's name drawn as -9 (status not 2) or -2.
        if (host.character && !(host.character->sheet().expansions & 0x8000)) return {-2, {}};
        const auto& xs = c.xtargets().slots;
        std::size_t n = static_cast<std::size_t>(type - 42);
        if (n >= xs.size()) return {-1, {}};
        if (const auto* t = spawn(world, xs[n].spawn_id))
            return {t->is_corpse ? -3 : spawn_hp_percent(host, c, *t) * 10, trim_name(t->name)};
        if (xs[n].type < 2 || xs[n].status == 0) return {-1, {}};
        return {xs[n].status != 2 ? -9 : -2, xs[n].name};
    }
    case 27: { // the target of target's HP (OP_TargetHoTT); its name
        const auto* t = spawn(world, c.target_of_target());
        if (!t) return {-1, {}};
        return {t->is_corpse ? -3 : spawn_hp_percent(host, c, *t) * 10, trim_name(t->name)};
    }
    case 28: { // the respawn timer (case 0x1c): with the respawn window up and the corpse's time set,
        // trunc((1 - min(now - corpse time + 500, total) / total) * 1000), total = CRespawnWnd +0x230 (the packet's
        // time remaining, 0x74e9a0's second argument). A total of 0 divides by zero in the original (x87: NaN or
        // inf, __ftol2_sse gives INT_MIN): drawn empty here.
        const auto& rw = c.respawn_window();
        if (!rw) return {-1, {}};
        if (rw->time_remaining_ms == 0) return {0, {}};
        std::int64_t total = static_cast<std::int32_t>(rw->time_remaining_ms);
        std::int64_t e = std::min<std::int64_t>(c.respawn_age_ms() + 500, total);
        double f = (1.0 - static_cast<double>(e) / static_cast<double>(total)) * 1000.0;
        return {static_cast<int>(f), {}};
    }
    case 29: { // case 0x1d: the combat timer, min(seconds left, 30) * 1000 / 30; -1 (not drawn) once none is left
        std::uint32_t left = c.rest_downtime_left();
        if (left == 0) return {-1, {}};
        return {static_cast<int>(std::min<std::uint32_t>(left, 30) * 1000 / 30), {}};
    }
    default:
        // The rest (leadership experience, achievements): not tracked here.
        return {-1, {}};
    }
}

} // namespace

GaugeValue eq_discipline_gauge(const EqHost& host, std::int64_t now_ms) {
    if (!host.conn || !host.in_game || !host.conn->discipline_active()) return {0, {}};
    const auto& c = *host.conn;
    const auto& buff = c.spell_state().buffs[game::kBuffSlots - 1];
    const auto* spell = c.spell_table().get(buff.spell);
    if (!spell) return {0, {}};
    // Gauge 26's helper @0x761600: remaining ticks * 1000 / SpellDuration @0x434420 (spell, the local player's
    // level, false), 0 when that is not positive; the gauge function caps it at 1000. RoF2 gives the gauge no text.
    // Hailstone additions: interpolation between buff ticks, the seconds text, and permanent effects drawn full.
    if (game::buff_is_permanent(buff.tics, spell)) return {1000, "Permanent"};
    const auto left = game::buff_countdown_held(spell, c.spell_state().buff_timers_on_hold)
                          ? static_cast<std::uint64_t>(buff.tics) * game::kBuffTickMs
                          : game::buff_ms_left(buff.tics, buff.received_ms, now_ms);
    const auto ticks = host.character ? host.character->spell_duration(*spell, c.player_status().level) : 0;
    const auto total = static_cast<std::int64_t>(ticks) * game::kBuffTickMs;
    const int value = total > 0
                          ? static_cast<int>(std::min<std::uint64_t>(left * 1000 / static_cast<std::uint64_t>(total), 1000))
                          : 0;
    return {value, game::format_duration(left)};
}

GaugeFill gauge_fill(int value) {
    if (value == -2) return {1000, 0xff800000u};
    if (value >= -110 && value <= -10) return {(value + 110) * 10, 0xff800080u};
    if (value >= -2000 && value <= -1000) return {value + 2000, 0xffc08080u};
    if (value < 0) return {};
    return {std::min(value, 1000), std::nullopt};
}

GaugeValue eq_gauge(const EqHost& host, int type) {
    GaugeValue g = stock_gauge(host, type);
    // The switch's common tail: above 1000 is 1000, below -2000 is -2000, and -999..-111 is -110 (the purple band's
    // empty end). The early returns (-1, -2, -3, -9) skip it in the original and are unchanged by it.
    if (g.value > 1000) g.value = 1000;
    else if (g.value < -2000) g.value = -2000;
    else if (g.value >= -999 && g.value <= -111) g.value = -110;
    return g;
}

} // namespace hailstone::app
