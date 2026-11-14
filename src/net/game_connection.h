#pragma once

// World and zone connection: everything after the login server hands out a
// world ticket. Runs the world handshake and character select, character
// creation, entering the world, the zone-in handshake, and then keeps the
// in-zone state (spawns, chat) and sends the player's movement.
//
// Sequence sources: the original client's connection handling and
// docs/protocol/rof2-packets.md (from the server's world/client.cpp and
// zone/client_packet.cpp).
//
// Not thread-safe; call update() every frame.

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <unordered_map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/character_files.h"
#include "core/process_serial.h"
#include "core/server_profile.h"
#include "game/alt_adv.h"
#include "game/animation_types.h"
#include "game/augments.h"
#include "game/bank.h"
#include "game/placement.h"
#include "game/real_estate.h"
#include "game/barter.h"
#include "game/bazaar.h"
#include "game/chat_text.h"
#include "game/client_options.h"
#include "game/combat.h"
#include "game/doors.h"
#include "game/eq_names.h"
#include "game/eq_strings.h"
#include "game/group.h"
#include "game/pet_buttons.h"
#include "game/social.h"
#include "game/interact.h"
#include "game/item_moves.h"
#include "game/item_sort.h"
#include "game/kept_items.h"
#include "game/bank_snapshot.h"
#include "game/inventory.h"
#include "game/item_links.h"
#include "game/mail.h"
#include "game/tradeskill.h"
#include "game/mercenary.h"
#include "game/mounts.h"
#include "game/player_path.h"
#include "game/missiles.h"
#include "game/server_ext.h"
#include "game/server_ext_chain.h"
#include "game/spells.h"
#include "game/group_role_sets.h"
#include "game/group_search.h"
#include "game/marketplace.h"
#include "game/spell_sets.h"
#include "game/tasks.h"
#include "game/reward_selection.h"
#include "game/achievements.h"
#include "game/task_window.h"
#include "game/text_overlay.h"
#include "game/tracking.h"
#include "game/trade_rules.h"
#include "game/training.h"
#include "game/boats.h"
#include "game/world_state.h"
#include "game/zoning.h"
#include "game/position_tracker.h"
#include "net/login.h"
#include "net/opcodes.h"
#include "net/adventure_packets.h"
#include "net/aggro_packets.h"
#include "net/door_packets.h"
#include "net/dz_packets.h"
#include "net/guild_bank_packets.h"
#include "net/item_packets.h"
#include "net/commerce.h"
#include "net/market_packets.h"
#include "net/settings_packets.h"
#include "net/zone_link_packets.h"
#include "plugin/policy.h"
#include "net/server_files.h"
#include "net/merchant_packets.h"
#include "net/movement_packets.h"
#include "net/quest_npc_packets.h"
#include "net/player_packets.h"
#include "net/tribute_packets.h"
#include "net/marketplace_packets.h"
#include "net/session.h"
#include "net/social_packets.h"
#include "net/spell_packets.h"
#include "net/world_packets.h"
#include "net/zone_packets.h"

namespace hailstone { class GameDir; }
namespace hailstone::game::mechanics { struct GameTables; }
namespace hailstone::game::mechanics { struct LiveState; }

namespace hailstone::game::mechanics { class Character; }
namespace hailstone::game { struct FacialFeatures; }

namespace hailstone::plugin { class Hooks; }

namespace hailstone::net {

struct MemorizeSpell;

class ChatServer;
struct ChannelMessage;
struct Action;
struct ChatServerEvent;
struct DuelRefusal;
struct InspectAnswer;
struct ReadBook;
struct TraderMessage;
struct TraderBuy;

// A yes/no question the original asks with ConfirmationDialog (the id is the
// dialog's: 0x6f set the surname, 0x75 clear it); answer_confirmation() answers.
struct Confirmation {
    std::uint32_t id = 0;
    std::string text;
    std::string value; // what the yes sends (the surname)
    ItemSlot slot;     // the attune question's destination slot
    AppPacket offer;   // a server offer's packet the answer echoes (translocate, resurrection, shared task)
    int timeout_ms = 0;  // the dialog's time limit (answered no when it runs out); 0 none
    bool ok_only = false; // flags 1: an OK button alone (a refusal to read), else Yes / No (0xc)
    std::uint32_t data = 0, data2 = 0; // what the yes acts on (a corpse slot, an ability, an item id, ...)
};
inline constexpr std::uint32_t kConfirmSurname = 0x6f, kConfirmClearSurname = 0x75, kConfirmGuildDelete = 0x6d,
                               kConfirmAttune = 0x7a, kConfirmVeteranReward = 0x7e;
// A transformation mold's combine (CContainerWnd::HandleCombine's dialog 0x86, offer the OP_TradeSkillCombine packet).
inline constexpr std::uint32_t kConfirmMoldCombine = 0x86;
// The mercenary manager's own ConfirmationDialog (dialog 2, the dismissal; the manager's callback, not
// CEverQuest's, so its id is kept apart from the ids above).
inline constexpr std::uint32_t kConfirmMercDismiss = 0x10002;
// The expedition invitation (OP_DzExpeditionInvite): this client's own question; a no sends the declining answer.
inline constexpr std::uint32_t kConfirmDzInvite = 0x10003;
// A translocation offered (OP_Translocate: a Translocate spell, NMS's waypoints) and a resurrection offered
// (OP_RezzRequest): this client's questions; the answer echoes the server's packet with the yes or no.
inline constexpr std::uint32_t kConfirmTranslocate = 0x10004, kConfirmRezz = 0x10005;
// A shared task invitation (OP_SharedTaskInvite, CTaskManager_HandleSharedTaskInvite @0x4803b0: eqstr 8934 with the
// inviter and the task, Yes / No, no time limit, the manager's dialog 0x3444); OP_SharedTaskInviteResponse answers.
inline constexpr std::uint32_t kConfirmSharedTask = 0x10006;
// The task window's abandon question (CTaskWnd's dialog 100; data the task id).
inline constexpr std::uint32_t kConfirmAbandonTask = 0x10009;
// The Options window's item and purchase questions (game/item_options.h), each its window's own dialog: the
// cursor's destroy (CInventoryWnd 100) and its OK-only refusal (0), Loot All (CLootWnd 0x65) and a no-drop item
// (CLootWnd 100, data the corpse slot, data2 auto loot), an AA (CAAWnd 0x64, data the rank, data2 train all), a
// leadership ability (CLeadershipWnd 0x64, data the ability), an adventure merchant's item (CPointMerchantWnd 100)
// and an alternate currency merchant's (CNewPointMerchantWnd 100; data the item id).
inline constexpr std::uint32_t kConfirmDestroy = 0x10101, kConfirmNotice = 0x10102, kConfirmLootAll = 0x10103,
                               kConfirmLootNoDrop = 0x10104, kConfirmAATrain = 0x10105, kConfirmLeadership = 0x10106,
                               kConfirmAdventureBuy = 0x10107, kConfirmPointBuy = 0x10108;
// A raid invitation with Options/RaidInviteConfirm on (Handle_OP_RaidUpdate 0x14 / 0x16: CRaid's own
// ConfirmationDialog); the answer is CRaid::SendInviteResponse's.
inline constexpr std::uint32_t kConfirmRaidInvite = 0x10007;
// /decaycorpse's question (dialog 0x64 on its own handler object, vtable 0x9c69bc; data the corpse, data2 the player).
inline constexpr std::uint32_t kConfirmDecayCorpse = 0x10008;
// /offlinemode's question with Options/OfflineModeConfirm on (CEverQuest's dialog 0x8f, eqstr 13345, 60 s); a yes
// sends opcode 0x53d3 (ConfirmationDialog_HandleResponse @0x52ea0b).
inline constexpr std::uint32_t kConfirmOfflineMode = 0x8f;
// The name changes (ConfirmationDialog_HandleResponse 0x7c, 0x8c, 0x8d; value the new name), a name change window's
// Disable Reminder question (the window's own dialog 100; data the window, GameConnection::NameKind) and the
// server's answer shown as dialog 0xb (OK only).
// The marketplace window's popups (callback wnd +0x220): a purchase's answer (id -1) and its refusals (the zip code,
// a quest-reward gate).
inline constexpr std::uint32_t kConfirmMarketplaceResult = 0x10300, kConfirmMarketplaceNotice = 0x10301;
inline constexpr std::uint32_t kConfirmNameChange = 0x7c, kConfirmPetNameChange = 0x8c, kConfirmMercNameChange = 0x8d,
                               kConfirmNameReminderOff = 0x10200, kConfirmNameResult = 0x10201;

// A book or note the server sent (Handle_OP_ReadBook @0x4c0ea0): type 0 the
// note window, 1 the book window, 2 an item display.
struct BookView {
    std::uint32_t type = 0;
    std::string text;
    ItemSlot slot;
    bool can_cast = false, can_scribe = false;
};

// The large dialog (CLargeDialogWnd, OP_OnLevelMessage): a server popup with
// OK, or Yes and No; answer_large_dialog() answers it.
struct LargeDialog {
    std::string title, text, yes, no;
    bool buttons = false;   // Yes / No instead of OK
    bool sound = false;     // the voice controls (mute, replay, volume)
    std::uint32_t popup_id = 0, negative_id = 0, close_id = 0;
    std::int64_t close_at_ms = 0; // the duration's end on now_ms()'s clock; 0 stays open
};

enum class ConnectionState : std::uint8_t {
    Idle,
    WorldConnecting,  // session to world, then OP_SendLoginInfo
    WorldAuth,        // waiting for the character list
    CharacterSelect,  // characters() valid; create, delete or enter
    Creating,         // name approval / character creation in flight
    EnteringWorld,    // OP_EnterWorld sent, waiting for OP_ZoneServerInfo
    ZoneConnecting,   // session to the zone server, then OP_ZoneEntry
    ZoneEntering,     // receiving profile, spawns and zone data
    InZone,
    Zoning,           // zone change accepted; reconnecting through world
    Camping,
    Disconnected,     // see error()
};

const char* to_string(ConnectionState state);

struct ChatEvent {
    enum class Kind : std::uint8_t { System, Say, Tell, Group, Guild, Raid, Shout, Auction, OOC, Emote, Combat, Spell, Other };
    Kind kind = Kind::System;
    std::string text;
    std::uint32_t rgb = 0; // 0xRRGGBB when the line has its own colour (consider), else 0
    // The original's colour index of the line (dsp_chat's colour: the basic colours below 256, the user colours
    // 256.. that the chat windows' filters route by, eqlib USERCOLOR_*; the server's Chat::*), 0 when not known.
    std::uint32_t color = 0;
    std::string peer; // the player the line is from: a tell's other player (the sender, or whom we told), a channel's
                      // speaker (Hailstone: the modern chat's name links)
};

// Something the client plays a sound for (audio/; the wave each gets is
// game/sound_rules.h). Taken once a frame with take_sound_cues().
struct SoundCue {
    enum class Kind : std::uint8_t {
        Hit,         // OP_Damage / killing blow, melee (ReportSuccessfulHit): target, source, damage
        LevelUp,     // OP_LevelUpdate to a higher level (Handle_OP_LevelUpdate @0x4b9c10)
        Door,        // a switch changed state (EQSwitch::ChangeState): door_type, door_state, position
        BeginCast,   // OP_BeginCast: source = caster, spell
        SpellLanded, // OP_Action type 231: source, target, spell
        Wave,        // EqSoundManager::WavePlay(spell = the wave id, 0): a 2D sound of the sound table
        SpawnWave,   // PlayerClient::PlaySound(spell = the wave id) at spawn `target`
        NewMail,     // MailMgr_OnNewMail @0x469a30: the "YouveGotMail" sound (MailMgr +0xdc), WavePlay(id, 0)
    };
    Kind kind = Kind::Hit;
    std::uint32_t source = 0, target = 0;
    std::int32_t damage = 0;
    std::uint32_t spell = 0;
    std::uint8_t door_type = 0, door_state = 0;
    glm::vec3 position{0.0f};
    bool environment = false; // a Hit from an environmental type (0xf3..0xfd) or a spell: Hit1..4 only, no FlshHit
};

// A spell effect stage to play (SpellEffect_SpawnStageEmitters): the cast
// stage at OP_BeginCast, the impact stage when OP_Action 231 lands a spell.
// Taken once a frame with take_spell_effect_cues().
struct SpellEffectCue {
    int stage = 0;               // 0 cast, 1 projectile, 2 impact
    std::uint32_t caster = 0;    // spawn ids; 0 = none
    std::uint32_t target = 0;
    std::uint32_t spell = 0;
    std::int32_t duration_ms = 0; // the requested duration (the cast time for stage 0)
    std::uint32_t source = 0;     // the impact stage's caster (OP_Action's source)
    double delay = 0.0;           // seconds to hold it back (app/spell_bolts.h: a projectile still on its way)
    // Hailstone: a cast stage the client adds for a spell that landed without an OP_BeginCast (an item, weapon or
    // augment proc; GameConnection::handle_spell). Not a cast the player is making: no held projectile for it.
    bool proc = false;
};

// An effect the server plays by number (a spellsnew.eff index, not a spell): OP_SpellEffect's add and remove
// (game::SpellEffectTimers), OP_AddNimbusEffect and OP_RemoveNimbusEffect. For the nimbus kinds caster = target =
// the spawn and the timings are 0.
struct EffectPacketCue {
    enum class Kind : std::uint8_t { Add, Remove, AddNimbus, RemoveNimbus };
    Kind kind = Kind::Add;
    std::uint32_t effect = 0, caster = 0, target = 0;
    std::uint32_t stage1_ms = 0, stage1_length_ms = 0, stage2_length_ms = 0;
    bool full = false;
};

// The local player's points and progress, from OP_PlayerProfile and the
// update packets (OP_HPUpdate, OP_ManaChange/OP_ManaUpdate,
// OP_EnduranceUpdate, OP_ExpUpdate, OP_LevelUpdate). Maxima are 0 until the
// server sends them (the RoF2 profile carries no maxima).
struct PlayerStatus {
    std::int64_t hp = 0, max_hp = 0;
    std::uint32_t mana = 0, max_mana = 0;
    std::uint32_t endurance = 0, max_endurance = 0;
    std::uint32_t exp = 0;    // into the current level, 0..330
    std::uint32_t aa_exp = 0; // 0..330
    std::uint8_t level = 0;
    std::uint8_t stand_state = 100; // 100 standing, 110 sitting (SpawnAppearance animation)
    std::int8_t owner_status = 0; // CharacterBase status: active 0, unconscious 3, dead 4
    bool dead = false;              // own OP_Death seen; cleared by the next zone-in
};

struct ConnectionOptions {
    // The plugins' extensions (game/server_ext_chain.h, plugin/native_plugins.h): asked first, each member while it
    // says it is active. Null: none.
    std::shared_ptr<game::ServerExtension> plugins;
    // Called with every zone packet before it is handled (tools: packet captures).
    std::function<void(const AppPacket&)> zone_packet_tap;
    // The plugin hook bus (plugin/hooks.h): packets both ways, chat lines, commands, game state, zoning and spawns
    // pass it. Null: no plugins.
    plugin::Hooks* hooks = nullptr;
    // The SOE session options of the world and zone connections (net/session.h; LoginClient::Options::session
    // does the same for login). The defaults are the original's timers; tests shorten them. The optional clock
    // also drives the time-of-day calendar.
    SessionOptions session;
    // The server profile (servers/<id>/, core/server_profile.h) the character's own files go under: the trader's
    // prices (BZR.ini) and chat.ini. Empty: the profile made from the world's name (profile_id_for_world).
    std::string profile_id;
    // The servers/ folder of those profiles; empty: platform::servers_dir() (tests name a scratch folder).
    std::filesystem::path servers_dir;
    // The saved server's folder (servers/<id>/): at the first zone-in the zone-in waits while the server's client
    // files are brought into its assets/ (net/server_files.h). Empty: no download (a test login, tests).
    std::filesystem::path server_files_dir;
    // How much those files may come to (hailstone.ini's assets.server_files_max_*_mb).
    ServerFileSync::Limits server_files_limits;
    // Hailstone: the bank as last seen is read from and saved to the character's bank.dat (game/bank_snapshot.h).
    // Off: kept for the session only (tests, which share character names).
    bool bank_snapshot_file = false;
};

// The modifier keys of a click on an item slot (CXWndManager::GetKeyboardFlags: Shift, Ctrl, Alt).
struct SlotClickKeys {
    bool shift = false, ctrl = false, alt = false;
};

class GameConnection {
public:
    GameConnection(const GameDir& game, ConnectionOptions options = {});
    ~GameConnection();
    // Tests: a zone packet handled as if it had arrived (no session needed).
    void receive_zone_packet_for_test(const AppPacket& p) { handle_zone(p); }
    void receive_world_packet_for_test(const AppPacket& p) { handle_world(p); }
    game::WorldState& world_for_test() { return world_; }
    void set_spell_table_for_test(std::shared_ptr<const game::SpellTable> t) { spell_table_ = std::move(t); }
    void expire_poison_timer_for_test() { if (poison_started_ms_) poison_started_ms_ = 1; } // as if 10 s had passed

    // Connects to the world server named in the ticket.
    void start(const WorldTicket& ticket);
    void update();
    // The ticket start() was given: a camp's end authenticates with world again on it (ProcessGame's
    // WorldAuthenticate with zoning 0 after exit reason 0x1e6).
    const WorldTicket& ticket() const { return ticket_; }

    ConnectionState state() const { return state_; }
    // In a zone, the camp's 30 s included (the player is still there): what the plugins see as in_zone.
    bool in_zone() const { return state_ == ConnectionState::InZone || state_ == ConnectionState::Camping; }
    const std::string& error() const { return error_; }
    // Where a finished camp goes (the camp kind at 0xdd2664: 0 character select, 1 the desktop, 2 the server list), once the
    // zone has dropped the link after OP_Logout (state Disconnected, error "Camped out."); nullopt otherwise.
    std::optional<std::uint32_t> camped_out() const { return camped_out_; }
    // A link death (HandleWorldDisconnect @0x51be10): the world or zone link dropped (the 180 s no-data timeout, the
    // server's disconnect) other than at a camp's end or a zone change. In game (DoMainLoop's call, line 0x4a81) the
    // client goes on as a camp would, with the camp kind at 0xdd2664 as it stands: 0 character select, 1 the desktop, 2 the
    // server list; elsewhere (character select, the zone-in) to the front end. nullopt otherwise.
    struct LinkDeath {
        bool in_game = false;
        std::uint32_t camp_kind = 0;
        // DoMainLoop @0x53ddb8: the drop came less than 15 s (0x3a98 ms) after /offlinemode went ahead; the
        // disconnect screen is OfflineMode.bmp and the client exits after HandleWorldDisconnect's 3 s.
        bool offline_mode = false;
    };
    const std::optional<LinkDeath>& link_death() const { return link_death_; }
    // Testing (app/input_script.h's droplink): the world and zone links go silent (net::Session::simulate_link_loss).
    void simulate_link_loss();
    // Human-readable progress for loading screens.
    const std::string& status() const { return status_; }

    // ---- character select ----
    const std::vector<CharacterSelectEntry>& characters() const { return characters_; }
    // Changes with each character list and each change of a character's classes (classes_mask), unique in the
    // process; 0 before the first list.
    std::uint32_t characters_serial() const { return characters_serial_; }
    // Hailstone's multiclass support: a character's classes as a bit mask (bit c-1 = class c), by name, as the
    // server's extension set them (set_character_classes); 0 = its class only (stock servers).
    std::uint32_t classes_mask(const std::string& name) const;
    void set_character_classes(const std::string& name, std::uint32_t mask);
    void clear_character_classes() {
        if (character_classes_.empty()) return;
        character_classes_.clear();
        characters_serial_ = core::next_process_serial();
    }
    const std::optional<CharCreateData>& char_create_data() const { return create_data_; }
    // The account's expansions (OP_ExpansionInfo's mask; 0xde09d4), none before world sends them.
    std::uint32_t world_expansions() const { return world_expansions_; }
    // OP_RandomNameGenerator: ask world for a name (the creation window's Random Name), and take its answer.
    void request_random_name(std::uint32_t race, std::uint32_t gender);
    std::optional<std::string> take_random_name() { return std::exchange(random_name_, std::nullopt); }
    // Name approval, then creation; the character list refreshes on success
    // and the state returns to CharacterSelect (error() says why on failure).
    void create_character(const std::string& name, const CharCreate& request);
    void delete_character(const std::string& name);
    // return_home: the character select's Return Home (the character goes to its home city). World refuses it, and
    // drops the connection, unless the character's go_home flag in the list allowed it.
    void enter_world(const std::string& name, bool return_home = false);

    // The world and zone sessions' traffic (the performance window); nullopt while there is no session.
    std::optional<SessionStats> world_session_stats() const {
        return world_session_ ? std::optional(world_session_->stats()) : std::nullopt;
    }
    std::optional<SessionStats> zone_session_stats() const {
        return zone_session_ ? std::optional(zone_session_->stats()) : std::nullopt;
    }

    // ---- in zone ----
    const game::WorldState& world() const { return world_; }
    // Eases spawns' display positions toward their latest positions (call every frame).
    // `actor_model`: CreatePlayerActor's height fix-up for the spawns whose actor can be made (WorldState::make_actors).
    void smooth(float dt, const game::WorldState::FloorFn& floor = {},
                const game::WorldState::ActorModelFn& actor_model = {},
                const game::WorldState::RegionFn& region = {},
                const game::WorldState::VehicleFn& vehicle_under = {},
                const game::WorldState::FloorFn& ground_floor = {},
                const game::WorldState::RemoteCollisionFn& collision = {});
    // The vehicles' part of smooth, before the local player moves (WorldState::smooth_vehicles).
    void smooth_vehicles(float dt);
    // The game clock's steps for the other spawns' physics (WorldState::physics_steps; nullopt: one per frame).
    void set_physics_steps(std::optional<std::vector<float>> steps_ms,
                           std::optional<std::uint32_t> completed_time_ms = {}) {
        world_.physics_steps = std::move(steps_ms);
        world_.physics_step_time_ms = completed_time_ms;
    }
    void note_physics_step(std::uint32_t id, std::uint32_t ms) {
        if (auto it = world_.spawns.find(id); it != world_.spawns.end()) it->second.physics_step_ms = ms;
    }
    void set_remote_probe_range(float range) { world_.remote_probe_range = range; }
    // The game clock's steps this frame on now_ms()'s clock (core/step_clock.h, docs/mechanics.md "Frame rate"): the
    // rules that run once a frame and wait for a frame past their interval (memorizing, scribing, the 1 s and 6 s
    // ticks, the socials' lines) run once per step at its time. nullopt: once a frame, now.
    void set_game_steps(std::optional<std::vector<std::int64_t>> steps) { game_steps_ = std::move(steps); }
    const std::optional<std::vector<std::int64_t>>& game_steps() const { return game_steps_; }
    // Hailstone (option ground_npcs): NPCs the server leaves in the air are drawn on the floor (WorldState::ground_npcs).
    void set_ground_npcs(bool on) { world_.ground_npcs = on; }
    // Hailstone (option illusion_mounts): a rider under an illusion still rides (WorldState::illusion_mounts).
    void set_illusion_mounts(bool on) { world_.illusion_mounts = on; }
    // Hailstone (option cast_animation_fallback): the added proc cast stage's animation for a spell without a
    // casting animation (game::spell_cast_animation).
    void set_cast_animation_fallback(bool on) { cast_animation_fallback_ = on; }
    void reground() { world_.reground(); }
    // The player's BodyWet (+0xa2) from this frame's movement, on its spawn.
    void set_player_body_wet(std::uint8_t body_wet) {
        if (auto it = world_.spawns.find(world_.player_id); world_.player_id && it != world_.spawns.end())
            it->second.body_wet = body_wet;
    }
    const std::optional<PlayerProfile>& profile() const { return profile_; }
    // The character we play (as the server names it).
    const std::string& character_name() const { return character_; }
    // The game state last given to the plugin hooks (plugin::kGameState*; 0 none yet).
    int hook_game_state() const { return hook_game_state_; }
    const std::optional<NewZone>& zone_info() const { return new_zone_; }
    // The last server time is the sky clock's reference; the current calendar advances between packets.
    const std::optional<TimeOfDay>& time_of_day() const { return time_; }
    std::optional<TimeOfDay> current_time_of_day() const;
    // The OP_Weather messages since the last call (game::Weather::handle_message takes them).
    std::vector<Weather> take_weather() { return std::exchange(weather_messages_, {}); }
    // The zone the server put us in, once per zone-in: load its geometry. Not while the server's files come.
    std::optional<std::string> take_zone_to_load();
    // The server's client files coming at the first zone-in (ConnectionOptions::server_files_dir), for the loading
    // screen: null when they are not coming now.
    const ServerFileSync* server_files_download() const {
        return server_files_ && !server_files_->finished() ? server_files_.get() : nullptr;
    }
    // True once after the download changed the server's assets/, before the zone-in goes on: the caller rescans
    // the GameDir it gave the connection, then calls reload_game_files().
    bool take_server_files_changed() { return std::exchange(server_files_changed_, false); }
    // Reads the connection's own game files again (the constructor's: strings, spells, race data; the stat tables).
    void reload_game_files();
    std::optional<ChatEvent> poll_chat();
    // A line the UI prints (CEverQuest::dsp_chat): `rgb` 0 for the system colour, or a chat colour index (`color`, as
    // MacroQuest's WriteChatColor gives it).
    void ui_message(std::string text, std::uint32_t rgb = 0, std::uint32_t color = 0) {
        chat(ChatEvent::Kind::System, std::move(text), rgb, color);
    }
    // The sound events since the last call (doors included).
    std::vector<SpellEffectCue> take_spell_effect_cues() { return std::exchange(effect_cues_, {}); }
    // The effect packets since the last call, in order (app/spell_particles.h plays them).
    std::vector<EffectPacketCue> take_effect_packets() { return std::exchange(effect_packets_, {}); }
    std::vector<std::uint32_t> take_blood_spurts() { return std::exchange(blood_spurts_, {}); }
    // The missiles the server launched since the last call (OP_SomeItemPacketMaybe, game/missiles.h), in order.
    std::vector<game::MissilePacket> take_missiles() { return std::exchange(missiles_, {}); }
    // Spell 859 local presentation; no packet is encoded or sent.
    void spell_missile_hit(const game::Missile& missile);
    std::vector<SoundCue> take_sound_cues();
    // What spells landing on the player changed that the movement owns
    // (EQ_Character::HitBySpell, game/hit_landing.h): taken once a frame.
    struct LandingEffects {
        glm::vec3 velocity_change{0.0f}; // units per tick, world axes
        std::optional<float> heading;    // EQ heading units
    };
    LandingEffects take_landing_effects() { return std::exchange(landing_, LandingEffects{}); }

