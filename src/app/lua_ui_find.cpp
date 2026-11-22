// The Find window's data (a Hailstone addition, docs/navigation.md "Find"; it replaces the original's
// FindLocationWnd): the zone's useful NPCs (game/useful_npcs.h) for the packs to list and mark on their maps.
//
//   rows = hs.FindNpcs([search]) -- nearest first, those whose name or subtitle has the text (any case): { id, name, subtitle, category ("merchant", "banker",
//                                  "guildmaster", "special", "service", "quest", "poi"), label (the category's
//                                  heading), x, y (map units, as hs.DrawZoneMap draws), z, distance, quests (the
//                                  server script's: 1 takes hand-ins, 2 talks; 0 none or no script) }; {} outside
//                                  the world. The first call in a zone asks the server script for its quest NPCs.
//   keys = hs.FindCategories()  -- the categories in their order: { { key, label }, ... }
//   Each row also carries opaque string context, incarnation and token fields. Retained controls pass the token
//   back to an action to require the same observed actor, connection and effective policy.
//   ok = hs.FindNpcTarget(id [, token]) -- targets the current eligible NPC; false for an expired observation
//   ok = hs.FindNpcPath(id [, token]) -- requests its guide/server path; false when unavailable or stale
//   ok, why = hs.FindNpcWalk(id [, token]) -- walks there (the navigator, as /nav id; its guide path shows): false and why
//                                  when it cannot
//   open = hs.FindOpen([open])  -- the Find window's state (the find key toggles it); with an argument, sets it
//
// The list is made again at most every 200 ms: a pack calls hs.FindNpcs every frame while the window or its map
// marks show.

#include <chrono>
#include <charconv>
#include <array>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <lua.hpp>

#include "app/lua_ui.h"
#include "game/map_data.h"
#include "game/useful_npcs.h"
#include "game/world_state.h"
#include "net/game_connection.h"
#include "game/server_ext.h"
#include "plugin/lua_keep.h"

namespace hailstone::app {

namespace {

struct FindCache {
    std::vector<game::UsefulNpc> rows;
    std::chrono::steady_clock::time_point made{};
    const net::GameConnection* conn = nullptr;
    std::uint64_t spawn_serial = 0;
    std::uint64_t context = 0, policy_revision = 0;
    std::uint64_t findable_revision = 0;
    std::size_t quests = 0;
    std::string zone;
};

UiHost& host_up(lua_State* L) { return *static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1))); }
FindCache& cache_up(lua_State* L) { return *static_cast<FindCache*>(lua_touserdata(L, lua_upvalueindex(2))); }

const game::Spawn* npc(const UiHost& h, lua_Integer id) {
    if (!h.in_game || !h.conn || id <= 0 || id > 0xffffffff) return nullptr;
    const auto* conn = h.conn;
    const auto actor = conn->captureActorRef(static_cast<std::uint32_t>(id));
    if (!actor || !conn->npc_find_allowed(static_cast<std::uint32_t>(id)) || h.conn != conn || !conn->matches(*actor)) return nullptr;
    const auto& spawns = conn->world().spawns;
    auto it = spawns.find(static_cast<std::uint32_t>(id));
    return it == spawns.end() ? nullptr : &it->second;
}

struct FindSnapshot {
    const net::GameConnection* conn = nullptr;
    std::uint64_t context = 0, revision = 0;
    bool allowed = false;
    bool server_only = false;
    bool operator==(const FindSnapshot&) const = default;
};

FindSnapshot snapshot(const UiHost& host) {
    FindSnapshot out;
    if (!host.in_game || !host.conn) return out;
    out.conn = host.conn;
    out.context = out.conn->interaction_context();
    const auto policy = out.conn->server_ext().map_policy(*out.conn);
    out.revision = policy.revision;
    out.allowed = policy.npc_find_enabled();
    out.server_only = policy.npc_find_server_only();
    if (host.conn != out.conn || out.conn->interaction_context() != out.context) return {};
    return out;
}

