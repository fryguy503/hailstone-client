#pragma once

// MacroQuest's Top-Level Objects (TLOs) and data types, answered from this client's state. A TLO (Me, Target,
// Spawn[...], Group, Zone, ...) gives a value; a value of a data type has members (Me.PctHPs, Target.Distance,
// Me.Buff[1].Duration), each member taking an optional index (the text in [] or the Lua call's arguments). The
// same evaluation serves MQ's ${...} text (mq/parse.h), Lua's mq.TLO (mq/lua_mq.h) and the compiled plugins'
// host API (plugin/hailstone_plugin.h).
//
// Names are MacroQuest's (docs: https://docs.macroquest.org/reference/top-level-objects/), case-insensitive as
// in MQ; what is covered is in docs/plugins.md. An unknown TLO or member, or a member of nothing, is nil (MQ's
// NULL), never an error.

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace hailstone::net { class GameConnection; }
namespace hailstone::nav { class NavControl; }

namespace hailstone::mq {

struct Type;
struct Context;

struct Value {
    enum class Kind : std::uint8_t { Nil, Bool, Int, Float, String, Object };
    Kind kind = Kind::Nil;
    bool b = false;
    std::int64_t i = 0; // Int; Object: what it refers to (a spawn id, a buff slot, a spell id ...)
    double f = 0;       // Float; Object: a second number (a heading's degrees, a gem's spell ...)
    std::string s;      // String; Object: a name it refers to
    const Type* type = nullptr;

    static Value nil() { return {}; }
    static Value boolean(bool v) {
        Value x;
        x.kind = Kind::Bool;
        x.b = v;
        return x;
    }
    static Value integer(std::int64_t v) {
        Value x;
        x.kind = Kind::Int;
        x.i = v;
        return x;
    }
    static Value number(double v) {
        Value x;
        x.kind = Kind::Float;
        x.f = v;
        return x;
    }
    static Value string(std::string v) {
        Value x;
        x.kind = Kind::String;
        x.s = std::move(v);
        return x;
    }
    static Value object(const Type* t, std::int64_t handle, double second = 0, std::string name = {}) {
        if (!t) return nil();
        Value x;
        x.kind = Kind::Object;
        x.type = t;
        x.i = handle;
        x.f = second;
        x.s = std::move(name);
        return x;
    }
    bool is_nil() const { return kind == Kind::Nil; }
};

// A member: (context, the value it is a member of, the index text; empty = none).
using MemberFn = std::function<Value(const Context&, const Value& self, std::string_view index)>;
using TloFn = std::function<Value(const Context&, std::string_view index)>;

struct CaseLess {
    bool operator()(std::string_view a, std::string_view b) const;
    using is_transparent = void;
};

struct Type {
    std::string name;
    std::map<std::string, MemberFn, CaseLess> members;
    // What the value is when used as a plain value (MQ's ToString / a Lua call without arguments): a spawn's
    // CleanName, a spell's Name, ... ; null: the type's name.
    std::function<Value(const Context&, const Value& self)> to_value;
    // Members not found here are looked up there (MQ's inheritance: character → spawn, buff → spell).
    // `convert` turns the value into the parent's (a buff slot into its spell); null keeps it.
    const Type* parent = nullptr;
    std::function<Value(const Context&, const Value& self)> convert;
    // Members not in `members` (a plugin's TLO answers any name it knows): nil = no such member.
    std::function<Value(const Context&, const Value& self, std::string_view name, std::string_view index)> any_member;
};

class Registry;

// What the TLOs read. The client fills it; null pieces answer nil.
// An ability the player activates other than a spell, as the Ability Book sees it (app/lua_ui_abilitybook.cpp):
// Me.AbilityReady (a skill), Me.CombatAbilityReady (a discipline), Me.AltAbilityReady (an activated AA).
enum class AbilityKind : std::uint8_t { Skill, Discipline, AltAbility };
struct AbilityState {
    std::string name;
    bool ready = false;            // its reuse timer has run out; its costs are the script's to check
    std::int64_t remaining_ms = 0; // until it is ready
    std::int64_t reuse_ms = 0;
};

struct Context {
    const Registry* registry = nullptr;
    const net::GameConnection* conn = nullptr;
    nav::NavControl* nav = nullptr; // the client's navigation (/nav, the Navigation TLO); null: none
    std::function<bool(std::string_view window)> window_open; // Window[name].Open
    std::function<bool(std::string_view plugin)> plugin_loaded; // Plugin[name].IsLoaded
    std::function<bool()> foreground; // EverQuest.Foreground: the game's window has the focus; null: false
    // One of the player's abilities by name (or an AA's group, a skill's number); nullopt: the player has none such.
    std::function<std::optional<AbilityState>(AbilityKind, std::string_view name)> ability;
    // Line of sight in the zone between two points (world coordinates, game/coords.h): nullopt while the zone's
    // collision is not loaded. LineOfSight[y,x,z:y,x,z].
    std::function<std::optional<bool>(const std::array<float, 3>& from, const std::array<float, 3>& to)> line_of_sight;
    // The client's own check from the player's eyes to a spawn, the one its abilities use (PlayerBase::CanSee):
    // Spawn.LineOfSight. nullopt: no such spawn, or no collision loaded.
    std::function<std::optional<bool>(std::uint32_t spawn)> can_see;
    std::string server_name;
    std::string ui_skin = "default";
    // MacroQuest.Path[config] and Hailstone.Path[config]: the folder the scripts' settings go in (<data folder>/scripts/config, as UTF-8, with no
    // trailing separator, as MQ gives its paths); empty answers nil.
    std::string config_path;
};

class Registry {
public:
    Registry();
    ~Registry();

