#include "game/spells.h"

#include "game/mechanics/spell_gems.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <span>
#include <unordered_map>
#include <vector>

#include "core/game_dir.h"
#include "core/log.h"
#include "game/spell_record.h"

namespace hailstone::game {

namespace {

constexpr std::int32_t kSpaHoldBuffs = 0x1b4; // SPA 436: buff timers held

constexpr std::size_t kColumns = 237;

std::int64_t to_int(std::string_view s) {
    // Integer columns are plain decimal; a few hold "0.000000"-style text, read up to the dot.
    std::int64_t v = 0;
    const char* b = s.data();
    const char* e = s.data() + s.size();
    if (b != e && *b == '+') ++b;
    std::from_chars(b, e, v);
    return v;
}

float to_float(std::string_view s) {
    if (s.empty()) return 0.0f;
    std::string tmp(s);
    float f = std::strtof(tmp.c_str(), nullptr);
    return std::isfinite(f) ? f : 0.0f;
}

// Splits a '^' line into its fields (a final empty field after a trailing '^' included).
std::vector<std::string_view> carets(std::string_view line) {
    std::vector<std::string_view> f;
    std::size_t pos = 0;
    while (true) {
        std::size_t caret = line.find('^', pos);
        f.push_back(line.substr(pos, caret == std::string_view::npos ? std::string_view::npos : caret - pos));
        if (caret == std::string_view::npos) break;
        pos = caret + 1;
    }
    return f;
}

// The live client's spells_us.txt column for each RoF2 column, -1 where live has none (worked out 2026-10-26 from the
// 34420 spells both files have under the same id and name: docs/layout.md "RoF2 or live"). The texts (RoF2 4..8)
// are in spells_us_str.txt and the effects (RoF2 20..55, 70..81, 86..97) in the last field.
constexpr std::array<int, kColumns> live_spell_columns() {
    std::array<int, kColumns> m{};
    for (auto& v : m) v = -1;
    m[0] = 0;
    m[1] = 1;
    m[3] = 3;
    for (int c = 9; c < 20; ++c) m[c] = c - 5;
    for (int c = 58; c < 70; ++c) m[c] = c - 43;
    m[82] = 27;
    m[83] = 28;
    m[85] = 29;
    for (int c = 98; c < 143; ++c) m[c] = c - 68;
    for (int c = 144; c < 149; ++c) m[c] = c - 69;
    m[150] = 81;
    m[151] = 82;
    for (int c = 153; c < 168; ++c) m[c] = c - 70;
    m[168] = 131; // is_skill
    for (int c = 173; c < 180; ++c) m[c] = c - 74;
    for (int c = 181; c < 219; ++c) m[c] = c - 75;
    m[206] = -1; // its place holds is_skill
    for (int c = 219; c < 237; ++c) m[c] = c - 76;
    return m;
}

} // namespace

std::string spell_line_from_live(std::string_view line, const std::array<std::string_view, 5>& texts) {
    static constexpr std::array<int, kColumns> kMap = live_spell_columns();
    std::vector<std::string_view> live = carets(line);
    std::array<std::string_view, kColumns> out;
    out.fill("0");
    for (std::size_t c = 0; c < kColumns; ++c)
        if (kMap[c] >= 0 && static_cast<std::size_t>(kMap[c]) < live.size()) out[c] = live[static_cast<std::size_t>(kMap[c])];
    out[2] = "";
    for (std::size_t k = 0; k < 5; ++k) out[4 + k] = texts[k];
    out[149] = "1"; // deletable: every RoF2 spell; live has no column for it
    // Effects: "slot|spa|base|base2|formula|max" joined by '$', slots 1..12; an empty slot is SPA 254, formula 100.
    for (std::size_t k = 0; k < 12; ++k) {
        out[86 + k] = "254";
        out[70 + k] = "100";
    }
    std::string_view effects = live.back();
    while (!effects.empty()) {
        std::size_t end = std::min(effects.find('$'), effects.size());
        std::string_view e = effects.substr(0, end);
        effects.remove_prefix(std::min(end + 1, effects.size()));
        std::array<std::string_view, 6> p{};
        std::size_t n = 0;
        for (std::size_t at = 0; n < 6 && at <= e.size();) {
            std::size_t bar = std::min(e.find('|', at), e.size());
            p[n++] = e.substr(at, bar - at);
            at = bar + 1;
        }
        std::int64_t slot = to_int(p[0]);
        if (n < 6 || slot < 1 || slot > 12) continue;
        std::size_t k = static_cast<std::size_t>(slot - 1);
        out[86 + k] = p[1];
        out[20 + k] = p[2];
        out[32 + k] = p[3];
        out[70 + k] = p[4];
        out[44 + k] = p[5];
    }
    std::string joined;
    for (std::size_t c = 0; c < kColumns; ++c) {
        if (c) joined += '^';
        joined += out[c];
    }
    return joined;
}

std::string spells_text_from_live(std::string_view spells, std::string_view strings) {
    // spells_us_str.txt: "id^you cast^other casts^cast on you^cast on other^fades^" after a '#' header.
    std::unordered_map<std::int64_t, std::array<std::string_view, 5>> texts;
    auto lines = [](std::string_view text, auto&& fn) {
        for (std::size_t pos = 0; pos < text.size();) {
            std::size_t end = std::min(text.find('\n', pos), text.size());
            std::string_view line = text.substr(pos, end - pos);
            pos = end + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!line.empty() && std::isdigit(static_cast<unsigned char>(line[0]))) fn(line);
        }
    };
    lines(strings, [&](std::string_view line) {
        std::vector<std::string_view> f = carets(line);
        std::array<std::string_view, 5> t{};
        for (std::size_t k = 0; k < 5 && k + 1 < f.size(); ++k) t[k] = f[k + 1];
        texts[to_int(f[0])] = t;
    });
    std::string out;
    out.reserve(spells.size() * 2);
    lines(spells, [&](std::string_view line) {
        auto it = texts.find(to_int(line.substr(0, line.find('^'))));
        out += spell_line_from_live(line, it == texts.end() ? std::array<std::string_view, 5>{} : it->second);
        out += '\n';
    });
    return out;
}

