#include "net/shroud_packets.h"

#include <charconv>
#include <unordered_set>

#include "net/packet_io.h"

namespace hailstone::net {
namespace {

using detail::decode_with;

// The server sends ASCII decimal values mixed with text fields. Keep empty
// fields (including a final empty description) and reject partial numbers.
class Fields {
public:
    Fields(std::string_view text, char separator) : text_(text), separator_(separator) {}

    bool done() const { return text_.data() == nullptr; }

    std::string_view next() {
        if (done()) throw std::out_of_range("truncated shroud text fields");
        const auto end = text_.find(separator_);
        const auto field = text_.substr(0, end);
        text_ = end == std::string_view::npos ? std::string_view{} : text_.substr(end + 1);
        return field;
    }

    template <typename T = std::uint32_t> T number(bool allow_hex = false) {
        auto field = next();
        const bool hex = allow_hex && (field.starts_with("0x") || field.starts_with("0X"));
        if (hex) field.remove_prefix(2);
        T value = 0;
        const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value, hex ? 16 : 10);
        if (error != std::errc{} || end != field.data() + field.size())
            throw std::out_of_range("invalid number in shroud text");
        return value;
    }

private:
    std::string_view text_;
    char separator_;
};

void require(bool condition) {
    if (!condition) throw std::out_of_range("invalid shroud payload");
}

std::string read_text(ByteReader& r) {
    auto value = r.cstring();
    require(r.empty());
    return value;
}

ShroudStats read_stats_text(std::string_view text) {
    Fields fields(text, '^');
    ShroudStats value;
    value.selection_id = fields.number();
    value.has_data = true;
    value.name = fields.next();
    value.description = fields.next();
    for (auto* field : {&value.class_, &value.level, &value.hp, &value.mana, &value.endurance})
        *field = fields.number();
    for (auto* field : {&value.str, &value.sta, &value.cha, &value.dex, &value.int_, &value.agi, &value.wis})
        *field = fields.number<std::int32_t>();
    const auto abilities = fields.number();
    for (std::uint32_t i = 0; i < abilities; ++i)
        value.abilities.push_back({fields.number(true), fields.number(true)});
    const auto items = fields.number();
    for (std::uint32_t i = 0; i < items; ++i)
        value.items.push_back({fields.number(true), std::string(fields.next())});
    require(fields.done());
    return value;
}

ShroudItemReply read_item_reply(ByteReader& r, std::uint32_t selection_id) {
    ShroudItemReply value;
    value.selection_id = selection_id;
    value.item_id = r.le<std::uint32_t>();
    value.item = read_item(r);
    require(r.empty() && value.item_id == value.item.id);
    return value;
}

// The ZoneEntry spawn and CharInventory blocks use their usual wire layouts.
// The section between them is a fixed internal profile image, not the counted
// OP_PlayerProfile wire format: Handle_Msg6562_ReplacePlayer @0x4c2ea0 installs
// it and hands the bytes after its 0x4ff8 to the inventory reader.
constexpr std::size_t kShroudProfileBytes = 0x4ff8;

std::vector<std::uint32_t> profile_u32_array(ByteReader& r, std::size_t start, std::size_t end) {
    r.seek(start);
    std::vector<std::uint32_t> values((end - start) / sizeof(std::uint32_t));
    for (auto& value : values) value = r.le<std::uint32_t>();
    return values;
}

void read_shroud_profile(ByteReader& r, PlayerProfile& v) {
    const auto u8 = [&] { return r.le<std::uint8_t>(); };
    const auto u32 = [&] { return r.le<std::uint32_t>(); };
    const auto i32 = [&] { return r.le<std::int32_t>(); };
    v.shroud_type = u32();
    r.seek(0x8);
    v.gender = u8();
    r.seek(0xc);
    v.race = u32();
    v.class_ = u8();
    v.level = u8();
    r.seek(0x14);
    v.binds.resize(5);
    for (auto& b : v.binds) b = {u32(), r.f32(), r.f32(), r.f32(), r.f32()};
    v.deity = u32();
    v.intoxication = u32();

    r.seek(0xa8);
    v.equipment.resize(22);
    for (auto& e : v.equipment) {
        e.material = u32();
        e.elite_material = u32();
        e.unknown1 = u32();
        e.hero_forge_model = u32();
        e.material2 = u32();
    }
    v.equipment_tint = profile_u32_array(r, 0x314, 0x338);
    r.seek(0x35c);
    v.hair_color = u8();
    v.beard_color = u8();
    r.seek(0x364);
    for (auto* part : {&v.eye_color1, &v.eye_color2, &v.hair_style, &v.beard, &v.face}) *part = u8();
    r.seek(0x36c);
    v.drakkin_heritage = u32();
    v.drakkin_tattoo = u32();
    v.drakkin_details = u32();
    r.seek(0x37c);
    v.height = r.f32();
    r.seek(0x394);
    for (auto* value : {&v.points, &v.mana, &v.cur_hp, &v.str, &v.sta, &v.cha, &v.dex, &v.int_, &v.agi, &v.wis})
        *value = u32();

    r.seek(0x3d8);
    v.aa.resize((0x11e8 - 0x3d8) / (sizeof(std::uint32_t) * 3));
    for (auto& rank : v.aa) rank = {u32(), u32(), u32()};
    v.skills = profile_u32_array(r, 0x11e8, 0x1378);
    v.innate_skills = profile_u32_array(r, 0x1378, 0x13dc);
    v.disciplines = profile_u32_array(r, 0x13dc, 0x188c);
    v.combat_ability_timers = profile_u32_array(r, 0x188c, 0x18dc);
    v.recast_timers = profile_u32_array(r, 0x18dc, 0x192c);
    v.item_recast_timers = profile_u32_array(r, 0x192c, 0x1abc);
    v.spell_book = profile_u32_array(r, 0x1abc, 0x25fc);
    v.mem_spells = profile_u32_array(r, 0x25fc, 0x263c);
    v.gem_refresh = profile_u32_array(r, 0x263c, 0x2670);

    constexpr std::size_t kBuffBytes = 0x58;
    v.buffs.resize((0x34e4 - 0x2674) / kBuffBytes);
    for (std::size_t i = 0; i < v.buffs.size(); ++i) {
        r.seek(0x2674 + i * kBuffBytes);
        auto& buff = v.buffs[i];
        buff.effect_type = u8();
        buff.level = u8();
        r.skip(2);
        buff.instrument_mod = r.f32();
        buff.spell_id = u32();
        buff.duration = i32();
        buff.caster_id = u32();
        buff.num_hits = u32();
        r.skip(16);
        buff.counters = u32();
    }
    r.seek(0x34e4);
    for (auto* value : {&v.platinum, &v.gold, &v.silver, &v.copper,
                        &v.platinum_cursor, &v.gold_cursor, &v.silver_cursor, &v.copper_cursor})
        *value = u32();
    r.seek(0x3510);
    v.thirst = u32();
    v.hunger = u32();
    v.aa_points_spent = u32();
    r.seek(0x3530);
    v.aa_points = u32();

    const auto read_item_reference = [&](ProfileBeltItem& item) {
        item.id = u32();
        item.icon = u32();
        item.name = r.fixed_string(64);
    };
    r.seek(0x3538);
    v.bandoliers.resize(20);
    for (auto& set : v.bandoliers) {
        set.name = r.fixed_string(32);
        for (auto& item : set.items) read_item_reference(item);
    }
    v.potion_belt.resize(5);
    for (auto& item : v.potion_belt) read_item_reference(item);

    v.inventory_slot_mask = u32();
    for (auto* value : {&v.base_hp_total, &v.base_endurance_total, &v.base_mana_total, &v.base_save_cold,
                        &v.base_save_fire, &v.base_save_magic, &v.base_save_disease, &v.base_save_poison,
                        &v.base_save_physical, &v.base_save_corruption})
        *value = i32();
    for (auto& value : v.profile_344c) value = i32();
    v.base_endurance_regen = i32();
    for (auto& value : v.profile_3464) value = i32();
    v.endurance = u32();
}

AppPacket shroud_id_request(Op op, std::uint32_t selection_id) {
    ByteWriter w;
    w.le(selection_id);
    return make_packet(op, std::move(w));
}

AppPacket shroud_detail_request(Op op, std::uint32_t selection_id, std::uint32_t detail_id) {
    ByteWriter w;
    w.le(selection_id);
    w.le(detail_id);
    return make_packet(op, std::move(w));
}

AppPacket mission_detail_request(Op op, std::uint32_t template_id, std::uint32_t detail_id) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le(template_id);
    w.le(detail_id);
    return make_packet(op, std::move(w));
}

} // namespace

