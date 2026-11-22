#include "ui/commerce_windows.h"

#include "game/quantity_dialog.h"
#include "ui/lua/lua_host.h"

#include <algorithm>
#include <format>

#include <imgui.h>

#include "game/merchant.h"
#include "ui/item_windows.h"

namespace hailstone::ui {

using net::Item;
using net::ItemSlot;

std::string short_price(std::int64_t copper) {
    if (copper <= 0) return "0c";
    std::string s;
    auto add = [&](std::int64_t n, const char* unit) {
        if (n <= 0) return;
        if (!s.empty()) s += ' ';
        s += std::to_string(n) + unit;
    };
    add(copper / 1000, "p");
    add(copper / 100 % 10, "g");
    add(copper / 10 % 10, "s");
    add(copper % 10, "c");
    return s;
}

namespace {

const ImVec4 kDim{0.62f, 0.64f, 0.68f, 1.0f};
const ImVec4 kGold{1.0f, 0.85f, 0.35f, 1.0f};
const ImVec4 kGreen{0.45f, 0.95f, 0.45f, 1.0f};

// One item slot (icon, stack count, tooltip). Returns true on a left click.
bool slot(const char* id, const Item* item, ItemIconCache* icons, float size, bool highlight = false,
          bool* right_clicked = nullptr) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("s", ImVec2(size, size), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (right_clicked) *right_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), IM_COL32(25, 25, 30, 220), 3.0f);
    if (item) {
        if (!icons || !icons->draw_at(dl, item->icon, p.x + 2, p.y + 2, size - 4))
            dl->AddText(ImVec2(p.x + 3, p.y + 3), IM_COL32(230, 230, 230, 255), item->name.substr(0, 5).c_str());
        if (item->stackable() && item->stack_count >= 1) {
            std::string n = std::to_string(item->stack_count);
            ImVec2 ts = ImGui::CalcTextSize(n.c_str());
            dl->AddText(ImVec2(p.x + size - ts.x - 3, p.y + size - ts.y - 1), IM_COL32(255, 255, 255, 255), n.c_str());
        }
        if (hovered) ImGui::SetTooltip("%s", item->name.c_str());
    }
    ImU32 border = highlight ? IM_COL32(120, 230, 120, 255) : hovered ? IM_COL32(220, 200, 120, 255) : IM_COL32(90, 90, 100, 255);
    dl->AddRect(p, ImVec2(p.x + size, p.y + size), border, 3.0f, 0, highlight ? 2.0f : 1.0f);
    ImGui::PopID();
    return clicked;
}

void coins_line(const char* label, const std::array<std::uint32_t, 4>& c) {
    ImGui::Text("%s %upp %ugp %usp %ucp", label, c[3], c[2], c[1], c[0]);
}

} // namespace

