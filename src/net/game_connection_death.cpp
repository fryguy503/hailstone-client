// Death, the respawn at bind, teleports by index and camping (game/death_report.h,
// game/zoning.h).
#include <algorithm>
#include <cctype>
#include <chrono>

#include "core/log.h"
#include "game/coords.h"
#include "game/combat.h"
#include "game/death_report.h"
#include "game/mechanics/character.h"
#include "net/game_connection.h"
#include "net/packet_io.h"
#include "net/player_packets.h"

namespace hailstone::net {

namespace {
using Clock = std::chrono::steady_clock;
} // namespace

void GameConnection::handle_death(const Death& d) {
    // Handle_OP_Death @0x532b50 (game/death_report.h).
    const std::uint32_t me = world_.player_id;
    auto find = [&](std::uint32_t id) -> game::Spawn* {
        auto it = world_.spawns.find(id);
        return it == world_.spawns.end() ? nullptr : &it->second;
    };
    game::Spawn* dead = find(d.spawn_id);
    const game::Spawn* killer = find(d.killer_id);
    game::DeathInput in;
    in.dead_known = dead != nullptr;
    in.dead_is_local = d.spawn_id == me;
    in.dead_id = d.spawn_id;
    in.killer_id = d.killer_id;
    in.killer_known = killer != nullptr;
    in.killer_is_local = killer && d.killer_id == me;
    in.local_id = me;
    in.spell = static_cast<std::int32_t>(d.spell_id);
    in.skill = static_cast<std::uint8_t>(d.attack_skill);
    in.damage = static_cast<std::int32_t>(d.damage);
    if (dead) {
        in.dead_type = dead->is_corpse ? 2 : (dead->is_npc ? 1 : 0);
        in.dead_class = dead->class_id;
        in.dead_stand_state = dead->stand_state;
        in.dead_name = dead->name;
        in.dead_shown = game::display_name(dead->name);
        // NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker): false positive, dead is non-null inside this block.
        in.distance = glm::distance(motion_known_ ? position_ : (world_.player() ? world_.player()->position : position_),
                                    dead->position);
        in.group_member = group_.active && group_.slot_of(game::display_name(dead->name)) >= 0;
    }
    if (killer) in.killer_shown = game::display_name(killer->name);
    in.local_name = character_;
    in.dead_is_duel_opponent = duel_.challenged != 0 && duel_.challenged == d.spawn_id;
    in.dueling = duel_.active;
    in.dead_is_trade_partner = trade_ && trade_->partner_id == d.spawn_id;
    in.dead_is_merchant = merchant_ && merchant_->npc_id == d.spawn_id;
    in.dead_is_banker = bank_ && bank_->banker_id == d.spawn_id;
    in.dead_is_trainer = training_ && training_->trainer_id == d.spawn_id;
    in.dead_is_target = world_.target_id != 0 && world_.target_id == d.spawn_id;
    in.in_group = group_.active;
    game::DeathResult r = game::report_death(in);
    if (!r.handled) {
        log::debug("OP_Death: spawn {} not found", d.spawn_id);
        return;
    }
    if (!dead) return; // report_death only handles known spawns; this keeps the corpse update below safe
    if (in.dead_is_local) send_position_history(true); // PlayerZoneClient::IDied @0x5a3d91
    using F = game::DeathCall::Fn;
    auto text = [&](const game::DeathChat& c) {
        return c.built ? strings_.format(c.eqstr, std::vector<std::string>{c.p1, c.p2}) : strings_.format(c.eqstr, {});
    };
    std::size_t chat_at = 0;
    auto say_next = [&](ChatEvent::Kind kind) {
        if (chat_at < r.chat.size()) chat(kind, text(r.chat[chat_at++]));
    };
    if (in.dead_is_duel_opponent) say_next(ChatEvent::Kind::System);
    for (const auto& c : r.calls) {
        switch (c.fn) {
        case F::SendDuelDecline:
            // The original's 8 bytes, dropped by stock EQEmu, which ends the duel itself in Client::Death; its 12-byte
            // form would reach a handler that does not check the dead player's entity still exists (zoned to bind).
            send_zone(encode_duel_decline(d.spawn_id, me));
            break;
        case F::TradeCancelAndClose: trade_cancel(); break;
        case F::BankWindowClose: close_bank(); break;
        case F::LootResetSession: loot_.reset(); break;
        case F::SendTrackTarget: say_next(ChatEvent::Kind::System); break; // tracking is not modelled
        case F::SetAutoAttackOff:
            // Hailstone: attack_until_xtarget_clear keeps it on while the extended target list has more to fight.
            if (attack_until_xtarget_clear_ && auto_attack_ && !in.dead_is_local && xtarget_holds_spawn(d.spawn_id)) {
                attack_held_for_xtarget_ = true;
                break;
            }
            set_auto_attack(false);
            break;
        case F::SetAutoRangeAttackOff: set_auto_fire(false); break;
        case F::GroupGetMember: break;
        case F::TrackingRemove: break;
        case F::SpawnDied: break; // below
        }
    }
    if (r.duel_ended) duel_ = {};
    if (r.merchant_window_closed) close_merchant();
    if (r.trainer_window_closed) close_trainer();
    if (r.target_cleared) {
        world_.target_id = 0;
        auto_target_due_ = true; // Hailstone (auto_target_xtarget): another attacker on the list, if any
    }
    std::erase(xtarget_attackers_, d.spawn_id);
    // ReportSuccessfulHit: the killing blow, shown when reported (or by the chat filters' pet rule).
    if (r.hit)
        if (auto line = game::damage_report(strings_, world_, d.spawn_id, d.killer_id, r.hit_type, r.hit_damage, 0))
            report_hit(*line, d.killer_id, r.hit_report);
    if (d.damage > 0 && d.attack_skill < 231) melee_sound(d.spawn_id, d.killer_id, static_cast<std::int32_t>(d.damage));
    if (r.local_died) {
        release_eye_control(true);
        clear_hover_rez();
        discard_questions([](const Confirmation& c) { return c.id == kConfirmRezz; });
        respawn_spawn_pending_ = false;
        respawn_placement_.reset();
        death_tip_pending_ = true; // 0x531730: CEverQuest +0x39580 = 1 (the death tip at the next zone-in)
        pstatus_.dead = true;
        pstatus_.owner_status = 4;
        pstatus_.hp = 0;
        set_auto_attack(false);
    }
    // PlayerZoneClient::IDied: the spawn stays as its corpse (same id) until the server deletes it.
    if (!dead->is_corpse) {
        std::string base = dead->name;
        while (!base.empty() && std::isdigit(static_cast<unsigned char>(base.back()))) base.pop_back();
        dead->name = base + "'s_corpse";
    }
    dead->is_corpse = true;
    dead->hp_percent = 0;
    dead->stand_state = 115;
    // IDied zeroes the velocities (+0x70..+0x78, speed_run +0x7c, speed_heading +0x8c) and sets fly mode 0, so a
    // spawn killed while moving drops where it died instead of sliding on with the last update's motion.
    dead->velocity = glm::vec3(0.0f);
    dead->speed_run = 0.0f;
    dead->speed_heading = 0.0f;
    dead->fly_mode = 0;
    if (dead->id == world_.eye_id) release_eye_control(true);
    // The death text: ours in colour 0x115 and on the screen, others' in 0x116 when reported.
    while (chat_at < r.chat.size()) {
        std::uint32_t color = r.chat[chat_at].color;
        std::string line = text(r.chat[chat_at++]);
        if (r.screen_text) text_overlay_.display(line, 10, 10, 0xc0, 1000, 1000, 4000, static_cast<std::uint32_t>(now_ms()));
        chat(ChatEvent::Kind::Combat, std::move(line), 0, color);
    }
}

void GameConnection::handle_zone_to_bind(const AppPacket& p) {
    // HandleWorldMessage's OP_ZonePlayerToBind case (game/zoning.h).
    std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(p.data.data()), p.data.size());
    auto packet = game::decode_zone_to_bind(bytes);
    if (!packet) return;
    const bool hover = respawn_.has_value() || respawn_spawn_pending_;
    const bool same_zone_hover = hover && packet->zone == current_zone_dword();
    // RoF2 publishes a new own spawn after a same-zone hover respawn. Until it arrives, our old ID names the
    // corpse: do not permit movement or let another spawn take control. A full zone-in resets this pending state.
    if (same_zone_hover) respawn_spawn_pending_ = true;
    clear_hover_rez();
    discard_questions([](const Confirmation& c) { return c.id == kConfirmRezz; });
    log::info("zone player to bind: zone {:#x} ({}), hp {} mana {} endurance {}", packet->zone, packet->zone_name,
              packet->hp, packet->mana, packet->endurance);
    game::ZoneToBindHooks h;
    h.deactivate_respawn_window = [&] {
        respawn_.reset();
        respawn_chosen_ = false;
    };
    h.activate_main_ui = [] {};
    h.chat = [&](std::uint32_t id, const std::string& zone, std::uint32_t color) { system(id, {zone}, color); };
    h.prepare_for_repop = [&] {
        // PcClient::PrepareForRepop: buffs removed, vitals to max, drunkenness 0.
        for (auto& b : spells_.buffs) b = {};
        pstatus_.hp = pstatus_.max_hp;
        if (!same_zone_hover) {
            pstatus_.mana = pstatus_.max_mana;
            pstatus_.endurance = pstatus_.max_endurance;
        }
        drunkenness_ = 0;
        pstatus_.dead = same_zone_hover;
        pstatus_.owner_status = same_zone_hover ? 4 : 0;
        // In a raid: CRaid::SetTargetRaidPlayer(0) and ClearInvitedState (a pending invitation is dropped).
        if (raid_.active) {
            raid_.target.clear();
            raid_.inviter.clear();
        }
        // The carried coin goes with the corpse (the PcBase virtual +0xc and the profile flag are not known: false).
        if (profile_ && game::repop_clears_coin(pvp_rules_, pstatus_.level, false, false))
            profile_->platinum = profile_->gold = profile_->silver = profile_->copper = 0;
    };
    h.set_vitals = [&](std::int32_t hp, std::int32_t mana, std::int32_t endurance) {
        pstatus_.hp = hp;
        // EQEmu's RoF2 encoder writes zero placeholders here. Resource updates can precede this packet, and
        // unchanged zero mana after a resurrection need not be sent again. Preserve the current server values.
        if (!same_zone_hover || mana != 0) pstatus_.mana = static_cast<std::uint32_t>(mana);
        if (!same_zone_hover || endurance != 0) pstatus_.endurance = static_cast<std::uint32_t>(endurance);
    };
    h.fill_air = [] {};
    h.update_spell_gems = [] {};
    h.stop_death_music = [] {};
    h.rewind_reset = [] {};
    h.send_position_list = [&] { flush_position_history(); }; // 0x58ff50
    h.move_to_zone = [&](const game::MoveToZoneCall& m) { move_to_zone(m); };
    h.local_view_setup = [] {};
    h.restore_camera = [] {};
    h.set_keep_items = [](std::uint8_t) {};
    h.finish = [] {};
    game::zone_player_to_bind(bytes.size(), *packet, current_zone_dword(), respawn_.has_value(), true, h);
}

