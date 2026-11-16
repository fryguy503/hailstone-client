// The Lua UI host: script loading, hot reload, the window registry, settings and error handling, with portable
// file access and no debugging console (Hailstone can be inspected directly).
#include "ui/lua/lua_host.h"
#include "ui/lua/lua_gc_live.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "core/atomic_file.h"
#include "core/asset_folder_choice.h"
#include "core/frame_sections.h"
#include "core/log.h"
#include "plugin/lua_keep.h"

namespace hailstone::ui::lua {

namespace fs = std::filesystem;

namespace {

std::optional<fs::path> inside(const fs::path& root, std::string_view given);

struct Script {
    std::string file;    // its name in the UI's folder, "<tag>/<name>" in a plugin's (LuaUi::set_folders)
    fs::path path;
    std::string tag;     // "" the UI's own, else the folder's tag
    fs::file_time_type written{};
    std::string error;
};

// Windows are layered by name: a plugin folder's window covers the UI's (and an earlier folder's) of the same name
// instead of replacing it, so the covered one returns when the plugin goes, a reload of the UI's file does not take
// the name back, and the covering window can draw the one beneath (ui.base_window).
struct Window {
    std::string name;
    std::string file;
    std::string tag; // its script's folder tag: "" the UI's own (LuaUi::set_folders)
    int draw_ref = LUA_NOREF; // registry reference to the draw function
    bool enabled = true;      // the player's choice (saved as enabled.<name>)
    bool failed = false;      // a script error turned it off until the file reloads (or, with no file, it is retried)
    std::string error;        // with no file (registered outside its folder's loading): its error, shown on its own
    const char* section = nullptr; // its name in the frame's timed sections (core/frame_sections.h)
};

using Clock = std::chrono::steady_clock;

std::string g_drawing; // the ui.window whose draw function runs (LuaUi::drawing_window; one LuaUi per process)
std::string g_caller_tag; // whose code runs (LuaUi::caller_tag)
const LuaUi::CallbackOwner* g_callback_source=nullptr;
char kSourceFunctions;
char kMaintenanceHost;

// The collector runs in LuaUi::frame, a slice of time each frame (collect_garbage): the windows make a few hundred
// KB of garbage a frame over a live heap of tens of MB, and Lua's own pacing charges a sweep about one unit an
// object against thousands for each 8 KB allocated, so a cycle's sweep fell into one or two frames (a 20-50 ms
// hitch about once a second, whatever the step multiplier).
// The state's memory and the time a piece of code may run (LuaUi::Impl::alloc, budget_hook), well above what the
// packs use (tens of MB; a window's draw takes well under a millisecond, loading the largest file a few): a window
// or plugin that loops forever or keeps allocating is stopped with an error, and its window is turned off, instead of
// hanging the client or taking the machine's memory.
constexpr std::size_t kMemoryLimit = std::size_t{1} << 30;
constexpr auto kPackBudget = std::chrono::milliseconds(5000); // the pack's own code, one window's draw or one load
constexpr auto kPluginBudget = std::chrono::milliseconds(1000); // a plugin folder's
constexpr int kHookCount = 10000; // instructions between the budget's checks
// What the plugin folders' code may allocate: the limit less this share, which the pack's own code keeps, so a
// plugin that holds what it allocated (a huge string in its globals) cannot leave the pack's windows without memory.
constexpr std::size_t kPackReserveShare = 4; // a quarter
// A plugin folder's share of the host's C++ state, which its code could otherwise grow without end: its settings
// (ui.SetSetting with ever new keys, saved to the file), the length of each, and its windows (ui.window with ever new
// names; each window's name is also kept for the frame's timed sections).
constexpr std::size_t kPluginSettings = 1024;
constexpr std::size_t kPluginSettingKey = 256, kPluginSettingValue = 16384;
constexpr std::size_t kPluginWindows = 128;
constexpr std::size_t kPluginSections = 512; // plugin windows' timed sections kept by name, all plugins together

constexpr auto kGcSlice = std::chrono::microseconds(1000);
constexpr int kGcStart = 2; // a cycle starts when the heap is this many times the live heap after the last
constexpr int kGcHurry = 4; // and runs to its end in one frame at this many times (the slices fell behind)
// The live heap is what the cycle found reachable (gc_live_bytes), not the heap when it ended: a sweep spread over
// many frames ends with all the garbage the windows made meanwhile, which only the next cycle frees. Counted as
// live, it raised both thresholds, so the next sweep was longer and its garbage more: a night with the Triune loot
// window (a fresh table per rule each frame) grew a 50 MB heap to 9 GB, and the client into swap.
} // namespace

struct LuaUi::Impl {
    lua_State* L = nullptr;
    fs::path dir;      // the scripts
    fs::path root;     // dofile paths are relative to it (the UI's folder)
    std::vector<fs::path> bases; // approved pack bases, nearest first; no settings/global environment of their own
    LuaUi::DataRoots presentation_data;
    std::vector<fs::path> active_import_files; // protected source/data imports; a file cannot enter itself again
    std::vector<LuaUi::Folder> folders; // more folders of windows (plugins'), set_folders
    std::vector<std::string> entries;   // set_entry_scripts: the only scripts of the UI's folder loaded (none: all)
    bool folders_changed = false;
    fs::path settings_file;
    std::vector<Script> scripts;
    std::vector<Window> windows;
    std::string running_file, running_tag; // the script loading or the window drawing (ui.window's file)
    std::string drawing_tag; // the tag of the layer drawing (ui.base_window)
    std::shared_ptr<void> callback_lifetime=std::make_shared<int>(0);
    std::map<std::string,std::shared_ptr<void>,std::less<>> callback_generations;
    std::map<std::string,std::shared_ptr<LuaUi::CallbackMaintenance>,std::less<>> callback_maintenance;
    const LuaUi::CallbackMaintenance* active_maintenance = nullptr;
    int maintenance_runner_ref = LUA_NOREF;
    bool maintaining_callbacks = false, closing = false;
    LuaUi::CallerScope scope; // what a folder's code runs under (set_caller_scope)
    LuaUi::ClipboardCheck clipboard; // whether a folder's code may use the clipboard (set_clipboard_check)
    int envs_ref = LUA_NOREF; // registry: tag -> the folder's globals (push_env)
    int views_ref = LUA_NOREF; // registry: every shared-table view of any folder -> its table (weak keys; make_env)
    int host_api_ref = LUA_NOREF, ui_ref = LUA_NOREF; // the pack's hs and ui tables
    bool show_demo = false;
    bool show_control = false;
    using Settings = std::map<std::string, std::string, std::less<>>;
    Settings settings;
    bool settings_dirty = false;
    Clock::time_point settings_changed{};
    // the character's layout (set_character_settings): its window keys over the shared file's
    fs::path character_file;
    Settings character_settings;
    bool character_dirty = false;
    std::map<std::string, std::size_t, std::less<>> plugin_settings; // each plugin's count of keys (plugin_keys)
    std::size_t plugin_sections = 0; // plugin windows' timed sections kept by name (kPluginSections)
    Clock::time_point last_scan{};
    bool scanned = false;
    std::size_t memory = 0; // what the state holds now (alloc)
    std::size_t allocated = 0; // every byte the state has allocated (alloc: what grew), freed or not; it wraps
    std::size_t memory_limit = kMemoryLimit;
    int limited = 0; // >0: Lua code of ours runs (Budget), so an allocation past the limit fails
    Clock::time_point deadline = Clock::time_point::max(); // the running code's end (Budget; budget_hook)
    Clock::duration pack_budget = kPackBudget, plugin_budget = kPluginBudget;
    int live_kb = 0; // the live heap the last collection cycle found (0: none yet)
    bool collecting = false;
    int gc_steps = 0; // tests: each frame's slice is this many steps rather than kGcSlice (0: the time)

    // A slice of the collector's work (kGcSlice), while a cycle is due or under way.
    void collect_garbage() {
        auto live = [&] { return std::max(1, static_cast<int>(gc_live_bytes(L) >> 10)); };
        if (live_kb == 0) { // the scripts as loaded: their loading's garbage goes at once
            lua_gc(L, LUA_GCCOLLECT);
            live_kb = live();
            return;
        }
        const int kb = lua_gc(L, LUA_GCCOUNT);
        if (!collecting && kb >= live_kb * kGcStart) collecting = true;
        if (!collecting) return;
        const bool hurry = kb >= live_kb * kGcHurry;
        const auto until = Clock::now() + kGcSlice;
        int steps = 0;
        do {
            if (lua_gc(L, LUA_GCSTEP, 0)) { // the cycle ended
                collecting = false;
                live_kb = live();
                return;
            }
        } while (hurry || (gc_steps > 0 ? ++steps < gc_steps : Clock::now() < until));
    }

    static Impl& of(lua_State* L) {
        return **static_cast<Impl**>(lua_getextraspace(L));
    }

    // The state's allocator: past memory_limit an allocation fails while our Lua code runs (the error it raises is
    // caught there); outside it (the host's own calls into the state, which nothing would catch) it is not refused.
    static void* alloc(void* ud, void* ptr, std::size_t old_size, std::size_t new_size) {
        auto* m = static_cast<Impl*>(ud);
        const std::size_t old = ptr ? old_size : 0;
        if (new_size == 0) {
            std::free(ptr);
            m->memory -= old;
            return nullptr;
        }
        if (m->limited > 0 && new_size > old) {
            const std::size_t limit =
                g_caller_tag.empty() ? m->memory_limit : m->memory_limit - m->memory_limit / kPackReserveShare;
            if (m->memory + (new_size - old) > limit) return nullptr;
        }
        void* p = std::realloc(ptr, new_size);
        if (p) {
            m->memory = m->memory - old + new_size;
            if (new_size > old) m->allocated += new_size - old;
        }
        return p;
    }

