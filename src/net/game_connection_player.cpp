// GameConnection: commands between players and the small per-player commands:
// duels, inspect, surname, corpse summoning and dragging, shielding, the key
// ring, /rewind, autosplit, the bandolier, reading books and the spell book
// command. Packets in net/player_packets.h, state in game/interact.h; every
// function names the original it follows (in eqgame.exe).

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <glm/geometric.hpp>

#include "core/log.h"
#include "core/sha1.h"
#include "game/coords.h"
#include "game/distance.h"
#include "game/eq_names.h"
#include "game/slash_commands.h"
#include "game/world_state.h"
#include "net/game_connection.h"
#include "net/opcodes.h"
#include "net/notice_packets.h"
#include "net/packet_io.h"
#include "net/player_packets.h"
#include "net/task_packets.h"

namespace hailstone::net {

namespace {

// CDisplay::PlayerDistance(.., 1.0f) limit for inspecting (Send_OP_InspectRequest, Handle_OP_InspectAnswer).
constexpr float kInspectRange = 20.0f;
constexpr std::uint8_t kClassLDoNObject = 62; // '>' (Send_OP_LDoNInspect)

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string after_dot(const std::string& s) {
    auto dot = s.rfind('.');
    return dot == std::string::npos ? s : s.substr(dot + 1);
}

// A spawn name without the trailing digits EQEmu appends ("a_rat01" -> "a_rat").
std::string base_name(const std::string& s) {
    std::size_t n = s.size();
    while (n > 0 && std::isdigit(static_cast<unsigned char>(s[n - 1]))) --n;
    return s.substr(0, n);
}

// A player corpse's owner: "Name's corpse0" -> "Name" (PlayerClient +0xe4).
std::string corpse_owner(const std::string& corpse) {
    auto at = corpse.find("'s corpse");
    return at == std::string::npos ? base_name(corpse) : corpse.substr(0, at);
}

} // namespace

// ---------------------------------------------------------------- helpers

bool GameConnection::looting_own_corpse() const {
    // CLootWnd's own-corpse test: stricmp(the player's name, the corpse's +0xe4 owner).
    if (!loot_) return false;
    auto it = world_.spawns.find(loot_->corpse_id);
    return it != world_.spawns.end() && iequals(corpse_owner(it->second.name), character_);
}

float GameConnection::distance_to(std::uint32_t spawn_id) const {
    auto it = world_.spawns.find(spawn_id);
    const game::Spawn* me = world_.player();
    if (it == world_.spawns.end() || !me) return 1e9f;
    glm::vec3 at = motion_known_ ? position_ : me->position;
    return game::player_distance(&at, &it->second.position);
}

bool GameConnection::is_ignored(const std::string& name) const {
    // CEverQuest::BeingIgnored @0x516a00: the chat server's ignore list, 'server.' prefixes stripped.
    std::string n = base_name(name);
    for (const auto& i : social_.ignored)
        if (iequals(after_dot(i), n)) return true;
    return false;
}

void GameConnection::reset_player_for_zone() {
    inspect_.reset();
    book_.reset();
    discard_questions(); // the shown one and those waiting: they were about the zone left
    offline_mode_at_ = 0; // CEverQuest_ResetClientStateForZone @0x531b36
}

// ---------------------------------------------------------------- duels

void GameConnection::duel() {
    // Cmd_duel @0x4f6930.
    if (state_ != ConnectionState::InZone) return;
    auto it = world_.spawns.find(world_.target_id);
    const game::Spawn* t = it == world_.spawns.end() ? nullptr : &it->second;
    bool pc = t && !t->is_npc && !t->is_corpse;
    auto out = game::duel_command(duel_, t ? t->id : 0, pc, t && t->id == world_.player_id);
    using K = game::DuelCommandOutcome::Kind;
    if (out.kind == K::Message) {
        chat(ChatEvent::Kind::System, strings_.format(out.eqstr, std::vector<std::string>{}));
        return;
    }
    chat(ChatEvent::Kind::System, strings_.format(out.eqstr, std::vector<std::string>{t->name}));
    send_zone(encode(DuelRequest{t->id, world_.player_id}));
    if (out.kind == K::Accept) send_zone(encode_duel_accept(t->id, world_.player_id));
}

void GameConnection::decline_duel() {
    // Cmd_decline @0x4f67b0.
    if (duel_.active) {
        system(13245); // You cannot decline now!  It is too late for you cowardly dog!
        return;
    }
    if (duel_.challenger == 0) {
        system(13247);
        return;
    }
    auto it = world_.spawns.find(duel_.challenger);
    if (it != world_.spawns.end()) {
        system(13246, {it->second.name});
        send_duel_refusal(it->second.id, 0);
    }
    duel_.challenger = 0;
}

void GameConnection::send_duel_refusal(std::uint32_t challenger, std::uint8_t kind) {
    send_zone(encode(DuelRefusal{challenger, world_.player_id, kind}));
    // EQEmu drops 0x52f7: a plain refusal (kind 0: declined or ignored) goes as its OP_DuelDecline too
    // (encode_duel_decline_eqemu). Not while a duel of our own runs or waits, whose state that would clear; kinds 1
    // and 2 are those cases, and EQEmu does not forward a challenge to a player in them.
    if (kind == 0 && !duel_.active && duel_.challenged == 0 && world_.spawns.count(challenger))
        send_zone(encode_duel_decline_eqemu(challenger, world_.player_id));
}

void GameConnection::duel_decline_message(std::vector<std::string>& args) {
    // EQEmu's Handle_OP_DuelDecline tells the challenger with the formatted string 1383 instead of the original's
    // 0x52f7, so that line from the player we challenged ends the challenge as handle_duel_refusal does. EQEmu
    // passes the name through Entity::GetName, which is "" (Mob's GetName hides it rather than overriding it), so
    // the line comes without one: the challenged player's name is filled in.
    if (duel_.active || duel_.challenged == 0) return;
    auto it = world_.spawns.find(duel_.challenged);
    if (it == world_.spawns.end()) return;
    if (args.empty() || args[0].empty()) args = {it->second.name};
    if (args[0] != it->second.name) return;
    duel_.challenged = 0;
}

void GameConnection::handle_duel_request(std::uint32_t challenger) {
    // Handle_OP_RequestDuel @0x4b67c0.
    auto it = world_.spawns.find(challenger);
    if (it == world_.spawns.end()) return;
    const std::string name = it->second.name;
    auto out = game::duel_request(duel_, challenger, is_ignored(name));
    using K = game::DuelRequestOutcome::Kind;
    switch (out.kind) {
    case K::Accepted: system(1424, {name}); break;
    case K::Challenged:
        system(1426, {name});
        window_toggle_ = "tip:duel"; // CTipWnd::Activate(0x1a): the duel tip
        break;
    case K::Refuse: send_duel_refusal(challenger, out.refusal_kind); break;
    case K::Ignore: break;
    }
}

void GameConnection::handle_duel_refusal(const DuelRefusal& r) {
    // HandleWorldMessage case 0x52f7 (@0x4c76b1): the player we challenged refused.
    if (r.kind == '*') return;
    auto it = world_.spawns.find(r.refuser);
    if (it == world_.spawns.end() || it->first != duel_.challenged) return;
    if (r.kind <= 2) system(1383u + r.kind, {it->second.name});
    duel_.active = false;
    duel_.challenged = 0;
}

void GameConnection::duel_spawn_removed(std::uint32_t spawn_id, bool decayed) {
    // msgEQRmPlayer @0x4b8380: the duel partner left.
    if (spawn_id != duel_.challenged || duel_.challenged == 0 || !decayed) return;
    auto it = world_.spawns.find(spawn_id);
    std::string name = it == world_.spawns.end() ? std::string() : it->second.name;
    if (!duel_.active) {
        system(1409, {name});
    } else {
        system(1408, {character_, name, name});
        // The original's 8 bytes, which stock EQEmu drops (it wants 12). Not its 12-byte form: Handle_OP_DuelDecline
        // calls entity_list.GetID(target_id)->IsClient() without a null check, and the partner has left the zone.
        // EQEmu ends both players' duel itself when a client goes (Client::~Client).
        send_zone(encode_duel_decline(spawn_id, world_.player_id));
    }
    duel_ = {};
}

// ---------------------------------------------------------------- inspect

bool GameConnection::inspect(std::uint32_t spawn_id) {
    // Send_OP_InspectRequest @0x51fa40: players only, within 20.
    auto it = world_.spawns.find(spawn_id);
    if (state_ != ConnectionState::InZone || it == world_.spawns.end() || it->second.is_npc || it->second.is_corpse)
        return false;
    if (distance_to(spawn_id) >= kInspectRange && spawn_id != world_.player_id) {
        system(12378, {it->second.name});
        return false;
    }
    send_zone(encode_inspect_request(spawn_id, world_.player_id));
    return true;
}

void GameConnection::inspect_target() {
    // /inspect (Send_OP_LDoNInspect @0x4e7b20).
    auto it = world_.spawns.find(world_.target_id);
    if (world_.target_id == 0 || it == world_.spawns.end()) {
        system(8516);
        return;
    }
    if (distance_to(world_.target_id) > kInspectRange) {
        system(8527);
        return;
    }
    if (it->second.class_id == kClassLDoNObject) {
        send_zone(make_empty(Op::LDoNInspect));
        return;
    }
    if (it->second.is_npc || it->second.is_corpse) {
        system(6821);
        return;
    }
    inspect(world_.target_id);
}

void GameConnection::handle_inspect_answer(const InspectAnswer& a) {
    // Handle_OP_InspectAnswer @0x68f9f0: shown only while the player is within 20 (else eqstr 1402).
    auto it = world_.spawns.find(a.player);
    if (it == world_.spawns.end()) return;
    const std::string& name = it->second.name;
    if (a.player != world_.player_id && distance_to(a.player) > kInspectRange) {
        system(1402, {name});
        return;
    }
    game::InspectView v;
    v.player_id = a.player;
    v.player_name = name;
    v.own = a.player == world_.player_id;
    for (int i = 0; i < kInspectSlots; ++i) {
        v.item_names[static_cast<std::size_t>(i)] = a.names[static_cast<std::size_t>(i)];
        v.item_icons[static_cast<std::size_t>(i)] = a.icons[static_cast<std::size_t>(i)];
    }
    v.text = a.text;
    // "You are inspecting %1." titles the window; it goes to chat as well when
    // the window was closed and the player is someone else.
    if (!v.own && !inspect_) system(1401, {name});
    inspect_ = std::move(v);
}

void GameConnection::close_inspect() { inspect_.reset(); }

void GameConnection::set_inspect_message(const std::string& text) {
    // CInspectWnd_SaveInspectMessage @0x68f520: our own message (254 characters on English clients).
    if (state_ != ConnectionState::InZone) return;
    std::string t = text.substr(0, 254);
    send_zone(encode_inspect_message(t));
    if (inspect_ && inspect_->own) inspect_->text = t;
}

void GameConnection::update_inspect() {
    // CInspectWnd::OnProcessFrame @0x68f480: once a second, closes when the player is over 20 away.
    if (!inspect_ || inspect_->own) return;
    std::int64_t now = now_ms();
    if (now < inspect_check_ms_) return;
    inspect_check_ms_ = now + 1000;
    if (distance_to(inspect_->player_id) > kInspectRange) inspect_.reset();
}

void GameConnection::toggle_inspect_command(const std::string& args) {
    // Cmd_toggleinspect @0x4e7f20.
    if (iequals(args, strings_.format(12947, std::vector<std::string>{}))) {
        inspect_others_ = true;
    } else if (iequals(args, strings_.format(12946, std::vector<std::string>{}))) {
        inspect_others_ = false;
    } else {
        system(13210);
        system(13211);
    }
    system(inspect_others_ ? 13212 : 13213);
}

// ---------------------------------------------------------------- right click

void GameConnection::right_click_player(std::uint32_t spawn_id) {
    // CEverQuest::RightClickedOnPlayer @0x536d80, after the service classes: a
    // trader (+0x320) opens its shop and a buyer (+0x3d4) its barter window,
    // whether the spawn is a player or an NPC (the test reads only the flag; a
    // server's simulated traders are NPCs); a player otherwise is considered
    // and (with /toggleinspect on) inspected.
    auto it = world_.spawns.find(spawn_id);
    if (state_ != ConnectionState::InZone || it == world_.spawns.end() || it->second.is_corpse) return;
    if (it->second.trader) {
        open_trader(spawn_id);
        return;
    }
    if (it->second.buyer) {
        open_buyer(spawn_id);
        return;
    }
    if (it->second.is_npc) return;
    if (inspect_others_) inspect(spawn_id);
}

// ---------------------------------------------------------------- surname

void GameConnection::surname_command(const std::string& args) {
    // Cmd_surname @0x4efe20.
    if (pstatus_.level < 20) {
        system(12940);
        return;
    }
    if (args.empty()) {
        if (profile_ && !profile_->last_name.empty())
            ask(Confirmation{kConfirmClearSurname, strings_.format(5155, std::vector<std::string>{}), {}});
        else
            system(12941);
        return;
    }
    if (args.size() >= 32) {
        system(2239);
        return;
    }
    std::string_view rest = args;
    std::string word = game::next_cmd_arg(rest).substr(0, 19);
    // Cmd_surname_ShowConfirm @0x517b60: "Your last name will be %1 is this correct?"
    ask(Confirmation{kConfirmSurname, strings_.format(9050, std::vector<std::string>{word}), word});
}

void GameConnection::ask(Confirmation c) {
    if (confirmation_) confirm_queue_.push_back(std::move(c));
    else confirmation_ = std::move(c);
}

void GameConnection::unwind_question(const Confirmation& c) {
    // What answer_confirmation's no does to this client's own state, without a packet.
    if (c.id == kConfirmLootNoDrop) loot_popup_pending_ = false;
    else if (c.id == kConfirmFellowshipInvite) {
        fellowship_.invite.reset();
        fellowship_invite_expires_ = 0;
    } else if (c.id >= game::kConfirmAugInsert && c.id <= game::kConfirmAugDestroy)
        spells_.pending = std::max(0, spells_.pending - 1);
    else if (c.id == kConfirmMoldCombine) combine_pending_ = false;
    else if (c.id == kConfirmMarketplaceResult || c.id == kConfirmMarketplaceNotice) {
        marketplace_.busy = false;
        marketplace_.dialog = false;
    }
}

void GameConnection::discard_questions(const std::function<bool(const Confirmation&)>& which) {
    auto picked = [&](const Confirmation& c) { return !which || which(c); };
    for (auto it = confirm_queue_.begin(); it != confirm_queue_.end();) {
        if (!picked(*it)) {
            ++it;
            continue;
        }
        unwind_question(*it);
        it = confirm_queue_.erase(it);
    }
    if (confirmation_ && picked(*confirmation_)) {
        unwind_question(*confirmation_);
        confirmation_.reset();
        if (!confirm_queue_.empty()) {
            confirmation_ = std::move(confirm_queue_.front());
            confirm_queue_.pop_front();
        }
    }
}

void GameConnection::answer_confirmation(bool yes) {
    // ConfirmationDialog_HandleResponse @0x52d540: 0x6f sends OP_Surname and
    // prints eqstr 9051; 0x75 sends an empty OP_ClearSurname; 0x7a equips the
    // attuneable cursor item.
    if (!confirmation_) return;
    Confirmation c = std::move(*confirmation_);
    confirmation_.reset();
    if (!confirm_queue_.empty()) { // the next waiting offer
        confirmation_ = std::move(confirm_queue_.front());
        confirm_queue_.pop_front();
    }
    if (c.id == kConfirmDzInvite) return answer_dz_invite(yes); // a no answers too
    if (c.id == kConfirmMarketplaceResult || c.id == kConfirmMarketplaceNotice) {
        // The window's popup callback (0x6d8570): a purchase's answer goes back to the main page (a Results OK
        // click); every popup then runs UpdateButtonStates and clears wnd +0x315.
        if (c.id == kConfirmMarketplaceResult) marketplace_.window.page = game::MarketplacePage::Main;
        marketplace_.busy = false;
        marketplace_.dialog = false;
        return;
    }
    if (c.id == 0x89) {
        // ConfirmationDialog_HandleResponse 0x89: with the estate known, +0xb9 = the answer (a no also prints eqstr
        // 7638); the body goes back on 0x5a63 either way.
        if (c.offer.data.size() < kReTransactionSize || state_ != ConnectionState::InZone) return;
        AppPacket back = c.offer;
        ByteReader r(back.data);
        if (real_estate_.record(r.le<std::uint32_t>())) {
            back.data[0xb9] = std::byte{static_cast<std::uint8_t>(yes ? 1 : 0)};
            if (!yes) system(0x1dd6, {}, 0xd);
        }
        send_zone(std::move(back));
        return;
    }
    if (c.id == kConfirmLootNoDrop) {
        // Send_OP_LootItem @0x6be810 (the loot window's dialog 100): the popup flag clears either way; a yes sends
        // the request kept with the question.
        loot_popup_pending_ = false;
        if (yes && state_ == ConnectionState::InZone) send_loot_item(static_cast<std::int16_t>(c.data), c.data2 != 0);
        return;
    }
    if (c.id == kConfirmRaidInvite) return answer_raid_invite(yes); // CRaid::SendInviteResponse(yes)
    if (c.id == kConfirmFellowshipInvite) return answer_fellowship_invite(yes);
    if (c.id == kConfirmDecayCorpse) {
        // /decaycorpse's handler @0x479e30: a yes sends opcode 0x7d50 {corpse, player}.
        if (yes && state_ == ConnectionState::InZone) send_zone(encode_decay_corpse(c.data, c.data2));
        return;
    }
    if (c.id == kConfirmOfflineMode) {
        // ConfirmationDialog_HandleResponse 0x8f @0x52ea0b: a yes sends opcode 0x53d3 alone.
        if (yes && state_ == ConnectionState::InZone) send_zone(AppPacket(kOpOfflineMode));
        return;
    }
    if (c.id == kConfirmVeteranReward) {
        // 0x7e (/veteranReward): yes: 3600 and opcode 0x7d89 (not in EQEmu's table); no: 3601. Colour 0xf.
        system(yes ? 3600 : 3601, {}, 0xf);
        if (yes && state_ == ConnectionState::InZone) send_zone(encode_veteran_reward_claim());
        return;
    }
    if (c.id == kConfirmNameResult) return;
    if (c.id == kConfirmNameReminderOff || c.id == kConfirmNameChange || c.id == kConfirmPetNameChange ||
        c.id == kConfirmMercNameChange) {
        answer_name_change(c, yes); // net/game_connection_names.cpp
        return;
    }
    if (c.id >= game::kConfirmAugInsert && c.id <= game::kConfirmAugDestroy) {
        // ConfirmationDialog_HandleResponse @0x52d540, types 0x76..0x79 (@0x52db52): a yes sends the kept
        // OP_AugmentItem block unchanged (nothing is checked again); either answer then pops the pending reason and
        // takes IsItemPending back (0x4232f0, clamped at 0).
        if (yes && state_ == ConnectionState::InZone) send_zone(c.offer);
        spells_.pending = std::max(0, spells_.pending - 1);
        return;
    }
    if (c.id == kConfirmMoldCombine) {
        // ConfirmationDialog_HandleResponse's 0x86: a yes sends the kept OP_TradeSkillCombine (the request stays
        // pending until the server answers); a no takes the pending request back.
        if (yes && state_ == ConnectionState::InZone) send_zone(c.offer);
        else combine_pending_ = false;
        return;
    }
    if (c.id == kConfirmSharedTask || c.id == kConfirmTranslocate || c.id == kConfirmRezz) {
        // A server offer: the answer goes back about the packet that asked (net/notice_packets.h).
        if (state_ != ConnectionState::InZone) return;
        if (c.id == kConfirmTranslocate) send_zone(answer_translocate(c.offer, yes));
        else if (c.id == kConfirmRezz) {
            if (respawn_spawn_pending_ || (respawn_ && respawn_chosen_)) return;
            clear_hover_rez();
            if (respawn_) {
                if (const auto* rez = respawn_resurrection_option()) {
                    hover_rez_choice_ = *rez;
                    hover_rez_accepted_ = yes;
                    log::info("hover resurrection offer {} locally; awaiting destination selection", yes ? "accepted" : "declined");
                } else if (yes) {
                    log::warn("hover resurrection offer has no reserved destination; leaving respawn choices unchanged");
                }
                // Native hover separates consent from destination selection. The server already retains the
                // offer: Yes enables its reserved row locally, and OP_RespawnWindow later commits the choice.
                // An ordinary OP_RezzAnswer acceptance would bypass the stock hover publication path.
                if (!yes) send_zone(answer_rezz(c.offer, false));
                discard_questions([](const Confirmation& q) { return q.id == kConfirmRezz; });
            } else {
                send_zone(answer_rezz(c.offer, yes));
            }
        } else if (auto i = decode_shared_task_invite(c.offer)) send_zone(encode_shared_task_invite_response(i->invite_id, yes));
        return;
    }
    if (!yes || state_ != ConnectionState::InZone) return;
    if (c.id == kConfirmSurname) {
        Surname s;
        s.name = character_;
        s.last_name = c.value;
        send_zone(encode(s));
        system(9051);
    } else if (c.id == kConfirmClearSurname) {
        send_zone(make_empty(Op::ClearSurname));
    } else if (c.id == kConfirmAttune) {
        equip_attuned(c.slot); // game_connection_items.cpp
    } else if (c.id == kConfirmMercDismiss) {
        // 0x5682b0, dialog 2: Send_OP_MercenaryDismiss(0, 1).
        with_merc([](game::MercManager& m, game::MercHooks& h) { game::merc_send_dismiss(m, false, true, h); });
    } else if (c.id == kConfirmDestroy) {
        destroy_cursor(); // CInventoryWnd's dialog 100: PcZoneClient::DestroyHeldItemOrMoney
    } else if (c.id == kConfirmLootAll) {
        loot_all_now(); // Send_OP_LootItem's dialog 0x65: CLootWnd::LootAll(1)
    } else if (c.id == kConfirmAATrain) {
        aa_train(c.data, c.data2 != 0); // CAAWnd's dialog 0x64: Send_OP_AAAction_TrainSelected
    } else if (c.id == kConfirmAbandonTask) {
        // CTaskWnd::ConfirmAbandonTask @0x76fd50: the selected row's entry must still be that task; OP_CancelTask
        // {index, type}, and Remove disabled.
        if (task_selection_) {
            const game::Task* t = tasks_.find(task_selection_->first, task_selection_->second);
            if (t && t->task_id == c.data) {
                send_zone(encode_cancel_task(task_selection_->second, task_selection_->first));
                task_abandon_sent_ = true;
            }
        }
    } else if (c.id == kConfirmLeadership) {
        buy_leadership(c.data); // CLeadershipWnd's dialog 0x64: Send_OP_PurchaseLeadershipAA
    } else if (c.id == kConfirmAdventureBuy) {
        adventure_merchant_buy(c.data); // CPointMerchantWnd's dialog 100: Send_OP_AdventureMerchantPurchase
    } else if (c.id == kConfirmPointBuy) {
        point_merchant_buy(c.data); // CNewPointMerchantWnd's dialog 100: OP_AltCurrencyPurchase
    } else if (c.id == kConfirmGuildDelete) {
        // ConfirmationDialog_HandleResponse: OP_GuildDelete {guild id, own name} (0x44 bytes, cleared first), 9061.
        ByteWriter w;
        w.le<std::uint32_t>(guild_.guild_id);
        w.fixed_string(character_.substr(0, 63), 64);
        send_zone(make_packet(Op::GuildDelete, std::move(w)));
        system(9061);
    }
}

// ---------------------------------------------------------------- corpses, shield, keys

void GameConnection::corpse_command(Op op) {
    // SendCorpseCommandForTarget @0x478d80 (0x478d00 picks the corpse): the
    // target when it is a corpse, else our first corpse in the zone.
    const game::Spawn* corpse = nullptr;
    auto t = world_.spawns.find(world_.target_id);
    if (t != world_.spawns.end() && t->second.is_corpse) {
        corpse = &t->second;
    } else {
        for (const auto& [id, sp] : world_.spawns) {
            if (id == world_.player_id || !sp.is_corpse || sp.is_npc) continue;
            // The corpse's owner (PlayerClient +0xe4) is our name.
            if (iequals(corpse_owner(sp.name), character_)) {
                corpse = &sp;
                break;
            }
        }
    }
    if (!corpse) {
        system(13048);
        return;
    }
    CorpseCommand c;
    c.corpse_name = corpse->name;
    c.player_name = character_;
    c.zone_id = zone_id();
    glm::vec3 server = game::world_to_server(corpse->position);
    c.y = server.y;
    c.x = server.x;
    c.z = server.z;
    send_zone(encode_corpse_command(op, c));
    system(13047, {game::display_name(corpse->name)});
}

void GameConnection::shield_command(const std::string& args) {
    // Cmd_shield @0x4e8d60: when commands may be used now and MayShield (may_shield). Colour 0x111.
    if (!can_use_command_now() || !may_shield()) return;
    std::uint32_t target = 0;
    if (args.empty()) {
        auto it = world_.spawns.find(world_.target_id);
        if (world_.target_id == 0 || it == world_.spawns.end()) {
            system(4031, {}, 0x111);
            system(4034, {}, 0x111);
            return;
        }
        if (world_.target_id == world_.player_id) {
            system(4030, {}, 0x111);
            return;
        }
        target = world_.target_id;
    } else {
        // PlayerManagerClient::GetPlayerFromPartialName.
        std::string want = lower(args);
        for (const auto& [id, sp] : world_.spawns) {
            if (lower(sp.name).rfind(want, 0) == 0) {
                target = id;
                break;
            }
        }
        if (target == 0) return;
        if (target == world_.player_id) {
            system(3278, {}, 0x111);
            return;
        }
    }
    send_zone(encode_shielding(target));
}

void GameConnection::autosplit_command(const std::string& args) {
    // Cmd_autosplit @0x4e75b0: "off" / "on" (first 3 / 2 letters), else toggles;
    // prints the state and sends OP_SpawnAppearance type 28 (AutoSplit).
    std::string a = lower(args);
    std::string off = lower(strings_.format(12946, std::vector<std::string>{}));
    std::string on = lower(strings_.format(12947, std::vector<std::string>{}));
    if (a.compare(0, 3, off, 0, 3) == 0 && !a.empty()) autosplit_ = false;
    else if (a.compare(0, 2, on, 0, 2) == 0 && !a.empty()) autosplit_ = true;
    else autosplit_ = !autosplit_;
    system(autosplit_ ? 13197 : 13198);
    SpawnAppearance s;
    s.spawn_id = static_cast<std::uint16_t>(world_.player_id);
    s.type = 28;
    s.parameter = autosplit_ ? 1 : 0;
    send_zone(encode(s));
}

// ---------------------------------------------------------------- bandolier

void GameConnection::init_bandolier_from_profile() {
    bandolier_.assign(game::kBandolierSets, {});
    if (!profile_) return;
    for (std::size_t i = 0; i < profile_->bandoliers.size() && i < bandolier_.size(); ++i) {
        const auto& in = profile_->bandoliers[i];
        auto& out = bandolier_[i];
        out.name = in.name;
        for (std::size_t j = 0; j < 4; ++j) out.items[j] = {in.items[j].id, in.items[j].icon, in.items[j].name};
    }
}

int GameConnection::bandolier_sets() const { return game::bandolier_set_count(aa_effect_total(363)); }

std::int32_t GameConnection::aa_effect_total(std::int32_t spa) const {
    // The AA part of CharacterZoneClient::TotalEffect: the owned ranks' effects with this SPA.
    std::int32_t total = 0;
    for (const auto& o : alt_adv_.owned) {
        const game::AltAbility* a = alt_adv_.get(o.id);
        if (!a) continue;
        for (const auto& e : a->effects)
            if (e.spa == spa) total += e.base;
    }
    return total;
}

bool GameConnection::is_spellcaster() const {
    // EQ_Character::IsSpellcaster: classes 2..15 except Monk (7) and Rogue (9). A server's client may answer for the
    // local character (the NMS patch's detours: any caster class in the mask).
    if (auto v = server_ext().is_spellcaster(*this)) return *v;
    const std::uint32_t c = visible_class();
    return c >= 2 && c <= 15 && c != 7 && c != 9;
}

int GameConnection::bandolier_find(const std::string& name) const {
    for (int i = 0; i < static_cast<int>(bandolier_.size()) && i < game::kBandolierSets; ++i)
        if (iequals(bandolier_[static_cast<std::size_t>(i)].name, name)) return i;
    return -1;
}

bool GameConnection::bandolier_create(int index, const std::string& name) {
    // PcZoneClient_CreateBandolierSet @0x584280: the four weapon slots' items.
    if (name.empty() || index < 0 || index >= bandolier_sets()) return false;
    game::BandolierSet set;
    set.name = name.substr(0, 31);
    for (std::size_t j = 0; j < 4; ++j) {
        if (const Item* it = inventory_.at(ItemSlot::possessions(game::kBandolierSlots[j])))
            set.items[j] = {it->id, it->icon, it->name};
        else
            set.items[j] = {0, 0xffffffffu, {}};
    }
    bandolier_[static_cast<std::size_t>(index)] = set;
    send_zone(encode_bandolier(kBandolierCreate, static_cast<std::uint8_t>(index), name));
    return true;
}

int GameConnection::bandolier_save(const std::string& name) {
    // PcZoneClient_SaveBandolierSetByName @0x585300: the set with this name or the first empty one.
    if (name.empty()) return -1;
    int count = bandolier_sets(), empty = -1, at = -1;
    for (int i = 0; i < count; ++i) {
        const auto& s = bandolier_[static_cast<std::size_t>(i)];
        if (empty == -1 && s.name.empty()) empty = i;
        if (s.name == name) {
            at = i;
            break;
        }
    }
    if (at == -1) at = empty;
    if (at == -1) {
        system(9344);
        return -1;
    }
    bandolier_create(at, name);
    return at;
}

void GameConnection::bandolier_delete(int index) {
    // Send_OP_Bandolier @0x57fe70.
    if (index < 0 || index >= game::kBandolierSets) return;
    bandolier_[static_cast<std::size_t>(index)] = {};
    send_zone(encode_bandolier(kBandolierRemove, static_cast<std::uint8_t>(index)));
}

void GameConnection::bandolier_hold(int index, int slot, bool shift) {
    if (index < 0 || slot < 0 || slot > 3 || state_ != ConnectionState::InZone) return;
    send_zone(encode_bandolier_hold(static_cast<std::uint8_t>(index), static_cast<std::uint8_t>(slot), shift));
}

bool GameConnection::bandolier_activate(int index) {
    // PcZoneClient::BandolierSwap @0x585aa0: the original moves the items
    // itself (MultipleItemMoveManager::ProcessMove without sending) and sends
    // only OP_Bandolier action 2; EQEmu's Client::SetBandolier makes the same
    // moves without telling the client. The moves here follow SetBandolier and
    // MoveItemToInventory, which EQEmu wrote to match the client.
    //
    // ProcessMove is all or nothing: a move that fails puts every item back
    // and BandolierSwap returns without sending, printing eqstr 9342 for
    // error 0x11 (no room) and 9343 for 0x1a. So a returned item with nowhere
    // to go cancels the whole swap here too. Sending it anyway would leave the
    // client without the item while the server keeps it (EQEmu puts it on the
    // cursor), and the next swap would then miss it.
    if (index < 0 || index >= bandolier_sets() || spells_.pending > 0) return false;
    const game::BandolierSet set = bandolier_[static_cast<std::size_t>(index)];
    if (set.name.empty()) return false;
    const game::Inventory before = inventory_;
    bool room = true;
    std::array<std::optional<Item>, 4> pulled;
    for (std::size_t b = 0; b < 4; ++b) {
        if (set.items[b].id == 0) continue;
        std::int16_t weapon = game::kBandolierSlots[b];
        std::optional<ItemSlot> from = find_item_for_bandolier(set.items[b].id);
        if (from) {
            Item* it = inventory_.at(*from);
            if ((b == 2 || b == 3) && it->stackable() && it->stack_count > 1) {
                Item one = *it;
                one.stack_count = 1;
                it->stack_count -= 1;
                pulled[b] = std::move(one);
            } else {
                pulled[b] = inventory_.take(*from);
            }
        } else if (auto held = inventory_.take(ItemSlot::possessions(weapon))) {
            room = return_item_to_inventory(std::move(*held)) && room;
        }
    }
    for (std::size_t b = 0; b < 4; ++b) {
        std::int16_t weapon = game::kBandolierSlots[b];
        if (set.items[b].id != 0) {
            if (!pulled[b]) continue;
            auto held = inventory_.take(ItemSlot::possessions(weapon));
            pulled[b]->slot = ItemSlot::possessions(weapon);
            inventory_.put(ItemSlot::possessions(weapon), std::move(*pulled[b]));
            if (held) room = return_item_to_inventory(std::move(*held)) && room;
        } else if (auto held = inventory_.take(ItemSlot::possessions(weapon))) {
            room = return_item_to_inventory(std::move(*held)) && room;
        }
    }
    if (!room) {
        inventory_ = before;
        system(9342); // You do not have enough room to use this bandolier.
        return false;
    }
    for (std::int16_t weapon : game::kBandolierSlots) refresh_worn_slot(weapon);
    send_zone(encode_bandolier(kBandolierSet, static_cast<std::uint8_t>(index)));
    return true;
}

std::optional<ItemSlot> GameConnection::find_item_for_bandolier(std::uint32_t item_id) const {
    // InventoryProfile::HasItem(id, 1, worn | personal): worn slots, then the
    // general slots and their bags in slot order, then the cursor and its bag.
    for (std::int16_t s = 0; s < kEquipmentSlots; ++s)
        if (const Item* it = inventory_.at(ItemSlot::possessions(s)); it && it->id == item_id) return ItemSlot::possessions(s);
    for (std::int16_t s = kSlotGeneral1; s <= kSlotGeneral10; ++s) {
        const Item* it = inventory_.at(ItemSlot::possessions(s));
        if (!it) continue;
        if (it->id == item_id) return ItemSlot::possessions(s);
        if (!it->is_bag()) continue;
        for (std::int16_t sub = 0; sub < static_cast<std::int16_t>(it->bag_slots); ++sub)
            if (const Item* in = inventory_.at(ItemSlot::possessions(s, sub)); in && in->id == item_id)
                return ItemSlot::possessions(s, sub);
    }
    if (const Item* c = inventory_.cursor()) {
        if (c->id == item_id) return ItemSlot::possessions(kSlotCursor);
        if (c->is_bag())
            for (std::int16_t sub = 0; sub < static_cast<std::int16_t>(c->bag_slots); ++sub)
                if (const Item* in = inventory_.at(ItemSlot::possessions(kSlotCursor, sub)); in && in->id == item_id)
                    return ItemSlot::possessions(kSlotCursor, sub);
    }
    return std::nullopt;
}

bool GameConnection::return_item_to_inventory(Item item) {
    // Client::MoveItemToInventory: a stackable item first tops up matching
    // stacks (general slots, then inside each bag), then the first free
    // general slot, or the first free slot of a bag that fits it by the
    // server's test (game::server_bag_accepts).
    if (item.stackable()) {
        for (std::int16_t s = kSlotGeneral1; s <= kSlotCursor && item.stack_count > 0; ++s) {
            Item* there = inventory_.at(ItemSlot::possessions(s));
            if (there && there->id == item.id && there->stack_count < there->max_stack) {
                std::uint32_t moved = std::min(there->max_stack - there->stack_count, item.stack_count);
                there->stack_count += moved;
                item.stack_count -= moved;
            }
            if (there && there->is_bag()) {
                for (std::int16_t sub = 0; sub < static_cast<std::int16_t>(there->bag_slots) && item.stack_count > 0; ++sub) {
                    Item* in = inventory_.at(ItemSlot::possessions(s, sub));
                    if (in && in->id == item.id && in->stack_count < in->max_stack) {
                        std::uint32_t moved = std::min(in->max_stack - in->stack_count, item.stack_count);
                        in->stack_count += moved;
                        item.stack_count -= moved;
                    }
                }
            }
        }
        if (item.stack_count == 0) return true;
    }
    for (std::int16_t s = kSlotGeneral1; s <= kSlotGeneral10; ++s) {
        const Item* there = inventory_.at(ItemSlot::possessions(s));
        if (!there) {
            item.slot = ItemSlot::possessions(s);
            inventory_.put(ItemSlot::possessions(s), std::move(item));
            return true;
        }
        if (game::server_bag_accepts(*there, item)) {
            for (std::int16_t sub = 0; sub < static_cast<std::int16_t>(there->bag_slots); ++sub) {
                if (inventory_.at(ItemSlot::possessions(s, sub))) continue;
                item.slot = ItemSlot::possessions(s, sub);
                inventory_.put(ItemSlot::possessions(s, sub), std::move(item));
                return true;
            }
        }
    }
    log::info("bandolier: no room for {}", item.name);
    return false;
}

void GameConnection::bandolier_command(const std::string& args) {
    // Cmd_bandolier @0x4decd0: /bandolier add|delete|activate <name or number>.
    std::string_view rest = args;
    std::string verb = game::next_cmd_arg(rest);
    std::string name = game::next_cmd_arg(rest);
    int number = std::atoi(name.c_str());
    auto is = [&](std::uint32_t id) { return iequals(verb, strings_.format(id, std::vector<std::string>{})); };
    if (is(9306)) {
        bandolier_save(name);
    } else if (is(9309)) {
        int i = bandolier_find(name);
        bandolier_delete(i >= 0 ? i : number);
    } else if (is(9310)) {
        int i = bandolier_find(name);
        bandolier_activate(i >= 0 ? i : number);
    } else {
        system(9329);
    }
}

// ---------------------------------------------------------------- books

bool GameConnection::read_book(const ItemSlot& slot) {
    // CInvSlot::HandleRButtonUp @0x697250: a book item (class 2) in our
    // possessions asks the server for its text; the reply opens the note
    // window (book type 0) or the book window.
    const Item* it = inventory_.at(slot);
    if (state_ != ConnectionState::InZone || !it || it->item_class != 2 || slot.type != kInvPossessions) return false;
    ReadBook r;
    r.window = 0xffffffff;
    r.slot = slot.slot;
    r.sub = slot.sub;
    r.aug = -1;
    r.type = it->book_type;
    r.target = world_.target_id;
    r.text = it->book_file;
    send_zone(encode(r));
    return true;
}

void GameConnection::handle_read_book(const ReadBook& r) {
    // Handle_OP_ReadBook @0x4c0ea0.
    BookView v;
    v.type = r.type;
    v.text = r.text.substr(0, 4000);
    v.slot = ItemSlot::possessions(r.slot, r.sub);
    v.can_cast = r.can_cast != 0;
    v.can_scribe = r.can_scribe != 0;
    book_ = std::move(v);
}

void GameConnection::close_book() { book_.reset(); }

void GameConnection::book_button() {
    // CBookWnd / CNoteWnd WndNotification: OP_BookButton {the book's slot, the target}, an item is pending.
    if (!book_ || state_ != ConnectionState::InZone) return;
    send_zone(encode_book_button(book_->slot.slot, book_->slot.sub, -1, world_.target_id));
    book_->can_cast = book_->can_scribe = false; // both buttons are taken down
}

void GameConnection::answer_large_dialog(int button) {
    if (!large_dialog_) return;
    LargeDialog d = std::move(*large_dialog_);
    large_dialog_.reset();
    // The OnLevelMessage dialog's handlers (vtable 0x9fa260 slots 95..98): Send_OP_PopupResponse, which sends only
    // for a non-zero id.
    std::uint32_t kind = button == 2 ? 1u : button == 3 ? 2u : 0u;
    std::uint32_t id = button == 2 ? d.negative_id : button == 3 ? d.close_id : d.popup_id;
    if (id != 0 && state_ == ConnectionState::InZone) send_zone(encode_popup_response(kind, id));
}

// ---------------------------------------------------------------- the spell book

void GameConnection::book_command(const std::string& args) {
    // Cmd_book @0x4f7230: "/book" toggles the spell book, "/book close"
    // closes it, "/book <n>" opens it at page n (rounded down to the left page).
    if (!profile_) return;
    if (args.empty() && spellbook_open_) {
        spellbook_open_ = false;
        window_toggle_ = "spellbook";
        return;
    }
    if (!args.empty() && iequals(args, strings_.format(8883, std::vector<std::string>{}))) {
        if (spellbook_open_) {
            spellbook_open_ = false;
            window_toggle_ = "spellbook";
        }
        return;
    }
    if (!is_spellcaster()) return;
    if (moving_) {
        system(12085);
        return;
    }
    if (pstatus_.stand_state == 105) return; // looting ('i')
    if (!spellbook_open_) {
        spellbook_open_ = true;
        window_toggle_ = "spellbook";
    }
    if (!args.empty()) {
        int page = std::atoi(args.c_str()) - 1;
        if (page < 1) page = 0;
        else if (page % 2 != 0) page -= 1;
        spellbook_page_ = page;
    }
}

// ---------------------------------------------------------------- packets

// ---------------------------------------------------------------- the potion belt and /useitem

const ProfileBeltItem* GameConnection::potion_belt_item(int slot) const {
    if (!profile_ || slot < 0 || slot >= static_cast<int>(profile_->potion_belt.size())) return nullptr;
    const ProfileBeltItem& b = profile_->potion_belt[static_cast<std::size_t>(slot)];
    return b.id ? &b : nullptr;
}

bool GameConnection::potion_belt_set(int slot) {
    const Item* held = inventory_.cursor();
    if (!profile_ || !held || slot < 0 || slot >= kPotionBeltSlots || state_ != ConnectionState::InZone) return false;
    if (slot >= kPotionBeltServerSlots) { // EQEmu's Handle_OP_PotionBelt keeps slots 0..3 and drops the rest
        chat(ChatEvent::Kind::System, "The server keeps only the first four potion belt slots.");
        return false;
    }
    // EQEmu takes any item (Handle_OP_PotionBelt); the original's PotionBeltWnd check is not decoded. The item stays
    // on the cursor: the belt only names it.
    if (static_cast<std::size_t>(slot) >= profile_->potion_belt.size()) profile_->potion_belt.resize(kPotionBeltSlots);
    profile_->potion_belt[static_cast<std::size_t>(slot)] = {held->name, held->id, held->icon};
    send_zone(encode_potion_belt(false, static_cast<std::uint32_t>(slot), held->id));
    return true;
}

void GameConnection::potion_belt_clear(int slot) {
    if (!potion_belt_item(slot) || state_ != ConnectionState::InZone) return;
    profile_->potion_belt[static_cast<std::size_t>(slot)] = {};
    send_zone(encode_potion_belt(true, static_cast<std::uint32_t>(slot), 0));
}

void GameConnection::potion_belt_hold(int slot, bool shift) {
    if (!potion_belt_item(slot) || state_ != ConnectionState::InZone) return;
    profile_->potion_belt[static_cast<std::size_t>(slot)] = {};
    send_zone(encode_potion_belt_hold(static_cast<std::uint32_t>(slot), shift));
}

std::optional<ItemSlot> GameConnection::find_possession(std::uint32_t item_id) const {
    // The worn slots, the general slots and their bags, in slot order (not the cursor).
    for (std::int16_t slot = 0; slot <= kSlotGeneral10; ++slot) {
        const Item* item = inventory_.at(ItemSlot::possessions(slot));
        if (!item) continue;
        if (item->id == item_id) return ItemSlot::possessions(slot);
        for (const auto& c : item->contents)
            if (c.item.id == item_id) return ItemSlot::possessions(slot, static_cast<std::int16_t>(c.index));
    }
    return std::nullopt;
}

std::uint32_t GameConnection::potion_belt_count(int slot) const {
    const ProfileBeltItem* b = potion_belt_item(slot);
    if (!b) return 0;
    std::uint32_t n = 0;
    auto add = [&](const Item& item) {
        if (item.id == b->id) n += item.stackable() ? item.stack_count : 1;
    };
    for (std::int16_t s = 0; s <= kSlotGeneral10; ++s)
        if (const Item* item = inventory_.at(ItemSlot::possessions(s))) {
            add(*item);
            for (const auto& c : item->contents) add(c.item);
        }
    return n;
}

const Item* GameConnection::potion_belt_first_item(int slot) const {
    const ProfileBeltItem* b = potion_belt_item(slot);
    auto at = b ? find_possession(b->id) : std::nullopt;
    return at ? inventory_.at(*at) : nullptr;
}

bool GameConnection::potion_belt_use(int slot) {
    const ProfileBeltItem* b = potion_belt_item(slot);
    if (!b) return false;
    auto at = find_possession(b->id);
    if (!at) {
        chat(ChatEvent::Kind::System, std::format("You have no {}.", b->name)); // this client's line
        return false;
    }
    return use_item(*at);
}

// ---------------------------------------------------------------- titles

void GameConnection::set_titles_open(bool open) {
    titles_.open = open;
    if (open && state_ == ConnectionState::InZone) send_zone(encode_request_titles()); // the list is the server's
}

void GameConnection::set_title(bool suffix, std::uint32_t title_id) {
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode_set_title(suffix, title_id));
}

