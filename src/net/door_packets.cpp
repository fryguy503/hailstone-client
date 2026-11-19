// Written from EQEmu rof2.cpp / rof2_structs.h (read as reference, no code
// copied) and the original's readers and writers
// (EQSwitch::LoadSwitchSounds, EQSwitch::UseSwitch, CEverQuest::LMouseUp).

#include "net/door_packets.h"

#include "net/opcodes.h"
#include "net/packet_io.h"

namespace hailstone::net {

using detail::decode_with;
using detail::write_cstring;

namespace {

DoorRecord read_door(ByteReader& r) {
    DoorRecord d;
    d.name = r.fixed_string(32);
    d.y = r.f32();
    d.x = r.f32();
    d.z = r.f32();
    d.heading = r.f32();
    d.incline = r.le<std::int32_t>();
    d.size = r.le<std::uint16_t>();
    d.unknown54 = r.le<std::uint16_t>();
    d.unknown56 = r.le<std::uint32_t>();
    d.door_id = r.le<std::uint8_t>();
    d.open_type = r.le<std::uint8_t>();
    d.state_at_spawn = r.le<std::uint8_t>();
    d.invert_state = r.le<std::uint8_t>();
    d.door_param = r.le<std::int32_t>();
    d.adventure_door_id = r.le<std::int32_t>();
    d.dyn_door_id = r.le<std::int32_t>();
    d.real_estate_door_id = r.le<std::int32_t>();
    d.has_script = r.le<std::uint8_t>();
    d.usable = r.le<std::uint8_t>();
    d.remain_open = r.le<std::uint8_t>();
    d.visible = r.le<std::uint8_t>();
    d.heading_changed = r.le<std::uint8_t>();
    d.unknown85 = r.le<std::uint8_t>();
    d.unknown86 = r.le<std::uint8_t>();
    d.unknown87 = r.le<std::uint8_t>();
    d.top_speed1 = r.f32();
    d.top_speed2 = r.f32();
    d.stamp_time = r.le<std::uint8_t>();
    for (auto& b : d.unknown97) b = r.le<std::uint8_t>();
    return d;
}

void write_door(ByteWriter& w, const DoorRecord& d) {
    w.fixed_string(d.name, 32);
    w.f32(d.y);
    w.f32(d.x);
    w.f32(d.z);
    w.f32(d.heading);
    w.le(d.incline);
    w.le(d.size);
    w.le(d.unknown54);
    w.le(d.unknown56);
    w.le(d.door_id);
    w.le(d.open_type);
    w.le(d.state_at_spawn);
    w.le(d.invert_state);
    w.le(d.door_param);
    w.le(d.adventure_door_id);
    w.le(d.dyn_door_id);
    w.le(d.real_estate_door_id);
    w.le(d.has_script);
    w.le(d.usable);
    w.le(d.remain_open);
    w.le(d.visible);
    w.le(d.heading_changed);
    w.le(d.unknown85);
    w.le(d.unknown86);
    w.le(d.unknown87);
    w.f32(d.top_speed1);
    w.f32(d.top_speed2);
    w.le(d.stamp_time);
    for (auto b : d.unknown97) w.le(b);
}

} // namespace

std::optional<std::vector<DoorRecord>> decode_spawn_doors(const AppPacket& p) {
    return decode_with(p, [&](ByteReader& r) {
        std::vector<DoorRecord> out;
        std::size_t count = p.data.size() / kDoorRecordSize;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) out.push_back(read_door(r));
        return out;
    });
}

AppPacket encode_spawn_doors(const std::vector<DoorRecord>& doors) {
    ByteWriter w;
    for (const auto& d : doors) write_door(w, d);
    return make_packet(Op::SpawnDoor, std::move(w));
}

AppPacket encode(const ClickDoor& v) {
    ByteWriter w;
    w.le(v.door_id);
    w.zeros(3);
    w.le(v.pick_skill);
    w.le(v.item_id);
    w.le(v.player_id);
    return make_packet(Op::ClickDoor, std::move(w));
}

