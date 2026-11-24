#include "game/chat_text.h"

#include <algorithm>
#include <string>

namespace hailstone::game {

namespace {

constexpr std::string_view kPercentEntity = "&PCT;";
constexpr std::string_view kAmpersandEntity = "&AMP;";
constexpr std::string_view kQuotedPercentEntity = "&AMP;PCT;";
constexpr std::string_view kQuotedAmpersandEntity = "&AMP;AMP;";

// The C locale's ctype for one byte (the original passes the byte zero-extended).
bool c_alpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool c_upper(unsigned char c) { return c >= 'A' && c <= 'Z'; }
char c_toupper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 0x20) : c; }
char c_tolower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c; }

// Utf8IsSpace @0x8095e0 (iswspace of the decoded character) for the ASCII range.
bool utf8_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

// _stricmp.
bool stricmp_eq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (c_tolower(a[i]) != c_tolower(b[i])) return false;
    return true;
}

// The length of the C string at `s` (an std::string may hold a NUL a garbler never writes; stay with the C view).
std::size_t c_len(const std::string& s) {
    std::size_t n = s.find('\0');
    return n == std::string::npos ? s.size() : n;
}

// GetNextCmdArg @0x477f60 with a 0x1000-byte buffer: skips white space; a word starting with ' or " runs to the
// matching quote (dropped); otherwise to white space. Item links' 0x12 end a word in either case. White space
// after the word is skipped. Returns the position after it.
std::size_t next_cmd_arg(const std::string& text, std::size_t pos, std::string& word) {
    word.clear();
    std::size_t n = c_len(text);
    while (pos < n && utf8_space(text[pos])) ++pos;
    int quote = 0;
    if (pos < n && text[pos] == '\'') quote = 1;
    else if (pos < n && text[pos] == '"') quote = 2;
    if (quote) ++pos;
    for (;;) {
        if (pos >= n) break;
        char c = text[pos];
        if (quote == 1 && c == '\'') { ++pos; break; }
        if (quote == 2 && c == '"') { ++pos; break; }
        if (quote == 0 && utf8_space(c)) { ++pos; break; }
        if (c == '\x12' || 0x1000 - 1 <= static_cast<int>(word.size()) + 1) break;
        word.push_back(c);
        ++pos;
    }
    while (pos < n && utf8_space(text[pos])) ++pos;
    return pos;
}

// strcat of a C string.
void append(std::string& dst, const std::string& word) { dst.append(word, 0, c_len(word)); }

// The broken speech of a short word (shorter than 8), compared without case in the original's order.
void broken_speech(std::string& w, std::int32_t roll, std::uint8_t skill) {
    struct Swap {
        const char* from;
        const char* to;
    };
    static constexpr Swap kAlways[] = {{"I", "me"},      {"I'", "me"},  {"my", "me"},   {"let's", "we"},  {"we'll", "we"},
                                       {"they'll", "them"}, {"it's", "it"}, {"its", "it"},  {"went", "did go"}, {"a", ""},
                                       {"am", ""},       {"an", ""},    {"to", ""},     {"or", ""},       {"do", ""},
                                       {"the", ""},      {"and", ""},   {"are", ""},    {"was", ""},      {"has", ""},
                                       {"were", ""},     {"that", ""}};
    static constexpr Swap kWorse[] = {{"of", ""}, {"is", ""}, {"his", "him"}, {"best", "more better"}};
    for (const Swap& s : kAlways)
        if (stricmp_eq(w, s.from)) {
            w = s.to;
            return;
        }
    if (roll - static_cast<std::int32_t>(skill) <= 20) return;
    for (const Swap& s : kWorse)
        if (stricmp_eq(w, s.from)) {
            w = s.to;
            return;
        }
}

} // namespace

