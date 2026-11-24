#include "game/hot_button_menu.h"

#include "game/hot_button_press.h"

namespace hailstone::game {

namespace {

bool item_type(HotButtonType t) {
    return t == HotButtonType::InventorySlot || t == HotButtonType::ItemLink || t == HotButtonType::KronoSlot;
}

} // namespace

HotButtonMouseAction hot_button_mouse(const HotButton& b, HotButtonMouse m, bool cursor) {
    using A = HotButtonMouseAction;
    if (m == HotButtonMouse::RightHeld) return A::Menu; // XWM_RCLICKHOLD: CHotButton_ShowContextMenu
    if (b.empty() && !cursor) return A::Menu;          // XWM_LCLICK / XWM_RCLICK on an empty button
    if (m == HotButtonMouse::LeftClick) return cursor ? A::Drop : A::Press;
    return item_type(b.type) ? A::None : A::RightClick;
}

HotButtonMenuRows hot_button_menu_rows(const HotButton& b, bool gem_spell, bool cast_spell_window) {
    HotButtonMenuRows r;
    const bool filled = !b.empty();
    const bool gem = b.type == HotButtonType::SpellGem;
    const bool has_spell = filled && gem && gem_spell;
    r.activate = filled && b.type != HotButtonType::KronoSlot;
    r.consume = filled && b.type == HotButtonType::KronoSlot;
    r.inspect = filled && (has_spell || item_type(b.type));
    r.clear = r.assign_icon = r.assign_label = filled;
    r.clear_icon = filled && b.icon_type != -1;
    r.memorize = cast_spell_window && filled && gem && !has_spell;
    r.unmemorize = cast_spell_window && has_spell;
    return r;
}

bool assign_social(HotButtonStore& store, HotButton& b, std::int32_t social) {
    b.type = HotButtonType::Social;
    b.slot = social;
    return social >= 0 && social < kSocialCount && store.social(social).name.empty();
}

bool assign_label(HotButtonStore& store, HotButton& b, const std::string& text) {
    if (b.type == HotButtonType::Social && b.slot >= 0 && b.slot < kSocialCount) {
        store.social(b.slot).name = text;
        return true;
    }
    b.label = text.substr(0, 63);
    return false;
}

std::optional<HotButton> inventory_hot_button(const net::ItemSlot& slot, const net::Item* item, bool shift_or_ctrl) {
    if (slot.type != net::kInvPossessions || !slot.valid()) return std::nullopt;
    const bool link = item && !shift_or_ctrl;
    if (!link && !slot.top_level()) return std::nullopt;
    HotButton b;
    if (link) {
        b.type = HotButtonType::ItemLink;
        b.slot = -1;
        if (!item->serial.empty()) b.guid = item->serial;
        b.item_id = static_cast<std::int32_t>(item->id);
    } else {
        b.type = HotButtonType::InventorySlot;
        b.slot = slot.slot;
    }
    return b;
}

std::optional<net::ItemSlot> hot_button_item_slot(const Inventory& inv, const HotButton& b) {
    if (b.type == HotButtonType::InventorySlot) {
        if (b.slot < 0 || b.slot >= net::kSlotCursor) return std::nullopt;
        return net::ItemSlot::possessions(static_cast<std::int16_t>(b.slot));
    }
    if (b.type != HotButtonType::ItemLink) return std::nullopt;
    const bool has_guid = b.guid.find_first_not_of('0') != std::string::npos;
    std::optional<net::ItemSlot> by_id;
    for (const auto& [key, top] : inv.items()) {
        if (key.first != net::kInvPossessions || key.second < 0 || key.second >= net::kSlotCursor) continue;
        auto check = [&](const net::Item& it, net::ItemSlot at) {
            if (has_guid && it.serial == b.guid) return true;
            if (!by_id && b.item_id != 0 && static_cast<std::int32_t>(it.id) == b.item_id) by_id = at;
            return false;
        };
        if (check(top, net::ItemSlot::possessions(key.second))) return net::ItemSlot::possessions(key.second);
        for (const auto& c : top.contents) {
            const auto at = net::ItemSlot::possessions(key.second, static_cast<std::int16_t>(c.index));
            if (check(c.item, at)) return at;
        }
    }
    return by_id;
}

std::uint64_t hot_button_stack_count(const Inventory& inv, HotButtonType type, const net::Item& item) {
    if (!item.stackable()) return 0;
    if (type == HotButtonType::InventorySlot) return item.stack_count;
    if (type != HotButtonType::ItemLink) return 0;

    std::uint64_t count = 0;
    auto add = [&](const net::Item& stack) {
        if (stack.id == item.id && stack.stackable()) count += stack.stack_count;
    };
    // Count the possessions a link can find, including stacks in bags.
    for (const auto& [key, top] : inv.items()) {
        if (key.first != net::kInvPossessions || key.second < 0 || key.second >= net::kSlotCursor) continue;
        add(top);
        for (const auto& child : top.contents) add(child.item);
    }
    return count;
}

} // namespace hailstone::game
