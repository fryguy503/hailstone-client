// GameConnection: groups and pets. The client rules follow the original's
// handlers (addresses below); the packets are
// net/group_packets.h and net/pet_packets.h, the state game/group.h.

#include <algorithm>
#include <charconv>
#include <cctype>
#include <functional>
#include <optional>
#include <stdexcept>

#include "core/byte_io.h"
#include "core/log.h"
#include "game/group_role_sets.h"
#include "game/group_rules.h"
#include "game/slash_commands.h"
#include "game/item_options.h"
#include "game/world_state.h"
#include "game/xtarget.h"
#include "game/leadership.h"
#include "game/option_commands.h"
#include "game/coords.h"
#include "net/game_connection.h"
#include "net/movement_packets.h"
#include "net/notice_packets.h"
#include "net/group_packets.h"
#include "net/opcodes.h"
#include "net/packet_io.h"
#include "net/pet_packets.h"
#include "net/spell_packets.h"

namespace hailstone::net {

namespace {

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// A spawn name without the trailing digits EQEmu appends ("Nmsbot001" → "Nmsbot").
std::string base_name(std::string n) {
    while (!n.empty() && std::isdigit(static_cast<unsigned char>(n.back()))) n.pop_back();
    return n;
}

// CGroupBase's member list and HandleWorldMessage_Group (game/group_rules.h,
// vector-checked) on GroupState: member handles are slot indices, and 6 is a
// member HandleWorldMessage_Group just created.
class StateGroup final : public game::GroupCalls, public game::GroupWorldCalls {
public:
    using Say = std::function<void(std::uint32_t, std::vector<std::string>)>;
    StateGroup(game::GroupState& g, const std::string& me, Say say) : g_(g), me_(me), say_(std::move(say)) {}

    // ---- CGroupBase's virtuals
    void attach_spawn(int) override {}
    void message(std::uint32_t code, int m) override {
        const std::string& name = name_of(m);
        game::GroupLine line = game::group_message_line(code, iequals(name, me_));
        if (line.eqstr) say_(line.eqstr, line.with_name ? std::vector<std::string>{game::display_name(name)} : std::vector<std::string>{});
    }
    void set_leader(int m) override {
        game::GroupSlotsModel model = model_of();
        game::group_set_leader(model, m, *this);
        g_.leader = name_of(m);
    }
    void group_vf18(int) override {}
    void group_vf1c(int) override {}
    void member_vf18(int) override {}
    void swap_members(int member, int owner) override { swap_ = {member, owner}; }

