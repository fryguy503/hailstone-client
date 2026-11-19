// plugin/lua_keep.h: C++ objects a lua_CFunction keeps across Lua calls that raise are freed with the state's
// collection, and a C++ exception becomes a Lua error. LuaScratch reuses one box; set_string_field makes no string
// for unchanged text.
#include <doctest/doctest.h>

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

#include <lua.hpp>

#include "plugin/lua_keep.h"

namespace {

int g_alive = 0;

struct Tracked {
    std::string text = std::string(1000, 'x'); // on the heap: a skipped destructor would leak it
    Tracked() { ++g_alive; }
    ~Tracked() { --g_alive; }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
};

// A state refusing any allocation past `limit` bytes, as the scripts' and the UI's are held to theirs.
struct Limit {
    std::size_t used = 0, limit = 0;
};
void* limited(void* ud, void* ptr, std::size_t old_size, std::size_t new_size) {
    auto* l = static_cast<Limit*>(ud);
    const std::size_t old = ptr ? old_size : 0;
    if (new_size == 0) {
        std::free(ptr);
        l->used -= old;
        return nullptr;
    }
    if (new_size > old && l->used + (new_size - old) > l->limit) return nullptr;
    void* p = std::realloc(ptr, new_size);
    if (p) l->used = l->used - old + new_size;
    return p;
}

// Keeps a Tracked, then pushes a string too big for the state: the push raises past this frame.
int keep_then_fail(lua_State* L) {
    Tracked& t = hailstone::plugin::lua_keep<Tracked>(L);
    const std::string& big = t.text;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 0; i < 100000; ++i) luaL_addlstring(&b, big.data(), big.size());
    luaL_pushresult(&b);
    return 1;
}

int throws(lua_State* L) {
    hailstone::plugin::cxx_guard(L, [] { throw std::runtime_error("boom"); });
    return 0;
}

// A value that counts itself, and can be reset (LuaScratch assigns T{} for each use).
struct Counted {
    static inline int alive = 0;
    std::string text;
    Counted() { ++alive; }
    Counted(const Counted& o) : text(o.text) { ++alive; }
    Counted(Counted&& o) noexcept : text(std::move(o.text)) { ++alive; }
    Counted& operator=(const Counted&) = default;
    Counted& operator=(Counted&&) noexcept = default;
    ~Counted() { --alive; }
};

// A counting allocator: how many allocations the state has made.
struct Count {
    std::size_t made = 0;
};
void* counting(void* ud, void* ptr, std::size_t, std::size_t new_size) {
    if (new_size == 0) {
        std::free(ptr);
        return nullptr;
    }
    if (!ptr) ++static_cast<Count*>(ud)->made;
    return std::realloc(ptr, new_size);
}

int pushes_made(lua_State* L) {
    hailstone::plugin::push_made(L, std::string(100, 'y'));
    return 1;
}

} // namespace

TEST_CASE("lua_keep: what a raise leaves behind is freed when the state collects it") {
    Limit lim;
    lim.limit = 4u << 20;
    lua_State* L = lua_newstate(limited, &lim);
    REQUIRE(L);
    g_alive = 0;
    for (int i = 0; i < 5; ++i) {
        lua_pushcfunction(L, keep_then_fail);
        CHECK(lua_pcall(L, 0, 1, 0) == LUA_ERRMEM);
        lua_pop(L, 1);
    }
    CHECK(g_alive == 5); // not freed by the raise, but still the state's
    lua_gc(L, LUA_GCCOLLECT);
    CHECK(g_alive == 0);
    lua_pushcfunction(L, keep_then_fail);
    CHECK(lua_pcall(L, 0, 1, 0) == LUA_ERRMEM);
    lua_close(L);
    CHECK(g_alive == 0); // and at close
}