const RespawnOption* GameConnection::respawn_resurrection_option() const {
    // EQEmu's RoF2 contract reserves the final option for resurrection and numbers the rows consecutively.
    // Do not infer a destination kind from translated text or its availability byte.
    if (!respawn_ || respawn_->options.size() < 2) return nullptr;
    for (std::size_t i = 0; i < respawn_->options.size(); ++i)
        if (respawn_->options[i].id != i) return nullptr;
    return &respawn_->options.back();
}

bool GameConnection::respawn_option_is_resurrection(std::uint32_t option_id) const {
    const auto* rez = respawn_resurrection_option();
    return rez && rez->id == option_id;
}

std::optional<std::uint32_t> GameConnection::accepted_hover_rez_option() const {
    const auto* rez = respawn_resurrection_option();
    if (hover_rez_accepted_ && hover_rez_choice_ && rez && *hover_rez_choice_ == *rez) return rez->id;
    return std::nullopt;
}

bool GameConnection::respawn_option_available(std::uint32_t option_id) const {
    if (!respawn_) return false;
    const auto option = std::find_if(respawn_->options.begin(), respawn_->options.end(),
                                    [option_id](const RespawnOption& o) { return o.id == option_id; });
    if (option == respawn_->options.end()) return false;
    if (const auto* rez = respawn_resurrection_option(); rez && rez->id == option_id) {
        if (hover_rez_choice_ && *hover_rez_choice_ == *rez) return hover_rez_accepted_;
        const auto question = [](const Confirmation& c) { return c.id == kConfirmRezz; };
        if ((confirmation_ && question(*confirmation_)) ||
            std::any_of(confirm_queue_.begin(), confirm_queue_.end(), question)) return false;
    }
    return option->available();
}

