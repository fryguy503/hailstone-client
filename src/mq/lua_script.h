#pragma once

// The script host's insides (mq/lua_mq.h), shared by its files: lua_mq.cpp (the mq module, running scripts) and
// lua_hailstone.cpp (the hailstone module: scripts as plugins). Not for use elsewhere.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lua.hpp>

#include "core/log.h"
#include "core/process_serial.h"
#include "core/sha256.h"
#include "game/server_ext.h"
#include "mq/event_pattern.h"
#include "mq/lua_mq.h"

namespace hailstone::mq {

namespace fs = std::filesystem;

// The mq.web table (mq/lua_mq_web.cpp), pushed onto the stack.
void push_web_module(lua_State* L);
using Clock = std::chrono::steady_clock;

inline constexpr std::size_t kMemoryLimit = 256u << 20; // a script's Lua heap
inline constexpr std::int64_t kSliceMs = 3;             // run this long, then wait for the next frame
inline constexpr std::int64_t kRunawayMs = 5000;        // this long without a chance to yield: the script is stopped
inline constexpr std::size_t kMaxQueuedEvents = 1000;
// mq.pickle's limits (docs/plugins.md): one file's size, and what the script's config directory (mq.configDir) may
// hold before a write would add a file or bytes to it.
inline constexpr std::size_t kMaxConfigFileBytes = 8u << 20;
inline constexpr std::size_t kMaxConfigFiles = 1000;
inline constexpr std::uintmax_t kMaxConfigBytes = 64u << 20;
// mq.event's limits (docs/plugins.md): every pattern is matched against every chat line, outside the script's time
// slice and memory limit, so a script's events are held well above what MQ macros use.
inline constexpr std::size_t kMaxEvents = 256;                      // events of one script
inline constexpr std::size_t kMaxEventPattern = 1024;               // bytes of one pattern
inline constexpr std::size_t kMaxEventPatterns = std::size_t{32} << 10; // bytes of all of one script's patterns
inline constexpr std::size_t kMaxEventName = 256;                   // bytes of an event's name
// What else one script may register (docs/plugins.md): each lives outside the script's Lua memory limit. The same
// name or command replaces its earlier one without counting twice; past the limit the call is an error.
inline constexpr std::size_t kMaxPacketHooks = 256; // hailstone.on_packet
inline constexpr std::size_t kMaxBinds = 256;       // mq.bind
inline constexpr std::size_t kMaxWindows = 64;      // mq.imgui.init
// A chat line this long or longer raises no events (LuaEventProcessor::Process: MQ_MAX_STRING, 2048).
inline constexpr std::size_t kMaxEventLine = 2048;
// mq.cmd's line, as MQ's own command buffer (MQ_MAX_STRING, 2048, with its terminator): longer is an error.
inline constexpr std::size_t kMaxCommandLine = 2048;
inline constexpr std::int64_t kPressMs = 50;            // a Lua social in press mode runs to its end within this
inline constexpr std::int64_t kFaceMs = 5;              // a social's face
inline constexpr std::int64_t kFaceEveryMs = 200;       // how often a face runs
// A Lua social's name begins so ("social 'Name'"); no script's may (ScriptHost::run).
inline constexpr std::string_view kSocialPrefix = "social '";
inline constexpr int kMaxNesting = 16;                  // a bind's thread started from inside the script this deep

struct Script;
class LuaExtension;
// The hailstone module (mq/lua_hailstone.cpp): the global `hailstone` in a new script state, and what a stopping
// script gave the client (its server extension, its UI) taken back.
void register_hailstone(lua_State* L);
void release_plugin(Script& s);
// A live capability/activation or whole-hook-source change invalidates source-owned interactions.
void invalidate_plugin_lifecycle(Script& s);
// The script's mq.configDir: env.config_dir for a script with every capability, config/<name>/ for another.
fs::path script_config_dir(const Script& s);

struct ScriptHost::Impl {
    ScriptHost* self = nullptr;
    ScriptEnv env;
    plugin::Hooks::Owner owner = 0;
    std::vector<std::unique_ptr<Script>> scripts;
    std::vector<std::pair<std::string, Sha256Digest>> required;
    // What required_addons_ready answered, and what it answered from: it is asked each frame, and works it out again
    // (the policy's identity checks for each script and module) only when the rules or the running scripts change.
    struct ReadyScript {
        std::string name, error;
        Sha256Digest hash{};
        int status = 0;
        bool started = false, stop_requested = false;
        std::vector<std::pair<std::string, Sha256Digest>> modules;
        bool operator==(const ReadyScript&) const = default;
    };
    struct ReadyInputs {
        std::uint32_t policy_serial = 0;
        std::vector<std::pair<std::string, Sha256Digest>> required;
        std::vector<ReadyScript> scripts;
        bool operator==(const ReadyInputs&) const = default;
    };
    std::optional<ReadyInputs> ready_inputs;
    bool ready_answer = true;
    std::string ready_problem;
    bool check_required_addons(std::string* problem) const; // required_addons_ready's work, uncached
    Clock::time_point start = Clock::now();

