#include "game/actor_look.h"

#include <cstdio>

// Follows the original's ActorClient appearance functions, checked against
// the recorded vectors; the order of the model calls is the original's (the
// vectors record it).

namespace hailstone::game {

namespace {

template <typename... Args> std::string format(const char* fmt, Args... args) {
    char buf[256];
    std::snprintf(buf, sizeof buf, fmt, args...);
    return buf;
}

// CDisplay::ChangeDag @0x40b260: tints the dag's part; zero RGB is untinted.
bool change_dag(ActorEnv& env, std::int32_t dag, std::uint32_t color) {
    if (!env.has_dag(dag) || !env.has_part(dag)) return false;
    if ((color & 0xffffffu) == 0) color = 0xffffffffu;
    env.tint_part(dag, color);
    return true;
}

// ActorClient::sub_0040a9c0 @0x40a9c0: attaches, then tints the part as it
// is; whether the dag has a part.
bool attach_tinted(ActorEnv& env, std::int32_t dag, const std::string& name, std::uint32_t tint,
                   const NewArmorAdjustment* adjustment, bool armor = false) {
    if (armor) env.attach_armor(dag, name, adjustment);
    else env.attach(dag, name, adjustment);
    if (!env.has_part(dag)) return false;
    env.tint_part(dag, tint);
    return true;
}

// The model parts of a wear slot (jump tables 0x40e400 / 0x40e424): the
// part an old (classic) model uses and the one a new model uses; -1 none.
struct WearSlotPart {
    std::int32_t classic;
    std::int32_t modern;
};
constexpr std::array<WearSlotPart, 9> kFirstPartBySlot{{
    {1, 1}, {0xb, 0xb}, {0x1b, 0x11}, {0xd, 0xd}, {0x13, 0x13}, {0xe, 0x14}, {0x15, 0x15}, {3, 3}, {4, 4},
}};
constexpr std::array<WearSlotPart, 9> kSecondPartBySlot{{
    {-1, -1}, {7, 7}, {0x1a, 0x11}, {0xc, 0xc}, {0x12, 0x12}, {0xe, 0x14}, {0x16, 0x16}, {-1, -1}, {-1, -1},
}};

std::int32_t slot_part(const std::array<WearSlotPart, 9>& parts, ActorEnv& env, std::uint32_t slot) {
    if (slot >= parts.size()) return -1;
    const WearSlotPart& p = parts[slot];
    // The jump table asks the model only where the two kinds differ.
    if (p.classic == p.modern) return p.classic;
    return env.new_model() ? p.modern : p.classic;
}

std::int32_t first_part(ActorEnv& env, std::uint32_t slot) { return slot_part(kFirstPartBySlot, env, slot); }

std::int32_t second_part(ActorEnv& env, std::uint32_t slot) { return slot_part(kSecondPartBySlot, env, slot); }

bool robe_material(std::int32_t material) {
    auto m = static_cast<std::uint32_t>(material);
    return m >= 0xa && m <= 0x10;
}

// A classic model's head-slot texture without a hero's forge piece, for
// material `m`: 0x13a5 for 0xf1 (and for Barbarians above 2), 0x1388 + m
// otherwise, except that 0xf0 and 3 take the race's helm (0x138b for the
// Iksar and Vah Shir, else 0x13a4); 3 stays 0x138b for the Iksar, Vah Shir
// and Frogloks.
std::int32_t classic_helm_texture(std::int32_t race, std::int32_t m) {
    if (m == 0xf1) return 0x13a5;
    if (m < 3) return m + 0x1388;
    if (race == 2) return 0x13a5;
    if (m != 0xf0 && m != 3) return m + 0x1388;
    const bool own_helm = race == 0x80 || race == 0x82;
    if (m == 3 && (own_helm || race == 0x14a)) return m + 0x1388;
    return own_helm ? 0x138b : 0x13a4;
}

bool beard_colour_by_face(std::int32_t race) { return race == 5 || race == 6 || race == 7; }

bool new_style_classic(const ActorLook& look) { return look.new_style && classic_head_race(look.race); }

} // namespace

std::uint32_t ActorEnv::set_fheb(ActorLook& look, std::uint32_t id, std::uint32_t type, std::uint32_t arg) {
    return game::set_fheb(look, *this, id, type, arg);
}

void ActorEnv::refresh_hair(ActorLook& look, std::uint8_t show_hair) { game::refresh_hair(look, *this, show_hair); }

bool classic_head_race(std::int32_t race) {
    return race <= 0xc || race == 0x80 || race == 0x82 || race == 0x14a || race == 0x20a;
}

HairColorRange allowed_hair_color_range(std::int32_t race) {
    switch (race) {
    case 5: return {4, 0x11};
    case 6: return {0xb, 0x13};
    case 9:
    case 10: return {0, 0x17};
    case 12: return {0, 0x16};
    case 128:
    case 130:
    case 330: return {0, 0};
    default: return {0, 0x12};
    }
}

std::uint8_t beard_kind(std::int32_t race, std::uint8_t gender) {
    // Women: only Dwarves (8) have the second kind. Men: High Elf, Dark Elf
    // and Half Elf (5..7) the second; Human, Barbarian, Erudite, Dwarf,
    // Halfling and Gnome the first.
    if (gender == 1) return race == 8 ? 2 : 0;
    if (race >= 5 && race <= 7) return 2;
    const bool first_kind = race == 1 || race == 2 || race == 3 || race == 8 || race == 11 || race == 12;
    return first_kind ? 1 : 0;
}

SplitFace split_face(std::int32_t face, std::uint8_t current_face, std::int32_t race, bool new_style) {
    if (new_style && classic_head_race(race)) return {static_cast<std::uint8_t>(face), std::nullopt};
    if (face < 0) face = current_face;
    if (face < 10) return {static_cast<std::uint8_t>(face), std::uint8_t{0}};
    auto index = static_cast<std::uint8_t>(face - face / 10 * 10);
    return {index, static_cast<std::uint8_t>((face - index) / 10)};
}

void normalize_slot_texture_ids(std::int32_t slot, std::int32_t& a, std::int32_t& c) {
    auto material = static_cast<std::uint32_t>(a);
    if (material >= 0xa && material <= 0x10) {
        if (slot == 1) c = static_cast<std::int32_t>(material - 0xa);
        a = 0xa;
        return;
    }
    if (slot == 0 && (material == 0xf0 || material == 0xf1)) {
        a = 1;
        return;
    }
    // 5..8 become 0..3; 0x11..0x13 and 0x14..0x16 each become 1..3; 0x17
    // is 4. Anything else stays.
    if (material >= 5 && material <= 8) a = static_cast<std::int32_t>(material - 5);
    else if (material >= 0x11 && material <= 0x13) a = static_cast<std::int32_t>(material - 0x10);
    else if (material >= 0x14 && material <= 0x16) a = static_cast<std::int32_t>(material - 0x13);
    else if (material == 0x17) a = 4;
}

std::optional<std::int32_t> npc_tint_entry(std::int32_t index, std::int32_t table_count) {
    if (index == 0) return std::nullopt;
    std::int32_t entry = static_cast<std::int32_t>(static_cast<std::uint32_t>(index) - 1u);
    if (entry >= table_count) return std::nullopt; // ColorTable_GetEntry: signed compare
    return entry;
}

std::int32_t race_sex_it_offset(std::int32_t race, std::uint8_t gender) {
    // The races in the order of their block of 60 helm IT numbers (block 0 is
    // every other race); women's numbers are 30 into the block.
    static constexpr std::array<std::int32_t, 14> kRaceByHelmBlock{7, 8, 6, 4, 5, 1, 128, 3, 2, 130, 9, 10, 11, 330};
    const std::int32_t women = gender == 1 ? 30 : 0;
    for (std::size_t block = 0; block < kRaceByHelmBlock.size(); ++block)
        if (kRaceByHelmBlock[block] == race) return static_cast<std::int32_t>(block + 1) * 60 + women;
    return women;
}

bool attach_helm_it(ActorLook& look, ActorEnv& env, std::int32_t id) {
    if (env.new_model()) return false;
    std::int32_t dag = 8 + (id >= 2000 ? 1 : 0);
    if (!env.has_dag(dag)) return false;
    id += race_sex_it_offset(look.race, look.gender);
    if ((look.race == 130 && look.face != 7) || look.race == 330) {
        look.hair_style = 0xff;
        look.beard = 0xff;
        id = 0;
    }
    return env.attach(dag, format("IT%d", id), nullptr);
}

std::uint32_t set_fheb(ActorLook& look, ActorEnv& env, std::uint32_t id, std::uint32_t type, std::uint32_t arg) {
    const auto feature = static_cast<std::uint8_t>(id);
    if (new_style_classic(look)) return 0; // SetFacialFeatureNewModel (the renderer's customization path)
    switch (static_cast<std::uint8_t>(type)) {
    case 1:
        if (!swap_face(look, env, feature, look.face, static_cast<std::int32_t>(arg))) return 0;
        if (look.luclin_model) {
            // The right eye again, from the left eye's colour (0 or 0xff: both 1).
            if (look.eye_color != 0xff && look.eye_color != 0) {
                look.eye_color2 = look.eye_color;
            } else {
                look.eye_color = 1;
                look.eye_color2 = 1;
            }
            set_luclin_eye(look, env, true, look.eye_color2);
        }
        return 1;
    case 2:
        if (env.new_model())
            return env.attach(8, format("%s_HS%2d_HEAD_HAIR", look.code.c_str(), static_cast<int>(feature)), nullptr) ? 1u : 0u;
        if (attach_helm_it(look, env, feature + 1000)) {
            look.hair_style = feature;
            return 1;
        }
        look.hair_style = 0xff;
        look.hair_color = 0xff;
        return 0;
    case 3:
        if (!set_luclin_eye(look, env, true, feature)) return 0;
        set_luclin_eye(look, env, false, feature);
        return 1;
    default:
        if (env.new_model())
            return env.attach(9, format("%s_FACIALHAIR_%02d", look.code.c_str(), static_cast<int>(feature)), nullptr) ? 1u : 0u;
        if (beard_kind(look.race, look.gender) == 2) return 1;
        if (attach_helm_it(look, env, feature + 2000)) {
            look.beard = feature;
            return 1;
        }
        look.beard = 0xff;
        look.beard_color = 0xff;
        return 0;
    }
}

std::uint32_t set_fheb_color(ActorLook& look, ActorEnv& env, std::uint32_t color, std::uint32_t type,
                             std::uint32_t arg) {
    const auto c = static_cast<std::uint8_t>(color);
    const auto t = static_cast<std::uint8_t>(type);
    if (c >= 0x18) {
        if (t != 1) return 0;
    } else if (t != 1) {
        HairColorRange range = allowed_hair_color_range(look.race);
        if (c < range.low || c > range.high) return 0;
    }
    std::int32_t dag;
    if (t == 4) {
        look.beard_color = c;
        if (look.gender == 1 ? look.race == 8 : beard_colour_by_face(look.race))
            return env.set_fheb(look, (type & 0xffffff00u) | look.face, 1, arg);
        dag = 9;
    } else if (t == 1) {
        std::uint8_t digit;
        if (new_style_classic(look)) {
            digit = 0xff;
        } else {
            digit = look.face;
            if (digit >= 10) digit = static_cast<std::uint8_t>(digit % 10);
        }
        return env.set_fheb(look, static_cast<std::uint8_t>(c * 10 + digit), 1, arg);
    } else {
        dag = 8;
        look.hair_color = c;
    }
    if (!env.has_dag(dag)) return 0;
    return change_dag(env, dag, kLuclinHairColors[c]) ? 1u : 0u;
}

void refresh_hair(ActorLook& look, ActorEnv& env, std::uint8_t show_hair) {
    if (look.race == 0x82 || !env.has_dag(8) || !env.has_dag(1)) return;
    if (env.has_part(1) && show_hair == 0) {
        env.attach(8, "0", nullptr);
        return;
    }
    env.set_fheb(look, look.hair_style, 2, 0xffffffffu);
    std::uint8_t color = look.hair_color;
    if (color < 0x18) {
        HairColorRange range = allowed_hair_color_range(look.race);
        if (color >= range.low && color <= range.high && env.has_dag(8)) change_dag(env, 8, kLuclinHairColors[color]);
    }
}

void normalize_body_materials(const ActorLook& look, ActorEnv& env, std::int32_t& from, std::int32_t& to,
                              std::int32_t kind) {
    if (to == 7) to = (look.race == 0x80 ? 1 : 0) + 2;
    if (from == 7) from = (look.race == 0x80 ? 1 : 0) + 2;
    const bool special = look.race == 0x14a || look.race == 0x82;
    auto adjust = [&](std::int32_t& value) {
        if (value == 4) {
            if (kind != 7) value = 1;
            return;
        }
        if (value < 17 || value > 23) return;
        // 17..22 become 1..3 twice over; 23 is 4 for kind 7, else 1.
        const int extended = value - 17;
        if (env.keep_extended_material(extended) && !special) return;
        if (extended == 6) value = kind == 7 ? 4 : 1;
        else value = extended % 3 + 1;
    };
    adjust(to);
    adjust(from);
    if (to == 0x17 && kind != 7) to = 1;
}

std::uint8_t swap_body_material(ActorLook& look, ActorEnv& env, std::int32_t slot, std::int32_t from,
                                std::int32_t to, std::uint32_t tint, std::int32_t variant, std::int32_t kind,
                                std::uint32_t no_tint) {
    normalize_body_materials(look, env, from, to, kind);
    if (slot < 1 || slot > 6) return 0;
    static constexpr const char* kSuffixes[6] = {"CH", "UA", "FA", "HN", "LG", "FT"};
    const std::string code = look.code + kSuffixes[slot - 1];
    const bool race82 = look.race == 0x82;
    const bool race14a = look.race == 0x14a;
    std::string from_name, to_name;
    if (!race82 && !race14a) {
        from_name = format("%s%02d01_MDF", code.c_str(), from);
        to_name = format("%s%02d01_MDF", code.c_str(), to);
    } else {
        from_name = format("%s%02d%01d1_MDF", code.c_str(), from, look.face_variant);
        to_name = format("%s%02d%01d1_MDF", code.c_str(), to, static_cast<int>(static_cast<std::uint8_t>(variant)));
    }
    std::uint32_t color = static_cast<std::uint8_t>(no_tint) != 0 ? 0xff000000u : (tint & 0xffffffu) | 0xff000000u;
    const bool extra = look.luclin_model || race82 || race14a;
    auto swap = [&](const std::string& a, const std::string& b) -> std::uint8_t {
        return env.replace_material(a, b, color) ? 1 : 0;
    };
    // Bumps a name's character k (the names are at least 9 long: code + 2 + "0101_MDF").
    auto bump = [](std::string& s, std::size_t k) { ++s[k]; };
    auto fr_step = [&] {
        if (to_name.compare(0, 2, "FR") == 0) {
            from_name[7] = '0';
            to_name[7] = '0';
        }
        bump(from_name, 8);
        bump(to_name, 8);
        swap(from_name, to_name);
    };
    std::uint8_t result = 1;
    switch (slot) {
    case 1:
        if (extra) {
            result = swap(from_name, to_name);
            for (int i = 0; i < 2; ++i) fr_step();
        } else {
            const std::string base = look.code.substr(0, 3);
            const bool from_high = static_cast<std::uint32_t>(from - 10) <= 6;
            if (static_cast<std::uint32_t>(to - 10) <= 6) {
                if (!from_high) {
                    to_name[5] = '0';
                    to_name[6] = '0';
                    std::uint8_t r = swap(from_name, to_name);
                    bump(from_name, 8);
                    bump(to_name, 8);
                    r |= swap(from_name, to_name);
                    result = r;
                    env.robe_mesh(1, base);
                }
                env.robe_tint(to - 6, color);
            } else {
                if (from_high) {
                    from_name[5] = '0';
                    from_name[6] = '0';
                    env.robe_off(base);
                    env.robe_mesh(0, base);
                }
                std::uint8_t r = swap(from_name, to_name);
                bump(from_name, 8);
                bump(to_name, 8);
                r |= swap(from_name, to_name);
                result = r;
            }
        }
        break;
    case 2:
    case 3:
        result = swap(from_name, to_name);
        if (extra) {
            bump(from_name, 8);
            bump(to_name, 8);
            swap(from_name, to_name);
        }
        break;
    case 4:
    case 6: {
        result = swap(from_name, to_name);
        int passes = extra ? 4 : 1;
        for (int i = 0; i < passes; ++i) {
            bump(from_name, 8);
            bump(to_name, 8);
            if (slot == 6 && race14a) color = 0xffffffffu;
            swap(from_name, to_name);
        }
        break;
    }
    case 5: {
        result = swap(from_name, to_name);
        if (race82) {
            std::string from_alt = from_name, to_alt = to_name;
            from_alt.replace(3, 2, "TA");
            to_alt.replace(3, 2, "TA");
            if (swap(from_alt, to_alt) == 0) {
                from_alt.replace(3, 4, "TA00");
                to_alt.replace(3, 4, "TA00");
                swap(from_alt, to_alt);
            }
        }
        int passes = extra ? 4 : 2;
        if (result == 1)
            for (int i = 0; i < passes; ++i) fr_step();
        break;
    }
    default: break;
    }
    if (result != 0) {
        look.armor[static_cast<std::size_t>(slot)].material = to;
        look.tint[static_cast<std::size_t>(slot)] = tint;
        return result;
    }
    if (!race14a) {
        if (slot == 1) {
            look.armor[1].material = static_cast<std::uint8_t>(no_tint) != 0 ? to : 0;
            return result;
        }
    } else if (slot == 1) {
        return result;
    }
    if (look.race != 0x14a || to == 0) return result;
    if (swap_body_material(look, env, slot, from, 0, tint, variant, kind, no_tint) != 0) {
        look.armor[static_cast<std::size_t>(slot)].material = 0;
        look.tint[static_cast<std::size_t>(slot)] = 0xffffffffu;
    }
    return result;
}

void update_new_armor_slot(ActorLook& look, ActorEnv& env, std::uint32_t slot) {
    const std::int32_t part = first_part(env, slot);
    const std::optional<std::string> part_object = env.part_object(part);
    ArmorRecord* record = slot < 9 ? &look.armor[slot] : nullptr;
    bool robed = false;
    bool keep_texture = true;
    if (robe_material(look.armor[1].material)) {
        robed = true;
        if (slot != 6 && slot != 4 && slot != 0 && slot != 1) keep_texture = false;
    }
    if (!record || (slot == 0 && !look.show_helm)) return;
    std::int32_t a = record->material;
    const std::int32_t b = record->variation;
    std::int32_t c = record->material2;
    normalize_slot_texture_ids(static_cast<std::int32_t>(slot), a, c);
    const std::string& code = look.code;
    std::optional<NewArmorTag> armor;
    const NewArmorAdjustment* part_record = nullptr;
    bool show_hair = false;
    if (record->new_armor_id && !look.new_armor_filtered) {
        std::int32_t variant = robed && part == 0xb ? 1 : 0;
        armor = env.find_new_armor_tag(record->new_armor_id, code);
        if (armor) show_hair = armor->show_hair;
        if (!armor || !armor->ignore_adjustment) part_record = env.find_adjustment(part, variant, code);
    }
    auto retexture = [&](std::int32_t which, const std::string& object_name, std::int32_t item,
                         const NewArmorAdjustment* adjustment) {
        std::string name;
        if (robed && !keep_texture) name = "0";
        else if (armor && !(robed && slot == 6)) name = format("IT%d", item);
        else name = format("%s_%02d_%02d_%s", code.c_str(), a, b, object_name.c_str());
        std::uint32_t tint = armor ? 0xff000000u : look.tint[slot];
        bool took = attach_tinted(env, which, name, tint, adjustment, armor.has_value());
        if (c && env.has_part(which)) env.overlay_part(which, format("A_%s_%02d_%02d_S%02d", code.c_str(), a, b, c));
        return took;
    };
    if (part_object) {
        bool took = retexture(part, *part_object, armor ? armor->it_number : 0, part_record);
        if (slot == 0) {
            if (took && !show_hair) {
                env.attach(8, "", nullptr); // ClearHairAttachment
                return;
            }
            env.attach(8, format("%s_HAIR_%02d", code.c_str(), static_cast<int>(look.hair_style)), nullptr);
            change_dag(env, 8, env.customization_hair_color(look, look.hair_color));
            return;
        }
    }
    if (slot != 6 && slot != 4 && slot != 3) return;
    const std::int32_t second = second_part(env, slot);
    const std::optional<std::string> second_object = env.part_object(second);
    part_record = nullptr;
    if (record->new_armor_id != 0 && (!armor || !armor->ignore_adjustment))
        part_record = env.find_adjustment(second, 0, code);
    if (!second_object) return;
    retexture(second, *second_object, armor ? armor->it_pair : 0, part_record);
}

void apply_armor_texture(ActorLook& look, ActorEnv& env, std::int32_t slot) {
    const std::int32_t race = look.race;
    if (!classic_head_race(race)) return;
    const std::int32_t end = slot == -1 ? 6 : slot;
    const bool robed = robe_material(look.armor[1].material);
    const bool froglok = race == 0x14a;
    const std::string& code = look.code;
    for (std::int32_t cur = slot < 0 ? 0 : slot; cur <= (end < 6 ? end : 6); ++cur) {
        if (env.new_model()) {
            update_new_armor_slot(look, env, static_cast<std::uint32_t>(cur));
            continue;
        }
        std::int32_t second = -1;
        const ArmorRecord& record = look.armor[static_cast<std::size_t>(cur)];
        const auto material = static_cast<std::uint32_t>(record.material);
        std::int32_t texture = 0;
        std::int32_t texture2 = 0;
        std::int32_t part = first_part(env, static_cast<std::uint32_t>(cur));
        std::optional<NewArmorTag> armor;
        if (record.new_armor_id && !look.new_armor_filtered) armor = env.find_new_armor_tag(record.new_armor_id, code);
        // A classic material (0..3) picks a pair of textures from `first` on.
        auto pair_from = [&](std::int32_t first) {
            if (material > 3) return;
            texture = first + 2 * static_cast<std::int32_t>(material);
            texture2 = texture + 1;
        };
        switch (cur) {
        case 0: {
            if ((!armor && race == 3 && robed && !env.mesh_hidden(1)) || !look.show_helm) {
                texture = 0;
                armor.reset();
                break;
            }
            if (!armor) texture = classic_helm_texture(race, static_cast<std::int32_t>(material));
            break;
        }
        case 1: {
            second = 7;
            if (!armor && material <= 3) {
                part = 0xb;
                texture = material == 3 ? 0x1770 : 0;
                const bool no_second = env.mesh_hidden(5) || (race == 0xc && look.gender == 1 && material == 1);
                texture2 = no_second ? 0 : static_cast<std::int32_t>(material) + 0xfaa;
            } else if (!armor && robe_material(record.material)) {
                texture2 = static_cast<std::int32_t>(material) + 0xf96;
                if (race == 3 && (look.new_armor_filtered || look.armor[0].new_armor_id == 0))
                    env.attach(1, "0", nullptr);
            }
            const std::int32_t last = froglok ? 3 : 6;
            for (std::int32_t sub = race != 3 ? 1 : 0; sub <= last; ++sub) {
                if (sub == 4) continue;
                if (sub != 6) env.show_part(sub, armor && armor->hide_mesh && robed ? 1 : 0, 1);
                if (sub != 1) apply_armor_texture(look, env, sub);
            }
            break;
        }
        case 2:
            second = env.new_model() ? 0x11 : 0x1a;
            if (!armor) pair_from(0x1b58);
            break;
        case 3:
            second = 0xc;
            if (!armor) pair_from(0x1f40);
            break;
        case 5:
            if (!armor) texture = static_cast<std::int32_t>(material) + 0xfb4;
            break;
        case 6:
            second = 0x16;
            if (robed) armor.reset();
            else if (armor) break;
            pair_from(0x2328);
            break;
        case 4:
            second = 0x12;
            break;
        default: break;
        }
        if (!froglok) env.show_part(cur, armor && armor->hide_mesh && !robed ? 1 : 0, 0);
        if (robed && cur != 0 && cur != 1 && cur != 4) {
            texture = 0;
            texture2 = 0;
            armor.reset();
        }
        const std::uint32_t tint = look.tint[static_cast<std::size_t>(cur)];
        auto name = [](std::int32_t id) { return id == 0 ? std::string("0") : format("IT%d", id); };
        if (armor) {
            const std::uint32_t color = tint & 0xff000000u;
            if (part != -1) {
                const NewArmorAdjustment* adjustment =
                    armor->ignore_adjustment ? nullptr : env.find_adjustment(part, robed && part == 0xb ? 1 : 0, code);
                env.attach_armor(part, name(armor->it_number), adjustment);
                change_dag(env, part, color);
            }
            if (second != -1) {
                const NewArmorAdjustment* adjustment =
                    armor->ignore_adjustment ? nullptr : env.find_adjustment(second, robed && second == 0xb ? 1 : 0, code);
                env.attach_armor(second, name(armor->it_pair), adjustment);
                change_dag(env, second, color);
            }
        } else {
            const std::int32_t offset = race_sex_it_offset(race, look.gender);
            if (part != -1) {
                env.attach(part, name(texture == 0 ? 0 : offset + texture), nullptr);
                change_dag(env, part, tint);
            }
            if (second != -1) {
                env.attach(second, name(texture2 == 0 ? 0 : offset + texture2), nullptr);
                change_dag(env, second, tint);
            }
        }
        if (cur == 0) env.refresh_hair(look, armor ? (armor->show_hair ? 1 : 0) : 0);
    }
}

namespace {

std::int32_t clamp_count(std::int32_t value, std::int32_t count) { return value < count ? value : 0; }

bool set_eye_material(ActorLook& look, ActorEnv& env, std::int32_t which, std::int32_t material) {
    return env.set_material(which, format("C_%s_%s_S%02d_M%02d", look.code.c_str(), which == 2 ? "LEFTEYE" : "RIGHTEYE",
                                          material, which + 1));
}

bool attach_facial_attachment(ActorLook& look, ActorEnv& env, std::int32_t id) {
    return env.attach(0x18, format("%s_FACIALATT_%02d", look.code.c_str(), id), nullptr);
}

} // namespace

bool set_tattoo(ActorLook& look, ActorEnv& env, std::int32_t tattoo) {
    bool shown = env.attach(0x19, format("%s_TATTOO_00", look.code.c_str()), nullptr);
    if (tattoo <= 0 || !env.has_part(0x19)) return shown;
    return env.set_part_material(0x19, 0, format("A_%s_TATTOO_S%02d_M01", look.code.c_str(), tattoo)) || shown;
}

std::uint32_t set_facial_feature_new_model(ActorLook& look, ActorEnv& env, std::int32_t value, std::int32_t type) {
    const CustomizationCounts counts = env.customization_counts(look);
    switch (type) {
    case 2: {
        const std::int32_t v = clamp_count(value, counts.hair_styles);
        look.hair_style = static_cast<std::uint8_t>(v);
        env.attach(8, format("%s_HAIR_%02d", look.code.c_str(), v), nullptr);
        if (look.hair_color >= counts.hair_colors) look.hair_color = 0;
        change_dag(env, 8, env.customization_hair_color(look, look.hair_color));
        return 0;
    }
    case 9:
    case 10: {
        const bool tattoo = type == 9;
        const std::int32_t v = clamp_count(value, tattoo ? counts.tattoos : counts.facial_attachments);
        const std::int32_t dag = tattoo ? 0x19 : 0x18;
        if (!env.has_dag(dag)) return 0;
        if (tattoo) {
            look.tattoo = v;
            set_tattoo(look, env, v);
        } else {
            look.details = v;
            attach_facial_attachment(look, env, v);
        }
        return change_dag(env, dag, env.customization_base_color(look)) ? 1u : 0u;
    }
    default: return 0;
    }
}

void apply_facial_features(ActorLook& look, ActorEnv& env, std::uint32_t arg) {
    if (new_style_classic(look)) {
        const CustomizationCounts counts = env.customization_counts(look);
        look.face = static_cast<std::uint8_t>(clamp_count(look.face, counts.faces));
        env.set_material(0, format("C_%s_HEAD_S%02d_M%02d", look.code.c_str(), static_cast<int>(look.face), 1));
        set_facial_feature_new_model(look, env, look.hair_style, 2);
        look.hair_color = static_cast<std::uint8_t>(clamp_count(look.hair_color, counts.hair_colors));
        change_dag(env, 8, env.customization_hair_color(look, look.hair_color));
        look.beard = static_cast<std::uint8_t>(clamp_count(look.beard, counts.beards));
        env.attach(9, format("%s_FACIALHAIR_%02d", look.code.c_str(), static_cast<int>(look.beard)), nullptr);
        if (look.beard_color >= counts.hair_colors) look.beard_color = 0;
        change_dag(env, 9, env.customization_hair_color(look, look.beard_color));
        look.beard_color = static_cast<std::uint8_t>(clamp_count(look.beard_color, counts.hair_colors));
        change_dag(env, 9, env.customization_hair_color(look, look.beard_color));
        const std::int32_t eyes = clamp_count(look.eye_color, counts.eyes);
        set_eye_material(look, env, 1, eyes);
        set_eye_material(look, env, 2, eyes);
        look.eye_color = look.eye_color2 = static_cast<std::uint8_t>(eyes);
        const std::int32_t tattoo = clamp_count(look.tattoo, counts.tattoos);
        if (env.has_dag(0x19)) {
            look.tattoo = tattoo;
            set_tattoo(look, env, tattoo);
            change_dag(env, 0x19, env.customization_base_color(look));
        }
        const std::int32_t details = clamp_count(look.details, counts.facial_attachments);
        if (env.has_dag(0x18)) {
            look.details = details;
            attach_facial_attachment(look, env, details);
            change_dag(env, 0x18, env.customization_base_color(look));
        }
        apply_armor_texture(look, env, -1);
        return;
    }
    if (!look.luclin_model || look.texture_type != 0xff) return;
    env.set_fheb(look, look.hair_style, 2, arg);
    env.set_fheb(look, look.beard, 4, arg);
    if (look.hair_color < 0x18) {
        HairColorRange range = allowed_hair_color_range(look.race);
        if (look.hair_color >= range.low && look.hair_color <= range.high && env.has_dag(8))
            change_dag(env, 8, kLuclinHairColors[look.hair_color]);
    }
    if (look.beard_color < 0x18) {
        HairColorRange range = allowed_hair_color_range(look.race);
        if (look.beard_color >= range.low && look.beard_color <= range.high) {
            if (look.gender == 1 ? look.race == 8 : beard_colour_by_face(look.race)) env.set_fheb(look, look.face, 1, arg);
            else if (env.has_dag(9)) change_dag(env, 9, kLuclinHairColors[look.beard_color]);
        }
    }
    look.eye_color2 = look.eye_color;
    set_luclin_eye(look, env, true, look.eye_color);
    set_luclin_eye(look, env, false, look.eye_color);
    attach_facial_attachment(look, env, look.details);
    apply_armor_texture(look, env, -1);
}

void find_eye_materials(ActorLook& look, ActorEnv& env) {
    // UpdateDAGPointers @0x40be3d: (&+0x140, &+0x144, race 11 with gender byte 0, race 0x14a).
    env.find_eye_materials(look.eye_left, look.eye_right, look.race == 11 && look.gender == 0, look.race == 0x14a);
}

std::uint32_t set_luclin_eye(ActorLook& look, ActorEnv& env, bool secondary, std::uint32_t material) {
    if (new_style_classic(look))
        return set_eye_material(look, env, secondary ? 1 : 0, static_cast<std::int32_t>(material)) ? 1u : 0u;
    if (!look.luclin_model) return 0;
    if (look.eye_right < 0 || material == 0xff) return 0;
    std::int32_t entry = look.eye_left;
    if (secondary) {
        const auto face = static_cast<std::uint8_t>(look.face >= 10 ? look.face % 10 : look.face);
        const bool male = look.gender == 0;
        if ((look.race == 1 && male && face == 3) || (look.race == 2 && male && face == 6)) material = 0xd1;
        else if ((look.race == 8 && male && face == 5) || (look.race == 2 && male && face == 4)) material = 0xcf;
        entry = look.eye_right;
    }
    // The races with eye textures of their own, in the order of their block
    // of 0x14 (block 0 is every other race).
    static constexpr std::array<std::int32_t, 6> kRaceByEyeBlock{6, 0x80, 0x82, 10, 9, 0x14a};
    std::uint32_t offset = 0;
    for (std::size_t block = 0; block < kRaceByEyeBlock.size(); ++block)
        if (kRaceByEyeBlock[block] == look.race) offset = static_cast<std::uint32_t>(block + 1) * 0x14u;
    if (!env.set_eye_texture(material + offset, entry)) return 0;
    (secondary ? look.eye_color2 : look.eye_color) = static_cast<std::uint8_t>(material);
    return 1;
}

std::uint8_t swap_face(ActorLook& look, ActorEnv& env, std::int32_t to, std::int32_t from, std::int32_t kind) {
    if (new_style_classic(look)) return set_facial_feature_new_model(look, env, to, 1) != 0 ? 1 : 0;
    if (from < 0 || to < 0) return 0;
    const SplitFace old_face = split_face(from, look.face, look.race, look.new_style);
    const SplitFace new_face = split_face(to, look.face, look.race, look.new_style);
    const std::uint8_t old_variant = old_face.variant.value_or(0);
    std::uint8_t new_variant = new_face.variant.value_or(0);
    if (beard_kind(look.race, look.gender) == 2 && new_variant > 3) new_variant = 0;
    const int old_index = old_face.index, new_index = new_face.index;
    const char* code = look.code.c_str();
    std::string old_name, new_name;
    auto swap = [&](std::uint32_t tint) { return env.replace_material(old_name, new_name, tint); };
    auto next = [&] {
        ++old_name[8];
        ++new_name[8];
        return swap(0);
    };
    if (look.texture_type != 0xff) {
        // A fixed body texture: face index 0 takes the HeadType variant, matched loosely (mode 1: the variant
        // digits of the palette entry's name are not compared).
        const int head = static_cast<std::int8_t>(look.head_type);
        old_name = format("%sHE%02d%d1_MDF", code, old_index == 0 ? head : 0, old_index);
        new_name = format("%sHE%02d%d1_MDF", code, new_index == 0 ? head : 0, new_index);
        auto swap_mode = [&] {
            return old_index == 0 ? env.replace_material_loose(old_name, new_name, 0) : swap(0);
        };
        swap_mode();
        ++old_name[8];
        ++new_name[8];
        swap_mode();
    } else {
        std::uint32_t tint = 0;
        if (look.luclin_model) {
            if (look.race == 0x82) {
                // Vah Shir: the face 7 head has its own hair part, the others none.
                if (env.has_dag(8)) {
                    std::string item = "0";
                    if (new_index == 7) {
                        look.hair_style = 0;
                        item = format("IT%d", race_sex_it_offset(look.race, look.gender) + 1000);
                    }
                    env.attach(8, item, nullptr);
                }
            } else if (beard_kind(look.race, look.gender) == 2 && new_variant != 0 && look.beard_color < 0x18) {
                tint = kLuclinHairColors[look.beard_color]; // the beard is painted on the face variant
            }
        }
        old_name = format("%sHE%02d%d1_MDF", code, static_cast<int>(old_variant), old_index);
        new_name = format("%sHE%02d%d1_MDF", code, static_cast<int>(new_variant), new_index);
        if (!swap(tint) && old_index != 0) {
            old_name = old_name.substr(0, 7) + "01_MDF"; // the face 0 entry of that variant
            swap(tint);
        }
        if (!look.luclin_model && look.race != 0x82) {
            next();
        } else if (look.race == 0x82 || look.race == 0x14a) {
            for (int i = 0; i < 7; ++i) next();
            for (std::int32_t slot = 1; slot <= 6; ++slot) {
                const ArmorRecord& r = look.armor[static_cast<std::size_t>(slot)];
                const std::uint32_t color = look.tint[static_cast<std::size_t>(slot)];
                const std::uint32_t no_tint = r.new_armor_id != 0 ? 1u : 0u;
                if (swap_body_material(look, env, slot, r.material, r.material, color, new_index, kind, no_tint))
                    continue;
                if (swap_body_material(look, env, slot, r.material, 0, color, new_index, kind, no_tint)) continue;
                look.face = 0;
                swap_body_material(look, env, slot, r.material, 0, color, new_index, kind, no_tint);
            }
        } else if (look.race == 0x80) {
            for (int i = 0; i < 7; ++i) next();
        } else {
            auto part = [&](char c, std::uint32_t t) {
                old_name[8] = c;
                new_name[8] = c;
                swap(t);
            };
            if (look.race == 3 || look.race == 9 || look.race == 10) part('2', 0);
            else if ((look.race >= 5 && look.race <= 7 && look.gender == 0) || (look.race == 8 && look.gender == 1))
                part('3', tint);
            part('4', 0);
            next();
        }
    }
    look.face = static_cast<std::uint8_t>(to);
    look.face_variant = new_index;
    return 1;
}

} // namespace hailstone::game
