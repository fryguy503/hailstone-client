#include <doctest/doctest.h>

#include <filesystem>

#include "core/atomic_file.h"
#include "core/character_files.h"
#include "game/alt_inventory.h"
#include "game/bank_snapshot.h"
#include "mock_server/harness.h"
#include "scratch_dir.h"

using namespace hailstone;
using namespace hailstone::test;

namespace {

using Session = game::AltInventoryCharacter::Session;
constexpr std::int64_t kNow = 1'795'657'320;

net::ItemSlot bank_slot(int slot, int sub = -1) {
    return {net::kInvBank, static_cast<std::int16_t>(slot), static_cast<std::int16_t>(sub), -1};
}

game::Inventory bags_and_bank(int count) {
    game::Inventory inventory;
    auto bag = make_item(17005, "Large Sack", net::kSlotGeneral1);
    bag.item_class = 1;
    bag.bag_slots = 4;
    REQUIRE(inventory.put(net::ItemSlot::possessions(net::kSlotGeneral1), bag));
    REQUIRE(inventory.put(net::ItemSlot::possessions(net::kSlotGeneral1, 2),
                          make_item(13041, "Spider Silk", net::kSlotGeneral1, count)));
    REQUIRE(inventory.put(bank_slot(5), make_item(13041, "Spider Silk", 5, 8)));
    REQUIRE(inventory.put(net::ItemSlot::possessions(net::kSlotCharm), make_item(2001, "Equipped", 0)));
    REQUIRE(inventory.put(net::ItemSlot::possessions(net::kSlotCursor), make_item(2002, "Cursor", net::kSlotCursor)));
    REQUIRE(inventory.put(net::ItemSlot{net::kInvSharedBank, 0, -1, -1}, make_item(2003, "Shared", 0)));
    return inventory;
}

} // namespace

TEST_CASE("Alt Finder: same-world boxes and offline alts keep per-area freshness and exact slots") {
    ScratchDir scratch("alt_inventory");
    const auto scope = scratch.path() / "profile" / "characters" / "World";
    game::AltInventoryIndex local(scope, "World", "Brannoc");
    const auto inv = bags_and_bank(20);
    local.update(inv, {}, false, kNow);
    {
        game::AltInventoryIndex box(scope, "World", "Mirelle");
        auto boxed_inventory = bags_and_bank(12);
        const auto last_bank = game::bank_snapshot_of(boxed_inventory, kNow - 22 * 60);
        box.update(boxed_inventory, last_bank, false, kNow);
        game::AltInventoryIndex nearby(scope, "World", "Torven");
        nearby.update(bags_and_bank(16), {}, true, kNow);
        auto all = local.characters(kNow);
        REQUIRE(all.size() == 3);
        CHECK(all[0].name == "Brannoc");
        CHECK(all[0].session == Session::Local);
        CHECK(all[0].bank.seen == 0);
        CHECK(all[0].bank.items.empty());
        CHECK(all[1].name == "Mirelle");
        CHECK(all[1].session == Session::Boxed);
        CHECK(all[1].carried.seen == kNow);
        REQUIRE(all[1].carried.items.size() == 2); // bag and stack; worn gear and cursor are excluded
        const auto& stack = all[1].carried.items[1];
        CHECK(stack.name == "Spider Silk");
        CHECK(stack.count == 12);
        CHECK(stack.slot == net::kSlotGeneral1);
        CHECK(stack.sub == 2);
        CHECK(stack.bag == "Large Sack");
        CHECK_FALSE(all[1].bank_live);
        CHECK(all[1].bank.seen == kNow - 22 * 60);
        REQUIRE(all[1].bank.items.size() == 1); // shared bank is excluded
        CHECK(all[1].bank.items[0].slot == 5);
        CHECK(all[1].bank.items[0].sub == -1);
        CHECK(all[1].bank.items[0].count == 8);
        CHECK(all[2].name == "Torven");
        CHECK(all[2].session == Session::Boxed);
        CHECK(all[2].bank_live);
        CHECK(all[2].bank.seen == kNow);

        // A new writer cannot replace a still-running box's inventory for the same character.
        game::AltInventoryIndex reconnect(scope, "World", "Mirelle");
        reconnect.update(bags_and_bank(24), {}, false, kNow + 1);
        CHECK(local.characters(kNow + 1)[1].carried.items[1].count == 12);

        // A direct stack quantity change does not bump the stock inventory revision.
        auto* silk = boxed_inventory.at(net::ItemSlot::possessions(net::kSlotGeneral1, 2));
        REQUIRE(silk);
        silk->stack_count = 7;
        box.update(boxed_inventory, last_bank, false, kNow + 1);
        all = local.characters(kNow + 1);
        CHECK(all[1].carried.items[1].count == 7);
        CHECK(all[1].carried.seen == kNow + 1);
        CHECK(all[1].bank.seen == kNow - 22 * 60);
        box.update(boxed_inventory, last_bank, false, kNow + 6);
        CHECK(local.characters(kNow + 6)[1].carried.seen == kNow + 6);

        // A stopped publisher becomes last-seen even while an unresponsive instance still holds its lock.
        all = local.characters(kNow + 22);
        CHECK(all[1].session == Session::Offline);
        CHECK(all[2].session == Session::Offline);
        CHECK_FALSE(all[2].bank_live);
        CHECK(all[2].bank.seen == kNow);
    }

    // Releasing the locks retires live status immediately, including while the most recent snapshots are fresh.
    const auto offline = local.characters(kNow + 7);
    REQUIRE(offline.size() == 3);
    CHECK(offline[1].session == Session::Offline);
    CHECK(offline[1].carried.items[1].count == 7);
    CHECK(offline[2].session == Session::Offline);
    CHECK_FALSE(offline[2].bank_live);

    game::AltInventoryIndex other_profile(scratch.path() / "other" / "characters" / "World", "World", "Brannoc");
    game::AltInventoryIndex other_world(scope.parent_path() / "Elsewhere", "Elsewhere", "Brannoc");
    game::AltInventoryIndex different_world_same_folder(scope, "World?", "Other");
    CHECK(other_profile.characters(kNow).size() == 1);
    CHECK(other_world.characters(kNow).size() == 1);
    CHECK(different_world_same_folder.characters(kNow).size() == 1);
}