    std::int64_t runaway_ms() const { return env.runaway_ms > 0 ? env.runaway_ms : kRunawayMs; }
    std::int64_t now() const {
        return env.now_ms ? env.now_ms()
                          : std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    }
    void say(const std::string& text) const {
        if (env.print) env.print(text);
    }
    Script* find(std::string_view name) const;
    // Where scripts are looked for: the server's scripts/ (set_server_dir) first, then env.dirs.
    fs::path server_dir;
    std::vector<fs::path> dirs() const {
        std::vector<fs::path> out;
        if (!server_dir.empty()) out.push_back(server_dir);
        out.insert(out.end(), env.dirs.begin(), env.dirs.end());
        return out;
    }
    // One of the server's own scripts under its hailstone.cfg: the player cannot stop or pause it.
    bool kept(const Script& s) const;
    // Whether the code running now may stop or pause the script (/lua stop, /lua pause, the Scripts window): the
    // player, or code that may do all the script may (plugin::caller_caps). A script or plugin with less cannot end
    // the player's own. refuse_control says so in the chat window when it may not.
    bool may_control(const Script& s) const;
    bool refuse_control(const Script& s) const;
    void end(Script& s, const std::string& why);
    std::optional<fs::path> resolve(const std::string& name) const;
    void lua_command(std::string_view args);
    // Closes and removes the stopped scripts and socials none of whose code runs; not while pulse() or imgui()
    // walks the lists (Walk), whose Lua may start or stop scripts.
    void reap();
    int walking = 0;
    struct Walk {
        Impl* m;
        explicit Walk(Impl& im) : m(&im) { ++m->walking; }
        void end() {
            if (m) --m->walking;
            m = nullptr;
        }
        ~Walk() { end(); }
        Walk(const Walk&) = delete;
        Walk& operator=(const Walk&) = delete;
    };
    // A new script's Lua state with the libraries, the mq module and ImGui; false with the reason.
    bool open_state(Script& sc, std::string& why);
    // Moves a script's config/ folder from the name it had before script_folder_name, when that is unambiguous.
    void migrate_config_dir(const Script& s) const;
    // The character's autorun list (set_autorun_file): the file and its names, lower case, in file order.
    fs::path autorun_file;
    std::vector<std::string> autorun;
    // Starts those of `names` that are not running and the rules allow, quietly (not asked for by a command).
    void start_allowed(const std::vector<std::string>& names);
    std::vector<ManagedScript> managed;
    void start_managed();
    // found()'s hashes, by file, kept while the file's size and time stay the same (the window asks each frame).
    struct HashEntry {
        std::uintmax_t size = 0;
        fs::file_time_type time{};
        Sha256Digest hash{};
    };
    mutable std::map<fs::path, HashEntry> hashes;
    std::optional<Sha256Digest> file_hash(const fs::path& file) const;

    // The Lua socials' states (press_button), by social.
    std::map<int, std::unique_ptr<Script>> buttons;
    std::map<int, std::pair<Sha256Digest, std::string>> broken; // socials whose code does not compile, by hash
    Script* button(const ButtonPress& p, std::string& why);
    void stop_threads(Script& s);
    void resume_threads(Script& s);
    void draw_windows(Script& s);
    plugin::ButtonRules rules(const game::LuaSocial& lua) const;
};

struct Script {
    ScriptHost::Impl* host = nullptr;
    std::string name;
    fs::path entry, root;
    Sha256Digest hash{};
    // Modules already loaded into this state remain subject to a later server policy change.
    std::vector<std::pair<std::string, Sha256Digest>> modules;
    bool managed = false;
    Sha256Digest managed_revision{};
    std::uint32_t caps = 0;
    lua_State* L = nullptr;
    lua_State* co = nullptr;
    int co_ref = LUA_NOREF;
    std::vector<std::string> args;
    bool started = false;
    enum class Status { Running, Paused, Exited, Failed } status = Status::Running;
    std::string error;
    bool stop_requested = false;
    int busy = 0; // entered (resuming, a bind, a window): closing waits
    bool closing = false; // close() runs: the state's last code may give the client nothing more
    std::size_t memory = 0;
    int limited = 0;              // >0: the script's own code runs (ScriptRun), so kMemoryLimit holds
    int runs = 0;                 // the ScriptRuns alive now (each in a host frame, so each ends)
    lua_State* resumed = nullptr; // the host's thread it resumes now: the only one preempt_hook yields
    plugin::Hooks::Owner owner = 0;
    Clock::time_point slice_start{};
    // The runaway limit fired in this slice (raise_runaway): from then on every instruction of the script, and every
    // pcall, xpcall or coroutine.resume it returns to, raises the same error again, so code that catches it cannot
    // run on where the host cannot yield it (a window, a hook, a coroutine of the script's own). A new slice clears it.
    bool runaway = false;
    std::string runaway_why;
    // Text the host pushes for the script's code (hailstone.server_dir ...), kept here rather than in the C function's
    // frame: a push that fails longjmps (Lua is built as C), and the frame's objects would not be destroyed.
    std::string scratch;
    // Whether a compiled plugin has the script's name (ScriptEnv::plugin_named), asked at its first folder.
    std::optional<bool> plugin_named;