    // The local player's position (world coordinates, game/coords.h) and heading (0..512).
    // Sent as OP_ClientUpdate under the original client's rules.
    void set_player_motion(const glm::vec3& position, float heading, const glm::vec3& velocity, bool moving);
    // Where the local player (and the mount it rides) is drawn this frame: the camera's place between the game
    // clock's steps (game::WorldState::set_local_drawn). Nothing is sent.
    void set_player_drawn(const glm::vec3& position, float heading) { world_.set_local_drawn(position, heading); }
    // What set_player_motion last gave: whether the player is moving, and the heading (0..512).
    bool player_moving() const { return moving_; }
    float player_heading() const { return heading_; }
    // Zone points of the current zone (OP_SendZonepoints): targets by zone_points.number.
    const std::vector<ZonePoint>& zone_points() const { return zone_points_; }
    // The server's zone connections for the map's zone finder (net/zone_link_packets.h): asked at most once a session,
    // and only once a zone has answered OP_HailstoneSettingsRequest (Hailstone's server script is there; a stock
    // server never answers). Cheap to call every frame.
    void request_zone_links();
    const std::optional<ZoneLinks>& zone_links() const { return zone_links_; }
    std::uint32_t zone_links_serial() const { return zone_links_serial_; }
    // The current zone's id and instance (from OP_NewZone; 0 before it arrives).
    std::uint16_t zone_id() const { return new_zone_ ? new_zone_->zone_id : 0; }
    // The spawns ProcessSwitches checks for a moving floor switch's riders (the app's player); null: none.
    void set_door_riders(const game::DoorRiders* riders) { door_riders_ = riders; }
    // The instance the player is in: the player profile's when it is for this zone. Stock EQEmu fills only
    // OP_NewZone's zone id (zone.cpp: newzone_data.zone_instance is never set), so OP_NewZone's instance is 0 in an
    // instance, and a request for the player's own instanced zone (a summon, a #goto) would look like another zone
    // and zone the player (CEverQuest::HandleWorldMessage's DoTeleportB compares the zone dwords).
    std::uint16_t zone_instance() const {
        if (profile_ && new_zone_ && profile_->zone_id == new_zone_->zone_id && profile_->zone_instance != 0)
            return profile_->zone_instance;
        return new_zone_ ? new_zone_->zone_instance : 0;
    }
    // Asks to zone, as the original client does when it walks into a zone line
    // (DoTeleportB → MoveToZone): an OP_ClientUpdate with the current position
    // (the server picks the zone point nearest to it), then OP_ZoneChange with
    // the target zone, instance and target position (world coordinates) and
    // reason 0. Ignored while a request is pending; the answer clears it
    // (success 1 starts zoning), or a 7 s timeout (MoveToZone's limit).
    // Returns false when nothing was sent.
    bool request_zone_change(std::uint16_t zone_id, std::uint16_t instance_id, const glm::vec3& target);
    bool zone_change_pending() const { return zone_change_pending_; }
    // ---- combat (game/combat.h) ----
    const PlayerStatus& player_status() const { return pstatus_; }
    // The player's own HP as Cur_HP / Max_HP give it. EQEmu's SendHPUpdate sends the player's own points without the
    // items' HP (GetHP() - itembonuses.HP, GetMaxHP() - itembonuses.HP), which the original adds back from its own
    // item sums (CharacterBase +0x24, DerivedStats::hp_bonus). The app sets what to add from the player's character
    // each in-zone frame (app/stats_ui.cpp share_own_hp_items): nothing for a value a server extension gives whole
    // (CharacterVitals), and nothing before the first in-zone frame. Everything that shows, reports or tests the
    // player's current HP reads this (docs/mechanics.md), not player_status()'s hp and max_hp, which are the packet's
    // and go below 1 while a geared player still stands. While the player is dead (the own death sets the HP to 0)
    // nothing is added to the current HP.
    struct OwnHp {
        std::int64_t cur = 0, max = 0;
    };
    OwnHp own_hp() const {
        return {pstatus_.dead ? pstatus_.hp : pstatus_.hp + own_hp_items_.cur, pstatus_.max_hp + own_hp_items_.max};
    }
    // Also sets the player's own spawn's HP percent again (SetSpawnHP's, from own_hp()) when the items' HP changed.
    void set_own_hp_items(std::int64_t cur, std::int64_t max);
    std::uint32_t target() const { return world_.target_id; }
    // Sets the target and tells the server (OP_TargetMouse, as the original
    // does on every target change); 0 clears it.
    void set_target(std::uint32_t spawn_id);
    // LocalPlayer_TargetSelfOrPet (F1 = 0, F2..F6 = 1..5, the group window's
    // rows): targets group member n, the n-th occupied slot, and selects it
    // (game/group_rules.h). The selection /disband reads is
    // group_member_targeted(): that ordinal, or, once the target changes, the
    // new target's slot index (CTargetWnd::OnProcessFrame), -1 none.
    void target_group_member(std::int32_t n, bool target_of_target = false);
    std::int32_t group_member_targeted() const { return group_member_targeted_; }
    // The target's target (OP_TargetHoTT; PlayerZoneClient::TargetOfTarget), 0 none.
    std::uint32_t target_of_target() const { return target_of_target_; }
    // The extended target list (OP_XTargetResponse) and ExtendedTargetList_TargetSlot (the TARGET_XTARGET_n keys,
    // a click on a slot): targets the slot's spawn.
    const game::XTargetState& xtargets() const { return xtargets_; }
    void target_xtarget(int slot);
    // Send_OP_XTargetRequest @0x5492a0: a slot whose type or name differs takes them (game::xtarget_set_slot) and
    // OP_XTargetRequest {1, slot, type, name} goes to the zone.
    void xtarget_request(int slot, std::uint32_t type, const std::string& name);
    // Send_OP_XTargetAutoAddHaters @0x548850: the list's auto add flag, sent when it changes.
    void set_xtarget_auto_haters(bool on);
    // 0x548960: the slot the window's menu acts on (kept only when in range).
    void select_xtarget(int slot);
    // ExtendedTargetList_TargetNextSlot @0x5488f0 (CYCLE_XTARGET): from the last cycled slot, the next one holding
    // a spawn (wrapping) becomes the target.
    void target_next_xtarget();
    // A slot the server extension leaves out (server_ext().hide_xtarget_slot; NMS: the player's own pets the server
    // puts in the last slots): no row, and the targeting above and add_xtarget pass it by as if it held no spawn.
    bool xtarget_slot_hidden(int slot) const;
    // __ExecuteCmd's ADD_XTARGET / REMOVE_XTARGET / REMOVE_XTARGET_BY_SLOT (cases 0x89, 0x8a, 0x8c): the target
    // into the first auto hater slot (2 a player, 3 an NPC), the last specific slot back to auto hater, every
    // specific slot holding the target back to auto hater.
    void add_xtarget();
    void remove_last_xtarget();
    void remove_target_xtargets();
    bool auto_attack() const { return auto_attack_; }
    // The attack key / button (__ExecuteCmd AUTOPRIM): toggles with the
    // original's checks and messages. /attack [on|off] goes through send_command.
    // True when it turned auto attack on and the player should turn to the target (face_on_attack).
    bool toggle_auto_attack();
    void set_auto_attack(bool on);
    // Hailstone (option attack_until_xtarget_clear, off by default; docs/mechanics.md): when the target dies, auto
    // attack stays on while the extended target list still holds a living spawn (slots the server extension hides
    // left out), and goes off once it holds none, unless a new target was taken first. The original turns it off
    // with the target's death.
    void set_attack_until_xtarget_clear(bool on) { attack_until_xtarget_clear_ = on; }
    bool attack_until_xtarget_clear() const { return attack_until_xtarget_clear_; }
    // A living spawn other than `except` in a slot of the extended target list that is not hidden.
    bool xtarget_holds_spawn(std::uint32_t except = 0) const;
    // Hailstone (option auto_target_xtarget, on by default; docs/mechanics.md): with no target, an NPC on the
    // extended target list (slots the server extension hides left out) that attacks the player (an OP_Damage on
    // the player from it, a miss included) becomes the target; so does such an NPC still on the list when the
    // target dies. Clearing the target by hand holds until the next hit. The original never targets by itself.
    void set_auto_target_xtarget(bool on) { auto_target_xtarget_ = on; }
    bool auto_target_xtarget() const { return auto_target_xtarget_; }
    // Hailstone (option face_on_attack, on by default; docs/mechanics.md): the attack key and buttons
    // (toggle_auto_attack) turn the player to the target when they turn auto attack on, as the original's AUTOPRIM
    // does. Off, the heading is left alone, which the original cannot do. /attack never turns the player.
    void set_face_on_attack(bool on) { face_on_attack_ = on; }
    bool face_on_attack() const { return face_on_attack_; }
    // __Autofire (0xe635ab): ranged auto attack. The server shoots while it is on (OP_AutoFire); the client only
    // turns it on and off.
    bool auto_fire() const { return auto_fire_; }
    // SetAutoRangeAttack @0x520b40: the flag (under the attack lock both attack flags go off), "Auto fire on." /
    // "off." (278 / 279, colour 0xd) when it changed; OP_AutoFire goes out when it differs from the last sent value
    // (DoPassageOfTime).
    void set_auto_fire(bool on);
    // cmd_autofire @0x4fb9b0 (/autofire): needs a target (12697, colour 0x12f); on turns it off, else auto attack
    // off and auto fire on. (Its held-model swap, the range item in hand 8, is the renderer's: not modelled.)
    void autofire_command();
    // PlayerZoneClient::AllowedToAttack(target, spell) for the local player
    // (game/attack_check.h): spell -1 is melee (CanIHit), else a cast.
    bool allowed_to_attack(std::uint32_t target, std::int32_t spell) const;
    // The server's PvP rule set (OP_LogServer +4, global 0xdcee70).
    std::int32_t pvp_rules() const { return pvp_rules_; }
    // The account's membership (OP_SendMembership; FreeToPlayClient::Instance()) and the feature table
    // (OP_SendMembershipDetails; GameFeatureRestrictions). Empty until world sends them.
    const std::optional<Membership>& membership() const { return membership_; }
    const std::optional<MembershipDetails>& membership_details() const { return membership_details_; }
    // The attack lock (0xddf70c): set by message 0x57da carrying the player's
    // name (auto attack and auto fire stop), cleared by 0x4d09 and by zoning;
    // DoAttack and CastSpell refuse while it is set. EQEmu sends neither.
    bool attack_locked() const { return attack_locked_; }
    // A one-shot animation on a spawn, as an OP_Animation for it would play it at speed 10 (a missile's
    // launch: Send_OP_Animation_0058f540 -> PlayAttackAnimation on its source).
    void play_animation(std::uint32_t spawn_id, std::uint8_t action);
    void play_animation(std::uint32_t spawn_id, AnimationType action);
    // OP_ControlBoat {spawn id, take_control}: take_control 1
    // makes that spawn pinstControlledPlayer ("You have the helm, captain.", 1407); release_boat() lets go
    // ({id, 0}). 0: the player itself is controlled.
    std::uint32_t controlled_boat() const { return controlled_boat_; }
    void release_boat();
    // One frame at the helm of the controlled boat (game/boats.h steer_boat): the keys sail it, `clear` keeps it in
    // the water and off docks and other boats (game::helm_resolve: a step it refuses stops the boat), and its
    // position goes out as OP_ClientUpdate under the boat's id (EQEmu's Handle_OP_ClientUpdate moves a
    // controllable boat and passes it on).
    void steer_boat(bool forward, bool back, bool left, bool right, float dt, const game::HelmClearFn& clear);
    // PlayerClient +0x150 for the local player: the vehicle it stands on (the app's floor test), 0 none. While it
    // is set the position goes out in the vehicle's frame with its id (PackPlayerPositionUpdateOnVehicle @0x4fa650,
    // DoMainLoop @0x53e0de).
    // ProcessControls lets a controlled small boat go (OP_ControlBoat {id, 0}) once the player no longer stands on
    // it.
    void set_standing_on(std::uint32_t vehicle_id);
    std::uint32_t standing_on() const { return standing_on_; }
    // CEverQuest::RightClickedOnPlayer's boat case (an NPC whose vf58, PlayerClient_IsSmallBoatRace, holds): the
    // boat you control is let go (release_boat), the one you stand on is asked for (OP_ControlBoat {id, 1}); any
    // other does nothing. Returns whether `id` is such a boat (no consider follows either way).
    bool right_click_boat(std::uint32_t id);
    // The melee report modes per category (CChatWindowManager +0x224..+0x238;
    // game::report_hit_line): 0 full sentence (the default), 1 short, 2 number.
    const game::HitModes& hit_modes() const { return hit_modes_; }
    void set_hit_modes(const game::HitModes& modes) { hit_modes_ = modes; }
    // OP_Consider for the current target; the answer prints the con line.
    void consider();
    // unk_SetStandState @0x5a0bc0 (PlayerZoneClient::ChangePosition)
    // for the player: 100 stand, 105
    // kneel (looting, bind wound), 110 sit, 111 crouch, 115 lie down (feign
    // death). Nothing for the same state or for sitting while mounted;
    // otherwise auto attack goes off, and crouch to stand or sit needs
    // `room_to_stand` (CanIFitHere and the room above; without it the player
    // stays crouched). An accepted change sends OP_SpawnAppearance {me, 14,
    // state} and sets the local state. Returns whether it changed.
    bool set_stand_state(std::uint32_t state, bool room_to_stand = true);
    // The sit / stand toggle (CEverQuest::Sit @0x5261d0): mesmerized (SPA 31)
    // prints eqstr 12368; standing or crouching sits unless frozen (102),
    // anything else stands.
    void sit_toggle();
    // set_stand_state(110 / 100).
    void set_sitting(bool sitting);
    bool sitting() const { return pstatus_.stand_state == 110; }
    // ---- the spell book (CSpellBookWnd's open/close rules, game/spell_book.h) ----
    // The UI calls open_spell_book() when the book is about to show (false:
    // the original refuses and the window must stay closed) and
    // close_spell_book() when it hides. The connection closes it itself on a
    // stand change away from sitting, horizontal movement and zoning; the UI
    // follows spell_book_open().
    bool open_spell_book();
    void close_spell_book();
    bool spell_book_open() const { return spell_book_open_; }
    // A quick right click on a book slot (CSpellBookWnd::WndNotification XWM_RCLICK -> Send_OP_SwapSpell @0x75ceb0),
    // sitting or with the controlled spawn not moving: the first picks the slot up (SelectedBookIndex, 12046; a
    // Deletable spell also prints 4027), the same slot again puts it back, another slot sends OP_SwapSpell {picked,
    // slot}. The server's echo swaps the two slots (Handle_OP_SwapSpell @0x75d380, 1388) and drops the pick.
    void spell_book_swap_click(int book_slot);
    // CSpellBookWnd +0x230 SelectedBookIndex: the book slot picked up to move, -1 none (cleared when the book opens).
    int spell_book_swap_slot() const { return book_swap_slot_; }
    // The controlled spawn's SpeedRun (+0x7c) this frame (AboutToShow refuses above 0).
    void set_speed_run(float speed_run) { speed_run_ = speed_run; }
    // The hover respawn window (Character:RespawnFromHover), if the server sent one. CRespawnWnd's Select
    // is enabled once the window's delay has passed (respawn_ready) and sends OP_RespawnWindow {u32 option};
    // the list and button are then disabled until OP_ZonePlayerToBind closes the window.
    const std::optional<RespawnWindow>& respawn_window() const { return respawn_; }
    bool respawn_ready() const;
    bool respawn_chosen() const { return respawn_chosen_; }
    // The respawn timer's milliseconds left (the window's time less the time since it came), and its whole span.
    // How long the window has been up (ms): the respawn gauge's and Select's clock (the original counts from the
    // corpse's time, PlayerZoneClient +0x2bc; taken as the window's arrival here).
    std::int64_t respawn_age_ms() const { return respawn_ ? now_ms() - respawn_shown_ms_ : 0; }
    std::int64_t respawn_ms_left() const {
        if (!respawn_) return 0;
        return std::max<std::int64_t>(0, static_cast<std::int64_t>(respawn_->time_remaining_ms) - (now_ms() - respawn_shown_ms_));
    }
    void choose_respawn(std::uint32_t option_id);

    // ---- personal tribute (net/game_connection_tribute.cpp, net/tribute_packets.h): the tributes the server offers
    // (asked for while zoning in), our chosen benefits, favour, the active timer, and the tribute master's window
    // (a right click on a class-63 NPC: OP_OpenTributeMaster) with its donations.
    struct Tribute {
        std::map<std::uint32_t, TributeAbility> abilities; // by tribute id
        TributeSettings settings;                           // the server's (OP_TributeUpdate)
        std::int32_t points = 0, career = 0;
        std::uint32_t timer_ms = 0;      // OP_TributeTimer's value
        std::int64_t timer_at = 0;       // when it came (now_ms)
        std::map<std::uint32_t, std::string> descriptions; // OP_SelectTribute's answers
        std::uint32_t master_id = 0;     // the tribute master whose window is open
        std::uint64_t master_serial = 0;
        bool benefit_open = false;       // TributeBenefitWnd
        std::int32_t last_donation = 0;  // the favour the last donation gave
        std::uint32_t donation_answers = 0; // OP_TributeItem answers so far (the Advanced Inventory's job waits on them)
        // The active cost: the chosen tiers' costs.
        std::uint32_t active_cost() const;
        // The active time left in ms (counting down while active).
        std::int64_t time_left(std::int64_t now) const;
    };
    const Tribute& tribute() const { return tribute_; }
    bool open_tribute_master(std::uint32_t npc_id);
    void close_tribute_master() { tribute_.master_id = 0; }
    void set_tribute_benefit_open(bool open) { tribute_.benefit_open = open; }
    void tribute_describe(std::uint32_t tribute_id);
    // Sends new benefits (OP_TributeUpdate); the server checks the tiers.
    void tribute_change(const TributeSettings& settings);
    void tribute_toggle(bool on);
    bool tribute_donate_cursor();
    bool tribute_donate_platinum(std::uint32_t platinum);

    // ---- expeditions (net/game_connection_dz.cpp, net/dz_packets.h): the server's expedition info, members and
    // lockouts (the DynamicZoneWnd), an invitation (a confirmation), the DZ switch list's choices, and the leader's
    // commands. EQEmu answers the commands itself (it checks the leader, the members and the lockouts).
    struct Expedition {
        DzInfo info;                     // assigned false: none
        std::vector<DzMember> members;
        std::vector<DzLockout> lockouts;
        std::int64_t lockouts_at = 0;    // when they came (now_ms); seconds count down from there
        std::optional<DzInvite> invite;  // awaiting an answer
        std::vector<DzChoice> choices;   // OP_DzChooseZone: the switch list is shown while it has entries
        std::vector<DzCompassPoint> compass; // OP_DzCompass: this zone's expedition switches (and a quest's point)
        bool window_open = false;
    };
    const Expedition& expedition() const { return expedition_; }
    void set_expedition_window_open(bool open) { expedition_.window_open = open; }
    void dz_add_player(const std::string& name);
    void dz_remove_player(const std::string& name);
    void dz_make_leader(const std::string& name);
    void dz_swap_player(const std::string& add, const std::string& remove);
    void dz_quit();
    void dz_list_timers();
    void dz_player_list();
    void dz_choose_zone(int index); // -1 closes the switch list without choosing
    bool dz_is_leader() const;
    // The expedition commands (/dzaddplayer .. /dzquit, /dzhelp, /compareexpedraid): false for another handler.
    bool dz_command(std::string_view handler, const std::string& args);
    // The points the compass marks, in zone-file coordinates: the expedition switches (OP_DzCompass) and the current
    // adventure's way in, when it is in this zone (OP_AdventureData's compass), the /find path's end and the task NPC.
    std::vector<glm::vec3> compass_points() const;
    // The find path (game/player_path.h: PlayerPath, the server's path from OP_FindPersonReply in world
    // coordinates, the destination first and the player's end last; empty while no path is shown). The map draws it,
    // the compass marks its end, the world draws its line (app/path_effects.h).
    const std::vector<glm::vec3>& find_path() const;
    const game::PlayerPath& player_path() const { return path_; }
    // The floor probe the path's points are put on (the zone's collision; null: they stay where the server put them).
    void set_path_floor(game::PlayerPath::FloorFn floor) { path_floor_ = std::move(floor); }
    // /find [name] (a client extension, see game_connection_path.cpp): the path to the named spawn
    // (PlayerManagerClient::GetSpawnByName, then PlayerPath_FindSpawn, as 0x59da00), or to the target; nothing
    // to find ends the path.
    void find_target(const std::string& name = {});
    // The path to a spawn by id, the player facing it when it comes (PlayerPath_FindSpawnByID(id, 1)); for the Find
    // window (app/lua_eq_find.cpp). False when there is no such spawn.
    bool find_spawn(std::uint32_t spawn_id);
    // PlayerPath_FindMapLocation (MapViewMap::HandleRButtonDown): the path to a point of the map, world coordinates
    // at the player's height.
    void find_map_location(const glm::vec3& at);
    // PlayerPath_FindRealEstatePlot (the plot search window's Go with mode 0): the path to a plot.
    void find_real_estate_plot(std::uint32_t plot);
    // PlayerPath_Stop (the find key / End Find while a path is shown): the path ends with its abort sound.
    void stop_find_path();
    // The navigation's server path (app/navigator.h, a Hailstone addition): the same OP_FindPersonRequest as a find
    // to a spawn, but its reply goes to take_quiet_path() alone: no path drawn, no sound, no chat line, and a shown
    // find path is left as it is.
    void request_quiet_path(std::uint32_t spawn_id);
    // The reply once it has come (the destination first, as sent; empty: the server has no path), then nullopt.
    std::optional<std::vector<glm::vec3>> take_quiet_path() { return std::exchange(quiet_path_, std::nullopt); }
    // 0x59a9c0 (zoning's position reset): the request again while a path is shown.
    void find_person_request();
    // A failed find the map shows (0x6c92e0: a one-point reply; the map window opens), taken by the map.
    struct FailedFind {
        glm::vec3 point{0.0f};
        game::FindKind kind = game::FindKind::Spawn;
    };
    std::optional<FailedFind> take_failed_find() { return std::exchange(failed_find_, std::nullopt); }
    // Counts the shown find replies (the original's FindLocationWnd closed on each: Show(false) before
    // HandleFindPersonReply).
    std::uint32_t find_replies() const { return find_replies_; }
    // The zone's findable NPCs (OP_SendFindableNPCs: the server's `findable` flag), by spawn id; cleared on zoning.
    // The original's find window listed them (Handle_OP_SendFindableNPCs); the Find window here does too
    // (game/useful_npcs.h).
    const std::unordered_map<std::uint32_t, FindableNpc>& findable_npcs() const { return findable_npcs_; }
    // The zone's quest NPCs from Hailstone's server script (net/quest_npc_packets.h), by their names as shown
    // (game::display_name, ASCII lower case): kQuestNpcItems / kQuestNpcTalks. Asked once a zone, the first time the
    // Find window wants them (request_quest_npcs); empty without the script. Cleared on zoning.
    void request_quest_npcs();
    const std::unordered_map<std::string, std::uint8_t>& quest_npcs() const { return quest_npcs_; }
    // The path's finish emitters, taken by the world's effects (PlayerPath_OnDestinationReached).
    std::optional<glm::vec3> take_path_finish() { return std::exchange(path_finish_, std::nullopt); }

    // ---- bug reports (/bugreport: BugReportWindow, OP_Bug) and veteran rewards (/claim: ClaimWnd, OP_VetClaim*)
    bool bug_report_open() const { return bug_report_open_; }
    void set_bug_report_open(bool open) { bug_report_open_ = open; }
    // Sends a report from here: the position, heading and (with kBugTargetInfo) the target are filled in.
    void send_bug_report(std::uint32_t category, std::uint32_t flags, const std::string& text);
    // The feedback window (/feedback: FeedbackWindow, OP_Feedback): the text is sent with the position and heading.
    bool feedback_open() const { return feedback_open_; }
    void set_feedback_open(bool open) { feedback_open_ = open; }
    void send_feedback(const std::string& text);
    struct Claims {
        bool open = false;
        std::vector<VetReward> rewards;
    };
    const Claims& claims() const { return claims_; }
    void set_claims_open(bool open);
    void claims_refresh();
    void claim_reward(std::uint32_t claim_id);

    // ---- looking for a guild (OP_LFGuild, LFGuildWnd): our player entry and the guild's as the server has them
    // (sent at zone-in), and the last searches' matches.
    struct LFGuild {
        bool open = false;
        LFGuildPlayer player;
        LFGuildGuild guild;
        std::vector<LFGuildPlayerMatch> players;
        std::vector<LFGuildGuildMatch> guilds;
    };
    const LFGuild& lfguild() const { return lfguild_; }
    void set_lfguild_open(bool open) { lfguild_.open = open; }
    void lfguild_post_player(const LFGuildPlayer& v);
    void lfguild_post_guild(const LFGuildGuild& v);
    void lfguild_search_players(std::uint32_t from_level, std::uint32_t to_level, std::uint32_t min_aa,
                                std::uint32_t time_zone, std::uint32_t classes);
    void lfguild_search_guilds(std::uint32_t level, std::uint32_t aa, std::uint32_t time_zone, std::uint32_t class_);

    // ---- PvP (OP_PVPStats at zone-in; PvPStatsWnd, PvpLeaderboardWnd) and the crystal counts (OP_CrystalCountUpdate:
    // the Norrath's Keepers and Dark Reign merchants' currencies)
    struct PvP {
        std::optional<PvPStats> stats;
        std::optional<PvPLeaderboard> leaderboard;
        bool stats_open = false, leaderboard_open = false;
    };
    const PvP& pvp() const { return pvp_; }
    void set_pvp_stats_open(bool open) { pvp_.stats_open = open; }
    void set_pvp_leaderboard_open(bool open);
    void pvp_leaderboard_request(std::uint32_t sort);
    const std::optional<CrystalCounts>& crystals() const { return crystals_; }

    // ---- the alarm (/alarm, CMD_TOGGLE_ALARM: AlarmWnd): a warning once the time set passes (client-side).
    bool alarm_open() const { return alarm_open_; }
    void set_alarm_open(bool open) { alarm_open_ = open; }
    void set_alarm(std::int64_t ms); // 0 clears it
    std::int64_t alarm_due_ms() const { return alarm_due_ms_; }

    // ---- the name change windows (net/game_connection_names.cpp): NameChangeWnd, NameChangePetWnd and
    // NameChangeMercWnd, opened by the server's invitations or /changename, /changepetname, /changemercname.
    enum NameKind : int { kNameCharacter, kNamePet, kNameMerc, kNameKinds };
    struct NameChangeWindow {
        bool open = false;
        bool remind = false;           // Open(1): the server's reminder (Remind Me Later, Disable Reminder shown)
        bool pending = false;          // +0x22d: a request is out ("Request Pending", Submit Name disabled)
        bool reminder_disabled = false; // +0x22c: the character ini's Preferences/Disable*NameChangeReminder
    };
    const NameChangeWindow& name_change(int kind) const { return name_change_[static_cast<std::size_t>(kind)]; }
    void set_name_change_open(int kind, bool open) { name_change_[static_cast<std::size_t>(kind)].open = open; }
    // Cmd_changename / Cmd_changepetname / Cmd_changemercname with the name typed (the window's Submit Name).
    void name_change_command(int kind, const std::string& args);
    // The Disable Reminder button: its question (the window's own ConfirmationDialog 100).
    void ask_disable_name_reminder(int kind);
    // The character ini (<name>_<server>.ini, the app's hotbuttons.ini) for what the windows keep there:
    // CharacterIni_GetInt and CharacterIni_WriteString.
    void set_character_ini(std::function<int(std::string_view section, std::string_view key, int fallback)> get,
                           std::function<void(std::string_view section, std::string_view key, const std::string& value)> put) {
        char_ini_get_ = std::move(get);
        char_ini_put_ = std::move(put);
    }
    // The character's UI ini (UI_<name>_<server>.ini; the chat windows' [ChatManager] lives there): ReadUIINIString
    // and CEverQuest::WriteIniString.
    void set_ui_ini(std::function<std::optional<std::string>(std::string_view section, std::string_view key)> get,
                    std::function<void(std::string_view section, std::string_view key, const std::string& value)> put) {
        ui_ini_get_ = std::move(get);
        ui_ini_put_ = std::move(put);
    }
    // CChatWindowManager::GetAllVisibleText @0x650c60 for /report: each chat window's visible text, each followed by
    // "\n===================================\n".
    void set_visible_chat_text(std::function<std::string()> fn) { visible_chat_text_ = std::move(fn); }
    // The windows' constructors: the reminder flags read from the character ini (the character entered the world).
    void load_name_change_flags();

    // The context tips the connection's events ask for (CTipWnd::Activate(pinstCTipWnd_2, id, 0) in the original's
    // handlers: sitting 0x16, level 10 0x18, consider 0x1d, the faction messages 0x1e); the app activates them.
    std::vector<int> take_context_tips() { return std::exchange(context_tips_, {}); }
    // The addresses the server sent to open (OP_Weblink, oldest first), taken once a frame.
    std::vector<std::string> take_web_links() { return std::exchange(web_links_, {}); }
    // A window's OK-only ConfirmationDialog (the journal category window's 3182): nothing follows the OK.
    void notice(std::string text) {
        Confirmation c;
        c.id = kConfirmNotice;
        c.text = std::move(text);
        c.ok_only = true;
        ask(std::move(c));
    }
    // The NPC lines OP_SpecialMesg asks the conversation journal to keep (DisplaySpecialMesg's journal hook): the
    // speaker, the zone, where the packet says, and the line as shown. The journal window takes them each frame.
    struct JournalLine {
        std::string npc;
        std::int32_t zone = 0;
        float x = 0, y = 0, z = 0;
        std::string text;
    };
    std::vector<JournalLine> take_journal_lines() { return std::exchange(journal_lines_, {}); }