bool SpellData::has_effect(int spa) const {
    return std::find(effect.begin(), effect.end(), spa) != effect.end();
}

bool SpellData::has_effect_before_blank(int spa) const {
    for (std::int32_t e : effect) {
        if (e == 254) return false;
        if (e == spa) return true;
    }
    return false;
}

std::uint8_t SpellData::level_needed(std::uint8_t class_id) const {
    if (class_id < 1 || class_id > 16) return 255;
    std::uint8_t l = class_level[class_id - 1u];
    return l == 0 ? 255 : l;
}

std::optional<SpellData> SpellTable::parse_line(std::string_view line) {
    if (line.empty() || !std::isdigit(static_cast<unsigned char>(line[0]))) return std::nullopt;
    std::array<std::string_view, kColumns> f{};
    std::size_t n = 0, pos = 0;
    while (n < kColumns) {
        std::size_t caret = line.find('^', pos);
        f[n++] = line.substr(pos, caret == std::string_view::npos ? std::string_view::npos : caret - pos);
        if (caret == std::string_view::npos) break;
        pos = caret + 1;
    }
    // EQ_Spell_InitFromLine reads the columns it knows; the file always has 237.
    if (n < 145) return std::nullopt;
    auto i32 = [&](std::size_t c) { return static_cast<std::int32_t>(to_int(f[c])); };
    auto u32 = [&](std::size_t c) { return static_cast<std::uint32_t>(to_int(f[c])); };
    auto u8 = [&](std::size_t c) { return static_cast<std::uint8_t>(to_int(f[c])); };
    auto b = [&](std::size_t c) { return to_int(f[c]) != 0; };
    SpellData s;
    s.id = u32(0);
    s.name = std::string(f[1]);
    s.player_1 = std::string(f[2]);
    s.teleport_zone = std::string(f[3]);
    s.cast_by_me = std::string(f[4]);
    s.cast_by_other = std::string(f[5]);
    s.cast_on_you = std::string(f[6]);
    s.cast_on_other = std::string(f[7]);
    s.wear_off = std::string(f[8]);
    s.range = to_float(f[9]);
    s.ae_range = to_float(f[10]);
    s.push_back = to_float(f[11]);
    s.push_up = to_float(f[12]);
    s.cast_time = u32(13);
    s.recovery_time = u32(14);
    s.recast_time = u32(15);
    s.duration_formula = u32(16);
    s.duration_cap = u32(17);
    s.ae_duration = u32(18);
    s.mana = i32(19);
    for (std::size_t k = 0; k < 12; ++k) {
        s.base[k] = i32(20 + k);
        s.base2[k] = i32(32 + k);
        s.max[k] = i32(44 + k);
        s.formula[k] = i32(70 + k);
        s.effect[k] = i32(86 + k);
    }
    // LoadTextSpells limits the missile duration except for a first-slot SPA 76 cloud (native +0x24/+0x11c).
    if (s.ae_duration > 14000 && s.effect[0] != 76) s.ae_duration = 14000;
    s.book_icon = i32(56);
    s.gem_icon = i32(57);
    for (std::size_t k = 0; k < 4; ++k) {
        s.reagent_id[k] = i32(58 + k);
        s.reagent_count[k] = i32(62 + k);
        s.no_expend_reagent[k] = i32(66 + k);
    }
    s.light_type = u8(82);
    s.spell_type = u8(83);
    s.activated = u8(84);
    s.resist = u8(85);
    s.target_type = u8(98);
    s.cast_difficulty = u8(99);
    s.skill = u8(100);
    s.zone_type = u8(101);
    s.environment = u8(102);
    s.time_of_day = u8(103);
    for (std::size_t k = 0; k < 16; ++k) s.class_level[k] = u8(104 + k);
    s.casting_anim = u8(120);
    s.target_anim = u8(121);
    s.travel_type = u8(122);
    s.spa_index = i32(123);
    for (std::size_t k = 0; k < 17; ++k)
        if (i32(125 + k) != 0) s.deity_mask |= 1u << k;
    s.cancel_on_sit = b(124);
    s.spell_icon = i32(144);
    if (n > 234) {
        s.spell_anim = i32(145);
        s.uninterruptable = b(146);
        s.resist_adj = i32(147);
        s.not_stackable_dot = b(148);
        s.deletable = b(149);
        s.duration_window = b(154);
        s.recourse_link = i32(150);
        s.description_id = i32(155);
        s.category = i32(156);
        s.subcategory = i32(157);
        s.subcategory2 = i32(158);
        s.npc_no_los = b(159);
        s.endurance_cost = i32(166);
        s.reuse_timer_index = i32(167);
        s.is_skill = b(168);
        s.endurance_upkeep = i32(174);
        s.hit_count_type = i32(175);
        s.hit_count = i32(176);
        s.global_group = i32(180);
        s.pcnpc_only = i32(183);
        s.cast_not_standing = b(184);
        s.no_dispell = b(186);
        s.sneak = b(196);
        s.not_focusable = b(197);
        s.stacks_with_self = b(203);
        s.suspendable = b(200);
        s.spell_group = i32(207);
        s.spell_rank = i32(208);
        s.req_association = i32(211);
        s.bypass_regen_check = b(212);
        s.can_cast_in_combat = b(213);
        s.can_cast_out_of_combat = b(214);
        s.max_targets = i32(218);
        s.spell_class = i32(221);
        s.spell_subclass = i32(222);
        s.distance_mod_start_x = to_float(f[227]);
        s.distance_mod_start_mod = to_float(f[228]);
        s.distance_mod_end_x = to_float(f[229]);
        {
            // As spell_record.cpp's range_ratio: the end modifier is used unrounded (the parser's double).
            std::string end_mod(f[230]);
            double v = end_mod.empty() ? 0.0 : std::strtod(end_mod.c_str(), nullptr);
            if (!std::isfinite(v)) v = 0.0;
            s.distance_mod_end_mod = static_cast<float>(v);
            double d = static_cast<double>(s.distance_mod_end_x) - static_cast<double>(s.distance_mod_start_x);
            constexpr double kEps = 1.1920928955078125e-07;
            if (-kEps < d && d < kEps) d = 1.0;
            s.distance_mod_ratio = static_cast<float>((v - static_cast<double>(s.distance_mod_start_mod)) / d);
        }
        s.min_range = to_float(f[231]);
        s.no_remove = b(232);
        s.only_during_fast_regen = b(234);
        s.spell_subgroup = i32(236);
    }
    return s;
}

