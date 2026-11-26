#pragma once

// A map the server fills by keys of its own choosing (a tribute id, a trader id) holds at most `cap` keys: a value for
// a key already there replaces it, a new key past the cap is dropped. The caps are generous, well above anything a
// server legitimately sends, so that only a flood of made-up keys meets them.

#include <cstddef>
#include <utility>

namespace hailstone::core {

// Sets m[key] = value unless that would add a key past `cap`. True when stored.
template <class Map, class Key, class Value>
bool assign_capped(Map& m, const Key& key, Value&& value, std::size_t cap) {
    if (auto it = m.find(key); it != m.end()) {
        it->second = std::forward<Value>(value);
        return true;
    }
    if (m.size() >= cap) return false;
    m.emplace(key, std::forward<Value>(value));
    return true;
}

} // namespace hailstone::core