TEST_CASE("Alt Finder: uses legacy banks, distinguishes empty from never seen, and ignores corrupt indexes") {
    ScratchDir scratch("alt_inventory_legacy");
    const auto scope = scratch.path() / "World";
    const auto other = scratch.path() / "Not World";
    game::AltInventoryIndex local(scope, "World", "Brannoc");
    game::AltInventoryIndex boxed(scope, "World", "Mirelle");
    const auto old_bank = game::bank_snapshot_of(bags_and_bank(20), kNow - 300);
    const CharacterFiles old{scope / "Selka"};
    const CharacterFiles empty{scope / "Durnik"};
    const CharacterFiles foreign{other / "Unrelated"};
    REQUIRE(write_file_atomic(old.bank_snapshot(), game::encode_bank_snapshot(old_bank)));
    REQUIRE(write_file_atomic(empty.bank_snapshot(), game::encode_bank_snapshot({kNow - 100, {}})));
    REQUIRE(write_file_atomic(foreign.bank_snapshot(), game::encode_bank_snapshot(old_bank)));
    local.update(bags_and_bank(20), {}, false, kNow);
    boxed.update(bags_and_bank(12), old_bank, false, kNow);
    const CharacterFiles newer{scope / "Mirelle"};
    REQUIRE(write_file_atomic(newer.bank_snapshot(), game::encode_bank_snapshot({kNow - 50, {}})));
    REQUIRE(write_file_atomic(scope / ".alt" / "Broken.dat", "HSAI"));

    auto visible = local.characters(kNow);
    REQUIRE(visible.size() == 4);
    CHECK(visible[0].bank.seen == 0);
    CHECK(visible[1].session == Session::Boxed);
    CHECK(visible[1].bank.seen == kNow - 50);
    CHECK(visible[1].bank.items.empty());
    CHECK(visible[2].name == "Durnik");
    CHECK(visible[2].bank.seen == kNow - 100);
    CHECK(visible[2].bank.items.empty());
    CHECK(visible[3].name == "Selka");
    CHECK(visible[3].carried.seen == 0);
    CHECK(visible[3].bank.seen == kNow - 300);
    REQUIRE(visible[3].bank.items.size() == 1);
    CHECK(visible[3].bank.items[0].name == "Spider Silk");

    // A valid, empty live personal bank wins over a saved bank file. Zoning keeps that known empty state.
    boxed.update({}, old_bank, true, kNow + 1);
    visible = local.characters(kNow + 1);
    CHECK(visible[1].bank_live);
    CHECK(visible[1].bank.seen == kNow + 1);
    CHECK(visible[1].bank.items.empty());
    boxed.update({}, old_bank, false, kNow + 2);
    visible = local.characters(kNow + 2);
    CHECK_FALSE(visible[1].bank_live);
    CHECK(visible[1].bank.seen == kNow + 1);
    CHECK(visible[1].bank.items.empty());
}
