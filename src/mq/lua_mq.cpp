#include "mq/lua_mq.h"
#include "mq/lua_script.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <iterator>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "core/ini_file.h"
#include "core/log.h"
#include "core/saturate.h"
#include "core/sha256.h"
#include "core/windows_names.h"
#include "game/eq_names.h"
#include "game/item_links.h"
#include "game/slash_commands.h"
#include "game/world_state.h"
#include "mq/parse.h"
#include "plugin/lua_keep.h"
#include "plugin/ui_bridge.h"
#include "net/game_connection.h"
#include "ui/lua/lua_host.h"

namespace hailstone::mq {

namespace fs = std::filesystem;

namespace {

char kExitKey; // mq.exit's error value

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

// A script is found whatever the case of its name, on a case-sensitive file system as on Windows and macOS: the
// loader asks for a lower-cased name, and an add-on keeps its package's spelling on disk (scripts/AutoLoot). The
// exact spelling wins; otherwise each part of the path must have exactly one match that differs only in case, so
// Foo/ beside foo/ is refused rather than one picked.
std::optional<fs::path> script_spelled_path(const fs::path& root, const fs::path& relative) {
    if (relative.empty() || relative.has_root_path()) return std::nullopt;
    const auto& native = relative.native();
    // Nothing rooted (a leading backslash is not, to Linux), no drive or stream (':'), no NUL.
    if (native.front() == static_cast<fs::path::value_type>('\\') ||
        native.find(static_cast<fs::path::value_type>(':')) != fs::path::string_type::npos ||
        native.find(fs::path::value_type{}) != fs::path::string_type::npos)
        return std::nullopt;
    for (const auto& part : relative)
        if (part == "." || part == "..") return std::nullopt;
    // As UTF-8: path::string() throws on Windows for a name the code page cannot hold.
    auto utf8 = [](const fs::path& p) {
        const auto u = p.u8string();
        return std::string(u.begin(), u.end());
    };
    fs::path current = root;
    for (const auto& part : relative) {
        std::error_code ec;
        const fs::path exact = current / part;
        if (fs::exists(exact, ec)) {
            current = exact;
            continue;
        }
        const std::string want = utf8(part);
        std::optional<fs::path> found;
        for (fs::directory_iterator it(current, ec), end; !ec && it != end; it.increment(ec)) {
            if (!iequals(utf8(it->path().filename()), want)) continue;
            if (found) return std::nullopt; // Foo and foo: neither
            found = it->path();
        }
        if (!found || ec) return std::nullopt;
        current = *found;
    }
    return current;
}

// The running one first: one stopped while pulse() or imgui() walked the list stays in it until reap().
Script* ScriptHost::Impl::find(std::string_view name) const {
    Script* stopped = nullptr;
    for (const auto& s : scripts)
        if (iequals(s->name, name)) {
            if (!s->stop_requested) return s.get();
            if (!stopped) stopped = s.get();
        }
    return stopped;
}

namespace {

void* limited_alloc(void* ud, void* ptr, std::size_t old_size, std::size_t new_size) {
    auto* s = static_cast<Script*>(ud);
    std::size_t old = ptr ? old_size : 0;
    if (new_size == 0) {
        std::free(ptr);
        s->memory -= old;
        return nullptr;
    }
    if (s->limited > 0 && new_size > old && s->memory + (new_size - old) > kMemoryLimit) return nullptr;
    void* p = std::realloc(ptr, new_size);
    if (p) s->memory = s->memory - old + new_size;
    return p;
}

// Past kSliceMs it yields the host's own thread (ScriptRun's), which the host resumes next frame; a coroutine the
// script made itself inherits the hook but is never yielded by it (that would return to the script's own
// coroutine.resume with no values, mid-work): there only the runaway check holds.
void preempt_hook(lua_State* L, lua_Debug*) {
    Script& s = Script::of(L);
    repair_limit(s);
    auto ran = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s.slice_start).count();
    if (!s.runaway && ran >= kSliceMs && L == s.resumed && lua_isyieldable(L)) {
        lua_yield(L, 0);
        return;
    }
    check_runaway(L, s, preempt_hook, ran, [&] {
        if (s.runaway_ms < 1000) return std::format("ran longer than {} ms", s.runaway_ms);
        return std::format("ran {} seconds without a chance to pause", ran / 1000);
    });
}

void watch(lua_State* thread) { lua_sethook(thread, preempt_hook, LUA_MASKCOUNT, 1000); }

// A long string match (the vendored Lua's check, ui::lua::kMatchCheckKey) is held to runaway_ms as Lua code is; it
// cannot pause mid-match. It stops the script as the hooks' runaway does (sticky: a pcall around it passes it on).
int match_check(lua_State* L) {
    Script& s = Script::of(L);
    auto ran = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s.slice_start).count();
    lua_Hook hook = lua_gethook(L);
    check_runaway(L, s, hook ? hook : preempt_hook, ran,
                  [&] { return std::format("ran longer than {} ms", s.runaway_ms); });
    return 0;
}

int raise_runaway_again(lua_State* L) {
    lua_pushstring(L, Script::of(L).runaway_why.c_str());
    return lua_error(L);
}

int runaway_safe_done(lua_State* L, int, lua_KContext) {
    if (Script::of(L).runaway) return raise_runaway_again(L);
    return lua_gettop(L);
}

} // namespace

