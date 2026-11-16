#include "game/server_ext_chain.h"

#include <algorithm>

#include "game/spells.h"

namespace hailstone::game {

namespace {

// The chain's skill rules: `ctx` is the chain's rules_target_, the member rules skill_rules() picked last.
const SkillRules* target(const void* ctx) { return *static_cast<const SkillRules* const*>(ctx); }

} // namespace

ExtensionChain::ExtensionChain() {
    rules_.ctx = &rules_target_;
    rules_.gate = [](const void* ctx, std::int32_t skill, std::uint32_t mask,
                     std::int32_t profile_skill) -> std::optional<bool> {
        const SkillRules* r = target(ctx);
        return r && r->gate ? r->gate(r->ctx, skill, mask, profile_skill) : std::nullopt;
    };
    rules_.spoof_class = [](const void* ctx, std::int32_t skill, std::uint32_t mask) -> std::uint32_t {
        const SkillRules* r = target(ctx);
        return r && r->spoof_class ? r->spoof_class(r->ctx, skill, mask) : 0;
    };
    rules_.tracking_allowed = [](const void* ctx, std::uint8_t class_id, std::uint32_t mask, std::uint32_t skill) {
        const SkillRules* r = target(ctx);
        if (r && r->tracking_allowed) return r->tracking_allowed(r->ctx, class_id, mask, skill);
        return game::tracking_allowed()(class_id, mask, skill);
    };
}

int ExtensionChain::add(std::shared_ptr<ServerExtension> ext, ActiveFn active) {
    Member m;
    m.id = next_id_++;
    m.ext = std::move(ext);
    m.active = std::move(active);
    members_.push_back(std::move(m));
    return members_.back().id;
}

void ExtensionChain::remove(int id) {
    for (Member& m : members_) {
        if (m.id != id || !m.alive) continue;
        m.alive = false;
        if (rules_owner_ == m.ext.get()) {
            rules_target_ = nullptr;
            rules_owner_ = nullptr;
        }
    }
    if (depth_) {
        dirty_ = true;
        return;
    }
    std::erase_if(members_, [](const Member& m) { return !m.alive; });
}

bool ExtensionChain::empty() const {
    return std::none_of(members_.begin(), members_.end(), [](const Member& m) { return m.alive; });
}

template <typename F> bool ExtensionChain::each(const net::GameConnection& c, F&& fn) const {
    ++depth_;
    bool stopped = false;
    // By index: a member added meanwhile waits for the next call.
    for (std::size_t i = 0, n = members_.size(); i < n && !stopped; ++i) {
        const Member& m = members_[i];
        if (!m.alive || (m.active && !m.active(c))) continue;
        stopped = fn(*m.ext);
    }
    if (--depth_ == 0 && dirty_) {
        auto& self = const_cast<ExtensionChain&>(*this);
        std::erase_if(self.members_, [](const Member& m) { return !m.alive; });
        dirty_ = false;
    }
    return stopped;
}

// ---------------------------------------------------------------- the connection

bool ExtensionChain::retry_zone_entry(net::GameConnection& c) {
    bool any = false;
    each(c, [&](ServerExtension& e) { return any = e.retry_zone_entry(c) || any, false; });
    return any;
}

bool ExtensionChain::handler_command(net::GameConnection& c, std::string_view handler, const std::string& args) {
    return each(c, [&](ServerExtension& e) { return e.handler_command(c, handler, args); });
}

std::optional<std::vector<std::string>> ExtensionChain::who_lines(const net::GameConnection& c,
                                                                  const net::WhoResponse& r) const {
    std::optional<std::vector<std::string>> out;
    each(c, [&](const ServerExtension& e) { return (out = e.who_lines(c, r)).has_value(); });
    return out;
}

// ---------------------------------------------------------------- classes

std::uint8_t ExtensionChain::cast_class(const net::GameConnection& c) const {
    std::uint8_t out = 0;
    each(c, [&](const ServerExtension& e) { return (out = e.cast_class(c)) != 0; });
    return out;
}

std::optional<bool> ExtensionChain::is_spellcaster(const net::GameConnection& c) const {
    std::optional<bool> out;
    each(c, [&](const ServerExtension& e) { return (out = e.is_spellcaster(c)).has_value(); });
    return out;
}

std::optional<std::uint8_t> ExtensionChain::spell_level_needed(const net::GameConnection& c, const SpellData& s,
                                                               std::uint32_t class_id) const {
    std::optional<std::uint8_t> out;
    each(c, [&](const ServerExtension& e) { return (out = e.spell_level_needed(c, s, class_id)).has_value(); });
    return out;
}

bool ExtensionChain::song_blocked(const net::GameConnection& c, std::uint8_t raw_bard_level) const {
    return each(c, [&](const ServerExtension& e) { return e.song_blocked(c, raw_bard_level); });
}

bool ExtensionChain::own_begin_cast_line(const net::GameConnection& c, const SpellData& s) const {
    return each(c, [&](const ServerExtension& e) { return e.own_begin_cast_line(c, s); });
}

std::optional<bool> ExtensionChain::can_use_gem(const net::GameConnection& c, int gem) const {
    std::optional<bool> out;
    each(c, [&](const ServerExtension& e) { return (out = e.can_use_gem(c, gem)).has_value(); });
    return out;
}

std::int32_t ExtensionChain::combat_ability_timer(const net::GameConnection& c, const SpellData& s) const {
    const std::int32_t stock = ServerExtension::combat_ability_timer(c, s);
    std::int32_t out = stock;
    each(c, [&](const ServerExtension& e) { return (out = e.combat_ability_timer(c, s)) != stock; });
    return out;
}

std::optional<std::uint32_t> ExtensionChain::discipline_reuse_timer(const net::GameConnection& c,
                                                                    std::int32_t id) const {
    std::optional<std::uint32_t> out;
    each(c, [&](const ServerExtension& e) { return (out = e.discipline_reuse_timer(c, id)).has_value(); });
    return out;
}

std::optional<bool> ExtensionChain::before_combat_ability(net::GameConnection& c, const SpellData& s) {
    std::optional<bool> out;
    each(c, [&](ServerExtension& e) { return (out = e.before_combat_ability(c, s)).has_value(); });
    return out;
}

std::optional<bool> ExtensionChain::before_use_skill(net::GameConnection& c, std::uint32_t skill) {
    std::optional<bool> out;
    each(c, [&](ServerExtension& e) { return (out = e.before_use_skill(c, skill)).has_value(); });
    return out;
}

void ExtensionChain::after_combat_ability(net::GameConnection& c, const SpellData& s, bool used) {
    each(c, [&](ServerExtension& e) { return e.after_combat_ability(c, s, used), false; });
}

std::optional<std::string> ExtensionChain::combat_effect_label(const net::GameConnection& c) const {
    std::optional<std::string> out;
    each(c, [&](const ServerExtension& e) { return (out = e.combat_effect_label(c)).has_value(); });
    return out;
}

std::optional<MapSpawnMark> ExtensionChain::map_spawn(const net::GameConnection& c, const Spawn& s,
                                                      const MapSpawnMark& defaults) const {
    std::optional<MapSpawnMark> out;
    each(c, [&](const ServerExtension& e) { return (out = e.map_spawn(c, s, defaults)).has_value(); });
    return out;
}

bool ExtensionChain::reuse_focus_dropped(const net::GameConnection& c, const SpellData& s) const {
    return each(c, [&](const ServerExtension& e) { return e.reuse_focus_dropped(c, s); });
}

bool ExtensionChain::can_use_all_items(const net::GameConnection& c) const {
    return each(c, [&](const ServerExtension& e) { return e.can_use_all_items(c); });
}

bool ExtensionChain::wide_item_links(const net::GameConnection& c) const {
    return each(c, [&](const ServerExtension& e) { return e.wide_item_links(c); });
}

std::uint32_t ExtensionChain::guild_member_classes(const net::GameConnection& c, std::uint32_t class_value) const {
    std::uint32_t out = 0;
    each(c, [&](const ServerExtension& e) { return (out = e.guild_member_classes(c, class_value)) != 0; });
    return out;
}

std::uint8_t ExtensionChain::hide_check_class(const net::GameConnection& c, std::uint8_t class_id) const {
    std::uint8_t out = class_id;
    each(c, [&](const ServerExtension& e) { return (out = e.hide_check_class(c, class_id)) != class_id; });
    return out;
}

bool ExtensionChain::aa_any_level(const net::GameConnection& c) const {
    return each(c, [&](const ServerExtension& e) { return e.aa_any_level(c); });
}

bool ExtensionChain::aa_train_all(const net::GameConnection& c) const {
    return each(c, [&](const ServerExtension& e) { return e.aa_train_all(c); });
}

bool ExtensionChain::augment_classes(const net::GameConnection& c) const {
    return each(c, [&](const ServerExtension& e) { return e.augment_classes(c); });
}

bool ExtensionChain::research_classes(const net::GameConnection& c) const {
    return each(c, [&](const ServerExtension& e) { return e.research_classes(c); });
}

bool ExtensionChain::focus_classes(const net::GameConnection& c) const {
    return each(c, [&](const ServerExtension& e) { return e.focus_classes(c); });
}

bool ExtensionChain::backstab_either_hand(const net::GameConnection& c) const {
    return each(c, [&](const ServerExtension& e) { return e.backstab_either_hand(c); });
}

bool ExtensionChain::solo_adventure(const net::GameConnection& c) const {
    return each(c, [&](const ServerExtension& e) { return e.solo_adventure(c); });
}

std::vector<std::uint8_t> ExtensionChain::max_vitals_classes(const net::GameConnection& c,
                                                             std::uint8_t class_id) const {
    const std::vector<std::uint8_t> stock{class_id};
    std::vector<std::uint8_t> out = stock;
    each(c, [&](const ServerExtension& e) { return (out = e.max_vitals_classes(c, class_id)) != stock; });
    return out;
}

CharacterVitals ExtensionChain::character_vitals(const net::GameConnection& c) const {
    CharacterVitals out;
    each(c, [&](const ServerExtension& e) {
        CharacterVitals v = e.character_vitals(c);
        for (auto field : {&CharacterVitals::max_hp, &CharacterVitals::cur_hp, &CharacterVitals::max_mana,
                           &CharacterVitals::cur_mana, &CharacterVitals::max_endurance, &CharacterVitals::cur_endurance})
            if (!(out.*field)) out.*field = v.*field;
        return false;
    });
    return out;
}

// ---------------------------------------------------------------- items, skills, displays

bool ExtensionChain::item_right_click(net::GameConnection& c, const net::ItemSlot& slot) {
    return each(c, [&](ServerExtension& e) { return e.item_right_click(c, slot); });
}

const SkillRules* ExtensionChain::skill_rules(const net::GameConnection& c) const {
    rules_target_ = nullptr;
    rules_owner_ = nullptr;
    each(c, [&](const ServerExtension& e) {
        rules_target_ = e.skill_rules(c);
        if (rules_target_) rules_owner_ = &e;
        return rules_target_ != nullptr;
    });
    return rules_target_ ? &rules_ : nullptr;
}

LevelNeededLine ExtensionChain::level_needed_line(const net::GameConnection& c) const {
    LevelNeededLine out;
    each(c, [&](const ServerExtension& e) { return static_cast<bool>(out = e.level_needed_line(c)); });
    return out;
}

std::optional<std::string> ExtensionChain::merchant_level(const net::GameConnection& c, const net::Item& item) const {
    std::optional<std::string> out;
    each(c, [&](const ServerExtension& e) { return (out = e.merchant_level(c, item)).has_value(); });
    return out;
}

void ExtensionChain::label_text(const net::GameConnection& c, int id, std::string& text) const {
    each(c, [&](const ServerExtension& e) { return e.label_text(c, id, text), false; });
}

void ExtensionChain::nameplate_text(const net::GameConnection& c, const Spawn& spawn, std::string& text) const {
    each(c, [&](const ServerExtension& e) { return e.nameplate_text(c, spawn, text), false; });
}

std::optional<NameplatePresentation> ExtensionChain::nameplate_style(const net::GameConnection& c, const Spawn& spawn,
                                                                   const NameplateContext& context) const {
    std::optional<NameplatePresentation> out;
    each(c, [&](const ServerExtension& e) { return (out = e.nameplate_style(c, spawn, context)).has_value(); });
    return out;
}

bool ExtensionChain::hide_xtarget_slot(const net::GameConnection& c, int slot) const {
    return each(c, [&](const ServerExtension& e) { return e.hide_xtarget_slot(c, slot); });
}

} // namespace hailstone::game