    // ---- stuns (OP_Stun: u32 duration ms first): while stunned the character neither moves, casts nor uses skills.
    // The character's stun state (CharacterZoneClient +0x100 = 1). StunMe sets it with the timer (the spawn's +0x43c);
    // DoPassageOfTime ends it once the timer passes, unless the player is mesmerized (EndStun then waits for the mez).
    bool stunned() const { return stun_state_; }
    // How many times the local player was made to stop. SpeedRun (+0x7c): a stun starting (CharacterZoneClient::StunMe
    // @0x4432c0, which stops the mount too) or sitting, looting or lying down (PlayerZoneClient::ChangePosition
    // @0x5a0bc0's StopMoving); the turn rate (+0x8c): ChangePosition's alone. Movement clears its body's speeds when
    // these change; it stops asking for speed while it cannot move, so the speed it had would carry it on.
    std::uint32_t speed_stops() const { return speed_stops_; }
    std::uint32_t turn_stops() const { return turn_stops_; }
    // How many stuns leaned the player: StunMe's last argument sets the turn rate (+0x8c) to 32.
    std::uint32_t stun_leans() const { return stun_leans_; }

    // ---- food and drink (OP_Consume): a right click eats through OP_ItemVerifyReply (handle_item_verify_reply);
    // below kAutoEatLevel (of EQEmu's 6000) the first food or drink in the general slots and their bags is eaten
    // automatically.
    static constexpr std::uint32_t kAutoEatLevel = 3000; // this client's threshold (the original's is not decoded)
    bool consume(const ItemSlot& slot, bool automatic = false);

    // ---- dyes (OP_Dye): sends the armour tints; the server answers with the wear changes.
    void dye_armor(const std::array<std::uint32_t, 9>& tints);
    // How many Vials of Prismatic Dye the possessions hold.
    std::uint32_t prismatic_dye_count() const;

    // ---- auras (OP_UpdateAura): the player's auras (AuraWindow); a right click removes one.
    const std::vector<AuraUpdate>& auras() const { return auras_; }
    void remove_aura(std::uint32_t spawn_id);

    // ---- the aggro meter (net/aggro_packets.h): the server's hate percentages for the meter's target (our
    // target, or a player target's target) and its lock (CMD_LOCK_AGGRO).
    struct Aggro {
        std::uint32_t lock_id = 0, target_id = 0, secondary_id = 0;
        std::array<std::uint16_t, kAggroTypes> pct{};
        bool window_open = false; // AggroMeterWnd (shown once the server sends the meter)
    };
    const Aggro& aggro() const { return aggro_; }
    void set_aggro_window_open(bool open) { aggro_.window_open = open; }
    // Locks the meter on the target, or unlocks it when it is locked.
    void aggro_lock_toggle();

    // ---- the guild bank (net/game_connection_guild_bank.cpp, net/guild_bank_packets.h): the deposit and main areas
    // as the server sent them (OP_GuildBankItemList while zoning into the guild hall, then OP_GuildBank updates),
    // and CGuildBankWnd's state: the banker a right click on a class-66 guild banker opened it at, the selected item
    // (key: area << 16 | slot) and the requests awaiting an answer (its share of EverQuestinfo::IsItemPending).
    struct GuildBank {
        std::uint64_t serial = 0;
        std::vector<std::optional<GuildBankItem>> deposit, main;
        std::uint32_t banker = 0;  // the banker's spawn id while the window is open (+0xd0b0)
        std::int32_t selected = -1; // +0xd0a8
        int pending = 0;
        std::int64_t checked_ms = 0;
    };
    const GuildBank& guild_bank() const { return guild_bank_; }
    const GuildBankItem* guild_bank_item(std::int32_t key) const;
    bool guild_bank_mine(std::int32_t key) const;
    bool guild_bank_can_withdraw(std::int32_t key) const;
    bool guild_bank_can_view(std::int32_t key) const;
    bool guild_bank_allowed() const;
    // A guild hall zone (EQWorldData's flag 0x80000000: the guild hall and the player guild halls).
    bool in_guild_hall_zone() const;
    bool open_guild_bank(std::uint32_t npc);
    void close_guild_bank();
    void guild_bank_select(std::int32_t key);
    void guild_bank_deposit();
    // A request on the selected item: promote, view, withdraw, split or merge, with its value.
    void guild_bank_send(GuildBankAction action, std::uint32_t value);
    void guild_bank_set_permission(GuildBankPermission permission, const std::string& member);

    // ---- real estate (net/game_connection_real_estate.cpp, net/real_estate_packets.h, game/real_estate.h): player
    // housing, guild halls and placed objects as RealEstateManagerClient keeps them, fed by a server that has them
    // (a stock EQEmu server sends none of it), and the real estate windows' state (CRealEstate*Wnd).
    struct RealEstateUi {
        bool items_open = false, manage_open = false, neighborhood_open = false, plot_search_open = false;
        bool purchase_open = false; // RealEstatePurchaseWnd (the plot window)
        std::uint32_t purchase_re = game::kReNone; // +0x228: the plot shown
        std::uint8_t plot_search_mode = 0;         // PlotSearchWnd +0x24c (0x2b35's byte)
        std::string plot_search_title;
        std::vector<RePlotRow> plots;              // PlotSearchWnd's PlotList
        std::uint32_t neighborhood_header = game::kReNone;
        std::uint32_t neighborhood_selected = game::kReNone;
        std::vector<ReNeighborhoodRow> neighborhoods;
        std::vector<std::string> neighborhood_players;
        std::vector<std::int32_t> neighborhood_guilds;
        std::optional<ReMenu> menu; // the placed object's context menu (0x21ec's answer)
        int changes = 0;            // bumped by every real estate packet (the windows refresh on it)
    };
    const game::RealEstateManager& real_estate() const { return real_estate_; }
    const RealEstateUi& real_estate_ui() const { return re_ui_; }
    enum class ReWindow { Items, Manage, Neighborhood, PlotSearch, Purchase };
    void set_real_estate_window(ReWindow w, bool open);
    // /realestate items | manage | plot | trophies (Cmd_realestate @0x4e4e60): toggles that window.
    void real_estate_command(std::string_view args);
    // The windows' requests. Each sends only while in a zone; the checks are the original's where named.
    void re_search(std::uint32_t subtype, std::uint32_t id, std::string_view s0 = {}, std::string_view s1 = {},
                   std::string_view s2 = {}, std::uint8_t flag = 0);
    void re_travel(std::uint32_t id, std::uint32_t secondary);
    void re_plot_select(std::uint32_t plot);
    // RealEstate_CanPurchasePlot @0x5b1890 (verbose: its refusal printed), then 0x45be with the combos' keys.
    bool re_can_purchase(std::uint32_t re, bool verbose);
    void re_purchase(std::uint32_t price_key, std::uint32_t upkeep_key);
    // RealEstate_CanSellCurrentPlot @0x5b3430 / RealEstate_CanTransferPlotToTarget @0x5b1380 on the current estate.
    bool re_can_sell(bool verbose);
    void re_sell();
    bool re_can_transfer(bool verbose);
    void re_transfer();
    void re_opt_out(bool opt_out);
    void re_escrow(ReEscrowAction action, std::vector<ReEscrow::Entry> entries = {});
    void re_save_access(std::uint32_t re, const ReAccess& access);
    // The item's serial as its key (net::Item::serial), its estate and id; the destination re_container_index's.
    void re_to_storage(std::string_view key, std::uint32_t re, std::uint32_t item, std::uint32_t destination);
    void re_to_inventory(std::string_view key, std::uint32_t re, std::uint32_t item);
    // The upkeep combo's choice (CRealEstatePurchaseWnd / ManageWnd notification on the combo): escrow action 2 for
    // the yard with the entry's key at +8.
    void re_set_upkeep(std::uint32_t key);
    // The item menu's other requests (net::encode_re_item_op / encode_re_menu5).
    void re_item_op(std::uint16_t op, std::uint32_t re, std::uint32_t item, std::uint32_t param);
    void re_store_all();
    void re_pack_crate(bool pack);
    void re_inspect(std::uint32_t re, std::uint32_t item);
    void re_request_menu(std::string_view key, std::uint32_t re, std::uint32_t item, std::int32_t x, std::int32_t y,
                         std::uint32_t f2f8);
    void re_close_menu() { re_ui_.menu.reset(); }
    // CRealEstatePurchaseWnd_SetPlotInfo @0x74dcb0: shows the plot window on a record.
    void re_show_plot(std::uint32_t re);
    // A right click on a placed object (CEverQuest::RMouseUp): its menu asked for when placed_menu_in_reach allows;
    // `x`, `y` the screen click. A spawn the server marked as a real estate NPC item (0x21ea) the same (within 60,
    // or its estate the current one); re_npc_item gives its {estate, item}.
    bool re_request_placed_menu(std::uint32_t re, std::uint32_t item, const glm::vec3& actor, float actor_radius,
                                std::int32_t x, std::int32_t y);
    std::optional<std::pair<std::uint32_t, std::uint32_t>> re_npc_item(std::uint32_t spawn_id) const;
    bool re_request_npc_item_menu(std::uint32_t spawn_id, std::int32_t x, std::int32_t y);

    // ---- item placement (game/placement.h): the session the server's 0x626e reply begins and its 0x1228 {0}
    // commits (ItemPlacement_Stop), 0x6213 cancels, a zone change ends.
    const game::Placement& placement() const { return placement_; }
    // CEverQuest::LMouseUp with a placeable item on the cursor: true when it asked to place it (0x626e) and the drop
    // must not happen. Not while a request is pending (the click is used up) or while placing.
    bool re_place_cursor_item(std::int32_t x, std::int32_t y);
    // The items window's Place on a row: an item already in an estate ({re, item}, the index left at -1s).
    void re_place_row(const std::string& serial, std::uint32_t re, std::uint32_t item, std::int32_t x, std::int32_t y);
    // The placement keys (CMD_REAL_ESTATE_ITEM_PLACEMENT_*): ItemPlacement_SetMode (-1 toggles), the resets, the
    // 90-degree turns, the scale (the mouse with the size modifier).
    void placement_set_mode(int mode);
    void placement_rotate(float heading, float pitch, float roll) { placement_.rotate(heading, pitch, roll); }
    void placement_reset_rotation(bool x, bool y, bool wheel) { placement_.reset_rotation(x, y, wheel); }
    void placement_adjust_scale(float delta) { placement_.adjust_scale(delta); }
    void placement_reset_scale() { placement_.reset_scale(); }
    // The ghost moved (world coordinates) and re-validated against the areas with its model's local bounds;
    // `extra_code` a refusal the app found (0x1d, 0x1c), 0 none.
    void placement_update(const glm::vec3& world, const glm::vec3& lo, const glm::vec3& hi, std::uint32_t extra_code);
    // ItemPlacement_OnLeftMouseUp: the confirm (0x711a) when valid, with the actor's box; the latch set.
    bool placement_click(const glm::vec3& box_a, const glm::vec3& box_b);
    // Escape while placing (this client's: the original's cancel paths are not traced); ItemPlacement_Stop.
    void placement_cancel() { placement_.stop(); }

    // ---- LDoN adventures (net/game_connection_adventure.cpp, net/adventure_packets.h): the recruiter's window (a
    // right click on a class-60 NPC: OP_AdventureInfoRequest), the request and the offer to accept, the current
    // adventure, points, stats and the leaderboard. EQEmu's world server picks the adventure and checks the group.
    struct Adventure {
        std::uint32_t recruiter = 0;     // the recruiter whose window is open
        std::string npc_text;            // OP_AdventureInfo: the recruiter's words or the server's refusal
        std::string offer;               // OP_AdventureDetails: awaiting accept / decline
        AdventureData data;              // the current adventure (assigned false: none)
        std::int64_t data_at = 0;        // when it came (now_ms); its times count down from there
        AdventureCount count;
        AdventurePoints points;
        std::optional<AdventureStats> stats;
        std::optional<AdventureLeaderboard> leaderboard;
        bool stats_open = false, leaderboard_open = false;
    };
    const Adventure& adventure() const { return adventure_; }
    bool open_adventure_recruiter(std::uint32_t npc);
    void close_adventure_recruiter() { adventure_answer(false); adventure_.recruiter = 0; }
    void adventure_request(std::uint32_t risk, std::uint32_t type);
    void adventure_answer(bool accept);
    // OP_LeaveAdventure (no body): leave the current adventure (EQEmu's world server may refuse, with its own line).
    void adventure_leave();
    void set_adventure_stats_open(bool open);
    void set_adventure_leaderboard_open(bool open);
    void adventure_stats_request();
    void adventure_leaderboard_request(std::uint32_t order, std::uint32_t theme, std::uint32_t risk);

    // ---- guild tribute (net/game_connection_tribute.cpp): the guild's tributes (asked for while zoning in), its two
    // benefits, favour and timer, our opt-in, and a guild tribute master's window (class 64:
    // OP_OpenGuildTributeMaster; TributeMasterWnd in guild mode).
    struct GuildTribute {
        std::map<std::uint32_t, TributeAbility> abilities;
        std::map<std::uint32_t, std::string> descriptions;
        GuildTributeActive active;
        std::int64_t timer_at = 0; // when active.timer_ms came (now_ms)
        std::optional<bool> opted_in;
        std::uint32_t master_id = 0;
        std::uint64_t master_serial = 0;
        std::int32_t last_donation = 0;
        std::uint32_t donation_answers = 0;
        std::uint32_t active_cost() const;
        std::int64_t time_left(std::int64_t now) const;
    };
    const GuildTribute& guild_tribute() const { return guild_tribute_; }
    void close_guild_tribute_master() { guild_tribute_.master_id = 0; }
    void guild_tribute_describe(std::uint32_t tribute_id, std::uint32_t tier = 0);
    // Saves the guild's two benefits (OP_GuildSaveActiveTributes, then OP_GuildModifyBenefits).
    void guild_tribute_change(const GuildTributeActive& v);
    void guild_tribute_toggle(bool on);
    void guild_tribute_opt(bool in);
    // The guild window's Tribute page: opts a member (yourself, or another with permission 22 / 24) out of the
    // guild tribute (OP_GuildOptInOut) or the trophy tribute (opcode 0x02bc), as CGuildMgmtWnd's buttons do.
    void guild_tribute_opt_out(const std::string& member);
    void guild_trophy_opt_out(const std::string& member);
    bool guild_tribute_donate_cursor();
    bool guild_tribute_donate_platinum(std::uint32_t platinum);

    // ---- alternate currencies and point merchants (net/game_connection_commerce.cpp, net/merchant_packets.h)
    struct AltCurrency {
        AltCurrencyEntry entry;
        std::uint32_t amount = 0;
    };
    const std::map<std::uint32_t, AltCurrency>& alt_currencies() const { return alt_currencies_; }
    // Reclaim: the cursor's currency item into the count (put_in), or `count` of it out onto the cursor.
    void alt_currency_reclaim(std::uint32_t currency, std::uint32_t count, bool put_in);
    struct PointMerchant {
        std::uint32_t npc = 0;           // the merchant while its window is open
        PointMerchantList list;          // the server's answer
        std::optional<AltCurrencyQuote> quote; // what the selected inventory item would fetch
        ItemSlot quoted = ItemSlot::invalid();
    };
    const PointMerchant& point_merchant() const { return point_merchant_; }
    bool open_point_merchant(std::uint32_t npc);
    void close_point_merchant();
    // Adventure merchants (classes 59, 61, 67, 68: NewPointMerchantWnd), paid in LDoN points, PvP points or crystals.
    struct AdventureMerchant {
        std::uint32_t npc = 0, type = 0; // type: 1 LDoN, 2 Discord, 4 Norrath's Keepers, 5 Dark Reign
        std::vector<AdventureMerchantItem> items;
    };
    const AdventureMerchant& adventure_merchant() const { return adventure_merchant_; }
    bool open_adventure_merchant(std::uint32_t npc);
    void close_adventure_merchant();
    void adventure_merchant_buy(std::uint32_t item_id);
    // The adventure merchant window's Purchase (CPointMerchantWnd_ConfirmPurchase @0x71bba0, one item): with
    // pointMerchantNoConfirm it buys at once, else it asks first (the merchant's point system's question).
    void adventure_merchant_purchase(std::uint32_t item_id);
    // Sells the item at `slot` (the cursor's, as the window takes it); false when the merchant will not buy it.
    bool adventure_merchant_sell(const ItemSlot& slot);
    static std::uint32_t adventure_merchant_type(std::uint8_t class_id); // 0: not an adventure merchant
    void point_merchant_buy(std::uint32_t item_id);
    // The alternate currency merchant window's Purchase (CNewPointMerchantWnd_ConfirmPurchase @0x6fce90, one item):
    // with pointMerchantNoConfirm it buys at once, else it asks first (8064).
    void point_merchant_purchase(std::uint32_t item_id);
    void point_merchant_quote(const ItemSlot& slot);
    void point_merchant_sell();

    // ---- leadership (net/game_connection_group.cpp; game/leadership.h): the profile's experience, points and
    // ranks as the server updates them, earning leadership experience on or off, and buying a rank.
    struct Leadership {
        double group_exp = 0, raid_exp = 0;
        std::uint32_t group_points = 0, raid_points = 0;
        std::array<std::uint32_t, 32> ranks{};
        bool exp_on = false;
        bool window_open = false;
        // The abilities in use (HandleWorldMessage_LeadershipAbility @0x560260): the group leader's group ability
        // ranks (OP_GroupLeadershipAAUpdate; Leadership_HasAbilityRank reads them for abilities 0..15) and the raid
        // leader's (16..31: OP_RaidUpdate 14 and 30, Leadership_SetRaidAbilityRanks), the delegated group marker and
        // main assist (OP_DelegateAbility 1 / 2, spawn ids), the raid's three main assists, markers and the third set
        // (OP_RaidDelegateAbility 3 / 4 / 5 by member number: the profile's +0x350, +0x41c and +0x428; the main
        // assists' names at +0x35c), the NPCs marked 1..3 for the group (OP_MarkNPC) and the raid (OP_MarkRaidNPC),
        // and the main assists' targets (OP_SetGroupTarget: the group's [0], the raid's [1..3]; PlayerZoneClient
        // GroupAssistNPC / RaidAssistNPC).
        std::array<std::uint32_t, 32> active_ranks{};
        std::uint32_t group_marker = 0, group_assist = 0;
        std::array<std::uint32_t, 3> raid_assists{}, raid_markers{}, raid_waypointers{}, group_marks{}, raid_marks{};
        std::array<std::string, 3> raid_assist_names;
        std::array<std::uint32_t, 4> assist_targets{};
    };
    const Leadership& leadership() const { return leadership_; }
    // Group mentoring (CGroupMentorWnd; the leadership window's Group Mentoring button opens it for the group
    // leader): the protege and the share of experience given (PcBase szGroupMentoringName / GroupMentoringPercent,
    // set when sent; the server sends them back to no one).
    struct GroupMentor {
        bool window_open = false;
        std::string name;
        std::int32_t percent = 0;
    };
    const GroupMentor& group_mentor() const { return group_mentor_; }
    void set_group_mentor_window_open(bool open) { group_mentor_.window_open = open; }
    // Send_OP_GroupMentor @0x6741a0: an empty name clears the mentor; a protege not in the zone is refused (0x266,
    // false). The name and percent are kept (PcBase's virtual +0x48) and sent.
    bool send_group_mentor(const std::string& name, std::int32_t percent);
    void set_leadership_window_open(bool open) { leadership_.window_open = open; }
    // Leadership_ActivateAbility @0x55f0f0: a hot button of type 11 (value: ability * 8 + the mark or rank) and the
    // commands (/marknpc, /rmarknpc, /inspectbuffs).
    void activate_leadership_ability(int ability, std::int32_t parameter);
    bool leadership_ability_active(int ability, std::int32_t rank) const; // Leadership_IsAbilityActive @0x55eb70
    bool has_gates_of_discord() const; // the character's expansion flag 0x40
    // Whether the leadership window's hot button n (1..3) for the ability is enabled (0x560180: not a passive
    // ability, n within the ability's count, and the character's own rank n or the ability active at rank n).
    bool leadership_hot_button_enabled(int ability, int n) const;
    // Its text (the record's three words, spaced, then " 1" (only for an ability with more than one hot button),
    // " 2" or " 3").
    std::string leadership_hot_button_label(int ability, int n) const;
    // The target of target window is shown (the app tells; Leadership_ActivateAbility 0xe / 0x1d hide it then).
    void set_target_of_target_open(bool open) { target_of_target_open_ = open; }
    // The group finder (CGroupSearchWnd "GroupSearchWnd" and its additional filters, CGroupSearchFiltersWnd; rules in
    // game/group_search.h, the windows in app/lua_eq_group_search.cpp): the fields as the original's children hold
    // them, what the window last sent (+0x340 the LFG record, +0xe7cc the LFP record; `on` = posted), the results
    // of the last searches and the filters (character ini [ADDITIONALFILTERS]).
    struct GroupSearch {
        bool window_open = false, filters_open = false;
        bool prepared = false; // the fields filled in (the window's constructor: sub_00677d30 / sub_00678a50)
        int info_page = -1;    // the "I am a:" page the group window's LFG button asks for (0x676550); -1 none
        std::string player_comments, player_min, player_max; // PIP_CommentsEditbox, PIP_LevelRange*Editbox
        std::string group_comments, group_min, group_max;    // GIP_
        std::array<bool, 16> roles{};                        // GIP_<abbr>Checkbox (game::kGroupSearchClassBoxes)
        bool players_show_all = false, groups_show_all = false; // PLP_ / GLP_ShowAllCheckbox (else Show Matches)
        LfgCommand lfg;
        LfpCommand lfp;
        bool lfg_remove_checked = false; // 0x678250 checks LFG off, the post unchecks it
        std::uint32_t last_query_ms = 0; // +0xe828, the display clock
        bool players_query_all = false, groups_query_all = false; // +0xe824 / +0xe7bc: unfiltered results
        std::vector<LfgMatch> players; // the rows added (CGroupSearchWnd_AddLfgMatchRow)
        std::vector<LfpMatch> groups;  // the groups' records (+0x398, 0x248 bytes each)
        int results_serial = 0;        // bumped by every reply
        game::GroupSearchFilters filters;
        bool filters_loaded = false;   // LoadIni ran (app/actions_ui.cpp)
        bool filters_dirty = false;    // SaveIni: written back to the character ini
    };
    GroupSearch& group_search() { return group_search_; }
    const GroupSearch& group_search() const { return group_search_; }
    // The window's constructor: the level ranges (GroupSearch_DefaultLevels), comments up to 63, all 16 classes,
    // Show Matches; once, with the player known.
    void group_search_prepare();
    // The "I am a:" pages' buttons (CGroupSearchWnd 0x679b70 / 0x67a6f0): LFG on (0x678df0) or Update
    // (0x678ea0), LFG off (0x678250); the group's post (0x679d30, the leader only) or Update
    // (0x679e00), off (0x678310). A bad field shows GroupSearch_ShowError's dialog and sends nothing.
    void group_search_post_player(bool update);
    void group_search_remove_player();
    void group_search_post_group(bool update);
    void group_search_remove_group();
    // 0x6786a0 / 0x679480: the fields differ from what was posted (the Update button is enabled).
    bool group_search_player_changed() const;
    bool group_search_group_changed() const;
    // Get Matches: HandleQueryingForPlayers @0x679050 (the group page's levels and classes, or Show All: 1..100, all)
    // and 0x678ed0 (the player page's levels, or Show All); both start the 15 s lock.
    void group_search_query_players();
    void group_search_query_groups();
    // 0x678550, on joining a group (Send_OP_GroupFollow2) or the group you formed arriving
    // (Handle_OP_GroupUpdateB with bIAmCreatingGroup): LFG off; LFP off too unless you lead a group short of six.
    void group_search_joined_group();
    // CGroupSearchWnd_IsRowFiltered on a name and guild id (CGuild::GetGuildName's text).
    bool group_search_filtered(const std::string& name, std::int32_t guild) const;
    // The group window's LFG button (CGroupWnd::WndNotification): hides the window, or shows it on the group page
    // for a group's leader, else on the player page.
    void group_search_toggle_from_group_window();
    void set_leadership_exp(bool on);
    void buy_leadership(std::uint32_t ability);
    // The leadership window's Purchase (CLeadershipWnd::WndNotification @0x6b7bf0): with LeadershipNoConfirm it buys
    // at once, else it asks first (8582 / 8583 group, 8587 / 8588 raid, with the next rank's cost and the name, 20 s).
    void leadership_purchase_button(std::uint32_t ability);

    // ---- titles (net/game_connection_player.cpp): the title window (TitleWnd), the server's list of titles we may
    // take (OP_RequestTitles when it opens), and choosing a prefix or suffix (OP_SetTitle; 0 clears).
    struct Titles {
        bool open = false;
        bool notify = true; // "Notify me when new titles are available."
        std::vector<TitleEntry> available;
    };
    const Titles& titles() const { return titles_; }
    void set_titles_open(bool open);
    void set_titles_notify(bool on) { titles_.notify = on; }
    void set_title(bool suffix, std::uint32_t title_id);

    // ---- teleports (game/zoning.h; net/game_connection_death.cpp) ----
    // CEverQuest::MoveToZone: OP_ZoneChange {zone | instance << 16, the first two caller dwords, y, x, z
    // (the call's x, y, z: the player's +0x64.. order, world x, y, z here), reason}.
    void move_to_zone(const game::MoveToZoneCall& m);
    // SendTeleportIndexRequestAndWait: opcode 0xcadf {u32 zone point}; the player waits (+0x1f05) until the
    // server's 0xcadf, and the server moves it. The client does not block: waiting_for_port() says it waits.
    void send_teleport_index_request(std::int32_t zone_point);
    bool waiting_for_port() const { return waiting_for_port_; }
    void clear_waiting_for_port() { waiting_for_port_ = false; }
    // ---- the position history (game/position_tracker.h; net/game_connection_death.cpp) ----
    // PlayerClient_RecordPositionSample(reason, position): the local player's position (the spawn's, world x, y and
    // the spawn's z), or `world` when given, as a sample; reasons 3, 4, 5 and 8 are sent at once.
    void record_position_sample(std::uint32_t reason, const glm::vec3* world = nullptr);
    // Send_OP_FloatListThing(force): without `force` at most once a second (PlayerClient +0x1e8c) and only the
    // samples older than 2 s; with it every sample, in as many packets as it takes.
    void send_position_history(bool force);
    // 0x58ff50: Send_OP_FloatListThing(1), then the samples are forgotten (0x56e510).
    void flush_position_history();
    const game::PositionTracker& position_tracker() const { return position_tracker_; }
    // The zone point table as the original holds it (OP_SendZonePoints rows, zone | instance << 16).
    std::vector<game::ZonePointRow> zone_point_rows() const;
    std::uint32_t current_zone_dword() const;
    // The position update DoTeleportB sends before a reason-0 teleport.
    void send_position_now();

    // ---- movement (game/mechanics/movement.h; net/game_connection_movement.cpp) ----
    // OP_Jump, and the endurance the original takes locally (DoMainLoop @0x53ed48).
    void report_jump(std::int32_t endurance_cost);
    // OP_EnvDamage with damage the client computed (falls 0xfc, drowning
    // 0xfb, ...); the HP drop is applied locally too (ApplyDamage @0x457d60).
    void report_env_damage(std::uint32_t damage, std::uint8_t type);
    // OP_SafeFallSuccess (ApplyFallDamage, when Safe Fall reduced a fall).
    void report_safe_fall();
    // __RunWalkState: OP_SetRunMode when it changes (DoPassageOfTime).
    void set_running(bool running);
    bool running() const { return running_; }

