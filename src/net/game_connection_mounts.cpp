// GameConnection: riding (game/mounts.h): the rider-mount link as spawns arrive, change and leave, the riders'
// position updates, /dismount's unseat and the mount-race jump.

#include <chrono>
#include <utility>

#include "core/log.h"
#include "game/coords.h"
#include "game/spells.h"
#include "net/ability_packets.h"
#include "net/game_connection.h"
#include "net/spell_packets.h"

namespace hailstone::net {

namespace {

constexpr std::int32_t kSpaMount = 113; // SPA 0x71: summon mount
constexpr std::uint8_t kMountJumpAction = 0x14; // DoMainLoop @0x53eb73: the word 0xa14
constexpr std::uint8_t kMountJumpSpeed = 0x0a;

// Without the app's answers, every mount has its dag and there is no room test.
class DefaultMountQueries final : public game::MountQueries {
public:
    bool mount_point(const game::Spawn&, const game::Spawn&) override { return true; }
    bool can_i_fit_here() override { return false; }
    double room_above() override { return 0.0; }
};

double steady_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

game::MountQueries& GameConnection::mount_queries() {
    static DefaultMountQueries defaults;
    return mount_queries_ ? *mount_queries_ : defaults;
}

std::uint32_t GameConnection::mount_id() const {
    const game::Spawn* me = world_.player();
    return me ? me->mount_id : 0;
}

void GameConnection::remove_mount_affect() {
    // GetPCSpellAffect(113, 0, 0) / RemovePCAffectex(it, 0): the first buff with SPA 113. The server is asked as the
    // buff window asks it (EQEmu's fade of it depops the mount); RemovePCAffectex's own OP_Buff is not sent
    // (docs/unknowns.md).
    for (int slot = 0; slot < game::kBuffSlots; ++slot) {
        const auto& b = spells_.buffs[static_cast<std::size_t>(slot)];
        if (b.empty()) continue;
        if (const game::SpellData* s = spell_table_->get(b.spell); s && s->has_effect(kSpaMount)) {
            send_zone(encode_buff_remove_request(static_cast<std::uint32_t>(slot), world_.player_id));
            return;
        }
    }
}

void GameConnection::mount_events(const game::MountEvents& ev) {
    for (std::uint32_t id : ev.messages) system(id, {}, 0xd);
    if (ev.remove_mount_affect) remove_mount_affect();
    // The dismount's CSpellBookWnd Deactivate when the book is active.
    if (ev.close_spell_book && spell_book_open_) spell_book_closed_by_game(false);
    if (ev.took_control) log::info("riding: the controls move the mount {}", world_.controlled_id);
    if (ev.released_control) log::info("riding: the controls move the player again");
}

void GameConnection::mount_up(const std::vector<std::uint32_t>& made) {
    // EQPlayer's constructor ends with MakeRiderMountUp (@0x596902), and CreateInitialActors runs it on the NPCs
    // (@0x51c846): here once a spawn's actor is made.
    for (std::uint32_t id : made) {
        auto it = world_.spawns.find(id);
        if (it == world_.spawns.end()) continue;
        game::MountEvents ev;
        const auto previous_mount = std::exchange(it->second.form_mount_id, 0u);
        if (previous_mount) {
            // do_change_form re-seats only after the replacement actor's attachment
            // points and height are known. Use the saved identity, not a name lookup.
            auto mount = world_.spawns.find(previous_mount);
            if (mount != world_.spawns.end())
                game::do_stand(world_, it->second, &mount->second, mount_queries(), ev);
        } else {
            game::make_rider_mount_up(world_, it->second, mount_queries(), ev);
        }
        mount_events(ev);
    }
}

void GameConnection::spawn_leaving(std::uint32_t id) {
    if (id == world_.eye_id || id == world_.player_id) release_eye_control(true);
    for (auto& [other_id, spawn] : world_.spawns) {
        (void)other_id;
        if (spawn.form_mount_id == id) spawn.form_mount_id = 0;
    }
    game::MountEvents ev;
    game::clear_vehicle_mount_links(world_, id, ev);
    mount_events(ev);
}

bool GameConnection::rider_update(std::uint32_t spawn_id, const Motion& m) {
    // Handle_OP_ClientUpdate @0x4b3e0a: a rider's update goes into its own physics and then, with the motion reset,
    // its mount's; a mount that has a rider ignores its own.
    auto it = world_.spawns.find(spawn_id);
    if (it == world_.spawns.end()) return false;
    game::Spawn& s = it->second;
    if (s.mount_id) {
        set_spawn_motion(s, m);
        if (auto mt = world_.spawns.find(s.mount_id); mt != world_.spawns.end()) set_spawn_motion(mt->second, m);
        log::debug("riding: {} moves its mount {} to ({:.1f}, {:.1f}, {:.1f}) speed {:.2f}", s.name, s.mount_id, m.x,
                   m.y, m.z, m.speed_run);
        return true;
    }
    if (s.rider_id) return true;
    set_spawn_motion(s, m);
    return true;
}

void GameConnection::set_controlled_motion(const glm::vec3& position, float heading, const glm::vec3& velocity,
                                           float speed_run, std::uint8_t body_wet, std::uint8_t feet_wet,
                                           std::int32_t colliding_type, float speed_heading) {
    auto it = world_.spawns.find(world_.controlled_id);
    if (it == world_.spawns.end()) return;
    game::Spawn& m = it->second;
    if (world_.eye_id == m.id && (m.position != position || m.heading != heading || m.speed_run != speed_run))
        eye_motion_dirty_ = true;
    m.position = position;
    m.heading = heading;
    m.velocity = velocity;
    m.speed_run = speed_run;
    m.body_wet = body_wet;
    m.feet_wet = feet_wet;
    m.colliding_type = colliding_type;
    m.speed_heading = speed_heading;
    m.update_time = steady_seconds();
}

void GameConnection::report_mount_jump() {
    if (state_ != ConnectionState::InZone || world_.controlled_id == 0) return;
    // The controlled spawn's PlayerAnimation vf +0xc(0x14, 1, 0, 1.0, 1, 0xff, 0), then the packet.
    play_animation(world_.controlled_id, kMountJumpAction);
    send_zone(encode_animation(static_cast<std::uint16_t>(world_.controlled_id), kMountJumpAction, kMountJumpSpeed));
}

void GameConnection::dismount_command() {
    // Cmd_dismount @0x4eac50.
    if (state_ != ConnectionState::InZone) return;
    if (pstatus_.stand_state == 102) { // 0x66
        system(6159); // How do you expect to dismount if you can't even control your own actions?
        return;
    }
    game::Spawn* me = world_.spawns.count(world_.player_id) ? &world_.spawns.at(world_.player_id) : nullptr;
    if (!me || !me->mount_id) {
        system(13285); // You are not currently on a mount.
        return;
    }
    system(13284); // Dismounting.
    remove_mount_affect();
    game::MountEvents ev;
    game::dismount(world_, *me, ev);
    mount_events(ev);
}

} // namespace hailstone::net