    // Every kHookCount instructions: past the deadline the code is stopped with an error, raised again at every
    // instruction after that (a pcall in the code cannot catch its way on) until the Budget that set it ends.
    static void budget_hook(lua_State* L, lua_Debug*) {
        Impl& m = of(L);
        if (Clock::now() > m.deadline) {
            if (lua_gethookcount(L) != 1) lua_sethook(L, &budget_hook, LUA_MASKCOUNT, 1);
            luaL_error(L, "ran too long (%d ms): stopped",
                       static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            g_caller_tag.empty() ? m.pack_budget : m.plugin_budget)
                                            .count()));
        }
        if (lua_gethookcount(L) == 1) lua_sethook(L, &budget_hook, LUA_MASKCOUNT, kHookCount);
    }

    // The state's long-match check (kMatchCheckKey): a string match past the deadline is stopped as Lua code is.
    static int match_check(lua_State* L) {
        budget_hook(L, nullptr);
        return 0;
    }

    // While it lives, Lua code of ours runs: the memory limit holds, and the code may run until `budget` from now
    // (no later than an enclosing Budget allows). Lives in a block around a pcall, never across a Lua error.
    struct Budget {
        Impl& m;
        Clock::time_point before;
        Budget(Impl& im, Clock::duration budget) : m(im), before(im.deadline) {
            ++m.limited;
            m.deadline = std::min(before, Clock::now() + budget);
        }
        ~Budget() {
            --m.limited;
            m.deadline = before;
            if (lua_gethookcount(m.L) == 1) lua_sethook(m.L, &budget_hook, LUA_MASKCOUNT, kHookCount);
        }
        Budget(const Budget&) = delete;
        Budget& operator=(const Budget&) = delete;
    };
    Clock::duration budget_for(const std::string& tag) const { return tag.empty() ? pack_budget : plugin_budget; }

    static int maintenance_runner(lua_State* state) {
        const auto* callback = of(state).active_maintenance;
        if (callback) plugin::cxx_guard(state,[&]{ (*callback)(state); });
        return 0;
    }
    void maintain_callbacks() {
        if (!L || maintaining_callbacks || callback_maintenance.empty() || !lua_checkstack(L,2)) return;
        const int top = lua_gettop(L);
        // A cleanup may replace its own service; retain this bounded snapshot until its protected call returns.
        std::vector<std::shared_ptr<LuaUi::CallbackMaintenance>> pending;
        try {
            pending.reserve(callback_maintenance.size());
            for (const auto& item : callback_maintenance) pending.push_back(item.second);
        } catch (...) { return; } // an allocation failure defers metadata cleanup to the next lifecycle point
        struct Reset {
            Impl& host;
            ~Reset(){host.active_maintenance=nullptr;host.maintaining_callbacks=false;}
        } reset{*this};
        maintaining_callbacks=true;
        for (const auto& callback : pending) {
            active_maintenance=callback.get();
            lua_rawgeti(L,LUA_REGISTRYINDEX,maintenance_runner_ref);
            int status=LUA_OK;
            {
                Budget budget(*this,std::chrono::milliseconds(50));
                status=lua_pcall(L,0,0,0);
            }
            active_maintenance=nullptr;
            if (status!=LUA_OK) {
                const char* why=lua_type(L,-1)==LUA_TSTRING?lua_tostring(L,-1):"non-string error";
                log::warn("UI callback maintenance failed: {}",why);
            }
            lua_settop(L,top);
        }
    }

    // ---- settings: key=value lines, for window options the scripts keep
    static Settings read_settings(const fs::path& file) {
        Settings out;
        std::ifstream f(file);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (auto separator = line.find('='); separator != std::string::npos)
                out[line.substr(0, separator)] = line.substr(separator + 1);
        }
        return out;
    }
    // Atomically (core/atomic_file.h): a crash, a full disk or another client reading it never finds it half written.
    static void write_settings(const fs::path& file, const Settings& values) {
        std::string text;
        for (const auto& [k, v] : values) text += k + '=' + v + '\n';
        write_file_atomic(file, text);
    }
    void load_settings() {
        settings = read_settings(settings_file);
        plugin_settings.clear();
    }
    void save_settings_now() {
        if (settings_dirty) write_settings(settings_file, settings);
        if (character_dirty && !character_file.empty()) write_settings(character_file, character_settings);
        settings_dirty = character_dirty = false;
    }
    void save_settings_if_due() {
        // A slider being dragged changes values every frame: write once it settles.
        if ((!settings_dirty && !character_dirty) || Clock::now() - settings_changed < std::chrono::seconds(1)) return;
        save_settings_now();
    }
    bool per_character(std::string_view key) const {
        return !character_file.empty() && LuaUi::character_setting(plugin_key_rest(key));
    }
    const std::string* get_setting(std::string_view key) const {
        if (per_character(key)) {
            if (auto it = character_settings.find(key); it != character_settings.end()) return &it->second;
            const auto rest = plugin_key_rest(key);
            // Quest identities and open sections belong to this character, never to a shared layout or another
            // character's old global settings. Window geometry still inherits the shared layout as before.
            if (rest.starts_with("tasks.tracked.") || rest.starts_with("tasks.tracker.") ||
                rest.starts_with("tasks.section.") || rest == "tasks.category") return nullptr;
        }
        auto it = settings.find(key);
        return it == settings.end() ? nullptr : &it->second;
    }
    static bool task_default(std::string_view key, std::string_view value) {
        return ((key.starts_with("tasks.tracked.") || key.starts_with("tasks.tracker.collapsed.")) && value == "0") ||
               (key.starts_with("tasks.section.") && value == "objectives");
    }
    // The file is one key=value a line: a key with '=' or a line break, or a value with a line break, would be read
    // back as other keys (a plugin's value as the pack's test hooks), so it is refused.
    static bool storable(std::string_view key, std::string_view value) {
        return !key.empty() && key.find_first_of("=\r\n") == std::string_view::npos &&
               value.find_first_of("\r\n") == std::string_view::npos;
    }
    // The plugin tag a key of a plugin's is under ("plugin:X" of "plugin.plugin:X.key"), else "".
    static std::string_view plugin_key_tag(std::string_view key) {
        const std::string_view rest = plugin_key_rest(key);
        if (rest.size() == key.size()) return {};
        return key.substr(7, key.size() - rest.size() - 8); // between "plugin." and the '.' before rest
    }
    // How many keys the plugin `tag` has in either file, counted once and then kept as keys are added and dropped.
    std::size_t& plugin_keys(std::string_view tag) {
        auto it = plugin_settings.find(tag);
        if (it != plugin_settings.end()) return it->second;
        const std::string prefix = plugin_key(tag, "");
        std::size_t n = 0;
        for (const Settings* map : {&settings, &character_settings})
            for (auto k = map->lower_bound(prefix); k != map->end() && k->first.starts_with(prefix); ++k) ++n;
        return plugin_settings.emplace(std::string(tag), n).first->second;
    }
    // Writes `key` into `map`: false for a plugin's new key past its share (kPluginSettings).
    bool store(Settings& map, std::string_view key, std::string_view value) {
        auto it = map.find(key);
        if (it != map.end()) {
            it->second.assign(value);
            return true;
        }
        const std::string_view tag = plugin_key_tag(key);
        if (!tag.empty()) {
            std::size_t& n = plugin_keys(tag);
            if (n >= kPluginSettings) return false;
            ++n;
        }
        map.emplace(std::string(key), std::string(value));
        return true;
    }
    void erase(Settings& map, std::string_view key) {
        auto it = map.find(key);
        if (it == map.end()) return;
        if (const std::string_view tag = plugin_key_tag(key); !tag.empty()) {
            std::size_t& n = plugin_keys(tag);
            if (n > 0) --n;
        }
        map.erase(it);
    }
    bool set_setting(std::string_view key, std::string_view value) {
        if (!storable(key, value)) return false;
        if (!plugin_key_tag(key).empty() &&
            (key.size() > kPluginSettingKey || value.size() > kPluginSettingValue)) return false;
        const std::string* now = get_setting(key);
        if (now && *now == value) return true;
        if (per_character(key)) {
            // A quest's flag at its default (not tracked, expanded, on Objectives) is dropped, not written: these
            // keys never fall back to the shared file, so a missing one reads as the default, and the file does
            // not gain a line for every quest ever opened.
            if (task_default(plugin_key_rest(key), value)) {
                if (character_settings.find(key) != character_settings.end()) {
                    erase(character_settings, key);
                    character_dirty = true;
                }
            } else {
                if (!store(character_settings, key, value)) return false;
                character_dirty = true;
            }
        } else {
            if (!store(settings, key, value)) return false;
            settings_dirty = true;
        }
        settings_changed = Clock::now();
        return true;
    }
    // A plugin's window keeps its own switch (ui.DisableWindow): its key under the plugin's prefix.
    bool saved_enabled(std::string_view name, std::string_view tag = {}) const {
        const std::string key = "enabled." + std::string(name);
        auto it = settings.find(tag.empty() ? key : plugin_key(tag, key));
        return it == settings.end() || it->second != "0";
    }

    // A plugin's settings (ui.GetSetting / SetSetting from its code) are its own: "plugin.<tag>.<key>".
    static std::string plugin_key(std::string_view tag, std::string_view key) {
        return "plugin." + std::string(tag) + "." + std::string(key);
    }
    // The key a plugin's key stands for ("hud.pos.X" of "plugin.<tag>.hud.pos.X"), else the key itself.
    static std::string_view plugin_key_rest(std::string_view key) {
        if (!key.starts_with("plugin.")) return key;
        const std::size_t colon = key.find(':'); // the tag's kind ("plugin:<name>", "script:<name>")
        const std::size_t dot = colon == std::string_view::npos ? std::string_view::npos : key.find('.', colon);
        return dot == std::string_view::npos ? key : key.substr(dot + 1);
    }

    // A folder's code runs: caller_tag names it and its scope holds while this lives. The pack's own code (tag "")
    // runs as whoever runs it (a pack window a plugin's draws, ui.base_window, stays the plugin's). Never alive
    // across a Lua error: Lua is built as C and longjmps past destructors, so each lives in a block around a pcall.
    struct Acting {
        Impl& m;
        std::string before; // (only when a folder's code runs: the pack's windows copy nothing)
        std::shared_ptr<void> held;
        Clock::time_point deadline_before;
        bool acting = false;
        Acting(Impl& im, std::string_view tag) : m(im), deadline_before(im.deadline) {
            if (tag.empty()) return;
            acting = true;
            before = g_caller_tag;
            if (g_caller_tag != tag) m.deadline = std::min(m.deadline, Clock::now() + m.plugin_budget);
            g_caller_tag = tag;
            try { if (m.scope) held = m.scope(tag); }
            catch (...) { g_caller_tag=std::move(before);m.deadline=deadline_before;throw; }
        }
        ~Acting() {
            held.reset();
            if (acting) g_caller_tag = std::move(before);
            m.deadline = deadline_before;
        }
        Acting(const Acting&) = delete;
        Acting& operator=(const Acting&) = delete;
    };

    const fs::path* folder_dir(std::string_view tag) const {
        for (const LuaUi::Folder& f : folders)
            if (f.tag == tag) return &f.dir;
        return nullptr;
    }

    // Pushes the folder's globals table, made at its first use (make_env).
    void push_env(const std::string& tag) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, envs_ref);
        if (lua_getfield(L, -1, tag.c_str()) != LUA_TTABLE) {
            lua_pop(L, 1);
            make_env(tag);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, tag.c_str());
        }
        lua_remove(L, -2);
    }
    void drop_env(const std::string& tag) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, envs_ref);
        lua_pushnil(L);
        lua_setfield(L, -2, tag.c_str());
        lua_pop(L, 1);
    }
    void make_env(const std::string& tag);

    void report(Script& s, const char* message) {
        callback_generations.erase(s.file);
        s.error = message ? message : "(error)";
        maintain_callbacks();
        log::warn("ui script error: {}", s.error);
    }
    // A window's draw function failed: the error goes to its script, or, for a window registered outside its folder's
    // loading (no file), to the window itself, shown and logged as a script's is.
    void report_window(Window& w, const char* message) {
        w.failed = true; // off until its script reloads, it is registered again, or (no file) it is retried
        if (!w.file.empty()) {
            for (Script& s : scripts)
                if (s.file == w.file) {
                    report(s, message);
                    return;
                }
        }
        w.error = message ? message : "(error)";
        log::warn("ui window error ({}{}{}): {}", w.tag, w.tag.empty() ? "" : "/", w.name, w.error);
    }

    void load_file(Script& s) {
        callback_generations[s.file]=std::make_shared<int>(0);
        maintain_callbacks();
        // Drop the windows this file registered before; the reloaded script registers them again.
        for (std::size_t i = 0; i < windows.size();) {
            if (windows[i].file == s.file) {
                luaL_unref(L, LUA_REGISTRYINDEX, windows[i].draw_ref);
                windows.erase(windows.begin() + static_cast<long>(i));
            } else {
                ++i;
            }
        }
        s.error.clear();
        const std::string path = s.path.string();
        std::string file_before = std::exchange(running_file, s.file), tag_before = std::exchange(running_tag, s.tag);
        log::debug("ui: loading {}", path);
        // A plugin's chunk runs with its own globals, made before the limits hold: made (make_env) outside any
        // protected call, an allocation the limit refused there would abort the client.
        if (!s.tag.empty()) push_env(s.tag);
        else lua_pushnil(L);
        int status = LUA_OK;
        {
            Budget budget(*this, budget_for(s.tag));
            Acting acting(*this, s.tag);
            status = luaL_loadfilex(L, path.c_str(), "t"); // text only: a binary chunk can corrupt memory
            if (status == LUA_OK && !s.tag.empty()) {
                lua_pushvalue(L, -2);
                lua_setupvalue(L, -2, 1); // the chunk's _ENV
            }
            if (status == LUA_OK) status = lua_pcall(L, 0, 0, 0);
        }
        if (status != LUA_OK) {
            report(s, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // (the env)
        running_file = std::move(file_before);
        running_tag = std::move(tag_before);
    }

    // A window's layer: 0 the UI's own, then the plugin folders in their order (a later one covers an earlier).
    int layer(const std::string& tag) const {
        if (tag.empty()) return 0;
        for (std::size_t i = 0; i < folders.size(); ++i)
            if (folders[i].tag == tag) return static_cast<int>(i) + 1;
        return -1;
    }
    // Whether another window of the same name covers w.
    bool covered(const Window& w) const {
        const int mine = layer(w.tag);
        return std::any_of(windows.begin(), windows.end(),
                           [&](const Window& o) { return &o != &w && o.name == w.name && layer(o.tag) > mine; });
    }
    // The window of this name just beneath the layer `tag`, or null.
    const Window* beneath(const std::string& name, const std::string& tag) const {
        const int above = layer(tag);
        const Window* best = nullptr;
        for (const Window& o : windows) {
            const int l = layer(o.tag);
            if (o.name == name && l >= 0 && l < above && (!best || l > layer(best->tag))) best = &o;
        }
        return best;
    }

    // Drops a script and the windows it registered.
    void unload_file(const std::string& file) {
        callback_generations.erase(file);
        for (std::size_t i = 0; i < windows.size();) {
            if (windows[i].file == file) {
                luaL_unref(L, LUA_REGISTRYINDEX, windows[i].draw_ref);
                windows.erase(windows.begin() + static_cast<long>(i));
            } else {
                ++i;
            }
        }
        std::erase_if(scripts, [&](const Script& x) { return x.file == file; });
        maintain_callbacks();
    }

    // The UI's folder, then each plugin folder: new and changed scripts load. A folder no longer listed takes its
    // scripts and windows with it.
    void scan(bool force_all) {
        if (folders_changed) {
            folders_changed = false;
            auto listed = [&](const std::string& tag) {
                return tag.empty() || std::any_of(folders.begin(), folders.end(),
                                                  [&](const LuaUi::Folder& f) { return f.tag == tag; });
            };
            std::vector<std::string> gone, gone_tags;
            for (const Script& x : scripts)
                if (!listed(x.tag)) {
                    gone.push_back(x.file);
                    if (std::find(gone_tags.begin(), gone_tags.end(), x.tag) == gone_tags.end())
                        gone_tags.push_back(x.tag);
                }
            for (const auto& f : gone) unload_file(f);
            // (and any window it registered from outside a load)
            for (std::size_t i = 0; i < windows.size();) {
                if (!listed(windows[i].tag)) {
                    luaL_unref(L, LUA_REGISTRYINDEX, windows[i].draw_ref);
                    windows.erase(windows.begin() + static_cast<long>(i));
                } else {
                    ++i;
                }
            }
            for (const auto& t : gone_tags) drop_env(t);
        }
        scan_folder("", dir, force_all);
        for (const LuaUi::Folder& f : folders) scan_folder(f.tag, f.dir, force_all);
        scanned = true;
        maintain_callbacks();
    }

    void scan_folder(const std::string& tag, const fs::path& folder, bool force_all) {
        std::error_code ec;
        std::map<std::string, fs::path, std::less<>> files; // nearest file of each entry name wins
        auto add = [&](const fs::path& source) {
            auto file = [&](const std::string& name) {
                const auto path = inside(source, name);
                if (path && fs::is_regular_file(*path, ec))
                    files.try_emplace(tag.empty() ? name : tag + "/" + name, *path);
            };
            if (tag.empty() && !entries.empty()) {
                for (const std::string& entry : entries) file(entry);
            } else {
                for (const auto& entry : fs::directory_iterator(source, ec))
                    if (entry.is_regular_file(ec) && entry.path().extension() == ".lua")
                        file(entry.path().filename().string());
            }
        };
        add(folder);
        if (tag.empty())
            for (const auto& base : bases) add(base);
        std::vector<std::string> gone;
        for (const auto& script : scripts)
            if (script.tag == tag && !files.contains(script.file)) gone.push_back(script.file);
        for (const auto& name : gone) unload_file(name);
        for (const auto& [name, p] : files) {
            Script* s = nullptr;
            for (Script& x : scripts)
                if (x.file == name) s = &x;
            bool force = force_all;
            if (!s) {
                Script n;
                n.file = name;
                n.path = p;
                n.tag = tag;
                scripts.push_back(std::move(n));
                s = &scripts.back();
                force = true;
            } else if (s->path != p) {
                s->path = p; // an override appeared or was removed, even when both files have the same timestamp
                force = true;
            }
            auto written = fs::last_write_time(p, ec);
            if (force || written != s->written) {
                s->written = written;
                load_file(*s);
            }
        }
    }

    void draw_errors() {
        bool any = false;
        for (const Script& s : scripts) any |= !s.error.empty();
        for (const Window& w : windows) any |= !w.error.empty();
        if (!any) return;
        ImGui::SetNextWindowSize(ImVec2(520, 160), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("UI errors")) {
            for (Script& s : scripts) {
                if (s.error.empty()) continue;
                ImGui::PushID(s.file.c_str());
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "%s", s.file.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("reload")) load_file(s);
                ImGui::TextWrapped("%s", s.error.c_str());
                ImGui::Separator();
                ImGui::PopID();
            }
            for (Window& w : windows) {
                if (w.error.empty()) continue;
                ImGui::PushID(&w);
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "%s%s%s (window)", w.tag.c_str(),
                                   w.tag.empty() ? "" : "/", w.name.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("retry")) {
                    w.error.clear();
                    w.failed = false;
                }
                ImGui::TextWrapped("%s", w.error.c_str());
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        ImGui::End();
    }

    // The control window: windows on/off, reload, the ImGui demo (a reference for script authors).
    void draw_control() {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Lua UI")) {
            if (ImGui::Button("Reload all scripts")) scan(true);
            ImGui::SameLine();
            ImGui::Checkbox("ImGui demo", &show_demo);
            ImGui::Separator();
            for (Window& w : windows) {
                ImGui::PushID(&w);
                if (ImGui::Checkbox(w.name.c_str(), &w.enabled)) {
                    set_setting("enabled." + w.name, w.enabled ? "1" : "0");
                    for (Window& o : windows)
                        if (o.name == w.name) o.enabled = w.enabled;
                }
                ImGui::SameLine();
                ImGui::TextDisabled(covered(w) ? "(%s, covered)" : "(%s)", w.file.c_str());
                ImGui::PopID();
            }
            if (scripts.empty()) ImGui::TextWrapped("No scripts in %s", dir.string().c_str());
        }
        ImGui::End();
        if (show_demo) ImGui::ShowDemoWindow(&show_demo);
    }
};

