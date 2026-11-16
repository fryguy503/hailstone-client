#pragma once

// C++ objects a lua_CFunction needs across Lua calls that can raise an error, for the UI's state (app/lua_ui*.cpp)
// and the scripts' (mq/lua_mq.cpp, mq/lua_hailstone.cpp).
//
// Lua is built as C (CMakeLists.txt), so its errors are longjmps: one raised by a failed argument check, a push that
// fails under a state's memory limit, a metamethod that lua_getfield or lua_gettable runs, or a lua_call leaves the
// C++ frames it passes without running their destructors, and a std::string or vector alive across such a call
// leaks. A C++ exception (std::bad_alloc) that leaves a lua_CFunction crosses Lua's C frames instead, which is
// undefined.
//
// So a function that makes C++ objects and pushes from them:
//   1. checks its arguments (luaL_check*) and reads table fields onto the stack first, while nothing is alive;
//   2. makes the objects that must live across a push in a box the state owns (lua_keep), not in its own frame:
//      a raise leaves them to the collector;
//   3. does its C++ work inside cxx_guard, which turns an exception into a Lua error once it has been caught;
//   4. pushes from the box (or from a string the state's host keeps for it, as the scripts' Script::scratch).
// A single string made for one push goes through push_made, which moves it out of the caller's frame first.

#include <cstdio>
#include <exception>
#include <new>
#include <string>
#include <utility>

#include <lua.hpp>

namespace hailstone::plugin {

namespace detail {

template <class T> int keep_gc(lua_State* L) {
    static_cast<T*>(lua_touserdata(L, 1))->~T();
    return 0;
}

// The registry key of T's box metatable (its address).
template <class T> inline const char keep_key = 0;

} // namespace detail

// A T made in a full userdata pushed on top of the stack, destroyed when the state collects it (or closes). It may
// raise only before T exists; T's constructor must not raise a Lua error, and one that throws leaves an inert
// userdata (it has no metatable yet). Pop or lua_remove the box once done; assigning T{} frees what it holds sooner.
template <class T, class... A> T& lua_keep(lua_State* L, A&&... args) {
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &detail::keep_key<T>) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_createtable(L, 0, 2);
        lua_pushcfunction(L, detail::keep_gc<T>);
        lua_setfield(L, -2, "__gc");
        lua_pushliteral(L, "locked");
        lua_setfield(L, -2, "__metatable");
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &detail::keep_key<T>);
    }
    void* mem = lua_newuserdatauv(L, sizeof(T), 0);
    T* t = new (mem) T(std::forward<A>(args)...);
    lua_rotate(L, -2, 1); // the box, then its metatable (nothing from here raises)
    lua_setmetatable(L, -2);
    return *t;
}

// Pushes s, a string made for this push (push_made(L, name_of(id))): moved first into a thread-local string, which
// outlives a push that raises, and the caller's left holding none.
inline void push_made(lua_State* L, std::string&& s) {
    thread_local std::string held;
    held = std::move(s);
    std::string().swap(s);
    lua_pushlstring(L, held.data(), held.size());
}

// Runs f, C++ work that makes no Lua call that can raise, and raises a Lua error for an exception it throws once the
// exception is gone (a longjmp out of a catch block would skip its destructor too).
template <class F> void cxx_guard(lua_State* L, F&& f) {
    char why[160];
    why[0] = '\0';
    bool failed = false;
    try {
        std::forward<F>(f)();
    } catch (const std::bad_alloc&) {
        failed = true;
        std::snprintf(why, sizeof why, "not enough memory");
    } catch (const std::exception& e) {
        failed = true;
        std::snprintf(why, sizeof why, "%s", e.what());
    }
    if (failed) luaL_error(L, "%s", why);
}

} // namespace hailstone::plugin