    // ---- spells (game/spells.h; net/game_connection_spells.cpp) ----
    const game::SpellTable& spell_table() const { return *spell_table_; }
    const game::SpellState& spell_state() const { return spells_; }
    // eqstr_us.txt / dbstr_us.txt (spell descriptions are dbstr type 6).
    const game::EqStrings& strings() const { return strings_; }
    // The player's classes as a bit mask: server_class_mask(), else the profile's class.
    std::uint32_t player_class_mask() const;
    // player_class_mask() where the server extension tests those class checks over the player's classes
    // (game/server_ext.h research_classes, focus_classes), else 0 (the profile's class alone).
    std::uint32_t research_class_mask() const { return server_ext().research_classes(*this) ? player_class_mask() : 0; }
    std::uint32_t focus_class_mask() const { return server_ext().focus_classes(*this) ? player_class_mask() : 0; }
    // ---- the server extension (game/server_ext.h) ----
    // The extension the core asks: the chain of ConnectionOptions::plugins, each member while it says it is active.
    // With none active it gives the stock client's answers.
    const game::ServerExtension& server_ext() const { return ext_chain_; }
    game::ServerExtension& server_ext() { return ext_chain_; }
    // The local player's classes as the server's extension set them (set_player_classes), else the mask its
    // character had at character select (classes_mask); 0 on a stock server.
    std::uint32_t server_class_mask() const {
        return player_classes_ ? player_classes_ : classes_mask(character_);
    }
    void set_player_classes(std::uint32_t mask) { player_classes_ = mask; }
    // A guild member's class column: the member's class value as the server stores it, through the extension's
    // guild_member_classes (a class mask: "War/Clr"), else the class's short name.
    std::string guild_member_class(std::uint32_t class_value) const;
    // The class the original's checks see: the profile's, or the extension's (the NMS patch's cast spoof while a
    // non-bard spell is being cast, 0x1016b670).
    std::uint8_t visible_class() const;
    // EQ_Character::IsSpellcaster for the local character (the extension may answer it).
    bool is_spellcaster() const;
    // EQ_Spell::GetSpellLevelNeeded(class_id) as the client answers it: the stock byte, or the extension's.
    std::uint8_t spell_level_needed(const game::SpellData& spell, std::uint32_t class_id) const;
    // CEverQuest::GetClassDesc(id).
    std::string class_desc(std::int32_t id) const { return game::class_name(static_cast<std::uint32_t>(id)); }
    // For plugins (with send_zone_packet): clear the global spell recovery (SpellCooldownETA), and the player's
    // points.
    void clear_spell_recovery() { spells_.recovery_ready = 0; }
    PlayerStatus& player_status_mut() { return pstatus_; }
    // Gems the player may use: 8, more with SPA 326 / Mnemonic Retention (see docs/unknowns.md).
    // The gems CharacterZoneClient::CanUseMemorizedSpellSlot allows (0..7, then 8 + n below the SPA 326 total),
    // counted from 0: the first gem it refuses. Recomputed once a frame (update_spells).
    int usable_gems() const { return usable_gems_; }
    // CCastSpellWnd::HandleSpellLeftClick: cast the spell in gem 0..11.
    void cast_gem(int gem);
    // Blocked buffs (OP_BlockedBuffs): the player's and the pet's lists as the server keeps them, adding and
    // removing one, and the lists sent again at every zone-in (the server forgets them).
    const std::vector<std::int32_t>& blocked_buffs(bool pet) const { return blocked_buffs_[pet ? 1 : 0]; }
    void block_buff(bool pet, std::int32_t spell);
    void unblock_buff(bool pet, std::int32_t spell);
    // ---- using items (net/game_connection_item_use.cpp)
    // A right click on an item slot (CInvSlot::HandleRButtonUp @0x697250): a book is read, a common item in the
    // possessions is used (use_item), a corpse slot looted; what is left to the windows comes back. The server
    // extension may take the click first (NMS: scribing scrolls and tomes); right_click_item_original is the stock
    // function without it, `class_id` the class its casting check sees (0: the visible class).
    enum class SlotRightClick : std::uint8_t {
        None,
        ToggleContainer, // CContainerMgr::OpenContainer / CloseContainer
        ItemPreview,     // Alt: OP_ItemPreviewRequest sent and CObjectPreviewWnd shown (preview_item)
        BazaarSelect,    // Ctrl + Shift on a trader's slot (CBazaarWnd::SelectBazaarSlotItem)
    };
    using KeyFlags = SlotClickKeys;
    SlotRightClick right_click_item(const ItemSlot& slot, KeyFlags keys = {});
    SlotRightClick right_click_item_original(const ItemSlot& slot, KeyFlags keys = {}, std::uint8_t class_id = 0);
    // Using a common item in the possessions (HandleRButtonUp, Cmd_useitem, PcZoneClient_UsePotionBeltSlot): the
    // sitting check of its click spell, then OP_ItemVerifyRequest with the target. The server's OP_ItemVerifyReply
    // eats, drinks or casts it (RoF2 casting slot 12). True when the request went out.
    static constexpr int kCastItemSlot = 12;
    bool use_item(const ItemSlot& slot);
    bool cast_item(const ItemSlot& slot); // use_item (the older name)
    // CEverQuest::ApplyPoison @0x5202d0, run by the item reply for a poison (item type 42): needs the Apply Poison
    // skill (12383), sits a standing player down and, sitting, starts the application (12382: its start time and
    // the poison's top-level slot, 0xddf724 / 0xddf728), else 12381. Nothing while camping, already applying,
    // interacting or with an item request pending. DoPassageOfTime sends OP_ApplyPoison once TotalEffect(SPA 234)
    // ms (10 s without it) have passed; a stand state change cancels it (12497).
    void apply_poison(const ItemSlot& slot);
    bool applying_poison() const { return poison_started_ms_ != 0; }
    // CharacterZoneClient::BardCastBard for the spell's record (the server extension's song block first).
    bool bard_cast_bard(const game::SpellData& spell, std::uint8_t class_id) const;
    // The potion belt (the profile's five slots; PotionBeltWnd, /potionbelt, POTION_SLOT_n): putting the cursor
    // item in a slot or clearing one (OP_PotionBelt; EQEmu keeps slots 0..3), how many of a slot's item the player
    // carries, and using it (the first one found in the possessions, use_item).
    static constexpr int kPotionBeltSlots = 5, kPotionBeltServerSlots = 4;
    bool potion_belt_open() const { return potion_belt_open_; }
    void set_potion_belt_open(bool open) { potion_belt_open_ = open; }
    const ProfileBeltItem* potion_belt_item(int slot) const;
    bool potion_belt_set(int slot);
    void potion_belt_clear(int slot);
    // The window's right button held on a slot (encode_potion_belt_hold); the server clears it, so it is cleared here.
    void potion_belt_hold(int slot, bool shift);
    std::uint32_t potion_belt_count(int slot) const;
    // The first carried item with the belt slot's item id (MultipleItemMoveManager_CollectItemsByID's first location,
    // as CPotionBeltWnd::PostDraw takes it; here in find_possession's order), null when none.
    const Item* potion_belt_first_item(int slot) const;
    bool potion_belt_use(int slot);
    // PcZoneClient::doCombatAbility(spell, rank) (game/mechanics/combat_ability.h): a discipline hot button
    // (type 10) and /discipline run it with rank 1. True when the cast went out.
    bool do_combat_ability(std::uint32_t spell, bool rank = true);
    // Cmd_discipline: /discipline <name> uses the ability, /discipline alone lists the ones on their reuse timer.
    void discipline_command(const std::string& args);
    // The spell's CARecastTimerID as this client holds it (the server extension may move it).
    std::int32_t combat_ability_timer(const game::SpellData& s) const;
    // GetDisciplineReuseTimer(id): the ready time in Unix seconds (0: ready).
    std::uint32_t discipline_reuse_timer(std::int32_t id) const;
    // PcZoneClient_IsDisciplineActive @0x5802c0: no open effect slot in the discipline's range (GetOpenEffectSlot(0,
    // 1, -1) == -1: for a player the last slot, 62, GetFirstEffectSlot(0, 1)).
    bool discipline_active() const { return !spells_.buffs[game::kBuffSlots - 1].empty(); }
    // Cmd_stopdisc @0x478cb0 (/stopdisc) and the combat ability window's effect button: with a discipline active,
    // RemoveMyAffect on its slot.
    void stop_discipline();
    // PcZoneClient::RemoveMyAffect @0x57fa50: an effect whose spell has no SPA 101 and may be clicked off
    // (EQ_Spell::IsClickOffAllowed false: beneficial, not NoRemove) goes (the removal request; the server's fade
    // clears the slot); anything else is "You cannot remove this effect." (token 137, colour 0xd). False then.
    bool remove_my_affect(int slot);

    // ---- abilities commands (net/game_connection_ability_commands.cpp) ----
    // The bard twist (PlayerClient +0x1e90 list of gem numbers 1.., +0x1eac position): Cmd_melody fills it,
    // Handle_OP_MemorizeSpell plays the next gem when a song's bar refresh comes (EQEmu: Spells:EnableBardMelody).
    // PlayerClient_ClearBardTwist @0x58ff30 (/cast, a gem click, a spell gem hot button, /stopsong, /melody).
    void clear_bard_twist() { bard_twist_.clear(), bard_twist_pos_ = 0; }
    // PlayerClient_GetNextBardTwistSpell @0x594d80: the gem number at the position, which moves on (wrapping); 0
    // without a twist.
    std::int32_t next_bard_twist_gem();
    // PlayerClient_GetCurrentBardTwistSpell @0x594d40: the entry before the position (entry 0 at 0); 0 without.
    std::int32_t current_bard_twist_gem() const;
    const std::vector<std::int32_t>& bard_twist() const { return bard_twist_; }
    // Cmd_melody @0x4f5bf0 (/melody 1 2 3 ...): bards only. The twist is cleared; while a spell that is a bard song
    // (BardCastBard) is being cast it only stops (StopCurrentSpellCast(0)); otherwise every number above 0 in the text
    // (0x594cf0 appends it) joins the twist and its first gem is clicked (HandleSpellLeftClick).
    void melody_command(const std::string& args);
    // CharacterZoneClient_StopCurrentSpellCast(0) as the twist uses it (game_connection_ability_commands.cpp).
    void stop_current_spell_cast(std::uint8_t reason = 0);
    // Cmd_stopsong @0x4f26b0: a bard stops the song being sung and the twist (/stopsong, the STOP_ACTION key).
    void stop_song();
    // BardCastBard for the profile's class: the spell is sung (a song) by this character.
    bool bard_song(const game::SpellData& s) const;
    // EQPlayer::GetMount for the local player: the mount the link seats it on (game/mounts.h), 0 none.
    std::uint32_t mount_id() const;
    // Cmd_dismount @0x4eac50 (/dismount): frozen (stand state 102) 6159; without a mount 13285; else "Dismounting."
    // (13284), the SPA 113 effect removed (GetPCSpellAffect(113) / RemovePCAffectex) and the local player's
    // virtual +0x60 unseats it.
    void dismount_command();
    // Cmd_offlinemode @0x4e6660 (/offlinemode, and the trader and barter windows' Offline Mode buttons, which call it
    // from CBazaarWnd::WndNotification @0x63a31b and CBarterWnd::WndNotification @0x631fc3).
    void offlinemode_command();
    // What the rider-mount link asks of the client (the app's models and collision; game/mounts.h). Null: the
    // defaults (Luclin models loaded, every mount has its rider dag, no room test).
    void set_mount_queries(game::MountQueries* q) { mount_queries_ = q; }
    // The spawn the local player moves (pinstControlledPlayer): its mount while it rides one, else 0.
    std::uint32_t controlled_id() const { return world_.controlled_id; }
    std::uint32_t eye_id() const { return world_.eye_id; }
    void acquire_eye_control();
    void release_eye_control(bool remove_effect = false);
    void reconcile_eye_control();
    // A spawn the local player's mount moved (the app runs the mount's physics): its position (world
    // coordinates), heading and motion, as the rider's are (the rider sits on it: PhysicsObject::vf8).
    void set_controlled_motion(const glm::vec3& position, float heading, const glm::vec3& velocity, float speed_run,
                               std::uint8_t body_wet = 0, std::uint8_t feet_wet = 0,
                               std::int32_t colliding_type = 0, float speed_heading = 0.0f);
    // CEverQuest::DoMainLoop's mount-race jump @0x53eb68: the mount's animation 0x14 and OP_Animation {the
    // controlled spawn, 0x14, 0x0a} on channel 4.
    void report_mount_jump();
    // Slash commands whose original works on state the app keeps (Cmd_doability: the Actions window's lists and
    // UseSkill): run_slash_command offers the handler name and arguments; true when taken.
    void set_app_commands(std::function<bool(std::string_view handler, const std::string& args)> fn) {
        app_commands_ = std::move(fn);
    }
    // The profile's combat ability n (PcBase::GetCombatAbility; 0 none).
    std::uint32_t combat_ability(std::int32_t n) const {
        if (!profile_ || n < 0 || n >= static_cast<std::int32_t>(profile_->disciplines.size())) return 0;
        return profile_->disciplines[static_cast<std::size_t>(n)];
    }
    // PlayerBase::CanSee's zone test (the app's collision): true when nothing blocks the segment (world coordinates).
    void set_line_of_sight(std::function<bool(const glm::vec3&, const glm::vec3&)> los) { line_of_sight_ = std::move(los); }
    // True once the app has given this connection its hooks (line of sight, teleport, app commands): the app installs
    // them on each new connection, and a fresh one has none.
    bool has_line_of_sight() const { return static_cast<bool>(line_of_sight_); }
    // OP_RequestClientZoneChange's DoTeleportB (the app's: the player's position and the zone's floor), given the
    // packet's zone dword (zone | instance << 16), its first float pair as world x / y, z, heading, the type as the
    // reason and its +4 dword; without one the client answers with OP_ZoneChange itself.
    using TeleportHandler = std::function<void(const game::TeleportBArgs&)>;
    void set_teleport_handler(TeleportHandler h) { teleport_handler_ = std::move(h); }
    // OP_RestState (PcClient_HandleRestState @0x578530): the combat flag (+0x31bc, vtable +0x80) and the
    // downtime still to run (+0x31c0 / +0x31c4, vtable +0x84 @0x577dd0, seconds).
    bool in_combat() const { return in_combat_; }
    std::uint32_t rest_downtime_left() const;
    // PcZoneClient_IsRestingForFastRegen @0x580370.
    bool resting_for_fast_regen() const;
    // What the character sheet is built from besides the profile, inventory and buffs (game_connection_ticks.cpp):
    // the level, the pools as the server sent them and the ticks moved them, sitting, hunger, auto attack.
    game::mechanics::LiveState live_state() const;
    std::int32_t drunkenness() const { return drunkenness_; }
    std::uint32_t drunkenness_updates() const { return drunkenness_updates_; }
    std::uint32_t drunkenness_motion_updates() const { return drunkenness_motion_updates_; }
    void update_player_consciousness(bool allow_knockout = false);
    std::uint32_t consciousness_changes() const { return consciousness_changes_; }
    // The hide and sneak flags the abilities code keeps (app/actions_ui.cpp), so the cast path's
    // MakeMeVisible can end them; take_visibility_cleared() hands back what it ended.
    void note_hide_state(bool hidden, bool sneaking) { hidden_ = hidden; sneaking_ = sneaking; }
    // PlayerZoneClient's Sneak flag as the server set it (SpawnAppearance 15); the movement update reads it.
    bool sneaking() const { return sneaking_; }
    struct VisibilityCleared {
        bool hide = false, sneak = false;
    };
    VisibilityCleared take_visibility_cleared() { return std::exchange(visibility_cleared_, {}); }
    // CharacterZoneClient::MakeMeVisible @0x4592b0: when invisible, the
    // invisibility effects are removed, the state is sent (SpawnAppearance {me, 3, 0}), hiding ends
    // (Send_OP_Hide: OP_Hide {0}, eqstr 12337) and with `appearance` sneaking too (eqstr 12338,
    // SpawnAppearance {me, 15, 0}).
    void make_me_visible(bool appearance);
    bool invisible() const;
    // Viewer awareness, shared by drawing, name plates, picking and target
    // cycling. Compute the effect rank once for a batch of spawns.
    std::int32_t see_invisible_rank() const;
    bool spawn_invisible(const game::Spawn& spawn, std::int32_t see_invisible_rank) const;
    // /stopcast: asks the server to interrupt (empty OP_ManaChange).
    void stop_cast();
    // CSpellBookWnd::StartSpellMemorization: memorize the book slot's spell into a gem.
    void memorize(int book_slot, int gem);
    // CSpellBookWnd::CanStartMemming @0x75bd40: whether the book slot's spell may go on the cursor to be memorized
    // (its refusals are printed: 12052 already memorized, 12054 level too low).
    bool can_start_memming(int book_slot);
    // CSpellBookWnd::StartSpellScribe: scribe the scroll on the cursor into a
    // book slot (-1: the first empty one). Book clicks need the player sitting.
    void scribe_cursor(int book_slot);
    // The spell of the scroll on the cursor, or 0.
    std::uint32_t cursor_scroll_spell() const;
    // CCastSpellWnd::Unmemorize: forget the gem's spell.
    void forget_gem(int gem);
    // ---- spell sets (game/spell_sets.h) ----
    // The character's 10 sets; loaded and saved by the client with the character ini (hotbuttons.ini).
    game::SpellLoadouts& spell_loadouts() { return spell_loadouts_; }
    // /grouproles' named role sets ([GroupRoleLoadouts] in the character ini; the app loads and saves them).
    game::GroupRoleSets& group_role_sets() { return group_role_sets_; }
    // A group role on or off for a member (Send_OP_GroupRoles: the group window's role menu, /grouproles).
    void send_group_role(const std::string& member, std::int32_t role, bool on);
    const game::SpellLoadouts& spell_loadouts() const { return spell_loadouts_; }
    // CSpellBookWnd::MemorizeSet @0x75cac0: memorizes `spells` (one per gem, -1 leaves a gem): with something held
    // on the cursor eqstr 3345 and false; the book is shown (and so the player sits); OP_LoadSpellSet asks the server
    // to forget the gems being replaced, then each gem is memorized in turn (continue_set_mem).
    bool memorize_set(const std::array<std::int32_t, game::kLoadoutGems>& spells);
    // CCastSpellWnd_TryMemorizeFromCursor @0x75cc70 (a spell select menu's spell for `gem`: the gem's own menu, a
    // hotbutton's Memorize): with something held on the cursor eqstr 3345 (colour 0xf) and nothing else; else the
    // book slot holding the spell (0x75bc00), CanStartMemming, the book shown (remembering whether it was), and
    // once it shows the memorize is scheduled 500 ms on (CSpellBookWnd +0x254 = 1, +0x294 / +0x298 the book slot and
    // gem, +0x250 the time; OnProcessFrame then StartSpellMemorization(slot, gem, 0)).
    void memorize_from_cursor(std::uint32_t spell, int gem);
    // The spell gem menu's set item: memorize set n (when in use), eqstr 8014 with its name.
    bool memorize_loadout(int n);
    // SaveLoadout: the gems as set `name` (eqstr 3236 with the name; all 10 in use: eqstr 3235 with "10").
    void save_loadout(std::string_view name);
    // The delete confirmation's yes: set n is no longer in use.
    void delete_loadout(int n);
    // Whether a spell set is being memorized (CSpellBookWnd +0x254 == 2).
    bool memorizing_set() const { return set_mem_.mode == 2; }
    // Clicks off one of the player's buffs (OP_BuffRemoveRequest).
    void remove_buff(int slot);
    // RemovePCAffectex(affect, 0) / NotifyPCAffectChange: the client drops a buff
    // itself (disarm while hidden, Sanctuary before an attack); no packet.
    void clear_buff_locally(int slot);
    // GetPCSpellAffect: the buff slots whose spell has this SPA, in slot order.
    std::vector<int> buff_slots_with_spa(std::int32_t spa) const;
    // Milliseconds on the clock the spell timers use.
    static std::int64_t now_ms();
    // __EQGetTime: now_ms() as the original's 32-bit millisecond clock (the gem and recovery timers).
    static std::uint32_t eq_time() { return static_cast<std::uint32_t>(now_ms()); }

    // ---- items (game/inventory.h; net/game_connection_items.cpp) ----
    const game::Inventory& inventory() const { return inventory_; }
    const net::Coins& coins() const { return coins_; }
    // A left click on an inventory, bank or shared bank slot
    // (CInvSlot::HandleLButtonUp): with an empty cursor picks the item up
    // (a stack opens the quantity dialog: quantity_request(); shift takes
    // the whole stack, ctrl one), else puts the cursor item down there
    // (merging stacks or swapping) after the original's checks; sends
    // OP_MoveItem. Corpse slots loot instead.
    // Ctrl with a bag at the slot moves the cursor item, or a cursor bag's contents, into that bag with
    // OP_MoveMultipleItems; an Attuneable item going into a worn slot asks first (confirmation 0x7a, eqstr 5912).
    void click_slot(const ItemSlot& slot, bool shift = false, bool ctrl = false);
    // The quantity dialog (CQuantityWnd) for picking up part of a stack:
    // Accept → CInvSlot::SliderComplete(quantity) splits the stack onto the
    // cursor (OP_MoveItem with the count) or, for all of it, moves it whole.
    const std::optional<QuantityRequest>& quantity_request() const { return quantity_; }
    void finish_quantity(std::uint32_t quantity);
    void cancel_quantity() { quantity_.reset(); }
    // Destroys the cursor item (PcZoneClient::DestroyHeldItemOrMoney).
    void destroy_cursor();
    // The inventory window's Destroy button (CInventoryWnd::DestroyHeld @0x692340, game/item_options.h): with Fast
    // Item Destroy the cursor's item or coin goes at once, else it asks first (2043 / 2061, 10 s).
    void destroy_held();
    // Puts the cursor item into the first free general slot or bag slot; false when it stays on the cursor.
    bool auto_inventory();
    // auto_inventory's last steps only: the first free general slot, else the first bag with room; never a worn slot
    // (Hailstone's bulk withdraw, which is not the original's and must not equip what it takes from the bank).
    bool stow_cursor_in_packs();
    // Hailstone: the items marked Keep (game/kept_items.h; the Advanced Inventory window), kept per character in
    // kept_items.txt (read at enter_world, written at each change). A kept item, or a bag holding one, is not
    // destroyed, dropped on the ground, put in a trade or a give, sold to a merchant or sent by parcel: each of those
    // says why in the chat and leaves it where it is.
    const game::KeptItems& kept_items() const { return kept_items_; }
    bool item_kept(const Item& item) const { return kept_items_.holds_kept(item); }
    void set_item_kept(std::uint32_t id, std::string_view name, bool on);
    // One persistence update for a set of marks (the Advanced Inventory's bulk mark actions).
    void set_items_kept(const std::vector<std::pair<std::uint32_t, std::string>>& items, bool on);
    // Counts the changes to the marks (a window's cache of them).
    std::uint64_t kept_items_revision() const { return kept_revision_; }
    // CharacterZoneClient::CanUseItem(item, 1, 0) for our character, quietly (the Advanced Inventory's "usable").
    bool item_usable(const Item& item) const { return can_use_item_full(item); }
    // A click on the inventory window's doll (CInventoryWnd::HandleLButtonUp @0x692ec0, IW_CharacterView): an item on
    // the cursor is auto-inventoried (sound 0x92), or, when it stays, "There are no open slots for the held item in
    // your inventory." (5290, colour 0xd; not for Krono); coin on the cursor goes back to the inventory as the same
    // coin (ClickedMoneyButton(CDisplay +0x11 - 1, -1)).
    void drop_on_doll();
    // Hailstone: sorting, stacking and tidying bags (game/item_sort.h; the modern UI's Sort / Stack buttons), run as
    // the clicks a player would make, one every kItemJobStepMs. A bag (`bag`: its top-level slot), the inventory's
    // bags or the open bank's (what is loose in a general or bank slot stays). `locked` slots are left as they are;
    // `filters` give bags (by top-level slot) a kind of item. Empty when it started, else why not.
    enum class ItemJobScope : std::uint8_t { Bag, Inventory, Bank };
    enum class ItemJobMode : std::uint8_t { Sort, Stack, Tidy, Sell, Destroy, Tribute, Withdraw };
    struct ItemJobOptions {
        std::vector<ItemSlot> locked;
        std::vector<std::pair<ItemSlot, game::ItemCategory>> filters;
    };
    std::string start_item_job(ItemJobScope scope, const ItemSlot& bag, ItemJobMode mode, const ItemJobOptions& options = {});
    // Sells every item with one of `ids` the player carries (the general slots and their bags) to the open merchant,
    // one at a time as the merchant answers. Empty when it started, else why not.
    std::string start_sell_job(const std::vector<std::uint32_t>& ids);
    // Hailstone: the Advanced Inventory's bulk actions on chosen slots (a deliberate difference, with no counterpart in
    // the original). Only what is carried (the general slots and their bags) is taken, never a kept item:
    //   Sell     to the open merchant, one sale at a time as it answers (not a bag; what the merchant buys);
    //   Destroy  each picked up and destroyed, with no confirmation each (the window asks once). Not a bag that holds
    //            anything, nor a No Destroy item;
    //   Tribute  each picked up and donated to the open tribute master, the next after its answer (an item worth no
    //            favour, or a bag, is left out; one the server refuses goes back to its slot).
    // bulk_items_plan is what a job would take now (the slots, in order); start_bulk_job is empty when it started, else
    // why not (bulk_items_blocked: the situation alone).
    std::vector<ItemSlot> bulk_items_plan(ItemJobMode mode, const std::vector<ItemSlot>& slots) const;
    std::string bulk_items_blocked(ItemJobMode mode) const;
    std::string start_bulk_job(ItemJobMode mode, const std::vector<ItemSlot>& slots);
    struct ItemJobStatus {
        std::size_t done = 0, total = 0;
        ItemJobMode mode = ItemJobMode::Sort;
        ItemJobScope scope = ItemJobScope::Bag;
    };
    // The job running, if one is.
    std::optional<ItemJobStatus> item_job() const;
    // CharacterBase_IsInvSlotAvailable for our character (a general slot the inventory has).
    bool slot_available(int slot) const { return inv_slot_available(slot); }
    void stop_item_job() { item_job_.reset(); }
    // Looting (CLootWnd): open a corpse, take one item (auto = into the
    // inventory, else onto the cursor), close.
    struct LootSession {
        std::uint32_t corpse_id = 0;
        std::string corpse_name;
        std::map<std::int16_t, Item> items; // by corpse slot
        net::Coins money;
        bool open = false;     // the server accepted the request
        bool complete = false; // every item sent
    };
    void loot(std::uint32_t corpse_id);
    // CLootWnd::RequestLootSlot: a corpse slot onto the cursor (or auto looted); a no-drop item may ask first
    // (LootNoDrop, game/item_options.h); a stack on someone else's corpse asks how many (quantity_request(),
    // Shift / Ctrl as the quantity dialog takes them).
    void loot_item(std::int16_t corpse_slot, bool auto_loot, bool shift = false, bool ctrl = false);
    // The Loot All button: asks first with LootAllConfirm (not on the player's own corpse), then CLootWnd::LootAll.
    void loot_all();
    void end_loot();
    const std::optional<LootSession>& loot_session() const { return loot_; }
    // Item links: asks the server for the linked item (OP_ItemLinkClick); its
    // OP_ItemLinkResponse arrives through take_item_display().
    void request_item(const game::ItemLink& link);
    // Send_OP_ItemLinkResponse(id, name, flags) (net/item_packets.h ItemLinkRequest): `shift` adds flag 2.
    void request_item_link(std::uint32_t item_id, const std::string& name, std::uint32_t flags, bool shift);
    std::optional<Item> take_item_display();
    // An item the server sent again as OP_ItemPacket type 0x67 (CEverQuest_HandleItemPacket @004c2570 → 0x6ae710
    // @006ae710): every open item display showing the same serial (+0xf8) is set to it in place.
    std::optional<Item> take_item_refresh();

    // ---- augments (game/augments.h; net/game_connection_augments.cpp) ----
    // What the request rules read: the cursor, the inventory searches, the class check.
    game::AugmentContext augment_context() const;
    // A left click on socket `socket` of an item display showing `window_item` (sub_006ae010: insert, remove or
    // swap), and the socket's context menu choice (CItemDisplayWnd::WndNotification XWM_MENUSELECT: 100 remove,
    // 0x65 swap, 0x66 destroy). Refusals go to chat in colour 13; a send is OP_AugmentItem; a confirmation asks with
    // ConfirmationDialog types 0x76..0x79 (IsItemPending + 1, pending reason 0x13, until it is answered).
    void augment_click(const Item& window_item, int socket);
    void augment_menu_select(const Item& window_item, int socket, std::uint32_t id);
    void run_augment_request(const game::AugmentRequest& r);
    // OP_AugmentInfo (CItemDisplayWnd::SetItem @006ae4b2): asks for the name of an augment's solvent for display
    // window `window`; the answer (5881 with the name, 13325 for a type-55 answer) becomes that window's
    // SolventText ("<BR>" + text), "" until then.
    void request_augment_info(std::uint32_t solvent_id, std::uint32_t window);
    std::string augment_solvent_text(std::uint32_t window) const;
    // servers/<profile>/characters/<world>/<character>/ (core/character_files.h); empty before a character enters the
    // world.
    CharacterFiles own_files() const;
    // The saved server this login is on (servers/<profile>/server.ini, read once for each profile); nullopt without
    // one.
    std::optional<ServerProfile> saved_server() const;