    // ---- HandleWorldMessage_Group's callees
    void say(std::uint32_t eqstr) override { say_(eqstr, {}); }
    void delete_group() override { g_.clear(); }
    void armor_filters_after_leaving() override {}
    int member_by_name(const std::string& name) override { return g_.slot_of(name); }
    int new_member(const game::GroupWorldPacket& p) override {
        fresh_ = {};
        fresh_.name = p.name2;
        fresh_.owner_name = p.name;
        fresh_.merc = p.merc == 1;
        fresh_.level = static_cast<std::uint32_t>(p.field_84);
        fresh_kind_ = p.merc;
        return kFresh;
    }
    bool add_member(int m) override {
        game::GroupSlotsModel model = model_of();
        int owner = fresh_.owner_name.empty() ? -1 : g_.slot_of(fresh_.owner_name);
        bool added = game::group_add_member(model, m, fresh_kind_, g_.has_member(fresh_.name), owner, *this);
        if (added) {
            for (std::size_t k = 0; k < 6; ++k)
                if (model.slots[k] == kFresh) {
                    g_.slots[k] = fresh_;
                    // CGroupBase_SwapMembers (not rewritten): the mercenary goes next to its owner.
                    if (swap_) place_next_to_owner(static_cast<int>(k), swap_->second);
                }
            g_.active = true;
        }
        return added;
    }
    void apply_armor_filter(const std::string&) override {}
    void pc_delete_group() override { g_.clear(); }
    bool remove_member_by_name(const std::string& name) override {
        int slot = g_.slot_of(name);
        if (slot < 0) return false;
        game::GroupSlotsModel model = model_of();
        bool removed = game::group_remove_member(model, slot, *this);
        if (removed) g_.slots[static_cast<std::size_t>(slot)] = {};
        return removed;
    }
    void rename(int m, const std::string& name) override {
        if (m >= 0 && m < 6) g_.slots[static_cast<std::size_t>(m)].name = name;
    }
    std::optional<std::string> leader_name() override {
        if (g_.leader.empty()) return std::nullopt;
        return g_.leader;
    }
    void set_member_role(int m, std::int32_t role, std::uint8_t on) override {
        // CGroupBase_SetMemberRole @0x7d6200: roles 1 main tank, 2 main assist, 3 puller (a mercenary only tanks); a
        // role taken is first cleared from every member (each clearing printed too); then 0x54dd70 code 4:
        // "%1 is now %2" 0x1da9 / 0x1daa (the member's trimmed name and GroupRole_GetName), colour 0x111. Nothing
        // changes without a leader or when the member already has that state.
        if (m < 0 || m >= 6 || role < 0 || role > 3 || g_.leader.empty()) return;
        auto& s = g_.slots[static_cast<std::size_t>(m)];
        if (s.name.empty()) return;
        bool* flag = role == 1 ? &s.main_tank : role == 2 ? &s.main_assist : role == 3 ? &s.puller : nullptr;
        const char* role_name = game::group_role_name(role);
        if (!flag || !role_name || *flag == (on != 0)) return;
        if (role != 1 && s.merc) return;
        if (on)
            for (int k = 0; k < 6; ++k) set_member_role(k, role, 0);
        *flag = on != 0;
        say_(on ? 0x1da9 : 0x1daa, {game::display_name(s.name), role_name});
    }

private:
    static constexpr int kFresh = 6;
    game::GroupSlotsModel model_of() const {
        game::GroupSlotsModel m;
        for (std::size_t k = 0; k < 6; ++k) m.slots[k] = g_.slots[k].name.empty() ? -1 : static_cast<int>(k);
        m.leader = g_.leader.empty() ? -1 : g_.slot_of(g_.leader);
        return m;
    }
    const std::string& name_of(int m) const {
        static const std::string none;
        if (m == kFresh) return fresh_.name;
        return m >= 0 && m < 6 ? g_.slots[static_cast<std::size_t>(m)].name : none;
    }
    void place_next_to_owner(int slot, int owner) {
        int next = owner + 1;
        if (next >= 6 || next == slot) return;
        std::swap(g_.slots[static_cast<std::size_t>(slot)], g_.slots[static_cast<std::size_t>(next)]);
    }
    game::GroupState& g_;
    std::string me_;
    Say say_;
    game::GroupMember fresh_;
    std::int16_t fresh_kind_ = 0;
    std::optional<std::pair<int, int>> swap_;
};

} // namespace

// ---------------------------------------------------------------- state

void GameConnection::target_xtarget(int slot) {
    // ExtendedTargetList_TargetSlot @0x548820: the slot's spawn, when it exists, becomes the target.
    if (slot < 0 || slot >= static_cast<int>(xtargets_.slots.size()) || xtarget_slot_hidden(slot)) return;
    std::uint32_t id = xtargets_.slots[static_cast<std::size_t>(slot)].spawn_id;
    if (id && world_.spawns.count(id)) set_target(id);
}

bool GameConnection::xtarget_slot_hidden(int slot) const {
    return slot >= 0 && slot < static_cast<int>(xtargets_.slots.size()) && server_ext().hide_xtarget_slot(*this, slot);
}

bool GameConnection::xtarget_holds_spawn(std::uint32_t except) const {
    for (std::size_t i = 0; i < xtargets_.slots.size(); ++i) {
        const game::XTargetSlot& s = xtargets_.slots[i];
        if (s.spawn_id == 0 || s.spawn_id == except || xtarget_slot_hidden(static_cast<int>(i))) continue;
        auto it = world_.spawns.find(s.spawn_id);
        if (it != world_.spawns.end() && !it->second.is_corpse) return true;
    }
    return false;
}

void GameConnection::attack_hold_tick() {
    if (!attack_held_for_xtarget_) return;
    if (!auto_attack_ || world_.target_id != 0) {
        attack_held_for_xtarget_ = false; // turned off, or a new target: the usual rules again
        return;
    }
    if (!xtarget_holds_spawn()) set_auto_attack(false);
}

std::uint32_t GameConnection::xtarget_attacker(std::uint32_t prefer) const {
    auto qualifies = [&](std::uint32_t id) {
        if (id == 0 || std::find(xtarget_attackers_.begin(), xtarget_attackers_.end(), id) == xtarget_attackers_.end())
            return false;
        auto it = world_.spawns.find(id);
        if (it == world_.spawns.end()) return false;
        const game::Spawn& s = it->second;
        if (!s.is_npc || s.is_corpse || !s.targetable) return false;
        // A player's pet (a charmed or summoned one fighting a duel) is left alone.
        if (s.pet_owner_id)
            if (auto o = world_.spawns.find(s.pet_owner_id); o != world_.spawns.end() && !o->second.is_npc)
                return false;
        return true;
    };
    std::uint32_t first = 0;
    for (std::size_t i = 0; i < xtargets_.slots.size(); ++i) {
        std::uint32_t id = xtargets_.slots[i].spawn_id;
        if (xtarget_slot_hidden(static_cast<int>(i)) || !qualifies(id)) continue;
        if (id == prefer) return id;
        if (!first) first = id;
    }
    return first;
}

void GameConnection::auto_target_tick() {
    if (!auto_target_due_) return;
    if (world_.target_id != 0 || state_ != ConnectionState::InZone || pstatus_.dead) {
        auto_target_due_ = false; // a target is held: nothing to do until the next hit or death
        return;
    }
    auto_target_due_ = false;
    if (!auto_target_xtarget_) return;
    if (std::uint32_t id = xtarget_attacker(auto_target_prefer_)) set_target(id);
}

void GameConnection::xtarget_request(int slot, std::uint32_t type, const std::string& name) {
    if (!game::xtarget_set_slot(xtargets_, slot, type, name)) return;
    const auto& s = xtargets_.slots[static_cast<std::size_t>(slot)];
    send_zone_packet(encode_xtarget_request(static_cast<std::uint32_t>(slot), s.type, s.name));
}

void GameConnection::set_xtarget_auto_haters(bool on) {
    if (on == xtargets_.auto_haters) return;
    xtargets_.auto_haters = on;
    send_zone_packet(AppPacket(to_wire(Op::XTargetAutoAddHaters), {static_cast<std::byte>(on ? 1 : 0)}));
}

void GameConnection::select_xtarget(int slot) {
    if (slot >= 0 && slot < static_cast<int>(xtargets_.slots.size())) xtargets_.selected = slot;
}

void GameConnection::target_next_xtarget() {
    // The loop of ExtendedTargetList_TargetNextSlot: the cursor advances (wrapping to 0) until a slot with a spawn
    // id above 0 was seen and the cursor is not back at its start; then that slot's spawn, when it exists.
    int n = static_cast<int>(xtargets_.slots.size());
    if (n == 0) return;
    // A client guard: a cursor past a list the server shrank would never come back to its start (the loop would not
    // end when no slot holds a spawn); it goes on from the last slot instead, so slot 0 is looked at first.
    int start = std::clamp(xtargets_.current, 0, n - 1), at = start;
    bool seen = false;
    for (;;) {
        at = at + 1 >= n ? 0 : at + 1;
        xtargets_.current = at;
        std::uint32_t id = at < n ? xtargets_.slots[static_cast<std::size_t>(at)].spawn_id : 0;
        if (xtarget_slot_hidden(at)) id = 0; // a hidden slot is passed by
        if (static_cast<std::int32_t>(id) > 0) seen = true;
        if (at == start) return;
        if (seen) {
            if (world_.spawns.count(id)) set_target(id);
            return;
        }
    }
}

void GameConnection::add_xtarget() {
    auto it = world_.spawns.find(world_.target_id);
    if (it == world_.spawns.end()) return;
    int slot = game::xtarget_first_auto_hater_slot(xtargets_, [this](int i) { return xtarget_slot_hidden(i); });
    if (slot < 0 || it->second.is_corpse) return; // spawn type 0 a player, 1 an NPC; corpses refused
    xtarget_request(slot, it->second.is_npc ? game::kXTargetSpecificNpc : game::kXTargetSpecificPc, it->second.name);
}

void GameConnection::remove_last_xtarget() {
    int slot = game::xtarget_last_specific_slot(xtargets_);
    if (slot >= 0) xtarget_request(slot, game::kXTargetAutoHater, std::string());
}

void GameConnection::remove_target_xtargets() {
    if (!world_.target_id) return;
    for (int slot = game::xtarget_specific_slot_by_spawn(xtargets_, world_.target_id); slot != -1;
         slot = game::xtarget_specific_slot_by_spawn(xtargets_, world_.target_id))
        xtarget_request(slot, game::kXTargetAutoHater, std::string());
}

void GameConnection::reset_group_for_zone() {
    // A new zone re-sends the group (RefreshGroupFromDB → OP_GroupUpdateB) and
    // the pets (OP_PetList, the pet's spawn); an open invitation lapses.
    group_.clear();
    group_.inviter.clear();
    pets_.clear();
    pet_buffs_.clear();
    // The extended target list: the slots hold this zone's spawns, and the server starts each zone with auto
    // haters in every slot (EQEmu Client::Client); the window restores its roles (docs/unknowns.md).
    std::uint32_t zone = xtargets_.zone + 1;
    xtargets_ = {};
    xtargets_.zone = zone;
    xtarget_attackers_.clear();
    auto_target_due_ = false;
    auto_target_prefer_ = 0;
    update_pet_presence();
}

void GameConnection::set_pet_list(std::vector<game::PetInfo> list) {
    pets_.list = std::move(list);
    pets_.have_list = true;
    bool focused_listed = std::any_of(pets_.list.begin(), pets_.list.end(),
                                      [&](const game::PetInfo& e) { return e.spawn_id == pets_.focused; });
    if (!focused_listed) pets_.focused = pets_.list.empty() ? 0 : pets_.list.front().spawn_id;
    update_pet_presence();
    log::debug("pet list: {} pet(s)", pets_.list.size());
}

void GameConnection::update_pet_presence() {
    // CPetInfoWnd_RefreshPetState @0x7104a0 (from EQPlayer::EQPlayer when a spawn arriving is the player's pet, from
    // ~PlayerClient when the pet goes, and every 100 ms): there is a pet when PetID's spawn is here; when that
    // changes to there being one, ReissueToggledPetCommands (game::pet_commands_to_reissue) sends the toggles that
    // are on to the new pet, before the packets after its spawn (the server's OP_PetCommandState among them). The
    // window's showing and hiding (PopupOnPet) is the window's own (app/lua_eq_misc.cpp).
    bool has = pet_window_linked() && pets_.focused && world_.spawns.count(pets_.focused);
    if (has == pet_present_) return;
    pet_present_ = has;
    if (!has) return;
    auto commands = game::pet_commands_to_reissue(pet_toggles_, pet_window_.usable);
    // IssuePetCommand sends on whatever zone connection there is (the pet may arrive while zoning in).
    for (std::uint32_t command : commands) send_zone(encode(PetCommandPacket{command, pets_.focused, 1}));
    if (!commands.empty()) log::debug("pet {}: {} toggled pet command(s) re-sent", pets_.focused, commands.size());
}

void GameConnection::note_pet_spawn(const game::Spawn& s) {
    // A spawn arriving with the player as its owner becomes the pet the
    // window shows (the original sets the player's PetID from the spawn's
    // MasterID); NMS re-sends the focused pet's spawn when the focus changes.
    if (world_.player_id == 0 || s.pet_owner_id != world_.player_id || s.is_corpse || s.id == world_.player_id) return;
    if (pets_.focused != s.id) {
        pets_.focused = s.id;
        pets_.pet_target = 0;
        pet_buffs_.clear();
    }
    update_pet_presence();
}

void GameConnection::forget_pet_spawn(std::uint32_t spawn_id) {
    if (pets_.focused != spawn_id) return;
    // NMS despawns and respawns the focused pet within one update; keep the
    // focus on a listed pet so the window does not flicker, else drop it.
    bool listed = std::any_of(pets_.list.begin(), pets_.list.end(),
                              [&](const game::PetInfo& p) { return p.spawn_id == spawn_id; });
    if (listed) {
        update_pet_presence(); // the spawn is gone until NMS sends it again
        return;
    }
    pets_.focused = 0;
    pets_.pet_target = 0;
    pet_buffs_.clear();
    for (const auto& [id, s] : world_.spawns)
        if (s.pet_owner_id == world_.player_id && world_.player_id && !s.is_corpse && id != world_.player_id) {
            pets_.focused = id;
            break;
        }
    update_pet_presence();
}

// ---------------------------------------------------------------- packets

void GameConnection::handle_group(Op op, const AppPacket& p) {
    auto say = [&](std::uint32_t id, std::vector<std::string> args = {}) {
        chat(ChatEvent::Kind::System, strings_.format(id, args));
    };
    const std::string me = character_;
    switch (op) {
    case Op::UpdateLeadershipAA:
        if (auto u = decode_leadership_rank(p); u && u->ability < leadership_.ranks.size()) {
            leadership_.ranks[u->ability] = u->rank;
            (u->ability >= static_cast<std::uint32_t>(game::kFirstRaidAbility) ? leadership_.raid_points
                                                                                : leadership_.group_points) = u->points_left;
        }
        break;
    case Op::LeadershipExpUpdate:
        if (auto e = decode_leadership_exp(p)) {
            leadership_.group_exp = e->group_exp;
            leadership_.raid_exp = e->raid_exp;
            leadership_.group_points = e->group_points;
            leadership_.raid_points = e->raid_points;
        }
        break;
    case Op::GroupInvite:
    case Op::GroupInvite2:
        // Handle_OP_GroupInvite2 @0x525400 (both opcodes; game/group_rules.h): 12280, then a pending invitation
        // declines with 1, a raid invitation with 4, a raid we are in (the inviter not a member, or we lead a raid
        // group) with 5; otherwise 12281 and the invitation waits for Follow / Decline, keeping the u32 at +0x94.
        if (auto inv = decode_group_invite(p)) {
            game::GroupInviteState st;
            st.invite_pending = !group_.inviter.empty();
            st.follow_with_group_follow = group_.invite_via_first_opcode;
            st.pending_word_148 = group_.invite_cookie;
            st.pending_inviter = group_.inviter;
            game::GroupInviteInput in;
            in.opcode = op == Op::GroupInvite ? game::kOpGroupInvite : game::kOpGroupInvite2;
            in.inviter = inv->inviter;
            in.my_name = me;
            in.word_148 = inv->follow_cookie;
            in.raid_invited = !raid_.inviter.empty();
            in.in_raid = raid_.active;
            in.inviter_raid_member = raid_.is_member(inv->inviter);
            if (const game::RaidMember* m = raid_.find(me)) in.raid_group_leader = m->group_leader;
            game::GroupInviteResult r = game::handle_group_invite(st, in);
            for (const auto& c : r.calls) {
                if (c.fn == game::GroupInviteCall::Fn::FormatMessage) say(static_cast<std::uint32_t>(c.value), {inv->inviter});
                else if (c.fn == game::GroupInviteCall::Fn::String) say(static_cast<std::uint32_t>(c.value));
                // PlayerClient::SetInvited @0x58b740: set to 1, the group invitation's context tip
                else if (c.fn == game::GroupInviteCall::Fn::PlayerSetInvited && c.value) context_tip(0xc);
            }
            if (r.cancel) {
                GroupCancel c;
                c.inviter = r.cancel->inviter;
                c.invitee = r.cancel->me;
                c.reason = r.cancel->reason;
                send_zone(encode(c));
            } else {
                group_.inviter = st.pending_inviter;
                group_.invite_cookie = st.pending_word_148;
                group_.invite_via_first_opcode = st.follow_with_group_follow;
            }
        }
        break;
    case Op::GroupFollow:
    case Op::GroupFollow2:
        // EQEmu echoes the invitee's acceptance to the inviter; the original's
        // group handler has no case for it (the group packets follow).
        if (auto f = decode_group_follow(p)) log::debug("group: {} accepted {}'s invitation", f->invitee, f->inviter);
        break;
    case Op::CancelInvite:
        // Handle_OP_CancelInvite @0x524d90: the player we invited declined; the line for the reason at +0x94 (1-5)
        // with the declining player's name, colour 0x111. EQEmu does not forward the declining client's
        // OP_CancelInvite; the Hailstone server script does (server/README.md).
        if (auto c = decode_group_cancel(p)) {
            static constexpr std::uint32_t kReasons[5] = {12264, 12265, 12266, 5086, 5088};
            std::uint32_t reason = c->reason & 0xff; // read as a byte
            if (reason >= 1 && reason <= 5)
                chat(ChatEvent::Kind::System, strings_.format(kReasons[reason - 1], std::vector<std::string>{c->invitee}),
                     0, 0x111);
        }
        break;
    case Op::GroupAcknowledge:
    case Op::GroupUpdate:
    case Op::GroupLeaderChange:
    case Op::GroupDisbandYou:
    case Op::GroupDisbandOther:
    case Op::GroupRoles:
    case Op::GroupDelete: {
        // HandleWorldMessage_Group @0x578000 (game/group_rules.h): the member list and its chat lines, colour 0x111.
        game::GroupWorldPacket g;
        g.opcode = p.opcode;
        if (op == Op::GroupUpdate) {
            auto j = decode_group_join(p);
            if (!j) break;
            g.name = j->owner_name;
            g.name2 = j->member_name;
            g.merc = static_cast<std::int8_t>(j->merc);
            g.field_84 = static_cast<std::int32_t>(j->level);
        } else if (op == Op::GroupRoles) {
            // EQEmu's Group::SetMainTank / SetMainAssist / SetPuller and the zone-in resend.
            auto r = decode_group_role(p);
            if (!r) break;
            g.name = r->member;
            g.name2 = r->leader;
            g.role = r->role;
            g.role_on = r->on;
        } else if (op == Op::GroupLeaderChange) {
            auto l = decode_group_leader_change(p);
            if (!l) break;
            g.name2 = *l;
        } else if (auto gg = decode_group_generic(p)) {
            g.name = gg->name1;
            g.name2 = gg->name2;
        }
        StateGroup sg(group_, me, [&](std::uint32_t id, std::vector<std::string> args) { say(id, std::move(args)); });
        bool grouped = group_.active;
        game::handle_group_world_message(grouped, g, sg);
        if (!grouped) group_.clear();
        break;
    }
    case Op::GroupUpdateB:
        // Handle_OP_GroupUpdateB @0x578300 → CGroupBase_HandleGroupUpdateB @0x7d6320:
        // creates the group if needed and rebuilds every slot.
        if (auto u = decode_group_update_b(p)) {
            group_.slots = {};
            for (const auto& m : u->members) {
                if (m.slot >= static_cast<std::uint32_t>(game::kGroupSlots)) continue;
                auto& s = group_.slots[m.slot];
                s.name = m.name;
                s.level = m.level;
                s.merc = m.merc != 0;
                s.owner_name = m.owner_name;
                s.main_tank = m.main_tank != 0;
                s.main_assist = m.main_assist != 0;
                s.puller = m.puller != 0;
                s.offline = m.offline != 0;
            }
            group_.leader = u->leader;
            group_.active = true;
            // The group you invited into being: the group finder's posts end (0x5166f0 → 0x678550).
            if (creating_group_) {
                group_search_joined_group();
                creating_group_ = false;
            }
        } else {
            log::warn("could not decode OP_GroupUpdateB ({} bytes)", p.data.size());
        }
        break;
    case Op::GroupLeadershipAAUpdate:
    case Op::DelegateAbility:
    case Op::RaidDelegateAbility:
    case Op::SetGroupTarget:
    case Op::MarkNPC:
    case Op::MarkRaidNPC:
    case Op::ClearNPCMarks:
    case Op::RaidClearNPCMarks:
    case Op::InspectBuffs:
        handle_leadership_ability(op, p);
        break;
    case Op::MobManaUpdate:
    case Op::MobEnduranceUpdate:
        if (auto m = decode_mob_percent(p)) {
            auto& map = op == Op::MobManaUpdate ? group_.mana_percent : group_.endurance_percent;
            map[m->spawn_id] = m->percent;
        }
        break;
    case Op::PetCommandState:
        // Handle_OP_PetCommandState @0x70e940: the toggle byte of the button (0..9), not saved.
        if (auto st = decode_pet_command_state(p); st && st->button < pet_toggles_.size())
            pet_toggles_[st->button] = st->state != 0;
        break;
    case Op::PetHoTT:
        if (auto t = decode_pet_hott(p)) pets_.pet_target = *t;
        break;
    case Op::XTargetResponse: {
        // ExtendedTargetList_UnSerializeSlots @0x548a10 (decode_xtarget_response): the list is resized to the max;
        // an empty status turns a specific NPC slot (type 3) back into an auto hater (1).
        auto x = decode_xtarget_response(p);
        if (!x) break;
        if (x->max <= 20) {
            xtargets_.slots.resize(x->max);
            // A client guard: the cursors stay in the list (target_next_xtarget; the window's menu slot).
            const int n = static_cast<int>(x->max);
            if (xtargets_.current >= n) xtargets_.current = n > 0 ? n - 1 : 0;
            if (xtargets_.selected >= n) xtargets_.selected = -1;
        }
        for (auto& e : x->entries) {
            if (e.slot < 0 || e.slot >= static_cast<std::int32_t>(xtargets_.slots.size())) continue;
            auto& s = xtargets_.slots[static_cast<std::size_t>(e.slot)];
            s.status = e.status;
            s.spawn_id = e.spawn_id;
            s.name = std::move(e.name);
            if (e.status == 0 && s.type == 3) s.type = 1;
        }
        break;
    }
    case Op::TargetHoTT:
        // OP_TargetHoTT (4 bytes, ClientTarget_Struct as OP_PetHoTT): the target's target
        // (PlayerZoneClient::TargetOfTarget, read by CTargetOfTargetWnd and EQTypes 27 / 120).
        if (auto t = decode_pet_hott(p)) target_of_target_ = *t;
        break;
    case Op::PetBuffWindow:
        if (auto bi = decode_buff_icons(p)) {
            if (bi->all_buffs || bi->entity_id != pets_.focused) pet_buffs_.clear();
            if (bi->entity_id) pets_.focused = bi->entity_id;
            update_pet_presence();
            for (const auto& e : bi->entries) {
                auto it = std::find_if(pet_buffs_.begin(), pet_buffs_.end(), [&](const auto& b) { return b.first == e.slot; });
                if (e.spell_id == 0xffffffffu) {
                    if (it != pet_buffs_.end()) pet_buffs_.erase(it);
                    continue;
                }
                game::BuffSlot b;
                b.spell = e.spell_id;
                b.tics = e.tics;
                b.num_hits = e.num_hits;
                b.received_ms = now_ms();
                b.caster_name = e.caster;
                if (it != pet_buffs_.end()) it->second = b;
                else pet_buffs_.emplace_back(e.slot, b);
            }
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- groups

void GameConnection::group_invite(const std::string& name) {
    if (state_ != ConnectionState::InZone) return;
    // Cmd_invite @0x4e8c10: while invited, /invite accepts.
    if (!group_.inviter.empty()) {
        accept_group_invite();
        return;
    }
    // DoGroupInvite @0x524ee0.
    auto say = [&](std::uint32_t id, std::vector<std::string> args = {}) {
        chat(ChatEvent::Kind::System, strings_.format(id, args));
    };
    const std::string me = character_;
    if (group_.active && !group_.is_leader(me)) return say(12267); // only your leader may invite
    auto tit = world_.spawns.find(world_.target_id);
    const game::Spawn* target = tit == world_.spawns.end() ? nullptr : &tit->second;
    if (!target) {
        if (name.empty()) return say(12268); // You must target a player or use /invite <name>
    } else if (name.empty()) {
        if (target->is_npc || target->is_corpse) return say(12269); // You can only group with player characters.
        if (target->id == world_.player_id) return say(12270);       // You cannot invite yourself.
    }
    if (target && other_deity_team(*target)) return say(12271); // PvP rule set 4: the other deity team
    std::string who = name.empty() ? target->name : name;
    if (iequals(who, me)) return say(12270);
    if (group_.has_member(who)) return say(12272);                  // That person is already in your party.
    if (group_.active && group_.count() >= game::kGroupSlots) return say(12275); // Your party is already full.
    GroupInvite inv;
    inv.invitee = who;
    inv.inviter = me;
    send_zone(encode(inv, group_.active)); // OP_GroupInvite to form a group, OP_GroupInvite2 to add to one
    if (!group_.active) creating_group_ = true; // DoGroupInvite: bIAmCreatingGroup
    say(12273, {who}); // You invite %1 to join your group.
}

void GameConnection::accept_group_invite() {
    if (state_ != ConnectionState::InZone || group_.inviter.empty()) return;
    // Send_OP_GroupFollow2 @0x525690.
    chat(ChatEvent::Kind::System, strings_.format(12283, std::vector<std::string>{group_.inviter})); // You notify %1 that you agree to join the group.
    GroupFollow f;
    f.inviter = group_.inviter;
    f.invitee = character_;
    f.unknown148 = group_.invite_cookie;
    send_zone(encode(f, !group_.invite_via_first_opcode));
    group_search_joined_group(); // the group finder's posts end (0x678550)
    creating_group_ = false;
    group_.inviter.clear();
    group_.invite_via_first_opcode = false;
}

void GameConnection::decline_group_invite() {
    if (state_ != ConnectionState::InZone || group_.inviter.empty()) return;
    // Cmd_disband @0x4e8c50, the invited branch.
    chat(ChatEvent::Kind::System, strings_.format(12289, std::vector<std::string>{group_.inviter})); // You cancel the invitation to join %1's group.
    GroupCancel c;
    c.inviter = group_.inviter;
    c.invitee = character_;
    c.reason = kCancelDeclined;
    send_zone(encode(c));
    group_.inviter.clear();
    group_.invite_via_first_opcode = false;
}

void GameConnection::group_disband() {
    if (state_ != ConnectionState::InZone) return;
    if (!group_.inviter.empty()) return decline_group_invite();
    // CEverQuest::Disband @0x525840: a member leaves ({me, me}); the leader removes the member in the group slot
    // GroupMemberTargeted (+0x1214, read as a raw slot index) with {member, leader} and "You remove %1 from the
    // party." (12287, colour 0x111), then clears the selection; an empty slot sends nothing. Without a selection
    // the leader sends {me, me} too.
    const std::string me = character_;
    if (group_.active && group_.is_leader(me) && group_member_targeted_ > -1) {
        if (group_member_targeted_ >= game::kGroupSlots) return;
        const auto& m = group_.slots[static_cast<std::size_t>(group_member_targeted_)];
        if (m.name.empty()) return;
        std::string member = m.name;
        send_zone(encode_group_generic(Op::GroupDisband, GroupGeneric{member, group_.leader}));
        chat(ChatEvent::Kind::System, strings_.format(12287, std::vector<std::string>{member})); // You remove %1 from the party.
        group_member_targeted_ = -1;
        return;
    }
    send_zone(encode_group_generic(Op::GroupDisband, GroupGeneric{me, me}));
}

void GameConnection::target_group_member(std::int32_t n, bool target_of_target) {
    if (state_ != ConnectionState::InZone) return;
    // Spawn handles are spawn ids (0 none -> -1).
    game::GroupTargetWorld w;
    auto spawn_of = [&](const std::string& name) -> int {
        for (const auto& [id, s] : world_.spawns)
            if (!s.is_corpse && (iequals(s.name, name) || iequals(base_name(s.name), name))) return static_cast<int>(id);
        return -1;
    };
    for (std::size_t k = 0; k < static_cast<std::size_t>(game::kGroupSlots); ++k)
        if (!group_.slots[k].name.empty()) w.members[k] = spawn_of(group_.slots[k].name);
    w.selected = group_member_targeted_;
    w.target = world_.target_id ? static_cast<int>(world_.target_id) : -1;
    w.player = world_.player_id ? static_cast<int>(world_.player_id) : -1;
    w.target_id_of = [](int) { return -1; }; // other spawns' targets are not known to the client
    w.spawn_by_id = [&](std::int32_t id) { return world_.spawns.count(static_cast<std::uint32_t>(id)) ? id : -1; };
    game::target_self_or_pet(w, group_.active, n, target_of_target, w.player);
    std::uint32_t target = w.target < 0 ? 0 : static_cast<std::uint32_t>(w.target);
    group_member_targeted_ = w.selected;
    set_target(target); // a new target: the target window then stores its slot index
}

void GameConnection::group_make_leader(const std::string& name) {
    if (state_ != ConnectionState::InZone) return;
    // Cmd_makeleader @0x4ef340.
    auto say = [&](std::uint32_t id, std::vector<std::string> args = {}) {
        chat(ChatEvent::Kind::System, strings_.format(id, args));
    };
    const std::string me = character_;
    if (!group_.active) return say(3292);           // You must be in a group to perform this action.
    if (!group_.is_leader(me)) return say(7601);    // You must be a group leader to perform this action.
    std::string who = name;
    if (who.empty()) {
        auto it = world_.spawns.find(world_.target_id);
        if (it != world_.spawns.end()) who = base_name(it->second.name);
    }
    int slot = group_.slot_of(who);
    if (slot < 0) return say(5042, {who});          // %1 is not in your group.
    GroupMakeLeader m;
    m.current_leader = me;
    m.new_leader = group_.slots[static_cast<std::size_t>(slot)].name;
    send_zone(encode(m));
    send_zone(encode_group_mentor(0, {})); // the original clears the group mentor too
}

// ---------------------------------------------------------------- pets

void GameConnection::pet_command(std::uint32_t command, std::uint32_t target) {
    if (state_ != ConnectionState::InZone) return;
    // CEverQuest::IssuePetCommand @0x5209b0: 30 targets the pet locally, 34 is "none".
    if (command == kPetNone) return;
    if (command == kPetTarget) {
        for (const auto& [id, s] : world_.spawns) {
            if (world_.player_id && s.pet_owner_id == world_.player_id && id != world_.player_id && !s.is_corpse) {
                chat(ChatEvent::Kind::System, strings_.format(13104, {})); // Targeting your pet.
                set_target(pets_.focused ? pets_.focused : id);
                return;
            }
        }
        chat(ChatEvent::Kind::System, strings_.format(13091, {})); // You don't have a pet to command!
        return;
    }
    PetCommandPacket c;
    c.command = command;
    c.target = target;
    send_zone(encode(c));
}

void GameConnection::pet_command_words(const std::string& args) {
    // cmd_pet @0x4e2150.
    auto parsed = game::parse_pet_command(args);
    if (!parsed) return;
    // The toggle words write the window's toggle bytes first and save them (without a pet too).
    auto set_toggle = [&](int index, bool on) {
        game::set_pet_toggle(pet_toggles_, index, on);
        if (pet_window_.saved) pet_window_.saved(index, on);
    };
    switch (parsed->toggle) {
    case game::PetToggleAction::None:
        break;
    case game::PetToggleAction::Flip:
        set_toggle(parsed->toggle_index, !game::pet_toggle(pet_toggles_, parsed->toggle_index));
        break;
    case game::PetToggleAction::On:
        set_toggle(parsed->toggle_index, true);
        break;
    case game::PetToggleAction::Off:
        set_toggle(parsed->toggle_index, false);
        break;
    case game::PetToggleAction::OnOff:
        set_toggle(parsed->toggle_index, true);
        set_toggle(parsed->toggle_index2, false);
        break;
    }
    std::uint32_t target = 0;
    auto tit = world_.spawns.find(world_.target_id);
    const game::Spawn* t = tit == world_.spawns.end() ? nullptr : &tit->second;
    bool other = t && t->id != world_.player_id;
    if (parsed->leader_target) {
        if (t && t->is_npc) target = t->id;
    } else if (!parsed->named_target.empty()) {
        // GetNearestSpawnMaybe with NAME, #NAME, a_NAME, #a_NAME.
        const glm::vec3 from = world_.player() ? world_.player()->position : glm::vec3(0.0f);
        for (const std::string& cand : {parsed->named_target, "#" + parsed->named_target, "a_" + parsed->named_target,
                                        "#a_" + parsed->named_target}) {
            float best = 1e30f;
            for (const auto& [id, s] : world_.spawns) {
                if (id == world_.player_id || !iequals(base_name(s.name), cand)) continue;
                float d = glm::distance(s.position, from);
                if (d < best) {
                    best = d;
                    target = id;
                }
            }
            if (target) break;
        }
        if (!target) return;
    } else if (parsed->command == kPetAttack) {
        if (!other) return; // an attack with no other target sends nothing
        target = t->id;
    } else if (parsed->uses_target && other) {
        target = t->id;
    }
    // Issued only when the window offers the command (GetPetCommandAvailable).
    if (pet_window_.available && !pet_window_.available(parsed->command)) return;
    pet_command(parsed->command, target);
}

std::vector<std::uint32_t> GameConnection::pet_window_extra_pets() const {
    std::vector<std::uint32_t> out;
    if (!pets_.have_list) return out;
    for (const auto& pet : pets_.list)
        if (pet.spawn_id != pets_.focused && world_.spawns.count(pet.spawn_id)) out.push_back(pet.spawn_id);
    return out;
}

void GameConnection::focus_pet(std::uint32_t spawn_id) {
    if (state_ != ConnectionState::InZone || spawn_id == 0) return;
    // NMS: targeting one of your pets makes it the focused pet
    // (Client::Handle_OP_TargetMouse → ConfigurePetWindow); the add-on's pet
    // gauges do just that (pet_window.cpp HandleClick).
    set_target(spawn_id);
}

// ---------------------------------------------------------------- commands

bool GameConnection::group_command(const std::string& command, const std::string& args) {
    if (command == "/invite" || command == "/inv") {
        group_invite(args);
    } else if (command == "/disband") {
        group_disband();
    } else if (command == "/makeleader") {
        group_make_leader(args);
    } else if (command == "/pet") {
        pet_command_words(args);
    } else {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- group roles

bool GameConnection::can_use_command_now() {
    // CanUseCommandNow @0x4de9e0: not while the interface is locked (/safelock, 0xe15dac: 0x2f3f), nor feigned or
    // out of control (stand state 0x66: 0x3050); colour 0x111. Its other refusals (casting a spell a bard could not:
    // 0x328a; busy: 0x2f34) have no state here.
    if (interface_locked()) {
        system(game::kStrInterfaceLocked, {}, game::kLockedColor);
        return false;
    }
    if (pstatus_.stand_state == 0x66) {
        system(0x3050, {}, 0x111);
        return false;
    }
    return true;
}

void GameConnection::send_group_role(const std::string& member, std::int32_t role, bool on) {
    // Send_OP_GroupRoles @0x525d30: in a group; a mercenary takes only role 1 (else 0x1db7, colour 0xd).
    if (state_ != ConnectionState::InZone || !group_.active) return;
    // Every entry point (including a menu opened before leadership or membership changed) checks the current
    // roster. The server confirms the change; a request never changes the local role flags.
    // Hailstone, not RoF2: the leader (0x1db1) and CanUseCommandNow checks are Cmd_grouproles' (@0x4f8360); the
    // original's group window menu (CGroupWnd::WndNotification @0x67c620) sends without them, relying on Roles being
    // offered to the leader only. Here every caller gets them, so a stale menu cannot send for a former leader.
    if (!group_.is_leader(character_)) return system(0x1db1, {}, 0xd);
    if (!game::group_role_name(role)) return;
    const int slot = group_.slot_of(member);
    if (slot < 0) return;
    const auto& m = group_.slots[static_cast<std::size_t>(slot)];
    if (role != 1 && m.merc) return system(0x1db7, {}, 0xd);
    if (!can_use_command_now()) return;
    GroupRole r;
    r.member = m.name;
    r.leader = character_;
    r.role = role;
    r.on = on ? 1 : 0;
    send_zone(encode_group_role(r));
}

std::uint32_t GameConnection::group_member_roles(int slot) const {
    const auto& m = group_.slots[static_cast<std::size_t>(slot)];
    return (m.main_tank ? 1u : 0u) | (m.main_assist ? 2u : 0u) | (m.puller ? 4u : 0u);
}

void GameConnection::grouproles_command(const std::string& args) {
    // Cmd_grouproles @0x4f8360.
    if (!can_use_command_now()) return;
    if (!group_.active) return system(0xcdc, {}, 0xd);
    if (args.empty()) {
        // DisplayChatWithLineBreaks: one line per "\n" or newline.
        std::string text = strings_.format(0x1dae, {});
        std::size_t at = 0;
        while (at <= text.size()) {
            std::size_t nl = std::min(text.find('\n', at), text.find("\\n", at));
            std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            chat(ChatEvent::Kind::System, line);
            if (nl == std::string::npos) break;
            at = nl + (text.compare(nl, 2, "\\n") == 0 ? 2 : 1);
        }
        return;
    }
    // A word names an action when it is a prefix of it (case ignored; strnicmp over the word's length).
    auto is = [](std::string_view word, std::string_view action) {
        if (word.size() > action.size()) return false;
        for (std::size_t i = 0; i < word.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(word[i])) != std::tolower(static_cast<unsigned char>(action[i])))
                return false;
        return true;
    };
    std::string_view rest = args;
    std::string command = game::next_cmd_arg(rest).substr(0, 63);
    if (is(command, "list")) {
        for (int k = 0; k < game::kGroupSlots; ++k) {
            const auto& m = group_.slots[static_cast<std::size_t>(k)];
            if (m.name.empty()) continue;
            chat(ChatEvent::Kind::System, m.name + " roles: " + game::format_group_roles(group_member_roles(k), true));
        }
        return;
    }
    if (is(command, "set") || is(command, "unset")) {
        if (!group_.is_leader(character_)) return system(0x1db1, {}, 0xd);
        bool on = is(command, "set");
        std::string name = game::next_cmd_arg(rest).substr(0, 63);
        if (name.size() >= 2 && name[0] == '%' && (name[1] == 't' || name[1] == 'T')) {
            auto it = world_.spawns.find(world_.target_id);
            if (it != world_.spawns.end()) name = it->second.name;
        }
        if (group_.slot_of(name) < 0) return;
        std::string role = game::next_cmd_arg(rest).substr(0, 63);
        bool numeric = !role.empty() && std::all_of(role.begin(), role.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c));
        });
        std::int32_t number = 0;
        const auto parsed = std::from_chars(role.data(), role.data() + role.size(), number);
        if (!numeric || parsed.ec != std::errc{} || !game::group_role_name(number)) return system(0x1db0, {}, 0xd);
        send_group_role(name, number, on);
        return;
    }
    if (!is(command, "roleset")) return;
    auto list_sets = [&] {
        system(0x1da8);
        bool any = false;
        for (int k = 0; k < game::kGroupRoleSets; ++k)
            if (group_role_sets_.at(k).in_use) {
                chat(ChatEvent::Kind::System, group_role_sets_.at(k).name);
                any = true;
            }
        if (!any) system(0x4c0);
    };
    std::string action = game::next_cmd_arg(rest).substr(0, 63);
    if (action.empty() || !(is(action, "save") || is(action, "load") || is(action, "delete"))) return list_sets();
    std::string name = game::next_cmd_arg(rest).substr(0, 63);
    if (name.empty()) {
        system(0x1daf, {}, 0xd);
        return list_sets();
    }
    int index = group_role_sets_.find(name);
    if (is(action, "save")) {
        if (index == -1) index = group_role_sets_.free_slot();
        if (index == -1) return system(0x1da5, {std::to_string(game::kGroupRoleSets)}, 0xd);
        auto& set = group_role_sets_.at(index);
        set.in_use = true;
        set.changed = true;
        set.name = name.substr(0, game::kGroupRoleSetNameMax - 1);
        // The members present fill their records; an empty slot leaves its record as it was.
        for (int k = 0; k < game::kGroupSlots; ++k) {
            const auto& m = group_.slots[static_cast<std::size_t>(k)];
            if (m.name.empty()) continue;
            set.members[static_cast<std::size_t>(k)] = {group_member_roles(k), m.name};
        }
        std::string saved = set.name;
        group_role_sets_.sort();
        system(0x1da4, {saved}, 0xf);
        return;
    }
    if (is(action, "load")) {
        if (!group_.is_leader(character_)) return system(0x1db1, {}, 0xd);
        if (index < 0) return system(0x1da6, {}, 0xd);
        const auto& set = group_role_sets_.at(index);
        for (const auto& m : set.members) {
            if (m.name.empty() || group_.slot_of(m.name) < 0) continue;
            for (std::int32_t role = 1; role < 4; ++role) send_group_role(m.name, role, (m.roles & game::group_role_bit(role)) != 0);
        }
        system(0x1da7, {set.name}, 0xf);
        return;
    }
    if (index < 0) return system(0x1da6, {}, 0xd);
    auto& set = group_role_sets_.at(index);
    std::string deleted = set.name;
    set.in_use = false;
    set.changed = true;
    group_role_sets_.sort();
    system(0x1db2, {deleted}, 0xf);
}

void GameConnection::groupleader_command() {
    // Cmd_groupleader @0x4ef690: "the leader is" (0x1db3) with the leader's name, or string 0x4c0 when there is
    // none, colour 0xf; no group: 0xcdc (colour 0xd).
    if (!group_.active) return system(0xcdc, {}, 0xd);
    system(0x1db3, {group_.leader.empty() ? strings_.format(0x4c0, {}) : group_.leader}, 0xf);
}

bool GameConnection::leadership_command(std::string_view h, const std::string& args) {
    if (state_ != ConnectionState::InZone) return false;
    if (h == "Cmd_grouproles") { grouproles_command(args); return true; }
    if (h == "Cmd_groupleader") { groupleader_command(); return true; }
    // The leadership commands: Gates of Discord first (else 0x2264, colour 0xf).
    if (h == "Cmd_gmarknpc") { mark_npc_command(args, 0, 0x2265); return true; } // /gmarknpc and /marknpc
    if (h == "Cmd_rmarknpc") { mark_npc_command(args, 0x10, 0x2266); return true; }
    if (h == "Cmd_inspectbuffs") {
        // Cmd_inspectbuffs @0x4e88a0: ability 6 rank 1.
        if (!has_gates_of_discord()) return system(0x2264, {}, 0xf), true;
        activate_leadership_ability(6, 1);
        return true;
    }
    if (h == "Cmd_findpc") {
        // Cmd_findpc @0x4e8800: a name of at least 4 bytes (else 0x2273).
        if (!has_gates_of_discord()) return system(0x2264, {}, 0xf), true;
        std::string_view rest = args;
        std::string name = game::next_cmd_arg(rest).substr(0, 63);
        if (name.size() < 4) return system(0x2273, {}, 0xf), true;
        find_path_to_player(name);
        return true;
    }
    if (h == "Cmd_clearmarks") {
        // Cmd_clearmarks @0x4e87d0: a raid leader or raid marker clears the raid's marks (OP_RaidClearNPCMarks), else a
        // group leader or the group's marker the group's (OP_ClearNPCMarks); both are the bare opcode.
        if (!has_gates_of_discord()) return true;
        const std::uint32_t me = world_.player_id;
        bool raid_marker = me && std::find(leadership_.raid_markers.begin(), leadership_.raid_markers.end(), me) !=
                                     leadership_.raid_markers.end();
        if ((raid_.active && iequals(raid_.leader, character_)) || raid_marker) send_zone(make_empty(Op::RaidClearNPCMarks));
        else if (group_.is_leader(character_) || (me && leadership_.group_marker == me)) send_zone(make_empty(Op::ClearNPCMarks));
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- leadership abilities

bool GameConnection::has_gates_of_discord() const { return profile_ && (profile_->expansions & 0x40) != 0; }

bool GameConnection::group_has_players(int n) const {
    // 0x443630 → GetNumPlayersInGroup(0) @0x7d5c10: the members whose +0x14 byte is clear (taken here as the
    // members that are not offline).
    if (!group_.active) return false;
    int count = 0;
    for (const auto& m : group_.slots)
        if (!m.name.empty() && !m.offline) ++count;
    return count >= n;
}

bool GameConnection::raid_has_players(int n) const {
    return raid_.active && static_cast<int>(raid_.members.size()) >= n; // CRaid::IsInRaid, GetNumRaidMembers
}

bool GameConnection::leadership_has_rank(int ability, std::int32_t rank) const {
    // Leadership_HasAbilityRank @0x55eb10: the ability's kind (1 group, 2 raid) picks the group leader's or the raid
    // leader's ranks, at least `rank`.
    if (ability < 0 || ability >= game::kLeadershipAbilities) return false;
    return static_cast<std::int32_t>(leadership_.active_ranks[static_cast<std::size_t>(ability)]) >= rank;
}

bool GameConnection::is_raid_delegate(const std::array<std::uint32_t, 3>& ids) const {
    // 0x57f760 / 0x57f790 / 0x57f7c0: the character's own spawn id among the profile's three.
    const std::uint32_t me = world_.player_id;
    return me && std::find(ids.begin(), ids.end(), me) != ids.end();
}

bool GameConnection::leadership_ability_active(int ability, std::int32_t rank) const {
    // Leadership_IsAbilityActive @0x55eb70: Gates of Discord
    // and the rank, then per ability.
    if (!has_gates_of_discord() || !leadership_has_rank(ability, rank)) return false;
    // Who may use each ability, by ability id; an id not listed (3, 0xf, past 0x1d) is never active.
    enum class Who : std::uint8_t { None, Group, GroupLeader, GroupMarker, GroupAssist, Raid, RaidLeader, RaidMarker,
                                    RaidWaypointer };
    static constexpr Who kWho[] = {
        Who::GroupMarker, Who::Group, Who::GroupAssist, Who::None, Who::GroupLeader, Who::GroupLeader,  // 0..5
        Who::Group, Who::Group, Who::Group, Who::Group, Who::Group, Who::Group, Who::Group, Who::Group, // 6..0xd
        Who::Group, Who::None,                                                                          // 0xe, 0xf
        Who::RaidMarker, Who::Raid, Who::RaidWaypointer, Who::RaidLeader, Who::RaidLeader,              // 0x10..0x14
        Who::RaidLeader, Who::Raid, Who::Raid, Who::Raid, Who::Raid, Who::Raid, Who::Raid, Who::Raid,   // 0x15..0x1c
        Who::Raid,                                                                                      // 0x1d
    };
    if (ability < 0 || static_cast<std::size_t>(ability) >= std::size(kWho)) return false;
    const std::uint32_t me = world_.player_id;
    const bool leader = group_.is_leader(character_);     // 0x443670
    const bool raid_leader = raid_.active && iequals(raid_.leader, character_);
    const Who who = kWho[static_cast<std::size_t>(ability)];
    if (who == Who::None) return false;
    if (who == Who::Group) return group_has_players(3);
    if (who == Who::GroupLeader) return group_has_players(3) && leader;
    // mark NPC (0): the group leader or its marker (0x57f700: the profile's +0x218)
    if (who == Who::GroupMarker) return group_has_players(3) && (leader || (me && leadership_.group_marker == me));
    // the main assist (2; 0x57f730: +0x21c)
    if (who == Who::GroupAssist) return group_has_players(3) && (leader || (me && leadership_.group_assist == me));
    if (who == Who::Raid) return raid_has_players(18);
    if (who == Who::RaidLeader) return raid_leader && raid_has_players(18);
    // the raid's mark NPC (0x10): the raid leader or one of its markers (0x57f790)
    if (who == Who::RaidMarker)
        return (raid_leader || is_raid_delegate(leadership_.raid_markers)) && raid_has_players(18);
    // 0x12: the raid leader or one of the third set (0x57f7c0)
    return (raid_leader || is_raid_delegate(leadership_.raid_waypointers)) && raid_has_players(18);
}

void GameConnection::check_group_size() {
    if (!group_has_players(3)) system(0x2263, {"3"}, 0xd);
}

void GameConnection::check_raid_size() {
    if (!raid_has_players(18)) system(0x2262, {"18"}, 0xd);
}

void GameConnection::set_target_of_target_window(bool show) { window_toggle_ = show ? "targetoftarget:1" : "targetoftarget:0"; }

void GameConnection::activate_leadership_ability(int ability, std::int32_t parameter) {
    // Leadership_ActivateAbility @0x55f0f0 (CHotButtonWnd::DoHotButton's type 11 and the commands); its refusals print
    // in colour 0xd. Abilities 1, 3, 7..12, 15, 17, 22..27 (and any other) do nothing.
    if (state_ != ConnectionState::InZone) return;
    auto target = world_.spawns.find(world_.target_id);
    const game::Spawn* t = target == world_.spawns.end() ? nullptr : &target->second;
    const bool target_pc = t && !t->is_npc && !t->is_corpse; // Type 0
    const std::uint32_t me = world_.player_id;
    const bool raid_leader = raid_.active && iequals(raid_.leader, character_); // CRaid__IsRaidLeader_on_instCRaid
    // 0x4493c0 on the target's name (CharacterZoneClient_IsSelfOrGroupMember).
    auto in_group = [&](const game::Spawn& s) { return s.id == me || group_.slot_of(base_name(s.name)) >= 0; };
    auto refuse = [&](std::uint32_t id) { system(id, {}, 0xd); };
    auto send = [&](int a, std::int32_t p) {
        send_zone(encode_do_group_leadership_ability(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(p)));
    };
    switch (ability) {
    case 0: // group mark NPC 1..3; the target already holding that mark is unmarked (0)
        if (!group_has_players(3)) return check_group_size();
        if (!group_.is_leader(character_) && !(me && leadership_.group_marker == me)) return refuse(0x2234);
        if (!leadership_ability_active(0, parameter)) return refuse(0x21c2);
        if (!t) return refuse(0x21b7);
        if (parameter >= 1 && parameter <= 3 && leadership_.group_marks[static_cast<std::size_t>(parameter - 1)] == t->id)
            parameter = 0;
        return send(0, parameter);
    case 2: // the group's main assist
        if (!group_has_players(3)) return check_group_size();
        if (!group_.is_leader(character_) && !(me && leadership_.group_assist == me)) return refuse(0x2234);
        if (!leadership_ability_active(2, parameter)) return refuse(0x21c2);
        return send(2, parameter);
    case 4: case 5: { // delegate mark NPC / main assist to the target: OP_DelegateAbility 1 / 2 with its name
        if (!group_has_players(3)) return check_group_size();
        if (!group_.is_leader(character_)) return refuse(0x21c8);
        if (!leadership_ability_active(ability, parameter)) return refuse(0x21c2);
        if (!target_pc || !in_group(*t)) return refuse(0x222b);
        DelegateAbility d;
        d.ability = ability == 4 ? 1 : 2;
        d.name = base_name(t->name);
        send_zone(encode(d));
        return;
    }
    case 6: // inspect buffs: a player in the group
        if (!group_has_players(3)) return check_group_size();
        if (!leadership_ability_active(6, 1)) return refuse(0x21c2);
        if (!target_pc || !in_group(*t)) return refuse(0x225d);
        return send(6, parameter);
    case 0xd: // find path to a group member (PlayerPath_FindGroupMember)
        if (!group_has_players(3)) return check_group_size();
        if (!leadership_ability_active(0xd, 1)) return refuse(0x21c2);
        if (!target_pc || !in_group(*t)) return refuse(0x21d4);
        return send_find_path(*t);
    case 0xe: case 0x1d: // health of target's target: the window shown hides; else it is toggled
        if (target_of_target_open_) return set_target_of_target_window(false);
        if (ability == 0xe) {
            if (!group_has_players(3)) return check_group_size();
            if (!leadership_ability_active(0xe, 1)) return refuse(0x21c2);
        } else {
            if (!raid_has_players(18)) return check_raid_size();
            if (!leadership_ability_active(0x1d, 1)) return refuse(0x21c1);
        }
        return set_target_of_target_window(!target_of_target_open_);
    case 0x10: // raid mark NPC
        if (!raid_has_players(18)) return check_raid_size();
        if (!raid_leader && !is_raid_delegate(leadership_.raid_markers)) return refuse(0x2233);
        if (!leadership_ability_active(0x10, parameter)) return refuse(0x21c1);
        if (!t) return refuse(0x21b7);
        if (parameter >= 1 && parameter <= 3 && leadership_.raid_marks[static_cast<std::size_t>(parameter - 1)] == t->id)
            parameter = 0;
        return send(0x10, parameter);
    case 0x12:
        if (!raid_has_players(18)) return check_raid_size();
        if (!raid_leader && !is_raid_delegate(leadership_.raid_waypointers)) return refuse(0x2233);
        if (!leadership_ability_active(0x12, parameter)) return refuse(0x21c1);
        return send(0x12, parameter);
    case 0x13: case 0x14: case 0x15: // delegate the raid's main assist / marker / the third set to the target
        if (!raid_has_players(18)) return check_raid_size();
        if (!raid_leader) return refuse(0x21c9);
        if (!leadership_ability_active(ability, parameter)) return refuse(0x21c1);
        if (!target_pc || !raid_.is_member(base_name(t->name))) return refuse(0x222c);
        raid_delegate_ability(ability == 0x13 ? 3u : ability == 0x14 ? 4u : 5u, base_name(t->name));
        return send(ability, parameter);
    case 0x1c: // find path to a raid member (PlayerPath_FindRaidMember)
        if (!raid_has_players(18)) return check_raid_size();
        if (!leadership_ability_active(0x1c, 1)) return refuse(0x21c1);
        if (!target_pc || !raid_.is_member(base_name(t->name))) return refuse(0x21d4);
        return send_find_path(*t);
    default:
        return;
    }
}

void GameConnection::mark_npc_command(const std::string& args, int ability, std::uint32_t usage) {
    // MarkNpc (Cmd_gmarknpc @0x4e8690, Cmd_rmarknpc @0x4e8730): the mark number 1..3 in the first word (9
    // characters), else the usage line.
    if (!has_gates_of_discord()) return system(0x2264, {}, 0xf);
    std::string_view rest = args;
    std::string word = game::next_cmd_arg(rest).substr(0, 9);
    bool numeric = !word.empty() && std::all_of(word.begin(), word.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c));
    });
    int mark = numeric ? std::atoi(word.c_str()) : 0;
    if (!numeric || mark < 1 || mark > 3) return system(usage, {}, 0xf);
    activate_leadership_ability(ability, mark);
}

void GameConnection::find_path_to_player(const std::string& name) {
    // Leadership_FindPathToPlayer @0x55eee0: Find Path to PC
    // (group ability 13, raid ability 28); a raid member through the raid (a raid of 18), else a group member through
    // the group (a group of 3). The path is the server's (OP_FindPersonRequest, as /find).
    bool group = leadership_has_rank(13, 1);
    bool raid = leadership_has_rank(28, 1);
    bool in_group = iequals(name, character_) || group_.slot_of(name) >= 0; // CharacterZoneClient_IsSelfOrGroupMember
    if (!group) {
        if (!raid) return system(0x2269, {}, 0xd);
        if (name.empty() || !raid_.is_member(name)) return system(0x226a, {}, 0xd);
        if (!raid_has_players(18)) return check_raid_size();
    } else if (raid) {
        if (name.empty()) return system(0x226a, {}, 0xd);
        if (!raid_.is_member(name) || !raid_has_players(18)) {
            if (!in_group || !group_has_players(3)) return check_group_size();
        }
    } else {
        if (name.empty() || !in_group) return system(0x226a, {}, 0xd);
        if (!group_has_players(3)) return check_group_size();
    }
    // PlayerManagerClient::GetSpawnByName, then PlayerPath_FindGroupMember / FindRaidMember: a player's path.
    const game::Spawn* found = nullptr;
    for (const auto& [id, sp] : world_.spawns)
        if (!sp.is_npc && !sp.is_corpse && iequals(base_name(sp.name), name)) found = &sp;
    if (!found) return system(0x226b, {}, 0xd);
    send_find_path(*found);
}

void GameConnection::handle_leadership_ability(Op op, const AppPacket& p) {
    // HandleWorldMessage_LeadershipAbility @0x560260 (only with Gates of Discord); its lines in colour 0x14a.
    auto& l = leadership_;
    if (op == Op::GroupLeadershipAAUpdate) {
        // Handle_OP_GroupLeadershipAAUpdate (not gated): the group leader's ranks.
        if (auto u = decode_group_leadership_update(p)) {
            for (std::size_t i = 0; i < u->ranks.size(); ++i) l.active_ranks[i] = u->ranks[i];
            l.group_marker = u->npc_marker;
        }
        return;
    }
    if (!has_gates_of_discord()) return;
    auto line = [&](std::uint32_t id, std::vector<std::string> args, std::uint32_t color = 0x14a) {
        system(id, std::move(args), color);
    };
    switch (op) {
    case Op::DelegateAbility:
        if (auto d = decode_delegate_ability(p)) {
            if (d->ability == 1) { // mark NPC: 0x2240 removed, 0x2246 given
                l.group_marker = d->action == 1 ? 0 : d->spawn_id;
                line(d->action == 1 ? 0x2240 : 0x2246, {d->name});
            } else if (d->ability == 2) { // main assist: 0x2241 / 0x2247
                l.group_assist = d->action == 1 ? 0 : d->spawn_id;
                line(d->action == 1 ? 0x2241 : 0x2247, {d->name});
            }
        }
        break;
    case Op::RaidDelegateAbility:
        // 0x2b33: ability 3 the raid's main assists (the spawn id and name kept by member number; removing one also
        // clears RaidAssistNPC[number]), 4 its markers, 5 the third set; the line (0x2242 / 0x2248, 0x2243 / 0x2249,
        // 0x2244 / 0x224a with the name, colour 0x14a) and the member's flag (CRaid_SetMemberMainAssist / Marker /
        // Waypointer). The original indexes the profile's arrays with the number unchecked; numbers over 2 are not
        // stored here. EQEmu numbers its markers 0.
        if (auto d = decode_delegate_ability(p)) {
            const bool removed = d->action == 1;
            const std::size_t n = d->member;
            if (d->ability == 3) {
                if (n < 3) {
                    l.raid_assists[n] = removed ? 0 : d->spawn_id;
                    l.raid_assist_names[n] = removed ? std::string() : d->name.substr(0, 63);
                }
                if (removed && n + 1 < l.assist_targets.size()) l.assist_targets[n + 1] = 0;
                line(removed ? 0x2242 : 0x2248, {d->name});
            } else if (d->ability == 4) {
                if (n < 3) l.raid_markers[n] = removed ? 0 : d->spawn_id;
                line(removed ? 0x2243 : 0x2249, {d->name});
            } else if (d->ability == 5) {
                if (n < 3) l.raid_waypointers[n] = removed ? 0 : d->spawn_id;
                line(removed ? 0x2244 : 0x224a, {d->name});
            }
            if (d->ability >= 3 && d->ability <= 5) {
                if (game::RaidMember* m = raid_.find(d->name)) {
                    bool& flag = d->ability == 3 ? m->main_assist : d->ability == 4 ? m->marker : m->waypointer;
                    flag = !removed;
                }
            }
        }
        break;
    case Op::SetGroupTarget:
        // 0x2814 (MarkNPC_Struct): the main assist's target, number 0 the group's (GroupAssistNPC), 1..3 the raid's
        // (the original writes GroupAssistNPC[number] unchecked: 1..3 are RaidAssistNPC[0..2]).
        if (auto m = decode_mark_npc(p); m && m->number < l.assist_targets.size()) l.assist_targets[m->number] = m->spawn_id;
        break;
    case Op::MarkNPC:
    case Op::MarkRaidNPC:
        if (auto m = decode_mark_npc(p)) {
            auto& marks = op == Op::MarkNPC ? l.group_marks : l.raid_marks;
            if (m->number == 0) {
                for (auto& x : marks)
                    if (x == m->spawn_id) x = 0;
                line(0x2261, {game::display_name(m->name)});
            } else if (m->number <= 3) {
                for (auto& x : marks)
                    if (x == m->spawn_id) x = 0;
                marks[m->number - 1] = m->spawn_id;
                // The raid's line goes out through dsp_chat_2 (the default colour).
                line(0x21be, {m->name, std::to_string(m->number)}, op == Op::MarkNPC ? 0x14a : 0);
            }
        }
        break;
    case Op::ClearNPCMarks:
        l.group_marks = {};
        break;
    case Op::RaidClearNPCMarks:
        l.raid_marks = {};
        break;
    case Op::InspectBuffs:
        if (auto b = decode_inspect_buffs(p)) {
            if (b->spells[0] <= 0) return line(0x225e, {});
            line(0x2260, {});
            for (std::size_t i = 0; i < b->spells.size(); ++i) {
                if (b->spells[i] <= 0) continue;
                const game::SpellData* s = spell_table_ ? spell_table_->get(static_cast<std::uint32_t>(b->spells[i])) : nullptr;
                if (!s) continue;
                if (b->ticks[i] < 1) chat(ChatEvent::Kind::System, s->name, 0, 0x14a);
                else line(0x225f, {s->name, std::to_string(inspect_buff_minutes(b->ticks[i]))});
            }
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- leadership

void GameConnection::set_leadership_exp(bool on) {
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode_leadership_exp_toggle(on));
    leadership_.exp_on = on; // EQEmu sets it and only answers in chat
}

bool GameConnection::leadership_hot_button_enabled(int ability, int n) const {
    // 0x560180 (CLeadershipWnd_UpdateAbilityDescription's hot buttons 1..3): 0x55fb60 is the character's own
    // rank (PcBase +0x1da0).
    if (game::leadership_passive(ability) || ability < 0 || ability >= game::kLeadershipAbilities) return false;
    const auto& def = game::leadership_hot_button_def(ability);
    if (n > def.count) return false;
    return static_cast<std::int32_t>(leadership_.ranks[static_cast<std::size_t>(ability)]) >= n ||
           leadership_ability_active(ability, n);
}

std::string GameConnection::leadership_hot_button_label(int ability, int n) const {
    // CLeadershipWnd::WndNotification @0x6b7bf0, the three hotkey buttons.
    const auto& def = game::leadership_hot_button_def(ability);
    std::string text;
    for (std::size_t i = 0; i < def.words.size(); ++i) {
        if (def.words[i] == 0) continue;
        if (i > 0) text += ' ';
        text += strings_.format(def.words[i], {});
    }
    if (n == 1) {
        if (def.count > 1) text += " 1";
    } else {
        text += n == 2 ? " 2" : " 3";
    }
    return text;
}

void GameConnection::leadership_purchase_button(std::uint32_t ability) {
    // CLeadershipWnd::WndNotification @0x6b7bf0, Purchase: Send_OP_PurchaseLeadershipAA at once with
    // LeadershipNoConfirm (0xde0d0c), else ConfirmationDialog(0x64, the group or raid question by whether the next rank
    // costs 1 (Leadership_GetNextRankCost, Leadership_IsGroupAbility): the cost "%d" and the name, 20 s).
    if (state_ != ConnectionState::InZone || ability >= leadership_.ranks.size()) return;
    if (client_option(game::ClientOption::LeadershipNoConfirm)) {
        buy_leadership(ability);
        return;
    }
    int index = static_cast<int>(ability);
    auto cost = static_cast<std::int32_t>(game::leadership_next_cost(index, leadership_.ranks[ability]));
    Confirmation c;
    c.id = kConfirmLeadership;
    c.text = strings_.format(game::leadership_buy_question(index < game::kFirstRaidAbility, cost),
                             std::vector<std::string>{std::to_string(cost), std::string(game::leadership_ability(index).name)});
    c.timeout_ms = game::kPurchaseConfirmMs;
    c.data = ability;
    ask(std::move(c));
}

bool GameConnection::send_group_mentor(const std::string& name, std::int32_t percent) {
    if (state_ != ConnectionState::InZone) return false;
    if (!name.empty()) {
        // PlayerManagerClient::GetSpawnByName.
        bool here = std::any_of(world_.spawns.begin(), world_.spawns.end(),
                                [&](const auto& e) { return iequals(e.second.name, name); });
        if (!here) {
            system(0x266, {name});
            return false;
        }
    }
    group_mentor_.name = name;
    group_mentor_.percent = percent;
    send_zone(encode_group_mentor(percent, name));
    return true;
}

void GameConnection::buy_leadership(std::uint32_t ability) {
    if (state_ != ConnectionState::InZone || ability >= leadership_.ranks.size()) return;
    // The server checks the points and the rank (EQEmu Handle_OP_PurchaseLeadershipAA) and answers with the rank.
    send_zone(encode_purchase_leadership(ability));
}

} // namespace hailstone::net
