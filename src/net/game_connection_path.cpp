// GameConnection: the find path (game/player_path.h). The finds send OP_FindPersonRequest, the server answers with
// its path (EQEmu's pathing, or a straight line without it), and PlayerPath_Update runs every frame on the player's
// position.

#include <algorithm>
#include <cctype>
#include <cmath>

#include "core/game_dir.h"
#include "core/ini_file.h"
#include "core/log.h"
#include "game/follow.h"
#include "game/server_ext.h"
#include "net/game_connection.h"
#include "net/movement_packets.h"

namespace hailstone::net {

namespace {
constexpr std::int64_t kFindReplyWaitMs = 10000;
}

std::uint64_t GameConnection::interaction_context() const {
    // Camping changes which actions are allowed, not this zone/connection/player lifetime. Callers still check
    // the actual state independently; a pending server camp offer must remain tied to the same opaque context.
    const auto context_state = state_ == ConnectionState::Camping ? ConnectionState::InZone : state_;
    return context_for(interaction_, context_state);
}

std::uint64_t GameConnection::context_for(InteractionContext& context, ConnectionState state) const {
    const auto* player = world_.player();
    const auto incarnation = player ? player->incarnation : 0;
    if (!context.id || context.player != incarnation || context.session != zone_session_.get() ||
        context.state != state || context.dead != pstatus_.dead)
        context = {core::next_process_identity(), incarnation, zone_session_.get(), state, pstatus_.dead};
    return context.id;
}

void GameConnection::invalidate_interaction_context() {
    interaction_.id = tracking_interaction_.id = 0;
    large_dialog_.reset();
}

std::optional<GameConnection::ActorRef> GameConnection::captureActorRef(std::uint32_t id) const {
    if (state_ != ConnectionState::InZone || !world_.player()) return std::nullopt;
    return capture_actor_ref(id, interaction_context());
}

std::optional<GameConnection::ActorRef> GameConnection::capture_actor_ref(std::uint32_t id, std::uint64_t context) const {
    const auto actor = world_.spawns.find(id);
    if (actor == world_.spawns.end() || !actor->second.incarnation) return std::nullopt;
    return ActorRef{context, id, actor->second.incarnation};
}

bool GameConnection::matches(const ActorRef& actor) const {
    return state_ == ConnectionState::InZone && world_.player() && matches_actor_ref(actor, interaction_context());
}

bool GameConnection::matches_actor_ref(const ActorRef& actor, std::uint64_t context) const {
    if (!actor.context || actor.context != context) return false;
    const auto current = world_.spawns.find(actor.id);
    return current != world_.spawns.end() && current->second.incarnation == actor.incarnation;
}

void GameConnection::expire_find_requests(std::int64_t now) {
    std::erase_if(find_requests_, [&](const auto& pending) {
        if (!pending.sent || now - pending.sent_at <= kFindReplyWaitMs) return false;
        // Taken as never answered (path_reply): later requests take later replies, as before the wait.
        if (pending.live) *pending.live = false;
        return true;
    });
}

game::ServerPathLease GameConnection::queue_find_request(bool quiet, bool npc_origin, game::ServerPathOrigin origin,
                                                       std::optional<ActorRef> actor, bool silent) {
    const auto now = now_ms();
    expire_find_requests(now);
    const auto owner_current = [&] {
        const auto lifetime = origin.lifetime.lock();
        return !origin.has_lifetime || (lifetime && *lifetime);
    };
    if (!owner_current()) return {};
    if (find_requests_.size() >= 64) return {};
    if (!origin.owner) origin.owner = core::next_process_identity();
    if (!origin.generation) origin.generation = 1;
    const auto policy = npc_origin ? server_ext().map_policy(*this).revision : 0;
    if (!owner_current() || find_requests_.size() >= 64) return {};
    if (!quiet && origin.generation != shown_find_generation_) return {};
    game::ServerPathLease lease{interaction_context(), origin.owner, origin.generation, core::next_process_identity()};
    find_requests_.push_back({quiet, silent, now, npc_origin, false, lease, actor, policy, std::make_shared<bool>(true),
                              origin.lifetime, origin.has_lifetime});
    return lease;
}

void GameConnection::commit_find_request(const game::ServerPathLease& lease) {
    const auto current = std::find_if(find_requests_.begin(), find_requests_.end(), [&](const auto& p) { return p.lease == lease; });
    if (current == find_requests_.end()) return;
    // Only now can it own a reply: outgoing callbacks may consume an older reply or send a nested request before this
    // packet reaches the transport. Requests in flight together each take their own reply, in the order sent.
    current->sent = true;
    current->sent_at = now_ms();
    if (current->quiet) quiet_path_.reset();
}

bool GameConnection::path_request_current(const PendingFindRequest& request) const {
    const auto owner_current = [&] {
        const auto lifetime = request.owner_lifetime.lock();
        return request.live && *request.live && request.lease.context == interaction_context() &&
               (!request.has_owner_lifetime || (lifetime && *lifetime)) &&
               (request.quiet || (request.lease == shown_find_lease_ && request.lease.generation == shown_find_generation_));
    };
    if (!request.lease || !owner_current() || state_ != ConnectionState::InZone ||
        !world_.player() || pstatus_.dead || (request.actor && !matches(*request.actor))) return false;
    if (!request.quiet && request.lease != shown_find_lease_) return false;
    if (request.npc_origin) {
        if (!request.actor || server_ext().map_policy(*this).revision != request.policy_revision ||
            !npc_find_allowed(request.actor->id)) return false;
        return matches(*request.actor) && server_ext().map_policy(*this).revision == request.policy_revision && owner_current();
    }
    return owner_current();
}

void GameConnection::rollback_find_request(const game::ServerPathLease& lease) {
    for (auto& pending : find_requests_) if (pending.lease == lease && pending.live) *pending.live = false;
    std::erase_if(find_requests_, [&](const auto& pending) { return pending.lease == lease; });
}

void GameConnection::cancel_server_path(const game::ServerPathLease& lease) {
    if (!lease) return;
    for (auto& pending : find_requests_) if (pending.lease == lease) {
        pending.discard = true;
        if (pending.live) *pending.live = false;
    }
    if (quiet_path_ && quiet_path_->request.lease == lease) {
        if (quiet_path_->request.live) *quiet_path_->request.live = false;
        quiet_path_.reset();
    }
}

std::optional<std::vector<glm::vec3>> GameConnection::take_quiet_path(const game::ServerPathLease& lease) {
    if (!quiet_path_ || quiet_path_->request.lease != lease) return std::nullopt;
    auto ready = std::move(*quiet_path_); quiet_path_.reset();
    if (!path_request_current(ready.request)) return std::nullopt;
    return std::move(ready.points);
}

bool GameConnection::npc_find_enabled() const {
    return server_ext().map_policy(*this).npc_find_enabled();
}

bool GameConnection::npc_find_visible(std::uint32_t id, bool server_only) const {
    if (server_only && (id > 0xffff || !findable_npcs_.contains(id))) return false;
    const auto found = world_.spawns.find(id);
    if (found == world_.spawns.end() || !found->second.is_npc || found->second.is_corpse) return false;
    const auto incarnation = found->second.incarnation;
    if (!server_ext().map_visible(*this, found->second, 1)) return false;
    const auto checked = world_.spawns.find(id); // (a provider's callback may replace the actor)
    return checked != world_.spawns.end() && checked->second.incarnation == incarnation &&
           checked->second.is_npc && !checked->second.is_corpse && (!server_only || findable_npcs_.contains(id));
}

bool GameConnection::npc_find_allowed(std::uint32_t id) const {
    const auto found = world_.spawns.find(id);
    if (found == world_.spawns.end() || !found->second.is_npc || found->second.is_corpse) return false;
    const auto incarnation = found->second.incarnation;
    const auto before = server_ext().map_policy(*this);
    if (!before.npc_find_enabled()) return false;
    const auto current = world_.spawns.find(id);
    if (current == world_.spawns.end() || current->second.incarnation != incarnation ||
        !npc_find_visible(id, before.npc_find_server_only())) return false;
    const auto after = server_ext().map_policy(*this);
    if (!after.npc_find_enabled()) return false;
    const auto checked = world_.spawns.find(id);
    return checked != world_.spawns.end() && checked->second.incarnation == incarnation &&
           checked->second.is_npc && !checked->second.is_corpse &&
           (!after.npc_find_server_only() || (id <= 0xffff && findable_npcs_.contains(id)));
}

const std::vector<glm::vec3>& GameConnection::find_path() const {
    static const std::vector<glm::vec3> none;
    return path_.active() && find_path_allowed() ? path_.points() : none;
}

bool GameConnection::has_find_path() const {
    if (path_.active() && find_path_allowed()) return true;
    return std::any_of(find_requests_.begin(), find_requests_.end(), [&](const PendingFindRequest& request) {
        return !request.quiet && !request.discard && request.lease == shown_find_lease_;
    });
}

bool GameConnection::find_path_allowed() const {
    if (shown_find_lease_ && shown_find_lease_.context != interaction_context()) return false;
    if (path_.kind() != game::FindKind::Spawn || !npc_find_identity_) return true;
    const auto identity = *npc_find_identity_;
    if (identity.context != interaction_context() ||
        server_ext().map_policy(*this).revision != identity.policy || !npc_find_enabled()) return false;
    const auto* player = world_.player();
    if (state_ != ConnectionState::InZone || !player || player->incarnation != identity.player ||
        path_.target_id() != static_cast<std::int32_t>(identity.id)) return false;
    const auto actor = world_.spawns.find(static_cast<std::uint32_t>(path_.target_id()));
    return actor != world_.spawns.end() && actor->second.incarnation == identity.actor &&
           npc_find_allowed(identity.id) && server_ext().map_policy(*this).revision == identity.policy;
}

void GameConnection::retire_npc_find() {
    path_.stop();
    cancel_server_path(shown_find_lease_);
    shown_find_lease_ = {};
    shown_find_silent_ = false;
    ++shown_find_generation_;
    npc_find_identity_.reset();
}

void GameConnection::play_wave(std::uint32_t id) {
    SoundCue c;
    c.kind = SoundCue::Kind::Wave;
    c.spell = id;
    sound_cues_.push_back(c);
}

void GameConnection::send_find(const game::FindRequest& r, bool found, bool silent) {
    const auto generation = shown_find_generation_;
    if (!find_path_allowed()) {
        if (generation == shown_find_generation_) retire_npc_find();
        return;
    }
    if (generation != shown_find_generation_) return;
    if (r.kind == game::FindKind::Spawn) {
        const auto actor = world_.spawns.find(static_cast<std::uint32_t>(r.id));
        if (actor != world_.spawns.end() && (actor->second.is_npc || actor->second.is_corpse) &&
            !npc_find_allowed(actor->first)) {
            if (generation == shown_find_generation_) retire_npc_find();
            return;
        }
    }
    if (generation != shown_find_generation_) return;
    // A find stopped the path first (PlayerPath_Stop: its abort sound unless this is a silent background path), and
    // reads SpellEffects\Path.ini again after the request. The file is read once here.
    const game::Spawn* me = world_.player();
    if (found) {
        if (!silent) play_wave(game::kPathSoundAbort);
        if (!path_settings_loaded_) {
            path_settings_loaded_ = true;
            IniFile ini;
            if (auto p = game_.find("SpellEffects/Path.ini")) ini.load(*p);
            path_settings_ = game::load_path_settings(&ini);
        }
        path_.set_settings(path_settings_, me ? me->heights.avatar_height : 6.0f);
    }
    if (state_ != ConnectionState::InZone || !me) return;
    FindPersonRequest fr;
    fr.kind = static_cast<std::uint32_t>(r.kind);
    fr.id = r.id;
    fr.item_a = r.item_a;
    fr.item_b = r.item_b;
    fr.at = motion_known_ ? position_ : me->position;
    fr.point = r.point;
    if (!shown_find_owner_) shown_find_owner_ = core::next_process_identity();
    const bool npc = npc_find_identity_.has_value() && r.kind == game::FindKind::Spawn;
    const auto lease = queue_find_request(false, npc,
        {shown_find_owner_, generation, npc ? game::ServerPathOrigin::Purpose::NpcFind : game::ServerPathOrigin::Purpose::ManualNavigation},
        r.kind == game::FindKind::Spawn ? captureActorRef(static_cast<std::uint32_t>(r.id)) : std::nullopt, silent);
    if (!lease) return;
    if (generation != shown_find_generation_) { rollback_find_request(lease); return; }
    shown_find_lease_ = lease;
    shown_find_silent_ = silent;
    const auto current = [this, lease] {
        const auto it = std::find_if(find_requests_.begin(), find_requests_.end(), [&](const auto& p) { return p.lease == lease; });
        if (it == find_requests_.end()) return false;
        const auto request = *it;
        return path_request_current(request);
    };
    try {
        if (!send_zone(encode(fr), true, current)) rollback_find_request(lease);
        else commit_find_request(lease);
    } catch (...) { rollback_find_request(lease); throw; }
}

void GameConnection::find_person_request() {
    // 0x59a9c0 (zoning's position reset): the request again while a path is shown.
    const auto generation = shown_find_generation_;
    if (!find_path_allowed() && generation == shown_find_generation_) retire_npc_find();
    if (generation != shown_find_generation_) return;
    if (path_.active()) send_find(path_.request(eq_time()), false, shown_find_silent_);
}

void GameConnection::find_target(const std::string& name) {
    // A client extension: RoF2's /find (Cmd_find @0x4f4250) is the GM command OP_GMFind. Here it finds a path as
    // 0x59da00 does (GetSpawnByName, then PlayerPath_FindSpawn: the player faces the path), to the named spawn
    // (its name with or without EQEmu's number, any case) or else the target.
    const game::Spawn* me = world_.player();
    if (state_ != ConnectionState::InZone || !me) return;
    auto lower = [](std::string t) {
        for (char& c : t) c = c == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t;
    };
    std::uint32_t id = world_.target_id;
    if (!name.empty()) {
        id = 0;
        std::string want = lower(name);
        for (const auto& [sid, sp] : world_.spawns) {
            std::string n = lower(sp.name), b = n;
            while (!b.empty() && std::isdigit(static_cast<unsigned char>(b.back()))) b.pop_back();
            if (n == want || b == want) {
                id = sid;
                break;
            }
        }
    }
    if (!id || id == world_.player_id) {
        stop_find_path();
        return;
    }
    find_spawn(id);
}

bool GameConnection::find_spawn(std::uint32_t spawn_id, FindPathOptions options) {
    const auto generation = shown_find_generation_;
    auto it = world_.spawns.find(spawn_id);
    if (it == world_.spawns.end()) return false;
    if ((it->second.is_npc || it->second.is_corpse) && !npc_find_allowed(spawn_id)) return false;
    if (generation != shown_find_generation_) return false;
    it = world_.spawns.find(spawn_id);
    if (it == world_.spawns.end()) return false;
    send_find_path(it->second, options);
    return true;
}

void GameConnection::send_find_path(const game::Spawn& to, FindPathOptions options) {
    // PlayerPath_FindGroupMember / FindRaidMember (after their checks; they leave +0x15 at 0) and
    // PlayerPath_FindSpawnByID(id, face): the server's path to a spawn.
    const auto id = to.id; // extension callbacks may replace the actor
    const auto incarnation = to.incarnation;
    const bool npc = to.is_npc || to.is_corpse;
    const auto generation = shown_find_generation_;
    const auto policy = npc ? server_ext().map_policy(*this).revision : 0;
    if (!world_.player() || (npc && !npc_find_allowed(id))) return;
    if (generation != shown_find_generation_) return;
    const auto actor = world_.spawns.find(id);
    if (actor == world_.spawns.end() || actor->second.incarnation != incarnation || !world_.player()) return;
    npc_find_identity_.reset();
    cancel_server_path(shown_find_lease_);
    ++shown_find_generation_;
    if (npc) npc_find_identity_ = NpcFindIdentity{id, incarnation, world_.player()->incarnation, interaction_context(), policy};
    send_find(path_.find_spawn(static_cast<std::uint16_t>(id), options.face, eq_time()), true, options.silent);
}

void GameConnection::find_map_location(const glm::vec3& at) {
    // MapViewMap::HandleRButtonDown @0x6c9680: PlayerPath_FindMapLocation(point, 1). EQEmu answers a request without
    // a spawn with an empty reply (the find fails).
    if (state_ != ConnectionState::InZone || !world_.player()) return;
    // Hailstone: a point no zone holds (not finite, or past any map: the UI hands this through from Lua) is not sent.
    constexpr float kMaxCoord = 1e6f;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(at[i]) || std::fabs(at[i]) > kMaxCoord) return;
    cancel_server_path(shown_find_lease_); ++shown_find_generation_;
    npc_find_identity_.reset();
    send_find(path_.find_map_location(at, true, eq_time()));
}

