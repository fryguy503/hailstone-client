// The real estate windows (CRealEstatePurchaseWnd, CRealEstateManageWnd, CRealEstateItemsWnd,
// CRealEstateNeighborhoodWnd, CRealEstatePlotSearchWnd) over the connection's real estate state
// (net/game_connection_real_estate.cpp, game/real_estate.h). What each shows and sends follows the original's
// functions named below; the packs draw them
// (ui/<pack>/realestate*.lua). Child ids are this client's, the windows being the packs' own.

#include <algorithm>
#include <cmath>
#include <string>

#include <imgui.h>

#include "app/lua_eq_windows.h"
#include "core/saturate.h"
#include "game/inventory.h"
#include "game/quantity_dialog.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

using game::kReNone;
using net::GameConnection;
using net::ReType;

std::string eqstr(EqHost& host, std::uint32_t id, std::vector<std::string> args = {}) {
    return host.conn ? host.conn->strings().format(id, args) : std::string();
}

ListData make_list(std::initializer_list<std::pair<const char*, int>> cols) {
    ListData d;
    d.columns = static_cast<int>(cols.size());
    for (auto [l, w] : cols) {
        d.labels.emplace_back(l);
        d.widths.push_back(w);
    }
    return d;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

constexpr Rgb kRed{1.0f, 0.0f, 0.0f}; // 0xffff0000, the original's refused / expired colour

std::string guild_name(const GameConnection& c, std::int32_t id) {
    if (id == -1) return {};
    auto it = c.guild().names.find(static_cast<std::uint32_t>(id));
    return it == c.guild().names.end() ? std::to_string(id) : it->second;
}

// Coin in copper as the four piles (platinum first), as the windows split the escrow's coin entry.
std::array<std::uint64_t, 4> split_copper(std::uint64_t c) {
    return {c / 1000, (c / 100) % 10, (c / 10) % 10, c % 10};
}

std::string coin_line(std::uint64_t copper) {
    auto p = split_copper(copper);
    std::string s = game::coin_text(static_cast<std::uint32_t>(p[0]), static_cast<std::uint32_t>(p[1]),
                                    static_cast<std::uint32_t>(p[2]), static_cast<std::uint32_t>(p[3]));
    return s.empty() ? "0 copper" : s;
}

// A currency other than coin: its name, dbstr type 0x12 of the currency id (the plural, as CInventoryWnd's Alt.
// Currency list and CNewPointMerchantWnd read it; type 0x13 is the AA categories), else the currency id.
std::string currency_line(const GameConnection& c, std::uint32_t currency, std::uint64_t amount) {
    if (currency == 0) return coin_line(amount);
    std::string name = c.strings().db_string(currency, 0x12);
    if (name.empty()) name = "currency " + std::to_string(currency);
    return std::to_string(amount) + " " + name;
}

// One cost entry's text: its leaves together (PopulateCostCombo @0x74d240 lists one entry per choice).
std::string cost_text(const GameConnection& c, const net::ReCostEntry& e) {
    std::string s;
    for (const auto& leaf : e.leaves) {
        if (!s.empty()) s += ", ";
        s += currency_line(c, leaf.currency, leaf.amount);
    }
    return s.empty() ? "free" : s;
}

void fill_cost_combo(const GameConnection& c, std::uint32_t group, ComboData& combo, std::vector<std::uint32_t>& keys,
                     std::optional<std::uint32_t> selected_key) {
    // RealEstate::PopulateCostCombo @0x74d240: the group's entries, each one choice with its key as the item data.
    combo.choices.clear();
    keys.clear();
    const net::ReCostSet* set = c.real_estate().cost_set(group);
    if (!set) {
        combo.selected = -1;
        return;
    }
    int keep = combo.selected;
    for (const auto& e : set->entries) {
        combo.choices.push_back(cost_text(c, e));
        keys.push_back(e.key);
    }
    combo.selected = -1;
    if (selected_key) {
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == *selected_key) combo.selected = static_cast<int>(i);
    } else if (keep >= 0 && keep < static_cast<int>(keys.size())) {
        combo.selected = keep;
    } else if (!keys.empty()) {
        combo.selected = 0;
    }
}

bool owns_plot(const GameConnection& c, const net::ReRecord* r) {
    // 0x4d6b10: a house or crate stands for its plot (the nearest plot above it); a player plot whose owner is
    // the player.
    const auto& m = c.real_estate();
    for (int i = 0; r && i < 10; ++i) {
        ReType t = m.definition_type(r->definition);
        if (!game::re_is_house_type(t) && !game::re_is_crate_type(t)) break;
        r = m.record(r->parent);
    }
    return r && m.definition_type(r->definition) == ReType::Plot && !r->owner.empty() &&
           iequals(r->owner, c.character_name());
}

std::int32_t neighborhood_guild(const GameConnection& c, const net::ReRecord* r) {
    const auto& m = c.real_estate();
    for (int i = 0; r && i <= 10; ++i) {
        if (game::re_is_neighborhood_type(m.definition_type(r->definition))) return r->guild_id;
        r = m.record(r->parent);
    }
    return -1;
}

