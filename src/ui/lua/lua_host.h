#pragma once

// The Lua UI: windows written as Lua scripts over Dear ImGui (the `ui` table)
// and the game (the `hs` table). Scripts are loaded from the UI's folder (ui/<name>), reloaded
// when they change, and each window they register with ui.window(name, draw)
// is drawn once per frame. A script error never stops the client: it shows in
// the "UI errors" window and the failed window stays off until it reloads.
// Every chunk is loaded as text: no precompiled Lua (a binary chunk can corrupt memory).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct lua_State;
struct ImFont;

namespace hailstone::ui {

// EQ UI font 1..6 (the <Font> numbers in the stock XML); null outside that
// range or before the ImGui layer loaded them.
ImFont* ui_font(int index);
// The chat windows' font list (/chatfontsize 0..10; ui/imgui_layer.cpp): null outside 0..10 or before it is loaded;
// and each one's cell height in pixels at UI scale 1.
inline constexpr std::size_t kChatFonts = 11;
ImFont* chat_font(int index);
float chat_font_size(int index);
// The display scale the UI fonts were built for (ui/imgui_layer.cpp): the EQ fonts are their XML sizes times it.
float ui_scale();
void set_ui_scale(float scale); // testing: the scale without an ImGui layer

namespace lua {

void register_ui_api(lua_State* L); // the global `ui` table (lua_imgui.cpp)

// Whether a Lua window stands in for the stock window `screen` (its XML screen name: "PlayerWindow",
// "CastSpellWnd", ...). The temporary C++ windows ask before drawing, so each Lua window turns its C++ counterpart
// off while it runs; the app sets the check (app/lua_ui.h UiApi::replaces).
bool replaces(std::string_view screen);
void set_replaced_check(std::function<bool(std::string_view)> check);

// While the player is asked about a server's compiled plugins (plugin/consent.h), the windows that ask are guarded
// (lua_imgui.cpp): no code but the pack's own (a plugin folder's, an MQ script's ImGui) may draw over every window
// (ui.Foreground) or locate them (ui.FindWindowRect), and they are raised above every other window at the end of the
// frame, so nothing can be drawn over the question to change what the player reads or where the answer is.
void set_consent_guard(bool on); // the question is up (app: Automation::frame)
bool consent_guard();
// The ImGui window just begun shows the question (the client's own prompt, after its Begin).
void guard_current_window();
// The pack's window drawing now (LuaUi::drawing_window) shows the question: the ImGui windows it begins this frame
// are guarded (hs.PluginConsent asked from the pack's own code).
void guard_drawing_window();
// At the end of the frame's windows: the guarded ones to the front.
void raise_guarded_windows();
// The client's own consent prompt's ImGui window (app/automation.cpp): while the question is up, only the pack's
// code may begin a window of this name, before the prompt's first frame too.
inline constexpr const char* kConsentWindowName = "Allow this server's plugin?###hailstone_plugin_consent";
// Its id alone: the prompt's title follows what it asks about (plugin::consent_wording), and a name with another
// title before this id is the same window.
inline constexpr const char* kConsentWindowId = "###hailstone_plugin_consent";
// Around each script's draw function (the Lua UI's windows, an MQ script's), always as a pair: how many windows
// ui.Begin and ui.BeginChild have open before it runs (and the style and font stacks' sizes then: its pops stop
// there, so it cannot pop what the code around it pushed); after it returns (normally or not, once ImGui's stacks are
// restored) that count is put back and anything the script set for the next window or item is dropped, so nothing it
// left reaches the windows drawn after it.
std::size_t windows_begun(bool preserve_parent = false);
// Recover only the innermost callback's additions; preserve caller-owned tables and tab bars.
void recover_script_windows();
void end_script_windows(std::size_t depth);
// Whether `L` runs the pack's own code now: the UI pack's state (LuaUi; its registry holds kPackStateKey), with no
// plugin folder's code running.
bool pack_code(lua_State* L);
// Execute a text Lua file from the caller's next approved UI base, retaining its environment and presenter owner.
// Only the managed UI VM can call this; the public host binding is hs.UI.interfaces.LoadBase.
int load_base_file(lua_State* L);
// Shared presentation-data modules use separately approved data roots, retaining the same caller restrictions.
int load_data_file(lua_State* L);
// A raster file in the active caller's UI folder, selected interface or approved
// bases. Unlike game assets this follows UI ownership; no arbitrary disk paths.
std::optional<std::filesystem::path> interface_image_path(lua_State* L, std::string_view relative);
// How many ui.CodeEditor states (lua_code_editor.cpp) are kept now; for the tests.
std::size_t code_editors_kept();
// The frame the longest-kept of them was first drawn in (ui::imgui_frame; -1 with none); for the tests.
std::int64_t code_editors_oldest();
// Where ui.CodeEditor draws each byte offset of `at` (x, y from the text's start), asked in that order of one placer;
// for the tests (an ImGui frame must be current).
std::vector<std::pair<float, float>> code_editor_places(const std::string& text, const std::vector<std::size_t>& at);
inline constexpr const char* kPackStateKey = "hailstone.ui.pack";
// The registry key of a state's long-match check: the vendored Lua's pattern matching (third_party/lua/lstrlib.c,
// "Hailstone") calls the C function held there every 65536 steps, and it raises an error once the code running the
// match is past its time limit (a match runs no Lua instructions, so a count hook never sees it). The UI's state
// sets one (lua_host.cpp), and so does each script's (mq/lua_mq.cpp).
inline constexpr const char* kMatchCheckKey = "hailstone.match_check";
// A plugin sees the pack's shared tables through views (lua_host.cpp); a C function that reads a table raw
// (lua_rawgeti, lua_next, lua_rawlen) calls this first on its argument: a view at idx is replaced by its table (only
// in the pack's state; anything else stays as it is).
void unview(lua_State* L, int idx);

class LuaUi {
public:
    struct DataRoots {
        std::filesystem::path root; // common presentation-data anchor, outside manager-owned code
        std::vector<std::string> ids; // selected stable interface ID first, then approved bases in order
    };
    // `scripts`: the UI's folder (ui/<name>, ui/ui_packs.h) holding the *.lua windows; dofile paths are
    // relative to it ("lib/sidl.lua"). The settings file keeps the scripts' key=value options.
    // approved_bases is the nearest-first chain validated by list_ui_packs, not paths supplied by Lua code.
    LuaUi(std::filesystem::path scripts, std::filesystem::path settings,
          std::vector<std::filesystem::path> approved_bases = {}, DataRoots presentation_data = {});
    ~LuaUi();
    LuaUi(const LuaUi&) = delete;
    LuaUi& operator=(const LuaUi&) = delete;

