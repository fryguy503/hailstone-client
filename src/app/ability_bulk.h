#pragma once

// The Ability Book's usable entries in one pass, for the compiled plugins' get_ability_data_bulk.

#include <vector>

#include "plugin/ability_bulk.h"

namespace hailstone::app {

struct UiHost;

// The entries the Ability Book lists, in its order: the owned activated AAs, the listed disciplines, then the usable
// skills (the combat page's, then the abilities page's). Empty without a connection or a character. A row's id is what
// the bulk read documents (a skill's id, a discipline's spell id, an AA's group id); readiness is the same as
// ability_state's.
std::vector<plugin::AbilityBulkRow> ability_bulk_rows(UiHost& h);

} // namespace hailstone::app