    // ---- merchants, NPC dialogue and trade (net/commerce.h; net/game_connection_commerce.cpp) ----
    // A right click on a merchant NPC (RightClickedOnPlayer, class 41, within
    // 20 units): OP_ShopRequest. The server's answer opens the window and
    // sends the stock as item packets of type 0x64.
    void open_merchant(std::uint32_t npc_id);
    // CMerchantWnd RequestGetItem: buy `quantity` of the merchant slot's item.
    void merchant_buy(std::uint32_t merchant_slot, std::uint32_t quantity);
    // RequestPutItem: sell `quantity` of the inventory item at `slot`.
    void merchant_sell(const ItemSlot& slot, std::uint32_t quantity);
    // Picks an inventory item for selling (an inventory click while the merchant is open).
    void merchant_select(const ItemSlot& slot);
    // EQEmu's parcels at the open parcel merchant (MerchantSession::parcels): take one (OP_ShopRetrieveParcel), or
    // all of them one after another, each asked for when the server has answered the last (Hailstone's Claim all).
    void retrieve_parcel(std::uint32_t slot);
    void claim_all_parcels();
    // Sends an item from the bags (`quantity` of a stack) or money (platinum, gold, silver, copper) by parcel
    // (OP_ShopSendParcel; MerchantMailPageHandler_RequestPutItem). Returns why not, empty when sent.
    std::string send_parcel(const ItemSlot& slot, std::uint32_t quantity, const std::string& to, const std::string& note);
    std::string send_parcel_money(const std::array<std::uint32_t, 4>& coins, const std::string& to, const std::string& note);
    // Drops that pick (a click on one of the merchant's rows considers that item instead).
    void merchant_unselect() {
        if (merchant_) merchant_->selected.reset();
    }
    // 0x698290, which CInventoryWnd::ClickedMoneyButton calls just before its quantity dialog (no coin
    // on the cursor) while the merchant window shows its mail (Parcels) page; the caller checks the page.
    // CInvSlotMgr::SelectSlot(0): the item picked in the bags is let go and the coin buttons' outline cleared; then
    // with `have` > 0 coins of that type, coin button `coin` (0 platinum .. 3 copper) is outlined (0x6909a0).
    // (The merchant window's own item and list selection, 0x6f2120, are the window model's.)
    void parcel_coin_button(int coin, std::int32_t have);
    // Hailstone (a deliberate difference): the quantity dialog's answer after parcel_coin_button is the money picked
    // for sending (MerchantSession::parcel_money), which the page's Send money sends; it stays in the inventory. The
    // original puts it on the cursor, for the player to drop on its SendMoneyWnd, which this client does not have.
    void set_parcel_money(int coin, std::uint32_t amount);
    void close_merchant();
    const std::optional<MerchantSession>& merchant() const { return merchant_; }
    // /hail and the HAIL key (Hail @0x4f3010): says "Hail, <target>" (eqstr 13085), or "Hail".
    void hail();
    // A left click on a spawn while holding an item or coin
    // (CEverQuest::DropItemOrMoneyOnPlayer): starts a trade or give, or drops
    // the cursor item into the open one.
    void give_to(std::uint32_t spawn_id);
    // CTradeWnd::DropItemIntoTrade: the cursor item into the first free trade slot.
    void trade_drop_cursor();
    // Puts coin of one type (0 copper .. 3 platinum) into the trade (through the cursor, as the original).
    void trade_coin(int type, std::uint32_t amount);
    // The Trade / Give button (OP_TradeAcceptClick) and Cancel.
    void trade_accept();
    void trade_cancel();
    const std::optional<TradeSession>& trade() const { return trade_; }

    // ---- bank (game/bank.h; net/game_connection_bank.cpp) ----
    // A right click on a banker (RightClickedOnPlayer: class 40, no banker
    // active, within 20 units): CBankWnd::Activate. The window is the
    // client's own; no packet opens or closes it. It closes itself every
    // 500 ms check that finds the banker gone or out of reach.
    void open_bank(std::uint32_t banker_id);
    // CBankWnd::Activate itself, without the right click's checks (class 40, within 20 units): any spawn, the
    // player too, as a plugin asks (the NMS patch's vault Bank button activates it with the local player). False
    // when out of a zone, dead, or `banker_id` is no spawn; an open window stays as it is.
    bool activate_bank(std::uint32_t banker_id);
    void close_bank();
    const std::optional<BankSession>& bank() const { return bank_; }
    // Hailstone: the bank as last seen (game/bank_snapshot.h). bank_live(): the inventory holds the bank as the server
    // has it in this zone (it came with OP_CharInventory or in item packets, or the bank window opened); until then
    // the bank's slots are empty in inventory() and bank_seen() is the copy to list in their place (seen 0: never
    // seen). The copy is never in inventory(), so nothing can move, use or count what it holds.
    bool bank_live() const { return bank_live_; }
    const game::BankSnapshot& bank_seen() const { return bank_seen_; }
    // Changes when bank_live() or bank_seen() does (a window's cache).
    std::uint64_t bank_view_revision() const { return bank_view_revision_; }
    const game::BankCoins& bank_coins() const { return bank_coins_; }
    // Coin on the cursor by wire type (0 copper .. 3 platinum).
    const std::array<std::uint32_t, 4>& cursor_coins() const { return cursor_coins_; }
    // CEverQuest::MoveMoney: moves coin between locations (kCoinCursor,
    // kCoinInventory, kCoinBank, kCoinSharedBank) with the original's
    // conversion, updates the local piles and sends OP_MoveCoin. Returns
    // false when refused (nothing sent).
    bool move_coin(int from, int to, int from_type, int to_type, std::uint32_t amount);
    // CInventoryWnd::ClickedMoneyButton(coin, amount) (game::clicked_money_button; coin 0 platinum .. 3 copper,
    // amount -1 for a click): cursor coins go into the inventory as that coin, or `amount` coins come onto the
    // cursor; for AskQuantity the caller opens the quantity dialog (max = start = inventory_coin(coin), use_keys,
    // min 0, at the button's centre, x / y of the result) and calls this again with the answer.
    game::MoneyButtonOut inventory_money_button(int coin, std::int32_t amount, const std::array<std::int32_t, 4>* button_rect);
    // The inventory's coins of a type (coin 0 platinum .. 3 copper; CInventoryWnd 0x6919e0).
    std::int32_t inventory_coin(int coin) const;
    // The bank window's coin, as the original moves it: through the cursor
    // (inventory money button → cursor → bank money button and back).
    // `shared`: the shared bank, whose one money button is platinum.
    void bank_deposit(int type, std::uint32_t amount, bool shared);
    void bank_withdraw(int type, std::uint32_t amount, bool shared);
    // The bank's Auto button (CBankWnd_AutoBankCursor): cursor coin, or the
    // cursor item into a matching stack, a free bank slot or a bank bag.
    void auto_bank();

    // ---- groups and pets (game/group.h; net/game_connection_group.cpp) ----
    const game::GroupState& group() const { return group_; }
    const game::PetState& pets() const { return pets_; }
    // Hailstone's support for several pets: the server's list of the player's pets, from its extension (NMS:
    // OP_PetList). The focused pet stays when it is still listed, else the first one becomes it.
    void set_pet_list(std::vector<game::PetInfo> list);
    // /invite [name] (Cmd_invite): accepts a pending invitation, else invites
    // the name or the current target (DoGroupInvite).
    void group_invite(const std::string& name = {});
    void accept_group_invite();
    // /disband (Cmd_disband): declines a pending invitation, else leaves the
    // group, or (leader) removes the targeted member / disbands.
    void decline_group_invite();
    void group_disband();
    void group_make_leader(const std::string& name = {});
    // /pet WORDS (cmd_pet) and the pet window's buttons (IssuePetCommand).
    void pet_command_words(const std::string& args);
    void pet_command(std::uint32_t command, std::uint32_t target = 0);
    // Focuses one of several pets (NMS): targets it; the server makes it the
    // pet the commands go to (Handle_OP_TargetMouse → ConfigurePetWindow).
    void focus_pet(std::uint32_t spawn_id);
    // The pet the native pet window shows (the local player's PetID), set by the client alone: the NMS add-on's
    // /petcycle (MultiPet::CyclePet) writes it without telling the server.
    void set_window_pet(std::uint32_t spawn_id) {
        pets_.focused = spawn_id;
        update_pet_presence();
    }
    // The pet window's side of cmd_pet and of the pet's arrival (CPetInfoWnd; app/lua_eq_misc.cpp's PetWindow links
    // itself to each new connection, as InitGameUI makes the window again).
    struct PetWindowLink {
        // CharacterZoneClient_CanUsePetCommand for the pet now there (ReissueToggledPetCommands).
        std::function<bool(std::uint32_t command)> usable;
        // CPetInfoWnd_GetPetCommandAvailable (cmd_pet sends a command only when it holds).
        std::function<bool(std::uint32_t command)> available;
        // CPetInfoWnd_SaveButtonToggleState: toggle command index 4..13's byte was set by a /pet word.
        std::function<void(int index, bool on)> saved;
    };
    // Until it is linked there is no window (pinstCPetInfoWnd null: RefreshPetState is not run); linking runs its first
    // refresh, which re-sends the toggles to a pet already there.
    void link_pet_window(PetWindowLink link) {
        pet_window_ = std::move(link);
        update_pet_presence();
    }
    bool pet_window_linked() const { return static_cast<bool>(pet_window_.saved); }
    // The window's toggle bytes (game::PetToggleStates, by PET_BUTTON_*): the UI ini's, then cmd_pet's toggle words
    // and the server's OP_PetCommandState. They stay over zoning, as the window does.
    const game::PetToggleStates& pet_toggles() const { return pet_toggles_; }
    void load_pet_toggles(const game::PetToggleStates& states) { pet_toggles_ = states; }
    // The NMS add-on's extra pet gauges (PetWindow::GetVisiblePets): the server's pet list (empty before an
    // OP_PetList) without the window's pet and without pets whose spawn is not here, in the list's order.
    std::vector<std::uint32_t> pet_window_extra_pets() const;
    // The focused pet's buffs (OP_PetBuffWindow): slot, buff.
    const std::vector<std::pair<std::uint32_t, game::BuffSlot>>& pet_buffs() const { return pet_buffs_; }

    // ---- doors and ground objects (game/doors.h; net/game_connection_doors.cpp) ----
    // The zone's switches (OP_SpawnDoor), animated by update() as the
    // original's CDisplay_ProcessSwitches does every frame.
    const std::vector<game::Door>& doors() const { return doors_; }
    const game::Door* door(std::uint8_t id) const;
    const std::vector<game::GroundObject>& ground_objects() const { return ground_objects_; }
    // CEverQuest::ClickedSwitch -> EQSwitch::UseSwitch: refuses unusable or
    // moving switches, says "You can't reach that, get closer." out of reach
    // (actor_radius: the door model's bounding radius, scaled), else sends
    // OP_ClickDoor with `key` (the item id, -1 for none) and `skill` (the
    // adjusted Pick Lock or Disarm Traps value, game/clicks.h clicked_switch);
    // OP_MoveDoor answers. Returns true when the click was sent.
    bool click_door(std::uint8_t door_id, float actor_radius, std::int32_t key = -1, std::int32_t skill = 0);
    // Whether the local player is within reach of the door for a click (UseSwitch's test, as click_door makes it).
    bool door_in_reach(std::uint8_t door_id, float actor_radius) const;
    // EQSwitch::UseSwitch(switch, user, key, skill, 0) as PlayerClient::TouchingSwitch calls it: `user` is the
    // spawn id; 0 (nobody, type 0x23) skips the reach test and sends player id 0.
    bool use_switch(std::uint8_t door_id, std::uint32_t user, float actor_radius, std::int32_t key, std::int32_t skill = 0);
    // TouchingSwitch's close of a type-0x32 switch past its repop time: the state byte (+0x27) = 0, without
    // ChangeState (no sound, nothing sent).
    void set_door_state_now(std::uint8_t door_id, std::uint8_t state);
    // CEverQuest::LMouseUp on a ground item: OP_ClickObject when it is under
    // 20 units away. Items come to the cursor; tradeskill containers open
    // (world_container()).
    bool click_object(std::uint32_t drop_id);
    const std::optional<game::WorldContainer>& world_container() const { return world_container_; }
    // Closes the open tradeskill container (an empty OP_ClickObjectAction).
    void close_world_container();
    // The tradeskill window's Experiment on a world container (CContainerMgr::OpenExperimentContainer): the container
    // in its own window, the tradeskill window closed.
    void show_world_container_window();
    // Combines the open container's contents (OP_TradeSkillCombine).
    void combine_world_container();
    // CEverQuest::LMouseUp on the ground with an item on the cursor
    // (game/cursor_drop.h): a NoGround item says 7697; NoSafeDrop 1 drops at
    // once (announced, 9058), -1 says 12320, anything else returns true: the
    // UI asks "ARE YOU SURE you wish to drop your %1 on the ground?" (9057,
    // drop_confirm_text()) and a yes calls drop_cursor_on_ground().
    bool request_drop_cursor();
    std::string drop_confirm_text() const;
    // CEverQuest::DropHeldItemOnGround(announce): the cursor item leaves and
    // an empty OP_GroundSpawn (0x6fca) goes out; announced says 9058.
    // Returns true when sent.
    bool drop_cursor_on_ground(bool announce = false);
    // NoSafeDrop (the FastDrop option, /fastdrop): 1 drop at once, -1 never, else confirm.
    std::int32_t no_safe_drop() const { return client_options_.get(game::ClientOption::FastDrop); }
    std::uint32_t door_moves_seen() const { return door_moves_seen_; } // OP_MoveDoor count (probes)

    // ---- skills and abilities (app/actions_ui.cpp, net/ability_packets.h) ----
    // Sends a zone packet the abilities code built (UseSkill's requests).
    void send_zone_packet(const AppPacket& p) { send_zone(p); }
    void send_world_packet(const AppPacket& p) { send_world(p); }
    // What plugins and scripts may do on this server (plugin/policy.h: its hailstone.cfg, or no rules), set once the
    // zone-in has checked the server's files and kept while in the world; the serial changes with it.
    const std::optional<plugin::Rules>& server_rules() const { return server_rules_; }
    std::uint32_t server_rules_serial() const { return server_rules_serial_; }
    // Zone packets the abilities code reads (OP_SpawnAppearance, OP_CancelSneakHide, OP_SkillUpdate, OP_Taunt,
    // OP_Begging, OP_PickPocket, OP_Track, OP_FeignDeath), oldest first; OP_SkillUpdate also updates the profile.
    std::optional<AppPacket> poll_ability_packet();

    // ---- progress: AAs, training, tradeskills, tasks (net/game_connection_progress.cpp) ----
    // Alternate advancement: the ranks the server sent, the owned ranks, points, AA experience and
    // percentage, the reuse timers (game/alt_adv.h).
    const game::AltAdvState& alt_adv() const { return alt_adv_; }
    // The rank's name and description (dbstr types 1 and 4).
    std::string aa_name(const game::AltAbility& a) const;
    std::string aa_description(const game::AltAbility& a) const;
    // The AA window's Category column: the category (dbstr type 0x13), else the expansion (0x14); empty for neither.
    std::string aa_category(const game::AltAbility& a) const;
    // AltAdvManager::CanTrainAbility for the player.
    bool aa_can_train(std::uint32_t rank_id) const;
    // The AA window's list for a tab (type 1..4): the visible ranks in ascending id order that its filter accepts
    // (CAAWnd_FilterAccepts, game/alt_adv.h).
    std::vector<const game::AltAbility*> aa_list(std::int32_t type, const game::AAFilter& filter) const;
    // AltAdvManager::IsAbilityReady (SPA 264 reduces the reuse time).
    game::AAReadiness aa_readiness(std::uint32_t rank_id) const;
    // The item recast timers, in Util::FastTime seconds (time(0),
    // as the server's ItemSerializationHeader last_cast_time): the item's own (ItemBase +0x34, net::Item
    // last_cast_time) and the profile's 100 by recast type (PcBase +0x6a5c, PcBase_Get/SetItemRecastTimer).
    // PcZoneClient::GetItemRecastTimer @0x580600: the later (unsigned) of the item's own and, for a recast type
    // 0..99 (an unsigned compare: -1 has none), that type's; 0 without an item.
    std::uint32_t item_recast_timer(const Item* item) const;
    // PcZoneClient_SetItemRecastTimer @0x5805b0: now + delay into the item's own (+0x30 marked changed) and, for a
    // type 0..99, the type's.
    void set_item_recast_timer(Item* item, std::int32_t delay, std::int32_t type);
    // PcZoneClient_ResetAllItemRecastTimers @0x583640 (opcode 0x73ac, which EQEmu never sends): the own timers of
    // the 34 possessions slots' items and of the items directly in the bags there, then the 100 type timers.
    void reset_all_item_recast_timers();
    std::uint32_t item_recast_type_timer(std::int32_t type) const {
        return static_cast<std::uint32_t>(type) < item_recast_timers_.size() ? item_recast_timers_[static_cast<std::size_t>(type)] : 0;
    }
    // CharacterBase vtable +0 GetGameFeature (PcClient::GetGameFeature_ @0x5784e0): the membership's entry
    // (OP_SendMembership; FreeToPlayClient +0xc + 4 x feature). Before OP_SendMembership the original reads
    // uninitialised memory; here -1 (no limit), what EQEmu sends for the features the client reads this way.
    std::int32_t game_feature(std::int32_t feature) const;
    // The player spawn's GM flag (PlayerBase vtable +0xdc; false without the spawn).
    bool player_is_gm() const {
        const game::Spawn* me = world_.player();
        return me && me->gm;
    }
    // 0x7b28a0 with the features ProcessAllStats passes (UseItem 0x13, Ornament 0x15) over the worn slots
    // 0..0x15 (game/item_stats_disabled.h), as ProcessAllStats' first loop runs it.
    void refresh_worn_stats_disabled();
    // The cast's item location (LaunchSpellData +0xc) as a slot.
    ItemSlot cast_item_slot() const;
    // The item preview (CObjectPreviewWnd, game/item_preview.h): the window, and what the last reply showed.
    struct ItemPreview {
        bool open = false;          // the window is shown
        bool show = false;          // the view and its buttons; else the "No item preview available." label
        bool on_player = false;     // on a copy of the player (preview_appearance), else the item's own model
        std::optional<Item> item;   // the reply's item
        std::uint32_t serial = 0;   // counts the replies (and the requests without an item)
    };
    const ItemPreview& item_preview() const { return item_preview_; }
    // Send_OP_ItemPreviewRequest @0x6fff10 for an item (null: the view hides, nothing is sent) and the window shown;
    // @0x6ffc20 by item id (0: the same as a null item).
    void preview_item(const Item* item);
    void preview_item_id(std::uint32_t item_id);
    void set_item_preview_open(bool open) { item_preview_.open = open; }

    // ---- the marketplace (CMarketplaceWnd; game/marketplace.h; net/game_connection_marketplace.cpp) ----
    struct Marketplace {
        std::vector<MarketplaceEntry> catalog; // CEverQuest +0x39584 (0x1e9f, a full replace; kept all session)
        std::vector<MarketplaceFund> funding;  // CEverQuest +0x3958c (0x09bf)
        game::MarketplaceWindow window;        // the rows, categories, view and selection
        bool open = false;                     // the window is shown
        bool built = false;                    // the window's constructor ran (its sends) since character select
        std::uint32_t wallet = 0xffffffffu;    // wnd +0x23c (game::kMarketplaceFundsUnknown until 0x4b7f)
        bool zip_on_file = false;              // wnd +0x240 (0x4b7f's byte, 0x3171)
        bool busy = false;                     // the window and its children disabled (processing, a result)
        bool dialog = false;                   // wnd +0x315: one of the window's popups is up
        ItemPreview preview;                   // the preview page (0xb07f)
        bool preview_pending = false;          // wnd +0x268: a request went (CMarketplaceWnd_SendItemPreview)
        std::string result_text;               // MKPW_Results_Result
        std::string find_text;                 // MKPW_FindEditBox (never cleared by the window)
        std::string zip_text;                  // MKPW_TaxInfoZipCodeEdit
        std::int64_t processing_ms = 0;        // wnd +0x328: when Confirm went (now_ms)
        std::int64_t ping_ms = 0;              // wnd +0x320: the last 0x5bd5 while shown
    };
    Marketplace& marketplace() { return marketplace_; }
    const Marketplace& marketplace() const { return marketplace_; }
    // Show / hide (Cmd_marketplace, the EQ main window's SC button, the key, the close box, Esc): a show runs
    // AboutToShow @0x6e35d0 (a stale catalog repopulated, ResetView, Ready 0x356f), a hide AboutToHide.
    void set_marketplace_open(bool open);
    // OpenToEntry @0x6dcc00: shown, then that entry's shelf and details.
    void marketplace_open_to_entry(std::uint32_t entry_id);
    void marketplace_click_category(std::uint32_t id);  // a tree row
    void marketplace_select_row(int row);                // a list row or a grid tile (refused for state 2)
    void marketplace_select_entry(std::uint32_t entry_id); // a landing tile (HandleLButtonUp 0x6dd120)
    void marketplace_find();                             // FindBtn / Enter (find_text)
    void marketplace_sort(int choice);
    void marketplace_set_view(game::MarketplaceView v);  // IconBtn / ListBtn
    // QtyEdit (Enter or a new value: SetQuantity(atoi(text))); Qty Up / Down (only on the main page).
    void marketplace_set_quantity_text(const std::string& text);
    void marketplace_step_quantity(int delta);
    void marketplace_buy();                              // MKPW_BuyBtn (0x6e2356)
    void marketplace_tax_next();                         // MKPW_TaxInfoNextButton (0x6d6010; zip_text)
    void marketplace_confirm();                          // MKPW_ConfirmPurchase_ConfirmBtn (0x6e2faf): 0x4a74
    void marketplace_cancel();                           // MKPW_CancelBtn and MKPW_Results_OKBtn (0x6e3420)
    void marketplace_inspect(bool shift);                // MKPW_DetailsInspect (0x6d2d00)
    void marketplace_preview_window();                   // MKPW_DetailsPreview (0x6d2db0): the object preview window
    void marketplace_item_of_week();                     // MKPW_ItemOfWeekBuyNowBtn: NavigateToEntry
    // The buttons as UpdateButtonStates and OnProcessFrame leave them (false while busy).
    bool marketplace_buy_enabled() const;
    bool marketplace_confirm_enabled() const;
    bool marketplace_quantity_up_enabled() const;
    bool marketplace_quantity_down_enabled() const;
    // OnProcessFrame's timers (update() calls it with now_ms()): the 30 s 0x5bd5 while shown, the 60 s give-up on
    // the results page.
    void marketplace_frame(std::int64_t now);
    // LaunchSpellData_IsCastItemValid @0x5a1f00: the item at the cast's location when the cast is an item cast
    // (is_item_cast) and that item is Type 0 with the cast's item id (+0x460); else null.
    Item* valid_cast_item();
    // LaunchSpellData_GetCastItem @0x59fcf0: the item at the cast's location when it is an item cast, else null.
    Item* cast_item();
    // The AA window's train button / /alternateadv buy: OP_AAAction 3 when trainable, else eqstr 13277.
    // `train_all`: the AA window's "Train All" box (NMS UI); the NMS patch then sends action 4 (buy all ranks). On a
    // server without action 4 the client buys the ranks one at a time itself (continue_aa_buy_all).
    bool aa_train(std::uint32_t rank_id, bool train_all = false);
    // The AA window's Train button (CAAWnd::WndNotification @0x60aa80): with AANoConfirm it trains at once, else it
    // asks first (4041 / 4042 with the cost and the name, 20 s).
    void aa_train_button(std::uint32_t rank_id, bool train_all = false);
    // /alternateadv act: activates the owned rank of the ability (group id): OP_AAAction 0 with the
    // target; eqstr 6198 when not owned.
    bool aa_activate(std::uint32_t group);
    // The AA window's experience slider (Send_OP_AAAction_SetExpPercent): 1..100 sends action 1, 0
    // turns AA experience off (action 2).
    void set_aa_percent(std::uint32_t percent);
    // The owned ranks' effects for the character sheet (TotalEffect's AA part).
    std::vector<game::mechanics::SheetAbility> aa_sheet_abilities() const { return alt_adv_.sheet_abilities(); }
    void aa_sheet_abilities_into(std::vector<game::mechanics::SheetAbility>& out) const {
        alt_adv_.sheet_abilities_into(out);
    }

    // Training at a guildmaster (game/training.h). A right click on a guildmaster (class 20..35):
    // eqstr 1467 for another class's guild, else OP_GMTraining; the answer opens training().
    bool open_trainer(std::uint32_t npc_id);
    const std::optional<game::TrainingSession>& training() const { return training_; }
    // Send_OP_GMTrainSkill: trains the row (index into training()->rows) after the original's checks.
    bool train(std::size_t row);
    // The cost in copper of training the row one point (0x584a30).
    std::int32_t train_cost(std::size_t row) const;
    // The window's Done / closing: OP_GMEndTraining.
    void close_trainer();
    // The trained value of a skill (0..99) or language (100 + n); the skill's name (eqstr).
    std::int32_t skill_value(std::uint32_t skill) const;
    std::string skill_name(std::uint32_t skill) const;
    // The practice points (profile +0x33dc).
    std::int32_t practice_points() const { return profile_ ? static_cast<std::int32_t>(profile_->points) : 0; }

    // Tradeskills: CContainerWnd::HandleCombine for a bag in a possessions slot (0..33; slot
    // `bag_slot`) with the original's container, cursor and stack checks; OP_TradeSkillCombine.
    bool combine_container(std::int16_t bag_slot);
    // An OP_TradeSkillCombine waits for the server's acknowledgement.
    bool combine_pending() const { return combine_pending_; }
    // The tradeskill window (game/tradeskill.h): opened with a tradeskill bag or the world container (its
    // favourites are asked for at once), favourites and searches (OP_RecipesFavorite / OP_RecipesSearch), a
    // recipe's components (OP_RecipeDetails) and the auto combine (OP_RecipeAutoCombine).
    const game::TradeskillState& tradeskill() const { return tradeskill_; }
    const game::RecipeFavorites& recipe_favorites() const { return recipe_favorites_; }
    void open_tradeskill_bag(const ItemSlot& bag_slot);
    // A right click opening the bag at `bag_slot` (CInvSlot::HandleRButtonUp → CContainerMgr::OpenContainer @0x65cae0,
    // game::opens_in_tradeskill_window): true when it opens in the tradeskill window rather than its own, by the
    // TradeskillCtrlClick option (CtrlBypass) and whether ctrl is held. May say why the character cannot use it.
    bool container_opens_tradeskill(const ItemSlot& bag_slot, bool ctrl);
    void close_tradeskill();
    void tradeskill_show_favorites();
    void tradeskill_search(std::uint32_t min_trivial, std::uint32_t max_trivial, const std::string& text);
    void tradeskill_select(std::uint32_t recipe_id);
    void tradeskill_favorite(std::uint32_t recipe_id, bool add);
    bool tradeskill_combine();
    // Hailstone: the selected recipe's components against what the character carries (game::recipe_needs; empty
    // until the details came).
    game::RecipeNeeds tradeskill_needs() const;
    // Hailstone: Combine N / Combine All (game::CombineBatch; `count` 0 all): the auto combine again and again, each
    // result put away first (game::put_away_slot), at the window's own pace (kCombineWaitMs). Empty when it started,
    // else why not (said in chat too; batch_refusal() asks without trying). It stops itself when done, out of components, out of room, or when the window, the recipe or
    // the cursor changes under it (said in chat).
    std::string tradeskill_start_batch(std::uint32_t count);
    void tradeskill_stop_batch(const std::string& why = "you stopped it");
    std::string batch_refusal() const;

