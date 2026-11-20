// SPDX-License-Identifier: GPL-2.0-only
#pragma once

// The string types of dbstr_us.txt (its second column), for EqStrings::db_string, by MacroQuest's names.
//
// Copied from MacroQuest (GPL-2.0, Copyright (C) 2002-present MacroQuest Authors; THIRD_PARTY.md):
// eDatabaseStringType from src/eqlib/include/eqlib/game/EQClasses.h.

#include <cstdint>

namespace hailstone::game {

enum eDatabaseStringType : std::uint32_t {
    eDefaultStringType = 0,
    eAltAbilityName = 1,
    eAltAbilityButton1 = 2,
    eAltAbilityButton2 = 3,
    eAltAbilityDescription = 4,
    eSpellCategory = 5,
    eSpellDescription = 6,
    eItemLoreGroup = 7,
    eRaceDescription = 8,
    eClassDescription = 9,
    eStatDescription = 10,
    eRaceName = 11,
    eRaceNamePlural = 12,
    eClassnamePlural = 13,
    eDeityDescription = 14,
    eStartingCityDescription = 15,
    eAugmentSlot = 16,
    eAltCurrencyName = 17,
    eAltCurrencyNamePlural = 18,
    eAltAbilityCategory = 19,
    eExpansionName = 20,
    eMercenaryCategory = 21,
    eMercenarySubCategory = 22,
    eMercenarySubCategoryDescription = 23,
    eMercenaryStanceName = 24,
    eMercenaryStanceDescriptionLong = 25,
    eMercenaryStanceDescriptionShort = 26,
    eSpellLineName = 27,
    eContextMenu = 28,
    // 29 does not exist
    eAlertName = 30,
    eAlertDescription = 31,
    eZoneBonusDescription = 34,
    // 35 does not exist
    eMercenaryAbilityType = 36,
    eMercenaryAbilityName = 37,
    eMercenaryAbilityDescription = 38,
    eSpellRestrictions = 39,
    eSpellStackingGroup = 40,
    eClaimFeatureDescription = 41,
    eDecal = 42,
    eClickDescription = 43,
    eFactionName = 45,
};

} // namespace hailstone::game
