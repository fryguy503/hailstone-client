// GameConnection: real estate (player housing, guild halls, placed objects) as RealEstateManagerClient and the
// CRealEstate*Wnd windows handle it (the functions named
// below). The state is game::RealEstateManager; the packets net/real_estate_packets.h. A stock EQEmu server sends
// none of these; a server with housing (Project Triune) sends them with the original's numbers.

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

#include "core/log.h"
#include "game/coords.h"
#include "game/guild_ranks.h"
#include "game/slash_commands.h"
#include "net/game_connection.h"

namespace hailstone::net {

namespace {

using game::kReNone;

constexpr std::uint32_t kConfirmRealEstateOffer = 0x89; // CEverQuest's dialog 0x89 (the 0x5a63 offer)
constexpr std::int32_t kGuildPermBuyPlot = 0x1b, kGuildPermSellPlot = 0x1c;
constexpr std::uint32_t kExpansionPlayerPlot = 0x10000, kExpansionGuildPlot = 0x20000;
constexpr std::uint32_t kGuildPlotMask = 0x80, kNeighborhoodMask = 0x10; // 1 << (8 - 1), 1 << (5 - 1)

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

} // namespace

bool GameConnection::handle_real_estate(const AppPacket& p) {
    game::RealEstateManager& m = real_estate_;
    auto bad = [&p] {
        log::warn("real estate: a bad 0x{:04x} ({} bytes) ignored", p.opcode, p.data.size());
        return true;
    };
    auto changed = [this] { ++re_ui_.changes; };
    switch (p.opcode) {
    case kOpReNop1:
    case kOpReNop2: return true;
    case kOpReCatalog: {
        // 0x2698: ReadDefsFromPacket, ReadCostsFromPacket, 0x5aaad0.
        auto v = decode_re_catalog(p);
        if (!v) return bad();
        m.set_catalog(std::move(*v));
        changed();
        return true;
    }
    case kOpReRecord:
    case kOpReSetCurrent: {
        auto v = decode_re_record(p);
        if (!v) return bad();
        std::uint32_t id = v->id;
        if (p.opcode == kOpReSetCurrent) m.set_current(std::move(*v));
        else m.add_record(std::move(*v));
        // 0x63fd refreshes an open plot window on that record (CRealEstatePurchaseWnd_SetPlotInfo).
        if (p.opcode == kOpReRecord && re_ui_.purchase_open && re_ui_.purchase_re == id) re_show_plot(id);
        changed();
        return true;
    }
    case kOpReRemove: {
        auto v = decode_re_record(p);
        if (!v) return bad();
        m.remove_record(v->id);
        changed();
        return true;
    }
    case kOpReAccess:
    case kOpReAccessUpdate: {
        auto v = decode_re_access(p);
        if (!v) return bad();
        m.set_access(v->re, std::move(v->access));
        changed();
        return true;
    }
    case kOpReItemSummary:
    case kOpReItemRows:
    case kOpReItemRowAdd: {
        auto v = decode_re_item_list(p);
        if (!v) return bad();
        m.add_item_rows(*v);
        changed();
        return true;
    }
    case kOpReItemSummaryUpdate:
        // 0x5afe00 with 0x1097: u32 id, then 0x5ab4b0's update of a known summary (not decoded here; the
        // rows themselves come again on 0x1cef / 0x2cf7).
        changed();
        return true;
    case kOpReItemSummaryRename: {
        auto v = decode_re_rename(p);
        if (!v) return bad();
        m.rename_summary(v->re, std::move(v->name));
        changed();
        return true;
    }
    case kOpReItemRemoved: {
        auto v = decode_re_item_removed(p);
        if (!v) return bad();
        m.remove_item(v->re, v->item_id);
        changed();
        return true;
    }
    case kOpReItemSummaryForget: {
        auto v = decode_re_dword(p);
        if (!v) return bad();
        m.forget_summary(static_cast<std::uint32_t>(*v));
        changed();
        return true;
    }
    case kOpReResult: {
        // RealEstate::ShowResultCode @0x5ae1f0: its line in chat colour 13; any answer releases the latch.
        auto v = decode_re_dword(p);
        if (!v) return bad();
        if (std::uint32_t msg = game::re_result_message(*v)) system(msg, {}, 0xd);
        // Code 0 commits a placement session (ItemPlacement_Stop(1, 1); the object itself comes on 0x1045).
        if (*v == 0 && placement_.active()) placement_.stop();
        m.clear_pending();
        changed();
        return true;
    }
    case kOpReSellResult:
    case kOpReTransferResult: {
        auto v = decode_re_dword(p);
        if (!v) return bad();
        std::uint32_t msg = p.opcode == kOpReSellResult ? game::re_sell_message(*v) : game::re_transfer_message(*v);
        if (msg) system(msg, {}, 0xd);
        changed();
        return true;
    }
    case kOpReSearch: {
        auto v = decode_re_search_reply(p);
        if (!v) return bad();
        switch (v->kind) {
        case ReSearchReply::PlotList:
            // ReadSearchResults: only while the plot search window exists (here: always), the title and the rows.
            re_ui_.plot_search_title = std::move(v->title);
            re_ui_.plots = std::move(v->plots);
            break;
        case ReSearchReply::Neighborhoods:
            // ReadNeighborhoodList kinds 0 / 1: the lists cleared, the selection lost (+0x228 = -1), the header,
            // and with the show byte the window shown.
            re_ui_.neighborhoods = std::move(v->neighborhoods);
            re_ui_.neighborhood_selected = kReNone;
            if (!re_ui_.neighborhoods.empty()) re_ui_.neighborhood_header = v->header_id;
            if (v->show) re_ui_.neighborhood_open = true;
            break;
        case ReSearchReply::Members:
            re_ui_.neighborhood_players = std::move(v->players);
            re_ui_.neighborhood_guilds = std::move(v->guilds);
            break;
        case ReSearchReply::Record:
            m.merge_search_record(v->record);
            if (v->auto_open) re_show_plot(v->record.id);
            break;
        case ReSearchReply::Ignored: break;
        }
        changed();
        return true;
    }
    case kOpRePurchase:
        // u8: non-zero shows the manage window (if it exists: always here).
        if (!p.data.empty() && p.data[0] != std::byte{0}) re_ui_.manage_open = true;
        changed();
        return true;
    case kOpReEscrow: {
        // RealEstate::EscrowParse @0x741670 (only while the manage window exists: always here). With the u8 at
        // +0x... clear, action 0 (else, set, action 1) and an entry: the first entry, a coin (currency 0), sets the
        // cursor's coin of its type (0 copper .. 3 platinum, the profile's +0x33d4 .. +0x33c8), so a withdrawal
        // arrives on the cursor. Action 2 (with the u8 clear) refreshes the upkeep combo only.
        auto v = decode_re_escrow(p);
        if (!v) return bad();
        bool coin = (v->b == 0 && v->action == 0) || (v->b != 0 && v->action == 1);
        if (coin && !v->entries.empty() && v->entries.front().currency == 0 && v->entries.front().coin_type < 4)
            cursor_coins_[v->entries.front().coin_type] = static_cast<std::uint32_t>(v->entries.front().amount);
        changed();
        return true;
    }
    case kOpReOwnedA:
    case kOpReOwnedB:
    case kOpReOwnedC: {
        auto v = decode_re_id_list(p);
        if (!v) return bad();
        auto list = p.opcode == kOpReOwnedA   ? game::RealEstateManager::OwnedList::A
                    : p.opcode == kOpReOwnedB ? game::RealEstateManager::OwnedList::B
                                              : game::RealEstateManager::OwnedList::C;
        m.set_owned(list, *v);
        changed();
        return true;
    }
    case kOpReOwnedAToggle:
    case kOpReOwnedBToggle: {
        auto v = decode_re_list_toggle(p);
        if (!v) return bad();
        m.toggle_owned(p.opcode == kOpReOwnedAToggle ? game::RealEstateManager::OwnedList::A
                                                     : game::RealEstateManager::OwnedList::B,
                       v->id, v->add);
        changed();
        return true;
    }
    case kOpReNpcItem: {
        // 0x5996e0 then +0x18 → spawn +0x198 (its estate), +0x1c → spawn +0x420 (its item): the creature of a
        // placed NPC item (a merchant, a pet), which RMouseUp then treats as the item. The binding of a placement
        // session whose guid this is (ItemPlacement +0x24 / +0x28) is not built (docs/unknowns.md).
        auto v = decode_re_npc_item(p);
        if (!v) return bad();
        re_npc_items_[v->spawn_id] = {v->f18, v->f1c};
        return true;
    }
    case kOpReContextMenu: {
        // CRealEstateItemsWnd_ShowItemContextMenu @0x7375f0 (only while the items window exists: always here).
        auto v = decode_re_menu(p);
        if (!v) return bad();
        re_ui_.menu = std::move(*v);
        changed();
        return true;
    }
    case kOpReInspect: {
        // ItemClient::CreateItemClient then CItemDisplayManager::ShowItem.
        auto v = decode_re_inspect(p);
        if (!v) return bad();
        item_displays_.push_back(std::move(*v));
        return true;
    }
    case kOpReOffer: {
        // 0x5ad760 keeps the 0xbc-byte body; eqstr 7636 "Do you wish to accept %1 from %2?" (%1 the estate's
        // name, else "this real estate"; %2 the text at +0x0c) for 5 minutes. The answer is
        // ConfirmationDialog_HandleResponse's 0x89.
        auto v = decode_re_transaction(p);
        if (!v) return bad();
        std::string what = "this real estate";
        if (const ReRecord* r = m.record(v->re)) what = r->name;
        Confirmation c;
        c.id = kConfirmRealEstateOffer;
        std::vector<std::string> args{what, v->text0c};
        c.text = strings_.format(0x1dd4, args);
        c.offer = p;
        c.timeout_ms = 300000;
        ask(std::move(c));
        return true;
    }
    case kOpRePlacedItem: {
        // HandleWorldMessage 0x1045: a new key adds (EQPlacedItemManager::Add, the actor built); a known one takes
        // the data (EQPlacedItem_CopyFrom). NPC items always without collision (H 43.154).
        auto v = decode_re_placed_item(p);
        if (!v) return bad();
        if (v->npc_item) v->no_collision = true;
        m.add_placed(*v);
        changed();
        return true;
    }
    case kOpRePlacedItemRemove: {
        auto v = decode_re_placed_item_remove(p);
        if (!v) return bad();
        m.remove_placed(v->re, v->item_id);
        changed();
        return true;
    }
    case kOpReAreas: {
        auto v = decode_re_areas(p);
        if (!v) return bad();
        m.set_areas(std::move(*v));
        changed();
        return true;
    }
    case kOpRePlotSearchOpen: {
        // HandleWorldMessage 0x2b35: the mode byte into the window (+0x24c), then it toggles.
        auto v = decode_re_dword(p);
        if (!v) return bad();
        re_ui_.plot_search_mode = static_cast<std::uint8_t>(*v);
        set_real_estate_window(ReWindow::PlotSearch, !re_ui_.plot_search_open);
        return true;
    }
    case kOpRePlace: {
        // HandleWorldMessage @0x4d058a: the object, its index, the flags and the area codes, the latch released
        // (0x5adb60), then BeginPlacement.
        auto v = decode_re_place_reply(p);
        if (!v) return bad();
        m.clear_pending();
        placement_.begin(*v);
        changed();
        return true;
    }
    case kOpRePlaceStop:
        // HandleWorldMessage 0x6213: ItemPlacement_Stop.
        placement_.stop();
        changed();
        return true;
    default: return false;
    }
}

void GameConnection::real_estate_tick() {
    // RealEstateManagerClient_Process, from the main loop.
    const game::Spawn* me = world_.player();
    real_estate_.process(static_cast<std::uint32_t>(now_ms()), me != nullptr, game::world_to_server(position_));
}

void GameConnection::reset_real_estate_for_zone() {
    // The records, catalog, areas and placed objects come again on arrival (the bring-up order); the character's
    // owned lists are the profile's and stay. Where the original resets its manager (0x5b1cf0's callers) is not
    // traced (docs/unknowns.md).
    auto lists = std::array{real_estate_.owned(game::RealEstateManager::OwnedList::A),
                            real_estate_.owned(game::RealEstateManager::OwnedList::B),
                            real_estate_.owned(game::RealEstateManager::OwnedList::C)};
    real_estate_.reset();
    placement_.stop();
    re_npc_items_.clear();
    real_estate_.set_owned(game::RealEstateManager::OwnedList::A, lists[0]);
    real_estate_.set_owned(game::RealEstateManager::OwnedList::B, lists[1]);
    real_estate_.set_owned(game::RealEstateManager::OwnedList::C, lists[2]);
    re_ui_ = RealEstateUi{};
}

void GameConnection::set_real_estate_window(ReWindow w, bool open) {
    switch (w) {
    case ReWindow::Items: re_ui_.items_open = open; break;
    case ReWindow::Manage:
        // CRealEstateManageWnd::AboutToShow @0x747e50: the yard must be a known record, else eqstr 727.
        if (open && !real_estate_.record(real_estate_.occupancy().yard)) {
            system(0x2d7, {}, 0xd);
            return;
        }
        re_ui_.manage_open = open;
        break;
    case ReWindow::Neighborhood: re_ui_.neighborhood_open = open; break;
    case ReWindow::PlotSearch:
        // CRealEstatePlotSearchWnd::AboutToShow @0x74bb00: only in a neighborhood (the root's type group 5), else
        // eqstr 8078; showing it sends the empty name search (0x74ba70 → 0x74b920("", 1)).
        if (open) {
            if (!game::re_is_neighborhood_type(real_estate_.occupancy().root_type)) {
                system(0x1f8e, {}, 0xd);
                return;
            }
            re_search(4, real_estate_.occupancy().root, {}, {}, {}, 1);
        }
        re_ui_.plot_search_open = open;
        break;
    case ReWindow::Purchase: re_ui_.purchase_open = open; break;
    }
    ++re_ui_.changes;
}

void GameConnection::real_estate_command(std::string_view args) {
    // Cmd_realestate @0x4e4e60: the first word (case-insensitive) toggles items / manage / plot; "trophies" toggles
    // the trophy window (none here); anything else prints eqstr 7511 in colour 15.
    std::string_view rest = args;
    std::string word = game::next_cmd_arg(rest);
    if (iequals(word, "items")) return set_real_estate_window(ReWindow::Items, !re_ui_.items_open);
    if (iequals(word, "manage")) return set_real_estate_window(ReWindow::Manage, !re_ui_.manage_open);
    if (iequals(word, "plot")) return set_real_estate_window(ReWindow::PlotSearch, !re_ui_.plot_search_open);
    if (iequals(word, "trophies")) return;
    system(0x1d57, {}, 0xf);
}

void GameConnection::re_search(std::uint32_t subtype, std::uint32_t id, std::string_view s0, std::string_view s1,
                               std::string_view s2, std::uint8_t flag) {
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode_re_search(subtype, id, s0, s1, s2, flag));
}