    // Tasks and quests (game/tasks.h).
    const game::TaskState& tasks() const { return tasks_; }
    // Stock Select Reward: received data, separate pending and preview managers. Confirmations
    // carry the collection revision, local tab token and exact received wire option ID.
    const game::RewardSelection& rewards() const { return rewards_; }
    const game::Achievements& achievements() const { return achievements_; }
    bool achievement_refresh_pending() const { return achievement_refresh_token_.has_value(); }
    const std::string& achievement_message() const { return achievement_message_; }
    void show_achievements(bool show);
    void close_achievement_detail();
    void reopen_achievement_detail();
    bool open_achievement(std::uint64_t catalog_token, std::uint32_t definition_index);
    bool request_achievements();
    bool compare_achievements();
    void clear_achievement_comparison();
    bool inspect_achievement_link(std::string_view metadata);
    bool achievement_reward(std::uint64_t catalog_token, std::uint32_t definition_index);
    std::optional<std::string> achievement_chat_link(std::uint64_t catalog_token, std::uint32_t definition_index) const;
    void show_reward_selection(bool show);
    void close_reward_selection(game::RewardLane lane);
    bool request_pending_rewards();
    bool preview_task_reward(std::uint32_t task_id);
    bool reward_claim(std::uint64_t revision, std::uint64_t tab_token, std::uint32_t wire_option_id,
                      std::string* why = nullptr);
    bool reward_inspect(game::RewardLane lane, std::uint64_t revision, std::uint64_t tab_token,
                        std::uint32_t wire_option_id, std::uint32_t entry_id, std::uint32_t item_id,
                        std::string* why = nullptr);
    // The large centre-screen text (CTextOverlay: OP_TaskActivityComplete's lines); the app draws its frame().
    game::TextOverlay& text_overlay() { return text_overlay_; }
    // The task select window's Accept for one offered task (OP_AcceptNewTask, or
    // OP_SharedTaskAcceptNew for a shared task selection); Decline sends task id 0.
    void accept_task(std::uint32_t task_id);
    void decline_tasks();
    // Abandons a task (OP_CancelTask).
    void cancel_task(std::uint32_t type, std::uint32_t sequence);
    // The task window's Remove (TASK_AbandonButton): the question (game::task_abandon_eqstr, 10 s, the task id) for
    // the task in (type, sequence); a yes abandons it when the window still has that task selected
    // (CTaskWnd::ConfirmAbandonTask @0x76fd50: OP_CancelTask, then the button is disabled until the list is built
    // again). The window tells the connection its selection (and the button's state) through these.
    void ask_abandon_task(std::uint32_t type, std::uint32_t sequence);
    void set_task_window_selection(std::optional<std::pair<std::uint32_t, std::uint32_t>> type_sequence) {
        task_selection_ = type_sequence;
    }
    // ConfirmAbandonTask disabled Remove; the next list build enables it again (take_ clears the flag).
    bool take_task_abandon_sent() { return std::exchange(task_abandon_sent_, false); }
    bool task_abandon_sent() const { return task_abandon_sent_; }
    // Asks for a completed task's elements (OP_TaskHistoryRequest; index into the history list). The window shows a
    // history entry the server already sent from tasks().history without asking again (bElementsReceived).
    void request_task_history(std::uint32_t index);
    // The task compass mark (0x76ff70, game::task_compass_mark): looked for again after each task change and
    // zone entry; the compass shows it with the other points.
    const std::optional<game::TaskCompassMark>& task_compass() const { return task_compass_; }
    // CTaskManager::GetElementDescription.
    std::string task_element_text(const game::TaskElement& e) const;
    // The shared task commands (/taskaddplayer, /taskremoveplayer, /taskmakeleader and the task window's buttons):
    // OP_SharedTaskAddPlayer / RemovePlayer / MakeLeader with the name; the server checks the leader and the task.
    void shared_task_add_player(const std::string& name);
    void shared_task_remove_player(const std::string& name);
    void shared_task_make_leader(const std::string& name);
    // We lead the shared task (the member list's leader): CTaskWnd_FillMemberList enables the leader's buttons.
    bool shared_task_is_leader() const;
    // The task window's shared task page (CTaskWnd::WndNotification @0x7716b0): Add Player adds the target, a player
    // (else 0x230e); Remove Player and Make Leader take the one selected member (else 0x230f / 0x2310, colour 0xf);
    // Invite Raid adds the raid's members and Invite Expedition the expedition's that are not in the task, both only
    // while the task has members.
    void shared_task_add_target();
    void shared_task_remove_selected(const std::optional<std::string>& member);
    void shared_task_leader_selected(const std::optional<std::string>& member);
    void shared_task_invite_raid();
    void shared_task_invite_expedition();

    // ---- chat, social commands, guilds and raids (game/social.h; net/game_connection_social.cpp)
    void say(const std::string& text);
    // CEverQuest::InterpretCmd @0x51fce0: a typed line. Slash words run the
    // original's command table (game/slash_commands.h, prefix matching in
    // table order); ';' and '[' lines go to the chat server; anything else is
    // said on the default channel.
    void send_command(const std::string& line);
    // A client message by eqstr id (the system colour), as the key commands print them.
    void system_message(std::uint32_t eqstr, std::uint32_t color = 0) { system(eqstr, {}, color); }
    const game::SocialState& social() const { return social_; }
    // The reply keys' place in last_tells (0xde21d8; game::cycle_last_tell steps it).
    int& last_tell_index() { return social_.last_tell_index; }
    // The user gave input this frame (keys, mouse, pad, typing): the AFK keepalive's activity (game/social.h).
    void note_user_activity() { afk_keepalive_.last_input_ms = now_ms(); }
    const game::GuildState& guild() const { return guild_; }
    // LocalPC_HasGuildPermission @0x4241f0 (game/guild_ranks.h): a GM always; else the rank's bit of the table
    // OP_GuildUpdate action 5 fills.
    bool has_guild_permission(std::int32_t permission) const;
    const game::RaidState& raid() const { return raid_; }
    // The last OP_WhoAllResponse as it came (its lines went to the chat window) and how many have come: the
    // gamepad pack's Who window lists it (a client extension: RoF2 has only the chat lines).
    const WhoResponse* last_who() const { return last_who_ ? &*last_who_ : nullptr; }
    std::uint32_t who_count() const { return who_count_; }
    // A member's public note (Send_OP_GuildPublicNote; the window checks permission 12, the server too).
    void guild_set_public_note(const std::string& member, const std::string& note);
    // Send_OP_GuildManageBanker(member, 2, on): a member's alt flag (yourself, or a GM / permission 0x1a; 516
    // otherwise).
    void guild_set_alt(const std::string& member, bool on);
    // CGuildMgmtWnd's OP_GuildUpdate: the web page (action 0) and chat channel (1), sent when they change and kept;
    // a rank's name (4) and permission (5).
    void guild_set_url(const std::string& url);
    void guild_set_channel(const std::string& channel);
    void guild_rename_rank(std::uint32_t rank, const std::string& name);
    void guild_set_rank_permission(std::uint32_t rank, std::uint32_t permission, bool on);
    // CGuildMgmtWnd::WndNotification's rank combo box on the Access page (a member's new rank): a higher rank with
    // permission 8 sends OP_GuildPromote {member, own name, guild id, rank}; a lower one, with permission 9 (0x1e for
    // yourself), OP_GuildDemote {own name, member, rank} (0x84 B); otherwise nothing (false: the box goes back).
    bool guild_change_member_rank(const std::string& member, std::uint32_t rank);
    // The guild invitation's Accept / Decline (OP_GuildInviteAccept; OP_HailstoneGuild for one from another zone).
    void answer_guild_invite(bool accept);
    // The zone answered OP_HailstoneGuild's hello: /guildinvite reaches players in other zones (net/social_packets.h).
    bool guild_world_invites() const { return guild_world_invites_; }
    // /raidinvite [name] (Cmd_raidinvite @0x4de470): invites the name or, without one, the targeted player.
    void raid_invite(const std::string& args);
    // CRaid::SendInviteResponse @0x47d700: /raidaccept and /raiddecline.
    void answer_raid_invite(bool accept);
    // /raiddisband and the raid window's Leave: leave the raid, or for its leader remove the raid target (OP_RaidInvite
    // action 5).
    void raid_leave();
    // The raid leader's commands, as CRaid sends them (net/social_packets.h): CRaid_SendLockRaid @0x47dbd0 / SendUnlockRaid
    // @0x47dc00 (the leader of an unlocked / locked raid), CRaid_SendMoveGroup @0x47db70 (the leader: group 0..11, -1
    // out of the groups), CRaid_SendRemoveGroupLeader @0x47dba0 (the leader), CRaid::SendLeadershipChange @0x47d820
    // (the leader: an empty name hands the raid to the raid target; a name not in the raid prints 5082),
    // CRaid::AddRaidLooter @0x47da60 / RemoveRaidLooter @0x47db00 (the raid target, for the leader with loot type 3;
    // else 5095 / 5096), CRaid::SetLootType @0x47d9b0 (/setloottype: one digit 1..4 other than the current type,
    // for the leader; else 5094), Send_OP_RaidInvite_0047c300 (the MOTD, 1164 bytes).
    void raid_lock(bool lock);
    void raid_move_group(const std::string& member, std::int32_t group);
    void raid_remove_group_leader(const std::string& member);
    void raid_send_leadership_change(const std::optional<std::string>& name);
    void raid_add_looter();
    void raid_remove_looter();
    void raid_set_loot_type(const std::string& text);
    void raid_set_motd(const std::string& motd);
    // The raid window's note Update: a member's note for the raid (OP_RaidInvite 0x24, encode_set_raid_note).
    void raid_set_note(const std::string& member, const std::string& note);
    // CRaid::SetTargetRaidPlayer @0x47cd80: a raid member becomes the raid target (and the target, as the original's
    // /target by name); an empty name clears it.
    void set_raid_target(const std::string& name);
    // Send_OP_RaidDelegateAbility @0x55e5b0: the raid window's Main Assist / Mark NPC buttons (abilities 3 / 4) for
    // the raid target; no member: 0x222c (colour 0xd).
    void raid_delegate_ability(std::uint32_t ability, const std::string& member);
    // The raid window's Find PC (Leadership_FindPathToPlayer on the raid target).
    void raid_find_path_to_target();
    // RaidOptionsWindow's open flag.
    struct RaidOptions {
        bool open = false;
    };
    const RaidOptions& raid_options() const { return raid_options_; }
    void set_raid_options_open(bool open) { raid_options_.open = open; }
    // /follow: the spawn to follow (0 none); the app runs PlayerClient::FollowPlayerAI (game/follow.h).
    std::uint32_t follow_target() const { return social_.follow_id; }
    void stop_follow(bool announce = true);
    // FollowPlayerAI's end: `eqstr` (12692 dead, 12693 too far) with the followed name, then TurnOffAutoFollow.
    void stop_follow_with(std::uint32_t eqstr);
    // The chat filters (eqclient.ini [Options] Channel00.., BadWord, Spam ...; game/chat_text.h).
    const game::ChatFilters& chat_filters() const { return chat_filters_; }
    void set_chat_filter(int filter, std::int32_t value) { chat_filters_.set(filter, value); }
    // The Options window's chat filters: the setting changed, kept in eqclient.ini [Options] and sent to the zone
    // (OP_SetServerFilter: EQEmu leaves out what the filters hide, Client::ServerFilter).
    void change_chat_filter(int filter, std::int32_t value);
    // The Options window's game options (game/client_options.h): the character's options.ini over eqclient.ini's.
    const game::ClientOptions& client_options() const { return client_options_; }
    bool client_option(game::ClientOption id) const { return client_options_.on(id); }
    // Sets an option as the Options window does: kept in options.ini, then acted on (apply_client_option: the
    // original's COptionsWnd page handlers, e.g. the anonymous flag's spawn appearance and the chat line).
    void change_client_option(game::ClientOption id, int value);
    // Hailstone: the layout copy window's Chat and Options boxes (app/lua_eq.cpp copy_layout). What another
    // character's chat.ini (filters, user colours) and options.ini (game options) set, from its folder `from_dir`,
    // written over this character's; the filters are read and sent again, and each option that changed is acted on
    // as an Options window change. The anonymous / roleplay flags stay this character's. The original keeps these in
    // the one eqclient.ini that every character shares, so it has nothing to copy.
    void copy_character_settings(const std::filesystem::path& from_dir, bool chat, bool options);
    // An option changed for this session only, neither saved nor acted on (/indicator's TargetIndicator flag).
    void set_client_option_for_session(game::ClientOption id, int value) { client_options_.set(id, value); }
    // The General and Display pages' boxes that show the character's own state rather than an ini value
    // (COptionsWnd::SyncGeneralPage @0x701400 / SyncDisplayPage @0x701a20): "loot_autosplit" (CharacterBase +0x46c),
    // "auto_consent_group" / "_raid" / "_guild" (PcClient +0x2921..+0x2923, the profile's autoconsent bytes),
    // "show_helm" (PcClient +0x2964, the profile's showhelm). nullopt for another name, or before the profile.
    std::optional<int> character_setting(std::string_view name) const;
    // Their boxes clicked (GeneralPageWndNotification @0x702640, DisplayPageWndNotification @0x7090f0); false for
    // another name.
    bool set_character_setting(std::string_view name, int value);
    // The anonymity flags (PlayerClient +0x2b8: 1 anonymous, 2 roleplaying): the own spawn's, else the option's.
    int anon_flags() const;
    // __ExecuteCmd case 8, the MOUSELOOK key: MouseActive (0xddf674 / 0xde0bf0) toggled, 12372 "Mouse control
    // deactivated." or 12373 told (dsp_chat 0x111). The original saves it with the other options at the exit.
    void toggle_mouse_look_option();
    // Cur_HP below 1, or the PC's state (its virtual base +0x104) 3 or 4: knocked out (PlayerZoneClient::KnockedOut)
    // or dead and waiting to respawn (the local repop). __ProcessMouseEvents @0x539e60 then keeps the mouse from the
    // world and 0x761f10 (CContextMenuManager::PopupMenu's test) opens no context menu. Here the own death, or
    // Cur_HP (own_hp(): the items' HP included) once the server sent the maximum.
    bool player_down() const { return pstatus_.dead || pstatus_.owner_status >= 3 || (pstatus_.max_hp > 0 && own_hp().cur < 1); }
    void send_server_filter();
    // Byte 0xddf70d: set by opcode 0x150e and cleared by 0x1f65 (each carrying the player's name); while it
    // is set the chat commands and socials send nothing.
    bool chat_blocked() const { return chat_blocked_; }
    bool chat_server_connected() const;
    // ---- client option commands (net/game_connection_options.cpp; game/option_commands.h) ----
    // /quit and /exit (Cmd_quit @0x4e63a0, Cmd_exit @0x4e64c0): the client closes the connection and quits; the app
    // takes the request once.
    enum class QuitRequest : std::uint8_t { None, Quit, Exit };
    QuitRequest take_quit_request() { return std::exchange(quit_request_, QuitRequest::None); }
    // The original handlers the app follows (windows, chat windows, display and sound settings;
    // app/client_app_commands.cpp): called with the handler's name and the arguments for a table command the
    // connection does not handle; true = handled.
    void set_client_commands(std::function<bool(std::string_view handler, const std::string& args)> fn) {
        client_commands_ = std::move(fn);
    }
    // A line in the chat windows as dsp_chat shows it (`color`: the chat colour index, 0 the default), and one from
    // the string table.
    void show_chat(std::string text, std::uint32_t color = 0) { chat(ChatEvent::Kind::System, std::move(text), 0, color); }
    void show_eqstr(std::uint32_t eqstr, std::vector<std::string> args = {}, std::uint32_t color = 0) {
        system(eqstr, std::move(args), color);
    }
    // DisplayChatWithLineBreaks @0x478090: one line per newline or "\n".
    void show_with_line_breaks(const std::string& text);
    // /safelock's password (0xe15dac): while set the interface is locked (locked_line_allowed).
    bool interface_locked() const { return !safelock_.empty(); }
    // /log: whether chat goes to the log file, and its path (the character's folder, <name>.txt).
    bool chat_logging() const { return client_options_.on(game::ClientOption::Log) && !chat_log_path_.empty(); }
    const std::filesystem::path& chat_log_path() const { return chat_log_path_; }
    // /secondslog's flag (0xde09ba): set and toggled, never read by the original.
    bool seconds_log() const { return seconds_log_; }

    // A window a command asked to toggle ("raid" for /raidwindow), for the app.
    std::optional<std::string> take_window_toggle();
    // The chat server's joined channels, in /1../10 order.
    std::vector<std::string> chat_channels() const;
    // Send_OP_Camp @0x5317a0: nothing while out of control (stand state 0x66); a second /camp cancels the camp
    // (12290); refused under SPA 31 (12368); sits (12291 when it cannot; Hailstone camps a mounted player without
    // sitting, where the original refuses with 12291 or 12292), saves (Save(0, 1, 0x21): OP_Save), sends
    // OP_Camp {the camp kind} and starts a 30 s timer (12293; 12684 every 5 s); sitting up through ChangePosition
    // or GetConscious cancels it (12290), standing by other means too (12683). The camp kind is camp_command's.
    void camp();
    // Cmd_camp @0x4e8b80: CanUseCommandNow, then the kind from the argument's first four letters, case ignored:
    // eqstr 13258 "desk" 1 (the desktop), 13259 "server" 2 (the server list), else 0 (character select).
    void camp_command(const std::string& args);
    // Hailstone (option fast_camp, off by default; docs/mechanics.md): the camp's OP_Logout goes at once instead of
    // after the 30 s. A stock EQEmu zone ends the camp properly only when its own 29 s timer has run (or for a GM);
    // a logout before that leaves the character link dead in the zone for Zone:ClientLinkdeadMS (90 s by default).
    void set_fast_camp(bool on) { fast_camp_ = on; }
    bool fast_camp() const { return fast_camp_; }
    // The player's air supply (PcBase AirSupply, kept by the app's ProcessAirSupply), which the saves carry.
    void set_air_supply(std::int32_t air) { air_supply_ = air; }
    void disconnect();

    // ---- players: duels, inspect, player commands (game/interact.h; net/game_connection_player.cpp) ----
    // /duel (Cmd_duel): challenges the target, or accepts the target's challenge.
    void duel();
    // /decline (Cmd_decline): refuses a challenge (opcode 0x52f7).
    void decline_duel();
    const game::DuelState& duel_state() const { return duel_; }
    // Send_OP_InspectRequest: inspects a player within 20 (ourselves too). /inspect: inspect_target().
    bool inspect(std::uint32_t spawn_id);
    void inspect_target();
    // /open (Send_OP_LDoNOpen @0x4e7cf0) and /reveal (Cmd_reveal @0x4e7e30), also UseSkill's 115 and 116.
    void ldon_open();
    void reveal_target();
    // The player customization window (game/player_customization.h): the player's features as its spawn shows
    // them (the window tries features there), and Accept: the profile takes them and OP_FaceChange goes with them,
    // then eqstr 12028 (CPlayerCustomizationWnd::WndNotification @0x715f30).
    game::FacialFeatures player_features() const;
    void set_player_features(const game::FacialFeatures& f);
    void accept_face_change();
    // The inspect window after OP_InspectAnswer; closes itself when the player goes beyond 20.
    const std::optional<game::InspectView>& inspect_view() const { return inspect_; }
    void close_inspect();
    // Our own inspect message (the inspect window of ourselves): OP_InspectMessageUpdate.
    void set_inspect_message(const std::string& text);
    // /toggleinspect: whether a right click on a player inspects (eqclient.ini Defaults/InspectOthers).
    // The byte 0xddfce4, Options/ClickThroughMask (bit 0 self, 1 pets, 2 mercenaries; game/clicks.h): the game
    // option, which /clickthrough (Cmd_clickthrough @0x4e5730) and the Options window's three boxes set.
    std::uint8_t click_through_mask() const {
        return static_cast<std::uint8_t>(client_options_.get(game::ClientOption::ClickThroughMask));
    }
    bool inspect_others() const { return inspect_others_; }
    void set_inspect_others(bool on) { inspect_others_ = on; }
    // CEverQuest::RightClickedOnPlayer for a trader or buyer (player or NPC): its shop or barter window; for any
    // other player: inspect.
    void right_click_player(std::uint32_t spawn_id);
    // ConfirmationDialog questions (/surname) and their answer.
    const std::optional<Confirmation>& confirmation() const { return confirmation_; }
    void answer_confirmation(bool yes);
    // The bandolier (PcBase +0x6d7c): the sets, how many may be used, and its buttons.
    const std::vector<game::BandolierSet>& bandolier() const { return bandolier_; }
    int bandolier_sets() const;
    int bandolier_find(const std::string& name) const;
    int bandolier_save(const std::string& name);
    // PcZoneClient_CreateBandolierSet: the four weapon slots' items as set `index` named `name` (the bandolier
    // window's Add, at its selected row); false for a bad index or an empty name.
    bool bandolier_create(int index, const std::string& name);
    void bandolier_delete(int index);
    bool bandolier_activate(int index);
    // The window's right button held on slot 0..3 of set `index` (encode_bandolier_hold).
    void bandolier_hold(int index, int slot, bool shift);
    // Reading a book or note item (a right click on it): OP_ReadBook; the text arrives in book().
    bool read_book(const ItemSlot& slot);
    const std::optional<BookView>& book() const { return book_; }
    void close_book();
    // The book and note windows' Cast Spell / Scribe buttons: OP_BookButton with the book's slot and the target.
    void book_button();
    // The large dialog (OP_OnLevelMessage) and its answers: 0 OK, 1 Yes, 2 No, 3 closed or timed out
    // (CLargeDialogWnd's handlers: OP_PopupResponse {0, popup}, {0, popup}, {1, negative}, {2, close id}).
    const std::optional<LargeDialog>& large_dialog() const { return large_dialog_; }
    void answer_large_dialog(int button);
    // The spell book window as /book leaves it (the app shows it).
    // The tracking window (CTrackingWnd, game/tracking.h): its 1000 entries from the last OP_Track reply.
    struct Tracking {
        bool open = false;                 // the window is shown
        bool auto_update = false;          // +0x230
        std::uint32_t next_controls = 0, next_track = 0; // +0x224 / +0x228
        std::uint32_t sort_order = 0;      // 0xddfcb8 (/tracksort; a ranger's only)
        game::TrackFilter filter;          // /trackfilter, /trackplayers, /trackpets, /trackmercs
        std::int32_t tracked = 0;          // the spawn being tracked (+0x22c)
        std::uint32_t revision = 0;        // counts list changes
        game::TrackList list;
    };
    const Tracking& tracking() const { return tracking_; }
    // CTrackingWnd::AboutToShow: Activate (the list cleared, the controls), then OP_Track.
    void open_tracking();
    void close_tracking();
    void send_track();                     // Send_OP_Track (no payload)
    void track_spawn(std::int32_t spawn_id); // the Track button: OP_TrackTarget {id}, "You begin tracking %1."
    // The window's auto update check box (CTrackingWnd::sub_00779310): kept and written as TrackAutoUpdate TRUE /
    // FALSE (the character's options.ini here); the window reads it back each time it opens.
    void set_track_auto_update(bool on);
    // CTrackingWnd::WndNotification: the window's con filter buttons, sort and players combo boxes and pets /
    // mercenaries check boxes run the slash commands (Cmd_trackfilter "R" .., Cmd_tracksort "N" .., Cmd_trackplayers,
    // Cmd_trackpets, Cmd_trackmercs with the on / off / group words), so they print, save and refilter as those do.
    void track_window_command(std::string_view handler, const std::string& args) { option_command(handler, args); }
    void set_track_sort(std::uint32_t order);  // /tracksort: DoTrackSort
    void set_track_filter(const game::TrackFilter& f); // DoTrackFilter(1)
    // The list lines (AddToList): every used, shown entry in window order with its chat colour.
    struct TrackLine {
        std::string text;
        std::uint32_t color = 0; // chat colour index
        std::int32_t spawn_id = 0;
    };
    std::vector<TrackLine> tracking_lines() const;
    // CEverQuest::IsOkToTransact's inputs from the connection's state (game/trade_rules.h).
    game::TransactState transact_state() const;
    bool ok_to_transact() const;
    bool spellbook_open() const { return spellbook_open_; }
    void set_spellbook_open(bool open) { spellbook_open_ = open; }
    int spellbook_page() const { return spellbook_page_; }
    bool autosplit() const { return autosplit_; }
    // The language the chat window speaks (Cmd_language): 0 common .. 31.
    std::uint32_t chat_language() const { return chat_language_; }
    void set_chat_language(std::uint32_t language);
    // The language a chat window's line is said in (CChatWindow::WndNotification puts the window's into the default
    // while the line runs); nothing is said.
    void use_chat_language(std::uint32_t language) { chat_language_ = language < 32 ? language : 0; }

    // ---- the bazaar (game/bazaar.h; net/game_connection_market.cpp) ----
    // The trader window (CBazaarWnd, /trader): opens only in the bazaar.
    bool open_trader_window();
    void close_trader_window();
    const game::TraderState& trader_state() const { return trader_; }
    void trader_select(int slot);                 // a click on a trader slot
    void trader_enter_price(std::uint32_t copper); // the money buttons
    void trader_set_price();                      // Set Price
    void trader_add();                            // Add (trader mode)
    void trader_remove();                         // Remove (trader mode)
    void trader_clear_price();                    // Clear
    int trader_start();                           // Begin Trader: 1 sent, -1 not here (6743), -2 nothing priced (6754)
    void trader_end();                            // End Trader
    void trader_list();                           // the listing (trader mode)
    // A right click on a trader: OP_TraderShop 28; on approval merchant() opens with trader set.
    bool open_trader(std::uint32_t spawn_id);
    // The bazaar search window (/bazaar).
    bool open_bazaar_search();
    void close_bazaar_search();
    bool bazaar_search_open() const { return bazaar_search_open_; }
    const game::BazaarSearchState& bazaar_search_state() const { return bazaar_; }
    void bazaar_search(const BazaarQuery& query);
    void bazaar_welcome();
    void bazaar_inspect(std::size_t result, bool shift = false);
    // Buys a search result: to the inventory, or by parcel.
    void bazaar_buy(std::size_t result, std::uint32_t quantity, bool parcel);
    // A result row's own Buy button (CBazaarSearchWnd::WndNotification, a click on the row's button): the RoF
    // expansion (eqstr 7728), no item pending; true when the row may be bought (the window then asks how many).
    bool bazaar_row_buy_allowed();
    // Find Trader (and a right click on a row's Trader column, `throttled`): the row's trader in this zone gets
    // opcode 0x0e44 {spawn id} and the find path (PlayerPath_FindSpawnByID(spawn, 1, 0)); the button waits 15 s.
    void bazaar_find_trader(std::size_t result, bool throttled = false);
    // BZR_FindTraderButton's state (UpdateButtons and OnProcessFrame): a row whose trader is in this zone, no find
    // path shown and 15 s since the last find.
    bool bazaar_find_trader_enabled(int result) const;
    // Hide Traders (UI ini BazaarSearchWnd HideTraders): the flag; a click also forgets the found trader.
    void bazaar_set_hide_traders(bool hide, bool clicked);
    // PlayerZoneClient::IsInvisible @0x5a1850's market part: with Hide Traders a trader other than the one Find
    // Trader found, with Hide Buyers a buyer other than the one Find found, is not drawn, named or clicked.
    bool market_hidden(const game::Spawn& spawn) const;
    // Whether market_hidden can hide anyone (Hide Traders or Hide Buyers checked): with neither, no spawn is asked.
    bool market_hides_any() const;
    // The membership refuses `feature` (GetGameFeature(feature) == 0): its line in colour 0x14f, true
    // (game/feature_restrictions.h).
    bool feature_refused(std::int32_t feature);
    // PlayerClient +0x2d4 (pTouchingSwitch): the door the last floor test found under the player
    // (CollsnCallbackForFloor), or none. The trader stall check (0x4431a0) reads it.
    void set_touching_switch(std::optional<std::uint8_t> door) { touching_switch_ = door; }
    std::optional<std::uint8_t> touching_switch() const { return touching_switch_; }
    void set_profile_for_test(const PlayerProfile& p) { profile_ = p; }
    // Tests: DoPassageOfTime's timed blocks as if a frame ran at `now` (now_ms()'s clock).
    void passage_of_time_for_test(std::int64_t now) { passage_of_time(now); }
    // ---- the barter hall (game/barter.h; net/game_connection_market.cpp) ----
    // CBarterMerchantWnd: a right click on a buyer sends OP_Barter 11 (RightClickedOnPlayer); the window opens on the
    // server's answer. A line is picked, Sell asks for the quantity (sell_start: the dialog's limit, or nothing when
    // refused) and sends the sale (OP_BuyerItems 9); a right click on a compensation item inspects it.
    bool open_buyer(std::uint32_t spawn_id);
    void close_buyer();
    const game::BarterMerchantState& barter_merchant() const { return barter_merchant_; }
    void barter_merchant_select(std::uint32_t slot);
    std::optional<std::uint32_t> barter_merchant_sell_start();
    void barter_merchant_sell(std::uint32_t quantity);
    void barter_merchant_inspect(int compensation, bool shift);
    // CBarterSearchWnd (/barter): the search, the matches and selling to one of them (sell_start checks the line and
    // gives the quantity dialog's limit), inspecting, the buyer's greeting, the hall's welcome, finding the buyer.
    const game::BarterSearchState& barter_search_state() const { return barter_search_; }
    void set_barter_search_open(bool open);
    void barter_search_set_text(std::string text) { barter_search_.text = std::move(text); }
    void barter_search_set_combo(int choice) { barter_search_.combo = choice; }
    void barter_search(const std::string& text);
    void barter_search_select(int line);
    void barter_search_select_detail(int row) { barter_search_.detail_sel = row; }
    void barter_search_refresh_inventory();
    std::optional<std::uint32_t> barter_search_sell_start(int line);
    void barter_search_sell(std::uint32_t quantity);
    void barter_search_inspect(int compensation, bool shift);
    void barter_search_greeting();
    void barter_search_welcome();
    void barter_search_find();
    void barter_search_set_hide(bool hide, bool clicked = false);
    // CBarterWnd (/buyer): our buy lines and buyer mode. The editor takes the picked item (or a line's values), the
    // count, the price (barter_set_price_coin with the dialog's answer) and the compensation items (add_start /
    // remove_start check and give the dialog's limit and start). Create / Modify / Toggle / Remove send at once in
    // buyer mode and change the local lines otherwise; the lines are kept in BART.ini.
    const game::BarterWindowState& barter_window() const { return barter_window_; }
    bool open_barter_window();
    void close_barter_window();
    game::BarterWindowButtons barter_window_buttons() const;
    void barter_item_search(const std::string& text);
    void barter_item_click(int row, bool shift, bool ctrl, bool alt);
    void barter_item_inspect(int row, bool shift);
    void barter_select_inventory(int row);
    void barter_select_compensation(int row);
    void barter_select_line(int row);
    void barter_set_count(std::int32_t count);
    void barter_set_price_coin(int coin, std::uint32_t value);
    std::optional<std::pair<std::int32_t, std::int32_t>> barter_compensation_add_start();
    void barter_compensation_add(std::uint32_t quantity);
    std::optional<std::pair<std::int32_t, std::int32_t>> barter_compensation_remove_start();
    void barter_compensation_remove(std::uint32_t quantity);
    void barter_create_line();
    void barter_modify_line();
    void barter_toggle_line();
    void barter_remove_line();
    void barter_list_lines();
    void barter_clear_status() { barter_window_.status.clear(); }
    void barter_start_stop();
    void barter_welcome_edited() { barter_window_.welcome_changed = true; }
    void barter_update_welcome(const std::string& text);
    void barter_refresh_inventory();

