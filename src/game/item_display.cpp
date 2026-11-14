#include "game/item_display.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <format>

#include "game/augments.h"
#include "game/eq_names.h"
#include "game/eq_strings.h"
#include "game/item_links.h"
#include "game/mechanics/character.h"
#include "game/mechanics/item_stats.h"
#include "game/mechanics/tables.h"
#include "game/spell_description.h"
#include "game/spells.h"

// Each section names the original's code it follows (CItemDisplayWnd_BuildItemInfo and the helpers it calls,
// read from the eqgame.exe disassembly where arguments were unclear).

namespace hailstone::game {

namespace m = mechanics;

namespace {

constexpr std::uint32_t kWhite = 0xffffffff;
constexpr std::uint32_t kRed = 0xffff0000;
constexpr std::uint32_t kGreen = 0xff00ff00;
constexpr std::uint32_t kPurple = 0xffa020f0;   // usable but for the prestige check
constexpr std::uint32_t kSpellName = 0xff8aa3ff; // the effect names in the description

std::string str(const ItemDisplayContext& ctx, std::uint32_t id) {
    return ctx.strings ? ctx.strings->format(id, {}) : std::string();
}

// The string table's text, or a plain label where the player's eqstr_us.txt lacks the id. The original always has
// its own table's; a live game folder's table dropped the stats live removed (13036..13044, 13315, 9354), and items
// on a RoF2 server still carry them.
std::string str_or(const ItemDisplayContext& ctx, std::uint32_t id, const char* fallback) {
    return ctx.strings && ctx.strings->has(id) ? ctx.strings->format(id, {}) : std::string(fallback);
}

std::string str_args(const ItemDisplayContext& ctx, std::uint32_t id, std::initializer_list<std::string> args) {
    std::vector<std::string> v(args);
    return ctx.strings ? ctx.strings->format(id, v) : std::string();
}

bool plain(const net::Item& item) { return item.item_class == 0; }

// The item and its children (augments) through a field, as the ItemBase stat getters with bIncludeAugs.
std::int32_t stat(const net::Item& item, std::int32_t (*field)(const net::Item&)) { return m::item_stat(item, field); }
// With the augments' part (CreateModString's `out`): only augments in sockets with the +0x1f1 flag count, so a stat
// an augment in an ordinary socket raises keeps the plain colour.
std::int32_t stat(const net::Item& item, std::int32_t (*field)(const net::Item&), std::int32_t& aug) {
    return m::item_stat(item, field, aug);
}

// MakeStmlColorTag(argb): <c "#RRGGBB">; the caller writes "<C " + tag-body + ">".
std::string color_tag(std::uint32_t argb) { return std::format("\"#{:06X}\"", argb & 0xffffff); }

// The stat grid as the window's helpers fill it.
struct Grid {
    ItemDisplay& d;
    int row = 1; // +0x2e4

