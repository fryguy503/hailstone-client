// The HUD windows: player, target, group, buffs, songs, spell gems, casting bar and hot bars. Each model follows
// the original window class: which children its OnProcessFrame / Update* shows, what it
// sets on them, and what its WndNotification does for a click.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <ctime>

#include <imgui.h>

#include "app/actions_ui.h"
#include "app/lua_eq_values.h"
#include "app/lua_eq_windows.h"
#include "game/combat.h"
#include "game/combat_ability_menu.h"
#include "app/lua_eq_hotbar.h"
#include "ui/ui_ini.h"
#include "game/hot_button_menu.h"
#include "game/hot_button_sets.h"
#include "net/market_packets.h"
#include "game/hotbuttons.h"
#include "core/game_dir.h"
#include "game/spell_gem.h"
#include "game/mechanics/character.h"
#include "game/mechanics/spell_gems.h"
#include "game/spells.h"
#include "game/world_state.h"
#include "mq/lua_mq.h"
#include "net/game_connection.h"

namespace hailstone::app {

namespace {

// The number after `prefix` in a child id ("Buff12" -> 12), or -1.
int index_after(std::string_view id, std::string_view prefix) {
    if (id.size() <= prefix.size() || id.substr(0, prefix.size()) != prefix) return -1;
    int v = -1;
    auto rest = id.substr(prefix.size());
    auto [p, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), v);
    return ec == std::errc() && p == rest.data() + rest.size() ? v : -1;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const game::Spawn* find_spawn(const game::WorldState& w, std::uint32_t id) {
    if (!id) return nullptr;
    auto it = w.spawns.find(id);
    return it == w.spawns.end() ? nullptr : &it->second;
}

const game::Spawn* spawn_named(const game::WorldState& w, const std::string& name) {
    std::string want = lower(name);
    for (const auto& [id, s] : w.spawns)
        if (lower(s.name) == want) return &s;
    return nullptr;
}

std::string self_name(const net::GameConnection& c) {
    if (const auto* me = c.world().player()) return me->name;
    return c.profile() ? c.profile()->name : std::string();
}

// The n-th (0-based) group member other than the player (CGroup_GetNthOtherMember).
const game::GroupMember* other_member(const net::GameConnection& c, int n) {
    if (!c.group().active) return nullptr;
    std::string me = lower(self_name(c));
    int seen = 0;
    for (const auto& m : c.group().slots) {
        if (m.name.empty() || lower(m.name) == me) continue;
        if (seen++ == n) return &m;
    }
    return nullptr;
}

const game::GroupMember* self_member(const net::GameConnection& c) {
    if (!c.group().active) return nullptr;
    std::string me = lower(self_name(c));
    for (const auto& m : c.group().slots)
        if (!m.name.empty() && lower(m.name) == me) return &m;
    return nullptr;
}

bool is_spellcaster(std::uint32_t class_id) { return class_id >= 2 && class_id <= 15 && class_id != 7 && class_id != 9; }

std::int64_t now_ms() { return net::GameConnection::now_ms(); }

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

std::string eqstr(const net::GameConnection& c, std::uint32_t id) { return c.strings().format(id, {}); }

// The spell select menu for a gem (CCastSpellWnd::MakeSpellSelectMenu; defined with the hot bars, which use it too).
std::vector<EqHost::MenuRow> spell_select_rows(EqHost& host, std::int32_t gem);

// GroupRole_GetName: 1 Main Tank, 2 Main Assist, 3 Puller (eqstr through the role table; the English names).
const char* role_name(int role) {
    switch (role) {
    case 1: return "Main Tank";
    case 2: return "Main Assist";
    case 3: return "Puller";
    default: return "";
    }
}

bool member_role(const game::GroupMember& m, int role) {
    return role == 1 ? m.main_tank : role == 2 ? m.main_assist : m.puller;
}

std::vector<EqHost::MenuRow> group_role_rows(EqHost& host, const std::string& name) {
    std::vector<EqHost::MenuRow> rows;
    if (!host.conn || !host.conn->group().active) return rows;
    const auto& c = *host.conn;
    const auto& g = c.group();
    int slot = g.slot_of(name);
    if (slot < 0) return rows;
    const auto& m = g.slots[static_cast<std::size_t>(slot)];
    bool leader = g.is_leader(self_name(c));
    for (int role = 1; role <= 3; ++role) {
        EqHost::MenuRow row;
        row.text = role_name(role);
        row.check = member_role(m, role);
        row.enabled = leader && (!m.merc || role == 1);
        bool on = !row.check;
        row.act = [&host, name, role, on] {
            if (!host.conn) return;
            const auto& group = host.conn->group();
            int current = group.slot_of(name);
            if (current < 0) return;
            // Resolve a stale roster by name and recheck authority, while preserving the displayed toggle's intent.
            const auto& member = group.slots[static_cast<std::size_t>(current)];
            host.conn->send_group_role(member.name, role, on);
        };
        rows.push_back(std::move(row));
    }
    EqHost::MenuRow note;
    note.text = "Assign in the same zone; clear roles from any zone";
    note.enabled = false;
    rows.push_back(std::move(note));
    return rows;
}

void append_window_menu(EqHost& host, std::vector<EqHost::MenuRow>& rows, const std::string& screen) {
    EqHost::MenuRow separator;
    separator.separator = true;
    rows.push_back(std::move(separator));
    EqHost::MenuRow window;
    window.text = "Window";
    window.act = [&host, screen] { host.window_menu = screen; };
    rows.push_back(std::move(window));
}

bool open_member_roles(EqHost& host, const std::string& name, const std::string& screen, float x, float y) {
    auto rows = group_role_rows(host, name);
    if (rows.empty()) return false;
    EqHost::MenuRow heading;
    heading.text = name;
    heading.enabled = false;
    rows.insert(rows.begin(), std::move(heading));
    append_window_menu(host, rows, screen);
    host.open_menu(x, y, std::move(rows));
    return true;
}

ImVec2 role_menu_position(const EqHost& host) {
    ImVec2 pos = ImGui::GetMousePos();
    // Name and icon drawing need not submit an item; an earlier item may even belong to a tooltip.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsMousePosValid(&pos)) return pos;
    const auto& r = host.click_rect;
    if (r[2] >= 0) return ImVec2(r[2], (r[1] + r[3]) / 2);
    if (!ImGui::IsMousePosValid(&pos)) {
        const auto& size = ImGui::GetIO().DisplaySize;
        pos = ImVec2(size.x / 2, size.y / 2);
    }
    return pos;
}

// The buff button of a buff window (CBuffWindow::SetBuffIcon): the spell's icon in A_SpellIcons on a blue
// (beneficial) or red (detrimental) background; nothing for an empty slot.
void buff_button(EqHost& host, const game::BuffSlot& b, Child& out) {
    const auto* s = b.empty() ? nullptr : host.conn->spell_table().get(b.spell);
    if (!s) {
        out.background = none();
        out.decal = none();
        return;
    }
    out.background = anim(s->beneficial() ? "BlueIconBackground" : "RedIconBackground");
    out.decal = host.textures ? image(host.textures->spell_icon(s->spell_icon)) : none();
}

std::optional<BuffInfo> buff_info(EqHost& host, const game::BuffSlot& b) {
    const auto* s = b.empty() ? nullptr : host.conn->spell_table().get(b.spell);
    if (!s) return std::nullopt;
    BuffInfo info;
    info.spell = b.spell;
    info.name = s->name;
    info.beneficial = s->beneficial();
    info.ms_left = game::buff_is_permanent(b.tics, s) ? -1
                                                       : static_cast<std::int64_t>(game::buff_ms_left(b.tics, b.received_ms, now_ms()));
    return info;
}

// ---------------------------------------------------------------- PlayerWindow (CPlayerWnd)

class PlayerWindow : public StockWindow {
public:
    PlayerWindow() : StockWindow("PlayerWindow", true) {}

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& c = *host.conn;
        const auto* me = c.world().player();
        if (id == "PetHP") { // shown with the Petbars option (default on) and a pet
            out.shown = petbars_ && find_spawn(c.world(), c.pets().focused) != nullptr;
        } else if (id == "PlayerMana" || id == "ManaLabel" || id == "ManPercLabel") {
            out.shown = me && c.is_spellcaster(); // IsSpellcaster_3 on the local player (NMS: the class mask)
        } else if (id == "PlayerHP") { // HPTextColor, green for the group leader
            if (c.group().active && c.group().is_leader(self_name(c))) out.color = Rgb{0, 1, 0};
        } else if (id == "PW_CombatStateAnim") {
            combat_state(host, out);
        } else if (id == "GroupRoleTank" || id == "GroupRoleAssist" || id == "GroupRolePuller") {
            int role = id == "GroupRoleTank" ? 1 : id == "GroupRoleAssist" ? 2 : 3;
            const auto* m = self_member(c);
            bool on = m && (role == 1 ? m->main_tank : role == 2 ? m->main_assist : m->puller);
            out.shown = on;
            out.tooltip = on ? role_name(role) : "";
        } else if (id == "PW_NewMailIcon" || id == "PW_ParcelsIcon" || id == "PW_ParcelsOverLimitIcon" ||
                   id == "PW_VoiceVolume" || id == "PW_AggroPctPlayerLabel" || id == "PW_AggroPctSecondaryLabel" ||
                   id == "PW_AggroNameSecondaryLabel") {
            out.shown = false; // no mail, parcels, voice chat or aggro meter on this client (docs/unknowns.md)
        } else if (id.starts_with("A_AttackIndicatorAnim")) {
            // CPlayerWnd::Draw: drawn while auto attack is on, tinted 0xFFFFvv vv with v pulsing 0..254 every 250 ms.
            out.shown = c.auto_attack();
            out.under = true;
            auto ms = static_cast<std::uint64_t>(now_ms());
            float v = static_cast<float>((ms % 250) * 255 / 250) / 255.0f;
            out.tint = std::array<float, 4>{1.0f, v, v, 1.0f};
        }
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn) return false;
        if (id == "PlayerHP" && button == "right") {
            if (!host.context_menus()) return true;
            auto pos = role_menu_position(host);
            return open_member_roles(host, self_name(*host.conn), screen_, pos.x, pos.y);
        }
        if (button != "left") return false;
        // CPlayerWnd::WndNotification: a click on the gauges or role icons targets the player.
        if (id == "PlayerHP" || id == "PlayerMana" || id == "PlayerFatigue" || id.starts_with("GroupRole")) {
            host.conn->set_target(host.conn->world().player_id);
            return true;
        }
        if (id == "PetHP") {
            host.conn->set_target(host.conn->pets().focused);
            return true;
        }
        return false;
    }

private:
    // CPlayerWnd::sub_007184a0: the combat state icon and its tooltip.
    void combat_state(EqHost& host, Child& out) {
        const auto& c = *host.conn;
        // In combat (PcClient vtable +0x80, OP_RestState's flag), the combat timer running (+0x84, its downtime
        // left), debuffed, sitting / regen, or standing.
        if (c.in_combat() || c.rest_downtime_left() != 0) {
            bool combat = c.in_combat();
            out.decal = anim(combat ? "A_PWCSInCombat" : "A_PWCSTimer");
            out.tooltip = eqstr(c, combat ? 0x23d4 : 0x23d9);
            return;
        }
        bool debuff = false;
        for (int i = 0; i < game::kBuffSlots && !debuff; ++i) {
            const auto& b = c.spell_state().buffs[static_cast<std::size_t>(i)];
            const auto* s = b.empty() ? nullptr : c.spell_table().get(b.spell);
            debuff = s && !s->beneficial();
        }
        const char* a;
        std::uint32_t tip;
        if (debuff) {
            a = "A_PWCSDebuff";
            tip = 0x23d5;
        } else if (c.sitting()) {
            a = "A_PWCSRegen";
            tip = 0x23d8;
        } else {
            a = "A_PWCSStanding";
            tip = 0x23d7;
        }
        out.decal = anim(a);
        out.tooltip = eqstr(c, tip);
    }

