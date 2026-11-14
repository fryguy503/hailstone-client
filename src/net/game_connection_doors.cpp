// GameConnection: doors (switches) and ground objects. The client rules are
// the original's (game/doors.h); the packets are net/door_packets.h.

#include <algorithm>

#include "core/log.h"
#include "game/coords.h"
#include "game/cursor_drop.h"
#include "game/distance.h"
#include "game/merchant.h"
#include "net/game_connection.h"
#include "net/opcodes.h"
#include "net/packet_io.h"

namespace hailstone::net {

namespace {

// eqstr_us.txt 1462 (UseSwitch's token_string_to_client 0x5b6).
constexpr std::uint32_t kCantReach = 1462;

bool object_in_reach(const std::vector<game::GroundObject>& objects, std::uint32_t id, const glm::vec3* at) {
    if (!at) return false;
    auto it = std::find_if(objects.begin(), objects.end(), [&](const game::GroundObject& o) { return o.drop_id == id; });
    // CContainerMgr::Process @0x65d740 checks the world container with 0x4dc160: the 3D distance from the ground
    // item to the player at most 20.0 (0x9c4ec0); a missing item closes it too.
    return it != objects.end() && game::player_distance(at, &it->position) <= game::kGroundItemReach;
}

game::DoorSpawn door_spawn(const DoorRecord& r) {
    game::DoorSpawn s;
    s.name = r.name;
    s.y = r.y;
    s.x = r.x;
    s.z = r.z;
    s.heading = r.heading;
    s.incline = r.incline;
    s.size = r.size;
    s.id = r.door_id;
    s.type = r.open_type;
    s.state = r.state_at_spawn;
    s.default_state = r.invert_state;
    s.param = r.door_param;
    s.usable = r.usable != 0;
    s.remain_open = r.remain_open != 0;
    s.visible = r.visible != 0;
    s.heading_changed = r.heading_changed != 0;
    s.top_speed1 = r.top_speed1;
    s.top_speed2 = r.top_speed2;
    s.stamp_time = r.stamp_time != 0;
    s.adventure_id = r.adventure_door_id;
    s.dyn_door_id = static_cast<std::uint32_t>(r.dyn_door_id);
    s.has_script = r.has_script;
    return s;
}

std::uint32_t switch_time() { return static_cast<std::uint32_t>(GameConnection::now_ms()); }

} // namespace

const game::Door* GameConnection::door(std::uint8_t id) const {
    for (const auto& d : doors_)
        if (d.id == id) return &d;
    return nullptr;
}

void GameConnection::handle_door(Op op, const AppPacket& p) {
    switch (op) {
    case Op::SpawnDoor:
        // HandleWorldMessage case 0x7291: a new switch per record (added to
        // the ones already there; OP_RemoveAllDoors clears them).
        if (auto list = decode_spawn_doors(p)) {
            auto now = switch_time();
            for (const auto& r : *list) {
                log::debug("door {} {}: ({:.1f}, {:.1f}, {:.1f}) heading {:.0f} size {} type {} state {} visible {}",
                           r.door_id, r.name, r.x, r.y, r.z, r.heading, r.size, r.open_type, r.state_at_spawn,
                           r.visible);
                game::Door d = game::make_door(door_spawn(r), now);
                auto it = std::find_if(doors_.begin(), doors_.end(), [&](const game::Door& o) { return o.id == d.id; });
                if (it != doors_.end()) *it = std::move(d);
                else doors_.push_back(std::move(d));
            }
            log::info("{} doors", doors_.size());
        } else {
            log::warn("could not decode OP_SpawnDoor ({} bytes)", p.data.size());
        }
        break;
    case Op::MoveDoor:
        // GetSwitchById then ChangeState(action).
        if (auto m = decode_move_door(p)) {
            ++door_moves_seen_;
            for (auto& d : doors_)
                if (d.id == m->door_id) {
                    game::change_state(d, m->action, switch_time());
                    log::debug("door {} ({}) -> state {}", d.id, d.name, m->action);
                    break;
                }
        }
        break;
    case Op::RemoveAllDoors:
        doors_.clear();
        break;
    case Op::GroundSpawn:
        if (auto o = decode_ground_object(p)) {
            game::GroundObject g;
            g.drop_id = o->drop_id;
            g.name = o->name;
            g.position = game::server_to_world({o->x, o->y, o->z});
            g.heading = o->heading;
            g.tilt_x = o->tilt_x;
            g.tilt_y = o->tilt_y;
            g.size = o->size;
            g.type = o->object_type;
            std::erase_if(ground_objects_, [&](const game::GroundObject& x) { return x.drop_id == g.drop_id; });
            log::debug("ground object {} {} type {} at ({:.1f} {:.1f} {:.1f})", g.drop_id, g.name, g.type, g.position.x,
                       g.position.y, g.position.z);
            ground_objects_.push_back(std::move(g));
        } else {
            log::warn("could not decode OP_GroundSpawn ({} bytes)", p.data.size());
        }
        break;
    case Op::ClickObject:
        // The object was picked up or decayed: the ground item goes away.
        if (auto c = decode_click_object(p))
            std::erase_if(ground_objects_, [&](const game::GroundObject& x) { return x.drop_id == c->drop_id; });
        break;
    case Op::ClickObjectAction:
        if (auto a = decode_click_object_action(p)) {
            if (a->drop_id == 0xffffffffu) { // someone else is using it (the server says so in chat)
                world_container_requested_.reset(); // the request is answered; nothing was opened to release
                break;
            }
            if (!world_container_requested_ || *world_container_requested_ != a->drop_id) break;
            const auto* me = world_.player();
            const auto* at = me ? (motion_known_ ? &position_ : &me->position) : nullptr;
            // Hailstone: the original opens it and Process closes it within 500 ms when it is out of reach or
            // gone; here it is released at once, with the same packet.
            if (!object_in_reach(ground_objects_, a->drop_id, at)) {
                close_world_container();
                break;
            }
            world_container_requested_.reset();
            world_container_ = game::WorldContainer{a->drop_id, a->type, a->icon, a->name};
            log::info("tradeskill container {} '{}' type {} opened", a->drop_id, a->name, a->type);
            open_tradeskill_world();
            world_container_->window = !(tradeskill_.open && tradeskill_.world);
            context_tip(0x1b); // CContainerMgr::OpenWorldContainer's last step: the world container's context tip
        }
        break;
    case Op::ClearObject:
        // Handle_OP_ClearObject @0x4b6e20 ({u8 clear, 3, u32 slot}) on the container manager: clear 0 removes the
        // world container's item in that slot (0x65ca80, a slot below the container's count), anything else
        // removes every slot (0x65ca40: RefPtrArray_RemoveAt for each of ItemBase_GetSlots). EQEmu sends
        // clear 1 after a combine emptied the container and when it closes one; the window stays open.
        if (auto c = decode_clear_object(p)) {
            if (c->clear == 0) inventory_.take(ItemSlot{kInvWorld, static_cast<std::int16_t>(c->slot), -1, -1, 0, 0});
            else
                for (std::int16_t i = 0; i < 10; ++i) inventory_.take(ItemSlot{kInvWorld, i, -1, -1, 0, 0});
        }
        break;
    default:
        break;
    }
}

void GameConnection::update_doors() {
    // CContainerMgr::Process @0x65d740: the open world container, once more than 500 ms have passed since the last
    // check (CContainerMgr::Timer). A request still waiting for its answer is not checked; its answer is.
    if (world_container_) {
        const std::int64_t now = now_ms();
        if (now > npc_checks_.world + 500) {
            npc_checks_.world = now;
            const auto* me = world_.player();
            const auto* at = me ? (motion_known_ ? &position_ : &me->position) : nullptr;
            if (!object_in_reach(ground_objects_, world_container_->drop_id, at)) close_world_container();
        }
    }
    auto use = game::process_doors(doors_, switch_clock_, switch_time(), zone_id(), door_riders_);
    // RepopSwitch "uses" types 0x28 and 0x2d again: UseSwitch(0, 0, 0xff, 0),
    // which online is an OP_ClickDoor without a player.
    for (std::uint8_t id : use) {
        const game::Door* d = door(id);
        if (!d || !game::door_can_be_used(*d)) continue;
        ClickDoor c;
        c.door_id = id;
        c.pick_skill = 0xfe;
        c.item_id = 0;
        c.player_id = 0;
        send_zone(encode(c));
    }
}

bool GameConnection::click_door(std::uint8_t door_id, float actor_radius, std::int32_t key, std::int32_t skill) {
    return use_switch(door_id, world_.player_id, actor_radius, key, skill);
}

bool GameConnection::door_in_reach(std::uint8_t door_id, float actor_radius) const {
    const game::Door* d = door(door_id);
    if (!d) return false;
    const game::Spawn* me = world_.player();
    float height = me && me->size > 0.0f ? me->size : 6.0f;
    return game::door_within_reach(*d, position_, height, actor_radius);
}

bool GameConnection::use_switch(std::uint8_t door_id, std::uint32_t user, float actor_radius, std::int32_t key,
                                std::int32_t skill) {
    if (state_ != ConnectionState::InZone) return false;
    const game::Door* d = door(door_id);
    if (!d || !game::door_can_be_used(*d)) return false;
    // A user that is a PC (the local player here) must be within reach; nobody (0) needs not.
    if (user != 0) {
        if (!door_in_reach(door_id, actor_radius)) {
            chat(ChatEvent::Kind::System, strings_.format(kCantReach, {}));
            return false;
        }
    }
    // EQSwitch::UseSwitch(user, key, skill): the key and skill CEverQuest::ClickedSwitch chose.
    ClickDoor c;
    c.door_id = door_id;
    c.item_id = static_cast<std::uint32_t>(key);
    c.pick_skill = skill;
    c.player_id = user;
    send_zone(encode(c));
    log::info("clicked door {} ({}, type {}, state {}, key {}, skill {}, user {})", d->id, d->name, d->type, d->state,
              key, skill, user);
    return true;
}

void GameConnection::set_door_state_now(std::uint8_t door_id, std::uint8_t state) {
    for (auto& d : doors_)
        if (d.id == door_id) d.state = state;
}

bool GameConnection::click_object(std::uint32_t drop_id) {
    if (state_ != ConnectionState::InZone) return false;
    auto it = std::find_if(ground_objects_.begin(), ground_objects_.end(),
                           [&](const game::GroundObject& o) { return o.drop_id == drop_id; });
    if (it == ground_objects_.end()) return false;
    const auto* me = world_.player();
    if (!me) return false;
    const auto& at = motion_known_ ? position_ : me->position;
    if (game::player_distance(&at, &it->position) >= game::kGroundItemReach) {
        chat(ChatEvent::Kind::System, strings_.format(kCantReach, {}));
        return false;
    }
    ClickObject c;
    c.drop_id = drop_id;
    c.player_id = world_.player_id;
    const auto name = it->name;
    if (it->type != 1) {
        close_world_container();
        world_container_requested_ = drop_id;
    }
    send_zone(encode(c));
    log::info("clicked object {} ({})", drop_id, name);
    return true;
}

void GameConnection::close_world_container() {
    if (!world_container_ && !world_container_requested_) return;
    send_zone(encode_close_object());
    world_container_requested_.reset();
    world_container_.reset();
    if (quantity_ && quantity_->slot.type == kInvWorld) quantity_.reset();
    discard_questions([](const Confirmation& c) { return c.id == kConfirmMoldCombine && c.slot.type == kInvWorld; });
    if (tradeskill_.world) close_tradeskill();
}

void GameConnection::show_world_container_window() {
    if (!world_container_) return;
    world_container_->window = true;
    if (tradeskill_.world) close_tradeskill();
}

void GameConnection::combine_world_container() {
    if (!world_container_ || combine_pending_) return;
    // CContainerWnd::HandleCombine's checks (net/game_connection_progress.cpp).
    std::vector<const Item*> contents;
    for (std::int16_t i = 0; i < 10; ++i) contents.push_back(inventory_.at(ItemSlot{kInvWorld, i, -1, -1, 0, 0}));
    if (!combine_checks(world_container_->type, contents)) return;
    // A world container combines as type World, slot -1 (EQEmu
    // RoF2ToServerSlot: SLOT_TRADESKILL_EXPERIMENT_COMBINE).
    send_combine(ItemSlot{kInvWorld, -1, -1, -1, 0, 0}, world_container_->type);
}

namespace {
bool no_ground(const Item& item) { // ItemBase_IsNoGround (the bag's contents too)
    if (item.no_ground) return true;
    for (const auto& c : item.contents)
        if (c.item.no_ground) return true;
    return false;
}
} // namespace

bool GameConnection::request_drop_cursor() {
    if (state_ != ConnectionState::InZone) return false;
    const Item* item = inventory_.cursor();
    if (!item) return false;
    if (refuse_kept(item, "drop")) return false; // Hailstone: a kept item stays on the cursor (game/kept_items.h)
    // LMouseUp's CanDrop(1, 1, 0, 1) @0x538b73 first: a no trade item (or one holding one, or a real estate item)
    // says why (colour 15) and stays on the cursor. The FV rules: the rule set 6 (0xddfd34 is not known here).
    if (auto r = game::can_drop(*item, pvp_rules_ == 6, true, true); r != game::CanDropResult::Droppable) {
        system(game::can_drop_message(r), {item->name}, 15);
        return false;
    }
    switch (game::ground_drop_decision(no_ground(*item), no_safe_drop())) {
    case game::DropDecision::Refused:
        chat(ChatEvent::Kind::System, strings_.format(game::kStrNoGround, {}));
        return false;
    case game::DropDecision::Disabled:
        chat(ChatEvent::Kind::System, strings_.format(game::kStrDropDisabled, {}));
        return false;
    case game::DropDecision::DropNow:
        drop_cursor_on_ground(true);
        return false;
    case game::DropDecision::Confirm:
        return true;
    }
    return false;
}

std::string GameConnection::drop_confirm_text() const {
    const Item* item = inventory_.cursor();
    return item ? strings_.format(game::kStrDropConfirm, std::vector<std::string>{item->name}) : std::string();
}

bool GameConnection::drop_cursor_on_ground(bool announce) {
    if (state_ != ConnectionState::InZone) return false;
    const Item* item = inventory_.cursor();
    if (refuse_kept(item, "drop")) return false;
    game::DropInput in;
    in.announce = announce;
    in.cursor_item = item != nullptr;
    bool sent = false;
    for (const auto& s : game::drop_held_item_steps(in)) {
        switch (s.kind) {
        case game::DropStep::Kind::Chat:
            chat(ChatEvent::Kind::System, strings_.format(s.value, std::vector<std::string>{item ? item->name : std::string()}));
            break;
        case game::DropStep::Kind::RemoveCursorItem: inventory_.take(ItemSlot::possessions(kSlotCursor)); break;
        case game::DropStep::Kind::Send:
            send_zone(make_empty(Op::GroundSpawn));
            sent = true;
            break;
        default: break;
        }
    }
    return sent;
}

} // namespace hailstone::net