// (lua_callk: a pcall around mq.delay in the script's main coroutine still yields through it.)
int runaway_safe_call(lua_State* L) {
    if (Script::of(L).runaway) return raise_runaway_again(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_callk(L, lua_gettop(L) - 1, LUA_MULTRET, 0, runaway_safe_done);
    return runaway_safe_done(L, LUA_OK, 0);
}

namespace {

// setmetatable without __gc (upvalue 1: Lua's own). A finalizer runs whenever the collector likes, with no hook
// (Lua turns hooks off in it, so a loop there is never stopped), and at close, after the script's hooks are gone.
int l_setmetatable(lua_State* L) {
    if (lua_type(L, 2) == LUA_TTABLE) {
        lua_pushliteral(L, "__gc");
        const bool gc = lua_rawget(L, 2) != LUA_TNIL;
        lua_pop(L, 1);
        if (gc) return luaL_error(L, "setmetatable: a script's tables cannot have __gc");
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

const Context& ctx_of(lua_State* L) { return Script::of(L).host->env.context(); }

const Registry& reg_of(lua_State* L) { return *Script::of(L).host->env.registry; }

void need(lua_State* L, std::uint32_t cap, const char* what) { need_caps(L, cap, what); }

// ---------------------------------------------------------------- mq.TLO values

struct TloRef {
    enum class Kind : std::uint8_t { Tlo, Member, Value } kind = Kind::Tlo;
    std::string name;
    Value base;
    // Keep the policy identity of a resolved member or indexed result across server rule changes.
    std::string lookup_root;
};

std::string_view root_name(const TloRef& r) { return r.kind == TloRef::Kind::Tlo ? r.name : r.lookup_root; }

constexpr const char* kTloMeta = "hailstone.mq.tlo";

// A TLO value is pushed in two steps, as Lua is built as C and an error (a failed allocation too) longjmps past the
// C++ objects of the frames it leaves: ref_slot pushes the metatable and the value's memory, which may raise while no
// TloRef or Value exists yet; ref_fill builds the TloRef there and sets the metatable, which cannot raise.
void* ref_slot(lua_State* L) {
    luaL_getmetatable(L, kTloMeta);
    return lua_newuserdatauv(L, sizeof(TloRef), 0);
}

void ref_fill(lua_State* L, void* mem, TloRef&& ref) {
    new (mem) TloRef(std::move(ref));
    lua_rotate(L, -2, 1); // the value, then its metatable
    lua_setmetatable(L, -2);
}

void push_ref(lua_State* L, TloRef::Kind kind, const char* name) {
    void* mem = ref_slot(L);
    ref_fill(L, mem, TloRef{kind, name ? name : "", {}, {}});
}

TloRef& check_ref(lua_State* L, int i) { return *static_cast<TloRef*>(luaL_checkudata(L, i, kTloMeta)); }

Value resolve(lua_State* L, const TloRef& r, std::string_view index) {
    need(L, plugin::kCapRead, "mq.TLO");
    const Context& ctx = ctx_of(L);
    if (!r.lookup_root.empty() && !reg_of(L).allow_tlo(ctx, r.lookup_root)) return Value::nil();
    switch (r.kind) {
    case TloRef::Kind::Tlo: return reg_of(L).tlo(ctx, r.name, index);
    case TloRef::Kind::Member: return reg_of(L).member(ctx, r.base, r.name, index);
    case TloRef::Kind::Value: return r.base;
    }
    return Value::nil();
}

// A value's plain form (Registry::plain) for push_plain, its text in the Script's scratch string: made before
// anything is pushed, and gone (but for the scratch) when push_plain may raise.
struct Plain {
    Value::Kind kind = Value::Kind::Nil;
    bool b = false;
    std::int64_t i = 0;
    double f = 0;
};

Plain plain_of(lua_State* L, const Value& v) {
    Value p = reg_of(L).plain(ctx_of(L), v);
    Script& s = Script::of(L);
    if (p.kind == Value::Kind::String) s.scratch = std::move(p.s);
    else if (p.kind == Value::Kind::Object) s.scratch = p.type ? p.type->name : std::string();
    return {p.kind, p.b, p.i, p.f};
}

void push_plain(lua_State* L, const Plain& p) {
    const std::string& text = Script::of(L).scratch;
    switch (p.kind) {
    case Value::Kind::Nil: lua_pushnil(L); return;
    case Value::Kind::Bool: lua_pushboolean(L, p.b); return;
    case Value::Kind::Int: lua_pushinteger(L, p.i); return;
    case Value::Kind::Float: lua_pushnumber(L, p.f); return;
    case Value::Kind::String:
    case Value::Kind::Object: lua_pushlstring(L, text.data(), text.size()); return;
    }
}

// Arguments from `first` on, as MQ's index text ("a,b").
// (Lua errors are longjmps here: the text is built in a Lua buffer, so no C++ object is left half alive.)
void push_index_text(lua_State* L, int first, int last) {
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = first; i <= last; ++i) {
        if (i > first) luaL_addchar(&b, ',');
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
}

int tlo_gc(lua_State* L) {
    check_ref(L, 1).~TloRef();
    return 0;
}

// (Each pushes once its C++ objects are gone or moved into what was pushed: ref_slot, plain_of, the scratch string.
// resolve raises only before it makes a Value.)
int tlo_index(lua_State* L) {
    TloRef& r = check_ref(L, 1);
    if (lua_type(L, 2) != LUA_TSTRING) {
        lua_pushnil(L);
        return 1;
    }
    void* mem = ref_slot(L);
    Value base = resolve(L, r, {});
    ref_fill(L, mem, TloRef{TloRef::Kind::Member, lua_tostring(L, 2), std::move(base), std::string(root_name(r))});
    return 1;
}

int tlo_call(lua_State* L) {
    const TloRef& r = check_ref(L, 1);
    need(L, plugin::kCapRead, "mq.TLO");
    int top = lua_gettop(L);
    if (top < 2) {
        const Plain p = plain_of(L, resolve(L, r, {}));
        push_plain(L, p);
        return 1;
    }
    push_index_text(L, 2, top);
    std::size_t n = 0;
    const char* index = lua_tolstring(L, -1, &n);
    void* mem = ref_slot(L);
    Value v = resolve(L, r, std::string_view(index, n));
    ref_fill(L, mem, TloRef{TloRef::Kind::Value, {}, std::move(v), std::string(root_name(r))});
    return 1;
}

int tlo_tostring(lua_State* L) {
    TloRef& r = check_ref(L, 1);
    Script& s = Script::of(L);
    s.scratch = reg_of(L).text(ctx_of(L), resolve(L, r, {}));
    lua_pushlstring(L, s.scratch.data(), s.scratch.size());
    return 1;
}

// An operand's plain value: a TLO value's, or the Lua value itself.
void push_operand(lua_State* L, int i) {
    if (luaL_testudata(L, i, kTloMeta)) {
        const Plain p = plain_of(L, resolve(L, *static_cast<TloRef*>(lua_touserdata(L, i)), {}));
        push_plain(L, p);
    } else {
        lua_pushvalue(L, i);
    }
}

int tlo_concat(lua_State* L) {
    Script& s = Script::of(L);
    for (int i = 1; i <= 2; ++i) {
        if (luaL_testudata(L, i, kTloMeta)) {
            s.scratch = reg_of(L).text(ctx_of(L), resolve(L, *static_cast<TloRef*>(lua_touserdata(L, i)), {}));
            lua_pushlstring(L, s.scratch.data(), s.scratch.size());
        } else {
            luaL_tolstring(L, i, nullptr);
        }
    }
    lua_concat(L, 2);
    return 1;
}

int tlo_compare(lua_State* L, int op) {
    push_operand(L, 1);
    push_operand(L, 2);
    bool numbers = lua_type(L, -1) == LUA_TNUMBER && lua_type(L, -2) == LUA_TNUMBER;
    bool strings = lua_type(L, -1) == LUA_TSTRING && lua_type(L, -2) == LUA_TSTRING;
    if (op != LUA_OPEQ && !numbers && !strings) return luaL_error(L, "attempt to compare a TLO value that is not a number");
    lua_pushboolean(L, lua_compare(L, -2, -1, op));
    return 1;
}
int tlo_eq(lua_State* L) { return tlo_compare(L, LUA_OPEQ); }
int tlo_lt(lua_State* L) { return tlo_compare(L, LUA_OPLT); }
int tlo_le(lua_State* L) { return tlo_compare(L, LUA_OPLE); }

// mq.TLO.<name>
int tlo_root_index(lua_State* L) {
    if (lua_type(L, 2) != LUA_TSTRING) {
        lua_pushnil(L);
        return 1;
    }
    push_ref(L, TloRef::Kind::Tlo, lua_tostring(L, 2));
    return 1;
}

// ---------------------------------------------------------------- mq functions

int l_print(lua_State* L) {
    int top = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= top; ++i) {
        if (i > 1) luaL_addchar(&b, ' ');
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    Script::of(L).host->say(lua_tostring(L, -1));
    return 0;
}

int l_cmd(lua_State* L) {
    Script& s = Script::of(L);
    // (Raised at the caller of mq.cmd / mq.cmdf: level 2, past the prelude's wrapper.)
    if (s.face_eval) {
        luaL_where(L, 2);
        lua_pushstring(L, "a button's face cannot run commands");
        lua_concat(L, 2);
        return lua_error(L);
    }
    if (s.commands_left == 0) {
        luaL_where(L, 2);
        lua_pushfstring(L, "this server lets one press send at most %d commands", s.press_commands);
        lua_concat(L, 2);
        return lua_error(L);
    }
    std::size_t len = 0;
    const char* given = luaL_checklstring(L, 1, &len);
    if (len >= kMaxCommandLine) {
        luaL_where(L, 2);
        lua_pushfstring(L, "mq.cmd: a command line is at most %d bytes", static_cast<int>(kMaxCommandLine - 1));
        lua_concat(L, 2);
        return lua_error(L);
    }
    if (s.commands_left > 0) --s.commands_left;
    // (The line lives only inside cxx_guard: an exception becomes a Lua error once it is gone.)
    if (s.dry) {
        plugin::cxx_guard(L, [&] {
            std::string line(given, len);
            if ((s.caps & plugin::kCapRead) && !lower(line).starts_with("/noparse")) line = parse_macro_data(ctx_of(L), line);
            std::uint32_t needs = plugin::command_caps(line);
            if ((s.caps & needs) != needs) line += "    (blocked: needs '" + plugin::caps_text(needs & ~s.caps) + "')";
            s.dry->push_back(std::move(line));
        });
        return 0;
    }
    if (!s.host->env.run) return 0;
    plugin::cxx_guard(L, [&] {
        plugin::CallerScope scope(s.caps, s.name, s.tlo_caller);
        s.host->env.run(std::string(given, len));
    });
    return 0;
}

int l_gettime(lua_State* L) {
    lua_pushinteger(L, Script::of(L).host->now());
    return 1;
}

// The longest wait mq.delay keeps (about 285 000 years): longer, math.huge included, is cut to it, so the cast is
// defined and mq.gettime() + ms cannot wrap.
constexpr lua_Integer kMaxDelayMs = 9'000'000'000'000'000;

lua_Integer delay_ms(double v) {
    if (v >= static_cast<double>(kMaxDelayMs)) return kMaxDelayMs;
    if (v <= -static_cast<double>(kMaxDelayMs)) return -kMaxDelayMs;
    return static_cast<lua_Integer>(v);
}

// mq.delay's argument in ms: a number, or text with ms / s / m / h. NaN is a bad time.
int l_delay_ms(lua_State* L) {
    if (lua_isinteger(L, 1)) {
        lua_pushinteger(L, std::clamp(lua_tointeger(L, 1), -kMaxDelayMs, kMaxDelayMs));
        return 1;
    }
    if (lua_type(L, 1) == LUA_TNUMBER) {
        const double v = lua_tonumber(L, 1);
        if (std::isnan(v)) return luaL_error(L, "mq.delay: bad time (NaN)");
        lua_pushinteger(L, delay_ms(v));
        return 1;
    }
    const char* given = luaL_checkstring(L, 1);
    double v = 0;
    bool ok = false;
    {
        std::string t = lower(given);
        double scale = 1;
        auto ends = [&](std::string_view suffix) {
            return t.size() >= suffix.size() && t.compare(t.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        if (ends("ms")) t.resize(t.size() - 2);
        else if (ends("s")) { t.pop_back(); scale = 1000; }
        else if (ends("m")) { t.pop_back(); scale = 60000; }
        else if (ends("h")) { t.pop_back(); scale = 3600000; }
        ok = parse_float(t, v) && !std::isnan(v);
        v *= scale;
    }
    if (!ok) return luaL_error(L, "mq.delay: bad time '%s'", given);
    lua_pushinteger(L, delay_ms(v));
    return 1;
}

// (The text is made in the Script's scratch string, not this frame: the push may raise.)
int l_parse(lua_State* L) {
    need(L, plugin::kCapRead, "mq.parse");
    const char* line = luaL_checkstring(L, 1);
    Script& s = Script::of(L);
    plugin::cxx_guard(L, [&] { s.scratch = parse_macro_data(ctx_of(L), line); });
    lua_pushlstring(L, s.scratch.data(), s.scratch.size());
    return 1;
}

// mq._dry(ms): true in a dry run, which notes the wait instead of waiting.
int l_dry(lua_State* L) {
    Script& s = Script::of(L);
    if (!s.dry) {
        lua_pushboolean(L, 0);
        return 1;
    }
    long long ms = luaL_checkinteger(L, 1);
    s.dry->push_back(std::format("(wait {} ms)", ms));
    lua_pushboolean(L, 1);
    return 1;
}

int l_exit(lua_State* L) {
    lua_pushlightuserdata(L, &kExitKey);
    return lua_error(L);
}

void add_event(lua_State* L, Script& s, const char* name, std::string_view pattern, bool keep_links);

int l_event(lua_State* L) {
    need(L, plugin::kCapEvents, "mq.event");
    Script& s = Script::of(L);
    std::size_t name_n = 0, pattern_n = 0;
    const char* name_text = luaL_checklstring(L, 1, &name_n);
    const char* pattern = luaL_checklstring(L, 2, &pattern_n);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    // MQ's options table: keepLinks (default false) matches and passes the line with its item links.
    bool keep_links = false;
    if (!lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TTABLE);
        lua_getfield(L, 4, "keepLinks");
        keep_links = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    // The limits (kMaxEvents ...), counted without the event this one replaces. Checked before add_event makes any
    // C++ object: the errors longjmp.
    std::size_t count = 0, bytes = 0;
    for (const auto& e : s.events)
        if (e.name.size() != name_n || e.name.compare(0, name_n, name_text, name_n) != 0) {
            ++count;
            bytes += e.pattern.size();
        }
    if (name_n > kMaxEventName) return luaL_error(L, "mq.event: a name is at most %d bytes", static_cast<int>(kMaxEventName));
    if (pattern_n > kMaxEventPattern)
        return luaL_error(L, "mq.event: a pattern is at most %d bytes", static_cast<int>(kMaxEventPattern));
    if (count >= kMaxEvents) return luaL_error(L, "mq.event: a script has at most %d events", static_cast<int>(kMaxEvents));
    if (bytes + pattern_n > kMaxEventPatterns)
        return luaL_error(L, "mq.event: a script's patterns hold at most %d bytes", static_cast<int>(kMaxEventPatterns));
    add_event(L, s, name_text, std::string_view(pattern, pattern_n), keep_links); // every text is a pattern
    return 0;
}

// The callback is argument 3 on L, the calling thread's stack.
void add_event(lua_State* L, Script& s, const char* name, std::string_view pattern, bool keep_links) {
    lua_pushvalue(L, 3);
    const int fn = luaL_ref(L, LUA_REGISTRYINDEX); // (before the Event: it may raise)
    Script::Event e{name, EventPattern(pattern), fn, keep_links};
    for (auto& old : s.events)
        if (old.name == name) {
            luaL_unref(L, LUA_REGISTRYINDEX, old.fn);
            old = std::move(e);
            return;
        }
    s.events.push_back(std::move(e));
    if (!s.chat_hooked) {
        s.chat_hooked = true;
        Script* sp = &s;
        s.host->env.hooks->on_chat(s.owner, [sp](plugin::ChatLine& line) {
            if (sp->status != Script::Status::Running && sp->status != Script::Status::Paused) return plugin::Flow::Continue;
            // The server may take 'events' away after mq.event (ScriptHost::policy_changed): no more lines then.
            if (!(sp->caps & plugin::kCapEvents)) return plugin::Flow::Continue;
            if (line.text.size() >= kMaxEventLine) return plugin::Flow::Continue;
            // MQ matches a line with its item links shown as their text, unless the event keeps them (keepLinks).
            std::optional<std::string> stripped;
            // A |...| in a pattern is the script's ${...}, read with the script's own 'read' (MQ's LuaVarProcess);
            // without it, the text as written. Its value is put in the pattern as text, never parsed again.
            auto origin = plugin::caller_tlo_proof();
            if (line.from_code && !origin) origin = plugin::WeakTloCaller{};
            EventPattern::Resolve resolve = [sp, &origin](std::string_view text) {
                if (!(sp->caps & plugin::kCapRead) || !sp->host) return std::string(text);
                const auto starter = origin ? origin->lock() : plugin::TloCaller{};
                const bool allow = !origin || (starter && sp->tlo_caller &&
                                               starter->identity() == sp->tlo_caller->identity());
                plugin::CallerScope scope(sp->caps & plugin::kCapRead, sp->name,
                                          allow ? plugin::WeakTloCaller(sp->tlo_caller) : plugin::WeakTloCaller{});
                return parse_macro_data(sp->host->env.context(), text);
            };
            // Each text folded once for all the script's patterns.
            std::optional<std::string> low_line, low_stripped;
            for (const auto& ev : sp->events) {
                const std::string* text = &line.text;
                const std::string* low = nullptr;
                if (!ev.keep_links) {
                    if (!stripped) stripped = game::strip_links(line.text);
                    if (!low_stripped) low_stripped = EventPattern::fold(*stripped);
                    text = &*stripped;
                    low = &*low_stripped;
                } else {
                    if (!low_line) low_line = EventPattern::fold(line.text);
                    low = &*low_line;
                }
                auto m = ev.pattern.match(*text, *low, resolve);
                if (!m) continue;
                std::vector<std::string> args{*text};
                for (auto& a : *m) args.push_back(std::move(a));
                if (sp->queued.size() < kMaxQueuedEvents)
                    sp->queued.push_back({ev.name, std::move(args), line.from_code ? line.origin_caps : plugin::kCapAll,
                                          origin});
            }
            return plugin::Flow::Continue;
        });
    }
}

int l_unevent(lua_State* L) {
    Script& s = Script::of(L);
    std::string name = luaL_checkstring(L, 1);
    for (auto it = s.events.begin(); it != s.events.end();)
        if (it->name == name) {
            luaL_unref(L, LUA_REGISTRYINDEX, it->fn);
            it = s.events.erase(it);
        } else {
            ++it;
        }
    std::erase_if(s.queued, [&](const auto& q) { return q.name == name; });
    return 0;
}

int l_flushevents(lua_State* L) {
    Script& s = Script::of(L);
    if (lua_isnoneornil(L, 1)) {
        s.queued.clear();
    } else {
        std::string name = luaL_checkstring(L, 1);
        std::erase_if(s.queued, [&](const auto& q) { return q.name == name; });
    }
    return 0;
}

void start_thread(Script& s, int fn_ref, const std::vector<std::string>& args, std::uint32_t limit = plugin::kCapAll,
                  bool tlo_allowed = true);

bool event_tlo_allowed(const Script& s, const std::optional<plugin::WeakTloCaller>& origin) {
    if (!origin || !s.tlo_caller) return true;
    const auto starter = origin->lock();
    if (starter && starter->identity() == s.tlo_caller->identity()) return true;
    const auto ctx = s.host->env.context();
    if (!ctx.conn) return true;
    const auto& rules = ctx.conn->server_rules();
    // A handler can yield across provisional rules and resume after the actual server grants arrive.
    return !rules || (!rules->provisional && !rules->tlos.addon_allow.contains(s.tlo_caller->identity()));
}

// fn, {args} = mq._next_event([name]): the oldest queued event (of that name); nil when there is none, false when it
// ran one as a thread (below).
// The event's text stays in the queue (the Script's) until it is on the stack: a push that fails longjmps (Lua is
// built as C), and no C++ object of this frame may be alive then.
// An event raised by another's line (a script's or plugin's print, Queued::origin) whose handler would get more than
// that code may do runs as a thread of its own with no more (start_thread's limit), as a bind it ran would; doevents
// goes on with the next. A thread so that the limit holds for each resume, if the handler waits. Each such event
// returns to doevents' Lua loop rather than looping here: a handler whose own line raises its event again would keep
// this C loop going forever, where no instruction hook could stop it.
// Without 'events' (taken away after mq.event, or not held by the thread running now) there is nothing to run: nil,
// not an error, since scripts call mq.doevents in their loop whether they have events or not. A script that lost the
// permission loses the lines it had queued too.
int l_next_event(lua_State* L) {
    Script& s = Script::of(L);
    const char* name = lua_isnoneornil(L, 1) ? nullptr : luaL_checkstring(L, 1);
    if (!(s.caps & plugin::kCapEvents)) s.queued.clear();
    if (!(s.caps & s.narrowed & plugin::kCapEvents)) {
        lua_pushnil(L);
        return 1;
    }
    while (true) {
        auto it = s.queued.begin();
        while (it != s.queued.end() && name && it->name != name) ++it;
        if (it == s.queued.end()) break;
        int fn = LUA_NOREF;
        for (const auto& e : s.events)
            if (e.name == it->name) fn = e.fn;
        if (fn == LUA_NOREF) { // unevented meanwhile
            s.queued.erase(it);
            continue;
        }
        const bool tlo_allowed = event_tlo_allowed(s, it->tlo_origin);
        if ((s.caps & s.narrowed & ~it->origin) != 0 || !tlo_allowed) {
            const std::uint32_t origin = it->origin;
            std::vector<std::string> args = std::move(it->args);
            s.queued.erase(it);
            start_thread(s, fn, args, origin, tlo_allowed);
            lua_pushboolean(L, 0);
            return 1;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, fn);
        lua_createtable(L, static_cast<int>(it->args.size()), 0);
        for (std::size_t i = 0; i < it->args.size(); ++i) {
            lua_pushlstring(L, it->args[i].data(), it->args[i].size());
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        s.queued.erase(it);
        return 2;
    }
    lua_pushnil(L);
    return 1;
}

// Runs fn(args...) as a new coroutine of the script (binds): it may mq.delay; pulse() resumes it.
// A bind that runs itself (mq.bind('/x', function() mq.cmd('/x') end)) nests here on the native stack: each resume
// starts Lua's own C-stack count again (no `from`), so past kMaxNesting the script is stopped instead.
// It runs with the script's capabilities, its starter's (the code that ran the command: another script's, a plugin's,
// the player's all) and `limit` together, at its first resume and every later one (Script::Thread::caps): a bind a
// lesser script ran does not get the script's own once it waited.
void start_thread(Script& s, int fn_ref, const std::vector<std::string>& args, std::uint32_t limit, bool tlo_allowed) {
    if (!s.L || s.stop_requested) return;
    if (s.busy >= kMaxNesting) {
        s.fail(std::format("its commands ran each other more than {} deep", kMaxNesting));
        return;
    }
    HostWork host(s);
    ++s.busy;
    lua_State* t = lua_newthread(s.L);
    int ref = luaL_ref(s.L, LUA_REGISTRYINDEX);
    if (args.size() > 1000 || !lua_checkstack(t, static_cast<int>(args.size()) + 1)) {
        luaL_unref(s.L, LUA_REGISTRYINDEX, ref);
        --s.busy;
        // That one line is refused; the script runs on (a long line it passes to its own bind is no reason to end it).
        s.host->say(std::format("[Lua] {}: a command with too many words for its bind ({}) is refused", s.name, args.size()));
        log::warn("lua: {}: a bind's command of {} words refused", s.name, args.size());
        return;
    }
    watch(t);
    lua_rawgeti(t, LUA_REGISTRYINDEX, fn_ref);
    for (const auto& a : args) lua_pushlstring(t, a.data(), a.size());
    const std::uint32_t caps = s.caps & s.narrowed & plugin::caller_caps() & limit;
    tlo_allowed = tlo_allowed && plugin::may_inherit_tlo_proof(s.tlo_caller);
    plugin::CallerScope scope(caps, s.name,
                              tlo_allowed ? plugin::WeakTloCaller(s.tlo_caller) : plugin::WeakTloCaller{});
    const std::uint32_t narrowed = std::exchange(s.narrowed, caps);
    // The thread gets a slice of its own; the code it ran from (doevents, an mq.cmd) goes on with what is left of
    // its own after: handlers started one after another cannot keep that code from ever pausing or being stopped.
    const Clock::time_point outer_slice = s.slice_start;
    s.new_slice();
    int nres = 0;
    int st = 0;
    {
        ScriptRun run(s, t);
        st = lua_resume(t, nullptr, static_cast<int>(args.size()), &nres);
    }
    s.slice_start = outer_slice;
    s.narrowed = narrowed;
    if (st == LUA_YIELD) {
        lua_pop(t, nres);
        s.threads.push_back({t, ref, caps, tlo_allowed});
    } else {
        if (st != LUA_OK && !(lua_touserdata(t, -1) == &kExitKey)) s.fail(lua_tostring(t, -1) ? lua_tostring(t, -1) : "error");
        else if (st != LUA_OK) s.stop_requested = true; // mq.exit
        luaL_unref(s.L, LUA_REGISTRYINDEX, ref);
    }
    --s.busy;
}

// mq.bind's work once its arguments are checked and the function is referenced (`ref`); false with the error's
// text in s.scratch. Nothing here calls Lua that could raise: Lua is built as C, and an error longjmps past this
// frame's strings (luaL_unref sets keys the registry has already, which allocates nothing).
bool bind_command(Script& s, const char* given, int ref) {
    std::string cmd = given;
    if (cmd.empty() || cmd.front() != '/') cmd.insert(cmd.begin(), '/');
    cmd = lower(cmd);
    plugin::Hooks& hooks = *s.host->env.hooks;
    if (hooks.reserved_command(cmd)) {
        s.scratch = "mq.bind: " + cmd + " is the client's own command";
        return false;
    }
    // Another script's or plugin's: taking it over would hand this script what the player types for the other (and
    // run it with this one's capabilities).
    if (auto o = hooks.command_owner(cmd); o && *o != s.owner && hooks.is_guest(*o)) {
        s.scratch = "mq.bind: " + cmd + " is " + hooks.owner_name(*o) + "'s command";
        return false;
    }
    if (s.binds.size() >= kMaxBinds && !s.binds.contains(cmd)) {
        s.scratch = std::format("mq.bind: a script has at most {} binds", kMaxBinds);
        return false;
    }
    if (auto it = s.binds.find(cmd); it != s.binds.end()) {
        luaL_unref(s.L, LUA_REGISTRYINDEX, it->second);
        s.binds.erase(it);
        hooks.remove_command(s.owner, cmd);
    }
    s.binds[cmd] = ref;
    Script* sp = &s;
    hooks.add_command(s.owner, cmd, [sp, ref](std::string_view args) {
        std::vector<std::string> words;
        std::string_view rest = args;
        while (!rest.empty()) {
            std::string w = game::next_cmd_arg(rest);
            if (w.empty() && rest.empty()) break;
            words.push_back(std::move(w));
        }
        start_thread(*sp, ref, words);
    });
    return true;
}

// mq.bind(cmd, fn): a command of the script's own (the 'commands' capability). The client's own commands (the game's,
// abbreviations included, and /lua, /plugin, MQ's) cannot be taken (plugin::Hooks::reserved_command), nor a name
// another script or plugin has now.
int l_bind(lua_State* L) {
    need(L, plugin::kCapCommands, "mq.bind");
    Script& s = Script::of(L);
    const char* given = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX); // (may raise: before any C++ object)
    if (bind_command(s, given, ref)) return 0;
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    luaL_where(L, 1);
    lua_pushlstring(L, s.scratch.data(), s.scratch.size());
    lua_concat(L, 2);
    return lua_error(L);
}

int l_unbind(lua_State* L) {
    Script& s = Script::of(L);
    std::string cmd = lower(luaL_checkstring(L, 1));
    if (cmd.empty() || cmd.front() != '/') cmd.insert(cmd.begin(), '/');
    if (auto it = s.binds.find(cmd); it != s.binds.end()) {
        luaL_unref(L, LUA_REGISTRYINDEX, it->second);
        s.binds.erase(it);
        s.host->env.hooks->remove_command(s.owner, cmd);
    }
    return 0;
}

int l_imgui_init(lua_State* L) {
    need(L, plugin::kCapUi, "mq.imgui");
    Script& s = Script::of(L);
    const char* given = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX); // it may raise: no C++ object in this frame (Lua is built as C)
    for (auto& w : s.windows)
        if (w.first == given) {
            luaL_unref(L, LUA_REGISTRYINDEX, w.second);
            w.second = ref;
            return 0;
        }
    if (s.windows.size() >= kMaxWindows) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "mq.imgui.init: a script has at most %d windows", static_cast<int>(kMaxWindows));
    }
    s.windows.emplace_back(given, ref);
    return 0;
}

int l_imgui_destroy(lua_State* L) {
    Script& s = Script::of(L);
    std::string name = luaL_checkstring(L, 1);
    for (auto it = s.windows.begin(); it != s.windows.end(); ++it)
        if (it->first == name) {
            luaL_unref(L, LUA_REGISTRYINDEX, it->second);
            s.windows.erase(it);
            break;
        }
    return 0;
}

int l_imgui_exists(lua_State* L) {
    Script& s = Script::of(L);
    std::string name = luaL_checkstring(L, 1);
    bool found = std::any_of(s.windows.begin(), s.windows.end(), [&](const auto& w) { return w.first == name; });
    lua_pushboolean(L, found);
    return 1;
}

int l_get_all_spawns(lua_State* L) {
    need(L, plugin::kCapRead, "mq.getAllSpawns");
    const Context& ctx = ctx_of(L);
    const Type* spawn = reg_of(L).find_type("spawn");
    lua_newtable(L);
    if (!ctx.conn || !spawn) return 1;
    // The ids' vector lives across the pushes: they are not refused at the script's memory limit (HostWork), so
    // none raises past it (Lua is built as C) but for a system out of memory.
    HostWork unlimited(Script::of(L));
    std::vector<std::uint32_t> ids;
    for (const auto& [id, s] : ctx.conn->world().spawns) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    lua_Integer n = 0;
    for (auto id : ids) {
        void* mem = ref_slot(L);
        ref_fill(L, mem, TloRef{TloRef::Kind::Value, {}, Value::object(spawn, id), {}});
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// Whether `file` is in the config directory under the script folder `dir` (mq.configDir is <scripts>/config):
// mq.pickle's data, which a script writes as it likes, is never run as a script or module. Both paths are taken
// canonical (links followed) and compared as Windows and a case-insensitive macOS volume would see them
// (core/windows_names.h: any case, a part's trailing dots and spaces dropped), so "Config", "config." or a link
// into it are the config directory too.
bool in_config_dir(const fs::path& dir, const fs::path& file) {
    std::error_code ec;
    fs::path d = fs::weakly_canonical(dir, ec);
    if (ec) d = dir.lexically_normal();
    fs::path f = fs::weakly_canonical(file, ec);
    if (ec) f = file.lexically_normal();
    std::string dk = windows_file_key(d.generic_string()), fk = windows_file_key(f.generic_string());
    while (!dk.empty() && dk.back() == '/') dk.pop_back();
    if (fk.size() <= dk.size() + 1 || fk.compare(0, dk.size(), dk) != 0 || fk[dk.size()] != '/') return false;
    const std::string_view rest = std::string_view(fk).substr(dk.size() + 1);
    return rest.starts_with("config/");
}

} // namespace

// A script's name as one folder name (its mq.configDir under config/, its hailstone.server_dir and character_dir
// under plugin-config/): the name itself where every system keeps it as written, else with the bytes that would not
// stay written as %XX (two upper-case hex digits): '%', control characters and bytes past ASCII, the characters
// Windows refuses or reads as a path (/ \ : * ? " < > |), a last '.' or ' ' (Windows drops them) and the first of a
// device name (CON, NUL, COM1 ...). Two names never share a folder ("a b" and "a_b", which config/ once shared as
// a_b); scripts' names are lower case, so a case-insensitive volume does not join two either.
std::string script_folder_name(std::string_view name) {
    if (name.empty()) return "%";
    static constexpr char kHex[] = "0123456789ABCDEF";
    const bool device = is_windows_device_name(name);
    std::string out;
    out.reserve(name.size());
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        const bool last = i + 1 == name.size();
        if (c < 0x20 || c >= 0x7f || std::string_view("%/\\:*?\"<>|").find(static_cast<char>(c)) != std::string_view::npos ||
            (last && (c == '.' || c == ' ')) || (i == 0 && device)) {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xf];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

namespace {

// config/'s folder for a script before script_folder_name: every byte but letters, digits, '-', '_' and '.' as '_'
// (so "a b" and "a_b" shared one). What open_state moves to the new name (migrate_config_dir).
std::string old_config_folder(std::string_view name) {
    std::string folder(name);
    for (char& c : folder)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') c = '_';
    if (folder.empty() || folder.find_first_not_of('.') == std::string::npos) folder = "_";
    return folder;
}

} // namespace

// The script's config directory (mq.configDir): the shared one for a script that may do everything; a folder of its
// own under it (config/<name>/, script_folder_name) for any other, so an add-on cannot rewrite or read the settings
// another script (a server's, the player's) keeps there. mq.configDir .. '/MyScript.lua', MQ's usual path, works
// either way.
fs::path script_config_dir(const Script& s) {
    const fs::path& base = s.host->env.config_dir;
    if (base.empty() || s.caps == plugin::kCapAll) return base;
    return base / script_folder_name(s.name);
}

// A script whose folder in config/ was another name before script_folder_name (old_config_folder: a name with a
// space or another such byte) gets that folder moved to its own name, once, when nothing else could have used it: its
// own folder is not there yet, and no other script in the script folders had the old name. Otherwise it starts with
// an empty folder, and the old one stays where it was.
void ScriptHost::Impl::migrate_config_dir(const Script& s) const {
    const fs::path to = script_config_dir(s);
    const fs::path& base = env.config_dir;
    if (to == base || to.empty()) return;
    const std::string old_name = old_config_folder(s.name);
    if (old_name == script_folder_name(s.name)) return;
    std::error_code ec;
    const fs::path from = base / old_name;
    if (fs::exists(to, ec) || !fs::is_directory(from, ec)) return;
    for (const auto& d : dirs())
        for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path& p = it->path();
            std::error_code fec;
            std::string other;
            if (it->is_directory(fec) && fs::is_regular_file(p / "init.lua", fec)) other = lower(p.filename().string());
            else if (it->is_regular_file(fec) && lower(p.extension().string()) == ".lua") other = lower(p.stem().string());
            else continue;
            if (other != s.name && (old_config_folder(other) == old_name || script_folder_name(other) == old_name)) {
                log::info("lua: {}: {} may be {}'s too: left where it is", s.name, from.string(), other);
                return;
            }
        }
    fs::rename(from, to, ec);
    if (ec) log::warn("lua: {}: cannot move {} to {}: {}", s.name, from.string(), to.string(), ec.message());
    else log::info("lua: {}: its settings moved from {} to {}", s.name, from.string(), to.string());
}

namespace {

// A path inside the script's config directory (relative paths are relative to it); nullopt outside it.
std::optional<fs::path> config_path(lua_State* L, const char* given) {
    const fs::path base = script_config_dir(Script::of(L));
    if (base.empty()) return std::nullopt;
    std::error_code ec;
    fs::path root = fs::weakly_canonical(base, ec);
    fs::path p = fs::path(given);
    if (p.is_relative()) p = root / p;
    p = fs::weakly_canonical(p, ec);
    auto rel = p.lexically_relative(root);
    if (ec || rel.empty() || *rel.begin() == "..") return std::nullopt;
    return p;
}

// What the config directory holds: its files and their bytes. Stops counting a little past kMaxConfigFiles (past
// it every write is refused anyway).
ScriptHost::Impl::ConfigUsage count_config(const fs::path& dir) {
    ScriptHost::Impl::ConfigUsage u;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && u.files <= kMaxConfigFiles; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        ++u.files;
        const auto size = it->file_size(fec);
        if (!fec) u.bytes += size;
    }
    return u;
}

bool path_within(const fs::path& dir, const fs::path& file) {
    const fs::path rel = file.lexically_relative(dir);
    return !rel.empty() && *rel.begin() != ".." && *rel.begin() != ".";
}

// mq._write_config(path, text): mq.pickle's file write, within kMaxConfigFileBytes for the file and kMaxConfigFiles
// / kMaxConfigBytes for the script's config directory.
int l_write_config(lua_State* L) {
    const char* given = luaL_checkstring(L, 1);
    std::size_t n = 0;
    const char* text = luaL_checklstring(L, 2, &n);
    const Script& s = Script::of(L);
    if (s.dry) return luaL_error(L, "mq.pickle does nothing in a dry run");
    if (s.closing) return luaL_error(L, "mq.pickle: the script is stopping");
    int problem = 0; // 1 outside, 2 cannot write, 3 too big, 4 the directory is full
    plugin::cxx_guard(L, [&] {
        auto p = config_path(L, given);
        if (!p) {
            problem = 1;
            return;
        }
        if (n > kMaxConfigFileBytes) {
            problem = 3;
            return;
        }
        std::error_code ec;
        const fs::path dir = fs::weakly_canonical(script_config_dir(s), ec);
        auto& counts = s.host->config_usage;
        auto it = counts.find(dir);
        if (it == counts.end()) it = counts.emplace(dir, count_config(dir)).first;
        // The file a write replaces is left out, as before it is written: one stat, never another walk.
        std::error_code fec;
        const bool replaces = fs::is_regular_file(*p, fec);
        std::uintmax_t old_size = 0;
        if (replaces) {
            const auto size = fs::file_size(*p, fec);
            if (!fec) old_size = size;
        }
        const auto& used = it->second;
        const std::size_t other_files = used.files - std::min<std::size_t>(used.files, replaces ? 1 : 0);
        const std::uintmax_t other_bytes = used.bytes - std::min(used.bytes, old_size);
        if (other_files + 1 > kMaxConfigFiles || other_bytes + n > kMaxConfigBytes) {
            problem = 4;
            return;
        }
        fs::create_directories(p->parent_path(), ec);
        std::ofstream f(*p, std::ios::binary | std::ios::trunc);
        if (f) f.write(text, static_cast<std::streamsize>(n));
        if (f) f.close();
        // Every counted directory the file is in (the shared one holds each script's own folder) gets the write; one
        // whose count is no longer known (a write that failed part way) is counted again at its next write.
        for (auto c = counts.begin(); c != counts.end();) {
            if (!path_within(c->first, *p)) {
                ++c;
            } else if (!f) {
                c = counts.erase(c);
            } else {
                if (!replaces) ++c->second.files;
                c->second.bytes = c->second.bytes - std::min(c->second.bytes, old_size) + n;
                ++c;
            }
        }
        if (!f) problem = 2;
    });
    if (problem == 1) return luaL_error(L, "mq.pickle: '%s' is outside the config directory", given);
    if (problem == 2) return luaL_error(L, "mq.pickle: cannot write '%s'", given);
    if (problem == 3)
        return luaL_error(L, "mq.pickle: '%s' would be over %d MiB", given, static_cast<int>(kMaxConfigFileBytes >> 20));
    if (problem == 4)
        return luaL_error(L, "mq.pickle: the config directory is full (%d files or %d MiB)",
                          static_cast<int>(kMaxConfigFiles), static_cast<int>(kMaxConfigBytes >> 20));
    return 0;
}

// What loadfile read: the file's text and chunk name, or why not.
struct ConfigFile {
    bool found = false;
    std::string text, chunk, msg;
};

// The prelude's load_config(path): a data file under the config directory (mq.pickle's), as text, with an empty
// environment; the prelude's loadfile runs it and hands out only what it returns, if that is data (kPrelude). The
// file's text and the messages live in a box the state owns (plugin::lua_keep) across the load and the pushes.
int l_load_config(lua_State* L) {
    const char* given = luaL_checkstring(L, 1);
    ConfigFile& f = plugin::lua_keep<ConfigFile>(L);
    plugin::cxx_guard(L, [&] {
        auto p = config_path(L, given);
        if (!p) {
            f.msg = std::string("'") + given + "' is outside the config directory";
            return;
        }
        auto text = read_file(*p);
        if (!text) {
            f.msg = "cannot open " + p->string();
            return;
        }
        f.found = true;
        f.text = std::move(*text);
        f.chunk = "@" + p->string();
    });
    if (!f.found) {
        lua_pushnil(L);
        lua_pushlstring(L, f.msg.data(), f.msg.size());
        lua_remove(L, -3); // the box
        return 2;
    }
    const bool loaded = luaL_loadbufferx(L, f.text.data(), f.text.size(), f.chunk.c_str(), "t") == LUA_OK;
    f = ConfigFile{};
    lua_remove(L, -2);
    if (!loaded) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    lua_newtable(L);
    lua_setupvalue(L, -2, 1); // _ENV: nothing to call
    return 1;
}

// What search_module found on disk: the module's text and chunk name, or why there is none.
struct ModuleFile {
    bool found = false;
    std::string text, chunk, msg;
};

// A typed path alias (needed/init.lua, ./needed/init) still belongs to the same top-level script folder.
std::string script_identity(const ScriptHost::Impl& host, const fs::path& entry, std::string_view fallback) {
    const fs::path normalized = entry.lexically_normal();
    for (const auto& dir : host.dirs()) {
        const fs::path relative = normalized.lexically_relative(dir.lexically_normal());
        if (relative.empty() || *relative.begin() == "..") continue;
        return lower(relative.parent_path().empty() ? relative.stem().string() : relative.begin()->string());
    }
    return lower(std::string(fallback));
}

// The module file `given` names, next to the script or in the script directories, allowed by the server's rules like
// the script. Calls no Lua.
void search_module(Script& s, std::string_view given, ModuleFile& out) {
    const std::string name(given);
    std::string rel = name;
    std::replace(rel.begin(), rel.end(), '.', '/');
    // Inside the folders only: no "..", nothing absolute or rooted (operator/ would take it in place of the folder).
    if (const fs::path rel_path(rel); rel.find("..") == std::string::npos && !rel.empty() && rel.front() != '/' &&
                                   rel.front() != '\\' && !rel_path.has_root_name() && !rel_path.is_absolute()) {
        std::vector<fs::path> roots{s.root};
        for (const auto& d : s.host->dirs()) roots.push_back(d);
        for (const auto& r : roots) {
            for (const fs::path& cand : {r / (rel + ".lua"), r / rel / "init.lua"}) {
                const auto& dirs = s.host->dirs();
                if (std::any_of(dirs.begin(), dirs.end(), [&](const fs::path& d) { return in_config_dir(d, cand); }))
                    continue;
                auto text = read_file(cand);
                if (!text) continue;
                for (const auto& addon : s.host->managed) {
                    std::error_code entry_error;
                    if (fs::equivalent(cand, addon.entry, entry_error)) {
                        out.msg = "managed add-on '" + addon.name + "'s entry cannot be loaded as another script's module";
                        return;
                    }
                }
                Sha256Digest h = sha256(*text);
                const std::string owner = script_identity(*s.host, cand, s.name);
                const std::uint32_t module_caps =
                    s.host->env.policy ? s.host->env.policy->caps(plugin::FileKind::Script, h, owner) : 0;
                if (module_caps == 0) {
                    out.msg = std::format("module '{}' ({}) may not run: {}", name, cand.string(),
                                          s.host->env.policy ? s.host->env.policy->refusal(plugin::FileKind::Script)
                                                             : std::string("no rules"));
                    return;
                }
                // The module runs in this script's state, with this script's capabilities: its own file must be
                // allowed every one of them. Otherwise an add-on with fewer (an add-on named "mq" shipping
                // mq/Icons.lua) would have its code run with the capabilities of any script that requires it.
                if ((module_caps & s.caps) != s.caps) {
                    out.msg = std::format(
                        "module '{}' ({}) may not run in {}: the module may do only '{}', the script '{}'", name,
                        cand.string(), s.name, plugin::caps_text(module_caps), plugin::caps_text(s.caps));
                    return;
                }
                if (std::none_of(s.modules.begin(), s.modules.end(), [&](const auto& m) { return m.second == h && m.first == owner; }))
                    s.modules.emplace_back(owner, h);
                if (s.tlo_caller) {
                    // The module can leave functions behind in this state. A different or unverified package
                    // revokes its TLO exception before the first instruction of the module runs.
                    if (s.tlo_caller->identity().empty() ||
                        s.host->env.policy->verified_tlo_identity(plugin::FileKind::Script, h, cand, false) !=
                        s.tlo_caller->identity()) {
                        s.tlo_caller->revoke();
                        s.tlo_caller.reset();
                        s.tlo_modules.clear();
                    } else {
                        const auto module = std::pair(cand, h);
                        if (std::find(s.tlo_modules.begin(), s.tlo_modules.end(), module) == s.tlo_modules.end())
                            s.tlo_modules.push_back(module);
                    }
                }
                out.found = true;
                out.text = std::move(*text);
                out.chunk = "@" + cand.string();
                return;
            }
        }
    }
    out.msg = "module '" + name + "' not found";
}

// require(name): mq, ImGui, or a module file next to the script or in the script directories; every file must be
// allowed by the server's rules like the script. find_module leaves the module's chunk on the stack (1), the cached or
// built-in value (2), or an error message (0). The lookups come first, while no C++ object exists; the file's text,
// its name and the message live in a box the state owns (plugin::lua_keep) across the load and the push.
int find_module(lua_State* L, const char* given) {
    lua_getfield(L, LUA_REGISTRYINDEX, "hailstone.loaded");
    lua_getfield(L, -1, given);
    lua_remove(L, -2);
    if (!lua_isnil(L, -1)) return 2;
    lua_pop(L, 1);
    if (std::strcmp(given, "mq") == 0) {
        lua_getfield(L, LUA_REGISTRYINDEX, "hailstone.mq");
        return 2;
    }
    for (const char* global : {"ImGui", "hailstone"})
        if (std::strcmp(given, global) == 0) {
            lua_getglobal(L, global);
            return 2;
        }
    Script& s = Script::of(L);
    ModuleFile& m = plugin::lua_keep<ModuleFile>(L);
    plugin::cxx_guard(L, [&] { search_module(s, given, m); });
    int found = 0;
    if (m.found) found = luaL_loadbufferx(L, m.text.data(), m.text.size(), m.chunk.c_str(), "t") == LUA_OK ? 1 : 0;
    else lua_pushlstring(L, m.msg.data(), m.msg.size());
    m = ModuleFile{}; // (its text now, not when the collector comes)
    lua_remove(L, -2);
    return found;
}

int l_require(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_settop(L, 1);
    int found = find_module(L, name);
    if (found == 0) return lua_error(L);
    if (found == 2) return 1;
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1); // the module's chunk, given its name
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "hailstone.loaded");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
    return 1;
}

// The Lua half of the mq module and MQ's ImGui table over the client's `ui` (ui/lua/lua_imgui.cpp).
constexpr const char* kPrelude = R"lua(
local mq, load_config = ...

-- loadfile(path) / dofile(path): mq.pickle's data back, from under mq.configDir. The file is run with an empty
-- environment, and loadfile hands out only what it returned, as data: a value that is or holds a function (a
-- closure the file made, a string method it took) refuses the file. A script writes these files as it likes
-- (mq.pickle), so a function from one would be code no server's rules looked at (docs/plugins.md).
do
    local type, next, pcall, error, tostring = type, next, pcall, error, tostring
    local pack, unpack = table.pack, table.unpack
    local function only_data(v, seen)
        local t = type(v)
        if t == 'function' or t == 'userdata' or t == 'thread' then return false end
        if t ~= 'table' or seen[v] then return true end
        seen[v] = true
        for k, x in next, v do
            if not only_data(k, seen) or not only_data(x, seen) then return false end
        end
        return true
    end
    function loadfile(path)
        local chunk, err = load_config(path)
        if not chunk then return nil, err end
        local r = pack(pcall(chunk))
        if not r[1] then return nil, r[2] end
        local seen = {}
        for i = 2, r.n do
            if not only_data(r[i], seen) then return nil, tostring(path) .. ': not data (it holds a function)' end
        end
        return function() return unpack(r, 2, r.n) end
    end
    function dofile(path)
        local f, err = loadfile(path)
        if not f then error(err, 0) end
        return f()
    end
end
local yield = coroutine.yield

function mq.delay(t, cond)
    local ms = mq._delay_ms(t)
    if mq._dry(ms) then return end
    if not coroutine.isyieldable() then
        error('mq.delay: this server lets a social run only on the key press, without waiting', 2)
    end
    local stop = mq.gettime() + ms
    repeat
        yield()
    until mq.gettime() >= stop or (cond ~= nil and cond())
end

function mq.doevents(name)
    while true do
        local fn, args = mq._next_event(name)
        if fn == nil then return end
        if fn then fn(table.unpack(args)) end
    end
end

local function join(...)
    local n = select('#', ...)
    local t = {}
    for i = 1, n do t[i] = tostring((select(i, ...))) end
    return table.concat(t, ' ')
end

mq.cmd = setmetatable({}, {
    __call = function(_, ...) mq._cmd(join(...)) end,
    __index = function(_, name)
        return function(...) mq._cmd('/' .. name .. (select('#', ...) > 0 and (' ' .. join(...)) or '')) end
    end,
})
function mq.cmdf(fmt, ...) mq._cmd(string.format(fmt, ...)) end

function mq.getFilteredSpawns(pred)
    local out = {}
    for _, s in ipairs(mq.getAllSpawns()) do
        if pred == nil or pred(s) then out[#out + 1] = s end
    end
    return out
end

local function pickle_value(v, indent)
    local t = type(v)
    if t == 'string' then return string.format('%q', v) end
    if t == 'number' or t == 'boolean' or t == 'nil' then return tostring(v) end
    if t ~= 'table' then return 'nil' end
    local pad = string.rep('    ', indent + 1)
    local keys = {}
    for k in pairs(v) do keys[#keys + 1] = k end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    local parts = {}
    for _, k in ipairs(keys) do
        local key = type(k) == 'string' and string.format('[%q]', k) or ('[' .. tostring(k) .. ']')
        parts[#parts + 1] = pad .. key .. ' = ' .. pickle_value(v[k], indent + 1)
    end
    return '{\n' .. table.concat(parts, ',\n') .. '\n' .. string.rep('    ', indent) .. '}'
end
function mq.pickle(path, tbl) mq._write_config(path, 'return ' .. pickle_value(tbl, 0) .. '\n') end

function printf(fmt, ...) print(string.format(fmt, ...)) end

-- MQ's ImGui table: the client's ui functions with MQ's argument and return orders.
ImGui = setmetatable({}, {__index = ui})
function ImGui.Begin(name, open, flags)
    if open == nil then
        local visible = ui.Begin(name, nil, flags or 0)
        return visible
    end
    local visible, still_open = ui.Begin(name, open, flags or 0)
    return still_open, visible
end
local function fmt_text(fmt, ...)
    if select('#', ...) > 0 then return string.format(fmt, ...) end
    return tostring(fmt)
end
function ImGui.Text(fmt, ...) ui.Text(fmt_text(fmt, ...)) end
function ImGui.TextWrapped(fmt, ...) ui.TextWrapped(fmt_text(fmt, ...)) end
function ImGui.TextDisabled(fmt, ...) ui.TextDisabled(fmt_text(fmt, ...)) end
function ImGui.BulletText(fmt, ...) ui.BulletText(fmt_text(fmt, ...)) end
function ImGui.TextColored(a, b, c, d, e, ...)
    if type(a) == 'table' then
        ui.TextColored(a.x or a[1], a.y or a[2], a.z or a[3], a.w or a[4], fmt_text(b, c, d, e, ...))
    else
        ui.TextColored(a, b, c, d, fmt_text(e, ...))
    end
end
function ImGui.Button(label, w, h)
    if type(w) == 'table' then return ui.Button(label, w.x or w[1], w.y or w[2]) end
    return ui.Button(label, w or 0, h or 0)
end
function ImGui.SetNextWindowSize(w, h, cond)
    if type(w) == 'table' then return ui.SetNextWindowSize(w.x or w[1], w.y or w[2], h) end
    return ui.SetNextWindowSize(w, h, cond)
end
function ImGui.SetNextWindowSizeConstraints(a, b, c, d)
    if type(a) == 'table' then
        return ui.SetNextWindowSizeConstraints(a.x or a[1], a.y or a[2], b.x or b[1], b.y or b[2])
    end
    return ui.SetNextWindowSizeConstraints(a, b, c, d)
end
function ImGui.SetNextWindowPos(x, y, cond)
    if type(x) == 'table' then return ui.SetNextWindowPos(x.x or x[1], x.y or x[2], y) end
    return ui.SetNextWindowPos(x, y, cond)
end
function ImVec2(x, y) return {x = x or 0, y = y or 0} end
function ImVec4(x, y, z, w) return {x = x or 0, y = y or 0, z = z or 0, w = w or 0} end
ImGuiWindowFlags = ui.WindowFlags
ImGuiCond = ui.Cond
ImGuiCol = ui.Col
ImGuiStyleVar = ui.StyleVar
ImGuiTableFlags = ui.TableFlags
ImGuiTableColumnFlags = ui.TableColumnFlags
ImGuiInputTextFlags = ui.InputTextFlags
ImGuiComboFlags = ui.ComboFlags
ImGuiSelectableFlags = ui.SelectableFlags
ImGuiMouseButton = ui.Mouse
ImGuiHoveredFlags = ui.HoveredFlags
)lua";

} // namespace

// ---------------------------------------------------------------- the host

std::optional<std::string> script_file_hash(const fs::path& file) {
    auto text = read_file(file);
    if (!text) return std::nullopt;
    return to_hex(sha256(*text));
}

std::optional<fs::path> ScriptHost::Impl::resolve(const std::string& name) const {
    if (name.empty() || name.find("..") != std::string::npos) return std::nullopt;
    // A name is a path inside the script folders: never absolute or rooted ("/x", "C:x", "\\x"), which
    // operator/ would take in place of the folder.
    const fs::path given(name);
    if (given.is_absolute() || given.has_root_name() || given.has_root_directory() || name.front() == '/' ||
        name.front() == '\\')
        return std::nullopt;
    std::string base = name;
    if (base.size() > 4 && iequals(std::string_view(base).substr(base.size() - 4), ".lua")) base.resize(base.size() - 4);
    for (const auto& d : dirs()) {
        std::error_code ec;
        const fs::path root = fs::weakly_canonical(d, ec);
        if (ec) continue;
        for (const fs::path& relative : {fs::path(base + ".lua"), fs::path(base) / "init.lua"}) {
            const auto spelled = script_spelled_path(d, relative.lexically_normal());
            if (!spelled) continue;
            const fs::path& cand = *spelled;
            if (in_config_dir(d, cand) || !fs::is_regular_file(cand, ec)) continue;
            // It must stay in the folder once links are followed.
            const fs::path rel = fs::weakly_canonical(cand, ec).lexically_relative(root);
            if (ec || rel.empty() || *rel.begin() == "..") continue;
            return cand;
        }
    }
    return std::nullopt;
}

void Script::close() {
    if (closing) return; // closed already (reap() during a walk), or closing now: once only
    closing = true;
    stop_requested = true;
    if (tlo_caller) tlo_caller->revoke();
    tlo_caller.reset();
    release_plugin(*this);
    if (host->env.hooks && owner) host->env.hooks->remove(owner);
    if (L) lua_close(L);
    L = co = nullptr; // final cleanup must not revisit the state lua_close just freed
    // What the state's last code (a to-be-closed variable, a C finalizer) still gave the client, though need_caps
    // refuses it while closing: nothing may keep this Script once it is freed.
    release_plugin(*this);
    if (host->env.hooks && owner) host->env.hooks->remove(owner);
    owner = 0;
}

ScriptHost::ScriptHost(ScriptEnv env) : impl_(std::make_unique<Impl>()) {
    impl_->env = std::move(env);
    impl_->self = this;
    Impl* m = impl_.get();
    m->owner = m->env.hooks->new_owner("lua");
    m->env.hooks->add_command(m->owner, "/lua", [m](std::string_view args) { m->lua_command(args); },
                              plugin::kCapCommands);
}

ScriptHost::~ScriptHost() {
    for (auto& s : impl_->scripts) s->close();
    for (auto& [social, s] : impl_->buttons) s->close();
    if (impl_->env.hooks) impl_->env.hooks->remove(impl_->owner);
}

bool ScriptHost::Impl::open_state(Script& sc, std::string& why) {
    config_usage.clear();
    sc.L = lua_newstate(limited_alloc, &sc, luaL_makeseed(nullptr));
    if (!sc.L) {
        why = "out of memory";
        return false;
    }
    lua_State* L = sc.L;
    *static_cast<Script**>(lua_getextraspace(L)) = &sc;
    // Only a note: the host reads script tables raw and lifts the memory limit outside ScriptRun, so nothing should
    // raise outside a protected call; when something does, Lua still aborts once this returns.
    lua_atpanic(L, [](lua_State* S) -> int {
        log::error("lua {}: error outside any protected call: {}", Script::of(S).name,
                   lua_tostring(S, -1) ? lua_tostring(S, -1) : "?");
        return 0;
    });
    // The main state's own calls (its windows, hooks, a social's press) are held to runaway_ms too; its threads
    // (lua_newthread copies the hook) also pause after kSliceMs.
    watch(L);
    lua_pushcfunction(L, match_check);
    lua_setfield(L, LUA_REGISTRYINDEX, ui::lua::kMatchCheckKey);
    sc.new_slice(); // the prelude below
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 1);
    lua_settop(L, 0);
    // The calls that catch errors raise the runaway limit's again (Script::runaway).
    lua_getglobal(L, "coroutine");
    for (const char* name : {"resume", "close"}) {
        lua_getfield(L, -1, name);
        lua_pushcclosure(L, runaway_safe_call, 1);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
    for (const char* name : {"pcall", "xpcall"}) {
        lua_getglobal(L, name);
        lua_pushcclosure(L, runaway_safe_call, 1);
        lua_setglobal(L, name);
    }
    // No code from strings or files beyond allowed modules; os only for the clock.
    for (const char* drop : {"load", "loadstring"}) {
        lua_pushnil(L);
        lua_setglobal(L, drop);
    }
    lua_getglobal(L, "string");
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    lua_getglobal(L, "os");
    lua_newtable(L);
    for (const char* keep : {"clock", "time", "date", "difftime"}) {
        lua_getfield(L, -2, keep);
        lua_setfield(L, -2, keep);
    }
    lua_setglobal(L, "os");
    lua_pop(L, 1);
    for (auto [n, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
             {"print", l_print}, {"require", l_require}}) {
        lua_pushcfunction(L, fn);
        lua_setglobal(L, n);
    }
    lua_getglobal(L, "setmetatable");
    lua_pushcclosure(L, l_setmetatable, 1);
    lua_setglobal(L, "setmetatable");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "hailstone.loaded");
    ui::lua::register_ui_api(L);
    // ui.GetClipboardText / SetClipboardText (ui/lua/lua_code_editor.cpp): the player's clipboard, only for a script
    // that may do everything (asked at each call: the rules may change while it runs).
    lua_pushcfunction(L, [](lua_State* l) -> int {
        lua_pushboolean(l, Script::of(l).caps == plugin::kCapAll);
        return 1;
    });
    lua_setfield(L, LUA_REGISTRYINDEX, "hailstone.clipboard_allowed");

    luaL_newmetatable(L, kTloMeta);
    for (auto [n, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
             {"__gc", tlo_gc}, {"__index", tlo_index}, {"__call", tlo_call}, {"__tostring", tlo_tostring},
             {"__concat", tlo_concat}, {"__eq", tlo_eq}, {"__lt", tlo_lt}, {"__le", tlo_le}}) {
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, n);
    }
    // Locked: every TLO value is marked for finalization when it is made, and Lua looks __gc up only when it
    // collects one, so a script that could reach this table could swap in a finalizer of its own.
    lua_pushliteral(L, "mq.TLO");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    lua_newtable(L); // mq
    for (auto [n, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
             {"_cmd", l_cmd}, {"gettime", l_gettime}, {"_delay_ms", l_delay_ms}, {"parse", l_parse},
             {"exit", l_exit}, {"event", l_event}, {"unevent", l_unevent}, {"flushevents", l_flushevents},
             {"_next_event", l_next_event}, {"bind", l_bind}, {"unbind", l_unbind},
             {"getAllSpawns", l_get_all_spawns}, {"_write_config", l_write_config}, {"_dry", l_dry}}) {
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, n);
    }
    lua_newtable(L); // mq.TLO
    lua_newtable(L);
    lua_pushcfunction(L, tlo_root_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, "TLO");
    lua_newtable(L); // mq.imgui
    for (auto [n, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
             {"init", l_imgui_init}, {"destroy", l_imgui_destroy}, {"exists", l_imgui_exists}}) {
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, n);
    }
    lua_setfield(L, -2, "imgui");
    push_web_module(L); // mq.web (mq/lua_mq_web.cpp)
    lua_setfield(L, -2, "web");
    // The folder the script was found in (the server's or the player's).
    std::string lua_dir = env.dirs.empty() ? std::string() : env.dirs.front().string();
    for (const auto& d : dirs()) {
        fs::path rel = sc.entry.lexically_relative(d);
        if (!sc.entry.empty() && !rel.empty() && *rel.begin() != "..") {
            lua_dir = d.string();
            break;
        }
    }
    lua_pushstring(L, lua_dir.c_str());
    lua_setfield(L, -2, "luaDir");
    migrate_config_dir(sc);
    lua_pushstring(L, script_config_dir(sc).string().c_str());
    lua_setfield(L, -2, "configDir");
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, "hailstone.mq");
    lua_pushvalue(L, -1);
    lua_setglobal(L, "mq");
    register_hailstone(L);
    if (luaL_loadbufferx(L, kPrelude, std::strlen(kPrelude), "=mq", "t") != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        sc.close();
        why = "internal error: " + err;
        return false;
    }
    lua_insert(L, -2);
    lua_pushcfunction(L, l_load_config); // (the prelude's alone: loadfile and dofile check what it loads)
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        sc.close();
        why = "internal error: " + err;
        return false;
    }
    return true;
}

