#include "audio/individual_sounds.h"

#include <algorithm>
#include <iterator>

namespace hailstone::audio {

namespace {

constexpr IndividualSound kSounds[] = {
    {SoundGroup::OwnFootsteps, "own_footsteps", "Own footsteps",
     "The steps, jumps and landings of your own character."},
    {SoundGroup::PetFootsteps, "pet_footsteps", "Pet footsteps",
     "The steps of pets (an NPC that has an owner)."},
    {SoundGroup::OtherFootsteps, "other_footsteps", "Other footsteps",
     "The steps of other NPCs and other players."},
    {SoundGroup::OpenBag, "open_bag", "Opening a bag", "The sound of a bag opening."},
    {SoundGroup::OpenBank, "open_bank", "Opening a bank bag",
     "The sound of a bag opening when it sits in a bank slot."},
    {SoundGroup::Money, "money", "Money", "The coin sound of buying, selling, looting and moving coin."},
    {SoundGroup::Thunder, "thunder", "Thunder", "The thunder of a storm."},
    {SoundGroup::Equip, "equip", "Equipping items", "The sound of putting on or taking off an item."},
    {SoundGroup::CreatureAmbient, "creature_ambient", "Creature ambient sounds",
     "The sounds some creatures make all the time, such as a spectre's hum."},
};

} // namespace

std::span<const IndividualSound> individual_sounds() { return kSounds; }

const IndividualSound* find_individual_sound(std::string_view name) {
    auto it = std::ranges::find(kSounds, name, &IndividualSound::name);
    return it == std::end(kSounds) ? nullptr : &*it;
}

const IndividualSound* find_individual_sound(SoundGroup group) {
    auto it = std::ranges::find(kSounds, group, &IndividualSound::group);
    return it == std::end(kSounds) ? nullptr : &*it;
}

} // namespace hailstone::audio
