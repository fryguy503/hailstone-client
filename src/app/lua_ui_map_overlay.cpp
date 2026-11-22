#include "app/lua_ui_map_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lua.hpp>

#include "app/lua_ui_owner.h"
#include "core/log.h"
#include "plugin/lua_keep.h"
#include "plugin/policy.h"
#include "ui/lua/lua_draw_scope.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {
namespace {
// Drawing an overlay needs the floor only: reading the game state and drawing. It never grants the extend bit.
constexpr std::uint32_t kFloor = plugin::kCapRead | plugin::kCapUi;
constexpr std::size_t kMaximum = 64, kPerOwner = 16;
// options.priority: the drawing order's key, within a range a script cannot overflow.
constexpr lua_Integer kLeastPriority = -10000, kMostPriority = 10000;
constexpr std::size_t kHandleLength = 128; // the longest handle UnregisterOverlay takes ("overlay:<gen>:<serial>")
constexpr int kPassStack = 16;             // stack slots kept free for the protected pass's pushes
constexpr int kViewFields = 16;            // the fields push_view sets, as the table's first size
constexpr std::size_t kFailureLength = 160; // the same bound core::cxx_guard uses for its message
constexpr char kRegistry = 0;
// The registry userdata's uservalues: the slot table and the protected pass. Nothing in the registry roots them, so a
// host lifetime that replaces the registry leaves no table or closure behind until the state closes.
constexpr int kSlotValue = 1, kPassValue = 2;
std::atomic<std::uint64_t> gGeneration = 0;

struct Entry {
    OwnedUiCallback owner;
    std::string handle;
    int callback = 0, priority = 0;
    std::uint64_t order = 0; // registration order: the tie-break within one priority
    bool active = true;
};
struct Registry {
    UiCallbackContext context;
    std::vector<std::shared_ptr<Entry>> entries;
    std::uint64_t generation = ++gGeneration, serial = 0;
    int dispatching = 0;      // a callback's own hs.DrawZoneMap / hs.DrawMinimap must not draw overlays again
    bool pass_warned = false; // a failed pass is said once per VM, not on every minimap frame
};
struct Call { std::shared_ptr<Entry> entry; std::string error; };
// The client's snapshot of one drawn view: the map's geometry, its zone name copied (a callback may load another
// zone's map, which would dangle a view into MapState), and the entries to run in order. The entries hold the
// registry's own references, so retiring one during the pass cannot free it under the loop.
struct Dispatch {
    Registry* registry = nullptr;
    MapOverlayView view;
    std::string zone;
    std::array<std::shared_ptr<Entry>, kMaximum> entries;
    std::size_t count = 0;
};

Registry& registry(lua_State* L) { return *static_cast<Registry*>(lua_touserdata(L, lua_upvalueindex(1))); }
void raw_field(lua_State* L, int at, const char* name) {
    at = lua_absindex(L, at); lua_pushstring(L, name); lua_rawget(L, at);
}
std::string_view text(lua_State* L, int at, std::size_t limit = kHandleLength) {
    std::size_t length = 0; const char* value = luaL_checklstring(L, at, &length);
    if (!length || length > limit) luaL_argerror(L, at, "nonempty bounded string expected");
    return {value, length};
}
// Pushes the callback slots of the registry userdata at `registry_index`; false (and nothing pushed) when there is
// none. Two stack slots at most. The userdata is named explicitly, never looked up by key: a binding kept across a
// replaced registry still owns exactly its own slots.
bool push_slots(lua_State* L, int registry_index) {
    if (lua_getiuservalue(L, registry_index, kSlotValue) == LUA_TTABLE) return true;
    lua_pop(L, 1);
    return false;
}
// The entry's slot is cleared in the registry that owns it. Its own userdata, never the current key's: an old binding
// must not clear a newer registry's callback at the same slot number.
void retire(lua_State* L, int registry_index, Entry& e) {
    e.active = false;
    const int slot = e.callback;
    e.callback = 0;
    if (!slot || !lua_checkstack(L, 2)) return;
    registry_index = lua_absindex(L, registry_index);
    if (!push_slots(L, registry_index)) return;
    lua_pushnil(L); lua_rawseti(L, -2, slot); lua_pop(L, 1);
}
// Lifecycle only: a source that is gone, a file that failed, or a plugin granted more than it registered under.
// A source that merely lost a capability is skipped by dispatch and resumes when the capability is back.
void prune(lua_State* L, int registry_index, Registry& r) {
    registry_index = lua_absindex(L, registry_index);
    for (const auto& e : r.entries) {
        // A plugin granted more than it registered under may not keep drawing with the wider authority.
        const std::uint32_t caps = e->owner.live_caps();
        if (!e->active || !e->owner.alive() || e->owner.expanded(caps)) retire(L, registry_index, *e);
    }
    std::erase_if(r.entries, [](const auto& e) { return !e->active; });
}
void maintain(lua_State* L) {
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kRegistry) == LUA_TUSERDATA) {
        try { prune(L, -1, *static_cast<Registry*>(lua_touserdata(L, -1))); } catch (...) {}
    }
    lua_pop(L, 1);
}
Entry* find(Registry& r, std::string_view handle) {
    for (const auto& e : r.entries) if (e->active && e->handle == handle) return e.get();
    return nullptr;
}
// nil, reason: what a refused registration answers, with the stack reset to where the keep box left it. A view, not
// a string: building the reason must not put a temporary with a destructor across a push that can raise.
int refuse(lua_State* L, int base, std::string_view reason) {
    lua_settop(L, base);
    lua_pushnil(L);
    lua_pushlstring(L, reason.data(), reason.size());
    return 2;
}
void set_number(lua_State* L, const char* name, float value) {
    lua_pushnumber(L, value);
    lua_setfield(L, -2, name);
}
// A fresh plain table for one callback: screen fields, the map scale and centre, the player when the map is its zone.
void push_view(lua_State* L, const MapOverlayView& v) {
    lua_createtable(L, 0, kViewFields);
    lua_pushstring(L, v.minimap ? "minimap" : "zone"); lua_setfield(L, -2, "kind");
    set_number(L, "x0", v.x0); set_number(L, "y0", v.y0); set_number(L, "x1", v.x1); set_number(L, "y1", v.y1);
    set_number(L, "cx", v.cx); set_number(L, "cy", v.cy); set_number(L, "radius", v.radius);
    set_number(L, "scale", v.scale); set_number(L, "turn", v.turn);
    set_number(L, "center_x", v.center_x); set_number(L, "center_y", v.center_y);
    if (v.have_player) { set_number(L, "player_x", v.player_x); set_number(L, "player_y", v.player_y); }
    lua_pushlstring(L, v.zone.empty() ? "" : v.zone.data(), v.zone.size()); lua_setfield(L, -2, "zone");
    lua_pushboolean(L, v.current_zone); lua_setfield(L, -2, "current_zone");
}