    // Adds functions to the `hs` table (on the top of the stack); called once
    // at start and again for every later registrar.
    using Registrar = std::function<void(lua_State*)>;
    void add_ui_api(const Registrar& r);
    // Its name up to 0.7 (app/lua_eq.h): the compiled server plugins' own tests still call it.
    void add_eq_api(const Registrar& r) { add_ui_api(r); }

    // Between ImGuiLayer::new_frame and render: reloads changed scripts (once a second), maintains callback
    // lifetimes, saves settings and collects garbage. draw=false skips window/control/error rendering only;
    // source initializers still run under their ordinary owner scope when files change.
    void frame(bool draw = true);

    lua_State* state() const;
    // Every byte the state has allocated since it was made, freed since or not (it wraps: take differences). Each
    // window's share of a frame goes to core::frame_sections::add_bytes under its section ("Lua window <name>").
    std::size_t allocated_bytes() const;
    struct LoadStatus {
        bool scanned = false;
        std::size_t script_count = 0, loaded_script_count = 0, window_count = 0, error_count = 0;
        std::vector<std::string> errors;
    };
    // Actual selected-pack loading/drawing errors, excluding independently owned plugin folders.
    LoadStatus load_status() const;
    // Tests: let the next frame scan changed/removed overlays without waiting for the one-second scan interval.
    void rescan_for_test();
    // Tests: the state's memory limit and how long the pack's and a plugin's code may run at a time (lua_host.cpp
    // kMemoryLimit, kPackBudget, kPluginBudget).
    void set_limits_for_test(std::size_t memory, std::chrono::milliseconds pack, std::chrono::milliseconds plugin);
    // The ui.window being drawn (its name) while its draw function runs, else "".
    static std::string_view drawing_window();
    // Whose code runs now: "" the pack's own (and the player's), a plugin folder's tag (set_folders) while that
    // folder's code runs: its scripts loading, its windows drawing, and any function it took from `hs` or `ui`,
    // whoever calls it. A pack window a plugin's draws (ui.base_window) runs as the plugin.
    static std::string_view caller_tag();
    // While it lives, the code runs as the client itself (caller_tag() ""), whatever folder called in: for a client
    // pass that runs add-ons' retained callbacks from its own drawing (the map overlays, app/lua_ui_map_overlay.cpp),
    // where one add-on draws the map and a second's callback is drawn into it. The capabilities are the app's to
    // lift (plugin::HostCallerScope), as the callback's own come from its owner (CallbackOwner::invoke).
    class HostScope {
    public:
        HostScope();
        ~HostScope();
        HostScope(const HostScope&) = delete;
        HostScope& operator=(const HostScope&) = delete;

