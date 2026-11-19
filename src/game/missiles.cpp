#include "game/missiles.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "assets/character.h"
#include "core/byte_io.h"
#include "core/log.h"
#include "game/doors.h"
#include "game/follow.h"
#include "game/mechanics/anim_select.h"
#include "game/mechanics/remote_spawns.h"
#include "game/sine_table.h"
#include "game/spells.h"

namespace hailstone::game {

namespace {

bool starts_with_nocase(const std::string& s, const char* prefix) {
    for (std::size_t i = 0; prefix[i]; ++i) {
        if (i >= s.size()) return false;
        char a = s[i], b = prefix[i];
        if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 32);
        if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 32);
        if (a != b) return false;
    }
    return true;
}

// The source races whose missiles start at a bone and get the large collision spheres (vf88 0x31, 0x9e,
// 0xc4).
bool large_source(std::uint16_t race) { return race == 0x31 || race == 0x9e || race == 0xc4; }

// |v| < 0.001 on every axis (the 0x9c4be8 / 0x9ca68c tests).
bool still(const glm::vec3& v) {
    return v.x < 0.001f && v.x > -0.001f && v.y < 0.001f && v.y > -0.001f && v.z < 0.001f && v.z > -0.001f;
}

// 0x7a3550: the squared distance from p to the segment from `from` along `dir`.
float segment_distance_squared(const glm::vec3& p, const glm::vec3& from, const glm::vec3& dir) {
    double dx = static_cast<double>(p.x) - from.x, dy = static_cast<double>(p.y) - from.y,
           dz = static_cast<double>(p.z) - from.z;
    double along = dir.z * dz + dir.y * dy + dir.x * dx;
    if (along > 0.0) {
        double len2 = static_cast<double>(dir.y) * dir.y + static_cast<double>(dir.x) * dir.x +
                      static_cast<double>(dir.z) * dir.z;
        double t = along < len2 ? along / len2 : 1.0;
        dx -= dir.x * t;
        dy -= dir.y * t;
        dz -= dir.z * t;
    }
    return static_cast<float>(dz * dz + dy * dy + dx * dx);
}

} // namespace

float spell_launch_steps(std::optional<std::uint32_t> stepped_at, std::uint32_t now) {
    // Hailstone: the original reads (now - the source's +0x30) x 0.02 unchecked, its spawns stepping every frame.
    // Here a time past now counts as none, and the age stops at 12 s, where a remote spawn stops stepping
    // (AdvanceSpawn): a stale or wrapped timestamp cannot throw the missile across the zone.
    // The age as a signed difference, so the 32-bit clock's wrap still reads as a short age.
    if (!stepped_at) return 0.0f;
    const std::uint32_t age = now - *stepped_at;
    if (age == 0 || age > 0x7fffffffu) return 0.0f;
    return static_cast<float>(std::min(age, mechanics::kSpawnStaleMs)) * 0.02f;
}

namespace {

void missile_contacts(Missile& m, std::uint32_t now, const MissileWorld& world) {
    if (m.packet.item_type != kMissileItemSpell || !world.spell || m.done) return;
    const SpellData* spell = world.spell(m.packet.spell);
    if (!spell) return;
    auto visible = [&](const MissileSpawn& from, const glm::vec3& to) {
        glm::vec3 origin = from.position;
        if (from.race == 8 || from.race == 11 || from.race == 12) origin.z += 1.0f;
        return !world.visible || world.visible(origin, to); // 0x4fcc20: a disabled collision query is clear
    };
    auto contact = [&](const MissileSpawn& target) {
        if (!target.has_actor || !world.contact) return;
        if (target.rider_id != 0 && world.spawn)
            if (auto rider = world.spawn(target.rider_id)) {
                world.contact(m, *rider);
                return;
            }
        world.contact(m, target);
    };
    if (spell->has_effect(76)) {
        m.area_contact = true;
        if (now <= m.next_contact) return;
        m.next_contact = now + 2000;
        if (world.spawns)
            for (const auto& target : world.spawns())
                if (target.speed_run != 0.0f && glm::distance(target.position, m.position) <= spell->ae_range * 2.5f &&
                    visible(target, m.position))
                    contact(target);
    } else {
        auto local = world.spawn ? world.spawn(world.local_id) : std::nullopt;
        auto source = world.spawn ? world.spawn(m.packet.source_id) : std::nullopt;
        if (local && source && glm::distance(local->position, m.position) < spell->ae_range &&
            (visible(*source, local->position) || (spell->npc_no_los && source->is_npc)) && now > m.next_contact) {
            m.next_contact = now + 3000;
            contact(*local);
        }
    }
}

} // namespace

