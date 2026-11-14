// The item windows: inventory, bags, loot, merchant, bank, trade and give. Each follows its original class
// (CInventoryWnd, CContainerMgr / CContainerWnd, CLootWnd, CMerchantWnd, CBankWnd, CTradeWnd, CGiveWnd); item
// clicks go through GameConnection::click_slot, the client's CInvSlot::HandleLButtonUp (pick up, drop, swap,
// split, sell-select while a merchant is open, drop into a trade, bank moves, loot).

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>

#include <imgui.h>

#include "app/lua_eq.h"
#include "app/lua_eq_windows.h"
#include "game/bank.h"
#include "game/hot_button_menu.h"
#include "game/quantity_dialog.h"
#include "game/inventory.h"
#include "game/item_links.h"
#include "game/keymap.h"
#include "game/merchant.h"
#include "game/server_ext.h"
#include "game/tradeskill.h"
#include "game/world_state.h"
#include "net/game_connection.h"
#include "net/trade_packets.h"

namespace hailstone::app {

namespace {

int index_after(std::string_view id, std::string_view prefix) {
    if (id.size() <= prefix.size() || id.substr(0, prefix.size()) != prefix) return -1;
    int v = -1;
    auto rest = id.substr(prefix.size());
    auto [p, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), v);
    return ec == std::errc() && p == rest.data() + rest.size() ? v : -1;
}

bool shift() { return ImGui::GetIO().KeyShift; }
bool ctrl() { return ImGui::GetIO().KeyCtrl; }

// The money buttons count platinum first: Money0 platinum .. Money3 copper; wire coin types count copper first.
int coin_type(int button) { return 3 - button; }

std::uint32_t coin_of(const net::Coins& c, int type) {
    switch (type) {
    case 0: return c.copper;
    case 1: return c.silver;
    case 2: return c.gold;
    default: return c.platinum;
    }
}

// The coin held on the cursor, if any: its type and amount.
std::optional<std::pair<int, std::uint32_t>> cursor_coin(const net::GameConnection& c) {
    const auto& cc = c.cursor_coins();
    for (int t = 3; t >= 0; --t)
        if (cc[static_cast<std::size_t>(t)]) return std::pair{t, cc[static_cast<std::size_t>(t)]};
    return std::nullopt;
}

// A money button where coin lives at `location` (net::kCoin*): with coin on the cursor it drops there, else that
// pile's coin of the button's type comes to the cursor (the original asks how many in its quantity window; here
// the whole pile comes, Ctrl takes one).
void money_click(net::GameConnection& c, int location, int type, std::uint32_t pile, bool shared = false) {
    if (auto held = cursor_coin(c)) {
        c.move_coin(net::kCoinCursor, location, held->first, shared ? 3 : held->first, static_cast<std::uint32_t>(held->second));
        return;
    }
    if (pile == 0) return;
    c.move_coin(location, net::kCoinCursor, type, type, ctrl() ? 1u : pile);
}

// The merchant window as the inventory's coin buttons see it (0x698290 asks for its mail page).
class ParcelPage {
public:
    virtual ~ParcelPage() = default;
    // The merchant window shows, on its mail (Parcels) page (CMerchantWnd_GetCurrentPageHandler == 2).
    virtual bool parcels_shown(EqHost& host) = 0;
    // 0x6f2120: the merchant window's own selected item and its list's selection are cleared.
    virtual void drop_selection() = 0;
};

std::function<bool()> merchant_quantity_owner(EqHost& host) {
    const auto* conn = host.conn;
    const auto serial = conn && conn->merchant() ? conn->merchant()->serial : 0;
    return [&host, conn, serial] {
        return host.conn == conn && conn && conn->merchant() && conn->merchant()->serial == serial;
    };
}

// CInventoryWnd::ClickedMoneyButton (GameConnection::inventory_money_button): coin on the
// cursor goes into the inventory as this button's coin; otherwise the quantity dialog opens at the button's centre
// with max = start = the inventory's coins of that type (none: nothing; one, Shift: all, Ctrl: 1 at once) and its
// answer comes onto the cursor. Before the dialog, on the merchant's Parcels page, 0x698290
// (GameConnection::parcel_coin_button): the bag item picked is let go and, with coins of that type, the button is
// outlined and the merchant window's selection cleared. Hailstone: there the answer is the money picked for sending
// (GameConnection::set_parcel_money; nothing moves until the page sends it), as there is no SendMoneyWnd to drop
// the cursor's coin on. Its question closes with that merchant session.
void inventory_money_click(EqHost& host, int coin, ParcelPage* page) {
    const auto& r = host.click_rect;
    std::array<std::int32_t, 4> rect{static_cast<std::int32_t>(r[0]), static_cast<std::int32_t>(r[1]),
                                     static_cast<std::int32_t>(r[2]), static_cast<std::int32_t>(r[3])};
    game::MoneyButtonOut out = host.conn->inventory_money_button(coin, -1, r[0] >= 0 ? &rect : nullptr);
    if (out.kind != game::MoneyButtonOut::Kind::AskQuantity) return;
    game::QuantityOpen q;
    q.max = q.start = host.conn->inventory_coin(coin);
    q.x = out.x;
    q.y = out.y;
    const bool parcels = page && page->parcels_shown(host);
    if (parcels) {
        host.conn->parcel_coin_button(coin, q.max);
        if (q.max > 0) page->drop_selection();
    }
    std::function<bool()> owner = parcels ? merchant_quantity_owner(host) : std::function<bool()>{};
    open_quantity(host, q, [&host, coin, parcels](std::int32_t n) {
        if (!host.conn) return;
        if (parcels && host.conn->merchant() && host.conn->merchant()->open) {
            if (n > 0) host.conn->set_parcel_money(coin, static_cast<std::uint32_t>(n));
            return;
        }
        host.conn->inventory_money_button(coin, n, nullptr);
    }, std::move(owner));
}

const ChildArt kNone{ChildArt::None, {}, {}};

// Hailstone: the inventory and bank windows' Auto-Inventory button (not the original's): the cursor's item into the
// inventory as /autoinventory puts it (cmd_autoinventory, which says so when there is no room). Enabled while the
// cursor holds an item.
constexpr std::string_view kAutoInventoryChild = "Hailstone_AutoInventory";

// ---------------------------------------------------------------- InventoryWindow (CInventoryWnd)

class InventoryWindow : public StockWindow {
public:
    explicit InventoryWindow(ParcelPage* merchant) : StockWindow("InventoryWindow", false), merchant_(merchant) {}

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& c = *host.conn;
        // OnProcessFrame: the AA experience label and gauges from level 51 (GetLevel >= 0x33).
        if (id == "AltAdvLabel" || id == "AltAdvGauge" || id == "AAVitalityGauge") {
            out.shown = c.player_status().level >= 51;
        } else if (id == "IW_ConsumeKrono" || id == "InvSlotKrono" || id == "IW_SubscriptionTime") {
            out.shown = false; // no Krono and no subscription on this server (KronoManager status never comes)
        } else if (int m = index_after(id, "IW_Money"); m >= 0 && m < 4) {
            out.text = std::to_string(coin_of(c.coins(), coin_type(m)));
            // the outline 0x698290 puts on the button picked for the Parcels page (CButtonWnd +0x259)
            if (const auto& ms = c.merchant(); ms && ms->open) out.checked = ms->parcel_coin == m;
        } else if (id == "IW_GoodPoints" || id == "IW_EvilPoints") {
            out.text = "0"; // CInventoryWnd_UpdateCrystalCounts: radiant / ebon crystals (not kept by this client)
        } else if (id == "AltCurr_MaxStackLabel") {
            const auto* a = selected_currency(c);
            out.text = a ? std::format("Max stack: {}", a->entry.stack_size) : std::string();
        } else if (id == "AltCurr_CreateItemButton") {
            const auto* a = selected_currency(c);
            out.enabled = a && a->amount > 0 && !c.inventory().cursor();
        } else if (id == "AltCurr_ReclaimButton") {
            out.enabled = selected_currency(c) && c.inventory().cursor();
        } else if (id == "AltCurr_DisplayMissingButton") {
            out.checked = show_missing_;
        } else if (id == kAutoInventoryChild) {
            out.enabled = c.inventory().cursor() != nullptr;
        }
    }

