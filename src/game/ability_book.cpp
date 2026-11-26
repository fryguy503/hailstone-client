#include "game/ability_book.h"

#include <algorithm>
#include <charconv>
#include <format>

#include "game/mechanics/abilities.h"

namespace hailstone::game {

namespace {

std::optional<std::int32_t> to_int(std::string_view s) {
    std::int32_t v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
    return v;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

} // namespace

std::string ability_key(AbilityKind kind, std::int32_t id) {
    switch (kind) {
    case AbilityKind::AltAbility: return std::format("aa:{}", id);
    case AbilityKind::Discipline: return std::format("disc:{}", id);
    case AbilityKind::Skill: return std::format("skill:{}", id);
    }
    return {};
}

std::string discipline_key(std::int32_t spell_group, std::uint32_t spell_id) {
    return spell_group > 0 ? ability_key(AbilityKind::Discipline, spell_group) : std::format("spell:{}", spell_id);
}

std::optional<AbilityKeyParts> parse_ability_key(std::string_view key) {
    const std::size_t colon = key.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    const std::string_view kind = key.substr(0, colon);
    const auto id = to_int(key.substr(colon + 1));
    if (!id) return std::nullopt;
    AbilityKeyParts p;
    p.id = *id;
    if (kind == "aa") p.kind = AbilityKind::AltAbility;
    else if (kind == "disc") p.kind = AbilityKind::Discipline;
    else if (kind == "spell") p.kind = AbilityKind::Discipline, p.spell_id = true;
    else if (kind == "skill") p.kind = AbilityKind::Skill;
    else return std::nullopt;
    return p;
}

std::string_view skill_description(std::uint32_t skill) {
    namespace s = mechanics::skill;
    switch (skill) {
    case s::kApplyPoison: return "Applies the poison on your cursor to your weapon.";
    case s::kBackstab: return "Strikes from behind with a piercing weapon in your primary hand for heavy damage.";
    case s::kBindWound: return "Bandages your target's wounds, or your own, with bandages from your inventory. "
                               "Interrupted by moving.";
    case s::kBash: return "Slams your shield, or your two-handed weapon with enough skill, into your target. "
                          "Can interrupt spell casting.";
    case s::kDisarm: return "Attempts to knock the weapon from your target's hand.";
    case s::kDisarmTraps: return "Attempts to disarm a trap in front of you.";
    case s::kDragonPunch: return "A powerful strike with your fists (Tail Rake for Iksar).";
    case s::kEagleStrike: return "A quick, piercing strike with your hands.";
    case s::kFeignDeath: return "Drops to the ground feigning death, so that enemies may lose interest in you.";
    case s::kFlyingKick: return "A leaping kick that deals heavy damage.";
    case s::kForage: return "Searches the area for food, drink and other useful items.";
    case s::kHide: return "Hides you from sight while you stand still.";
    case s::kKick: return "Kicks your target for damage.";
    case s::kMend: return "Heals a portion of your own health. A failure can hurt you.";
    case s::kPickLock: return "Attempts to open a locked door or container with lock picks.";
    case s::kRoundKick: return "A spinning kick that deals moderate damage.";
    case s::kSafeFall: return "Lessens the damage you take from falling.";
    case s::kSenseHeading: return "Gives you a sense of which direction you are facing.";
    case s::kSneak: return "Moves quietly so that you are harder to notice.";
    case s::kPickPockets: return "Attempts to steal from your target without being noticed.";
    case s::kTigerClaw: return "A fast strike with your fingers.";
    case s::kTracking: return "Shows the creatures and players around you and the way to them.";
    case s::kFishing: return "Casts a line with the fishing pole in your primary hand.";
    case s::kSenseTraps: return "Searches nearby for traps.";
    case s::kBegging: return "Asks your target for money. Some take offence.";
    case s::kIntimidation: return "Frightens your target so that it may flee.";
    case s::kTaunt: return "Goads your target into attacking you instead of others.";
    case s::kFrenzy: return "A wild flurry of strikes against your target.";
    case s::kRemoveTraps: return "Attempts to remove a trap.";
    case s::kSlam: return "Slams your body into your target; no shield needed. Can interrupt spell casting.";
    default: return {};
    }
}

std::string format_autoskill_rules(const std::vector<AutoskillRule>& rules) {
    std::string out;
    for (const auto& r : rules) {
        if (!out.empty()) out += ',';
        out += r.key;
        const bool more = r.target_hp_above || r.self_hp_below || r.when != AutoskillWhen::Fighting || r.keep_buff;
        if (more || r.min_endurance_pct || r.min_mana_pct || r.target_hp_below)
            out += std::format(";{};{};{}", r.min_endurance_pct, r.min_mana_pct, r.target_hp_below);
        if (more)
            out += std::format(";{};{};{};{}", r.target_hp_above, r.self_hp_below, static_cast<int>(r.when),
                               r.keep_buff ? 1 : 0);
    }
    return out;
}

std::vector<AutoskillRule> parse_autoskill_rules(std::string_view text) {
    std::vector<AutoskillRule> out;
    while (!text.empty()) {
        const std::size_t end = text.find(',');
        std::string_view item = trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (item.empty()) continue;
        AutoskillRule r;
        const std::size_t semi = item.find(';');
        r.key = std::string(item.substr(0, semi));
        if (!parse_ability_key(r.key)) continue;
        if (semi != std::string_view::npos) {
            std::string_view rest = item.substr(semi + 1);
            int when = 0, buff = 0;
            struct Field {
                int* v;
                int max;
            };
            const Field fields[] = {{&r.min_endurance_pct, 100}, {&r.min_mana_pct, 100}, {&r.target_hp_below, 100},
                                    {&r.target_hp_above, 100},   {&r.self_hp_below, 100}, {&when, 2},
                                    {&buff, 1}};
            for (const Field& f : fields) {
                const std::size_t e = rest.find(';');
                if (auto v = to_int(trim(rest.substr(0, e)))) *f.v = std::clamp(*v, 0, f.max);
                if (e == std::string_view::npos) break;
                rest = rest.substr(e + 1);
            }
            r.when = static_cast<AutoskillWhen>(when);
            r.keep_buff = buff != 0;
        }
        bool dup = false;
        for (const auto& o : out) dup = dup || o.key == r.key;
        if (!dup) out.push_back(std::move(r));
    }
    return out;
}

bool autoskill_free(const AutoskillWorld& w) {
    return w.in_game && !w.dead && !w.sitting && !w.feigned && !w.stunned && !w.casting && !w.invisible;
}

bool autoskill_can_act(const AutoskillWorld& w) {
    return autoskill_free(w) && w.auto_attack && w.target_npc && w.target_in_range;
}

bool autoskill_when_holds(AutoskillWhen when, bool beneficial, const AutoskillWorld& w) {
    if (!beneficial) when = AutoskillWhen::Fighting;
    switch (when) {
    case AutoskillWhen::Always: return autoskill_free(w);
    case AutoskillWhen::Resting: return autoskill_free(w) && !w.auto_attack && !w.in_combat;
    case AutoskillWhen::Fighting: break;
    }
    return autoskill_can_act(w);
}

std::optional<std::size_t> Autoskill::next(const std::vector<AutoskillRule>& rules,
                                           const AutoskillQuery& query, const AutoskillWorld& w) {
    if (!autoskill_free(w)) return std::nullopt;
    if (last_ms_ && w.now_ms - *last_ms_ < kGapMs) return std::nullopt;
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const auto& r = rules[i];
        auto t = tried_.find(r.key);
        if (t != tried_.end() && w.now_ms - t->second.at_ms < (kRetryMs << t->second.misses)) continue;
        const std::optional<AutoskillAbility> a = query(r.key);
        if (!a || !autoskill_when_holds(r.when, a->beneficial, w)) continue;
        if (r.min_endurance_pct && w.endurance_pct < r.min_endurance_pct) continue;
        if (r.min_mana_pct && w.mana_pct < r.min_mana_pct) continue;
        if (r.self_hp_below && w.hp_pct >= r.self_hp_below) continue;
        if ((r.target_hp_below || r.target_hp_above) && !w.target_npc) continue;
        if (r.target_hp_below && w.target_hp_pct >= r.target_hp_below) continue;
        if (r.target_hp_above && w.target_hp_pct <= r.target_hp_above) continue;
        if (!a->ready || (r.keep_buff && a->buff_on)) {
            if (t != tried_.end()) tried_.erase(t); // its timer runs or its buff is on: the last use went through
            continue;
        }
        return i;
    }
    return std::nullopt;
}

void Autoskill::used(const std::string& key, std::uint32_t now_ms) {
    last_ms_ = now_ms;
    auto [t, fresh] = tried_.try_emplace(key);
    if (!fresh) t->second.misses = std::min(t->second.misses + 1, kMaxBackoff); // ready again at once: it failed
    t->second.at_ms = now_ms;
}

void Autoskill::reset() {
    last_ms_.reset();
    tried_.clear();
}

bool move_autoskill_rule(std::vector<AutoskillRule>& rules, std::string_view key, std::int64_t delta) {
    auto it = std::ranges::find_if(rules, [&](const AutoskillRule& r) { return r.key == key; });
    if (it == rules.end()) return false;
    const auto from = static_cast<std::int64_t>(it - rules.begin());
    const auto last = static_cast<std::int64_t>(rules.size()) - 1;
    // The step held to the list's length first: from + a delta near 2^63 would overflow.
    const auto to = std::clamp<std::int64_t>(from + std::clamp<std::int64_t>(delta, -last, last), 0, last);
    if (to == from) return false;
    AutoskillRule r = std::move(*it);
    rules.erase(it);
    rules.insert(rules.begin() + to, std::move(r));
    return true;
}

} // namespace hailstone::game