    bool petbars_ = true; // CPlayerWnd "Petbars" (the constructor's default)
};

// ---------------------------------------------------------------- TargetWindow (CTargetWnd)

class TargetWindow : public StockWindow {
public:
    TargetWindow() : StockWindow("TargetWindow", true) {}

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& c = *host.conn;
        const auto& w = c.world();
        const auto* t = find_spawn(w, w.target_id);
        if (id == "HPLabel" || id == "HPPercLabel") {
            out.shown = t != nullptr;
        } else if (id.starts_with("A_TargetBoxStaticAnim")) {
            // CTargetWnd::Draw: drawn with a target that cons (GetConLevel != 0), tinted with its con colour.
            game::Con con = t && t->id != w.player_id ? game::con_level(*t, c.player_status().level, c.pvp_rules()) : game::Con::None;
            out.shown = con != game::Con::None;
            out.under = true;
            if (con != game::Con::None) {
                Rgb rgb = argb_to_rgb(game::con_rgb(con));
                out.tint = std::array<float, 4>{rgb[0], rgb[1], rgb[2], 1.0f};
            }
        } else if (int i = index_after(id, "Buff"); i >= 0) {
            const game::BuffSlot* b = target_buff(c, i);
            if (b) buff_button(host, *b, out);
            else {
                out.background = none();
                out.decal = none();
            }
        }
    }

    std::optional<BuffInfo> buff(EqHost& host, std::string_view id, unsigned) override {
        int i = index_after(id, "Buff");
        if (!host.conn || i < 0) return std::nullopt;
        const auto* b = target_buff(*host.conn, i);
        return b ? buff_info(host, *b) : std::nullopt;
    }

