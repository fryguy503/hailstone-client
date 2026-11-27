// The chat channels of the colours and the Filters menu's items (game/chat_windows.h).
#include <doctest/doctest.h>

#include "game/chat_windows.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

using namespace hailstone::game;

TEST_CASE("Chat filters: the extended colours 355..408 go to the extended channels") {
    constexpr std::array<int, 53> destinations = {
        22, // 355: a colour of its own, no channel
        57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
        72, 73, 74, 75, 76, 77, 78, 79, 80, 81,
        22, // 381: faction links
        84, 85,
        22, 22, 22, 22, // 384..387: item comparison/encounter palette entries
        88, 89,
        95, 95, 95, 95, 95, 95, 95, // seven consider colours
        40, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106
    };
    for (std::size_t index = 0; index < destinations.size(); ++index) {
        const auto color = 355 + static_cast<std::int32_t>(index);
        CAPTURE(color);
        CHECK(chat_channel_of_color(color) == destinations[index]);
    }
    CHECK(chat_channel_of_color(408) == 107);
    CHECK(kChatChannelCount == 109);
    CHECK(kChatChannelCode == 108); // no colour goes there: the plugins' and scripts' own lines do
    for (int color = -300; color <= 1000; ++color) CHECK(chat_channel_of_color(color) != kChatChannelCode);
    CHECK(kChatUserColorCount == 153);
}

TEST_CASE("Chat filters: every colour up to 354 goes to the channel GetChannelFromColor gives it") {
    // The original's channel of each colour 256..350 (the table at 0x650780, as the client before the extended
    // channels had it); every other colour up to 354, and below 256, is Other (22).
    constexpr std::array<int, 95> rof2 = {
        0,  1,  2,  4,  5,  6,  7,  8,  10, 9,  24, 23, 25, 22, 11, 31, //
        22, 22, 22, 22, 22, 28, 29, 26, 27, 50, 22, 40, 39, 22, 22, 42, //
        36, 37, 22, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 30, 38, 32, //
        33, 34, 35, 0,  1,  2,  4,  5,  6,  7,  8,  12, 13, 14, 15, 16, //
        17, 18, 19, 20, 21, 22, 22, 3,  43, 46, 22, 44, 45, 41, 47, 49, //
        51, 52, 53, 22, 22, 22, 54, 22, 22, 4,  55, 22, 8,  4,  56};
    for (int color = -300; color <= 354; ++color) {
        CAPTURE(color);
        const int expected = color >= 256 && color <= 350 ? rof2[static_cast<std::size_t>(color - 256)] : kChatChannelOther;
        CHECK(chat_channel_of_color(color) == expected);
    }
    // What EQEmu sends most: Chat::NonMelee (283) is Non Melee Hits, Chat::DotDamage (325) Other.
    CHECK(chat_channel_of_color(283) == 40);
    CHECK(chat_channel_of_color(325) == kChatChannelOther);
    for (const auto color : {409, 1000, std::numeric_limits<int>::max(), std::numeric_limits<int>::min()}) {
        CAPTURE(color);
        CHECK(chat_channel_of_color(color) == kChatChannelOther);
    }
}

TEST_CASE("Chat filters: menu descriptors cover every destination and form an acyclic hierarchy") {
    std::array<int, kChatChannelCount> destinations{};
    for (const auto& item : chat_filter_items()) {
        REQUIRE(item.channel < kChatChannelCount);
        ++destinations[item.channel];
        if (item.channel >= 57) CHECK_FALSE(item.label.empty());
    }
    // Every channel once but the unused 48, and the extended ones no extended colour reaches. The plugins' and
    // scripts' channel is reached by who wrote a line, not its colour.
    for (int channel = 0; channel < kChatChannelCount; ++channel) {
        CAPTURE(channel);
        bool reached = channel < kChatChannelCountRoF2 || channel == kChatChannelCode;
        for (int color = 355; color <= 408 && !reached; ++color) reached = chat_channel_of_color(color) == channel;
        CHECK(destinations[static_cast<std::size_t>(channel)] == (channel == 48 || !reached ? 0 : 1));
    }
    // The extended items are in Hailstone's own submenus only; the original menus keep their RoF2 items, and the
    // Filters menu adds only Plugins and Scripts.
    for (const auto& item : chat_filter_items()) {
        if (item.channel == kChatChannelCode) {
            CHECK(item.menu == ChatFilterMenu::Filters);
            CHECK(item.label == "Plugins and Scripts");
            continue;
        }
        const bool original_menu = item.menu == ChatFilterMenu::Filters || item.menu == ChatFilterMenu::ChatChannel ||
                                   item.menu == ChatFilterMenu::Melee || item.menu == ChatFilterMenu::Spells;
        CHECK(original_menu == (item.channel < kChatChannelCountRoF2));
    }
    const auto groups = chat_filter_groups();
    CHECK(groups.size() == 11);
    for (const auto& group : groups) {
        CHECK_FALSE(group.label.empty());
        CHECK(group.menu != ChatFilterMenu::Filters);
        CHECK(std::count_if(groups.begin(), groups.end(), [&](const auto& other) {
            return other.menu == group.menu;
        }) == 1);
        auto parent = group.parent;
        int depth = 0;
        while (parent != ChatFilterMenu::Filters && depth <= 10) {
            auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& other) {
                return other.menu == parent;
            });
            REQUIRE(found != groups.end());
            parent = found->parent;
            ++depth;
        }
        CHECK(depth <= 10);
    }
    for (const auto& item : chat_filter_items()) {
        if (item.menu == ChatFilterMenu::Filters) continue;
        CHECK(std::any_of(groups.begin(), groups.end(), [&](const auto& group) {
            return group.menu == item.menu;
        }));
    }
    CHECK(chat_filter_all_eqstr(ChatFilterMenu::Filters) == 6040);
    CHECK(chat_filter_all_eqstr(ChatFilterMenu::Melee) == 6041);
    CHECK(chat_filter_all_eqstr(ChatFilterMenu::Spells) == 6042);
    CHECK(chat_filter_all_eqstr(ChatFilterMenu::ChatChannel) == 6043);
    CHECK(chat_filter_all_eqstr(ChatFilterMenu::Healing) == 0);
    CHECK(chat_filter_all_eqstr(ChatFilterMenu::DamageOverTime) == 0);
}