void GameConnection::find_real_estate_plot(std::uint32_t plot) {
    // CRealEstatePlotSearchWnd's Go with mode 0: PlayerPath_FindRealEstatePlot @0x59c2c0 (/find kind 4, the plot at
    // +0x34). EQEmu decodes only the request's npc id, so a stock server fails it (eqstr 6779).
    if (state_ != ConnectionState::InZone || !world_.player()) return;
    cancel_server_path(shown_find_lease_); ++shown_find_generation_;
    npc_find_identity_.reset();
    send_find(path_.find_plot(static_cast<std::int32_t>(plot), true, eq_time()));
}

void GameConnection::stop_find_path(bool audible) {
    if (audible && !shown_find_silent_) play_wave(game::kPathSoundAbort);
    retire_npc_find();
}

game::ServerPathLease GameConnection::request_quiet_path(std::uint32_t spawn_id, game::ServerPathOrigin origin) {
    const game::Spawn* me = world_.player();
    if (state_ != ConnectionState::InZone || !me || !spawn_id || spawn_id > 0xffff) return {};
    const auto actor = captureActorRef(spawn_id);
    if (!actor) return {};
    const bool npc_find = origin.purpose == game::ServerPathOrigin::Purpose::NpcFind;
    if (npc_find && !npc_find_allowed(spawn_id)) return {};
    // Admission calls server extensions, which may replace either actor or the local player.
    if (!matches(*actor) || pstatus_.dead) return {};
    me = world_.player();
    if (!me) return {};
    FindPersonRequest fr;
    fr.kind = static_cast<std::uint32_t>(game::FindKind::Spawn);
    fr.id = static_cast<std::uint16_t>(spawn_id);
    fr.at = motion_known_ ? position_ : me->position;
    const auto lease = queue_find_request(true, npc_find, origin, actor);
    if (!lease) return {};
    const auto current = [this, lease] {
        const auto it = std::find_if(find_requests_.begin(), find_requests_.end(), [&](const auto& p) { return p.lease == lease; });
        if (it == find_requests_.end()) return false;
        const auto request = *it;
        return path_request_current(request);
    };
    try {
        if (!send_zone(encode(fr), true, current)) { rollback_find_request(lease); return {}; }
        commit_find_request(lease);
    } catch (...) { rollback_find_request(lease); throw; }
    return lease;
}

