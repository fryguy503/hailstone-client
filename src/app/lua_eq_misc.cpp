// The pet, actions, compass and spell book windows (CPetInfoWnd, CActionsWnd, CCompassWnd, CSpellBookWnd).

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <span>

#include <imgui.h>
#include <lua.hpp>

#include "app/actions_ui.h"
#include "app/lua_eq_values.h"
#include "app/lua_eq_windows.h"
#include "game/combat.h"
#include "game/hotbuttons.h"
#include "game/mechanics/abilities.h"
#include "game/mechanics/character.h"
#include "game/pet_buttons.h"
#include "game/spells.h"
#include "game/world_state.h"
#include "game/zone_names.h"
#include "game/tracking.h"
#include "net/game_connection.h"
#include "net/pet_packets.h"
#include "ui/ui_ini.h"

namespace hailstone::app {

namespace {

int index_after(std::string_view id, std::string_view prefix) {
    if (id.size() <= prefix.size() || id.substr(0, prefix.size()) != prefix) return -1;
    int v = -1;
    auto rest = id.substr(prefix.size());
    auto [p, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), v);
    return ec == std::errc() && (p == rest.data() + rest.size() || *p == '_') ? v : -1;
}

ChildArt anim(const char* name) {
    ChildArt a;
    a.kind = ChildArt::Anim;
    a.anim = name;
    return a;
}

ChildArt none() {
    ChildArt a;
    a.kind = ChildArt::None;
    return a;
}

ChildArt image(const std::optional<ui::UiTextures::Cell>& c) {
    if (!c) return none();
    ChildArt a;
    a.kind = ChildArt::Image;
    a.cell = *c;
    return a;
}

// ---------------------------------------------------------------- PetInfoWindow (CPetInfoWnd)

// The command buttons Pet0..Pet13 (game::PetButtonLayout, EqHost::pet_buttons): the constructor's commands, then the
// UI ini's PetButtonState; a click runs the command the button holds as /pet does (WndNotification: the eqstr words
// 7512 "report health" .. 13103 "target"); the right button down on one pops up the command menu.
int pet_button_index(std::string_view id) {
    int b = index_after(id, "Pet");
    return b >= 0 && b < game::kPetWindowButtons && id.ends_with("_Button") && id.find("Buff") == std::string_view::npos
               ? b
               : -1;
}

// Whether the button holding command index `i` is checked (UpdateToggleButtonChecks: a toggle command's state).
bool pet_command_checked(const net::GameConnection& c, const game::PetButtonLayout& layout, int i) {
    int b = layout.button_of(i), st = game::pet_command_toggle_state(i);
    return b >= 0 && st >= 0 && game::is_toggle_pet_command(layout.text(b)) && c.pet_toggles()[static_cast<std::size_t>(st)];
}

} // namespace

std::optional<bool> pet_button_checked(EqHost& host, int index) {
    // CPetInfoWnd::GetButton(index) and its +0x258 (enabled: while there is a pet) / +0x1e4 (checked).
    if (!host.conn || index < 0 || index >= game::kPetWindowButtons || !host.conn->pets().focused) return std::nullopt;
    const int i = host.pet_buttons.command_of(index);
    return i >= 0 && pet_command_checked(*host.conn, host.pet_buttons, i);
}

namespace {

// CPetInfoWnd_RefreshPetState @0x7104a0: when the player gets or loses a pet, the window shows or hides (its
// virtual +0x90 / +0x94, StoreIniVis) with PopupOnPet (+0x765: the UI ini's PopupOnPet in the window's section,
// default on; CPetInfoWnd::SetShowOnSummon @0x7119e0 sets and saves it, the Options window's box). Otherwise it stays
// as it is, shown or not, pet or no pet. The pet state it last saw (+0x764) starts false in the constructor
// (InitGameUI, after the UI ini's places and shown states are read), so a pet already there when the character
// enters the world shows the window over a saved hidden state.
// The toggle bytes (game::PetToggleStates) live on the connection, which ReissueToggledPetCommands needs at the
// pet's spawn; the window links itself to each new connection (the constructor's LoadButtonToggleStates) and
// saves the keys cmd_pet sets.
class PetWindow : public StockWindow {
public:
    PetWindow() : StockWindow("PetInfoWindow", false) {}