// hs.UI.map.RegisterOverlay(draw [, {priority = n}])   -- draw(view) is called for the map window's zone map and for
// the minimap, after the client has drawn its own view and before the pack's own marks: by ascending priority, ties by
// registration order. `view` is one plain table: kind ("zone" or "minimap"), the rectangle's screen x0, y0, x1, y1,
// the centre's screen cx, cy and the minimap's radius (0 for the zone map), scale (screen pixels per map unit), turn
// (the view's rotation in heading units, 0 north up), the centre in map units (center_x, center_y), the player's map
// unit (player_x, player_y; absent on another zone's map), the loaded map's short name (zone) and whether it is the
// player's zone (current_zone). Returns a handle for UnregisterOverlay; a callback that fails is retired.
int RegisterOverlay(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (!lua_isnoneornil(L, 2)) { ui::lua::unview(L, 2); luaL_checktype(L, 2, LUA_TTABLE); }
    auto& call = plugin::lua_keep<Call>(L);
    const int base = lua_gettop(L); // the box stays under every answer, so refuse() resets here
    const int registry_index = lua_upvalueindex(1); // this binding's own registry, never a replaced one by key
    auto& r = registry(L);
    plugin::cxx_guard(L, [&] {
        prune(L, registry_index, r); call.entry = std::make_shared<Entry>();
        auto owner = ui::lua::LuaUi::callback_owner(L);
        if (!owner) { call.error = "Map overlays require a live managed UI source"; return; }
        call.entry->owner = {*owner, r.context, plugin::caller_caps()};
    });
    if (!call.error.empty()) return refuse(L, base, call.error);
    Entry& e = *call.entry;
    if (lua_istable(L, 2)) {
        raw_field(L, 2, "priority");
        const auto priority = luaL_optinteger(L, -1, 0); lua_pop(L, 1);
        if (priority < kLeastPriority || priority > kMostPriority) return luaL_error(L, "Map overlay priority is out of range");
        e.priority = static_cast<int>(priority);
    }
    bool registration = false;
    plugin::cxx_guard(L, [&] { registration = e.owner.can_register(kFloor) && e.owner.ready_for_call(kFloor); });
    if (!registration) return refuse(L, base, "Registration permissions are unavailable");
    // The serial and the handle are taken before the push that answers with it: the push can run a GC step whose
    // finalizer registers an overlay, and a serial taken after it could be handed out twice.
    plugin::cxx_guard(L, [&] {
        e.order = ++r.serial;
        e.handle = "overlay:" + std::to_string(r.generation) + ":" + std::to_string(e.order);
    });
    // The handle is pushed before the registration is published: a push that fails for memory leaves nothing live
    // that the caller has no handle for.
    lua_pushlstring(L, e.handle.data(), e.handle.size());
    plugin::cxx_guard(L, [&] {
        // That push can run a GC step whose finalizer registers or removes an overlay: check the owner, the limits and
        // the free slot after it, not before.
        if (!e.owner.can_register(kFloor) || !e.owner.ready_for_call(kFloor)) {
            call.error = "Registration permissions are unavailable"; return;
        }
        prune(L, registry_index, r);
        const auto own = std::ranges::count_if(r.entries, [&](const auto& item) { return item->owner.source.tag == e.owner.source.tag; });
        if (r.entries.size() >= kMaximum || own >= static_cast<std::ptrdiff_t>(kPerOwner)) { call.error = "Map overlay registration limit reached"; return; }
        for (int slot = 1; slot <= static_cast<int>(kMaximum); ++slot)
            if (std::ranges::none_of(r.entries, [&](const auto& item) { return item->callback == slot; })) { e.callback = slot; break; }
    });
    if (!call.error.empty()) return refuse(L, base, call.error);
    // Commit: the slot table was made with room for kMaximum, the vector's capacity was reserved with the registry and
    // the handle already is the answer, so nothing below allocates or raises.
    lua_getiuservalue(L, registry_index, kSlotValue);
    lua_pushvalue(L, 1); lua_rawseti(L, -2, e.callback); lua_pop(L, 1);
    r.entries.push_back(call.entry);
    return 1;
}

