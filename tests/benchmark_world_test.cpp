#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

#include "app/benchmark_world.h"

using namespace hailstone;

namespace {
net::ServerInfo benchmark_server(std::uint32_t id, std::string name, std::int32_t status = 0) {
    net::ServerInfo world;
    world.id = id;
    world.name = std::move(name);
    world.status = status;
    return world;
}
} // namespace

TEST_CASE("Benchmark world: an absent world never selects the first world") {
    const std::vector worlds{benchmark_server(1, "Other World"), benchmark_server(2, "Development World")};
    CHECK_FALSE(app::benchmark_world_id(worlds, "Missing World"));
    CHECK_FALSE(app::benchmark_world_id(worlds, ""));
    CHECK_FALSE(app::benchmark_world_id({}, "Development World"));
    CHECK(app::benchmark_world_id(worlds, "Development World") == 2);
    CHECK(app::benchmark_world_id(worlds, saved_character_key_part("Development World")) == 2);
}

TEST_CASE("Benchmark world: duplicate names and colliding saved keys are ambiguous") {
    const std::vector worlds{benchmark_server(1, "Development World"), benchmark_server(2, "Development World")};
    CHECK_FALSE(app::benchmark_world_id(worlds, "Development World"));
    CHECK_FALSE(app::benchmark_world_id(worlds, saved_character_key_part("Development World")));
    const std::vector duplicate_ids{benchmark_server(1, "Other World"), benchmark_server(1, "Development World")};
    CHECK_FALSE(app::benchmark_world_id(duplicate_ids, "Development World"));
}

TEST_CASE("Benchmark world: offline world waits, locked GM world can be requested") {
    CHECK_FALSE(app::benchmark_world_id(std::vector{benchmark_server(1, "Development", 1)}, "Development"));
    CHECK_FALSE(app::benchmark_world_id(std::vector{benchmark_server(1, "Development", 5)}, "Development"));
    CHECK(app::benchmark_world_id(std::vector{benchmark_server(1, "Development", 4)}, "Development") == 1);
}