    ItemDisplayCell& cell(int r, int c) { return d.grid[static_cast<std::size_t>(r - 1)][static_cast<std::size_t>(c - 1)]; }
    bool valid(int r) const { return r >= 1 && r <= ItemDisplay::kRows; }
    // 0x69b0d0 / the inline stat-label sets, then 0x69b160 (an int: green for a positive modifier, red for
    // a negative one) or 0x69b260 (text: red for a positive modifier, green for a negative one).
    void put(int c, const std::string& label, const std::string& value, std::int32_t mod = 0, bool text = false) {
        if (valid(row)) {
            ItemDisplayCell& x = cell(row, c);
            x.shown = true;
            x.stat = label;
            x.value = value;
            x.value_argb = mod == 0 ? kWhite : ((mod > 0) != text ? kGreen : kRed);
        }
        ++row;
    }
    void put_int(int c, const std::string& label, std::int32_t v, std::int32_t mod = 0) { put(c, label, std::to_string(v), mod); }
    // 0x69b3d0: "+n" in gold right of the value just written (row - 1).
    void heroic(int c, std::int32_t v) {
        int r = row - 1;
        if (!valid(r) || d.heroic_count >= 13) return;
        cell(r, c).heroic = "+" + std::to_string(v);
        ++d.heroic_count;
    }
};

std::int32_t f_ac(const net::Item& i) { return i.ac; }
std::int32_t f_hp(const net::Item& i) { return i.hp; }
std::int32_t f_mana(const net::Item& i) { return i.mana; }
std::int32_t f_end(const net::Item& i) { return static_cast<std::int32_t>(i.endurance); }
std::int32_t f_dmg(const net::Item& i) { return static_cast<std::int32_t>(i.damage); }
std::int32_t f_backstab(const net::Item& i) { return static_cast<std::int32_t>(i.backstab_dmg); }
std::int32_t f_str(const net::Item& i) { return i.astr; }
std::int32_t f_sta(const net::Item& i) { return i.asta; }
std::int32_t f_int(const net::Item& i) { return i.aint; }
std::int32_t f_wis(const net::Item& i) { return i.awis; }
std::int32_t f_agi(const net::Item& i) { return i.aagi; }
std::int32_t f_dex(const net::Item& i) { return i.adex; }
std::int32_t f_cha(const net::Item& i) { return i.acha; }
std::int32_t f_hstr(const net::Item& i) { return i.heroic_str; }
std::int32_t f_hsta(const net::Item& i) { return i.heroic_sta; }
std::int32_t f_hint(const net::Item& i) { return i.heroic_int; }
std::int32_t f_hwis(const net::Item& i) { return i.heroic_wis; }
std::int32_t f_hagi(const net::Item& i) { return i.heroic_agi; }
std::int32_t f_hdex(const net::Item& i) { return i.heroic_dex; }
std::int32_t f_hcha(const net::Item& i) { return i.heroic_cha; }
std::int32_t f_mr(const net::Item& i) { return i.mr; }
std::int32_t f_fr(const net::Item& i) { return i.fr; }
std::int32_t f_cr(const net::Item& i) { return i.cr; }
std::int32_t f_dr(const net::Item& i) { return i.dr; }
std::int32_t f_pr(const net::Item& i) { return i.pr; }
std::int32_t f_corr(const net::Item& i) { return i.sv_corruption; }
std::int32_t f_hmr(const net::Item& i) { return i.heroic_mr; }
std::int32_t f_hfr(const net::Item& i) { return i.heroic_fr; }
std::int32_t f_hcr(const net::Item& i) { return i.heroic_cr; }
std::int32_t f_hdr(const net::Item& i) { return i.heroic_dr; }
std::int32_t f_hpr(const net::Item& i) { return i.heroic_pr; }
std::int32_t f_hcorr(const net::Item& i) { return i.heroic_sv_corrup; }
std::int32_t f_attack(const net::Item& i) { return i.attack; }
std::int32_t f_regen(const net::Item& i) { return i.regen; }
std::int32_t f_mregen(const net::Item& i) { return i.mana_regen; }
std::int32_t f_eregen(const net::Item& i) { return i.end_regen; }
std::int32_t f_sshield(const net::Item& i) { return i.spell_shield; }
std::int32_t f_ce(const net::Item& i) { return i.combat_effects; }
std::int32_t f_shielding(const net::Item& i) { return i.shielding; }
std::int32_t f_ds(const net::Item& i) { return i.damage_shield; }
std::int32_t f_dot(const net::Item& i) { return i.dot_shield; }
std::int32_t f_dsmit(const net::Item& i) { return static_cast<std::int32_t>(i.ds_mitigation); }
std::int32_t f_avoid(const net::Item& i) { return i.avoidance; }
std::int32_t f_acc(const net::Item& i) { return i.accuracy; }
std::int32_t f_stun(const net::Item& i) { return i.stun_resist; }
std::int32_t f_strike(const net::Item& i) { return i.strike_through; }
std::int32_t f_heal(const net::Item& i) { return i.heal_amt; }
std::int32_t f_sdmg(const net::Item& i) { return i.spell_dmg; }
std::int32_t f_clair(const net::Item& i) { return i.clairvoyance; }

// ItemDefinition_IsWeapon: damage > 0 and not item types 27, 28, 43, 44 (arrows, ...).
bool is_weapon(const net::Item& item) {
    return item.damage > 0 && item.item_type != 27 && item.item_type != 28 && item.item_type != 43 && item.item_type != 44;
}

// CItemDisplayWnd_CreateItemFlagsString @0x69e280: the flags, ", " between them.
std::string item_flags(const net::Item& item, const ItemDisplayContext& ctx) {
    std::string s;
    auto add = [&](bool on, std::uint32_t id) {
        if (on) s += str(ctx, id) + ", ";
    };
    auto any_child = [&](auto&& self, const net::Item& it, auto pred) -> bool {
        if (pred(it)) return true;
        for (const auto& c : it.contents)
            if (self(self, c.item, pred)) return true;
        return false;
    };
    add(any_child(any_child, item, [](const net::Item& i) { return i.magic != 0; }), 0xc16);       // Magic
    add(item.is_lore(), 0xc17);                                                                       // Lore
    add(item.artifact != 0, 0xc19);                                                                   // Artifact
    add(item.summoned != 0, 0xc1a);                                                                   // Summoned
    add(any_child(any_child, item, [](const net::Item& i) { return i.no_trade(); }), 0xc1b);         // No Trade
    add(any_child(any_child, item, [](const net::Item& i) { return i.no_transfer != 0; }), 0x180b);  // No Storage
    add(item.attuneable != 0, 0x1717);                                                                // Attunable
    add(any_child(any_child, item, [](const net::Item& i) { return i.no_rent == 0; }), 0xc1c);       // Temporary
    add(item.no_pet != 0, 0x22ac);                                                                    // No Pet
    add(item.item_type == 54, 0xd1c);                                                                 // Augmentation
    add(item.quest_item != 0, 0x11d1);                                                                // Quest
    if (item.purity > 0) add(true, item.power > 0 ? 0x23de : 0x23dd);                                // Infused / Infusible
    add(any_child(any_child, item, [](const net::Item& i) { return i.heirloom != 0; }), 0x1d54);     // Heirloom
    add(m::item_prestige(item) > 0, 0x1552);                                                          // Glamour
    add(item.placeable_flags() != 0, 0x190d);                                                         // Placeable
    add(item.copied != 0, 0x1d9d);                                                                    // Copied
    add(item.collectible(), 0x1e08);                                                                  // Collectible
    add(item.no_destroy(), 0x1e0d);                                                                   // No Destroy
    add(any_child(any_child, item, [](const net::Item& i) { return i.no_zone != 0; }), 0x1e0f);      // No Zone
    add(any_child(any_child, item, [](const net::Item& i) { return i.no_ground != 0; }), 0x1e10);    // No Ground
    // CXStr::StripTrailing(' '), then (',').
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.back() == ',') s.pop_back();
    return s;
}

// A list split as the original splits it: the first `first` entries on one line, the rest on a second.
struct TwoLines {
    std::string a, b;
    int count = 0;
};

// The race abbreviations by race bit (the switch at 0x6a9937).
constexpr std::uint32_t kRaceAbbr[17] = {0xbea, 0xbeb, 0xbec, 0xbed, 0xbee, 0xbef, 0xbf0, 0xbf1, 0xbf2,
                                          0xbf3, 0xbf4, 0xbf5, 0xbf6, 0xbf7, 0xfcc, 0x131, 0xe18};
// The class abbreviations (0xb2bb98).
constexpr std::uint32_t kClassAbbr[16] = {3065, 3066, 3067, 3069, 3068, 3070, 3071, 3072,
                                           3073, 3074, 3075, 3076, 3077, 3078, 3079, 5810};
// The slot names by slot (0xa02238; 0x9ebaf8 maps the position to the slot, itself).
constexpr std::uint32_t kSlotNames[23] = {5776, 559, 560, 561, 559, 562, 563, 564, 565, 566, 566, 567,
                                           568, 569, 570, 571, 571, 572, 573, 574, 575, 9178, 576};
// The size names (0xb2bbd8): TINY .. GIANT.
constexpr std::uint32_t kSizeNames[5] = {3128, 3129, 3130, 3131, 3132};

std::string size_name(const ItemDisplayContext& ctx, std::uint32_t s) { return str(ctx, kSizeNames[s < 4 ? s : 4]); }

// ItemDisplay_BuildSpellEffectText @0x6a1c70: one line per item spell (click, combat, worn, focus, the bard focus),
// then the augments' (`recurse`). The spells with a description go to `described` for the description blocks.
std::string spell_effect_text(const net::Item& item, const ItemDisplayContext& ctx, bool recurse,
                              std::vector<const net::ItemEffect*>& described) {
    std::string out;
    // A charm whose stat scale is below 1 shows no effects.
    bool show = !(item.charm_file_id != 0 && static_cast<std::int32_t>(item.scaled_value) < 1);
    const SpellData* spell = nullptr;
    auto name_of = [&](const net::ItemEffect& e, const SpellData* s) { return e.name.empty() && s ? s->name : e.name; };
    auto remember = [&](const net::ItemEffect& e, const SpellData* s) {
        if (s && s->description_id != 0) described.push_back(&e);
    };
    if (item.item_type != 20 && show) {
        const auto& e = item.effects[net::kEffectClick];
        if (e.present() && ctx.spell && (spell = ctx.spell(e.effect))) {
            std::string how;
            switch (e.type) {
            case 1: case 3: case 5:
                how = e.cast_time() < 1 ? str(ctx, 0xbd7) : [&] {
                    char b[64];
                    std::snprintf(b, sizeof b, "(Casting Time: %2.1f)", static_cast<double>(e.cast_time()) * 0.001);
                    return std::string(b);
                }();
                break;
            case 4:
                how = e.cast_time() < 1 ? str(ctx, 0xbda) : [&] {
                    char b[64];
                    std::snprintf(b, sizeof b, "(Must Equip.  Casting Time: %2.1f)", static_cast<double>(e.cast_time()) * 0.001);
                    return std::string(b);
                }();
                break;
            default: how = str(ctx, 0xbdb); break;
            }
            out += str_args(ctx, 0xbdc, {name_of(e, spell), how}) + "<BR>";
            remember(e, spell);
            if (e.recast_delay() > 0) {
                // 0x4f1a70: the recast delay as time text; "Recast Type: n" when it has one.
                std::uint32_t s = e.recast_delay();
                std::string t = s >= 60 ? std::format("{}m {}s", s / 60, s % 60) : std::format("{}s", s);
                std::string line = str_args(ctx, 0x2412, {t});
                if (e.recast_type() != -1) line += ", " + str_args(ctx, 0x2417, {std::to_string(e.recast_type())});
                out += line + "<BR>";
            }
        }
        auto line = [&](int kind, std::uint32_t fmt, bool suffix) {
            const auto& x = item.effects[static_cast<std::size_t>(kind)];
            const SpellData* s = x.present() && ctx.spell ? ctx.spell(x.effect) : nullptr;
            if (!s) return;
            if (suffix) out += str_args(ctx, 0xbdc, {name_of(x, s), str(ctx, fmt)}) + "<BR>";
            else out += str_args(ctx, fmt, {name_of(x, s)}) + "<BR>";
            remember(x, s);
        };
        line(net::kEffectProc, 0xbd5, true);   // Effect: name (Combat)
        line(net::kEffectWorn, 0xbd8, true);   // Effect: name (Worn)
        line(net::kEffectFocus, 0x159a, false); // Focus Effect: name
        line(net::kEffectBard, 0x159a, false);
    }
    if (recurse)
        for (const auto& c : item.contents)
            if (c.index < 6) out += spell_effect_text(c.item, ctx, false, described);
    return out;
}

// ItemDisplay_BuildBaneDamageText @0x69d420 (the item, then its augments).
std::string bane_text(const net::Item& item, const ItemDisplayContext& ctx, bool recurse) {
    std::string out;
    std::uint32_t race = item.bane_dmg_race, body = item.bane_dmg_body;
    std::int32_t amount = race ? static_cast<std::int32_t>(item.bane_dmg_race_amt) : item.bane_dmg_amt;
    if ((race || body) && amount > 0) {
        std::string r = race ? race_name(race) : std::string(), b = body ? std::format("body type {}", body) : std::string();
        std::string a = std::to_string(amount);
        if (!race) out = str_args(ctx, 0xbcf, {b, a});
        else if (!body) out = str_args(ctx, 0xbce, {r, a});
        else out = str_args(ctx, 0xbd0, {b, r, a});
        out += "<BR>";
    }
    if (recurse)
        for (const auto& c : item.contents)
            if (c.index < 6) out += bane_text(c.item, ctx, false);
    return out;
}

// CItemDisplayWnd_CreateSkillModString @0x69c610.
std::string skill_mod_text(const net::Item& item, const ItemDisplayContext& ctx, bool recurse) {
    std::string out;
    std::int32_t value = item.skill_mod_value, type = item.skill_mod_type;
    if (type >= 0) {
        std::string line;
        if (type < 100) {
            if (value != 0) {
                line = str_args(ctx, 0xbd1, {str(ctx, m::skill_name_eqstr(static_cast<std::uint32_t>(type)))});
                line += std::format("  {} % ", value);
            }
            if (item.skill_mod_extra != 0)
                line += std::format("{}{:+d} ", line.empty() ? "" : " ", static_cast<std::int32_t>(item.skill_mod_extra));
            if (item.skill_mod_max > 0) line += str_args(ctx, 0x248d, {std::to_string(item.skill_mod_max)});
        } else {
            line = str_args(ctx, 0xbd1, {"INVALID SKILL", std::to_string(value)});
        }
        out += line;
        if (!line.empty()) out += "<BR>";
    }
    if (recurse)
        for (const auto& c : item.contents)
            if (c.index < 6) out += skill_mod_text(c.item, ctx, false);
    return out;
}

AugmentStrings aug_strings(const ItemDisplayContext& ctx) {
    AugmentStrings a;
    a.strings = ctx.strings;
    a.db_string = ctx.db_string;
    return a;
}

// The spell's description (GetSpellDescriptionText), as the effects string's "Spell Effect:" blocks use it.
std::string spell_description(const SpellData& s, const ItemDisplayContext& ctx) {
    if (!ctx.db_string || s.description_id == 0) return {};
    std::string text = ctx.db_string(static_cast<std::uint32_t>(s.description_id), 6);
    return spell_description_text(s, text, ctx.character, ctx.player_level, ctx.player_class);
}

// CItemDisplayWnd_CreateEvolvingString @0x69e010: 6146 "Evolving: Level %1/%2 %3%% %4" with the level (ItemClient
// +0x108), the highest level (+0x2c) and the progress (+0x20, a double, truncated); below the highest level %4 is a
// "Final Result" (6194) link, WndNotify 0x1b "1^<item number>^<+0xec>^<level>^<link hash of the item without its
// augments>" (+0xec taken as the evolving data's final item id).
std::string evolving_text(const net::Item& item, const ItemDisplayContext& ctx) {
    const net::EvolvingInfo& e = item.evolving;
    std::string link;
    if (e.evolve_level < e.evolve_max_level) {
        net::Item bare = item;
        bare.contents.erase(std::remove_if(bare.contents.begin(), bare.contents.end(),
                                           [](const net::BagItem& b) { return b.index < 6; }),
                            bare.contents.end());
        std::uint32_t hash = item_link_hash(item_link_hash_input(bare));
        link = std::format("<a WndNotify=\"27,1^{}^{}^{}^{}\">{}</a>", item.id, e.final_item_id, e.evolve_level, hash,
                           str(ctx, 6194));
    }
    return str_args(ctx, 6146, {std::to_string(e.evolve_level), std::to_string(e.evolve_max_level),
                                std::to_string(static_cast<std::int32_t>(e.progress)), link});
}

// CItemDisplayWnd::CreateMealSizeString @0x69db90: food (item type 14) and drink (15) by their duration
// (ItemBase_GetFoodDuration, ItemDefinition +0x51c: EQEmu's cast time): food up to 5 a snack (3080), then 3081 to 20,
// 3082 to 30, 3083 to 40, 3084 to 50, 3085 to 60, 3086 above; drink up to 5 3087, then 3088 to 20, 3089 to 30, 3090
// to 40, 3091 to 50, 3092 to 60, 3093 above. Other items: nothing.
std::string meal_size_text(const net::Item& item, const ItemDisplayContext& ctx) {
    if (item.item_type != 14 && item.item_type != 15) return {};
    std::int32_t d = item.cast_time_;
    std::uint32_t id;
    if (item.item_type == 14)
        id = d < 6 ? 3080 : d < 21 ? 3081 : d < 31 ? 3082 : d < 41 ? 3083 : d < 51 ? 3084 : d < 61 ? 3085 : 3086;
    else
        id = d < 6 ? 3087 : d < 21 ? 3088 : d < 31 ? 3089 : d < 41 ? 3090 : d < 51 ? 3091 : d < 61 ? 3092 : 3093;
    return str(ctx, id) + "<BR>";
}

// The compass (item type 40) in CreateEffectsString: 3009 "The compass points %1." with the player's heading as one of
// eight directions (12427 North .. 12434 NorthWest; North is 0..32 and 480..511, each other one 64 wide, turning
// west first).
std::string compass_text(const ItemDisplayContext& ctx) {
    if (ctx.heading < 0.0f) return {};
    auto h = static_cast<std::int16_t>(ctx.heading); // __ftol2_sse, kept as a short
    std::uint32_t id = 0;
    if (h >= 0x1e0) id = 12427;
    else if (h >= 0x1a1) id = 12428;
    else if (h >= 0x161) id = 12429;
    else if (h >= 0x121) id = 12430;
    else if (h >= 0xe1) id = 12431;
    else if (h >= 0xa1) id = 12432;
    else if (h >= 0x61) id = 12433;
    else if (h >= 0x21) id = 12434;
    else if (h >= 0) id = 12427;
    return str_args(ctx, 3009, {id ? str(ctx, id) : std::string()}) + "<BR>";
}

// CItemDisplayWnd_CreatePlaceableString @0x69e740: for a placeable item, "This item is placeable in" and the
// places its flags allow (all five when the flags are -1), joined with ", " and a final " and ", then ".". The
// "(of type: ...)" list it adds from the item instance is not sent by the server (docs/unknowns.md).
std::string placeable_text(const net::Item& item, const ItemDisplayContext& ctx) {
    std::int32_t flags = item.placeable_flags();
    if (flags == 0) return {};
    std::uint32_t mask = 0x8f; // the five places the client knows
    if (flags != -1) mask &= static_cast<std::uint32_t>(flags);
    int left = std::popcount(mask);
    if (left == 0) return {};
    std::string s = "<BR>" + str(ctx, 0x190e) + " ";
    // yards, guild yards, houses, guild halls, certain open zones
    for (auto [bit, id] : {std::pair{0x8u, 0x1491u}, {0x80u, 0x2ecu}, {0x4u, 0x1910u}, {0x2u, 0x1911u}, {0x1u, 0x190fu}}) {
        if (!(mask & bit)) continue;
        s += str(ctx, id);
        --left;
        s += left == 0 ? "" : left == 1 ? " and " : ", ";
    }
    return s + ".<BR>";
}

// The collection hint (eqstr 7690) and a trophy's benefit lines (CreateEffectsString after the placeable string):
// a personal trophy (benefit id set, flags 8 or 4) or a guild one (flags 0x80 or 2) whose benefit is known.
std::string collectible_trophy_text(const net::Item& item, const ItemDisplayContext& ctx) {
    std::string s;
    if (item.collectible()) s += str(ctx, 0x1e0a) + "<BR>";
    std::int32_t id = item.trophy_benefit();
    std::uint32_t flags = static_cast<std::uint32_t>(item.placeable_flags());
    if (id == -1 || flags == 0 || !ctx.trophy_benefit) return s;
    bool personal = (flags & 0xc) != 0, guild = (flags & 0x82) != 0;
    if (!personal && !guild) return s;
    auto b = ctx.trophy_benefit(id, !personal);
    if (!b) return s;
    s += "<BR>";
    s += (personal ? "This trophy grants you access to " : "This guild trophy grants your guild access to ") + b->first +
         ".<BR><BR>";
    s += "<C " + color_tag(kSpellName) + ">" + b->first + "</C><BR>" + b->second + "<BR>";
    return s;
}

// CItemDisplayWnd_CreateEffectsString @0x6a6810 (what this client keeps of an item).
std::string effects_text(const net::Item& item, const ItemDisplayContext& ctx) {
    std::string a, b, c, d; // the original's four text buffers
    std::vector<const net::ItemEffect*> described;
    if (item.item_class == 0) {
        if (item.is_lore() && item.lore_group > 0 && ctx.db_string) {
            std::string group = ctx.db_string(static_cast<std::uint32_t>(item.lore_group), 7);
            if (!group.empty()) a += str(ctx, 0x1806) + group + "<BR>";
        }
        if (item.expendable_arrow != 0) d += str(ctx, 0x16f) + "<BR>"; // Expendable
        if (item.is_evolving) d += evolving_text(item, ctx) + "<BR>"; // ItemClient +0x10c
        if (item.item_type != 54) {
            const auto& click = item.effects[net::kEffectClick];
            if (click.type == 3 || item.item_type == 21) {
                d += str_args(ctx, 0xbb9, {std::to_string(static_cast<std::int32_t>(item.charges))}) + "<BR>";
            } else if (click.present() && ctx.spell && ctx.spell(click.effect) && click.max_charges() > 0) {
                d += (static_cast<std::int32_t>(item.charges) < 1 ? str(ctx, 0xbba)
                                                                    : str_args(ctx, 0xbbb, {std::to_string(item.charges)})) +
                     "<BR>";
            }
            if (item.potion_belt_enabled) d += str_or(ctx, 0x248a, "Potion Belt Enabled") + "<BR>";
        } else {
            d += augment_slot_types_line(item, aug_strings(ctx)) + "<BR>"; // 0x69d6e0
        }
        if (item.item_type == 20) { // a scroll: the levels each usable class gets the spell at
            const auto& sc = item.effects[net::kEffectScroll];
            const SpellData* s = sc.present() && ctx.spell ? ctx.spell(sc.effect) : nullptr;
            if (s) {
                std::string levels;
                for (int k = 0; k < 16; ++k)
                    if (item.classes & (1u << k))
                        levels += std::format("{}({}) ", str(ctx, kClassAbbr[k]), s->class_level[static_cast<std::size_t>(k)]);
                if (!levels.empty()) {
                    auto line = ctx.level_needed_line ? ctx.level_needed_line(*s) : std::nullopt;
                    c += (line ? *line : str_args(ctx, 0xbbc, {levels})) + "<BR>";
                }
                if (ctx.show_spell_descriptions && s->description_id != 0)
                    c += str_args(ctx, s->is_skill ? 0x16b4 : 0x213b, {spell_description(*s, ctx)}) + "<BR>";
            }
        }
        if (item.item_type == 40) d += compass_text(ctx);
        d += bane_text(item, ctx, true);
        a += skill_mod_text(item, ctx, true);
        c += spell_effect_text(item, ctx, true, described);
        if (item.tradeskills) d += str(ctx, 0x236b) + "<BR>";
        d += augment_effects_restriction(item, aug_strings(ctx)); // ItemType 54 with an AugRestrictions text
        d += meal_size_text(item, ctx);
        if (ctx.show_spell_descriptions)
            for (const net::ItemEffect* e : described) {
                const SpellData* s = ctx.spell ? ctx.spell(e->effect) : nullptr;
                if (!s) continue;
                d += "<BR>";
                d += "<C " + color_tag(kSpellName) + ">" + (e->name.empty() ? s->name : e->name) + "</C><BR>";
                if (e->level > 0) d += str_args(ctx, 0x1aa4, {std::to_string(e->level)}) + "<BR>";
                d += str_args(ctx, 0x213b, {spell_description(*s, ctx)}) + "<BR>";
            }
    } else if (item.item_class == 1) {
        a += " " + str(ctx, 0xbe1) + "<BR>"; // Container: CLOSED. (the open state is the window's)
        if (item.is_lore() && item.lore_group > 0 && ctx.db_string) {
            std::string group = ctx.db_string(static_cast<std::uint32_t>(item.lore_group), 7);
            if (!group.empty()) a += str(ctx, 0x1806) + group + "<BR>";
        }
    } else if (item.item_class == 2) {
        a += " ";
        if (item.tradeskills) a += str(ctx, 0x236b) + "<BR>";
    }
    d += placeable_text(item, ctx);
    d += collectible_trophy_text(item, ctx);
    auto block = [](const std::string& s) { return s.empty() ? std::string() : s + "<BR>"; };
    return block(a) + block(b) + block(c) + d;
}

} // namespace

