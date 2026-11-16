#include "app/actions_ui.h"
#include "ui/lua/lua_host.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <cmath>
#include <random>

#include <imgui.h>

#include "core/config.h"
#include "core/log.h"
#include "core/msvc_atoi.h"
#include "game/coords.h"
#include "game/eq_strings.h"
#include "game/hot_button_menu.h"
#include "game/mechanics/attack_rules.h"
#include "game/mechanics/character.h"
#include "game/mechanics/tables.h"
#include "game/race_models.h"
#include "game/tracking.h"
#include "app/skill_race.h"
#include "net/ability_packets.h"
#include "net/game_connection.h"
#include "net/notice_packets.h"
#include "net/packet_io.h"
#include "net/zone_packets.h"
#include "platform/paths.h"
#include "platform/window.h"

namespace hailstone::app {

namespace mech = game::mechanics;
using game::HotButton;
using game::HotButtonType;

namespace {

constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
constexpr const char* kDragType = "HAILSTONE_HOTBUTTON";

bool is_modifier(std::uint8_t code) {
    return code == 0x2a || code == 0x36 || code == 0x1d || code == 0x9d || code == 0x38 || code == 0xb8;
}

std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

const char* menu_button_name(std::int32_t slot) {
    switch (static_cast<game::MenuButton>(slot)) {
    case game::MenuButton::Who: return "Who";
    case game::MenuButton::InviteFollow: return "Invite";
    case game::MenuButton::Disband: return "Disband";
    case game::MenuButton::Camp: return "Camp";
    case game::MenuButton::Sit: return "Sit";
    case game::MenuButton::RunWalk: return "Walk";
    case game::MenuButton::Options: return "Options";
    case game::MenuButton::ToggleView: return "View";
    case game::MenuButton::Inventory: return "Inventory";
    case game::MenuButton::SpellBook: return "Spells";
    case game::MenuButton::Find: return "Find";
    }
    return "?";
}

constexpr std::int32_t kMenuButtons[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 12};

// A skill button (type 9: UseSkill of the skill id; types 2 / 3 index the Actions window's lists).
HotButton skill_button(std::uint32_t skill, bool) {
    HotButton b;
    b.type = HotButtonType::Skill;
    b.slot = static_cast<std::int32_t>(skill);
    return b;
}

// The button a drag carries (only one drag at a time).
HotButton& dragged() {
    static HotButton b;
    return b;
}

std::string seconds_text(std::uint32_t ms) {
    std::uint32_t s = (ms + 999) / 1000;
    if (s >= 60) return std::to_string(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + std::to_string(s % 60);
    return std::to_string(s);
}

} // namespace

ActionsUi::ActionsUi(Config& config, std::filesystem::path config_path)
    : config_(config), config_path_(std::move(config_path)) {
    reload_keymap();
}

void ActionsUi::reload_keymap() {
    keymap_.reset_defaults(config_.get_bool("UseWASDDefault", true));
    keymap_.load(config_);
}

ActionsUi::~ActionsUi() { save_character(); }

void ActionsUi::press_social(std::int32_t social) {
    if (social < 0 || social >= game::kSocialCount) return;
    if (store_.social(social).is_lua()) {
        if (hooks_.press_lua_social) hooks_.press_lua_social(social, -1, -1);
        return;
    }
    social_queue_.current = social; // (the line and pause counters stay, as a hot button's press leaves them)
}

std::vector<game::KeyMap::Event> ActionsUi::process_keys(const platform::InputState& in, bool keyboard_free) {
    std::vector<game::KeyMap::Event> out;
    auto combo = [&](std::uint8_t code) {
        game::KeyCombo k;
        k.code = code;
        k.shift = in.key_down[0x2a] || in.key_down[0x36];
        k.ctrl = in.key_down[0x1d] || in.key_down[0x9d];
        k.alt = in.key_down[0x38] || in.key_down[0xb8];
        return k;
    };
    for (const platform::KeyEvent& ev : in.key_events) {
        if (capture_.command >= 0) {
            if (!ev.down || ev.repeat || is_modifier(ev.code)) continue;
            if (ev.code != 0x01 || capture_.escape_clears) { // Escape cancels (the Keys page: unbinds)
                if (keymap_.bind(capture_.command, capture_.bank, ev.code == 0x01 ? game::KeyCombo{} : combo(ev.code))) {
                    keymap_.save(config_);
                    config_.save(config_path_);
                } else {
                    messages_.push_back({"That key is reserved.", 0xd});
                }
            }
            capture_ = {};
            continue;
        }
        if (ev.repeat) continue;
        if (!keyboard_free) {
            // The remappable UI visibility key also works while an edit box or modal owns the keyboard.
            // Capturing a new binding takes priority above; every other game command stays blocked.
            const auto key = combo(ev.code);
            const int toggle = static_cast<int>(game::KeyCommand::FULLSCREEN);
            const auto matches = [&](int bank) {
                const auto& binding = keymap_.binding(toggle, bank);
                return binding.bound() && binding == key;
            };
            if (ev.down && (matches(0) || matches(1))) out.push_back({toggle, true});
            continue;
        }
        if (ev.down) keymap_.key_down(combo(ev.code), out);
        else keymap_.key_up(ev.code, out);
    }
    if (!keyboard_free) keymap_.release_all(out);
    // Keys released while the chat had the keyboard still end their commands.
    if (keyboard_free)
        for (std::size_t c = 0; c < platform::kKeyCodes; ++c)
            if (in.key_released[c]) keymap_.key_up(static_cast<std::uint8_t>(c), out);
    return out;
}

void ActionsUi::enter_zone(const std::filesystem::path& hot_buttons_ini) {
    std::string who = hot_buttons_ini.string();
    state_.sneaking = state_.hidden = state_.fishing = false;
    state_.activity = -1;
    loadouts_fresh_ = true;
    save_character(); // what waited for save_if_due, before the zone (or the next character's file)
    if (who == loaded_for_) return;
    // Another character: the running social (an index into the last one's socials) and its counters stop, as
    // /stopsocial does. The same character zoning leaves it as it is.
    stop_social();
    loaded_for_ = who;
    char_ini_path_ = hot_buttons_ini;
    char_ini_.load(char_ini_path_);
    store_.load(char_ini_);
    sets_.load(char_ini_);
    store_.load_lua(lua_socials_dir());
    if (hooks_.forget_lua_social) hooks_.forget_lua_social(-1); // another character's socials
    state_ = {};
    log::info("hot buttons: {}", char_ini_path_.string());
}

void ActionsUi::write_char_ini(std::string_view section, std::string_view key, const std::string& value) {
    char_ini_.set(section, key, value);
    if (char_ini_path_.empty()) return;
    store_.save(char_ini_);
    if (!char_ini_.save(char_ini_path_)) log::warn("cannot write {}", char_ini_path_.string());
    dirty_ = false;
}

void ActionsUi::copy_char_ini_sections(const IniFile& from, std::initializer_list<std::string_view> sections,
                                       bool reload) {
    store_.save(char_ini_);
    for (std::string_view section : sections)
        for (const std::string& key : from.keys(section))
            if (auto v = from.get(section, key)) char_ini_.set(section, key, *v);
    if (reload) store_.load(char_ini_);
    sets_.load(char_ini_); // ([HBLoadouts] may be one of them)
    if (!char_ini_path_.empty() && !char_ini_.save(char_ini_path_)) log::warn("cannot write {}", char_ini_path_.string());
    dirty_ = false;
}

std::optional<std::string> ActionsUi::hot_button_spec(int bar, int page, int button) const {
    if (loaded_for_.empty() || bar < 0 || bar >= game::kHotBars || page < 0 || page >= game::kHotPages || button < 0 ||
        button >= game::kHotButtons)
        return std::nullopt;
    const game::HotButton& b = store_.at(bar, page, button);
    return b.empty() ? std::string() : game::format_hot_button(b);
}

bool ActionsUi::set_hot_button_spec(int bar, int page, int button, const std::string& spec) {
    if (!hot_button_spec(bar, page, button)) return false;
    store_.at(bar, page, button) = spec.empty() ? game::HotButton{} : game::parse_hot_button(spec);
    changed();
    return true;
}

bool ActionsUi::save_if_due(std::int64_t now_ms) {
    if (!dirty_) {
        dirty_seen_ms_.reset();
        return false;
    }
    if (!dirty_seen_ms_) dirty_seen_ms_ = now_ms;
    if (now_ms - *dirty_seen_ms_ < kSaveDelayMs) return false;
    save_character();
    return true;
}

void ActionsUi::save_character() {
    dirty_seen_ms_.reset();
    if (!dirty_ || char_ini_path_.empty()) return;
    store_.save(char_ini_);
    sets_.save(char_ini_);
    if (!char_ini_.save(char_ini_path_)) log::warn("cannot write {}", char_ini_path_.string());
    if (!store_.save_lua(lua_socials_dir())) log::warn("cannot write the Lua socials in {}", lua_socials_dir().string());
    dirty_ = false;
}

std::vector<std::pair<std::string, std::uint32_t>> ActionsUi::take_messages() {
    auto out = std::move(messages_);
    messages_.clear();
    return out;
}

bool ActionsUi::take_attack_toggle() {
    bool t = attack_toggle_;
    attack_toggle_ = false;
    return t;
}

void ActionsUi::set_page(int bar, int page) {
    store_.set_page(bar, page);
}

void ActionsUi::toggle_actions(int page) {
    if (actions_open_ && actions_page_ == page) {
        actions_open_ = false;
        return;
    }
    actions_open_ = true;
    actions_page_ = page;
    actions_page_forced_ = true;
}

bool ActionsUi::close_top_window() {
    if (keys_open_) {
        keys_open_ = false;
        capture_ = {};
        return true;
    }
    if (actions_open_) {
        actions_open_ = false;
        return true;
    }
    return false;
}

void ActionsUi::message(net::GameConnection& conn, const mech::SkillMessage& m) {
    std::vector<std::string> args;
    for (const std::string& a : m.args) {
        // "@<id>": an eqstr text (the original copies getString(id) into the argument).
        if (!a.empty() && a[0] == '@') args.push_back(conn.strings().format(static_cast<std::uint32_t>(std::stoul(a.substr(1))), {}));
        else args.push_back(a);
    }
    messages_.push_back({conn.strings().format(m.eqstr, args), m.color});
    log::info("skill: {}", messages_.back().first);
}

std::string ActionsUi::skill_name(net::GameConnection& conn, std::uint32_t skill) const {
    if (skill == mech::skill::kSlam) return "Slam";
    std::uint32_t id = mech::skill_name_eqstr(skill);
    return id ? conn.strings().format(id, {}) : "Skill " + std::to_string(skill);
}

std::string ActionsUi::button_label(net::GameConnection& conn, const HotButton& b) const {
    if (!b.label.empty()) return b.label;
    switch (b.type) {
    case HotButtonType::WeaponSlot: return b.slot == 0 ? "Melee Attack" : "Range Attack";
    case HotButtonType::CombatSkill:
    case HotButtonType::Ability:
    case HotButtonType::Skill: {
        std::int32_t s = button_skill(b);
        return s >= 0 ? skill_name(conn, static_cast<std::uint32_t>(s)) : "";
    }
    case HotButtonType::Social:
        return b.slot >= 0 && b.slot < game::kSocialPages * game::kSocialButtons ? store_.social(b.slot).name : "";
    case HotButtonType::MenuButton: return menu_button_name(b.slot);
    case HotButtonType::SpellGem: {
        const auto& gems = conn.spell_state().gems;
        if (b.slot >= 0 && b.slot < static_cast<int>(gems.size())) {
            std::uint32_t spell = gems[static_cast<std::size_t>(b.slot)].spell;
            if (const auto* s = conn.spell_table().get(spell)) return s->name;
        }
        return "Gem " + std::to_string(b.slot + 1);
    }
    default: return "";
    }
}

mech::SkillContext ActionsUi::context(net::GameConnection& conn, const mech::Character& ch,
                                      const AbilityView& view) const {
    mech::SkillContext ctx;
    ctx.ch = &ch;
    const auto& world = conn.world();
    const auto& ps = conn.player_status();
    auto actor = [&](const game::Spawn& s, const glm::vec3& world_pos, float heading) {
        mech::SkillActor a;
        a.id = s.id;
        a.type = s.is_corpse ? 2 : s.is_npc ? 1 : 0;
        glm::vec3 p = game::world_to_server(world_pos);
        a.x = p.x;
        a.y = p.y;
        a.z = p.z;
        a.heading = heading;
        a.scale = s.heights.model_scale;          // +0x31c (SetHeights)
        a.avatar_height = s.heights.avatar_height; // +0x138
        a.melee_radius = s.melee_radius;
        a.level = s.level;
        a.is_pet = s.pet_owner_id != 0;
        a.body_wet = s.body_wet;
        a.pvp_flag = s.pvp != 0;
        a.gm = s.gm;
        a.name = game::display_name(s.name);
        return a;
    };
    if (const game::Spawn* me = world.player()) ctx.me = actor(*me, view.position, view.heading);
    ctx.me.level = ps.level;
    if (auto it = world.spawns.find(world.target_id); world.target_id != 0 && it != world.spawns.end()) {
        const game::Spawn& t = it->second;
        bool self = it->first == world.player_id;
        ctx.target = self ? ctx.me
                          : actor(t, t.display_valid ? t.display_position : t.position,
                                  t.display_valid ? t.display_heading : t.heading);
        ctx.target_is_me = self;
        // PlayerZoneClient::NotOnSameDeity (PvP rule set 4): pick pockets and bind wound ask it.
        if (const game::Spawn* me = world.player(); me && !self) {
            mech::AttackSpawn a, b;
            a.type = 0;
            a.id = me->id;
            a.deity = me->deity;
            b.type = t.is_corpse ? 2 : t.is_npc ? 1 : 0;
            b.id = t.id;
            b.deity = t.deity;
            ctx.target->same_deity_ok = !mech::not_on_same_deity(&a, &b, false, t.gm);
        }
    }
    ctx.pvp_ruleset = static_cast<std::uint8_t>(conn.pvp_rules());
    ctx.tracking_window_open = conn.tracking().open;
    ctx.stand_state = ps.stand_state;
    // Cur_HP, the items' HP included (GameConnection::own_hp).
    ctx.cur_hp = ps.dead ? 0 : (ps.max_hp > 0 ? static_cast<std::int32_t>(conn.own_hp().cur) : 1);
    if (ch.sheet().vitals.cur_hp) ctx.cur_hp = ch.cur_hp(true, 0); // Cur_HP as a server's client patch answers it
    ctx.casting = conn.spell_state().casting.casting();
    ctx.is_bard = ch.sheet().class_id == 8;
    ctx.attacking = conn.auto_attack();
    ctx.autofire = conn.auto_fire();
    glm::vec3 v = game::world_to_server(view.velocity);
    ctx.speed_x = v.x;
    ctx.speed_y = v.y;
    ctx.speed_z = v.z;
    ctx.cursor_has_item = conn.inventory().at(net::ItemSlot::possessions(net::kSlotCursor)) != nullptr;
    ctx.can_see_target = view.can_see_target;
    ctx.form_slam = mech::slam_innate(skill_race(conn, ch)); // current form; illusions can grant/lose Slam
    if (ctx.target && !ctx.target_is_me) ctx.allowed_to_attack = conn.allowed_to_attack(world.target_id, -1);
    ctx.attack_locked = conn.attack_locked();
    ctx.backstab_either_hand = conn.server_ext().backstab_either_hand(conn);
    ctx.char_state = conn.stunned() ? 1 : 0; // CharacterBase +0x100: 1 stunned
    ctx.buff_slots_with_spa = [&conn](std::int32_t spa) { return conn.buff_slots_with_spa(spa); };
    ctx.now = static_cast<std::uint32_t>(net::GameConnection::now_ms());
    ctx.tick_count = ctx.now;
    ctx.random = [] { return static_cast<std::int32_t>(rng()() & 0x3fffffff); }; // Random(): the additive RNG & 0x3fffffff
    ctx.random_range = [](std::int32_t lo, std::int32_t hi) {
        return std::uniform_int_distribution<std::int32_t>(lo, hi)(rng());
    };
    return ctx;
}

bool ActionsUi::target_in_melee_range(net::GameConnection& conn, const mech::Character& ch, const AbilityView& view) {
    mech::SkillContext ctx = context(conn, ch, view);
    if (!ctx.target || ctx.target_is_me) return false;
    return std::hypot(ctx.target->x - ctx.me.x, ctx.target->y - ctx.me.y) <= mech::melee_range(&ctx.me, &*ctx.target);
}

bool ActionsUi::use_combat_ability_button(net::GameConnection& conn, int button) {
    if (button < 0 || button >= game::kCombatAbilityButtons) return false;
    const std::int32_t value = store_.combat_abilities[static_cast<std::size_t>(button)];
    if (value <= 0) return false;
    const std::uint32_t spell = conn.combat_ability(value - game::kCombatAbilityValueBase);
    if (!conn.spell_table().get(spell)) return false; // SpellManager::GetSpellByID(0) is none
    conn.do_combat_ability(spell, true);
    return true;
}

void ActionsUi::doability_command(net::GameConnection& conn, const mech::Character* ch, const AbilityView& view,
                                  const std::string& args, const NamedAbility& use_named) {
    // _atoi(args) - 1: 0..5 index the [Abilities] list (0xe15e60), 6..9 the four dwords before it, the
    // [CombatSkills] list (0xe15e50; the original indexes 0xe15e38 + 4 * n).
    // In 64 bits: "-2147483648" - 1 overflowed.
    std::string_view name = args;
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.remove_prefix(1);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.remove_suffix(1);
    // Hailstone's extension: anything that does not start as a number is the name of one of the player's abilities,
    // used as the Ability Book's row of that name is (the plugins' use_ability, by name, in any case): a skill first,
    // then a discipline, then an activated AA. RoF2's /doability takes only a button number, which stays as it was.
    const bool numeric = name.empty() || std::isdigit(static_cast<unsigned char>(name.front())) ||
                         name.front() == '-' || name.front() == '+';
    if (!numeric) {
        const std::optional<bool> used = use_named ? use_named(name) : std::nullopt;
        // Found but refused for the moment (not in the world): nothing to say, as its hot button says nothing.
        if (!used.has_value()) messages_.push_back({"No ability by that name.", 0x111});
        return;
    }
    const std::int64_t wide = std::int64_t{core::msvc_atoi(args)} - 1;
    if (wide < 0 || wide >= 10) {
        messages_.push_back({conn.strings().format(13256, {}), 0x111}); // Format: /doability 1 - 10
        messages_.push_back({conn.strings().format(13257, {}), 0x111}); // (1 - 6 are abilities, 7 - 10 ...)
        return;
    }
    const auto button = static_cast<int>(wide);
    const std::int32_t skill = button < 6 ? store_.abilities[static_cast<std::size_t>(button)]
                                          : store_.combat_skills[static_cast<std::size_t>(button - 6)];
    if (skill == -1) return;
    use_skill(conn, ch, view, static_cast<std::uint8_t>(skill)); // CharacterZoneClient::UseSkill(low byte, target)
}

void ActionsUi::use_skill(net::GameConnection& conn, const mech::Character* ch, const AbilityView& view,
                          std::uint32_t skill) {
    if (!ch) return;
    // A server extension may refuse the skill first (it says why).
    if (auto allowed = conn.server_ext().before_use_skill(conn, skill); allowed && !*allowed) return;
    // Tracking's skill check goes through the server extension (game/server_ext.h; stock: game::tracking_allowed()).
    if (skill == mech::skill::kTracking && !conn.tracking_admitted())
        return;
    mech::SkillContext ctx = context(conn, *ch, view);
    mech::SkillResult r = mech::use_skill(ctx, state_, skill);
    for (const auto& m : r.messages) message(conn, m);
    for (const auto& s : r.sends) {
        // UseSkill's 114..116 call the commands themselves (Send_OP_LDoNInspect, Send_OP_LDoNOpen, Cmd_reveal),
        // with their target and distance checks.
        using K = mech::SkillSend::Kind;
        if (!s.raw && s.kind == K::LDoNInspect) conn.inspect_target();
        else if (!s.raw && s.kind == K::LDoNOpen) conn.ldon_open();
        else if (!s.raw && s.kind == K::Reveal) conn.reveal_target();
        else if (auto p = net::encode_skill_send(s)) conn.send_zone_packet(*p);
    }
    if (r.stand_state) conn.set_stand_state(*r.stand_state); // unk_SetStandState
    conn.note_hide_state(state_.hidden, state_.sneaking);
    if (r.stop_attack) conn.set_auto_attack(false);
    if (r.stop_auto_fire) conn.set_auto_fire(false);
    for (int slot : r.removed_buffs) conn.clear_buff_locally(slot);
    if (r.compass_reset) compass_.sense_heading();
    if (r.open_tracking_window) conn.open_tracking(true);  // the skill's admission was checked above
    else if (r.track_request) conn.send_track();
    log::info("UseSkill {} (value {}, adjusted {}): {} sends, {} messages; target {} at {:.2f} (melee range {:.2f})", skill,
              skill < 100 ? ch->sheet().skills[skill] : 0, skill < 100 ? ch->adjusted_skill(skill) : 0, r.sends.size(),
              r.messages.size(), ctx.target ? ctx.target->name : std::string("none"),
              ctx.target ? std::hypot(ctx.target->x - ctx.me.x, ctx.target->y - ctx.me.y) : 0.0f,
              ctx.target ? mech::melee_range(&ctx.me, &*ctx.target) : 0.0f);
}

namespace {

// CHotButtonWnd::DoHotButton's callees over the client (game/hot_button_press.h).
class PressQueries final : public game::HotButtonQueries {
public:
    using UseSkillFn = std::function<void(std::uint32_t)>;
    PressQueries(net::GameConnection& c, const game::HotButtonStore& store, game::SocialQueue& socials,
                 const HotButtonHooks& hooks, std::vector<std::pair<std::string, std::uint32_t>>& messages,
                 bool& attack_toggle, UseSkillFn use_skill, std::function<void()> ranged)
        : c_(c), store_(store), socials_(socials), hooks_(hooks), messages_(messages), attack_toggle_(attack_toggle),
          use_skill_(std::move(use_skill)), ranged_(std::move(ranged)) {}

