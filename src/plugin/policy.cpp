#include "plugin/policy.h"

#include "core/distribution.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <thread>
#include <utility>

#include "core/addon_source.h"
#include "core/ini_file.h"
#include "core/log.h"
#include "game/slash_commands.h"

namespace hailstone::plugin {

namespace {

constexpr std::array<std::pair<Cap, const char*>, 13> kNames{{{kCapRead, "read"},
                                                              {kCapCommands, "commands"},
                                                              {kCapTarget, "target"},
                                                              {kCapActions, "actions"},
                                                              {kCapMove, "move"},
                                                              {kCapChat, "chat"},
                                                              {kCapEvents, "events"},
                                                              {kCapUi, "ui"},
                                                              {kCapPackets, "packets"},
                                                              {kCapExtend, "extend"},
                                                              {kCapLogin, "login"},
                                                              {kCapNet, "net"},
                                                              {kCapWeb, "web"}}};

// EQ's commands by their original handler.
constexpr std::string_view kTarget[] = {"Send_OP_TargetCommand_004e9790", "Cmd_assist", "Cmd_rtarget", "Cmd_xtarget", "Cmd_targetgroupbuff"};
constexpr std::string_view kActions[] = {"Cmd_cast",     "Cmd_discipline", "Cmd_doability", "Cmd_useitem",
                                         "Cmd_attack",   "Cmd_memspellset", "Cmd_stopcast",  "Cmd_stopdisc",
                                         "Cmd_bandolier", "Cmd_potionbelt", "Cmd_stopsong",  "Cmd_melody",
                                         "Cmd_spellscribe", "Cmd_aggressive", "Cmd_stance"};
constexpr std::string_view kMove[] = {"Cmd_follow", "Cmd_sit", "Cmd_stand", "Cmd_duck", "Cmd_dismount",
                                      "Cmd_corpsedrag", "Cmd_camp", "Cmd_kneel"};
constexpr std::string_view kChat[] = {"Cmd_say",   "Cmd_gsay",     "Cmd_shout",     "Cmd_auction",  "Cmd_ooc",
                                      "Cmd_guildsay", "Cmd_rsay",  "Cmd_fsay",      "Cmd_tell",     "Cmd_reply",
                                      "Cmd_ttell", "Cmd_emote",    "Cmd_emoteworld", "Cmd_emotezone", "Cmd_announce",
                                      "Cmd_broadcast", "Cmd_chat", "Cmd_yell",      "Cmd_join",     "Cmd_1",
                                      "Cmd_2",     "Cmd_3",        "Cmd_4",         "Cmd_5",        "Cmd_6",
                                      "Cmd_7",     "Cmd_8",        "Cmd_9",         "Cmd_10"};
constexpr std::string_view kWeb[] = {"Cmd_url", "Cmd_www"};

template <std::size_t N> bool in(const std::string_view (&list)[N], std::string_view h) {
    return std::find(std::begin(list), std::end(list), h) != std::end(list);
}

} // namespace

std::string caps_text(std::uint32_t caps) {
    std::string s;
    for (auto [bit, name] : kNames)
        if (caps & bit) {
            if (!s.empty()) s += ',';
            s += name;
        }
    return s.empty() ? "none" : s;
}

std::uint32_t parse_caps(std::string_view text) {
    std::uint32_t caps = 0;
    while (!text.empty()) {
        std::size_t end = text.find_first_of(", ");
        std::string word(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (word == "all") caps |= kCapAll;
        for (auto [bit, name] : kNames)
            if (word == name) caps |= bit;
    }
    return caps;
}

std::optional<std::uint32_t> parse_caps_strict(std::string_view text) {
    std::uint32_t caps = 0;
    while (!text.empty()) {
        std::size_t end = text.find_first_of(", ");
        std::string word(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (word.empty() || word == "none") continue;
        if (word == "all") {
            caps |= kCapAll;
            continue;
        }
        auto it = std::find_if(kNames.begin(), kNames.end(), [&](const auto& n) { return word == n.second; });
        if (it == kNames.end()) return std::nullopt;
        caps |= it->first;
    }
    return caps;
}

std::vector<std::string> caps_described(std::uint32_t caps) {
    // The same order as kNames.
    static constexpr std::array<const char*, 13> kWords{
        "read the game state",
        "run commands (windows, options, other plugins' commands)",
        "change your target",
        "cast spells, use abilities and items, attack",
        "move your character (face, follow, sit, camp)",
        "speak for you (say, tells, group, guild, channels)",
        "see the chat lines, and hide them",
        "draw windows",
        "see, change and send raw packets",
        "change the client's rules (as a server's own client does)",
        "run from the world login, before the zone-in",
        "open network connections of its own",
        "open web pages in the in-game browser"};
    static_assert(kWords.size() == kNames.size(), "a capability without its words for the player");
    std::vector<std::string> out;
    for (std::size_t i = 0; i < kNames.size(); ++i)
        if (caps & kNames[i].first) out.emplace_back(kWords[i]);
    return out;
}

std::uint32_t command_caps(std::string_view line, const game::EqStrings* strings) {
    game::CommandLine cl = game::parse_command_line(line, false, strings);
    switch (cl.kind) {
    case game::CommandLine::Kind::Empty: return 0;
    case game::CommandLine::Kind::ChatServer:
    case game::CommandLine::Kind::Text: return kCapChat;
    case game::CommandLine::Kind::Invalid: return kCapCommands;
    case game::CommandLine::Kind::Command: break;
    }
    std::string_view h = game::slash_commands()[cl.index].handler;
    if (in(kTarget, h)) return kCapTarget;
    if (in(kActions, h)) return kCapActions;
    if (in(kMove, h)) return kCapMove;
    if (in(kChat, h)) return kCapChat;
    if (in(kWeb, h)) return kCapWeb;
    return kCapCommands;
}

namespace {
// The client's thread's: plugins and scripts run there (another thread sees none of it, on_main_thread).
std::uint32_t g_caller_caps = kCapAll;
std::string g_caller_name;
// Set during the program's static initialisation, which runs on the thread that starts it.
const std::thread::id g_main_thread = std::this_thread::get_id();
} // namespace

bool on_main_thread() { return std::this_thread::get_id() == g_main_thread; }

std::uint32_t caller_caps() { return on_main_thread() ? g_caller_caps : 0; }
std::string_view caller_name() { return on_main_thread() ? std::string_view(g_caller_name) : "another thread"; }

CallerScope::CallerScope(std::uint32_t caps, std::string_view name) : active_(on_main_thread()) {
    if (!active_) return;
    prev_caps_ = g_caller_caps;
    prev_name_ = std::move(g_caller_name);
    g_caller_caps = caps & prev_caps_; // a script a plugin runs gets no more than the plugin
    g_caller_name = name;
}

CallerScope::~CallerScope() {
    if (!active_) return;
    g_caller_caps = prev_caps_;
    g_caller_name = std::move(prev_name_);
}

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// A value without its trailing "; comment" or "# comment".
std::string value_of(std::string_view v) {
    std::size_t c = v.find_first_of(";#");
    return lower(trim(c == std::string_view::npos ? v : v.substr(0, c)));
}

std::optional<bool> on_off(std::string_view v) {
    if (v == "on" || v == "yes" || v == "true" || v == "1") return true;
    if (v == "off" || v == "no" || v == "false" || v == "0") return false;
    return std::nullopt;
}

// Every word of a capability list must be one (parse_caps ignores the others).
bool known_caps(std::string_view text) {
    while (!text.empty()) {
        std::size_t end = text.find_first_of(", ");
        std::string_view word = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (word.empty() || word == "all" || word == "none") continue;
        if (std::none_of(kNames.begin(), kNames.end(), [&](const auto& n) { return word == n.second; })) return false;
    }
    return true;
}

} // namespace

Rules parse_server_cfg(std::string_view text, std::vector<std::string>* warnings) {
    Rules r;
    r.configured = true;
    auto warn = [&](std::string w) {
        if (warnings) warnings->push_back(std::move(w));
    };
    IniFile ini;
    ini.parse(text);
    // [plugins] and [scripts]' player and player_caps, by section (1 scripts), settled after the loop.
    bool player_on[2] = {false, false};
    std::optional<std::uint32_t> player_caps[2];
    for (const std::string& section_name : ini.sections()) {
        std::string section = lower(section_name);
        if (section.empty()) {
            if (!ini.keys(section_name).empty()) warn("lines before the first [section] are ignored");
            continue;
        }
        static constexpr std::string_view kOwn[] = {"scripts", "plugins", "assets", "socials", "abilities", "movement", "web",
                                                    "addons"};
        if (std::find(std::begin(kOwn), std::end(kOwn), section) == std::end(kOwn)) {
            auto& kept = r.script_sections[section];
            for (const std::string& key_name : ini.keys(section_name))
                kept[lower(key_name)] = value_of(ini.get_string(section_name, key_name));
            warn(std::format("[{}] is not one of Hailstone's sections; kept for scripts (hailstone.server_rule)", section));
            continue;
        }
        for (const std::string& key_name : ini.keys(section_name)) {
            std::string key = lower(key_name);
            std::string v = value_of(ini.get_string(section_name, key_name));
            std::string where = std::format("[{}] {}", section, key);
            auto flag = [&](bool& out) {
                if (auto b = on_off(v)) out = *b;
                else warn(std::format("{}: \"{}\" is not on or off", where, v));
            };
            auto caps = [&](std::uint32_t& out) {
                if (!known_caps(v)) warn(std::format("{}: unknown capabilities in \"{}\" are ignored", where, v));
                out = parse_caps(v);
            };
            if (section == "scripts" || section == "plugins") {
                bool scripts = section == "scripts";
                if (key == "player") {
                    bool allowed = false;
                    flag(allowed);
                    player_on[scripts] = allowed;
                    continue;
                }
                if (key == "player_caps") {
                    std::uint32_t c = 0;
                    caps(c);
                    player_caps[scripts] = c;
                    continue;
                }
                if (scripts && key == "run") {
                    std::string_view rest = v;
                    while (!rest.empty()) {
                        std::size_t end = rest.find_first_of(", ");
                        std::string_view name = rest.substr(0, end);
                        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
                        if (!name.empty()) r.run_scripts.emplace_back(name);
                    }
                    continue;
                }
            } else if (section == "assets") {
                if (key == "player") {
                    flag(r.player_assets);
                    continue;
                }
            } else if (section == "socials") {
                if (key == "mode") {
                    if (v == "off") r.social_mode = ButtonMode::Off;
                    else if (v == "press") r.social_mode = ButtonMode::Press;
                    else if (v == "full") r.social_mode = ButtonMode::Full;
                    else warn(std::format("{}: \"{}\" is not off, press or full", where, v));
                    continue;
                }
                if (key == "caps") {
                    caps(r.social_caps);
                    continue;
                }
                if (key == "press_commands") {
                    int n = -1;
                    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
                    if (ec != std::errc() || p != v.data() + v.size() || n < 0)
                        warn(std::format("{}: \"{}\" is not a number", where, v));
                    else
                        r.social_press_commands = n;
                    continue;
                }
                if (key == "faces") {
                    flag(r.social_faces);
                    continue;
                }
            } else if (section == "abilities") {
                if (key == "autoskill") {
                    flag(r.autoskill);
                    continue;
                }
            } else if (section == "movement") {
                if (key == "npc_collision") {
                    flag(r.npc_collision);
                    continue;
                }
                if (key == "passable_ladders") {
                    flag(r.passable_ladders);
                    continue;
                }
                if (key == "solid_ladders") {
                    flag(r.solid_ladders);
                    continue;
                }
            } else if (section == "web") {
                if (key == "allow") {
                    std::string_view rest = v;
                    while (!rest.empty()) {
                        std::size_t end = rest.find_first_of(", ");
                        std::string_view host = rest.substr(0, end);
                        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
                        if (!host.empty()) r.web_allow.emplace_back(host);
                    }
                    continue;
                }
            } else if (section == "addons") {
                if (key == "suggest") {
                    std::string_view rest = v;
                    while (!rest.empty()) {
                        std::size_t end = rest.find_first_of(", ");
                        std::string_view link = rest.substr(0, end);
                        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
                        if (link.empty()) continue;
                        std::string why;
                        if (auto src = parse_addon_link(link, &why)) r.suggest_addons.push_back(addon_source_text(*src));
                        else warn(std::format("{}: \"{}\" is not a GitHub add-on link ({})", where, link, why));
                    }
                    continue;
                }
            }
            warn(std::format("{}: not a setting Hailstone knows; ignored", where));
        }
    }
    for (int scripts = 0; scripts < 2; ++scripts) {
        if (player_caps[scripts] && !player_on[scripts])
            warn(std::format("[{}] player_caps: has no effect without player = on", scripts ? "scripts" : "plugins"));
        (scripts ? r.player_script_caps : r.player_plugin_caps) = player_on[scripts] ? player_caps[scripts].value_or(kCapAll) : 0;
    }
    return r;
}

Rules server_rules(const std::filesystem::path& dir, const std::vector<ListedFile>& files,
                   std::vector<std::string>* warnings) {
    Rules r;
    const ListedFile* cfg = nullptr;
    for (const auto& f : files) {
        std::string path = lower(f.path);
        if (path == kServerCfgName) cfg = &f;
        else if (path.starts_with("scripts/")) r.server_files.push_back({FileKind::Script, f.path, f.sha256});
        else if (path.starts_with("plugins/")) r.server_files.push_back({FileKind::Plugin, f.path, f.sha256});
    }
    if (cfg) {
        std::ifstream in(dir / cfg->path, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in.bad() || sha256(text) != cfg->sha256) {
            if (warnings) warnings->push_back(std::string(kServerCfgName) + " is not the server's: its defaults apply");
            text.clear();
        }
        std::vector<ServerFile> sent = std::move(r.server_files);
        r = parse_server_cfg(text, warnings);
        r.server_files = std::move(sent);
    }
    r.dir = dir;
    return r;
}

void Policy::set_ceilings(std::vector<Ceiling> ceilings) {
    ceilings_ = std::move(ceilings);
    ++serial_;
}

std::optional<std::uint32_t> Policy::ceiling(FileKind kind, const Sha256Digest& hash) const {
    std::optional<std::uint32_t> out;
    // A file two add-ons both ship gets what both allow.
    for (const auto& c : ceilings_)
        if (c.kind == kind && c.sha256 == hash) out = out.value_or(kCapAll) & c.caps;
    return out;
}

void Policy::set(Rules rules) {
    rules_ = std::move(rules);
    ++serial_;
}

void Policy::clear() {
    if (!rules_) return;
    rules_.reset();
    ++serial_;
}

bool Policy::server_file(FileKind kind, const Sha256Digest& hash) const {
    if (!rules_) return false;
    return std::any_of(rules_->server_files.begin(), rules_->server_files.end(),
                       [&](const ServerFile& f) { return f.kind == kind && f.sha256 == hash; });
}

std::uint32_t Policy::caps(FileKind kind, const Sha256Digest& hash) const {
    // A managed distribution admits only its linked module, independently of server/local policy files.
    if (distribution::managed()) return 0;
    if (!rules_) return 0;
    std::uint32_t caps = kCapAll;
    if (rules_->configured && !server_file(kind, hash))
        caps = (kind == FileKind::Plugin ? rules_->player_plugin_caps : rules_->player_script_caps) & kCapAll;
    // An add-on's file: no more than its manifest declared (the server's own files keep every capability).
    if (!server_file(kind, hash))
        if (auto c = ceiling(kind, hash)) caps &= *c;
    // Before the zone-in only what may run then.
    if (rules_->provisional && !(caps & kCapLogin)) return 0;
    return caps;
}

std::string Policy::refusal(FileKind kind) const {
    const char* what = kind == FileKind::Plugin ? "plugins" : "scripts";
    if (!rules_) return std::format("{} are off until the zone-in has checked the server's files", what);
    if (rules_->provisional)
        return std::format("before the zone-in only {} with the server's 'login' permission run", what);
    return std::format("this server runs only its own {} (its {})", what, kServerCfgName);
}

ButtonRules Policy::button_rules() const {
    ButtonRules r;
    if (distribution::managed()) {
        r.mode = ButtonMode::Off;
        r.faces = false;
        return r;
    }
    if (!rules_ || rules_->provisional) return r; // socials: from the zone-in
    if (!rules_->configured) {
        r.mode = ButtonMode::Full;
        r.caps = kCapAll & ~(kCapPackets | kCapExtend | kCapLogin | kCapNet);
        r.faces = true;
        return r;
    }
    r.mode = rules_->social_mode;
    r.faces = rules_->social_faces;
    if (r.mode == ButtonMode::Press) {
        r.caps = rules_->social_caps & kCapAll &
                 ~(kCapEvents | kCapUi | kCapWeb | kCapPackets | kCapExtend | kCapLogin | kCapNet);
        r.press_commands = rules_->social_press_commands;
    } else if (r.mode == ButtonMode::Full) {
        r.caps = rules_->social_caps & kCapAll & ~(kCapPackets | kCapExtend | kCapLogin | kCapNet);
    }
    return r;
}

} // namespace hailstone::plugin