bool ScriptHost::run(const std::string& name, const std::vector<std::string>& args, std::string* why) {
    return run_file(name, args, why, false);
}

bool ScriptHost::run_file(const std::string& name, const std::vector<std::string>& args, std::string* why,
                          bool zone_policy) {
    Impl& m = *impl_;
    auto refuse = [&](std::string text) {
        if (why) *why = text;
        m.say("[Lua] " + text);
        return false;
    };
    m.reap();
    std::string key = lower(name);
    // The Lua socials' names (button_name): a script of such a name would share a social's config folder and
    // browser owner.
    if (key.starts_with(kSocialPrefix)) return refuse(std::format("{}: a script's name cannot start with \"{}\"", key, kSocialPrefix));
    if (Script* old = m.find(key); old && !old->stop_requested) return refuse(key + " is already running");
    // reap() closed every stopped one nothing is in. One still running code (it stops itself and runs itself again)
    // would close later, and its close takes the name's UI registrations away: the new one's too.
    if (Script* old = m.find(key); old && !old->closing) return refuse(key + " is still stopping: run it again");
    auto entry = m.resolve(key);
    if (!entry) return refuse("no script named " + name);
    auto text = read_file(*entry);
    if (!text) return refuse("cannot read " + entry->string());
    Sha256Digest hash = sha256(*text);
    const ManagedScript* managed = nullptr;
    for (const auto& addon : m.managed) {
        std::error_code ec;
        if (fs::equivalent(*entry, addon.entry, ec)) { managed = &addon; break; }
    }
    // Activation follows the installed file, not its spelling in /lua run. Paths such as addon/init.lua,
    // dotted aliases and in-folder links must never bypass disabled state or create a second owner of its UI.
    if (managed && (!managed->enabled || !managed->verified || hash != managed->hash))
        return refuse(key + ": the installed add-on is disabled or its installed files do not verify");
    if (managed && key != managed->name)
        return refuse(key + ": use the installed add-on's name " + managed->name);
    if (!m.env.policy) return refuse("scripts are off");
    std::uint32_t caps = m.env.policy->caps(plugin::FileKind::Script, hash, script_identity(m, *entry, key));
    if (caps == 0) return refuse(std::format("{} may not run: {}", key, m.env.policy->refusal(plugin::FileKind::Script)));
    // A script another started (mq.cmd("/lua run"), a plugin, a plugin's window) gets no more than its starter may
    // do now; the player's own start gives it all its file allows.
    caps &= plugin::caller_caps();
    if (caps == 0)
        return refuse(std::format("{} may not run: {} may not give it anything it would do", key,
                                  plugin::caller_name()));

    auto s = std::make_unique<Script>();
    Script& sc = *s;
    sc.host = &m;
    sc.runaway_ms = m.runaway_ms();
    sc.name = key;
    sc.entry = *entry;
    sc.root = entry->parent_path();
    sc.hash = hash;
    sc.tlo_caller = m.env.policy->verified_tlo_caller(plugin::FileKind::Script, hash, *entry);
    if (!plugin::may_inherit_tlo_proof(sc.tlo_caller)) sc.tlo_caller.reset();
    sc.managed = managed != nullptr;
    if (managed) sc.managed_revision = managed->revision;
    sc.caps = caps;
    sc.args = args;
    sc.owner = m.env.hooks->new_guest("lua:" + key);
    if (std::string err; !m.open_state(sc, err)) return refuse(err);
    lua_State* L = sc.L;

    std::string chunk = "@" + entry->string();
    if (luaL_loadbufferx(L, text->data(), text->size(), chunk.c_str(), "t") != LUA_OK) {
        std::string err = lua_tostring(L, -1);
        sc.close();
        return refuse(key + ": " + err);
    }
    sc.co = lua_newthread(L);
    lua_insert(L, -2);
    lua_xmove(L, sc.co, 1); // the chunk onto the coroutine
    sc.co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    watch(sc.co);
    m.say(std::format("[Lua] running {} ({})", key, plugin::caps_text(caps)), zone_policy, "lua:" + key);
    log::info("lua: running {} from {}, sha256 {}, caps {}", key, entry->string(), to_hex(hash), plugin::caps_text(caps));
    m.scripts.push_back(std::move(s));
    return true;
}

