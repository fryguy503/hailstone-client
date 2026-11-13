// Written from EQEmu rof2.cpp/rof2_structs.h (read as reference, no code copied)
// and the original client; OpenEQ crates/openeq-net/src/zone.rs was studied to
// cross-check the OP_ClientUpdate offsets (THIRD_PARTY.md).

#include "net/zone_packets.h"

#include "game/mechanics/remote_spawns.h"

#include <cmath>
#include <stdexcept>

#include "net/packet_io.h"

namespace hailstone::net {

using detail::bits_s;
using detail::bits_u;
using detail::decode_with;
using detail::put_bits;
using detail::write_cstring;

namespace {

bool exact(const AppPacket& p, std::size_t n) { return p.data.size() == n; }

// common/misc_functions.cpp FloatToEQ19/13/10 (truncating casts) and FloatToEQ12.
std::int64_t to_fixed(float v, float scale) {
    float s = v * scale;
    if (!std::isfinite(s)) return 0;
    return static_cast<std::int64_t>(s);
}
std::int64_t heading_to_fixed(float h) {
    if (!std::isfinite(h)) return 0;
    auto v = static_cast<std::int64_t>((h + 2048.0f) * 4.0f) % 2048;
    return v < 0 ? v + 2048 : v;
}
float from_fixed(std::int32_t v, float scale) { return static_cast<float>(v) / scale; }

constexpr float kCoord = 8.0f, kDelta = 64.0f, kHeading = 4.0f, kDeltaHeading = 20.0f;

// Throws when `count` elements of `elem` bytes cannot fit in what is left.
void need(const ByteReader& r, std::uint64_t count, std::size_t elem) {
    if (elem && count > r.remaining() / elem) throw std::out_of_range("count exceeds payload");
}

std::vector<std::uint32_t> read_u32_array(ByteReader& r) {
    std::uint32_t n = r.le<std::uint32_t>();
    need(r, n, 4);
    std::vector<std::uint32_t> v(n);
    for (auto& x : v) x = r.le<std::uint32_t>();
    return v;
}

void skip_counted(ByteReader& r, std::size_t elem) {
    std::uint32_t n = r.le<std::uint32_t>();
    need(r, n, elem);
    r.skip(static_cast<std::size_t>(n) * elem);
}

// Spawn_Struct_Position (rof2_structs.h), five little-endian dwords.

// PlayerPositionUpdateServer_Struct after spawn_id/vehicle_id. Spawns use the same
// layout (the active Spawn_Struct_Position in rof2_structs.h; the commented-out
// one above it is an older layout, confirmed wrong against the live server).
void write_update_position(ByteWriter& w, const Motion& m) {
    w.le(put_bits(to_fixed(m.y, kCoord), 12, 19));
    w.le(put_bits(to_fixed(m.delta_z, kDelta), 0, 13) | put_bits(to_fixed(m.delta_x, kDelta), 13, 13));
    w.le(put_bits(to_fixed(m.x, kCoord), 0, 19) | put_bits(heading_to_fixed(m.heading), 19, 12));
    w.le(put_bits(to_fixed(m.delta_heading, kDeltaHeading), 0, 10) | put_bits(to_fixed(m.z, kCoord), 10, 19));
    w.le(put_bits(m.animation, 0, 10) | put_bits(to_fixed(m.delta_y, kDelta), 10, 13));
}

Motion motion_from_packed(game::mechanics::PackedPosition w) {
    // unpack_physics @0x4ee6c0 (game/mechanics/remote_spawns.h): signed fields times their units, as the
    // original stores them (the heading is signed, the turn rate is x 0.05).
    game::mechanics::clear_stopped_motion(w);
    game::mechanics::SpawnPhysics p;
    game::mechanics::unpack_spawn_position(p, w, {});
    Motion m;
    m.y = p.y;
    m.x = p.x;
    m.z = p.z;
    m.delta_y = p.speed_y;
    m.delta_x = p.speed_x;
    m.delta_z = p.speed_z;
    m.heading = p.heading;
    m.delta_heading = p.speed_heading;
    m.pitch = p.pitch;
    m.speed_run = p.speed_run;
    m.animation = bits_s(w[4], 0, 10);
    return m;
}

Motion read_update_position(ByteReader& r) {
    game::mechanics::PackedPosition w;
    for (auto& x : w) x = r.le<std::uint32_t>();
    return motion_from_packed(w);
}

} // namespace

// ---------------------------------------------------------------- client → zone

AppPacket encode(const ZoneEntryRequest& v) {
    ByteWriter w;
    std::uint32_t crc = v.name_crc;
    if (crc == 0) crc = eq_crc32({reinterpret_cast<const std::byte*>(v.name.data()), v.name.size()});
    w.le(crc);
    w.fixed_string(std::string_view(v.name).substr(0, 63), 64);
    w.le(v.spell_crc);
    w.le(v.spell_assoc_crc);
    return make_packet(Op::ZoneEntry, std::move(w));
}

std::optional<ZoneEntryRequest> decode_zone_entry_request(const AppPacket& p) {
    if (!exact(p, kZoneEntryRequestSize)) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ZoneEntryRequest v;
        v.name_crc = r.le<std::uint32_t>();
        v.name = r.fixed_string(64);
        v.spell_crc = r.le<std::uint32_t>();
        v.spell_assoc_crc = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const ClientPositionUpdate& v) {
    const Motion& m = v.motion;
    ByteWriter w;
    w.le(v.sequence);
    w.le(v.spawn_id);
    w.le(v.vehicle_id);
    w.zeros(4);
    w.f32(m.delta_x);
    w.le(put_bits(heading_to_fixed(m.heading), 0, 12));
    w.f32(m.x);
    w.f32(m.delta_z);
    w.f32(m.z);
    w.f32(m.y);
    w.le(put_bits(m.animation, 0, 10));
    w.f32(m.delta_y);
    w.le(put_bits(to_fixed(m.delta_heading, kDeltaHeading), 0, 10));
    return make_packet(Op::ClientUpdate, std::move(w));
}

std::optional<ClientPositionUpdate> decode_client_position_update(const AppPacket& p) {
    // Zone accepts 46 or 47 bytes (Handle_OP_ClientUpdate).
    if (p.data.size() != kClientPositionUpdateSize && p.data.size() != kClientPositionUpdateSize + 1)
        return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ClientPositionUpdate v;
        Motion& m = v.motion;
        v.sequence = r.le<std::uint16_t>();
        v.spawn_id = r.le<std::uint16_t>();
        v.vehicle_id = r.le<std::uint16_t>();
        r.skip(4);
        m.delta_x = r.f32();
        m.heading = static_cast<float>(bits_u(r.le<std::uint32_t>(), 0, 12)) / kHeading;
        m.x = r.f32();
        m.delta_z = r.f32();
        m.z = r.f32();
        m.y = r.f32();
        m.animation = static_cast<std::int32_t>(bits_u(r.le<std::uint32_t>(), 0, 10));
        m.delta_y = r.f32();
        m.delta_heading = from_fixed(bits_s(r.le<std::uint32_t>(), 0, 10), kDeltaHeading);
        return v;
    });
}

