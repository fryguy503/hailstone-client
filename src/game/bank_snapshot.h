#pragma once

// Hailstone: the bank as last seen. EQEmu's Inventory:LazyLoadBank rule (on by default) leaves the bank and shared
// bank out of OP_CharInventory and sends them, slot by slot, only once per zone and only when a banker comes near
// (Client::Process, Mob::IsCloseToBanker), so at most zone-ins the client does not know what is in the bank. The
// Advanced Inventory and hs.FindItems list this copy then, marked as last seen; it is kept apart from the live
// inventory so that nothing that moves, uses, counts or lore-checks items ever sees it. Saved per character in
// bank.dat (docs/layout.md). Not the original's: the original client keeps no copy of the bank.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "game/inventory.h"
#include "net/item_packets.h"

namespace hailstone::game {

struct BankSnapshot {
    std::int64_t seen = 0;          // when it was taken (Unix seconds)
    std::vector<net::Item> items;   // the bank's and shared bank's top-level items, bag contents inside
};

// The bank and shared bank items of `inv`, as a snapshot taken at `seen`.
BankSnapshot bank_snapshot_of(const Inventory& inv, std::int64_t seen);

// bank.dat: "HSBK", u32 version (1), i64 seen, u32 count, then each item in SerializeItem's format (net::write_item).
std::string encode_bank_snapshot(const BankSnapshot& snapshot);
// Nothing for a file that is not one, from a later version, or damaged; items not in a bank or shared bank slot
// are dropped.
std::optional<BankSnapshot> decode_bank_snapshot(std::span<const std::byte> data);

} // namespace hailstone::game
