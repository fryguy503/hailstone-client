// The bank's slots, the coin on the cursor and the trade's state, for UIs that show the bank's bags inside the bank
// window rather than in container windows of their own (ui/gamepad/bank.lua), move coin with the pad and say who
// has accepted a trade (ui/gamepad/trade.lua, ui/modern/trade.lua):
//
//   item = hs.BankItem(slot [, bagSlot [, shared]])   -- the bank slot's item (slot 0-based; shared: the shared
//                                                        bank's), or the item in that bag's bagSlot (0-based); nil
//   ok = hs.BankSlotClick(slot, bagSlot|nil, button [, shared])  -- the slot logic there while the bank is open:
//                                                        "left" picks up, drops, swaps or splits (CInvSlot's
//                                                        HandleLButtonUp: Shift whole stack, Ctrl one), "rightup"
//                                                        uses the item (a bag opens its container), "rightheld"
//                                                        shows its item display
//   button, amount = hs.CursorCoin()                  -- the coin on the cursor: its money button (0 platinum ..
//                                                        3 copper, as the windows' Money0..3) and amount; nil if none
//   t = hs.TradeState()                               -- the open trade or give window (CTradeWnd / CGiveWnd): { npc,
//                                                        partner, requested (our request waits for the answer), open,
//                                                        myReady, theirReady (CTradeWnd +0x2f9 / +0x2f8: the Trade
//                                                        button clicked; any change to the offer clears both) }; nil
//                                                        without one
//
// The clicks are the ones a bank bag's container window (CContainerWnd's slots) and the bank window's slots make:
// GameConnection::click_slot on the bank location, so the server sees the same moves.
#include <optional>

#include <lua.hpp>

#include <imgui.h>

#include "app/lua_ui_windows.h"
#include "net/game_connection.h"
#include "net/item_packets.h"

namespace hailstone::app {

namespace {

UiHost& host(lua_State* L) { return *static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1))); }

// The bank slot from argument `first` on, nullopt when a number is past a slot's range (slot_args).
std::optional<net::ItemSlot> bank_slot(lua_State* L, int first) {
    std::int16_t slot = 0, bag = -1;
    if (!slot_args(L, first, slot, bag)) return std::nullopt;
    bool shared = lua_toboolean(L, first + 3) != 0;
    return net::ItemSlot{shared ? net::kInvSharedBank : net::kInvBank, slot, bag, -1, 0, 0};
}

int BankItem(lua_State* L) {
    auto& h = host(L);
    if (!h.conn || !h.in_game) return push_item(L, nullptr);
    std::int16_t slot = 0, bag = -1;
    if (!slot_args(L, 1, slot, bag)) return push_item(L, nullptr);
    bool shared = lua_toboolean(L, 3) != 0;
    return push_item(L, h.conn->inventory().at(net::ItemSlot{shared ? net::kInvSharedBank : net::kInvBank, slot, bag, -1, 0, 0}));
}

int BankSlotClick(lua_State* L) {
    auto& h = host(L);
    // Only while the bank window is open (pinstActiveBanker), as its slots and its bags' windows are.
    if (!h.conn || !h.in_game || !h.conn->bank()) {
        lua_pushboolean(L, false);
        return 1;
    }
    const std::optional<net::ItemSlot> at = bank_slot(L, 1);
    if (!at) {
        lua_pushboolean(L, false);
        return 1;
    }
    const net::ItemSlot s = *at;
    std::string_view button = lua_isstring(L, 3) ? lua_tostring(L, 3) : "left";
    if (button == "left") {
        const ImGuiIO& io = ImGui::GetIO();
        h.conn->click_slot(s, io.KeyShift, io.KeyCtrl);
    } else if (button == "right" || button == "rightup") {
        item_right_click(h, s);
    } else if (button == "rightheld") {
        const net::Item* it = h.conn->inventory().at(s);
        if (it && it->id != 0 && h.inspect_item) h.inspect_item(*it);
    }
    lua_pushboolean(L, true);
    return 1;
}

int CursorCoin(lua_State* L) {
    auto& h = host(L);
    if (!h.conn || !h.in_game) {
        lua_pushnil(L);
        return 1;
    }
    if (auto held = h.conn->held_coin()) {
        lua_pushinteger(L, 3 - held->first);
        lua_pushinteger(L, held->second);
        return 2;
    }
    lua_pushnil(L);
    return 1;
}

int TradeState(lua_State* L) {
    auto& h = host(L);
    const auto* t = h.conn && h.in_game && h.conn->trade() ? &*h.conn->trade() : nullptr;
    if (!t) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 6);
    auto set_bool = [&](const char* k, bool v) {
        lua_pushboolean(L, v);
        lua_setfield(L, -2, k);
    };
    set_bool("npc", t->with_npc);
    set_bool("requested", t->requested);
    set_bool("open", t->open);
    set_bool("myReady", t->my_ready);
    set_bool("theirReady", t->their_ready);
    lua_pushstring(L, t->partner_name.c_str());
    lua_setfield(L, -2, "partner");
    return 1;
}

} // namespace

void register_exchange_api(lua_State* L, UiHost& h) {
    static const luaL_Reg fns[] = {{"BankItem", BankItem},     {"BankSlotClick", BankSlotClick},
                                   {"CursorCoin", CursorCoin}, {"TradeState", TradeState},
                                   {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h);
    luaL_setfuncs(L, fns, 1);
}

} // namespace hailstone::app