AppPacket encode_camp(std::uint32_t kind) {
    ByteWriter w;
    w.le(kind);
    return make_packet(Op::Camp, std::move(w));
}

namespace {

AppPacket encode_save_packet(Op op, const SaveRequest& v) {
    ByteWriter d; // the 476 checked bytes (0x77 dwords)
    d.f32(v.y);
    d.f32(v.x);
    d.f32(v.z);
    d.le(v.heading);
    for (const auto& a : v.armor) {
        d.le(a.material);
        d.le(a.unknown1);
        d.le(a.elite_material);
        d.le(a.hero_forge_model);
        d.le(a.material2);
    }
    d.le<std::uint8_t>(v.vehicle ? 1 : 0);
    d.zeros(3);
    d.le(v.minutes_played);
    d.le(v.primary);
    d.le(v.secondary);
    d.le(v.air_supply);
    std::vector<std::byte> data = d.take();
    ByteWriter w;
    w.le(eq_crc32(data));
    w.le(v.kind);
    w.bytes(data);
    w.zeros(kSaveRequestSize - 5 - data.size());
    return make_packet(op, std::move(w));
}

} // namespace

AppPacket encode_save(const SaveRequest& v) { return encode_save_packet(Op::Save, v); }
AppPacket encode_save_on_zone(const SaveRequest& v) { return encode_save_packet(Op::SaveOnZoneReq, v); }

std::optional<SaveRequest> decode_save(const AppPacket& p) {
    if (p.data.size() != kSaveRequestSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        SaveRequest v;
        r.le<std::uint32_t>();
        v.kind = r.le<std::uint8_t>();
        v.y = r.f32();
        v.x = r.f32();
        v.z = r.f32();
        v.heading = r.le<std::int32_t>();
        for (auto& a : v.armor) {
            a.material = r.le<std::uint32_t>();
            a.unknown1 = r.le<std::uint32_t>();
            a.elite_material = r.le<std::uint32_t>();
            a.hero_forge_model = r.le<std::uint32_t>();
            a.material2 = r.le<std::uint32_t>();
        }
        v.vehicle = r.le<std::uint8_t>() != 0;
        r.skip(3);
        v.minutes_played = r.le<std::uint32_t>();
        v.primary = r.le<std::int32_t>();
        v.secondary = r.le<std::int32_t>();
        v.air_supply = r.le<std::int32_t>();
        return v;
    });
}

// ---------------------------------------------------------------- spawns

bool is_pc_model_race(std::uint32_t race) {
    return race <= 12 || race == 128 || race == 130 || race == 330 || race == 522;
}

namespace {
bool interactive_form(std::uint8_t other) { return (other & 0x04) != 0 || (other & 0xe0) == 0xe0; }
bool destructible_form(std::uint8_t other) { return (other & 0xe0) == 0xe0; }
} // namespace

AppPacket encode(const Spawn& v) {
    // Mirrors rof2.cpp ENCODE(OP_ZoneSpawns) for one spawn.
    ByteWriter w;
    write_cstring(w, v.name);
    w.le(v.spawn_id);
    w.le(v.level);
    w.f32(v.melee_radius);
    w.le(v.npc);
    std::uint32_t bf = v.bitfield;
    if (bf == 0) {
        bf = put_bits(v.gender, 0, 2) | put_bits(v.is_pet, 2, 1) | put_bits(v.afk, 3, 1) | put_bits(v.anon, 4, 2) |
             put_bits(v.gm, 6, 1) | put_bits(v.sneak, 7, 1) | put_bits(v.lfg, 8, 1) | put_bits(v.invis, 10, 12) |
             put_bits(v.linkdead, 22, 1) | put_bits(v.show_helm, 23, 1) | put_bits(v.trader, 25, 1) |
             put_bits(v.targetable, 27, 1) | put_bits(v.targetable_with_hotkey, 28, 1) |
             put_bits(v.show_name, 29, 1);
    }
    w.le(bf);
    std::uint8_t other = v.other_data;
    if (!v.title.empty()) other = static_cast<std::uint8_t>(other | 0x10);
    if (!v.suffix.empty()) other = static_cast<std::uint8_t>(other | 0x20);
    if (v.interactive) other = static_cast<std::uint8_t>(other | 0x04);
    w.le(other);
    w.f32(v.emitter_scaling_radius);
    w.le(v.default_emitter_id);
    if (interactive_form(other)) {
        const InteractiveObject io = v.interactive.value_or(InteractiveObject{});
        write_cstring(w, io.model);
        write_cstring(w, io.name2);
        write_cstring(w, io.string);
        w.le(io.appearance);
        w.le(io.animation_id);
        for (auto s : io.sound_ids) w.le(s);
        w.le(io.collidable);
        w.le(io.object_type);
    }
    if (destructible_form(other)) {
        w.le<std::uint8_t>(0);
    } else {
        w.le(static_cast<std::uint8_t>(v.properties.empty() ? 1 : v.properties.size()));
        if (v.properties.empty())
            w.le(v.body_type);
        else
            for (auto x : v.properties) w.le(x);
    }
    for (auto b : {v.hp_percent, v.hair_color, v.beard_color, v.eye_color1, v.eye_color2, v.hair_style, v.beard})
        w.le(b);
    w.le(v.drakkin_heritage);
    w.le(v.drakkin_tattoo);
    w.le(v.drakkin_details);
    for (auto b : {v.texture_type, v.material, v.variation, v.head_type}) w.le(b);
    w.f32(v.size);
    w.le(v.face);
    w.f32(v.walk_speed);
    w.f32(v.run_speed);
    w.le(v.race);
    w.le(v.holding);
    w.le(v.deity);
    w.le(v.guild_id);
    w.le(v.guild_rank);
    for (auto b : {v.class_, v.pvp, v.stand_state, v.light, v.fly_mode}) w.le(b);
    write_cstring(w, v.last_name);
    w.le(v.aa_title);
    w.le(v.guild_show);
    w.le(v.temp_pet);
    w.le(v.pet_owner_id);
    w.le(v.find_bits);
    for (auto x : {v.player_state, v.npc_tint_id, v.primary_tint, v.secondary_tint, v.weapon_tint1, v.weapon_tint2})
        w.le(x);
    auto texture = [&](const EquipTexture& t) {
        for (auto x : {t.material, t.unknown1, t.elite_material, t.hero_forge_model, t.material2}) w.le(x);
    };
    if (v.long_equipment) {
        for (auto c : v.equipment_tint) w.le(c);
        for (const auto& t : v.equipment) texture(t);
    } else {
        texture(EquipTexture{});
        texture(v.equipment[7]);
        texture(v.equipment[8]);
    }
    write_update_position(w, v.motion);
    if (other & 0x10) write_cstring(w, v.title);
    if ((other & 0x20) && !destructible_form(other)) write_cstring(w, v.suffix);
    w.le(v.spline1);
    w.le(v.spline2);
    w.le(v.is_mercenary);
    write_cstring(w, v.real_estate_guid.empty() ? std::string_view("0000000000000000") : v.real_estate_guid);
    w.le(v.real_estate_id);
    w.le(v.real_estate_item_id);
    w.le(v.always_show_aura);
    w.le(static_cast<std::uint32_t>(v.physics_effects.size()));
    for (const auto& e : v.physics_effects)
        for (auto x : e) w.le(x);
    for (auto x : v.spawn_status) w.le(x);
    return make_packet(Op::ZoneEntry, std::move(w));
}

