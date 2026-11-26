// /itemnotify's line (game/item_notify.h): the slots and items it names, its notifications and MacroQuest's messages.
#include <doctest/doctest.h>

#include "game/item_notify.h"

using namespace hailstone;
using game::ItemNotification;
using game::parse_item_notify;
using net::ItemSlot;

namespace {
bool at(const game::ItemNotify& n, std::int16_t type, std::int16_t slot, std::int16_t sub = -1) {
    return n.error.empty() && n.slot && n.slot->type == type && n.slot->slot == slot && n.slot->sub == sub;
}
} // namespace

TEST_CASE("itemnotify: a slot by number or name") {
    CHECK(at(parse_item_notify("23 leftmouseup"), net::kInvPossessions, 23));
    CHECK(at(parse_item_notify("0 leftmouseup"), net::kInvPossessions, 0));
    CHECK(at(parse_item_notify("33 leftmouseup"), net::kInvPossessions, net::kSlotCursor));
    CHECK(at(parse_item_notify("mainhand rightmouseup"), net::kInvPossessions, net::kSlotPrimary));
    CHECK(at(parse_item_notify("ChEsT leftmouseup"), net::kInvPossessions, net::kSlotChest));
    CHECK(at(parse_item_notify("pack1 leftmouseup"), net::kInvPossessions, net::kSlotGeneral1));
    CHECK(at(parse_item_notify("pack10 rightmouseup"), net::kInvPossessions, net::kSlotGeneral10));
    CHECK(at(parse_item_notify("cursor leftmouseup"), net::kInvPossessions, net::kSlotCursor));
    CHECK(at(parse_item_notify("loot1 leftmouseup"), net::kInvCorpse, net::kCorpseSlotBegin));
    CHECK(at(parse_item_notify("loot3 rightmouseup"), net::kInvCorpse, net::kCorpseSlotBegin + 2));
    CHECK(at(parse_item_notify("bank2 leftmouseup"), net::kInvBank, 1));
    CHECK(at(parse_item_notify("sharedbank2 leftmouseup"), net::kInvSharedBank, 1));
    CHECK(at(parse_item_notify("trade1 leftmouseup"), net::kInvTrade, 0));
    CHECK(at(parse_item_notify("enviro3 leftmouseup"), net::kInvWorld, 2));
    CHECK(parse_item_notify("pack1 rightmouseup").notification == ItemNotification::RightMouseUp);
    CHECK(parse_item_notify("pack1 rightmouseheld").notification == ItemNotification::RightMouseHeld);
    CHECK(parse_item_notify("pack1 LEFTMOUSEUP").notification == ItemNotification::LeftMouseUp);
}

TEST_CASE("itemnotify: in a bag, 1-based") {
    CHECK(at(parse_item_notify("in pack3 5 leftmouseup"), net::kInvPossessions, net::kSlotGeneral1 + 2, 4));
    CHECK(at(parse_item_notify("in pack10 1 leftmouseup"), net::kInvPossessions, net::kSlotGeneral10, 0));
    CHECK(at(parse_item_notify("in bank4 2 leftmouseup"), net::kInvBank, 3, 1));
    CHECK(at(parse_item_notify("in sharedbank1 3 rightmouseup"), net::kInvSharedBank, 0, 2));
    // Out of range, not a bag place, a missing word.
    CHECK(parse_item_notify("in pack11 1 leftmouseup").error == "Could not find slot to send notification to pack11 1");
    CHECK(parse_item_notify("in pack1 0 leftmouseup").error == "Could not find slot to send notification to pack1 0");
    CHECK(parse_item_notify("in chest 1 leftmouseup").error == "Could not find slot to send notification to chest 1");
    CHECK(parse_item_notify("in pack1 1").error == "Syntax: /itemnotify in <bag slot> <slot # in bag> <notification>");
}

TEST_CASE("itemnotify: an item by name or id; quotes; errors") {
    auto byName = parse_item_notify("\"Fine Steel Long Sword\" leftmouseup");
    CHECK(byName.error.empty());
    CHECK_FALSE(byName.slot);
    CHECK(byName.item_name == "Fine Steel Long Sword");
    auto byId = parse_item_notify("#13010 rightmouseheld");
    CHECK(byId.item_id == 13010u);
    CHECK(byId.notification == ItemNotification::RightMouseHeld);
    // The slot quoted, as E3 sends ${Me.Inventory[...]}'s expansion.
    CHECK(at(parse_item_notify("\"pack2\" leftmouseup"), net::kInvPossessions, net::kSlotGeneral1 + 1));
    CHECK(parse_item_notify("pack1").error.rfind("Syntax: /itemnotify", 0) == 0);
    CHECK(parse_item_notify("").error.rfind("Syntax: /itemnotify", 0) == 0);
    CHECK(parse_item_notify("pack1 doubleclick").error == "Could not send notification to pack1 doubleclick");
    CHECK(parse_item_notify("34 leftmouseup").error == "Invalid item slot '34'");
    CHECK(parse_item_notify("#0 leftmouseup").error == "Invalid item slot '#0'");
}