std::optional<ClickDoor> decode_click_door(const AppPacket& p) {
    if (p.data.size() != kClickDoorSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ClickDoor v;
        v.door_id = r.le<std::uint8_t>();
        r.skip(3);
        v.pick_skill = r.le<std::int32_t>();
        v.item_id = r.le<std::uint32_t>();
        v.player_id = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const MoveDoor& v) {
    ByteWriter w;
    w.le(v.door_id);
    w.le(v.action);
    return make_packet(Op::MoveDoor, std::move(w));
}

std::optional<MoveDoor> decode_move_door(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        MoveDoor v;
        v.door_id = r.le<std::uint8_t>();
        v.action = r.le<std::uint8_t>();
        return v;
    });
}

AppPacket encode(const GroundObject& v) {
    ByteWriter w;
    w.le(v.drop_id);
    write_cstring(w, v.name);
    w.le(v.zone_id);
    w.le(v.zone_instance);
    w.le(v.drop_id2);
    w.le(v.unknown);
    w.f32(v.heading);
    w.f32(v.tilt_x);
    w.f32(v.tilt_y);
    w.f32(v.size);
    w.f32(v.y);
    w.f32(v.x);
    w.f32(v.z);
    w.le(v.object_type);
    return make_packet(Op::GroundSpawn, std::move(w));
}

std::optional<GroundObject> decode_ground_object(const AppPacket& p) {
    return decode_with(p, [](ByteReader& r) {
        GroundObject v;
        v.drop_id = r.le<std::uint32_t>();
        v.name = r.cstring();
        v.zone_id = r.le<std::uint16_t>();
        v.zone_instance = r.le<std::uint16_t>();
        v.drop_id2 = r.le<std::uint32_t>();
        v.unknown = r.le<std::uint32_t>();
        v.heading = r.f32();
        v.tilt_x = r.f32();
        v.tilt_y = r.f32();
        v.size = r.f32();
        v.y = r.f32();
        v.x = r.f32();
        v.z = r.f32();
        v.object_type = r.le<std::int32_t>();
        return v;
    });
}

AppPacket encode(const ClickObject& v) {
    ByteWriter w;
    w.le(v.drop_id);
    w.le(v.player_id);
    return make_packet(Op::ClickObject, std::move(w));
}

std::optional<ClickObject> decode_click_object(const AppPacket& p) {
    if (p.data.size() < kClickObjectSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ClickObject v;
        v.drop_id = r.le<std::uint32_t>();
        v.player_id = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const ClickObjectAction& v) {
    ByteWriter w;
    w.le(v.drop_id);
    w.le(v.unknown04);
    w.le(v.unknown08);
    w.le(v.type);
    w.le(v.unknown16);
    w.le(v.icon);
    w.le(v.unknown24);
    w.fixed_string(std::string_view(v.name).substr(0, 63), 64);
    return make_packet(Op::ClickObjectAction, std::move(w));
}

AppPacket encode(const ClearObject& v) {
    ByteWriter w;
    w.le(v.clear);
    for (int i = 0; i < 3; ++i) w.le(std::uint8_t{0});
    w.le(v.slot);
    return make_packet(Op::ClearObject, std::move(w));
}

std::optional<ClearObject> decode_clear_object(const AppPacket& p) {
    if (p.data.size() < kClearObjectSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ClearObject v;
        v.clear = r.le<std::uint8_t>();
        r.skip(3);
        v.slot = r.le<std::int32_t>();
        return v;
    });
}

std::optional<ClickObjectAction> decode_click_object_action(const AppPacket& p) {
    if (p.data.size() != kClickObjectActionSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        ClickObjectAction v;
        v.drop_id = r.le<std::uint32_t>();
        v.unknown04 = r.le<std::int32_t>();
        v.unknown08 = r.le<std::int32_t>();
        v.type = r.le<std::uint32_t>();
        v.unknown16 = r.le<std::uint32_t>();
        v.icon = r.le<std::uint32_t>();
        v.unknown24 = r.le<std::uint32_t>();
        v.name = r.fixed_string(64);
        return v;
    });
}

AppPacket encode_close_object() { return make_empty(Op::ClickObjectAction); }

AppPacket encode(const TradeSkillCombine& v) {
    ByteWriter w;
    write_slot(w, v.container_slot);
    write_slot(w, v.guildtribute_slot);
    return make_packet(Op::TradeSkillCombine, std::move(w));
}