std::optional<Spawn> decode_spawn(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        Spawn v;
        v.name = r.cstring();
        v.spawn_id = r.le<std::uint32_t>();
        v.level = r.le<std::uint8_t>();
        v.melee_radius = r.f32();
        v.npc = r.le<std::uint8_t>();
        std::uint32_t bf = v.bitfield = r.le<std::uint32_t>();
        v.gender = static_cast<std::uint8_t>(bits_u(bf, 0, 2));
        v.is_pet = bits_u(bf, 2, 1);
        v.afk = bits_u(bf, 3, 1);
        v.anon = static_cast<std::uint8_t>(bits_u(bf, 4, 2));
        v.gm = bits_u(bf, 6, 1);
        v.sneak = bits_u(bf, 7, 1);
        v.lfg = bits_u(bf, 8, 1);
        v.invis = static_cast<std::uint16_t>(bits_u(bf, 10, 12));
        v.linkdead = bits_u(bf, 22, 1);
        v.show_helm = bits_u(bf, 23, 1);
        v.trader = bits_u(bf, 25, 1);
        v.targetable = bits_u(bf, 27, 1);
        v.targetable_with_hotkey = bits_u(bf, 28, 1);
        v.show_name = bits_u(bf, 29, 1);
        std::uint8_t other = v.other_data = r.le<std::uint8_t>();
        v.emitter_scaling_radius = r.f32();
        v.default_emitter_id = r.le<std::uint32_t>();
        if (interactive_form(other)) {
            InteractiveObject io;
            io.model = r.cstring();
            io.name2 = r.cstring();
            io.string = r.cstring();
            io.appearance = r.le<std::uint32_t>();
            io.animation_id = r.le<std::uint32_t>();
            for (auto& s : io.sound_ids) s = r.le<std::uint32_t>();
            io.collidable = r.le<std::uint8_t>();
            io.object_type = r.le<std::uint32_t>();
            v.interactive = std::move(io);
        }
        std::uint8_t props = r.le<std::uint8_t>();
        need(r, props, 4);
        v.properties.resize(props);
        for (auto& x : v.properties) x = r.le<std::uint32_t>();
        if (!v.properties.empty()) v.body_type = v.properties[0];
        for (std::uint8_t* b :
             {&v.hp_percent, &v.hair_color, &v.beard_color, &v.eye_color1, &v.eye_color2, &v.hair_style, &v.beard})
            *b = r.le<std::uint8_t>();
        v.drakkin_heritage = r.le<std::uint32_t>();
        v.drakkin_tattoo = r.le<std::uint32_t>();
        v.drakkin_details = r.le<std::uint32_t>();
        for (std::uint8_t* b : {&v.texture_type, &v.material, &v.variation, &v.head_type}) *b = r.le<std::uint8_t>();
        v.size = r.f32();
        v.face = r.le<std::uint8_t>();
        v.walk_speed = r.f32();
        v.run_speed = r.f32();
        v.race = r.le<std::uint32_t>();
        v.holding = r.le<std::uint8_t>();
        v.deity = r.le<std::uint32_t>();
        v.guild_id = r.le<std::uint32_t>();
        v.guild_rank = r.le<std::uint32_t>();
        for (std::uint8_t* b : {&v.class_, &v.pvp, &v.stand_state, &v.light, &v.fly_mode}) *b = r.le<std::uint8_t>();
        v.last_name = r.cstring();
        v.aa_title = r.le<std::uint32_t>();
        v.guild_show = r.le<std::uint8_t>();
        v.temp_pet = r.le<std::uint8_t>();
        v.pet_owner_id = r.le<std::uint32_t>();
        v.find_bits = r.le<std::uint8_t>();
        for (std::uint32_t* x : {&v.player_state, &v.npc_tint_id, &v.primary_tint, &v.secondary_tint,
                                 &v.weapon_tint1, &v.weapon_tint2})
            *x = r.le<std::uint32_t>();
        auto texture = [&](EquipTexture& t) {
            for (std::uint32_t* x : {&t.material, &t.unknown1, &t.elite_material, &t.hero_forge_model, &t.material2})
                *x = r.le<std::uint32_t>();
        };
        // The client picks the form from the race and the spawn type
        // (UnSerializeSpawn): PC-model races or type 0 get the long block.
        v.long_equipment = v.npc == 0 || is_pc_model_race(v.race);
        if (v.long_equipment) {
            for (auto& c : v.equipment_tint) c = r.le<std::uint32_t>();
            for (auto& t : v.equipment) texture(t);
        } else {
            EquipTexture slot0;
            texture(slot0);
            texture(v.equipment[7]);
            texture(v.equipment[8]);
        }
        v.motion = read_update_position(r);
        if (other & 0x10) v.title = r.cstring();
        if ((other & 0x20) && !destructible_form(other)) v.suffix = r.cstring();
        v.spline1 = r.le<std::uint32_t>();
        v.spline2 = r.le<std::uint32_t>();
        v.is_mercenary = r.le<std::uint8_t>();
        v.real_estate_guid = r.cstring();
        v.real_estate_id = r.le<std::uint32_t>();
        v.real_estate_item_id = r.le<std::uint32_t>();
        v.always_show_aura = r.le<std::uint8_t>();
        std::uint32_t effects = r.le<std::uint32_t>();
        need(r, effects, 16);
        v.physics_effects.resize(effects);
        for (auto& e : v.physics_effects)
            for (auto& x : e) x = r.le<std::uint32_t>();
        for (auto& x : v.spawn_status) x = r.le<std::uint32_t>();
        // Interactive objects of type 4 carry 4 more dwords (banners); EQEmu
        // never sends them. Anything else left over is ignored.
        return v;
    });
}

AppPacket encode(const SpawnPositionUpdate& v) {
    ByteWriter w;
    w.le(v.spawn_id);
    w.le(v.vehicle_id);
    write_update_position(w, v.motion);
    return make_packet(Op::ClientUpdate, std::move(w));
}

std::optional<SpawnPositionUpdate> decode_spawn_position_update(const AppPacket& p) {
    if (p.data.size() < kSpawnPositionUpdateSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        SpawnPositionUpdate v;
        v.spawn_id = r.le<std::uint16_t>();
        v.vehicle_id = r.le<std::uint16_t>();
        v.motion = read_update_position(r);
        return v;
    });
}

std::optional<SpawnPositionUpdate> decode_npc_move_update(const AppPacket& p) {
    auto record = game::mechanics::unpack_npc_move_update(
        {reinterpret_cast<const std::uint8_t*>(p.data.data()), p.data.size()});
    SpawnPositionUpdate v;
    v.spawn_id = static_cast<std::uint16_t>(record[0]);
    v.vehicle_id = static_cast<std::uint16_t>(record[0] >> 16);
    v.motion = motion_from_packed({record[1], record[2], record[3], record[4], record[5]});
    return v;
}

// ---------------------------------------------------------------- appearance