void GameConnection::re_travel(std::uint32_t id, std::uint32_t secondary) {
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode_re_travel(id, secondary));
}

void GameConnection::re_plot_select(std::uint32_t plot) {
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode_re_plot_select(plot));
}

bool GameConnection::re_can_purchase(std::uint32_t re, bool verbose) {
    // RealEstate_CanPurchasePlot @0x5b1890.
    const game::RealEstateManager& m = real_estate_;
    if (!profile_ || m.pending()) return false;
    const ReRecord* r = m.record(re);
    if (!r) return false;
    ReType type = m.definition_type(r->definition);
    if (!game::re_is_owner_type(type)) return false;
    // 0x5ad900: the PC's vt+0x90, read here as the GM flag (docs/unknowns.md).
    if (player_is_gm()) return true;
    auto say = [&](std::uint32_t id) {
        if (verbose) system(id, {}, 0xd);
        return false;
    };
    // 0x5b5b60: under a guild plot the account needs 0x20000, else 0x10000 (the PC's vt+0x5c: the expansions).
    // Both walks up go through find_up, at most 10 steps as the manager's others (FindChildAtPosition): a parent
    // cycle from the server (A under B under A) would otherwise loop for ever. Whether 0x5b5b60 / 0x5b5130
    // bound their own walks is not traced (docs/unknowns.md); a chain of at most 10 is walked the same.
    const bool guild_area = m.find_up(r, kGuildPlotMask) != nullptr;
    if (!guild_area && !(world_expansions_ & kExpansionPlayerPlot)) return say(0x14c3);
    if (guild_area && !(world_expansions_ & kExpansionGuildPlot)) return say(0x1536);
    // 0x5b5b90 (a guild plot under feature restriction 10) prints a line 0x7dbc50 builds: not traced; such
    // accounts are refused by the server instead.
    if (m.already_owned(re)) return say(0x191b);
    // 0x5b5130: the nearest neighborhood's guild, when it has one, must be the player's (5317).
    if (const ReRecord* up = m.find_up(r, kNeighborhoodMask))
        if (up->guild_id != -1 && static_cast<std::uint32_t>(up->guild_id) != guild_.guild_id) return say(0x14c5);
    if (type != ReType::GuildPlot) return true;
    if (guild_.guild_id == 0xffffffffu) return say(0x2f3);
    if (has_guild_permission(kGuildPermBuyPlot)) return true;
    return say(0x2f2);
}