private:
    // CTargetWnd::RefreshTargetBuffs: the target's buffs (OP_TargetBuffs) in its own button order.
    static const game::BuffSlot* target_buff(const net::GameConnection& c, int i) {
        const auto& st = c.spell_state();
        const auto& w = c.world();
        if (!w.target_id || st.target_buffs_entity != w.target_id) return nullptr;
        if (i < 0 || i >= static_cast<int>(st.target_buffs.size())) return nullptr;
        return &st.target_buffs[static_cast<std::size_t>(i)].second;
    }
};

// ---------------------------------------------------------------- GroupWindow (CGroupWnd)

class GroupWindow : public StockWindow {
public:
    GroupWindow() : StockWindow("GroupWindow", true) {}

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& c = *host.conn;
        const auto& g = c.group();
        bool invited = !g.inviter.empty();
        // CGroupWnd::UpdateButtons
        if (id == "FollowButton" || id == "DeclineButton") {
            out.shown = invited;
            return;
        }
        if (id == "Hailstone_Inviter") { // Hailstone: who invited us (the modern pack's invite popup)
            out.text = game::display_name(g.inviter);
            out.shown = invited;
            return;
        }
        if (id == "DisbandButton") {
            out.shown = !invited;
            out.enabled = !invited && g.active;
            return;
        }
        if (id == "InviteButton") {
            const auto& w = c.world();
            const auto* t = find_spawn(w, w.target_id);
            bool can = (!g.active || (g.is_leader(self_name(c)) && g.count() < 6)) && t && !t->is_npc &&
                       !t->is_corpse && t->id != w.player_id;
            out.shown = !invited;
            out.enabled = !invited && can;
            return;
        }
        if (id == "LFGButton") {
            bool on = !g.active || g.is_leader(self_name(c));
            out.shown = on;
            out.enabled = on;
            return;
        }
        // CGroupWnd::UpdateDisplay for row n (1..5 = the n-th other member).
        static constexpr std::string_view kRows[] = {"HPLabel", "HPPercLabel", "PetGauge", "ManaGauge", "ManaLabel",
                                                     "ManaPercLabel", "STAGauge", "STALabel", "STAPercLabel",
                                                     "GW_AggroPctPlayer", "GroupRoleTank", "GroupRoleAssist",
                                                     "GroupRolePuller", "Gauge"};
        for (std::string_view prefix : kRows) {
            int n = index_after(id, prefix);
            if (n < 1 || n > 5) continue;
            const auto* m = other_member(c, n - 1);
            const game::Spawn* s = m ? spawn_named(c.world(), m->name) : nullptr;
            bool here = s != nullptr;
            if (prefix == "Gauge") {
                // HPTextColor: green for the leader, grey offline, gold for a mercenary whose owner is grouped.
                if (m && g.is_leader(m->name)) out.color = Rgb{0, 1, 0};
                else if (m && m->offline) out.color = argb_to_rgb(0xff808080);
                else if (m && m->merc && g.has_member(m->owner_name)) out.color = argb_to_rgb(0xffe1b511);
            } else if (prefix == "HPLabel" || prefix == "HPPercLabel" || prefix == "GW_AggroPctPlayer") {
                out.shown = here;
            } else if (prefix == "PetGauge") {
                bool pet = false;
                if (s && !s->is_corpse)
                    for (const auto& [pid, p] : c.world().spawns) pet = pet || (p.pet_owner_id == s->id && p.is_npc);
                out.shown = here && pet;
            } else if (prefix.starts_with("Mana")) {
                out.shown = here && (s->id == c.world().player_id ? c.is_spellcaster() : is_spellcaster(s->class_id));
            } else if (prefix.starts_with("STA")) {
                out.shown = here;
            } else { // role icons (CGroupWnd_UpdateRoleLabels)
                int role = prefix == "GroupRoleTank" ? 1 : prefix == "GroupRoleAssist" ? 2 : 3;
                bool on = m && (role == 1 ? m->main_tank : role == 2 ? m->main_assist : m->puller);
                out.shown = on;
                out.tooltip = on ? role_name(role) : "";
            }
            return;
        }
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn) return false;
        auto& c = *host.conn;
        // CGroupWnd::WndNotification
        // 0x67ae00: the member row a child belongs to (its gauges and labels end in the row number).
        auto row_of = [](std::string_view child) {
            for (std::string_view p : {"Gauge", "HPLabel", "HPPercLabel"})
                if (child.rfind(p, 0) == 0 && child.size() == p.size() + 1) return child.back() - '0';
            return -1;
        };
        if (button == "right") {
            if (!host.context_menus()) return true;
            int n = row_of(id);
            const auto* m = n >= 1 && n <= 5 ? other_member(c, n - 1) : nullptr;
            auto pos = role_menu_position(host);
            return m && open_member_roles(host, m->name, screen_, pos.x, pos.y);
        }
        if (button != "left") return false;
        if (id == "FollowButton") c.accept_group_invite();
        else if (id == "InviteButton") c.group_invite();
        else if (id == "DeclineButton" || id == "DisbandButton") c.group_disband();
        else if (id == "LFGButton") c.group_search_toggle_from_group_window(); // the group finder
        else if (int n = row_of(id); n >= 1 && n <= 5) {
            // A click on a member's row: LocalPlayer_TargetSelfOrPet(row) (0x58d230 with 0x67ae00's row),
            // as F2..F6 do; it also selects the row /disband reads (group_member_targeted()).
            c.target_group_member(n);
        } else return false;
        return true;
    }

};

// ---------------------------------------------------------------- BuffWindow / ShortDurationBuffWindow (CBuffWindow)

class BuffWindow : public StockWindow {
public:
    BuffWindow(const char* screen, int first, int count) : StockWindow(screen, true), first_(first), count_(count) {}

    void child(EqHost& host, std::string_view id, Child& out) override {
        int i = index_after(id, "Buff");
        if (!host.conn || i < 0) return;
        if (i >= count_) {
            out.shown = false;
            return;
        }
        buff_button(host, host.conn->spell_state().buffs[static_cast<std::size_t>(first_ + i)], out);
    }