std::optional<WearChange> decode_wear_change(const AppPacket& p) {
    if (p.data.size() < 27) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        WearChange v;
        v.spawn_id = r.le<std::uint16_t>();
        for (std::uint32_t* x : {&v.armor.material, &v.armor.unknown1, &v.armor.elite_material,
                                 &v.armor.hero_forge_model, &v.armor.material2})
            *x = r.le<std::uint32_t>();
        v.tint = r.le<std::uint32_t>();
        v.slot = r.le<std::uint8_t>();
        return v;
    });
}

AppPacket encode(const WearChange& v) {
    ByteWriter w;
    w.le(v.spawn_id);
    for (auto x : {v.armor.material, v.armor.unknown1, v.armor.elite_material, v.armor.hero_forge_model,
                   v.armor.material2})
        w.le(x);
    w.le(v.tint);
    w.le(v.slot);
    return make_packet(Op::WearChange, std::move(w));
}

std::optional<Illusion> decode_illusion(const AppPacket& p) {
    if (p.data.size() < 256) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        Illusion v;
        v.spawn_id = r.le<std::uint32_t>();
        r.skip(64); // name
        v.race = r.le<std::uint16_t>();
        r.skip(2);
        v.gender = r.le<std::uint8_t>();
        v.texture = r.le<std::uint8_t>();
        r.skip(2);
        v.helm = r.le<std::uint8_t>();
        r.skip(3);
        v.face = r.le<std::uint32_t>();
        v.hair_style = r.le<std::uint8_t>();
        v.hair_color = r.le<std::uint8_t>();
        v.beard = r.le<std::uint8_t>();
        v.beard_color = r.le<std::uint8_t>();
        v.size = r.f32();
        r.skip(148 + 4);
        v.drakkin_heritage = r.le<std::uint32_t>();
        v.drakkin_tattoo = r.le<std::uint32_t>();
        v.drakkin_details = r.le<std::uint32_t>();
        return v;
    });
}

// ---------------------------------------------------------------- player profile

std::optional<PlayerProfile> decode_player_profile(const AppPacket& p) {
    // Follows rof2.cpp ENCODE(OP_PlayerProfile) field by field. Arrays are
    // read with their own count prefix, so a server with other array sizes
    // still parses.
    return decode_with(p, [](ByteReader& r) {
        PlayerProfile v;
        auto u8 = [&] { return r.le<std::uint8_t>(); };
        auto u32 = [&] { return r.le<std::uint32_t>(); };
        auto skip_u32 = [&](std::size_t n) { r.skip(n * 4); };

        v.checksum = u32();
        v.checksum_size = u32();
        skip_u32(2);
        v.gender = u8();
        v.race = u32();
        v.class_ = u8();
        v.level = u8();
        u8(); // level again

        std::uint32_t binds = u32();
        need(r, binds, 20);
        v.binds.resize(binds);
        for (auto& b : v.binds) {
            b.zone_id = u32();
            b.x = r.f32();
            b.y = r.f32();
            b.z = r.f32();
            b.heading = r.f32();
        }
        v.deity = u32();
        v.intoxication = u32();
        skip_counted(r, 4); // 10 unknown

        std::uint32_t equip = u32();
        need(r, equip, 20);
        v.equipment.resize(equip);
        for (auto& t : v.equipment)
            for (std::uint32_t* x : {&t.material, &t.unknown1, &t.elite_material, &t.hero_forge_model, &t.material2})
                *x = u32();
        skip_counted(r, 20); // Equipment2
        v.equipment_tint = read_u32_array(r);
        skip_counted(r, 4); // Tint2

        v.hair_color = u8();
        v.beard_color = u8();
        u32();
        v.eye_color1 = u8();
        v.eye_color2 = u8();
        v.hair_style = u8();
        v.beard = u8();
        v.face = u8();
        v.drakkin_heritage = u32();
        v.drakkin_tattoo = u32();
        v.drakkin_details = u32();
        r.skip(5);
        v.height = r.f32();
        r.skip(3 * 4 + 2 * 4); // three floats, primary/secondary
        v.points = u32();
        v.mana = u32();
        v.cur_hp = u32();
        v.str = u32();
        v.sta = u32();
        v.cha = u32();
        v.dex = u32();
        v.int_ = u32();
        v.agi = u32();
        v.wis = u32();
        skip_u32(7);

        std::uint32_t aas = u32();
        need(r, aas, 12);
        v.aa.resize(aas);
        for (auto& a : v.aa)
            for (auto& x : a) x = u32();
        v.skills = read_u32_array(r);
        v.innate_skills = read_u32_array(r);
        v.disciplines = read_u32_array(r);
        v.combat_ability_timers = read_u32_array(r);
        v.recast_timers = read_u32_array(r);
        v.item_recast_timers = read_u32_array(r);
        v.spell_book = read_u32_array(r);
        v.mem_spells = read_u32_array(r);
        v.gem_refresh = read_u32_array(r);
        u8();

        std::uint32_t buffs = u32();
        need(r, buffs, 80);
        v.buffs.resize(buffs);
        for (auto& b : v.buffs) {
            u8();
            b.instrument_mod = r.f32();
            b.caster_id = u32();
            u8();
            b.counters = u32();
            b.duration = u32();
            b.level = u8();
            b.spell_id = u32();
            b.effect_type = u8();
            b.num_hits = u32();
            u32();
            u32(); // counters again
            r.skip(44);
        }

        v.platinum = u32();
        v.gold = u32();
        v.silver = u32();
        v.copper = u32();
        v.platinum_cursor = u32();
        v.gold_cursor = u32();
        v.silver_cursor = u32();
        v.copper_cursor = u32();
        skip_u32(3);
        v.thirst = u32();
        v.hunger = u32();
        v.aa_points_spent = u32();
        std::uint32_t tabs = u32(); // 5: assigned, then general/class/archetype/special/unspent
        u32();
        need(r, tabs, 4);
        for (std::uint32_t i = 0; i < tabs; ++i) v.aa_points = u32();
        r.skip(2);

        std::uint32_t bandoliers = u32();
        need(r, bandoliers, 1 + 4 * 9);
        v.bandoliers.resize(bandoliers);
        for (auto& b : v.bandoliers) {
            b.name = r.cstring();
            for (auto& it : b.items) { // BANDOLIER_ITEM_COUNT: primary, secondary, range, ammo
                it.name = r.cstring();
                it.id = u32();
                it.icon = u32();
            }
        }
        std::uint32_t potions = u32();
        need(r, potions, 9);
        v.potion_belt.resize(potions);
        for (auto& it : v.potion_belt) {
            it.name = r.cstring();
            it.id = u32();
            it.icon = u32();
        }
        // The inventory slot mask (PlayerProfileData +0x4fa0, PcBase_SetInventorySlotBitmask; EQEmu sends -1).
        v.inventory_slot_mask = u32();
        auto i32 = [&] { return static_cast<std::int32_t>(u32()); };
        v.base_hp_total = i32();
        v.base_endurance_total = i32();
        v.base_mana_total = i32();
        v.base_save_cold = i32();
        v.base_save_fire = i32();
        v.base_save_magic = i32();
        v.base_save_disease = i32();
        v.base_save_poison = i32();
        v.base_save_physical = i32();
        v.base_save_corruption = i32();
        for (auto& x : v.profile_344c) x = i32();
        v.base_endurance_regen = i32();
        for (auto& x : v.profile_3464) x = i32();
        v.endurance = u32();
        skip_u32(2);

        std::uint32_t name_len = u32();
        v.name = r.fixed_string(name_len);
        std::uint32_t last_len = u32();
        v.last_name = r.fixed_string(last_len);
        v.birthday = u32();
        u32();
        v.last_login = u32();
        v.time_played_min = u32();
        u32();
        v.expansions = u32();
        std::uint32_t langs = u32();
        need(r, langs, 1);
        v.languages.resize(langs);
        for (auto& l : v.languages) l = u8();

        v.zone_id = r.le<std::uint16_t>();
        v.zone_instance = r.le<std::uint16_t>();
        v.y = r.f32();
        v.x = r.f32();
        v.z = r.f32();
        v.heading = r.f32();
        u8();
        v.pvp = u8();
        u8();
        v.gm = u8();
        v.guild_id = u32();
        v.guild_rank = u8();
        u32();
        u8();
        u32();
        v.exp = r.le<std::uint64_t>();
        u8();
        v.platinum_bank = u32();
        v.gold_bank = u32();
        v.silver_bank = u32();
        v.copper_bank = u32();
        v.platinum_shared = u32();
        skip_u32(3 + 1 + 2);
        v.career_tribute_points = u32();
        u32();
        v.tribute_points = u32();
        u32();
        r.skip(2);
        {
            std::uint32_t n = u32(); // the tributes: {tribute id (0xffffffff none), tier}
            need(r, n, 8);
            v.tributes.clear();
            for (std::uint32_t i = 0; i < n; ++i) {
                std::uint32_t id = u32();
                std::uint32_t tier = u32();
                v.tributes.push_back({id, tier});
            }
        }
        skip_counted(r, 8);  // guild tributes
        skip_u32(6);
        r.skip(125);
        skip_u32(2 + 4 + 3); // crystals
        skip_counted(r, 1);  // 64-byte string
        r.skip(3);
        skip_u32(2);
        r.skip(3);
        u32();
        r.skip(3);
        u32();
        u8();
        u32();
        skip_counted(r, 1);
        skip_counted(r, 1);
        u32();
        r.skip(320 + 343);
        v.character_id = u32();
        v.leadership_exp_on = u8() != 0; // leadAAActive
        skip_counted(r, 4);  // LDoN points
        u32();               // LDoN available
        v.group_leadership_exp = r.f64();
        v.raid_leadership_exp = r.f64();
        v.group_leadership_points = u32();
        v.raid_leadership_points = u32();
        {
            std::uint32_t n = u32(); // the leadership ability ranks (RoF2: 64, EQEmu fills 32)
            need(r, n, 4);
            v.leadership_ranks.clear();
            for (std::uint32_t i = 0; i < n; ++i) v.leadership_ranks.push_back(u32());
        }
        v.air_supply = static_cast<std::int32_t>(u32()); // air
        skip_u32(7);         // PvP stats
        for (int i = 0; i < 2; ++i) {
            r.cstring();
            skip_u32(6);
        }
        u32();
        std::uint32_t kills = u32();
        need(r, kills, 1 + 24);
        for (std::uint32_t i = 0; i < kills; ++i) {
            r.cstring();
            skip_u32(6);
        }
        v.aa_exp = u32();
        v.last_fellowship_join = u32();
        v.fellowship_vitality_normal = r.le<std::uint64_t>();
        v.fellowship_vitality_aa = u32();
        for (auto& c : v.auto_consent) c = u8(); // group, raid, guild autoconsent
        u8();       // unknown
        u32();      // level again
        v.show_helm = u8();
        v.rest_timer = u32();
        skip_counted(r, 1);
        skip_u32(2);
        u8();
        v.trailing_bytes = r.remaining();
        return v;
    });
}

