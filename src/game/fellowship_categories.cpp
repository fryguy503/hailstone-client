#include "game/fellowship_categories.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <string_view>

#include "core/byte_io.h"
#include "core/game_dir.h"

namespace hailstone::game {

namespace {
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
}

std::vector<FellowshipCategory> parse_fellowship_categories(std::span<const std::byte> bytes) {
    if (bytes.size() > kMaxFellowshipCategoryBytes) return {};
    ByteReader reader(bytes);
    const std::string text = reader.fixed_string(bytes.size());
    if (text.size() != bytes.size()) return {}; // text files do not contain embedded NULs
    std::vector<FellowshipCategory> result;
    std::string_view rest(text);
    while (!rest.empty()) {
        const auto end = rest.find('\n');
        auto line = trim(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        if (line.empty() || line.front() == '#') continue;
        const auto separator = line.find('^');
        if (separator == std::string_view::npos) continue;
        const auto number = trim(line.substr(0, separator));
        const auto name = trim(line.substr(separator + 1));
        std::uint32_t id = 0;
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || id == 0 || id >= 32 ||
            name.empty() || name.size() > 255 || name.find('^') != std::string_view::npos ||
            std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; })) continue;
        if (std::any_of(result.begin(), result.end(), [id](const auto& category) { return category.id == id; })) continue;
        result.push_back({id, std::string(name)});
    }
    return result;
}

std::vector<FellowshipCategory> load_fellowship_categories(const GameDir& game) {
    const auto path = game.find("Resources/campsitecategories.txt");
    if (!path) return {};
    std::ifstream file(*path, std::ios::binary);
    if (!file) return {};
    // Read at most one byte beyond the cap, including when the file grows while
    // it is being read. A server overlay cannot trigger an unbounded allocation.
    std::vector<std::byte> bytes(kMaxFellowshipCategoryBytes + 1);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    const auto size = static_cast<std::size_t>(file.gcount());
    if (size > kMaxFellowshipCategoryBytes || file.bad()) return {};
    bytes.resize(size);
    return parse_fellowship_categories(bytes);
}

} // namespace hailstone::game