std::string encode_chat_percents(std::string_view text, std::size_t max_bytes) {
    std::string out;
    out.reserve(std::min(text.size(), max_bytes));
    auto link_start = std::string::npos;
    for (std::size_t i = 0; i < text.size();) {
        const auto suffix = text.substr(i);
        std::size_t consumed = 1;
        auto bytes = suffix.substr(0, consumed);
        if (text[i] == '%') {
            bytes = kPercentEntity;
        } else if (suffix.starts_with(kPercentEntity)) {
            bytes = kQuotedPercentEntity;
            consumed = kPercentEntity.size();
        } else if (suffix.starts_with(kAmpersandEntity)) {
            bytes = kQuotedAmpersandEntity;
            consumed = kAmpersandEntity.size();
        } else {
            // Keep a UTF-8 character together as the escaped text reaches its byte limit.
            while (consumed < suffix.size() && (static_cast<unsigned char>(suffix[consumed]) & 0xc0) == 0x80)
                ++consumed;
            bytes = suffix.substr(0, consumed);
        }
        if (bytes.size() > max_bytes - out.size()) {
            if (link_start != std::string::npos) out.resize(link_start);
            return out;
        }
        if (text[i] == '\x12') link_start = link_start == std::string::npos ? out.size() : std::string::npos;
        out += bytes;
        i += consumed;
    }
    return out;
}

std::string decode_chat_percents(std::string_view text) {
    if (text.find('&') == std::string_view::npos) return std::string(text);
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto suffix = text.substr(i);
        if (suffix.starts_with(kPercentEntity)) {
            out += '%';
            i += kPercentEntity.size();
        } else if (suffix.starts_with(kQuotedPercentEntity)) {
            out += kPercentEntity;
            i += kQuotedPercentEntity.size();
        } else if (suffix.starts_with(kQuotedAmpersandEntity)) {
            out += kAmpersandEntity;
            i += kQuotedAmpersandEntity.size();
        } else {
            out += text[i++];
        }
    }
    return out;
}

std::int32_t SeededChatRandom::random() {
    // xorshift64*: a 30-bit value as the game's Random gives.
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return static_cast<std::int32_t>((state_ * 0x2545f4914f6cdd1dull) >> 34);
}

bool is_vowel(char c) {
    switch (c) {
    case 'a': case 'A': case 'e': case 'E': case 'i': case 'I': case 'o': case 'O': case 'u': case 'U': return true;
    default: return false;
    }
}

std::string garble_by_language_skill(std::string_view text, std::uint8_t skill, bool garbling_on, ChatRandom& rng,
                                     bool* changed) {
    if (changed) *changed = false;
    std::string src(text.substr(0, text.find('\0')));
    if (skill > 99 || !garbling_on) return src;
    const int length = static_cast<int>(src.size());
    std::string dst(src.size(), '\0');
    bool garbled = false, in_link = false;
    auto at = [&](int i) { return i < length ? src[static_cast<std::size_t>(i)] : '\0'; };
    for (int i = 0; i < length; ++i) {
        char c = src[static_cast<std::size_t>(i)];
        if (c == '\x12') in_link = !in_link;
        auto& out = dst[static_cast<std::size_t>(i)];
        if (!c_alpha(static_cast<unsigned char>(c)) || in_link || rng.percent_roll() <= 50) {
            out = c;
            continue;
        }
        auto r = static_cast<std::uint8_t>(rng.percent_roll());
        if (r <= 5 || static_cast<int>(r) <= static_cast<int>(skill) + 10) {
            out = c;
            continue;
        }
        garbled = true;
        if (r > 98 && i > 0 && dst[static_cast<std::size_t>(i - 1)] != ' ' && at(i + 1) != ' ' && i != length - 1) {
            out = ' ';
        } else if (static_cast<int>(r) - static_cast<int>(skill) > 30) {
            out = static_cast<char>(rng.random_between(0, 25) + 0x60);
        } else if (is_vowel(c)) {
            out = rng.random_vowel();
        } else {
            do out = static_cast<char>(rng.random_between(0, 24) + 0x61);
            while (is_vowel(out));
        }
    }
    if (changed) *changed = garbled;
    return dst;
}

const std::array<const char*, 20>& profanity_words() {
    static constexpr std::array<const char*, 20> kWords = {
        "fuck", "shit",  "piss", "cock",  "penis",  "pussy",   "fag",     "cunt", "kunt",  "nigger",
        "niger", "asshole", "tits", "nigga", "jesus!", "christ!", "damn", "dong", "bitch", "dickhead"};
    return kWords;
}

bool is_bad_word(std::string_view word) {
    // StrStrNoCase @0x8084a0 (_toupper both sides) against each listed word.
    for (const char* bad : profanity_words()) {
        std::string_view b(bad);
        if (b.empty()) return true;
        for (std::size_t i = 0; i + b.size() <= word.size(); ++i) {
            bool match = true;
            for (std::size_t k = 0; k < b.size() && match; ++k) match = c_toupper(word[i + k]) == c_toupper(b[k]);
            if (match) return true;
        }
    }
    return false;
}

