#pragma once

// Key bindings, hot buttons, skills and combat abilities: the glue between the original's rules
// (game/keymap.h, game/hotbuttons.h, game/mechanics/abilities.h), the connection (net/ability_packets.h) and the
// developer windows drawn with Dear ImGui until the stock UI engine (M5): the hot bar (HotButtonWnd), the Actions
// window (ActionsWnd: Main, Abilities, Combat and Socials pages, with every usable skill and its timer) and the key
// binding window (the Keyboard page of the original's options window).
//
// Key bindings are kept in the client config (KEYMAPPING_<command>_<1|2> = KeyCombo int, as the original's
// eqclient.ini [KeyMaps]); hot buttons, socials and the Actions window's buttons per character in
// servers/<profile>/characters/<world>/<name>/hotbuttons.ini (the original's <name>_<server>.ini), in its sections and format.

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "core/ini_file.h"
#include "game/compass.h"
#include "game/hot_button_press.h"
#include "game/hot_button_sets.h"
#include "game/hotbuttons.h"
#include "game/keymap.h"
#include "game/mechanics/abilities.h"

namespace hailstone { class Config; }
namespace hailstone::platform { struct InputState; }
namespace hailstone::net { class GameConnection; }
namespace hailstone::game::mechanics { class Character; }

namespace hailstone::app {

// What the client knows about the player this frame that the connection does not.
struct AbilityView {
    glm::vec3 position{0.0f};   // the spawn's reference point, world coordinates (feet + floor offset)
    float heading = 0.0f;       // 0..512
    glm::vec3 velocity{0.0f};   // world units per physics tick (SpeedX/Y/Z)
    bool can_see_target = true; // line of sight to the target
};

// What a hot button press reaches outside the actions code (client_app sets them).
struct HotButtonHooks {
    std::function<void(std::uint32_t command)> execute_cmd;          // __ExecuteCmd (AUTOPRIM 10, the find window 354)
    std::function<void()> toggle_view;                              // CDisplay::ToggleView
    std::function<bool(game::HotButtonWindow)> window_active;       // CXWnd::IsActive
    std::function<void(game::HotButtonWindow, bool show)> window_show; // Activate / Deactivate
    std::function<bool(int button)> pet_button;                     // the pet window's button n (false: none)
    std::function<void(std::int32_t slot, std::int32_t bag)> use_item; // a right click on an inventory slot
    std::function<void(std::int32_t social)> open_social_editor;     // CSocialEditWnd (0x758bb0)
    std::function<void(std::int32_t group)> show_aa_ability;         // CAAWnd activated, ShowAbility(group)
    std::function<void(std::int32_t gem)> spell_gem_right_click;     // CCastSpellWnd::HandleSpellRightClick
    // Lua socials (game/lua_social.h, mq::ScriptHost): a press runs its Lua; a right click stops it (true when it
    // ran) or opens its editor; forget drops a social's Lua state (-1: every social's).
    std::function<void(std::int32_t social, int bar, int button)> press_lua_social; // bar -1: not on a bar
    std::function<bool(std::int32_t social)> stop_lua_social;
    std::function<void(std::int32_t social)> edit_lua_social;
    std::function<void(std::int32_t social)> forget_lua_social;
};

class ActionsUi {
public:
    ActionsUi(Config& config, std::filesystem::path config_path);
    ~ActionsUi();