// hs.UI.map.UnregisterOverlay(handle)   -- true when the handle was this caller's and is now gone. Ungated, so an
// add-on that lost a capability can still take its own overlay off; another add-on's handle is never touched.
int UnregisterOverlay(lua_State* L) {
    const auto handle = text(L, 1);
    const int registry_index = lua_upvalueindex(1);
    auto& r = registry(L);
    // Find and check ownership before pruning: an owner whose source just went away must still be told its handle
    // was its own and is gone.
    Entry* e = find(r, handle);
    const bool own = e && e->owner.source.tag == ui::lua::LuaUi::caller_tag();
    plugin::cxx_guard(L, [&] {
        if (own) retire(L, registry_index, *e);
        prune(L, registry_index, r);
    });
    lua_pushboolean(L, own); return 1;
}

// One protected pass over the snapshot: builds each callback's table and calls it. A raise from the table's pushes
// returns here as a status from the caller's lua_pcall, so no Lua error skips the map draw's objects or its clip.
// Nothing with a destructor may live in this frame across a push that can raise. The registry userdata is upvalue 1:
// the one whose entries these are, so a slot is cleared in the registry that owns it.
int run_dispatch(lua_State* L) {
    auto* d = static_cast<Dispatch*>(lua_touserdata(L, 1));
    char failure[kFailureLength] = {};
    if (!d || !d->registry) return 0;
    const int registry_index = lua_upvalueindex(1);
    try {
        for (std::size_t i = 0; i < d->count; ++i) {
            const std::shared_ptr<Entry>& e = d->entries[i];
            if (!e->active || !e->callback) continue;
            const int base = lua_gettop(L);
            push_view(L, d->view);
            // The pushes above can run a GC step, and a finalizer may unregister its own overlay: check again before
            // the slot is taken, or a nil slot would be reported as a failing file.
            const std::uint32_t caps = e->owner.live_caps();
            if (!e->active || !e->callback || !e->owner.ready_for_call_with(kFloor, caps)) { lua_settop(L, base); continue; }
            const int slot = e->callback;
            if (!push_slots(L, registry_index)) { lua_settop(L, base); continue; }
            lua_rawgeti(L, -1, slot);
            lua_remove(L, -2); // (the slot table)
            lua_insert(L, -2); // (function, then its one argument)
            // kFailed retires the overlay and kSkipped (the frame's end, not its fault) leaves it registered.
            auto outcome = ui::lua::LuaUi::CallbackOwner::Call::kFailed;
            bool called = false;
            try {
                // The owner's own authority, not the drawing add-on's: the callback enters the owner's tag.
                plugin::CallerScope scope(e->owner.ceiling, e->owner.source.tag);
                ui::lua::WindowDrawScope on_window_list; // the map's clip, even when the drawing code asked for foreground
                if (e->owner.source.invoke_drawn) { outcome = e->owner.source.invoke_drawn(L, 1, 0); called = true; }
            } catch (...) {}
            lua_settop(L, base);
            if (called && outcome == ui::lua::LuaUi::CallbackOwner::Call::kFailed) retire(L, registry_index, *e);
        }
    } catch (const std::exception& error) {
        std::snprintf(failure, sizeof failure, "map overlay dispatch failed: %s", error.what());
    } catch (...) {
        std::snprintf(failure, sizeof failure, "map overlay dispatch failed");
    }
    // Raised with the exception already gone: a longjmp out of its catch block would skip its destructor too.
    if (failure[0] != '\0') return luaL_error(L, "%s", failure);
    return 0;
}

} // namespace

