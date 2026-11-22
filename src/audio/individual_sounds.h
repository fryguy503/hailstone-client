#pragma once

// The "Individual Sounds" volumes (Hailstone, not in the original): a volume of 0..100 for each of a few sounds that
// players find tiresome, on top of audio.effects. hailstone.ini keeps them as audio.<name>; the options window lists
// them by the same names (hs.Option / hs.SetOption). The categories are audio's own: app/ decides which event is in
// which one (app/sound_events.cpp) and what the Test button plays (app::test_wave).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace hailstone::audio {

inline constexpr int kSilentPercent = 0;
inline constexpr int kFullVolumePercent = 100;

enum class SoundGroup : std::uint8_t {
    None = 0,       // any other sound: always at full volume
    OwnFootsteps,   // the local player's steps, jumps and landings
    PetFootsteps,   // the steps of a pet (an NPC with an owner)
    OtherFootsteps, // the steps of other NPCs and players
    OpenBag,        // a bag opening
    OpenBank,       // a bag in a bank slot opening
    Money,          // the coin sound of a sale, purchase, loot or move of coin
    Thunder,        // the thunder of a storm
    Equip,          // an item put on or taken off
    CreatureAmbient, // the sounds some creatures make all the time (spectres, undead): their ambient and idle loops
    Count,          // not a group: the number of them
};
inline constexpr std::size_t kSoundGroupCount = static_cast<std::size_t>(SoundGroup::Count);

// One volume for each SoundGroup, 100 until set. The group None has one too, and it stays 100.
struct GroupVolumes {
    std::array<int, kSoundGroupCount> percent;
    GroupVolumes() { percent.fill(kFullVolumePercent); }
    int& operator[](SoundGroup g) { return percent[static_cast<std::size_t>(g)]; }
    int operator[](SoundGroup g) const { return percent[static_cast<std::size_t>(g)]; }
};

struct IndividualSound {
    SoundGroup group;
    std::string_view name;    // the option name and the ini key audio.<name>
    std::string_view label;   // what the options window calls it
    std::string_view tip;     // its tooltip
};

// The sounds in the order the options window lists them.
std::span<const IndividualSound> individual_sounds();
const IndividualSound* find_individual_sound(std::string_view name);
const IndividualSound* find_individual_sound(SoundGroup group);

} // namespace hailstone::audio