    void ini_loaded(EqHost& host, ui::UiIni& ini, const std::filesystem::path& user_dir) override {
        ini_ = &ini;
        // A character's UI ini (not a copied layout's reload): the window is made again.
        if (user_dir != ini_dir_) {
            ini_dir_ = user_dir;
            had_pet_ = false;
            if (host.conn && host.conn->pet_window_linked()) load_toggles(host);
        }
        popup_ = game::pet_window_popup_setting(ini.get("PetInfoWindow", "PopupOnPet"));
        // The constructor (InitGameUI, per character): the default buttons, then CPetInfoWnd_LoadPetButtonState,
        // which saves the defaults when the UI ini has none.
        auto& layout = host.pet_buttons;
        layout = {};
        if (!layout.load(ini.get("PetInfoWindow", "PetButtonState").value_or(""))) save_layout(layout);
    }
    void frame(EqHost& host) override {
        // The window is made when the character enters the world (InitGameUI, after the UI ini is read).
        if (host.in_game && host.conn && !host.conn->pet_window_linked()) link(host);
        const game::Spawn* p = host.conn ? pet(host) : nullptr;
        bool has = p != nullptr;
        if (game::pet_window_refresh(had_pet_, has, popup_)) {
            open_ = has;
            if (ini_) ini_->store_shown(ui::window_ini_info(screen_), has); // StoreIniVis
        }
        // CPetInfoWnd_RefreshPetState: with the pet, UpdateToggleButtonChecks asks CanUsePetCommand for each command
        // index (the command menu's rows); then SyncHotButtonStates enables the buttons holding a command.
        auto& layout = host.pet_buttons;
        if (p) {
            game::PetCommandQuery q = query(host, *p);
            std::array<bool, game::kPetCommandCount> usable{};
            for (int i = 0; i < game::kPetCommandCount; ++i)
                usable[static_cast<std::size_t>(i)] = game::can_use_pet_command(q, game::pet_command_of_index(i));
            layout.set_usable(usable);
        }
        layout.sync_enabled();
    }
    std::optional<int> setting(std::string_view name) const override {
        if (name == "popup") return popup_ ? 1 : 0;
        return std::nullopt;
    }
    bool set_setting(EqHost&, std::string_view name, int value) override {
        if (name != "popup") return false;
        popup_ = value != 0;
        if (ini_) {
            ini_->put("PetInfoWindow", "PopupOnPet", popup_ ? "1" : "0"); // WriteBoolToUIINI
            ini_->save();
        }
        return true;
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& c = *host.conn;
        if (int b = pet_button_index(id); b >= 0) {
            const auto& layout = host.pet_buttons;
            out.text = layout.text(b);
            out.enabled = layout.enabled(b); // the constructor disables all; SyncHotButtonStates enables the held ones
            out.action = true;               // the constructor's bAction: the right button reaches a disabled one
            if (int i = layout.command_of(b); i >= 0) out.checked = pet_command_checked(c, layout, i);
        } else if (int n = index_after(id, "PetBuff"); n >= 0) {
            const auto& buffs = c.pet_buffs();
            if (n >= static_cast<int>(buffs.size())) {
                out.background = none();
                out.decal = none();
                return;
            }
            const auto* s = c.spell_table().get(buffs[static_cast<std::size_t>(n)].second.spell);
            if (!s) {
                out.background = none();
                out.decal = none();
                return;
            }
            out.background = anim(s->beneficial() ? "PetBlueIconBackground" : "PetRedIconBackground");
            out.decal = host.textures ? image(host.textures->spell_icon(s->spell_icon)) : none();
        } else if (id == "Pet2HPGauge" || id == "Pet3HPGauge") {
            // A later client's (EQType 0, nothing fills them in RoF2); the NMS add-on (pet_window.cpp
            // PetWindow::OnPulse) shows its server's other pets there: the HP percent and the name without its
            // trailing digits, hidden while there is no such pet (no OP_PetList on a stock server).
            auto extra = c.pet_window_extra_pets();
            std::size_t which = id == "Pet2HPGauge" ? 0 : 1;
            const auto& spawns = c.world().spawns;
            if (which >= extra.size()) {
                out.shown = false;
                return;
            }
            const auto& s = spawns.at(extra[which]);
            std::string name = s.name;
            while (!name.empty() && name.back() >= '0' && name.back() <= '9') name.pop_back(); // CleanPetName
            out.shown = true;
            out.text = name;
            out.has_gauge = true;
            out.gauge = std::clamp(static_cast<int>(s.hp_percent), 0, 100) * 10;
            out.gauge_text = name;
        }
    }

    std::optional<BuffInfo> buff(EqHost& host, std::string_view id, unsigned) override {
        int n = index_after(id, "PetBuff");
        if (!host.conn || n < 0) return std::nullopt;
        const auto& buffs = host.conn->pet_buffs();
        if (n >= static_cast<int>(buffs.size())) return std::nullopt;
        const auto& b = buffs[static_cast<std::size_t>(n)].second;
        const auto* s = host.conn->spell_table().get(b.spell);
        if (!s) return std::nullopt;
        BuffInfo info{b.spell, game::buff_is_permanent(b.tics, s) ? -1 : static_cast<std::int64_t>(game::buff_ms_left(b.tics, b.received_ms, net::GameConnection::now_ms())), s->name, s->beneficial()};
        return info;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn) return false;
        if (int b = pet_button_index(id); b >= 0) {
            if (button == "rightdown" || button == "right") {
                command_menu(host, b);
                return true;
            }
            if (button != "left") return button.starts_with("right");
            // WndNotification message 1 from the button the map holds for a command: its words to cmd_pet; without
            // the pet (the local player's PetID) nothing. A disabled button sends no click.
            const auto& layout = host.pet_buttons;
            int i = layout.command_of(b);
            if (pet(host) && layout.enabled(b) && i >= 0) host.conn->pet_command_words(std::string(game::pet_command_words(i)));
            return true;
        }
        if (button != "left") return false;
        if (id == "PetHPGauge") {
            host.conn->set_target(host.conn->pets().focused);
            return true;
        }
        if (id == "Pet2HPGauge" || id == "Pet3HPGauge") {
            // PetWindow::HandleClick: the pet that gauge shows becomes the target (on NMS the server then makes it
            // the focused pet, Handle_OP_TargetMouse → ConfigurePetWindow).
            auto extra = host.conn->pet_window_extra_pets();
            std::size_t n = id == "Pet2HPGauge" ? 0 : 1;
            if (n >= extra.size()) return false;
            host.conn->set_target(extra[n]);
            return true;
        }
        return false;
    }

