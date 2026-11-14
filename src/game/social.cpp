#include "game/social.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <string_view>

#include "core/msvc_atoi.h"
#include "game/eq_names.h"
#include "game/eq_strings.h"
#include "game/slash_commands.h"
#include "game/zone_names.h"
#include "net/social_packets.h"

namespace hailstone::game {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool iprefix(std::string_view text, std::string_view prefix) {
    return !prefix.empty() && prefix.size() <= text.size() && iequals(text.substr(0, prefix.size()), prefix);
}

bool all_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

std::string str(const EqStrings& s, std::uint32_t id, std::vector<std::string> args = {}) { return s.format(id, args); }

} // namespace

// ---------------------------------------------------------------- /who

WhoParse parse_who_args(const std::string& args, const WhoContext& ctx, const EqStrings& strings) {
    WhoParse out;
    std::string_view rest = args;
    bool class_set = false, race_set = false, low_set = false, high_set = false;
    std::int32_t low = -1, high = -1;
    for (int word_no = 0; word_no < 6; ++word_no) {
        std::string w = next_cmd_arg(rest);
        if (w.empty()) break;
        if (iequals(w, str(strings, 3118))) { // corpse
            out.type = 2;
        } else if (iequals(w, str(strings, 13205)) || iequals(w, "all")) {
            out.type = 3;
        } else if (iequals(w, str(strings, 673)) || iequals(w, "ripoff")) {
            out.type = 4;
        } else if (iequals(w, str(strings, 3119)) || iequals(w, "GM")) {
            out.gm = 1;
        } else if (iequals(w, str(strings, 3120))) { // NONGM
            out.gm = 0;
        } else if (iequals(w, str(strings, 3112)) || iequals(w, "friends")) {
            out.guild = static_cast<std::uint32_t>(-2);
            out.friends = true;
        } else if (iequals(w, str(strings, 3121))) { // LFG
            out.guild = static_cast<std::uint32_t>(-3);
        } else if (iequals(w, "Trader") && out.type != 3) {
            out.guild = static_cast<std::uint32_t>(-4);
        } else if (iequals(w, "Buyer") && out.type != 3) {
            out.guild = static_cast<std::uint32_t>(-5);
        } else if (std::string g = str(strings, 13122); w.size() > 5 && iprefix(w, g) &&
                                                         std::isdigit(static_cast<unsigned char>(w[5]))) {
            out.guild = static_cast<std::uint32_t>(core::msvc_atoi(std::string_view(w).substr(5)));
        } else if (iequals(w, str(strings, 13122))) { // guild
            if (ctx.own_guild_id == 0xffffffff) {
                out.error = 12336; // You aren't in a guild, so can't ask for guild info in who list.
                return out;
            }
            out.guild = ctx.own_guild_id;
        } else {
            // A class name prefix (1..35), then a race name prefix, unless the
            // word is a zone's short name.
            bool matched = false;
            if (!class_set) {
                for (std::uint32_t c = 1; c < 0x24 && !matched; ++c) {
                    std::string n = class_name(c);
                    if (n.rfind("Class ", 0) == 0) continue;
                    if (iprefix(n, w)) {
                        out.class_ = c;
                        class_set = matched = true;
                    }
                }
            }
            if (!matched && !race_set && !all_digits(w)) {
                for (std::uint32_t r = 0; r < 0x2dd && !matched; ++r) {
                    std::string n = race_name(r);
                    if (n.rfind("Race ", 0) == 0 || !iprefix(n, w)) continue;
                    if (ctx.is_zone_short_name && ctx.is_zone_short_name(w)) break;
                    out.race = r == 0x1a ? 0x14a : r; // the original's froglok fix-up
                    race_set = matched = true;
                }
            }
            if (matched) continue;
            if (all_digits(w) && !low_set) {
                low = core::msvc_atoi(w);
                low_set = true;
            } else if (all_digits(w) && !high_set) {
                high = core::msvc_atoi(w);
                high_set = true;
            } else if (auto gid = ctx.guild_by_name ? ctx.guild_by_name(w) : std::nullopt) {
                out.guild = *gid;
                out.name = w;
            } else if (out.name.empty()) {
                out.name = w;
            }
        }
    }
    // The level range: one number gives that level; two are ordered.
    if (low > 0 || high > 0) {
        std::int32_t a = low > 0 ? low : high, b = high > 0 ? high : low;
        out.level_low = static_cast<std::uint32_t>(std::min(a, b));
        out.level_high = static_cast<std::uint32_t>(std::max(a, b));
    }
    return out;
}