void GameConnection::re_purchase(std::uint32_t price_key, std::uint32_t upkeep_key) {
    // CRealEstatePurchaseWnd's Purchase: 0x45be {the plot, the price and upkeep combos' keys}; the window hides.
    if (state_ != ConnectionState::InZone || !re_can_purchase(re_ui_.purchase_re, true)) return;
    send_zone(encode_re_purchase(re_ui_.purchase_re, price_key, upkeep_key));
    real_estate_.set_pending(static_cast<std::uint32_t>(now_ms()));
    set_real_estate_window(ReWindow::Purchase, false);
}

bool GameConnection::re_can_sell(bool verbose) {
    // RealEstate_CanSellCurrentPlot @0x5b3430 on the current estate, 0x5b7fa0's checks as far as they are
    // traced: a player plot must be the player's (2), a guild plot needs 0x20000 (8) and the player's guild (9);
    // then a guild plot needs guild permission 28 (0x2f5). Its other refusals (3, 5, 6, 7, 10..13) are the server's.
    const game::RealEstateManager& m = real_estate_;
    if (!profile_ || m.pending()) return false;
    const ReRecord* r = m.record(m.occupancy().current);
    if (!r) return false;
    if (player_is_gm()) return true;
    ReType type = m.definition_type(r->definition);
    std::int32_t code = 0;
    if (type == ReType::Plot) {
        if (!iequals(r->owner, character_)) code = 2;
    } else if (type == ReType::GuildPlot) {
        if (!(world_expansions_ & kExpansionGuildPlot)) code = 8;
        else if (static_cast<std::uint32_t>(r->guild_id) != guild_.guild_id) code = 9;
    } else {
        code = 1;
    }
    if (code != 0) {
        if (verbose) system(game::re_sell_message(code), {}, 0xd);
        return false;
    }
    if (type != ReType::GuildPlot || has_guild_permission(kGuildPermSellPlot)) return true;
    if (verbose) system(0x2f5, {}, 0xd);
    return false;
}