std::int32_t weapon_damage_bonus(std::int32_t level, std::uint8_t player_class, const net::Item& item) {
    if (level < 28) return 0;
    // The classes that get it (the byte table at 0x7e8c70): WAR PAL RNG SHD MNK BRD ROG BST BER.
    static constexpr bool kMelee[16] = {true, false, true, true, true, false, true, true,
                                        true, false, false, false, false, false, true, true};
    if (player_class < 1 || player_class > 16 || !kMelee[player_class - 1]) return 0;
    std::int32_t bonus = (level - 28) / 3 + 1;
    std::int32_t delay = item.delay;
    if (player_class == 16 && item.item_type == 19 && level >= 40) return delay * ((level - 40) / 2 + 1) / 30;
    if (item.item_type == 1 || item.item_type == 4 || item.item_type == 35) { // two-handed
        if (delay < 28) {
            bonus += 1;
        } else if (level > 29) {
            std::int32_t b = (level - 30) / 5 + 1;
            if (level > 50) {
                b += 1;
                std::int32_t e = level - 50;
                for (int over : {54, 56, 58, 59, 67})
                    if (level > over) ++e;
                b += delay * e / 40;
            }
            bonus += b;
        }
    }
    if (delay >= 40) {
        std::int32_t s = (delay - 40) / 3 + 1;
        if (delay >= 43) ++s;
        if (delay >= 45) ++s;
        bonus += s;
    }
    return bonus;
}