namespace {

constexpr std::uint32_t kNoClassName = 0x5cc;

// The string table keeps classes 1..15 in its own order: by id, but Shadow Knight (5) ahead of Ranger (4).
std::uint32_t class_string_slot(std::int32_t class_id) {
    if (class_id == 4) return 4;
    if (class_id == 5) return 3;
    return static_cast<std::uint32_t>(class_id - 1);
}

// Which title a level past 50 earns: 51..54, 55..59, 60..64, 65..69, 70..74, 75 up.
std::size_t title_bracket(std::int32_t level) {
    if (level >= 75) return 5;
    if (level >= 70) return 4;
    if (level >= 65) return 3;
    if (level >= 60) return 2;
    if (level >= 55) return 1;
    return 0;
}

// Classes 1..15: the name, then its first three titles, four ids apart per slot; the later three titles each in a
// run of their own, one id per slot.
std::uint32_t classic_class_eqstr(std::uint32_t slot, bool title, std::int32_t level) {
    const std::uint32_t name = 0x5de + 4 * slot;
    if (!title) return name;
    const std::size_t bracket = title_bracket(level);
    if (bracket < 3) return name + 1 + static_cast<std::uint32_t>(bracket);
    static constexpr std::uint32_t kLaterRuns[3] = {0x167b, 0x171a, 0x1f64};
    return kLaterRuns[bracket - 3] + slot;
}

} // namespace

std::uint32_t kunark_class_eqstr(std::int32_t class_id, std::int32_t level, bool base_name) {
    const bool title = level >= 51 && !base_name;
    if (class_id >= 1 && class_id <= 15) return classic_class_eqstr(class_string_slot(class_id), title, level);
    if (class_id == 16) { // Berserker, added later: its strings sit apart
        static constexpr std::uint32_t kBerserkerTitles[6] = {0x16a1, 0x16a2, 0x16a3, 0x16a4, 0x1719, 0x1f73};
        return title ? kBerserkerTitles[title_bracket(level)] : 0x169f;
    }
    if (title) return kNoClassName;
    // The GM classes 20..35 and the banker and merchant kinds 40, 41 have a name only.
    if (class_id >= 20 && class_id <= 34) return 0x5cf + class_string_slot(class_id - 19);
    if (class_id == 35) return 0x16a0;
    if (class_id == 40) return 0x5ce;
    if (class_id == 41) return 0x5cd;
    return kNoClassName;
}

std::string who_class_text(const EqStrings& strings, std::int32_t class_id, std::int32_t level) {
    std::string out = str(strings, kunark_class_eqstr(class_id, level, false));
    if (level > 50) out += " (" + str(strings, kunark_class_eqstr(class_id, level, true)) + ")";
    return out;
}

std::string trim_name(std::string_view name) {
    std::string out;
    for (char c : name) {
        if (c == '\0') break;
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '#') continue;
        out.push_back(c == '_' ? ' ' : c);
    }
    return out;
}

std::optional<std::size_t> player_from_partial_name(std::string_view word, const std::vector<std::string>& names,
                                                    std::optional<std::size_t> exclude) {
    const std::string wanted = trim_name(word);
    if (wanted.empty()) return std::nullopt;
    std::optional<std::size_t> best;
    std::size_t best_length = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::string_view raw = names[i];
        raw = raw.substr(0, raw.find('\0'));
        // __strnicmp(trimmed, wanted, length): the trimmed name must be at least as long.
        if (!iprefix(trim_name(raw), wanted)) continue;
        if (best && raw.size() >= best_length) continue;
        if (exclude && *exclude == i) continue;
        best = i;
        best_length = raw.size();
    }
    return best;
}