void GameConnection::re_sell() {
    // The confirm (eqstr 7634) is the window's; Yes sends 0x792c for the current estate.
    if (state_ != ConnectionState::InZone || !re_can_sell(true)) return;
    send_zone(encode_re_sell(real_estate_.occupancy().current));
}

bool GameConnection::re_can_transfer(bool verbose) {
    // RealEstate_CanTransferPlotToTarget @0x5b1380: a current estate and a player target (eqstr 3278 otherwise);
    // 0x5b50b0: a player plot the player owns (2 if not; anything else 1).
    const game::RealEstateManager& m = real_estate_;
    if (!world_.player() || m.pending()) return false;
    const ReRecord* r = m.record(m.occupancy().current);
    if (!r) return false;
    auto t = world_.spawns.find(world_.target_id);
    if (t == world_.spawns.end() || t->second.is_npc || t->second.id == world_.player_id) {
        if (verbose) system(0xcce, {}, 0xd);
        return false;
    }
    std::int32_t code = 1;
    if (m.definition_type(r->definition) == ReType::Plot) code = player_is_gm() || iequals(r->owner, character_) ? 0 : 2;
    if (code == 0) return true;
    if (verbose) system(game::re_transfer_message(code), {}, 0xd);
    return false;
}

void GameConnection::re_transfer(std::uint32_t target, const std::string& name) {
    // RealEstate_SendTransferPlot @0x5b1470: 0x507f {the current estate, the target's name at +0x6c}.
    if (state_ != ConnectionState::InZone || !re_can_transfer(true)) return;
    auto t = world_.spawns.find(world_.target_id);
    if (t == world_.spawns.end()) return;
    // Hailstone: only to the player the question named. The original reads the target at the Yes, so a target
    // changed while the question was up (10 s) would get the plot instead.
    if (world_.target_id != target || t->second.name != name) {
        log::info("real estate: the target changed while the transfer was asked; nothing sent");
        return;
    }
    send_zone(encode_re_transfer(real_estate_.occupancy().current, t->second.name));
}

