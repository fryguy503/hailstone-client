#pragma once

// The appearance data files the original client reads at start-up:
// the colour tables (Resources/npct.ini,
// itmt.ini), the hero's forge tables (Resources/NewArmorTagData.txt,
// NewArmorAdjData.txt), Resources/PlayerCustomization.txt and
// Resources/ActorTagInfo.txt.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "game/actor_look.h"

namespace hailstone::game {

// ColorTable_LoadFromIni @0x574b60: [Data] Number, Value1..ValueN (default
// 0xffffff), each stored | 0xff000000. ColorTable_GetEntry @0x574b40 returns
// entry `index` when index < Number (signed; npc_tint_entry gives the index).
struct ColorTable {
    static constexpr std::int64_t kMaxEntries = 65536; // a client bound on Number (appearance_data.cpp)
    std::vector<std::uint32_t> values;
    static ColorTable parse(std::string_view ini);
    const std::uint32_t* entry(std::int32_t index) const {
        if (index < 0 || static_cast<std::size_t>(index) >= values.size()) return nullptr;
        return &values[static_cast<std::size_t>(index)];
    }
};

// NewArmor_LoadTagData @0x573c20 and NewArmor_LoadAdjAndTagData @0x5741b0.
class NewArmorTables {
public:
    // NewArmorTagData.txt: tab separated "ID Tag IT IT-pair IgnoreAdjustment
    // ShowHair HideMesh"; '#' lines skipped. The tag is a model code, or (at
    // most two letters) '*' (the race table's codes with flag 0x20), 'L'
    // (0x20 without 8) or 'N' (0x28), then 'M' / 'F' for one gender. Every
    // code gets its own record and a later line replaces an earlier one
    // (0x574560 removes the old record before 0x574980 adds).
    void parse_tags(std::string_view text);
    // NewArmorAdjData.txt: "code, part, sx, sy, sz, tx, ty, tz, rx, ry, rz[,
    // variant]" -> RotationYawPitchRoll(rx°, ry°, rz°), rows 0..2 scaled,
    // row 3 the translation (stored column-major for p' = M p).
    void parse_adjustments(std::string_view text);

    std::optional<NewArmorTag> find_tag(std::uint32_t id, std::string_view code) const;
    const NewArmorAdjustment* find_adjustment(std::int32_t part, std::int32_t variant, std::string_view code) const;
    std::size_t tag_count() const { return tags_.size(); }
    std::size_t adjustment_count() const { return adjustments_.size(); }

private:
    std::map<std::pair<std::uint32_t, std::string>, NewArmorTag> tags_;
    std::map<std::tuple<std::int32_t, std::int32_t, std::string>, NewArmorAdjustment> adjustments_;
};

// CPlayerCustomizationManager (Resources/PlayerCustomization.txt, '^'
// separated, keyed {race, PARENT_ID (heritage), SEX}).
struct CustomizationEntry {
    std::string name;
    std::uint32_t base_color = 0; // | 0xff000000
    std::vector<std::int32_t> classes;
    std::vector<std::uint32_t> colors; // hair / beard colours, | 0xff000000
    std::int32_t faces = 0, hair_styles = 0, eyes = 0, beards = 0, tattoos = 0, facial_attachments = 0;
};

class PlayerCustomization {
public:
    void parse(std::string_view text);
    const CustomizationEntry* find(std::int32_t race, std::int32_t heritage, std::int32_t gender) const;
    // GetHairColor @0x8ceb30: COLOR_LIST[index], or 0 (untinted) without an
    // entry or past the list.
    std::uint32_t hair_color(std::int32_t race, std::int32_t heritage, std::int32_t gender, std::int32_t index) const;
    // GetBaseColor @0x8ceaa0: BASE_COLOR, or 0 without an entry.
    std::uint32_t base_color(std::int32_t race, std::int32_t heritage, std::int32_t gender) const;
    // CPlayerCustomizationManager_FindHeritages @0x8ced00 for {race, class, gender}: the PARENT_IDs of the race's
    // and gender's entries whose CLASS_LIST holds the class (the original's list follows its hash table; here by id).
    std::vector<std::int32_t> heritages(std::int32_t race, std::int32_t class_id, std::int32_t gender) const;
    std::size_t size() const { return entries_.size(); }

private:
    std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, CustomizationEntry> entries_;
};

// Resources\ActorTagInfo.txt, which the original reads at start-up for CDisplay::IsShield: one record a line,
// '^' separated, field 1 the item model number (IT<n>) and field 2 its kind, 1 for a shield. The other fields
// (docs/formats/characters.md "ActorTagInfo.txt") are not used. Once the file has loaded, a model it does not list
// is not a shield.
class ActorTagTable {
public:
    void parse(std::string_view text);
    // Whether the file gave any record. Without one the original's fixed list applies (game::is_shield_model).
    // Hailstone, a deliberate difference: the original counts the file as loaded once it opens (@0x7f4130), so an
    // empty or garbled file would hold every model in the hand; here such a file falls back to the fixed list.
    bool loaded() const { return records_ != 0; }
    bool is_shield(std::uint32_t item_model) const { return shields_.contains(item_model); }
    std::size_t record_count() const { return records_; }
    std::size_t shield_count() const { return shields_.size(); }

private:
    std::size_t records_ = 0;
    std::unordered_set<std::uint32_t> shields_;
};

// The tables together (CharacterLibrary keeps one, loaded from the game's
// Resources directory).
struct AppearanceData {
    ColorTable npc_tints;  // npct.ini
    ColorTable item_tints; // itmt.ini (absent in NMS: empty)
    NewArmorTables new_armor;
    PlayerCustomization customization;
    ActorTagTable actor_tags; // ActorTagInfo.txt (absent: not loaded)
    // [Defaults] ActorNewArmorFilter (default 24): bit 0x10 NPCs, 8 players.
    std::uint32_t new_armor_filter = 24;
};

// PlayerClient_ApplyNewArmorFilter @0x595de0 for a spawn that is not the
// local player, a group or raid member: whether hero's forge is hidden.
inline bool new_armor_filtered(std::uint32_t filter, bool npc) { return (filter & (npc ? 0x10u : 0x08u)) == 0; }

} // namespace hailstone::game