namespace {

// Keys a plugin's code may not read or write as given: the pack's test hooks (test.click.<window> clicks a window's
// children, test.show opens windows), other owners' settings, and the windows' switches.
bool plugin_reserved(std::string_view key) {
    return key.starts_with("test.") || key.starts_with("plugin.") || key.starts_with("enabled.");
}

// value = ui.GetSetting(key [, default])   -- a saved string, or default. A plugin's code reads its own key
// ("plugin.<tag>.<key>"), else the pack's of that name (the palette, a window's place it took over), never a
// reserved one (plugin_reserved).
int lua_get_setting(lua_State* L) {
    std::size_t len = 0;
    const char* key_text = luaL_checklstring(L, 1, &len);
    const std::string_view key(key_text, len);
    auto& ui = LuaUi::Impl::of(L);
    const std::string* value = nullptr;
    if (g_caller_tag.empty()) {
        value = ui.get_setting(key);
    } else {
        {
            const std::string own = LuaUi::Impl::plugin_key(g_caller_tag, key); // (gone before anything can raise)
            value = ui.get_setting(own);
        }
        if (!value && !plugin_reserved(key)) value = ui.get_setting(key);
    }
    if (!value) {
        lua_settop(L, 2);
        return 1;
    }
    lua_pushlstring(L, value->data(), value->size());
    return 1;
}

// saved = ui.SetSetting(key, value)   -- saved (numbers and booleans as text). A plugin's code writes its own key
// ("plugin.<tag>.<key>"; never test.*, which the pack's tests act on). Refused (false) for a key with '=' or a line
// break, or a value with a line break: the file is one key=value a line.
int lua_set_setting(lua_State* L) {
    std::size_t key_len = 0;
    const char* key = luaL_checklstring(L, 1, &key_len);
    std::size_t value_len = 1;
    const char* value = lua_isboolean(L, 2) ? (lua_toboolean(L, 2) ? "1" : "0") : luaL_tolstring(L, 2, &value_len);
    auto& ui = LuaUi::Impl::of(L);
    const std::string_view k(key, key_len), v(value, value_len);
    // (A NUL would cut the text short: refused as the line breaks are.)
    const bool nul = k.find('\0') != std::string_view::npos || v.find('\0') != std::string_view::npos;
    bool saved = false;
    if (!nul && g_caller_tag.empty()) saved = ui.set_setting(k, v);
    else if (!nul && !k.starts_with("test.")) saved = ui.set_setting(LuaUi::Impl::plugin_key(g_caller_tag, k), v);
    lua_pushboolean(L, saved);
    return 1;
}

// ui.Log(text)   -- a line in the client's log (diagnostics from scripts)
int lua_log(lua_State* L) {
    log::info("ui script: {}", luaL_checkstring(L, 1));
    return 0;
}

// ui.DisableWindow(name)   -- turns a script window off (saved); the "Lua UI" window turns it back on. A plugin's
// code turns off its own window of that name only, and keeps the switch under its own prefix.
int lua_disable_window(lua_State* L) {
    auto& ui = LuaUi::Impl::of(L);
    std::string name = luaL_checkstring(L, 1);
    const std::string& tag = g_caller_tag;
    bool mine = false;
    for (Window& w : ui.windows)
        if (w.name == name && w.tag == tag) {
            w.enabled = false;
            mine = true;
        }
    if (tag.empty()) {
        for (Window& w : ui.windows) // (the name's every layer, as the "Lua UI" window switches it)
            if (w.name == name) w.enabled = false;
        ui.set_setting("enabled." + name, "0");
    } else if (mine) {
        ui.set_setting(LuaUi::Impl::plugin_key(tag, "enabled." + name), "0");
    }
    return 0;
}

// ui.window(name, draw): registers (or replaces) a window drawn by calling draw() each frame. A plugin's window
// covers the UI's of the same name (Window). The window is the caller's (LuaUi::caller_tag).
int lua_window(lua_State* L) {
    auto& ui = LuaUi::Impl::of(L);
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    const std::string& tag = g_caller_tag;
    if (!tag.empty() && !ui.folder_dir(tag)) { // a plugin's code after its folder went: nothing to draw it in
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return 0;
    }
    std::size_t mine = 0;
    for (Window& w : ui.windows) {
        if (w.name == name && w.tag == tag) {
            luaL_unref(L, LUA_REGISTRYINDEX, w.draw_ref);
            w.draw_ref = ref;
            w.file = ui.running_tag == tag ? ui.running_file : std::string();
            w.failed = false; // (a new draw function: it may draw again)
            w.error.clear();
            return 0;
        }
        if (w.tag == tag) ++mine;
    }
    if (!tag.empty() && mine >= kPluginWindows) { // (each window is kept in C++: a plugin's are held to a share)
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "ui.window: a plugin may have %d windows", static_cast<int>(kPluginWindows));
    }
    Window w;
    w.name = name;
    w.file = ui.running_tag == tag ? ui.running_file : std::string();
    w.tag = tag;
    w.draw_ref = ref;
    w.enabled = ui.saved_enabled(name) && ui.saved_enabled(name, tag);
    ui.windows.push_back(std::move(w));
    return 0;
}