std::optional<MissilePacket> decode_missile_packet(std::span<const std::byte> data) {
    if (data.size() < 116) return std::nullopt;
    ByteReader r(data);
    MissilePacket p;
    auto vec = [&r] {
        glm::vec3 v;
        v.x = r.f32();
        v.y = r.f32();
        v.z = r.f32();
        return v;
    };
    p.position = vec();
    p.velocity = vec();
    p.speed = r.f32();
    p.heading = r.f32();
    p.tilt = r.f32();
    p.tilt_rate = r.f32();
    p.heading_rate = r.f32();
    p.pitch = r.f32();
    p.source_id = r.le<std::uint32_t>();
    p.target_id = r.le<std::uint32_t>();
    p.item_id = r.le<std::uint32_t>();
    p.item_extra = r.le<std::uint32_t>();
    p.spell = r.le<std::uint32_t>();
    p.light = r.le<std::uint8_t>();
    r.le<std::uint32_t>(); // +0x45 (EQEmu's 175 at +0x46: its "ranged, not a swing")
    p.range = r.le<std::uint8_t>();
    p.item_type = r.le<std::uint8_t>();
    p.skill = r.le<std::uint8_t>();
    p.to_location = r.le<std::uint8_t>() != 0;
    p.location = vec();
    p.model = r.fixed_string(27);
    // Not the original's check: a NaN or infinite position, velocity or angle would carry into the flight and the
    // renderer's matrices, so such a packet is refused, as spells.cpp and collision.cpp refuse non-finite input.
    auto finite = [](const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    if (!finite(p.position) || !finite(p.velocity) || !finite(p.location) || !std::isfinite(p.speed) ||
        !std::isfinite(p.heading) || !std::isfinite(p.tilt) || !std::isfinite(p.tilt_rate) ||
        !std::isfinite(p.heading_rate) || !std::isfinite(p.pitch))
        return std::nullopt;
    return p;
}

std::uint8_t missile_launch_animation(std::uint8_t skill, std::uint8_t item_type, std::uint8_t range,
                                      std::uint16_t source_race, std::uint8_t source_class, std::uint8_t body_wet,
                                      bool new_style) {
    // Send_OP_Animation_0058f540: PlayAttackAnimation(param 3, param 4, (int)param 5, &speed, 7).
    mechanics::AttackAnimInput in;
    in.skill = skill;
    in.item_type = item_type;
    in.range = range;
    in.slot = 7;
    in.race = source_race;
    in.class_id = source_class;
    in.body_wet = body_wet;
    in.new_style = new_style;
    return mechanics::attack_animation(in);
}

std::uint32_t missile_launch_delay(std::uint32_t anim_length_ms, std::uint8_t range) {
    if (range == 0) return 0;
    // 0x58f5f0-0x58f622: fild of the length, fmul by the float 0.7 (0x9cf8b8, 0.699999988) at extended precision,
    // then truncated (the control word's rounding bits set to chop); a result below 1 ms falls back to 0x5dc
    // (1500 ms). The product is not rounded to a float first, so 1000 ms gives 699, not 700; a double holds it
    // exactly (24-bit factor by a length under 2^29).
    const auto d =
        static_cast<std::uint32_t>(static_cast<double>(anim_length_ms) * static_cast<double>(kLaunchFraction));
    return d < 1 ? kDefaultLaunchMs : d;
}

std::uint32_t missile_launch_delay(const assets::AnimationClip* clip, std::uint8_t range) {
    return missile_launch_delay(clip ? assets::clip_length_ms(*clip) : 0, range);
}

float cmath_angle512(float a, float b) {
    if (b <= 0.0f) return 128.0f; // a >= 0: case 2 with b == 0 (MissileAI never passes b < 0)
    // Case 1: the tangent table's last entry not above a / b (the table holds tan(i pi / 256), i < 128).
    double r = static_cast<double>(a) / b;
    double i = std::floor(std::atan(r) * 256.0 / std::numbers::pi);
    return static_cast<float>(std::clamp(i, 0.0, 127.0));
}

float missile_actor_scale(const std::string& model, std::int32_t zone_id) {
    float s = 1.0f;
    if (zone_id != 0x3c && zone_id != 0x43 && zone_id != 0x13 && zone_id != 0x37) {
        if (starts_with_nocase(model, "IT66") || starts_with_nocase(model, "IT69") || starts_with_nocase(model, "IT73"))
            s = 1.5f;
        else if (starts_with_nocase(model, "IT74"))
            s = 1.3f;
    } else if (starts_with_nocase(model, "IT70") || starts_with_nocase(model, "IT74")) {
        s = 0.8f;
    }
    if (starts_with_nocase(model, "BBBOARD")) s = 1.5f;
    return s;
}

Missile make_missile(const MissilePacket& p, std::uint32_t now, std::uint32_t launch_delay) {
    Missile m;
    m.packet = p;
    m.position = p.position;
    m.velocity = p.velocity;
    m.speed = p.speed;
    m.heading = p.heading;
    m.tilt = p.tilt;
    m.tilt_rate = p.tilt_rate;
    m.heading_rate = p.heading_rate;
    m.pitch = p.pitch;
    m.target_id = p.target_id;
    // The velocity's tiny components are zeroed (|v| < 0.001).
    for (int i = 0; i < 3; ++i)
        if (std::fabs(m.velocity[i]) < 0.001f) m.velocity[i] = 0.0f;
    m.launch_at = now + launch_delay;
    return m;
}

void process_missile(Missile& m, std::uint32_t now, const MissileWorld& world) {
    if (m.packet.item_type != kMissileItemSpell && now < m.launch_at) return;
    std::optional<MissileSpawn> source = world.spawn ? world.spawn(m.packet.source_id) : std::nullopt;
    if (!source) {
        m.done = true;
        return;
    }
    int stage = 0;
    if (m.packet.item_type == kMissileItemSpell) {
        const SpellData* spell = m.packet.spell >= 1 && m.packet.spell <= kMaxSpellId && world.spell
                                     ? world.spell(m.packet.spell) : nullptr;
        if (!spell) {
            m.done = true;
            return;
        }
        m.ends_at = now + (spell->ae_duration != 0 ? spell->ae_duration : spell->travel_type == 3 ? 30000u : 50u);
        const bool area = spell->target_type == 8 || spell->target_type == 21 || spell->target_type == 22 ||
                          spell->target_type == 23 || spell->target_type == 28 || spell->target_type == 45;
        stage = area ? 2 : 1;
        if (area) {
            if (spell->target_type == 45) m.position = m.packet.location;
            else if (auto target = world.spawn ? world.spawn(m.packet.target_id) : std::nullopt) m.position = target->position;
            // A missing area target leaves the packet's safe position; the native stack value is unspecified.
            m.velocity = glm::vec3(0.0f);
            m.speed = 0.0f;
        } else {
            if (large_source(source->race)) {
                if (auto point = world.launch_point ? world.launch_point(source->id) : std::nullopt) {
                    m.position = *point + glm::vec3(m.velocity.x * 32.5f, m.velocity.y * 32.5f, -12.0f);
                }
                m.pitch = -72.0f;
                m.velocity.z += cmath_sin(m.pitch) * m.speed;
            }
            if (spell->travel_type == 3) m.velocity.z = 0.0f;
            else {
                m.position += m.velocity * (5.5f + spell_launch_steps(source->stepped_at, now));
            }
        }
    } else {
        m.ends_at = now + 6000;
        m.position += m.velocity; // one step of its own velocity (x, y, then z)
    }
    // CreateActor's collision sphere (the fifth float): with the range byte 0.1 (1.0 for the large races),
    // without it 1.5 (20.5).
    if (m.packet.range == 0) m.radius = large_source(source->race) ? 20.5f : 1.5f;
    else m.radius = large_source(source->race) ? 1.0f : 0.1f;
    m.has_actor = true;
    m.actor_position = m.position;
    m.actor_scale = missile_actor_scale(m.packet.model, world.zone_id);
    // The actor's vtable +0x64 (0.1, 0) and the light (+0x118, CreateLight) are not drawn here.
    m.aim_heading = m.heading;
    m.aim_pitch = m.pitch;
    m.aim_distance = 2000.0f;
    m.last_step = now;
    m.last_aim = now;
    if (stage != 0 && world.effect) world.effect(m, stage);
}

void missile_ai(Missile& m, std::uint32_t now, const MissileWorld& world) {
    if (m.speed == 0.0f || m.packet.to_location || m.target_id == 0) return;
    std::optional<MissileSpawn> t = world.spawn ? world.spawn(m.target_id) : std::nullopt;
    if (!t || t->dead) return;
    // CDisplay::TrueDistance with z scale 0: across the ground.
    float dx = m.position.x - t->position.x, dy = m.position.y - t->position.y;
    float d = std::sqrt(dx * dx + dy * dy);
    m.aim_distance = d;
    if (t->id == m.packet.source_id) {
        m.target_id = 0;
        return;
    }
    std::uint32_t interval;
    if (d < 200.0f) {
        interval = 0;
        if (d < t->avatar_height) {
            m.done = true; // HitActor(target, 0)
            return;
        }
    } else if (d < 250.0f) {
        interval = 250;
    } else if (d < 350.0f) {
        interval = 500;
    } else {
        interval = 1000;
    }
    if (interval < now - m.last_aim) {
        float dz = t->position.z - m.position.z;
        m.last_aim = now;
        m.aim_heading = static_cast<float>(x87_bearing(m.position.x, m.position.y, t->position.x, t->position.y,
                                                       mechanics::Fpu{}));
        float up = 128.0f - cmath_angle512(m.aim_distance, std::fabs(dz));
        m.aim_pitch = dz < 0.0f ? -up : up;
    }
    const bool thrown = m.packet.item_type == kMissileItemThrown;
    if (m.aim_distance < 42.0f) {
        m.heading = m.aim_heading;
        if (m.aim_pitch <= m.pitch) {
            if (!thrown) m.tilt_rate = -0.5f;
        } else if (!thrown) {
            m.tilt_rate = 0.5f;
        }
        m.pitch = m.aim_pitch;
        return;
    }
    // __HeadingDiff: the shorter way, and its signed form (negative across the wrap).
    auto diff = [](float a, float b, float& sign) {
        float hi = a, lo = b;
        if (a <= b) {
            hi = b;
            lo = a;
        }
        float v = hi - lo;
        if (v > 256.0f) {
            sign = v - 512.0f;
            return -(v - 512.0f);
        }
        sign = v;
        return v;
    };
    float hsign = 0.0f, psign = 0.0f;
    float hd = diff(m.heading, m.aim_heading, hsign);
    float pd = diff(m.pitch, m.aim_pitch, psign);
    if (hd >= 1.0f) {
        float step = std::min(hd, 16.0f);
        float h;
        if (m.aim_heading <= m.heading || hsign >= 0.0f) {
            h = (m.heading <= m.aim_heading || hsign <= 0.0f) ? m.heading + step : m.heading - step;
        } else {
            h = m.heading - step;
        }
        m.heading = fix_heading(h);
    } else {
        m.heading = m.aim_heading;
    }
    if (pd < 1.0f) {
        m.pitch = m.aim_pitch;
        return;
    }
    if (pd < 16.0f) return;
    if (psign < 0.0f || m.aim_pitch < m.pitch) {
        m.pitch -= 16.0f;
        if (!thrown) m.tilt_rate = -0.5f;
    } else {
        m.pitch += 16.0f;
        if (!thrown) m.tilt_rate = 0.5f;
    }
    m.pitch = fix_heading(m.pitch);
}

void missile_physics(Missile& m, std::uint32_t now) {
    if (m.last_step < 1) {
        m.last_step = now;
        return;
    }
    // Ticks of 50 ms since the last step, at most 3 (the game timer's vtable +8, 0x9d7070).
    float ticks = static_cast<float>(now - m.last_step) * 0.02f;
    if (3.0f < ticks) ticks = 3.0f;
    m.last_step = now;
    // PlayerPhysics::IntegrateHeading: the heading turns at its rate, kept in 0..512 (at most six wraps).
    m.heading += m.heading_rate * ticks;
    for (int i = 0; m.heading > 512.0f; ++i) {
        m.heading -= 512.0f;
        if (i > 5) {
            m.heading = 0.0f;
            break;
        }
    }
    for (int i = 0; m.heading < 0.0f; ++i) {
        m.heading += 512.0f;
        if (i > 5) {
            m.heading = 0.0f;
            break;
        }
    }
    // Vtable +8 (0x8d2770): the tilt turns at its rate.
    m.tilt += m.tilt_rate * ticks;
    // Vtable +0xc (0x8d2620): the push along heading and pitch, x 0.8 (0x8d2000's first pair).
    glm::vec3 push{0.0f};
    if (m.speed != 0.0f) {
        float flat = cmath_cos(m.heading) * m.speed;
        push.x += cmath_cos(m.pitch) * flat;
        flat = cmath_sin(m.heading) * m.speed;
        push.y += cmath_cos(m.pitch) * flat;
        if (m.pitch != 0.0f) push.z += cmath_sin(m.pitch) * m.speed;
    }
    if (push.x != 0.0f) push.x *= 0.8f;
    if (push.y != 0.0f) push.y *= 0.8f;
    if (push.z != 0.0f) push.z *= 0.8f;
    // Vtable +4 (PhysicsObject::ApplyFriction): 0.625 with the range byte (0x8d2000's second pair), else 0.85.
    float friction = m.packet.range != 0 ? 0.625f : 0.85f;
    for (int i = 0; i < 3; ++i) {
        if (m.velocity[i] != 0.0f) m.velocity[i] *= friction;
        if (std::fabs(m.velocity[i]) < 1.1920929e-07f) m.velocity[i] = 0.0f;
        m.velocity[i] += push[i];
    }
    // Without the range byte a missile slower than 0.05 across the ground stops.
    if (m.packet.range == 0 && std::sqrt(m.velocity.y * m.velocity.y + m.velocity.x * m.velocity.x) < 0.05f)
        m.velocity = glm::vec3(0.0f);
    m.position += m.velocity * ticks;
}

bool move_missile(Missile& m, std::uint32_t now, const MissileWorld& world) {
    const glm::vec3 from = m.actor_position; // the actor's position (vtable +0x80)
    const glm::vec3 to = m.position;         // where the physics put it
    const glm::vec3 d = to - from;
    const bool moved = !(d.x < 0.001f && d.x > -0.001f && d.y < 0.001f && d.y > -0.001f && d.z < 0.001f &&
                         d.z > -0.001f);
    // (Not moving: the counter at +0xf0, which nothing here raises, would date +0xf4.)
    if (still(m.velocity)) {
        missile_contacts(m, now, world);
        if (now > m.ends_at) return false;
    } else {
        if (now > m.ends_at) return false;
        if (!moved && m.packet.item_type != kMissileItemSpell) return false;
        if (moved && (m.target_id == 0 || m.packet.item_type == kMissileItemSpell)) {
            // The sweep (its result is always 0; a hit ends the missile through the callback and the
            // skitter distance it leaves).
            if (world.sweep && world.sweep(from, to, m.radius)) return false;
        }
        if (moved && m.target_id != 0)
            if (std::optional<MissileSpawn> t = world.spawn ? world.spawn(m.target_id) : std::nullopt) {
                // The ray from the actor to the new position against the target's Height.
                if (segment_distance_squared(t->position, from, d) < t->height * t->height) m.done = true; // HitActor
            }
        if (m.done) return false;
        m.position = to;
    }
    // Outside the zone: stopped, back where the actor was (when that is inside; else 0xddfb3c, not modelled).
    if (world.inside && !world.inside(m.position)) {
        m.velocity = glm::vec3(0.0f);
        m.position = from;
    }
    // Not falling while level with the actor: at most -1 down.
    float dz = from.z - m.position.z;
    if (dz < 0.001f && dz > -0.001f && m.velocity.z < -1.0f) m.velocity.z = -1.0f;
    m.actor_position = m.position; // Move; Orient (heading, tilt, 0)
    return true;
}

void MissileList::update(std::uint32_t now, const MissileWorld& world) {
    auto retire = [&](const Missile& m) {
        if (m.has_actor && m.packet.item_type == kMissileItemSpell && m.packet.spell == 54 && world.effect) {
            // Placed in the world where the missile was, not on its actor: the actor goes with the missile
            // (ended below), and the impact must outlive it.
            Missile impact = m;
            impact.position -= impact.velocity;
            impact.actor_position = impact.position;
            impact.has_actor = false;
            world.effect(impact, 2);
        }
        if (m.has_actor && m.packet.item_type == kMissileItemSpell && m.packet.spell == 859 && world.spell_hit)
            if (auto local = world.spawn ? world.spawn(world.local_id) : std::nullopt;
                local && glm::distance(glm::vec2(local->position), glm::vec2(m.position)) < 40.0f)
                world.spell_hit(m);
        if (world.ended) world.ended(m);
    };
    for (std::size_t i = 0; i < missiles_.size();) {
        Missile& m = missiles_[i];
        bool keep = true;
        // DeleteMyMissiles @0x5a0b60 (see the header): a missile whose source is gone ends, one whose target is
        // gone (or was not known when it came: the constructor's GetSpawnByID left +0x6c null) loses it.
        if (world.spawn) {
            if (!world.spawn(m.packet.source_id)) {
                if (m.has_actor)
                    log::debug("missile {} ended at ({:.1f}, {:.1f}, {:.1f}): its source is gone", m.packet.model,
                               m.position.x, m.position.y, m.position.z);
                retire(m);
                missiles_.erase(missiles_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (m.target_id != 0 && !world.spawn(m.target_id)) {
                m.target_id = 0;
                m.packet.target_id = 0;
            }
        }
        if (!m.has_actor) {
            process_missile(m, now, world);
            if (m.done) keep = false;
            else if (m.has_actor)
                log::debug("missile {} launched at ({:.1f}, {:.1f}, {:.1f})", m.packet.model, m.position.x,
                           m.position.y, m.position.z);
        } else if (now - m.last_step > 0x20) {
            missile_ai(m, now, world);
            missile_physics(m, now);
            keep = move_missile(m, now, world);
            if (!keep)
                log::debug("missile {} ended at ({:.1f}, {:.1f}, {:.1f}) after {} ms{}", m.packet.model, m.position.x,
                           m.position.y, m.position.z, now - (m.ends_at - 6000), m.done ? ": hit" : now > m.ends_at ? ": lifetime" : "");
        }
        if (keep) ++i;
        else {
            retire(m);
            missiles_.erase(missiles_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

} // namespace hailstone::game
