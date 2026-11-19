#pragma once

#include <cstddef>

struct lua_State;

namespace hailstone::ui::lua {

// The live heap the collector's last finished cycle found, in bytes: LUA_GCCOUNT less the garbage made while that
// cycle ran (which only the next cycle can free). Lua's API has no call for it.
std::size_t gc_live_bytes(lua_State* L);

// Whether the collector has left plain generational mode: after a major collection of Lua's own that freed too little
// (a "bad collection", lgc.c genstep), it sets lastatomic and does its next majors itself (stepgenfull, possibly in
// incremental mode), where the host does not time them. Lua's API has no call for it.
bool gc_left_generational(lua_State* L);

// A full collection that leaves the collector in plain generational mode: LUA_GCCOLLECT (fullgen, which clears
// lastatomic) while it is generational, else LUA_GCGEN, whose switch back (entergen) is itself the full collection,
// so there are never two.
void gc_full_generational(lua_State* L);

} // namespace hailstone::ui::lua