std::optional<ShroudSelection> decode_shroud_selection(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        ShroudSelection value;
        value.context_id = r.le<std::uint32_t>();
        value.status = r.le<std::uint32_t>();
        r.skip(4);
        if (value.status != 0) return value;
        const auto text = read_text(r);
        if (text.empty()) return value;

        Fields fields(text, '\x07');
        std::unordered_set<std::uint32_t> ids;
        while (!fields.done()) {
            const auto kind = fields.next();
            if (kind == "PROGRESSION") {
                auto& category = value.categories.emplace_back();
                category.id = fields.number();
                category.name = fields.next();
                category.description = fields.next();
            } else if (kind == "BRANCH") {
                require(!value.categories.empty());
                auto& branch = value.categories.back().branches.emplace_back();
                branch.id = fields.number();
                branch.name = fields.next();
                branch.description = fields.next();
                branch.progress = fields.number();
            } else if (kind == "TEMPLATE") {
                require(!value.categories.empty() && !value.categories.back().branches.empty());
                auto& form = value.categories.back().branches.back().forms.emplace_back();
                form.name = fields.next();
                form.selection_id = fields.number();
                form.level = fields.number();
                form.unlocked = fields.number() != 0;
                require(!form.unlocked || (form.selection_id != 0 && ids.emplace(form.selection_id).second));
            } else {
                throw std::out_of_range("unknown shroud selection record");
            }
        }
        return value;
    });
}