bool GameConnection::respawn_ready() const {
    return respawn_ && !respawn_chosen_ && respawn_age_ms() > static_cast<std::int64_t>(respawn_->delay_ms);
}

std::int64_t GameConnection::respawn_now_ms() const {
    const auto now = options_.session.clock ? options_.session.clock() : Clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

void GameConnection::choose_respawn(std::uint32_t option_id) {
    if (respawn_selection_sending_ || !respawn_ready() || !respawn_option_available(option_id)) return;
    const auto option = std::find_if(respawn_->options.begin(), respawn_->options.end(),
                                    [option_id](const RespawnOption& o) { return o.id == option_id; });
    if (option == respawn_->options.end()) return;
    const RespawnOption selected = *option;
    const auto session = respawn_session_;
    const auto player = world_.player_id;
    const auto current = [&] {
        return state_ == ConnectionState::InZone && !zone_change_pending_ && respawn_session_ == session &&
            world_.player_id == player && respawn_ready() && respawn_option_available(option_id) &&
            std::find(respawn_->options.begin(), respawn_->options.end(), selected) != respawn_->options.end();
    };
    // Reserve only the send, not the player's choice: a dropped packet leaves the same consent and window usable.
    // Hooks may replace that window or disconnect; the guard must not roll back any state they installed.
    respawn_selection_sending_ = true;
    struct SendGuard {
        bool& sending;
        ~SendGuard() { sending = false; }
    } guard{respawn_selection_sending_};
    if (!send_zone(encode_respawn_selection(option_id), true, current)) return;
    log::info("hover respawn selection sent: option {}", option_id);
    respawn_chosen_ = true;
    clear_hover_rez();
    discard_questions([](const Confirmation& c) { return c.id == kConfirmRezz; });
}

std::uint32_t GameConnection::current_zone_dword() const {
    return static_cast<std::uint32_t>(zone_id()) | (static_cast<std::uint32_t>(zone_instance()) << 16);
}

std::vector<game::ZonePointRow> GameConnection::zone_point_rows() const {
    std::vector<game::ZonePointRow> rows;
    rows.reserve(zone_points_.size());
    for (const auto& z : zone_points_) {
        game::ZonePointRow r;
        r.number = static_cast<std::int32_t>(z.number);
        r.x = z.y; // the packet's first float, the player's +0x64 order
        r.y = z.x;
        r.z = z.z;
        r.heading = z.heading;
        r.zone = static_cast<std::int32_t>(static_cast<std::uint32_t>(z.zone_id) | (static_cast<std::uint32_t>(z.instance_id) << 16));
        r.index_request = static_cast<std::int32_t>(z.unknown024);
        rows.push_back(r);
    }
    return rows;
}

void GameConnection::send_position_now() {
    motion_dirty_ = true;
    send_position(true);
}

void GameConnection::move_to_zone(const game::MoveToZoneCall& m) {
    if (state_ != ConnectionState::InZone || zone_change_pending_ || !zone_session_) return;
    ZoneChange z;
    z.char_name = character_;
    z.zone_id = static_cast<std::uint16_t>(m.zone & 0xffff);
    z.instance_id = static_cast<std::uint16_t>(static_cast<std::uint32_t>(m.zone) >> 16);
    z.unknown068 = static_cast<std::uint32_t>(m.arg8);
    z.unknown072 = static_cast<std::uint32_t>(m.arg9);
    z.y = m.x;
    z.x = m.y;
    z.z = m.z;
    z.zone_reason = static_cast<std::uint32_t>(m.reason);
    send_position_history(true); // CEverQuest::MoveToZone @0x5417b1
    start_zone_change(z); // the original's wait for the answer (net/game_connection.cpp)
    log::info("MoveToZone: zone {} instance {} ({:.1f}, {:.1f}, {:.1f}) reason {}", z.zone_id, z.instance_id, m.x, m.y, m.z,
              m.reason);
}

void GameConnection::send_teleport_index_request(std::int32_t zone_point) {
    waiting_for_port_ = true;
    if (state_ != ConnectionState::InZone) return;
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(zone_point));
    send_zone(AppPacket(game::kOpTeleportIndex, w.take()));
    log::info("teleport by index: zone point {} (opcode 0xcadf), waiting for the server", zone_point);
}

