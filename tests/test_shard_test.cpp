// tests/test_shard.h: HAILSTONE_TEST_SHARD's parts cover every case once.
#include <doctest/doctest.h>

#include <string>

#include "test_shard.h"

using hailstone::test::shard_range;

TEST_CASE("test shards: the parts cover every case once") {
    for (std::size_t total : {0u, 1u, 7u, 8u, 9u, 4176u}) {
        for (unsigned n : {1u, 3u, 8u}) {
            std::size_t next = 1;
            for (unsigned k = 1; k <= n; ++k) {
                const auto r = shard_range(std::to_string(k) + "/" + std::to_string(n), total);
                REQUIRE(r);
                CHECK(r->first == next);
                CHECK(r->last >= r->first);
                next = r->last + 1;
            }
            CHECK(next > total);
        }
    }
}

TEST_CASE("test shards: anything but k/n with 1 <= k <= n is refused") {
    for (const char* bad : {"", "3", "/8", "3/", "0/8", "9/8", "3/0", "a/8", "3/8x", "-1/8", " 3/8"})
        CHECK_FALSE(shard_range(bad, 100));
    CHECK(shard_range("8/8", 100));
}