std::optional<ShroudStats> decode_shroud_stats(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        const auto selection_id = r.le<std::uint32_t>();
        if (r.le<std::uint32_t>() == 0) {
            require(r.empty() || (r.remaining() == 1 && r.le<std::uint8_t>() == 0));
            ShroudStats value;
            value.selection_id = selection_id;
            return value;
        }
        auto value = read_stats_text(read_text(r));
        require(value.selection_id == selection_id);
        return value;
    });
}

std::optional<std::vector<ShroudProgress>> decode_shroud_progress(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        auto count = r.le<std::uint32_t>();
        std::vector<ShroudProgress> result;
        if (count == 0) {
            require(r.empty());
            return result;
        }
        const auto bytes = r.bytes(r.remaining());
        require(!bytes.empty());
        std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        Fields fields(text, '\x07');
        for (std::uint32_t i = 0; i < count; ++i) {
            require(fields.next() == "PROGRESSION");
            auto& row = result.emplace_back();
            row.category_id = fields.number();
            row.name = fields.next();
            row.description = fields.next();
            row.unlocked = fields.number();
            row.total = fields.number();
            row.next_name = fields.next();
            row.next_description = fields.next();
            row.percent = fields.number();
            require(row.unlocked <= row.total && row.percent <= 100);
        }
        require(fields.done());
        return result;
    });
}

std::optional<ShroudAbilityReply> decode_shroud_ability(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        ShroudAbilityReply value{r.le<std::uint32_t>(), r.le<std::uint32_t>()};
        require(r.empty());
        return value;
    });
}

std::optional<ShroudItemReply> decode_shroud_item(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        const auto selection_id = r.le<std::uint32_t>();
        return read_item_reply(r, selection_id);
    });
}

