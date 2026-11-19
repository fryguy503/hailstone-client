#include "game/race_models.h"

#include <algorithm>
#include <iterator>
#include <set>

namespace hailstone::game {

namespace {

constexpr RaceModel kRaceModels[] = {
#define HAILSTONE_RACE_MODEL(race, gender, code, flags, fallback) {race, gender, code, flags, fallback},
#define HAILSTONE_ANIM_SOURCE(model, donor)
#include "game/race_models.inc"
#undef HAILSTONE_ANIM_SOURCE
#undef HAILSTONE_RACE_MODEL
};

struct AnimSource {
    std::string_view model;
    std::string_view donor;
};
constexpr AnimSource kAnimSources[] = {
#define HAILSTONE_RACE_MODEL(race, gender, code, flags, fallback)
#define HAILSTONE_ANIM_SOURCE(model, donor) {model, donor},
#include "game/race_models.inc"
#undef HAILSTONE_ANIM_SOURCE
#undef HAILSTONE_RACE_MODEL
};

const RaceModel* exact(std::uint16_t race, std::uint8_t gender) {
    auto it = std::lower_bound(std::begin(kRaceModels), std::end(kRaceModels), std::pair{race, gender},
                               [](const RaceModel& m, std::pair<std::uint16_t, std::uint8_t> v) {
                                   return m.race != v.first ? m.race < v.first : m.gender < v.second;
                               });
    return it != std::end(kRaceModels) && it->race == race && it->gender == gender ? it : nullptr;
}

} // namespace

const RaceModel* find_race_model(std::uint16_t race, std::uint8_t gender) {
    if (const auto* m = exact(race, gender)) return m;
    for (std::uint8_t g : {std::uint8_t{2}, std::uint8_t{0}, std::uint8_t{1}})
        if (const auto* m = exact(race, g)) return m;
    return nullptr;
}

std::string animation_source(std::string_view code, bool tunic_point) {
    if (tunic_point && code.size() == 3) {
        const std::string_view two = code.substr(0, 2);
        if (two == "HU") return {};
        if (two == "ER") return code[2] == 'F' ? "HUF" : "HUM";
        if (two == "BA") return {};
        if (code == "SKE") return "HUM";
    }
    for (const auto& a : kAnimSources)
        if (a.model == code) return std::string(a.donor);
    // GetAlternateAnimTag @0x406a60: a code under one of these prefixes, unless an earlier test named it (those,
    // ERO and BRI among them, are all in the table), takes the wood elf's two letters and keeps its last one, the
    // tunic flag aside (0x4077f0). The table names most of them; race 127's IVM it leaves out.
    static constexpr std::string_view kWoodElfPrefixes[] = {"DA", "HI", "HA", "HH", "QC", "NG", "FP", "EG", "BR",
                                                            "HL", "ZO", "FEM", "FEF", "IV", "GHM", "GHF", "GF", "GE"};
    constexpr std::string_view kWoodElf = "EL";
    constexpr std::size_t kCodeLength = 3;
    if (code.size() == kCodeLength)
        for (std::string_view prefix : kWoodElfPrefixes)
            if (code.starts_with(prefix)) return std::string(kWoodElf) + code.back();
    return {};
}

std::string_view animation_fallback_code(std::string_view code) {
    for (const auto& m : kRaceModels) {
        if (m.code != code) continue;
        if (!(m.flags & kHasAnimationFallback) || m.anim_fallback == 0 || m.anim_fallback == m.race) return {};
        const RaceModel* f = find_race_model(m.anim_fallback, m.gender);
        return f && f->code != code ? f->code : std::string_view{};
    }
    return {};
}

std::vector<std::string_view> model_codes_with_flags(std::uint8_t flags, std::uint8_t exclude) {
    std::vector<std::string_view> out;
    for (const auto& m : kRaceModels)
        if ((m.flags & flags) == flags && (m.flags & exclude) == 0 &&
            std::find(out.begin(), out.end(), m.code) == out.end())
            out.push_back(m.code);
    return out;
}

const RaceModel* find_model_code(std::string_view code) {
    for (const auto& m : kRaceModels)
        if (m.code == code) return &m;
    return nullptr;
}

bool is_mount_race(std::int32_t race) {
    switch (race) {
    case 216: case 472: case 492: case 517: case 582: case 583: case 584: case 594: case 597: case 598: case 623:
    case 625: case 631: case 652: case 654: case 655: case 656: case 657: case 671: case 672: case 673: case 674:
    case 675: case 676: case 677: case 679: case 680: case 682: case 683: case 684: case 685: case 709: case 720:
    case 721: case 732:
        return true;
    default:
        return false;
    }
}

bool is_vehicle_race(std::uint16_t race, std::uint8_t gender) {
    const RaceModel* m = find_race_model(race, gender);
    return m && (m->flags & kVehicleRace);
}

bool is_small_boat_race(std::uint16_t race, std::uint8_t gender) {
    const RaceModel* m = find_race_model(race, gender);
    return m && (m->flags & kVehicleRace) && !(m->flags & kLargeShipRace);
}

std::size_t race_model_race_count() {
    std::set<std::uint16_t> races;
    for (const auto& m : kRaceModels) races.insert(m.race);
    return races.size();
}

} // namespace hailstone::game
