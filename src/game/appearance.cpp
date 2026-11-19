#include "game/appearance.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <unordered_set>

namespace hailstone::game {
    // PlayerZoneClient::IsInvisible @0x5a1850 (rof2decomp spawn_visibility.cpp). The spawn itself is invisible only
    // to a GM (virtual +0xdc, PlayerClient_IsGM @0x597f40) whose hide mode (+0x338) is 3001; corpses (type >= 2) and
    // spawns at 0 HP never are; PlayerBase::HasProperty(0x64) (body type 100, or 100 in the property list) always is.
    bool spawn_invisible(const Spawn& viewer, const Spawn& target, std::int32_t see_invisible_rank, bool group_member) {
        if (viewer.id == target.id) return viewer.gm && target.invis_rank == 3001;
        if (target.is_corpse || target.hp_percent == 0) return false;
        if (target.body_type == 100 || std::find(target.properties.begin(), target.properties.end(), 100u) != target.properties.end())
            return true;
        if (target.invis_rank <= 0) return false;
        if (see_invisible_rank >= target.invis_rank) return target.gm && !viewer.gm;
        // PcClient's awareness override (0x5801d0 / 0x4493c0) checks the
        // player's group, with no equivalent raid exemption.
        return !group_member;
    }

    void apply_name_invisibility(std::string& name, NameInvisibility invisibility) {
        if (name.empty()) return;
        if (invisibility.undead) {
            name.insert(name.begin(), '[');
            name.push_back(']');
        }
        if (invisibility.normal) {
            name.insert(name.begin(), '(');
            name.push_back(')');
        }
    }

	std::uint8_t new_armor_bits(std::uint8_t spawn_type, bool local_player, bool group_member, bool raid_member) {
		if (spawn_type == 1) return 0x10;
		if (spawn_type != 0) return 0;
		std::uint8_t bits = 0x08;
		if (local_player) bits |= 0x01 | 0x02; // IsSelfOrGroupMember counts the local player too
		if (group_member) bits |= 0x02;
		if (raid_member) bits |= 0x04;
		return bits;
	}

	bool appearance_new_armor_filtered(const Appearance& a, std::uint32_t filter) {
		return (filter & a.new_armor_bits) == 0;
	}

	void resolve_new_armor_filter(Appearance& a, std::uint32_t filter) {
		a.new_armor_bits = appearance_new_armor_filtered(a, filter) ? 0 : kNewArmorAllBits;
	}

	bool is_pc_model_race(std::uint32_t race) {
		return race <= 12 || race == 128 || race == 130 || race == 330 || race == 522;
	}

	void normalize_appearance(Appearance& a, std::uint32_t race) {
		if (!is_pc_model_race(race)) return;
		if (a.texture != kNoTexture)
			for (int s = kSlotChest; s <= kSlotFeet; ++s) a.material[static_cast<std::size_t>(s)] = a.texture;
		a.texture = kNoTexture;
	}

	Appearance drawn_appearance(const Appearance& a, std::uint32_t race) {
		Appearance out = a;
		if (!is_pc_model_race(race))
			for (int s = kSlotHead; s <= kSlotFeet; ++s) out.tint[static_cast<std::size_t>(s)] = 0;
		return out;
	}

	bool apply_wear_change(Appearance& a, std::uint8_t slot, std::uint32_t material, std::uint32_t hero_forge,
		std::uint32_t tint, std::uint32_t variation, std::uint32_t elite_material, std::uint32_t new_armor_type) {
		if (slot >= 9) return false;
		if (slot < kSlotPrimary && a.texture != kNoTexture) return false; // fixed body texture
		// material -1 resets the slot (the actor part goes back to its default).
		a.material[slot] = material == 0xffffffffu ? 0 : material;
		a.material_variation[slot] = variation;
		a.elite_material[slot] = elite_material;
		a.tint[slot] = tint;
		a.hero_forge[slot] = hero_forge;
		a.new_armor_type[slot] = new_armor_type;
		return true;
	}

	std::uint32_t effective_material(const Appearance& a, int slot) {
		if (slot < 0 || slot > kSlotFeet) return 0;
		if (a.texture != kNoTexture) return slot == kSlotHead ? a.helm : a.texture;
		auto s = static_cast<std::size_t>(slot);
		if (slot == kSlotHead) {
			if (!a.show_helm) return 0;
			// SwapHead: material 7 is drawn as 2 on old (non new-style) models
			// (3 for Iksar, which this function cannot tell).
			return a.material[0] == 7 ? 2 : a.material[0];
		}
		if ((slot == kSlotArms || slot == kSlotWrists) && is_robe(a.material[kSlotChest])) return a.material[kSlotChest];
		return a.material[s];
	}