std::optional<ShroudTransition> decode_shroud_transition(const AppPacket& p, const PlayerProfile* previous) {
    return decode_with(p, [&](ByteReader& r) {
        const auto spawn_id = r.le<std::uint32_t>();
        const auto profile_offset = r.le<std::uint16_t>();
        require(profile_offset > r.position());
        const auto spawn_body = r.bytes(profile_offset - r.position());
        auto spawn = decode_spawn({to_wire(Op::ZoneEntry), {spawn_body.begin(), spawn_body.end()}});
        require(spawn && spawn_id == spawn->spawn_id && spawn->npc == 0);

        ShroudTransition transition;
        transition.spawn = std::move(*spawn);
        if (r.empty()) return transition;

        ByteReader profile(r.bytes(kShroudProfileBytes));
        transition.profile = previous ? *previous : PlayerProfile{};
        read_shroud_profile(profile, *transition.profile);
        transition.profile->name = transition.spawn.name;
        transition.profile->last_name = transition.spawn.last_name;
        transition.profile->x = transition.spawn.motion.x;
        transition.profile->y = transition.spawn.motion.y;
        transition.profile->z = transition.spawn.motion.z;
        transition.profile->heading = transition.spawn.motion.heading;
        if (r.empty()) return transition; // Native permits no inventory block for an empty profile.
        const auto items = r.bytes(r.remaining());
        auto possessions = decode_char_inventory({to_wire(Op::CharInventory), {items.begin(), items.end()}});
        require(possessions.has_value());
        transition.possessions = std::move(*possessions);
        return transition;
    });
}

AppPacket encode_shroud_request_stats(std::uint32_t selection_id) {
    return shroud_id_request(Op::ShroudRequestStats, selection_id);
}

AppPacket encode_shroud_select(std::uint32_t selection_id) {
    return shroud_id_request(Op::ShroudSelect, selection_id);
}

AppPacket encode_shroud_cancel() { return make_empty(Op::ShroudSelectCancel); }

AppPacket encode_shroud_request_ability(std::uint32_t selection_id, std::uint32_t rank_id) {
    return shroud_detail_request(Op::ShroudRequestAbility, selection_id, rank_id);
}

AppPacket encode_shroud_request_item(std::uint32_t selection_id, std::uint32_t item_id) {
    return shroud_detail_request(Op::ShroudRequestItem, selection_id, item_id);
}

std::optional<MonsterMissionCatalog> decode_monster_mission_catalog(const AppPacket& p) {
    // 0x482ae0 decodes the list that 0x481e80 attaches to the active shared task.
    return decode_with(p, [](ByteReader& r) {
        MonsterMissionCatalog value;
        r.skip(8);
        value.task_id = r.le<std::uint32_t>();
        r.skip(4);
        value.selection_locked = r.le<std::uint8_t>() != 0;
        const auto count = r.le<std::uint32_t>();
        const auto min_record_bytes = 4 * sizeof(std::uint32_t) + 2;
        require(value.task_id != 0 && count <= r.remaining() / min_record_bytes);
        value.templates.reserve(count);
        std::unordered_set<std::uint32_t> ids;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto& form = value.templates.emplace_back();
            form.id = r.le<std::uint32_t>();
            form.minimum = r.le<std::int32_t>();
            form.maximum = r.le<std::int32_t>();
            r.skip(4);
            form.selectable = r.le<std::uint8_t>() != 0;
            form.name = r.cstring();
            require(form.id != 0 && ids.emplace(form.id).second);
        }
        require(r.empty());
        return value;
    });
}

std::optional<ShroudStats> decode_monster_mission_stats(const AppPacket& p) {
    // 0x4822d2 skips the four-dword header before passing the text to 0x5ca570.
    return decode_with(p, [](ByteReader& r) {
        r.skip(16);
        const auto text = r.cstring();
        while (!r.empty()) require(r.le<std::uint8_t>() == 0);
        return read_stats_text(text);
    });
}

std::optional<ShroudAbilityReply> decode_monster_mission_ability(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        r.skip(8);
        ShroudAbilityReply value{r.le<std::uint32_t>(), r.le<std::uint32_t>()};
        require(r.empty());
        return value;
    });
}

std::optional<ShroudItemReply> decode_monster_mission_item(const AppPacket& p) {
    // 0x48250c reads the item ID and passes the remaining cursor to the item reader.
    return decode_with(p, [](ByteReader& r) { return read_item_reply(r, 0); });
}

std::optional<MonsterMissionAssignment> decode_monster_mission_assignment(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        r.skip(8);
        MonsterMissionAssignment value{r.fixed_string(64), r.le<std::uint32_t>()};
        require(r.empty() && !value.name.empty());
        return value;
    });
}

std::optional<MonsterMissionResult> decode_monster_mission_result(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        r.skip(8);
        MonsterMissionResult value{r.le<std::uint32_t>(), r.le<std::uint32_t>()};
        require(r.empty());
        return value;
    });
}