    void cant_use_command() override { messages_.push_back({c_.strings().format(game::kStrCantUseCommand, {}), 0x111}); }
    void must_be_stopped() override { messages_.push_back({c_.strings().format(game::kStrMustBeStopped, {}), 0x111}); }
    void execute_cmd(std::uint32_t command) override {
        if (command == game::kCmdAutoPrimary) attack_toggle_ = true; // the client's AUTOPRIM toggles and faces
        else if (hooks_.execute_cmd) hooks_.execute_cmd(command);
    }
    void ranged_attack() override {
        if (ranged_) ranged_();
    }
    std::uint32_t list_entry(std::uint8_t type, std::int32_t index) override {
        if (type == 2) return index >= 0 && index < 4 ? static_cast<std::uint32_t>(store_.combat_skills[static_cast<std::size_t>(index)]) : 0xffffffffu;
        return index >= 0 && index < 6 ? static_cast<std::uint32_t>(store_.abilities[static_cast<std::size_t>(index)]) : 0xffffffffu;
    }
    void use_skill(std::uint8_t skill) override {
        if (skill != 0xff) use_skill_(skill);
    }
    std::int32_t combat_ability(std::int32_t index) override { return static_cast<std::int32_t>(c_.combat_ability(index)); }
    bool spell_exists(std::int32_t spell) override { return spell > 0 && c_.spell_table().get(static_cast<std::uint32_t>(spell)); }
    void do_combat_ability(std::int32_t spell) override { c_.do_combat_ability(static_cast<std::uint32_t>(spell)); }
    void set_current_social(std::int32_t social) override {
        // A Lua social runs its Lua instead of being queued (game/lua_social.h).
        if (social >= 0 && social < game::kSocialCount && store_.social(social).is_lua() && hooks_.press_lua_social) {
            hooks_.press_lua_social(social, bar, button);
            return;
        }
        socials_.current = social;
    }
    void leadership_ability(std::int32_t ability, std::int32_t parameter) override {
        c_.activate_leadership_ability(ability, parameter); // Leadership_ActivateAbility(value / 8, value % 8)
    }
    std::optional<game::HotButtonSlot> find_inv_slot(std::int32_t slot, std::int32_t bag) override {
        slot_ = {slot, bag};
        return game::HotButtonSlot{true, 0, 0};
    }
    std::optional<game::HotButtonSlot> find_krono_slot() override {
        slot_ = {23, -1}; // the slot of location 23
        return game::HotButtonSlot{true, 0, 0};
    }
    void inv_slot_right_click(std::int32_t, std::int32_t) override {
        if (hooks_.use_item) hooks_.use_item(slot_.first, slot_.second);
    }
    void who() override { c_.send_command("/who"); }
    void group_follow() override { c_.accept_group_invite(); }
    void group_invite() override { c_.group_invite(); }
    void disband() override { c_.send_command("/disband"); }
    void camp() override { c_.camp(); }
    void sit() override { c_.sit_toggle(); }
    void toggle_view() override {
        if (hooks_.toggle_view) hooks_.toggle_view();
    }
    bool window_active(game::HotButtonWindow w) override { return hooks_.window_active && hooks_.window_active(w); }
    void window_activate(game::HotButtonWindow w) override {
        if (hooks_.window_show) hooks_.window_show(w, true);
    }
    void window_deactivate(game::HotButtonWindow w) override {
        if (hooks_.window_show) hooks_.window_show(w, false);
    }
    void clear_bard_twist() override { c_.clear_bard_twist(); }
    void spell_gem_left_click(std::int32_t gem) override { c_.cast_gem(gem); }
    std::optional<game::HotButtonPetButton> pet_button(std::int32_t index) override {
        if (index < 0 || index > 13) return std::nullopt;
        pet_ = index;
        return game::HotButtonPetButton{0, 0}; // the client draws the checks from the pet's state
    }
    void pet_button_set_check(bool) override {}
    void pet_window_click(std::int32_t) override {
        if (hooks_.pet_button) hooks_.pet_button(pet_);
    }

private:
    net::GameConnection& c_;
    const game::HotButtonStore& store_;
    game::SocialQueue& socials_;
    const HotButtonHooks& hooks_;
    std::vector<std::pair<std::string, std::uint32_t>>& messages_;
    bool& attack_toggle_;
    UseSkillFn use_skill_;
    std::function<void()> ranged_;

public:
    int bar = -1, button = -1; // the hot button pressed (-1: the Actions window)

private:
    std::pair<std::int32_t, std::int32_t> slot_{-1, -1};
    int pet_ = -1;
};

} // namespace

// CharacterZoneClient::DoPassageOfTime's social step (game/hot_button_press.h).
void ActionsUi::run_socials(net::GameConnection& conn) {
    struct Q final : game::SocialRunQueries {
        ActionsUi& ui;
        net::GameConnection& c;
        Q(ActionsUi& u, net::GameConnection& cn) : ui(u), c(cn) {}
        const std::string* line(std::int32_t social, std::int32_t n) override {
            if (social < 0 || social >= game::kSocialPages * game::kSocialButtons || n < 0 || n >= game::kSocialLines)
                return nullptr;
            return &ui.store_.social(social).lines[static_cast<std::size_t>(n)];
        }
        void interpret(const std::string& command) override {
            std::size_t at = command.find_first_not_of(' ');
            if (at != std::string::npos) c.send_command(command.substr(at));
        }
        void alternate_ability(std::int32_t group) override {
            // "%s %s %d" of eqstr 0x3773 (/alternateadv) and 0x33d9 (act).
            c.send_command(c.strings().format(0x3773, {}) + " " + c.strings().format(0x33d9, {}) + " " + std::to_string(group));
        }
        bool cooldown_running(std::int32_t social) override {
            return social >= 0 && social < 120 && ui.store_.social(social).cooldown_length != 0;
        }
        void start_cooldown(std::int32_t social, std::int32_t ms) override {
            if (social < 0 || social >= 120) return;
            auto& s = ui.store_.social(social);
            s.cooldown_start = static_cast<std::uint32_t>(net::GameConnection::now_ms());
            s.cooldown_length = static_cast<std::uint32_t>(ms);
        }
    } q(*this, conn);
    const auto now = static_cast<std::uint32_t>(net::GameConnection::now_ms());
    for (int i = 0; i < game::kSocialPages * game::kSocialButtons; ++i) {
        auto& s = store_.social(i);
        if (s.cooldown_length != 0 && now - s.cooldown_start >= s.cooldown_length) s.cooldown_start = s.cooldown_length = 0;
    }
    // Once per step of the game clock (net::GameConnection::set_game_steps): a line waits for a step past 100 ms.
    if (const auto& steps = conn.game_steps())
        for (std::int64_t t : *steps) game::social_passage_of_time(social_queue_, static_cast<std::uint32_t>(t), q);
    else
        game::social_passage_of_time(social_queue_, now, q);
}

std::int32_t ActionsUi::button_skill(const HotButton& b) const {
    switch (b.type) {
    case HotButtonType::Skill: return b.slot;
    case HotButtonType::CombatSkill:
        return b.slot >= 0 && b.slot < 4 ? store_.combat_skills[static_cast<std::size_t>(b.slot)] : -1;
    case HotButtonType::Ability: return b.slot >= 0 && b.slot < 6 ? store_.abilities[static_cast<std::size_t>(b.slot)] : -1;
    default: return -1;
    }
}

void ActionsUi::hot_button(net::GameConnection& conn, const mech::Character* ch, const AbilityView& view, int bar,
                           int button, bool mouse) {
    if (bar < 0 || bar >= game::kHotBars || button < 0 || button >= game::kHotButtons) return;
    const HotButton b = store_.current(bar, button);
    const auto& ps = conn.player_status();
    game::HotButtonGlobals g;
    g.stand_state = ps.stand_state;
    g.item_pending = conn.spell_state().pending;
    g.range_attack_ready = state_.attack.primary_ready ? 1 : 0; // 0xde0864, the primary swing's ready flag
    g.group_invited = conn.group().inviter.empty() ? 0 : 1;
    g.run_walk = conn.running() ? 1 : 0;
    g.toggle_89c = toggle_89c_;
    // The controlled spawn's speed (+0x7c): moving keys held count as moving.
    g.controlled_speed = view.velocity == glm::vec3(0.0f) ? 0.0f : 1.0f;
    game::HotButtonRecord r;
    r.type = static_cast<std::uint8_t>(b.type);
    r.value = b.slot;
    // A link's item (the record's +8), wherever it is now (game::hot_button_item_slot).
    if (b.type == HotButtonType::ItemLink)
        if (auto at = game::hot_button_item_slot(conn.inventory(), b)) r.item = game::HotButtonRecord::Item{at->slot, at->sub};
    PressQueries q(conn, store_, social_queue_, hooks_, messages_, attack_toggle_,
                   [&](std::uint32_t skill) { use_skill(conn, ch, view, skill); },
                   [&] { melee_attack(conn, ch, view, 11); }); // DoAttack(11, 0, target)
    q.bar = bar;
    q.button = button;
    game::do_hot_button(g, r, mouse ? 1 : 0, q);
    state_.attack.primary_ready = g.range_attack_ready != 0;
    toggle_89c_ = g.toggle_89c;
    if ((g.run_walk != 0) != conn.running()) conn.set_running(g.run_walk != 0);
}

void ActionsUi::hot_button_right_click(net::GameConnection& conn, int bar, int button) {
    if (bar < 0 || bar >= game::kHotBars || button < 0 || button >= game::kHotButtons) return;
    const HotButton b = store_.current(bar, button);
    // A Lua social: a right click stops it while it runs, else opens its editor.
    if (b.type == HotButtonType::Social && b.slot >= 0 && b.slot < game::kSocialCount && store_.social(b.slot).is_lua()) {
        if (hooks_.stop_lua_social && hooks_.stop_lua_social(b.slot)) return;
        if (hooks_.edit_lua_social) hooks_.edit_lua_social(b.slot);
        return;
    }
    struct Q final : game::HotButtonRightClickQueries {
        ActionsUi& ui;
        net::GameConnection& c;
        std::optional<net::ItemSlot> slot; // the item's (game::hot_button_item_slot)
        Q(ActionsUi& u, net::GameConnection& cn, std::optional<net::ItemSlot> s) : ui(u), c(cn), slot(s) {}
        void social_stop(std::int32_t social, bool keep) override {
            if (auto cleared = game::social_stop(ui.social_queue_, social, keep);
                cleared && *cleared >= 0 && *cleared < game::kSocialPages * game::kSocialButtons) {
                auto& s = ui.store_.social(*cleared);
                s.cooldown_start = s.cooldown_length = 0;
            }
        }
        void open_social_editor(std::int32_t social) override {
            if (ui.hooks_.open_social_editor) ui.hooks_.open_social_editor(social);
        }
        void aa_window_show(std::int32_t group) override {
            if (ui.hooks_.show_aa_ability) ui.hooks_.show_aa_ability(group);
        }
        void inv_slot_right_click(std::int32_t, std::int32_t) override {
            if (slot && ui.hooks_.use_item) ui.hooks_.use_item(slot->slot, slot->sub);
        }
        void spell_gem_right_click(std::int32_t gem) override {
            if (ui.hooks_.spell_gem_right_click) ui.hooks_.spell_gem_right_click(gem);
        }
    } q(*this, conn,
        b.type == HotButtonType::KronoSlot ? std::optional(net::ItemSlot::possessions(static_cast<std::int16_t>(b.slot)))
                                           : game::hot_button_item_slot(conn.inventory(), b));
    game::HotButtonRightClick c;
    c.type = static_cast<std::uint8_t>(b.type);
    c.value = b.slot;
    c.current_social = social_queue_.current;
    if (b.slot >= 0 && b.slot < game::kSocialPages * game::kSocialButtons)
        c.social_cooldown_length = static_cast<std::int32_t>(store_.social(b.slot).cooldown_length);
    c.slot = game::HotButtonSlot{true, 0, 0};
    c.gem_window = true;
    game::hot_button_right_click(c, q);
}

void ActionsUi::update(net::GameConnection& conn, const mech::Character* ch, const AbilityView& view) {
    // Last frame's changes (the windows' clicks, the plugins' and Lua's edits), once they have waited a little.
    save_if_due(net::GameConnection::now_ms());
    // The spell sets, /grouproles' role sets and the group finder's filters live on the connection and in this
    // character file ([SpellLoadouts], [GroupRoleLoadouts], [ADDITIONALFILTERS]): read at every zone-in (the
    // connection may be new), written back here when they change (SaveSpellLoadoutsToIni,
    // SaveGroupRoleLoadoutsToIni, CGroupSearchFiltersWnd::SaveIni).
    if (loadouts_fresh_) {
        conn.spell_loadouts().load(char_ini_);
        conn.group_role_sets().load(char_ini_);
        // CGroupSearchFiltersWnd::LoadIni ([ADDITIONALFILTERS]), once per connection as the window is made once.
        if (!conn.group_search().filters_loaded) {
            conn.group_search().filters.load(char_ini_);
            conn.group_search().filters_loaded = true;
        }
        loadouts_fresh_ = false;
    } else {
        if (conn.spell_loadouts().take_dirty()) {
            conn.spell_loadouts().save(char_ini_);
            changed();
        }
        if (conn.group_role_sets().take_dirty()) {
            conn.group_role_sets().save(char_ini_);
            changed();
        }
        if (auto& gs = conn.group_search(); gs.filters_dirty) {
            gs.filters_dirty = false; // CGroupSearchFiltersWnd::SaveIni after each change
            gs.filters.save(char_ini_);
            changed();
        }
    }
    // A cast's MakeMeVisible ended hiding or sneaking (net/game_connection_spells.cpp).
    auto cleared = conn.take_visibility_cleared();
    if (cleared.hide) state_.hidden = false;
    if (cleared.sneak) state_.sneaking = false;
    while (auto p = conn.poll_ability_packet()) {
        auto op = net::from_wire(p->opcode);
        if (op == net::Op::SpawnAppearance) {
            auto a = net::decode_spawn_appearance(*p);
            if (!a || a->spawn_id != conn.world().player_id) continue;
            if (a->type == 15) state_.sneaking = a->parameter != 0; // Handle_OP_SpawnAppearance: sneak
            if (a->type == 3 && a->parameter == 0) state_.hidden = false;
        } else if (auto bind = op == net::Op::Bind_Wound ? net::decode_bind_wound(*p) : std::nullopt) {
            // EQEmu's Client::BindWound answers (net/notice_packets.h). The original's Handle_OP_Bind_Wound is not
            // decoded: once the bind ends the character stands up from the bind-wound kneel, and the lines are this
            // client's.
            std::uint16_t type = *bind;
            const char* line = type == 1 ? "You finish binding the wounds."
                               : type == 4 || type == 5 ? "Your bind wound target is gone."
                               : type == 7 ? "You have no bandages." : nullptr;
            if (line) {
                conn.ui_message(line);
                if (conn.player_status().stand_state == 105) conn.set_stand_state(100);
            }
        } else if (op == net::Op::CancelSneakHide) {
            // The server ended sneak and hide; newer clients answer with a 4-byte OP_Hide (Send_OP_Hide).
            if (state_.activity == static_cast<std::int32_t>(mech::skill::kHide)) state_.activity = -1;
            state_.hidden = false;
            state_.sneaking = false;
            conn.send_zone_packet(net::encode_hide_cancel());
        }
    }
    run_socials(conn);
    if (!ch) return;
    mech::SkillContext ctx = context(conn, *ch, view);
    state_.timers.update(*ch, ctx.now, state_.sneaking, state_.hidden);
    // The class the hide check tests: a server's client may change it (the NMS patch NOPs the test: any class).
    const std::uint8_t hide_class = conn.server_ext().hide_check_class(conn, ch->sheet().class_id);
    if (auto m = mech::check_hide_moved(ctx, state_, hide_class)) message(conn, *m);
    if (state_.fishing && static_cast<std::int32_t>(ctx.now - state_.fishing_eta) >= 0) state_.fishing = false;
    conn.note_hide_state(state_.hidden, state_.sneaking);
}

void ActionsUi::melee_timers(net::GameConnection& conn, const mech::Character* ch, const AbilityView& view) {
    if (!ch) return;
    mech::SkillContext ctx = context(conn, *ch, view);
    mech::attack_on_reset(ctx, state_, conn.auto_attack());
    // @0x45e389: StandState not 100 turns auto attack off (__ExecuteCmd(AUTOPRIM, 1, 0)) and auto fire
    // (SetAutoRangeAttack(0)).
    if (conn.player_status().stand_state != 100) {
        if (conn.auto_attack()) conn.toggle_auto_attack();
        if (conn.auto_fire()) conn.set_auto_fire(false);
    }
    mech::swing_ready(ctx, state_);
}

void ActionsUi::melee_attack(net::GameConnection& conn, const mech::Character* ch, const AbilityView& view,
                             std::int32_t slot) {
    if (!ch) return;
    mech::SkillContext ctx = context(conn, *ch, view);
    const std::uint32_t before = slot == 14 ? state_.attack.last_secondary : state_.attack.last_primary;
    mech::SkillResult r;
    bool done = mech::do_attack(ctx, state_, static_cast<std::uint8_t>(slot), 0, r);
    for (const auto& m : r.messages) message(conn, m);
    for (int buff : r.removed_buffs) conn.clear_buff_locally(buff);
    for (const auto& s : r.sends) // DoRangedAttack's OP_CombatAbility (slot 11)
        if (auto p = net::encode_skill_send(s)) conn.send_zone_packet(*p);
    if (r.stop_auto_fire) conn.set_auto_fire(false);
    // The swing's animation: SendCombatAbilityAttack computes PlayerZoneClient_PlayAttackAnimation's choice and
    // drops it (@0x4fb627); the server's OP_Animation, which EQEmu sends to the attacker too, plays it.
    const std::uint32_t after = slot == 14 ? state_.attack.last_secondary : state_.attack.last_primary;
    if (!r.swings.empty() || !r.messages.empty() || !r.sends.empty())
        log::info("melee: DoAttack({}) at {}: {} swing(s), {} send(s), {} message(s), returned {}, timer {} -> {}", slot,
                  ctx.now, r.swings.size(), r.sends.size(), r.messages.size(), done ? 1 : 0, before, after);
}

void ActionsUi::setup_test_buttons(const std::vector<std::uint32_t>& skills) {
    for (std::size_t i = 0; i < skills.size() && i < game::kHotButtons; ++i) {
        std::uint32_t s = skills[i];
        store_.at(0, 0, static_cast<int>(i)) = skill_button(s, mech::skill_combat_type(s) != 0);
    }
    changed();
}

// ---------------------------------------------------------------- windows

void ActionsUi::draw(net::GameConnection& conn, const mech::Character* ch, const AbilityView& view) {
    if (hotbar_open_ && !ui::lua::replaces("HotButtonWnd")) draw_hotbar(conn, ch, view); // off while the Lua bar runs
    if (actions_open_ && !ui::lua::replaces("ActionsWindow")) draw_actions(conn, ch, view);
    if (keys_open_) draw_keys(conn);
}

void ActionsUi::hot_button_menu(net::GameConnection& conn, const mech::Character* ch, HotButton& b) {
    if (ImGui::MenuItem("Clear")) {
        b = {};
        changed();
    }
    if (ImGui::MenuItem("Melee Attack")) {
        b = {};
        b.type = HotButtonType::WeaponSlot;
        b.slot = 0;
        b.label = "Melee Attack";
        changed();
    }
    if (ch && ImGui::BeginMenu("Skills")) {
        for (bool combat : {true, false})
            for (std::uint32_t s : mech::usable_skills(*ch, combat, skill_race(conn, *ch)))
                if (ImGui::MenuItem(skill_name(conn, s).c_str())) {
                    b = skill_button(s, combat);
                    changed();
                }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Spell gems")) {
        const auto& gems = conn.spell_state().gems;
        for (int g = 0; g < 12; ++g) {
            HotButton gem;
            gem.type = HotButtonType::SpellGem;
            gem.slot = g;
            std::string label = std::to_string(g + 1) + ": " + button_label(conn, gem);
            if (g < static_cast<int>(gems.size()) && ImGui::MenuItem(label.c_str())) {
                b = gem;
                changed();
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Menu buttons")) {
        for (std::int32_t m : kMenuButtons)
            if (ImGui::MenuItem(menu_button_name(m))) {
                b = {};
                b.type = HotButtonType::MenuButton;
                b.slot = m;
                changed();
            }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Socials")) {
        for (int i = 0; i < game::kSocialPages * game::kSocialButtons; ++i) {
            const auto& s = store_.social(i);
            if (s.empty()) continue;
            std::string label = s.name + "##social" + std::to_string(i);
            if (ImGui::MenuItem(label.c_str())) {
                b = {};
                b.type = HotButtonType::Social;
                b.slot = i;
                changed();
            }
        }
        ImGui::EndMenu();
    }
}

void ActionsUi::draw_hotbar(net::GameConnection& conn, const mech::Character* ch, const AbilityView& view) {
    float em = ImGui::GetFontSize();
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y - em * 0.6f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 1.0f));
    if (!ImGui::Begin("Hotbar##hailstone_hotbar", &hotbar_open_, kPanelFlags | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    const int bar = 0;
    int page = store_.page(bar);
    std::uint32_t now = static_cast<std::uint32_t>(net::GameConnection::now_ms());
    float size = em * 3.6f;
    for (int i = 0; i < game::kHotButtons; ++i) {
        if (i % 6) ImGui::SameLine();
        ImGui::PushID(i);
        HotButton& b = store_.current(bar, i);
        std::string label = b.empty() ? "" : button_label(conn, b);
        // The key bound to HOT1_<n> in the corner, as the original prints it.
        std::string key = game::key_text(keymap_.binding(static_cast<int>(game::KeyCommand::HOT1_1) + i, 0));
        if (key == "-------") key.clear();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.6f));
        bool clicked = ImGui::Button(("##hb" + std::to_string(i)).c_str(), ImVec2(size, size));
        ImGui::PopStyleVar();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Cooldown: the part of the reuse time left, darkening the button from the top.
        std::uint32_t left = 0, total = 0;
        if (ch && button_skill(b) >= 0) {
            auto s = static_cast<std::uint32_t>(button_skill(b));
            left = state_.timers.remaining(*ch, s, now);
            total = state_.timers.timer_duration(*ch, s);
        }
        if (left > 0 && total > 0) {
            float f = std::min(1.0f, static_cast<float>(left) / static_cast<float>(total));
            dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size * f), IM_COL32(0, 0, 0, 150));
        }
        // The label wrapped to the button, then the key and the time left.
        if (!label.empty()) {
            ImGui::PushClipRect(p, ImVec2(p.x + size, p.y + size), true);
            ImVec2 ts = ImGui::CalcTextSize(label.c_str(), nullptr, false, size - 4.0f);
            dl->AddText(ImGui::GetFont(), em * 0.85f, ImVec2(p.x + 3.0f, p.y + (size - ts.y * 0.85f) * 0.5f),
                        IM_COL32(235, 235, 235, 255), label.c_str(), nullptr, size - 6.0f);
            ImGui::PopClipRect();
        }
        if (!key.empty()) dl->AddText(ImGui::GetFont(), em * 0.75f, ImVec2(p.x + 3.0f, p.y + 2.0f), IM_COL32(255, 220, 120, 255), key.c_str());
        if (left > 0) {
            std::string t = seconds_text(left);
            dl->AddText(ImVec2(p.x + size - ImGui::CalcTextSize(t.c_str()).x - 3.0f, p.y + size - em - 2.0f),
                        IM_COL32(255, 255, 255, 255), t.c_str());
        }
        if (clicked) hot_button(conn, ch, view, bar, i);
        if (ImGui::BeginDragDropTarget()) {
            if (ImGui::AcceptDragDropPayload(kDragType)) {
                b = dragged();
                changed();
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("hbmenu")) {
            hot_button_menu(conn, ch, b);
            ImGui::EndPopup();
        }
        if (!b.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label.c_str());
        ImGui::PopID();
    }
    // The page buttons (HB_PageUp / HB_PageDown) and the page number.
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) store_.set_page(bar, page - 1);
    ImGui::SameLine();
    ImGui::Text("%d", page + 1);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) store_.set_page(bar, page + 1);
    ImGui::SameLine();
    ImGui::TextDisabled("right-click a button to set it");
    ImGui::End();
}

