// GameConnection: game commands that act on the player or the target: /shieldgroup, /blockspell, /sheathe, /open,
// /reveal, /hidecorpses (and the loot window's hide of a looted corpse), /decaycorpse and /setstartcity. Packets in
// net/player_packets.h and net/spell_packets.h; every function names the original it follows (in
// eqgame.exe).

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "core/log.h"
#include "game/distance.h"
#include "game/slash_commands.h"
#include "game/mechanics/character.h"
#include "game/player_customization.h"
#include "game/spells.h"
#include "game/world_state.h"
#include "net/game_connection.h"
#include "net/packet_io.h"
#include "net/player_packets.h"
#include "net/spell_packets.h"

namespace hailstone::net {

namespace {

constexpr std::uint32_t kColorCommand = 0x111;  // the shield commands' refusals
constexpr std::uint32_t kColorLDoN = 0x10e;     // /open and /reveal (270)
constexpr std::uint32_t kColorBlockspell = 0xf; // /blockspell's usage and /hidecorpses' answers
constexpr std::uint32_t kColorBlockList = 10;   // /blockspell display
constexpr std::uint8_t kClassLDoNObject = 62;   // an adventure chest (+0xeb8 == 0x3e)
constexpr float kOpenRange = 20.0f;             // *0x9c4ec0 (Send_OP_LDoNOpen, PlayerClient_IsWithinInteractRange)
constexpr float kRevealRange = 75.0f;           // *0x9ca704 (Cmd_reveal)
constexpr std::int32_t kSpaShieldGroup = 0x16e; // Cmd_shieldgroup's GetLargestCachedEffect(0x16e, 0)
constexpr std::uint32_t kExpansionProphecyOfRo = 0x400;

// __strnicmp(arg, word, strlen(arg)) == 0: the argument is a prefix of the word, case ignored (an empty one matches).
bool prefix_of(std::string_view arg, std::string_view word) {
    if (arg.size() > word.size()) return false;
    for (std::size_t k = 0; k < arg.size(); ++k)
        if (std::tolower(static_cast<unsigned char>(arg[k])) != std::tolower(static_cast<unsigned char>(word[k])))
            return false;
    return true;
}

bool iequals(std::string_view a, std::string_view b) { return a.size() == b.size() && prefix_of(a, b); }

// Utf8StrNICmp(a, b, n) == 0: the first n characters agree, case ignored, up to a NUL in both.
bool same_first(std::string_view a, std::string_view b, std::size_t n) {
    for (std::size_t k = 0; k < n; ++k) {
        char ca = k < a.size() ? a[k] : '\0', cb = k < b.size() ? b[k] : '\0';
        if (std::tolower(static_cast<unsigned char>(ca)) != std::tolower(static_cast<unsigned char>(cb))) return false;
        if (ca == '\0') return true;
    }
    return true;
}

constexpr std::uint32_t kExpansionRainOfFear = 0x40000;

} // namespace

bool GameConnection::game_command(std::string_view h, const std::string& args) {
    if (h == "Cmd_shieldgroup") { shield_group_command(); return true; }
    if (h == "Cmd_blockspell") { blockspell_command(args); return true; }
    if (h == "Cmd_sheathe") {
        // Cmd_sheathe @0x4de600: toggles the sheathed bit (*0x9d77a0) of the player's PlayerState (+0x14c) and tells
        // the server: OP_PlayerStateRemove when it was set, else OP_PlayerStateAdd {spawn id, bit}.
        auto found = world_.spawns.find(world_.player_id);
        if (found == world_.spawns.end()) return true;
        game::Spawn* me = &found->second;
        bool add = (me->player_state & kPlayerStateSheathed) == 0;
        if (add) me->player_state |= kPlayerStateSheathed;
        else me->player_state &= ~kPlayerStateSheathed;
        send_zone(encode_player_state(add, PlayerState{world_.player_id, kPlayerStateSheathed}));
        return true;
    }
    if (h == "Send_OP_LDoNOpen") { ldon_open(); return true; }
    if (h == "Cmd_reveal") { reveal_target(); return true; }
    if (h == "cmd_hidecorpses") { hidecorpses_command(args); return true; }
    if (h == "Cmd_decaycorpse") { decaycorpse_command(); return true; }
    if (h == "Cmd_targetgroupbuff") { target_group_buff_command(args); return true; }
    if (h == "Cmd_spellscribe") { spellscribe_command(args); return true; }
    if (h == "Cmd_offlinemode") { offlinemode_command(); return true; }
    if (h == "Cmd_setstartcity") {
        // Cmd_setstartcity @0x4deac0: the argument text as it is (InterpretCmd hands an empty one too).
        send_zone(encode_set_start_city(args));
        return true;
    }
    return false;
}

bool GameConnection::may_shield() {
    // MayShield: no merchant, guild master or banker open
    // (0x1aaa) and a warrior of level 30 or more (0xfc1). Colour 0x111.
    if (merchant_ || training_ || bank_) {
        system(6826, {}, kColorCommand);
        return false;
    }
    if (!(player_class_mask() & 1u) || pstatus_.level < 30) {
        system(4033, {}, kColorCommand);
        return false;
    }
    return true;
}

void GameConnection::shield_group_command() {
    // Cmd_shieldgroup @0x4e8fd0: when commands may be used now, with the group shielding effect (SPA 0x16e: the
    // largest of the cached buff, AA and item totals; else 0x1ab0) and MayShield: opcode 0x5a79 with the spawn ids of
    // the group members in the zone; without a group 0x1aa8 and 0x1aa9. Colour 0x111.
    if (!can_use_command_now()) return;
    std::uint32_t effect = 0;
    with_character([&](game::mechanics::Character& ch) {
        effect = std::max({static_cast<std::uint32_t>(ch.cached_buff(kSpaShieldGroup, 0)),
                           static_cast<std::uint32_t>(ch.cached_aa(kSpaShieldGroup, 0)),
                           static_cast<std::uint32_t>(ch.cached_item(kSpaShieldGroup, 0))});
    });
    if (effect == 0) {
        system(0x1ab0, {}, kColorCommand);
        return;
    }
    if (!may_shield()) return;
    if (!group_.active) {
        system(0x1aa8, {}, kColorCommand);
        system(0x1aa9, {}, kColorCommand);
        return;
    }
    // CGroupBase +4..+0x18, each member's spawn (+0x28) when it is in the zone.
    std::vector<std::uint32_t> ids;
    for (const auto& m : group_.slots) {
        if (m.name.empty()) continue;
        for (const auto& [id, s] : world_.spawns) {
            if (s.is_corpse || !iequals(game::display_name(s.name), m.name)) continue;
            ids.push_back(id);
            break;
        }
    }
    send_zone(encode_shield_group(ids));
}

void GameConnection::blockspell_command(const std::string& args) {
    // Cmd_blockspell @0x4fc730: Prophecy of Ro owners only. "/blockspell <action> <me|pet> <spell ids>": the action
    // is the first of error, display, add, remove, clear, reload that the word begins (error and reload do nothing);
    // the target word must begin "me" or "pet" (else nothing). The lines are the exe's own text, colour 0xf.
    auto line = [&](const char* text, std::uint32_t color = kColorBlockspell) {
        chat(ChatEvent::Kind::System, text, 0, color);
    };
    if (!profile_ || !(profile_->expansions & kExpansionProphecyOfRo)) {
        line("You need to own EverQuest: Prophecy of Ro to use this feature.");
        return;
    }
    if (args.empty()) {
        line("Usage: /blockspell <action> <target> <spellnumber>");
        line("Valid <action>: display, add, remove, clear");
        line("Valid <target>: me pet");
        line("Valid <spellnumber>: list of spell numbers separated by spaces");
        line("Example:  /blockspell add me 1693 2517");
        line("Example:  /blockspell display pet");
        return;
    }
    static constexpr std::string_view kActions[] = {"error", "display", "add", "remove", "clear", "reload"};
    enum { kError, kDisplay, kAdd, kRemove, kClear, kReload };
    std::string_view rest = args;
    std::string word = game::next_cmd_arg(rest);
    int action = kError;
    for (int k = 0; k < 6; ++k)
        if (prefix_of(word, kActions[k])) {
            action = k;
            break;
        }
    std::string target = game::next_cmd_arg(rest);
    bool pet;
    if (prefix_of(target, "me")) pet = false;
    else if (prefix_of(target, "pet")) pet = true;
    else return;
    switch (action) {
    case kClear:
        send_zone(encode_clear_blocked_buffs(pet));
        return;
    case kDisplay: {
        // PcBase_GetBlockedBuffList: 4580 (pets) or 4579, then each spell's name, colour 10. The original walks its
        // hash table's buckets; this list is in the server's order.
        system(pet ? 4580 : 4579, {}, kColorBlockList);
        for (std::int32_t id : blocked_buffs_[pet ? 1 : 0]) {
            const game::SpellData* sp = spell_table_ && id > 0 ? spell_table_->get(static_cast<std::uint32_t>(id)) : nullptr;
            if (sp) line(sp->name.c_str(), kColorBlockList);
        }
        // the original falls through to read the spell numbers, then sends nothing for display
        return;
    }
    case kAdd:
    case kRemove: {
        // 30 arguments read with GetNextCmdArg and atoi: a number fills its slot, anything else -1; the count is
        // how many were numbers. OP_BlockedBuffs adds, OP_RemoveBlockedBuffs removes (128 bytes, initialise 0).
        BlockedBuffs b;
        std::uint32_t count = 0;
        for (int k = 0; k < kBlockedBuffSlots; ++k) {
            std::int32_t id = std::atoi(game::next_cmd_arg(rest).c_str());
            if (id == 0) id = -1;
            else ++count;
            b.spells.push_back(id);
        }
        b.count = count;
        b.pet = pet;
        send_zone(encode(b, action == kRemove));
        return;
    }
    default:
        return;
    }
}

void GameConnection::ldon_open() {
    // Send_OP_LDoNOpen @0x4e7cf0: the target must be an adventure chest (class 62) within 20 (an equal distance
    // counts) and in interact range (PlayerClient_IsWithinInteractRange(target, 1, 20.0): the same distance here).
    // No target 8518, not a chest 8519, too far 8528; colour 0x10e. Then an empty OP_LDoNOpen.
    auto it = world_.spawns.find(world_.target_id);
    if (world_.target_id == 0 || it == world_.spawns.end() || !world_.player()) {
        system(8518, {}, kColorLDoN);
        return;
    }
    if (it->second.class_id != kClassLDoNObject) {
        system(8519, {}, kColorLDoN);
        return;
    }
    if (distance_to(world_.target_id) > kOpenRange) {
        system(8528, {}, kColorLDoN);
        return;
    }
    send_zone(make_empty(Op::LDoNOpen));
}

void GameConnection::reveal_target() {
    // Cmd_reveal @0x4e7e30: no target 9157; farther than 75 (CDisplay::PlayerDistance) 9158; colour 0x10e. Then
    // opcode 0x15e7 with no body.
    if (world_.target_id == 0 || !world_.spawns.count(world_.target_id)) {
        system(9157, {}, kColorLDoN);
        return;
    }
    if (distance_to(world_.target_id) > kRevealRange) {
        system(9158, {}, kColorLDoN);
        return;
    }
    send_zone(encode_reveal());
}

void GameConnection::hidecorpses_command(const std::string& args) {
    // cmd_hidecorpses @0x4e78c0 matches the whole argument without case and prints the answer before sending
    // OP_HideCorpse. Hailstone keeps the mode for this character and adds a local "always" mode.
    using Mode = game::HideCorpseMode;
    if (args.empty()) {
        system(13202);
        system(13203);
        system(13204);
        chat(ChatEvent::Kind::System, "/hidecorpses always also hides your own corpse.");
        constexpr const char* modes[] = {"none", "all", "allbutgroup", "looted", "npc", "always"};
        chat(ChatEvent::Kind::System,
             std::string("Current /hidecorpses mode is ") + modes[static_cast<int>(hide_corpse_mode())] + ".");
        return;
    }
    auto is = [&](std::string_view word, std::uint32_t id) {
        return iequals(args, word) || iequals(args, strings_.format(id, {}));
    };
    Mode mode = Mode::None;
    std::uint32_t answer = 13209;
    if (is("all", 13205)) mode = Mode::All, answer = 13206;
    else if (is("allbutgroup", 13207)) mode = Mode::AllButGroup, answer = 13208;
    else if (is("looted", 13302)) mode = Mode::Looted, answer = 13303;
    else if (is("npc", 6358)) mode = Mode::Npc, answer = 6359;
    else if (iequals(args, "always")) mode = Mode::Always;
    if (mode == Mode::Always)
        chat(ChatEvent::Kind::System, "All corpses are now hidden, including your own.", 0, kColorBlockspell);
    else
        system(answer, {}, kColorBlockspell);

    const auto option = game::ClientOption::HideCorpses;
    if (mode == hide_corpse_mode()) send_hide_corpse_setting();
    else change_client_option(option, static_cast<int>(mode));
}

void GameConnection::send_hide_corpse_setting(game::HideCorpseMode previous) {
    if (state_ != ConnectionState::InZone) return;
    using Mode = game::HideCorpseMode;
    const Mode mode = hide_corpse_mode();
    const bool hid_players = previous == Mode::All || previous == Mode::AllButGroup;
    // EQEmu ignores Looted and only despawns NPCs for Npc. Clear a broader server filter before either mode.
    if ((mode == Mode::Npc && hid_players) || (mode == Mode::Looted && (hid_players || previous == Mode::Npc)))
        send_zone(encode_hide_corpse(kHideCorpseNone));
    std::uint32_t action = kHideCorpseNone;
    switch (mode) {
    case Mode::All: action = kHideCorpseAll; break;
    case Mode::AllButGroup: action = kHideCorpseAllButGroup; break;
    case Mode::Looted: action = kHideCorpseLooted; break;
    case Mode::Npc: action = kHideCorpseNpc; break;
    case Mode::None:
    case Mode::Always: break; // "always" hides locally, after clearing the server filter.
    }
    send_zone(encode_hide_corpse(action));
}

void GameConnection::hide_looted_corpse() {
    // CLootWnd::AboutToHide @0x6bcd40, with the hide-looted flag: when items are left (0x6bee70 counts the
    // loot slots) OP_HideCorpse {4, the corpse}; then a corpse that is not the player's own is hidden here
    // (bHideCorpse, PrepForDestroyPlayer). The original's own-corpse test compares two names it reads through
    // 0x59e520 (not decoded): taken here as the corpse's name holding the player's.
    if (hide_corpse_mode() != game::HideCorpseMode::Looted || !loot_) return;
    std::uint32_t corpse = loot_->corpse_id;
    if (!loot_->items.empty()) send_zone(encode_hide_corpse(kHideCorpseThis, corpse));
    auto it = world_.spawns.find(corpse);
    if (it == world_.spawns.end() || it->second.name.find(character_) != std::string::npos) return;
    if (world_.target_id == corpse) world_.target_id = 0;
    spawn_leaving(corpse);
    world_.spawns.erase(corpse);
    ++world_.spawn_set_serial;
}

void GameConnection::decaycorpse_command() {
    // Cmd_decaycorpse @0x479140: the target must be one of our corpses (a corpse whose name holds ours), else the
    // exe's own line; then a Yes / No question (10 s) whose yes (@0x479e30) sends opcode 0x7d50 {corpse, player}.
    auto it = world_.spawns.find(world_.target_id);
    if (world_.target_id == 0 || it == world_.spawns.end() || !it->second.is_corpse ||
        it->second.name.find(character_) == std::string::npos) {
        chat(ChatEvent::Kind::System, "You must target one of your own corpses to decay.");
        return;
    }
    Confirmation c;
    c.id = kConfirmDecayCorpse;
    c.text = "Are you sure you want to decay this corpse?";
    c.timeout_ms = 10000;
    c.data = world_.target_id;
    c.data2 = world_.player_id;
    ask(std::move(c));
}

game::FacialFeatures GameConnection::player_features() const {
    game::FacialFeatures f;
    const game::Spawn* me = world_.player();
    if (!me) return f;
    const game::Appearance& a = me->appearance;
    f.face = a.face;
    f.hair_color = a.hair_color;
    f.beard_color = a.beard_color;
    f.eye1 = a.eye_color1;
    f.eye2 = a.eye_color2;
    f.hair_style = a.hair_style;
    f.beard = a.beard;
    f.heritage = static_cast<std::int32_t>(a.drakkin_heritage);
    f.tattoo = static_cast<std::int32_t>(a.drakkin_tattoo);
    f.details = static_cast<std::int32_t>(a.drakkin_details);
    return f;
}

void GameConnection::set_player_features(const game::FacialFeatures& f) {
    auto it = world_.spawns.find(world_.player_id);
    if (it == world_.spawns.end()) return;
    game::Appearance& a = it->second.appearance;
    a.face = f.face;
    a.hair_color = f.hair_color;
    a.beard_color = f.beard_color;
    a.eye_color1 = f.eye1;
    a.eye_color2 = f.eye2;
    a.hair_style = f.hair_style;
    a.beard = f.beard;
    a.drakkin_heritage = static_cast<std::uint32_t>(f.heritage);
    a.drakkin_tattoo = static_cast<std::uint32_t>(f.tattoo);
    a.drakkin_details = static_cast<std::uint32_t>(f.details);
}

void GameConnection::accept_face_change() {
    // CPlayerCustomizationWnd::WndNotification @0x715f30, Accept in game: the profile takes the actor's features
    // (PcBase_SetBeard .. SetRightEye, the heritage, tattoo and details), OP_FaceChange carries them, then 12028.
    if (state_ != ConnectionState::InZone || !world_.player()) return;
    const game::FacialFeatures f = player_features();
    if (profile_) {
        profile_->beard = f.beard;
        profile_->beard_color = f.beard_color;
        profile_->face = f.face;
        profile_->hair_style = f.hair_style;
        profile_->hair_color = f.hair_color;
        profile_->eye_color1 = f.eye1;
        profile_->eye_color2 = f.eye2;
        profile_->drakkin_heritage = static_cast<std::uint32_t>(f.heritage);
        profile_->drakkin_tattoo = static_cast<std::uint32_t>(f.tattoo);
        profile_->drakkin_details = static_cast<std::uint32_t>(f.details);
    }
    FaceChange c;
    c.hair_color = f.hair_color;
    c.beard_color = f.beard_color;
    c.eye1 = f.eye1;
    c.eye2 = f.eye2;
    c.hair_style = f.hair_style;
    c.beard = f.beard;
    c.face = f.face;
    c.heritage = static_cast<std::uint32_t>(f.heritage);
    c.tattoo = static_cast<std::uint32_t>(f.tattoo);
    c.details = static_cast<std::uint32_t>(f.details);
    c.spawn_id = world_.player_id;
    send_zone(encode(c));
    system(12028); // Facial features accepted.
}

// ---------------------------------------------------------------- /targetgroupbuff, /spellscribe, /offlinemode

void GameConnection::target_group_buff_command(const std::string& args) {
    // Cmd_targetgroupbuff @0x4ea820 (/targetgroupbuff, /tgb): an argument starting with "off" (eqstr 12946, 3
    // characters) or "on" (12947, 2) is written as TargetGroupBuff "0" / "1" ([Defaults], here the character's
    // options.ini) and sent as OP_TGB {u32 0 / 1}; then, and for any other argument, OP_TGB {u32 2} asks the server for
    // the state (EQEmu answers TGB_ON / TGB_OFF when its Character:EnableTGB rule is on). No line of its own.
    if (state_ != ConnectionState::InZone) return;
    std::uint32_t setting = 2;
    if (same_first(args, strings_.format(12946, {}), 3)) setting = 0;
    else if (same_first(args, strings_.format(12947, {}), 2)) setting = 1;
    auto tgb = [&](std::uint32_t v) {
        ByteWriter w;
        w.le(v);
        send_zone(make_packet(Op::TGB, std::move(w)));
    };
    if (setting != 2) {
        int old_value = client_options_.get(game::ClientOption::TargetGroupBuff);
        client_options_.set(game::ClientOption::TargetGroupBuff, static_cast<int>(setting));
        if (old_value != static_cast<int>(setting)) save_client_option(game::ClientOption::TargetGroupBuff);
        tgb(setting);
    }
    tgb(2);
}

void GameConnection::send_target_group_buff_setting() {
    // CEverQuest::DoMainLoop @0x53d936: GetEQClientInt("Defaults", "TargetGroupBuff", "*NULL*"); unless the text
    // starts with '*', OP_TGB with its atoi.
    const int v = client_options_.get(game::ClientOption::TargetGroupBuff);
    if (v == game::kTargetGroupBuffUnset) return;
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(v));
    send_zone(make_packet(Op::TGB, std::move(w)));
}

