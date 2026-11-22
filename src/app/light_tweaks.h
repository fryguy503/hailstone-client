#pragma once

// Hailstone's Light Tweaks presets (the Options window's Lighting page; docs/graphics.md "Light tweaks"): one choice
// that sets the modern lighting's darkness and light sliders together. "Default" is each slider's default; a preset
// is a row of kLightTweakPresets, so a new one is one line here. The sliders that match no preset show as Custom.

#include <array>
#include <cstddef>
#include <string_view>

namespace hailstone::app {

// The sliders a preset sets, as the display.* keys save them (percent, or units for the distance).
struct LightTweaks {
    int ambient_light = 100;          // display.ambient_light
    int baked_lamp_light = 100;       // display.baked_lamp_light
    int lamp_light_distance = 600;    // display.lamp_light_distance (render::kLampLightDistance)
    int carried_light = 100;          // display.carried_light
    int carried_light_reach = 100;    // display.carried_light_reach
    int carried_light_stacking = 0;   // display.carried_light_stacking: the brightest carried light alone
    int infravision_strength = 100;   // display.infravision_strength
    int ultravision_strength = 100;   // display.ultravision_strength
    bool operator==(const LightTweaks&) const = default;
};

struct LightTweakPreset {
    std::string_view name;
    LightTweaks values;
};

// The presets, in the dropdown's order; the first is the defaults.
inline constexpr std::array<LightTweakPreset, 2> kLightTweakPresets{{
    {"Default", LightTweaks{}},
    {"Option 1", LightTweaks{40, 300, 700, 140, 80, 0, 10, 10}},
}};

// The dropdown's entry for `values`: the first preset they match, else kLightTweakPresets.size() (Custom).
inline std::size_t light_tweak_preset(const LightTweaks& values) {
    for (std::size_t i = 0; i < kLightTweakPresets.size(); ++i)
        if (kLightTweakPresets[i].values == values) return i;
    return kLightTweakPresets.size();
}

} // namespace hailstone::app