void ActionsUi::run_button(net::GameConnection& conn, const mech::Character* ch, const AbilityView& view,
                           const HotButton& b) {
    game::HotButton saved = store_.current(9, 11);
    store_.current(9, 11) = b; // run it through DoHotButton
    hot_button(conn, ch, view, 9, 11);
    store_.current(9, 11) = saved;
}

void ActionsUi::draw_actions(net::GameConnection& conn, const mech::Character* ch, const AbilityView& view) {
    float em = ImGui::GetFontSize();
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x - em * 0.6f, display.y * 0.35f), ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(em * 22.0f, em * 26.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Actions##hailstone_actions", &actions_open_, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    std::uint32_t now = static_cast<std::uint32_t>(net::GameConnection::now_ms());
    // A skill button: click uses it, drag puts it on the hot bar; the time left on it.
    auto skill_row = [&](std::uint32_t s, bool combat) {
        ImGui::PushID(static_cast<int>(s));
        std::string name = skill_name(conn, s);
        std::uint32_t left = ch ? state_.timers.remaining(*ch, s, now) : 0;
        if (ImGui::Button(name.c_str(), ImVec2(em * 9.0f, 0))) use_skill(conn, ch, view, s);
        HotButton hb = skill_button(s, combat);
        if (ImGui::BeginDragDropSource()) {
            dragged() = hb;
            ImGui::SetDragDropPayload(kDragType, &s, sizeof(s));
            ImGui::TextUnformatted(name.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::SameLine();
        if (s == mech::skill::kSlam) ImGui::TextUnformatted("innate");
        else ImGui::Text("%3d", ch && s < mech::kSkills ? ch->sheet().skills[s] : 0);
        ImGui::SameLine();
        if (left > 0) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s s", seconds_text(left).c_str());
        else ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "ready");
        ImGui::PopID();
    };
    auto slot_buttons = [&](auto& slots, bool combat, const char* id) {
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (i % 2) ImGui::SameLine();
            ImGui::PushID(id);
            ImGui::PushID(static_cast<int>(i));
            std::int32_t s = slots[i];
            std::string label = s < 0 ? "(empty)" : skill_name(conn, static_cast<std::uint32_t>(s));
            if (ImGui::Button((label + "##slot").c_str(), ImVec2(em * 9.5f, 0)) && s >= 0)
                use_skill(conn, ch, view, static_cast<std::uint32_t>(s));
            if (s >= 0 && ImGui::BeginDragDropSource()) {
                dragged() = skill_button(static_cast<std::uint32_t>(s), combat);
                ImGui::SetDragDropPayload(kDragType, &s, sizeof(s));
                ImGui::TextUnformatted(label.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("slotmenu")) {
                if (ImGui::MenuItem("Clear")) {
                    slots[i] = -1;
                    changed();
                }
                if (ch)
                    for (std::uint32_t k : mech::usable_skills(*ch, combat, skill_race(conn, *ch)))
                        if (ImGui::MenuItem(skill_name(conn, k).c_str())) {
                            slots[i] = static_cast<std::int32_t>(k);
                            changed();
                        }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::PopID();
        }
    };
    if (ImGui::BeginTabBar("##pages")) {
        const int forced = actions_page_forced_ ? actions_page_ : -1;
        auto tab = [&](const char* name, int page) {
            ImGuiTabItemFlags f = forced == page ? ImGuiTabItemFlags_SetSelected : 0;
            bool open = ImGui::BeginTabItem(name, nullptr, f);
            if (open) actions_page_ = page;
            return open;
        };
        if (tab("Main", 0)) {
            for (std::size_t i = 0; i < std::size(kMenuButtons); ++i) {
                if (i % 3) ImGui::SameLine();
                std::int32_t m = kMenuButtons[i];
                const char* label = m == 4 ? (conn.sitting() ? "Stand" : "Sit") : m == 5 ? (conn.running() ? "Walk" : "Run")
                                                                                         : menu_button_name(m);
                if (ImGui::Button(label, ImVec2(em * 6.0f, 0))) {
                    HotButton hb;
                    hb.type = HotButtonType::MenuButton;
                    hb.slot = m;
                    game::HotButton saved = store_.current(9, 11);
                    store_.current(9, 11) = hb; // run it through DoHotButton
                    hot_button(conn, ch, view, 9, 11);
                    store_.current(9, 11) = saved;
                }
                if (ImGui::BeginDragDropSource()) {
                    HotButton hb;
                    hb.type = HotButtonType::MenuButton;
                    hb.slot = m;
                    dragged() = hb;
                    ImGui::SetDragDropPayload(kDragType, &m, sizeof(m));
                    ImGui::TextUnformatted(label);
                    ImGui::EndDragDropSource();
                }
            }
            ImGui::EndTabItem();
        }
        if (tab("Abilities", 1)) {
            slot_buttons(store_.abilities, false, "ab");
            ImGui::Separator();
            if (ch)
                for (std::uint32_t s : mech::usable_skills(*ch, false, skill_race(conn, *ch))) skill_row(s, false);
            ImGui::EndTabItem();
        }
        if (tab("Combat", 2)) {
            if (ImGui::Button(conn.auto_attack() ? "Attack (on)" : "Attack", ImVec2(em * 9.5f, 0))) attack_toggle_ = true;
            if (ImGui::BeginDragDropSource()) {
                HotButton hb;
                hb.type = HotButtonType::WeaponSlot;
                hb.slot = 0;
                hb.label = "Melee Attack";
                dragged() = hb;
                int zero = 0;
                ImGui::SetDragDropPayload(kDragType, &zero, sizeof(zero));
                ImGui::TextUnformatted("Melee Attack");
                ImGui::EndDragDropSource();
            }
            slot_buttons(store_.combat_skills, true, "cb");
            ImGui::Separator();
            if (ch)
                for (std::uint32_t s : mech::usable_skills(*ch, true, skill_race(conn, *ch))) skill_row(s, true);
            ImGui::EndTabItem();
        }
        if (tab("Socials", 3)) {
            ImGui::SetNextItemWidth(em * 6.0f);
            int shown = social_page_ + 1;
            if (ImGui::InputInt("Page", &shown)) social_page_ = std::clamp(shown, 1, game::kSocialPages) - 1;
            for (int i = 0; i < game::kSocialButtons; ++i) {
                if (i % 3) ImGui::SameLine();
                int index = social_page_ * game::kSocialButtons + i;
                game::Social& s = store_.social(index);
                ImGui::PushID(index);
                // Queued as a hot button's press is (/pause, /timer, /alt act and Lua socials run as there).
                if (ImGui::Button(s.empty() ? "(empty)##s" : (s.name + "##s").c_str(), ImVec2(em * 6.0f, 0)) && !s.empty())
                    press_social(index);
                if (!s.empty() && ImGui::BeginDragDropSource()) {
                    HotButton hb;
                    hb.type = HotButtonType::Social;
                    hb.slot = index;
                    dragged() = hb;
                    ImGui::SetDragDropPayload(kDragType, &index, sizeof(index));
                    ImGui::TextUnformatted(s.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginPopupContextItem("edit")) {
                    char name[16] = {};
                    s.name.copy(name, 15);
                    if (ImGui::InputText("Name", name, sizeof name)) {
                        s.name = name;
                        changed();
                    }
                    for (int l = 0; l < game::kSocialLines; ++l) {
                        char line[128] = {};
                        s.lines[static_cast<std::size_t>(l)].copy(line, 127);
                        if (ImGui::InputText(("Line " + std::to_string(l + 1)).c_str(), line, sizeof line)) {
                            s.lines[static_cast<std::size_t>(l)] = line;
                            changed();
                        }
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::TextDisabled("right-click a social to edit it");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    actions_page_forced_ = false;
    ImGui::End();
}

void ActionsUi::reset_keys() {
    keymap_.reset_defaults(config_.get_bool("UseWASDDefault", true));
    keymap_.save(config_);
    config_.save(config_path_);
}

void ActionsUi::draw_keys(net::GameConnection& conn) {
    float em = ImGui::GetFontSize();
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.45f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em * 30.0f, em * 28.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Keyboard##hailstone_keys", &keys_open_, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    static const std::pair<std::uint32_t, const char*> kCategories[] = {
        {0, "All"}, {1, "Movement"}, {2, "Commands"}, {4, "Spells"}, {8, "Target"}, {0x10, "Camera"},
        {0x20, "Chat"}, {0x40, "Windows"}, {0x80, "Hot Bar 1"}, {0x100, "Hot Bar 2"}, {0x200, "Hot Bar 3"},
    };
    const char* current = "All";
    for (const auto& [c, n] : kCategories)
        if (c == key_category_) current = n;
    ImGui::SetNextItemWidth(em * 10.0f);
    if (ImGui::BeginCombo("Category", current)) {
        for (const auto& [c, n] : kCategories)
            if (ImGui::Selectable(n, c == key_category_)) key_category_ = c;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Restore defaults")) {
        keymap_.reset_defaults(config_.get_bool("UseWASDDefault", true));
        keymap_.save(config_);
        config_.save(config_path_);
    }
    if (capture_.command >= 0)
        ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "Press a key for %s (Esc cancels)", game::command_name(capture_.command));
    else
        ImGui::TextDisabled("Click a key to change it; right-click clears it.");
    if (ImGui::BeginTable("##keys", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Key 1", ImGuiTableColumnFlags_WidthFixed, em * 7.0f);
        ImGui::TableSetupColumn("Key 2", ImGuiTableColumnFlags_WidthFixed, em * 7.0f);
        ImGui::TableHeadersRow();
        for (int cmd : game::listed_commands()) {
            std::uint32_t cat = game::command_category(cmd);
            if (key_category_ != 0 && (cat & key_category_) == 0) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::uint32_t label = game::command_label_eqstr(cmd);
            ImGui::TextUnformatted(label ? conn.strings().format(label, {}).c_str() : game::command_name(cmd));
            for (int bank = 0; bank < 2; ++bank) {
                ImGui::TableNextColumn();
                ImGui::PushID(cmd * 2 + bank);
                bool capturing = capture_.command == cmd && capture_.bank == bank;
                std::string text = capturing ? "..." : game::key_text(keymap_.binding(cmd, bank));
                if (ImGui::Button(text.c_str(), ImVec2(-1, 0))) capture_ = {cmd, bank};
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    keymap_.bind(cmd, bank, game::KeyCombo{});
                    keymap_.save(config_);
                    config_.save(config_path_);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace hailstone::app