void GameConnection::spellscribe_command(const std::string& args) {
    // Cmd_spellscribe @0x4f7b90: with a local player, the main profile (+0x340c 0: always here) and the world's
    // OP_LogServer +0x18 switch on (0xddfd3c; EQEmu sends 0, so nothing goes out there): opcode 0x51e5 with our name
    // over the target's (net/player_packets.h). Its three words are read (the second and third through atoi) and
    // not used.
    (void)args;
    const game::Spawn* me = world_.player();
    if (state_ != ConnectionState::InZone || !me || spell_scribe_allowed_ == 0) return;
    const game::Spawn* other = me;
    if (auto t = world_.spawns.find(world_.target_id); world_.target_id != 0 && t != world_.spawns.end())
        other = &t->second;
    send_zone(encode_spell_scribe(me->name, other->name));
}

void GameConnection::offlinemode_command() {
    // Cmd_offlinemode @0x4e6660: Send_OP_FloatListThing(1) (the position history, which this client does not keep)
    // and Save(0, 1, 0x25) first. Then a GM (CharacterBase virtual +0x90: 0x580300, the spawn's GM flag) or the
    // Rain of Fear flag (0x40000) else 774; a player in state 0x66 768; not trader (PlayerClient +0x320) nor buyer
    // (+0x3d4) 771. The time is kept (0xddf8ac; the link death within 15 s exits the client), and with
    // Options/OfflineModeConfirm the question 0x8f (13345, Yes / No, 60 s), else opcode 0x53d3 alone (not in
    // EQEmu's RoF2 table: EQEmu drops it).
    send_save(0x25);
    const game::Spawn* me = world_.player();
    const bool gm = me && me->gm;
    // The refusals go through dsp_chat_2 @0x51f3e0: colour 0x111.
    if (!gm && !(profile_ && (profile_->expansions & kExpansionRainOfFear))) return system(774, {}, 0x111);
    if (pstatus_.stand_state == 0x66) return system(768, {}, 0x111);
    if (!me || (!me->trader && !me->buyer)) return system(771, {}, 0x111);
    offline_mode_at_ = now_ms();
    log::info("Offline mode requested.");
    if (client_option(game::ClientOption::OfflineModeConfirm)) {
        Confirmation c;
        c.id = kConfirmOfflineMode;
        c.text = strings_.format(13345, {});
        c.timeout_ms = 60000;
        ask(std::move(c));
        return;
    }
    if (state_ == ConnectionState::InZone) send_zone(AppPacket(kOpOfflineMode));
}

} // namespace hailstone::net
