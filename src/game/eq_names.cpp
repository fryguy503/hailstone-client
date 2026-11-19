#include "game/eq_names.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace hailstone::game {

namespace {

// Race ids 1-139 in order (index = id - 1).
constexpr std::string_view kRaces[] = {
    "Human", "Barbarian", "Erudite", "Wood Elf", "High Elf", "Dark Elf", "Half Elf", "Dwarf", "Troll", "Ogre",
    "Halfling", "Gnome", "Aviak", "Werewolf", "Brownie", "Centaur", "Golem", "Giant", "Trakanon", "Venril Sathir",
    "Evil Eye", "Beetle", "Kerran", "Fish", "Fairy", "Froglok", "Froglok Ghoul", "Fungusman", "Gargoyle", "Gasbag",
    "Gelatinous Cube", "Ghost", "Ghoul", "Giant Bat", "Giant Eel", "Giant Rat", "Giant Snake", "Giant Spider", "Gnoll",
    "Goblin", "Gorilla", "Wolf", "Bear", "Freeport Guard", "Demi Lich", "Imp", "Griffin", "Kobold", "Lava Dragon",
    "Lion", "Lizard Man", "Mimic", "Minotaur", "Orc", "Human Beggar", "Pixie", "Drachnid", "Solusek Ro",
    "Bloodgill", "Skeleton", "Shark", "Tunare", "Tiger", "Treant", "Vampire", "Statue of Rallos Zek",
    "Highpass Citizen", "Tentacle Terror", "Wisp", "Zombie", "Qeynos Citizen", "Ship", "Launch", "Piranha",
    "Elemental", "Puma", "Neriak Citizen", "Erudite Citizen", "Bixie", "Reanimated Hand", "Rivervale Citizen",
    "Scarecrow", "Skunk", "Snake Elemental", "Spectre", "Sphinx", "Armadillo", "Clockwork Gnome", "Drake",
    "Halas Citizen", "Alligator", "Grobb Citizen", "Oggok Citizen", "Kaladim Citizen", "Cazic Thule", "Cockatrice",
    "Daisy Man", "Elf Vampire", "Denizen", "Dervish", "Efreeti", "Froglok Tadpole", "Phinigel Autropos", "Leech",
    "Swordfish", "Felguard", "Mammoth", "Eye of Zomm", "Wasp", "Mermaid", "Harpy", "Fayguard", "Drixie",
    "Ghost Ship", "Clam", "Sea Horse", "Dwarf Ghost", "Erudite Ghost", "Sabertooth", "Wolf Elemental", "Gorgon",
    "Dragon Skeleton", "Innoruuk", "Unicorn", "Pegasus", "Djinn", "Invisible Man", "Iksar", "Scorpion", "Vah Shir",
    "Sarnak", "Draglock", "Drolvarg", "Mosquito", "Rhinoceros", "Xalgoz", "Kunark Goblin", "Yeti", "Iksar Citizen",
};

// Ids above the dense table that are common in play.
constexpr std::pair<std::uint32_t, std::string_view> kSparseRaces[] = {
    {240, "Teleport Man"},
    {330, "Froglok"},
    {522, "Drakkin"},
};

struct ClassNames {
    std::string_view name;
    std::string_view abbrev;
};

// Player classes 1-16 (index = id - 1).
constexpr ClassNames kClasses[] = {
    {"Warrior", "WAR"},   {"Cleric", "CLR"},      {"Paladin", "PAL"}, {"Ranger", "RNG"},
    {"Shadow Knight", "SHD"}, {"Druid", "DRU"},    {"Monk", "MNK"},    {"Bard", "BRD"},
    {"Rogue", "ROG"},     {"Shaman", "SHM"},      {"Necromancer", "NEC"}, {"Wizard", "WIZ"},
    {"Magician", "MAG"},  {"Enchanter", "ENC"},   {"Beastlord", "BST"}, {"Berserker", "BER"},
};

// NPC-only class ids.
constexpr std::pair<std::uint32_t, std::string_view> kNpcClasses[] = {
    {40, "Banker"},
    {41, "Merchant"},
    {59, "Discord Merchant"},
    {60, "Adventure Recruiter"},
    {61, "Adventure Merchant"},
    {62, "LDoN Treasure"},
    {63, "Tribute Master"},
    {64, "Guild Tribute Master"},
    {66, "Guild Banker"},
    {67, "Norrath's Keepers Merchant"},
    {68, "Dark Reign Merchant"},
    {69, "Fellowship Master"},
    {70, "Alternate Currency Merchant"},
    {71, "Mercenary Liaison"},
};

constexpr std::uint32_t kGmClassFirst = 20; // 20-35: GM trainers for classes 1-16
constexpr std::uint32_t kGmClassLast = 35;

// The server's names (set_class_name_override): each class's owners in the order they last named it, the latest
// last. `any` spares a stock server the lock.
struct ClassNameOverride {
    std::uint64_t owner = 0;
    std::string name, short_name;
};
struct ClassNameOverrides {
    std::mutex mutex;
    std::array<std::vector<ClassNameOverride>, std::size(kClasses)> classes;
    std::atomic<bool> any{false};
    std::atomic<std::uint64_t> revision{1};