// Observation tokens are identity checks, not permissions. Live policy/action gates still run on every use.
struct ObservationToken {
    std::array<char, 112> data{};
    std::size_t size = 0;
};
ObservationToken token(const net::GameConnection::ActorRef& actor, std::uint64_t revision) {
    ObservationToken out;
    for (const auto value : {actor.context, static_cast<std::uint64_t>(actor.id), actor.incarnation, revision}) {
        if (out.size) out.data[out.size++] = ':';
        const auto encoded = std::to_chars(out.data.data() + out.size, out.data.data() + out.data.size(), value);
        if (encoded.ec != std::errc{}) return {};
        out.size = static_cast<std::size_t>(encoded.ptr - out.data.data());
    }
    return out;
}
void opaque(lua_State* L, std::uint64_t value, const char* field) {
    std::array<char, 20> encoded{};
    const auto end = std::to_chars(encoded.data(), encoded.data() + encoded.size(), value).ptr;
    lua_pushlstring(L, encoded.data(), static_cast<std::size_t>(end - encoded.data()));
    lua_setfield(L, -2, field);
}

struct FindObservation {
    net::GameConnection::ActorRef actor;
    bool server_only;
};

std::optional<FindObservation> observed_actor(lua_State* L, UiHost& host) {
    const auto id = luaL_checkinteger(L, 1);
    std::size_t expected_size = 0;
    const char* expected = lua_isnoneornil(L, 2) ? nullptr : luaL_checklstring(L, 2, &expected_size);
    if (id <= 0 || id > 0xffffffff || expected_size > 112) return std::nullopt;
    std::optional<FindObservation> observation;
    plugin::cxx_guard(L, [&] {
        const auto before = snapshot(host);
        if (!before.allowed || !before.conn) return;
        const auto actor = before.conn->captureActorRef(static_cast<std::uint32_t>(id));
        if (!actor || !npc(host, id) || snapshot(host) != before || host.conn != before.conn ||
            !before.conn->matches(*actor) || (before.server_only && !before.conn->findable_npcs().contains(actor->id)))
            return;
        if (expected) {
            const auto current = token(*actor, before.revision);
            if (std::string_view{expected, expected_size} != std::string_view{current.data.data(), current.size}) return;
        }
        observation = FindObservation{*actor, before.server_only};
    });
    return observation;
}

const std::vector<game::UsefulNpc>& rows(UiHost& h, FindCache& c, const FindSnapshot& view) {
    if (!view.allowed || !view.conn) {
        c.rows.clear();
        c.conn = nullptr;
        return c.rows;
    }
    const auto& w = h.conn->world();
    const auto& findable = h.conn->findable_npcs();
    // The server script's quest NPCs: asked once a zone, the first time the list is wanted (not at the zone-in).
    static const game::QuestNpcMap no_quests;
    if (!view.server_only) h.conn->request_quest_npcs();
    const auto& quests = view.server_only ? no_quests : h.conn->quest_npcs();
    const auto findable_revision = h.conn->findable_npcs_revision();
    const auto now = std::chrono::steady_clock::now();
    // Made again when spawns come or go, the findable list changes, or 200 ms passed (distances, positions).
    if (c.conn == h.conn && c.spawn_serial == w.spawn_set_serial && c.findable_revision == findable_revision &&
        c.quests == quests.size() &&
        c.zone == h.zone_short_name && c.context == view.context && c.policy_revision == view.revision &&
        now - c.made < std::chrono::milliseconds(200))
        return c.rows;
    std::unordered_set<std::uint32_t> ids;
    ids.reserve(findable.size());
    for (const auto& [id, n] : findable) ids.insert(id);
    glm::vec3 from{h.feet[0], h.feet[1], h.feet[2]};
    if (const game::Spawn* me = w.player()) from = me->display_valid ? me->display_position : me->position;
    c.rows = game::useful_npcs(w, ids, from, quests, view.server_only);
    c.quests = quests.size();
    c.made = now;
    c.conn = h.conn;
    c.spawn_serial = w.spawn_set_serial;
    c.findable_revision = findable_revision;
    c.zone = h.zone_short_name;
    c.context = view.context;
    c.policy_revision = view.revision;
    return c.rows;
}

