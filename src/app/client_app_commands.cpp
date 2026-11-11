// ClientApp: the original's slash commands that act on the client rather than the connection (the rules
// are game/option_commands.h): the windows (/filter, /note, /map, /loadskin), the
// chat windows (/chatfontsize, /clearchat, /clearallchat, /usercolor), hot buttons (/hotbutton), and the display,
// sound and mouse settings, which go to hailstone.ini as the Options window's do (the original's eqclient.ini
// [Defaults] / [Options]). GameConnection::run_slash_command hands them here (set_client_commands).
//

#include <format>
#include <sstream>

#include <imgui.h>

#include "app/client_app_internal.h"
#include "app/lua_eq.h"
#include "core/distribution.h"
#include "app/lua_eq_windows.h"
#include "core/msvc_atoi.h"
#include "game/hotbuttons.h"
#include "game/option_commands.h"
#include "game/slash_commands.h"
#include "game/xtarget.h"

namespace hailstone::app {

namespace client_internal {

// Cmd_xtarget @0x4e4780 (the usage: Cmd_xtarget_PrintUsage @0x4e3530). Up to three
// words (GetNextCmdArg). A first word starting with a digit targets that slot (1-based); "show" (any prefix of it)
// shows the window with "on", hides it with "off", else toggles it; "target <n>" targets slot n; "add" puts the
// target (a player 2, an NPC 3, by name) in the first auto hater slot; "remove" turns the last specific slot back
// into an auto hater, "remove target" every specific slot holding the target; "set <n> <type or name>" sets slot n
// to a type (XTarget_ParseTargetType; CurrentTarget takes the target) or to an existing player or NPC by that
// name; "auto" (any prefix) sets the auto add flag ("on", "off", else toggled), which the window stores.
void ClientApp::xtarget_command(const std::string& args) {
    net::GameConnection& c = *conn_;
    const std::uint32_t expansions = c.profile() ? c.profile()->expansions : 0;
    if (!(expansions & 0x8000)) {
        c.show_chat("In order to gain access to this feature, you must purchase the EverQuest expansion: Underfoot.");
        return;
    }
    auto usage = [&] {
        static constexpr std::pair<const char*, int> kUsage[] = {
            {"/xtarget USAGE:", 15},
            {"/xtarget show <on/off>", 15},
            {" /xtarget show is used to toggle the visiblity of the window", 15},
            {"\t---example: /xtarget show on", 15},
            {"", -1},
            {"/xtarget set <slot> <role>", 15},
            {"OR: /xtarget set <slot> <player_name>", 15},
            {" /xtarget set is used to assign a role, player, or NPC to a specific slot", 15},
            {"\t---example: /xtarget set 3 grouptank ", 15},
            {"\t---example: /xtarget set 3 Lorudan ", 15},
            {"", -1},
            {"/xtarget target <slot>", 15},
            {" /xtarget target is used to select a target at a specific slot", 15},
            {"\t---example: /xtarget target 7", 15},
            {"\t---shortcut example: /xtarget 7", 15},
            {"", -1},
            {"/xtarget auto <on/off>", 15},
            {" /xtarget auto is used to toggle auto-hater-targeting", 15},
            {"\t---example: /xtarget auto on", 15},
            {"", -1},
            {"/xtarget add", 15},
            {" /xtarget add will add current target to your next available slot", 15},
            {"", -1},
            {"/xtarget remove", 15},
            {" /xtarget remove will remove the last added target", 15},
            {"\t---example: /xtarget remove", 15},
            {" /xtarget remove target will remove targeted player from slots not using rolls (specific PC/NPC slots)", 15},
            {"\t---example: /xtarget remove target", 15},
        };
        for (auto [text, color] : kUsage) c.show_chat(text, color < 0 ? 0u : static_cast<std::uint32_t>(color));
    };
    if (args.empty()) {
        c.show_chat("You must enter a valid Extended Target command.", 0xd);
        usage();
        return;
    }
    std::string word[3];
    std::string_view rest = args;
    for (auto& w : word) {
        w = game::next_cmd_arg(rest).substr(0, 0x7f);
        if (w.empty()) break;
    }
    auto prefix = [](const std::string& w, std::string_view of, std::size_t n) { // strnicmp(w, of, n) == 0
        for (std::size_t i = 0; i < n; ++i) {
            char a = i < w.size() ? w[i] : 0, b = i < of.size() ? of[i] : 0;
            if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b))) return false;
            if (!a) break;
        }
        return true;
    };
    auto digit = [](const std::string& w) { return !w.empty() && w[0] >= '0' && w[0] <= '9'; }; // Utf8IsDigit
    const game::XTargetState& list = c.xtargets();
    const int count = static_cast<int>(list.slots.size());
    auto range = [&] {
        c.show_chat(std::format("Slot_number must be between 1 and {} (your current maximum number of slots).", count));
    };
    auto target_slot = [&](const std::string& w) {
        // In 64 bits: "2147483648" is INT_MIN, and INT_MIN - 1 overflowed.
        const std::int64_t n = std::int64_t{core::msvc_atoi(w)} - 1;
        if (n < 0 || n >= count) return range();
        c.target_xtarget(static_cast<int>(n));
    };
    auto incorrect = [&] { c.show_chat("Incorrect Usage. Type /xtarget for correct usage.", 0xd); };
    const game::Spawn* target = nullptr;
    if (auto it = c.world().spawns.find(c.world().target_id); it != c.world().spawns.end()) target = &it->second;
    // A player 2, an NPC 3; corpses none.
    auto specific = [](const game::Spawn& s) -> std::optional<std::uint32_t> {
        if (s.is_corpse) return std::nullopt;
        return s.is_npc ? game::kXTargetSpecificNpc : game::kXTargetSpecificPc;
    };
    if (digit(word[0])) return target_slot(word[0]);
    if (prefix(word[0], "show", word[0].size())) {
        if (!eq_api_) return;
        if (prefix(word[1], "off", 3)) eq_api_->show("ExtendedTargetWnd", false);
        else if (prefix(word[1], "on", 2)) eq_api_->show("ExtendedTargetWnd", true);
        else eq_api_->toggle("ExtendedTargetWnd");
        return;
    }
    if (prefix(word[0], "target", 6)) {
        if (!digit(word[1])) return incorrect();
        return target_slot(word[1]);
    }
    if (prefix(word[0], "add", 3)) {
        if (!target || target->name.empty()) return;
        int slot = game::xtarget_first_auto_hater_slot(list, [&c](int i) { return c.xtarget_slot_hidden(i); });
        auto type = specific(*target);
        if (slot < 0 || !type) return;
        c.xtarget_request(slot, *type, target->name);
        return;
    }
    if (prefix(word[0], "remove", 6)) {
        if (prefix(word[1], "target", 6)) c.remove_target_xtargets();
        else c.remove_last_xtarget();
        return;
    }
    if (prefix(word[0], "set", 3)) {
        if (!digit(word[1])) return incorrect();
        const std::int64_t wide = std::int64_t{core::msvc_atoi(word[1])} - 1;
        if (wide < 0) return incorrect();
        const int slot = static_cast<int>(wide);
        std::uint32_t type = 0;
        std::string name;
        if (auto parsed = game::parse_xtarget_type(word[2])) {
            type = *parsed;
            if (type == game::kXTargetSpecificPc) { // CurrentTarget
                if (!target || target->name.empty()) return;
                auto t = specific(*target);
                if (!t) return;
                type = *t;
                name = target->name;
            }
        } else {
            // PlayerManagerClient::GetSpawnByName: the name as typed is sent.
            const game::Spawn* who = nullptr;
            for (const auto& [id, s] : c.world().spawns)
                if (game::same_no_case(s.name, word[2])) {
                    who = &s;
                    break;
                }
            if (!who) {
                c.show_chat("That is neither a valid slot type nor an existing player or NPC name.", 0xd);
                return incorrect();
            }
            auto t = specific(*who);
            if (!t) return;
            type = *t;
            name = word[2];
        }
        if (slot >= count) return range();
        c.xtarget_request(slot, type, name);
        return;
    }
    if (prefix(word[0], "auto", word[0].size())) {
        bool on = prefix(word[1], "off", 3) ? false : prefix(word[1], "on", 2) ? true : !list.auto_haters;
        c.set_xtarget_auto_haters(on); // the window stores its settings (StoreIniInfo) when it sees the change
        return;
    }
    incorrect();
}