// drawn = ui.base_window(): from a window's draw function, draws the window it covers (the UI's own of the same
// name, or an earlier plugin's); false when it covers none. A plugin's window uses it to leave the stock one in
// place while the plugin has nothing to add (another server). The window beneath runs as the plugin (Acting).
int lua_base_window(lua_State* L) {
    auto& ui = LuaUi::Impl::of(L);
    if (g_drawing.empty()) return luaL_error(L, "ui.base_window: only from a window's draw function");
    const Window* base = ui.beneath(g_drawing, ui.drawing_tag);
    if (!base) {
        lua_pushboolean(L, 0);
        return 1;
    }
    int status = LUA_OK;
    {
        std::string tag = std::exchange(ui.drawing_tag, base->tag);
        std::string file = std::exchange(ui.running_file, base->file);
        std::string running = std::exchange(ui.running_tag, base->tag);
        lua_rawgeti(L, LUA_REGISTRYINDEX, base->draw_ref);
        {
            LuaUi::Impl::Acting acting(ui, base->tag); // (base may move while it draws)
            status = lua_pcall(L, 0, 0, 0);
        }
        ui.drawing_tag = std::move(tag);
        ui.running_file = std::move(file);
        ui.running_tag = std::move(running);
    }
    if (status != LUA_OK) return lua_error(L);
    lua_pushboolean(L, 1);
    return 1;
}

// The file `given` names in `root`: nullopt when it is absolute or leads out of root ("..", a link).
std::optional<fs::path> inside(const fs::path& root, std::string_view given) {
    const fs::path rel(given);
    if (rel.empty() || rel.has_root_name() || rel.has_root_directory()) return std::nullopt;
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(root, ec);
    if (ec) return std::nullopt;
    fs::path p = fs::weakly_canonical(base / rel, ec);
    if (ec) return std::nullopt;
    const fs::path r = p.lexically_relative(base);
    if (r.empty() || *r.begin() == ".." || *r.begin() == ".") return std::nullopt;
    return p;
}

// dofile(path): a path relative to the UI's folder ("lib/sidl.lua"), or in a plugin's dofile (upvalue 1: its tag)
// to the plugin's folder, falling back to the UI's own when the plugin's lacks the file (its "lib/hud.lua"), so a
// plugin's windows can look like the pack's; a plugin's file runs with the plugin's globals. Nothing outside those
// folders, and text only.
int lua_dofile(lua_State* L) {
    const char* given = luaL_checkstring(L, 1);
    lua_settop(L, 1);
    auto& ui = LuaUi::Impl::of(L);
    const char* tag = lua_tostring(L, lua_upvalueindex(1)); // null: the pack's own dofile
    bool found = false;
    {
        const fs::path* folder = tag ? ui.folder_dir(tag) : nullptr;
        std::optional<fs::path> p = folder ? inside(*folder, given) : std::nullopt;
        std::error_code ec;
        if (!p || !fs::is_regular_file(*p, ec)) {
            p = inside(ui.root, given);
            if (!p || !fs::is_regular_file(*p, ec)) {
                p.reset();
                for (const auto& base : ui.bases) {
                    const auto candidate = inside(base, given);
                    if (candidate && fs::is_regular_file(*candidate, ec)) { p = candidate; break; }
                }
            }
        }
        if (p) {
            const std::string text = p->string();
            lua_pushlstring(L, text.data(), text.size());
            found = true;
        }
    }
    if (!found) return luaL_error(L, "dofile: '%s' is not in the UI's folder", given);
    if (luaL_loadfilex(L, lua_tostring(L, 2), "t") != LUA_OK) return lua_error(L);
    if (tag) {
        ui.push_env(tag);
        lua_setupvalue(L, -2, 1); // its _ENV: the plugin's globals
    }
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - 2;
}

struct FileImport {
    fs::path file;
    std::string path, tag, error;
};

bool beneath_folder(const fs::path& root, const fs::path& file) {
    std::error_code ec;
    const auto base = fs::weakly_canonical(root, ec);
    if (ec) return false;
    const auto relative = file.lexically_relative(base);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

// This protected frame includes loading and environment setup, so every error returns through the recursion guard.
int call_import_file(lua_State* L) {
    const auto& load = *static_cast<FileImport*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (luaL_loadfilex(L, load.path.c_str(), "t") != LUA_OK) return lua_error(L);
    if (!load.tag.empty()) {
        LuaUi::Impl::of(L).push_env(load.tag);
        lua_setupvalue(L, -2, 1);
    }
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L);
}

enum class ImportKind { Base, Data };