// ---------------------------------------------------------------- new zone

AppPacket encode(const NewZone& v) {
    ByteWriter w;
    w.fixed_string(v.char_name, 64);
    w.fixed_string(v.zone_short_name, 128);
    w.fixed_string(v.zone_long_name, 128);
    w.zeros(150); // zone_desc[5][30]
    w.le(v.zone_type);
    for (auto c : v.fog_red) w.le(c);
    for (auto c : v.fog_green) w.le(c);
    for (auto c : v.fog_blue) w.le(c);
    w.zeros(1);
    for (auto f : v.fog_minclip) w.f32(f);
    for (auto f : v.fog_maxclip) w.f32(f);
    w.f32(v.gravity);
    w.le(v.time_type);
    for (const auto* a : {&v.rain_chance, &v.rain_duration, &v.snow_chance, &v.snow_duration})
        for (auto c : *a) w.le(c);
    for (int i = 0; i < 32; ++i) w.le<std::uint8_t>(0xff); // +537, as EQEmu
    w.le<std::uint8_t>(0);                                   // +569 time zone
    w.le(v.sky);                                             // +570
    w.zeros(1);
    w.le<std::uint32_t>(4); // water/day/night midi
    w.le<std::uint32_t>(2);
    w.le<std::uint32_t>(0);
    w.f32(v.zone_exp_multiplier); // +584
    w.f32(v.safe_y);
    w.f32(v.safe_x);
    w.f32(v.safe_z);
    w.f32(0); // +600 min_z
    w.f32(v.max_z);
    w.f32(v.underworld);
    w.f32(v.minclip); // +612
    w.f32(v.maxclip);
    w.zeros(692 - 620);
    w.le(v.old_model_scale); // +692 (0x2b4)
    w.zeros(704 - 696);
    w.fixed_string(v.zone_short_name2, 32); // +704
    w.zeros(32 + 32);                       // weather/sky strings
    w.le<std::int32_t>(-1);                 // +800 SkyRelated2
    w.zeros(32 + 4 + 4);
    w.le<std::int32_t>(600);  // +844 NPCAggroMaxDist
    w.le<std::int32_t>(2008); // +848 FilterID
    w.le(v.zone_id);          // +852
    w.le(v.zone_instance);
    w.zeros(4 + 4 + 4);
    w.le(v.underworld_teleport_index); // +868
    w.zeros(4);
    w.le(v.suspend_buffs); // +876
    w.le(v.lava_damage);
    w.le(v.min_lava_damage);
    w.le<std::uint8_t>(1); // +888 bDisallowManaStone
    w.le(v.no_bind);
    w.le(v.no_attack);
    w.zeros(3);
    w.le(v.fall_damage_off); // +894
    w.zeros(1);
    w.le(v.fast_regen_hp); // +896
    w.le(v.fast_regen_mana);
    w.le(v.fast_regen_endurance);
    w.le<std::uint32_t>(2); // campsite
    w.le<std::uint32_t>(2); // guild banner
    w.f32(v.fog_density);   // +916
    w.zeros(4 + 4 + 4);
    w.le<std::int32_t>(-1); // +932
    w.le<std::int32_t>(-1);
    w.le(v.no_levitate); // +940
    w.f32(1.0f);         // +944 blooming
    return make_packet(Op::NewZone, std::move(w));
}