void dispatch_map_overlays(lua_State* L, const MapOverlayView& view) {
    if (!L) return;
    const int top = lua_gettop(L);
    // The usual case is no overlays at all: one registry lookup and back, with no stack reserved and no table built.
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kRegistry) != LUA_TUSERDATA) { lua_settop(L, top); return; }
    const int registry_index = lua_absindex(L, -1); // stays on the stack: retiring an entry clears its slot in it
    Registry& r = *static_cast<Registry*>(lua_touserdata(L, registry_index));
    if (r.dispatching != 0 || r.entries.empty()) { lua_settop(L, top); return; }
    // Room for every push made from here on; after this the only Lua work is the protected pass below.
    if (!lua_checkstack(L, kPassStack)) { lua_settop(L, top); return; }
    Dispatch dispatch;
    dispatch.registry = &r;
    try {
        // One live-grant lookup per entry decides retirement, suspension and this draw's eligibility alike.
        for (const auto& e : r.entries) {
            const std::uint32_t caps = e->owner.live_caps();
            if (!e->active || !e->owner.alive() || e->owner.expanded(caps)) { retire(L, registry_index, *e); continue; }
            if (dispatch.count < kMaximum && e->owner.ready_with(kFloor, caps)) dispatch.entries[dispatch.count++] = e;
        }
        std::erase_if(r.entries, [](const auto& e) { return !e->active; });
        if (dispatch.count == 0) { lua_settop(L, top); return; }
        std::sort(dispatch.entries.begin(), dispatch.entries.begin() + static_cast<std::ptrdiff_t>(dispatch.count),
                  [](const auto& a, const auto& b) {
                      if (a->priority != b->priority) return a->priority < b->priority;
                      return a->order < b->order;
                  });
        // Copied only now that something will draw, and copied where nothing can dangle it.
        dispatch.zone = std::string(view.zone);
        dispatch.view = view;
        dispatch.view.zone = dispatch.zone;
    } catch (...) { lua_settop(L, top); return; }
    // The client draws here, whatever add-on's window called it in: one add-on's callback runs as itself even when
    // another add-on's map code is on the stack. Both scopes outlive the protected pass, so its errors cannot leave
    // them set.
    struct Depth { int& value; explicit Depth(int& v) : value(v) { ++value; } ~Depth() { --value; } } depth(r.dispatching);
    plugin::HostCallerScope host_caps;
    ui::lua::LuaUi::HostScope host_tag;
    int status = LUA_ERRRUN;
    try {
        lua_getiuservalue(L, registry_index, kPassValue);
        lua_pushlightuserdata(L, &dispatch);
        status = lua_pcall(L, 1, 0, 0);
    } catch (...) {}
    if (status != LUA_OK && !r.pass_warned) {
        // A raise here is an allocation failure while a view table was made; the error object is dropped rather than
        // converted (a non-string error could allocate again in the middle of the map draw). The minimap draws every
        // frame, so a persistent failure is said once, not sixty times a second.
        r.pass_warned = true;
        try {
            if (status == LUA_ERRMEM) log::warn("map overlays: out of memory building a view; the rest of this pass was skipped");
            else log::warn("map overlays: a view could not be built; the rest of this pass was skipped");
        } catch (...) {}
    }
    lua_settop(L, top);
}