SpellTable::SpellTable() : default_crc_(spell_record_crc(unknown_spell_record())) {}

std::size_t SpellTable::parse(std::string_view text) {
    std::size_t count = 0, pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty() && std::isdigit(static_cast<unsigned char>(line[0]))) {
            // LoadTextSpells: SpellsCrc32[id] = EQ_Spell_CalcCRC of the parsed record (ids up to 45000).
            SpellRecord record = spell_record_from_line(line);
            std::uint32_t id = static_cast<std::uint32_t>(to_int(line.substr(0, line.find('^'))));
            if (id > kMaxSpellId) break;
            crcs_[id] = spell_record_crc(record);
        }
        auto s = parse_line(line);
        if (!s) continue;
        // LoadTextSpells stops at the first id above 45000 (@0x5c1db1).
        if (s->id > kMaxSpellId) break;
        spells_[s->id] = std::move(*s);
        ++count;
    }
    build_indices();
    return count;
}

namespace {
std::string lower_name(std::string_view name) {
    std::string out(name);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::uint64_t group_key(std::int32_t group, std::int32_t subgroup) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(group)) << 32) | static_cast<std::uint32_t>(subgroup);
}
} // namespace

void SpellTable::build_indices() {
    by_name_.clear();
    by_group_.clear();
    for (const auto& [id, s] : spells_) {
        auto [it, added] = by_name_.try_emplace(lower_name(s.name), id);
        if (!added && id < it->second) it->second = id;
        if (s.spell_group > 0) by_group_[group_key(s.spell_group, s.spell_subgroup)].push_back({s.spell_rank, id});
    }
    for (auto& [key, ranked] : by_group_)
        std::sort(ranked.begin(), ranked.end(), [](const RankedId& a, const RankedId& b) {
            return a.rank != b.rank ? a.rank < b.rank : a.id < b.id;
        });
}

