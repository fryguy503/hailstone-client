#include "app/lua_ui_windows.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <string_view>

#include <lua.hpp>

#include "app/lua_ui_record.h"
#include "net/game_connection.h"
#include "plugin/lua_keep.h"

namespace hailstone::app {
namespace {
UiHost& host(lua_State* L) { return *static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1))); }

void text(lua_State* L, const char* key, std::string_view value) {
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, key);
}
// A number as text, made on the stack (no std::string alive across the push: plugin/lua_keep.h).
void number_text(lua_State* L, const char* key, std::uint64_t value) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, value);
    text(L, key, std::string_view(buf, static_cast<std::size_t>(r.ptr - buf)));
}
void integer(lua_State* L, const char* key, std::uint32_t value) {
    lua_pushinteger(L, static_cast<lua_Integer>(value));
    lua_setfield(L, -2, key);
}
void boolean(lua_State* L, const char* key, bool value) {
    lua_pushboolean(L, value);
    lua_setfield(L, -2, key);
}
bool token(lua_State* L, int at, std::uint64_t& value) {
    // Opaque identities never pass through Lua floating-point conversion.
    if (lua_type(L, at) != LUA_TSTRING) return false;
    std::size_t length = 0;
    const auto* begin = lua_tolstring(L, at, &length);
    if (!length || length > 20 || (length > 1 && *begin == '0')) return false;
    const auto result = std::from_chars(begin, begin + length, value);
    return result.ec == std::errc{} && result.ptr == begin + length;
}
bool number(lua_State* L, int at, std::uint32_t& value) {
    if (!lua_isinteger(L, at)) return false;
    const auto input = lua_tointeger(L, at);
    if (input < 0 || static_cast<std::uint64_t>(input) > std::numeric_limits<std::uint32_t>::max()) return false;
    value = static_cast<std::uint32_t>(input);
    return true;
}
bool lane(lua_State* L, int at, game::RewardLane& value) {
    if (lua_type(L, at) != LUA_TSTRING) return false;
    std::size_t length = 0;
    const auto* input = lua_tolstring(L, at, &length);
    const std::string_view name(input, length);
    if (name == "pending") value = game::RewardLane::Pending;
    else if (name == "preview") value = game::RewardLane::Preview;
    else return false;
    return true;
}
int result(lua_State* L, bool accepted, std::string_view why = {}) {
    lua_pushboolean(L, accepted);
    if (why.empty()) lua_pushnil(L);
    else lua_pushlstring(L, why.data(), why.size());
    return 2;
}
const char* kind_label(net::RewardEntryType type) {
    using T = net::RewardEntryType;
    switch (type) {
    case T::Text: return "Reward";
    case T::Money: return "Money";
    case T::Item: return "Item";
    case T::Experience: return "Experience";
    case T::AAAbility: return "Alternate ability";
    case T::AAPoints: return "Alternate advancement points";
    case T::GenericPoints: return "Points";
    }
    return "Reward";
}
std::string scaled(std::uint32_t value, float multiplier) {
    // RoF2's numeric reward renderers use signed DWORDs and truncate the
    // multiplied value toward zero (0x464290/0x464740/0x464900). Keep the
    // original wire values separately and avoid undefined overflow in the host.
    const double product = std::trunc(static_cast<double>(std::bit_cast<std::int32_t>(value)) * multiplier);
    if (!std::isfinite(product) || product < std::numeric_limits<std::int32_t>::min() ||
        product >= 2147483648.0) return "Unavailable";
    return std::to_string(static_cast<std::int32_t>(product));
}
std::string details(const net::RewardEntry& entry, float multiplier) {
    using T = net::RewardEntryType;
    const auto& v = entry.values;
    switch (entry.type) {
    case T::Money: {
        const std::array<std::string, 4> amounts = {scaled(v[0], multiplier), scaled(v[1], multiplier),
                                                   scaled(v[2], multiplier), scaled(v[3], multiplier)};
        if (std::any_of(amounts.begin(), amounts.end(), [](const auto& amount) { return amount == "Unavailable"; }))
            return "Amount unavailable";
        return std::format("{}p {}g {}s {}c", amounts[0], amounts[1], amounts[2], amounts[3]);
    }
    case T::AAAbility: return std::format("Ability rank {}, cumulative rank {}", v[0], v[1]);
    case T::AAPoints: return std::format("{} points", scaled(v[0], multiplier));
    case T::GenericPoints: return std::format("{} points", scaled(v[2], multiplier));
    default: return {};
    }
}
// `made`: a string in a box the state owns (plugin/lua_keep.h), for the text this makes.
void push_entry(lua_State* L, const net::RewardEntry& entry, float multiplier, std::string& made) {
    lua_createtable(L, 0, 10);
    integer(L, "entry_id", entry.entry_id);
    integer(L, "string_id", entry.string_id);
    integer(L, "type", static_cast<std::uint32_t>(entry.type));
    integer(L, "flag", entry.flag);
    lua_createtable(L, 4, 0);
    for (std::size_t value = 0; value < entry.values.size(); ++value) {
        lua_pushinteger(L, static_cast<lua_Integer>(entry.values[value]));
        lua_rawseti(L, -2, static_cast<lua_Integer>(value + 1));
    }
    lua_setfield(L, -2, "values");
    text(L, "kind_label", kind_label(entry.type));
    text(L, "description", entry.description);
    plugin::cxx_guard(L, [&] { made = details(entry, multiplier); });
    text(L, "details", made);
    lua_createtable(L, static_cast<int>(entry.items.size()), 0);
    lua_Integer i = 0;
    for (const auto& item : entry.items) {
        lua_createtable(L, 0, 5);
        integer(L, "id", item.id);
        integer(L, "quantity", item.quantity);
        text(L, "name", item.name);
        boolean(L, "lore", item.lore);
        integer(L, "lore_group", item.lore_group);
        lua_rawseti(L, -2, ++i);
    }
    lua_setfield(L, -2, "items");
}
// Fills the lane record on top (app/lua_ui_record.h: refilled in place; tabs nil while unchanged, else a new list
// whenever it has entries).
void fill_lane(lua_State* L, const game::RewardLaneState& state, int cached_at, std::string& made) {
    number_text(L, "revision", state.revision);
    boolean(L, "shown", state.shown);
    boolean(L, "claim_pending", state.claim_pending);
    boolean(L, "claim_uncertain", state.claim_uncertain);
    boolean(L, "inspect_pending", state.inspect_pending);
    record::text(L, "message", state.message);
    std::uint64_t cached = 0;
    const bool unchanged = token(L, cached_at, cached) && cached == state.revision;
    boolean(L, "unchanged", unchanged);
    if (unchanged) {
        record::clear(L, "tabs");
        return;
    }
    record::list(L, "tabs", state.tabs.size());
    lua_Integer tab_index = 0;
    for (const auto& tab : state.tabs) {
        lua_createtable(L, 0, 7);
        number_text(L, "token", tab.token);
        text(L, "title", tab.reward.title);
        integer(L, "pending_id", tab.reward.pending_id);
        integer(L, "set_id", tab.reward.set_id);
        lua_pushnumber(L, tab.reward.multiplier);
        lua_setfield(L, -2, "multiplier");
        boolean(L, "blocked", tab.blocked);
        lua_createtable(L, static_cast<int>(tab.reward.options.size()), 0);
        lua_Integer option_index = 0;
        for (const auto& option : tab.reward.options) {
            lua_createtable(L, 0, 5);
            integer(L, "wire_id", option.wire_id);
            boolean(L, "common", option.common);
            integer(L, "flags", option.flags);
            text(L, "label", option.label);
            lua_createtable(L, static_cast<int>(option.entries.size()), 0);
            lua_Integer entry_index = 0;
            for (const auto& entry : option.entries) {
                push_entry(L, entry, tab.reward.multiplier, made);
                lua_rawseti(L, -2, ++entry_index);
            }
            lua_setfield(L, -2, "entries");
            lua_rawseti(L, -2, ++option_index);
        }
        lua_setfield(L, -2, "options");
        lua_rawseti(L, -2, ++tab_index);
    }
    lua_setfield(L, -2, "tabs");
}
// state = hs.RewardSelection([pendingRevision, previewRevision [, into]])   -- the reward selection windows: shown,
// and pending and preview, each { revision (opaque), shown, claim_pending, claim_uncertain, inspect_pending, message,
// unchanged (the revision given is current), tabs (only when it is not) }. `into`, a table the caller keeps from
// frame to frame, is filled in place instead of a new table, its lane records too (app/lua_ui_record.h: every field
// written; tabs nil while unchanged, else a new list whenever it has entries).
int RewardSelection(lua_State* L) {
    auto& h = host(L);
    if (!h.conn || !h.in_game) { lua_pushnil(L); return 1; }
    const auto& rewards = h.conn->rewards();
    const auto& pending = rewards.lane(game::RewardLane::Pending);
    const auto& preview = rewards.lane(game::RewardLane::Preview);
    // Reserve argument slots before pushing the result; omitted cached tokens
    // must not accidentally refer to the table being constructed.
    lua_settop(L, 3);
    plugin::LuaScratch<std::string> box(L); // (one box for every call: plugin/lua_keep.h)
    std::string& made = *box;
    record::push(L, 3, 3);
    boolean(L, "shown", pending.shown || preview.shown);
    record::subrecord(L, "pending", 8);
    fill_lane(L, pending, 1, made);
    lua_pop(L, 1);
    record::subrecord(L, "preview", 8);
    fill_lane(L, preview, 2, made);
    lua_pop(L, 1);
    box.release();
    return 1;
}
int RewardSelectionClaim(lua_State* L) {
    std::uint64_t revision = 0, tab = 0;
    std::uint32_t option = 0;
    auto& h = host(L);
    if (!h.conn || !h.in_game || !token(L, 1, revision) || !token(L, 2, tab) || !number(L, 3, option))
        return result(L, false, "Invalid or retired reward choice.");
    std::string& why = plugin::lua_keep<std::string>(L); // across the push (plugin/lua_keep.h)
    bool accepted = false;
    plugin::cxx_guard(L, [&] { accepted = h.conn->reward_claim(revision, tab, option, &why); });
    return result(L, accepted, why);
}
int RewardSelectionInspect(lua_State* L) {
    game::RewardLane value;
    std::uint64_t revision = 0, tab = 0;
    std::uint32_t option = 0, entry = 0, item = 0;
    auto& h = host(L);
    if (!h.conn || !h.in_game || !lane(L, 1, value) || !token(L, 2, revision) || !token(L, 3, tab) ||
        !number(L, 4, option) || !number(L, 5, entry) || !number(L, 6, item))
        return result(L, false, "Invalid or retired reward item.");
    std::string& why = plugin::lua_keep<std::string>(L); // across the push (plugin/lua_keep.h)
    bool accepted = false;
    plugin::cxx_guard(L, [&] { accepted = h.conn->reward_inspect(value, revision, tab, option, entry, item, &why); });
    return result(L, accepted, why);
}
int RestorePendingRewards(lua_State* L) {
    auto& h = host(L);
    const bool accepted = h.conn && h.in_game && h.conn->request_pending_rewards();
    return result(L, accepted, accepted ? "" : "Pending rewards could not be requested in this session.");
}
int RewardSelectionClose(lua_State* L) {
    game::RewardLane value;
    auto& h = host(L);
    if (!h.conn || !lane(L, 1, value)) return result(L, false, "Unknown reward page.");
    h.conn->close_reward_selection(value);
    return result(L, true);
}

class RewardSelectionWindow final : public StockWindow {
public:
    RewardSelectionWindow() : StockWindow("RewardSelectionWnd", false) {}
    bool shown(UiHost& h) override {
        return h.conn && h.in_game && (h.conn->rewards().lane(game::RewardLane::Pending).shown ||
                                      h.conn->rewards().lane(game::RewardLane::Preview).shown);
    }
    void set_shown(UiHost& h, bool show) override { if (h.conn) h.conn->show_reward_selection(show); }
    std::string title(UiHost&) override { return "Select Reward"; }
};
} // namespace

void add_reward_selection_window(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<RewardSelectionWindow>());
}
void register_reward_selection_api(lua_State* L, UiHost& h) {
    static const luaL_Reg functions[] = {
        {"RewardSelection", RewardSelection}, {"RewardSelectionClaim", RewardSelectionClaim},
        {"RewardSelectionInspect", RewardSelectionInspect}, {"RestorePendingRewards", RestorePendingRewards},
        {"RewardSelectionClose", RewardSelectionClose}, {nullptr, nullptr}};
    lua_pushlightuserdata(L, &h);
    luaL_setfuncs(L, functions, 1);
}
} // namespace hailstone::app
