#pragma once

// The trade window's ready flags (CTradeWnd +0x2f8 the partner's, +0x2f9
// ours): follows the original's offer handling,
// checked against the CTradeWnd_*.jsonl vectors. Every change to our own
// offer (coin clicked or dropped in, an item moved into or out of a trade
// slot) and every change to the partner's coin clears both flags; the
// partner un-readying clears ours too.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hailstone::game {

struct Spawn;

// CEverQuest::DropItemOrMoneyOnPlayer refuses direct hand-ins to these NPC services (eqstr 8505).
bool refuses_direct_items(const Spawn& spawn);

struct TradeFlags {
    std::uint8_t his_ready = 0, my_ready = 0;
};

// A coin move CTradeWnd asks for: CEverQuest::MoveMoney(0, 3, from, to, amount, 0) (cursor to trade).
struct TradeMoneyMove {
    std::uint32_t from_coin = 0, to_coin = 0;
    std::int32_t amount = 0;
};

// CTradeWnd::SetHisReadyTrade @0x77f120.
void trade_set_his_ready(TradeFlags& f, std::uint8_t ready);
// CTradeWnd::SetHisMoney @0x77f140 (the coin stored by the caller): both flags clear.
inline void trade_set_his_money(TradeFlags& f) { f = {}; }
// CTradeWnd_OnTradeItemMoved @0x77f650: location type 3 is a trade slot. Returns whether the flags cleared.
bool trade_item_moved(TradeFlags& f, std::int32_t from_location, std::int32_t to_location);
// CTradeWnd::ClickedMoneyButton @0x77f560: the cursor's coin type (1 platinum .. 4 copper, 0 none) and the
// profile's cursor coins (platinum first); all of the cursor's coin goes into trade coin slot `coin`.
// Returns the move, or none for an empty cursor.
bool trade_clicked_money_button(TradeFlags& f, std::uint8_t cursor_type, const std::array<std::int32_t, 4>& cursor,
                                std::uint32_t coin, TradeMoneyMove& move);
// CTradeWnd_DropCursorMoneyIntoTrade @0x77fda0: into the slot of the cursor's own coin type.
bool trade_drop_cursor_money(TradeFlags& f, std::uint8_t cursor_type, const std::array<std::int32_t, 4>& cursor,
                             TradeMoneyMove& move);

// ---- who may trade (the recorded vectors CEverQuest_IsOkToTransact.jsonl, Handle_OP_TradeRequest.jsonl)

// CEverQuest::IsOkToTransact @0x51c970's inputs, in the order it checks them (no messages).
enum TransactWindow : std::uint8_t {
    kTransactSpellbook, kTransactTrain, kTransactBazaar, kTransactBarter, kTransactTrade, kTransactBank,
    kTransactMerchant, kTransactLoot, kTransactBarterMerchant, kTransactGuildBank, kTransactWindows
};
struct TransactState {
    bool vehicle = false;             // the local player's vehicle (PlayerBase +0x150)
    bool vehicle_allows = false;      // the vehicle's virtual +0x5c: the answer when there is a vehicle
    std::uint32_t respawn_timer = 0;  // PlayerBase +0x2bc: nonzero blocks
    std::uint32_t busy_ddf8a0 = 0;    // 0xddf8a0: nonzero blocks
    std::int32_t currently_interacting = 0; // __CurrentlyInteracting: > 0 blocks
    std::int32_t item_pending = 0;    // EverQuestinfo__IsItemPending: > 0 blocks
    bool container_manager = true;    // pinstCContainerMgr
    std::uint32_t container_open = 0; // its +0xa4 WorldContainer (an open world container): nonzero blocks
    // Each window: nullopt when it does not exist, else whether it is active (CXWnd::IsActive).
    std::array<std::optional<bool>, kTransactWindows> windows{};
};
// With a vehicle its virtual +0x5c decides alone; otherwise a respawn timer, 0xddf8a0, interacting, an item
// request pending, an open world container, then any of the ten windows active refuse; true otherwise.
bool is_ok_to_transact(const TransactState& s);

// The [Options] Trade setting (0xde0bb0): 2 everyone (the default), 1 group only, 0 no one. Read from eqclient.ini
// with the default from defaults.ini, else 2; its only reader is Handle_OP_TradeRequest.
inline constexpr std::int32_t kTradeEveryone = 2, kTradeGroupOnly = 1, kTradeNoOne = 0;
// GetPrivateProfileInt over [Options] Trade: eqclient.ini's value, else defaults.ini's, else 2 (atoi of the text).
std::int32_t trade_setting(const std::optional<std::string>& eqclient, const std::optional<std::string>& defaults);
inline constexpr std::uint32_t kStrTradeRefused = 1447; // eqstr 0x5a7, colour 0x111
inline constexpr std::uint32_t kOpTradeRequestAck = 0x5505;

struct TradeRequestInput {
    std::uint32_t requester_id = 0;  // packet +4
    bool requester_spawn = true;     // GetSpawnByID found it
    std::uint32_t my_spawn_id = 0;   // the local player's +0x148
    std::int32_t setting = kTradeEveryone;
    bool pc = true;                  // pinstLocalPC
    bool group = false;              // PcClient +0x31cc
    bool requester_in_group = false; // CGroup_GetMemberByPlayer(requester)
    bool ok_to_transact = true;      // what IsOkToTransact answers (asked twice)
    bool attacking = false;          // __Attack
    bool world_connection = true;    // __gWorld
    bool send_ok = true;             // UdpConnection::Send's result
    bool quiet_debug = false;        // 0xddf654
};
struct TradeRequestCall {
    enum class Fn : std::uint8_t {
        GroupMemberLookup, IsOkToTransact, PendingRequestReport, TradeWindowActivate, Send, CloseWorldConnection,
        DebugLog, FormatMessage, Chat
    } fn{};
    std::int32_t value = 0; // IsOkToTransact / lookup: the answer; report: the code; send: the length;
                            // format: the eqstr; chat: the colour; debug log: the opcode
};
// The refusal OP_TradeRequestAck (0x5505, 12 bytes): requester, own id, reason dword ('b' no one, 'c' group only,
// 1 busy; the original sets only its low byte).
struct TradeRefusal {
    std::uint32_t requester_id = 0, my_spawn_id = 0;
    std::uint8_t reason = 0;
};
struct TradeRequestResult {
    std::vector<TradeRequestCall> calls;
    std::optional<TradeRefusal> refusal; // set: the refusal was built (sent when connected) and 1447 printed
    bool open_trade = false;             // CTradeWnd::Activate(requester, 0)
};
// Handle_OP_TradeRequest @0x4c23c0: setting 0 refuses 'b'; setting 1 refuses 'c' unless the requester is in our
// group; otherwise IsOkToTransact (a failure files pending report 0x40 and it is asked again) and not
// auto-attacking opens the trade with no reply, else the refusal with reason 1.
TradeRequestResult handle_trade_request(const TradeRequestInput& in);

} // namespace hailstone::game