    game::KeyMap& keymap() { return keymap_; }
    // The key bindings read again (a character's settings took over, Config::attach_character).
    void reload_keymap();
    // The options window's Keys page (COptionsWnd::KeyboardPageWndNotification / sub_0070abc0): the next key pressed
    // goes to the command's bank (0 Keypress, 1 Alternate); with escape_clears, Escape unbinds it (the original
    // attaches an empty combo for Escape). The binding is saved; a reserved key changes nothing.
    void capture_key(int command, int bank, bool escape_clears = false) { capture_ = {command, bank, escape_clears}; }
    std::pair<int, int> key_capture() const { return {capture_.command, capture_.bank}; }
    void cancel_key_capture() { capture_ = {}; }
    // ResetKeysToDefault: the default bindings (UseWASDDefault), saved.
    void reset_keys();
    game::HotButtonStore& hot_buttons() { return store_; }
    // A button put on bar 0..9, button 0..11 of the bar's current page (dropped from the cursor), saved.
    void set_hot_button(int bar, int button, const game::HotButton& b) {
        if (bar < 0 || bar >= game::kHotBars || button < 0 || button >= game::kHotButtons) return;
        store_.current(bar, button) = b;
        changed();
    }
    // Any button of any page as its ini string ("" empty) for the plugins (PluginHostApi hot_button / set_hot_button;
    // nullopt before a character's file is loaded or out of range), and a bar's page; a change is saved.
    std::optional<std::string> hot_button_spec(int bar, int page, int button) const;
    bool set_hot_button_spec(int bar, int page, int button, const std::string& spec);
    int hot_bar_page(int bar) const { return bar >= 0 && bar < game::kHotBars ? store_.page(bar) : -1; }
    void set_hot_bar_page(int bar, int page) {
        if (bar < 0 || bar >= game::kHotBars) return;
        store_.set_page(bar, page); // (not saved: the character file keeps the buttons, no bar's page)
    }
    // The Actions window's ability buttons (Abilities 0..5, Combat 0..3): the skill on one (-1 clears it), saved with
    // the character's hot buttons; and a skill's name as the buttons show it.
    void set_ability_button(bool combat, int slot, std::int32_t skill) {
        std::span<std::int32_t> a = combat ? std::span<std::int32_t>(store_.combat_skills)
                                           : std::span<std::int32_t>(store_.abilities);
        if (slot < 0 || slot >= static_cast<int>(a.size())) return;
        a[static_cast<std::size_t>(slot)] = skill;
        changed();
    }
    std::string skill_label(net::GameConnection& conn, std::uint32_t skill) const { return skill_name(conn, skill); }
    // /hotbutton (Cmd_hotbutton @0x4f79b0): 0x4ea9f0(name, line, colour, -1, 1) on the character's socials, saved;
    // the social's index or -1.
    int store_social(std::string_view name, std::string_view line, std::int32_t color) {
        int index = game::store_social(store_, name, line, color, -1, true);
        if (index >= 0) changed();
        return index;
    }
    // The combat ability window's button 0..7 (HotButtonStore::combat_abilities: 0x84 + the ability's index, -1
    // empty), saved (SaveCombatAbilitiesToIni).
    void set_combat_ability_button(int button, std::int32_t value) {
        if (button < 0 || button >= game::kCombatAbilityButtons) return;
        store_.combat_abilities[static_cast<std::size_t>(button)] = value;
        changed();
    }
    // 0x659c80 (a left click on CAW_Button<n + 1>; the keys CMD_OPENING_ATTACK1..8): a button holding an ability
    // runs PcZoneClient::doCombatAbility(GetCombatAbility(value - 0x84), 1). False for an empty one.
    bool use_combat_ability_button(net::GameConnection& conn, int button);
    // The target is within __get_melee_range of the player (the range CanIHit tests; the Ability Book's autoskill).
    bool target_in_melee_range(net::GameConnection& conn, const game::mechanics::Character& ch, const AbilityView& view);
    game::mechanics::SkillState& skill_state() { return state_; }
    // The compass's heading error (CCompassWnd, game/compass.h); a successful Sense Heading resets it.
    game::CompassError& compass() { return compass_; }

    // The key events of a frame through the key map (HandleKeyDown / HandleKeyUp). With keyboard_free false (chat
    // input has the keyboard) held commands are released and only the UI visibility toggle can start. A key
    // binding being captured in the key window takes the next key instead, including the visibility shortcut.
    std::vector<game::KeyMap::Event> process_keys(const platform::InputState& in, bool keyboard_free);