void ask_confirm(EqHost& host, std::string text, int timeout_ms, std::function<void()> yes) {
    EqHost::Confirm c;
    c.text = std::move(text);
    c.timeout_ms = timeout_ms;
    c.done = [y = std::move(yes)](bool ok) {
        if (ok) y();
    };
    host.confirm = std::move(c);
}

// ---------------------------------------------------------------------------------------------------------------
// RealEstatePurchaseWnd: CRealEstatePurchaseWnd_SetPlotInfo @0x74dcb0 and WndNotification @0x74d500.
class PurchaseWindow : public StockWindow {
public:
    PurchaseWindow() : StockWindow("RealEstatePurchaseWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->real_estate_ui().purchase_open; }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_real_estate_window(GameConnection::ReWindow::Purchase, show);
    }

    std::string title(EqHost& host) override {
        // The title: the record's address, with " (<id>)" for a GM (the PC's vt+0x90).
        const net::ReRecord* r = record(host);
        if (!r) return {};
        std::string t = r->address.empty() ? r->name : r->address;
        if (host.conn->player_is_gm()) t += " (" + std::to_string(r->id) + ")";
        return t;
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        const net::ReRecord* r = record(host);
        if (!r) return;
        const auto& c = *host.conn;
        const auto& m = c.real_estate();
        const net::ReDefinition* d = m.definition(r->definition);
        if (!d) return;
        State s = state(host, *r, *d);
        bool guild = d->type == ReType::GuildPlot;
        if (id == "AddressLabel") out.text = r->address;
        else if (id == "NameLabel") out.text = r->name;
        else if (id == "TypeLabel") out.text = guild ? "Guild" : "Player";
        else if (id == "OwnerLabel") out.text = guild ? guild_name(c, r->guild_id) : r->owner;
        else if (id == "CapacityLabel") out.text = std::to_string(d->capacity);
        else if (id == "VaultLabel") out.text = std::to_string(d->vault_capacity);
        else if (id == "PetLabel") out.text = std::to_string(d->pet_capacity < 0 ? d->capacity : d->pet_capacity);
        else if (id == "CategoryLabel") out.text = c.strings().db_string(d->category, 0x1b);
        else if (id == "DescriptionLabel") out.text = eqstr(host, guild ? 0x3416 : 0x3415);
        else if (id == "NeighborhoodGuildLabel") {
            // The neighborhood's guild, red when it is not the player's.
            std::int32_t g = neighborhood_guild(c, r);
            out.shown = g != -1;
            out.text = guild_name(c, g);
            if (g != -1 && static_cast<std::uint32_t>(g) != c.guild().guild_id) out.color = kRed;
        } else if (id == "PurchaseButton") {
            out.shown = !s.mine;
            out.enabled = s.purchasable && host.conn->re_can_purchase(r->id, false);
        } else if (id == "SellButton") out.shown = s.mine;
        else if (id == "TransferButton") out.shown = s.transfer;
        else if (id == "ManageButton") out.enabled = s.owned;
        else if (id == "OptOutCheck") {
            out.checked = r->opt_out;
            out.enabled = s.manage;
        } else if (id == "UpkeepCombo") out.enabled = s.manage;
    }

