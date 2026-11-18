#include "game/combat.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <span>
#include <vector>

#include "game/coords.h"
#include "game/mechanics/anim_select.h"

namespace hailstone::game {

namespace {

// eqstr_us.txt ids (the original's hex in comments).
constexpr std::uint32_t kStrHe = 12209;          // 0x2fb1 he / she / it
constexpr std::uint32_t kStrAlly = 12212;        // 0x2fb4 .. 0x2fbc: faction 1..9
constexpr std::uint32_t kStrIndifferent = 12216; // 0x2fb8, players
constexpr std::uint32_t kStrConLine = 12239;     // 0x2fcf "%1 %2 -- %3"
constexpr std::uint32_t kStrYou = 12185, kStrYourself = 12186, kStrYOU = 12187;
constexpr std::uint32_t kStrHimself = 12188; // 0x2f9c himself / herself / itself
constexpr std::uint32_t kStrHitLine = 12162;  // 0x2f82 "%1 %2 %3 %4"
constexpr std::uint32_t kStrPoints = 12160;   // 0x2f80 "for %1 points of damage."
constexpr std::uint32_t kStrPoint = 12161;    // 0x2f81 "for %1 point of damage."
constexpr std::uint32_t kStrTryTo = 12158;    // 0x2f7e "%1 try to %2 %3, but %4!"
constexpr std::uint32_t kStrTriesTo = 12159;  // 0x2f7f "%1 tries to %2 %3, but %4!"

std::string str(const EqStrings& s, std::uint32_t id, std::initializer_list<std::string> args = {}) {
    std::vector<std::string> a(args);
    return s.format(id, a);
}

// _toupper on the first character, C locale (a..z only).
std::string capitalised(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

const Spawn* find(const WorldState& w, std::uint32_t id) {
    auto it = w.spawns.find(id);
    return it == w.spawns.end() ? nullptr : &it->second;
}

bool is_pc_model_race(std::uint32_t race) { return race <= 12 || race == 128 || race == 130 || race == 330 || race == 522; }

} // namespace

// ---------------------------------------------------------------- consider

Con standard_con(int theirs, int mine) {
    if (theirs > 105) return Con::Red;
    if (mine > 105) return Con::Gray;
    if (theirs <= 0 || mine <= 0) return Con::None; // never filled in the table
    int diff = theirs - mine;
    if (diff > 3) return Con::Red;
    if (diff > 0) return Con::Yellow;
    if (diff == 0) return Con::White;
    int light = mine > 60 ? mine - 15 : (3 * mine) / 4;
    int green = mine > 60 ? mine - 20 : (2 * mine) / 3;
    light = std::min(light, mine - 5);
    green = std::min(green, light);
    if (theirs >= mine - 5) return Con::DarkBlue;
    if (theirs >= light) return Con::LightBlue;
    if (theirs >= green) return Con::Green;
    return Con::Gray;
}

std::uint32_t con_level_value(const ConLevelInput& in) {
    if (in.spawn_type >= 2 || in.virtual_50) return 0;
    if (in.virtual_98 && (in.field_1e60 != 1 || !in.field_122a)) return 0;
    const std::int32_t mine = in.my_level, theirs = in.their_level;
    const auto diff = static_cast<std::int32_t>(static_cast<std::uint32_t>(theirs) - static_cast<std::uint32_t>(mine));
    std::uint32_t color = in.standard;
    if (in.spawn_type != 0) return color;
    auto scheme = [&](std::int32_t low, std::int32_t high) -> std::uint32_t {
        if (diff < low) return 1;
        return diff >= high ? 7u : 5u;
    };
    switch (in.pvp_rules) {
    case 0: return color;
    case 1: color = scheme(-4, 5); break;
    case 2: color = scheme(-8, 9); break;
    case 4: color = scheme(-5, 6); break;
    default: break;
    }
    if (theirs < 6 && mine > 5) return 1;
    if (theirs > 5 && mine < 6) return 7;
    if (theirs < 6 && mine < 6) return 1;
    return color;
}

Con con_level(const Spawn& target, int my_level, std::int32_t pvp_rules) {
    ConLevelInput in;
    in.pvp_rules = pvp_rules;
    in.my_level = my_level;
    in.their_level = target.level;
    in.standard = static_cast<std::uint32_t>(standard_con(target.level, my_level));
    in.spawn_type = target.is_corpse ? 2 : (target.is_npc ? 1 : 0);
    return static_cast<Con>(con_level_value(in));
}

std::uint32_t con_chat_color(Con con) {
    switch (con) {
    case Con::Green: return 2;
    case Con::LightBlue: return 18;
    case Con::DarkBlue: return 4;
    case Con::White: return 10;
    case Con::Yellow: return 15;
    case Con::Red: return 13;
    default: return 6;
    }
}

Con con_from_server(std::uint32_t level) {
    switch (level) {
    case 2: return Con::Green;
    case 4: return Con::DarkBlue;
    case 6: return Con::Gray;
    case 10: case 20: return Con::White;
    case 13: return Con::Red;
    case 15: return Con::Yellow;
    case 18: return Con::LightBlue;
    default: return Con::None;
    }
}

std::optional<std::uint32_t> npc_name_colour(const Spawn& s, int my_level, std::int32_t pvp_rules, bool enabled) {
    if (!enabled || !s.is_npc || s.is_corpse) return std::nullopt;
    return con_rgb(con_level(s, my_level, pvp_rules));
}

std::optional<std::uint32_t> gm_name_colour(const Spawn& s) {
    // PlayerClient::SetNameSpriteTint @0x58bf00 (rof2decomp name_sprite.cpp): races from 0x8cd up take their own tint
    // first; then IsGM (vtable +0xdc, the record's gm byte) with Type != 1 and Anon == 0 gives the GM green. Anon is
    // the whole field, so anonymous (1) and roleplaying (2) both drop it. Its tint bytes are blue, green, red
    // (docs/graphics.md "Name plates").
    // A deliberate difference: Type 1 is a live NPC only, so the original keeps the green on a corpse that carries the
    // gm flag; here a corpse never takes it and shows the ordinary corpse colour. Stock EQEmu sends corpses with gm 0,
    // so this only shows on a server that does not.
    if (!s.gm || s.race >= 0x8cd || s.is_npc || s.is_corpse || s.anon != 0) return std::nullopt;
    return 0x1da64bu;
}

std::optional<std::uint32_t> mercenary_name_colour(const Spawn& s) {
    // SetNameSpriteTint @0x58bf00: type != 0 with IsMercenary stores the bytes 11 b5 e1, read blue first like every
    // tint of that function (docs/graphics.md "Name plates"). Corpses keep this client's grey.
    if (!s.is_npc || s.is_corpse || !s.mercenary) return std::nullopt;
    return 0xe1b511u;
}

std::uint32_t con_rgb(Con con) {
    switch (con) {
    case Con::Gray: return 0x9a9a9a;
    case Con::Green: return 0x00ee00;
    case Con::LightBlue: return 0x00e0ff;
    case Con::DarkBlue: return 0x4060ff;
    case Con::White: return 0xffffff;
    case Con::Yellow: return 0xffff00;
    case Con::Red: return 0xff2020;
    default: return 0xc0c0c0;
    }
}

std::string consider_message(const EqStrings& strings, const Spawn& target, int my_level, std::uint32_t faction,
                             std::uint32_t short_form, std::int32_t pvp_rules) {
    std::string pronoun = str(strings, kStrHe + std::min<std::uint32_t>(target.gender, 2));
    std::string standing;
    if (!target.is_npc) standing = str(strings, kStrIndifferent);
    else if (faction >= 1 && faction <= 9) standing = str(strings, kStrAlly + faction - 1);
    // A deity consider (short_form 4) prints the deities' relation instead; not supported.
    int difference = static_cast<int>(target.level) - my_level;
    Con con = con_level(target, my_level, pvp_rules);
    std::uint32_t colour = con_chat_color(con);
    std::string text;
    auto pick = [&](std::uint32_t id) { text = str(strings, id); };
    auto build = [&](std::uint32_t id) { text = str(strings, id, {pronoun}); };
    if (short_form != 0) {
        if (colour == 2) build(12223);
        else if (colour == 10) build(12224);
        else if (colour == 13) pick(12225);
    } else if (my_level < 15) {
        switch (colour) {
        case 2: case 18: pick(12227); break;
        case 4: pick(12228); break;
        case 6: pick(12226); break;
        case 10: pick(my_level < 8 ? 12229 : 12230); break;
        case 13: pick(12225); break;
        case 15: pick(12231); break;
        default: break;
        }
    } else if (my_level < 25) {
        switch (colour) {
        case 2: pick(12232); break;
        case 4: pick(12234); break;
        case 6: pick(difference < -5 ? 12226 : 12232); break;
        case 10: build(12235); break;
        case 13: pick(12225); break;
        case 15: pick(12231); break;
        case 18: pick(12233); break;
        default: break;
        }
    } else {
        switch (colour) {
        case 2: pick(12232); break;
        case 4: build(12235); break;
        case 6: pick(difference < -10 ? 12236 : 12237); break;
        case 10: pick(12231); break;
        case 13: pick(12225); break;
        case 15: build(12238); break;
        case 18: pick(12233); break;
        default: break;
        }
    }
    return str(strings, kStrConLine, {display_name(target.name), standing, text});
}

// ---------------------------------------------------------------- melee messages

std::string damage_verb(const EqStrings& strings, std::uint8_t type, std::uint32_t attacker_race, bool third_person) {
    auto pair = [&](std::uint32_t base, std::uint32_t third) { return str(strings, third_person ? third : base); };
    switch (type) {
    case 0: case 2: return pair(12191, 12192);               // crush
    case 1: case 3: return pair(12179, 12180);               // slash
    case 7: case 51: return pair(12183, 12184);              // hit (archery, throwing)
    case 8: return pair(12199, 12200);                       // backstab
    case 10: return pair(12201, 12202);                      // bash
    case 21: case 23: case 52: return pair(12197, 12198);    // strike
    case 26: case 30: case 38: case 71: return pair(12195, 12196); // kick
    case 36: case 77: return pair(12193, 12194);             // pierce
    case 74: return pair(5838, 5839);                        // frenzy
    default: break;                                          // 28 hand to hand and the rest: by race
    }
    // SetDefaultDamageDescByRace @0x5143c0.
    switch (attacker_race) {
    case 43: case 177: case 305: return pair(12163, 12165); // maul
    case 22: case 24: case 34: case 36: case 37: case 38: case 42: case 61: case 74: case 83: case 84: case 87:
    case 91: case 102: case 113: case 120: case 148: case 171: case 176: case 194: case 222: case 223: case 326:
    case 327: case 415: case 416:
        return pair(12164, 12166); // bite
    case 79: case 109: case 129: case 134: case 172: return pair(12171, 12172); // sting
    case 85: case 105: case 174: return pair(12174, 12173);                     // slice
    case 107: case 135: case 169: case 191: return pair(12169, 12170);           // gore
    case 199: return pair(12179, 12180);                                         // slash
    case 220: return pair(12175, 12176);                                         // smash
    case 229: return pair(12177, 12178);                                         // rend
    // Jump table @0x5147a0/@0x5147c8, target @0x5145cd (also race 304).
    case 14: case 23: case 47: case 49: case 50: case 63: case 76: case 111: case 119: case 158: case 184: case 196:
    case 200: case 221: case 304:
        return pair(12167, 12168); // claw
    default: break;
    }
    if (!is_pc_model_race(attacker_race)) return pair(12183, 12184); // hit
    return pair(12181, 12182);                                       // punch
}

std::optional<HitLine> damage_report(const EqStrings& strings, const WorldState& world, std::uint32_t target,
                                     std::uint32_t source, std::uint8_t type, std::int32_t damage,
                                     std::uint32_t special) {
    if (type >= 231) return std::nullopt; // spells: the server sends their messages
    const Spawn* victim = find(world, target);
    const Spawn* attacker = source ? find(world, source) : nullptr;
    if (!victim || !attacker) return std::nullopt;
    const std::uint32_t me = world.player_id;
    bool i_attack = source == me;
    bool on_me = target == me;
    std::string attacker_name, victim_name;
    if (i_attack) {
        attacker_name = str(strings, kStrYou);
        victim_name = on_me ? str(strings, kStrYourself) : display_name(victim->name);
    } else if (on_me) {
        attacker_name = display_name(attacker->name);
        victim_name = str(strings, kStrYOU);
    } else {
        attacker_name = display_name(attacker->name);
        victim_name = source == target ? str(strings, kStrHimself + std::min<std::uint32_t>(attacker->gender, 2))
                                       : display_name(victim->name);
    }
    std::string verb = damage_verb(strings, type, attacker->race, !i_attack && damage > 0);
    HitLine out;
    out.verb = verb;
    out.victim = victim_name;
    out.attacker = attacker_name;
    out.damage = damage;
    out.i_attack = i_attack;
    out.on_me = on_me;
    out.npc_hit_itself = source == target && attacker->is_npc && !attacker->is_corpse;
    // The category (ReportSuccessfulHit's colour 0x109.. / 0x10a / 0x117): the player's own attack, else one on
    // the player (also when the player hits itself), else others (a pet of the player's too, colour 0x148).
    bool hit = damage > 0;
    if (on_me) out.kind = hit ? HitReport::YouBeingHit : HitReport::YouBeingMissed;
    else if (i_attack) out.kind = hit ? HitReport::YourHits : HitReport::YourMisses;
    else out.kind = hit ? HitReport::OthersHits : HitReport::OthersMisses;
    if (hit) {
        std::string amount = str(strings, damage < 2 ? kStrPoint : kStrPoints, {std::to_string(damage)});
        std::string line = str(strings, kStrHitLine, {attacker_name, verb, victim_name, amount});
        if (special == 1) line += " (Wild Rampage)";
        else if (special == 2) line += " (Rampage)";
        out.text = capitalised(line);
        return out;
    }
    // Avoided: who avoided it and how.
    std::string self_name = victim_name;
    if (on_me && i_attack) self_name = "you";
    std::string how;
    switch (damage) {
    case -6: how = on_me ? str(strings, 12154) : str(strings, 12155, {victim_name}); break;
    case -5: how = str(strings, on_me ? 12152 : 12153, {victim_name}); break;
    case -4: how = str(strings, on_me ? 12150 : 12151, {on_me ? self_name : victim_name}); break;
    case -3: how = str(strings, on_me ? 12148 : 12149, {on_me ? self_name : victim_name}); break;
    case -2: how = str(strings, on_me ? 12146 : 12147, {on_me ? self_name : victim_name}); break;
    case -1: how = str(strings, on_me ? 12144 : 12145, {on_me ? self_name : victim_name}); break;
    default: how = str(strings, i_attack ? 12156 : 12157); break;
    }
    out.text = capitalised(str(strings, i_attack ? kStrTryTo : kStrTriesTo, {attacker_name, verb, victim_name, how}));
    return out;
}

bool hit_line_shown(const HitLine& line, bool by_my_pet, const ChatFilters& filters, bool report) {
    // Deliberate change from RoF2 (a fix): with filter 14 or 15 hiding others' lines, the original replaces
    // `report` with the pet flag (@0x53092c, @0x53078b), so the pet's lines beyond the report range showed only
    // while others' were hidden. Here an unreported line stays hidden either way.
    if (!report) return false;
    auto hidden = [&filters](int filter) { return filters.mode(filter) == 1; };
    bool shown = report;
    if (line.damage > 0) {
        if (!line.on_me && !line.i_attack && hidden(15)) shown = by_my_pet;
        return shown && !line.npc_hit_itself;
    }
    if (line.on_me && hidden(16)) shown = false;
    if (line.i_attack) {
        if (hidden(13)) shown = false;
    } else if (!line.on_me && hidden(14)) {
        shown = by_my_pet;
    }
    return shown;
}

std::optional<std::string> damage_message(const EqStrings& strings, const WorldState& world, std::uint32_t target,
                                          std::uint32_t source, std::uint8_t type, std::int32_t damage,
                                          std::uint32_t special) {
    auto r = damage_report(strings, world, target, source, type, damage, special);
    if (!r) return std::nullopt;
    return r->text;
}

std::optional<std::string> report_hit_line(const EqStrings& strings, const HitLine& line, std::int32_t mode) {
    bool hit = line.kind == HitReport::YourHits || line.kind == HitReport::YouBeingHit ||
               line.kind == HitReport::OthersHits;
    if (mode == 0) return line.text;
    if (mode == 2) return hit ? std::to_string(line.damage) : str(strings, 12156); // "%d" / eqstr 0x2f7c
    if (mode != 1) return std::nullopt;
    std::string number = std::to_string(line.damage);
    switch (line.kind) {
    case HitReport::YourHits: return str(strings, 0x852, {line.verb, line.victim, number});
    case HitReport::YouBeingHit: return str(strings, 0x83c, {line.attacker, line.verb, number});
    case HitReport::OthersHits: return str(strings, 0x850, {line.attacker, line.verb, line.victim, number});
    case HitReport::YourMisses: return str(strings, 0x3abf, {line.victim});
    case HitReport::YouBeingMissed: return str(strings, 0x3ac0, {line.attacker});
    case HitReport::OthersMisses: return str(strings, 0x3ac1, {line.attacker, line.victim});
    }
    return std::nullopt;
}

std::optional<std::string> death_message(const EqStrings& strings, const WorldState& world, std::uint32_t victim,
                                         std::uint32_t killer, std::uint32_t attack_skill) {
    const Spawn* dead = find(world, victim);
    const Spawn* by = killer ? find(world, killer) : nullptr;
    auto skill = static_cast<std::int8_t>(attack_skill & 0xff);
    std::string dead_name = dead ? display_name(dead->name) : std::string();
    std::string by_name = by ? display_name(by->name) : std::string();
    if (victim == world.player_id) {
        if (skill == -6) return str(strings, 12105);
        if (skill == -5) return str(strings, 12104);
        if (skill == -4) return str(strings, 12103);
        if (killer == 0) return str(strings, 12106);
        if (!by) return str(strings, 12108);
        return capitalised(str(strings, 12107, {by_name}));
    }
    if (skill == -6) return capitalised(str(strings, 12111, {dead_name}));
    if (skill == -5) return capitalised(str(strings, 12110, {dead_name}));
    if (skill == -4) return capitalised(str(strings, 12109, {dead_name}));
    if (killer == 0) return capitalised(str(strings, 12112, {dead_name}));
    if (killer == world.player_id) return str(strings, 12113, {dead_name});
    if (!by) return capitalised(str(strings, 12115, {dead_name}));
    // NOLINTNEXTLINE(clang-analyzer-core.NullDereference): by is non-null here (checked above), so by == dead means dead is too.
    if (by == dead && dead->is_npc) return std::nullopt;
    return capitalised(str(strings, 12114, {dead_name, by_name}));
}

// ---------------------------------------------------------------- animation

std::optional<std::uint8_t> hit_animation(const HitAnimInput& in) {
    if (!in.target_alive) return std::nullopt; // @0x5311f2: type 2 and above return
    std::int32_t anim = -1;
    std::int32_t damage = in.damage;
    if (in.type == 231) {
        if (in.spell_target_anim != 0 && in.spell_target_anim < 0x4f &&
            (in.target_stand_state == 100 || in.target_stand_state == 102))
            anim = in.spell_target_anim;
        if (!(damage > 0 && in.spell_detrimental)) damage = 0; // the landed damage is set only then
    } else if (is_environment_hit(in.type)) {
        // The switch's cases -0xd..-3 (@0x52fc58..), then @0x5302f0: no damage drops the animation.
        if (damage == 0) return std::nullopt;
        if (in.type <= 0xf9) anim = 0xc;      // D01
        else if (in.type <= 0xfb) anim = 0xf; // D04
        else anim = 0xe;                      // D03
    }
    if (damage > 0 && anim == -1 && in.local_stand_state != 115 && in.local_stand_state != 120) anim = 0xd;
    if (anim == -1) return std::nullopt;
    return static_cast<std::uint8_t>(anim);
}

bool hit_landed(const HitAnimInput& in) {
    if (!in.target_alive || in.damage <= 0) return false; // @0x5311ee: Type 2 and above return; 0 < the landed damage
    if (in.type == 231) return in.spell_detrimental;      // @0x530ddb: set only for a detrimental spell
    return true;
}

EnvHit environment_hit(const EqStrings& strings, const EnvHitInput& in) {
    EnvHit out;
    if (!is_environment_hit(in.type)) return out;
    std::uint32_t id = 0;
    bool named = !in.on_me; // another's line takes the name (BuildStringFromEQSTR with the victim's name)
    switch (in.type) {
    case 0xf3: id = 0x2378; break; // mind burns
    case 0xf4: id = 0x165a; break; // rotting
    case 0xf5: id = 0x2f6b; break; // chilled
    case 0xf6: id = 0x2f6d; break; // freezing
    case 0xf7: id = 0x2f69; break; // tormented
    case 0xf8: id = 0x2f67; break; // burned
    case 0xf9: id = 0x2f65; break; // thorns
    case 0xfa: id = 0x2f63; break; // burning
    case 0xfb: // drowning; suffocating in zone 0xab
        id = in.on_me ? (in.zone_id == 0xab ? 0x2f5f : 0x2f60) : (in.zone_id == 0xab ? 0x2f61 : 0x2f62);
        break;
    case 0xfc: // falling: "%1 were injured by falling." with "YOU" (0x2f9b) on the local player
        id = in.on_me ? 0x2f56 : 0x2f57;
        named = true;
        out.fall_hit_sound = true;
        break;
    case 0xfd: // a trap
        if (!in.on_me) {
            id = 0x2f58;
            break;
        }
        switch (in.spell) {
        case 0x73: case 0x74: id = 0x2f59; break; // saw blade
        case 0x78: case 0x7d: id = 0x2f5a; break; // spear
        case 0x82: id = 0x2f5b; break;            // pendulum
        case 0x87: id = 0x2f5c; break;            // blade
        case 0x8c: case 0x91: case 0x92: case 0x96: case 0x97: case 0x98:
            id = 0x2f5d; // crushed
            out.fall_hit_sound = true;
            break;
        default: id = 0x2f5e; break;
        }
        break;
    default: break;
    }
    // Another's line for 0xf3..0xfa is the next id, and none with chat filter 0x11 at mode 1.
    if (!in.on_me && in.type <= 0xfa) {
        if (in.filter_mode == 1) id = 0;
        else ++id;
    }
    out.color = in.on_me ? 0x10a : in.type <= 0xfa ? 0x149 : 0x117;
    if (in.damage == 0 || id == 0) return out;
    std::string name = in.on_me ? str(strings, kStrYOU) : in.victim;
    out.text = capitalised(named ? str(strings, id, {name}) : str(strings, id));
    return out;
}

const char* animation_code(std::uint8_t action) {
    // The old-style names at 0xb787a0 (game/mechanics/anim_select.h old_anim_clip; literals, NUL-terminated).
    std::string_view code = mechanics::old_anim_clip(action);
    return code.empty() ? nullptr : code.data();
}

std::string spawn_label(const Spawn& s) {
    std::string label = display_name(s.name);
    if (!s.last_name.empty()) label += " " + s.last_name;
    return label;
}

bool name_sprite_shows_guild(const Spawn& s, int show_names) {
    if (s.is_npc || s.is_corpse || (show_names != 3 && show_names != 4)) return false;
    if (s.guild_id == 0xffffffff || s.guild_id == 0 || !s.guild_show) return false;
    return s.anon == 0 || ((s.anon >> 1) & 1) != 0;
}

NameSpriteMarketWords name_sprite_market_words(const Spawn& s, const EqStrings& strings) {
    NameSpriteMarketWords out;
    if (s.is_npc || s.is_corpse) return out;
    // The string table's word; a plain one where the player's eqstr_us.txt lacks it (the original would show its
    // table's own, which is always there).
    auto word = [&](std::uint32_t id, const char* fallback) {
        out.prefix += strings.has(id) ? strings.format(id, {}) : std::string(fallback);
        out.prefix += ' ';
    };
    if (s.offline) word(766, "Offline");
    if (s.trader) word(5503, "Trader");
    else if (s.buyer) word(6055, "Buyer");
    out.replaces_title = s.trader || s.buyer;
    return out;
}

bool name_sprite_switched_on(const Spawn& s, bool local_player, bool model_hidden, const NameSpriteSwitches& on) {
    if (s.is_npc || s.is_corpse) return on.npc_names;
    if (local_player) return on.pc_names && on.own_name && !model_hidden;
    return on.pc_names;
}

std::string npc_owner_line(const Spawn& s, bool pet_owner_names) {
    if (!s.is_npc || s.is_corpse || s.last_name.empty()) return {};
    if (s.pet_owner_id == 0) return "(" + s.last_name + ")";
    if (!pet_owner_names) return {};
    std::string owner = display_name(s.last_name); // CEverQuest::trimName
    return owner.empty() ? std::string() : "(" + owner + ")";
}

std::string name_sprite_with_marks(const Spawn& s, const std::string& name, const NameSpriteMarks& m) {
    if (!m.leadership || (!s.is_npc && !s.is_corpse)) return name;
    auto has = [&](const auto& ids) { return std::find(ids.begin(), ids.end(), s.id) != ids.end(); };
    const bool assisted = has(m.assist_targets);
    std::string text = assisted ? ">> " : "";
    int mark = -1;
    for (int i = 0; i < 3; ++i)
        if (m.group_marks[static_cast<std::size_t>(i)] == s.id) { mark = i; break; }
    for (int i = 0; i < 3; ++i)
        if (m.raid_marks[static_cast<std::size_t>(i)] == s.id) { mark = i; break; }
    if (mark != -1) text += std::to_string(mark + 1) + " - ";
    text += name;
    const bool npc = s.is_npc && !s.is_corpse;
    if (npc && m.npc_health && (s.id == m.target_id || assisted || has(m.group_marks) || has(m.raid_marks)))
        text += " - " + std::to_string(s.hp_percent) + "%";
    if (assisted) text += " <<";
    return text;
}

// ---------------------------------------------------------------- targeting

float heading_to(const glm::vec3& from, const glm::vec3& to) {
    return yaw_to_heading(std::atan2(to.y - from.y, to.x - from.x));
}

namespace {

struct Candidate {
    std::uint32_t id;
    float distance;
};

std::vector<Candidate> npcs_in_front(const WorldState& world, const glm::vec3& from, float yaw, float max_distance) {
    std::vector<Candidate> out;
    glm::vec2 forward(std::cos(yaw), std::sin(yaw));
    for (const auto& [id, s] : world.spawns) {
        if (id == world.player_id || !s.is_npc || s.is_corpse || !s.targetable) continue;
        glm::vec3 p = s.display_valid ? s.display_position : s.position;
        float d = glm::distance(p, from);
        if (d > max_distance) continue;
        glm::vec2 to(p.x - from.x, p.y - from.y);
        if (glm::dot(to, forward) < 0.0f) continue;
        out.push_back({id, d});
    }
    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        return a.distance < b.distance || (a.distance == b.distance && a.id < b.id);
    });
    return out;
}

} // namespace