std::optional<NewZone> decode_new_zone(const AppPacket& p) {
    if (p.data.size() < kNewZoneSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        NewZone v;
        v.char_name = r.fixed_string(64);
        v.zone_short_name = r.fixed_string(128);
        v.zone_long_name = r.fixed_string(128);
        r.seek(470);
        v.zone_type = r.le<std::uint8_t>();
        for (auto& c : v.fog_red) c = r.le<std::uint8_t>();
        for (auto& c : v.fog_green) c = r.le<std::uint8_t>();
        for (auto& c : v.fog_blue) c = r.le<std::uint8_t>();
        r.seek(484);
        for (auto& f : v.fog_minclip) f = r.f32();
        for (auto& f : v.fog_maxclip) f = r.f32();
        v.gravity = r.f32();
        v.time_type = r.le<std::uint8_t>();
        for (auto* a : {&v.rain_chance, &v.rain_duration, &v.snow_chance, &v.snow_duration})
            for (auto& c : *a) c = r.le<std::uint8_t>();
        r.seek(570);
        v.sky = r.le<std::uint8_t>();
        r.seek(584);
        v.zone_exp_multiplier = r.f32();
        v.safe_y = r.f32();
        v.safe_x = r.f32();
        v.safe_z = r.f32();
        r.seek(604);
        v.max_z = r.f32();
        v.underworld = r.f32();
        v.minclip = r.f32();
        v.maxclip = r.f32();
        r.seek(692);
        v.old_model_scale = r.le<std::uint32_t>();
        r.seek(704);
        v.zone_short_name2 = r.fixed_string(32);
        r.seek(852);
        v.zone_id = r.le<std::uint16_t>();
        v.zone_instance = r.le<std::uint16_t>();
        r.seek(868);
        v.underworld_teleport_index = r.le<std::uint32_t>();
        r.seek(876);
        v.suspend_buffs = r.le<std::uint32_t>();
        v.lava_damage = r.le<std::uint32_t>();
        v.min_lava_damage = r.le<std::uint32_t>();
        r.seek(889);
        v.no_bind = r.le<std::uint8_t>();
        v.no_attack = r.le<std::uint8_t>();
        r.seek(894);
        v.fall_damage_off = r.le<std::uint8_t>();
        r.seek(896);
        v.fast_regen_hp = r.le<std::uint32_t>();
        v.fast_regen_mana = r.le<std::uint32_t>();
        v.fast_regen_endurance = r.le<std::uint32_t>();
        r.seek(916);
        v.fog_density = r.f32();
        r.seek(940);
        v.no_levitate = r.le<std::uint32_t>();
        return v;
    });
}

// ---------------------------------------------------------------- small fixed packets

AppPacket encode(const TimeOfDay& v) {
    ByteWriter w;
    w.le(v.hour);
    w.le(v.minute);
    w.le(v.day);
    w.le(v.month);
    w.le(v.year);
    return make_packet(Op::TimeOfDay, std::move(w));
}

std::optional<TimeOfDay> decode_time_of_day(const AppPacket& p) {
    if (p.data.size() < 8) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        TimeOfDay v;
        v.hour = r.le<std::uint8_t>();
        v.minute = r.le<std::uint8_t>();
        v.day = r.le<std::uint8_t>();
        v.month = r.le<std::uint8_t>();
        v.year = r.le<std::uint32_t>();
        return v;
    });
}

TimeOfDay advance_time_of_day(const TimeOfDay& ref, std::uint32_t elapsed_s) {
    std::uint32_t t = elapsed_s * 20u; // game seconds, wrapping at 32 bits as the original's
    std::uint32_t years = t / 29030400u;
    t -= years * 29030400u;
    std::uint32_t months = t / 2419200u;
    t -= months * 2419200u;
    std::uint32_t days = t / 86400u;
    t -= days * 86400u;
    std::uint32_t hours = t / 3600u;
    t -= hours * 3600u;
    std::uint32_t minutes = t / 60u;

    TimeOfDay v = ref;
    auto u8 = [](std::uint32_t x) { return static_cast<std::uint8_t>(x & 0xffu); };
    v.minute = u8(minutes + ref.minute);
    while (v.minute > 59) {
        v.minute = u8(v.minute - 60u);
        ++hours;
    }
    std::uint8_t h = u8(ref.hour + hours - 1u);
    while (h > 23) {
        h = u8(h - 24u);
        ++days;
    }
    v.hour = u8(h + 1u);
    std::uint8_t d = u8(ref.day + days - 1u);
    while (d > 27) {
        d = u8(d - 28u);
        ++months;
    }
    v.day = u8(d + 1u);
    v.month = u8(ref.month + months);
    while (v.month > 12) {
        v.month = u8(v.month - 12u);
        ++years;
    }
    // The year is a signed 32-bit field there; past 9999 it is cleared.
    auto year = static_cast<std::int32_t>(ref.year + years);
    v.year = year > 9999 ? 0u : static_cast<std::uint32_t>(year);
    return v;
}

AppPacket encode(const Weather& v) {
    ByteWriter w;
    w.le(v.kind);
    w.le(v.intensity);
    w.le(v.mode);
    return make_packet(Op::Weather, std::move(w));
}

std::optional<Weather> decode_weather(const AppPacket& p) {
    if (p.data.size() < 8) return std::nullopt;
    const bool mode = p.data.size() >= 12;
    return decode_with(p, [mode](ByteReader& r) {
        Weather v;
        v.kind = r.le<std::int32_t>();
        v.intensity = r.le<std::int32_t>();
        if (mode) v.mode = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode_delete_spawn(std::uint32_t spawn_id) {
    ByteWriter w;
    w.le(spawn_id);
    w.le<std::uint8_t>(1); // "seen 1"
    return make_packet(Op::DeleteSpawn, std::move(w));
}

AppPacket encode_delete_own_spawn(std::uint32_t spawn_id) {
    ByteWriter w;
    w.le(spawn_id);
    return make_packet(Op::DeleteSpawn, std::move(w));
}

std::optional<std::uint32_t> decode_delete_spawn(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    return decode_with(p, [](ByteReader& r) { return r.le<std::uint32_t>(); });
}

AppPacket encode(const SpawnAppearance& v) {
    ByteWriter w;
    w.le(v.spawn_id);
    w.le(v.type);
    w.le(v.parameter);
    return make_packet(Op::SpawnAppearance, std::move(w));
}

std::optional<SpawnAppearance> decode_spawn_appearance(const AppPacket& p) {
    if (p.data.size() < 8) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        SpawnAppearance v;
        v.spawn_id = r.le<std::uint16_t>();
        v.type = r.le<std::uint16_t>();
        v.parameter = r.le<std::uint32_t>();
        return v;
    });
}

std::optional<ChangeSize> decode_change_size(const AppPacket& p) {
    if (p.data.size() < 8) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ChangeSize v;
        v.spawn_id = r.le<std::uint32_t>();
        v.size = r.f32();
        if (r.remaining() >= 8) {
            v.view_offset = r.f32();
            v.anim_scale = r.f32();
        }
        return v;
    });
}

AppPacket encode(const HpUpdate& v) {
    ByteWriter w;
    w.le(v.spawn_id);
    w.le(v.cur_hp);
    w.le(v.max_hp);
    return make_packet(Op::HPUpdate, std::move(w));
}