    const ComboData* combo(EqHost& host, std::string_view id) override {
        const net::ReRecord* r = record(host);
        const net::ReDefinition* d = r ? host.conn->real_estate().definition(r->definition) : nullptr;
        if (!d) return nullptr;
        if (id == "PriceCombo") {
            fill_cost_combo(*host.conn, d->price_group, price_, price_keys_, std::nullopt);
            return &price_;
        }
        if (id == "UpkeepCombo") {
            fill_cost_combo(*host.conn, d->upkeep_group, upkeep_, upkeep_keys_,
                            r->owner.empty() && r->guild_id == -1 ? std::nullopt : std::optional(r->upkeep_set));
            return &upkeep_;
        }
        return nullptr;
    }
    bool combo_select(EqHost& host, std::string_view id, int index) override {
        if (id == "PriceCombo") {
            combo(host, id);
            price_.selected = index;
            return true;
        }
        if (id != "UpkeepCombo") return false;
        combo(host, id);
        upkeep_.selected = index;
        // The upkeep choice of an owned yard: escrow action 2 with the entry's key (0x5aee30's check is the
        // server's here); for others the combo goes back to the record's.
        const net::ReRecord* yard = host.conn->real_estate().record(host.conn->real_estate().occupancy().yard);
        if (yard && index >= 0 && index < static_cast<int>(upkeep_keys_.size()) &&
            (owns_plot(*host.conn, yard) || host.conn->player_is_gm()))
            host.conn->re_set_upkeep(upkeep_keys_[static_cast<std::size_t>(index)]);
        return true;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        if (id == "PurchaseButton") {
            // RealEstate_CanPurchasePlot (verbose), then 0x45be with the combos' keys; the window hides.
            const net::ReRecord* r = record(host);
            combo(host, "PriceCombo"); // the choices as they stand (a pack may not have drawn them)
            combo(host, "UpkeepCombo");
            if (!r || price_.selected < 0 || price_.selected >= static_cast<int>(price_keys_.size())) return true;
            std::uint32_t upkeep = upkeep_.selected >= 0 && upkeep_.selected < static_cast<int>(upkeep_keys_.size())
                                       ? upkeep_keys_[static_cast<std::size_t>(upkeep_.selected)]
                                       : 0;
            c.re_purchase(price_keys_[static_cast<std::size_t>(price_.selected)], upkeep);
        } else if (id == "SellButton") {
            // RealEstate_CanSellCurrentPlot (verbose), then dialog 100: eqstr 7634 for 10 s; yes sends 0x792c.
            if (c.re_can_sell(true) && record(host))
                ask_confirm(host, eqstr(host, 0x1dd2), 10000, [&host] { if (host.conn) host.conn->re_sell(); });
        } else if (id == "TransferButton") {
            // RealEstate_CanTransferPlotToTarget (verbose), then dialog 0x65: eqstr 7635 with the target's name.
            if (c.re_can_transfer(true)) {
                auto t = c.world().spawns.find(c.target());
                std::string name = t == c.world().spawns.end() ? std::string() : t->second.name;
                ask_confirm(host, eqstr(host, 0x1dd3, {name}), 10000, [&host] { if (host.conn) host.conn->re_transfer(); });
            }
        } else if (id == "ManageButton") c.real_estate_command("manage");
        else if (id == "ItemsButton") c.real_estate_command("items");
        else if (id == "OptOutCheck") {
            // 0x6573 {the yard, the box's new state} (0x5b09d0's check is the server's here).
            const net::ReRecord* yard = c.real_estate().record(c.real_estate().occupancy().yard);
            if (yard) c.re_opt_out(!yard->opt_out);
        } else return false;
        return true;
    }

private:
    struct State {
        bool owned = false, purchasable = false, manage = false, mine = false, transfer = false;
    };
    const net::ReRecord* record(EqHost& host) {
        if (!host.conn) return nullptr;
        return host.conn->real_estate().record(host.conn->real_estate_ui().purchase_re);
    }
    State state(EqHost& host, const net::ReRecord& r, const net::ReDefinition& d) {
        // SetPlotInfo: +0x22c buyable (not owned and a price), +0x22d manage (0x4ff6c0's access 0xc, read here
        // as the player's own plot or GM), +0x22e the owner's (the player's plot, GM, or a guild plot of the
        // player's guild), +0x22f transfer (the owner's, not a guild plot).
        const auto& c = *host.conn;
        State s;
        bool own = owns_plot(c, &r) || c.player_is_gm();
        bool guild_own = d.type == ReType::GuildPlot && r.guild_id != -1 && static_cast<std::uint32_t>(r.guild_id) == c.guild().guild_id;
        const net::ReCostSet* price = c.real_estate().cost_set(d.price_group);
        s.owned = c.real_estate().already_owned(r.id);
        s.purchasable = !s.owned && price && !price->entries.empty();
        s.manage = !s.purchasable && own;
        s.mine = !s.purchasable && (own || guild_own);
        s.transfer = s.mine && d.type != ReType::GuildPlot;
        return s;
    }

    ComboData price_, upkeep_;
    std::vector<std::uint32_t> price_keys_, upkeep_keys_;
};