std::string censor_bad_words(std::string_view text) {
    std::string out(text);
    auto space = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
    std::size_t i = 0;
    while (i < out.size()) {
        while (i < out.size() && space(out[i])) ++i;
        std::size_t start = i;
        while (i < out.size() && !space(out[i])) ++i;
        if (i > start && is_bad_word(std::string_view(out).substr(start, i - start))) std::fill(out.begin() + static_cast<std::ptrdiff_t>(start), out.begin() + static_cast<std::ptrdiff_t>(i), '*');
    }
    return out;
}

std::string filter_profanity_and_slur(std::string_view text, std::uint8_t skill, std::int32_t filter_mode,
                                      ChatRandom& rng) {
    std::string src(text.substr(0, text.find('\0')));
    bool capital = src.empty() || c_upper(static_cast<unsigned char>(src[0]));
    if (skill > 99 && filter_mode == 0) return src;
    std::string dst;
    std::string buf = src.substr(0, std::min<std::size_t>(src.size(), 0xfff));
    std::size_t pos = 0;
    std::string word, token;
    while (pos < c_len(buf)) {
        if (buf[pos] == '\x12') {
            std::size_t k = 1;
            while (pos + k < c_len(buf) && buf[pos + k] != '\x12') ++k;
            if (pos + k < c_len(buf)) {
                dst.append(buf, pos, k + 1); // the link with both 0x12
                dst.push_back(' ');
                pos += k + 1;
            } else {
                buf[pos] = 'Q'; // an unclosed link mark: read as a letter on the next pass
            }
            continue;
        }
        std::size_t next = next_cmd_arg(buf, pos, token);
        if (token.empty()) {
            // StrNCpyTerminated(word, rest, 0x1000): the rest of the text is the word.
            word = buf.substr(next, std::min<std::size_t>(c_len(buf) - std::min(next, c_len(buf)), 0xfff));
            pos = next + word.size();
        } else {
            word = token.substr(0, 0xfff);
            pos = next;
        }
        if (filter_mode == 1 && !word.empty() && is_bad_word(word)) {
            for (char& c : word) c = static_cast<char>(rng.random() % 26 + 0x60);
            append(dst, word);
            if (dst.back() != ' ') dst.push_back(' ');
            continue;
        }
        auto roll = static_cast<std::uint8_t>(rng.percent_roll());
        if (skill < roll && word.size() < 8) broken_speech(word, roll, skill);
        const std::size_t n = c_len(word);
        for (std::size_t i = 0; i < n; ++i) {
            if (!c_alpha(static_cast<unsigned char>(word[i])) || rng.percent_roll() <= 40) continue;
            auto r = static_cast<std::uint8_t>(rng.percent_roll());
            if (r <= 5 || static_cast<std::uint32_t>(r) <= static_cast<std::uint32_t>(skill) + 10) continue;
            char next_char = i + 1 < word.size() ? word[i + 1] : '\0';
            if (r < 99 || i < 1 || word[i - 1] == ' ' || next_char == ' ') {
                if (static_cast<int>(r) - static_cast<int>(skill) < 31) {
                    if (!is_vowel(word[i])) {
                        do word[i] = static_cast<char>(rng.random() % 25 + 'a');
                        while (is_vowel(word[i]));
                    } else {
                        word[i] = rng.random_vowel();
                    }
                } else {
                    word[i] = static_cast<char>(rng.random() % 26 + 0x60);
                }
            } else {
                word[i] = ' ';
            }
        }
        if (c_len(word) != 0) {
            append(dst, word);
            if (dst.back() != ' ') dst.push_back(' ');
        }
    }
    // The text's last character decides the line's: a space is kept, else the joining space goes.
    if (src.size() >= 2 && !dst.empty()) {
        dst.back() = src.back() == ' ' ? ' ' : '\0';
        if (dst.back() == '\0') dst.pop_back();
        if (capital && !dst.empty() && dst[0] > '`' && dst[0] < '{') dst[0] = static_cast<char>(dst[0] - 0x20);
    }
    return dst;
}

// ---------------------------------------------------------------- chat filters