void GameConnection::handle_player(Op op, const AppPacket& p) {
    switch (op) {
    case Op::SendTitleList:
        if (auto t = decode_title_list(p)) titles_.available = std::move(*t);
        break;
    case Op::SetTitleReply:
        if (auto t = decode_title_reply(p)) {
            auto it = world_.spawns.find(t->spawn_id);
            if (it != world_.spawns.end()) (t->suffix ? it->second.suffix : it->second.title) = t->text;
        }
        break;
    case Op::NewTitlesAvailable:
        // EQEmu's NotifyNewTitlesAvailable (empty). The original's line is not decoded; this is this client's.
        if (titles_.notify) chat(ChatEvent::Kind::System, "New titles are available (Titles window).");
        if (titles_.open) send_zone(encode_request_titles());
        break;
    case Op::RequestDuel:
        // The challenger is at +4 (Handle_OP_RequestDuel @0x4b67c0).
        if (auto d = decode_duel_request(p)) handle_duel_request(d->second);
        break;
    case Op::DuelAccept:
        break; // HandleWorldMessage: nothing (case 0x68d3 is empty)
    case Op::InspectAnswer:
        if (auto a = decode_inspect_answer(p)) handle_inspect_answer(*a);
        break;
    case Op::ReadBook:
        if (auto r = decode_read_book(p)) handle_read_book(*r);
        break;
    case Op::Bandolier:
        // Handle_OP_Bandolier @0x6266f0: action 4 renames an item in the sets (EQEmu does not send it).
        break;
    case Op::Surname:
        if (auto s = decode_surname(p); s && s->unknown64 == 1 && profile_) profile_->last_name = s->last_name;
        break;
    case Op::OnLevelMessage:
        // Handle_OP_OnLevelMessage @0x755350: CLargeDialogWnd::Open with the title, the text, the buttons and the
        // duration (the text's eqstr templates and language garbling are not applied, docs/unknowns.md).
        if (auto m = decode_level_message(p)) {
            LargeDialog d;
            d.title = m->title;
            d.text = m->text;
            d.yes = m->yes;
            d.no = m->no;
            d.buttons = m->buttons != 0;
            d.sound = m->sound != 0;
            d.popup_id = m->popup_id;
            d.negative_id = m->negative_id;
            d.close_id = m->close_id;
            d.close_at_ms = m->duration ? now_ms() + static_cast<std::int64_t>(m->duration) : 0;
            large_dialog_ = std::move(d);
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- languages

void GameConnection::set_chat_language(std::uint32_t language) {
    // CChatWindow_SetLanguage @0x655fe0 (announced): "You are now speaking %1." style eqstr 3133.
    if (language > 31) return;
    chat_language_ = language;
    system(3133, {strings_.format(game::language_eqstr(language), std::vector<std::string>{})});
}

void GameConnection::language_command(const std::string& args) {
    // Cmd_language @0x4fc420: the known languages (profile skill > 0) numbered from 1.
    std::vector<std::uint32_t> known;
    if (profile_)
        for (std::uint32_t i = 0; i < profile_->languages.size() && i < 32; ++i)
            if (profile_->languages[i] != 0) known.push_back(i);
    if (iequals(args, strings_.format(13300, std::vector<std::string>{}))) {
        system(13260);
        for (std::size_t i = 0; i < known.size(); ++i)
            chat(ChatEvent::Kind::System,
                 std::format("{} - {}", i + 1, strings_.format(game::language_eqstr(known[i]), std::vector<std::string>{})));
        return;
    }
    if (args.empty()) {
        system(13262, {strings_.format(game::language_eqstr(chat_language_), std::vector<std::string>{}),
                       std::to_string(chat_language_)});
        return;
    }
    int n = std::atoi(args.c_str()) - 1;
    if (n >= 0 && n < static_cast<int>(known.size())) {
        set_chat_language(known[static_cast<std::size_t>(n)]);
        return;
    }
    system(13261);
}

// ---------------------------------------------------------------- the command table

bool GameConnection::player_command(std::string_view h, const std::string& args) {
    if (h == "Cmd_duel") { duel(); return true; }
    if (h == "Cmd_decline") { decline_duel(); return true; }
    if (h == "Send_OP_LDoNInspect") { inspect_target(); return true; }
    if (h == "Cmd_toggleinspect") { toggle_inspect_command(args); return true; }
    if (h == "Cmd_clickthrough") {
        // Cmd_clickthrough @0x4e5730: atoi into the byte, saved as Options/ClickThroughMask; the masks are worked
        // out at every pick here (0x490ba0 re-applies them to the actors in the original).
        change_client_option(game::ClientOption::ClickThroughMask,
                             static_cast<std::uint8_t>(std::atoi(args.c_str())));
        return true;
    }
    if (h == "Cmd_surname") { surname_command(args); return true; }
    if (h == "Cmd_changename") { name_change_command(kNameCharacter, args); return true; }
    if (h == "Cmd_changepetname") { name_change_command(kNamePet, args); return true; }
    if (h == "Cmd_changemercname") { name_change_command(kNameMerc, args); return true; }
    if (h == "Cmd_corpse") { corpse_command(Op::GMSummon); return true; }
    if (h == "Cmd_corpsedrag") { corpse_command(Op::CorpseDrag); return true; }
    if (h == "Cmd_corpsedrop") {
        // Cmd_corpsedrop @0x4781b0: the target's name (none: an empty name).
        auto it = world_.spawns.find(world_.target_id);
        send_zone(encode_corpse_drop(it == world_.spawns.end() ? std::string() : it->second.name));
        return true;
    }
    if (h == "Cmd_shield") { shield_command(args); return true; }
    if (game_command(h, args)) return true; // net/game_connection_game_commands.cpp
    if (h == "Cmd_keys") { send_zone(encode_key_ring_request()); return true; }
    if (h == "Cmd_rewind") {
        // Cmd_rewind @0x4e6d00: not while being flung (we have no fling state).
        send_zone(encode_rewind());
        return true;
    }
    if (h == "Cmd_resetaas") {
        // Cmd_resetaas @0x4782e0: an empty OP_ResetAA (EQEmu resets the AAs for guide status and above only).
        send_zone(encode_reset_aa());
        return true;
    }
    if (h == "Cmd_petition") {
        // Cmd_petition @0x4e2ce0 hands the text to a commerce session (Commerce_RequestNewSession) that opens the
        // PetitionWebWnd page; EQEmu has no such session but takes OP_Petition, which the original sends for
        // /guidehelp (Cmd_guidehelp_SendRequest @0x52b530). So the text goes as OP_Petition (Send_OP_Petition
        // @0x4dc890: at most 0x3ff characters) with that function's other line, 12325 "You petition, '%1'".
        // docs/unknowns.md "Petition".
        if (args.empty()) {
            chat(ChatEvent::Kind::System, "Usage: /petition <text>");
            return true;
        }
        if (auto p = encode_petition(args)) {
            send_zone(std::move(*p));
            system(12325, {args});
        }
        return true;
    }
    if (h == "Cmd_survey") {
        // Cmd_survey @0x478b70 opens the survey page in an HTML window (SurveyWebWnd): the URL with "&custom1=" and
        // the SHA-1 of the login's account id (EQLogin +0x40c) in lower-case hex. Here the browser window, as
        // OP_Weblink's pages (docs/browser.md).
        web_links_.push_back("http://www.questionpro.com/akira/TakeSurvey?id=1069785&custom1=" +
                             to_hex(sha1(std::to_string(ticket_.account_id))));
        return true;
    }
    if (h == "Cmd_autosplit") { autosplit_command(args); return true; }
    if (h == "Cmd_potionbelt") { potion_belt_open_ = !potion_belt_open_; return true; }
    if (h == "Cmd_find") { find_target(args); return true; } // a find path (a client extension)
    if (h == "Cmd_alarm") { alarm_open_ = !alarm_open_; return true; }
    // Cmd_bugreport @0x4e7310 and Cmd_claim @0x4e5cd0 show their window (vtable +0x90, Activate); they never hide it.
    if (h == "Cmd_bugreport") { bug_report_open_ = true; return true; }
    if (h == "Cmd_claim") {
        if (!claims_.open) set_claims_open(true);
        return true;
    }
    if (h == "Cmd_aggrometer" || h == "Cmd_aggrolock") {
        // Cmd_aggrometer @0x4f3b20 / Cmd_aggrolock @0x4f3c10: Rain of Fear (expansion flag 0x40000), else 4615 with
        // "Aggro Meter" / "Aggro Lock" and "Rain of Fear" (colour 0xf); then the meter window shows or hides (its
        // visibility kept), or the lock on the target toggles.
        if (!profile_ || !(profile_->expansions & 0x40000)) {
            system(4615, {h == "Cmd_aggrometer" ? "Aggro Meter" : "Aggro Lock", "Rain of Fear"}, 0xf);
            return true;
        }
        if (h == "Cmd_aggrometer") aggro_.window_open = !aggro_.window_open;
        else aggro_lock_toggle();
        return true;
    }
    if (h == "Cmd_useitem") { useitem_command(args); return true; }
    if (h == "Cmd_bandolier") { bandolier_command(args); return true; }
    if (h == "Cmd_book") { book_command(args); return true; }
    if (h == "Cmd_memspellset") { memspellset_command(args); return true; }
    if (h == "Cmd_language") { language_command(args); return true; }
    if (h == "Cmd_popmail") return true; // Cmd_popmail @0x4eb0e0 is empty
    if (h == "Cmd_stopsong") {
        stop_song();
        return true;
    }
    return false;
}

void GameConnection::stop_song() {
    // Cmd_stopsong @0x4f26b0: bards stop the song being sung (and the twist).
    if (player_class_mask() & (1u << 7)) {
        clear_bard_twist();
        stop_current_spell_cast(); // StopCurrentSpellCast(0)
    }
}

} // namespace hailstone::net