std::vector<std::string> format_who_response(const net::WhoResponse& r, const EqStrings& strings, bool multiclass_masks) {
    std::vector<std::string> lines;
    if (r.title_string != 0xffffffff) {
        lines.push_back(str(strings, r.title_string));
        lines.push_back(r.line);
    }
    for (const auto& e : r.entries) {
        if (e.format == 5026 || e.format == 5027) { // ZONE: %1 is UP / isn't ready yet
            lines.push_back(str(strings, e.format, {zone_short_name(e.zone_id)}));
            continue;
        }
        std::string offline, trade, afk, linkdead, rip;
        if (e.flags & 0x400) {
            offline = str(strings, 767);
            if ((e.flags & 0x30) == 0) offline += str(strings, 778).substr(0, 4);
        } else {
            if (e.flags & 1) afk = str(strings, 12311);
            if (e.flags & 8) linkdead = str(strings, 12313);
        }
        if (e.flags & 0x10) trade = str(strings, 12315);
        else if (e.flags & 0x20) trade = str(strings, 6056);
        if (e.flags & 0x40) rip = str(strings, 12958);
        std::string rank = e.rank_string != 0xffffffff ? std::to_string(e.rank_string) : std::string();
        std::string prefix = str(strings, 6357, {offline, trade, afk, linkdead, rank, rip});
        std::string extra = e.extra_string != 0xffffffff ? str(strings, e.extra_string) : std::string();
        if (e.format == 5022 || e.format == 5025) {
            std::string pid;
            if (e.pid_string == 5003) pid = str(strings, 5003, {std::to_string(e.unknown1), e.account});
            else if (e.pid_string == 5004) pid = str(strings, 5004, {e.account});
            std::string zone;
            if (e.zone_string != 0xffffffff)
                zone = str(strings, e.zone_string, {(e.zone_id >> 15) & 1 ? e.zone_name : zone_short_name(e.zone_id)});
            // 0x44d420: the class name, with its level title above 50 (who_class_text). NMS class masks
            // list their classes instead.
            std::string klass;
            if (multiclass_masks && e.class_ != 0) {
                std::vector<std::uint32_t> ids;
                for (std::uint32_t c = 1; c <= 16; ++c)
                    if (e.class_ & (1u << (c - 1))) ids.push_back(c);
                klass = class_list_name(ids, ids.size() > 1);
            } else if (e.class_ != 0) {
                klass = who_class_text(strings, static_cast<std::int32_t>(e.class_), static_cast<std::int32_t>(e.level));
            }
            std::string race = e.race ? race_name(e.race) : std::string();
            lines.push_back(str(strings, e.format,
                                {prefix, std::to_string(e.level), klass, e.name, race, e.guild, zone, pid, extra}));
        } else if (e.format == 5023) {
            lines.push_back(str(strings, e.format, {prefix, e.name, e.guild, extra}));
        } else if (e.format == 5024) {
            lines.push_back(str(strings, e.format, {prefix, e.name, extra}));
        } else {
            lines.push_back(str(strings, e.format, {prefix, e.name, e.guild, extra}));
        }
    }
    switch (r.footer_string) {
    case 5028: case 5034: case 5035: case 5036:
    case 5029: case 5033:
        lines.push_back(str(strings, r.footer_string, {std::to_string(r.footer_count)}));
        break;
    case 0xffffffff:
        break;
    default:
        lines.push_back(str(strings, r.footer_string, {std::to_string(r.footer_count)}));
        break;
    }
    return lines;
}

// ---------------------------------------------------------------- /random