    struct Event {
        std::string name;
        EventPattern pattern;
        int fn = LUA_NOREF;
        bool keep_links = false; // mq.event's {keepLinks = true}: the line with its item links, not their text
    };
    std::vector<Event> events;
    bool chat_hooked = false;
    std::size_t packet_hooks = 0; // hailstone.on_packet registrations (they stay until the script ends)
    struct Queued {
        std::string name;
        std::vector<std::string> args; // {line, args...}
        // What the code whose line it was may do (plugin::ChatLine::origin_caps): a line a script or plugin wrote
        // runs another script's handler with no more than that (l_next_event).
        std::uint32_t origin = plugin::kCapAll;
    };
    std::deque<Queued> queued;
    std::vector<std::pair<std::string, int>> windows;                    // mq.imgui: name, draw function
    std::vector<std::pair<std::string, int>> drawing;                    // the windows draw_windows draws now
    // Binds (and a full Lua social's press, and handlers of events another's line raised) still running: each with
    // the capabilities it was started with, the script's own and its starter's together, for every resume.
    struct Thread {
        lua_State* t = nullptr;
        int ref = LUA_NOREF;
        std::uint32_t caps = 0;
    };
    std::vector<Thread> threads;
    // The capabilities of the thread resumed now (Thread::caps), which need_caps holds the script's functions to as
    // well; kCapAll for the script's own run.
    std::uint32_t narrowed = plugin::kCapAll;
    std::map<std::string, int> binds;                                    // command, function ref

    // A Lua social's state (ScriptHost::press_button).
    int social = -1;
    int press_ref = LUA_NOREF, face_ref = LUA_NOREF;
    std::int64_t runaway_ms = kRunawayMs;                // no chance to yield this long: an error
    int commands_left = -1;                              // press mode: commands this press may still send
    int press_commands = 0;
    bool face_eval = false;                              // the face runs: no commands
    std::vector<std::string>* dry = nullptr;             // a dry run: the commands are noted here, not sent
    std::uint32_t presses = 0;
    ButtonFace face;
    std::int64_t face_at = 0;
    bool face_ran = false;

    // The script as a plugin (the hailstone module): its server extension and its member of the chain, the hook
    // table, what it set, and whether it gave the client anything (then it stays after its main chunk returns).
    std::shared_ptr<LuaExtension> extension;
    int chain_id = 0;
    int hooks_ref = LUA_NOREF;
    bool active = true;
    std::uint32_t flags = 0; // HAILSTONE_FLAG_* (sdk/hailstone_plugin.h)
    // Owns the class names it set (hailstone.set_class_name; game/eq_names.h), taken back with its `extend` or its end.
    std::uint64_t class_names_owner = core::next_process_identity();
    std::optional<game::CharacterVitals> vitals;
    bool registered = false;
    bool main_done = false;

    // The host enters the script's code: its time and runaway limit start again.
    void new_slice() {
        slice_start = Clock::now();
        runaway = false;
    }

    static Script& of(lua_State* L) { return **static_cast<Script**>(lua_getextraspace(L)); }

    // `log_text`: what the log gets instead of `why` (the error with its traceback), once.
    void fail(std::string why, std::string_view log_text = {}) {
        if (social >= 0) { // a Lua social reports and stays: the next press tries again
            host->say(std::format("[Lua] {}: {}", name, why));
            log::warn("lua {}: {}", name, why);
            return;
        }
        if (status == Status::Failed || status == Status::Exited) return;
        status = Status::Failed;
        error = std::move(why);
        host->say(std::format("[Lua] {} ended with an error: {}", name, error));
        log::warn("lua script {}: {}", name, log_text.empty() ? std::string_view(error) : log_text);
        stop_requested = true;
    }