    // Loads the character's hotbuttons.ini (CharacterFiles::hot_buttons(); once per character) and resets the skill
    // state for a new zone.
    void enter_zone(const std::filesystem::path& hot_buttons_ini);
    // The character ini (<name>_<server>.ini, this client's hotbuttons.ini) for other users of it:
    // CharacterIni_GetInt, and CharacterIni_WriteString (written at once).
    int char_ini_int(std::string_view section, std::string_view key, int fallback) const {
        return char_ini_.get_int(section, key, fallback);
    }
    // CharacterIni_GetString: the key's text, empty when missing.
    std::string char_ini_string(std::string_view section, std::string_view key) const {
        auto v = char_ini_.get(section, key);
        return v ? *v : std::string();
    }
    // The character whose file is loaded (its hotbuttons.ini; empty before the first zone-in).
    const std::filesystem::path& char_ini_path() const { return char_ini_path_; }
    void write_char_ini(std::string_view section, std::string_view key, const std::string& value);
    // __CopyLayout's 0x55b930: every key of `sections` in `from` (another character's file) written into this
    // character's (CharacterIni_WriteString), then, with `reload`, the hot buttons and socials read again
    // (LoadCharacterIni).
    void copy_char_ini_sections(const IniFile& from, std::initializer_list<std::string_view> sections, bool reload);
    // Per frame: skill timers, the hide-while-moving check, fishing, and the server's answers.
    void update(net::GameConnection& conn, const game::mechanics::Character* ch, const AbilityView& view);
    // DoPassageOfTime's attack part for this frame, before ProcessControls (game/mechanics/melee.h): the attack-on
    // reset, auto attack off when the player is not standing (__ExecuteCmd AUTOPRIM), the swing-ready flags.
    void melee_timers(net::GameConnection& conn, const game::mechanics::Character* ch, const AbilityView& view);
    // PlayerZoneClient::DoAttack(slot, 0, pinstTarget) for one of ProcessControls' ready flags (13 primary, 14
    // secondary): the refusals' messages and the swing animations.
    void melee_attack(net::GameConnection& conn, const game::mechanics::Character* ch, const AbilityView& view,
                      std::int32_t slot);
    // The swing timers and the two ready flags (0xde0864 / 0xde0865) ProcessControls reads and clears.
    game::mechanics::melee::AttackState& attack_state() { return state_.attack; }
    // The hot bar, the Actions window and the key window.
    void draw(net::GameConnection& conn, const game::mechanics::Character* ch, const AbilityView& view);

    // CHotButtonWnd::DoHotButton for bar 0..9, button 0..11 of its current page (game/hot_button_press.h);
    // `mouse`: a left click on the button (items of types 5 and 13 then do nothing).
    void hot_button(net::GameConnection& conn, const game::mechanics::Character* ch, const AbilityView& view, int bar,
                    int button, bool mouse = false);
    // 0x54fc10: a right click on a hot button (the socials' stop and cooldown; items' right click).
    void hot_button_right_click(net::GameConnection& conn, int bar, int button);
    void set_hooks(HotButtonHooks h) { hooks_ = std::move(h); }
    // The skill a button of types 2, 3 or 9 uses (2 / 3: the [CombatSkills] / [Abilities] list entry), -1 none.
    std::int32_t button_skill(const game::HotButton& b) const;
    // /stopsocial: Social_Stop(-1, 0).
    void stop_social() { game::social_stop(social_queue_, -1, false); }
    // The social editor's Test: what a hot button's press of social 0..119 does (0x6143a0 queues it; a Lua
    // social runs its Lua instead). The queued social, -1 none.
    void press_social(std::int32_t social);
    std::int32_t current_social() const { return social_queue_.current; }
    // The social editor changed a social: the socials are saved (SaveSocialsToIni).
    void socials_changed() { changed(); }
    // CharacterZoneClient::UseSkill, then the packets and messages.
    void use_skill(net::GameConnection& conn, const game::mechanics::Character* ch, const AbilityView& view,
                   std::uint32_t skill);
    // Cmd_doability @0x4e8aa0 (/doability n): buttons 1..6 are the Abilities page's ([Abilities]), 7..10 the Combat
    // page's ([CombatSkills]); an empty one does nothing; any other number prints its usage (13256 and 13257,
    // colour 0x111). UseSkill on the target. Hailstone's extension: a name instead of a number uses the player's
    // ability of that name through `use_named` (app::use_ability_named: a skill, then a discipline, then an activated
    // AA); none by that name, or no `use_named`, prints "No ability by that name.".
    // `use_named`: nullopt when the player has no ability of that name, else whether it was used.
    using NamedAbility = std::function<std::optional<bool>(std::string_view name)>;
    void doability_command(net::GameConnection& conn, const game::mechanics::Character* ch, const AbilityView& view,
                           const std::string& args, const NamedAbility& use_named = {});
    void set_page(int bar, int page);
    // Runs one button through DoHotButton without it being on a bar (the Actions window's buttons).
    void run_button(net::GameConnection& conn, const game::mechanics::Character* ch, const AbilityView& view,
                    const game::HotButton& b);
    void close_actions() { actions_open_ = false; }
    // The text a hotbutton shows: its label, else the skill / spell / social / menu name.
    std::string button_label(net::GameConnection& conn, const game::HotButton& b) const;
    void toggle_hotbar() { hotbar_open_ = !hotbar_open_; }
    // The Actions window: 0 main, 1 abilities, 2 combat, 3 socials; toggles when that page already shows.
    void toggle_actions(int page);
    void toggle_keys() { keys_open_ = !keys_open_; }
    bool close_top_window(); // CLOSE_TOP_WINDOW: the key window, then the Actions window

