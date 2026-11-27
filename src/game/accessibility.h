#pragma once

// Accessibility options (docs/dev-ui.md "Accessibility"): Hailstone's own presentation settings, not RoF2 rules. The
// defaults leave everything as it was.
//
//   colour_vision   con colours (game::con_rgb: the target, name plates, map, chat, extended targets) from a
//                   colour-blind-safe set; the Lua packs swap their meaning colours (bars, good / bad) to match
//   con_labels      the target's con colour also named in words ("Red"), where the target window shows it
//   min_text_size   no UI text smaller than this many points at 100 % display scale (ui/text_floor.h); 0 off
//   screen_effects  how strongly the full-screen effects show, percent: the lightning's flash, OP_CameraEffect's
//                   shake and drunkenness' field-of-view warp (100 as the game has them, 0 none)

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include <glm/vec3.hpp>

namespace hailstone { class Config; }

namespace hailstone::game {

enum class Con : std::uint8_t;

enum class ColourVision : int { Off = 0, Deuteranopia = 1, Protanopia = 2, Tritanopia = 3 };
inline constexpr int kColourVisionModes = 4;

inline constexpr int kMinTextSizeOff = 0;
inline constexpr int kMinTextSizeLow = 10, kMinTextSizeHigh = 24; // points; anything above 0 is held to this range
inline constexpr int kScreenEffectsFull = 100;

struct Accessibility {
    ColourVision colour_vision = ColourVision::Off;
    bool con_labels = false;
    int min_text_size = kMinTextSizeOff;
    int screen_effects = kScreenEffectsFull;
};

// hailstone.ini accessibility.colour_vision (off, deuteranopia, protanopia, tritanopia), accessibility.con_labels,
// accessibility.min_text_size, accessibility.screen_effects; a value out of range is brought into it, an unknown
// mode name is off.
Accessibility load_accessibility(const Config& config);
void store_accessibility(const Accessibility& a, Config& config);

// The options (hs.Option / hs.SetOption): colour_vision (0..3, option_choices "colour_vision" names them),
// con_labels, min_text_size, screen_effects. nullopt / false for another name; a value set out of range is clamped.
std::optional<int> accessibility_option(const Accessibility& a, std::string_view name);
bool set_accessibility_option(Accessibility& a, std::string_view name, int value);

std::string_view colour_vision_key(ColourVision mode);   // "off", "deuteranopia", ... (the ini's value)
std::string_view colour_vision_label(ColourVision mode); // what the Options windows show
std::optional<ColourVision> parse_colour_vision(std::string_view key);
int clamp_min_text_size(int points);

// The settings in force for this process, set by the app when it loads or changes them. con_rgb(Con) and the HUD
// read them from here, so every caller of the con colours follows one switch.
void set_accessibility(const Accessibility& a);
const Accessibility& accessibility();

// The con colour (0xRRGGBB) for a colour-vision mode. Off is the client's own set (con_rgb). The others:
//   deuteranopia, protanopia  Okabe and Ito's Color Universal Design set (Okabe & Ito 2008): bluish green, sky blue,
//                             blue, yellow and vermillion, with the grey and white kept
//   tritanopia                Paul Tol's "vibrant" qualitative scheme (Tol 2021, SRON/EPS/TN/09-002): teal, cyan,
//                             blue, orange and red, with Tol's grey and white
// Each set is chosen so its colours stay apart for that kind of colour blindness; the order easy to hard keeps the
// cool-to-warm run the stock colours have.
std::uint32_t con_rgb(Con con, ColourVision mode);
// The con colour's name ("Gray", "Green", "Light Blue", "Blue", "White", "Yellow", "Red"), as players say a mob cons;
// "" for Con::None.
std::string_view con_colour_name(Con con);

// screen_effects as a factor, 0..1.
float screen_effect_scale(int percent);
// A shaken camera position with the shake scaled: `shaken` is CameraShake::offset's, `still` the camera without it.
glm::vec3 scaled_shake(const glm::vec3& still, const glm::vec3& shaken, int percent);

} // namespace hailstone::game