void GameConnection::re_opt_out(bool opt_out) {
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode_re_opt_out(real_estate_.occupancy().yard, opt_out));
}

void GameConnection::re_escrow(ReEscrowAction action, std::vector<ReEscrow::Entry> entries) {
    // CRealEstateManageWnd::SendEscrowFromCursor @0x741ee0: for the yard, and only an owned one (0x5b41d0). A
    // deposit with no entries given takes the cursor's coin (one denomination, platinum first) and empties it.
    if (state_ != ConnectionState::InZone) return;
    std::uint32_t yard = real_estate_.occupancy().yard;
    if (!real_estate_.already_owned(yard)) return;
    if (action == ReEscrowAction::Deposit && entries.empty()) {
        for (int type = 3; type >= 0; --type) {
            auto& held = cursor_coins_[static_cast<std::size_t>(type)];
            if (held == 0) continue;
            entries.push_back({0, static_cast<std::uint32_t>(type), held});
            held = 0;
            break;
        }
        if (entries.empty()) return;
    }
    send_zone(encode_re_escrow(action, yard, std::move(entries)));
}

void GameConnection::re_save_access(std::uint32_t re, const ReAccess& access) {
    // CRealEstateManageWnd_SendQueuedUpdates @0x747ed0: 0x3146 per edited estate.
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode_re_access_save(re, access));
}

