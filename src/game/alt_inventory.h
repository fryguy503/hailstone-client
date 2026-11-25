#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "platform/shared_file.h"

namespace hailstone::game {

class Inventory;
struct BankSnapshot;

struct AltInventoryItem {
    std::string name, bag;
    std::uint32_t id = 0, icon = 0, count = 0;
    std::int16_t slot = 0, sub = -1;
    bool operator==(const AltInventoryItem&) const = default;
};

struct AltInventoryArea {
    std::int64_t seen = 0; // Unix seconds, zero until the server supplied this area
    std::vector<AltInventoryItem> items;
};

struct AltInventoryCharacter {
    enum class Session { Local, Boxed, Offline };
    std::string name;
    AltInventoryArea carried, bank;
    Session session = Session::Offline;
    bool bank_live = false;
};

// A read-only search index enabled by an inventory UI. Only this character is written, in the same saved profile
// and world as its other character files. Files contain search display fields, never account or item instance IDs.
// The native lock permits one writer per character and releases on camp or crash. Freshness also expires when a
// window goes unresponsive or stops publishing. Cached rows never enter the live inventory.
class AltInventoryIndex {
public:
    AltInventoryIndex(std::filesystem::path characters, std::string world, std::string character);

    bool matches(const std::filesystem::path& characters, const std::string& world, const std::string& character) const;
    void update(const Inventory& inventory, const BankSnapshot& last_bank, bool bank_live, std::int64_t now);
    std::vector<AltInventoryCharacter> characters(std::int64_t now) const;

private:
    std::filesystem::path characters_, directory_, snapshot_, lock_file_;
    std::string world_;
    std::optional<platform::ScopedFileLock> lock_;
    AltInventoryCharacter local_;
    std::int64_t last_write_ = 0;
    bool pending_ = true;
};

} // namespace hailstone::game