AppPacket encode(const RecipeFavorites& v) {
    ByteWriter w;
    w.le(v.object_type);
    w.le(v.container_id);
    for (std::size_t n = 0; n < 500; ++n) w.le<std::uint32_t>(n < v.recipes.size() ? v.recipes[n] : 0);
    return make_packet(Op::RecipesFavorite, std::move(w));
}

AppPacket encode(const RecipesSearch& v) {
    ByteWriter w;
    w.le(v.object_type);
    w.le(v.container_id);
    w.le(v.min_trivial);
    w.le(v.max_trivial);
    w.fixed_string(v.text.substr(0, 55), 56);
    w.le<std::uint32_t>(0x300);
    w.le<std::uint32_t>(0);
    return make_packet(Op::RecipesSearch, std::move(w));
}

std::optional<RecipeReply> decode_recipe_reply(const AppPacket& p) {
    if (p.data.size() < 84) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        RecipeReply v;
        v.object_type = r.le<std::uint32_t>();
        v.container_id = r.le<std::uint32_t>();
        v.component_count = r.le<std::uint32_t>();
        v.recipe_id = r.le<std::uint32_t>();
        v.trivial = r.le<std::uint32_t>();
        v.name = r.fixed_string(64);
        return v;
    });
}

AppPacket encode(const RecipeReply& v) {
    ByteWriter w;
    w.le(v.object_type);
    w.le(v.container_id);
    w.le(v.component_count);
    w.le(v.recipe_id);
    w.le(v.trivial);
    w.fixed_string(v.name.substr(0, 63), 64);
    return make_packet(Op::RecipeReply, std::move(w));
}

AppPacket encode_recipe_details_request(std::uint32_t recipe_id) {
    ByteWriter w;
    w.le(recipe_id);
    return make_packet(Op::RecipeDetails, std::move(w));
}

std::optional<RecipeDetails> decode_recipe_details(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        RecipeDetails v;
        v.recipe_id = r.be<std::uint32_t>();
        // Ten entries, as CTradeSkillWnd_HandleRecipeDetails (0x77dfd0) reads them: a big-endian 0xffffffff for an
        // unused one, else {item id, icon, name}. Whatever follows them is not read: EQEmu since #5100 appends the
        // recipe's trivial (big-endian u32) for a later client, and the trivial already came in OP_RecipeReply.
        for (int n = 0; n < 10 && r.remaining() >= 4; ++n) {
            const auto item = r.be<std::uint32_t>();
            if (item == 0xffffffffu) continue;
            RecipeComponent c;
            c.item_id = item;
            c.icon = r.be<std::uint32_t>();
            c.name = r.cstring();
            v.components.push_back(std::move(c));
        }
        return v;
    });
}

AppPacket encode(const RecipeDetails& v) {
    ByteWriter w;
    w.be<std::uint32_t>(v.recipe_id);
    for (std::size_t n = v.components.size(); n < 10; ++n) w.le<std::uint32_t>(0xffffffff);
    for (const auto& c : v.components) {
        w.be<std::uint32_t>(c.item_id);
        w.be<std::uint32_t>(c.icon);
        write_cstring(w, c.name);
    }
    return make_packet(Op::RecipeDetails, std::move(w));
}

AppPacket encode(const RecipeAutoCombine& v) {
    ByteWriter w;
    w.le(v.object_type);
    w.le(v.container_id);
    write_slot(w, v.container_slot);
    write_slot(w, v.other_slot);
    w.le(v.recipe_id);
    w.le(v.reply_code);
    return make_packet(Op::RecipeAutoCombine, std::move(w));
}

std::optional<RecipeAutoCombine> decode_recipe_auto_combine(const AppPacket& p) {
    if (p.data.size() < kRecipeAutoCombineSize) return std::nullopt;
    return decode_with(p, [](ByteReader& r) {
        RecipeAutoCombine v;
        v.object_type = r.le<std::uint32_t>();
        v.container_id = r.le<std::uint32_t>();
        v.container_slot = read_slot(r);
        v.other_slot = read_slot(r);
        v.recipe_id = r.le<std::uint32_t>();
        v.reply_code = r.le<std::uint32_t>();
        return v;
    });
}

} // namespace hailstone::net
