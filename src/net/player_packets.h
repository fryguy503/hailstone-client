#pragma once

// Packets between players and the small per-player commands: duels, inspect,
// surname, corpse summoning and dragging, shielding, the key ring, /rewind,
// the bandolier and reading books. Layouts follow the original's senders and
// readers (addresses at each struct) and the NMS server's
// EQEmu RoF2 translator (common/patches/rof2.cpp, rof2_structs.h, zone/
// client_packet.cpp). Per-packet notes: docs/protocol/rof2-packets.md
// "Duels, inspect and player commands".

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "net/item_packets.h"
#include "net/opcodes.h"
#include "net/packet.h"

namespace hailstone::net {

// ---------------------------------------------------------------- duels

// OP_RequestDuel (8 bytes both ways, Duel_Struct). Cmd_duel @0x4f6930 sends
// {target, me}; EQEmu swaps the two and forwards {challenger, challenger} to
// the target, whose Handle_OP_RequestDuel @0x4b67c0 reads the challenger at +4.
// When the target accepts, the server sends the challenger {accepter, accepter}.
struct DuelRequest {
    std::uint32_t first = 0;  // client → server: the target; server → client: the challenger
    std::uint32_t second = 0; // client → server: our id; server → client: the challenger again
};
inline constexpr std::size_t kDuelRequestSize = 8;
AppPacket encode(const DuelRequest& v);
std::optional<DuelRequest> decode_duel_request(const AppPacket& p);
// OP_DuelAccept (8 bytes): Cmd_duel's acceptance {challenger, me}, sent right
// after an OP_RequestDuel with the same body.
AppPacket encode_duel_accept(std::uint32_t challenger, std::uint32_t me);
// OP_DuelDecline (8 bytes from the original: {partner, me}). The original
// sends it only when the duel partner dies by our hand (Handle_OP_Death
// @0x532b50) or despawns while the duel runs (msgEQRmPlayer @0x4b8380).
// EQEmu's handler wants 12 bytes (DuelResponse_Struct) and drops it.
AppPacket encode_duel_decline(std::uint32_t partner, std::uint32_t me);
// The same opcode in EQEmu's 12 bytes (DuelResponse_Struct {target_id, entity_id, unknown}): EQEmu has no handler
// for the original's refusal (0x52f7), so a refusal of kind 0 also sends this {challenger, me, 0}. EQEmu's
// Handle_OP_DuelDecline then clears both players' duel state (else the challenger can challenge nobody until it
// zones) and tells the challenger "%1 has declined your challenge to duel to the death." (1383, the line the
// original's refusal prints). Its handler does not check that the challenger still exists, so it is sent only for a
// challenger in our spawn list.
AppPacket encode_duel_decline_eqemu(std::uint32_t challenger, std::uint32_t me);
inline constexpr std::size_t kDuelResponseSize = 12;

// Opcode 0x52f7 (no name in EQEmu's RoF2 table; the NMS server does not know
// it): the original's duel refusal, 12 bytes {u32 challenger, u32 me, u8 kind,
// 3 bytes the original leaves uninitialised}. Kind 0: ignored or declined
// (/decline, Cmd_decline @0x4f67b0), 1: already dueling, 2: considering
// another duel (Handle_OP_RequestDuel). The receiver prints eqstr 1383 + kind
// when the sender is the player it challenged (HandleWorldMessage @0x4c76b1).
// Kind '*' (0x2a) carries a time instead and is not a duel message.
inline constexpr std::uint16_t kOpDuelRefusal = 0x52f7;
struct DuelRefusal {
    std::uint32_t challenger = 0; // the receiver reads the refusing player at +4
    std::uint32_t refuser = 0;
    std::uint8_t kind = 0;
};
inline constexpr std::size_t kDuelRefusalSize = 12;
AppPacket encode(const DuelRefusal& v);
std::optional<DuelRefusal> decode_duel_refusal(const AppPacket& p);

// ---------------------------------------------------------------- inspect

// OP_InspectRequest (8 bytes, Inspect_Struct): Send_OP_InspectRequest
// @0x51fa40 sends {target, me}. EQEmu answers itself for RoF2 targets
// (Client::ProcessInspectRequest) and tells the target "<name> is looking at
// your equipment...".
AppPacket encode_inspect_request(std::uint32_t target, std::uint32_t me);
struct InspectRequest {
    std::uint32_t target = 0, player = 0;
};
std::optional<InspectRequest> decode_inspect_request(const AppPacket& p);

// OP_InspectAnswer (1860 bytes, InspectResponse_Struct): u32 requester, u32
// the inspected player, 23 × char[64] worn item names (empty = none), 23 ×
// u32 icons (0xffffffff = none), char[288] the player's inspect message.
// Handle_OP_InspectAnswer @0x68f9f0 reads the inspected player at +4.
inline constexpr int kInspectSlots = 23;
struct InspectAnswer {
    std::uint32_t requester = 0;
    std::uint32_t player = 0;
    std::array<std::string, kInspectSlots> names;
    std::array<std::uint32_t, kInspectSlots> icons{};
    std::string text;
};
inline constexpr std::size_t kInspectAnswerSize = 1860;
AppPacket encode(const InspectAnswer& v); // server direction, for tests
std::optional<InspectAnswer> decode_inspect_answer(const AppPacket& p);

// OP_InspectMessageUpdate (256 bytes, InspectMessage_Struct): our own inspect
// message, sent when the inspect window of ourselves is edited
// (CInspectWnd_SaveInspectMessage @0x68f520).
inline constexpr std::size_t kInspectMessageSize = 256;
AppPacket encode_inspect_message(const std::string& text);

// ---------------------------------------------------------------- player commands

// OP_Surname (100 bytes, Surname_Struct): name[64], u32 unknown, lastname[32].
// /surname confirms first (Cmd_surname_ShowConfirm @0x517b60, eqstr 9050);
// the server answers with the same struct (unknown 1 = accepted).
struct Surname {
    std::string name;
    std::uint32_t unknown64 = 0;
    std::string last_name;
};
inline constexpr std::size_t kSurnameSize = 100;
AppPacket encode(const Surname& v);
std::optional<Surname> decode_surname(const AppPacket& p);

// The 152-byte body SendCorpseCommandForTarget @0x478d80 sends for /corpse
// (OP_GMSummon) and /corpsedrag (OP_CorpseDrag): GMSummon_Struct with the
// corpse's name, our name, success 0, our zone id and the corpse's y, x, z
// (the original copies the floats' bits).
struct CorpseCommand {
    std::string corpse_name;
    std::string player_name;
    std::uint32_t zone_id = 0;
    float y = 0, x = 0, z = 0;
};
inline constexpr std::size_t kCorpseCommandSize = 152;
AppPacket encode_corpse_command(Op op, const CorpseCommand& v);
// /corpsedrop (Cmd_corpsedrop @0x4781b0): OP_CorpseDrop with the target's
// name and its NUL (an empty name gives a lone NUL).
AppPacket encode_corpse_drop(const std::string& target_name);

// /shield (Cmd_shield @0x4e8d60): OP_Shielding with the target's spawn id (4 bytes).
AppPacket encode_shielding(std::uint32_t target);
// /shieldgroup (Cmd_shieldgroup @0x4e8fd0): opcode 0x5a79 (no name in EQEmu's RoF2 table; EQEmu does not handle
// it), 28 bytes: the spawn ids of the group members in the zone (at most 6, the rest 0), then their count.
inline constexpr std::uint16_t kOpShieldGroup = 0x5a79;
AppPacket encode_shield_group(const std::vector<std::uint32_t>& members);
// /sheathe (Cmd_sheathe @0x4de600): OP_PlayerStateAdd / OP_PlayerStateRemove {u32 spawn id, u32 state bit} (8
// bytes, EQEmu PlayerState_Struct); the same packets come back for other players (Handle_OP_PlayerStateAdd @0x4b6d90
// ORs the bit into the spawn's +0x14c, ...Remove @0x4b6dc0 clears it).
inline constexpr std::uint32_t kPlayerStateSheathed = 0x2; // *0x9d77a0
// An instrument (item types 23..26) in the primary or secondary hand: the original's 0x5923f0 sets it for the local
// player and sends OP_PlayerStateAdd / OP_PlayerStateRemove on the transition; the server relays it for others.
inline constexpr std::uint32_t kPlayerStateInstrument = 0x10; // *0x9d77ac
struct PlayerState {
    std::uint32_t spawn_id = 0;
    std::uint32_t state = 0;
};
AppPacket encode_player_state(bool add, const PlayerState& v);
std::optional<PlayerState> decode_player_state(const AppPacket& p);
// /hidecorpses (cmd_hidecorpses @0x4e78c0) and the loot window's hide (CLootWnd::AboutToHide @0x6bcd40):
// OP_HideCorpse {u32 action, u32 corpse id} (8 bytes, EQEmu HideCorpse_Struct). Actions: 0 none, 1 all, 2 all but
// the group, 3 looted (also sent at every zone-in while it is on), 4 the corpse just looted, 5 NPCs.
inline constexpr std::uint32_t kHideCorpseNone = 0, kHideCorpseAll = 1, kHideCorpseAllButGroup = 2,
                               kHideCorpseLooted = 3, kHideCorpseThis = 4, kHideCorpseNpc = 5;
AppPacket encode_hide_corpse(std::uint32_t action, std::uint32_t corpse_id = 0);
// /decaycorpse's yes (the dialog handler @0x479e30): opcode 0x7d50 (no name in EQEmu's RoF2 table; EQEmu does not
// handle it), 8 bytes {u32 the corpse's spawn id, u32 ours}.
inline constexpr std::uint16_t kOpDecayCorpse = 0x7d50;
AppPacket encode_decay_corpse(std::uint32_t corpse_id, std::uint32_t player_id);
// /spellscribe (Cmd_spellscribe @0x4f7b90): opcode 0x51e5 (no name in EQEmu's RoF2 table; EQEmu does not handle it),
// 0x88 bytes: char[0x40] at +0 and +0x40, u32 4 at +0x80, 4 zero bytes. The original copies the player's name to +0
// and the target's (or the player's own) to +0x40, then the local player's name over both: a longer target name's
// tail stays after the NUL at +0x40.
inline constexpr std::uint16_t kOpSpellScribe = 0x51e5;
inline constexpr std::size_t kSpellScribeSize = 0x88;
AppPacket encode_spell_scribe(const std::string& self, const std::string& other);
// /offlinemode (Cmd_offlinemode @0x4e6660) and its question's yes: opcode 0x53d3 alone; 0x5369 from the server makes
// the client forget the request (HandleWorldMessage @0x4cb984). Neither is in EQEmu's RoF2 table.
inline constexpr std::uint16_t kOpOfflineMode = 0x53d3, kOpOfflineModeRefused = 0x5369;
// /setstartcity (Cmd_setstartcity @0x4deac0): OP_SetStartCity with the argument text and its NUL (EQEmu reads a
// number; anything else lists the cities).
AppPacket encode_set_start_city(const std::string& text);
// /reveal (Cmd_reveal @0x4e7e30): opcode 0x15e7 with no body (no name in EQEmu's RoF2 table; EQEmu does not handle
// it).
inline constexpr std::uint16_t kOpReveal = 0x15e7;
AppPacket encode_reveal();
// OP_FaceChange (C->Z, CPlayerCustomizationWnd's Accept, WndNotification @0x715f30) and OP_SetFace (Z->C, another
// player's: Handle_OP_SetFace @0x4b6b00), 24 bytes (EQEmu FaceChange_Struct): u8 hair colour, beard colour, eye 1,
// eye 2, hair style, beard, face, one byte the original leaves; u32 heritage, tattoo, details, and the spawn id
// (the original sends a stack value there; the server fills it for OP_SetFace).
struct FaceChange {
    std::uint8_t hair_color = 0, beard_color = 0, eye1 = 0, eye2 = 0, hair_style = 0, beard = 0, face = 0;
    std::uint32_t heritage = 0, tattoo = 0, details = 0;
    std::uint32_t spawn_id = 0;
};
inline constexpr std::size_t kFaceChangeSize = 24;
AppPacket encode(const FaceChange& v);
std::optional<FaceChange> decode_face_change(const AppPacket& p);
// /keys (Cmd_keys @0x4e4ff0): OP_KeyRing with "KEY" and its NUL.
AppPacket encode_key_ring_request();
// /rewind (Cmd_rewind @0x4e6d00): an empty OP_Rewind.
AppPacket encode_rewind();
// /resetaas (Cmd_resetaas @0x4782e0): an empty OP_ResetAA.
AppPacket encode_reset_aa();
// Send_OP_Petition @0x4dc890: OP_Petition with the text and its NUL; nullopt for a text of 0x400 characters or
// more, which the original does not send.
std::optional<AppPacket> encode_petition(const std::string& text);

// ---------------------------------------------------------------- reports and server requests

// /report (Cmd_report @0x4eff50): OP_Report, "<player>|<reporter>|<the chat windows' visible text>" with its NUL,
// the text cut at 0x2004 characters.
AppPacket encode_report(std::string text);
// The feedback window's Send (CFeedbackWnd::WndNotification @0x667830): OP_Feedback, 0x47c bytes: the character's
// name (char[0x40], +0x40 a zero byte), y, x, z (floats, +0x60), the heading (int, +0x6c), the text (char[0x400],
// +0x78), 4 more bytes. The gaps are the original's uninitialised stack (0 here).
struct Feedback {
    std::string name;
    float y = 0, x = 0, z = 0;
    std::int32_t heading = 0;
    std::string text;
};
inline constexpr std::size_t kFeedbackSize = 0x47c;
AppPacket encode(const Feedback& v);
std::optional<Feedback> decode_feedback(const AppPacket& p); // tests

// Opcodes not in EQEmu's RoF2 table. /uptime (Cmd_uptime @0x4785b0) and /servertransfer (Cmd_servertransfer
// @0x4e82d0) send the opcode alone; the veteran reward claim (ConfirmationDialog_HandleResponse 0x7e @0x52d540) sends
// 0x7d89 with 0x48 bytes of the original's uninitialised stack (zeros here); /acompare (Send_AchievementCompareRequest
// @0x51fb90) sends 0x1665 {u32 the target's spawn id, u32 ours}.
inline constexpr std::uint16_t kOpUptime = 0x344f, kOpServerTransfer = 0x2b8e, kOpVeteranRewardClaim = 0x7d89,
                               kOpAchievementCompare = 0x1665;
AppPacket encode_veteran_reward_claim();
AppPacket encode_achievement_compare(std::uint32_t target, std::uint32_t me);
// The answer to /uptime (HandleWorldMessage 0x344f @0x4ccd40): {u32 the world's seconds, u32 the zone's}, each shown
// by Handle_Opcode_0x344f @0x4b8dd0 as "%02d:%02d:%02d:%02d" (days, hours, minutes, seconds) in eqstr 4102 / 4103.
struct Uptime {
    std::uint32_t world_seconds = 0, zone_seconds = 0;
};
std::optional<Uptime> decode_uptime(const AppPacket& p);
AppPacket encode(const Uptime& v); // server direction, for tests
std::string format_uptime(std::uint32_t seconds);

// ---------------------------------------------------------------- bandolier

// OP_Bandolier (40 bytes): u32 action, u8 set number, char name[32] (create
// only), 3 trailing bytes. Actions: 0 create (PcZoneClient_CreateBandolierSet
// @0x584280, the name and the four weapon slots' items are stored), 1 remove
// (Send_OP_Bandolier @0x57fe70), 2 activate (PcZoneClient::BandolierSwap
// @0x585aa0). The server sends action 4 back to rename item references
// (Handle_OP_Bandolier @0x6266f0; EQEmu never does).
inline constexpr std::uint32_t kBandolierCreate = 0, kBandolierRemove = 1, kBandolierSet = 2;
inline constexpr std::size_t kBandolierSize = 40;
AppPacket encode_bandolier(std::uint32_t action, std::uint8_t number, const std::string& name = {});
// The bandolier window's right button held on a set's slot (CBandolierWnd::WndNotification @0x626980, XWM 0x18):
// 12 bytes {u32 3, u8 set, u8 slot, two bytes the original leaves unset (0 here), u32 Shift ? 2 : 0}. (EQEmu's
// handler wants the 40-byte struct and drops it.)
AppPacket encode_bandolier_hold(std::uint8_t set, std::uint8_t slot, bool shift);
struct BandolierPacket {
    std::uint32_t action = 0;
    std::uint8_t number = 0;
    std::string name;
};
std::optional<BandolierPacket> decode_bandolier(const AppPacket& p);

// ---------------------------------------------------------------- books

// OP_ReadBook (8216 bytes, BookRequest_Struct) both ways: u32 window
// (0xffffffff: a new window), the book's slot (TypelessInventorySlot: i16
// slot, i16 bag sub-slot, i16 augment, i16 unknown), u32 type (0 note, 1 book, 2 item info),
// u32 target, u8 can cast, u8 can scribe, char[8194] the text file (request)
// or the text (answer). CInvSlot::HandleRButtonUp @0x697250 requests a book
// item's file (item_def +0x526) with its book type; Handle_OP_ReadBook
// @0x4c0ea0 shows the answer in the note window (type 0), the book window (1)
// or an item display (2).
struct ReadBook {
    std::uint32_t window = 0xffffffff;
    std::int16_t slot = -1, sub = -1, aug = -1, slot_unknown = 0;
    std::uint32_t type = 0;
    std::uint32_t target = 0;
    std::uint8_t can_cast = 0, can_scribe = 0;
    std::string text;
};
inline constexpr std::size_t kReadBookSize = 8216;
AppPacket encode(const ReadBook& v);
std::optional<ReadBook> decode_read_book(const AppPacket& p);

// OP_BookButton (16 bytes): the book and note windows' Cast Spell / Scribe
// buttons (CBookWnd::WndNotification @0x6409e0, CNoteWnd @0x6ff520): the
// book's slot {slot, sub, aug, 0}, the target's spawn id, 0 (EQEmu
// BookButton_Struct: TypelessInventorySlot, target_id, unused).
AppPacket encode_book_button(std::int16_t slot, std::int16_t sub, std::int16_t aug, std::uint32_t target);

// ---------------------------------------------------------------- the large dialog

// OP_OnLevelMessage (Handle_OP_OnLevelMessage @0x755350, read with
// SerializeBuffer): four counted strings (title, text, the yes and no
// buttons' labels), u8 buttons (Yes / No instead of OK), u8 sound controls,
// u32 duration (ms; 0 stays), u32 the popup id (OK and Yes answer it), u32
// the negative id (No), u32 the id a close or timeout answers, u32 the text's
// language. EQEmu (rof2.cpp ENCODE(OP_OnLevelMessage)) fills the first two ids
// and leaves the rest 0.
struct LevelMessage {
    std::string title, text, yes, no;
    std::uint8_t buttons = 0, sound = 0;
    std::uint32_t duration = 0, popup_id = 0, negative_id = 0, close_id = 0, language = 0;
};
std::optional<LevelMessage> decode_level_message(const AppPacket& p);
// OP_PopupResponse (8 bytes, Send_OP_PopupResponse @0x755080): {kind, id};
// kind 0 OK / Yes with the popup id, 1 No with the negative id, 2 closed or
// timed out with the close id. Sent only for a non-zero id.
AppPacket encode_popup_response(std::uint32_t kind, std::uint32_t id);

// ---------------------------------------------------------------- titles
//
// EQEmu's layouts (zone/titles.cpp, passed through RoF2's translator); the original's CTitleWnd is not decoded.

// OP_RequestTitles (empty, client → zone) is answered by OP_SendTitleList: u32 count, count × {u32 id, prefix,
// suffix} (either may be empty): the titles this character may take.
struct TitleEntry {
    std::uint32_t id = 0;
    std::string prefix, suffix;
};
AppPacket encode_request_titles();
std::optional<std::vector<TitleEntry>> decode_title_list(const AppPacket& p);
AppPacket encode_title_list(const std::vector<TitleEntry>& v); // server direction, for tests

// OP_SetTitle (8 bytes, client → zone): u32 suffix (0 the prefix, 1 the suffix), u32 title id (0 clears it).
AppPacket encode_set_title(bool suffix, std::uint32_t title_id);

// OP_SetTitleReply (40 bytes, zone → client, to everyone near): u32 suffix, char[32] text, u32 spawn id.
struct TitleReply {
    bool suffix = false;
    std::string text;
    std::uint32_t spawn_id = 0;
};
std::optional<TitleReply> decode_title_reply(const AppPacket& p);
AppPacket encode(const TitleReply& v); // server direction, for tests

// OP_PotionBelt (12 bytes, client → zone, EQEmu MovePotionToBelt_Struct): u32 action (0 put the item in the slot,
// 1 clear it), u32 slot, u32 item id. The belt itself comes in the player profile.
AppPacket encode_potion_belt(bool clear, std::uint32_t slot, std::uint32_t item_id);
// CPotionBeltWnd's right button held on a slot (XWM_RCLICKHOLD): {3, slot, Shift ? 2 : 0}. EQEmu clears the slot for
// any action but 0.
AppPacket encode_potion_belt_hold(std::uint32_t slot, bool shift);

// OP_Dye (C→Z, 36 bytes, EQ::TintProfile): the nine slots' tints as 0xUURRGGBB (UU 0xff tinted). EQEmu dyes each
// slot whose colour changed, using one Vial of Prismatic Dye (item 32557) each, sends the wear changes, and answers
// with an empty OP_Dye.
inline constexpr std::uint32_t kPrismaticDye = 32557;
AppPacket encode_dye(const std::array<std::uint32_t, 9>& tints);

// OP_Bug (C→Z, 8740 bytes, EQEmu BugReport_Struct; not translated for RoF2): u32 category, char[64] its name,
// char[64] the reporter, 32, char[128] UI path, f32 x, y, z (server order), u32 heading, u32, u32 minutes played, 8,
// u32 target id, 140, u32, char[64] target name, u32 flags (1 can duplicate, 2 crash, 4 target info, 8 character
// flags), 2052, char[2050] the report, char[4098] system info. EQEmu's categories 0..12: Other, Video, Audio,
// Pathing, Quest, Tradeskills, Spell Stacking, Doors and Portals, Items, NPC, Dialogs, LoN - TCG, Mercenaries.
inline constexpr int kBugCategories = 13;
const char* bug_category_name(std::uint32_t category);
inline constexpr std::uint32_t kBugRepeatable = 1, kBugCrash = 2, kBugTargetInfo = 4, kBugCharacterFlags = 8;
inline constexpr std::size_t kBugReportSize = 8740;
struct BugReport {
    std::uint32_t category = 0;
    std::string reporter;
    float x = 0, y = 0, z = 0; // server coordinates
    std::uint32_t heading = 0, minutes_played = 0, target_id = 0, flags = 0;
    std::string target_name, text, system_info;
};
AppPacket encode(const BugReport& v);

// OP_PVPStats (Z→C, 4612 bytes, EQEmu PVPStats_Struct, sent at zone-in and on PvP points): kills, deaths, points
// available, career points, best kill streak, worst death streak, current streak, infamy, vitality, then 88-byte
// entries {char[64] name, level, race, class, zone, time, points}: the last death, the last kill, 50 kills in the
// last 24 hours (EQEmu fills only the counts).
struct PvPEntry {
    std::string name;
    std::uint32_t level = 0, race = 0, class_ = 0, zone = 0, time = 0, points = 0;
};
struct PvPStats {
    std::uint32_t kills = 0, deaths = 0, points = 0, career_points = 0, best_streak = 0, worst_streak = 0,
                  current_streak = 0, infamy = 0, vitality = 0;
    PvPEntry last_death, last_kill;
    std::vector<PvPEntry> recent_kills; // the named ones
};
std::optional<PvPStats> decode_pvp_stats(const AppPacket& p);
AppPacket encode(const PvPStats& v); // server direction, for tests
// OP_PVPLeaderBoardRequest (C→Z, 4 bytes: sort 0 kills, 1 points, 2 infamy); OP_PVPLeaderBoardReply (Z→C, 8024
// bytes): u32, our kills, points, rank, deaths, infamy, 100 × {char[64] name, kills, deaths, points, infamy}
// (EQEmu sends it zeroed: not recorded).
AppPacket encode_pvp_leaderboard_request(std::uint32_t sort);
struct PvPLeader {
    std::string name;
    std::uint32_t kills = 0, deaths = 0, points = 0, infamy = 0;
};
struct PvPLeaderboard {
    std::uint32_t kills = 0, points = 0, rank = 0, deaths = 0, infamy = 0;
    std::vector<PvPLeader> rows; // the named ones
};
std::optional<PvPLeaderboard> decode_pvp_leaderboard(const AppPacket& p);
// OP_CrystalCountUpdate (Z→C, 16 bytes, RoF2 order): radiant now, radiant career, ebon now, ebon career.
struct CrystalCounts {
    std::uint32_t radiant = 0, radiant_career = 0, ebon = 0, ebon_career = 0;
};
std::optional<CrystalCounts> decode_crystal_counts(const AppPacket& p);

// Veteran rewards. OP_VetClaimRequest (C→Z, 80 bytes, RoF2 VeteranClaim: char[68], u32 claim id (0xffffffff asks
// for the list), u32, u32 action); OP_VetRewardsAvaliable (Z→C): u32 count, count × {u32 claim id, u32 available,
// u32 item count, u8 enabled, item count × {u32 name length, the name (not NUL-terminated), u32 item id, u32
// charges}}; OP_VetClaimReply (Z→C, 80 bytes): the same struct, action 0 claimed, 1 refused.
inline constexpr std::uint32_t kVetClaimList = 0xffffffff;
AppPacket encode_vet_claim(std::uint32_t claim_id);
struct VetRewardItem {
    std::string name;
    std::uint32_t item_id = 0, charges = 0;
};
struct VetReward {
    std::uint32_t claim_id = 0, available = 0;
    bool enabled = false;
    std::vector<VetRewardItem> items;
};
std::optional<std::vector<VetReward>> decode_vet_rewards(const AppPacket& p);
AppPacket encode(const std::vector<VetReward>& v); // server direction, for tests
struct VetClaimReply {
    std::uint32_t claim_id = 0;
    bool claimed = false;
};
std::optional<VetClaimReply> decode_vet_claim_reply(const AppPacket& p);

} // namespace hailstone::net