    // The Alt. Currency page (not decoded; EQEmu's OP_AltCurrency): icon, name and amount per currency the server
    // listed as shown (Display Missing adds the ones at 0). Create Item takes some out onto the cursor (the quantity
    // dialog, at most a stack), Reclaim puts the cursor's currency item back (OP_AltCurrencyReclaim). The names'
    // dbstr type is not known, so a currency shows as "Currency <n>".
    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn || id != "AltCurr_PointList") return nullptr;
        currencies_ = {};
        currencies_.columns = 3;
        currencies_.labels = {"", "Currency", "Amount"};
        currencies_.widths = {24, 200, 60};
        for (std::uint32_t cid : listed(*host.conn)) {
            const auto& a = host.conn->alt_currencies().at(cid);
            ListRow r;
            r.texts = {"", std::format("Currency {}", cid), std::to_string(a.amount)};
            if (host.textures) r.icon = host.textures->item_icon(a.entry.icon);
            if (cid == selected_) currencies_.selected = static_cast<int>(currencies_.rows.size());
            currencies_.rows.push_back(std::move(r));
        }
        return &currencies_;
    }
    // The rows are worked out again here: a UI pack that draws the page itself never asks for list().
    bool list_click(EqHost& host, std::string_view id, int row, bool, int) override {
        if (id != "AltCurr_PointList") return false;
        if (!host.conn) return true;
        auto ids = listed(*host.conn);
        if (row >= 0 && row < static_cast<int>(ids.size())) selected_ = ids[static_cast<std::size_t>(row)];
        return true;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        if (id == "IW_Destroy") c.destroy_held(); // CInventoryWnd::DestroyHeld (Fast Item Destroy)
        else if (id == "IW_Tinting") host.body_tint_open = !host.body_tint_open;
        else if (id == "IW_FacePick") { // CInventoryWnd::WndNotification: CPlayerCustomizationWnd's Activate
            if (host.show_window) host.show_window("PlayerCustomizationWnd");
        }
        else if (id == "IW_AltAdvBtn") { // the Alt. Adv. button: the AA window, as its key (TOGGLE_ALTADVWIN) opens it
            if (host.run_command) host.run_command(static_cast<int>(game::KeyCommand::TOGGLE_ALTADVWIN));
        }
        else if (id == "IW_Skills") { // CInventoryWnd::WndNotification @0x693490: toggles CSkillsWnd
            if (host.run_command) host.run_command(static_cast<int>(game::KeyCommand::TOGGLE_SKILLS_WND));
        }
        else if (id == "DoneButton" || id == "IW_DoneButton") open_ = false;
        else if (id == "AltCurr_DisplayMissingButton") show_missing_ = !show_missing_;
        else if (id == "AltCurr_ReclaimButton") {
            if (const auto* a = selected_currency(c)) c.alt_currency_reclaim(a->entry.id, 0, true);
        } else if (id == "AltCurr_CreateItemButton") {
            const auto* a = selected_currency(c);
            if (!a || a->amount == 0) return true;
            std::uint32_t cid = a->entry.id;
            std::uint32_t max = std::min(a->amount, std::max<std::uint32_t>(1, a->entry.stack_size));
            if (max == 1) {
                c.alt_currency_reclaim(cid, 1, false);
                return true;
            }
            game::QuantityOpen q;
            q.max = q.start = static_cast<std::int32_t>(max);
            open_quantity(host, q, [&host, cid](std::int32_t v) {
                if (host.conn && v > 0) host.conn->alt_currency_reclaim(cid, static_cast<std::uint32_t>(v), false);
            });
        }
        else if (int m = index_after(id, "IW_Money"); m >= 0 && m < 4)
            inventory_money_click(host, m, merchant_);
        else if (id == "IW_CharacterView") c.drop_on_doll(); // CInventoryWnd::HandleLButtonUp
        else if (id == kAutoInventoryChild) c.say("/autoinventory");
        else return false;
        return true;
    }

    std::optional<int> tab_page(EqHost&, std::string_view id) override {
        if (id == "IW_Subwindows") return page_;
        return std::nullopt;
    }
    void set_tab_page(EqHost&, std::string_view id, int page) override {
        if (id == "IW_Subwindows") page_ = page;
    }

private:
    // The currencies the list shows, in its order (eq.AltCurrencies lists them alike).
    std::vector<std::uint32_t> listed(const net::GameConnection& c) const {
        std::vector<std::uint32_t> v;
        for (const auto& [cid, a] : c.alt_currencies())
            if (show_missing_ || (a.amount > 0 && a.entry.shown)) v.push_back(cid);
        return v;
    }
    const net::GameConnection::AltCurrency* selected_currency(const net::GameConnection& c) const {
        auto it = c.alt_currencies().find(selected_);
        return it == c.alt_currencies().end() ? nullptr : &it->second;
    }

    ParcelPage* merchant_;
    int page_ = 0;
    bool show_missing_ = false;
    std::uint32_t selected_ = 0;
    ListData currencies_;
};

// ---------------------------------------------------------------- bag windows (CContainerMgr / CContainerWnd)

// The open containers, in the order they opened ("Bag1" is the first). A container closes when its bag leaves
// its slot.
struct Containers {
    std::vector<net::ItemSlot> open;
    // CInvSlotMgr +0x2010: set by CloseAllBags, cleared by OpenAllBags; TOGGLE_INV_BAGS (0x6989e0) opens while
    // it is set, else closes. The constructor (@0x697d00) sets it, so the first press opens.
    bool all_closed = true;
    // +0x2011, the same for the bank's bags (0x698d60, CMD_TOGGLE_BANK_BAGS), also set by the constructor.
    bool bank_all_closed = true;
    std::uint64_t bank_serial = 0;
    void toggle(const net::ItemSlot& s) {
        for (auto it = open.begin(); it != open.end(); ++it)
            if (it->same(s)) {
                open.erase(it);
                return;
            }
        open.push_back(s);
    }
    void prune(const net::GameConnection& c) {
        const auto serial = c.bank() ? c.bank()->serial : 0;
        const bool bank_changed = serial != bank_serial;
        bank_serial = serial;
        if (!c.bank() || bank_changed) bank_all_closed = true;
        std::erase_if(open, [&](const net::ItemSlot& s) {
            if ((s.type == net::kInvBank || s.type == net::kInvSharedBank) && (!c.bank() || bank_changed)) return true;
            const net::Item* bag = c.inventory().at(s);
            return !bag || !bag->is_bag();
        });
    }
};

Containers& containers() {
    static Containers c;
    return c;
}