int FindNpcs(lua_State* L) {
    auto& host = host_up(L);
    FindSnapshot view;
    const std::vector<game::UsefulNpc>* list = nullptr;
    plugin::cxx_guard(L, [&] { view = snapshot(host); list = &rows(host, cache_up(L), view); });
    if (!view.allowed) { // NPC Find off (a server's map policy) or not in game: no row, nothing asked per NPC
        lua_newtable(L);
        return 1;
    }
    const std::string_view search = lua_type(L, 1) == LUA_TSTRING ? std::string_view(lua_tostring(L, 1)) : "";
    lua_createtable(L, static_cast<int>(list->size()), 0);
    int i = 0;
    for (const auto& n : *list) {
        std::optional<net::GameConnection::ActorRef> actor;
        plugin::cxx_guard(L, [&] {
            // The policy was read once for every row (snapshot); each row asks only its own visibility.
            if (game::useful_npc_matches(n, search) && host.conn == view.conn &&
                view.conn->npc_find_visible(n.spawn_id, view.server_only))
                actor = view.conn->captureActorRef(n.spawn_id);
        });
        if (!actor || actor->context != view.context) continue;
        const auto observation = token(*actor, view.revision);
        lua_createtable(L, 0, 14);
        lua_pushinteger(L, static_cast<lua_Integer>(n.spawn_id));
        lua_setfield(L, -2, "id");
        opaque(L, actor->context, "context");
        opaque(L, actor->incarnation, "incarnation");
        lua_pushlstring(L, observation.data.data(), observation.size); lua_setfield(L, -2, "token");
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
    bool current = false;
    plugin::cxx_guard(L, [&] { current = snapshot(host) == view; });
    if (!current) { lua_pop(L, 1); lua_newtable(L); }
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
    UiHost& h = host_up(L);
    const auto observation = observed_actor(L, h);
    plugin::cxx_guard(L, [&] { if (observation) h.conn->set_target(observation->actor.id); });
    lua_pushboolean(L, observation.has_value());
    return 1;
}

int FindNpcPath(lua_State* L) {
    UiHost& h = host_up(L);
    const auto observation = observed_actor(L, h);
    bool accepted = false;
    // Server-only Find always asks the server for the path to its advertised NPC.
    plugin::cxx_guard(L, [&] {
        if (observation) {
            const auto& actor = observation->actor;
            accepted = !observation->server_only && h.guide_to && h.guide_to(actor.id);
            if (!accepted && h.conn && h.conn->matches(actor)) accepted = h.conn->find_spawn(actor.id);
        }
    });
    lua_pushboolean(L, accepted);
    return 1;
}

int FindNpcWalk(lua_State* L) {
    UiHost& h = host_up(L);
    const bool observed = !lua_isnoneornil(L, 2);
    // A Lua allocation can run finalizers. Prepare storage before checking the retained observation.
    lua_settop(L, 2);
    auto& why = plugin::lua_keep<std::string>(L);
    const auto observation = observed_actor(L, h);
    plugin::cxx_guard(L, [&] {
        why = !observation ? (observed ? "NPC changed or is no longer available" : "no such NPC") :
            (observation->server_only || !h.walk_to) ? "walking is not available" : h.walk_to(observation->actor.id);
    });
    lua_pushboolean(L, why.empty());
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}

int FindOpen(lua_State* L) {
    UiHost& h = host_up(L);
    plugin::cxx_guard(L, [&] {
        if (h.conn && !h.conn->npc_find_enabled()) h.find_open = false;
        else if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) h.find_open = lua_toboolean(L, 1) != 0;
    });
    lua_pushboolean(L, h.find_open);
    return 1;
}

int gc_cache(lua_State* L) {
    static_cast<FindCache*>(lua_touserdata(L, 1))->~FindCache();
    return 0;
}

} // namespace

void register_find_api(lua_State* L, UiHost& host) {
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
        lua_setfield(L, -3, f.name); // the hs table, under the cache
    }
    lua_pop(L, 1);
}

} // namespace hailstone::app