RandomRange parse_random_args(const std::string& args) {
    // Cmd_random @0x4df6c0: the first word and the rest of the line (each at
    // most 6 characters), through atoi; EQEmu turns 0..0 into 0..100.
    RandomRange out;
    std::string_view rest = args;
    std::string a = next_cmd_arg(rest);
    std::string b(rest);
    if (b.size() > 6 || a.size() > 6) {
        out.error = 13060; // Please use smaller numbers.
        return out;
    }
    long x = std::atol(b.c_str()), y = std::atol(a.c_str());
    if (x < 0 || y < 0) {
        out.error = 13061; // Please use positive numbers.
        return out;
    }
    if (std::max(x, y) > 32000) {
        out.error = 13062; // Please use numbers <= 32000.
        return out;
    }
    out.low = static_cast<std::uint32_t>(std::min(x, y));
    out.high = static_cast<std::uint32_t>(std::max(x, y));
    return out;
}

// ---------------------------------------------------------------- /played, /time

std::string format_duration(const EqStrings& strings, std::uint64_t seconds) {
    std::uint64_t days = seconds / 86400, hours = seconds / 3600 % 24, minutes = seconds / 60 % 60, secs = seconds % 60;
    std::vector<std::string> parts;
    auto add = [&](std::uint64_t n, std::uint32_t one, std::uint32_t many) {
        if (n) parts.push_back(strings.format(n == 1 ? one : many, std::vector<std::string>{std::to_string(n)}));
    };
    add(days, 12969, 12970);
    add(hours, 12971, 12972);
    add(minutes, 12973, 12974);
    add(secs, 6686, 6687);
    switch (parts.size()) {
    case 0: return strings.format(6688, std::vector<std::string>{strings.format(6687, std::vector<std::string>{"0"})});
    case 1: return strings.format(6688, parts);
    case 2: return strings.format(6689, parts);
    case 3: return strings.format(6690, parts);
    default: return strings.format(6691, parts);
    }
}

namespace {
constexpr std::array<const char*, 7> kWeekdays = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
constexpr std::array<const char*, 12> kMonths = {"January", "February", "March",     "April",   "May",      "June",
                                                 "July",    "August",   "September", "October", "November", "December"};
int day_of_week(int y, int m, int d) {
    using namespace std::chrono;
    auto days = sys_days{year{y} / month{static_cast<unsigned>(m)} / day{static_cast<unsigned>(d)}};
    return static_cast<int>(std::chrono::weekday(days).c_encoding());
}
} // namespace

std::string long_date(int y, int m, int d) {
    m = std::clamp(m, 1, 12);
    d = std::clamp(d, 1, 31);
    return std::format("{}, {} {}, {}", kWeekdays[static_cast<std::size_t>(day_of_week(y, m, d))], kMonths[static_cast<std::size_t>(m - 1)], d, y);
}

std::string long_date_time(int y, int m, int d, int hh, int mm, int ss) {
    return std::format("{} {:02}:{:02}:{:02}", long_date(y, m, d), hh, mm, ss);
}

std::string format_game_time(const EqStrings& strings, int y, int m, int d, int hour) {
    int h24 = ((hour - 1) % 24 + 24) % 24;
    int h12 = h24 % 12 == 0 ? 12 : h24 % 12;
    // EQWorldData::CurrentGameTime @0x7dc130 gives _wcsftime the weekday (day - 1) % 7, not the real calendar's:
    // every game month starts on a Sunday.
    m = std::clamp(m, 1, 12);
    d = std::clamp(d, 1, 31);
    std::string date = std::format("{}, {} {}, {}", kWeekdays[static_cast<std::size_t>((d - 1) % 7)],
                                   kMonths[static_cast<std::size_t>(m - 1)], d, y);
    std::string text = std::format("{} - {} {}", date, h12, h24 < 12 ? "AM" : "PM");
    return strings.format(6706, std::vector<std::string>{text});
}

// ---------------------------------------------------------------- chat

ChannelFormat channel_format(std::uint32_t channel) {
    switch (channel) {
    case 0: case 0x11: return {1415, true};
    case 2: return {1414, true};
    case 3: return {1420, true};
    case 4: return {1421, true};
    case 5: return {1422, false};
    case 6: return {1413, false};
    case 7: case 0xe: return {1416, true};
    case 8: return {1410, true};
    case 10: return {1423, false};
    case 0xb: case 0x12: return {1425, false};
    case 0xf: return {5112, true};
    case 0x15: return {4585, true};
    case 0x16: return {1036, false};
    default: return {1410, true};
    }
}