bool spells_text_is_live_layout(std::string_view text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = std::min(text.find('\n', pos), text.size());
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty() || !std::isdigit(static_cast<unsigned char>(line[0]))) continue;
        return std::count(line.begin(), line.end(), '^') < 200; // RoF2: 236; the live client: 165
    }
    return false;
}

bool SpellTable::load(const GameDir& game) {
    auto data = game.read("spells_us.txt");
    if (!data) return false;
    std::string_view text(reinterpret_cast<const char*>(data->data()), data->size());
    std::string converted;
    if (spells_text_is_live_layout(text)) {
        // The live client's layout, read as RoF2's: the server's own spells_us.txt (in its assets folder) is the
        // one that matches it; this is the fallback.
        auto strings = game.read("spells_us_str.txt");
        converted = spells_text_from_live(
            text, strings ? std::string_view(reinterpret_cast<const char*>(strings->data()), strings->size())
                          : std::string_view());
        log::warn("spells_us.txt is the live client's: read in RoF2's layout{}; the server's own spells_us.txt "
                  "would match it",
                  strings ? "" : " (no spells_us_str.txt: no spell messages)");
        text = converted;
    }
    spells_.clear();
    crcs_.clear();
    auto bytes = [](const auto& d) {
        return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(d.data()), d.size());
    };
    file_crc_ = mem_checker1(bytes(*data));
    auto assoc = game.read("Resources/SpellRequirementAssociations.txt");
    assoc_file_crc_ = assoc ? mem_checker1(bytes(*assoc)) : 0;
    parse(text);
    return true;
}

std::uint32_t SpellTable::spell_crc(std::uint32_t id) const {
    if (id > kMaxSpellId) return default_crc_;
    auto it = crcs_.find(id);
    return it == crcs_.end() ? default_crc_ : it->second;
}

const SpellData* SpellTable::get(std::uint32_t id) const {
    auto it = spells_.find(id);
    return it == spells_.end() ? nullptr : &it->second;
}

const SpellData* SpellTable::find_by_name(std::string_view name) const {
    auto it = by_name_.find(lower_name(name));
    return it == by_name_.end() ? nullptr : get(it->second);
}