	int luclin_armor_slot(const Appearance& a, std::uint32_t race, int slot) {
		if (!is_robe(a.material[kSlotChest])) return slot;
		if (slot == kSlotArms || slot == kSlotWrists || (slot == kSlotLegs && race == 330)) return kSlotChest;
		return slot;
	}

	namespace {

		// The head material a race's 0xf0 helm becomes, by gender, in the order of
		// the male ids. The Barbarian (2) takes 0x219 / 0x212 for 0xf0 and 0xf1.
		struct RaceHelm {
			std::uint32_t race;
			std::uint32_t male;
			std::uint32_t female;
		};
		constexpr RaceHelm kRaceHelms[] = {
			{6, 0x221, 0x21c},  {8, 0x22d, 0x226},   {4, 0x235, 0x231},  {3, 0x23f, 0x23a},
			{12, 0x249, 0x244}, {7, 0x253, 0x24e},   {5, 0x25d, 0x258},  {11, 0x267, 0x262},
			{1, 0x273, 0x26c},  {128, 0x27b, 0x276}, {10, 0x285, 0x280}, {9, 0x28f, 0x28a},
		};

	} // namespace

	std::uint32_t map_head_material(std::uint32_t race, std::uint8_t gender, std::uint32_t id) {
		if (id != 0xf0 && id != 0xf1) return id;
		const bool female = gender != 0;
		if (race == 2) return female ? 0x212 : 0x219;
		for (const RaceHelm& helm : kRaceHelms) {
			if (helm.race != race) continue;
			if (id == 0xf0) return female ? helm.female : helm.male;
			// 0xf1: the Ogre (10) has a helm of its own; the other races take 3.
			return race == 10 ? (female ? 0x281u : 0x286u) : 3u;
		}
		return id;
	}

	HeadModel head_model(std::uint32_t race, std::uint8_t gender, std::uint32_t material, std::uint32_t hero_forge) {
		if (race == 130) { // Vah Shir: helmets are items (IT<n>), else none ("0")
			std::uint32_t m = material;
			bool item;
			if (m > 0x297) {
				item = m >= 0x29a && m <= 0x29c;
			}
			else if (m >= 0x295) {
				item = true;
			}
			else if (m != 0 && (m <= 3 || m == 0xf0)) {
				if (m == 0xf0) m = 3;
				m += gender == 1 ? 0x294 : 0x299;
				item = true;
			}
			else {
				item = false;
			}
			return { 0, item ? m : 0 };
		}
		std::uint32_t v = material;
		bool mapped = false;
		if (v - 0xf0 <= 0xf) {
			v = map_head_material(race, gender, v);
			mapped = true;
		}
		if (v == 7) v = race == 128 ? 3 : 2;
		if (v >= 0xf0 && mapped) return { 0, v }; // IT<v> on the head point, head HE00
		// A material above 0xf1 not mapped names the head by the record's +0x10.
		return { v > 0xf1 ? hero_forge : v, 0 };
	}

	namespace {
		std::atomic<bool> g_extra_shields{ true };
		const std::unordered_set<std::uint32_t> kExtraShields = {
		#include "game/shield_models.inc"
		};
	} // namespace

	void set_extra_shields(bool on) { g_extra_shields = on; }

	bool is_shield_model(std::uint32_t m) {
		if (is_stock_shield_model(m))
		{
			return true;
		}
		if (g_extra_shields)
		{
			if (kExtraShields.contains(m))
			{
				return true;
			}
		}
		return false;
	}

	bool is_stock_shield_model(std::uint32_t m) {
		if (m - 200u <= 28u || m - 11001u <= 19u || m - 10530u <= 14u || m - 10664u <= 7u) return true;
		if (m >= 11496u && m <= 11497u) return true;
		static constexpr std::uint32_t kSingles[] = { 10691, 10729, 10730, 67367, 10738, 10754, 10772, 10775, 10781,
													 10790, 10826, 10827, 10832, 10833, 10849, 10850, 10857, 10858,
													 11356, 11364, 11372, 11391, 11399, 14000, 11443 };
		return std::find(std::begin(kSingles), std::end(kSingles), m) != std::end(kSingles);
	}

} // namespace hailstone::game
