// Using items: a right click on an item slot (CInvSlot::HandleRButtonUp @0x697250), /useitem (Cmd_useitem
// @0x479200) and the potion belt send OP_ItemVerifyRequest; the server's OP_ItemVerifyReply
// (Handle_OP_ItemVerifyReply @0x4bf800) eats, drinks, applies a poison or casts the item's click effect (read
// from the disassembly).
#include "net/game_connection.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <format>
#include <glm/geometric.hpp>

#include "core/log.h"
#include "core/wrap_math.h"
#include "game/augments.h"
#include "game/item_preview.h"
#include "game/merchant.h"
#include "game/mechanics/character.h"
#include "game/server_ext.h"
#include "game/slash_commands.h"
#include "net/item_packets.h"

namespace hailstone::net {

namespace {

// ItemContainerInstance values HandleRButtonUp tests (eqlib Items.h).
constexpr std::int16_t kInvBazaar = 12, kInvInspect = 13, kInvKrono = 23;
// The slot the reply's dispatch keeps for a timer of its own (@0x4c57af: 0xdd25bc; EQEmu never sends it).
constexpr std::int16_t kTimerSlot = 0x407;
constexpr std::uint32_t kChatSpells = 0x121; // token_string_to_client's colour here
constexpr std::uint32_t kChatFood = 0x111;   // the eating and drinking lines
constexpr std::int32_t kFull = 6000;         // hunger / thirst cap (CMP 6000 @0x4bf8de)
constexpr std::uint32_t kChatDefault = 0x111;  // dsp_chat_2's colour
constexpr std::uint32_t kSkillApplyPoison = 6;
constexpr std::uint32_t kSpaPoisonTime = 234;  // TotalEffect(0xea, 1, 0, 1, 1) @0x45e18f: the application's ms
constexpr std::int64_t kPoisonTime = 10000;    // without it (0x2710)
constexpr std::uint32_t kChatPoisonResult = 0x10e;

bool is_vowel(char c) {
    switch (std::tolower(static_cast<unsigned char>(c))) {
    case 'a': case 'e': case 'i': case 'o': case 'u': return true;
    default: return false;
    }
}

} // namespace

bool GameConnection::bard_cast_bard(const game::SpellData& spell, std::uint8_t class_id) const {
    // CharacterZoneClient::BardCastBard (game/mechanics/cast_spell.h), the server extension's song block first.
    if (server_ext().song_blocked(*this, spell.class_level[7])) return false;
    if (class_id != 8 || spell.is_skill) return false;
    const std::uint8_t level = spell_level_needed(spell, 8);
    return level != 0 && level < 0xff;
}

GameConnection::SlotRightClick GameConnection::right_click_item(const ItemSlot& slot, KeyFlags keys) {
    // A server may supply an optional direct-sale protocol. Validate the actual
    // carried click before exposing a context; the extension owns no inventory state.
    if (!keys.alt && !keys.ctrl && !keys.shift && state_ == ConnectionState::InZone && !zone_change_pending_ &&
        !pstatus_.dead && !interface_locked() && merchant_ && merchant_->open && !merchant_->trader &&
        !merchant_->pending && !quantity_ && slot.type == kInvPossessions && slot.slot >= 0 &&
        slot.slot <= kSlotGeneral10 && slot.aug < 0 && slot.sub >= -1) {
        const auto* me = world_.player();
        const auto vendor = world_.spawns.find(merchant_->npc_id);
        const auto* item = inventory_.at(slot);
        const auto* parent = slot.sub >= 0 ? inventory_.at(slot.parent()) : nullptr;
        const bool carried = slot.sub == -1 || (slot.slot >= kSlotGeneral1 && parent && parent->is_bag() &&
            static_cast<std::uint32_t>(slot.sub) < parent->bag_slots);
        if (me && vendor != world_.spawns.end() && item && !item->is_bag() && item->id && carried &&
            glm::distance(me->position, vendor->second.position) < 20.0f) {
            const auto quantity = item->stackable() ? item->stack_count : 1u;
            if (quantity && server_ext().merchant_right_click(*this, merchant_->npc_id, slot, item->id, quantity))
                return SlotRightClick::None;
        }
    }
    // The server's client may take the click first (NMS: the scroll and tome scribe path).
    if (server_ext().item_right_click(*this, slot)) return SlotRightClick::None;
    return right_click_item_original(slot, keys);
}

GameConnection::SlotRightClick GameConnection::right_click_item_original(const ItemSlot& slot, KeyFlags keys,
                                                                         std::uint8_t class_id) {
    if (state_ != ConnectionState::InZone || !profile_) return SlotRightClick::None;
    // Nothing while a request is pending (PendingRequestLog_SendReport(0x4a)).
    if (spells_.pending > 0) {
        log::debug("item right click refused: {} request(s) pending", spells_.pending);
        return SlotRightClick::None;
    }
    if (static_cast<std::uint16_t>(slot.type) > 0x17 || slot.slot == -1) return SlotRightClick::None;
    if (slot.type == kInvInspect) return SlotRightClick::None;
    if (slot.type == kInvBank && slot.slot >= (bank_ ? bank_->slots : 16)) return SlotRightClick::None; // InvSlot_IsBankSlotBeyondLimit
    const Item* item = inventory_.at(slot);
    // Alt (keyboard flags 0xc): the item preview, Send_OP_ItemPreviewRequest @0x6fff10 and the window shown.
    if (keys.alt) {
        if (!item) return SlotRightClick::None;
        preview_item(item);
        return SlotRightClick::ItemPreview;
    }
    // The Krono slot: eqstr 5370 without Krono, else confirmation 0x90 (5369); this client has no Krono slot.
    if (slot.type == kInvKrono) return SlotRightClick::None;
    // Ctrl + Shift on a trader's slot selects it (CBazaarWnd::SelectBazaarSlotItem).
    if (keys.ctrl && keys.shift && slot.type == kInvBazaar) return SlotRightClick::BazaarSelect;
    // While a spell is being cast, only a bard's song lets the click through (BardCastBard on the spawn's class).
    if (spells_.casting.casting()) {
        if (const game::SpellData* s = spell_table_->get(spells_.casting.spell))
            if (!bard_cast_bard(*s, class_id ? class_id : visible_class())) return SlotRightClick::None;
    }
    if (!item) return SlotRightClick::None; // (CBazaarWnd_RebuildTraderItems, AlertInventoryChanged)
    if (slot.type == kInvCorpse) { // CLootWnd::RequestLootSlot(slot, 1)
        loot_item(slot.slot, true, keys.shift, keys.ctrl);
        return SlotRightClick::None;
    }
    if (item->item_class == 1) { // a container
        if (item->placeable_flags() != 0) {
            if (slot.type != kInvWorld) system(5311, {item->name}, 0xf);
            return SlotRightClick::None;
        }
        // Not in a world container and top level: open or close it; Ctrl with a bag on the cursor trades the two bags'
        // contents instead (@0x6976ca: MoveItem(cursor, slot, 1, 1, Ctrl, bForceBag 1), game/bag_move.h).
        if (slot.type != kInvWorld && slot.top_level()) {
            const Item* held = inventory_.cursor();
            if (keys.ctrl && held && held->item_class == 1) {
                move_into_bag(slot, true);
                return SlotRightClick::None;
            }
            return SlotRightClick::ToggleContainer;
        }
        return SlotRightClick::None;
    }
    if (slot.type != kInvPossessions) return SlotRightClick::None;
    if (item->item_class == 2) { // a book or note: OP_ReadBook
        read_book(slot);
        return SlotRightClick::None;
    }
    if (item->item_class == 0) use_item(slot);
    return SlotRightClick::None;
}

bool GameConnection::use_item(const ItemSlot& slot) {
    // HandleRButtonUp's common-item path, as Cmd_useitem and PcZoneClient_UsePotionBeltSlot run it too.
    if (state_ != ConnectionState::InZone || !profile_) return false;
    const Item* item = inventory_.at(slot);
    if (!item || item->item_class != 0) return false;
    // An item with a click spell that cannot be cast sitting: stand up (the auto-stand option 0xde0d5b), else
    // 12441 (300 for a skill) and, feigning, 12368; nothing is sent then.
    const ItemEffect& click = item->effects[kEffectClick];
    if (const game::SpellData* s = click.effect > 0 ? spell_table_->get(static_cast<std::uint32_t>(click.effect)) : nullptr;
        s && !s->cast_not_standing) {
        if (pstatus_.stand_state == 110 && client_option(game::ClientOption::AllowAutoStand)) set_stand_state(100);
        if (pstatus_.stand_state != 100) {
            spell_message(s->is_skill ? 300 : 12441, {}, kChatSpells);
            if (pstatus_.stand_state == 102) spell_message(12368, {}, kChatSpells);
            return false;
        }
    }
    // IsItemPending + 1 (PendingRequestLog_PushPendingReason(0x1c)); the reply takes it back.
    ++spells_.pending;
    send_zone(encode_item_verify_request(slot, world_.target_id));
    log::info("use item {} at {}/{}/{}", item->name, slot.type, slot.slot, slot.sub);
    return true;
}

bool GameConnection::cast_item(const ItemSlot& slot) { return use_item(slot); }

void GameConnection::handle_item_verify_reply(const ItemVerifyReply& r) {
    // The dispatch (@0x4c579a): every reply but the timer slot's takes back IsItemPending; a negative spell
    // stops there. pinstTarget is kept around the handler (any retarget of CastSpell's is undone).
    if (r.slot.slot != kTimerSlot) spells_.pending = std::max(0, spells_.pending - 1);
    log::debug("item verify reply: slot {}/{}/{} spell {}, casting {}", r.slot.type, r.slot.slot, r.slot.sub, r.spell,
               spells_.casting.casting() ? static_cast<std::int64_t>(spells_.casting.spell) : -1);
    if (r.spell < 0 || r.slot.slot == kTimerSlot) return;
    Item* item = inventory_.at(r.slot);
    if (!item) return;
    const ItemSlot slot = r.slot;
    const std::uint8_t type = item->item_type;
    std::int32_t spell = r.spell;
    bool remove_one = false;
    auto wave = [this](std::uint32_t id) {
        SoundCue cue;
        cue.kind = SoundCue::Kind::Wave;
        cue.spell = id;
        sound_cues_.push_back(cue);
    };
    switch (item->item_type) {
    case kItemTypePoison:
        apply_poison(slot); // then, as in the original, the click effect below (poisons have none)
        break;
    case kItemTypeFood:
    case kItemTypeDrink: {
        const bool drink = item->item_type == kItemTypeDrink;
        std::uint32_t& level = drink ? spells_.thirst : spells_.hunger;
        if (static_cast<std::int32_t>(level) >= kFull) {
            system(drink ? 12077 : 12074, {}, kChatFood); // too full to eat / drink
            return;
        }
        const bool vowel = !item->name.empty() && is_vowel(item->name[0]);
        system(drink ? (vowel ? 8502 : 12078) : (vowel ? 8501 : 12075), {item->name}, kChatFood);
        // FoodDuration x 100 / 2 in the original's 32 bits (the product wraps, the division is signed): a duration past
        // 2^31 / 100 wraps as it does there, written with wrapped_div so that no compiler folds it as if nothing
        // wrapped (AGENTS.md).
        // The add wraps in 32 bits too: the level is whatever the server last sent (OP_Stamina can carry any u32),
        // so it is done unsigned, never as a signed add that could overflow.
        const std::int32_t gain = core::wrapped_div(static_cast<std::uint32_t>(item->cast_time_) * 100u, 2);
        const auto sum = static_cast<std::int32_t>(level + static_cast<std::uint32_t>(gain));
        level = static_cast<std::uint32_t>(std::min(kFull, sum));
        if (drink) wave(0x95);
        system(drink ? 12079 : 12076, {}, kChatFood);
        send_zone(encode_consume_reply(slot, drink));
        consume_pending_ = true; // the auto-eat waits for the server's OP_Stamina
        remove_one = true;
        break;
    }
    case kItemTypeAlcohol: {
        if (drunkenness_ >= 200) {
            system(12080, {}, kChatFood);
            return;
        }
        system(12081, {item->name}, kChatFood);
        // 0x7bf990(the Alcohol Tolerance skill when known, else 0): drunkenness +
        // ftol(max(0, 270 - skill) / 270 x 30 + 10), at most 200.
        const std::int32_t tolerance = skill_value(66) > 0 ? skill_value(66) : 0;
        const std::int32_t x = std::max(0, 270 - tolerance);
        const double gain = static_cast<double>(x) * static_cast<double>(0.0037037036f) * 30.0 + 10.0;
        drunkenness_ = std::min(200, drunkenness_ + static_cast<std::int32_t>(gain));
        MoveItem del;
        del.from = slot;
        del.to = ItemSlot{-1, -1, -1, -1, -1, 0};
        del.count = 0xffffffffu;
        send_zone(encode_delete_item(del));
        system(12079, {}, kChatFood);
        remove_one = true;
        break;
    }
    default:
        break;
    }
    // The click effect: the reply's spell, else the item's (block 0). A charm scaled below 1 % does not cast.
    // CastSpell(12, spell, &item, slot, 0, 0, 1, 0, 0) @0x4bff71: the sixth argument, OP_CastSpell's gate
    // (@0x43c7b3: CMP byte [esp+0xb0],0), is 0. The cast runs locally (the recast timer, the cast bar) and
    // nothing is sent: the server cast the item when it took OP_ItemVerifyRequest (EQEmu's
    // Handle_OP_ItemVerifyRequest), and a second OP_CastSpell would cast an instant clicky again.
    if (spell == 0) spell = item->effects[kEffectClick].effect;
    if (spell > 0 && spell_table_->get(static_cast<std::uint32_t>(spell))) {
        const bool charm_off = item->charm_file_id != 0 && static_cast<std::int32_t>(item->scaled_value) < 1;
        if ((!spells_.casting.casting() || visible_class() == 8) && !charm_off) {
            cast_spell_slot(kCastItemSlot, static_cast<std::uint32_t>(spell), &slot, true, false);
            if (type == kItemTypePotion) wave(0x95);
        }
    }
    if (remove_one) {
        if (Item* it = inventory_.at(slot); it && it->stack_count > 1) --it->stack_count;
        else inventory_.take(slot);
    }
}

void GameConnection::apply_poison(const ItemSlot& slot) {
    if (skill_value(kSkillApplyPoison) <= 0) { // CharacterZoneClient::HasSkill(6)
        system(12383, {}, kChatDefault);          // You have no skill in the art of applying poisons.
        return;
    }
    if (pstatus_.stand_state == kStandStanding) set_stand_state(kStandSitting);
    if (pstatus_.stand_state != kStandSitting) {
        system(12381, {}, kChatDefault); // You must be sitting to apply the poison.
        return;
    }
    // Already applying, camping (0xddf8a0), quitting (0xddf8a8), interacting or an item request pending: nothing.
    if (poison_started_ms_ != 0 || state_ == ConnectionState::Camping || transact_state().currently_interacting > 0 ||
        spells_.pending > 0)
        return;
    poison_started_ms_ = std::max<std::int64_t>(1, now_ms());
    poison_slot_ = slot.slot; // *(short *)(index + 4): the top-level slot only, a poison in a bag sends its bag
    system(12382, {}, kChatDefault); // You begin applying the poison.
}

void GameConnection::apply_poison_tick() {
    // DoPassageOfTime @0x45e179: not while camping, interacting or with an item request pending.
    if (poison_started_ms_ == 0 || state_ != ConnectionState::InZone) return;
    if (transact_state().currently_interacting > 0 || spells_.pending > 0) return;
    std::int64_t time = 0;
    with_character([&](game::mechanics::Character& ch) {
        time = static_cast<std::uint32_t>(ch.total_effect(kSpaPoisonTime, true, 0, true, true));
    });
    if (time == 0) time = kPoisonTime;
    const std::int64_t now = now_ms();
    if (poison_started_ms_ > now) poison_started_ms_ = now;
    if (now - poison_started_ms_ <= time) return;
    // Both counters are set to 1 (__CurrentlyInteracting and IsItemPending zeroed then raised; interaction reason 6,
    // pending reason 0x2f); the reply takes them back.
    spells_.pending = 1;
    poison_interacting_ = true;
    ApplyPoison a;
    a.slot = poison_slot_;
    send_zone(encode(a));
    log::info("apply poison: OP_ApplyPoison slot {}", a.slot);
    poison_started_ms_ = 0;
    poison_slot_ = 0;
}

void GameConnection::handle_apply_poison(const ApplyPoison& r) {
    // Handle_OP_ApplyPoison @0x4b5c20: the interaction ends (IsItemPending and __CurrentlyInteracting 0, the reason
    // lists cleared), then 12905 / 12906 by success. EQEmu has already removed the poison (DeleteItemInInventory
    // with its client update) and added the proc to the primary weapon.
    spells_.pending = 0;
    poison_interacting_ = false;
    system(r.success ? 12905 : 12906, {}, kChatPoisonResult);
}

void GameConnection::useitem_command(const std::string& args) {
    // Cmd_useitem @0x479200: /useitem <slot 0..32> [index in the bag], numbers only.
    std::string_view rest = args;
    const std::string a = game::next_cmd_arg(rest);
    const bool numeric = !a.empty() && std::all_of(a.begin(), a.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
    if (!numeric) {
        static constexpr const char* kSlots[] = {"Charm", "Left Ear", "Head", "Face", "Right Ear", "Neck", "Shoulders",
                                                 "Arms", "Back", "Left Wrist", "Right Wrist", "Range", "Hands",
                                                 "Primary", "Secondary", "Left Ring", "Right Ring", "Chest", "Legs",
                                                 "Feet", "Waist", "Power Source", "Ammo"};
        chat(ChatEvent::Kind::System, "usage: /useitem <slot> <subindex>");
        chat(ChatEvent::Kind::System, "slot definitions:");
        for (int i = 0; i < 23; ++i) chat(ChatEvent::Kind::System, std::format("       {} = {}", kSlots[i], i));
        for (int i = 23; i < 33; ++i) chat(ChatEvent::Kind::System, std::format("       Bag Slot {} = {}", i - 22, i));
        chat(ChatEvent::Kind::System,
             "subindex: Only used if slot is a bag. Bag slot number starting at 0, left to right, top to bottom.");
        return;
    }
    const unsigned long n = std::strtoul(a.c_str(), nullptr, 10);
    if (n > 0x20) {
        chat(ChatEvent::Kind::System, "That is an invalid slot index.", 0, 0xf);
        return;
    }
    ItemSlot at = ItemSlot::possessions(static_cast<std::int16_t>(n));
    std::int32_t sub = -1;
    if (const std::string b = game::next_cmd_arg(rest); !b.empty() &&
        std::all_of(b.begin(), b.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
        sub = std::atoi(b.c_str());
    // A general slot holding a bag: the index picks the item in it (a bad index keeps the bag itself).
    if (n >= kSlotGeneral1 && n <= kSlotGeneral10)
        if (const Item* bag = inventory_.at(at); bag && bag->is_bag()) {
            if (sub < 0 || static_cast<std::uint32_t>(sub) >= bag->bag_slots)
                chat(ChatEvent::Kind::System, "That is an invalid bag slot index.", 0, 0xf);
            else
                at.sub = static_cast<std::int16_t>(sub);
        }
    use_item(at);
}

// ---------------------------------------------------------------- item recast timers

std::uint32_t GameConnection::item_recast_timer(const Item* item) const {
    if (!item) return 0;
    const std::uint32_t own = item->last_cast_time;
    const std::uint32_t type = item_recast_type_timer(item->effects[kEffectClick].recast_type());
    return own > type ? own : type;
}

void GameConnection::set_item_recast_timer(Item* item, std::int32_t delay, std::int32_t type) {
    const auto at = static_cast<std::uint32_t>(std::time(nullptr)) + static_cast<std::uint32_t>(delay);
    if (item) item->last_cast_time = at; // +0x30 (changed) is the original's update mark; nothing reads it here
    if (static_cast<std::uint32_t>(type) < item_recast_timers_.size()) item_recast_timers_[static_cast<std::size_t>(type)] = at;
}

void GameConnection::reset_all_item_recast_timers() {
    for (std::int16_t slot = 0; slot < 0x22; ++slot) {
        Item* item = inventory_.at(ItemSlot::possessions(slot));
        if (!item) continue;
        item->last_cast_time = 0;
        if (!item->is_bag()) continue;
        for (auto& c : item->contents) c.item.last_cast_time = 0;
    }
    item_recast_timers_.fill(0);
}

ItemSlot GameConnection::cast_item_slot() const {
    const auto& at = spells_.casting.item_location;
    ItemSlot s;
    s.type = static_cast<std::int16_t>(at[0] & 0xffffu);
    s.slot = static_cast<std::int16_t>(at[1] & 0xffffu);
    s.sub = static_cast<std::int16_t>(at[1] >> 16);
    s.aug = static_cast<std::int16_t>(at[2] & 0xffffu);
    return s;
}

Item* GameConnection::cast_item() { return spells_.casting.is_item_cast() ? inventory_.at(cast_item_slot()) : nullptr; }

Item* GameConnection::valid_cast_item() {
    Item* item = cast_item();
    return item && item->item_class == 0 && item->id == spells_.casting.item_id ? item : nullptr;
}

// ---------------------------------------------------------------- the item preview

void GameConnection::preview_item(const Item* item) {
    if (state_ != ConnectionState::InZone) return;
    item_preview_.open = true; // vtable +0xd8 (1, 1, 1)
    if (!item) { // 0x6ff980(0, 1): the view and its buttons hidden, the label shown
        item_preview_.show = false;
        item_preview_.item.reset();
        ++item_preview_.serial;
        return;
    }
    log::debug("item preview: request for {} ({})", item->id, item->name);
    send_zone(encode_item_preview_request(item->id, item));
}

void GameConnection::preview_item_id(std::uint32_t item_id) {
    if (state_ != ConnectionState::InZone) return;
    if (item_id == 0) {
        preview_item(nullptr);
        return;
    }
    item_preview_.open = true;
    send_zone(encode_item_preview_request(item_id));
}

void GameConnection::handle_item_preview(const AppPacket& p) {
    // HandleWorldMessage case 0x7f80 (0xb07f, the marketplace's, is not handled: no marketplace): 0x6ffa70.
    auto r = decode_item_preview(p);
    if (!r) {
        log::warn("could not decode the item preview ({} bytes)", p.data.size());
        return;
    }
    const game::ItemPreviewShow s = game::item_preview_show(r->item, r->trailing);
    item_preview_.show = s.show;
    item_preview_.on_player = s.on_player;
    if (s.show) item_preview_.item = std::move(r->item);
    else item_preview_.item.reset();
    ++item_preview_.serial;
    log::info("item preview: {} ({}), {}", item_preview_.item ? item_preview_.item->name : std::string("nothing"),
              item_preview_.item ? item_preview_.item->id_file : std::string(),
              !s.show ? "no preview" : s.on_player ? "on the player" : "alone");
}

// ---------------------------------------------------------------- stats-disabled bytes

std::int32_t GameConnection::game_feature(std::int32_t feature) const {
    if (!membership_ || feature < 0 || static_cast<std::size_t>(feature) >= membership_->entries.size()) return -1;
    return membership_->entries[static_cast<std::size_t>(feature)];
}

void GameConnection::refresh_worn_stats_disabled() {
    const std::int32_t use_item = game_feature(0x13), ornament = game_feature(0x15);
    for (std::int16_t slot = 0; slot < 0x16; ++slot)
        if (Item* item = inventory_.at(ItemSlot::possessions(slot))) game::update_stats_disabled(*item, use_item, ornament, true);
}

} // namespace hailstone::net