ItemDisplay build_item_display(const net::Item& item, const ItemDisplayContext& ctx) {
    ItemDisplay d;
    d.title = item.name;
    // SetItem @006ae100 (@006ae245): " (Augmented)" when a valid, visible socket holds an augment.
    if (is_augmented(item)) d.title += " " + str(ctx, 3368);
    auto info = [&](std::string text, std::uint32_t argb = kWhite) {
        if (static_cast<int>(d.info.size()) < ItemDisplay::kInfoLines) d.info.push_back({std::move(text), argb});
    };
    // The name, then the flags (both always).
    d.measured[0] = item.name;
    d.measured[1] = item_flags(item, ctx);
    info(d.measured[0]);
    info(d.measured[1]);

    // "Class: ..." (not for scrolls, item type 20): 8 on the first line, the rest on a second; "ALL" for all 16.
    if (item.item_type != 20) {
        TwoLines t;
        t.a = str(ctx, 0xbf8);
        for (int k = 0; k < 16; ++k)
            if (item.classes & (1u << k)) {
                (t.count < 8 ? t.a : t.b) += str(ctx, kClassAbbr[k]) + " ";
                ++t.count;
            }
        if (t.count == 0) t.a += str(ctx, 0x4c0);
        else if (t.count >= 16) t.a = str(ctx, 0xbf8) + "  " + str(ctx, 0x3395), t.b.clear();
        d.measured[3] = t.a, d.measured[4] = t.b;
        info(t.a);
        if (!t.b.empty()) info(t.b);
    }
    // "Race: ...": the race mask of the item and its augments.
    {
        TwoLines t;
        t.a = str(ctx, 0xbe9);
        std::uint32_t races = m::item_races(item);
        for (int k = 0; k < 17; ++k)
            if (races & (1u << k)) {
                (t.count < 8 ? t.a : t.b) += str(ctx, kRaceAbbr[k]) + " ";
                ++t.count;
            }
        if (t.count == 0) t.a += str(ctx, 0x4c0);
        else if (t.count >= 16) t.a = str(ctx, 0xbe9) + "  " + str(ctx, 0x3395), t.b.clear();
        d.measured[5] = t.a, d.measured[6] = t.b;
        info(t.a);
        if (!t.b.empty()) info(t.b);
    }
    // The deities (no label): 9 on the first line.
    if (std::uint32_t deity = m::item_deity(item)) {
        TwoLines t;
        for (int k = 1; k < 18; ++k)
            if (deity & (1u << (k - 1))) {
                std::string name = ctx.deity_name ? ctx.deity_name(static_cast<std::uint32_t>(200 + k)) : std::string();
                (t.count <= 8 ? t.a : t.b) += name + " ";
                ++t.count;
            }
        d.measured[7] = t.a, d.measured[8] = t.b;
        info(t.a);
        if (!t.b.empty()) info(t.b);
    }
    // ItemDisplay_BuildEquipSlotsText: the slots (8, 7 and the rest per line; one Ear, Wrist and Finger each).
    {
        std::string l[3];
        int n = 0;
        for (int k = 0; k < 23; ++k) {
            if (k == 4 || k == 10 || k == 16) continue;
            if (item.equip_slots & (1u << k)) {
                (n < 8 ? l[0] : n < 15 ? l[1] : l[2]) += str(ctx, kSlotNames[k]) + " ";
                ++n;
            }
        }
        d.measured[2] = l[0];
        if (!l[0].empty()) {
            info(l[0]);
            if (!l[1].empty()) {
                info(l[1]);
                if (!l[2].empty()) info(l[2]);
            }
            // The name line's colour: red when the player cannot use it anywhere, purple when only the prestige
            // check fails, green otherwise.
            bool usable;
            int t = item.item_type;
            if (t == 54 || t == 1 || t == 4 || t == 8 || t == 35)
                usable = ctx.can_use_all || (ctx.character && ctx.character->can_use_item(item, false));
            else usable = ctx.can_equip && ctx.can_equip(item);
            const bool prestige_ok = ctx.can_use_all || !ctx.character || ctx.character->can_use_item(item, true);
            d.info[0].argb = !usable ? kRed : (!prestige_ok ? kPurple : kGreen);
        }
    }

    Grid g{d};
    std::uint8_t skill = m::item_type_to_skill(item.item_type);
    auto modified = [&](std::int32_t v, std::uint8_t sk) {
        return ctx.modified && ctx.character ? ctx.character->recommended_item_value(item, v, sk) : v;
    };

    // ---- column 1, first block: size, weight, the container's numbers, levels, skill.
    g.put(1, str(ctx, 0x32cb), size_name(ctx, m::item_size(item)), 0, true);
    if (std::int32_t w = m::item_weight(item)) {
        char b[32];
        std::snprintf(b, sizeof b, "%10.1f", static_cast<double>(static_cast<float>(w) * 0.1f));
        g.put(1, str(ctx, 12999), b, 0, true);
    }
    if (item.item_class == 1) {
        if (item.weight_reduction) g.put_int(1, str(ctx, 0x33ff), item.weight_reduction);
        if (item.bag_slots) g.put_int(1, str(ctx, 0x3400), item.bag_slots);
        g.put(1, str(ctx, 0x3401), size_name(ctx, item.bag_size), 0, true);
    }
    if (std::int32_t v = m::item_recommended_level(item)) g.put_int(1, str(ctx, 0x32cc), v);
    if (std::int32_t v = m::item_required_level(item)) g.put_int(1, str(ctx, 0x32cd), v);
    if (is_weapon(item) || item.item_type == 27 || item.item_type == 28) {
        std::uint8_t sk = skill;
        if (item.item_type == 27 || item.item_type == 28) sk = 7;
        bool show = true;
        if (item.item_type == 19) sk = 51;
        else if (sk == 28 && item.item_type != 45) show = false;
        if (show) {
            if (item.item_type == 35) g.put(1, str(ctx, 0x32f5), "2H Piercing", 0, true);
            else g.put(1, str(ctx, 0x32f5), str(ctx, m::skill_name_eqstr(sk)), 0, true);
        }
    }
    if (std::int32_t v = m::item_recommended_skill(item)) g.put_int(1, str(ctx, 0x32ca), v);
    int end1 = g.row; // +0x2f0

    // ---- column 2: AC, HP, mana, endurance, haste, purity, power.
    g.row = 1;
    int columns = 1;
    bool any2 = false;
    auto col2 = [&](std::uint32_t id, std::int32_t (*f)(const net::Item&)) {
        std::int32_t aug = 0;
        std::int32_t v = modified(stat(item, f, aug), skill);
        if (v != 0) {
            g.put_int(2, str(ctx, id), v, aug);
            any2 = true;
        }
    };
    col2(0x32ce, f_ac);
    col2(0x32d2, f_hp);
    col2(0x32da, f_mana);
    col2(0x32db, f_end);
    // CItemDisplayWnd_AppendHasteString: the item's haste, then each augment's.
    {
        std::vector<std::int32_t> hastes;
        if (std::int32_t h = m::item_haste(item, 0, false)) hastes.push_back(h);
        if (plain(item))
            for (const auto& c : item.contents)
                if (c.index < 6)
                    if (std::int32_t h = m::item_haste(c.item, 0, false)) hastes.push_back(h);
        for (std::int32_t h : hastes) {
            g.put(2, str(ctx, 0x32e9), std::to_string(h) + "%", 0, true);
            any2 = true;
        }
    }
    if (item.purity > 0) {
        g.put(2, str(ctx, 0x340a), std::to_string(m::aug_stat_percent(item)), 0, true);
        any2 = true;
    }
    if (item.power > 0) { // MaxPower: "<power * 100 / max>%", the tooltip "<power>/<max>"
        // The power field this client keeps is the item's maximum; its current power is not sent separately.
        // Deliberate difference: the original divides an int32 product (instance +0x110 * 100 / GetMaxPower, the
        // product wrapping, the division signed), which wraps past 21.4 million. With the current power unknown and the
        // maximum standing in for it, the wrap would only turn the 100% this can show into a meaningless number, so it
        // multiplies in 64 bits.
        std::uint32_t cur = item.power, max = item.power;
        g.put(2, str(ctx, 0x340b), std::to_string(static_cast<std::uint64_t>(cur) * 100 / max) + "%", 0, true);
        g.cell(g.row - 1, 2).tooltip = std::to_string(cur) + "/" + std::to_string(max);
        any2 = true;
    }
    int end2 = g.row; // +0x2f4
    if (any2) columns = 2;

    // ---- column 3: damage, elemental damage, backstab, delay (and the damage bonus), range.
    g.row = 1;
    bool any3 = false;
    std::int32_t bonus = ctx.character ? weapon_damage_bonus(ctx.player_level, ctx.player_class, item) : 0;
    std::int32_t dmg_aug = 0;
    if (std::int32_t v = modified(stat(item, f_dmg, dmg_aug), skill)) {
        g.put_int(3, str(ctx, 0x32cf), v, dmg_aug);
        any3 = true;
    }
    if (plain(item)) { // CItemDisplayWnd_CreateElementalDamageString: sums by element over the item and augments
        std::array<std::int32_t, 10> elem{};
        auto add = [&](const net::Item& i) {
            if (i.elem_dmg_type > 0 && i.elem_dmg_type < 10 && i.elem_dmg_amt != 0) elem[i.elem_dmg_type] += i.elem_dmg_amt;
        };
        add(item);
        for (const auto& c : item.contents)
            if (c.index < 6) add(c.item);
        static constexpr std::uint32_t kElem[10] = {0, 0x3404, 0x3405, 0x3406, 0x3407, 0x3408, 0x11f4, 0x11f5, 0x11f6, 0x3409};
        for (int k = 1; k < 10; ++k)
            if (elem[static_cast<std::size_t>(k)] != 0) {
                g.put(3, str(ctx, kElem[k]), std::to_string(elem[static_cast<std::size_t>(k)]), 0, true);
                any3 = true;
            }
    }
    std::int32_t bs_aug = 0;
    if (std::int32_t v = modified(stat(item, f_backstab, bs_aug), skill)) {
        g.put_int(3, str(ctx, 0x3413), v, bs_aug);
        any3 = true;
    }
    if (item.delay != 0) {
        g.put_int(3, str(ctx, 0x32d1), item.delay);
        any3 = true;
        if (bonus != 0) g.put_int(3, str(ctx, 0x32d0), bonus);
    }
    if (item.range != 0) {
        g.put_int(3, str(ctx, 0x32c9), item.range);
        any3 = true;
    }
    int end3 = g.row; // +0x2f8
    if (any3 && columns < 3) columns = 3;

    // ---- the second block starts one row below the longest column.
    int start = std::max({end1, end2, end3}) + 1;
    g.row = start;
    bool any = false;
    auto stat_row = [&](int col, std::uint32_t id, std::int32_t (*f)(const net::Item&), std::int32_t (*h)(const net::Item&)) {
        std::int32_t aug = 0;
        std::int32_t v = modified(stat(item, f, aug), skill);
        std::int32_t hv = h ? modified(stat(item, h), skill) : 0;
        if (v != 0 || hv != 0) {
            g.put_int(col, str(ctx, id), v, aug);
            if (hv > 0) g.heroic(col, hv);
            any = true;
        }
    };
    stat_row(1, 0x32dc, f_str, f_hstr);
    stat_row(1, 0x32dd, f_sta, f_hsta);
    stat_row(1, 0x32de, f_int, f_hint);
    stat_row(1, 0x32df, f_wis, f_hwis);
    stat_row(1, 0x32e0, f_agi, f_hagi);
    stat_row(1, 0x32e1, f_dex, f_hdex);
    stat_row(1, 0x32e2, f_cha, f_hcha);
    int end4 = g.row; // +0x2fc
    if (any && columns < 1) columns = 1;

    g.row = start;
    any = false;
    stat_row(2, 0x32e3, f_mr, f_hmr);
    stat_row(2, 0x32e4, f_fr, f_hfr);
    stat_row(2, 0x32e5, f_cr, f_hcr);
    stat_row(2, 0x32e6, f_dr, f_hdr);
    stat_row(2, 0x32e7, f_pr, f_hpr);
    stat_row(2, 0x32e8, f_corr, f_hcorr);
    int end5 = g.row; // +0x300
    if (any && columns < 2) columns = 2;

    g.row = start;
    any = false;
    auto col3 = [&](std::uint32_t id, std::int32_t (*f)(const net::Item&), bool mod, const char* fallback = nullptr) {
        std::int32_t aug = 0;
        std::int32_t v = stat(item, f, aug);
        if (mod) v = modified(v, skill);
        if (v != 0) {
            g.put_int(3, fallback ? str_or(ctx, id, fallback) : str(ctx, id), v, aug);
            any = true;
        }
    };
    col3(0x340e, f_attack, false);
    col3(0x3402, f_regen, false);
    col3(0x32ea, f_mregen, false);
    col3(0x32eb, f_eregen, false);
    col3(0x32ec, f_sshield, false, "Spell Shield:");
    col3(0x32ed, f_ce, false, "Combat Effects:");
    col3(0x32ee, f_shielding, false, "Shielding:");
    col3(0x32ef, f_ds, false, "Damage Shield:");
    col3(0x32f0, f_dot, true, "DoT Shielding:");
    col3(0x32f1, f_dsmit, true, "DS Mitigation:");
    col3(0x32f2, f_avoid, true, "Avoidance:");
    col3(0x32f3, f_acc, true, "Accuracy:");
    col3(0x32f4, f_stun, true, "Stun Resist:");
    col3(0x3403, f_strike, true, "Strikethrough:");
    col3(0x33fb, f_heal, true);
    col3(0x33fc, f_sdmg, true);
    col3(0x23ed, f_clair, true);
    if (item.extra_dmg_amt != 0 && item.extra_dmg_skill >= 0) { // the damage bonus for one skill
        g.put_int(3, str(ctx, m::skill_name_eqstr(static_cast<std::uint32_t>(item.extra_dmg_skill))) + ":", item.extra_dmg_amt);
        any = true;
    }
    int end6 = g.row; // +0x304
    if (any && columns < 3) columns = 3;

    d.columns = columns;
    d.last_row = std::max({end4, end5, end6}) - 1;
    for (int r = ItemDisplay::kRows; r >= 1; --r)
        if (d.grid[static_cast<std::size_t>(r - 1)][0].shown || d.grid[static_cast<std::size_t>(r - 1)][1].shown ||
            d.grid[static_cast<std::size_t>(r - 1)][2].shown) {
            d.last_row = std::max(d.last_row, r);
            break;
        }
    d.mod_button = str(ctx, ctx.modified ? 0x3412 : 0x3411);

    // UpdateStrings: the effects, then the item's own text (the lore text this client has).
    d.description = effects_text(item, ctx);
    return d;
}