std::uint32_t own_chat_echo(std::uint32_t channel) {
    switch (channel) {
    case 0: return 12263;
    case 2: return 12346;
    case 3: return 12348;
    case 4: return 12350;
    case 5: return 12352;
    case 8: return 12344;
    case 15: return 5109;
    case 21: return 4574;
    default: return 0;
    }
}

// ---------------------------------------------------------------- social emotes

namespace {

struct SocialRow {
    int id, animation;
    std::uint32_t ou;
    const char* ou_slots;
    std::uint32_t ot;
    const char* ot_slots;
    std::uint32_t eu;
    const char* eu_slots;
    std::uint32_t et;
    const char* et_slots;
};
constexpr SocialRow kSocials[] = {
#define HAILSTONE_SOCIAL(id, anim, ou, ous, ot, ots, eu, eus, et, ets) {id, anim, ou, ous, ot, ots, eu, eus, et, ets},
#define HAILSTONE_SOCIAL_COMMAND(handler, id, standing)
#include "game/socials.inc"
#undef HAILSTONE_SOCIAL
#undef HAILSTONE_SOCIAL_COMMAND
};
struct SocialCommand {
    const char* handler;
    int id;
    bool standing;
};
constexpr SocialCommand kSocialCommands[] = {
#define HAILSTONE_SOCIAL(id, anim, ou, ous, ot, ots, eu, eus, et, ets)
#define HAILSTONE_SOCIAL_COMMAND(handler, id, standing) {handler, id, standing != 0},
#include "game/socials.inc"
#undef HAILSTONE_SOCIAL
#undef HAILSTONE_SOCIAL_COMMAND
};

// he / him / his for a gender (send_social's prologue: 12209/12297/12301,
// 12210/12298/12299, 12211/12211/12300).
std::array<std::uint32_t, 3> pronouns(int gender) {
    if (gender == 0) return {12209, 12297, 12301};
    if (gender == 1) return {12210, 12298, 12299};
    return {12211, 12211, 12300};
}

std::string with_period(std::string s) {
    if (!s.empty() && std::isalnum(static_cast<unsigned char>(s.back()))) s += '.';
    return s;
}

} // namespace

const SocialEmote* social_for_handler(std::string_view handler) {
    static std::vector<SocialEmote> table = [] {
        std::vector<SocialEmote> out;
        for (const auto& c : kSocialCommands) {
            for (const auto& r : kSocials) {
                if (r.id != c.id) continue;
                SocialEmote e;
                e.id = r.id;
                e.animation = r.animation;
                e.others_untargeted = r.ou;
                e.others_untargeted_slots = r.ou_slots;
                e.others_targeted = r.ot;
                e.others_targeted_slots = r.ot_slots;
                e.echo_untargeted = r.eu;
                e.echo_untargeted_slots = r.eu_slots;
                e.echo_targeted = r.et;
                e.echo_targeted_slots = r.et_slots;
                e.needs_standing = c.standing;
                out.push_back(e);
            }
        }
        return out;
    }();
    for (std::size_t i = 0; i < std::size(kSocialCommands); ++i)
        if (handler == kSocialCommands[i].handler)
            for (const auto& e : table)
                if (e.id == kSocialCommands[i].id) return &e;
    return nullptr;
}