int lua_import_file(lua_State* L, ImportKind kind) {
    std::size_t length = 0;
    const char* given = luaL_checklstring(L, 1, &length);
    const bool data = kind == ImportKind::Data;
    const bool base_only = data && lua_toboolean(L, 2);
    const char* method = data ? "LoadData" : "LoadBase";
    const bool managed = lua_getfield(L, LUA_REGISTRYINDEX, kPackStateKey) == LUA_TBOOLEAN && lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (!managed) return luaL_error(L, "%s requires the client UI state", method);
    lua_settop(L, 1);
    auto& ui = LuaUi::Impl::of(L);
    auto& load = plugin::lua_keep<FileImport>(L);
    plugin::cxx_guard(L, [&] {
        load.tag = g_caller_tag;
        if (!load.tag.empty() && !ui.folder_dir(load.tag)) {
            load.error = "the caller's UI folder is no longer active";
            return;
        }
        const std::string_view raw(given, length);
        if (raw.empty() || raw.size() > 4096 || raw.find(':') != std::string_view::npos ||
            std::any_of(raw.begin(), raw.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
            load.error = "expected a relative Lua file";
            return;
        }
        std::string relative(raw);
        std::replace(relative.begin(), relative.end(), '\\', '/');
        const fs::path rel(relative);
        if (rel.has_root_path() || rel.extension() != ".lua" ||
            std::any_of(rel.begin(), rel.end(), [](const fs::path& part) { return part == "." || part == ".."; })) {
            load.error = "expected a relative Lua file without traversal";
            return;
        }
        std::vector<fs::path> roots;
        std::optional<fs::path> selected_data;
        if (data) {
            const auto& approved = ui.presentation_data;
            if (!approved.root.empty()) {
                std::error_code anchor_error;
                const auto anchor = fs::weakly_canonical(approved.root, anchor_error);
                if (anchor_error) {
                    load.error = "invalid approved presentation-data root";
                    return;
                }
                for (std::size_t i = 0; i < approved.ids.size(); ++i) {
                    const auto& id = approved.ids[i];
                    if (id.empty() || id == "." || id == ".." || id.find_first_of("/\\:") != std::string::npos ||
                        std::any_of(id.begin(), id.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
                        load.error = "invalid approved presentation-data ID";
                        return;
                    }
                    const auto folder = inside(anchor, id);
                    if (!folder) {
                        load.error = "presentation-data folder escapes its approved root";
                        return;
                    }
                    // Each approved stable ID owns its directory, even when another ID is inside the anchor.
                    if (!same_folder(*folder, anchor / id)) {
                        load.error = "presentation-data folder redirects its approved ID";
                        return;
                    }
                    if (i == 0) selected_data = *folder;
                    if (!base_only || i != 0) roots.push_back(*folder);
                }
            }
        } else {
            // A base helper can decorate its parent while still importing a selected pack's different helper.
            std::size_t first = 0;
            lua_Debug frame{};
            for (int level = 1; lua_getstack(L, level, &frame); ++level) {
                if (!lua_getinfo(L, "S", &frame) || !frame.what || std::string_view(frame.what) == "C") continue;
                if (frame.source && frame.source[0] == '@') {
                    std::error_code ec;
                    const auto caller = fs::weakly_canonical(fs::path(frame.source + 1), ec);
                    if (!ec && !beneath_folder(ui.root, caller))
                        for (std::size_t i = 0; i < ui.bases.size(); ++i)
                            if (beneath_folder(ui.bases[i], caller)) { first = i + 1; break; }
                }
                break;
            }
            roots.assign(ui.bases.begin() + static_cast<std::ptrdiff_t>(first), ui.bases.end());
        }
        std::error_code ec;
        for (const auto& root : roots) {
            const auto candidate = inside(root, relative);
            if (!candidate || !fs::is_regular_file(*candidate, ec)) continue;
            if ((!data && beneath_folder(ui.root, *candidate)) ||
                (data && base_only && selected_data && beneath_folder(*selected_data, *candidate))) {
                load.error = "a base-only import cannot load the selected UI's own files";
                return;
            }
            for (const auto& active : ui.active_import_files)
                if (fs::equivalent(active, *candidate, ec)) {
                    load.error = data ? "recursive presentation-data load" : "recursive base-file load";
                    return;
                }
            load.file = *candidate;
            load.path = candidate->string();
            return;
        }
        load.error = data ? "no approved presentation-data root supplies that Lua file" : "no approved base supplies that Lua file";
    });
    if (!load.error.empty()) return luaL_error(L, "%s: %s", method, load.error.c_str());
    lua_pushvalue(L, 2); // keep the request alive as the protected loader's upvalue
    lua_pushcclosure(L, &call_import_file, 1);
    plugin::cxx_guard(L, [&] { ui.active_import_files.push_back(load.file); });
    const int status = lua_pcall(L, 0, LUA_MULTRET, 0);
    ui.active_import_files.pop_back();
    if (status != LUA_OK) return lua_error(L);
    return lua_gettop(L) - 2;
}

// load(chunk [, name [, mode [, env]]]): Lua's, text only; in a plugin's (upvalue 2: its globals) env defaults to
// the plugin's globals, not the pack's.
int lua_load_text(lua_State* L) {
    int n = lua_gettop(L);
    if (n < 3) {
        lua_settop(L, 3);
        n = 3;
    }
    lua_pushliteral(L, "t");
    lua_replace(L, 3);
    if (n < 4 && !lua_isnil(L, lua_upvalueindex(2))) {
        lua_pushvalue(L, lua_upvalueindex(2));
        n = 4;
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    return lua_gettop(L);
}

int read_only_write(lua_State* L) { return luaL_error(L, "this table is the UI's: it cannot be changed here"); }

// collectgarbage for a plugin's code: "count" and "step" only (a step of the usual size). The collector's pacing is
// the host's (LuaUi::Impl::collect_garbage): stopping it, a full cycle every frame or other parameters are not a
// plugin's to set.
int plugin_collectgarbage(lua_State* L) {
    const std::string_view opt = luaL_optstring(L, 1, "collect");
    if (opt == "count") {
        const int kb = lua_gc(L, LUA_GCCOUNT), bytes = lua_gc(L, LUA_GCCOUNTB);
        lua_pushnumber(L, static_cast<lua_Number>(kb) + static_cast<lua_Number>(bytes) / 1024.0);
        return 1;
    }
    if (opt == "step") {
        lua_pushboolean(L, lua_gc(L, LUA_GCSTEP, 0));
        return 1;
    }
    return luaL_error(L, "collectgarbage('%s'): a plugin may ask only 'count' and 'step'", opt.data());
}

// A table that reads through to `real` (an index) and refuses writes; its metatable is hidden.
void push_read_only(lua_State* L, int real) {
    real = lua_absindex(L, real);
    lua_newtable(L);
    lua_createtable(L, 0, 3);
    lua_pushvalue(L, real);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, &read_only_write);
    lua_setfield(L, -2, "__newindex");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
}

void push_in(lua_State* L, int v, int S, int tag);

// A function a plugin took from hs or ui, or one it left in a table the pack shares (shared-table views, below):
// runs the real one (upvalue 1) as the plugin (upvalue 2, its tag), whoever calls it. Only the pack's code or the
// plugin's own may call it: another plugin would act with this one's capabilities. Once the plugin's folder is gone
// (it unloaded) it does nothing. A plugin's own Lua function (upvalue 3: its folder's view state, below) is handed
// its arguments as the plugin sees the pack's values (push_in: a table as a view), whoever passes them: a pack
// library a plugin calls runs as the plugin and passes its own tables to the plugin's callbacks.
int call_as(lua_State* L) {
    auto& ui = LuaUi::Impl::of(L);
    const char* tag = lua_tostring(L, lua_upvalueindex(2));
    if (!tag || !ui.folder_dir(tag)) return 0;
    const auto* source=lua_type(L,lua_upvalueindex(4))==LUA_TUSERDATA?static_cast<LuaUi::CallbackOwner*>(lua_touserdata(L,lua_upvalueindex(4))):nullptr;
    if(source&&(!source->valid||!source->valid()))return luaL_error(L,"this callback registration belongs to a retired UI source");
    const bool same = g_caller_tag == tag;
    if (!same && !g_caller_tag.empty())
        return luaL_error(L, "this function is %s's: %s's code cannot call it", tag, g_caller_tag.c_str());
    const int n = lua_gettop(L);
    if (lua_type(L, lua_upvalueindex(3)) == LUA_TTABLE && !lua_iscfunction(L, lua_upvalueindex(1))) {
        luaL_checkstack(L, 8, nullptr);
        for (int i = 1; i <= n; ++i) {
            push_in(L, i, lua_upvalueindex(3), lua_upvalueindex(2));
            lua_replace(L, i);
        }
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    if (same&&!source) { // already the plugin: as it is
        lua_call(L, n, LUA_MULTRET);
        return lua_gettop(L);
    }
    int status = LUA_OK;
    {
        struct SourceScope { const LuaUi::CallbackOwner* before=g_callback_source; ~SourceScope(){g_callback_source=before;} } source_scope;
        if(source)g_callback_source=source;
        LuaUi::Impl::Acting acting(ui, tag);
        status = lua_pcall(L, n, LUA_MULTRET, 0);
    }
    if (status != LUA_OK) return lua_error(L);
    return lua_gettop(L);
}

// ---- The tables the pack shares, seen from a plugin's code (make_env). A plugin reads the pack's globals; a table
// among them (GPSTATE, modern_colors, ui_palette, ITEM_TOOLS ...) comes as a view: reads go through to the table
// (a table in it comes as a view too), writes land in it, but a function the plugin writes is wrapped to run as the
// plugin (call_as) and a table of its own is put in as an owned view, whose functions run as the plugin whoever
// reads them. So nothing the plugin leaves there runs with the player's authority when the pack calls it, and
// nothing of it works once the plugin unloads. A view's metatable cannot be read or changed, and rawset cannot get
// past it. A pack function the plugin gets (a global, a table's, ui's) comes wrapped (call_pack): what it returns
// comes as the plugin sees the pack's values, so a pack table it returns is a view too; and what the pack passes the
// plugin's functions comes the same way (call_as). getmetatable gives the plugin a metatable it set itself as it is,
// any other as a view. What is still the plugin's to reach: a value it hands to a pack function as an argument (or
// returns to the pack from a callback), which the pack keeps as it is (docs/todo.md).
//
// Each folder's state S (an upvalue of the closures below): [kPackReal] pack view -> its table, [kOwnReal] owned view
// -> the plugin's table, [kPackOf] / [kOwnOf] the other way, [kWrapped] function -> its call_as, [kPackFn] pack
// function -> its call_pack, [kMetas] the metatables the plugin set (plugin_setmetatable), [kPackMeta] / [kOwnMeta]
// the views' metatables. All but the last two weak-keyed.
enum ViewSlot : int { kPackReal = 1, kOwnReal, kPackOf, kOwnOf, kWrapped, kPackFn, kMetas, kPackMeta, kOwnMeta };
enum ViewKind : int { kPackView = 0, kOwnView = 1 };

bool is_view(lua_State* L, int v) {
    if (lua_type(L, v) != LUA_TTABLE) return false;
    v = lua_absindex(L, v);
    lua_rawgeti(L, LUA_REGISTRYINDEX, LuaUi::Impl::of(L).views_ref);
    lua_pushvalue(L, v);
    const bool yes = lua_rawget(L, -2) != LUA_TNIL;
    lua_pop(L, 2);
    return yes;
}

// Pushes S[slot][key at k] (raw).
int s_get(lua_State* L, int S, int slot, int k) {
    k = lua_absindex(L, k);
    lua_rawgeti(L, S, slot);
    lua_pushvalue(L, k);
    const int t = lua_rawget(L, -2);
    lua_remove(L, -2);
    return t;
}

// Pushes call_as(the function at fn, tag), one per function.
void push_wrapped(lua_State* L, int fn, int S, int tag) {
    fn = lua_absindex(L, fn);
    if (s_get(L, S, kWrapped, fn) == LUA_TFUNCTION) return;
    lua_pop(L, 1);
    lua_pushvalue(L, fn);
    lua_pushvalue(L, tag);
    lua_pushvalue(L, S);
    lua_pushcclosure(L, &call_as, 3);
    lua_rawgeti(L, S, kWrapped);
    lua_pushvalue(L, fn);
    lua_pushvalue(L, -3);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

// Pushes the view of the table at `real`, one per table and kind.
void push_view(lua_State* L, int real, int S, int kind) {
    real = lua_absindex(L, real);
    const int of = kind == kOwnView ? kOwnOf : kPackOf;
    if (s_get(L, S, of, real) == LUA_TTABLE) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_rawgeti(L, S, kind == kOwnView ? kOwnMeta : kPackMeta);
    lua_setmetatable(L, -2);
    lua_rawgeti(L, S, of);
    lua_pushvalue(L, real);
    lua_pushvalue(L, -3);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    lua_rawgeti(L, S, kind == kOwnView ? kOwnReal : kPackReal);
    lua_pushvalue(L, -2);
    lua_pushvalue(L, real);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, LuaUi::Impl::of(L).views_ref);
    lua_pushvalue(L, -2);
    lua_pushvalue(L, real);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

int call_pack(lua_State* L);

// Pushes call_pack(the pack's Lua function at fn), one per function.
void push_pack_fn(lua_State* L, int fn, int S, int tag) {
    fn = lua_absindex(L, fn);
    if (s_get(L, S, kPackFn, fn) == LUA_TFUNCTION) return;
    lua_pop(L, 1);
    lua_pushvalue(L, fn);
    lua_pushvalue(L, tag);
    lua_pushvalue(L, S);
    lua_pushcclosure(L, &call_pack, 3);
    lua_rawgeti(L, S, kPackFn);
    lua_pushvalue(L, fn);
    lua_pushvalue(L, -3);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

// A value from the pack's side (read through a pack view or from the pack's globals, returned by a pack function,
// passed to the plugin's function), pushed as the plugin gets it: a table as a pack view (a view as it is, an owned
// view of the plugin's as its own table), the pack's Lua function wrapped (call_pack), a function of the plugin's
// that the pack held (its call_as) as the plugin's own. C functions are the client's and come as they are.
void push_in(lua_State* L, int v, int S, int tag) {
    v = lua_absindex(L, v);
    switch (lua_type(L, v)) {
    case LUA_TTABLE:
        if (s_get(L, S, kOwnReal, v) == LUA_TTABLE) return;
        lua_pop(L, 1);
        if (is_view(L, v)) lua_pushvalue(L, v);
        else push_view(L, v, S, kPackView);
        return;
    case LUA_TFUNCTION:
        if (!lua_iscfunction(L, v)) {
            push_pack_fn(L, v, S, tag);
            return;
        }
        if (lua_tocfunction(L, v) == &call_as && lua_getupvalue(L, v, 2)) {
            const bool own = lua_rawequal(L, -1, tag) != 0;
            lua_pop(L, 1);
            if (own && lua_getupvalue(L, v, 1)) return;
        }
        lua_pushvalue(L, v);
        return;
    default:
        lua_pushvalue(L, v);
        return;
    }
}

// A value read through a view of `kind`, pushed as the plugin gets it: through a pack view as push_in gives it,
// through an owned view a table as an owned view (a view as it is) and a function as the plugin's (call_as).
void push_out(lua_State* L, int v, int S, int tag, int kind) {
    if (kind == kPackView) {
        push_in(L, v, S, tag);
        return;
    }
    v = lua_absindex(L, v);
    if (lua_type(L, v) == LUA_TTABLE && !is_view(L, v)) push_view(L, v, S, kind);
    else if (lua_type(L, v) == LUA_TFUNCTION) push_wrapped(L, v, S, tag);
    else lua_pushvalue(L, v);
}

// A value the plugin writes into a shared table, pushed as it is stored: a function wrapped as the plugin's (a pack
// function it was given, call_pack, as the pack's own), a table of its own as an owned view, a pack view as its table.
void push_export(lua_State* L, int v, int S, int tag) {
    v = lua_absindex(L, v);
    if (lua_type(L, v) == LUA_TFUNCTION) {
        if (lua_tocfunction(L, v) == &call_pack && lua_getupvalue(L, v, 1)) return;
        push_wrapped(L, v, S, tag);
    } else if (lua_type(L, v) == LUA_TTABLE) {
        if (s_get(L, S, kPackReal, v) == LUA_TTABLE) return;
        lua_pop(L, 1);
        if (is_view(L, v)) lua_pushvalue(L, v);
        else push_view(L, v, S, kOwnView);
    } else {
        lua_pushvalue(L, v);
    }
}

// The table behind the view at `view` (upvalue 3: its kind).
void push_real(lua_State* L, int view, int S) {
    const int kind = static_cast<int>(lua_tointeger(L, lua_upvalueindex(3)));
    if (s_get(L, S, kind == kOwnView ? kOwnReal : kPackReal, view) != LUA_TTABLE)
        luaL_error(L, "a shared table's view without its table");
}

int do_get(lua_State* L) { // (t, k): t[k]
    lua_gettable(L, 1);
    return 1;
}
int do_set(lua_State* L) { // (t, k, v)
    lua_settable(L, 1);
    return 0;
}
int do_len(lua_State* L) { // (t): #t
    lua_pushinteger(L, luaL_len(L, 1));
    return 1;
}

// Runs the C function `fn` on the n values on the top as the owned view's plugin (its table's metamethods are the
// plugin's code), unless that plugin runs already; leaves `results` values.
void as_owner(lua_State* L, lua_CFunction fn, int n, int results) {
    const char* tag = lua_tostring(L, lua_upvalueindex(2));
    lua_pushcfunction(L, fn);
    lua_insert(L, -n - 1);
    if (g_caller_tag == tag || !LuaUi::Impl::of(L).folder_dir(tag ? tag : "")) {
        lua_call(L, n, results);
        return;
    }
    int status = LUA_OK;
    {
        LuaUi::Impl::Acting acting(LuaUi::Impl::of(L), tag ? tag : "");
        status = lua_pcall(L, n, results, 0);
    }
    if (status != LUA_OK) lua_error(L);
}

// Whether the table at t has a metatable (its metamethods are code).
bool has_meta(lua_State* L, int t) {
    if (!lua_getmetatable(L, t)) return false;
    lua_pop(L, 1);
    return true;
}

// The views' metamethods: upvalue 1 S, 2 the tag, 3 the kind.
int view_index(lua_State* L) {
    const int S = lua_upvalueindex(1);
    push_real(L, 1, S); // 3
    lua_pushvalue(L, 2);
    if (lua_tointeger(L, lua_upvalueindex(3)) != kOwnView) lua_gettable(L, 3);
    else if (!has_meta(L, 3)) lua_rawget(L, 3);
    else as_owner(L, &do_get, 2, 1);
    push_out(L, -1, S, lua_upvalueindex(2), static_cast<int>(lua_tointeger(L, lua_upvalueindex(3))));
    return 1;
}

int view_newindex(lua_State* L) {
    const int S = lua_upvalueindex(1);
    const char* tag = lua_tostring(L, lua_upvalueindex(2));
    const bool own = lua_tointeger(L, lua_upvalueindex(3)) == kOwnView;
    // Another plugin's code (it found this view where the plugin left it) would write as this one.
    if (!g_caller_tag.empty() && g_caller_tag != (tag ? tag : ""))
        return luaL_error(L, "this table is %s's: %s's code cannot change it", tag, g_caller_tag.c_str());
    push_real(L, 1, S); // 4
    lua_pushvalue(L, 2);
    if (own && g_caller_tag.empty()) lua_pushvalue(L, 3); // the pack's code writes into the plugin's table as it is
    else push_export(L, 3, S, lua_upvalueindex(2));
    if (!own) lua_settable(L, 4);
    else if (!has_meta(L, 4)) lua_rawset(L, 4);
    else as_owner(L, &do_set, 3, 0);
    return 0;
}

int view_len(lua_State* L) {
    push_real(L, 1, lua_upvalueindex(1));
    if (lua_tointeger(L, lua_upvalueindex(3)) == kOwnView && has_meta(L, -1)) {
        as_owner(L, &do_len, 1, 1);
        return 1;
    }
    lua_pushinteger(L, luaL_len(L, -1));
    return 1;
}

// for k, v in pairs(view): the table's keys, the values as push_out gives them.
int view_next(lua_State* L) {
    lua_settop(L, 2);
    push_real(L, 1, lua_upvalueindex(1)); // 3
    lua_pushvalue(L, 2);
    if (!lua_next(L, 3)) return 0;
    push_out(L, -1, lua_upvalueindex(1), lua_upvalueindex(2), static_cast<int>(lua_tointeger(L, lua_upvalueindex(3))));
    lua_remove(L, -2);
    return 2;
}
int view_pairs(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_pushvalue(L, lua_upvalueindex(3));
    lua_pushcclosure(L, &view_next, 3);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

// A pack's Lua function as a plugin gets it (push_in): upvalue 1 the function, 2 the plugin's tag, 3 its S. Called
// by the plugin's code it runs as ever (as the plugin), but what it returns, or raises, comes as push_in gives it.
// Called by anyone else (the pack, which got it back) it is the pack's function as it is.
int call_pack(lua_State* L) {
    const int n = lua_gettop(L);
    const char* tag = lua_tostring(L, lua_upvalueindex(2));
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    if (!tag || g_caller_tag != tag) {
        lua_call(L, n, LUA_MULTRET);
        return lua_gettop(L);
    }
    const int status = lua_pcall(L, n, LUA_MULTRET, 0);
    const int results = lua_gettop(L);
    luaL_checkstack(L, 8, nullptr);
    for (int i = 1; i <= results; ++i) {
        push_in(L, i, lua_upvalueindex(3), lua_upvalueindex(2));
        lua_replace(L, i);
    }
    if (status != LUA_OK) return lua_error(L);
    return results;
}

// The env's fallback to the pack's globals, and the plugin's ui to the pack's ui (upvalue 1 that table, 2 S, 3 the
// tag): the value as push_in gives it.
int pack_global(lua_State* L) {
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1)); // 3
    push_in(L, 3, lua_upvalueindex(2), lua_upvalueindex(3));
    return 1;
}

// setmetatable and rawset for a plugin: never on a view, and no __gc (a finalizer runs whenever the collector
// likes, as whoever runs then). Upvalue 1: Lua's own; setmetatable's 2: S, where the metatables it sets are noted
// (plugin_getmetatable).
int plugin_setmetatable(lua_State* L) {
    if (is_view(L, 1)) return luaL_error(L, "this table is the UI's: its metatable cannot be changed");
    if (lua_type(L, 2) == LUA_TTABLE) {
        lua_pushliteral(L, "__gc");
        const bool gc = lua_rawget(L, 2) != LUA_TNIL;
        lua_pop(L, 1);
        if (gc) return luaL_error(L, "setmetatable: a plugin's tables cannot have __gc");
        lua_rawgeti(L, lua_upvalueindex(2), kMetas);
        lua_pushvalue(L, 2);
        lua_pushboolean(L, 1);
        lua_rawset(L, -3);
        lua_pop(L, 1);
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}
// getmetatable for a plugin (upvalue 1 Lua's own, 2 S, 3 the tag): a metatable it set itself as it is, any other (a
// pack table's or a userdata's) as a pack view, so the plugin cannot plant a metamethod the pack's code would run.
int plugin_getmetatable(lua_State* L) {
    luaL_checkany(L, 1);
    lua_settop(L, 1);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, 1, 1);
    if (lua_type(L, 1) != LUA_TTABLE) return 1;
    if (s_get(L, lua_upvalueindex(2), kMetas, 1) != LUA_TNIL) {
        lua_pop(L, 1);
        return 1;
    }
    lua_pop(L, 1);
    push_in(L, 1, lua_upvalueindex(2), lua_upvalueindex(3));
    return 1;
}
int plugin_rawset(lua_State* L) {
    if (is_view(L, 1)) return luaL_error(L, "this table is the UI's: rawset cannot be used on it");
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

// Makes a folder's S with its two metatables (make_env), and leaves it on the stack.
void push_view_state(lua_State* L, int tag) {
    tag = lua_absindex(L, tag);
    lua_createtable(L, kOwnMeta, 0);
    const int S = lua_gettop(L);
    for (int slot = kPackReal; slot <= kMetas; ++slot) {
        lua_newtable(L);
        lua_createtable(L, 0, 1);
        lua_pushliteral(L, "k");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        lua_rawseti(L, S, slot);
    }
    for (int kind : {kPackView, kOwnView}) {
        lua_createtable(L, 0, 5);
        for (auto [name, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
                 {"__index", &view_index}, {"__newindex", &view_newindex}, {"__len", &view_len}, {"__pairs", &view_pairs}}) {
            lua_pushvalue(L, S);
            lua_pushvalue(L, tag);
            lua_pushinteger(L, kind);
            lua_pushcclosure(L, fn, 3);
            lua_setfield(L, -2, name);
        }
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "__metatable");
        lua_rawseti(L, S, kind == kOwnView ? kOwnMeta : kPackMeta);
    }
}

void push_wrapping(lua_State* L, int real, int cache, int tag);

// The __index of a plugin's view of hs (and its tables): upvalues the real table, the cache (real value -> what the
// plugin gets), the tag. A function comes wrapped to run as the plugin (call_as); a table as such a view.
int wrapping_index(lua_State* L) {
    lua_settop(L, 2);
    lua_pushvalue(L, 2);
    const int type = lua_gettable(L, lua_upvalueindex(1)); // 3: the real value
    if (type != LUA_TFUNCTION && type != LUA_TTABLE) return 1;
    bool source_bound=false;
    if(type==LUA_TFUNCTION){
        lua_rawgetp(L,LUA_REGISTRYINDEX,&kSourceFunctions);
        if(lua_istable(L,-1)){lua_pushvalue(L,3);source_bound=lua_rawget(L,-2)!=LUA_TNIL;lua_pop(L,1);}
        lua_pop(L,1);
    }
    if(source_bound){
        lua_pushvalue(L,3);lua_pushvalue(L,lua_upvalueindex(3));lua_pushnil(L);
        auto& owner=plugin::lua_keep<LuaUi::CallbackOwner>(L);
        plugin::cxx_guard(L,[&]{if(auto current=LuaUi::callback_owner(L))owner=std::move(*current);});
        lua_pushcclosure(L,&call_as,4);
        return 1;
    }
    lua_pushvalue(L, 3);
    if (lua_gettable(L, lua_upvalueindex(2)) != LUA_TNIL) return 1;
    lua_pop(L, 1);
    if (type == LUA_TFUNCTION) {
        lua_pushvalue(L, 3);
        lua_pushvalue(L, lua_upvalueindex(3));
        lua_pushcclosure(L, &call_as, 2);
    } else {
        push_wrapping(L, 3, lua_upvalueindex(2), lua_upvalueindex(3));
    }
    lua_pushvalue(L, 3);
    lua_pushvalue(L, -2);
    lua_settable(L, lua_upvalueindex(2));
    return 1;
}

// A read-only view of `real` whose functions run as the plugin `tag` (wrapping_index).
void push_wrapping(lua_State* L, int real, int cache, int tag) {
    real = lua_absindex(L, real);
    cache = lua_absindex(L, cache); // (an upvalue's index stays as it is)
    tag = lua_absindex(L, tag);
    lua_newtable(L);
    lua_createtable(L, 0, 3);
    lua_pushvalue(L, real);
    lua_pushvalue(L, cache);
    lua_pushvalue(L, tag);
    lua_pushcclosure(L, &wrapping_index, 3);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, &read_only_write);
    lua_setfield(L, -2, "__newindex");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
}

} // namespace

int load_base_file(lua_State* L) { return lua_import_file(L, ImportKind::Base); }
int load_data_file(lua_State* L) { return lua_import_file(L, ImportKind::Data); }

// A plugin folder's globals: its own table, over a layer it cannot reach that holds its _G (the table itself), its
// hs (functions running as the folder), its ui (read-only, ui.window running as the folder), its dofile and load,
// and read-only standard libraries, over the pack's globals (read only through the layer: a write lands in the
// folder's own table). Leaves it on the stack.
void LuaUi::Impl::make_env(const std::string& tag) {
    lua_newtable(L); // env
    const int env = lua_gettop(L);
    lua_newtable(L); // the layer
    const int layer = lua_gettop(L);
    lua_pushvalue(L, env);
    lua_setfield(L, layer, "_G");
    lua_pushstring(L, tag.c_str());
    const int tag_at = lua_gettop(L);
    push_view_state(L, tag_at); // the views' state (S)
    const int S = lua_gettop(L);

    lua_newtable(L); // the cache of wrapped functions and views, by the real one
    lua_createtable(L, 0, 1);
    lua_pushliteral(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    const int cache = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, host_api_ref);
    push_wrapping(L, -1, cache, tag_at);
    // Temporary compatibility for existing addon windows; both names are the same owner-guarded view.
    // Selected-interface root compatibility is configured separately when its VM is created.
    if (!tag.empty()) {
        lua_pushvalue(L, -1);
        lua_setfield(L, layer, "eq");
    }
    lua_setfield(L, layer, "hs");
    lua_pop(L, 1);

    // ui: its constant tables read-only, ui.window as the folder, the rest the UI's own (they act on ImGui only).
    lua_rawgeti(L, LUA_REGISTRYINDEX, ui_ref);
    const int real_ui = lua_gettop(L);
    lua_newtable(L);
    const int ui_layer = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, real_ui)) {
        if (lua_type(L, -1) == LUA_TTABLE) {
            push_read_only(L, -1);
            lua_pushvalue(L, -3);
            lua_insert(L, -2);
            lua_settable(L, ui_layer);
        }
        lua_pop(L, 1);
    }
    lua_getfield(L, real_ui, "window");
    lua_pushvalue(L, tag_at);
    lua_pushcclosure(L, &call_as, 2);
    lua_setfield(L, ui_layer, "window");
    lua_createtable(L, 0, 1); // the rest as push_in gives it (a pack's Lua function in ui: its ui.SetSetting cache)
    lua_pushvalue(L, real_ui);
    lua_pushvalue(L, S);
    lua_pushvalue(L, tag_at);
    lua_pushcclosure(L, &pack_global, 3);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, ui_layer);
    push_read_only(L, ui_layer);
    lua_setfield(L, layer, "ui");
    lua_pop(L, 2);

    lua_pushvalue(L, tag_at);
    lua_pushcclosure(L, &lua_dofile, 1);
    lua_setfield(L, layer, "dofile");
    lua_getglobal(L, "load"); // (the text-only one: its first upvalue is Lua's)
    lua_getupvalue(L, -1, 1);
    lua_remove(L, -2);
    lua_pushvalue(L, env);
    lua_pushcclosure(L, &lua_load_text, 2);
    lua_setfield(L, layer, "load");
    for (const char* lib : {LUA_STRLIBNAME, LUA_TABLIBNAME, LUA_MATHLIBNAME, LUA_UTF8LIBNAME, LUA_OSLIBNAME}) {
        lua_getglobal(L, lib);
        push_read_only(L, -1);
        lua_setfield(L, layer, lib);
        lua_pop(L, 1);
    }
    lua_pushcfunction(L, &plugin_collectgarbage);
    lua_setfield(L, layer, "collectgarbage");
    lua_getglobal(L, "rawset");
    lua_pushcclosure(L, &plugin_rawset, 1);
    lua_setfield(L, layer, "rawset");
    lua_getglobal(L, "setmetatable");
    lua_pushvalue(L, S);
    lua_pushcclosure(L, &plugin_setmetatable, 2);
    lua_setfield(L, layer, "setmetatable");
    lua_getglobal(L, "getmetatable");
    lua_pushvalue(L, S);
    lua_pushvalue(L, tag_at);
    lua_pushcclosure(L, &plugin_getmetatable, 3);
    lua_setfield(L, layer, "getmetatable");

    // The pack's globals as push_in gives them (pack_global).
    lua_createtable(L, 0, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_pushvalue(L, S);
    lua_pushvalue(L, tag_at);
    lua_pushcclosure(L, &pack_global, 3);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, layer);
    lua_createtable(L, 0, 2);
    lua_pushvalue(L, layer);
    lua_setfield(L, -2, "__index");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, env);
    lua_settop(L, env);
}