    std::optional<BuffInfo> buff(EqHost& host, std::string_view id, unsigned overlays) override {
        int i = index_after(id, "Buff");
        if (!host.conn || i < 0 || i >= count_) return std::nullopt;
        const auto& c = *host.conn;
        const auto& buffs = c.spell_state().buffs;
        const auto& b = buffs[static_cast<std::size_t>(first_ + i)];
        auto info = buff_info(host, b);
        const auto* s = info ? c.spell_table().get(b.spell) : nullptr;
        if (!s) return info;
        // CBuffWindow::PostDraw @0x6426a0: the buff window (type 0) by CSpellDisplayManager +0x40..+0x42, the
        // song window (type 1, ShortDurationBuffWindow) by +0x43..+0x45. The counters and hits come from all of
        // the local player's effects (CharacterZoneClient::GetMaxEffects).
        using O = game::ClientOption;
        bool songs = first_ == game::kLongBuffSlots;
        std::span<const game::BuffSlot> effects(buffs.data(), buffs.size());
        if ((overlays & kBuffTimerOverlay) && c.client_option(songs ? O::ShowSongTimer : O::ShowBuffTimer))
            info->timer = game::buff_time_overlay(*s, info->ms_left);
        if ((overlays & kBuffCounterOverlay) && c.client_option(songs ? O::ShowSongCounters : O::ShowBuffCounters))
            info->counter = game::buff_counter_overlay(game::spell_counters(*s, effects));
        if ((overlays & kBuffLimitOverlay) && c.client_option(songs ? O::ShowSongLimitUse : O::ShowBuffLimitUse))
            info->limit = game::buff_limit_overlay(*s, effects);
        // CBuffWindow::OnProcessFrame: the fade under 19 s, except a bard's own short song (the profile's class).
        bool short_song = false;
        if (info->ms_left > 0 && info->ms_left / 1000 < 19 && host.character) {
            std::uint8_t cls = c.profile() ? static_cast<std::uint8_t>(c.profile()->class_) : 0;
            short_song = c.bard_cast_bard(*s, cls) && host.character->spell_duration(*s, b.level, true) < 4;
        }
        info->fade = game::buff_fade(info->ms_left, now_ms(), short_song);
        return info;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        // eq.WindowButton(window, child, 3) from sidl.lua (a right click): CBuffWindow::sub_006412d0 removes it.
        int i = index_after(id, "Buff");
        if (!host.conn || i < 0 || i >= count_ || button != "remove") return false;
        host.conn->remove_buff(first_ + i);
        return true;
    }

private:
    int first_, count_;
};

// ---------------------------------------------------------------- CastSpellWnd (CCastSpellWnd)

class CastSpellWindow;
const CastSpellWindow* g_cast_spell_window = nullptr; // the gems' looks for the spell gem hot buttons

class CastSpellWindow : public StockWindow {
public:
    CastSpellWindow() : StockWindow("CastSpellWnd", true) { g_cast_spell_window = this; }
    ~CastSpellWindow() override {
        if (g_cast_spell_window == this) g_cast_spell_window = nullptr;
    }

    // Dragging a memorized gem makes its hotbutton (type 7: the gem, whatever spell it holds then).
    std::optional<game::HotButton> pick_up(EqHost& host, std::string_view id, int) override {
        int g = index_after(id, "CSPW_Spell");
        if (!host.conn || g < 0 || g >= game::kSpellGems || g >= host.conn->usable_gems()) return std::nullopt;
        if (host.conn->spell_state().gems[static_cast<std::size_t>(g)].spell == game::kNoSpell) return std::nullopt;
        game::HotButton b;
        b.type = game::HotButtonType::SpellGem;
        b.slot = g;
        return b;
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        const auto& c = *host.conn;
        if (id == "CSPW_HorizontalOrientationBook" || id == "CSPW_HorizontalOrientationTemplate" ||
            id == "CSPW_VerticalOrientationTemplate") {
            out.shown = false; // the stock window is vertical; the templates only lay it out
            return;
        }
        int g = index_after(id, "CSPW_Spell");
        if (g < 0 || g >= game::kSpellGems) return;
        // RefreshSpellGemButtons: gems the character cannot use are disabled and hidden.
        bool usable = g < c.usable_gems();
        out.shown = usable;
        out.enabled = usable;
        const auto& gem = c.spell_state().gems[static_cast<std::size_t>(g)];
        const auto* s = gem.spell != game::kNoSpell ? c.spell_table().get(gem.spell) : nullptr;
        if (!s) {
            out.spellicon = none();
            out.tooltip = "";
            return;
        }
        out.spellicon = host.textures ? image(host.textures->gem_icon(s->spell_icon)) : none();
        out.tooltip = s->name; // UpdateSpellGemTooltips
        out.gem_tint = tint_of(*s, g);
        out.gem_overlay = overlay_[static_cast<std::size_t>(g)];
        out.gem_overlay_color = cool_down_color();
    }

    // A spell gem hot button (type 7) shows its gem as the bar does: the same stages and cool-down (its own
    // CSpellGemWnd is refreshed from the same timers in CHotButtonWnd_UpdateButtonStates and drawn by the same
    // CSpellGemWnd::Draw). Tint (background, [3] icon alpha) and the cool-down's fraction; none for an empty gem.
    std::optional<std::pair<std::array<float, 4>, std::optional<double>>> gem_look(EqHost& host, int g) const {
        if (!host.conn || g < 0 || g >= game::kSpellGems) return std::nullopt;
        const auto& gem = host.conn->spell_state().gems[static_cast<std::size_t>(g)];
        const auto* s = gem.spell != game::kNoSpell ? host.conn->spell_table().get(gem.spell) : nullptr;
        if (!s) return std::nullopt;
        return std::pair{tint_of(*s, g), overlay_[static_cast<std::size_t>(g)]};
    }
    std::uint32_t gem_cool_down_left(int g) const {
        if (g < 0 || g >= game::kSpellGems) return 0;
        return gems_[static_cast<std::size_t>(g)].cooldown_left(net::GameConnection::eq_time());
    }