    // Chat lines produced since the last call (text, eqstr colour).
    std::vector<std::pair<std::string, std::uint32_t>> take_messages();
    // The melee attack button asks the client to toggle attack (and face the target).
    bool take_attack_toggle();
    // Saves the character file now, when anything changed. Changes are also saved by update(), at most every
    // kSaveDelayMs (save_if_due): a drag, the social editor or a plugin's edits do not rewrite the file (and the Lua
    // socials folder) each frame. Zoning (enter_zone), leaving the character and the end save at once.
    void save_character();
    static constexpr std::int64_t kSaveDelayMs = 3000;
    // Saves when a change has waited kSaveDelayMs (on GameConnection::now_ms()'s clock); whether it saved.
    bool save_if_due(std::int64_t now_ms);
    bool unsaved() const { return dirty_; }
    // Hotbutton sets (game/hot_button_sets.h, CHotButtonWnd's menu; app/actions_ui_sets.cpp): the sets, and the
    // menu's Save (the text entry's OK), Load and Delete (their confirmations' Yes) and Clear current hotbuttons
    // for bar 0..9, each with the original's chat line; the character file is written at once.
    const game::HotButtonSets& hot_button_sets() const { return sets_; }
    void save_hot_button_set(net::GameConnection& conn, int bar, const std::string& name);
    void load_hot_button_set(net::GameConnection& conn, int bar, int n);
    void delete_hot_button_set(net::GameConnection& conn, int n);
    void clear_hot_bar(net::GameConnection& conn, int bar);
    // A social was changed (its name, lines or Lua): saved with the character, its Lua state dropped.
    void social_changed(int social) {
        changed();
        if (hooks_.forget_lua_social) hooks_.forget_lua_social(social);
    }
    std::filesystem::path lua_socials_dir() const {
        return char_ini_path_.empty() ? std::filesystem::path() : char_ini_path_.parent_path() / "socials";
    }
    // Testing (--abilities-test): put these skills on hot bar 1 page 1 and open the windows.
    void setup_test_buttons(const std::vector<std::uint32_t>& skills);
    void open_all() {
        hotbar_open_ = actions_open_ = keys_open_ = true;
        actions_page_ = 2; // Combat
        actions_page_forced_ = true;
    }

private:
    game::mechanics::SkillContext context(net::GameConnection& conn, const game::mechanics::Character& ch,
                                          const AbilityView& view) const;
    void message(net::GameConnection& conn, const game::mechanics::SkillMessage& m);
    std::string skill_name(net::GameConnection& conn, std::uint32_t skill) const;
    void draw_hotbar(net::GameConnection& conn, const game::mechanics::Character* ch, const AbilityView& view);
    void draw_actions(net::GameConnection& conn, const game::mechanics::Character* ch, const AbilityView& view);
    void draw_keys(net::GameConnection& conn);
    void hot_button_menu(net::GameConnection& conn, const game::mechanics::Character* ch, game::HotButton& b);
    void changed() { dirty_ = true; }
    void save_now(); // the character file written at once (SaveHotButtonsToIni, SaveHotButtonLoadoutsToIni)
    void run_socials(net::GameConnection& conn);

    Config& config_;
    std::filesystem::path config_path_;
    game::KeyMap keymap_;
    game::HotButtonStore store_;
    game::HotButtonSets sets_;
    IniFile char_ini_;
    std::filesystem::path char_ini_path_;
    std::string loaded_for_;
    bool dirty_ = false;
    std::optional<std::int64_t> dirty_seen_ms_; // when save_if_due first saw the change waiting
    bool loadouts_fresh_ = false; // the spell sets are read into the connection on the next update
    game::mechanics::SkillState state_;
    game::CompassError compass_;
    std::uint32_t clock_start_ = 0;

    HotButtonHooks hooks_;
    game::SocialQueue social_queue_; // __CurrentSocial and DoPassageOfTime's social counters
    std::uint8_t toggle_89c_ = 0;          // 0xddf89c

    bool hotbar_open_ = true, actions_open_ = false, keys_open_ = false;
    int actions_page_ = 1;
    bool actions_page_forced_ = false;
    int social_page_ = 0;
    struct Capture {
        int command = -1;
        int bank = 0;
        bool escape_clears = false;
    } capture_;
    std::uint32_t key_category_ = 0; // 0 = all
    std::vector<std::pair<std::string, std::uint32_t>> messages_;
    bool attack_toggle_ = false;
};

} // namespace hailstone::app