const SpellData* SpellTable::by_group_and_rank(std::int32_t group, std::int32_t subgroup, std::int32_t rank,
                                               bool below) const {
    if (group <= 0) return nullptr;
    auto it = by_group_.find(group_key(group, subgroup));
    if (it == by_group_.end()) return nullptr;
    const std::vector<RankedId>& ranked = it->second; // by rank, then id
    const auto first_of = std::lower_bound(ranked.begin(), ranked.end(), rank,
                                           [](const RankedId& r, std::int32_t v) { return r.rank < v; });
    const auto past = std::upper_bound(ranked.begin(), ranked.end(), rank,
                                       [](std::int32_t v, const RankedId& r) { return v < r.rank; });
    // Of exactly `rank`, the highest id.
    if (past != first_of) return get(std::prev(past)->id);
    // Else the highest rank (then id): of all of them at -1 (none is -1 here), or of those below `rank`.
    if (rank == -1) return get(ranked.back().id);
    if (below && first_of != ranked.begin()) return get(std::prev(first_of)->id);
    return nullptr;
}

bool target_type_needs_target(std::uint8_t t) {
    switch (t) {
    case 5: case 7: case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 18:
    case 19: case 20: case 21: case 22: case 23: case 27: case 28: case 30: case 34: case 35: case 38: case 39:
    case 43: case 46: case 47:
        return true;
    default:
        return false;
    }
}

bool target_type_requires_target(std::uint8_t t) {
    switch (t) {
    // @0x43b75e: these jump to the "need a target" path (0x43bc5a → eqstr 214) without one.
    case 5: case 9: case 10: case 11: case 12: case 13: case 15: case 16: case 17: case 18: case 20: case 27:
    case 34: case 35: case 39: case 43: case 47:
    // Targeted AE (8, 21, 22, 23, 28).
    case 8: case 21: case 22: case 23: case 28:
        return true;
    default:
        return false;
    }
}

std::uint8_t level_needed(const SpellData& spell, std::uint32_t class_mask, std::uint8_t class_id) {
    if (class_mask == 0) return spell.level_needed(class_id);
    std::uint8_t best = 255;
    for (std::uint8_t c = 1; c <= 16; ++c)
        if (class_mask & (1u << (c - 1))) best = std::min(best, spell.level_needed(c));
    return best;
}

std::optional<std::string> spell_hit_message(const EqStrings& strings, const WorldState& world, const SpellData& spell,
                                             std::uint32_t target, std::int32_t damage) {
    auto it = world.spawns.find(target);
    if (it == world.spawns.end()) return std::nullopt;
    std::string line;
    if (target == world.player_id) {
        if (damage < 0) {
            std::string amount = std::to_string(-static_cast<std::int64_t>(damage));
            line = strings.format(damage < -1 ? 4600 : 4601, std::array<std::string, 2>{spell.cast_on_you, amount});
        } else if (damage == 0) {
            line = spell.cast_on_you;
        } else {
            line = strings.format(damage > 1 ? 12203 : 12204,
                                  std::array<std::string, 2>{spell.cast_on_you, std::to_string(damage)});
        }
    } else {
        // "%1<CastOnAnother>" with the target's name; no text, no line.
        if (spell.cast_on_other.empty()) return std::nullopt;
        line = display_name(it->second.name) + spell.cast_on_other;
    }
    while (!line.empty() && line.front() == ' ') line.erase(line.begin());
    if (line.empty()) return std::nullopt;
    return line;
}

namespace {

bool hit_amount(std::string_view text) {
    // Server amounts are unsigned decimal; older damage reports use grouped thousands (ConvertArray). Validate
    // the complete value before shortening, including comma placement, rather than consuming a numeric prefix.
    if (text.empty()) return false;
    std::string digits;
    std::size_t group = 0;
    bool grouped = false;
    for (char c : text) {
        if (c == ',') {
            if (group == 0 || (grouped ? group != 3 : group > 3)) return false;
            grouped = true;
            group = 0;
        } else {
            if (c < '0' || c > '9') return false;
            digits += c;
            ++group;
        }
    }
    if (group == 0 || (grouped && group != 3)) return false;
    std::uint64_t amount = 0;
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), amount);
    return result.ec == std::errc{} && result.ptr == digits.data() + digits.size();
}

} // namespace