    bool gem_click(EqHost& host, std::string_view id, bool right) override {
        int g = index_after(id, "CSPW_Spell");
        if (!host.conn || g < 0 || g >= game::kSpellGems) return false;
        auto& c = *host.conn;
        if (right) {
            // HandleSpellRightClick @0x648c10: a gem with a spell forgets it (Unmemorize); an empty one pops up the
            // spell select menu at the gem's right edge, half way down (GemMemming = the gem).
            if (c.spell_state().gems[static_cast<std::size_t>(g)].spell != game::kNoSpell) {
                c.forget_gem(g);
            } else if (auto rows = spell_select_rows(host, g); !rows.empty()) {
                const auto& r = host.click_rect;
                const ImVec2 mp = ImGui::GetMousePos();
                host.open_menu(r[2] >= 0 ? r[2] : mp.x, r[2] >= 0 ? (r[1] + r[3]) / 2 : mp.y, std::move(rows));
            }
        } else if (host.cursor_spell) {
            // HandleSpellLeftClick with a book spell on the cursor: StartSpellMemorization(slot, gem, 0), which
            // refuses a gem that already holds a spell (bReplace 0, @0x75bfea); Hailstone's replace_gem_spell
            // memorizes over it (EqHost::gem_takes_book_spell). The spell stays on the cursor until the memorize is
            // sent.
            if (g < c.usable_gems() && host.gem_takes_book_spell(g)) c.memorize(*host.cursor_spell, g);
        } else {
            c.clear_bard_twist(); // CCastSpellWnd::WndNotification: PlayerClient_ClearBardTwist first
            c.cast_gem(g);        // HandleSpellLeftClick
        }
        return true;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if ((id == "CSPW_SpellBookV" || id == "CSPW_SpellBookH") && button == "left") {
            if (host.toggle_spell_book) host.toggle_spell_book();
            return true;
        }
        return false;
    }

    // Once a frame: RefreshSpellGemButtons' stages, each gem's OnProcessFrame, and Draw's cool-down (whose
    // rectangle's fraction is computed as the gem draws).
    void frame(EqHost& host) override {
        if (!host.conn) return;
        const net::GameConnection& c = *host.conn;
        const game::SpellState& st = c.spell_state();
        const std::uint32_t now = net::GameConnection::eq_time();      // CDisplay +0x154
        const auto unix_now = static_cast<std::uint32_t>(std::time(nullptr)); // Util::FastTime
        bool casting = st.casting.casting();
        for (int g = 0; g < game::kSpellGems; ++g) {
            auto i = static_cast<std::size_t>(g);
            const game::SpellGem& gem = st.gems[i];
            const game::SpellData* sp = gem.spell != game::kNoSpell ? c.spell_table().get(gem.spell) : nullptr;
            const std::uint32_t eta = gem.ready;
            // PcZoneClient_IsLinkedSpellTimerReady on the gem's spell (a missing record: not ready).
            std::optional<std::int32_t> reuse;
            if (sp) reuse = sp->reuse_timer_index;
            auto linked_timer = [&st](std::int32_t k) { return st.linked_ready[static_cast<std::size_t>(k)]; };
            game::SpellGemReadiness r;
            r.casting = casting;
            r.has_spell = sp != nullptr;
            r.gem_timer_done = !(now < eta);
            r.global_done = !(now < st.recovery_ready);
            r.linked_ready = game::mechanics::gems::is_linked_spell_timer_ready(reuse, unix_now, linked_timer);
            game::SpellGemDisplay& d = gems_[i];
            d.refresh(r.ready());
            d.process_frame();
            if (!casting && sp) {
                std::uint32_t gem_left = now < eta ? eta - now : 0;
                // The linked timer's time left is whole seconds in the original ((end - FastTime()) * 1000).
                std::uint32_t linked_left = 0;
                if (sp->reuse_timer_index >= 1 && sp->reuse_timer_index <= 19) {
                    std::uint32_t end = st.linked_ready[static_cast<std::size_t>(sp->reuse_timer_index)];
                    if (unix_now < end) linked_left = (end - unix_now) * 1000;
                }
                // The focus reuse modifier (CharacterZoneClient::GetFocusReuseMod) only shortens the overlay's
                // total; a server extension may drop it.
                const std::int32_t focus =
                    host.character && !c.server_ext().reuse_focus_dropped(c, *sp) ? host.character->focus_reuse_mod(*sp) : 0;
                d.update_cooldown(now, gem_left, linked_left, sp->recast_time - static_cast<std::uint32_t>(focus));
            }
            overlay_[i] = d.overlay_fraction(now);
        }
    }

private:
    // SetSpellGemTint (0x4ad890: by target type) and the tint stage (game/spell_gem.h).
    std::array<float, 4> tint_of(const game::SpellData& s, int g) const {
        game::SpellGemTints tints = game::spell_gem_tints(game::spell_target_color(s.target_type, s.ae_duration));
        const game::SpellGemDisplay& d = gems_[static_cast<std::size_t>(g)];
        Rgb rgb = argb_to_rgb(tints.colors[static_cast<std::size_t>(d.tint_index)]);
        float alpha = d.tint_index == 0 ? 1.0f : static_cast<float>(tints.alphas[static_cast<std::size_t>(d.tint_index)]) / 255.0f;
        return {rgb[0], rgb[1], rgb[2], alpha};
    }

    std::array<game::SpellGemDisplay, game::kSpellGems> gems_{};
    std::array<std::optional<double>, game::kSpellGems> overlay_{};
};

// ---------------------------------------------------------------- CastingWindow (CCastingWnd)

class CastingWindow : public StockWindow {
public:
    CastingWindow() : StockWindow("CastingWindow", false) {}
    // CCastingWnd::OnProcessFrame shows the window while the casting gauge has a value and hides it after.
    bool shown(EqHost& host) override { return host.in_game && eq_gauge(host, 7).value >= 0; }
    void set_shown(EqHost&, bool) override {}
};

// ---------------------------------------------------------------- HotButtonWnd..HotButtonWnd10 (CHotButtonWnd)

// The gem's memorized spell (the profile's +0x3060 + 4 * gem), kNoSpell for none.
std::uint32_t gem_spell(const EqHost& host, std::int32_t gem) {
    if (!host.conn || gem < 0 || gem >= static_cast<int>(host.conn->spell_state().gems.size())) return game::kNoSpell;
    return host.conn->spell_state().gems[static_cast<std::size_t>(gem)].spell;
}

// A spell select menu item (10000 + spell, IsValidSpellID): CCastSpellWnd_TryMemorizeFromCursor(spell, gem), the
// spell book shown and the memorize 500 ms later (GameConnection::memorize_from_cursor).
void memorize_from_menu(EqHost& host, std::uint32_t spell, std::int32_t gem) {
    if (!host.conn || !host.conn->spell_table().get(spell)) return;
    host.conn->memorize_from_cursor(spell, gem);
}