// ---------------------------------------------------------------------------------------------------------------
// RealEstateManageWnd: the access lists (CRealEstateManageWnd_RefreshAccessLists @0x744200), the escrow page
// (RefreshBalanceList @0x741930, RefreshUpkeepList @0x742cd0) and WndNotification @0x745090, on the yard.
class ManageWindow : public StockWindow {
public:
    ManageWindow() : StockWindow("RealEstateManageWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->real_estate_ui().manage_open; }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_real_estate_window(GameConnection::ReWindow::Manage, show);
    }
    std::string title(EqHost& host) override {
        const net::ReRecord* r = yard(host);
        return r ? (r->address.empty() ? r->name : r->address) : std::string();
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        const net::ReRecord* r = yard(host);
        if (!r) return;
        auto& c = *host.conn;
        bool own = owns_plot(c, r) || c.player_is_gm();
        // "Perm_<group>_<bit>": the access table's bits.
        if (id.starts_with("Perm_")) {
            int g = 0, b = 0;
            if (std::sscanf(std::string(id).c_str(), "Perm_%d_%d", &g, &b) == 2 && g >= 0 && g < net::kReAccessGroups &&
                b >= 0 && b < net::kReAccessBits) {
                out.checked = (access(host).groups[static_cast<std::size_t>(g)].bits >> b) & 1u;
                out.enabled = own;
            }
            return;
        }
        if (id == "TimeLeftLabel") {
            // RealEstate_FormatTimeRemaining of the least days; red when expired.
            double days = game::re_upkeep_days(c.real_estate(), *r);
            out.text = game::re_format_time_left(days);
            if (days == 0) out.color = kRed;
        } else if (id == "DepositButton") {
            const auto& held = c.cursor_coins();
            out.enabled = c.real_estate().already_owned(r->id) && std::any_of(held.begin(), held.end(), [](auto n) { return n != 0; });
        } else if (id == "WithdrawButton") out.enabled = balance_sel_ >= 0 && balance_sel_ < static_cast<int>(balance_rows_.size());
        else if (id == "AddMemberButton" || id == "UpkeepCombo") out.enabled = own;
        else if (id == "RemoveMemberButton" || id == "MemberGroupCombo")
            out.enabled = own && member_sel_ >= 0 && member_sel_ < static_cast<int>(access(host).members.size());
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        const net::ReRecord* r = yard(host);
        if (!r) return nullptr;
        auto& c = *host.conn;
        if (id == "MemberList") {
            members_ = make_list({{"Name", 160}, {"Group", 120}});
            for (const auto& mem : access(host).members)
                members_.rows.push_back(ListRow{{mem.name, group_name(mem.group)}});
            members_.selected = member_sel_;
            return &members_;
        }
        if (id == "BalanceList") {
            // The escrow by currency; the coin entry (key 0, total copper) as its four denominations.
            balance_ = make_list({{"Currency", 140}, {"Balance", 120}});
            balance_rows_.clear();
            for (const auto& b : r->escrow) {
                auto total = saturate_cast<std::uint64_t>(b.amount); // the server's f64: NaN or negative 0
                if (b.currency == 0) {
                    static const char* kNames[] = {"Platinum", "Gold", "Silver", "Copper"};
                    auto piles = split_copper(total);
                    for (int i = 0; i < 4; ++i) {
                        balance_.rows.push_back(ListRow{{kNames[i], std::to_string(piles[static_cast<std::size_t>(i)])}});
                        balance_rows_.push_back({0, static_cast<std::uint32_t>(3 - i), total});
                    }
                } else {
                    std::string name = c.strings().db_string(b.currency, 0x12);
                    balance_.rows.push_back(ListRow{{name.empty() ? "currency " + std::to_string(b.currency) : name, std::to_string(total)}});
                    balance_rows_.push_back({b.currency, 1, total});
                }
            }
            balance_.selected = balance_sel_;
            return &balance_;
        }
        if (id == "UpkeepList") {
            // The daily cost per currency of the record's upkeep entry and the days each pays for.
            upkeep_list_ = make_list({{"Currency", 140}, {"Daily", 120}, {"Time Left", 90}});
            const net::ReDefinition* d = c.real_estate().definition(r->definition);
            const net::ReCostSet* set = d ? c.real_estate().cost_set(d->upkeep_group) : nullptr;
            if (set)
                for (const auto& e : set->entries) {
                    if (e.key != r->upkeep_set) continue;
                    for (const auto& leaf : e.leaves) {
                        double balance = 0;
                        for (const auto& b : r->escrow)
                            if (b.currency == leaf.currency) balance = b.amount;
                        double days = leaf.amount ? balance / static_cast<double>(leaf.amount) : -1.0;
                        ListRow row{{leaf.currency == 0 ? "Coin" : currency_line(c, leaf.currency, 0).substr(2),
                                     currency_line(c, leaf.currency, leaf.amount), game::re_format_time_left(days)}};
                        if (days == 0) row.cell_colors = {std::nullopt, std::nullopt, kRed};
                        upkeep_list_.rows.push_back(std::move(row));
                    }
                }
            return &upkeep_list_;
        }
        return nullptr;
    }
    bool list_click(EqHost&, std::string_view id, int row, bool, int) override {
        if (id == "MemberList") member_sel_ = row;
        else if (id == "BalanceList") balance_sel_ = row;
        else return false;
        return true;
    }

