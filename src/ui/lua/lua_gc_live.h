#pragma once

#include <cstddef>

struct lua_State;

namespace hailstone::ui::lua {

// The live heap the collector's last finished cycle found, in bytes: LUA_GCCOUNT less the garbage made while that
// cycle ran (which only the next cycle can free). Lua's API has no call for it.
std::size_t gc_live_bytes(lua_State* L);

// Whether the collector has left generational minor mode: once the objects that minor collections made old pass a
// share of the live heap (LUA_GCPMINORMAJOR, lgc.c checkminormajor), Lua shifts to major mode and runs its majors
// itself, incrementally (KGC_GENMAJOR), where the host does not time them. Lua's API has no call for it.
bool gc_left_generational(lua_State* L);

// A full collection that leaves the collector in generational minor mode: LUA_GCCOLLECT (fullgen) while it is in
// minor mode, else LUA_GCGEN, whose switch (entergen) finishes the cycle under way and then is itself the full
// collection, so no second one is added after it.
void gc_full_generational(lua_State* L);

} // namespace hailstone::ui::lua
