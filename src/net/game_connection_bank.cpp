// GameConnection: the bank window (CBankWnd), coin moves (CEverQuest::MoveMoney)
// and the bank's Auto button. Item moves into and out of the bank go
// through click_slot (net/game_connection_items.cpp) with the rules in
// game/bank.h. They follow the original's CBankWnd and CEverQuest functions
// named at each function below.
//
// The window has no packets of its own: RightClickedOnPlayer @0x536d80 calls
// CBankWnd::Activate(banker) for a class 40 spawn when no banker is active
// and PlayerClient_IsWithinInteractRange(banker, 1, 20.0) holds. The server
// checks a banker within 200 units (GetClosestBanker, USE_NPC_RANGE2) on
// every bank item and coin move.

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iterator>
#include <glm/geometric.hpp>

#include "core/atomic_file.h"
#include "core/log.h"
#include "game/distance.h"
#include "game/world_state.h"
#include "net/game_connection.h"
#include "net/trade_packets.h"

namespace hailstone::net {

namespace {

constexpr std::uint8_t kClassBanker = 40;
constexpr float kInteractRange = 20.0f;

ItemSlot cursor_slot() { return ItemSlot::possessions(kSlotCursor); }

} // namespace

void GameConnection::open_bank(std::uint32_t banker_id) {
    if (state_ != ConnectionState::InZone || pstatus_.dead) return;
    auto it = world_.spawns.find(banker_id);
    const game::Spawn* me = world_.player();
    if (it == world_.spawns.end() || !me || it->second.class_id != kClassBanker || it->second.is_corpse) return;
    if (bank_) return; // pinstActiveBanker already set
    glm::vec3 at = motion_known_ ? position_ : me->position;
    if (glm::distance(at, it->second.position) >= kInteractRange) return;
    activate_bank(banker_id);
}

bool GameConnection::activate_bank(std::uint32_t banker_id) {
    if (state_ != ConnectionState::InZone || pstatus_.dead) return false;
    auto it = world_.spawns.find(banker_id);
    if (it == world_.spawns.end()) return false;
    // (Activate over an open window is not known; the open one is kept, the safe choice.)
    if (bank_) return true;
    // CBankWnd::Activate @0x6273e0: "%1 tells you, 'Welcome to my bank!'"
    // (1465), auto attack off (1466) and auto fire off (SetAutoRangeAttack's 279), then the window shows. It also switches the music (not here) and
    // calls MakeMeVisible(0, 0): only when invisible, the invisibility
    // effects go, SpawnAppearance {me, 3, 0} and OP_Hide when hidden.
    bank_ = BankSession{};
    bank_->serial = ++npc_session_serial_;
    bank_->banker_id = banker_id;
    bank_->name = game::display_name(it->second.name);
    bank_->slots = game::bank_slot_count(profile_ ? profile_->expansions : 0, inventory_);
    chat(ChatEvent::Kind::Tell, strings_.format(1465, std::vector<std::string>{bank_->name}));
    if (auto_attack_) {
        set_auto_attack(false);
        chat(ChatEvent::Kind::System, strings_.format(1466, std::vector<std::string>{}));
    }
    if (auto_fire_) set_auto_fire(false);
    make_me_visible(false);
    bank_check_ms_ = 0; // AboutToShow: NextRefreshTime = 0
    // Hailstone: with a banker in reach the server has sent what the bank holds (EQEmu's lazy load starts once a banker
    // is near), so the inventory's bank is the bank, even when it is empty, and the copy as last seen is no longer
    // listed. A plugin's bank with no banker near (the NMS vault) counts too: what the window shows is the bank then,
    // and the player's moves into it must be listed.
    set_bank_live(true);
    log::info("bank: {} open, {} slots", bank_->name, bank_->slots);
    return true;
}

void GameConnection::close_bank() {
    // CBankWnd::Show(false) @0x627680: "%1 tells you, 'Come back soon!'"
    // (12143) while a banker is set, then the banker is cleared. AboutToHide
    // sends nothing; the quantity dialog closes with the window (OnShow).
    // OnShow @0x627760 also runs CContainerMgr::CloseAllContainers @0x65d4e0: every container window closes,
    // carried bags too (the UI's bag windows follow BankSession::serial, app/lua_eq_items.cpp), the world
    // container is released (OP_ClickObjectAction) and the tradeskill window hides.
    if (!bank_) return;
    chat(ChatEvent::Kind::Tell, strings_.format(12143, std::vector<std::string>{bank_->name}));
    bank_.reset();
    quantity_.reset();
    if (world_container_) close_world_container();
    if (tradeskill_.open) close_tradeskill();
    capture_bank();
}

// ---------------------------------------------------------------- the bank as last seen (Hailstone; game/bank_snapshot.h)

void GameConnection::set_bank_live(bool live) {
    if (bank_live_ == live) return;
    bank_live_ = live;
    ++bank_view_revision_;
}

void GameConnection::capture_bank() {
    if (!bank_live_) return;
    bank_seen_ = game::bank_snapshot_of(inventory_, static_cast<std::int64_t>(std::time(nullptr)));
    ++bank_view_revision_;
    if (!options_.bank_snapshot_file) return;
    std::filesystem::path path = own_files().bank_snapshot();
    if (path.empty()) return;
    if (!write_file_atomic(path, game::encode_bank_snapshot(bank_seen_))) log::warn("cannot write {}", path.string());
}

void GameConnection::load_bank_snapshot() {
    bank_seen_ = {};
    bank_live_ = false;
    ++bank_view_revision_;
    if (!options_.bank_snapshot_file) return;
    std::filesystem::path path = own_files().bank_snapshot();
    if (path.empty()) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (auto s = game::decode_bank_snapshot(std::as_bytes(std::span(bytes.data(), bytes.size())))) bank_seen_ = std::move(*s);
    else log::warn("{} is not a bank snapshot this build reads; ignored", path.string());
}

void GameConnection::update_bank() {
    // CBankWnd::OnProcessFrame @0x627850: every 500 ms, without the banker
    // or out of PlayerClient_IsWithinInteractRange(banker, 0, 20.0) (the
    // same check as the merchant window's), the window hides.
    if (!bank_) return;
    std::int64_t now = now_ms();
    if (now < bank_check_ms_) return;
    bank_check_ms_ = now + 500;
    const game::Spawn* me = world_.player();
    auto it = world_.spawns.find(bank_->banker_id);
    bool in_range = false;
    if (me && it != world_.spawns.end()) {
        glm::vec3 at = motion_known_ ? position_ : me->position;
        in_range = !(game::player_distance(&at, &it->second.position) > kInteractRange);
    }
    if (!in_range) {
        log::info("bank: {} is out of reach, closing", bank_->name);
        close_bank();
    }
}

// ---------------------------------------------------------------- coin

std::uint32_t* GameConnection::coin_pile(int location, int type) {
    if (type < 0 || type > 3) return nullptr;
    switch (location) {
    case kCoinCursor: return &cursor_coins_[static_cast<std::size_t>(type)];
    case kCoinInventory: return type == 0 ? &coins_.copper : type == 1 ? &coins_.silver : type == 2 ? &coins_.gold : &coins_.platinum;
    case kCoinBank: return bank_coins_.of(type, false);
    case kCoinSharedBank: return bank_coins_.of(type, true);
    case kCoinTrade: return trade_ ? &trade_->my_coins[static_cast<std::size_t>(type)] : nullptr;
    default: return nullptr;
    }
}

bool GameConnection::move_coin(int from, int to, int from_type, int to_type, std::uint32_t amount) {
    // CEverQuest::MoveMoney @0x51ad70 (game::move_money; types here are the wire's, 0 copper
    // .. 3 platinum, the original's coins 3 - type). The packet carries the amount asked, not the converted one
    // (the server converts again, as EQEmu's OPMoveCoin does).
    if (state_ != ConnectionState::InZone || from_type < 0 || from_type > 3 || to_type < 0 || to_type > 3) return false;
    // EQEmu keeps only platinum in the shared bank; its other coins have no counter here.
    if ((from == kCoinSharedBank && from_type != 3) || (to == kCoinSharedBank && to_type != 3)) return false;
    game::MoneyState st;
    for (int location = 0; location < 5; ++location) {
        for (int coin = 0; coin < 4; ++coin) {
            std::uint32_t* pile = coin_pile(location, 3 - coin);
            st.coins[static_cast<std::size_t>(location)][static_cast<std::size_t>(coin)] = pile ? static_cast<std::int32_t>(*pile) : 0;
        }
    }
    st.present[kCoinTrade] = trade_.has_value();
    game::MoneyEnv env;
    env.has_local_pc = profile_.has_value();
    env.bank_window_active = bank_.has_value();
    // CCursorAttachment::RemoveAttachment(3): the cursor must hold no item and no money, and the quantity window
    // must be closed.
    env.cursor_attachment_free = !inventory_.cursor() && !quantity_ &&
                                 std::all_of(cursor_coins_.begin(), cursor_coins_.end(), [](std::uint32_t c) { return c == 0; });
    game::MoneyMoveResult r = game::move_money(st, env, from, to, static_cast<std::uint32_t>(3 - from_type),
                                               static_cast<std::uint32_t>(3 - to_type), static_cast<std::int32_t>(amount), 0);
    if (r.value == 0) return false;
    for (int location = 0; location < 5; ++location) {
        for (int coin = 0; coin < 4; ++coin) {
            if (std::uint32_t* pile = coin_pile(location, 3 - coin))
                *pile = static_cast<std::uint32_t>(st.coins[static_cast<std::size_t>(location)][static_cast<std::size_t>(coin)]);
        }
    }
    if (r.played_sound) {
        SoundCue cue;
        cue.kind = SoundCue::Kind::Wave;
        cue.spell = 0x8a;
        sound_cues_.push_back(cue);
    }
    if (r.send) send_zone(encode(MoveCoin{from, to, r.wire_from_coin, r.wire_to_coin, static_cast<std::int32_t>(amount)}));
    return true;
}

std::int32_t GameConnection::inventory_coin(int coin) const {
    switch (coin) {
    case 0: return static_cast<std::int32_t>(coins_.platinum);
    case 1: return static_cast<std::int32_t>(coins_.gold);
    case 2: return static_cast<std::int32_t>(coins_.silver);
    case 3: return static_cast<std::int32_t>(coins_.copper);
    default: return 0;
    }
}

game::MoneyButtonOut GameConnection::inventory_money_button(int coin, std::int32_t amount,
                                                            const std::array<std::int32_t, 4>* button_rect) {
    // CInventoryWnd::ClickedMoneyButton @0x692890.
    game::MoneyButtonIn in;
    in.coin = coin;
    in.amount = amount;
    for (int c = 0; c < 4; ++c) {
        in.cursor_coins[static_cast<std::size_t>(c)] = static_cast<std::int32_t>(cursor_coins_[static_cast<std::size_t>(3 - c)]);
        if (in.cursor_coin_type == 0 && in.cursor_coins[static_cast<std::size_t>(c)] > 0)
            in.cursor_coin_type = static_cast<std::uint8_t>(c + 1); // CDisplay +0x11, as MoveMoney leaves it
    }
    in.has_button = button_rect != nullptr;
    if (button_rect) in.button_rect = *button_rect;
    game::MoneyButtonOut out = game::clicked_money_button(in);
    if (out.kind != game::MoneyButtonOut::Kind::AskQuantity && out.amount > 0 && out.from_coin <= 3 && out.to_coin <= 3)
        move_coin(out.from, out.to, 3 - static_cast<int>(out.from_coin), 3 - static_cast<int>(out.to_coin),
                  static_cast<std::uint32_t>(out.amount));
    return out;
}

void GameConnection::bank_deposit(int type, std::uint32_t amount, bool shared) {
    // CInventoryWnd's money button with the quantity dialog (MoveMoney
    // inventory → cursor), then the bank's money button with coin held
    // (CBankWnd::sub_00628270: MoveMoney(cursor → bank or shared bank,
    // held type, button type, all the held coin of that type)).
    if (!bank_ || type < 0 || type > 3) return;
    if (!move_coin(kCoinInventory, kCoinCursor, type, type, amount)) return;
    int to_type = shared ? 3 : type; // the shared bank's button is platinum
    std::uint32_t held = cursor_coins_[static_cast<std::size_t>(type)];
    if (!move_coin(kCoinCursor, shared ? kCoinSharedBank : kCoinBank, type, to_type, held)) return;
    // What did not convert (a remainder below one platinum) goes back.
    if (std::uint32_t left = cursor_coins_[static_cast<std::size_t>(type)])
        move_coin(kCoinCursor, kCoinInventory, type, type, left);
}

void GameConnection::bank_withdraw(int type, std::uint32_t amount, bool shared) {
    // The bank's money button with the quantity dialog (sub_00628270 with no
    // coin held: MoveMoney(bank → cursor)), then the inventory's money button.
    if (!bank_ || type < 0 || type > 3 || (shared && type != 3)) return;
    if (!move_coin(shared ? kCoinSharedBank : kCoinBank, kCoinCursor, type, type, amount)) return;
    move_coin(kCoinCursor, kCoinInventory, type, type, cursor_coins_[static_cast<std::size_t>(type)]);
}

// ---------------------------------------------------------------- Auto button

int GameConnection::merge_stacks(const ItemSlot& from, const ItemSlot& to) {
    // Send_OP_MoveItem @0x4ee3c0 → ItemContainer_MergeStacks(src, dst, -1, partial)
    // @0x7e4ea0: as much as fits moves; OP_MoveItem carries the count moved.
    Item* src = inventory_.at(from);
    Item* dst = inventory_.at(to);
    if (!src || !dst || !game::can_stack_with(*src, *dst)) return 2;
    std::uint32_t space = dst->max_stack > dst->stack_count ? dst->max_stack - dst->stack_count : 0;
    if (space == 0) return 2;
    std::uint32_t moved = std::min(space, src->stack_count);
    dst->stack_count += moved;
    src->stack_count -= moved;
    MoveItem m;
    m.from = from;
    m.to = to;
    m.count = moved;
    send_zone(encode(m));
    if (src->stack_count == 0) {
        inventory_.take(from);
        return 0;
    }
    return 1;
}

void GameConnection::auto_bank() {
    if (!bank_ || state_ != ConnectionState::InZone) return;
    auto_bank_cursor(true);
}

void GameConnection::auto_bank_cursor(bool stacks) {
    // CBankWnd_AutoBankCursor @0x6288e0.
    const ItemSlot cursor = cursor_slot();
    const Item* held = inventory_.cursor();
    if (!held) {
        // Coin held: one coin type into the bank, copper first (the original's
        // cursor indices 3, 2, 1, 0), all of it.
        for (int type = 0; type < 4; ++type) {
            if (std::uint32_t n = cursor_coins_[static_cast<std::size_t>(type)]) {
                move_coin(kCoinCursor, kCoinBank, type, type, n);
                return;
            }
        }
        return;
    }
    if (auto r = game::bank_refusal(*held)) {
        chat(ChatEvent::Kind::System, strings_.format(r.eqstr, std::vector<std::string>{held->name}));
        return;
    }
    const int slots = bank_->slots;
    if (held->stackable() && stacks) {
        // First pass: merge into bank stacks of the same item, then into
        // stacks of it inside bank bags (those not already full).
        for (std::int16_t s = 0; s < slots; ++s) {
            const Item* there = inventory_.at({kInvBank, s, -1, -1, 0, 0});
            if (!there || there->id != held->id) continue;
            if (merge_stacks(cursor, {kInvBank, s, -1, -1, 0, 0}) == 0) return;
            held = inventory_.cursor();
        }
        for (std::int16_t s = 0; s < slots; ++s) {
            const Item* bag = inventory_.at({kInvBank, s, -1, -1, 0, 0});
            if (!bag || !bag->is_bag()) continue;
            for (std::int16_t i = 0; i < bag->bag_slots; ++i) {
                ItemSlot in{kInvBank, s, i, -1, 0, 0};
                const Item* there = inventory_.at(in);
                if (!there || there->id != held->id || there->stack_count >= there->max_stack) continue;
                if (merge_stacks(cursor, in) == 0) return;
                held = inventory_.cursor();
            }
        }
        auto_bank_cursor(false);
        return;
    }
    // A free bank slot.
    for (std::int16_t s = 0; s < slots; ++s) {
        ItemSlot to{kInvBank, s, -1, -1, 0, 0};
        if (!inventory_.at(to)) {
            move_item(cursor, to, 0);
            return;
        }
    }
    // A free slot in a bank bag (not for a bag); CanGoInBag decides.
    if (!held->is_bag()) {
        for (std::int16_t s = 0; s < slots; ++s) {
            const Item* bag = inventory_.at({kInvBank, s, -1, -1, 0, 0});
            if (!bag || !bag->is_bag() || game::bag_fit(*bag, *held) != 1) continue;
            for (std::int16_t i = 0; i < bag->bag_slots; ++i) {
                ItemSlot to{kInvBank, s, i, -1, 0, 0};
                if (!inventory_.at(to)) {
                    move_item(cursor, to, 0);
                    return;
                }
            }
        }
    }
    chat(ChatEvent::Kind::System, strings_.format(3485, std::vector<std::string>{})); // You have no room left in the bank.
}

} // namespace hailstone::net