    // WndNotification message 0x17 (a drag from a command button): the hot button "Pet <its text>" of the button's
    // index (game/pet_buttons.h); a button holding no command is disabled and gives nothing.
    std::optional<game::HotButton> pick_up(EqHost& host, std::string_view id, int) override {
        int b = pet_button_index(id);
        if (!host.conn || b < 0 || !host.pet_buttons.enabled(b)) return std::nullopt;
        return game::pet_command_hot_button(b, host.conn->strings().format(game::kStrPet, {}), host.pet_buttons.text(b));
    }

private:
    ui::UiIni* ini_ = nullptr;
    std::filesystem::path ini_dir_; // the character folder whose UI ini the window was made from
    bool popup_ = true;    // +0x765
    bool had_pet_ = false; // +0x764: the pet state the window last saw
    // CPetInfoWnd_LoadButtonToggleStates @0x70ed20: PetButtonToggleState0..9 (ReadUIINIBool, default 0).
    void load_toggles(EqHost& host) {
        game::PetToggleStates st{};
        if (ini_)
            for (int index = 4; index < 14; ++index)
                game::set_pet_toggle(st, index, game::ini_bool_value(ini_->get("PetInfoWindow", game::pet_toggle_ini_key(index)), false));
        host.conn->load_pet_toggles(st);
    }
    // A new connection in the world (a login, camp and back): the toggle bytes from the UI ini, as the constructor
    // reads them, then the window's side of cmd_pet and the pet's arrival (its first refresh).
    void link(EqHost& host) {
        net::GameConnection::PetWindowLink l;
        EqHost* h = &host;
        l.usable = [h](std::uint32_t command) {
            const game::Spawn* p = h->conn ? pet(*h) : nullptr;
            return p && game::can_use_pet_command(query(*h, *p), command);
        };
        l.available = [h](std::uint32_t command) { return game::pet_command_available(h->pet_buttons, command); };
        l.saved = [this](int index, bool on) {
            // CPetInfoWnd_SaveButtonToggleState: WriteBoolToUIINI.
            if (!ini_) return;
            ini_->put("PetInfoWindow", game::pet_toggle_ini_key(index), on ? "1" : "0");
            ini_->save();
        };
        load_toggles(host);
        host.conn->link_pet_window(std::move(l));
    }
    // What CanUsePetCommand reads of the pet and the character.
    static game::PetCommandQuery query(EqHost& host, const game::Spawn& p) {
        game::PetCommandQuery q;
        q.has_pet = true;
        q.race = p.race;
        q.summoned = p.summoned;
        q.property_1f = std::find(p.properties.begin(), p.properties.end(), 0x1fu) != p.properties.end();
        if (const auto* ch = host.character)
            q.effect = [ch](std::int32_t spa, std::int32_t sub) { return ch->total_effect(spa, true, sub, true, true); };
        return q;
    }
    // CPetInfoWnd_SavePetButtonState: WriteIniString to the UI ini, the window's section.
    void save_layout(const game::PetButtonLayout& layout) {
        if (!ini_) return;
        ini_->put("PetInfoWindow", "PetButtonState", layout.save());
        ini_->save();
    }
    // WndNotification 0x2d: +0x8fc = the button, and the command menu (+0x8f8) pops up at the mouse; a row chosen
    // (0x14) goes to that button (game::PetButtonLayout::choose), then SavePetButtonState.
    void command_menu(EqHost& host, int b) {
        std::vector<EqHost::MenuRow> rows;
        for (const auto& r : host.pet_buttons.menu()) {
            EqHost::MenuRow row;
            row.text = std::string(r.name);
            const int item = r.item;
            row.act = [this, &host, b, item] {
                host.pet_buttons.choose(b, item);
                save_layout(host.pet_buttons);
            };
            rows.push_back(std::move(row));
        }
        // The mouse (CXWndManager +0x94 / +0x98); a click not from the mouse (a pad) opens it at the button.
        const auto& r = host.click_rect;
        ImVec2 mp = ImGui::GetMousePos();
        bool on_button = ImGui::IsMousePosValid(&mp) && r[2] >= 0 && mp.x >= r[0] && mp.x <= r[2] && mp.y >= r[1] && mp.y <= r[3];
        if (!on_button && r[2] >= 0) mp = {r[2], (r[1] + r[3]) / 2};
        host.open_menu(mp.x, mp.y, std::move(rows));
    }
    static const game::Spawn* pet(EqHost& host) {
        const auto& w = host.conn->world();
        auto it = w.spawns.find(host.conn->pets().focused);
        return host.conn->pets().focused && it != w.spawns.end() ? &it->second : nullptr;
    }
};

} // namespace

std::optional<bool> pet_hot_button_checked(EqHost& host, std::string_view label) {
    if (!host.conn) return std::nullopt;
    const auto& c = *host.conn;
    // Only a label of the pet word, a space and a command can match (game::pet_hot_button_checked compares the whole
    // label with each "<pet word> <command>"); any other leaves the commands unread: each hot button asks every frame.
    const std::string pet_word = c.strings().format(game::kStrPet, {});
    if (label.size() <= pet_word.size() || label[pet_word.size()] != ' ') return std::nullopt;
    for (std::size_t i = 0; i < pet_word.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(label[i])) != std::tolower(static_cast<unsigned char>(pet_word[i])))
            return std::nullopt;
    const auto& layout = host.pet_buttons;
    // SyncHotButtonStates walks the map (+0x26c), command index order.
    game::PetButtonFace faces[game::kPetCommandCount];
    for (int i = 0; i < game::kPetCommandCount; ++i) {
        int b = layout.button_of(i);
        if (b < 0) continue;
        faces[i].text = layout.text(b);
        faces[i].checked = pet_command_checked(c, layout, i);
    }
    return game::pet_hot_button_checked(label, pet_word, faces);
}

