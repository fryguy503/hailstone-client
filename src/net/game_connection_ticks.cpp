// GameConnection: the timed character updates of DoPassageOfTime (game/passage_of_time.h) on the connection's
// state, and the live state the character sheet is built from.

#include <algorithm>
#include <functional>

#include "game/mechanics/character.h"
#include "game/mechanics/sheet_builder.h"
#include "game/mechanics/tables.h"
#include "game/eye_control.h"
#include "game/passage_of_time.h"
#include "net/game_connection.h"

namespace hailstone::net {

namespace {

// PassageHost over the connection's own calls.
struct FunctionPassageHost final : game::PassageHost {
    std::function<void(std::uint32_t, std::uint32_t)> on_chat;
    std::function<void(int, bool)> on_remove_buff;
    std::function<std::int32_t()> on_rand;
    std::function<void()> on_stop_cast, on_end_stun, on_summon_mount;
    std::function<bool()> on_eye_present;
    std::function<void()> on_view_eye;
    void chat(std::uint32_t eqstr, std::uint32_t colour) override { on_chat(eqstr, colour); }
    void remove_buff(int slot, bool notify) override { on_remove_buff(slot, notify); }
    std::int32_t rand() override { return on_rand(); }
    void stop_cast() override { on_stop_cast(); }
    void end_stun() override { on_end_stun(); }
    void summon_mount() override { on_summon_mount(); }
    bool eye_present() override { return on_eye_present(); }
    void view_eye() override { on_view_eye(); }
};

} // namespace

void GameConnection::stun_me(std::uint32_t duration_ms, bool lean) {
    // CharacterZoneClient::StunMe @0x4432c0: nothing without a spawn or under stun immunity (TotalEffect(40) > 0).
    if (!world_.player()) return;
    std::int32_t immune = 0;
    with_character([&](game::mechanics::Character& ch) { immune = ch.total_effect(40, true, 0, true, true); });
    if (immune > 0) return;
    // Below 1000 ms a player's stun is 1000 ms (another spawn's would be nothing); nothing while one runs.
    duration_ms = std::max<std::uint32_t>(duration_ms, 1000);
    if (stun_timer_ms_ != 0) return;
    stun_timer_ms_ = now_ms() + duration_ms;
    stun_state_ = true;
    // The display's special environment 0x17 (the app's listener), the spawn's and its mount's SpeedRun 0.
    ++speed_stops_;
    // "You are stunned!" (0x30bf, colour 0x154) unless the chat filter 0x1a hides it.
    if (chat_filters_.mode(0x1a) == 0) system(0x30bf, {}, 0x154);
    if (lean) ++stun_leans_;
}

void GameConnection::end_stun() {
    // CharacterZoneClient_EndStun @0x4433d0: the listener environment updated (the app's), the timer and the stun
    // state cleared, "You are unstunned." (0x30c0, colour 0x154) unless the chat filter 0x1a hides it.
    stun_timer_ms_ = 0;
    stun_state_ = false;
    if (chat_filters_.mode(0x1a) == 0) system(0x30c0, {}, 0x154);
}

void GameConnection::update_player_consciousness(bool allow_knockout) {
    if (pstatus_.dead || !world_.player()) return;
    if (allow_knockout) {
        // ApplyDamage @0x458da6 calls KnockedOut below 1 HP while status <3. KnockedOut @0x5a3ca0 lies down
        // and stops all speeds, but only active status (<=0) changes to unconscious (3).
        if (own_hp().cur >= 1 || pstatus_.owner_status >= 3) return;
        set_stand_state(kStandLying);
        auto& player = world_.spawns.at(world_.player_id);
        player.velocity = glm::vec3(0.0f);
        player.speed_run = player.speed_heading = 0.0f;
        if (pstatus_.owner_status <= 0) {
            pstatus_.owner_status = 3;
            ++consciousness_changes_;
            system(0x324d, {}, 0xf);
        }
        return;
    }
    // DoPassageOfTime @0x45f3b4: recovery is after the HP tick, not on the HP packet itself. A server death
    // remains authoritative and cannot be undone by a local regeneration prediction.
    if (own_hp().cur > 0 && pstatus_.owner_status == 3) {
        pstatus_.owner_status = 0;
        ++consciousness_changes_;
        system(0x324c, {}, 0xd);
        set_stand_state(kStandStanding);
    }
}

game::LeadershipBonuses GameConnection::leadership_bonuses() const {
    return game::leadership_passive_bonuses(leadership_.active_ranks,
        has_gates_of_discord() && group_has_players(3), has_gates_of_discord() && raid_has_players(18));
}

game::mechanics::LiveState GameConnection::live_state() const {
    game::mechanics::LiveState live;
    live.leadership = leadership_bonuses();
    live.level = pstatus_.level;
    live.pvp_rules = pvp_rules_;
    live.class_mask = server_class_mask(); // the server extension's mask (NMS); 0 = the profile class only
    live.focus_classes = focus_class_mask();
    live.skill_rules = server_ext().skill_rules(*this);
    live.vitals = server_ext().character_vitals(*this); // the NMS patch's HP / mana / endurance detours
    live.hp = pstatus_.hp;
    live.max_hp = pstatus_.max_hp;
    live.mana = pstatus_.mana;
    live.endurance = pstatus_.endurance;
    live.fast_regen_resting = resting_for_fast_regen();
    // hunger / thirst: the profile's, then OP_Stamina's (0 starving)
    live.hunger = spells_.hunger;
    live.thirst = spells_.thirst;
    live.sitting = sitting();
    // The spawn's +0x374: when it sat down (the stand state seen changing to sitting, once a frame).
    const std::int64_t now = now_ms();
    live.sitting_ms = live.sitting && sat_down_ms_ != 0 ? static_cast<std::uint32_t>(std::max<std::int64_t>(0, now - sat_down_ms_))
                                                        : 0u;
    live.attacking = auto_attack();
    // GetEnduranceRegen reads the local player's SpeedRun, not whether a movement key is held. The controlled
    // mount's SpeedRun is published separately; PhysicsObject::vf8 leaves its rider's SpeedRun at zero.
    const game::Spawn* player = world_.player();
    live.running = running_ && speed_run_ != 0.0f && player && player->mount_id == 0 && world_.eye_id == 0;
    live.drunkenness = drunkenness_;
    live.owner_status = pstatus_.owner_status;
    live.player_id = world_.player_id;
    live.now_ms = now;
    return live;
}

void GameConnection::passage_of_time(std::int64_t now) {
    reconcile_eye_control();
    // The time since sitting down (GetHPRegen's long sit): the spawn's +0x374, kept here once a frame.
    const bool is_sitting = sitting();
    if (is_sitting && !was_sitting_) sat_down_ms_ = now;
    if (!is_sitting) sat_down_ms_ = 0;
    was_sitting_ = is_sitting;

    // DoPassageOfTime's stun check: the timer passing clears it, and ends the stun unless TotalEffect(31) (a mez,
    // whose removal ends it then).
    if (stun_timer_ms_ != 0 && now >= stun_timer_ms_ && world_.player()) {
        stun_timer_ms_ = 0;
        std::int32_t mez = 0;
        with_character([&](game::mechanics::Character& ch) { mez = ch.total_effect(31, true, 0, true, true); });
        if (mez == 0) end_stun();
    }

    if (!profile_ || !spell_table_ || !world_.player()) return;
    const bool upkeep_due = game::one_second_tick_due(spells_, now);
    const bool buff_tick_due = game::six_second_tick_due(spells_, now);
    if (!upkeep_due && !buff_tick_due) return;
    if (buff_tick_due) {
        // DoPassageOfTime @0x45f25b: drunkenness changes before ProcessAffects or the stats used by regen.
        const std::int32_t tolerance = profile_->skills.size() > 66 ? static_cast<std::int32_t>(profile_->skills[66]) : 0;
        const auto drunk = game::drunkenness_tick(drunkenness_, tolerance,
                                                 mount_id() != 0);
        drunkenness_ = drunk.value;
        ++drunkenness_updates_;
        if (drunk.changed) ++drunkenness_motion_updates_;
        if (drunk.remove_mount) {
            remove_mount_affect();
            if (int slot = own_effect_with(113); slot >= 0) clear_buff_locally(slot);
            game::MountEvents ev;
            game::dismount(world_, world_.spawns.at(world_.player_id), ev);
            mount_events(ev);
            system(0x2374, {}, 0xd);
        }
    }
    // Without the game folder's BaseData.txt row for the character the regen and the maxima the pools are clamped
    // to have nothing to work with: the ticks wait rather than clamp the server's values to nothing.
    const std::int32_t level = pstatus_.level ? pstatus_.level : profile_->level;
    if (!tables().class_levels.row(level, profile_->class_)) {
        if (buff_tick_due) spells_.last_buff_tick_ms = now;
        return;
    }

    FunctionPassageHost host;
    host.on_chat = [this](std::uint32_t eqstr, std::uint32_t colour) { system(eqstr, {}, colour); };
    host.on_remove_buff = [this](int slot, bool notify) {
        if (slot < 0 || slot >= game::kBuffSlots) return;
        if (notify) remove_own_effect(slot); // OP_Buff to the server, the slot cleared
        else spells_.buffs[static_cast<std::size_t>(slot)] = {};
        reconcile_eye_control();
    };
    host.on_rand = [this, now] {
        // The C runtime's rand() (the original's): an LCG seeded once.
        if (tick_rand_ == 0) tick_rand_ = static_cast<std::uint32_t>(now) | 1u;
        tick_rand_ = tick_rand_ * 214013u + 2531011u;
        return static_cast<std::int32_t>((tick_rand_ >> 16) & 0x7fffu);
    };
    host.on_stop_cast = [this] { stop_current_spell_cast(1); };
    host.on_end_stun = [this] { end_stun(); };
    host.on_summon_mount = [this] {
        // ProcessAffects' SPA 113 case @0x43cc48: with a summon mount buff and no mount, the mount is looked for.
        game::MountEvents ev;
        game::summon_mount_tick(world_, mount_queries(), ev);
        mount_events(ev);
    };
    host.on_eye_present = [this] { return !pstatus_.dead && game::find_eye_summon(world_) != 0; };
    host.on_view_eye = [this] { acquire_eye_control(); };

    game::TickPools pools;
    pools.hp = pstatus_.hp;
    pools.max_hp = pstatus_.max_hp;
    pools.mana = static_cast<std::int32_t>(pstatus_.mana);
    pools.endurance = static_cast<std::int32_t>(pstatus_.endurance);
    pools.dead = pstatus_.dead;
    // ProcessAllStats' first loop: the worn items' stats-disabled bytes before the sums read them.
    refresh_worn_stats_disabled();
    const game::mechanics::LiveState live = live_state();
    const auto lookup = game::mechanics::spell_lookup(*spell_table_);
    auto sheet = [&] {
        game::mechanics::CharacterSheet s = game::mechanics::build_sheet(
            *profile_, inventory_, spells_, live, new_zone_ ? &*new_zone_ : nullptr, &world_);
        s.abilities = alt_adv_.sheet_abilities();
        s.prestige_limit = game_feature(0x13);
        return s;
    };
    // The 1 s block comes first in DoPassageOfTime (@0x45ee13), the 6 s block after it (@0x45f25b).
    if (upkeep_due) {
        game::mechanics::CharacterSheet s = sheet();
        game::one_second_tick(spells_, s, tables(), lookup, now, pools, host);
    }
    if (buff_tick_due) {
        game::mechanics::CharacterSheet s = sheet(); // the buffs as the upkeep left them
        s.mana = pools.mana;
        s.endurance = pools.endurance;
        game::six_second_tick(spells_, s, tables(), lookup, now, pools, host);
    }
    pstatus_.hp = pools.hp;
    pstatus_.mana = static_cast<std::uint32_t>(std::max(pools.mana, 0));
    pstatus_.endurance = static_cast<std::uint32_t>(std::max(pools.endurance, 0));
    if (buff_tick_due) update_player_consciousness();
}

} // namespace hailstone::net