bool ScriptHost::Impl::kept(const Script& s) const {
    return env.policy && env.policy->current() && env.policy->current()->configured &&
           (env.policy->server_file(plugin::FileKind::Script, s.hash) ||
            env.policy->required_file(plugin::FileKind::Script, s.hash));
}

void ScriptHost::Impl::end(Script& s, const std::string& why, bool zone_policy) {
    s.stop_requested = true;
    if (s.tlo_caller) s.tlo_caller->revoke();
    if (s.status == Script::Status::Running || s.status == Script::Status::Paused) s.status = Script::Status::Exited;
    say("[Lua] " + s.name + " " + why, zone_policy, "lua:" + lower(s.name), true);
}

bool ScriptHost::Impl::may_control(const Script& s) const { return (s.caps & ~plugin::caller_caps()) == 0; }

bool ScriptHost::Impl::refuse_control(const Script& s) const {
    if (may_control(s)) return false;
    say(std::format("[Lua] {} may not stop or pause {}: it may do more than {}", plugin::caller_name(), s.name,
                    plugin::caller_name()));
    return true;
}

bool ScriptHost::stop(std::string_view name) {
    Script* s = impl_->find(lower(name));
    if (!s || s->stop_requested) return false;
    if (impl_->kept(*s)) {
        impl_->say("[Lua] " + s->name + (impl_->env.policy->server_file(plugin::FileKind::Script, s->hash)
                                           ? " is this server's own script: it keeps running"
                                           : " is required by this server: it keeps running"));
        return true;
    }
    if (impl_->refuse_control(*s)) return true;
    impl_->end(*s, "stopped");
    impl_->reap();
    return true;
}

