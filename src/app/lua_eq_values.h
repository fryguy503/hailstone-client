#pragma once

// Labels and gauges by EQType: the original's __GetLabelFromEQ @0x763640 and __GetGaugeValueFromEQ @0x762410 over
// this client's state. The type numbers are the switch values of those functions (the label switch goes through a
// byte table at 0x768850, the gauge switch through 0x7635a8; both indexed by type - 1).

#include <cstdint>
#include <optional>
#include <string>

#include "app/lua_eq.h"

namespace hailstone::app {

struct LabelValue {
    std::string text;
    std::optional<std::uint32_t> argb; // the colour the label is drawn in when the function sets one
};
LabelValue eq_label(const EqHost& host, int type);

// Gauge values as CGaugeWnd::Draw reads them: 0..1000; -1 draws nothing; -2 full in dark red; -3 (a corpse)
// empty; -2000..-1001 drawn pink, -110..-10 purple.
struct GaugeValue {
    int value = -2;
    std::string text;
};
GaugeValue eq_gauge(const EqHost& host, int type);

// Gauge 26 at a supplied frame time: the active discipline's remaining duration, with seconds in its text.
GaugeValue eq_discipline_gauge(const EqHost& host, std::int64_t now_ms);

// How CGaugeWnd::Draw @0x66fc30 fills a gauge value: 0..1000 as it is (no tint of its own); -2 full in dark red
// (0xff800000, no lines fill); -110..-10 at (value + 110) * 10 in purple (0xff800080); -2000..-1000 at value + 2000
// in pink (0xffc08080); anything else below 0 (-1, -3, -9) empty (fill -1).
struct GaugeFill {
    int fill = -1;
    std::optional<std::uint32_t> tint;
};
GaugeFill gauge_fill(int value);

// 0x4b37a0: cur / (max * 0.01f), truncated; 0 when max is 0.
int eq_percent(std::int64_t cur, std::int64_t max);
// GetValueCompareColor @0x761b20 / LabelFromEQ_FormatIntValue: grey when equal, green above, red below.
std::uint32_t compare_color(std::int64_t value, std::int64_t base);
// PlayerWnd / gauge text of a name: CEverQuest::trimName (the display name).
std::string trim_name(const std::string& name);

} // namespace hailstone::app