    const ComboData* combo(EqHost& host, std::string_view id) override {
        const net::ReRecord* r = yard(host);
        if (!r) return nullptr;
        if (id == "UpkeepCombo") {
            const net::ReDefinition* d = host.conn->real_estate().definition(r->definition);
            if (!d) return nullptr;
            fill_cost_combo(*host.conn, d->upkeep_group, upkeep_, upkeep_keys_, r->upkeep_set);
            return &upkeep_;
        }
        if (id == "MemberGroupCombo") {
            group_combo_.choices.clear();
            for (std::uint32_t g = 0; g < net::kReAccessGroups; ++g) group_combo_.choices.push_back(group_name(g));
            const auto& mem = access(host).members;
            group_combo_.selected = member_sel_ >= 0 && member_sel_ < static_cast<int>(mem.size())
                                        ? static_cast<int>(mem[static_cast<std::size_t>(member_sel_)].group)
                                        : -1;
            return &group_combo_;
        }
        return nullptr;
    }
    bool combo_select(EqHost& host, std::string_view id, int index) override {
        if (!yard(host)) return false;
        if (id == "UpkeepCombo") {
            combo(host, id);
            if (index >= 0 && index < static_cast<int>(upkeep_keys_.size()))
                host.conn->re_set_upkeep(upkeep_keys_[static_cast<std::size_t>(index)]);
            return true;
        }
        if (id == "MemberGroupCombo") {
            net::ReAccess a = access(host);
            if (member_sel_ < 0 || member_sel_ >= static_cast<int>(a.members.size()) || index < 0) return true;
            a.members[static_cast<std::size_t>(member_sel_)].group = static_cast<std::uint32_t>(index);
            save(host, a);
            return true;
        }
        return false;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        const net::ReRecord* r = yard(host);
        auto& c = *host.conn;
        if (id.starts_with("Perm_")) {
            int g = 0, b = 0;
            if (!r || std::sscanf(std::string(id).c_str(), "Perm_%d_%d", &g, &b) != 2 || g < 0 || g >= net::kReAccessGroups ||
                b < 0 || b >= net::kReAccessBits)
                return true;
            net::ReAccess a = access(host);
            a.groups[static_cast<std::size_t>(g)].bits ^= 1u << b;
            save(host, a);
            return true;
        }
        if (id == "DepositButton") c.re_escrow(net::ReEscrowAction::Deposit);
        else if (id == "AddDayButton") c.re_escrow(net::ReEscrowAction::AddDay);
        else if (id == "AddWeekButton") c.re_escrow(net::ReEscrowAction::AddWeek);
        else if (id == "WithdrawAllButton") c.re_escrow(net::ReEscrowAction::WithdrawAll);
        else if (id == "WithdrawButton") withdraw(host);
        else if (id == "TransferButton") {
            if (c.re_can_transfer(true)) {
                auto t = c.world().spawns.find(c.target());
                std::string name = t == c.world().spawns.end() ? std::string() : t->second.name;
                ask_confirm(host, eqstr(host, 0x1dd3, {name}), 10000, [&host] { if (host.conn) host.conn->re_transfer(); });
            }
        } else if (id == "AddMemberButton") {
            EqHost::TextEntry e;
            e.prompt = "Name";
            e.min_chars = 1;
            e.max_chars = 64;
            e.done = [this, &host](const std::string& name) {
                if (!host.conn || !yard(host) || name.empty()) return;
                net::ReAccess a = access(host);
                a.members.push_back({name, 1}); // a visitor to start with
                save(host, a);
            };
            host.text_entry = std::move(e);
        } else if (id == "RemoveMemberButton") {
            net::ReAccess a = access(host);
            if (member_sel_ >= 0 && member_sel_ < static_cast<int>(a.members.size())) {
                a.members.erase(a.members.begin() + member_sel_);
                member_sel_ = -1;
                save(host, a);
            }
        } else return false;
        return true;
    }

private:
    struct BalanceRow {
        std::uint32_t currency = 0, coin_type = 0;
        std::uint64_t total = 0; // coin: all of it in copper
    };
    const net::ReRecord* yard(EqHost& host) {
        if (!host.conn) return nullptr;
        return host.conn->real_estate().record(host.conn->real_estate().occupancy().yard);
    }
    net::ReAccess access(EqHost& host) {
        const net::ReAccess* a = host.conn->real_estate().access(host.conn->real_estate().occupancy().yard);
        return a ? *a : net::ReAccess{};
    }
    void save(EqHost& host, const net::ReAccess& a) {
        // The original queues edits and sends them with CRealEstateManageWnd_SendQueuedUpdates @0x747ed0 (0x3146);
        // here each edit goes at once, and the server's 0x485d brings the table back.
        host.conn->re_save_access(host.conn->real_estate().occupancy().yard, a);
    }
    static std::string group_name(std::uint32_t g) {
        // The seven access groups in table order (as read from the live saves).
        static const char* kGroups[] = {"Everyone", "Visitor", "Acquaintance", "Friend", "Trustee", "Co-Owner", "Reserved"};
        return g < 7 ? kGroups[g] : std::to_string(g);
    }
    void withdraw(EqHost& host) {
        // SendEscrowFromCursor's second path: the quantity window (max: the balance in that denomination, at most
        // 1e9), then escrow action 1 {currency, coin type (alternate currencies 1), amount}.
        if (balance_sel_ < 0 || balance_sel_ >= static_cast<int>(balance_rows_.size())) return;
        BalanceRow b = balance_rows_[static_cast<std::size_t>(balance_sel_)];
        std::uint64_t max = b.currency == 0 ? b.total / static_cast<std::uint64_t>(std::pow(10.0, b.coin_type)) : b.total;
        max = std::min<std::uint64_t>(max, 1000000000);
        if (max == 0) return;
        game::QuantityOpen q;
        q.max = static_cast<std::int32_t>(max);
        q.start = static_cast<std::int32_t>(max);
        if (host.click_rect[0] >= 0) {
            q.x = static_cast<std::int32_t>((host.click_rect[0] + host.click_rect[2]) / 2);
            q.y = static_cast<std::int32_t>((host.click_rect[1] + host.click_rect[3]) / 2);
        }
        open_quantity(host, q, [&host, b](std::int32_t n) {
            if (host.conn && n > 0)
                host.conn->re_escrow(net::ReEscrowAction::Withdraw, {{b.currency, b.coin_type, static_cast<std::uint64_t>(n)}});
        });
    }

    ListData members_, balance_, upkeep_list_;
    std::vector<BalanceRow> balance_rows_;
    ComboData upkeep_, group_combo_;
    std::vector<std::uint32_t> upkeep_keys_;
    int member_sel_ = -1, balance_sel_ = -1;
};

