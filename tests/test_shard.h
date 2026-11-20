#pragma once

// HAILSTONE_TEST_SHARD=<k>/<n>: run the k-th of n parts of hailstone_tests (tests/test_main.cpp), so ctest can run the
// parts at once (CMakeLists.txt, HAILSTONE_TEST_SHARDS). The parts are ranges of doctest's list, which is shuffled
// with a fixed seed first so the slow cases (the UI packs', which sit together in name and file order) spread over
// every part instead of all landing in one.

#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>
#include <system_error>

namespace hailstone::test {

struct ShardRange {
    unsigned first = 0; // doctest's --first and --last: 1-based, inclusive, over the cases that pass the filters
    unsigned last = 0;
};

// The range of part `spec` ("k/n", 1 <= k <= n) of `total` cases, or none when `spec` is not one. Each part is at most
// ceil(total / n) long and the last ends at or past `total`, so the parts cover every case once even when fewer pass
// the filters (a case skipped without the game folder) than are registered.
inline std::optional<ShardRange> shard_range(std::string_view spec, std::size_t total) {
    const auto slash = spec.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    unsigned k = 0;
    unsigned n = 0;
    const auto number = [](std::string_view text, unsigned& out) {
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
        return ec == std::errc() && end == text.data() + text.size() && !text.empty();
    };
    if (!number(spec.substr(0, slash), k) || !number(spec.substr(slash + 1), n)) return std::nullopt;
    if (n == 0 || k == 0 || k > n) return std::nullopt;
    const std::size_t per = total < n ? 1 : (total + n - 1) / n; // a part past the end runs nothing
    ShardRange r;
    r.first = static_cast<unsigned>((k - 1) * per + 1);
    r.last = static_cast<unsigned>(k * per);
    return r;
}

} // namespace hailstone::test