namespace {
std::function<bool(std::string_view)>& replaced_check() {
    static std::function<bool(std::string_view)> check;
    return check;
}
} // namespace

void unview(lua_State* L, int idx) {
    if (lua_type(L, idx) != LUA_TTABLE) return;
    idx = lua_absindex(L, idx);
    const bool pack = lua_getfield(L, LUA_REGISTRYINDEX, kPackStateKey) == LUA_TBOOLEAN;
    lua_pop(L, 1);
    if (!pack) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, LuaUi::Impl::of(L).views_ref);
    lua_pushvalue(L, idx);
    if (lua_rawget(L, -2) == LUA_TTABLE) lua_replace(L, idx);
    else lua_pop(L, 1);
    lua_pop(L, 1);
}

bool replaces(std::string_view screen) {
    const auto& check = replaced_check();
    return check && check(screen);
}

void set_replaced_check(std::function<bool(std::string_view)> check) { replaced_check() = std::move(check); }

LuaUi::LuaUi(fs::path scripts, fs::path settings, std::vector<fs::path> approved_bases, DataRoots presentation_data,
             bool legacy_alias)
    : impl_(std::make_unique<Impl>()) {
    Impl& m = *impl_;
    m.dir = std::move(scripts);
    m.root = m.dir;
    m.bases = std::move(approved_bases);
    m.presentation_data = std::move(presentation_data);
    m.settings_file = std::move(settings);
    m.L = lua_newstate(&Impl::alloc, &m);
    lua_State* L = m.L;
    lua_atpanic(L, [](lua_State* S) -> int {
        log::error("ui: Lua error outside any script: {}", lua_tostring(S, -1) ? lua_tostring(S, -1) : "?");
        return 0;
    });
    lua_sethook(L, &Impl::budget_hook, LUA_MASKCOUNT, kHookCount);
    // The collector runs only in frame()'s slices (kGcSlice). The smallest step multiplier (incstep takes it as 1)
    // makes each LUA_GCSTEP about 500 objects' work, so a slice ends close to its time.
    lua_gc(L, LUA_GCINC, 0, 1, 0);
    lua_gc(L, LUA_GCSTOP);
    *static_cast<Impl**>(lua_getextraspace(L)) = &m;
    lua_pushlightuserdata(L,&m);lua_rawsetp(L,LUA_REGISTRYINDEX,&kMaintenanceHost);
    lua_pushcfunction(L,&Impl::maintenance_runner);
    m.maintenance_runner_ref=luaL_ref(L,LUA_REGISTRYINDEX);
    lua_pushboolean(L, 1); // the pack's state (pack_code)
    lua_setfield(L, LUA_REGISTRYINDEX, kPackStateKey);
    // The clipboard (ui/lua/lua_code_editor.cpp asks this before ui.GetClipboardText / SetClipboardText, as a script's
    // state answers in mq/lua_mq.cpp): the pack's code, or a plugin's that set_clipboard_check lets.
    lua_pushcfunction(L, [](lua_State* S) -> int {
        const auto& check = Impl::of(S).clipboard;
        lua_pushboolean(S, g_caller_tag.empty() || (check && check(g_caller_tag)));
        return 1;
    });
    lua_setfield(L, LUA_REGISTRYINDEX, "hailstone.clipboard_allowed");
    lua_pushcfunction(L, &Impl::match_check);
    lua_setfield(L, LUA_REGISTRYINDEX, kMatchCheckKey);
    // Libraries: no io (scripts do not need file access); os is reduced to clock/time/date.
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 1);
    lua_settop(L, 0);
    lua_getglobal(L, "os");
    lua_newtable(L);
    for (const char* keep : {"clock", "time", "date"}) {
        lua_getfield(L, -2, keep);
        lua_setfield(L, -2, keep);
    }
    lua_setglobal(L, "os");
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_pushcclosure(L, &lua_dofile, 1);
    lua_setglobal(L, "dofile");
    // Text only: no loadfile (any file), load refuses binary chunks; the strings' metatable cannot be reached (its
    // __index is the shared string table).
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    lua_getglobal(L, "load");
    lua_pushnil(L);
    lua_pushcclosure(L, &lua_load_text, 2);
    lua_setglobal(L, "load");
    lua_pushliteral(L, "");
    if (lua_getmetatable(L, -1)) {
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "__metatable");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_newtable(L);
    m.envs_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L); // every view (make_env) -> its table, by weak key
    lua_createtable(L, 0, 1);
    lua_pushliteral(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    m.views_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    register_ui_api(L); // the global table ui
    lua_getglobal(L, "ui");
    for (auto [name, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{
             {"window", &lua_window},
             {"base_window", &lua_base_window},
             {"GetSetting", &lua_get_setting},
             {"SetSetting", &lua_set_setting},
             {"DisableWindow", &lua_disable_window},
             {"Log", &lua_log}}) {
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, name);
    }
    lua_getglobal(L, "ui");
    m.ui_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    m.host_api_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (legacy_alias) {
        lua_pushvalue(L, -1);
        lua_setglobal(L, "eq");
    }
    lua_setglobal(L, "hs");
    m.load_settings();
    log::info("ui: Lua started, scripts in {}", m.dir.string());
}