// Opens or closes a bag's window (CInvSlot::HandleRButtonUp: an open container closes, else
// CContainerMgr::OpenContainer @0x65cae0). A tradeskill bag opens in the tradeskill window instead of its own when
// GameConnection::container_opens_tradeskill says so (the TradeskillCtrlClick option, CtrlBypass: by default a right
// click, ctrl+right click for the bag's own window; Experiment goes to the bag's own window). Closing the bag
// closes a tradeskill window showing it too.
void toggle_container(EqHost& host, const net::ItemSlot& s) {
    auto& cs = containers();
    if (host.conn) cs.prune(*host.conn);
    bool open = std::any_of(cs.open.begin(), cs.open.end(), [&](const net::ItemSlot& o) { return o.same(s); });
    if (!open && host.conn && host.conn->container_opens_tradeskill(s, ctrl())) {
        host.conn->open_tradeskill_bag(s);
        return;
    }
    cs.toggle(s);
    if (!host.conn || !open) return;
    const auto& t = host.conn->tradeskill();
    if (t.open && !t.world && t.container_slot.same(s)) host.conn->close_tradeskill();
}

bool container_open(const net::ItemSlot& s) {
    const auto& cs = containers();
    return std::any_of(cs.open.begin(), cs.open.end(), [&](const net::ItemSlot& o) { return o.same(s); });
}

} // namespace

// CInvSlotMgr_OpenAllBags @0x6987b0: every bag in general slots 23..32 that is not placeable and not open goes
// through CContainerMgr::OpenContainer, straight to its own window (no tradeskill window, unlike a right click).
void open_all_bags(EqHost& host) {
    auto& cs = containers();
    if (host.conn) {
        for (std::int16_t g = net::kSlotGeneral1; g <= net::kSlotGeneral10; ++g) {
            net::ItemSlot s = net::ItemSlot::possessions(g);
            const net::Item* bag = host.conn->inventory().at(s);
            if (bag && bag->is_bag() && bag->placeable_flags() == 0 && !container_open(s)) cs.open.push_back(s);
        }
    }
    cs.all_closed = false;
}

// CInvSlotMgr_CloseAllBags @0x698900: every open bag in general slots 23..32 closes (CContainerMgr::CloseContainer).
void close_all_bags(EqHost& host) {
    for (std::int16_t g = net::kSlotGeneral1; g <= net::kSlotGeneral10; ++g) {
        net::ItemSlot s = net::ItemSlot::possessions(g);
        if (container_open(s)) toggle_container(host, s);
    }
    containers().all_closed = true;
}

void toggle_all_bags(EqHost& host) {
    if (containers().all_closed) open_all_bags(host);
    else close_all_bags(host);
}

// CMD_TOGGLE_BANK_BAGS (0x698d60): by +0x2011, CInvSlotMgr_OpenAllBankBags @0x698a00 or its close (@0x698bf0).
// Open, only while the bank window shows: every bag in the bank's slots (CBankWnd::GetNumBankSlots) and the shared
// bank's two that is not placeable and not open, then the flag clears. Close: every open bag in those slots closes
// (CContainerMgr::CloseContainer), then the flag is set.
void toggle_all_bank_bags(EqHost& host) {
    auto& cs = containers();
    if (host.conn) cs.prune(*host.conn);
    if (cs.bank_all_closed) {
        if (!host.conn || !host.conn->bank()) return;
        auto open = [&](std::int16_t type, int slots) {
            for (std::int16_t i = 0; i < slots; ++i) {
                net::ItemSlot s{type, i, -1, -1, 0, 0};
                const net::Item* bag = host.conn->inventory().at(s);
                if (bag && bag->is_bag() && bag->placeable_flags() == 0 && !container_open(s)) cs.open.push_back(s);
            }
        };
        open(net::kInvBank, host.conn->bank()->slots);
        open(net::kInvSharedBank, game::kSharedBankSlots);
        cs.bank_all_closed = false;
        return;
    }
    std::vector<net::ItemSlot> bank;
    for (const net::ItemSlot& s : cs.open)
        if ((s.type == net::kInvBank || s.type == net::kInvSharedBank) && s.top_level()) bank.push_back(s);
    for (const net::ItemSlot& s : bank) toggle_container(host, s);
    cs.bank_all_closed = true;
}

namespace {

class BagWindow : public StockWindow {
public:
    explicit BagWindow(int n) : StockWindow("Bag" + std::to_string(n), false), n_(n) {}

    const net::ItemSlot* slot(EqHost& host) {
        if (!host.conn) return nullptr;
        auto& cs = containers();
        cs.prune(*host.conn);
        return n_ - 1 < static_cast<int>(cs.open.size()) ? &cs.open[static_cast<std::size_t>(n_ - 1)] : nullptr;
    }
    const net::ItemSlot* bag_slot(EqHost& host) override { return slot(host); }
    const net::Item* bag(EqHost& host) {
        const net::ItemSlot* s = slot(host);
        return s ? host.conn->inventory().at(*s) : nullptr;
    }

    bool shown(EqHost& host) override { return bag(host) != nullptr; }
    void set_shown(EqHost& host, bool show) override {
        if (!show)
            if (const net::ItemSlot* s = slot(host)) toggle_container(host, *s);
    }
    std::string title(EqHost& host) override {
        const net::Item* b = bag(host);
        return b ? b->name : std::string();
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if ((id == "StandardDoneButton" || id == "CombineDoneButton") && button == "left") {
            set_shown(host, false);
            return true;
        }
        if (id == "Container_Combine" && button == "left") { // CContainerWnd::HandleCombine
            const net::ItemSlot* s = slot(host);
            if (s && s->type == net::kInvPossessions) host.conn->combine_container(s->slot);
            return true;
        }
        return false;
    }
    std::optional<std::pair<std::string, int>> bag_info(EqHost& host) override {
        const net::Item* b = bag(host);
        if (!b) return std::nullopt;
        return std::pair{b->name, static_cast<int>(b->bag_slots)};
    }
    const net::Item* bag_item(EqHost& host, int index) override {
        if (index < 0) return bag(host); // the container itself
        const net::ItemSlot* s = slot(host);
        if (!s) return nullptr;
        net::ItemSlot in = *s;
        in.sub = static_cast<std::int16_t>(index);
        return host.conn->inventory().at(in);
    }
    bool bag_click(EqHost& host, int index, std::string_view button, bool outside) override {
        const net::ItemSlot* s = slot(host);
        if (!s) return false;
        net::ItemSlot in = *s;
        in.sub = static_cast<std::int16_t>(index);
        if (button == "leftheld") return item_left_held(host, in);
        if (button == "left") host.conn->click_slot(in, shift(), ctrl());
        else if ((button == "right" || button == "rightup") && !outside) item_right_click(host, in);
        return true;
    }

private:
    int n_;
};

// The open world container (a tradeskill object in the zone, OP_ClickObjectAction) in its own window, when it has one
// (game::WorldContainer::window): ten slots, Combine and Done. Its slots are the world inventory's (kInvWorld).
class WorldContainerWindow : public StockWindow {
public:
    static constexpr int kSlots = 10;
    WorldContainerWindow() : StockWindow("WorldContainer", false) {}

