#pragma once

// The game-side parsers in tests/fuzz_decoders.inc (HAILSTONE_FUZZ_GAME_DECODER) take the bytes as a span or text.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "game/bank_snapshot.h"
#include "game/barter.h"
#include "game/character_import.h"
#include "game/missiles.h"
#include "game/spells.h"
#include "game/zoning.h"

namespace hailstone::game {

// spell_text_hit_report over the bytes as text, on the NonMelee type (283) that takes both hit and heal forms.
inline auto fuzz_spell_text_hit_report(std::string_view text) { return spell_text_hit_report(text, 283); }

inline auto fuzz_character_import(std::string_view text) {
    static const HotButtonStore destination;
    HotButtonStore staged;
    return plan_character_import(text, destination, {.socials = true, .hot_buttons = true}, staged);
}

} // namespace hailstone::game

namespace hailstone::fuzz_game {

template <typename R> void feed(R (*fn)(std::span<const std::byte>), std::span<const std::byte> d) { (void)fn(d); }

template <typename R> void feed(R (*fn)(std::span<const std::uint8_t>), std::span<const std::byte> d) {
    (void)fn(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(d.data()), d.size()));
}

template <typename R> void feed(R (*fn)(std::string_view), std::span<const std::byte> d) {
    (void)fn(std::string_view(reinterpret_cast<const char*>(d.data()), d.size()));
}

} // namespace hailstone::fuzz_game
