// The original client's switch (door) rules, followed exactly; sources in game/doors.h.

#include "game/doors.h"

#include "game/collision_callbacks.h"
#include "game/mechanics/switches.h"

#include <bit>
#include <cctype>
#include <cmath>
#include <numbers>

#include <glm/gtc/matrix_transform.hpp>

namespace hailstone::game {

namespace {

// EQSwitch::TopSpeed @0x50f2c0: the top speed of the spinning types and,
// through `accel`, their acceleration per tick.
float top_speed(std::uint8_t type, float* accel) {
    switch (type) {
    case 100: case 105: if (accel) *accel = 0.025f; return 3.0f;   // 0x3ccccccd
    case 101: case 106: if (accel) *accel = 0.075f; return 9.0f;   // 0x3d99999a
    case 102: case 107: if (accel) *accel = 0.125f; return 15.0f;  // 0x3e000000
    case 0x73: case 0x74: if (accel) *accel = 0.375f; return 45.0f; // 0x3ec00000
    default: return 0.0f;
    }
}

// EQSwitch::ResetSwitchState @0x510b50.
void reset_switch_state(Door& d, std::uint8_t state) {
    d.y = d.default_y;
    d.x = d.default_x;
    d.z = d.default_z;
    d.heading = d.default_heading;
    d.door_angle = d.default_door_angle;
    if (state != kSwitchOpening && state != kSwitchOpen && !d.remain_open) {
        d.state = d.state == kSwitchClosing ? 0 : state;
        return;
    }
    std::uint8_t t = d.type;
    bool stays = !(t == 0x35 || t < 0x32 || t > 0x3a) || static_cast<std::uint8_t>(t + 0x67) <= 2;
    d.state = stays ? kSwitchOpen : kSwitchOpening;
}

// EQSwitch::RepopSwitch @0x512690. True when the original uses the switch
// again instead (types 0x28 and 0x2d: UseSwitch(0, 0, 0xff, 0)).
bool repop_switch(Door& d) {
    if (d.remain_open) {
        d.state = kSwitchOpen;
        return false;
    }
    if (d.state == d.default_state) return false;
    if (d.type == 0x28 || d.type == 0x2d) return true;
    d.state = d.default_state == 0 ? kSwitchClosing : d.default_state == 1 ? kSwitchOpening : d.default_state;
    return false;
}

// CDisplay::SlideSwitchLeftRight @0x4880a0: moves the door sideways (along
// world x or y by its heading) toward its open or closed position. Returns
// true when it got there; with `change`, it then becomes open or closed.
bool slide_switch_left_right(Door& d, float speed, bool change, float ticks, std::uint32_t now_ms) {
    float distance = 0.0f;
    if (speed == 0.0f) speed = 0.35f;
    switch (d.type) {
    case 10: case 11: case 12: distance = 8.0f; break;
    case 15: case 16: case 17: distance = 10.0f; break;
    case 20: case 21: case 22: distance = 14.0f; break;
    case 0x4f: case 0x51:
        speed = -speed;
        [[fallthrough]];
    case 25: case 26: case 27: case 0x4e: case 0x50:
        distance = custom_move_distance(d);
        if (distance == 0.0f) distance = 22.0f;
        break;
    default: break;
    }
    if (speed < 0.0f) distance = -distance;
    float* pos;
    float* def;
    if (d.heading == 128.0f) {
        pos = &d.y;
        def = &d.default_y;
    } else if (d.heading == 256.0f) {
        pos = &d.x;
        def = &d.default_x;
    } else {
        speed = -speed;
        distance = -distance;
        if (d.heading == 384.0f) {
            pos = &d.y;
            def = &d.default_y;
        } else {
            pos = &d.x;
            def = &d.default_x;
        }
    }
    float target = *def + distance;
    distance = std::abs(distance);
    if (d.state == kSwitchOpening) {
        *pos += speed * ticks;
        if (distance <= std::abs(*pos - *def)) {
            *pos = target;
            if (change) change_state(d, kSwitchOpen, now_ms);
            return true;
        }
    } else {
        *pos -= speed * ticks;
        if (distance <= std::abs(*pos - target)) {
            *pos = *def;
            if (change) change_state(d, kSwitchClosed, now_ms);
            return true;
        }
    }
    return false;
}

} // namespace

float fix_heading(float h) {
    for (int i = 0; h >= 512.0f && i < 7; ++i) h -= 512.0f;
    for (int i = 0; h < 0.0f && i < 7; ++i) h += 512.0f;
    return h;
}

float heading_diff(float a, float b) {
    float hi = a, lo = b;
    if (a <= b) {
        hi = b;
        lo = a;
    }
    float d = hi - lo;
    return d > 256.0f ? -(d - 512.0f) : d;
}

float custom_move_distance(const Door& d) {
    if (d.param > 0) return static_cast<float>(d.param);
    if (d.param < -3) return -static_cast<float>(d.param);
    return 0.0f;
}

Door make_door(const DoorSpawn& s, std::uint32_t now_ms) {
    Door d;
    d.id = s.id;
    d.name = s.name;
    d.type = s.type;
    d.state = s.state;
    d.default_state = s.default_state;
    d.default_y = s.y;
    d.default_x = s.x;
    d.default_z = s.z;
    d.default_heading = s.heading;
    d.default_door_angle = static_cast<float>(s.incline);
    d.param = s.param;
    d.adventure_id = s.adventure_id;
    d.dyn_door_id = s.dyn_door_id;
    d.has_script = s.has_script;
    d.y = d.default_y;
    d.x = d.default_x;
    d.z = d.default_z;
    d.heading = d.default_heading;
    d.door_angle = d.default_door_angle;
    d.scale_factor = static_cast<std::int16_t>(s.size);
    d.top_speed1 = s.top_speed1;
    d.top_speed2 = s.top_speed2;
    if (d.type != 0x35) reset_switch_state(d, d.state);
    d.usable = s.usable;
    d.remain_open = s.remain_open;
    d.visible = s.visible;
    d.heading_changed = s.heading_changed;
    if (s.stamp_time) d.timestamp = now_ms;
    // PostInit.
    if (d.type >= 100 && d.type <= 102) {
        d.top_speed1 = top_speed(d.type, &d.top_speed2);
        d.type = 0x9c;
    } else if (d.type >= 105 && d.type <= 107) {
        d.top_speed1 = top_speed(d.type, &d.top_speed2);
        d.type = 0x9d;
    } else if (d.type == 0x73 || d.type == 0x74) {
        d.top_speed1 = top_speed(d.type, &d.top_speed2);
    }
    if (d.scale_factor == 0xff || d.scale_factor < 1) d.scale_factor = 100;
    if (d.key == 0) d.key = -1;
    if (d.remain_open) d.state = kSwitchOpening;
    d.timestamp = now_ms;
    return d;
}

bool change_state(Door& d, std::uint8_t state, std::uint32_t now_ms) {
    if (d.remain_open) {
        d.state = kSwitchOpen;
        if (state != kSwitchOpening) state = kSwitchOpen;
    }
    if (state == kSwitchClosing) d.accel = d.top_speed1;
    else if (state == kSwitchOpen) d.accel = 0.0f;
    std::uint8_t t = d.type;
    if (t == 0x39 || t == 0x3a || t == 0x9f || t == 0xa0 || t == 0xa1) return false;
    if (d.state == state) return false;
    d.state = state;
    d.timestamp = now_ms;
    if (d.sound_states.size() < 8) d.sound_states.push_back(d.state);
    return true;
}

std::uint32_t repop_frequency(const Door& d) {
    std::uint32_t f = 20000;
    switch (d.type) {
    case 0x00: case 0x05: f = 10000; break;
    case 0x23: case 0x24: case 0x74: case 0x91: case 0x97: f = 6000; break;
    case 0x28: case 0x2d: case 0x32:
        if (d.param > 0) f = d.param == 0x7d01 ? 1800000u : static_cast<std::uint32_t>(d.param);
        break;
    case 0x35: f = 1; break; // (Dependent == 0); RoF2 records carry no dependency
    case 0x39: case 0x3a: case 0x8c: f = 2000; break;
    case 0x3b: f = d.param > 0xaa ? 0 : 20000; break;
    case 0x73: f = 1; break;
    case 0x78: case 0x7d: case 0x87: case 0x92: case 0x98: f = 3000; break;
    case 0x82: f = 800; break;
    case 0x96: f = 4000; break;
    case 0x9c: case 0x9d: f = 0; break;
    default: break;
    }
    if (d.param == -2 || d.param == -3) f = 0;
    return f;
}

bool is_floor_switch(const Door& d, std::int32_t zone) {
    std::uint8_t t = d.type;
    if ((t >= 0x99 && t <= 0x9b) || (t >= 0x1e && t <= 0x2d) || (t >= 0x36 && t <= 0x3b) || (t >= 0x32 && t <= 0x33))
        return true;
    if (zone > 0x2ff) zone &= 0xffff;
    switch (zone) {
    case 0x9a: return t == 0x1b;
    case 0xcd: case 0xd4: return t == 0x1a;
    case 0xd0: return t == 8;
    case 0x192: return d.name == "OBJ_SHADE_ICESTAT" || d.name == "OBJ_SHADE_SECRET";
    case 0x194: return d.name == "OBJ_VM_CRYPTDOORA";
    case 0x1a3: return d.name == "OBJ_ZONEIN" || d.name == "OBJ_LIBTELPORTPAD";
    case 0x1a5: return d.name == "OBJ_SUNDIAL_PAD";
    default: return false;
    }
}

bool door_has_actor(const Door& d) { return d.type != 0x35; }

bool door_is_invisible(const Door& d) {
    return d.type == 0x35 || d.type == 0x36 || d.type == 0x32 || (d.type == 0x39 && d.default_door_angle == 1.0f);
}

bool door_blocks_movement(const Door& d) {
    FilterActor f;
    f.has_owner = true;
    f.type = kActorSwitch;
    f.switch_found = true;
    f.switch_type = d.type;
    return door_has_actor(d) && collision_filter_movement(f, false) != 0;
}

bool door_can_be_used(const Door& d) {
    if (d.type == 0x36 || d.type == 0x37 || d.type == 0x38 || !d.usable || !d.visible) return false;
    if (d.type == 0x9e) return true; // a book (OP_ReadBook in the original)
    // GetSwitchDamage is 0 for every type in RoF2 records without trap data.
    bool moving = d.state == kSwitchOpening || d.state == kSwitchClosing;
    if (d.type != 0x9d && moving) return false;
    if (d.state == kSwitchOpen && d.param == -2) return false;
    return true;
}

void update_particle_switch_day_night(Door& d, int hour, bool rain_screen) {
    if (d.type != 0x35) return;
    const bool day = hour >= 5 && hour <= 18;
    if (d.default_door_angle == 1.0f) d.state = day ? kSwitchOpen : kSwitchClosed;
    else if (d.default_door_angle == 2.0f) d.state = rain_screen || !day ? kSwitchOpen : kSwitchClosed;
}

namespace {

mechanics::Switch to_switch(const Door& d) {
    mechanics::Switch s;
    s.type = d.type;
    s.state = d.state;
    s.default_state = d.default_state;
    s.timestamp = d.timestamp;
    s.custom = d.param;
    s.base[0] = d.default_y;
    s.base[1] = d.default_x;
    s.base[2] = d.default_z;
    s.base_heading = d.default_heading;
    s.base_pitch = d.default_door_angle;
    s.spin = d.top_speed1;
    s.spin_accel = d.top_speed2;
    s.position[0] = d.y;
    s.position[1] = d.x;
    s.position[2] = d.z;
    s.heading = d.heading;
    s.pitch = d.door_angle;
    s.swing_speed = d.accel;
    s.swing_stop = d.heading_changed;
    s.particle = d.particle;
    return s;
}

void from_switch(const mechanics::Switch& s, Door& d) {
    d.state = s.state;
    d.timestamp = s.timestamp;
    d.y = s.position[0];
    d.x = s.position[1];
    d.z = s.position[2];
    d.heading = s.heading;
    d.door_angle = s.pitch;
    d.accel = s.swing_speed;
    d.heading_changed = s.swing_stop;
    d.particle = s.particle;
}

// ProcessSwitches' callees for the client's doors (game/mechanics/switches.h).
class DoorFrame final : public mechanics::SwitchQueries {
public:
    DoorFrame(std::vector<Door*>& doors, std::vector<mechanics::Switch>& switches_, std::uint32_t now,
              std::int32_t zone, const DoorRiders* riders, const DoorParticles* particles)
        : switches(switches_), doors_(doors), now_(now), zone_(zone), riders_(riders), particles_(particles) {}
    std::vector<mechanics::Switch>& switches;
    std::vector<std::uint8_t> use;