    void changed() { // under the lock
        any.store(std::any_of(classes.begin(), classes.end(), [](const auto& v) { return !v.empty(); }),
                  std::memory_order_release);
        revision.fetch_add(1, std::memory_order_acq_rel);
    }
};
ClassNameOverrides& overrides() {
    static ClassNameOverrides o;
    return o;
}

// The latest owner's names for player class `class_id` (1-16), either of which may be empty.
std::optional<std::pair<std::string, std::string>> override_of(std::uint32_t class_id) {
    ClassNameOverrides& o = overrides();
    if (!o.any.load(std::memory_order_acquire) || class_id < 1 || class_id > std::size(kClasses)) return std::nullopt;
    std::lock_guard lock(o.mutex);
    const auto& owners = o.classes[class_id - 1];
    if (owners.empty()) return std::nullopt;
    return std::pair{owners.back().name, owners.back().short_name};
}

bool printable(std::string_view text) {
    return std::none_of(text.begin(), text.end(), [](char ch) {
        const auto b = static_cast<unsigned char>(ch);
        return b < 0x20 || b == 0x7f;
    });
}

} // namespace

std::string race_name(std::uint32_t race_id) {
    if (race_id >= 1 && race_id <= std::size(kRaces)) return std::string(kRaces[race_id - 1]);
    for (const auto& [id, name] : kSparseRaces)
        if (id == race_id) return std::string(name);
    return "Race " + std::to_string(race_id);
}

bool is_playable_race(std::uint32_t race_id) {
    return (race_id >= 1 && race_id <= 12) || race_id == 128 || race_id == 130 || race_id == 330 || race_id == 522;
}

std::string stock_class_name(std::uint32_t class_id) {
    if (class_id >= 1 && class_id <= std::size(kClasses)) return std::string(kClasses[class_id - 1].name);
    if (class_id >= kGmClassFirst && class_id <= kGmClassLast)
        return std::string(kClasses[class_id - kGmClassFirst].name) + " Guildmaster";
    for (const auto& [id, name] : kNpcClasses)
        if (id == class_id) return std::string(name);
    return "Class " + std::to_string(class_id);
}

std::string stock_class_short_name(std::uint32_t class_id) {
    if (class_id >= 1 && class_id <= std::size(kClasses)) return std::string(kClasses[class_id - 1].abbrev);
    return stock_class_name(class_id);
}

std::string class_name(std::uint32_t class_id) {
    if (class_id >= kGmClassFirst && class_id <= kGmClassLast)
        return class_name(class_id - kGmClassFirst + 1) + " Guildmaster";
    if (auto o = override_of(class_id); o && !o->first.empty()) return std::move(o->first);
    return stock_class_name(class_id);
}