    const game::WorldContainer* container(EqHost& host) {
        if (!host.conn) return nullptr;
        const auto& wc = host.conn->world_container();
        return wc && wc->window ? &*wc : nullptr;
    }
    const net::ItemSlot* bag_slot(EqHost& host) override { return container(host) ? &slot_ : nullptr; }
    bool shown(EqHost& host) override { return container(host) != nullptr; }
    void set_shown(EqHost& host, bool show) override {
        if (!show && container(host)) host.conn->close_world_container();
    }
    std::string title(EqHost& host) override {
        const game::WorldContainer* wc = container(host);
        return wc ? wc->name : std::string();
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!container(host) || button != "left") return false;
        if (id == "StandardDoneButton" || id == "CombineDoneButton") {
            host.conn->close_world_container();
            return true;
        }
        if (id == "Container_Combine") {
            host.conn->combine_world_container();
            return true;
        }
        return false;
    }
    std::optional<std::pair<std::string, int>> bag_info(EqHost& host) override {
        const game::WorldContainer* wc = container(host);
        if (!wc) return std::nullopt;
        return std::pair{wc->name.empty() ? std::string("Container") : wc->name, kSlots};
    }
    const net::Item* bag_item(EqHost& host, int index) override {
        if (index < 0 || index >= kSlots || !container(host)) return nullptr;
        return host.conn->inventory().at(net::ItemSlot{net::kInvWorld, static_cast<std::int16_t>(index), -1, -1, 0, 0});
    }
    bool bag_click(EqHost& host, int index, std::string_view button, bool outside) override {
        if (index < 0 || index >= kSlots || !container(host)) return false;
        net::ItemSlot in{net::kInvWorld, static_cast<std::int16_t>(index), -1, -1, 0, 0};
        if (button == "left") host.conn->click_slot(in, shift(), ctrl());
        else if ((button == "right" || button == "rightup") && !outside) item_right_click(host, in);
        return true;
    }

private:
    net::ItemSlot slot_{net::kInvWorld, -1, -1, -1, 0, 0};
};

// ---------------------------------------------------------------- LootWnd (CLootWnd)

class LootWindow : public StockWindow {
public:
    LootWindow() : StockWindow("LootWnd", false) {}
    bool shown(EqHost& host) override {
        return host.conn && host.conn->loot_session() && host.conn->loot_session()->open;
    }
    void set_shown(EqHost& host, bool show) override {
        if (!show && host.conn) host.conn->end_loot();
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn || !host.conn->loot_session()) return;
        if (id == "LW_CorpseName") out.text = game::display_name(host.conn->loot_session()->corpse_name);
    }
    const net::Item* slot_item(EqHost& host, std::string_view id) override {
        int n = index_after(id, "LW_LootSlot");
        if (n < 0 || !host.conn || !host.conn->loot_session()) return nullptr;
        const auto& items = host.conn->loot_session()->items;
        auto it = items.find(static_cast<std::int16_t>(net::kCorpseSlotBegin + n));
        return it == items.end() ? nullptr : &it->second;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool outside) override {
        if (!host.conn) return false;
        auto& c = *host.conn;
        if (int n = index_after(id, "LW_LootSlot"); n >= 0) {
            auto slot = static_cast<std::int16_t>(net::kCorpseSlotBegin + n);
            if (button == "left") c.click_slot(net::ItemSlot::corpse(slot), shift(), ctrl()); // onto the cursor
            else if ((button == "right" || button == "rightup") && !outside) c.loot_item(slot, true, shift(), ctrl()); // auto loot
            return true;
        }
        if (button != "left") return false;
        if (id == "BroadcastButton") {
            if (!c.loot_session()) return false; // the window's last frame after the server closed the corpse
            // CLootWnd_LinkItemToChat @0x6bcf20: every item of the window, in slot order, as a link into the active
            // chat window's input (ItemBase::CreateItemTagString, CEditWnd::AddItemTag), which then has the focus.
            std::vector<std::string> tags;
            for (const auto& [slot, item] : c.loot_session()->items)
                tags.push_back(game::make_item_link(item, c.server_ext().wide_item_links(c)));
            request_chat_link_all(std::move(tags));
            return true;
        }
        if (id == "LootAllButton") c.loot_all();
        else if (id == "DoneButton") c.end_loot();
        else return false;
        return true;
    }
};

// ---------------------------------------------------------------- MerchantWnd (CMerchantWnd)

std::string coin_column(std::int64_t copper, int type) {
    std::int64_t v = type == 3 ? copper / 1000 : (copper / static_cast<std::int64_t>(type == 2 ? 100 : type == 1 ? 10 : 1)) % 10;
    return std::to_string(v);
}

class MerchantWindow : public StockWindow, public ParcelPage {
public:
    MerchantWindow() : StockWindow("MerchantWnd", false) {}
    bool parcels_shown(EqHost& host) override { return page_ == 2 && session(host) != nullptr; }
    void drop_selection() override { selected_.reset(); }
    bool shown(EqHost& host) override { return session(host) != nullptr; }
    void set_shown(EqHost& host, bool show) override {
        if (!show && host.conn) host.conn->close_merchant();
    }
    std::string title(EqHost& host) override {
        const auto* m = session(host);
        return m ? m->name : std::string();
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        const auto* m = session(host);
        if (!m) return;
        const net::Item* item = considered(host);
        // CMerchantWnd shows the buttons of the page on show: Buy / Sell on Purchase, Recover on Recover, Retrieve,
        // Send, To and Note on Parcels.
        static constexpr std::string_view kPurchase[] = {"MW_Buy_Button", "MW_Sell_Button"};
        static constexpr std::string_view kRecover[] = {"MW_Recover_Button"};
        static constexpr std::string_view kParcels[] = {"MW_Retrieve_Button", "MW_Send_Button", "MW_Send_To_Label",
                                                        "MW_Send_To_Edit", "MW_Note_Label", "MW_Note_Edit"};
        // Buy and Sell share a place: Sell while an inventory item is being considered, else Buy.
        bool selling = m->selected && host.conn->inventory().at(*m->selected);
        if (id == kPurchase[0]) out.shown = page_ == 0 && !selling;
        if (id == kPurchase[1]) out.shown = page_ == 0 && selling;
        if (id == "MW_UsableButton" || id == "MW_UsableLabel") out.shown = page_ == 0; // children of MW_PurchasePage
        for (auto x : kRecover) if (id == x) out.shown = page_ == 1;
        for (auto x : kParcels) if (id == x) out.shown = page_ == 2;
        if (id == "MW_MerchantName") {
            out.text = m->name;
        } else if (id == "MW_SelectedItemLabel") {
            if (item) out.text = item->name;
        } else if (id == "MW_SelectedPriceLabel") {
            if (item) out.text = price_text(host, *m, *item);
        } else if (id == "MW_SelectedItem") {
            out.decal = item && host.textures ? image(host.textures->item_icon(item->icon)) : kNone;
        } else if (id == "MW_Buy_Button") {
            out.enabled = buy_slot(*m).has_value() && m->pending == 0;
        } else if (id == "MW_Sell_Button") {
            out.enabled = m->selected && host.conn->inventory().at(*m->selected) && m->pending == 0;
        } else if (id == "MW_Send_Button") {
            out.enabled = m->selected && host.conn->inventory().at(*m->selected) && !send_to_.empty() &&
                          !m->parcel_sending && m->pending == 0;
        } else if (id == "MW_Send_To_Edit") {
            out.text = send_to_;
        } else if (id == "MW_Note_Edit") {
            out.text = note_;
        } else if (id == "MW_Recover_Button" || id == "MW_Retrieve_Button") {
            out.enabled = false; // buyback is not built; parcels are taken from Hailstone's list (eq.Parcels)
        } else if (id == "MW_Inspect_Button" || id == "MW_Preview_Button") {
            out.enabled = item != nullptr;
        } else if (id == "MW_UsableButton") {
            out.checked = !show_all_;
        }
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        const auto* m = session(host);
        if (!m || id != "ItemList") return nullptr;
        data_ = {};
        data_.columns = 9;
        // The coin columns' headings are coin pictures in the XML (Header_PlatCoin ..): their abbreviations here.
        data_.labels = {"", "Item Name", "Qty", "pp", "gp", "sp", "cp", "Lvl", ""};
        data_.widths = {24, 140, 28, 72, 24, 24, 24, 24, 20};
        slots_.clear();
        // While an inventory item is being considered no merchant row is.
        bool selling = m->selected && host.conn->inventory().at(*m->selected);
        const auto& prof = host.conn->profile();
        int row = 0;
        for (const auto& [mslot, item] : m->items) {
            if (!show_all_ && prof &&
                game::merchant_usable_hides(item, prof->class_, prof->race, [&] { return scroll_known(host, item); }))
                continue;
            ListRow r;
            std::int64_t price = item.price;
            // PurchasePageHandler::UpdateList: the merchant quantity and the level, "--" when there is none.
            std::string qty = game::merchant_quantity_text(static_cast<std::int32_t>(item.merchant_slot));
            std::int32_t level = game::merchant_list_level(item, [&]() -> std::optional<std::int32_t> {
                const auto& p = host.conn->profile();
                const auto& sc = item.effects[net::kEffectScroll];
                const game::SpellData* sp =
                    sc.effect > 0 ? host.conn->spell_table().get(static_cast<std::uint32_t>(sc.effect)) : nullptr;
                if (!p || !sp || p->class_ < 1 || !(item.classes & (1u << (p->class_ - 1)))) return std::nullopt;
                std::uint8_t l = host.conn->spell_level_needed(*sp, p->class_);
                return l == 0xff ? std::nullopt : std::optional<std::int32_t>(l);
            });
            r.texts = {"", item.name, qty, coin_column(price, 3), coin_column(price, 2), coin_column(price, 1),
                       coin_column(price, 0), game::merchant_level_text(level), ""};
            // A server's client may fill the Lvl column (the NMS patch: a spell scroll's level over the class mask).
            if (auto lvl = host.conn->server_ext().merchant_level(*host.conn, item)) r.texts[7] = *lvl;
            if (host.textures) r.icon = host.textures->item_icon(item.icon);
            if (selected_ && *selected_ == mslot && !selling) data_.selected = row;
            slots_.push_back(mslot);
            data_.rows.push_back(std::move(r));
            ++row;
        }
        return &data_;
    }