namespace {

// ---------------------------------------------------------------- ActionsWindow (CActionsWnd)

// The ability slot a right click on the Actions window picks a skill for (SkillsSelectWindow); slot -1 none.
struct SkillPick {
    bool combat = false;
    int slot = -1;
};
SkillPick g_skill_pick;

class ActionsWindow : public StockWindow {
public:
    ActionsWindow() : StockWindow("ActionsWindow", false) {}

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& c = *host.conn;
        // The main page's pairs share a place; one of each shows: what the player can do now.
        if (id == "AMP_SitButton") out.shown = !c.sitting();
        else if (id == "AMP_StandButton") out.shown = c.sitting();
        else if (id == "AMP_RunButton") out.shown = !c.running();
        else if (id == "AMP_WalkButton") out.shown = c.running();
        else if (id == "AMP_InviteButton") out.shown = c.group().inviter.empty();
        else if (id == "AMP_FollowButton") out.shown = !c.group().inviter.empty();
        else if (id == "AMP_EndFindButton") out.shown = c.player_path().active() || host.guide_line;
        else if (id == "ACP_MeleeAttackButton" || id == "ACP_RangeAttackButton") {
            // CombatPageOnProcessFrame: both checked while the primary swing is not ready or auto fire is on.
            HostButtonQueries q(host);
            out.checked = game::weapon_button_checked(id == "ACP_MeleeAttackButton", q, hotbutton_extras(host));
        } else if (auto s = ability(host, id)) {
            out.text = *s >= 0 && host.skill_name ? host.skill_name(static_cast<std::uint32_t>(*s)) : "";
            // 0x613a70 (Abilities) / CombatPageOnProcessFrame: the skill's timer and SetCheck(!IsAvailable).
            game::ButtonCoolDown& cd = cool_downs_[static_cast<std::size_t>(ability_index(id) + (id.starts_with("ACP_") ? 6 : 0))];
            if (*s < 0) {
                cd.stop();
            } else {
                HostButtonQueries q(host);
                bool checked = false;
                game::update_skill_button(cd, checked, static_cast<std::uint32_t>(*s), q);
                out.checked = checked;
                out.cool_down = cd.overlay_fraction(q.now());
            }
        } else if (int n = index_after(id, "ASP_SocialButton"); n >= 1 && n <= game::kSocialButtons && host.hot_buttons) {
            const int index = social_page_ * game::kSocialButtons + n - 1;
            const game::Social& so = host.hot_buttons->social(index);
            out.text = so.name;
            out.color = social_text_color(so.color); // UpdateButtonText: GetSocialTextColor(the social's colour)
            // SocialsPageOnProcessFrame: SetCheck(SetCoolDown(the /timer start, length)).
            game::ButtonCoolDown& cd = social_cool_downs_[static_cast<std::size_t>(n - 1)];
            const std::uint32_t now = net::GameConnection::eq_time();
            out.checked = cd.set_running(now, so.cooldown_start, so.cooldown_length);
            out.cool_down = cd.overlay_fraction(now);
        } else if (id == "ASP_CurrentSocialPageLabel") {
            out.text = std::to_string(social_page_ + 1);
        }
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn) return false;
        if (button == "right" || button == "rightup") {
            // SocialsPageWndNotification: a right click (notification 3, the button's right button up) on a social
            // button opens the social editor on it, empty or not.
            if (int n = index_after(id, "ASP_SocialButton"); n >= 1 && n <= game::kSocialButtons) {
                if (host.open_social_editor)
                    host.open_social_editor(social_page_ * game::kSocialButtons + n - 1);
                return true;
            }
            // A right click on an ability button chooses its skill (the selector windows; not decoded).
            if (!ability(host, id)) return false;
            g_skill_pick = {id.starts_with("ACP_"), ability_index(id)};
            return true;
        }
        if (button != "left") return false;
        auto& c = *host.conn;
        auto menu = [&](game::MenuButton m) {
            game::HotButton b;
            b.type = game::HotButtonType::MenuButton;
            b.slot = static_cast<std::int32_t>(m);
            if (host.run_button) host.run_button(b);
        };
        if (id == "AMP_WhoButton") menu(game::MenuButton::Who);
        else if (id == "AMP_InviteButton") c.group_invite();
        else if (id == "AMP_FollowButton") c.accept_group_invite();
        else if (id == "AMP_DisbandButton") menu(game::MenuButton::Disband);
        else if (id == "AMP_CampButton") menu(game::MenuButton::Camp);
        else if (id == "AMP_SitButton") c.set_sitting(true);
        else if (id == "AMP_StandButton") c.set_sitting(false);
        else if (id == "AMP_RunButton") c.set_running(true);
        else if (id == "AMP_WalkButton") c.set_running(false);
        else if (id == "AMP_FindButton") menu(game::MenuButton::Find); // the find key: Hailstone's Find window
        else if (id == "AMP_EndFindButton") {
            if (c.player_path().active()) c.stop_find_path();
            if (host.guide_line && host.end_guide) host.end_guide();
        }
        else if (id == "ACP_MeleeAttackButton" || id == "ACP_RangeAttackButton") {
            game::HotButton b;
            b.type = game::HotButtonType::WeaponSlot;
            b.slot = id == "ACP_MeleeAttackButton" ? 0 : 1;
            if (host.run_button) host.run_button(b);
        } else if (auto s = ability(host, id)) {
            if (*s < 0) { // an empty button: choose its skill, as its right click does
                g_skill_pick = {id.starts_with("ACP_"), ability_index(id)};
                return true;
            }
            // CActionsWnd: a type 2 / 3 button whose value is the list index (DoHotButton reads the list entry).
            game::HotButton b;
            b.type = id.starts_with("ACP_") ? game::HotButtonType::CombatSkill : game::HotButtonType::Ability;
            b.slot = ability_index(id);
            if (host.run_button) host.run_button(b);
        } else if (int n = index_after(id, "ASP_SocialButton"); n >= 1) {
            game::HotButton b;
            b.type = game::HotButtonType::Social;
            b.slot = social_page_ * game::kSocialButtons + n - 1;
            if (host.hot_buttons && !host.hot_buttons->social(b.slot).empty() && host.run_button) host.run_button(b);
        } else if (id == "ASP_SocialPageLeftButton") {
            social_page_ = (social_page_ + game::kSocialPages - 1) % game::kSocialPages;
        } else if (id == "ASP_SocialPageRightButton") {
            social_page_ = (social_page_ + 1) % game::kSocialPages;
        } else {
            return false;
        }
        return true;
    }

    // CActionsWnd: dragging a button makes a hotbutton of it (the same button its click runs).
    std::optional<game::HotButton> pick_up(EqHost& host, std::string_view id, int) override {
        game::HotButton b;
        static constexpr std::pair<std::string_view, game::MenuButton> kMenu[] = {
            {"AMP_WhoButton", game::MenuButton::Who},         {"AMP_InviteButton", game::MenuButton::InviteFollow},
            {"AMP_FollowButton", game::MenuButton::InviteFollow}, {"AMP_DisbandButton", game::MenuButton::Disband},
            {"AMP_CampButton", game::MenuButton::Camp},       {"AMP_SitButton", game::MenuButton::Sit},
            {"AMP_StandButton", game::MenuButton::Sit},       {"AMP_RunButton", game::MenuButton::RunWalk},
            {"AMP_WalkButton", game::MenuButton::RunWalk},    {"AMP_FindButton", game::MenuButton::Find},
        };
        for (const auto& [name, m] : kMenu)
            if (id == name) {
                b.type = game::HotButtonType::MenuButton;
                b.slot = static_cast<std::int32_t>(m);
                return b;
            }
        if (id == "ACP_MeleeAttackButton" || id == "ACP_RangeAttackButton") {
            b.type = game::HotButtonType::WeaponSlot;
            b.slot = id == "ACP_MeleeAttackButton" ? 0 : 1;
        } else if (auto s = ability(host, id)) {
            if (*s < 0) return std::nullopt;
            b.type = id.starts_with("ACP_") ? game::HotButtonType::CombatSkill : game::HotButtonType::Ability;
            b.slot = ability_index(id);
        } else if (int n = index_after(id, "ASP_SocialButton"); n >= 1 && n <= game::kSocialButtons) {
            b.type = game::HotButtonType::Social;
            b.slot = social_page_ * game::kSocialButtons + n - 1;
            if (!host.hot_buttons || host.hot_buttons->social(b.slot).empty()) return std::nullopt;
        } else {
            return std::nullopt;
        }
        return b;
    }

    std::optional<int> tab_page(EqHost&, std::string_view id) override {
        if (id == "ACTW_ActionsSubwindows") return page_;
        return std::nullopt;
    }
    void set_tab_page(EqHost&, std::string_view id, int page) override {
        if (id == "ACTW_ActionsSubwindows") page_ = page;
    }