std::string class_short_name(std::uint32_t class_id) {
    if (class_id >= 1 && class_id <= std::size(kClasses)) {
        if (auto o = override_of(class_id); o && !o->second.empty()) return std::move(o->second);
        return std::string(kClasses[class_id - 1].abbrev);
    }
    return class_name(class_id);
}

bool set_class_name_override(std::uint64_t owner, std::uint32_t class_id, std::string_view name,
                             std::string_view short_name) {
    if (owner == 0 || class_id < 1 || class_id > std::size(kClasses) || name.size() > kClassNameMax ||
        short_name.size() > kClassShortNameMax || !printable(name) || !printable(short_name))
        return false;
    ClassNameOverrides& o = overrides();
    std::lock_guard lock(o.mutex);
    auto& owners = o.classes[class_id - 1];
    std::erase_if(owners, [owner](const ClassNameOverride& e) { return e.owner == owner; });
    if (!name.empty() || !short_name.empty()) owners.push_back({owner, std::string(name), std::string(short_name)});
    o.changed();
    return true;
}

void clear_class_name_overrides(std::uint64_t owner) {
    ClassNameOverrides& o = overrides();
    if (!o.any.load(std::memory_order_acquire)) return;
    std::lock_guard lock(o.mutex);
    bool erased = false;
    for (auto& owners : o.classes)
        erased |= std::erase_if(owners, [owner](const ClassNameOverride& e) { return e.owner == owner; }) > 0;
    if (erased) o.changed();
}

void clear_class_name_overrides() {
    ClassNameOverrides& o = overrides();
    if (!o.any.load(std::memory_order_acquire)) return;
    std::lock_guard lock(o.mutex);
    for (auto& owners : o.classes) owners.clear();
    o.changed();
}

std::optional<std::string> class_name_override(std::uint32_t class_id, bool short_name) {
    auto o = override_of(class_id);
    if (!o) return std::nullopt;
    std::string& name = short_name ? o->second : o->first;
    if (name.empty()) return std::nullopt;
    return std::move(name);
}

std::uint64_t class_names_revision() { return overrides().revision.load(std::memory_order_acquire); }

std::string class_list_name(std::span<const std::uint32_t> class_ids, bool abbreviate) {
    std::string out;
    for (std::uint32_t id : class_ids) {
        if (id == 0) continue;
        if (!out.empty()) out += abbreviate ? "/" : " / ";
        out += abbreviate ? class_short_name(id) : class_name(id);
    }
    return out.empty() ? std::string("None") : out;
}

std::string class_mask_name(std::uint32_t mask) {
    std::string out;
    for (std::size_t c = 0; c < 16 && c < std::size(kClasses); ++c) {
        if (!((mask >> c) & 1u)) continue;
        const std::string short_name = class_short_name(static_cast<std::uint32_t>(c + 1));
        std::string_view a = short_name; // "WAR" -> "War"
        if (!out.empty()) out += '/';
        out += a.substr(0, 1);
        for (char ch : a.substr(1)) out += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    }
    return out;
}

std::uint32_t language_eqstr(std::uint32_t lang) {
    // The language names' eqstr ids come in runs: languages first .. first + count - 1 say id .. id + count - 1.
    struct Run {
        std::uint32_t first, id, count;
    };
    static constexpr Run kRuns[] = {
        {0, 3114, 1},  {1, 3200, 9},  {13, 3209, 8}, {10, 3217, 1}, {21, 3218, 1},
        {11, 3219, 2}, {22, 3221, 3}, {25, 7658, 2},
    };
    for (const Run& run : kRuns)
        if (lang >= run.first && lang - run.first < run.count) return run.id + (lang - run.first);
    return 3115;
}

} // namespace hailstone::game