    bool list_click(EqHost& host, std::string_view id, int row, bool right, int) override {
        if (id != "ItemList" || row < 0 || row >= static_cast<int>(slots_.size())) return false;
        selected_ = slots_[static_cast<std::size_t>(row)];
        if (host.conn) host.conn->merchant_unselect(); // the merchant's item is considered now: Buy, its price
        if (right) inspect_selected(host);
        return true;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        const auto* m = session(host);
        if (!m || button != "left") return false;
        auto& c = *host.conn;
        if (id == "MW_Buy_Button") {
            // CMerchantWnd::HandleBuy: the quantity dialog at the Buy button (game::merchant_buy_limits: the merchant's
            // count capped at the stack, start 1 for a scroll with a spell); RequestGetItem with the answer.
            if (auto s = buy_slot(*m)) {
                const auto& it = m->items.at(*s);
                game::QuantityOpen q;
                if (it.stackable()) {
                    auto lim = game::merchant_buy_limits(static_cast<std::int32_t>(it.merchant_slot),
                                                         static_cast<std::int32_t>(it.max_stack),
                                                         it.item_type == 20 && it.effects[net::kEffectScroll].effect > 0);
                    q.max = lim.max, q.start = lim.start;
                } else {
                    q.max = q.start = 1;
                }
                at_button(host, q);
                std::uint32_t slot = *s;
                open_quantity(host, q, [&host, slot](std::int32_t n) {
                    if (host.conn && n > 0) host.conn->merchant_buy(slot, static_cast<std::uint32_t>(n));
                }, merchant_quantity_owner(host));
            }
        } else if (id == "MW_Sell_Button") {
            // HandleSell: max = start = the stack count (1 unstackable).
            if (m->selected) {
                const net::Item* it = c.inventory().at(*m->selected);
                if (it) {
                    auto lim = game::merchant_sell_limits(it->stackable() ? static_cast<std::int32_t>(it->stack_count) : 1);
                    game::QuantityOpen q;
                    q.max = lim.max, q.start = lim.start;
                    at_button(host, q);
                    net::ItemSlot where = *m->selected;
                    open_quantity(host, q, [&host, where](std::int32_t n) {
                        if (host.conn && n > 0) host.conn->merchant_sell(where, static_cast<std::uint32_t>(n));
                    }, merchant_quantity_owner(host));
                }
            }
        } else if (id == "MW_Send_Button") {
            // The mail page's RequestPutItem: a stack asks how many (the whole stack to start), then the parcel.
            if (m->selected) {
                if (const net::Item* it = c.inventory().at(*m->selected)) {
                    net::ItemSlot where = *m->selected;
                    std::string to = send_to_, note = note_;
                    auto send = [&host, where, to, note](std::int32_t n) {
                        if (host.conn && n > 0) host.conn->send_parcel(where, static_cast<std::uint32_t>(n), to, note);
                    };
                    if (it->stackable() && it->stack_count > 1) {
                        game::QuantityOpen q;
                        q.max = q.start = static_cast<std::int32_t>(it->stack_count);
                        at_button(host, q);
                        open_quantity(host, q, send, merchant_quantity_owner(host));
                    } else {
                        send(1);
                    }
                }
            }
        } else if (id == "MW_UsableButton") {
            show_all_ = !show_all_; // WndNotification: +0x60 = !Checked, the list rebuilt
        } else if (id == "MW_Done_Button") {
            c.close_merchant();
        } else if (id == "MW_Inspect_Button") {
            inspect_selected(host);
        } else if (id == "MW_Preview_Button") {
            // WndNotification @0x6f3f90: Send_OP_ItemPreviewRequest(the selected item) and the preview shown.
            if (const net::Item* it = considered(host)) host.conn->preview_item(it);
        } else {
            return false;
        }
        return true;
    }

    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        if (id == "MW_Send_To_Edit") send_to_ = text.substr(0, 63);
        else if (id == "MW_Note_Edit") note_ = text.substr(0, 127);
        else return false;
        return true;
    }

    std::optional<int> tab_page(EqHost&, std::string_view id) override {
        if (id == "MW_MerchantSubwindows") return page_;
        return std::nullopt;
    }
    void set_tab_page(EqHost&, std::string_view id, int page) override {
        if (id == "MW_MerchantSubwindows") page_ = page;
    }

