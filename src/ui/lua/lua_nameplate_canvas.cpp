#include "ui/lua/lua_nameplate_canvas.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <string_view>
#include <utility>

#include <lua.hpp>

#include "game/nameplate_canvas.h"
#include "core/lua_keep.h"

namespace hailstone::ui::lua {
namespace {
using namespace game;
char kBinding;
struct Registration {
    int ref = LUA_NOREF;
    NameplateCanvasLuaOwner caller;
    std::string name;
};
struct State {
    lua_State *L = nullptr;
    NameplateCanvasLuaEnv env;
    std::weak_ptr<void> registry_lifetime;
    bool active = true;
    std::map<NameplateCanvasOwner, Registration> registrations;
    bool live() const { return active && L && env.registry && !registry_lifetime.expired(); }
    void remove(NameplateCanvasOwner id) {
        if (env.registry && !registry_lifetime.expired())
            env.registry->remove(id);
        if (const auto it = registrations.find(id); it != registrations.end()) {
            if (L)
                luaL_unref(L, LUA_REGISTRYINDEX, it->second.ref);
            registrations.erase(it);
        }
    }
    void close() {
        active = false;
        while (!registrations.empty())
            remove(registrations.begin()->first);
        L = nullptr;
    }
    ~State() { close(); }
};
using Holder = std::shared_ptr<State>;
State &state(lua_State *L) { return **static_cast<Holder *>(lua_touserdata(L, lua_upvalueindex(1))); }
int get(lua_State *L, int table, const char *name) {
    lua_pushstring(L, name);
    return lua_rawget(L, table < 0 ? table - 1 : table);
}
bool plain(lua_State *L, int index) {
    if (!lua_istable(L, index))
        return false;
    if (!lua_getmetatable(L, index))
        return true;
    lua_pop(L, 1);
    return false;
}
bool numeric(lua_State *L, int table, const char *name, float &value, bool required = false) {
    get(L, table, name);
    const bool absent = lua_isnil(L, -1);
    const bool valid = absent ? !required
                              : lua_type(L, -1) == LUA_TNUMBER && std::isfinite(lua_tonumber(L, -1)) &&
                                    std::abs(lua_tonumber(L, -1)) <= 65536;
    if (valid && !absent)
        value = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return valid;
}
bool integer(lua_State *L, int table, const char *name, int &value) {
    get(L, table, name);
    const bool absent = lua_isnil(L, -1);
    const bool valid = absent || (lua_isinteger(L, -1) && lua_tointeger(L, -1) >= 0 && lua_tointeger(L, -1) <= 65535);
    if (valid && !absent)
        value = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return valid;
}
bool boolean(lua_State *L, int table, const char *name, bool &value) {
    get(L, table, name);
    const bool absent = lua_isnil(L, -1);
    const bool valid = absent || lua_isboolean(L, -1);
    if (valid && !absent)
        value = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return valid;
}
bool string(lua_State *L, int table, const char *name, std::string &value, bool required = false) {
    get(L, table, name);
    const bool absent = lua_isnil(L, -1);
    const bool valid = absent ? !required : lua_type(L, -1) == LUA_TSTRING;
    if (valid && !absent) {
        std::size_t length = 0;
        const char *p = lua_tolstring(L, -1, &length);
        if (length > kCanvasMaxTextBytes) {
            lua_pop(L, 1);
            return false;
        }
        value.assign(p, length);
    }
    lua_pop(L, 1);
    return valid;
}
bool color(lua_State *L, int table, const char *name, std::uint32_t &value) {
    get(L, table, name);
    const bool absent = lua_isnil(L, -1);
    const bool valid =
        absent || (lua_isinteger(L, -1) && lua_tointeger(L, -1) >= 0 && lua_tointeger(L, -1) <= 0xffffff);
    if (valid && !absent)
        value = static_cast<std::uint32_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return valid;
}
bool array(lua_State *L, int table, std::size_t limit, std::size_t &size) {
    table = lua_absindex(L, table);
    if (!plain(L, table))
        return false;
    size = lua_rawlen(L, table);
    if (size > limit)
        return false;
    std::size_t count = 0;
    bool valid = true;
    lua_pushnil(L);
    while (lua_next(L, table)) {
        valid = lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1 &&
                static_cast<lua_Unsigned>(lua_tointeger(L, -2)) <= size;
        lua_pop(L, 1);
        if (!valid) {
            lua_pop(L, 1);
            return false;
        }
        if (++count > limit) {
            lua_pop(L, 1);
            return false;
        }
    }
    return count == size;
}
bool parse_command(lua_State *L, State &binding, CanvasCommand &c) {
    const int t = lua_absindex(L, -1);
    if (!plain(L, t))
        return false;
    get(L, t, "kind");
    std::size_t length = 0;
    const char *key = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &length) : nullptr;
    static constexpr std::string_view names[] = {"Rect",         "RectFilled", "Gradient",       "Line", "Circle",
                                                 "CircleFilled", "Triangle",   "TriangleFilled", "Text", "Image"};
    std::size_t kind = 0;
    while (kind < std::size(names) && (!key || names[kind] != std::string_view(key, length)))
        ++kind;
    lua_pop(L, 1);
    if (kind == std::size(names))
        return false;
    c.kind = static_cast<CanvasCommandKind>(kind);
    if (!numeric(L, t, "x", c.x) || !numeric(L, t, "y", c.y) || !numeric(L, t, "x0", c.x0) ||
        !numeric(L, t, "y0", c.y0) || !numeric(L, t, "x1", c.x1) || !numeric(L, t, "y1", c.y1) ||
        !numeric(L, t, "x2", c.x2) || !numeric(L, t, "y2", c.y2) || !numeric(L, t, "radius", c.radius) ||
        !numeric(L, t, "rounding", c.rounding) || !numeric(L, t, "thickness", c.thickness) ||
        !numeric(L, t, "size", c.size) || !integer(L, t, "font", c.font) || !integer(L, t, "segments", c.segments) ||
        !color(L, t, "color", c.color) || !numeric(L, t, "alpha", c.alpha) ||
        !string(L, t, "text", c.text, c.kind == CanvasCommandKind::Text))
        return false;
    c.color2 = c.color;
    c.alpha2 = c.alpha;
    if (!color(L, t, "color2", c.color2) || !numeric(L, t, "alpha2", c.alpha2) || !numeric(L, t, "u0", c.u0) ||
        !numeric(L, t, "v0", c.v0) || !numeric(L, t, "u1", c.u1) || !numeric(L, t, "v1", c.v1))
        return false;
    get(L, t, "texture");
    if (!lua_isnil(L, -1) && !lua_islightuserdata(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    c.texture = reinterpret_cast<std::uintptr_t>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (c.kind == CanvasCommandKind::Image && (!binding.env.texture_valid || !binding.env.texture_valid(c.texture)))
        return false;
    return valid_canvas_command(c);
}
bool parse_models(lua_State *L, State &binding, const NameplateCanvasFrame &frame,
                  std::vector<NameplateCanvasModel> &result) {
    std::size_t count = 0;
    const int t = lua_absindex(L, -1);
    if (!array(L, t, kCanvasMaxCandidates, count))
        return false;
    result.reserve(count);
    std::array<std::size_t, kCanvasMaxCandidates> order{};
    for (std::size_t i = 0; i < count; ++i)
        order[i] = i + 1;
    const auto target =
        std::find_if(frame.candidates.begin(), frame.candidates.end(), [](const auto &c) { return c.context.target; });
    if (target != frame.candidates.end())
        for (std::size_t i = 0; i < count; ++i) {
            lua_rawgeti(L, t, static_cast<lua_Integer>(i + 1));
            if (!plain(L, -1)) {
                lua_pop(L, 1);
                return false;
            }
            get(L, -1, "token");
            std::size_t length = 0;
            const char *token = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &length) : nullptr;
            const bool match = token && std::string_view(token, length) == target->token;
            lua_pop(L, 2);
            if (match) {
                std::rotate(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(i),
                            order.begin() + static_cast<std::ptrdiff_t>(i + 1));
                break;
            }
        }
    std::size_t total = 0;
    for (std::size_t i = 1; i <= count; ++i) {
        lua_rawgeti(L, t, static_cast<lua_Integer>(order[i - 1]));
        const int model = lua_absindex(L, -1);
        if (!plain(L, model))
            return false;
        result.emplace_back();
        auto &m = result.back();
        if (!string(L, model, "token", m.token, true) || !boolean(L, model, "hide", m.hide) ||
            !boolean(L, model, "through_walls", m.through_walls) || !boolean(L, model, "clickable", m.clickable) ||
            !numeric(L, model, "offset_x", m.offset_x) || !numeric(L, model, "offset_y", m.offset_y))
            return false;
        get(L, model, "bounds");
        const int b = lua_absindex(L, -1);
        if (!lua_isnil(L, b) || !m.hide) {
            if (!plain(L, b) || !numeric(L, b, "x", m.bounds.x, true) || !numeric(L, b, "y", m.bounds.y, true) ||
                !numeric(L, b, "w", m.bounds.w, true) || !numeric(L, b, "h", m.bounds.h, true))
                return false;
        }
        lua_pop(L, 1);
        get(L, model, "commands");
        std::size_t commands = 0;
        if (!lua_isnil(L, -1) || !m.hide) {
            const int list = lua_absindex(L, -1);
            if (!array(L, list, kCanvasMaxCommands, commands))
                return false;
            if (commands > kCanvasMaxFrameCommands - total) {
                lua_pop(L, 2);
                result.pop_back();
                continue;
            }
            total += commands;
            m.commands.reserve(commands);
            for (std::size_t j = 1; j <= commands; ++j) {
                lua_rawgeti(L, list, static_cast<lua_Integer>(j));
                m.commands.emplace_back();
                if (!parse_command(L, binding, m.commands.back()))
                    return false;
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        if (!valid_canvas_model(m))
            return false;
        lua_pop(L, 1);
    }
    return true;
}
void number_field(lua_State *L, const char *key, double n) {
    lua_pushnumber(L, n);
    lua_setfield(L, -2, key);
}
void int_field(lua_State *L, const char *key, std::uint64_t n) {
    lua_pushinteger(L, static_cast<lua_Integer>(n));
    lua_setfield(L, -2, key);
}
void bool_field(lua_State *L, const char *key, bool n) {
    lua_pushboolean(L, n);
    lua_setfield(L, -2, key);
}
void str_field(lua_State *L, const char *key, const std::string &n) {
    lua_pushlstring(L, n.data(), n.size());
    lua_setfield(L, -2, key);
}
void optional_field(lua_State *L, const char *key, const std::optional<double> &n) {
    if (n && std::isfinite(*n) && *n >= 0 && *n <= 100)
        number_field(L, key, *n);
}
void push_frame(lua_State *L, const NameplateCanvasFrame &f) {
    lua_createtable(L, 0, 7);
    int_field(L, "serial", f.serial);
    number_field(L, "now_ms", f.now_ms);
    number_field(L, "dt_ms", f.dt_ms);
    number_field(L, "font_size", f.font_size);
    int_field(L, "font", static_cast<std::uint64_t>(f.font));
    lua_createtable(L, 0, 4);
    number_field(L, "x", f.viewport.x);
    number_field(L, "y", f.viewport.y);
    number_field(L, "w", f.viewport.w);
    number_field(L, "h", f.viewport.h);
    lua_setfield(L, -2, "viewport");
    lua_createtable(L, static_cast<int>(f.candidates.size()), 0);
    lua_Integer i = 0;
    for (const auto &c : f.candidates) {
        lua_createtable(L, 0, 7);
        str_field(L, "token", c.token);
        number_field(L, "x", c.x);
        number_field(L, "y", c.y);
        number_field(L, "depth", c.depth);
        number_field(L, "native_scale", c.native_scale);
        lua_createtable(L, 0, 20);
        const auto &s = c.spawn;
        int_field(L, "id", s.id);
        int_field(L, "incarnation", s.incarnation);
        str_field(L, "name", s.name);
        int_field(L, "level", s.level);
        int_field(L, "class", s.class_id);
        int_field(L, "race", s.race);
        int_field(L, "owner", s.owner);
        int_field(L, "guild", s.guild);
        int_field(L, "rider", s.rider);
        int_field(L, "body_type", s.body_type);
        number_field(L, "x", s.x);
        number_field(L, "y", s.y);
        number_field(L, "z", s.z);
        number_field(L, "heading", s.heading);
        bool_field(L, "npc", s.npc);
        bool_field(L, "corpse", s.corpse);
        bool_field(L, "mercenary", s.mercenary);
        bool_field(L, "gm", s.gm);
        lua_setfield(L, -2, "spawn");
        lua_createtable(L, 0, 21);
        const auto &x = c.context;
        str_field(L, "name", x.name);
        bool_field(L, "target", x.target);
        bool_field(L, "self", x.self);
        bool_field(L, "in_group", x.in_group);
        bool_field(L, "in_raid", x.in_raid);
        bool_field(L, "concealed", x.concealed);
        bool_field(L, "invisible", x.invisible);
        bool_field(L, "invisible_undead", x.invisible_undead);
        number_field(L, "distance", x.distance);
        number_field(L, "now_ms", f.now_ms);
        number_field(L, "dt_ms", f.dt_ms);
        optional_field(L, "hp_percent", x.hp_percent);
        optional_field(L, "mana_percent", x.mana_percent);
        optional_field(L, "endurance_percent", x.endurance_percent);
        optional_field(L, "aggro_percent", x.aggro_percent);
        int_field(L, "group_mark", x.group_mark);
        int_field(L, "raid_mark", x.raid_mark);
        if (x.casting) {
            lua_createtable(L, 0, 4);
            int_field(L, "spell_id", x.casting->spell_id);
            str_field(L, "name", x.casting->name);
            number_field(L, "elapsed_ms", x.casting->elapsed_ms);
            number_field(L, "duration_ms", x.casting->duration_ms);
            lua_setfield(L, -2, "casting");
        }
        lua_setfield(L, -2, "context");
        lua_rawseti(L, -2, ++i);
    }
    lua_setfield(L, -2, "candidates");
}
struct Invocation {
    State *binding = nullptr;
    const NameplateCanvasFrame *frame = nullptr;
    int ref = LUA_NOREF;
    std::vector<NameplateCanvasModel> models;
    bool answered = false, valid = false;
};
int Invoke(lua_State *L) {
    auto &call = *static_cast<Invocation *>(lua_touserdata(L, 1));
    lua_rawgeti(L, LUA_REGISTRYINDEX, call.ref);
    push_frame(L, *call.frame);
    lua_call(L, 1, 1);
    if (lua_isnil(L, -1) || !call.binding->live())
        return 0;
    call.answered = true;
    core::cxx_guard(L, [&] { call.valid = parse_models(L, *call.binding, *call.frame, call.models); });
    return 0;
}
std::optional<std::vector<NameplateCanvasModel>> run(const std::shared_ptr<State> &b, NameplateCanvasOwner owner,
                                                     const NameplateCanvasFrame &f) {
    if (!b->live())
        return std::nullopt;
    const auto found = b->registrations.find(owner);
    if (found == b->registrations.end())
        return std::nullopt;
    const auto caller = found->second.caller;
    if ((caller.valid && !caller.valid()) || (caller.ready && !caller.ready()))
        return std::nullopt;
    lua_State *L = b->L;
    const int top = lua_gettop(L);
    Invocation call{b.get(), &f, found->second.ref, {}, false, false};
    lua_pushcfunction(L, Invoke);
    lua_pushlightuserdata(L, &call);
    if (!caller.invoke || !caller.invoke(L, 1, 0)) {
        if (b->live())
            b->remove(owner);
        lua_settop(L, top);
        return std::nullopt;
    }
    if (!b->live() || (caller.valid && !caller.valid())) {
        lua_settop(L, top);
        return std::nullopt;
    }
    auto models = call.answered && call.valid ? std::optional<std::vector<NameplateCanvasModel>>(std::move(call.models))
                                              : std::nullopt;
    lua_settop(L, top);
    return models;
}
struct RegisterWork {
    Holder binding;
    NameplateCanvasLuaOwner caller;
    NameplateCanvasOwner id = 0;
    std::string handle, name;
    int pending_ref = LUA_NOREF;
    NameplateCanvasOwner pending_owner = 0;
    ~RegisterWork() {
        if (!binding)
            return;
        if (pending_owner && binding->live())
            binding->env.registry->remove(pending_owner);
        if (pending_ref != LUA_NOREF && binding->L)
            luaL_unref(binding->L, LUA_REGISTRYINDEX, pending_ref);
    }
};
int Register(lua_State *L) {
    const bool named = lua_type(L, 1) == LUA_TSTRING;
    std::size_t name_length = 0;
    const char *name = named ? luaL_checklstring(L, 1, &name_length) : "default";
    if (named && (name_length == 0 || name_length > 64 ||
                  std::any_of(name, name + name_length, [](unsigned char c) { return c < 32 || c == 127; })))
        return luaL_error(L, "invalid nameplate renderer name");
    const int function = named ? 2 : 1;
    luaL_checktype(L, function, LUA_TFUNCTION);
    auto &b = state(L);
    if (!b.live()) {
        lua_pushnil(L);
        lua_pushliteral(L, "nameplate canvas is unavailable");
        return 2;
    }
    auto &work = core::lua_keep<RegisterWork>(L);
    core::cxx_guard(L, [&] {
        work.binding = *static_cast<Holder *>(lua_touserdata(L, lua_upvalueindex(1)));
        work.caller = b.env.owner ? b.env.owner(L) : NameplateCanvasLuaOwner{};
        work.name.assign(name, named ? name_length : 7);
    });
    bool permitted = false;
    core::cxx_guard(L, [&] {
        permitted = b.live() && !work.caller.key.empty() && work.caller.valid && work.caller.valid() && b.live();
    });
    if (!permitted)
        return luaL_error(L, "nameplate canvas registration is not allowed for this owner");
    lua_pushvalue(L, function);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    work.pending_ref = ref;
    core::cxx_guard(L, [&] {
        const std::weak_ptr<State> weak = work.binding;
        for (auto it = b.registrations.begin(); it != b.registrations.end();) {
            if (it->second.caller.valid && !it->second.caller.valid()) {
                const auto id = it->first;
                ++it;
                b.remove(id);
            } else
                ++it;
        }
        auto identity = std::make_shared<NameplateCanvasOwner>(0);
        for (auto it = b.registrations.begin(); it != b.registrations.end();) {
            if (it->second.caller.key == work.caller.key && it->second.name == work.name) {
                const auto id = it->first;
                ++it;
                b.remove(id);
            } else
                ++it;
        }
        work.id = b.env.registry->add(
            work.caller.key + ":" + work.name,
            [weak, identity](const auto &frame) -> std::optional<std::vector<NameplateCanvasModel>> {
                auto held = weak.lock();
                return held ? run(held, *identity, frame) : std::nullopt;
            },
            [weak, caller = work.caller] {
                auto held = weak.lock();
                return held && held->live() && (!caller.valid || caller.valid());
            });
        *identity = work.id;
        work.pending_owner = work.id;
        if (work.id) {
            b.registrations.emplace(work.id, Registration{ref, work.caller, work.name});
            work.pending_owner = 0;
            work.pending_ref = LUA_NOREF;
            work.handle = "canvas:" + std::to_string(work.id);
            if (b.env.registered)
                b.env.registered();
        }
    });
    if (!work.id) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        work.pending_ref = LUA_NOREF;
        lua_pushnil(L);
        lua_pushliteral(L, "nameplate canvas registration limit reached");
        return 2;
    }
    lua_pushlstring(L, work.handle.data(), work.handle.size());
    return 1;
}
int Unregister(lua_State *L) {
    std::size_t length = 0;
    const char *handle = luaL_checklstring(L, 1, &length);
    auto &b = state(L);
    NameplateCanvasOwner owner = 0;
    const std::string_view value(handle, length);
    bool known = false;
    core::cxx_guard(L, [&] {
        if (value.starts_with("canvas:"))
            for (const auto &[id, r] : b.registrations) {
                (void)r;
                if (value == "canvas:" + std::to_string(id)) {
                    owner = id;
                    known = true;
                    break;
                }
            }
    });
    bool allowed = false;
    if (known && b.live()) {
        auto &work = core::lua_keep<NameplateCanvasLuaOwner>(L);
        core::cxx_guard(L, [&] { work = b.env.owner ? b.env.owner(L) : NameplateCanvasLuaOwner{}; });
        core::cxx_guard(L, [&] {
            if (!b.live() || !work.valid || !work.valid() || !b.live()) return;
            const auto found = b.registrations.find(owner);
            allowed = found != b.registrations.end() && work.key == found->second.caller.key;
            if (allowed) b.remove(owner);
        });
    }
    lua_pushboolean(L, allowed);
    return 1;
}
int MeasureText(lua_State *L) {
    std::size_t length = 0;
    const char *text = luaL_checklstring(L, 1, &length);
    const double requested = luaL_optnumber(L, 2, 16);
    const lua_Integer font = luaL_optinteger(L, 3, 0);
    auto &b = state(L);
    if (!b.live()) {
        lua_pushnil(L);
        return 1;
    }
    auto &caller = core::lua_keep<NameplateCanvasLuaOwner>(L);
    core::cxx_guard(L, [&] { caller = b.env.owner ? b.env.owner(L) : NameplateCanvasLuaOwner{}; });
    bool permitted = false;
    core::cxx_guard(L, [&] { permitted = caller.valid && caller.valid(); });
    if (!permitted)
        return luaL_error(L, "nameplate canvas metrics are not allowed for this owner");
    if (!b.live() || font < 0 || font > 6 || !std::isfinite(requested) || requested < 4 || requested > 256) {
        lua_pushnil(L);
        return 1;
    }
    const float size = static_cast<float>(requested);
    const auto metrics = b.env.registry->measure(std::string_view(text, length), size, static_cast<int>(font));
    if (!metrics) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 6);
    number_field(L, "width", metrics->width);
    number_field(L, "height", metrics->height);
    number_field(L, "ink_x0", metrics->ink_x0);
    number_field(L, "ink_y0", metrics->ink_y0);
    number_field(L, "ink_x1", metrics->ink_x1);
    number_field(L, "ink_y1", metrics->ink_y1);
    return 1;
}
int SpellIcon(lua_State *L) {
    const lua_Integer id = luaL_checkinteger(L, 1);
    auto &b = state(L);
    if (!b.live()) {
        lua_pushnil(L);
        return 1;
    }
    auto &caller = core::lua_keep<NameplateCanvasLuaOwner>(L);
    core::cxx_guard(L, [&] { caller = b.env.owner ? b.env.owner(L) : NameplateCanvasLuaOwner{}; });
    bool permitted = false;
    core::cxx_guard(L, [&] { permitted = caller.valid && caller.valid(); });
    if (!permitted)
        return luaL_error(L, "nameplate canvas images are not allowed for this owner");
    if (!b.live() || id < 0 || id > 65535) {
        lua_pushnil(L);
        return 1;
    }
    const auto icon = b.env.registry->icon(static_cast<std::uint32_t>(id));
    if (!icon) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlightuserdata(L, reinterpret_cast<void *>(icon->texture));
    lua_pushnumber(L, icon->u0);
    lua_pushnumber(L, icon->v0);
    lua_pushnumber(L, icon->u1);
    lua_pushnumber(L, icon->v1);
    return 5;
}
} // namespace

void push_nameplate_canvas(lua_State *L, NameplateCanvasLuaEnv env) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kBinding);
    if (auto *previous = static_cast<Holder *>(lua_touserdata(L, -1));
        previous && *previous &&
        (!(*previous)->active || (*previous)->env.registry != env.registry ||
         (*previous)->registry_lifetime.expired() ||
         (*previous)->env.owner_lifetime.owner_before(env.owner_lifetime) ||
         env.owner_lifetime.owner_before((*previous)->env.owner_lifetime))) {
        (*previous)->close();
        lua_pop(L, 1);
        lua_pushnil(L);
    }
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        auto &holder = core::lua_keep<Holder>(L);
        core::cxx_guard(L, [&] {
            holder = std::make_shared<State>();
            holder->L = L;
            holder->env = std::move(env);
            if (holder->env.registry)
                holder->registry_lifetime = holder->env.registry->lifetime();
        });
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &kBinding);
    }
    const int holder = lua_absindex(L, -1);
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "version");
    static constexpr luaL_Reg functions[] = {{"Register", Register}, {"Unregister", Unregister},
                                             {"MeasureText", MeasureText}, {"SpellIcon", SpellIcon}};
    for (const auto& [name, fn] : functions) {
        lua_pushvalue(L, holder);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    }
    lua_remove(L, holder);
}
void release_nameplate_canvas(lua_State *L) {
    if (!L)
        return;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kBinding);
    if (auto *holder = static_cast<Holder *>(lua_touserdata(L, -1)); holder && *holder)
        (*holder)->close();
    lua_pop(L, 1);
}
} // namespace hailstone::ui::lua