// CCastSpellWnd::RecacheCategorizedSpells @0x648200 and MakeSpellSelectMenu @0x646f90: the book's spells (slots
// 0..719) filed as CategorizeSpell files them (game/combat_ability_menu.h's order: categories and subcategories by
// name, spells by level, highest first), each "%02d - %s" with id 10000 + the spell in the spell's target-type
// colour (0x4ad890, the gem tint's); choosing one memorizes it into `gem`. Empty: the book has no categorized
// spell (no menu).
std::vector<EqHost::MenuRow> spell_select_rows(EqHost& host, std::int32_t gem) {
    std::vector<EqHost::MenuRow> rows;
    if (!host.conn) return rows;
    const auto& c = *host.conn;
    const std::uint32_t cls = c.visible_class();
    std::vector<game::CombatAbilityEntry> list;
    for (std::uint32_t id : c.spell_state().book) {
        const game::SpellData* s = id == game::kNoSpell ? nullptr : c.spell_table().get(id);
        if (!s) continue;
        list.push_back({static_cast<std::int32_t>(id), s->category, s->subcategory, s->subcategory2,
                        c.spell_level_needed(*s, cls), s->name});
    }
    const auto menu = game::build_combat_ability_menu(
        list, [&c](std::int32_t id) { return c.strings().db_string(static_cast<std::uint32_t>(id), 5); });
    for (const auto& cat : menu) {
        EqHost::MenuRow row;
        row.text = cat.name;
        row.submenu = [&host, cat, gem] {
            std::vector<EqHost::MenuRow> subs;
            for (const auto& sub : cat.children) {
                EqHost::MenuRow sr;
                sr.text = sub.name;
                sr.submenu = [&host, sub, gem] {
                    std::vector<EqHost::MenuRow> items;
                    for (const auto& it : sub.items) {
                        EqHost::MenuRow ir;
                        ir.text = it.text;
                        const auto spell = static_cast<std::uint32_t>(it.index);
                        // AddMenuItem's colour; the list's SetColors(-1, -1, 0xffffff00) is only the selected row's
                        if (const auto* sp = host.conn ? host.conn->spell_table().get(spell) : nullptr)
                            ir.rgb = game::spell_target_color(sp->target_type, sp->ae_duration) & 0xffffffu;
                        ir.act = [&host, spell, gem] { memorize_from_menu(host, spell, gem); };
                        items.push_back(std::move(ir));
                    }
                    return items;
                };
                subs.push_back(std::move(sr));
            }
            return subs;
        };
        rows.push_back(std::move(row));
    }
    return rows;
}

// CHotButton_ShowContextMenu for bar 0..9, button 0..11 of the bar's current page at x, y (game/hot_button_menu.h):
// the rows CHotButton_UpdateContextMenu leaves shown, in CHotButtonWnd_BuildContextMenus' order.
void open_hot_button_menu(EqHost& host, int bar, int button, float x, float y) {
    if (!host.hot_buttons || !host.conn) return;
    auto text = [&host](std::uint32_t id) { return host.conn->strings().format(id, {}); };
    const game::HotButton b = host.hot_buttons->current(bar, button);
    const bool gem = b.type == game::HotButtonType::SpellGem;
    const std::uint32_t spell = gem ? gem_spell(host, b.slot) : game::kNoSpell;
    const bool has_spell = spell != game::kNoSpell && host.conn->spell_table().get(spell);
    const game::HotButtonMenuRows shown = game::hot_button_menu_rows(b, has_spell);
    // The button again when a row is chosen (the menu stays open over frames; the page may have turned).
    auto record = [&host, bar, button]() -> game::HotButton& { return host.hot_buttons->current(bar, button); };
    auto save = [&host, bar, button](const game::HotButton& nb) {
        if (host.set_hot_button) host.set_hot_button(bar, button, nb); // the changed flag and SaveHotButtonsToIni
    };
    std::vector<EqHost::MenuRow> rows;
    auto add = [&rows](std::string t, std::function<void()> act) {
        EqHost::MenuRow r;
        r.text = std::move(t);
        r.act = std::move(act);
        rows.push_back(std::move(r));
    };
    // 0x1b58 / 0x1b61: DoHotButton(n, 0).
    auto activate = [&host, bar, button] {
        if (host.press_hot_button) host.press_hot_button(bar, button, false);
    };
    if (shown.activate) add(text(game::kStrHbActivate), activate);
    if (shown.consume) add(text(game::kStrHbConsume), activate);
    if (shown.inspect)
        add(text(game::kStrHbInspect), [&host, record] {
            const game::HotButton& r = record();
            if (r.type == game::HotButtonType::SpellGem) {
                // CSpellDisplayManager::ShowSpell(the gem's spell, !Shift, 1, 6).
                const std::uint32_t s = gem_spell(host, r.slot);
                if (s != game::kNoSpell) host.show_spell(static_cast<std::int32_t>(s), false);
            } else if (host.conn) {
                // 0x6949d0: the slot's item in an item display; a krono slot (its CInvSlotWnd's location
                // eItemContainerKrono) without an item asks the server for the Krono's: Send_OP_ItemLinkResponse(88888,
                // "Krono", 0), flag 2 with Shift.
                const auto at = game::hot_button_item_slot(host.conn->inventory(), r);
                const net::Item* it = at ? host.conn->inventory().at(*at) : nullptr;
                if (it && host.inspect_item) host.inspect_item(*it);
                else if (!it && r.type == game::HotButtonType::KronoSlot)
                    host.conn->request_item_link(net::kKronoItemId, "Krono", 0, ImGui::GetIO().KeyShift);
            }
        });
    if (shown.clear) add(text(game::kStrHbClear), [save] { save(game::HotButton{}); }); // 0x550c50
    // 0x1b60 Change Size: the bar's CQuantityWnd (0x68a170) at the button's centre (here where the menu opened)
    if (shown.change_size) add(text(game::kStrHbChangeSize), [&host, bar, x, y] { open_hot_bar_size(host, bar, x, y); });
    // 0x1b5a Assign Icon: CIconSelectionWnd on the button's icon (0x68f330), its Accept (0x28) sets it
    if (shown.assign_icon)
        add(text(game::kStrHbAssignIcon), [&host, bar, button, x, y] { open_icon_selection(host, bar, button, x, y); });
    if (shown.clear_icon)
        add(text(game::kStrHbClearIcon), [record, save] {
            game::HotButton r = record();
            r.icon_type = r.icon_slot = -1; // 0x550040({-1, -1})
            save(r);
        });
    if (shown.assign_label)
        add(text(game::kStrHbAssignLabel), [&host, bar, button, record, save] {
            EqHost::TextEntry e;
            e.prompt = host.conn ? host.conn->strings().format(game::kStrHbEnterLabel, {}) : std::string();
            e.max_chars = game::kHotButtonLabelEntryMax;
            e.text = host.hot_button_label ? host.hot_button_label(bar, button) : record().label; // 0x54faf0
            e.done = [&host, record, save](const std::string& t) {
                game::HotButton r = record();
                if (game::assign_label(*host.hot_buttons, r, t) && host.actions)
                    host.actions->social_changed(r.slot); // SaveSocialsToIni(0)
                save(r);
            };
            host.text_entry = std::move(e);
        });
    {
        // Assign Social: "Page %1" 1..10, each page's 12 socials by name, "<New Social>" for an unnamed one.
        EqHost::MenuRow social;
        social.text = text(game::kStrHbAssignSocial);
        social.submenu = [&host, record, save] {
            std::vector<EqHost::MenuRow> pages;
            for (int p = 0; p < game::kSocialPages; ++p) {
                EqHost::MenuRow pr;
                const std::vector<std::string> args{std::to_string(p + 1)};
                pr.text = host.conn ? host.conn->strings().format(game::kStrHbSocialPage, args) : "";
                pr.submenu = [&host, p, record, save] {
                    std::vector<EqHost::MenuRow> items;
                    for (int i = 0; i < game::kSocialButtons; ++i) {
                        const int index = p * game::kSocialButtons + i;
                        EqHost::MenuRow ir;
                        const std::string& name = host.hot_buttons->social(index).name;
                        ir.text = !name.empty() || !host.conn ? name
                                                              : host.conn->strings().format(game::kStrHbNewSocial, {});
                        ir.act = [&host, index, record, save] {
                            game::HotButton r = record();
                            const bool unnamed = game::assign_social(*host.hot_buttons, r, index);
                            save(r);
                            if (unnamed && host.open_social_editor) host.open_social_editor(index); // 0x758bb0
                        };
                        items.push_back(std::move(ir));
                    }
                    return items;
                };
                pages.push_back(std::move(pr));
            }
            return pages;
        };
        rows.push_back(std::move(social));
    }
    if (shown.memorize) {
        EqHost::MenuRow mem;
        mem.text = text(game::kStrHbMemorize);
        const std::int32_t g = b.slot;
        mem.submenu = [&host, g] { return spell_select_rows(host, g); };
        rows.push_back(std::move(mem));
    }
    if (shown.unmemorize)
        add(text(game::kStrHbUnmemorize), [&host, record] {
            const game::HotButton& r = record();
            if (host.conn && r.type == game::HotButtonType::SpellGem && gem_spell(host, r.slot) != game::kNoSpell)
                host.conn->forget_gem(r.slot); // CCastSpellWnd::Unmemorize
        });
    host.open_menu(x, y, std::move(rows));
}


