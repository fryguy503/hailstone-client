#pragma once

// What plugins give the Lua UI (docs/plugins.md "Plugin windows"): functions their Lua windows call
// (hs.Plugin(name)), options the Options window lists (hs.PluginOptions), entries for the packs' menus
// (hs.PluginMenu), stock-window contributions, and each plugin's folder, whose ui/<pack>/ holds its windows (the app hands
// them to ui::lua::LuaUi::set_folders). Compiled plugins add theirs through the host table (plugin/native_plugins.h),
// Lua plugins (scripts) through hailstone.ui_function and the rest (mq/lua_hailstone.cpp); everything of a plugin
// goes when it unloads. Values cross as UiValue trees, so nothing here depends on Lua. Single threaded.
//
// Each entry belongs to an owner by kind and name. The owner's tag ("plugin:<name>", "script:<name>"; owner_tag)
// is what its windows run as (LuaUi::caller_tag) and what its capabilities are looked up
// by (app::Automation::plugin_caps), so a script can never take a compiled plugin's place, folder or functions by
// naming itself after it: a registration replaces or removes only its own kind's entries. Names compare without
// case. A lookup by a bare name ("nms", what the packs' hs.Plugin passes) finds a compiled plugin's before a
// script's; a lookup by tag finds that owner alone.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct HailstoneUiContribution;

namespace hailstone::plugin {

// A value between a plugin and Lua: nil, a boolean, an integer, a number, a string, an array (Lua's 1..n) or a
// table of string keys.
struct UiField;

struct UiValue {
    enum class Kind : std::uint8_t { Nil, Bool, Int, Number, String, Array, Table };
    Kind kind = Kind::Nil;
    bool b = false;
    std::int64_t i = 0;
    double n = 0.0;
    std::string s;
    std::vector<UiValue> items;                          // Array
    std::vector<UiField> fields;                         // Table, in the order they were set

    static UiValue boolean(bool v);
    static UiValue integer(std::int64_t v);
    static UiValue number(double v);
    static UiValue string(std::string v);
    // A table's field by key (null when absent or not a table).
    const UiValue* field(std::string_view key) const;
};

// A table's field. A struct declared after UiValue rather than a std::pair: a pair's member must be complete where
// the vector is declared, which Clang with libstdc++ (and so macOS) refuses inside UiValue itself.
struct UiField {
    std::string key;
    UiValue value;
    UiField(std::string k, UiValue v) : key(std::move(k)), value(std::move(v)) {}
};

class UiBridge {
public:
    enum class Kind : std::uint8_t { Plugin, Script, Upscaler };
    // "plugin:<name>" / "script:<name>" / "upscaler:<name>" (the latter owns contributions only).
    static std::string owner_tag(Kind kind, std::string_view name);
    // A tag's kind and name; nullopt for a bare name.
    static std::optional<std::pair<Kind, std::string>> parse_tag(std::string_view tag);

    using Fn = std::function<UiValue(const std::vector<UiValue>& args)>;
    // The most arguments one call passes (hs.Plugin(...).fn(table.unpack(t)) could pass a million): past it the call
    // is refused (nullopt). Each one is a slot on the callee's Lua stack.
    static constexpr std::size_t kMaxCallArgs = 64;
    // What one owner may have (docs/plugins.md "Plugin windows"): past it an add is refused (false). A script's
    // entries live outside its Lua memory limit, and the packs list every option and menu entry each frame.
    static constexpr std::size_t kMaxFunctions = 256;
    static constexpr std::size_t kMaxOptions = 64;
    static constexpr std::size_t kMaxMenuEntries = 32;
    static constexpr std::size_t kMaxContributions = 32;
    static constexpr std::size_t kMaxTotalContributions = 256;
    static constexpr std::size_t kMaxContributionIdBytes = 128;
    static constexpr std::size_t kMaxContributionWindowBytes = 128;
    static constexpr std::size_t kMaxContributionTargetBytes = 128;
    static constexpr std::size_t kMaxContributionTextBytes = 1024;
    static constexpr std::size_t kMaxContributionActionLabelBytes = 128;
    static constexpr std::size_t kMaxContributionCommandBytes = 512;
    struct Option {
        std::string plugin, name, label;
        int min = 0, max = 1; // max 1: a switch; else a slider
        std::function<int()> get;
        std::function<void(int)> set;
        Kind kind = Kind::Plugin; // whose: plugin's kind
        std::string owner() const { return owner_tag(kind, plugin); }
    };
    struct MenuEntry {
        std::string plugin, label, command; // the command line run when it is chosen
        Kind kind = Kind::Plugin;
        std::string owner() const { return owner_tag(kind, plugin); }
    };
    struct Contribution {
        std::string plugin, id, window, target, text, action_label, command;
        bool disabled = false;
        Kind kind = Kind::Plugin;
        std::string owner() const { return owner_tag(kind, plugin); }
    };