// ---------------------------------------------------------------- the position history

void GameConnection::record_position_sample(std::uint32_t reason, const glm::vec3* world) {
    game::PositionTracker::Gate g;
    g.in_game = zone_ready_ && (state_ == ConnectionState::InZone || state_ == ConnectionState::Camping);
    g.zone_connected = zone_session_ != nullptr;
    g.controlled = world_.player() != nullptr && !pstatus_.dead;
    g.main_loop = g.in_game;
    const glm::vec3 p = world ? *world : position_;
    // The spawn's +0x64 order (Y, X, Z): the server's y and x, which are the world's x and y.
    if (position_tracker_.record({p.x, p.y, p.z}, reason, static_cast<std::uint32_t>(now_ms()), g))
        send_position_history(true);
}

void GameConnection::send_position_history(bool force) {
    // IsSpawnInfoPlayerSpawn on the local player; nothing recorded, nothing sent.
    if (!world_.player() || position_tracker_.empty() || !zone_session_) return;
    auto now = static_cast<std::uint32_t>(now_ms());
    if (now - position_history_sent_ < 1000 && !force) return;
    for (;;) {
        std::vector<std::byte> payload = position_tracker_.take(force, now);
        if (payload.empty()) break;
        position_history_sent_ = now;
        if (position_history_packets_++ % 30 == 0 || force)
            log::info("position history: {} entries sent (packet {}{})", payload.size() / 17,
                      position_history_packets_, force ? ", forced" : "");
        send_zone(AppPacket(to_wire(Op::FloatListThing), std::move(payload)));
        if (position_tracker_.empty() || !force) break;
    }
}