void GameConnection::path_reply(const AppPacket& p) {
    auto points = decode_find_person_reply(p);
    if (!points) return;
    // Each request gets one reply, and the oldest waiting takes it. EQEmu does not promise the order: Client::SendPath
    // sends an empty reply (no target, no route, a partial one) at once, but builds a full path in a task on the
    // thread pool (SendPathPacket, CullPoints) and sends it when that ends. So a quiet request for a long path then a
    // /find that fails can still swap (docs/unknowns.md, Navigation). A request EQEmu does not answer at all (a find of
    // a trader warps the player instead) would leave every later reply one off, so one waiting longer than
    // kFindReplyWaitMs, our own number, is taken as never answered.
    const std::int64_t now = now_ms();
    expire_find_requests(now);
    const auto pending = std::find_if(find_requests_.begin(), find_requests_.end(), [](const auto& request) { return request.sent; });
    if (pending == find_requests_.end()) return; // no reply can belong to an unsent reservation
    const auto request = *pending; find_requests_.erase(pending);
    if (request.discard || !path_request_current(request)) return;
    if (request.quiet) {
        // The navigation's request (request_quiet_path): no find shown, no sound, no message.
        quiet_path_ = QuietPath{request, std::move(*points)};
        return;
    }
    const auto generation = request.lease.generation;
    if (!find_path_allowed()) {
        if (generation == shown_find_generation_) retire_npc_find();
        return;
    }
    if (generation != shown_find_generation_) return;
    const game::Spawn* me = world_.player();
    auto candidate = path_; // floor/query callbacks must not mutate a callback-created replacement path
    const auto floor = path_floor_;
    auto r = candidate.handle_reply(*points, eq_time(), me && me->gm, floor);
    if (!path_request_current(request) || generation != shown_find_generation_) return;
    path_ = std::move(candidate);
    ++find_replies_; // FindLocationWnd closes only for the admitted current reply
    me = world_.player();
    if (r.failed) {
        if (request.silent) shown_find_silent_ = false; // no silent path remains to suppress a later player stop
        if (r.failed_point) failed_find_ = FailedFind{*r.failed_point, r.failed_kind};
        if (!request.silent) system(game::kStrPathPrevented, {}, 0x111);
        return;
    }
    if (!request.silent) {
        play_wave(game::kPathSoundStart);
        system(game::kStrPathAppears, {}, 0x111);
    }
    if (r.face && me) {
        // PlayerZoneClient_FaceLocation @0x59e7f0: the heading toward the point (__get_bearing).
        glm::vec3 at = motion_known_ ? position_ : me->position;
        landing_.heading = static_cast<float>(game::x87_bearing(at.x, at.y, r.face->x, r.face->y, game::mechanics::Fpu{}));
    }
}

void GameConnection::path_tick() {
    const auto generation = shown_find_generation_;
    if (!find_path_allowed()) {
        if (generation == shown_find_generation_) retire_npc_find();
        return;
    }
    if (generation != shown_find_generation_) return;
    const game::Spawn* me = world_.player();
    if (state_ != ConnectionState::InZone || !me) return;
    auto u = path_.update(motion_known_ ? position_ : me->position, eq_time(), zone_id(), me->gm, path_floor_);
    if (u.arrived) {
        // Background TargetPath keeps the finish emitters but not the sound or chat.
        if (!shown_find_silent_) {
            play_wave(game::kPathSoundComplete);
            system(game::kStrPathFades, {}, 0x111);
        }
        path_finish_ = u.finish_at;
    }
}

} // namespace hailstone::net