void ScriptHost::stop_all() {
    for (auto& s : impl_->scripts)
        if (!s->stop_requested) impl_->end(*s, "stopped");
    impl_->reap();
}

void ScriptHost::set_server_dir(const fs::path& dir) { impl_->server_dir = dir; }

bool ScriptHost::pause(std::string_view name, std::optional<bool> paused) {
    Script* s = impl_->find(lower(name));
    if (!s || s->stop_requested) return false;
    if (impl_->kept(*s)) {
        impl_->say("[Lua] " + s->name + (impl_->env.policy->server_file(plugin::FileKind::Script, s->hash)
                                           ? " is this server's own script: it keeps running"
                                           : " is required by this server: it keeps running"));
        return true;
    }
    if (impl_->refuse_control(*s)) return true;
    bool now_paused = paused ? *paused : s->status != Script::Status::Paused;
    s->status = now_paused ? Script::Status::Paused : Script::Status::Running;
    impl_->say(std::format("[Lua] {} {}", s->name, now_paused ? "paused" : "resumed"));
    return true;
}

std::vector<ScriptHost::Info> ScriptHost::list() const {
    std::vector<Info> out;
    for (const auto& s : impl_->scripts) {
        Info i;
        i.name = s->name;
        switch (s->status) {
        case Script::Status::Running: i.status = "RUNNING"; break;
        case Script::Status::Paused: i.status = "PAUSED"; break;
        case Script::Status::Exited: i.status = "EXITED"; break;
        case Script::Status::Failed: i.status = "ERROR"; break;
        }
        i.caps = s->caps;
        i.sha256 = to_hex(s->hash);
        i.error = s->error;
        i.kept = impl_->kept(*s);
        out.push_back(std::move(i));
    }
    return out;
}

