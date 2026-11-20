#pragma once

// The numbers the buff windows draw over their buttons (CBuffWindow::PostDraw @0x6426a0), each on a dark
// (0xff404040) box in font 1, at least 10 pixels wide:
//  - the time left, bottom right, white (CBuffWindow_DrawBuffTimeOverlays @0x641ae0);
//  - the disease / poison / curse / corruption counters left, top right, coloured by the counter
//    (CBuffWindow_DrawSpellCounterOverlays @0x641e10 over 0x75ef10);
//  - the hits left of a limited-use buff, bottom left, white (CBuffWindow_DrawEffectCountOverlays @0x642190).
// PostDraw draws each one only when its option is on: the BuffWindow ShowBuffTimer / ShowBuffCounters /
// ShowBuffLimitUse, the ShortDurationBuffWindow ShowSongTimer / ShowSongCounters / ShowSongLimitUse
// (CSpellDisplayManager +0x40..+0x45, eqclient.ini [Defaults]); other windows (the pet's, the target's) draw none.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "game/counter_spas.h"

namespace hailstone::game {

struct SpellData;
struct BuffSlot;

// 0x75ef10 (CSpellDisplayManager): over the local player's effects holding the spell, for each of
// the spell's 12 slots that is a counter effect, the slot's base value is added to `base` and the effect's slot
// data (EQ_Affect_GetSlotDataValue: the counters left) to `left`, in the order corruption (369), disease (35),
// curse (116), poison (36).
struct SpellCounters {
    std::array<std::int32_t, 4> base{};
    std::array<std::int32_t, 4> left{};
};
SpellCounters spell_counters(const SpellData& spell, std::span<const BuffSlot> effects);

struct BuffOverlayText {
    std::string text;
    std::uint32_t argb = 0xffffffff;
};

// CBuffWindow_DrawBuffTimeOverlays: nothing for a spell without a duration formula (EQ_Spell +0x1c) or under a
// second left (permanent buffs included: their time is negative); else SecondsToHMS's largest non-zero part,
// "%dh", "%dm" or "%ds" (truncated: 1 h 59 min is "1h").
std::optional<BuffOverlayText> buff_time_overlay(const SpellData& spell, std::int64_t ms_left);
// CBuffWindow_DrawSpellCounterOverlays: none without a positive base; else, starting from corruption when its base
// is 1..999998 (else 999999), each later counter whose base is positive and below the value picked so far takes
// over with its counters left (the original compares the base with the value); a positive result is drawn,
// corruption 0xffff8c00, disease 0xffffff00, curse 0xffff0000, poison 0xff00ff00.
std::optional<BuffOverlayText> buff_counter_overlay(const SpellCounters& counters);
// CBuffWindow_DrawEffectCountOverlays: for a spell with a hit count (EQ_Spell +0x1a8), the hits left (EQ_Affect
// +0x14) of the last of the local player's effects holding it, when positive; white.
std::optional<BuffOverlayText> buff_limit_overlay(const SpellData& spell, std::span<const BuffSlot> effects);

// CBuffWindow::OnProcessFrame @0x6415f0 (every 50 ms): a buff with less than 19 s left (ms_left / 1000 below 19, more
// than 0) fades out once a second: its button's alpha is the window's times (100 - (now % 1000) / 10) / 100, now the
// frame's time stamp in ms. A bard's own song (BardCastBard) whose duration (SpellDuration(spell, level, 1)) is under
// 4 ticks keeps the window's alpha (`short_song`). The factor (0.01..1) over the window's alpha; 1 otherwise.
double buff_fade(std::int64_t ms_left, std::int64_t now_ms, bool short_song);

} // namespace hailstone::game