// ---------------------------------------------------------------------------------------------------------------
// RealEstateItemsWnd: the item rows of the yard, house and crate (CRealEstateItemsWnd_RefreshItemList @0x736bc0),
// the buttons (WndNotification @0x73cc50) and the item's context menu (ShowItemContextMenu @0x7375f0 and the 0x65 +
// index arms).
class ItemsWindow : public StockWindow {
public:
    ItemsWindow() : StockWindow("RealEstateItemsWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->real_estate_ui().items_open; }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_real_estate_window(GameConnection::ReWindow::Items, show);
    }

    void frame(EqHost& host) override {
        // The server's 0x21ec answer pops the menu up at the click.
        if (!host.conn || !host.conn->real_estate_ui().menu) return;
        net::ReMenu menu = *host.conn->real_estate_ui().menu;
        host.conn->re_close_menu();
        open_menu(host, menu);
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& occ = host.conn->real_estate().occupancy();
        const Row* row = selected();
        bool busy = host.conn->real_estate().pending();
        bool any = row && !busy;
        if (id == "InventoryButton" || id == "InspectButton") out.enabled = any;
        else if (id == "ClosetButton") out.enabled = any && occ.house != kReNone;
        else if (id == "ShedButton") out.enabled = any && occ.yard != kReNone;
        else if (id == "CrateButton") out.enabled = any && occ.crate != kReNone;
        else if (id == "PlaceButton") out.enabled = false; // placement mode is not built
        else if (id == "StowButton" || id == "PackButton" || id == "UnpackButton") out.enabled = !busy && occ.yard != kReNone;
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn || id != "ItemList") return nullptr;
        const auto& m = host.conn->real_estate();
        const auto& occ = m.occupancy();
        // A row's section is the estate it was fed under (H 43.139).
        list_ = make_list({{"", 24}, {"Item", 200}, {"Where", 70}, {"State", 70}});
        rows_.clear();
        auto add = [&](std::uint32_t re, const char* where) {
            if (re == kReNone) return;
            const game::ReItemSummary* s = m.summary(re);
            if (!s) return;
            for (const auto& r : s->rows) {
                ListRow row{{"", r.item.name.empty() ? r.name : r.item.name, where, r.placed ? "Placed" : "Stored"}};
                if (host.textures) row.icon = host.textures->item_icon(r.item.icon);
                list_.rows.push_back(std::move(row));
                rows_.push_back({r.re, r.item_id, r.item.serial, r.item});
            }
        };
        add(occ.yard, "Yard");
        add(occ.house, "House");
        add(occ.crate, "Crate");
        list_.selected = -1;
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].re == sel_re_ && rows_[i].item_id == sel_item_) list_.selected = static_cast<int>(i);
        return &list_;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool right, int) override {
        if (id != "ItemList") return false;
        if (row < 0 || row >= static_cast<int>(rows_.size())) return true;
        const Row& r = rows_[static_cast<std::size_t>(row)];
        sel_re_ = r.re;
        sel_item_ = r.item_id;
        // WndNotification 3 on the list: the 0x21ec request with the item's key, ids and the mouse.
        if (right && host.conn) {
            ImVec2 mp = ImGui::GetIO().MousePos;
            host.conn->re_request_menu(r.key, r.re, r.item_id, static_cast<std::int32_t>(mp.x), static_cast<std::int32_t>(mp.y),
                                       r.item.id);
        }
        return true;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        const auto& occ = c.real_estate().occupancy();
        const Row* r = selected();
        if (id == "InventoryButton") {
            if (r) c.re_to_inventory(r->key, r->re, r->item_id);
        } else if (id == "ClosetButton" || id == "ShedButton" || id == "CrateButton") {
            // The destination: the container index of the estate the button names (2 house, 3 yard, 6 crate).
            std::uint32_t dest = id == "ClosetButton" ? 2 : id == "ShedButton" ? 3 : 6;
            if (r) c.re_to_storage(r->key, r->re, r->item_id, dest);
        } else if (id == "InspectButton") {
            // Type 7's arm: an item the client has shows at once, else 0x58c7 asks for it.
            if (r && r->item.id != 0 && host.inspect_item) host.inspect_item(r->item);
            else if (r) c.re_inspect(r->re, r->item_id);
        } else if (id == "StowButton") {
            // CRealEstateItemsWnd_DialogResponse 100: 0x6917 (the dialog's own text is not traced: this client's).
            ask_confirm(host, "Move every placed item into storage?", 0, [&host] { if (host.conn) host.conn->re_store_all(); });
        } else if (id == "PackButton" || id == "UnpackButton") {
            // Responses 101 / 102: 0x613d {1 pack, 0 unpack}.
            bool pack = id == "PackButton";
            ask_confirm(host, pack ? "Pack the moving crate?" : "Unpack the moving crate?", 0,
                        [&host, pack] { if (host.conn) host.conn->re_pack_crate(pack); });
        } else return false;
        (void)occ;
        return true;
    }

    // The menu's rows (exposed for tests): ShowItemContextMenu's, the item's name first.
    static std::vector<EqHost::MenuRow> menu_rows(EqHost& host, const net::ReMenu& menu) {
        std::vector<EqHost::MenuRow> rows;
        EqHost::MenuRow title;
        title.text = menu.name;
        title.rgb = 0x00ff00;
        title.enabled = false;
        rows.push_back(std::move(title));
        auto text_of = [&](const net::ReMenu::Entry& e) {
            return e.type == 9 || e.type == 0xa ? host.conn->strings().db_string(e.text, 0x1c) : eqstr(host, e.text);
        };
        auto act_of = [&host, menu](const net::ReMenu::Entry& e) { return [&host, menu, e] { choose(host, menu, e); }; };
        for (const auto& e : menu.entries) {
            if (menu.restricted && ((e.type == 2 && e.param == 1) || e.type == 3 || e.type == 4 || e.type == 0xb || e.type == 0xe))
                continue;
            if (e.type == 9) continue; // under its sub-menu
            EqHost::MenuRow row;
            row.text = text_of(e);
            row.enabled = e.type != 0x12 && e.type != 2; // 0x12 disabled; 2 needs placement mode
            if (e.type == 0xa) {
                std::vector<EqHost::MenuRow> sub;
                for (const auto& s : menu.entries)
                    if (s.type == 9 && s.sub == e.param) {
                        EqHost::MenuRow r;
                        r.text = text_of(s);
                        r.act = act_of(s);
                        sub.push_back(std::move(r));
                    }
                row.submenu = [sub] { return sub; };
            } else {
                row.act = act_of(e);
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    // The 0x65 + index arm for the entry's type (CRealEstateItemsWnd::WndNotification).
    static void choose(EqHost& host, const net::ReMenu& m, const net::ReMenu::Entry& e) {
        if (!host.conn) return;
        auto& c = *host.conn;
        bool busy = c.real_estate().pending();
        switch (e.type) {
        case 0: c.set_real_estate_window(GameConnection::ReWindow::Manage, !c.real_estate_ui().manage_open); break;
        case 1: c.re_travel(m.re, m.item); break; // 0x218c {re, item, 0}: a house's door
        case 3:
            if (!busy) c.re_to_storage(m.key, m.re, m.item, net::re_container_index(c.real_estate().record_type(m.re)));
            break;
        case 4:
            if (!busy) c.re_to_inventory(m.key, m.re, m.item);
            break;
        case 5:
            if (!busy) c.re_item_op(net::kOpReMenu5, m.re, m.item, 0);
            break;
        case 7: c.re_inspect(m.re, m.item); break;
        case 8: c.re_item_op(net::kOpReMenu8, m.re, m.item, static_cast<std::uint32_t>(e.param)); break;
        case 9: c.re_item_op(net::kOpReMenu9, m.re, m.item, static_cast<std::uint32_t>(e.param)); break;
        case 0xb: c.set_real_estate_window(GameConnection::ReWindow::Items, !c.real_estate_ui().items_open); break;
        case 0x13: c.re_item_op(net::kOpReMenu13, m.re, m.item, static_cast<std::uint32_t>(e.param)); break;
        // 2 (placement), 0xc / 0xd (the item's spawn as target), 0xe (window 0x4b) and 0x11 (dialog 0x67, eqstr
        // 0x14d9) are not built.
        default: break;
        }
    }

private:
    struct Row {
        std::uint32_t re = 0, item_id = 0;
        std::string key;
        net::Item item;
    };
    const Row* selected() const {
        for (const auto& r : rows_)
            if (r.re == sel_re_ && r.item_id == sel_item_) return &r;
        return nullptr;
    }
    static void open_menu(EqHost& host, const net::ReMenu& menu) {
        // Pops up at the point the request sent (+0x2f0 / +0x2f4).
        host.open_menu(static_cast<float>(menu.x), static_cast<float>(menu.y), menu_rows(host, menu));
    }

    ListData list_;
    std::vector<Row> rows_;
    std::uint32_t sel_re_ = kReNone, sel_item_ = kReNone;
};

// ---------------------------------------------------------------------------------------------------------------
// RealEstateNeighborhoodWnd: the lists ReadNeighborhoodList @0x74ab50 fills, WndNotification @0x74a3a0.
class NeighborhoodWindow : public StockWindow {
public:
    NeighborhoodWindow() : StockWindow("RealEstateNeighborhoodWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->real_estate_ui().neighborhood_open; }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_real_estate_window(GameConnection::ReWindow::Neighborhood, show);
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        if (id == "GoButton") out.enabled = selected(host) != kReNone;
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& ui = host.conn->real_estate_ui();
        if (id == "NeighborhoodList") {
            hoods_ = make_list({{"Neighborhood", 180}, {"Guild", 120}, {"Player Plots", 80}, {"Guild Plots", 80}});
            for (const auto& r : ui.neighborhoods)
                hoods_.rows.push_back(ListRow{{r.name, guild_name(*host.conn, r.guild_id),
                                               std::to_string(r.player_used) + "/" + std::to_string(r.player_plots),
                                               std::to_string(r.guild_used) + "/" + std::to_string(r.guild_plots)}});
            hoods_.selected = -1;
            for (std::size_t i = 0; i < ui.neighborhoods.size(); ++i)
                if (ui.neighborhoods[i].id == sel_) hoods_.selected = static_cast<int>(i);
            return &hoods_;
        }
        if (id == "PlayerList") {
            players_ = make_list({{"Player", 160}});
            for (const auto& p : ui.neighborhood_players) players_.rows.push_back(ListRow{{p}});
            return &players_;
        }
        if (id == "GuildList") {
            guilds_ = make_list({{"Guild", 160}});
            for (auto g : ui.neighborhood_guilds) guilds_.rows.push_back(ListRow{{guild_name(*host.conn, g)}});
            return &guilds_;
        }
        return nullptr;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool, int) override {
        if (id != "NeighborhoodList") return id == "PlayerList" || id == "GuildList";
        const auto& rows = host.conn->real_estate_ui().neighborhoods;
        if (row < 0 || row >= static_cast<int>(rows.size())) return true;
        // A row selected: 0x2691 subtype 2 with its id (the members' lists come back as kind 2).
        sel_ = rows[static_cast<std::size_t>(row)].id;
        host.conn->re_search(2, sel_);
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        if (id == "GoButton") {
            // 0x218c {the neighborhood, -1}.
            if (selected(host) != kReNone) c.re_travel(selected(host), kReNone);
        } else if (id == "MyPlotsButton") c.re_search(0, c.real_estate().occupancy().root);
        else if (id == "MyItemsButton") c.re_search(1, c.real_estate().occupancy().root);
        else return false;
        return true;
    }

private:
    std::uint32_t selected(EqHost& host) const {
        // The row picked, else the list's header id (+0x22c).
        if (sel_ != kReNone) return sel_;
        return host.conn ? host.conn->real_estate_ui().neighborhood_header : kReNone;
    }
    ListData hoods_, players_, guilds_;
    std::uint32_t sel_ = kReNone;
};

// ---------------------------------------------------------------------------------------------------------------
// RealEstatePlotSearchWnd: the name search (0x74b920), the PlotList ReadSearchResults @0x74c4c0 fills, and Go
// (WndNotification @0x74c110: with mode +0x24c set 0x796c, else /find kind 4).
class PlotSearchWindow : public StockWindow {
public:
    PlotSearchWindow() : StockWindow("RealEstatePlotSearchWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->real_estate_ui().plot_search_open; }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_real_estate_window(GameConnection::ReWindow::PlotSearch, show);
    }
    std::string title(EqHost& host) override { return host.conn ? host.conn->real_estate_ui().plot_search_title : std::string(); }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        if (id == "NameEdit") out.text = name_;
        else if (id == "GoButton") out.enabled = selected(host) != nullptr;
    }
    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        if (id != "NameEdit") return false;
        name_ = text.substr(0, 63);
        return true;
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn || id != "PlotList") return nullptr;
        list_ = make_list({{"", 24}, {"Owner", 140}, {"Type", 60}, {"Address", 200}});
        for (const auto& p : host.conn->real_estate_ui().plots) {
            // The icon: items.icon from 500 (IconCache::GetIcon).
            ListRow row{{"", p.owner, p.type == ReType::GuildPlot ? "Guild" : "Player", p.address}};
            if (host.textures && p.icon >= 500) row.icon = host.textures->item_icon(p.icon);
            list_.rows.push_back(std::move(row));
        }
        list_.selected = -1;
        const auto& plots = host.conn->real_estate_ui().plots;
        for (std::size_t i = 0; i < plots.size(); ++i)
            if (plots[i].re == sel_) list_.selected = static_cast<int>(i);
        return &list_;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool, int) override {
        if (id != "PlotList") return false;
        const auto& plots = host.conn->real_estate_ui().plots;
        if (row >= 0 && row < static_cast<int>(plots.size())) sel_ = plots[static_cast<std::size_t>(row)].re;
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        if (id == "SearchButton") {
            // CRealEstatePlotSearchWnd_SendSearchByName: {4, the root, 0}, the name at +0x5c, flag 1.
            c.re_search(4, c.real_estate().occupancy().root, {}, name_, {}, 1);
        } else if (id == "GoButton") {
            const net::RePlotRow* p = selected(host);
            if (!p) return true;
            if (c.real_estate_ui().plot_search_mode != 0) c.re_plot_select(p->re);
            else c.find_real_estate_plot(p->re);
        } else return false;
        return true;
    }

private:
    const net::RePlotRow* selected(EqHost& host) const {
        for (const auto& p : host.conn->real_estate_ui().plots)
            if (p.re == sel_) return &p;
        return nullptr;
    }
    ListData list_;
    std::string name_;
    std::uint32_t sel_ = kReNone;
};

} // namespace

void add_real_estate_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<PurchaseWindow>());
    out.push_back(std::make_unique<ManageWindow>());
    out.push_back(std::make_unique<ItemsWindow>());
    out.push_back(std::make_unique<NeighborhoodWindow>());
    out.push_back(std::make_unique<PlotSearchWindow>());
}

std::vector<EqHost::MenuRow> real_estate_menu_rows(EqHost& host, const net::ReMenu& menu) {
    return ItemsWindow::menu_rows(host, menu);
}

} // namespace hailstone::app