private:
    // The ability buttons: Abilities page 1..6 (the [Abilities] slots), Combat page 1..4 ([CombatSkills]).
    static std::int32_t ability_index(std::string_view id) {
        static constexpr std::string_view kOrd[] = {"First", "Second", "Third", "Fourth", "Fifth", "Sixth"};
        for (int i = 0; i < 6; ++i)
            if (id.substr(4).starts_with(kOrd[i])) return i;
        return -1;
    }
    static std::optional<std::int32_t> ability(EqHost& host, std::string_view id) {
        static constexpr std::string_view kOrd[] = {"First", "Second", "Third", "Fourth", "Fifth", "Sixth"};
        if (!host.hot_buttons) return std::nullopt;
        for (int i = 0; i < 6; ++i) {
            if (id == std::string("AAP_") + std::string(kOrd[i]) + "AbilityButton") return host.hot_buttons->abilities[static_cast<std::size_t>(i)];
            if (i < 4 && id == std::string("ACP_") + std::string(kOrd[i]) + "AbilityButton")
                return host.hot_buttons->combat_skills[static_cast<std::size_t>(i)];
        }
        return std::nullopt;
    }
    int page_ = 0;
    int social_page_ = 0;
    std::array<game::ButtonCoolDown, 10> cool_downs_{};       // Abilities 1..6, then Combat 1..4
    std::array<game::ButtonCoolDown, 12> social_cool_downs_{}; // the page's social buttons
};

// ---------------------------------------------------------------- SkillsSelectWindow (CSkillsSelectWnd)

// The skills the character has that the Actions window's Abilities / Combat buttons take (the activated skills
// game/mechanics/use_skill.cpp handles, split as this client does: the original's lists are not decoded). Both
// pages use this one window (CActionsWnd: CSkillsSelectWnd::SetTypesToDisplay 3 for the Abilities page, 2 for the
// Combat page); a click puts the skill on the button chosen with a right click and closes. (CombatSkillSelectWnd is
// the combat ability window's list, app/lua_eq_abilities.cpp.)
class SkillSelectWindow : public StockWindow {
public:
    SkillSelectWindow() : StockWindow("SkillsSelectWindow", false) {}

    bool shown(EqHost& host) override { return host.in_game && g_skill_pick.slot >= 0; }
    void set_shown(EqHost&, bool show) override {
        if (!show) g_skill_pick.slot = -1;
    }
    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn || !host.actions || id != "SKSW_SkillSelectorList") return nullptr;
        list_ = {};
        skills_.clear();
        list_.rows.push_back(ListRow{{"(none)"}});
        skills_.push_back(-1);
        for (int s : g_skill_pick.combat ? std::span<const int>(kCombat) : std::span<const int>(kAbilities)) {
            if (s < 100 && host.character && host.character->sheet().skills[static_cast<std::size_t>(s)] <= 0) continue;
            list_.rows.push_back(ListRow{{host.actions->skill_label(*host.conn, static_cast<std::uint32_t>(s))}});
            skills_.push_back(s);
        }
        return &list_;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool, int) override {
        if (id != "SKSW_SkillSelectorList" || !host.actions || row < 0 || row >= static_cast<int>(skills_.size()))
            return false;
        host.actions->set_ability_button(g_skill_pick.combat, g_skill_pick.slot, skills_[static_cast<std::size_t>(row)]);
        g_skill_pick.slot = -1;
        return true;
    }

private:
    static constexpr int kAbilities[] = {9, 17, 25, 27, 29, 32, 35, 40, 42, 48, 53, 55, 62, 67, 75};
    static constexpr int kCombat[] = {8, 10, 16, 21, 23, 26, 30, 38, 52, 71, 73, 74, game::mechanics::skill::kSlam};
    ListData list_;
    std::vector<std::int32_t> skills_;
};

// ---------------------------------------------------------------- CompassWindow (CCompassWnd)

class CompassWindow : public StockWindow {
public:
    CompassWindow() : StockWindow("CompassWindow", true) {}
};

// ---------------------------------------------------------------- AggroMeterWnd (CAggroMeterWnd, not decoded)

// Its labels and gauge are EQTypes (app/lua_eq_values.cpp 149, 302..307); the window opens when the server first
// sends the meter.
class AggroMeterWindow : public StockWindow {
public:
    AggroMeterWindow() : StockWindow("AggroMeterWnd", false) {}
    bool shown(EqHost& host) override { return host.conn && host.conn->aggro().window_open; }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_aggro_window_open(show);
    }
};

// ---------------------------------------------------------------- BreathWindow (CBreathWnd, not decoded)

// Its gauge is EQType 8 (the air supply); shown while the player is short of air.
class BreathWindow : public StockWindow {
public:
    BreathWindow() : StockWindow("BreathWindow", false) {}
    bool shown(EqHost& host) override { return host.in_game && host.air >= 0 && host.air < host.max_air; }
    void set_shown(EqHost&, bool) override {}
};

// ---------------------------------------------------------------- AuraWindow (CAuraWnd, not decoded)

// The auras the server sent (OP_UpdateAura), name and icon; a right click removes one. A plain window, as CAuraWnd
// is: its IniFlags (0xffffffff, the ctor @0x620cd0) show it on a character with no "Show" in the UI ini, and its
// close box and CMD_TOGGLE_AURAWND (__ExecuteCmd case 124: Activate / Deactivate, StoreIniVis) open and close it,
// the state kept in the UI ini. Gaining an aura never opens it (ClientSOIManager_AddAura @0x466db0 only adds its row).
class AuraWindow : public StockWindow {
public:
    AuraWindow() : StockWindow("AuraWindow", false) {}
    bool shown(EqHost& host) override { return host.conn && open_; }
    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn || id != "AuraList") return nullptr;
        list_ = {};
        for (const auto& a : host.conn->auras()) {
            ListRow row{{a.name}};
            if (host.textures) row.icon = host.textures->spell_icon(static_cast<std::int32_t>(a.icon));
            list_.rows.push_back(std::move(row));
        }
        return &list_;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool right, int) override {
        if (id != "AuraList" || !host.conn) return false;
        const auto& a = host.conn->auras();
        if (right && row >= 0 && row < static_cast<int>(a.size())) host.conn->remove_aura(a[static_cast<std::size_t>(row)].spawn_id);
        return true;
    }

private:
    ListData list_;
};

// ---------------------------------------------------------------- SpellBookWnd (CSpellBookWnd)