std::uint32_t nearest_npc(const WorldState& world, const glm::vec3& from, float yaw, float max_distance,
                          std::uint32_t exclude) {
    for (const auto& c : npcs_in_front(world, from, yaw, max_distance))
        if (c.id != exclude) return c.id;
    return 0;
}

std::uint32_t next_npc(const WorldState& world, const glm::vec3& from, float yaw, float max_distance,
                       std::uint32_t current) {
    auto list = npcs_in_front(world, from, yaw, max_distance);
    if (list.empty()) return 0;
    for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i].id == current) return list[(i + 1) % list.size()].id;
    return list.front().id;
}

std::optional<std::uint32_t> pick_spawn(const WorldState& world, const glm::vec3& origin, const glm::vec3& dir,
                                        float max_distance, std::uint32_t skip) {
    std::optional<std::uint32_t> best;
    float best_t = max_distance;
    glm::vec3 d = glm::normalize(dir);
    for (const auto& [id, s] : world.spawns) {
        if (id == skip || !s.targetable) continue;
        glm::vec3 p = s.display_valid ? s.display_position : s.position;
        // Body: the feet are AvatarHeight below the spawn's z (game/spawn_heights.h); the height is about the size.
        float size = s.heights.height > 0.0f ? s.heights.height : 6.0f;
        float radius = std::max(1.5f, size * 0.4f);
        float bottom = p.z - s.heights.avatar_height;
        float top = bottom + std::max(2.0f, size * 1.1f);
        if (s.is_corpse) top = bottom + 2.0f;
        // Ray against the infinite cylinder in XY, then clip to [bottom, top].
        glm::vec2 o(origin.x - p.x, origin.y - p.y), v(d.x, d.y);
        float a = glm::dot(v, v), b = 2.0f * glm::dot(o, v), c = glm::dot(o, o) - radius * radius;
        float t0 = 0.0f, t1 = max_distance;
        if (a < 1e-8f) {
            if (c > 0.0f) continue; // vertical ray outside the cylinder
        } else {
            float disc = b * b - 4.0f * a * c;
            if (disc < 0.0f) continue;
            float sq = std::sqrt(disc);
            t0 = (-b - sq) / (2.0f * a);
            t1 = (-b + sq) / (2.0f * a);
        }
        // Slab in z.
        if (std::abs(d.z) < 1e-8f) {
            if (origin.z < bottom || origin.z > top) continue;
        } else {
            float za = (bottom - origin.z) / d.z, zb = (top - origin.z) / d.z;
            if (za > zb) std::swap(za, zb);
            t0 = std::max(t0, za);
            t1 = std::min(t1, zb);
        }
        if (t1 < t0 || t1 < 0.0f) continue;
        float t = std::max(t0, 0.0f);
        if (t < best_t) {
            best_t = t;
            best = id;
        }
    }
    return best;
}

} // namespace hailstone::game