private:
    static ChildArt image(const std::optional<ui::UiTextures::Cell>& c) {
        if (!c) return kNone;
        ChildArt a;
        a.kind = ChildArt::Image;
        a.cell = *c;
        return a;
    }
    // HandleBuy / HandleSell open the dialog at the Buy button's client rectangle's top left.
    static void at_button(EqHost& host, game::QuantityOpen& q) {
        if (host.click_rect[0] >= 0) {
            q.x = static_cast<std::int32_t>(host.click_rect[0]);
            q.y = static_cast<std::int32_t>(host.click_rect[1]);
        }
    }
    const net::MerchantSession* session(EqHost& host) {
        if (!host.conn || !host.conn->merchant() || !host.conn->merchant()->open) {
            selected_.reset();
            return nullptr;
        }
        return &*host.conn->merchant();
    }
    std::optional<std::uint32_t> buy_slot(const net::MerchantSession& m) const {
        if (selected_ && m.items.count(*selected_)) return selected_;
        return std::nullopt;
    }
    // The item being considered: the one picked from the inventory to sell, else the merchant's selected one.
    const net::Item* considered(EqHost& host) {
        const auto* m = session(host);
        if (!m) return nullptr;
        if (m->selected)
            if (const net::Item* it = host.conn->inventory().at(*m->selected)) return it;
        if (auto s = buy_slot(*m)) return &m->items.at(*s);
        return nullptr;
    }
    // CMerchantWnd::DisplayBuyOrSellPrice: the merchant's price, or what the item sells for.
    std::string price_text(EqHost& host, const net::MerchantSession& m, const net::Item& item) {
        std::int64_t copper = item.price;
        if (m.selected && host.conn->inventory().at(*m.selected) == &item) copper = game::sell_value(item, m.rate, 1);
        return game::coin_text(static_cast<std::uint32_t>(copper / 1000), static_cast<std::uint32_t>(copper / 100 % 10),
                               static_cast<std::uint32_t>(copper / 10 % 10), static_cast<std::uint32_t>(copper % 10));
    }
    void inspect_selected(EqHost& host) {
        if (const net::Item* it = considered(host); it && host.inspect_item) host.inspect_item(*it);
    }
    // UpdateList's ScrollKnown: the scroll's spell is in the book (SpellBookInfo::IsSpellKnown), or, for a spell of a
    // group with a rank, one of a higher rank is: from the group's top rank down while above the scroll's.
    static bool scroll_known(EqHost& host, const net::Item& item) {
        const auto& book = host.conn->spell_state().book;
        auto known = [&](std::uint32_t id) { return std::find(book.begin(), book.end(), id) != book.end(); };
        const std::int32_t id = item.effects[net::kEffectScroll].effect;
        if (id <= 0) return false;
        if (known(static_cast<std::uint32_t>(id))) return true;
        const auto& table = host.conn->spell_table();
        const game::SpellData* sp = table.get(static_cast<std::uint32_t>(id));
        if (!sp || sp->spell_group <= 0 || sp->spell_rank < 0) return false;
        for (const game::SpellData* s = table.by_group_and_rank(sp->spell_group, sp->spell_subgroup, -1, false);
             s && s->spell_rank > sp->spell_rank;
             s = table.by_group_and_rank(s->spell_group, s->spell_subgroup, s->spell_rank - 1, true))
            if (known(s->id)) return true;
        return false;
    }

    std::optional<std::uint32_t> selected_;
    int page_ = 0;
    // PurchasePageHandler +0x60, "show every item": the window builds MW_UsableButton checked (CMerchantWnd's
    // constructor, SetCheck(1)), so the list starts filtered; a click sets it to !Checked. Kept for the session, as
    // the original's window is.
    bool show_all_ = false;
    std::vector<std::uint32_t> slots_;
    ListData data_;
    std::string send_to_, note_; // the Parcels page's To and Note boxes
};

// ---------------------------------------------------------------- BankWnd / BigBankWnd (CBankWnd)

class BankWindow : public StockWindow {
public:
    BankWindow(const char* screen, int size, std::string prefix) : StockWindow(screen, false), size_(size), p_(std::move(prefix)) {}
    // CBankWnd is created from "BigBankWnd" (24 slots) or "BankWnd" (16), by the account's bank size.
    bool shown(EqHost& host) override {
        return host.conn && host.conn->bank() && host.conn->bank()->slots == size_;
    }
    void set_shown(EqHost& host, bool show) override {
        if (!show && host.conn) host.conn->close_bank();
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!shown(host)) return;
        const auto& c = *host.conn;
        auto bc = c.bank_coins();
        if (id == p_ + "BankerName") out.text = c.bank()->name;
        else if (int m = index_after(id, p_ + "Money"); m >= 0 && m < 4) out.text = std::to_string(*bc.of(coin_type(m), false));
        else if (id == p_ + "SharedMoney0") out.text = std::to_string(bc.shared_platinum);
        else if (id == "AltStorageButton" || id == "ChangeButton") out.enabled = false; // not built
        else if (id == kAutoInventoryChild) out.enabled = c.inventory().cursor() != nullptr;
    }
    const net::Item* slot_item(EqHost& host, std::string_view id) override {
        auto s = slot_of(id);
        return s && host.conn ? host.conn->inventory().at(*s) : nullptr;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool outside) override {
        if (!shown(host)) return false;
        auto& c = *host.conn;
        if (auto s = slot_of(id)) {
            if (button == "left") c.click_slot(*s, shift(), ctrl());
            else if ((button == "right" || button == "rightup") && !outside) item_right_click(host, *s);
            return true;
        }
        if (button != "left") return false;
        auto bc = c.bank_coins();
        if (int m = index_after(id, p_ + "Money"); m >= 0 && m < 4) money_click(c, net::kCoinBank, coin_type(m), *bc.of(coin_type(m), false));
        else if (id == p_ + "SharedMoney0") money_click(c, net::kCoinSharedBank, 3, bc.shared_platinum, true);
        else if (id == "AutoButton") c.auto_bank();
        else if (id == kAutoInventoryChild) c.say("/autoinventory");
        else if (id == "DoneButton") c.close_bank();
        else return false;
        return true;
    }

private:
    std::optional<net::ItemSlot> slot_of(std::string_view id) const {
        if (int n = index_after(id, p_ + "BankSlot"); n >= 0 && n < size_)
            return net::ItemSlot{net::kInvBank, static_cast<std::int16_t>(n), -1, -1, 0, 0};
        if (int n = index_after(id, p_ + "SharedBankSlot"); n >= 0 && n < 2)
            return net::ItemSlot{net::kInvSharedBank, static_cast<std::int16_t>(n), -1, -1, 0, 0};
        return std::nullopt;
    }
    int size_;
    std::string p_;
};

// ---------------------------------------------------------------- TradeWnd (CTradeWnd) / GiveWnd (CGiveWnd)

class TradeWindow : public StockWindow {
public:
    TradeWindow(const char* screen, bool npc, std::string prefix)
        : StockWindow(screen, false), npc_(npc), p_(std::move(prefix)) {}
    bool shown(EqHost& host) override { return session(host) != nullptr; }
    void set_shown(EqHost& host, bool show) override {
        if (!show && host.conn) host.conn->trade_cancel();
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        const auto* t = session(host);
        if (!t) return;
        std::string me = host.conn->world().player() ? host.conn->world().player()->name : std::string();
        if (id == p_ + "HisName" || id == p_ + "NPCName") out.text = game::display_name(t->partner_name);
        else if (id == p_ + "MyName") out.text = me;
        else if (int m = index_after(id, p_ + "MyMoney"); m >= 0 && m < 4)
            out.text = std::to_string(t->my_coins[static_cast<std::size_t>(coin_type(m))]);
        else if (int h = index_after(id, p_ + "HisMoney"); h >= 0 && h < 4)
            out.text = std::to_string(t->their_coins[static_cast<std::size_t>(coin_type(h))]);
    }
    const net::Item* slot_item(EqHost& host, std::string_view id) override {
        const auto* t = session(host);
        if (!t) return nullptr;
        int n = index_after(id, npc_ ? p_ + "MyItemSlot" : p_ + "TradeSlot");
        if (n < 0) return nullptr;
        if (n < 8) return host.conn->inventory().at(net::ItemSlot{net::kInvTrade, static_cast<std::int16_t>(n), -1, -1, 0, 0});
        auto it = t->theirs.find(static_cast<std::int16_t>(n - 8));
        return it == t->theirs.end() ? nullptr : &it->second;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool outside) override {
        const auto* t = session(host);
        if (!t) return false;
        auto& c = *host.conn;
        int n = index_after(id, npc_ ? p_ + "MyItemSlot" : p_ + "TradeSlot");
        if (n >= 0) {
            if (button == "left" && n < 8) c.click_slot(net::ItemSlot{net::kInvTrade, static_cast<std::int16_t>(n), -1, -1, 0, 0}, shift(), ctrl());
            else if ((button == "right" || button == "rightup") && !outside)
                if (const net::Item* it = slot_item(host, id); it && host.inspect_item) host.inspect_item(*it);
            return true;
        }
        if (button != "left") return false;
        if (int m = index_after(id, p_ + "MyMoney"); m >= 0 && m < 4) {
            // CTradeWnd_DropCursorMoneyIntoTrade: the coin on the cursor goes into the trade (back to the inventory
            // first: trade_coin moves it inventory -> cursor -> trade as the original does).
            if (auto held = cursor_coin(c)) {
                c.move_coin(net::kCoinCursor, net::kCoinInventory, held->first, held->first, held->second);
                c.trade_coin(held->first, held->second);
            }
            return true;
        }
        if (id == p_ + "Trade_Button" || id == p_ + "Give_Button") c.trade_accept();
        else if (id == p_ + "Cancel_Button") c.trade_cancel();
        else return false;
        return true;
    }

private:
    const net::TradeSession* session(EqHost& host) {
        if (!host.conn || !host.conn->trade() || !host.conn->trade()->open || host.conn->trade()->with_npc != npc_) return nullptr;
        return &*host.conn->trade();
    }
    bool npc_;
    std::string p_;
};

} // namespace