namespace {

struct FilterDef {
    std::int32_t base, options, fallback;
    const char* key;
};
// The definitions @0xa1e3c0 (mode base, option count, default, ini key).
constexpr FilterDef kFilterDefs[kChatFilterCount] = {
    {0, 2, 1, "Channel00"},     {0, 2, 1, "Channel01"},      {0, 2, 1, "Channel02"},       {0, 2, 1, "Channel03"},
    {0, 2, 1, "Channel04"},     {0, 2, 1, "Channel05"},      {0, 2, 0, "BadWord"},         {2, 3, 0, "PCSpells"},
    {12, 2, 0, "NPCSpells"},    {8, 4, 0, "BardSongs"},      {5, 3, 0, "CriticalSpells"},  {5, 3, 0, "CriticalMelee"},
    {5, 3, 0, "SpellDamage"},   {0, 2, 1, "Channel06"},      {0, 2, 1, "Channel07"},       {0, 2, 1, "Channel08"},
    {0, 2, 1, "Channel09"},     {12, 2, 0, "HideDamageShield"}, {8, 4, 0, "DotDamage"},    {12, 2, 0, "PetAttacks"},
    {12, 2, 0, "PetMisses"},    {12, 2, 0, "FocusEffects"},  {12, 2, 0, "PetSpells"},      {5, 3, 0, "HealOverTimeSpells"},
    {12, 2, 0, "ItemSpeech"},   {12, 2, 0, "Strikethrough"}, {12, 2, 0, "Stun"},           {12, 2, 0, "BardSongsOnPets"},
    {12, 2, 0, "SwarmPetDeath"}, {12, 2, 0, "FellowshipChat"}, {0, 2, 1, "MercenaryMessages"}, {0, 2, 0, "Spam"},
    {5, 3, 0, "Achievements"},  {0, 2, 1, "PvPMessages"}};
// The mode table @0xa1e384.
constexpr std::int32_t kModes[15] = {1, 0, 2, 1, 4, 2, 3, 1, 2, 3, 4, 1, 0, 1, 0};

} // namespace

ChatFilters::ChatFilters() {
    for (int f = 0; f < kChatFilterCount; ++f) settings[static_cast<std::size_t>(f)] = kFilterDefs[f].fallback;
}

void ChatFilters::set(int filter, std::int32_t value) {
    if (filter < 0 || filter >= kChatFilterCount) return;
    const FilterDef& d = kFilterDefs[filter];
    settings[static_cast<std::size_t>(filter)] = value >= 0 && value < d.options ? value : d.fallback;
}

std::int32_t ChatFilters::mode(int filter) const {
    if (filter < 0 || filter >= kChatFilterCount) return 0;
    std::int32_t at = kFilterDefs[filter].base + settings[static_cast<std::size_t>(filter)];
    return at >= 0 && at < 15 ? kModes[at] : 0;
}

int ChatFilters::options(int filter) {
    return filter >= 0 && filter < kChatFilterCount ? kFilterDefs[filter].options : 0;
}

std::int32_t ChatFilters::mode_of(int filter, std::int32_t setting) {
    if (filter < 0 || filter >= kChatFilterCount) return 0;
    std::int32_t at = kFilterDefs[filter].base + setting;
    return at >= 0 && at < 15 ? kModes[at] : 0;
}

const char* ChatFilters::ini_key(int filter) {
    return filter >= 0 && filter < kChatFilterCount ? kFilterDefs[filter].key : "";
}

bool chat_line_shown(const ChatLineFacts& f, const ChatFilters& filters) {
    if (f.spam && filters.mode(kFilterSpam) == 1) return false;
    if (f.channel < 6 && filters.mode(static_cast<int>(f.channel)) == 1) return false;
    if ((f.channel == 10 || f.channel == 11) && (!f.listener_gm || f.gm_chat_hidden)) return false;
    if ((f.channel == 17 || f.channel == 0) && (filters.mode(0) == 1 || !f.guild_permission)) return false;
    if (f.channel != 6 && f.channel != 14 && !f.sender_gm && f.sender_ignored) return false;
    return true;
}

bool chat_line_filtered(std::uint32_t channel, bool listener_gm, bool sender_gm) {
    return channel != 6 && channel != 20 && channel != 14 && !listener_gm && !sender_gm;
}

std::uint32_t language_prefix_eqstr(std::uint32_t channel, std::uint8_t listener_skill, bool sender_gm) {
    return listener_skill > 24 || channel == 14 || sender_gm ? 1411 : 1412;
}

} // namespace hailstone::game