class SpellBookWindow : public StockWindow {
public:
    SpellBookWindow() : StockWindow("SpellBookWnd", false) {}
    // CSpellBookWnd's open and close rules are the connection's (game/spell_book.h): AboutToShow may refuse
    // (open_spell_book() false: its message is printed, the window stays closed); standing up, moving and zoning close
    // the book, so the window follows spell_book_open().
    bool shown(EqHost& host) override {
        if (!host.conn) return false;
        if (int p = host.conn->spellbook_page(); p != book_page_) left_page_ = book_page_ = p; // /book <n>
        return host.conn->spell_book_open();
    }
    void set_shown(EqHost& host, bool show) override {
        if (!host.conn || show == host.conn->spell_book_open()) return;
        if (show) host.conn->open_spell_book();
        else host.conn->close_spell_book();
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& c = *host.conn;
        // UpdateSpellBookDisplay: page numbers, then each slot's icon (CBuffWindow::SetBuffIcon) and name.
        if (id == "SBW_LeftPageNum") out.text = std::to_string(left_page_ + 1);
        else if (id == "SBW_RightPageNum") out.text = std::to_string(left_page_ + 2);
        else if (int n = index_after(id, "SBW_SpellName"); n >= 0 && n < 16) {
            const auto* s = spell(c, n);
            out.text = s ? s->name : std::string();
        } else if (int m = index_after(id, "SBW_Spell"); m >= 0 && m < 16) {
            const auto* s = spell(c, m);
            if (!s) {
                out.background = none();
                out.decal = none();
                out.tooltip = "";
                return;
            }
            out.background = anim(s->beneficial() ? "BlueIconBackground" : "RedIconBackground");
            if (left_page_ * 8 + m == c.spell_book_swap_slot()) {
                // The slot picked up to move (SelectedBookIndex): SetBuffIcon with a static stepping -1, 0, 1 on
                // every redraw (OnProcessFrame redraws each frame while one is picked): none, red, blue in turn.
                const int step = ImGui::GetFrameCount() % 3;
                out.background = step == 0 ? none() : anim(step == 1 ? "RedIconBackground" : "BlueIconBackground");
            }
            out.decal = host.textures ? image(host.textures->spell_icon(s->spell_icon)) : none();
            out.tooltip = s->name;
        } else if (id == "SBW_Memorize_Gauge") {
            // The memorize and scribe gauges share the ScreenID: shown while either runs.
            out.shown = c.spell_state().memorizing.has_value() || c.spell_state().scribing.has_value();
        }
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool outside) override {
        if (!host.conn) return false;
        auto& c = *host.conn;
        // WndNotification @0x75e810, XWM_LCLICK: PageUp (the right-hand arrow) turns forward, PageDown back
        // (TurnToPage clamps to 0 .. 88).
        if (button == "left" && id == "SBW_PageUp_Button") left_page_ = std::min(game::kSpellBookSize / 8 - 2, left_page_ + 2);
        else if (button == "left" && id == "SBW_PageDown_Button") left_page_ = std::max(0, left_page_ - 2);
        else if (button == "left" && id == "DoneButton") set_shown(host, false);
        else if (int m = index_after(id, "SBW_Spell"); m >= 0 && m < 16 && id.find("Name") == std::string_view::npos) {
            int slot = left_page_ * 8 + m;
            // HandleSpellInfoDisplay: the slot's spell in a spell display, on XWM_RCLICKHOLD (24) or a left click
            // with Alt; XWM_RBUTTONUPAFTERHELD (26) closes the held one again (CWndDisplayManager_CloseNewest).
            const bool alt = ImGui::GetIO().KeyAlt;
            if (button == "rightheld" || (button == "left" && alt)) {
                if (const auto* s = spell(c, m)) host.show_spell(static_cast<std::int32_t>(s->id), button == "rightheld");
                return true;
            }
            if (button == "rightupheld") {
                std::erase_if(host.spell_displays, [](const auto& d) { return d.second; });
                return true;
            }
            // XWM_RCLICK (3, a quick right click released on the slot): Send_OP_SwapSpell, picking the slot up or
            // swapping it with the one picked.
            if ((button == "rightup" || button == "right") && !outside) {
                c.spell_book_swap_click(slot);
                return true;
            }
            if (button != "left") return true;
            // CSpellBookWnd @0x75e790, sitting (or mounted): a scroll on the cursor is scribed into the slot
            // (StartSpellScribe); else the spell goes on the cursor (AttachToCursor type 1, the book slot) when
            // CanStartMemming allows, for a click on a gem to memorize it there.
            if (!c.sitting()) return true;
            if (c.cursor_scroll_spell() != 0) {
                c.scribe_cursor(slot);
            } else if (c.can_start_memming(slot)) {
                host.cursor_button.reset();
                host.cursor_spell = slot;
                ++host.cursor_serial;
            }
        } else return false;
        return true;
    }

private:
    const game::SpellData* spell(const net::GameConnection& c, int i) const {
        int slot = left_page_ * 8 + i;
        if (slot < 0 || slot >= game::kSpellBookSize) return nullptr;
        std::uint32_t id = c.spell_state().book[static_cast<std::size_t>(slot)];
        return id == game::kNoSpell ? nullptr : c.spell_table().get(id);
    }
    int left_page_ = 0; // CSpellBookWnd::LeftPageNbr
    int book_page_ = 0; // the page /book <n> asked for last
};

} // namespace

// The compass's heading error (CCompassWnd::OnProcessFrame, game/compass.h), in heading units.
static int compass_error(const EqHost& host) { return host.actions ? host.actions->compass().error() : 0; }

// left1, left2, w, degrees = eq.CompassStrips()   -- where CCompassWnd::OnProcessFrame puts the two strips for the
// heading: x = trunc(heading * 0.3515625 - 145) + 47, wrapped below 1 by 180; the second 180 to the right. The heading
// is the one the compass shows: the true heading plus its error below Sense Heading 200 (game/compass.h), so degrees
// (the 4th answer) carries the error too.
int CompassStrips(lua_State* L) {
    auto* host = static_cast<EqHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!host || !host->in_game) {
        lua_pushnil(L);
        return 1;
    }
    // (trunc(heading) + error) % 512, a C remainder (negative when the error takes it below 0, as the original's)
    const int h = (static_cast<int>(host->heading) + compass_error(*host)) % 512;
    int x = static_cast<int>(static_cast<double>(h) * 0.3515625 - 145.0) + 0x2f;
    if (x > 0) x = x - 180 - ((x - 1) / 180) * 180;
    lua_pushinteger(L, x);
    lua_pushinteger(L, x + 180);
    lua_pushinteger(L, 180);
    // the heading the compass shows in degrees (to the left), 0..360
    double shown = std::fmod(static_cast<double>(host->heading) + compass_error(*host), 512.0);
    if (shown < 0.0) shown += 512.0;
    lua_pushnumber(L, shown * 360.0 / 512.0);
    return 4;
}

