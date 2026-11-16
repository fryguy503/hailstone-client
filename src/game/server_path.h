#pragma once

// Local ownership of an unkeyed OP_FindPersonRequest reply. These values never go on the wire and grant no
// authority: callers must still admit their destination. A replacement goal cannot consume an earlier reply.
#include <cstdint>
#include <memory>
#include <utility>

namespace hailstone::game {
struct ServerPathOrigin {
    enum class Purpose { ManualNavigation, NpcFind };
    std::uint64_t owner = 0;
    std::uint64_t generation = 0;
    Purpose purpose = Purpose::ManualNavigation;
    std::weak_ptr<const bool> lifetime;
    bool has_lifetime = false;
    ServerPathOrigin(std::uint64_t source_owner = 0, std::uint64_t goal = 0,
                     Purpose intent = Purpose::ManualNavigation, std::weak_ptr<const bool> source_lifetime = {},
                     bool check_lifetime = false)
        : owner(source_owner), generation(goal), purpose(intent), lifetime(std::move(source_lifetime)),
          has_lifetime(check_lifetime) {}
};
struct ServerPathLease {
    std::uint64_t context = 0;
    std::uint64_t owner = 0;
    std::uint64_t generation = 0;
    std::uint64_t request = 0;
    explicit operator bool() const { return request != 0; }
    bool operator==(const ServerPathLease&) const = default;
};
} // namespace hailstone::game