void GameConnection::re_to_storage(std::string_view key, std::uint32_t re, std::uint32_t item, std::uint32_t destination) {
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode_re_to_storage(key, re, item, destination));
}

void GameConnection::re_to_inventory(std::string_view key, std::uint32_t re, std::uint32_t item) {
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode_re_to_inventory(key, re, item));
}

void GameConnection::re_set_upkeep(std::uint32_t key) {
    if (state_ != ConnectionState::InZone) return;
    ReEscrow v;
    v.action = static_cast<std::uint32_t>(ReEscrowAction::SetUpkeep);
    v.re = real_estate_.occupancy().yard;
    v.f8 = key;
    send_zone(encode(v));
}

void GameConnection::re_item_op(std::uint16_t op, std::uint32_t re, std::uint32_t item, std::uint32_t param) {
    if (state_ != ConnectionState::InZone) return;
    send_zone(op == kOpReMenu5 ? encode_re_menu5(re, item) : encode_re_item_op(op, re, item, param));
}

void GameConnection::re_store_all() {
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode_re_store_all());
}

void GameConnection::re_pack_crate(bool pack) {
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode_re_pack_crate(pack));
}

void GameConnection::re_inspect(std::uint32_t re, std::uint32_t item) {
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode_re_inspect(re, item));
}

void GameConnection::re_request_menu(std::string_view key, std::uint32_t re, std::uint32_t item, std::int32_t x,
                                     std::int32_t y, std::uint32_t f2f8) {
    if (state_ != ConnectionState::InZone) return;
    send_zone(encode_re_menu_request(key, re, item, x, y, f2f8));
}