    // ---- mail (game/mail.h; net/game_connection_mail.cpp) ----
    const game::MailState& mail() const { return mail_; }
    void mail_open();
    // MailMgr_SetMailForwarding: the Options mail page's forwarding box, sent to the chat service.
    void mail_set_forwarding(bool on);
    void mail_close();
    void mail_select_mailbox(int index);
    void mail_request_headers();
    bool mail_request_body(std::uint32_t id);
    void mail_set_status(const std::vector<std::uint32_t>& ids, game::MailStatus status);
    void mail_delete(const std::vector<std::uint32_t>& ids);
    void mail_undelete(const std::vector<std::uint32_t>& ids);
    void mail_empty_trash();
    bool mail_send(const std::string& to, const std::string& subject, const std::string& body);
    // The address book (game::MailAddressBook); the app loads and saves it.
    game::MailAddressBook& mail_address_book() { return mail_.address_book; }
    // The mail ignore list (MailIgnoreListWindow), a book of the same kind; the app loads and saves it.
    game::MailAddressBook& mail_ignore_list() { return mail_.ignore_list; }
    // The built-in lists the address book shows beside its entries: "Guild" (the guild's other members) and "Group"
    // (the group's other members), when there are any.
    std::vector<game::MailAddressEntry> mail_builtin_lists() const;
    // __ServerName as mail addresses carry it (the chat login's short name).
    std::string mail_server() const;
    // Our own mail address ("SOE.EQ.<server>.<name>", the composition window's From).
    std::string mail_own_address() const { return game::mail_qualify_address(character_, mail_server()); }

    // ---- mercenaries (game/mercenary.h; net/game_connection_merc.cpp) ----
    const game::MercManager& merc() const { return merc_; }
    // A right click on a mercenary liaison (RightClickedOnPlayer: class 71 within 20): its id is kept and the
    // merchant window shows, whose show handler asks for the list (OP_MercenaryDataRequest).
    bool open_merc_liaison(std::uint32_t npc_id);
    // The three windows' open state (MMTW_MerchantWnd, MMGW_ManageWnd, MIW_InfoWnd).
    bool merc_window_open(int which) const { return merc_windows_[which]; }
    void set_merc_window_open(int which, bool open);
    // /mercwindows (Cmd_mercwindows): toggles the manage window when allowed.
    void merc_windows_command();
    // The merchant window: picks a merchant entry (0x566b00 copies it to +0x3c) and the hire button.
    void merc_select_entry(std::size_t index);
    void merc_hire(std::uint32_t merc_id);
    // The manage window's buttons.
    void merc_suspend() { with_merc([](game::MercManager&, game::MercHooks& h) { game::merc_send_suspend(h); }); }
    void merc_dismiss(); // with the DismissMercenaryConfirm option, eqstr 7612 first
    void merc_set_assist(bool on);
    void merc_call_for_assist() { with_merc([](game::MercManager&, game::MercHooks& h) { game::merc_send_call_for_assist(h); }); }
    void merc_switch(std::int32_t index);
    void merc_set_owner();
    // A stance chosen in the manage window's stance list (Send_OP_MercenaryCommand): `row_data` is the list's
    // stance per row; returns the rows whose colour changed ({row, ARGB}).
    std::vector<std::pair<std::int32_t, std::uint32_t>> merc_choose_stance(const std::vector<std::uint32_t>& row_data,
                                                                           std::int32_t row);
    // Events 0x0d (the merchant list arrived) seen so far: the merchant window refills on a change.
    std::uint32_t merc_list_serial() const { return merc_list_serial_; }

private:
    void fail(std::string message);
    void handle_world(const AppPacket& p);
    void handle_zone(const AppPacket& p);
    // Through the plugins' outgoing packet hooks (ConnectionOptions::hooks).
    void send_world(const AppPacket& p);
    bool send_zone(const AppPacket& p, bool reliable = true, const std::function<bool()>& still_current = {});
    // The hook bus's game state, zoning and spawn events, from the state changes since the last update.
    void raise_hook_events();
    void send_checksums();
    void maybe_request_client_spawn();
    void finish_zone_in();
    void send_position(bool force);
    void apply_spawn(const Spawn& s);
    void set_spawn_motion(game::Spawn& s, const Motion& m);
    void connect_world(bool zoning);
    void chat(ChatEvent::Kind kind, std::string text, std::uint32_t rgb = 0, std::uint32_t color = 0,
              std::string peer = {});
    // ReportSuccessfulHit's melee line for an attack by `source`, through the chat filters (`report`: its argument).
    void report_hit(const game::HitLine& line, std::uint32_t source, bool report = true);
    void handle_combat(Op op, const AppPacket& p);
    void handle_spell(Op op, const AppPacket& p);
    // An OP_Action of type 231; from_server false: a local presentation (spell 859's missile, spell_missile_hit).
    void handle_spell_action(const Action& action, std::int64_t now, bool from_server);
    // Handle_OP_MemorizeSpell @0x4be150 (game/mechanics/spell_gems.h) on the connection's state.
    void handle_memorize_spell(const MemorizeSpell& m);
    void land_spell_on_me(const Action& a, std::int64_t now);
    void handle_item(Op op, const AppPacket& p);
    void handle_ability(Op op, const AppPacket& p);
    void handle_door(Op op, const AppPacket& p);
    void handle_progress(Op op, const AppPacket& p);
    bool handle_rewards(const AppPacket& p);
    bool handle_achievements(const AppPacket& p);
    void reset_achievements();
    void update_achievements();
    std::pair<std::uint32_t, std::uint64_t> achievement_target() const;
    bool reward_request_ready(std::string* why = nullptr) const;
    void reset_rewards();
    std::int64_t reward_now_ms() const;
    bool progress_command(std::string_view handler, const std::string& args);
    void handle_skill_update(const AppPacket& p);
    void handle_level_update(std::uint32_t new_level);
    void reset_progress_for_zone();
    void request_aa_data();
    // A client-side "buy all ranks" (aa_train with train_all on a server without action 4): once the rank last
    // bought is owned and the points are updated, the next rank is bought while it can be trained.
    void continue_aa_buy_all();
    bool combine_checks(std::uint32_t container_type, const std::vector<const Item*>& contents);
    bool combine_gates(std::uint32_t container_type); // the race and class containers' refusals
    void send_combine(const ItemSlot& container_slot, std::uint32_t container_type);
    game::TraineeView trainee() const;
    const game::mechanics::GameTables& tables() const;
    void melee_sound(std::uint32_t target, std::uint32_t source, std::int32_t damage);
    // Handle_OP_Damage's report flag: the hit involves the player, or its target is within 75 units.
    bool hit_reported(std::uint32_t target, std::uint32_t source) const;
    // CEverQuest::ReportSuccessfulHit @0x52ee40 for a hit (OP_Damage, or the player's own environmental damage):
    // its chat line, sounds and the target's animation.
    void report_successful_hit(const CombatDamage& d, bool reported);
    void update_doors();
    // Sends an OP_ZoneChange and waits for its answer (RequestZoneChangeStatus).
    void start_zone_change(const ZoneChange& request);
    const game::DoorRiders* door_riders_ = nullptr;
    // Riding (net/game_connection_mounts.cpp).
    game::MountQueries* mount_queries_ = nullptr;
    game::MountQueries& mount_queries();
    void mount_events(const game::MountEvents& ev);
    void remove_mount_affect();
    void mount_up(const std::vector<std::uint32_t>& made);
    // Handle_OP_ClientUpdate's rider rule @0x4b3e0a: false when the update is for a mount that has a rider (dropped);
    // a rider's update also moves its mount.
    bool rider_update(std::uint32_t spawn_id, const Motion& m);
    void spawn_leaving(std::uint32_t id);
    // PlayerClient_ApplyNewArmorFilter's inputs for one spawn (Appearance::new_armor_bits): its kind, and whether it
    // is the local player, a group or a raid member.
    void refresh_new_armor_bits(game::Spawn& s) const;
    // Whether the local player, the group or the raid changed since the last call (no allocation when not).
    bool new_armor_membership_changed();
    // CEverQuest_ProcessCursorQueue @0x5321a0, once a frame (net/game_connection_items.cpp).
    void process_cursor_queue();
    bool item_command(const std::string& command, const std::string& args);
    void handle_commerce(Op op, const AppPacket& p);
    void commerce_item(std::uint32_t packet_type, Item item);
    bool commerce_command(const std::string& command, const std::string& args);
    void close_trade(bool completed);
    void reset_commerce_for_zone();
    void handle_group(Op op, const AppPacket& p);
    bool group_command(const std::string& command, const std::string& args);
    // The group roles and the leadership abilities' commands (net/game_connection_group.cpp): /grouproles,
    // /groupleader, /marknpc, /gmarknpc, /rmarknpc, /clearmarks, /findpc, /inspectbuffs.
    bool leadership_command(std::string_view handler, const std::string& args);
    bool can_use_command_now(); // CanUseCommandNow @0x4de9e0
    void handle_leadership_ability(Op op, const AppPacket& p);
    // The leadership abilities (Leadership_IsAbilityActive @0x55eb70, Leadership_ActivateAbility @0x55f0f0,
    // Leadership_FindPathToPlayer @0x55eee0, their group and raid size checks @0x55ea00 / @0x55ea70).
    bool group_has_players(int n) const;                    // 0x443630: a group of at least n
    bool raid_has_players(int n) const;                     // 0x57f670: a raid of at least n
    bool leadership_has_rank(int ability, std::int32_t rank) const; // Leadership_HasAbilityRank
    void check_group_size();                                // 0x2263 with "3" below three players
    void check_raid_size();                                 // 0x2262 with "18"
    bool is_raid_delegate(const std::array<std::uint32_t, 3>& ids) const; // 0x57f760 / 790 / 7c0
    void set_target_of_target_window(bool show);            // CTargetOfTargetWnd's Show, through the app
    void find_path_to_player(const std::string& name);
    void send_find_path(const game::Spawn& to, bool face = false); // PlayerPath_FindSpawnByID / FindGroupMember
    void mark_npc_command(const std::string& args, int ability, std::uint32_t usage);
    std::uint32_t group_member_roles(int slot) const; // the role bits (1 tank, 2 assist, 4 puller)
    void grouproles_command(const std::string& args);
    void groupleader_command();
    void reset_group_for_zone();
    void note_pet_spawn(const game::Spawn& s);
    void forget_pet_spawn(std::uint32_t spawn_id);
    void move_item(const ItemSlot& from, const ItemSlot& to, std::uint32_t count);
    void refresh_worn_slot(int slot);
    void init_spells_from_profile();
    void update_spells();
    void update_spells_at(std::int64_t now); // one step of update_spells, at `now` (now_ms()'s clock)
    // DoPassageOfTime's 1 s and 6 s blocks (game/passage_of_time.h), once a frame in a zone.
    void passage_of_time(std::int64_t now);
    // CSpellBookWnd::GetSpellMemTicksLeft once (the book's memorise gauge drawn).
    void memorize_tick(std::int64_t now);
    std::optional<std::vector<std::int64_t>> game_steps_; // set_game_steps
    void update_usable_gems();
    // The stats port over the current profile, inventory, buffs and AAs (CharacterZoneClient's effect totals).
    void with_character(const std::function<void(game::mechanics::Character&)>& fn) const;
    // with_character's sheet, rebuilt in place on each call (build_sheet_into: no allocation for an unchanged
    // inventory); a nested call builds its own.
    mutable std::unique_ptr<game::mechanics::CharacterSheet> character_sheet_;
    mutable bool character_sheet_busy_ = false;
    void update_merchant();
    void update_npc_interactions();
    void update_barter();
    void update_bank();
    void update_loot();
    void loot_failed(std::uint32_t eqstr);
    void reset_loot_session();
    void auto_bank_cursor(bool stacks);
    // ItemContainer_MergeStacks through Send_OP_MoveItem: 0 merged whole,
    // 1 part moved, 2 nothing (not stackable together or no room).
    int merge_stacks(const ItemSlot& from, const ItemSlot& to);
    // CharacterBase_IsInvSlotAvailable for our character (game::inv_slot_available).
    bool inv_slot_available(int slot) const;
    // CharacterZoneClient::CanUseItem(item, 1, 0) for our character, quietly.
    bool can_use_item_full(const Item& item) const;
    // MoveItem's bag path with bMoveBagContents (OP_MoveMultipleItems): the cursor item, or a cursor bag's
    // contents, into the bag at `to`; with `force_bag` (Ctrl + right-click) the two bags' contents trade places.
    void move_into_bag(const ItemSlot& to, bool force_bag = false);
    // A server OP_MoveItem (Handle_OP_MoveItem_Apply, or the trade / world removal).
    void apply_server_move(const MoveItem& m);
    // ConfirmationDialog_HandleResponse case 0x7a, yes: the cursor item goes into `slot` by the ordinary MoveItem,
    // which sets its NoDrop flag (no attune packet).
    void equip_attuned(const ItemSlot& slot);
    std::uint32_t* coin_pile(int location, int type);
    // `color`: the original's colour index of the line (ChatEvent::color), 0 for the channel's.
    void spell_message(std::uint32_t eqstr, std::vector<std::string> args = {}, std::uint32_t color = 0);
    // StopSpellCast(3, spell) (kNoSpell: the spell being cast); a bard song goes through bard_song_stopped.
    void stop_spell_cast(std::uint32_t spell = game::kNoSpell);
    // StopSpellCast's bard song part (game_connection_ability_commands.cpp) for `reason` (0 the client's stop).
    void bard_song_stopped(std::uint32_t reason);
    void restart_bard_twist(std::uint8_t reason);
    void attack_command(const std::string& args);
    void target_by_name(const std::string& name);
    // Social (net/game_connection_social.cpp).
    bool run_slash_command(std::size_t index, const std::string& args);
    void send_channel(std::uint32_t channel, const std::string& target, const std::string& text);
    void say_to_channel(std::uint32_t channel, const std::string& text);
    void say_command(const std::string& text);
    void away_reply(const ChannelMessage& m);
    void handle_social(Op op, const AppPacket& p);
    void handle_world_social(Op op, const AppPacket& p);
    void handle_channel_message(const ChannelMessage& m);
    void handle_special_message(const SpecialMessage& m);
    // `keep_target`: the target CastSpell would change (a pet or owner spell) stays (Handle_OP_ItemVerifyReply's
    // dispatch restores pinstTarget). `send`: CastSpell's sixth argument, whether OP_CastSpell goes out.
    void cast_spell_slot(int gem, std::uint32_t spell, const ItemSlot* item_slot = nullptr, bool keep_target = false,
                         bool send = true);
    void handle_item_verify_reply(const ItemVerifyReply& r);
    void apply_poison_tick();                          // DoPassageOfTime's poison block
    void handle_apply_poison(const ApplyPoison& r);    // Handle_OP_ApplyPoison
    void handle_item_preview(const AppPacket& p);      // HandleWorldMessage case 0x7f80
    std::int64_t poison_started_ms_ = 0; // 0xddf724 (0: not applying)
    std::int16_t poison_slot_ = 0;       // 0xddf728: the poison's top-level slot (ItemGlobalIndex slot 0)
    bool poison_interacting_ = false;    // __CurrentlyInteracting's count for the sent OP_ApplyPoison
    void useitem_command(const std::string& args); // Cmd_useitem
    ItemPreview item_preview_;
    Marketplace marketplace_;
    std::array<std::uint32_t, 100> item_recast_timers_{}; // PcBase +0x6a5c: the item recast timers by type (time(0))
    void system(std::uint32_t eqstr, std::vector<std::string> args = {}, std::uint32_t color = 0);
    void send_tell(const std::string& name, const std::string& text);
    void who_command(const std::string& args);
    void emote_command(const std::string& args);
    void social_emote(const game::SocialEmote& e, const std::string& args);
    void random_command(const std::string& args);
    void location_command();
    void time_command();
    void played_command();
    void anonymous_command(const std::string& args, bool roleplay);
    void fastdrop_command(const std::string& args);   // Cmd_fastdrop @0x4e5200
    void lootnodrop_command(const std::string& args); // Cmd_lootnodrop @0x4f2700
    void say_lootnodrop_setting(std::int32_t value);  // "New Setting: " and the setting's name
    void afk_command(const std::string& args);
    void lfg_command(const std::string& args);
    void assist_command(const std::string& args);
    void follow_command();
    void reply_command(const std::string& args);
    void guild_command(std::string_view handler, const std::string& args);
    void set_raid_ability_ranks(const std::array<std::uint32_t, 64>& block);
    void friends_command(const std::string& args);
    void ignore_command(const std::string& args);
    void chat_server_command(const std::string& text);
    void send_chat_server_message(const std::string& text, bool via_world);
    void numbered_channel(int n, const std::string& text);
    void update_chat_server();
    void handle_chat_server_event(const struct ChatServerEvent& e);
    void reset_social_for_zone();
    std::string target_name_or(const std::string& args) const;
    // PvP rule set 4 and PlayerZoneClient::NotOnSameDeity(local player, spawn, no spell): the guild (12244), raid
    // (395) and group (12271) invitations refuse the other deity team.
    bool other_deity_team(const game::Spawn& s) const;
    // Players (net/game_connection_player.cpp).
    bool player_command(std::string_view handler, const std::string& args);
    // The client option commands (net/game_connection_options.cpp).
    bool option_command(std::string_view handler, const std::string& args);
    // The client, help and report commands (net/game_connection_client.cpp).
    bool client_info_command(std::string_view handler, const std::string& args);
    void help_command(const std::string& args);
    void report_command(const std::string& args);
    void autojoin_channels();
    void quit_command(bool exit, const std::string& args);
    void log_command(const std::string& args);
    void open_chat_log(const std::string& name);
    void write_chat_log(const std::string& text);
    void close_chat_log();
    void netstats_command();
    void track_toggle_command(game::ClientOption id, const std::string& args);
    void tracking_from_options();
    QuitRequest quit_request_ = QuitRequest::None;
    std::function<bool(std::string_view, const std::string&)> client_commands_;
    std::string safelock_;
    std::filesystem::path chat_log_path_;
    // The open log and the path it was opened for: one open per file, not per line (a raid's hundreds of lines a
    // second). Reopened when /log names another file; closed when /log goes off.
    std::unique_ptr<std::ofstream> chat_log_;
    std::filesystem::path chat_log_open_path_;
    bool seconds_log_ = false;
    float distance_to(std::uint32_t spawn_id) const;
    bool is_ignored(const std::string& name) const;
    void reset_player_for_zone();
    void handle_player(Op op, const AppPacket& p);
    void handle_duel_request(std::uint32_t challenger);
    void handle_duel_refusal(const DuelRefusal& r);
    void send_duel_refusal(std::uint32_t challenger, std::uint8_t kind);
    void duel_decline_message(std::vector<std::string>& args);
    void duel_spawn_removed(std::uint32_t spawn_id, bool decayed);
    void handle_death(const Death& d);
    void handle_zone_to_bind(const AppPacket& p);
    void cancel_camp(std::uint32_t eqstr);
    // Save @0x52ae60 (OP_Save, `kind` its third argument) and SavePCForce @0x51d4d0 with argument 0
    // (OP_SaveOnZoneReq): the PC's position, armour, actors, minutes played and air supply (SaveRequest).
    SaveRequest save_request(std::uint8_t kind) const;
    void send_save(std::uint8_t kind);
    void save_tick();
    std::uint32_t save_ticks_ = 0;   // 0xde0868: 6 s ticks since the last save
    std::uint32_t save_tick_ms_ = 0; // the last tick
    void camp_tick();
    void attack_hold_tick(); // attack_until_xtarget_clear: auto attack off once the list holds no spawn
    void auto_target_tick(); // auto_target_xtarget: a pending pick, with no target, of an attacker on the list
    // auto_target_xtarget: the attacker to target, `prefer` first when it qualifies, else the first in slot order;
    // 0 none.
    std::uint32_t xtarget_attacker(std::uint32_t prefer) const;
    void guild_invite_tick(); // an unanswered guild invitation declines after kGuildInviteMs
    void handle_hailstone_guild(const HailstoneGuild& g);
    void handle_inspect_answer(const InspectAnswer& a);
    void update_inspect();
    void toggle_inspect_command(const std::string& args);
    void surname_command(const std::string& args);
    void corpse_command(Op op);
    void shield_command(const std::string& args);
    bool may_shield(); // MayShield, the checks /shield and /shieldgroup share
    // The game commands of net/game_connection_game_commands.cpp: /shieldgroup, /blockspell, /sheathe, /open,
    // /reveal, /hidecorpses, /decaycorpse, /setstartcity.
    bool game_command(std::string_view handler, const std::string& args);
    void shield_group_command();
    void blockspell_command(const std::string& args);
    void hidecorpses_command(const std::string& args);
    void decaycorpse_command();
    // The loot window closing (CLootWnd::AboutToHide @0x6bcd40) with /hidecorpses looted on.
    void hide_looted_corpse();
    bool hide_looted_corpses_ = false; // CDisplay +0x14 (bHideLootedCorpses)
    // /targetgroupbuff and /tgb (Cmd_targetgroupbuff @0x4ea820), /spellscribe (Cmd_spellscribe @0x4f7b90),
    // /offlinemode (Cmd_offlinemode @0x4e6660).
    void target_group_buff_command(const std::string& args);
    void send_target_group_buff_setting(); // DoMainLoop's start (@0x53d936): the kept TargetGroupBuff
    void spellscribe_command(const std::string& args);
    // 0xddf8ac: when /offlinemode last went ahead (ms, now_ms()); 0 none. Cleared at a zone change
    // (CEverQuest_ResetClientStateForZone @0x531b36) and by the server's opcode 0x5369 (HandleWorldMessage @0x4cb984).
    std::int64_t offline_mode_at_ = 0;
    void autosplit_command(const std::string& args);
    void init_bandolier_from_profile();
    std::int32_t aa_effect_total(std::int32_t spa) const;
    std::optional<ItemSlot> find_item_for_bandolier(std::uint32_t item_id) const;
    // Client::MoveItemToInventory short of its cursor fallback: false (nothing placed) when there is no room.
    bool return_item_to_inventory(Item item);
    void bandolier_command(const std::string& args);
    void handle_read_book(const ReadBook& r);
    void book_command(const std::string& args);
    void language_command(const std::string& args);
    // The bazaar (net/game_connection_market.cpp).
    bool market_command(std::string_view handler, const std::string& args);
    void handle_market(Op op, const AppPacket& p);
    void reset_market_for_zone();
    // The barter manager (game/barter.h): its status text and log (CBarterWnd_AppendStatusText, Barter_WriteLog),
    // the BART ini, the sale of a line (OP_BuyerItems 9), one of our lines sent or changed locally
    // (Send_OP_BuyerItems_004229a0), the barter packets.
    void barter_status(std::uint32_t color, const std::string& text, bool log = true);
    void barter_log(const std::string& text);
    void barter_load_ini();
    void barter_save_ini(int slot);
    void barter_send_line(const BuyLine& line);
    void barter_reset_editor();
    void handle_barter(const AppPacket& p);
    void handle_buyer_items(const AppPacket& p);
    void bazaar_text(std::uint32_t color, std::string text);
    bool stall_under_player() const; // 0x4431a0's switch test (touching_switch_ is a stall)
    void load_trader_slots();
    bool store_trader_price(std::uint32_t price);
    void handle_trader_message(const TraderMessage& m);
    void trader_reconcile();
    void trader_buy(const Item& item, std::uint32_t quantity);
    void handle_trader_purchase(const TraderBuy& b);
    // Mercenaries (net/game_connection_merc.cpp).
    struct MercLink;
    bool handle_merc(const AppPacket& p);
    void with_merc(const std::function<void(game::MercManager&, game::MercHooks&)>& fn);
    bool merc_command(std::string_view handler, const std::string& args);
    void merc_zone_in();
    void update_merc();
    // Mail (net/game_connection_mail.cpp).
    bool mail_send_command(const std::string& text);
    void handle_mail_event(const ChatServerEvent& e);

    const GameDir& game_;
    ConnectionOptions options_;
    std::optional<plugin::Rules> server_rules_;
    void update_server_rules();
    bool guild_world_invites_ = false; // OP_HailstoneGuildReply's hello this zone-in (server/'s script)
    std::uint32_t server_rules_serial_ = 0;
    int hook_game_state_ = 0;              // the game state last raised (plugin::kGameState*; 0 none yet)
    ConnectionState hook_state_ = ConnectionState::Idle;
    std::vector<std::uint32_t> hook_spawns_; // the spawn ids last raised, sorted
    std::vector<std::uint32_t> hook_spawns_scratch_; // raise_hook_events' buffer for the next list
    ConnectionState state_ = ConnectionState::Idle;
    std::string error_;
    std::string status_;

    WorldTicket ticket_;
    mutable std::optional<std::pair<std::string, std::optional<ServerProfile>>> saved_server_; // by profile id
    std::unique_ptr<Session> world_session_;
    std::unique_ptr<Session> zone_session_;
    std::string zone_host_;
    std::uint16_t zone_port_ = 0;
    bool zoning_ = false;
    bool world_login_sent_ = false;
    // ZoneServerInfo's hand-off (4.4): OP_WorldComplete went out and the zone is connected once world drops the
    // link (NetMgr_WaitForDisconnect, 30 s).
    bool world_drop_pending_ = false;
    std::chrono::steady_clock::time_point world_drop_deadline_{};
    // Leaving a zone (MoveToZone's WaitForWorldDisconnect, 120 s; a camp's NetMgr_WaitForDisconnect, 3 s): the
    // zone is to drop the link; then world again (zoning) or the end of the camp.
    enum class ZoneExit { None, Zoning, CampLogout, CampDeleteSpawn };
    ZoneExit zone_exit_ = ZoneExit::None;
    std::chrono::steady_clock::time_point zone_exit_deadline_{};
    void leave_zone_dropped();
    std::optional<Membership> membership_;
    std::optional<MembershipDetails> membership_details_;
    bool tributes_requested_ = false; // Tribute_SendPendingRequests' flags (+0x1c, +0x60): once per session

