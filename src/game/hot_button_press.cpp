#include "game/hot_button_press.h"

#include <cstdlib>
#include <cstring>

#include "core/msvc_atoi.h"
#include "core/wrap_math.h"

namespace hailstone::game {

namespace {

// Types 5, 12 and 13: a right click at the slot window's top left.
void right_click_slot(const std::optional<HotButtonSlot>& slot, HotButtonQueries& q) {
    if (!slot || !slot->window) return;
    q.inv_slot_right_click(slot->left, slot->top);
}

// Actions 7 and 9: activated unless already active.
void open_window(HotButtonWindow w, HotButtonQueries& q) {
    if (q.window_active(w)) return;
    q.window_activate(w);
}

void do_action(HotButtonGlobals& g, std::int32_t action, HotButtonQueries& q) {
    switch (action) {
    case 0:
        if (!g.no_who) q.who();
        return;
    case 1:
        if (g.group_invited) q.group_follow();
        else q.group_invite();
        return;
    case 2: q.disband(); return;
    case 3: q.camp(); return;
    case 4: q.sit(); return;
    case 5: g.run_walk ^= 1; return;
    case 7: open_window(HotButtonWindow::Options, q); return;
    case 8: q.toggle_view(); return;
    case 9: open_window(HotButtonWindow::Inventory, q); return;
    case 10:
        if (q.window_active(HotButtonWindow::SpellBook)) {
            q.window_deactivate(HotButtonWindow::SpellBook);
            return;
        }
        if (g.controlled_speed && *g.controlled_speed > 0.01f) {
            q.must_be_stopped();
            return;
        }
        if (g.stand_state && *g.stand_state == kStandBindWound) return;
        q.window_activate(HotButtonWindow::SpellBook);
        return;
    case 11: g.toggle_89c ^= 1; return;
    case 12: q.execute_cmd(kCmdToggleFindWindow); return;
    default: return; // 6 and anything above 12
    }
}

} // namespace

void do_hot_button(HotButtonGlobals& g, const HotButtonRecord& r, std::int32_t mouse, HotButtonQueries& q) {
    if (g.stand_state && *g.stand_state == kStandStunned) {
        q.cant_use_command();
        return;
    }
    if (mouse != 0 && (r.type == 5 || r.type == 13)) return;
    if (g.item_pending > 0) {
        q.cant_use_command();
        return;
    }
    const std::int32_t value = r.value;
    switch (r.type) {
    case 1:
        if (value == 0) {
            q.execute_cmd(kCmdAutoPrimary);
        } else if (value == 1 && g.range_attack_ready) {
            g.range_attack_ready = 0;
            q.ranged_attack();
        }
        return;
    case 2:
    case 3:
        if (value != -1) q.use_skill(static_cast<std::uint8_t>(q.list_entry(r.type, value)));
        return;
    case 9:
        if (value != -1) q.use_skill(static_cast<std::uint8_t>(value));
        return;
    case 10: {
        const auto ability = static_cast<std::int32_t>(static_cast<std::uint32_t>(value) - 0x84u);
        if (value == -1 || ability < 0) return;
        const std::int32_t spell = q.combat_ability(ability);
        if (!q.spell_exists(spell)) return;
        q.do_combat_ability(spell);
        return;
    }
    case 4: q.set_current_social(value); return;
    case 11: q.leadership_ability(value / 8, value % 8); return;
    case 5: right_click_slot(q.find_inv_slot(value, -1), q); return;
    case 13: right_click_slot(q.find_krono_slot(), q); return;
    case 12:
        if (!r.item) return;
        right_click_slot(q.find_inv_slot(r.item->slot, r.item->bag_slot), q);
        return;
    case 6: do_action(g, value, q); return;
    case 7:
        q.clear_bard_twist();
        q.spell_gem_left_click(value);
        return;
    case 8: {
        auto pet = q.pet_button(value);
        if (!pet) return;
        if (pet->enabled) q.pet_button_set_check(pet->checked == 0);
        q.pet_window_click(value);
        return;
    }
    default: return;
    }
}

bool hot_button_right_click(const HotButtonRightClick& c, HotButtonRightClickQueries& q) {
    if (c.type == 4) {
        const std::int32_t social = c.value;
        if (social == c.current_social) q.social_stop(-1, false);
        else if (social < kSocialCount) {
            if (c.social_cooldown_length != 0) q.social_stop(social, true);
            else q.open_social_editor(social);
        } else {
            q.aa_window_show(social - kSocialCount);
        }
        return true;
    }
    if (c.type == 5 || c.type == 12 || c.type == 13) {
        if (c.slot && c.slot->window) q.inv_slot_right_click(c.slot->left, c.slot->top);
        return true;
    }
    if (c.type == 7 && c.no_spell_info == 0 && c.gem_window) q.spell_gem_right_click(c.value);
    return true;
}

std::optional<std::int32_t> social_stop(SocialQueue& s, std::int32_t social, bool keep_queued) {
    std::optional<std::int32_t> cleared;
    bool skip = false;
    if (social == -1) social = s.current;
    else if (social >= kSocialCount) skip = true;
    if (!skip) {
        const std::int32_t page = social / 12;
        const std::int32_t button = social - page * 12;
        if (page < 10 && button < 12) {
            // The record's offset wraps as the original's 32-bit multiplication by 0x51c.
            const auto index = static_cast<std::uint32_t>(page * 12 + button);
            cleared = core::wrapped_div(index * 0x51cu, 0x51c);
            s.pause = 0;
            s.line = 0;
        }
    }
    if (!keep_queued) s.current = -1;
    return cleared;
}

namespace {

bool starts_with_nocase(const std::string& s, const char* prefix) {
    std::size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// strstr(text, what) as a CXStr: the rest from the match, or empty.
std::string from(const std::string& text, char what) {
    std::size_t at = text.find(what);
    return at == std::string::npos ? std::string() : text.substr(at);
}

} // namespace

SocialLineParts split_social_line(const std::string& text) {
    SocialLineParts out;
    std::string cmd = text;
    for (;;) {
        if (cmd.empty()) break;
        if (starts_with_nocase(cmd, "/pause")) {
            cmd = from(cmd, ' ');
            std::int32_t n = core::msvc_atoi(cmd);
            out.pause = n > 600 ? 600 : (n < 1 ? 1 : n);
        } else if (starts_with_nocase(cmd, "/timer")) {
            cmd = from(cmd, ' ');
            std::int32_t n = core::msvc_atoi(cmd);
            out.timers.push_back(n > 360000 ? 360000 : (n < 0 ? 0 : n));
        } else {
            break;
        }
        cmd = from(cmd, ',');
        if (cmd.empty()) break;
        if (cmd.size() > 1) cmd = cmd.substr(1);
    }
    out.command = std::move(cmd);
    return out;
}

void social_passage_of_time(SocialQueue& s, std::uint32_t now_ms, SocialRunQueries& q) {
    if (now_ms - s.last_tick <= 100) return;
    s.last_tick = now_ms;
    const std::int32_t social = s.current;
    std::int32_t line = s.line;
    std::int32_t pause;
    if (s.pause < 1) {
        if (social >= kSocialCount) {
            q.alternate_ability(social - kSocialCount);
            s = {-1, 0, 0, s.last_tick};
            return;
        }
        if (social < 0) {
            // The original reads the bytes before the table (record -1), then drops the queue.
            s = {-1, 0, 0, s.last_tick};
            return;
        }
        auto text = [&](std::int32_t n) -> std::string {
            const std::string* t = q.line(social, n);
            return t ? *t : std::string();
        };
        while (line < 5 && text(line).empty()) ++line;
        pause = s.pause;
        if (line < 5) {
            SocialLineParts parts = split_social_line(text(line));
            pause = parts.pause && q.allow_directive("/pause") ? *parts.pause : 1;
            if (!parts.timers.empty() && q.allow_directive("/timer"))
                for (std::int32_t n : parts.timers)
                    if (n * 100 > 0 && !q.cooldown_running(social)) q.start_cooldown(social, n * 100);
            if (!parts.command.empty()) q.interpret(parts.command);
            ++line;
        }
    } else {
        pause = s.pause - 1;
    }
    if (line < 5) {
        if (pause < 0) pause = 0;
        if (social >= 0 && line >= 0) {
            s.pause = pause;
            s.line = line;
            return;
        }
    }
    s = {-1, 0, 0, s.last_tick};
}

} // namespace hailstone::game
