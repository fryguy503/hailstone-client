// The Find window's data (a Hailstone addition, docs/navigation.md "Find"; it replaces the original's
// FindLocationWnd): the zone's useful NPCs (game/useful_npcs.h) for the packs to list and mark on their maps.
//
//   rows = eq.FindNpcs([search]) -- nearest first, those whose name or subtitle has the text (any case): { id, name, subtitle, category ("merchant", "banker",
//                                  "guildmaster", "special", "service", "quest", "poi"), label (the category's
//                                  heading), x, y (map units, as eq.DrawZoneMap draws), z, distance, quests (the
//                                  server script's: 1 takes hand-ins, 2 talks; 0 none or no script) }; {} outside
//                                  the world. The first call in a zone asks the server script for its quest NPCs.
//   keys = eq.FindCategories()  -- the categories in their order: { { key, label }, ... }
//   ok = eq.FindNpcTarget(id)   -- targets the NPC (false: no such NPC)
//   ok = eq.FindNpcPath(id)     -- the guide path to it (app/guide_path.h; false: no such NPC)
//   ok, why = eq.FindNpcWalk(id) -- walks there (the navigator, as /nav id; its guide path shows): false and why
//                                  when it cannot
//   open = eq.FindOpen([open])  -- the Find window's state (the find key toggles it); with an argument, sets it
//
// The list is made again at most every 200 ms: a pack calls eq.FindNpcs every frame while the window or its map
// marks show.

#include <chrono>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <lua.hpp>

#include "app/lua_eq.h"
#include "core/distribution.h"
#include "game/map_data.h"
#include "game/useful_npcs.h"
#include "game/world_state.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

struct FindCache {
    std::vector<game::UsefulNpc> rows;
    std::chrono::steady_clock::time_point made{};
    const net::GameConnection* conn = nullptr;
    std::uint64_t spawn_serial = 0;
    std::size_t findable = 0;
    std::size_t quests = 0;
    std::string zone;
};

EqHost& host_up(lua_State* L) { return *static_cast<EqHost*>(lua_touserdata(L, lua_upvalueindex(1))); }
FindCache& cache_up(lua_State* L) { return *static_cast<FindCache*>(lua_touserdata(L, lua_upvalueindex(2))); }

const game::Spawn* npc(const EqHost& h, lua_Integer id) {
    if (!h.in_game || !h.conn || id <= 0 || id > 0xffffffff) return nullptr;
    const auto& spawns = h.conn->world().spawns;
    auto it = spawns.find(static_cast<std::uint32_t>(id));
    return it == spawns.end() || !it->second.is_npc ? nullptr : &it->second;
}

const std::vector<game::UsefulNpc>& rows(EqHost& h, FindCache& c) {
    if (!h.in_game || !h.conn) {
        c.rows.clear();
        c.conn = nullptr;
        return c.rows;
    }
    const auto& w = h.conn->world();
    const auto& findable = h.conn->findable_npcs();
    // The server script's quest NPCs: asked once a zone, the first time the list is wanted (not at the zone-in).
    h.conn->request_quest_npcs();
    const auto& quests = h.conn->quest_npcs();
    const auto now = std::chrono::steady_clock::now();
    // Made again when spawns come or go, the findable list changes, or 200 ms passed (distances, positions).
    if (c.conn == h.conn && c.spawn_serial == w.spawn_set_serial && c.findable == findable.size() && c.quests == quests.size() &&
        c.zone == h.zone_short_name && now - c.made < std::chrono::milliseconds(200))
        return c.rows;
    std::unordered_set<std::uint32_t> ids;
    ids.reserve(findable.size());
    for (const auto& [id, n] : findable) ids.insert(id);
    glm::vec3 from{h.feet[0], h.feet[1], h.feet[2]};
    if (const game::Spawn* me = w.player()) from = me->display_valid ? me->display_position : me->position;
    c.rows = game::useful_npcs(w, ids, from, quests);
    c.quests = quests.size();
    c.made = now;
    c.conn = h.conn;
    c.spawn_serial = w.spawn_set_serial;
    c.findable = findable.size();
    c.zone = h.zone_short_name;
    return c.rows;
}