    std::vector<CharacterSelectEntry> characters_;
    std::uint32_t characters_serial_ = 0;
    // server_ext(): ConnectionOptions::plugins.
    game::ExtensionChain ext_chain_;
    std::map<std::string, std::uint32_t, std::less<>> character_classes_; // set_character_classes
    std::uint32_t player_classes_ = 0;                                      // set_player_classes
    std::optional<CharCreateData> create_data_;
    std::optional<CharCreate> pending_create_;
    std::string pending_create_name_;
    std::string character_;

    std::optional<PlayerProfile> profile_;
    std::optional<NewZone> new_zone_;
    std::optional<TimeOfDay> time_;
    std::chrono::steady_clock::time_point time_received_{};
    game::WorldState world_;
    bool own_spawn_seen_ = false;
    bool weather_seen_ = false;
    std::vector<Weather> weather_messages_;
    bool client_spawn_requested_ = false;
    bool zone_ready_ = false;
    std::optional<std::string> zone_to_load_;
    // The server's client files (net/server_files.h): asked at the first zone-in; the zone-in waits for them.
    std::unique_ptr<ServerFileSync> server_files_;
    bool server_files_changed_ = false;
    void finish_server_files();
    std::deque<ChatEvent> chat_;
    std::vector<ZonePoint> zone_points_;
    bool zone_change_pending_ = false;
    ZoneChange zone_change_request_; // the pending request, for "not ready" retries
    std::chrono::steady_clock::time_point zone_change_started_{}, zone_change_sent_{}, zone_change_retry_at_{};
    game::EqStrings strings_;

    // Movement sending.
    glm::vec3 position_{0.0f};
    glm::vec3 velocity_{0.0f};
    float heading_ = 0.0f;
    bool moving_ = false;
    bool motion_dirty_ = false;
    bool motion_known_ = false;
    game::PositionTracker position_tracker_; // its sample count is OP_ClientUpdate's sequence
    std::uint32_t position_history_sent_ = 0; // PlayerClient +0x1e8c
    std::uint32_t position_history_packets_ = 0; // OP_FloatListThing packets sent (log)
    std::chrono::steady_clock::time_point last_position_send_{};
    std::chrono::steady_clock::time_point last_heading_mark_{};
    glm::vec3 last_sent_position_{0.0f};
    float last_sent_heading_ = 0.0f;

    bool running_ = true; // __RunWalkState (the original starts running)

    // Combat.
    PlayerStatus pstatus_;
    std::uint32_t consciousness_changes_ = 0;
    OwnHp own_hp_items_; // what own_hp() adds to pstatus_ (set_own_hp_items)
    void set_own_spawn_hp(); // the player's spawn's HP percent from own_hp() (SetSpawnHP)
    bool auto_attack_ = false;
    bool attack_until_xtarget_clear_ = false;
    bool fast_camp_ = false;
    bool cast_animation_fallback_ = true; // set_cast_animation_fallback
    bool attack_held_for_xtarget_ = false; // kept on at the target's death (attack_until_xtarget_clear)
    bool auto_target_xtarget_ = true;
    bool face_on_attack_ = true;
    std::vector<std::uint32_t> xtarget_attackers_; // NPCs that hit (or missed) the player this zone
    bool auto_target_due_ = false;                 // a hit or the target's death since the last pick
    std::uint32_t auto_target_prefer_ = 0;         // the latest attacker
    std::int32_t pvp_rules_ = 0;
    std::int32_t spell_scribe_allowed_ = 0; // 0xddfd3c, OP_LogServer +0x18 (/spellscribe)
    bool spell_book_open_ = false;
    int book_swap_slot_ = -1; // CSpellBookWnd::SelectedBookIndex
    int usable_gems_ = 8;
    std::uint32_t controlled_boat_ = 0; // pinstControlledPlayer when not the player
    game::Helm helm_;
    std::chrono::steady_clock::time_point last_helm_send_{};
    bool helm_moving_ = false;
    std::uint32_t standing_on_ = 0;     // the local player's +0x150
    void handle_control_boat(const AppPacket& p);
    std::int32_t group_member_targeted_ = -1; // PlayerClient +0x1214 GroupMemberTargeted
    bool book_was_sitting_ = false; // CSpellBookWnd +0x248
    game::SpellLoadouts spell_loadouts_;
    game::GroupRoleSets group_role_sets_;
    // CSpellBookWnd's spell set state: +0x254 mode (2 a set), +0x258 the book was open before, +0x25c the gem looked
    // at, +0x260 the set's spells, +0x290 their count, +0x294 / +0x298 the next memorise's book index and gem (-1
    // none), +0x250 when it starts (__EQGetTime).
    struct SetMemorize {
        std::int32_t mode = 0;
        bool keep_open = false;
        std::int32_t at = -1;
        std::array<std::int32_t, game::kLoadoutGems> spells{};
        std::int32_t count = 0;
        std::int32_t book_index = -1;
        std::int32_t target_gem = -1;
        std::uint32_t next_time = 0;
    } set_mem_;
    void continue_set_mem();   // CSpellBookWnd::ContinueSetMem @0x75bc70
    void set_mem_frame();      // CSpellBookWnd::OnProcessFrame's part @0x75ce2b
    // CSpellBookWnd::StartSpellMemorization @0x75bf30 (no sitting check: the book's click makes that one)
    bool start_spell_memorization(int book_index, int gem, bool replace);
    void memspellset_command(std::string_view args); // Cmd_memspellset @0x4f7100
    float speed_run_ = 0.0f;
    void spell_book_closed_by_game(bool stand); // LocalPlayer_CloseSpellbookAndDrag
    bool attack_locked_ = false;
    bool sent_auto_attack_ = false;
    bool auto_fire_ = false, sent_auto_fire_ = false; // __Autofire and DoPassageOfTime's last sent copy (0xb827f4)
    std::vector<std::int32_t> bard_twist_;            // PlayerClient +0x1e90: gem numbers (1-based)
    std::uint32_t bard_twist_pos_ = 0;                // +0x1eac
    std::function<bool(std::string_view, const std::string&)> app_commands_;
    std::optional<RespawnWindow> respawn_;

    // Spells.
    std::shared_ptr<const game::SpellTable> spell_table_;
    game::HitModes hit_modes_{};
    game::SpellState spells_;

    // Items.
    game::Inventory inventory_;
    game::KeptItems kept_items_;  // Hailstone: the Keep marks (kept_items.txt)
    std::uint64_t kept_revision_ = 0;
    void load_kept_items();
    // A kept item (or a bag holding one) about to be `verb`ed ("destroy", "drop", ...): says so, and true.
    bool refuse_kept(const Item* item, std::string_view verb);
    bool refuse_kept_id(std::uint32_t id, std::string_view verb);
    net::Coins coins_;
    net::Coins pending_split_; // opcode 0x3ed4's coin (0xddfcd4..0xddfce0), credited by process_cursor_queue
    std::optional<LootSession> loot_;
    std::deque<std::int16_t> loot_all_queue_;
    bool loot_popup_pending_ = false; // CLootWnd +0x398 (bPopupPending): a no-drop question is showing
    bool loot_close_on_complete_ = false; // +0x2f8 (bCloseOnComplete): a Loot All closes the window when done
    bool loot_all_deferred_ = false;      // +0x2f9: Loot All asked for before every item arrived
    std::int64_t loot_check_ms_ = 0;      // CLootWnd::nNextRefreshTime
    void loot_all_now();              // CLootWnd::LootAll(1)
    void send_loot_item(std::int16_t corpse_slot, bool auto_loot, std::uint32_t quantity = 0); // OP_LootItem
    bool looting_own_corpse() const;  // the loot corpse's owner (+0xe4) is the player
    bool loot_asks_no_drop(const Item& item) const;
    bool can_use_item_class(const Item& item) const; // CanUseItem(item, 0, 0)
    std::deque<Item> item_displays_;
    std::deque<Item> item_refreshes_;                     // for take_item_refresh()
    std::map<std::uint32_t, std::string> augment_info_;   // SolventText by display window (OP_AugmentInfo)
    // PcZoneClient::GetItemByID @00585a10 / GetItemByItemClass @00586210: the possessions searched recursively.
    std::optional<game::FoundItem> find_possession(const std::function<bool(const Item&)>& match) const;
    void handle_augment_info(const AppPacket& p);
    std::deque<AppPacket> ability_packets_; // for poll_ability_packet()

    // Doors and ground objects.
    std::vector<game::Door> doors_;
    // The OP_FindPersonRequests waiting for their reply, in the order sent (each reply goes to the oldest; EQEmu's
    // order is not guaranteed, see path_reply): true for request_quiet_path's, false for a shown find's; with when
    // each went out (now_ms).
    std::deque<std::pair<bool, std::int64_t>> find_requests_;
    std::optional<std::vector<glm::vec3>> quiet_path_;
    game::SwitchClock switch_clock_;
    std::vector<game::GroundObject> ground_objects_;
    std::optional<game::WorldContainer> world_container_;
    std::optional<std::uint32_t> world_container_requested_;
    std::uint32_t door_moves_seen_ = 0;
    std::vector<SoundCue> sound_cues_;
    std::vector<SpellEffectCue> effect_cues_;
    std::vector<EffectPacketCue> effect_packets_;
    std::vector<std::uint32_t> blood_spurts_; // local player landed sharp-weapon hit target IDs
    // Per caster, the spell whose cast stage has played (its OP_BeginCast, or the one added for a proc) and until when
    // (now_ms) an OP_Action of it counts as that cast's landing rather than a proc.
    std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::int64_t>> cast_stage_shown_;
    std::vector<game::MissilePacket> missiles_;
    LandingEffects landing_;
    std::int32_t drunkenness_ = 0; // PcBase's drunkenness as HitBySpell's SPA 160 sets it
    std::uint32_t drunkenness_updates_ = 0;
    std::uint32_t drunkenness_motion_updates_ = 0;
    void send_eye_position(bool force);
    std::chrono::steady_clock::time_point last_eye_position_send_{};
    bool eye_motion_dirty_ = false;
    std::optional<MerchantSession> merchant_;
    std::uint64_t npc_session_serial_ = 0;
    std::int64_t merchant_check_ms_ = 0; // CMerchantWnd::NextRefreshTime: the range check every 500 ms
    std::optional<TradeSession> trade_;
    std::optional<BankSession> bank_;
    game::BankSnapshot bank_seen_;     // Hailstone: the bank as last seen (bank.dat)
    bool bank_live_ = false;
    std::uint64_t bank_view_revision_ = 0;
    void set_bank_live(bool live);
    // While bank_live(): bank_seen() becomes the inventory's bank now, saved to bank.dat (bank_snapshot_file).
    void capture_bank();
    void load_bank_snapshot();
    std::int64_t bank_check_ms_ = 0; // CBankWnd::NextRefreshTime
    game::BankCoins bank_coins_;
    std::array<std::uint32_t, 4> cursor_coins_{};
    std::optional<QuantityRequest> quantity_;
    struct ItemJob {
        std::vector<game::SortClick> clicks;
        std::size_t next = 0;
        std::int64_t next_ms = 0;
        ItemJobStatus status;
        // Destroy and Tribute: the current item's step (0 pick up, 1 destroy or donate, 2 wait for the tribute
        // master's answer, 3 put a refused donation back), the answer count before the donation and when to give up.
        int phase = 0;
        std::uint32_t answers = 0;
        std::int64_t deadline_ms = 0;
        bool guild_tribute = false;
        std::uint32_t tribute_master = 0;
        // A replacement stack of the same item must not inherit a destructive action.
        std::vector<std::pair<std::string, std::uint32_t>> expected;
    };
    std::optional<ItemJob> item_job_;
    std::string item_job_blocked(ItemJobScope scope) const; // why a job cannot run now (empty: it can)
    void update_item_job();
    void update_bulk_job(std::int64_t now); // Destroy and Tribute's steps
    void next_parcel_claim(); // Claim all: the next parcel, when none is being taken
    std::string parcel_send_blocked(const std::string& to) const;
    std::uint32_t parcel_money_picks_ = 0; // MerchantSession::ParcelMoneyPick::serial
    struct LastHit {
        std::uint32_t target = 0, source = 0;
        std::int32_t damage = 0;
        std::chrono::steady_clock::time_point when{};
    } last_hit_;

    // Social, guilds, raids, the chat server.
    game::SocialState social_;
    game::GuildState guild_;
    Tracking tracking_;
    void handle_track_reply(const AppPacket& p);
    void track_filter_all();
    game::RaidState raid_;
    std::optional<WhoResponse> last_who_;
    std::uint32_t who_count_ = 0;
    std::unique_ptr<ChatServer> chat_server_;
    std::optional<ChatServerInfo> chat_server_info_; // OP_SetChatServer2, until the zone is up
    std::string chat_server_target_;
    std::string afk_message_;
    bool afk_manual_ = false;            // the AFK flag came from /afk (the zone's own AFK: false)
    game::AfkKeepalive afk_keepalive_;
    void update_afk();
    void set_own_afk(bool afk);
    bool announce_ = false; // eqclient.ini [Defaults] Announce (Cmd_announce; not kept here)
    std::optional<std::string> window_toggle_;
    bool death_tip_pending_ = false;
    game::ChatFilters chat_filters_;
    game::ClientOptions client_options_;
    game::SeededChatRandom chat_random_;
    bool garbling_on_ = true;  // 0xddfd50, OP_LogServer +0x14 (EQEmu sends 1)
    bool chat_blocked_ = false; // 0xddf70d

    // CastSpell's state.
    std::function<bool(const glm::vec3&, const glm::vec3&)> line_of_sight_;
    TeleportHandler teleport_handler_;
    bool in_combat_ = false;          // PcClient +0x31bc
    std::uint32_t rest_downtime_ = 0; // +0x31c0, seconds
    std::int64_t rest_since_ms_ = 0;  // +0x31c4 (FastTime), 0 = no countdown
    bool hidden_ = false, sneaking_ = false;
    VisibilityCleared visibility_cleared_;
    std::int32_t invis_state_ = 0;    // the own spawn's +0x338 as the server's SpawnAppearance 3 sets it
    // CastSpell's live adapter (game_connection_cast.cpp) reads the state above.
    friend class CastQueries;
    friend class CombatAbilityQueries;
    friend struct GameConnectionTestAccess; // tests: state set up without a server
    // The first own effect slot whose spell has `spa`, or -1.
    int own_effect_with(std::int32_t spa) const;
    // CharacterZoneClient_RemoveAffect(slot, notify): the slot cleared and OP_Buff sent.
    void remove_own_effect(int slot);
    void load_chat_filters();
    void load_client_options();
    void apply_client_option(game::ClientOption id, int old_value);
    // Writes `id` to the character's options.ini (CEverQuest::WriteIntToClientINI and the like).
    void save_client_option(game::ClientOption id);
    // The anonymity flags set: the own spawn's +0x2b8, the spawn appearance 0x15 sent, Options/Anonymous kept
    // (SendAnonymous in GeneralPageWndNotification, Cmd_anonymous / Cmd_roleplay's Save).
    void set_anon_flags(int flags);
    // Options/Anonymous takes the flags (and is saved) without sending: Handle_OP_SpawnAppearance 0x15 for us.
    void keep_anon_flags(int flags);

    // Progress (AAs, training, tradeskills, tasks).
    game::AltAdvState alt_adv_;
    std::optional<game::TrainingSession> training_;
    std::uint32_t training_requested_ = 0; // OP_GMTraining sent to this guildmaster
    bool combine_pending_ = false;
    game::TradeskillState tradeskill_;
    game::RecipeFavorites recipe_favorites_{}; // CTradeSkillWnd +0x450, from the UI ini once (+0xc20)
    bool recipe_favorites_loaded_ = false;
    void open_tradeskill_world();
    void update_tradeskill_batch();
    // The cursor item into game::put_away_slot's place; false when there is none.
    bool put_away_cursor();
    game::TaskState tasks_;
    game::RewardSelection rewards_;
    game::Achievements achievements_;
    std::uint64_t achievement_view_intent_ = 0;
    std::optional<std::uint64_t> achievement_refresh_token_;
    std::optional<std::int64_t> achievement_refresh_requested_ms_;
    std::int64_t achievement_refresh_sent_ms_ = 0;
    std::string achievement_message_;
    std::optional<std::pair<std::uint32_t, std::uint64_t>> achievement_compare_target_, achievement_link_target_;
    std::optional<std::uint64_t> achievement_link_request_token_;
    std::array<std::optional<std::int64_t>, 2> reward_view_requested_ms_, reward_inspect_requested_ms_;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> task_selection_; // the task window's selected (type, slot)
    bool task_abandon_sent_ = false;
    bool task_compass_dirty_ = true;                     // CTaskWnd +0x1594
    std::optional<game::TaskCompassMark> task_compass_;  // +0x155c, +0x1574
    std::uint32_t task_compass_last_id_ = 0;             // +0x158c, the last mark's (task, slot)
    std::int32_t task_compass_last_slot_ = -1;           // +0x1590
    void update_task_compass();
    game::TextOverlay text_overlay_;
    bool aa_requested_ = false;
    std::uint32_t aa_buy_all_ = 0; // the rank a client-side "buy all ranks" is waiting on (0: none)
    mutable std::unique_ptr<game::mechanics::GameTables> tables_; // SkillCaps / BaseData, loaded when first needed
    std::int64_t sat_down_ms_ = 0;  // the spawn's +0x374: when the player sat down (0 standing)
    bool was_sitting_ = false;
    std::uint32_t tick_rand_ = 0;   // the buff tick's rand() state

    // Players.
    game::DuelState duel_;
    std::optional<game::InspectView> inspect_;
    std::int64_t inspect_check_ms_ = 0;
    bool inspect_others_ = true; // eqclient.ini [Defaults] InspectOthers (TRUE unless set)
    std::optional<Confirmation> confirmation_;
    std::vector<game::BandolierSet> bandolier_;
    std::optional<BookView> book_;
    std::optional<LargeDialog> large_dialog_;
    std::uint32_t target_of_target_ = 0;
    game::XTargetState xtargets_;
    bool spellbook_open_ = false;
    int spellbook_page_ = 0;
    bool autosplit_ = false;
    std::uint32_t chat_language_ = 0; // the chat window's language (CChatWindow +0x274, Cmd_language)

    // The bazaar.
    game::TraderState trader_;
    game::TraderPrices trader_prices_;
    game::BazaarSearchState bazaar_;
    bool bazaar_search_open_ = false;
    std::uint32_t trader_click_ = 0; // OP_TraderShop 28 sent to this trader
    int search_pending_ = 0;         // bazaar search purchases waiting for their answer
    game::BarterMerchantState barter_merchant_;
    game::BarterSearchState barter_search_;
    game::BarterWindowState barter_window_;
    std::optional<std::uint8_t> touching_switch_; // PlayerClient +0x2d4
    Titles titles_;
    Leadership leadership_;
    GroupMentor group_mentor_;
    GroupSearch group_search_;
    bool creating_group_ = false; // PcClient bIAmCreatingGroup (DoGroupInvite without a group)
    void group_search_error(int code); // GroupSearch_ShowError
    void send_lfg(const LfgCommand& v); // Send_OP_LFGCommand: also the local player's LFG flag
    std::map<std::uint32_t, AltCurrency> alt_currencies_;
    PointMerchant point_merchant_;
    std::array<std::vector<std::int32_t>, 2> blocked_buffs_; // player, pet
    void resend_blocked_buffs();
    Tribute tribute_;
    GuildTribute guild_tribute_;
    struct PendingTributeDonation {
        bool guild = false;
        ItemSlot slot;
        std::uint32_t quantity = 0, master_id = 0;
        Item item;
        std::int64_t expires_ms = 0; // session_now_ms() after which no answer is expected any more
    };
    // Neither reply carries an instance or transaction ID. Keep this single request even after a batch timeout,
    // so a late reply cannot be mistaken for another donation. Other inventory actions remain available.
    // EQEmu answers in the handler or not at all (a master out of range or gone, no guild), so the request is
    // also dropped when that channel's master window is answered again (the answer to an open sent after the
    // donation comes after the donation's own) and after kTributeReplyLimitMs.
    std::optional<PendingTributeDonation> pending_tribute_donation_;
    static constexpr std::int64_t kTributeReplyLimitMs = 30000;
    // The session's clock (ConnectionOptions::session.clock when a test sets one) in ms, on now_ms()'s scale.
    std::int64_t session_now_ms() const;
    bool tribute_donation_waiting() const {
        return pending_tribute_donation_ && session_now_ms() < pending_tribute_donation_->expires_ms;
    }
    void expire_tribute_donation();
    bool begin_tribute_donation(bool guild, const Item& item, std::uint32_t master_id);
    bool finish_tribute_donation(bool guild, const ItemSlot& slot, std::uint32_t quantity,
                                std::uint32_t master_id, bool consumed);
    void reset_tribute_donations() { pending_tribute_donation_.reset(); }
    std::uint32_t guild_platinum_pending_ = 0; // a guild platinum donation awaiting its answer
    void handle_tribute(Op op, const AppPacket& p);
    Expedition expedition_;
    Adventure adventure_;
    RaidOptions raid_options_;
    bool target_of_target_open_ = false;
    game::PlayerPath path_;
    game::PlayerPath::FloorFn path_floor_;
    std::optional<FailedFind> failed_find_;
    std::uint32_t find_replies_ = 0;
    std::unordered_map<std::uint32_t, FindableNpc> findable_npcs_;
    std::unordered_map<std::string, std::uint8_t> quest_npcs_;
    bool quest_npcs_asked_ = false;
    bool hailstone_script_ = false;   // a zone answered OP_HailstoneSettingsRequest
    bool zone_links_asked_ = false;
    std::optional<ZoneLinks> zone_links_;
    std::uint32_t zone_links_serial_ = 0;
    std::optional<glm::vec3> path_finish_;
    bool path_settings_loaded_ = false;
    game::PathSettings path_settings_;
    // Send_OP_FindPersonRequest; `found`: a new find (the stop sound, then PlayerPath_LoadSettings).
    void send_find(const game::FindRequest& r, bool found = true);
    void path_reply(const AppPacket& p);        // PlayerPath_HandleFindPersonReply
    void path_tick();                           // PlayerPath_Update
    void play_wave(std::uint32_t id);           // EqSoundManager::WavePlay(id, 0)
    GuildBank guild_bank_;
    game::RealEstateManager real_estate_;
    RealEstateUi re_ui_;
    game::Placement placement_;
    std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> re_npc_items_; // spawn -> {re, item}
    Aggro aggro_;
    bool bug_report_open_ = false;
    bool feedback_open_ = false;
    // Cmd_report's 0xddf640: 1 once the zone is entered (the 0x345d step), 2 once a report went out.
    int report_state_ = 0;
    // CEverQuest's alreadyAutoJoined: cleared by each chat server connect (ChatServerConnect @0x5135b0).
    bool autojoined_ = true;
    std::function<std::optional<std::string>(std::string_view, std::string_view)> ui_ini_get_;
    std::function<void(std::string_view, std::string_view, const std::string&)> ui_ini_put_;
    std::function<std::string()> visible_chat_text_;
    Claims claims_;
    std::int64_t stun_timer_ms_ = 0; // the spawn's +0x43c: when the stun ends (0: none running)
    bool stun_state_ = false;        // CharacterZoneClient +0x100 = 1
    std::uint32_t speed_stops_ = 0, turn_stops_ = 0, stun_leans_ = 0;
    // CharacterZoneClient::StunMe @0x4432c0 and CharacterZoneClient_EndStun @0x4433d0 (game_connection_ticks.cpp).
    void stun_me(std::uint32_t duration_ms, bool lean);
    void end_stun();
    bool alarm_open_ = false;
    std::int64_t alarm_due_ms_ = 0;
    std::array<NameChangeWindow, kNameKinds> name_change_{};
    std::uint32_t world_expansions_ = 0;
    std::vector<JournalLine> journal_lines_;
    std::optional<std::string> random_name_;
    std::vector<int> context_tips_;
    std::vector<std::string> web_links_;
    void context_tip(int tip) { context_tips_.push_back(tip); }
    std::function<int(std::string_view, std::string_view, int)> char_ini_get_;
    std::function<void(std::string_view, std::string_view, const std::string&)> char_ini_put_;
    bool handle_name_change(const AppPacket& p);
    bool handle_group_search(const AppPacket& p);
    bool handle_marketplace(const AppPacket& p);
    void marketplace_build();                     // the constructor's tail (0x6e18eb): 0x5bd5, 0x5148, 0x76c3, 0x356f
    void marketplace_send_preview();              // the model's preview request (SendItemPreview @0x6d86c0)
    void marketplace_result(const MarketplacePurchaseResult& r, bool timeout); // HandlePurchaseResult
    void marketplace_set_funds(std::uint32_t balance); // SetAvailableFunds @0x6d7f70
    void marketplace_popup(std::string text);     // a ConfirmationDialog of the window's (OK, wnd +0x315)
    void open_name_change(int kind, bool remind);
    void set_name_reminder_disabled(int kind, bool disabled);
    void answer_name_change(const Confirmation& c, bool yes);
    bool consume_pending_ = false; // an OP_Consume awaits the server's OP_Stamina
    // Server offers waiting behind the question on screen (one ConfirmationDialog at a time).
    std::deque<Confirmation> confirm_queue_;
    void ask(Confirmation c);
    // Drops the questions `which` picks (all of them without one), on screen and waiting, without answering: the state
    // an open question holds (the loot window's popup flag, an augment's pending request, ...) is taken back as a no
    // would, but nothing is sent. The next waiting question that stays takes the screen.
    void discard_questions(const std::function<bool(const Confirmation&)>& which = {});
    void unwind_question(const Confirmation& c);
    void auto_consume();
    PvP pvp_;
    LFGuild lfguild_;
    std::optional<CrystalCounts> crystals_;
    std::vector<AuraUpdate> auras_;
    bool aggro_seen_ = false; // the server has sent the meter (it opens the window once)
    AdventureMerchant adventure_merchant_;
    void point_sale(const PointSale& s); // a point merchant's sale: the sold charges leave the inventory
    void handle_guild_bank(Op op, const AppPacket& p);
    void guild_bank_tick();
    // The real estate packets (both dispatchers' cases); false: not one of them.
    bool handle_real_estate(const AppPacket& p);
    void real_estate_tick();
    void reset_real_estate_for_zone();
    void server_coin(Op op, const AppPacket& p); // begging and pick-pocket coin (net/game_connection_abilities.cpp)
    void handle_adventure(Op op, const AppPacket& p);
    void handle_dz(Op op, const AppPacket& p);
    void answer_dz_invite(bool accept);
    bool potion_belt_open_ = false;
    std::optional<ItemSlot> find_possession(std::uint32_t item_id) const;

    // Mail.
    game::MailState mail_;
    game::MercManager merc_;
    std::array<bool, 3> merc_windows_{}; // merchant, manage, info
    std::uint32_t merc_list_serial_ = 0;

    // Groups and pets.
    game::GroupState group_;
    // The local player's id and the group then raid member names Appearance::new_armor_bits were last made with.
    std::uint32_t new_armor_player_ = 0;
    std::vector<std::string> new_armor_members_;
    game::PetState pets_;
    std::vector<std::pair<std::uint32_t, game::BuffSlot>> pet_buffs_;
    game::PetToggleStates pet_toggles_{};
    PetWindowLink pet_window_;
    bool pet_present_ = false; // CPetInfoWnd +0x764: there was a pet (PetID set and its spawn here)
    // RefreshPetState's has-pet test after PetID changes: when a pet comes, ReissueToggledPetCommands.
    void update_pet_presence();

    std::chrono::steady_clock::time_point camp_started_{};
    int camp_notices_ = 0;          // 12684 lines shown (one per 5 s)
    std::uint32_t camp_kind_ = 0;   // 0xdd2664 (Cmd_camp)
    std::optional<std::uint32_t> camped_out_;
    std::optional<LinkDeath> link_death_;
    std::int32_t air_supply_ = 0;
    bool waiting_for_port_ = false; // PlayerZoneClient +0x1f05
    std::int64_t respawn_shown_ms_ = 0;
    bool respawn_chosen_ = false;
};

} // namespace hailstone::net