std::optional<HpUpdate> decode_hp_update(const AppPacket& p) {
    if (p.data.size() < 10) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        HpUpdate v;
        v.spawn_id = r.le<std::int16_t>();
        v.cur_hp = r.le<std::uint32_t>();
        v.max_hp = r.le<std::int32_t>();
        return v;
    });
}

std::optional<MobHealth> decode_mob_health(const AppPacket& p) {
    if (p.data.size() < 3) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        MobHealth v;
        v.spawn_id = r.le<std::uint16_t>();
        v.percent = r.le<std::uint8_t>();
        return v;
    });
}

// ---------------------------------------------------------------- chat

AppPacket encode(const ChannelMessage& v) {
    // CEverQuest::SendNewText @0x51dcd0 steve_strncpy's the sender and target into 0x40 and the text into 0x800,
    // NUL included. Stock EQEmu's DECODE(OP_ChannelMessage) strcpy's the two names into char[64] on its stack,
    // so a longer name (a quoted /tell recipient) would overrun it.
    ByteWriter w;
    write_cstring(w, std::string_view(v.sender).substr(0, 0x3f));
    write_cstring(w, std::string_view(v.target).substr(0, 0x3f));
    w.le<std::uint32_t>(0);
    w.le(v.language);
    w.le(v.channel);
    w.zeros(5);
    w.le(v.language_skill);
    write_cstring(w, std::string_view(v.message).substr(0, 0x7ff));
    w.zeros(15);
    return make_packet(Op::ChannelMessage, std::move(w));
}

std::optional<ChannelMessage> decode_channel_message(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        ChannelMessage v;
        v.sender = r.cstring();
        v.target = r.cstring();
        r.skip(4);
        v.language = r.le<std::uint32_t>();
        v.channel = r.le<std::uint32_t>();
        r.skip(5);
        v.language_skill = r.le<std::uint32_t>();
        v.message = r.cstring();
        if (r.remaining() >= 1) v.spam = r.le<std::uint8_t>() != 0;
        if (r.remaining() >= 8) {
            r.skip(4);
            v.color = r.le<std::uint32_t>();
        }
        return v;
    });
}

std::optional<SpecialMessage> decode_special_message(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        SpecialMessage v;
        v.speak_mode = r.le<std::uint8_t>();
        v.journal_mode = r.le<std::uint8_t>();
        v.language = r.le<std::uint8_t>();
        v.type = r.le<std::uint32_t>();
        v.target_spawn_id = r.le<std::uint32_t>();
        v.sayer = r.cstring();
        for (auto& x : v.loc) x = r.le<std::uint32_t>();
        v.message = r.cstring();
        return v;
    });
}

std::optional<FormattedMessage> decode_formatted_message(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        FormattedMessage v;
        v.unknown0 = r.le<std::uint32_t>();
        v.string_id = r.le<std::uint32_t>();
        v.type = r.le<std::uint32_t>();
        // Up to 9 NUL-terminated arguments, then a final NUL.
        while (!r.empty() && v.args.size() < 9) {
            std::string s = r.cstring();
            if (s.empty()) break;
            v.args.push_back(std::move(s));
        }
        return v;
    });
}

// ---------------------------------------------------------------- zoning

AppPacket encode(const ZoneChange& v) {
    ByteWriter w;
    w.fixed_string(std::string_view(v.char_name).substr(0, 63), 64);
    w.le(v.zone_id);
    w.le(v.instance_id);
    w.le(v.unknown068);
    w.le(v.unknown072);
    w.f32(v.y);
    w.f32(v.x);
    w.f32(v.z);
    w.le(v.zone_reason);
    w.le(v.success);
    w.le(v.unknown096);
    return make_packet(Op::ZoneChange, std::move(w));
}

std::optional<ZoneChange> decode_zone_change(const AppPacket& p) {
    if (!exact(p, 100)) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ZoneChange v;
        v.char_name = r.fixed_string(64);
        v.zone_id = r.le<std::uint16_t>();
        v.instance_id = r.le<std::uint16_t>();
        v.unknown068 = r.le<std::uint32_t>();
        v.unknown072 = r.le<std::uint32_t>();
        v.y = r.f32();
        v.x = r.f32();
        v.z = r.f32();
        v.zone_reason = r.le<std::uint32_t>();
        v.success = r.le<std::int32_t>();
        v.unknown096 = r.le<std::uint32_t>();
        return v;
    });
}

std::optional<RequestClientZoneChange> decode_request_client_zone_change(const AppPacket& p) {
    if (p.data.size() < 28) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        RequestClientZoneChange v;
        v.zone_id = r.le<std::uint16_t>();
        v.instance_id = r.le<std::uint16_t>();
        v.unknown004 = r.le<std::uint32_t>();
        v.y = r.f32();
        v.x = r.f32();
        v.z = r.f32();
        v.heading = r.f32();
        v.type = r.le<std::uint32_t>();
        return v;
    });
}

std::optional<std::vector<ZonePoint>> decode_zone_points(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        auto count = r.le<std::uint32_t>();
        if (count > r.remaining() / 32) throw std::out_of_range("zone point count exceeds packet");
        std::vector<ZonePoint> out;
        out.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            ZonePoint z;
            z.number = r.le<std::uint32_t>();
            z.y = r.f32();
            z.x = r.f32();
            z.z = r.f32();
            z.heading = r.f32();
            z.zone_id = r.le<std::uint16_t>();
            z.instance_id = r.le<std::uint16_t>();
            z.unknown024 = r.le<std::uint32_t>();
            z.unknown028 = r.le<std::uint32_t>();
            out.push_back(z);
        }
        // The trailing entry outside count is ignored.
        return out;
    });
}

// ---------------------------------------------------------------- combat

AppPacket encode(const SimpleMessage& v) {
    ByteWriter w;
    w.le(v.string_id);
    w.le(v.color);
    w.le(v.unknown8);
    return make_packet(Op::SimpleMessage, std::move(w));
}

std::optional<SimpleMessage> decode_simple_message(const AppPacket& p) {
    if (p.data.size() < 8) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        SimpleMessage v;
        v.string_id = r.le<std::uint32_t>();
        v.color = r.le<std::uint32_t>();
        if (r.remaining() >= 4) v.unknown8 = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode_target(bool command, std::uint32_t spawn_id) {
    ByteWriter w;
    w.le(spawn_id);
    return make_packet(command ? Op::TargetCommand : Op::TargetMouse, std::move(w));
}

std::optional<std::uint32_t> decode_target(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    return decode_with(p, [](ByteReader& r) { return r.le<std::uint32_t>(); });
}

AppPacket encode_auto_attack(bool second, bool on) {
    ByteWriter w;
    w.le<std::uint32_t>(on ? 1u : 0u);
    return make_packet(second ? Op::AutoAttack2 : Op::AutoAttack, std::move(w));
}

std::optional<bool> decode_auto_attack(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    return decode_with(p, [](ByteReader& r) { return r.le<std::uint32_t>() != 0; });
}

AppPacket encode_auto_fire(bool on) {
    ByteWriter w;
    w.le<std::uint8_t>(on ? 1 : 0);
    return make_packet(Op::AutoFire, std::move(w));
}

AppPacket encode(const Consider& v) {
    ByteWriter w;
    w.le(v.player_id);
    w.le(v.target_id);
    w.le(v.faction);
    w.le(v.level);
    w.le(v.pvp_con);
    w.zeros(3);
    return make_packet(Op::Consider, std::move(w));
}

std::optional<Consider> decode_consider(const AppPacket& p) {
    if (p.data.size() < kConsiderSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        Consider v;
        v.player_id = r.le<std::uint32_t>();
        v.target_id = r.le<std::uint32_t>();
        v.faction = r.le<std::uint32_t>();
        v.level = r.le<std::uint32_t>();
        v.pvp_con = r.le<std::uint8_t>();
        return v;
    });
}

