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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <utility>

#include <lua.hpp>

#include "core/log.h"

namespace hailstone::core {

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

namespace detail {

template <class T> struct ScratchBox {
    T value{};
    bool busy = false;
    std::uint64_t generation = 0; // lua_scratch_generation() when it was taken
};

// The registry key of T's scratch box (its address).
template <class T> inline const char scratch_key = 0;

inline std::uint64_t& scratch_generation() {
    static std::uint64_t generation = 1; // (main thread only, as every Lua state here)
    return generation;
}

// A box held since an earlier generation was left by a raise: said once, as the cost it brings back (a new box each
// call) is only garbage.
inline void scratch_reclaimed() {
    static bool said = false;
    if (said) return;
    said = true;
    log::info("lua: a per-call scratch box left held by a Lua error was taken back (said once)");
}

} // namespace detail

// The current generation of LuaScratch boxes. lua_scratch_new_generation starts the next: the host calls it where no
// lua_CFunction of any state is running (LuaUi::frame, before any Lua runs in it), so a box still held from an earlier
// generation was left by a raise that skipped its release(), and its next use takes it back.
inline std::uint64_t lua_scratch_generation() { return detail::scratch_generation(); }
inline void lua_scratch_new_generation() { ++detail::scratch_generation(); }

// lua_keep for a function called every frame (a window's per-gem or per-button query): one box of T the state keeps
// (made on the first call, destroyed when the state closes) and reset to T{} for each call, where lua_keep makes a
// new userdata with a finalizer each time, which the collector must find in its unsplittable final step, finalize,
// and free a cycle later. T must be default-constructible and move-assignable.
//
// This handle has no destructor (a raise would skip it: see above), so the function calls release() before it
// returns. While the box is held, another use (a call nested in this one) gets a lua_keep box instead, pushed on the
// stack as lua_keep pushes it; otherwise nothing is pushed. A raise that skips release() leaves the box held until
// the host starts a new generation (lua_scratch_new_generation, each frame); until then the function's calls take
// lua_keep boxes, as they did before this existed, and the first use after it takes the box back.
template <class T> class LuaScratch {
public:
    explicit LuaScratch(lua_State* L) {
        detail::ScratchBox<T>* box = nullptr;
        if (lua_rawgetp(L, LUA_REGISTRYINDEX, &detail::scratch_key<T>) == LUA_TUSERDATA) {
            box = static_cast<detail::ScratchBox<T>*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
        } else {
            lua_pop(L, 1);
            box = &lua_keep<detail::ScratchBox<T>>(L);
            lua_rawsetp(L, LUA_REGISTRYINDEX, &detail::scratch_key<T>); // (the registry holds it from here)
        }
        const std::uint64_t now = lua_scratch_generation();
        if (box->busy) {
            if (box->generation == now) { // (held by a call this one is nested in)
                value_ = &lua_keep<T>(L);
                return;
            }
            detail::scratch_reclaimed(); // (held since an earlier generation: left by a raise)
        }
        box->value = T{};
        box->busy = true;
        box->generation = now;
        box_ = box;
        value_ = &box->value;
    }
    LuaScratch(const LuaScratch&) = delete;
    LuaScratch& operator=(const LuaScratch&) = delete;

    T& operator*() const { return *value_; }
    T* operator->() const { return value_; }
    // Whether this use has the state's box (false: a lua_keep box on the stack).
    bool kept() const { return box_ != nullptr; }
    // Gives the box back for the next call (what it holds stays until then).
    void release() {
        if (box_) box_->busy = false;
        box_ = nullptr;
    }

private:
    detail::ScratchBox<T>* box_ = nullptr;
    T* value_ = nullptr;
};

namespace detail {

// The registry key of a function's T kept for the state (its address).
template <class T> inline const char state_value_key = 0;

} // namespace detail

// One T for the state, made on the first call and kept until the state closes (unlike LuaScratch, never reset): what
// a function remembers from one call to the next, such as its last answer. T is a type of the function's own, so no
// other code shares it. Pushes nothing.
template <class T> T& lua_state_value(lua_State* L) {
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &detail::state_value_key<T>) == LUA_TUSERDATA) {
        T* t = static_cast<T*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        return *t;
    }
    lua_pop(L, 1);
    T& t = lua_keep<T>(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &detail::state_value_key<T>); // (the registry holds it from here)
    return t;
}

// Sets field `field` of the table on top of the stack, which has no metatable (a record the caller hands back each
// frame), to the text s, unless the field already holds that text. Lua makes a new object for every string longer
// than 40 bytes it is given (shorter ones it finds again), so a tooltip pushed each frame is garbage each frame even
// when it has not changed. As lua_pushstring, s ends at its first NUL.
inline void set_string_field(lua_State* L, const char* field, const char* s) {
    const std::size_t len = std::strlen(s);
    lua_getfield(L, -1, field);
    std::size_t n = 0;
    const char* old = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &n) : nullptr;
    const bool same = old && n == len && std::memcmp(old, s, len) == 0;
    lua_pop(L, 1);
    if (same) return;
    lua_pushlstring(L, s, len);
    lua_setfield(L, -2, field);
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

} // namespace hailstone::core
