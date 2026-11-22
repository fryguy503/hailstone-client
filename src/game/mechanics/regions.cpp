#include "game/mechanics/regions.h"

#include <cstring>

namespace hailstone::game::mechanics {

namespace {

constexpr std::uint8_t kStandState6f = 0x6f;
constexpr float kIceFriction = 0.99f;     // 0x9c5994
constexpr float kIceAcceleration = 0.02f; // 0x9c58fc

// A float copied through the FPU: a signalling NaN comes out quiet.
float through_fpu(float v) {
    std::uint32_t b;
    std::memcpy(&b, &v, 4);
    if ((b & 0x7f800000u) == 0x7f800000u && (b & 0x007fffffu)) b |= 0x00400000u;
    std::memcpy(&v, &b, 4);
    return v;
}

} // namespace

double collision_height(const CollisionHeightSpawn& s, const Fpu& f) {
    double h = static_cast<double>(through_fpu(s.avatar_height)); // +0x138, quiet on load
    if (s.stand_state == kStandState6f) return h;
    if (s.mount_race && s.has_rider) return x87_mul(f, X87(h), X87(3.0f));
    if (0.0f < s.length) return static_cast<double>(through_fpu(s.length));
    return x87_add(f, X87(h), X87(h)); // h + h
}

float region_sample_z(const CollisionHeightSpawn& s, float z, const Fpu& f) {
    float avatar = through_fpu(s.avatar_height);
    double v = x87_sub(f, X87(collision_height(s, f)), X87(avatar)); // avatarHeight a memory operand
    v = x87_sub(f, X87(0.25f), X87(v));                              // 0.25 - v
    v = x87_add(f, X87(v), X87(through_fpu(z)));
    return static_cast<float>(v);                                    // rounds to float here
}

RegionChange check_region_change(RegionSpawn& s, const RegionPoint& position, const RegionZone& zone,
                                 RegionQueries& q, const Fpu& f) {
    RegionChange out;
    out.sample = {through_fpu(position.x), through_fpu(position.y), through_fpu(position.z)};
    out.sample.z = region_sample_z(s.body, out.sample.z, f);
    if (!q.inside(out.sample)) return out;
    out.asked = true;
    std::uint32_t flags = q.regions(out.sample);
    if (flags == s.held_flags) return out;
    s.held_flags = flags;
    if ((flags & kRegionIce) && s.type != 1) {
        s.area_friction = kIceFriction;
        s.acceleration_friction = kIceAcceleration;
    } else {
        s.area_friction = through_fpu(zone.area_friction);
        s.acceleration_friction = through_fpu(zone.acceleration_friction);
    }
    if ((flags & kRegionZoneLine) && s.type <= 1 && q.do_teleport(out.sample)) {
        out.teleported = true;
        return out;
    }
    s.pvp_flag = static_cast<std::uint8_t>((flags >> 30) & 1);
    if (zone.zone_id == kNoPvpFlagZone) s.pvp_flag = 0;
    if (s.pvp_flag != 0 && q.is_player_spawn()) q.print(kStrPvpRegion);
    q.feet_environment_change(flags);
    return out;
}

std::optional<std::int32_t> FeetWetness::change(std::uint32_t flags, float speed_z) {
    const std::uint8_t next = static_cast<std::uint8_t>(flags & 0xff);
    if (next == type_) return std::nullopt;
    const std::uint8_t previous = type_;
    type_ = next;
    switch (next) {
    case kRegionWaterType:
    case kRegionSlimeType:
    case kRegionFreezingWaterType:
    case kRegionWater2Type:
    case kRegionWater3Type:
        // The original's two ordered compares against 1 and -1 (0x59ec6b).
        if (speed_z >= kWaterVerticalSpeed || speed_z <= -kWaterVerticalSpeed) return kSoundWaterIn;
        break;
    case kRegionLavaType:
        return kSoundWaterOut;
    default:
        break;
    }
    // The original's check before the switch (0x59ec4c): only after it has
    // found no sound of its own does a step out of any type into 0 play.
    if (previous != 0 && next == 0) return kSoundWaterOut;
    return std::nullopt;
}

} // namespace hailstone::game::mechanics