    // Takes back what the script gave the client and closes its state (lua_mq.cpp).
    void close();
};

// While it lives, the script's own code runs (a lua_resume or lua_pcall of the host's): an allocation past
// kMemoryLimit fails, and the error it raises is caught there. Outside it the host's own pushes and reads, which
// nothing would catch (a script state has no panic handler that could carry on), are never refused.
// It lives in the host's frame around the lua_resume or lua_pcall, which catches the script's errors, so it always
// ends, and it puts back the values it found: what a HostWork further in left behind when an error longjmped past
// its destructor (Lua is built as C) is undone here at the latest (and sooner by repair_limit).
struct ScriptRun {
    Script& s;
    lua_State* before;
    int before_limited;
    // `thread`: the host's coroutine resumed (preempt_hook may yield it); null for a pcall on the main state.
    explicit ScriptRun(Script& sc, lua_State* thread = nullptr) : s(sc), before(sc.resumed), before_limited(sc.limited) {
        s.limited = std::max(before_limited, 0) + 1;
        ++s.runs;
        if (thread) s.resumed = thread;
    }
    ~ScriptRun() {
        s.limited = before_limited;
        --s.runs;
        s.resumed = before;
    }
    ScriptRun(const ScriptRun&) = delete;
    ScriptRun& operator=(const ScriptRun&) = delete;
};

// The host's own work on a script's state while its code runs further up the stack (a bind its mq.cmd started):
// unprotected, so no allocation is refused until it ends. Its destructor is skipped if an error longjmps past it (a
// system out of memory, the only error its work can raise): the limit then comes back at the next instruction hook
// (repair_limit) or when the ScriptRun around it ends, not never.
struct HostWork {
    Script& s;
    int before;
    explicit HostWork(Script& sc) : s(sc), before(sc.limited) { s.limited = 0; }
    ~HostWork() { s.limited = before; }
    HostWork(const HostWork&) = delete;
    HostWork& operator=(const HostWork&) = delete;
};

// While Lua code of the script runs (a hook fires only then), a ScriptRun is around it and no HostWork is (a
// HostWork's work runs no Lua instructions but under a ScriptRun of its own): an unlimited state then is one a
// skipped HostWork destructor left, and the limit comes back. Called by the instruction hooks; cheap.
inline void repair_limit(Script& s) {
    if (s.runs > 0 && s.limited <= 0) s.limited = 1;
}

// The instruction hooks' runaway check (preempt_hook, runaway_hook), given the hook itself: an error once the slice
// ran s.runaway_ms, and from then on at every instruction (Script::runaway; a count of 1 on this thread) until the
// host starts a new slice, when the hook goes back to every 1000 instructions. `why` gets the error's text for a
// runaway that fires now (the time it ran).
template <class Why> inline void check_runaway(lua_State* L, Script& s, lua_Hook hook, std::int64_t ran, Why&& why) {
    if (s.runaway) luaL_error(L, "%s", s.runaway_why.c_str());
    if (lua_gethookcount(L) == 1) lua_sethook(L, hook, LUA_MASKCOUNT, 1000); // a runaway's, from an earlier slice
    if (ran < s.runaway_ms) return;
    s.runaway = true;
    s.runaway_why = why();
    lua_sethook(L, hook, LUA_MASKCOUNT, 1);
    luaL_error(L, "%s", s.runaway_why.c_str());
}

// pcall, xpcall, coroutine.resume and coroutine.close in a script's state (open_state): Lua's own (upvalue 1), and
// then the runaway's error raised again when it fired inside, rather than handed back as a result.
int runaway_safe_call(lua_State* L);

// The capability check of the script's functions (mq.*, hailstone.*): a Lua error when the script may not do `cap`.
// Nothing but reading works in a closing state or in a Lua social's dry run (which notes its commands and waits, and
// does nothing else).
inline void need_caps(lua_State* L, std::uint32_t cap, const char* what) {
    const Script& s = Script::of(L);
    if ((s.caps & s.narrowed & cap) != cap) {
        luaL_where(L, 1);
        lua_pushfstring(L, "%s needs the server's '%s' permission", what, plugin::caps_text(cap).c_str());
        lua_concat(L, 2);
        lua_error(L); // not luaL_error: the text's std::string is gone before the error jumps (Lua is built as C)
    }
    if ((cap & ~plugin::kCapRead) == 0) return;
    if (s.closing) luaL_error(L, "%s: the script is stopping", what);
    if (s.dry) luaL_error(L, "%s does nothing in a dry run", what);
}

} // namespace hailstone::mq