int item_display_text_width(const ItemDisplay& d, const std::function<int(std::string_view)>& extent) {
    const auto& m = d.measured;
    int w = extent(m[0].size() > m[1].size() ? m[0] : m[1]);
    for (std::size_t i = 2; i < m.size(); ++i) w = std::max(w, extent(m[i]));
    return w;
}

namespace {
// __ftol2_sse of (len - base) * 6.6f (the float at 0x9ec0cc): how much wider a socket's description makes the window.
int stml_overflow(int len, int base) {
    return static_cast<int>(static_cast<double>(len - base) * static_cast<double>(6.6f));
}
} // namespace

ItemDisplayLayout layout_item_display(const ItemDisplay& d, const ItemDisplayFrameIn& in) {
    ItemDisplayLayout l;
    // 0x69b590: row 1 starts 32 below the last info line (when there are two or more; else at 32), the info
    // lines keeping their XML places (16 apart from 10, 20 high). A shown appearance socket goes 5 below the last
    // line (@006a9e36) and the grid then starts from its top + 24 instead.
    const int count = static_cast<int>(d.info.size());
    const int last_info = count > 1 ? 10 + 16 * (count - 1) : 0;
    l.appearance_top = (count > 1 ? last_info + 20 : 0) + 5;
    l.grid_top = (in.appearance ? l.appearance_top + 24 : last_info) + 32;
    // @006ad8e4: the stat label one row below the blank row after the longest column (row last + 2). The 78 stat
    // labels (+0x344) are followed by the value labels (+0x47c), so past row 26 that is a value label of row - 26.
    int r = d.last_row + 2;
    if (r > ItemDisplay::kRows) r -= ItemDisplay::kRows;
    const int cell_top = l.row_top(r);
    // @006ad973: the Modified button on that row (x of IDW_Row1Col1Stat), 15 x 15; the layout goes on below it.
    l.mod_top = cell_top;
    float y = static_cast<float>(cell_top + 15);
    int columns = d.columns;
    int extra = 0; // [esp+0x68]: what the socket descriptions add to the width
    // @006ad9f2: a shown appearance socket uses three columns; a description over 80 characters widens the window.
    if (in.appearance) {
        columns = std::max(columns, 3);
        if (in.appearance_text > 0x50) extra = stml_overflow(in.appearance_text, 0x50);
    }
    // @006adad0: each shown socket screen 11 (the first) or 3 below the one above, 20 high, at its own x; a
    // description over 85 characters widens the window.
    bool first = true;
    for (std::size_t i = 0; i < in.sockets.size(); ++i) {
        l.socket_top[i] = -1;
        if (!in.sockets[i]) continue;
        columns = std::max(columns, 3);
        if (in.socket_text[i] > 0x55) extra = std::max(extra, stml_overflow(in.socket_text[i], 0x55));
        y += first ? 11.0f : 3.0f;
        first = false;
        l.socket_top[i] = static_cast<int>(y);
        y = static_cast<float>(l.socket_top[i] + 20);
    }
    // @006adc24: the description 8 below (CXWnd_SetAnchors(10, 1, top, 1, 0, 0, 3, 0)).
    l.description_top = static_cast<int>(y + 8.0f);
    // @006adc5c: wide enough for the widest info text from IDW_ItemInfo1's x (60) with 70 to spare, else for the
    // last value column, the socket descriptions and 50; as high as the description's top and 150.
    const int right = ItemDisplayLayout::kValueRight[std::clamp(columns, 1, 3) - 1];
    l.width = 60 + in.text_width > right ? 60 + in.text_width + 0x46 : right + extra + 0x32;
    l.height = l.description_top + 0x96;
    return l;
}

} // namespace hailstone::game