int FindNpcs(lua_State* L) {
    const auto& list = rows(host_up(L), cache_up(L));
    const std::string_view search = lua_type(L, 1) == LUA_TSTRING ? std::string_view(lua_tostring(L, 1)) : "";
    lua_createtable(L, static_cast<int>(list.size()), 0);
    int i = 0;
    for (const auto& n : list) {
        if (!game::useful_npc_matches(n, search)) continue;
        lua_createtable(L, 0, 10);
        lua_pushinteger(L, static_cast<lua_Integer>(n.spawn_id));
        lua_setfield(L, -2, "id");
        lua_pushlstring(L, n.name.data(), n.name.size());
        lua_setfield(L, -2, "name");
        lua_pushlstring(L, n.subtitle.data(), n.subtitle.size());
        lua_setfield(L, -2, "subtitle");
        const auto key = game::npc_category_key(n.category), label = game::npc_category_label(n.category);
        lua_pushlstring(L, key.data(), key.size());
        lua_setfield(L, -2, "category");
        lua_pushlstring(L, label.data(), label.size());
        lua_setfield(L, -2, "label");
        const glm::vec3 m = game::world_to_map(n.position);
        lua_pushnumber(L, m.x);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, m.y);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, m.z);
        lua_setfield(L, -2, "z");
        lua_pushnumber(L, n.distance);
        lua_setfield(L, -2, "distance");
        lua_pushinteger(L, n.quests);
        lua_setfield(L, -2, "quests");
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

int FindCategories(lua_State* L) {
    lua_createtable(L, game::kNpcCategoryCount, 0);
    for (int i = 0; i < game::kNpcCategoryCount; ++i) {
        const auto c = static_cast<game::NpcCategory>(i);
        lua_createtable(L, 2, 0);
        const auto key = game::npc_category_key(c), label = game::npc_category_label(c);
        lua_pushlstring(L, key.data(), key.size());
        lua_rawseti(L, -2, 1);
        lua_pushlstring(L, label.data(), label.size());
        lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int FindNpcTarget(lua_State* L) {
    EqHost& h = host_up(L);
    const game::Spawn* s = npc(h, luaL_checkinteger(L, 1));
    if (s) h.conn->set_target(s->id);
    lua_pushboolean(L, s != nullptr);
    return 1;
}

int FindNpcPath(lua_State* L) {
    EqHost& h = host_up(L);
    const game::Spawn* s = npc(h, luaL_checkinteger(L, 1));
    // The guide path over the client's mesh (app/guide_path.h); without it (tests), the server's find path.
    if (s && !(h.guide_to && h.guide_to(s->id))) h.conn->find_spawn(s->id);
    lua_pushboolean(L, s != nullptr);
    return 1;
}

int FindNpcWalk(lua_State* L) {
    if (distribution::managed()) {
        lua_pushboolean(L, false);
        lua_pushliteral(L, "navigation automation is disabled in this distribution");
        return 2;
    }
    EqHost& h = host_up(L);
    const game::Spawn* s = npc(h, luaL_checkinteger(L, 1));
    std::string why = !s ? "no such NPC" : !h.walk_to ? "walking is not available" : h.walk_to(s->id);
    lua_pushboolean(L, why.empty());
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}

int FindOpen(lua_State* L) {
    EqHost& h = host_up(L);
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) h.find_open = lua_toboolean(L, 1) != 0;
    lua_pushboolean(L, h.find_open);
    return 1;
}

int gc_cache(lua_State* L) {
    static_cast<FindCache*>(lua_touserdata(L, 1))->~FindCache();
    return 0;
}

} // namespace

void register_find_api(lua_State* L, EqHost& host) {
    static const luaL_Reg fns[] = {{"FindNpcs", FindNpcs},           {"FindCategories", FindCategories},
                                   {"FindNpcTarget", FindNpcTarget}, {"FindNpcPath", FindNpcPath},
                                   {"FindNpcWalk", FindNpcWalk},     {"FindOpen", FindOpen}};
    // The one cache, the second upvalue of each function (freed with the last of them).
    new (lua_newuserdatauv(L, sizeof(FindCache), 0)) FindCache();
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, gc_cache);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    for (const auto& f : fns) {
        lua_pushlightuserdata(L, &host);
        lua_pushvalue(L, -2);
        lua_pushcclosure(L, f.func, 2);
        lua_setfield(L, -3, f.name); // the eq table, under the cache
    }
    lua_pop(L, 1);
}

} // namespace hailstone::app