void GameConnection::flush_position_history() {
    send_position_history(true);
    position_tracker_.flush();
}

// ---------------------------------------------------------------- camp

void GameConnection::camp_command(const std::string& args) {
    if (!can_use_command_now()) return;
    // Utf8StrNICmp(argument, eqstr, 4) == 0: the first four letters, case ignored.
    auto starts = [&](std::uint32_t eqstr) {
        std::string word = strings_.format(eqstr, {});
        if (word.starts_with('[')) word = eqstr == 13258 ? "desk" : "server"; // not loaded: eqstr_us.txt's texts
        for (std::size_t i = 0; i < 4; ++i) {
            char a = i < args.size() ? args[i] : '\0', b = i < word.size() ? word[i] : '\0';
            if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b))) return false;
            if (a == '\0') break;
        }
        return true;
    };
    camp_kind_ = starts(13258) ? 1 : starts(13259) ? 2 : 0;
    camp();
}

void GameConnection::camp() {
    if (pstatus_.stand_state == 0x66) return;
    if (state_ == ConnectionState::Camping) {
        cancel_camp(12290); // a second /camp
        return;
    }
    if (state_ != ConnectionState::InZone) return;
    std::int32_t mez = 0; // TotalEffect(31, 1, 0, 1, 1)
    with_character([&](game::mechanics::Character& ch) { mez = ch.total_effect(31, true, 0, true, true); });
    if (mez != 0) return system(12368);
    // Not sitting: CEverQuest::Sit, and 12291 when the player is still not sitting. The original refuses a mounted
    // player (Sit fails on a mount: 12291; already sitting on one, EQPlayer::GetMount: 12292). Hailstone (a deliberate
    // difference, docs/mechanics.md "Camp"): on a mount the camp goes ahead from the saddle, without sitting. A stock
    // EQEmu zone's Handle_OP_Camp checks neither sitting nor a mount.
    if (!mount_id() && !sitting()) {
        set_sitting(true);
        if (!sitting()) return system(12291);
    }
    send_save(0x21); // Save(0, 1, 0x21)
    send_zone(encode_camp(camp_kind_));
    state_ = ConnectionState::Camping;
    camp_started_ = Clock::now();
    camp_notices_ = 0;
    system(12293);
}

