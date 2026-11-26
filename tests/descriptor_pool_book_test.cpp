// ui/descriptor_pool_book.h: which ImGui texture pool a set is asked from, and when an empty one goes.

#include <doctest/doctest.h>

#include "ui/descriptor_pool_book.h"

using hailstone::ui::DescriptorPoolBook;

TEST_CASE("Descriptor pools: a full pool is not asked; the newest is asked first") {
    DescriptorPoolBook<int> book(2);
    CHECK(book.with_room().empty());
    book.added(1);
    book.allocated(1);
    book.allocated(1);
    CHECK(book.with_room().empty()); // full: a new pool is made instead of asking this one
    book.added(2);
    book.allocated(2);
    CHECK(book.with_room() == std::vector<int>{2});
    // A set freed from the older pool gives it room again, behind the newest.
    CHECK_FALSE(book.freed(1).has_value());
    CHECK(book.with_room() == std::vector<int>{2, 1});
    CHECK(book.live(1) == 1);
    CHECK(book.live(2) == 1);
}

TEST_CASE("Descriptor pools: an emptied pool is destroyed unless it is the newest") {
    DescriptorPoolBook<int> book(1);
    book.added(1);
    book.allocated(1);
    book.added(2);
    book.allocated(2);
    // The newest emptied: kept for the next texture.
    CHECK_FALSE(book.freed(2).has_value());
    CHECK(book.pool_count() == 2);
    CHECK(book.with_room() == std::vector<int>{2});
    // An older one emptied: destroyed, and gone from the book.
    CHECK(book.freed(1) == std::optional<int>{1});
    CHECK(book.pool_count() == 1);
    CHECK(book.live(1) == 0);
    // A set made and freed over and over in the newest pool keeps it.
    for (int i = 0; i < 3; ++i) {
        book.allocated(2);
        CHECK_FALSE(book.freed(2).has_value());
    }
    CHECK(book.pool_count() == 1);
}

TEST_CASE("Descriptor pools: a free for an unknown or already empty pool does nothing") {
    DescriptorPoolBook<int> book(4);
    CHECK_FALSE(book.freed(7).has_value());
    book.added(1);
    CHECK_FALSE(book.freed(1).has_value());
    CHECK(book.live(1) == 0);
    CHECK(book.pool_count() == 1);
}