LuaUi::~LuaUi() {
    impl_->closing=true;
    impl_->callback_lifetime.reset();
    impl_->callback_generations.clear();
    impl_->maintain_callbacks(); // the registry and its state still exist; no callback may survive lua_close
    impl_->callback_maintenance.clear();
    impl_->save_settings_now(); // (a change of the last second is not lost)
    if (impl_->L) {lua_close(impl_->L);impl_->L=nullptr;}
}

bool LuaUi::layout_setting(std::string_view key) { return key.starts_with("hud.") || key == "tasks.listw"; }

std::string LuaUi::setting(std::string_view key) const {
    if (auto v = impl_->get_setting(key)) return *v;
    return {};
}

bool LuaUi::set_setting(std::string_view key, std::string_view value) {
    return impl_->set_setting(key, value);
}

bool LuaUi::character_setting(std::string_view key) {
    return layout_setting(key) || key.starts_with("items.") || key.starts_with("tasks.tracked.") ||
           key.starts_with("tasks.tracker.") || key.starts_with("tasks.section.") || key == "tasks.category";
}

void LuaUi::set_character_settings(const fs::path& file) {
    Impl& m = *impl_;
    if (file == m.character_file) return;
    m.save_settings_now();
    m.character_file = file;
    m.character_settings = file.empty() ? Impl::Settings{} : Impl::read_settings(file);
    m.plugin_settings.clear(); // (counted again)
}

void LuaUi::copy_character_layout(const fs::path& from) {
    Impl& m = *impl_;
    if (m.character_file.empty()) return;
    Impl::Settings copied;
    for (auto& [k, v] : m.character_settings)
        if (!layout_setting(Impl::plugin_key_rest(k))) copied[k] = v; // its item marks stay its own
    if (!from.empty())
        for (auto& [k, v] : Impl::read_settings(from))
            if (layout_setting(Impl::plugin_key_rest(k))) copied[k] = std::move(v);
    m.character_settings = std::move(copied);
    m.plugin_settings.clear(); // (counted again)
    m.character_dirty = true;
    m.save_settings_now();
}

lua_State* LuaUi::state() const { return impl_->L; }

LuaUi::LoadStatus LuaUi::load_status() const {
    LoadStatus out;
    out.scanned = impl_->scanned;
    for (const auto& script : impl_->scripts) {
        if (!script.tag.empty()) continue;
        ++out.script_count;
        if (script.error.empty()) ++out.loaded_script_count;
        else out.errors.push_back(script.file + ": " + script.error);
    }
    for (const auto& window : impl_->windows) {
        if (!window.tag.empty()) continue;
        ++out.window_count;
        if (!window.error.empty()) out.errors.push_back(window.name + ": " + window.error);
    }
    out.error_count = out.errors.size();
    return out;
}

std::size_t LuaUi::allocated_bytes() const { return impl_->allocated; }

void LuaUi::set_gc_steps_for_test(int steps) { impl_->gc_steps = steps; }