std::uint32_t MonsterMissionResult::message_id() const {
    constexpr std::array<std::uint32_t, 7> messages{3610, 3609, 3613, 3611, 3612, 3614, 3621};
    return result < messages.size() ? messages[result] : messages[2];
}

AppPacket encode_monster_mission_catalog_request() {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    return make_packet(Op::MonsterMissionCatalog, std::move(w));
}

AppPacket encode_monster_mission_stats_request(std::uint32_t template_id) {
    // 0x76df53 sends four dwords followed by a zero-filled 4096-byte buffer.
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le(template_id);
    w.le<std::uint32_t>(0);
    w.zeros(4096);
    return make_packet(Op::MonsterMissionStats, std::move(w));
}

AppPacket encode_monster_mission_ability_request(std::uint32_t template_id, std::uint32_t rank_id) {
    return mission_detail_request(Op::MonsterMissionAbility, template_id, rank_id);
}

AppPacket encode_monster_mission_item_request(std::uint32_t template_id, std::uint32_t item_id) {
    return mission_detail_request(Op::MonsterMissionItem, template_id, item_id);
}

AppPacket encode_monster_mission_select(std::uint32_t template_id) {
    ByteWriter w;
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le(template_id);
    return make_packet(Op::MonsterMissionSelect, std::move(w));
}

std::optional<AltStorageControl> decode_alt_storage(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        AltStorageControl value;
        value.action = static_cast<AltStorageAction>(r.le<std::uint32_t>());
        switch (value.action) {
        case AltStorageAction::Open:
            value.entity_id = r.le<std::uint32_t>();
            break;
        case AltStorageAction::Close:
        case AltStorageAction::Refresh:
            break;
        case AltStorageAction::Withdraw:
            r.skip(4);
            value.storage_key = r.le<std::uint64_t>();
            break;
        case AltStorageAction::Remove:
            r.skip(4);
            value.storage_key = static_cast<std::uint64_t>(r.le<std::int32_t>());
            break;
        case AltStorageAction::Inspect:
            r.skip(4);
            value.storage_key = r.le<std::uint64_t>();
            value.modifier = r.le<std::uint32_t>();
            r.skip(4);
            break;
        case AltStorageAction::Currency:
            for (auto* field : {&value.platinum, &value.gold, &value.silver, &value.copper})
                *field = r.le<std::uint32_t>();
            break;
        default:
            throw std::out_of_range("unknown shroud bank action");
        }
        require(r.empty());
        return value;
    });
}

std::optional<AltStorageList> decode_alt_storage_list(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        AltStorageList value;
        value.retention_seconds = r.le<std::uint32_t>();
        const auto count = r.le<std::uint32_t>();
        const auto min_entry_bytes = 1 + sizeof(std::uint64_t) + 2 * sizeof(std::uint32_t);
        require(count <= r.remaining() / min_entry_bytes);
        value.entries.reserve(count);
        std::unordered_set<std::uint64_t> keys;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto& entry = value.entries.emplace_back();
            entry.name = r.cstring();
            entry.icon = r.le<std::uint32_t>();
            entry.key = r.le<std::uint64_t>();
            entry.quantity = r.le<std::uint32_t>();
            require(entry.key != 0 && keys.emplace(entry.key).second);
        }
        require(r.empty());
        return value;
    });
}

AppPacket encode_alt_storage_open(std::uint32_t entity_id) {
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(AltStorageAction::Open));
    w.le(entity_id);
    return make_packet(Op::AltStorage, std::move(w));
}

AppPacket encode_alt_storage_withdraw(std::uint64_t key) {
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(AltStorageAction::Withdraw));
    w.le<std::uint32_t>(0);
    w.le(key);
    return make_packet(Op::AltStorage, std::move(w));
}

AppPacket encode_alt_storage_inspect(std::uint64_t key, std::uint32_t modifier) {
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(AltStorageAction::Inspect));
    w.le<std::uint32_t>(0);
    w.le(key);
    w.le(modifier);
    w.le<std::uint32_t>(0);
    return make_packet(Op::AltStorage, std::move(w));
}

} // namespace hailstone::net