    private:
        std::string before_;
    };
    struct CallbackOwner {
        // What a protected callback call came to. kSkipped: nothing ran, because the code the callback is drawn
        // inside had already used up the frame's time; that is not the callback's own failure (invoke_drawn).
        enum class Call { kRan, kFailed, kSkipped };
        std::string key, tag;
        std::function<bool()> valid;
        std::function<bool(lua_State*,int,int)> invoke;
        // As invoke, for a callback that is drawn inside another add-on's code (a map overlay): a call that cannot
        // start, or is stopped, only because that code has used up the frame's time is kSkipped. Nothing is
        // reported as an error and a drawing layer can keep the callback registered.
        std::function<Call(lua_State*,int,int)> invoke_drawn;
    };
    // Captures the current file generation and normal caller scope for a retained callback. A reload,
    // removed folder or closed VM invalidates it, even when another window retained the Lua closure.
    static std::optional<CallbackOwner> callback_owner(lua_State* L);
    // A callback-registration function taken from a host view retains its source generation as well as its tag.
    static void source_generation_function(lua_State* L, int index);
    // Host-private metadata cleanup only: no user callbacks, drawing or gameplay. At most 16 named services,
    // replaced by name. Runs after source retirement/loading and each idle frame, protected while L is alive.
    // Returning false leaves the caller's stack unchanged (not a UI state, closing, invalid name or capacity).
    using CallbackMaintenance = std::function<void(lua_State*)>;
    static bool set_callback_maintenance(lua_State* L, std::string_view name, const CallbackMaintenance& callback);

    // What a plugin folder's code runs under (the app: a plugin::CallerScope with that plugin's capabilities): made
    // when its code starts, released when it ends. Unset: nothing.
    using CallerScope = std::function<std::shared_ptr<void>(std::string_view tag)>;
    void set_caller_scope(CallerScope scope);

    // Whether a plugin folder's code (its tag) may use the player's clipboard (ui.GetClipboardText / SetClipboardText,
    // which ask the state's registry function "hailstone.clipboard_allowed"): it holds whatever the player copied, a
    // password too. The pack's own code always may; unset, no plugin's may. The app: only an owner with every
    // capability.
    using ClipboardCheck = std::function<bool(std::string_view tag)>;
    void set_clipboard_check(ClipboardCheck check);

    // The "Lua UI" control window (scripts on/off, reload, the ImGui demo): a developer window, off by default.
    void set_show_control(bool show);

    // Only these scripts of the UI's folder load, by their path in it ("tools/texstudio.lua"), instead of every *.lua
    // at its top: a program other than the client drawing its own screen in a pack's look, with the pack's lib/
    // (tools/texstudio). Call before the first frame. Their dofile paths stay relative to the UI's folder.
    void set_entry_scripts(std::vector<std::string> files);

    // More folders of windows besides the UI's own: a loaded plugin's ui/<pack>/ (app: plugin::UiBridge). Each
    // folder's *.lua load like the UI's (named "<tag>/<file>" in errors), their dofile paths are relative to their
    // folder, and a folder no longer listed takes its windows with it. Applied at the next frame.
    // A folder's code is confined (docs/plugins.md "Plugin windows"): its globals are its own table (reads fall back
    // to the pack's globals; writes stay its own), it sees `hs`, `ui` and the standard libraries read-only, every
    // `hs` and `ui.window` function it takes runs as the folder (caller_tag, set_caller_scope), and dofile stays
    // inside its folder or the pack's.
    struct Folder {
        std::string tag;
        std::filesystem::path dir;
    };
    void set_folders(std::vector<Folder> folders);
    // Tests: pushes a folder's globals table (nil before its first script loaded).
    void push_folder_env_for_test(std::string_view tag) const;
    // Tests: runs `code` as a chunk of the folder `tag` ("" the pack's own), with its globals and as its code runs
    // (caller_tag, the caller scope); its first result as text, or "error: <message>".
    std::string run_for_test(std::string_view tag, const std::string& code);

    // The character's own layout: the window keys (character_setting) are read from `file` before the shared settings
    // file, and written to it; a key it lacks comes from the shared file, so a new character starts from the shared
    // layout. Empty: none (before a character enters the world), every key in the shared file. Saves what was pending.
    void set_character_settings(const std::filesystem::path& file);
    // The layout copy: the character's layout becomes the window keys of `from` (another character's file), or the
    // shared layout when `from` is empty; the character's other keys of its own (its item marks) stay. Saved at once.
    void copy_character_layout(const std::filesystem::path& from);
    std::string capture_layout_profile_settings() const;
    bool apply_layout_profile_settings(std::string_view snapshot);
    // Whether a settings key is the character's rather than shared (the palette, the spell book's sort and filters,
    // ...): its window layout (layout_setting), and the item marks ("items." keys: locks, junk, bag filters). The
    // character's file is per server, so a same-named character elsewhere has marks of its own.
    static bool character_setting(std::string_view key);
    // Whether a settings key is the window layout: "hud." keys (places, sizes, scales, opacity, fades, locks, the
    // buff trays' and bars' shapes) and the task window's list width.
    static bool layout_setting(std::string_view key);

    // A saved setting (the same ui.GetSetting reads; "" when it is not set), and setting one (ui.SetSetting's
    // storage, saved to the character's layout or the shared file as that key asks). For the client's own menus.
    std::string setting(std::string_view key) const;
    bool set_setting(std::string_view key, std::string_view value);

    struct Impl; // lua_host.cpp

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace lua
} // namespace hailstone::ui