void LuaUi::set_limits_for_test(std::size_t memory, std::chrono::milliseconds pack, std::chrono::milliseconds plugin) {
    impl_->memory_limit = memory;
    impl_->pack_budget = pack;
    impl_->plugin_budget = plugin;
}

std::string_view LuaUi::drawing_window() { return g_drawing; }
std::string_view LuaUi::caller_tag() { return g_caller_tag; }

std::optional<LuaUi::CallbackOwner> LuaUi::callback_owner(lua_State* L) {
    if(g_callback_source)return *g_callback_source;
    const bool pack_state=lua_getfield(L,LUA_REGISTRYINDEX,kPackStateKey)==LUA_TBOOLEAN;
    lua_pop(L,1);
    if(!pack_state)return std::nullopt;
    Impl& m=Impl::of(L);
    const std::string tag(g_caller_tag);
    if(!tag.empty()&&!m.folder_dir(tag))return std::nullopt;
    const std::string file=m.running_tag==tag?m.running_file:std::string();
    const std::string generation=file.empty()?"@owner:"+tag:file;
    auto& token=m.callback_generations[generation];
    if(!token)token=std::make_shared<int>(0);
    const std::weak_ptr<void> vm=m.callback_lifetime,source=token;
    CallbackOwner owner;
    owner.tag=tag;
    owner.key="ui:"+std::to_string(reinterpret_cast<std::uintptr_t>(m.L))+":"+tag+":"+generation;
    owner.valid=[vm,source,tag,host=&m]{return !vm.expired()&&!source.expired()&&(tag.empty()||host->folder_dir(tag));};
    owner.invoke=[vm,source,tag,file,host=&m](lua_State* state,int args,int results){
        if(vm.expired()||source.expired()||state!=host->L||(!tag.empty()&&!host->folder_dir(tag)))return false;
        struct SourceState {
            Impl& host;
            std::string file, tag;
            explicit SourceState(Impl& owner) : host(owner), file(owner.running_file), tag(owner.running_tag) {}
            ~SourceState() { host.running_file=std::move(file);host.running_tag=std::move(tag); }
        } source_state(*host);
        host->running_file=file;host->running_tag=tag;
        const bool imgui=ImGui::GetCurrentContext()!=nullptr&&ImGui::GetCurrentContext()->WithinFrameScope&&GImGui->CurrentWindow;
        struct DrawingState {
            bool active;
            std::size_t begun;
            explicit DrawingState(bool on) : active(on), begun(on?windows_begun(true):0) {}
            ~DrawingState() { if(active){recover_script_windows();end_script_windows(begun);} }
        } drawing_state(imgui);
        int status=LUA_OK;
        {
            Impl::Budget budget(*host,std::min(host->budget_for(tag),std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(50))));
            Impl::Acting acting(*host,tag);
            status=lua_pcall(state,args,results,0);
        }
        if(status==LUA_OK)return true;
        const char* why=lua_tostring(state,-1);
        bool reported=false;
        for(auto& script:host->scripts)if(script.file==file){host->report(script,why);reported=true;break;}
        if(!reported)log::warn("ui retained callback error: {}",why?why:"error");
        lua_pop(state,1);
        return false;
    };
    return owner;
}

void LuaUi::source_generation_function(lua_State* L,int index) {
    index=lua_absindex(L,index);
    if(lua_rawgetp(L,LUA_REGISTRYINDEX,&kSourceFunctions)!=LUA_TTABLE){
        lua_pop(L,1);lua_newtable(L);lua_newtable(L);lua_pushliteral(L,"k");lua_setfield(L,-2,"__mode");lua_setmetatable(L,-2);
        lua_pushvalue(L,-1);lua_rawsetp(L,LUA_REGISTRYINDEX,&kSourceFunctions);
    }
    lua_pushvalue(L,index);lua_pushboolean(L,1);lua_rawset(L,-3);lua_pop(L,1);
}

bool LuaUi::set_callback_maintenance(lua_State* L,std::string_view name,const CallbackMaintenance& callback) {
    if (!L || name.empty() || name.size()>64 || !lua_checkstack(L,1)) return false;
    for (const unsigned char c : name) if (c<33 || c>126) return false;
    const int top=lua_gettop(L);
    lua_rawgetp(L,LUA_REGISTRYINDEX,&kMaintenanceHost);
    auto* host=lua_type(L,-1)==LUA_TLIGHTUSERDATA?static_cast<Impl*>(lua_touserdata(L,-1)):nullptr;
    lua_settop(L,top);
    if (!host || host->L!=L || host->closing) return false;
    try {
        if (!callback) {host->callback_maintenance.erase(std::string(name));return true;}
        if (!host->callback_maintenance.contains(name) && host->callback_maintenance.size()>=16) return false;
        auto held=std::make_shared<CallbackMaintenance>(callback);
        host->callback_maintenance.insert_or_assign(std::string(name),std::move(held));
        return true;
    } catch (...) { return false; }
}

void LuaUi::set_caller_scope(CallerScope scope) { impl_->scope = std::move(scope); }

void LuaUi::set_clipboard_check(ClipboardCheck check) { impl_->clipboard = std::move(check); }

void LuaUi::push_folder_env_for_test(std::string_view tag) const {
    lua_State* L = impl_->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, impl_->envs_ref);
    lua_getfield(L, -1, std::string(tag).c_str());
    lua_remove(L, -2);
}

std::string LuaUi::run_for_test(std::string_view tag_view, const std::string& code) {
    Impl& m = *impl_;
    lua_State* L = m.L;
    const std::string tag(tag_view);
    const int top = lua_gettop(L);
    if (!tag.empty()) m.push_env(tag); // (before the limits hold, as load_file)
    else lua_pushnil(L);
    int status = LUA_OK;
    {
        Impl::Budget budget(m, m.budget_for(tag));
        Impl::Acting acting(m, tag);
        status = luaL_loadbufferx(L, code.data(), code.size(), "=test", "t");
        if (status == LUA_OK && !tag.empty()) {
            lua_pushvalue(L, -2);
            lua_setupvalue(L, -2, 1);
        }
        if (status == LUA_OK) status = lua_pcall(L, 0, 1, 0);
    }
    std::string out;
    if (status != LUA_OK) out = std::string("error: ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
    else if (lua_isboolean(L, -1)) out = lua_toboolean(L, -1) ? "true" : "false";
    else if (lua_isstring(L, -1)) out = lua_tostring(L, -1);
    else out = luaL_typename(L, -1);
    lua_settop(L, top);
    return out;
}

void LuaUi::add_ui_api(const Registrar& r) {
    lua_State* L = impl_->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, impl_->host_api_ref);
    r(L);
    lua_pop(L, 1);
}

void LuaUi::set_show_control(bool show) { impl_->show_control = show; }

void LuaUi::rescan_for_test() { impl_->last_scan = Clock::time_point{}; }

void LuaUi::set_entry_scripts(std::vector<std::string> files) { impl_->entries = std::move(files); }

void LuaUi::set_folders(std::vector<Folder> folders) {
    bool same = folders.size() == impl_->folders.size() &&
                std::equal(folders.begin(), folders.end(), impl_->folders.begin(),
                           [](const Folder& a, const Folder& b) { return a.tag == b.tag && a.dir == b.dir; });
    if (same) return;
    // Invalidate retained callbacks immediately, before the next script scan closes their windows.
    for(const auto& old:impl_->folders) {
        const auto found=std::find_if(folders.begin(),folders.end(),[&](const Folder& f){return f.tag==old.tag&&f.dir==old.dir;});
        if(found==folders.end()) {
            impl_->callback_generations.erase("@owner:"+old.tag);
            for(const auto& script:impl_->scripts)if(script.tag==old.tag)impl_->callback_generations.erase(script.file);
        }
    }
    impl_->folders = std::move(folders);
    impl_->folders_changed = true;
    impl_->maintain_callbacks();
}

void LuaUi::frame(bool draw) {
    Impl& m = *impl_;
    lua_State* L = m.L;
    core::frame_sections::Lap lap;
    if (m.scripts.empty() && m.last_scan == Clock::time_point{}) {
        m.last_scan = Clock::now();
        m.scan(true);
    } else if (m.folders_changed || Clock::now() - m.last_scan >= std::chrono::seconds(1)) { // reload changed scripts
        m.last_scan = Clock::now();
        m.scan(false);
    }
    m.maintain_callbacks();
    lap.mark("Lua script scan");
    if (draw && m.show_control) m.draw_control();
    // Each name's top layer draws (Window); by index, since a draw function can add windows.
    // (Only with plugin windows; each window's layer found once, and names compared only among the plugins' and the
    // pack's windows they could cover.)
    std::vector<bool> covered;
    if (draw && std::any_of(m.windows.begin(), m.windows.end(), [](const Window& w) { return !w.tag.empty(); })) {
        const std::size_t n = m.windows.size();
        std::vector<int> layers(n);
        for (std::size_t i = 0; i < n; ++i) layers[i] = m.layer(m.windows[i].tag);
        covered.assign(n, false);
        for (std::size_t i = 0; i < n; ++i) {
            if (layers[i] <= 0) continue; // (a plugin's window covers; the pack's never does)
            for (std::size_t j = 0; j < n; ++j)
                if (j != i && layers[j] < layers[i] && m.windows[j].name == m.windows[i].name) covered[j] = true;
        }
    }
    for (std::size_t i = 0; draw && i < m.windows.size(); ++i) {
        Window& w = m.windows[i];
        if (!w.enabled || w.failed || (i < covered.size() && covered[i])) continue;
        ImGuiErrorRecoveryState state;
        ImGui::ErrorRecoveryStoreState(&state);
        const std::size_t begun = windows_begun();
        lua_rawgeti(L, LUA_REGISTRYINDEX, w.draw_ref);
        // (w may move while it draws: a draw function can add windows, never remove one, so index i stays its own)
        g_drawing = w.name;
        m.drawing_tag = w.tag;
        m.running_tag = w.tag;
        m.running_file = w.file;
        int status = LUA_OK;
        const std::size_t allocated_before = m.allocated;
        {
            Impl::Budget budget(m, m.budget_for(w.tag));
            Impl::Acting acting(m, w.tag);
            status = lua_pcall(L, 0, 0, 0);
        }
        m.running_file.clear();
        m.running_tag.clear();
        m.drawing_tag.clear();
        g_drawing.clear();
        // A window the script left open (a Begin without its End) is closed here, error or not, so the windows
        // drawn after it do not nest inside it.
        ImGui::ErrorRecoveryTryToRecoverState(&state);
        end_script_windows(begun);
        Window& drawn = m.windows[i];
        if (status != LUA_OK) {
            m.report_window(drawn, lua_tostring(L, -1)); // stays off until the script is fixed and reloads
            lua_pop(L, 1);
        }
        if (!drawn.section) {
            // Every name kept is kept for good (core/frame_sections.h): the plugins' windows share a few hundred.
            if (drawn.tag.empty()) drawn.section = core::frame_sections::intern("Lua window " + drawn.name);
            else if (m.plugin_sections < kPluginSections) {
                const std::string name = "Lua window " + drawn.tag + "/" + drawn.name;
                ++m.plugin_sections;
                drawn.section = core::frame_sections::intern(name);
            } else {
                drawn.section = "Lua windows of plugins (more)";
            }
        }
        lap.mark(drawn.section);
        core::frame_sections::add_bytes(drawn.section, m.allocated - allocated_before);
    }
    if (draw) m.draw_errors();
    m.save_settings_if_due();
    lap.mark("Lua errors, settings");
    m.collect_garbage();
    lap.mark("Lua garbage collection");
}

} // namespace hailstone::ui::lua