SocialText build_social(const EqStrings& strings, const SocialEmote& e, const std::string* target_name, int own_gender,
                        int target_gender, const std::string& extra) {
    auto own = pronouns(own_gender), tgt = pronouns(target_gender);
    auto fill = [&](std::uint32_t id, const char* slots, bool echo) -> std::string {
        if (!id) return {};
        std::vector<std::string> args;
        for (const char* c = slots; *c; ++c) {
            switch (*c) {
            case 'n': args.push_back(echo ? strings.format(12302, {}) : std::string()); break; // "You" / the server adds the name
            case 't': args.push_back(target_name ? *target_name : std::string()); break;
            case 's': args.push_back(strings.format(echo ? 12303 : own[0], {})); break;
            case 'o': args.push_back(strings.format(echo ? 12303 : own[1], {})); break;
            case 'p': args.push_back(strings.format(echo ? 12304 : own[2], {})); break;
            case 'S': args.push_back(strings.format(tgt[0], {})); break;
            case 'O': args.push_back(strings.format(tgt[1], {})); break;
            case 'P': args.push_back(strings.format(tgt[2], {})); break;
            default: break;
            }
        }
        return strings.format(id, args);
    };
    SocialText out;
    // Others: the custom text, else the targeted line, else " " + the untargeted one.
    if (extra.size() > 1) {
        out.others = " " + extra + ".";
    } else if (target_name) {
        out.others = fill(e.others_targeted, e.others_targeted_slots, false);
    } else {
        std::string u = fill(e.others_untargeted, e.others_untargeted_slots, false);
        if (!u.empty()) out.others = " " + u;
    }
    out.others = with_period(out.others);
    // The player's own line (colour 0x107).
    if (extra.size() > 1) out.echo = strings.format(12305, std::vector<std::string>{extra});
    else if (target_name) out.echo = fill(e.echo_targeted, e.echo_targeted_slots, true);
    else out.echo = fill(e.echo_untargeted, e.echo_untargeted_slots, true);
    out.echo = with_period(out.echo);
    return out;
}

// ---------------------------------------------------------------- guilds

std::string GuildState::name_of(std::uint32_t id) const {
    auto it = names.find(id);
    return it == names.end() ? std::string() : it->second;
}

std::optional<std::uint32_t> GuildState::id_of(const std::string& name) const {
    for (const auto& [id, n] : names)
        if (iequals(n, name)) return id;
    return std::nullopt;
}

std::string GuildState::rank_name(std::uint32_t index) const {
    if (index < rank_names.size() && !rank_names[index].empty()) return rank_names[index];
    return guild_rank_name(index);
}

std::string guild_rank_name(std::uint32_t rank) {
    switch (rank) {
    case 1: return "Leader";
    case 2: return "Senior Officer";
    case 3: return "Officer";
    case 4: return "Senior Member";
    case 5: return "Member";
    case 6: return "Junior Member";
    case 7: return "Initiate";
    case 8: return "Recruit";
    default: return "";
    }
}

GuildAppearanceLine guild_id_appearance_line(const GuildState& g, bool own, std::uint32_t spawn_guild,
                                             std::uint32_t id, const std::string& spawn_name) {
    auto valid = [](std::uint32_t v) { return static_cast<std::int32_t>(v) >= 0; };
    if (own) {
        if (g.guild_id == id) return {};
        if (!valid(id)) {
            if (std::string old = g.name_of(g.guild_id); !old.empty()) return {12880, {old}};
            return {12879, {}};
        }
        return {12878, {g.name_of(id)}};
    }
    if (spawn_guild == id) return {};
    if (g.guild_id == id && valid(id)) return {12881, {spawn_name}};
    if (g.guild_id == spawn_guild && valid(spawn_guild)) return {12882, {spawn_name}};
    return {};
}

GuildAppearanceLine guild_rank_appearance_line(const GuildState& g, bool own, std::uint32_t spawn_guild,
                                               std::uint32_t spawn_rank, std::uint32_t rank,
                                               const std::string& spawn_name) {
    if (rank - 1 >= 8) return {};
    if (own) {
        if (g.status == rank) return {};
        return {7655, {g.rank_name(rank)}};
    }
    if (spawn_rank == rank || g.guild_id != spawn_guild || static_cast<std::int32_t>(spawn_guild) < 0) return {};
    return {7656, {spawn_name, g.rank_name(rank)}};
}

// ---------------------------------------------------------------- raids