std::optional<SpellHitLine> spell_message_hit_report(std::uint32_t eqstr, std::span<const std::string> args) {
    // Message identity and argument roles follow EQEmu's string_ids.h and spell damage/heal senders. The compact
    // wording is this client's own UI. In particular no language-dependent scan of a rendered line is involved.
    std::size_t count = 0, amount = 0;
    SpellHitReport kind = SpellHitReport::DirectDamage;
    switch (eqstr) {
    case 9073: count = 3; amount = 2; break;  // non-melee hit: attacker, target, amount
    case 434: count = 2; amount = 1; break;   // legacy non-melee hit (also damage shields): target, amount
    case 12481: count = 1; break;             // legacy non-melee hit on the player: amount
    case 428: count = 1; break;               // own critical blast: amount
    case 1040: count = 2; amount = 1; break;  // another's critical blast: attacker, amount
    case 9072: count = 3; amount = 1; kind = SpellHitReport::DamageOverTime; break;
    case 12954: count = 3; kind = SpellHitReport::DamageOverTime; break;
    case 13327: count = 4; amount = 1; kind = SpellHitReport::DamageOverTime; break;
    case 9068: case 419: case 1039:
        count = 2; amount = 1; kind = SpellHitReport::Healing; break;
    case 427:
        // EQEmu uses this same critical-heal report (and 1039) for direct and periodic healing. With no tick
        // discriminator in the message, both follow Healing rather than guessing from nearby chat or buffs.
        count = 1; kind = SpellHitReport::Healing; break;
    case 12976: count = 2; kind = SpellHitReport::HealingOverTime; break;
    case 12997: case 12998: count = 3; amount = 1; kind = SpellHitReport::HealingOverTime; break;
    default: return std::nullopt;
    }
    if (args.size() < count || !hit_amount(args[amount])) return std::nullopt;
    for (std::size_t i = 0; i < count; ++i)
        if (args[i].empty()) return std::nullopt;

    SpellHitLine line;
    line.kind = kind;
    line.amount = args[amount];
    const std::string& n = line.amount;
    switch (eqstr) {
    case 9073: line.abbreviated = args[0] + " -> " + args[1] + ": " + n + " damage"; break;
    case 434: line.abbreviated = args[0] + ": " + n + " non-melee"; break;
    case 12481: line.abbreviated = "You: " + n + " non-melee"; break;
    case 428: line.abbreviated = "You: " + n + " critical blast"; break;
    case 1040: line.abbreviated = args[0] + ": " + n + " critical blast"; break;
    case 9072: line.abbreviated = args[0] + ": " + n + " DoT (" + args[2] + ")"; break;
    case 12954: line.abbreviated = "You: " + n + " DoT"; break;
    case 13327:
        // Servers differ in the last two substitutions' ordering. Amount and victim are unambiguous; do not
        // invent which remaining argument names the caster or spell.
        line.abbreviated = args[0] + ": " + n + " DoT"; break;
    case 9068: line.abbreviated = "You -> " + args[0] + ": " + n + " heal"; break;
    case 419: line.abbreviated = args[0] + " -> You: " + n + " heal"; break;
    case 427: line.abbreviated = "You: " + n + " critical heal"; break;
    case 1039: line.abbreviated = args[0] + ": " + n + " critical heal"; break;
    case 12976: line.abbreviated = "You: " + n + " HoT (" + args[1] + ")"; break;
    case 12997: line.abbreviated = "You -> " + args[0] + ": " + n + " HoT (" + args[2] + ")"; break;
    case 12998: line.abbreviated = args[0] + " -> You: " + n + " HoT (" + args[2] + ")"; break;
    default: break;
    }
    return line;
}

