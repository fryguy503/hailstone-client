// GameConnection: alternate advancement, training at guildmasters, tradeskill combines, tasks, and the
// skill and level messages (game/alt_adv.h, game/training.h, game/tasks.h; net/aa_packets.h,
// net/training_packets.h, net/task_packets.h). docs/protocol/rof2-packets.md "Alternate advancement",
// "Training", "Tradeskills", "Tasks".
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <format>
#include <set>

#include "core/log.h"
#include "game/eq_names.h"
#include "game/item_options.h"
#include "game/merchant.h"
#include "game/mechanics/character.h"
#include "game/mechanics/sheet_builder.h"
#include "game/mechanics/tables.h"
#include "game/slash_commands.h"
#include "net/aa_packets.h"
#include "net/ability_packets.h"
#include "net/door_packets.h"
#include "net/game_connection.h"
#include "net/packet_io.h"
#include "net/task_packets.h"
#include "net/training_packets.h"

namespace hailstone::net {

namespace {

constexpr float kInteractRange = 20.0f; // PlayerClient_IsWithinInteractRange(npc, 1, 20.0)
constexpr std::uint8_t kFirstGuildmaster = 20, kLastGuildmaster = 35;

std::int64_t unix_now() { return static_cast<std::int64_t>(std::time(nullptr)); } // Util::FastTime

bool iequals(std::string_view a, std::string_view b) { // __stricmp == 0
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

} // namespace

// ---------------------------------------------------------------- packets

void GameConnection::handle_progress(Op op, const AppPacket& p) {
    switch (op) {
    // ---- alternate advancement
    case Op::SendAATable:
        if (auto a = decode_send_aa_table(p)) alt_adv_.add(std::move(*a));
        else log::warn("could not decode OP_SendAATable ({} bytes)", p.data.size());
        break;
    case Op::RespondAA:
        if (auto r = decode_respond_aa(p)) {
            alt_adv_.set_owned(r->spent, r->assigned, std::move(r->owned));
            log::debug("AAs: {} owned, {} spent", alt_adv_.owned.size(), alt_adv_.spent);
        }
        break;
    case Op::AAExpUpdate:
        // Handle_OP_AAExpUpdate @0x4b70c0.
        if (auto u = decode_aa_exp_update(p)) {
            alt_adv_.exp = u->exp;
            alt_adv_.points = u->points;
            alt_adv_.percent = u->percent;
            pstatus_.aa_exp = u->exp;
            // PcBase_SetAAPoints writes the profile's unspent count (+0x8900), which the AA labels read.
            if (profile_) profile_->aa_points = u->points;
            continue_aa_buy_all();
        }
        break;
    case Op::AAAction:
        // Handle_OP_AAAction @0x4b5bd0: a reuse timer, on the server's clock.
        if (auto t = decode_aa_timer(p)) alt_adv_.set_timer(t->timer, t->begin, t->server_now, unix_now());
        break;
    case Op::ClearAA:
        alt_adv_.clear_owned();
        break;

    // ---- training
    case Op::GMTraining:
        // Handle_OP_GMTraining @0x4b49c0: the guildmaster must still be within reach
        // (PlayerClient_IsWithinInteractRange(gm, 0, 20.0): 3D, at most 20).
        if (auto g = decode_gm_training(p)) {
            training_requested_ = 0;
            auto it = world_.spawns.find(g->trainer_id);
            if (it == world_.spawns.end() || distance_to(g->trainer_id) > kInteractRange) break;
            if (!g->success) break; // the original then ends the interaction
            game::TrainingSession s;
            s.trainer_id = g->trainer_id;
            s.trainer_name = game::display_name(it->second.name);
            s.skill_caps = g->skills;
            s.language_caps = g->languages;
            s.greed = g->greed;
            training_ = std::move(s);
            training_->rows = game::training_rows(trainee(), training_->skill_caps, training_->language_caps);
            log::info("training with {} ({} rows)", training_->trainer_name, training_->rows.size());
        }
        break;
    case Op::GMTrainSkillConfirm:
        // HandleWorldMessage @0x4d2747: pay, one practice point less, the guildmaster's line (colour 0x111).
        if (auto c = decode_gm_train_skill_confirm(p)) {
            if (!game::take_money(coins_, c->cost)) coins_ = {};
            if (profile_ && profile_->points > 0) --profile_->points;
            std::string what = c->skill > 99 ? strings_.format(game::language_eqstr(c->skill - 100), {})
                                             : skill_name(c->skill);
            std::uint32_t id = c->new_skill ? 12066 : (c->skill > 99 ? 12067 : 12068);
            system(id, {c->trainer, what}, 0x111);
        }
        break;

    // ---- tradeskills
    case Op::TradeSkillCombine:
        // The server's acknowledgement (HandleWorldMessage @0x4c73e3: the pending request ends).
        combine_pending_ = false;
        break;
    case Op::RecipeReply:
        if (auto r = decode_recipe_reply(p); r && tradeskill_.open) tradeskill_.add_recipe(*r);
        break;
    case Op::RecipeDetails:
        if (auto d = decode_recipe_details(p); d && tradeskill_.open && d->recipe_id == tradeskill_.selected)
            tradeskill_.details = std::move(*d);
        break;
    case Op::RecipeAutoCombine:
        // The answer (EQEmu HandleAutoCombine: the server also says what went wrong, and sends the items).
        if (auto a = decode_recipe_auto_combine(p)) {
            tradeskill_.combining = false;
            if (auto& b = tradeskill_.batch; b && a->recipe_id == b->recipe_id) {
                // 0: the components were taken (the combine made or failed); else nothing happened (missing
                // components, a lore conflict: the server says which).
                if (a->reply_code != 0) {
                    tradeskill_stop_batch("the combine did not start");
                } else {
                    ++b->done;
                    b->next_ms = now_ms() + game::kBatchSettleMs;
                    b->collect_until_ms = b->next_ms + game::kBatchCollectMs;
                }
            }
        }
        break;

    // ---- tasks (CTaskManager::HandleMessage @0x482080). Each change the original marks the task window for a
    // rebuild with (0x76ff50) bumps tasks_.revision and looks for the compass mark again.
    case Op::TaskDescription:
        // CTaskManager_HandleTaskDescription @0x4812b0: with open_window set the entry is emptied first and the
        // window shown.
        if (auto t = decode_task_description(p)) {
            t->received_ms = now_ms();
            bool open = t->open_window;
            const game::Task& task = tasks_.describe(std::move(*t));
            ++tasks_.revision;
            task_compass_dirty_ = true;
            if (open) window_toggle_ = "show:TaskWnd";
            log::info("task {} '{}' (type {}, slot {})", task.task_id, task.title, task.type, task.sequence);
        } else {
            log::warn("could not decode OP_TaskDescription ({} bytes)", p.data.size());
        }
        break;
    case Op::TaskActivity:
        if (auto a = decode_task_activity(p)) {
            if (!tasks_.update_element(a->type, a->sequence, a->task_id, std::move(a->element)))
                log::debug("task activity for an unknown task {} (type {}, slot {})", a->task_id, a->type, a->sequence);
            ++tasks_.revision;
            task_compass_dirty_ = true;
        } else {
            log::warn("could not decode OP_TaskActivity ({} bytes)", p.data.size());
        }
        break;
    case Op::CompletedTasks:
        // CTaskManager_HandleCompletedTasks @0x480c50: the history entries emptied (their elements too), the list
        // read again.
        if (auto c = decode_completed_tasks(p)) {
            tasks_.completed = std::move(*c);
            tasks_.history.clear();
            ++tasks_.revision;
        }
        break;
    case Op::TaskSelectWindow:
    case Op::SharedTaskSelectWindow:
        if (auto o = decode_task_select(p)) {
            log::info("{} task offer(s) from {}", o->offers.size(), o->giver_id);
            tasks_.offers = std::move(*o);
        } else {
            log::warn("could not decode the task select window ({} bytes)", p.data.size());
        }
        break;
    case Op::TaskHistoryReply:
        // CTaskManager_HandleTaskHistoryReply @0x480ac0: the entry at the packet's index (below 50) gets the elements
        // and is marked received; the window shows them (CTaskWnd_FillHistoryElements).
        if (auto h = decode_task_history(p); h && h->index < 50) tasks_.history[h->index] = std::move(h->elements);
        break;
    case Op::TaskActivityComplete:
        // CTaskManager::HandleMessage @0x48253a: a stage (6005), or the task's end: completed (6003) or
        // failed (6004) with its title, as the large centre-screen text only (game/text_overlay.h).
        if (auto c = decode_task_activity_complete(p)) {
            game::TaskCompleteText t = game::task_complete_text(c->stage_complete != 0, c->completed != 0);
            std::string text;
            if (!t.with_title) {
                text = strings_.format(t.eqstr, std::vector<std::string>{});
            } else {
                if (!game::task_slot_valid(c->type, c->index)) break; // the original reads a null slot
                const game::Task* task = tasks_.find(c->type, c->index);
                if (!task || task->task_id != c->task_id) break;
                text = strings_.format(t.eqstr, std::vector<std::string>{task->title});
            }
            if (text.empty()) break;
            text_overlay_.display(std::move(text), t.colour, t.priority, game::kTaskTextAlpha, game::kTaskTextFadeInMs,
                                  game::kTaskTextFadeOutMs, game::kTaskTextHoldMs, static_cast<std::uint32_t>(now_ms()));
        }
        break;
    case Op::CancelTask:
        // CTaskManager_HandleCancelTask @0x4811a0: the entry emptied; for the shared task (type 1) the shared task id
        // and the member list too.
        if (auto c = decode_cancel_task(p)) {
            tasks_.remove(c->type, c->sequence);
            if (c->type == game::kTaskTypeShared) {
                tasks_.shared_members.clear();
                tasks_.shared_leader.clear();
            }
            ++tasks_.revision;
            task_compass_dirty_ = true;
        }
        break;
    case Op::SharedTaskMemberList:
        // CTaskManager_HandleSharedTaskMemberList @0x480d70 (SharedTaskMemberList_Deserialize @0x4828f0): the list
        // replaced, each member {name, template id, role}; CTaskWnd_FillMemberList shows the role STPR_Leader as
        // "Leader".
        if (auto v = decode_shared_task_members(p)) {
            tasks_.shared_members.clear();
            tasks_.shared_leader.clear();
            for (auto& m : *v) {
                if (m.leader) tasks_.shared_leader = m.name;
                tasks_.shared_members.push_back(std::move(m.name));
            }
        }
        break;
    case Op::SharedTaskMemberChange:
        // CTaskManager_HandleSharedTaskMemberChange @0x480cd0: the member added (0x2165) or removed (0x2166),
        // dsp_chat colour 0.
        // SharedTask_AddMember @0x4804b0 appends a node (no check for a name already there, no role);
        // SharedTask_RemoveMemberByName @0x480550 unlinks the first node with the name (strcmp) after the head,
        // or the head itself when it matches.
        if (auto v = decode_shared_task_member_change(p)) {
            auto& m = tasks_.shared_members;
            if (v->added) {
                m.push_back(v->name);
            } else if (!m.empty()) {
                if (m.front() == v->name) {
                    if (tasks_.shared_leader == m.front()) tasks_.shared_leader.clear();
                    m.erase(m.begin());
                } else {
                    // The original's loop unlinks every later node with the name (it checks each next node).
                    for (std::size_t i = 1; i < m.size();) {
                        if (m[i] == v->name) {
                            if (tasks_.shared_leader == m[i]) tasks_.shared_leader.clear();
                            m.erase(m.begin() + static_cast<std::ptrdiff_t>(i));
                        } else {
                            ++i;
                        }
                    }
                }
            }
            system(v->added ? 0x2165 : 0x2166, {v->name});
        }
        break;
    case Op::TaskRequestTimer:
        // PcBase::TaskRequestTimer = CDisplay::TimeStamp + the packet's milliseconds (label EQType 132 counts it down).
        if (auto ms = decode_task_request_timer(p)) tasks_.request_timer_until_ms = now_ms() + *ms;
        break;
    default:
        break;
    }
}

void GameConnection::reset_progress_for_zone() {
    training_.reset();
    training_requested_ = 0;
    combine_pending_ = false;
    tasks_.offers.reset();
    aa_requested_ = false;
    aa_buy_all_ = 0;
}

void GameConnection::request_aa_data() {
    // ProcessGame after EnterZone ("Requesting AA data"): the table unless it is already loaded
    // (Send_OP_SendAATable @0x4a39b0), and the owned ranks (Send_OP_UpdateAA @0x756ff0).
    if (aa_requested_) return;
    aa_requested_ = true;
    if (alt_adv_.table.empty()) send_zone(make_empty(Op::SendAATable));
    send_zone(make_empty(Op::UpdateAA));
}

// ---------------------------------------------------------------- skills and levels

void GameConnection::handle_skill_update(const AppPacket& p) {
    // Handle_OP_SkillUpdate @0x4be7c0: a skill (0..99) or a language (100..131), and eqstr 12091 with its
    // name and new value when the packet's byte 8 is set (EQEmu always sets it).
    auto u = decode_skill_update(p);
    if (!u || !profile_ || static_cast<std::int32_t>(u->skill) < 0) return;
    std::string name;
    std::int32_t value = static_cast<std::int32_t>(u->value);
    if (u->skill >= 100) {
        std::uint32_t lang = u->skill - 100;
        if (lang >= 0x20) return;
        if (profile_->languages.size() <= lang) profile_->languages.resize(lang + 1);
        profile_->languages[lang] = static_cast<std::uint8_t>(u->value);
        value = profile_->languages[lang];
        name = strings_.format(game::language_eqstr(lang), {});
    } else {
        if (u->skill >= profile_->skills.size()) return;
        profile_->skills[u->skill] = u->value;
        name = skill_name(u->skill);
    }
    if (u->announce) system(12091, {name, std::to_string(value)}, 270); // the Skills filter
}

void GameConnection::handle_level_update(std::uint32_t new_level) {
    // Handle_OP_LevelUpdate @0x4b9c10: losing levels prints 442 /
    // 443 (colour 13); gaining one adds 5 practice points.
    std::uint32_t current = pstatus_.level;
    if (current == 0) return;
    if (new_level < current) {
        std::uint32_t lost = current - new_level;
        if (lost == 1) system(442, {std::to_string(new_level)}, 13);
        else system(443, {std::to_string(lost), std::to_string(new_level)}, 13);
    }
    if (new_level > current && profile_) profile_->points += 5;
}

std::int32_t GameConnection::skill_value(std::uint32_t skill) const {
    if (!profile_) return 0;
    if (skill >= 100) {
        std::uint32_t lang = skill - 100;
        return lang < profile_->languages.size() ? profile_->languages[lang] : 0;
    }
    return skill < profile_->skills.size() ? static_cast<std::int32_t>(profile_->skills[skill]) : 0;
}

std::string GameConnection::skill_name(std::uint32_t skill) const {
    // CSkillMgr::GetNameToken → StringTable::getString.
    return strings_.format(game::mechanics::skill_name_eqstr(skill), {});
}

// ---------------------------------------------------------------- alternate advancement

std::string GameConnection::aa_name(const game::AltAbility& a) const {
    return strings_.db_string(static_cast<std::uint32_t>(a.title_sid), 1);
}

std::string GameConnection::aa_description(const game::AltAbility& a) const {
    return strings_.db_string(static_cast<std::uint32_t>(a.desc_sid), 4);
}

std::string GameConnection::aa_category(const game::AltAbility& a) const {
    if (a.category > 0) return strings_.db_string(static_cast<std::uint32_t>(a.category), 0x13);
    if (a.expansion != 0) return strings_.db_string(static_cast<std::uint32_t>(a.expansion), 0x14);
    return {};
}

bool GameConnection::aa_can_train(std::uint32_t rank_id) const {
    const game::AltAbility* a = alt_adv_.get(rank_id);
    if (!a || !profile_) return false;
    game::AATrainer who;
    who.class_id = profile_->class_;
    who.level = pstatus_.level ? pstatus_.level : profile_->level;
    who.expansions = profile_->expansions;
    who.ignore_level = server_ext().aa_any_level(*this); // the NMS patch: AAs from level 1
    return alt_adv_.can_train(*a, who);
}

std::vector<const game::AltAbility*> GameConnection::aa_list(std::int32_t type, const game::AAFilter& filter) const {
    if (!profile_) return {};
    game::AATrainer who;
    who.class_id = profile_->class_;
    who.level = pstatus_.level ? pstatus_.level : profile_->level;
    who.expansions = profile_->expansions;
    who.ignore_level = server_ext().aa_any_level(*this);
    return alt_adv_.visible(type, who.class_id, filter, who);
}

game::AAReadiness GameConnection::aa_readiness(std::uint32_t rank_id) const {
    const game::AltAbility* a = alt_adv_.get(rank_id);
    if (!a) return {};
    std::int32_t reduction = 0;
    if (a->refresh != 0 && profile_) {
        // TotalEffect(SPA 264, items, the ability's group, AAs, buffs) through the character rules.
        static const game::mechanics::GameTables no_tables;
        game::mechanics::LiveState live;
        live.drunkenness = drunkenness_;
        live.owner_status = pstatus_.owner_status;
        live.pvp_rules = pvp_rules_;
        live.level = pstatus_.level;
        live.player_id = world_.player_id;
        live.now_ms = now_ms();
        game::mechanics::CharacterSheet sheet = game::mechanics::build_sheet(*profile_, inventory_, spells_, live, nullptr, &world_);
        sheet.abilities = alt_adv_.sheet_abilities();
        game::mechanics::Character ch(sheet, no_tables, game::mechanics::spell_lookup(*spell_table_));
        reduction = ch.total_effect(264, true, a->group, true, true);
    }
    return alt_adv_.readiness(*a, reduction, unix_now());
}

bool GameConnection::aa_train(std::uint32_t rank_id, bool train_all) {
    if (state_ != ConnectionState::InZone) return false;
    if (!aa_can_train(rank_id)) {
        system(13277);
        return false;
    }
    // A server's client may buy all ranks (the NMS patch's SendMessage detour: with a Train All box checked,
    // OP_AAAction 3 (buy) becomes 4 (buy all)).
    const bool server_all = train_all && server_ext().aa_train_all(*this);
    // Deliberate difference: without such a server the box still buys every rank, one OP_AAAction 3 at a time
    // (continue_aa_buy_all), the stock purchase repeated; a plain train ends that.
    aa_buy_all_ = train_all && !server_all ? rank_id : 0;
    send_zone(encode(AAAction{server_all ? kAATrainAll : kAATrain, rank_id, 0, 0}));
    return true;
}

void GameConnection::continue_aa_buy_all() {
    // EQEmu's FinishAlternateAdvancementPurchase answers a purchase with the next rank (OP_SendAATable), the owned
    // list (OP_RespondAA) and then the points (OP_AAExpUpdate), so this runs on the last. An update with the rank
    // not owned yet (experience from a kill, the AA share changed, a refused purchase, which EQEmu does not answer)
    // leaves the chain waiting: every other way to buy a rank is a plain aa_train, which ends it, as zoning does.
    if (!aa_buy_all_ || !alt_adv_.has(aa_buy_all_)) return;
    const game::AltAbility* a = alt_adv_.get(aa_buy_all_);
    aa_buy_all_ = 0;
    if (!a || a->next_id <= 0) return;
    auto next = static_cast<std::uint32_t>(a->next_id);
    if (aa_can_train(next)) aa_train(next, true);
}

void GameConnection::aa_train_button(std::uint32_t rank_id, bool train_all) {
    // CAAWnd::WndNotification @0x60aa80, Train: Send_OP_AAAction_TrainSelected at once with AANoConfirm (0xde0d04),
    // else ConfirmationDialog(0x64, 4041 / 4042 by whether the rank costs 1: the cost "%d" and its name, 20 s), not
    // for an unknown rank.
    if (state_ != ConnectionState::InZone) return;
    if (client_option(game::ClientOption::AANoConfirm)) {
        aa_train(rank_id, train_all);
        return;
    }
    const game::AltAbility* a = alt_adv_.get(rank_id);
    if (!a) return;
    Confirmation c;
    c.id = kConfirmAATrain;
    c.text = strings_.format(game::aa_train_question(a->cost), std::vector<std::string>{std::to_string(a->cost), aa_name(*a)});
    c.timeout_ms = game::kPurchaseConfirmMs;
    c.data = rank_id;
    c.data2 = train_all ? 1 : 0;
    ask(std::move(c));
}

bool GameConnection::aa_activate(std::uint32_t group) {
    if (state_ != ConnectionState::InZone) return false;
    const game::AltAbility* a = alt_adv_.owned_rank(static_cast<std::int32_t>(group));
    if (!a) {
        system(6198);
        return false;
    }
    // cmd_alternateadv @0x4f73e0: an ability whose spell is not cast sitting (+0x240 clear, or no spell) stands a
    // sitting player up first with AllowAutoStand (0xde0d5b).
    const game::SpellData* spell = a->spell >= 0 ? spell_table_->get(static_cast<std::uint32_t>(a->spell)) : nullptr;
    if ((!spell || !spell->cast_not_standing) && sitting() && client_option(game::ClientOption::AllowAutoStand))
        set_stand_state(kStandStanding);
    send_zone(encode(AAAction{kAAActivate, a->id, world_.target_id, 0}));
    return true;
}

void GameConnection::set_aa_percent(std::uint32_t percent) {
    if (state_ != ConnectionState::InZone) return;
    alt_adv_.percent = static_cast<std::uint8_t>(std::min<std::uint32_t>(percent, 100));
    if (alt_adv_.percent == 0) send_zone(encode(AAAction{kAAExpOff, 0, 0, 0}));
    else send_zone(encode(AAAction{kAASetPercent, 0, 0, alt_adv_.percent}));
}

// ---------------------------------------------------------------- training

const game::mechanics::GameTables& GameConnection::tables() const {
    if (!tables_) {
        tables_ = std::make_unique<game::mechanics::GameTables>();
        if (!tables_->load(game_)) log::warn("training: SkillCaps / BaseData not found in the game directory");
    }
    return *tables_;
}

game::TraineeView GameConnection::trainee() const {
    game::TraineeView v;
    if (!profile_) return v;
    v.race = profile_->race;
    v.class_id = profile_->class_;
    // NMS multiclass characters are Bard in the profile: the rules use the guildmaster's class when it
    // is one of the player's (the server's OPGMTraining does the same with the class bits).
    if (training_ || training_requested_) {
        std::uint32_t npc = training_ ? training_->trainer_id : training_requested_;
        auto it = world_.spawns.find(npc);
        if (it != world_.spawns.end() && it->second.class_id >= kFirstGuildmaster) {
            std::uint32_t cls = it->second.class_id - 19u;
            if (player_class_mask() & (1u << (cls - 1))) v.class_id = static_cast<std::uint8_t>(cls);
        }
    }
    v.level = pstatus_.level ? pstatus_.level : profile_->level;
    v.practice_points = static_cast<std::int32_t>(profile_->points);
    v.skill = [this](std::uint32_t s) { return skill_value(s); };
    v.language = [this](std::uint32_t l) { return skill_value(100 + l); };
    std::uint8_t cls = v.class_id;
    v.class_cap = [this, cls](std::uint32_t s) { return s < 100 ? tables().skill_caps.cap(cls, s, 100) : 0; };
    v.min_level = [this, cls](std::uint32_t s) { return tables().skill_caps.min_level(s, cls); };
    // PcClient::AtSkillLimit → GetPcSkillLimit @0x584be0 (game/training.h): the skill cap at the player's level
    // with its items, AAs and buffs, the tradeskill 200 cap and the specialization limits.
    std::int32_t level = v.level;
    v.at_skill_limit = [this, cls, level](std::uint32_t s) {
        if (s >= 100) return false;
        game::mechanics::LiveState live;
        live.drunkenness = drunkenness_;
        live.owner_status = pstatus_.owner_status;
        live.pvp_rules = pvp_rules_;
        live.level = level;
        live.player_id = world_.player_id;
        live.now_ms = now_ms();
        game::mechanics::CharacterSheet sheet = game::mechanics::build_sheet(*profile_, inventory_, spells_, live, nullptr, &world_);
        sheet.abilities = alt_adv_.sheet_abilities();
        game::mechanics::Character ch(sheet, tables(), game::mechanics::spell_lookup(*spell_table_));
        game::SkillLimitView who;
        who.level = level;
        who.class_id = cls;
        who.skill = [this](std::uint32_t k) { return skill_value(k); };
        who.skill_cap = [&ch](std::int32_t l, std::int32_t c, std::uint32_t k) {
            return ch.skill_cap(l, c, static_cast<std::int32_t>(k));
        };
        who.level_capped = [](std::uint32_t k) { return game::mechanics::skill_level_capped(k); };
        who.highest_level = [&sheet] { return sheet.highest_level; };
        who.total_effect = [&ch](std::int32_t spa) { return ch.total_effect(spa); };
        return game::at_skill_limit(who, s);
    };
    return v;
}

bool GameConnection::open_trainer(std::uint32_t npc_id) {
    if (state_ != ConnectionState::InZone || pstatus_.dead || !profile_) return false;
    auto it = world_.spawns.find(npc_id);
    const game::Spawn* me = world_.player();
    if (it == world_.spawns.end() || !me || !it->second.is_npc || it->second.is_corpse) return false;
    std::uint8_t npc_class = it->second.class_id;
    if (npc_class < kFirstGuildmaster || npc_class > kLastGuildmaster) return false;
    glm::vec3 at = motion_known_ ? position_ : me->position;
    if (glm::distance(at, it->second.position) >= kInteractRange) return false;
    // RightClickedOnPlayer @0x537462: only the player's own class guild: profile class + 19 (NMS: also any of
    // the multiclass classes; EQEmu with AllowCrossClassTrainers shows every guildmaster as the viewer's).
    std::uint32_t cls = npc_class - 19u;
    if (profile_->class_ + 19u != npc_class && !(player_class_mask() & (1u << (cls - 1)))) {
        system(1467, {game::class_name(cls)});
        return false;
    }
    if (training_ || training_requested_) return false;
    if (auto_attack_) {
        set_auto_attack(false);
        system(1466);
    }
    GMTraining g;
    g.trainer_id = npc_id;
    g.player_id = world_.player_id;
    send_zone(encode(g));
    training_requested_ = npc_id;
    return true;
}

bool GameConnection::train(std::size_t row) {
    if (!training_ || row >= training_->rows.size() || state_ != ConnectionState::InZone) return false;
    const game::TrainingRow& r = training_->rows[row];
    if (auto refusal = game::check_train(trainee(), r)) {
        if (refusal->id == 0) return false;
        std::vector<std::string> args;
        if (refusal->trainer) args.push_back(training_->trainer_name);
        if (refusal->named)
            args.push_back(r.language() ? strings_.format(game::language_eqstr(static_cast<std::uint32_t>(r.id())), {})
                                        : skill_name(static_cast<std::uint32_t>(r.id())));
        system(refusal->id, args);
        return false;
    }
    send_zone(encode_gm_train_skill(training_->trainer_id, r.language(), static_cast<std::uint32_t>(r.id())));
    return true;
}

std::int32_t GameConnection::train_cost(std::size_t row) const {
    if (!training_ || row >= training_->rows.size()) return 0;
    return game::training_cost(skill_value(static_cast<std::uint32_t>(training_->rows[row].kind)));
}

void GameConnection::close_trainer() {
    std::uint32_t npc = training_ ? training_->trainer_id : training_requested_;
    if (!npc) return;
    if (state_ == ConnectionState::InZone) send_zone(encode_gm_end_training(npc, world_.player_id));
    training_.reset();
    training_requested_ = 0;
}

// ---------------------------------------------------------------- tradeskills

bool GameConnection::combine_gates(std::uint32_t type) {
    // CContainerWnd::HandleCombine @0x65f7b0 and CTradeSkillWnd::HandleCombine @0x77e100: race and class containers
    // (colour 15).
    std::uint32_t race = profile_ ? profile_->race : 0;
    std::uint32_t mask = player_class_mask();
    auto has_class = [&](std::uint32_t c) { return (mask & (1u << (c - 1))) != 0; };
    struct Gate {
        std::uint32_t type;
        bool by_class;
        std::uint32_t who;
        std::uint32_t eqstr;
    };
    static constexpr Gate kGates[] = {
        {0x18, true, 12, 12010},  {0x19, true, 13, 12010},  {0x1a, true, 11, 12010},  {0x1b, true, 14, 12010},
        {0x1f, false, 5, 12011},  {0x20, false, 6, 12012},  {0x21, false, 10, 12013}, {0x22, false, 8, 12014},
        {0x23, false, 12, 12015}, {0x24, false, 2, 12016},  {0x26, false, 128, 12017}, {0x27, false, 1, 12018},
        {0x28, false, 1, 12018},  {0x2d, false, 128, 12019}, {0x2a, false, 3, 12020}, {0x29, false, 11, 12021},
        {0x2b, false, 4, 12022},  {0x2c, false, 4, 12023},
    };
    for (const Gate& g : kGates) {
        if (g.type != type) continue;
        bool ok = g.by_class ? has_class(g.who) : race == g.who;
        if (!ok) {
            system(g.eqstr, {}, 15);
            return false;
        }
    }
    return true;
}

bool GameConnection::combine_checks(std::uint32_t type, const std::vector<const Item*>& contents) {
    if (!combine_gates(type)) return false;
    // Nothing on the cursor, item or coin (12024).
    bool cursor_coin = std::any_of(cursor_coins_.begin(), cursor_coins_.end(), [](std::uint32_t c) { return c != 0; });
    if (inventory_.cursor() || cursor_coin) {
        system(12024);
        return false;
    }
    // No stacks (8504, an OK dialog in the original).
    for (const Item* it : contents)
        if (it && it->stackable() && it->stack_count > 1) {
            system(8504);
            return false;
        }
    if (std::none_of(contents.begin(), contents.end(), [](const Item* it) { return it != nullptr; })) {
        system(12025);
        return false;
    }
    return true;
}

bool GameConnection::combine_container(std::int16_t bag_slot) {
    if (state_ != ConnectionState::InZone || combine_pending_) return false;
    const Item* bag = inventory_.at(ItemSlot::possessions(bag_slot));
    if (!bag || !bag->is_bag() || bag_slot > kSlotCursor) return false;
    std::vector<const Item*> contents;
    for (const BagItem& b : bag->contents) contents.push_back(&b.item);
    if (!combine_checks(bag->bag_type, contents)) return false;
    send_combine(ItemSlot::possessions(bag_slot), bag->bag_type);
    log::info("combine in {} ({}, type {})", possessions_slot_name(bag_slot), bag->name, bag->bag_type);
    return true;
}

void GameConnection::send_combine(const ItemSlot& container_slot, std::uint32_t container_type) {
    // The end of CContainerWnd::HandleCombine @0x65f7b0: the request is pending (IsItemPending, reason 0x13) before
    // anything is sent. A transformation mold (0x37 / 0x38) first asks 6328 / 6339 (ConfirmationDialog 0x86, Yes /
    // No, no time limit, with a copy of the request, 0x65da10); ConfirmationDialog_HandleResponse's 0x86 sends
    // that copy on a yes and takes the pending request back on a no (0x4232f0).
    TradeSkillCombine c;
    c.container_slot = container_slot;
    combine_pending_ = true;
    if (container_type == 0x37 || container_type == 0x38) {
        Confirmation q;
        q.id = kConfirmMoldCombine;
        q.slot = container_slot;
        q.text = strings_.format(container_type == 0x37 ? 6328 : 6339, std::vector<std::string>{});
        q.offer = encode(c);
        ask(std::move(q));
        return;
    }
    send_zone(encode(c));
}

// ---------------------------------------------------------------- the tradeskill window

void GameConnection::open_tradeskill_bag(const ItemSlot& bag_slot) {
    const Item* bag = inventory_.at(bag_slot);
    if (!bag || !bag->is_bag() || !game::is_combine_container(bag->bag_type)) return;
    tradeskill_ = {};
    tradeskill_.open = true;
    tradeskill_.container_slot = bag_slot;
    tradeskill_.object_type = bag->bag_type;
    tradeskill_.container_id = bag->id;
    tradeskill_.container_name = bag->name;
    tradeskill_.container_icon = bag->icon;
    tradeskill_show_favorites();
}

bool GameConnection::container_opens_tradeskill(const ItemSlot& bag_slot, bool ctrl) {
    const Item* bag = inventory_.at(bag_slot);
    if (!bag || !bag->is_bag() || !profile_) return false;
    game::OpenContainerInput in;
    in.tradeskill = game::container_tradeskill_wanted(client_options_.get(game::ClientOption::TradeskillCtrlClick) != 0, ctrl);
    in.tradeskill_window_active = tradeskill_.open;
    in.top_level_or_world = bag_slot.type == kInvPossessions && bag_slot.sub < 0 && bag_slot.slot >= kSlotGeneral1 &&
                            bag_slot.slot <= kSlotGeneral10;
    in.bag_type = bag->bag_type;
    return game::opens_in_tradeskill_window(in, [&] {
        // PcZoneClient_GetContainerTradeskill (its refusal said in colour 0xd), then HasSkill: the skill above 0.
        game::ContainerUser u;
        u.class_id = profile_->class_;
        u.race = static_cast<std::int32_t>(profile_->race);
        u.deity = static_cast<std::int32_t>(profile_->deity);
        u.research_classes = research_class_mask();
        game::ContainerSkill s = game::container_tradeskill(bag->bag_type, bag->classes, bag->deity, bag->races, u);
        if (s.message) system(s.message, {}, 0xd);
        return s.skill >= 0 && s.skill != 0xff && skill_value(static_cast<std::uint32_t>(s.skill)) > 0;
    });
}

void GameConnection::open_tradeskill_world() {
    if (!world_container_ || !game::is_combine_container(world_container_->type)) return;
    tradeskill_ = {};
    tradeskill_.open = true;
    tradeskill_.world = true;
    tradeskill_.container_slot = ItemSlot{kInvWorld, -1, -1, -1, 0, 0};
    tradeskill_.object_type = world_container_->type;
    tradeskill_.container_name = world_container_->name;
    tradeskill_.container_icon = world_container_->icon;
    tradeskill_show_favorites();
}

void GameConnection::close_tradeskill() {
    if (tradeskill_.batch) tradeskill_stop_batch("the window closed");
    tradeskill_.open = false;
    tradeskill_.combining = false;
}

void GameConnection::tradeskill_show_favorites() {
    // Send_OP_RecipesFavorite @0x77c6d0 (Show Favorites, and CTradeSkillWnd_SetContainer): the favourites read from the
    // UI ini the first time, the list emptied, {type, container id, the 500 places} sent even when all are 0, "Requesting
    // favorite recipes..." (0x10e), and the search and favourites buttons wait 5 s.
    if (!tradeskill_.open || state_ != ConnectionState::InZone) return;
    if (tradeskill_.batch) tradeskill_stop_batch("the recipe list changed");
    if (!recipe_favorites_loaded_) {
        auto v = ui_ini_get_ ? ui_ini_get_("TradeskillWnd", "Favorites") : std::nullopt;
        recipe_favorites_ = game::parse_recipe_favorites(v.value_or("***"));
        recipe_favorites_loaded_ = true;
    }
    tradeskill_.recipes.clear();
    tradeskill_.selected = 0;
    tradeskill_.details.reset();
    RecipeFavorites f;
    f.object_type = tradeskill_.object_type;
    f.container_id = tradeskill_.container_id;
    f.recipes.assign(recipe_favorites_.begin(), recipe_favorites_.end());
    send_zone(encode(f));
    chat(ChatEvent::Kind::System, game::kRequestingFavorites, 0, 0x10e);
    tradeskill_.request_ready_ms = now_ms() + game::kRecipeRequestWaitMs;
}

void GameConnection::tradeskill_search(std::uint32_t min_trivial, std::uint32_t max_trivial, const std::string& text) {
    // Send_OP_RecipesSearch @0x77bb00: the list emptied, the request sent, "Requesting matching recipes..." (0x10e),
    // and the search and favourites buttons wait 5 s.
    if (!tradeskill_.open || state_ != ConnectionState::InZone) return;
    if (tradeskill_.batch) tradeskill_stop_batch("the recipe list changed");
    tradeskill_.recipes.clear();
    tradeskill_.selected = 0;
    tradeskill_.details.reset();
    RecipesSearch q;
    q.object_type = tradeskill_.object_type;
    q.container_id = tradeskill_.container_id;
    q.min_trivial = min_trivial;
    q.max_trivial = max_trivial;
    q.text = text;
    send_zone(encode(q));
    chat(ChatEvent::Kind::System, game::kRequestingRecipes, 0, 0x10e);
    tradeskill_.request_ready_ms = now_ms() + game::kRecipeRequestWaitMs;
}

void GameConnection::tradeskill_select(std::uint32_t recipe_id) {
    if (!tradeskill_.open || tradeskill_.selected == recipe_id) return;
    if (tradeskill_.batch) tradeskill_stop_batch("another recipe was picked");
    tradeskill_.selected = recipe_id;
    tradeskill_.details.reset();
    if (recipe_id && state_ == ConnectionState::InZone) send_zone(encode_recipe_details_request(recipe_id));
}

void GameConnection::tradeskill_favorite(std::uint32_t recipe_id, bool add) {
    // sub_0077bf60 / sub_0077c010: the selected recipe into the first empty place, or out of every place; saved to the
    // UI ini (CTradeSkillWnd_SaveFavoritesToIni) and said with the recipe's name (3458 / 3459, colour 15).
    const net::RecipeReply* r = nullptr;
    for (const auto& have : tradeskill_.recipes)
        if (have.recipe_id == recipe_id) r = &have;
    if (!r) return;
    bool changed = add ? game::add_recipe_favorite(recipe_favorites_, recipe_id)
                       : game::remove_recipe_favorite(recipe_favorites_, recipe_id);
    if (!changed) return;
    if (ui_ini_put_) ui_ini_put_("TradeskillWnd", "Favorites", game::format_recipe_favorites(recipe_favorites_));
    system(add ? 3458 : 3459, {r->name}, 15);
}

bool GameConnection::tradeskill_combine() {
    // CTradeSkillWnd::HandleCombine @0x77e100: the race and class containers (as CContainerWnd's, colour 15), then
    // nothing on the cursor, item or coin (12024); the container (the world container, else its possessions slot),
    // then the selected recipe: OP_RecipeAutoCombine, the request pending (reason 0x2d), and the combine button
    // waits 2.1 s.
    if (!tradeskill_.open || !tradeskill_.selected || state_ != ConnectionState::InZone) return false;
    if (!combine_gates(tradeskill_.object_type)) return false;
    bool cursor_coin = std::any_of(cursor_coins_.begin(), cursor_coins_.end(), [](std::uint32_t c) { return c != 0; });
    if (inventory_.cursor() || cursor_coin) {
        system(12024, {}, 15);
        return false;
    }
    RecipeAutoCombine c;
    c.object_type = tradeskill_.object_type;
    c.container_id = tradeskill_.container_id;
    c.container_slot = tradeskill_.container_slot;
    c.recipe_id = tradeskill_.selected;
    send_zone(encode(c));
    tradeskill_.combining = true;
    tradeskill_.combine_ready_ms = now_ms() + game::kCombineWaitMs;
    return true;
}

// ---------------------------------------------------------------- Hailstone: Combine N / Combine All

game::RecipeNeeds GameConnection::tradeskill_needs() const {
    const net::RecipeReply* r = tradeskill_.selected_recipe();
    if (!tradeskill_.open || !r || !tradeskill_.details) return {};
    return game::recipe_needs(*tradeskill_.details, r->component_count, inventory_);
}

std::string GameConnection::tradeskill_start_batch(std::uint32_t count) {
    std::string why = batch_refusal();
    if (!why.empty()) {
        chat(ChatEvent::Kind::System, why);
        return why;
    }
    game::CombineBatch b;
    b.recipe_id = tradeskill_.selected;
    b.wanted = count;
    tradeskill_.batch = b;
    log::info("combine batch: recipe {} x {}", b.recipe_id, count ? std::to_string(count) : std::string("all"));
    return {};
}

std::string GameConnection::batch_refusal() const {
    if (!tradeskill_.open || state_ != ConnectionState::InZone) return "The tradeskill window is not open.";
    if (tradeskill_.batch) return "Already combining.";
    if (!tradeskill_.selected_recipe()) return "Pick a recipe first.";
    if (!tradeskill_.details) return "The recipe's components have not come yet.";
    if (tradeskill_.combining || combine_pending_) return "A combine is still waiting for its answer.";
    if (item_job_) return "Already busy with your items.";
    if (trade_ || pstatus_.dead) return "You cannot combine now.";
    bool cursor_coin = std::any_of(cursor_coins_.begin(), cursor_coins_.end(), [](std::uint32_t c) { return c != 0; });
    if (inventory_.cursor() || cursor_coin) return "Put down what you are holding first.";
    if (tradeskill_needs().makeable == 0) return "You do not have the components.";
    if (!game::has_room_for_result(inventory_, [&](int s) { return inv_slot_available(s); }, 0))
        return "Your inventory is full.";
    return {};
}

void GameConnection::tradeskill_stop_batch(const std::string& why) {
    if (!tradeskill_.batch) return;
    std::uint32_t done = tradeskill_.batch->done;
    tradeskill_.batch.reset();
    chat(ChatEvent::Kind::System, std::format("Combining stopped after {} combine{}: {}.", done, done == 1 ? "" : "s", why));
}

bool GameConnection::put_away_cursor() {
    const Item* held = inventory_.cursor();
    if (!held) return true;
    auto to = game::put_away_slot(inventory_, *held, [&](int s) { return inv_slot_available(s); });
    if (!to) return false;
    if (tradeskill_.batch) tradeskill_.batch->result_id = held->id;
    const ItemSlot cursor = ItemSlot::possessions(kSlotCursor);
    if (to->merge) merge_stacks(cursor, to->to);
    else move_item(cursor, to->to, 0);
    return true;
}

void GameConnection::update_tradeskill_batch() {
    if (!tradeskill_.batch) return;
    game::CombineBatch& b = *tradeskill_.batch;
    std::int64_t now = now_ms();
    if (!tradeskill_.open || tradeskill_.selected != b.recipe_id) return tradeskill_stop_batch("the recipe changed");
    if (pstatus_.dead || trade_ || item_job_ || quantity_) return tradeskill_stop_batch("your items are busy");
    if (tradeskill_.combining) {
        if (now - b.sent_ms > game::kBatchAnswerTimeoutMs) tradeskill_stop_batch("the server did not answer");
        return;
    }
    if (now < b.next_ms) return;
    // The last combine's items (the result, or what a failure gives back) come to the cursor: put away one move at
    // a time, and nothing more is made while one has nowhere to go.
    if (inventory_.cursor()) {
        if (now > b.collect_until_ms) return tradeskill_stop_batch("you are holding an item");
        if (!put_away_cursor()) return tradeskill_stop_batch("your inventory is full");
        b.next_ms = now + game::kBatchStepMs;
        b.collect_until_ms = now + game::kBatchCollectMs;
        return;
    }
    if (std::any_of(cursor_coins_.begin(), cursor_coins_.end(), [](std::uint32_t c) { return c != 0; }))
        return tradeskill_stop_batch("you are holding coin");
    if (b.wanted != 0 && b.done >= b.wanted) {
        std::uint32_t done = b.done;
        tradeskill_.batch.reset();
        chat(ChatEvent::Kind::System, std::format("Combining done: {} combine{}.", done, done == 1 ? "" : "s"));
        return;
    }
    if (now <= tradeskill_.combine_ready_ms || combine_pending_ || !tradeskill_.details) return;
    if (tradeskill_needs().makeable == 0) return tradeskill_stop_batch("you are out of components");
    if (!game::has_room_for_result(inventory_, [&](int s) { return inv_slot_available(s); }, b.result_id))
        return tradeskill_stop_batch("your inventory is full");
    if (!tradeskill_combine()) return tradeskill_stop_batch("the combine was refused");
    b.sent_ms = now;
}

// ---------------------------------------------------------------- tasks

void GameConnection::accept_task(std::uint32_t task_id) {
    if (!tasks_.offers || state_ != ConnectionState::InZone) return;
    const game::TaskOffers& o = *tasks_.offers;
    if (o.shared) {
        float multiplier = 1.0f;
        for (const auto& t : o.offers)
            if (t.task_id == task_id) multiplier = t.reward_multiplier;
        send_zone(encode_shared_task_accept(o.giver_id, task_id, multiplier));
    } else {
        send_zone(encode_accept_task(o.type, task_id, o.giver_id));
    }
    tasks_.offers.reset();
}

void GameConnection::decline_tasks() {
    if (!tasks_.offers || state_ != ConnectionState::InZone) return;
    if (!tasks_.offers->shared) send_zone(encode_accept_task(tasks_.offers->type, 0, tasks_.offers->giver_id));
    tasks_.offers.reset();
}

void GameConnection::cancel_task(std::uint32_t type, std::uint32_t sequence) {
    if (state_ != ConnectionState::InZone || !tasks_.find(type, sequence)) return;
    send_zone(encode_cancel_task(sequence, type));
}

void GameConnection::ask_abandon_task(std::uint32_t type, std::uint32_t sequence) {
    // CTaskWnd::WndNotification, TASK_AbandonButton: the selected row's entry (its title, "" without one) in the
    // type's question; ConfirmationDialog(CTaskWnd, 100, ..., 0xc, 10000, task id, 1).
    if (state_ != ConnectionState::InZone) return;
    const game::Task* t = tasks_.find(type, sequence);
    Confirmation c;
    c.id = kConfirmAbandonTask;
    c.text = strings_.format(game::task_abandon_eqstr(type), std::vector<std::string>{t ? t->title : std::string()});
    c.timeout_ms = game::kTaskAbandonTimeoutMs;
    c.data = t ? t->task_id : 0;
    ask(std::move(c));
}

void GameConnection::update_task_compass() {
    // 0x76ff70 (from CTaskWnd::OnProcessFrame once a task changed or a zone was entered): the last mark is
    // remembered and taken off, then the first matching NPC marked; 6035 (colour 0xf) when it is a different task
    // element than the last one.
    task_compass_dirty_ = false;
    std::uint32_t last_id = 0;
    std::int32_t last_slot = -1;
    if (task_compass_) {
        last_id = task_compass_last_id_;
        last_slot = task_compass_last_slot_;
        task_compass_last_id_ = 0;
        task_compass_last_slot_ = -1;
    }
    task_compass_.reset();
    std::vector<game::TaskCompassSpawn> spawns;
    spawns.reserve(world_.spawns.size());
    // GetFirstSpawnInfo's list in spawn id order (the order the spawns arrived in, for the most part).
    std::vector<std::uint32_t> ids;
    ids.reserve(world_.spawns.size());
    for (const auto& [id, sp] : world_.spawns) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    for (std::uint32_t id : ids) {
        const game::Spawn& sp = world_.spawns.at(id);
        spawns.push_back({sp.name, sp.is_npc && !sp.is_corpse, sp.position});
    }
    auto mark = game::task_compass_mark(tasks_, zone_id(), spawns);
    if (!mark) return;
    task_compass_ = mark;
    task_compass_last_id_ = mark->task_id;
    task_compass_last_slot_ = static_cast<std::int32_t>(mark->slot);
    if (last_id == mark->task_id && last_slot == static_cast<std::int32_t>(mark->slot)) return;
    system(6035, {}, 0xf);
}

void GameConnection::request_task_history(std::uint32_t index) {
    if (state_ != ConnectionState::InZone || index >= tasks_.completed.size()) return;
    send_zone(encode_task_history_request(index));
}

std::string GameConnection::task_element_text(const game::TaskElement& e) const {
    return game::element_description(e, strings_, [this](std::uint32_t id) {
        const game::SpellData* s = spell_table_->get(id);
        return s ? s->name : std::string();
    });
}

// ---------------------------------------------------------------- commands

bool GameConnection::progress_command(std::string_view h, const std::string& args) {
    if (h == "cmd_alternateadv") {
        // cmd_alternateadv @0x4f73e0. A server's client
        // may take some forms first (the NMS patch: "/alt toggle <id>").
        if (server_ext().handler_command(*this, h, args)) return true;
        std::string_view rest = args;
        std::string first = game::next_cmd_arg(rest);
        std::string second = game::next_cmd_arg(rest);
        auto starts = [&](std::uint32_t id, std::size_t n) {
            std::string word = strings_.format(id, {});
            if (first.size() < n || word.size() < n) return false;
            for (std::size_t i = 0; i < n; ++i)
                if (std::tolower(static_cast<unsigned char>(first[i])) != std::tolower(static_cast<unsigned char>(word[i])))
                    return false;
            return true;
        };
        if (starts(12947, 2)) { // on
            alt_adv_.percent = 0;
            send_zone(encode(AAAction{kAASetPercent, 0, 0, static_cast<std::uint32_t>(std::atoi(second.c_str()))}));
            return true;
        }
        if (starts(12946, 3)) { // off
            alt_adv_.percent = 0;
            send_zone(encode(AAAction{kAAExpOff, 0, 0, 0}));
            return true;
        }
        if (starts(13271, 4)) { // list: the abilities below id 0xc34f our class may have
            for (const auto& [id, a] : alt_adv_.table) {
                if (id >= 0xc34f || !profile_ || !a.class_allowed(profile_->class_)) continue;
                system(13274, {std::to_string(a.group), aa_name(a)});
                system(13275, {aa_description(a)});
                system(13276, {std::to_string(a.cost)});
            }
            return true;
        }
        if (starts(13272, 3)) { // buy
            aa_train(static_cast<std::uint32_t>(std::atoi(second.c_str())));
            return true;
        }
        if (starts(13273, 3)) { // act
            aa_activate(static_cast<std::uint32_t>(std::atoi(second.c_str())));
            return true;
        }
        system(418);
        return true;
    }
    if (h == "Cmd_tasktimers") {
        send_zone(make_empty(Op::TaskTimers));
        return true;
    }
    // The shared task commands (they need a character, which a connection in a zone has). The name ones take the
    // first word, an empty one too: their usage lines (0x22c2..0x22c4) only show for a null argument, which
    // InterpretCmd never passes.
    std::string_view rest = args;
    if (h == "Cmd_taskhelp") {
        // Cmd_taskhelp @0x4ecda0.
        for (std::uint32_t id : {0x22c6u, 0x22c2u, 0x22c3u, 0x22c4u, 0x22c5u}) system(id);
        return true;
    }
    if (h == "Send_OP_AvaliableTask") { shared_task_add_player(game::next_cmd_arg(rest)); return true; }
    if (h == "Send_OP_SharedTaskRemovePlayer") { shared_task_remove_player(game::next_cmd_arg(rest)); return true; }
    if (h == "Send_OP_SharedTaskMakeLeader") { shared_task_make_leader(game::next_cmd_arg(rest)); return true; }
    if (h == "Send_OP_SharedTaskPlayerList") { send_zone(encode_shared_task_player_list()); return true; }
    if (h == "Send_OP_SharedTaskQuit") { send_zone(encode_shared_task_quit()); return true; }
    if (h == "Cmd_comparetaskraid") {
        // Cmd_comparetaskraid @0x4ed960: the task's members not in the raid, then the raid's members not in the task
        // (__stricmp; the exe's own English lines).
        const auto& m = tasks_.shared_members;
        for (const auto& n : m)
            if (!raid_.is_member(n)) chat(ChatEvent::Kind::System, n + " is in your current task but not in your raid.");
        for (const auto& r : raid_.members)
            if (std::none_of(m.begin(), m.end(), [&](const std::string& n) { return iequals(n, r.name); }))
                chat(ChatEvent::Kind::System, r.name + " is not in your current task but is in your raid.");
        return true;
    }
    return false;
}

void GameConnection::shared_task_add_player(const std::string& name) {
    if (state_ == ConnectionState::InZone) send_zone(encode_shared_task_add_player(name.substr(0, 63)));
}

void GameConnection::shared_task_remove_player(const std::string& name) {
    if (state_ == ConnectionState::InZone) send_zone(encode_shared_task_remove_player(name.substr(0, 63)));
}

void GameConnection::shared_task_make_leader(const std::string& name) {
    if (state_ == ConnectionState::InZone) send_zone(encode_shared_task_make_leader(name.substr(0, 63)));
}

bool GameConnection::shared_task_is_leader() const {
    return !tasks_.shared_leader.empty() && tasks_.shared_leader == character_; // CTaskWnd_FillMemberList: strcmp
}

void GameConnection::shared_task_add_target() {
    auto it = world_.spawns.find(world_.target_id);
    if (it == world_.spawns.end() || it->second.is_npc || it->second.is_corpse) return system(0x230e, {}, 0xf);
    shared_task_add_player(it->second.name);
}

void GameConnection::shared_task_remove_selected(const std::optional<std::string>& member) {
    if (!member) return system(0x230f, {}, 0xf);
    shared_task_remove_player(*member);
}

void GameConnection::shared_task_leader_selected(const std::optional<std::string>& member) {
    if (!member) return system(0x2310, {}, 0xf);
    shared_task_make_leader(*member);
}

namespace {
// 0x47c090: a shared task member by name (__stricmp).
bool in_shared_task(const game::TaskState& t, std::string_view name) {
    return std::any_of(t.shared_members.begin(), t.shared_members.end(),
                       [&](const std::string& n) { return iequals(n, name); });
}
} // namespace

void GameConnection::shared_task_invite_raid() {
    if (tasks_.shared_members.empty()) return;
    for (const auto& r : raid_.members) // CRaid::GetRaidMemberAt 0..71
        if (!in_shared_task(tasks_, r.name)) shared_task_add_player(r.name);
}

void GameConnection::shared_task_invite_expedition() {
    if (tasks_.shared_members.empty()) return;
    for (const auto& m : expedition_.members)
        if (!in_shared_task(tasks_, m.name)) shared_task_add_player(m.name);
}

} // namespace hailstone::net
