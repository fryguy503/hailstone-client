#include "game/accessibility.h"

#include <algorithm>

#include "core/config.h"
#include "game/combat.h"

namespace hailstone::game {
namespace {

constexpr std::array<std::string_view, kColourVisionModes> kKeys = {"off", "deuteranopia", "protanopia", "tritanopia"};
constexpr std::array<std::string_view, kColourVisionModes> kLabels = {
    "Off", "Deuteranopia (red-green)", "Protanopia (red-green)", "Tritanopia (blue-yellow)"};

ColourVision to_mode(int v) {
    return v >= 0 && v < kColourVisionModes ? static_cast<ColourVision>(v) : ColourVision::Off;
}
int clamp_effects(int v) { return std::clamp(v, 0, kScreenEffectsFull); }

Accessibility g_accessibility;

} // namespace

int clamp_min_text_size(int points) {
    return points <= kMinTextSizeOff ? kMinTextSizeOff : std::clamp(points, kMinTextSizeLow, kMinTextSizeHigh);
}

std::string_view colour_vision_key(ColourVision mode) { return kKeys[static_cast<std::size_t>(to_mode(static_cast<int>(mode)))]; }
std::string_view colour_vision_label(ColourVision mode) {
    return kLabels[static_cast<std::size_t>(to_mode(static_cast<int>(mode)))];
}

std::optional<ColourVision> parse_colour_vision(std::string_view key) {
    for (std::size_t i = 0; i < kKeys.size(); ++i)
        if (key == kKeys[i]) return static_cast<ColourVision>(i);
    return std::nullopt;
}

Accessibility load_accessibility(const Config& config) {
    Accessibility a;
    a.colour_vision = parse_colour_vision(config.get_string("accessibility.colour_vision", "off")).value_or(ColourVision::Off);
    a.con_labels = config.get_bool("accessibility.con_labels", false);
    a.min_text_size = clamp_min_text_size(config.get_int("accessibility.min_text_size", kMinTextSizeOff));
    a.screen_effects = clamp_effects(config.get_int("accessibility.screen_effects", kScreenEffectsFull));
    return a;
}

void store_accessibility(const Accessibility& a, Config& config) {
    config.set("accessibility.colour_vision", colour_vision_key(a.colour_vision));
    config.set_bool("accessibility.con_labels", a.con_labels);
    config.set_int("accessibility.min_text_size", a.min_text_size);
    config.set_int("accessibility.screen_effects", a.screen_effects);
}

std::optional<int> accessibility_option(const Accessibility& a, std::string_view name) {
    if (name == "colour_vision") return static_cast<int>(a.colour_vision);
    if (name == "con_labels") return a.con_labels ? 1 : 0;
    if (name == "min_text_size") return a.min_text_size;
    if (name == "screen_effects") return a.screen_effects;
    return std::nullopt;
}

bool set_accessibility_option(Accessibility& a, std::string_view name, int v) {
    if (name == "colour_vision") a.colour_vision = to_mode(std::clamp(v, 0, kColourVisionModes - 1));
    else if (name == "con_labels") a.con_labels = v != 0;
    else if (name == "min_text_size") a.min_text_size = clamp_min_text_size(v);
    else if (name == "screen_effects") a.screen_effects = clamp_effects(v);
    else return false;
    return true;
}

void set_accessibility(const Accessibility& a) { g_accessibility = a; }
const Accessibility& accessibility() { return g_accessibility; }

std::uint32_t con_rgb(Con con, ColourVision mode) {
    // Index: gray, green, light blue, dark blue, white, yellow, red.
    static constexpr std::array<std::uint32_t, 7> kOff = {0x9a9a9a, 0x00ee00, 0x00e0ff, 0x4060ff, 0xffffff, 0xffff00, 0xff2020};
    // Okabe & Ito 2008: bluish green, sky blue, blue, yellow, vermillion.
    static constexpr std::array<std::uint32_t, 7> kRedGreen = {0x9a9a9a, 0x009e73, 0x56b4e9, 0x0072b2, 0xffffff, 0xf0e442, 0xd55e00};
    // Tol's vibrant: grey, teal, cyan, blue, orange, red.
    static constexpr std::array<std::uint32_t, 7> kBlueYellow = {0xbbbbbb, 0x009988, 0x33bbee, 0x0077bb, 0xffffff, 0xee7733, 0xcc3311};
    if (con == Con::None || static_cast<int>(con) > 7) return 0xc0c0c0;
    const auto i = static_cast<std::size_t>(static_cast<int>(con) - 1);
    switch (mode) {
    case ColourVision::Deuteranopia:
    case ColourVision::Protanopia: return kRedGreen[i];
    case ColourVision::Tritanopia: return kBlueYellow[i];
    default: return kOff[i];
    }
}

std::string_view con_colour_name(Con con) {
    switch (con) {
    case Con::Gray: return "Gray";
    case Con::Green: return "Green";
    case Con::LightBlue: return "Light Blue";
    case Con::DarkBlue: return "Blue";
    case Con::White: return "White";
    case Con::Yellow: return "Yellow";
    case Con::Red: return "Red";
    default: return "";
    }
}

float screen_effect_scale(int percent) { return static_cast<float>(clamp_effects(percent)) / 100.0f; }

glm::vec3 scaled_shake(const glm::vec3& still, const glm::vec3& shaken, int percent) {
    return still + (shaken - still) * screen_effect_scale(percent);
}

} // namespace hailstone::game