void register_map_overlay_api(lua_State* L, const UiCallbackContext& context) {
    const int root = lua_absindex(L, -1);
    Registry* data = nullptr;
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kRegistry) == LUA_TUSERDATA) {
        auto* old = static_cast<Registry*>(lua_touserdata(L, -1));
        const bool same = !old->context.lifetime.owner_before(context.lifetime) && !context.lifetime.owner_before(old->context.lifetime);
        if (same) data = old; else lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }
    if (!data) {
        if (luaL_newmetatable(L, "hailstone.ui.map.overlays")) {
            lua_pushcfunction(L, [](lua_State* state) -> int { static_cast<Registry*>(lua_touserdata(state, 1))->~Registry(); return 0; });
            lua_setfield(L, -2, "__gc");
        }
        data = new (lua_newuserdatauv(L, sizeof(Registry), 2)) Registry;
        lua_pushvalue(L, -2); lua_setmetatable(L, -2); lua_remove(L, -2);
        plugin::cxx_guard(L, [&] { data->context = context; data->entries.reserve(kMaximum); });
        // The slot table and the protected pass are the userdata's own uservalues: nothing in the registry roots them,
        // so a later host lifetime that replaces this registry leaves no table or closure to reach lua_close. The pass
        // holds the userdata as its upvalue, so retiring an entry clears the slot in the registry that owns it.
        lua_createtable(L, static_cast<int>(kMaximum), 0);
        lua_setiuservalue(L, -2, kSlotValue);
        lua_pushvalue(L, -1);
        lua_pushcclosure(L, &run_dispatch, 1);
        lua_setiuservalue(L, -2, kPassValue);
        lua_pushvalue(L, -1); lua_rawsetp(L, LUA_REGISTRYINDEX, &kRegistry);
    }
    const int state = lua_absindex(L, -1);
    for (const auto& binding : std::initializer_list<std::pair<const char*, lua_CFunction>>{
            {"MapOverlayRegister", &RegisterOverlay}, {"MapOverlayUnregister", &UnregisterOverlay}}) {
        lua_pushvalue(L, state); lua_pushcclosure(L, binding.second, 1); lua_setfield(L, root, binding.first);
    }
    lua_pop(L, 1);
    static const ui::lua::LuaUi::CallbackMaintenance cleanup = maintain;
    ui::lua::LuaUi::set_callback_maintenance(L, "hs.UI.map.overlays", cleanup);
}
} // namespace hailstone::app