CommerceWindowsActions commerce_windows(CommerceWindowsState& st, const CommerceWindowsInput& in, ItemIconCache* icons) {
    CommerceWindowsActions act;
    float em = ImGui::GetFontSize();
    float cell = em * 2.6f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const Item* held = in.inventory ? in.inventory->cursor() : nullptr;

    // ---- merchant (CMerchantWnd)
    if (const net::MerchantSession* m = in.merchant; m && m->open && !lua::replaces("MerchantWnd")) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + em * 2.0f, vp->WorkPos.y + em * 6.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(em * 26.0f, em * 34.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.9f);
        bool open = true;
        if (ImGui::Begin(std::format("{}###hailstone_merchant", m->name).c_str(), &open, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextColored(kDim, "Merchant items (%zu)", m->items.size());
            ImGui::BeginChild("stock", ImVec2(0, em * 16.0f), ImGuiChildFlags_Borders);
            for (const auto& [mslot, item] : m->items) {
                ImGui::PushID(static_cast<int>(mslot));
                bool selected = st.selected_buy && *st.selected_buy == mslot;
                ImVec2 row = ImGui::GetCursorPos();
                if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, em * 1.7f))) {
                    st.selected_buy = mslot;
                    // CMerchantWnd::HandleBuy: max the merchant's count capped at the stack, start 1 for a
                    // scroll with a spell (type 20), else the max (game/quantity_dialog.h).
                    st.buy_quantity = item.stackable() ? game::merchant_buy_limits(
                        static_cast<std::int32_t>(item.merchant_slot), static_cast<std::int32_t>(item.max_stack),
                        item.item_type == 20 && item.effects[net::kEffectScroll].effect > 0).start : 1;
                }
                ImGui::SetCursorPos(row);
                if (icons) icons->draw(item.icon, em * 1.6f, item.name.c_str());
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(item.name.c_str());
                ImGui::SameLine(em * 17.0f);
                ImGui::TextColored(kGold, "%s", short_price(item.price).c_str());
                if (item.merchant_slot != 1 && static_cast<std::int32_t>(item.merchant_slot) > 0) {
                    ImGui::SameLine();
                    ImGui::TextColored(kDim, "(%u)", item.merchant_slot);
                }
                ImGui::PopID();
            }
            ImGui::EndChild();

            // Buying (CMerchantWnd::DisplayBuyOrSellPrice: ItemBase_GetPrice × quantity).
            const Item* buy = nullptr;
            if (st.selected_buy) {
                auto it = m->items.find(*st.selected_buy);
                if (it != m->items.end()) buy = &it->second;
            }
            if (buy) {
                if (buy->stackable()) {
                    ImGui::SetNextItemWidth(em * 6.0f);
                    ImGui::InputInt("Quantity##buy", &st.buy_quantity);
                    st.buy_quantity = std::clamp(st.buy_quantity, 1, game::merchant_buy_limits(
                        static_cast<std::int32_t>(buy->merchant_slot), static_cast<std::int32_t>(buy->max_stack), false).max);
                } else {
                    st.buy_quantity = 1;
                }
                std::int64_t cost = static_cast<std::int64_t>(buy->price) * st.buy_quantity;
                ImGui::Text("Buy %s for", buy->name.c_str());
                ImGui::SameLine();
                ImGui::TextColored(kGold, "%s", short_price(cost).c_str());
                bool afford = game::total_copper(in.coins) >= cost;
                ImGui::BeginDisabled(!afford || m->pending > 0);
                if (ImGui::Button("Buy")) act.buy = {*st.selected_buy, static_cast<std::uint32_t>(st.buy_quantity)};
                ImGui::EndDisabled();
            } else {
                ImGui::TextColored(kDim, "Click an item above to buy it.");
            }
            ImGui::Separator();

            // Selling (ItemBase::ValueSellMerchant at the merchant's rate).
            const Item* sell = m->selected && in.inventory ? in.inventory->at(*m->selected) : nullptr;
            if (sell && (!st.sell_for || !st.sell_for->same(*m->selected))) {
                st.sell_for = *m->selected;
                st.sell_quantity = static_cast<int>(game::merchant_sell_limits(static_cast<std::int32_t>(sell->stack_count)).start);
            }
            if (sell) {
                if (sell->stackable() && sell->stack_count > 1) {
                    ImGui::SetNextItemWidth(em * 6.0f);
                    ImGui::InputInt("Quantity##sell", &st.sell_quantity);
                    st.sell_quantity = std::clamp(st.sell_quantity, 1, static_cast<int>(sell->stack_count));
                } else {
                    st.sell_quantity = 1;
                }
                std::int32_t value = game::sell_value(*sell, m->rate, st.sell_quantity);
                ImGui::Text("Sell %s for", sell->name.c_str());
                ImGui::SameLine();
                if (value > 0) ImGui::TextColored(kGreen, "%s", short_price(value).c_str());
                else ImGui::TextColored(kDim, "no value");
                ImGui::BeginDisabled(m->pending > 0);
                if (ImGui::Button("Sell")) act.sell = {*m->selected, static_cast<std::uint32_t>(st.sell_quantity)};
                ImGui::EndDisabled();
            } else {
                ImGui::TextColored(kDim, "Click an item in your inventory to sell it.");
            }
            ImGui::Separator();
            ImGui::Text("Your money: %upp %ugp %usp %ucp", in.coins.platinum, in.coins.gold, in.coins.silver, in.coins.copper);
            if (ImGui::Button("Done")) act.close_merchant = true;
        }
        ImGui::End();
        if (!open) act.close_merchant = true;
    } else {
        st.selected_buy.reset();
    }

    // ---- trade (CTradeWnd) / give (CGiveWnd)
    if (const net::TradeSession* t = in.trade; t && t->open && !lua::replaces(t->with_npc ? "GiveWnd" : "TradeWnd")) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f - em * 12.0f, vp->WorkPos.y + em * 7.0f),
                                ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.9f);
        bool open = true;
        std::string title = t->with_npc ? std::format("Give: {}###hailstone_trade", t->partner_name)
                                        : std::format("Trade: {}###hailstone_trade", t->partner_name);
        if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            if (!t->with_npc) {
                ImGui::TextColored(t->their_ready ? kGreen : kDim, "%s%s", t->partner_name.c_str(),
                                   t->their_ready ? " (ready to trade)" : "");
                for (std::int16_t n = 0; n < 8; ++n) {
                    if (n % 4) ImGui::SameLine();
                    auto it = t->theirs.find(n);
                    slot(std::format("h{}", n).c_str(), it == t->theirs.end() ? nullptr : &it->second, icons, cell);
                }
                coins_line("Their coin:", t->their_coins);
                ImGui::Separator();
            }
            ImGui::TextColored(t->my_ready ? kGreen : kDim, "You%s", t->my_ready ? " (ready)" : "");
            for (std::int16_t n = 0; n < static_cast<std::int16_t>(t->slots()); ++n) {
                if (n % 4) ImGui::SameLine();
                const Item* mine = in.inventory ? in.inventory->at({net::kInvTrade, n, -1, -1, 0, 0}) : nullptr;
                if (slot(std::format("m{}", n).c_str(), mine, icons, cell) && held) act.trade_drop = true;
            }
            coins_line("Your coin:", t->my_coins);
            if (in.holding_coin && ImGui::Button("Offer held coin")) act.trade_drop_coin = true;
            static constexpr const char* kCoins[] = {"copper", "silver", "gold", "platinum"};
            ImGui::SetNextItemWidth(em * 6.0f);
            ImGui::InputInt("##coin", &st.coin_amount);
            st.coin_amount = std::max(st.coin_amount, 0);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(em * 6.0f);
            ImGui::Combo("##cointype", &st.coin_type, kCoins, 4);
            ImGui::SameLine();
            if (ImGui::Button("Add coin") && st.coin_amount > 0) {
                act.trade_coin = {st.coin_type, static_cast<std::uint32_t>(st.coin_amount)};
                st.coin_amount = 0;
            }
            if (held) ImGui::TextColored(kGold, "Click one of your slots to offer %s.", held->name.c_str());
            if (ImGui::Button(t->with_npc ? "Give" : "Trade")) act.trade_accept = true;
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) act.trade_cancel = true;
        }
        ImGui::End();
        if (!open) act.trade_cancel = true;
    } else if ((held || in.holding_coin) && in.target_id && !in.trade && !(in.merchant && in.merchant->open)) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - em * 9.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.8f);
        if (ImGui::Begin("##hailstone_give", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing)) {
            if (ImGui::Button(std::format("Give {} to {}", held ? held->name : "coin", in.target_name).c_str()))
                act.give_to = in.target_id;
        }
        ImGui::End();
    }
    // ---- bank (CBankWnd, the BigBankWnd layout: 24 slots, two shared slots)
    if (const net::BankSession* b = in.bank; b && !lua::replaces(b->slots == 24 ? "BigBankWnd" : "BankWnd")) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + em * 2.0f, vp->WorkPos.y + em * 6.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.9f);
        bool open = true;
        if (ImGui::Begin(std::format("Bank: {}###hailstone_bank", b->name).c_str(), &open,
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextColored(kDim, "Bank (%d slots; right click a bag to show it)", b->slots);
            for (std::int16_t n = 0; n < b->slots; ++n) {
                if (n % 8) ImGui::SameLine();
                ItemSlot is{net::kInvBank, n, -1, -1, 0, 0};
                const Item* item = in.inventory ? in.inventory->at(is) : nullptr;
                bool right = false;
                if (slot(std::format("bk{}", n).c_str(), item, icons, cell, false, &right)) act.bank_click = is;
                if (right && item && item->is_bag() && !st.open_bank_bags.erase(n)) st.open_bank_bags.insert(n);
            }
            for (auto it = st.open_bank_bags.begin(); it != st.open_bank_bags.end();) {
                const Item* bag = in.inventory ? in.inventory->at({net::kInvBank, *it, -1, -1, 0, 0}) : nullptr;
                if (!bag || !bag->is_bag()) {
                    it = st.open_bank_bags.erase(it);
                    continue;
                }
                ImGui::TextColored(kDim, "%s (bank slot %d)", bag->name.c_str(), *it + 1);
                for (std::int16_t i = 0; i < bag->bag_slots; ++i) {
                    if (i % 8) ImGui::SameLine();
                    ItemSlot is{net::kInvBank, *it, i, -1, 0, 0};
                    if (slot(std::format("bb{}_{}", *it, i).c_str(), in.inventory->at(is), icons, cell)) act.bank_click = is;
                }
                ++it;
            }
            ImGui::TextColored(kDim, "Shared bank");
            for (std::int16_t n = 0; n < game::kSharedBankSlots; ++n) {
                if (n) ImGui::SameLine();
                ItemSlot is{net::kInvSharedBank, n, -1, -1, 0, 0};
                const Item* item = in.inventory ? in.inventory->at(is) : nullptr;
                if (slot(std::format("sb{}", n).c_str(), item, icons, cell)) act.bank_click = is;
            }
            const game::BankCoins& c = in.bank_coins;
            ImGui::Text("Bank coin: %upp %ugp %usp %ucp   shared: %upp", c.platinum, c.gold, c.silver, c.copper,
                        c.shared_platinum);
            ImGui::Text("Your coin: %upp %ugp %usp %ucp", in.coins.platinum, in.coins.gold, in.coins.silver, in.coins.copper);
            static constexpr const char* kCoins[] = {"copper", "silver", "gold", "platinum"};
            ImGui::SetNextItemWidth(em * 6.0f);
            ImGui::InputInt("##bankcoin", &st.bank_coin_amount);
            st.bank_coin_amount = std::max(st.bank_coin_amount, 0);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(em * 6.0f);
            ImGui::Combo("##bankcointype", &st.bank_coin_type, kCoins, 4);
            auto amount = static_cast<std::uint32_t>(st.bank_coin_amount);
            ImGui::BeginDisabled(amount == 0);
            if (ImGui::Button("Deposit")) act.deposit = {st.bank_coin_type, amount};
            ImGui::SameLine();
            if (ImGui::Button("Withdraw")) act.withdraw = {st.bank_coin_type, amount};
            ImGui::BeginDisabled(st.bank_coin_type != 3);
            ImGui::SameLine();
            if (ImGui::Button("Deposit shared")) {
                act.deposit = {3, amount};
                act.shared_coin = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Withdraw shared")) {
                act.withdraw = {3, amount};
                act.shared_coin = true;
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            if (held) ImGui::TextColored(kGold, "Holding %s: click a slot, or Auto.", held->name.c_str());
            ImGui::BeginDisabled(!held);
            if (ImGui::Button("Auto")) act.auto_bank = true;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Done")) act.close_bank = true;
        }
        ImGui::End();
        if (!open) act.close_bank = true;
    } else {
        st.open_bank_bags.clear();
    }

    // ---- quantity dialog (CQuantityWnd: a slider 0..max starting at max, Accept / Cancel)
    if (const net::QuantityRequest* q = in.quantity; q && !lua::replaces("QuantityWnd")) {
        if (!st.quantity_for || !st.quantity_for->same(q->slot)) {
            st.quantity_for = q->slot;
            st.quantity = static_cast<int>(q->start); // CQuantityWnd::Open: the slider starts at `start`
        }
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.4f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("Quantity###hailstone_quantity", nullptr,
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
            ImGui::SetNextItemWidth(em * 12.0f);
            ImGui::SliderInt("##qty", &st.quantity, 0, static_cast<int>(q->max));
            ImGui::SetNextItemWidth(em * 12.0f);
            ImGui::InputInt("##qtyn", &st.quantity);
            st.quantity = std::clamp(st.quantity, 0, static_cast<int>(q->max));
            if (ImGui::Button("Accept") || ImGui::IsKeyPressed(ImGuiKey_Enter))
                act.quantity_accept = static_cast<std::uint32_t>(st.quantity);
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) act.quantity_cancel = true;
        }
        ImGui::End();
    } else {
        st.quantity_for.reset();
    }
    return act;
}

} // namespace hailstone::ui