TEST_CASE("lua_keep: cxx_guard turns an exception into a Lua error; push_made pushes the text") {
    lua_State* L = luaL_newstate();
    lua_pushcfunction(L, throws);
    REQUIRE(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
    CHECK(std::string(lua_tostring(L, -1)) == "boom");
    lua_pop(L, 1);
    lua_pushcfunction(L, pushes_made);
    REQUIRE(lua_pcall(L, 0, 1, 0) == LUA_OK);
    CHECK(std::string(lua_tostring(L, -1)) == std::string(100, 'y'));
    lua_close(L);
}

TEST_CASE("LuaScratch: one box for every use, reset each time, freed at close; a nested use gets its own") {
    lua_State* L = luaL_newstate();
    Counted::alive = 0;
    Counted* first = nullptr;
    {
        hailstone::plugin::LuaScratch<Counted> a(L);
        CHECK(a.kept());
        CHECK(lua_gettop(L) == 0); // nothing pushed
        CHECK(a->text.empty());
        a->text = "held";
        first = &*a;
        // A use while it is held: a lua_keep box on the stack, never the held one.
        hailstone::plugin::LuaScratch<Counted> nested(L);
        CHECK_FALSE(nested.kept());
        CHECK(&*nested != first);
        CHECK(lua_gettop(L) == 1);
        CHECK(a->text == "held");
        nested.release();
        a.release();
        lua_settop(L, 0);
    }
    {
        hailstone::plugin::LuaScratch<Counted> b(L);
        CHECK(b.kept());
        CHECK(&*b == first); // the same box
        CHECK(b->text.empty()); // reset for this use
        b.release();
    }
    {
        // A use never released (as a raise would leave it): later uses take lua_keep boxes.
        hailstone::plugin::LuaScratch<Counted> lost(L);
        CHECK(lost.kept());
        hailstone::plugin::LuaScratch<Counted> later(L);
        CHECK_FALSE(later.kept());
        later.release();
        lua_settop(L, 0);
    }
    lua_close(L);
    CHECK(Counted::alive == 0);
}

// Takes the scratch box, then raises before its release() (a push failing under a memory limit, or an exception
// cxx_guard turned into an error).
int raises_holding(lua_State* L) {
    hailstone::plugin::LuaScratch<Counted> box(L);
    box->text = "left";
    return luaL_error(L, "raised before release");
}

TEST_CASE("LuaScratch: a box left held by a raise is taken back in the next generation, with no new allocation") {
    Count count;
    lua_State* L = lua_newstate(counting, &count);
    REQUIRE(L);
    Counted::alive = 0;
    Counted* first = nullptr;
    {
        hailstone::plugin::LuaScratch<Counted> a(L); // (makes the box)
        first = &*a;
        a.release();
    }
    lua_pushcfunction(L, raises_holding);
    REQUIRE(lua_pcall(L, 0, 0, 0) != LUA_OK);
    lua_settop(L, 0);
    {
        // The same generation: the box is still held (as by a call this one is nested in), so a lua_keep box.
        hailstone::plugin::LuaScratch<Counted> same(L);
        CHECK_FALSE(same.kept());
        same.release();
        lua_settop(L, 0);
    }
    hailstone::core::lua_scratch_new_generation(); // (as LuaUi::frame, each frame)
    const std::size_t before = count.made;
    {
        hailstone::plugin::LuaScratch<Counted> next(L);
        CHECK(next.kept());
        CHECK(&*next == first);       // the box taken back
        CHECK(next->text.empty());    // reset for this use
        CHECK(lua_gettop(L) == 0);    // nothing pushed
        next.release();
    }
    CHECK(count.made == before);
    {
        hailstone::plugin::LuaScratch<Counted> again(L); // and given back as usual
        CHECK(again.kept());
        again.release();
    }
    lua_close(L);
    CHECK(Counted::alive == 0);
}

TEST_CASE("set_string_field: an unchanged string makes no new string; a changed one is written") {
    Count count;
    lua_State* L = lua_newstate(counting, &count);
    REQUIRE(L);
    const std::string tip(200, 't'); // past Lua's 40-byte short strings, which it finds again without allocating
    lua_createtable(L, 0, 2);
    hailstone::plugin::set_string_field(L, "tooltip", tip.c_str());
    lua_getfield(L, -1, "tooltip");
    CHECK(std::string(lua_tostring(L, -1)) == tip);
    lua_pop(L, 1);
    const std::size_t before = count.made;
    for (int i = 0; i < 10; ++i) hailstone::plugin::set_string_field(L, "tooltip", tip.c_str());
    CHECK(count.made == before);
    const std::string other(200, 'u');
    hailstone::plugin::set_string_field(L, "tooltip", other.c_str());
    CHECK(count.made > before);
    lua_getfield(L, -1, "tooltip");
    CHECK(std::string(lua_tostring(L, -1)) == other);
    lua_pop(L, 1);
    // A field holding something else, or nothing, is written.
    lua_pushinteger(L, 5);
    lua_setfield(L, -2, "label");
    hailstone::plugin::set_string_field(L, "label", "five");
    hailstone::plugin::set_string_field(L, "kind", "spell");
    lua_getfield(L, -1, "label");
    CHECK(std::string(lua_tostring(L, -1)) == "five");
    lua_getfield(L, -2, "kind");
    CHECK(std::string(lua_tostring(L, -1)) == "spell");
    lua_close(L);
}