RaidMember* RaidState::find(const std::string& name) {
    for (auto& m : members)
        if (iequals(m.name, name)) return &m;
    return nullptr;
}

bool RaidState::is_member(const std::string& name) const {
    return std::any_of(members.begin(), members.end(), [&](const RaidMember& m) { return iequals(m.name, name); });
}

bool RaidState::add(const RaidMember& m) {
    if (RaidMember* e = find(m.name)) {
        std::string note = std::move(e->note); // a member added again keeps its note (OP_RaidUpdate 0x24)
        *e = m;
        if (e->note.empty()) e->note = std::move(note);
        return false;
    }
    members.push_back(m);
    return true;
}

bool RaidState::remove(const std::string& name) {
    auto it = std::find_if(members.begin(), members.end(), [&](const RaidMember& m) { return iequals(m.name, name); });
    if (it == members.end()) return false;
    members.erase(it);
    return true;
}

void RaidState::clear() {
    ++generation;
    active = false;
    leader.clear();
    locked = false;
    motd.clear();
    members.clear();
    target.clear();
    loot_type = 1;
    looters.clear();
    id = 0;
}

// ---------------------------------------------------------------- anonymity

void add_last_tell(std::vector<std::string>& names, const std::string& name) {
    // Shifts the list down with the name at the front, then drops the first later copy (ignoring case; the 50th
    // name comes back when one went).
    auto same = [&](const std::string& n) {
        return n.size() == name.size() && std::equal(n.begin(), n.end(), name.begin(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
               });
    };
    auto it = std::find_if(names.begin(), names.end(), same);
    if (it != names.end()) names.erase(it);
    names.insert(names.begin(), name.substr(0, 0x3f));
    if (names.size() > 50) names.resize(50);
}

void add_last_tell(SocialState& s, const std::string& name) {
    add_last_tell(s.last_tells, name);
    s.last_tell_index = 0;
}

bool starts_with_word(std::string_view args, std::string_view word, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        char a = i < args.size() ? args[i] : '\0', b = i < word.size() ? word[i] : '\0';
        if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b))) return false;
        if (a == '\0') return true;
    }
    return true;
}

AnonChange anonymous_command(int flags, std::string_view args, std::string_view off, std::string_view on) {
    if (flags & kRoleplayFlag) return {flags, 13235, false}; // 0x33b3
    int f = starts_with_word(args, off, 3) ? 0 : (starts_with_word(args, on, 2) || flags == 0) ? kAnonFlag : 0;
    return {f, f ? 13236u : 13237u, true}; // 0x33b4 / 0x33b5
}

AnonChange roleplay_command(int flags, std::string_view args, std::string_view off, std::string_view on) {
    bool roleplaying = (flags & kRoleplayFlag) != 0; // 0x58b720
    bool want = !starts_with_word(args, off, 3) && (starts_with_word(args, on, 2) || !roleplaying);
    if (want && !roleplaying && (flags & kAnonFlag)) return {flags, 8873, false}; // 0x22a9
    int f = want ? flags | kRoleplayFlag : flags & ~kRoleplayFlag;
    return {f, (f & kRoleplayFlag) ? 13233u : 13234u, true}; // 0x33b1 / 0x33b2
}

AfkAction afk_keepalive_step(AfkKeepalive& k, std::int64_t now_ms, bool afk, bool manual) {
    if (afk) {
        if (manual || k.zone_afk_since_ms < 0 || k.last_input_ms <= k.zone_afk_since_ms) return AfkAction::None;
        k.zone_afk_since_ms = -1;
        k.last_sent_ms = now_ms;
        return AfkAction::Clear;
    }
    k.zone_afk_since_ms = -1;
    if (k.last_input_ms < 0) return AfkAction::None;
    if (k.last_sent_ms >= 0 && (k.last_input_ms <= k.last_sent_ms || now_ms - k.last_sent_ms < kAfkKeepaliveMs))
        return AfkAction::None;
    k.last_sent_ms = now_ms;
    return AfkAction::Keepalive;
}

} // namespace hailstone::game