void open_container(EqHost& host, const net::ItemSlot& s) {
    auto& cs = containers();
    if (std::none_of(cs.open.begin(), cs.open.end(), [&](const net::ItemSlot& o) { return o.same(s); })) toggle_container(host, s);
}

// A quick right click on an item (CInvSlot::HandleRButtonUp, GameConnection::right_click_item): a bag opens or
// closes its container, a book is read, a common item is used (OP_ItemVerifyRequest: clickies, food and drink,
// scrolls and tomes on servers that scribe them). The item display is the held button's (inspect_held).
void item_right_click(EqHost& host, const net::ItemSlot& slot) {
    if (!host.conn) return;
    const ImGuiIO& io = ImGui::GetIO();
    using R = net::GameConnection::SlotRightClick;
    switch (host.conn->right_click_item(slot, {io.KeyShift, io.KeyCtrl, io.KeyAlt})) {
    case R::ToggleContainer: toggle_container(host, slot); break;
    default: break; // the item preview went out (GameConnection::preview_item); the trader's selection is not built
    }
}

bool item_left_held(EqHost& host, const net::ItemSlot& slot) {
    if (!host.conn || !host.in_game || host.cursor_button || host.cursor_spell) return false;
    const auto& inv = host.conn->inventory();
    const auto& coins = host.conn->cursor_coins();
    if (inv.cursor() || std::any_of(coins.begin(), coins.end(), [](std::uint32_t c) { return c != 0; })) return false;
    const ImGuiIO& io = ImGui::GetIO();
    auto b = game::inventory_hot_button(slot, inv.at(slot), io.KeyShift || io.KeyCtrl);
    if (!b) return false;
    host.cursor_button = std::move(*b);
    ++host.cursor_serial;
    return true;
}

namespace {

// ---------------------------------------------------------------- TradeskillWnd (CTradeskillWnd)

// The tradeskill window (game/tradeskill.h, CTradeSkillWnd::WndNotification @0x77e7f0) over EQEmu's recipe
// packets: the container (SkillLabel, ContainerIcon, ContainerName), favourites (Add / Remove the selected recipe,
// kept in the UI ini; Show: OP_RecipesFavorite), the search (Min. / Max. Trivial, 0 and 999 when the window opens,
// Partial Name, Search Recipes or Enter in the name box: OP_RecipesSearch; both wait 5 s), the matching recipes
// (RecipeList: name, trivial in its colour; a click asks for the components), the components (RequiredIcon0..9,
// the item names as tooltips), Combine (OP_RecipeAutoCombine: the server takes the components from the inventory;
// then 2.1 s), Experiment (CContainerMgr::OpenExperimentContainer: the container's own window, this one closes),
// Auto-Inventory (the cursor item) and Close.
// Hailstone's children on top (not in the original's XML): ComponentList (one row per component: name, "have /
// need" against the general slots and their bags, green when there are enough, red when not), MakeableLabel (how
// many combines the carried components make), CombineCountEdit and the batch buttons CombineOneButton,
// CombineCountButton (that many), CombineAllButton (until they run out) and CombineStopButton
// (GameConnection::tradeskill_start_batch), and CombineProgressLabel.
class TradeskillWindow : public StockWindow {
public:
    TradeskillWindow() : StockWindow("TradeskillWnd", false) {}