// { x, ... } = eq.CompassMarkers()   -- this client's marks on the compass (not decoded): the window x of each point
// GameConnection::compass_points gives, the strips' half a pixel per degree from the centre (x 49 faces the heading;
// a point to the left has a larger heading), only those within the window's 98 pixels.
int CompassMarkers(lua_State* L) {
    auto* host = static_cast<EqHost*>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_newtable(L);
    if (!host || !host->in_game || !host->conn) return 1;
    const game::Spawn* me = host->conn->world().player();
    if (!me) return 1;
    int n = 0;
    for (const auto& at : host->conn->compass_points()) {
        // heading units, 512 a turn; from the heading the compass shows
        float d = host->heading + static_cast<float>(compass_error(*host)) - game::heading_to(me->position, at);
        d = std::fmod(d + 256.0f, 512.0f);
        if (d < 0) d += 512.0f;
        d -= 256.0f;
        float x = 49.0f + d * 0.3515625f;
        if (x < 0.0f || x > 98.0f) continue;
        lua_pushnumber(L, x);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// ---------------------------------------------------------------- TrackingWnd (CTrackingWnd)

// The tracking window over the connection's list (game/tracking.h): the names in window order with GetTrackColor's
// chat colours, Track (OP_TrackTarget), Cancel, the ranger's con filters and sort, the player / pet / mercenary
// filters and the auto update check box (OP_Track every 6 s).
class TrackingWindow : public StockWindow {
public:
    TrackingWindow() : StockWindow("TrackingWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->tracking().open; }
    void set_shown(EqHost& host, bool show) override {
        if (!host.conn) return;
        if (show && !host.conn->tracking().open) host.conn->open_tracking();
        else if (!show) host.conn->close_tracking();
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& t = host.conn->tracking();
        // CTrackingWnd::sub_00779ed0: the con filters and the sort are a ranger's (disabled, and shown as 0, for
        // anyone else).
        const bool ranger = ranger_(host);
        for (const auto& [name, bit, letters] : kFilters)
            if (id == name) {
                out.checked = ranger && (t.filter.con_mask & bit) != 0;
                out.enabled = ranger;
            }
        if (id == "TRW_TrackSortCombobox") out.enabled = ranger;
        if (id == "TRW_AutoUpdateButton") out.checked = t.auto_update;
        else if (id == "TRW_TrackPetsCheckbox") out.checked = t.filter.pets;
        else if (id == "TRW_TrackMercsCheckbox") out.checked = t.filter.mercs;
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn || id != "TRW_TrackingList") return nullptr;
        list_ = {};
        list_.columns = 1;
        list_.labels = {""};
        list_.widths = {200};
        ids_.clear();
        for (const auto& l : host.conn->tracking_lines()) {
            if (l.spawn_id == selected_id_) list_.selected = static_cast<int>(list_.rows.size());
            list_.rows.push_back({{l.text}, argb_to_rgb(0xff000000u | chat_index_rgb(static_cast<int>(l.color))), std::nullopt});
            ids_.push_back(l.spawn_id);
        }
        return &list_;
    }
    bool list_click(EqHost&, std::string_view id, int row, bool, int) override {
        if (id != "TRW_TrackingList" || row < 0 || row >= static_cast<int>(ids_.size())) return false;
        selected_id_ = ids_[static_cast<std::size_t>(row)];
        return true;
    }

    const ComboData* combo(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& t = host.conn->tracking();
        auto fill = [&](ComboData& c, std::initializer_list<std::uint32_t> ids, int selected) {
            c.choices.clear();
            for (std::uint32_t s : ids) c.choices.push_back(host.conn->strings().format(s, {}));
            c.selected = selected;
            return &c;
        };
        // The constructor's choices: normal, alpha, distance, consider, ralpha, rdistance, rconsider; off, on, Group.
        if (id == "TRW_TrackSortCombobox")
            return fill(sort_, {0x3340, 0x18d1, 0x3358, 0x3359, 0x18d2, 0x335a, 0x335b},
                        ranger_(host) ? static_cast<int>(t.sort_order) : 0);
        if (id == "TRW_TrackPlayersCombobox") return fill(players_, {0x3292, 0x3293, 0x335c}, t.filter.players);
        return nullptr;
    }
    // CTrackingWnd::WndNotification: a choice runs /tracksort with its letters, or /trackplayers with the on / off /
    // group word (so it prints, is saved and refilters as the command does).
    bool combo_select(EqHost& host, std::string_view id, int index) override {
        if (!host.conn || index < 0) return false;
        auto& c = *host.conn;
        static constexpr const char* kSorts[] = {"N", "A", "D", "C", "RA", "RD", "RC"};
        if (id == "TRW_TrackSortCombobox" && index <= 6) c.track_window_command("Cmd_tracksort", kSorts[index]);
        else if (id == "TRW_TrackPlayersCombobox" && index <= 2) {
            static constexpr std::uint32_t kWords[] = {0x3292, 0x3293, 0x335c};
            c.track_window_command("Cmd_trackplayers", c.strings().format(kWords[index], {}));
        } else return false;
        return true;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        auto& c = *host.conn;
        auto on_off = [&](bool on) { return c.strings().format(on ? 0x3293 : 0x3292, {}); };
        for (const auto& [name, bit, letters] : kFilters)
            if (id == name) {
                c.track_window_command("Cmd_trackfilter", letters); // toggles the colour, with its message
                return true;
            }
        if (id == "TRW_TrackButton") {
            if (selected_id_ != 0) c.track_spawn(selected_id_);
        } else if (id == "DoneButton") {
            c.close_tracking();
        } else if (id == "TRW_AutoUpdateButton") {
            c.set_track_auto_update(!c.tracking().auto_update);
        } else if (id == "TRW_TrackPetsCheckbox") {
            c.track_window_command("Cmd_trackpets", on_off(!c.tracking().filter.pets));
        } else if (id == "TRW_TrackMercsCheckbox") {
            c.track_window_command("Cmd_trackmercs", on_off(!c.tracking().filter.mercs));
        } else {
            return false;
        }
        return true;
    }

private:
    static bool ranger_(const EqHost& host) { return host.conn->profile() && host.conn->profile()->class_ == 4; }
    // The con filter buttons, their colour bit and the letters the window passes to /trackfilter.
    struct Filter {
        std::string_view name;
        std::uint32_t bit;
        const char* letters;
    };
    static constexpr Filter kFilters[] = {
        {"TRW_FilterRedButton", game::kTrackRed, "R"},       {"TRW_FilterYellowButton", game::kTrackYellow, "Y"},
        {"TRW_FilterWhiteButton", game::kTrackWhite, "W"},   {"TRW_FilterBlueButton", game::kTrackBlue, "B"},
        {"TRW_FilterLightBlueButton", game::kTrackLightBlue, "LB"}, {"TRW_FilterGreenButton", game::kTrackGreen, "GN"},
        {"TRW_FilterGray", game::kTrackGrey, "GY"}};
    ListData list_;
    std::vector<std::int32_t> ids_;
    std::int32_t selected_id_ = 0;
    ComboData sort_, players_;
};

namespace {

// CRespawnWnd: the hover respawn choices OP_RespawnWindow sent (OptionsList: their names; a click picks one, the
// server's selection first), the timer gauge (EQType 28), and Respawn (Select: once the
// window's delay has passed, OP_RespawnWindow {option}; then the list and button stay disabled until the server
// moves us, GameConnection::choose_respawn). Shown while the server's window is there.
class RespawnWindow : public StockWindow {
public:
    RespawnWindow() : StockWindow("RespawnWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->respawn_window().has_value(); }
    void set_shown(EqHost&, bool) override {} // the server's window: it closes when we are moved

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn || !host.conn->respawn_window()) return;
        sync_selection(host);
        const auto& w = *host.conn->respawn_window();
        // TimeGauge is the XML's EQType 28 (app/lua_eq_values.cpp).
        if (id == "SelectButton") {
            const auto* selected = option(w);
            out.enabled = host.conn->respawn_ready() && selected && host.conn->respawn_option_available(selected->id);
        } else if (id == "OptionsList") {
            out.enabled = !host.conn->respawn_chosen();
        }
    }
    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn || !host.conn->respawn_window() || id != "OptionsList") return nullptr;
        sync_selection(host);
        const auto& w = *host.conn->respawn_window();
        // Handle_OP_RespawnWindow @0x74ec70 (the window's AddOption) fills a line per option: a marker cell ("*"
        // @0x9c4678 for the default option, "r" @0x9cf320 for one whose final byte is set, else empty), the option's
        // name and the zone's full name (GetFullZoneNameById); a line with that byte set is red (0xffff0000) and
        // added disabled. On EQEmu that byte is availability (RoF2 RespawnOptions_Struct), so `disabled` follows
        // GameConnection::respawn_option_available: a yes to a resurrection offer here enables the disabled lines
        // (ConfirmationDialog_HandleResponse @0x52d540 case 0x67, 0x74e9e0 recolours them green, 0xff00ff00) and
        // leaves the selection and the marker as they were. The colours of the reserved row (red, then green)
        // and the grey of another unavailable one are this client's. The XML's columns.
        list_ = {};
        list_.columns = 3;
        list_.labels = {"", "Name", "Zone"};
        list_.widths = {10, 100, 100};
        if (!chosen_) chosen_ = w.selected;
        for (const auto& o : w.options) {
            if (o.id == *chosen_) list_.selected = static_cast<int>(list_.rows.size());
            ListRow row;
            const bool available = host.conn->respawn_option_available(o.id);
            row.texts = {o.id == w.selected ? "*" : !o.available() ? "r" : "", o.name, game::zone_long_name(o.zone_id)};
            row.disabled = !available;
            if (host.conn->respawn_option_is_resurrection(o.id)) {
                row.color = available ? Rgb{0.25f, 1.0f, 0.35f} : Rgb{1.0f, 0.25f, 0.25f};
                row.cell_colors.resize(3);
                row.cell_colors[1] = row.color;
                row.cell_colors[2] = row.color;
            } else if (!available) {
                row.color = {0.6f, 0.6f, 0.6f};
            }
            list_.rows.push_back(std::move(row));
        }
        return &list_;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool, int) override {
        if (!host.conn || !host.conn->respawn_window() || id != "OptionsList") return false;
        sync_selection(host);
        const auto& opts = host.conn->respawn_window()->options;
        if (!host.conn->respawn_chosen() && row >= 0 && row < static_cast<int>(opts.size())) {
            chosen_ = opts[static_cast<std::size_t>(row)].id;
            manual_choice_ = true;
        }
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (id != "SelectButton") return false;
        sync_selection(host);
        if (button == "left" && host.conn && host.conn->respawn_window())
            if (const auto* o = option(*host.conn->respawn_window()); o && host.conn->respawn_option_available(o->id))
                host.conn->choose_respawn(o->id);
        return true;
    }
    void frame(EqHost& host) override {
        if (!host.conn || !host.conn->respawn_window()) {
            chosen_.reset();
            manual_choice_ = false;
        }
    }

private:
    void sync_selection(EqHost& host) {
        if (!host.conn) return;
        if (connection_ != host.conn || session_ != host.conn->respawn_session()) {
            connection_ = host.conn;
            session_ = host.conn->respawn_session();
            chosen_.reset();
            manual_choice_ = false;
        }
        // Until a row is clicked the server's selection is shown; accepting a resurrection does not move it
        // (0x74e9e0 only enables and recolours lines).
        if (!manual_choice_ && !host.conn->respawn_chosen() && host.conn->respawn_window())
            chosen_ = host.conn->respawn_window()->selected;
    }
    const net::RespawnOption* option(const net::RespawnWindow& w) const {
        std::uint32_t want = chosen_.value_or(w.selected);
        for (const auto& o : w.options)
            if (o.id == want) return &o;
        return nullptr;
    }

    ListData list_;
    std::optional<std::uint32_t> chosen_;
    bool manual_choice_ = false;
    const net::GameConnection* connection_ = nullptr;
    std::uint32_t session_ = 0;
};

} // namespace

void add_misc_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<RespawnWindow>());
    out.push_back(std::make_unique<TrackingWindow>());
    out.push_back(std::make_unique<PetWindow>());
    out.push_back(std::make_unique<ActionsWindow>());
    out.push_back(std::make_unique<CompassWindow>());
    out.push_back(std::make_unique<AggroMeterWindow>());
    out.push_back(std::make_unique<SkillSelectWindow>());
    out.push_back(std::make_unique<AuraWindow>());
    out.push_back(std::make_unique<BreathWindow>());
    out.push_back(std::make_unique<SpellBookWindow>());
    out.push_back(make_player_notes_window());
    out.push_back(std::make_unique<StockWindow>("BrowserWnd", false)); // the in-game browser (app/lua_eq_web.cpp)
    out.push_back(std::make_unique<StockWindow>("ScriptsWnd", false)); // the Lua scripts (app/lua_eq_scripts.cpp)
}

} // namespace hailstone::app