std::uint32_t ScriptHost::caps_of(std::string_view name) const {
    for (const auto& s : impl_->scripts)
        if (s->name == name && (s->status == Script::Status::Running || s->status == Script::Status::Paused)) return s->caps;
    return 0;
}

bool ScriptHost::running(std::string_view name) const {
    const Script* s = impl_->find(lower(name));
    return s && !s->stop_requested && (s->status == Script::Status::Running || s->status == Script::Status::Paused);
}

// A stopped script that nothing has entered is closed at once, even while pulse() or imgui() walks the lists: a
// script of the same name started in the walk must not lose its registrations to the old one's close() later. Only
// the erase waits for the walk's end (its own reap()).
void ScriptHost::Impl::reap() {
    if (walking > 0) {
        for (auto& [social, sp] : buttons)
            if (sp->stop_requested && sp->busy == 0) sp->close();
        for (auto& sp : scripts)
            if (sp->stop_requested && sp->busy == 0) {
                if (sp->status == Script::Status::Running || sp->status == Script::Status::Paused)
                    sp->status = Script::Status::Exited;
                sp->close();
            }
        return;
    }
    for (auto it = buttons.begin(); it != buttons.end();) {
        Script& s = *it->second;
        if (s.stop_requested && s.busy == 0) {
            s.close();
            it = buttons.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = scripts.begin(); it != scripts.end();) {
        Script& s = **it;
        if (s.stop_requested && s.busy == 0) {
            if (s.status == Script::Status::Running || s.status == Script::Status::Paused) s.status = Script::Status::Exited;
            s.close();
            it = scripts.erase(it);
        } else {
            ++it;
        }
    }
}

void ScriptHost::pulse() {
    Impl& m = *impl_;
    Impl::Walk walk(m);
    // A script started in this walk (mq.cmd('/lua run x')) first runs next frame: two scripts that restart each other
    // cannot keep one walk going.
    const std::size_t count = m.scripts.size();
    for (std::size_t i = 0; i < count; ++i) {
        Script& s = *m.scripts[i];
        if (s.stop_requested || s.status != Script::Status::Running || !s.co) continue;
        if (s.main_done) { // a plugin whose main chunk returned: only its binds' threads
            ++s.busy;
            plugin::CallerScope scope(s.caps, s.name, s.tlo_caller);
            m.resume_threads(s);
            --s.busy;
            continue;
        }
        ++s.busy;
        plugin::CallerScope scope(s.caps, s.name, s.tlo_caller);
        s.new_slice();
        int nargs = 0;
        if (!s.started) {
            s.started = true;
            if (s.args.size() > 1000 || !lua_checkstack(s.co, static_cast<int>(s.args.size()) + 1)) {
                s.fail("too many arguments");
                --s.busy;
                continue;
            }
            for (const auto& a : s.args) lua_pushlstring(s.co, a.data(), a.size());
            nargs = static_cast<int>(s.args.size());
        }
        int nres = 0;
        int st = 0;
        {
            ScriptRun run(s, s.co);
            st = lua_resume(s.co, nullptr, nargs, &nres);
        }
        if (st == LUA_YIELD) {
            lua_pop(s.co, nres);
        } else if (st == LUA_OK && s.registered) {
            s.main_done = true; // a plugin: it stays for what it gave the client (hooks, packets, windows)
        } else if (st == LUA_OK) {
            s.stop_requested = true; // the script's main chunk returned
            s.status = Script::Status::Exited;
            m.say("[Lua] " + s.name + " ended");
        } else if (lua_touserdata(s.co, -1) == &kExitKey) {
            s.stop_requested = true;
            s.status = Script::Status::Exited;
            m.say("[Lua] " + s.name + " ended");
        } else {
            const char* err = lua_tostring(s.co, -1);
            luaL_traceback(s.L, s.co, err ? err : "error", 0);
            std::string trace = lua_tostring(s.L, -1);
            lua_pop(s.L, 1);
            s.fail(trace.substr(0, trace.find('\n')), trace); // the traceback in the log, once
        }
        m.resume_threads(s);
        --s.busy;
    }
    for (auto& [social, sp] : m.buttons) {
        Script& s = *sp;
        if (s.stop_requested || s.threads.empty()) continue;
        ++s.busy;
        plugin::CallerScope scope(s.caps, s.name);
        m.resume_threads(s);
        --s.busy;
    }
    walk.end();
    m.reap();
}

// The binds (and a Lua social's presses) still running.
void ScriptHost::Impl::resume_threads(Script& s) {
    for (std::size_t t = 0; t < s.threads.size() && !s.stop_requested;) {
        const auto [thread, ref, caps, tlo_allowed] = s.threads[t];
        s.new_slice();
        int tn = 0;
        int ts = 0;
        {
            plugin::CallerScope scope(caps, s.name,
                                      tlo_allowed ? plugin::WeakTloCaller(s.tlo_caller) : plugin::WeakTloCaller{});
            const std::uint32_t narrowed = std::exchange(s.narrowed, caps);
            ScriptRun run(s, thread);
            ts = lua_resume(thread, nullptr, 0, &tn);
            s.narrowed = narrowed;
        }
        if (ts == LUA_YIELD) {
            lua_pop(thread, tn);
            ++t;
            continue;
        }
        if (ts != LUA_OK) {
            if (lua_touserdata(thread, -1) == &kExitKey) {
                if (s.social < 0) s.stop_requested = true;
            } else {
                s.fail(lua_tostring(thread, -1) ? lua_tostring(thread, -1) : "error");
            }
        }
        luaL_unref(s.L, LUA_REGISTRYINDEX, ref);
        s.threads.erase(s.threads.begin() + static_cast<std::ptrdiff_t>(t));
    }
}

// A window's draw may start or stop scripts (mq.cmd('/lua run x')): the list is walked by index, and nothing is
// removed from it (reap) until the walk ends.
void ScriptHost::imgui() {
    Impl& m = *impl_;
    Impl::Walk walk(m);
    for (std::size_t i = 0; i < m.scripts.size(); ++i) m.draw_windows(*m.scripts[i]);
    for (auto& [social, sp] : m.buttons) m.draw_windows(*sp);
    walk.end();
    m.reap();
}

void ScriptHost::Impl::draw_windows(Script& s) {
    if (s.stop_requested || s.windows.empty() || (s.caps & plugin::kCapUi) == 0) return;
    ++s.busy;
    plugin::CallerScope scope(s.caps, s.name, s.tlo_caller);
    // A draw function may destroy or make windows (its own too): this frame draws the ones there were, those still
    // there. The main state's preempt_hook (open_state) stops one that runs kRunawayMs. (The copy goes into the same
    // vector each frame: its strings keep their buffers, so a frame allocates nothing.)
    s.drawing = s.windows;
    for (const auto& w : s.drawing) {
        if (s.stop_requested) break;
        if (std::find(s.windows.begin(), s.windows.end(), w) == s.windows.end()) continue;
        ImGuiErrorRecoveryState state;
        ImGui::ErrorRecoveryStoreState(&state);
        const std::size_t begun = ui::lua::windows_begun();
        lua_rawgeti(s.L, LUA_REGISTRYINDEX, w.second);
        s.new_slice();
        int st = 0;
        {
            ScriptRun run(s);
            st = lua_pcall(s.L, 0, 0, 0);
        }
        // What the script left open is closed, error or not: the next windows (the consent prompt among them) do
        // not nest in it or take its next-window settings.
        ImGui::ErrorRecoveryTryToRecoverState(&state);
        ui::lua::end_script_windows(begun);
        if (st != LUA_OK) {
            if (lua_touserdata(s.L, -1) == &kExitKey) {
                if (s.social < 0) s.stop_requested = true;
                else stop_threads(s);
            } else {
                s.fail(std::string("window ") + w.first + ": " + (lua_tostring(s.L, -1) ? lua_tostring(s.L, -1) : "error"));
            }
            lua_pop(s.L, 1);
        }
    }
    --s.busy;
}

void ScriptHost::policy_changed(const std::vector<std::string>& autoload, bool zone_policy) {
    Impl& m = *impl_;
    for (auto& sp : m.scripts) {
        Script& s = *sp;
        if (s.stop_requested) continue;
        std::uint32_t caps = m.env.policy
                                 ? m.env.policy->caps(plugin::FileKind::Script, s.hash, script_identity(m, s.entry, s.name))
                                 : 0;
        if (m.env.policy && m.env.policy->current() && m.env.policy->current()->addons.enabled)
            for (const auto& module : s.modules) {
                const auto allowed = m.env.policy->caps(plugin::FileKind::Script, module.second, module.first);
                if ((allowed & caps) != caps) { caps = 0; break; }
            }
        if (s.tlo_caller && s.tlo_caller->identity().empty()) {
            auto current = m.env.policy->verified_tlo_caller(plugin::FileKind::Script, s.hash, s.entry);
            if (!current || !s.tlo_caller->same_addon(*current) ||
                std::any_of(s.tlo_modules.begin(), s.tlo_modules.end(), [&](const auto& module) {
                    return m.env.policy->verified_tlo_identity(plugin::FileKind::Script, module.second, module.first,
                                                               false) != current->identity();
                }))
                current.reset();
            // The same proof, renewed: the /timed lines and events the script queued under the old policy hold it.
            if (current) {
                s.tlo_caller->renew(*current);
            } else {
                s.tlo_caller->revoke();
                s.tlo_caller.reset();
            }
        }
        if (caps == 0) {
            m.end(s, "stopped: it may not run here", zone_policy);
        } else if (caps != s.caps) {
            s.caps = caps;
            constexpr std::uint32_t needed = plugin::kCapUi | plugin::kCapExtend;
            if ((caps & needed) != needed && m.env.ui)
                m.env.ui->remove_contributions(s.name, plugin::UiBridge::Kind::Script);
            if (!(caps & plugin::kCapExtend)) game::clear_class_name_overrides(s.class_names_owner);
            invalidate_plugin_lifecycle(s);
            m.say(std::format("[Lua] {} now may: {}", s.name, plugin::caps_text(caps)));
        }
    }
    plugin::ButtonRules r = m.env.policy ? m.env.policy->button_rules() : plugin::ButtonRules{};
    for (auto& [social, sp] : m.buttons) {
        Script& s = *sp;
        if (r.mode != plugin::ButtonMode::Full) {
            if (button_running(social))
                m.say("[Lua] " + s.name + " stopped: this server does not let it run on", zone_policy,
                      "lua-button:" + lower(s.name), true);
            m.stop_threads(s);
        } else {
            if (s.caps != r.caps) invalidate_plugin_lifecycle(s);
            s.caps = r.caps;
            if (!(s.caps & plugin::kCapExtend)) game::clear_class_name_overrides(s.class_names_owner);
        }
    }
    m.reap();
    // The server's scripts its hailstone.cfg starts ([scripts] run).
    if (m.env.policy && m.env.policy->current())
        for (const auto& name : m.env.policy->current()->run_scripts)
            if (!running(name)) run_file(name, {}, nullptr, zone_policy);
    // Required managed add-ons start independently of the player's autorun choices.
    m.required.clear();
    if (m.env.policy && m.env.policy->current() && !m.env.policy->current()->provisional) {
        m.required = m.env.policy->required_entries(plugin::FileKind::Script);
        for (auto& entry : m.required) {
            entry.first = lower(entry.first);
            if (running(entry.first)) continue;
            const auto file = m.resolve(entry.first);
            const auto text = file ? read_file(*file) : std::nullopt;
            if (text && sha256(*text) == entry.second &&
                m.env.policy->caps(plugin::FileKind::Script, entry.second, entry.first) != 0)
                run_file(entry.first, {}, nullptr, zone_policy);
        }
    }
    // The player's own the settings and the character's autorun list name.
    m.start_allowed(autoload, zone_policy);
    m.start_allowed(m.autorun, zone_policy);
    m.start_managed(zone_policy);
}

std::size_t ScriptHost::retry_required_addons() {
    Impl& m = *impl_;
    if (!m.env.policy || !m.env.policy->current() || m.env.policy->current()->provisional) return 0;
    m.reap();
    std::size_t started = 0;
    const auto required = m.required; // (run() can reach policy_changed)
    for (const auto& [name, hash] : required) {
        if (running(name)) continue;
        const auto file = m.resolve(name);
        const auto text = file ? read_file(*file) : std::nullopt;
        if (!text || sha256(*text) != hash || m.env.policy->caps(plugin::FileKind::Script, hash, name) == 0) continue;
        log::info("lua: the player retries the required add-on {}", name);
        if (run(name, {})) ++started;
    }
    return started;
}

void ScriptHost::Impl::start_managed(bool zone_policy) {
    if (!env.policy || !env.policy->received()) return;
    for (const auto& addon : managed) {
        if (!addon.enabled || !addon.verified || self->running(addon.name)) continue;
        auto entry = resolve(addon.name);
        std::error_code ec;
        if (!entry || !fs::equivalent(*entry, addon.entry, ec)) continue; // never start a shadowing server/manual file
        const auto hash = file_hash(*entry);
        if (!hash || *hash != addon.hash || env.policy->caps(plugin::FileKind::Script, *hash) == 0) continue;
        self->run_file(addon.name, {}, nullptr, zone_policy);
    }
}

void ScriptHost::set_managed_addons(std::vector<ManagedScript> addons) {
    Impl& m = *impl_;
    for (auto& addon : addons) addon.name = lower(addon.name);
    // No unverified manifest or changed file can turn into a hand-installed script through this path.
    for (auto& sp : m.scripts) {
        Script& s = *sp;
        const auto found = std::find_if(addons.begin(), addons.end(), [&](const ManagedScript& a) {
            std::error_code path_error;
            return fs::equivalent(s.entry, a.entry, path_error);
        });
        std::error_code ec;
        const bool owns_entry = found != addons.end() && fs::equivalent(s.entry, found->entry, ec);
        if (s.managed && (found == addons.end() || !owns_entry || !found->enabled || !found->verified ||
                          s.hash != found->hash || s.managed_revision != found->revision)) {
            if (!s.stop_requested) m.end(s, "managed add-on deactivated or changed");
        } else if (owns_entry) {
            if (s.name != found->name || !found->enabled || !found->verified || s.hash != found->hash) {
                if (!s.stop_requested) m.end(s, "managed add-on is disabled or modified");
            } else {
                s.managed = true;
                s.managed_revision = found->revision;
            }
        }
    }
    m.managed = std::move(addons);
    m.reap();
    m.start_managed();
}

bool ScriptHost::required_addons_ready(std::string* problem) const {
    if (problem) problem->clear();
    Impl& m = *impl_;
    if (!m.env.policy || !m.env.policy->current()) return true;
    const auto& rules = *m.env.policy->current();
    if (!rules.addons.enabled || rules.provisional) return true;
    Impl::ReadyInputs inputs{m.env.policy->serial(), m.required, {}};
    for (const auto& s : m.scripts)
        inputs.scripts.push_back({s->name, s->error, s->hash, static_cast<int>(s->status), s->started,
                                  s->stop_requested, s->modules});
    if (m.ready_inputs != inputs) {
        m.ready_inputs = std::move(inputs);
        m.ready_problem.clear();
        m.ready_answer = m.check_required_addons(&m.ready_problem);
    }
    if (problem) *problem = m.ready_problem;
    return m.ready_answer;
}

bool ScriptHost::Impl::check_required_addons(std::string* problem) const {
    const Impl& m = *this;
    auto fail = [&](std::string text) {
        if (problem) *problem = std::move(text);
        return false;
    };
    for (const auto& s : m.scripts) {
        if (s->stop_requested) continue;
        if (m.env.policy->caps(plugin::FileKind::Script, s->hash, script_identity(m, s->entry, s->name)) == 0)
            return fail(s->name + " is still running but is not allowed by this server");
        for (const auto& module : s->modules)
            if (m.env.policy->caps(plugin::FileKind::Script, module.second, module.first) == 0)
                return fail(s->name + " is still running a module not allowed by this server");
    }
    for (const auto& entry : m.required) {
        // Named by its repository, as the server's add-on policy names it (docs/plugins.md).
        const std::string who =
            "Required add-on " + m.env.policy->required_label(plugin::FileKind::Script, entry.second, entry.first);
        if (m.env.policy->caps(plugin::FileKind::Script, entry.second, entry.first) == 0)
            return fail(who + " is not permitted by this server's rules");
        const Script* s = m.find(entry.first);
        if (s && s->hash != entry.second)
            return fail(who + " is shadowed by a different script");
        if (!s || s->stop_requested)
            return fail(who + " did not remain running");
        if (s->status != Script::Status::Running)
            return fail(who + (s->error.empty() ? " is not running" : ": " + s->error));
        if (!s->started) return false;
    }
    return true;
}

void ScriptHost::Impl::start_allowed(const std::vector<std::string>& names, bool zone_policy) {
    if (!env.policy || !env.policy->received()) return;
    for (const auto& name : names) {
        if (name.empty() || self->running(name)) continue;
        auto entry = resolve(lower(name));
        auto text = entry ? read_file(*entry) : std::nullopt;
        if (text && env.policy->caps(plugin::FileKind::Script, sha256(*text), name) != 0)
            self->run_file(name, {}, nullptr, zone_policy);
    }
}

std::optional<Sha256Digest> ScriptHost::Impl::file_hash(const fs::path& file) const {
    std::error_code ec;
    auto size = fs::file_size(file, ec);
    if (ec) return std::nullopt;
    auto time = fs::last_write_time(file, ec);
    if (ec) return std::nullopt;
    if (auto it = hashes.find(file); it != hashes.end() && it->second.size == size && it->second.time == time)
        return it->second.hash;
    auto text = read_file(file);
    if (!text) return std::nullopt;
    HashEntry& e = hashes[file];
    e = {size, time, sha256(*text)};
    return e.hash;
}

std::vector<ScriptHost::Found> ScriptHost::found() const {
    const Impl& m = *impl_;
    std::vector<Found> out;
    const auto dirs = m.dirs();
    for (std::size_t d = 0; d < dirs.size(); ++d) {
        std::error_code ec;
        std::vector<std::pair<std::string, fs::path>> here;
        for (fs::directory_iterator it(dirs[d], ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path& p = it->path();
            std::error_code fec;
            if (it->is_directory(fec)) {
                // mq.configDir's settings, not a script
                if (windows_file_key(p.filename().generic_string()) == "config") continue;
                if (fs::is_regular_file(p / "init.lua", fec)) here.emplace_back(lower(p.filename().string()), p / "init.lua");
            } else if (it->is_regular_file(fec) && lower(p.extension().string()) == ".lua") {
                here.emplace_back(lower(p.stem().string()), p);
            }
        }
        // <name>.lua before <name>/init.lua, as resolve() looks
        std::sort(here.begin(), here.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first < b.first : a.second.filename() != "init.lua";
        });
        for (const auto& candidate : here) { // (no structured binding: Apple Clang's lambdas cannot capture one)
            const std::string& name = candidate.first;
            const fs::path& entry = candidate.second;
            if (name.empty() || name.find("..") != std::string::npos) continue;
            if (std::any_of(out.begin(), out.end(), [&name](const Found& f) { return f.name == name; })) continue;
            Found f;
            f.name = name;
            f.entry = entry;
            f.server = !m.server_dir.empty() && d == 0;
            if (!m.env.policy) {
                f.why = "scripts are off";
            } else if (auto hash = m.file_hash(entry)) {
                f.caps = m.env.policy->caps(plugin::FileKind::Script, *hash, name);
                if (f.caps == 0) f.why = m.env.policy->refusal(plugin::FileKind::Script);
            } else {
                f.why = "cannot read " + entry.string();
            }
            out.push_back(std::move(f));
        }
    }
    std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return a.name < b.name; });
    return out;
}