    // A new type (or the one of that name); keep the pointer, it stays valid.
    Type& type(const std::string& name);
    const Type* find_type(std::string_view name) const;
    void add_tlo(std::string name, TloFn fn);
    void remove_tlo(std::string_view name);
    bool has_tlo(std::string_view name) const;
    std::vector<std::string> tlos() const;

    Value tlo(const Context& ctx, std::string_view name, std::string_view index = {}) const;
    Value member(const Context& ctx, const Value& self, std::string_view name, std::string_view index = {}) const;
    // An object's plain value (its type's to_value, until it is not an object); other values as they are.
    Value plain(const Context& ctx, const Value& v) const;
    // MQ's text for a value: TRUE / FALSE, floats with two decimals, NULL for nil.
    std::string text(const Context& ctx, const Value& v) const;

    // Completion for an editor: `path` is MQ's text up to the cursor ("Me.Buff[1].Du", "Tar"). The TLOs, or the
    // members of the value before the last '.', whose names start with the last part (ignoring case), sorted. The
    // value is read from the game; a TLO that is NULL now (no target) offers its type's members.
    std::vector<std::string> complete(const Context& ctx, std::string_view path) const;

    // The built-in types every value has members of: int, float, string, bool (MQ's pure types).
    const Type* int_type() const { return int_; }
    const Type* float_type() const { return float_; }
    const Type* string_type() const { return string_; }
    const Type* bool_type() const { return bool_; }

private:
    std::map<std::string, std::unique_ptr<Type>, CaseLess> types_;
    std::map<std::string, TloFn, CaseLess> tlos_;
    const Type *int_ = nullptr, *float_ = nullptr, *string_ = nullptr, *bool_ = nullptr;
};

// MQ's index splitting: "a,b" → {"a", "b"}, trimmed.
std::vector<std::string> split_index(std::string_view index);
// A whole-number index ("3"), or nullopt-like -1 when the text is not one.
bool parse_int(std::string_view text, std::int64_t& out);
bool parse_float(std::string_view text, double& out);
// A number cut to a whole one (toward zero), as ${Int[...]} and float.Int give it; nullopt for NaN, an infinity or one
// past int64 (std::from_chars reads "nan", "inf" and 1e300, and a cast of those is undefined).
std::optional<std::int64_t> whole_number(double d);
// A number read from text as T when it is finite and fits T (an integer T: cut toward zero); nullopt for NaN, an
// infinity or one beyond T, whose cast would be undefined. For ids, door numbers, coordinates.
template <typename T> std::optional<T> in_range(double d) {
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(d) || std::fabs(d) > static_cast<double>(std::numeric_limits<T>::max())) return std::nullopt;
        return static_cast<T>(d);
    } else {
        static_assert(std::is_integral_v<T> && sizeof(T) <= 4, "whole_number for 64 bits");
        const double t = std::trunc(d); // (NaN stays NaN and fails below)
        if (!(t >= static_cast<double>(std::numeric_limits<T>::min()) && t <= static_cast<double>(std::numeric_limits<T>::max())))
            return std::nullopt;
        return static_cast<T>(t);
    }
}
bool iequals(std::string_view a, std::string_view b);
// Case-insensitive substring.
bool icontains(std::string_view haystack, std::string_view needle);

// The game's TLOs and types (mq/tlo_game.cpp): Me, Target, Spawn, SpawnCount, NearestSpawn, Group, Zone, Spell,
// Cursor, FindItem, FindItemCount, Window, Time, GameTime, EverQuest, MacroQuest, Hailstone, Int, Float, Bool, String, Math, If,
// Range, Plugin, ...
void register_game_tlos(Registry& r);

} // namespace hailstone::mq
