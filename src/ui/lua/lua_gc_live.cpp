// The one place the client reads the collector's internals (third_party/lua, pinned): kept in a file of its own so
// Lua's internal macros (G, cast, ...) reach nothing else.

#include "ui/lua/lua_gc_live.h"

extern "C" {
#include "lstate.h"
}

namespace hailstone::ui::lua {

std::size_t gc_live_bytes(lua_State* L) {
    // The bytes the last finished cycle marked: set after each atomic step of an incremental or major cycle
    // (lgc.c checkmajorminor) and when the collector turns generational (atomic2gen); a minor collection keeps it
    // (youngcollection counts the objects it makes old in GCmarked instead). What was made after the atomic step is
    // not in it.
    const l_mem marked = G(L)->GCmajorminor;
    return marked > 0 ? static_cast<std::size_t>(marked) : 0;
}

bool gc_left_generational(lua_State* L) { return G(L)->gckind != KGC_GENMINOR; }

void gc_full_generational(lua_State* L) {
    if (G(L)->gckind == KGC_GENMINOR) lua_gc(L, LUA_GCCOLLECT); // (fullgen: ends in minor mode, lgc.c luaC_fullgc)
    else lua_gc(L, LUA_GCGEN);                                  // (entergen, from incremental or major mode)
}

} // namespace hailstone::ui::lua