class HotButtonWindow : public StockWindow {
public:
    HotButtonWindow(int bar, const std::string& screen) : StockWindow(screen, bar == 0), bar_(bar) {
        register_hot_bar_window(bar, this);
    }
    ~HotButtonWindow() override { unregister_hot_bar_window(bar_, this); }

    // CHotButtonWnd::LoadIniInfo: PercentSize, ShowKeyMap, ShowSpinner (app/lua_eq_hotbar.h).
    void ini_loaded(EqHost&, ui::UiIni& ini, const std::filesystem::path&) override {
        load_hot_bar_settings(bar_, ini, ui::window_ini_info(screen_).section);
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (id == "HB_VerticalBarPageButtons" || id == "HB_VerticalBarTemplate" || id == "HB_HorizontalBarTemplate" ||
            id == "HB_NoSpinnerBarTemplate") {
            out.shown = false; // horizontal bar: the vertical page buttons are hidden, the templates only lay it out
        } else if (id == "HB_HorizontalBarPageButtons") {
            out.shown = hot_bar_settings(bar_).spinner; // sub_0068a850: with ShowSpinner (a horizontal bar)
        } else if (id == "HB_HorizontalCurrentPageLabel" || id == "HB_VerticalCurrentPageLabel") {
            out.text = std::to_string(host.hot_buttons ? host.hot_buttons->page(bar_) + 1 : 1);
        } else if (int n = index_after(id, "HB_Button"); n >= 1 && n <= game::kHotButtons) {
            // the key map text 0x5506b0 draws on the button while ShowKeyMap is on
            out.text = hot_bar_settings(bar_).key_map ? hot_button_key_text(host, bar_, n - 1) : std::string();
        }
    }

