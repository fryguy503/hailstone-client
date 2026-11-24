#pragma once

// What the original does to received chat before it shows it (msg_new_text @0x4c0080):
// the chat filters that drop a line, the bad word filter and broken speech of ChatText_FilterProfanityAndSlur
// @0x502e10, the listener's language garbling of ChatText_GarbleByLanguageSkill @0x5036f0, and the language
// prefix. The random numbers come from a ChatRandom, so the recorded vectors (ChatText_*.jsonl) can
// replay the original's.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hailstone::game {

// Transport encoding for literal percent signs after chat variables have been expanded.
// Keep an escape or a link whole when it reaches the packet's text limit.
std::string encode_chat_percents(std::string_view text, std::size_t max_bytes);
std::string decode_chat_percents(std::string_view text);

// The game's random helpers the two filters call.
class ChatRandom {
public:
    virtual ~ChatRandom() = default;
    virtual std::int32_t random() = 0;                                  // Random @0x4ddad0: 0..2^30 - 1
    virtual std::int32_t percent_roll() = 0;                            // RandomPercentRoll: 1..100
    virtual char random_vowel() = 0;                                    // RandomVowel @0x502dd0: "aeiouy"[Random() % 6]
    virtual std::int32_t random_between(std::int32_t low, std::int32_t high) = 0; // RandomNumberLowHigh
};

// A ChatRandom on a seeded generator (the live client's; the original's own sequence is not reproduced).
class SeededChatRandom : public ChatRandom {
public:
    explicit SeededChatRandom(std::uint64_t seed = 0x5eed) : state_(seed ? seed : 1) {}
    std::int32_t random() override;
    std::int32_t percent_roll() override { return random() % 100 + 1; }
    char random_vowel() override { return "aeiouy"[random() % 6]; }
    std::int32_t random_between(std::int32_t low, std::int32_t high) override {
        return low + static_cast<std::int32_t>(static_cast<std::uint32_t>(random()) %
                                               (static_cast<std::uint32_t>(high - low) + 1));
    }

private:
    std::uint64_t state_;
};

// IsVowel @0x808ae0: a, e, i, o, u in either case (not y).
bool is_vowel(char c);

// ChatText_GarbleByLanguageSkill @0x5036f0: the text as a listener with `skill` in the language hears it. With a
// skill above 99, or garbling off (the dword at 0xddfd50, on unless the client turns it off), a copy. Otherwise
// each letter outside item links (0x12 toggles) is kept unless a roll is over 50 and a second roll over 5 and over
// skill + 10; then it becomes a space (second roll over 98, between letters, not the last), a random letter (the
// roll over skill + 30), a random vowel for a vowel, or a random consonant. `changed`: whether anything was garbled.
std::string garble_by_language_skill(std::string_view text, std::uint8_t skill, bool garbling_on, ChatRandom& rng,
                                     bool* changed = nullptr);

// The bad word list (ProfanityFilter_InitWordList @0x5027c0 with "word", as the static initializer at 0x9ad040
// fills 0xdcf870): matched as substrings of a word, ignoring case.
const std::array<const char*, 20>& profanity_words();
// ProfanityFilter_IsBadWord @0x502c00: the word contains a listed word.
bool is_bad_word(std::string_view word);
// ProfanityFilter_CensorText @0x4ddc80 with the bad word filter on (mode 1; off it copies): every word (split at
// white space, Utf8IsSpace) holding a listed word is overwritten with '*'.
std::string censor_bad_words(std::string_view text);

// ChatText_FilterProfanityAndSlur @0x502e10: the text word by word (GetNextCmdArg), item links kept whole. With
// the bad word filter on (`filter_mode` 1) a word containing a listed word becomes Random() % 26 + 0x60 letters.
// Otherwise, when a roll is above the speaker's skill, short words are swapped or dropped (broken speech: "I" ->
// "me", "went" -> "did go", "the" -> "", and with the roll more than 20 above the skill "of", "is", "his",
// "best"), and the letters are garbled like the listener's garbling (a first roll over 40). Words are joined by
// single spaces; the text's own trailing space is kept, a capital first letter too. A skill above 99 with the
// filter off copies the text.
std::string filter_profanity_and_slur(std::string_view text, std::uint8_t speaker_skill, std::int32_t filter_mode,
                                      ChatRandom& rng);

// ---------------------------------------------------------------- chat filters

// The 34 chat filters (eqlib eChatFilters order: 0 guild, 1 socials, 2 group, 3 shouts, 4 auction, 5 ooc,
// 6 bad words, ... 30 mercenary, 31 spam, 32 achievements, 33 PvP), as eqclient.ini [Options] stores them
// (Channel00..Channel05, BadWord, ...). ChatFilters_GetMode @0x8cf360 maps a setting to a mode through the
// definitions @0xa1e3c0 and the mode table @0xa1e384; for the filters msg_new_text asks, mode 1 hides.
inline constexpr int kChatFilterCount = 34;
inline constexpr int kFilterBadWord = 6, kFilterPcSpells = 7, kFilterSpam = 31;
struct ChatFilters {
    std::array<std::int32_t, kChatFilterCount> settings;
    ChatFilters();                                  // the definitions' defaults
    // ChatFilters_SetSetting @0x8cf390: the default when `value` is negative or not below the option count.
    void set(int filter, std::int32_t value);
    std::int32_t mode(int filter) const;            // ChatFilters_GetMode (0 for an unknown filter)
    static const char* ini_key(int filter);         // "Channel00", "BadWord", ...
    static int options(int filter);                 // its settings' count (0 for an unknown filter)
    // The setting's mode (ChatFilters_GetMode's table): 0 shown, 1 hidden (the bad word filter: on), 2 all,
    // 3 your own only, 4 your group's only.
    static std::int32_t mode_of(int filter, std::int32_t setting);
};

// The parts of a received OP_ChannelMessage msg_new_text decides on.
struct ChatLineFacts {
    std::uint32_t channel = 0;
    bool spam = false;          // ChatMessage +0x9c (the byte after the text)
    bool own = false;           // the sender is the player (a tell echo; handled before the filters)
    bool listener_gm = false;   // the local player's vtable +0xdc
    bool gm_chat_hidden = false; // 0xde0584: a GM hides channels 10 and 11
    bool guild_permission = true; // LocalPC_HasGuildPermission(0x13) (game/guild_ranks.h)
    bool sender_gm = false;     // GetSpawnByName(sender) is a GM
    bool sender_ignored = false; // CEverQuest::BeingIgnored
};
// msg_new_text's drops, in its order: the spam flag under filter 31; channels below 6 by their own filter;
// channels 10 and 11 unless the player is a GM (a GM with 0xde0584 set too); the guild channels (0 and 17) under
// filter 0 or without guild permission 0x13; then a sender the player ignores (not on channels 6 and 14, not a
// GM). True when the line is shown.
bool chat_line_shown(const ChatLineFacts& f, const ChatFilters& filters);

// Whether msg_new_text runs the bad word filter and the garbling: the client language (0xddfd54) is 0, the
// channel is not 6, 14 or 20, and neither the listener nor the sender is a GM.
bool chat_line_filtered(std::uint32_t channel, bool listener_gm, bool sender_gm);

// The language part of a chat line: nothing for language 0; " in %1," (eqstr 1411 with the language) when the
// listener's skill in it is above 24, on channel 14 or from a GM; else eqstr 1412.
std::uint32_t language_prefix_eqstr(std::uint32_t channel, std::uint8_t listener_skill, bool sender_gm);

} // namespace hailstone::game
