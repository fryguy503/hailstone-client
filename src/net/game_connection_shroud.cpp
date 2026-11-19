#include "net/game_connection.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/log.h"

namespace hailstone::net {

namespace {

constexpr float kShroudNpcRange = 50.0f; // RoF2 Send Shroud Select and AltStorage share this reach check.
constexpr std::int64_t kShroudTransitionWaitMs = 45000; // The transition notice from the RoF2 sender at 0x413830.
constexpr std::uint64_t kFailedWithdrawal = std::numeric_limits<std::uint64_t>::max();

std::string_view shroud_window(std::uint32_t context_id) {
    return context_id == 0 ? "ProgressionSelectionWnd" : "TaskTemplateSelectWnd";
}

} // namespace

void GameConnection::reset_shroud_selection() {
    shroud_selection_.reset();
    shroud_selection_open_ = false;
    shroud_offer_npc_ = 0;
    ++shroud_selection_revision_;
    shroud_stats_.reset();
    monster_mission_pending_ = 0;
    shroud_transition_until_ms_ = 0;
    shroud_requested_stats_ = shroud_requested_ability_ = shroud_requested_item_ = shroud_ability_detail_ = 0;
}

bool GameConnection::shroud_form_offered(std::uint64_t revision, std::uint32_t id, bool require_unlocked) const {
    if (state_ != ConnectionState::InZone || !shroud_selection_open_ || !shroud_selection_ ||
        revision != shroud_selection_revision_ ||
        shroud_selection_->status != 0 || shrouded() || zone_change_pending_ || player_down() || id == 0) return false;
    const std::uint32_t task_id = shroud_selection_->context_id;
    if (task_id && !shared_task_monster_select_available(task_id)) return false;
    for (const auto& category : shroud_selection_->categories)
        for (const auto& branch : category.branches)
            for (const auto& form : branch.forms)
                if (form.selection_id == id) return !require_unlocked || form.unlocked;
    return false;
}

bool GameConnection::request_shroud_stats(std::uint64_t revision, std::uint32_t selection_id) {
    if (!shroud_form_offered(revision, selection_id, false)) return false;
    shroud_stats_.reset();
    shroud_requested_stats_ = selection_id;
    shroud_requested_ability_ = shroud_requested_item_ = shroud_ability_detail_ = 0;
    send_zone(shroud_selection_->native_mission ? encode_monster_mission_stats_request(selection_id)
                                                : encode_shroud_request_stats(selection_id));
    return true;
}

bool GameConnection::select_shroud(std::uint64_t revision, std::uint32_t selection_id) {
    if (shroud_selection_pending() || !shroud_form_offered(revision, selection_id, true)) return false;
    if (shroud_offer_npc_ && !shroud_npc_in_range(shroud_offer_npc_)) return false;
    if (shroud_selection_->native_mission) {
        monster_mission_pending_ = selection_id;
        send_zone(encode_monster_mission_select(selection_id));
    } else {
        shroud_transition_until_ms_ = now_ms() + kShroudTransitionWaitMs;
        send_zone(encode_shroud_select(selection_id));
    }
    return true;
}

bool GameConnection::cancel_shroud_selection(std::uint64_t revision) {
    if (state_ != ConnectionState::InZone || !shroud_selection_ || revision != shroud_selection_revision_) return false;
    if (shroud_selection_->context_id == 0) {
        send_zone(encode_shroud_cancel());
        reset_shroud_selection();
    } else {
        // A server-pushed mission offer can be reopened from the task journal. Cancel on the wire revokes the offer.
        shroud_selection_open_ = false;
        ++shroud_selection_revision_;
        shroud_stats_.reset();
        shroud_requested_stats_ = shroud_requested_ability_ = shroud_requested_item_ = shroud_ability_detail_ = 0;
    }
    return true;
}

bool GameConnection::request_shroud_ability(std::uint64_t revision, std::uint32_t selection_id, std::uint32_t rank_id) {
    if (!shroud_form_offered(revision, selection_id, false) || !shroud_stats_ ||
        shroud_stats_->selection_id != selection_id ||
        std::none_of(shroud_stats_->abilities.begin(), shroud_stats_->abilities.end(),
                     [rank_id](const ShroudAbility& ability) { return ability.rank_id == rank_id; })) return false;
    shroud_requested_ability_ = rank_id;
    shroud_ability_detail_ = 0;
    send_zone(shroud_selection_->native_mission ? encode_monster_mission_ability_request(selection_id, rank_id)
                                                : encode_shroud_request_ability(selection_id, rank_id));
    return true;
}

bool GameConnection::request_shroud_item(std::uint64_t revision, std::uint32_t selection_id, std::uint32_t item_id) {
    if (!shroud_form_offered(revision, selection_id, false) || !shroud_stats_ ||
        shroud_stats_->selection_id != selection_id ||
        std::none_of(shroud_stats_->items.begin(), shroud_stats_->items.end(),
                     [item_id](const ShroudItem& item) { return item.item_id == item_id; })) return false;
    shroud_requested_item_ = item_id;
    send_zone(shroud_selection_->native_mission ? encode_monster_mission_item_request(selection_id, item_id)
                                                : encode_shroud_request_item(selection_id, item_id));
    return true;
}

bool GameConnection::shared_task_monster_select_available(std::uint32_t task_id) const {
    const game::Task* task = tasks_.find(game::kTaskTypeShared, 0);
    if (state_ != ConnectionState::InZone || shrouded() || zone_change_pending_ || player_down() ||
        task_id == 0 || !task || task->task_id != task_id) return false;
    if (monster_mission_catalog_ && monster_mission_catalog_->task_id == task_id &&
        !monster_mission_catalog_->templates.empty()) return true;
    return shroud_selection_ && shroud_selection_->context_id == task_id && shroud_selection_->status == 0;
}

bool GameConnection::shared_task_monster_select(std::uint32_t task_id) {
    if (!shared_task_monster_select_available(task_id)) return false;
    if (monster_mission_catalog_ && monster_mission_catalog_->task_id == task_id) {
        if (!shroud_selection_ || !shroud_selection_->native_mission) {
            auto catalog = *monster_mission_catalog_;
            set_monster_mission_catalog(std::move(catalog));
        }
        send_zone(encode_monster_mission_catalog_request());
    }
    shroud_selection_open_ = true;
    window_toggle_ = "show:" + std::string(shroud_window(task_id));
    return true;
}

std::uint32_t GameConnection::shared_task_member_template(std::string_view member) const {
    const auto it = tasks_.shared_member_templates.find(member);
    return it == tasks_.shared_member_templates.end() ? 0 : it->second;
}

std::string GameConnection::shared_task_member_template_name(std::string_view member) const {
    const auto id = shared_task_member_template(member);
    if (!id) return {};
    if (const auto* role = monster_mission_template(id)) return role->name;
    if (!monster_mission_catalog_ && id <= 16) return game::class_name(id);
    return {};
}

const MonsterMissionTemplate* GameConnection::monster_mission_template(std::uint32_t id) const {
    if (!monster_mission_catalog_) return nullptr;
    const auto& roles = monster_mission_catalog_->templates;
    const auto found = std::find_if(roles.begin(), roles.end(),
                                    [id](const MonsterMissionTemplate& role) { return role.id == id; });
    return found == roles.end() ? nullptr : &*found;
}

void GameConnection::refresh_monster_mission_availability() {
    if (!shroud_selection_ || !shroud_selection_->native_mission || !monster_mission_catalog_) return;
    // The existing assignment is locked once the server sets the task's flag (CTaskTemplateSelectWnd @0x76d7e0).
    const bool can_change = !monster_mission_catalog_->selection_locked || shared_task_member_template(character_) == 0;
    for (auto& category : shroud_selection_->categories) {
        for (auto& branch : category.branches) {
            for (auto& form : branch.forms) {
                const auto* role = monster_mission_template(form.selection_id);
                if (!role) continue;
                form.assigned_players = static_cast<std::uint32_t>(
                    std::count_if(tasks_.shared_member_templates.begin(), tasks_.shared_member_templates.end(),
                                  [&](const auto& member) { return member.second == role->id; }));
                form.unlocked = can_change && role->selectable &&
                                (form.maximum_players == 0 || form.assigned_players < form.maximum_players);
            }
        }
    }
}

void GameConnection::set_monster_mission_catalog(MonsterMissionCatalog catalog) {
    const auto* task = tasks_.find(game::kTaskTypeShared, 0);
    if (!task || task->task_id != catalog.task_id) return;
    const bool was_open = shroud_selection_open_ && shroud_selection_ &&
                          shroud_selection_->context_id == catalog.task_id;
    const bool first_offer = !monster_mission_catalog_ || monster_mission_catalog_->task_id != catalog.task_id ||
                             monster_mission_catalog_->templates.empty();
    reset_shroud_selection();
    monster_mission_catalog_ = std::move(catalog);
    if (monster_mission_catalog_->templates.empty()) return;

    shroud_selection_ = ShroudSelection{};
    shroud_selection_->native_mission = true;
    shroud_selection_->context_id = task->task_id;
    auto& category = shroud_selection_->categories.emplace_back();
    category.id = task->task_id;
    category.name = task->title;
    for (const bool required : {true, false}) {
        ShroudBranch branch;
        branch.id = required ? 1u : 2u;
        branch.name = required ? "Required" : "Optional";
        for (const auto& role : monster_mission_catalog_->templates) {
            if ((role.minimum > 0) != required) continue;
            auto& form = branch.forms.emplace_back();
            form.selection_id = role.id;
            form.name = role.name;
            form.minimum_players = static_cast<std::uint32_t>(std::max(0, role.minimum));
            form.maximum_players = static_cast<std::uint32_t>(std::max(0, role.maximum));
        }
        if (!branch.forms.empty()) category.branches.push_back(std::move(branch));
    }
    refresh_monster_mission_availability();
    if ((was_open || first_offer) && shared_task_monster_select_available(task->task_id)) {
        shroud_selection_open_ = true;
        window_toggle_ = "show:" + std::string(shroud_window(task->task_id));
    }
}

void GameConnection::receive_shroud_stats(std::optional<ShroudStats> stats) {
    if (stats && shroud_selection_open_ && shroud_requested_stats_ != 0 &&
        stats->selection_id == shroud_requested_stats_) shroud_stats_ = std::move(*stats);
}

void GameConnection::receive_shroud_ability(std::optional<ShroudAbilityReply> reply) {
    if (!reply || !shroud_stats_ || shroud_requested_ability_ == 0 ||
        reply->selection_id != shroud_stats_->selection_id || reply->rank_id != shroud_requested_ability_) return;
    shroud_ability_detail_ = reply->rank_id;
    shroud_requested_ability_ = 0;
}

void GameConnection::receive_shroud_item(std::optional<ShroudItemReply> reply) {
    if (!reply || !shroud_stats_ || shroud_requested_item_ == 0 ||
        (reply->selection_id != 0 && reply->selection_id != shroud_stats_->selection_id) ||
        reply->item_id != shroud_requested_item_) return;
    item_displays_.push_back(std::move(reply->item));
    if (item_displays_.size() > 16) item_displays_.pop_front();
    shroud_requested_item_ = 0;
}

void GameConnection::handle_monster_mission(Op op, const AppPacket& p) {
    switch (op) {
    case Op::MonsterMissionCatalog:
        if (auto catalog = decode_monster_mission_catalog(p)) set_monster_mission_catalog(std::move(*catalog));
        break;
    case Op::MonsterMissionStats:
        if (shroud_selection_ && shroud_selection_->native_mission) receive_shroud_stats(decode_monster_mission_stats(p));
        break;
    case Op::MonsterMissionAbility:
        if (shroud_selection_ && shroud_selection_->native_mission) receive_shroud_ability(decode_monster_mission_ability(p));
        break;
    case Op::MonsterMissionItem:
        if (shroud_selection_ && shroud_selection_->native_mission) receive_shroud_item(decode_monster_mission_item(p));
        break;
    case Op::MonsterMissionAssignment:
        if (auto assignment = decode_monster_mission_assignment(p);
            assignment && std::find(tasks_.shared_members.begin(), tasks_.shared_members.end(), assignment->name) !=
                              tasks_.shared_members.end()) {
            if (assignment->template_id)
                tasks_.shared_member_templates.insert_or_assign(assignment->name, assignment->template_id);
            else tasks_.shared_member_templates.erase(assignment->name);
            refresh_monster_mission_availability();
        }
        break;
    case Op::MonsterMissionResult:
        if (auto result = decode_monster_mission_result(p);
            result && monster_mission_pending_ != 0 && result->template_id == monster_mission_pending_) {
            monster_mission_pending_ = 0;
            if (const auto id = result->message_id()) {
                const auto* role = monster_mission_template(result->template_id);
                system(id, {role ? role->name : std::string{}, shared_task_member_template_name(character_)});
            }
            if (result->result == 0 && shroud_selection_open_) cancel_shroud_selection(shroud_selection_revision_);
        }
        break;
    default:
        break;
    }
}

void GameConnection::sync_shroud_mode() {
    if (profile_->shroud_type != 0) {
        typed_shroud_mode_ = true;
        shroud_mode_ = profile_->shroud_type == 1 ? ShroudMode::Spirit
                     : profile_->shroud_type == 2 ? ShroudMode::MonsterMission : ShroudMode::Unknown;
        return;
    }
    if (std::exchange(typed_shroud_mode_, false)) {
        shroud_mode_ = ShroudMode::Normal;
        return;
    }
    // A form can survive zoning. The draft RoF2 server also marks its character list with the alternate model.
    if (shrouded()) return;
    for (const auto& listed : characters_) {
        if (listed.name != character_) continue;
        if ((listed.shroud_class && listed.shroud_class != listed.class_ && profile_->class_ == listed.shroud_class) ||
            (listed.shroud_race && listed.shroud_race != listed.race && profile_->race == listed.shroud_race))
            shroud_mode_ = ShroudMode::Unknown;
        break;
    }
}

void GameConnection::apply_shroud_transition(ShroudTransition transition) {
    const auto& spawn = transition.spawn;
    const auto prior = world_.spawns.find(spawn.spawn_id);
    if (prior == world_.spawns.end() || prior->second.is_corpse || prior->second.is_npc ||
        spawn.npc != 0 || prior->second.name != spawn.name) return;
    if (spawn.spawn_id != world_.player_id) {
        if (!transition.profile) apply_spawn(spawn);
        return;
    }
    if (!transition.profile) return;
    apply_spawn(spawn);
    profile_ = std::move(transition.profile);
    if (std::exchange(shroud_restoring_, false)) {
        shroud_mode_ = ShroudMode::Normal;
        typed_shroud_mode_ = false;
        profile_->shroud_type = 0;
    } else if (profile_->shroud_type != 0) {
        sync_shroud_mode();
    } else if (shroud_selection_) {
        shroud_mode_ = shroud_selection_->context_id == 0 ? ShroudMode::Spirit : ShroudMode::MonsterMission;
    } else if (!shrouded()) {
        shroud_mode_ = ShroudMode::Unknown;
    }
    reset_shroud_selection();
    close_shroud_bank();
    invalidate_interaction_context();
    pstatus_.hp = profile_->cur_hp;
    pstatus_.mana = profile_->mana;
    pstatus_.endurance = profile_->endurance;
    pstatus_.level = profile_->level;
    drunkenness_ = static_cast<std::int32_t>(std::min(profile_->intoxication, 200u));
    coins_ = {profile_->platinum, profile_->gold, profile_->silver, profile_->copper};
    cursor_coins_ = {profile_->copper_cursor, profile_->silver_cursor, profile_->gold_cursor, profile_->platinum_cursor};
    clear_bard_twist();
    init_spells_from_profile();
    init_bandolier_from_profile();
    aa_buy_all_ = 0;
    std::vector<game::OwnedAbility> owned;
    for (const auto& aa : profile_->aa)
        if (aa[0] && aa[0] != 0xffffffffu) owned.push_back({aa[0], aa[1], aa[2]});
    alt_adv_.timers = {};
    alt_adv_.timers_seen = false;
    alt_adv_.points = profile_->aa_points;
    alt_adv_.set_owned(profile_->aa_points_spent, {}, std::move(owned));
    // The inline item list enumerates the new form's possessions. Keep the separate bank containers.
    for (const auto& [key, item] : inventory_.items())
        if (key.first == kInvBank || key.first == kInvSharedBank) transition.possessions.push_back(item);
    inventory_.load(std::move(transition.possessions));
    mark_cursor_origin(CursorOrigin::Unknown);
    quantity_.reset();
}

void GameConnection::handle_shroud(Op op, const AppPacket& p) {
    switch (op) {
    case Op::ShroudSelectionWindow:
        if (auto offer = decode_shroud_selection(p)) {
            reset_shroud_selection();
            if (offer->status != 0) {
                shroud_bank_available_ = true;
                // 0x414320: a nonzero status prints eqstr 4557 (empty the Shroud Bank first) in colour 0xd.
                system(4557, {}, 0xd);
                break;
            }
            // RoF2 names the shroudkeeper. The EQEmu draft reuses the same field for an active shared task.
            const auto entry = world_.spawns.find(offer->context_id);
            const bool names_npc = entry != world_.spawns.end() && entry->second.is_npc && !entry->second.is_corpse;
            const auto* task = tasks_.find(game::kTaskTypeShared, 0);
            if (names_npc || !task || task->task_id != offer->context_id) {
                shroud_offer_npc_ = offer->context_id;
                offer->context_id = 0;
            }
            const auto context_id = offer->context_id;
            shroud_selection_ = std::move(*offer);
            if (context_id == 0 || shared_task_monster_select_available(context_id)) {
                shroud_selection_open_ = true;
                window_toggle_ = "show:" + std::string(shroud_window(context_id));
            }
        }
        break;
    case Op::ShroudRespondStats:
        if (shroud_selection_ && !shroud_selection_->native_mission) receive_shroud_stats(decode_shroud_stats(p));
        break;
    case Op::ShroudRespondAbility:
        if (shroud_selection_ && !shroud_selection_->native_mission) receive_shroud_ability(decode_shroud_ability(p));
        break;
    case Op::ShroudRespondItem:
        if (shroud_selection_ && !shroud_selection_->native_mission) receive_shroud_item(decode_shroud_item(p));
        break;
    case Op::ShroudProgress:
    case Op::ShroudProgress2:
        if (auto rows = decode_shroud_progress(p)) {
            shroud_progress_available_ = true;
            if (op == Op::ShroudProgress) {
                shroud_progress_ = std::move(*rows);
            } else {
                for (auto& row : *rows) {
                    const auto old = std::find_if(shroud_progress_.begin(), shroud_progress_.end(),
                                                  [&](const ShroudProgress& cur) { return cur.name == row.name; });
                    if (old == shroud_progress_.end()) shroud_progress_.push_back(std::move(row));
                    else *old = std::move(row);
                }
            }
        }
        break;
    case Op::Shroud:
        if (auto form = decode_shroud_transition(p, profile_ ? &*profile_ : nullptr))
            apply_shroud_transition(std::move(*form));
        else
            log::warn("could not decode OP_Shroud ({} bytes)", p.data.size());
        break;
    case Op::ShroudRemove:
        if (p.data.empty()) shroud_restoring_ = true;
        break;
    default:
        break;
    }
}

bool GameConnection::shroud_npc_in_range(std::uint32_t id) const {
    const auto it = world_.spawns.find(id);
    const auto* player = world_.player();
    if (!player || it == world_.spawns.end() || !it->second.is_npc || it->second.is_corpse) return false;
    const auto delta = (motion_known_ ? position_ : player->position) - it->second.position;
    return std::abs(delta.x) + std::abs(delta.y) + std::abs(delta.z) <= kShroudNpcRange;
}

bool GameConnection::can_open_shroud_bank() const {
    return (shroud_bank_available_ || shroud_progress_available_) && state_ == ConnectionState::InZone &&
           !shrouded() && !zone_change_pending_ && !player_down() && bank_ && shroud_npc_in_range(bank_->banker_id);
}

bool GameConnection::open_shroud_bank() {
    if (!can_open_shroud_bank()) return false;
    if (shroud_bank_ && shroud_bank_->entity_id == bank_->banker_id) return true;
    shroud_bank_requested_entity_ = bank_->banker_id;
    send_zone(encode_alt_storage_open(shroud_bank_requested_entity_));
    return true;
}

void GameConnection::close_shroud_bank() {
    shroud_bank_.reset();
    shroud_bank_requested_entity_ = 0;
}

void GameConnection::update_shroud_bank() {
    if (!shroud_bank_) return;
    if (zone_change_pending_ || player_down() || shrouded() || !shroud_npc_in_range(shroud_bank_->entity_id))
        close_shroud_bank();
}

bool GameConnection::shroud_bank_has_entry(std::uint64_t revision, std::uint64_t key) const {
    if (state_ != ConnectionState::InZone || zone_change_pending_ || player_down() || shrouded() || !shroud_bank_ ||
        !shroud_bank_->loaded || shroud_bank_->revision != revision ||
        !shroud_npc_in_range(shroud_bank_->entity_id)) return false;
    return std::any_of(shroud_bank_->entries.begin(), shroud_bank_->entries.end(),
                       [key](const AltStorageEntry& row) { return row.key == key; });
}

bool GameConnection::inspect_shroud_bank(std::uint64_t revision, std::uint64_t key, bool preview) {
    if (!shroud_bank_has_entry(revision, key)) return false;
    send_zone(encode_alt_storage_inspect(key, preview ? 1u : 0u));
    return true;
}

bool GameConnection::withdraw_shroud_bank(std::uint64_t revision, std::uint64_t key) {
    if (!shroud_bank_has_entry(revision, key) || shroud_bank_->pending_key) return false;
    shroud_bank_->pending_key = key;
    send_zone(encode_alt_storage_withdraw(key));
    return true;
}

void GameConnection::handle_shroud_bank(Op op, const AppPacket& p) {
    if (op == Op::AltStorageNoItems) {
        if (p.data.empty()) {
            shroud_bank_available_ = true;
            close_shroud_bank();
            // 0x4136f0 (routed by 0x414a90): eqstr 4558, the Shroud Bank not opened for want of items, colour 0xf.
            system(4558, {}, 0xf);
        }
        return;
    }
    if (op == Op::AltStorageList) {
        if (auto list = decode_alt_storage_list(p)) {
            if (!shroud_bank_ && shroud_bank_requested_entity_ && can_open_shroud_bank() &&
                bank_->banker_id == shroud_bank_requested_entity_) {
                shroud_bank_ = ShroudBankState{};
                shroud_bank_->entity_id = shroud_bank_requested_entity_;
            }
            shroud_bank_requested_entity_ = 0;
            if (!shroud_bank_) return;
            shroud_bank_->retention_seconds = list->retention_seconds;
            shroud_bank_->entries = std::move(list->entries);
            shroud_bank_->revision = core::next_process_identity();
            shroud_bank_->loaded = true;
        }
        return;
    }
    const auto control = decode_alt_storage(p);
    if (!control) return;
    if (control->action == AltStorageAction::Currency) {
        // CAltStorageWnd_HandleMessage @0x61da10 case 6: the coin handed back from the shroud, as eqstr 6188 with
        // FormatCoinAmountsText's phrase, in colour 0xf. Nothing is kept in the window.
        system(6188, {game::coin_amounts_text(control->platinum, control->gold, control->silver, control->copper)},
               0xf);
        return;
    }
    if (control->action == AltStorageAction::Open && control->entity_id != 0) {
        shroud_bank_available_ = true;
        if (shrouded() || player_down() || zone_change_pending_ || !shroud_npc_in_range(control->entity_id)) return;
        shroud_bank_requested_entity_ = 0;
        if (!shroud_bank_ || shroud_bank_->entity_id != control->entity_id) {
            shroud_bank_ = ShroudBankState{};
            shroud_bank_->entity_id = control->entity_id;
            shroud_bank_->revision = core::next_process_identity();
        }
    }
    if (!shroud_bank_) return;
    switch (control->action) {
    case AltStorageAction::Close:
        close_shroud_bank();
        break;
    case AltStorageAction::Withdraw:
        if (!shroud_bank_->pending_key) break;
        if (control->storage_key == kFailedWithdrawal) shroud_bank_->pending_key.reset();
        else if (control->storage_key == *shroud_bank_->pending_key) {
            shroud_bank_->pending_key.reset();
            std::erase_if(shroud_bank_->entries, [&](const AltStorageEntry& row) { return row.key == control->storage_key; });
            shroud_bank_->revision = core::next_process_identity();
        }
        break;
    case AltStorageAction::Remove:
        std::erase_if(shroud_bank_->entries, [&](const AltStorageEntry& row) { return row.key == control->storage_key; });
        shroud_bank_->revision = core::next_process_identity();
        break;
    case AltStorageAction::Refresh:
        shroud_bank_->entries.clear();
        shroud_bank_->pending_key.reset();
        shroud_bank_->revision = core::next_process_identity();
        break;
    default:
        break;
    }
}

} // namespace hailstone::net