std::optional<SpellHitLine> spell_text_hit_report(std::string_view text, std::uint32_t color) {
    // Some servers send complete combat sentences through GENERIC_STRING (6688, "%1") or OP_SpecialMesg. Stock
    // EQEmu sends its hit and heal reports as the typed string ids spell_message_hit_report handles; its one
    // plain-text case is Mob::HealDamage's caster-less "You have been healed for %d points of damage."
    // (Client::Message, Chat::NonMelee). These have no typed amount: require a combat type and the entire known
    // English grammar, including its terminator, before shortening. Never search arbitrary player chat for a
    // number or infer a tick from a spell.
    bool damage = true, healing = true;
    SpellHitReport damage_kind = SpellHitReport::DirectDamage;
    switch (color) {
    case 283: case 302: break; // NonMelee / SpellCrit, including legacy healing reports
    case 374: case 375: case 402: case 403: case 404: damage = false; break;
    case 376: case 377: case 399: case 400: case 401:
        healing = false;
        damage_kind = SpellHitReport::DamageOverTime;
        break;
    case 379: case 397: case 398: healing = false; break;
    default: return std::nullopt;
    }
    for (char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7f) return std::nullopt;
    }

    std::string_view spell;
    if (text.ends_with(')')) {
        const auto suffix = text.rfind(". (");
        if (suffix == std::string_view::npos) return std::nullopt;
        spell = text.substr(suffix + 3, text.size() - suffix - 4);
        if (spell.empty() || spell.front() == ' ' || spell.back() == ' ') return std::nullopt;
        int depth = 0;
        for (char c : spell) {
            if (c == '(') ++depth;
            else if (c == ')' && --depth < 0) return std::nullopt;
        }
        if (depth != 0) return std::nullopt;
        text = text.substr(0, suffix + 1);
    }
    std::string_view units;
    bool heal = false;
    for (auto candidate : {" points of non-melee damage.", " point of non-melee damage."}) {
        if (text.ends_with(candidate)) { units = candidate; break; }
    }
    if (units.empty()) {
        for (auto candidate : {" points of damage.", " point of damage."}) {
            if (text.ends_with(candidate)) { units = candidate; heal = true; break; }
        }
    }
    if (units.empty() || (heal ? !healing : !damage)) return std::nullopt;
    const auto report = text.substr(0, text.size() - units.size());
    const auto for_at = report.rfind(" for ");
    if (for_at == std::string_view::npos) return std::nullopt;
    const auto amount = report.substr(for_at + 5);
    if (!hit_amount(amount)) return std::nullopt;

    const auto actors = report.substr(0, for_at);
    if (heal && actors == "You have been healed") {
        // Stock EQEmu's caster-less heal (zone/attack.cpp Mob::HealDamage). It is filtered as a heal over time
        // but names no healer or spell and cannot be told from a direct heal, so like YOU_HEALED (419) it
        // follows Healing, abbreviated as a heal on the player.
        SpellHitLine line;
        line.kind = SpellHitReport::Healing;
        line.amount = amount;
        line.abbreviated = "You: " + line.amount + " heal";
        if (!spell.empty()) line.abbreviated += " (" + std::string(spell) + ")";
        return line;
    }
    const bool has = actors.find(" has healed ") != std::string_view::npos;
    const bool have = actors.find(" have healed ") != std::string_view::npos;
    if (heal ? actors.find(" hit ") != std::string_view::npos : has || have) return std::nullopt;
    std::string_view verb = " hit ";
    if (heal) {
        if (has && have) return std::nullopt;
        verb = has ? " has healed " : " have healed ";
    }
    const auto verb_at = actors.find(verb);
    if (verb_at == std::string_view::npos || actors.find(verb, verb_at + verb.size()) != std::string_view::npos)
        return std::nullopt;
    const auto caster = actors.substr(0, verb_at), target = actors.substr(verb_at + verb.size());
    // A name may hold spaces, apostrophes and backticks (NPC names such as "a gnoll`s pup"), but never a comma,
    // a double quote or a colon, which mark a speaker's prefix ("Somebody says, ...", "Bob tells you: ...").
    auto name = [](std::string_view value) {
        if (value.empty() || value.front() == ' ' || value.back() == ' ') return false;
        if (value.find_first_of(",\":") != std::string_view::npos) return false;
        for (auto speech : {" says", " tells", " shouts", " auctions", " whispers"})
            if (value.find(speech) != std::string_view::npos) return false;
        return true;
    };
    if (!name(caster) || !name(target)) return std::nullopt;

    SpellHitLine line;
    // Generic heals carry no periodic/direct discriminator, even when the server used a HoT filter internally.
    line.kind = heal ? SpellHitReport::Healing : damage_kind;
    line.amount = amount;
    const char* effect = heal ? " heal" : damage_kind == SpellHitReport::DamageOverTime ? " DoT" : " damage";
    line.abbreviated = std::string(caster) + " -> " + std::string(target) + ": " + line.amount + effect;
    if (!spell.empty()) line.abbreviated += " (" + std::string(spell) + ")";
    return line;
}