    void change_state(mechanics::Switch& s, std::uint32_t state) override {
        Door& d = door(s);
        from_switch(s, d);
        game::change_state(d, static_cast<std::uint8_t>(state & 0xff), now_);
        sync(s, d);
    }
    void repop_switch(mechanics::Switch& s) override {
        Door& d = door(s);
        from_switch(s, d);
        if (game::repop_switch(d)) {
            use.push_back(d.id);
            // A deliberate difference: the original's UseSwitch online only sends OP_ClickDoor and leaves the state
            // and timestamp alone, so the click goes again every frame until the server's OP_MoveDoor comes back.
            // EQEmu's HandleClick toggles the door on each one, so the door ends open or shut by how many frames fit
            // in the round trip (doors opening on their own at a high frame rate). The timestamp is restarted
            // instead: one click per repop period.
            d.timestamp = now_;
        }
        sync(s, d);
    }
    std::uint32_t repop_frequency(const mechanics::Switch& s) override { return game::repop_frequency(door(s)); }
    std::uint8_t slide_switch_left_right(mechanics::Switch& s, std::uint32_t flag, float dt) override {
        Door& d = door(s);
        from_switch(s, d);
        bool r = game::slide_switch_left_right(d, 0.0f, flag != 0, dt, now_);
        sync(s, d);
        return r ? 1 : 0;
    }
    void update_day_night(mechanics::Switch& s) override {
        if (!particles_ || !particles_->update_day_night) return;
        Door& d = door(s);
        from_switch(s, d);
        particles_->update_day_night(d);
        sync(s, d);
    }
    bool local_player() override { return particles_ && particles_->local_player && particles_->local_player(); }
    bool start_particle(mechanics::Switch& s, std::int32_t id) override {
        if (!particles_ || !particles_->start) return false;
        Door& d = door(s);
        from_switch(s, d);
        d.particle = particles_->start(d, id);
        sync(s, d);
        return d.particle;
    }
    void stop_particle(mechanics::Switch& s) override {
        Door& d = door(s);
        from_switch(s, d);
        if (particles_ && particles_->stop) particles_->stop(d);
        d.particle = false;
        sync(s, d);
    }
    bool is_floor_switch(const mechanics::Switch& s) override { return game::is_floor_switch(door(s), zone_); }
    std::optional<double> actor_radius(const mechanics::Switch& s) override {
        const Door& d = door(s);
        if (!door_has_actor(d)) return std::nullopt;
        return riders_ && riders_->actor_radius ? riders_->actor_radius(d) : 0.0;
    }
    void actor_place(mechanics::Switch&, const float*, const float*) override {} // door_transform follows the fields
    std::span<const mechanics::SwitchSpawn> spawns() override {
        return riders_ ? riders_->spawns : std::span<const mechanics::SwitchSpawn>{};
    }
    void reset_collision_counter(std::size_t i) override {
        if (riders_ && riders_->reset_collision_counter) riders_->reset_collision_counter(i);
    }
    void set_switch_moved(std::size_t) override {}

private:
    Door& door(const mechanics::Switch& s) const { return *doors_[static_cast<std::size_t>(&s - switches.data())]; }
    static void sync(mechanics::Switch& s, const Door& d) {
        mechanics::Switch t = to_switch(d);
        s.state = t.state;
        s.default_state = t.default_state;
        s.timestamp = t.timestamp;
        s.position[0] = t.position[0];
        s.position[1] = t.position[1];
        s.position[2] = t.position[2];
        s.heading = t.heading;
        s.pitch = t.pitch;
        s.swing_speed = t.swing_speed;
        s.swing_stop = t.swing_stop;
        s.particle = t.particle;
    }
    std::vector<Door*>& doors_;
    std::uint32_t now_;
    std::int32_t zone_;
    const DoorRiders* riders_;
    const DoorParticles* particles_;
};

// `scratch.doors` holds the frame's doors; its other lists are filled here.
std::vector<std::uint8_t> run_frame(SwitchClock::Scratch& scratch, std::uint32_t elapsed_ms, std::uint32_t now_ms,
                                    std::int32_t zone, const DoorRiders* riders, const DoorParticles* particles) {
    std::vector<Door*>& doors = scratch.doors;
    DoorFrame frame(doors, scratch.switches, now_ms, zone, riders, particles);
    frame.switches.clear();
    for (Door* d : doors) frame.switches.push_back(to_switch(*d));
    scratch.list.clear();
    for (auto& s : frame.switches) scratch.list.push_back(&s);
    mechanics::process_switches(scratch.list, elapsed_ms, now_ms, frame);
    for (std::size_t i = 0; i < doors.size(); ++i) from_switch(frame.switches[i], *doors[i]);
    return std::move(frame.use);
}

} // namespace

std::vector<std::uint8_t> process_door(Door& d, float ticks, std::uint32_t now_ms) {
    auto elapsed = static_cast<std::uint32_t>(std::lround(ticks / 0.02f));
    SwitchClock::Scratch scratch;
    scratch.doors.push_back(&d);
    return run_frame(scratch, elapsed, now_ms, 0, nullptr, nullptr);
}

std::vector<std::uint8_t> process_doors(std::vector<Door>& doors, SwitchClock& clock, std::uint32_t now_ms,
                                        std::int32_t zone, const DoorRiders* riders, const DoorParticles* particles) {
    if (!clock.started) {
        clock.started = true;
        clock.last_ms = now_ms;
    }
    std::uint32_t elapsed = now_ms - clock.last_ms;
    clock.last_ms = now_ms;
    clock.scratch.doors.clear();
    for (auto& d : doors) clock.scratch.doors.push_back(&d);
    return run_frame(clock.scratch, elapsed, now_ms, zone, riders, particles);
}

glm::mat4 door_transform(const Door& d) {
    constexpr float kUnit = 2.0f * std::numbers::pi_v<float> / 512.0f;
    glm::mat4 m = glm::translate(glm::mat4(1.0f), d.world_position());
    m = glm::rotate(m, d.heading * kUnit, glm::vec3(0, 0, 1));
    m = glm::rotate(m, -d.door_angle * kUnit, glm::vec3(0, 1, 0));
    return glm::scale(m, glm::vec3(d.scale()));
}

bool door_within_reach(const Door& d, const glm::vec3& player, float height, float radius,
                      std::optional<glm::vec3> hit) {
    // EQSwitch::UseSwitch @0x511d70 takes the hit point from CEverQuest::ClickedSwitch.
    if (hit) return glm::distance(player, *hit) <= height + 20.0f;
    return glm::distance(player, d.world_position()) <= height + 20.0f + radius;
}

std::string create_actor_definition(std::string_view name) {
    std::string out(name.substr(0, 0x3f)); // steve_strncpy into a 0x40-byte buffer
    auto is_player = [&] {
        constexpr std::string_view kPlayer = "PLAYER_1";
        if (out.size() != kPlayer.size()) return false;
        for (std::size_t i = 0; i < out.size(); ++i)
            if (std::toupper(static_cast<unsigned char>(out[i])) != kPlayer[i]) return false;
        return true;
    };
    if (out.size() < 10 && !is_player()) out += "_ACTORDEF";
    return out;
}

glm::mat4 ground_object_transform(const GroundObject& o) {
    constexpr float kUnit = 2.0f * std::numbers::pi_v<float> / 512.0f;
    glm::mat4 m = glm::translate(glm::mat4(1.0f), o.position);
    m = glm::rotate(m, o.heading * kUnit, glm::vec3(0, 0, 1));
    m = glm::rotate(m, -o.tilt_y * kUnit, glm::vec3(0, 1, 0));
    m = glm::rotate(m, o.tilt_x * kUnit, glm::vec3(1, 0, 0));
    float s = std::isfinite(o.size) && o.size > 0.0f ? o.size : 1.0f;
    return glm::scale(m, glm::vec3(s));
}

GroundObjectChange ground_object_change(const GroundObject& was, const GroundObject& now) {
    const auto same = [](float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); };
    GroundObjectChange c;
    c.model = was.name != now.name;
    c.placement = !same(was.position.x, now.position.x) || !same(was.position.y, now.position.y) ||
                  !same(was.position.z, now.position.z) || !same(was.heading, now.heading) ||
                  !same(was.tilt_x, now.tilt_x) || !same(was.tilt_y, now.tilt_y) || !same(was.size, now.size);
    return c;
}

} // namespace hailstone::game