bool GameConnection::re_request_placed_menu(std::uint32_t re, std::uint32_t item, const glm::vec3& actor,
                                            float actor_radius, std::int32_t x, std::int32_t y) {
    // CEverQuest::RMouseUp (not while placing): the player's +0x64.. and height; 0x542a90's body.
    const game::Spawn* me = world_.player();
    if (!me || placement_.active() || state_ != ConnectionState::InZone) return false;
    if (!game::placed_menu_in_reach(actor, actor_radius, position_, me->heights.height, re, real_estate_.occupancy().current))
        return false;
    send_zone(encode_re_menu_request(re, item, x, y));
    return true;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> GameConnection::re_npc_item(std::uint32_t spawn_id) const {
    auto it = re_npc_items_.find(spawn_id);
    if (it == re_npc_items_.end() || it->second.first == kReNone) return std::nullopt;
    return it->second;
}

bool GameConnection::re_request_npc_item_menu(std::uint32_t spawn_id, std::int32_t x, std::int32_t y) {
    // RMouseUp's ClickedPlayer arm: a spawn with +0x198 != -1, within 60 or its estate the current one; the menu
    // unless (no Shift and) the spawn's own right click applies within 20 (0x59eee0 / 0x59ef30, not traced:
    // here the menu always).
    auto ids = re_npc_item(spawn_id);
    auto sp = world_.spawns.find(spawn_id);
    if (!ids || sp == world_.spawns.end() || state_ != ConnectionState::InZone) return false;
    if (!game::npc_item_menu_in_reach(sp->second.position, position_, ids->first, real_estate_.occupancy().current))
        return false;
    send_zone(encode_re_menu_request(ids->first, ids->second, x, y));
    return true;
}

bool GameConnection::re_place_cursor_item(std::int32_t x, std::int32_t y) {
    // CEverQuest::LMouseUp @0x5388xx: a placeable item (ItemBase_IsPlaceable), unless (no current estate or Shift)
    // and wearable: then the request, or nothing while one is pending.
    const Item* held = inventory().cursor();
    if (!held || held->placeable_flags() == 0 || state_ != ConnectionState::InZone || placement_.active()) return false;
    if (real_estate_.pending()) return true;
    RePlaceRequest q;
    q.key = held->serial.empty() ? std::string("0000000000000000") : held->serial;
    q.index = {0, kReCursorSlot, -1, -1};
    q.x = x;
    q.y = y;
    send_zone(encode(q));
    real_estate_.set_pending(static_cast<std::uint32_t>(now_ms()));
    return true;
}

void GameConnection::re_place_row(const std::string& serial, std::uint32_t re, std::uint32_t item, std::int32_t x,
                                  std::int32_t y) {
    // CRealEstateItemsWnd's Place on a row of container type 0x0e: the row's serial and ids, the index -1s.
    if (state_ != ConnectionState::InZone || real_estate_.pending()) return;
    RePlaceRequest q;
    q.key = serial;
    q.re = re;
    q.item = item;
    q.x = x;
    q.y = y;
    send_zone(encode(q));
    real_estate_.set_pending(static_cast<std::uint32_t>(now_ms()));
}

void GameConnection::placement_set_mode(int mode) {
    // ItemPlacement_SetMode: a change shows the mode's name (ItemPlacement_ShowModeChanged), a refusal eqstr 5270
    // with it (ItemPlacement_ShowModeNotAllowed).
    if (!placement_.active()) return;
    if (placement_.set_mode(mode)) {
        system(game::place_mode_name(placement_.mode()));
    } else if (mode >= 0) {
        system(5270, {strings_.format(game::place_mode_name(mode), std::vector<std::string>{})});
    }
}

void GameConnection::placement_update(const glm::vec3& world, const glm::vec3& lo, const glm::vec3& hi,
                                      std::uint32_t extra_code) {
    if (!placement_.active()) return;
    placement_.move_to(world);
    placement_.validate(real_estate_.areas(), real_estate_.occupancy().root, lo, hi);
    if (placement_.code() == 0 && extra_code != 0) placement_.set_code(extra_code);
}

bool GameConnection::placement_click(const glm::vec3& box_a, const glm::vec3& box_b) {
    // The listener's slot 2 (Send_Opcode_0x711a): only with a valid code; RealEstateManagerClient_SetRequestPending.
    if (!placement_.active() || state_ != ConnectionState::InZone || real_estate_.pending()) return false;
    auto p = placement_.confirm(box_a, box_b);
    if (!p) return false;
    send_zone(std::move(*p));
    real_estate_.set_pending(static_cast<std::uint32_t>(now_ms()));
    return true;
}

void GameConnection::re_show_plot(std::uint32_t re) {
    re_ui_.purchase_re = re;
    re_ui_.purchase_open = true;
    ++re_ui_.changes;
}

} // namespace hailstone::net