std::vector<fs::path> ScriptHost::folders() const { return impl_->dirs(); }

void ScriptHost::set_autorun_file(const fs::path& file, bool zone_policy) {
    Impl& m = *impl_;
    if (file == m.autorun_file) return;
    m.autorun_file = file;
    m.autorun.clear();
    IniFile ini;
    if (!file.empty() && ini.load(file))
        for (const auto& key : ini.keys("Autorun"))
            if (ini.get_int("Autorun", key, 0) != 0) m.autorun.push_back(lower(key));
    m.start_allowed(m.autorun, zone_policy);
}

bool ScriptHost::autorun(std::string_view name) const {
    const auto& a = impl_->autorun;
    return std::find(a.begin(), a.end(), lower(name)) != a.end();
}

std::vector<std::string> ScriptHost::autorun_list() const { return impl_->autorun; }
bool ScriptHost::autorun_available() const { return !impl_->autorun_file.empty(); }

bool ScriptHost::set_autorun(std::string_view name, bool on) {
    Impl& m = *impl_;
    std::string key = lower(name);
    if (m.autorun_file.empty() || key.empty() || key.find_first_of("=[]\r\n") != std::string::npos) return false;
    IniFile ini;
    ini.load(m.autorun_file); // (a missing file: a new one)
    if (on) ini.set_int("Autorun", key, 1);
    else ini.erase("Autorun", key);
    std::error_code ec;
    fs::create_directories(m.autorun_file.parent_path(), ec);
    if (!ini.save(m.autorun_file)) return false;
    std::erase(m.autorun, key);
    if (on) m.autorun.push_back(key);
    return true;
}

void ScriptHost::Impl::lua_command(std::string_view args) {
    std::string_view rest = args;
    std::string verb = lower(game::next_cmd_arg(rest));
    std::vector<std::string> words;
    while (true) {
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        if (rest.empty()) break;
        words.push_back(game::next_cmd_arg(rest));
    }
    if (verb == "run" && !words.empty()) {
        std::string name = words.front();
        words.erase(words.begin());
        self->run(name, words);
        return;
    }
    if (verb == "stop") {
        if (words.empty()) {
            for (auto& s : scripts)
                if (!s->stop_requested && !kept(*s) && may_control(*s)) end(*s, "stopped");
            reap();
        } else if (!self->stop(words.front())) say("[Lua] no running script named " + words.front());
        return;
    }
    if (verb == "pause") {
        if (words.empty()) {
            std::vector<std::string> names;
            for (auto& s : scripts)
                if (!s->stop_requested && !kept(*s) && may_control(*s)) names.push_back(s->name);
            for (const auto& n : names) self->pause(n);
        } else if (!self->pause(words.front())) {
            say("[Lua] no running script named " + words.front());
        }
        return;
    }
    if (verb == "list" || verb == "ps" || verb == "info") {
        reap();
        if (std::all_of(scripts.begin(), scripts.end(), [](const auto& s) { return s->stop_requested; }))
            say("[Lua] no scripts are running");
        for (const auto& s : scripts) {
            if (s->stop_requested) continue; // stopped while a window drew (reap waits for the walk)
            std::string st = s->status == Script::Status::Paused ? "paused" : "running";
            say(std::format("[Lua] {} ({}) - {}", s->name, st, plugin::caps_text(s->caps)));
        }
        return;
    }
    say("[Lua] usage: /lua run <script> [args] | stop [script] | pause [script] | list");
}

} // namespace hailstone::mq