AppPacket encode(const CombatDamage& v) {
    ByteWriter w;
    w.le(v.target);
    w.le(v.source);
    w.le(v.type);
    w.le(v.spell_id);
    w.le(v.damage);
    w.f32(v.force);
    w.f32(v.hit_heading);
    w.f32(v.hit_pitch);
    w.le(v.secondary);
    w.le(v.special);
    return make_packet(Op::Damage, std::move(w));
}

std::optional<CombatDamage> decode_combat_damage(const AppPacket& p) {
    if (p.data.size() < kCombatDamageSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        CombatDamage v;
        v.target = r.le<std::uint16_t>();
        v.source = r.le<std::uint16_t>();
        v.type = r.le<std::uint8_t>();
        v.spell_id = r.le<std::uint32_t>();
        v.damage = r.le<std::int32_t>();
        v.force = r.f32();
        v.hit_heading = r.f32();
        v.hit_pitch = r.f32();
        v.secondary = r.le<std::uint8_t>();
        v.special = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const Death& v) {
    ByteWriter w;
    w.le(v.spawn_id);
    w.le(v.killer_id);
    w.le(v.corpse_id);
    w.le(v.bind_zone_id);
    w.le(v.spell_id);
    w.le(v.attack_skill);
    w.le(v.damage);
    w.le<std::uint32_t>(0);
    return make_packet(Op::Death, std::move(w));
}

std::optional<Death> decode_death(const AppPacket& p) {
    if (p.data.size() < 28) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        Death v;
        v.spawn_id = r.le<std::uint32_t>();
        v.killer_id = r.le<std::uint32_t>();
        v.corpse_id = r.le<std::uint32_t>();
        v.bind_zone_id = r.le<std::uint32_t>();
        v.spell_id = r.le<std::uint32_t>();
        v.attack_skill = r.le<std::uint32_t>();
        v.damage = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const ManaChange& v) {
    ByteWriter w;
    w.le(v.mana);
    w.le(v.endurance);
    w.le(v.spell_id);
    w.le(v.keep_casting);
    w.zeros(3);
    w.le(v.slot);
    return make_packet(Op::ManaChange, std::move(w));
}

std::optional<ManaChange> decode_mana_change(const AppPacket& p) {
    if (p.data.size() < 12) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ManaChange v;
        v.mana = r.le<std::uint32_t>();
        v.endurance = r.le<std::uint32_t>();
        v.spell_id = r.le<std::uint32_t>();
        if (r.remaining() >= 8) {
            v.keep_casting = r.le<std::uint8_t>();
            r.skip(3);
            v.slot = r.le<std::int32_t>();
        }
        return v;
    });
}

AppPacket encode_points_update(bool endurance, const PointsUpdate& v) {
    ByteWriter w;
    w.le(v.current);
    w.le(v.maximum);
    w.le(v.spawn_id);
    return make_packet(endurance ? Op::EnduranceUpdate : Op::ManaUpdate, std::move(w));
}

std::optional<PointsUpdate> decode_points_update(const AppPacket& p) {
    if (p.data.size() < 10) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        PointsUpdate v;
        v.current = r.le<std::uint32_t>();
        v.maximum = r.le<std::uint32_t>();
        v.spawn_id = r.le<std::uint16_t>();
        return v;
    });
}

AppPacket encode(const ExpUpdate& v) {
    ByteWriter w;
    w.le(v.exp);
    w.le(v.tip);
    return make_packet(Op::ExpUpdate, std::move(w));
}

std::optional<ExpUpdate> decode_exp_update(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ExpUpdate v;
        v.exp = r.le<std::uint32_t>();
        if (r.remaining() >= 4) v.tip = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const LevelUpdate& v) {
    ByteWriter w;
    w.le(v.level);
    w.le(v.old_level);
    w.le(v.exp);
    return make_packet(Op::LevelUpdate, std::move(w));
}

std::optional<LevelUpdate> decode_level_update(const AppPacket& p) {
    if (p.data.size() < 12) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        LevelUpdate v;
        v.level = r.le<std::uint32_t>();
        v.old_level = r.le<std::uint32_t>();
        v.exp = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const Animation& v) {
    ByteWriter w;
    w.le(v.spawn_id);
    w.le(v.action);
    w.le(v.speed);
    return make_packet(Op::Animation, std::move(w));
}

std::optional<Animation> decode_animation(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        Animation v;
        v.spawn_id = r.le<std::uint16_t>();
        v.action = r.le<std::uint8_t>();
        v.speed = r.le<std::uint8_t>();
        return v;
    });
}

AppPacket encode(const ZonePlayerToBind& v) {
    ByteWriter w;
    w.le(v.zone_id);
    w.le(v.instance_id);
    w.f32(v.x);
    w.f32(v.y);
    w.f32(v.z);
    w.f32(v.heading);
    write_cstring(w, v.zone_name);
    w.le<std::uint8_t>(1); // save items
    w.zeros(12);           // hp, mana, endurance
    return make_packet(Op::ZonePlayerToBind, std::move(w));
}

std::optional<ZonePlayerToBind> decode_zone_player_to_bind(const AppPacket& p) {
    if (p.data.size() < 21) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ZonePlayerToBind v;
        v.zone_id = r.le<std::uint16_t>();
        v.instance_id = r.le<std::uint16_t>();
        v.x = r.f32();
        v.y = r.f32();
        v.z = r.f32();
        v.heading = r.f32();
        v.zone_name = r.cstring();
        return v;
    });
}

std::optional<RespawnWindow> decode_respawn_window(const AppPacket& p) {
    if (p.data.size() < 16) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        RespawnWindow v;
        v.selected = r.le<std::uint32_t>();
        v.time_remaining_ms = r.le<std::uint32_t>();
        v.delay_ms = r.le<std::uint32_t>();
        auto count = r.le<std::uint32_t>();
        if (count > r.remaining() / 26) throw std::out_of_range("respawn option count exceeds packet");
        for (std::uint32_t i = 0; i < count; ++i) {
            RespawnOption o;
            o.id = r.le<std::uint32_t>();
            o.zone_id = r.le<std::uint32_t>();
            o.x = r.f32();
            o.y = r.f32();
            o.z = r.f32();
            o.heading = r.f32();
            o.name = r.cstring();
            o.type = r.le<std::uint8_t>();
            v.options.push_back(std::move(o));
        }
        return v;
    });
}

AppPacket encode_respawn_selection(std::uint32_t option_id) {
    ByteWriter w;
    w.le(option_id);
    return make_packet(Op::RespawnWindow, std::move(w));
}

} // namespace hailstone::net
