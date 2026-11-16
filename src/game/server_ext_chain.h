#pragma once

// Several server extensions as one (game/server_ext.h): the plugins of a server and the player's each add theirs,
// and the core asks the chain as it would ask one extension. Each member has an `active` check (a plugin that has
// not seen its server yet); only active members are asked, in the order they were added. docs/plugins.md.
//
// How the answers combine:
// - events (after_combat_ability): every active member, in order.
// - handler_command, item_right_click: in order until one takes it.
// - retry_zone_entry: every member is asked; true when any says so.
// - yes / no switches (song_blocked, own_begin_cast_line, can_use_all_items, wide_item_links, aa_any_level,
//   aa_train_all, augment_classes, research_classes, focus_classes, backstab_either_hand, solo_adventure,
//   hide_xtarget_slot): true when any says so.
// - answers (optional values, cast_class, guild_member_classes, level_needed_line, ...): the first member
//   with one. Where the hook returns the stock value rather than an optional (hide_check_class,
//   combat_ability_timer, max_vitals_classes), the first member whose answer differs from the stock one.
// - character_vitals: field by field, the first member with a value.
// - label_text: each member in turn may change the text.
// - skill_rules: the chain's own rules (a pointer that stays valid), which ask the first active member that has
//   rules when skill_rules() was last called.
//
// Single threaded. A member removed while the chain is running stops at once and is dropped when it returns.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game/server_ext.h"

namespace hailstone::game {

class ExtensionChain final : public ServerExtension {
public:
    using ActiveFn = std::function<bool(const net::GameConnection&)>;

    ExtensionChain();
    ExtensionChain(const ExtensionChain&) = delete;
    ExtensionChain& operator=(const ExtensionChain&) = delete;

    // Adds a member after the others; `active` empty = always active. Returns its id for remove().
    int add(std::shared_ptr<ServerExtension> ext, ActiveFn active = {});
    void remove(int id);
    bool empty() const;

    const char* name() const override { return "chain"; }

    std::optional<FollowProfile> follow_profile(const net::GameConnection& c) const override;
    std::optional<MountProfile> mount_profile(const net::GameConnection& c) const override;
    bool map_visible(const net::GameConnection& c, const Spawn& spawn, std::uint32_t purpose) const override;
    MapPolicy map_policy(const net::GameConnection& c) const override;
    std::uint64_t lifecycle_revision(const net::GameConnection& c) const override;
    bool merchant_right_click(net::GameConnection& c, std::uint32_t merchant, const net::ItemSlot& slot,
                              std::uint32_t item, std::uint32_t quantity) override;
    bool retry_zone_entry(net::GameConnection& c) override;
    bool handler_command(net::GameConnection& c, std::string_view handler, const std::string& args) override;
    std::optional<std::vector<std::string>> who_lines(const net::GameConnection& c,
                                                      const net::WhoResponse& r) const override;

    std::uint8_t cast_class(const net::GameConnection& c) const override;
    std::optional<bool> is_spellcaster(const net::GameConnection& c) const override;
    std::optional<std::uint8_t> spell_level_needed(const net::GameConnection& c, const SpellData& s,
                                                   std::uint32_t class_id) const override;
    bool song_blocked(const net::GameConnection& c, std::uint8_t raw_bard_level) const override;
    bool own_begin_cast_line(const net::GameConnection& c, const SpellData& s) const override;
    std::optional<bool> can_use_gem(const net::GameConnection& c, int gem) const override;
    std::int32_t combat_ability_timer(const net::GameConnection& c, const SpellData& s) const override;
    std::optional<std::uint32_t> discipline_reuse_timer(const net::GameConnection& c, std::int32_t id) const override;
    std::optional<bool> before_combat_ability(net::GameConnection& c, const SpellData& s) override;
    std::optional<bool> before_use_skill(net::GameConnection& c, std::uint32_t skill) override;
    void after_combat_ability(net::GameConnection& c, const SpellData& s, bool used) override;
    std::optional<std::string> combat_effect_label(const net::GameConnection& c) const override;
    bool reuse_focus_dropped(const net::GameConnection& c, const SpellData& s) const override;
    std::optional<MapSpawnMark> map_spawn(const net::GameConnection& c, const Spawn& s,
                                          const MapSpawnMark& defaults) const override;
    bool can_use_all_items(const net::GameConnection& c) const override;
    bool wide_item_links(const net::GameConnection& c) const override;
    std::uint32_t guild_member_classes(const net::GameConnection& c, std::uint32_t class_value) const override;
    std::uint8_t hide_check_class(const net::GameConnection& c, std::uint8_t class_id) const override;
    bool aa_any_level(const net::GameConnection& c) const override;
    bool aa_train_all(const net::GameConnection& c) const override;
    bool augment_classes(const net::GameConnection& c) const override;
    bool research_classes(const net::GameConnection& c) const override;
    bool focus_classes(const net::GameConnection& c) const override;
    bool backstab_either_hand(const net::GameConnection& c) const override;
    bool solo_adventure(const net::GameConnection& c) const override;
    std::vector<std::uint8_t> max_vitals_classes(const net::GameConnection& c, std::uint8_t class_id) const override;
    CharacterVitals character_vitals(const net::GameConnection& c) const override;
    bool item_right_click(net::GameConnection& c, const net::ItemSlot& slot) override;
    const SkillRules* skill_rules(const net::GameConnection& c) const override;
    LevelNeededLine level_needed_line(const net::GameConnection& c) const override;
    std::optional<std::string> merchant_level(const net::GameConnection& c, const net::Item& item) const override;
    std::optional<std::string> zone_display_name(const net::GameConnection&, std::uint32_t, std::uint32_t) const override;
    std::optional<std::string> item_flags(const net::GameConnection&, const net::Item&, std::string_view) const override;
    std::optional<std::string> item_description_append(const net::GameConnection&, const net::Item&) const override;
    std::uint64_t item_presentation_revision(const net::GameConnection&) const override;
    std::optional<std::string> loot_confirmation(const net::GameConnection&, const net::Item&,
        std::span<const std::uint32_t>, bool) const override;
    void label_text(const net::GameConnection& c, int id, std::string& text) const override;
    void nameplate_text(const net::GameConnection& c, const Spawn& spawn, std::string& text) const override;
    bool hide_xtarget_slot(const net::GameConnection& c, int slot) const override;

private:
    struct PolicyStamp {
        int member = 0;
        std::uint32_t flags = 0;
        std::uint64_t revision = 0;
        std::uint64_t lifecycle = 0;
        std::string name;
        bool operator==(const PolicyStamp&) const = default;
    };
    mutable std::vector<PolicyStamp> policy_stamps_;
    mutable std::uint64_t policy_revision_ = 0;
    mutable std::vector<std::pair<int, std::uint64_t>> lifecycle_stamps_;
    mutable std::uint64_t lifecycle_revision_ = 0;
    struct Member {
        int id = 0;
        std::shared_ptr<ServerExtension> ext;
        ActiveFn active;
        bool alive = true;
    };
    // Calls fn(ext) for each active member until fn returns true; returns whether one did.
    template <typename F> bool each(const net::GameConnection& c, F&& fn) const;

    std::vector<Member> members_;
    int next_id_ = 1;
    mutable int depth_ = 0;
    mutable bool dirty_ = false;
    // skill_rules(): the stable rules handed out, and the member rules they ask.
    SkillRules rules_{};
    mutable const SkillRules* rules_target_ = nullptr;
    mutable const ServerExtension* rules_owner_ = nullptr;
};

} // namespace hailstone::game