void GameConnection::cancel_camp(std::uint32_t eqstr) {
    if (state_ != ConnectionState::Camping || zone_exit_ != ZoneExit::None) return; // OP_Logout has gone
    state_ = ConnectionState::InZone;
    camp_started_ = {};
    camp_kind_ = 0;
    system(eqstr);
}

void GameConnection::camp_tick() {
    // DoPassageOfTime: 12684 every 5 s; standing by any other means than ChangePosition cancels with 12683.
    if (state_ != ConnectionState::Camping || camp_started_ == Clock::time_point{}) return;
    // Hailstone: a camp made on a mount (above) holds while the player stays mounted; getting off cancels it.
    if (pstatus_.stand_state != kStandSitting && !mount_id()) {
        cancel_camp(12683);
        return;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - camp_started_).count();
    int due = static_cast<int>(elapsed / 5);
    if (due > camp_notices_ && elapsed < 30) {
        camp_notices_ = due;
        system(12684, {std::to_string(30 - due * 5)});
    }
}

// ---------------------------------------------------------------- saves

SaveRequest GameConnection::save_request(std::uint8_t kind) const {
    SaveRequest v;
    v.kind = kind;
    const game::Spawn* me = world_.player();
    if (me) {
        glm::vec3 at = game::world_to_server(motion_known_ ? position_ : me->position);
        v.y = at.y;
        v.x = at.x;
        v.z = at.z;
        v.heading = static_cast<std::int32_t>(motion_known_ ? heading_ : me->heading); // __ftol2_sse
    }
    // PcBase_GetArmorProperties(0..21): the profile's records, the nine worn ones set from the actor first
    // (PlayerClient_GetEquipmentSlotPtr), retaining the complete five-word armor record.
    if (profile_)
        for (std::size_t i = 0; i < v.armor.size() && i < profile_->equipment.size(); ++i) v.armor[i] = profile_->equipment[i];
    if (me)
        for (std::size_t i = 0; i < 9; ++i) {
            v.armor[i].material = me->appearance.material[i];
            v.armor[i].unknown1 = me->appearance.material_variation[i];
            v.armor[i].elite_material = me->appearance.elite_material[i];
            v.armor[i].hero_forge_model = me->appearance.hero_forge[i];
            v.armor[i].material2 = me->appearance.new_armor_type[i];
        }
    v.vehicle = false; // no vehicles
    v.minutes_played = profile_ ? profile_->time_played_min : 0;
    // PcBase_SetPrimaryActor / SetSecondaryActor: the IT number of the items in slots 13 and 14 (their IDFile
    // without the "IT"), as the worn slots 7 and 8 show it.
    if (me) {
        v.primary = static_cast<std::int32_t>(me->appearance.material[7]);
        v.secondary = static_cast<std::int32_t>(me->appearance.material[8]);
    }
    v.air_supply = air_supply_;
    return v;
}

void GameConnection::send_save(std::uint8_t kind) {
    // Save @0x52ae60's conditions with state here: a local PC in the game (GameState 5, after OP_ClientReady) with
    // HP above 0; nothing while an interaction window is open or an item request is pending, and then the save
    // counter (0xde0868) is set to 300 so the next 6 s tick tries again. A save sent sets it to 0.
    if ((state_ != ConnectionState::InZone && state_ != ConnectionState::Camping) || !world_.player() ||
        player_down())
        return;
    if (transact_state().currently_interacting > 0 || spells_.pending > 0) {
        save_ticks_ = 300;
        return;
    }
    send_zone(encode_save(save_request(kind)));
    save_ticks_ = 0;
}

void GameConnection::save_tick() {
    // CharacterZoneClient::DoPassageOfTime's 6 s block for a PC (@0x45f25b): the save counter counts up, and above
    // 300 (half an hour) Save(0, 1, 0xf).
    if (!zone_ready_ || (state_ != ConnectionState::InZone && state_ != ConnectionState::Camping)) return;
    auto now = static_cast<std::uint32_t>(now_ms());
    if (save_tick_ms_ == 0) save_tick_ms_ = now;
    if (now - save_tick_ms_ <= 6000) return;
    save_tick_ms_ = now;
    if (++save_ticks_ > 300) send_save(0xf);
}

} // namespace hailstone::net
