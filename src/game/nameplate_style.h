#pragma once

// How the name plates' text looks (docs/graphics.md "Name plates"): Hailstone's options, not the original's (its name
// sprites have one fixed font). The face is a font family name, the rest small numbers; the defaults draw the names
// as they were before these options: the UI's face at the UI's size, regular, with a drop shadow, the same size at
// any distance. The renderer (app/nameplate_layer.h) and the font atlas (ui::ImGuiLayer::set_plate_font) read them.

#include <optional>
#include <string>
#include <string_view>

namespace hailstone { class Config; }

namespace hailstone::game {

// The edge drawn around each letter so a name reads over any background.
enum class PlateEdge : int { Shadow = 0, Outline = 1, None = 2 };

inline constexpr int kPlateFontSizeMin = 8, kPlateFontSizeMax = 48, kPlateFontSizeDefault = 15; // points at 100 % UI scale
inline constexpr int kPlateFarScaleMin = 25, kPlateFarScaleMax = 100;  // % of the size at the names' range
inline constexpr int kPlateTagScaleMin = 50, kPlateTagScaleMax = 100;  // % of the name's size

struct PlateStyle {
    std::string face;                      // a font family (ui/font_faces.h); empty: the UI's font
    int size = kPlateFontSizeDefault;
    bool bold = false;
    PlateEdge edge = PlateEdge::Shadow;
    int far_scale = kPlateFarScaleMax;     // a name at the plates' range is drawn at this % of its size; 100: no shrink
    int tag_scale = kPlateTagScaleMax;     // the lines under the name (guild, owner) at this % of the name's size
};

// hailstone.ini display.plate_font_face, display.plate_font_size, display.plate_font_bold, display.plate_edge,
// display.plate_far_scale, display.plate_tag_scale; a value out of range is brought into it.
PlateStyle load_plate_style(const Config& config);
void store_plate_style(const PlateStyle& style, Config& config);

// The numeric options (hs.Option / hs.SetOption): plate_font_size, plate_font_bold, plate_edge, plate_far_scale,
// plate_tag_scale. nullopt / false for another name; a value set out of range is clamped. The face is the client's
// (its list of families is the system's).
std::optional<int> plate_style_option(const PlateStyle& style, std::string_view name);
bool set_plate_style_option(PlateStyle& style, std::string_view name, int value);

// The size factor of a name `distance` from the camera: 1 up close, falling evenly to far_scale % at `range` and
// staying there beyond it.
float plate_distance_factor(float distance, float range, int far_scale);

} // namespace hailstone::game