// ---------------------------------------------------------------- Lua socials

namespace hailstone::mq {

namespace {

std::string button_name(const std::string& name) { return std::string(kSocialPrefix) + name + "'"; }

// t[key] = the value at the top, popped (raw: no metamethods run).
void raw_set(lua_State* L, int t, const char* key) {
    t = lua_absindex(L, t);
    lua_pushstring(L, key);
    lua_insert(L, -2);
    lua_rawset(L, t);
}

// The `button` table the social's Lua sees. Read and written raw, outside any protected call: a metamethod the
// social's code gave `button` or _G would run where its error could only abort.
void set_button(Script& s, const ButtonPress& p) {
    lua_State* L = s.L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_pushliteral(L, "button");
    if (lua_rawget(L, -2) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        raw_set(L, -3, "button");
    }
    lua_pushinteger(L, p.social + 1);
    raw_set(L, -2, "social");
    if (p.bar >= 0) lua_pushinteger(L, p.bar + 1);
    else lua_pushnil(L);
    raw_set(L, -2, "bar");
    if (p.button >= 0) lua_pushinteger(L, p.button + 1);
    else lua_pushnil(L);
    raw_set(L, -2, "button");
    lua_pushboolean(L, p.shift);
    raw_set(L, -2, "shift");
    lua_pushboolean(L, p.ctrl);
    raw_set(L, -2, "ctrl");
    lua_pushboolean(L, p.alt);
    raw_set(L, -2, "alt");
    lua_pushinteger(L, static_cast<lua_Integer>(s.presses));
    raw_set(L, -2, "presses");
    lua_pop(L, 2);
}

// The value of `key` in the table at the top (raw: no metamethods run).
int raw_field(lua_State* L, const char* key) {
    lua_pushstring(L, key);
    return lua_rawget(L, -2);
}

std::string string_field(lua_State* L, const char* key) {
    std::string out;
    int t = raw_field(L, key);
    if (t == LUA_TSTRING || t == LUA_TNUMBER) {
        std::size_t n = 0;
        const char* p = lua_tolstring(L, -1, &n);
        out.assign(p, n);
    }
    lua_pop(L, 1);
    return out;
}

ButtonFace read_face(lua_State* L) {
    ButtonFace f;
    if (lua_type(L, -1) == LUA_TSTRING) {
        f.has = true;
        f.label = lua_tostring(L, -1);
        return f;
    }
    if (!lua_istable(L, -1)) return f;
    f.has = true;
    f.label = string_field(L, "label");
    f.overlay = string_field(L, "overlay");
    f.tooltip = string_field(L, "tooltip");
    if (raw_field(L, "color") == LUA_TNUMBER) {
        f.color = static_cast<std::uint32_t>(lua_tointeger(L, -1)) & 0xffffff;
    } else if (lua_istable(L, -1)) {
        std::uint32_t rgb = 0;
        for (int i = 1; i <= 3; ++i) {
            lua_rawgeti(L, -1, i);
            double v = lua_tonumber(L, -1);
            lua_pop(L, 1);
            if (v <= 1.0) v *= 255.0;
            rgb = rgb << 8 | saturate_cast<std::uint32_t>(std::clamp(v, 0.0, 255.0)); // (NaN: 0)
        }
        f.color = rgb;
    }
    lua_pop(L, 1);
    if (raw_field(L, "cooldown") == LUA_TNUMBER) f.cooldown = static_cast<float>(std::clamp(lua_tonumber(L, -1), 0.0, 1.0));
    lua_pop(L, 1);
    if (raw_field(L, "enabled") != LUA_TNIL) f.enabled = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return f;
}

} // namespace

plugin::ButtonRules ScriptHost::Impl::rules(const game::LuaSocial&) const {
    return env.policy ? env.policy->button_rules() : plugin::ButtonRules{};
}

// The social's state, made again when its code changed; null with the reason (a syntax error).
Script* ScriptHost::Impl::button(const ButtonPress& p, std::string& why) {
    Sha256Digest hash = game::lua_social_hash(p.lua);
    const int key = p.state >= 0 ? p.state : p.social;
    if (auto b = broken.find(key); b != broken.end()) {
        if (b->second.first == hash) {
            why = b->second.second;
            return nullptr;
        }
        broken.erase(b);
    }
    if (auto it = buttons.find(key); it != buttons.end()) {
        Script& old = *it->second;
        if (old.hash == hash && !old.stop_requested && old.L) {
            old.name = button_name(p.name);
            return &old;
        }
        if (old.busy > 0) {
            why = "it is running";
            return nullptr;
        }
        old.close();
        buttons.erase(it);
    }
    auto sp = std::make_unique<Script>();
    Script& sc = *sp;
    sc.host = this;
    sc.runaway_ms = runaway_ms();
    sc.social = p.social;
    sc.name = button_name(p.name);
    sc.hash = hash;
    if (!env.dirs.empty()) sc.root = env.dirs.front();
    sc.owner = env.hooks->new_guest(std::format("social:{}", p.social + 1));
    if (!open_state(sc, why)) {
        env.hooks->remove(sc.owner);
        return nullptr;
    }
    lua_State* L = sc.L;
    watch(L);
    lua_newtable(L);
    lua_setglobal(L, "state");
    auto compile = [&](const std::string& code, const char* part, int& ref) {
        if (code.empty()) return true;
        std::string chunk = std::format("={} ({})", sc.name, part);
        if (luaL_loadbufferx(L, code.data(), code.size(), chunk.c_str(), "t") != LUA_OK) {
            why = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
            lua_pop(L, 1);
            return false;
        }
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
        return true;
    };
    if (!compile(p.lua.press, "press", sc.press_ref) || !compile(p.lua.face, "face", sc.face_ref)) {
        sc.close();
        broken[key] = {hash, why};
        return nullptr;
    }
    Script* out = &sc;
    buttons[key] = std::move(sp);
    return out;
}

void ScriptHost::Impl::stop_threads(Script& s) {
    if (!s.L) return;
    for (const auto& th : s.threads) luaL_unref(s.L, LUA_REGISTRYINDEX, th.ref);
    s.threads.clear();
    for (auto& e : s.events) luaL_unref(s.L, LUA_REGISTRYINDEX, e.fn);
    s.events.clear();
    s.queued.clear();
    for (auto& [cmd, ref] : s.binds) {
        luaL_unref(s.L, LUA_REGISTRYINDEX, ref);
        env.hooks->remove_command(s.owner, cmd);
    }
    s.binds.clear();
    for (auto& [name, ref] : s.windows) luaL_unref(s.L, LUA_REGISTRYINDEX, ref);
    s.windows.clear();
}

plugin::ButtonRules ScriptHost::button_rules(const game::LuaSocial& lua) const { return impl_->rules(lua); }

bool ScriptHost::button_running(int social) const {
    auto it = impl_->buttons.find(social);
    if (it == impl_->buttons.end()) return false;
    const Script& s = *it->second;
    return !s.stop_requested && (!s.threads.empty() || !s.events.empty() || !s.binds.empty() || !s.windows.empty());
}

bool ScriptHost::stop_button(int social) {
    if (!button_running(social)) return false;
    Script& s = *impl_->buttons[social];
    if (s.busy > 0) {
        // Stopped from inside its own Lua (/stopsocial): the state goes once the Lua returns.
        s.stop_requested = true;
        return true;
    }
    impl_->stop_threads(s);
    impl_->say("[Lua] " + s.name + " stopped");
    return true;
}

void ScriptHost::forget_button(int social) {
    Impl& m = *impl_;
    if (social < 0) {
        m.broken.clear();
        for (auto& [n, s] : m.buttons) s->stop_requested = true;
        m.reap();
        return;
    }
    m.broken.erase(social);
    auto it = m.buttons.find(social);
    if (it == m.buttons.end()) return;
    it->second->stop_requested = true;
    m.reap();
}

bool ScriptHost::press_button(const ButtonPress& p, std::string* why) {
    Impl& m = *impl_;
    auto refuse = [&](const std::string& text) {
        if (why) *why = text;
        m.say(std::format("[Lua] {}: {}", button_name(p.name), text));
        return false;
    };
    if (p.social < 0 || p.lua.press.empty()) return refuse("it has no Lua to run on a press");
    // Pressed again while it runs: it stops (the original's social queue does the same with a right click).
    if (stop_button(p.social)) return true;
    plugin::ButtonRules r = m.rules(p.lua);
    if (r.mode == plugin::ButtonMode::Off)
        return refuse(m.env.policy && m.env.policy->received() ? "this server does not run Lua socials"
                                                                : "Lua socials are off until the zone-in has checked the server's files");
    std::string err;
    Script* s = m.button(p, err);
    if (!s) return refuse(err);
    ++s->presses;
    set_button(*s, p);
    s->caps = r.caps;
    if (r.mode == plugin::ButtonMode::Full) {
        start_thread(*s, s->press_ref, {});
        m.reap();
        return true;
    }
    ++s->busy;
    s->press_commands = r.press_commands;
    s->commands_left = r.press_commands > 0 ? r.press_commands : -1;
    s->runaway_ms = kPressMs;
    bool ok = true;
    {
        plugin::CallerScope scope(s->caps, s->name);
        s->new_slice();
        lua_rawgeti(s->L, LUA_REGISTRYINDEX, s->press_ref);
        int st = 0;
        {
            ScriptRun run(*s);
            st = lua_pcall(s->L, 0, 0, 0);
        }
        if (st != LUA_OK) {
            if (lua_touserdata(s->L, -1) != &kExitKey) {
                ok = false;
                s->fail(lua_tostring(s->L, -1) ? lua_tostring(s->L, -1) : "error");
            }
            lua_pop(s->L, 1);
        }
    }
    s->commands_left = -1;
    s->runaway_ms = s->host->runaway_ms();
    --s->busy;
    m.reap();
    return ok;
}

ButtonDryRun ScriptHost::dry_run_button(const ButtonPress& p) {
    Impl& m = *impl_;
    ButtonDryRun out;
    out.rules = m.rules(p.lua);
    if (p.lua.press.empty()) {
        out.error = "it has no Lua to run on a press";
        return out;
    }
    Script* s = m.button(p, out.error);
    if (!s) return out;
    if (s->busy > 0) {
        out.error = "it is running";
        return out;
    }
    set_button(*s, p);
    const std::uint32_t caps = s->caps;
    // On a server without Lua socials the dry run may still read the game state; every command shows as blocked.
    s->caps = out.rules.mode == plugin::ButtonMode::Off ? plugin::kCapRead : out.rules.caps;
    s->press_commands = out.rules.press_commands;
    s->commands_left = out.rules.mode == plugin::ButtonMode::Press && out.rules.press_commands > 0 ? out.rules.press_commands : -1;
    s->runaway_ms = kPressMs;
    s->dry = &out.lines;
    ++s->busy;
    {
        plugin::CallerScope scope(s->caps, s->name);
        s->new_slice();
        lua_rawgeti(s->L, LUA_REGISTRYINDEX, s->press_ref);
        int st = 0;
        {
            ScriptRun run(*s);
            st = lua_pcall(s->L, 0, 0, 0);
        }
        if (st != LUA_OK) {
            if (lua_touserdata(s->L, -1) != &kExitKey) out.error = lua_tostring(s->L, -1) ? lua_tostring(s->L, -1) : "error";
            lua_pop(s->L, 1);
        }
    }
    --s->busy;
    s->dry = nullptr;
    s->commands_left = -1;
    s->runaway_ms = s->host->runaway_ms();
    s->caps = caps;
    out.ok = out.error.empty();
    m.reap();
    return out;
}

const ButtonFace& ScriptHost::button_face(int social, const std::string& name, const game::LuaSocial& lua) {
    static const ButtonFace none;
    Impl& m = *impl_;
    if (lua.face.empty() || !m.rules(lua).faces) return none;
    ButtonPress p;
    p.social = social;
    p.name = name;
    p.lua = lua;
    std::string err;
    Script* s = m.button(p, err);
    if (!s) return none;
    if (s->busy > 0) return s->face;
    const std::int64_t now = m.now();
    if (s->face_ran && now - s->face_at < kFaceEveryMs) return s->face;
    s->face_ran = true;
    s->face_at = now;
    const std::uint32_t caps = s->caps;
    s->caps = plugin::kCapRead;
    s->face_eval = true;
    s->runaway_ms = kFaceMs;
    ++s->busy;
    ButtonFace f;
    {
        plugin::CallerScope scope(plugin::kCapRead, s->name);
        s->new_slice();
        lua_rawgeti(s->L, LUA_REGISTRYINDEX, s->face_ref);
        int st = 0;
        {
            ScriptRun run(*s);
            st = lua_pcall(s->L, 0, 1, 0);
        }
        if (st == LUA_OK) {
            f = read_face(s->L);
        } else {
            f.error = lua_tostring(s->L, -1) ? lua_tostring(s->L, -1) : "error";
            if (f.error != s->face.error) m.say(std::format("[Lua] {} (face): {}", s->name, f.error));
        }
        lua_pop(s->L, 1);
    }
    --s->busy;
    s->face_eval = false;
    s->runaway_ms = s->host->runaway_ms();
    s->caps = caps;
    s->face = std::move(f);
    return s->face;
}

} // namespace hailstone::mq