    // The window's ContextMenuID: the bar's own menu (game/hot_button_sets.h).
    bool context_menu(EqHost& host, float x, float y) override {
        open_hot_bar_menu(host, bar_, x, y, true);
        return true;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.hot_buttons || button != "left") return false;
        int page = host.hot_buttons->page(bar_);
        if (id == "HB_PageUpButton" || id == "HB_PageLeftButton") page = (page + game::kHotPages - 1) % game::kHotPages;
        else if (id == "HB_PageDownButton" || id == "HB_PageRightButton") page = (page + 1) % game::kHotPages;
        else return false;
        host.hot_buttons->set_page(bar_, page);
        return true;
    }

    std::optional<HotButtonInfo> hot_button(EqHost& host, std::string_view id) override {
        int n = index_after(id, "HB_Button");
        if (!host.hot_buttons || n < 1 || n > game::kHotButtons) return std::nullopt;
        const game::HotButton& b = host.hot_buttons->current(bar_, n - 1);
        if (b.empty()) return HotButtonInfo{};
        HotButtonInfo info = hot_button_info(host, b, host.hot_button_label ? host.hot_button_label(bar_, n - 1) : b.label);
        // 0x54ff40: an icon of the button's own (Assign Icon) is drawn instead of its decal, item or spell icon
        if (b.icon_type != -1)
            if (auto c = hot_button_custom_icon(host, b.icon_type, b.icon_slot)) info.icon = *c;
        const auto now = net::GameConnection::eq_time();
        if (b.type == game::HotButtonType::SpellGem) {
            if (auto look = g_cast_spell_window ? g_cast_spell_window->gem_look(host, b.slot) : std::nullopt) {
                info.gem_tint = look->first;
                info.cool_down = look->second;
                if (info.cool_down) info.cool_down_left = g_cast_spell_window->gem_cool_down_left(b.slot);
            }
            return info;
        }
        game::HotButtonDisplay& d = displays_[static_cast<std::size_t>(n - 1)];
        info.checked = info.checked || d.checked; // the pet window's check (hot_button_info) or the button's own
        info.cool_down = d.cool_down.overlay_fraction(now);
        if (info.cool_down) info.cool_down_left = d.cool_down.left(now);
        return info;
    }

    // CHotButtonWnd_UpdateButtonStates, each button of the page shown (game/hot_button_state.h).
    void frame(EqHost& host) override {
        if (!host.hot_buttons || !host.in_game) return;
        HostButtonQueries q(host);
        const bool extras = hotbutton_extras(host);
        for (int i = 0; i < game::kHotButtons; ++i)
            game::update_hot_button(displays_[static_cast<std::size_t>(i)], host.hot_buttons->current(bar_, i), q, extras);
    }

    // 0x5536b0 (game/hot_button_menu.h): an empty button's click and a held right button open the button's menu;
    // a left click puts the cursor's hotbutton here or runs the button, a quick right click 0x54fc10.
    bool click_hot_button(EqHost& host, std::string_view id, std::string_view button) override {
        int n = index_after(id, "HB_Button");
        if (n < 1 || n > game::kHotButtons) return false;
        const game::HotButtonMouse m = button == "rightheld" ? game::HotButtonMouse::RightHeld
                                       : button == "right"   ? game::HotButtonMouse::RightClick
                                                             : game::HotButtonMouse::LeftClick;
        const game::HotButton b = host.hot_buttons ? host.hot_buttons->current(bar_, n - 1) : game::HotButton{};
        using A = game::HotButtonMouseAction;
        switch (game::hot_button_mouse(b, m, host.cursor_button.has_value())) {
        case A::Menu: {
            // CHotButton_ShowContextMenu: at the mouse (a pad's slot: where eq.ClickHotButton put it; no mouse: the
            // screen's centre)
            ImVec2 mp = ImGui::GetMousePos();
            if (!ImGui::IsMousePosValid(&mp)) mp = ImVec2(ImGui::GetIO().DisplaySize.x / 2, ImGui::GetIO().DisplaySize.y / 2);
            const auto& r = host.click_rect;
            open_hot_button_menu(host, bar_, n - 1, r[0] >= 0 ? r[0] : mp.x, r[0] >= 0 ? r[1] : mp.y);
            break;
        }
        case A::Drop: {
            // 0x552e00: the cursor's hotbutton goes here; what was here goes on the cursor.
            if (!host.hot_buttons || !host.set_hot_button) return true;
            host.set_hot_button(bar_, n - 1, *host.cursor_button);
            host.cursor_button.reset();
            if (!b.empty()) {
                host.cursor_button = b;
                ++host.cursor_serial;
            }
            break;
        }
        case A::Press:
            if (host.press_hot_button) host.press_hot_button(bar_, n - 1, true);
            break;
        case A::RightClick:
        case A::None: // an item's own right click (its CInvSlot's HandleRButtonUp: the item's use), as the actions take it
            if (host.hot_button_menu) host.hot_button_menu(bar_, n - 1);
            break;
        }
        return true;
    }

    // Dragging a button off the bar takes it onto the cursor.
    std::optional<game::HotButton> pick_up(EqHost& host, std::string_view id, int) override {
        int n = index_after(id, "HB_Button");
        if (!host.hot_buttons || !host.set_hot_button || n < 1 || n > game::kHotButtons) return std::nullopt;
        game::HotButton b = host.hot_buttons->current(bar_, n - 1);
        if (b.empty()) return std::nullopt;
        host.set_hot_button(bar_, n - 1, game::HotButton{});
        return b;
    }

private:
    int bar_;
    std::array<game::HotButtonDisplay, game::kHotButtons> displays_{};
};

} // namespace

HotButtonInfo hot_button_info(EqHost& host, const game::HotButton& b, std::string label) {
    HotButtonInfo info;
    if (b.empty()) return info;
    info.label = std::move(label);
    info.kind = "button";
    info.checked = pet_hot_button_checked(host, info.label).value_or(false);
    if (b.type == game::HotButtonType::SpellGem && host.conn) {
        info.kind = "spell";
        const auto& gems = host.conn->spell_state().gems;
        if (b.slot >= 0 && b.slot < static_cast<int>(gems.size()))
            if (const auto* s = host.conn->spell_table().get(gems[static_cast<std::size_t>(b.slot)].spell))
                if (host.textures) info.icon = host.textures->gem_icon(s->spell_icon);
    } else if (b.type == game::HotButtonType::Social && host.hot_buttons && b.slot >= 0 &&
               b.slot < game::kSocialPages * game::kSocialButtons) {
        const game::Social& s = host.hot_buttons->social(b.slot);
        info.color = social_text_color(s.color);
        if (s.is_lua() && host.scripts) {
            info.lua = true;
            info.running = host.scripts->button_running(b.slot);
            const mq::ButtonFace& f = host.scripts->button_face(b.slot, s.name, s.lua);
            if (f.has) {
                if (!f.label.empty()) info.label = f.label;
                if (f.color)
                    info.color = Rgb{static_cast<float>((*f.color >> 16) & 0xff) / 255.0f,
                                     static_cast<float>((*f.color >> 8) & 0xff) / 255.0f,
                                     static_cast<float>(*f.color & 0xff) / 255.0f};
                info.cooldown = f.cooldown;
                info.overlay = f.overlay;
                info.enabled = f.enabled;
                info.tooltip = f.tooltip;
            }
        }
    } else if ((b.type == game::HotButtonType::InventorySlot || b.type == game::HotButtonType::ItemLink) && host.conn) {
        info.kind = "item";
        if (const auto at = game::hot_button_item_slot(host.conn->inventory(), b))
            if (const auto* it = host.conn->inventory().at(*at); it && host.textures) info.icon = host.textures->item_icon(it->icon);
    }
    return info;
}

void add_hud_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<PlayerWindow>());
    out.push_back(std::make_unique<TargetWindow>());
    out.push_back(std::make_unique<GroupWindow>());
    out.push_back(std::make_unique<BuffWindow>("BuffWindow", 0, game::kLongBuffSlots));
    out.push_back(std::make_unique<BuffWindow>("ShortDurationBuffWindow", game::kLongBuffSlots, game::kShortBuffSlots));
    out.push_back(std::make_unique<CastSpellWindow>());
    out.push_back(std::make_unique<CastingWindow>());
    for (int bar = 0; bar < game::kHotBars; ++bar)
        out.push_back(std::make_unique<HotButtonWindow>(bar, bar == 0 ? "HotButtonWnd" : "HotButtonWnd" + std::to_string(bar + 1)));
    add_hotbar_windows(out);
}

} // namespace hailstone::app
