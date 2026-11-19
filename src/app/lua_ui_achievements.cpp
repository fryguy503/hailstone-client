#include "app/lua_ui_windows.h"

#include <charconv>
#include <limits>
#include <string_view>

#include <lua.hpp>

#include "app/lua_ui_record.h"
#include "net/game_connection.h"

namespace hailstone::app {
namespace {
UiHost& host(lua_State* L) { return *static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1))); }
void text(lua_State* L, const char* key, std::string_view value) {
    lua_pushlstring(L, value.data(), value.size()); lua_setfield(L, -2, key);
}
void integer(lua_State* L, const char* key, std::uint64_t value) {
    lua_pushinteger(L, static_cast<lua_Integer>(value)); lua_setfield(L, -2, key);
}
void boolean(lua_State* L, const char* key, bool value) {
    lua_pushboolean(L, value); lua_setfield(L, -2, key);
}
bool token(lua_State* L, int at, std::uint64_t& value) {
    if (lua_type(L, at) != LUA_TSTRING) return false;
    std::size_t length = 0;
    const auto* begin = lua_tolstring(L, at, &length);
    if (!length || length > 20 || (length > 1 && *begin == '0')) return false;
    const auto parsed = std::from_chars(begin, begin + length, value);
    return parsed.ec == std::errc{} && parsed.ptr == begin + length;
}
bool index(lua_State* L, int at, std::uint32_t& value) {
    if (!lua_isinteger(L, at)) return false;
    const auto input = lua_tointeger(L, at);
    if (input < 0 || static_cast<std::uint64_t>(input) > std::numeric_limits<std::uint32_t>::max()) return false;
    value = static_cast<std::uint32_t>(input); return true;
}
int result(lua_State* L, bool accepted, std::string_view why = {}) {
    lua_pushboolean(L, accepted);
    if (why.empty()) lua_pushnil(L); else lua_pushlstring(L, why.data(), why.size());
    return 2;
}
net::GameConnection* connection(lua_State* L) {
    auto& h = host(L); return h.in_game ? h.conn : nullptr;
}
const char* status(net::AchievementStatus value) {
    switch (value) {
    case net::AchievementStatus::Completed: return "Completed";
    case net::AchievementStatus::Open: return "Open";
    case net::AchievementStatus::Locked: return "Locked";
    case net::AchievementStatus::Hidden: return "Hidden";
    }
    return "Unknown";
}
void state_header(lua_State* L, const net::AchievementState* state) {
    boolean(L, "known", state != nullptr);
    text(L, "status", state ? status(state->status) : "Unknown");
    if (state && state->completed_at) integer(L, "completion_time", *state->completed_at);
}
void push_components(lua_State* L, const net::AchievementDefinition& definition, const net::AchievementState* state) {
    lua_newtable(L);
    lua_Integer row = 0;
    for (std::size_t type = 0; type < definition.components.size(); ++type) {
        for (std::size_t i = 0; i < definition.components[type].size(); ++i) {
            const auto& component = definition.components[type][i];
            lua_createtable(L, 0, 11);
            integer(L, "id", component.id); integer(L, "type", type);
            text(L, "name", component.name); text(L, "description", component.description);
            integer(L, "required_count", component.required_count);
            integer(L, "display_order", component.display_order);
            const bool satisfied_known = state && type < 3 && i < state->satisfied[type].size();
            boolean(L, "satisfied_known", satisfied_known);
            boolean(L, "satisfied", satisfied_known && state->satisfied[type][i] != 0);
            const bool count_known = state && type < 3 && i < state->counts[type].size() && state->counts[type][i].has_value();
            boolean(L, "count_known", count_known);
            if (count_known) {
                integer(L, "count", *state->counts[type][i]);
                text(L, "count_text", std::to_string(*state->counts[type][i]));
            }
            lua_rawseti(L, -2, ++row);
        }
    }
    lua_setfield(L, -2, "components");
}
void push_catalog(lua_State* L, const net::AchievementCatalog& catalog) {
    lua_createtable(L, 0, 2);
    lua_createtable(L, static_cast<int>(catalog.categories.size()), 0);
    lua_Integer n = 0;
    for (const auto& category : catalog.categories) {
        lua_createtable(L, 0, 9);
        integer(L, "id", category.id); integer(L, "parent_id", category.parent_id);
        text(L, "name", category.name); text(L, "description", category.description); text(L, "icon", category.icon);
        integer(L, "display_order", category.display_order);
        lua_createtable(L, static_cast<int>(category.child_ids.size()), 0);
        lua_Integer child = 0;
        for (auto id : category.child_ids) { lua_pushinteger(L, id); lua_rawseti(L, -2, ++child); }
        lua_setfield(L, -2, "child_ids");
        lua_createtable(L, static_cast<int>(category.associations.size()), 0);
        lua_Integer association = 0;
        for (const auto& item : category.associations) {
            lua_createtable(L, 0, 3);
            integer(L, "achievement_id", item.achievement_id); text(L, "text", item.text);
            integer(L, "display_order", item.display_order); lua_rawseti(L, -2, ++association);
        }
        lua_setfield(L, -2, "associations"); lua_rawseti(L, -2, ++n);
    }
    lua_setfield(L, -2, "categories");
    lua_createtable(L, static_cast<int>(catalog.definitions.size()), 0);
    for (std::size_t i = 0; i < catalog.definitions.size(); ++i) {
        const auto& definition = catalog.definitions[i];
        lua_createtable(L, 0, 9);
        integer(L, "index", i); integer(L, "id", definition.id);
        text(L, "name", definition.name); text(L, "description", definition.description);
        integer(L, "icon_id", definition.icon_id); integer(L, "points", definition.points);
        boolean(L, "has_reward", definition.has_reward); boolean(L, "persistent", definition.persistent);
        integer(L, "version", definition.version); lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "definitions");
}
// state = hs.Achievements([catalogToken, stateRevision [, into]])   -- the achievements: shown, detail_shown, ready,
// progress_ready, refresh_pending, message, catalog_token and state_revision (opaque), catalog_unchanged and
// states_unchanged (the tokens given are current), selected_index (with a selection), detail_source, comparison
// { pending, ready, name, message }, catalog (only when the token given is not current) and states (only when the
// revision given is not current), points, total_points, completed, total. `into`, a table the caller keeps from frame
// to frame, is filled in place instead of a new table (app/lua_ui_record.h: every field written, catalog and states
// nil while unchanged; comparison refilled in place).
int Achievements(lua_State* L) {
    auto* c = connection(L);
    if (!c) { lua_pushnil(L); return 1; }
    const auto& model = c->achievements();
    lua_settop(L, 3);
    std::uint64_t cached_catalog = 0, cached_state = 0;
    const bool catalog_unchanged = token(L, 1, cached_catalog) && cached_catalog == model.catalog_token();
    const bool states_unchanged = token(L, 2, cached_state) && cached_state == model.revision();
    record::push(L, 3, 20);
    boolean(L, "shown", model.shown()); boolean(L, "detail_shown", model.detail_shown());
    boolean(L, "ready", model.primary_ready()); boolean(L, "progress_ready", model.progress_ready());
    boolean(L, "refresh_pending", c->achievement_refresh_pending());
    record::text(L, "message", c->achievement_message());
    record::text(L, "catalog_token", std::to_string(model.catalog_token()));
    record::text(L, "state_revision", std::to_string(model.revision()));
    boolean(L, "catalog_unchanged", catalog_unchanged); boolean(L, "states_unchanged", states_unchanged);
    const auto& selection = model.selection();
    if (selection) integer(L, "selected_index", selection->definition_index);
    else record::clear(L, "selected_index");
    text(L, "detail_source", selection && selection->source == game::AchievementDetailSource::Link ? "link" : "own");
    record::subrecord(L, "comparison", 4);
    boolean(L, "pending", model.compare_pending()); boolean(L, "ready", !model.comparison_name().empty());
    record::text(L, "name", model.comparison_name()); record::text(L, "message", c->achievement_message());
    lua_pop(L, 1); // (the comparison, in its field)
    if (!catalog_unchanged && model.catalog()) { push_catalog(L, *model.catalog()); lua_setfield(L, -2, "catalog"); }
    else record::clear(L, "catalog");
    std::uint64_t points = 0, total_points = 0, completed = 0, total = 0;
    if (!states_unchanged) record::list(L, "states", model.states().size());
    for (std::size_t i = 0; i < model.states().size(); ++i) {
        const auto& state = model.states()[i];
        if (!states_unchanged) {
            lua_createtable(L, 0, 3); state_header(L, state ? &*state : nullptr);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        if (!state || state->status == net::AchievementStatus::Hidden || !model.catalog()) continue;
        ++total; total_points += model.catalog()->definitions[i].points;
        if (state->status == net::AchievementStatus::Completed) { ++completed; points += model.catalog()->definitions[i].points; }
    }
    if (!states_unchanged) lua_setfield(L, -2, "states");
    else record::clear(L, "states");
    integer(L, "points", points); integer(L, "total_points", total_points);
    integer(L, "completed", completed); integer(L, "total", total);
    return 1;
}
int Achievement(lua_State* L) {
    auto* c = connection(L);
    if (!c) { lua_pushnil(L); return 1; }
    const auto& model = c->achievements();
    const auto& selected = model.selection();
    if (!model.catalog() || !selected || selected->catalog_token != model.catalog_token() ||
        selected->definition_index >= model.catalog()->definitions.size()) { lua_pushnil(L); return 1; }
    const auto& definition = model.catalog()->definitions[selected->definition_index];
    lua_createtable(L, 0, 16);
    boolean(L, "shown", model.detail_shown()); text(L, "revision", std::to_string(model.detail_revision()));
    text(L, "catalog_token", std::to_string(model.catalog_token())); integer(L, "definition_index", selected->definition_index);
    const bool linked = selected->source == game::AchievementDetailSource::Link;
    text(L, "source", linked ? "link" : "own");
    text(L, "player_name", linked && model.detail_link() ? model.detail_link()->player_name : c->character_name());
    integer(L, "id", definition.id); text(L, "name", definition.name); text(L, "description", definition.description);
    integer(L, "icon_id", definition.icon_id); integer(L, "points", definition.points); boolean(L, "has_reward", definition.has_reward);
    lua_createtable(L, 0, 4); state_header(L, model.detail_state()); push_components(L, definition, model.detail_state());
    lua_setfield(L, -2, "own");
    if (!linked && selected->definition_index < model.comparison().size()) {
        const auto& state = model.comparison()[selected->definition_index];
        lua_createtable(L, 0, 5); text(L, "name", model.comparison_name()); state_header(L, &state);
        push_components(L, definition, &state); lua_setfield(L, -2, "comparison");
    }
    return 1;
}
bool arguments(lua_State* L, std::uint64_t& epoch, std::uint32_t& definition) {
    return token(L, 1, epoch) && index(L, 2, definition);
}
int AchievementOpen(lua_State* L) {
    auto* c = connection(L); std::uint64_t epoch = 0; std::uint32_t definition = 0;
    const bool accepted = c && arguments(L, epoch, definition) && c->open_achievement(epoch, definition);
    return result(L, accepted, accepted ? "" : "The achievement catalog changed or this entry is unavailable.");
}
int AchievementRefresh(lua_State* L) {
    auto* c = connection(L); const bool accepted = c && c->request_achievements();
    return result(L, accepted, accepted ? "" : "Achievements cannot be refreshed in this session yet.");
}
int AchievementCompare(lua_State* L) {
    auto* c = connection(L); const bool accepted = c && c->compare_achievements();
    return result(L, accepted, accepted ? "" : "Select a nearby player and wait for any earlier comparison reply.");
}
int AchievementClearCompare(lua_State* L) {
    auto* c = connection(L); if (c) c->clear_achievement_comparison();
    return result(L, c != nullptr);
}
int AchievementReward(lua_State* L) {
    auto* c = connection(L); std::uint64_t epoch = 0; std::uint32_t definition = 0;
    const bool accepted = c && arguments(L, epoch, definition) && c->achievement_reward(epoch, definition);
    return result(L, accepted, accepted ? "" : "The selected achievement reward is unavailable in this view.");
}
int AchievementLink(lua_State* L) {
    auto* c = connection(L); std::uint64_t epoch = 0; std::uint32_t definition = 0;
    if (c && arguments(L, epoch, definition)) {
        if (auto link = c->achievement_chat_link(epoch, definition)) {
            request_chat_link_all({std::move(*link)}); return result(L, true);
        }
    }
    return result(L, false, "The selected achievement state is not ready to link.");
}
class AchievementsWindow final : public StockWindow {
public:
    AchievementsWindow() : StockWindow("AchievementsWnd", false) {}
    bool shown(UiHost& h) override { return h.conn && h.in_game && h.conn->achievements().shown(); }
    void set_shown(UiHost& h, bool show) override { if (h.conn) h.conn->show_achievements(show); }
    std::string title(UiHost&) override { return "Achievements"; }
};
class AchievementWindow final : public StockWindow {
public:
    AchievementWindow() : StockWindow("AchievementWnd", false) {}
    bool shown(UiHost& h) override { return h.conn && h.in_game && h.conn->achievements().detail_shown(); }
    void set_shown(UiHost& h, bool show) override {
        if (!h.conn) return;
        if (!show) h.conn->close_achievement_detail();
        else h.conn->reopen_achievement_detail();
    }
    std::string title(UiHost&) override { return "Achievement"; }
};
} // namespace
void add_achievement_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<AchievementsWindow>()); out.push_back(std::make_unique<AchievementWindow>());
}
void register_achievement_api(lua_State* L, UiHost& h) {
    static const luaL_Reg functions[] = {{"Achievements", Achievements}, {"Achievement", Achievement},
        {"AchievementOpen", AchievementOpen}, {"AchievementRefresh", AchievementRefresh},
        {"AchievementCompare", AchievementCompare}, {"AchievementClearCompare", AchievementClearCompare},
        {"AchievementReward", AchievementReward}, {"AchievementLink", AchievementLink}, {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h); luaL_setfuncs(L, functions, 1);
}
} // namespace hailstone::app