std::optional<SpellHitLine> spell_damage_hit_report(const WorldState& world, const SpellData& spell,
                                                  std::uint32_t target, std::int32_t damage) {
    if (target != world.player_id || !world.spawns.contains(target) || damage == 0) return std::nullopt;
    SpellHitLine line;
    line.kind = damage < 0 ? SpellHitReport::Healing : SpellHitReport::DirectDamage;
    const auto signed_amount = static_cast<std::int64_t>(damage);
    line.amount = std::to_string(signed_amount < 0 ? -signed_amount : signed_amount);
    line.abbreviated = "You: " + line.amount + (damage < 0 ? " heal" : " damage");
    if (!spell.name.empty()) line.abbreviated += " (" + spell.name + ")";
    return line;
}

std::string report_spell_hit_line(const SpellHitLine& line, std::string_view normal, std::int32_t mode) {
    if (mode == 1) return line.abbreviated;
    if (mode == 2) return line.amount;
    return std::string(normal);
}

bool buff_is_permanent(std::int32_t tics, const SpellData* spell) {
    return tics < 0 || (spell && spell->duration_formula == 50);
}

std::uint64_t buff_ms_left(std::int32_t tics, std::int64_t received_ms, std::int64_t now_ms) {
    std::int64_t left = static_cast<std::int64_t>(tics) * kBuffTickMs - (now_ms - received_ms);
    return static_cast<std::uint64_t>(std::max<std::int64_t>(left, 0));
}

std::int32_t buff_tics_left(std::int32_t tics, std::int64_t received_ms, std::int64_t now_ms) {
    if (tics < 0) return tics; // permanent: the server's count, as sent
    const std::int64_t elapsed = now_ms > received_ms ? (now_ms - received_ms) / kBuffTickMs : 0;
    return static_cast<std::int32_t>(std::max<std::int64_t>(0, tics - elapsed));
}

bool SpellState::gem_castable(int gem, std::uint32_t now) const {
    if (gem < 0 || gem > kSpellGems) return false;
    return mechanics::gems::gem_castable(now, gems[static_cast<std::size_t>(gem)].ready, recovery_ready);
}

bool SpellState::gem_timers_done(int gem, std::uint32_t now) const {
    if (gem < 0 || gem > kSpellGems) return false;
    return !(now < gems[static_cast<std::size_t>(gem)].ready) && !(now < recovery_ready);
}

bool buff_countdown_held(const SpellData* spell, bool timers_on_hold, bool zone_suspends_buffs) {
    if (!spell || !spell->suspendable) return false;
    return zone_suspends_buffs || (timers_on_hold && !spell->has_effect(kSpaHoldBuffs));
}

std::uint64_t buff_ms_left(const BuffSlot& b, const SpellData* spell, bool timers_on_hold, bool zone_suspends_buffs,
                           std::int64_t now_ms) {
    if (buff_countdown_held(spell, timers_on_hold, zone_suspends_buffs))
        return static_cast<std::uint64_t>(std::max<std::int32_t>(b.tics, 0)) * kBuffTickMs;
    return buff_ms_left(b.tics, b.received_ms, now_ms);
}

bool scribe_countdown(Scribing& scribing, std::uint32_t& last, std::uint32_t now) {
    if (now - last > 1) {
        scribing.ticks_left -= 3;
        last = now;
    }
    if (scribing.ticks_left >= 1) return false;
    scribing.ticks_left = 0;
    last = 0;
    return true;
}

std::string format_duration(std::uint64_t ms) {
    std::uint64_t s = (ms + 999) / 1000;
    if (s < 60) return std::to_string(s) + "s";
    if (s >= 100 * 3600) return std::to_string(s / 86400) + "d";
    std::uint64_t h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    auto two = [](std::uint64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
    if (h) return std::to_string(h) + ":" + two(m) + ":" + two(sec);
    return std::to_string(m) + ":" + two(sec);
}

} // namespace hailstone::game