    bool shown(EqHost& host) override {
        bool open = host.conn && host.conn->tradeskill().open;
        if (open && !was_open_) min_ = "0", max_ = "999", text_.clear(); // CTradeSkillWnd_ResetRecipeDisplay
        was_open_ = open;
        if (!open) return false;
        const auto& t = host.conn->tradeskill();
        if (t.world) return host.conn->world_container().has_value();
        const net::Item* bag = host.conn->inventory().at(t.container_slot);
        if (!bag || bag->id != t.container_id) { // the bag left its slot
            host.conn->close_tradeskill();
            return false;
        }
        return true;
    }
    void set_shown(EqHost& host, bool show) override {
        if (!show && host.conn) host.conn->close_tradeskill();
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& t = host.conn->tradeskill();
        const net::RecipeReply* r = t.selected_recipe();
        if (id == "MinTrivialEdit") out.text = min_;
        else if (id == "MaxTrivialEdit") out.text = max_;
        else if (id == "SearchTextEdit") out.text = text_;
        else if (id == "ContainerName") out.text = t.container_name;
        else if (id == "SkillLabel") {
            // "Tradeskill (skill)" in the XML: the container's tradeskill and the character's value in it.
            out.text = t.container_name;
            if (const auto& p = host.conn->profile()) {
                game::ContainerUser u{p->class_, static_cast<std::int32_t>(p->race), static_cast<std::int32_t>(p->deity),
                                     host.conn->research_class_mask()};
                game::ContainerSkill sk = game::container_tradeskill(t.object_type, 0, 0, 0, u);
                if (sk.skill >= 0 && sk.skill != 0xff) {
                    auto skill = static_cast<std::uint32_t>(sk.skill);
                    out.text = std::format("{} ({})", host.conn->skill_name(skill), host.conn->skill_value(skill));
                }
            }
        }
        else if (id == "ContainerIcon") {
            out.decal = image(host.textures ? host.textures->item_icon(t.container_icon) : std::nullopt);
            out.tooltip = t.container_name;
        } else if (int n = index_after(id, "RequiredIcon"); n >= 0 && n < 10) {
            const net::RecipeComponent* c =
                t.details && n < static_cast<int>(t.details->components.size()) ? &t.details->components[static_cast<std::size_t>(n)] : nullptr;
            out.decal = c && host.textures ? image(host.textures->item_icon(c->icon)) : kNone;
            if (c) out.tooltip = c->name;
        } else if (id == "AddFavoriteButton" || id == "RemoveFavoriteButton") {
            // sub_0077c9b0 (every 250 ms): a selected recipe; Add with an empty place and the recipe not among the
            // favourites, Remove with it among them.
            const auto& f = host.conn->recipe_favorites();
            bool fav = r && game::is_recipe_favorite(f, r->recipe_id);
            bool room = std::find(f.begin(), f.end(), 0u) != f.end();
            out.enabled = id == "AddFavoriteButton" ? r && room && !fav : fav;
        } else if (id == "SearchButton") out.enabled = search_ready(host);
        else if (id == "CombineButton") out.enabled = combine_ready(host) && !t.batch;
        else if (id == "CombineOneButton" || id == "CombineCountButton" || id == "CombineAllButton")
            out.enabled = !t.batch && host.conn->batch_refusal().empty();
        else if (id == "CombineStopButton") out.enabled = t.batch.has_value();
        else if (id == "CombineCountEdit") out.text = count_;
        else if (id == "MakeableLabel") {
            game::RecipeNeeds needs = host.conn->tradeskill_needs();
            if (!needs.components.empty())
                out.text = std::format("{}{}", needs.complete ? "" : "up to ", needs.makeable);
        } else if (id == "CombineProgressLabel" && t.batch) {
            out.text = t.batch->wanted ? std::format("Combining {} of {}", t.batch->done, t.batch->wanted)
                                       : std::format("Combining: {} done", t.batch->done);
        }
        else if (id == "AutoInvButton") out.enabled = host.conn->inventory().cursor() != nullptr;
    }
    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        if (id == "MinTrivialEdit") min_ = text;
        else if (id == "MaxTrivialEdit") max_ = text;
        else if (id == "SearchTextEdit") text_ = text;
        else if (id == "CombineCountEdit") count_ = text;
        else return false;
        return true;
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (host.conn && id == "ComponentList") return components(host);
        if (!host.conn || id != "RecipeList") return nullptr;
        const auto& t = host.conn->tradeskill();
        list_ = {};
        list_.columns = 2;
        list_.labels = {"Recipe Name", "Trivial"};
        list_.widths = {140, 50};
        ids_.clear();
        std::int32_t skill = skill_value(host);
        for (const auto& r : t.recipes) {
            ListRow row;
            row.texts = {r.name, std::to_string(r.trivial)};
            // sub_0077bd30: the trivial column in the colour of trivial - skill.
            row.cell_colors = {std::nullopt, argb_to_rgb(game::recipe_trivial_color(static_cast<std::int32_t>(r.trivial), skill))};
            if (r.recipe_id == t.selected) list_.selected = static_cast<int>(list_.rows.size());
            ids_.push_back(r.recipe_id);
            list_.rows.push_back(std::move(row));
        }
        return &list_;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool, int) override {
        if (!host.conn || id != "RecipeList") return false;
        if (row >= 0 && row < static_cast<int>(ids_.size())) host.conn->tradeskill_select(ids_[static_cast<std::size_t>(row)]);
        return true;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn) return false;
        if (index_after(id, "RequiredIcon") >= 0 || id == "ContainerIcon") return true;
        if (button != "left") return false;
        auto& c = *host.conn;
        const auto& t = c.tradeskill();
        if (id == "SearchButton") {
            // Send_OP_RecipesSearch: the edits' atoi (999 for a missing maximum box only).
            if (search_ready(host))
                c.tradeskill_search(static_cast<std::uint32_t>(std::atoi(min_.c_str())),
                                    static_cast<std::uint32_t>(std::atoi(max_.c_str())), text_);
        }
        else if (id == "ShowFavoritesButton") c.tradeskill_show_favorites();
        else if (id == "AddFavoriteButton") c.tradeskill_favorite(t.selected, true);
        else if (id == "RemoveFavoriteButton") c.tradeskill_favorite(t.selected, false);
        else if (id == "CombineButton") {
            if (combine_ready(host) && !t.batch) c.tradeskill_combine();
        }
        else if (id == "CombineOneButton") c.tradeskill_start_batch(1);
        else if (id == "CombineCountButton") {
            int n = std::atoi(count_.c_str());
            if (n > 0) c.tradeskill_start_batch(static_cast<std::uint32_t>(n));
        }
        else if (id == "CombineAllButton") c.tradeskill_start_batch(0);
        else if (id == "CombineStopButton") c.tradeskill_stop_batch();
        else if (id == "AutoInvButton") c.auto_inventory();
        else if (id == "ExperimentButton") {
            // Experiment: back to the container's own window.
            net::ItemSlot bag = t.container_slot;
            bool world = t.world;
            if (world) c.show_world_container_window();
            else c.close_tradeskill();
            auto& cs = containers();
            if (!world && std::none_of(cs.open.begin(), cs.open.end(), [&](const net::ItemSlot& o) { return o.same(bag); }))
                cs.toggle(bag);
        } else if (id == "CloseButton") c.close_tradeskill();
        else return false;
        return true;
    }

private:
    static ChildArt image(const std::optional<ui::UiTextures::Cell>& c) {
        if (!c) return kNone;
        ChildArt a;
        a.kind = ChildArt::Image;
        a.cell = *c;
        return a;
    }
    // sub_0077c9b0: Search waits for +0x444; Combine for +0x36c +4, a selected recipe, an empty cursor (no item) and
    // no item request pending.
    static bool search_ready(EqHost& host) {
        return host.conn && net::GameConnection::now_ms() > host.conn->tradeskill().request_ready_ms;
    }
    static bool combine_ready(EqHost& host) {
        if (!host.conn) return false;
        const auto& t = host.conn->tradeskill();
        return net::GameConnection::now_ms() > t.combine_ready_ms && t.selected_recipe() && !t.combining &&
               !host.conn->inventory().cursor();
    }
    // CTradeSkillWnd_UpdateSkillLabel's value (+0x43c): the container's tradeskill as the character has it.
    static std::int32_t skill_value(EqHost& host) {
        const auto& p = host.conn->profile();
        if (!p) return 0;
        const auto& t = host.conn->tradeskill();
        game::ContainerUser u{p->class_, static_cast<std::int32_t>(p->race), static_cast<std::int32_t>(p->deity),
                             host.conn->research_class_mask()};
        game::ContainerSkill sk = game::container_tradeskill(t.object_type, 0, 0, 0, u);
        if (sk.skill < 0 || sk.skill == 0xff) return 0;
        return static_cast<std::int32_t>(host.conn->skill_value(static_cast<std::uint32_t>(sk.skill)));
    }

    // ComponentList: game::recipe_needs as rows (name, "have / need"; the item's icon), green with enough, else red.
    const ListData* components(EqHost& host) {
        game::RecipeNeeds n = host.conn->tradeskill_needs();
        components_ = {};
        components_.columns = 2;
        components_.labels = {"Component", "Have"};
        components_.widths = {140, 50};
        for (const game::ComponentNeed& c : n.components) {
            ListRow row;
            row.texts = {c.name, std::format("{} / {}", c.have, c.need)};
            row.color = c.have >= c.need ? Rgb{0.35f, 0.85f, 0.35f} : Rgb{0.95f, 0.3f, 0.3f};
            if (host.textures) row.icon = host.textures->item_icon(c.icon);
            components_.rows.push_back(std::move(row));
        }
        return &components_;
    }

    // CTradeSkillWnd_ResetRecipeDisplay (AboutToShow): 0, 999 and nothing.
    std::string min_ = "0", max_ = "999", text_;
    std::string count_ = "1";
    ListData components_;
    bool was_open_ = false;
    ListData list_;
    std::vector<std::uint32_t> ids_;
};

} // namespace

void add_item_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    auto merchant = std::make_unique<MerchantWindow>();
    ParcelPage* parcels = merchant.get(); // both windows live in `out`, for as long as each other
    out.push_back(std::make_unique<TradeskillWindow>());
    out.push_back(std::make_unique<InventoryWindow>(parcels));
    for (int n = 1; n <= 36; ++n) out.push_back(std::make_unique<BagWindow>(n));
    out.push_back(std::make_unique<WorldContainerWindow>());
    out.push_back(std::make_unique<LootWindow>());
    out.push_back(std::move(merchant));
    out.push_back(std::make_unique<BankWindow>("BankWnd", 16, "BW_"));
    out.push_back(std::make_unique<BankWindow>("BigBankWnd", 24, "BIGB_"));
    out.push_back(std::make_unique<TradeWindow>("TradeWnd", false, "TRDW_"));
    out.push_back(std::make_unique<TradeWindow>("GiveWnd", true, "GVW_"));
}

} // namespace hailstone::app
