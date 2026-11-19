// The one place the client reads the collector's internals (third_party/lua, pinned): kept in a file of its own so
// Lua's internal macros (G, cast, ...) reach nothing else.

#include "ui/lua/lua_gc_live.h"

extern "C" {
#include "lstate.h"
}

namespace hailstone::ui::lua {

std::size_t gc_live_bytes(lua_State* L) {
    // Set by the cycle's atomic step to the heap then, and lowered by what its sweep frees: the objects the cycle
    // found reachable. What was made after the atomic step is not in it (lgc.c singlestep, sweepstep).
    return static_cast<std::size_t>(G(L)->GCestimate);
}

bool gc_left_generational(lua_State* L) { return G(L)->gckind != KGC_GEN || G(L)->lastatomic != 0; }

void gc_full_generational(lua_State* L) {
    if (G(L)->gckind == KGC_GEN) lua_gc(L, LUA_GCCOLLECT); // (fullgen: atomic2gen clears lastatomic)
    else lua_gc(L, LUA_GCGEN, 0, 0);                       // (entergen, from incremental: a full collection)
}

} // namespace hailstone::ui::lua