bool ClientApp::client_command(std::string_view h, const std::string& args) {
    if (!conn_) return false;
    net::GameConnection& c = *conn_;
    auto eqstr = [&](std::uint32_t id) { return c.strings().format(id, {}); };
    auto save = [&] { config_.save(config_path_); };
    const std::string off = eqstr(12946), on = eqstr(12947);

    // ---- windows
    if (h == "Hailstone_abilitybook") {
        // The Ability Book (game/ability_book.h): no argument shows or hides it; "auto" toggles autoskill, "auto on"
        // / "auto off" set it.
        if (!eq_api_) return true;
        std::string_view rest = args;
        std::string word = game::next_cmd_arg(rest);
        for (char& ch : word) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (word.empty()) {
            eq_api_->show("AbilityBookWnd", !eq_api_->shown("AbilityBookWnd"));
        } else if (word == "auto") {
            if (distribution::managed()) {
                c.show_chat("Autoskill is disabled in this distribution.", 13);
                return true;
            }
            std::string to = game::next_cmd_arg(rest);
            for (char& ch : to) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            eq_api_->click_child("AbilityBookWnd", to == "on" ? "AutoskillOn" : to == "off" ? "AutoskillOff" : "AutoskillToggle");
        } else {
            c.show_chat("Usage: /abilitybook [auto [on|off]]", 15);
        }
        return true;
    }
    if (h == "Hailstone_finditem") {
        // The Advanced Inventory (/finditem, /advi): no argument shows or hides it; text opens it searching for that
        // (its filters cleared, so the search covers everything the character has).
        if (!eq_api_) return true;
        std::string text = args;
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
        if (text.empty()) {
            eq_api_->toggle("InventoryManagerWnd");
        } else {
            eq_host_.item_search = text;
            eq_api_->show("InventoryManagerWnd", true);
        }
        return true;
    }
    if (h == "Cmd_xtarget") {
        xtarget_command(args);
        return true;
    }
    if (h == "Cmd_filter") {
        // Cmd_filter @0x4e83a0: the options window shown on its chat filter page (4).
        if (eq_api_) {
            eq_api_->show("OptionsWindow", true);
            eq_api_->set_tab_page("OptionsWindow", "OPTW_OptionsSubwindows", 4);
        }
        return true;
    }
    if (h == "Cmd_note") {
        // Cmd_note @0x4f5e50: no text opens the notes window; text is added to it (13224 when that fails).
        if (!eq_api_) return true;
        if (args.empty()) eq_api_->show("PlayerNotesWindow", true);
        else if (!eq_api_->add_player_note(args)) c.show_eqstr(13224);
        return true;
    }
    if (h == "Cmd_map") {
        if (eq_api_)
            for (std::uint32_t id : eq_api_->map_command(args)) c.show_eqstr(id);
        return true;
    }
    if (h == "Cmd_loadskin") {
        // Cmd_loadskin @0x4f31e0: the skin is this client's user interface (ui/<pack>). No argument: the load-skin
        // window (LoadskinWnd, app/lua_eq_layout.cpp: the user interfaces). Not while the give, trade or send-money window is
        // open (3192, colour 0xf). "<skin> [flag]" writes the skin (the server's UI choice, as the Options window
        // does), says 3125 and loads it before the next frame; the flag (reload the window settings) is not used: the
        // window places are the character's and stay. (Its zone check, zone type 0xbc: 6028, has no such zone here.)
        if (!eq_api_) return true;
        if (args.empty()) {
            eq_api_->show("LoadskinWnd", true);
            return true;
        }
        if (eq_api_->shown("GiveWnd") || eq_api_->shown("TradeWnd") || c.trade().has_value()) {
            c.show_eqstr(3192, {}, 0xf);
            return true;
        }
        std::string_view rest = args;
        std::string skin = game::next_cmd_arg(rest);
        if (skin.empty()) return c.show_eqstr(3124), true;
        c.show_eqstr(3125, {skin});
        for (std::size_t i = 0; i < ui_packs_.size(); ++i)
            if (game::same_no_case(ui_packs_[i].id, skin) || game::same_no_case(ui_packs_[i].name, skin)) {
                set_video_option("ui_name", static_cast<int>(i));
                pending_ui_pack_ = ui_packs_[i].id;
                return true;
            }
        log::warn("/loadskin: no user interface named {}", skin);
        return true;
    }

    if (h == "Cmd_copylayout") {
        // Cmd_copylayout @0x4f33d0: no argument opens the layout copy window
        // (LayoutCopyWindow, app/lua_eq_layout.cpp); not while the give, trade or send-money window is open (3192,
        // colour 0xf); "usage" / "help" print 6335; "restore" is the screen's size as "%dx%d", else the first two
        // words are the size and the ini file (none: the current one; neither: 6335). It prints 6336 and copies
        // (EqHost::copy_layout, the boxes off). The ini file is UI_<name>_<server>.ini, default(s).ini or a
        // character's name here; the posted message's own reading of it (WndProc 0x4646) is not decoded
        // (docs/unknowns.md).
        if (!eq_api_) return true;
        if (args.empty()) {
            eq_api_->show("LayoutCopyWindow", true);
            return true;
        }
        if (eq_api_->shown("GiveWnd") || eq_api_->shown("TradeWnd") || c.trade().has_value()) {
            c.show_eqstr(3192, {}, 0xf);
            return true;
        }
        if (game::same_no_case(args, "usage") || game::same_no_case(args, "help")) return c.show_eqstr(6335), true;
        std::string resolution, ini;
        if (game::same_no_case(args, "restore")) {
            resolution = std::format("{}x{}", static_cast<int>(ImGui::GetIO().DisplaySize.x), static_cast<int>(ImGui::GetIO().DisplaySize.y));
        } else {
            std::string_view rest = args;
            resolution = game::next_cmd_arg(rest);
            ini = game::next_cmd_arg(rest);
        }
        if (resolution.empty()) return c.show_eqstr(6335), true;
        std::vector<EqHost::LayoutSource> sources = eq_host_.layout_sources ? eq_host_.layout_sources() : std::vector<EqHost::LayoutSource>{};
        auto names = [&](const EqHost::LayoutSource& s) {
            if (ini.empty()) return s.file == eq_api_->layout_file();
            if (s.label == "Defaults.ini") return game::same_no_case(ini, "defaults.ini") || game::same_no_case(ini, "default.ini");
            return game::same_no_case(ini, s.label) || game::same_no_case(ini, "UI_" + s.label + "_" + char_view_.server_name + ".ini");
        };
        EqHost::LayoutSource from;
        from.mode = game::same_no_case(resolution, "windowed") ? "Windowed" : resolution;
        from.label = ini.empty() ? "UI.ini" : ini;
        for (const auto& s : sources)
            if (names(s)) {
                from.file = s.file;
                from.label = s.label;
                break;
            }
        if (from.file.empty()) from.file = ini.empty() ? eq_api_->layout_file() : std::filesystem::path(ini);
        c.show_eqstr(6336, {from.label, resolution});
        if (eq_host_.copy_layout)
            if (auto refused = eq_host_.copy_layout(from, {})) c.show_eqstr(refused->first, refused->second);
        return true;
    }

    // ---- chat windows
    if (h == "Cmd_chatfontsize") {
        // Cmd_chatfontsize @0x4e9a10: atoi 0..10 for the active chat window (a word that is not a number is 0);
        // otherwise 13270 (colour 0x111, to the active window in the original).
        if (!args.empty()) {
            auto size = static_cast<std::uint32_t>(game::eq_atoi(args));
            if (size <= 10) {
                set_active_chat_font(static_cast<int>(size));
                return true;
            }
        }
        c.show_eqstr(13270, {}, 0x111);
        return true;
    }
    if (h == "Cmd_clearchat" || h == "Cmd_clearallchat") {
        clear_chat_windows(h == "Cmd_clearallchat");
        return true;
    }
    if (h == "Cmd_usercolor") {
        // Cmd_usercolor @0x4e9b60 (game::usercolor_command): a help page, or the colour set (SetUserDefinedColor with
        // index - 1, which ignores anything outside 0..98) and kept (User_<n>_Red / _Green / _Blue in chat.ini).
        game::UserColor u = game::usercolor_command(args);
        if (u.page) {
            for (const char* line : game::usercolor_help_lines(*u.page)) c.show_chat(line, 0x111);
            return true;
        }
        std::uint32_t rgb = u.reset ? default_user_color_rgb(u.index - 1)
                                    : static_cast<std::uint32_t>(u.red << 16 | u.green << 8 | u.blue);
        set_user_color(eq_host_, u.index - 1, rgb);
        return true;
    }
    if (h == "Cmd_hotbutton") {
        // Cmd_hotbutton @0x4f79b0: a social made from the line (game::hotbutton_command), put on the cursor (the
        // original's 0x4ea9f0 with start -1); 13282 made, 13283 not; no name the usage.
        game::HotButtonCommand hb = game::hotbutton_command(args);
        if (hb.usage) {
            for (std::uint32_t id = game::kStrHotButtonUsage; id <= game::kStrHotButtonUsage + 3; ++id) c.show_eqstr(id);
            return true;
        }
        int index = actions_ui_ ? actions_ui_->store_social(hb.name, hb.line, hb.color) : -1;
        if (index >= 0) {
            game::HotButton b;
            b.type = game::HotButtonType::Social;
            b.slot = index;
            eq_host_.cursor_button = b;
            ++eq_host_.cursor_serial;
        }
        c.show_eqstr(index >= 0 ? game::kStrHotButtonMade : game::kStrHotButtonFailed);
        return true;
    }

    // ---- display
    if (h == "Cmd_shownames") {
        // Cmd_shownames @0x4eb0f0: the line first, then the names (CDisplay::ToggleCharacterNameSprites) and the
        // level (__ShowNames, the name sprite's parts: draw_nameplates); ShowNamesLevel and PCNames kept.
        game::ShowNames n = game::shownames_command(args, off);
        c.show_eqstr(n.eqstr, {}, 0x111);
        if (n.usage) return true;
        pc_names_ = !n.off;
        if (!n.off) show_names_ = n.level;
        config_.set_int("display.show_names", n.off ? 0 : n.level);
        config_.set_bool("display.pc_names", pc_names_);
        save();
        return true;
    }
    if (h == "Cmd_showspelleffects") {
        // Cmd_showspelleffects @0x4e5780: "off" turns the spell particles off, any other word on; no word the usage
        // (13168, 13169); then the state (13170 / 13171). ShowSpellEffects kept.
        if (!args.empty()) {
            spell_effects_ = !game::same_no_case(args, off);
            config_.set_bool("display.spell_effects", spell_effects_);
            save();
        } else {
            c.show_eqstr(13168);
            c.show_eqstr(13169);
        }
        c.show_eqstr(spell_effects_ ? 13170 : 13171);
        return true;
    }
    if (h == "Cmd_dynamiclights") {
        // Cmd_dynamiclights @0x4e98f0: off / on (3 / 2 characters) / toggle; ShowDynamicLights kept; 13268 / 13269
        // (colour 0x111) say it takes effect at the next zone.
        bool lights = config_.get_bool("display.dynamic_lights", true);
        switch (game::off_on_toggle(args, off, on)) {
        case game::Switch::Off: lights = false; break;
        case game::Switch::On: lights = true; break;
        case game::Switch::Toggle: lights = !lights; break;
        }
        config_.set_bool("display.dynamic_lights", lights);
        save();
        c.show_eqstr(lights ? 13268 : 13269, {}, 0x111);
        return true;
    }
    if (h == "Cmd_showgrass") {
        // Cmd_showgrass @0x4e6da0: on / off when it is not so already (13179 / 13182; 13181 / 13183 when it is);
        // anything else the usage (13184) and the state. ShowGrass kept.
        bool grass = grass_;
        if (!args.empty() && (game::same_no_case(args, on) || game::same_no_case(args, off))) {
            bool want = game::same_no_case(args, on);
            if (grass == want) {
                c.show_eqstr(want ? 13181 : 13183);
            } else {
                grass_ = want; // the scene's virtual +0xb8: the flora drawn or not
                config_.set_bool("display.grass", want);
                save();
                c.show_eqstr(want ? 13179 : 13182);
            }
            return true;
        }
        c.show_eqstr(13184);
        c.show_eqstr(grass ? 13179 : 13182);
        return true;
    }
    if (h == "Cmd_shadows") {
        // Cmd_shadows @0x4de880: "on" turns shadows on (7002; here the Options window's shadows at Medium when they
        // were off) and a number after it is the shadow clip distance (0..100, ShadowClipPlane); "off" (7003);
        // anything else 7001.
        std::string_view rest = args;
        std::string first = game::next_cmd_arg(rest).substr(0, 0x27), second = game::next_cmd_arg(rest).substr(0, 0x27);
        if (game::same_no_case(first, on)) {
            if (display_option("shadows").value_or(0) == 0) set_display_option("shadows", 2);
            c.show_eqstr(7002);
            if (game::is_numeric_string(second)) {
                shadow_clip_plane_ = std::clamp(game::eq_atoi(second), 0, 100);
                config_.set_int("display.shadow_clip_plane", shadow_clip_plane_);
                save();
            }
        } else if (game::same_no_case(first, off)) {
            set_display_option("shadows", 0);
            c.show_eqstr(7003);
        } else {
            c.show_eqstr(7001);
        }
        return true;
    }
    if (h == "Cmd_particledensity") {
        // Cmd_particledensity @0x4ea740: 0..3 or OFF / LOW / MED / HIG(H); the line repeats the argument.
        // Cmd_particledensity_Set: the zone's particle clouds' density (EnvironmentParticleDensity).
        auto level = game::particle_density(args);
        if (!level) return c.show_chat(game::kParticleDensityUsage), true;
        set_particle_density("environment", game::particle_density_value(*level));
        save();
        c.show_chat(game::particle_density_line(args));
        return true;
    }
    if (h == "Cmd_viewport") {
        // Cmd_viewport @0x4eb380 (game::viewport_command): set, reset, or else the current one and the usage
        // (colour 0x111). The 3D view is drawn in it from the next frame (CDisplay::UpdateCameraFov; update_view_area),
        // kept as display.viewport (the original: the UI ini's ViewPort section per resolution). The screen is the
        // window, in its own coordinates (the UI's).
        ImVec2 screen = ImGui::GetIO().DisplaySize;
        auto sw = static_cast<std::int32_t>(screen.x), sh = static_cast<std::int32_t>(screen.y);
        if (auto v = game::viewport_command(args, sw, sh)) {
            viewport_ = *v;
            config_.set("display.viewport", game::viewport_text(*v));
            save();
            return true;
        }
        // The current one: the globals as the frame drew them (the whole screen without one).
        game::Viewport now;
        now.v = {static_cast<std::int32_t>(view_area_.x), static_cast<std::int32_t>(view_area_.y),
                 static_cast<std::int32_t>(view_area_.x + view_area_.w), static_cast<std::int32_t>(view_area_.y + view_area_.h),
                 0, 0, 100, 100};
        if (viewport_) {
            for (int k = 4; k < 8; ++k) now.v[k] = viewport_->v[k];
            if (!viewport_->scaled()) now.v[6] = now.v[7] = 0;
        }
        c.show_chat(game::viewport_line(now), 0x111);
        c.show_eqstr(game::kStrViewportUsage, {}, 0x111);
        return true;
    }
    if (h == "Cmd_indicator") {
        // Cmd_indicator @0x5c5530: the target indicator off / on (__stricmp) for this session (its flag, which
        // Show3dTargetIndicator sets at the start); "on" reads TargetIndicator.ini again.
        if (args.empty()) return true;
        if (game::same_no_case(args, off)) {
            c.set_client_option_for_session(game::ClientOption::ShowTargetRing, 0);
        } else if (game::same_no_case(args, on)) {
            c.set_client_option_for_session(game::ClientOption::ShowTargetRing, 1);
            target_indicator_.reset();
        }
        return true;
    }
    if (h == "Cmd_fullscreen") {
        // Cmd_fullscreen @0x478140: "on" when windowed, "off" when full screen, "toggle" either way.
        bool full = video_option("window_mode").value_or(0) != 0;
        bool want = full;
        if (game::same_no_case(args, "on")) want = true;
        else if (game::same_no_case(args, "off")) want = false;
        else if (game::same_no_case(args, "toggle")) want = !full;
        if (want != full) set_video_option("window_mode", want ? 2 : 0);
        return true;
    }
    if (h == "Cmd_fontface") {
        // Cmd_fontface @0x4de560: no face 3378 (colour 0xf); else Fonts / Font.<language>.0 (kept as ui.font_face).
        if (args.empty()) return c.show_eqstr(3378, {}, 0xf), true;
        config_.set("ui.font_face", args);
        save();
        apply_font_face(); // the font manager's +0x90: every UI font in the face from the next frame
        return true;
    }

    // ---- sound and mouse
    if (h == "Cmd_combatmusic") {
        // Cmd_combatmusic @0x4e5880: "off" off, any other word on (CombatMusic); no word 7523; then 7524 / 7525.
        if (!args.empty()) set_option_value("combat_music", game::same_no_case(args, off) ? 0 : 1);
        else c.show_eqstr(7523);
        c.show_eqstr(option_value("combat_music").value_or(1) ? 7524 : 7525);
        return true;
    }
    if (h == "Cmd_reversesound") {
        // Cmd_reversesound @0x4e81e0: "normal" (4 characters) / "reverse" (3) swap the stereo channels back or over;
        // no word 13214 and 13215; then the state (13217 / 13218); ReverseSound kept either way.
        bool reverse = option_value("reverse_sound").value_or(0) != 0;
        if (args.empty()) {
            c.show_eqstr(13214);
            c.show_eqstr(13215);
        } else if (game::starts_no_case(args, eqstr(13120), 4)) {
            reverse = false;
        } else if (game::starts_no_case(args, eqstr(13216), 3)) {
            reverse = true;
        }
        c.show_eqstr(reverse ? 13217 : 13218);
        set_option_value("reverse_sound", reverse ? 1 : 0);
        return true;
    }
    if (h == "Cmd_mixahead") {
        // Cmd_mixahead @0x4f5230: 1..100 kept (SoundMixAhead); else 7528; then 7529 with the value.
        // EqSoundManager::SetMixAhead: the output's buffer (audio::mix_ahead_ms).
        audio::AudioSettings settings = audio_ ? audio_->settings() : audio::load_audio_settings(config_);
        int value = settings.mix_ahead;
        if (auto v = game::mixahead_value(args)) {
            value = settings.mix_ahead = *v;
            if (audio_) audio_->set_settings(settings);
            audio::store_audio_settings(settings, config_);
            save();
        } else {
            c.show_eqstr(game::kStrMixAheadUsage);
        }
        c.show_eqstr(game::kStrMixAhead, {std::to_string(value)});
        return true;
    }
    if (h == "Cmd_mousespeed") {
        // Cmd_mousespeed @0x4f5130: a number kept as the pointer speed (MousePointerSpeedMod, the argument as typed);
        // else 13165, 13166; then 13167 with the value.
        int value = window_.pointer_speed();
        if (game::is_numeric_string(args)) {
            value = game::eq_atoi(args);
            set_pointer_speed(value);
            config_.set("mouse.pointer_speed", args);
            save();
        } else {
            c.show_eqstr(game::kStrMouseSpeedUsage);
            c.show_eqstr(game::kStrMouseSpeedUsage + 1);
        }
        c.show_eqstr(game::kStrMouseSpeed, {std::to_string(value)});
        return true;
    }
    return client_info_command(h, args);
}

} // namespace client_internal

} // namespace hailstone::app