    // `plugin` names the owner (with `kind`); in the lookups below it is a bare name or a tag (owner_tag).
    // Replaces one of the same owner and name; false (nothing added) when the owner has kMaxFunctions others.
    bool add_function(const std::string& plugin, const std::string& name, Fn fn, Kind kind = Kind::Plugin);
    void remove_function(std::string_view plugin, std::string_view name, Kind kind = Kind::Plugin);
    // nullopt: no such plugin or function, or more than kMaxCallArgs arguments.
    std::optional<UiValue> call(std::string_view plugin, std::string_view name, const std::vector<UiValue>& args) const;
    bool has_plugin(std::string_view plugin) const;
    // The tag of the owner a bare name or tag finds (a compiled plugin's before a script's); "" none.
    std::string resolve(std::string_view plugin) const;

    bool add_option(Option o); // replaces one of the same owner and name; false past kMaxOptions
    const std::vector<Option>& options() const { return options_; }
    Option* find_option(std::string_view plugin, std::string_view name);

    // Replaces the owner's entry of the same label (its command changes); false past kMaxMenuEntries.
    bool add_menu(MenuEntry e);
    const std::vector<MenuEntry>& menu() const { return menu_; }

    // The controls that are the player's way out of an add-on or a broken pack: the upscaler (an upscaler add-on),
    // the UI pack (a UI pack add-on, or a pack that no longer works) and the texture pack (a pack a tool add-on built).
    // A contribution may put a notice on them but never disable them: set_contribution refuses such an entry whole,
    // from any owner (script, plugin or upscaler) and in any window.
    static bool escape_target(std::string_view target);
    // Replaces an entry by owner and id. An invalid update leaves its previous entry intact.
    bool set_contribution(Contribution entry);
    bool set_contribution_from_sdk(std::string_view plugin, const HailstoneUiContribution* entry,
                                   Kind kind = Kind::Plugin);
    bool remove_contribution(std::string_view plugin, std::string_view id, Kind kind = Kind::Plugin);
    bool remove_contribution_from_sdk(std::string_view plugin, const char* id, Kind kind = Kind::Plugin);
    void remove_contributions(std::string_view plugin, Kind kind = Kind::Plugin);
    const std::vector<Contribution>& contributions() const { return contributions_; }
    const Contribution* find_contribution(std::string_view window, std::string_view owner, std::string_view id) const;

    // The owner's folder: its windows are in <folder>/ui/<pack>/. folders() lists them by owner tag (the tag their
    // windows run as).
    void set_folder(const std::string& plugin, std::filesystem::path folder, Kind kind = Kind::Plugin);
    const std::vector<std::pair<std::string, std::filesystem::path>>& folders() const { return folders_; }

    // Everything of an owner (it unloaded).
    void remove(std::string_view plugin, Kind kind = Kind::Plugin);

private:
    struct Function {
        std::string plugin, name;
        Fn fn;
        Kind kind = Kind::Plugin;
        std::string owner() const { return owner_tag(kind, plugin); }
    };
    std::vector<Function> functions_;
    std::vector<Option> options_;
    std::vector<MenuEntry> menu_;
    std::vector<Contribution> contributions_;
    std::vector<std::pair<std::string, std::filesystem::path>> folders_;
};

} // namespace hailstone::plugin
